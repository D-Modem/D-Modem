/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_startup_test.cpp - V.cvm's start-up (dsp/vcvm_startup.h): the
 *                            capability field, INFO0 and MP, the message
 *                            mode, the probing and the loading, and two
 *                            DCEs' start-ups on a line with a delay, a
 *                            gain, a clock difference and the codecs of
 *                            codec_sim (where the libraries are), against
 *                            clause 13's performance
 *
 */

#include <math.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bits.h"
#include "vcvm_line.h"
#include "vcvm_startup.h"

using namespace dsp;

namespace {

using namespace vcvm_test;

VcvmCaps caps(bool fs16 = false)
{
	VcvmCaps c;
	c.fs16 = fs16;
	c.wide = c.wide400 = fs16;
	return c;
}

} // namespace

/*
 *    the field, INFO0 and MP
 */

TEST(VcvmCaps, Field)
{
	VcvmCaps c;
	c.fs16 = true;
	c.wide = true;
	c.fax = true;
	c.param = false;
	uint8_t o[2];
	c.encode(o);
	EXPECT_EQ(o[0], 0x20 | 0x10 | 0x08 | 0x04);
	EXPECT_EQ(o[1], 0x80 | 0x40 | 0x20 | 0x08 | 0x04);
	VcvmCaps d = VcvmCaps::decode(o);
	EXPECT_TRUE(d.fs16 && d.wide && d.fax && d.rings && d.dpsk8 && d.sym22);
	EXPECT_FALSE(d.param || d.wide400 || d.quick);
	EXPECT_EQ(d.fs(), 16000);
	/* a later version: version 1's bits */
	o[0] = (uint8_t)((o[0] & 0x1f) | 0x40);
	EXPECT_EQ(VcvmCaps::decode(o).version, 1);
}

TEST(VcvmCaps, NonStandardFacilities)
{
	VcvmCaps c = caps(true), d;
	uint8_t nsf[16];
	int n = vcvm_nsf_build(c, nsf);
	EXPECT_EQ(n, 6);
	EXPECT_EQ(nsf[3], VCVM_NSF_ID);
	ASSERT_TRUE(vcvm_nsf_find(nsf, n, &d));
	EXPECT_TRUE(d.fs16 && d.wide && d.wide400 && d.param);
	/* a provider code of another length */
	uint8_t other[] = { 0x61, 0x12, 0x43, 0x20, 0xC1 };
	ASSERT_TRUE(vcvm_nsf_find(other, 5, &d));
	EXPECT_TRUE(d.param && !d.fs16);
	/* a provider's other use of the category */
	uint8_t none[] = { 0xB5, 0x00, 0x43, 0x00, 0x12, 0x34 };
	EXPECT_FALSE(vcvm_nsf_find(none, 6, &d));
}

TEST(VcvmCaps, V8)
{
	/* in a CM: after the standard categories, extension octets only */
	VcvmCaps c = caps(true), d;
	c.param = false;
	uint8_t cm[40] = { 0xe0, 0xc1, 0x45, 0x13, 0x94, 0x2a, 0x0d };
	int n = 7 + vcvm_v8_ns(c, cm + 7);
	EXPECT_EQ(n, 7 + 1 + 13);
	EXPECT_EQ(cm[7], 0x0F);
	for (int i = 8; i < n; i++) {
		EXPECT_EQ(cm[i] & 0x38, 0x10) << i;
		EXPECT_NE(cm[i], 0);
	}
	ASSERT_TRUE(vcvm_v8_find(cm, n, &d));
	EXPECT_TRUE(d.fs16 && d.wide && !d.param);
	/* none in a plain CM, nor in a provider's other NS block */
	EXPECT_FALSE(vcvm_v8_find(cm, 7, &d));
	uint8_t other[40] = { 0xe0, 0xc1, 0x45, 0x0F };
	for (int i = 4; i < 12; i++)
		other[i] = 0x10 | (uint8_t)(i & 7);
	EXPECT_FALSE(vcvm_v8_find(other, 12, &d));
	/* the categories' parser of the V.8 pump: no octet of the field
	   reads as a category tag */
	for (int i = 8; i < n; i++)
		for (int tag : { 0x01, 0x05, 0x07, 0x0A, 0x0D })
			EXPECT_NE(cm[i] & 0x1F, tag);
}

