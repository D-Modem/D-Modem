/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    cx_daa.cpp - a Conexant AC'97 modem codec's SmartDAA (cx_daa.h)
 *
 */

#include <cstdarg>
#include <cstdio>
#include <iterator>

#include "dm_types.h"
#include "cx_daa.h"

#include "cx_country.h"

namespace dmd {

namespace {

/* SOAR's flag word per kind (0 to 0x26); the bits that matter here:
   8 echo, 9 no wait for the link, 14 a read's command first, 19 the
   status in register 4 */
const u32 kind_flags[39] = {
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
const u32 kind_flags2[39] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0x001, 0x400, 0x400, 0x400,
	0x400, 0x400, 0x400, 0x480, 0x10a, 0x10a, 0x078, 0, 0, 0,
	0, 0x600, 0x600, 0x200, 0x200, 0, 0, 0x200, 0x200,
};

/* LSD registers of the line's control (power, hook, DC, ring), as
   SOAR's write has them */
bool control_reg(s16 reg)
{
	return reg == 0x03 || reg == 0x14 || reg == 0x15 ||
	       (reg >= 0x1c && reg <= 0x25);
}

/* the most status reads a command waits through */
const s16 MAX_POLLS = 2000;

} // namespace

/* see cx_daa.h */
CxVariant cx_variant(s16 kind)
{
	CxVariant v;

	if (kind < 0 || kind >= static_cast<s16>(sizeof(kind_flags) /
				      sizeof(kind_flags[0])))
		return v;
	u32 f = kind_flags[kind];
	v.echo = f & (1u << 8);
	v.no_ready = f & (1u << 9);
	v.cmd_first = f & (1u << 14);
	v.status4 = f & (UINT32_C(1) << 19);
	v.control_bit6 = kind_flags2[kind] & (1u << 4);
	return v;
}

/* see cx_daa.h: "CX" with a device 0x54xx, "RS" with 0x53xx, as
   hsfmc97ich.o takes them */
bool cx_codec(u16 id1, u16 id2)
{
	return (id1 == 0x4358 && (id2 & 0xff00) == 0x5400) ||
	       (id1 == 0x5253 && (id2 & 0xff00) == 0x5300);
}

/* see cx_daa.h: hsfmc97ich.o's table of DAA types (0x20 to 0x31),
   kind 5 for the others */
s16 cx_kind(u16 id2)
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
bool CxDaa::ssd_read(s16 reg, u8 *v)
{
	u16 x;

	reg &= 0x3f;
	if (reg == SSD_COMMAND)
		regs_.delay_us(40);
	if (!regs_.write(AC97_WINDOW, static_cast<u16>(0x8000 | reg << 8)))
		return fail("SSD %#x: codec register 0x7a not written", reg);
	if (reg == SSD_COMMAND)
		regs_.delay_us(40);
	if (!regs_.read(AC97_WINDOW, &x) || x == 0xffff)
		return fail("SSD %#x: no answer", reg);
	*v = static_cast<u8>(x);
	return true;
}

/* see cx_daa.h */
bool CxDaa::ssd_write(s16 reg, u8 v)
{
	reg &= 0x3f;
	if (reg == SSD_COMMAND)
		regs_.delay_us(40);
	if (!regs_.write(AC97_WINDOW, static_cast<u16>(reg << 8 | v)))
		return fail("SSD %#x: codec register 0x7a not written", reg);
	return true;
}

/* see cx_daa.h (private) */
bool CxDaa::clear_status(u8 mask)
{
	s16 st = v_.status4 ? 4 : 0;
	u8 s;

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
	s16 st = v_.status4 ? 4 : 0;
	s16 frames = 9;
	u8 s, tick = 0xff;

	for (s16 i = 0; i < MAX_POLLS; i++) {
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
	u8 s = 0;

	for (s16 i = 0; i < MAX_POLLS; i++)
		if (!ssd_read(0, &s) || (s & ST_READY))
			break;
	for (s16 i = 0; i < MAX_POLLS; i++)
		if (!ssd_read(0, &s) || (s & ST_TICK))
			break;
}

/* see cx_daa.h: up to 5 tries; with echo, an answer still the pattern
   (0x55, shifted at each try) is none */
bool CxDaa::lsd_read(s16 reg, u8 *v)
{
	u8 pattern = 0x55, x;

	reg &= 0x3f;
	for (s16 t = 0; t < 5; t++) {
		if (v_.echo && !ssd_write(SSD_ANSWER, pattern))
			return false;
		if (!v_.echo && !v_.no_ready)
			wait_ready();
		if (v_.cmd_first &&
		    !ssd_write(SSD_COMMAND, static_cast<u8>(reg)))
			return false;
		if (!clear_status(ST_DONE | ST_ERROR))
			return false;
		if (!v_.cmd_first &&
		    !ssd_write(SSD_COMMAND, static_cast<u8>(reg)))
			return false;
		if (!wait_done())
			continue;
		if (!ssd_read(SSD_ANSWER, &x))
			return false;
		if (v_.echo && x == pattern) {
			fail("LSD %#x: no answer (%#x)", reg, x);
			pattern = static_cast<u8>(pattern << 1);
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
bool CxDaa::lsd_write(s16 reg, u8 v)
{
	bool control = control_reg(reg & 0x3f);
	s16 tries = (reg & 0x3f) == 0x17 ? 1 : 5;
	u8 x, cmd;

	reg &= 0x3f;
	cmd = static_cast<u8>(reg | CMD_WRITE);
	if (control && v_.control_bit6)
		cmd |= CMD_CONTROL;
	for (s16 t = 0; t < tries; t++) {
		if (!ssd_write(SSD_DATA, v))
			return false;
		if (!v_.control_bit6) {
			u8 s;
			if (control &&
			    (!ssd_read(0x17, &s) ||
			     (!(s & 0x02) && !ssd_write(0x17, s | 0x02))))
				return false;
			if (!clear_status(ST_DONE | ST_ERROR))
				return false;
			if (v_.echo &&
			    !ssd_write(SSD_ANSWER, static_cast<u8>(~v)))
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
bool CxDaa::lsd_field(s16 reg, u8 mask, u8 val)
{
	u8 old;

	if (!lsd_read(reg, &old))
		return false;
	u8 v = static_cast<u8>((old & ~mask) | (val & mask));
	return v == old || lsd_write(reg, v);
}

/*
 *    The library's setup for kind 0x12, from its writes in the HSF
 *    harness (docs/hardware.md): twice, its line side's registers first
 *    all 0 and then all 0xff, which bits each write keeps.  W a write;
 *    F (old & keep) | set, written; B the same, written if it changes
 *    (the library's set and clear); on the SSD or the LSD.  VERSION
 *    writes the version back to LSD register 0x22, POWER powers the LSD
 *    up (wait_power()), DELAY waits set ms.  The board's bytes (SSD
 *    registers 0x06, 0x0b, 0x21) and the country's (LSD 0x12's bits
 *    6:5) are the HSF .inf's and the USA's in the tables, set_board()'s
 *    and set_country()'s when they run (param()).
 */

struct CxDaa::Op {
	enum Kind : u8 { W, F, B, VERSION, POWER, DELAY } kind;
	bool lsd;
	u8 reg, keep, set;
	/* set the board's or the country's instead (param()) */
	enum Param : u8 {
		NONE, SSD21, SSD0B, SSD06, IDLE, OFF,
		V19,            /* set, bit 5 too on LSD version 2 */
		V21,            /* set, bit 7 not on version 2 */
		NOT_V2          /* not on version 2 */
	} param;
};

namespace {

typedef u8 u8;
constexpr bool SSD = false, LSD = true;

#define SW(r, v)        { CxDaa::Op::W, SSD, r, 0, v, CxDaa::Op::NONE }
#define SF(r, k, v)     { CxDaa::Op::B, SSD, r, k, v, CxDaa::Op::NONE }
#define LW(r, v)        { CxDaa::Op::W, LSD, r, 0, v, CxDaa::Op::NONE }
#define LF(r, k, v)     { CxDaa::Op::F, LSD, r, k, v, CxDaa::Op::NONE }
#define LB(r, k, v)     { CxDaa::Op::B, LSD, r, k, v, CxDaa::Op::NONE }
/* the same, set the board's or the country's p */
#define SWP(r, v, p)    { CxDaa::Op::W, SSD, r, 0, v, CxDaa::Op::p }
#define SFP(r, k, v, p) { CxDaa::Op::B, SSD, r, k, v, CxDaa::Op::p }
#define LFP(r, k, v, p) { CxDaa::Op::F, LSD, r, k, v, CxDaa::Op::p }
#define LWP(r, v, p)    { CxDaa::Op::W, LSD, r, 0, v, CxDaa::Op::p }

} // namespace

/* power up: the SSD, the LSD's version read between (power_up()) */
static const CxDaa::Op ssd_start[] = {
	SF(0x1c, 0xbf, 0x40), SF(0x0c, 0xfe, 0x00), SF(0x17, 0xfb, 0x04),
	SF(0x14, 0xf7, 0x08), SWP(0x0b, 0x5f, SSD0B),
	SFP(0x06, 0x0f, 0xc0, SSD06),
	SF(0x19, 0xdf, 0x20), SW(0x05, 0x00), SW(0x04, 0x00),
	SW(0x07, 0x00), SW(0x08, 0x00), SW(0x09, 0x00), SW(0x0a, 0x00),
	SW(0x0d, 0x10), SW(0x1e, 0x00), SF(0x0f, 0x7f, 0x00),
	SF(0x14, 0x4c, 0x00), SF(0x19, 0x94, 0x28), SW(0x1c, 0x7f),
	SW(0x1d, 0x00), SW(0x1b, 0x00), SW(0x1a, 0x00), SW(0x25, 0x5a),
	SW(0x23, 0x43), SW(0x24, 0x80), SW(0x22, 0x5a), SW(0x20, 0x63),
	SWP(0x21, 0xa8, SSD21),
};
static const CxDaa::Op line_start[] = {
	SW(0x07, 0x00), SW(0x08, 0x00), SW(0x09, 0x00), SW(0x0a, 0x00),
	SW(0x0c, 0x14), SW(0x1d, 0x00), SW(0x1b, 0x00), SW(0x1a, 0x00),
	{ CxDaa::Op::VERSION, LSD, 0x22, 0, 0, CxDaa::Op::NONE },
	LB(0x1c, 0xfe, 0x00), { CxDaa::Op::POWER, LSD, 0x02, 0, 0, CxDaa::Op::NONE },
	LW(0x20, 0x00), LWP(0x25, 0x29, NOT_V2), LWP(0x24, 0x28, NOT_V2),
	LWP(0x21, 0x80, V21),
	LF(0x19, 0x7f, 0x80), LW(0x08, 0x00), LW(0x0a, 0xd9), LW(0x0e, 0x00),
	LW(0x0c, 0x07), LF(0x12, 0x9f, 0x00), LW(0x10, 0x00),
	LF(0x19, 0x3f, 0xc0), LW(0x1f, 0x08), LF(0x15, 0x2f, 0x40),
	LF(0x1c, 0x82, 0x78), LB(0x15, 0x68, 0x00), LF(0x14, 0x07, 0xe0),
	LF(0x07, 0xe0, 0x04), LW(0x18, 0x43), LWP(0x19, 0xcb, V19),
	LB(0x07, 0x64, 0x00), LB(0x14, 0xe3, 0x00), LB(0x15, 0x40, 0x00),
	LB(0x06, 0xe2, 0x00), LF(0x06, 0x60, 0x02), LW(0x0c, 0x06),
	LFP(0x12, 0x9f, 0x60, IDLE), LF(0x1c, 0x82, 0x30), LW(0x0e, 0x00),
	LW(0x18, 0x43), LW(0x10, 0x00), LF(0x1c, 0x82, 0x68),
	LB(0x1c, 0xfd, 0x02), LW(0x0c, 0x02), LW(0x0a, 0xdf), LW(0x10, 0x00),
	LW(0x0e, 0x02), LW(0x10, 0x00),
};
/* off hook (the answer's or the dial's): 0x15 = 0x41 a while first */
static const CxDaa::Op off_hook[] = {
	LW(0x15, 0x41), { CxDaa::Op::DELAY, LSD, 0, 0, 100, CxDaa::Op::NONE },
	LFP(0x12, 0x9f, 0x40, OFF), LW(0x0e, 0x00), LF(0x14, 0x03, 0x20),
	LW(0x18, 0x43), LW(0x10, 0x00), LWP(0x21, 0x82, V21),
	LF(0x1c, 0x80, 0x32), LW(0x15, 0x40), LW(0x15, 0x80), LWP(0x19, 0x8b, V19),
	SF(0x14, 0xfc, 0x01),
};
/* on hook, the line as power_up() left it */
static const CxDaa::Op on_hook[] = {
	SF(0x14, 0xfc, 0x00),
	LFP(0x12, 0x9f, 0x60, IDLE), LW(0x15, 0x40), LWP(0x19, 0xcb, V19),
	LW(0x0e, 0x00),
	LWP(0x21, 0x80, V21), LF(0x14, 0x03, 0xe0), LW(0x18, 0x43), LW(0x10, 0x00),
	LF(0x1c, 0x80, 0x6a), LW(0x10, 0x00), LW(0x0e, 0x02), LW(0x10, 0x00),
};

#undef SW
#undef SF
#undef LW
#undef LF
#undef LB
#undef SWP
#undef SFP
#undef LFP
#undef LWP

/* see cx_daa.h (private): the steps in turn, the first failure the end */
bool CxDaa::run(const Op *ops, s16 n)
{
	for (s16 i = 0; i < n; i++) {
		const Op &o = ops[i];
		u8 old = 0, v;

		if (o.param == Op::NOT_V2 && (version_ & 0x3f) == 2)
			continue;
		u8 set = o.param == Op::NOT_V2 ? o.set :
			      o.param ? param(o.param, o.set) : o.set;

		switch (o.kind) {
		case Op::VERSION:
			if (!lsd_write(LSD_VERSION, version_))
				return false;
			continue;
		case Op::POWER:
			if (!wait_power())
				return false;
			continue;
		case Op::DELAY:
			regs_.delay_us(u32{o.set} * 1000u);
			continue;
		case Op::W:
			v = set;
			break;
		default:
			if (!(o.lsd ? lsd_read(o.reg, &old) :
				      ssd_read(o.reg, &old)))
				return false;
			v = static_cast<u8>((old & o.keep) | set);
			if (o.kind == Op::B && v == old)
				continue;
			break;
		}
		if (!(o.lsd ? lsd_write(o.reg, v) : ssd_write(o.reg, v)))
			return false;
	}
	return true;
}

/* see cx_daa.h (private) */
u8 CxDaa::param(s16 p, u8 set) const
{
	bool v2 = (version_ & 0x3f) == 2;

	switch (p) {
	case Op::V19:   return static_cast<u8>(v2 ? set | 0x20 : set);
	case Op::V21:   return static_cast<u8>(v2 ? set & 0x7f : set);
	case Op::SSD21: return board_->ssd21();
	case Op::SSD0B: return board_->ssd0b();
	case Op::SSD06: return board_->ssd06();
	case Op::IDLE:  return static_cast<u8>(country_->idle << 5);
	case Op::OFF:   return static_cast<u8>(country_->off << 5);
	default:        return 0;
	}
}

/* see cx_daa.h (private): the HSF codec's (DAA type 0x29) */
const CxBoard *CxDaa::default_board()
{
	return &cx_board(0x29);
}

/* see cx_daa.h (private) */
const CxCountry *CxDaa::default_country()
{
	return &cx_usa();
}

/* see cx_daa.h */
void CxDaa::set_board(const CxBoard &b)
{
	board_ = &b;
}

/* see cx_daa.h: on hook the line's setting at once */
bool CxDaa::set_country(const CxCountry &c)
{
	country_ = &c;
	if (!setup_ || off_hook_)
		return true;
	static const Op idle = { Op::F, true, 0x12, 0x9f, 0, Op::IDLE };
	return run(&idle, 1);
}

/* see cx_daa.h (private): as the library, 3 = 0 and 4 = 0x2f again
   at each try, 50 tries 1 ms apart */
bool CxDaa::wait_power()
{
	u8 p = 0;

	for (s16 i = 0; i < 50; i++) {
		if (!lsd_write(0x03, 0x00) || !lsd_write(0x04, 0x2f) ||
		    !lsd_read(0x02, &p))
			return false;
		if (p == 0x0f)
			return true;
		regs_.delay_us(1000);
	}
	return fail("LSD: not powered up (register 2 %#x)", p);
}

/* see cx_daa.h */
bool CxDaa::power_up(u8 *version)
{
	setup_ = false;
	off_hook_ = false;
	if (kind_ != 0x12)
		return probe(version);
	if (!run(ssd_start, static_cast<s16>(std::size(ssd_start))) ||
	    !probe(&version_))
		return false;
	if ((version_ & 0x3f) > 0x11)
		return fail("LSD version %#x not one the library takes",
			    version_);
	*version = version_;
	if ((version_ & 0x3f) == 0 || (version_ & 0x3f) >= 0x10)
		return true;    /* the library's setup for it not known */
	if (!run(line_start, static_cast<s16>(std::size(line_start))))
		return false;
	setup_ = true;
	return true;
}

/* see cx_daa.h */
bool CxDaa::go_off_hook()
{
	off_hook_ = true;
	return setup_ ? run(off_hook, static_cast<s16>(std::size(off_hook)))
		      : set_hook(HOOK_OFF);
}

/* see cx_daa.h */
bool CxDaa::go_on_hook()
{
	off_hook_ = false;
	return setup_ ? run(on_hook, static_cast<s16>(std::size(on_hook)))
		      : set_hook(HOOK_ON);
}

/* see cx_daa.h */
bool CxDaa::probe(u8 *version)
{
	return lsd_read(LSD_VERSION, version);
}

/* see cx_daa.h: LSD versions 0x10 and 0x11 with bit 7 the other way
   (the library's 0xc0 on hook, 0x00 off) */
bool CxDaa::set_hook(Hook h)
{
	u8 b = h;

	if ((version_ & 0x3f) >= 0x10)
		b ^= 0x80;
	return lsd_field(LSD_HOOK, HOOK_MASK, b);
}

} // namespace dmd
