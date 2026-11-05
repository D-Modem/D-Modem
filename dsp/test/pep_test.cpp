/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pep_test.cpp - Telebit's PEP (pep.h): two of the modem, caller and
 *    answerer, back to back at 8000 Hz on a simple line (a delay each
 *    way, the band of a telephone line, white noise): the start-up,
 *    data both ways without an error, the line's rate following its
 *    noise, Adaptive Duplex (one side with nothing to send leaves the
 *    line to the other), and the packets lost in a dropout sent again.
 *    Each pair's rates are printed.
 *
 *    PEP_DEBUG=1   the modems' logs
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pep.h"

using dsp::PepLoading;
using dsp::PepModem;

namespace {

/* a PRBS of x^23 + x^18 + 1, an octet at a time */
struct Prbs {
	u32 r = 0x5a5a5;
	/* the next octet */
	u8 octet()
	{
		u8 o = 0;
		for (s16 i = 0; i < 8; i++) {
			u32 b = ((r >> 22) ^ (r >> 17)) & 1;
			r = (r << 1) | b;
			o = static_cast<u8>(o | b << i);
		}
		return o;
	}
};

/* what a side receives, against the far side's Prbs: in order and
   without an error, octet by octet */
struct Check {
	Prbs ref;
	s64 octets = 0, errors = 0;
	/* n octets received */
	void in(const u8 *p, s32 n)
	{
		for (s32 i = 0; i < n; i++, octets++)
			if (p[i] != ref.octet())
				errors++;
	}
};

/* one way of the line: a delay, a band pass (300 Hz to 3400 Hz, two
   biquads), the gain, white noise snr dB below a level of -9 dBm0 */
struct Line {
	std::vector<float> d;
	size_t at = 0;
	float hp[4] = {}, lp[4] = {};
	float b_hp[5], b_lp[5];
	float gain = 0.5f, sigma = 0;
	std::mt19937 rng{1};
	std::normal_distribution<float> g{0, 1};
	bool drop = false;      /* nothing through */

	/* a biquad's coefficients b0 b1 b2 a1 a2 (RBJ), high or low pass
	   at f, Q 0.7 */
	static void biquad(float *c, bool high, float f)
	{
		double w = 2 * M_PI * f / 8000, cw = cos(w), al = sin(w) / 1.4;
		double a0 = 1 + al;
		double b0 = high ? (1 + cw) / 2 : (1 - cw) / 2;
		double b1 = high ? -(1 + cw) : 1 - cw;
		c[0] = static_cast<float>(b0 / a0);
		c[1] = static_cast<float>(b1 / a0);
		c[2] = static_cast<float>(b0 / a0);
		c[3] = static_cast<float>(-2 * cw / a0);
		c[4] = static_cast<float>((1 - al) / a0);
	}
	/* (see biquad()) one sample of it, its state s */
	static float run_bq(const float *c, float *s, float x)
	{
		float y = c[0] * x + c[1] * s[0] + c[2] * s[1] - c[3] * s[2] -
			c[4] * s[3];
		s[1] = s[0];
		s[0] = x;
		s[3] = s[2];
		s[2] = y;
		return y;
	}
	/* delay samples, snr dB */
	void init(s32 delay, float snr, unsigned seed)
	{
		d.assign(static_cast<size_t>(delay), 0);
		biquad(b_hp, true, 300);
		biquad(b_lp, false, 3400);
		sigma = 5700 * gain * std::pow(10.0f, -snr / 20);
		rng.seed(seed);
	}
	/* a sample in, the far end's out */
	float run(float x)
	{
		float y = d[at];
		d[at] = x;
		at = (at + 1) % d.size();
		y = run_bq(b_lp, lp, run_bq(b_hp, hp, y)) * gain;
		if (drop)
			y = 0;
		return y + sigma * g(rng);
	}
};

struct Pair {
	PepModem m[2];          /* the caller, the answerer */
	Prbs tx[2];
	Check rx[2];
	bool idle[2] = { false, false };    /* nothing to send */
	Line line[2];           /* to the answerer, to the caller */
	s16 in[2] = {}, out[2] = {};
	s64 t = 0;
	bool debug = std::getenv("PEP_DEBUG") != nullptr;

