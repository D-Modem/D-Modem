/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_data_test.cpp - V.cvm's data mode (dsp/vcvm_data.h): octets
 *                         through the inner code, the interleaver, the
 *                         scrambler and a loading's tones or lags, back
 *                         to back and through the codecs; the control
 *                         sequence; the loss of the signal
 *
 */

#include <string.h>

#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bits.h"
#include "vcvm_data.h"
#include "vcvm_line.h"

using namespace dsp;
using namespace vcvm_test;

namespace {

/* a loading of comb code at fs: each of its tones pb bits of phase and
   rb of ring, at rate (and sym22) */
VcvmLoading comb(int code, int fs, int pb, int rb, int rate, bool sym22 = false)
{
	VcvmLoading l;
	VcvmComb c;
	c.init(code, fs, sym22);
	l.kind = VcvmLoading::COMB;
	l.code = code;
	l.sym22 = sym22;
	l.tones = c.tones;
	l.rate = rate;
	l.load.all(c.tones, pb);
	for (int k = 0; k < c.tones; k++)
		l.load.ring[k] = (uint8_t)rb;
	return l;
}

/* a parametric loading, P10 or P20, at rate */
VcvmLoading param(bool p10, int rate)
{
	VcvmLoading l;
	l.kind = VcvmLoading::PARAM;
	l.p10 = p10;
	l.rate = rate;
	return l;
}

struct Result {
	std::vector<uint8_t> sent, got;
	VcvmDataStats st;
	bool confirmed = false, control = false, lost = false;
	long wrong = 0;                 /* octets of got not as sent */
};

/* secs of random octets from a transmitter through the line to a
   receiver; then (ctl) the control sequence, or (quiet) silence */
Result call(const VcvmLoading &l, int fs, double secs, const char *codec,
	    double ppm, bool ctl = false, double quiet = 0)
{
	Result r;
	std::mt19937 rng(11);
	VcvmDataTx tx;
	VcvmDataRx rx;
	EXPECT_EQ(tx.init(l, fs, SCR_GPA, [&]() {
		uint8_t v = (uint8_t)rng();
		r.sent.push_back(v);
		return (int)v;
	}), 0);
	EXPECT_EQ(rx.init(l, fs, SCR_GPA, [&](const uint8_t *d, int n) {
		r.got.insert(r.got.end(), d, d + n);
	}), 0);
	std::vector<float> x((size_t)fs / 5, 0.0f);
	tx.start(x);
	long n = (long)(secs * 1000 / l.sym_ms());
	for (long i = 0; i < n; i++)
		tx.symbol(x);
	if (ctl)
		tx.control(x);
	x.insert(x.end(), (size_t)(quiet * fs), 0.0f);
	Path p(fs, 41, 0.6, ppm, codec);
	rx.start(0);
	for (size_t i = 0; i < x.size(); i += 160) {
		int k = (int)std::min<size_t>(160, x.size() - i);
		p.push(&x[i], k);
		std::vector<float> y((size_t)k);
		p.pull(y.data(), k);
		rx.push(y.data(), k);
	}
	r.st = rx.stats();
	r.confirmed = rx.confirmed();
	r.control = rx.control();
	r.lost = rx.lost();
	for (size_t i = 0; i < r.got.size() && i < r.sent.size(); i++)
		r.wrong += r.got[i] != r.sent[i];
	return r;
}

} // namespace

/* combs on a clean line, at 8000 and 16000 Hz: every octet through,
   none corrected, the phase errors small */
TEST(VcvmData, CombClean)
{
	/* N200 with 8-DPSK and four rings: 80 bits a symbol */
	Result r = call(comb(0, 8000, 3, 2, 2), 8000, 5, "", 150);
	EXPECT_TRUE(r.confirmed);
	EXPECT_GT(r.got.size(), 2000u);
	EXPECT_EQ(r.wrong, 0);
	EXPECT_EQ(r.st.failed, 0);
	EXPECT_EQ(r.st.corrected, 0);
	EXPECT_FALSE(r.control);
	/* every loaded tone's phase error small */
	for (int k = 0; k < 16; k++)
		EXPECT_LT(r.st.sigma[k], 3) << k;
	/* at 16000 Hz, W300 */
	r = call(comb(4, 16000, 3, 0, 1), 16000, 3, "", -150);
	EXPECT_TRUE(r.confirmed);
	EXPECT_EQ(r.wrong, 0);
	EXPECT_EQ(r.st.failed, 0);
}

