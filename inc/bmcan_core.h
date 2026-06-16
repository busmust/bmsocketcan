/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BM USB-CAN FD SocketCAN driver — shared definitions.
 *
 * Copyright (C) 2026 Busmust Tech Co.,Ltd
 * SPDX-FileCopyrightText: 2026 Busmust Tech Co.,Ltd
 */
#ifndef BMCAN_CORE_H
#define BMCAN_CORE_H

#include <linux/types.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/version.h>

/* usb_fill_bulk_urb() takes 7 parameters in all released kernels (4.19 – 6.12+).
 * A gfp_t parameter was briefly added then removed during the 6.8 -rc cycle,
 * but no released kernel ever shipped with the 8-argument version. */
#define bmcan_fill_bulk_urb(urb, dev, pipe, buf, len, cb, ctx) \
    usb_fill_bulk_urb(urb, dev, pipe, buf, len, cb, ctx)

/* ---- Kernel API compatibility (4.19 – 6.12+) ---- *
 * Each wrapper isolates a known API change so the rest of the driver
 * calls a single, stable interface regardless of kernel version.
 * Manual override: define the macro on the build command line to
 * force a particular path, e.g.  make BMCAN_ECHO_HAS_LEN=1  */

/* can_put/get/free_echo_skb gained a frame_len parameter in 5.12.
 * Ubuntu 22.04 (5.15) and all newer kernels use the 4-arg / 3-arg forms.
 * Kernels < 5.12 (4.19, 5.4, 5.10) use the older 3-arg / 2-arg forms. */
#ifndef BMCAN_ECHO_HAS_LEN
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
#define BMCAN_ECHO_HAS_LEN 1
#else
#define BMCAN_ECHO_HAS_LEN 0
#endif
#endif

/* netif_napi_add() lost its weight parameter in 6.1.
 * Before 6.1: netif_napi_add(dev, napi, poll_fn, weight)
 * 6.1+:       netif_napi_add(dev, napi, poll_fn)            */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,1,0)
#define bmcan_napi_add(ndev, napi, poll_fn, weight) \
    netif_napi_add(ndev, napi, poll_fn)
#else
#define bmcan_napi_add(ndev, napi, poll_fn, weight) \
    netif_napi_add(ndev, napi, poll_fn, weight)
#endif

#define BMCAN_DRIVER_NAME "bmcan"
#define BMCAN_DEFAULT_BITRATE 500000U
#define BMCAN_DEFAULT_DBITRATE 2000000U

/* Timeout definitions (milliseconds) */
#define BMCAN_TIMEOUT_SHORT    100   /* Short timeout - fast operations */
#define BMCAN_TIMEOUT_NORMAL   1000  /* Normal timeout */
#define BMCAN_TIMEOUT_LONG     3000  /* Long timeout - storage operations */

/* Termination resistor values (ohms) */
#define BMCAN_TERMINATION_NONE 0     /* Termination disabled */
#define BMCAN_TERMINATION_120  120   /* 120 ohm termination */

/* USB endpoints used by BM devices (from BM protocol definitions) */
#define BMCAN_USB_EP_IN  0x81U
#define BMCAN_USB_EP_OUT 0x01U
#define BMCAN_USB_EP_CMD 0x00U

/* BM USB protocol sizes */
#define BM_DATA_HEADER_SIZE 8U
#define BM_DATA_TAIL_SIZE 16U
#define BM_DATA_PAYLOAD_MAX_SIZE (72U + BM_DATA_TAIL_SIZE)
#define BM_RX_PACKET_SIZE 4096U

/* Multi-URB RX optimization for high frame rate support */
#define BMCAN_NUM_RX_URBS 8      /* 8 URBs — reduced for RK3568/dwc3 stability */
#define BMCAN_RX_URB_SIZE (16 * 1024) /* 16KB per URB — reduced for embedded USB host */

/* Multi-URB TX optimization for high frame rate support */
#define BMCAN_NUM_TX_URBS 16     /* Number of concurrent TX URBs */
#define BMCAN_TX_MAX_FRAME (BM_DATA_HEADER_SIZE + sizeof(struct bm_can_msg))

