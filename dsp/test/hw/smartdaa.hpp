/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/smartdaa.hpp - Conexant's SmartDAA, behind the Conexant AC'97
 *                      modem codec's register 0x7a: a mixin of a harness
 *
 *    A write of 0x8000 | reg << 8 to the codec's 0x7a asks for one of
 *    the system side's (SSD's) registers (the answer, in 0x7a: reg << 8
 *    | its value), reg << 8 | value writes one.  Behind SSD registers 1
 *    to 4 are the line side chip's (LSD's), as Conexant's SOAR library
 *    drives them for the HSF codec (its kind 0x12, dmodemd/cx_daa.h):
 *    2 the byte to write, 3 the command (the LSD register in bits 5:0,
 *    bit 7 a write), 1 the LSD's answer (the register's byte after the
 *    command), 4 the status (bit 4 the command done).  SSD register 0
 *    has the link up (bit 1) and a bit toggling at each frame over the
 *    barrier (bit 2).  The LSD's version is its register 0x22 (the
 *    library takes 0 to 0x11); its register 2 reads 0x0f when it has
 *    powered up (the library waits for that after writing 3 and 4).
 *    The LSD's other registers read as written.  SSD register 0x0e,
 *    bits 5 and 6, are the loopbacks: the DAC's samples back to the ADC
 *    (the library measures the path's delay by them).
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
	enum {
		SSD_ANSWER = 1, SSD_DATA = 2, SSD_COMMAND = 3, SSD_STATUS = 4,
		ST_READY = 0x02, ST_TICK = 0x04, ST_DONE = 0x10,
		LSD_POWER = 0x02, LSD_VERSION = 0x22
	};

	byte smartdaa_regs[0x40] = { ST_READY };
	byte smartdaa_line_side[0x40] = {};

	/* the LSD powered up, its version 5 */
	SmartDaa()
	{
		smartdaa_line_side[LSD_POWER] = 0x0f;   /* powered up */
		smartdaa_line_side[LSD_VERSION] = 0x05;
	}

	/* a write of v to the codec's register 0x7a: a read of an SSD
	   register (v then the answer) or a write */
	void smartdaa_access(word &v)
	{
		int reg = (v >> 8) & 0x3f;
		if (v & 0x8000) {
			if (reg == 0)
				smartdaa_regs[0] ^= ST_TICK;
			if (host().dbg > 2)
				host().log("daa read %02x: %02x\n", reg, smartdaa_regs[reg]);
			v = (word)(reg << 8 | smartdaa_regs[reg]);
			return;
		}
		byte b = (byte)v;
		if ((smartdaa_regs[reg] != b && host().dbg) || host().dbg > 2)
			host().log("daa %02x = %02x\n", reg, b);
		if (reg == 0)
			b = (byte)((b & ~(ST_READY | ST_TICK)) |
				   (smartdaa_regs[0] & (ST_READY | ST_TICK)));
		smartdaa_regs[reg] = b;
		if (reg == SSD_COMMAND)
			command(b);
	}

	/* the LSD's registers but its status, power and version (0x02,
	   0x16, 0x1a, 0x1e, 0x1f, 0x22, 0x23), all f */
	void smartdaa_fill(byte f)
	{
		for (int r = 0; r < 0x40; r++)
			if (r != LSD_POWER && r != 0x16 && r != 0x1a &&
			    r != 0x1e && r != 0x1f && r != LSD_VERSION &&
			    r != 0x23)
				smartdaa_line_side[r] = f;
	}

	/* the DAC's samples back to the ADC (register 0x0e, bit 5 or 6) */
	bool smartdaa_loopback() const { return smartdaa_regs[0x0e] & 0x60; }

private:
	/* an LSD command: the register written with SSD register 2's byte
	   (bit 7) or read; its byte the answer; done at once */
	void command(byte c)
	{
		int lr = c & 0x3f;
		if (c & 0x80) {
			byte d = smartdaa_regs[SSD_DATA];
			if (host().dbg)
				host().log("line side %02x = %02x\n", lr, d);
			smartdaa_line_side[lr] = d;
		} else if (host().dbg > 1) {
			host().log("line side %02x: %02x\n", lr,
				   smartdaa_line_side[lr]);
		}
		smartdaa_regs[SSD_ANSWER] = smartdaa_line_side[lr];
		smartdaa_regs[SSD_STATUS] |= ST_DONE;
	}

	/* the harness */
	Host &host() { return static_cast<Host &>(*this); }
};

}

#endif