/* P20 and P10 on a clean line: every octet through */
TEST(VcvmData, ParamClean)
{
	for (bool p10 : { false, true }) {
		Result r = call(param(p10, 2), 8000, 8, "", 100);
		EXPECT_TRUE(r.confirmed) << p10;
		EXPECT_GE(r.got.size(), 28u * 2);
		EXPECT_EQ(r.wrong, 0);
		EXPECT_EQ(r.st.failed, 0);
	}
}

/* combs and P20 through AMR and AMR-WB: every octet through */
TEST(VcvmData, Codecs)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	/* N200 with DQPSK at rate 3/4 through AMR 12.2 (1200 bit/s coded) */
	Result r = call(comb(0, 8000, 2, 0, 1), 8000, 8, "amr:12.2", 100);
	EXPECT_TRUE(r.confirmed);
	EXPECT_GT(r.got.size(), 900u);
	EXPECT_EQ(r.wrong, 0) << r.st.failed << " of " << r.st.words << " failed";
	EXPECT_EQ(r.st.failed, 0);
	printf("AMR 12.2 N200 DQPSK: %ld octets corrected, %ld marked, of %ld words\n",
	       (long)r.st.corrected, (long)r.st.erasures, (long)r.st.words);
	/* N300 with 8-DPSK at rate 3/4 */
	r = call(comb(1, 8000, 3, 0, 1), 8000, 8, "amr:12.2", 0);
	printf("AMR 12.2 N300 8-DPSK: %ld corrected, %ld failed of %ld words\n",
	       (long)r.st.corrected, (long)r.st.failed, (long)r.st.words);
	EXPECT_TRUE(r.confirmed);
	EXPECT_EQ(r.wrong, 0) << r.st.failed << " of " << r.st.words << " failed";
	/* P20 through AMR 7.4 at rate 7/8 */
	r = call(param(false, 2), 8000, 12, "amr:7.4", 0);
	printf("AMR 7.4 P20: %ld corrected, %ld failed of %ld words\n",
	       (long)r.st.corrected, (long)r.st.failed, (long)r.st.words);
	EXPECT_TRUE(r.confirmed);
	EXPECT_GE(r.got.size(), 28u * 4);
	EXPECT_EQ(r.wrong, 0) << r.st.failed << " of " << r.st.words << " failed";
	/* W200 through AMR-WB 23.85 */
	VcvmLoading w = comb(3, 16000, 2, 0, 1);
	for (int k = 28; k < 33; k++)
		w.load.phase[k] = 0;
	r = call(w, 16000, 5, "amrwb:23.85", 0);
	printf("AMR-WB 23.85 W200 DQPSK: %ld corrected, %ld failed of %ld words\n",
	       (long)r.st.corrected, (long)r.st.failed, (long)r.st.words);
	EXPECT_TRUE(r.confirmed);
	EXPECT_EQ(r.wrong, 0) << r.st.failed << " of " << r.st.words << " failed";
}

/* the control sequence after data: seen, none of it taken for data, and
   none seen in data alone */
TEST(VcvmData, Control)
{
	/* the control sequence after data: seen, and nothing of it taken
	   for data; none seen in 10 s of data */
	for (const VcvmLoading &l : { comb(0, 8000, 2, 0, 1), comb(2, 8000, 3, 1, 2),
				     param(false, 2), param(true, 2) }) {
		SCOPED_TRACE(l.str());
		Result r = call(l, 8000, 3, "", 0, true, 0.2);
		EXPECT_TRUE(r.control);
		EXPECT_EQ(r.wrong, 0);
		r = call(l, 8000, 10, "", 0);
		EXPECT_FALSE(r.control);
	}
}

