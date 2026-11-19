/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_retro.c - dmodem_retro_random() (dmodem_retro.h), for
 *                     clang-retro's builds: 32 random bits from
 *                     advapi32's RtlGenRandom (SystemFunction036, XP and
 *                     later) where it is there, else from a generator
 *                     (xorshift64) seeded from the clocks and the process
 *
 */
#include <stdint.h>
#include <time.h>
#include <windows.h>

#include "dmodem_retro.h"

#ifdef DMODEM_RETRO_RANDOM
/* see dmodem_retro.h */
uint32_t dmodem_retro_random(void)
{
	typedef BOOLEAN(WINAPI * gen_fn)(PVOID, ULONG);
	static gen_fn gen;
	static volatile LONG looked;
	static uint64_t x;
	static CRITICAL_SECTION lock;
	static volatile LONG lock_init;
	uint32_t r;

	if (!looked) {
		HMODULE a = LoadLibraryA("advapi32.dll");
		if (a)
			gen = (gen_fn)(void *)GetProcAddress(a,
							     "SystemFunction036");
		InterlockedExchange(&looked, 1);
	}
	if (gen && gen(&r, sizeof(r)))
		return r;

	/* (the first caller makes the lock; the others wait for it) */
	if (InterlockedCompareExchange(&lock_init, 1, 0) == 0) {
		InitializeCriticalSection(&lock);
		InterlockedExchange(&lock_init, 2);
	}
	while (lock_init != 2)
		Sleep(0);
	EnterCriticalSection(&lock);
	if (!x) {
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		x = (uint64_t)c.QuadPart ^ ((uint64_t)GetTickCount() << 32) ^
			((uint64_t)GetCurrentProcessId() << 16) ^
			(uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ULL;
	}
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	r = (uint32_t)(x >> 32);
	LeaveCriticalSection(&lock);
	return r;
}
#endif
