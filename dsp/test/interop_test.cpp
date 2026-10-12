/*
 *
 *    interop_test.cpp - the interop tests: calls in real time between the
 *                       open modem (half_open, half_open8) and the other
 *                       modems we have as programs (Smart Link's original
 *                       library, the Agere, SM56, PCTel and HSF soft
 *                       modems, spandsp's modems), through interop_call
 *
 *    One table of calls; each test program has the calls of one ctest
 *    label (INTEROP_LABEL) that run alone or not (INTEROP_SERIAL).  The
 *    modem programs are HALF_* (undefined: that modem's calls are left
 *    out), netsim NETSIM; each call's logs go to INTEROP_LOG_DIR/<test>.
 *    A call's environment (interop_call.h: NET, STREAM, EXPECT_B, ...;
 *    and the modems' and netsim's own: DSP_*, NET_*, ...) is set for it
 *    alone.
 *
 */

#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "interop_call.h"

namespace {

struct Call {
	std::string label;
	std::string name;
	std::string a, b;       /* the modems: blob, open, open8, agere, ... */
	std::string proto;      /* the AT commands (fax: the setup) */
	std::vector<std::string> env;
	int fax = 0;            /* fax: the modulation, data call: 0 */
	bool serial = false;    /* not with other tests (ctest RUN_SERIAL) */
};

const std::map<std::string, const char *> &programs()
{
	static const std::map<std::string, const char *> p = {
		{ "blob", HALF_BLOB }, { "open", HALF_OPEN },
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
	};
	return p;
}

std::vector<Call> calls()
{
	std::vector<Call> c;
	auto add = [&c](const char *label, std::string name, const char *a,
			const char *b, std::string proto,
			std::vector<std::string> env = {}) {
		c.push_back({ label, std::move(name), a, b, std::move(proto),
			      std::move(env) });
		return &c.back();
	};

	/* --- the original library (half_blob) */
	for (const char *ms : { "34", "132", "122", "22", "212", "103", "23" }) {
		std::string p = std::string("ATX3;AT+MS=") + ms;
		add("interop", std::string("interop_") + ms + "_blob_calls",
		    "blob", "open", p);
		add("interop", std::string("interop_") + ms + "_open_calls",
		    "open", "blob", p);
	}
	/* V.34 through a line hit (300 ms of silence both ways) in the
	   middle of the data: both modems retrain, V.42 carries on */
	for (auto [n, a, b] : { std::make_tuple("blob", "blob", "open"),
				std::make_tuple("open", "open", "blob") })
		add("interop", std::string("interop_34_hit_") + n + "_calls",
		    a, b, "ATX3;AT+MS=34",
		    { "NET=9600:9600:ulaw", "STREAM=25", "NET_DROP=20:300" });
	/* V.34 rate renegotiation: the open modem starts one (and later
	   goes back up), the original library answers it; and two open
	   modems on a noisy line that gets 3 dB worse 10 s after CONNECT
	   (NET_SYNC), each lowers its receiving rate */
	for (auto [n, a, b] : { std::make_tuple("blob", "blob", "open"),
				std::make_tuple("open", "open", "blob") })
		add("interop", std::string("interop_34_reneg_") + n + "_calls",
		    a, b, "ATX3;AT+MS=34",
		    { "NET=9600:9600:ulaw", "NET_SYNC=1", "STREAM=25",
		      "DSP_V34_RENEG=5000:8" });
	add("interop", "interop_34_noise", "open", "open", "ATX3;AT+MS=34",
	    { "NET=9600:9600:ulaw", "NET_SYNC=1", "STREAM=30",
	      "NET_FLOOR=250", "NET_NOISE=10:250" });
	/* V.90: the original library (analogue modem) calls the open one
	   (digital modem, 8000 Hz) through the simulated G.711 network.
	   NET_FLOOR: on a line without any noise its phase 4 check (the
	   error before and after TRN2d corrects its levels from the DIL by
	   a unit or two) fails, and it retrains once for nothing; a floor
	   of -81 dBFS keeps the check to what it is for on a real line */
	add("interop", "interop_v90_ulaw", "blob", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=3" });
	add("interop", "interop_v90_alaw", "blob", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:alaw", "DSP_G711=alaw" });
	/* V.90 rate renegotiation (9.6/V.90): ours after 5 s of data, to
	   24 000 bit/s upstream; the library's own when noise on the PCM
	   codes from 40 s on worsens the downstream: it asks for a silent
	   period (CPs), and the noise stops with our silence (else it may
	   pick 56000 again and the data goes wrong) */
	add("interop", "interop_v90_reneg", "blob", "open8", "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=3", "STREAM=15",
	      "DSP_V34_RENEG=5000:10",
	      "EXPECT_B=V.90 renegotiated [0-9]+/24000" });
	add("interop", "interop_v90_reneg_silence", "blob", "open8",
	    "ATX3;AT+MS=90",
	    { "NET=9600:8000:ulaw", "NET_FLOOR=3", "STREAM=50",
	      "NET_NOISE=40:20:10", "NET_NOISE_DIR=bacodes",
	      "NET_NOISE_QUIET=50",
	      "EXPECT_B=(?s)CPs'.*V.90 renegotiated" });
	/* V.92 quick connect (short phase 1): the original library, a V.92
	   analogue modem, starts with QC1a; then V.90 */
	add("interop", "interop_v92_qc", "blob", "open8", "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "EXPECT_B=V.92 quick connect" });
	/* V.92 PCM upstream: the original library is a "V.92 Lite" build,
	   which retrains when INFO1d offers PCM upstream; BLOB_V92FULL
	   (legacy/modem.cpp) sets its full V.92 bit, and it then goes
	   through phases 3 and 4 of V.92 with us.  Lite: we answer its
	   retrain without the offer, V.90.  A rate the line cannot carry
	   (44 000): B1u does not come through, we retrain to V.90.  With the
	   prefilter we design (6.4.2/V.92) the rate is up to 38 666 (mu-law)
	   and 40 000 (A-law); without it (DSP_V92_PREFILTER=0), the precoder
	   alone, 34 000 to 36 000 */
	for (const char *law : { "ulaw", "alaw" })
		add("interop", std::string("interop_v92_pcmup_") + law, "blob",
		    "open8", "ATX3;AT+MS=92",
		    { std::string("NET=9600:8000:") + law, "STREAM=10",
		      "BLOB_V92FULL=1",
		      "EXPECT_B=(?s)prefilter of 64 taps.*V.92 connect "
		      "56000/(3[89]|4[0-2])" });
	add("interop", "interop_v92_pcmup_noprefilter", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "BLOB_V92FULL=1",
	      "DSP_V92_PREFILTER=0", "EXPECT_B=V.92 connect 56000/3[4-9]" });
	/* (quick connect's timing does not take a busy machine: alone) */
	add("interop", "interop_v92_pcmup_calls", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "CALLS=2", "STREAM=3", "BLOB_V92FULL=1",
	      "EXPECT_B=(?s)V.92 connect.*V.92 connect" })->serial = true;
	add("interop", "interop_v92_pcmup_lite", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=3",
	      "EXPECT_B=(?s)no PCM upstream offer.*V.90 connect" });
	add("interop", "interop_v92_pcmup_fallback", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=3", "BLOB_V92FULL=1",
	      "DSP_V92_UP_DRN=16",
	      "EXPECT_B=(?s)B1u does not come through.*V.90 connect" });
	/* rate renegotiation (9.8/V.92): ours after 3 s of data, to 30 666
	   bit/s upstream (DSP_V92_RENEG=<ms>:<drn>); and on its own when
	   noise on the way up eats the margin of our rate */
	add("interop", "interop_v92_reneg", "blob", "open8", "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "BLOB_V92FULL=1",
	      "DSP_V92_RENEG=3000:6",
	      "EXPECT_B=V.92 renegotiated 56000/30666" });
	add("interop", "interop_v92_reneg_noise", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=15", "BLOB_V92FULL=1",
	      "NET_NOISE=26:60", "NET_NOISE_DIR=ab",
	      "EXPECT_B=V.92 renegotiated 56000/[23]" });
	/* fast parameter exchange (9.9/V.92): ours after 3 s of data, to
	   30 666 bit/s upstream (DSP_V92_FPE=<ms>:<drn>), in the data mode
	   modulation.  Without the prefilter (below with it) */
	add("interop", "interop_v92_fpe", "blob", "open8", "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "BLOB_V92FULL=1",
	      "DSP_V92_PREFILTER=0", "DSP_V92_FPE=3000:6",
	      "EXPECT_B=(?s)fast parameter exchange 1 done.*V.92 "
	      "renegotiated 56000/30666" });
	/* modem-on-hold asked for by us (AT+PMHR, 9.10.2.1/V.92) in the
	   middle of PCM upstream data: the original library answers our
	   tone B with a retrain, which we follow, and the data goes on */
	add("interop", "interop_v92_moh_request", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=30", "BLOB_V92FULL=1",
	      "MIDCALL_B=8:AT+PMHR;WAIT 6",
	      "EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.92 "
	      "retrained" });
	/* and with an analogue modem that answers it (netsim's NET_MOH
	   stands in for one from 22 s on: the original library does not):
	   MHack, the remote on hold for 5 s, ANSam; MHnack, MHfrr and
	   ANSam.  The original library is cut off meanwhile and hangs up,
	   so nothing comes back to ANSam and we disconnect after T1 (10 s) */
	add("interop", "interop_v92_moh_request_ack", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=40", "BLOB_V92FULL=1",
	      "NET_MOH=ack:1@22", "MIDCALL_B=12:AT+PMHR=5;WAIT 8",
	      "EXPECT_DROP=1",
	      "EXPECT_B=(?s)MHack, T1 10 s: the remote on hold for 5 s.*on "
	      "hold, ANSam for 10 s.*on hold for T1: disconnect" });
	add("interop", "interop_v92_moh_request_nack", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=35", "BLOB_V92FULL=1",
	      "NET_MOH=nack@22", "MIDCALL_B=12:AT+PMHR;WAIT 3",
	      "EXPECT_DROP=1",
	      "EXPECT_B=(?s)MHnack: MHfrr.*on hold \\(reconnect\\), ANSam.*"
	      "on hold for T1: disconnect" });
	/* and with the prefilter: not down (the original library sends
	   SUVu' to FB1u on the new points with the old moduli), so at the
	   rate it has; the library's sequence through to B1u, and the data
	   goes on */
	add("interop", "interop_v92_fpe_prefilter", "blob", "open8",
	    "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=10", "BLOB_V92FULL=1",
	      "DSP_V92_FPE=3000:6",
	      "EXPECT_B=(?s)prefilter not down.*SUVu'.*fast parameter "
	      "exchange 1 done.*V.92 renegotiated 56000/3[89]" });
	/* without the offer (DSP_V92_PCMUP=0): V.90, V.34 upstream */
	add("interop", "interop_v92_nopcmup", "blob", "open8", "ATX3;AT+MS=92",
	    { "NET=9600:8000:ulaw", "STREAM=3", "BLOB_V92FULL=1",
	      "DSP_V92_PCMUP=0", "EXPECT_B=V.90 connect" });
	/* class 1 fax calls both ways */
	for (int m : { 24, 48, 72, 96, 73, 97, 121, 122, 145 }) {
		std::string s = std::to_string(m);
		add("interop", "fax_interop_" + s + "_blob_sends", "blob",
		    "open", "ATX3")->fax = m;
		add("interop", "fax_interop_" + s + "_open_sends", "open",
		    "blob", "ATX3")->fax = m;
	}

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
	/* V.92 quick connect: the Agere modem (+PQC=0) remembers the line
	   of the first call and starts the second with QC1a, and short
	   phase 2.  NET_LOOP: our PCM signal reaches it through an analogue
	   loop (on an ideal line, the same samples as ours, its V.90
	   training fails at some delays, every 48 samples); not half a
	   sample: then our sample instants fall exactly between its
	   samples, and at some alignments (AGERE_SYNC_PHASE 96, 288) it
	   takes 33333 bit/s downstream for 53333 */
	add("agere", "agere_v92_qc", "agere", "open8",
	    "ATX3;AT+MS=V92;AT+PQC=0",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "CALLS=2", "DIAL=5551234", "STREAM=3",
	      "EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2",
	      "PROTO_B=ATX3;AT+MS=92" })->serial = true;
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
	   answer tone for unscrambled ones, see the README) */
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
	   INFO1d flag its full phase 2 has cleared) */
	add("sm56", "sm56_v92_qc", "sm56", "open8", "ATX3;AT+MS=V92",
	    { "NET_LOOP=0.45", "NET=8000:8000:ulaw", "DSP_G711=ulaw",
	      "CALLS=2", "DIAL=5551234", "STREAM=3",
	      "EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2.*V.90 "
	      "connect", "PROTO_B=ATX3;AT+MS=92" });
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

	/* --- the PCTel HSP soft modem (8000 Hz) and the open modem (9600
	   Hz).  Its pumps run its automode whatever +MS says (the controller
	   keeps the setting, but the modulation it gives the pumps is the
	   same), so only the calls its automode makes work: V.32bis and
	   V.34 both ways, and our V.22bis caller (see the README) */
	for (auto [n, ms] : std::vector<std::pair<const char *, const char *>>{
		     { "V32B,0", "132" }, { "V34", "34" } }) {
		std::string a = std::string("ATX3;AT+MS=") + n;
		std::string o = std::string("ATX3;AT+MS=") + ms;
		add("pctel", std::string("pctel_") + ms + "_pctel_calls",
		    "pctel", "open", a, { "NET=8000:9600:ulaw", "PROTO_B=" + o });
		add("pctel", std::string("pctel_") + ms + "_open_calls", "open",
		    "pctel", o, { "NET=9600:8000:ulaw", "PROTO_B=" + a });
	}
	add("pctel", "pctel_122_open_calls", "open", "pctel", "ATX3;AT+MS=122",
	    { "NET=9600:8000:ulaw", "PROTO_B=ATX3" });

	/* --- the HSF soft modem (8000 Hz) and the open modem (9600 Hz):
	   V.22bis, V.32bis and V.34 both ways and V.21 with the HSF modem
	   calling; V.90 and V.92 (PCM upstream)
	   with it as the analogue modem (see the README for the calls that
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
	/* V.92 with PCM upstream: it starts with quick connect (QC1a) on
	   the first call already; and with it off (+PQC=3), V.8 */
	for (std::string law : { "ulaw", "alaw" })
		add("hsf", "hsf_v92_pcmup_" + law, "hsf", "open8",
		    "ATX3;AT+MS=V92",
		    { "NET_LOOP=0.45", "NET=8000:8000:" + law,
		      "DSP_G711=" + law, "STREAM=10",
		      "EXPECT_B=(?s)V.92 quick connect.*V.92 connect "
		      "5[0-9]{4}/(3[4-9]|4[0-2])", "PROTO_B=ATX3;AT+MS=92" });
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
	explicit InteropCall(Call c) : c_(std::move(c)) {}

	void TestBody() override
	{
		Env env;
		env.set(std::string("NETSIM=") + NETSIM);
		env.set("MODEM_REALTIME=1");
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

/* this program's calls: its label's, alone or not, with the modems
   built */
int registered = [] {
	for (auto &c : calls()) {
		if (c.label != INTEROP_LABEL || c.serial != INTEROP_SERIAL ||
		    !programs().count(c.a) || !programs().count(c.b))
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
