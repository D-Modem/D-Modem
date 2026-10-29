/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vcvm_test.cpp - V.cvm's signal set (dsp/vcvm.h): the combs and the
 *                    parametric mode back to back, through a delay, a
 *                    gain and a clock difference of 200 ppm, and through
 *                    the AMR and AMR-WB codecs of codec_sim (where the
 *                    libraries are), against what docs/v_cvm.pdf's
 *                    Appendix I measured
 *
 */

#include <math.h>

#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "vcvm.h"
#ifdef DMODEM_HAVE_AMR
#include "codec_sim.h"
#endif

using namespace dsp;

namespace {

/* the line: a delay, a gain, the far clock ppm off (sinc
   interpolation), and a codec ("" none) */
std::vector<float> line(const std::vector<float> &x, int fs, int delay,
			double gain, double ppm, const char *codec)
{
	std::vector<float> y(delay, 0.0f);
	double r = 1 + ppm * 1e-6;
	const int H = 16;
	for (double p = 0; p < x.size() - H - 1; p += r) {
		long i = (long)floor(p);
		double f = p - i, v = 0;
		for (int k = -H + 1; k <= H; k++) {
			long j = i + k;
			if (j < 0 || j >= (long)x.size())
				continue;
			double t = k - f;
			double s = fabs(t) < 1e-9 ? 1 : sin(M_PI * t) / (M_PI * t);
			double wn = 0.5 + 0.5 * cos(M_PI * t / H);
			v += x[(size_t)j] * s * wn;
		}
		y.push_back((float)(v * gain));
	}
	if (codec && *codec) {
#ifdef DMODEM_HAVE_AMR
		SpeechCodec c;
		const char *err = nullptr;
		EXPECT_EQ(c.init(codec, 0, 1, &err, fs == 16000 ? 16000 : 8000), 0)
			<< (err ? err : "");
		for (float &v : y) {
			int s = (int)lrint(v);
			s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
			v = (float)c.sample(s);
		}
#else
		(void)fs;
#endif
	}
	return y;
}

/* whether the speech codecs are built in (DMODEM_HAVE_AMR) */
bool have_codec()
{
#ifdef DMODEM_HAVE_AMR
	return true;
#else
	return false;
#endif
}

struct CombResult {
	long bits = 0, errs = 0;        /* data bits and those wrong */
	long syms = 0, symerr = 0;      /* tone symbols */
};

/* the reference and n data symbols of random bits on comb `code` with
   loading l, through the line; what the receiver decided against
   what was sent */
CombResult comb_call(int code, int fs, const VcvmLoad &l, int n, double ppm,
		     const char *codec = "", bool sym22 = false)
{
	VcvmCombTx tx;
	VcvmCombRx rx;
	EXPECT_EQ(tx.init(code, fs, sym22), 0);
	EXPECT_EQ(rx.init(code, fs, sym22), 0);
	tx.set_load(l);
	rx.set_load(l);
	const int sps = tx.sps(), nb = tx.bits();
	std::mt19937 rng(7);
	std::vector<std::vector<uint8_t>> sent;
	std::vector<float> x(fs / 4, 0.0f), s(sps);
	tx.reference(s.data());
	x.insert(x.end(), s.begin(), s.end());
	for (int i = 0; i < n; i++) {
		std::vector<uint8_t> b(nb);
		for (auto &v : b)
			v = rng() & 1;
		tx.data(b.data(), s.data());
		x.insert(x.end(), s.begin(), s.end());
		sent.push_back(b);
	}
	x.insert(x.end(), fs / 4, 0.0f);
	std::vector<float> y = line(x, fs, 37, 0.5, ppm, codec);
	rx.push(y.data(), (int)y.size());
	long on = rx.onset(0);
	EXPECT_GE(on, 0);
	EXPECT_EQ(rx.acquire(on, on), 0);
	CombResult r;
	VcvmCombSym sym;
	EXPECT_TRUE(rx.next(&sym));             /* the reference */
	/* the codec's first symbols aside (its start) */
	int skip = codec && *codec ? 3 : 0;
	for (int i = 0; i < n; i++) {
		if (!rx.next(&sym)) {
			ADD_FAILURE() << "symbol " << i << " missing";
			break;
		}
		if (i < skip)
			continue;
		EXPECT_EQ(sym.nbits, nb);
		int bi = 0;
		for (int k = 0; k < tx.comb().tones; k++) {
			int w = l.phase[k] + l.ring[k];
			if (!l.phase[k])
				continue;
			bool bad = false;
			for (int j = 0; j < w; j++, bi++)
				if (sym.bits[bi] != sent[(size_t)i][(size_t)bi]) {
					bad = true;
					r.errs++;
				}
			r.symerr += bad;
			r.syms++;
		}
		r.bits += nb;
	}
	return r;
}

/* a load of tones tones, each pb bits of phase and rb of ring */
VcvmLoad load_of(int tones, int pb, int rb = 0)
{
	VcvmLoad l;
	l.all(tones, pb);
	for (int k = 0; k < tones; k++)
		l.ring[k] = (uint8_t)rb;
	return l;
}

} // namespace

