/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    clear_call_test.cpp - the clear 64 kbit/s channel (AT+MS=CLEAR, an
 *    ISDN B-channel's data call): two of libdmodem's modems on a
 *    simulated digital channel, each way's octets unchanged after a
 *    delay.  CONNECT 64000 both ways within a second, data both ways
 *    through V.42 byte for byte, and without error correction every
 *    octet value; the channel from the program (dmodem_config's clear)
 *    whatever +MS says; an answerer that comes late; no answerer (NO
 *    CARRIER after S7); a nailed-up channel (AT&L2) that calls again
 *    by itself.  libdmodem's octets on a clear channel are A-law
 *    samples, which give every octet back.
 *
 *    CLEAR_DEBUG=<level>   libdmodem's log
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "portable.h"

namespace {

struct Dte {
	int fd[2];
	/* a socket pair, both ends non-blocking */
	Dte()
	{
		if (dmd::stream_pair(fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			dmd::set_nonblocking(f);
	}
	/* (both ends closed) */
	~Dte()
	{
		dmd::fd_close(fd[0]);
		dmd::fd_close(fd[1]);
	}
	/* what the modem wrote to the DTE so far */
	std::string read_all()
	{
		std::string s;
		char buf[4096];
		ssize_t n;
		while ((n = dmd::fd_read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, static_cast<size_t>(n));
		return s;
	}
	/* s from at on to the modem, what it took */
	size_t write_some(const std::string &s, size_t at)
	{
		if (at >= s.size())
			return 0;
		ssize_t n = dmd::fd_write(fd[1], s.data() + at,
					  std::min<size_t>(s.size() - at, 4096));
		return n > 0 ? static_cast<size_t>(n) : 0;
	}
};

/* one end: its modem, its DTE's text, and its data once online */
struct End {
	Dte dte;
	struct dmodem *m = nullptr;
	std::string text, data;
	bool online = false;
	int connects = 0, no_carriers = 0;
	size_t sent = 0;
	/* a modem with its init line, clear: the program's clear channel */
	End(const char *name, const char *init, bool clear = false,
	    dmodem_law law = DMODEM_LAW_ALAW)
	{
		dmodem_config c{};
		c.name = name;
		c.dte_fd = dte.fd[0];
		c.init = init;
		c.law = law;
		c.clear = clear;
		m = dmodem_create(&c);
	}
	/* (the modem destroyed) */
	~End() { dmodem_destroy(m); }
	/* the DTE's text so far: CONNECT, data, NO CARRIER */
	void take()
	{
		dmodem_poll_dte(m);
		std::string s = dte.read_all();
		if (s.empty())
			return;
		text += s;
		if (!online) {
			if (s.find("NO CARRIER") != std::string::npos)
				no_carriers++;
			size_t at = s.find("CONNECT");
			if (at == std::string::npos)
				return;
			size_t e = s.find("\r\n", at);
			connects++;
			online = true;
			data = e == std::string::npos ? std::string() :
				s.substr(e + 2);
			return;
		}
		struct dmodem_status st;
		dmodem_status(m, &st);
		if (st.link != DMODEM_ONLINE &&
		    s.find("NO CARRIER") != std::string::npos) {
			no_carriers++;
			online = false;
			return;
		}
		data += s;
	}
	/* its rates now */
	std::pair<uint32_t, uint32_t> rates() const
	{
		struct dmodem_status st;
		dmodem_status(m, &st);
		return { st.tx_rate, st.rx_rate };
	}
};

/* a digital channel: each way's octets delay octets later (at least a
   block's, 160), unchanged; idle (A-law's silence) before the first
   and while cut */
struct Channel {
	size_t delay = 240;
	std::deque<unsigned char> ca, ac;
	double drop_at = -1, drop_s = 0;
	long t = 0;
	/* an octet as it comes out at the far end */
	unsigned char pass(unsigned char o, long k) const
	{
		double s = static_cast<double>(k) / 8000;
		return s >= drop_at && s < drop_at + drop_s ? 0xD5 : o;
	}
};

template <class Stop>
/* secs of the channel between c and a, or until stop() */
void run(Channel &l, End &c, End &a, double secs, Stop stop)
{
	const int32_t n = 160;
	std::vector<unsigned char> ci(n), ai(n), co(n), ao(n);
	long end = l.t + static_cast<long>(secs * 8000);
	if (l.ca.empty()) {
		l.ca.assign(l.delay, 0xD5);
		l.ac.assign(l.delay, 0xD5);
	}
	while (l.t < end) {
		for (int32_t i = 0; i < n; i++) {
			ci[i] = l.pass(l.ac.front(), l.t + i);
			ai[i] = l.pass(l.ca.front(), l.t + i);
			l.ac.pop_front();
			l.ca.pop_front();
		}
		l.t += n;
		dmodem_process_g711(c.m, ci.data(), co.data(), n);
		dmodem_process_g711(a.m, ai.data(), ao.data(), n);
		l.ca.insert(l.ca.end(), co.begin(), co.end());
		l.ac.insert(l.ac.end(), ao.begin(), ao.end());
		c.take();
		a.take();
		if (stop())
			break;
	}
}

/* n bytes of data, a different stream for each side */
std::string bulk(size_t n, unsigned side)
{
	std::string s(n, '\0');
	for (size_t k = 0; k < n; k++) {
		unsigned h = static_cast<unsigned>(k) * 2654435761u + side;
		h = ((h >> 16) ^ h) * 0x45d9f3b;
		h = ((h >> 16) ^ h) * 0x45d9f3b;
		s[k] = static_cast<char>((h >> 16) ^ h);
	}
	return s;
}

/* the log level from CLEAR_DEBUG */
void debug()
{
	dmodem_init();
	if (getenv("CLEAR_DEBUG"))
		dmodem_set_debug(atoi(getenv("CLEAR_DEBUG")));
}

/* n bytes each way within secs, byte for byte */
bool exchange(Channel &l, End &c, End &a, size_t n, double secs,
	      unsigned seed = 1)
{
	std::string to_a = bulk(n, seed), to_c = bulk(n, seed + 1);
	c.sent = a.sent = 0;
	c.data.clear();
	a.data.clear();
	run(l, c, a, secs, [&] {
		c.sent += c.dte.write_some(to_a, c.sent);
		a.sent += a.dte.write_some(to_c, a.sent);
		return a.data.size() >= n && c.data.size() >= n;
	});
	return a.data == to_a && c.data == to_c;
}

/* ATD at c, ATA at a, then CONNECT at both within secs */
bool call(Channel &l, End &c, End &a, double secs)
{
	if (dmodem_dial(c.m, "5551000") || dmodem_answer(a.m))
		return false;
	run(l, c, a, secs, [&] { return c.online && a.online; });
	return c.online && a.online;
}

} // namespace

/* +MS=CLEAR at both ends: CONNECT 64000 both ways within a second of
   the call, then 200 kB each way through V.42 */
TEST(Clear, ConnectsAt64000)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3+MS=CLEAR\r"), a("answerer", "ATE0+MS=CLEAR\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 64000"), std::string::npos) << c.text;
	EXPECT_NE(a.text.find("CONNECT 64000"), std::string::npos) << a.text;
	EXPECT_EQ(c.rates(), std::make_pair(64000u, 64000u));
	EXPECT_EQ(a.rates(), std::make_pair(64000u, 64000u));
	EXPECT_TRUE(exchange(l, c, a, 200000, 60)) << c.data.size() << " "
						   << a.data.size();
}

/* without error correction (\N0): every octet value, each way, as it
   was sent */
TEST(Clear, EveryOctetWithoutV42)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\\N0+MS=CLEAR\r"),
		a("answerer", "ATE0\\N0+MS=CLEAR\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	/* (the far end's link may still be starting) */
	run(l, c, a, 0.2, [] { return false; });
	c.data.clear();
	a.data.clear();
	std::string all;
	for (int k = 0; k < 4; k++)
		for (int v = 0; v < 256; v++)
			all += static_cast<char>(v);
	c.sent = a.sent = 0;
	run(l, c, a, 3, [&] {
		c.sent += c.dte.write_some(all, c.sent);
		a.sent += a.dte.write_some(all, a.sent);
		return a.data.size() >= all.size() &&
			c.data.size() >= all.size();
	});
	EXPECT_EQ(a.data, all);
	EXPECT_EQ(c.data, all);
}

/* the program says the line is clear (an ISDN data call) and +MS is
   the default: the clear channel all the same */
TEST(Clear, FromTheProgram)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\r", true), a("answerer", "ATE0\r", true);
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	EXPECT_EQ(c.rates().first, 64000u);
	EXPECT_TRUE(exchange(l, c, a, 20000, 10));
}

/* dmodem_set_clear() before the call, and back to G.711 after it: the
   next call is V.91 between the two again */
TEST(Clear, SetPerCall)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\r", false, DMODEM_LAW_ULAW),
		a("answerer", "ATE0\r", false, DMODEM_LAW_ULAW);
	dmodem_set_clear(c.m, 1);
	dmodem_set_clear(a.m, 1);
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	EXPECT_EQ(c.rates().first, 64000u);
	dmodem_hangup(c.m);
	dmodem_hangup(a.m);
	run(l, c, a, 0.5, [] { return false; });
	c.online = a.online = false;
	dmodem_set_clear(c.m, 0);
	dmodem_set_clear(a.m, 0);
	ASSERT_TRUE(call(l, c, a, 20)) << c.text << a.text;
	EXPECT_LT(c.rates().first, 64000u);
	EXPECT_GT(c.rates().first, 50000u);
}

/* the answerer 3 s after the caller: the caller's flags go on until
   the answerer's come */
TEST(Clear, LateAnswerer)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3+MS=CLEAR\r");
	End idle("idle", "ATE0\r");
	ASSERT_EQ(dmodem_dial(c.m, "5551000"), 0);
	run(l, c, idle, 3, [] { return false; });
	EXPECT_FALSE(c.online);
	End a("answerer", "ATE0+MS=CLEAR\r");
	ASSERT_EQ(dmodem_answer(a.m), 0);
	run(l, c, a, 2, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online && a.online) << c.text << a.text;
	std::string msg = "late but here\r\n";
	c.dte.write_some(msg, 0);
	run(l, c, a, 2, [&] {
		return a.data.find(msg) != std::string::npos;
	});
	EXPECT_NE(a.data.find(msg), std::string::npos);
}

/* nobody answers: NO CARRIER after S7 (2 s) */
TEST(Clear, NoAnswer)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3S7=2+MS=CLEAR\r"), idle("idle", "ATE0\r");
	ASSERT_EQ(dmodem_dial(c.m, "5551000"), 0);
	run(l, c, idle, 4, [&] { return c.no_carriers > 0; });
	EXPECT_EQ(c.no_carriers, 1) << c.text;
	EXPECT_FALSE(c.online);
}

/* a nailed-up channel (AT&L2, S0=1 answering with no ring): ATD with
   no number, CONNECT, data both ways */
TEST(Clear, NailedUp)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3&L2+MS=CLEAR\r"),
		a("answerer", "ATE0&L2S0=1+MS=CLEAR\r");
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	run(l, c, a, 2, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online && a.online) << c.text << a.text;
	EXPECT_TRUE(exchange(l, c, a, 20000, 10));
}
