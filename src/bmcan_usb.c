// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BM USB transport layer.
 *
 * Copyright (C) 2026 Busmust Tech Co.,Ltd
 * SPDX-FileCopyrightText: 2026 Busmust Tech Co.,Ltd
 *
 * Ubuntu compatibility notes:
 * - The URB lifecycle and workqueue usage are stable across 20.04/22.04/24.04.
 * - We avoid autosuspend during active use to prevent RX stalls.
 */
#include "bmcan_core.h"
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/mm.h>

/* Bring-up: skip netdev registration */
static bool bmcan_no_netdev = false;
static struct workqueue_struct *bmcan_rx_wq;
module_param_named(no_netdev, bmcan_no_netdev, bool, 0644);
MODULE_PARM_DESC(no_netdev, "[bring-up] Skip netdev registration for debugging");

/* Verbose TX/RX hex dump logging */
bool bmcan_debug_comm = false;
module_param_named(debug_comm, bmcan_debug_comm, bool, 0644);
MODULE_PARM_DESC(debug_comm, "Enable verbose TX/RX debug logging");

/* Last-activity tracker: records what the driver was doing most recently.
 * Visible on serial console if kernel freezes — shows freeze point. */
atomic_t bmcan_last_activity = ATOMIC_INIT(0);

/* Heartbeat counters — used by pr_info HB for crash forensics */
static atomic_t bmcan_diag_ctrl_count = ATOMIC_INIT(0);
static atomic_t bmcan_diag_ctrl_errors = ATOMIC_INIT(0);
static atomic_t bmcan_diag_ctrl_max_ms = ATOMIC_INIT(0);
static atomic_t bmcan_diag_urb_errs = ATOMIC_INIT(0);

#define BMCAN_USB_VID 0x0810

// Gen2 devices (F-prefix, 4th digit = 2)
#define BMCAN_PID_F012 0xF012
#define BMCAN_PID_F112 0xF112
#define BMCAN_PID_F122 0xF122
#define BMCAN_PID_F142 0xF142

// Gen2.5 devices (E-prefix, has NVM storage, logging/replay)
#define BMCAN_PID_E122 0xE122
#define BMCAN_PID_E142 0xE142

// Gen3 devices (4th digit = 3, HPM platform)
#define BMCAN_PID_F013 0xF013
#define BMCAN_PID_F023 0xF023
#define BMCAN_PID_F043 0xF043
#define BMCAN_PID_0043 0x0043
#define BMCAN_PID_0083 0x0083

#define BMCAN_IS_READ_CMD(cmd) ((((cmd) & 0xF0) == 0xD0) || ((cmd) >= 0xF1 && (cmd) <= 0xFA))

/* Map product ID to channel count. */
static int bmcan_channels_from_pid(u16 pid)
{
    switch (pid)
    {
    case BMCAN_PID_0083:
    case BMCAN_PID_F043:
    case BMCAN_PID_E142:
    case BMCAN_PID_F142:
        return 4;
    case BMCAN_PID_0043:
    case BMCAN_PID_F023:
    case BMCAN_PID_E122:
    case BMCAN_PID_F122:
        return 2;
    case BMCAN_PID_F013:
    case BMCAN_PID_F012:
    case BMCAN_PID_F112:
        return 1;
    default:
        return 1;
    }
}

/* Get device generation based on Product ID. */
enum bmcan_generation bmcan_get_generation(u16 pid)
{
    switch (pid)
    {
    /* Gen2 devices (no non-volatile storage)
     * PID encoding: F0xx/F1xx where 3rd digit = channel count, 4th = version (2) */
    case BMCAN_PID_F012:
    case BMCAN_PID_F112:
    case BMCAN_PID_F122:
    case BMCAN_PID_F142:
        return BMCAN_GEN2;

    /* Gen2.5 devices (with non-volatile storage, E-prefix PIDs) */
    case BMCAN_PID_E122:
    case BMCAN_PID_E142:
        return BMCAN_GEN2_5;

    /* Gen3 devices (HPM platform, version digit = 3)
     * F013/F023/F043: F-prefix with version 3
     * 0043/0083: new PID scheme */
    case BMCAN_PID_0043:
    case BMCAN_PID_0083:
    case BMCAN_PID_F013:
    case BMCAN_PID_F023:
    case BMCAN_PID_F043:
        return BMCAN_GEN3;

    default:
        pr_warn("bmcan: unknown PID 0x%04x, assuming Gen2\n", pid);
        return BMCAN_GEN2;
    }
}

/* Debug: count RX URB completions */
static atomic_t bmcan_rx_completion_count = ATOMIC_INIT(0);

