/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    core_test - the modem core by itself: its Host interface, the
 *                timers, the options, a line that fails, and two modems
 *                back to back with data both ways, byte for byte
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <regex>

#include <dcore.h>
#include <log.h>
#include <modem_param.h>
#include <dsp.h>
#include <t4.h>
#include <t38.h>

extern "C" int prop_dp_init(void);

using dcore::LinkState;
using dcore::Modem;
using dcore::Result;

/* the data pumps, once for the program */
static struct Pumps {
	/* the pumps registered */
	Pumps() {
		prop_dp_init();
	}
} pumps;

static uint64_t now;

/* the Host: a DTE that keeps the output, a line that can fail to start
   or to change the hook */
struct Line final : public dcore::Host {
	std::string out;
	int fail_start = 0, fail_hook = 0;
	int starts = 0, stops = 0, last_hook = -1, volume = -1;
	int delay = 0, delay_change = 0;
	/* counted; fail_start fails it */
	s16 start(Modem &) override {
		starts++;
		return fail_start;
	}
	/* counted */
	s16 stop(Modem &) override {
		stops++;
		return 0;
	}
	/* fail_hook fails it, else the state is kept */
	s16 set_hook(Modem &, s16 state) override {
		if (fail_hook)
			return -1;
		last_hook = state;
		return 0;
	}
	/* the volume, kept */
	void speaker(Modem &, s16 v) override { volume = v; }
	/* the line's delay, and the changes asked for, added up */
	s32 io_delay(Modem &) override { return delay; }
	/* (see io_delay()) */
	void update_delay(Modem &, s32 samples) override {
		delay_change += samples;
	}
	/* the DTE's output, kept */
	s32 dte_write(Modem &, const char *buf, s32 n) override {
		out.append(buf, n);
		return n;
	}
	/* the test's clock */
	uint64_t now_ms() override { return now; }

	/* an analogue line (analog), whose hook takes pulses (pulses):
	   each break and make kept with the samples processed by then */
	bool analog = false, pulses = false;
	unsigned long samples = 0;
	std::vector<std::pair<unsigned long, bool>> edges;
	/* the line analogue, as the test sets it */
	bool analog_line(Modem &) override { return analog; }
	/* the local phone sensed, as the test sets it */
	bool local_phone = false;
	/* (see local_phone) */
	bool senses_local_phone(Modem &) override { return local_phone; }
	/* a break or make kept with the samples by then; -1 unless pulses */
	s16 pulse_hook(Modem &, bool broken) override {
		if (!pulses)
			return -1;
		edges.emplace_back(samples, broken);
		return 0;
	}
	/* pulse dialling's start and end, kept with the samples by then */
	std::vector<std::pair<unsigned long, bool>> dialling;
	/* (see dialling) */
	void pulse_dialling(Modem &, bool on) override {
		dialling.emplace_back(samples, on);
	}	/* a handset on the modem (handset), the voice engine's routes
	   asked for, kept */
	bool handset = false;
	std::vector<unsigned> routes;
	/* (see handset) */
	bool has_handset(Modem &) override { return handset; }
	/* (see handset) */
	s16 voice_route(Modem &, u16 device) override {
		routes.push_back(device);
		return device == VOICE_DEVICE_LINE || handset ? 0 : -1;
	}
	/* a relayed call (relay): its state and rates as the test sets
	   them; the far end's octets in rx, ours to the far end's
	   (peer_rx) while it has fewer than window */
	bool relay = false;
	s16 relay_st = 0;
	s32 relay_tx = 0, relay_rx = 0;
	std::string rx, *peer_rx = nullptr;
	size_t window = 64;
	/* (see relay) */
	bool relay_line(Modem &) override { return relay; }
	/* (see relay) */
	s16 relay_state(Modem &, s32 *tx, s32 *rx_) override {
		*tx = relay_tx;
		*rx_ = relay_rx;
		return relay_st;
	}
	/* (see relay) */
	s32 relay_read(Modem &, u8 *buf, s32 max) override {
		s32 n = std::min<s32>(max, static_cast<s32>(rx.size()));
		memcpy(buf, rx.data(), n);
		rx.erase(0, n);
		return n;
	}
	/* (see relay) */
	s32 relay_write(Modem &, const u8 *buf, s32 n) override {
		if (!peer_rx || peer_rx->size() >= window)
			return 0;
		s32 k = std::min<s32>(n, static_cast<s32>(window -
							 peer_rx->size()));
		peer_rx->append(reinterpret_cast<const char *>(buf), k);
		return k;
	}
	/* T.38: the fax engine's IFP packets, as T.38 2002 codes them */
	std::vector<std::string> t38;
	int t38_sent = 0;
	/* (see t38) */
	void t38_send(Modem &, const void *ifp) override {
		unsigned char b[512];
		s32 n = dsp::t38_ifp_encode(
			static_cast<const struct dsp::t38_ifp *>(ifp), 1, b,
			sizeof(b));
		if (n > 0)
			t38.emplace_back((const char *)b, n);
		t38_sent++;
	}
};

/* a modem on l at 8000 Hz, in country (NULL: USA) */
static Modem *make(Line &l, const char *name, const char *country = nullptr)
{
	dcore::Options o;
	o.name = name;
	o.rate = 8000;
	o.country = country;
	return new Modem(l, o);
}

/* a command line; what the modem answered (SHOW prints it) */
static std::string cmd(Modem *m, Line &l, const char *s)
{
	l.out.clear();
	m->dte_input(s, strlen(s));
	if (getenv("SHOW")) {
		printf("[");
		for (char c : l.out)
			printf(c == '\r' ? "\\r" : c == '\n' ? "\\n" : "%c", c);
		printf("]\n");
	}
	return l.out;
}

/* the country, ATI7, S-registers out of range, an idle modem */
TEST(Core, Options)
{
	Line l;
	Modem *m = make(l, "uk", "UK");
	EXPECT_TRUE(m->ok());
	EXPECT_EQ(cmd(m, l, "ATI7\r"), "ATI7\r\r\nCountry: UK\r\nOK\r\n");
	delete m;
	m = make(l, "xx", "NOWHERE");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"), "ATE0I7\r\r\nCountry: USA\r\nOK\r\n");
	EXPECT_EQ(m->sreg(SREG_ECHO), 0);
	EXPECT_EQ(m->set_sreg(300, 1), -1);
	EXPECT_EQ(m->sreg(300), -1);
	EXPECT_EQ(m->next_timer_ms(), -1);
	EXPECT_EQ(m->link_state(), LinkState::Idle);
	EXPECT_TRUE(!m->started());
	delete m;
}

/* the country by the table's name, the ITU list's of the same country
   (UNITED_KINGDOM: UK), or one only the list has (Fiji: the USA's line
   settings, its own tones) */
TEST(Core, Countries)
{
	Line l;
	Modem *m = make(l, "uk", "united kingdom");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"), "ATE0I7\r\r\nCountry: UK\r\nOK\r\n");
	delete m;
	m = make(l, "fj", "Fiji");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"),
		  "ATE0I7\r\r\nCountry: FIJI (USA line)\r\nOK\r\n");
	delete m;
	m = make(l, "de", "GERMANY");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"), "ATE0I7\r\r\nCountry: GERMANY\r\nOK\r\n");
	delete m;
}

/* a line that cannot start or change the hook */
TEST(Core, FailingLine)
{
	Line l;
	Modem *m = make(l, "fail");
	l.fail_start = -1;
	/* the call ends at once (NO CARRIER), the command fails */
	EXPECT_EQ(cmd(m, l, "ATE0X3D\r"),
		  "ATE0X3D\r\r\nNO CARRIER\r\nERROR\r\n");
	EXPECT_TRUE(!m->started());
	EXPECT_EQ(m->link_state(), LinkState::Idle);
	l.fail_start = 0;
	l.fail_hook = 1;
	EXPECT_EQ(cmd(m, l, "ATA\r"), "NO CARRIER\r\nERROR\r\n");
	EXPECT_TRUE(!m->started());
	EXPECT_EQ(cmd(m, l, "ATH1\r"), "ERROR\r\n");
	l.fail_hook = 0;
	EXPECT_EQ(cmd(m, l, "ATH1\r"), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "ATH\r"), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	EXPECT_EQ(cmd(m, l, "ATM2L2\r"), "OK\r\n");
	EXPECT_EQ(l.volume, 2);
	delete m;
}

/* n ms of the line into m: silence, or with f a tone of f Hz (half
   scale) */
static void line(Modem *m, int ms, double f = 0)
{
	static unsigned long t;
	int16_t in[80], out[80];
	for (int b = 0; b < ms / 10; b++) {
		for (int i = 0; i < 80; i++, t++)
			in[i] = f ? (int16_t)(16000 * sin(2 * M_PI * f * t /
							   8000)) : 0;
		m->process(in, out, 80);
		now += 10;
		m->poll();
	}
}

/* adaptive answer (+FAA=1): answering, a T.38 offer makes the call a
   fax call (FAX, class 1); without +FAA no; from fax mode the data
   answer runs in data mode, fax mode back after the call; and the
   program's fax receiver (Options::fax_detect) told of a CNG burst,
   or of one cut short by the answer tone, the modem staying in data
   mode */
TEST(Core, AdaptiveAnswer)
{
	Line l;
	Modem *m = make(l, "faa");
	EXPECT_EQ(cmd(m, l, "ATE0+FAA=1\r"), "ATE0+FAA=1\r\r\nOK\r\n");
	cmd(m, l, "ATA\r");
	line(m, 500);
	EXPECT_TRUE(m->adaptive_answering());
	EXPECT_EQ(modem_get_param(m->handle(), MDMPRM_ADAPTIVE), 1);
	l.out.clear();
	EXPECT_EQ(m->t38_start(300), 0);
	EXPECT_EQ(l.out, "FAX\r\n");
	EXPECT_EQ(m->mode(), MODEM_MODE_FAX);
	EXPECT_TRUE(m->fax_caller());
	EXPECT_TRUE(!m->adaptive_answering());
	EXPECT_EQ(modem_get_param(m->handle(), MDMPRM_ADAPTIVE), 0);
	m->hangup_now();
	line(m, 100);
	delete m;

	m = make(l, "nofaa");
	cmd(m, l, "ATE0\r");
	cmd(m, l, "ATA\r");
	line(m, 500);
	EXPECT_TRUE(!m->adaptive_answering());
	EXPECT_EQ(m->t38_start(300), -1);
	m->hangup_now();
	line(m, 100);
	delete m;

	m = make(l, "faa1");
	cmd(m, l, "ATE0+FCLASS=1;+FAA=1\r");
	cmd(m, l, "ATA\r");
	line(m, 500);
	EXPECT_EQ(m->mode(), MODEM_MODE_DATA);
	EXPECT_EQ(modem_get_param(m->handle(), MDMPRM_ADAPTIVE), 1);
	m->hangup_now();
	line(m, 100);
	EXPECT_EQ(m->mode(), MODEM_MODE_FAX);
	delete m;

	dcore::Options o;
	o.name = "detect";
	o.rate = 8000;
	o.fax_detect = true;
	m = new Modem(l, o);
	cmd(m, l, "ATE0\r");
	cmd(m, l, "ATA\r");
	line(m, 300);
	EXPECT_EQ(modem_get_param(m->handle(), MDMPRM_ADAPTIVE), 1);
	l.out.clear();
	line(m, 500, 1100);
	line(m, 300);
	EXPECT_TRUE(m->fax_caller());
	EXPECT_EQ(m->mode(), MODEM_MODE_DATA);
	EXPECT_EQ(l.out, "");
	m->hangup_now();
	line(m, 100);
	delete m;

	/* a CNG burst cut short (0.25 s) as our answer tone starts (S92's
	   2 s of silence and 0.2 s of the pump's after ATA): a fax caller;
	   the same burst 3 s later is not */
	for (int late : { 0, 1 }) {
		m = new Modem(l, o);
		cmd(m, l, "ATE0\r");
		cmd(m, l, "ATA\r");
		line(m, late ? 5300 : 2300);
		line(m, 250, 1100);
		line(m, 300);
		EXPECT_EQ(m->fax_caller(), !late) << late;
		m->hangup_now();
		line(m, 100);
		delete m;
	}
}

