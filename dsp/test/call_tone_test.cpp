/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    call_tone_test.cpp - the call progress tone detector (dsp.h's
 *                         CallToneDetect): the ringing and busy tones
 *                         of E.180's networks, through G.711, at the
 *                         sample rates the modem runs at, and what is
 *                         neither (speech, noise, a dial tone's level
 *                         too low); the answer tones; a calling fax
 *                         machine's CNG (CngDetect)
 *
 */
#include <gtest/gtest.h>

#include <math.h>

#include <vector>

#include "dsp.h"
#include "g711.h"

using namespace dsp;

namespace {

/* a tone: its frequencies (Hz), and its cadence of on and off ms in
   turn (empty: steady) */
struct Tone {
	std::vector<double> f;
	std::vector<int> cadence;
	double am = 0;          /* modulated at 16 Hz this deep (Japan's) */
};

/* what the detector made of a tone: its rings, the ms at which each came
   on and at which each ended (the quiet counted from it), its busy
   cadences */
struct Heard {
	unsigned rings = 0, busy = 0;
	std::vector<long> on_ms, off_ms;
};

/* ms of tone t at dbm0 a frequency (0 dBm0 is 3.14 dB below a full
   scale sine in mu-law), through mu-law if law, at srate */
Heard listen(const Tone &t, int ms, int srate = 8000, double dbm0 = -19,
	     bool law = true)
{
	CallToneDetect d;
	Heard h;
	double a = 32767 * pow(10, (dbm0 - 3.14) / 20);
	long period = 0;
	bool was = false;

	d.init(srate);
	for (int c : t.cadence)
		period += c;
	for (long i = 0; i < (long)ms * srate / 1000; i++) {
		long at = i * 1000 / srate;
		bool on = true;
		if (period) {
			long p = at % period;
			for (size_t k = 0; k < t.cadence.size(); k++) {
				if (p < t.cadence[k]) {
					on = k % 2 == 0;
					break;
				}
				p -= t.cadence[k];
			}
		}
		double x = 0;
		for (double f : t.f)
			x += on ? a * sin(2 * M_PI * f * i / srate) : 0;
		if (t.am)
			x *= 1 + t.am * sin(2 * M_PI * 16 * i / srate);
		if (law)
			x = g711::decode(false, g711::encode(false, (int)x));
		d.sample((float)x);
		if (d.ringing && !was)
			h.on_ms.push_back(at);
		if (!d.ringing && was)
			h.off_ms.push_back(at - d.quiet_ms());
		was = d.ringing;
	}
	h.rings = d.rings;
	h.busy = d.busy;
	return h;
}

} // namespace

/* E.180's ringing tones: one ring a cadence, each heard from 0.6 s of
   it (the UK's two bursts of 0.4 s one ring, heard 0.3 s into the
   second), its end dated to the tone's end within a block; no busy
   cadence */
TEST(CallTone, RingingTones)
{
	struct { const char *name; Tone t; } cases[] = {
		{ "North America", { { 440, 480 }, { 2000, 4000 } } },
		{ "UK", { { 400, 450 }, { 400, 200, 400, 2000 } } },
		{ "Europe", { { 425 }, { 1000, 4000 } } },
		{ "France", { { 440 }, { 1500, 3500 } } },
		{ "Japan", { { 400 }, { 1000, 2000 }, 0.8 } },
		{ "Australia", { { 400, 425 }, { 400, 200, 400, 2000 } } },
	};
	for (const auto &c : cases) {
		long period = 0, first = 0;
		for (int x : c.t.cadence)
			period += x;
		for (size_t k = 0; k + 1 < c.t.cadence.size(); k++)
			first += c.t.cadence[k];
		/* 0.6 s in, or 0.3 s into the burst after the gap */
		long heard_at = c.t.cadence[0] >= 600 ? 600 :
			c.t.cadence[0] + c.t.cadence[1] + 300;
		Heard h = listen(c.t, 3 * (int)period + 500);
		EXPECT_EQ(h.rings, 3u) << c.name;
		EXPECT_EQ(h.busy, 0u) << c.name;
		ASSERT_EQ(h.off_ms.size(), 3u) << c.name;
		for (int k = 0; k < 3; k++) {
			EXPECT_NEAR(h.on_ms[k], k * period + heard_at, 60)
				<< c.name << " ring " << k;
			EXPECT_NEAR(h.off_ms[k], k * period + first, 30)
				<< c.name << " ring " << k;
		}
	}
}

/* at each rate, without G.711, and from -10 to -40 dBm0 */
TEST(CallTone, RatesAndLevels)
{
	Tone na = { { 440, 480 }, { 2000, 4000 } };
	for (int srate : { 8000, 9600, 16000 })
		EXPECT_EQ(listen(na, 7000, srate, -19, false).rings, 2u)
			<< srate;
	for (double dbm0 : { -10, -25, -40 })
		EXPECT_EQ(listen(na, 7000, 8000, dbm0).rings, 2u) << dbm0;
	/* below the detector's -50 dBFS */
	EXPECT_EQ(listen(na, 7000, 8000, -52).rings, 0u);
}

