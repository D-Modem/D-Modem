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

#include <stdlib.h>

#include <string>

#include <gtest/gtest.h>

#include "dsplibs.h"
#include "codec_sim.h"
#include "loopback.h"

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

/* data pump ids: 8 V.34, 32 V.32, 132 V.32bis, 232 V.32terbo, 168 HST, 61 V.61, 126 V.26 ter, 226 V.26 bis,
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
   16800 at 29 dB, V.32bis's 14400 at 25 dB; each without errors */
TEST(V32Terbo, Rates)
{
	for (auto [snr, rate] : { std::make_pair(40.0f, 19200),
				  std::make_pair(29.0f, 16800),
				  std::make_pair(25.0f, 14400) }) {
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

/* V.32bis's rate renegotiation (8.5/V.32bis, DSP_V32_RENEG) by the
   caller or the answerer, down to 9600 bit/s: the other modem answers
   it (no retrain), both go on at 9600; the errors all in the one burst
   of the change over (the count realigned once after it, no errors
   since: SHOW_ERRORS with V32_RENEG_LOG shows them) */
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
