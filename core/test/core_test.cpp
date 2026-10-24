/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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

#include <dcore.h>
#include <log.h>
#include <modem_param.h>

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
	int start(Modem &) override {
		starts++;
		return fail_start;
	}
	/* counted */
	int stop(Modem &) override {
		stops++;
		return 0;
	}
	/* fail_hook fails it, else the state is kept */
	int set_hook(Modem &, int state) override {
		if (fail_hook)
			return -1;
		last_hook = state;
		return 0;
	}
	/* the volume, kept */
	void speaker(Modem &, int v) override { volume = v; }
	/* the line's delay, and the changes asked for, added up */
	int io_delay(Modem &) override { return delay; }
	/* (see io_delay()) */
	void update_delay(Modem &, int samples) override {
		delay_change += samples;
	}
	/* the DTE's output, kept */
	int dte_write(Modem &, const char *buf, int n) override {
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
	int pulse_hook(Modem &, bool broken) override {
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
	int voice_route(Modem &, unsigned device) override {
		routes.push_back(device);
		return device == VOICE_DEVICE_LINE || handset ? 0 : -1;
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

INSTANTIATE_TEST_SUITE_P(Pumps, CoreCall, testing::Values(
	Call{ "+MS=132", 14400, 20000 },
	Call{ "+MS=34", 28800, 40000 },
	Call{ "+MS=22", 1200, 4000 }),
	[](const testing::TestParamInfo<Call> &i) {
		return std::string("ms") + (i.param.ms + 4);
	});
