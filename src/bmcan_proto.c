// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BMCAN USB protocol handling
 *
 * Copyright (C) 2026 Busmust Tech Co.,Ltd
 * SPDX-FileCopyrightText: 2026 Busmust Tech Co.,Ltd
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include "bmcan_core.h"

/*
 * CAN FD DLC to byte length conversion.
 * DLC values 0-8 map directly, 9-15 map to 12/16/20/24/32/48/64.
 */
static const u8 canfd_dlc2len[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
};

/* Reverse lookup: byte length to DLC for TX path */
static const u8 canfd_len2dlc[65] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8,	/* 0-8 */
	9, 9, 9, 9,				/* 9-12 -> DLC 9 (12 bytes) */
	10, 10, 10, 10,			/* 13-16 -> DLC 10 (16 bytes) */
	11, 11, 11, 11,			/* 17-20 -> DLC 11 (20 bytes) */
	12, 12, 12, 12,			/* 21-24 -> DLC 12 (24 bytes) */
	13, 13, 13, 13, 13, 13, 13, 13,	/* 25-32 -> DLC 13 (32 bytes) */
	14, 14, 14, 14, 14, 14, 14, 14,	/* 33-40 -> DLC 14 (48 bytes) */
	14, 14, 14, 14, 14, 14, 14, 14,	/* 41-48 -> DLC 14 (48 bytes) */
	15, 15, 15, 15, 15, 15, 15, 15,	/* 49-56 -> DLC 15 (64 bytes) */
	15, 15, 15, 15, 15, 15, 15, 15	/* 57-64 -> DLC 15 (64 bytes) */
};

/* Helper: convert byte length to DLC (valid for 0-64 bytes) */
static inline u8 len_to_dlc(u8 len)
{
	if (len >= 65)
		return 0; /* Invalid, clamp to 0 */
	return canfd_len2dlc[len];
}

/* Helper: convert DLC to byte length (valid for DLC 0-15) */
static inline u8 dlc_to_len(u8 dlc)
{
	return canfd_dlc2len[dlc & 0x0F];
}

/* USB control transfers for device configuration */
static int bmcan_invalidate_entries(struct bmcan_dev *dev, u8 port,
                                     u8 request, const u8 *buffer, u16 buf_size,
                                     int count, const char *name);

int bmcan_proto_set_mode(struct bmcan_dev *dev, u8 port, u32 mode)
{
    u16 value = (u16)(mode & 0xFFFF);
    int ret;

    pr_debug("bmcan: set_mode: port=%u mode=0x%x value=0x%x\n", port, mode, value);
    ret = bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_MODE),
                                   value, port, NULL, 0);
    pr_debug("bmcan: set_mode: port=%u result=%d\n", port, ret);
    return ret;
}

/* Send firmware bus-off recovery command (0xF5).
 * Only supported by firmware >= 3.1. */
int bmcan_proto_busoff_cmd_recovery(struct bmcan_dev *dev, u8 port)
{
    return bmcan_usb_send_control(dev, BM_BUSOFF_RECOVERY, 0, port, NULL, 0);
}

int bmcan_proto_set_bitrate(struct bmcan_dev *dev, u8 port, u32 bitrate,
                             u32 dbitrate, u16 sample_point, u16 data_sample_point)
{
    struct bm_bitrate br;
    int err;

    pr_debug("bmcan: [TX_CONFIG] port=%u bitrate=%u dbitrate=%u (bps)\n", port, bitrate, dbitrate);

    br.nbitrate = cpu_to_le16((u16)(bitrate / 1000));
    br.dbitrate = cpu_to_le16((u16)(dbitrate / 1000));
    br.nsamplepos = (u8)sample_point;
    br.dsamplepos = (u8)data_sample_point;
    br.clockfreq = 0;
    br.reserved = 0;
    br.nbtr0 = 0;
    br.nbtr1 = 0;
    br.dbtr0 = 0;
    br.dbtr1 = 0;

    pr_debug("bmcan:   nbitrate=%u kbps dbitrate=%u kbps nsamplepos=%u dsamplepos=%u\n",
             le16_to_cpu(br.nbitrate), le16_to_cpu(br.dbitrate),
             br.nsamplepos, br.dsamplepos);

    pr_debug("bmcan: [TX_CONFIG] sending USB control for port=%u\n", port);
    /* BMAPI: BM_Control(handle, BM_CAN_CTRL_WR(BM_CAN_BITRATE), 0, port, &br, sizeof(br))
     * value=0 (unused), index=port (channel ID) */
    err = bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_BITRATE),
                                   0, port, &br, sizeof(br));
    if (err < 0)
    {
        pr_err("bmcan: [TX_ERROR] set_bitrate FAILED: port=%u err=%d\n", port, err);
    }
    else
    {
        pr_debug("bmcan: [TX_SUCCESS] set_bitrate OK: port=%u\n", port);
    }
    return err;
}

int bmcan_proto_get_bitrate(struct bmcan_dev *dev, u8 port,
                             struct bm_bitrate *br)
{
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_RD(BM_CAN_BITRATE),
                                   0, port, br, sizeof(*br));
}

