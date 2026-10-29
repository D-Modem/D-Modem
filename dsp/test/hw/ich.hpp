/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/ich.hpp - the Intel ICH AC'97 modem controller (8086:2416), as
 *                 the soft modems drive it: a mixin of a harness
 *
 *    Its codec's registers at NAMBAR (the primary codec's at 0, a
 *    secondary's at 0x80); at NABMBAR its two bus masters, PCM in (to the
 *    modem) at 0x00 and PCM out at 0x10 (BDBAR, CIV, LVI, SR, PICB, CR),
 *    GLOB_CNT at 0x3c and GLOB_STA at 0x40.  Each bus master walks its
 *    buffer descriptor list a sample at a time; a buffer done with IOC,
 *    or the last valid one, sets its status, and its interrupt is a
 *    status bit whose enable is set (a level: up while the bit is).  At
 *    the last valid buffer it halts, and goes on with the next once LVI
 *    moves; paused (RPBM cleared) it keeps its place.
 *
 *    The harness (Host, a half::HalfModem) derives from Ich<Host> and
 *    has the codec: word codec_read(int r) and void codec_write(int r,
 *    word v) (an Ac97Codec<Host>'s, hw/ac97_codec.hpp).  Its port I/O
 *    goes to ich_io_in() and ich_io_out(), and each codec sample through
 *    ich_sample() of each bus master.
 *
 */

#ifndef HW_ICH_HPP
#define HW_ICH_HPP

#include <stdint.h>
#include <stdio.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class Ich {
public:
	enum {
		SR_DCH = 0x01, SR_CELV = 0x02, SR_LVBCI = 0x04, SR_BCIS = 0x08,
		SR_FIFOE = 0x10,
		CR_RPBM = 0x01, CR_RR = 0x02, CR_LVBIE = 0x04, CR_FEIE = 0x08,
		CR_IOCE = 0x10,
	};
	struct BusMaster {
		u32 bdbar = 0;
		byte civ = 0, lvi = 0, cr = 0;
		word sr = SR_DCH, picb = 0;
		int pos = 0;            /* samples done in the current buffer */
		bool fresh = true;      /* not run since a reset */
	};

	BusMaster pcm_in, pcm_out;
	u32 glob_cnt = 0, glob_sta;

	/* nambar and nabmbar the ports of the two BARs; secondary: the
	   codec's registers at NAMBAR + 0x80 (and GLOB_STA's primary and
	   secondary ready), else at NAMBAR; bus: what a bus address is
	   short of the address (the SM56 module's kernel puts memory at
	   0x02000000 on) */
	Ich(unsigned nambar, unsigned nabmbar, bool secondary, uintptr_t bus) :
		glob_sta(secondary ? 0x300 : 0x100), nambar_(nambar),
		nabmbar_(nabmbar), codec_at_(secondary ? 0x80 : 0), bus_(bus)
	{
	}

	/* a port read of size bytes at a: the codec's and the controller's
	   registers; false if a is not the controller's */
	bool ich_io_in(unsigned a, int size, u32 *v);
	/* a port write */
	bool ich_io_out(unsigned a, u32 v, int size);

	/* a codec sample through a running bus master: in to memory (PCM
	   in), or out of it (PCM out) */
	short ich_sample(BusMaster &b, short in);
	/* b's interrupt; either's */
	bool ich_irq(const BusMaster &b) const
	{
		return ((b.sr & SR_BCIS) && (b.cr & CR_IOCE)) ||
			((b.sr & SR_LVBCI) && (b.cr & CR_LVBIE)) ||
			((b.sr & SR_FIFOE) && (b.cr & CR_FEIE));
	}
	/* either bus master's interrupt */
	bool ich_irq() const { return ich_irq(pcm_in) || ich_irq(pcm_out); }
	/* GLOB_STA, its PCM in and out interrupt bits as the bus masters
	   are */
	u32 ich_status() const
	{
		return (glob_sta & ~6u) | (ich_irq(pcm_in) ? 2 : 0) |
			(ich_irq(pcm_out) ? 4 : 0);
	}
	/* a running bus master at the start of a buffer: none of it done */
	bool ich_at_start(const BusMaster &b) const
	{
		return (b.cr & CR_RPBM) && !(b.sr & SR_DCH) && b.bdbar &&
			b.picb == (bd(b, b.civ)[1] & 0xffff);
	}

private:
	const unsigned nambar_, nabmbar_, codec_at_;
	const uintptr_t bus_;

	/* the harness */
	Host &host() { return static_cast<Host &>(*this); }
	/* the harness */
	const Host &host() const { return static_cast<const Host &>(*this); }
	/* b's name: "pcm in" or "pcm out" */
	const char *name(const BusMaster &b) const
	{
		return &b == &pcm_in ? "pcm in" : "pcm out";
	}

	/* buffer descriptor i of b's list: the buffer's bus address, its
	   length in samples | IOC (bit 31) */
	u32 *bd(const BusMaster &b, int i) const
	{
		return (u32 *)(uintptr_t)(b.bdbar + 8 * (i & 31) + bus_);
	}
	/* the current descriptor's length into PICB, from its start */
	void load(BusMaster &b)
	{
		b.picb = (word)(bd(b, b.civ)[1] & 0xffff);
		b.pos = 0;
	}
	void control(BusMaster &b, byte v);
};

