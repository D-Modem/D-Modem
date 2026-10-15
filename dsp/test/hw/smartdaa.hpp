/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/smartdaa.hpp - Conexant's SmartDAA, behind the Conexant AC'97
 *                      modem codec's register 0x7a: a mixin of a harness
 *
 *    A write of 0x8000 | reg << 8 to the codec's 0x7a asks for one of
 *    the DAA's registers (the answer, in 0x7a: reg << 8 | its value),
 *    reg << 8 | value writes one.  Behind the DAA's registers 1 (data)
 *    and 3 (bit 7 a read, the register below it) are the line side
 *    chip's.  Its register 0x0e, bits 5 and 6, are its loopbacks: the
 *    DAC's samples back to the ADC (the SmartDAA library measures the
 *    path's delay by them).
 *
 *    The harness (Host, a half::HalfModem) gives the codec's 0x7a
 *    writes to smartdaa_access() (its Ac97Codec's codec_write_hook) and
 *    loops the samples back while smartdaa_loopback().
 *
 */

#ifndef HW_SMARTDAA_HPP
#define HW_SMARTDAA_HPP

#include "../modem_half.hpp"

namespace half {

template <class Host>
class SmartDaa {
public:
	byte smartdaa_regs[0x80] = {};
	byte smartdaa_line_side[0x80] = {};

	/* a write of v to the codec's register 0x7a: a read of a DAA
	   register (v then the answer) or a write */
	void smartdaa_access(word &v)
	{
		int reg = (v >> 8) & 0x7f;
		if (v & 0x8000) {
			if (host().dbg > 2)
				host().log("daa read %02x: %02x\n", reg, smartdaa_regs[reg]);
			v = (word)(reg << 8 | smartdaa_regs[reg]);
			return;
		}
		if (smartdaa_regs[reg] != (byte)v && host().dbg)
			host().log("daa %02x = %02x\n", reg, v & 0xff);
		smartdaa_regs[reg] = (byte)v;
		if (reg == 0x03) {
			int lr = v & 0x7f;
			if (v & 0x80)
				smartdaa_regs[0x01] = smartdaa_line_side[lr];
			else {
				if (smartdaa_line_side[lr] != smartdaa_regs[0x01] && host().dbg)
					host().log("line side %02x = %02x\n", lr,
						   smartdaa_regs[0x01]);
				smartdaa_line_side[lr] = smartdaa_regs[0x01];
			}
		}
	}

	/* the DAC's samples back to the ADC (register 0x0e, bit 5 or 6) */
	bool smartdaa_loopback() const { return smartdaa_regs[0x0e] & 0x60; }

private:
	Host &host() { return static_cast<Host &>(*this); }
};

}

#endif
