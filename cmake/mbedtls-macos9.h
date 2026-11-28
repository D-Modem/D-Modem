/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * mbedtls-macos9.h - Mbed TLS's user configuration
 * (MBEDTLS_USER_CONFIG_FILE, cmake/libre.cmake) for the classic Mac OS
 * (the MacOS9 system of D-Modem/gui's cmake/macos9.cmake, over the C
 * library of clang-retro/mac-sdk):
 *
 * - no entropy from the platform: the Mac OS is not Unix and not
 *   Windows for Mbed TLS.  libre takes only digests and AES from Mbed
 *   TLS, never its random numbers.
 * - no time: Mbed TLS has no mbedtls_ms_time() for the Mac OS.  Only TLS
 *   and X.509 use the time, and libre does not take them.  The same for
 *   the timing and the network modules, which are for Unix and Windows.
 */
#ifndef DMODEM_MBEDTLS_MACOS9_H
#define DMODEM_MBEDTLS_MACOS9_H

#define MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_NET_C

#endif
