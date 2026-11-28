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

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <regex>

#include <dcore.h>
#include <v54.h>
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
	/* +GSN's and +GOI's (serial_number(), object_id()); NULL: none */
	const char *serial = nullptr, *oid = nullptr;
	/* (see serial) */
	const char *serial_number() override { return serial; }
	/* (see oid) */
	const char *object_id() override { return oid; }
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
	/* DSVD's voice path (dsvd): a tone of tone Hz for the microphone,
	   the earpiece's samples kept, the voice channel's state */
	bool dsvd = false, dsvd_on = false;
	int dsvd_opened = 0;
	double tone = 440;
	long mic_sent = 0;
	std::vector<s16> ear;
	/* (see dsvd) */
	bool dsvd_voice(Modem &) override { return dsvd; }
	/* (see dsvd) */
	void dsvd_voice_state(Modem &, bool on) override {
		dsvd_on = on;
		dsvd_opened += on;
	}
	/* (see dsvd) */
	s16 dsvd_mic(Modem &, s16 *pcm, s16 n) override {
		for (s16 i = 0; i < n; i++, mic_sent++)
			pcm[i] = static_cast<s16>(8000 * sin(2 * M_PI * tone *
							     mic_sent / 8000));
		return n;
	}
	/* (see dsvd) */
	void dsvd_ear(Modem &, const s16 *pcm, s16 n) override {
		ear.insert(ear.end(), pcm, pcm + n);
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

/* a modem on l at 8000 Hz, in country (NULL: USA), with the program's
   speaker spk (NULL: none) */
static Modem *make(Line &l, const char *name, const char *country = nullptr,
		   dcore::SpeakerPath *spk = nullptr)
{
	dcore::Options o;
	o.name = name;
	o.rate = 8000;
	o.country = country;
	o.speaker = spk;
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

/* s in EBCDIC, in IBM's code page cp */
static std::string ebc(const std::string &s, u16 cp = 37)
{
	dcore::ebcdic::CodePage page(cp);
	std::string e = s;
	page.out(&e[0], static_cast<s32>(e.size()));
	return e;
}

/* V.52's 511-bit pattern (O.153 2.1): it begins with the first ONE
   of its 9 ONEs, the 10th bit is a ZERO, it repeats after 511 bits,
   and its longest run of ZEROs is 8 */
TEST(V52, PatternStart)
{
	dcore::v54::Prbs9 g;
	std::vector<int> b;
	for (int i = 0; i < 1022; i++)
		b.push_back(g.bit());
	for (int i = 0; i < 9; i++)
		EXPECT_EQ(b[i], 1) << i;
	EXPECT_EQ(b[9], 0);
	int zeros = 0, run = 0;
	for (int i = 0; i < 511; i++) {
		EXPECT_EQ(b[i], b[i + 511]) << i;
		run = b[i] ? 0 : run + 1;
		zeros = std::max(zeros, run);
	}
	EXPECT_EQ(zeros, 8);
}

/* the checker synchronises again when the error ratio is 0.20 or more
   in its interval (O.153 2.6 a), and not below that: one bit in 6
   wrong (0.17) keeps it in step, one in 4 (0.25) does not */
TEST(V52, CheckerLossOfSync)
{
	for (int every : { 6, 4 }) {
		dcore::v54::Checker c;
		c.set_window(2400);
		dcore::v54::Prbs9 g;
		bool lost = false;
		for (int i = 0; i < 20000; i++) {
			int b = g.bit();
			bool was = c.locked();
			if (was && i % every == 0)
				b ^= 1;
			c.bit(static_cast<u8>(b));
			if (was && !c.locked())
				lost = true;
		}
		EXPECT_EQ(lost, every == 4) << every;
	}
	/* a 10 % burst for less than the interval: no loss either */
	dcore::v54::Checker c;
	c.set_window(9600);
	dcore::v54::Prbs9 g;
	bool lost = false;
	for (int i = 0; i < 40000; i++) {
		int b = g.bit();
		bool was = c.locked();
		if (was && i > 10000 && i < 12000 && i % 3 == 0)
			b ^= 1;
		c.bit(static_cast<u8>(b));
		lost |= was && !c.locked();
	}
	EXPECT_FALSE(lost);
}

/* each code page one octet for one octet both ways, "AT", "at" and
   "A/" the same octets in each, and no ASCII "AT" (any parity) seen as
   EBCDIC */
TEST(Core, EbcdicCodePages)
{
	for (const dcore::ebcdic::Table &t : dcore::ebcdic::tables) {
		dcore::ebcdic::CodePage p(t.id);
		ASSERT_EQ(p.id(), t.id);
		for (int i = 0; i < 256; i++)
			EXPECT_EQ((u8)p.in(p.out((char)i)), i) << t.id;
		EXPECT_EQ(dcore::ebcdic::detect(p.out('A'), p.out('T')), 1);
		EXPECT_EQ(dcore::ebcdic::detect(p.out('a'), p.out('t')), 1);
		EXPECT_EQ(dcore::ebcdic::detect(p.out('A'), p.out('/')), 1);
		EXPECT_EQ(p.out('\r'), '\r');
	}
	for (int a : { 0x41, 0xC1, 0x61, 0xE1 })
		for (int t : { 0x54, 0xD4, 0x74, 0xF4, 0x2F, 0xAF })
			EXPECT_EQ(dcore::ebcdic::detect((char)a, (char)t), 0);
	EXPECT_EQ(dcore::ebcdic::detect('A', 'X'), -1);
	/* 1047 has the C brackets where 037 has Ý and ¨ */
	EXPECT_EQ((u8)dcore::ebcdic::CodePage(1047).out('['), 0xAD);
	EXPECT_EQ((u8)dcore::ebcdic::CodePage(37).out('['), 0xBA);
	EXPECT_EQ(dcore::ebcdic::CodePage(999).id(), 37);
}

/* +DEBC=1: each command line's "AT" tells the character set; the echo,
   the information and the result codes come in the same set.  0: ASCII
   only; 2: EBCDIC only; <cp> from the list only */
TEST(Core, EbcdicCommands)
{
	Line l;
	Modem *m = make(l, "ebcdic");
	EXPECT_EQ(cmd(m, l, ebc("ATI7\r").c_str()),
		  ebc("ATI7\r\r\nCountry: USA\r\n\r\nOK\r\n"));
	EXPECT_EQ(cmd(m, l, "AT\r"), "AT\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, ebc("at+debc?\r").c_str()),
		  ebc("at+debc?\r\r\n+DEBC: 1,0,37\r\n\r\nOK\r\n"));
	/* A/ in EBCDIC: the last line again */
	EXPECT_EQ(cmd(m, l, ebc("A/").c_str()),
		  ebc("At+debc?\r\r\n+DEBC: 1,0,37\r\n\r\nOK\r\n"));
	/* a backspace (S5, EBCDIC 0x16) and an unknown code page */
	EXPECT_EQ(cmd(m, l, ebc("ATX\b\r").c_str()),
		  ebc("ATX\b \b\r\r\nOK\r\n"));
	EXPECT_EQ(cmd(m, l, ebc("AT+DEBC=1,0,999\r").c_str()),
		  ebc("AT+DEBC=1,0,999\r\r\nERROR\r\n"));
	/* 0: EBCDIC is not a command line */
	EXPECT_EQ(cmd(m, l, "AT+DEBC=0\r"), "AT+DEBC=0\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, ebc("AT\r").c_str()).find(ebc("OK")),
		  std::string::npos);
	/* 2: EBCDIC only, the line that sets it answered in ASCII */
	EXPECT_EQ(cmd(m, l, "AT+DEBC=2,0,1047\r"),
		  "AT+DEBC=2,0,1047\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT\r").find("OK"), std::string::npos);
	EXPECT_EQ(cmd(m, l, ebc("AT+DEBC?\r", 1047).c_str()),
		  ebc("AT+DEBC?\r\r\n+DEBC: 2,0,1047\r\n\r\nOK\r\n", 1047));
	/* ATZ: +DEBC=1 again */
	EXPECT_EQ(cmd(m, l, ebc("ATZ\r", 1047).c_str()),
		  ebc("ATZ\r\r\nOK\r\n", 1047));
	EXPECT_EQ(cmd(m, l, "AT\r"), "AT\r\r\nOK\r\n");
	/* &V shows it */
	EXPECT_NE(cmd(m, l, "AT&V\r").find(" +DEBC=1,0,37\r\n"),
		  std::string::npos);
	delete m;
}

/* +GSN: the Host's serial number, else Options' serial, else the
   device name, else the name; +GOI: the Host's identifier, else no
   text; "=?" on them gives no text */
TEST(Core, SerialAndObjectId)
{
	Line l;
	dcore::Options o;
	o.name = "modem7";
	o.rate = 8000;
	std::unique_ptr<Modem> m(new Modem(l, o));
	cmd(m.get(), l, "ATE0\r");
	EXPECT_EQ(cmd(m.get(), l, "AT+GSN\r"), "\r\nmodem7\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m.get(), l, "AT+GOI\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m.get(), l, "AT+GSN=?\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m.get(), l, "AT+GOI=?\r"), "\r\nOK\r\n");

	o.dev_name = "ttyDM7";
	m.reset(new Modem(l, o));
	cmd(m.get(), l, "ATE0\r");
	EXPECT_EQ(cmd(m.get(), l, "AT+GSN\r"), "\r\nttyDM7\r\n\r\nOK\r\n");

	o.serial = "DM-0001";
	m.reset(new Modem(l, o));
	cmd(m.get(), l, "ATE0\r");
	EXPECT_EQ(cmd(m.get(), l, "AT+GSN?\r"), "\r\nDM-0001\r\n\r\nOK\r\n");

	l.serial = "USB1234";
	l.oid = "1.3.6.1.4.1.99999";
	EXPECT_EQ(cmd(m.get(), l, "AT+GSN;+GOI\r"),
		  "\r\nUSB1234\r\n1.3.6.1.4.1.99999\r\n\r\nOK\r\n");
}

/* the country, ATI7, S-registers out of range, an idle modem */
TEST(Core, Options)
{
	Line l;
	Modem *m = make(l, "uk", "UK");
	EXPECT_TRUE(m->ok());
	EXPECT_EQ(cmd(m, l, "ATI7\r"), "ATI7\r\r\nCountry: UK\r\n\r\nOK\r\n");
	delete m;
	m = make(l, "xx", "NOWHERE");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"), "ATE0I7\r\r\nCountry: USA\r\n\r\nOK\r\n");
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
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"), "ATE0I7\r\r\nCountry: UK\r\n\r\nOK\r\n");
	delete m;
	m = make(l, "fj", "Fiji");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"),
		  "ATE0I7\r\r\nCountry: FIJI (USA line)\r\n\r\nOK\r\n");
	delete m;
	m = make(l, "de", "GERMANY");
	EXPECT_EQ(cmd(m, l, "ATE0I7\r"), "ATE0I7\r\r\nCountry: GERMANY\r\n\r\nOK\r\n");
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
		  "ATE0X3D\r\r\nNO CARRIER\r\n\r\nERROR\r\n");
	EXPECT_TRUE(!m->started());
	EXPECT_EQ(m->link_state(), LinkState::Idle);
	l.fail_start = 0;
	l.fail_hook = 1;
	EXPECT_EQ(cmd(m, l, "ATA\r"), "\r\nNO CARRIER\r\n\r\nERROR\r\n");
	EXPECT_TRUE(!m->started());
	EXPECT_EQ(cmd(m, l, "ATH1\r"), "\r\nERROR\r\n");
	l.fail_hook = 0;
	EXPECT_EQ(cmd(m, l, "ATH1\r"), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "ATH\r"), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	EXPECT_EQ(cmd(m, l, "ATM2L2\r"), "\r\nOK\r\n");
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
	EXPECT_EQ(l.out, "\r\nFAX\r\n");
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
	EXPECT_EQ(cmd(m, l, "ATE0+PMH?\r"), "ATE0+PMH?\r\r\n+PMH: 0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+PMHT?\r"), "\r\n+PMHT: 5\r\n\r\nOK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 5);
	EXPECT_EQ(cmd(m, l, "AT+PMHT=?\r"), "\r\n+PMHT: (0-13)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+PMHT=9\r"), "\r\nOK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 9);
	EXPECT_EQ(cmd(m, l, "AT+PMHT=14\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+PMH=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_T1), 0);
	EXPECT_EQ(cmd(m, l, "AT+PMH=2\r"), "\r\nERROR\r\n");
	/* without a call */
	EXPECT_EQ(cmd(m, l, "AT+PMHR\r"), "\r\nERROR\r\n");
	EXPECT_EQ(m->get_param(MDMPRM_MOH_REQ), 0);
	/* ATZ: granted again, 1 min */
	EXPECT_EQ(cmd(m, l, "ATZ\r"), "\r\nOK\r\n");
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
		EXPECT_EQ(run(1), "\r\nOK\r\n");
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
	EXPECT_EQ(run(1), "\r\nOK\r\n");
	EXPECT_EQ(gone(), "\x10l");
	EXPECT_FALSE(m->started());
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "\r\n0\r\n\r\nOK\r\n");

	EXPECT_EQ(taken("AT+VRX\r").substr(0, 9), "\r\nCONNECT");
	EXPECT_EQ(gone(), "\x10"
"l\x10"
"\x03"
"\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT\r"), "\r\nOK\r\n");

	EXPECT_EQ(taken("AT+VTX\r"), "\r\nCONNECT\r\n");
	EXPECT_EQ(gone(), "\x10l");
	EXPECT_EQ(cmd(m, l, "AT\rx\x10\x10\x10~"), "");
	EXPECT_EQ(cmd(m, l, "\x10\x03" "AT+VLS?\r"), "\r\nOK\r\n\r\n0\r\n\r\nOK\r\n");

	EXPECT_EQ(taken("AT+VTR\r").substr(0, 9), "\r\nCONNECT");
	EXPECT_EQ(gone(), "\x10l");
	EXPECT_EQ(cmd(m, l, "AT\r\x10\x03\x10!"), "");
	EXPECT_EQ(cmd(m, l, "\x10^AT\r"), "\x10"
"\x03"
"\r\nOK\r\n\r\nOK\r\n");

	cmd(m, l, "AT+VLS=1\r");
	run(1);
	cmd(m, l, "AT+VTS=[440,0,100]\r");
	EXPECT_EQ(run(2), "");
	EXPECT_EQ(gone(), "\x10"
"l\r\nERROR\r\n");

	/* a voice dial the line fails (FAILED 404, its socket's end) */
	cmd(m, l, "AT+VRN=20\r");
	EXPECT_EQ(cmd(m, l, "ATD5551000\r"), "");
	run(2);
	l.out.clear();
	m->line_failed(Result::NoCarrier);
	m->line_dropped();
	EXPECT_EQ(l.out, "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "ATD5551000\r"), "");
	EXPECT_EQ(gone(), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "\r\n0\r\n\r\nOK\r\n");
	delete m;
}

/* Rockwell's and US Robotics' voice dialect (#CLS=8): the modes, the
   sample format's commands (#VBS, #VSR, #VSM: US Robotics' ADPCM only
   after #VSM=129, at 8000 only), the kept settings in hex or decimal,
   VCON for the line taken (ATA; #VLS=0A, US Robotics' way to answer),
   #VTX as +VTX, #VTS's durations in 100 ms; +FCLASS=8 is V.253's again */
TEST(Core, VoiceHashDialect)
{
	Line l;
	Modem *m = make(l, "hash");
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

	EXPECT_EQ(cmd(m, l, "ATE0#CLS=?\r"), "ATE0#CLS=?\r\r\n0,1,2,8\r\n\r\nOK\r\n");
	/* settings in data mode too, as vgetty's Rockwell driver sends
	   them before #CLS=8 */
	EXPECT_EQ(cmd(m, l, "AT#VSP=55#VSD=0#VTD=3F,3F,3F#VSS=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VTD?\r"), "\r\n3F,3F,3F\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VSP?;#VSS=?\r"), "\r\n55\r\n(0-3)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VSS=4\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT#TL=8000\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VTX\r"), "\r\nERROR\r\n");  /* not in voice */
	EXPECT_EQ(cmd(m, l, "AT#CLS=8\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#CLS?;+FCLASS?\r"), "\r\n8\r\n8\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VLS=?\r"), "\r\n0,4\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VLS=2\r"), "\r\nERROR\r\n");

	/* the sample format */
	EXPECT_EQ(cmd(m, l, "AT#VBS?;#VSR?\r"), "\r\n8\r\n8000\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VBS=2\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VBS=4\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VBS=16#VSR=7200\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VSM?\r"), "\r\n129,7200,0,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VSM=128,8000\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VSM=129,8000#VBS=4\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VBS=?;#VSM?\r"),
		  "\r\n4,8,16\r\n129,8000\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VSM?\r"), "\r\n133,8000,0,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VSR=7200\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT#VBS=8\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VSM?\r"), "\r\n1,8000,0,0\r\n\r\nOK\r\n");

	/* the line taken: VCON, as a number with ATV0 */
	EXPECT_EQ(cmd(m, l, "AT#VLS=0A\r"), "");
	EXPECT_EQ(run(1), "\r\nVCON\r\n");
	EXPECT_TRUE(m->started());
	EXPECT_EQ(cmd(m, l, "AT#VTX\r"), "");
	EXPECT_EQ(run(1), "\r\nCONNECT\r\n");
	EXPECT_EQ(cmd(m, l, "\x10\x03"), "");
	EXPECT_EQ(run(1), "\r\nOK\r\n");
	/* [f1,f2,d]: d in 100 ms, so 0.3 s */
	EXPECT_EQ(cmd(m, l, "AT#VTS=[440,0,3]\r"), "");
	EXPECT_EQ(run(10), "");
	EXPECT_EQ(run(10), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "ATH0\r"), "\r\nOK\r\n");
	EXPECT_FALSE(m->started());
	EXPECT_EQ(cmd(m, l, "AT#CLS?\r"), "\r\n0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "ATV0#CLS=8\r"), "0\r");
	EXPECT_EQ(cmd(m, l, "ATA\r"), "");
	EXPECT_EQ(run(1), "90\r");
	EXPECT_EQ(cmd(m, l, "ATV1H0\r"), "\r\nOK\r\n");

	/* +FCLASS=8: V.253's OK for the line taken */
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=8;+VLS=1\r"), "");
	EXPECT_EQ(run(1), "\r\nOK\r\n");
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
		  "\r\n0,\"\",00800000,00800000,90800000\r\n1,\"T\",0B843060,0B843060,0B843060\r\n\r\nOK\r\n");
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
	EXPECT_EQ(cmd(m, l, "AT+VCID=0\r"), "\r\nOK\r\n");
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
	EXPECT_EQ(run(std::vector<s16>(160)), "\r\nOK\r\n");
	sent.clear();
	EXPECT_EQ(run(cas), "");
	EXPECT_EQ(key(0, sent.size()), 'A');
	std::string got = run(msg);
	EXPECT_NE(got.find("\x10XNMBR = 5551000\r\nNAME = BOB\r\n\x10."),
		  std::string::npos) << got;

	/* +VCID=0: no acknowledgement, no report */
	EXPECT_EQ(cmd(m, l, "AT+VCID=0\r"), "\r\nOK\r\n");
	sent.clear();
	EXPECT_EQ(run(cas), "");
	EXPECT_EQ(key(0, sent.size()), ' ');
	EXPECT_EQ(run(msg), "");
	cmd(m, l, "AT+VLS=0\r");
	delete m;
}