int bmcan_proto_get_status(struct bmcan_dev *dev, u8 port,
                            struct bmcan_can_status *status)
{
    struct bmcan_can_status buf;

    /* Caller may pass NULL to check liveness only — use stack buffer
     * to avoid passing NULL to usb_control_msg (would DMA to address 0). */
    if (!status)
        status = &buf;

    return bmcan_usb_send_control(dev, BM_CAN_CTRL_RD(BM_CAN_STATUS),
                                   0, port, status, sizeof(*status));
}

int bmcan_proto_set_rxfilter(struct bmcan_dev *dev, u8 port, u16 index,
                               const struct bmcan_rx_filter *filter)
{
    /* Device supports exactly 2 RX filters (index 0-1) */
    if (index >= 2)
    {
        pr_warn("bmcan: rxfilter index %u out of range (max 1)\n", index);
        return -EINVAL;
    }

    /* BMAPI: BM_Control(handle, BM_CAN_CTRL_WR(BM_CAN_RXFILTER_TABLE),
     *                     filter_index, port, &filter, sizeof(filter)) */
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_RXFILTER_TABLE),
                                   index, port, (void *)filter,
                                   sizeof(*filter));
}

int bmcan_proto_set_terminal_resistor(struct bmcan_dev *dev, u8 port,
                                       u16 resistor)
{
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_TERMINAL_RESISTOR),
                                   resistor, port, NULL, 0);
}

/* TXTASK configuration */
int bmcan_proto_set_txtask(struct bmcan_dev *dev, u8 port, u16 index,
                            const void *data, u16 size)
{
    if (size > BMCAN_TXTASK_ENTRY_SIZE)
    {
        pr_err("bmcan: txtask data too large (%u > %u)\n",
               size, BMCAN_TXTASK_ENTRY_SIZE);
        return -EINVAL;
    }
    if (dev->caps.loaded && index >= dev->caps.max_txtask)
    {
        pr_warn("bmcan: txtask index %u out of range (max %u)\n",
                index, dev->caps.max_txtask);
        return -EINVAL;
    }
    if (!dev->caps.loaded && index >= 64)
    {
        pr_warn("bmcan: txtask index %u out of range (0-63, caps not loaded)\n", index);
        return -EINVAL;
    }

    /* BMAPI: BM_Control(handle, BM_CAN_CTRL_WR(BM_CAN_TXTASK_TABLE),
     *                     entry_index, port, data, size) */
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_TXTASK_TABLE),
                                   index, port, (void *)data, size);
}

/* Route configuration */
int bmcan_proto_set_route(struct bmcan_dev *dev, u8 port, u16 index,
                           const void *data, u16 size)
{
    if (size > BMCAN_ROUTE_ENTRY_SIZE)
    {
        pr_err("bmcan: route data too large (%u > %u)\n",
               size, BMCAN_ROUTE_ENTRY_SIZE);
        return -EINVAL;
    }
    if (dev->caps.loaded && index >= dev->caps.max_route)
    {
        pr_warn("bmcan: route index %u out of range (max %u)\n",
                index, dev->caps.max_route);
        return -EINVAL;
    }
    if (!dev->caps.loaded && index >= 256)
    {
        pr_warn("bmcan: route index %u out of range (0-255, caps not loaded)\n", index);
        return -EINVAL;
    }

    /* BMAPI: value=entry_index, index=port (same pattern as TXTASK/RXFILTER) */
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_ROUTE_TABLE),
                                   index, port, (void *)data, size);
}

/* Get TXTASK configuration */
int bmcan_proto_get_txtask(struct bmcan_dev *dev, u8 port, u16 index,
                            void *data, u16 size)
{
    /*
     * CORRECT PARAMETER ORDER (from Windows BMAPI):
     * BM_Control(handle, request, entry_index, port, data, size)
     * This means: value=entry_index, index=port
     */
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_RD(BM_CAN_TXTASK_TABLE),
                                   index, port, data, size);
}

/* Get route configuration */
int bmcan_proto_get_route(struct bmcan_dev *dev, u8 port, u16 index,
                           void *data, u16 size)
{
    /*
     * CORRECT PARAMETER ORDER (from Windows BMAPI):
     * BM_Control(handle, request, entry_index, port, data, size)
     * This means: value=entry_index, index=port
     */
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_RD(BM_CAN_ROUTE_TABLE),
                                   index, port, data, size);
}

/*
 * Configuration save/load/clear operations
 *
 * Storage space check behavior:
 * - Gen2 (F012, F112, F122, F142): No storage, not applicable
 * - Gen2.5 (E122, E142): NVM storage, BM_GET_STAT works correctly
 * - Gen3 (0043, 0083, F013, F023, F043): NVM storage, BM_GET_STAT returns
 *   freekb=0 even when storage is available (firmware quirk). Storage check
 *   is skipped for Gen3 — save/load/clear proceeds unconditionally.
 *   Firmware handles out-of-space conditions internally.
 */

/* Check storage availability via BM_GET_STAT (BM_CONTROL query).
 * Gen3 devices return freekb=0 even when storage is available (firmware quirk),
 * so we skip the check for Gen3 and let save/load proceed unconditionally.
 * For other generations, returns -ENOSPC if firmware reports no free storage.
 */