/* the line's delay as Smart Link's original library sees it: asked of
   the host (MDMPRM_IODELAY), and its changes passed on to it
   (MDMPRM_UPDATE_DELAY) */
TEST(Core, LineDelay)
{
	Line l;
	Modem *m = make(l, "delay");
	EXPECT_EQ(modem_get_param(m->handle(), MDMPRM_IODELAY), 0);
	l.delay = 128;
	EXPECT_EQ(modem_get_param(m->handle(), MDMPRM_IODELAY), 128);
	EXPECT_EQ(modem_set_param(m->handle(), MDMPRM_UPDATE_DELAY, -48), 0);
	EXPECT_EQ(l.delay_change, -48);
	EXPECT_EQ(modem_set_param(m->handle(), MDMPRM_UPDATE_DELAY, 20), 0);
	EXPECT_EQ(l.delay_change, -28);
	delete m;
}

/* V.92 modem-on-hold's commands: what we grant (+PMH, +PMHT, read by the
   pump as MDMPRM_MOH_T1), and +PMHR, only online (in command mode) */
TEST(Core, ModemOnHold)
{
	Line l;
	Modem *m = make(l, "moh");
	EXPECT_EQ(cmd(m, l, "ATE0+PMH?\r"), "ATE0+PMH?\r\r\n+PMH: 0\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+PMHT?\r"), "+PMHT: 5\r\nOK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 5);
	EXPECT_EQ(cmd(m, l, "AT+PMHT=?\r"), "+PMHT: (0-13)\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+PMHT=9\r"), "OK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 9);
	EXPECT_EQ(cmd(m, l, "AT+PMHT=14\r"), "ERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+PMH=1\r"), "OK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 0);
	EXPECT_EQ(cmd(m, l, "AT+PMH=2\r"), "ERROR\r\n");
	/* without a call */
	EXPECT_EQ(cmd(m, l, "AT+PMHR\r"), "ERROR\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_REQ), 0);
	/* ATZ: granted again, 1 min */
	EXPECT_EQ(cmd(m, l, "ATZ\r"), "OK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 5);
	delete m;
}

/* pulse dialling (ATDP) on an analogue line, by the country's table:
   each digit its pulses (the break and the make as long as the
   country's), the gap between digits; the digit patterns of Sweden
   (n + 1) and New Zealand (10 - n); 'T' back to DTMF; and a line
   without pulses dials in DTMF */
TEST(Core, PulseDialling)
{
	struct Want {
		const char *country, *dial;
		std::vector<int> pulses;        /* a digit's, in turn */
		int brk, mk;                    /* ms */
	};
	for (const Want &w : { Want{ "USA", "ATX3DP102\r", { 1, 10, 2 }, 64, 36 },
			       Want{ "SWEDEN", "ATX3DP12\r", { 2, 3 }, 60, 40 },
			       Want{ "NEW_ZEALAND", "ATX3DP12\r", { 9, 8 }, 67, 33 },
			       Want{ "USA", "ATX3DP1T2\r", { 1 }, 64, 36 } }) {
		SCOPED_TRACE(std::string(w.country) + " " + w.dial);
		Line l;
		l.analog = l.pulses = true;
		Modem *m = make(l, "pulse", w.country);
		int16_t in[80] = {}, out[80];
		cmd(m, l, w.dial);
		for (int i = 0; i < 100 * 12; i++) {     /* 12 s of 10 ms */
			now += 10;
			if (m->started())
				m->process(in, out, 80);
			l.samples += 80;
			m->poll();
		}
		/* breaks and makes in turn, from a break */
		std::vector<int> got;
		int n = 0;
		for (size_t k = 0; k < l.edges.size(); k++) {
			EXPECT_EQ(l.edges[k].second, k % 2 == 0);
			if (k % 2 == 0) {
				if (k > 0 && l.edges[k].first - l.edges[k - 1].first > 8 * 300) {
					got.push_back(n);       /* a gap: the digit's end */
					n = 0;
				}
				n++;
			} else {
				long brk = (long)(l.edges[k].first - l.edges[k - 1].first);
				EXPECT_NEAR(brk, 8 * w.brk, 80);  /* (a block) */
			}
			if (k % 2 == 0 && k > 0 && n > 1) {
				long mk = (long)(l.edges[k].first - l.edges[k - 1].first);
				EXPECT_NEAR(mk, 8 * w.mk, 80);
			}
		}
		if (n)
			got.push_back(n);
		EXPECT_EQ(got, w.pulses);
		/* pulse dialling on before the first break, over after the
		   last make (at the string's end or its T) */
		ASSERT_EQ(l.dialling.size(), 2u);
		EXPECT_TRUE(l.dialling[0].second);
		EXPECT_FALSE(l.dialling[1].second);
		ASSERT_FALSE(l.edges.empty());
		EXPECT_LE(l.dialling[0].first, l.edges.front().first);
		EXPECT_GE(l.dialling[1].first, l.edges.back().first);
		delete m;
	}
	/* no pulses on the line: DTMF, no break */
	Line l;
	l.analog = true;
	Modem *m = make(l, "tone");
	int16_t in[80] = {}, out[80];
	cmd(m, l, "ATX3DP5\r");
	float peak = 0;
	for (int i = 0; i < 100 * 4; i++) {
		now += 10;
		if (m->started()) {
			m->process(in, out, 80);
			for (int16_t x : out)
				peak = std::max(peak, (float)abs(x));
		}
		m->poll();
	}
	EXPECT_TRUE(l.edges.empty());
	EXPECT_GT(peak, 3000.0f);               /* the digit's tones */
	/* (pulse dialling begun and over at once) */
	ASSERT_EQ(l.dialling.size(), 2u);
	EXPECT_TRUE(l.dialling[0].second);
	EXPECT_FALSE(l.dialling[1].second);
	delete m;
}

/* the result codes V.253 allows in voice mode (Table 3: no NO CARRIER)
   when the line ends a call (Modem::line_dropped(), the far end hung
   up) or a voice dial fails (line_failed()): <DLE>l alone in the
   command state; +VRX's samples end with <DLE><ETX>, then OK; +VTX's and
   +VTR's go on until the DTE ends them (what it sends meanwhile
   discarded), then OK, +VTR's after <DLE><ETX>; +VTS's tones ERROR; a
   voice dial's NO CARRIER ERROR.  The line is left (on hook, +VLS=0). */
TEST(Core, VoiceLineGone)
{
	Line l;
	Modem *m = make(l, "voice");
	int16_t in[160] = {}, out[160];
	/* blocks of the line's samples (20 ms each), what the DTE got */
	auto run = [&](int blocks) {
		l.out.clear();
		for (int i = 0; i < blocks; i++) {
			now += 20;
			if (m->started())
				m->process(in, out, 160);
			m->poll();
		}
		return l.out;
	};
	/* the line taken, then cmd's result after a block */
	auto taken = [&](const char *c) {
		cmd(m, l, "AT+VLS=1\r");
		EXPECT_EQ(run(1), "OK\r\n");
		cmd(m, l, c);
		return run(1);
	};
	auto gone = [&]() {
		l.out.clear();
		m->line_dropped();
		return l.out;
	};

	EXPECT_EQ(cmd(m, l, "ATE0+FCLASS=8\r"), "ATE0+FCLASS=8\r\r\nOK\r\n");
	/* the command state */
	cmd(m, l, "AT+VLS=1\r");
	EXPECT_EQ(run(1), "OK\r\n");
	EXPECT_EQ(gone(), "\x10l");
	EXPECT_FALSE(m->started());
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "+VLS=0\r\nOK\r\n");

	EXPECT_EQ(taken("AT+VRX\r").substr(0, 9), "CONNECT\r\n");
	EXPECT_EQ(gone(), "\x10l\x10\x03OK\r\n");
	EXPECT_EQ(cmd(m, l, "AT\r"), "OK\r\n");

	EXPECT_EQ(taken("AT+VTX\r"), "CONNECT\r\n");
	EXPECT_EQ(gone(), "\x10l");
	EXPECT_EQ(cmd(m, l, "AT\rx\x10\x10\x10~"), "");
	EXPECT_EQ(cmd(m, l, "\x10\x03" "AT+VLS?\r"), "OK\r\n+VLS=0\r\nOK\r\n");

	EXPECT_EQ(taken("AT+VTR\r").substr(0, 9), "CONNECT\r\n");
	EXPECT_EQ(gone(), "\x10l");
	EXPECT_EQ(cmd(m, l, "AT\r\x10\x03\x10!"), "");
	EXPECT_EQ(cmd(m, l, "\x10^AT\r"), "\x10\x03OK\r\nOK\r\n");

	cmd(m, l, "AT+VLS=1\r");
	run(1);
	cmd(m, l, "AT+VTS=[440,0,100]\r");
	EXPECT_EQ(run(2), "");
	EXPECT_EQ(gone(), "\x10lERROR\r\n");

	/* a voice dial the line fails (FAILED 404, its socket's end) */
	cmd(m, l, "AT+VRN=20\r");
	EXPECT_EQ(cmd(m, l, "ATD5551000\r"), "");
	run(2);
	l.out.clear();
	m->line_failed(Result::NoCarrier);
	m->line_dropped();
	EXPECT_EQ(l.out, "ERROR\r\n");
	EXPECT_EQ(cmd(m, l, "ATD5551000\r"), "");
	EXPECT_EQ(gone(), "ERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "+VLS=0\r\nOK\r\n");
	delete m;
}

/* the local phone (a phone on the modem's jack) off and on hook: in
   voice mode V.253's event 8, <DLE>H and <DLE>h, once a change (in any
   other mode nothing); +VLS=? lists event 8 in every state of both
   devices on a line that senses it */
TEST(Core, LocalPhone)
{
	Line l;
	Modem *m = make(l, "phone");
	l.local_phone = true;
	auto change = [&](bool off) {
		l.out.clear();
		m->local_phone(off);
		return l.out;
	};

	EXPECT_EQ(change(true), "");            /* data mode */
	EXPECT_TRUE(m->local_phone_off_hook());
	EXPECT_EQ(change(false), "");
	EXPECT_EQ(cmd(m, l, "ATE0+FCLASS=8\r"), "ATE0+FCLASS=8\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=?\r"),
		  "0,\"\",00800000,00800000,90800000\r\n"
		  "1,\"T\",0B843060,0B843060,0B843060\r\nOK\r\n");
	EXPECT_EQ(change(true), "\x10H");
	EXPECT_EQ(change(true), "");            /* no change */
	EXPECT_EQ(change(false), "\x10h");
	EXPECT_FALSE(m->local_phone_off_hook());
	delete m;
}

/* caller ID on the line as a sender would put it there */
namespace cidline {

/* an MDMF message (0x80): its parameters (type, value), the checksum */
static std::vector<u8> mdmf(std::vector<std::pair<u8, std::string>> ps)
{
	std::vector<u8> m = { 0x80, 0 };
	u8 sum = 0;
	for (auto &p : ps) {
		m.push_back(p.first);
		m.push_back(static_cast<u8>(p.second.size()));
		m.insert(m.end(), p.second.begin(), p.second.end());
	}
	m[1] = static_cast<u8>(m.size() - 2);
	for (u8 b : m)
		sum = static_cast<u8>(sum + b);
	m.push_back(static_cast<u8>(-sum));
	return m;
}

/* msg as FSK at 1200 bit/s, mark and space Hz: 50 ms of silence, the
   channel seizure (300 bits of 0101...), 180 marks, the octets 8N1, a
   few marks, 50 ms of silence; at 8000 Hz, -13 dBm or so */
static std::vector<s16> fsk(const std::vector<u8> &msg, float mark,
			    float space)
{
	std::vector<s16> bits;
	for (int i = 0; i < 300; i++)
		bits.push_back(i & 1);
	bits.insert(bits.end(), 180, 1);
	for (u8 b : msg) {
		bits.push_back(0);
		for (int k = 0; k < 8; k++)
			bits.push_back((b >> k) & 1);
		bits.push_back(1);
	}
	bits.insert(bits.end(), 10, 1);
	std::vector<s16> x(400, 0);
	dsp::FskBitTx tx;
	size_t at = 0;
	tx.init(mark, space, 6000, 1200, 8000, 1);
	for (;;) {
		float y = tx.sample([&]() -> s16 {
			return at < bits.size() ? bits[at++] : -1;
		});
		if (tx.ended)
			break;
		x.push_back(static_cast<s16>(y));
	}
	x.insert(x.end(), 400, 0);
	return x;
}

/* the digits of s as DTMF, 70 ms each with 70 ms between, then 100 ms
   of silence */
static std::vector<s16> dtmf(const char *s)
{
	std::vector<s16> x;
	for (; *s; s++) {
		dsp::DtmfGen g;
		g.set(*s, 8000);
		for (int i = 0; i < 560; i++)
			x.push_back(static_cast<s16>(g.next()));
		x.insert(x.end(), 560, 0);
	}
	x.insert(x.end(), 800, 0);
	return x;
}

} // namespace cidline

/* caller ID before the first ring, on the samples of an idle line
   (idle_audio()), as +VCID=1 reports it: V.23 FSK (ETSI, the UK), Bell
   202 FSK, DTMF (a number, a withheld one, one ended by a pause); none
   with +VCID=0 */
TEST(Core, CallerIdBeforeRing)
{
	using namespace cidline;
	Line l;
	Modem *m = make(l, "cidpre");
	auto idle = [&](const std::vector<s16> &x) {
		l.out.clear();
		for (size_t k = 0; k < x.size(); k += 160)
			m->idle_audio(x.data() + k, static_cast<s32>(
				std::min<size_t>(160, x.size() - k)));
		return l.out;
	};
	std::vector<u8> msg = mdmf({ { 0x01, "10301043" },
				     { 0x02, "01632960123" },
				     { 0x07, "J SMITH" } });
	const std::string want = "\r\nDATE = 1030\r\nTIME = 1043\r\n"
		"NMBR = 01632960123\r\nNAME = J SMITH\r\n";

	EXPECT_EQ(cmd(m, l, "ATE0+VCID=1\r"), "ATE0+VCID=1\r\r\nOK\r\n");
	EXPECT_EQ(idle(fsk(msg, 1300, 2100)), want);
	EXPECT_EQ(idle(fsk(msg, 1200, 2200)), want);
	EXPECT_EQ(idle(dtmf("A4512345678C")), "\r\nNMBR = 4512345678\r\n");
	EXPECT_EQ(idle(dtmf("B10C")), "\r\nNMBR = P\r\n");
	EXPECT_EQ(idle(dtmf("D00#")), "\r\nNMBR = O\r\n");
	std::vector<s16> paused = dtmf("A123");
	paused.insert(paused.end(), 4800, 0);
	EXPECT_EQ(idle(paused), "\r\nNMBR = 123\r\n");
	/* a message with a bad checksum: nothing */
	std::vector<u8> bad = msg;
	bad.back() ^= 1;
	EXPECT_EQ(idle(fsk(bad, 1300, 2100)), "");
	EXPECT_EQ(cmd(m, l, "AT+VCID=0\r"), "OK\r\n");
	EXPECT_EQ(idle(fsk(msg, 1300, 2100)), "");
	delete m;
}

/* a line reversal (line_reversal()) snoops the line for caller ID
   before the ring; the message ends it */
TEST(Core, CallerIdAfterLineReversal)
{
	using namespace cidline;
	Line l;
	Modem *m = make(l, "cidrev");
	std::vector<s16> x = fsk(mdmf({ { 0x02, "5551000" },
					{ 0x08, "P" } }), 1300, 2100);
	int16_t out[160];

	EXPECT_EQ(cmd(m, l, "ATE0+VCID=1\r"), "ATE0+VCID=1\r\r\nOK\r\n");
	l.out.clear();
	m->line_reversal();
	EXPECT_TRUE(m->started());
	EXPECT_EQ(l.last_hook, MODEM_HOOK_SNOOPING);
	for (size_t k = 0; k + 160 <= x.size() && m->started(); k += 160) {
		now += 20;
		m->process(x.data() + k, out, 160);
		m->poll();
	}
	EXPECT_EQ(l.out, "\r\nNMBR = 5551000\r\nNAME = P\r\n");
	EXPECT_FALSE(m->started());
	delete m;
}

/* Type II caller ID in voice mode on the call: the CPE alerting signal
   (2130 + 2750 Hz, 80 ms), our DTMF A after it, then the FSK message,
   reported as a <DLE>X packet; the line's audio to the DTE silent
   meanwhile; with +VCID=0 no acknowledgement */
TEST(Core, CallerIdOnTheCall)
{
	using namespace cidline;
	Line l;
	Modem *m = make(l, "cid2");
	std::vector<s16> sent;
	auto run = [&](const std::vector<s16> &x) {
		std::vector<s16> in(x);
		in.resize((in.size() + 159) / 160 * 160, 0);
		int16_t out[160];
		l.out.clear();
		for (size_t k = 0; k < in.size(); k += 160) {
			now += 20;
			m->process(in.data() + k, out, 160);
			m->poll();
			sent.insert(sent.end(), out, out + 160);
		}
		return l.out;
	};
	std::vector<s16> cas(640);
	for (int i = 0; i < 640; i++)
		cas[i] = static_cast<s16>(
			4000 * sin(2 * M_PI * 2130 * i / 8000) +
			4000 * sin(2 * M_PI * 2750 * i / 8000));
	cas.insert(cas.end(), 1600, 0);         /* 200 ms for the ack */
	std::vector<s16> msg = fsk(mdmf({ { 0x02, "5551000" },
					  { 0x07, "BOB" } }), 1200, 2200);
	/* the DTMF digit in sent's samples [from, to), ' ' for none */
	auto key = [&](size_t from, size_t to) {
		dsp::DtmfDetect d;
		d.init(8000);
		for (size_t i = from; i < to && i < sent.size(); i++)
			if (d.sample(sent[i]) == dsp::DtmfDetect::ON)
				return d.digit;
		return ' ';
	};

	EXPECT_EQ(cmd(m, l, "ATE0+FCLASS=8;+VCID=1\r"),
		  "ATE0+FCLASS=8;+VCID=1\r\r\nOK\r\n");
	cmd(m, l, "AT+VLS=1\r");
	EXPECT_EQ(run(std::vector<s16>(160)), "OK\r\n");
	sent.clear();
	EXPECT_EQ(run(cas), "");
	EXPECT_EQ(key(0, sent.size()), 'A');
	std::string got = run(msg);
	EXPECT_NE(got.find("\x10XNMBR = 5551000\r\nNAME = BOB\r\n\x10."),
		  std::string::npos) << got;

	/* +VCID=0: no acknowledgement, no report */
	EXPECT_EQ(cmd(m, l, "AT+VCID=0\r"), "OK\r\n");
	sent.clear();
	EXPECT_EQ(run(cas), "");
	EXPECT_EQ(key(0, sent.size()), ' ');
	EXPECT_EQ(run(msg), "");
	cmd(m, l, "AT+VLS=0\r");
	delete m;
}

/* a SIP call's caller (caller_id()) after its first RING, as +VCID asks:
   formatted (1), the MDMF message in hex (2), a voice mode packet, none
   (0), withheld and unknown fields; only the first RING has it */
TEST(Core, CallerIdFromLine)
{
	Line l;
	Modem *m = make(l, "cid");
	auto ring = [&](const char *date, const char *nmbr, const char *name) {
		l.out.clear();
		m->caller_id(date, nmbr, name);
		m->ring_signal(true);
		std::string first = l.out;
		l.out.clear();
		m->ring_signal(true);
		EXPECT_EQ(l.out, "RING\r\n");
		m->ring_signal(false);
		return first;
	};

	EXPECT_EQ(cmd(m, l, "ATE0+VCID=1\r"), "ATE0+VCID=1\r\r\nOK\r\n");
	EXPECT_EQ(ring("10301043", "+15551234", "Alice Smith"),
		  "\r\nRING\r\n\r\nDATE = 1030\r\nTIME = 1043\r\n"
		  "NMBR = +15551234\r\nNAME = Alice Smith\r\n");
	EXPECT_EQ(ring("10301043", nullptr, nullptr),
		  "\r\nRING\r\n\r\nDATE = 1030\r\nTIME = 1043\r\n"
		  "NMBR = P\r\nNAME = P\r\n");
	EXPECT_EQ(ring("", "5551000", ""),
		  "\r\nRING\r\n\r\nNMBR = 5551000\r\nNAME = O\r\n");
	/* 0x80, 13 octets: 0x02 "5551000", 0x08 'O'; the checksum */
	EXPECT_EQ(cmd(m, l, "AT+VCID=2\r"), "OK\r\n");
	EXPECT_EQ(ring("", "5551000", ""),
		  "\r\nRING\r\n\r\nMESG = 800C0207353535313030300801"
		  "4FB3\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VCID=0\r"), "OK\r\n");
	EXPECT_EQ(ring("10301043", "5551000", "Bob"), "\r\nRING\r\n");
	/* voice mode: the packet */
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=8;+VCID=1\r"), "OK\r\n");
	std::string v = ring("", "5551000", "Bob");
	EXPECT_NE(v.find("\x10XNMBR = 5551000\r\nNAME = Bob\r\n\x10."),
		  std::string::npos) << v;
	delete m;
}

