/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/si3054_hda.hpp - a Silicon Labs Si3054 HD-audio modem codec and
 *                        its line side: a mixin of a harness
 *
 *    The interface is the one Linux's snd-hda-codec-si3054 drives
 *    (sound/pci/hda/patch_si3054.c), the codecs of its list made to it:
 *
 *    - the root node (0): its vendor ID (si3054_vendor, 0x163c3055 by
 *      default), revision, and one function group, node 1, a modem
 *      function group (type 2), with its subsystem ID;
 *    - SET_CODEC_RESET (0x7ff) on the root;
 *    - node 1's stream format (0x2 / 0xa) and stream and channel (0x706
 *      / 0xf06), which the generic code sets;
 *    - its registers, each a node of its own (2 to 21), 16 bits: read
 *      with verb 0x9, written with 0x1 (4 bit verbs, the 16 bit payload
 *      the value).  EXTENDED_MID (2) reads its ready bits (0xf) once
 *      written after a reset; LINE_RATE (3) is the codec's sample rate
 *      (8000, 9600 or 16000); LINE_LEVEL (4) has the stream numbers of
 *      the capture (bits 4..7, ATAG) and of the playback (12..15,
 *      DTAG); GPIO_CONTROL (10) the hook (bit 0, off hook) and the line
 *      monitor (bit 1, "Caller ID"); CHIPID (12) the line side chip's ID
 *      and revisions; LINE_STATUS (14) bit 6, the link to the line side
 *      up (frame detect).
 *
 *    The line: off hook the codec's ADC hears it and its DAC drives it;
 *    on hook the ADC hears it only with the line monitor on (rings and
 *    caller ID), else nothing, and the DAC is not on it.
 *
 *    The harness (Host, a half::HalfModem) gives the codec its commands
 *    through si3054_command() and each sample at si3054_rate() through
 *    si3054_sample().
 *
 */

#ifndef HW_SI3054_HDA_HPP
#define HW_SI3054_HDA_HPP

#include "../modem_half.hpp"

namespace half {

template <class Host>
class Si3054Hda {
public:
	enum {
		EXTENDED_MID = 2, LINE_RATE = 3, LINE_LEVEL = 4, GPIO_CFG = 5,
		GPIO_POLARITY = 6, GPIO_STICKY = 7, GPIO_WAKEUP = 8,
		GPIO_STATUS = 9, GPIO_CONTROL = 10, MISC_AFE = 11, CHIPID = 12,
		LINE_CFG1 = 13, LINE_STATUS = 14, DC_TERMINATION = 15,
		LINE_CONFIG = 16, CALLPROG_ATT = 17, SQ_CONTROL = 18,
		MISC_CONTROL = 19, RING_CTRL1 = 20, RING_CTRL2 = 21,
		NREGS = 22,
	};
	enum {
		MEI_READY = 0xf, GPIO_OH = 0x1, GPIO_CID = 0x2,
		LINE_FDT = 0x40,
		MFG = 1,                /* the modem function group's node */
	};

	u32 si3054_vendor = 0x163c3055, si3054_subsystem = 0x163c3055;
	word si3054_regs[NREGS] = {};
	word si3054_fmt = 0;            /* node 1's stream format */
	byte si3054_stream = 0;         /* node 1's stream and channel */
	bool si3054_mei = false;        /* EXTENDED_MID written: ready */
	unsigned long si3054_bad = 0;   /* commands it has no answer for */

	Si3054Hda() { si3054_defaults(); }

	/* a command word: its response */
	u32 si3054_command(u32 cmd)
	{
		unsigned nid = (cmd >> 20) & 0xff, v = cmd & 0xfffff;
		unsigned v4 = v >> 16, payload16 = v & 0xffff;
		if (cmd >> 28)
			return bad(cmd);        /* (one codec, address 0) */
		/* the registers: 4 bit verbs 1 (write) and 9 (read) */
		if (nid >= EXTENDED_MID && nid < NREGS && (v4 == 1 || v4 == 9)) {
			if (v4 == 9)
				return reg(nid);
			set_reg(nid, (word)payload16);
			return 0;
		}
		if (v4 == 0x2 && nid == MFG) {
			si3054_fmt = (word)payload16;
			return 0;
		}
		if (v4 == 0xa && nid == MFG)
			return si3054_fmt;
		unsigned verb = v >> 8, p = v & 0xff;
		switch (verb) {
		case 0xf00:     /* GET_PARAMETER */
			return param(nid, p, cmd);
		case 0xf20:     /* the subsystem ID */
			return nid == MFG ? si3054_subsystem : bad(cmd);
		case 0x7ff:     /* SET_CODEC_RESET */
			if (nid != 0)
				return bad(cmd);
			si3054_defaults();
			if (host().dbg)
				host().log("si3054: reset\n");
			return 0;
		case 0x706:     /* SET_CHANNEL_STREAMID */
			if (nid != MFG)
				return bad(cmd);
			si3054_stream = (byte)p;
			return 0;
		case 0xf06:
			return nid == MFG ? si3054_stream : bad(cmd);
		case 0x705:     /* SET_POWER_STATE, GET_POWER_STATE: D0 */
			return 0;
		case 0xf05:
			return 0;
		}
		return bad(cmd);
	}