/* a SIP call's caller (caller_id()) after its first RING, as +VCID asks:
   formatted (1), the MDMF message in hex (2), a voice mode packet, none
   (0), withheld and unknown fields; only the first RING has it; +VRID
   gives the last one again */
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
		EXPECT_EQ(l.out, "\r\nRING\r\n");
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
	EXPECT_EQ(cmd(m, l, "AT+VCID=2\r"), "\r\nOK\r\n");
	EXPECT_EQ(ring("", "5551000", ""),
		  "\r\nRING\r\n\r\nMESG = 800C0207353535313030300801"
		  "4FB3\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VCID=0\r"), "\r\nOK\r\n");
	EXPECT_EQ(ring("10301043", "5551000", "Bob"), "\r\nRING\r\n");
	/* +VRID (V.253 9.1.3): the last call's caller again, with +VCID
	   0 too: formatted (0, the default) or in hex (1) */
	EXPECT_EQ(cmd(m, l, "AT+VRID\r"),
		  "\r\nDATE = 1030\r\nTIME = 1043\r\nNMBR = 5551000\r\n"
		  "NAME = Bob\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VRID=0\r"), cmd(m, l, "AT+VRID\r"));
	EXPECT_EQ(cmd(m, l, "AT+VRID=1\r").substr(0, 15),
		  "\r\nMESG = 801801");
	EXPECT_EQ(cmd(m, l, "AT+VRID=?\r"), "\r\n(0,1)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VRID=2\r"), "\r\nERROR\r\n");
	/* voice mode: the packet */
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=8;+VCID=1\r"), "\r\nOK\r\n");
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
	EXPECT_EQ(cmd(m, l, "AT+VLS=14\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=15\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=2\r"), "\r\nERROR\r\n");
	EXPECT_TRUE(l.routes.empty());
	l.handset = true;
	EXPECT_EQ(cmd(m, l, "AT+VLS=?\r"),
		  "\r\n0,\"\",00000000,00000000,90000000\r\n1,\"T\",0B043060,0B043060,0B043060\r\n14,\"H\",08000000,08000000,08000000\r\n15,\"HT\",0B043060,0B043060,0B043060\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=13\r"), "\r\nERROR\r\n");

	l.last_hook = -1;
	cmd(m, l, "AT+VLS=14\r");
	EXPECT_EQ(run(1), "\r\nOK\r\n");
	EXPECT_TRUE(m->started());
	EXPECT_EQ(l.last_hook, -1);             /* still on hook */
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "\r\n14\r\n\r\nOK\r\n");
	cmd(m, l, "AT+VRX\r");
	EXPECT_EQ(run(1).substr(0, 9), "\r\nCONNECT");
	run(5);
	cmd(m, l, "x");
	run(1);
	EXPECT_NE(l.out.find("\x10\x03\r\nOK\r\n"), std::string::npos);
	EXPECT_EQ(cmd(m, l, "AT+VLS=15\r"), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "\r\n15\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "AT+VLS=14\r"), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	EXPECT_TRUE(m->started());
	EXPECT_EQ(cmd(m, l, "AT+VLS=0\r"), "\r\nOK\r\n");
	EXPECT_FALSE(m->started());
	EXPECT_EQ(l.routes, (std::vector<unsigned>{ 14, 15, 1, 14, 1 }));

	l.routes.clear();
	cmd(m, l, "AT+VLS=15\r");
	EXPECT_EQ(run(1), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_OFF);
	EXPECT_EQ(cmd(m, l, "ATH\r"), "\r\nOK\r\n");
	EXPECT_EQ(l.last_hook, MODEM_HOOK_ON);
	EXPECT_EQ(l.routes, (std::vector<unsigned>{ 15, 1 }));
	delete m;
}

/* V.253's event reports: +VLS=? lists each device with the events it
   reports in the transmit, receive and command states (Table 1's bits,
   event 0 leftmost); +VEM, the speakerphone's reports, 0 or 1, back to
   0 with +VIP; neither outside voice mode; the test (=?) of the
   actions +VIP, +VTX, +VRX and +VTR: OK */
TEST(Core, VoiceEvents)
{
	Line l;
	Modem *m = make(l, "events");

	EXPECT_EQ(cmd(m, l, "ATE0+VEM?\r"), "ATE0+VEM?\r\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=8\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS=?\r"),
		  "\r\n0,\"\",00000000,00000000,90000000\r\n1,\"T\",0B043060,0B043060,0B043060\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VLS?\r"), "\r\n0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "\r\n0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM=?\r"), "\r\n(0-1)\r\n\r\nOK\r\n");
	/* an action's test (V.253 6.3.3.1.2): OK */
	for (const char *a : { "AT+VIP=?\r", "AT+VTX=?\r", "AT+VRX=?\r",
			       "AT+VTR=?\r" })
		EXPECT_EQ(cmd(m, l, a), "\r\nOK\r\n") << a;
	EXPECT_EQ(cmd(m, l, "AT+VEM=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "\r\n1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM=2\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "\r\n1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VIP\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+VEM?\r"), "\r\n0\r\n\r\nOK\r\n");
	delete m;
}

/* two modems back to back */
struct Pair {
	Line la, lb;
	Modem *a, *b;
	std::vector<int16_t> ia, ib, oa, ob;
	std::string got_a, got_b;       /* what each DTE received */
	static const int BLOCK = 160;
	/* two modems, ATE0X3 and the modulation command ms; the
	   program's speakers sa and sb (NULL: none) */
	Pair(const char *ms, dcore::SpeakerPath *sa = nullptr,
	     dcore::SpeakerPath *sb = nullptr) {
		a = make(la, "A", nullptr, sa);
		b = make(lb, "B", nullptr, sb);
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

/* V.8 between two V.34 modems: each AT#UD has the CM and the JM (keys
   4 and 5).  The CM: a data call; V.34; V.32bis, V.22bis; V.23 duplex,
   V.21; LAPM; PSTN access.  The JM: the same, because the two modems
   have the same menu (7.4/V.8) */
TEST(Core, V8MenusConform)
{
	Pair p("+MS=34");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	p.run_until([](Pair &) { return false; }, 1100);
	p.got_a.clear();
	p.got_b.clear();
	p.a->dte_input("+++", 3);
	p.b->dte_input("+++", 3);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos &&
			q.got_b.find("OK") != std::string::npos; }, 2000));
	const std::string menu = "\"C14513942A0D\"";
	for (Modem *m : { p.a, p.b }) {
		std::string ud = cmd(m, m == p.a ? p.la : p.lb, "AT#UD\r");
		EXPECT_NE(ud.find(" 4=" + menu), std::string::npos) << ud;
		EXPECT_NE(ud.find(" 5=" + menu), std::string::npos) << ud;
	}
}

/* an EBCDIC DTE on a call (+DEBC=1,1, V.22): its data as ASCII to the
   remote, the remote's as EBCDIC, CONNECT in EBCDIC, the escape as
   EBCDIC "+++" (0x4E 0x4E 0x4E), its OK in EBCDIC */
TEST(Core, EbcdicCall)
{
	Pair p("+MS=22");
	std::string s = ebc("AT+DEBC=1,1\r");
	p.a->dte_input(s.data(), s.size());
	EXPECT_EQ(p.la.out, ebc("\r\nOK\r\n"));
	p.la.out.clear();
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_NE(p.got_a.find(ebc("CONNECT")), std::string::npos) << p.got_a;
	EXPECT_EQ(p.got_b.find(ebc("CONNECT")), std::string::npos);
	p.got_a.clear();
	p.got_b.clear();
	std::string da = "Hello from an EBCDIC DTE [037]", db = "Hi, ASCII here!";
	s = ebc(da);
	p.a->dte_input(s.data(), s.size());
	p.b->dte_input(db.data(), db.size());
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.got_b.size() >= 30 && q.got_a.size() >= 15; }, 15000));
	EXPECT_EQ(p.got_b, da);
	EXPECT_EQ(p.got_a, ebc(db));
	p.got_a.clear();
	p.run_until([](Pair &) { return false; }, 1100);
	p.a->dte_input("\x4E\x4E\x4E", 3);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.got_a.find(ebc("OK")) != std::string::npos; }, 2000))
		<< p.got_a;
	EXPECT_EQ(p.a->link_state(), LinkState::Online);
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
			EXPECT_EQ(p.got_b, "\r\nOK\r\n");
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
	EXPECT_EQ(cmd(m, l, "AT+FCLASS=0\r"), "\r\nOK\r\n");
	EXPECT_TRUE(m->started());
	EXPECT_EQ(cmd(m, l, "ATH\r"), "\r\nOK\r\n");
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
   answering; +ESA's FCS-16; +ER: NONE before CONNECT): frames each way between the DTEs, then
   <EM><ecs> (online command state, OK, ATO back), and <EM><eot>: the
   carrier ends, NO CARRIER at both, <EM><eot> before it at the other,
   and the two lines off hook until ATH */
TEST(Core, V80)
{
	Pair p("+MS=132;+ES=6,,8;+ESA=,,,,1;+ITF=383,128,0;+ER=1");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ES?\r"), "\r\n+ES: 6,0,8\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ESA?\r"),
		  "\r\n+ESA: 0,0,0,0,1,0,255,255\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ITF=10,20\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ES=5\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(p.a, p.la, "AT+ESA=3\r"), "\r\nERROR\r\n");
	p.la.out.clear();

	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_TRUE(p.got_a.find("CONNECT") != std::string::npos);
	EXPECT_TRUE(p.got_b.find("CONNECT") != std::string::npos);
	/* 8.3/V.80: +ER: NONE before CONNECT */
	EXPECT_TRUE(p.got_a.find("+ER: NONE\r\n\r\nCONNECT") !=
		    std::string::npos) << p.got_a.substr(0, 80);
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
		return q.got_a.find("NO CARRIER") != std::string::npos &&
			q.got_b.find("NO CARRIER") != std::string::npos; },
		20000));
	EXPECT_EQ(p.got_a.substr(0, 2), std::string("\x19\xba"));
	EXPECT_TRUE(p.got_a.find("NO CARRIER") != std::string::npos);
	size_t e = p.got_b.find("\x19\xba");
	EXPECT_TRUE(e != std::string::npos &&
		    p.got_b.find("NO CARRIER", e) != std::string::npos)
		<< "B: [" << p.got_b.substr(p.got_b.size() > 40 ?
					   p.got_b.size() - 40 : 0) << "]";
	/* 8.8.4.3/V.80: the two lines stay off hook in command state,
	   until ATH */
	p.run_until([](Pair &) { return false; }, 500);
	EXPECT_TRUE(p.a->started());
	EXPECT_TRUE(p.b->started());
	EXPECT_EQ(cmd(p.a, p.la, "ATH\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(p.b, p.lb, "ATH\r"), "\r\nOK\r\n");
	EXPECT_FALSE(p.a->started());
	EXPECT_FALSE(p.b->started());
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
   renegotiation, on a call with LAPM at modulation ms (+MS=34 ...); the
   link goes on; ATI11 counts them; ATO2 is an error */
static void retrain_on_request(const char *ms)
{
	Pair p(ms);
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
			EXPECT_EQ(cmd(p.a, p.la, "ATO2\r"), "\r\nERROR\r\n");
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

/* (see retrain_on_request()) on V.34 */
TEST(Core, RetrainOnRequest)
{
	retrain_on_request("+MS=34");
}

/* and on V.61 (its rate renegotiation of clause 11) */
TEST(Core, RetrainOnRequestV61)
{
	retrain_on_request("+MS=61");
}

/* MNP 10 between two D-Modems: the caller starts at V.22 (*H1), and the
   fall forwards climb across the modulations, by start-ups in each new
   one (V.22bis, V.32bis, V.32terbo, V.34 through V.8) and rate
   renegotiations within V.32bis and V.32terbo, to V.34; the data both
   ways after */
TEST(Core, Mnp10Climbs)
{
	if (getenv("SHOW")) {
		dcore::log_set_hook([](const char *s) { fputs(s, stdout); });
		dcore::log_level = 2;
	}
	Pair p("+MS=34+ES=4,0,2*H1");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	printf("online at %u/%u bit/s\n", p.a->tx_rate(), p.a->rx_rate());
	EXPECT_EQ(p.a->tx_rate(), 1200u);
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.a->tx_rate() >= 26400 && q.b->tx_rate() >= 26400 &&
			q.a->link_state() == LinkState::Online; }, 400000))
		<< p.a->tx_rate() << "/" << p.b->tx_rate();
	printf("climbed to %u/%u bit/s\n", p.a->tx_rate(), p.b->tx_rate());
	std::string da = data('a', 20000), db = data('b', 15000);
	size_t sa = 0, sb = 0;
	p.got_a.clear();
	p.got_b.clear();
	for (int t = 0; t < 120000; t += 20) {
		int n = std::min<int>(p.a->dte_space(), da.size() - sa);
		sa += p.a->dte_input(da.data() + sa, n);
		n = std::min<int>(p.b->dte_space(), db.size() - sb);
		sb += p.b->dte_input(db.data() + sb, n);
		p.step();
		if (p.got_b.size() >= da.size() && p.got_a.size() >= db.size())
			break;
	}
	EXPECT_TRUE(p.got_b == da) << "B got " << p.got_b.size();
	EXPECT_TRUE(p.got_a == db) << "A got " << p.got_a.size();
	EXPECT_TRUE(p.a->carrier() && p.b->carrier());
	dcore::log_level = 0;
	dcore::log_set_hook(nullptr);
}