/* a handset on the modem (has_handset()): +VLS=? lists it alone (14,
   its DTMF digits) and on the call (15); +VLS=14 starts the voice engine
   on it with the line on hook, 15 or 1 takes the line (the engine going
   on), 14 leaves it again, and each move is a voice_route(); +VLS=0, or
   ATH, puts the audio back on the line.  Without a handset 14 and 15 are
   an ERROR, as is any other device */
TEST(Core, VoiceHandset)
{
	Line l;
	Modem *m = make(l, "handset");
	int16_t in[160] = {}, out[160];
	auto run = [&](int blocks) {
		l.out.clear();
		for (int i = 0; i < blocks; i++) {
			now += 20;
			if (m->started())
				m->process(in, out, 160);
			m->poll();
		}
		return l.out;
	};

	EXPECT_EQ(cmd(m, l, "ATE0+FCLASS=8\r"), "ATE0+FCLASS=8\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=14\r"), "ERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=15\r"), "ERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=2\r"), "ERROR\r\n");
	EXPECT_TRUE(l.routes.empty());
	l.handset = true;
	EXPECT_EQ(cmd(m, l, "AT+VLS=?\r"),
		  "0,\"\",00000000,00000000,90000000\r\n"
		  "1,\"T\",0B043060,0B043060,0B043060\r\n"
		  "14,\"H\",08000000,08000000,08000000\r\n"
		  "15,\"HT\",0B043060,0B043060,0B043060\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=13\r"), "ERROR\r\n");

	l.last_hook = -1;
	cmd(m, l, "AT+VLS=14\r");
	EXPECT_EQ(run(1), "OK\r\n");
	EXPECT_TRUE(m->started());
	EXPECT_EQ(l.last_hook, -1);             /* still on hook */
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "+VLS=14\r\nOK\r\n");
	cmd(m, l, "AT+VRX\r");
	EXPECT_EQ(run(1).substr(0, 9), "CONNECT\r\n");
	run(5);
	cmd(m, l, "x");
	run(1);
	EXPECT_NE(l.out.find("\x10\x03OK\r\n"), std::string::npos);
	EXPECT_EQ(cmd(m, l, "AT+VLS=15\r"), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "+VLS=15\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=1\r"), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "AT+VLS=14\r"), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	EXPECT_TRUE(m->started());
	EXPECT_EQ(cmd(m, l, "AT+VLS=0\r"), "OK\r\n");
	EXPECT_FALSE(m->started());
	EXPECT_EQ(l.routes, (std::vector<unsigned>{ 14, 15, 1, 14, 1 }));

	l.routes.clear();
	cmd(m, l, "AT+VLS=15\r");
	EXPECT_EQ(run(1), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "ATH\r"), "OK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	EXPECT_EQ(l.routes, (std::vector<unsigned>{ 15, 1 }));
	delete m;
}

