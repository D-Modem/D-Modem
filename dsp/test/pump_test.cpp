/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pump_test.cpp - the open data pumps back to back (loopback.h): each
 *    modulation on a clean and a noisy line, at 8000 Hz, with clock
 *    offsets, line gains, near and far echoes; and the answering automode,
 *    a caller of another modulation finding its way to it.  Both ways
 *    connected, with a bit error rate of 1e-3 at most.  V.61 also with
 *    speech: a tone on each side's microphone heard at the other.
 *
 *    DSP_DEBUG=<level>   the pumps' log
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <complex>
#include <tuple>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dsp.h"
#include "dsplibs.h"
#include "codec_sim.h"
#include "loopback.h"
#include "zyx.h"

struct Pumps {
	const char *name;
	LoopbackConfig cfg;
};

/* the answerer's pump dp (the caller's too) on a line of snr dB for
   secs of line time, and the rest of LoopbackConfig as given */
static LoopbackConfig line(int dp, float snr, float secs, int srate = 9600,
			   float gain = 0.5f, float ppm = 0, float echo_db = 0,
			   float far_db = 0, int far_delay = 0)
{
	LoopbackConfig c;
	c.dp = dp;
	c.snr = snr;
	c.secs = secs;
	c.srate = srate;
	c.gain = gain;
	c.ppm = ppm;
	c.echo_db = echo_db;
	c.far_db = far_db;
	c.far_delay = far_delay;
	return c;
}

/* c over a VoIP path: ms more delay toward the answerer, toward the
   caller (the far echo with them) */
static LoopbackConfig voip(LoopbackConfig c, int to_answerer, int to_caller)
{
	c.line_delay = to_answerer * c.srate / 1000;
	c.line_delay_c = to_caller * c.srate / 1000;
	return c;
}

/* the caller's pump, the answerer's, on a line of 30 dB */
static LoopbackConfig automode(int caller, int answerer, float secs)
{
	LoopbackConfig c = line(answerer, 30, secs);
	c.caller_dp = caller;
	return c;
}

/* (see automode()) both in automode (S31=1, +MS=PEP's default) */
static LoopbackConfig pep_auto(int caller, int answerer, float secs)
{
	LoopbackConfig c = automode(caller, answerer, secs);
	c.sregs.push_back({ SREG_AUTOMODE, 1 });
	return c;
}

class PumpLoopback : public testing::TestWithParam<Pumps> {};

/* one loopback run: no pump failed, both sides connected with a BER
   of 1e-3 at most; the rates printed */
TEST_P(PumpLoopback, DataBothWays)
{
	LoopbackResult r = loopback_run(GetParam().cfg);

	EXPECT_FALSE(r.dp_failed) << "a pump ended with an error";
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
	printf("caller at %d/%d bit/s, answerer at %d/%d bit/s\n",
	       r.side[0].tx_rate, r.side[0].rx_rate, r.side[1].tx_rate,
	       r.side[1].rx_rate);
}

/* the case's name for the test's */
static std::string name(const testing::TestParamInfo<Pumps> &i)
{
	return i.param.name;
}

/* data pump ids: 8 V.34, 32 V.32, 132 V.32bis, 232 V.32terbo, 234 ZyX, 168 HST, 61 V.61, 126 V.26 ter, 226 V.26 bis,
   22 V.22, 122 V.22bis, 212 Bell 212A, 21 V.21, 23 V.23, 103 Bell 103 */
INSTANTIATE_TEST_SUITE_P(Line, PumpLoopback, testing::Values(
	Pumps{ "v32bis", line(132, 40, 20) },
	Pumps{ "v32bis_snr25", line(132, 25, 20) },
	Pumps{ "v32bis_8000", line(132, 30, 20, 8000) },
	Pumps{ "v32bis_300ppm", line(132, 30, 30, 9600, 0.5f, 300) },
	Pumps{ "v32bis_echo6", line(132, 30, 20, 9600, 0.5f, 0, -6) },
	Pumps{ "v32bis_snr11", line(132, 11, 20) },
	Pumps{ "v32", line(32, 30, 20) },
	/* V.32terbo: 19200 bit/s on a clean line, 16800 on a noisier one
	   (V32Terbo.Rates checks the rates), on a line without its band
	   limit, at 8000 Hz, with a clock offset and with an echo */
	Pumps{ "v32terbo", line(232, 40, 20) },
	Pumps{ "v32terbo_snr27", line(232, 27, 20) },
	Pumps{ "v32terbo_flat", [] {
		LoopbackConfig c = line(232, 50, 20);
		c.flat = true;
		return c;
	}() },
	Pumps{ "v32terbo_8000", line(232, 40, 20, 8000) },
	Pumps{ "v32terbo_300ppm", line(232, 40, 30, 9600, 0.5f, 300) },
	Pumps{ "v32terbo_echo6", line(232, 40, 20, 9600, 0.5f, 0, -6) },
	/* ZyXEL's ZyX: 19200 bit/s at 2742.857 baud on a clean line,
	   the framed rates below (ZyX.Rates checks them), at 8000 Hz,
	   with a clock offset and with an echo */
	Pumps{ "zyx", line(234, 40, 20) },
	Pumps{ "zyx_snr27", line(234, 27, 20) },
	Pumps{ "zyx_8000", line(234, 40, 20, 8000) },
	Pumps{ "zyx_300ppm", line(234, 40, 30, 9600, 0.5f, 300) },
	Pumps{ "zyx_echo6", line(234, 40, 20, 9600, 0.5f, 0, -6) },
	/* V.61 without speech: data alone, 14400 bit/s down to 4800 */
	Pumps{ "v61", line(61, 40, 15) },
	Pumps{ "v61_snr15", line(61, 15, 15) },
	Pumps{ "v61_8000", line(61, 30, 15, 8000) },
	Pumps{ "v61_gain2_m300ppm", line(61, 30, 15, 9600, 2, -300) },
	Pumps{ "v61_echoes", line(61, 30, 15, 9600, 0.5f, 0, -10, -15) },
	/* V.26 ter: 2400 bit/s, its echo canceller on the near echo */
	Pumps{ "v26ter", line(126, 40, 25) },
	Pumps{ "v26ter_snr15", line(126, 15, 25) },
	Pumps{ "v26ter_8000", line(126, 30, 25, 8000) },
	Pumps{ "v26ter_gain2_m300ppm", line(126, 30, 25, 9600, 2, -300) },
	Pumps{ "v26ter_echo6", line(126, 30, 25, 9600, 0.5f, 0, -6) },
	/* HST: the forward channel at the rate the line allows, 300
	   bit/s back */
	Pumps{ "hst", line(168, 40, 20) },
	Pumps{ "hst_snr20", line(168, 20, 20) },
	Pumps{ "hst_8000", line(168, 30, 20, 8000) },
	Pumps{ "hst_gain2_m300ppm", line(168, 30, 20, 9600, 2, -300) },
	Pumps{ "hst_echo6", line(168, 30, 20, 9600, 0.5f, 0, -6) },
	Pumps{ "pep", line(255, 40, 20) },
	Pumps{ "pep_snr20", line(255, 20, 25) },
	Pumps{ "pep_8000", line(255, 30, 20, 8000) },
	Pumps{ "pep_gain2_m300ppm", line(255, 30, 20, 9600, 2, -300) },
	Pumps{ "pep_gain005_300ppm", line(255, 30, 20, 9600, 0.05f, 300) },
	Pumps{ "pep_echo6", line(255, 30, 20, 9600, 0.5f, 0, -6) },
	Pumps{ "pep_echoes", line(255, 30, 20, 9600, 0.5f, 0, -10, -15) },
	Pumps{ "v26bis", line(226, 40, 25) },
	Pumps{ "v26bis_snr15", line(226, 15, 25) },
	Pumps{ "v26bis_8000", line(226, 30, 25, 8000) },
	Pumps{ "v26bis_gain2_m300ppm", line(226, 30, 25, 9600, 2, -300) },
	Pumps{ "v26bis_echo6", line(226, 30, 25, 9600, 0.5f, 0, -6) },
	Pumps{ "v22bis", line(122, 40, 20) },
	Pumps{ "v22bis_snr15", line(122, 15, 20) },
	Pumps{ "v22bis_8000", line(122, 30, 20, 8000) },
	Pumps{ "v22bis_gain2_m300ppm", line(122, 30, 30, 9600, 2, -300) },
	Pumps{ "v22bis_gain005_300ppm", line(122, 30, 30, 9600, 0.05f, 300) },
	Pumps{ "v34", line(8, 30, 20) },
	/* only 2400 baud carries data (2400 bit/s, which no other symbol
	   rate has) */
	Pumps{ "v34_snr10", line(8, 10, 25) },
	/* through G.711 (mu-law) each way, its noise larger on the outer
	   points: non-linear encoding asked for (V34NonLinear) */
	Pumps{ "v34_ulaw", [] {
		LoopbackConfig c = line(8, 45, 25, 8000);
		c.ulaw = true;
		return c;
	}() },
	/* a line whose loss rises 7 dB over the band: L1's 3000 Hz tone
	   much weaker, pre-emphasis asked for (V34PreEmphasis) */
	Pumps{ "v34_tilt", [] {
		LoopbackConfig c = line(8, 30, 25);
		c.tilt = 0.25f;
		return c;
	}() },
	/* (the caller on the answerer's clock: loop timing) */
	Pumps{ "v34_150ppm", line(8, 30, 25, 9600, 0.5f, 150) },
	Pumps{ "v34_echoes", line(8, 40, 22, 9600, 0.5f, 0, -10, -15) },
	Pumps{ "v34_echoes_8000", line(8, 40, 22, 8000, 0.5f, 0, -10, -15) },
	/* a far echo over VoIP */
	Pumps{ "v34_far_echo_voip", line(8, 40, 22, 9600, 0.5f, 0, 0, -15, 1920) },
	/* a round trip of 0.8 s, 600 ms one way and 220 ms the other (as
	   the USB modems' tests through their exchange): phase 2 measures
	   it short (a reversal of the answerer's recovery taken for its
	   answer), and the answerer's J is still there when the caller's
	   echo canceller would train; it waits for its end */
	Pumps{ "v34_voip_asymmetric",
	       voip(line(8, 40, 25, 9600, 0.5f, 0, 0, -24), 600, 220) },
	Pumps{ "v34_voip_asymmetric_8000",
	       voip(line(8, 40, 25, 8000, 0.5f, 0, 0, -20), 600, 220) },
	Pumps{ "v34_voip_asymmetric_back",
	       voip(line(8, 40, 25, 9600, 0.5f, 0, 0, -20), 220, 600) },
	/* a round trip of 1 s and more (the USB modems' tests on a busy
	   machine): the far echo comes back only late in each side's
	   training, which is searched for until it is found and goes on
	   while the far part trains */
	Pumps{ "v34_voip_1s",
	       voip(line(8, 40, 40, 9600, 0.5f, 0, 0, -15), 450, 450) },
	Pumps{ "v34_voip_1s_8000",
	       voip(line(8, 40, 40, 8000, 0.5f, 0, 0, -20), 560, 440) },
	Pumps{ "v34_voip_1s_back",
	       voip(line(8, 40, 40, 9600, 0.5f, 0, 0, -20), 440, 560) },
	Pumps{ "v34_voip_1150ms",
	       voip(line(8, 40, 40, 8000, 0.5f, 0, 0, -20), 450, 700) },
	Pumps{ "v22", line(22, 15, 20) },
	/* its 4 points decided 45 degrees each way, not on the axes */
	Pumps{ "v22_snr7", line(22, 7, 20) },
	Pumps{ "bell212", line(212, 15, 20) },
	Pumps{ "v21", line(21, 8, 20) },
	Pumps{ "v23", line(23, 12, 30) },
	Pumps{ "bell103_300ppm", line(103, 8, 20, 9600, 0.5f, 300) },
	/* a minute at 3000 ppm: the line's clock drifts by 1700 samples,
	   more than its resampler kept at first */
	Pumps{ "v21_3000ppm_long", line(21, 20, 60, 9600, 0.5f, 3000) }), name);