/* silence after the data: the signal lost after a while, not at once */
TEST(VcvmData, Lost)
{
	/* silence after the data: lost in about 2 s (in 2.5 blocks, 3.2 s,
	   of P20) */
	Result r = call(comb(0, 8000, 2, 0, 1), 8000, 2, "", 0, false, 1.0);
	EXPECT_FALSE(r.lost);
	r = call(comb(0, 8000, 2, 0, 1), 8000, 2, "", 0, false, 3.0);
	EXPECT_TRUE(r.lost);
	r = call(param(false, 2), 8000, 4, "", 0, false, 4.5);
	EXPECT_TRUE(r.lost);
}

/* AMR 12.2 with 1 per cent of its frames erased: few words failed */
TEST(VcvmData, Erasures)
{
	/* a codec frame lost now and then (1 per cent): the code takes it */
#ifndef DMODEM_HAVE_AMR
	GTEST_SKIP() << "no AMR";
#else
	VcvmLoading l = comb(1, 8000, 2, 0, 1);
	Result r;
	std::mt19937 rng(3);
	VcvmDataTx tx;
	VcvmDataRx rx;
	tx.init(l, 8000, SCR_GPC, [&]() {
		uint8_t v = (uint8_t)rng();
		r.sent.push_back(v);
		return (int)v;
	});
	rx.init(l, 8000, SCR_GPC, [&](const uint8_t *d, int n) {
		r.got.insert(r.got.end(), d, d + n);
	});
	std::vector<float> x(1600, 0.0f);
	tx.start(x);
	for (int i = 0; i < 500; i++)
		tx.symbol(x);
	SpeechCodec c;
	const char *err = nullptr;
	ASSERT_EQ(c.init("amr:12.2", 1.0, 5, &err), 0);
	for (float &v : x)
		v = (float)c.sample((int)lrint(v * 0.6));
	rx.start(0);
	rx.push(x.data(), (int)x.size());
	long wrong = 0;
	for (size_t i = 0; i < r.got.size(); i++)
		wrong += r.got[i] != r.sent[i];
	printf("AMR 12.2, %ld of %ld frames erased: %ld of %ld words failed, "
	       "%ld octets corrected, %ld marked; %ld of %zu octets given "
	       "wrong\n", c.erased(), c.frames(),
	       (long)rx.stats().failed, (long)rx.stats().words,
	       (long)rx.stats().corrected, (long)rx.stats().erasures, wrong,
	       r.got.size());
	EXPECT_TRUE(rx.confirmed());
	EXPECT_LE(rx.stats().failed, rx.stats().words / 20);
#endif
}

/*
 *    two DCEs
 */

#include "vcvm_modem.h"