static int bmcan_check_storage_support(struct bmcan_dev *dev, u8 port)
{
    u32 freekb = 0;
    int ret;

    /* Gen3: skip storage check — firmware always reports freekb=0 despite
     * storage being available. Windows host confirms Gen3 save/load works. */
    if (dev->generation == BMCAN_GEN3)
    {
        pr_debug("bmcan: port=%u Gen3 — skipping storage check (known freekb=0 quirk)\n", port);
        return 0;
    }

    ret = bmcan_usb_send_control(dev, BM_GET_STAT,
                                   BM_STAT_FREE_STORAGE_SIZE_KB,
                                   port,
                                   (u8 *)&freekb,
                                   sizeof(freekb));
    if (ret < 0)
    {
        pr_warn("bmcan: port=%u failed to query storage space: %d\n", port, ret);
        return ret;
    }

    if (freekb == 0)
    {
        pr_warn("bmcan: port=%u storage reports no free space\n", port);
        return -ENOSPC;
    }

    pr_info("bmcan: port=%u storage space available: %u KB\n", port, freekb);
    return 0;
}

int bmcan_proto_save_config(struct bmcan_dev *dev, u8 port, u16 configmask)
{
    int ret;
    u32 result;
    struct bmcan_priv *priv;
    unsigned long flags;
    int i;
    u8 txtask_buffer[BMCAN_TXTASK_ENTRY_SIZE];
    u8 route_buffer[BMCAN_ROUTE_ENTRY_SIZE];
    bool is_empty;
    int j;
    struct bmcan_txtask_entry local_txtask[BMCAN_TXTASK_CACHED_INDEX];
    struct bmcan_route_entry local_route[BMCAN_ROUTE_CACHED_INDEX];

    pr_info("bmcan: port=%u saving configuration (gen=%d)\n", port, dev->generation);

    priv = dev->ch[port];
    if (!priv)
    {
        pr_err("bmcan: port=%u cannot find priv for save\n", port);
        return -ENODEV;
    }

    /* Storage space check */
    /* Storage space check: Gen2.5 firmware may report free=0 for NVM operations
     * even though config save/load works fine. Warn but continue.
     */
    ret = bmcan_check_storage_support(dev, port);
    if (ret < 0)
        return ret;

    /* Step 1: Read current txtask/route data and cache in kernel */
    for (i = 0; i < BMCAN_TXTASK_CACHED_INDEX; i++)
    {
        memset(txtask_buffer, 0, BMCAN_TXTASK_ENTRY_SIZE);
        local_txtask[i].valid = false;
        ret = bmcan_proto_get_txtask(dev, port, i, txtask_buffer, BMCAN_TXTASK_ENTRY_SIZE);
        if (ret > 0)
        {
            is_empty = true;
            for (j = 0; j < BMCAN_TXTASK_ENTRY_SIZE; j++)
            {
                if (txtask_buffer[j] != 0x00 && txtask_buffer[j] != 0xFF)
                {
                    is_empty = false;
                    break;
                }
            }
            if (!is_empty)
            {
                memcpy(local_txtask[i].data, txtask_buffer, BMCAN_TXTASK_ENTRY_SIZE);
                local_txtask[i].valid = true;
                if (i == 0)
                {
                    pr_info("bmcan: port=%u saved txtask[0]: type=%u ID=0x%03X payload[64-67]=%02X %02X %02X %02X\n",
                           port, txtask_buffer[0],
                           (txtask_buffer[13] << 8) | txtask_buffer[12],
                           txtask_buffer[64], txtask_buffer[65],
                           txtask_buffer[66], txtask_buffer[67]);
                }
            }
        }
    }

    for (i = 0; i < BMCAN_ROUTE_CACHED_INDEX; i++)
    {
        memset(route_buffer, 0, BMCAN_ROUTE_ENTRY_SIZE);
        local_route[i].valid = false;
        ret = bmcan_proto_get_route(dev, port, i, route_buffer, BMCAN_ROUTE_ENTRY_SIZE);
        if (ret > 0)
        {
            is_empty = true;
            for (j = 0; j < BMCAN_ROUTE_ENTRY_SIZE; j++)
            {
                if (route_buffer[j] != 0x00 && route_buffer[j] != 0xFF)
                {
                    is_empty = false;
                    break;
                }
            }
            if (!is_empty)
            {
                memcpy(local_route[i].data, route_buffer, BMCAN_ROUTE_ENTRY_SIZE);
                local_route[i].valid = true;
            }
        }
    }

    /* Step 2: Send save command to firmware */
    pr_info("bmcan: port=%u saving config (configmask=0x%X)\n", port, configmask);
    ret = bmcan_usb_send_control(dev, BM_SAVE_CONFIG, configmask, port,
                                   (u8 *)&result, sizeof(result));
    if (ret < 0)
    {
        pr_err("bmcan: port=%u save_config USB command failed (%d)\n", port, ret);
        return ret;
    }

    pr_info("bmcan: port=%u save_config returned result=%u\n", port, result);
    if (result != 0)
    {
        pr_warn("bmcan: port=%u save_config returned non-zero result=%u\n", port, result);
    }

    /* Step 3: Wait for hardware to write to NVM.
     * Poll with short sleeps so we exit early on device disconnect. */
    pr_info("bmcan: port=%u waiting for hardware NVM write...\n", port);
    {
        int wait_ms;
        for (wait_ms = 0; wait_ms < BMCAN_TIMEOUT_LONG; wait_ms += 100)
        {
            if (!atomic_read(&dev->present))
                return -ENODEV;
            msleep(100);
        }
    }

    /* Step 4: Update kernel cache with current config state */
    spin_lock_irqsave(&priv->config_cache.lock, flags);
    memcpy(priv->config_cache.txtask, local_txtask, sizeof(local_txtask));
    memcpy(priv->config_cache.route, local_route, sizeof(local_route));
    priv->config_cache.loaded = true;
    spin_unlock_irqrestore(&priv->config_cache.lock, flags);

    pr_info("bmcan: port=%u configuration saved and cached successfully\n", port);
    return 0;
}