/* each narrowband comb, each order, with and without the rings, through
   a delay and a gain: no bit wrong */
TEST(VcvmComb, CleanNarrowband)
{
	for (int code = 0; code < 3; code++) {
		VcvmComb c;
		ASSERT_EQ(c.init(code, 8000), 0);
		for (auto [pb, rb] : std::vector<std::pair<int, int>>{
			     { 2, 0 }, { 3, 0 }, { 3, 1 }, { 3, 2 }, { 2, 1 } }) {
			CombResult r = comb_call(code, 8000, load_of(c.tones, pb, rb),
						 100, 0);
			EXPECT_EQ(r.errs, 0) << "code " << code << " phase bits "
					     << pb << " ring bits " << rb;
			EXPECT_GT(r.bits, 0);
		}
	}
}

/* the far clock 200 ppm fast and slow (a sample every 31 symbols of
   N200): the receiver follows it */
TEST(VcvmComb, ClockDifference)
{
	for (double ppm : { 200.0, -200.0 }) {
		CombResult r = comb_call(0, 8000, load_of(16, 3, 1), 400, ppm);
		EXPECT_EQ(r.errs, 0) << ppm << " ppm";
		r = comb_call(1, 8000, load_of(11, 3, 0), 400, ppm);
		EXPECT_EQ(r.errs, 0) << ppm << " ppm, N300";
	}
}

/* the wideband and super wideband combs, and N200's 22.5 ms symbol */
TEST(VcvmComb, WideCombs)
{
	for (int code = 3; code < 9; code++) {
		VcvmComb c;
		int fs = code < 6 ? 16000 : 32000;
		ASSERT_EQ(c.init(code, fs), 0);
		CombResult r = comb_call(code, fs, load_of(c.tones, 3, 2), 60, 100);
		EXPECT_EQ(r.errs, 0) << "code " << code;
	}
	CombResult r = comb_call(0, 8000, load_of(16, 3, 1), 100, 0, "", true);
	EXPECT_EQ(r.errs, 0) << "22.5 ms";
}

/* the combs go with their rates only */
TEST(VcvmComb, Codes)
{
	VcvmComb c;
	EXPECT_EQ(c.init(0, 8000), 0);
	EXPECT_EQ(c.tones, 16);
	EXPECT_EQ(c.sps, 160);
	EXPECT_EQ(c.init(1, 8000), 0);
	EXPECT_EQ(c.tones, 11);
	EXPECT_EQ(c.tu, 80);
	EXPECT_EQ(c.init(6, 32000), 0);
	EXPECT_EQ(c.tones, 68);
	EXPECT_EQ(c.init(0, 8000, true), 0);
	EXPECT_EQ(c.sps, 180);
	EXPECT_NE(c.init(3, 8000), 0);
	EXPECT_NE(c.init(1, 8000, true), 0);
	EXPECT_NE(c.init(9, 8000), 0);
	VcvmLoad l = load_of(16, 3, 1);
	EXPECT_EQ(l.bits(16), 64);
}