/* V.253's event reports: +VLS=? lists each device with the events it
   reports in the transmit, receive and command states (Table 1's bits,
   event 0 leftmost); +VEM, the speakerphone's reports, 0 or 1, back to
   0 with +VIP; neither outside voice mode */
TEST(Core, VoiceEvents)
{
	Line l;
	Modem *m = make(l, "events");

	EXPECT_EQ(cmd(m, l, "ATE0+VEM?\r"), "ATE0+VEM?\r\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=8\r"), "OK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=?\r"),
		  "0,\"\",00000000,00000000,90000000\r\n"
		  "1,\"T\",0B043060,0B043060,0B043060\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "+VLS=0\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "+VEM=0\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM=?\r"), "+VEM=0-1\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM=1\r"), "OK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "+VEM=1\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM=2\r"), "ERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "+VEM=1\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VIP\r"), "OK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "+VEM=0\r\nOK\r\n");
	delete m;
}

/* two modems back to back */
struct Pair {
	Line la, lb;
	Modem *a, *b;
	std::vector<int16_t> ia, ib, oa, ob;
	std::string got_a, got_b;       /* what each DTE received */
	static const int BLOCK = 160;
	/* two modems, ATE0X3 and the modulation command ms */
	Pair(const char *ms) {
		a = make(la, "A");
		b = make(lb, "B");
		ia.assign(BLOCK, 0);
		ib = oa = ob = ia;
		std::string s = std::string("ATE0X3") + ms + "\r";
		a->dte_input(s.data(), s.size());
		b->dte_input(s.data(), s.size());
		la.out.clear();
		lb.out.clear();
	}
	/* the modems freed */
	~Pair() {
		delete a;
		delete b;
	}
	/* 20 ms of audio both ways, the timers, the DTEs' output */
	void step() {
		if (a->started())
			a->process(ia.data(), oa.data(), BLOCK);
		else
			std::fill(oa.begin(), oa.end(), 0);
		if (b->started())
			b->process(ib.data(), ob.data(), BLOCK);
		else
			std::fill(ob.begin(), ob.end(), 0);
		ia = ob;
		ib = oa;
		/* T.38's packets across */
		for (int k = 0; k < 2; k++) {
			Line &from = k ? lb : la;
			Modem *to = k ? a : b;
			for (const std::string &pkt : from.t38) {
				struct dsp::t38_ifp ifp;
				if (!dsp::t38_ifp_decode(&ifp, 1,
						(const unsigned char *)pkt.data(),
						pkt.size()))
					to->t38_rx(&ifp);
			}
			from.t38.clear();
		}
		now += BLOCK / 8;
		a->poll();
		b->poll();
		got_a += la.out;
		got_b += lb.out;
		la.out.clear();
		lb.out.clear();
	}
	/* steps until cond or ms; cond's value */
	bool run_until(bool (*cond)(Pair &), int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond(*this))
				return true;
			step();
		}
		return cond(*this);
	}
};

/* n pseudo-random octets from who: text that compresses, any octet */
static std::string data(char who, int n)
{
	std::string d;
	unsigned x = who;
	while ((int)d.size() < n) {
		x = x * 1103515245 + 12345;
		/* text that compresses, and every byte value */
		d += (char)((x >> 16) & 1 ? "hello modem "[d.size() % 12] :
			    (x >> 8) & 0xff);
	}
	return d;
}

/* a call: its modulation command, its least rate (bit/s), the data */
struct Call {
	const char *ms;
	unsigned min_rate;
	int bytes;
};

class CoreCall : public testing::TestWithParam<Call> {};

/* a call: online, data both ways byte for byte, the escape, the remote
   hangs up (NO CARRIER) */
TEST_P(CoreCall, DataBothWaysAndHangup)
{
	const char *ms = GetParam().ms;
	unsigned min_rate = GetParam().min_rate;
	int bytes = GetParam().bytes;
	Pair p(ms);
	std::string da = data('a', bytes), db = data('b', bytes * 3 / 4);
	size_t sa = 0, sb = 0;

	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	EXPECT_EQ(p.a->link_state(), LinkState::Connecting);
	EXPECT_TRUE(p.a->caller() && !p.b->caller());
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	printf("%s: online at %u/%u bit/s\n", ms, p.a->tx_rate(),
	       p.a->rx_rate());
	EXPECT_TRUE(p.a->tx_rate() >= min_rate && p.b->rx_rate() >= min_rate);
	EXPECT_TRUE(p.a->carrier() && p.b->carrier());
	EXPECT_TRUE(p.got_a.find("CONNECT") != std::string::npos);
	p.got_a.clear();
	p.got_b.clear();
	/* the data, both ways, as fast as the modems take it */
	for (int t = 0; t < 120000; t += 20) {
		int n = p.a->dte_space();
		if (n > (int)(da.size() - sa))
			n = da.size() - sa;
		sa += p.a->dte_input(da.data() + sa, n);
		n = p.b->dte_space();
		if (n > (int)(db.size() - sb))
			n = db.size() - sb;
		sb += p.b->dte_input(db.data() + sb, n);
		p.step();
		if (p.got_b.size() >= da.size() && p.got_a.size() >= db.size())
			break;
	}
	EXPECT_TRUE(p.got_b == da) << "B got " << p.got_b.size() << "/" <<
		da.size() << " bytes";
	EXPECT_TRUE(p.got_a == db) << "A got " << p.got_a.size() << "/" <<
		db.size() << " bytes";
	p.run_until([](Pair &q) {
		return !q.a->tx_pending() && !q.b->tx_pending(); }, 5000);
	EXPECT_TRUE(!p.a->tx_pending() && !p.b->tx_pending());

	/* the escape: guard time, +++, guard time; the timer says when */
	p.run_until([](Pair &) { return false; }, 1100);
	p.a->dte_input("+++", 3);
	EXPECT_TRUE(p.a->next_timer_ms() > 400 && p.a->next_timer_ms() <= 500);
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos; }, 1000));
	EXPECT_EQ(p.a->link_state(), LinkState::Online);
	p.got_a.clear();
	/* the remote hangs up; we hear NO CARRIER */
	p.b->hangup_now();
	EXPECT_TRUE(!p.b->started());
	EXPECT_EQ(p.b->link_state(), LinkState::Idle);
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return !q.a->started(); }, 15000));
	EXPECT_EQ(p.a->link_state(), LinkState::Idle);
	EXPECT_TRUE(!p.a->carrier());
	EXPECT_TRUE(p.la.starts == 1 && p.la.stops == 1);
	EXPECT_TRUE(p.got_a.find("NO CARRIER") != std::string::npos);
	if (getenv("SHOW"))
		printf("A after the hangup: [%s]\n", p.got_a.c_str());
}

/* the escape while the DTE's data still waits for the line (V.22 at
   1200 bit/s, 800 octets): none of it lost, the +++ after it as data,
   all of it to the remote once ATO is back online; the command
   interpreter takes none of it */
TEST(Core, EscapeKeepsData)
{
	Pair p("+MS=22");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	p.got_a.clear();
	p.got_b.clear();
	std::string d = data('a', 800);
	size_t sent = 0;
	while (sent < d.size()) {
		sent += p.a->dte_input(d.data() + sent,
				       std::min<int>(p.a->dte_space(),
						     d.size() - sent));
		p.step();
	}
	/* guard time, +++, guard time: OK with octets still waiting */
	p.run_until([](Pair &) { return false; }, 1100);
	p.a->dte_input("+++", 3);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos; }, 2000));
	EXPECT_LT(p.got_b.size(), d.size()) << "nothing was waiting";
	EXPECT_TRUE(p.a->tx_pending());
	p.got_a.clear();
	p.a->dte_input("ATO\r", 4);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find("CONNECT") != std::string::npos; }, 5000));
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.got_b.size() >= 803; }, 15000));
	EXPECT_EQ(p.got_b, d + "+++");
	EXPECT_EQ(p.got_a.find("ERROR"), std::string::npos) << p.got_a;
}

/* a fax machine (class 1, its CNG) calls a modem in voice mode, on
   the line (+VLS=1): <DLE>c; +FCLASS=1 keeps the call, and ATA on it
   is the fax answer (CED and the DIS's flags: the caller's CONNECT).
   With +FAA=1 the modem switches by itself: FAX, then CONNECT.  ATH
   ends a held call. */
TEST(Core, VoiceFaxCall)
{
	for (int faa : { 0, 1 }) {
		Pair p("");
		auto has = [](const std::string &s, const char *x) {
			return s.find(x) != std::string::npos;
		};
		p.a->dte_input("AT+FCLASS=1\r", 12);
		std::string vb = std::string("AT+FCLASS=8;+FAA=") +
			(faa ? "1" : "0") + ";+VLS=1\r";
		p.b->dte_input(vb.data(), vb.size());
		p.step();
		p.a->dte_input("ATD\r", 4);
		ASSERT_TRUE(p.run_until([](Pair &q) {
			return q.got_b.find("\x10" "c") != std::string::npos;
		}, 8000)) << faa;
		if (!faa) {
			p.got_b.clear();
			p.b->dte_input("AT+FCLASS=1\r", 12);
			p.step();
			EXPECT_EQ(p.got_b, "OK\r\n");
			EXPECT_TRUE(p.b->started());
			EXPECT_EQ(p.b->mode(), MODEM_MODE_FAX);
			p.b->dte_input("ATA\r", 4);
		}
		ASSERT_TRUE(p.run_until([](Pair &q) {
			return q.got_a.find("CONNECT") != std::string::npos &&
				q.got_b.find("CONNECT") != std::string::npos;
		}, 10000)) << faa << ": A [" << p.got_a << "] B [" <<
			p.got_b << "]";
		if (getenv("SHOW"))
			printf("+FAA=%d: A [%s] B [%s]\n", faa,
			       p.got_a.c_str(), p.got_b.c_str());
		EXPECT_EQ(has(p.got_b, "FAX\r\n"), faa == 1);
		EXPECT_EQ(p.b->mode(), MODEM_MODE_FAX);
	}

	/* ATH on the held line */
	Line l;
	Modem *m = make(l, "held");
	int16_t in[160] = {}, out[160];
	cmd(m, l, "ATE0+FCLASS=8;+VLS=1\r");
	m->process(in, out, 160);
	EXPECT_TRUE(m->started());
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=0\r"), "OK\r\n");
	EXPECT_TRUE(m->started());
	EXPECT_EQ(cmd(m, l, "ATH\r"), "OK\r\n");
	EXPECT_TRUE(!m->started());
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	delete m;
}

/* a data modem's V.25 calling tone (1300 Hz, 0.6 s every 2.6 s) on a
   voice call: <DLE>e, at most once in 4 s; with +FAA=1 the modem goes
   over to data mode and its adaptive answer */