/* BM USB control definitions (from bm_usb_private_def.h) */
#define BM_CAN_CTRL_WR(type) (0xC0 | (type))
#define BM_CAN_CTRL_RD(type) (0xD0 | (type))
#define BM_CAN_MODE 0x0U
#define BM_CAN_STATUS 0x1U
#define BM_CAN_BITRATE 0x2U
#define BM_CAN_TERMINAL_RESISTOR 0x3U
#define BM_CAN_RXFILTER_TABLE 0x8U
#define BM_CAN_TXTASK_TABLE 0x9U
#define BM_CAN_ROUTE_TABLE 0xAU
#define BM_CAN_LOGGING_CONFIG 0xBU
#define BM_CAN_REPLAY_CONFIG 0xCU
#define BM_LOAD_CONFIG 0xF8U
#define BM_SAVE_CONFIG 0xF9U
#define BM_CLEAR_CONFIG 0xFAU
#define BM_GET_STAT 0xF4U
#define BM_GET_DEVICE_VERSION          0xF1U  /* Read 4-byte firmware version */
#define BM_BUSOFF_RECOVERY             0xF5U  /* Firmware bus-off recovery command */
#define BM_SET_DEVICE_PTP_MODE 0xFDU
#define BM_STAT_TOTAL_STORAGE_SIZE_KB 7U
#define BM_STAT_FREE_STORAGE_SIZE_KB 8U
/* Device capability queries (BM_STAT_CAP range, see bm_usb_def.h) */
#define BM_STAT_MAX_TXTASK   0x42U
#define BM_STAT_MAX_ROUTE    0x46U
/* Device feature support flags (BM_STAT_SUPPORT range) */
#define BM_STAT_SUPPORT_OFFLINE  0x60U
#define BM_STAT_SUPPORT_ROUTE    0x61U
#define BM_STAT_SUPPORT_LOGGING  0x62U
#define BM_STAT_SUPPORT_REPLAY   0x63U
/* Mode values match device protocol (see bm_usb_def.h). */
#define BM_CAN_CONFIGURATION_MODE 0x04U
#define BM_CAN_NORMAL_MODE 0x00U
#define BM_CAN_INTERNAL_LOOPBACK_MODE 0x02U
#define BM_CAN_LISTEN_ONLY_MODE 0x03U
#define BM_CAN_CLASSIC_MODE 0x06U
#define BM_CAN_NON_ISO_MODE 0x08U
#define BM_CAN_NON_AUTORETX_MODE 0x10U
#define BM_CAN_NOACK_MODE 0x20U
#define BM_CAN_DISABLE_AUTO_BUSOFF_RECOVERY 0x40U

/* Firmware version threshold for bus-off recovery command support.
 * Firmware >= 3.1 supports the F5 bus-off recovery command.
 * Older firmware requires loopback + dummy frame recovery. */
#define BMCAN_FW_BUSOFF_CMD_VERSION    0x03010000UL
#define BMCAN_BUSOFF_DUMMY_COUNT       256

/* Data types carried in BM USB frames. */
#define BM_DATA_TYPE_CAN_FD 0x02U
#define BM_DATA_TYPE_ACK    0x08U

/* RX filter type IDs (subset used by this driver). */
#define BM_RXFILTER_INVALID 0U
#define BM_RXFILTER_BASIC   1U

/* Configuration type IDs for save/load/clear config commands. */
#define BM_CAN_TXTASK_CONFIG  9U   /* Save/load txtask configuration */
#define BM_CAN_ROUTE_CONFIG  10U  /* Save/load route configuration */

/* Configuration masks (bit flags for BM_SAVE/LOAD/CLEAR_CONFIG commands).
 * Each bit = (1 << configid) where configid matches firmware BM_TableTypeTypeDef.
 * Source: BMAPI SDK python-can bmapi.py */
