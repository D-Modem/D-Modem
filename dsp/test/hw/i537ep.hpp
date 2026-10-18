/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/i537ep.hpp - Intel's 537EP modem chip (8086:1080): a mixin of a
 *                    harness
 *
 *    Its registers are a memory BAR the driver reaches through four
 *    accessors of its own (read8, read32, write8, write32: base + offset,
 *    offsets up to 0x64), so here they are plain memory, i537_regs: the
 *    driver's iobase points at them.  What the chip does with them
 *    happens between the driver's calls (i537_sample(), i537_fix()):
 *
 *      0x01 bit 0     the DMA runs
 *      0x02           interrupt enables (bits 1, 2)
 *      0x04           interrupt status: 2 the DAC's ring, 4 the ADC's;
 *                     written back to clear (the driver acknowledges
 *                     what it read: i537_acked() after its handler)
 *      0x06 bit 7     the DAA busy (never)
 *      0x08 .. 0x14   the ADC's ring: base, end (its last word), the
 *                     interrupt point, the current word
 *      0x18 .. 0x24   the DAC's ring, the same
 *      0x35 bit 0     off hook
 *      0x37           the codec's rate: 7200, 8000, 57600/7, 8400, 9000,
 *                     9600, 72000/7, 16000, 96000/7 Hz by 0 to 8
 *      0x3c           bit 6 the codec's clock locked; bits 0..4 the loop
 *                     current (off hook)
 *
 *    Each ring is 32-bit words, a sample in the low 16 bits of each, in
 *    32 periods; a period's end (the interrupt point the driver sets
 *    after each interrupt) raises the ring's status bit.  The rings are
 *    the driver's own memory (the bus addresses are its pointers: a
 *    32-bit process).
 *
 *    The harness (Host, a half::HalfModem) runs each codec sample through
 *    i537_sample() and, when i537_irq(), the driver's interrupt.
 *
 */

#ifndef HW_I537EP_HPP
#define HW_I537EP_HPP

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class I537ep {
public:
	enum { I537_SIZE = 0x68 };
	/* the registers (u32 aligned: read32 and write32 at offsets of 4) */
	u32 i537_words[I537_SIZE / 4] = {};
	/* the registers as octets */
	byte *i537_regs() { return (byte *)i537_words; }

	/* the codec's rate (Hz) by 0x37 */
	double i537_rate() const
	{
		static const double hz[9] = { 7200, 8000, 57600.0 / 7, 8400, 9000,
					      9600, 72000.0 / 7, 16000,
					      96000.0 / 7 };
		unsigned c = reg(0x37);
		return c < 9 ? hz[c] : 9600;
	}

	/* the registers only the chip sets, as it has them (after each
	   call into the driver) */
	void i537_fix()
	{
		byte *r = i537_regs();
		r[0x06] &= 0x7f;
		r[0x3c] = 0x40 | ((r[0x35] & 1) ? 10 : 0);
		r[0x43] &= ~1;
		r[0x04] = status_;
	}

	/* an interrupt pending */
	bool i537_irq() const
	{
		return (status_ & reg(0x02) & 6) != 0;
	}

	/* the driver's handler acknowledged what it read */
	void i537_acked() { status_ = 0; i537_fix(); }

	/* a codec sample each way through the rings: the ADC's in, the
	   DAC's out */
	short i537_sample(short adc)
	{
		bool run = reg(0x01) & 1;
		short dac = 0;
		if (run && !run_) {
			/* (the DMA started: each ring from its base) */
			word32(0x14) = word32(0x08);
			word32(0x24) = word32(0x18);
			if (host().dbg)
				host().log("dma on: rx %08x..%08x int %08x, tx "
					   "%08x..%08x int %08x, rate %.1f\n",
					   word32(0x08), word32(0x0c),
					   word32(0x10), word32(0x18),
					   word32(0x1c), word32(0x20),
					   i537_rate());
		} else if (!run && run_ && host().dbg)
			host().log("dma off\n");
		run_ = run;
		if (!run)
			return 0;
		dac = (short)ring(0x18, 0, 2);
		ring(0x08, (u32)(unsigned short)adc, 4);
		return dac;
	}

private:
	u32 status_ = 0;
	bool run_ = false;

	/* the harness */
	Host &host() { return static_cast<Host &>(*this); }
	/* the register octet at o */
	byte reg(int o) const { return ((const byte *)i537_words)[o]; }
	/* the register word at o */
	u32 &word32(int o) { return i537_words[o / 4]; }

	/* the ring at registers b (base, end, interrupt point, current):
	   its current word written with v (the ADC's, bit 4) or read (the
	   DAC's, bit 2), the next, the interrupt at the point */
	u32 ring(int b, u32 v, u32 bit)
	{
		u32 base = word32(b), end = word32(b + 4), cur = word32(b + 12);
		u32 *p, out = 0;
		if (!base || cur < base || cur > end)
			cur = base;
		p = (u32 *)(uintptr_t)cur;
		if (b == 0x08)
			*p = v;
		else
			out = *p;
		cur = cur + 4 > end ? base : cur + 4;
		word32(b + 12) = cur;
		if (cur == word32(b + 8)) {
			status_ |= bit;
			((byte *)i537_words)[0x04] = (byte)status_;
		}
		return out;
	}
};

}

#endif