TEST(Core, VoiceDataCallingTone)
{
	for (int faa : { 0, 1 }) {
		Line l;
		Modem *m = make(l, "ct");
		std::string c = std::string("ATE0+FCLASS=8;+FAA=") +
			(faa ? "1" : "0") + ";+VLS=1\r";
		cmd(m, l, c.c_str());
		line(m, 100);
		l.out.clear();
		for (int k = 0; k < 3; k++) {
			line(m, 600, 1300);
			line(m, 2000);
		}
		size_t n = 0;
		for (size_t i = l.out.find("\x10" "e"); i != std::string::npos;
		     i = l.out.find("\x10" "e", i + 1))
			n++;
		if (faa) {
			EXPECT_EQ(n, 1u);
			EXPECT_EQ(m->mode(), MODEM_MODE_DATA);
			EXPECT_TRUE(m->adaptive_answering());
		} else {
			EXPECT_EQ(n, 2u) << l.out;
			EXPECT_EQ(m->mode(), MODEM_MODE_VOICE);
		}
		m->hangup_now();
		line(m, 100);
		delete m;
	}
}

static int log_lines;

/* the log hook gets the modem's lines */
TEST(Core, Log)
{
	dcore::log_set_hook([](const char *) { log_lines++; });
	dcore::log_level = 1;
	Line l;
	Modem *m = make(l, "log");
	cmd(m, l, "ATI0\r");
	delete m;
	dcore::log_level = 0;
	dcore::log_set_hook(nullptr);
	EXPECT_TRUE(log_lines > 0);
}

/* V.80 framed frames for the DCE: <EM><flag>, then each frame with
   EM, 99h, DC1 and DC3 shielded and <EM><flag> after it */
static std::string v80_frames(const std::vector<std::string> &frames)
{
	static const char em = 0x19;
	std::string s = { em, (char)0xb1 };
	for (const std::string &f : frames) {
		for (char c : f) {
			unsigned char u = (unsigned char)c;
			int t = u == 0x19 ? 0x5c : u == 0x99 ? 0x76 :
				u == 0x11 ? 0xa0 : u == 0x13 ? 0xa1 : -1;
			if (t < 0) {
				s += c;
			} else {
				s += em;
				s += (char)t;
			}
		}
		s += em;
		s += (char)0xb1;
	}
	return s;
}

/* what a V.80 DCE gave its DTE, from the first <EM><err> (the
   receiver's hunt finding a flag) on: the frames ended with <EM><flag>
   (good) or <EM><err> (bad), as text "<frame>|" and "<frame>!" */
static std::string v80_received(const std::string &got)
{
	size_t i = 0;
	for (; i + 1 < got.size(); i++)
		if (got[i] == 0x19 && (unsigned char)got[i + 1] == 0xb2)
			break;
	std::string out, frame;
	for (i += 2; i < got.size(); i++) {
		unsigned char c = (unsigned char)got[i];
		if (c != 0x19 || i + 1 == got.size()) {
			frame += (char)c;
			continue;
		}
		unsigned char code = (unsigned char)got[++i];
		switch (code) {
		case 0x5c: frame += (char)0x19; break;
		case 0x76: frame += (char)0x99; break;
		case 0xa0: frame += (char)0x11; break;
		case 0xa1: frame += (char)0x13; break;
		case 0xb1: out += frame + "|"; frame.clear(); break;
		case 0xb2: out += frame + "!"; frame.clear(); break;
		default: out += "<" + std::to_string(code) + ">"; break;
		}
	}
	return out + frame;
}

/* V.80 Synchronous Access Mode end to end (+ES=6 calling, 8
   answering; +ESA's FCS-16): frames each way between the DTEs, then
   <EM><ecs> (online command state, OK, ATO back), and <EM><eot>: the
   carrier ends, NO CARRIER at both, <EM><eot> before it at the other */
TEST(Core, V80)
{
	Pair p("+MS=132;+ES=6,,8;+ESA=,,,,1;+ITF=383,128,0");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ES?\r"), "+ES: 6,0,8\r\nOK\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ESA?\r"),
		  "+ESA: 0,0,0,0,1,0,255,255\r\nOK\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ITF=10,20\r"), "ERROR\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ES=5\r"), "ERROR\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ESA=3\r"), "ERROR\r\n");
	p.la.out.clear();

	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_TRUE(p.got_a.find("CONNECT") != std::string::npos);
	EXPECT_TRUE(p.got_b.find("CONNECT") != std::string::npos);
	/* transparent at first: the DTE's first underrun reported */
	size_t c = p.got_a.find("CONNECT");
	EXPECT_TRUE(p.got_a.find("\x19\xb4", c) != std::string::npos);
	/* both framed; the frames */
	std::string fa = data('a', 200), fb = data('b', 120);
	std::vector<std::string> to_b = { "hello", fa, std::string(1, 0x19) },
		to_a = { fb, "x" };
	std::string sa = v80_frames(to_b), sb = v80_frames(to_a);
	EXPECT_EQ(p.a->dte_input(sa.data(), sa.size()), (int)sa.size());
	EXPECT_EQ(p.b->dte_input(sb.data(), sb.size()), (int)sb.size());
	p.got_a.clear();
	p.got_b.clear();
	p.run_until([](Pair &) { return false; }, 2000);
	std::string want_b, want_a;
	for (const std::string &f : to_b)
		want_b += f + "|";
	for (const std::string &f : to_a)
		want_a += f + "|";
	EXPECT_EQ(v80_received(p.got_b), want_b);
	EXPECT_EQ(v80_received(p.got_a), want_a);
	/* +++ is data here */
	std::string plus = "+++";
	p.run_until([](Pair &) { return false; }, 1100);
	p.a->dte_input(plus.data(), plus.size());
	p.run_until([](Pair &) { return false; }, 1500);
	EXPECT_TRUE(p.got_a.find("OK") == std::string::npos);
	/* <EM><ecs>: online command state */
	p.got_a.clear();
	std::string ecs = { 0x19, (char)0xbb };
	p.a->dte_input(ecs.data(), ecs.size());
	p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos; }, 1000);
	EXPECT_EQ(p.got_a, std::string("\x19\xbb\r\nOK\r\n"));
	EXPECT_EQ(cmd(p.a, p.la, "ATO\r").find("CONNECT") != std::string::npos,
		  true);
	/* framed still: another frame */
	p.got_b.clear();
	std::string more = { 'o', 'k', 0x19, (char)0xb1 };
	p.a->dte_input(more.data(), more.size());
	p.run_until([](Pair &) { return false; }, 1000);
	EXPECT_TRUE(p.got_b.find(std::string("ok\x19\xb1")) !=
		    std::string::npos);
	/* <EM><eot>: the carrier ends */
	p.got_a.clear();
	p.got_b.clear();
	std::string eot = { 0x19, (char)0xba };
	p.a->dte_input(eot.data(), eot.size());
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return !q.a->started() && !q.b->started(); }, 20000));
	EXPECT_EQ(p.got_a.substr(0, 2), std::string("\x19\xba"));
	EXPECT_TRUE(p.got_a.find("NO CARRIER") != std::string::npos);
	size_t e = p.got_b.find("\x19\xba");
	EXPECT_TRUE(e != std::string::npos &&
		    p.got_b.find("NO CARRIER", e) != std::string::npos)
		<< "B: [" << p.got_b.substr(p.got_b.size() > 40 ?
					   p.got_b.size() - 40 : 0) << "]";
}

/* the last <EM><rate><tx><rx> in got (V.80), its two codes; false if
   none */
static bool v80_last_rate(const std::string &got, int &tx, int &rx)
{
	size_t at = got.rfind("\x19\xbe");
	if (at == std::string::npos || at + 3 >= got.size())
		return false;
	tx = (unsigned char)got[at + 2];
	rx = (unsigned char)got[at + 3];
	return true;
}

/* V.80 on V.34: the DTE's <EM><rtn> retrains (the other DTE hears
   <EM><rtn>, both <EM><rate> after it), frames go on; <EM><rate> with
   24000 bit/s each way, then <EM><rrn>: a renegotiation (<EM><rrn> at
   the other end) to rates no faster */
TEST(Core, V80Retrain)
{
	Pair p("+MS=34;+ES=6,,8");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	std::string flag = { 0x19, (char)0xb1 };
	p.a->dte_input(flag.data(), flag.size());
	p.b->dte_input(flag.data(), flag.size());
	p.run_until([](Pair &) { return false; }, 1000);
	p.got_a.clear();
	p.got_b.clear();
	std::string rtn = { 0x19, (char)0xbd };
	p.a->dte_input(rtn.data(), rtn.size());
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find("\x19\xbe") != std::string::npos &&
			q.got_b.find("\x19\xbe") != std::string::npos; },
		30000)) << "no <EM><rate> after the retrain";
	EXPECT_TRUE(p.got_b.find("\x19\xbd") != std::string::npos)
		<< "B: no <EM><rtn>";
	EXPECT_TRUE(p.got_a.find("\x19\xbd") == std::string::npos)
		<< "A: its own retrain indicated";
	EXPECT_TRUE(p.a->link_state() == LinkState::Online &&
		    p.a->carrier());
	/* frames still */
	p.got_b.clear();
	std::string f = "after the retrain" + flag;
	p.a->dte_input(f.data(), f.size());
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.got_b.find("after the retrain\x19\xb1") !=
			std::string::npos; }, 3000));
	/* at most 24000 bit/s each way, then a renegotiation */
	p.got_a.clear();
	p.got_b.clear();
	std::string rrn = { 0x19, (char)0xbe, 0x2a, 0x2a, 0x19, (char)0xbc };
	p.a->dte_input(rrn.data(), rrn.size());
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find("\x19\xbe") != std::string::npos; },
		20000)) << "no <EM><rate> after the renegotiation";
	EXPECT_TRUE(p.got_b.find("\x19\xbc") != std::string::npos)
		<< "B: no <EM><rrn>";
	int tx = 0, rx = 0;
	EXPECT_TRUE(v80_last_rate(p.got_a, tx, rx));
	EXPECT_TRUE(tx <= 0x2a && rx <= 0x2a && tx >= 0x20 && rx >= 0x20)
		<< "A's rates " << tx << " " << rx;
	EXPECT_TRUE(p.a->tx_rate() <= 24000 && p.a->rx_rate() <= 24000)
		<< p.a->tx_rate() << "/" << p.a->rx_rate();
}

/* ATO1 (V.250) back online with a retrain, ATO3 (Rockwell's) with a rate
   renegotiation, on a V.34 call with LAPM; the link goes on; ATI11
   counts them; ATO2 is an error */
TEST(Core, RetrainOnRequest)
{
	Pair p("+MS=34");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	for (const char *o : { "ATO1\r", "ATO3\r" }) {
		p.run_until([](Pair &) { return false; }, 1100);
		p.got_a.clear();
		p.a->dte_input("+++", 3);
		EXPECT_TRUE(p.run_until([](Pair &q) {
			return q.got_a.find("OK") != std::string::npos; },
			2000));
		if (!strcmp(o, "ATO1\r")) {
			EXPECT_EQ(cmd(p.a, p.la, "ATO2\r"), "ERROR\r\n");
		}
		EXPECT_TRUE(cmd(p.a, p.la, o).find("CONNECT") !=
			    std::string::npos);
		p.run_until([](Pair &) { return false; }, 15000);
		EXPECT_TRUE(p.a->link_state() == LinkState::Online &&
			    p.a->carrier()) << o;
	}
	/* data after both */
	p.got_b.clear();
	p.a->dte_input("still here", 10);
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.got_b.find("still here") != std::string::npos; },
		3000));
	p.run_until([](Pair &) { return false; }, 1100);
	p.got_a.clear();
	p.a->dte_input("+++", 3);
	p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos; }, 2000);
	std::string i11 = cmd(p.a, p.la, "ATI11\r");
	EXPECT_TRUE(std::regex_search(i11,
		std::regex("Retrains by Local Modem\\s+1\\b")) &&
		std::regex_search(i11,
		std::regex("Rate Renegotiation by Local Modem\\s+1\\b")))
		<< i11;
}

