/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    interop_test.cpp - the interop tests: calls between the open modem
 *                       (half_open, half_open8) and the other modems we
 *                       have as programs (Smart Link's original library,
 *                       the Agere, SM56, PCTel and HSF soft modems,
 *                       Broadcom's Windows driver, spandsp's modems),
 *                       through interop_call
 *
 *    One table of calls; each test program has the calls of one ctest
 *    label (INTEROP_LABEL).  The modem programs are HALF_* (undefined:
 *    that modem's calls are left out), netsim NETSIM; each call's logs go
 *    to its test's directory of the run's test logs (DMODEM_TEST_DIR,
 *    cmake/test_logs.cmake), or run by hand to INTEROP_LOG_DIR/<test>.
 *    A call's environment (interop_call.h: NET, STREAM, EXPECT_B, ...;
 *    and the modems' and netsim's own: DSP_*, NET_*, ...) is set for it
 *    alone.
 *
 *    The calls run as fast as the modems can: they keep time by the
 *    line's samples, and so does the call, by netsim's clock
 *    or for fax by interop_call's relay of the line.  MODEM_REALTIME=1
 *    for ctest runs every call in real time.
 *
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "interop_call.h"
#include "tones.h"

namespace {

struct Call {
	std::string label;
	std::string name;
	std::string a, b;       /* the modems: smartlink, open, open8, ... */
	std::string proto;      /* the AT commands (fax: the setup) */
	std::vector<std::string> env;
	int fax = 0;            /* fax: the modulation, data call: 0 */
};

/* the modem programs built (HALF_* defined): the tables' names to paths */
const std::map<std::string, const char *> &programs()
{
	static const std::map<std::string, const char *> p = {
		{ "smartlink", HALF_SMARTLINK }, { "open", HALF_OPEN },
		{ "open8", HALF_OPEN8 },
#ifdef HALF_AGERE
		{ "agere", HALF_AGERE },
#endif
#ifdef HALF_SM56
		{ "sm56", HALF_SM56 },
#endif
#ifdef HALF_SPANDSP
		{ "spandsp", HALF_SPANDSP },
#endif
#ifdef HALF_PCTEL
		{ "pctel", HALF_PCTEL },
#endif
#ifdef HALF_HSF
		{ "hsf", HALF_HSF },
#endif
#ifdef HALF_BROADCOM
		{ "broadcom", HALF_BROADCOM },
#endif
#ifdef HALF_INTEL
		{ "intel", HALF_INTEL },
#endif
#ifdef HALF_ESS
		{ "ess", HALF_ESS },
#endif
#ifdef HALF_HDA
		{ "hda", HALF_HDA },
#endif
	};
	return p;
}

/* every call of every label, in order */
std::vector<Call> calls()
{
	std::vector<Call> c;
	/* a call: label, name, the modems a (caller or fax sender) and b,
	   the AT commands and the environment; the Call, for fax */
	auto add = [&c](const char *label, std::string name, const char *a,
			const char *b, std::string proto,
			std::vector<std::string> env = {}) {
		c.push_back({ label, std::move(name), a, b, std::move(proto),
			      std::move(env) });
		return &c.back();
	};

	/* --- Smart Link's soft modem, its original library on the modem
	   core (half_smartlink, 9600 Hz), and the open modem call each
	   other at each modulation */
	for (const char *ms : { "34", "132", "122", "22", "212", "103",
				"23" }) {
		std::string p = std::string("ATX3;AT+MS=") + ms;
		add("smartlink",
		    std::string("smartlink_") + ms + "_smartlink_calls",
		    "smartlink", "open", p);
		add("smartlink", std::string("smartlink_") + ms + "_open_calls",
		    "open", "smartlink", p);
	}
	/* V.34 through a line hit (300 ms of silence both ways) in the
	   middle of the data: both modems retrain, V.42 carries on */
	for (auto [n, a, b] :
	     { std::make_tuple("smartlink", "smartlink", "open"),
	       std::make_tuple("open", "open", "smartlink") })
		add("smartlink", std::string("smartlink_34_hit_") + n +
		    "_calls", a, b, "ATX3;AT+MS=34",
		    { "NET=9600:9600:ulaw", "STREAM=25", "NET_DROP=20:300" });
	/* V.34 rate renegotiation: the open modem starts one (and later
	   goes back up), the original library answers it */
	for (auto [n, a, b] :
	     { std::make_tuple("smartlink", "smartlink", "open"),
	       std::make_tuple("open", "open", "smartlink") })
		add("smartlink", std::string("smartlink_34_reneg_") + n +
		    "_calls", a, b, "ATX3;AT+MS=34",
		    { "NET=9600:9600:ulaw", "NET_SYNC=1", "STREAM=25",
		      "DSP_V34_RENEG=5000:8" });
	/* V.90: the original library (analogue modem) calls the open one
	   (digital modem, 8000 Hz) through the simulated G.711 network.
	   NET_FLOOR: on a line without any noise its phase 4 check (the
	   error before and after TRN2d corrects its levels from the DIL by
	   a unit or two) fails, and it retrains once for nothing; a floor
	   of -81 dBFS keeps the check to what it is for on a real line */
	add("smartlink", "smartlink_v90_ulaw", "smartlink", "open8",
	    "ATX3;AT+MS=90", { "NET=9600:8000:ulaw", "NET_FLOOR=3" });
	add("smartlink", "smartlink_v90_alaw", "smartlink", "open8",
	    "ATX3;AT+MS=90", { "NET=9600:8000:alaw", "DSP_G711=alaw" });
	/* V.90 rate renegotiation (9.6/V.90): ours after 5 s of data, to
	   24 000 bit/s upstream; the library's own when noise on the PCM
	   codes from 40 s on worsens the downstream: it asks for a silent
	   period (CPs), and the noise stops with our silence (else it may
	   pick 56000 again and the data goes wrong) */
	add("smartlink", "smartlink_v90_reneg", "smartlink", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=3", "STREAM=15",
	      "DSP_V34_RENEG=5000:10",
	      "EXPECT_B=V.90 renegotiated [0-9]+/24000" });
	add("smartlink", "smartlink_v90_reneg_silence", "smartlink", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=3", "STREAM=50",
	      "NET_NOISE=40:20:10", "NET_NOISE_DIR=bacodes",
	      "NET_NOISE_QUIET=50",
	      "EXPECT_B=(?s)CPs'.*V.90 renegotiated" });
	/* V.92 quick connect (short phase 1): the original library, a V.92
	   analogue modem, starts with QC1a; then V.90 */
	add("smartlink", "smartlink_v92_qc", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "EXPECT_B=V.92 quick connect" });
	/* V.92 PCM upstream: the original library is a "V.92 Lite" build,
	   which retrains when INFO1d offers PCM upstream; SMARTLINK_V92FULL
	   (smartlink_glue.cpp) sets its full V.92 bit, and it then goes
	   through phases 3 and 4 of V.92 with us.  Lite: we answer its
	   retrain without the offer, V.90.  A rate the line cannot carry
	   (44 000): B1u does not come through, we retrain to V.90.  With the
	   prefilter we design (6.4.2/V.92) the rate is up to 38 666 (mu-law)
	   and 40 000 (A-law); without it (DSP_V92_PREFILTER=0), the precoder
	   alone, 34 000 to 36 000 */
	for (const char *law : { "ulaw", "alaw" })
		add("smartlink", std::string("smartlink_v92_pcmup_") + law,
		    "smartlink", "open8", "ATX3;AT+MS=92",
		    { std::string("NET=9600:8000:") + law, "STREAM=10",
		      "SMARTLINK_V92FULL=1",
		      "EXPECT_B=(?s)prefilter of 64 taps.*V.92 connect "
		      "56000/(3[89]|4[0-2])" });
	add("smartlink", "smartlink_v92_pcmup_noprefilter", "smartlink",
	    "open8", "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "SMARTLINK_V92FULL=1",
	      "DSP_V92_PREFILTER=0", "EXPECT_B=V.92 connect 56000/3[4-9]" });
	/* two calls: the second starts wherever the hangup left the two
	   modems' lines (a busy machine moves it), and at some of those
	   alignments, on a line without any noise, the library's phase 4
	   check (as in V.90, above) came to 1.6 or 1.7 and it retrained,
	   to V.90 without the offer; NET_FLOOR keeps it to 1.2 at most */
	add("smartlink", "smartlink_v92_pcmup_calls", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=3", "CALLS=2", "STREAM=3",
	      "SMARTLINK_V92FULL=1",
	      "EXPECT_B=(?s)V.92 connect.*V.92 connect" });
	add("smartlink", "smartlink_v92_pcmup_lite", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=3",
	      "EXPECT_B=(?s)no PCM upstream offer.*V.90 connect" });
	add("smartlink", "smartlink_v92_pcmup_fallback", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=3", "SMARTLINK_V92FULL=1",
	      "DSP_V92_UP_DRN=16",
	      "EXPECT_B=(?s)B1u does not come through.*V.90 connect" });
	/* rate renegotiation (9.8/V.92): ours after 3 s of data, to 30 666
	   bit/s upstream (DSP_V92_RENEG=<ms>:<drn>); and on its own when
	   noise on the way up eats the margin of our rate */
	add("smartlink", "smartlink_v92_reneg", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "SMARTLINK_V92FULL=1",
	      "DSP_V92_RENEG=3000:6",
	      "EXPECT_B=V.92 renegotiated 56000/30666" });
	add("smartlink", "smartlink_v92_reneg_noise", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=15", "SMARTLINK_V92FULL=1",
	      "NET_NOISE=26:60", "NET_NOISE_DIR=ab",
	      "EXPECT_B=V.92 renegotiated 56000/[23]" });
	/* fast parameter exchange (9.9/V.92): ours after 3 s of data, to
	   30 666 bit/s upstream (DSP_V92_FPE=<ms>:<drn>), in the data mode
	   modulation.  Without the prefilter (below with it) */
	add("smartlink", "smartlink_v92_fpe", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "SMARTLINK_V92FULL=1",
	      "DSP_V92_PREFILTER=0", "DSP_V92_FPE=3000:6",
	      "EXPECT_B=(?s)fast parameter exchange 1 done.*V.92 "
	      "renegotiated 56000/30666" });
	/* modem-on-hold asked for by us (AT+PMHR, 9.10.2.1/V.92) in the
	   middle of PCM upstream data: the original library answers our
	   tone B with a retrain, which we follow, and the data goes on */
	add("smartlink", "smartlink_v92_moh_request", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=30", "SMARTLINK_V92FULL=1",
	      "MIDCALL_B=8:AT+PMHR;WAIT 6",
	      "EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.92 "
	      "retrained" });
	/* and with an analogue modem that answers it (netsim's NET_MOH
	   stands in for one from 22 s on: the original library does not):
	   MHack, the remote on hold for 5 s, then we take it back (its
	   ANSam, our CM: 9.10.2.1/V.92); MHnack, MHfrr and ANSam.  The
	   original library is cut off meanwhile and hangs up, so no ANSam
	   comes after the hold (we give up after 5 s), and nothing to ours
	   after MHfrr (we disconnect after T1, 10 s) */
	add("smartlink", "smartlink_v92_moh_request_ack", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=40", "SMARTLINK_V92FULL=1",
	      "NET_MOH=ack:1@22", "MIDCALL_B=12:AT+PMHR=5;WAIT 8",
	      "EXPECT_DROP=1",
	      "EXPECT_B=(?s)MHack, T1 10 s: the remote on hold for 5 s.*"
	      "taking the remote back from hold.*back from hold: no ANSam" });
	add("smartlink", "smartlink_v92_moh_request_nack", "smartlink",
	    "open8", "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=35", "SMARTLINK_V92FULL=1",
	      "NET_MOH=nack@22", "MIDCALL_B=12:AT+PMHR;WAIT 3",
	      "EXPECT_DROP=1",
	      "EXPECT_B=(?s)MHnack: MHfrr.*on hold \\(reconnect\\), ANSam.*"
	      "on hold for T1: disconnect" });
	/* and with the prefilter: not down (the original library sends
	   SUVu' to FB1u on the new points with the old moduli), so at the
	   rate it has; the library's sequence through to B1u, and the data
	   goes on */
	add("smartlink", "smartlink_v92_fpe_prefilter", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "SMARTLINK_V92FULL=1",
	      "DSP_V92_FPE=3000:6",
	      "EXPECT_B=(?s)prefilter not down.*SUVu'.*fast parameter "
	      "exchange 1 done.*V.92 renegotiated 56000/3[89]" });
	/* the original library's own fast parameter exchange
	   (half_smartlink's SMARTLINK_FPE_AT calls its initiateFPE, which
	   nothing in it does on its own): its RM, our Rf, its SUVu to B1u,
	   and the data goes on; with the prefilter and without */
	for (int pf : { 1, 0 }) {
		std::vector<std::string> e = {
			"NET=9600:8000:ulaw", "STREAM=10",
			"SMARTLINK_V92FULL=1", "SMARTLINK_FPE_AT=4",
			"DSP_V92_SLIP=0",
			"EXPECT_B=(?s)RM at.*fast parameter exchange 1 done.*"
			"V.92 renegotiated 56000/(3[0-9]|4[01])" };
		if (!pf)
			e.push_back("DSP_V92_PREFILTER=0");
		add("smartlink", pf ? "smartlink_v92_fpe_remote" :
		    "smartlink_v92_fpe_remote_noprefilter", "smartlink",
		    "open8", "ATX3;AT+MS=92", e);
	}
	/* the original library's timing slips against the network's
	   clock, and the prefilter's design with it: about every 35 s of
	   line a design for the slip in a fast parameter exchange at the
	   same rate (0.35 s), and the data goes on */
	for (const char *law : { "ulaw", "alaw" })
		add("smartlink", std::string("smartlink_v92_slip_") + law,
		    "smartlink", "open8", "ATX3;AT+MS=92",
		    { std::string("NET=9600:8000:") + law, "STREAM=30",
		      "SMARTLINK_V92FULL=1",
		      "EXPECT_B=(?s)the prefilter for a slip.*fast parameter "
		      "exchange 1 done.*V.92 renegotiated 56000/(3[89]|4[0-2])" });
	/* after the slip's exchange our own (DSP_V92_FPE) to a rate the
	   design does not have (48000): it stays at the rate with the CPd
	   as it is (not from the first design's gap with the slip's
	   precoder) */
	add("smartlink", "smartlink_v92_slip_fpe", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=40", "SMARTLINK_V92FULL=1",
	      "DSP_V92_FPE=29000:19",
	      "EXPECT_B=(?s)the prefilter for a slip.*fast parameter "
	      "exchange 1 done.*FPE CPd: as it is.*fast parameter "
	      "exchange 2 done" });
	/* without the offer (DSP_V92_PCMUP=0): V.90, V.34 upstream */
	add("smartlink", "smartlink_v92_nopcmup", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=3", "SMARTLINK_V92FULL=1",
	      "DSP_V92_PCMUP=0", "EXPECT_B=V.90 connect" });
	/* class 1 fax calls both ways (the line's clock: 9600 Hz) */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145 }) {
		std::string s = std::to_string(m);
		add("smartlink", "smartlink_fax_" + s + "_smartlink_sends",
		    "smartlink", "open", "ATX3",
		    { "LINE_RATE=9600" })->fax = m;
		add("smartlink", "smartlink_fax_" + s + "_open_sends", "open",
		    "smartlink", "ATX3", { "LINE_RATE=9600" })->fax = m;
	}

	/* --- two open modems through the same driver: V.34 on a noisy
	   line that gets 3 dB worse 10 s after CONNECT (NET_SYNC), each
	   lowers its receiving rate */
	add("open", "open_34_noise", "open", "open", "ATX3;AT+MS=34",
	    { "NET=9600:9600:ulaw", "NET_SYNC=1", "STREAM=30",
	      "NET_FLOOR=250", "NET_NOISE=10:250" });
	/* --- the open modem as V.90's analogue modem (9600 Hz)
	   and the open digital modem (8000 Hz), each calling: V.8 agrees
	   on V.90, phase 2 has the analogue modem as the V.34 answer
	   modem, INFO0d's law, INFO1a asks for V.90; in phase 3 Ja asks
	   for a DIL of every Ucode, the PCM receiver finds Sd-bar, trains
	   on TRN1d and takes Jd, our S ends Jd, Jd' our S-bar; two rounds
	   of the DIL measured, our S and S-bar end it.  Phase 4: our CPt,
	   the digital modem's Ri, Ri-bar, TRN2d, MP; our CP, CP', E; its
	   Ed and B1d, decoded without an error.  Then the data mode, PCM
	   down and V.34 up (BULK: octets each way, checked; the
	   downstream's error under 1 rms, where the smallest levels are
	   16 apart).  With netsim's pads (NET_PAD) and robbed bits
	   (NET_RBS) on the codes to the analogue modem the DIL finds them,
	   and CP leaves out what they make alike; DSP_V90A_DIL=0 asks for
	   no DIL */
	add("open", "open_v90a_ulaw", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "BULK=200000",
	      "EXPECT_A=(?s)V.90 with us as the analogue modem.*INFO0d u-law"
	      ".*V.90 upstream 3429 baud.*Sd-bar.*Jd: rates 3fffff.*Jd'"
	      ".*DIL: pad 0.0 dB.*robbed none.*phase 3 done.*CPt: Ucodes"
	      ".*Ri-bar.*TRN2d: 0 of 2040 symbols wrong.*CP: D 42 .*MP'"
	      ".*Ed at frame.*B1d: 0 bit errors in 48 frames of 42 bits"
	      ".*V.90 connect 3[0-9]+/56000 \\(analogue modem\\)"
	      ".*V.90 data: error 0.[0-9]+ rms",
	      "EXPECT_B=(?s)V.90 with us as the digital modem.*INFO1a: V.90, "
	      "upstream 3429 baud, UINFO 79.*DIL N 128.*V.90 S after [0-9]+ "
	      "Jd.*V.90 DIL ends after 2 rounds.*V.90 CPt: D 18, K 12, M 4 4 "
	      "4 4 4 4.*CP' at symbol.*E at symbol.*V.90 connect 56000/" });
	add("open", "open_v90a_alaw", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:alaw", "DSP_G711=alaw",
	      "EXPECT_A=(?s)INFO0d A-law.*V.90 upstream.*Jd: rates.*"
	      "DIL: pad 0.0 dB.*robbed none.*phase 3 done.*"
	      "B1d: 0 bit errors in 48 frames of 42 bits",
	      "EXPECT_B=V.90 connect 56000/" });
	add("open", "open_v90a_answers", "open8", "open", "ATX3;AT+MS=90",
	    { "NET=8000:9600:ulaw",
	      "EXPECT_A=(?s)INFO1a: V.90, upstream 3429 baud, UINFO 79.*"
	      "V.90 DIL ends.*V.90 connect 56000/",
	      "EXPECT_B=(?s)V.90 with us as the analogue modem.*INFO0d u-law"
	      ".*V.90 upstream 3429 baud.*DIL: pad 0.0 dB.*phase 3 done.*"
	      "B1d: 0 bit errors in 48 frames of 42 bits" });
	add("open", "open_v90a_nodil", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "DSP_V90A_DIL=0",
	      "EXPECT_A=(?s)phase 3 done \\(no DIL\\).*CP: D 42 .*"
	      "B1d: 0 bit errors in 48 frames of 42 bits",
	      "EXPECT_B=V.90 connect 56000/" });
	/* the analogue modem's codec on a clock of its own (netsim's
	   NET_PHASE, NET_PPM): its samples half a network sample off the
	   symbols (where a T-spaced receiver at 8000 Hz loses the band's
	   top), and fast or slow by 100 ppm, the timing loop finding it */
	add("open", "open_v90a_phase", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_PHASE=0.5",
	      "EXPECT_A=(?s)timing [-+]0.[0-9]+ ppm.*robbed none.*"
	      "CP: D 42 .*B1d: 0 bit errors in 48 frames of 42 bits",
	      "EXPECT_B=V.90 connect 56000/" });
	add("open", "open_v90a_ppm_fast", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_PPM=100", "NET_PHASE=0.3",
	      "BULK=100000",
	      "EXPECT_A=(?s)timing \\+(99.[5-9]|100.[0-4])[0-9]* ppm.*"
	      "robbed none.*B1d: 0 bit errors in 48 frames of 4[0-2] bits"
	      ".*V.90 data: error [0-2].[0-9]+ rms",
	      "EXPECT_B=V.90 connect 5[0-9]+/" });
	add("open", "open_v90a_ppm_slow", "open8", "open", "ATX3;AT+MS=90",
	    { "NET=8000:9600:alaw", "DSP_G711=alaw", "NET_PPM=-100",
	      "NET_PHASE=0.7",
	      "EXPECT_A=V.90 connect 5[0-9]+/",
	      "EXPECT_B=(?s)timing -(99.[5-9]|100.[0-4])[0-9]* ppm.*"
	      "robbed none.*B1d: 0 bit errors in 48 frames of 4[0-2] bits" });
	/* the analogue modem's own echo from its line's hybrid
	   (DSP_TEST_ECHO, on its side only): the echo canceller trains on
	   S, PP, TRN and Ja (the digital modem silent), by least squares
	   at the end, and takes it from the PCM receiver's line; CP is
	   designed on TRN2d's noise, our SCR and its echo going out */
	add("open", "open_v90a_echo", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "DSP_TEST_ECHO=-10:1c",
	      "EXPECT_A=(?s)echo canceller: least squares [4-9][0-9].[0-9] "
	      "dB.*echo canceller took [4-9][0-9].[0-9] dB away[^:].*"
	      "B1d: 0 bit errors in 48 frames of 4[0-2] bits",
	      "EXPECT_B=V.90 connect 5[0-9]+/" });
	add("open", "open_v90a_echo_answers", "open8", "open",
	    "ATX3;AT+MS=90",
	    { "NET=8000:9600:alaw", "DSP_G711=alaw", "DSP_TEST_ECHO=-6:0.4a",
	      "NET_PPM=50", "NET_PHASE=0.4", "BULK=100000",
	      "EXPECT_A=V.90 connect [0-9]+/",
	      "EXPECT_B=(?s)echo canceller took [3-9][0-9].[0-9] dB away[^:].*"
	      "B1d: 0 bit errors in 48 frames.*V.90 data: error [0-2].[0-9]+ "
	      "rms" });
	add("open", "open_v90a_dil_pad", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_PAD=3",
	      "EXPECT_A=(?s)DIL: pad 3.0 dB.*robbed none.*"
	      "B1d: 0 bit errors in 48 frames of 4[0-2] bits",
	      "EXPECT_B=V.90 connect 5[0-9]+/" });
	add("open", "open_v90a_dil_rbs", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_RBS=0x01", "BULK=100000",
	      "EXPECT_A=(?s)PCM receiver ready at symbol [0-9]+: noise 0.[0-9]+"
	      ".*DIL: pad 0.0 dB.*robbed [0-5], distinct.*"
	      "B1d: 0 bit errors in 48 frames of 42 bits.*"
	      "V.90 data: error [0-1].[0-9]+ rms",
	      "EXPECT_B=V.90 connect 56000/" });
	add("open", "open_v90a_dil_rbs_alaw", "open", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:alaw", "DSP_G711=alaw", "NET_RBS=0x08",
	      "EXPECT_A=(?s)PCM receiver ready at symbol [0-9]+: noise 0.[0-9]+"
	      ".*robbed [0-5], distinct.*B1d: 0 bit errors in 48 frames of "
	      "4[0-2] bits",
	      "EXPECT_B=V.90 connect 5[0-9]+/" });
	add("open", "open_v90a_dil_pad_rbs", "open", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_PAD=6", "NET_RBS=0x24",
	      "NET_FLOOR=30",
	      "EXPECT_A=(?s)DIL: pad 6.0 dB.*robbed [0-5],[0-5], distinct.*"
	      "B1d: 0 bit errors in 48 frames",
	      "EXPECT_B=V.90 connect [0-9]+/" });
	/* stage F: rate renegotiations (9.6/V.90).  The digital modem's
	   (DSP_V34_RENEG on it: upstream to 24 000 bit/s): its Rd, Rd-bar,
	   TRN2d, MP; our S, S-bar, SCR, CP again, CP', E, B1, data at the
	   new rates, the bulk transfer across it */
	add("open", "open_v90a_reneg", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "DSP_V34_RENEG=3000:10a", "BULK=100000",
	      "EXPECT_A=(?s)V.90 connect 3[0-9]+/56000 \\(analogue modem\\)"
	      ".*Rd at symbol.*Rd-bar at symbol.*V.90 renegotiation 1 "
	      "\\(the digital modem's Rd\\).*CP: D 42.*B1d: 0 bit errors.*"
	      "V.90 renegotiated 24000/56000 \\(analogue modem\\)",
	      "EXPECT_B=(?s)V.90 renegotiation 1 \\(test\\).*"
	      "V.90 renegotiated 56000/24000" });
	/* ours (DSP_V90A_RENEG on the analogue modem): our S, S-bar, SCR;
	   its Rd after our S-bar, Rd-bar, TRN2d, MP; our CP ... */
	add("open", "open_v90a_reneg_ours", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "DSP_V90A_RENEG=3000c", "BULK=100000",
	      "EXPECT_A=(?s)V.90 connect 3[0-9]+/56000 \\(analogue modem\\)"
	      ".*V.90 renegotiation 1 \\(ours: test\\).*Rd at symbol.*Rd-bar at "
	      "symbol.*CP: D 42.*B1d: 0 bit errors.*"
	      "V.90 renegotiated 3[0-9]+/56000 \\(analogue modem\\)",
	      "EXPECT_B=(?s)V.90 renegotiation 1 \\(the remote's S\\).*"
	      "V.90 renegotiated 56000/" });
	/* ours on errors: noise on the line to us 16 s in (some 3 s into
	   the data mode), 8 dB and more above what CP was designed for:
	   CP anew on it, a lower rate, the bulk transfer across it */
	add("open", "open_v90a_reneg_noise", "open", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_NOISE=16:25", "NET_NOISE_DIR=ba",
	      "BULK=100000",
	      "EXPECT_A=(?s)V.90 connect 3[0-9]+/56000 \\(analogue modem\\)"
	      ".*V.90 renegotiation 1 \\(ours: errors\\).*"
	      "V.90 renegotiated 3[0-9]+/[34][0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=V.90 renegotiated" });
	/* stage F: retrains (9.5/V.90).  The digital modem's in data mode
	   (DSP_V34_RETRAIN on it): its silence and tone B, our silence and
	   tone A, phase 2 again (the analogue modem the V.34 answerer),
	   phases 3 and 4, data without a new CONNECT; ours (our tone A);
	   and a line too noisy for any CP: retrains, V.34 after them */
	add("open", "open_v90a_retrain", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "DSP_V34_RETRAIN=3000a", "BULK=100000",
	      "EXPECT_A=(?s)V.90 connect 3[0-9]+/56000 \\(analogue modem\\)"
	      ".*retrain 1 \\(tone B\\).*phase 3 done.*"
	      "V.90 retrained 3[0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)retrain 1 \\(test\\).*V.90 retrained" });
	add("open", "open_v90a_retrain_ours", "open", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "DSP_V34_RETRAIN=3000c", "BULK=100000",
	      "EXPECT_A=(?s)V.90 connect 3[0-9]+/56000 \\(analogue modem\\)"
	      ".*retrain 1 \\(test\\).*phase 3 done.*"
	      "V.90 retrained 3[0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)retrain 1 \\(tone A\\).*V.90 retrained" });
	add("open", "open_v90a_v34", "open", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=400", "NET_NOISE_DIR=ba",
	      "EXPECT_A=(?s)V.90 with us as the analogue modem.*"
	      "V.90 failed, V.34 next.*V.34 this time.*"
	      "data rates tx [0-9]+ rx [0-9]+, trellis" });
	/* the DTE's retrain (ATO1) and rate renegotiation (ATO3) in V.90
	   and V.92 data, on either modem: V.90's (9.6/V.90) ours as the
	   analogue modem and as the digital modem, a retrain; with PCM
	   upstream V.92's, the digital modem's (its Ru) and the analogue
	   modem's, and a retrain */
	for (auto [n, k, o, v92, want] : std::vector<std::tuple<const char *,
		     char, const char *, bool, const char *>>{
		     { "v90a_dte_retrain", 'A', "ATO1", false,
		       "retrain 1 \\(the DTE's\\).*V.90 retrained" },
		     { "v90a_dte_reneg", 'A', "ATO3", false,
		       "V.90 renegotiation 1 \\(ours: the DTE's\\).*"
		       "V.90 renegotiated" },
		     { "v90_dte_reneg", 'B', "ATO3", false,
		       "V.90 renegotiation 1 \\(the DTE's\\).*"
		       "V.90 renegotiated" },
		     { "v92_dte_reneg", 'B', "ATO3", true,
		       "V.92 renegotiation \\(the DTE's\\).*v92: renegotiated" },
		     { "v92a_dte_reneg", 'A', "ATO3", true,
		       "V.92 renegotiation 1 \\(ours: the DTE's\\).*"
		       "V.92 renegotiated" },
		     { "v92_dte_retrain", 'B', "ATO1", true,
		       "retrain 1 \\(the DTE's\\).*v92: connect" } }) {
		std::string side(1, k);
		std::vector<std::string> e = {
			"NET=9600:8000:ulaw", "STREAM=25",
			"MIDCALL_" + side + "=6:WAIT 0",
			"MIDCALL_ATO_" + side + "=" + o,
			"EXPECT_" + side + "=(?s)" + want };
		if (v92)
			e.push_back("DSP_V92A_UP=1");
		add("open", std::string("open_") + n, "open", "open8",
		    v92 ? "ATX3" : "ATX3;AT+MS=90", e);
	}
	/* V.92 as the analogue modem (V.92 in our INFO0a, the digital
	   modem's INFO0d): V.90 with V.34 upstream, and modem-on-hold
	   (9.10/V.92).  Ours (AT+PMHR=5 in the middle of the data): tone
	   A, MHreq; its MHack (T1 1 min), our silence, its ANSam; after
	   5 s our CM, phases 1 to 4 again and the data goes on.  Its
	   (AT+PMHR=5 on the digital modem): its tone B and MHreq, our
	   MHack, its silence, our ANSam; its CM after 5 s */
	add("open", "open_v92a_moh", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "STREAM=50",
	      "MIDCALL_A=8:AT+PMHR=5;WAIT 12",
	      "EXPECT_A=(?s)INFO0d V.92: V.92 as the analogue modem.*"
	      "MHack, T1 60 s: the remote on hold for 5 s.*back from hold: "
	      "ANSam, V.8 as the call modem.*V.9[02] back from hold, "
	      "[34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)INFO0a V.92 1.*MHreq: MHack \\(on hold\\).*"
	      "on hold, ANSam.*V.9[02] back from hold" });
	add("open", "open_v92a_moh_theirs", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "STREAM=50",
	      "MIDCALL_B=8:AT+PMHR=5;WAIT 12",
	      "EXPECT_A=(?s)MHreq: MHack \\(on hold\\).*on hold, ANSam.*"
	      "V.9[02] back from hold, [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)MHack, T1 60 s: the remote on hold for 5 s.*"
	      "back from hold: ANSam, V.8 as the call modem.*V.9[02] back "
	      "from hold" });
	/* V.92's PCM upstream as the analogue modem (DSP_V92A_UP=1):
	   INFO1a of Table 18, phase 3 (Ru, TRN1u, Ja; Su, Jp, the second
	   TRN1u during the DIL, CPt, E1u) and phase 4 (TRN2u, SUVu, CPu;
	   the digital modem's SUVd, CPd, Ed, B1d; E2u, B1u) on the PAM
	   transmitter, then the data both ways: the digital modem's
	   prefilter design (the default), and its plain precoder */
	add("open", "open_v92a_up", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_UP=1", "BULK=100000",
	      "EXPECT_A=(?s)V.92: PCM upstream phase 3, LU.*Jp: Su-bar.*"
	      "Jp' after.*V.92: CPt, drn.*PCM upstream phase 3 done.*"
	      "SUVd at frame.*V.92: CPu, drn.*CPd'? at frame.*"
	      "B1d: 0 bit errors.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)v92: Ru reversal at.*v92: Ja: DIL N 128.*"
	      "v92: Su reversal after Jp.*v92: CPt at symbol.*"
	      "v92: E1u at symbol.*v92: SUVu at symbol.*v92: CPu at symbol.*"
	      "prefilter of 64 taps.*v92: E2u at symbol.*"
	      "v92: B1u: 0 zeros.*V.92 connect 5[0-9]+/[34][0-9]+" });
	add("open", "open_v92a_up_precoder", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_UP=1", "DSP_V92_PREFILTER=0",
	      "BULK=100000",
	      "EXPECT_A=(?s)CPd'? at frame [0-9]+: [0-9]+ bit/s.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)CPd: [0-9]+ bit/s, M [0-9]+, [0-9]+ points, G "
	      "[0-9.]+ \\(mse [0-9.]+\\), precoding.*"
	      "v92: B1u: 0 zeros.*V.92 connect 5[0-9]+/[34][0-9]+" });
	/* V.92's quick connect as the analogue modem calling (9.2.1.1 and
	   9.4.2/V.92): QC1a before CM, the digital modem's QCA1d, ANSpcm
	   answered with TONEq, short phase 2 (no probing), PCM upstream;
	   without our QC1a (DSP_V92A_QC=0) V.8 and short phase 2; with a
	   digital modem that does not answer QC1a (DSP_V92_QC=0), V.8 */
	add("open", "open_v92a_qc", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "BULK=100000",
	      "EXPECT_A=(?s)V.92 QC1a.*QCA1d: quick connect.*"
	      "ANSpcm: TONEq.*ANSpcm ended.*V.92 short phase 2.*"
	      "PCM upstream phase 3 done.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)V.92 quick connect, UQTS 79.*TONEq heard.*"
	      "INFO0a V.92 1, short phase 2 1.*V.92 short phase 2.*"
	      "v92: B1u: 0 zeros.*V.92 connect 5[0-9]+/[34][0-9]+" });
	add("open", "open_v92a_qc_off", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_QC=0",
	      "EXPECT_A=(?s)V.8 JM received.*V.92 short phase 2.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)V.8 CM received.*short phase 2 1.*"
	      "V.92 connect 5[0-9]+/" });
	add("open", "open_v92a_qc_unanswered", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92_QC=0",
	      "EXPECT_A=(?s)V.92 QC1a.*V.8 JM received.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)V.8 CM received.*V.92 connect 5[0-9]+/" });
	/* PCM upstream's rate renegotiation (9.8.2/V.92): the digital
	   modem's (DSP_V92_RENEG on it: its Rd, our Ru, TRN2u, phase 4
	   again), ours (DSP_V90A_RENEG on us: our Ru first); its fast
	   parameter exchange (9.9.2.2/V.92, DSP_V92_FPE on it: its Rf, our
	   RM, RM', SUVu, CPus in the data mode, E2u, FB1u, B1u), with its
	   prefilter and with its plain precoder */
	add("open", "open_v92a_reneg", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92_RENEG=3000", "BULK=150000",
	      "EXPECT_A=(?s)V.92 connect [34][0-9]+/5[0-9]+ \\(analogue "
	      "modem\\).*Rd-bar at symbol.*V.92 renegotiation 1 \\(the "
	      "digital modem's Rd\\).*SUVd at frame.*CPd'? at frame.*"
	      "V.92 renegotiated [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)v92: Rd.*v92: Ru at symbol.*rate renegotiation.*"
	      "TRN2u \\(renegotiation\\).*v92: renegotiated" });
	add("open", "open_v92a_reneg_ours", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V90A_RENEG=3000c", "BULK=150000",
	      "EXPECT_A=(?s)V.92 connect.*V.92 renegotiation 1 \\(ours: "
	      "test\\).*Rd-bar at symbol.*"
	      "V.92 renegotiated [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)v92: Ru at symbol.*rate renegotiation.*v92: Rd.*"
	      "v92: renegotiated" });
	add("open", "open_v92a_fpe", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92_FPE=3000", "BULK=150000",
	      "EXPECT_A=(?s)V.92 connect.*Rf at frame.*Rf-bar at frame.*"
	      "FPE: SUVd from frame.*V.92 fast parameter exchange 1 \\(the "
	      "digital modem's Rf\\), the prefilter anew.*FPE: CPus.*"
	      "CPd'? at frame.*FPE: E2u, FB1u, B1u.*FPE: Ed at frame.*"
	      "B1d: 0 bit errors.*V.92 fast parameter exchange done "
	      "[34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)v92: Rf.*v92: RM at symbol.*FPE: SUVu from.*"
	      "CPus at symbol.*FPE: E2u.*FPE: B1u at.*"
	      "fast parameter exchange 1 done" });
	add("open", "open_v92a_fpe_precoder", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92_FPE=3000", "DSP_V92_PREFILTER=0",
	      "BULK=150000",
	      "EXPECT_A=(?s)V.92 fast parameter exchange 1 \\(the digital "
	      "modem's Rf\\), downstream.*V.92 fast parameter exchange done",
	      "EXPECT_B=(?s)v92: RM at symbol.*FPE: FB1u, [0-9] zeros.*"
	      "fast parameter exchange 1 done" });
	/* our own fast parameter exchange (9.9.2.1/V.92): RM, RM' and
	   SUVu until the digital modem's Rf, then a CPu designed on the
	   data mode's error, B1d on it */
	add("open", "open_v92a_fpe_ours", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_FPE=3000c", "BULK=150000",
	      "EXPECT_A=(?s)V.92 connect.*V.92 fast parameter exchange 1 "
	      "\\(ours: test\\).*Rf at frame.*Rf-bar at frame.*"
	      "FPE: SUVd from frame.*FPE: CPu, drn.*FPE: E2u, FB1u, B1u.*"
	      "FPE: Ed at frame.*B1d: 0 bit errors.*"
	      "V.92 fast parameter exchange done [34][0-9]+/5[0-9]+ "
	      "\\(analogue modem\\)",
	      "EXPECT_B=(?s)v92: RM at symbol.*"
	      "FPE: SUVu from.*CPu at symbol.*FPE: E2u.*FPE: B1u at.*"
	      "fast parameter exchange 1 done" });
	add("open", "open_v92a_fpe_ours_precoder", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_FPE=3000c",
	      "DSP_V92_PREFILTER=0", "BULK=150000",
	      "EXPECT_A=(?s)V.92 fast parameter exchange 1 \\(ours: test\\), "
	      "downstream.*FPE: CPu, drn.*B1d: 0 bit errors.*"
	      "V.92 fast parameter exchange done",
	      "EXPECT_B=(?s)v92: RM at symbol.*v92: RM' at symbol.*"
	      "fast parameter exchange 1 done" });
	/* its symbols on the network's clock as the PCM receiver finds it
	   (100 ppm off ours: half a symbol off by the second TRN1u's end
	   otherwise), answering on an A-law network, and its echo */
	add("open", "open_v92a_up_ppm", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_UP=1", "NET_PPM=100",
	      "NET_PHASE=0.3", "BULK=100000",
	      "EXPECT_A=(?s)timing \\+(99.[5-9]|100.[0-4])[0-9]* ppm.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=(?s)v92: Su reversal after Jp.*v92: CPt at symbol.*"
	      "v92: E1u at symbol [0-9]+, mse 0.00[0-4].*"
	      "v92: B1u: 0 zeros.*V.92 connect" });
	add("open", "open_v92a_up_answers", "open8", "open", "ATX3",
	    { "NET=8000:9600:alaw", "DSP_G711=alaw", "DSP_V92A_UP=1",
	      "BULK=100000",
	      "EXPECT_A=(?s)v92: E1u at symbol.*v92: B1u: 0 zeros.*"
	      "V.92 connect",
	      "EXPECT_B=(?s)PCM upstream phase 3 done.*V.92 connect "
	      "[34][0-9]+/5[0-9]+ \\(analogue modem\\)" });
	add("open", "open_v92a_up_echo", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V92A_UP=1", "DSP_TEST_ECHO=-10:1c",
	      "BULK=100000",
	      "EXPECT_A=(?s)echo canceller: least squares [4-9][0-9].[0-9] "
	      "dB.*DIL: pad 0.0 dB.*V.92 connect",
	      "EXPECT_B=(?s)v92: B1u: 0 zeros.*V.92 connect" });
	/* the analogue modem with its codec at 8000 Hz (MODEM_ANALOG: its
	   line an analogue one; netsim's NET_ANALOG), on the network's
	   clock: the PCM receiver on the line's own samples, with
	   DSP_V90A=1 (at 8000 Hz half a sample off the network's symbols
	   the band's top cancels, and a codec's clock of its own drifts
	   through that); at a phase of 0.25, and 0.1 answering on an A-law
	   network.  Without DSP_V90A=1 such a line is V.34's */
	add("open", "open_v90a_8k", "open8", "open8", "ATX3;AT+MS=90",
	    { "NET=8000:8000:ulaw", "NET_ANALOG=a", "MODEM_ANALOG=modemA",
	      "NET_PHASE=0.25", "DSP_V90A=1", "BULK=100000",
	      "EXPECT_A=(?s)V.90 with us as the analogue modem.*"
	      "B1d: 0 bit errors in 48 frames of 42 bits.*"
	      "V.90 connect 3[0-9]+/56000 \\(analogue modem\\).*"
	      "V.90 data: error [0-9].[0-9]+ rms",
	      "EXPECT_B=V.90 connect 56000/" });
	add("open", "open_v90a_8k_answers", "open8", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=8000:8000:alaw", "DSP_G711=alaw", "NET_ANALOG=b",
	      "MODEM_ANALOG=modemB", "NET_PHASE=0.1", "DSP_V90A=1",
	      "EXPECT_B=(?s)V.90 with us as the analogue modem.*"
	      "V.90 connect 3[0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_A=V.90 connect 5[0-9]+/" });
	add("open", "open_v90a_8k_off", "open8", "open8", "ATX3",
	    { "NET=8000:8000:ulaw", "NET_ANALOG=a", "MODEM_ANALOG=modemA",
	      "NET_PHASE=0.25",
	      "EXPECT_A=data rates tx [0-9]+ rx [0-9]+, trellis",
	      "EXPECT_B=data rates tx [0-9]+ rx [0-9]+, trellis" });
	/* PCM as the analogue modem is the 9600 Hz modem's default: both
	   modems with the default AT+MS (92) connect at V.92 with PCM
	   upstream; DSP_V90A=0 leaves it out, V.34 */
	add("open", "open_v90a_default", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw",
	      "EXPECT_A=(?s)V.90 with us as the analogue modem.*"
	      "V.92 connect [34][0-9]+/5[0-9]+ \\(analogue modem\\)",
	      "EXPECT_B=V.92 connect 5[0-9]+/" });
	add("open", "open_v90a_off", "open", "open8", "ATX3",
	    { "NET=9600:8000:ulaw", "DSP_V90A=0",
	      "EXPECT_A=data rates tx [0-9]+ rx [0-9]+, trellis" });
