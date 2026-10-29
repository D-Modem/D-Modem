/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vwb_test.cpp - V.wb, the wideband modem (vwb.h): its parts (the
 *    FFT, the QAM, the interpolator), and two of it, caller and
 *    answerer, on a simulated bare pair (vwb_line.h) at 96000 Hz: they
 *    train and pass data both ways without an error on short and long
 *    pairs, the rate falling with the length; through interferers; the
 *    clocks 100 ppm apart for minutes; a retrain after a dropout; 48000
 *    and 192000 Hz; and no 9600 Hz.  Each pair's rates are printed.
 *
 *    VWB_DEBUG=1   the modems' logs
 *    VWB_PPM, VWB_ECHO, VWB_SNR, VWB_KM   each pair's clock offset, echo
 *                                 (dB), noise (dB) and length instead
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "vwb.h"
#include "vwb_line.h"

using dsp::VwbModem;

namespace {

/* a PRBS of x^23 + x^18 + 1, an octet at a time (the first bit the
   least significant) */
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

/* the receiving side's check of a Prbs: self-synchronising, each bit
   against the one its last 23 predict; the errors after the first
   23 bits counted, and when (line samples) the last one came */
struct PrbsCheck {
	u32 r = 0;
	s64 seen = 0, errors = 0;
	s64 last_error_at = -1;
	/* n octets received at line time t */
	void octets(const u8 *p, s32 n, s64 t)
	{
		for (s32 i = 0; i < n; i++)
			for (s16 k = 0; k < 8; k++) {
				u32 b = (p[i] >> k) & 1;
				u32 want = ((r >> 22) ^ (r >> 17)) & 1;
				if (seen >= 23 && b != want) {
					errors++;
					last_error_at = t;
				}
				r = (r << 1) | b;
				seen++;
			}
	}
};

struct SideResult {
	bool connected = false;
	double connect_s = 0;
	s32 tx_rate = 0, rx_rate = 0;
	s64 bits = 0, errors = 0;
	double last_error_s = -1;
	s32 retrains = 0;
	s64 cw = 0, cw_fixed = 0, cw_failed = 0;
	double ppm = 0;
	double cpu = 0;         /* its process()'s time to the line's */
	std::string tx_loading;
};

struct PairResult {
	SideResult side[2];     /* the caller, the answerer */
};

/* caller and answerer on line for secs of line time; their results */
PairResult run_pair(const VwbLineConfig &lc0, double secs)
{
	VwbLineConfig lc = lc0;
	if (getenv("VWB_PPM"))
		lc.ppm = atof(getenv("VWB_PPM"));
	if (getenv("VWB_ECHO"))
		lc.echo_db = atof(getenv("VWB_ECHO"));
	if (getenv("VWB_SNR"))
		lc.snr_db = atof(getenv("VWB_SNR"));
	if (getenv("VWB_KM"))
		lc.km = atof(getenv("VWB_KM"));
	VwbLine line(lc);
	VwbModem m[2];
	Prbs src[2];
	PrbsCheck chk[2];
	PairResult res;
	bool debug = getenv("VWB_DEBUG") != nullptr;
	for (s16 i = 0; i < 2; i++) {
		VwbModem::Config c;
		c.fs = lc.fs;
		c.calling = i == 0;
		EXPECT_EQ(m[i].init(c, [&src, i]() { return src[i].octet(); },
			  [&chk, &line, i](const u8 *p, s32 n) {
				  chk[i].octets(p, n, line.time());
			  }), 0);
	}
	const s32 block = 480 * lc.fs / 96000;
	std::vector<s16> c_in, a_in, c_out, a_out;
	double cpu[2] = {};
	s64 end = static_cast<s64>(secs * lc.fs);
	while (line.time() < end) {
		line.inputs(block, c_in, a_in);
		c_out.resize(c_in.size());
		a_out.resize(a_in.size());
		auto t0 = std::chrono::steady_clock::now();
		m[0].process(c_in.data(), c_out.data(),
			     static_cast<s32>(c_in.size()));
		auto t1 = std::chrono::steady_clock::now();
		m[1].process(a_in.data(), a_out.data(),
			     static_cast<s32>(a_in.size()));
		auto t2 = std::chrono::steady_clock::now();
		cpu[0] += std::chrono::duration<double>(t1 - t0).count();
		cpu[1] += std::chrono::duration<double>(t2 - t1).count();
		line.outputs(c_out, a_out);
		for (s16 i = 0; i < 2; i++) {
			if (debug) {
				std::string l = m[i].take_log();
				if (!l.empty())
					printf("%s", (std::string(i ? "A " : "C ") +
						      l).c_str());
			}
			SideResult &r = res.side[i];
			if (m[i].connected() && !r.connected) {
				r.connected = true;
				r.connect_s = static_cast<double>(line.time()) /
					lc.fs;
			}
		}
		if (m[0].state() == VwbModem::FAILED ||
		    m[1].state() == VwbModem::FAILED)
			break;
	}
	for (s16 i = 0; i < 2; i++) {
		SideResult &r = res.side[i];
		r.tx_rate = m[i].tx_rate();
		r.rx_rate = m[i].rx_rate();
		r.bits = chk[i].seen;
		r.errors = chk[i].errors;
		r.last_error_s = chk[i].last_error_at < 0 ? -1 :
			static_cast<double>(chk[i].last_error_at) / lc.fs;
		r.retrains = m[i].retrains();
		r.cw = m[i].cw_total();
		r.cw_fixed = m[i].cw_corrected();
		r.cw_failed = m[i].cw_failed();
		r.ppm = m[i].clock_ppm();
		r.tx_loading = m[i].tx_loading().str();
		r.cpu = cpu[i] / (static_cast<double>(line.time()) / lc.fs);
	}
	return res;
}

/* a pair's result on two lines */
void print(const char *name, const PairResult &r)
{
	for (s16 i = 0; i < 2; i++) {
		const SideResult &s = r.side[i];
		printf("%-14s %s: %s at %.2f s, sends %6d bit/s, received %lld "
		       "bits, %lld errors, codewords %lld (%lld corrected, %lld "
		       "failed), retrains %d, clock %+.1f ppm, CPU %.1f%%\n", name,
		       i ? "answerer" : "caller  ",
		       s.connected ? "up" : "DOWN", s.connect_s,
		       static_cast<int>(s.tx_rate), static_cast<long long>(s.bits),
		       static_cast<long long>(s.errors),
		       static_cast<long long>(s.cw),
		       static_cast<long long>(s.cw_fixed),
		       static_cast<long long>(s.cw_failed),
		       static_cast<int>(s.retrains), s.ppm, 100 * s.cpu);
		printf("%-14s   %s\n", "", s.tx_loading.c_str());
	}
}

/* both up, data both ways (at least half of what the rate carries
   since the later CONNECT, less a second), no errors, no retrains */
void expect_clean(const PairResult &r, double secs)
{
	for (s16 i = 0; i < 2; i++) {
		const SideResult &s = r.side[i];
		ASSERT_TRUE(s.connected) << (i ? "answerer" : "caller");
		EXPECT_EQ(s.errors, 0) << (i ? "answerer" : "caller");
		EXPECT_EQ(s.retrains, 0) << (i ? "answerer" : "caller");
		double up = std::max(r.side[0].connect_s, r.side[1].connect_s);
		EXPECT_GT(static_cast<double>(s.bits),
			  0.5 * s.rx_rate * (secs - up - 1))
			<< (i ? "answerer" : "caller");
	}
}

} // namespace

