/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/hda.hpp - an Intel High Definition Audio controller, as a modem
 *                 codec's driver uses it: a mixin of a harness
 *
 *    After the HD Audio specification (revision 1.0a): its global
 *    registers (GCAP, GCTL's controller reset, STATESTS's codec on SDIN
 *    0, INTCTL, INTSTS), the immediate command interface (ICOI, IRII,
 *    ICIS: a command word to the codec, its response) and two stream
 *    descriptors, input stream 0 (at 0x80) and output stream 0 (at 0xa0):
 *    SDnCTL (stream reset, run, interrupt on completion, the stream
 *    number), SDnSTS (buffer completion), SDnLPIB, SDnCBL, SDnLVI,
 *    SDnFMT, SDnBDPL/BDPU.  A running stream walks its buffer descriptor
 *    list (16 octets an entry: the address, the length, IOC) a sample
 *    (16 bits) at a time, LPIB its position in the cyclic buffer of CBL
 *    octets; an entry done with IOC sets BCIS.
 *
 *    Memory is the process's own: a descriptor's address is a pointer.
 *
 *    The harness (Host, a half::HalfModem) has the codec on SDIN 0:
 *    u32 hda_codec_command(u32 cmd) answers a command word (its address
 *    in bits 28..31, its node in 20..27, the verb and its payload in
 *    0..19).  Its driver reads and writes the registers through
 *    hda_read() and hda_write(), and each codec sample goes through
 *    hda_dma() of the stream whose number the codec sends it on.
 *
 */

#ifndef HW_HDA_HPP
#define HW_HDA_HPP

