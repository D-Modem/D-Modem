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
 *
 *    The linear and ADPCM formats run at 7200, 8000 or 11025 samples/s,
 *    the G.711 ones at 8000.  No silence compression, no timing marks.
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

enum Coding : u8 { SIGNED8, UNSIGNED8, ULAW, ALAW, LINEAR16, IMA_ADPCM };

/* a format of +VSM=?: its number, its V.253 label, its coding, its bits
   a sample, the rates it runs at (0: none) */
struct Format {
	u16 cml;
	const char *label;
	Coding coding;
	u8 bps;
	u16 rates[3];
};

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

/* one direction's coder in a format: samples to octets (encode) or
   octets to samples (decode), either one a coder */
struct Codec {
	Coding coding = UNSIGNED8;
	Ima ima;
	/* the octet waiting for its other half: the low octet of a 16 bit
	   sample, a first ADPCM nibble */
	u8 held = 0;
	bool holding = false;

	void reset(Coding c)
	{
		coding = c;
		ima = Ima();
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
		case IMA_ADPCM: {
			u8 c = ima.encode(x);
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