/* busy and congestion tones: a cadence every four rounds, and no ring
   (each burst is shorter than one) */
TEST(CallTone, BusyTones)
{
	struct { const char *name; Tone t; } cases[] = {
		{ "North America busy", { { 480, 620 }, { 500, 500 } } },
		{ "North America reorder", { { 480, 620 }, { 250, 250 } } },
		{ "Europe busy", { { 425 }, { 500, 500 } } },
		{ "UK busy", { { 400 }, { 375, 375 } } },
		{ "Japan busy", { { 400 }, { 500, 500 } } },
	};
	for (const auto &c : cases) {
		Heard h = listen(c.t, 4 * (c.t.cadence[0] +
					   c.t.cadence[1]) + 100);
		EXPECT_EQ(h.busy, 1u) << c.name;
		EXPECT_EQ(h.rings, 0u) << c.name;
	}
	/* Germany's congestion, 0.24 s on and off: busy, though its gaps
	   are too short to part its bursts as rings */
	Heard h = listen({ { 425 }, { 240, 240 } }, 2000);
	EXPECT_EQ(h.busy, 1u);
	/* ringing tones are no busy tone */
	EXPECT_EQ(listen({ { 400, 450 }, { 400, 200, 400, 2000 } }, 30000).busy,
		  0u);
}

/* neither: noise, a voice (a vowel's harmonics of a pitch that moves,
   with pauses), a steady tone outside the band, a burst too short */
TEST(CallTone, NotCallTones)
{
	const int srate = 8000;
	CallToneDetect d;
	unsigned seed = 11;

	d.init(srate);
	for (long i = 0; i < 20L * srate; i++) {
		seed = seed * 1103515245 + 12345;
		d.sample((float)((int)(seed >> 16 & 0x7fff) - 16384) / 4);
	}
	EXPECT_EQ(d.rings, 0u);
	EXPECT_EQ(d.busy, 0u);

	/* 20 s of speech-like sound: 1.5 s phrases with 0.4 s pauses, a
	   pitch from 100 to 250 Hz and back over 0.8 s, its harmonics to
	   3.5 kHz falling 6 dB an octave */
	d.init(srate);
	double ph = 0;
	for (long i = 0; i < 20L * srate; i++) {
		double t = (double)i / srate;
		double pitch = 175 + 75 * sin(2 * M_PI * t / 0.8);
		double x = 0;
		ph += 2 * M_PI * pitch / srate;
		if (fmod(t, 1.9) < 1.5)
			for (int k = 1; k * pitch < 3500; k++)
				x += 4000.0 / k * sin(k * ph);
		d.sample((float)x);
	}
	EXPECT_EQ(d.rings, 0u);
	EXPECT_EQ(d.busy, 0u);
	EXPECT_EQ(d.answer[0] + d.answer[1], 0u);

	EXPECT_EQ(listen({ { 1000 } }, 5000).rings, 0u);
	EXPECT_EQ(listen({ { 440, 480 }, { 500, 4000 } }, 9000).rings, 0u);
}

/* answer tones (V.253's <DLE>a and <DLE>f): ANS and ANSam (2100 Hz, its
   phase reversed every 450 ms, ANSam amplitude-modulated at 15 Hz) and
   Bell's 2225 Hz, each once, 0.3 s in, through G.711; not 1800 Hz, not
   a tone of 0.2 s, not the ringing or a busy tone */
TEST(CallTone, AnswerTones)
{
	struct { const char *name; double f, am; bool reverse; int ms;
		 unsigned a2100, a2225; } cases[] = {
		{ "ANS", 2100, 0, false, 3300, 1, 0 },
		{ "/ANS", 2100, 0, true, 3300, 1, 0 },
		{ "ANSam", 2100, 0.2, true, 3300, 1, 0 },
		{ "Bell", 2225, 0, false, 2000, 0, 1 },
		{ "1800 Hz", 1800, 0, false, 2000, 0, 0 },
		{ "short", 2100, 0, false, 200, 0, 0 },
	};
	for (const auto &c : cases) {
		CallToneDetect d;
		long on_at = -1;
		double a = 32767 * pow(10, (-12 - 3.14) / 20);
		d.init(8000);
		for (long i = 0; i < 8L * (c.ms + 500); i++) {
			double t = i / 8000.0, x = 0;
			if (i < 8L * c.ms) {
				int rev = c.reverse ? (int)(t / 0.45) : 0;
				x = a * sin(2 * M_PI * c.f * t + M_PI * rev) *
					(1 + c.am * sin(2 * M_PI * 15 * t));
			}
			d.sample(g711::decode(false, g711::encode(false, (int)x)));
			if (on_at < 0 && d.answer[0] + d.answer[1])
				on_at = i / 8;
		}
		EXPECT_EQ(d.answer[0], c.a2100) << c.name;
		EXPECT_EQ(d.answer[1], c.a2225) << c.name;
		if (c.a2100 + c.a2225) {
			EXPECT_NEAR(on_at, 300, 30) << c.name;
		}
		EXPECT_EQ(d.rings, 0u) << c.name;
		EXPECT_EQ(d.busy, 0u) << c.name;
	}
	EXPECT_EQ(listen({ { 440, 480 }, { 2000, 4000 } }, 7000).rings, 2u);
	CallToneDetect d;
	d.init(8000);
	for (long i = 0; i < 8000L * 5; i++)
		d.sample((i / 4000) % 2 ? 0 : (float)(3000 *
			(sin(2 * M_PI * 480 * i / 8000.0) +
			 sin(2 * M_PI * 620 * i / 8000.0))));
	EXPECT_EQ(d.busy, 1u);
	EXPECT_EQ(d.answer[0] + d.answer[1], 0u);
}