int bmcan_proto_load_config(struct bmcan_dev *dev, u8 port, u16 configmask)
{
    struct bmcan_priv *priv;
    unsigned long flags;
    int ret, i, j;
    u32 load_result;
    u8 txtask_buffer[BMCAN_TXTASK_ENTRY_SIZE];
    u8 route_buffer[BMCAN_ROUTE_ENTRY_SIZE];
    struct bmcan_txtask_entry local_txtask[BMCAN_TXTASK_CACHED_INDEX];
    struct bmcan_route_entry local_route[BMCAN_ROUTE_CACHED_INDEX];
    bool is_empty;

    pr_info("bmcan: port=%u loading configuration (gen=%d, mask=0x%X)\n",
            port, dev->generation, configmask);

    priv = dev->ch[port];
    if (!priv)
    {
        pr_err("bmcan: port=%u cannot find priv for load\n", port);
        return -ENODEV;
    }

    ret = bmcan_check_storage_support(dev, port);
    if (ret < 0)
        return ret;

    /* Step 1: Send BM_LOAD_CONFIG to firmware.
     * Firmware reads NVM, restores runtime config, and applies it
     * (including txtask payload, mode, bitrate, etc.). */
    ret = bmcan_usb_send_control(dev, BM_LOAD_CONFIG, configmask, port,
                                   (u8 *)&load_result, sizeof(load_result));
    if (ret < 0)
    {
        pr_err("bmcan: port=%u load_config USB command failed (%d)\n", port, ret);
        return ret;
    }
    pr_info("bmcan: port=%u BM_LOAD_CONFIG result=%u\n", port, load_result);

    msleep(BMCAN_TIMEOUT_SHORT);

    /* Step 2: Read back restored config from firmware and update driver cache.
     * This ensures the driver cache reflects the actual post-load state. */
    for (i = 0; i < BMCAN_TXTASK_CACHED_INDEX; i++)
    {
        memset(txtask_buffer, 0, BMCAN_TXTASK_ENTRY_SIZE);
        local_txtask[i].valid = false;
        ret = bmcan_proto_get_txtask(dev, port, i, txtask_buffer, BMCAN_TXTASK_ENTRY_SIZE);
        if (ret > 0)
        {
            is_empty = true;
            for (j = 0; j < BMCAN_TXTASK_ENTRY_SIZE; j++)
            {
                if (txtask_buffer[j] != 0x00 && txtask_buffer[j] != 0xFF)
                {
                    is_empty = false;
                    break;
                }
            }
            if (!is_empty)
            {
                memcpy(local_txtask[i].data, txtask_buffer, BMCAN_TXTASK_ENTRY_SIZE);
                local_txtask[i].valid = true;
            }
        }
    }

    for (i = 0; i < BMCAN_ROUTE_CACHED_INDEX; i++)
    {
        memset(route_buffer, 0, BMCAN_ROUTE_ENTRY_SIZE);
        local_route[i].valid = false;
        ret = bmcan_proto_get_route(dev, port, i, route_buffer, BMCAN_ROUTE_ENTRY_SIZE);
        if (ret > 0)
        {
            is_empty = true;
            for (j = 0; j < BMCAN_ROUTE_ENTRY_SIZE; j++)
            {
                if (route_buffer[j] != 0x00 && route_buffer[j] != 0xFF)
                {
                    is_empty = false;
                    break;
                }
            }
            if (!is_empty)
            {
                memcpy(local_route[i].data, route_buffer, BMCAN_ROUTE_ENTRY_SIZE);
                local_route[i].valid = true;
            }
        }
    }

    spin_lock_irqsave(&priv->config_cache.lock, flags);
    memcpy(priv->config_cache.txtask, local_txtask, sizeof(local_txtask));
    memcpy(priv->config_cache.route, local_route, sizeof(local_route));
    priv->config_cache.loaded = true;
    spin_unlock_irqrestore(&priv->config_cache.lock, flags);

    pr_info("bmcan: port=%u config loaded and cache updated\n", port);
    return 0;
}

/*
 * bmcan_invalidate_entries - Helper to invalidate configuration entries
 * @dev: Device
 * @port: Port number
 * @request: USB request type (BM_CAN_CTRL_WR)
 * @buffer: Zeroed buffer to send
 * @buf_size: Buffer size
 * @count: Number of entries
 * @name: Entry name for logging
 *
 * Returns: Number of failures
 */
