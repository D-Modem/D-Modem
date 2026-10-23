/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    cx_daa.cpp - a Conexant AC'97 modem codec's SmartDAA (cx_daa.h)
 *
 */

#include <stdarg.h>
#include <stdio.h>

#include "cx_daa.h"

namespace dmd {

namespace {

/* SOAR's flag word per kind (0 to 0x26); the bits that matter here:
   8 echo, 9 no wait for the link, 14 a read's command first, 19 the
   status in register 4 */
const uint32_t kind_flags[39] = {
	0x01000000, 0x01000000, 0x01000000, 0x0000f840, 0x00004040,
	0x004040c4, 0x004040c4, 0x004042c4, 0x3000f840, 0x3000f840,
	0x00000000, 0x00003836, 0x004000d6, 0x004000d6, 0x00002057,
	0x0000395f, 0x00063957, 0x084e21df, 0x084e21df, 0x07402157,
	0x07402157, 0x47402157, 0x47402157, 0xc7402157, 0x43022955,
	0x43022955, 0xc3402157, 0x00000000, 0x00000000, 0x002a0150,
	0x00aa8150, 0x87402057, 0x87402057, 0x87402057, 0x87402057,
	0x00000000, 0x00000000, 0x87402057, 0x87402057,
};

/* SOAR's second flag word per kind; the bit that matters here: 4 the
   line's control registers written with the command's bit 6 */
const uint32_t kind_flags2[39] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0x001, 0x400, 0x400, 0x400,
	0x400, 0x400, 0x400, 0x480, 0x10a, 0x10a, 0x078, 0, 0, 0,
	0, 0x600, 0x600, 0x200, 0x200, 0, 0, 0x200, 0x200,
};

/* LSD registers of the line's control (power, hook, DC, ring), as
   SOAR's write has them */
bool control_reg(int reg)
{
	return reg == 0x03 || reg == 0x14 || reg == 0x15 ||
	       (reg >= 0x1c && reg <= 0x25);
}

/* the most status reads a command waits through */
const int MAX_POLLS = 2000;

} // namespace

/* see cx_daa.h */
CxVariant cx_variant(int kind)
{
	CxVariant v;

	if (kind < 0 || kind >= (int)(sizeof(kind_flags) /
				      sizeof(kind_flags[0])))
		return v;
	uint32_t f = kind_flags[kind];
	v.echo = f & (1u << 8);
	v.no_ready = f & (1u << 9);
	v.cmd_first = f & (1u << 14);
	v.status4 = f & (1u << 19);
	v.control_bit6 = kind_flags2[kind] & (1u << 4);
	return v;
}

/* see cx_daa.h: "CX" with a device 0x54xx, "RS" with 0x53xx, as
   hsfmc97ich.o takes them */
bool cx_codec(uint16_t id1, uint16_t id2)
{
	return (id1 == 0x4358 && (id2 & 0xff00) == 0x5400) ||
	       (id1 == 0x5253 && (id2 & 0xff00) == 0x5300);
}

/* see cx_daa.h: hsfmc97ich.o's table of DAA types (0x20 to 0x31),
   kind 5 for the others */
int cx_kind(uint16_t id2)
{
	switch (id2 & 0xff) {
	case 0x21: return 0x0c;
	case 0x22: return 0x13;
	case 0x23: return 0x15;
	case 0x24: return -1;
	case 0x28: return 0x0d;
	case 0x29: return 0x12;
	case 0x30: return 0x17;
	case 0x31: return 0x20;
	default: return 0x05;
	}
}

/* the failure's text kept; false */
bool CxDaa::fail(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	error_ = buf;
	return false;
}

/* see cx_daa.h */
bool CxDaa::ssd_read(int reg, uint8_t *v)
{
	uint16_t x;

	reg &= 0x3f;
	if (reg == SSD_COMMAND)
		regs_.delay_us(40);
	if (!regs_.write(AC97_WINDOW, (uint16_t)(0x8000 | reg << 8)))
		return fail("SSD %#x: codec register 0x7a not written", reg);
	if (reg == SSD_COMMAND)
		regs_.delay_us(40);
	if (!regs_.read(AC97_WINDOW, &x) || x == 0xffff)
		return fail("SSD %#x: no answer", reg);
	*v = (uint8_t)x;
	return true;
}

/* see cx_daa.h */
bool CxDaa::ssd_write(int reg, uint8_t v)
{
	reg &= 0x3f;
	if (reg == SSD_COMMAND)
		regs_.delay_us(40);
	if (!regs_.write(AC97_WINDOW, (uint16_t)(reg << 8 | v)))
		return fail("SSD %#x: codec register 0x7a not written", reg);
	return true;
}

