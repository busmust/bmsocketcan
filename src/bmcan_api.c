// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BM USB-CAN FD userspace configuration tool.
 *
 * Copyright (C) 2026 Busmust Tech Co.,Ltd
 * SPDX-FileCopyrightText: 2026 Busmust Tech Co.,Ltd
 */
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bm_usb_def.h"
#include "bm_usb_router_def.h"

// Device PID definitions
// Gen2 devices (F-prefix, 4th digit = 2)
#define BMCAN_PID_F012 0xF012
#define BMCAN_PID_F112 0xF112
#define BMCAN_PID_F122 0xF122
#define BMCAN_PID_F142 0xF142

// Gen2.5 devices (E-prefix, has NVM storage)
#define BMCAN_PID_E122 0xE122
#define BMCAN_PID_E142 0xE142

// Gen3 devices (4th digit = 3, HPM platform)
#define BMCAN_PID_F013 0xF013
#define BMCAN_PID_F023 0xF023
#define BMCAN_PID_F043 0xF043
#define BMCAN_PID_0043 0x0043
#define BMCAN_PID_0083 0x0083

static void usage_txtasks(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [txtasks] --type <fixed|incdata|incid|randomdata|randomid> --id <hex> [options]\n"
            "  Common options:\n"
            "    --ext --fd --brs --rtr\n"
            "    --payload <hex>     (payload template, 0..64 bytes)\n"
            "    --length <n>        (payload length if no payload, 0..64)\n"
            "    --cycle <ms> --delay <ms> --rounds <n> --messages <n>\n"
            "    --index <0..63> --device <canX> --apply\n"
            "  INCDATA options: --startbit <n> --nbits <n> --format <intel|moto> --min <n> --max <n> --step <n>\n"
            "  INCID options:   --min <n> --max <n> --step <n>\n"
            "  RANDOMDATA:      --startbit <n> --nbits <n> --format <intel|moto> --min <n> --max <n> --seed <n>\n"
            "  RANDOMID:        --min <n> --max <n> --seed <n>\n",
            prog);
}

static void usage_invalidate(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [invalidate] --device <canX> [options]\n"
            "  Options:\n"
            "    --all              invalidate all (txtasks, routes, logging, replay)\n"
            "    --txtasks          invalidate txtasks entries (alias: --txtask)\n"
            "    --route            invalidate route entries\n"
            "    --logging          invalidate logging (stop recording)\n"
            "    --replay            invalidate replay (stop playback)\n"
            "    --tx-range a-b      index range for txtasks (default 0-63)\n"
            "    --route-range a-b   index range for routes (default 0-63)\n"
            "\n"
            "  Description:\n"
            "    Invalidate hardware features by writing invalid data.\n"
            "    - txtasks: stops periodic transmission\n"
            "    - route: disables frame forwarding\n"
            "    - logging: stops CAN frame recording\n"
            "    - replay: stops CAN frame playback\n"
            "\n"
            "  Note: Only Gen2.5 devices (E122, E142) support logging/replay.\n",
            prog);
}

static void usage_routes(const char *prog)
{
        fprintf(stderr,
            "Usage: %s routes --route-type <unicast|broadcast|idmap|flagsmap|idflagsmap|e2epass|e2efail|num> [options]\n"
            "  Options:\n"
            "    --source <0..15> --target <n>\n"
            "    --flags-mask <hex> --flags-value <hex>\n"
            "    --id-mask <hex> --id-value <hex>\n"
            "    --index <n> --device <canX> --apply\n",
            prog);
}

static void usage_logging(const char *prog)
{
    fprintf(stderr,
            "Usage: %s logging [options]\n"
            "  Options:\n"
            "    --mode <disabled|always|trigger> --format <bbd|pcap|log|asc|blf>\n"
            "    --channels <mask> --direction <rx|tx|all|none>\n"
            "    --path-mode <fixed|index|time> --path-format <string> --path-arg <n>\n"
            "    --start-channels <mask> --start-flags-mask <hex> --start-flags-value <hex> --start-id-mask <hex> --start-id-value <hex>\n"
            "    --stop-channels <mask>  --stop-flags-mask <hex>  --stop-flags-value <hex>  --stop-id-mask <hex>  --stop-id-value <hex>\n"
            "    --seg-create-new <0|1> --seg-overwrite <0|1> --seg-nfiles <n>\n"
            "    --seg-nmessages <n> --seg-nbytes <n> --seg-nseconds <n>\n",
            prog);
}

static void usage_replay(const char *prog)
{
    fprintf(stderr,
            "Usage: %s replay [options]\n"
            "  Options:\n"
            "    --mode <disabled|always|trigger> --format <bbd|pcap|log|asc|blf>\n"
            "    --channels <mask> --direction <rx|tx|all|none> --cyclic <0|1>\n"
            "    --path-mode <fixed|index|time> --path-format <string> --path-arg <n>\n"
            "    --start-channels <mask> --start-flags-mask <hex> --start-flags-value <hex> --start-id-mask <hex> --start-id-value <hex>\n"
            "    --stop-channels <mask>  --stop-flags-mask <hex>  --stop-flags-value <hex>  --stop-id-mask <hex>  --stop-id-value <hex>\n"
            "    --msgdelay <ms> --sessiondelay <ms> --cycledelay <ms> --force-zero-ts <0|1>\n",
            prog);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *s, uint8_t *out, size_t max, size_t *out_len)
{
    size_t len = 0;
    while (*s)
    {
        while (*s && (isspace((unsigned char)*s) || *s == ':' || *s == ','))
            s++;
        if (!*s)
            break;
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]))
            return -EINVAL;
        if (len >= max)
            return -E2BIG;
        out[len++] = (uint8_t)((hex_val(s[0]) << 4) | hex_val(s[1]));
        s += 2;
    }
    *out_len = len;
    return 0;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0')
        return -EINVAL;
    *out = (uint32_t)v;
    return 0;
}

static int parse_endian_format(const char *s, uint8_t *out)
{
    if (!s)
        return -EINVAL;
    if (!strcmp(s, "intel"))
    {
        *out = 0x80;
        return 0;
    }
    if (!strcmp(s, "moto") || !strcmp(s, "motorola"))
    {
        *out = 0x00;
        return 0;
    }
    return -EINVAL;
}

static int parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFFFF)
        return -EINVAL;
    *out = (uint16_t)v;
    return 0;
}

static int parse_u8(const char *s, uint8_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFF)
        return -EINVAL;
    *out = (uint8_t)v;
    return 0;
}

static int parse_bool(const char *s, uint8_t *out)
{
    if (!s)
        return -EINVAL;
    if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "yes"))
    {
        *out = 1;
        return 0;
    }
    if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "no"))
    {
        *out = 0;
        return 0;
    }
    return -EINVAL;
}

