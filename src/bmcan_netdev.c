// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BM USB-CAN SocketCAN netdev glue.
 *
 * Copyright (C) 2026 Busmust Tech Co.,Ltd
 * SPDX-FileCopyrightText: 2026 Busmust Tech Co.,Ltd
 *
 * Ubuntu compatibility notes:
 * - 20.04 (kernel 5.4) up to 24.04 (kernel 6.8): echo skb APIs differ.
 *   Use wrapper helpers below to pass the extra length argument when required
 *   (>= 6.0) while keeping older kernels building and running.
 */
#include "bmcan_core.h"
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/can/skb.h>
#include <linux/netdevice.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/bitmap.h>

/*
 * Echo skb helpers: BMCAN_ECHO_HAS_LEN is defined in bmcan_core.h.
 * 1 = kernel has 4-arg can_put_echo_skb / 3-arg can_get/free_echo_skb (5.12+)
 * 0 = older 3-arg / 2-arg forms (4.19 – 5.11)
 */

#if BMCAN_ECHO_HAS_LEN
static inline void bmcan_put_echo_skb(struct sk_buff *skb, struct net_device *dev,
                                      unsigned int idx, unsigned int len)
{
    can_put_echo_skb(skb, dev, idx, len);
}

static inline void bmcan_get_echo_skb(struct net_device *dev, unsigned int idx,
                                      unsigned int len)
{
    int __attribute__((unused)) rc = can_get_echo_skb(dev, idx, &len);
}

static inline void bmcan_free_echo_skb(struct net_device *dev, unsigned int idx,
                                       unsigned int len)
{
    can_free_echo_skb(dev, idx, &len);
}
#else
static inline void bmcan_put_echo_skb(struct sk_buff *skb, struct net_device *dev,
                                      unsigned int idx, unsigned int len)
{
    (void)len;
    can_put_echo_skb(skb, dev, idx);
}

static inline void bmcan_get_echo_skb(struct net_device *dev, unsigned int idx,
                                      unsigned int len)
{
    (void)len;
    can_get_echo_skb(dev, idx);
}

static inline void bmcan_free_echo_skb(struct net_device *dev, unsigned int idx,
                                       unsigned int len)
{
    (void)len;
    can_free_echo_skb(dev, idx);
}
#endif

struct bmcan_tx_ctx
{
    struct bmcan_priv *priv;
    struct urb *urb;
    u8 *buf;
    size_t len;
    unsigned int echo_len;
};
/* NOTE: bmcan_tx_ctx kept for reference; TX now uses pre-allocated bmcan_tx_urb pool */

/* Dedicated TX flush workqueue to avoid system_wq congestion under sustained
 * high throughput. system_wq is shared with many kernel subsystems; a backlog
 * there delays batch flushing, which prevents URB completion → tx_busy never
 * decrements → netif_stop_queue never wakes → TX path deadlock. */
static struct workqueue_struct *bmcan_tx_wq;
static atomic_t bmcan_tx_wq_ref = ATOMIC_INIT(0);
static DEFINE_MUTEX(bmcan_tx_wq_mutex);

/* Dedicated workqueue for status polling.  Must NOT run on system_wq
 * (synchronous USB control transfer blocks system worker threads,
 *  freezing SSH/disk when 8 channels poll concurrently) and must NOT
 *  run on bmcan_tx_wq (HIGHPRI steals USB bandwidth from TX, causing
 *  CAN bus-off under load).  Plain WQ_UNBOUND keeps it isolated. */
static struct workqueue_struct *bmcan_status_wq;
static atomic_t bmcan_status_wq_ref = ATOMIC_INIT(0);
static DEFINE_MUTEX(bmcan_status_wq_mutex);

/* Forward declaration (called before definition in open_workfn). */
static int bmcan_set_operating_mode(struct bmcan_priv *priv);

/* TX stall watchdog interval (seconds) */
#define BMCAN_TX_STALL_CHECK_SEC 2
/* Maximum time queue may stay stopped without progress before force-wake (seconds) */
#define BMCAN_TX_STALL_TIMEOUT_SEC 5
/* Retry interval after bounded TX URB unlink did not drain all completions */
#define BMCAN_TX_RECOVER_RETRY_MS 500

static struct bmcan_priv *bmcan_priv_from_netdev(const struct net_device *dev);

static void bmcan_stop_tx_queue(struct net_device *netdev)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    if (!netif_queue_stopped(netdev))
        priv->last_queue_stop_jiffies = jiffies;
    netif_stop_queue(netdev);
}

static void bmcan_tx_wq_get(void)
{
    mutex_lock(&bmcan_tx_wq_mutex);
    if (atomic_inc_return(&bmcan_tx_wq_ref) == 1)
    {
        bmcan_tx_wq = alloc_workqueue("bmcan_tx",
                                       WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM,
                                       0);
        if (!bmcan_tx_wq)
            bmcan_tx_wq = system_wq;
    }
    mutex_unlock(&bmcan_tx_wq_mutex);
}

static void bmcan_tx_wq_put(void)
{
    mutex_lock(&bmcan_tx_wq_mutex);
    if (atomic_dec_and_test(&bmcan_tx_wq_ref) && bmcan_tx_wq != system_wq)
    {
        destroy_workqueue(bmcan_tx_wq);
        bmcan_tx_wq = NULL;
    }
    mutex_unlock(&bmcan_tx_wq_mutex);
}

static void bmcan_status_wq_get(void)
{
    mutex_lock(&bmcan_status_wq_mutex);
    if (atomic_inc_return(&bmcan_status_wq_ref) == 1)
    {
        bmcan_status_wq = alloc_workqueue("bmcan_sts", WQ_UNBOUND, 0);
        if (!bmcan_status_wq)
            bmcan_status_wq = system_wq;
    }
    mutex_unlock(&bmcan_status_wq_mutex);
}

static void bmcan_status_wq_put(void)
{
    mutex_lock(&bmcan_status_wq_mutex);
    if (atomic_dec_and_test(&bmcan_status_wq_ref) && bmcan_status_wq != system_wq)
    {
        destroy_workqueue(bmcan_status_wq);
        bmcan_status_wq = NULL;
    }
    mutex_unlock(&bmcan_status_wq_mutex);
}

static bool bmcan_disable_rx = false;
module_param_named(disable_rx, bmcan_disable_rx, bool, 0644);
MODULE_PARM_DESC(disable_rx, "[bring-up] Disable USB RX submission for debugging");

static bool bmcan_rx_drop = false;
module_param_named(rx_drop, bmcan_rx_drop, bool, 0644);
MODULE_PARM_DESC(rx_drop, "[bring-up] Drop RX frames after parsing (debug isolation)");

unsigned int bmcan_rx_queue_limit = 4096;
module_param_named(rx_queue_limit, bmcan_rx_queue_limit, uint, 0644);
MODULE_PARM_DESC(rx_queue_limit, "Max queued RX skb before dropping (0=unlimited)");

static unsigned int bmcan_rx_napi_weight = 256;
module_param_named(rx_napi_weight, bmcan_rx_napi_weight, uint, 0644);
MODULE_PARM_DESC(rx_napi_weight, "NAPI poll weight for RX delivery");

static unsigned int bmcan_rx_rate_limit = 0;
module_param_named(rx_rate_limit, bmcan_rx_rate_limit, uint, 0644);
MODULE_PARM_DESC(rx_rate_limit, "Max RX frames per second (0=disabled)");

static unsigned int bmcan_rx_rate_burst = 1024;
module_param_named(rx_rate_burst, bmcan_rx_rate_burst, uint, 0644);
MODULE_PARM_DESC(rx_rate_burst, "RX rate-limit burst size");

static bool bmcan_disable_ctrl = false;
module_param_named(disable_ctrl, bmcan_disable_ctrl, bool, 0644);
MODULE_PARM_DESC(disable_ctrl, "[bring-up] Skip USB control (bitrate/mode) in open for debugging");

static bool bmcan_fail_open = false;
module_param_named(fail_open, bmcan_fail_open, bool, 0644);
MODULE_PARM_DESC(fail_open, "[bring-up] Force ndo_open to fail for testing");

static bool bmcan_disable_echo = false;
module_param_named(disable_echo, bmcan_disable_echo, bool, 0644);
MODULE_PARM_DESC(disable_echo, "[bring-up] Disable SocketCAN echo handling (benchmark only)");

static unsigned int bmcan_defer_open_ms = 0;
module_param_named(defer_open_ms, bmcan_defer_open_ms, uint, 0644);
MODULE_PARM_DESC(defer_open_ms, "[bring-up] Defer HW config after ndo_open (ms, 0=synchronous default)");

static unsigned int bmcan_status_poll_ms = 1000;
module_param_named(status_poll_ms, bmcan_status_poll_ms, uint, 0644);
MODULE_PARM_DESC(status_poll_ms, "Poll CAN status and report errors (ms, 0=disable)");

static bool bmcan_auto_clear_config = false;
module_param_named(auto_clear_config, bmcan_auto_clear_config, bool, 0644);
MODULE_PARM_DESC(auto_clear_config, "Automatically clear txtask/route config on device open (default=false)");

static bool bmcan_rxfilter_enable = false;
module_param_named(rxfilter_enable, bmcan_rxfilter_enable, bool, 0644);
MODULE_PARM_DESC(rxfilter_enable, "Enable a single hardware RX filter entry");

static unsigned int bmcan_rxfilter_id = 0;
module_param_named(rxfilter_id, bmcan_rxfilter_id, uint, 0644);
MODULE_PARM_DESC(rxfilter_id, "RX filter ID value (mask applied)");

static unsigned int bmcan_rxfilter_mask = 0;
module_param_named(rxfilter_mask, bmcan_rxfilter_mask, uint, 0644);
MODULE_PARM_DESC(rxfilter_mask, "RX filter ID mask (0=match all)");

static bool bmcan_rxfilter_ext = false;
module_param_named(rxfilter_ext, bmcan_rxfilter_ext, bool, 0644);
MODULE_PARM_DESC(rxfilter_ext, "RX filter for extended IDs");

static bool bmcan_rxfilter_rtr = false;
module_param_named(rxfilter_rtr, bmcan_rxfilter_rtr, bool, 0644);
MODULE_PARM_DESC(rxfilter_rtr, "RX filter for RTR frames");

static bool bmcan_tx_recovery_ready_locked(struct bmcan_priv *priv)
{
    int i;

    if (atomic_read(&priv->tx_active_urbs) != 0)
        return false;

    /* tx_complete decrements tx_active_urbs before returning the URB to the
     * pool under tx_pool_lock.  Require in_use=false as well so reset never
     * races with the tail of a completion callback. */
    for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
    {
        if (priv->tx_urbs[i].in_use)
            return false;
    }
    return true;
}

static void bmcan_discard_tx_batch_locked(struct bmcan_priv *priv)
{
    struct net_device *netdev = priv->netdev;
    struct bmcan_tx_urb *batch = priv->tx_batch;
    int i;

    if (!batch)
        return;

    for (i = 0; i < batch->echo_count; i++)
    {
        if (!bmcan_disable_echo)
            bmcan_free_echo_skb(netdev, batch->echo_slots[i],
                                batch->echo_lens[i]);
        set_bit(batch->echo_slots[i], priv->echo_free_bm);
    }
    atomic_sub(batch->echo_count, &priv->tx_busy);
    batch->in_use = false;
    batch->priv = NULL;
    batch->echo_count = 0;
    set_bit(batch->idx, priv->tx_free_bm);
    priv->tx_batch = NULL;
}

static bool bmcan_finish_tx_recovery(struct bmcan_priv *priv, const char *reason)
{
    struct net_device *netdev = priv->netdev;
    unsigned long flags;
    unsigned long *to_free;
    int i;

    to_free = bitmap_zalloc(BMCAN_TX_ECHO_MAX, GFP_KERNEL);

    spin_lock_irqsave(&priv->tx_pool_lock, flags);
    if (!bmcan_tx_recovery_ready_locked(priv))
    {
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
        if (to_free)
            bitmap_free(to_free);
        return false;
    }

    if (priv->tx_batch)
        bmcan_discard_tx_batch_locked(priv);

    if (to_free)
    {
        for (i = 0; i < BMCAN_TX_ECHO_MAX; i++)
        {
            if (!test_bit(i, priv->echo_free_bm))
            {
                __set_bit(i, to_free);
                __set_bit(i, priv->echo_free_bm);
            }
        }
    }
    else
    {
        for (i = 0; i < BMCAN_TX_ECHO_MAX; i++)
        {
            if (!test_bit(i, priv->echo_free_bm))
                bmcan_free_echo_skb(netdev, i, 0);
        }
        bitmap_fill(priv->echo_free_bm, BMCAN_TX_ECHO_MAX);
    }

    atomic_set(&priv->tx_busy, 0);
    atomic_set(&priv->tx_ongoing_bytes, 0);
    atomic_set(&priv->tx_active_urbs, 0);
    bitmap_fill(priv->tx_free_bm, BMCAN_NUM_TX_URBS);
    for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
    {
        priv->tx_urbs[i].in_use = false;
        priv->tx_urbs[i].priv = NULL;
        priv->tx_urbs[i].echo_count = 0;
    }
    WRITE_ONCE(priv->tx_recovering, false);
    spin_unlock_irqrestore(&priv->tx_pool_lock, flags);

    if (to_free)
    {
        for (i = 0; i < BMCAN_TX_ECHO_MAX; i++)
        {
            if (test_bit(i, to_free))
                bmcan_free_echo_skb(netdev, i, 0);
        }
        bitmap_free(to_free);
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s TX recovery complete (%s)\n",
             netdev->name, reason);
    return true;
}

static void bmcan_unlink_active_tx_urbs(struct bmcan_priv *priv)
{
    int i;

    for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
    {
        if (priv->tx_urbs[i].urb && priv->tx_urbs[i].in_use)
            usb_unlink_urb(priv->tx_urbs[i].urb);
    }
}

static void bmcan_tx_recover_workfn(struct work_struct *work);

