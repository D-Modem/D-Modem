/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_types.h - integers of a fixed width, by their short names: u8,
 *                 u16, u32, u64 unsigned and s8, s16, s32, s64 signed
 *                 (s16 is also a 16 bit linear sample).  Every width-
 *                 sensitive integer in the tree is one of these, so the
 *                 code means the same where int is 16 bits.
 *
 *    C linkage; the header is valid C as well as C++.  The public
 *    libdmodem header (dmodemd/dmodem.h) keeps the <stdint.h> names, so
 *    that programs using it don't get these short names.
 *
 */

#ifndef DM_TYPES_H
#define DM_TYPES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#endif