/* Forward declaration for three-phase RX pipeline */
static void bmcan_rx_urb_complete(struct urb *urb);

/* RX diagnostics: measure URB payload size and frame rate */
static atomic_t bmcan_diag_urb_count = ATOMIC_INIT(0);
static atomic_t bmcan_diag_frame_count = ATOMIC_INIT(0);
static atomic_t bmcan_diag_total_bytes = ATOMIC_INIT(0);

/* Heartbeat deferred work — runs in workqueue (process) context, safe for pr_warn.
 * Uses pr_warn (not pr_debug) so output is visible on serial console without
 * enabling dynamic debug — essential for hard-lockup forensics on RK3568.
 * Rate: every 3 seconds (fast enough to catch freeze point). */
static void bmcan_hb_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(bmcan_hb_work, bmcan_hb_workfn);

static void bmcan_hb_workfn(struct work_struct *work)
{
    int urbs = atomic_xchg(&bmcan_diag_urb_count, 0);
    int frames = atomic_xchg(&bmcan_diag_frame_count, 0);
    int ctrl = atomic_read(&bmcan_diag_ctrl_count);
    int ctrl_max = atomic_read(&bmcan_diag_ctrl_max_ms);
    int ctrl_err = atomic_read(&bmcan_diag_ctrl_errors);
    int urb_err = atomic_read(&bmcan_diag_urb_errs);

    pr_debug("bmcan: HB urbs=%d frames=%d ctrl=%d(max_ms=%d,err=%d) ue=%d mem=%lu last=%d\n",
            urbs, frames, ctrl, ctrl_max, ctrl_err, urb_err,
            si_mem_available(), atomic_read(&bmcan_last_activity));

    /* Reschedule every 3 seconds for lockup diagnosis */
    schedule_delayed_work(&bmcan_hb_work, 3 * HZ);
}

/*
 * RX drain pipeline — three-phase design for clarity and correctness:
 *
 *   Phase 1 (drain):   Parse all messages from the completed URB buffer.
 *   Phase 2 (deliver): Splice per-port skb lists into NAPI queues.
 *   Phase 3 (resubmit): Return the URB to the USB controller for reuse.
 *
 * Data lifecycle:
 *   USB DMA → coherent buffer → parse → memcpy into skb → NAPI → netstack
 *   The coherent buffer is safe to read until the URB is resubmitted (Phase 3).
 */

/* Phase 1: Drain — parse ALL messages from completed URB buffer into skb queues.
 * Returns number of CAN frames parsed. */
static int bmcan_rx_drain_urb(struct bmcan_dev *dev, int urb_idx,
                               u32 saved_length,
                               struct sk_buff_head *port_rxq)
{
    u8 *buf = dev->rx_urbs_buf + (urb_idx * dev->rx_urb_size);
    int consumed;

    /* Drain: consume ALL messages in this URB buffer */
    consumed = bmcan_proto_parse_rx(dev, buf, saved_length, port_rxq);

    /* Diagnostics: accumulate stats (safe even if consumed == 0) */
    atomic_inc(&bmcan_diag_urb_count);
    atomic_add(saved_length, &bmcan_diag_total_bytes);
    atomic_add(consumed, &bmcan_diag_frame_count);

    /* Kick heartbeat on first URB after driver load — heartbeat self-reschedules
     * every 3s once started. The heartbeat uses pr_warn for serial console visibility. */
    if (atomic_read(&bmcan_diag_urb_count) == 1)
        schedule_delayed_work(&bmcan_hb_work, 0);

    pr_debug(BMCAN_DRIVER_NAME ": rx_urb#%d drained: %u bytes -> %d frames\n",
             urb_idx, saved_length, consumed);

    return consumed;
}

/* Phase 2: Deliver — splice per-port skb queues into NAPI RX queues.
 * Drops frames for down interfaces or when rx_queue_limit is exceeded. */
static void bmcan_rx_deliver(struct bmcan_dev *dev,
                              struct sk_buff_head *port_rxq)
{
    int i;

    /* Drop all frames during USB disconnect — prevents napi_schedule
     * after bmcan_netdev_unregister has freed the netdev. */
    if (!atomic_read(&dev->present))
    {
        for (i = 0; i < 8; i++)
            __skb_queue_purge(&port_rxq[i]);
        return;
    }

