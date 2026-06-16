/**
 * @file        bm_usb_router_def.h
 * @brief       BUSMUST USB router protocol data type definitions.
 * @author      BUSMUST
 * @version     1.0.0.1
 *
 * @copyright
 *              Copyright 2020-2026 BUSMASTER BMAPI Project.
 *              All rights reserved. Property of Busmust Tech Co.,Ltd.
 *
 * @license
 *              This header defines BUSMUST/BMAPI protocol data structures.
 *              It is not part of the Linux kernel driver license.
 *
 *              Permission is granted to copy, compile, and redistribute this
 *              header only as part of the official BUSMUST BMAPI package or
 *              the official BUSMUST BMCAN SocketCAN software package.
 *
 *              No permission is granted to independently modify, relicense,
 *              extract, or reuse this header for non-BUSMUST products or
 *              non-BUSMUST-compatible devices without prior written permission
 *              from Busmust Tech Co.,Ltd.
 */
#pragma once

#include "bm_usb_def.h"

typedef struct
{
    uint8_t reserved[32];
    //type  // 0=invalid
        //asr1 - 5
        //OEM defined
        //args
        //maxcounter
        //crcpoly
        //crcinit
        //crcinv
        //countersignal.id
        //passed by signal
        //crcsignal.id
        //passed by signal

        //init function
        //protect function
        //check function
} BM_E2EInfoTypeDef;

typedef union
{
    float f;
    uint32_t u;
} BM_SignalValueTypeDef;

typedef struct  
{
    BM_SignalValueTypeDef k;
    BM_SignalValueTypeDef b;
    uint16_t valuestart;
    uint16_t nvalues;
} LinearSignalInterpretion;

typedef struct
{
    uint8_t type; /* 0=invalid, f32/u1-u32(u1=bool), 0x80=Intel(1)/Motorola(0) */
    uint8_t bitstart;
    uint8_t nbits;
    uint8_t edian;
    uint16_t startroute;
    uint16_t validsignal;
    uint16_t interpretion;
    uint16_t reserved[3];
    BM_SignalValueTypeDef max;
    BM_SignalValueTypeDef min;
    BM_SignalValueTypeDef def;
    BM_SignalValueTypeDef err;
} BM_SignalInfoTypeDef;

typedef struct
{
    BM_MessageIdTypeDef id; /* id=0&&dlc=0 indicates invalid? */
    uint16_t length;
    uint8_t  flags;
    uint8_t  e2e;
    uint8_t  protocol;
    uint8_t  codec;
    uint8_t  lengthsignal;
    uint8_t  validsignal;
    uint16_t signalstart;
    uint16_t nsignals;
    uint16_t cycle;
    uint8_t channel;
} BM_MessageInfoTypeDef;

/**
 * @enum  BM_RouteTypeTypeDef
 * @brief CAN route type IDs, used in BM_MessageRouteTypeDef.
 */
typedef enum
{
    BM_ROUTE_INVALID = 0,            /**< Invalid (unused) route entry */
    BM_ROUTE_UNICAST = 1,
    BM_ROUTE_BROADCAST = 2,
    BM_ROUTE_ID_MAP = 4,             /* src.id => dst.id if src.flags & flags_mask == flags_value */
    BM_ROUTE_FLAGS_MAP = 8,
    BM_ROUTE_ID_AND_FLAGS_MAP = (BM_ROUTE_ID_MAP | BM_ROUTE_FLAGS_MAP),
    BM_ROUTE_E2EPASS = 0x40U,        /**< Busmust E2E route, accept only messages that passed E2E checking */
    BM_ROUTE_E2EFAIL = 0x80U,        /**< Busmust E2E route, accept only messages that failed E2E checking (for debugging purpose) */
} BM_RouteTypeTypeDef;

typedef struct
{
    uint8_t reserved[16];
    // type // 0=invalid
    //     linear
    //     k
    //     b
    //     special values
    //     piecewise
    //     n
    //     [k，b]
    // lut
    //     n
    //     [i->o]
    // User defined function
    //     a
    //     b
    //     init function
    //     forward function
    //     backward function
    //     args from signal
    //     signal.id
    //     valid.id
} BM_TransformationTypeDef;

typedef struct
{
    uint8_t operation; // 0=invalid
    uint8_t reserved[3];
    BM_SignalValueTypeDef value;
    uint16_t signala;
    uint16_t signalb;
    uint16_t signalc;
    uint16_t signald;
    // signal1 - 2
    //     id
    //     logic
    //     or
    //     and
    //     xor
    //     not
    //     cmp
    //     gt
    //     lt
    //     eq
    //     gteq
    //     lteq
    //     arg
    //     signal - cmp.id
    //     cmp - const - value
} BM_ConditionTypeDef;

/*
 *	End of file
 */
