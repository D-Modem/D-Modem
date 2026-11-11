/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    voice_codec.h - the sample formats of AT+VSM (V.253 10.2.8): what
 *                    the voice engine's samples to and from the DTE are
 *
 *    V.253's Table 17 numbers 0 to 127; 128 to 132 are the numbers
 *    Conexant's modems give their formats, so that software written
 *    for those finds them where it looks:
 *
 *      0  SIGNED PCM     8 bits, two's complement
 *      1  UNSIGNED PCM   8 bits, offset by 128 (the default)
 *      4  G.711U         mu-law
 *      5  G.711A         A-law
 *    128  8-BIT LINEAR   as 1
 *    129  16-BIT LINEAR  16 bits signed, the low octet first
 *    130  8-BIT ALAW     as 5
 *    131  8-BIT ULAW     as 4
 *    132  IMA ADPCM      4 bits, the IMA/DVI algorithm, two samples an
 *                        octet, the first in the low nibble (as a WAV
 *                        file's), its state from zero at each +VTX,
 *                        +VRX, +VTR
 *    133  G.726 ADPCM    G.726 at 32 kbit/s (G.721), 4 bits, two samples
 *                        an octet, the first in the low nibble: US
 *                        Robotics' ADPCM (#VSM=129, #VBS=4), its state
 *                        from the start at each command as 132's
 *
 *    The linear and IMA ADPCM formats run at 7200, 8000 or 11025
 *    samples/s, the G.711 and G.726 ones at 8000.  No silence
 *    compression, no timing marks.
 *
 *    Header only: the core (AT+VSM) and the voice engine have it.
 *
 */
#ifndef DSP_VOICE_CODEC_H
#define DSP_VOICE_CODEC_H

#include "dm_types.h"
#include "g711.h"

namespace dsp {
namespace voice {

enum Coding : u8 { SIGNED8, UNSIGNED8, ULAW, ALAW, LINEAR16, IMA_ADPCM,
		   G726_32 };

/* a format of +VSM=?: its number, its V.253 label, its coding, its bits
   a sample, the rates it runs at (0: none) */
struct Format {
	u16 cml;
	const char *label;
	Coding coding;
	u8 bps;
	u16 rates[3];
};

/* the formats, *n of them, in +VSM=?'s order */
inline const Format *formats(int *n)
{
	static const Format f[] = {
		{ 0, "SIGNED PCM", SIGNED8, 8, { 7200, 8000, 11025 } },
		{ 1, "UNSIGNED PCM", UNSIGNED8, 8, { 7200, 8000, 11025 } },
		{ 4, "G.711U", ULAW, 8, { 8000, 0, 0 } },
		{ 5, "G.711A", ALAW, 8, { 8000, 0, 0 } },
		{ 128, "8-BIT LINEAR/D-Modem", UNSIGNED8, 8,
		  { 7200, 8000, 11025 } },
		{ 129, "16-BIT LINEAR/D-Modem", LINEAR16, 16,
		  { 7200, 8000, 11025 } },
		{ 130, "8-BIT ALAW/D-Modem", ALAW, 8, { 8000, 0, 0 } },
		{ 131, "8-BIT ULAW/D-Modem", ULAW, 8, { 8000, 0, 0 } },
		{ 132, "IMA ADPCM/D-Modem", IMA_ADPCM, 4,
		  { 7200, 8000, 11025 } },
		{ 133, "G.726 ADPCM/D-Modem", G726_32, 4, { 8000, 0, 0 } },
	};
	*n = static_cast<int>(sizeof(f) / sizeof(f[0]));
	return f;
}

/* the format numbered cml, nullptr for none */
inline const Format *format(unsigned cml)
{
	int n;
	const Format *f = formats(&n);
	for (int i = 0; i < n; i++)
		if (f[i].cml == cml)
			return &f[i];
	return nullptr;
}

/* whether the format numbered cml runs at vsr samples/s */
inline bool runs_at(unsigned cml, u32 vsr)
{
	const Format *f = format(cml);
	if (!f)
		return false;
	for (u16 r : f->rates)
		if (r && r == vsr)
			return true;
	return false;
}

/* IMA/DVI ADPCM, one direction's state */
struct Ima {
	s32 pred = 0;
	s16 index = 0;

	/* the step size at index i (0 to 88) */
	static s16 step(s16 i)
	{
		static const s16 t[89] = {
			7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25,
			28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
			97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
			279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
			724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
			1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
			4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
			9493, 10442, 11487, 12635, 13899, 15289, 16818,
			18500, 20350, 22385, 24623, 27086, 29794, 32767,
		};
		return t[i];
	}
	/* a code's sample, the state moved on */
	s16 decode(u8 code)
	{
		static const s16 adjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
		s32 st = step(index), diff = st >> 3;

		if (code & 4)
			diff += st;
		if (code & 2)
			diff += st >> 1;
		if (code & 1)
			diff += st >> 2;
		pred += (code & 8) ? -diff : diff;
		if (pred > 32767)
			pred = 32767;
		else if (pred < -32768)
			pred = -32768;
		index += adjust[code & 7];
		if (index < 0)
			index = 0;
		else if (index > 88)
			index = 88;
		return static_cast<s16>(pred);
	}
	/* x's code, the state moved on as the decoder's will */
	u8 encode(s16 x)
	{
		s32 st = step(index), d = x - pred;
		u8 code = 0;

		if (d < 0) {
			code = 8;
			d = -d;
		}
		if (d >= st) {
			code |= 4;
			d -= st;
		}
		st >>= 1;
		if (d >= st) {
			code |= 2;
			d -= st;
		}
		st >>= 1;
		if (d >= st)
			code |= 1;
		decode(code);
		return code;
	}
};

/* G.726 at 32 kbit/s (G.721 of 1988), one direction's state: the
   adaptive quantizer and the predictor of two poles and six zeros
   (G.726 4.2), in the Recommendation's fixed point; samples are 16 bit
   linear, the coder's 14 bits shifted up */
struct G726 {
	s32 yl = 34816;          /* the slow scale factor (YL) */
	s16 yu = 544;            /* the fast one (YU) */
	s16 dms = 0, dml = 0;    /* short and long term means of F[I] */
	s16 ap = 0;              /* the speed control (AP) */
	s16 a[2] = { 0, 0 };     /* the pole coefficients */
	s16 b[6] = { 0, 0, 0, 0, 0, 0 };  /* the zero ones */
	s16 pk[2] = { 0, 0 };    /* the signs of the last two DQ + SEZ */
	/* the last six quantized differences and two reconstructed
	   signals, in G.726's floating format (sign, 4 exponent bits,
	   6 mantissa bits) */
	s16 dq[6] = { 32, 32, 32, 32, 32, 32 };
	s16 sr[2] = { 32, 32 };
	s16 td = 0;              /* a tone was detected */

	/* the i of table[i - 1] <= v < table[i], among n */
	static int quan(int v, const s16 *table, int n)
	{
		int i;
		for (i = 0; i < n; i++)
			if (v < table[i])
				break;
		return i;
	}
	/* 1, 2, 4, ... 0x4000: quan()'s table for a value's exponent */
	static const s16 *power2()
	{
		static const s16 t[15] = { 1, 2, 4, 8, 0x10, 0x20, 0x40,
			0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000,
			0x4000 };
		return t;
	}
	/* a coefficient an times a value srn of the floating format
	   (FMULT) */
	static int fmult(int an, int srn)
	{
		int anmag = an > 0 ? an : ((-an) & 0x1FFF);
		int anexp = quan(anmag, power2(), 15) - 6;
		int anmant = anmag == 0 ? 32 :
			anexp >= 0 ? anmag >> anexp : anmag << -anexp;
		int wanexp = anexp + ((srn >> 6) & 0xF) - 13;
		int wanmant = (anmant * (srn & 077) + 0x30) >> 4;
		int r = wanexp >= 0 ? (wanmant << wanexp) & 0x7FFF :
			wanmant >> -wanexp;
		return (an ^ srn) < 0 ? -r : r;
	}
	/* v as the floating format */
	static s16 to_float(int v)
	{
		if (v == 0)
			return 0x20;
		if (v <= -32768)
			return static_cast<s16>(0xFC20);
		int mag = v > 0 ? v : -v;
		int e = quan(mag, power2(), 15);
		int f = (e << 6) + ((mag << 6) >> e);
		return static_cast<s16>(v > 0 ? f : f - 0x400);
	}
	/* the six zeros' part of the prediction (SEZI) */
	int zeros() const
	{
		int s = 0;
		for (int i = 0; i < 6; i++)
			s += fmult(b[i] >> 2, dq[i]);
		return s;
	}
	/* the two poles' part (SEZI's addition to SEI) */
	int poles() const
	{
		return fmult(a[1] >> 2, sr[1]) + fmult(a[0] >> 2, sr[0]);
	}
	/* the quantizer's scale factor (Y) */
	int step() const
	{
		if (ap >= 256)
			return yu;
		int y = yl >> 6, dif = yu - y, al = ap >> 2;
		if (dif > 0)
			y += (dif * al) >> 6;
		else if (dif < 0)
			y += (dif * al + 0x3F) >> 6;
		return y;
	}
	/* the difference for code i (DQ, sign and magnitude: the sign
	   0x8000 below zero) */
	static int reconstruct(int i, int y)
	{
		static const s16 dqln[16] = { -2048, 4, 135, 213, 273, 323,
			373, 425, 425, 373, 323, 273, 213, 135, 4, -2048 };
		int sign = i & 8, dql = dqln[i] + (y >> 2);
		if (dql < 0)
			return sign ? -0x8000 : 0;
		int dex = (dql >> 7) & 15, dqt = 128 + (dql & 127);
		int dq_ = (dqt << 7) >> (14 - dex);
		return sign ? dq_ - 0x8000 : dq_;
	}
	/* the state after code i: y its scale factor, dq_ its
	   difference, sr_ the signal, dqsez DQ + SEZ */
	void update(int i, int y, int dq_, int sr_, int dqsez)
	{
		static const s16 wi[16] = { -12, 18, 41, 64, 112, 198, 355,
			1122, 1122, 355, 198, 112, 64, 41, 18, -12 };
		static const s16 fi[16] = { 0, 0, 0, 0x200, 0x200, 0x200,
			0x600, 0xE00, 0xE00, 0x600, 0x200, 0x200, 0x200, 0,
			0, 0 };
		int pk0 = dqsez < 0 ? 1 : 0;
		int mag = dq_ & 0x7FFF;
		/* a transition from a tone (TR) */
		int ylint = static_cast<int>(yl >> 15);
		int ylfrac = static_cast<int>(yl >> 10) & 0x1F;
		int thr1 = (32 + ylfrac) << ylint;
		int thr2 = ylint > 9 ? 31 << 10 : thr1;
		int dqthr = (thr2 + (thr2 >> 1)) >> 1;
		bool tr = td && mag > dqthr;
		/* the scale factors */
		int u = y + (((wi[i] << 5) - y) >> 5);
		yu = static_cast<s16>(u < 544 ? 544 : u > 5120 ? 5120 : u);
		yl += yu + ((-yl) >> 6);
		/* the predictor's coefficients */
		int a2p = 0;
		if (tr) {
			a[0] = a[1] = 0;
			for (s16 &c : b)
				c = 0;
		} else {
			int pks1 = pk0 ^ pk[0];
			a2p = a[1] - (a[1] >> 7);
			if (dqsez != 0) {
				int fa1 = pks1 ? a[0] : -a[0];
				if (fa1 < -8191)
					a2p -= 0x100;
				else if (fa1 > 8191)
					a2p += 0xFF;
				else
					a2p += fa1 >> 5;
				if (pk0 ^ pk[1]) {
					if (a2p <= -12160)
						a2p = -12288;
					else if (a2p >= 12416)
						a2p = 12288;
					else
						a2p -= 0x80;
				} else if (a2p <= -12416) {
					a2p = -12288;
				} else if (a2p >= 12160) {
					a2p = 12288;
				} else {
					a2p += 0x80;
				}
			}
			a[1] = static_cast<s16>(a2p);
			int a1 = a[0] - (a[0] >> 8);
			if (dqsez != 0)
				a1 += pks1 ? -192 : 192;
			int a1ul = 15360 - a2p;
			if (a1 < -a1ul)
				a1 = -a1ul;
			else if (a1 > a1ul)
				a1 = a1ul;
			a[0] = static_cast<s16>(a1);
			for (int k = 0; k < 6; k++) {
				int c = b[k] - (b[k] >> 8);
				if (dq_ & 0x7FFF)
					c += (dq_ ^ dq[k]) >= 0 ? 128 : -128;
				b[k] = static_cast<s16>(c);
			}
		}
		/* the delay lines */
		for (int k = 5; k > 0; k--)
			dq[k] = dq[k - 1];
		if (mag == 0) {
			dq[0] = static_cast<s16>(dq_ >= 0 ? 0x20 : 0xFC20);
		} else {
			int e = quan(mag, power2(), 15);
			int f = (e << 6) + ((mag << 6) >> e);
			dq[0] = static_cast<s16>(dq_ >= 0 ? f : f - 0x400);
		}
		sr[1] = sr[0];
		sr[0] = to_float(sr_);
		pk[1] = pk[0];
		pk[0] = static_cast<s16>(pk0);
		/* tones (TD), then the speed control */
		td = !tr && a2p < -11776;
		dms = static_cast<s16>(dms + ((fi[i] - dms) >> 5));
		dml = static_cast<s16>(dml + (((fi[i] << 2) - dml) >> 7));
		int d = (dms << 2) - dml;
		if (tr)
			ap = 256;
		else if (y < 1536 || td || (d < 0 ? -d : d) >= (dml >> 3))
			ap = static_cast<s16>(ap + ((0x200 - ap) >> 4));
		else
			ap = static_cast<s16>(ap + ((-ap) >> 4));
	}
	/* code i's sample, the state moved on */
	s16 decode(u8 i)
	{
		i &= 0x0f;
		int sezi = zeros(), sez = sezi >> 1;
		int se = (sezi + poles()) >> 1;
		int y = step();
		int d = reconstruct(i, y);
		int s = d < 0 ? se - (d & 0x3FFF) : se + d;
		update(i, y, d, s, s - se + sez);
		s <<= 2;
		return static_cast<s16>(s > 32767 ? 32767 :
					s < -32768 ? -32768 : s);
	}
	/* x's code, the state moved on as the decoder's will */
	u8 encode(s16 x)
	{
		static const s16 qtab[7] = { -124, 80, 178, 246, 300, 349,
			400 };
		int sl = x >> 2;
		int sezi = zeros(), sez = sezi >> 1;
		int se = (sezi + poles()) >> 1;
		int dif = sl - se;
		int y = step();
		/* the quantizer (G.726 4.2.3), its log of |dif| against the
		   table */
		int dqm = dif < 0 ? -dif : dif;
		int e = quan(dqm >> 1, power2(), 15);
		int mant = ((dqm << 7) >> e) & 0x7F;
		int dln = (e << 7) + mant - (y >> 2);
		int k = quan(dln, qtab, 7);
		int i = dif < 0 ? 15 - k : k == 0 ? 15 : k;
		int d = reconstruct(i, y);
		int s = d < 0 ? se - (d & 0x3FFF) : se + d;
		update(i, y, d, s, s + sez - se);
		return static_cast<u8>(i);
	}
};

/* one direction's coder in a format: samples to octets (encode) or
   octets to samples (decode), either one a coder */
struct Codec {
	Coding coding = UNSIGNED8;
	Ima ima;
	G726 g726;
	/* the octet waiting for its other half: the low octet of a 16 bit
	   sample, a first ADPCM nibble */
	u8 held = 0;
	bool holding = false;

	/* a coder of c, its state from the start */
	void reset(Coding c)
	{
		coding = c;
		ima = Ima();
		g726 = G726();
		held = 0;
		holding = false;
	}

	/* the octets of sample x into b (0 to 2 of them; 0 for an ADPCM
	   sample that waits for the next): how many */
	int encode(s16 x, u8 *b)
	{
		switch (coding) {
		case SIGNED8:
			b[0] = static_cast<u8>(x >> 8);
			return 1;
		case ULAW:
			b[0] = g711::lin2ulaw(x);
			return 1;
		case ALAW:
			b[0] = g711::lin2alaw(x);
			return 1;
		case LINEAR16:
			b[0] = static_cast<u8>(x & 0xff);
			b[1] = static_cast<u8>((x >> 8) & 0xff);
			return 2;
		case IMA_ADPCM:
		case G726_32: {
			u8 c = coding == IMA_ADPCM ? ima.encode(x) :
				g726.encode(x);
			if (!holding) {
				held = c;
				holding = true;
				return 0;
			}
			holding = false;
			b[0] = static_cast<u8>(held | (c << 4));
			return 1;
		}
		case UNSIGNED8:
		default:
			b[0] = static_cast<u8>((x >> 8) + 128);
			return 1;
		}
	}

	/* the samples of octet c into x (0 to 2 of them; 0 for the low
	   octet of a 16 bit sample): how many */
	int decode(u8 c, s16 *x)
	{
		switch (coding) {
		case SIGNED8:
			x[0] = static_cast<s16>(static_cast<s8>(c) * 256);
			return 1;
		case ULAW:
			x[0] = g711::ulaw2lin(c);
			return 1;
		case ALAW:
			x[0] = g711::alaw2lin(c);
			return 1;
		case LINEAR16:
			if (!holding) {
				held = c;
				holding = true;
				return 0;
			}
			holding = false;
			x[0] = static_cast<s16>(static_cast<u16>(held) |
						static_cast<u16>(c) << 8);
			return 1;
		case IMA_ADPCM:
			x[0] = ima.decode(c & 0x0f);
			x[1] = ima.decode(c >> 4);
			return 2;
		case G726_32:
			x[0] = g726.decode(c & 0x0f);
			x[1] = g726.decode(c >> 4);
			return 2;
		case UNSIGNED8:
		default:
			x[0] = static_cast<s16>((static_cast<s16>(c) - 128) *
						256);
			return 1;
		}
	}
};

} // namespace voice
} // namespace dsp

#endif