namespace {

/* 4 octet records: a sequence number, its check */
struct Records {
	long next = 0;                  /* the source's */
	long good = 0, bad = 0, last = -1, back = 0;
	std::vector<uint8_t> part;
	/* octet i (0..3) of the next record: the sequence number's two, its
	   check, 0xA5 */
	int src(int i)
	{
		long s = next;
		int v = i == 0 ? (int)(s >> 8 & 255) : i == 1 ? (int)(s & 255) :
			i == 2 ? (int)((s * 37 + 11) & 255) : 0xA5;
		return v;
	}
	int pos = 0;
	/* the source's next octet, the records one after another */
	int source()
	{
		int v = src(pos);
		if (++pos == 4) {
			pos = 0;
			next = (next + 1) & 0xffff;
		}
		return v;
	}
	/* n octets d received: each whole record counted good (and back, if
	   out of order) or bad */
	void sink(const uint8_t *d, int n)
	{
		part.insert(part.end(), d, d + n);
		size_t i = 0;
		for (; i + 4 <= part.size(); i += 4) {
			const uint8_t *r = &part[i];
			long s = r[0] << 8 | r[1];
			if (r[3] == 0xA5 && r[2] == ((s * 37 + 11) & 255)) {
				good++;
				if (last >= 0 && s <= last)
					back++;
				last = s;
			} else
				bad++;
		}
		part.erase(part.begin(), part.begin() + (long)i);
	}
};

struct Pair {
	VcvmModem a, b;
	Records ra, rb;                 /* a's source and b's sink, and back */
	Records rb_src, ra_sink;
};

/* a call of secs through the line; at (secs) the hook */
template <class Hook>
void run(Pair *p, int fs, const char *codec, double secs, double ppm, Hook hook,
	 VcvmCaps ca = VcvmCaps(), VcvmCaps cb = VcvmCaps(), bool timers = false)
{
	VcvmModem::Config k;
	k.su.fs = fs;
	k.su.calling = true;
	k.su.own = ca;
	k.su.peer = cb;
	k.retrain_timers = timers;
	ASSERT_EQ(p->a.init(k, [p]() { return p->ra.source(); },
			    [p](const uint8_t *d, int n) { p->ra_sink.sink(d, n); }), 0);
	k.su.calling = false;
	k.su.own = cb;
	k.su.peer = ca;
	ASSERT_EQ(p->b.init(k, [p]() { return p->rb_src.source(); },
			    [p](const uint8_t *d, int n) { p->rb.sink(d, n); }), 0);
	Path ab(fs, fs * 50 / 1000, 0.5, ppm, codec);
	Path ba(fs, fs * 50 / 1000 + 3, 0.7, -ppm, codec);
	const int B = fs / 50;
	std::vector<float> ia(B), oa(B), ib(B), ob(B);
	for (long t = 0; t < (long)(secs * fs); t += B) {
		ba.pull(ia.data(), B);
		p->a.process(ia.data(), oa.data(), B);
		bool mute = hook((double)t / fs);
		if (mute)
			std::fill(oa.begin(), oa.end(), 0.0f);
		ab.push(oa.data(), B);
		ab.pull(ib.data(), B);
		p->b.process(ib.data(), ob.data(), B);
		if (mute)
			std::fill(ob.begin(), ob.end(), 0.0f);
		ba.push(ob.data(), B);
		if (p->a.state() == VcvmModem::FAILED || p->b.state() == VcvmModem::FAILED)
			break;
	}
}

/* both DCEs' logs, the calling one's first */
std::string logs(const Pair &p)
{
	return "calling:\n" + p.a.log() + "answering:\n" + p.b.log();
}

} // namespace

/* a call on a clean line: connected, the records through both ways
   without errors or renegotiations */
TEST(VcvmModem, Clean)
{
	Pair p;
	run(&p, 8000, "", 40, 100, [](double) { return false; });
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
	/* 4000 bit/s at rate 7/8, 3500 coded, from about 18 s */
	EXPECT_GT(p.rb.good, 18 * 3500 / 32) << logs(p);
	EXPECT_GT(p.ra_sink.good, 18 * 3500 / 32);
	EXPECT_EQ(p.rb.bad + p.ra_sink.bad, 0);
	EXPECT_EQ(p.rb.back + p.ra_sink.back, 0);
	EXPECT_EQ(p.a.renegotiations() + p.b.renegotiations(), 0) << logs(p);
	printf("%s", logs(p).c_str());
}

/* a call through AMR 12.2: connected, the records through, few bad */
TEST(VcvmModem, Amr122)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	Pair p;
	run(&p, 8000, "amr:12.2", 50, 100, [](double) { return false; });
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
	long coded = p.b.rx_loading().coded_bps();
	EXPECT_GE(coded, 900);
	EXPECT_GT(p.rb.good, 25L * coded / 32) << logs(p);
	EXPECT_LE(p.rb.bad + p.ra_sink.bad, (p.rb.good + p.ra_sink.good) / 100) << logs(p);
	printf("%s", logs(p).c_str());
	printf("records: %ld good, %ld bad one way, %ld good, %ld bad the other\n",
	       p.rb.good, p.rb.bad, p.ra_sink.good, p.ra_sink.bad);
}

