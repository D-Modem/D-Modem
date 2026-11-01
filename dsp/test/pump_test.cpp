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

/* data pump ids: 8 V.34, 32 V.32, 132 V.32bis, 232 V.32terbo, 61 V.61, 126 V.26 ter,
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
	Pumps{ "v22bis", line(122, 40, 20) },
	Pumps{ "v22bis_snr15", line(122, 15, 20) },
	Pumps{ "v22bis_8000", line(122, 30, 20, 8000) },
	Pumps{ "v22bis_gain2_m300ppm", line(122, 30, 30, 9600, 2, -300) },
	Pumps{ "v22bis_gain005_300ppm", line(122, 30, 30, 9600, 0.05f, 300) },
	Pumps{ "v34", line(8, 30, 20) },
	/* only 2400 baud carries data (2400 bit/s, which no other symbol
	   rate has) */
	Pumps{ "v34_snr10", line(8, 10, 25) },
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
	Pumps{ "bell103_to_v34", automode(103, 8, 45) }), name);

#include "v34.h"

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
