/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem_msvc.h - what of POSIX the sources take from the C library
 *                    and MSVC's lacks: ssize_t, mode_t, clock_gettime()
 *                    on CLOCK_MONOTONIC and CLOCK_REALTIME, usleep(),
 *                    strcasecmp(), popen(), ftruncate(), S_IRUSR and S_IWUSR,
 *                    PATH_MAX.  Included before each source of an
 *                    MSVC build (/FI); unistd.h, strings.h and
 *                    sys/time.h beside it stand in for POSIX's.
 *
 */
#ifndef DMODEM_MSVC_H
#define DMODEM_MSVC_H
#ifdef _MSC_VER

#include <io.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
typedef unsigned short mode_t;

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;

/* (kernel32's, without windows.h here) */
__declspec(dllimport) int __stdcall QueryPerformanceCounter(
	union _LARGE_INTEGER *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(
	union _LARGE_INTEGER *);

/* CLOCK_MONOTONIC: the performance counter; CLOCK_REALTIME: the C
   library's UTC time (timespec_get) */
static __inline int clock_gettime(clockid_t id, struct timespec *ts)
{
	if (id == CLOCK_MONOTONIC) {
		static int64_t freq;
		int64_t now;
		if (!freq)
			QueryPerformanceFrequency((union _LARGE_INTEGER *)&freq);
		QueryPerformanceCounter((union _LARGE_INTEGER *)&now);
		ts->tv_sec = (time_t)(now / freq);
		ts->tv_nsec = (long)((now % freq) * 1000000000 / freq);
		return 0;
	}
	return timespec_get(ts, TIME_UTC) ? 0 : -1;
}
#endif

__declspec(dllimport) void __stdcall Sleep(unsigned long);

/* (ms granularity, at least 1 ms) */
static __inline int usleep(unsigned int us)
{
	Sleep(us < 1000 ? 1 : us / 1000);
	return 0;
}

#define popen _popen
#define pclose _pclose
#define strcasecmp _stricmp
#define strncasecmp _strnicmp

/* POSIX's ftruncate(): fd's file cut (or grown) to length; 0, or -1 */
static __inline int ftruncate(int fd, int64_t length)
{
	return _chsize_s(fd, length) ? -1 : 0;
}

/* (the C library's _S_IREAD and _S_IWRITE, the owner's) */
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#endif
#ifndef PATH_MAX
#define PATH_MAX 260
#endif

#ifdef __cplusplus
}
#endif

#endif /* _MSC_VER */
#endif