/* the control sequence (10.7): half turns on every loaded tone */
TEST(VcvmComb, ControlSequence)
{
	VcvmCombTx tx;
	VcvmCombRx rx;
	ASSERT_EQ(tx.init(2, 8000), 0);
	ASSERT_EQ(rx.init(2, 8000), 0);
	std::vector<float> x(2000, 0.0f), s(tx.sps());
	uint8_t half[VCVM_TONES], zero[VCVM_TONES] = {};
	memset(half, 4, sizeof(half));
	tx.reference(s.data());
	x.insert(x.end(), s.begin(), s.end());
	for (int i = 0; i < 16; i++) {
		tx.steps(i < 8 ? zero : half, nullptr, s.data());
		x.insert(x.end(), s.begin(), s.end());
	}
	x.insert(x.end(), 2000, 0.0f);
	rx.push(x.data(), (int)x.size());
	long on = rx.onset(0);
	ASSERT_EQ(rx.acquire(on, on), 0);
	VcvmCombSym sym;
	ASSERT_TRUE(rx.next(&sym));
	for (int i = 0; i < 16; i++) {
		ASSERT_TRUE(rx.next(&sym));
		for (int k = 0; k < 8; k++)
			EXPECT_EQ(sym.ph8[k], i < 8 ? 0 : 4) << i << " " << k;
	}
}

/* through AMR at 12.2: every narrowband comb with DQPSK, and N300 with
   8-DPSK, at a tone symbol error ratio under 2 per cent (Table I.6:
   0.000 to 0.003); through AMR 10.2, N300 with DQPSK (3e-3) */
TEST(VcvmComb, Amr)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR libraries";
	for (int code = 0; code < 3; code++) {
		VcvmComb c;
		c.init(code, 8000);
		CombResult r = comb_call(code, 8000, load_of(c.tones, 2), 150, 0,
					 "amr:12.2");
		EXPECT_LT((double)r.symerr / r.syms, 0.02) << "N code " << code;
	}
	CombResult r = comb_call(1, 8000, load_of(11, 3), 150, 0, "amr:12.2");
	EXPECT_LT((double)r.symerr / r.syms, 0.02) << "N300 8-DPSK";
	r = comb_call(1, 8000, load_of(11, 2), 150, 0, "amr:10.2");
	EXPECT_LT((double)r.symerr / r.syms, 0.02) << "N300 AMR 10.2";
	r = comb_call(1, 8000, load_of(11, 2), 300, 200, "amr:12.2");
	EXPECT_LT((double)r.symerr / r.syms, 0.02) << "N300, 200 ppm";
}

/* through AMR-WB 23.85 at 16000 Hz: W300 with DQPSK on its lower 16
   tones (Table I.7 loads 16 to 20 of them) */
TEST(VcvmComb, AmrWb)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR libraries";
	VcvmLoad l = load_of(16, 2);
	CombResult r = comb_call(4, 16000, l, 150, 0, "amrwb:23.85");
	EXPECT_LT((double)r.symerr / r.syms, 0.03);
}