/* the program's speaker (Options::speaker): the volumes it was told,
   the rate, and the samples it had (what came, what was sent) */
struct Speaker final : public dcore::SpeakerPath {
	std::vector<int> levels;
	s32 rate = 0;
	long samples = 0;
	double tx_pow = 0;
	/* kept */
	void volume(s16 level, s32 r) override {
		levels.push_back(level);
		rate = r;
	}
	/* counted, tx's power added up */
	void audio(const s16 *, const s16 *tx, s32 n) override {
		samples += n;
		for (s32 i = 0; i < n; i++)
			tx_pow += static_cast<double>(tx[i]) * tx[i];
	}
};

/* the two modems of p online (V.22's handshake) */
static bool online(Pair &p)
{
	return p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 20000);
}

/* ATM1 (the default): on at ATL's volume while the call starts, both
   ends, the line's samples meanwhile; off when it is connected, and
   no samples after.  The Host is told too */
TEST(Core, SpeakerUntilConnect)
{
	Speaker sa, sb;
	Pair p("+MS=V22", &sa, &sb);
	p.a->dte_input("ATL2\r", 5);
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(online(p));
	EXPECT_EQ(sa.levels, (std::vector<int>{ 2, 0 }));
	EXPECT_EQ(sb.levels, (std::vector<int>{ 3, 0 }));
	EXPECT_EQ(sa.rate, 8000);
	EXPECT_EQ(p.la.volume, 0);
	EXPECT_GT(sa.samples, 8000);            /* the handshake's, 1 s+ */
	EXPECT_GT(sb.tx_pow, 0.0);              /* the answer tone */
	long heard = sa.samples;
	p.run_until([](Pair &) { return false; }, 1000);
	EXPECT_EQ(sa.samples, heard);
}

/* ATM3: not while the number is dialled (an analogue line's DTMF),
   then on until the call is connected; ATM1 at once */
TEST(Core, SpeakerAfterDialling)
{
	for (const char *m : { "ATM3\r", "ATM1\r" }) {
		SCOPED_TRACE(m);
		Speaker sa;
		Pair p("+MS=V22", &sa);
		p.la.analog = true;
		p.a->dte_input(m, 5);
		EXPECT_EQ(p.b->answer(), 0);
		EXPECT_EQ(p.a->dial("123"), 0);
		p.step();
		if (m[3] == '3') {
			EXPECT_TRUE(sa.levels.empty());
		} else {
			EXPECT_EQ(sa.levels, (std::vector<int>{ 3 }));
		}
		/* (three digits: well over 300 ms) */
		p.run_until([](Pair &) { return false; }, 200);
		if (m[3] == '3') {
			EXPECT_TRUE(sa.levels.empty());
		}
		ASSERT_TRUE(online(p));
		EXPECT_EQ(sa.levels, (std::vector<int>{ 3, 0 }));
		EXPECT_GT(sa.samples, 0);
	}
}