/* +MS's highest rate holds on V.34 as well (each MP asks for no more,
   both ways) */
TEST(Core, V34MaxRate)
{
	Pair p("+MS=34,1,2400,21600");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_TRUE(p.a->tx_rate() <= 21600 && p.a->rx_rate() <= 21600 &&
		    p.a->tx_rate() >= 2400)
		<< p.a->tx_rate() << "/" << p.a->rx_rate();
}

INSTANTIATE_TEST_SUITE_P(Pumps, CoreCall, testing::Values(
	Call{ "+MS=132", 14400, 20000 },
	Call{ "+MS=34", 28800, 40000 },
	Call{ "+MS=22", 1200, 4000 }),
	[](const testing::TestParamInfo<Call> &i) {
		return std::string("ms") + (i.param.ms + 4);
	});

/* class 2 and 2.0 fax (T.32): its settings, the commands, and a call
   between two modems in it (with +FBO's bit order and ECM) */
struct Fax2Case {
	const char *cls;        /* +FCLASS */
	int bo, ec;             /* +FBO, +FIS's ec */
	int t38;                /* 1: on T.38 from the start, 2: from the
				   DIS on */
	int v34;                /* +FIS's br 13: V.34 half-duplex */
};

/* octet c, its bits the other way round */
static unsigned char reverse_bits(unsigned char c)
{
	unsigned char r = 0;
	for (int j = 0; j < 8; j++)
		if (c & (1 << j))
			r |= 0x80 >> j;
	return r;
}

/* a page of rows rows, a black bar moving across it */
static dsp::T4Page fax2_page(int rows, int shift)
{
	dsp::T4Page pg;
	std::vector<unsigned char> row(T4_STRIDE(T4_WIDTH));
	pg.init(T4_WIDTH, 1);
	for (int y = 0; y < rows; y++) {
		std::fill(row.begin(), row.end(), 0);
		int x0 = (y * 7 + shift) % 1500;
		for (int x = x0; x < x0 + 120; x++)
			row[x / 8] |= 0x80 >> (x % 8);
		pg.add_row(row.data());
	}
	return pg;
}

/* the page as a class 2 DTE gives it: MH, the first bit of an octet its
   top one if msb, <DLE> shielded, <DLE><ETX> */
static std::string fax2_dte_data(const dsp::T4Page &pg, bool msb)
{
	dsp::T4Bits b;
	std::string d;
	pg.encode_mh(&b, 0, 0, 1);
	for (size_t k = 0; k < (b.n + 7) / 8; k++) {
		unsigned char c = msb ? b.buf[k] : reverse_bits(b.buf[k]);
		if (c == 0x10)
			d += '\x10';
		d += (char)c;
	}
	return d + "\x10\x03";
}

/* the page in a DTE's data from +FDR (after CONNECT, to <DLE><ETX>) */
static dsp::T4Page fax2_decode(const std::string &got, bool msb, bool fine)
{
	dsp::T4Page pg;
	std::string d;
	size_t k = got.find("CONNECT\r\n");
	pg.init(T4_WIDTH, fine);
	if (k == std::string::npos)
		return pg;
	for (k += 9; k + 1 < got.size(); k++) {
		if (got[k] == '\x10') {
			if (got[++k] == '\x03')
				break;
		}
		d += got[k];
	}
	pg.decode((const unsigned char *)d.data(), d.size(), 3, 0, msb, 0);
	return pg;
}

class CoreFax2 : public testing::TestWithParam<Fax2Case> {};

/* A sends B two pages: the DIS and the session told, CONNECT for each
   page both ways, B's pages the same as A's, +FPS and +FET, +FHS:00 at
   the end, and the call over by itself */