/* a write of the control register: reset (RR), run (RPBM: from the
   current descriptor after a reset, on where it was after a pause, as
   RPBM keeps the state; the HSF module pauses it to move LVI on), stop
   (DCH set) */
template <class Host>
void Ich<Host>::control(BusMaster &b, byte v)
{
	if (host().dbg > 1)
		host().log("%s cr %02x\n", &b == &pcm_in ? "pi" : "po", v);
	if (v & CR_RR) {
		b.civ = b.lvi = 0;
		b.picb = 0;
		b.pos = 0;
		b.sr = SR_DCH;
		b.cr = 0;
		b.fresh = true;
		return;
	}
	if ((v & CR_RPBM) && !(b.cr & CR_RPBM) && !b.fresh) {
		b.sr &= ~SR_DCH;
		if (b.picb == 0 && b.civ != b.lvi) {
			b.civ = (b.civ + 1) & 31;
			b.sr &= ~SR_CELV;
			load(b);
		}
		b.cr = v & 0x1f;
		return;
	}
	if ((v & CR_RPBM) && !(b.cr & CR_RPBM)) {
		b.fresh = false;
		if (host().dbg) {
			host().log("%s runs: lvi %u:", name(b), b.lvi);
			for (int i = 0; i < 32 && b.bdbar; i++)
				if (bd(b, i)[1] & 0xffff)
					fprintf(stderr, " %x", bd(b, i)[1]);
			fprintf(stderr, "\n");
		}
		b.sr &= ~(SR_DCH | SR_CELV);
		if (b.bdbar)
			load(b);
	}
	if (!(v & CR_RPBM))
		b.sr |= SR_DCH;
	b.cr = v & 0x1f;
}

/* a codec sample through b while it runs: in to its buffer (PCM
   in) or its buffer's out (PCM out); on to the next descriptor,
   halted at the last valid one */
template <class Host>
short Ich<Host>::ich_sample(BusMaster &b, short in)
{
	short out = 0;
	u32 *d;
	short *buf;

	if ((b.cr & CR_RPBM) && (b.sr & SR_CELV) && b.civ != b.lvi) {
		/* halted at the last valid buffer, and there is another
		   (LVI moved): on with it, as the ICH does */
		b.civ = (b.civ + 1) & 31;
		b.sr &= ~(SR_CELV | SR_DCH);
		load(b);
	}
	if (!(b.cr & CR_RPBM) || (b.sr & SR_DCH) || !b.bdbar)
		return 0;
	d = bd(b, b.civ);
	buf = (short *)(uintptr_t)(d[0] + bus_);
	if (b.pos < (int)(d[1] & 0xffff) && d[0]) {
		if (&b == &pcm_in)
			buf[b.pos] = in;
		else
			out = buf[b.pos];
	}
	b.pos++;
	if (b.picb)
		b.picb--;
	if (b.picb == 0) {
		if (d[1] & 0x80000000)
			b.sr |= SR_BCIS;
		if (b.civ == b.lvi) {
			if (host().dbg)
				host().log("%s halted at %llu ms (civ %u)\n", name(b),
					   host().ms(), b.civ);
			b.sr |= SR_LVBCI | SR_CELV | SR_DCH;
		} else {
			b.civ = (b.civ + 1) & 31;
			load(b);
		}
	}
	return out;
}