/* ATM2: on at once, and through the call; ATM0: never on */
TEST(Core, SpeakerAlwaysAndNever)
{
	Speaker sa, sb;
	Pair p("+MS=V22", &sa, &sb);
	p.a->dte_input("ATM2\r", 5);
	p.b->dte_input("ATM0\r", 5);
	EXPECT_EQ(sa.levels, (std::vector<int>{ 3 }));
	EXPECT_TRUE(sb.levels.empty());
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(online(p));
	long heard = sa.samples;
	p.run_until([](Pair &) { return false; }, 500);
	EXPECT_GT(sa.samples, heard);
	EXPECT_EQ(sa.levels, (std::vector<int>{ 3 }));
	EXPECT_TRUE(sb.levels.empty());
	EXPECT_EQ(sb.samples, 0);
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
	EXPECT_EQ(p.got_a, "\r\nOK\r\n\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "\r\nOK\r\n\r\nOK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	p.a->dte_input("AT+FCLASS?;+FNR?\r", 17);
	p.step();
	EXPECT_EQ(p.got_a, "\r\n" + std::string(c.cls) +
		  "\r\n1,1,1,0\r\n\r\nOK\r\n");
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
		  ":1,5,0,2,0," + (c.ec ? "2" : "0") + ",0,0\r\n\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "");
	p.got_a.clear();
	/* the DCS goes with +FDT: then B's ATA has its OK */
	p.a->dte_input("AT+FDT\r", 7);
	ASSERT_TRUE(until([&] {
		return has(p.got_a, "CONNECT") && has(p.got_b, "OK\r\n");
	}, 20000)) << "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(br(p.got_b), "+" + nm("FCON", "FCO") + "\r\n+" +
		  nm("FTSI", "FTI") + ":\"Fax A 111\"\r\n+" + nm("FDCS", "FCS") +
		  ":1,5,0,2,0," + (c.ec ? "2" : "0") + ",0,0\r\n\r\nOK\r\n");
	p.got_b.clear();

	for (int page = 0; page < 2; page++) {
		dsp::T4Page pg = fax2_page(300 + 50 * page, page * 333);
		std::string d = fax2_dte_data(pg, c.bo & 1);
		size_t sent = 0;

		if (page)
			p.a->dte_input("AT+FDT\r", 7);
		ASSERT_TRUE(until([&] { return has(p.got_a, "CONNECT"); },
				  20000)) << page << ": A [" << p.got_a << "]";
		if (!page) {
			EXPECT_TRUE(has(br(p.got_a), "+" + nm("FDCS", "FCS") +
					":1,5,0,2,0,")) << p.got_a;
		}
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
		EXPECT_EQ(has(p.got_a, "CONNECT\r\n\x11\r\nOK"), c2) << p.got_a;
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
				"\r\n\r\nOK\r\n")) << page;
		p.got_b.clear();

		ASSERT_TRUE(until([&] { return ok_after(p.got_a, "+F"); },
				  30000)) << page << ": A [" << p.got_a << "]";
		EXPECT_EQ(p.got_a, "+" + nm("FPTS", "FPS") + ":1,0,0,0,0\r\n" +
			  (page ? "+" + nm("FHNG", "FHS") + ":" +
			   (c2 ? "0" : "00") + "\r\n" : "") + "\r\nOK\r\n");
		p.got_a.clear();
	}
	p.b->dte_input("AT+FDR\r", 7);
	ASSERT_TRUE(until([&] { return has(p.got_b, "OK\r\n"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+" + nm("FHNG", "FHS") + ":" + (c2 ? "0" : "00") +
		  "\r\n\r\nOK\r\n");
	EXPECT_TRUE(until([&] {
		return !p.a->started() && !p.b->started();
	}, 2000));
	/* (the pages went as T.38's packets, or as audio) */
	EXPECT_EQ(p.la.t38_sent > 100 && p.lb.t38_sent > 20, c.t38 != 0)
		<< p.la.t38_sent << " " << p.lb.t38_sent;
	p.a->dte_input("AT+FHS?\r", 8);
	p.step();
	EXPECT_EQ(p.got_a, "\r\n" + std::string(c2 ? "0" : "00") +
		  "\r\n\r\nOK\r\n");
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
	EXPECT_EQ(p.got_a, "+FHS:02\r\n\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "+FHS:70\r\n\r\nNO CARRIER\r\n");
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

	type(p.a, "AT+FCLASS=2.0;+FNR=1,1,1,0;+FLI=\"Caller A\"\r");
	type(p.a, "AT+FPI=\"Poller A\"\r");
	type(p.b, "AT+FCLASS=2.0;+FNR=1,1,1,0;+FLI=\"Polled B\";+FLP=1\r");
	p.step();
	EXPECT_EQ(p.got_a, "\r\nOK\r\n\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "\r\nOK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	type(p.b, "ATA\r");
	type(p.a, "ATD\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 30000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "+FCO\r\n+FCI:\"Polled B\"\r\n+FIS:1,5,0,2,0,0,0,0\r\n+FPO\r\n\r\nOK\r\n");
	p.got_a.clear();
	type(p.a, "AT+FDR\r");
	p.step();
	EXPECT_EQ(p.got_a, "\r\nERROR\r\n");
	p.got_a.clear();
	type(p.a, "AT+FSP=1;+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "OK\r\n"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+FCO\r\n+FPI:\"Poller A\"\r\n+FTC:1,5,0,2,0,0,0,0\r\n\r\nOK\r\n");
	p.got_b.clear();

	type(p.b, "AT+FDT\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "CONNECT"); }, 20000))
		<< "B [" << p.got_b << "]";
	EXPECT_TRUE(has(p.got_b, "+FCS:1,5,0,2,0,0,0,0\r\n\r\nCONNECT")) <<
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
			"\r\n\r\nCONNECT")) << p.got_a;
	type(p.a, "\x12");
	ASSERT_TRUE(until([&] { return ok_after(p.got_a, "+FET:"); }, 5000))
		<< "A [" << p.got_a << "]";
	dsp::T4Page rx = fax2_decode(p.got_a, false, true);
	EXPECT_EQ(rx.rows, pg.rows);
	EXPECT_TRUE(rx.bits == pg.bits);
	EXPECT_TRUE(has(p.got_a, "+FET:2\r\n\r\nOK\r\n")) << p.got_a;
	p.got_a.clear();
	ASSERT_TRUE(until([&] { return ok_after(p.got_b, "+FHS"); }, 30000))
		<< "B [" << p.got_b << "]";
	EXPECT_EQ(p.got_b, "+FPS:1,0,0,0,0\r\n+FHS:00\r\n\r\nOK\r\n");
	type(p.a, "AT+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 20000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "+FHS:00\r\n\r\nOK\r\n");
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
	type(p.a, "AT+FPI=\"Poller A\";+FSP=1;+FNR=1,1,1,0\r");
	type(p.b, "AT+FCLASS=2.0;+FLI=\"Polled B\";+FLP=1;"
		  "+FIS=1,13,0,2,0,2,0,0\r");
	type(p.b, "AT+FNR=1,1,1,0\r");
	p.step();
	EXPECT_EQ(p.got_a, "\r\nOK\r\n\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "\r\nOK\r\n\r\nOK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	type(p.b, "ATA\r");
	type(p.a, "ATD\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 30000))
		<< "A [" << p.got_a << "]";
	EXPECT_GE(br_after(p.got_a, "+FIS:1,"), 8) << p.got_a;
	EXPECT_TRUE(has(p.got_a, "+FPO\r\n\r\nOK\r\n")) << p.got_a;
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
	EXPECT_EQ(p.got_b, "+FPS:1,0,0,0,0\r\n+FHS:00\r\n\r\nOK\r\n");
	type(p.a, "AT+FDR\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 20000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "+FHS:00\r\n\r\nOK\r\n");
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
	if (getenv("TEXT_DEBUG")) {
		dcore::log_set_hook([](const char *s) { fputs(s, stderr); });
		dcore::log_level = 2;
		dsplibs_debug_level = 2;
	}
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
   shift); EDT; DTMF (a new line LF alone on the line, CR LF to the
   DTE); the V.21 text telephones; V.18 automoding answering a TTY, an
   EDT terminal, a DTMF one, a V.21, a Bell 103 and a V.23 (Minitel)
   text telephone, and another automoding modem (CI, the answer tone
   and TXP: V.18's own mode); V.18 automoding calling a V.21, a Bell
   103 and a V.23 text telephone (their carriers tell) and a V.21, a
   Bell 103 and a V.23 data modem (after the answer tone and our TXP
   for V.21 and V.23; their 8N1 receivers see the parity bit, so the
   line has only characters of even parity) */
static const TextCall text_calls[] = {
	{ "+MS=BAUDOT", "+MS=BAUDOT", 45, 0, "Hello 123-?\r\n",
	  "HELLO 123-?\r\n", nullptr },
	{ "+MBAUDOT=50,2125,2295,1;+MS=BAUDOT",
	  "+MBAUDOT=50,2125,2295,1;+MS=BAUDOT", 50, 0, "RYRY 1/2=3+4\r\n",
	  "RYRY 1/2=3+4\r\n", nullptr },
	{ "+MS=EDT", "+MS=EDT", 110, 0, "Hello, edt!\r\n", "Hello, edt!\r\n",
	  nullptr },
	{ "+MS=DTMF", "+MV18S=3", 10, 0, "Hi, DTMF 42?\r\n",
	  "Hi, DTMF 42?\r\n", nullptr },
	{ "+MV18S=5", "+MS=V21TXT", 300, 0, "Hej, V.21 {*}\r\n",
	  "Hej, V.21 {*}\r\n", nullptr },
	{ "+MS=BAUDOT", "+MV18S=1;+MV18R=1", 45, 0, "GA\r\n", "GA\r\n",
	  "+MV18R: 2" },
	{ "+MS=EDT", "+MV18S=1;+MV18R=1", 110, 0, "Hi edt\r\n",
	  "Hi edt\r\n", "+MV18R: 4" },
	{ "+MS=DTMF", "+MV18S=1;+MV18R=1", 10, 0, "Hi dtmf\r\n",
	  "Hi dtmf\r\n", "+MV18R: 3" },
	{ "+MV18S=5", "+MV18S=1;+MV18R=1", 300, 0, "Hello GA\r\n",
	  "Hello GA\r\n", "+MV18R: 5" },
	{ "+MV18S=7", "+MV18S=1;+MV18R=1", 300, 0, "Hello GA\r\n",
	  "Hello GA\r\n", "+MV18R: 7" },
	{ "+MV18S=6", "+MV18S=1;+MV18R=1", 1200, 75, "Hello GA\r\n",
	  "Hello GA\r\n", "+MV18R: 6" },
	{ "+MS=V18", "+MS=V18;+MV18R=1", 300, 0, "Hello, V.18\r\n",
	  "Hello, V.18\r\n", "+MV18R: 1" },
	{ "+MS=V18;+MV18R=1", "+MV18S=5", 300, 0, "Hello GA\r\n",
	  "Hello GA\r\n", "+MV18R: 5" },
	{ "+MS=V18;+MV18R=1", "+MV18S=7", 300, 0, "Hello GA\r\n",
	  "Hello GA\r\n", "+MV18R: 7" },
	{ "+MS=V18;+MV18R=1", "+MV18S=6", 1200, 75, "Hello GA\r\n",
	  "Hello GA\r\n", "+MV18R: 6" },
	{ "+MS=V18;+MV18R=1", "+MS=21,0", 300, 0, "Hello\n", "Hello\n",
	  "+MV18R: 5" },
	{ "+MS=V18;+MV18R=1", "+MS=103,0", 300, 0, "Hello\n", "Hello\n",
	  "+MV18R: 7" },
	{ "+MS=V18;+MV18R=1", "+MS=V23C,0", 1200, 75, "Hello\n",
	  "Hello\n", "+MV18R: 6" },
	/* Table A.1 and A.2/V.18: = and + in figures, # and & sent as $
	   and +, the backspace 00000 */
	{ "+MS=BAUDOT", "+MS=BAUDOT", 45, 0, "1=2+3#4&5\b6\r\n",
	  "1=2+3$4+5\b6\r\n", nullptr },
	/* 5.2.5/V.18: automoding finds a TTY at 50 bit/s by its bits'
	   length */
	{ "+MBAUDOT=50,1400,1800,0;+MS=BAUDOT", "+MV18S=1;+MV18R=1", 50, 0,
	  "GA 12\r\n", "GA 12\r\n", "+MV18R: 2" },
};

/* each case's name */
static std::string text_name(const testing::TestParamInfo<TextCall> &i)
{
	static const char *names[] = {
		"Tty", "Telex", "Edt", "Dtmf", "V21Text", "V18Tty", "V18Edt",
		"V18Dtmf", "V18AnswersV21", "V18AnswersBell103",
		"V18AnswersMinitel", "V18V18", "V18CallsV21",
		"V18CallsBell103", "V18CallsMinitel", "V18CallsV21Modem",
		"V18CallsBell103Modem", "V18CallsV23Modem", "TtyCodeTable",
		"V18Tty50",
	};
	return names[i.index];
}

INSTANTIATE_TEST_SUITE_P(Modes, CoreText, testing::ValuesIn(text_calls),
			 text_name);

/* a modem's line signal, in 20 ms blocks: each block's strongest tone
   of V.18's (0 for silence) */
static std::vector<int> v18_blocks(const std::vector<int16_t> &x)
{
	static const int f[] = { 980, 1180, 1300, 1400, 1650, 1800, 2100,
				 2225, 697, 1209 };
	std::vector<int> out;
	for (size_t at = 0; at + 160 <= x.size(); at += 160) {
		float best = 100.0f * 100.0f / 2, e = 0, p2100 = 0;
		int tone = 0;
		for (int k : f) {
			dsp::Goertzel g;
			g.init(static_cast<float>(k), 160, 8000);
			for (size_t i = 0; i < 160; i++)
				g.run(x[at + i]);
			if (g.power > best) {
				best = g.power;
				tone = k;
			}
			if (k == 2100)
				p2100 = g.power;
		}
		/* (ANSam's phase reversal spreads its block toward 2225
		   Hz) */
		if (tone == 2225 && p2100 > 0.3f * best)
			tone = 2100;
		for (size_t i = 0; i < 160; i++)
			e += static_cast<float>(x[at + i]) * x[at + i];
		/* (a block with energy, but no tone of the list: a mix) */
		if (!tone && e / 160 > 100.0f * 100.0f / 2)
			tone = -1;
		out.push_back(tone);
	}
	/* (a block between two of the same tone: a phase reversal) */
	for (size_t i = 1; i + 1 < out.size(); i++)
		if (out[i - 1] == out[i + 1] && out[i - 1] > 0)
			out[i] = out[i - 1];
	return out;
}

/* the 5-bit codes on a line signal x at baud (1400 and 1800 Hz), and
   each one's start, in samples */
static std::vector<std::pair<int, size_t>> v18_codes(
	const std::vector<int16_t> &x, float baud)
{
	dsp::FskDemod d;
	dsp::Uart u;
	d.init(1400, 1800, baud, 8000);
	u.init(baud, 5, 8000);
	std::vector<std::pair<int, size_t>> out;
	size_t start = 0;
	for (size_t i = 0; i < x.size(); i++) {
		d.run(x[i]);
		bool was = u.busy;
		int32_t v = u.run(d.last_dec, d.carrier);
		if (!was && u.busy)
			start = i;
		if (v >= 0)
			out.push_back({ static_cast<int>(v), start });
	}
	return out;
}

/* A.4/V.18: LTRS before the first character (here a figure), the shift
   again after 72 characters, and DEL as LTRS */
TEST(CoreV18, FiveBitShifts)
{
	Pair p("");
	for (Modem *m : { p.a, p.b })
		m->dte_input("AT+MS=BAUDOT\r", 13);
	p.step();
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	for (int t = 0; t < 5000 && p.a->link_state() != LinkState::Online;
	     t += 20)
		p.step();
	ASSERT_EQ(p.a->link_state(), LinkState::Online);
	std::string line = "1" + std::string(80, 'E') + "\x7f" + "E";
	p.a->dte_input(line.data(), line.size());
	std::vector<int16_t> tap;
	for (int t = 0; t < 20000; t += 20) {
		p.step();
		tap.insert(tap.end(), p.oa.begin(), p.oa.end());
	}
	auto codes = v18_codes(tap, 45.45f);
	ASSERT_GE(codes.size(), 84u);
	EXPECT_EQ(codes[0].first, 0x1F);
	EXPECT_EQ(codes[1].first, 0x1B);
	EXPECT_EQ(codes[2].first, 0x17);
	int run = 0, longest = 0, es = 0;
	bool del_ltrs = false;
	for (size_t i = 0; i < codes.size(); i++) {
		int c = codes[i].first;
		if (c == 0x1F || c == 0x1B) {
			run = 0;
			/* (DEL: a LTRS between the 80th and the 81st E) */
			if (es == 80)
				del_ltrs = true;
			continue;
		}
		longest = std::max(longest, ++run);
		es += c == 0x01;
	}
	EXPECT_EQ(es, 81);
	EXPECT_LE(longest, 72);
	EXPECT_TRUE(del_ltrs);
}

/* C.1/V.18: EDT's carrier 300 ms before the first character */
TEST(CoreV18, EdtLead)
{
	Pair p("");
	for (Modem *m : { p.a, p.b })
		m->dte_input("AT+MS=EDT\r", 10);
	p.step();
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	for (int t = 0; t < 5000 && p.a->link_state() != LinkState::Online;
	     t += 20)
		p.step();
	ASSERT_EQ(p.a->link_state(), LinkState::Online);
	p.a->dte_input("A", 1);
	std::vector<int16_t> tap;
	for (int t = 0; t < 2000; t += 20) {
		p.step();
		tap.insert(tap.end(), p.oa.begin(), p.oa.end());
	}
	/* the first sample of the carrier, and the start bit's (the
	   first 1180 Hz block) */
	auto blocks = v18_blocks(tap);
	int on = -1, space = -1;
	for (size_t i = 0; i < blocks.size(); i++) {
		if (on < 0 && blocks[i] != 0)
			on = static_cast<int>(i);
		if (on >= 0 && space < 0 && blocks[i] == 1180)
			space = static_cast<int>(i);
	}
	ASSERT_GE(on, 0);
	ASSERT_GT(space, on);
	EXPECT_GE((space - on) * 20, 280);
	EXPECT_LE((space - on) * 20, 340);
}

/* the runs of a block list: (tone, first block, blocks) */
static std::vector<std::array<int, 3>> v18_runs(const std::vector<int> &b)
{
	std::vector<std::array<int, 3>> out;
	for (size_t i = 0; i < b.size(); i++) {
		if (!out.empty() && out.back()[0] == b[i])
			out.back()[2]++;
		else
			out.push_back({ b[i], static_cast<int>(i), 1 });
	}
	return out;
}

/* 5.2.12/V.18: a silent caller.  After Ta (3 s) the probes start with
   ANSam (1 s) and 75 ms of silence; the 5-bit message at 47.6 bit/s
   (A.3); Tm (3 s) of silence after a message; a carrier probe as ANSam,
   75 ms of silence and its carrier for Tc (6 s) */
TEST(CoreV18, Probes)
{
	Pair p("");
	p.b->dte_input("AT+MV18S=1\r", 11);
	p.step();
	EXPECT_EQ(p.b->answer(), 0);
	std::vector<int16_t> tap;
	for (int t = 0; t < 32000; t += 20) {
		p.step();
		tap.insert(tap.end(), p.ob.begin(), p.ob.end());
	}
	auto runs = v18_runs(v18_blocks(tap));
	/* the first signal: ANSam */
	size_t k = 0;
	while (k < runs.size() && runs[k][0] == 0)
		k++;
	ASSERT_LT(k + 2, runs.size());
	EXPECT_EQ(runs[k][0], 2100);
	/* (after the answer's 2 s of silence and Ta) */
	EXPECT_NEAR(runs[k][1] * 20, 5000, 100);
	EXPECT_NEAR(runs[k][2] * 20, 1000, 40);
	/* the silence, to the 5-bit message's mark */
	size_t f = k + 1;
	while (f < runs.size() && runs[f][0] != 1400)
		f++;
	ASSERT_LT(f, runs.size());
	EXPECT_NEAR((runs[f][1] - runs[k][1] - runs[k][2]) * 20, 75, 25);
	/* the message's rate: two characters' start bits 7.5 bits apart */
	size_t from = static_cast<size_t>(runs[f][1]) * 160;
	std::vector<int16_t> msg(tap.begin() + static_cast<long>(from),
				 tap.begin() + static_cast<long>(from) + 8000);
	auto codes = v18_codes(msg, 47.6f);
	ASSERT_GE(codes.size(), 2u);
	EXPECT_NEAR((codes[1].second - codes[0].second) / 8.0,
		    7.5 * 1000 / 47.6, 3);
	/* the silence after the message (its signal 1400/1800 Hz) */
	size_t m = f;
	while (m < runs.size() && runs[m][0] != 0 &&
	       runs[m][0] != 980 && runs[m][0] != 1180)
		m++;
	while (m < runs.size() && runs[m][0] != 0)
		m++;
	ASSERT_LT(m, runs.size());
	EXPECT_GE(runs[m][2] * 20, 2900);
	/* the V.21 probe: ANSam, 75 ms of silence, 1650 Hz for 6 s */
	size_t v = 0;
	for (size_t i = 2; i < runs.size(); i++)
		if (runs[i][0] == 1650 && runs[i][2] > 50) {
			v = i;
			break;
		}
	ASSERT_GT(v, 2u);
	EXPECT_EQ(runs[v - 2][0], 2100);
	EXPECT_NEAR(runs[v - 2][2] * 20, 1000, 40);
	EXPECT_EQ(runs[v - 1][0], 0);
	EXPECT_NEAR(runs[v - 1][2] * 20, 75, 25);
	EXPECT_NEAR(runs[v][2] * 20, 6000, 40);
}

/* 5.2.10/V.18: a calling tone (1300 Hz for 600 ms, once the answerer
   listens after its 2 s of silence) and 1 s of silence: the probes at
   once (their ANSam), not after Ta (at 5 s) */
TEST(CoreV18, CallingToneProbes)
{
	Pair p("");
	p.b->dte_input("AT+MV18S=1\r", 11);
	p.step();
	EXPECT_EQ(p.b->answer(), 0);
	std::vector<int16_t> in(160), out(160), tap;
	double ph = 0;
	for (int t = 0; t < 6000; t += 20) {
		for (int i = 0; i < 160; i++) {
			bool on = t >= 2500 && t < 3100;
			in[i] = on ? static_cast<int16_t>(
				8000 * sin(2 * M_PI * ph)) : 0;
			ph += 1300.0 / 8000;
		}
		p.b->process(in.data(), out.data(), 160);
		now += 20;
		p.b->poll();
		tap.insert(tap.end(), out.begin(), out.end());
	}
	auto runs = v18_runs(v18_blocks(tap));
	size_t k = 0;
	while (k < runs.size() && runs[k][0] == 0)
		k++;
	ASSERT_LT(k, runs.size());
	EXPECT_EQ(runs[k][0], 2100);
	/* (the tone's end at 3100 ms, 1 s of silence) */
	EXPECT_NEAR(runs[k][1] * 20, 4100, 100);
}

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
	EXPECT_EQ(p.got_a, "ERROR\r\nOK\r\nOK\r\nOK\r\n\r\nNO CARRIER\r\n");
	EXPECT_EQ(p.got_b,
		  "+SUBMIT: \"+4412345\"\r\nHello [world]\r\n"
		  "+SUBMIT: \"0800\"\r\n"
		  "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\r\n"
		  "+SUBMIT: \"+12345\"\r\nhellohello\r\n"
		  "\r\nNO CARRIER\r\n");
}

/* the service centre's side: a delivery from a name, stamped now */
TEST_F(CoreSms, Delivers)
{
	call();
	type(p.a, "<Centre,Your code is 1234\r");
	ASSERT_TRUE(until_idle(30000))
		<< "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "OK\r\n\r\nNO CARRIER\r\n");
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
	EXPECT_EQ(p.got_a, "\r\nNO CARRIER\r\n");
	EXPECT_EQ(p.got_b, "\r\nNO CARRIER\r\n");
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

/* the core's log lines while a test wants them */
static std::vector<std::string> *log_kept;

/* a call with V.8 bis (AT+A8E's <v8b>) at a (the caller) and b (the
   answerer), the core's and the pumps' log kept: online at rate or
   better, the log's lines; the call then hung up */
static std::vector<std::string> v8bis_call(const char *a8e_a,
					   const char *a8e_b, unsigned rate)
{
	std::vector<std::string> lines;
	Pair p("");
	log_kept = &lines;
	dcore::log_set_hook([](const char *s) {
		if (log_kept)
			log_kept->push_back(s);
	});
	dcore::log_level = 2;
	dsplibs_debug_level = 2;
	EXPECT_EQ(cmd(p.a, p.la, a8e_a), "\r\nOK\r\n");
	EXPECT_EQ(cmd(p.b, p.lb, a8e_b), "\r\nOK\r\n");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	EXPECT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_GE(p.a->tx_rate(), rate);
	EXPECT_GE(p.b->tx_rate(), rate);
	if (getenv("SHOW"))
		for (const std::string &l : lines)
			printf("%s", l.c_str());
	dcore::log_level = 0;
	dsplibs_debug_level = 0;
	dcore::log_set_hook(nullptr);
	log_kept = nullptr;
	p.b->hangup_now();
	p.run_until([](Pair &q) { return !q.a->started(); }, 15000);
	return lines;
}

/* a line of lines has s in it */
static bool logged(const std::vector<std::string> &lines, const char *s)
{
	for (const std::string &l : lines)
		if (l.find(s) != std::string::npos)
			return true;
	return false;
}

/* V.34's cleardown (11.7/V.34): the caller hangs up in data mode, its
   pump clears the call down, with LAPM (its DISC first, then both
   pumps) and without error correction; the answerer's NO CARRIER comes
   from the cleardown (ATI11: Remote cleardown), both go idle well
   before the carrier's loss would have ended the call */
class V34Cleardown : public testing::TestWithParam<const char *> {};

/* (see V34Cleardown) the call's end, with +MS=34 and the case's link */
TEST_P(V34Cleardown, EndsTheCall)
{
	std::vector<std::string> lines;
	std::string ms = std::string("+MS=34") + GetParam();
	Pair p(ms.c_str());
	log_kept = &lines;
	dcore::log_set_hook([](const char *s) {
		if (log_kept)
			log_kept->push_back(s);
	});
	dcore::log_level = 2;
	dsplibs_debug_level = 2;
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	p.a->dte_input("hello", 5);
	p.run_until([](Pair &q) {
		return q.got_b.find("hello") != std::string::npos; }, 3000);
	p.got_b.clear();
	p.a->hangup();
	bool idle = p.run_until([](Pair &q) {
		return !q.a->started() && !q.b->started(); }, 3000);
	if (getenv("SHOW"))
		for (const std::string &l : lines)
			printf("%s", l.c_str());
	dcore::log_level = 0;
	dsplibs_debug_level = 0;
	dcore::log_set_hook(nullptr);
	log_kept = nullptr;
	EXPECT_TRUE(idle) << "still in the call 3 s after the hangup";
	/* both ends through it: their MP' sequences (with LAPM both
	   start one, the answerer on the caller's DISC) */
	int done = 0;
	for (const std::string &l : lines)
		done += l.find("v34: cleardown done") != std::string::npos;
	EXPECT_TRUE(logged(lines, "v34: cleardown (ours)"));
	EXPECT_EQ(done, 2);
	EXPECT_TRUE(p.got_b.find("NO CARRIER") != std::string::npos)
		<< p.got_b;
	std::string i11 = cmd(p.b, p.lb, "ATI11\r");
	EXPECT_TRUE(i11.find("Remote cleardown") != std::string::npos)
		<< i11;
	EXPECT_TRUE(std::regex_search(i11,
		std::regex("Rate Renegotiation by Remote Modem\\s+0\\b")))
		<< i11;
}

INSTANTIATE_TEST_SUITE_P(Link, V34Cleardown, testing::Values("", "\\N0"),
	[](const testing::TestParamInfo<const char *> &i) {
		return std::string(*i.param ? "plain" : "lapm");
	});

/* +A8E and +DSVD: their values, their ranges, the values they refuse */
TEST(Core, A8e)
{
	Line l;
	Modem *m = make(l, "A");
	EXPECT_EQ(cmd(m, l, "AT+A8E?\r"), "AT+A8E?\r\r\n+A8E: 1,1,0,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+A8E=?\r"), "AT+A8E=?\r\r\n+A8E: (1,6),(1,5),(0),(0,1)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+A8E=1,1,,1\r"), "AT+A8E=1,1,,1\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+A8E?\r"), "AT+A8E?\r\r\n+A8E: 1,1,0,1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+A8E=,,,2\r"), "AT+A8E=,,,2\r\r\nERROR\r\n");
	/* +DSVD, D-Modem's own: DSVD offered in V.8 bis */
	EXPECT_EQ(cmd(m, l, "AT+DSVD?\r"), "AT+DSVD?\r\r\n+DSVD: 0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+DSVD=?\r"), "AT+DSVD=?\r\r\n+DSVD: (0,1)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+DSVD=1\r"), "AT+DSVD=1\r\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+DSVD?\r"), "AT+DSVD?\r\r\n+DSVD: 1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+DSVD=2\r"), "AT+DSVD=2\r\r\nERROR\r\n");
	delete m;
}