#define BM_CONFIG_MODE_MASK              (1U << 0)    /* 0x0001 BM_CAN_MODE */
#define BM_CONFIG_BITRATE_MASK           (1U << 2)    /* 0x0004 BM_CAN_BITRATE */
#define BM_CONFIG_TERM_MASK              (1U << 3)    /* 0x0008 BM_CAN_TERMINAL_RESISTOR */
#define BM_CONFIG_RXFILTER_MASK          (1U << 8)    /* 0x0100 BM_CAN_RXFILTER_TABLE */
#define BM_CONFIG_TXTASK_MASK            (1U << 9)    /* 0x0200 BM_CAN_TXTASK_TABLE */
#define BM_CONFIG_ROUTE_MASK             (1U << 10)   /* 0x0400 BM_CAN_ROUTE_TABLE */
#define BM_CONFIG_LOGGING_MASK           (1U << 11)   /* 0x0800 BM_CAN_LOGGING_CONFIG */
#define BM_CONFIG_REPLAY_MASK            (1U << 12)   /* 0x1000 BM_CAN_REPLAY_CONFIG */
#define BM_CONFIG_ALL_MASK               0xFFFFU
#define BM_CONFIG_OFFLINE_REPLAY_MASK    (BM_CONFIG_MODE_MASK | BM_CONFIG_BITRATE_MASK | \
                                          BM_CONFIG_TERM_MASK | BM_CONFIG_REPLAY_MASK)  /* 0x100D */
#define BM_CONFIG_OFFLINE_LOGGING_MASK   (BM_CONFIG_MODE_MASK | BM_CONFIG_BITRATE_MASK | \
                                          BM_CONFIG_TERM_MASK | BM_CONFIG_LOGGING_MASK)  /* 0x080D */

/* CAN message flags (subset used by this driver). */
#define BM_CAN_MESSAGE_FLAGS_IDE 0x01U
#define BM_CAN_MESSAGE_FLAGS_RTR 0x02U

#define BM_HEADER_TYPE_MASK 0x000F
#define BM_HEADER_FLAGS_MASK 0x0010
#define BM_HEADER_GROUP_MASK 0x00E0
#define BM_HEADER_DCHN_MASK  0x0F00
#define BM_HEADER_SCHN_MASK  0xF000

#define BM_HEADER_TYPE(h)   ((h) & BM_HEADER_TYPE_MASK)
#define BM_HEADER_FLAGS(h)  (((h) & BM_HEADER_FLAGS_MASK) >> 4)
#define BM_HEADER_GROUP(h)  (((h) & BM_HEADER_GROUP_MASK) >> 5)
#define BM_HEADER_DCHN(h)   (((h) & BM_HEADER_DCHN_MASK) >> 8)
#define BM_HEADER_SCHN(h)   (((h) & BM_HEADER_SCHN_MASK) >> 12)

/* BM data header: type + routing in a packed 16-bit field. */
struct bm_data_header
{
    __le16 raw;
} __packed;

/* BM data envelope: header + length + timestamp + payload. */
struct bm_data
{
    struct bm_data_header header;
    __le16 length;      /* payload length (tail included if flags=1) */
    __le32 timestamp;
    u8 payload[BM_DATA_PAYLOAD_MAX_SIZE];
} __packed;

/* BM CAN message encoding (packed 32-bit fields) */
#define BM_TXCTRL_DLC_MASK 0x0000000FU
#define BM_TXCTRL_IDE      0x00000010U
#define BM_TXCTRL_RTR      0x00000020U
#define BM_TXCTRL_BRS      0x00000040U
#define BM_TXCTRL_FDF      0x00000080U
#define BM_TXCTRL_ESI      0x00000100U
#define BM_TXCTRL_SEQ_SHIFT 9
#define BM_TXCTRL_ECHO     0x00020000U

/* BM CAN message payload (classic/FD). */
struct bm_can_msg
{
    __le32 id;   /* SID/EID packed in 32-bit
                  * Standard ID: SID in bits 0-10, EID=0
                  * Extended ID: SID in bits 0-10, EID in bits 11-28
                  * Conversion: ext_id = (SID << 18) | EID */
    __le32 ctrl; /* TX/RX ctrl fields packed */
    u8 payload[64];
} __packed;