static void bmcan_start_tx_recovery(struct bmcan_priv *priv, const char *reason)
{
    struct net_device *netdev = priv->netdev;
    struct bmcan_dev *dev = priv->parent;
    unsigned long flags;
    int wl = 50;

    if (!netif_running(netdev) || !atomic_read(&dev->present))
        return;

    bmcan_stop_tx_queue(netdev);
    WRITE_ONCE(priv->tx_recovering, true);

    cancel_delayed_work_sync(&priv->tx_flush_work);

    spin_lock_irqsave(&priv->tx_pool_lock, flags);
    bmcan_discard_tx_batch_locked(priv);
    spin_unlock_irqrestore(&priv->tx_pool_lock, flags);

    bmcan_unlink_active_tx_urbs(priv);

    while (atomic_read(&priv->tx_active_urbs) > 0 && --wl > 0)
        msleep(10);

    if (bmcan_finish_tx_recovery(priv, reason))
    {
        if (!READ_ONCE(priv->stopping) && netif_running(netdev) &&
            atomic_read(&dev->present))
            netif_wake_queue(netdev);
        return;
    }

    pr_warn_ratelimited(BMCAN_DRIVER_NAME
                        ": %s TX recovery pending after 500ms (%s, active=%d)\n",
                        netdev->name, reason,
                        atomic_read(&priv->tx_active_urbs));
    queue_delayed_work(bmcan_status_wq, &priv->tx_recover_work,
                       msecs_to_jiffies(BMCAN_TX_RECOVER_RETRY_MS));
}

static void bmcan_tx_recover_workfn(struct work_struct *work)
{
    struct bmcan_priv *priv = container_of(to_delayed_work(work),
                                           struct bmcan_priv,
                                           tx_recover_work);
    struct net_device *netdev = priv->netdev;
    struct bmcan_dev *dev = priv->parent;

    if (!netif_running(netdev) || !atomic_read(&dev->present) ||
        READ_ONCE(priv->stopping))
        return;

    if (bmcan_finish_tx_recovery(priv, "retry"))
    {
        if (netif_running(netdev) && atomic_read(&dev->present))
            netif_wake_queue(netdev);
        return;
    }

    pr_warn_ratelimited(BMCAN_DRIVER_NAME
                        ": %s TX recovery still waiting for URB completion (active=%d)\n",
                        netdev->name, atomic_read(&priv->tx_active_urbs));
    queue_delayed_work(bmcan_status_wq, &priv->tx_recover_work,
                       msecs_to_jiffies(BMCAN_TX_RECOVER_RETRY_MS));
}

static bool bmcan_force_fd = false;
module_param_named(force_fd, bmcan_force_fd, bool, 0644);
MODULE_PARM_DESC(force_fd, "Force CAN FD capable mode regardless of netlink");

static int bmcan_term_resistor[8] = { [0 ... 7] = -1 };
module_param_array_named(term_resistor, bmcan_term_resistor, int, NULL, 0644);
MODULE_PARM_DESC(term_resistor, "Terminal resistor per channel (0=off,60,120,0xFFFF=auto,-1=skip)");

static u32 bmcan_get_can_mode(const struct bmcan_priv *priv);

static void bmcan_open_workfn(struct work_struct *work)
{
    struct bmcan_priv *priv = container_of(to_delayed_work(work), struct bmcan_priv, open_work);
    struct bmcan_dev *dev = priv->parent;
    u32 bitrate = priv->can.bittiming.bitrate;
    u32 dbitrate = priv->can.data_bittiming.bitrate;
    u32 mode;
    int err;

    if (!netif_running(priv->netdev) || !atomic_read(&dev->present))
        return;

    if (!bitrate)
        bitrate = BMCAN_DEFAULT_BITRATE;

    /* Determine CAN-FD data bitrate */
#ifdef CANFD_MTU
    if (priv->netdev->mtu == CANFD_MTU)
    {
        if (!dbitrate)
            dbitrate = BMCAN_DEFAULT_DBITRATE;
    }
else
#endif
    {
        dbitrate = bitrate;
    }

    mode = bmcan_get_can_mode(priv);

    pr_debug(BMCAN_DRIVER_NAME ": %s open_workfn: port=%u bitrate=%u dbitrate=%u mode=0x%x\n",
            priv->netdev->name, priv->port, bitrate, dbitrate, mode);

    /* Apply nominal/data bitrate, RX filters, and CAN mode.
     * Sequence matches BMAPI SDK: BITRATE -> delay -> MODE */
    if (!bmcan_disable_ctrl)
    {
        /* Step 1: Set bitrate FIRST (before mode) */
        pr_debug("bmcan: %s open_workfn: setting bitrate %u/%u\n",
                priv->netdev->name, bitrate, dbitrate);
        err = bmcan_proto_set_bitrate(dev, priv->port, bitrate, dbitrate,
                                     priv->can.bittiming.sample_point / 10,
                                     priv->can.data_bittiming.sample_point / 10);
        if (err < 0)
        {
            pr_err(BMCAN_DRIVER_NAME ": %s set bitrate failed (%d)\n", priv->netdev->name, err);
            return;
        }
        pr_debug("bmcan: %s open_workfn: bitrate OK\n", priv->netdev->name);

        /* Step 2: Brief delay like BMAPI Sleep(10) between bitrate and mode */
        msleep(10);

        /* Step 3: Configure RX filters BEFORE setting mode */
        if (bmcan_rxfilter_enable)
        {
            struct bmcan_rx_filter filter;

            memset(&filter, 0, sizeof(filter));
            filter.type = BM_RXFILTER_BASIC;
            filter.flags_mask = (bmcan_rxfilter_rtr ? 0x01 : 0x00) | (bmcan_rxfilter_ext ? 0x10 : 0x00);
            filter.flags_value = (bmcan_rxfilter_rtr ? 0x01 : 0x00) | (bmcan_rxfilter_ext ? 0x10 : 0x00);
            filter.id_mask = cpu_to_le32(bmcan_rxfilter_mask);
            filter.id_value = cpu_to_le32(bmcan_rxfilter_id);

            err = bmcan_proto_set_rxfilter(dev, priv->port, 0, &filter);
            if (err < 0)
                pr_warn(BMCAN_DRIVER_NAME ": %s set rxfilter[0] failed (%d)\n",
                        priv->netdev->name, err);

            memset(&filter, 0, sizeof(filter));
            filter.type = BM_RXFILTER_INVALID;
            err = bmcan_proto_set_rxfilter(dev, priv->port, 1, &filter);
            if (err < 0)
                pr_warn(BMCAN_DRIVER_NAME ": %s disable rxfilter[1] failed (%d)\n",
                        priv->netdev->name, err);
        }

        /* Step 4: Set CAN mode (after bitrate is configured) */
        pr_debug("bmcan: %s open_workfn: setting mode 0x%x (port=%u)\n",
                priv->netdev->name, mode, priv->port);
        err = bmcan_set_operating_mode(priv);
        if (err < 0)
        {
            pr_err(BMCAN_DRIVER_NAME ": %s set mode failed (%d)\n", priv->netdev->name, err);
            return;
        }
        pr_debug("bmcan: %s open_workfn: mode 0x%x OK\n", priv->netdev->name, priv->operating_mode);
    }
}

static inline struct bmcan_priv *bmcan_priv_from_netdev(const struct net_device *netdev)
{
    return container_of((struct can_priv *)netdev_priv((struct net_device *)netdev),
                        struct bmcan_priv, can);
}

static const struct can_bittiming_const bmcan_bittiming_const =
{
    .name = "bmcan",
    .tseg1_min = 1,
    .tseg1_max = 16,
    .tseg2_min = 1,
    .tseg2_max = 8,
    .sjw_max = 4,
    .brp_min = 1,
    .brp_max = 1024,
    .brp_inc = 1,
};

static const struct can_bittiming_const bmcan_data_bittiming_const =
{
    .name = "bmcan_fd",
    .tseg1_min = 1,
    .tseg1_max = 256,  /* Wide range for flexible sample-point */
    .tseg2_min = 1,
    .tseg2_max = 256,
    .sjw_max = 256,
    .brp_min = 1,
    .brp_max = 1024,
    .brp_inc = 1,
};

static int bmcan_validate_bittiming(const struct net_device *netdev,
                                    const struct can_bittiming *bt,
                                    const struct can_bittiming_const *btc,
                                    const char *name)
{
    u32 tseg1;

    if (!bt->bitrate || !bt->tq || !bt->brp)
        goto invalid;

    if (!bt->sample_point || bt->sample_point >= 1000)
        goto invalid;

    tseg1 = bt->prop_seg + bt->phase_seg1;
    if (tseg1 < btc->tseg1_min || tseg1 > btc->tseg1_max)
        goto invalid;

    if (bt->phase_seg2 < btc->tseg2_min || bt->phase_seg2 > btc->tseg2_max)
        goto invalid;

    if (!bt->sjw || bt->sjw > btc->sjw_max || bt->sjw > bt->phase_seg2)
        goto invalid;

    if (bt->brp < btc->brp_min || bt->brp > btc->brp_max)
        goto invalid;

    if (btc->brp_inc && ((bt->brp - btc->brp_min) % btc->brp_inc))
        goto invalid;

    return 0;

invalid:
    netdev_err(netdev,
               "invalid %s bittiming: bitrate=%u sample-point=%u.%u%% tq=%u prop=%u phase1=%u phase2=%u sjw=%u brp=%u\n",
               name, bt->bitrate, bt->sample_point / 10, bt->sample_point % 10,
               bt->tq, bt->prop_seg, bt->phase_seg1, bt->phase_seg2,
               bt->sjw, bt->brp);
    return -EINVAL;
}

static u32 bmcan_get_can_mode(const struct bmcan_priv *priv)
{
    u32 mode;
    bool fd_enabled = bmcan_force_fd;

#ifdef CANFD_MTU
    if (priv->netdev && priv->netdev->mtu == CANFD_MTU)
        fd_enabled = true;
#endif

    pr_debug_ratelimited("bmcan: %s mode_check: ctrlmode=0x%x FD_flag=%d mtu=%u mtu_fd=%d force_fd=%d\n",
             priv->netdev->name, priv->can.ctrlmode,
             !!(priv->can.ctrlmode & CAN_CTRLMODE_FD),
             priv->netdev ? priv->netdev->mtu : 0,
             !!(priv->netdev && priv->netdev->mtu == CANFD_MTU),
             bmcan_force_fd);

    if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
        mode = BM_CAN_LISTEN_ONLY_MODE;
    else if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
        mode = BM_CAN_INTERNAL_LOOPBACK_MODE;
    else if (fd_enabled || (priv->can.ctrlmode & CAN_CTRLMODE_FD))
    {
        mode = BM_CAN_NORMAL_MODE;
        pr_debug("bmcan: %s using CAN FD mode (fd_enabled=%d)\n", priv->netdev->name, fd_enabled);
    }
    else
        mode = BM_CAN_CLASSIC_MODE;

    if (priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
        mode |= BM_CAN_NON_ISO_MODE;

#ifdef CAN_CTRLMODE_ONE_SHOT
    /* One-shot maps to device no-auto-retry mode. */
    if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
        mode |= BM_CAN_NON_AUTORETX_MODE;
#endif

#ifdef CAN_CTRLMODE_PRESUME_ACK
    /* Presume-ACK maps to device no-ack mode. */
    if (priv->can.ctrlmode & CAN_CTRLMODE_PRESUME_ACK)
        mode |= BM_CAN_NOACK_MODE;
#endif

    return mode;
}

/* Set operating mode, optionally OR'ing BM_CAN_DISABLE_AUTO_BUSOFF_RECOVERY
 * based on restart_ms and firmware version.
 * Stores the clean operating_mode (without disable flag) for later recovery. */
static int bmcan_set_operating_mode(struct bmcan_priv *priv)
{
    struct bmcan_dev *dev = priv->parent;
    u32 mode = bmcan_get_can_mode(priv);

    priv->operating_mode = mode;

    if (dev->fw_version >= BMCAN_FW_BUSOFF_CMD_VERSION && priv->can.restart_ms != 0)
        mode |= BM_CAN_DISABLE_AUTO_BUSOFF_RECOVERY;

    return bmcan_proto_set_mode(dev, priv->port, mode);
}

static int bmcan_change_mtu(struct net_device *netdev, int new_mtu)
{
    if (new_mtu == CAN_MTU)
    {
        netdev->mtu = new_mtu;
        return 0;
    }

#if IS_ENABLED(CONFIG_CAN_FD)
    if (new_mtu == CANFD_MTU)
    {
        netdev->mtu = new_mtu;
        return 0;
    }
#endif

    return -EINVAL;
}

/* Nominal bitrate path for classic CAN and base phase of CAN FD. */
static int bmcan_do_set_bittiming(struct net_device *netdev)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    int ret;

    ret = bmcan_validate_bittiming(netdev, &priv->can.bittiming,
                                   &bmcan_bittiming_const, "nominal");
    if (ret)
        return ret;

    /* Only update bittiming in memory, do not configure hardware immediately */
    /* Hardware configuration is done in open_workfn */
    /* Note: SocketCAN does not allow bitrate modification in UP state, must DOWN then UP */
    pr_debug(BMCAN_DRIVER_NAME ": %s bittiming updated (deferred to open)\n", netdev->name);
    return 0;
}

/* Data bitrate path for CAN FD (BRS). */
#ifdef CANFD_MTU
static int bmcan_do_set_data_bittiming(struct net_device *netdev)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    int ret;

    ret = bmcan_validate_bittiming(netdev, &priv->can.data_bittiming,
                                   &bmcan_data_bittiming_const, "data");
    if (ret)
        return ret;

    /* Keep kernel calculated data_bittiming values
     * Since data_bittiming_const is set, kernel will calculate proper values
     * based on user input (dbitrate, dsample-point).
     * Hardware configuration is done in open_workfn */
    pr_debug(BMCAN_DRIVER_NAME ": %s data_bittiming updated (deferred to open)\n", netdev->name);
    return 0;
}
#endif

/* Send dummy CAN frames directly via USB bulk OUT, bypassing SocketCAN stack.
 * Frame format: CAN 2.0, ID=0x0, RTR, DLC=0.
 * NOT counted in netdev TX/RX statistics. */