/* both with V.8 bis: MRe, CRd, CL, MS, ACK(1) (transaction 10), then
   V.8 */
TEST(Core, V8bisBoth)
{
	std::vector<std::string> l = v8bis_call("AT+A8E=1,1,,1\r",
						"AT+A8E=1,1,,1\r", 24000);
	EXPECT_TRUE(logged(l, "A: pump 108 (driver 108), caller"));
	EXPECT_TRUE(logged(l, "B: pump 108 (driver 108), answerer"));
	EXPECT_TRUE(logged(l, "v8bis: CRd in"));
	EXPECT_TRUE(logged(l, "v8bis: CL in"));
	EXPECT_TRUE(logged(l, "v8bis: MS in"));
	EXPECT_TRUE(logged(l, "v8bis: ACK(1) in"));
	EXPECT_TRUE(logged(l, "V.8 CM received"));
}

/* the answerer alone: its MRe unanswered, V.8 after 3 s */
TEST(Core, V8bisAnswererAlone)
{
	std::vector<std::string> l = v8bis_call("AT+A8E=1,1,,0\r",
						"AT+A8E=1,1,,1\r", 24000);
	EXPECT_FALSE(logged(l, "A: pump 108"));
	EXPECT_TRUE(logged(l, "B: pump 108 (driver 108), answerer"));
	EXPECT_TRUE(logged(l, "v8bis: no answer to MRe"));
	EXPECT_TRUE(logged(l, "V.8 CM received"));
}

/* V.70: both with V.8 bis and +DSVD; V.8 bis chooses DSVD, the link is
   V.76's multiplexer, its data channel (opened by the answerer, V.75's
   OpenLogicalChannel) carries the DTEs' characters both ways */
TEST(Core, DsvdBoth)
{
	std::vector<std::string> lines;
	Pair p("");
	log_kept = &lines;
	dcore::log_set_hook([](const char *s) {
		if (log_kept)
			log_kept->push_back(s);
	});
	dcore::log_level = 2;
	dsplibs_debug_level = 2;
	EXPECT_EQ(cmd(p.a, p.la, "AT+A8E=1,1,,1+DSVD=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(p.b, p.lb, "AT+A8E=1,1,,1+DSVD=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_NE(p.got_a.find("CONNECT"), std::string::npos);
	std::string da = data('a', 3000), db = data('b', 2000);
	size_t sa = 0, sb = 0;
	p.got_a.clear();
	p.got_b.clear();
	for (int t = 0; t < 60000; t += 20) {
		int n = std::min<int>(p.a->dte_space(),
				      static_cast<int>(da.size() - sa));
		sa += p.a->dte_input(da.data() + sa, n);
		n = std::min<int>(p.b->dte_space(),
				  static_cast<int>(db.size() - sb));
		sb += p.b->dte_input(db.data() + sb, n);
		p.step();
		if (p.got_b.size() >= da.size() && p.got_a.size() >= db.size())
			break;
	}
	EXPECT_TRUE(p.got_b == da) << "B got " << p.got_b.size();
	EXPECT_TRUE(p.got_a == db) << "A got " << p.got_a.size();
	/* the answerer hangs up: its DISC closes the channel */
	p.b->hangup();
	p.run_until([](Pair &q) {
		return !q.a->started() && !q.b->started(); }, 15000);
	if (getenv("SHOW"))
		for (const std::string &l : lines)
			printf("%s", l.c_str());
	dcore::log_level = 0;
	dsplibs_debug_level = 0;
	dcore::log_set_hook(nullptr);
	log_kept = nullptr;
	EXPECT_TRUE(logged(lines, "v8bis: DSVD chosen"));
	EXPECT_TRUE(logged(lines, "A: V.8 bis: DSVD (V.70)"));
	EXPECT_TRUE(logged(lines, "B: V.8 bis: DSVD (V.70)"));
	EXPECT_TRUE(logged(lines, "v76: initiator"));
	EXPECT_TRUE(logged(lines, "v76: responder"));
	EXPECT_TRUE(logged(lines, "v76: DLCI 0 in: OpenLogicalChannel, "
			   "channel 1, data, ERM"));
	EXPECT_TRUE(logged(lines, "v76: DLCI 0 in: OpenLogicalChannelAck"));
	EXPECT_TRUE(logged(lines, "v76: DLCI 0 in: CloseLogicalChannel,"));
	EXPECT_TRUE(logged(lines, "A: v76: DLCI 0 out: CloseLogicalChannelAck"));
}

/* the share of the samples' energy at f Hz */
static double tone_share(const std::vector<s16> &x, double f)
{
	double c = 0, sn = 0, all = 0;
	for (size_t i = 0; i < x.size(); i++) {
		c += x[i] * cos(2 * M_PI * f * i / 8000);
		sn += x[i] * sin(2 * M_PI * f * i / 8000);
		all += double(x[i]) * x[i];
	}
	return all > 0 ? 2 * (c * c + sn * sn) / double(x.size()) / all : 0;
}

/* DSVD with a voice path at both ends (Host::dsvd_voice()): the
   initiator opens the voice channel beside the data channel, each end
   hears the other's tone through G.729 Annex A while the data goes;
   the hang-up closes it */
TEST(Core, DsvdVoice)
{
	std::vector<std::string> lines;
	Pair p("");
	log_kept = &lines;
	dcore::log_set_hook([](const char *s) {
		if (log_kept)
			log_kept->push_back(s);
	});
	dcore::log_level = 2;
	p.la.dsvd = p.lb.dsvd = true;
	p.la.tone = 440;
	p.lb.tone = 700;
	EXPECT_EQ(cmd(p.a, p.la, "AT+A8E=1,1,,1+DSVD=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(p.b, p.lb, "AT+A8E=1,1,,1+DSVD=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online &&
			q.la.dsvd_on && q.lb.dsvd_on; }, 40000));
	p.la.ear.clear();
	p.lb.ear.clear();
	std::string da = data('a', 3000);
	size_t sa = 0;
	p.got_b.clear();
	for (int t = 0; t < 2000; t += 20) {
		int n = std::min<int>(p.a->dte_space(),
				      static_cast<int>(da.size() - sa));
		sa += p.a->dte_input(da.data() + sa, n);
		p.step();
	}
	const double ha = tone_share(p.la.ear, 700),
		hb = tone_share(p.lb.ear, 440);
	EXPECT_GT(p.la.ear.size(), 14000u);
	EXPECT_GT(p.lb.ear.size(), 14000u);
	EXPECT_GT(ha, 0.8) << "A hears B's 700 Hz";
	EXPECT_GT(hb, 0.8) << "B hears A's 440 Hz";
	EXPECT_TRUE(p.got_b == da) << "B got " << p.got_b.size();
	p.b->hangup();
	p.run_until([](Pair &q) {
		return !q.a->started() && !q.b->started(); }, 15000);
	if (getenv("SHOW"))
		for (const std::string &l : lines)
			printf("%s", l.c_str());
	dcore::log_level = 0;
	dcore::log_set_hook(nullptr);
	log_kept = nullptr;
	EXPECT_FALSE(p.la.dsvd_on || p.lb.dsvd_on);
	EXPECT_EQ(p.la.dsvd_opened, 1);
	EXPECT_TRUE(logged(lines, "v76: DLCI 1 in: OpenLogicalChannel, "
			   "channel 2, audio, UNERM"));
	EXPECT_TRUE(logged(lines, "A: DSVD: voice channel open"));
	EXPECT_TRUE(logged(lines, "B: DSVD: voice channel closed"));
}

/* +DSVD at the caller alone: V.8 bis chooses data, then V.42 as usual */
TEST(Core, DsvdCallerAlone)
{
	std::vector<std::string> l = v8bis_call("AT+A8E=1,1,,1+DSVD=1\r",
						"AT+A8E=1,1,,1\r", 24000);
	EXPECT_TRUE(logged(l, "v8bis: data chosen"));
	EXPECT_FALSE(logged(l, "DSVD (V.70)"));
	EXPECT_FALSE(logged(l, "v76:"));
	EXPECT_TRUE(logged(l, "ec: connected."));
}

/* V.61: both with V.8 bis and +ASVD, no voice path: V.8 bis chooses
   ASVD, V.61's start-up follows (no V.8), every frame data alone at
   14400 bit/s, V.42 on it */
TEST(Core, AsvdDataAlone)
{
	std::vector<std::string> l = v8bis_call("AT+A8E=1,1,,1+ASVD=1\r",
						"AT+A8E=1,1,,1+ASVD=1\r", 9600);
	EXPECT_TRUE(logged(l, "v8bis: ASVD chosen"));
	EXPECT_TRUE(logged(l, "A: V.8 bis: ASVD (V.61)"));
	EXPECT_TRUE(logged(l, "B: V.8 bis: ASVD (V.61)"));
	EXPECT_TRUE(logged(l, "A: pump 61 (driver 61), caller"));
	EXPECT_TRUE(logged(l, "B: pump 61 (driver 61), answerer"));
	EXPECT_FALSE(logged(l, "V.8 CM received"));
	EXPECT_TRUE(logged(l, "v61: data, data alone at"));
	EXPECT_TRUE(logged(l, "ec: connected."));
}

/* +ASVD at the caller alone: V.8 bis chooses data, then V.8 */
TEST(Core, AsvdCallerAlone)
{
	std::vector<std::string> l = v8bis_call("AT+A8E=1,1,,1+ASVD=1\r",
						"AT+A8E=1,1,,1\r", 24000);
	EXPECT_TRUE(logged(l, "v8bis: data chosen"));
	EXPECT_FALSE(logged(l, "ASVD (V.61)"));
	EXPECT_TRUE(logged(l, "V.8 CM received"));
}

/* both offer DSVD and ASVD: the caller picks DSVD (V.70) */
TEST(Core, AsvdAndDsvdPicksDsvd)
{
	std::vector<std::string> l = v8bis_call(
		"AT+A8E=1,1,,1+DSVD=1+ASVD=1\r",
		"AT+A8E=1,1,,1+DSVD=1+ASVD=1\r", 9600);
	EXPECT_TRUE(logged(l, "v8bis: DSVD chosen"));
	EXPECT_FALSE(logged(l, "ASVD (V.61)"));
	EXPECT_TRUE(logged(l, "v76: initiator"));
}

/* V.61 with a voice path at both ends (Host::dsvd_voice()): speech
   and 4800 bit/s of data on one carrier, each end hearing the other's
   tone while V.42's data goes; the hang-up closes the voice path */
TEST(Core, AsvdVoice)
{
	std::vector<std::string> lines;
	Pair p("");
	log_kept = &lines;
	dcore::log_set_hook([](const char *s) {
		if (log_kept)
			log_kept->push_back(s);
	});
	dcore::log_level = 2;
	dsplibs_debug_level = 2;
	p.la.dsvd = p.lb.dsvd = true;
	p.la.tone = 440;
	p.lb.tone = 700;
	EXPECT_EQ(cmd(p.a, p.la, "AT+A8E=1,1,,1+ASVD=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(p.b, p.lb, "AT+A8E=1,1,,1+ASVD=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online &&
			q.la.dsvd_on && q.lb.dsvd_on; }, 40000));
	EXPECT_EQ(p.a->tx_rate(), 4800u);
	p.la.ear.clear();
	p.lb.ear.clear();
	std::string da = data('a', 1500), db = data('b', 1000);
	size_t sa = 0, sb = 0;
	p.got_a.clear();
	p.got_b.clear();
	for (int t = 0; t < 4000; t += 20) {
		int n = std::min<int>(p.a->dte_space(),
				      static_cast<int>(da.size() - sa));
		sa += p.a->dte_input(da.data() + sa, n);
		n = std::min<int>(p.b->dte_space(),
				  static_cast<int>(db.size() - sb));
		sb += p.b->dte_input(db.data() + sb, n);
		p.step();
	}
	const double ha = tone_share(p.la.ear, 700),
		hb = tone_share(p.lb.ear, 440);
	EXPECT_GT(p.la.ear.size(), 28000u);
	EXPECT_GT(p.lb.ear.size(), 28000u);
	EXPECT_GT(ha, 0.8) << "A hears B's 700 Hz";
	EXPECT_GT(hb, 0.8) << "B hears A's 440 Hz";
	EXPECT_TRUE(p.got_b == da) << "B got " << p.got_b.size();
	EXPECT_TRUE(p.got_a == db) << "A got " << p.got_a.size();
	p.b->hangup();
	p.run_until([](Pair &q) {
		return !q.a->started() && !q.b->started(); }, 15000);
	if (getenv("SHOW"))
		for (const std::string &l : lines)
			printf("%s", l.c_str());
	dcore::log_level = 0;
	dsplibs_debug_level = 0;
	dcore::log_set_hook(nullptr);
	log_kept = nullptr;
	EXPECT_FALSE(p.la.dsvd_on || p.lb.dsvd_on);
	EXPECT_EQ(p.la.dsvd_opened, 1);
	EXPECT_TRUE(logged(lines, "A: V.61: voice channel open"));
	EXPECT_TRUE(logged(lines, "B: V.61: voice channel closed"));
	EXPECT_TRUE(logged(lines, "v61: voice plus data"));
}

/* the caller alone: no V.8 bis at all */
TEST(Core, V8bisCallerAlone)
{
	std::vector<std::string> l = v8bis_call("AT+A8E=1,1,,1\r",
						"AT+A8E=1,1,,0\r", 24000);
	EXPECT_FALSE(logged(l, "pump 108"));
	EXPECT_TRUE(logged(l, "V.8 CM received"));
}

/* the pump table (dcore.h): each id once; V.59's high-level objects each
   named by one pump, which has V.59's name; the codes the switches it
   replaced gave */
TEST(Core, PumpTable)
{
	using dcore::DpDriver;
	using dcore::dp_driver;
	std::vector<int> ids;
	for (const DpDriver *d = dcore::dp_drivers(); d->id; d++) {
		EXPECT_EQ(std::count(ids.begin(), ids.end(), d->id), 0) << d->id;
		ids.push_back(d->id);
		EXPECT_EQ(dp_driver(d->id), d);
		if (d->flags & dcore::DPF_LOOP3) {
			EXPECT_TRUE(d->flags & dcore::DPF_LEASED) << d->id;
		}
		if (!(d->flags & dcore::DPF_V59_OBJECT))
			continue;
		EXPECT_NE(d->v59_high, 0) << d->id;
		EXPECT_NE(d->v59_name, nullptr) << d->id;
		for (const DpDriver *e = d + 1; e->id; e++) {
			if (e->flags & dcore::DPF_V59_OBJECT) {
				EXPECT_NE(e->v59_high, d->v59_high) << e->id;
			}
		}
	}
	EXPECT_EQ(dp_driver(0), nullptr);
	EXPECT_EQ(dp_driver(9999), nullptr);
	EXPECT_EQ(dp_driver(DP_V32)->v59_high, dp_driver(DP_V32BIS)->v59_high);
	EXPECT_FALSE(dp_driver(DP_V32)->flags & dcore::DPF_V59_OBJECT);
	EXPECT_EQ(dp_driver(DP_V90)->v58, 0xC);
	EXPECT_EQ(dp_driver(DP_V90)->v58_pcm, 0xE);
	EXPECT_EQ(dp_driver(DP_V32TERBO)->v58, dp_driver(DP_V32BIS)->v58);
	EXPECT_STREQ(dp_driver(DP_V29FP)->v59_name, "V.29FastPOS");
	EXPECT_TRUE(dp_driver(DP_B202)->flags & dcore::DPF_LEASED);
	EXPECT_FALSE(dp_driver(DP_B202)->flags & dcore::DPF_LOOP3);
	EXPECT_FALSE(dp_driver(DP_V91)->flags & dcore::DPF_LEASED);
}