TEST(VcvmCaps, Allowed)
{
	VcvmCaps a = caps(true), b = caps(true);
	EXPECT_EQ(vcvm_allowed(a, b, VCVM_M_ALL),
		  VCVM_M_ALL | VCVM_M_SYM22 | VCVM_M_WIDE400);
	b.param = false;
	b.rings = false;
	int x = vcvm_allowed(a, b, VCVM_M_ALL & ~VCVM_M_8DPSK);
	EXPECT_FALSE(x & (VCVM_M_PARAM | VCVM_M_RINGS | VCVM_M_8DPSK));
	EXPECT_TRUE(x & VCVM_M_WIDE);
	/* wideband needs both at 16000 Hz */
	EXPECT_FALSE(vcvm_allowed(caps(false), b, VCVM_M_ALL) & VCVM_M_WIDE);
	EXPECT_EQ(vcvm_probe_combs(8000, VCVM_M_ALL), 7u);
	EXPECT_EQ(vcvm_probe_combs(16000, VCVM_M_ALL | VCVM_M_WIDE400), 0x38u);
	EXPECT_EQ(vcvm_probe_combs(16000, VCVM_M_ALL), 0x18u);
}

TEST(VcvmMessages, Info0)
{
	VcvmInfo0 i, j;
	i.modes = caps().modes();
	i.combs = 0x105;
	i.quick_offer = true;
	i.level = 6;
	uint8_t o[4];
	ASSERT_EQ(i.encode(o), 4);
	EXPECT_EQ(o[1], 0xA0);
	EXPECT_EQ(o[2], 0xD0);
	ASSERT_EQ(j.decode(o, 4), 0);
	EXPECT_EQ(j.combs, 0x105u);
	EXPECT_TRUE(j.quick_offer && !j.quick_accept && j.l2() && j.l3());
	EXPECT_EQ(j.level, 6);
	o[0] = 0;
	EXPECT_EQ(j.decode(o, 4), -1);
}

TEST(VcvmMessages, Mp)
{
	VcvmMp m, d;
	m.ack = true;
	m.rtt = 31;
	m.l.kind = VcvmLoading::COMB;
	m.l.code = 1;
	m.l.tones = 11;
	m.l.rate = 1;
	for (int k = 0; k < 11; k++) {
		m.l.load.phase[k] = (uint8_t)(k < 2 ? 0 : k < 6 ? 2 : 3);
		m.l.load.ring[k] = (uint8_t)(k >= 9 ? 2 : k >= 7 ? 1 : 0);
	}
	uint8_t o[48];
	int n = m.encode(o);
	EXPECT_EQ(n, 5 + 3 + 3);
	EXPECT_EQ(o[0], 0x80 | 0x08);
	EXPECT_EQ(o[3], 0x14);
	EXPECT_EQ(o[4], 11);
	EXPECT_EQ(o[5], 0x05);          /* 00 00 01 01 */
	ASSERT_EQ(d.decode(o, n), 0);
	EXPECT_TRUE(d.ack);
	EXPECT_EQ(d.rtt, 31);
	EXPECT_EQ(d.l.kind, VcvmLoading::COMB);
	EXPECT_EQ(d.l.code, 1);
	EXPECT_EQ(d.l.rate, 1);
	EXPECT_EQ(memcmp(d.l.load.phase, m.l.load.phase, 11), 0);
	EXPECT_EQ(memcmp(d.l.load.ring, m.l.load.ring, 11), 0);
	EXPECT_EQ(d.l.raw_bps(), m.l.raw_bps());
	EXPECT_EQ(m.l.raw_bps(), (4 * 2 + 5 * 3 + 2 * 1 + 2 * 2) * 50);
	EXPECT_EQ(d.decode(o, n - 1), -1);
	/* the comb's tones must be its own */
	o[4] = 12;
	EXPECT_EQ(d.decode(o, n), -1);
	/* the parametric mode; no data mode */
	m = VcvmMp();
	m.l.kind = VcvmLoading::PARAM;
	m.l.p10 = true;
	m.l.rate = 2;
	n = m.encode(o);
	EXPECT_EQ(n, 5);
	ASSERT_EQ(d.decode(o, n), 0);
	EXPECT_EQ(d.l.kind, VcvmLoading::PARAM);
	EXPECT_TRUE(d.l.p10);
	EXPECT_EQ(d.l.raw_bps(), 400);
	EXPECT_EQ(d.rtt, 255);
	m = VcvmMp();
	n = m.encode(o);
	ASSERT_EQ(d.decode(o, n), 0);
	EXPECT_EQ(d.l.kind, VcvmLoading::NONE);
	/* the 22.5 ms symbol */
	m.l.kind = VcvmLoading::COMB;
	m.l.code = 0;
	m.l.sym22 = true;
	m.l.tones = 16;
	m.l.load.all(16, 2);
	n = m.encode(o);
	ASSERT_EQ(d.decode(o, n), 0);
	EXPECT_TRUE(d.l.sym22);
	EXPECT_EQ(d.l.raw_bps(), 1422);
}