static int bmcan_send_dummy_frames(struct bmcan_priv *priv)
{
    struct bmcan_dev *dev = priv->parent;
    u8 *buf;
    size_t total_len = 0, frame_len;
    int i, err;

    /* Each dummy frame ~16 bytes. 256 * 16 = 4096 bytes. */
    buf = kzalloc(4096, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    for (i = 0; i < BMCAN_BUSOFF_DUMMY_COUNT; i++)
    {
        err = bmcan_proto_build_tx(priv, 0x0, false, true,
                                    false, false, 0, NULL,
                                    buf + total_len, &frame_len);
        if (err)
            goto free_buf;
        total_len += frame_len;
    }

    err = usb_bulk_msg(dev->udev,
                       usb_sndbulkpipe(dev->udev, dev->ep_out),
                       buf, (int)total_len, NULL, 2000);
free_buf:
    kfree(buf);
    return err;
}

/* Recover from bus-off using loopback mode + dummy frames.
 * Used when firmware does not support the F5 bus-off recovery command.
 * Temporarily switches to high-speed bitrate (1000K/8000K) for fast recovery,
 * then restores original bitrate and operating mode. */
static int bmcan_loopback_recovery(struct bmcan_priv *priv)
{
    struct bmcan_dev *dev = priv->parent;
    struct net_device *netdev = priv->netdev;
    u32 bitrate = priv->can.bittiming.bitrate;
    u32 dbitrate = priv->can.data_bittiming.bitrate;
    int err;

    pr_info(BMCAN_DRIVER_NAME ": %s loopback recovery: saving bitrate %u/%u\n",
            netdev->name, bitrate, dbitrate);

    /* Step 1: Set high-speed bitrate for fast recovery (1000K/8000K) */
    err = bmcan_proto_set_bitrate(dev, priv->port, 1000000, 8000000, 87, 75);
    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s loopback recovery: set high-speed bitrate failed (%d)\n",
                netdev->name, err);
    msleep(10);

    /* Step 2: Switch directly to internal loopback mode */
    err = bmcan_proto_set_mode(dev, priv->port, BM_CAN_INTERNAL_LOOPBACK_MODE);
    if (err)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s loopback recovery: set loopback mode failed (%d)\n",
                netdev->name, err);
        goto restore;
    }
    msleep(10);

    /* Step 3: Send 256 dummy frames (ID=0x0, RTR, DLC=0) */
    WRITE_ONCE(priv->loopback_recovery_active, true);
    err = bmcan_send_dummy_frames(priv);
    WRITE_ONCE(priv->loopback_recovery_active, false);

    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s loopback recovery: dummy frames failed (%d)\n",
                netdev->name, err);

restore:
    /* Step 4: Restore original bitrate */
    bmcan_proto_set_bitrate(dev, priv->port, bitrate, dbitrate,
                            priv->can.bittiming.sample_point / 10,
                            priv->can.data_bittiming.sample_point / 10);
    msleep(10);

    /* Step 5: Restore original operating mode */
    bmcan_set_operating_mode(priv);

    pr_info(BMCAN_DRIVER_NAME ": %s loopback recovery: restored bitrate %u/%u mode=0x%x\n",
            netdev->name, bitrate, dbitrate, priv->operating_mode);

    return err;
}

/* Bus-off restart work: executed in process context (workqueue), not softirq.
 * Chooses recovery method based on firmware version:
 * - Firmware >= 3.1: send F5 bus-off recovery command
 * - Firmware < 3.1:  loopback + 256 dummy frames */
static void bmcan_restart_workfn(struct work_struct *work)
{
    struct bmcan_priv *priv = container_of(work, struct bmcan_priv, restart_work);
    struct net_device *netdev = priv->netdev;
    struct bmcan_dev *dev = priv->parent;
    int err = 0;

    if (!netif_running(netdev) || !atomic_read(&dev->present))
        return;

    pr_info(BMCAN_DRIVER_NAME ": %s bus-off recovery starting (fw=%08x)\n",
            netdev->name, dev->fw_version);

    if (dev->fw_version >= BMCAN_FW_BUSOFF_CMD_VERSION)
    {
        /* Firmware >= 3.1: send F5 bus-off recovery command */
        err = bmcan_proto_busoff_cmd_recovery(dev, priv->port);
        if (err)
        {
            pr_warn(BMCAN_DRIVER_NAME
                    ": %s firmware command recovery failed (%d), fallback to loopback\n",
                    netdev->name, err);
            err = bmcan_loopback_recovery(priv);
        }
        else
        {
            pr_info(BMCAN_DRIVER_NAME ": %s firmware command recovery sent\n", netdev->name);
        }
    }
    else
    {
        /* Firmware < 3.1: loopback + dummy frames */
        err = bmcan_loopback_recovery(priv);
    }

    if (err)
        pr_err(BMCAN_DRIVER_NAME ": %s bus-off recovery failed (%d)\n",
               netdev->name, err);
    else
        pr_info(BMCAN_DRIVER_NAME ": %s bus-off recovery complete\n", netdev->name);

    /* Always reset TX state */
    bmcan_start_tx_recovery(priv, "bus-off restart");
}

/* Bus-off auto-restart: called from softirq (restart timer).
 * Only updates lightweight state here; defers heavy work to restart_work. */
static int bmcan_do_set_mode(struct net_device *netdev, enum can_mode mode)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *dev = priv->parent;

    if (!atomic_read(&dev->present))
        return -ENODEV;

    switch (mode)
    {
    case CAN_MODE_START:
        priv->can.state = CAN_STATE_ERROR_ACTIVE;
        priv->last_status.txbo = false;
        /* Defer heavy TX state reset to process context.
         * Use dedicated workqueue to avoid blocking system_wq
         * during loopback recovery (can take ~200ms-2s). */
        queue_work(bmcan_status_wq, &priv->restart_work);
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

/* Termination resistor setting support */
static const u16 bmcan_termination_const[] = { 0, 60, 120 };

static int bmcan_do_set_termination(struct net_device *netdev, u16 termination)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *dev = priv->parent;
    int ret;

    /*
     * Map SocketCAN termination values to hardware protocol values.
     * Hardware only supports 120Ω and disabled (0xFFFF).
     * - 0 (off)  -> 0xFFFF (BM_TRESISTOR_DISABLED)
     * - 60       -> 120    (hardware does not support 60Ω, fallback to 120)
     * - 120      -> 120    (BM_TRESISTOR_120)
     *
     * Reference: bm_usb_def.h BM_TerminalResistorTypeDef
     */
    {
        u16 hw_val = termination;
        if (hw_val == 0)
            hw_val = 0xFFFF; /* Map 0 to DISABLED */
        else if (hw_val == 60)
        {
            hw_val = 120; /* Hardware does not support 60Ω, fallback to 120 */
            pr_info(BMCAN_DRIVER_NAME ": %s 60Ω termination not supported, using 120Ω\n",
                    netdev->name);
        }

        ret = bmcan_proto_set_terminal_resistor(dev, priv->port, hw_val);
        if (ret < 0)
        {
            pr_warn(BMCAN_DRIVER_NAME ": %s set termination %u failed (%d)\n",
                    netdev->name, termination, ret);
            return ret;
        }
    }

    /* Store the actual effective value so queries reflect hardware state.
     * 0→0 (off), 60→120 (fallback), 120→120 */
    priv->can.termination = (termination == 60) ? 120 : termination;
    pr_debug(BMCAN_DRIVER_NAME ": %s set termination %u\n", netdev->name, termination);
    return 0;
}

#ifdef CAN_CTRLMODE_BERR_REPORTING
static int bmcan_get_berr_counter(const struct net_device *netdev,
                                  struct can_berr_counter *bec)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_can_status st;
    int ret;

    if (!bec)
        return -EINVAL;

    ret = bmcan_proto_get_status(priv->parent, priv->port, &st);
    if (ret < 0)
        return ret;

    bec->txerr = st.tec;
    bec->rxerr = st.rec;
    return 0;
}
#endif

static void bmcan_report_status(struct bmcan_priv *priv, const struct bmcan_can_status *st)
{
    struct net_device *netdev = priv->netdev;
    enum can_state new_state;

    if (st->txbo)
        new_state = CAN_STATE_BUS_OFF;
    else if (st->txbp || st->rxbp)
        new_state = CAN_STATE_ERROR_PASSIVE;
    else if (st->txwarn || st->rxwarn)
        new_state = CAN_STATE_ERROR_WARNING;
    else
        new_state = CAN_STATE_ERROR_ACTIVE;

    if (new_state == CAN_STATE_BUS_OFF)
    {
        if (!priv->last_status_valid || !priv->last_status.txbo)
        {
            can_bus_off(netdev);
        }
        priv->can.state = CAN_STATE_BUS_OFF;
    }
    else
    {
        /* Only report state transitions — recovery is handled by the
         * kernel restart timer via do_set_mode() -> Plan A/B. */
        if (new_state == CAN_STATE_ERROR_PASSIVE &&
                 priv->can.state != CAN_STATE_ERROR_PASSIVE)
        {
            priv->can.can_stats.error_passive++;
            priv->can.state = new_state;
        }
        else if (new_state == CAN_STATE_ERROR_WARNING &&
                 priv->can.state != CAN_STATE_ERROR_WARNING)
        {
            priv->can.can_stats.error_warning++;
            priv->can.state = new_state;
        }
        else
        {
            priv->can.state = new_state;
        }
    }

#ifdef CAN_CTRLMODE_BERR_REPORTING
    if (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING)
    {
        if (!priv->last_status_valid || memcmp(&priv->last_status, st, sizeof(*st)) != 0)
        {
            struct can_frame *cf;
            struct sk_buff *skb = alloc_can_err_skb(netdev, &cf);
            if (skb)
            {
                cf->can_id |= CAN_ERR_CRTL;
                if (st->txwarn)
                    cf->data[1] |= CAN_ERR_CRTL_TX_WARNING;
                if (st->rxwarn)
                    cf->data[1] |= CAN_ERR_CRTL_RX_WARNING;
                if (st->txbp)
                    cf->data[1] |= CAN_ERR_CRTL_TX_PASSIVE;
                if (st->rxbp)
                    cf->data[1] |= CAN_ERR_CRTL_RX_PASSIVE;
                if (st->txbo)
                    cf->can_id |= CAN_ERR_BUSOFF;
                cf->data[6] = st->tec;
                cf->data[7] = st->rec;
                netdev->stats.rx_packets++;
                netdev->stats.rx_bytes += CAN_ERR_DLC;
                netif_receive_skb(skb);
            }
        }
    }
#endif
}

static void bmcan_status_workfn(struct work_struct *work)
{
    struct bmcan_priv *priv = container_of(to_delayed_work(work), struct bmcan_priv, status_work);
    struct bmcan_dev *dev = priv->parent;
    struct bmcan_can_status st;
    int ret;

    if (!bmcan_status_poll_ms)
        return;

    if (!netif_running(priv->netdev) || !atomic_read(&dev->present))
        return;

    atomic_set(&bmcan_last_activity, ACTIVITY_STATUS_WORK);

    ret = bmcan_proto_get_status(dev, priv->port, &st);

    if (ret >= 0)
    {
        pr_debug(BMCAN_DRIVER_NAME ": %s status: txbo=%d txbp=%d rxbp=%d txwarn=%d rxwarn=%d tec=%d rec=%d\n",
                priv->netdev->name, st.txbo, st.txbp, st.rxbp, st.txwarn, st.rxwarn, st.tec, st.rec);
        bmcan_report_status(priv, &st);
        priv->last_status = st;
        priv->last_status_valid = true;
    }

    /* TX stall watchdog: detect and recover from stuck TX path.
     * Must NOT require netif_queue_stopped — under a rapid stop/wake
     * cycle (URB stuck, flow control cycling) the queue may appear
     * running when checked, masking the stall.  Instead detect by
     * "no tx_complete for N seconds while frames are in flight". */
    if (!READ_ONCE(priv->tx_recovering) &&
        atomic_read(&priv->tx_busy) > 0 && priv->last_tx_complete_jiffies)
    {
        unsigned long no_progress = jiffies - priv->last_tx_complete_jiffies;
        if (no_progress > (BMCAN_TX_STALL_TIMEOUT_SEC * HZ))
        {
            priv->tx_stall_count++;
            pr_warn(BMCAN_DRIVER_NAME ": %s TX stall: no tx_complete for >%ds (tx_busy=%d, ongoing_bytes=%d, queue_stopped=%d), starting recovery #%u\n",
                    priv->netdev->name, BMCAN_TX_STALL_TIMEOUT_SEC,
                    atomic_read(&priv->tx_busy), atomic_read(&priv->tx_ongoing_bytes),
                    netif_queue_stopped(priv->netdev), priv->tx_stall_count);
            bmcan_start_tx_recovery(priv, "TX stall");
        }
    }
    else if (netif_queue_stopped(priv->netdev) && atomic_read(&priv->tx_busy) == 0)
    {
        unsigned long stopped_dur = jiffies - priv->last_queue_stop_jiffies;
        if (stopped_dur > (BMCAN_TX_STALL_TIMEOUT_SEC * HZ))
        {
            unsigned long wflags;
            priv->tx_stall_count++;
            pr_warn(BMCAN_DRIVER_NAME ": %s TX queue stuck for >%ds (tx_busy=0, ongoing_bytes=%d), force-waking (recovery #%u)\n",
                    priv->netdev->name, BMCAN_TX_STALL_TIMEOUT_SEC,
                    atomic_read(&priv->tx_ongoing_bytes), priv->tx_stall_count);
            spin_lock_irqsave(&priv->tx_pool_lock, wflags);
            atomic_set(&priv->tx_ongoing_bytes, 0);
            spin_unlock_irqrestore(&priv->tx_pool_lock, wflags);
            if (!READ_ONCE(priv->stopping) && !READ_ONCE(priv->tx_recovering) &&
                netif_running(priv->netdev) && atomic_read(&priv->parent->present))
                netif_wake_queue(priv->netdev);
        }
    }

    /* Periodic echo bitmap diagnostic (every ~60s, pr_debug only).
     * Detects echo bitmap / tx_busy inconsistency early. */
    priv->diag_status_count++;
    if (priv->diag_status_count >= 60)
    {
        unsigned long flags2;
        int free_slots, used_slots, tx_busy_val, ongoing;
        priv->diag_status_count = 0;

        spin_lock_irqsave(&priv->tx_pool_lock, flags2);
        free_slots = bitmap_weight(priv->echo_free_bm, BMCAN_TX_ECHO_MAX);
        used_slots = BMCAN_TX_ECHO_MAX - free_slots;
        tx_busy_val = atomic_read(&priv->tx_busy);
        ongoing = atomic_read(&priv->tx_ongoing_bytes);
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags2);

        pr_debug(BMCAN_DRIVER_NAME ": %s diag: echo_used=%d tx_busy=%d ongoing=%d stopped=%d stall=%u\n",
                 priv->netdev->name, used_slots, tx_busy_val, ongoing,
                 netif_queue_stopped(priv->netdev), priv->tx_stall_count);
        if (used_slots != tx_busy_val)
            pr_warn(BMCAN_DRIVER_NAME ": %s echo leak: used=%d busy=%d delta=%d\n",
                    priv->netdev->name, used_slots, tx_busy_val, tx_busy_val - used_slots);
    }

    queue_delayed_work(bmcan_status_wq, &priv->status_work,
                       msecs_to_jiffies(bmcan_status_poll_ms));
}