/* AT!M: the settings model for programs, a line of tab-separated
   fields for each setting and each of its choices, in any state */
TEST(Core, ModelListing)
{
	Line l;
	Modem *m = make(l, "model");
	cmd(m, l, "ATE0S0=2+GCI=B4\r");
	std::string r = cmd(m, l, "AT!M\r");
	ASSERT_GE(r.size(), 4u);
	EXPECT_EQ(r.substr(r.size() - 4), "OK\r\n");
	std::vector<std::vector<std::string>> rows;
	size_t from = 0;
	while (from < r.size()) {
		size_t e = r.find("\r\n", from);
		std::string line = r.substr(from, e - from);
		from = e + 2;
		if (line.empty() || line == "OK")
			continue;
		std::vector<std::string> f;
		size_t a = 0, t;
		while ((t = line.find('\t', a)) != std::string::npos) {
			f.push_back(line.substr(a, t - a));
			a = t + 1;
		}
		f.push_back(line.substr(a));
		rows.push_back(f);
	}
	ASSERT_FALSE(rows.empty());
	ASSERT_EQ(rows[0].size(), 3u);
	EXPECT_EQ(rows[0][0], "model");
	EXPECT_EQ(rows[0][1], "1");
	size_t settings = 0;
	bool country = false, echo = false, s0 = false;
	for (const auto &f : rows) {
		if (f[0] == "setting") {
			ASSERT_EQ(f.size(), 11u) << f[1];
			settings++;
			if (f[1] == "country") {
				country = true;
				EXPECT_EQ(f[2], "pick");
				EXPECT_EQ(f[3], "B4");
				EXPECT_EQ(f[4], "B5");
				EXPECT_EQ(f[7], "+GCI=%s");
			}
			if (f[1] == "echo") {
				echo = true;
				EXPECT_EQ(f[3], "0");
				EXPECT_EQ(f[4], "1");
			}
			if (f[2] == "number") {
				EXPECT_FALSE(f[5].empty());
				EXPECT_FALSE(f[6].empty());
			}
			if (f[1] == "auto-answer") {
				s0 = true;
				EXPECT_EQ(f[3], "2");
			}
		} else if (f[0] != "model") {
			EXPECT_EQ(f[0], "choice");
			EXPECT_EQ(f.size(), 4u);
		}
	}
	EXPECT_TRUE(country);
	EXPECT_TRUE(echo);
	EXPECT_TRUE(s0);
	EXPECT_EQ(rows[0][2], std::to_string(settings));
	/* in any state: off hook */
	cmd(m, l, "ATH1\r");
	EXPECT_EQ(cmd(m, l, "AT!M\r").substr(0, 8), "\r\nmodel\t");
	/* nothing after M */
	EXPECT_EQ(cmd(m, l, "ATH0!MX\r").find("ERROR"), std::string::npos)
		<< "!M takes the rest of the line as further commands";
}

/* AT!R: the init string of what differs from the defaults, in any
   state; AT followed by nothing when nothing does */
TEST(Core, InitString)
{
	Line l;
	Modem *m = make(l, "r");
	EXPECT_EQ(cmd(m, l, "AT!R\r"), "AT!R\r\r\nAT\r\n\r\nOK\r\n");
	cmd(m, l, "ATS0=2L1\\N0+MS=34,1,1200,28800+GCI=B4\r");
	EXPECT_EQ(cmd(m, l, "AT!R\r"),
		  "AT!R\r\r\nAT+GCI=B4;+MS=34,1,1200,28800;\\N0L1S0=2+ES=1,0,1\r\n\r\nOK\r\n");
	/* the string reproduces the settings on a modem at its defaults */
	Line l2;
	Modem *m2 = make(l2, "r2");
	cmd(m2, l2, "AT+GCI=B4;+MS=34,1,1200,28800;\\N0L1S0=2+ES=1,0,1\r");
	EXPECT_EQ(cmd(m2, l2, "AT!R\r"), cmd(m, l, "AT!R\r"));
	/* in any state: off hook */
	cmd(m, l, "ATE0H1\r");
	EXPECT_EQ(cmd(m, l, "AT!R\r"),
		  "\r\nAT+GCI=B4;+MS=34,1,1200,28800;\\N0E0L1S0=2+ES=1,0,1\r\n\r\nOK\r\n");
	/* a long one in lines of 63 characters at most */
	cmd(m, l, "ATH0S6=9S7=99S9=9S10=99S11=99S12=99S24=99S18=9S92=9X3V0Q0&C1&H3\r");
	std::string r = cmd(m, l, "AT!R\r");
	EXPECT_NE(r.find("\r\nAT"), std::string::npos);
	size_t from = 0, lines = 0;
	while (from < r.size()) {
		size_t e = r.find("\r\n", from);
		/* (ATV0: the result code ends with S3 alone) */
		if (e == std::string::npos)
			e = r.size();
		EXPECT_LE(e - from, 63u) << r;
		from = e + 2;
		lines++;
	}
	EXPECT_GE(lines, 3u) << r;
	delete m2;
	delete m;
}

/* AT!CT: the command line configurator; settings take effect at once;
   exit ends it with OK; only with the modem idle and on hook */
TEST(Core, ConfigCli)
{
	Line l;
	Modem *m = make(l, "ct");
	std::string s = cmd(m, l, "AT!CT\r");
	EXPECT_NE(s.find("dmodem#"), std::string::npos) << s;
	s = cmd(m, l, "sh run\r");
	EXPECT_NE(s.find("Current configuration:"), std::string::npos) << s;
	EXPECT_EQ(s.find("echo"), std::string::npos) << s;
	s = cmd(m, l, "conf t\r");
	EXPECT_NE(s.find("dmodem(config)#"), std::string::npos) << s;
	s = cmd(m, l, "echo off\r");
	EXPECT_EQ(m->sreg(SREG_ECHO), 0);
	cmd(m, l, "max-rate 33600\r");
	cmd(m, l, "modulation v34\r");
	cmd(m, l, "auto-answer 3\r");
	EXPECT_EQ(m->sreg(SREG_RINGS_TO_AUTO_ANSWER), 3);
	/* errors as IOS gives them */
	s = cmd(m, l, "echo maybe\r");
	EXPECT_NE(s.find("% Invalid input detected at '^' marker."),
		  std::string::npos) << s;
	EXPECT_NE(s.find("\n" + std::string(20, ' ') + "^"), std::string::npos)
		<< s;
	s = cmd(m, l, "c\r");
	EXPECT_NE(s.find("% Ambiguous command:  \"c\""), std::string::npos)
		<< s;
	s = cmd(m, l, "echo\r");
	EXPECT_NE(s.find("% Incomplete command."), std::string::npos) << s;
	s = cmd(m, l, "max-rate 99999\r");
	EXPECT_NE(s.find("% Invalid input"), std::string::npos) << s;
	/* help and completion */
	s = cmd(m, l, "echo ?");
	EXPECT_NE(s.find("off"), std::string::npos) << s;
	EXPECT_NE(s.find("on"), std::string::npos) << s;
	cmd(m, l, "\x15");
	s = cmd(m, l, "spea\t");
	EXPECT_NE(s.find("speaker "), std::string::npos) << s;
	cmd(m, l, "\x15");
	/* no: off, default: the default */
	cmd(m, l, "no echo\r");
	EXPECT_EQ(m->sreg(SREG_ECHO), 0);
	cmd(m, l, "default echo\r");
	EXPECT_EQ(m->sreg(SREG_ECHO), 1);
	cmd(m, l, "no echo\r");
	s = cmd(m, l, "do show init-string\r");
	EXPECT_NE(s.find("AT+MS=34,1,300,33600;E0S0=3"), std::string::npos)
		<< s;
	s = cmd(m, l, "\x1a");
	EXPECT_NE(s.find("dmodem#"), std::string::npos) << s;
	s = cmd(m, l, "exit\r");
	EXPECT_NE(s.find("OK\r\n"), std::string::npos) << s;
	/* back to AT commands */
	EXPECT_EQ(cmd(m, l, "ATS0?\r"), "\r\n003\r\n\r\nOK\r\n");
	/* the rest of the line after AT!CT is not read; what comes after
	   exit is a command */
	s = cmd(m, l, "AT!CT\rexit\rATS0?\r");
	EXPECT_NE(s.find("OK\r\n\r\n003\r\n\r\nOK"), std::string::npos) << s;
	/* off hook: ERROR (AT!R still works) */
	cmd(m, l, "ATV1H1\r");
	EXPECT_EQ(cmd(m, l, "AT!CT\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT!C\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT!CF\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT!CX\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT!R\r").substr(0, 2), "\r\n");
	cmd(m, l, "ATH0\r");
	/* a RING ends it, with no OK */
	cmd(m, l, "AT!CT\r");
	l.out.clear();
	m->ring_signal(true);
	EXPECT_NE(l.out.find("RING"), std::string::npos) << l.out;
	EXPECT_EQ(l.out.find("OK"), std::string::npos) << l.out;
	m->ring_signal(false);
	EXPECT_EQ(cmd(m, l, "AT\r"), "\r\nOK\r\n");
	delete m;
}

/* AT!C: a terminal that answers the cursor position report gets the
   full screen (its size and UTF-8 asked), one that does not the command
   line */
TEST(Core, ConfigAuto)
{
	Line l;
	Modem *m = make(l, "c");
	std::string s = cmd(m, l, "AT!C\r");
	EXPECT_NE(s.find("\x1b[6n"), std::string::npos) << s;
	l.out.clear();
	now += 2000;
	m->poll();
	EXPECT_NE(l.out.find("dmodem#"), std::string::npos) << l.out;
	cmd(m, l, "exit\r");

	s = cmd(m, l, "AT!C\r");
	s = cmd(m, l, "\x1b[5;1R");
	EXPECT_NE(s.find("\x1b[999;999H\x1b[6n"), std::string::npos) << s;
	s = cmd(m, l, "\x1b[30;100R");
	EXPECT_NE(s.find("\r\xc3\xa9\x1b[6n"), std::string::npos) << s;
	s = cmd(m, l, "\x1b[30;2R");
	EXPECT_NE(s.find("D-Modem Setup"), std::string::npos) << s;
	EXPECT_NE(s.find("\xe2\x95\x94"), std::string::npos) << s;  /* ╔ */
	EXPECT_NE(s.find("\x1b[30;1H"), std::string::npos) << s;
	/* the Terminal group (the fourth): echo off, F2 saves it */
	cmd(m, l, "\t\x1b[B\x1b[B\x1b[B\t");
	EXPECT_EQ(m->sreg(SREG_ECHO), 1);
	cmd(m, l, "\x1b[C");    /* the next value */
	EXPECT_EQ(m->sreg(SREG_ECHO), 1);
	cmd(m, l, "\x1bOQ");    /* F2 */
	EXPECT_EQ(m->sreg(SREG_ECHO), 0);
	cmd(m, l, "\r");        /* the message box */
	/* a change not saved, then Esc: the question, No */
	cmd(m, l, "\x1b[C");
	s = cmd(m, l, "\x1b");
	EXPECT_EQ(s, "");
	now += 200;
	l.out.clear();
	m->poll();
	EXPECT_NE(l.out.find("Save the changes"), std::string::npos);
	s = cmd(m, l, "n");
	EXPECT_NE(s.find("OK\r\n"), std::string::npos) << s;
	EXPECT_EQ(m->sreg(SREG_ECHO), 0);

	/* AT!CF on a terminal in CP437; Alt-X with nothing changed leaves */
	cmd(m, l, "AT!CF\r");
	cmd(m, l, "\x1b[1;1R\x1b[24;80R");
	s = cmd(m, l, "\x1b[24;3R");
	EXPECT_NE(s.find("\xc9"), std::string::npos);   /* ╔ */
	EXPECT_EQ(s.find("\xe2\x95\x94"), std::string::npos);
	s = cmd(m, l, "\x1bx");
	EXPECT_NE(s.find("OK"), std::string::npos) << s;
	delete m;
}

/*
 *    ETC (S116, AT&F5, AT&F6, AT+EF; core/etc.h)
 */

/* the default set's response as the firmware's taps give it at 8000 Hz:
   a tilt down from +5.1 dB at 300 Hz to -6.0 dB at 3600 Hz; the filter
   at a gain of 1 at 1800 Hz, the tilt the same */
TEST(Core, EtcFilter)
{
	auto db = [](double g) { return 20 * log10(g); };
	EXPECT_NEAR(db(dcore::etc_gain(5, 300, 8000)), 5.1, 0.1);
	EXPECT_NEAR(db(dcore::etc_gain(5, 1800, 8000)), 1.6, 0.1);
	EXPECT_NEAR(db(dcore::etc_gain(5, 3600, 8000)), -6.0, 0.1);
	EXPECT_NEAR(db(dcore::etc_gain(0, 1000, 8000)), 0, 0.01);
	for (int set = 1; set <= 6; set++)
		EXPECT_GT(dcore::etc_gain(set, 300, 8000),
			  dcore::etc_gain(set, 3300, 8000)) << set;
	/* a sine's level through the filter */
	auto level = [](int set, double f) {
		dcore::EtcFilter e;
		e.start(set, 8000);
		std::vector<int16_t> s(8000);
		for (size_t i = 0; i < s.size(); i++)
			s[i] = static_cast<int16_t>(
				lround(8000 * sin(2 * M_PI * f * i / 8000)));
		e.process(s.data(), static_cast<int>(s.size()));
		double sq = 0;
		for (size_t i = 100; i < s.size(); i++)
			sq += static_cast<double>(s[i]) * s[i];
		return 10 * log10(sq / (s.size() - 100) / (8000.0 * 8000 / 2));
	};
	EXPECT_NEAR(level(5, 1800), 0, 0.1);
	EXPECT_NEAR(level(5, 300), 5.1 - 1.6, 0.2);
	EXPECT_NEAR(level(5, 3300), -5.1 - 1.6, 0.2);
	EXPECT_NEAR(level(0, 700), 0, 0.1);
	/* a set out of range is the default */
	EXPECT_NEAR(level(99, 300), level(5, 300), 0.01);
}

/* &F5 and &F6 set ETC's flags and the settings of 3Com's profiles; +EF
   takes 0 to 10 */
TEST(Core, EtcSettings)
{
	Line l;
	Modem *m = make(l, "A");
	EXPECT_EQ(cmd(m, l, "ATE0\r"), "ATE0\r\r\nOK\r\n");
	EXPECT_EQ(m->sreg(SREG_ETC), 0);
	EXPECT_EQ(cmd(m, l, "AT&F5\r"), "\r\nOK\r\n");
	EXPECT_EQ(m->sreg(SREG_ETC), 0xEE);
	EXPECT_EQ(m->sreg(SREG_DP), DP_V32BIS);
	EXPECT_EQ(m->sreg(SREG_EC), 2);
	EXPECT_EQ(m->sreg(SREG_CARRIER_LOSS_DISCONNECT_TIME), 40);
	EXPECT_EQ(m->sreg(SREG_WAIT_CARRIER_AFTER_DIAL), 90);
	EXPECT_EQ(cmd(m, l, "AT+ES?\r"), "\r\n+ES: 3,2,4\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT&F6\r"), "\r\nOK\r\n");
	EXPECT_EQ(m->sreg(SREG_ETC), 0xB0);
	EXPECT_EQ(m->sreg(SREG_EC), 3);
	EXPECT_EQ(cmd(m, l, "ATS116=0\r"), "\r\nOK\r\n");
	EXPECT_EQ(m->sreg(SREG_ETC), 0);
	EXPECT_EQ(cmd(m, l, "AT+EF?\r"), "\r\n+EF: 5\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+EF=?\r"), "\r\n+EF: (0-10)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+EF=2\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+EF?\r"), "\r\n+EF: 2\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+EF=11\r"), "\r\nERROR\r\n");
	delete m;
}

/* a cellular side (&F5) calls a landline side (&F6): the caller's 1500
   Hz calling tone, which the landline side finds; V.32bis with V.42,
   the filter on both transmitters, /ETC in both CONNECTs, 0x82 and
   frames of 64 octets in the caller's #UD; the data both ways */
