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
 */
#ifndef DMODEM_MBEDTLS_WIN32_LEGACY_H
#define DMODEM_MBEDTLS_WIN32_LEGACY_H

#define MBEDTLS_NO_PLATFORM_ENTROPY

#include <stdio.h>
#include <stdlib.h>
#undef _TRUNCATE

#endif