static void bmcan_tx_complete(struct urb *urb)
{
    struct bmcan_tx_urb *txu = urb->context;
    struct bmcan_priv *priv = txu->priv;
    struct net_device *netdev;
    unsigned long flags;
    int i;

    /* Guard against disconnect or stop killing the URB.
     * Note: tx_active_urbs may not reach zero if stop clears priv first,
     * but bmcan_stop handles this with atomic_set(0) after the wait. */
    if (!priv)
        return;
    netdev = priv->netdev;
    atomic_set(&bmcan_last_activity, ACTIVITY_TX_COMPLETE);

    if (urb->status == 0)
    {
        if (bmcan_debug_comm)
            pr_info("bmcan: %s TX complete: %d frames OK\n",
                     netdev->name, txu->echo_count);
        pr_debug(BMCAN_DRIVER_NAME ": %s tx_complete: %d frames OK\n",
                 netdev->name, txu->echo_count);
        for (i = 0; i < txu->echo_count; i++)
        {
            if (!bmcan_disable_echo)
                bmcan_get_echo_skb(netdev, txu->echo_slots[i], txu->echo_lens[i]);
            netdev->stats.tx_packets++;
            netdev->stats.tx_bytes += txu->echo_lens[i];
        }
    }
    else
    {
        pr_debug(BMCAN_DRIVER_NAME ": %s tx_complete: FAILED status=%d\n",
                 netdev->name, urb->status);
        netdev->stats.tx_errors += txu->echo_count;
        for (i = 0; i < txu->echo_count; i++)
        {
            if (!bmcan_disable_echo)
                bmcan_free_echo_skb(netdev, txu->echo_slots[i], txu->echo_lens[i]);
        }
    }

    /* Return URB to pool and free echo slots */
    atomic_dec(&priv->tx_active_urbs);
    spin_lock_irqsave(&priv->tx_pool_lock, flags);
    txu->in_use = false;
    txu->priv = NULL;
    set_bit(txu->idx, priv->tx_free_bm);
    for (i = 0; i < txu->echo_count; i++)
        set_bit(txu->echo_slots[i], priv->echo_free_bm);
    atomic_sub(txu->echo_count, &priv->tx_busy);
    atomic_sub((int)txu->len, &priv->tx_ongoing_bytes);
    spin_unlock_irqrestore(&priv->tx_pool_lock, flags);

    priv->last_tx_complete_jiffies = jiffies;
    atomic_set(&bmcan_last_activity, ACTIVITY_TX_COMP_UNLOCK);
    /* Wake queue only when device is active and not shutting down.
     * During bmcan_stop, usb_kill_urb triggers this callback — without
     * the stopping check, we'd re-wake the queue into partially-reset TX state. */
    if (!READ_ONCE(priv->stopping) && !READ_ONCE(priv->tx_recovering) &&
        netif_running(netdev) && atomic_read(&priv->parent->present) &&
        (atomic_read(&priv->tx_busy) == 0 ||
         atomic_read(&priv->tx_busy) < BMCAN_TX_ECHO_MAX - 1 ||
         atomic_read(&priv->tx_ongoing_bytes) < (int)priv->parent->tx_limits.device_buf_size / 2))
    {
        atomic_set(&bmcan_last_activity, ACTIVITY_TX_COMP_WAKE);
        netif_wake_queue(netdev);
    }
}

/* Flush the current TX batch: submit accumulated frames as one USB transfer.
 * CAUTION: caller holds tx_pool_lock; this function releases it only when
 * it actually submits a batch (txu && echo_count > 0). Early return paths
 * (empty batch) leave the lock held — caller must handle this. */
static void bmcan_tx_flush_batch(struct bmcan_priv *priv, unsigned long flags)
{
    struct bmcan_dev *dev = priv->parent;
    struct bmcan_tx_urb *txu = priv->tx_batch;

    if (!txu || txu->echo_count == 0)
        return;

    priv->tx_batch = NULL;
    /* Account bytes BEFORE dropping lock so flow control sees accurate value */
    atomic_add((int)txu->len, &priv->tx_ongoing_bytes);
    spin_unlock_irqrestore(&priv->tx_pool_lock, flags);

    pr_debug_ratelimited(BMCAN_DRIVER_NAME ": %s flush batch: %d frames %zu bytes\n",
                         priv->netdev->name, txu->echo_count, txu->len);
    if (bmcan_debug_comm)
        pr_info("bmcan: %s TX submit: %d frames %zu bytes\n",
                priv->netdev->name, txu->echo_count, txu->len);

    bmcan_fill_bulk_urb(txu->urb, dev->udev,
                      usb_sndbulkpipe(dev->udev, dev->ep_out),
                      txu->buf, (int)txu->len, bmcan_tx_complete, txu);

    atomic_inc(&priv->tx_active_urbs);
    atomic_set(&bmcan_last_activity, ACTIVITY_TX_FLUSH);
    if (usb_submit_urb(txu->urb, GFP_ATOMIC))
    {
        int i;
        atomic_dec(&priv->tx_active_urbs);
        pr_err_ratelimited(BMCAN_DRIVER_NAME ": %s batch submit failed\n",
                           priv->netdev->name);
        priv->netdev->stats.tx_errors += txu->echo_count;
        for (i = 0; i < txu->echo_count; i++)
        {
            if (!bmcan_disable_echo)
                bmcan_free_echo_skb(priv->netdev, txu->echo_slots[i],
                                    txu->echo_lens[i]);
        }
        spin_lock_irqsave(&priv->tx_pool_lock, flags);
        txu->in_use = false;
        txu->priv = NULL;
        set_bit(txu->idx, priv->tx_free_bm);
        for (i = 0; i < txu->echo_count; i++)
            set_bit(txu->echo_slots[i], priv->echo_free_bm);
        atomic_sub(txu->echo_count, &priv->tx_busy);
        atomic_sub((int)txu->len, &priv->tx_ongoing_bytes);
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
        if (!READ_ONCE(priv->stopping) && !READ_ONCE(priv->tx_recovering) &&
            netif_running(priv->netdev) && atomic_read(&priv->parent->present))
            netif_wake_queue(priv->netdev);
    }
    return;
}

/* Delayed work to flush partial batch */
static void bmcan_tx_flush_workfn(struct work_struct *work)
{
    struct bmcan_priv *priv = container_of(work, struct bmcan_priv,
                                           tx_flush_work.work);
    unsigned long flags;

    spin_lock_irqsave(&priv->tx_pool_lock, flags);
    if (priv->tx_batch && priv->tx_batch->echo_count > 0)
        bmcan_tx_flush_batch(priv, flags);
    else
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
}

/*
 * TX path: convert SocketCAN skb to BM USB payload with multi-frame batching.
 * Multiple frames are packed into a single USB transfer (4-byte aligned each)
 * to reduce per-transfer overhead and match BMAPI SDK behavior.
 * Byte-based flow control prevents overwhelming the device TXQ.
 */
static netdev_tx_t bmcan_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *dev = priv->parent;
    struct bmcan_tx_urb *txu = NULL;
    u8 *buf;
    size_t len = 0, frame_size;
    int ret, i;
    u32 can_id;
    bool is_ext;
    bool is_rtr;
#ifdef CANFD_MTU
    bool brs = false;