TEST(Core, EtcCall)
{
	Pair p("\\V1");
	cmd(p.a, p.la, "AT&F5\r");
	cmd(p.b, p.lb, "AT&F6\r");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_LE(p.a->tx_rate(), 14400u);
	EXPECT_GE(p.a->tx_rate(), 4800u);
	EXPECT_TRUE(std::regex_search(p.got_a,
		std::regex("CONNECT [0-9]+/LAPM/ETC"))) << p.got_a;
	EXPECT_TRUE(std::regex_search(p.got_b,
		std::regex("CONNECT [0-9]+/LAPM/ETC"))) << p.got_b;
	std::string da = data('a', 8000), db = data('b', 6000);
	size_t sa = 0, sb = 0;
	p.got_a.clear();
	p.got_b.clear();
	for (int t = 0; t < 60000; t += 20) {
		int n = std::min<int>(p.a->dte_space(), da.size() - sa);
		sa += p.a->dte_input(da.data() + sa, n);
		n = std::min<int>(p.b->dte_space(), db.size() - sb);
		sb += p.b->dte_input(db.data() + sb, n);
		p.step();
		if (p.got_b.size() >= da.size() && p.got_a.size() >= db.size())
			break;
	}
	EXPECT_TRUE(p.got_b == da) << "B got " << p.got_b.size();
	EXPECT_TRUE(p.got_a == db) << "A got " << p.got_a.size();
	p.run_until([](Pair &) { return false; }, 1100);
	p.got_a.clear();
	p.a->dte_input("+++", 3);
	p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos; }, 2000);
	std::string ud = cmd(p.a, p.la, "AT#UD\r");
	EXPECT_NE(ud.find(" 40=82"), std::string::npos) << ud;
	/* V.42's N401: 64 octets, as ETC presets it */
	EXPECT_NE(ud.find(" 41=40"), std::string::npos) << ud;
}

/* the landline side's detector: 1500 Hz found (at -10 and -40 dBm0, and
   under noise 10 dB down), not 1300 or 1800 Hz, nor silence or noise,
   nor 1500 Hz for 30 ms (one block) */
TEST(Core, EtcToneDetector)
{
	auto found = [](double f, double dbm0, int ms, double noise_db) {
		dcore::EtcToneDetector d;
		d.start(8000);
		const double a = sqrt(2 * 2.49e8 * pow(10, dbm0 / 10));
		const double na = noise_db > -100 ?
			sqrt(2.49e8 * pow(10, noise_db / 10)) : 0;
		std::vector<int16_t> s(8000);
		unsigned x = 1;
		for (size_t i = 0; i < s.size(); i++) {
			x = x * 1103515245 + 12345;
			const double nz = na * (((x >> 16) & 0x7fff) / 16384.0 -
						1) * sqrt(3);
			const double t = static_cast<int>(i) < ms * 8 ?
				a * sin(2 * M_PI * f * i / 8000) : 0;
			s[i] = static_cast<int16_t>(lround(t + nz));
		}
		return d.process(s.data(), static_cast<int>(s.size()));
	};
	EXPECT_TRUE(found(1500, -10, 700, -200));
	EXPECT_TRUE(found(1500, -40, 700, -200));
	EXPECT_TRUE(found(1500, -20, 700, -30));
	EXPECT_FALSE(found(1300, -10, 700, -200));
	EXPECT_FALSE(found(1800, -10, 700, -200));
	EXPECT_FALSE(found(1500, -60, 700, -200));
	EXPECT_FALSE(found(1500, -10, 0, -20));
	EXPECT_FALSE(found(1500, -10, 30, -200));
}

/* a caller without ETC: the landline side (&F6) finds no 1500 Hz, so no
   /ETC */
TEST(Core, EtcLandPlainCaller)
{
	Pair p("\\V1+MS=V32B");
	cmd(p.b, p.lb, "AT&F6\r");
	EXPECT_EQ(p.b->answer(), 0);
	EXPECT_EQ(p.a->dial(""), 0);
	ASSERT_TRUE(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	EXPECT_TRUE(p.got_b.find("CONNECT") != std::string::npos &&
		    p.got_b.find("/ETC") == std::string::npos) << p.got_b;
	EXPECT_TRUE(p.got_a.find("/ETC") == std::string::npos) << p.got_a;
}

/* ZyXEL's type 2 security by itself (zyxel_sec.h): the firmware's
   limits, 8 characters a password and 26 a number, 3 tries */
TEST(Core, ZyxelSecurity)
{
	using S = dcore::ZyxelSecurity;
	auto feed = [](S &s, const std::string &t) {
		for (char c : t)
			s.feed(static_cast<u8>(c), '\r', '\b');
	};
	S s;
	s.type = 3;
	s.user[7] = "12345678";
	ASSERT_TRUE(s.on());
	s.start();
	EXPECT_EQ(s.out, "PassWord:");
	s.out.clear();
	/* a 9th character is wrong at once (and goes) */
	feed(s, "123456789");
	EXPECT_EQ(s.out, "********\r\nError..., try again\r\nPassWord:");
	EXPECT_EQ(s.step, S::Step::Password);
	s.out.clear();
	feed(s, "12345678\r");
	EXPECT_EQ(s.out, "********\r\nCorrect\r\n");
	EXPECT_EQ(s.end, S::End::Pass);
	EXPECT_EQ(s.slot, 7);
	EXPECT_TRUE(s.holds());
	s.stop();
	EXPECT_FALSE(s.holds());

	/* an empty slot never matches; the third wrong one ends it */
	s.start();
	feed(s, "\r\r");
	EXPECT_EQ(s.step, S::Step::Password);
	feed(s, "\r");
	EXPECT_EQ(s.end, S::End::Fail);
	EXPECT_EQ(s.out, "PassWord:\r\nError..., try again\r\nPassWord:"
		  "\r\nError..., try again\r\nPassWord:");

	/* *G5: S5 erases (not past the start), 26 characters at most */
	s.type = 5;
	s.start();
	feed(s, "12345678\r");
	EXPECT_EQ(s.step, S::Step::Number);
	s.out.clear();
	feed(s, "\b12\b3\r");
	EXPECT_EQ(s.out, "12\b3");
	EXPECT_EQ(s.end, S::End::CallNumber);
	EXPECT_EQ(s.number, "13");
	s.start();
	feed(s, "12345678\r" + std::string(26, '5'));
	EXPECT_EQ(s.step, S::Step::Number);
	feed(s, "5");
	EXPECT_EQ(s.step, S::Step::Password);

	/* *G4: the stored number's slot */
	s.type = 4;
	s.start();
	feed(s, "12345678\r");
	EXPECT_EQ(s.end, S::End::CallStored);
	EXPECT_EQ(s.slot, 7);
}

/* ZyXEL's fax mode (class 6): a page as its DTE gives it, MH with an
   RTC, the first bit of an octet its lowest, nothing shielded */
static std::string zfax_data(const dsp::T4Page &pg)
{
	dsp::T4Bits b;
	std::string d;
	pg.encode_mh(&b, 0, 0, 1);
	for (size_t k = 0; k < (b.n + 7) / 8; k++)
		d += (char)reverse_bits(b.buf[k]);
	return d;
}

/* the whole pages (each to its RTC) in class 6 data */
static std::vector<dsp::T4Page> zfax_pages(const std::string &d)
{
	std::vector<dsp::T4Page> pages;
	dsp::T4Page pg;
	dsp::T4Rx rx;
	pg.init(T4_WIDTH, 1);
	rx.init(&pg, 0);
	for (unsigned char c : d)
		for (int k = 0; k < 8; k++)
			if (rx.bit((c >> k) & 1)) {
				rx.release();
				pages.push_back(pg);
				pg = dsp::T4Page();
				pg.init(T4_WIDTH, 1);
				rx.init(&pg, 0);
			}
	rx.release();
	return pages;
}

/* class 6 with #B0 or #B1 (the parameter): A sends B two pages.  B
   answers as a data modem first and finds the fax caller.  A has
   CONNECT FAX/... with B's #P, sends the pages and has DISCONNECT0; B
   has CONNECT FAX/... with A's #P and the pages (#B1: CONNECT FAX and
   ZyXEL, then the parameters and the first page for a DC2, the second
   page for another), then DISCONNECT0 */
class CoreZfax : public testing::TestWithParam<int> {};

/* (see above) */
TEST_P(CoreZfax, TwoPages)
{
	const int zb = GetParam();
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
	const std::string id_a = "5551111" + std::string(13, ' ');
	const std::string id_b = "5552222" + std::string(13, ' ');

	p.a->dte_input("AT+FCLASS=6#V1#P5551111\r", 24);
	std::string sb = "AT+FCLASS=6#B" + std::to_string(zb) + "#P5552222\r";
	p.b->dte_input(sb.data(), sb.size());
	p.step();
	EXPECT_EQ(p.got_a, "\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "\r\nOK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	p.a->dte_input("AT+FCLASS?;+FCLASS=?\r", 21);
	p.step();
	EXPECT_EQ(p.got_a, "\r\n6\r\n0,1,1.0,2,2.0,6,8\r\n\r\nOK\r\n");
	p.got_a.clear();

	p.b->dte_input("ATA\r", 4);
	p.a->dte_input("ATD\r", 4);
	ASSERT_TRUE(until([&] { return has(p.got_a, id_b + "\r\n"); }, 60000))
		<< "A [" << p.got_a << "] B [" << p.got_b << "]";
	EXPECT_EQ(p.got_a, "CONNECT FAX/S1440V1T0R0L2C7P" + id_b + "\r\n");
	p.got_a.clear();

	std::vector<dsp::T4Page> sent;
	std::string d;
	for (int page = 0; page < 2; page++) {
		sent.push_back(fax2_page(300 + 50 * page, page * 333));
		d += zfax_data(sent.back());
	}
	size_t n_sent = 0;
	while (n_sent < d.size()) {
		int n = p.a->dte_space();
		if (n > (int)(d.size() - n_sent))
			n = d.size() - n_sent;
		n_sent += p.a->dte_input(d.data() + n_sent, n);
		p.step();
	}

	size_t data_at = std::string::npos;
	if (zb) {
		ASSERT_TRUE(until([&] { return has(p.got_b, "ZyXEL\r\n"); },
				  30000)) << "B [" << p.got_b << "]";
		EXPECT_EQ(p.got_b, "CONNECT FAX\r\nZyXEL\r\n");
		/* nothing more without a DC2 */
		until([] { return false; }, 3000);
		EXPECT_EQ(p.got_b, "CONNECT FAX\r\nZyXEL\r\n");
		p.b->dte_input("\x12", 1);
		ASSERT_TRUE(until([&] {
			return zfax_pages(p.got_b).size() >= 1;
		}, 30000)) << "B [" << p.got_b << "]";
		const std::string params = "\r\n/S1440V1T0R0L2C7P" +
			id_a.substr(0, 18) + "\r\n";
		EXPECT_EQ(p.got_b.substr(20, params.size()), params);
		data_at = 20 + params.size();
		/* the second page waits for its DC2 */
		until([] { return false; }, 2000);
		EXPECT_EQ(zfax_pages(p.got_b.substr(data_at)).size(), 1u);
		p.b->dte_input("\x12", 1);
	} else {
		const std::string c = "CONNECT FAX/S1440V1T0R0L2C7P" + id_a +
			"\r\n";
		ASSERT_TRUE(until([&] { return has(p.got_b, c); }, 30000))
			<< "B [" << p.got_b << "]";
		EXPECT_EQ(p.got_b.find(c), 0u);
		data_at = c.size();
	}
	ASSERT_TRUE(until([&] { return has(p.got_b, "DISCONNECT"); }, 60000))
		<< "B [" << p.got_b << "]";
	size_t end = p.got_b.find("DISCONNECT");
	std::vector<dsp::T4Page> got =
		zfax_pages(p.got_b.substr(data_at, end - data_at));
	ASSERT_EQ(got.size(), 2u);
	for (int page = 0; page < 2; page++) {
		EXPECT_EQ(got[page].rows, sent[page].rows) << page;
		EXPECT_TRUE(got[page].bits == sent[page].bits) << page;
	}
	ASSERT_TRUE(until([&] { return has(p.got_b, id_a + "\r\n"); }, 5000));
	EXPECT_EQ(p.got_b.substr(end), "DISCONNECT0P" + id_a + "\r\n");
	ASSERT_TRUE(until([&] { return has(p.got_a, id_b + "\r\n"); }, 30000))
		<< "A [" << p.got_a << "]";
	EXPECT_EQ(p.got_a, "DISCONNECT0P" + id_b + "\r\n");
	EXPECT_TRUE(until([&] {
		return !p.a->started() && !p.b->started();
	}, 5000));
	p.got_a.clear();
	p.a->dte_input("AT+FCLASS?\r", 11);
	p.step();
	EXPECT_EQ(p.got_a, "\r\n6\r\n\r\nOK\r\n");
}

INSTANTIATE_TEST_SUITE_P(Zb, CoreZfax, testing::Values(0, 1),
			 [](const testing::TestParamInfo<int> &i) {
				 return "B" + std::to_string(i.param);
			 });

/* class 6's commands: the # settings, ERROR for #V2, #C8, a #P of 26
   characters; #B2 taken; &N32 taken */
TEST(Core, ZfaxCommands)
{
	Pair p("");
	auto cmd = [&p](const std::string &s) {
		p.got_a.clear();
		p.a->dte_input(s.data(), s.size());
		p.step();
		return p.got_a;
	};
	EXPECT_EQ(cmd("AT#F#B1#V0#T1#R2#L1#C5&N32\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd("AT#B2\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd("AT#B4\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd("AT#V2\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd("AT#C8\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd("AT#P" + std::string(25, '1') + "\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd("AT#P" + std::string(26, '1') + "\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd("AT#CID=1\r"), "\r\nOK\r\n");
}

/* ZyXEL's encryption: ATCK needs '<' (the key to '>' or the line's end,
   the commands after '>' going on); S102 takes its bits */
TEST(Core, ZyxelAtck)
{
	Pair p("");
	auto cmd = [&p](const std::string &s) {
		p.got_a.clear();
		p.a->dte_input(s.data(), s.size());
		p.step();
		return p.got_a;
	};
	EXPECT_EQ(cmd("ATCKsecret\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd("ATCK<secret>S102=7\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd("ATS102?\r"), "\r\n007\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd("ATCK<to the end\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd("ATC1\r"), "\r\nOK\r\n");
}

/* #B3, the ring cadence meter: its two lines; a burst of ring pulses
   (every 60 ms for 2 s), 4 s with none, another burst: a line at each
   change, the time of what ended in 10 ms steps; no RING; a key stops
   it (OK), and the next ring is a RING again */
TEST(Core, ZfaxRingCadence)
{
	Pair p("");
	auto run = [&p](int ms, bool ring) {
		for (int t = 0; t < ms; t += 20) {
			if (ring && t % 60 == 0)
				p.b->ring();
			p.step();
		}
	};
	p.b->dte_input("AT#B3\r", 6);
	p.step();
	EXPECT_EQ(p.got_b, "\r\n\r\nStart to measure ring cadence...\r\nHit any key to stop!\r\n");
	p.got_b.clear();
	run(1000, false);
	run(2000, true);
	run(4000, false);
	run(2000, true);
	run(500, false);
	/* (a burst's last pulse at 1980 ms of 2000; the step after
	   AT#B3 is 20 ms more of the first gap) */
	EXPECT_EQ(p.got_b, "OFF      1020 ms\r\n"
		  "ON       1980 ms\r\n"
		  "OFF      4020 ms\r\n"
		  "ON       1980 ms\r\n");
	p.got_b.clear();
	p.b->dte_input("x", 1);
	p.step();
	EXPECT_EQ(p.got_b, "\r\nOK\r\n");
	p.got_b.clear();
	run(2000, true);
	run(3000, false);
	EXPECT_EQ(p.got_b, "\r\nRING\r\n");
}

/* #B2, polling: A in class 6 calls B, which has a document to be
   polled (class 2.0, +FLP=1); A sends its CIG (#P) and DTC, and gets
   CONNECT FAX/... with B's identity, B's page and DISCONNECT0 */
TEST(Core, ZfaxPoll)
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
	auto type = [](Modem *m, const std::string &l) {
		m->dte_input(l.data(), l.size());
	};
	const std::string id_b = "Polled B" + std::string(12, ' ');
	dsp::T4Page pg = fax2_page(240, 77);
	std::string d = fax2_dte_data(pg, false);
	size_t sent = 0;

	type(p.a, "AT+FCLASS=6#B2#P5551111\r");
	type(p.b, "AT+FCLASS=2.0;+FNR=1,1,1,0;+FLI=\"Polled B\";+FLP=1\r");
	p.step();
	EXPECT_EQ(p.got_a, "\r\nOK\r\n");
	EXPECT_EQ(p.got_b, "\r\nOK\r\n");
	p.got_a.clear();
	p.got_b.clear();
	type(p.b, "ATA\r");
	type(p.a, "ATD\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "OK\r\n"); }, 30000))
		<< "B [" << p.got_b << "] A [" << p.got_a << "]";
	EXPECT_EQ(p.got_b, "+FCO\r\n+FPI:\"5551111\"\r\n+FTC:1,5,0,2,0,0,0,0\r\n\r\nOK\r\n");
	p.got_b.clear();
	type(p.b, "AT+FDT\r");
	ASSERT_TRUE(until([&] { return has(p.got_b, "CONNECT"); }, 20000))
		<< "B [" << p.got_b << "]";
	while (sent < d.size()) {
		int n = p.b->dte_space();
		if (n > (int)(d.size() - sent))
			n = d.size() - sent;
		sent += p.b->dte_input(d.data() + sent, n);
		p.step();
	}
	ASSERT_TRUE(until([&] { return ok_after(p.got_b, "CONNECT"); },
			  5000)) << "B [" << p.got_b << "]";
	type(p.b, "AT+FET=2\r");

	ASSERT_TRUE(until([&] { return has(p.got_a, "DISCONNECT"); }, 60000))
		<< "A [" << p.got_a << "]";
	const std::string c = "CONNECT FAX/S1440V1T0R0L2C7P" + id_b + "\r\n";
	ASSERT_EQ(p.got_a.find(c), 0u) << p.got_a;
	size_t end = p.got_a.find("DISCONNECT");
	std::vector<dsp::T4Page> got =
		zfax_pages(p.got_a.substr(c.size(), end - c.size()));
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(got[0].rows, pg.rows);
	EXPECT_TRUE(got[0].bits == pg.bits);
	ASSERT_TRUE(until([&] { return has(p.got_a, id_b + "\r\n"); }, 5000));
	EXPECT_EQ(p.got_a.substr(end), "DISCONNECT0P" + id_b + "\r\n");
	EXPECT_TRUE(until([&] {
		return !p.a->started() && !p.b->started();
	}, 30000));
}

/* T.31's class 1 parameters (Table 5, 8.5, Annex A): +FAR (0, 1), +FCL
   (0-255), +FDD (0), +FIT (0-255, 0 or 1) and +FLO (as +IFC) are taken
   and read back; +FTS=? and +FRS=? give their range */
TEST(Core, FaxClass1Parameters)
{
	Line l;
	Modem *m = make(l, "class1");
	cmd(m, l, "ATE0+FCLASS=1\r");
	EXPECT_EQ(cmd(m, l, "AT+FAR=?\r"), "\r\n+FAR: (0,1)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FAR=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FAR=2\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FAR=0\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCL=50\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCL?\r"), "\r\n+FCL: 50\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FDD=0\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FIT=30,1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FIT?\r"), "\r\n+FIT: 30,1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FIT=256,0\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FLO=?\r"), "\r\n(0-2)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FLO=1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+IFC?\r"), "\r\n+IFC: 1,1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FLO?\r"), "\r\n1\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+IFC=2,0\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FLO?\r"), "\r\n255\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FTS=?\r"), "\r\n(0-255)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FRS=?\r"), "\r\n(0-255)\r\n\r\nOK\r\n");
	delete m;
}

