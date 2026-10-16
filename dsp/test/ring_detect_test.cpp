/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    ring_detect_test.cpp - the ring signal detector (dsp.h's RingDetect)
 *                           on a codec's on-hook samples: a pulse each
 *                           cycle of a ring burst, at the bells'
 *                           frequencies and the modem's rates, clipped
 *                           or not; none for the voice band, noise or a
 *                           low hum
 *
 */
#include <gtest/gtest.h>

#include <math.h>

#include <vector>

#include "dsp.h"

using namespace dsp;

namespace {

/* the pulses for ms of a wave of f Hz at amplitude a (clipped to
   +-clip), on and off ms at a time (off 0: steady), at srate; the ms of
   each pulse into *at if given */
unsigned pulses(double f, double a, int on, int off, int ms, int srate,
		double clip = 32767, std::vector<long> *at = nullptr)
{
	RingDetect d;
	unsigned n = 0;

	d.init(srate);
	for (long i = 0; i < (long)ms * srate / 1000; i++) {
		long t = i * 1000 / srate;
		double x = 0;
		if (!off || t % (on + off) < on)
			x = a * sin(2 * M_PI * f * i / srate);
		x = x > clip ? clip : x < -clip ? -clip : x;
		if (d.sample((float)x)) {
			n++;
			if (at)
				at->push_back(t);
		}
	}
	return n;
}

} // namespace

/* the bells' 16, 20, 25 and 50 Hz, 2 s on and 4 s off: about a pulse a
   cycle, less the first 50 ms, in each burst and none between */
TEST(RingDetect, Frequencies)
{
	for (double f : { 16.0, 20.0, 25.0, 50.0 }) {
		std::vector<long> at;
		unsigned n = pulses(f, 20000, 2000, 4000, 12000, 8000, 32767,
				    &at);
		EXPECT_NEAR(n, 2 * (2 * f - f / 20), 3) << f;
		for (long t : at)
			EXPECT_TRUE(t % 6000 < 2100) << f << " Hz at " << t;
	}
}

/* at 9600 and 16000 Hz too, and a wave clipped at the codec's full
   scale (a ring of 90 V into a line monitor path) */
TEST(RingDetect, RatesAndClipping)
{
	for (int srate : { 8000, 9600, 16000 })
		EXPECT_NEAR(pulses(20, 20000, 2000, 0, 2000, srate), 39, 2)
			<< srate;
	EXPECT_NEAR(pulses(20, 200000, 2000, 0, 2000, 9600), 39, 2);
	EXPECT_NEAR(pulses(25, 6000, 2000, 0, 2000, 9600), 49, 2);
}

/* nothing in the voice band (a dial tone, DTMF's 697 Hz at full scale),
   for a hum of 50 Hz at -36 dBFS, or for noise */
TEST(RingDetect, NotRings)
{
	EXPECT_EQ(pulses(440, 30000, 0, 0, 3000, 8000), 0u);
	EXPECT_EQ(pulses(697, 32000, 0, 0, 3000, 9600), 0u);
	EXPECT_EQ(pulses(50, 500, 0, 0, 3000, 9600), 0u);
	RingDetect d;
	d.init(8000);
	unsigned seed = 3, n = 0;
	for (long i = 0; i < 8000L * 5; i++) {
		seed = seed * 1103515245 + 12345;
		n += d.sample((float)((int)(seed >> 16 & 0x7fff) - 16384));
	}
	EXPECT_EQ(n, 0u);
}