/* c with point of sale fast connect: S49=s49 (bit 0 the short
   handshake, bit 1 HDLC flags in it) and S50=ans10 (the answer tone, 10
   ms) on both sides */
static LoopbackConfig fast(LoopbackConfig c, int s49, int ans10 = 0)
{
	c.sregs = { { SREG_FAST_CONNECT, s49 }, { SREG_FAST_ANS_TONE, ans10 } };
	return c;
}

struct FastCase {
	const char *name;
	LoopbackConfig cfg;
	double within;          /* s from the answerer's start, at most */
};

class FastConnect : public testing::TestWithParam<FastCase> {};

/* both sides connected within the case's time, with a bit error rate
   of 1e-3 at most */
TEST_P(FastConnect, Connects)
{
	const FastCase &fc = GetParam();
	LoopbackResult r = loopback_run(fc.cfg);

	EXPECT_FALSE(r.dp_failed) << "a pump ended with an error";
	for (int k = 0; k < 2; k++) {
		const char *who = k ? "answerer" : "caller";
		EXPECT_TRUE(r.side[k].ok()) << r.side[k].text(who);
		EXPECT_LE(r.side[k].connect_s, fc.within) << who;
	}
	printf("caller connected at %.2f s, answerer at %.2f s\n",
	       r.side[0].connect_s, r.side[1].connect_s);
}

/* (see name()) */
static std::string fast_name(const testing::TestParamInfo<FastCase> &i)
{
	return i.param.name;
}

/* V.22 (22), V.22bis (122) and Bell 212A (212) with S49: their normal
   handshakes take 2.5 to 4 s from the answer, the short one under 1 s
   (V.22bis 1.6 s); with HDLC flags for the scrambled ones; on noisy and
   slow lines, at 8000 Hz, with a 300 ppm clock offset; and a longer
   answer tone (S50=70) */
INSTANTIATE_TEST_SUITE_P(Pos, FastConnect, testing::Values(
	FastCase{ "v22", fast(line(22, 30, 10), 1), 1.0 },
	FastCase{ "v22_flags", fast(line(22, 30, 10), 3), 1.0 },
	FastCase{ "v22_snr10", fast(line(22, 10, 10), 1), 1.0 },
	FastCase{ "v22_flags_snr10", fast(line(22, 10, 10), 3), 1.0 },
	FastCase{ "v22_8000", fast(line(22, 20, 10, 8000), 3), 1.0 },
	FastCase{ "v22_gain005_300ppm",
		  fast(line(22, 20, 10, 9600, 0.05f, 300), 1), 1.0 },
	FastCase{ "v22_echo6",
		  fast(line(22, 20, 10, 9600, 0.5f, 0, -6), 3), 1.0 },
	FastCase{ "v22_ans700", fast(line(22, 30, 10), 1, 70), 1.3 },
	FastCase{ "bell212", fast(line(212, 30, 10), 1), 1.0 },
	FastCase{ "bell212_flags", fast(line(212, 15, 10), 3), 1.0 },
	FastCase{ "v22bis", fast(line(122, 30, 15), 1), 1.8 },
	FastCase{ "v22bis_snr20", fast(line(122, 20, 15), 1), 1.8 },
	FastCase{ "v22bis_8000", fast(line(122, 25, 15, 8000), 1), 1.8 },
	FastCase{ "v22bis_gain2_m300ppm",
		  fast(line(122, 25, 15, 9600, 2, -300), 1), 1.8 }),
	fast_name);

/* V.32terbo's rate follows the line: 19200 bit/s at 40 dB of SNR,
   16800 at 29 dB, V.32bis's 14400 at 25.5 dB; each without errors.
   (14400 needs an estimate from 25 to 26.5 dB: at 25.5 dB of line SNR
   the caller's estimate is about 26.0 dB with glibc's libm and some
   0.8 dB lower with the libm of Windows; at 25 dB it fell to 24.9 dB
   there, and the modems chose 12000.) */
TEST(V32Terbo, Rates)
{
	for (auto [snr, rate] : { std::make_pair(40.0f, 19200),
				  std::make_pair(29.0f, 16800),
				  std::make_pair(25.5f, 14400) }) {
		LoopbackResult r = loopback_run(line(232, snr, 20));
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			EXPECT_TRUE(r.side[k].ok()) << snr << " dB: "
						     << r.side[k].text(who);
			EXPECT_EQ(r.side[k].tx_rate, rate) << snr << " dB";
			EXPECT_EQ(r.side[k].rx_rate, rate) << snr << " dB";
			EXPECT_EQ(r.side[k].errors, 0) << snr << " dB: "
						      << r.side[k].text(who);
		}
	}
}

/* V.32bis's rate renegotiation (8/V.32bis, DSP_V32_RENEG) by the
   caller or the answerer, down to 9600 bit/s: the other modem hears the
   preamble and answers it (no retrain), both go on at 9600; the errors
   all in the one burst of the change over (the count realigned once
   after it, no errors since: SHOW_ERRORS with V32_RENEG_LOG shows
   them) */
TEST(V32bis, Renegotiation)
{
	const char *only = getenv("V32_RENEG_ONLY");
	for (const char *who : { "c", "a" }) {
		if (only && *only != *who)
			continue;
		setenv("DSP_V32_RENEG", (std::string("3000:9600") + who).c_str(),
		       1);
		setenv("DSP_DEBUG", "2", 1);
		testing::internal::CaptureStderr();
		LoopbackResult r = loopback_run(line(132, 35, 15));
		std::string log = testing::internal::GetCapturedStderr();
		if (getenv("V32_RENEG_LOG"))
			fputs(log.c_str(), stdout);
		unsetenv("DSP_V32_RENEG");
		unsetenv("DSP_DEBUG");
		dsplibs_debug_level = 0;

		EXPECT_NE(log.find("rate renegotiation 1 (ours)"),
			  std::string::npos) << who;
		EXPECT_NE(log.find("rate renegotiation 1 (remote)"),
			  std::string::npos) << who;
		EXPECT_EQ(log.find("retrain 1"), std::string::npos) << who;
		EXPECT_NE(log.find("renegotiation preamble"),
			  std::string::npos) << who;
		for (int k = 0; k < 2; k++) {
			const char *side = k ? "answerer" : "caller";
			EXPECT_TRUE(r.side[k].connected) << who << " " << side;
			EXPECT_EQ(r.side[k].tx_rate, 9600) << who << " " << side;
			EXPECT_EQ(r.side[k].rx_rate, 9600) << who << " " << side;
			EXPECT_LE(r.side[k].resyncs, 1) << who << ": "
							 << r.side[k].text(side);
			EXPECT_LT(r.side[k].errors, 2000) << who << ": "
							  << r.side[k].text(side);
			EXPECT_GT(r.side[k].checked, 60000) << who << ": "
							    << r.side[k].text(side);
		}
	}
}