    for (i = 0; i < dev->channels; i++)
    {
        struct bmcan_priv *priv;
        unsigned long flags;

        if (skb_queue_empty(&port_rxq[i]) || !dev->ch[i])
            continue;

        priv = dev->ch[i];

        /* Drop frames for interfaces that are administratively down.
         * NAPI is disabled in this state, so frames would accumulate
         * indefinitely without a consumer. */
        if (!netif_running(priv->netdev))
        {
            priv->netdev->stats.rx_dropped += skb_queue_len(&port_rxq[i]);
            __skb_queue_purge(&port_rxq[i]);
            continue;
        }

        /* Enforce rx_queue_limit to bound memory usage when userspace
         * is not consuming frames fast enough. */
        if (READ_ONCE(bmcan_rx_queue_limit) &&
            skb_queue_len(&priv->rxq) >= bmcan_rx_queue_limit)
        {
            priv->netdev->stats.rx_dropped += skb_queue_len(&port_rxq[i]);
            __skb_queue_purge(&port_rxq[i]);
            continue;
        }

        spin_lock_irqsave(&priv->rxq.lock, flags);
        skb_queue_splice_tail(&port_rxq[i], &priv->rxq);
        spin_unlock_irqrestore(&priv->rxq.lock, flags);
        atomic_set(&bmcan_last_activity, ACTIVITY_RX_DELIVER);
        queue_work(bmcan_rx_wq, &priv->rx_deliver_work);
    }
}

/* Phase 3: Resubmit — return URB to USB controller for next reception. */
static void bmcan_rx_resubmit(struct bmcan_dev *dev, struct urb *urb,
                               struct bmcan_rx_urb_ctx *ctx)
{
    int urb_idx = ctx->idx;
    int ret;

    if (READ_ONCE(dev->rx_shutting_down))
        return;

    bmcan_fill_bulk_urb(urb, dev->udev,
                      usb_rcvbulkpipe(dev->udev, dev->ep_in),
                      dev->rx_urbs_buf + (urb_idx * dev->rx_urb_size),
                      dev->rx_urb_size, bmcan_rx_urb_complete, ctx);
    urb->transfer_dma = dev->rx_urbs_dma + (urb_idx * dev->rx_urb_size);
    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    atomic_set(&bmcan_last_activity, ACTIVITY_RX_RESUBMIT);
    ret = usb_submit_urb(urb, GFP_ATOMIC);
    if (ret)
    {
        pr_err_ratelimited(BMCAN_DRIVER_NAME ": rx_urb#%d resubmit failed: %d\n",
                           urb_idx, ret);
        ctx->submitted = false;
        schedule_delayed_work(&dev->rx_resubmit_work, msecs_to_jiffies(100));
    }
    else
    {
        ctx->submitted = true;
        atomic_inc(&dev->rx_active_urbs);
    }
}

/* URB completion callback — orchestrates the three-phase RX pipeline. */
static void bmcan_rx_urb_complete(struct urb *urb)
{
    struct bmcan_rx_urb_ctx *ctx = urb->context;
    struct bmcan_dev *dev = ctx->dev;
    int urb_idx = ctx->idx;
    int saved_status = urb->status;
    u32 saved_length = urb->actual_length;

    if (!dev || !atomic_read(&dev->present))
        return;

    atomic_set(&bmcan_last_activity, ACTIVITY_RX_URB_COMPLETE);
    WRITE_ONCE(dev->rx_last_jiffies, jiffies);
    atomic_inc_return(&bmcan_rx_completion_count);
    atomic_dec(&dev->rx_active_urbs);

    if (bmcan_debug_comm)
        pr_info("bmcan: RX URB#%d complete: status=%d len=%u active=%d\n",
                urb_idx, saved_status, saved_length,
                atomic_read(&dev->rx_active_urbs));

    /* Device physically disconnected -- mark as gone.
     * Note: -ESHUTDOWN comes from driver-initiated usb_kill_urb (kill_rx)
     * and must NOT clear present, otherwise subsequent open/reconfig fails. */
    if (saved_status == -ENODEV)
    {
        atomic_set(&dev->present, 0);
        return;
    }

    /* Driver-initiated kill or shutdown — stop silently */
    if (saved_status == -ESHUTDOWN || READ_ONCE(dev->rx_shutting_down))
        return;

    /* Phase 1+2: Drain and deliver if we have valid data */
    if (saved_status == 0 && saved_length > 0)
    {
        struct sk_buff_head port_rxq[8];
        int i, consumed;

        for (i = 0; i < 8; i++)
            __skb_queue_head_init(&port_rxq[i]);

        consumed = bmcan_rx_drain_urb(dev, urb_idx, saved_length, port_rxq);
        if (bmcan_debug_comm && consumed > 0)
        {
            int q;
            for (q = 0; q < dev->channels; q++)
                if (!skb_queue_empty(&port_rxq[q]))
                    pr_info("bmcan: RX parsed: URB#%d port=%d frames=%d\n",
                            urb_idx, q, skb_queue_len(&port_rxq[q]));
        }
        bmcan_rx_deliver(dev, port_rxq);
    }

    /* Phase 3: Resubmit URB for continuous reception.
     * On success, resubmit immediately. On transient errors (EPROTO,
     * ETIME, EPIPE, etc.), defer to rx_resubmit_work with backoff —
     * immediate resubmit in hard-IRQ context can cause IRQ storm on
     * DWC3/4.x when the host controller is in an error state. */
    if (saved_status == 0)
    {
        bmcan_rx_resubmit(dev, urb, ctx);
    }
    else if (saved_status != -ESHUTDOWN && saved_status != -ENODEV &&
             saved_status != -ENOENT && saved_status != -ECONNRESET &&
             !READ_ONCE(dev->rx_shutting_down))
    {
        ctx->submitted = false;
        schedule_delayed_work(&dev->rx_resubmit_work, msecs_to_jiffies(50));
    }

    /* Log transient errors */
    if (saved_status && saved_status != -ENOENT &&
        saved_status != -ESHUTDOWN && saved_status != -ENODEV &&
        saved_status != -ECONNRESET)
    {
        atomic_inc(&bmcan_diag_urb_errs);
        pr_debug(BMCAN_DRIVER_NAME ": rx_urb#%d status=%d\n", urb_idx, saved_status);
    }
}