#include <stdint.h>
#include <string.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class HdaController {
public:
	enum {
		GCAP = 0x00, VMIN = 0x02, VMAJ = 0x03, GCTL = 0x08,
		STATESTS = 0x0e, INTCTL = 0x20, INTSTS = 0x24,
		ICOI = 0x60, IRII = 0x64, ICIS = 0x68,
		SD0 = 0x80, SD_SIZE = 0x20,
		/* a stream descriptor's registers, from its base */
		SD_CTL = 0x00, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08,
		SD_LVI = 0x0c, SD_FIFOS = 0x10, SD_FMT = 0x12, SD_BDPL = 0x18,
		SD_BDPU = 0x1c,
	};
	enum {
		GCTL_CRST = 0x1,
		ICIS_ICB = 0x1, ICIS_IRV = 0x2,
		CTL_SRST = 0x1, CTL_RUN = 0x2, CTL_IOCE = 0x4,
		STS_BCIS = 0x4, STS_FIFOE = 0x8, STS_DESE = 0x10,
		INTCTL_GIE = 0x80000000u,
	};
	enum { IN = 0, OUT = 1 };       /* the streams */

	struct Stream {
		u32 ctl = 0;            /* 24 bits */
		byte sts = 0;
		u32 lpib = 0, cbl = 0, bdpl = 0, bdpu = 0;
		word lvi = 0, fmt = 0;
		int civ = 0;            /* the descriptor entry being done */
		u32 pos = 0;            /* octets of it done */
		/* it runs: RUN, not in reset */
		bool running() const { return (ctl & CTL_RUN) && !(ctl & CTL_SRST); }
		/* its stream number (SDnCTL bits 20..23) */
		int tag() const { return (int)(ctl >> 20) & 15; }
	};

	Stream hda_sd[2];
	u32 hda_gctl = 0, hda_intctl = 0;
	word hda_statests = 0, hda_icis = 0;
	u32 hda_icoi = 0, hda_irii = 0;
	unsigned long hda_commands = 0; /* sent to the codec */

	/* a register read of size octets (1, 2, 4) at off */
	u32 hda_read(unsigned off, int size)
	{
		u32 d = dword(off & ~3u);
		return (d >> 8 * (off & 3)) & mask(size);
	}

	/* a register write */
	void hda_write(unsigned off, u32 v, int size)
	{
		unsigned base = off & ~3u, sh = 8 * (off & 3);
		u32 m = mask(size) << sh;
		v = (v << sh) & m;
		if (base == GCTL) {
			bool was = hda_gctl & GCTL_CRST;
			hda_gctl = (hda_gctl & ~m) | v;
			bool now = hda_gctl & GCTL_CRST;
			if (was && !now)
				reset();
			if (!was && now)
				hda_statests |= 1;      /* the codec on SDIN 0 */
			return;
		}
		if (base == 0x0c) {             /* WAKEEN, STATESTS (RW1C) */
			hda_statests &= (word)~(v >> 16);
			return;
		}
		if (!(hda_gctl & GCTL_CRST))
			return;                 /* (in reset: no writes) */
		if (base == INTCTL) {
			hda_intctl = (hda_intctl & ~m) | v;
			return;
		}
		if (base == ICOI) {
			hda_icoi = (hda_icoi & ~m) | v;
			return;
		}
		if (base == ICIS) {
			if (v & ICIS_IRV)
				hda_icis &= (word)~ICIS_IRV;
			if ((v & ICIS_ICB) && !(hda_icis & ICIS_ICB)) {
				hda_commands++;
				hda_irii = host().hda_codec_command(hda_icoi);
				hda_icis |= ICIS_IRV;
			}
			return;
		}
		if (base >= SD0 && base < SD0 + 2 * SD_SIZE) {
			Stream &s = hda_sd[(base - SD0) / SD_SIZE];
			sd_write(s, (base - SD0) % SD_SIZE, v, m);
		}
	}

	/* the interrupt: a stream's completion, with its IOCE, and GIE */
	bool hda_irq() const { return (hda_intctl & INTCTL_GIE) && intsts(); }

	/* one sample through stream number tag of direction d, if it runs:
	   *s to memory (IN) or from it (OUT); false (*s untouched) if no
	   stream of that number runs */
	bool hda_dma(int d, int tag, short *s)
	{
		Stream &st = hda_sd[d];
		if (!st.running() || st.tag() != tag || !tag)
			return false;
		const byte *e = entry(st, st.civ);
		u32 len = le32(e + 8), ioc = le32(e + 12) & 1;
		byte *p = (byte *)(uintptr_t)(le32(e) |
					     (uint64_t)le32(e + 4) << 32) + st.pos;
		if (len < 2) {
			st.sts |= STS_DESE;
			st.ctl &= ~(u32)CTL_RUN;
			return false;
		}
		if (d == IN) {
			p[0] = (byte)*s;
			p[1] = (byte)((unsigned short)*s >> 8);
		} else {
			*s = (short)(p[0] | p[1] << 8);
		}
		st.pos += 2;
		st.lpib += 2;
		if (st.lpib >= st.cbl)
			st.lpib = 0;
		if (st.pos >= len) {
			st.pos = 0;
			if (ioc)
				st.sts |= STS_BCIS;
			st.civ = st.civ >= st.lvi ? 0 : st.civ + 1;
		}
		return true;
	}

	/* the rate of a stream format (SDnFMT, a converter's): 0 if it
	   has none */
	static unsigned hda_fmt_rate(word fmt)
	{
		unsigned base = fmt & 0x4000 ? 44100 : 48000;
		unsigned mult = ((fmt >> 11) & 7) + 1, div = ((fmt >> 8) & 7) + 1;
		return mult > 4 ? 0 : base * mult / div;
	}

	/* the format of 16 bit mono at rate (48 kHz's divisors): 0 if none */
	static word hda_fmt(unsigned rate)
	{
		for (unsigned div = 1; div <= 8; div++)
			for (unsigned mult = 1; mult <= 4; mult++)
				if (48000 * mult / div == rate &&
				    48000 * mult % div == 0)
					return (word)((mult - 1) << 11 |
						      (div - 1) << 8 | 1 << 4);
		return 0;
	}