#ifdef INTEROP_HAVE_AMR
	/* --- two open modems (8000 Hz) through a mobile network's speech
	   codec (netsim's NET_CODEC): V.22 passes AMR at 12.2 kbit/s with
	   its data intact, V.21 AMR-WB at 23.85 (docs/cellular/v_cvm.pdf
	   has the whole picture) */
	add("open", "open_22_amr122", "open8", "open8", "ATX3;AT+MS=22",
	    { "NET=8000:8000:ulaw", "NET_CODEC=amr:12.2", "STREAM=10" });
	add("open", "open_21_amrwb2385", "open8", "open8", "ATX3;AT+MS=21",
	    { "NET=8000:8000:ulaw", "NET_CODEC=amrwb:23.85", "STREAM=10" });
	/* V.cvm (+MS=V_CVM): V.8 agrees on it, its start-up, then LAPM
	   over its data mode: a comb through AMR 12.2 and AMR-WB 23.85,
	   the parametric mode (or four tones) through AMR 7.4 */
	add("open", "open_vcvm_amr122", "open8", "open8", "ATX3;AT+MS=V_CVM",
	    { "NET=8000:8000:ulaw", "NET_CODEC=amr:12.2", "STREAM=10",
	      "EXPECT_A=(?s)V.8 agreed on V.cvm.*vcvm: phase 4: transmitting N[234]00",
	      "EXPECT_B=(?s)V.8 agreed on V.cvm.*vcvm: phase 4: transmitting N[234]00" });
	/* (175 to 350 bit/s: one line of the stream, not 3.5 kB) */
	add("open", "open_vcvm_amr74", "open8", "open8", "ATX3;AT+MS=V_CVM",
	    { "NET=8000:8000:ulaw", "NET_CODEC=amr:7.4", "STREAM=0.5",
	      "EXPECT_A=vcvm: phase 4", "EXPECT_B=vcvm: phase 4" });
	add("open", "open_vcvm_amrwb2385", "open8", "open8", "ATX3;AT+MS=V_CVM",
	    { "NET=8000:8000:ulaw", "NET_CODEC=amrwb:23.85", "STREAM=10",
	      "EXPECT_A=vcvm: phase 4", "EXPECT_B=vcvm: phase 4" });
