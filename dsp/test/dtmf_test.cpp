/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dtmf_test.cpp - the DTMF detector (dsp.h's DtmfDetect) on the
 *                    generator's digits, at the sample rates the modem
 *                    runs at, through G.711, and on Q.24's twist limits
 *
 */
#include <gtest/gtest.h>

#include <math.h>

#include <string>

#include "dsp.h"
#include "g711.h"

using namespace dsp;

namespace {

const char KEYS[] = "123A456B789C*0#D";
const double LO[] = { 697, 770, 852, 941 };
const double HI[] = { 1209, 1336, 1477, 1633 };

/* the digits the detector reports (each ON) for samples fed in turn */
struct Listener {
	DtmfDetect det;
	std::string got;
	int offs = 0;

	explicit Listener(int srate) { det.init(srate); }
	void feed(float x)
	{
		switch (det.sample(x)) {
		case DtmfDetect::ON:
			got += det.digit;
			break;
		case DtmfDetect::OFF:
			offs++;
			break;
		default:
			break;
		}
	}
	void silence(int n)
	{
		for (int i = 0; i < n; i++)
			feed(0);
	}
};

/* digit k's two tones for n samples at srate, the low one at amplitude
   lo and the high one at hi */
void tones(Listener &r, int k, int n, int srate, double lo, double hi,
	   bool law = false)
{
	for (int i = 0; i < n; i++) {
		double t = (double)i / srate;
		double x = lo * sin(2 * M_PI * LO[k / 4] * t) +
			hi * sin(2 * M_PI * HI[k % 4] * t);
		if (law)
			x = g711::decode(false, g711::encode(false, (int)x));
		r.feed((float)x);
	}
}

} // namespace

/* every digit of the generator, 60 ms on and 60 ms off, once each, at
   each rate */
TEST(Dtmf, GeneratorDigitsEachRate)
{
	for (int srate : { 8000, 9600, 16000 }) {
		Listener r(srate);
		DtmfGen g;
		for (int k = 0; k < 16; k++) {
			ASSERT_EQ(g.set(KEYS[k], srate), 0);
			for (int i = 0; i < srate * 60 / 1000; i++)
				r.feed(g.next());
			r.silence(srate * 60 / 1000);
		}
		EXPECT_EQ(r.got, KEYS) << srate;
		EXPECT_EQ(r.offs, 16) << srate;
	}
}

/* through G.711 mu-law, at -30 dBFS a tone: all detected */
TEST(Dtmf, ThroughG711)
{
	Listener r(8000);
	double a = 32767 * pow(10, -30 / 20.0);
	for (int k = 0; k < 16; k++) {
		tones(r, k, 480, 8000, a, a, true);
		r.silence(480);
	}
	EXPECT_EQ(r.got, KEYS);
}

/* Q.24's twist: the high tone 8 dB below the low one or 4 dB above it
   passes, 12 dB below or 7 dB above does not */
TEST(Dtmf, Twist)
{
	const double a = 6000;
	struct { double db; bool ok; } cases[] = {
		{ -7, true }, { 3.5, true }, { -12, false }, { 7, false },
	};
	for (const auto &c : cases) {
		Listener r(8000);
		tones(r, 5, 800, 8000, a, a * pow(10, c.db / 20));
		r.silence(400);
		EXPECT_EQ(r.got, c.ok ? "5" : "") << c.db << " dB";
	}
}

/* too short (one block's worth, 20 ms, aligned to a block) or too
   quiet (-55 dBFS) is no digit; 45 ms is */
TEST(Dtmf, LengthAndLevel)
{
	Listener a(8000);
	tones(a, 0, 160, 8000, 6000, 6000);
	a.silence(800);
	EXPECT_EQ(a.got, "");
	Listener b(8000);
	double q = 32767 * pow(10, -55 / 20.0);
	tones(b, 0, 2000, 8000, q, q);
	EXPECT_EQ(b.got, "");
	Listener c(8000);
	tones(c, 0, 360, 8000, 6000, 6000);
	c.silence(800);
	EXPECT_EQ(c.got, "1");
}