/*
 *    the message mode
 */

namespace {

/* two messages 100 ms apart from one DCE to the other: what came */
std::vector<VcvmMsg> messages(int fs, bool param, bool calling,
			      const char *codec, double ppm = 0)
{
	VcvmMsgTx tx;
	VcvmMsgRx rx;
	EXPECT_EQ(tx.init(fs, param, calling), 0);
	EXPECT_EQ(rx.init(fs, param, calling), 0);
	std::vector<float> x((size_t)fs / 5, 0.0f);
	uint8_t a[] = { 0x7e, 0x7d, 0x00, 0xff, 0x12 };
	uint8_t b[20];
	for (int i = 0; i < 20; i++)
		b[i] = (uint8_t)(i * 37 + 1);
	tx.send(a, sizeof(a), x);
	x.insert(x.end(), (size_t)fs / 10, 0.0f);
	tx.send(b, sizeof(b), x);
	std::vector<float> y = through(x, fs, 41, 0.5, ppm, codec);
	for (size_t i = 0; i < y.size(); i += 160)
		rx.push(&y[i], (int)std::min<size_t>(160, y.size() - i));
	std::vector<VcvmMsg> out;
	VcvmMsg m;
	while (rx.get(&m))
		out.push_back(m);
	return out;
}

void expect_messages(const std::vector<VcvmMsg> &v)
{
	ASSERT_EQ(v.size(), 2u);
	EXPECT_TRUE(v[0].ok);
	EXPECT_TRUE(v[1].ok);
	EXPECT_EQ(v[0].octets.size(), 5u);
	EXPECT_EQ(v[1].octets.size(), 20u);
	if (v[1].octets.size() == 20) {
		EXPECT_EQ(v[1].octets[19], (uint8_t)(19 * 37 + 1));
	}
	EXPECT_LT(v[0].end, v[1].start);
}

} // namespace

TEST(VcvmMessages, Clean)
{
	for (int fs : { 8000, 16000 })
		for (bool param : { true, false })
			for (bool calling : { true, false }) {
				SCOPED_TRACE(std::to_string(fs) + (param ? " P20" :
					     " V.21") + (calling ? " calling" : " answering"));
				expect_messages(messages(fs, param, calling, "", 200));
			}
}

TEST(VcvmMessages, Codecs)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	/* P20 through AMR 7.4 and AMR-WB 6.6; V.21 through AMR 12.2 (its bit
	   error ratio through AMR 7.4 to 10.2 is 1 to 5 10^-3, which a long
	   message sent twice does not always survive) */
	{
		SCOPED_TRACE("P20 amr:7.4");
		expect_messages(messages(8000, true, true, "amr:7.4"));
	}
	{
		SCOPED_TRACE("P20 amrwb:6.6");
		expect_messages(messages(16000, true, false, "amrwb:6.6"));
	}
	{
		SCOPED_TRACE("V.21 amr:12.2");
		expect_messages(messages(8000, false, false, "amr:12.2"));
		expect_messages(messages(8000, false, true, "amr:12.2"));
	}
}