static void bmcan_rx_resubmit_workfn(struct work_struct *work)
{
    struct bmcan_dev *dev = container_of(work, struct bmcan_dev, rx_resubmit_work.work);
    int i, retried = 0, failed = 0;

    if (!dev || !atomic_read(&dev->present) || READ_ONCE(dev->rx_shutting_down))
        return;

    /* Multi-URB recovery: resubmit any URBs that failed to resubmit in
     * completion callback. GFP_KERNEL is safe here (process context). */
    for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
    {
        struct bmcan_rx_urb_ctx *ctx = dev->rx_urb_ctx[i];
        if (!ctx || ctx->submitted || !dev->rx_urbs[i])
            continue;

        bmcan_fill_bulk_urb(dev->rx_urbs[i], dev->udev,
                          usb_rcvbulkpipe(dev->udev, dev->ep_in),
                          dev->rx_urbs_buf + (i * dev->rx_urb_size),
                          dev->rx_urb_size, bmcan_rx_urb_complete, ctx);
        dev->rx_urbs[i]->transfer_dma = dev->rx_urbs_dma + (i * dev->rx_urb_size);
        dev->rx_urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

        if (usb_submit_urb(dev->rx_urbs[i], GFP_KERNEL) == 0)
        {
            ctx->submitted = true;
            atomic_inc(&dev->rx_active_urbs);
            retried++;
        }
        else
        {
            pr_warn(BMCAN_DRIVER_NAME ": rx_urb#%d retry failed\n", i);
            failed++;
        }
    }
    if (retried)
        pr_info(BMCAN_DRIVER_NAME ": rx recovery: resubmitted %d URBs\n", retried);
    /* If any URBs still failed, schedule another retry with backoff.
     * Prevents permanent RX death from transient USB errors. */
    if (failed > 0)
        schedule_delayed_work(&dev->rx_resubmit_work, msecs_to_jiffies(500));
}