#endif
    u8 data_len;
    const u8 *data;
    unsigned long flags;
    u32 max_packet = dev->tx_limits.max_packet_size;
    u32 device_buf = dev->tx_limits.device_buf_size;

    if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
    {
        netdev->stats.tx_dropped++;
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    if (!atomic_read(&dev->present))
    {
        netdev->stats.tx_dropped++;
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    atomic_set(&bmcan_last_activity, ACTIVITY_XMIT_ENTER);

    /* Extract CAN frame fields from skb */
#ifdef CANFD_MTU
    if (can_is_canfd_skb(skb))
    {
        const struct canfd_frame *cfd = (const struct canfd_frame *)skb->data;
        can_id = cfd->can_id & ((cfd->can_id & CAN_EFF_FLAG) ? CAN_EFF_MASK : CAN_SFF_MASK);
        is_ext = (cfd->can_id & CAN_EFF_FLAG) != 0;
        is_rtr = (cfd->can_id & CAN_RTR_FLAG) != 0;
        brs = (cfd->flags & CANFD_BRS) != 0;
        data_len = cfd->len;
        data = cfd->data;
    }
    else
#endif
    {
#ifndef CANFD_MTU
        if (can_is_canfd_skb(skb))
            goto drop_nolock;
#endif
        const struct can_frame *cf = (const struct can_frame *)skb->data;
        can_id = cf->can_id & ((cf->can_id & CAN_EFF_FLAG) ? CAN_EFF_MASK : CAN_SFF_MASK);
        is_ext = (cf->can_id & CAN_EFF_FLAG) != 0;
        is_rtr = (cf->can_id & CAN_RTR_FLAG) != 0;
        data_len = cf->can_dlc;
        data = cf->data;
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s xmit: id=0x%x len=%d ext=%d rtr=%d\n",
             netdev->name, can_id, data_len, is_ext, is_rtr);

    if (bmcan_debug_comm)
        pr_info("bmcan: %s xmit: id=0x%x len=%d ext=%d rtr=%d fd=%d\n",
                netdev->name, can_id, data_len, is_ext, is_rtr,
                can_is_canfd_skb(skb));

    /* Estimate frame size (4-byte aligned, matching build_tx output) */
    frame_size = ((8 + 8 + data_len) + 3) & ~(size_t)3;

    spin_lock_irqsave(&priv->tx_pool_lock, flags);
    atomic_set(&bmcan_last_activity, ACTIVITY_XMIT_LOCKED);
    if (READ_ONCE(priv->tx_recovering))
    {
        bmcan_stop_tx_queue(netdev);
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
        return NETDEV_TX_BUSY;
    }
    if (atomic_read(&priv->tx_ongoing_bytes) + (int)frame_size > (int)device_buf)
    {
        bmcan_stop_tx_queue(netdev);
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
        return NETDEV_TX_BUSY;
    }

    /* Allocate echo slot for this frame */
    i = find_first_bit(priv->echo_free_bm, BMCAN_TX_ECHO_MAX);
    if (i >= BMCAN_TX_ECHO_MAX)
    {
        bmcan_stop_tx_queue(netdev);
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
        return NETDEV_TX_BUSY;
    }
    clear_bit(i, priv->echo_free_bm);

    /* Try to append to existing batch, or retry after flush.
     * After flush releases/reacquires the lock, another CPU may have
     * allocated a new batch — loop back to check. */
retry_batch:
    txu = priv->tx_batch;
    if (txu)
    {
        if (txu->echo_count < BMCAN_TX_MAX_BATCH &&
            txu->len + frame_size <= max_packet &&
            atomic_read(&priv->tx_ongoing_bytes) + (int)(txu->len + frame_size) <= (int)device_buf)
        {
            /* Append to existing batch */
            buf = txu->buf + txu->len;
            ret = bmcan_proto_build_tx(priv, can_id, is_ext, is_rtr,
                                        (can_is_canfd_skb(skb)),
#ifdef CANFD_MTU
                                        brs,
#else
                                        false,
#endif
                                        data_len, data, buf, &len);
            if (ret)
            {
                set_bit(i, priv->echo_free_bm);
                goto drop;
            }

            txu->echo_slots[txu->echo_count] = i;
            txu->echo_lens[txu->echo_count] = data_len;
            txu->echo_count++;
            txu->len += len;
            atomic_inc(&priv->tx_busy);

            if (!bmcan_disable_echo)
                bmcan_put_echo_skb(skb, netdev, i, data_len);
            else
                dev_kfree_skb_any(skb);

            /* Flush if batch is full (byte limit or frame count) */
            if (txu->len + 32 > max_packet || txu->echo_count >= BMCAN_TX_MAX_BATCH)
            {
                atomic_set(&bmcan_last_activity, ACTIVITY_XMIT_FLUSHING);
                bmcan_tx_flush_batch(priv, flags);
            }
            else
            {
                /* Schedule flush for partial batch */
                mod_delayed_work(bmcan_tx_wq, &priv->tx_flush_work,
                                 usecs_to_jiffies(500));
                spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
            }
            return NETDEV_TX_OK;
        }
        /* Can't append, flush existing batch and start new one */
        atomic_set(&bmcan_last_activity, ACTIVITY_XMIT_FLUSHING);
        bmcan_tx_flush_batch(priv, flags);
        spin_lock_irqsave(&priv->tx_pool_lock, flags);
        /* Re-validate: another CPU may have allocated tx_batch
         * while the lock was dropped. Retry appending. */
        if (priv->tx_batch)
            goto retry_batch;
    }

    /* Allocate new URB for batch */
    {
        int j = find_first_bit(priv->tx_free_bm, BMCAN_NUM_TX_URBS);
        if (j >= BMCAN_NUM_TX_URBS || !priv->tx_urbs[j].urb || !priv->tx_urbs[j].buf)
        {
            set_bit(i, priv->echo_free_bm);
            bmcan_stop_tx_queue(netdev);
            spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
            return NETDEV_TX_BUSY;
        }
        txu = &priv->tx_urbs[j];
        txu->in_use = true;
        clear_bit(j, priv->tx_free_bm);
        txu->priv = priv;
        txu->echo_count = 0;
        txu->len = 0;
    }

    /* Build first frame into batch buffer */
    buf = txu->buf;
    ret = bmcan_proto_build_tx(priv, can_id, is_ext, is_rtr,
                                (can_is_canfd_skb(skb)),
#ifdef CANFD_MTU
                                brs,
#else
                                false,
#endif
                                data_len, data, buf, &len);
    if (ret)
    {
        set_bit(i, priv->echo_free_bm);
        txu->in_use = false;
        set_bit(txu->idx, priv->tx_free_bm);
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
        goto drop_nolock;
    }

    txu->echo_slots[0] = i;
    txu->echo_lens[0] = data_len;
    txu->echo_count = 1;
    txu->len = len;

    atomic_inc(&priv->tx_busy);

    if (!bmcan_disable_echo)
        bmcan_put_echo_skb(skb, netdev, i, data_len);
    else
        dev_kfree_skb_any(skb);

    priv->tx_batch = txu;

    if (atomic_read(&priv->tx_busy) >= BMCAN_TX_ECHO_MAX)
        bmcan_stop_tx_queue(netdev);

    /* Schedule flush for partial batch */
    mod_delayed_work(bmcan_tx_wq, &priv->tx_flush_work, usecs_to_jiffies(500));
    spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
    return NETDEV_TX_OK;

drop:
    spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
drop_nolock:
    netdev->stats.tx_dropped++;
    dev_kfree_skb_any(skb);
    return NETDEV_TX_OK;
}

static int bmcan_open(struct net_device *netdev)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *dev = priv->parent;
    u32 bitrate = priv->can.bittiming.bitrate;
    u32 dbitrate = priv->can.data_bittiming.bitrate;
    u32 mode;
    int err;

    atomic_set(&bmcan_last_activity, ACTIVITY_OPEN);
    if (!atomic_read(&dev->present))
        return -ENODEV;

    WRITE_ONCE(priv->stopping, false);
    WRITE_ONCE(priv->tx_recovering, false);

    if (bmcan_fail_open)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s open blocked by module param\n", netdev->name);
        return -EOPNOTSUPP;
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s open\n", netdev->name);

    /* Auto-invalidate runtime config (txtask/route) - does NOT clear hardware storage */
    if (bmcan_auto_clear_config && !bmcan_disable_ctrl)
    {
        err = bmcan_proto_invalidate_config(dev, priv->port);
        if (err < 0)
        {
            pr_warn(BMCAN_DRIVER_NAME ": %s invalidate config failed (%d)\n", netdev->name, err);
            /* Do not interrupt open flow, continue execution */
        }
        else
        {
            pr_info(BMCAN_DRIVER_NAME ": %s invalidated previous config (txtask/route, hardware storage preserved)\n", netdev->name);
        }
    }

    err = open_candev(netdev);
    if (err)
        return err;

    if (!bitrate)
    {
        bitrate = BMCAN_DEFAULT_BITRATE;
        priv->can.bittiming.bitrate = bitrate;
        pr_debug(BMCAN_DRIVER_NAME ": %s using default bitrate %u\n", netdev->name, bitrate);
    }

    /* Determine data bitrate for CAN FD */
    if (!dbitrate || !(priv->can.ctrlmode & CAN_CTRLMODE_FD))
    {
#ifdef CANFD_MTU
        /* Check MTU to determine if FD mode is enabled */
        if (priv->netdev->mtu == CANFD_MTU)
        {
            /* FD mode but no dbitrate specified - use default */
            if (!dbitrate)
            {
                dbitrate = BMCAN_DEFAULT_DBITRATE;
                pr_debug(BMCAN_DRIVER_NAME ": %s using default dbitrate %u (FD mode)\n",
                        netdev->name, dbitrate);
            }
            /* Do not overwrite non-zero dbitrate value */
        }
        else
#endif
        {
            dbitrate = bitrate;
        }
    }

    mode = bmcan_get_can_mode(priv);

    pr_debug(BMCAN_DRIVER_NAME ": %s open: port=%u mode=0x%x mtu=%u "
            "nominal=%u/%u.%u%% data=%u/%u.%u%%\n",
            netdev->name, priv->port, mode, netdev->mtu,
            bitrate / 1000,
            priv->can.bittiming.sample_point / 10,
            priv->can.bittiming.sample_point % 10,
            dbitrate / 1000,
            priv->can.data_bittiming.sample_point / 10,
            priv->can.data_bittiming.sample_point % 10);

    /* Configure hardware synchronously before enabling TX path.
     * Sequence: bitrate -> delay -> RX filter -> mode (matches BMAPI SDK).
     * This eliminates the race where ip link up returns before HW is ready.
     * Only deferred when defer_open_ms > 0 for bring-up debugging. */
    if (!bmcan_disable_ctrl && !bmcan_defer_open_ms)
    {
        err = bmcan_proto_set_bitrate(dev, priv->port, bitrate, dbitrate,
                                     priv->can.bittiming.sample_point / 10,
                                     priv->can.data_bittiming.sample_point / 10);
        if (err < 0)
        {
            pr_err(BMCAN_DRIVER_NAME ": %s set bitrate failed (%d)\n",
                   netdev->name, err);
            close_candev(netdev);
            return err;
        }

        msleep(10);

        /* Configure RX filters before setting mode */
        if (bmcan_rxfilter_enable)
        {
            struct bmcan_rx_filter filter;

            memset(&filter, 0, sizeof(filter));
            filter.type = BM_RXFILTER_BASIC;
            filter.flags_mask = (bmcan_rxfilter_rtr ? 0x01 : 0x00) | (bmcan_rxfilter_ext ? 0x10 : 0x00);
            filter.flags_value = (bmcan_rxfilter_rtr ? 0x01 : 0x00) | (bmcan_rxfilter_ext ? 0x10 : 0x00);
            filter.id_mask = cpu_to_le32(bmcan_rxfilter_mask);
            filter.id_value = cpu_to_le32(bmcan_rxfilter_id);

            err = bmcan_proto_set_rxfilter(dev, priv->port, 0, &filter);
            if (err < 0)
                pr_warn(BMCAN_DRIVER_NAME ": %s set rxfilter[0] failed (%d)\n",
                        netdev->name, err);

            memset(&filter, 0, sizeof(filter));
            filter.type = BM_RXFILTER_INVALID;
            err = bmcan_proto_set_rxfilter(dev, priv->port, 1, &filter);
            if (err < 0)
                pr_warn(BMCAN_DRIVER_NAME ": %s disable rxfilter[1] failed (%d)\n",
                        netdev->name, err);
        }

        pr_debug(BMCAN_DRIVER_NAME ": %s open: setting mode 0x%x (port=%u)\n",
                netdev->name, mode, priv->port);
        err = bmcan_set_operating_mode(priv);
        if (err < 0)
        {
            pr_err(BMCAN_DRIVER_NAME ": %s set mode failed (%d)\n",
                   netdev->name, err);
            close_candev(netdev);
            return err;
        }
        pr_debug("bmcan: %s open: HW config complete (mode=0x%x)\n", netdev->name, priv->operating_mode);

#ifdef DEBUG
        /* Read back bitrate from firmware to verify configuration */
        {
            struct bm_bitrate rb;
            memset(&rb, 0, sizeof(rb));
            err = bmcan_proto_get_bitrate(dev, priv->port, &rb);
            if (err >= 0)
            {
                pr_info(BMCAN_DRIVER_NAME ": %s readback: nbitrate=%u dbitrate=%u nsamplepos=%u dsamplepos=%u nbtr0=%u nbtr1=%u dbtr0=%u dbtr1=%u clock=%u\n",
                        netdev->name,
                        le16_to_cpu(rb.nbitrate), le16_to_cpu(rb.dbitrate),
                        rb.nsamplepos, rb.dsamplepos,
                        rb.nbtr0, rb.nbtr1, rb.dbtr0, rb.dbtr1,
                        rb.clockfreq);
            }
            else
            {
                pr_warn(BMCAN_DRIVER_NAME ": %s bitrate readback failed (%d)\n",
                        netdev->name, err);
            }
        }
#endif
    }

    /* Track active netdevs per USB device. RX URBs are submitted at probe time
     * and kept alive across open/close — no kill/resubmit cycle here.
     * Retry if all RX URBs died (e.g. probe-time USB not ready). */
    mutex_lock(&dev->open_lock);
    if (atomic_read(&dev->open_count) == 0 &&
        atomic_read(&dev->rx_active_urbs) == 0 &&
        atomic_read(&dev->present))
    {
        pr_info(BMCAN_DRIVER_NAME ": %s re-submitting RX URBs (all inactive)\n",
                netdev->name);
        bmcan_usb_submit_rx(dev);
    }
    atomic_inc(&dev->open_count);
    mutex_unlock(&dev->open_lock);

    priv->can.state = CAN_STATE_ERROR_ACTIVE;
    netif_carrier_on(netdev);

    /* Purge any frames that accumulated in rxq while the netdev was down.
     * RX URBs remain active across open/close, so frames can arrive between
     * stop() and this open(). Without purging, stale frames are delivered to
     * sockets on the next NAPI poll, causing candump -n 1 to receive the wrong
     * frame and exit prematurely. */
    skb_queue_purge(&priv->rxq);

    napi_enable(&priv->napi);
    priv->last_tx_complete_jiffies = jiffies;
    priv->last_queue_stop_jiffies = 0;
    netif_start_queue(netdev);

    if (bmcan_status_poll_ms)
        queue_delayed_work(bmcan_status_wq, &priv->status_work,
                       msecs_to_jiffies(bmcan_status_poll_ms));

    /* Query device capability limits on first open (per USB device).
     * Done after HW is fully configured to ensure device is ready. */
    if (!dev->caps.loaded)
    {
        u32 val = 0;
        if (bmcan_usb_send_control(dev, BM_GET_STAT,
                                    BM_STAT_MAX_TXTASK, 0,
                                    (u8 *)&val, sizeof(val)) >= 0 && val > 0)
            dev->caps.max_txtask = val;
        else
            dev->caps.max_txtask = 64;  /* safe fallback */

        val = 0;
        if (bmcan_usb_send_control(dev, BM_GET_STAT,
                                    BM_STAT_MAX_ROUTE, 0,
                                    (u8 *)&val, sizeof(val)) >= 0 && val > 0)
            dev->caps.max_route = val;
        else
            dev->caps.max_route = 64;

        dev->caps.loaded = true;
        pr_info(BMCAN_DRIVER_NAME ": device caps: max_txtask=%u max_route=%u\n",
                dev->caps.max_txtask, dev->caps.max_route);
    }

    /* Deferred HW config for bring-up debugging only (defer_open_ms > 0).
     * Default path configures HW synchronously above. */
    if (bmcan_defer_open_ms)
        schedule_delayed_work(&priv->open_work, msecs_to_jiffies(bmcan_defer_open_ms));

    return 0;
}

static int bmcan_stop(struct net_device *netdev)
{
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *dev = priv->parent;
    int i;

    atomic_set(&bmcan_last_activity, ACTIVITY_CLOSE);
    pr_debug(BMCAN_DRIVER_NAME ": %s stop: port=%u open_count=%d\n",
            netdev->name, priv->port, atomic_read(&dev->open_count));

    WRITE_ONCE(priv->stopping, true);
    bmcan_stop_tx_queue(netdev);
    /* Cancel status work FIRST — it does TX stall recovery which kills
     * URBs, resets state, and wakes the queue.  If we don't cancel it
     * before killing TX URBs ourselves, both paths race and the stall
     * recovery can wake the queue into partially-reset TX state. */
    cancel_delayed_work_sync(&priv->status_work);
    /* Flush any pending TX batch and cancel deferred open */
    cancel_delayed_work_sync(&priv->tx_flush_work);
    cancel_delayed_work_sync(&priv->open_work);
    cancel_work_sync(&priv->restart_work);
    cancel_delayed_work_sync(&priv->tx_recover_work);

    /* Discard partial TX batch: free echo slots, return URB to pool */
    {
        unsigned long flags;
        spin_lock_irqsave(&priv->tx_pool_lock, flags);
        if (priv->tx_batch)
        {
            struct bmcan_tx_urb *batch = priv->tx_batch;
            int j;
            for (j = 0; j < batch->echo_count; j++)
            {
                if (!bmcan_disable_echo)
                    bmcan_free_echo_skb(netdev, batch->echo_slots[j],
                                        batch->echo_lens[j]);
                set_bit(batch->echo_slots[j], priv->echo_free_bm);
            }
            atomic_sub(batch->echo_count, &priv->tx_busy);
            batch->in_use = false;
            batch->priv = NULL;
            set_bit(batch->idx, priv->tx_free_bm);
            priv->tx_batch = NULL;
        }
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
    }

    /* Unlink all in-flight TX URBs asynchronously and wait with bounded timeout.
     * usb_unlink_urb is non-blocking — avoids indefinite stall if EHCI host
     * controller hangs (usb_kill_urb would block forever in that case). */
    for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
    {
        if (priv->tx_urbs[i].urb && priv->tx_urbs[i].in_use)
            usb_unlink_urb(priv->tx_urbs[i].urb);
    }
    {
        /* Bounded wait: 50 × 10ms = 500ms max for all TX URBs to complete */
        int wait_loops = 50;
        int active;
        while ((active = atomic_read(&priv->tx_active_urbs)) > 0 && --wait_loops > 0)
            msleep(10);
        if (active > 0)
            pr_warn(BMCAN_DRIVER_NAME ": %s stop: %d TX URBs still active after 500ms\n",
                    netdev->name, active);
    }

    cancel_work_sync(&priv->rx_deliver_work);
    napi_disable(&priv->napi);
    skb_queue_purge(&priv->rxq);
    netif_carrier_off(netdev);
    priv->can.state = CAN_STATE_STOPPED;

    /* Auto-invalidate runtime config (txtask/route) - does NOT clear hardware storage.
     * Skip if device is gone (USB disconnect) — USB control transfers would fail anyway.
     */
    if (bmcan_auto_clear_config && !bmcan_disable_ctrl && atomic_read(&dev->present))
    {
        int err;
        pr_debug(BMCAN_DRIVER_NAME ": %s stop: invalidate config\n", netdev->name);
        err = bmcan_proto_invalidate_config(dev, priv->port);
        if (err < 0)
            pr_warn(BMCAN_DRIVER_NAME ": %s invalidate config on stop failed (%d)\n", netdev->name, err);
    }

    if (atomic_read(&dev->present))
        bmcan_proto_set_mode(dev, priv->port, BM_CAN_CONFIGURATION_MODE);

    mutex_lock(&dev->open_lock);
    atomic_dec(&dev->open_count);
    mutex_unlock(&dev->open_lock);

    close_candev(netdev);

    /* Full TX state reset under lock: all URBs killed, echo skbs
     * freed by close_candev, so it is safe to reinitialize everything. */
    {
        unsigned long flags;
        spin_lock_irqsave(&priv->tx_pool_lock, flags);
        atomic_set(&priv->tx_busy, 0);
        atomic_set(&priv->tx_ongoing_bytes, 0);
        atomic_set(&priv->tx_active_urbs, 0);
        WRITE_ONCE(priv->tx_recovering, false);
        bitmap_fill(priv->echo_free_bm, BMCAN_TX_ECHO_MAX);
        bitmap_fill(priv->tx_free_bm, BMCAN_NUM_TX_URBS);
        for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
        {
            priv->tx_urbs[i].in_use = false;
            priv->tx_urbs[i].priv = NULL;
            priv->tx_urbs[i].echo_count = 0;
        }
        priv->tx_batch = NULL;
        spin_unlock_irqrestore(&priv->tx_pool_lock, flags);
    }

    return 0;
}