/* HST's forward channel follows the line: 16800 bit/s at 40 dB of SNR,
   14400 at 27, 12000 at 23, 7200 at 17, 4800 at 12; each without
   errors, the back channel at 300 bit/s */
TEST(Hst, Rates)
{
	for (auto [snr, rate] : { std::make_pair(40.0f, 16800),
				  std::make_pair(27.0f, 14400),
				  std::make_pair(23.0f, 12000),
				  std::make_pair(17.0f, 7200),
				  std::make_pair(12.0f, 4800) }) {
		LoopbackResult r = loopback_run(line(168, snr, 20));
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			EXPECT_TRUE(r.side[k].ok()) << snr << " dB: "
						     << r.side[k].text(who);
			EXPECT_EQ(k ? r.side[k].tx_rate : r.side[k].rx_rate,
				  rate) << snr << " dB";
			EXPECT_EQ(k ? r.side[k].rx_rate : r.side[k].tx_rate,
				  300) << snr << " dB";
			EXPECT_EQ(r.side[k].errors, 0) << snr << " dB: "
						      << r.side[k].text(who);
		}
	}
}

/* V.61 with speech both ways (tones on the two microphones): 4800 bit/s
   of data and each side hearing the other's tone, on a line of snr dB;
   at 2800 symbols/s too */
static void v61_voice(float snr, int srate, const char *baud)
{
	if (baud)
		setenv("DSP_V61_BAUD", baud, 1);
	LoopbackConfig c = line(61, snr, 15, srate);
	c.voice_hz[0] = 440;
	c.voice_hz[1] = 700;
	LoopbackResult r = loopback_run(c);
	unsetenv("DSP_V61_BAUD");

	EXPECT_FALSE(r.dp_failed);
	for (int k = 0; k < 2; k++) {
		const char *who = k ? "answerer" : "caller";
		EXPECT_TRUE(r.side[k].ok()) << r.side[k].text(who);
		EXPECT_EQ(r.side[k].errors, 0) << r.side[k].text(who);
		EXPECT_EQ(r.side[k].rx_rate, 4800) << r.side[k].text(who);
		EXPECT_TRUE(r.side[k].voice_on) << r.side[k].text(who);
		EXPECT_GT(r.side[k].tone_share, 0.85) << r.side[k].text(who);
	}
}

/* (see v61_voice()) at 30 dB */
TEST(V61, VoicePlusData)
{
	v61_voice(30, 9600, nullptr);
}

/* at 20 dB: the speech's headroom keeps the data clean */
TEST(V61, VoicePlusDataSnr20)
{
	v61_voice(20, 9600, nullptr);
}

/* on a line sampled at 8000 Hz */
TEST(V61, VoicePlusData8000)
{
	v61_voice(30, 8000, nullptr);
}

/* at 2800 symbols/s (the 1680 Hz carrier, 1493.3 Hz audio carrier) */
TEST(V61, VoicePlusData2800)
{
	v61_voice(30, 9600, "2800");
}

/* V.61 with the answerer's microphone silent: it sends data alone
   (14400 bit/s here) between the caller's speech frames, so the
   caller receives far more than the 4800 bit/s the call connected at,
   and hears nothing of the far tone */
TEST(V61, SilenceSendsDataAlone)
{
	LoopbackConfig c = line(61, 40, 15);
	c.voice_hz[0] = 440;
	c.voice_hz[1] = -1;
	LoopbackResult r = loopback_run(c);

	EXPECT_FALSE(r.dp_failed);
	for (int k = 0; k < 2; k++) {
		const char *who = k ? "answerer" : "caller";
		EXPECT_TRUE(r.side[k].ok()) << r.side[k].text(who);
		EXPECT_EQ(r.side[k].errors, 0) << r.side[k].text(who);
	}
	EXPECT_GT(r.side[0].carried, 2 * r.side[1].carried)
		<< r.side[0].text("caller");
	EXPECT_LT(r.side[0].tone_share, 0.1) << r.side[0].text("caller");
	EXPECT_GT(r.side[1].tone_share, 0.85) << r.side[1].text("answerer");
}

/* V.22 through AMR 12.2 (a mobile call): the codec turns the points by
   20 degrees and more now and then, which 1200 bit/s takes without an
   error */
TEST(Loopback, V22ThroughAmr122)
{
	dsp::SpeechCodec probe;
	const char *err = "";

	if (probe.init("amr:12.2", 0, 1, &err))
		GTEST_SKIP() << "no AMR: " << err;
	LoopbackConfig c = line(22, 40, 20, 8000);
	c.codec = "amr:12.2";
	LoopbackResult r = loopback_run(c);

	EXPECT_FALSE(r.dp_failed);
	for (int k = 0; k < 2; k++) {
		const char *who = k ? "answerer" : "caller";
		EXPECT_TRUE(r.side[k].ok()) << r.side[k].text(who);
		EXPECT_EQ(r.side[k].errors, 0) << r.side[k].text(who);
	}
}

/* a call whose receivers stall halfway (nothing they receive from then
   on comes out) is no good, however right the bits before were: ok()
   wants the data the call should carry, which the BER alone does not */
TEST(Loopback, StalledCallFails)
{
	LoopbackConfig c = line(132, 40, 20);
	c.cut_s = 12;
	LoopbackResult r = loopback_run(c);

	for (const LoopbackSide &s : r.side) {
		EXPECT_TRUE(s.connected);
		EXPECT_GT(s.checked, 300);
		EXPECT_LE((double)s.errors / s.checked, 1e-3);
		EXPECT_LT(s.carried, s.expected * 0.9);
		EXPECT_FALSE(s.ok()) << s.text("side");
	}
}

/* the caller's INFO0 offering only some of the symbol rates and carriers
   (DSP_V34_INFO0=<bits 12:19>,c): the answerer keeps to them, and the
   caller retrains on an INFO1a that does not */
struct Info0Case {
	const char *name;
	const char *info0;      /* DSP_V34_INFO0 */
	int max_rate;           /* the most at the symbol rate left */
};

/* (gtest prints a case by its name) */
static void PrintTo(const Info0Case &c, std::ostream *os)
{
	*os << c.name;
}

class V34Info0 : public testing::TestWithParam<Info0Case> {};

/* connected both ways, at a rate of the symbol rate left */
TEST_P(V34Info0, AnswererKeepsToIt)
{
	setenv("DSP_V34_INFO0", GetParam().info0, 1);
	LoopbackResult r = loopback_run(line(8, 40, 25));
	unsetenv("DSP_V34_INFO0");

	EXPECT_FALSE(r.dp_failed) << "a pump ended with an error";
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
	EXPECT_LE(r.side[0].tx_rate, GetParam().max_rate);
	EXPECT_LE(r.side[0].rx_rate, GetParam().max_rate);
}

INSTANTIATE_TEST_SUITE_P(Caller, V34Info0, testing::Values(
	/* 3429 allowed but not supported, 3000 and 3200 on the high
	   carrier only: 3200 with the caller on the high carrier */
	Info0Case{ "high_carrier", "d0,c", 31200 },
	/* no 3429, the low carrier only */
	Info0Case{ "low_carrier", "a8,c", 31200 },
	/* neither carrier at 3000 or 3200, no 3429: 2400 baud */
	Info0Case{ "2400_only", "80,c", 21600 }),
	[](const testing::TestParamInfo<Info0Case> &i) {
		return std::string(i.param.name);
	});

/* phase 2's recovery (11.2.2/V.34): one side's first INFO0 lost
   (DSP_V34_LOSE_INFO0=c or a: its CRC wrong), the other repeats its
   own until that one comes again, acknowledging it */
class V34LoseInfo0 : public testing::TestWithParam<const char *> {};

/* connected both ways all the same */
TEST_P(V34LoseInfo0, PhaseTwoRecovers)
{
	setenv("DSP_V34_LOSE_INFO0", GetParam(), 1);
	LoopbackResult r = loopback_run(line(8, 30, 20));
	unsetenv("DSP_V34_LOSE_INFO0");

	EXPECT_FALSE(r.dp_failed) << "a pump ended with an error";
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
}

INSTANTIATE_TEST_SUITE_P(Lost, V34LoseInfo0, testing::Values("c", "a"),
	[](const testing::TestParamInfo<const char *> &i) {
		return std::string(*i.param == 'c' ? "callers" :
				   "answerers");
	});

/* precoding (from a gain of 0.3 dB on, DSP_V34_PRECODE_MIN) on a line
   too noisy for the training to keep a good equalizer, and a
   renegotiation (DSP_V34_RENEG): the precoder's 1 + H(z) goes into the
   equalizer once each time, never on top of the last (the log says) */