private:
	/* the harness */
	Host &host() { return static_cast<Host &>(*this); }

	/* the bits of a size octet register */
	static u32 mask(int size)
	{
		return size >= 4 ? 0xffffffffu : (1u << 8 * size) - 1;
	}
	/* the little endian 32 bit word at p */
	static u32 le32(const byte *p)
	{
		return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
	}

	/* entry i of s's buffer descriptor list */
	const byte *entry(const Stream &s, int i) const
	{
		return (const byte *)(uintptr_t)((s.bdpl & ~0x7fu) |
						  (uint64_t)s.bdpu << 32) + 16 * i;
	}

	/* INTSTS: the streams' completions they interrupt for, and GIS */
	u32 intsts() const
	{
		u32 v = 0;
		for (int i = 0; i < 2; i++)
			if ((hda_sd[i].sts & STS_BCIS) &&
			    (hda_sd[i].ctl & CTL_IOCE))
				v |= 1u << i;
		return v ? v | 0x80000000u : 0;
	}

	/* the controller's reset: all but STATESTS to their defaults */
	void reset()
	{
		hda_sd[0] = hda_sd[1] = Stream();
		hda_intctl = 0;
		hda_icis = 0;
		hda_icoi = hda_irii = 0;
	}

	/* the 32 bit register at off (0 if none) */
	u32 dword(unsigned off) const
	{
		switch (off) {
		case GCAP:      /* one output and one input stream, v1.0 */
			return 0x1100u | 0u << 16 | 1u << 24;
		case GCTL: return hda_gctl;
		case 0x0c: return (u32)hda_statests << 16;
		case INTCTL: return hda_intctl;
		case INTSTS: return intsts();
		case ICOI: return hda_icoi;
		case IRII: return hda_irii;
		case ICIS: return hda_icis;
		}
		if (off >= SD0 && off < SD0 + 2 * SD_SIZE) {
			const Stream &s = hda_sd[(off - SD0) / SD_SIZE];
			switch ((off - SD0) % SD_SIZE) {
			case SD_CTL: return s.ctl | (u32)s.sts << 24;
			case SD_LPIB: return s.lpib;
			case SD_CBL: return s.cbl;
			case SD_LVI: return s.lvi;
			case SD_FIFOS: return 0x40u | (u32)s.fmt << 16;
			case SD_BDPL: return s.bdpl;
			case SD_BDPU: return s.bdpu;
			}
		}
		return 0;
	}

	/* stream s's register r written with v under m: SDnSTS cleared
	   by its 1s, a reset, the rest only while it does not run */
	void sd_write(Stream &s, unsigned r, u32 v, u32 m)
	{
		switch (r) {
		case SD_CTL: {
			/* SDnSTS (bits 24..31) is RW1C */
			s.sts &= (byte)~(v >> 24);
			u32 cm = m & 0xffffff, was = s.ctl;
			s.ctl = (s.ctl & ~cm) | (v & cm);
			if (!(was & CTL_SRST) && (s.ctl & CTL_SRST)) {
				u32 keep = s.ctl;
				s = Stream();
				s.ctl = keep & CTL_SRST;
			}
			if (!(was & CTL_RUN) && (s.ctl & CTL_RUN) &&
			    s.lpib == 0 && s.pos == 0)
				s.civ = 0;
			break;
		}
		case SD_CBL:
			if (!s.running())
				s.cbl = (s.cbl & ~m) | v;
			break;
		case SD_LVI:
			if (!s.running())
				s.lvi = (word)(((s.lvi & ~m) | v) & 0xff);
			break;
		case SD_FIFOS:  /* (FIFOS is read only) SDnFMT */
			if (!s.running())
				s.fmt = (word)(((s.fmt << 16 & ~m) | v) >> 16);
			break;
		case SD_BDPL:
			if (!s.running())
				s.bdpl = ((s.bdpl & ~m) | v) & ~0x7fu;
			break;
		case SD_BDPU:
			if (!s.running())
				s.bdpu = (s.bdpu & ~m) | v;
			break;
		}
	}
};

}

#endif