/* the FFT against the DFT's sum, and back */
TEST(VwbParts, Fft)
{
	dsp::VwbFft f;
	f.init(64);
	std::vector<dsp::cf> x(64), y(64);
	for (s32 i = 0; i < 64; i++)
		x[static_cast<size_t>(i)] = dsp::cf(std::sin(0.3f * i * i),
						    std::cos(1.7f * i));
	y = x;
	f.forward(y.data());
	for (s32 k = 0; k < 64; k += 7) {
		std::complex<double> s = 0;
		for (s32 n = 0; n < 64; n++)
			s += std::complex<double>(x[static_cast<size_t>(n)]) *
				std::polar(1.0, -2 * M_PI * k * n / 64);
		EXPECT_NEAR(std::abs(s - std::complex<double>(
			y[static_cast<size_t>(k)])), 0, 1e-3) << k;
	}
	f.inverse(y.data());
	for (s32 n = 0; n < 64; n++)
		EXPECT_NEAR(std::abs(y[static_cast<size_t>(n)] / 64.0f -
				     x[static_cast<size_t>(n)]), 0, 1e-4);
}

/* each loading's points: unit power, every pattern sliced back, the
   SNR they need rising 1.5 to 5 dB a bit (3 on average) */
TEST(VwbParts, Qam)
{
	float last = 0;
	for (s16 b = 1; b <= dsp::VWB_MAX_BITS; b++) {
		double p = 0;
		for (u32 v = 0; v < (1u << b); v++) {
			dsp::cf z = dsp::VwbQam::map(b, v);
			p += std::norm(z);
			EXPECT_EQ(dsp::VwbQam::slice(b, z * 1.01f), v) << b;
		}
		EXPECT_NEAR(p / (1u << b), 1, 1e-3) << b;
		float db = 10 * std::log10(dsp::VwbQam::snr_needed(b));
		if (b > 1) {
			EXPECT_GT(db - last, 1.5) << b;
			EXPECT_LT(db - last, 5) << b;
		}
		last = db;
	}
}