TEST(V34Precoding, RenegotiationAppliesItOnce)
{
	setenv("DSP_V34_PRECODE_MIN", "0.3", 1);
	setenv("DSP_V34_RENEG", "6000:3", 1);
	setenv("DSP_DEBUG", "2", 1);
	testing::internal::CaptureStderr();
	LoopbackResult r = loopback_run(line(8, 14, 25));
	std::string log = testing::internal::GetCapturedStderr();
	unsetenv("DSP_V34_PRECODE_MIN");
	unsetenv("DSP_V34_RENEG");
	unsetenv("DSP_DEBUG");
	dsplibs_debug_level = 0;

	size_t at = 0;
	int once = 0;
	while ((at = log.find("into the equalizer: 48 taps", at)) !=
	       std::string::npos) {
		once++;
		at++;
	}
	EXPECT_NE(log.find("S in data mode"), std::string::npos)
		<< "no renegotiation";
	EXPECT_EQ(once, 4) << "1 + H(z) from the plain equalizer";
	EXPECT_EQ(log.find("into the equalizer: 54 taps"), std::string::npos)
		<< "1 + H(z) twice";
	EXPECT_TRUE(r.side[0].connected && r.side[1].connected);
}

INSTANTIATE_TEST_SUITE_P(Automode, PumpLoopback, testing::Values(
	Pumps{ "v22bis_to_v34", automode(122, 8, 25) },
	Pumps{ "v34_to_v22", automode(8, 22, 25) },
	Pumps{ "v34_to_v21", automode(8, 21, 25) },
	Pumps{ "v34_to_v23", automode(8, 23, 25) },
	Pumps{ "v34_to_bell103", automode(8, 103, 30) },
	Pumps{ "v34_to_bell212", automode(8, 212, 25) },
	Pumps{ "v21_to_v34", automode(21, 8, 45) },
	Pumps{ "v23_to_v34", automode(23, 8, 60) },
	Pumps{ "bell103_to_v34", automode(103, 8, 45) },
	/* PEP in automode (+MS=PEP,1): an answerer that hears no PEP
	   caller goes on with V.8 and its chain; a caller that hears
	   ANSam goes to V.34, one that hears another modem's answer to
	   V.32bis's chain */
	Pumps{ "v34_to_pep", pep_auto(8, 255, 30) },
	Pumps{ "v32bis_to_pep", pep_auto(132, 255, 30) },
	Pumps{ "v22bis_to_pep", pep_auto(122, 255, 30) },
	Pumps{ "pep_to_v34", pep_auto(255, 8, 30) },
	Pumps{ "pep_to_v32bis", pep_auto(255, 132, 30) },
	Pumps{ "pep_to_v22bis", pep_auto(255, 122, 30) },
	Pumps{ "pep_to_pep", pep_auto(255, 255, 25) }), name);

/* a Telebit T2000's answer (DSP_PEP_ANS=1: 3.1 s of V.25's answer tone
   before PEP): the PEP caller in automode waits for PEP after it */
TEST(PepAnswer, AfterAnswerTone)
{
	setenv("DSP_PEP_ANS", "1", 1);
	LoopbackResult r = loopback_run(pep_auto(255, 255, 30));
	unsetenv("DSP_PEP_ANS");
	EXPECT_FALSE(r.dp_failed);
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
	EXPECT_GT(r.side[0].rx_rate, 4800);
}

#include <cmath>

#include "v34.h"

/* the pre-emphasis filters (5.4/V.34) at every symbol rate and carrier:
   the magnitude over d/e - 0.45 to d/e + 0.45 of the symbol rate within
   the templates' 1 dB of their shape (Figures 1 and 2; the level is the
   signal's own), at the power of a signal without them */
TEST(V34PreEmphasis, FiltersMatchTheTemplates)
{
	for (s16 idx = 1; idx <= V34_PE_MAX; idx++)
		for (s16 b = 0; b < dsp::V34_NBAUDS; b++)
			for (s16 high = 0; high < 2; high++) {
				float h[V34_PE_TAPS];
				double S = dsp::v34_symbol_rate(b),
				       fc = dsp::v34_carrier(b, high),
				       worst = 0, pw = 0, ref = 0, tref = 0;
				int n = 0;
				ASSERT_EQ(dsp::v34_preemph_design(idx, b, high, h),
					  0);
				auto db = [&](double f) {
					double re = 0, im = 0;
					for (int k = 0; k < V34_PE_TAPS; k++) {
						re += h[k] * cos(2 * M_PI * f * k / 9600);
						im -= h[k] * sin(2 * M_PI * f * k / 9600);
					}
					return 10 * log10(re * re + im * im);
				};
				/* (the level where the shapes are compared) */
				ref = db(fc);
				tref = dsp::v34_preemph_db(idx, fc / S);
				for (double v = fc / S - 0.45; v <= fc / S + 0.45;
				     v += 0.01) {
					/* (indices 6 to 10: their step from 0 to
					   beta has no template) */
					if (idx > 5 && v > 0.65 && v < 0.9)
						continue;
					double e = db(v * S) - ref -
						   (dsp::v34_preemph_db(idx, v) - tref);
					worst = std::max(worst, fabs(e));
				}
				EXPECT_LT(worst, 1.0) << "index " << idx
					<< " at " << S << " baud, carrier " << fc;
				for (double f = fc - S / 2; f <= fc + S / 2; f += 10) {
					pw += pow(10, db(f) / 10);
					n++;
				}
				EXPECT_NEAR(pw / n, 1, 0.02) << "index " << idx
					<< " at " << S << " baud, carrier " << fc;
			}
	float h[V34_PE_TAPS];
	EXPECT_EQ(dsp::v34_preemph_design(0, 0, 0, h), -1);
	EXPECT_EQ(dsp::v34_preemph_design(11, 0, 0, h), -1);
}

/* a line whose loss rises 7 dB over the band (v34_tilt): the receivers'
   probing finds that pre-emphasis gains and asks for it, the
   transmitters apply it, and the call carries its data */
TEST(V34PreEmphasis, TiltedLineAsksForIt)
{
	LoopbackConfig c = line(8, 30, 20);
	c.tilt = 0.25f;
	setenv("DSP_DEBUG", "2", 1);
	testing::internal::CaptureStderr();
	LoopbackResult r = loopback_run(c);
	std::string log = testing::internal::GetCapturedStderr();
	unsetenv("DSP_DEBUG");
	dsplibs_debug_level = 0;

	EXPECT_NE(log.find("gains"), std::string::npos)
		<< "the probing found no gain";
	EXPECT_NE(log.find("v34 tx: pre-emphasis filter"), std::string::npos)
		<< "no pre-emphasis";
	EXPECT_FALSE(r.dp_failed);
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
}

/* each filter, both ways, asked for whatever the line (DSP_V34_PREEMPH):
   the call connects and carries its data */
class V34PreEmphasisIndex : public testing::TestWithParam<int> {};

/* (see V34PreEmphasisIndex) */
TEST_P(V34PreEmphasisIndex, DataBothWays)
{
	setenv("DSP_V34_PREEMPH", std::to_string(GetParam()).c_str(), 1);
	LoopbackResult r = loopback_run(line(8, 35, 20));
	unsetenv("DSP_V34_PREEMPH");
	EXPECT_FALSE(r.dp_failed);
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
	printf("pre-emphasis %d: %d/%d bit/s\n", GetParam(),
	       r.side[0].tx_rate, r.side[0].rx_rate);
}

INSTANTIATE_TEST_SUITE_P(Index, V34PreEmphasisIndex,
			 testing::Values(3, 5, 8, 10));

/* non-linear encoding (9.7/V.34) asked for both ways
   (DSP_V34_NONLINEAR=1) over G.711, with precoding (from a gain of
   0.3 dB on) and without: each transmitter warps its points, each
   receiver undoes it (and 1 + H(z) after it), the data carried */
class V34NonLinear : public testing::TestWithParam<const char *> {};

/* (see V34NonLinear; the parameter: DSP_V34_PRECODE_MIN) */
TEST_P(V34NonLinear, DataBothWays)
{
	LoopbackConfig c = line(8, 45, 20, 8000);
	c.ulaw = true;
	setenv("DSP_V34_NONLINEAR", "1", 1);
	setenv("DSP_V34_PRECODE_MIN", GetParam(), 1);
	setenv("DSP_DEBUG", "2", 1);
	testing::internal::CaptureStderr();
	LoopbackResult r = loopback_run(c);
	std::string log = testing::internal::GetCapturedStderr();
	unsetenv("DSP_V34_NONLINEAR");
	unsetenv("DSP_V34_PRECODE_MIN");
	unsetenv("DSP_DEBUG");
	dsplibs_debug_level = 0;

	EXPECT_NE(log.find("non-linear tx, non-linear rx"), std::string::npos)
		<< "not asked for";
	/* precoding where asked for: its 1 + H(z) never in the equalizer */
	EXPECT_EQ(log.find("into the equalizer"), std::string::npos);
	EXPECT_FALSE(r.dp_failed);
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
	EXPECT_GE(r.side[0].rx_rate, 28800);
	EXPECT_GE(r.side[1].rx_rate, 28800);
}

INSTANTIATE_TEST_SUITE_P(Precoding, V34NonLinear,
			 testing::Values("0.3", "99"),
	[](const testing::TestParamInfo<const char *> &i) {
		return std::string(i.param[0] == '0' ? "precoded" :
				   "plain");
	});

/* the non-linear encoder and its inverse (9.7/V.34): back to the point
   to 1e-5 over the whole superconstellation's range */