/* a port read: the codec's registers at NAMBAR, the bus masters'
   and GLOB_CNT, GLOB_STA at NABMBAR; false if a is neither */
template <class Host>
bool Ich<Host>::ich_io_in(unsigned a, int size, u32 *v)
{
	u32 r = 0xffffffff;

	if (a >= nambar_ && a < nambar_ + 0x100) {
		unsigned o = a - nambar_;
		if (!codec_at_)
			r = host().codec_read(o);
		else
			r = o >= codec_at_ ? host().codec_read(o - codec_at_) : 0xffff;
	} else if (a >= nabmbar_ && a < nabmbar_ + 0x80) {
		unsigned o = a - nabmbar_;
		BusMaster *b = o < 0x10 ? &pcm_in : o < 0x20 ? &pcm_out : nullptr;
		if (b) {
			switch (o & 0xf) {
			case 0x0: r = b->bdbar; break;
			case 0x4: r = b->civ | b->lvi << 8 | (u32)b->sr << 16; break;
			case 0x5: r = b->lvi; break;
			case 0x6: r = b->sr; break;
			case 0x8: r = b->picb; break;
			case 0xa: r = (b->civ + 1) & 31; break;
			case 0xb: r = b->cr; break;
			default:
				if (host().dbg)
					host().log("in%d bm %02x\n", size, o);
			}
		} else {
			switch (o) {
			case 0x3c: r = glob_cnt; break;
			case 0x40: r = ich_status(); break;
			case 0x44: r = 0; break;        /* the codec is ours */
			default:
				if (host().dbg)
					host().log("in%d nabm %02x\n", size, o);
			}
		}
	} else
		return false;
	*v = size == 1 ? r & 0xff : size == 2 ? r & 0xffff : r;
	return true;
}

/* a port write, to the same registers as ich_io_in's; false if a
   is not the controller's */
template <class Host>
bool Ich<Host>::ich_io_out(unsigned a, u32 v, int size)
{
	if (a >= nambar_ && a < nambar_ + 0x100) {
		unsigned o = a - nambar_;
		if (!codec_at_)
			host().codec_write(o, (word)v);
		else if (o >= codec_at_)
			host().codec_write(o - codec_at_, (word)v);
		return true;
	}
	if (a < nabmbar_ || a >= nabmbar_ + 0x80)
		return false;
	unsigned o = a - nabmbar_;
	BusMaster *b = o < 0x10 ? &pcm_in : o < 0x20 ? &pcm_out : nullptr;
	if (b) {
		switch (o & 0xf) {
		case 0x0: b->bdbar = v & ~7u; return true;
		case 0x5: b->lvi = v & 31; return true;
		case 0x6: b->sr &= ~(v & (SR_LVBCI | SR_BCIS | SR_FIFOE)); return true;
		case 0xb: control(*b, (byte)v); return true;
		}
		if (host().dbg)
			host().log("out%d bm %02x = %x\n", size, o, v);
		return true;
	}
	switch (o) {
	case 0x3c: glob_cnt = v; return true;
	case 0x40: glob_sta &= ~(v & 0xc01); return true;
	case 0x44: return true;
	}
	if (host().dbg)
		host().log("out%d nabm %02x = %x\n", size, o, v);
	return true;
}

}

#endif