	/* on a line of snr dB */
	explicit Pair(float snr)
	{
		for (int k = 0; k < 2; k++) {
			PepModem::Config c;
			c.calling = k == 0;
			m[k].init(c,
				  [this, k](u8 *b, s32 n) {
					  if (idle[k])
						  return 0;
					  for (s32 i = 0; i < n; i++)
						  b[i] = tx[k].octet();
					  return n;
				  },
				  [this, k](const u8 *p, s32 n) {
					  rx[k].in(p, n);
				  });
		}
		line[0].init(37, snr, 1);
		line[1].init(53, snr, 2);
	}
	/* secs more */
	void run(double secs)
	{
		s64 end = t + static_cast<s64>(secs * 8000);
		for (; t < end; t++) {
			s16 a = static_cast<s16>(std::lround(
				std::max(-32767.0f, std::min(32767.0f,
					line[1].run(out[1])))));
			s16 b = static_cast<s16>(std::lround(
				std::max(-32767.0f, std::min(32767.0f,
					line[0].run(out[0])))));
			in[0] = a;
			in[1] = b;
			for (int k = 0; k < 2; k++)
				m[k].process(&in[k], &out[k], 1);
			if (debug)
				for (int k = 0; k < 2; k++) {
					std::string s = m[k].take_log();
					if (!s.empty())
						fputs(s.c_str(), stdout);
				}
		}
	}
	/* both connected, no errors, received octets at least min each
	   way; the rates printed */
	void expect_ok(s64 min0, s64 min1) const
	{
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			EXPECT_TRUE(m[k].connected()) << who << ": " << m[k].why();
			EXPECT_EQ(rx[k].errors, 0) << who;
			EXPECT_GE(rx[k].octets, k ? min1 : min0) << who;
			printf("%s: line %d bit/s out, %d in; %lld octets in, "
			       "packets %lld good %lld bad; SNR %.1f dB, %d "
			       "turns\n", who, m[k].tx_rate(), m[k].rx_rate(),
			       static_cast<long long>(rx[k].octets),
			       static_cast<long long>(m[k].packets_good()),
			       static_cast<long long>(m[k].packets_bad()),
			       static_cast<double>(m[k].snr_db()),
			       m[k].turns_tx());
		}
	}
};

} // namespace

/* the loading's bits counted, the pilots left out */
TEST(PepLoading, Uniform)
{
	PepLoading l;
	std::vector<u8> mask(512, 1);
	l.uniform(mask, 2);
	EXPECT_EQ(l.bits[dsp::pep::TONE1], dsp::pep::PILOT);
	EXPECT_EQ(l.bits[dsp::pep::TONE2], dsp::pep::PILOT);
	EXPECT_EQ(l.bits[1], 2);
	EXPECT_EQ(l.total, 2 * 509);
	EXPECT_EQ(l.octets(), 127);
}

/* a clean line: connected within 6 s, data both ways at once without
   an error, the line's rate near Telebit's best (18031 bit/s) or above */
TEST(Pep, BothWays)
{
	Pair p(40);
	p.run(6);
	ASSERT_TRUE(p.m[0].connected() && p.m[1].connected());
	p.run(14);
	p.expect_ok(12000, 12000);
	EXPECT_GE(p.m[0].tx_rate(), 18000);
	EXPECT_GE(p.m[1].tx_rate(), 18000);
}

/* the line's rate falls with its SNR (Hughes-Hartogs's loading), each
   without errors */
TEST(Pep, RateFollowsNoise)
{
	s32 last = 1 << 30;
	for (float snr : { 40.0f, 30.0f, 24.0f, 19.0f }) {
		Pair p(snr);
		p.run(20);
		p.expect_ok(3000, 3000);
		s32 r = p.m[1].tx_rate();
		printf("%.0f dB: %d bit/s\n", static_cast<double>(snr), r);
		EXPECT_LT(r, last) << snr << " dB";
		last = r;
	}
}

/* Adaptive Duplex: the caller has nothing to send, so its turns are
   one packet; the answerer's data gets most of the line (half as much
   again as it gets when both send all they can) */
TEST(Pep, AdaptiveDuplex)
{
	Pair both(40);
	both.run(20);
	Pair one(40);
	one.idle[0] = true;
	one.run(20);
	one.expect_ok(20000, 0);
	EXPECT_EQ(one.rx[1].octets, 0);
	EXPECT_GT(one.rx[0].octets, 3 * both.rx[0].octets / 2);
	printf("one way %lld octets, both ways %lld\n",
	       static_cast<long long>(one.rx[0].octets),
	       static_cast<long long>(both.rx[0].octets));
}

/* half a second of nothing on the line in data: the packets lost then
   are sent again, everything still in order without an error */
TEST(Pep, Dropout)
{
	Pair p(35);
	p.run(8);
	ASSERT_TRUE(p.m[0].connected() && p.m[1].connected());
	s64 again = p.m[0].packets_again() + p.m[1].packets_again();
	p.line[0].drop = p.line[1].drop = true;
	p.run(0.5);
	p.line[0].drop = p.line[1].drop = false;
	p.run(12);
	p.expect_ok(10000, 10000);
	EXPECT_GT(p.m[0].packets_again() + p.m[1].packets_again(), again);
}