TEST(VcvmMessages, V21Alignments)
{
	/* the bit clock pulled in from every phase of the first bit, after a
	   long silence */
	VcvmMp mp;
	mp.rtt = 12;
	mp.l.kind = VcvmLoading::COMB;
	mp.l.tones = 16;
	for (int k = 0; k < 16; k++) {
		mp.l.load.phase[k] = 3;
		mp.l.load.ring[k] = 2;
	}
	uint8_t o[48];
	int n = mp.encode(o);
	for (int lead = 32000; lead < 32027; lead++) {
		VcvmMsgTx tx;
		VcvmMsgRx rx;
		tx.init(8000, false, true);
		rx.init(8000, false, true);
		std::vector<float> x((size_t)lead, 0.0f);
		tx.send(o, n, x);
		std::vector<float> y = through(x, 8000, 480, 0.7, -100, "");
		for (size_t i = 0; i < y.size(); i += 160)
			rx.push(&y[i], (int)std::min<size_t>(160, y.size() - i));
		VcvmMsg g;
		ASSERT_TRUE(rx.get(&g)) << lead;
		EXPECT_TRUE(g.ok) << lead;
		EXPECT_EQ((int)g.octets.size(), n) << lead;
	}
}

/*
 *    the loading
 */

namespace {

void fill(VcvmCombMeas *c, int code, bool sym22, float sigma, float sa)
{
	VcvmComb cb;
	static const int fs[3] = { 8000, 16000, 32000 };
	cb.init(code, fs[code / 3], sym22);
	c->ok = true;
	c->code = code;
	c->sym22 = sym22;
	c->tones = cb.tones;
	c->n = 27;
	for (int k = 0; k < cb.tones; k++) {
		c->sigma[k] = sigma;
		c->sigma_a[k] = sa;
		c->level_db[k] = -1;
		c->errs[k] = 0;
	}
}

VcvmProbeMeas narrow(float sigma, float sa)
{
	VcvmProbeMeas m;
	m.ncomb = 3;
	for (int i = 0; i < 3; i++)
		fill(&m.comb[i], i, false, sigma, sa);
	fill(&m.l2, 0, true, sigma, sa);
	m.l3 = true;
	m.n20 = 40;
	m.n10 = 80;
	return m;
}

const int ALL = VCVM_M_ALL | VCVM_M_SYM22;

} // namespace

TEST(VcvmLoading, Orders)
{
	VcvmLoading l = vcvm_choose(narrow(5, 0.3f), ALL);
	EXPECT_EQ(l.kind, VcvmLoading::COMB);
	EXPECT_EQ(l.code, 0);
	EXPECT_FALSE(l.sym22);
	EXPECT_EQ(l.raw_bps(), 16 * 5 * 50);
	EXPECT_EQ(l.rate, 2);
	/* the mode set without 8-DPSK and rings */
	l = vcvm_choose(narrow(5, 0.3f), ALL & ~(VCVM_M_8DPSK | VCVM_M_RINGS));
	EXPECT_EQ(l.raw_bps(), 1600);
	/* the thresholds: 9 and 20 degrees, 0.4 and 0.8 dB */
	l = vcvm_choose(narrow(9, 0.8f), ALL);
	EXPECT_EQ(l.load.phase[0], 3);
	EXPECT_EQ(l.load.ring[0], 1);
	l = vcvm_choose(narrow(9.1f, 0.81f), ALL);
	EXPECT_EQ(l.load.phase[0], 2);
	EXPECT_EQ(l.load.ring[0], 0);
	EXPECT_EQ(l.rate, 2);
	l = vcvm_choose(narrow(20, 1), ALL);
	EXPECT_EQ(l.load.phase[0], 2);
	EXPECT_EQ(l.rate, 0);
}