TEST(V34NonLinear, InverseUndoesIt)
{
	for (float x = -3; x <= 3; x += 0.05f)
		for (float y = -3; y <= 3; y += 0.25f) {
			dsp::cplx s = { x, y };
			dsp::cplx w = dsp::v34_nonlinear(s, 0.3125f);
			dsp::cplx b = dsp::v34_nonlinear_inv(w, 0.3125f);
			EXPECT_NEAR(b.re, x, 1e-5) << x << " " << y;
			EXPECT_NEAR(b.im, y, 1e-5) << x << " " << y;
		}
}

/* the auxiliary channel (8.3/V.34): Table 9's multiplexing patterns
   from W and P, Table 8's b for each rate with its 200 bit/s */
TEST(V34Aux, PatternsAndFrames)
{
	static const u16 amp[dsp::V34_NBAUDS] = { 0x6DB, 0x56B, 0x15AB,
						  0x2AAB, 0x5555, 0x1555 };
	for (s16 baud = 0; baud < dsp::V34_NBAUDS; baud++) {
		dsp::V34Params p{};
		ASSERT_EQ(p.init(baud, 4, 1, 1), 0) << baud;
		EXPECT_EQ(p.aux, 1);
		u16 pat = 0;
		s16 c = 0;
		for (s16 i = 0; i < p.P; i++) {
			c += p.W;
			pat = static_cast<u16>(pat << 1 | (c >= p.P));
			if (c >= p.P)
				c -= p.P;
		}
		EXPECT_EQ(pat, amp[baud]) << baud;
		float S = dsp::v34_symbol_rate(baud);
		for (s16 n = 1; n <= 14; n++) {
			bool ok = dsp::v34_rates[baud][n - 1].b != 0;
			EXPECT_EQ(dsp::v34_aux_rates[baud][n - 1].b != 0,
				  ok || (baud == 0 && n == 1)) << baud << " " << n;
			if (!dsp::v34_aux_rates[baud][n - 1].b)
				continue;
			s16 b = static_cast<s16>(
				std::ceil((2400 * n + 200) * 8 / S - 1e-3));
			EXPECT_EQ(dsp::v34_aux_rates[baud][n - 1].b, b)
				<< baud << " " << n;
			dsp::V34Params q{};
			EXPECT_EQ(q.init(baud, n, 1, 1), 0) << baud << " " << n;
		}
	}
}

/* the auxiliary channel both ways (DSP_V34_AUX=2: in both MPs, a PRBS
   in it): its bits arrive without errors, the primary channel's data
   with them */
TEST(V34Aux, DataBothWays)
{
	setenv("DSP_V34_AUX", "2", 1);
	setenv("DSP_DEBUG", "2", 1);
	testing::internal::CaptureStderr();
	LoopbackResult r = loopback_run(line(8, 40, 25));
	std::string log = testing::internal::GetCapturedStderr();
	unsetenv("DSP_V34_AUX");
	unsetenv("DSP_DEBUG");
	dsplibs_debug_level = 0;

	EXPECT_FALSE(r.dp_failed);
	EXPECT_TRUE(r.side[0].ok()) << r.side[0].text("caller");
	EXPECT_TRUE(r.side[1].ok()) << r.side[1].text("answerer");
	s16 sides = 0;
	for (size_t at = 0;
	     (at = log.find("aux channel: ", at)) != std::string::npos;
	     at++) {
		int bits = 0, errs = -1;
		ASSERT_EQ(sscanf(log.c_str() + at,
				 "aux channel: %d bits in, %d errors", &bits,
				 &errs), 2);
		EXPECT_GE(bits, 500);
		EXPECT_EQ(errs, 0);
		sides++;
	}
	EXPECT_EQ(sides, 2) << "the aux channel not on both ways";
}

/* V.34's J (it repeats) and J': TRN's random bits match J's 16 now and
   then, which once took a call's TRN for J; two in a row are J */
TEST(V34J, RandomBitsAreNoJ)
{
	const u32 J_4POINT = 0x8990, J_DASH = 0x899F;
	dsp::V34Rx rx{};
	u32 r = 12345;
	for (s32 i = 0; i < 400000; i++) {
		r = r * 1103515245u + 12345u;
		rx.got_bit(static_cast<s16>((r >> 16) & 1));
	}
	EXPECT_EQ(rx.ev_j, 0);
	for (s16 k = 0; k < 2; k++)
		for (s16 i = 0; i < 16; i++)
			rx.got_bit(static_cast<s16>((J_4POINT >> i) & 1));
	EXPECT_EQ(rx.ev_j, 1);
	EXPECT_EQ(rx.j16, 0);
	EXPECT_EQ(rx.ev_jp, 0);
	for (s16 i = 0; i < 16; i++)
		rx.got_bit(static_cast<s16>((J_DASH >> i) & 1));
	EXPECT_EQ(rx.ev_jp, 1);
}

/* ZyX at each of its rates (MDMPRM_MAX_RATE), on a clean line: that
   rate both ways, no errors; and the rate follows the line: 19200 bit/s
   at 40 dB of SNR, the framed 14400 at 26, 12000 at 20 (16800's band
   is narrow, between 19200's and 14400's: not a point the line's noise
   draws the same on each platform) */
TEST(ZyX, Rates)
{
	for (int rate : { 19200, 16800, 14400, 12000, 9600, 7200 }) {
		LoopbackConfig c = line(234, 40, 15);
		c.max_rate = rate;
		LoopbackResult r = loopback_run(c);
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			EXPECT_TRUE(r.side[k].ok()) << rate << ": "
						     << r.side[k].text(who);
			EXPECT_EQ(r.side[k].tx_rate, rate);
			EXPECT_EQ(r.side[k].rx_rate, rate);
			EXPECT_EQ(r.side[k].errors, 0) << rate << ": "
						      << r.side[k].text(who);
		}
	}
	for (auto [snr, rate] : { std::make_pair(40.0f, 19200),
				  std::make_pair(26.0f, 14400),
				  std::make_pair(20.0f, 12000) }) {
		LoopbackResult r = loopback_run(line(234, snr, 15));
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			EXPECT_TRUE(r.side[k].ok()) << snr << " dB: "
						     << r.side[k].text(who);
			EXPECT_EQ(r.side[k].tx_rate, rate) << snr << " dB";
			EXPECT_EQ(r.side[k].rx_rate, rate) << snr << " dB";
			EXPECT_EQ(r.side[k].errors, 0) << snr << " dB: "
						      << r.side[k].text(who);
		}
	}
}

/* ZyX's peer marker (S115=1, SREG_ZYX_PEER): two D-Modems that both
   send it go to ZyX from V.32bis (or from ZyX's own start), 19200 bit/s
   on a clean line, no errors; with the marker at one side only, the
   other side does not send its reply, and the call stays V.32bis at
   14400 bit/s */
TEST(ZyX, PeerMarker)
{
	struct Case {
		int dp;
		int on[2];      /* S115 of the caller, the answerer */
		int rate;
	};
	for (const Case &t : { Case{ 132, { 1, 1 }, 19200 },
			       Case{ 234, { 1, 1 }, 19200 },
			       Case{ 132, { 1, 0 }, 14400 },
			       Case{ 132, { 0, 1 }, 14400 } }) {
		LoopbackConfig c = line(t.dp, 40, 15);
		for (int k = 0; k < 2; k++)
			c.side_sregs[k] = { { SREG_ZYX_PEER, t.on[k] } };
		LoopbackResult r = loopback_run(c);
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			std::string id = std::to_string(t.dp) + " S115=" +
				std::to_string(t.on[0]) + "," +
				std::to_string(t.on[1]);
			EXPECT_TRUE(r.side[k].ok()) << id << ": "
						     << r.side[k].text(who);
			EXPECT_EQ(r.side[k].tx_rate, t.rate) << id << " " << who;
			EXPECT_EQ(r.side[k].rx_rate, t.rate) << id << " " << who;
			EXPECT_EQ(r.side[k].errors, 0) << id << ": "
						      << r.side[k].text(who);
		}
	}
}

/* ZyX's frame mapper against the U-1496S firmware: its frame split
   (0x92D9E, run in an emulator) on 7 random groups gives the ring
   table's row and each symbol's M bits (Q1 Q2 first, then the uncoded
   ones); encode() must give the same, decode() the groups back */