static const struct net_device_ops bmcan_netdev_ops =
{
    .ndo_open = bmcan_open,
    .ndo_stop = bmcan_stop,
    .ndo_start_xmit = bmcan_start_xmit,
    .ndo_change_mtu = bmcan_change_mtu,
};

#define BMCAN_TXTASK_MAX_BYTES 128
#define BMCAN_TXTASK_MAX_INDEX 64
#define BMCAN_ROUTE_MAX_BYTES 16
#define BMCAN_ROUTE_MAX_INDEX 256

static int bmcan_hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int bmcan_parse_hex_bytes(const char *s, u8 *out, size_t max, size_t *out_len)
{
    size_t len = 0;

    while (*s)
    {
        while (*s && (isspace(*s) || *s == ':' || *s == ','))
            s++;
        if (!*s)
            break;
        if (!isxdigit(s[0]) || !isxdigit(s[1]))
            return -EINVAL;
        if (len >= max)
            return -E2BIG;
        out[len++] = (u8)((bmcan_hex_val(s[0]) << 4) | bmcan_hex_val(s[1]));
        s += 2;
    }

    *out_len = len;
    return 0;
}

static ssize_t bmcan_show_tx_task(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    unsigned long flags;
    ssize_t len = 0;
    int i, j, valid_count = 0;
    /* Local buffer for quick data copy */
    struct bmcan_txtask_entry copy[BMCAN_TXTASK_CACHED_INDEX];
    bool loaded;
    u8 port;

    pr_debug(BMCAN_DRIVER_NAME ": %s reading TxTask configuration via sysfs\n",
             netdev->name);

    /* Quick data copy, minimize lock hold time */
    spin_lock_irqsave(&priv->config_cache.lock, flags);

    loaded = priv->config_cache.loaded;
    port = priv->port;

    if (loaded)
    {
        /* Quick copy all txtask entries */
        for (i = 0; i < BMCAN_TXTASK_CACHED_INDEX; i++)
        {
            if (priv->config_cache.txtask[i].valid)
            {
                memcpy(&copy[i], &priv->config_cache.txtask[i],
                       sizeof(struct bmcan_txtask_entry));
                valid_count++;
            }
            else
            {
                copy[i].valid = false;
            }
        }
    }

    spin_unlock_irqrestore(&priv->config_cache.lock, flags);

    /* Format output without lock */
    if (!loaded)
    {
        pr_debug(BMCAN_DRIVER_NAME ": %s TxTask read - config not loaded\n", netdev->name);
        return scnprintf(buf, PAGE_SIZE,
                       "Config not loaded from hardware yet.\n"
                       "Use load_config to sync with hardware.\n");
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s TxTask read - %d valid entries (port %u)\n",
            netdev->name, valid_count, port);

    len = scnprintf(buf, PAGE_SIZE, "# TXTask Configuration Cache (port %u)\n", port);
    len += scnprintf(buf + len, PAGE_SIZE - len, "# Format: index hex_data\n");

    for (i = 0; i < BMCAN_TXTASK_CACHED_INDEX && len < PAGE_SIZE - 200; i++)
    {
        if (copy[i].valid)
        {
            len += scnprintf(buf + len, PAGE_SIZE - len, "[%d] ", i);
            for (j = 0; j < BMCAN_TXTASK_ENTRY_SIZE && len < PAGE_SIZE - 10; j++)
            {
                len += scnprintf(buf + len, PAGE_SIZE - len, "%02X", copy[i].data[j]);
            }
            len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
        }
    }

    return len;
}

static ssize_t bmcan_store_tx_task(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *bdev = priv->parent;
    char *kbuf;
    char *p;
    char *nl;
    char *idx_str;
    char *data_str;
    unsigned int idx;
    u8 data[BMCAN_TXTASK_MAX_BYTES];
    size_t data_len = 0;
    int ret;
    unsigned long flags;

    /* Debug log: show received data */
    pr_debug(BMCAN_DRIVER_NAME ": %s tx_task write: count=%zu buf=%.*s\n",
            netdev->name, count, (int)min((size_t)count, (size_t)64), buf);

    kbuf = kstrdup(buf, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    p = strim(kbuf);
    /* Only process the first line (multi-index writes from bmcan_api may send
     * multiple lines in one sysfs write call). */
    nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    idx_str = strsep(&p, " \t");
    data_str = p;
    if (!idx_str || !data_str)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: invalid format (no index or data)\n",
                netdev->name);
        kfree(kbuf);
        return -EINVAL;
    }

    ret = kstrtouint(idx_str, 0, &idx);
    if (ret)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: invalid index '%s'\n",
                netdev->name, idx_str);
        kfree(kbuf);
        return ret;
    }
    if (bdev->caps.loaded && idx >= bdev->caps.max_txtask)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: index %u out of range (max %u)\n",
                netdev->name, idx, bdev->caps.max_txtask - 1);
        kfree(kbuf);
        return -ERANGE;
    }
    if (!bdev->caps.loaded && idx >= BMCAN_TXTASK_MAX_INDEX)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: index %u out of range (max %u)\n",
                netdev->name, idx, BMCAN_TXTASK_MAX_INDEX - 1);
        kfree(kbuf);
        return -ERANGE;
    }

    ret = bmcan_parse_hex_bytes(data_str, data, sizeof(data), &data_len);
    if (ret)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: hex parse failed (%d) data_str='%.*s'\n",
                netdev->name, ret, (int)min((size_t)40, strlen(data_str)), data_str);
        kfree(kbuf);
        return ret;
    }
    if (data_len != BMCAN_TXTASK_MAX_BYTES)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: data length %zu != required %u\n",
                netdev->name, data_len, BMCAN_TXTASK_MAX_BYTES);
        kfree(kbuf);
        return -EINVAL;
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s tx_task: idx=%u len=%zu\n",
             netdev->name, idx, data_len);

    ret = bmcan_proto_set_txtask(bdev, priv->port, (u16)idx, data, (u16)data_len);
    if (ret < 0)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s tx_task: failed to set entry %u (ret=%d)\n",
                netdev->name, idx, ret);
        kfree(kbuf);
        return ret;
    }

    /* Update cache (if index is within cache range) */
    if (idx < BMCAN_TXTASK_CACHED_INDEX)
    {
        spin_lock_irqsave(&priv->config_cache.lock, flags);
        memcpy(priv->config_cache.txtask[idx].data, data, BMCAN_TXTASK_ENTRY_SIZE);
        priv->config_cache.txtask[idx].valid = true;
        priv->config_cache.loaded = true;  /* Mark as loaded since we have valid config in memory */
        spin_unlock_irqrestore(&priv->config_cache.lock, flags);
        pr_debug(BMCAN_DRIVER_NAME ": %s TxTask entry added: index=%u\n",
                netdev->name, idx);
    }
    else
    {
        pr_debug(BMCAN_DRIVER_NAME ": %s TxTask entry added to hardware: index=%u (beyond cache range %u)\n",
                netdev->name, idx, BMCAN_TXTASK_CACHED_INDEX);
    }

    kfree(kbuf);
    return count;
}

static DEVICE_ATTR(txtasks, 0664, bmcan_show_tx_task, bmcan_store_tx_task);

static ssize_t bmcan_show_route(struct device *dev,
                                struct device_attribute *attr,
                                char *buf)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    unsigned long flags;
    ssize_t len = 0;
    int i, j, valid_count = 0;
    /* Local buffer for quick data copy */
    struct bmcan_route_entry copy[BMCAN_ROUTE_CACHED_INDEX];
    bool loaded;
    u8 port;

    pr_debug(BMCAN_DRIVER_NAME ": %s reading Route configuration via sysfs\n",
             netdev->name);

    /* Quick data copy, minimize lock hold time */
    spin_lock_irqsave(&priv->config_cache.lock, flags);

    loaded = priv->config_cache.loaded;
    port = priv->port;

    if (loaded)
    {
        /* Quick copy all route entries */
        for (i = 0; i < BMCAN_ROUTE_CACHED_INDEX; i++)
        {
            if (priv->config_cache.route[i].valid)
            {
                memcpy(&copy[i], &priv->config_cache.route[i],
                       sizeof(struct bmcan_route_entry));
                valid_count++;
            }
            else
            {
                copy[i].valid = false;
            }
        }
    }

    spin_unlock_irqrestore(&priv->config_cache.lock, flags);

    /* Format output without lock */
    if (!loaded)
    {
        pr_debug(BMCAN_DRIVER_NAME ": %s Route read - config not loaded\n", netdev->name);
        return scnprintf(buf, PAGE_SIZE,
                       "Config not loaded from hardware yet.\n"
                       "Use load_config to sync with hardware.\n");
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s Route read - %d valid entries (port %u)\n",
            netdev->name, valid_count, port);

    len = scnprintf(buf, PAGE_SIZE, "# Route Configuration Cache (port %u)\n", port);
    len += scnprintf(buf + len, PAGE_SIZE - len, "# Format: index hex_data\n");

    for (i = 0; i < BMCAN_ROUTE_CACHED_INDEX && len < PAGE_SIZE - 100; i++)
    {
        if (copy[i].valid)
        {
            len += scnprintf(buf + len, PAGE_SIZE - len, "[%d] ", i);
            for (j = 0; j < BMCAN_ROUTE_ENTRY_SIZE && len < PAGE_SIZE - 10; j++)
            {
                len += scnprintf(buf + len, PAGE_SIZE - len, "%02X", copy[i].data[j]);
            }
            len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
        }
    }

    return len;
}

static ssize_t bmcan_store_route(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *bdev = priv->parent;
    char *kbuf;
    char *p;
    char *nl;
    char *idx_str;
    char *data_str;
    unsigned int idx;
    u8 data[BMCAN_ROUTE_MAX_BYTES];
    size_t data_len = 0;
    int ret;
    unsigned long flags;

    /* Debug log: show received data */
    pr_debug(BMCAN_DRIVER_NAME ": %s route write: count=%zu buf=%.*s\n",
            netdev->name, count, (int)min((size_t)count, (size_t)64), buf);

    kbuf = kstrdup(buf, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    p = strim(kbuf);
    nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    idx_str = strsep(&p, " \t");
    data_str = p;
    if (!idx_str || !data_str)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: invalid format (no index or data)\n",
                netdev->name);
        kfree(kbuf);
        return -EINVAL;
    }

    ret = kstrtouint(idx_str, 0, &idx);
    if (ret)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: invalid index '%s'\n",
                netdev->name, idx_str);
        kfree(kbuf);
        return ret;
    }
    if (bdev->caps.loaded && idx >= bdev->caps.max_route)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: index %u out of range (max %u)\n",
                netdev->name, idx, bdev->caps.max_route - 1);
        kfree(kbuf);
        return -ERANGE;
    }
    if (!bdev->caps.loaded && idx >= BMCAN_ROUTE_MAX_INDEX)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: index %u out of range (max %u)\n",
                netdev->name, idx, BMCAN_ROUTE_MAX_INDEX - 1);
        kfree(kbuf);
        return -ERANGE;
    }

    ret = bmcan_parse_hex_bytes(data_str, data, sizeof(data), &data_len);
    if (ret)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: hex parse failed (%d)\n",
                netdev->name, ret);
        kfree(kbuf);
        return ret;
    }
    if (data_len != BMCAN_ROUTE_MAX_BYTES)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: data length %zu != required %u\n",
                netdev->name, data_len, BMCAN_ROUTE_MAX_BYTES);
        kfree(kbuf);
        return -EINVAL;
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s route: idx=%u len=%zu\n",
             netdev->name, idx, data_len);

    ret = bmcan_proto_set_route(bdev, priv->port, (u16)idx, data, (u16)data_len);
    if (ret < 0)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s route: failed to set entry %u (ret=%d)\n",
                netdev->name, idx, ret);
        kfree(kbuf);
        return ret;
    }

    /* Update cache (if index is within cache range) */
    if (idx < BMCAN_ROUTE_CACHED_INDEX)
    {
        spin_lock_irqsave(&priv->config_cache.lock, flags);
        memcpy(priv->config_cache.route[idx].data, data, BMCAN_ROUTE_ENTRY_SIZE);
        priv->config_cache.route[idx].valid = true;
        priv->config_cache.loaded = true;  /* Mark as loaded since we have valid config in memory */
        spin_unlock_irqrestore(&priv->config_cache.lock, flags);
        pr_debug(BMCAN_DRIVER_NAME ": %s Route entry added: index=%u\n",
                netdev->name, idx);
    }
    else
    {
        pr_debug(BMCAN_DRIVER_NAME ": %s Route entry added to hardware: index=%u (beyond cache range %u)\n",
                netdev->name, idx, BMCAN_ROUTE_CACHED_INDEX);
    }

    kfree(kbuf);
    return count;
}