TEST(VcvmLoading, BandAndComb)
{
	/* the codec's band: the tones over 2500 Hz 25 dB down */
	VcvmProbeMeas m = narrow(5, 1);
	for (VcvmCombMeas *c : { &m.comb[0], &m.comb[1], &m.comb[2], &m.l2 }) {
		VcvmComb cb;
		cb.init(c->code, 8000, c->sym22);
		for (int k = 0; k < cb.tones; k++)
			if (cb.freq(k) > 2500)
				c->level_db[k] = -25;
	}
	VcvmLoading l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.code, 0);
	EXPECT_EQ(l.loaded(), 11);
	/* the denser comb too noisy: the 300 Hz one */
	m = narrow(5, 1);
	for (int k = 0; k < 16; k++)
		m.comb[0].sigma[k] = m.l2.sigma[k] = 25;
	l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.code, 1);
	EXPECT_EQ(l.raw_bps(), 11 * 3 * 50);
	/* the 22.5 ms symbol where the 20 ms one is worse */
	m = narrow(15, 1);
	for (int k = 0; k < 16; k++)
		m.l2.sigma[k] = 8;
	l = vcvm_choose(m, ALL);
	EXPECT_TRUE(l.sym22);
	EXPECT_EQ(l.raw_bps(), 2133);
	EXPECT_EQ(vcvm_choose(m, ALL & ~VCVM_M_SYM22).sym22, false);
	/* measured errors set the code rate */
	m = narrow(5, 1);
	for (int k = 0; k < 16; k++)
		m.comb[0].errs[k] = 1;
	EXPECT_EQ(vcvm_choose(m, ALL & ~VCVM_M_8DPSK).rate, 0);
}

TEST(VcvmLoading, Parametric)
{
	/* three tones: the parametric mode */
	VcvmProbeMeas m = narrow(30, 1);
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
			m.comb[i].sigma[k] = 5;
	VcvmLoading l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.kind, VcvmLoading::PARAM);
	EXPECT_TRUE(l.p10);
	m.e10 = 2;
	l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.kind, VcvmLoading::PARAM);
	EXPECT_FALSE(l.p10);
	EXPECT_EQ(l.raw_bps(), 200);
	EXPECT_EQ(l.rate, 2);
	/* one P20 symbol in error: P20 at rate 1/2; two: no data mode */
	m.e20 = 1;
	l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.kind, VcvmLoading::PARAM);
	EXPECT_EQ(l.rate, 0);
	m.e20 = 2;
	EXPECT_EQ(vcvm_choose(m, ALL).kind, VcvmLoading::NONE);
	m.e20 = 0;
	EXPECT_EQ(vcvm_choose(m, ALL & ~VCVM_M_PARAM).kind, VcvmLoading::NONE);
	/* four tones load the comb where they carry more than P20 */
	m.comb[0].sigma[3] = 5;
	EXPECT_EQ(vcvm_choose(m, ALL).kind, VcvmLoading::COMB);
	/* and not where they carry less than P10 after the code */
	m.e10 = 0;
	for (int k = 0; k < 4; k++)
		m.comb[0].sigma[k] = 19;
	EXPECT_EQ(vcvm_choose(m, ALL).kind, VcvmLoading::PARAM);
	EXPECT_TRUE(vcvm_choose(m, ALL).p10);
}

/*
 *    the probing
 */

namespace {

VcvmProbeMeas probe(int fs, unsigned combs, const char *codec, double ppm,
		    bool *ok)
{
	VcvmProbe p;
	p.fs = fs;
	p.combs = combs;
	p.l2 = p.l3 = true;
	p.tap = SCR_GPA;
	std::vector<float> x((size_t)fs / 5, 0.0f);
	vcvm_probe_send(p, x);
	long len = (long)x.size() - fs / 5;
	std::vector<float> y = through(x, fs, 53, 0.5, ppm, codec);
	VcvmProbeRx rx;
	EXPECT_EQ(rx.init(p), 0);
	rx.start(fs / 10);
	for (size_t i = (size_t)fs / 10; i < y.size(); i += 160)
		rx.push(&y[i], (int)std::min<size_t>(160, y.size() - i));
	*ok = rx.done();
	EXPECT_TRUE(rx.done());
	/* the end where it is, the delay and the clock counted (and a
	   codec's own delay, 33 ms at most) */
	double want = fs / 5 + 53 + PATH_MARGIN + len * (1 - ppm * 1e-6);
	EXPECT_GE((double)rx.end(), want - 8);
	EXPECT_LE((double)rx.end(), want + (codec && *codec ? fs / 30 : 8));
	return rx.meas();
}

double max_sigma(const VcvmCombMeas &c, int from, int to)
{
	double m = 0;
	for (int k = from; k < to; k++)
		m = std::max(m, (double)c.sigma[k]);
	return m;
}

} // namespace

