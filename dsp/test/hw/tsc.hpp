/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/tsc.hpp - the time stamp counter as a driver reads it (rdtsc),
 *                 on the line's clock: a mixin of a harness (i386)
 *
 *    The line's time at 1 GHz, monotonic, and on by 0.1 us each read
 *    while the line stands still (so that a busy wait ends).  The
 *    harness (Host) has the line: line_n, its samples so far, and
 *    line_rate (a half::HalfModem's); tsc_trap() makes rdtsc fault
 *    (PR_TSC_SIGSEGV), and its SIGSEGV handler calls tsc_step().
 *
 */

#ifndef HW_TSC_HPP
#define HW_TSC_HPP

#include <stdint.h>
#include <sys/prctl.h>
#include <ucontext.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class LineTsc {
public:
	unsigned long long tsc_last = 0;        /* the last read */

	/* a read */
	unsigned long long tsc_read()
	{
		unsigned long long t = host().line_n *
			(1000000000ull / (unsigned long long)host().line_rate);
		if (t <= tsc_last)
			t = tsc_last + 100;
		return tsc_last = t;
	}

	/* rdtsc to fault (SIGSEGV); prctl's result */
	int tsc_trap() { return prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0); }

#if defined(__i386__)
	/* the instruction at the context's EIP, if rdtsc: done (a read),
	   EIP past it; false if not */
	bool tsc_step(ucontext_t *uc)
	{
		greg_t *g = uc->uc_mcontext.gregs;
		byte *ip = (byte *)(uintptr_t)g[REG_EIP];
		if (ip[0] != 0x0f || ip[1] != 0x31)
			return false;
		unsigned long long t = tsc_read();
		g[REG_EAX] = (greg_t)(u32)t;
		g[REG_EDX] = (greg_t)(u32)(t >> 32);
		g[REG_EIP] += 2;
		return true;
	}
#endif

private:
	Host &host() { return static_cast<Host &>(*this); }
};

}

#endif
