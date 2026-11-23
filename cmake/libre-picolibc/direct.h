/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * direct.h - libre on picolibc (cmake/libre-picolibc/compat.h), which has
 * no <direct.h>: Microsoft's _mkdir() of src/sys/fs.c, as POSIX mkdir().
 */
#ifndef DMODEM_LIBRE_PICOLIBC_DIRECT_H
#define DMODEM_LIBRE_PICOLIBC_DIRECT_H
#include <sys/stat.h>
#define _mkdir(path) mkdir((path), 0700)
#endif