TEST(VcvmProbe, Clean)
{
	for (int fs : { 8000, 16000 }) {
		SCOPED_TRACE(fs);
		bool ok;
		VcvmProbeMeas m = probe(fs, vcvm_probe_combs(fs, ALL | VCVM_M_WIDE400),
					"", -150, &ok);
		ASSERT_TRUE(ok);
		ASSERT_EQ(m.ncomb, 3);
		for (int i = 0; i < 3; i++) {
			EXPECT_TRUE(m.comb[i].ok);
			EXPECT_EQ(m.comb[i].n, VcvmProbe::L1_SYMS - 3);
			EXPECT_LT(max_sigma(m.comb[i], 0, m.comb[i].tones), 2);
		}
		EXPECT_TRUE(m.l2.ok);
		EXPECT_LT(max_sigma(m.l2, 0, m.l2.tones), 2);
		EXPECT_TRUE(m.l3);
		EXPECT_EQ(m.n20, 40);
		EXPECT_EQ(m.n10, 80);
		EXPECT_EQ(m.e20 + m.e10, 0);
		VcvmLoading l = vcvm_choose(m, ALL | VCVM_M_WIDE400);
		EXPECT_EQ(l.kind, VcvmLoading::COMB);
		EXPECT_EQ(l.code, fs == 8000 ? 0 : 3);
		EXPECT_EQ(l.rate, 2);
		EXPECT_EQ(l.raw_bps(), l.tones * 5 * 50) << l.str();
	}
}

TEST(VcvmProbe, Amr)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	bool ok;
	VcvmProbeMeas m = probe(8000, 7, "amr:12.2", 100, &ok);
	ASSERT_TRUE(ok);
	VcvmLoading l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.kind, VcvmLoading::COMB) << l.str();
	EXPECT_GE(l.raw_bps(), 1200) << l.str();
	EXPECT_GE(l.rate, 1) << l.str();
	EXPECT_EQ(m.e20, 0);
	EXPECT_GT(m.cbar, 0.9);
	/* AMR 7.4: no comb, the parametric mode */
	m = probe(8000, 7, "amr:7.4", 0, &ok);
	ASSERT_TRUE(ok);
	l = vcvm_choose(m, ALL);
	EXPECT_EQ(l.kind, VcvmLoading::PARAM) << l.str();
	EXPECT_EQ(m.e20, 0);
}

/*
 *    the start-up, both DCEs
 */

