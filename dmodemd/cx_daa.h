/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    cx_daa.h - the line of a Conexant (Rockwell) AC'97 modem codec: its
 *               SmartDAA's hook, which is not the AC'97 modem GPIO the
 *               kernel's "Off-hook Switch" drives
 *
 *    The SmartDAA is two chips: the system side (SSD) in the codec and
 *    the line side (LSD) beyond the isolation barrier.  Their registers
 *    are bytes, 0 to 63, found in Conexant's HSF driver for ICH modems
 *    (hsfmc97ich.o, hsfsoar.o; docs/hardware.md):
 *
 *    - an SSD register through the codec's register 0x7a: written with
 *      (reg << 8) | value, read with 0x8000 | (reg << 8) written and
 *      0x7a read again (its low byte; 0xffff, none).  SSD register 3
 *      wants 40 us before each access to it and before its reading;
 *    - an LSD register through four SSD ones: 2 the byte to write, 3 the
 *      command (the LSD register in bits 5:0; bit 7 a write), 1 the
 *      LSD's answer, and a status register (0, or 4 on some SSDs): bit 4
 *      done, bit 3 an error (both cleared before each command), bit 2
 *      toggling at each frame over the barrier, bit 1 the link up.  A
 *      write to one of the line's control registers (0x03, 0x14, 0x15,
 *      0x1c to 0x25) first sets SSD register 0x17's bit 1; on one SSD
 *      (SOAR's kind 0x1a) it has the command's bit 6 instead, and no
 *      status cleared.  A write is good when the answer is the byte
 *      written;
 *    - the hook: LSD register 0x15, bits 7:6: 01 on hook, 10 off hook,
 *      00 a dial pulse's make, 11 its break.
 *
 *    The SSDs differ (Conexant's SOAR library has 39 kinds): CxVariant
 *    holds the differences in the LSD's access.
 *
 */
#ifndef DMODEMD_CX_DAA_H
#define DMODEMD_CX_DAA_H

#include <stdint.h>

#include <string>

namespace dmd {

/* an AC'97 codec's registers (16 bit, at even numbers to 0x7e) */
class Ac97Regs {
public:
	virtual ~Ac97Regs() = default;
	/* register reg into *v, or written with v; false on an error */
	virtual bool read(int reg, uint16_t *v) = 0;
	virtual bool write(int reg, uint16_t v) = 0;
	/* at least us microseconds gone */
	virtual void delay_us(unsigned us) = 0;
};

/* how an SSD gives its LSD's registers (SOAR's flags per kind) */
struct CxVariant {
	bool echo = false;      /* SSD register 1 primed before each
				   command (a read: a pattern the answer
				   must not be; a write: the byte's
				   inverse), an absent LSD seen */
	bool no_ready = false;  /* a read does not first wait for the
				   link (status bits 1, then 2) */
	bool cmd_first = false; /* a read's command before the status
				   is cleared */
	bool status4 = false;   /* the status in SSD register 4, not 0 */
	bool control_bit6 = false; /* a control register's write: the
				   command's bit 6, not SSD register
				   0x17's bit 1; the status not cleared,
				   SSD register 1 not primed */
};

/* SOAR's kind kind's differences (0 to 0x26); the plain one for others */
CxVariant cx_variant(int kind);

/* whether the codec's vendor ID (registers 0x7c, 0x7e) is a Conexant
   or Rockwell modem codec with a SmartDAA */
bool cx_codec(uint16_t id1, uint16_t id2);

/* the SSD's kind of a cx_codec() codec, from register 0x7e's low byte
   (its DAA type) as Conexant's ICH driver takes it; -1 for type 0x24,
   whose SSD is in the codec's own registers, not behind 0x7a */
int cx_kind(uint16_t id2);

class CxDaa {
public:
	/* LSD register 0x15's hook states (bits 7:6) */
	enum Hook : uint8_t {
		HOOK_ON = 0x40, HOOK_OFF = 0x80, PULSE_MAKE = 0x00,
		PULSE_BREAK = 0xc0
	};
	enum {
		LSD_HOOK = 0x15, HOOK_MASK = 0xc0,
		LSD_VERSION = 0x22,
		SSD_ANSWER = 1, SSD_DATA = 2, SSD_COMMAND = 3,
		CMD_WRITE = 0x80, CMD_CONTROL = 0x40,
		ST_READY = 0x02, ST_TICK = 0x04, ST_ERROR = 0x08,
		ST_DONE = 0x10,
		/* the codec register the SSD's are behind */
		AC97_WINDOW = 0x7a
	};

	/* the SmartDAA behind regs's register 0x7a, an SSD of kind v */
	explicit CxDaa(Ac97Regs &regs, CxVariant v = CxVariant())
		: regs_(regs), v_(v) {}

	/* an SSD register into *v, or written with v */
	bool ssd_read(int reg, uint8_t *v);
	bool ssd_write(int reg, uint8_t v);
	/* an LSD register into *v (from the line side, not a copy), or
	   written with v (the answer checked; tried again 4 times, LSD
	   register 0x17 once) */
	bool lsd_read(int reg, uint8_t *v);
	bool lsd_write(int reg, uint8_t v);
	/* the bits mask of LSD register reg made val's, the rest kept;
	   not written if they are already */
	bool lsd_field(int reg, uint8_t mask, uint8_t val);
	/* whether the line side answers: its version (LSD register 0x22)
	   into *version */
	bool probe(uint8_t *version);
	/* the hook (LSD register 0x15's bits 7:6), its other bits kept */
	bool set_hook(Hook h);
	/* what the last failure was */
	const std::string &error() const { return error_; }

private:
	/* the status register's bits clear mask cleared */
	bool clear_status(uint8_t mask);
	/* the command's end: true if done, false on its error or no end
	   within 9 frames */
	bool wait_done();
	/* for read(): the link up (status bits 1, then 2), a while at most */
	void wait_ready();
	bool fail(const char *fmt, ...);

	Ac97Regs &regs_;
	CxVariant v_;
	std::string error_;
};

} // namespace dmd

#endif