TEST_P(CoreFax2, TwoPages)
{
	const Fax2Case c = GetParam();
	const bool c2 = std::string(c.cls) == "2";
	auto nm = [c2](const char *n2, const char *n20) {
		return std::string(c2 ? n2 : n20);
	};
	Pair p("");
	/* steps until cond, at most ms */
	auto until = [&p](auto cond, int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	};
	auto has = [](const std::string &s, const std::string &x) {
		return s.find(x) != std::string::npos;
	};
	auto ok_after = [](const std::string &s, const std::string &x) {
		size_t k = s.find(x);
		return k != std::string::npos &&
			s.find("OK\r\n", k) != std::string::npos;
	};
	/* V.34: the br of the DIS and the session's (+FIS:, +FCS: and
	   class 2's names) a V.34 rate of 21600 bit/s at least (8), as 5
	   for the comparisons */
	auto br = [&c](std::string s) {
		for (const char *n : { "IS:1,", "CS:1," }) {
			size_t k = 0;
			while (c.v34 && (k = s.find(n, k)) != std::string::npos) {
				k += 5;
				size_t e = s.find(',', k);
				int v = atoi(s.substr(k, e - k).c_str());
				EXPECT_GE(v, 8) << s;
				s.replace(k, e - k, "5");
			}
		}
		return s;
	};

	for (int side = 0; side < 2; side++) {
		Modem *m = side ? p.b : p.a;
		std::string s = std::string("AT+FCLASS=") + c.cls + ";+" +
			nm("FLID", "FLI") + "=\"" + (side ? "Fax B 222" : "Fax A 111") +
			"\";+" + nm("FBOR", "FBO") + "=" + std::to_string(c.bo) +
			";+" + nm("FDIS", "FIS") + "=1," + (c.v34 ? "13" : "5") +
			",0,2,0," + std::to_string(c.ec) + ",0,0\r";
		m->dte_input(s.data(), s.size());
		m->dte_input("AT+FNR=1,1,1,0\r", 15);
	}
	p.step();
	EXPECT_EQ(p.got_a, "OK\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "OK\r\nOK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	p.a->dte_input("AT+FCLASS?;+FNR?\r", 17);
	p.step();
	EXPECT_EQ(p.got_a, std::string(c.cls) + "\r\n1,1,1,0\r\nOK\r\n");
	p.got_a.clear();

	p.b->dte_input("ATA\r", 4);
	p.a->dte_input("ATD\r", 4);
	p.step();
	if (c.t38 == 1) {
		EXPECT_EQ(p.a->t38_start(300), 0);
		EXPECT_EQ(p.b->t38_start(300), 0);
	}
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 30000))
		<< "A [" << p.got_a << "]";
	if (c.t38 == 2) {
		/* (B waits for the DCS, A for +FDT) */
		EXPECT_EQ(p.a->t38_start(300), 0);
		EXPECT_EQ(p.b->t38_start(300), 0);
	}
	EXPECT_EQ(br(p.got_a), "+" + nm("FCON", "FCO") + "\r\n+" +
		  nm("FCSI", "FCI") + ":\"Fax B 222\"\r\n+" + nm("FDIS", "FIS") +
		  ":1,5,0,2,0," + (c.ec ? "2" : "0") + ",0,0\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "");
	p.got_a.clear();
	/* the DCS goes with +FDT: then B's ATA has its OK */
	p.a->dte_input("AT+FDT\r", 7);
	ASSERT_TRUE(until([&] {
		return has(p.got_a, "CONNECT") && has(p.got_b, "OK\r\n");
	}, 20000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(br(p.got_b), "+" + nm("FCON", "FCO") + "\r\n+" +
		  nm("FTSI", "FTI") + ":\"Fax A 111\"\r\n+" + nm("FDCS", "FCS") +
		  ":1,5,0,2,0," + (c.ec ? "2" : "0") + ",0,0\r\nOK\r\n");
	p.got_b.clear();

	for (int page = 0; page < 2; page++) {
		dsp::T4Page pg = fax2_page(300 + 50 * page, page * 333);
		std::string d = fax2_dte_data(pg, c.bo & 1);
		size_t sent = 0;

		if (page)
			p.a->dte_input("AT+FDT\r", 7);
		ASSERT_TRUE(until([&] { return has(p.got_a, "CONNECT"); },
				  20000)) << page << ": A [" << p.got_a << "]";
		if (!page)
			EXPECT_TRUE(has(br(p.got_a), "+" + nm("FDCS", "FCS") +
					":1,5,0,2,0,")) << p.got_a;
		while (sent < d.size()) {
			int n = p.a->dte_space();
			if (n > (int)(d.size() - sent))
				n = d.size() - sent;
			sent += p.a->dte_input(d.data() + sent, n);
			p.step();
		}
		ASSERT_TRUE(until([&] { return ok_after(p.got_a, "CONNECT"); },
				  5000)) << page << ": A [" << p.got_a << "]";
		/* (class 2: an XON for the data) */
		EXPECT_EQ(has(p.got_a, "CONNECT\r\n\x11OK"), c2) << p.got_a;
		p.got_a.clear();
		std::string fet = std::string("AT+FET=") + (page ? "2" : "0") +
			"\r";
		p.a->dte_input(fet.data(), fet.size());

		p.b->dte_input("AT+FDR\r", 7);
		ASSERT_TRUE(until([&] { return has(p.got_b, "CONNECT"); },
				  60000)) << page << ": B [" << p.got_b << "]";
		p.b->dte_input("\x12", 1);
		ASSERT_TRUE(until([&] { return ok_after(p.got_b, "+FET:"); },
				  5000)) << page << ": B [" << p.got_b << "]";
		dsp::T4Page rx = fax2_decode(p.got_b, c.bo & 1, true);
		EXPECT_EQ(rx.rows, pg.rows) << page;
		EXPECT_TRUE(rx.bits == pg.bits) << page;
		EXPECT_TRUE(has(p.got_b, "\x10\x03+" + nm("FPTS", "FPS") +
				":1," + std::to_string(pg.rows) +
				",0,0,0\r\n+FET:" + (page ? "2" : "0") +
				"\r\nOK\r\n")) << page;
		p.got_b.clear();

		ASSERT_TRUE(until([&] { return ok_after(p.got_a, "+F"); },
				  30000)) << page << ": A [" << p.got_a << "]";
		EXPECT_EQ(p.got_a, "+" + nm("FPTS", "FPS") + ":1,0,0,0,0\r\n" +
			  (page ? "+" + nm("FHNG", "FHS") + ":" +
			   (c2 ? "0" : "00") + "\r\n" : "") + "OK\r\n");
		p.got_a.clear();
	}
	p.b->dte_input("AT+FDR\r", 7);
	ASSERT_TRUE(until([&] { return has(p.got_b, "OK\r\n"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+" + nm("FHNG", "FHS") + ":" + (c2 ? "0" : "00") +
		  "\r\nOK\r\n");
	EXPECT_TRUE(until([&] {
		return !p.a->started() && !p.b->started();
	}, 2000));
	/* (the pages went as T.38's packets, or as audio) */
	EXPECT_EQ(p.la.t38_sent > 100 && p.lb.t38_sent > 20, c.t38 != 0)
		<< p.la.t38_sent << " " << p.lb.t38_sent;
	p.a->dte_input("AT+FHS?\r", 8);
	p.step();
	EXPECT_EQ(p.got_a, std::string(c2 ? "0" : "00") + "\r\nOK\r\n");
}

INSTANTIATE_TEST_SUITE_P(
	Classes, CoreFax2,
	testing::Values(Fax2Case{ "2.0", 0, 0, 0, 0 }, Fax2Case{ "2", 0, 0, 0, 0 },
			Fax2Case{ "2.0", 1, 2, 0, 0 }, Fax2Case{ "2.0", 0, 0, 1, 0 },
			Fax2Case{ "2", 0, 2, 1, 0 }, Fax2Case{ "2.0", 0, 2, 2, 0 },
			Fax2Case{ "2.0", 0, 2, 0, 1 },
			Fax2Case{ "2", 1, 2, 0, 1 }),
	[](const testing::TestParamInfo<Fax2Case> &i) {
		return std::string(i.param.cls[1] ? "class20" : "class2") +
			"_bo" + std::to_string(i.param.bo) + "_ec" +
			std::to_string(i.param.ec) +
			(i.param.t38 == 1 ? "_t38" :
			 i.param.t38 == 2 ? "_t38_after_dis" : "") +
			(i.param.v34 ? "_v34" : "");
	});

/* class 2.0: +FKS after the DIS ends the call with a DCN, +FHS:02 and
   OK; the answerer, still in its ATA, has the DCN as a hangup in phase
   B: +FHS:70 and NO CARRIER */
TEST(Core, Fax2Abort)
{
	Pair p("");
	auto until = [&p](auto cond, int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	};
	p.a->dte_input("AT+FCLASS=2.0\r", 14);
	p.b->dte_input("AT+FCLASS=2.0\r", 14);
	p.step();
	p.got_a.clear();
	p.got_b.clear();
	p.b->dte_input("ATA\r", 4);
	p.a->dte_input("ATD\r", 4);
	ASSERT_TRUE(until([&] {
		return p.got_a.find("OK\r\n") != std::string::npos;
	}, 30000)) << p.got_a;
	p.got_a.clear();
	p.a->dte_input("AT+FKS\r", 7);
	ASSERT_TRUE(until([&] {
		return p.got_a.find("OK\r\n") != std::string::npos &&
			p.got_b.find("NO CARRIER") != std::string::npos;
	}, 20000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "+FHS:02\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "+FHS:70\r\nNO CARRIER\r\n");
	EXPECT_TRUE(until([&] {
		return !p.a->started() && !p.b->started();
	}, 2000));
}

/* class 2.0 polling: B has a document (+FLP=1), A polls it (+FSP=1):
   +FPO after the DIS, +FDR sends A's CIG and DTC, B's ATA has +FPI and
   +FTC and B sends the page with +FDT; A receives it as with +FDR, and
   both have +FHS:00.  Without +FSP, +FDR is ERROR */
TEST(Core, Fax2Poll)
{
	Pair p("");
	auto until = [&p](auto cond, int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	};
	auto has = [](const std::string &s, const std::string &x) {
		return s.find(x) != std::string::npos;
	};
	auto ok_after = [](const std::string &s, const std::string &x) {
		size_t k = s.find(x);
		return k != std::string::npos &&
			s.find("OK\r\n", k) != std::string::npos;
	};
	/* a command line to modem m */
	auto type = [](Modem *m, const std::string &l) {
		m->dte_input(l.data(), l.size());
	};
	dsp::T4Page pg = fax2_page(240, 77);
	std::string d = fax2_dte_data(pg, false);
	size_t sent = 0;

	type(p.a, "AT+FCLASS=2.0;+FLI=\"Caller A\"\r");
	type(p.a, "AT+FPI=\"Poller A\"\r");
	type(p.b, "AT+FCLASS=2.0;+FLI=\"Polled B\";+FLP=1\r");
	p.step();
	EXPECT_EQ(p.got_a, "OK\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "OK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	type(p.b, "ATA\r");
	type(p.a, "ATD\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 30000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "+FCO\r\n+FCI:\"Polled B\"\r\n"
		  "+FIS:1,5,0,2,0,0,0,0\r\n+FPO\r\nOK\r\n");
	p.got_a.clear();
	type(p.a, "AT+FDR\r");
	p.step();
	EXPECT_EQ(p.got_a, "ERROR\r\n");
	p.got_a.clear();
	type(p.a, "AT+FSP=1;+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "OK\r\n"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+FCO\r\n+FPI:\"Poller A\"\r\n"
		  "+FTC:1,5,0,2,0,0,0,0\r\nOK\r\n");
	p.got_b.clear();

	type(p.b, "AT+FDT\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "CONNECT"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_TRUE(has(p.got_b, "+FCS:1,5,0,2,0,0,0,0\r\nCONNECT")) <<
		p.got_b;
	while (sent < d.size()) {
		int n = p.b->dte_space();
		if (n > (int)(d.size() - sent))
			n = d.size() - sent;
		sent += p.b->dte_input(d.data() + sent, n);
		p.step();
	}
	ASSERT_TRUE(until([&] { return ok_after(p.got_b, "CONNECT"); },
			  5000)) << "B [" << p.got_b << "]";
	p.got_b.clear();
	type(p.b, "AT+FET=2\r");

	ASSERT_TRUE(until([&] { return has(p.got_a, "CONNECT"); }, 60000))
		<< "A [" << p.got_a << "]";
	EXPECT_TRUE(has(p.got_a, "+FTI:\"Polled B\"\r\n+FCS:1,5,0,2,0,0,0,0"
			"\r\nCONNECT")) << p.got_a;
	type(p.a, "\x12");
	ASSERT_TRUE(until([&] { return ok_after(p.got_a, "+FET:"); }, 5000))
		<< "A [" << p.got_a << "]";
	dsp::T4Page rx = fax2_decode(p.got_a, false, true);
	EXPECT_EQ(rx.rows, pg.rows);
	EXPECT_TRUE(rx.bits == pg.bits);
	EXPECT_TRUE(has(p.got_a, "+FET:2\r\nOK\r\n")) << p.got_a;
	p.got_a.clear();
	ASSERT_TRUE(until([&] { return ok_after(p.got_b, "+FHS"); }, 30000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+FPS:1,0,0,0,0\r\n+FHS:00\r\nOK\r\n");
	type(p.a, "AT+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 20000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "+FHS:00\r\nOK\r\n");
}

/* class 2.0 polling on V.34 half-duplex: +FSP=1 before the call, so
   that V.8 says the caller polls; B, polled, is the primary channel's
   source and sends its page at a V.34 rate */
TEST(Core, Fax2PollV34)
{
	Pair p("");
	auto until = [&p](auto cond, int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	};
	auto has = [](const std::string &s, const std::string &x) {
		return s.find(x) != std::string::npos;
	};
	auto ok_after = [](const std::string &s, const std::string &x) {
		size_t k = s.find(x);
		return k != std::string::npos &&
			s.find("OK\r\n", k) != std::string::npos;
	};
	/* a command line to modem m */
	auto type = [](Modem *m, const std::string &l) {
		m->dte_input(l.data(), l.size());
	};
	/* the br after x (a V.34 rate's: 8, 21600 bit/s, at least) */
	auto br_after = [](const std::string &s, const std::string &x) {
		size_t k = s.find(x);
		return k == std::string::npos ? -1 :
			atoi(s.c_str() + k + x.size());
	};
	dsp::T4Page pg = fax2_page(240, 77);
	std::string d = fax2_dte_data(pg, false);
	size_t sent = 0;

	type(p.a, "AT+FCLASS=2.0;+FLI=\"Caller A\";+FIS=1,13,0,2,0,2,0,0\r");
	type(p.a, "AT+FPI=\"Poller A\";+FSP=1\r");
	type(p.b, "AT+FCLASS=2.0;+FLI=\"Polled B\";+FLP=1;"
		  "+FIS=1,13,0,2,0,2,0,0\r");
	p.step();
	EXPECT_EQ(p.got_a, "OK\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "OK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	type(p.b, "ATA\r");
	type(p.a, "ATD\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 30000))
		<< "A [" << p.got_a << "]";
	EXPECT_GE(br_after(p.got_a, "+FIS:1,"), 8) << p.got_a;
	EXPECT_TRUE(has(p.got_a, "+FPO\r\nOK\r\n")) << p.got_a;
	p.got_a.clear();
	type(p.a, "AT+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "OK\r\n"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_TRUE(has(p.got_b, "+FPI:\"Poller A\"\r\n+FTC:1,")) <<
		p.got_b;
	p.got_b.clear();

	type(p.b, "AT+FDT\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "CONNECT"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_GE(br_after(p.got_b, "+FCS:1,"), 8) << p.got_b;
	while (sent < d.size()) {
		int n = p.b->dte_space();
		if (n > (int)(d.size() - sent))
			n = d.size() - sent;
		sent += p.b->dte_input(d.data() + sent, n);
		p.step();
	}
	ASSERT_TRUE(until([&] { return ok_after(p.got_b, "CONNECT"); },
			  5000)) << "B [" << p.got_b << "]";
	p.got_b.clear();
	type(p.b, "AT+FET=2\r");

	ASSERT_TRUE(until([&] { return has(p.got_a, "CONNECT"); }, 60000))
		<< "A [" << p.got_a << "]";
	type(p.a, "\x12");
	ASSERT_TRUE(until([&] { return ok_after(p.got_a, "+FET:"); }, 5000))
		<< "A [" << p.got_a << "]";
	dsp::T4Page rx = fax2_decode(p.got_a, false, true);
	EXPECT_EQ(rx.rows, pg.rows);
	EXPECT_TRUE(rx.bits == pg.bits);
	p.got_a.clear();
	ASSERT_TRUE(until([&] { return ok_after(p.got_b, "+FHS"); }, 30000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+FPS:1,0,0,0,0\r\n+FHS:00\r\nOK\r\n");
	type(p.a, "AT+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 20000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "+FHS:00\r\nOK\r\n");
}

/* a text telephone call: each side's commands, the rate A and B
   CONNECT at (B's 0: A's), a line A types, what B's DTE gets of it
   (the 5-bit code has capitals alone), and the +MV18R report either
   side gives (or none) */
struct TextCall {
	const char *a, *b;
	unsigned rate, rate_b;
	const char *typed, *got;
	const char *report;
};

class CoreText : public testing::TestWithParam<TextCall> {};

/* online on both sides, then A's line to B and B's to A */
TEST_P(CoreText, LineEachWay)
{
	const TextCall &c = GetParam();
	Pair p("");
	for (int k = 0; k < 2; k++) {
		Modem *m = k ? p.b : p.a;
		std::string s = std::string("AT") + (k ? c.b : c.a) + "\r";
		m->dte_input(s.data(), s.size());
	}
	p.step();
	ASSERT_TRUE(p.got_a.find("OK") != std::string::npos) << p.got_a;
	ASSERT_TRUE(p.got_b.find("OK") != std::string::npos) << p.got_b;
	p.got_a.clear();
	p.got_b.clear();

	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	/* the caller's line 2.5 s after it is online, once the answerer
	   (after its answer delay) listens: an automoding answerer finds
	   the caller by it */
	bool typed = false;
	int a_online = -1;
	auto online = [](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online;
	};
	for (int t = 0; t < 40000 && !online(p); t += 20) {
		if (a_online < 0 && p.a->link_state() == LinkState::Online)
			a_online = t;
		if (!typed && a_online >= 0 && t - a_online >= 2500) {
			p.a->dte_input(c.typed, strlen(c.typed));
			typed = true;
		}
		p.step();
	}
	ASSERT_TRUE(online(p)) << "A [" << p.got_a << "] B [" << p.got_b
			       << "]";
	std::string connect = "CONNECT " + std::to_string(c.rate) + "\r";
	std::string connect_b = "CONNECT " +
		std::to_string(c.rate_b ? c.rate_b : c.rate) + "\r";
	EXPECT_TRUE(p.got_a.find(connect) != std::string::npos) << p.got_a;
	EXPECT_TRUE(p.got_b.find(connect_b) != std::string::npos) << p.got_b;
	std::string both = p.got_a + p.got_b;
	if (c.report)
		EXPECT_TRUE(both.find(c.report) != std::string::npos) << both;
	else
		EXPECT_TRUE(both.find("+MV18R") == std::string::npos);
	if (!typed)
		p.a->dte_input(c.typed, strlen(c.typed));
	std::string want = c.got;
	p.run_until([](Pair &) { return false; }, 6000);
	EXPECT_TRUE(p.got_b.find(want) != std::string::npos)
		<< "B [" << p.got_b << "]";

	/* and back */
	p.got_a.clear();
	p.b->dte_input(c.typed, strlen(c.typed));
	p.run_until([](Pair &) { return false; }, 6000);
	EXPECT_TRUE(p.got_a.find(want) != std::string::npos)
		<< "A [" << p.got_a << "]";
}

/* the US TTY's 5-bit mode; telex (50 bit/s, ITA2, RTTY's 170 Hz
   shift); EDT; V.18 automoding answering a TTY, an EDT terminal, and
   another automoding modem (which hears the caller's 5-bit probe and
   answers in 5-bit); V.18 automoding calling a V.21, a Bell 103 and a
   V.23 modem (their answer signals tell) */
static const TextCall text_calls[] = {
	{ "+MS=BAUDOT", "+MS=BAUDOT", 45, 0, "Hello 123-?\r\n",
	  "HELLO 123-?\r\n", nullptr },
	{ "+MBAUDOT=50,2125,2295,1;+MS=BAUDOT",
	  "+MBAUDOT=50,2125,2295,1;+MS=BAUDOT", 50, 0, "RYRY 1/2=3+4\r\n",
	  "RYRY 1/2=3+4\r\n", nullptr },
	{ "+MS=EDT", "+MS=EDT", 110, 0, "Hello, edt!\r\n", "Hello, edt!\r\n",
	  nullptr },
	{ "+MS=BAUDOT", "+MV18S=1;+MV18R=1", 45, 0, "GA\r\n", "GA\r\n",
	  "+MV18R: 2" },
	{ "+MS=EDT", "+MV18S=1;+MV18R=1", 110, 0, "Hi edt\r\n",
	  "Hi edt\r\n", "+MV18R: 4" },
	{ "+MS=V18", "+MS=V18;+MV18R=1", 45, 0, "HELLO\r\n", "HELLO\r\n",
	  "+MV18R: 2" },
	{ "+MS=V18;+MV18R=1", "+MS=21,0", 300, 0, "Hello\r\n", "Hello\r\n",
	  "+MV18R: 5" },
	{ "+MS=V18;+MV18R=1", "+MS=103,0", 300, 0, "Hello\r\n",
	  "Hello\r\n", "+MV18R: 7" },
	{ "+MS=V18;+MV18R=1", "+MS=V23C,0", 1200, 75, "Hello\r\n",
	  "Hello\r\n", "+MV18R: 6" },
};

/* each case's name */
static std::string text_name(const testing::TestParamInfo<TextCall> &i)
{
	static const char *names[] = {
		"Tty", "Telex", "Edt", "V18Tty", "V18Edt", "V18V18",
		"V18CallsV21", "V18CallsBell103", "V18CallsV23",
	};
	return names[i.index];
}

INSTANTIATE_TEST_SUITE_P(Modes, CoreText, testing::ValuesIn(text_calls),
			 text_name);

/* Contact ID: B the receiver (answering), A the panel (calling) */
class CoreContactId : public testing::Test {
protected:
	Pair p{""};

	/* both in +MS=CONTACTID, B answers, A dials: both online */
	void call()
	{
		type(p.a, "AT+MS=CONTACTID\r");
		type(p.b, "AT+MS=CONTACTID\r");
		p.step();
		ASSERT_TRUE(has(p.got_a, "OK")) << p.got_a;
		ASSERT_TRUE(has(p.got_b, "OK")) << p.got_b;
		p.got_a.clear();
		p.got_b.clear();
		EXPECT_EQ(p.b->answer(), 0);
		EXPECT_EQ(p.a->dial(""), 0);
		ASSERT_TRUE(until([&] {
			return p.a->link_state() == LinkState::Online &&
				p.b->link_state() == LinkState::Online;
		}, 10000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
		EXPECT_TRUE(has(p.got_a, "CONNECT 40\r")) << p.got_a;
		EXPECT_TRUE(has(p.got_b, "CONNECT 40\r")) << p.got_b;
		p.got_a.clear();
		p.got_b.clear();
	}
	/* the run until the panel's DTE has line */
	bool until_a(const char *line, int ms)
	{
		return until([&] { return has(p.got_a, line); }, ms);
	}
	/* steps until cond, at most ms */
	template <class F> bool until(F cond, int ms)
	{
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	}
	/* s has x */
	static bool has(const std::string &s, const std::string &x)
	{
		return s.find(x) != std::string::npos;
	}
	/* s to m's DTE */
	static void type(Modem *m, const char *s)
	{
		m->dte_input(s, strlen(s));
	}
};

/* two messages, one with its checksum from the panel, each to the
   receiver's DTE and kissed off; a line that is no message, ERROR */
TEST_F(CoreContactId, MessagesKissedOff)
{
	call();
	type(p.a, "1234 18 1131 01 015\r9876181602000034\rXYZ\r");
	ASSERT_TRUE(until([&] {
		return p.got_a.find("ACK\r\nACK\r\n") != std::string::npos;
	}, 10000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "ERROR\r\nACK\r\nACK\r\n");
	EXPECT_EQ(p.got_b, "1234181131010158\r\n9876181602000034\r\n");
}

/* a message with a bad checksum: never kissed off, NAK after four
   times; the next one still goes */
TEST_F(CoreContactId, BadChecksumNak)
{
	call();
	type(p.a, "1234181131010151\r1234181131010158\r");
	ASSERT_TRUE(until_a("ACK\r\n", 30000))
		<< "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "NAK\r\nACK\r\n");
	EXPECT_EQ(p.got_b, "1234181131010158\r\n");
}

/* fixed-line SMS: B the answerer (it receives), A the caller (it
   sends) */
class CoreSms : public testing::Test {
protected:
	Pair p{""};

	/* both in +MS=SMS, B answers, A dials: both online */
	void call()
	{
		type(p.a, "AT+MS=SMS\r");
		type(p.b, "AT+MS=SMS\r");
		p.step();
		ASSERT_TRUE(has(p.got_a, "OK")) << p.got_a;
		ASSERT_TRUE(has(p.got_b, "OK")) << p.got_b;
		p.got_a.clear();
		p.got_b.clear();
		EXPECT_EQ(p.b->answer(), 0);
		EXPECT_EQ(p.a->dial(""), 0);
		ASSERT_TRUE(until([&] {
			return p.a->link_state() == LinkState::Online &&
				p.b->link_state() == LinkState::Online;
		}, 10000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
		EXPECT_TRUE(has(p.got_a, "CONNECT 1200\r")) << p.got_a;
		EXPECT_TRUE(has(p.got_b, "CONNECT 1200\r")) << p.got_b;
		p.got_a.clear();
		p.got_b.clear();
	}
	/* steps until both have hung up, at most ms */
	bool until_idle(int ms)
	{
		return until([&] {
			return p.a->link_state() == LinkState::Idle &&
				p.b->link_state() == LinkState::Idle;
		}, ms);
	}
	/* steps until cond, at most ms */
	template <class F> bool until(F cond, int ms)
	{
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	}
	/* s has x */
	static bool has(const std::string &s, const std::string &x)
	{
		return s.find(x) != std::string::npos;
	}
	/* s to m's DTE */
	static void type(Modem *m, const char *s)
	{
		m->dte_input(s, strlen(s));
	}
};

/* three submissions (7-bit with an escape, UCS2, a TPDU in hex) and a
   line that is none: each to the answerer's DTE and acknowledged, then
   SMS_REL and both hang up */
TEST_F(CoreSms, SubmitsThenReleases)
{
	call();
	type(p.a, "+4412345,Hello [world]\r"
		  "0800,\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\r"
		  "no comma\r"
		  ":010005912143F500000AE8329BFD4697D9EC37\r");
	ASSERT_TRUE(until_idle(30000))
		<< "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "ERROR\r\nOK\r\nOK\r\nOK\r\nNO CARRIER\r\n");
	EXPECT_EQ(p.got_b,
		  "+SUBMIT: \"+4412345\"\r\nHello [world]\r\n"
		  "+SUBMIT: \"0800\"\r\n"
		  "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\r\n"
		  "+SUBMIT: \"+12345\"\r\nhellohello\r\n"
		  "NO CARRIER\r\n");
}

/* the service centre's side: a delivery from a name, stamped now */
TEST_F(CoreSms, Delivers)
{
	call();
	type(p.a, "<Centre,Your code is 1234\r");
	ASSERT_TRUE(until_idle(30000))
		<< "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "OK\r\nNO CARRIER\r\n");
	EXPECT_EQ(p.got_b.rfind("+CMT: \"Centre\",,\"", 0), 0u) << p.got_b;
	EXPECT_TRUE(has(p.got_b, "+00\"\r\nYour code is 1234\r\n"))
		<< p.got_b;
}

/* nothing to send: SMS_REL after 3 s, and both hang up */
TEST_F(CoreSms, NothingToSend)
{
	call();
	ASSERT_TRUE(until_idle(10000))
		<< "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "NO CARRIER\r\n");
	EXPECT_EQ(p.got_b, "NO CARRIER\r\n");
}

/* V.150.1's modem relay: B answers a relayed call (DP_RELAY at once), A
   dials one and hears the answer tone the far end's gateway plays; both
   CONNECT once the relay is up, at its rates */
class CoreRelay : public testing::Test {
protected:
	Pair p{""};

	/* both on relayed lines, B answers, A dials and hears 2100 Hz:
	   both online once the relay is up */
	void call()
	{
		for (Line *l : { &p.la, &p.lb }) {
			l->relay = true;
			l->relay_tx = l->relay_rx = 33600;
		}
		p.la.peer_rx = &p.lb.rx;
		p.lb.peer_rx = &p.la.rx;
		EXPECT_EQ(p.b->answer(), 0);
		EXPECT_EQ(p.a->dial(""), 0);
		/* the answer tone, then silence: the relay comes up */
		for (int t = 0; t < 3000; t += 20) {
			step_tone(t < 2000);
			if (t == 2500)
				p.la.relay_st = p.lb.relay_st = 1;
		}
		ASSERT_TRUE(until([&] {
			return p.a->link_state() == LinkState::Online &&
				p.b->link_state() == LinkState::Online;
		}, 5000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
		EXPECT_TRUE(has(p.got_a, "CONNECT 33600\r")) << p.got_a;
		EXPECT_TRUE(has(p.got_b, "CONNECT 33600\r")) << p.got_b;
		p.got_a.clear();
		p.got_b.clear();
	}
	/* a step, A hearing 2100 Hz if tone (the line's samples are
	   otherwise not the call's) */
	void step_tone(bool tone)
	{
		if (tone)
			for (int i = 0; i < Pair::BLOCK; i++)
				p.ia[i] = static_cast<int16_t>(8000 * sin(
					2 * M_PI * 2100 * (phase_++) / 8000.0));
		p.step();
	}
	/* steps until cond, at most ms */
	template <class F> bool until(F cond, int ms)
	{
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return true;
			p.step();
		}
		return cond();
	}
	/* s has x */
	static bool has(const std::string &s, const std::string &x)
	{
		return s.find(x) != std::string::npos;
	}
	long phase_ = 0;
};

/* the data each way through the relay, every octet value, more than
   its window holds at once */
TEST_F(CoreRelay, DataBothWays)
{
	call();
	std::string da = data('a', 2000), db = data('b', 1500);
	p.a->dte_input(da.data(), da.size());
	p.b->dte_input(db.data(), db.size());
	ASSERT_TRUE(until([&] {
		return p.got_b.size() >= da.size() &&
			p.got_a.size() >= db.size();
	}, 20000)) << p.got_a.size() << " " << p.got_b.size();
	EXPECT_EQ(p.got_b, da);
	EXPECT_EQ(p.got_a, db);
}

/* the relay over: NO CARRIER at both */
TEST_F(CoreRelay, RelayOver)
{
	call();
	p.la.relay_st = p.lb.relay_st = -1;
	ASSERT_TRUE(until([&] {
		return p.a->link_state() == LinkState::Idle &&
			p.b->link_state() == LinkState::Idle;
	}, 5000));
	EXPECT_TRUE(has(p.got_a, "NO CARRIER")) << p.got_a;
	EXPECT_TRUE(has(p.got_b, "NO CARRIER")) << p.got_b;
}
