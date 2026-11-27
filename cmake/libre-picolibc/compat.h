/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * compat.h - libre on picolibc (clang-retro's win32-picolibc: Windows 95,
 * 98, ME and NT 4.0 and later). cmake/libre.cmake includes this file
 * before each source of libre (-include).
 *
 * libre's Windows code expects Microsoft's C library (msvcrt). picolibc
 * is a POSIX C library. This file gives these names:
 *
 * - _open, _read, _close, _dup, _dup2, _fileno, _unlink, _stricmp and _strnicmp
 *   as the POSIX functions. (libre's "#define open _open" then gives
 *   open again, because a macro does not expand in its own expansion.)
 * - gmtime_s() with Microsoft's arguments, on gmtime_r(), and
 *   fopen_s(), on fopen().
 * - strerror_s() and memset_s() of C11 (__STDC_WANT_LIB_EXT1__).
 * - pipe(): libre's Windows pipe() takes sockets, not the int of
 *   <unistd.h>. libre's is re_win32_pipe() here.
 * - once_flag and call_once(): picolibc's <stdlib.h> has C23's, and
 *   libre's Windows threads (re_thread.h) have their own. libre's are
 *   re_once_flag and re_call_once() here.
 */
#ifndef DMODEM_LIBRE_PICOLIBC_COMPAT_H
#define DMODEM_LIBRE_PICOLIBC_COMPAT_H

#ifndef __STDC_WANT_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define _open open
#define _read read
#define _close close
#define _dup dup
#define _dup2 dup2
#define _fileno fileno
#define _unlink unlink
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

/* Microsoft's gmtime_s(): 0 on success */
static inline int dmodem_gmtime_s(struct tm *tm, const time_t *t)
{
	return gmtime_r(t, tm) ? 0 : -1;
}
#define gmtime_s dmodem_gmtime_s

/* Microsoft's fopen_s(): 0, or the errno of fopen() */
static inline int dmodem_fopen_s(FILE **fp, const char *name, const char *mode)
{
	*fp = fopen(name, mode);
	return *fp ? 0 : errno;
}
#define fopen_s dmodem_fopen_s

#define pipe re_win32_pipe
#define once_flag re_once_flag
#define call_once re_call_once

#endif