/* Send a vendor USB control message (read or write). */
int bmcan_usb_send_control(struct bmcan_dev *dev, u8 request, u16 value, u16 index, void *data, u16 size)
{
    unsigned int pipe;
    u8 req_type = USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    int ret;
    bool is_read = BMCAN_IS_READ_CMD(request);
    void *buf = NULL;

    if (!dev || !dev->udev || !atomic_read(&dev->present))
        return -ENODEV;

    pr_debug("bmcan: [USB_CTRL] req=0x%02x value=0x%04x index=0x%04x size=%u is_read=%d\n",
             request, value, index, size, is_read);

    if (size)
    {
        buf = kmalloc(size, GFP_KERNEL);
        if (!buf)
            return -ENOMEM;
        if (!is_read && data)
        {
            memcpy(buf, data, size);
            if (request == BM_CAN_CTRL_WR(BM_CAN_BITRATE) && size == sizeof(struct bm_bitrate))
            {
                struct bm_bitrate *br = (struct bm_bitrate *)buf;
                pr_debug("bmcan: [USB_CTRL_TX_BITRATE] value=%d(index=%d)\n", value, index);
                pr_debug("bmcan:   nbitrate=%u dbitrate=%u\n", br->nbitrate, br->dbitrate);
                pr_debug("bmcan:   nsamplepos=%u dsamplepos=%u\n", br->nsamplepos, br->dsamplepos);
                pr_debug("bmcan:   nbtr0=%u nbtr1=%u\n", br->nbtr0, br->nbtr1);
                pr_debug("bmcan:   dbtr0=%u dbtr1=%u\n", br->dbtr0, br->dbtr1);
            }
        }
    }

    if (is_read)
    {
        pipe = usb_rcvctrlpipe(dev->udev, 0);
        req_type |= USB_DIR_IN;
        if (buf && size)
            memset(buf, 0, size);
    }
    else
    {
        pipe = usb_sndctrlpipe(dev->udev, 0);
    }

    /* Track in-flight transfers for disconnect drain (no serialization —
     * USB core already serializes on the control endpoint). */
    {
        unsigned long t0 = jiffies;

        atomic_inc(&dev->ctrl_inflight);
        /* Re-check after incrementing: disconnect may have started. */
        if (!dev->udev || !atomic_read(&dev->present))
        {
            atomic_dec(&dev->ctrl_inflight);
            kfree(buf);
            return -ENODEV;
        }

        atomic_set(&bmcan_last_activity, ACTIVITY_CTRL_MSG);
        ret = usb_control_msg(dev->udev, pipe, request, req_type, value, index,
            buf, size, 2000);
        atomic_dec(&dev->ctrl_inflight);

        /* Post-transfer diagnostics (outside lock, no timing impact on critical section) */
        {
            int elapsed_ms = jiffies_to_msecs(jiffies - t0);
            int old_max;
            atomic_inc(&bmcan_diag_ctrl_count);
            if (ret < 0)
                atomic_inc(&bmcan_diag_ctrl_errors);
            old_max = atomic_read(&bmcan_diag_ctrl_max_ms);
            while (elapsed_ms > old_max)
            {
                if (atomic_cmpxchg(&bmcan_diag_ctrl_max_ms, old_max, elapsed_ms) == old_max)
                    break;
                old_max = atomic_read(&bmcan_diag_ctrl_max_ms);
            }
            if (elapsed_ms > 50)
                pr_debug("bmcan: CTRL req=0x%02x elapsed=%dms last=%d\n",
                         request, elapsed_ms, atomic_read(&bmcan_last_activity));
        }
    }

    if (ret < 0)
    {
        pr_err("bmcan: [USB_CTRL_ERROR] req=0x%02x ret=%d\n", request, ret);
        pr_err(BMCAN_DRIVER_NAME ": usb control failed req=0x%x err=%d\n", request, ret);
        kfree(buf);
        if (ret == -ENODEV)
            atomic_set(&dev->present, 0);
        return ret;
    }

    /* Only copy data back on successful read operations.
     * Warn on short reads — caller expects exactly 'size' bytes. */
    if (is_read && data && buf && ret > 0)
    {
        size_t copy_len = min((size_t)size, (size_t)ret);
        if ((size_t)ret < (size_t)size)
            pr_debug("bmcan: short read: req=0x%02x expected=%u got=%d\n",
                     request, size, ret);
        memcpy(data, buf, copy_len);
    }

    kfree(buf);
    pr_debug("bmcan: [USB_CTRL] req=0x%02x ret=%d\n", request, ret);

    return ret;
}

/* Submit RX URB(s) to start receiving frames. */
int bmcan_usb_submit_rx(struct bmcan_dev *dev)
{
    int i;
    int ret;
    int submitted = 0;

    if (!dev || !atomic_read(&dev->present))
        return -EINVAL;

    /* Multi-URB mode: submit all RX URBs */
    if (dev->rx_urbs[0] && dev->rx_urbs_buf)
    {
        pr_debug(BMCAN_DRIVER_NAME ": submit_rx: submitting %d URBs, %zu bytes each\n",
                BMCAN_NUM_RX_URBS, dev->rx_urb_size);

        /* Kill existing URBs if re-submitting (resubmission case).
         * With rx_shutting_down flag, completion callbacks won't resubmit,
         * so one kill round is sufficient. */
        if (atomic_read(&dev->rx_active_urbs) > 0)
        {
            cancel_delayed_work_sync(&dev->rx_resubmit_work);
            WRITE_ONCE(dev->rx_shutting_down, true);
            for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
            {
                if (dev->rx_urbs[i])
                    usb_kill_urb(dev->rx_urbs[i]);
            }
            WRITE_ONCE(dev->rx_shutting_down, false);
        }

        /* Reset active counter before submitting. */
        atomic_set(&dev->rx_active_urbs, 0);

        for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
        {
            if (!dev->rx_urbs[i])
                continue;

            bmcan_fill_bulk_urb(dev->rx_urbs[i], dev->udev,
                              usb_rcvbulkpipe(dev->udev, dev->ep_in),
                              dev->rx_urbs_buf + (i * dev->rx_urb_size),
                              dev->rx_urb_size, bmcan_rx_urb_complete,
                              dev->rx_urb_ctx[i]);
            dev->rx_urbs[i]->transfer_dma = dev->rx_urbs_dma + (i * dev->rx_urb_size);
            dev->rx_urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

            ret = usb_submit_urb(dev->rx_urbs[i], GFP_KERNEL);
            if (ret)
            {
                pr_err(BMCAN_DRIVER_NAME ": submit_rx: URB#%d failed: %d (urb->status=%d)\n",
                       i, ret, dev->rx_urbs[i]->status);
                dev->rx_urb_ctx[i]->submitted = false;
            }
            else
            {
                atomic_inc(&dev->rx_active_urbs);
                dev->rx_urb_ctx[i]->submitted = true;
                submitted++;
            }
        }

        pr_debug(BMCAN_DRIVER_NAME ": submit_rx: %d/%d URBs submitted\n",
                submitted, BMCAN_NUM_RX_URBS);
        return submitted > 0 ? 0 : -EIO;
    }

    return -EINVAL;
}

