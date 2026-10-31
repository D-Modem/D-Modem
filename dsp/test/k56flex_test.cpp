/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_test.cpp - K56flex's data mode (dsp/k56flex.h): the encoder
 *                       into the decoder at every rate, pad, law and
 *                       robbed-bit pattern, the bits each frame takes,
 *                       the shaping's DC, and the codes of one frame
 *                       against those Motorola's SM56 client decodes
 *                       (k56flex_sm56.cpp checks all of them there)
 *
 */

#include <stdlib.h>

#include <vector>

#include <gtest/gtest.h>

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
			if (v)
				EXPECT_EQ(v > 0 ? 1 : -1, ref) << c;
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

} // namespace