static int bmcan_invalidate_entries(struct bmcan_dev *dev, u8 port,
                                     u8 request, const u8 *buffer, u16 buf_size,
                                     int count, const char *name)
{
    int i, ret, fail = 0;

    for (i = 0; i < count; i++)
    {
        ret = bmcan_usb_send_control(dev, request, i, port,
                                       (void *)buffer, buf_size);
        if (ret < 0)
        {
            pr_warn("bmcan: port=%u failed to invalidate %s[%d] (%d)\n",
                    port, name, i, ret);
            fail++;
        }
        else
        {
            pr_debug("bmcan: port=%u %s[%d] invalidated\n", port, name, i);
        }
    }

    pr_info("bmcan: port=%u invalidated %s: %d/%d ok\n",
            port, name, count - fail, count);
    return fail;
}

/*
 * bmcan_proto_invalidate_config - Invalidate runtime table only (NOT hardware storage)
 *
 * This function ONLY invalidates the runtime txtask/route tables by sending
 * INVALID entries to the hardware. It does NOT send BM_CLEAR_CONFIG command,
 * so hardware non-volatile storage is preserved.
 *
 * Use cases:
 * - Device up/down (auto_clear_config)
 * - Device open/close (auto_clear_config)
 *
 * For API-initiated clear (should also clear hardware storage), use
 * bmcan_proto_clear_config() instead.
 *
 * Returns: 0 on success (allow partial failures), -EIO if majority failed
 */
int bmcan_proto_invalidate_config(struct bmcan_dev *dev, u8 port)
{
    u8 txtask_buffer[BMCAN_TXTASK_ENTRY_SIZE];
    u8 route_buffer[BMCAN_ROUTE_ENTRY_SIZE];
    int total, failed;

    pr_info("bmcan: port=%u invalidating runtime configuration (txtask/route)\n", port);

    /* Initialize buffers with INVALID type (0 = BM_TXTASK_INVALID) */
    memset(txtask_buffer, 0, BMCAN_TXTASK_ENTRY_SIZE);
    memset(route_buffer, 0, BMCAN_ROUTE_ENTRY_SIZE);

    /* Invalidate txtask entries */
    failed = bmcan_invalidate_entries(dev, port, BM_CAN_CTRL_WR(BM_CAN_TXTASK_TABLE),
                                       txtask_buffer, BMCAN_TXTASK_ENTRY_SIZE,
                                       BMCAN_TXTASK_CACHED_INDEX, "txtask");

    /* Invalidate route entries */
    failed += bmcan_invalidate_entries(dev, port, BM_CAN_CTRL_WR(BM_CAN_ROUTE_TABLE),
                                        route_buffer, BMCAN_ROUTE_ENTRY_SIZE,
                                        BMCAN_ROUTE_CACHED_INDEX, "route");

    /* Allow partial failures (invalidate is "best effort" operation) */
    total = BMCAN_TXTASK_CACHED_INDEX + BMCAN_ROUTE_CACHED_INDEX;
    if (failed > 0)
    {
        pr_warn("bmcan: port=%u invalidate had %d/%d failures\n", port, failed, total);
        if (failed > total / 2)
        {
            pr_err("bmcan: port=%u invalidate majority failed, returning error\n", port);
            return -EIO;
        }
    }

    pr_info("bmcan: port=%u runtime configuration invalidated (hardware storage preserved)\n", port);
    return 0;
}

int bmcan_proto_clear_config(struct bmcan_dev *dev, u8 port, u16 configmask)
{
    int ret;
    u32 clear_result = 0;

    /* Check device status before clearing config
     * Only proceed if device is ready (powered on)
     * Skip if device is not ready to avoid "invalidate majority failed" errors
     * Reference: Windows BMAPI behavior - clear_config only after device is ready
     */
    ret = bmcan_proto_get_status(dev, port, NULL);
    if (ret < 0)
    {
        pr_debug("bmcan: port=%u device not ready, skipping clear_config (%d)\n", port, ret);
        return 0;  /* Not an error - device just not ready yet */
    }

    /* Check device storage availability */
    ret = bmcan_check_storage_support(dev, port);
    if (ret < 0)
        return ret;

    pr_info("bmcan: port=%u clearing hardware storage (txtask/route)\n", port);

    /*
     * Send BM_CLEAR_CONFIG command to hardware
     * This clears the non-volatile storage only
     * Does NOT invalidate runtime table
     *
     * Reference: Windows BMAPI BM_ClearConfig implementation:
     * BM_Control(handle, BM_CLEAR_CONFIG, configmask, port, &result, sizeof(result))
     *
     * configmask: bit flags for which config types to clear
     * - (1 << 9) = txtask configuration
     * - (1 << 10) = route configuration
     */
    pr_info("bmcan: port=%u sending BM_CLEAR_CONFIG command (configmask=0x%X)\n", port, configmask);

    ret = bmcan_usb_send_control(dev, BM_CLEAR_CONFIG, configmask, port,
                                   (u8 *)&clear_result, sizeof(clear_result));
    if (ret < 0)
    {
        pr_err("bmcan: port=%u BM_CLEAR_CONFIG USB command failed with error %d\n", port, ret);
        return ret;
    }

    pr_info("bmcan: port=%u BM_CLEAR_CONFIG returned result=%u\n", port, clear_result);

    if (clear_result != 0)
    {
        pr_warn("bmcan: port=%u BM_CLEAR_CONFIG returned non-zero result=%u (may indicate issue)\n",
                port, clear_result);
    }

    /* Give hardware time to process clear operation */
    usleep_range(10000, 20000);

    pr_info("bmcan: port=%u hardware storage cleared successfully\n", port);
    return 0;
}