/* Stop RX stream safely. */
void bmcan_usb_kill_rx(struct bmcan_dev *dev)
{
    int i;

    if (!dev)
        return;

    /* Signal completion callbacks to stop resubmitting, then kill all URBs. */
    WRITE_ONCE(dev->rx_shutting_down, true);

    for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
    {
        if (dev->rx_urbs[i])
            usb_kill_urb(dev->rx_urbs[i]);
        if (dev->rx_urb_ctx[i])
            dev->rx_urb_ctx[i]->submitted = false;
    }

    /* Cancel resubmit work AFTER killing URBs: a URB completion during the
     * kill loop may have scheduled resubmit work. Cancel after to catch it. */
    cancel_delayed_work_sync(&dev->rx_resubmit_work);

    WRITE_ONCE(dev->rx_shutting_down, false);
}

/* USB probe: allocate device, buffers, and register netdevs. */
static int bmcan_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct bmcan_dev *dev;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *ep;
    int i;
    int channels;
    int err;

    /* Composite USB devices (e.g. E142) expose multiple interfaces.
     * Only bind to interface 0 which carries the CAN function. */
    if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
    {
        dev_info(&intf->dev, "skipping interface %d (not CAN function)\n",
                 intf->cur_altsetting->desc.bInterfaceNumber);
        return -ENODEV;
    }

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->udev = usb_get_dev(udev);
    dev->intf = intf;
    dev->ep_in = BMCAN_USB_EP_IN;
    dev->ep_out = BMCAN_USB_EP_OUT;
    dev->ep_cmd = BMCAN_USB_EP_CMD;
    spin_lock_init(&dev->state_lock);
    spin_lock_init(&dev->ts_lock);
    dev->ts_last_us = 0;
    dev->ts_wrap_count = 0;
    dev->ts_initialized = false;
    mutex_init(&dev->open_lock);
    atomic_set(&dev->present, 1);
    atomic_set(&dev->open_count, 0);

    /* Detect device generation from Product ID */
    dev->generation = bmcan_get_generation(le16_to_cpu(udev->descriptor.idProduct));
    {
        const char *gen_str = (dev->generation == BMCAN_GEN2) ? "Gen2" :
                              (dev->generation == BMCAN_GEN2_5) ? "Gen2.5" :
                              (dev->generation == BMCAN_GEN3) ? "Gen3" : "Unknown";
        pr_info("bmcan: detected device %s (PID=0x%04x)\n",
                gen_str, le16_to_cpu(udev->descriptor.idProduct));
    }

    /* Set TX limits based on device generation (matching BMAPI SDK).
     * deviceVersion = (pid & 0x000F), deviceChannelCount = (pid & 0x00F0) >> 4.
     * Gen3 (hpm32): buf=24000, max_packet=12000.
     * Gen2 multi-ch: buf=20480, max_packet=512.
     * Gen2 1-ch: buf=2048, max_packet=256. */
    {
        u16 pid = le16_to_cpu(udev->descriptor.idProduct);
        u32 dev_ver = pid & 0x000FU;
        int ch_count = bmcan_channels_from_pid(pid);

        if (dev_ver >= 3)
        {
            dev->tx_limits.device_buf_size = 24000;
            dev->tx_limits.max_packet_size = 12000;
        }
        else if (ch_count == 1)
        {
            dev->tx_limits.device_buf_size = 2048;
            dev->tx_limits.max_packet_size = 256;
        }
        else if (ch_count >= 8)
        {
            dev->tx_limits.device_buf_size = 4096;
            dev->tx_limits.max_packet_size = 512;
        }
        else
        {
            dev->tx_limits.device_buf_size = 20480;
            dev->tx_limits.max_packet_size = 512;
        }
        pr_info(BMCAN_DRIVER_NAME ": TX limits: device_buf=%u max_packet=%u\n",
                dev->tx_limits.device_buf_size, dev->tx_limits.max_packet_size);
    }

    /* Read firmware version via BM_GET_DEVICE_VERSION (0xF1).
     * Returns 4 bytes: major, minor, revision, build. */
    {
        u8 ver[4];
        int verr = bmcan_usb_send_control(dev, BM_GET_DEVICE_VERSION,
                                           0, 0, ver, sizeof(ver));
        if (verr >= 0)
        {
            dev->fw_version = ((u32)ver[0] << 24) | ((u32)ver[1] << 16) |
                              ((u32)ver[2] << 8) | (u32)ver[3];
            dev_info(&intf->dev, "BMCAN firmware v%u.%u.%u.%u\n",
                     ver[0], ver[1], ver[2], ver[3]);
        }
        else
        {
            dev->fw_version = 0;
            dev_warn(&intf->dev, "Failed to read firmware version (%d)\n", verr);
        }
    }

    /* Prevent autosuspend from breaking the device link. */
    usb_disable_autosuspend(dev->udev);

    iface_desc = intf->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; i++)
    {
        ep = &iface_desc->endpoint[i].desc;
        if (usb_endpoint_is_bulk_in(ep))
            dev->ep_in = ep->bEndpointAddress;
        else if (usb_endpoint_is_bulk_out(ep))
            dev->ep_out = ep->bEndpointAddress;
    }

    pr_info(BMCAN_DRIVER_NAME ": ep_in=0x%02x ep_out=0x%02x\n", dev->ep_in, dev->ep_out);

    /* Allocate Multi-URB RX resources */
    dev->rx_urb_size = BMCAN_RX_URB_SIZE;
    dev->rx_urbs_buf = usb_alloc_coherent(dev->udev,
                                          BMCAN_NUM_RX_URBS * dev->rx_urb_size,
                                          GFP_KERNEL, &dev->rx_urbs_dma);
    if (!dev->rx_urbs_buf)
    {
        pr_err(BMCAN_DRIVER_NAME ": multi-URB buffer alloc failed\n");
        err = -ENOMEM;
        goto err_free;
    }

    /* Allocate URBs and contexts */
    for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
    {
        dev->rx_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        if (!dev->rx_urbs[i])
        {
            pr_warn(BMCAN_DRIVER_NAME ": URB#%d alloc failed\n", i);
            continue;
        }
        dev->rx_urb_ctx[i] = kmalloc(sizeof(struct bmcan_rx_urb_ctx),
                                      GFP_KERNEL);
        if (!dev->rx_urb_ctx[i])
        {
            pr_warn(BMCAN_DRIVER_NAME ": URB ctx#%d alloc failed\n", i);
            usb_free_urb(dev->rx_urbs[i]);
            dev->rx_urbs[i] = NULL;
            continue;
        }
        dev->rx_urb_ctx[i]->dev = dev;
        dev->rx_urb_ctx[i]->idx = i;
        dev->rx_urb_ctx[i]->submitted = false;
    }
    atomic_set(&dev->rx_active_urbs, 0);
    pr_debug(BMCAN_DRIVER_NAME ": multi-URB RX: %d URBs x %zu bytes = %zu KB total\n",
            BMCAN_NUM_RX_URBS, dev->rx_urb_size,
            (BMCAN_NUM_RX_URBS * dev->rx_urb_size) / 1024);

    INIT_DELAYED_WORK(&dev->rx_resubmit_work, bmcan_rx_resubmit_workfn);
    WRITE_ONCE(dev->rx_last_jiffies, jiffies);

    channels = bmcan_channels_from_pid(le16_to_cpu(udev->descriptor.idProduct));
    if (!bmcan_no_netdev)
    {
        for (i = 0; i < channels; i++)
        {
            err = bmcan_netdev_register(dev, (u8)i);
            if (err)
            {
                pr_err(BMCAN_DRIVER_NAME ": register netdev %d failed (%d)\n", i, err);
                goto err_netdev;
            }
        }
    }
    else
    {
        pr_warn(BMCAN_DRIVER_NAME ": netdev registration disabled\n");
        channels = 0;
    }

    usb_set_intfdata(intf, dev);
    dev->channels = channels;

    /* Start RX URBs at probe time — keep them alive across open/close cycles.
     * This matches BMAPI SDK behavior where the USB read thread never stops,
     * preventing firmware state corruption from kill/resubmit cycles. */
    if (!bmcan_no_netdev && bmcan_usb_submit_rx(dev) < 0)
        pr_warn(BMCAN_DRIVER_NAME ": initial RX submit failed (will retry on open)\n");

    pr_info(BMCAN_DRIVER_NAME ": probed %d channel(s)\n", channels);
    return 0;