/* see cx_daa.h (private) */
bool CxDaa::clear_status(uint8_t mask)
{
	int st = v_.status4 ? 4 : 0;
	uint8_t s;

	if (!ssd_read(st, &s))
		return false;
	if (s & mask)
		return ssd_write(st, s & ~mask);
	return true;
}

/* see cx_daa.h (private): the status polled, its frame bit (SSD
   register 0's bit 2) counted */
bool CxDaa::wait_done()
{
	int st = v_.status4 ? 4 : 0;
	int frames = 9;
	uint8_t s, tick = 0xff;

	for (int i = 0; i < MAX_POLLS; i++) {
		if (!ssd_read(st, &s))
			return false;
		if (s & ST_DONE)
			return true;
		if (s & ST_ERROR)
			return fail("LSD: an error over the barrier");
		if (st != 0 && !ssd_read(0, &s))
			return false;
		if ((s & ST_TICK) != tick) {
			if (tick != 0xff && --frames == 0)
				break;
			tick = s & ST_TICK;
		}
	}
	return fail("LSD: no answer within 9 frames");
}

/* see cx_daa.h (private) */
void CxDaa::wait_ready()
{
	uint8_t s = 0;

	for (int i = 0; i < MAX_POLLS; i++)
		if (!ssd_read(0, &s) || (s & ST_READY))
			break;
	for (int i = 0; i < MAX_POLLS; i++)
		if (!ssd_read(0, &s) || (s & ST_TICK))
			break;
}

/* see cx_daa.h: up to 5 tries; with echo, an answer still the pattern
   (0x55, shifted at each try) is none */
bool CxDaa::lsd_read(int reg, uint8_t *v)
{
	uint8_t pattern = 0x55, x;

	reg &= 0x3f;
	for (int t = 0; t < 5; t++) {
		if (v_.echo && !ssd_write(SSD_ANSWER, pattern))
			return false;
		if (!v_.echo && !v_.no_ready)
			wait_ready();
		if (v_.cmd_first && !ssd_write(SSD_COMMAND, (uint8_t)reg))
			return false;
		if (!clear_status(ST_DONE | ST_ERROR))
			return false;
		if (!v_.cmd_first && !ssd_write(SSD_COMMAND, (uint8_t)reg))
			return false;
		if (!wait_done())
			continue;
		if (!ssd_read(SSD_ANSWER, &x))
			return false;
		if (v_.echo && x == pattern) {
			fail("LSD %#x: no answer (%#x)", reg, x);
			pattern = (uint8_t)(pattern << 1);
			continue;
		}
		*v = x;
		return true;
	}
	if (error_.empty())
		fail("LSD %#x: not read", reg);
	return false;
}

/* see cx_daa.h: as SOAR's: with control_bit6, the byte and the
   command alone; else a control register's write sets SSD register
   0x17's bit 1 first, the status is cleared, with echo SSD register 1
   primed with the byte's inverse */
bool CxDaa::lsd_write(int reg, uint8_t v)
{
	bool control = control_reg(reg & 0x3f);
	int tries = (reg & 0x3f) == 0x17 ? 1 : 5;
	uint8_t x, cmd;

	reg &= 0x3f;
	cmd = (uint8_t)(reg | CMD_WRITE);
	if (control && v_.control_bit6)
		cmd |= CMD_CONTROL;
	for (int t = 0; t < tries; t++) {
		if (!ssd_write(SSD_DATA, v))
			return false;
		if (!v_.control_bit6) {
			uint8_t s;
			if (control &&
			    (!ssd_read(0x17, &s) ||
			     (!(s & 0x02) && !ssd_write(0x17, s | 0x02))))
				return false;
			if (!clear_status(ST_DONE | ST_ERROR))
				return false;
			if (v_.echo && !ssd_write(SSD_ANSWER, (uint8_t)~v))
				return false;
		}
		if (!ssd_write(SSD_COMMAND, cmd))
			return false;
		if (!wait_done())
			continue;
		if (!ssd_read(SSD_ANSWER, &x))
			return false;
		if (x == v)
			return true;
		fail("LSD %#x: wrote %#x, it answered %#x", reg, v, x);
	}
	return false;
}

/* see cx_daa.h */
bool CxDaa::lsd_field(int reg, uint8_t mask, uint8_t val)
{
	uint8_t old;

	if (!lsd_read(reg, &old))
		return false;
	uint8_t v = (uint8_t)((old & ~mask) | (val & mask));
	return v == old || lsd_write(reg, v);
}

/* see cx_daa.h */
bool CxDaa::probe(uint8_t *version)
{
	return lsd_read(LSD_VERSION, version);
}

/* see cx_daa.h */
bool CxDaa::set_hook(Hook h)
{
	return lsd_field(LSD_HOOK, HOOK_MASK, h);
}

} // namespace dmd
