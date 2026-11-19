/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_retro.h - what of POSIX the sources take from the C library
 *                     and clang-retro's mingw-w64 sysroots (msvcrt.dll,
 *                     without winpthreads) lack: clock_gettime() on
 *                     CLOCK_MONOTONIC and CLOCK_REALTIME; and before
 *                     Windows XP, random numbers for std::random_device's
 *                     place (dmodem_retro_random()).  Included
 *                     before each source of a clang-retro build
 *                     (-include, platforms/windows/clang-retro.cmake).
 *
 *    It uses only what Windows 95 and NT 3.51 have: the performance
 *    counter and the system time.
 *
 */
#ifndef DMODEM_RETRO_H
#define DMODEM_RETRO_H
#if defined(_WIN32) && defined(__MINGW32__)

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;

/* (kernel32's, without windows.h here) */
__declspec(dllimport) int __stdcall QueryPerformanceCounter(
	union _LARGE_INTEGER *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(
	union _LARGE_INTEGER *);
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(
	struct _FILETIME *);

/* CLOCK_MONOTONIC: the performance counter; CLOCK_REALTIME: the system
   time (100 ns units from 1601) from 1970 */
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
	} else {
		uint64_t ft;
		GetSystemTimeAsFileTime((struct _FILETIME *)&ft);
		ft -= 116444736000000000ULL;
		ts->tv_sec = (time_t)(ft / 10000000);
		ts->tv_nsec = (long)(ft % 10000000) * 100;
		return 0;
	}
}
#endif

#if _WIN32_WINNT < 0x0501
/* MinGW's std::random_device is msvcrt's rand_s(), which Windows before
   XP lacks: there it throws.  dmodem_retro_random() (dmodem_retro.c):
   32 random bits for its place */
#define DMODEM_RETRO_RANDOM 1
uint32_t dmodem_retro_random(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 && __MINGW32__ */
#endif /* DMODEM_RETRO_H */