static int parse_storage_mode(const char *s, uint8_t *out)
{
    if (!s) return -EINVAL;
    if (!strcmp(s, "disabled") || !strcmp(s, "off")) { *out = BM_STORAGE_DISABLED; return 0; }
    if (!strcmp(s, "always")) { *out = BM_STORAGE_ALWAYS_ON; return 0; }
    if (!strcmp(s, "trigger")) { *out = BM_STORAGE_TRIGGERED; return 0; }
    return -EINVAL;
}

static int parse_storage_format(const char *s, uint8_t *out)
{
    if (!s) return -EINVAL;
    if (!strcmp(s, "bbd")) { *out = BM_STORAGE_BBD_FORMAT; return 0; }
    if (!strcmp(s, "pcap")) { *out = BM_STORAGE_PCAP_FORMAT; return 0; }
    if (!strcmp(s, "log")) { *out = BM_STORAGE_LOG_FORMAT; return 0; }
    if (!strcmp(s, "asc")) { *out = BM_STORAGE_ASC_FORMAT; return 0; }
    if (!strcmp(s, "blf")) { *out = BM_STORAGE_BLF_FORMAT; return 0; }
    return -EINVAL;
}

static int parse_storage_direction(const char *s, uint8_t *out)
{
    if (!s) return -EINVAL;
    if (!strcmp(s, "none")) { *out = BM_STORAGE_DIRECTION_NONE; return 0; }
    if (!strcmp(s, "rx")) { *out = BM_STORAGE_DIRECTION_RX; return 0; }
    if (!strcmp(s, "tx")) { *out = BM_STORAGE_DIRECTION_TX; return 0; }
    if (!strcmp(s, "all")) { *out = BM_STORAGE_DIRECTION_ALL; return 0; }
    return -EINVAL;
}

static int parse_path_mode(const char *s, uint8_t *out)
{
    if (!s) return -EINVAL;
    if (!strcmp(s, "fixed")) { *out = BM_STORAGE_FIXED_PATH; return 0; }
    if (!strcmp(s, "index")) { *out = BM_STORAGE_INDEX_PATH; return 0; }
    if (!strcmp(s, "time")) { *out = BM_STORAGE_TIME_PATH; return 0; }
    return -EINVAL;
}

static int parse_route_type(const char *s, uint8_t *out)
{
    uint32_t v = 0;
    if (!s) return -EINVAL;
    if (!parse_u32(s, &v))
    {
        *out = (uint8_t)v;
        return 0;
    }
    if (!strcmp(s, "unicast")) { *out = BM_ROUTE_UNICAST; return 0; }
    if (!strcmp(s, "broadcast")) { *out = BM_ROUTE_BROADCAST; return 0; }
    if (!strcmp(s, "idmap")) { *out = BM_ROUTE_ID_MAP; return 0; }
    if (!strcmp(s, "flagsmap")) { *out = BM_ROUTE_FLAGS_MAP; return 0; }
    if (!strcmp(s, "idflagsmap")) { *out = BM_ROUTE_ID_AND_FLAGS_MAP; return 0; }
    if (!strcmp(s, "e2epass")) { *out = (uint8_t)(BM_ROUTE_E2EPASS); return 0; }
    if (!strcmp(s, "e2efail")) { *out = (uint8_t)(BM_ROUTE_E2EFAIL); return 0; }
    return -EINVAL;
}

static void print_hex_bytes(const uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
    {
        printf("%02X", buf[i]);
        if (i + 1 < len)
            printf(" ");
    }
    printf("\n");
}

static int is_netdev_up(const char *dev)
{
    char path[256];
    char buf[32];
    FILE *f;

    if (!dev || !*dev)
        return 0;
    /* Reject path traversal characters in netdev name */
    if (strchr(dev, '/') || strchr(dev, '.') || strlen(dev) > 15)
        return 0;

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", dev);
    f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(buf, sizeof(buf), f))
    {
        fclose(f);
        return 0;
    }
    fclose(f);

    if (!strncmp(buf, "up", 2))
        return 1;
    return 0;
}

// Check if device supports logging/replay (only Gen2.5 devices)
static int device_supports_storage(const char *dev)
{
    char path[256];
    char buf[32];
    FILE *f;
    unsigned int pid;

    if (!dev || !*dev)
        return 0;

    // Try to read device modalias or product ID from sysfs
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/modalias", dev);
    f = fopen(path, "r");
    if (!f)
    {
        // If modalias not available, try alternative path
        snprintf(path, sizeof(path), "/sys/class/net/%s/uevent", dev);
        f = fopen(path, "r");
        if (!f)
            return 0;
    }

    int found = 0;
    while (fgets(buf, sizeof(buf), f))
    {
        // Look for product ID in modalias or uevent
        // Format: usb:vXXXXpYYYY...
        if (strstr(buf, "usb:v") && strstr(buf, "p"))
        {
            char *pid_str = strstr(buf, "p");
            if (pid_str && strlen(pid_str) >= 5)
            {
                if (sscanf(pid_str + 1, "%04X", &pid) == 1)
                {
                    found = 1;
                    break;
                }
            }
        }
    }
    fclose(f);

    if (!found)
        return 0;

    // Only Gen2.5 (E122, E142) supports logging/replay
    if (pid == BMCAN_PID_E122 || pid == BMCAN_PID_E142)
        return 1;

    // Gen2 and Gen3 do NOT support logging/replay in this driver
    return 0;
}

static uint32_t pack_can_id(uint32_t can_id, int ext)
{
    if (!ext)
        return can_id & 0x7FFU;
    {
        uint32_t sid = (can_id >> 18) & 0x7FFU;
        uint32_t eid = can_id & 0x3FFFFU;
        return (eid << 11) | sid;
    }
}

typedef enum 
{
    CMD_TXTASK = 0,
    CMD_ROUTE,
    CMD_LOGGING,
    CMD_REPLAY,
    CMD_SAVECONFIG,
    CMD_LOADCONFIG,
    CMD_CLEARCONFIG,    // BMAPI standard: clear configuration saved in storage
    CMD_INVALIDATE,     // Invalidate hardware txtask/route (write all zeros)
} bm_cmd_t;