err_netdev:
    for (i = 0; i < 8; i++)
        bmcan_netdev_unregister(dev, (u8)i);
    bmcan_usb_kill_rx(dev);
    for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
    {
        if (dev->rx_urbs[i])
            usb_free_urb(dev->rx_urbs[i]);
        kfree(dev->rx_urb_ctx[i]);
    }
    if (dev->rx_urbs_buf)
        usb_free_coherent(dev->udev,
                          BMCAN_NUM_RX_URBS * dev->rx_urb_size,
                          dev->rx_urbs_buf, dev->rx_urbs_dma);
err_free:
    usb_put_dev(dev->udev);
    kfree(dev);
    return err;
}

/* USB disconnect: tear down netdevs and buffers. */
static void bmcan_disconnect(struct usb_interface *intf)
{
    struct bmcan_dev *dev = usb_get_intfdata(intf);
    int i;

    usb_set_intfdata(intf, NULL);
    if (!dev)
        return;

    atomic_set(&dev->present, 0);

    cancel_delayed_work_sync(&bmcan_hb_work);
    usb_enable_autosuspend(dev->udev);

    bmcan_usb_kill_rx(dev);
    for (i = 0; i < 8; i++)
        bmcan_netdev_unregister(dev, (u8)i);

    /* Free multi-URB RX resources */
    for (i = 0; i < BMCAN_NUM_RX_URBS; i++)
    {
        if (dev->rx_urbs[i])
            usb_free_urb(dev->rx_urbs[i]);
        kfree(dev->rx_urb_ctx[i]);
    }
    if (dev->rx_urbs_buf)
        usb_free_coherent(dev->udev,
                          BMCAN_NUM_RX_URBS * dev->rx_urb_size,
                          dev->rx_urbs_buf, dev->rx_urbs_dma);

    /* Drain in-flight control transfers: netdev_unregister removed sysfs
     * files (waits for in-progress store callbacks) and cancelled works,
     * but a transfer that passed the initial present check may still be
     * inside usb_control_msg. Wait for ctrl_inflight to reach 0. */
    {
        int timeout = 300; /* 3 seconds (usb_control_msg timeout is 2s) */
        while (atomic_read(&dev->ctrl_inflight) > 0 && --timeout > 0)
            msleep(10);
        if (timeout <= 0)
            pr_warn(BMCAN_DRIVER_NAME ": ctrl_inflight drain timed out\n");
    }

    usb_put_dev(dev->udev);
    kfree(dev);
}