static DEVICE_ATTR(routes, 0664, bmcan_show_route, bmcan_store_route);

static ssize_t bmcan_store_load_config(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *bdev = priv->parent;
    u16 configmask;
    int ret;

    if (bdev->generation < BMCAN_GEN2_5)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s load not supported on this device generation\n",
                netdev->name);
        return -ENOTSUPP;
    }

    /* Parse hex mask value, default to all */
    if (count >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    {
        if (kstrtou16(buf + 2, 16, &configmask))
            return -EINVAL;
    }
    else
    {
        unsigned long val;
        if (kstrtoul(buf, 0, &val))
            return -EINVAL;
        if (val > 0xFFFF)
            return -EINVAL;
        configmask = val ? (u16)val : BM_CONFIG_ALL_MASK;
    }

    pr_info(BMCAN_DRIVER_NAME ": %s loading config (mask=0x%04X)\n", netdev->name, configmask);

    ret = bmcan_proto_load_config(bdev, priv->port, configmask);
    if (ret < 0)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s load_config failed (ret=%d)\n",
                netdev->name, ret);
        return ret;
    }

    priv->config_cache.loaded = true;
    pr_info(BMCAN_DRIVER_NAME ": %s config loaded successfully\n", netdev->name);
    return count;
}

static ssize_t bmcan_store_save_config(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *bdev = priv->parent;
    u16 configmask;
    int ret;

    if (bdev->generation < BMCAN_GEN2_5)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s save not supported on this device generation\n",
                netdev->name);
        return -ENOTSUPP;
    }

    /* Parse hex mask value, e.g. "0x000F". Default to all if just "1". */
    if (count >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    {
        if (kstrtou16(buf + 2, 16, &configmask))
            return -EINVAL;
    }
    else
    {
        unsigned long val;
        if (kstrtoul(buf, 0, &val))
            return -EINVAL;
        if (val > 0xFFFF)
            return -EINVAL;
        configmask = val ? (u16)val : BM_CONFIG_ALL_MASK;
    }

    pr_info(BMCAN_DRIVER_NAME ": %s saving config (mask=0x%04X)\n", netdev->name, configmask);

    ret = bmcan_proto_save_config(bdev, priv->port, configmask);
    if (ret < 0)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s save_config failed (ret=%d)\n",
                netdev->name, ret);
        return ret;
    }

    pr_info(BMCAN_DRIVER_NAME ": %s config saved successfully\n", netdev->name);
    return count;
}

static ssize_t bmcan_store_clear_config(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(dev);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_dev *bdev = priv->parent;
    u16 configmask;
    unsigned long flags;
    int ret;
    int i;

    if (bdev->generation < BMCAN_GEN2_5)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s clear not supported on this device generation\n",
                netdev->name);
        return -ENOTSUPP;
    }

    /* Parse hex mask value, default to all */
    if (count >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    {
        if (kstrtou16(buf + 2, 16, &configmask))
            return -EINVAL;
    }
    else
    {
        unsigned long val;
        if (kstrtoul(buf, 0, &val))
            return -EINVAL;
        if (val > 0xFFFF)
            return -EINVAL;
        configmask = val ? (u16)val : BM_CONFIG_ALL_MASK;
    }

    pr_info("bmcan: %s clearing config (mask=0x%04X)\n", netdev->name, configmask);

    /* 1. Clear hardware configuration */
    ret = bmcan_proto_clear_config(bdev, priv->port, configmask);
    if (ret < 0)
        return ret;

    /* 2. Clear kernel cache (CRITICAL! ensure sysfs reflects real state) */
    spin_lock_irqsave(&priv->config_cache.lock, flags);

    for (i = 0; i < BMCAN_TXTASK_CACHED_INDEX; i++)
    {
        memset(&priv->config_cache.txtask[i], 0, sizeof(priv->config_cache.txtask[i]));
    }

    for (i = 0; i < BMCAN_ROUTE_CACHED_INDEX; i++)
    {
        memset(&priv->config_cache.route[i], 0, sizeof(priv->config_cache.route[i]));
    }

    priv->config_cache.loaded = false;

    spin_unlock_irqrestore(&priv->config_cache.lock, flags);

    pr_info(BMCAN_DRIVER_NAME ": %s TxTask cleared (all %d entries invalidated)\n",
            netdev->name, BMCAN_TXTASK_CACHED_INDEX);
    pr_info(BMCAN_DRIVER_NAME ": %s Route cleared (all %d entries invalidated)\n",
            netdev->name, BMCAN_ROUTE_CACHED_INDEX);
    pr_info("bmcan: %s clear_config completed successfully (cache cleared)\n", netdev->name);
    return count;
}

static DEVICE_ATTR(load, 0220, NULL, bmcan_store_load_config);
static DEVICE_ATTR(save, 0220, NULL, bmcan_store_save_config);
static DEVICE_ATTR(clear, 0220, NULL, bmcan_store_clear_config);

/* Logging configuration sysfs */
static ssize_t bmcan_show_logging(struct device *d, struct device_attribute *attr, char *buf)
{
    struct net_device *netdev = to_net_dev(d);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_logging_config cfg;
    int ret, i;
    ssize_t len = 0;

    if (priv->parent->generation != BMCAN_GEN2_5)
        return -ENOTSUPP;

    ret = bmcan_proto_get_logging(priv->parent, &cfg);
    if (ret < 0)
        return ret;

    for (i = 0; i < (int)sizeof(cfg); i++)
        len += scnprintf(buf + len, PAGE_SIZE - len, "%02X%s", ((u8 *)&cfg)[i],
                       i + 1 < (int)sizeof(cfg) ? " " : "\n");
    return len;
}

static ssize_t bmcan_store_logging(struct device *d, struct device_attribute *attr,
                                    const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(d);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_logging_config cfg;
    u8 *raw = (u8 *)&cfg;
    int i, ret;
    const char *p = buf;

    if (priv->parent->generation != BMCAN_GEN2_5)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s logging not supported on this device generation\n",
                netdev->name);
        return -ENOTSUPP;
    }

    memset(&cfg, 0, sizeof(cfg));
    for (i = 0; i < (int)sizeof(cfg); i++)
    {
        int hi, lo;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (!*p)
            break;
        hi = hex_to_bin(*p);
        if (hi < 0)
            return -EINVAL;
        p++;
        if (!*p)
            return -EINVAL;
        lo = hex_to_bin(*p);
        if (lo < 0)
            return -EINVAL;
        p++;
        raw[i] = (u8)((hi << 4) | lo);
    }
    if (i < (int)sizeof(cfg))
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s logging: only %d/%zu bytes provided, padding with zeros\n",
                netdev->name, i, sizeof(cfg));
    }

    ret = bmcan_proto_set_logging(priv->parent, &cfg);
    if (ret < 0)
    {
        pr_err(BMCAN_DRIVER_NAME ": %s set logging failed (%d)\n", netdev->name, ret);
        return ret;
    }
    pr_info(BMCAN_DRIVER_NAME ": %s logging config updated\n", netdev->name);
    return count;
}

static DEVICE_ATTR(logging, 0664, bmcan_show_logging, bmcan_store_logging);

/* Replay configuration sysfs */
static ssize_t bmcan_show_replay(struct device *d, struct device_attribute *attr, char *buf)
{
    struct net_device *netdev = to_net_dev(d);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_replay_config cfg;
    int ret, i;
    ssize_t len = 0;

    if (priv->parent->generation != BMCAN_GEN2_5)
        return -ENOTSUPP;

    ret = bmcan_proto_get_replay(priv->parent, &cfg);
    if (ret < 0)
        return ret;

    for (i = 0; i < (int)sizeof(cfg); i++)
        len += scnprintf(buf + len, PAGE_SIZE - len, "%02X%s", ((u8 *)&cfg)[i],
                       i + 1 < (int)sizeof(cfg) ? " " : "\n");
    return len;
}

static ssize_t bmcan_store_replay(struct device *d, struct device_attribute *attr,
                                   const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(d);
    struct bmcan_priv *priv = bmcan_priv_from_netdev(netdev);
    struct bmcan_replay_config cfg;
    u8 *raw = (u8 *)&cfg;
    int i, ret;
    const char *p = buf;

    if (priv->parent->generation != BMCAN_GEN2_5)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s replay not supported on this device generation\n",
                netdev->name);
        return -ENOTSUPP;
    }

    memset(&cfg, 0, sizeof(cfg));
    for (i = 0; i < (int)sizeof(cfg); i++)
    {
        int hi, lo;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (!*p)
            break;
        hi = hex_to_bin(*p);
        if (hi < 0)
            return -EINVAL;
        p++;
        if (!*p)
            return -EINVAL;
        lo = hex_to_bin(*p);
        if (lo < 0)
            return -EINVAL;
        p++;
        raw[i] = (u8)((hi << 4) | lo);
    }
    if (i < (int)sizeof(cfg))
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s replay: only %d/%zu bytes provided, padding with zeros\n",
                netdev->name, i, sizeof(cfg));
    }

    ret = bmcan_proto_set_replay(priv->parent, &cfg);
    if (ret < 0)
    {
        pr_err(BMCAN_DRIVER_NAME ": %s set replay failed (%d)\n", netdev->name, ret);
        return ret;
    }
    pr_info(BMCAN_DRIVER_NAME ": %s replay config updated\n", netdev->name);
    return count;
}

static DEVICE_ATTR(replay, 0664, bmcan_show_replay, bmcan_store_replay);

/* Deferred NAPI scheduling: moves RX softirq off the USB IRQ CPU (CPU0 on
 * RK3588) to spread netif_receive_skb() across available cores.
 * Called from WQ_UNBOUND worker — napi_schedule() triggers softirq there. */
static void bmcan_rx_deliver_workfn(struct work_struct *work)
{
    struct bmcan_priv *priv = container_of(work, struct bmcan_priv, rx_deliver_work);
    if (!netif_running(priv->netdev))
        return;
    napi_schedule(&priv->napi);
}

static int bmcan_napi_poll(struct napi_struct *napi, int budget)
{
    struct bmcan_priv *priv = container_of(napi, struct bmcan_priv, napi);
    struct sk_buff_head localq;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,19,0)
    LIST_HEAD(batch);
#endif
    struct sk_buff *skb;
    int work_done = 0;
    unsigned long flags;

    atomic_set(&bmcan_last_activity, ACTIVITY_NAPI_POLL);
    __skb_queue_head_init(&localq);

    /* Atomically move all pending skbs (must lock: URB completion also
     * touches priv->rxq from hard-IRQ context via skb_queue_splice). */
    spin_lock_irqsave(&priv->rxq.lock, flags);
    skb_queue_splice_init(&priv->rxq, &localq);
    spin_unlock_irqrestore(&priv->rxq.lock, flags);

    /* On 4.19+ the kernel offers netif_receive_skb_list(): deliver the whole
     * batch in one protocol-stack entry, avoiding per-frame overhead that
     * under multi-pair load drove NET_RX softirq starvation on devices with
     * reduced CPU headroom. Older kernels fall back to per-frame calls. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,19,0)
    while (work_done < budget && (skb = __skb_dequeue(&localq)))
    {
        list_add_tail(&skb->list, &batch);
        work_done++;
    }
    if (!list_empty(&batch))
        netif_receive_skb_list(&batch);
#else
    while (work_done < budget && (skb = __skb_dequeue(&localq)))
    {
        netif_receive_skb(skb);
        work_done++;
    }
#endif

    /* Put leftover frames back atomically */
    if (!skb_queue_empty(&localq))
    {
        spin_lock_irqsave(&priv->rxq.lock, flags);
        skb_queue_splice(&localq, &priv->rxq);
        spin_unlock_irqrestore(&priv->rxq.lock, flags);
    }

    if (work_done < budget)
        napi_complete_done(napi, work_done);

    return work_done;
}

void bmcan_netdev_rx(struct bmcan_priv *priv, u32 can_id, u8 data_len, const u8 *data,
                     bool is_ext, bool is_rtr, bool is_fd, bool brs, u64 ts_ns,
                     struct sk_buff_head *port_queues)
{
    struct net_device *netdev = priv->netdev;
    struct sk_buff *skb;

    pr_debug("bmcan: netdev_rx: %s id=0x%x len=%u ext=%d rtr=%d fd=%d\n",
            netdev->name, can_id, data_len, is_ext, is_rtr, is_fd);

    if (bmcan_debug_comm)
        pr_info("bmcan: %s RX: id=0x%x len=%u ext=%d rtr=%d fd=%d brs=%d\n",
                netdev->name, can_id, data_len, is_ext, is_rtr, is_fd, brs);

    if (bmcan_rx_drop)
    {
        netdev->stats.rx_dropped++;
        return;
    }

    /* Token-bucket RX limiter (only evaluated when enabled) */
    if (bmcan_rx_rate_limit)
    {
        unsigned long flags;
        unsigned long now = jiffies;
        unsigned long delta;
        spin_lock_irqsave(&priv->rx_rate_lock, flags);
        delta = now - priv->rx_rate_ts;
        if (delta)
        {
            u64 total = (u64)priv->rx_rate_rem + (u64)delta * bmcan_rx_rate_limit;
            u32 add = (u32)(total / HZ);
            priv->rx_rate_rem = (u32)(total % HZ);
            if (add)
            {
                u32 old_tokens, new_tokens;
                do
                {
                    old_tokens = atomic_read(&priv->rx_rate_tokens);
                    new_tokens = min(old_tokens + add, bmcan_rx_rate_burst);
                } while (atomic_cmpxchg(&priv->rx_rate_tokens, old_tokens, new_tokens) != old_tokens);
            }
            priv->rx_rate_ts = now;
        }
        if (atomic_read(&priv->rx_rate_tokens) == 0)
        {
            spin_unlock_irqrestore(&priv->rx_rate_lock, flags);
            netdev->stats.rx_dropped++;
            return;
        }
        atomic_dec(&priv->rx_rate_tokens);
        spin_unlock_irqrestore(&priv->rx_rate_lock, flags);
    }

#ifdef CANFD_MTU
    /* CAN FD receive */
    if (is_fd)
    {
        struct canfd_frame *cfd;
        skb = alloc_canfd_skb(netdev, &cfd);
        if (!skb)
        {
            netdev->stats.rx_dropped++;
            return;
        }
        cfd->can_id = can_id;
        if (is_ext)
            cfd->can_id |= CAN_EFF_FLAG;
        if (is_rtr)
            cfd->can_id |= CAN_RTR_FLAG;
        cfd->len = data_len;
        cfd->flags = brs ? CANFD_BRS : 0;
        if (!is_rtr)
            memcpy(cfd->data, data, min_t(u8, data_len, 64));
        if (ts_ns)
            skb->tstamp = ns_to_ktime(ts_ns);
        netdev->stats.rx_packets++;
        netdev->stats.rx_bytes += data_len;
        __skb_queue_tail(&port_queues[priv->port], skb);
        return;
    }
#else
    if (is_fd)
    {
        netdev->stats.rx_dropped++;
        return;
    }
#endif