int main(int argc, char **argv)
{
    bm_cmd_t cmd = CMD_TXTASK;
    int argi = 1;
    if (argc > 1 && argv[1][0] != '-')
    {
        if (!strcmp(argv[1], "txtasks"))
        {
            cmd = CMD_TXTASK;
            argi = 2;
        }
         else if (!strcmp(argv[1], "routes"))
        {
            cmd = CMD_ROUTE;
            argi = 2;
        }
         else if (!strcmp(argv[1], "logging"))
        {
            cmd = CMD_LOGGING;
            argi = 2;
        }
         else if (!strcmp(argv[1], "replay"))
        {
            cmd = CMD_REPLAY;
            argi = 2;
        }
         else if (!strcmp(argv[1], "save"))
        {
            cmd = CMD_SAVECONFIG;
            argi = 2;
        }
         else if (!strcmp(argv[1], "load"))
        {
            cmd = CMD_LOADCONFIG;
            argi = 2;
        }
         else if (!strcmp(argv[1], "clear"))
        {
            cmd = CMD_CLEARCONFIG;
            argi = 2;
        }
         else if (!strcmp(argv[1], "invalidate"))
        {
            cmd = CMD_INVALIDATE;
            argi = 2;
        }
    }

    if (cmd == CMD_ROUTE)
    {
        BM_MessageRouteTypeDef route;
        memset(&route, 0, sizeof(route));  // Clear all fields to avoid union uninitialized issues
        const char *route_type_str = NULL;
        int source = -1;
        uint32_t target = 0;
        uint32_t flags_mask = 0;
        uint32_t flags_value = 0;
        uint32_t id_mask = 0;
        uint32_t id_value = 0;
        int route_index = 0;
        const char *route_device = NULL;
        int route_apply = 0;
        memset(&route, 0, sizeof(route));

        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--route-type") && i + 1 < argc)
            {
                route_type_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--source") && i + 1 < argc)
            {
                source = atoi(argv[++i]);
            }
             else if (!strcmp(argv[i], "--target") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &target)) return 1;
            }
             else if (!strcmp(argv[i], "--flags-mask") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &flags_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--flags-value") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &flags_value)) return 1;
            }
             else if (!strcmp(argv[i], "--id-mask") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &id_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--id-value") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &id_value)) return 1;
            }
             else if (!strcmp(argv[i], "--index") && i + 1 < argc)
            {
                route_index = atoi(argv[++i]);
            }
             else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                route_device = argv[++i];
            }
             else if (!strcmp(argv[i], "--apply"))
            {
                route_apply = 1;
            }
            else
            {
                usage_routes(argv[0]);
                return 1;
            }
        }

        if (!route_type_str || source < 0 || source > 15)
        {
            usage_routes(argv[0]);
            return 1;
        }
        if (parse_route_type(route_type_str, &route.type))
        {
            fprintf(stderr, "invalid --route-type\n");
            return 1;
        }
        if (target > 0xFFFFU)
        {
            fprintf(stderr, "--target out of range (0..65535)\n");
            return 1;
        }
        if (flags_mask > 0xFFU || flags_value > 0xFFU)
        {
            fprintf(stderr, "--flags-mask/value out of range (0..255)\n");
            return 1;
        }
        route.source = (uint8_t)source;
        route.target = (uint16_t)target;
        route.flagsmask = (uint8_t)flags_mask;
        route.flagsvalue = (uint8_t)flags_value;
        route.idmask = id_mask;
        route.idvalue = id_value;

        print_hex_bytes((const uint8_t *)&route, sizeof(route));

        if (route_device)
        {
            if (route_index < 0 || route_index > 255)
            {
                fprintf(stderr, "--index out of range (0..255)\n");
                return 1;
            }
            if (!is_netdev_up(route_device))
            {
                fprintf(stderr, "device %s is not up\n", route_device);
                return 2;
            }
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/routes", route_device);
            FILE *f = fopen(path, "w");
            if (!f)
            {
                perror("fopen route");
                return 1;
            }
            char rline[64];
            int rpos = snprintf(rline, sizeof(rline), "%d ", route_index);
            for (size_t j = 0; j < sizeof(route); j++)
                rpos += snprintf(rline + rpos, sizeof(rline) - rpos, "%s%02X", j ? " " : "", ((const uint8_t *)&route)[j]);
            rpos += snprintf(rline + rpos, sizeof(rline) - rpos, "\n");
            size_t rwritten = fwrite(rline, 1, rpos, f);
            int rclosed = fclose(f);
            if (rwritten != (size_t)rpos || rclosed != 0)
            {
                fprintf(stderr, "Failed to write route to %s\n", route_device);
                return 1;
            }

            if (route_apply)
            {
                char spath[256];
                snprintf(spath, sizeof(spath), "/sys/class/net/%s/save", route_device);
                FILE *sf = fopen(spath, "w");
                if (!sf)
                {
                    perror("fopen save");
                    return 4;
                }
                if (fprintf(sf, "0x0400") < 0 || fclose(sf) != 0)
                {
                    fprintf(stderr, "Failed to save config on %s\n", route_device);
                    return 4;
                }
                printf("Config saved to %s (mask=0x0400: route)\n", route_device);
            }
        }
        return 0;
    }

    if (cmd == CMD_LOGGING)
    {
        // Check device support for logging
        const char *log_device = NULL;
        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                log_device = argv[++i];
                break;
            }
        }

        if (log_device && !device_supports_storage(log_device))
        {
            fprintf(stderr, "WARNING: Device '%s' may not support logging (not Gen2.5).\n", log_device);
            fprintf(stderr, "Proceeding anyway - device firmware will reject if unsupported.\n");
        }

        BM_LoggingConfigTypeDef cfg;
        memset(&cfg, 0, sizeof(cfg));  // Clear all fields to avoid union uninitialized issues
        const char *mode_str = NULL;
        const char *format_str = NULL;
        const char *direction_str = NULL;
        const char *path_mode_str = NULL;
        const char *path_format_str = NULL;
        uint16_t channels = 0;
        uint8_t path_arg = 0;
        uint8_t seg_create = 0;
        uint8_t seg_overwrite = 0;
        uint16_t seg_nfiles = 0;
        uint32_t seg_nmessages = 0;
        uint32_t seg_nbytes = 0;
        uint32_t seg_nseconds = 0;
        int logging_apply = 0;

        memset(&cfg, 0, sizeof(cfg));
        cfg.version = 1;

        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--mode") && i + 1 < argc)
            {
                mode_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--format") && i + 1 < argc)
            {
                format_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--channels") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &channels)) return 1;
            }
             else if (!strcmp(argv[i], "--direction") && i + 1 < argc)
            {
                direction_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--path-mode") && i + 1 < argc)
            {
                path_mode_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--path-format") && i + 1 < argc)
            {
                path_format_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--path-arg") && i + 1 < argc)
            {
                if (parse_u8(argv[++i], &path_arg)) return 1;
            }
             else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                i++;  /* skip device name, already parsed above */
            }
             else if (!strcmp(argv[i], "--apply"))
            {
                logging_apply = 1;
            }
             else if (!strcmp(argv[i], "--start-channels") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.starttrigger.channels)) return 1;
            }
             else if (!strcmp(argv[i], "--start-flags-mask") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.starttrigger.flags_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--start-flags-value") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.starttrigger.flags_value)) return 1;
            }
             else if (!strcmp(argv[i], "--start-id-mask") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.starttrigger.id_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--start-id-value") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.starttrigger.id_value)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-channels") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.stoptrigger.channels)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-flags-mask") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.stoptrigger.flags_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-flags-value") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.stoptrigger.flags_value)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-id-mask") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.stoptrigger.id_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-id-value") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.stoptrigger.id_value)) return 1;
            }
             else if (!strcmp(argv[i], "--seg-create-new") && i + 1 < argc)
            {
                if (parse_bool(argv[++i], &seg_create)) return 1;
            }
             else if (!strcmp(argv[i], "--seg-overwrite") && i + 1 < argc)
            {
                if (parse_bool(argv[++i], &seg_overwrite)) return 1;
            }
             else if (!strcmp(argv[i], "--seg-nfiles") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &seg_nfiles)) return 1;
            }
             else if (!strcmp(argv[i], "--seg-nmessages") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &seg_nmessages)) return 1;
            }
             else if (!strcmp(argv[i], "--seg-nbytes") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &seg_nbytes)) return 1;
            }
             else if (!strcmp(argv[i], "--seg-nseconds") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &seg_nseconds)) return 1;
            }
            else
            {
                usage_logging(argv[0]);
                return 1;
            }
        }

        if (mode_str && parse_storage_mode(mode_str, &cfg.mode))
        {
            fprintf(stderr, "invalid --mode\n");
            return 1;
        }
        if (format_str && parse_storage_format(format_str, &cfg.format))
        {
            fprintf(stderr, "invalid --format\n");
            return 1;
        }
        if (direction_str && parse_storage_direction(direction_str, &cfg.direction))
        {
            fprintf(stderr, "invalid --direction\n");
            return 1;
        }
        if (path_mode_str && parse_path_mode(path_mode_str, &cfg.path.mode))
        {
            fprintf(stderr, "invalid --path-mode\n");
            return 1;
        }
        cfg.channels = channels;
        cfg.path.arg = path_arg;
        if (path_format_str)
        {
            if (strlen(path_format_str) >= sizeof(cfg.path.format))
            {
                fprintf(stderr, "--path-format too long (max %lu)\n", (unsigned long)(sizeof(cfg.path.format) - 1));
                return 1;
            }
            strncpy(cfg.path.format, path_format_str, sizeof(cfg.path.format) - 1);
            cfg.path.format[sizeof(cfg.path.format) - 1] = '\0';
        }
        cfg.segmentation.createNewFileOnStart = seg_create;
        cfg.segmentation.overwriteOldFileOnFull = seg_overwrite;
        cfg.segmentation.nfiles = seg_nfiles;
        cfg.segmentation.nmessagesPerFile = seg_nmessages;
        cfg.segmentation.nbytesPerFile = seg_nbytes;
        cfg.segmentation.nsecondsPerFile = seg_nseconds;

        /* Write to device if --device specified */
        if (log_device)
        {
            if (!is_netdev_up(log_device))
            {
                fprintf(stderr, "device %s is not up\n", log_device);
                return 2;
            }
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/logging", log_device);
            FILE *f = fopen(path, "w");
            if (!f)
            {
                perror("fopen logging");
                return 4;
            }
            const uint8_t *raw = (const uint8_t *)&cfg;
            char lbuf[512];
            int lpos = 0;
            for (size_t j = 0; j < sizeof(cfg); j++)
                lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "%s%02X", j ? " " : "", raw[j]);
            lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "\n");
            size_t lwritten = fwrite(lbuf, 1, lpos, f);
            int lclosed = fclose(f);
            if (lwritten != (size_t)lpos || lclosed != 0)
            {
                fprintf(stderr, "Failed to write logging config to %s\n", log_device);
                return 1;
            }
            printf("Logging config applied to %s\n", log_device);

            /* --apply: save config to device storage after setting */
            if (logging_apply)
            {
                if (!log_device)
                {
                    fprintf(stderr, "--apply requires --device\n");
                    return 1;
                }
                char spath[256];
                snprintf(spath, sizeof(spath), "/sys/class/net/%s/save", log_device);
                FILE *sf = fopen(spath, "w");
                if (!sf)
                {
                    perror("fopen save");
                    return 4;
                }
                /* Save mode+bitrate+term+logging */
                if (fprintf(sf, "0x080D") < 0 || fclose(sf) != 0)
                {
                    fprintf(stderr, "Failed to save config on %s\n", log_device);
                    return 4;
                }
                printf("Config saved to %s (mask=0x080D: mode+bitrate+term+logging)\n", log_device);
            }
        }
        else
        {
            print_hex_bytes((const uint8_t *)&cfg, sizeof(cfg));
        }
        return 0;
    }

    if (cmd == CMD_REPLAY)
    {
        // Check device support for replay
        const char *replay_device = NULL;
        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                replay_device = argv[++i];
                break;
            }
        }

        if (replay_device && !device_supports_storage(replay_device))
        {
            fprintf(stderr, "WARNING: Device '%s' may not support replay (not Gen2.5).\n", replay_device);
            fprintf(stderr, "Proceeding anyway - device firmware will reject if unsupported.\n");
        }

        BM_ReplayConfigTypeDef cfg;
        memset(&cfg, 0, sizeof(cfg));  // Clear all fields to avoid union uninitialized issues
        const char *mode_str = NULL;
        const char *format_str = NULL;
        const char *direction_str = NULL;
        const char *path_mode_str = NULL;
        const char *path_format_str = NULL;
        uint16_t channels = 0;
        uint8_t path_arg = 0;
        uint8_t cyclic = 0;
        uint8_t force_zero = 0;
        uint16_t msgdelay = 0;
        uint16_t sessiondelay = 0;
        uint16_t cycledelay = 0;
        int replay_apply = 0;

        memset(&cfg, 0, sizeof(cfg));
        cfg.version = 1;

        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--mode") && i + 1 < argc)
            {
                mode_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--format") && i + 1 < argc)
            {
                format_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--channels") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &channels)) return 1;
            }
             else if (!strcmp(argv[i], "--direction") && i + 1 < argc)
            {
                direction_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--cyclic") && i + 1 < argc)
            {
                if (parse_bool(argv[++i], &cyclic)) return 1;
            }
             else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                i++;  /* skip device name, already parsed above */
            }
             else if (!strcmp(argv[i], "--apply"))
            {
                replay_apply = 1;
            }
             else if (!strcmp(argv[i], "--path-mode") && i + 1 < argc)
            {
                path_mode_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--path-format") && i + 1 < argc)
            {
                path_format_str = argv[++i];
            }
             else if (!strcmp(argv[i], "--path-arg") && i + 1 < argc)
            {
                if (parse_u8(argv[++i], &path_arg)) return 1;
            }
             else if (!strcmp(argv[i], "--start-channels") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.starttrigger.channels)) return 1;
            }
             else if (!strcmp(argv[i], "--start-flags-mask") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.starttrigger.flags_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--start-flags-value") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.starttrigger.flags_value)) return 1;
            }
             else if (!strcmp(argv[i], "--start-id-mask") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.starttrigger.id_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--start-id-value") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.starttrigger.id_value)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-channels") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.stoptrigger.channels)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-flags-mask") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.stoptrigger.flags_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-flags-value") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cfg.stoptrigger.flags_value)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-id-mask") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.stoptrigger.id_mask)) return 1;
            }
             else if (!strcmp(argv[i], "--stop-id-value") && i + 1 < argc)
            {
                if (parse_u32(argv[++i], &cfg.stoptrigger.id_value)) return 1;
            }
             else if (!strcmp(argv[i], "--msgdelay") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &msgdelay)) return 1;
            }
             else if (!strcmp(argv[i], "--sessiondelay") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &sessiondelay)) return 1;
            }
             else if (!strcmp(argv[i], "--cycledelay") && i + 1 < argc)
            {
                if (parse_u16(argv[++i], &cycledelay)) return 1;
            }
             else if (!strcmp(argv[i], "--force-zero-ts") && i + 1 < argc)
            {
                if (parse_bool(argv[++i], &force_zero)) return 1;
            }
            else
            {
                usage_replay(argv[0]);
                return 1;
            }
        }

        if (mode_str && parse_storage_mode(mode_str, &cfg.mode))
        {
            fprintf(stderr, "invalid --mode\n");
            return 1;
        }
        if (format_str && parse_storage_format(format_str, &cfg.format))
        {
            fprintf(stderr, "invalid --format\n");
            return 1;
        }
        if (direction_str && parse_storage_direction(direction_str, &cfg.direction))
        {
            fprintf(stderr, "invalid --direction\n");
            return 1;
        }
        if (path_mode_str && parse_path_mode(path_mode_str, &cfg.path.mode))
        {
            fprintf(stderr, "invalid --path-mode\n");
            return 1;
        }
        cfg.channels = channels;
        cfg.path.arg = path_arg;
        cfg.cyclic = cyclic;
        if (path_format_str)
        {
            if (strlen(path_format_str) >= sizeof(cfg.path.format))
            {
                fprintf(stderr, "--path-format too long (max %lu)\n", (unsigned long)(sizeof(cfg.path.format) - 1));
                return 1;
            }
            strncpy(cfg.path.format, path_format_str, sizeof(cfg.path.format) - 1);
            cfg.path.format[sizeof(cfg.path.format) - 1] = '\0';
        }
        cfg.timing.msgdelay = msgdelay;
        cfg.timing.sessiondelay = sessiondelay;
        cfg.timing.cycledelay = cycledelay;
        cfg.timing.forceZeroTimestampOnFirstMsg = force_zero;

        /* Write to device if --device specified */
        if (replay_device)
        {
            if (!is_netdev_up(replay_device))
            {
                fprintf(stderr, "device %s is not up\n", replay_device);
                return 2;
            }
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/replay", replay_device);
            FILE *f = fopen(path, "w");
            if (!f)
            {
                perror("fopen replay");
                return 4;
            }
            const uint8_t *raw = (const uint8_t *)&cfg;
            char rbuf[512];
            int rpos2 = 0;
            for (size_t j = 0; j < sizeof(cfg); j++)
                rpos2 += snprintf(rbuf + rpos2, sizeof(rbuf) - rpos2, "%s%02X", j ? " " : "", raw[j]);
            rpos2 += snprintf(rbuf + rpos2, sizeof(rbuf) - rpos2, "\n");
            size_t rwritten = fwrite(rbuf, 1, rpos2, f);
            int r2closed = fclose(f);
            if (rwritten != (size_t)rpos2 || r2closed != 0)
            {
                fprintf(stderr, "Failed to write replay config to %s\n", replay_device);
                return 1;
            }
            printf("Replay config applied to %s\n", replay_device);

            /* --apply: save config to device storage after setting */
            if (replay_apply)
            {
                if (!replay_device)
                {
                    fprintf(stderr, "--apply requires --device\n");
                    return 1;
                }
                char spath[256];
                snprintf(spath, sizeof(spath), "/sys/class/net/%s/save", replay_device);
                FILE *sf = fopen(spath, "w");
                if (!sf)
                {
                    perror("fopen save");
                    return 4;
                }
                /* Save mode+bitrate+term+replay */
                if (fprintf(sf, "0x100D") < 0 || fclose(sf) != 0)
                {
                    fprintf(stderr, "Failed to save config on %s\n", replay_device);
                    return 4;
                }
                printf("Config saved to %s (mask=0x100D: mode+bitrate+term+replay)\n", replay_device);
            }
        }
        else
        {
            print_hex_bytes((const uint8_t *)&cfg, sizeof(cfg));
        }
        return 0;
    }

    int ext = 0, fd = 0, brs = 0, rtr = 0;
    const char *id_str = NULL;
    const char *type_str = NULL;
    const char *payload_str = "";
    char payload_buf[512];
    payload_buf[0] = '\0';
    const char *device = NULL;
    int apply = 0;
    int index = 0;
    int cycle = 0;
    int delay = 0;
    int rounds = 0xFFFF;
    int messages = 1;
    int length = -1;
    uint32_t min = 0, max = 0, step = 0, seed = 0;
    uint16_t startbit = 0;
    uint8_t nbits = 0;
    uint8_t format = 0x80;
    int type = BM_TXTASK_FIXED;
    int i;

    // Save/Load/Clear config commands (handle before txtask parsing)
    if (cmd == CMD_SAVECONFIG || cmd == CMD_LOADCONFIG || cmd == CMD_CLEARCONFIG)
    {
        const char *device = NULL;
        const char *mask_str = NULL;

        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                device = argv[++i];
            }
             else if (!strcmp(argv[i], "--mask") && i + 1 < argc)
            {
                mask_str = argv[++i];
            }
        }

        if (!device)
        {
            fprintf(stderr, "Error: --device <canX> is required\n");
            fprintf(stderr, "Usage: %s [save|load|clear] --device <canX> [--mask 0xNNNN]\n", argv[0]);
            return 1;
        }

        if (!is_netdev_up(device))
        {
            fprintf(stderr, "device %s is not up\n", device);
            return 2;
        }

        char path[256];
        const char *op = NULL;
        if (cmd == CMD_SAVECONFIG)
        {
            op = "save";
        }
         else if (cmd == CMD_LOADCONFIG)
        {
            op = "load";
        }
         else if (cmd == CMD_CLEARCONFIG)
        {
            op = "clear";
        }

        snprintf(path, sizeof(path), "/sys/class/net/%s/%s", device, op);
        FILE *f = fopen(path, "w");
        if (!f)
        {
            perror("fopen config sysfs");
            return 1;
        }

        /* Write hex mask if specified, otherwise 0xFFFF (all) */
        const char *write_val = "0xFFFF";
        char mask_buf[16];
        if (mask_str)
        {
            if (strncmp(mask_str, "0x", 2) == 0 || strncmp(mask_str, "0X", 2) == 0)
                snprintf(mask_buf, sizeof(mask_buf), "%s", mask_str);
            else
                snprintf(mask_buf, sizeof(mask_buf), "0x%s", mask_str);
            write_val = mask_buf;
        }

        if (fprintf(f, "%s", write_val) < 0 || fflush(f) != 0)
        {
            fprintf(stderr, "%s config write error on %s\n", op, device);
            fclose(f);
            return 1;
        }
        if (fclose(f) != 0)
        {
            fprintf(stderr, "%s config failed on %s (errno=%d: %s)\n",
                    op, device, errno, strerror(errno));
            return 1;
        }

        printf("%s operation completed on %s\n", op, device);
        return 0;
    }

    // INVALIDATE command - Invalidate hardware txtask/route/logging/replay (write all zeros)
    if (cmd == CMD_INVALIDATE)
    {
        const char *device = NULL;
        int invalidate_tx = 0;
        int invalidate_route = 0;
        int invalidate_logging = 0;
        int invalidate_replay = 0;
        int tx_start = 0, tx_end = 63;  // Default clear all 64 entries
        int route_start = 0, route_end = 63;

        for (int i = argi; i < argc; i++)
        {
            if (!strcmp(argv[i], "--device") && i + 1 < argc)
            {
                device = argv[++i];
            }
            else if (!strcmp(argv[i], "--txtasks") || !strcmp(argv[i], "--txtask"))
            {
                invalidate_tx = 1;
            }
            else if (!strcmp(argv[i], "--route"))
            {
                invalidate_route = 1;
            }
            else if (!strcmp(argv[i], "--logging"))
            {
                invalidate_logging = 1;
            }
            else if (!strcmp(argv[i], "--replay"))
            {
                invalidate_replay = 1;
            }
            else if (!strcmp(argv[i], "--all"))
            {
                invalidate_tx = 1;
                invalidate_route = 1;
                invalidate_logging = 1;
                invalidate_replay = 1;
            }
            else if (!strcmp(argv[i], "--tx-range") && i + 1 < argc)
            {
                char *tmp = strdup(argv[++i]);
                char *dash;
                char *endp;
                if (!tmp)
                {
                    fprintf(stderr, "Error: out of memory parsing --tx-range\n");
                    return 1;
                }
                dash = strchr(tmp, '-');
                if (!dash)
                {
                    fprintf(stderr, "Error: --tx-range requires format START-END (e.g. 0-63)\n");
                    free(tmp);
                    return 1;
                }
                *dash = '\0';
                if (tmp[0] == '\0')
                {
                    fprintf(stderr, "Error: invalid --tx-range start: empty value\n");
                    free(tmp);
                    return 1;
                }
                if (dash[1] == '\0')
                {
                    fprintf(stderr, "Error: invalid --tx-range end: empty value\n");
                    free(tmp);
                    return 1;
                }
                tx_start = (int)strtol(tmp, &endp, 10);
                if (*endp)
                {
                    fprintf(stderr, "Error: invalid --tx-range start: '%s'\n", tmp);
                    free(tmp);
                    return 1;
                }
                tx_end = (int)strtol(dash + 1, &endp, 10);
                if (*endp)
                {
                    fprintf(stderr, "Error: invalid --tx-range end: '%s'\n", dash + 1);
                    free(tmp);
                    return 1;
                }
                free(tmp);
            }
            else if (!strcmp(argv[i], "--route-range") && i + 1 < argc)
            {
                char *tmp = strdup(argv[++i]);
                char *dash;
                char *endp;
                if (!tmp)
                {
                    fprintf(stderr, "Error: out of memory parsing --route-range\n");
                    return 1;
                }
                dash = strchr(tmp, '-');
                if (!dash)
                {
                    fprintf(stderr, "Error: --route-range requires format START-END (e.g. 0-63)\n");
                    free(tmp);
                    return 1;
                }
                *dash = '\0';
                if (tmp[0] == '\0')
                {
                    fprintf(stderr, "Error: invalid --route-range start: empty value\n");
                    free(tmp);
                    return 1;
                }
                if (dash[1] == '\0')
                {
                    fprintf(stderr, "Error: invalid --route-range end: empty value\n");
                    free(tmp);
                    return 1;
                }
                route_start = (int)strtol(tmp, &endp, 10);
                if (*endp)
                {
                    fprintf(stderr, "Error: invalid --route-range start: '%s'\n", tmp);
                    free(tmp);
                    return 1;
                }
                route_end = (int)strtol(dash + 1, &endp, 10);
                if (*endp)
                {
                    fprintf(stderr, "Error: invalid --route-range end: '%s'\n", dash + 1);
                    free(tmp);
                    return 1;
                }
                free(tmp);
            }
            else
            {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                usage_invalidate(argv[0]);
                return 1;
            }
        }

        if (!device)
        {
            fprintf(stderr, "Error: --device <canX> is required for invalidate\n");
            return 1;
        }

        /* Validate range bounds */
        if (tx_start < 0 || tx_end > 63 || tx_start > tx_end)
        {
            fprintf(stderr, "Error: --tx-range %d-%d invalid (must be 0-63, start <= end)\n",
                    tx_start, tx_end);
            return 1;
        }
        if (route_start < 0 || route_end > 63 || route_start > route_end)
        {
            fprintf(stderr, "Error: --route-range %d-%d invalid (must be 0-63, start <= end)\n",
                    route_start, route_end);
            return 1;
        }

        // Default clear tx and route
        if (!invalidate_tx && !invalidate_route && !invalidate_logging && !invalidate_replay)
        {
            invalidate_tx = 1;
            invalidate_route = 1;
        }

        if (!is_netdev_up(device))
        {
            fprintf(stderr, "device %s is not up\n", device);
            return 2;
        }

        // Skip logging/replay on devices that don't support storage
        if ((invalidate_logging || invalidate_replay) && !device_supports_storage(device))
        {
            invalidate_logging = 0;
            invalidate_replay = 0;
        }

        // Clear txtask: write 128 bytes of zeros to all specified indices
        if (invalidate_tx)
        {
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/txtasks", device);

            for (int idx = tx_start; idx <= tx_end; idx++)
            {
                FILE *f = fopen(path, "w");
                if (!f)
                {
                    perror("fopen txtasks");
                    return 4;
                }
                char line[512];
                int pos = snprintf(line, sizeof(line), "%d ", idx);
                for (int j = 0; j < 128; j++)
                    pos += snprintf(line + pos, sizeof(line) - pos, "%s%02X", j ? " " : "", 0);
                pos += snprintf(line + pos, sizeof(line) - pos, "\n");
                size_t written = fwrite(line, 1, pos, f);
                int closed = fclose(f);
                if (written != (size_t)pos || closed != 0)
                {
                    fprintf(stderr, "Failed to invalidate txtask index %d\n", idx);
                    return 4;
                }
            }
            printf("Invalidated txtasks indices %d-%d on %s\n", tx_start, tx_end, device);
        }

        // Clear route: write 16 bytes of zeros per index (open/write/close each)
        if (invalidate_route)
        {
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/routes", device);

            for (int idx = route_start; idx <= route_end; idx++)
            {
                FILE *f = fopen(path, "w");
                if (!f)
                {
                    perror("fopen route");
                    return 5;
                }
                char line[256];
                int pos = snprintf(line, sizeof(line), "%d ", idx);
                for (int j = 0; j < 16; j++)
                    pos += snprintf(line + pos, sizeof(line) - pos, "%s%02X", j ? " " : "", 0);
                pos += snprintf(line + pos, sizeof(line) - pos, "\n");
                size_t written = fwrite(line, 1, pos, f);
                int closed = fclose(f);
                if (written != (size_t)pos || closed != 0)
                {
                    fprintf(stderr, "Failed to invalidate route index %d\n", idx);
                    return 5;
                }
            }
            printf("Invalidated routes indices %d-%d on %s\n", route_start, route_end, device);
        }

        // Clear logging: write empty logging configuration (stop recording)
        if (invalidate_logging)
        {
            BM_LoggingConfigTypeDef cfg;
            memset(&cfg, 0, sizeof(cfg));  // All zeros = disabled = stop recording

            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/logging", device);
            FILE *f = fopen(path, "w");
            if (!f)
            {
                perror("fopen logging");
                return 4;
            }
            const uint8_t *raw = (const uint8_t *)&cfg;
            char ilbuf[512];
            int ilpos = 0;
            for (size_t j = 0; j < sizeof(cfg); j++)
                ilpos += snprintf(ilbuf + ilpos, sizeof(ilbuf) - ilpos, "%s%02X", j ? " " : "", raw[j]);
            ilpos += snprintf(ilbuf + ilpos, sizeof(ilbuf) - ilpos, "\n");
            size_t ilwritten = fwrite(ilbuf, 1, ilpos, f);
            int ilclosed = fclose(f);
            if (ilwritten != (size_t)ilpos || ilclosed != 0)
            {
                fprintf(stderr, "Failed to invalidate logging on %s\n", device);
                return 4;
            }
            printf("Invalidated logging on %s (stopped recording)\n", device);
        }

        // Clear replay: write empty replay configuration (stop playback)
        if (invalidate_replay)
        {
            BM_ReplayConfigTypeDef cfg;
            memset(&cfg, 0, sizeof(cfg));  // All zeros = disabled = stop playback

            char path[256];
            snprintf(path, sizeof(path), "/sys/class/net/%s/replay", device);
            FILE *f = fopen(path, "w");
            if (!f)
            {
                perror("fopen replay");
                return 4;
            }
            const uint8_t *raw = (const uint8_t *)&cfg;
            char irbuf[512];
            int irpos = 0;
            for (size_t j = 0; j < sizeof(cfg); j++)
                irpos += snprintf(irbuf + irpos, sizeof(irbuf) - irpos, "%s%02X", j ? " " : "", raw[j]);
            irpos += snprintf(irbuf + irpos, sizeof(irbuf) - irpos, "\n");
            size_t irwritten = fwrite(irbuf, 1, irpos, f);
            int irclosed = fclose(f);
            if (irwritten != (size_t)irpos || irclosed != 0)
            {
                fprintf(stderr, "Failed to invalidate replay on %s\n", device);
                return 4;
            }
            printf("Invalidated replay on %s (stopped playback)\n", device);
        }

        return 0;
    }

    // TXTASK command (default)
    for (i = argi; i < argc; i++)
    {
        if (!strcmp(argv[i], "--type") && i + 1 < argc)
        {
            type_str = argv[++i];
        }
         else if (!strcmp(argv[i], "--id") && i + 1 < argc)
        {
            id_str = argv[++i];
        }
         else if (!strcmp(argv[i], "--ext"))
        {
            ext = 1;
        }
         else if (!strcmp(argv[i], "--fd"))
        {
            fd = 1;
        }
         else if (!strcmp(argv[i], "--brs"))
        {
            brs = 1;
        }
         else if (!strcmp(argv[i], "--rtr"))
        {
            rtr = 1;
        }
        else if (!strcmp(argv[i], "--payload") && i + 1 < argc)
        {
            size_t used = 0;
            payload_buf[0] = '\0';
            while (i + 1 < argc)
            {
                const char *tok = argv[i + 1];
                if (tok[0] == '-' && tok[1] == '-')
                    break;
                if (used + strlen(tok) + 2 >= sizeof(payload_buf))
                {
                    fprintf(stderr, "payload too long\n");
                    return 1;
                }
                if (used > 0)
                {
                    payload_buf[used++] = ' ';
                    payload_buf[used] = '\0';
                }
                strncpy(payload_buf + used, tok, sizeof(payload_buf) - used - 1);
                used += strlen(tok);
                payload_buf[used] = '\0';
                i++;
            }
            payload_str = payload_buf;
        }
         else if (!strcmp(argv[i], "--length") && i + 1 < argc)
        {
            length = atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--cycle") && i + 1 < argc)
        {
            cycle = atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
        {
            delay = atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
        {
            rounds = atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--messages") && i + 1 < argc)
        {
            messages = atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--startbit") && i + 1 < argc)
        {
            startbit = (uint16_t)atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--nbits") && i + 1 < argc)
        {
            nbits = (uint8_t)atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--format") && i + 1 < argc)
        {
            if (parse_endian_format(argv[++i], &format))
            {
                fprintf(stderr, "invalid --format (intel|moto)\n");
                return 1;
            }
        }
         else if (!strcmp(argv[i], "--min") && i + 1 < argc)
        {
            if (parse_u32(argv[++i], &min)) return 1;
        }
         else if (!strcmp(argv[i], "--max") && i + 1 < argc)
        {
            if (parse_u32(argv[++i], &max)) return 1;
        }
         else if (!strcmp(argv[i], "--step") && i + 1 < argc)
        {
            if (parse_u32(argv[++i], &step)) return 1;
        }
         else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
        {
            if (parse_u32(argv[++i], &seed)) return 1;
        }
         else if (!strcmp(argv[i], "--index") && i + 1 < argc)
        {
            index = atoi(argv[++i]);
        }
         else if (!strcmp(argv[i], "--device") && i + 1 < argc)
        {
            device = argv[++i];
        }
         else if (!strcmp(argv[i], "--apply"))
        {
            apply = 1;
        }
        else
        {
            usage_txtasks(argv[0]);
            return 1;
        }
    }

    if (!type_str || !id_str)
    {
        usage_txtasks(argv[0]);
        return 1;
    }
    if (index < 0 || index > 63)
    {
        fprintf(stderr, "index must be 0..63\n");
        return 1;
    }

    if (!strcmp(type_str, "fixed"))
        type = BM_TXTASK_FIXED;
    else if (!strcmp(type_str, "incdata"))
        type = BM_TXTASK_INCDATA;
    else if (!strcmp(type_str, "incid"))
        type = BM_TXTASK_INCID;
    else if (!strcmp(type_str, "randomdata"))
        type = BM_TXTASK_RANDOMDATA;
    else if (!strcmp(type_str, "randomid"))
        type = BM_TXTASK_RANDOMID;
    else
    {
        fprintf(stderr, "invalid --type\n");
        return 1;
    }

    if (sizeof(BM_TxTaskTypeDef) != 128)
    {
        fprintf(stderr, "BM_TxTaskTypeDef size=%lu, expected 128\n", (unsigned long)sizeof(BM_TxTaskTypeDef));
        return 1;
    }

    uint8_t payload[64];
    size_t payload_len = 0;
    if (parse_hex_bytes(payload_str, payload, sizeof(payload), &payload_len) != 0)
    {
        fprintf(stderr, "invalid payload hex\n");
        return 1;
    }
    if (payload_len == 0 && length >= 0)
    {
        if (length < 0 || length > 64)
        {
            fprintf(stderr, "invalid --length (0..64)\n");
            return 1;
        }
        payload_len = (size_t)length;
    }

    uint32_t can_id = (uint32_t)strtoul(id_str, NULL, 16);
    uint8_t flags = 0;
    if (ext) flags |= BM_CAN_MESSAGE_FLAGS_IDE;
    if (rtr) flags |= BM_CAN_MESSAGE_FLAGS_RTR;
    if (fd)  flags |= BM_CAN_MESSAGE_FLAGS_FDF;
    if (brs) flags |= BM_CAN_MESSAGE_FLAGS_BRS;

    BM_TxTaskTypeDef task;
    memset(&task, 0, sizeof(task));
    task.type = (uint8_t)type;
    task.version = 1;
    task.flags = flags;
    task.length = (uint8_t)(payload_len & 0x7F);
    task.lengthunit = 0;
    task.delay = (uint16_t)delay;
    task.cycle = (uint16_t)cycle;
    task.nrounds = (uint16_t)rounds;
    task.nmessages = (uint16_t)messages;

    if (ext)
    {
        task.can.SID = (pack_can_id(can_id, 1) & 0x7FFU);
        task.can.EID = (pack_can_id(can_id, 1) >> 11) & 0x3FFFFU;
    }
    else
    {
        task.can.SID = (pack_can_id(can_id, 0) & 0x7FFU);
        task.can.EID = 0;
    }

    switch (type)
    {
    case BM_TXTASK_INCDATA:
        task.pattern.incdata.startbit = startbit;
        task.pattern.incdata.nbits = nbits;
        task.pattern.incdata.format = format;
        task.pattern.incdata.min = min;
        task.pattern.incdata.max = max;
        task.pattern.incdata.step = step;
        break;
    case BM_TXTASK_INCID:
        task.pattern.incid.min = min;
        task.pattern.incid.max = max;
        task.pattern.incid.step = step;
        break;
    case BM_TXTASK_RANDOMDATA:
        task.pattern.randomdata.startbit = startbit;
        task.pattern.randomdata.nbits = nbits;
        task.pattern.randomdata.format = format;
        task.pattern.randomdata.min = min;
        task.pattern.randomdata.max = max;
        task.pattern.randomdata.seed = seed;
        break;
    case BM_TXTASK_RANDOMID:
        task.pattern.randomid.min = min;
        task.pattern.randomid.max = max;
        task.pattern.randomid.seed = seed;
        break;
    default:
        break;
    }

    memcpy(task.payload, payload, payload_len);

    print_hex_bytes((const uint8_t *)&task, sizeof(task));

    /* Write to device if --device is specified */
    if (device)
    {
        if (!is_netdev_up(device))
        {
            fprintf(stderr, "device %s is not up\n", device);
            return 2;
        }
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/net/%s/txtasks", device);
        FILE *f = fopen(path, "w");
        if (!f)
        {
            perror("fopen txtasks");
            return 1;
        }
        char line[512];
        int pos = snprintf(line, sizeof(line), "%d ", index);
        for (size_t j = 0; j < sizeof(task); j++)
            pos += snprintf(line + pos, sizeof(line) - pos, "%s%02X", j ? " " : "", ((const uint8_t *)&task)[j]);
        pos += snprintf(line + pos, sizeof(line) - pos, "\n");
        size_t twritten = fwrite(line, 1, pos, f);
        int tclosed = fclose(f);
        if (twritten != (size_t)pos || tclosed != 0)
        {
            fprintf(stderr, "Failed to write txtask to %s\n", device);
            return 1;
        }

        /* --apply: also save txtask config to device storage */
        if (apply)
        {
            char spath[256];
            snprintf(spath, sizeof(spath), "/sys/class/net/%s/save", device);
            FILE *sf = fopen(spath, "w");
            if (!sf)
            {
                perror("fopen save");
                return 4;
            }
            if (fprintf(sf, "0x0200") < 0 || fclose(sf) != 0)
            {
                fprintf(stderr, "Failed to save config on %s\n", device);
                return 4;
            }
            printf("Config saved to %s (mask=0x0200: txtask)\n", device);
        }
    }

    return 0;
}