int bmcan_proto_set_logging(struct bmcan_dev *dev, const struct bmcan_logging_config *cfg)
{
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_LOGGING_CONFIG),
                                   0, 0, (void *)cfg, sizeof(*cfg));
}

int bmcan_proto_get_logging(struct bmcan_dev *dev, struct bmcan_logging_config *cfg)
{
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_RD(BM_CAN_LOGGING_CONFIG),
                                   0, 0, cfg, sizeof(*cfg));
}

int bmcan_proto_set_replay(struct bmcan_dev *dev, const struct bmcan_replay_config *cfg)
{
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_WR(BM_CAN_REPLAY_CONFIG),
                                   0, 0, (void *)cfg, sizeof(*cfg));
}

int bmcan_proto_get_replay(struct bmcan_dev *dev, struct bmcan_replay_config *cfg)
{
    return bmcan_usb_send_control(dev, BM_CAN_CTRL_RD(BM_CAN_REPLAY_CONFIG),
                                   0, 0, cfg, sizeof(*cfg));
}

/* Build CAN message for transmission */
int bmcan_proto_build_tx(struct bmcan_priv *priv, u32 can_id, bool is_ext,
                         bool is_rtr, bool is_fd, bool brs,
                         u8 data_len, const u8 *data,
                         u8 *out, size_t *out_len)
{
    struct bm_data *msg;
    __le32 id_le;
    __le32 ctrl_le = 0;
    u8 *payload;
    u32 sid, eid;

    if (!out || !out_len || !data)
        return -EINVAL;

    if (is_fd && data_len > 64)
        return -EINVAL;
    if (!is_fd && data_len > 8)
        return -EINVAL;

    msg = (struct bm_data *)out;

    /* Build header matching BMAPI SDK convention:
     * type = BM_CAN_FD_DATA (2), dchn = port, schn = 0xF (ANY) */
    msg->header.raw = cpu_to_le16(
        BM_DATA_TYPE_CAN_FD |
        ((priv->port & 0xF) << 8) |   /* DCHN: destination channel */
        (0xF << 12)                    /* SCHN: BM_MESSAGE_ANY_CHANNEL */
    );

    pr_debug_ratelimited("bmcan: %s build_tx: port=%u header=0x%04x id=0x%x len=%u is_fd=%d\n",
            priv->netdev->name, priv->port,
            le16_to_cpu(msg->header.raw), can_id, data_len, is_fd);

    /*
     * Convert CAN ID to BM protocol format:
     * - Standard ID (11-bit): SID = id, EID = 0
     * - Extended ID (29-bit): SID = id[28:18], EID = id[17:0]
     * BM protocol stores: bits 0-10 = SID, bits 11-28 = EID
     */
    if (is_ext)
    {
        /* Extended frame: split 29-bit ID into SID (high 11-bit) and EID (low 18-bit) */
        sid = (can_id >> 18) & 0x7FFU;  /* bits 18-28 -> SID */
        eid = can_id & 0x3FFFFU;         /* bits 0-17 -> EID */
        id_le = cpu_to_le32((eid << 11) | sid);
        pr_debug_ratelimited("bmcan: %s ext_id split: can_id=0x%x sid=0x%x eid=0x%x packed=0x%x\n",
                priv->netdev->name, can_id, sid, eid, (eid << 11) | sid);
    }
    else
    {
        /* Standard frame: SID only (11-bit), EID = 0 */
        sid = can_id & 0x7FFU;
        id_le = cpu_to_le32(sid);
        pr_debug_ratelimited("bmcan: %s std_id: sid=0x%x\n", priv->netdev->name, sid);
    }

    /* Build ctrl field - convert byte length to DLC for FD frames */
    if (is_fd)
    {
	ctrl_le |= (__le32)len_to_dlc(data_len);
    }
    else
    {
	ctrl_le |= (__le32)(data_len & BM_TXCTRL_DLC_MASK);
    }
    if (is_ext)
        ctrl_le |= (__le32)BM_TXCTRL_IDE;
    if (is_rtr)
        ctrl_le |= (__le32)BM_TXCTRL_RTR;
    if (is_fd)
    {
        ctrl_le |= (__le32)BM_TXCTRL_FDF;
        if (brs)
            ctrl_le |= (__le32)BM_TXCTRL_BRS;
    }

    /* Variable-length payload matching BMAPI SDK BM_WriteCanMessage behavior:
     * length = sizeof(id) + sizeof(ctrl) + data_bytes = 8 + data_len
     * Rounded up to 4-byte alignment. Only DLC=15 (64 bytes) reaches 72. */
    {
        size_t payload_len = 8 + data_len;  /* id(4) + ctrl(4) + data */
        size_t aligned_payload = (payload_len + 3) & ~(size_t)3;

        msg->length = cpu_to_le16(aligned_payload);
        msg->timestamp = 0;
        payload = msg->payload;

        /* Clear only the bytes we use (id + ctrl + data + padding) */
        memset(payload, 0, aligned_payload);

        /* Copy ID and ctrl */
        memcpy(payload, &id_le, 4);
        memcpy(payload + 4, &ctrl_le, 4);

        /* Copy data */
        if (data_len > 0 && !is_rtr)
            memcpy(payload + 8, data, data_len);

        /* Total: header(8) + variable payload, already 4-byte aligned */
        *out_len = 8 + aligned_payload;
    }

    return 0;
}