/* Nominal/data bitrate parameters in kbps. */
struct bm_bitrate
{
    __le16 nbitrate;  /* kbps */
    __le16 dbitrate;  /* kbps */
    u8 nsamplepos;
    u8 dsamplepos;
    u8 clockfreq;
    u8 reserved;
    u8 nbtr0;
    u8 nbtr1;
    u8 dbtr0;
    u8 dbtr1;
} __packed;

/* Device RX filter item (BM_RxFilterTypeDef). */
struct bmcan_rx_filter
{
    u8 type;
    u8 unused;
    u8 flags_mask;
    u8 flags_value;
    u8 reserved[4];
    __le32 id_mask;
    __le32 id_value;
    u8 payload_mask[8];
    u8 payload_value[8];
} __packed;

/* CAN channel status from device (see BM_CanStatusInfoTypeDef). */
struct bmcan_can_status
{
    u8 txbo;
    u8 reserved;
    u8 txbp;
    u8 rxbp;
    u8 txwarn;
    u8 rxwarn;
    u8 tec;
    u8 rec;
} __packed;

/* Storage mode/direction/format values (match bm_usb_def.h). */
#define BMCAN_STORAGE_DISABLED   0
#define BMCAN_STORAGE_ALWAYS_ON  1
#define BMCAN_STORAGE_TRIGGERED  2

#define BMCAN_STORAGE_DIRECTION_NONE 0
#define BMCAN_STORAGE_DIRECTION_RX   1
#define BMCAN_STORAGE_DIRECTION_TX   2
#define BMCAN_STORAGE_DIRECTION_ALL  3

#define BMCAN_STORAGE_PATH_FIXED 0
#define BMCAN_STORAGE_PATH_INDEX 1
#define BMCAN_STORAGE_PATH_TIME  2

/* Logging/replay config struct sizes. */
#define BMCAN_LOGGING_CONFIG_SIZE 96
#define BMCAN_REPLAY_CONFIG_SIZE  88

struct bmcan_path_spec
{
    u8 mode;
    u8 arg;
    char format[30];
} __packed;

struct bmcan_event_trigger
{
    __le16 channels;
    __le16 reserved;
    __le16 flags_mask;
    __le16 flags_value;
    __le32 id_mask;
    __le32 id_value;
} __packed;

struct bmcan_logging_config
{
    u8 version;
    u8 mode;
    u8 format;
    u8 reserved;
    __le16 channels;
    u8 direction;
    u8 padding[9];
    struct bmcan_path_spec path;
    struct bmcan_event_trigger starttrigger;
    struct bmcan_event_trigger stoptrigger;
    struct
    {
        u8 createNewFileOnStart;
        u8 overwriteOldFileOnFull;
        __le16 nfiles;
        __le32 nmessagesPerFile;
        __le32 nbytesPerFile;
        __le32 nsecondsPerFile;
    } segmentation;
} __packed;

struct bmcan_replay_config
{
    u8 version;
    u8 mode;
    u8 format;
    u8 reserved;
    __le16 channels;
    u8 direction;
    u8 cyclic;
    u8 padding[8];
    struct bmcan_path_spec path;
    struct bmcan_event_trigger starttrigger;
    struct bmcan_event_trigger stoptrigger;
    struct
    {
        __le16 msgdelay;
        __le16 sessiondelay;
        __le16 cycledelay;
        u8 forceZeroTimestampOnFirstMsg;
        u8 reserved[1];
    } timing;
} __packed;

/* Per-device TX limits (from BMAPI SDK, varies by generation).
 * deviceBufferSize: device TXQ capacity (flow control limit).
 * maxPacketSize: max bytes per USB bulk OUT transfer. */
struct bmcan_tx_limits
{
    u32 device_buf_size;
    u32 max_packet_size;
};

/* Device capability limits (queried from firmware via BM_GET_STAT).
 * Queried on first port open, cached for subsequent use. */
