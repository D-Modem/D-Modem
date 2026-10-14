/*
 *
 *    hw/x86_traps.hpp - the instructions of a driver's own code that
 *                       trap in a process (no I/O permission): in and
 *                       out, cli and sti, mov to and from a control
 *                       register; a mixin of a harness (i386)
 *
 *    The harness (Host) has its port I/O: u32 io_in(unsigned port, int
 *    size) and void io_out(unsigned port, u32 v, int size), and from its
 *    SIGSEGV handler calls traps_step(): the instruction at the fault
 *    carried out and stepped over.  CR0 reads with TS clear (the FPU
 *    ours to use), CR4 with OSFXSR; cli and sti do nothing (one thread).
 *    rdtsc is hw/tsc.hpp's.
 *
 */

#ifndef HW_X86_TRAPS_HPP
#define HW_X86_TRAPS_HPP

#if defined(__i386__)

#include <stdint.h>
#include <ucontext.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class X86Traps {
public:
	/* the instruction at the context's EIP, if one of those: done, EIP
	   past it; false if not */
	bool traps_step(ucontext_t *uc);

private:
	u32 cr_[8] = { 0x80050033, 0, 0, 0, 0x000006f0 };

	Host &host() { return static_cast<Host &>(*this); }
};

template <class Host>
bool X86Traps<Host>::traps_step(ucontext_t *uc)
{
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];
	int size = 4, n = 1;
	unsigned port = (unsigned)g[REG_EDX] & 0xffff;

	if (ip[0] == 0x0f && (ip[1] == 0x20 || ip[1] == 0x22) &&
	    (ip[2] & 0xc0) == 0xc0) {
		/* mov from and to a control register */
		static const int greg[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX,
					     REG_ESP, REG_EBP, REG_ESI, REG_EDI };
		int c = (ip[2] >> 3) & 7, r = greg[ip[2] & 7];
		if (ip[1] == 0x20)
			g[r] = (greg_t)cr_[c];
		else
			cr_[c] = (u32)g[r];
		g[REG_EIP] += 3;
		return true;
	}
	if (ip[0] == 0x66) {
		size = 2;
		ip++;
		n++;
	}
	switch (ip[0]) {
	case 0xfa:      /* cli, sti: one thread, nothing to keep out */
	case 0xfb:
		break;
	case 0xe4:      /* in al, imm8; in eax, imm8; out imm8, al/eax */
		g[REG_EAX] = (g[REG_EAX] & ~0xff) | host().io_in(ip[1], 1);
		n++;
		break;
	case 0xe5:
		g[REG_EAX] = size == 2 ? (g[REG_EAX] & ~0xffff) | host().io_in(ip[1], 2) :
			host().io_in(ip[1], 4);
		n++;
		break;
	case 0xe6:
		host().io_out(ip[1], g[REG_EAX] & 0xff, 1);
		n++;
		break;
	case 0xe7:
		host().io_out(ip[1], size == 2 ? g[REG_EAX] & 0xffff : (u32)g[REG_EAX],
			      size);
		n++;
		break;
	case 0xec:      /* in al, dx; in ax/eax, dx; out dx, al/ax/eax */
		g[REG_EAX] = (g[REG_EAX] & ~0xff) | host().io_in(port, 1);
		break;
	case 0xed:
		if (size == 2)
			g[REG_EAX] = (g[REG_EAX] & ~0xffff) | host().io_in(port, 2);
		else
			g[REG_EAX] = host().io_in(port, 4);
		break;
	case 0xee:
		host().io_out(port, g[REG_EAX] & 0xff, 1);
		break;
	case 0xef:
		host().io_out(port, size == 2 ? g[REG_EAX] & 0xffff : (u32)g[REG_EAX],
			      size);
		break;
	default:
		return false;
	}
	g[REG_EIP] += n;
	return true;
}

}

#endif

#endif