/* Extend 32-bit firmware timestamp to 64-bit nanoseconds with wrap detection.
 * Used only for Gen2/Gen2.5 devices which lack 64-bit tail timestamps.
 * Firmware clock is device-global (shared across all CAN ports).
 * Wrap is detected when now < last AND the backward gap exceeds half the range. */
static u64 bmcan_extend_timestamp(struct bmcan_dev *dev, u32 ts_now_us)
{
    unsigned long flags;
    u64 ts_ns;

    spin_lock_irqsave(&dev->ts_lock, flags);

    if (!dev->ts_initialized)
    {
        dev->ts_last_us = ts_now_us;
        dev->ts_initialized = true;
        ts_ns = (u64)ts_now_us * 1000ULL;
        spin_unlock_irqrestore(&dev->ts_lock, flags);
        return ts_ns;
    }

    /* Detect forward wrap: now < last AND the backward gap > half the range */
    if (ts_now_us < dev->ts_last_us &&
        (dev->ts_last_us - ts_now_us) > (U32_MAX >> 1))
        dev->ts_wrap_count++;

    dev->ts_last_us = ts_now_us;
    ts_ns = ((u64)dev->ts_wrap_count << 32 | (u64)ts_now_us) * 1000ULL;

    spin_unlock_irqrestore(&dev->ts_lock, flags);
    return ts_ns;
}

/* Extract 64-bit UTC timestamp from data tail (Gen3+ devices).
 * Returns true if tail was present and valid, false otherwise.
 * Tail offset follows BMAPI convention: align4(8 + dlc) within payload. */
static bool bmcan_extract_tail_timestamp(const struct bm_data *msg,
                                          u16 payload_len, u16 actual_msg_len,
                                          u64 *ts_ns)
{
    const u8 *tail_ptr;
    u32 utctsl, utctsh;
    u64 hwts;

    /* Tail is present when header.flags == 1 and payload has room for it */
    if (payload_len < actual_msg_len + BM_DATA_TAIL_SIZE)
        return false;

    /* utctsl at offset 8, utctsh at offset 12 within the tail */
    tail_ptr = &msg->payload[actual_msg_len];
    utctsl = le32_to_cpu(*(const __le32 *)(tail_ptr + 8));
    utctsh = le32_to_cpu(*(const __le32 *)(tail_ptr + 12));
    hwts = ((u64)utctsh << 32) | (u64)utctsl;

    /* Firmware reports UTC microseconds; convert to nanoseconds */
    *ts_ns = hwts * 1000ULL;
    return true;
}

/* Parse a single BM data message and deliver CAN frame if applicable */
static int bmcan_parse_one_msg(struct bmcan_dev *dev, const struct bm_data *msg,
                                struct sk_buff_head *port_queues)
{
    const struct bm_can_msg *cm;
    struct bmcan_priv *priv;
    u16 header_val = le16_to_cpu(msg->header.raw);
    u16 msg_type = BM_HEADER_TYPE(header_val);
    u16 payload_len = le16_to_cpu(msg->length);
    u32 can_id;
    u8 dlc;
    u32 ctrl;
    bool is_ext, is_rtr, is_fd, brs;
    const u8 *data_ptr;
    u8 port;
    u32 timestamp;
    u64 ts_ns;

    /* Type field is a bitmask: CAN_FD=0x02, ACK=0x08.
     * Actual CAN data has type 0x02 (RX from bus) or 0x0A (TX echo/ACK with CAN data).
     * Skip anything that doesn't carry CAN data. */
    pr_debug("bmcan: parse_one_msg: header=0x%04x type=%u plen=%u\n",
            header_val, msg_type, payload_len);
    if (!(msg_type & BM_DATA_TYPE_CAN_FD))
        return 0;

    /* ACK-only without CAN data (type=0x08) - skip */
    if (msg_type == BM_DATA_TYPE_ACK)
        return 0;

    /* Validate payload: need at least id(4) + ctrl(4) = 8 bytes */
    if (payload_len < 8)
        return 0;

    cm = (const struct bm_can_msg *)msg->payload;

    /* Extract ctrl fields */
    ctrl = le32_to_cpu(cm->ctrl);
    dlc = ctrl & BM_TXCTRL_DLC_MASK;
    is_ext = (ctrl & BM_TXCTRL_IDE) != 0;
    is_rtr = (ctrl & BM_TXCTRL_RTR) != 0;
    is_fd = (ctrl & BM_TXCTRL_FDF) != 0;
    brs = (ctrl & BM_TXCTRL_BRS) != 0;