TEST(ZyX, FrameMapper)
{
	static const struct {
		int bps;
		u8 groups[7];
		int row;
		u8 sym[8];
	} vec[] = {
	{ 16800, { 0x11, 0x48, 0x6C, 0x66, 0x61, 0x08, 0x20 }, 17, { 0x12, 0x0C, 0x1B, 0x1C, 0x10, 0x11, 0x00, 0x08 } },
	{ 16800, { 0x0F, 0x3F, 0x61, 0x39, 0x3C, 0x53, 0x30 }, 399, { 0x0F, 0x01, 0x07, 0x07, 0x1E, 0x06, 0x05, 0x0C } },
	{ 16800, { 0x64, 0x1A, 0x0C, 0x3E, 0x03, 0x72, 0x6A }, 356, { 0x06, 0x0C, 0x18, 0x17, 0x01, 0x04, 0x17, 0x1A } },
	{ 14400, { 0x18, 0x1B, 0x26, 0x30, 0x31, 0x00, 0x2C }, 728, { 0x09, 0x09, 0x00, 0x07, 0x0C, 0x00, 0x00, 0x0B } },
	{ 14400, { 0x1C, 0x11, 0x2E, 0x33, 0x0E, 0x25, 0x3C }, 92, { 0x09, 0x0B, 0x03, 0x0B, 0x03, 0x05, 0x02, 0x0F } },
	{ 14400, { 0x06, 0x39, 0x14, 0x01, 0x01, 0x01, 0x29 }, 582, { 0x03, 0x05, 0x01, 0x04, 0x00, 0x01, 0x04, 0x0A } },
	{ 12000, { 0x11, 0x00, 0x1E, 0x1C, 0x0C, 0x15, 0x06 }, 17, { 0x07, 0x01, 0x07, 0x04, 0x05, 0x02, 0x05, 0x01 } },
	{ 12000, { 0x1F, 0x0D, 0x17, 0x00, 0x10, 0x07, 0x18 }, 1471, { 0x03, 0x01, 0x00, 0x00, 0x06, 0x03, 0x00, 0x06 } },
	{ 12000, { 0x0E, 0x1E, 0x0F, 0x11, 0x07, 0x0B, 0x07 }, 1998, { 0x07, 0x02, 0x04, 0x07, 0x04, 0x05, 0x06, 0x01 } },
	{ 9600, { 0x0A, 0x03, 0x0C, 0x07, 0x0F, 0x04, 0x0E }, 3130, { 0x03, 0x01, 0x03, 0x03, 0x00, 0x01, 0x02, 0x03 } },
	{ 9600, { 0x00, 0x06, 0x0D, 0x0E, 0x08, 0x0E, 0x0A }, 3424, { 0x02, 0x03, 0x00, 0x02, 0x02, 0x03, 0x02, 0x02 } },
	{ 9600, { 0x01, 0x02, 0x0A, 0x0F, 0x0B, 0x0D, 0x04 }, 2593, { 0x03, 0x03, 0x03, 0x02, 0x01, 0x03, 0x00, 0x01 } },
	{ 7200, { 0x00, 0x05, 0x02, 0x07, 0x05, 0x07, 0x05 }, 8, { 0x01, 0x01, 0x03, 0x03, 0x02, 0x03, 0x03, 0x02 } },
	{ 7200, { 0x04, 0x07, 0x07, 0x03, 0x04, 0x06, 0x07 }, 28, { 0x03, 0x03, 0x03, 0x00, 0x02, 0x02, 0x03, 0x03 } },
	{ 7200, { 0x05, 0x01, 0x02, 0x02, 0x04, 0x07, 0x07 }, 13, { 0x00, 0x01, 0x02, 0x00, 0x02, 0x03, 0x03, 0x03 } },
	};
	for (const auto &v : vec) {
		const dsp::ZyxFrameRate &f = v.bps == 16800 ? dsp::zyx_16800 :
			v.bps == 14400 ? dsp::zyx_14400 :
			v.bps == 12000 ? dsp::zyx_12000 :
			v.bps == 9600 ? dsp::zyx_9600 : dsp::zyx_7200;
		u8 bits[ZYX_MAX_BITS], back[ZYX_MAX_BITS];
		int i = 0;
		for (int g = 0; g < ZYX_GROUPS; g++)
			for (int b = 0; b < f.n; b++)
				bits[i++] = (v.groups[g] >> b) & 1;
		s16 q[ZYX_FRAME];
		f.encode(bits, q);
		u16 rings = dsp::zyx_rings[v.row];
		for (int k = 0; k < ZYX_FRAME; k++) {
			int want = (v.sym[k] & 3) | ((rings >> (2 * k)) & 3) << 2 |
				(v.sym[k] >> 2) << 4;
			EXPECT_EQ(q[k], want) << v.bps << " symbol " << k;
		}
		EXPECT_TRUE(f.decode(q, back)) << v.bps;
		for (int j = 0; j < f.frame_bits(); j++)
			EXPECT_EQ(back[j], bits[j]) << v.bps << " bit " << j;
	}
}

/* the carrier detector of the FSK demodulator (V.21's channel 1, at 8000
   Hz) after 1 s of a mark tone at dbm0; on (true) first for 1 s at -20
   dBm0 if was_on */
static bool fsk_carrier(double dbm0, bool was_on)
{
	dsp::FskDemod d;
	d.init(980, 1180, 300, 8000);
	int i = 0;
	if (was_on)
		for (; i < 8000; i++)
			d.run(static_cast<float>(22300 * 0.1 *
				sin(2 * M_PI * 980 * i / 8000.0)));
	const double a = 22300 * pow(10, dbm0 / 20);
	for (int k = 0; k < 8000; k++, i++)
		d.run(static_cast<float>(a * sin(2 * M_PI * 980 * i / 8000.0)));
	return d.carrier != 0;
}

/* 8.3/V.21 (and 4.3/V.23): the carrier on above -43 dBm, off below -48
   dBm, with at least 2 dB between the two levels (0 dBm0 taken as 0 dBm) */
TEST(Fsk, CarrierThresholds)
{
	EXPECT_TRUE(fsk_carrier(-42.5, false));
	EXPECT_FALSE(fsk_carrier(-48.5, false));
	EXPECT_FALSE(fsk_carrier(-48.5, true));
	EXPECT_TRUE(fsk_carrier(-44.5, true));
	EXPECT_FALSE(fsk_carrier(-44.5, false));
}

/* the ms from the end of 1 s of a mark tone at dbm0 (8000 Hz) to the
   carrier detector's OFF, for the channel of mark, space and baud (the
   default: V.21's channel 1); 1000 if it stays ON */
static double fsk_off_ms(double dbm0, float mark = 980, float space = 1180,
			 float baud = 300)
{
	dsp::FskDemod d;
	d.init(mark, space, baud, 8000);
	const double a = 22300 * pow(10, dbm0 / 20);
	for (int i = 0; i < 8000; i++)
		d.run(static_cast<float>(a * sin(2 * M_PI * mark * i / 8000.0)));
	int n = 0;
	while (d.carrier && n < 8000) {
		d.run(0);
		n++;
	}
	return n / 8.0;
}

/* 8.2.2/V.21: circuit 109 ON to OFF in 20 to 80 ms, from a loud carrier
   and from a weak one */
TEST(Fsk, CarrierOffTime)
{
	for (double l : { 0.0, -10.0, -20.0, -30.0, -42.0 }) {
		EXPECT_GE(fsk_off_ms(l), 20.0) << l << " dBm0";
		EXPECT_LE(fsk_off_ms(l), 80.0) << l << " dBm0";
	}
}

/* 8.3.2/V.23: circuit 109 (the 1200 baud channel) ON to OFF in 5 to 15
   ms, circuit 122 (the 75 baud channel) in 15 to 80 ms */
TEST(Fsk, V23CarrierOffTime)
{
	for (double l : { 0.0, -10.0, -20.0, -30.0, -42.0 }) {
		const double f = fsk_off_ms(l, 1300, 2100, 1200);
		EXPECT_GE(f, 5.0) << l << " dBm0";
		EXPECT_LE(f, 15.0) << l << " dBm0";
		const double b = fsk_off_ms(l, 390, 450, 75);
		EXPECT_GE(b, 15.0) << l << " dBm0";
		EXPECT_LE(b, 80.0) << l << " dBm0";
	}
}

/* ms from the start of a mark tone at dbm0 (after 1 s of silence) to
   carrier ON, for the FSK channel mark/space at baud, at 8000 Hz; 1000
   if none in 1 s */
static double fsk_on_ms(double dbm0, float mark, float space, float baud)
{
	dsp::FskDemod d;
	d.init(mark, space, baud, 8000);
	for (int i = 0; i < 8000; i++)
		d.run(0);
	const double a = 22300 * pow(10, dbm0 / 20);
	for (int i = 0; i < 8000; i++) {
		d.run(static_cast<float>(a * sin(2 * M_PI * mark * i / 8000.0)));
		if (d.carrier)
			return i / 8.0;
	}
	return 1000;
}

/* 8.3.2/V.23: circuit 109 (the 1200 baud channel) OFF to ON in 10 ms to
   20 ms after the call set up, and circuit 122 (the 75 baud channel) in
   less than 80 ms, from 3 dB over the ON level (-43 dBm) to 0 dBm0 */
TEST(Fsk, V23CarrierOnTime)
{
	for (double l : { 0.0, -10.0, -20.0, -30.0, -39.5 }) {
		double fwd = fsk_on_ms(l, 1300, 2100, 1200);
		EXPECT_GE(fwd, 10) << l;
		EXPECT_LE(fwd, 20) << l;
		EXPECT_LT(fsk_on_ms(l, 390, 450, 75), 80) << l;
	}
}

/* the last time (s) in the samples of file that a 10 ms block goes from
   below to above amplitude 500, before limit_s (at srate); -1 if none */
static double fsk_onset(const std::string &file, double limit_s, int srate)
{
	FILE *f = fopen(file.c_str(), "rb");
	if (!f)
		return -1;
	const int blk = srate / 100;
	std::vector<s16> b(blk);
	double at = -1;
	bool was = false;
	for (long n = 0; fread(b.data(), sizeof(s16), blk, f) ==
		     static_cast<size_t>(blk); n += blk) {
		if (n >= limit_s * srate)
			break;
		bool on = false;
		for (s16 v : b)
			on = on || abs(v) > 500;
		if (on && !was)
			at = static_cast<double>(n) / srate;
		was = on;
	}
	fclose(f);
	return at;
}