static const struct usb_device_id bmcan_usb_table[] = {
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_0043) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_0083) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_E122) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_E142) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F012) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F013) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F023) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F043) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F112) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F122) },
    { USB_DEVICE(BMCAN_USB_VID, BMCAN_PID_F142) },
    { }
};
MODULE_DEVICE_TABLE(usb, bmcan_usb_table);

static struct usb_driver bmcan_usb_driver = {
    .name = BMCAN_DRIVER_NAME,
    .probe = bmcan_probe,
    .disconnect = bmcan_disconnect,
    .id_table = bmcan_usb_table,
};

/* Module init/exit. */
static int __init bmcan_init(void)
{
    int ret;

    /* Ensure CAN core modules are present to satisfy exported symbols. */
    request_module("can");
    request_module("can_dev");
    request_module("can_raw");
    bmcan_rx_wq = alloc_workqueue("bmcan_rx", WQ_UNBOUND | WQ_HIGHPRI, 0);
    if (!bmcan_rx_wq)
        return -ENOMEM;

    ret = usb_register(&bmcan_usb_driver);
    if (ret)
    {
        destroy_workqueue(bmcan_rx_wq);
        bmcan_rx_wq = NULL;
    }
    return ret;
}

static void __exit bmcan_exit(void)
{
    usb_deregister(&bmcan_usb_driver);
    cancel_delayed_work_sync(&bmcan_hb_work);
    if (bmcan_rx_wq) {
        destroy_workqueue(bmcan_rx_wq);
        bmcan_rx_wq = NULL;
    }
}

module_init(bmcan_init);
module_exit(bmcan_exit);

MODULE_SOFTDEP("pre: can can_dev can_raw");
MODULE_AUTHOR("BUSMASTER BMAPI Project");
MODULE_DESCRIPTION("BUSMUST USB-CAN FD SocketCAN driver");
MODULE_LICENSE("GPL");
