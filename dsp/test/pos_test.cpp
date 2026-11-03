/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pos_test.cpp - a point of sale transaction between two of
 *    libdmodem's modems back to back: the terminal dials the host and
 *    they trade messages in turn, the host first, each sent once the one
 *    before has come whole.  SDLC frames, as a terminal and its host
 *    trade them (SNRM, UA, an information frame each way, DISC, UA),
 *    through V.80's framed sub-mode with the FCS (AT+ES=6,,8,
 *    AT+ESA=0,0,0,,1); or VISA II's plain characters (ENQ, the request,
 *    ACK and the answer, ACK, EOT).  Over V.29 FastPOS (half duplex), V.22
 *    and Bell 212A fast connect (with HDLC flags in the handshake or
 *    not), and plain V.22 for the time it takes; on a clean line and
 *    with noise.  Every message must arrive whole, and the transaction
 *    end within the case's time.
 *
 *    DMODEM_DEBUG=<level>   libdmodem's log
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "portable.h"

#define FRAME 160               /* 20 ms */

/* V.80's in-band codes (Table 9/V.80) */
static const unsigned char EM = 0x19;
static const unsigned char EM_T1 = 0x5C;        /* a 19h */
static const unsigned char EM_T2 = 0x76;        /* a 99h */
static const unsigned char EM_FLAG = 0xB1;
static const unsigned char EM_ERR = 0xB2;

struct PosCase {
	const char *name;
	const char *at;         /* both modems' (after ATE0Q1X3) */
	double within;          /* s from the dial to the last frame */
	float snr = 0;          /* > 0: white noise this far below the
				   signal (dB) */
	bool async = false;     /* plain characters (VISA II's exchange),
				   not SDLC frames through V.80 */
};

/* the SDLC frames of the transaction, the host's (even) and the
   terminal's (odd) in turn, each to or from address 30h: SNRM and UA,
   an information frame each way (the host's prompt of 120 octets, the
   terminal's authorization request of 200), DISC and UA */
static std::vector<std::string> transaction()
{
	std::vector<std::string> f;
	std::mt19937 rng(7);
	auto info = [&](unsigned char ctl, int len) {
		std::string s = { '\x30', static_cast<char>(ctl) };
		for (int k = 0; k < len; k++)
			s += static_cast<char>(rng() & 0xFF);
		return s;
	};
	f.push_back(std::string("\x30\x93", 2));      /* SNRM, P */
	f.push_back(std::string("\x30\x73", 2));      /* UA, F */
	f.push_back(info(0x10, 120));                 /* I(0,0), P */
	f.push_back(info(0x30, 200));                 /* I(0,1), F */
	f.push_back(std::string("\x30\x53", 2));      /* DISC, P */
	f.push_back(std::string("\x30\x73", 2));      /* UA, F */
	return f;
}

/* VISA II's exchange in plain characters, the host's (even) and the
   terminal's (odd) in turn: ENQ; the request, STX ... ETX and its LRC;
   ACK and the answer; ACK; EOT */
static std::vector<std::string> visa2()
{
	auto block = [](const std::string &text) {
		std::string s = "\x02" + text + "\x03";
		char lrc = 0;
		for (size_t k = 1; k < s.size(); k++)
			lrc ^= s[k];
		return s + lrc;
	};
	return { "\x05",
		 block("1.10000000000012345678901234560406=2512101123456789"
		       "00000000012500"),
		 "\x06" + block("APPROVED 123456 000012500"), "\x06", "\x04" };
}

/* frame f for the DTE: 19h and 99h shielded, then <EM><flag> */
static std::string shielded(const std::string &f)
{
	std::string s;
	for (unsigned char c : f) {
		if (c == EM) {
			s += static_cast<char>(EM);
			s += static_cast<char>(EM_T1);
		} else if (c == 0x99) {
			s += static_cast<char>(EM);
			s += static_cast<char>(EM_T2);
		} else
			s += static_cast<char>(c);
	}
	s += static_cast<char>(EM);
	s += static_cast<char>(EM_FLAG);
	return s;
}

/* what a DTE gets: octets, then <EM><flag> for a good frame (or
   <EM><err> for a bad one); other codes ignored.  Before the first good
   frame an <EM><err> is the hunt's (what came in the transparent
   sub-mode, then the first flag) */