#endif
	/* V.cvm on a line without a codec (G.711 alone) */
	add("open", "open_vcvm", "open8", "open8", "ATX3;AT+MS=V_CVM",
	    { "NET=8000:8000:ulaw", "STREAM=10",
	      "EXPECT_A=(?s)V.8 agreed on V.cvm.*vcvm: phase 4: transmitting N200",
	      "EXPECT_B=vcvm: phase 4: transmitting N200" });

	/* --- the Agere soft modem (8000 Hz) and the open modem (9600 Hz)
	   call each other; AT+MS takes names there (B103, V21, ..., V34) */
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "B103", "103" }, { "V21", "21" }, { "V23C", "23" },
		     { "B212", "212" }, { "V22", "22" }, { "V22B", "122" },
		     { "V32", "32" }, { "V32B", "132" }, { "V34", "34" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		add("agere", std::string("agere_") + ms + "_agere_calls",
		    "agere", "open", a,
		    { "NET=8000:9600:ulaw", "PROTO_B=" + o });
		add("agere", std::string("agere_") + ms + "_open_calls", "open",
		    "agere", o, { "NET=9600:8000:ulaw", "PROTO_B=" + a });
	}
	/* V.44 both ways (the Agere modem offers it): 60000 octets each way
	   of text, binary, repeats and noise, which it sends in transparent
	   mode now and then */
	add("agere", "agere_v44_agere_calls", "agere", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34", "BULK=60000" });
	add("agere", "agere_v44_open_calls", "open", "agere", "ATX3;AT+MS=34",
	    { "NET=9600:8000:ulaw", "PROTO_B=ATX3;AT+MS=V34", "BULK=60000" });
	/* V.34 rate renegotiation started by the open modem, both ways; and
	   the line much noisier 10 s after CONNECT: a retrain */
	add("agere", "agere_34_reneg_agere_calls", "agere", "open",
	    "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "NET_SYNC=1", "STREAM=30",
	      "DSP_V34_RENEG=5000:8", "PROTO_B=ATX3;AT+MS=34" });
	add("agere", "agere_34_reneg_open_calls", "open", "agere",
	    "ATX3;AT+MS=34",
	    { "NET=9600:8000:ulaw", "NET_SYNC=1", "STREAM=30",
	      "DSP_V34_RENEG=5000:8", "PROTO_B=ATX3;AT+MS=V34" });
	add("agere", "agere_34_noise", "agere", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "NET_SYNC=1", "STREAM=40",
	      "NET_NOISE=10:300", "PROTO_B=ATX3;AT+MS=34" });
	/* phase 2's recovery (11.2.2/V.34): our INFO0a lost
	   (DSP_V34_LOSE_INFO0, our first INFO0 with a bad CRC), the Agere
	   modem repeats its INFO0c, and we ours */
	add("agere", "agere_34_lose_info0", "agere", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "DSP_V34_LOSE_INFO0=1",
	      "EXPECT_B=INFO0 again \\(the remote has not taken ours\\)",
	      "PROTO_B=ATX3;AT+MS=34" });
	/* V.92 quick connect: the Agere modem (+PQC=0) remembers the line
	   of the first call and starts the second with QC1a, and short
	   phase 2 (in real time its timing did not take a busy machine; on
	   the line's clock any machine does).  NET_LOOP: our PCM signal
	   reaches it through an analogue loop (on an ideal line, the same
	   samples as ours, its V.90 training fails at some delays, every
	   48 samples); not half a sample: then our sample instants fall
	   exactly between its samples, and at some alignments
	   (AGERE_SYNC_PHASE 96, 288) it takes 33333 bit/s downstream for
	   53333 */
	add("agere", "agere_v92_qc", "agere", "open8",
	    "ATX3;AT+MS=V92;AT+PQC=0",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "CALLS=2", "DIAL=5551234", "ANSWER_AFTER=3", "STREAM=3",
	      "EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2",
	      "PROTO_B=ATX3;AT+MS=92" });
	/* V.90: the Agere modem as the analogue modem, the open one as the
	   digital modem (both at 8000 Hz) */
	for (std::string law : { "ulaw", "alaw" })
		add("agere", "agere_v90_" + law, "agere", "open8",
		    "ATX3;AT+MS=V90",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law,
		      "DSP_G711=" + law, "PROTO_B=ATX3;AT+MS=90" });
	/* V.92 modem-on-hold: in the middle of the data the Agere modem
	   asks for it (AT+PMHR), we grant 1 min (MHack), it comes back
	   after 10 s (ATO): V.8, phase 2 to 4, and the data goes on (V.42
	   knows nothing of it); denied (MHnack), it asks for a fast
	   reconnect (MHfrr) */
	add("agere", "agere_v92_moh_grant", "agere", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=50", "MIDCALL_A=8:AT+PMHR;WAIT 10",
	      "EXPECT_B=(?s)MHack.*V.90 back from hold",
	      "PROTO_B=ATX3;AT+MS=92" });
	add("agere", "agere_v92_moh_deny", "agere", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=40", "DSP_V92_MOH=0", "MIDCALL_A=8:AT+PMHR;WAIT 5",
	      "EXPECT_B=(?s)MHnack.*MHfrr.*V.90 back from hold",
	      "PROTO_B=ATX3;AT+MS=92" });
	/* and ours (AT+PMHR on our side): the Agere modem takes our tone B
	   for a retrain and reverses its tone A; we follow, and the data
	   goes on after the retrain */
	add("agere", "agere_v92_moh_request", "agere", "open8",
	    "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=40", "MIDCALL_B=8:AT+PMHR;WAIT 6",
	      "EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.90 "
	      "retrained", "PROTO_B=ATX3;AT+MS=92" });
	/* at the alignment that took 33333 bit/s with half a sample of
	   delay */
	add("agere", "agere_v90_phase96", "agere", "open8", "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "AGERE_SYNC_PHASE=96", "NET=8000:8000:ulaw",
	      "DSP_G711=ulaw", "EXPECT_B=V.90 connect 5[0-9]{4}/",
	      "PROTO_B=ATX3;AT+MS=90" });
	/* V.90 rate renegotiation: ours to 19 200 bit/s upstream (the Agere
	   modem's own, as noise on the PCM codes rises, come too late for a
	   test, if at all) */
	add("agere", "agere_v90_reneg_open", "agere", "open8",
	    "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=15", "DSP_V34_RENEG=400:8",
	      "EXPECT_B=V.90 renegotiated [0-9]+/19200",
	      "PROTO_B=ATX3;AT+MS=90" });
	/* class 1 fax both ways at 8000 Hz */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145 }) {
		std::string s = std::to_string(m);
		add("agere", "agere_fax_" + s + "_agere_sends", "agere", "open8",
		    "ATX3")->fax = m;
		add("agere", "agere_fax_" + s + "_open_sends", "open8", "agere",
		    "ATX3")->fax = m;
	}

	/* --- spandsp's modem (8000 Hz) and the open modem (9600 Hz) call
	   each other at each modulation it has (its own label: CI's
	   "spandsp" are quick) */
	for (const char *ms : { "21", "22", "122", "23", "103" }) {
		std::string p = std::string("ATX3;AT+MS=") + ms;
		add("spanpeer", std::string("spandsp_") + ms + "_spandsp_calls",
		    "spandsp", "open", p,
		    { "NET=8000:9600:ulaw", "PROTO_B=" + p });
		add("spanpeer", std::string("spandsp_") + ms + "_open_calls",
		    "open", "spandsp", p,
		    { "NET=9600:8000:ulaw", "PROTO_B=" + p });
	}

	/* --- the Motorola SM56 soft modem (8000 Hz) and the open modem
	   (9600 Hz) call each other, as the Agere modem's tests.  (Its
	   automode is off below V.34: +MS=V22 alone is an error.  Calling
	   at V.22bis it is in automode: forced to V.22bis it takes the
	   answer tone for unscrambled ones, see docs/interop.md) */
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "V21,0", "21" }, { "V22,0", "22" }, { "V22B,0", "122" },
		     { "V32B,0", "132" }, { "V34", "34" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		add("sm56", std::string("sm56_") + ms + "_sm56_calls", "sm56",
		    "open", strcmp(ms, "122") ? a : "ATX3;AT+MS=V92",
		    { "NET=8000:9600:ulaw", "PROTO_B=" + o });
		add("sm56", std::string("sm56_") + ms + "_open_calls", "open",
		    "sm56", o, { "NET=9600:8000:ulaw", "PROTO_B=" + a });
	}
	/* V.90: the SM56 modem as the analogue modem, the open one as the
	   digital modem (both at 8000 Hz; the SM56 modem's codec runs at
	   9600 Hz for it) */
	for (std::string law : { "ulaw", "alaw" })
		add("sm56", "sm56_v90_" + law, "sm56", "open8", "ATX3;AT+MS=V90",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law,
		      "DSP_G711=" + law, "EXPECT_B=V.90 connect 5[0-9]{4}/",
		      "PROTO_B=ATX3;AT+MS=90" });
	/* V.92: its QC1a on the first call already; on the second, quick
	   connect with short phase 2.  (Its upstream stays V.34: the module
	   chooses PCM upstream only in quick connect's INFO1a, from an
	   INFO1d flag its full phase 2 has cleared.)  Without a retrain:
	   its INFO1a says high carrier, but it sends on the low one */
	add("sm56", "sm56_v92_qc", "sm56", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "CALLS=2", "DIAL=5551234", "ANSWER_AFTER=3", "STREAM=3",
	      "EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2.*V.90 "
	      "connect", "REJECT_B=(?s)V.92 short phase 2.*retrain",
	      "PROTO_B=ATX3;AT+MS=92" });
	/* V.92 modem-on-hold: it asks (AT+PMHR), we grant it; it comes back
	   with quick connect (a while after its ATO) */
	add("sm56", "sm56_v92_moh_grant", "sm56", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=60", "MIDCALL_A=8:AT+PMHR;WAIT 10",
	      "EXPECT_B=(?s)MHreq: MHack.*V.92 quick connect.*V.90 back "
	      "from hold", "PROTO_B=ATX3;AT+MS=92" });
	/* and ours: as the Agere modem, it takes our tone B for a retrain's
	   (it answers MHreq only where its phase 2 begins, in place of
	   INFO0d, and one that knows nothing of it is no different until
	   then) */
	add("sm56", "sm56_v92_moh_request", "sm56", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=40", "MIDCALL_B=8:AT+PMHR;WAIT 6",
	      "EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.90 "
	      "retrained", "PROTO_B=ATX3;AT+MS=92" });

	/* --- Broadcom's V.92 soft modem for Windows (its WDM driver on
	   interop_windows.cpp's kernel and half_broadcom's BCM4212, 8000 Hz)
	   and the open modem (9600 Hz) call each other at each modulation */
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "B103", "103" }, { "V21", "21" }, { "V23C", "23" },
		     { "B212", "212" }, { "V22", "22" }, { "V22B", "122" },
		     { "V32", "32" }, { "V32B", "132" }, { "V34", "34" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		add("broadcom", std::string("broadcom_") + ms + "_broadcom_calls",
		    "broadcom", "open", a, { "NET=8000:9600:ulaw", "PROTO_B=" + o });
		add("broadcom", std::string("broadcom_") + ms + "_open_calls",
		    "open", "broadcom", o, { "NET=9600:8000:ulaw", "PROTO_B=" + a });
	}
	/* V.44 both ways (it offers it): 60000 octets each way */
	add("broadcom", "broadcom_v44_broadcom_calls", "broadcom", "open",
	    "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34", "BULK=60000" });
	add("broadcom", "broadcom_v44_open_calls", "open", "broadcom",
	    "ATX3;AT+MS=34",
	    { "NET=9600:8000:ulaw", "PROTO_B=ATX3;AT+MS=V34", "BULK=60000" });
	/* its DAA an Si3034 (BROADCOM_DAA=3034): the driver sets up the
	   international registers and calls as before; off hook its
	   register 18 is 0x03 (an Si3035's 0x01) */
	add("broadcom", "broadcom_si3034_broadcom_calls", "broadcom", "open",
	    "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34", "BROADCOM_DAA=3034",
	      "BROADCOM_DEBUG=1", "EXPECT_A=daa 18 w = 03" });
	/* V.90: the Broadcom modem as the analogue modem, the open one as the
	   digital modem */
	for (std::string law : { "ulaw", "alaw" })
		add("broadcom", "broadcom_v90_" + law, "broadcom", "open8",
		    "ATX3;AT+MS=V90",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law, "DSP_G711=" + law,
		      "EXPECT_B=V.90 connect 5[0-9]{4}/", "PROTO_B=ATX3;AT+MS=90" });
	/* V.92: quick connect with short phase 2 on the second call */
	add("broadcom", "broadcom_v92_qc", "broadcom", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "CALLS=2", "DIAL=5551234", "ANSWER_AFTER=3", "STREAM=3",
	      "EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2.*V.90 "
	      "connect", "PROTO_B=ATX3;AT+MS=92" });
	/* PCM upstream (+PIG=0; off by default): its Su is 96T before
	   Su-bar, and phases 3 and 4 get through; it sends no B1u after
	   E2u, and after a retrain the call is V.90 */
	add("broadcom", "broadcom_v92_pcmup", "broadcom", "open8",
	    "ATX3;AT+PIG=0;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "EXPECT_B=(?s)Su-bar reversal.*E2u at symbol.*V.90 connect "
	      "5[0-9]{4}/", "PROTO_B=ATX3;AT+MS=92" });
	/* class 1 fax both ways at 8000 Hz */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145 }) {
		std::string s = std::to_string(m);
		add("broadcom", "broadcom_fax_" + s + "_broadcom_sends", "broadcom",
		    "open8", "ATX3")->fax = m;
		add("broadcom", "broadcom_fax_" + s + "_open_sends", "open8",
		    "broadcom", "ATX3")->fax = m;
	}

	/* --- the PCTel HSP soft modem (8000 Hz) and the open modem (9600
	   Hz): V.32bis and V.34 both ways with its +MS set to them */
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "V32B,0", "132" }, { "V34", "34" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		add("pctel", std::string("pctel_") + ms + "_pctel_calls",
		    "pctel", "open", a, { "NET=8000:9600:ulaw", "PROTO_B=" + o });
		add("pctel", std::string("pctel_") + ms + "_open_calls", "open",
		    "pctel", o, { "NET=9600:8000:ulaw", "PROTO_B=" + a });
	}
	/* V.90: the PCTel modem as the analogue modem (its codec at 72000 / 7
	   Hz, locked to the line, see half_pctel.cpp).  +MS=V90 with its top
	   rate: without one (0) it takes the top rate for less than 28000
	   and keeps the downstream to 45333 */
	for (std::string law : { "ulaw", "alaw" })
		add("pctel", "pctel_v90_" + law, "pctel", "open8",
		    "ATX3;AT+MS=V90,1,300,56000",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law,
		      "DSP_G711=" + law, "STREAM=5",
		      "EXPECT_B=V.90 connect 5[0-9]{4}/",
		      "PROTO_B=ATX3;AT+MS=90" });
	/* our INFO0d lost (DSP_V34_LOSE_INFO0): the PCTel modem repeats its
	   INFO0a, and we INFO0d (9.2.1.2/V.90).  It misses ours by itself
	   at some starts of the call (the noise its codec gets with it) */
	add("pctel", "pctel_v90_lose_info0", "pctel", "open8",
	    "ATX3;AT+MS=V90,1,300,56000",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "DSP_V34_LOSE_INFO0=1",
	      "EXPECT_B=(?s)INFO0 again \\(the remote has not taken "
	      "ours\\).*V.90 connect 5[0-9]{4}/",
	      "PROTO_B=ATX3;AT+MS=90" });
	/* two calls (PCTel answers ATH with NO CARRIER, and wants more than
	   2 s on hook before the next): V.90 again */
	add("pctel", "pctel_v90_calls", "pctel", "open8",
	    "ATX3;AT+MS=V90,1,300,56000",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=3", "CALLS=2", "HANGUP_WAIT=5",
	      "EXPECT_B=(?s)V.90 connect 5[0-9]{4}/.*V.90 connect 5[0-9]{4}/",
	      "PROTO_B=ATX3;AT+MS=90" });
	/* and in its automode (no +MS: forced to one of these its answerer
	   is silent after its answer tone), V.21, V.22, V.22bis and V.23
	   both ways against our forced ones */
	for (const char *ms : { "21", "22", "122", "23" }) {
		std::string o = std::string("ATX3;AT+MS=") + ms;
		const char *rate = !strcmp(ms, "21") ? "DATA_RATE=300" :
			!strcmp(ms, "23") ? "DATA_RATE=75" : "DATA_RATE=1200";
		add("pctel", std::string("pctel_") + ms + "_open_calls", "open",
		    "pctel", o, { "NET=9600:8000:ulaw", rate, "PROTO_B=ATX3" });
		add("pctel", std::string("pctel_auto_") + ms + "_pctel_calls",
		    "pctel", "open", "ATX3",
		    { "NET=8000:9600:ulaw", rate, "PROTO_B=" + o });
	}

	/* --- the HSF soft modem (8000 Hz) and the open modem (9600 Hz):
	   V.22bis, V.32bis and V.34 both ways and V.21 with the HSF modem
	   calling; V.90 and V.92 (PCM upstream)
	   with it as the analogue modem (see docs/interop.md for the calls that
	   fail) */
	struct Hsf {
		const char *n, *ms;
		bool hsf_calls, open_calls;
	};
	for (auto h : { Hsf{ "V21,0", "21", true, false },
			Hsf{ "V22B", "122", true, true },
			Hsf{ "V32B,0", "132", true, true },
			Hsf{ "V34", "34", true, true } }) {
		std::string a = std::string("ATX3;AT+MS=") + h.n;
		std::string o = std::string("ATX3;AT+MS=") + h.ms;
		if (h.hsf_calls)
			add("hsf", std::string("hsf_") + h.ms + "_hsf_calls",
			    "hsf", "open", a,
			    { "NET=8000:9600:ulaw", "PROTO_B=" + o });
		if (h.open_calls)
			add("hsf", std::string("hsf_") + h.ms + "_open_calls",
			    "open", "hsf", o,
			    { "NET=9600:8000:ulaw", "PROTO_B=" + a });
	}
	for (std::string law : { "ulaw", "alaw" })
		add("hsf", "hsf_v90_" + law, "hsf", "open8", "ATX3;AT+MS=V90",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law,
		      "DSP_G711=" + law, "EXPECT_B=V.90 connect 5[0-9]{4}/",
		      "PROTO_B=ATX3;AT+MS=90" });
	/* our INFO0d lost (DSP_V34_LOSE_INFO0): the HSF modem takes it all
	   the same (its INFO0a acknowledges it) but repeats its INFO0a
	   until ours acknowledges that (9.2.1.2/V.90) */
	add("hsf", "hsf_v90_lose_info0", "hsf", "open8", "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "DSP_V34_LOSE_INFO0=1",
	      "EXPECT_B=(?s)INFO0 again \\(the remote's acknowledges "
	      "ours\\).*V.90 connect 5[0-9]{4}/",
	      "PROTO_B=ATX3;AT+MS=90" });
	/* V.8 (the open modem with AT+MS=34) agreeing on the HSF modem's
	   modulation (its automode up to it), both ways (its V.21 and V.23
	   answerers work only so: forced, +MS=V21,0, it is silent, and in
	   automode it gives up on a caller without V.8) */
	for (auto [n, ms, rate] : std::vector<std::tuple<const char *,
		     const char *, const char *>>{ { "V21", "21", "300" },
		     { "V23C", "23", "75" }, { "V22B", "122", "2400" },
		     { "V32B", "132", "14400" } })
	{
		add("hsf", std::string("hsf_v8_") + ms + "_open_calls", "open",
		    "hsf", "ATX3;AT+MS=34",
		    { "NET=9600:8000:ulaw", std::string("DATA_RATE=") + rate,
		      std::string("PROTO_B=ATX3;AT+MS=") + n });
		/* and its CM, up to that modulation, to our answerer */
		add("hsf", std::string("hsf_v8_") + ms + "_hsf_calls", "hsf",
		    "open", std::string("ATX3;AT+MS=") + n,
		    { "NET=8000:9600:ulaw", std::string("DATA_RATE=") + rate,
		      "PROTO_B=ATX3;AT+MS=34",
		      std::string("EXPECT_B=V.8 agreed on ") +
		      (!strcmp(ms, "21") ? "V.21" : !strcmp(ms, "23") ? "V.23" :
		       !strcmp(ms, "122") ? "V.22bis" : "V.32bis") });
	}
	/* our forced low speed callers against its answerer in automode up
	   to their modulation: V.21 through V.8 on its ANSam (it answers
	   V.21 only so), V.23, V.22 and V.22bis without */
	for (auto [n, ms, rate] : std::vector<std::tuple<const char *,
		     const char *, const char *>>{ { "V21", "21", "300" },
		     { "V23C", "23", "75" }, { "V22", "22", "1200" },
		     { "V22B", "122", "2400" } })
		add("hsf", std::string("hsf_low_") + ms + "_open_calls", "open",
		    "hsf", std::string("ATX3;AT+MS=") + ms,
		    { "NET=9600:8000:ulaw", std::string("DATA_RATE=") + rate,
		      std::string("PROTO_B=ATX3;AT+MS=") + n });
	/* V.92 with PCM upstream: it starts with quick connect (QC1a) on
	   the first call already; and with it off (+PQC=3), V.8 */
	for (std::string law : { "ulaw", "alaw" })
		add("hsf", "hsf_v92_pcmup_" + law, "hsf", "open8",
		    "ATX3;AT+MS=V92",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law,
		      "DSP_G711=" + law, "STREAM=10",
		      "EXPECT_B=(?s)V.92 quick connect.*V.92 connect "
		      "5[0-9]{4}/(3[4-9]|4[0-2])", "PROTO_B=ATX3;AT+MS=92" });
	/* it has no fast parameter exchange (no RM to our Rf): a
	   renegotiation instead */
	add("hsf", "hsf_v92_fpe", "hsf", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=10", "DSP_V92_FPE=3000", "DSP_V92_SLIP=0",
	      "EXPECT_B=(?s)no RM: no fast parameter exchange.*V.92 "
	      "renegotiated", "PROTO_B=ATX3;AT+MS=92" });
	/* the echo of our signal from the hybrid at its line (8000 Hz,
	   before the network codec): our echo canceller takes it away,
	   and the PCM upstream design counts with the codec's
	   quantization of it */
	add("hsf", "hsf_v92_echo", "hsf", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "NET_ECHO=-10:50", "STREAM=10",
	      "EXPECT_B=(?s)far echo at.*echo canceller took [23][0-9]\\.[0-9] "
	      "dB away\n.*V.92 connect 5[0-9]{4}/[23][0-9]{4}",
	      "PROTO_B=ATX3;AT+MS=92" });
	add("hsf", "hsf_v90_echo", "hsf", "open8", "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "NET_ECHO=-20:50", "STREAM=10",
	      "EXPECT_B=(?s)far echo at.*echo canceller took [23][0-9]\\.[0-9] "
	      "dB away\n.*V.90 (connect|retrained,) 5[0-9]{4}/",
	      "PROTO_B=ATX3;AT+MS=90" });
	add("hsf", "hsf_v92_pcmup_noqc", "hsf", "open8",
	    "ATX3;AT+MS=V92;AT+PQC=3",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=10", "EXPECT_B=V.92 connect 5[0-9]{4}/(3[4-9]|4[0-2])",
	      "PROTO_B=ATX3;AT+MS=92" });

	/* --- ESS's V.92 soft modem for Windows (its WDM driver on
	   interop_windows.cpp's kernel and half_ess's ES2838, 8000 Hz) and
	   the open modem (9600 Hz) call each other at each modulation.  It
	   says CONNECT a little before it receives (the first octets of
	   an FSK call lost), so the data waits a second (DATA_WAIT) */
	const std::string ess_wait = "DATA_WAIT=1";
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "B103", "103" }, { "V21", "21" }, { "V23C", "23" },
		     { "V22", "22" }, { "V22B", "122" }, { "V32", "32" },
		     { "V32B", "132" }, { "V34", "34" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		std::string m = ms;
		std::string rate = m == "103" || m == "21" ? "DATA_RATE=300" :
			m == "23" ? "DATA_RATE=75" : m == "22" ? "DATA_RATE=1200" : "";
		std::vector<std::string> ee = { "NET=8000:9600:ulaw", "PROTO_B=" + o,
						ess_wait };
		std::vector<std::string> eo = { "NET=9600:8000:ulaw", "PROTO_B=" + a,
						ess_wait };
		if (!rate.empty()) {
			ee.push_back(rate);
			eo.push_back(rate);
		}
		/* (as the Bell 103 caller it takes no answer tone already on
		   the line as it dials: the answer a while after, as after
		   rings) */
		if (m == "103") {
			ee.push_back("DIAL=5551234");
			ee.push_back("ANSWER_AFTER=2");
		}
		add("ess", std::string("ess_") + ms + "_ess_calls", "ess",
		    "open", a, ee);
		add("ess", std::string("ess_") + ms + "_open_calls", "open", "ess",
		    o, eo);
	}
	/* in their automodes (V.8: V.34), and V.44 both ways (it offers
	   it): 60000 octets each way */
	add("ess", "ess_auto_ess_calls", "ess", "open", "ATX3",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3",
	      "EXPECT_B=(?s)V.8 CM received.*v34: connect 3[0-9]{4}/" });
	add("ess", "ess_v44_ess_calls", "ess", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34", "BULK=60000",
	      "EXPECT_B=negotiated: compression 3 \\(V.44\\)" });
	add("ess", "ess_v44_open_calls", "open", "ess", "ATX3;AT+MS=34",
	    { "NET=9600:8000:ulaw", "PROTO_B=ATX3;AT+MS=V34", "BULK=60000",
	      "EXPECT_A=negotiated: compression 3 \\(V.44\\)" });
	/* V.90: the ESS modem as the analogue modem, the open one as the
	   digital modem */
	for (std::string law : { "ulaw", "alaw" })
		add("ess", "ess_v90_" + law, "ess", "open8", "ATX3;AT+MS=V90",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law, "DSP_G711=" + law,
		      "EXPECT_B=V.90 connect 5[0-9]{4}/", "PROTO_B=ATX3;AT+MS=90" });
	/* V.92 as it comes (V.90: it leaves PCM upstream out), and with PCM
	   upstream (+PIG=0), both laws: on our prefilter (its INFO1a has p1
	   and z2 alone, our CPd no z1 or p2; 2M points at least, for the
	   trellis frame's 4th symbol), some 41333 upstream */
	add("ess", "ess_v92", "ess", "open8", "ATX3",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "EXPECT_B=V.9[02] connect [45][0-9]{4}/", "PROTO_B=ATX3" });
	add("ess", "ess_v92_pcmup", "ess", "open8", "ATX3;AT+PIG=0",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "EXPECT_B=(?s)prefilter of 64 taps.*V.92 connect "
	      "[45][0-9]{4}/4[0-9]{4}\n", "PROTO_B=ATX3" });
	/* V.34 rate renegotiation started by us, both ways; a much noisier
	   line 10 s in; our INFO0a lost */
	add("ess", "ess_34_reneg_ess_calls", "ess", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "NET_SYNC=1", "STREAM=30",
	      "DSP_V34_RENEG=5000:8", "PROTO_B=ATX3;AT+MS=34" });
	add("ess", "ess_34_reneg_open_calls", "open", "ess", "ATX3;AT+MS=34",
	    { "NET=9600:8000:ulaw", "NET_SYNC=1", "STREAM=30",
	      "DSP_V34_RENEG=5000:8", "PROTO_B=ATX3;AT+MS=V34" });
	add("ess", "ess_34_noise", "ess", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "NET_SYNC=1", "STREAM=40",
	      "NET_NOISE=10:300", "PROTO_B=ATX3;AT+MS=34" });
	add("ess", "ess_34_lose_info0", "ess", "open", "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "DSP_V34_LOSE_INFO0=1",
	      "EXPECT_B=INFO0 again \\(the remote has not taken ours\\)",
	      "PROTO_B=ATX3;AT+MS=34" });
	/* V.90: our INFO0d lost; our renegotiation (after its own, to
	   26400, at the start: it may renegotiate again every half second
	   or so, and our test's time counts from the last); echo */
	add("ess", "ess_v90_lose_info0", "ess", "open8", "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "DSP_V34_LOSE_INFO0=1",
	      "EXPECT_B=(?s)INFO0 again \\(the remote has not taken ours\\).*"
	      "V.90 connect 5[0-9]{4}/", "PROTO_B=ATX3;AT+MS=90" });
	add("ess", "ess_v90_reneg_open", "ess", "open8", "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=15", "DSP_V34_RENEG=400:8",
	      "EXPECT_B=V.90 renegotiated [0-9]+/19200",
	      "PROTO_B=ATX3;AT+MS=90" });
	add("ess", "ess_v90_echo", "ess", "open8", "ATX3;AT+MS=V90",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "NET_ECHO=-20:50", "STREAM=10",
	      "EXPECT_B=(?s)far echo at.*echo canceller took [23][0-9]\\.[0-9] "
	      "dB away\n.*V.90 connect [45][0-9]{4}/",
	      "PROTO_B=ATX3;AT+MS=90" });
	/* V.92: our modem-on-hold request (it takes our tone B for a
	   retrain's, as the Agere modem).  It offers V.92 in INFO0a but
	   neither short phase 2 nor quick connect (+PQC=0 too), and its
	   own AT+PMHR needs the +++ escape, after which (its OK given) it
	   parses no command (ATO goes out as data): no tests of those.
	   With PCM upstream (+PIG=0), as the echo's */
	add("ess", "ess_v92_moh_request", "ess", "open8",
	    "ATX3;AT+MS=V92;AT+PIG=0",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=40", "MIDCALL_B=8:AT+PMHR;WAIT 6",
	      "EXPECT_B=(?s)modem-on-hold request.*retrains instead.*"
	      "retrained", "PROTO_B=ATX3;AT+MS=92" });
	add("ess", "ess_v92_echo", "ess", "open8",
	    "ATX3;AT+MS=V92;AT+PIG=0",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "NET_ECHO=-10:50", "STREAM=10",
	      "EXPECT_B=(?s)far echo at.*echo canceller took [23][0-9]\\.[0-9] "
	      "dB away\n.*V.92 connect [45][0-9]{4}/[34][0-9]{4}\n",
	      "PROTO_B=ATX3;AT+MS=92" });
	add("ess", "ess_v92_pcmup_alaw", "ess", "open8", "ATX3;AT+PIG=0",
	    { "NET_LOOP=0.45", "NET=8000:8000:alaw", "DSP_G711=alaw",
	      "EXPECT_B=(?s)prefilter of 64 taps.*V.92 connect "
	      "[45][0-9]{4}/4[0-9]{4}\n", "PROTO_B=ATX3" });
	/* V.8 agreeing on a lower modulation, both ways; our forced low
	   speed callers against its automode */
	for (auto [n, ms, rate] : std::vector<std::tuple<const char *,
		     const char *, const char *>>{ { "V21", "21", "300" },
		     { "V23C", "23", "75" }, { "V22B", "122", "2400" },
		     { "V32B", "132", "14400" } }) {
		add("ess", std::string("ess_v8_") + ms + "_open_calls", "open",
		    "ess", "ATX3;AT+MS=34",
		    { "NET=9600:8000:ulaw", std::string("DATA_RATE=") + rate,
		      ess_wait, std::string("PROTO_B=ATX3;AT+MS=") + n });
		add("ess", std::string("ess_v8_") + ms + "_ess_calls", "ess",
		    "open", std::string("ATX3;AT+MS=") + n,
		    { "NET=8000:9600:ulaw", std::string("DATA_RATE=") + rate,
		      ess_wait, "PROTO_B=ATX3;AT+MS=34" });
	}
	for (auto [ms, rate] : std::vector<std::pair<const char *, const char *>>{
		     { "21", "300" }, { "23", "75" }, { "22", "1200" },
		     { "122", "2400" } })
		add("ess", std::string("ess_low_") + ms + "_open_calls", "open",
		    "ess", std::string("ATX3;AT+MS=") + ms,
		    { "NET=9600:8000:ulaw", std::string("DATA_RATE=") + rate,
		      ess_wait, "PROTO_B=ATX3" });
	/* MNP 4 alone (our %C2), it calling.  Answering with MNP
	   (+ES=3,0,6) it often takes our direction no higher than 14400
	   (at some answer timings 7200) and retrains up soon after (as an
	   MNP 10 modem does); with MNP 5 the link goes on through it, with
	   MNP 4 alone it gives its DTE no CONNECT after it: not that way */
	add("ess", "ess_mnp4_ess_calls", "ess", "open",
	    "ATX3;AT+MS=V34;AT+ES=4,4,6",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34;AT%C2",
	      "BULK=20000", "EXPECT_B=optimization 3\\.\n" });
	/* class 1 fax at 8000 Hz with it receiving.  As the sender (the
	   caller) it takes the answerer's DIS but no AT+FTH=3 after it (as
	   after +++, no command parsed once the frames' OK is given; with
	   the Agere and Broadcom answerers too): no calls that way */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145, 146 }) {
		std::string s = std::to_string(m);
		add("ess", "ess_fax_" + s + "_open_sends", "open8", "ess",
		    "ATX3")->fax = m;
	}

	/* --- Intel's 537EP soft modem, its line at 9600 Hz (its codec's
	   rates are 7200 to 16000 Hz, 9600 by default, 72000 / 7 for V.34,
	   and its V.90 is better without the resampling to 8000), and the
	   open modem (9600 Hz): V.34, V.32bis, V.22bis, V.22, V.21, V.23 and
	   Bell 103 both ways, each forced */
	const std::string i537 = "INTEL_RATE=9600";
	for (auto [n, ms, rate] : std::vector<std::tuple<const char *,
		     const char *, const char *>>{
		     { "V34", "34", "" }, { "V32B", "132", "" },
		     { "V22B", "122", "" }, { "V22", "22", "DATA_RATE=1200" },
		     { "V21", "21", "DATA_RATE=300" },
		     { "V23C", "23", "DATA_RATE=75" },
		     { "BELL103", "103", "DATA_RATE=300" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		std::vector<std::string> e = { i537, "NET=9600:9600:ulaw" };
		if (*rate)
			e.push_back(rate);
		auto ei = e, eo = e;
		ei.push_back("PROTO_B=" + o);
		eo.push_back("PROTO_B=" + a);
		add("intel", std::string("intel_") + ms + "_intel_calls", "intel",
		    "open", a, ei);
		add("intel", std::string("intel_") + ms + "_open_calls", "open",
		    "intel", o, eo);
	}
	/* in their automodes (V.8: V.34) with V.42bis, and V.44 (it
	   offers it) both ways, 60000 octets each way */
	add("intel", "intel_auto_intel_calls", "intel", "open", "ATX3",
	    { i537, "NET=9600:9600:ulaw", "PROTO_B=ATX3",
	      "EXPECT_B=(?s)V.8 CM received.*v34: connect 3[0-9]{4}/" });
	add("intel", "intel_v44_intel_calls", "intel", "open", "ATX3;AT+MS=V34",
	    { i537, "NET=9600:9600:ulaw", "PROTO_B=ATX3;AT+MS=34",
	      "BULK=60000", "EXPECT_B=negotiated: compression 3 \\(V.44\\)" });
	add("intel", "intel_v44_open_calls", "open", "intel", "ATX3;AT+MS=34",
	    { i537, "NET=9600:9600:ulaw", "PROTO_B=ATX3;AT+MS=V34",
	      "BULK=60000", "EXPECT_A=negotiated: compression 3 \\(V.44\\)" });
	/* V.90 with it as the analogue modem, both laws */
	for (std::string law : { "ulaw", "alaw" })
		add("intel", "intel_v90_" + law, "intel", "open8",
		    "ATX3;AT+MS=V90",
		    { i537, "NET_LOOP=0.45", "NET=9600:8000:" + law,
		      "DSP_G711=" + law, "STREAM=5",
		      "EXPECT_B=V.90 connect 5[0-9]{4}/",
		      "PROTO_B=ATX3;AT+MS=90" });
	/* V.92 as it comes (+MS=V92): its INFO0a says V.92, its INFO1a asks
	   for no PCM upstream (V.90 and V.34 upstream).  With +PIG=0 it does
	   (phase 3, TRN2u, SUVu, and SUVu' to our CPd) but sends no CPu: tone
	   A 0.4 s after, and V.90 after the retrain */
	add("intel", "intel_v92", "intel", "open8", "ATX3",
	    { i537, "NET_LOOP=0.45", "NET=9600:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=5",
	      "EXPECT_B=(?s)INFO0a V.92 1.*INFO1a: V.90.*V.90 connect 5[0-9]{4}/",
	      "PROTO_B=ATX3" });
	add("intel", "intel_v92_pcmup", "intel", "open8", "ATX3;AT+PIG=0",
	    { i537, "NET_LOOP=0.45", "NET=9600:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=5",
	      "EXPECT_B=(?s)v92: Ja: DIL.*v92: SUVu' at.*v92: tone A.*V.90 "
	      "connect 5[0-9]{4}/", "PROTO_B=ATX3" });
	/* class 1 fax both ways */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145, 146 }) {
		std::string s = std::to_string(m);
		add("intel", "intel_fax_" + s + "_intel_sends", "intel", "open",
		    "ATX3", { i537, "NET=9600:9600:ulaw" })->fax = m;
		add("intel", "intel_fax_" + s + "_open_sends", "open", "intel",
		    "ATX3", { i537, "NET=9600:9600:ulaw" })->fax = m;
	}

	/* --- MNP 2 to 4 (V.42's alternative procedure) with MNP 5, both
	   ways with each modem that has it, 20000 octets each way of text,
	   binary, repeats and noise: it calls with MNP alone (+ES=4,4,6)
	   and our answerer takes its LR in the detection phase; we call
	   with MNP alone (\N5) and it answers so (+ES=3,0,6).  HDLC frames,
	   8 outstanding, of up to 256 octets */
	const std::string mnp5 = "mnp: LR: framing mode 3, k 8, N401 256, "
		"optimization 3, MNP 5";
	for (const char *p : { "agere", "pctel", "sm56", "hsf", "broadcom",
			       "intel", "ess" }) {
		std::string n = p;
		/* (the Intel modem's line at 9600 Hz, the others' 8000;
		   INTEL_RATE only half_intel reads) */
		bool i = n == "intel";
		std::string r = i ? "9600" : "8000";
		add(p, n + "_mnp5_" + n + "_calls", p, "open",
		    "ATX3;AT+MS=V34;AT+ES=4,4,6",
		    { "NET=" + r + ":9600:ulaw", "PROTO_B=ATX3;AT+MS=34",
		      "BULK=20000", "EXPECT_B=" + mnp5, i537 });
		add(p, n + "_mnp5_open_calls", "open", p,
		    "ATX3;AT+MS=34;AT\\N5",
		    { "NET=9600:" + r + ":ulaw",
		      "PROTO_B=ATX3;AT+MS=V34;AT+ES=3,0,6", "BULK=20000",
		      "EXPECT_A=" + mnp5, i537 });
	}
	/* and MNP 4 alone (our %C2: no MNP 5) */
	add("agere", "agere_mnp4_agere_calls", "agere", "open",
	    "ATX3;AT+MS=V34;AT+ES=4,4,6",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34;AT%C2",
	      "BULK=20000", "EXPECT_B=optimization 3\\.\n" });
	add("agere", "agere_mnp4_open_calls", "open", "agere",
	    "ATX3;AT+MS=34;AT\\N5%C2",
	    { "NET=9600:8000:ulaw", "PROTO_B=ATX3;AT+MS=V34;AT+ES=3,0,6",
	      "BULK=20000", "EXPECT_A=optimization 3\\.\n" });
	/* --- retrains and rate renegotiations the DTE asks for (ATO1,
	   ATO3: MDMPRM_LINK_REQ; V.80's <EM><rtn>, <EM><rrn>) in the middle
	   of V.34 data, both ways with each modem that runs V.34 here; the
	   Agere, Broadcom and HSF modems answer our retrain as the caller
	   with tone A and as much again at 1800 Hz.  The data goes on (V.42
	   carries it over) */
	for (const char *p : { "agere", "broadcom", "sm56", "pctel", "hsf",
			       "intel", "ess" }) {
		std::string n = p;
		std::string r = n == "intel" ? "9600" : "8000";
		std::string rt = "EXPECT_A=(?s)retrain 1 \\(the DTE's\\).*"
			"retrained";
		add(p, n + "_34_dte_retrain_open_calls", "open", p,
		    "ATX3;AT+MS=34",
		    { "NET=9600:" + r + ":ulaw", "PROTO_B=ATX3;AT+MS=V34",
		      "STREAM=25", "MIDCALL_A=6:WAIT 0", "MIDCALL_ATO_A=ATO1",
		      rt, i537 });
		add(p, n + "_34_dte_retrain_" + n + "_calls", p, "open",
		    "ATX3;AT+MS=V34",
		    { "NET=" + r + ":9600:ulaw", "PROTO_B=ATX3;AT+MS=34",
		      "STREAM=25", "MIDCALL_B=6:WAIT 0", "MIDCALL_ATO_B=ATO1",
		      "EXPECT_B=(?s)retrain 1 \\(the DTE's\\).*retrained",
		      i537 });
	}
	for (const char *p : { "agere", "broadcom" })
		add(p, std::string(p) + "_34_dte_reneg_open_calls", "open", p,
		    "ATX3;AT+MS=34",
		    { "NET=9600:8000:ulaw", "PROTO_B=ATX3;AT+MS=V34",
		      "STREAM=25", "MIDCALL_A=6:WAIT 0", "MIDCALL_ATO_A=ATO3",
		      "EXPECT_A=(?s)renegotiation 1 \\(the DTE's\\).*"
		      "renegotiated" });
	/* --- V.80 Synchronous Access Mode (+ES=6 calling, 8 answering;
	   docs/link.md, V.80): after CONNECT both DTEs go over to the
	   framed sub-mode and send 20 frames each, every octet value among
	   them, which the other DTE must get whole, each ended by
	   <EM><flag>.  V.34, our transmitter at most 28800 bit/s (the SM56
	   modem renegotiates down from 33600, and V.80 has no error
	   correction to carry frames over it).  What the peers' DCEs do:
	   - the Agere and Broadcom modems (the same V.80 code): no FCS or
	     FCS-16 (their FCS-32 sends nothing, and gives the DTE nothing);
	     a checked FCS is dropped, but by the octets of the shielded
	     output, so an FCS octet 19h or 99h leaves a stray EM before the
	     <EM><flag> (V80_STRAY);
	   - the SM56 modem: FCS-16 calling, its DTE getting the FCS with
	     each frame (V80_EXTRA); answering it takes no +ESA (any +ESA
	     and it does not answer), so no FCS; its receiver hunts only
	     from its own first frame (the exchange's warm-up frame);
	   - the Intel modem: no FCS only, and its transmitter sends 16 1s
	     in place of a frame's last two octets (as if there were an
	     FCS), which every receiver takes for an abort: only its
	     reception is checked (V80_ONLY);
	   - HSF, PCTel and ESS have no Synchronous Access Mode (+ES=? has
	     no 6 or 8) */
	const std::string esa1 = "AT+ESA=0,0,0,0,1,0,255,255";
	const std::string ours = "ATX3;AT+MS=34,1,2400,28800;AT+ES=6,0,8";
	for (const char *p : { "agere", "broadcom" })
		for (int crc : { 0, 1 }) {
			std::string n = p, c = std::to_string(crc);
			std::string esa = "AT+ESA=0,0,0,0," + c + ",0,255,255";
			std::string theirs = "ATX3;AT+MS=V34;AT+ES=6,0,8;" + esa;
			add(p, n + "_v80_crc" + c + "_" + n + "_calls", p, "open",
			    theirs, { "V80=1", "NET=8000:9600:ulaw",
				      "PROTO_B=" + ours + ";" + esa,
				      "V80_STRAY_A=1" });
			add(p, n + "_v80_crc" + c + "_open_calls", "open", p,
			    ours + ";" + esa, { "V80=1", "NET=9600:8000:ulaw",
					      "PROTO_B=" + theirs,
					      "V80_STRAY_B=1" });
		}
	add("sm56", "sm56_v80_crc1_sm56_calls", "sm56", "open",
	    "ATX3;AT+MS=V34;AT+ES=6,0,8;" + esa1,
	    { "V80=1", "NET=8000:9600:ulaw", "PROTO_B=" + ours + ";" + esa1,
	      "V80_EXTRA_A=2" });
	add("sm56", "sm56_v80_open_calls", "open", "sm56", ours,
	    { "V80=1", "NET=9600:8000:ulaw",
	      "PROTO_B=ATX3;AT+MS=V34;AT+ES=6,0,8" });
	add("intel", "intel_v80_intel_calls", "intel", "open",
	    "ATX3;AT+MS=V34;AT+ES=6,0,8",
	    { "V80=1", "INTEL_RATE=9600", "NET=9600:9600:ulaw",
	      "PROTO_B=" + ours, "V80_ONLY=A" });
	add("intel", "intel_v80_open_calls", "open", "intel", ours,
	    { "V80=1", "INTEL_RATE=9600", "NET=9600:9600:ulaw",
	      "PROTO_B=ATX3;AT+MS=V34;AT+ES=6,0,8", "V80_ONLY=B" });
	/* --- the open modem on the emulated HD-audio modem codec
	   (half_hda: an HD Audio controller and a Si3054, its codec at
	   9600 Hz on the 8000 Hz line) and the open modem (9600 Hz): each
	   modulation both ways, V.34 with the codec at 8000 Hz too; V.90
	   and V.92 (PCM upstream) with it as the analogue modem; fax both
	   ways */
	for (const char *ms : { "34", "132", "122", "22", "212", "103",
				"21", "23" }) {
		std::string p = std::string("ATX3;AT+MS=") + ms;
		add("hda", std::string("hda_") + ms + "_hda_calls", "hda",
		    "open", p, { "NET=8000:9600:ulaw" });
		add("hda", std::string("hda_") + ms + "_open_calls", "open",
		    "hda", p, { "NET=9600:8000:ulaw" });
	}
	add("hda", "hda_34_8k_hda_calls", "hda", "open", "ATX3;AT+MS=34",
	    { "NET=8000:9600:ulaw", "HDA_CODEC_RATE=8000" });
	/* (PCM: the line at the codec's 9600 Hz, as the Intel modem's: a
	   resampler from 8000 Hz, its gain a little different at each of
	   its five phases, is what the DIL measures and no PCM line has) */
	for (std::string law : { "ulaw", "alaw" })
		add("hda", "hda_v90_" + law, "hda", "open8", "ATX3;AT+MS=90",
		    { "HDA_RATE=9600", "NET=9600:8000:" + law,
		      "DSP_G711=" + law, "BULK=100000",
		      "EXPECT_A=V.90 connect [0-9]+/5[0-9]{4} \\(analogue modem\\)",
		      "EXPECT_B=V.90 connect 5[0-9]{4}/" });
	add("hda", "hda_v92_pcmup", "hda", "open8", "ATX3",
	    { "HDA_RATE=9600", "NET=9600:8000:ulaw", "DSP_G711=ulaw",
	      "STREAM=5", "EXPECT_A=(?s)V.92 quick connect.*PCM upstream "
	      "phase 3 done.*V.92 connect 3[0-9]{4}/5[0-9]{4}" });
	/* (fax: at 8000 Hz on the line, with the open modem at 8000) */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145 }) {
		std::string s = std::to_string(m);
		add("hda", "hda_fax_" + s + "_hda_sends", "hda", "open8",
		    "ATX3")->fax = m;
		add("hda", "hda_fax_" + s + "_open_sends", "open8", "hda",
		    "ATX3")->fax = m;
	}
	/* --- netsim puts the call through (NET_RING): two open modems,
	   in each country of tones.cpp A (in that country: --country)
	   waiting for its dial tone (X4) and dialling 123 in DTMF, the
	   ringing (the ring signal's frequency, the ringing tone's cadence)
	   after it, B answering at its first RING (ATS0=1); and with no
	   dial tone wait nor digits (X3: the ringing NET_DIAL_TIMEOUT after
	   A goes off hook) B at the second ring, two calls, V.34 */
	for (const tones::Country *rc = tones::COUNTRIES; rc->name; rc++) {
		std::string n = rc->name;
		for (char &ch : n)
			ch = (char)tolower((unsigned char)ch);
		add("ring", "ring_" + n, "open", "open", "ATX4;AT+MS=122",
		    { "NET=9600:9600:ulaw", std::string("NET_RING=") + rc->name,
		      std::string("COUNTRY_A=") + rc->name, "DIAL=123",
		      "STREAM=1" });
	}
	/* and B's line busy (NET_BUSY), the network congested
	   (NET_CONGESTION): A, in that country, waits for its dial tone,
	   dials and ends with BUSY on the tone (its country's busy or
	   congestion cadence: dp_callprog.cpp, four of its cycles); NO
	   ANSWER after S7 on a steady one (Ascension's congestion tone) */
	for (const tones::Country *rc = tones::COUNTRIES; rc->name; rc++) {
		std::string n = rc->name;
		for (char &ch : n)
			ch = (char)tolower((unsigned char)ch);
		for (int cong = 0; cong < 2; cong++) {
			const tones::Tone &t = cong ? rc->congestion : rc->busy;
			/* (S7: four of its cycles, Egypt's 5 s, and more) */
			int s7 = 15 + 4 * (int)tones::period(t);
			add("ring", "ring_" + n + (cong ? "_congestion" : "_busy"),
			    "open", "open",
			    "ATX4;ATS7=" + std::to_string(s7) + ";AT+MS=122",
			    { "NET=9600:9600:ulaw",
			      std::string("NET_RING=") + rc->name,
			      std::string("COUNTRY_A=") + rc->name,
			      cong ? "NET_CONGESTION=1" : "NET_BUSY=1",
			      "DIAL=123", t.n ? "EXPECT_FAIL=BUSY" :
			      "EXPECT_FAIL=NO ANSWER" });
		}
	}
	add("ring", "ring_uk_second", "open", "open", "ATX3;AT+MS=122",
	    { "NET=9600:9600:ulaw", "NET_RING=UK", "RING_ANSWER=2",
	      "STREAM=1" });
	add("ring", "ring_usa_calls", "open", "open", "ATX3;AT+MS=122",
	    { "NET=9600:9600:ulaw", "NET_RING=USA", "CALLS=2", "STREAM=1" });
	add("ring", "ring_germany_34", "open", "open", "ATX3;AT+MS=34",
	    { "NET=9600:9600:ulaw", "NET_RING=GERMANY", "STREAM=3" });
	/* the HD-audio modem rung (its line monitor on hook hears the
	   ring signal), and pulse dialling after its dial tone (its breaks
	   too short to end the call; the ringing after the digits) */
	add("ring", "ring_hda_answers", "open", "hda", "ATX3;AT+MS=122",
	    { "NET=9600:8000:ulaw", "NET_RING=GERMANY", "STREAM=1" });
	add("ring", "ring_hda_pulse_dials", "hda", "open", "ATX4;AT+MS=122",
	    { "NET=8000:9600:ulaw", "NET_RING=UK", "DIAL=P123",
	      "STREAM=1" });

	/* --- adaptive answer (+FAA=1): the answerer finds out whether the
	   call is a fax machine's or a data modem's.  Each modem's class 1
	   fax sender calls the open modem, answering in fax mode (and ours
	   in data mode as well): FAX, then the page (the senders' CNG, or
	   the V.8 CM of a fax call that the Agere and Conexant modems send
	   on ANSam) */
	const std::string faa = "SETUP_B=AT+FAA=1";
	for (int m : { 96, 145 })
		add("adaptive", "adaptive_fax_" + std::to_string(m) +
		    "_open_sends", "open8", "open8", "ATX3",
		    { faa, "ANSWER_B=FAX" })->fax = m;
	add("adaptive", "adaptive_fax_96_open_sends_data_mode", "open8",
	    "open8", "ATX3",
	    { "SETUP_B=AT+FCLASS=0;AT+FAA=1", "ANSWER_B=FAX" })->fax = 96;
	add("adaptive", "adaptive_fax_96_smartlink_sends", "smartlink",
	    "open", "ATX3",
	    { faa, "ANSWER_B=FAX", "LINE_RATE=9600" })->fax = 96;
	for (const char *peer : { "agere", "hsf", "broadcom", "hda" })
		add("adaptive", std::string("adaptive_fax_96_") + peer +
		    "_sends", peer, "open8", "ATX3",
		    { faa, "ANSWER_B=FAX" })->fax = 96;
	add("adaptive", "adaptive_fax_96_intel_sends", "intel", "open",
	    "ATX3", { faa, "ANSWER_B=FAX", "INTEL_RATE=9600",
		      "NET=9600:9600:ulaw" })->fax = 96;
	/* a V.8 fax call (spandsp's CM with the call function of a fax
	   machine that sends) into the open modem in data mode: FAX, and
	   no data call (spandsp, no fax machine, gives up) */
	add("adaptive", "adaptive_v8_fax_cm", "spandsp", "open", "ATX3",
	    { "NET=8000:9600:ulaw", "SPANDSP_V8_CALL=4",
	      "PROTO_B=ATX3;AT+FAA=1", "EXPECT_FAIL=1",
	      "EXPECT_B=(?s)a fax call \\(V.8\\).*result: FAX" });
	/* data callers into +FAA=1, in data mode and in fax mode: DATA,
	   then the data call */
	add("adaptive", "adaptive_data_agere_34", "agere", "open",
	    "ATX3;AT+MS=V34",
	    { "NET=8000:9600:ulaw", "PROTO_B=ATX3;AT+MS=34;AT+FAA=1",
	      "EXPECT_B=result: DATA" });
	add("adaptive", "adaptive_data_agere_122_fax_mode", "agere", "open",
	    "ATX3;AT+MS=V22B",
	    { "NET=8000:9600:ulaw",
	      "PROTO_B=ATX3;AT+MS=122;AT+FCLASS=1;AT+FAA=1",
	      "EXPECT_B=result: DATA" });
	add("adaptive", "adaptive_data_smartlink_34_fax_mode", "smartlink",
	    "open", "ATX3;AT+MS=34",
	    { "PROTO_B=ATX3;AT+MS=34;AT+FCLASS=1;AT+FAA=1",
	      "EXPECT_B=result: DATA" });
	for (const char *ms : { "122", "21" })
		add("adaptive", std::string("adaptive_data_spandsp_") + ms,
		    "spandsp", "open", std::string("ATX3;AT+MS=") + ms,
		    { "NET=8000:9600:ulaw", std::string("PROTO_B=ATX3;AT+MS=") +
		      ms + ";AT+FCLASS=1;AT+FAA=1",
		      "EXPECT_B=result: DATA" });
	for (const char *peer : { "sm56", "hsf" })
		add("adaptive", std::string("adaptive_data_") + peer + "_v90",
		    peer, "open8", "ATX3;AT+MS=V90",
		    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
		      "EXPECT_B=(?s)V.90 connect.*result: DATA",
		      "PROTO_B=ATX3;AT+MS=90;AT+FAA=1" });
	/* fax senders calling the open modem in voice mode, on the line
	   (+VLS=1) with +FAA=1: their CNG (<DLE>c) makes the call a fax
	   call, FAX and the class 1 answer on it, then the page (Smart
	   Link's library sends no CNG on a silent line) */
	for (const char *peer : { "open8", "agere", "hsf" })
		add("adaptive", std::string("adaptive_fax_96_") + peer +
		    "_sends_voice_mode", peer, "open8", "ATX3",
		    { faa, "ANSWER_VOICE=1", "ANSWER_B=\x10" "cFAX" })->fax =
			96;
	/* a fax sender without CNG (DSP_FAX_CNG=0), silent until the DIS,
	   into fax and data mode: no answer to any stage of the automode
	   chain, so FAX at its end (Bell 103's 3 s) */
	add("adaptive", "adaptive_fax_96_silent_sender", "open8", "open8",
	    "ATX3", { faa, "ANSWER_B=FAX", "DSP_FAX_CNG=0",
		      "EXPECT_B=a fax call \\(silent" })->fax = 96;
	add("adaptive", "adaptive_fax_96_silent_sender_data_mode", "open8",
	    "open8", "ATX3",
	    { "SETUP_B=AT+FCLASS=0;AT+FAA=1", "ANSWER_B=FAX",
	      "DSP_FAX_CNG=0" })->fax = 96;
	/* slow legacy callers silent until they hear their own answer
	   signal (the Agere modem forced to Bell 103, V.21 or V.22, no
	   V.8) into fax mode, the whole automode chain: DATA, then the call */
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "B103", "103" }, { "V21", "21" }, { "V22", "22" } })
		add("adaptive", std::string("adaptive_data_agere_") + ms +
		    "_fax_mode", "agere", "open",
		    std::string("ATX3;AT+MS=") + n,
		    { "NET=8000:9600:ulaw",
		      "PROTO_B=ATX3;AT+FCLASS=1;AT+FAA=1",
		      "EXPECT_B=result: DATA" });
	add("adaptive", "adaptive_data_open_92", "open", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "EXPECT_B=result: DATA",
	      "PROTO_B=ATX3;AT+MS=92;AT+FCLASS=1;AT+FAA=1" });
	return c;
}

