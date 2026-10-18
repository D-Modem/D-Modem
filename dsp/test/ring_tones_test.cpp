/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    ring_tones_test.cpp - the countries' tones (tones.h): their
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
#include "tones.h"

extern "C" int prop_dp_init(void);

namespace {

struct Pumps {
	/* the modem's DSP pumps registered (prop_dp_init()) */
	Pumps() { prop_dp_init(); }
} pumps;

unsigned long now = 1000000;      /* (not 0: the core's last pulse "at" 0) */

/* the core's host: a line on hook, its DTE's output kept */
struct Line final : public dcore::Host {
	std::string out;
	bool answered = false;
	/* the line answered: answered set */
	int start(dcore::Modem &) override {
		answered = true;
		return 0;
	}
	/* (no line to stop) */
	int stop(dcore::Modem &) override { return 0; }
	/* (no hook to set) */
	int set_hook(dcore::Modem &, int) override { return 0; }
	/* the DTE's output appended to out: all n of it */
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		out.append(buf, n);
		return n;
	}
	/* the test's clock, now */
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
unsigned bursts(const tones::Country &c, double s)
{
	bool gap = false;
	for (int k = 1; k < c.ringing.n; k += 2)
		gap |= c.ringing.cadence[k] >= 1.5f;
	if (!gap)
		return (unsigned)(s / 6) + 1;
	unsigned n = 0;
	double t = 0, off = 1e9;
	for (int k = 0; t < s; k = (k + 1) % c.ringing.n) {
		if (k % 2 == 0 && off >= 1.5)
			n++;
		off = k % 2 ? c.ringing.cadence[k] : 0;
		t += c.ringing.cadence[k];
	}
	return n;
}

/* s seconds of country c's ring signal at a modem on hook (S0=s0):
   its DTE's output, *answered whether it answered */
std::string ring_for(const tones::Country &c, double s, int s0,
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
		if (tones::on_at(c.ringing, t))
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

/* a sample of tone t at t s of it (netsim's: 5000 at its peak) */
float tone_at(const tones::Tone &t, double s)
{
	if (!tones::on_at(t, s))
		return 0;
	double x = sin(2 * M_PI * t.f1 * s);
	if (t.f2)
		x = (x + sin(2 * M_PI * t.f2 * s)) / 2;
	if (t.mod > 0)
		x *= 0.6 + 0.4 * sin(2 * M_PI * t.mod * s);
	return (float)(5000 * x);
}

/* up to s seconds of tone t on the line of a modem in country c that
   dials blind (ATX3D, nothing to dial): its DTE's output up to its
   result */
std::string dial_into(const tones::Country &c, const tones::Tone &t, double s)
{
	Line l;
	dcore::Options o;
	o.name = c.name;
	o.rate = 8000;
	o.country = c.name;
	dcore::Modem m(l, o);
	static const char cmd[] = "ATE0X3S7=90D\r";
	m.dte_input(cmd, sizeof(cmd) - 1);
	int16_t in[80], out[80];
	for (long b = 0; b < (long)(s * 100); b++) {
		for (int k = 0; k < 80; k++)
			in[k] = (int16_t)tone_at(t, (b * 80 + k) / 8000.0);
		if (m.started())
			m.process(in, out, 80);
		now += 10;
		m.poll();
		if (l.out.find("BUSY") != std::string::npos ||
		    l.out.find("NO ") != std::string::npos)
			break;
	}
	return l.out;
}

} // namespace

/* the rows: names, tones in the voice band, a cadence of ons and offs */
TEST(RingTones, Rows)
{
	int n = 0;
	for (const tones::Country *c = tones::COUNTRIES; c->name; c++, n++) {
		SCOPED_TRACE(c->name);
		for (const tones::Tone *t : { &c->ringing, &c->dial, &c->busy,
					     &c->congestion }) {
			EXPECT_TRUE(t->f1 >= 300 && t->f1 <= 500);
			EXPECT_TRUE(!t->f2 || (t->f2 >= 300 && t->f2 <= 700));
			EXPECT_TRUE(t->mod >= 0 && t->mod < 100);
		}
		for (const tones::Tone *t : { &c->dial, &c->busy,
					     &c->congestion }) {
			EXPECT_TRUE(t->n >= 0 && t->n <= 8 && t->n % 2 == 0);
			for (int k = 0; k < t->n; k++)
				EXPECT_TRUE(t->cadence[k] > 0 &&
					    t->cadence[k] <= 5);
		}
		EXPECT_TRUE(c->ring_hz == 16 || c->ring_hz == 20 ||
			    c->ring_hz == 25 || c->ring_hz == 50);
		EXPECT_TRUE(c->ringing.n >= 2 && c->ringing.n <= 8 &&
			    c->ringing.n % 2 == 0);
		for (int k = 0; k < c->ringing.n; k++)
			EXPECT_GT(c->ringing.cadence[k], 0);
		EXPECT_EQ(tones::find(c->name), c);
		if (c > tones::COUNTRIES) {
			EXPECT_LT(strcmp(c[-1].name, c->name), 0);
		}
	}
	EXPECT_GE(n, 150);
	for (const tones::Alias *a = tones::ALIASES; a->alias; a++)
		EXPECT_EQ(tones::find(a->alias), tones::find(a->name)) << a->alias;
	ASSERT_TRUE(tones::find("usa"));
	EXPECT_EQ(tones::find("usa")->ringing.f2, 480);
	EXPECT_EQ(tones::find("United Kingdom")->ringing.cadence[3], 2.0f);
	EXPECT_EQ(tones::find("japan")->ring_hz, 16);
	EXPECT_FALSE(tones::find("ATLANTIS"));
	/* the cadence round and round: UK's 0.4 on, 0.2 off, 0.4 on, 2 off */
	const tones::Tone &uk = tones::find("UK")->ringing;
	EXPECT_TRUE(tones::on_at(uk, 0.1));
	EXPECT_FALSE(tones::on_at(uk, 0.5));
	EXPECT_TRUE(tones::on_at(uk, 0.7));
	EXPECT_FALSE(tones::on_at(uk, 2.0));
	EXPECT_TRUE(tones::on_at(uk, 3.1));
	/* dial tones: the USA's 350 + 440 Hz, Germany's 425 Hz, continuous;
	   Italy's in a cadence (0.2 s on, 0.2 off, 0.6 on, 1 off) */
	const tones::Tone &us = tones::find("USA")->dial;
	EXPECT_EQ(us.f1 + us.f2, 790);
	EXPECT_EQ(us.n, 0);
	EXPECT_TRUE(tones::on_at(us, 1234.5));
	EXPECT_EQ(tones::find("GERMANY")->dial.f1, 425);
	EXPECT_EQ(tones::find("GERMANY")->dial.n, 0);
	const tones::Tone &it = tones::find("ITALY")->dial;
	ASSERT_EQ(it.n, 4);
	EXPECT_TRUE(tones::on_at(it, 0.1));
	EXPECT_FALSE(tones::on_at(it, 0.3));
	EXPECT_TRUE(tones::on_at(it, 0.6));
	EXPECT_FALSE(tones::on_at(it, 1.5));
	EXPECT_NEAR(tones::period(it), 2.0, 1e-6);
	/* busy and congestion: the USA's 480 + 620 Hz, Germany's 425 Hz at
	   0.48 s on and off, and twice as fast */
	const tones::Country &de = *tones::find("GERMANY");
	EXPECT_EQ(tones::find("USA")->busy.f2, 620);
	EXPECT_EQ(de.busy.n, 2);
	EXPECT_EQ(de.busy.cadence[0], 0.48f);
	EXPECT_EQ(de.congestion.cadence[1], 0.24f);
}

/* each country's ringing, 26 s of it with no auto answer: a RING for
   each of its rings */
TEST(RingTones, RingEachCountry)
{
	for (const tones::Country *c = tones::COUNTRIES; c->name; c++) {
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
	for (const tones::Country *c = tones::COUNTRIES; c->name; c++) {
		SCOPED_TRACE(c->name);
		double period = 0;
		for (int k = 0; k < c->ringing.n; k++)
			period += c->ringing.cadence[k];
		unsigned s0 = period <= 10.5 ? 2 : 1;
		bool answered = false;
		std::string out = ring_for(*c, 30, s0, &answered);
		EXPECT_TRUE(answered);
		EXPECT_EQ(rings(out), s0);
	}
}

/* a modem in each country takes its busy and its congestion tone (the
   ITU list's, tones.h) for busy: BUSY on each with a cadence */
TEST(RingTones, BusyEachCountry)
{
	for (const tones::Country *c = tones::COUNTRIES; c->name; c++)
		for (const tones::Tone *t : { &c->busy, &c->congestion }) {
			if (!t->n)
				continue;       /* (steady: no cadence) */
			SCOPED_TRACE(std::string(c->name) +
				     (t == &c->busy ? " busy" : " congestion"));
			std::string out = dial_into(*c, *t, 45);
			EXPECT_NE(out.find("BUSY"), std::string::npos) << out;
		}
}

/* and not its ringing tone */
TEST(RingTones, RingingIsNoBusy)
{
	for (const tones::Country *c = tones::COUNTRIES; c->name; c++) {
		SCOPED_TRACE(c->name);
		std::string out = dial_into(*c, c->ringing, 30);
		EXPECT_EQ(out.find("BUSY"), std::string::npos);
	}
}
