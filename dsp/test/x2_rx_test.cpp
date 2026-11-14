/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    x2_rx_test.cpp - x2's client receiver (dsp/x2_rx.h): the server's
 *                     training and frames (X2Train's codes) through a
 *                     simulated line, the preamble found, the J words
 *                     decoded, the sync frames' ones, the server's
 *                     record, and the data back bit for bit
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "bits.h"
#include "dsplibs.h"
#include "g711.h"
#include "k56flex.h"
#include "v90.h"
#include "x2.h"
#include "x2_rx.h"

using namespace dsp;

namespace {

/* DSP_DEBUG=<level>: the DSP's log to stderr */
const int debug_level = [] {
	const char *e = getenv("DSP_DEBUG");
	dsplibs_debug_level = e ? static_cast<unsigned>(atoi(e)) : 0;
	return 0;
}();

/* the line: a short channel, a gain, white noise snr dB below the
   signal; at 9600 Hz a resampler (windowed sinc) from the network's
   8000, frac of a sample late */
struct Line {
	int rate;
	double gain, noise, frac;
	std::vector<double> h;
	std::vector<double> in;         /* 8000 Hz, after h */
	double t;                       /* the next output's time (8 kHz) */
	std::mt19937 rng{ 7 };
	std::normal_distribution<double> nd{ 0, 1 };

	/* at rate r, gain g, noise snr_db below, frac f late */
	Line(int r, double g, double snr_db, double f)
		: rate(r), gain(g), frac(f), t(0)
	{
		h = { 0.05, 1.0, 0.25, -0.12, 0.04 };
		/* the signal's rms about 2500 (mu-law's training levels) */
		noise = 2500 * g * pow(10, -snr_db / 20);
	}
	/* one 8000 Hz value in; its outputs at rate into out */
	void put(double v, std::vector<float> &out)
	{
		in.push_back(0);
		size_t n = in.size();
		for (size_t i = 0; i < h.size() && i < n; i++)
			in[n - 1 - i] += h[i] * v;
		double step = 8000.0 / rate;
		/* the outputs whose taps are all in (16 either side) */
		while (t + frac + 16 < static_cast<double>(n) - 1 -
		       static_cast<double>(h.size())) {
			double tt = t + frac, s = 0;
			long c = static_cast<long>(floor(tt));
			for (long j = c - 15; j <= c + 16; j++) {
				if (j < 0)
					continue;
				double x = tt - j;
				double w = 0.5 + 0.5 * cos(M_PI * x / 16);
				double sn = fabs(x) < 1e-9 ? 1 :
					sin(M_PI * x) / (M_PI * x);
				s += in[j] * sn * w;
			}
			out.push_back(static_cast<float>(gain * s +
							 noise * nd(rng)));
			t += step;
		}
	}
};

/* the server's code as its value (x2_sample()'s), across a digital pad
   of pad_db (as netsim's) */
double code_value(unsigned char c, double pad_db = 0)
{
	double v = v90_ucode_linear(0, c & 0x7f);
	if (pad_db > 0)
		v = g711::ulaw2lin(g711::lin2ulaw(static_cast<s32>(
			lrint(v * pow(10, -pad_db / 20)))));
	return c & 0x80 ? v : -v;
}

struct Result {
	s32 sync_codes = 6 * 1700;
	s32 data_frames = 3000;
	bool ok_js = false, ok_sync = false, ok_rec = false,
		ok_start = false;
	long data_bits = 0, data_errors = 0;
	double snr = 0, ppm = 0;
	s16 pad = -1;
	s16 best = 0;
};

/* a server's training and data at rate through the line to an X2Rx */
Result run_line(int rate, double gain, double snr, double frac,
		double pad_db = 0, s16 down = 1)
{
	Result res;
	X2Rx *rx = new X2Rx;
	rx->init(0, static_cast<s16>(rate), x2_rate_for(1));
	s16 mode = pad_db > 4 ? 2 : pad_db > 1 ? 1 : 0;
	Line ln(rate, gain, snr, frac);
	X2Train t;
	t.init();
	t.data_rate = x2_rate_for(down, mode);
	std::vector<float> out;
	std::vector<unsigned char> sent;
	std::mt19937 rng(3);
	auto feed = [&](unsigned char c) {
		out.clear();
		ln.put(code_value(c, pad_db), out);
		for (float v : out)
			rx->line(v);
	};
	for (int i = 0; i < 800; i++)
		feed(t.next());
	t.segment(X2_PRE);
	s32 n = 0;
	while (t.seg != X2_SYNC)
		feed(t.next()), n++;
	res.ok_js = rx->ev_js == 1;
	for (s32 i = 0; i < res.sync_codes; i++)
		feed(t.next());
	res.ok_sync = rx->ev_sync;
	res.pad = rx->measure_pad();
	/* (as an MP at another rate would have it) */
	if (down != 1) {
		rx->lv_d.init(x2_rate_for(down, rx->mode), rx->mode);
	}
	res.best = rx->best_rate();
	t.go = 1;
	while (t.seg == X2_SYNC)
		feed(t.next());
	/* three records, then our E */
	while (t.rec_n < 3)
		feed(t.next());
	rx->e_sent = 1;
	t.go = 1;
	while (t.seg != X2_DATA)
		feed(t.next());
	s16 nb = static_cast<s16>(t.data_rate->B + t.data_rate->MD);
	std::vector<unsigned char> got;
	for (s32 f = 0; f < res.data_frames; f++) {
		unsigned char bits[X2_MAXBITS];
		for (s16 i = 0; i < nb; i++) {
			bits[i] = static_cast<unsigned char>(rng() & 1);
			sent.push_back(bits[i]);
		}
		for (s16 i = 0; i < X2_FRAME; i++)
			feed(t.next(bits));
		unsigned char b[X2RX_Q];
		s16 m = rx->take(b, X2RX_Q);
		got.insert(got.end(), b, b + m);
	}
	res.ok_rec = rx->rec_n >= 2 && rx->rec_w[0] == 0xf33c &&
		rx->rec_w[1] == 0x7fff && rx->rec_w[2] == 0;
	res.ok_start = rx->ev_start && rx->ev_data;
	/* (the receiver some frames behind the line) */
	size_t m = std::min(got.size(), sent.size());
	res.data_bits = static_cast<long>(m);
	for (size_t i = 0; i < m; i++)
		res.data_errors += got[i] != sent[i];
	res.snr = rx->snr_db();
	res.ppm = rx->ppm();
	delete rx;
	return res;
}

/* a clean line at 8000 Hz: every stage, the data bit for bit */
TEST(X2Rx, CleanLine8000)
{
	Result r = run_line(8000, 0.5, 60, 0.0);
	EXPECT_TRUE(r.ok_js);
	EXPECT_TRUE(r.ok_sync);
	EXPECT_TRUE(r.ok_rec);
	EXPECT_TRUE(r.ok_start);
	EXPECT_GT(r.data_bits, 2900 * 24);
	EXPECT_EQ(r.data_errors, 0);
}

/* across 3 dB and 6 dB pads: the pad found, the data in its levels */
TEST(X2Rx, Pads)
{
	for (double pad : { 3.0, 6.0 }) {
		Result r = run_line(9600, 0.4, 50, 0.21, pad);
		s16 mode = pad > 4 ? 2 : 1;
		EXPECT_EQ(r.pad, mode) << pad << " dB";
		EXPECT_TRUE(r.ok_rec) << pad << " dB";
		EXPECT_TRUE(r.ok_start) << pad << " dB";
		EXPECT_GT(r.data_bits, 2900 * 24) << pad << " dB";
		EXPECT_EQ(r.data_errors, 0) << pad << " dB";
	}
}

/* the noise sets the rate taken: with none to speak of, near the most;
   with more of it, fewer levels */
TEST(X2Rx, BestRate)
{
	s16 b70 = run_line(8000, 0.5, 70, 0).best;
	s16 b50 = run_line(9600, 0.4, 50, 0.3).best;
	s16 b40 = run_line(9600, 0.4, 40, 0.3).best;
	EXPECT_GE(b70, 13);
	EXPECT_LT(b50, b70);
	EXPECT_GT(b50, 1);
	EXPECT_LE(b40, b50);
}

/* at 9600 Hz behind the resampler, 45 dB: the same, and the SNR seen */
TEST(X2Rx, Analogue9600)
{
	Result r = run_line(9600, 0.3, 45, 0.37);
	EXPECT_TRUE(r.ok_js);
	EXPECT_TRUE(r.ok_sync);
	EXPECT_TRUE(r.ok_rec);
	EXPECT_TRUE(r.ok_start);
	EXPECT_GT(r.data_bits, 2900 * 24);
	EXPECT_EQ(r.data_errors, 0);
	EXPECT_GT(r.snr, 30);
}

} // namespace