struct bmcan_dev_caps
{
    u32 max_txtask;    /* Max TX task entries (e.g. 64) */
    u32 max_route;     /* Max route entries (e.g. 64) */
    bool loaded;       /* true after first successful query */
};

struct bmcan_priv;

/* Device generation based on Product ID.
 * PID encoding (F/E-prefix): 3rd digit = channel count, 4th digit = version (2 or 3).
 * 0043/0083 use a different PID scheme (Gen3 HPM platform). */
enum bmcan_generation
{
    BMCAN_GEN2 = 2,      /* F012, F112, F122, F142 - no storage, no logging/replay */
    BMCAN_GEN2_5 = 3,    /* E122, E142 - NVM storage, logging/replay supported */
    BMCAN_GEN3 = 4,      /* 0043, 0083, F013, F023, F043 - HPM platform */
};

/* RX URB context for O(1) index lookup in completion callback */
struct bmcan_rx_urb_ctx
{
    struct bmcan_dev *dev;
    int idx;
    bool submitted;   /* true while URB is in flight */
};

struct bmcan_dev
{
    struct usb_device *udev;
    struct usb_interface *intf;
    u8 ep_in;
    u8 ep_out;
    u8 ep_cmd;
    /* Multi-URB RX for high frame rate */
    struct urb *rx_urbs[BMCAN_NUM_RX_URBS];
    struct bmcan_rx_urb_ctx *rx_urb_ctx[BMCAN_NUM_RX_URBS];
    u8 *rx_urbs_buf;          /* Shared coherent buffer for all URBs */
    size_t rx_urb_size;       /* Size per URB (BMCAN_RX_URB_SIZE) */
    dma_addr_t rx_urbs_dma;   /* DMA address of shared buffer */
    atomic_t rx_active_urbs;  /* Number of active URBs */
    bool rx_shutting_down;    /* True while kill_rx is in progress */
    struct delayed_work rx_resubmit_work;
    unsigned long rx_last_jiffies;  /* jiffies of last RX URB completion (WRITE_ONCE only) */
    spinlock_t ts_lock;       /* protects ts_last_us, ts_wrap_count, ts_initialized */
    u32 ts_last_us;           /* last raw firmware timestamp (microseconds) */
    u32 ts_wrap_count;        /* number of 2^32 us wraps detected */
    bool ts_initialized;      /* first RX timestamp recorded */
    spinlock_t state_lock;
    struct mutex open_lock;      /* Serializes open_count + submit/kill_rx */
    atomic_t ctrl_inflight;      /* Counts in-flight USB control transfers */
    atomic_t present;
    atomic_t open_count;
    int channels;
    enum bmcan_generation generation;  /* Device generation */
    u32 fw_version;  /* (major<<24)|(minor<<16)|(rev<<8)|build, 0=unknown */
    struct bmcan_tx_limits tx_limits;  /* TX limits per generation */
    struct bmcan_dev_caps caps;        /* Device capability limits (queried at probe) */
    struct bmcan_priv *ch[8];
};

/* Configuration cache size limits */
#define BMCAN_TXTASK_CACHED_INDEX 4  /* Cache first 4 txtask entries */
#define BMCAN_ROUTE_CACHED_INDEX 4   /* Cache first 4 route entries */
#define BMCAN_TXTASK_ENTRY_SIZE 128
#define BMCAN_ROUTE_ENTRY_SIZE 16

/* Cached txtask entry */
struct bmcan_txtask_entry
{
    u8 data[BMCAN_TXTASK_ENTRY_SIZE];
    bool valid;
};

/* Cached route entry */
struct bmcan_route_entry
{
    u8 data[BMCAN_ROUTE_ENTRY_SIZE];
    bool valid;
};

/* Device configuration cache */
struct bmcan_config_cache
{
    struct bmcan_txtask_entry txtask[BMCAN_TXTASK_CACHED_INDEX];
    struct bmcan_route_entry route[BMCAN_ROUTE_CACHED_INDEX];
    bool loaded;  /* true if cache is synchronized with hardware */
    spinlock_t lock;  /* Protects cache access */
};

