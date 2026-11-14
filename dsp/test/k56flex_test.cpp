/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_test.cpp - K56flex's data mode (dsp/k56flex.h): the encoder
 *                       into the decoder at every rate, pad, law and
 *                       robbed-bit pattern, the bits each frame takes,
 *                       the shaping's DC, the robbed-bit and pad
 *                       models against SM56's TRN1B templates, and the
 *                       codes of one frame against those Motorola's
 *                       SM56 client decodes
 *                       (k56flex_sm56.cpp checks all of them there)
 *
 */

#include <stdlib.h>

#include <vector>

#include <gtest/gtest.h>

#include "g711.h"
#include "k56flex.h"

using namespace dsp;

namespace {

/* nframes frames of random bits through the encoder and the decoder;
   *rds: the running sum at the end, *nbits: the bits sent */
void round_trip(const K56Line &l, s32 nframes, s32 *rds = nullptr,
		s32 *nbits = nullptr)
{
	K56Enc e;
	K56Dec d;
	ASSERT_EQ(e.init(l), 0);
	ASSERT_EQ(d.init(l), 0);
	srand(static_cast<unsigned>(l.rate + l.pad + l.robbed));
	s32 n = 0;
	for (s32 k = 0; k < nframes; k++) {
		unsigned char in[K56_MAXBITS], out[K56_MAXBITS], code[8];
		s16 nb = e.frame_bits();
		for (s16 i = 0; i < nb; i++)
			in[i] = static_cast<unsigned char>(rand() & 1);
		e.frame(in, code);
		/* through the network as G.711, robbed bits set to 1 */
		for (s16 i = 0; i < 8; i++) {
			unsigned char g = k56_g711(code[i], l.alaw);
			if (l.rate > 34000 && (l.robbed >> ((8 * (k % 3) + i) % 6)) & 1)
				g |= 1;
			code[i] = k56_code(g, l.alaw);
		}
		ASSERT_EQ(d.frame(code, out), nb) << "frame " << k;
		for (s16 i = 0; i < nb; i++)
			ASSERT_EQ(out[i], in[i]) << "frame " << k << " bit " << i;
		n += nb;
	}
	EXPECT_EQ(d.errors, 0);
	if (rds)
		*rds = e.rds;
	if (nbits)
		*nbits = n;
}

/* every rate, pad, law and a set of robbed-bit patterns: the bits come
   back */
TEST(K56flex, RoundTrip)
{
	static const unsigned char robbed[] = { 0, 1, 3, 7, 0x15, 0x0f, 0x1f,
						0x3f, 0x21, 0x2a };
	for (s32 rate = 32000; rate <= 56000; rate += 2000)
		for (s16 pad = 0; pad < 3; pad++)
			for (s16 alaw = 0; alaw < 2; alaw++)
				for (unsigned char rb : robbed) {
					SCOPED_TRACE(testing::Message() << rate << " pad " << pad
						     << " alaw " << alaw << " robbed " << int(rb));
					round_trip({ rate, pad, alaw, rb }, 300);
				}
}

/* the rate without robbed bits; robbed-bit lines lose 2, 4, 4, 6, 8 or 8
   bits a frame for 1 to 6 robbed symbols in 6 */
TEST(K56flex, BitsPerFrame)
{
	for (s32 rate = 32000; rate <= 56000; rate += 2000) {
		s32 n;
		round_trip({ rate, 0, 0, 0 }, 600, nullptr, &n);
		EXPECT_EQ(n, 600 * rate / 1000);
	}
	static const struct { unsigned char rb; s32 loss; } t[] = {
		{ 0x01, 2 }, { 0x03, 4 }, { 0x07, 4 }, { 0x15, 4 }, { 0x0f, 6 },
		{ 0x1f, 8 }, { 0x3f, 8 } };
	for (auto &c : t) {
		s32 n;
		round_trip({ 52000, 0, 0, c.rb }, 600, nullptr, &n);
		EXPECT_EQ(n, 600 * (52 - c.loss)) << int(c.rb);
	}
	/* and below 36000 bit/s none are robbed */
	s32 n;
	round_trip({ 34000, 0, 0, 0x3f }, 600, nullptr, &n);
	EXPECT_EQ(n, 600 * 34);
}

/* the shaping keeps the running sum near 0 */
TEST(K56flex, Shaping)
{
	for (s32 rate = 32000; rate <= 56000; rate += 2000) {
		s32 rds;
		round_trip({ rate, 0, 0, 0 }, 3000, &rds);
		EXPECT_LT(abs(rds), 200000) << rate;
	}
}

/* rates and pads that K56flex does not have */
TEST(K56flex, BadLine)
{
	K56Enc e;
	EXPECT_EQ(e.init({ 30000, 0, 0, 0 }), -1);
	EXPECT_EQ(e.init({ 57000, 0, 0, 0 }), -1);
	EXPECT_EQ(e.init({ 58000, 0, 0, 0 }), -1);
	EXPECT_EQ(e.init({ 56000, 3, 0, 0 }), -1);
}

/* codes to G.711 octets and back, the sign as the octet's */
TEST(K56flex, G711)
{
	for (s16 alaw = 0; alaw < 2; alaw++)
		for (s32 c = 0; c < 256; c++) {
			unsigned char g = k56_g711(static_cast<unsigned char>(c), alaw);
			EXPECT_EQ(k56_code(g, alaw), c);
			/* the octet's value is the code's */
			s32 v = k56_value(static_cast<unsigned char>(c), alaw);
			s32 ref = alaw ? (((g ^ 0x55) & 0x80) ? 1 : -1) : ((g & 0x80) ? 1 : -1);
			if (v) {
				EXPECT_EQ(v > 0 ? 1 : -1, ref) << c;
			}
		}
}

/* the first frames of all-zero data at 56000 bit/s: what SM56 decodes
   back to zeros (k56flex_sm56.cpp) */
TEST(K56flex, Zeros)
{
	K56Enc e;
	ASSERT_EQ(e.init({ 56000, 0, 0, 0 }), 0);
	unsigned char in[K56_MAXBITS] = {}, code[8];
	static const unsigned char want[2][8] = {
		{ 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x9c },
		{ 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x9c } };
	for (auto &w : want) {
		e.frame(in, code);
		for (s16 i = 0; i < 8; i++)
			EXPECT_EQ(code[i], w[i]) << i;
	}
}

/* the robbed-bit and pad models against SM56's TRN1B templates
   (expectTRN1B_Mlaw and expectTRN1B_Alaw in sm56.ko: law, pad, a, b,
   the six TRN1B magnitudes' values, mu-law in 14 bit units, A-law in
   13).  The rows SM56 leaves at zero are not in the list */
TEST(K56flex, RbsTemplates)
{
	struct Row { s16 law, pad, a, b; s16 v[6]; };
	static const Row rows[] = {
		{ 0, 0, 2, 0, { 527, 527, 847, 911, 1087, 1087 } },
		{ 0, 0, 2, 1, { 495, 495, 815, 879, 1023, 1023 } },
		{ 0, 0, 2, 2, { 495, 527, 847, 879, 1023, 1087 } },
		{ 0, 0, 2, 3, { 511, 511, 831, 895, 1055, 1055 } },
		{ 0, 1, 1, 1, { 327, 327, 559, 623, 687, 687 } },
		{ 0, 1, 1, 2, { 343, 343, 591, 623, 719, 719 } },
		{ 0, 1, 1, 3, { 335, 335, 575, 639, 703, 703 } },
		{ 0, 1, 2, 0, { 343, 375, 591, 655, 719, 783 } },
		{ 0, 1, 2, 1, { 327, 359, 559, 623, 687, 751 } },
		{ 0, 1, 2, 2, { 343, 375, 591, 623, 719, 783 } },
		{ 0, 1, 2, 3, { 335, 367, 575, 639, 703, 767 } },
		{ 0, 2, 1, 1, { 231, 231, 391, 423, 495, 495 } },
		{ 0, 2, 1, 2, { 247, 247, 407, 439, 527, 527 } },
		{ 0, 2, 1, 3, { 239, 239, 399, 431, 511, 511 } },
		{ 0, 2, 2, 0, { 247, 279, 439, 439, 527, 591 } },
		{ 0, 2, 2, 1, { 231, 263, 423, 423, 495, 559 } },
		{ 0, 2, 2, 2, { 247, 263, 423, 439, 527, 559 } },
		{ 0, 2, 2, 3, { 239, 271, 431, 431, 511, 575 } },
		{ 1, 0, 2, 0, { 280, 280, 440, 472, 560, 560 } },
		{ 1, 0, 2, 1, { 264, 264, 424, 456, 528, 528 } },
		{ 1, 0, 2, 2, { 264, 280, 440, 456, 528, 560 } },
		{ 1, 1, 1, 0, { 188, 188, 312, 344, 376, 376 } },
		{ 1, 1, 1, 1, { 180, 180, 296, 328, 360, 360 } },
		{ 1, 1, 1, 2, { 188, 188, 296, 328, 376, 376 } },
		{ 1, 1, 2, 0, { 188, 204, 312, 344, 376, 408 } },
		{ 1, 1, 2, 1, { 180, 196, 296, 328, 360, 392 } },
		{ 1, 1, 2, 2, { 188, 196, 312, 328, 376, 392 } },
		{ 1, 2, 1, 2, { 132, 132, 212, 228, 264, 264 } },
		{ 1, 2, 2, 0, { 140, 140, 220, 236, 280, 280 } },
		{ 1, 2, 2, 2, { 132, 140, 220, 228, 264, 280 } } };
	static const unsigned char mag[6] = { 0x40, 0x41, 0x4b, 0x4c, 0x50, 0x51 };
	s16 lin[2][128];
	for (s16 l = 0; l < 2; l++)
		for (s32 i = 0; i < 128; i++)
			lin[l][i] = g711::decode(l, k56_g711(static_cast<unsigned char>(0x80 | i), l));
	for (const Row &r : rows)
		for (s16 k = 0; k < 6; k++)
			EXPECT_EQ(k56_rbs_value(lin[r.law], r.law, mag[k], r.pad, r.a, r.b) /
				  (r.law ? 8.0f : 4.0f), r.v[k])
				<< r.law << " " << r.pad << " " << r.a << " " << r.b << " " << k;
}

} // namespace
