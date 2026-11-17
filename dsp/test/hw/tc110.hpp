/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/tc110.hpp - a 3Com/Megahertz XJ5560 / CC5560 PC Card modem (the
 *                   firmware of the TC110 flash update): its TMS320C5x,
 *                   the flash, the host UART, the cable's EEPROM, the
 *                   line sense and the codec
 *
 *    The C5x runs everything (no other CPU): its program 0 to 0x7fff
 *    RAM, 0x8000 to 0xffff the flash's page that port 0x806f selects
 *    (8 pages of 32 K words), its data all RAM (tms320c5x.hpp's flat
 *    map).  At reset it runs page 0 at 0x8000.
 *
 *    The on-chip ROM (program 0x0030 to 0x0fff while PMST's MP/MC is 0:
 *    the data pump clears it) is not in the flash.  It is a USRobotics
 *    DSP library, of the same family as the Courier 3453C's mask ROM:
 *    the routines found so far are here, those of the 3453C copied to
 *    their TC110 addresses (sine and cosine, arctangent, divide), the
 *    others written from how the firmware calls them (rom_call()).  An
 *    address of the ROM not known stops the DSP (rom_miss).
 *
 *    The host UART is a 16550 at ports 0x8050 to 0x8057 (the DTE's side
 *    of it is this class's from_dte() and to_dte), port 0x805b its
 *    events to the DSP (bit 0 a received octet, 1 the transmitter
 *    empty, 3 a change of DTR), each one INT2.  Port 0x8056 is the modem
 *    status as the DSP sees it (bit 7 DTR, bit 3 its change).
 *
 *    Port 0x8058 bit 6 and port 0x805f bits 4 and 7 are the I2C bus of
 *    the cable's 24Cxx EEPROM (its data in on port 0x805f bit 7): no
 *    cable by default (a landline).  Port
 *    0x8059 bit 0 is the line sense (1: line current).
 *
 *    The codec is on the serial port (DXR the DAC's sample, DRR the
 *    ADC's), a TLC320AC01 or the like: a DAC word with its two low bits
 *    set asks for a secondary frame (its own XINT half a sample later),
 *    whose word is a codec register's (codec_reg()).  Its rate is set
 *    by its registers and port 0x8065 (codec_clock()): 7200 Hz from the
 *    boot (the answer tone's phase steps are 2100 Hz and 15 Hz at that
 *    rate), 9600 Hz for V.34.  The DSP is taken to be at 40 MIPS, its timer
 *    counting each instruction.
 */

#ifndef HW_TC110_HPP
#define HW_TC110_HPP

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "tms320c5x.hpp"

namespace half {

/* a 24Cxx serial EEPROM on an I2C bus that the DSP drives a bit at a
   time (the cable's) */
struct I2cEeprom {
	std::vector<unsigned char> mem = std::vector<unsigned char>(2048, 0xff);
	int scl = 1, sda = 1;   /* the lines as the master drives them */
	int out = 1;            /* the EEPROM's SDA (1: released) */
	bool present = false;   /* a cable with its EEPROM plugged in */
	enum { IDLE, ADDR, ACK_ADDR, WORD, ACK_WORD, WDATA, ACK_DATA, RDATA,
	       RACK } st = IDLE;
	int bit = 0;
	unsigned char sh = 0;
	unsigned addr = 0;
	bool rd = false;

	/* SDA as the master reads it */
	int line() const { return sda & out; }

	/* the master's SCL and SDA */
	void set(int nscl, int nsda)
	{
		if (nscl && scl && nsda != sda) {
			/* SDA moves while SCL is high: START or STOP */
			if (!nsda) {
				st = ADDR;
				bit = 0;
				sh = 0;
			} else {
				st = IDLE;
			}
			out = 1;
			sda = nsda;
			return;
		}
		if (!scl && nscl)
			rise(nsda);
		if (scl && !nscl)
			fall();
		scl = nscl;
		sda = nsda;
	}

	/* SCL rises: a bit in */
	void rise(int d)
	{
		switch (st) {
		case ADDR:
		case WORD:
		case WDATA:
			sh = (unsigned char)(sh << 1 | d);
			bit++;
			break;
		case RACK:
			if (d) {
				st = IDLE;
			} else {
				st = RDATA;
				bit = 0;
			}
			break;
		default:
			break;
		}
	}

	/* SCL falls: the next bit out, or an acknowledge */
	void fall()
	{
		switch (st) {
		case ADDR:
			if (bit == 8) {
				rd = sh & 1;
				if (!present || (sh & 0xf0) != 0xa0) {
					st = IDLE;
					out = 1;
					break;
				}
				addr = (addr & 0xff) | ((sh >> 1) & 7) << 8;
				st = ACK_ADDR;
				out = 0;
			}
			break;
		case ACK_ADDR:
			out = 1;
			bit = 0;
			if (rd) {
				st = RDATA;
				drive();
			} else {
				st = WORD;
				sh = 0;
			}
			break;
		case WORD:
			if (bit == 8) {
				addr = (addr & 0x700) | sh;
				st = ACK_WORD;
				out = 0;
			}
			break;
		case ACK_WORD:
		case ACK_DATA:
			out = 1;
			st = WDATA;
			bit = 0;
			sh = 0;
			break;
		case WDATA:
			if (bit == 8) {
				mem[addr % mem.size()] = sh;
				addr = (addr & ~15u) | ((addr + 1) & 15);
				st = ACK_DATA;
				out = 0;
			}
			break;
		case RDATA:
			if (bit == 8) {
				out = 1;
				st = RACK;
				addr++;
			} else {
				drive();
			}
			break;
		default:
			break;
		}
	}

	/* the next bit of the word at addr on SDA */
	void drive()
	{
		out = (mem[addr % mem.size()] >> (7 - bit)) & 1;
		bit++;
	}
};

class Tc110 {
public:
	typedef unsigned char u8;
	typedef C5x::w16 w16;

	C5x dsp;
	std::vector<w16> flash;         /* 8 pages of 32 K words */
	int page = 0;                   /* port 0x806f's */
	double ips = 40e6;              /* the DSP's instructions a second */
	double codec_rate = 7200;
	bool line_ok = true;            /* port 0x8059 bit 0 */
	std::function<void(w16)> on_dxr; /* (a test's: each DAC word) */
	bool negmu = getenv("TC110_NEGMU") != nullptr; /* (a test) */
	/* the adaptive filters' step (0x0cd0 to 0x0d42) shifted right by
	   this: the 3453C's routines have twice the TC110's step (the
	   echo cancellers and the equalizer diverge with it); TC110_LMS_SHIFT
	   changes it (a test) */
	bool freeze_eq = getenv("TC110_FREEZE_EQ") != nullptr;
	int lms_shift = getenv("TC110_LMS_SHIFT") ? atoi(getenv("TC110_LMS_SHIFT")) : 1;
	int fftshift = getenv("TC110_FFTSHIFT") ? atoi(getenv("TC110_FFTSHIFT")) : 0;
	int eqmu = getenv("TC110_EQMU") ? atoi(getenv("TC110_EQMU")) : 0;
	bool negmu_far = negmu && getenv("TC110_NEGMU")[0] == 'f';
	bool negmu_near_only = negmu && getenv("TC110_NEGMU")[0] == 'n';
	int carrier_sign = 1;            /* (a test: -1 the other way) */
	int rom_miss = -1;              /* a ROM address not known, reached */
	/* if set: the count of each page:PC the DSP runs */
	std::map<unsigned, long> *prof = nullptr;
	/* on_watch called each time the DSP is at watch_pc */
	unsigned watch_pc = 0x10000;
	std::function<void()> on_watch;
	/* if hist_on: the last 256 page:PCs, the newest at hist_n - 1 */
	bool hist_on = false;
	unsigned hist[256] = {}, hist_n = 0;
	int wait_left() const { return wait_; }
	int dbg = 0;                    /* 1: the ROM calls not yet known */

	std::function<void(u8)> to_dte;
	std::function<void(const char *)> log;

	/* the flash image (TC110's, 0x40000 words, little-endian) loaded,
	   the DSP reset; false (and err) if it cannot be read */
	bool load(const char *path, std::string &err)
	{
		FILE *f = fopen(path, "rb");
		if (!f) {
			err = std::string(path) + ": cannot read";
			return false;
		}
		flash.assign(0x40000, 0xffff);
		size_t n = fread(flash.data(), 2, flash.size(), f);
		fclose(f);
		if (n < 0x8000) {
			err = std::string(path) + ": too short";
			return false;
		}
		dsp.flat = true;
		dsp.PR.assign(0x8000, 0);
		dsp.win = &flash[0];
		dsp.ROM.assign(0x1000, 0xffff);
		dsp.rom_lo = 0x30;
		dsp.shared_lo = getenv("TC110_NOSHARE") ? 0x8000 : 0x7c00;
		rom_init();
		dsp.io_rd = [this](unsigned p) { return io_rd(p); };
		dsp.io_wr = [this](unsigned p, w16 v) { io_wr(p, v); };
		dsp.mmr_wr = [this](C5x::u32 a, w16 old) { mmr_wr(a, old); };
		dsp.pc = 0x8000;
		return true;
	}

	/* DTR from the DTE */
	void set_dtr(bool on)
	{
		if (on == dtr_)
			return;
		dtr_ = on;
		msr_ = (w16)((on ? 0x99 : 0x19) | 0x08);
		ev_ |= 0x08;
		dsp.D[6] |= 1;          /* (INT1: the DSP woken) */
	}

	/* an octet from the DTE (the UART's receiver) */
	void from_dte(u8 b)
	{
		rx_.push_back(b);
		dsp.D[6] |= 1;
	}
	/* the DTE's octets not yet read by the DSP */
	size_t dte_queued() const { return rx_.size(); }

	/* one codec sample: the ADC's in, the DSP run for the sample's
	   time; the DAC's out */
	short sample(short adc)
	{
		due_ += ips / codec_rate;
		while (due_ >= 1 && !dsp.stopped && rom_miss < 0) {
			due_ -= 1;
			tick();
		}
		/* the frame: the transmitter's and the receiver's
		   interrupts, if the DSP enabled them */
		w16 spc = dsp.D[0x22];
		if (spc & 0x40) {
			dsp.D[0x22] |= 0x0800;
			dsp.D[6] |= 0x20;
		}
		if (spc & 0x80) {
			dsp.D[0x20] = (w16)adc;
			dsp.D[0x22] |= 0x0400;
			dsp.D[6] |= 0x10;
		}
		return dac_;
	}

private:
	std::deque<u8> rx_;             /* the UART's receiver */
	w16 ur_[32] = {};               /* the UART's registers (16, 17:
					   the divisor latch) */
	w16 msr_ = 0;                   /* port 0x8056 */
	w16 ev_ = 0;                    /* port 0x805b's events, other than
					   the UART's own */
	bool dtr_ = false;
	bool thre_ = false;             /* the transmitter's interrupt */
	long thre_in_ = -1;             /* instructions until it */
	w16 p58_ = 0, p5f_ = 0;
	I2cEeprom ee_;
	short dac_ = 0;
	double due_ = 0;
	int psc_ = 0;                   /* the timer's prescaler */
	int wait_ = 0;                  /* 0x0cb0's blocks still to wait */
	int sec_ = 0;                   /* the codec's secondary frame: 1
					   due, 2 its word next */
	long sec_in_ = 0;               /* instructions until it */
	w16 creg_[8] = {};              /* the codec's registers */
	w16 p65_ = 0;                   /* port 0x8065: the clock's divider */

	/* port 0x805b: the events */
	w16 events() const
	{
		return (w16)(((!rx_.empty() && (ur_[1] & 1)) ? 1 : 0) |
			     ((thre_ && (ur_[1] & 2)) ? 2 : 0) | ev_);
	}

	/* one DSP instruction's time: the timer, the UART, interrupts */
	/* at an adaptive filter routine's start: its steps (DP's 0x7d and
	   0x7e, and 0x7c and 0x7f for the 4 word ones) times 2^-lms_shift */
	void lms_step()
	{
		unsigned b = dsp.dp << 7, lo, hi;
		switch (dsp.pc) {
		case 0x0cd0: case 0x0ce4: case 0x0d19: case 0x0d42:
			lo = 0x7d; hi = 0x7e; break;
		case 0x0cfc: case 0x0d29:
			lo = 0x7c; hi = 0x7f; break;
		default:
			return;
		}
		for (unsigned a = lo; a <= hi; a++)
			dsp.D[b + a] = (w16)((short)dsp.D[b + a] >> lms_shift);
		if (freeze_eq && dsp.pc == 0x0cfc)
			for (unsigned a = lo; a <= hi; a++)
				dsp.D[b + a] = 0;
	}
	void tick()
	{
		if (!(dsp.D[0x26] & 0x10) && psc_-- <= 0) {
			psc_ = dsp.D[0x26] & 15;
			if (dsp.D[0x24]-- == 0) {
				dsp.D[0x24] = dsp.D[0x25];
				dsp.D[6] |= 8;
			}
		}
		if (thre_in_ >= 0 && --thre_in_ < 0)
			thre_ = true;
		if (sec_ == 1 && --sec_in_ < 0) {
			sec_ = 2;
			dsp.D[0x22] |= 0x0800;
			dsp.D[6] |= 0x20;
		}
		if (events())
			dsp.D[6] |= 2;
		if (!dsp.intm && !dsp.rpt_pending()) {
			unsigned pend = dsp.D[6] & dsp.D[4] & 0x1ff;
			if (pend) {
				int k = __builtin_ctz(pend);
				dsp.D[6] &= (w16)~(1u << k);
				dsp.intr(2 * (k + 1), dsp.pc);
			}
		}
		if (dsp.idle && (dsp.D[6] & dsp.D[4] & 0x1ff))
			dsp.idle = false;
		if (dsp.idle)
			return;
		if (prof)
			(*prof)[(unsigned)page << 16 | dsp.pc]++;
		if (hist_on)
			hist[hist_n++ & 255] = (unsigned)page << 16 | dsp.pc;
		if (dsp.pc == watch_pc && on_watch)
			on_watch();
		if (lms_shift && dsp.pc >= 0x0cd0 && dsp.pc <= 0x0d42)
			lms_step();
		if (fftshift && dsp.pc == 0x6da0 && page == 1)
			for (unsigned a = 0x280; a < 0x300; a++)
				dsp.D[a] = (w16)((short)dsp.D[a] >> fftshift);
		if (eqmu && dsp.pc == 0x6e25 && page == 1) {
			dsp.D[0x37d] = (w16)((short)dsp.D[0x37d] >> eqmu);
			dsp.D[0x37e] = (w16)((short)dsp.D[0x37e] >> eqmu);
		}
		if (negmu && (dsp.pc == 0x0cd0 || dsp.pc == 0x0ce4) &&
		    (negmu_far ? dsp.D[0x11] >= 0x7e00 : dsp.D[0x11] < 0x7e00 ||
		     !negmu_near_only)) {
			dsp.D[0x3fd] = 0;
			dsp.D[0x3fe] = 0;
		}
		if (!(dsp.D[0x07] & 8) && dsp.pc >= dsp.rom_lo && dsp.pc < 0x1000 &&
		    dsp.ROM[dsp.pc] == 0xffff && !rom_call())
			return;
		dsp.step();
	}

	/* the ROM's routines that are not code here, at the DSP's PC: done
	   (and returned from), else rom_miss set; false if the DSP is not
	   to step */
	bool rom_call()
	{
		switch (dsp.pc) {
		case 0x0b12:
		case 0x0b2c: {
			/* the sine of the phase in ACCH: Q15, in ACCH (0x0b12)
			   or shifted left 15 (0x0b2c) */
			int ph = (int)(dsp.acc >> 16) & 0xffff;
			int q = (int)lrint(sin(ph * 2 * M_PI / 65536.0) * 32767);
			dsp.acc = (C5x::u32)((long long)q <<
					     (dsp.pc == 0x0b2c ? 15 : 16));
			break;
		}
		case 0x0ca7:
			/* a block's end: the wait 0x0cb0 set counted down,
			   else on to the pump's continuation (MMR 0x6d; 0:
			   none, a return) */
			if (wait_ > 0) {
				wait_--;
				break;
			}
			if (!dsp.D[0x6d])
				break;
			dsp.pc = dsp.D[0x6d];
			return false;
		case 0x0cb0:
			/* a wait of ACC blocks, then as 0x0cb1 */
			wait_ = (int)(dsp.acc & 0xffff);
			dsp.pc = 0x0cb1;
			return false;
		case 0x0cb1:
			/* the coroutine's yield: its return address to MMR
			   0x6d (0x0ca7 goes on there), then back to the
			   caller's caller */
			dsp.D[0x6d] = dsp.pop();
			break;
		case 0x0cb4: {
			/* ACC's bits ORed into the bit string at AR0, its
			   bit 0 at bit p, the word at 0x7f of the data page
			   (DP), its bit k at bit p - k */
			int p = dsp.D[(dsp.dp << 7 | 0x7f) & 0xffff];
			C5x::u32 v = dsp.acc;
			for (int k = 0; k < 32; k++)
				if (v >> k & 1)
					bit_set(dsp.D[0x10], p - k);
			break;
		}
		case 0x0cc5: {
			/* the 32 bits of the bit string at AR0 to bit ACC,
			   in ACC: bit k of it bit ACC - k of the string */
			int p = (int)(dsp.acc & 0xffff);
			C5x::u32 v = 0;
			for (int k = 0; k < 32; k++)
				v |= (C5x::u32)bit_get(dsp.D[0x10], p - k) << k;
			dsp.acc = v;
			break;
		}
		default:
			rom_miss = dsp.pc;
			if (log) {
				char s[96];
				snprintf(s, sizeof(s), "the DSP called ROM %04x "
					 "(not known), from %04x", dsp.pc, dsp.top());
				log(s);
			}
			return false;
		}
		dsp.pc = dsp.pop();
		return false;
	}

	/* the word and the bit of bit p of a bit string at b: bit 0 the
	   MSB of word b, bit 16 the MSB of word b + 1, and so on (the
	   INFO receiver's shift register: the newest bit its bit 0, at
	   the MSB of 0x258, and the older ones on from it) */
	w16 *bit_at(unsigned b, int p, int &sh)
	{
		sh = 15 - (p & 15);
		return &dsp.D[(b + (p >> 4)) & 0xffff];
	}
	/* bit p of the bit string at b set */
	void bit_set(unsigned b, int p)
	{
		int sh;
		w16 *w = bit_at(b, p, sh);
		*w |= (w16)(1u << sh);
	}
	/* bit p of the bit string at b */
	int bit_get(unsigned b, int p)
	{
		int sh;
		w16 *w = bit_at(b, p, sh);
		return *w >> sh & 1;
	}

	/* the ROM's code: the 3453C's routines at their places here */
	void rom_init()
	{
		/* an 80 tap low-pass filter, symmetrical (3453C 0x0030 to
		   0x007f, the same place) */
		static const w16 fir[] = {
			0xfff3, 0xfffe, 0x0015, 0x0039, 0x006c, 0x00aa, 0x00f1, 0x013a,
			0x017c, 0x01ac, 0x01be, 0x01a4, 0x0152, 0x00c0, 0xffe7, 0xfec9,
			0xfd6e, 0xfbe6, 0xfa48, 0xf8b5, 0xf755, 0xf654, 0xf5e1, 0xf62b,
			0xf75d, 0xf998, 0xfcf4, 0x017b, 0x0725, 0x0dd9, 0x156c, 0x1da2,
			0x2633, 0x2ec9, 0x370b, 0x3e9b, 0x4521, 0x4a50, 0x4dea, 0x4fc1,
			0x4fc1, 0x4dea, 0x4a50, 0x4521, 0x3e9b, 0x370b, 0x2ec9, 0x2633,
			0x1da2, 0x156c, 0x0dd9, 0x0725, 0x017b, 0xfcf4, 0xf998, 0xf75d,
			0xf62b, 0xf5e1, 0xf654, 0xf755, 0xf8b5, 0xfa48, 0xfbe6, 0xfd6e,
			0xfec9, 0xffe7, 0x00c0, 0x0152, 0x01a4, 0x01be, 0x01ac, 0x017c,
			0x013a, 0x00f1, 0x00aa, 0x006c, 0x0039, 0x0015, 0xfffe, 0xfff3,
		};
		/* the V.34 PP sequence (10.1.3.6/V.34): 48 phase indices of
		   4 bits (at index k, symbol 47 - k), then the 12 points (steps
		   of 30 degrees); 3453C 0x06b8 to 0x06db, here 0x0728 */
		static const w16 pp[] = {
			0x0ba9, 0x420a, 0x0963, 0x0840, 0x4b61, 0x0606, 0x05a3, 0x4804,
			0x0369, 0x0246, 0x4567, 0x0000, 0x2d41, 0x0000, 0x2731, 0x16a1,
			0x16a1, 0x2731, 0x0000, 0x2d41, 0xe95f, 0x2731, 0xd8cf, 0x16a1,
			0xd2bf, 0x0000, 0xd8cf, 0xe95f, 0xe95f, 0xd8cf, 0x0000, 0xd2bf,
			0x16a1, 0xd8cf, 0x2731, 0xe95f,
		};
		/* the 11 pre-emphasis filters (10.1.2.5/V.34), 5 taps each
		   (symmetrical, Q14: the firmware's PM is 1) at 9600 Hz: not of
		   the ROM; least squares fits of the templates for 3200 baud
		   on 1920 Hz, with the power of a flat signal */
		static const w16 pre[] = {
			0x0000, 0x0000, 0x4000, 0x0000, 0x0000,
			0xffc3, 0xfc31, 0x41b6, 0xfc31, 0xffc3,
			0xffcc, 0xf831, 0x4385, 0xf831, 0xffcc,
			0x0019, 0xf40e, 0x4569, 0xf40e, 0x0019,
			0x00a8, 0xefda, 0x475a, 0xefda, 0x00a8,
			0x0172, 0xeba2, 0x4952, 0xeba2, 0x0172,
			0x01af, 0xfbf3, 0x4375, 0xfbf3, 0x01af,
			0x037a, 0xf7b8, 0x46f8, 0xf7b8, 0x037a,
			0x055f, 0xf357, 0x4a7f, 0xf357, 0x055f,
			0x0758, 0xeedc, 0x4dff, 0xeedc, 0x0758,
			0x0961, 0xea52, 0x516c, 0xea52, 0x0961,
		};
		/* the symbol timing's first estimate (0x27c7, once a symbol
		   in S): 4 of the interpolator's 6 tap sets (as 0x2929 makes
		   them, Q14), 2 for each call of 0x296e. At 0x0a1c the
		   delays 0 and 1/2 of a sample (with AR0 2), at 0x0a10 3/4 and
		   1/4 (with AR0 1): the 4 points are a quarter of a symbol
		   apart. These are made from the code, not read from a
		   TC110 */
		static const w16 t0a10[] = {
			154, (w16)-1386, 13860, 4619, (w16)-990, 125,
			126, (w16)-990, 4619, 13859, (w16)-1387, 154,
			0, 0, 0, 16384, 0, 0,
			192, (w16)-1601, 9600, 9599, (w16)-1600, 191,
		};
		/* L1 and L2's period (10.1.2.4/V.34), 64 samples at 9600 Hz
		   (3453C 0x0080, the same place) */
		static const w16 probe[] = {
			0x5000, 0x506e, 0x2b25, 0xcc49, 0xee65, 0x16df, 0xb2d2, 0x018b,
			0x1b50, 0xc334, 0x2c0c, 0xfb62, 0xba10, 0x39c9, 0x39f2, 0x3e50,
			0x2000, 0xc437, 0x10e0, 0x4876, 0x3c91, 0xfca5, 0xb2f1, 0x0c29,
			0x04b0, 0xae38, 0x13ae, 0x40ff, 0xdafa, 0xbc17, 0xe48c, 0xd369,
			0xb000, 0xd369, 0xe48c, 0xbc17, 0xdafa, 0x40ff, 0x13ae, 0xae38,
			0x04b0, 0x0c29, 0xb2f1, 0xfca5, 0x3c91, 0x4876, 0x10e0, 0xc437,
			0x2000, 0x3e50, 0x39f2, 0x39c9, 0xba10, 0xfb62, 0x2c0c, 0xc334,
			0x1b50, 0x018b, 0xb2d2, 0x16df, 0xee65, 0xcc49, 0x2b25, 0x506e,
		};
		/* 0x0aca to 0x0bb0: the 3453C library's routines from 0x09e6
		   on (USR's DSP code has the same at 0x90b0, which this is,
		   its addresses moved): 0x0aca the constellation's quadrant
		   rotations, 0x0ad2 sine and cosine, 0x0b45 arctangent, 0x0b70 divide, 0x0b8c 10 log10
		   (times 0x0c0b), 0x0b92 log2 */
		static const w16 lib[] = {
			0x0b0b, 0x0b05, 0x0afe, 0x0b0c, 0x0b0b, 0x0b0c, 0x0afe, 0x0b05,
			0x997d, 0x6d7b, 0xa080, 0x527d, 0xbe03, 0xf344, 0x0aea, 0x8d7e,
			0xb900, 0xbfcf, 0x8001, 0x737e, 0xbe80, 0xb10e, 0x507e, 0x8d80,
			0x7380, 0xbe80, 0x102b, 0x507e, 0x8d80, 0x7380, 0xdec7, 0x707d,
			0x98a0, 0x1f7b, 0xbe80, 0x6488, 0x507e, 0x8d80, 0x7380, 0xbe80,
			0xd6a8, 0x507e, 0x8d80, 0x7380, 0xc519, 0x507e, 0x8d80, 0x7380,
			0xdfb7, 0xbe04, 0x9890, 0xee00, 0x1080, 0xbe02, 0x90a0, 0x1080,
			0xff00, 0xbe02, 0x9090, 0x10a0, 0x7690, 0x8aa0, 0xff00, 0xbe02,
			0x9090, 0xef00, 0x76a0, 0x1080, 0x8a90, 0xff00, 0xbe02, 0x9080,
			0x997d, 0x6d7b, 0xa080, 0x527d, 0xbe03, 0xff44, 0x8d7e, 0xb900,
			0xbfcf, 0x8001, 0x737e, 0xbe80, 0xb10e, 0x507e, 0x8d7d, 0x737d,
			0xbe80, 0x102b, 0x507e, 0x8d7d, 0x737d, 0xdec7, 0xbe04, 0xff00,
			0xe500, 0xbe02, 0x997d, 0x6d7b, 0xa080, 0x527d, 0x8d7e, 0x1f7b,
			0xbe80, 0x6488, 0x507e, 0x8d7d, 0x737d, 0xbe80, 0xd6a8, 0x507e,
			0x8d7d, 0x737d, 0xc519, 0x507e, 0x8d7d, 0x737d, 0xdfb7, 0xbe04,
			0xff00, 0xe500, 0xbe02, 0x108a, 0x6c89, 0xbe0a, 0xbfb0, 0xc000,
			0x907c, 0x417c, 0x6aa0, 0x629a, 0xbe1e, 0x65a0, 0x6690, 0xbe1d,
			0x61a0, 0x6299, 0xf500, 0xbe02, 0xbe1d, 0x7a80, 0x0b70, 0x987d,
			0xbe59, 0x527d, 0x8d7f, 0xca2f, 0x507f, 0x8d7e, 0x737e, 0xdcb0,
			0x507f, 0x8d7e, 0x737e, 0xc192, 0x507f, 0x8d7e, 0x737e, 0xdf8f,
			0xbe04, 0xbf9d, 0x4001, 0xff00, 0x2e7c, 0x9a7c, 0xbe1a, 0x987f,
			0xbe1a, 0xbe00, 0x987d, 0x907e, 0xb91f, 0x8809, 0xb900, 0xbe1d,
			0xbe00, 0xbec6, 0x0b84, 0x667e, 0x657d, 0xe311, 0x0b84, 0x627e,
			0x617d, 0xbe4e, 0xbe14, 0xbe1d, 0x407f, 0xe744, 0xba01, 0xff00,
			0xe500, 0xbe02, 0x7a80, 0x0b92, 0x880c, 0xff00, 0xcc0b, 0xbe03,
			0xbe1e, 0xef88, 0xb11f, 0xbfef, 0xf308, 0x0b9c, 0xbe1f, 0x907d,
			0x7c10, 0x6a7d, 0xbe4e, 0xbe0d, 0xbb0e, 0xa090, 0x987d, 0x527d,
			0x8d7e, 0xbf8d, 0xddd2, 0xcc0b, 0x707e, 0xd7ca, 0x507d, 0x8d7f,
			0x737f, 0xc271, 0xbe04, 0xbfee, 0xff00, 0x817f, 0x2a7f,
		};
		/* 0x0bb1: the 64 point FFT on the words at DP's 0x7a, and
		   its sine table (the 3453C's 0x0a68, moved) */
		static const w16 fft[] = {
			0x017a, 0xb002, 0xb91f, 0x8809, 0xbec6, 0x0bc2, 0x1f7b, 0x2de0,
			0x2dd0, 0x98e0, 0x3e80, 0x9890, 0x1f7b, 0x2de0, 0x2dd0, 0x98e0,
			0x3e80, 0x98a0, 0xae7f, 0x0002, 0x737f, 0x6b7b, 0xbe09, 0x8818,
			0xbfe1, 0xba01, 0x8813, 0xb400, 0xae7c, 0x0000, 0x107c, 0xbf90,
			0x0c06, 0xa679, 0xbf90, 0x0010, 0xa678, 0x737f, 0xb940, 0xbe5b,
			0xba01, 0x8809, 0xb801, 0x207c, 0x907c, 0x0814, 0xbe09, 0x627a,
			0x8811, 0x637b, 0x8812, 0xbec6, 0x0bfa, 0x8baa, 0x73a0, 0x5479,
			0x7189, 0x5478, 0x5079, 0x2e7b, 0x997e, 0x2f80, 0x999a, 0x657e,
			0x9990, 0x1e7b, 0x7489, 0x5478, 0xbe04, 0x997d, 0x2f80, 0x99ea,
			0x657d, 0x99e9, 0x8b8c, 0x8bab, 0x7b99, 0x0bcf, 0x697f, 0xb801,
			0x907f, 0xba06, 0xe3cc, 0x0bc5, 0xef00, 0x0000, 0x0646, 0x0c7c,
			0x1294, 0x187e, 0x1e2b, 0x238e, 0x289a, 0x2d41, 0x3179, 0x3537,
			0x3871, 0x3b21, 0x3d3f, 0x3ec5, 0x3fb1, 0x4000, 0x3fb1, 0x3ec5,
			0x3d3f, 0x3b21, 0x3871, 0x3537, 0x3179, 0x2d41, 0x289a, 0x238e,
			0x1e2b, 0x187e, 0x1294, 0x0c7c, 0x0646, 0x0000, 0xf9ba, 0xf384,
			0xed6c, 0xe782, 0xe1d5, 0xdc72, 0xd766, 0xd2bf, 0xce87, 0xcac9,
			0xc78f, 0xc4df, 0xc2c1, 0xc13b, 0xc04f,
		};
		/* 0x0cd0 to 0x0dd4: the 3453C library's adaptive filters'
		   and others from 0x0b21 on, moved (0x0cd0, 0x0ce4, 0x0cfc,
		   0x0d19, 0x0d42, 0x0d52, 0x0d66 the ones called) */
		static const w16 lms[] = {
			0x8809, 0x737d, 0x5489, 0xbec6, 0x0ce2, 0x6a8a, 0x628e, 0x747e,
			0x548d, 0x51a9, 0x989a, 0x909b, 0x6a8c, 0x628e, 0x747d, 0x54ad,
			0x508b, 0x989c, 0x9099, 0xef00, 0x8809, 0x737d, 0x548e, 0xbec6,
			0x0cfa, 0x1b7b, 0x747e, 0x548d, 0x51a9, 0xbfeb, 0x618a, 0x6289,
			0x989a, 0x909e, 0x1b7b, 0x747d, 0x54ad, 0x508b, 0xbfeb, 0x618c,
			0x628b, 0x989c, 0x909e, 0xef00, 0x8809, 0xb002, 0x737c, 0x54e9,
			0xbec6, 0x0d17, 0x6a8a, 0x628d, 0x747d, 0x548e, 0x747f, 0x54e0,
			0x747e, 0x548d, 0x51d9, 0x989a, 0x909b, 0x6a8c, 0x628d, 0x747f,
			0x54ae, 0x747d, 0x54d0, 0x707c, 0x54ad, 0x50eb, 0x989c, 0x9099,
			0xef00, 0x8809, 0x737d, 0x5489, 0xbec6, 0x0d27, 0x688c, 0x747e,
			0x548b, 0x51a9, 0x989a, 0x688c, 0x747d, 0x54ab, 0x508a, 0x9899,
			0xef00, 0x8809, 0xb002, 0x737c, 0x54e9, 0xbec6, 0x0d40, 0x688b,
			0x747d, 0x548c, 0x747f, 0x54e0, 0x747e, 0x548b, 0x51d9, 0x989a,
			0x688b, 0x747f, 0x54ac, 0x747d, 0x54d0, 0x707c, 0x54ab, 0x50ea,
			0x9899, 0xef00, 0x8809, 0x737d, 0x54a9, 0xbec6, 0x0d50, 0x688b,
			0x747e, 0x5490, 0x51a9, 0x98aa, 0x688b, 0x747d, 0x54a0, 0x50aa,
			0x98a9, 0xef00, 0x8809, 0xbec6, 0x0d5d, 0x6a8a, 0x6289, 0xbe1e,
			0x2d7b, 0xbfed, 0xbe02, 0xbe10, 0x98aa, 0x90a9, 0xef00, 0x8809,
			0xbec6, 0x0d64, 0x6880, 0x3480, 0x98a0, 0xef00, 0x907c, 0x817d,
			0x7c02, 0xbec5, 0x0181, 0x90a0, 0xbf00, 0x017d, 0x697c, 0x8809,
			0xbe09, 0x627d, 0x8812, 0xb901, 0xbec6, 0x0d78, 0x90aa, 0x9099,
			0xb801, 0x127c, 0x8809, 0xbe0a, 0xb802, 0x8818, 0x697d, 0xb882,
			0x907f, 0x8811, 0x247c, 0xb801, 0x8812, 0xaee0, 0x0001, 0xaee0,
			0xfff8, 0xaee0, 0x001c, 0xaee0, 0xffc8, 0xb002, 0x017f, 0x8b90,
			0xbec6, 0x0da5, 0xbe59, 0xbb07, 0xa2d0, 0x0dcd, 0xbe04, 0x7811,
			0x907e, 0xbe58, 0x10d0, 0xbb07, 0xa2d0, 0x0dcd, 0xbe04, 0x7813,
			0x2f7e, 0x999a, 0x9980, 0x3f99, 0x90aa, 0x9099, 0xb97e, 0x8809,
			0x017f, 0xb900, 0xbec6, 0x0db1, 0x62a0, 0x2f90, 0x90a0, 0xe711,
			0xbe0d, 0x98a0, 0x117c, 0x8809, 0xbe0a, 0xb801, 0x8818, 0x697d,
			0xb830, 0x8811, 0x227c, 0x8812, 0xaee0, 0x0001, 0xaed0, 0xfffc,
			0xb004, 0xbec6, 0x0dca, 0x1090, 0x2290, 0x3280, 0x3190, 0x2290,
			0x30e0, 0x90aa, 0x9099, 0xbf01, 0xef00, 0x0008, 0xffe4, 0x0038,
			0xffba, 0x0038, 0xffe4, 0x0008, 0xffff,
		};
		/* 0x0400 to 0x059f: the first quadrant of V.34's
		   constellation (Figure 5/V.34), 416 points in their
		   order, each one x in the high octet and y in the low
		   (the 3453C's 0x03c0) */
		static const w16 quad[] = {
			0x0101, 0xfd01, 0x01fd, 0xfdfd, 0x0105, 0x0501, 0xfd05, 0x05fd,
			0x0505, 0xf901, 0x01f9, 0xf9fd, 0xfdf9, 0xf905, 0x05f9, 0x0109,
			0x0901, 0xfd09, 0x09fd, 0xf9f9, 0x0509, 0x0905, 0xf501, 0x01f5,
			0xf909, 0xf5fd, 0x09f9, 0xfdf5, 0xf505, 0x05f5, 0x0909, 0x010d,
			0x0d01, 0xf5f9, 0xf9f5, 0xfd0d, 0x0dfd, 0x050d, 0x0d05, 0xf509,
			0x09f5, 0xf90d, 0x0df9, 0xf101, 0x01f1, 0xf1fd, 0xfdf1, 0xf5f5,
			0x090d, 0x0d09, 0xf105, 0x05f1, 0xf1f9, 0xf9f1, 0x0111, 0xf50d,
			0x1101, 0x0df5, 0xfd11, 0x11fd, 0xf109, 0x09f1, 0x0511, 0x1105,
			0xf911, 0x0d0d, 0x11f9, 0xf1f5, 0xf5f1, 0xed01, 0x01ed, 0x0911,
			0x1109, 0xedfd, 0xfded, 0xed05, 0x05ed, 0xf10d, 0x0df1, 0xf511,
			0xedf9, 0x11f5, 0xf9ed, 0x0115, 0xed09, 0x1501, 0x09ed, 0xfd15,
			0x15fd, 0xf1f1, 0x0d11, 0x110d, 0x0515, 0x1505, 0xedf5, 0xf5ed,
			0xf915, 0x15f9, 0xf111, 0x11f1, 0x0915, 0x1509, 0xed0d, 0xe901,
			0x0ded, 0x01e9, 0xe9fd, 0xfde9, 0xe905, 0x05e9, 0xf515, 0x15f5,
			0x1111, 0xe9f9, 0xf9e9, 0xedf1, 0xf1ed, 0x0d15, 0x150d, 0xe909,
			0x09e9, 0x0119, 0x1901, 0xfd19, 0x19fd, 0x0519, 0xed11, 0x1905,
			0xe9f5, 0x11ed, 0xf5e9, 0xf115, 0x15f1, 0xf919, 0x19f9, 0xe90d,
			0x0de9, 0x0919, 0x1909, 0xeded, 0x1115, 0x1511, 0xe501, 0x01e5,
			0xe5fd, 0xfde5, 0xf519, 0x19f5, 0xe505, 0xe9f1, 0xf1e9, 0x05e5,
			0xe5f9, 0xf9e5, 0x0d19, 0x190d, 0xed15, 0x15ed, 0xe509, 0x09e5,
			0xe911, 0x11e9, 0x011d, 0x1d01, 0xfd1d, 0xf119, 0x1dfd, 0xe5f5,
			0x19f1, 0xf5e5, 0x051d, 0x1d05, 0x1515, 0xf91d, 0x1df9, 0xe9ed,
			0xede9, 0xe50d, 0x0de5, 0x1119, 0x1911, 0x091d, 0x1d09, 0xe5f1,
			0xf1e5, 0xf51d, 0xe101, 0x1df5, 0x01e1, 0xe915, 0xe1fd, 0x15e9,
			0xfde1, 0xed19, 0xe105, 0x19ed, 0x05e1, 0x0d1d, 0x1d0d, 0xe1f9,
			0xf9e1, 0xe511, 0x11e5, 0xe109, 0x09e1, 0xe9e9, 0xf11d, 0x1519,
			0x1915, 0x1df1, 0xe1f5, 0xf5e1, 0x0121, 0x2101, 0xe5ed, 0xede5,
			0xfd21, 0x21fd, 0x0521, 0x2105, 0x111d, 0x1d11, 0xe10d, 0x0de1,
			0xf921, 0x21f9, 0xe919, 0x19e9, 0x0921, 0xe515, 0x2109, 0x15e5,
			0xe1f1, 0xf1e1, 0xed1d, 0x1ded, 0xf521, 0x21f5, 0xdd01, 0x01dd,
			0xddfd, 0xfddd, 0x1919, 0xe111, 0xdd05, 0x11e1, 0x05dd, 0x0d21,
			0x210d, 0xe5e9, 0xe9e5, 0xddf9, 0xf9dd, 0x151d, 0x1d15, 0xdd09,
			0x09dd, 0xf121, 0x21f1, 0xe1ed, 0xede1, 0xddf5, 0xf5dd, 0xe519,
			0x19e5, 0x0125, 0xe91d, 0x2501, 0x1de9, 0xfd25, 0x1121, 0x2111,
			0x25fd, 0x0525, 0xdd0d, 0x2505, 0x0ddd, 0xe115, 0x15e1, 0xf925,
			0x25f9, 0x0925, 0xed21, 0x2509, 0xddf1, 0x21ed, 0xf1dd, 0xe5e5,
			0x191d, 0x1d19, 0xf525, 0x25f5, 0xe1e9, 0xe9e1, 0xdd11, 0x11dd,
			0xd901, 0x01d9, 0x1521, 0x2115, 0xd9fd, 0xfdd9, 0x0d25, 0x250d,
			0xd905, 0x05d9, 0xe51d, 0xd9f9, 0x1de5, 0xf9d9, 0xe119, 0xdded,
			0x19e1, 0xeddd, 0xf125, 0x25f1, 0xd909, 0x09d9, 0xe921, 0x21e9,
			0xd9f5, 0xf5d9, 0x1125, 0x2511, 0xdd15, 0x15dd, 0x0129, 0x1d1d,
			0x2901, 0xfd29, 0xd90d, 0x29fd, 0xe1e5, 0xe5e1, 0x0dd9, 0x0529,
			0x2905, 0x1921, 0x2119, 0xf929, 0xed25, 0x29f9, 0x25ed, 0xd9f1,
			0xf1d9, 0xdde9, 0xe9dd, 0x0929, 0x2909, 0xf529, 0xe11d, 0x29f5,
			0x1de1, 0x1525, 0x2515, 0xd911, 0x11d9, 0xe521, 0x21e5, 0x0d29,
			0xdd19, 0x290d, 0xd501, 0x19dd, 0x01d5, 0xd5fd, 0xfdd5, 0xd505,
			0x05d5, 0xd9ed, 0xedd9, 0xe925, 0xd5f9, 0x25e9, 0xf9d5, 0xf129,
			0x29f1, 0xe1e1, 0x1d21, 0x211d, 0xd509, 0x09d5, 0xdde5, 0xe5dd,
			0xd915, 0x15d9, 0x1129, 0x2911, 0xd5f5, 0xf5d5, 0x1925, 0x2519,
			0xd50d, 0x0dd5, 0x012d, 0x2d01, 0xfd2d, 0x2dfd, 0xed29, 0x29ed,
			0x052d, 0xe121, 0x2d05, 0xd9e9, 0x21e1, 0xe9d9, 0xdd1d, 0x1ddd,
			0xf92d, 0x2df9, 0xd5f1, 0xf1d5, 0xe525, 0x25e5, 0x092d, 0x2d09,
		};
		memcpy(&dsp.ROM[0x0030], fir, sizeof(fir));
		memcpy(&dsp.ROM[0x0080], probe, sizeof(probe));
		memcpy(&dsp.ROM[0x0728], pp, sizeof(pp));
		memcpy(&dsp.ROM[0x0a10], t0a10, sizeof(t0a10));
		if (getenv("TC110_T0E0"))
			for (int k = 0; k < 16; k++) {
				char c[2] = { getenv("TC110_T0E0")[k], 0 };
				dsp.ROM[0x00e0 + k] = (w16)strtol(c, nullptr, 16);
			}
		memcpy(&dsp.ROM[0x0a38], pre, sizeof(pre));
		if (getenv("TC110_PRE_FLAT"))
			for (int k = 0; k < 11; k++)
				for (int j = 0; j < 5; j++)
					dsp.ROM[0x0a38 + 5 * k + j] = j == 2 ? 16384 : 0;
		/* the carriers (0x0a74): for each of 4, the address of its
		   table and the table's length; the tables (0x0a7c to 0x0ac9,
		   which they fill) cosine and sine pairs of one period,
		   read from the end down.  1/4, 2/9, 1/5 and 4/21 of the
		   codec's rate: 1800 Hz at 2400 baud, 1600 Hz at 2400 and
		   1829 Hz at 2743, 1920 Hz at 3200, 1829 Hz at 3200 (and the
		   other rates' carriers, at their codec rates).  Not of the
		   ROM: made from how the firmware reads them */
		static const int car[4][2] = { { 1, 4 }, { 2, 9 }, { 1, 5 },
					       { 4, 21 } };
		unsigned at = 0x0a7c;
		for (int n = 0; n < 4; n++) {
			int per = car[n][1];
			dsp.ROM[0x0a74 + 2 * n] = (w16)at;
			dsp.ROM[0x0a75 + 2 * n] = (w16)(2 * per);
			for (int j = 0; j < per; j++) {
				double th = -carrier_sign * 2 * M_PI * car[n][0] *
					    j / per;
				dsp.ROM[at++] = (w16)lrint(32767 * cos(th));
				dsp.ROM[at++] = (w16)lrint(32767 * sin(th));
			}
		}

		memcpy(&dsp.ROM[0x0400], quad, sizeof(quad));
		memcpy(&dsp.ROM[0x0aca], lib, sizeof(lib));
		memcpy(&dsp.ROM[0x0bb1], fft, sizeof(fft));
		memcpy(&dsp.ROM[0x0cd0], lms, sizeof(lms));
		/* (0x0b12 and 0x0b2c here are not the library's: the
		   firmware gives them the whole phase, which the library's
		   do not take; rom_call()'s) */
		dsp.ROM[0x0b12] = dsp.ROM[0x0b2c] = 0xffff;
	}

	/* a port read by the DSP */
	w16 io_rd(unsigned p)
	{
		w16 v = 0;
		switch (p) {
		case 0x8050:
			if (ur_[3] & 0x80) {
				v = ur_[16];
			} else if (!rx_.empty()) {
				v = rx_.front();
				rx_.pop_front();
			}
			break;
		case 0x8051:
			v = (ur_[3] & 0x80) ? ur_[17] : ur_[1];
			break;
		case 0x8052:
			v = (w16)(0xc0 | ((!rx_.empty() && (ur_[1] & 1)) ? 0x04 :
					  (thre_ && (ur_[1] & 2)) ? 0x02 : 0x01));
			if ((v & 0x0f) == 2)
				thre_ = false;
			break;
		case 0x8053:
		case 0x8054:
		case 0x8057:
		case 0x805c:
			v = ur_[p & 15];
			break;
		case 0x8055:
			v = (w16)((rx_.empty() ? 0 : 1) | 0x20);
			break;
		case 0x8056:
			v = msr_;
			msr_ &= 0xf0;
			break;
		case 0x8058:
			v = p58_ & 0xf0;
			break;
		case 0x8059:
			v = line_ok ? 1 : 0;
			break;
		case 0x805b:
			v = events();
			ev_ = 0;
			break;
		case 0x805f:
			v = (w16)((ee_.line() ? 0x80 : 0) | 0x40);
			break;
		}
		return v;
	}

	/* a port written by the DSP */
	void io_wr(unsigned p, w16 v)
	{
		if (p == 0x8065) {
			p65_ = v;
			codec_clock();
			return;
		}
		if (p == 0x806f) {
			page = v & 7;
			dsp.win = &flash[(size_t)page << 15];
			return;
		}
		if (p == 0x8058 || p == 0x805f) {
			if (p == 0x8058)
				p58_ = v;
			else
				p5f_ = v;
			ee_.set((p58_ >> 6) & 1,
				(p5f_ & 0x10) ? 1 : ((p5f_ >> 7) & 1));
		}
		if (p < 0x8050 || p >= 0x8060)
			return;
		if ((ur_[3] & 0x80) && (p == 0x8050 || p == 0x8051)) {
			ur_[p == 0x8050 ? 16 : 17] = v;
			return;
		}
		if (p == 0x8050) {
			/* the transmitter: the octet out, empty again an
			   octet's time later (as fast as the DSP may write) */
			if (to_dte)
				to_dte((u8)v);
			thre_ = false;
			thre_in_ = 2000;
			return;
		}
		if (p == 0x8051 && (v & 2) && thre_in_ < 0)
			thre_ = true;
		ur_[p & 15] = v;
	}

	/* a word to the codec's register (bits 8 to 12 its number, 0 to 7
	   its value): registers 1 (A) and 2 (B) set the rate (codec_clock()) */
	void codec_reg(w16 v)
	{
		unsigned r = (v >> 8) & 0x1f;
		if (log && dbg >= 2) {
			char s[48];
			snprintf(s, sizeof(s), "codec register word %04x", v);
			log(s);
		}
		if (v & 0x2000 || r >= 8)
			return;
		creg_[r] = v & 0xff;
		if (r == 1 || r == 2)
			codec_clock();
	}

	/* the codec's rate: 25.92 MHz / (port 0x8065 + 1) / (A B), with A
	   and B its registers 1 and 2.  (The boot's 0x11, 10 and 20 give
	   7200 Hz, which the answer tone shows; V.34's 0x0e, 10 and 18
	   give 9600 Hz, 16 samples to the INFO receiver's bit at 600
	   bit/s.) */
	void codec_clock()
	{
		if (!creg_[1] || !creg_[2])
			return;
		double hz = 25.92e6 / ((p65_ & 0xff) + 1) / (creg_[1] * creg_[2]);
		if (hz != codec_rate && log && dbg) {
			char s[64];
			snprintf(s, sizeof(s), "codec at %.2f Hz", hz);
			log(s);
		}
		codec_rate = hz;
	}

	/* after the DSP's write of data 0x20 to 0x2f (old: the word's value
	   before) */
	void mmr_wr(C5x::u32 a, w16 old)
	{
		if (a == 0x22) {
			/* SPC: its status bits are the port's own */
			w16 v = dsp.D[0x22];
			dsp.D[0x22] = (w16)((v & ~0x3c00) | (old & 0x3c00));
			if (!(v & 0x40))
				dsp.D[0x22] |= 0x0800;
		}
		if (a == 0x21) {
			dsp.D[0x22] &= (w16)~0x0800;
			w16 v = dsp.D[0x21];
			if (sec_ == 2) {
				/* the secondary frame's word: a codec
				   register */
				sec_ = 0;
				codec_reg(v);
				return;
			}
			dac_ = (short)(v & ~3);
			if (on_dxr)
				on_dxr(v);
			if ((v & 3) == 3) {
				/* a secondary frame asked for: its XINT half
				   a sample later */
				sec_ = 1;
				sec_in_ = (long)(ips / codec_rate / 2);
			}
		}
	}
};

}

#endif
