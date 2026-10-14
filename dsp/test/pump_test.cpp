/*
 *    pump_test.cpp - the open data pumps back to back (loopback.h): each
 *    modulation on a clean and a noisy line, at 8000 Hz, with clock
 *    offsets, line gains, near and far echoes; and the answering automode,
 *    a caller of another modulation finding its way to it.  Both ways
 *    connected, with a bit error rate of 1e-3 at most.
 *
 *    DSP_DEBUG=<level>   the pumps' log
 */

#include <stdlib.h>

#include <string>

#include <gtest/gtest.h>

#include "dsplibs.h"
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

/* data pump ids: 8 V.34, 32 V.32, 132 V.32bis, 22 V.22, 122 V.22bis,
   212 Bell 212A, 21 V.21, 23 V.23, 103 Bell 103 */
INSTANTIATE_TEST_SUITE_P(Line, PumpLoopback, testing::Values(
	Pumps{ "v32bis", line(132, 40, 20) },
	Pumps{ "v32bis_snr25", line(132, 25, 20) },
	Pumps{ "v32bis_8000", line(132, 30, 20, 8000) },
	Pumps{ "v32bis_300ppm", line(132, 30, 30, 9600, 0.5f, 300) },
	Pumps{ "v32bis_echo6", line(132, 30, 20, 9600, 0.5f, 0, -6) },
	Pumps{ "v32bis_snr11", line(132, 11, 20) },
	Pumps{ "v32", line(32, 30, 20) },
	Pumps{ "v22bis", line(122, 40, 20) },
	Pumps{ "v22bis_snr15", line(122, 15, 20) },
	Pumps{ "v22bis_8000", line(122, 30, 20, 8000) },
	Pumps{ "v22bis_gain2_m300ppm", line(122, 30, 30, 9600, 2, -300) },
	Pumps{ "v22bis_gain005_300ppm", line(122, 30, 30, 9600, 0.05f, 300) },
	Pumps{ "v34", line(8, 30, 20) },
	/* only 2400 baud carries data (2400 bit/s, which no other symbol
	   rate has) */
	Pumps{ "v34_snr10", line(8, 10, 25) },
	Pumps{ "v34_echoes", line(8, 40, 22, 9600, 0.5f, 0, -10, -15) },
	Pumps{ "v34_echoes_8000", line(8, 40, 22, 8000, 0.5f, 0, -10, -15) },
	/* a far echo over VoIP */
	Pumps{ "v34_far_echo_voip", line(8, 40, 22, 9600, 0.5f, 0, 0, -15, 1920) },
	Pumps{ "v22", line(22, 15, 20) },
	Pumps{ "bell212", line(212, 15, 20) },
	Pumps{ "v21", line(21, 8, 20) },
	Pumps{ "v23", line(23, 12, 30) },
	Pumps{ "bell103_300ppm", line(103, 8, 20, 9600, 0.5f, 300) }), name);

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