struct FrameRx {
	std::string cur;
	bool em = false;
	std::vector<std::string> good;
	int bad = 0;
	/* plain characters: the messages to come, cut out of them by
	   their lengths */
	const std::vector<std::string> *expect = nullptr;

	/* n octets from the modem */
	void feed(const char *buf, int n)
	{
		if (expect) {
			cur.append(buf, n);
			while (good.size() < expect->size() &&
			       cur.size() >= (*expect)[good.size()].size()) {
				size_t len = (*expect)[good.size()].size();
				good.push_back(cur.substr(0, len));
				cur.erase(0, len);
			}
			return;
		}
		for (int i = 0; i < n; i++) {
			unsigned char c = static_cast<unsigned char>(buf[i]);
			if (!em) {
				if (c == EM)
					em = true;
				else
					cur += static_cast<char>(c);
				continue;
			}
			em = false;
			if (c == EM_T1)
				cur += static_cast<char>(EM);
			else if (c == EM_T2)
				cur += static_cast<char>(0x99);
			else if (c == EM_FLAG) {
				if (!cur.empty())
					good.push_back(cur);
				cur.clear();
			} else if (c == EM_ERR) {
				if (!cur.empty() && !good.empty())
					bad++;
				cur.clear();
			} else
				cur.clear();    /* <mark>, <eot>... */
		}
	}
};

/* fd non-blocking */
static void nonblock(int fd)
{
	dmd::set_nonblocking(fd);
}

/* the case's transaction; what went wrong into failures */
static void run(const PosCase &pc, std::vector<std::string> &failures)
{
	std::vector<std::string> tr = pc.async ? visa2() : transaction();
	struct dmodem_config c = {};
	struct dmodem *term, *host;
	int st[2], sh[2], frames = 30 * 50, online = 0, next = 0;
	short th[FRAME] = { 0 }, ht[FRAME] = { 0 }, ot[FRAME], oh[FRAME];
	char init[160];
	FrameRx rx[2];          /* the terminal's DTE, the host's */
	std::mt19937 rng(3);
	std::normal_distribution<float> noise(0, 1);
	float npow = 0;
	double done_at = 0;
	/* what each DTE is to get: the host's at the terminal, the
	   terminal's at the host */
	std::vector<std::string> want[2];
	for (size_t k = 0; k < tr.size(); k++)
		want[k % 2 ? 1 : 0].push_back(tr[k]);
	if (pc.async)
		for (int k = 0; k < 2; k++)
			rx[k].expect = &want[k];

	dmodem_init();
	if (getenv("DMODEM_DEBUG"))
		dmodem_set_debug(atoi(getenv("DMODEM_DEBUG")));
	dmd::stream_pair(st);
	dmd::stream_pair(sh);
	nonblock(st[0]); nonblock(st[1]); nonblock(sh[0]); nonblock(sh[1]);
	snprintf(init, sizeof(init), "ATE0Q1X3%s\r", pc.at);
	c.init = init;
	c.law = DMODEM_LAW_ULAW;
	c.name = "terminal";
	c.dte_fd = st[0];
	term = dmodem_create(&c);
	c.name = "host";
	c.dte_fd = sh[0];
	host = dmodem_create(&c);
	ASSERT_TRUE(term && host);
	char buf[4096];
	while (dmd::fd_read(st[1], buf, sizeof(buf)) > 0)
		;
	while (dmd::fd_read(sh[1], buf, sizeof(buf)) > 0)
		;
	dmodem_answer(host);
	dmodem_dial(term, "");

	for (int i = 0; i < frames && !done_at; i++) {
		struct dmodem_status s1, s2;
		int n;
		dmodem_process(term, ht, ot, FRAME);
		dmodem_process(host, th, oh, FRAME);
		for (int k = 0; k < FRAME; k++) {
			/* the noise from the signal's power while it lasts */
			float p = 0.5f * (ot[k] * ot[k] + oh[k] * oh[k]);
			if (p > 1e4f)
				npow += 0.001f * (p - npow);
			float sd = pc.snr > 0 ?
				sqrtf(npow * powf(10, -pc.snr / 10)) : 0;
			th[k] = (short)(ot[k] * 0.5f + sd * noise(rng));
			ht[k] = (short)(oh[k] * 0.5f + sd * noise(rng));
		}
		dmodem_poll_dte(term);
		dmodem_poll_dte(host);
		dmodem_status(term, &s1);
		dmodem_status(host, &s2);
		if (!online && s1.link == DMODEM_ONLINE &&
		    s2.link == DMODEM_ONLINE) {
			online = i;
			printf("online after %.2f s, %u/%u bit/s\n", i / 50.0,
			       s1.tx_rate, s1.rx_rate);
			/* both DTEs to the framed sub-mode */
			const char go[] = { (char)EM, (char)EM_FLAG };
			if (!pc.async) {
				dmd::fd_write(st[1], go, 2);
				dmd::fd_write(sh[1], go, 2);
			}
		}
		while ((n = (int)dmd::fd_read(st[1], buf, sizeof(buf))) > 0)
			rx[0].feed(buf, n);
		while ((n = (int)dmd::fd_read(sh[1], buf, sizeof(buf))) > 0)
			rx[1].feed(buf, n);
		if (!online)
			continue;
		/* the next frame once the one before it has come: the host's
		   (even) from the host, the terminal's from the terminal */
		size_t have = rx[0].good.size() + rx[1].good.size();
		if (next < (int)tr.size() && have == (size_t)next &&
		    i >= online + 10) {
			std::string s = pc.async ? tr[next] : shielded(tr[next]);
			int fd = next % 2 ? st[1] : sh[1];
			dmd::fd_write(fd, s.data(), s.size());
			next++;
		}
		if (next == (int)tr.size() && have == tr.size()) {
			done_at = i / 50.0;
			printf("transaction done after %.2f s\n", done_at);
		}
	}
	if (!online)
		failures.push_back("never online");
	/* the host's frames at the terminal, the terminal's at the host,
	   in order */
	for (int k = 0; k < 2; k++) {
		const char *who = k ? "host" : "terminal";
		if (rx[k].good != want[k])
			failures.push_back(std::string(who) + ": " +
				std::to_string(rx[k].good.size()) + " of " +
				std::to_string(want[k].size()) +
				" frames, or not as sent");
		if (rx[k].bad)
			failures.push_back(std::string(who) + ": " +
				std::to_string(rx[k].bad) + " bad frames");
	}
	if (!done_at)
		failures.push_back("transaction not done");
	else if (done_at > pc.within)
		failures.push_back("transaction took " +
				   std::to_string(done_at) + " s");
	dmodem_destroy(term);
	dmodem_destroy(host);
	dmd::fd_close(st[0]); dmd::fd_close(st[1]);
	dmd::fd_close(sh[0]); dmd::fd_close(sh[1]);
}