	/* the codec's sample rate */
	unsigned si3054_rate() const
	{
		word r = si3054_regs[LINE_RATE];
		return r == 8000 || r == 9600 || r == 16000 ? r : 9600;
	}
	bool si3054_off_hook() const { return si3054_regs[GPIO_CONTROL] & GPIO_OH; }
	bool si3054_monitor() const { return si3054_regs[GPIO_CONTROL] & GPIO_CID; }
	/* the stream numbers it sends the capture on and takes the
	   playback from */
	int si3054_in_tag() const { return (si3054_regs[LINE_LEVEL] >> 4) & 15; }
	int si3054_out_tag() const { return (si3054_regs[LINE_LEVEL] >> 12) & 15; }

	/* a codec sample: line, what the line brings; *adc what the ADC
	   gives the capture; dac what the playback gave the DAC; what goes
	   on to the line */
	short si3054_sample(short line, short *adc, short dac)
	{
		bool off = si3054_off_hook();
		*adc = off || si3054_monitor() ? line : 0;
		return off ? dac : 0;
	}

private:
	Host &host() { return static_cast<Host &>(*this); }

	void si3054_defaults()
	{
		for (word &r : si3054_regs)
			r = 0;
		si3054_regs[LINE_RATE] = 9600;
		/* the line side: an international Si3018 (DAA ID 3), codec
		   revision 5, DAA revision 2 */
		si3054_regs[CHIPID] = 0x1000 | 0x0300 | 0x0020 | 0x0005;
		si3054_mei = false;
		si3054_fmt = 0;
		si3054_stream = 0;
	}

	u32 bad(u32 cmd)
	{
		si3054_bad++;
		if (host().dbg)
			host().log("si3054: no answer to %08x\n", cmd);
		return 0;
	}

	u32 param(unsigned nid, unsigned p, u32 cmd)
	{
		if (nid == 0) {
			switch (p) {
			case 0x00: return si3054_vendor;
			case 0x02: return 0x00100100;   /* revision 1.0, 1 */
			case 0x04: return 1u << 16 | 1; /* node 1, one node */
			}
		} else if (nid == MFG) {
			switch (p) {
			case 0x04: return 0;            /* no widgets */
			case 0x05: return 0x02;         /* a modem function group */
			case 0x08: return 0;            /* audio caps */
			case 0x0a: return 0x00020060;   /* 16 bit; 8, 16 kHz */
			case 0x0b: return 1;            /* PCM */
			case 0x0f: return 0x1;          /* D0 */
			}
		}
		return bad(cmd);
	}

	word reg(unsigned r) const
	{
		word v = si3054_regs[r];
		if (r == EXTENDED_MID && si3054_mei)
			v |= MEI_READY;
		if (r == LINE_STATUS)
			v = (word)(v | LINE_FDT);       /* the link is up */
		return v;
	}

	void set_reg(unsigned r, word v)
	{
		word was = si3054_regs[r];
		si3054_regs[r] = v;
		if (r == EXTENDED_MID)
			si3054_mei = true;
		if (r == CHIPID)
			si3054_regs[r] = was;   /* (read only) */
		if (host().dbg > 1 || (host().dbg && was != v &&
				       r != GPIO_CONTROL))
			host().log("si3054: reg %2u = %04x\n", r, v);
		if (r == GPIO_CONTROL && (was ^ v) && host().dbg)
			host().log("si3054: %s hook%s\n",
				   v & GPIO_OH ? "off" : "on",
				   v & GPIO_CID ? ", line monitor" : "");
	}
};

}

#endif
