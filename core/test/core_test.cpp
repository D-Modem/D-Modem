/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    core_test - the modem core by itself: its Host interface, the
 *                timers, the options, a line that fails, and two modems
 *                back to back with data both ways, byte for byte
 *
 */

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
	Pumps() {
		prop_dp_init();
	}
} pumps;

static unsigned long now;

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
	void update_delay(Modem &, int samples) override {
		delay_change += samples;
	}
	/* the DTE's output, kept */
	int dte_write(Modem &, const char *buf, int n) override {
		out.append(buf, n);
		return n;
	}
	/* the test's clock */
	unsigned long now_ms() override { return now; }
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
		  "0,\"\",00000000,00000000,10000000\r\n"
		  "1,\"T\",08043060,08043060,08043060\r\nOK\r\n");
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
