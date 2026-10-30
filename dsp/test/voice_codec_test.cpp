/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    voice_codec_test.cpp - +VSM's sample formats (voice_codec.h): the
 *                           table, each coding's octets, IMA ADPCM on a
 *                           tone
 *
 */

#include <math.h>

#include <vector>

#include <gtest/gtest.h>

#include "voice_codec.h"

using namespace dsp::voice;

namespace {

/* samples through an encoder and a decoder of coding c */
std::vector<s16> round_trip(Coding c, const std::vector<s16> &x)
{
	Codec enc, dec;
	std::vector<s16> y;
	enc.reset(c);
	dec.reset(c);
	for (s16 v : x) {
		u8 b[2];
		s16 out[2];
		int n = enc.encode(v, b);
		for (int i = 0; i < n; i++) {
			int m = dec.decode(b[i], out);
			y.insert(y.end(), out, out + m);
		}
	}
	return y;
}

std::vector<s16> tone(double f, int n, double a = 8000)
{
	std::vector<s16> x(n);
	for (int i = 0; i < n; i++)
		x[i] = static_cast<s16>(a * sin(2 * M_PI * f * i / 8000));
	return x;
}

} // namespace

/* the formats: V.253's numbers and Conexant's, the rates each runs at */
TEST(VoiceCodec, Formats)
{
	for (unsigned cml : { 0u, 1u, 4u, 5u, 128u, 129u, 130u, 131u, 132u })
		EXPECT_NE(format(cml), nullptr) << cml;
	for (unsigned cml : { 2u, 3u, 6u, 7u, 8u, 127u, 133u, 255u })
		EXPECT_EQ(format(cml), nullptr) << cml;
	EXPECT_TRUE(runs_at(1, 8000));
	EXPECT_TRUE(runs_at(129, 11025));
	EXPECT_TRUE(runs_at(132, 7200));
	EXPECT_FALSE(runs_at(4, 11025));
	EXPECT_FALSE(runs_at(1, 9000));
	EXPECT_EQ(format(129)->bps, 16);
	EXPECT_EQ(format(132)->bps, 4);
}

/* each 8 and 16 bit coding's octets for a sample */
TEST(VoiceCodec, Octets)
{
	Codec c;
	u8 b[2];

	c.reset(UNSIGNED8);
	ASSERT_EQ(c.encode(0, b), 1);
	EXPECT_EQ(b[0], 0x80);
	ASSERT_EQ(c.encode(-32768, b), 1);
	EXPECT_EQ(b[0], 0x00);
	c.reset(SIGNED8);
	ASSERT_EQ(c.encode(0x1234, b), 1);
	EXPECT_EQ(b[0], 0x12);
	ASSERT_EQ(c.encode(-256, b), 1);
	EXPECT_EQ(b[0], 0xff);
	c.reset(ULAW);
	ASSERT_EQ(c.encode(0, b), 1);
	EXPECT_EQ(b[0], 0xff);
	c.reset(ALAW);
	ASSERT_EQ(c.encode(0, b), 1);
	EXPECT_EQ(b[0], 0xd5);
	/* the low octet first */
	c.reset(LINEAR16);
	ASSERT_EQ(c.encode(0x1234, b), 2);
	EXPECT_EQ(b[0], 0x34);
	EXPECT_EQ(b[1], 0x12);
	s16 x[2];
	EXPECT_EQ(c.decode(0xcd, x), 0);
	ASSERT_EQ(c.decode(0xab, x), 1);
	EXPECT_EQ(x[0], static_cast<s16>(0xabcd));
}

/* the linear codings' round trips: 16 bits exact, 8 bits the high octet */
TEST(VoiceCodec, LinearRoundTrip)
{
	std::vector<s16> x = tone(1000, 400, 20000);
	EXPECT_EQ(round_trip(LINEAR16, x), x);
	for (Coding c : { SIGNED8, UNSIGNED8 }) {
		std::vector<s16> y = round_trip(c, x);
		ASSERT_EQ(y.size(), x.size());
		for (size_t i = 0; i < x.size(); i++)
			EXPECT_EQ(y[i], static_cast<s16>((x[i] >> 8) * 256));
	}
}

/* IMA ADPCM: two samples an octet, the first in the low nibble; a tone
   comes back within a few percent once the step has adapted */
TEST(VoiceCodec, ImaAdpcm)
{
	Codec c;
	u8 b[2];
	s16 x[2];

	c.reset(IMA_ADPCM);
	EXPECT_EQ(c.encode(1000, b), 0);
	ASSERT_EQ(c.encode(-1000, b), 1);
	EXPECT_EQ(b[0] & 0x08, 0);              /* the first up */
	EXPECT_NE(b[0] & 0x80, 0);              /* the second down */
	c.reset(IMA_ADPCM);
	ASSERT_EQ(c.decode(0x07, x), 2);
	EXPECT_GT(x[0], 0);
	EXPECT_GE(x[1], x[0]);                  /* code 0: a small step up */
	EXPECT_LT(x[1] - x[0], x[0]);

	std::vector<s16> in = tone(440, 8000), out =
		round_trip(IMA_ADPCM, in);
	ASSERT_EQ(out.size(), in.size());
	double e = 0, p = 0;
	for (size_t i = 800; i < in.size(); i++) {
		double d = out[i] - in[i];
		e += d * d;
		p += static_cast<double>(in[i]) * in[i];
	}
	EXPECT_GT(10 * log10(p / e), 20.0);     /* SNR, dB */
}
