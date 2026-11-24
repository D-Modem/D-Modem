/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * mbedtls-win32-legacy.h - Mbed TLS's user configuration
 * (MBEDTLS_USER_CONFIG_FILE, cmake/libre.cmake) for Windows before Vista
 * (DMODEM_WINNT below 0x0600):
 *
 * - no BCryptGenRandom() (bcrypt.dll is Vista's): no entropy from the
 *   platform.  libre takes only digests and AES from Mbed TLS, never its
 *   random numbers.
 * - its snprintf() for Windows on vsnprintf(), not on msvcrt.dll's
 *   vsnprintf_s(), which Windows 9x, NT 4 and 2000 lack.  (_TRUNCATE
 *   tells platform.c that there is a vsnprintf_s(): stdio.h first, then
 *   _TRUNCATE gone.)
 * - picolibc (clang-retro's win32-picolibc): its C11 memset_s() and
 *   gmtime_s(), which platform_util.c uses when __STDC_LIB_EXT1__ is
 *   set, are declared only when __STDC_WANT_LIB_EXT1__ is set before
 *   the first header.  msvcrt's headers ignore it.  picolibc has no
 *   gmtime_s() (Microsoft's or C11's) for mbedtls_platform_gmtime_r():
 *   no dates (MBEDTLS_HAVE_TIME_DATE), which only X.509 uses.
 */
#ifndef DMODEM_MBEDTLS_WIN32_LEGACY_H
#define DMODEM_MBEDTLS_WIN32_LEGACY_H

#define MBEDTLS_NO_PLATFORM_ENTROPY

#ifndef __STDC_WANT_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#include <stdio.h>
#include <stdlib.h>
#undef _TRUNCATE

#if defined(__PICOLIBC__)
#undef MBEDTLS_HAVE_TIME_DATE
#endif

#endif