namespace {

struct Call {
	VcvmStartup a, b;               /* calling, answering */
	bool ok = false;
	double secs = 0;
};

void startup(Call *c, int fs, const char *codec, double ppm = 0,
	     VcvmCaps ca = VcvmCaps(), VcvmCaps cb = VcvmCaps(),
	     int modes_a = VCVM_M_ALL, int modes_b = VCVM_M_ALL)
{
	VcvmStartup::Config k;
	k.fs = fs;
	k.calling = true;
	k.own = ca;
	k.peer = cb;
	k.modes = modes_a;
	ASSERT_EQ(c->a.init(k), 0);
	k.calling = false;
	k.own = cb;
	k.peer = ca;
	k.modes = modes_b;
	ASSERT_EQ(c->b.init(k), 0);
	/* 60 ms each way, the codec's and the radio's */
	Path ab(fs, fs * 60 / 1000, 0.5, ppm, codec);
	Path ba(fs, fs * 60 / 1000, 0.7, -ppm, codec);
	const int B = fs / 50;
	std::vector<float> ia(B), oa(B), ib(B), ob(B);
	long t = 0;
	for (; t < 40L * fs; t += B) {
		ba.pull(ia.data(), B);
		c->a.process(ia.data(), oa.data(), B);
		ab.push(oa.data(), B);
		ab.pull(ib.data(), B);
		c->b.process(ib.data(), ob.data(), B);
		ba.push(ob.data(), B);
		if (c->a.state() != VcvmStartup::RUNNING &&
		    c->b.state() != VcvmStartup::RUNNING)
			break;
	}
	c->secs = (double)t / fs;
	c->ok = c->a.state() == VcvmStartup::DONE &&
		c->b.state() == VcvmStartup::DONE;
	EXPECT_TRUE(c->ok) << "calling:\n" << c->a.log() << "answering:\n"
			   << c->b.log();
	if (!c->ok)
		return;
	/* what each asked for, the other transmits */
	auto same = [](const VcvmLoading &x, const VcvmLoading &y) {
		return x.kind == y.kind && x.code == y.code && x.sym22 == y.sym22 &&
		       x.p10 == y.p10 && x.rate == y.rate &&
		       x.raw_bps() == y.raw_bps();
	};
	EXPECT_TRUE(same(c->a.rx_loading(), c->b.tx_loading()));
	EXPECT_TRUE(same(c->b.rx_loading(), c->a.tx_loading()));
	/* close together: an MP apart at most (2.5 s for W200's) */
	EXPECT_LT(labs(c->a.done_at() - c->b.done_at()), 3L * fs);
	/* the round trip: 120 ms, the line margin's 24 and the codecs' */
	if (c->a.rtt_ms() > 260 || c->b.rtt_ms() > 260)
		printf("calling:\n%sanswering:\n%s", c->a.log().c_str(),
		       c->b.log().c_str());
	EXPECT_GE(c->a.rtt_ms(), 110);
	EXPECT_LE(c->a.rtt_ms(), 260);
	EXPECT_GE(c->b.rtt_ms(), 110);
	EXPECT_LE(c->b.rtt_ms(), 260);
}

} // namespace

TEST(VcvmStartup, Clean)
{
	Call c;
	startup(&c, 8000, "", 100);
	if (!c.ok)
		return;
	for (const VcvmStartup *s : { &c.a, &c.b }) {
		EXPECT_TRUE(s->param_messages());
		EXPECT_EQ(s->rx_loading().kind, VcvmLoading::COMB);
		EXPECT_EQ(s->rx_loading().code, 0);
		EXPECT_EQ(s->rx_loading().raw_bps(), 4000)
			<< s->rx_loading().str();
	}
	EXPECT_LT(c.secs, 20);
	printf("%s", c.a.log().c_str());
}

TEST(VcvmStartup, V21Messages)
{
	/* the answering DCE without the parametric mode: messages in V.21,
	   no L3 */
	VcvmCaps nb;
	nb.param = false;
	Call c;
	startup(&c, 8000, "", -100, VcvmCaps(), nb);
	if (!c.ok)
		return;
	EXPECT_FALSE(c.a.param_messages());
	EXPECT_FALSE(c.a.meas().l3);
	EXPECT_EQ(c.a.rx_loading().code, 0);
	/* the mode set: DQPSK alone */
	Call d;
	startup(&d, 8000, "", 0, VcvmCaps(), VcvmCaps(),
		VCVM_M_ALL & ~(VCVM_M_8DPSK | VCVM_M_RINGS), VCVM_M_ALL);
	if (!d.ok)
		return;
	EXPECT_EQ(d.a.rx_loading().raw_bps(), 1600);
	EXPECT_EQ(d.b.rx_loading().raw_bps(), 1600);
}

TEST(VcvmStartup, Amr122)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	Call c;
	startup(&c, 8000, "amr:12.2", 100);
	if (!c.ok)
		return;
	/* 13 a): a narrowband mode of 1200 bit/s, rate 3/4 or better */
	for (const VcvmStartup *s : { &c.a, &c.b }) {
		EXPECT_EQ(s->rx_loading().kind, VcvmLoading::COMB);
		EXPECT_GE(s->rx_loading().raw_bps(), 1200) << s->rx_loading().str();
		EXPECT_GE(s->rx_loading().rate, 1) << s->rx_loading().str();
	}
	printf("%s", c.b.log().c_str());
}