/* class 1 (T.31 8.3.5 c), 8.5.4): the answerer's +FTH=3 after CED,
   with no frame from the DTE for 5 s, ends with ERROR; then +FIT=3,0
   hangs up 3 s after that result, and +FCLASS is 0 again */
TEST(Core, FaxClass1DteTimeouts)
{
	Pair p("");
	auto until = [&p](auto cond, int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return t;
			p.step();
		}
		return cond() ? ms : -1;
	};
	p.a->dte_input("AT+FCLASS=1\r", 12);
	p.b->dte_input("AT+FCLASS=1;+FIT=3,0\r", 21);
	p.step();
	p.got_a.clear();
	p.got_b.clear();
	p.b->dte_input("ATA\r", 4);
	p.a->dte_input("ATD\r", 4);
	ASSERT_GE(until([&] {
		return p.got_b.find("CONNECT") != std::string::npos;
	}, 10000), 0) << p.got_b;
	p.got_b.clear();
	int t = until([&] {
		return p.got_b.find("ERROR") != std::string::npos;
	}, 10000);
	EXPECT_GE(t, 4800) << p.got_b;
	EXPECT_LE(t, 5400) << p.got_b;
	p.got_b.clear();
	t = until([&] {
		return p.got_b.find("NO CARRIER") != std::string::npos;
	}, 10000);
	EXPECT_GE(t, 2800) << p.got_b;
	EXPECT_LE(t, 3400) << p.got_b;
	ASSERT_GE(until([&] { return !p.b->started(); }, 3000), 0);
	p.step();
	p.got_b.clear();
	p.b->dte_input("AT+FCLASS?\r", 11);
	p.step();
	EXPECT_NE(p.got_b.find("\r\n0\r\n"), std::string::npos) << p.got_b;
}

/* class 1 (T.31 8.3.4, 8.4, 8.5.1): the caller's +FRM=96 hears the
   answerer's V.21 frames: +FCERROR (ATV0: +F4), and the +FRH=3 that
   follows has the frame; with +FAR=1, +FRH:3 and then CONNECT */
TEST(Core, FaxClass1FcError)
{
	Pair p("");
	auto until = [&p](auto cond, int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond())
				return t;
			p.step();
		}
		return cond() ? ms : -1;
	};
	auto has_a = [&p](const char *s) {
		return [&p, s] { return p.got_a.find(s) != std::string::npos; };
	};
	auto has_b = [&p](const char *s) {
		return [&p, s] { return p.got_b.find(s) != std::string::npos; };
	};
	/* a DIS from B's DTE (+FTH=3's CONNECT came): a final frame */
	auto dis = [&p]() {
		static const char f[] = "\xff\x13\x80\x00\x46\x00\x10\x03";
		p.b->dte_input(f, sizeof(f) - 1);
	};
	p.a->dte_input("AT+FCLASS=1\r", 12);
	p.b->dte_input("AT+FCLASS=1\r", 12);
	p.step();
	p.b->dte_input("ATA\r", 4);
	p.a->dte_input("ATD\r", 4);
	/* the first DIS to A's first +FRH=3, after B's CED */
	ASSERT_GE(until(has_b("CONNECT"), 10000), 0) << p.got_b;
	dis();
	ASSERT_GE(until(has_a("\x10\x03\r\nOK\r\n"), 5000), 0) << p.got_a;
	ASSERT_GE(until(has_b("OK"), 3000), 0) << p.got_b;
	/* B's DIS again, A's +FRM=96 */
	p.got_a.clear();
	p.got_b.clear();
	p.a->dte_input("AT+FRM=96\r", 10);
	p.step();
	p.b->dte_input("AT+FTH=3\r", 9);
	ASSERT_GE(until(has_b("CONNECT"), 3000), 0) << p.got_b;
	p.got_b.clear();
	dis();
	ASSERT_GE(until(has_a("\r\n+FCERROR\r\n"), 3000), 0) << p.got_a;
	EXPECT_EQ(p.got_a.find("CONNECT"), std::string::npos) << p.got_a;
	p.got_a.clear();
	p.a->dte_input("AT+FRH=3\r", 9);
	ASSERT_GE(until(has_a("\x10\x03\r\nOK\r\n"), 3000), 0) << p.got_a;
	EXPECT_NE(p.got_a.find("\xff\x13\x80"), std::string::npos);
	ASSERT_GE(until(has_b("OK"), 3000), 0) << p.got_b;

	/* ATV0: +F4 */
	p.got_a.clear();
	p.got_b.clear();
	p.a->dte_input("ATV0\r", 5);
	ASSERT_GE(until(has_a("0\r"), 1000), 0) << p.got_a;
	p.got_a.clear();
	p.a->dte_input("AT+FRM=96\r", 10);
	p.step();
	p.b->dte_input("AT+FTH=3\r", 9);
	ASSERT_GE(until(has_b("CONNECT"), 3000), 0) << p.got_b;
	dis();
	ASSERT_GE(until(has_a("+F4\r"), 3000), 0) << p.got_a;
	ASSERT_GE(until(has_b("OK"), 3000), 0) << p.got_b;

	/* +FAR=1: +FRH:3, then CONNECT and the frame */
	p.got_a.clear();
	p.got_b.clear();
	p.step();
	p.a->dte_input("ATV1+FAR=1\r", 11);
	ASSERT_GE(until(has_a("OK\r\n"), 1000), 0) << p.got_a;
	p.got_a.clear();
	p.a->dte_input("AT+FRM=96\r", 10);
	p.step();
	p.b->dte_input("AT+FTH=3\r", 9);
	ASSERT_GE(until(has_b("CONNECT"), 3000), 0) << p.got_b;
	dis();
	ASSERT_GE(until(has_a("\x10\x03\r\nOK\r\n"), 3000), 0) << p.got_a;
	size_t r = p.got_a.find("+FRH:3\r\n"), c = p.got_a.find("CONNECT");
	EXPECT_NE(r, std::string::npos) << p.got_a;
	EXPECT_LT(r, c) << p.got_a;
	EXPECT_EQ(p.got_a.find("FCERROR"), std::string::npos) << p.got_a;
}

/* T.32's parameters (Appendix I): +FCQ, +FCT, +FNR and the like give
   their values at the start and their ranges, and take only values in
   them (class 2.0: hexadecimal); +FIP sets them, +FLI and +FCC as at
   the start again, with +FCLASS kept */
TEST(Core, Fax2Parameters)
{
	Line l;
	Modem *m = make(l, "class2");
	cmd(m, l, "ATE0+FCLASS=2.0\r");
	EXPECT_EQ(cmd(m, l, "AT+FCQ?\r"), "\r\n1,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCQ=?\r"), "\r\n(0-2),(0-2)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCQ=3\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCT?\r"), "\r\n1E\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCT=?\r"), "\r\n(0-FF)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCT=3C\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCT?\r"), "\r\n3C\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FNR=?\r"),
		  "\r\n(0,1),(0,1),(0,1),(0,1)\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FNR=1,1,1,1\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FNR=,0,,0;+FNR?\r"),
		  "\r\n1,0,1,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FAP=1,0,0\r"), "\r\nERROR\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FAP=0,0,0\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FLI=\"123\";+FCC=0,3\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FIP\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCT?;+FNR?\r"),
		  "\r\n1E\r\n0,0,0,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FLI?\r"), "\r\n\"\"\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCC?\r"), "\r\n1,5,0,2,0,0,0,0\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCLASS?\r"), "\r\n2.0\r\n\r\nOK\r\n");
	/* class 2: decimal */
	cmd(m, l, "AT+FCLASS=2\r");
	EXPECT_EQ(cmd(m, l, "AT+FCT?\r"), "\r\n30\r\n\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FIP=0\r"), "\r\nOK\r\n");
	EXPECT_EQ(cmd(m, l, "AT+FCLASS?\r"), "\r\n2\r\n\r\nOK\r\n");
	delete m;
}

/* class 2.0's +FNR (T.32 8.5.1.11): with +FNR=1,0,0,0 the caller's ATD
   reports +FCO and +FIS: (rpr), not +FCI: (idr 0); the answerer's ATA,
   with +FNR at 0,0,0,0, only +FCO */
TEST(Core, Fax2ReportsAsFnr)
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
	const char *la = "AT+FCLASS=2.0;+FLI=\"A\";+FNR=1,0,0,0\r",
		   *lb = "AT+FCLASS=2.0;+FLI=\"B\"\r";
	p.a->dte_input(la, static_cast<s32>(strlen(la)));
	p.b->dte_input(lb, static_cast<s32>(strlen(lb)));
	p.step();
	p.got_a.clear();
	p.got_b.clear();
	p.b->dte_input("ATA\r", 4);
	p.a->dte_input("ATD\r", 4);
	ASSERT_TRUE(until([&] {
		return p.got_a.find("OK\r\n") != std::string::npos;
	}, 30000)) << p.got_a;
	EXPECT_NE(p.got_a.find("+FCO"), std::string::npos) << p.got_a;
	EXPECT_NE(p.got_a.find("+FIS:"), std::string::npos) << p.got_a;
	EXPECT_EQ(p.got_a.find("+FCI:"), std::string::npos) << p.got_a;
	p.got_a.clear();
	p.a->dte_input("AT+FDT\r", 7);
	ASSERT_TRUE(until([&] {
		return p.got_b.find("OK\r\n") != std::string::npos;
	}, 20000)) << p.got_b;
	EXPECT_NE(p.got_b.find("+FCO"), std::string::npos) << p.got_b;
	EXPECT_EQ(p.got_b.find("+FTI:"), std::string::npos) << p.got_b;
	EXPECT_EQ(p.got_b.find("+FCS:"), std::string::npos) << p.got_b;
	p.a->hangup_now();
	p.b->hangup_now();
}

/* class 2.0 (T.32 8.3.3.7, Appendix II.1): two pages, the first ended
   with <DLE><mps> (OK once the MCF came, no +FPS:), the second with
   <DLE><eop> (+FHS:00 and OK); the receiver has both pages, +FET:0 and
   +FET:2 */
TEST(Core, Fax2PostPageDle)
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
	auto type = [](Modem *m, const std::string &l) {
		m->dte_input(l.data(), l.size());
	};
	type(p.a, "AT+FCLASS=2.0;+FNR=1,1,1,0\r");
	type(p.b, "AT+FCLASS=2.0;+FNR=1,1,1,0\r");
	p.step();
	p.got_a.clear();
	p.got_b.clear();
	type(p.b, "ATA\r");
	type(p.a, "ATD\r");
	ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); }, 30000))
		<< p.got_a;
	for (int page = 0; page < 2; page++) {
		dsp::T4Page pg = fax2_page(200, page * 50);
		std::string d = fax2_dte_data(pg, false);
		/* <DLE><ETX> becomes <DLE><ppm> */
		d.back() = page ? '.' : ',';
		size_t sent = 0;
		p.got_a.clear();
		type(p.a, "AT+FDT\r");
		ASSERT_TRUE(until([&] { return has(p.got_a, "CONNECT"); },
				  20000)) << page << ": A [" << p.got_a << "]";
		p.got_a.clear();
		while (sent < d.size()) {
			int n = p.a->dte_space();
			if (n > (int)(d.size() - sent))
				n = d.size() - sent;
			sent += p.a->dte_input(d.data() + sent, n);
			p.step();
		}
		type(p.b, "AT+FDR\r");
		ASSERT_TRUE(until([&] { return has(p.got_b, "CONNECT"); },
				  60000)) << page << ": B [" << p.got_b << "]";
		type(p.b, "\x12");
		ASSERT_TRUE(until([&] {
			return has(p.got_b, "+FET:") && has(p.got_b, "OK\r\n");
		}, 20000)) << page << ": B [" << p.got_b << "]";
		dsp::T4Page rx = fax2_decode(p.got_b, false, true);
		EXPECT_EQ(rx.rows, pg.rows) << page;
		EXPECT_TRUE(has(p.got_b, page ? "+FET:2" : "+FET:0")) << p.got_b;
		p.got_b.clear();
		ASSERT_TRUE(until([&] { return has(p.got_a, "OK\r\n"); },
				  30000)) << page << ": A [" << p.got_a << "]";
		EXPECT_EQ(p.got_a, page ? "+FHS:00\r\n\r\nOK\r\n" : "\r\nOK\r\n")
			<< page;
	}
	EXPECT_TRUE(until([&] {
		return !p.a->started();
	}, 5000));
	p.b->hangup_now();
}