/* 8.2.2/V.21 and 8.3.2/V.23: in the call set up, circuit 109 (here the
   CONNECT) comes ON 300 ms to 700 ms after the far mark starts, at each
   V.21 side and at the V.23 caller (the V.23 answerer has circuit 122);
   and the answerer before the caller, which then sends no data that the
   answerer cannot receive yet */
TEST(Fsk, CallSetupCarrierOnTime)
{
	for (int dp : { 21, 23 }) {
		LoopbackConfig c = line(dp, 40, 6);
		std::string dump = testing::TempDir() + "fsk_setup_" +
			std::to_string(dp);
		c.dump = dump.c_str();
		LoopbackResult r = loopback_run(c);
		ASSERT_TRUE(r.side[0].connected && r.side[1].connected) << dp;
		for (int k = 0; k < (dp == 21 ? 2 : 1); k++) {
			double con = r.side[k].connect_s;
			double on = fsk_onset(dump + (k ? ".a" : ".c"), con,
					      c.srate);
			ASSERT_GE(on, 0) << dp << " side " << k;
			EXPECT_GE(con - on, 0.29) << dp << " side " << k;
			EXPECT_LE(con - on, 0.71) << dp << " side " << k;
		}
		EXPECT_LE(r.side[1].connect_s, r.side[0].connect_s) << dp;
		remove((dump + ".a").c_str());
		remove((dump + ".c").c_str());
	}
}

/* V.22's circuit 109 (Circuit109 with dp_v22.cpp's settings) on the
   answerer's band pass, 1200 Hz, at 8000 Hz: a tone at dbm0 for 1 s.
   Its state at the end (from OFF, or from ON first at -20 dBm0 if
   was_on); *off_ms (if not null): the ms to OFF after the tone stops,
   at rate's OFF time */
static bool v22_109(double dbm0, bool was_on, double *off_ms = nullptr,
		    int rate = 1200)
{
	dsp::Biquad bp;
	bp.bandpass(1200, 1.0f, 8000);
	dsp::Circuit109 c;
	c.init(V22_ON_AMP, V22_OFF_AMP, 8000);
	auto run = [&](double a, int n, int &i) {
		for (int k = 0; k < n; k++, i++)
			c.run(bp.run(static_cast<float>(a * sin(2 * M_PI *
				1200 * i / 8000.0))), V22_OFF_MS(rate), false,
			      V22_BACK_MS);
	};
	int i = 0;
	if (was_on)
		run(22300 * 0.1, 8000, i);
	run(22300 * pow(10, dbm0 / 20), 8000, i);
	bool on = c.on() != 0;
	if (off_ms) {
		int n = 0;
		while (c.on() && n < 8000) {
			c.run(bp.run(0), V22_OFF_MS(rate), false, V22_BACK_MS);
			n++;
		}
		*off_ms = n / 8.0;
	}
	return on;
}

/* V.26 ter's circuit 109 in data (Circuit109 with dp_v26.cpp's
   settings) on an 1800 Hz tone at 8000 Hz: ON at -20 dBm0 for 1 s, then
   1 s at dbm0. Its state then; *off_ms (if not null): the ms to OFF
   after the tone stops; *on_ms (if not null): then, after 1 s of
   silence, the ms to ON again at dbm0 (1000: none in 1 s).  bis: V.26
   bis's (dp_v26bis.cpp: on the band of the forward channel, ON again
   after V26B_BACK_MS) */
static bool v26_109(double dbm0, double *off_ms = nullptr,
		    double *on_ms = nullptr, bool bis = false)
{
	dsp::Circuit109 c;
	c.init(V26_ON_AMP, V26_OFF_AMP, 8000, V26_SLOW_TC, V26_FAST_TC);
	c.cd.on = 1;
	dsp::Biquad bp;
	bp.bandpass(1800, 0.75f, 8000);
	int i = 0;
	auto tone = [&](double a) {
		return static_cast<float>(a * sin(2 * M_PI * 1800 * i++ /
						  8000.0));
	};
	auto step = [&](float x) {
		if (bis)
			c.run(bp.run(x), V26_OFF_MS, true, 8);
		else
			c.run(x, V26_OFF_MS, true, V26_BACK_MS);
	};
	const double a = 22300 * pow(10, dbm0 / 20);
	for (int k = 0; k < 8000; k++)
		step(tone(22300 * 0.1));
	for (int k = 0; k < 8000; k++)
		step(tone(a));
	bool on = c.on() != 0;
	if (off_ms || on_ms) {
		int n = 0;
		for (; c.on() && n < 8000; n++)
			step(0);
		if (off_ms)
			*off_ms = n / 8.0;
		for (int k = 0; k < 8000; k++)
			step(0);
		for (n = 0; !c.on() && n < 8000; n++)
			step(tone(a));
		if (on_ms)
			*on_ms = n / 8.0;
	}
	return on;
}

/* 3.3/V.26 ter: 109 ON above -43 dBm, OFF below -48 dBm, 2 dB of
   hysteresis at least; ON to OFF in 5 to 15 ms; after a dropout, ON 40
   to 50 ms after the level is over the threshold */
TEST(V26, Circuit109)
{
	EXPECT_TRUE(v26_109(-45.5));
	EXPECT_FALSE(v26_109(-48.5));
	double off, on;
	v26_109(-42.5, nullptr, &on);
	EXPECT_LT(on, 1000);
	v26_109(-44.5, nullptr, &on);
	EXPECT_EQ(on, 1000);
	for (double l : { 0.0, -10.0, -20.0, -30.0, -40.0 }) {
		v26_109(l, &off, &on);
		EXPECT_GE(off, 5.0) << l << " dBm0";
		EXPECT_LE(off, 15.0) << l << " dBm0";
		EXPECT_GE(on, 40.0) << l << " dBm0";
		EXPECT_LE(on, 50.0) << l << " dBm0";
	}
}

/* Table 3/V.26 bis: the caller's circuit 109 in data, ON to OFF in 5 to
   15 ms, OFF to ON after a dropout in 5 to 15 ms, from 3 dB over the ON
   level; and in the call set up, 109 (the caller's CONNECT) 300 to 700
   ms after the answerer's synchronizing signal starts */
TEST(V26bis, Circuit109)
{
	EXPECT_TRUE(v26_109(-45.5, nullptr, nullptr, true));
	EXPECT_FALSE(v26_109(-48.5, nullptr, nullptr, true));
	for (double l : { 0.0, -10.0, -20.0, -30.0, -40.0 }) {
		double off, on;
		v26_109(l, &off, &on, true);
		EXPECT_GE(off, 5.0) << l << " dBm0";
		EXPECT_LE(off, 15.0) << l << " dBm0";
		EXPECT_GE(on, 5.0) << l << " dBm0";
		EXPECT_LE(on, 15.0) << l << " dBm0";
	}
	LoopbackConfig c = line(226, 40, 6);
	std::string dump = testing::TempDir() + "v26bis_setup";
	c.dump = dump.c_str();
	LoopbackResult r = loopback_run(c);
	ASSERT_TRUE(r.side[0].connected);
	double on = fsk_onset(dump + ".c", r.side[0].connect_s, c.srate);
	ASSERT_GE(on, 0);
	EXPECT_GE(r.side[0].connect_s - on, 0.29);
	EXPECT_LE(r.side[0].connect_s - on, 0.71);
	remove((dump + ".a").c_str());
	remove((dump + ".c").c_str());
}

/* 3.3/V.22 and 3.3/V.22 bis: 109 ON above -43 dBm, OFF below -48 dBm,
   2 dB of hysteresis at least; 109 ON to OFF in 10 to 24 ms (Table
   3/V.22), 40 to 65 ms at 2400 bit/s (3.2/V.22 bis) */
TEST(V22, Circuit109)
{
	EXPECT_TRUE(v22_109(-42.5, false));
	EXPECT_FALSE(v22_109(-44.5, false));
	EXPECT_TRUE(v22_109(-45.5, true));
	EXPECT_FALSE(v22_109(-48.5, true));
	for (double l : { 0.0, -10.0, -20.0, -30.0, -42.0 }) {
		double t;
		v22_109(l, false, &t, 1200);
		EXPECT_GE(t, 10.0) << l << " dBm0";
		EXPECT_LE(t, 24.0) << l << " dBm0";
		v22_109(l, false, &t, 2400);
		EXPECT_GE(t, 40.0) << l << " dBm0";
		EXPECT_LE(t, 65.0) << l << " dBm0";
	}
}

/* the sample times (from the start of the file path, 16 bit samples at
   srate) of the first count phase reversals of the tone at freq: where
   its phasor (over 2.5 ms, which takes out the other lines of AA and AC)
   turns over against 16 symbols before, the time that its projection on
   the earlier phasor crosses zero; the steady tone before each at least
   1/8 of the largest */