class Pos : public testing::TestWithParam<PosCase> {};

/* the transaction whole, in time */
TEST_P(Pos, Transaction)
{
	std::vector<std::string> f;
	run(GetParam(), f);
	for (auto &e : f)
		ADD_FAILURE() << e;
}

/* the case's name for the test's */
static std::string name(const testing::TestParamInfo<PosCase> &i)
{
	return i.param.name;
}

/* the DTEs' settings: V.80's framed sub-mode with FCS-16, flags between
   frames; no answer delay (S92=0) */
#define SYNC "\\N0S92=0+ES=6,,8+ESA=0,0,0,,1"

INSTANTIATE_TEST_SUITE_P(Modes, Pos, testing::Values(
	PosCase{ "v29fastpos", SYNC "+MS=V29FP", 4.0 },
	PosCase{ "v29fastpos_snr25", SYNC "+MS=V29FP", 4.0, 25 },
	PosCase{ "v29fastpos_7200", SYNC "+MS=V29FP,0,4800,7200", 4.0 },
	PosCase{ "v22_fast_flags", SYNC "S49=3+MS=22", 5.0 },
	PosCase{ "v22_fast_flags_snr12", SYNC "S49=3+MS=22", 5.0, 12 },
	PosCase{ "v22_fast", SYNC "S49=1+MS=22", 5.0 },
	PosCase{ "bell212_fast_flags", SYNC "S49=3+MS=212", 5.0 },
	PosCase{ "v22", SYNC "+MS=22", 10.0 },
	PosCase{ "v29fastpos_visa2", "\\N0S92=0+MS=V29FP", 3.5, 0, true },
	PosCase{ "v22_fast_visa2", "\\N0S92=0S49=1+MS=22", 3.5, 0, true },
	PosCase{ "v22_visa2", "\\N0S92=0+MS=22", 8.0, 0, true }), name);