/* the call's environment, for it alone */
class Env {
	std::vector<std::pair<std::string, std::string>> old_;
	std::vector<std::string> unset_;

public:
	/* NAME=value set, the old value (or none) kept */
	void set(const std::string &kv)
	{
		size_t e = kv.find('=');
		std::string k = kv.substr(0, e);
		if (const char *v = getenv(k.c_str()))
			old_.emplace_back(k, v);
		else
			unset_.push_back(k);
		setenv(k.c_str(), kv.substr(e + 1).c_str(), 1);
	}
	/* the environment back as it was */
	~Env()
	{
		for (auto &k : unset_)
			unsetenv(k.c_str());
		for (auto &[k, v] : old_)
			setenv(k.c_str(), v.c_str(), 1);
	}
};

class InteropCall : public testing::Test {
	Call c_;

public:
	/* the test of call c */
	explicit InteropCall(Call c) : c_(std::move(c)) {}

	/* the call with its environment, NETSIM and its LOG_DIR set (the
	   test's own directory in the run's test logs, DMODEM_TEST_DIR, or
	   INTEROP_LOG_DIR/<call>); it must go through */
	void TestBody() override
	{
		Env env;
		const char *t = getenv("DMODEM_TEST_DIR");
		std::string logs = t && *t ? std::string(t) :
			std::string(INTEROP_LOG_DIR "/") + c_.name;
		env.set(std::string("NETSIM=") + NETSIM);
		env.set("LOG_DIR=" + logs);
		for (auto &e : c_.env)
			env.set(e);
		const char *a = programs().at(c_.a), *b = programs().at(c_.b);
		if (c_.fax)
			EXPECT_TRUE(interop::fax_call(a, b, c_.fax, c_.proto))
				<< c_.name << " (logs: " << logs << ")";
		else
			EXPECT_TRUE(interop::data_call(a, b, c_.proto))
				<< c_.name << " (logs: " << logs << ")";
	}
};

/* this program's calls: its label's, with the modems built */
int registered = [] {
	for (auto &c : calls()) {
		if (c.label != INTEROP_LABEL || !programs().count(c.a) ||
		    !programs().count(c.b))
			continue;
		testing::RegisterTest(INTEROP_LABEL, c.name.c_str(), nullptr,
				      nullptr, __FILE__, __LINE__,
				      [c]() -> testing::Test * {
					      return new InteropCall(c);
				      });
	}
	return 0;
}();

} // namespace