TEST(VcvmStartup, Amr74)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	/* 13 b): 200 bit/s or more: the parametric mode, or four tones
	   of N300 where they carry more after the code */
	Call c;
	startup(&c, 8000, "amr:7.4");
	if (!c.ok)
		return;
	for (const VcvmStartup *s : { &c.a, &c.b }) {
		const VcvmLoading &l = s->rx_loading();
		EXPECT_NE(l.kind, VcvmLoading::NONE);
		EXPECT_GE(l.raw_bps(), 200) << l.str();
		EXPECT_GE(l.coded_bps(), 175) << l.str();
		if (l.kind == VcvmLoading::COMB) {
			EXPECT_LE(l.loaded(), 6) << l.str();
		}
	}
	printf("%s", c.a.log().c_str());
}

TEST(VcvmStartup, AmrWb)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR-WB";
	/* 13 c): through AMR-WB 23.85 at 16000 Hz, a wideband mode of 2400
	   bit/s */
	Call c;
	startup(&c, 16000, "amrwb:23.85", 0, caps(true), caps(true));
	if (!c.ok)
		return;
	for (const VcvmStartup *s : { &c.a, &c.b }) {
		EXPECT_EQ(s->rx_loading().kind, VcvmLoading::COMB);
		EXPECT_GE(s->rx_loading().code, 3);
		EXPECT_GE(s->rx_loading().raw_bps(), 2400) << s->rx_loading().str();
	}
	printf("%s", c.a.log().c_str());
}

TEST(VcvmStartup, Lines)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	/* the delays and clock differences of a mobile call, through the
	   modes that carry a comb (AMR 12.2 and 10.2) and the parametric
	   mode alone (AMR 7.4) */
	struct {
		const char *codec;
		int ms;
		double ppm;
	} lines[] = {
		{ "amr:12.2", 5, -200 }, { "amr:12.2", 293, 130 },
		{ "amr:10.2", 37, 0 }, { "amr:10.2", 151, -200 },
		{ "amr:7.4", 60, -200 }, { "amr:7.4", 151, 130 },
	};
	for (const auto &ln : lines) {
		SCOPED_TRACE(std::string(ln.codec) + " " + std::to_string(ln.ms) +
			     " ms " + std::to_string((int)ln.ppm) + " ppm");
		VcvmStartup a, b;
		VcvmStartup::Config k;
		k.calling = true;
		ASSERT_EQ(a.init(k), 0);
		k.calling = false;
		ASSERT_EQ(b.init(k), 0);
		Path ab(8000, 8 * ln.ms, 0.5, ln.ppm, ln.codec);
		Path ba(8000, 8 * ln.ms + 3, 0.8, -ln.ppm, ln.codec);
		std::vector<float> ia(160), oa(160), ib(160), ob(160);
		for (long t = 0; t < 40L * 8000; t += 160) {
			ba.pull(ia.data(), 160);
			a.process(ia.data(), oa.data(), 160);
			ab.push(oa.data(), 160);
			ab.pull(ib.data(), 160);
			b.process(ib.data(), ob.data(), 160);
			ba.push(ob.data(), 160);
			if (a.state() != VcvmStartup::RUNNING &&
			    b.state() != VcvmStartup::RUNNING)
				break;
		}
		ASSERT_EQ(a.state(), VcvmStartup::DONE) << a.log() << b.log();
		ASSERT_EQ(b.state(), VcvmStartup::DONE) << a.log() << b.log();
		bool comb = strcmp(ln.codec, "amr:7.4") != 0;
		for (const VcvmStartup *s : { &a, &b }) {
			const VcvmLoading &l = s->rx_loading();
			if (comb) {
				EXPECT_EQ(l.kind, VcvmLoading::COMB) << l.str();
				EXPECT_GE(l.raw_bps(), 900) << l.str();
			} else
				EXPECT_NE(l.kind, VcvmLoading::NONE) << l.str();
			/* the round trip: twice the delay, 24 ms of the line's
			   margin and the codec's 50 */
			EXPECT_NEAR(s->rtt_ms(), 2 * ln.ms + 24 + 50, 40);
		}
	}
}
