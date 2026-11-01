/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    mac_realtime.h - the tests' line clocks (line_exchange.cpp,
 *                     wb_pair.cpp) on time on macOS
 *
 *    macOS lets an ordinary process's sleeps run long (its timers
 *    coalesced): on a CI runner the exchange's 20 ms ticks came tens of
 *    ms late, again and again, and the modems' line ran at a third of
 *    its rate.  Mach's time constraint policy, as audio threads (and
 *    dmodemd, main.cpp's go_realtime()) have it, keeps them on time; it
 *    needs no root.  Elsewhere nothing is done.
 *
 */
#ifndef DMODEM_TEST_MAC_REALTIME_H
#define DMODEM_TEST_MAC_REALTIME_H

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#endif

/* this thread woken every tick_ns, on time: up to half of it to run
   (macOS; what fails said on stderr) */
static inline void mac_realtime(long tick_ns)
{
#ifdef __APPLE__
	mach_timebase_info_data_t tb;
	thread_time_constraint_policy_data_t p;

	mach_timebase_info(&tb);
	const double tick = static_cast<double>(tick_ns) * tb.denom / tb.numer;
	p.period = static_cast<uint32_t>(tick);
	p.computation = static_cast<uint32_t>(tick / 2);
	p.constraint = static_cast<uint32_t>(tick);
	p.preemptible = TRUE;
	kern_return_t r = thread_policy_set(pthread_mach_thread_np(pthread_self()),
		THREAD_TIME_CONSTRAINT_POLICY,
		reinterpret_cast<thread_policy_t>(&p),
		THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (r != KERN_SUCCESS)
		fprintf(stderr, "no time constraint policy: %s\n",
			mach_error_string(r));
#else
	(void)tick_ns;
#endif
}

#endif