/* TX multi-frame batching: pack multiple CAN frames into one USB transfer.
 * Frame format matches BMAPI SDK: each frame padded to 4-byte alignment.
 * Batch byte limit from device generation (see struct bmcan_tx_limits). */
#define BMCAN_TX_BUF_SIZE 12288    /* Max USB bulk OUT buffer per URB (Gen3=12000 + margin) */
#define BMCAN_TX_MAX_BATCH 256     /* Max frames per USB transfer */
#define BMCAN_TX_ECHO_MAX 1024     /* Max concurrent echo skb slots */

struct bmcan_tx_urb
{
    struct urb *urb;
    u8 *buf;
    size_t len;
    int echo_slots[BMCAN_TX_MAX_BATCH]; /* echo skb indices per frame */
    u32 echo_lens[BMCAN_TX_MAX_BATCH];
    int echo_count;                         /* frames in this batch */
    int idx;            /* Pool index */
    bool in_use;
    struct bmcan_priv *priv;
};

struct bmcan_priv
{
    struct can_priv can;
    struct bmcan_dev *parent;
    struct net_device *netdev;
    struct napi_struct napi;
    struct sk_buff_head rxq;
    struct work_struct rx_deliver_work;
    atomic_t rx_rate_tokens;
    u32 rx_rate_rem;
    unsigned long rx_rate_ts;
    spinlock_t rx_rate_lock;
    u8 port;
    atomic_t tx_busy;
    /* Multi-URB TX pool for high frame rate */
    struct bmcan_tx_urb tx_urbs[BMCAN_NUM_TX_URBS];
    atomic_t tx_active_urbs;
    spinlock_t tx_pool_lock;  /* Protects tx_urbs pool + tx_free_bitmap */
    unsigned long tx_free_bm[BITS_TO_LONGS(BMCAN_NUM_TX_URBS)];
    atomic_t tx_ongoing_bytes; /* Bytes submitted but not yet completed (flow control) */
    /* TX batch accumulator */
    struct bmcan_tx_urb *tx_batch;       /* current unsent batch URB (NULL if idle) */
    struct delayed_work tx_flush_work;   /* flush partial batch after timeout */
    unsigned long last_tx_complete_jiffies; /* timestamp of last tx_complete */
    unsigned long last_queue_stop_jiffies;  /* timestamp of last netif_stop_queue */
    unsigned int tx_stall_count;            /* number of stall recoveries */
    unsigned int diag_status_count;         /* status poll counter for periodic diag */
    unsigned long echo_free_bm[BITS_TO_LONGS(BMCAN_TX_ECHO_MAX)]; /* echo slot availability */
    /* Deferred and periodic work items */
    struct delayed_work open_work;
    struct delayed_work status_work;
    struct work_struct restart_work;  /* Deferred bus-off restart (avoids softirq deadlock) */
    struct delayed_work tx_recover_work; /* Retry TX recovery after bounded unlink timeout */
    struct bmcan_can_status last_status;
    bool last_status_valid;
    bool stopping;       /* True during bmcan_stop — prevents tx_complete from waking queue */
    bool tx_recovering;  /* True while TX URBs are being unlinked/drained */
    bool loopback_recovery_active;  /* Discard RX echo during loopback recovery */
    u32 operating_mode;  /* Current CAN operating mode for bus-off recovery */
    struct bmcan_config_cache config_cache;  /* Hardware configuration cache */
};

/* Global debug flag (defined in bmcan_usb.c) */
extern bool bmcan_debug_comm;

/* Crash forensics: last-activity tracker (defined in bmcan_usb.c) */
extern atomic_t bmcan_last_activity;
#define ACTIVITY_RX_URB_COMPLETE  1
#define ACTIVITY_RX_RESUBMIT      2
#define ACTIVITY_TX_COMPLETE      3
#define ACTIVITY_TX_FLUSH         4
#define ACTIVITY_CTRL_MSG         5
#define ACTIVITY_STATUS_WORK      6
#define ACTIVITY_OPEN             7
#define ACTIVITY_CLOSE            8
#define ACTIVITY_NAPI_POLL        9
/* Fine-grained lockup probe (diagnostic, safe: atomic_set in any context) */
#define ACTIVITY_XMIT_ENTER       10
#define ACTIVITY_XMIT_LOCKED      11
#define ACTIVITY_XMIT_FLUSHING    12
#define ACTIVITY_TX_COMP_UNLOCK   13
#define ACTIVITY_TX_COMP_WAKE     14
#define ACTIVITY_RX_DELIVER       15

