/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/si3034.hpp - a Silicon Labs Si3034 DAA behind a controller's
 *                    serial port: a mixin of a harness
 *
 *    Its registers by commands (register in bits 8..12, 0x2000 a read,
 *    the data in bits 0..7), a read's result at si3034_data; its PLL
 *    (registers 7 to 9) sets its sample rate on a 4 MHz clock, register
 *    5 bit 0 its hook, register 12 its line side's frame detect and loop
 *    current, register 13's bit 6 the chip: set for an Si3034
 *    (international), clear for an Si3035 (the USA only), as
 *    si3034_intl says.
 *
 *    The harness (Host, a half::HalfModem) has its controller give the
 *    commands to si3034_cmd(); the codec runs at si3034_rate.
 *
 */

#ifndef HW_SI3034_HPP
#define HW_SI3034_HPP

#include <math.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class Si3034 {
public:
	byte si3034_regs[32] = {};
	u32 si3034_data = 0;            /* the last read's result */
	unsigned si3034_rate = 8000;    /* its sample rate */
	bool si3034_off_hook = false;
	bool si3034_intl = false;       /* an Si3034, not an Si3035 */

	/* a command: a register read or write */
	void si3034_cmd(u32 v)
	{
		int r = (v >> 8) & 0x1f;
		if (v & 0x2000) {
			si3034_data = si3034_regs[r];
			if (r == 12)    /* line side: frame detect, loop current
					   (half the scale: 1111 is an overload) */
				si3034_data = 0x40 | (si3034_off_hook ? 0x08 : 0);
			if (r == 13)    /* line side revision: international */
				si3034_data = (si3034_regs[r] & ~0x40u) |
					(si3034_intl ? 0x40 : 0);
			if (host().dbg > 1)
				host().log("daa %2d r = %02x\n", r, si3034_data);
			return;
		}
		if (host().dbg > 1 || (host().dbg && r != 5 && si3034_regs[r] != (byte)v))
			host().log("daa %2d w = %02x\n", r, v & 0xff);
		si3034_regs[r] = (byte)v;
		if (r >= 7 && r <= 9)
			pll();
		if (r == 5 && (bool)(v & 1) != si3034_off_hook) {
			si3034_off_hook = v & 1;
			if (host().dbg)
				host().log("%s hook\n", si3034_off_hook ? "off" : "on");
		}
	}

private:
	/* the harness */
	Host &host() { return static_cast<Host &>(*this); }

	/* its sample rate from its PLL (registers 7 to 9: N1 - 1, M1 - 1,
	   and N2 - 1, M2 - 1 in the high and low nibbles), on a 4 MHz
	   clock: 4 MHz M1 M2 / (N1 N2) / 5120 */
	void pll()
	{
		const byte *d = si3034_regs;
		double f = 4000000.0 * (d[8] + 1) / (d[7] + 1) *
			((d[9] & 15) + 1) / ((d[9] >> 4) + 1) / 5120;
		unsigned r = (unsigned)lrint(f);
		if (r >= 4000 && r <= 48000 && r != si3034_rate) {
			if (host().dbg)
				host().log("daa rate %u Hz\n", r);
			si3034_rate = r;
		}
	}
};

}

#endif