/* a renegotiation the calling DCE asks for, 25 s into the call: one on
   each side, and the data on after it */
TEST(VcvmModem, Renegotiate)
{
	Pair p;
	bool asked = false;
	long before = 0;
	run(&p, 8000, "", 50, 0, [&](double t) {
		if (t >= 25 && !asked) {
			asked = true;
			before = p.rb.good;
			p.a.renegotiate();
		}
		return false;
	});
	EXPECT_EQ(p.a.renegotiations(), 1) << logs(p);
	EXPECT_EQ(p.b.renegotiations(), 1) << logs(p);
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
	EXPECT_GT(p.rb.good, before + 15 * 3500 / 32);
	EXPECT_EQ(p.rb.bad + p.ra_sink.bad, 0);
}

/* a retrain the answering DCE asks for, 25 s into the call: one on each
   side, both connected after it */
TEST(VcvmModem, Retrain)
{
	Pair p;
	bool asked = false;
	run(&p, 8000, "", 70, 0, [&](double t) {
		if (t >= 25 && !asked) {
			asked = true;
			p.b.retrain();
		}
		return false;
	});
	EXPECT_EQ(p.a.retrains(), 1) << logs(p);
	EXPECT_EQ(p.b.retrains(), 1) << logs(p);
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
}

/* 3 s of nothing either way: a renegotiation or retrain, connected
   again */
TEST(VcvmModem, LineCut)
{
	/* 3 s of nothing either way: both see codewords fail and
	   renegotiate (or, the signal lost, retrain), and connect again */
	Pair p;
	run(&p, 8000, "", 75, 0, [&](double t) { return t >= 25 && t < 28; });
	EXPECT_GE(p.a.renegotiations() + p.a.retrains(), 1) << logs(p);
	EXPECT_GE(p.b.renegotiations() + p.b.retrains(), 1) << logs(p);
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
	long before = p.rb.good;
	(void)before;
	EXPECT_EQ(p.rb.back + p.ra_sink.back, 0);
}

/* a call through AMR 7.4: connected, the parametric mode's retrain
   (if it is the mode), the records through */
TEST(VcvmModem, Parametric)
{
	/* AMR 7.4: the parametric mode (or four tones of N300); its retrain
	   30 s into phase 4 (c-bar over 0.85), and phase 4 again */
	if (!have_codec())
		GTEST_SKIP() << "no AMR";
	Pair p;
	run(&p, 8000, "amr:7.4", 85, 0, [](double) { return false; }, VcvmCaps(),
	    VcvmCaps(), true);
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
	bool param = p.a.rx_loading().kind == VcvmLoading::PARAM ||
		p.b.rx_loading().kind == VcvmLoading::PARAM;
	if (param) {
		EXPECT_GE(p.a.retrains(), 1) << logs(p);
	}
	EXPECT_GT(p.rb.good + p.ra_sink.good, 20L * 175 / 32) << logs(p);
	EXPECT_LE(p.rb.bad + p.ra_sink.bad, 2) << logs(p);
	printf("%s", logs(p).c_str());
}

/* a call through AMR-WB at 8000 Hz: connected without retrains */
TEST(VcvmModem, AmrWbNarrow)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR-WB";
	/* AMR-WB 23.85 with the signal interface at 8000 Hz (the interop
	   call's line): the comb narrowband, 8-DPSK and rings offered */
	Pair p;
	VcvmCaps c = vcvm_caps_for(VCVM_M_ALL, 8000);
	run(&p, 8000, "amrwb:23.85", 60, 100, [](double) { return false; }, c, c);
	EXPECT_TRUE(p.a.connected()) << logs(p);
	EXPECT_TRUE(p.b.connected()) << logs(p);
	EXPECT_EQ(p.a.retrains() + p.b.retrains(), 0) << logs(p);
	printf("%s", logs(p).c_str());
	printf("records: %ld good, %ld bad one way, %ld good, %ld bad the other\n",
	       p.rb.good, p.rb.bad, p.ra_sink.good, p.ra_sink.bad);
}