static std::vector<double> v32_flips(const std::string &path, double freq,
				     int srate, int count)
{
	std::vector<double> at;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return at;
	std::vector<s16> x;
	s16 v;
	while (fread(&v, sizeof(v), 1, f) == 1)
		x.push_back(v);
	fclose(f);
	const int w = srate / 400, d = 16 * srate / 2400;
	std::vector<std::complex<double>> z(x.size(), 0);
	double peak = 0;
	for (size_t n = 0; n + w < x.size(); n++) {
		std::complex<double> s = 0;
		for (int k = 0; k < w; k++)
			s += static_cast<double>(x[n + k]) *
				std::polar(1.0, -2 * M_PI * freq * (n + k) /
					   srate);
		z[n] = s;
		peak = std::max(peak, std::abs(s));
	}
	for (size_t n = d; n < z.size() && at.size() < (size_t)count; n++) {
		std::complex<double> a = z[n], b = z[n - d];
		if (std::abs(b) < peak / 8 || std::abs(a) < 0.5 * std::abs(b) ||
		    std::real(a * std::conj(b)) > -0.5 * std::abs(a) *
		    std::abs(b))
			continue;
		/* the zero crossing of the projection on b */
		double prev = std::abs(b);
		for (size_t m = n - d + 1; m <= n; m++) {
			double p = std::real(z[m] * std::conj(b)) /
				std::abs(b);
			if (p <= 0) {
				at.push_back(m - 1 + prev / (prev - p) +
					     (w - 1) / 2.0);
				break;
			}
			prev = p;
		}
		n += 4 * d;
	}
	return at;
}

/* 5.4/V.32 (6.1 and 6.2/V.32 bis): the caller's AA to CC transition
   64 +- 2 symbols after the answerer's AC to CA reversal arrives at its
   line terminals, and the answerer's CA to AC transition 64 +- 2
   symbols after the CC arrives.  The dumps hold what each pump takes in
   the next block (FRAG, 48 samples, later), the line's delays (37
   samples to the answerer, 53 to the caller) in them; a flat line */
TEST(V32, TurnRoundDelay)
{
	for (int srate : { 8000, 9600 }) {
		LoopbackConfig c = line(32, 40, 6, srate);
		c.flat = true;
		std::string dump = testing::TempDir() + "v32_turn";
		c.dump = dump.c_str();
		loopback_run(c);
		std::vector<double> c6 = v32_flips(dump + ".c", 600, srate, 20);
		std::vector<double> a18 = v32_flips(dump + ".a", 1800, srate,
						    20);
		remove((dump + ".a").c_str());
		remove((dump + ".c").c_str());
		/* CC: the caller's 1800 Hz turned over within 100 ms after
		   a reversal of the answerer's; the reversals each side */
		double ca[2] = { -1, -1 }, cc = -1;
		for (double a : a18) {
			for (double v : c6)
				if (v < a && v > a - srate / 10)
					ca[0] = v;
				else if (v > a && ca[1] < 0)
					ca[1] = v;
			if (ca[0] >= 0) {
				cc = a;
				break;
			}
			ca[1] = -1;
		}
		ASSERT_GE(cc, 0) << srate;
		ASSERT_GE(ca[1], 0) << srate;
		const double t = srate / 2400.0;
		double caller = ((cc - 37) - (ca[0] + 48)) / t;
		double answerer = ((ca[1] - 53) - (cc + 48)) / t;
		EXPECT_NEAR(caller, 64, 2) << srate;
		EXPECT_NEAR(answerer, 64, 2) << srate;
	}
}

/* 2.4.1.1/V.32: 9600 bit/s without the trellis code, with a modem that
   offers no trellis code (DSP_V32_NO_TCM, the caller): V.32 and V.32bis
   both at 9600 bit/s both ways, without errors */
TEST(V32, UncodedRate)
{
	setenv("DSP_V32_NO_TCM", "1c", 1);
	for (int dp : { 32, 132 }) {
		LoopbackResult r = loopback_run(line(dp, 35, 15));
		for (int k = 0; k < 2; k++) {
			const char *who = k ? "answerer" : "caller";
			EXPECT_TRUE(r.side[k].ok()) << dp << " "
						     << r.side[k].text(who);
			EXPECT_EQ(r.side[k].tx_rate, 9600) << dp << " " << who;
			EXPECT_EQ(r.side[k].rx_rate, 9600) << dp << " " << who;
			EXPECT_EQ(r.side[k].errors, 0) << dp << " "
						      << r.side[k].text(who);
		}
	}
	unsetenv("DSP_V32_NO_TCM");
}

/* 5.4/V.32: the caller's R2 calls for a GSTN cleardown
   (DSP_V32_CLEARDOWN): the answerer's R3 calls for one too, and neither
   modem connects */
TEST(V32, Cleardown)
{
	setenv("DSP_V32_CLEARDOWN", "1c", 1);
	setenv("DSP_DEBUG", "2", 1);
	testing::internal::CaptureStderr();
	LoopbackResult r = loopback_run(line(132, 35, 15));
	std::string log = testing::internal::GetCapturedStderr();
	unsetenv("DSP_V32_CLEARDOWN");
	unsetenv("DSP_DEBUG");
	dsplibs_debug_level = 0;
	EXPECT_FALSE(r.side[0].connected);
	EXPECT_FALSE(r.side[1].connected);
	/* the answerer's word, B4, B8 and the sync bits alone */
	EXPECT_NE(log.find("no common rate in 0991: cleardown"),
		  std::string::npos) << log;
	EXPECT_NE(log.find("R word 0991: cleardown"), std::string::npos)
		<< log;
}

/* Table 6/V.32 and Table 5/V.32bis: V.32's rate words have B4 (2400
   bit/s) 0 and B8 for its trellis code (R1 0791: 4800, 9600 and the
   trellis code); V.32bis's B4 and B8 1, at 4800 bit/s alone too (R1
   0D91) */
TEST(V32, RateWords)
{
	for (auto [dp, max, word] : { std::make_tuple(32, 0, "0791"),
				      std::make_tuple(132, 0, "0FF9"),
				      std::make_tuple(132, 4800, "0D91") }) {
		LoopbackConfig c = line(dp, 35, 8);
		c.max_rate = max;
		setenv("DSP_DEBUG", "2", 1);
		testing::internal::CaptureStderr();
		LoopbackResult r = loopback_run(c);
		std::string log = testing::internal::GetCapturedStderr();
		unsetenv("DSP_DEBUG");
		dsplibs_debug_level = 0;
		EXPECT_TRUE(r.side[0].connected && r.side[1].connected) << dp;
		EXPECT_NE(log.find(std::string("R word ") + word),
			  std::string::npos) << dp << " " << max << "\n" << log;
	}
}

/* the reversals of tone A (2400 Hz, *a) in what the caller hears and of
   tone B (1200 Hz, *b) in what the answerer hears, in a V.34 call's
   phase 2 (from 3 s on, after V.8), each after 20 ms of its tone without
   a reversal (not the DPSK of the INFO frames) */
static void v34_tone_flips(const std::string &dump, int srate,
			   std::vector<double> *a, std::vector<double> *b)
{
	const double from = 3.0 * srate, quiet = 0.035 * srate;
	for (int k = 0; k < 2; k++) {
		std::vector<double> f = v32_flips(dump + (k ? ".a" : ".c"),
						  k ? 1200 : 2400, srate, 400);
		std::vector<double> *out = k ? b : a;
		for (size_t i = 1; i < f.size(); i++)
			if (f[i] >= from && f[i] - f[i - 1] >= quiet)
				out->push_back(f[i]);
	}
}

/* 11.2.1/V.34: in phase 2 each reversal of tone A or B that answers a
   reversal of the far modem's tone is on the line 40 +- 1 ms after that
   reversal was at the line terminals: the caller's B-bar after A-bar,
   the answerer's A after B-bar, and the caller's B-bar after the
   answerer's second A-bar (before L1).  (Line delays as in
   V32.TurnRoundDelay: 37 samples to the answerer, 53 to the caller,
   and the dumps a block, 48 samples, early.) */
TEST(V34, ToneReversalDelay)
{
	for (int srate : { 8000, 9600 }) {
		LoopbackConfig c = line(34, 40, 5, srate);
		c.flat = true;
		std::string dump = testing::TempDir() + "v34_turn";
		c.dump = dump.c_str();
		loopback_run(c);
		std::vector<double> a, b;
		v34_tone_flips(dump, srate, &a, &b);
		remove((dump + ".a").c_str());
		remove((dump + ".c").c_str());
		const double ms = srate / 1000.0;
		/* each B-bar 25 to 75 ms after an A reversal: the caller's
		   delay; the first A reversal after the first B-bar: the
		   answerer's */
		std::vector<double> caller, answerer;
		for (double bb : b) {
			for (double aa : a)
				if (bb - aa > 25 * ms && bb - aa < 75 * ms) {
					caller.push_back(((bb - 37) - (aa + 48)) /
							 ms);
					break;
				}
		}
		/* (the B-bar of the first pair) */
		for (double bb : b)
			for (double aa : a)
				if (answerer.empty() && bb - aa > 25 * ms &&
				    bb - aa < 75 * ms)
					for (double a2 : a)
						if (a2 > bb && a2 - bb < 75 * ms) {
							answerer.push_back(
								((a2 - 53) -
								 (bb + 48)) / ms);
							break;
						}
		/* (later pairs are L1 and L2) */
		ASSERT_GE(caller.size(), 2u) << srate;
		ASSERT_GE(answerer.size(), 1u) << srate;
		for (int i = 0; i < 2; i++)
			EXPECT_NEAR(caller[i], 40, 1) << srate << " caller";
		EXPECT_NEAR(answerer[0], 40, 1) << srate << " answerer";
	}
}