namespace {

struct ParamResult {
	long syms = 0, errs = 0;
	float cbar = 0;
	bool locked = false;
};

/* n random symbols of P10 or P20 at fs after the preamble, through a
   line with codec and the far clock ppm off: whether the receiver
   locked, the symbols it gave, those wrong and its c-bar */
ParamResult param_call(int fs, bool p10, int n, double ppm,
		       const char *codec = "")
{
	VcvmParamTx tx;
	VcvmParamRx rx;
	EXPECT_EQ(tx.init(fs, p10), 0);
	EXPECT_EQ(rx.init(fs, p10), 0);
	const int T = tx.sps();
	std::mt19937 rng(11);
	std::vector<int> sent;
	std::vector<float> x(fs / 5, 0.0f), s(8 * T);
	tx.preamble(s.data());
	x.insert(x.end(), s.begin(), s.end());
	for (int i = 0; i < n; i++) {
		int idx = rng() % 16;
		tx.symbol(idx, s.data());
		x.insert(x.end(), s.begin(), s.begin() + T);
		sent.push_back(idx);
	}
	x.insert(x.end(), fs / 4, 0.0f);
	std::vector<float> y = line(x, fs, 53, 0.7, ppm, codec);
	rx.push(y.data(), (int)y.size());
	ParamResult r;
	r.locked = rx.search();
	if (!r.locked)
		return r;
	for (int i = 0; i < n; i++) {
		s16 d;
		if (!rx.next(&d))
			break;
		r.syms++;
		r.errs += d != sent[(size_t)i];
	}
	r.cbar = rx.cbar();
	return r;
}

} // namespace

/* P20 and P10 at 8000 and 16000 Hz, with the far clock 200 ppm off: no
   symbol wrong */
TEST(VcvmParam, Clean)
{
	for (int fs : { 8000, 16000 })
		for (bool p10 : { false, true })
			for (double ppm : { 0.0, 200.0, -200.0 }) {
				ParamResult r = param_call(fs, p10, 200, ppm);
				ASSERT_TRUE(r.locked) << fs << (p10 ? " P10" : " P20");
				EXPECT_EQ(r.syms, 200);
				EXPECT_EQ(r.errs, 0) << fs << (p10 ? " P10 " : " P20 ")
						     << ppm << " ppm";
				EXPECT_GT(r.cbar, 0.9) << fs << (p10 ? " P10 " : " P20 ") << ppm;
			}
}

/* the Gray code of the lags (Table 10-3) */
TEST(VcvmParam, Bits)
{
	static const char *want[16] = {
		"0000", "0001", "0011", "0010", "0110", "0111", "0101", "0100",
		"1100", "1101", "1111", "1110", "1010", "1011", "1001", "1000" };
	for (int i = 0; i < 16; i++) {
		uint8_t b[4];
		VcvmParamRx::bits(i, b);
		std::string s;
		for (int j = 0; j < 4; j++)
			s += (char)('0' + b[j]);
		EXPECT_EQ(s, want[i]) << i;
	}
}

/* through the codecs (Table I.9: P20 without a symbol in error through
   AMR 7.4 and AMR-WB 6.6, P10 through AMR 12.2); c-bar above 0.9
   through AMR 12.2 and below 0.85 through AMR 4.75 (Table I.10) */
TEST(VcvmParam, Codecs)
{
	if (!have_codec())
		GTEST_SKIP() << "no AMR libraries";
	ParamResult r = param_call(8000, false, 150, 0, "amr:7.4");
	ASSERT_TRUE(r.locked);
	EXPECT_EQ(r.errs, 0) << "P20 AMR 7.4";
	r = param_call(8000, false, 150, 0, "amrwb:6.6");
	ASSERT_TRUE(r.locked);
	EXPECT_EQ(r.errs, 0) << "P20 AMR-WB 6.6";
	r = param_call(8000, true, 200, 0, "amr:12.2");
	ASSERT_TRUE(r.locked);
	EXPECT_EQ(r.errs, 0) << "P10 AMR 12.2";
	r = param_call(8000, false, 150, 0, "amr:12.2");
	EXPECT_GT(r.cbar, 0.9) << "AMR 12.2";
	r = param_call(8000, false, 150, 0, "amr:4.75");
	EXPECT_LT(r.cbar, 0.85) << "AMR 4.75";
}