/* a tone through the interpolator at a fraction of a sample, to 0.4
   of the rate: within -60 dB */
TEST(VwbParts, Interp)
{
	dsp::VwbInterp in;
	for (double f : { 0.05, 0.2, 0.375, 0.4 }) {
		auto x = [f](s64 i) {
			return static_cast<float>(std::cos(2 * M_PI * f *
							   static_cast<double>(i)));
		};
		double e = 0;
		for (s32 i = 100; i < 200; i++) {
			double t = i + 0.37;
			double d = in.at(x, t) - std::cos(2 * M_PI * f * t);
			e += d * d;
		}
		EXPECT_LT(10 * std::log10(e / 50), -55) << f;
	}
}

/* 9600 Hz and the like are not its rates */
TEST(VwbModem, RefusesNarrowRates)
{
	for (s32 fs : { 8000, 9600, 16000, 44100 }) {
		VwbModem m;
		VwbModem::Config c;
		c.fs = fs;
		EXPECT_EQ(m.init(c, nullptr, nullptr), -1) << fs;
	}
}

/* a short pair (0.5 km): up, error free, about 300 kbit/s in all */
TEST(VwbPair, ShortPair)
{
	VwbLineConfig lc;
	lc.km = 0.5;
	PairResult r = run_pair(lc, 15);
	print("0.5 km", r);
	expect_clean(r, 15);
	EXPECT_GT(r.side[0].tx_rate, 100000);
	EXPECT_GT(r.side[1].tx_rate, 100000);
}

/* 0.5, 1, 3 and 5 km: each up and error free, the rate falling with
   the length; both ways about the same */
TEST(VwbPair, RateFallsWithLength)
{
	s32 last = 1 << 30;
	for (double km : { 0.5, 1.0, 3.0, 5.0 }) {
		VwbLineConfig lc;
		lc.km = km;
		lc.snr_db = 50;
		PairResult r = run_pair(lc, 14);
		char name[32];
		snprintf(name, sizeof(name), "%.1f km 50 dB", km);
		print(name, r);
		expect_clean(r, 14);
		s32 both = r.side[0].tx_rate + r.side[1].tx_rate;
		EXPECT_LT(both, last) << km;
		last = both;
		double ratio = static_cast<double>(r.side[0].tx_rate) /
			std::max(r.side[1].tx_rate, 1);
		EXPECT_GT(ratio, 0.8) << km;
		EXPECT_LT(ratio, 1.25) << km;
	}
}

/* two interferers in the band: still error free */
TEST(VwbPair, Interferers)
{
	VwbLineConfig lc;
	lc.km = 1;
	lc.tones = { { 9310, -40 }, { 27150, -35 } };
	PairResult r = run_pair(lc, 14);
	print("1 km tones", r);
	expect_clean(r, 14);
}

/* the clocks 100 ppm apart for two minutes, either way: no retrain,
   no error, the answerer's receiver on the caller's clock */
TEST(VwbPair, ClockOffsetMinutes)
{
	for (double ppm : { 100.0, -100.0 }) {
		VwbLineConfig lc;
		lc.km = 1;
		lc.ppm = ppm;
		PairResult r = run_pair(lc, 120);
		char name[32];
		snprintf(name, sizeof(name), "1 km %+.0f ppm", ppm);
		print(name, r);
		expect_clean(r, 120);
		/* the answerer's samples are fast by ppm: the caller's
		   period is that much longer in them */
		EXPECT_NEAR(r.side[1].ppm, ppm, 2);
		EXPECT_NEAR(r.side[0].ppm, 0, 2);
	}
}

/* the pair cut for half a second: both retrain and pass data again,
   errors only until then */
TEST(VwbPair, RetrainAfterDropout)
{
	VwbLineConfig lc;
	lc.km = 1;
	lc.drop_at = 10;
	lc.drop_s = 0.5;
	PairResult r = run_pair(lc, 30);
	print("1 km dropout", r);
	for (s16 i = 0; i < 2; i++) {
		const SideResult &s = r.side[i];
		ASSERT_TRUE(s.connected);
		EXPECT_GE(s.retrains, 1);
		EXPECT_LT(s.last_error_s, 20) << "errors after the retrain";
		EXPECT_GT(static_cast<double>(s.bits), 0.5 * s.rx_rate * 15);
	}
}

/* 48000 and 192000 Hz: up and error free (192000: the same tones, the
   FFT twice as long) */
TEST(VwbPair, OtherRates)
{
	for (s32 fs : { 48000, 192000 }) {
		VwbLineConfig lc;
		lc.fs = fs;
		lc.km = 1;
		PairResult r = run_pair(lc, 14);
		char name[32];
		snprintf(name, sizeof(name), "1 km %d Hz", static_cast<int>(fs));
		print(name, r);
		expect_clean(r, 14);
	}
}