    /* Convert ID from BM protocol format */
    {
        u32 id_raw = le32_to_cpu(cm->id);
        u32 sid = id_raw & 0x7FFU;
        u32 eid = (id_raw >> 11) & 0x3FFFFU;
        can_id = is_ext ? ((sid << 18) | eid) : sid;
    }

    /* Convert DLC to byte length */
    if (is_rtr)
    {
        dlc = 0;
    }
    else if (is_fd)
    {
        dlc = dlc_to_len(dlc);
    }
    else if (dlc > 8)
    {
        /* Firmware may omit FDF bit for CAN FD frames (e.g. offline replay).
         * DLC > 8 in classic CAN is invalid; treat as CAN FD. */
        is_fd = true;
        dlc = dlc_to_len(dlc);
    }

    /* Determine port from header routing (needed for DLC warning below) */
    {
        u16 dchn = BM_HEADER_DCHN(header_val);
        u16 schn = BM_HEADER_SCHN(header_val);
        port = (dchn == 15) ? schn : dchn;
    }

    /* Point directly to payload data (no copy).
     * Validate payload_len covers the declared DLC to prevent reading
     * beyond the actual message boundary (firmware bug / USB corruption). */
    data_ptr = cm->payload;
    if (!is_rtr && dlc > 64)
        dlc = 64;
    {
        /* payload_len includes tail if header.flags == 1 */
        u16 data_plen = payload_len;
        u16 avail;
        if (header_val & BM_HEADER_FLAGS_MASK)
            data_plen = (data_plen >= BM_DATA_TAIL_SIZE) ?
                        (u16)(data_plen - BM_DATA_TAIL_SIZE) : 0;
        avail = (data_plen >= 8) ? (u16)(data_plen - 8) : 0;
        if (dlc > avail)
        {
            pr_warn_ratelimited("bmcan: DLC %u expects %u bytes but payload has only %u (plen=%u port=%u)\n",
                                ctrl & BM_TXCTRL_DLC_MASK, dlc, avail, payload_len, port);
            dlc = avail;
        }
    }

    /* Filter ACK (TX echo) before touching timestamp state —
     * ACK frames must not pollute the 32-bit wrap tracker. */
    if (msg_type & BM_DATA_TYPE_ACK)
        return 0;

    /* Resolve hardware timestamp:
     * Gen3+ with tail (header.flags == 1): use 64-bit UTC from tail, no wrap issue.
     * Gen2/Gen2.5: use 32-bit local timestamp with wrap extension. */
    if (dev->generation >= BMCAN_GEN3 &&
        (header_val & BM_HEADER_FLAGS_MASK) &&
        bmcan_extract_tail_timestamp(msg, payload_len,
                                      (u16)((8 + dlc + 3) & ~3U), &ts_ns))
    {
        /* 64-bit UTC timestamp from tail — already in nanoseconds */
    }
    else
    {
        timestamp = le32_to_cpu(msg->timestamp);
        ts_ns = bmcan_extend_timestamp(dev, timestamp);
    }

    /* Validate port */
    if (port >= dev->channels || !dev->ch[port])
        return 0;

    priv = dev->ch[port];

    /* Discard loopback echo frames during bus-off recovery */
    if (READ_ONCE(priv->loopback_recovery_active))
        return 0;

    /* Deliver to netdev - pass data pointer directly, no intermediate copy */
    bmcan_netdev_rx(priv, can_id, dlc, data_ptr, is_ext, is_rtr, is_fd, brs, ts_ns,
                    port_queues);

    return 0;
}

/* Parse received data from device - handles multiple messages per URB */
int bmcan_proto_parse_rx(struct bmcan_dev *dev, const u8 *buf, size_t len,
                          struct sk_buff_head *port_queues)
{
    const struct bm_data *msg;
    size_t offset = 0;
    int count = 0;

    if (!dev || !buf || len < BM_DATA_HEADER_SIZE)
        return 0;

    pr_debug("bmcan: parse_rx: buf_len=%zu\n", len);

    /* Loop through all BM messages in the URB buffer */
    while (offset + BM_DATA_HEADER_SIZE <= len)
    {
        u16 payload_len;
        size_t msg_total;

        msg = (const struct bm_data *)(buf + offset);
        payload_len = le16_to_cpu(msg->length);

        pr_debug("bmcan: msg[%d] offset=%zu header=0x%04x type=%u payload_len=%u\n",
                 count, offset, le16_to_cpu(msg->header.raw),
                 BM_HEADER_TYPE(le16_to_cpu(msg->header.raw)), payload_len);

        msg_total = BM_DATA_HEADER_SIZE + payload_len;
        if (msg_total < BM_DATA_HEADER_SIZE || offset + msg_total > len)
        {
            pr_debug("bmcan: truncated msg at offset=%zu\n", offset);
            break;
        }

        bmcan_parse_one_msg(dev, msg, port_queues);

        offset += msg_total;
        count++;

        /* Align to 4-byte boundary as BM protocol may pad */
        if (offset % 4)
            offset += 4 - (offset % 4);
    }

    return count;
}