/* RX queue limit (defined in bmcan_netdev.c) */
extern unsigned int bmcan_rx_queue_limit;

/* USB helpers */
int bmcan_usb_send_control(struct bmcan_dev *dev, u8 request, u16 value, u16 index, void *data, u16 size);
int bmcan_usb_submit_rx(struct bmcan_dev *dev);
void bmcan_usb_kill_rx(struct bmcan_dev *dev);
enum bmcan_generation bmcan_get_generation(u16 pid);

/* Protocol helpers */
int bmcan_proto_build_tx(struct bmcan_priv *priv, u32 can_id, bool is_ext, bool is_rtr,
                         bool is_fd, bool brs, u8 data_len, const u8 *data,
                         u8 *out, size_t *out_len);
int bmcan_proto_parse_rx(struct bmcan_dev *dev, const u8 *buf, size_t len,
                          struct sk_buff_head *port_queues);
int bmcan_proto_set_bitrate(struct bmcan_dev *dev, u8 port, u32 bitrate, u32 dbitrate,
                          u16 sample_point, u16 data_sample_point);
int bmcan_proto_get_bitrate(struct bmcan_dev *dev, u8 port, struct bm_bitrate *br);
int bmcan_proto_get_status(struct bmcan_dev *dev, u8 port, struct bmcan_can_status *status);
int bmcan_proto_set_rxfilter(struct bmcan_dev *dev, u8 port, u16 index, const struct bmcan_rx_filter *filter);
int bmcan_proto_set_mode(struct bmcan_dev *dev, u8 port, u32 mode);
int bmcan_proto_busoff_cmd_recovery(struct bmcan_dev *dev, u8 port);
int bmcan_proto_set_terminal_resistor(struct bmcan_dev *dev, u8 port, u16 resistor);
int bmcan_proto_set_txtask(struct bmcan_dev *dev, u8 port, u16 index, const void *data, u16 size);
int bmcan_proto_set_route(struct bmcan_dev *dev, u8 port, u16 index, const void *data, u16 size);
int bmcan_proto_get_txtask(struct bmcan_dev *dev, u8 port, u16 index, void *data, u16 size);
int bmcan_proto_get_route(struct bmcan_dev *dev, u8 port, u16 index, void *data, u16 size);
int bmcan_proto_invalidate_config(struct bmcan_dev *dev, u8 port);  /* Only invalidate runtime table */
int bmcan_proto_load_config(struct bmcan_dev *dev, u8 port, u16 configmask);
int bmcan_proto_save_config(struct bmcan_dev *dev, u8 port, u16 configmask);
int bmcan_proto_clear_config(struct bmcan_dev *dev, u8 port, u16 configmask);
int bmcan_proto_set_logging(struct bmcan_dev *dev, const struct bmcan_logging_config *cfg);
int bmcan_proto_get_logging(struct bmcan_dev *dev, struct bmcan_logging_config *cfg);
int bmcan_proto_set_replay(struct bmcan_dev *dev, const struct bmcan_replay_config *cfg);
int bmcan_proto_get_replay(struct bmcan_dev *dev, struct bmcan_replay_config *cfg);

/* Netdev RX delivery */
void bmcan_netdev_rx(struct bmcan_priv *priv, u32 can_id, u8 data_len, const u8 *data,
                     bool is_ext, bool is_rtr, bool is_fd, bool brs, u64 ts_ns,
                     struct sk_buff_head *port_queues);
int bmcan_netdev_register(struct bmcan_dev *dev, u8 port);
void bmcan_netdev_unregister(struct bmcan_dev *dev, u8 port);

#endif
