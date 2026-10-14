/*
 *
 *    half_stack.h - the stack before a call into a closed module
 *                   (half_pctel, half_agere, half_sm56, half_hsf)
 *
 *    The modules read some of their stack before writing it: a status
 *    built there with values left unset (Agere's pumps' reports, SM56's
 *    dial response), locals of HSF's engine, a name copied there
 *    without its 0 (PCTel's +MS).  What they find is what the calls
 *    before left there, the harness's own among them (a read or write
 *    of the pty that the DTE's timing decides), so the same samples
 *    could give another call.  stack_clear() before each call into a
 *    module zeroes the stack below the caller's frame: the module finds
 *    the same there every time.  The modules ran on kernel stacks of 4
 *    or 8 KB, so their own frames lie in the 8 KB cleared (PCTel's
 *    calls go 988 octets deep at most); what goes deeper is the
 *    harness's, under them (SM56's port I/O signal frames, HSF's NVM
 *    file read).
 *
 *    HALF_STACK_FILL=<byte>: that instead of 0, to see whether a
 *    module's results depend on what it has not written.
 *
 */

#ifndef HALF_STACK_H
#define HALF_STACK_H

#include <stdlib.h>
#include <string.h>

#define STACK_CLEAR 8192

static __attribute__((noinline)) void stack_clear(void)
{
	static int fill = -1;
	char z[STACK_CLEAR];

	if (fill < 0)
		fill = getenv("HALF_STACK_FILL") ?
			atoi(getenv("HALF_STACK_FILL")) & 0xff : 0;
	memset(z, fill, sizeof(z));
	__asm__ volatile("" : : "r"(z) : "memory");
}

#endif