    /* Classic CAN receive */
    {
        struct can_frame *cf;
        skb = alloc_can_skb(netdev, &cf);
        if (!skb)
        {
            netdev->stats.rx_dropped++;
            return;
        }
        cf->can_id = can_id;
        if (is_ext)
            cf->can_id |= CAN_EFF_FLAG;
        if (is_rtr)
            cf->can_id |= CAN_RTR_FLAG;
        cf->can_dlc = data_len;
        if (!is_rtr)
            memcpy(cf->data, data, min_t(u8, data_len, 8));
        if (ts_ns)
            skb->tstamp = ns_to_ktime(ts_ns);
        netdev->stats.rx_packets++;
        netdev->stats.rx_bytes += data_len;
        __skb_queue_tail(&port_queues[priv->port], skb);
    }
}

int bmcan_netdev_register(struct bmcan_dev *dev, u8 port)
{
    struct net_device *netdev;
    struct bmcan_priv *priv;
    int err;

    bmcan_tx_wq_get();
    bmcan_status_wq_get();

    netdev = alloc_candev(sizeof(*priv), BMCAN_TX_ECHO_MAX);
    if (!netdev)
    {
        bmcan_status_wq_put();
        bmcan_tx_wq_put();
        return -ENOMEM;
    }

    priv = bmcan_priv_from_netdev(netdev);
    priv->parent = dev;
    priv->netdev = netdev;
    priv->port = port;
    atomic_set(&priv->tx_busy, 0);
    atomic_set(&priv->tx_active_urbs, 0);
    atomic_set(&priv->tx_ongoing_bytes, 0);
    priv->last_tx_complete_jiffies = 0;
    priv->last_queue_stop_jiffies = 0;
    priv->tx_stall_count = 0;
    priv->stopping = false;
    priv->tx_recovering = false;
    spin_lock_init(&priv->tx_pool_lock);

    /* Pre-allocate TX URB pool */
    {
        int i;
        memset(priv->tx_urbs, 0, sizeof(priv->tx_urbs));
        bitmap_fill(priv->tx_free_bm, BMCAN_NUM_TX_URBS);
        bitmap_fill(priv->echo_free_bm, BMCAN_TX_ECHO_MAX);
        for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
        {
            priv->tx_urbs[i].urb = usb_alloc_urb(0, GFP_KERNEL);
            if (!priv->tx_urbs[i].urb)
            {
                pr_warn(BMCAN_DRIVER_NAME ": tx_urb[%d] alloc failed\n", i);
                clear_bit(i, priv->tx_free_bm);
                continue;
            }
            priv->tx_urbs[i].buf = kmalloc(BMCAN_TX_BUF_SIZE, GFP_KERNEL);
            if (!priv->tx_urbs[i].buf)
            {
                pr_warn(BMCAN_DRIVER_NAME ": tx_buf[%d] alloc failed\n", i);
                usb_free_urb(priv->tx_urbs[i].urb);
                priv->tx_urbs[i].urb = NULL;
                clear_bit(i, priv->tx_free_bm);
                continue;
            }
            priv->tx_urbs[i].idx = i;
            priv->tx_urbs[i].in_use = false;
        }
    }
    priv->tx_batch = NULL;
    INIT_DELAYED_WORK(&priv->tx_flush_work, bmcan_tx_flush_workfn);
    INIT_DELAYED_WORK(&priv->open_work, bmcan_open_workfn);
    INIT_DELAYED_WORK(&priv->status_work, bmcan_status_workfn);
    INIT_WORK(&priv->restart_work, bmcan_restart_workfn);
    INIT_DELAYED_WORK(&priv->tx_recover_work, bmcan_tx_recover_workfn);
    priv->last_status_valid = false;
    atomic_set(&priv->rx_rate_tokens, bmcan_rx_rate_burst);
    priv->rx_rate_rem = 0;
    priv->rx_rate_ts = jiffies;
    spin_lock_init(&priv->rx_rate_lock);
    skb_queue_head_init(&priv->rxq);

    /* Initialize configuration cache */
    spin_lock_init(&priv->config_cache.lock);
    priv->config_cache.loaded = false;
    memset(priv->config_cache.txtask, 0, sizeof(priv->config_cache.txtask));
    memset(priv->config_cache.route, 0, sizeof(priv->config_cache.route));

    bmcan_napi_add(netdev, &priv->napi, bmcan_napi_poll, bmcan_rx_napi_weight);
    INIT_WORK(&priv->rx_deliver_work, bmcan_rx_deliver_workfn);

    netdev->netdev_ops = &bmcan_netdev_ops;
    if (!bmcan_disable_echo)
        netdev->flags |= IFF_ECHO;
    SET_NETDEV_DEV(netdev, &dev->intf->dev);

    priv->can.clock.freq = 40000000;  /* 40MHz - hardware fixed */
    priv->can.bittiming_const = &bmcan_bittiming_const;
    priv->can.do_set_bittiming = bmcan_do_set_bittiming;
    /* Termination resistor support */
    priv->can.termination_const = bmcan_termination_const;
    priv->can.termination_const_cnt = ARRAY_SIZE(bmcan_termination_const);
    priv->can.do_set_termination = bmcan_do_set_termination;
    /* Advertise supported CAN control modes to SocketCAN. */
    priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY;
    priv->can.do_set_mode = bmcan_do_set_mode;
#ifdef CAN_CTRLMODE_ONE_SHOT
    priv->can.ctrlmode_supported |= CAN_CTRLMODE_ONE_SHOT;
#endif
#ifdef CAN_CTRLMODE_PRESUME_ACK
    priv->can.ctrlmode_supported |= CAN_CTRLMODE_PRESUME_ACK;
#endif
#ifdef CANFD_MTU
    /* CAN FD enable + data bitrate hooks when kernel supports it. */
    priv->can.ctrlmode_supported |= CAN_CTRLMODE_FD;
#ifdef CAN_CTRLMODE_FD_NON_ISO
    priv->can.ctrlmode_supported |= CAN_CTRLMODE_FD_NON_ISO;
#endif
    priv->can.data_bittiming_const = &bmcan_data_bittiming_const;  /* Wide range for data bittiming */

    /* Initialize data_bittiming with nominal bittiming values to avoid
     * "incorrect/missing data bit-timing" error during registration check. */
    priv->can.data_bittiming.bitrate = priv->can.bittiming.bitrate;
    priv->can.data_bittiming.sample_point = priv->can.bittiming.sample_point;
    priv->can.data_bittiming.tq = priv->can.bittiming.tq;
    priv->can.data_bittiming.prop_seg = 0;
    priv->can.data_bittiming.phase_seg1 = 0;
    priv->can.data_bittiming.phase_seg2 = 0;
    priv->can.data_bittiming.sjw = 0;
    priv->can.data_bittiming.brp = 0;

    priv->can.do_set_data_bittiming = bmcan_do_set_data_bittiming;
    netdev->mtu = CANFD_MTU;
#else
    netdev->mtu = CAN_MTU;
#endif

#ifdef CAN_CTRLMODE_BERR_REPORTING
    priv->can.ctrlmode_supported |= CAN_CTRLMODE_BERR_REPORTING;
    priv->can.do_get_berr_counter = bmcan_get_berr_counter;
#endif

    netdev->tx_queue_len = 1024;

    err = register_candev(netdev);
    if (err)
    {
        pr_err(BMCAN_DRIVER_NAME ": %s register_candev failed (%d)\n",
               netdev->name, err);
        goto err_register;
    }

    pr_debug(BMCAN_DRIVER_NAME ": %s TxTask cache initialized (max %d entries)\n",
            netdev->name, BMCAN_TXTASK_CACHED_INDEX);
    pr_debug(BMCAN_DRIVER_NAME ": %s Route cache initialized (max %d entries)\n",
            netdev->name, BMCAN_ROUTE_CACHED_INDEX);

    /* Create sysfs files for configuration management.
     * tx_task and route are critical features - fail if they cannot be created. */
    err = device_create_file(&netdev->dev, &dev_attr_txtasks);
    if (err)
    {
        pr_err(BMCAN_DRIVER_NAME ": %s failed to create tx_task sysfs file (%d)\n",
               netdev->name, err);
        goto err_tx_task;
    }

    err = device_create_file(&netdev->dev, &dev_attr_routes);
    if (err)
    {
        pr_err(BMCAN_DRIVER_NAME ": %s failed to create route sysfs file (%d)\n",
               netdev->name, err);
        goto err_route;
    }

    /* load/save/clear config are optional - warn but continue */
    err = device_create_file(&netdev->dev, &dev_attr_load);
    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s failed to create load sysfs file (%d)\n",
                netdev->name, err);

    err = device_create_file(&netdev->dev, &dev_attr_save);
    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s failed to create save sysfs file (%d)\n",
                netdev->name, err);

    err = device_create_file(&netdev->dev, &dev_attr_clear);
    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s failed to create clear sysfs file (%d)\n",
                netdev->name, err);

    err = device_create_file(&netdev->dev, &dev_attr_logging);
    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s failed to create logging sysfs file (%d)\n",
                netdev->name, err);

    err = device_create_file(&netdev->dev, &dev_attr_replay);
    if (err)
        pr_warn(BMCAN_DRIVER_NAME ": %s failed to create replay sysfs file (%d)\n",
                netdev->name, err);

    /* Set default termination to 120 ohm at device init time.
     * User can disable via 'ip link set canX type can termination 0'. */
    priv->can.termination = BMCAN_TERMINATION_120;
    err = bmcan_proto_set_terminal_resistor(dev, port, BMCAN_TERMINATION_120);
    if (err < 0)
    {
        pr_warn(BMCAN_DRIVER_NAME ": %s set default termination %u ohm failed (%d)\n",
                netdev->name, BMCAN_TERMINATION_120, err);
    }
    else
    {
        pr_info(BMCAN_DRIVER_NAME ": %s default termination 120 ohm enabled"
                " (disable: ip link set %s type can termination 0)\n",
                netdev->name, netdev->name);
    }

    dev->ch[port] = priv;
    dev->channels = max_t(int, dev->channels, port + 1);
    return 0;

err_route:
    device_remove_file(&netdev->dev, &dev_attr_txtasks);
err_tx_task:
    unregister_candev(netdev);
err_register:
    /* Cleanup all allocated resources on registration failure */
    {
        int j;
        cancel_delayed_work_sync(&priv->tx_flush_work);
        cancel_delayed_work_sync(&priv->status_work);
        cancel_delayed_work_sync(&priv->tx_recover_work);
        cancel_work_sync(&priv->rx_deliver_work);
        netif_napi_del(&priv->napi);
        for (j = 0; j < BMCAN_NUM_TX_URBS; j++)
        {
            if (priv->tx_urbs[j].urb)
                usb_free_urb(priv->tx_urbs[j].urb);
            kfree(priv->tx_urbs[j].buf);
        }
        free_candev(netdev);
        bmcan_status_wq_put();
        bmcan_tx_wq_put();
        return err;
    }
}

void bmcan_netdev_unregister(struct bmcan_dev *dev, u8 port)
{
    struct bmcan_priv *priv = dev->ch[port];
    int i;
    if (!priv)
        return;
    cancel_delayed_work_sync(&priv->open_work);
    cancel_delayed_work_sync(&priv->status_work);
    cancel_delayed_work_sync(&priv->tx_flush_work);
    cancel_work_sync(&priv->restart_work);
    cancel_delayed_work_sync(&priv->tx_recover_work);

    device_remove_file(&priv->netdev->dev, &dev_attr_replay);
    device_remove_file(&priv->netdev->dev, &dev_attr_logging);
    device_remove_file(&priv->netdev->dev, &dev_attr_clear);
    device_remove_file(&priv->netdev->dev, &dev_attr_save);
    device_remove_file(&priv->netdev->dev, &dev_attr_load);
    device_remove_file(&priv->netdev->dev, &dev_attr_routes);
    device_remove_file(&priv->netdev->dev, &dev_attr_txtasks);

    /* Unregister netdev first — this triggers ndo_stop (bmcan_stop) which:
     * - kills all in-flight TX URBs synchronously (usb_kill_urb)
     * - disables NAPI (napi_disable, waits for in-progress poll)
     * - frees echo skbs via close_candev
     * Must happen BEFORE usb_free_urb to avoid UAF: previously we freed
     * URB structs first, then unregister_candev → bmcan_stop called
     * usb_kill_urb on already-freed URBs. */
    unregister_candev(priv->netdev);

    /* NAPI cleanup after napi_disable (called by bmcan_stop above) */
    cancel_work_sync(&priv->rx_deliver_work);
    netif_napi_del(&priv->napi);

    /* Now safe to free TX URB pool — bmcan_stop already killed them.
     * usb_kill_urb on an already-killed URB is a safe no-op. */
    for (i = 0; i < BMCAN_NUM_TX_URBS; i++)
    {
        if (priv->tx_urbs[i].urb)
        {
            usb_kill_urb(priv->tx_urbs[i].urb);
            usb_free_urb(priv->tx_urbs[i].urb);
        }
        kfree(priv->tx_urbs[i].buf);
    }
    skb_queue_purge(&priv->rxq);
    free_candev(priv->netdev);
    dev->ch[port] = NULL;
    bmcan_status_wq_put();
    bmcan_tx_wq_put();
}
