/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    ring_tones_test.cpp - the countries' ringing (ring_tones.h): its
 *                          rows, and each country's ring signal, at its
 *                          frequency and in its cadence, through the ring
 *                          detector (dsp.h's RingDetect) to the core, as a
 *                          line on hook hears it: a RING each ring, the
 *                          auto answer at S0's
 *
 */
#include <gtest/gtest.h>

#include <math.h>
#include <string.h>

#include <string>

#include <dcore.h>

#include "dsp.h"
#include "ring_tones.h"

extern "C" int prop_dp_init(void);

namespace {

struct Pumps {
	Pumps() { prop_dp_init(); }
} pumps;

unsigned long now = 1000000;      /* (not 0: the core's last pulse "at" 0) */

/* the core's host: a line on hook, its DTE's output kept */
struct Line final : public dcore::Host {
	std::string out;
	bool answered = false;
	int start(dcore::Modem &) override {
		answered = true;
		return 0;
	}
	int stop(dcore::Modem &) override { return 0; }
	int set_hook(dcore::Modem &, int) override { return 0; }
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		out.append(buf, n);
		return n;
	}
	unsigned long now_ms() override { return now; }
};

/* the RINGs in out */
unsigned rings(const std::string &out)
{
	unsigned n = 0;
	for (size_t at = 0; (at = out.find("RING", at)) != std::string::npos; at++)
		n++;
	return n;
}

/* the rings the core finds in s seconds of the cadence: its bursts
   (ons with less than 1.5 s between taken as one) that start by s, or
   with no such gap one every 6 s */
unsigned bursts(const ring::Country &c, double s)
{
	bool gap = false;
	for (int k = 1; k < c.n; k += 2)
		gap |= c.cadence[k] >= 1.5f;
	if (!gap)
		return (unsigned)(s / 6) + 1;
	unsigned n = 0;
	double t = 0, off = 1e9;
	for (int k = 0; t < s; k = (k + 1) % c.n) {
		if (k % 2 == 0 && off >= 1.5)
			n++;
		off = k % 2 ? c.cadence[k] : 0;
		t += c.cadence[k];
	}
	return n;
}

/* s seconds of country c's ring signal at a modem on hook (S0=s0):
   its DTE's output, *answered whether it answered */
std::string ring_for(const ring::Country &c, double s, int s0,
		     bool *answered = nullptr)
{
	Line l;
	dcore::Options o;
	o.name = c.name;
	o.rate = 8000;
	dcore::Modem m(l, o);
	dsp::RingDetect d;
	d.init(8000);
	std::string cmd = "ATE0S0=" + std::to_string(s0) + "\r";
	m.dte_input(cmd.c_str(), cmd.size());
	l.out.clear();
	long n = (long)(s * 8000);
	for (long i = 0; i < n && !l.answered; i++) {
		double t = i / 8000.0;
		float x = 0;
		if (ring::on_at(c, t))
			x = (float)(24000 * sin(2 * M_PI * c.ring_hz * t));
		if (d.sample(x))
			m.ring();
		if (i % 80 == 79) {
			now += 10;
			m.poll();
		}
	}
	if (answered)
		*answered = l.answered;
	return l.out;
}

} // namespace

/* the rows: names, tones in the voice band, a cadence of ons and offs */
TEST(RingTones, Rows)
{
	int n = 0;
	for (const ring::Country *c = ring::COUNTRIES; c->name; c++, n++) {
		SCOPED_TRACE(c->name);
		EXPECT_TRUE(c->f1 >= 300 && c->f1 <= 500);
		EXPECT_TRUE(!c->f2 || (c->f2 >= 300 && c->f2 <= 700));
		EXPECT_TRUE(c->mod >= 0 && c->mod < 100);
		EXPECT_TRUE(c->ring_hz == 16 || c->ring_hz == 20 ||
			    c->ring_hz == 25 || c->ring_hz == 50);
		EXPECT_TRUE(c->n >= 2 && c->n <= 8 && c->n % 2 == 0);
		for (int k = 0; k < c->n; k++)
			EXPECT_GT(c->cadence[k], 0);
		EXPECT_EQ(ring::find(c->name), c);
		if (c > ring::COUNTRIES) {
			EXPECT_LT(strcmp(c[-1].name, c->name), 0);
		}
	}
	EXPECT_GE(n, 150);
	for (const ring::Alias *a = ring::ALIASES; a->alias; a++)
		EXPECT_EQ(ring::find(a->alias), ring::find(a->name)) << a->alias;
	ASSERT_TRUE(ring::find("usa"));
	EXPECT_EQ(ring::find("usa")->f2, 480);
	EXPECT_EQ(ring::find("United Kingdom")->cadence[3], 2.0f);
	EXPECT_EQ(ring::find("japan")->ring_hz, 16);
	EXPECT_FALSE(ring::find("ATLANTIS"));
	/* the cadence round and round: UK's 0.4 on, 0.2 off, 0.4 on, 2 off */
	const ring::Country &uk = *ring::find("UK");
	EXPECT_TRUE(ring::on_at(uk, 0.1));
	EXPECT_FALSE(ring::on_at(uk, 0.5));
	EXPECT_TRUE(ring::on_at(uk, 0.7));
	EXPECT_FALSE(ring::on_at(uk, 2.0));
	EXPECT_TRUE(ring::on_at(uk, 3.1));
}

/* each country's ringing, 26 s of it with no auto answer: a RING for
   each of its rings */
TEST(RingTones, RingEachCountry)
{
	for (const ring::Country *c = ring::COUNTRIES; c->name; c++) {
		SCOPED_TRACE(c->name);
		std::string out = ring_for(*c, 26, 0);
		/* (a ring is reported 2.5 s after its first pulse) */
		unsigned want = bursts(*c, 26 - 2.6), got = rings(out);
		if (getenv("SHOW"))
			printf("%-40s %2u of %2u\n", c->name, got, want);
		EXPECT_GE(got + 1, want);
		EXPECT_LE(got, want);
	}
}

/* the auto answer after S0=2 rings, in each country; after the first
   where the cadence is longer than the 8 s the core keeps a ring for
   (Tuvalu's 12.5 s) */
TEST(RingTones, AutoAnswerEachCountry)
{
	for (const ring::Country *c = ring::COUNTRIES; c->name; c++) {
		SCOPED_TRACE(c->name);
		double period = 0;
		for (int k = 0; k < c->n; k++)
			period += c->cadence[k];
		unsigned s0 = period <= 10.5 ? 2 : 1;
		bool answered = false;
		std::string out = ring_for(*c, 30, s0, &answered);
		EXPECT_TRUE(answered);
		EXPECT_EQ(rings(out), s0);
	}
}