/* a ring a long way into the quiet: the ms since it ended keep counting,
   and a burst of the tone too short for a ring does not start them
   over */
TEST(CallTone, QuietAfterRing)
{
	CallToneDetect d;
	d.init(8000);
	auto tone = [&](int ms, bool on) {
		for (long i = 0; i < ms * 8L; i++)
			d.sample(on ? (float)(3000 * sin(2 * M_PI * 425 * i /
							   8000.0)) : 0);
	};
	EXPECT_GE(d.quiet_ms(), 0);
	tone(1000, true);
	EXPECT_TRUE(d.ringing);
	tone(2000, false);
	EXPECT_FALSE(d.ringing);
	EXPECT_NEAR(d.quiet_ms(), 2000, 20);
	tone(200, true);
	tone(1000, false);
	EXPECT_EQ(d.rings, 1u);
	EXPECT_NEAR(d.quiet_ms(), 3200, 20);
}

/* the CNG bursts (dsp.h's CngDetect) in ms of f Hz on for on_ms of every
   period_ms (period_ms 0: steady), at dbm0, with an answer tone's echo
   (2100 Hz) echo times its amplitude (0: none), at srate */
static unsigned cng_bursts(double f, int on_ms, int period_ms, int ms,
		    double dbm0 = -15, double echo = 0, int srate = 8000)
{
	CngDetect d;
	double a = 32767 * pow(10, (dbm0 - 3.14) / 20);
	d.init(srate);
	for (long i = 0; i < (long)ms * srate / 1000; i++) {
		long at = i * 1000 / srate;
		bool on = !period_ms || at % period_ms < on_ms;
		double t = (double)i / srate;
		double x = on ? a * sin(2 * M_PI * f * t) : 0;
		if (echo)
			x += a * echo * sin(2 * M_PI * 2100 * t);
		d.sample((float)x);
	}
	return d.bursts;
}

/* CNG (1100 Hz +-38 Hz, 0.5 s of every 3.5 s) found out, at both the
   modem's rates, quiet, and under an answer tone's echo as loud; not a
   steady tone, nor V.22's unscrambled ones (1050 Hz, steady or not), an
   answer tone, V.21's or Bell 103's marks, nor a burst too long */
TEST(CallTone, Cng)
{
	for (int srate : { 8000, 9600 }) {
		SCOPED_TRACE(srate);
		EXPECT_EQ(cng_bursts(1100, 500, 3500, 7600, -15, 0, srate),
			  3u);
		EXPECT_EQ(cng_bursts(1062, 500, 3500, 4300, -15, 0, srate),
			  2u);
		EXPECT_EQ(cng_bursts(1138, 500, 3500, 4300, -15, 0, srate),
			  2u);
		EXPECT_EQ(cng_bursts(1100, 500, 3500, 4300, -40, 0, srate),
			  2u);
		EXPECT_EQ(cng_bursts(1100, 500, 3500, 4300, -20, 1, srate),
			  2u);
		EXPECT_EQ(cng_bursts(1100, 0, 0, 4000, -15, 0, srate), 0u);
		EXPECT_EQ(cng_bursts(1050, 500, 3500, 4300, -15, 0, srate),
			  0u);
		EXPECT_EQ(cng_bursts(1050, 0, 0, 4000, -15, 0, srate), 0u);
		EXPECT_EQ(cng_bursts(2100, 500, 3500, 4300, -15, 0, srate),
			  0u);
		EXPECT_EQ(cng_bursts(980, 500, 3500, 4300, -15, 0, srate),
			  0u);
		EXPECT_EQ(cng_bursts(1270, 500, 3500, 4300, -15, 0, srate),
			  0u);
		EXPECT_EQ(cng_bursts(1100, 1200, 3500, 4000, -15, 0, srate),
			  0u);
	}
}
