/*
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
 *    to INTEROP_LOG_DIR/<test>.
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

#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "interop_call.h"

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
	/* (it ran alone once: quick connect's timing did not take a busy
	   machine; next to the unpaced calls, which run nicer, and a dozen
	   calls in real time it connects at the same moments every time) */
	add("smartlink", "smartlink_v92_pcmup_calls", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "CALLS=2", "STREAM=3",
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
	   MHack, the remote on hold for 5 s, ANSam; MHnack, MHfrr and
	   ANSam.  The original library is cut off meanwhile and hangs up,
	   so nothing comes back to ANSam and we disconnect after T1 (10 s) */
	add("smartlink", "smartlink_v92_moh_request_ack", "smartlink", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=40", "SMARTLINK_V92FULL=1",
	      "NET_MOH=ack:1@22", "MIDCALL_B=12:AT+PMHR=5;WAIT 8",
	      "EXPECT_DROP=1",
	      "EXPECT_B=(?s)MHack, T1 10 s: the remote on hold for 5 s.*on "
	      "hold, ANSam for 10 s.*on hold for T1: disconnect" });
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
	      "STREAM=15", "DSP_V34_RENEG=5000:8",
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

	/* the call with its environment, NETSIM and its LOG_DIR set; it must
	   go through */
	void TestBody() override
	{
		Env env;
		env.set(std::string("NETSIM=") + NETSIM);
		env.set(std::string("LOG_DIR=") + INTEROP_LOG_DIR "/" + c_.name);
		for (auto &e : c_.env)
			env.set(e);
		const char *a = programs().at(c_.a), *b = programs().at(c_.b);
		if (c_.fax)
			EXPECT_TRUE(interop::fax_call(a, b, c_.fax, c_.proto))
				<< c_.name << " (logs: " INTEROP_LOG_DIR "/"
				<< c_.name << ")";
		else
			EXPECT_TRUE(interop::data_call(a, b, c_.proto))
				<< c_.name << " (logs: " INTEROP_LOG_DIR "/"
				<< c_.name << ")";
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
