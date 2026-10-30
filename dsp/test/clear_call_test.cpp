/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    clear_call_test.cpp - the clear 64 kbit/s channel (AT+MS=CLEAR, an
 *    ISDN B-channel's data call): two of libdmodem's modems on a
 *    simulated digital channel, each way's octets unchanged after a
 *    delay.  CONNECT 64000 both ways within a second, data both ways
 *    through X.75 (+B2=0, the default) byte for byte, and through the
 *    modem's link (+B2=7) without error correction every octet value;
 *    the channel from the program (dmodem_config's clear) whatever +MS
 *    says; an answerer that comes late; no answerer (NO CARRIER after
 *    S7); a nailed-up channel (AT&L2) that calls again by itself.
 *    X.75 on the line as T.90 and isdn4linux have it (the frames
 *    decoded here), through a dropout, and its release; PPP's frames
 *    (+B2=5) in synchronous HDLC on the line and in RFC 1662's async
 *    framing at the DTEs.  libdmodem's octets on a clear channel are
 *    A-law samples, which give every octet back.
 *
 *    CLEAR_DEBUG=<level>   libdmodem's log
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <string>
#include <tuple>
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
	/* each way's octets as sent, from the start */
	std::vector<unsigned char> tap_ca, tap_ac;
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
		l.tap_ca.insert(l.tap_ca.end(), co.begin(), co.end());
		l.tap_ac.insert(l.tap_ac.end(), ao.begin(), ao.end());
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
   the call, then 200 kB each way through X.75 */
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

/* the modem's link (+B2=7) without error correction (\N0): every octet
   value, each way, as it was sent */
TEST(Clear, EveryOctetWithoutV42)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\\N0+MS=CLEAR;+B2=7\r"),
		a("answerer", "ATE0\\N0+MS=CLEAR;+B2=7\r");
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

namespace {

/* the FCS-16 (RFC 1662) of n octets, the register after them */
unsigned fcs16(const unsigned char *p, size_t n, unsigned fcs = 0xffff)
{
	while (n--) {
		fcs ^= *p++;
		for (int i = 0; i < 8; i++)
			fcs = fcs & 1 ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
	}
	return fcs;
}

/* the good HDLC frames (without FCS) in a channel's octets, each
   octet's first bit its most significant, decoded independently of
   the link layer: flags, zero deletion, aborts, the FCS checked */
std::vector<std::vector<unsigned char>> hdlc_frames(
	const std::vector<unsigned char> &line)
{
	std::vector<unsigned char> bits;
	for (unsigned char o : line)
		for (int k = 0; k < 8; k++)
			bits.push_back(o >> (7 - k) & 1);
	/* the 8 bits at i are a flag (0, six 1s, 0); the 7 at i are 1s */
	auto flag = [&](size_t i) {
		if (i + 8 > bits.size() || bits[i] || bits[i + 7])
			return false;
		for (size_t k = 1; k < 7; k++)
			if (!bits[i + k])
				return false;
		return true;
	};
	auto abort7 = [&](size_t i) {
		if (i + 7 > bits.size())
			return false;
		for (size_t k = 0; k < 7; k++)
			if (!bits[i + k])
				return false;
		return true;
	};
	std::vector<std::vector<unsigned char>> frames;
	std::vector<unsigned char> f;
	unsigned ones = 0, n = 0, octet = 0;
	bool in = false;
	for (size_t i = 0; i < bits.size();) {
		if (flag(i)) {
			if (in && !n && f.size() >= 4 &&
			    fcs16(f.data(), f.size()) == 0xf0b8) {
				f.resize(f.size() - 2);
				frames.push_back(f);
			}
			f.clear();
			in = true;
			ones = n = octet = 0;
			i += 8;
			continue;
		}
		if (!in) {
			i++;
			continue;
		}
		if (abort7(i)) {
			in = false;
			i += 7;
			continue;
		}
		unsigned b = bits[i++];
		if (ones == 5) {
			ones = 0;       /* (b is 0: a stuffed one) */
			continue;
		}
		ones = b ? ones + 1 : 0;
		octet |= b << n;
		if (++n == 8) {
			f.push_back(static_cast<unsigned char>(octet));
			n = octet = 0;
		}
	}
	return frames;
}

/* a frame in RFC 1662's async framing: flag, the octets and FCS with
   7Eh, 7Dh (and, esc_ctl, the control characters) escaped, flag */
std::string async_frame(const std::vector<unsigned char> &f, bool esc_ctl)
{
	std::vector<unsigned char> all = f;
	unsigned fcs = ~fcs16(f.data(), f.size()) & 0xffff;
	all.push_back(static_cast<unsigned char>(fcs));
	all.push_back(static_cast<unsigned char>(fcs >> 8));
	std::string s = "\x7e";
	for (unsigned char c : all) {
		if (c == 0x7e || c == 0x7d || (esc_ctl && c < 0x20)) {
			s += '\x7d';
			c ^= 0x20;
		}
		s += static_cast<char>(c);
	}
	return s + "\x7e";
}

/* the frames (without FCS) in RFC 1662 async framing in s, those with
   a good FCS */
std::vector<std::vector<unsigned char>> async_frames(const std::string &s)
{
	std::vector<std::vector<unsigned char>> frames;
	std::vector<unsigned char> f;
	bool esc = false;
	for (char ch : s) {
		unsigned char c = static_cast<unsigned char>(ch);
		if (c == 0x7e) {
			if (f.size() >= 4 && fcs16(f.data(), f.size()) == 0xf0b8) {
				f.resize(f.size() - 2);
				frames.push_back(f);
			}
			f.clear();
			esc = false;
		} else if (c == 0x7d)
			esc = true;
		else {
			f.push_back(esc ? c ^ 0x20 : c);
			esc = false;
		}
	}
	return frames;
}

/* a PPP frame of n octets: FF 03, protocol 0021 (IP), then octets of
   every value from seed */
std::vector<unsigned char> ppp_frame(size_t n, unsigned seed)
{
	std::vector<unsigned char> f = { 0xff, 0x03, 0x00, 0x21 };
	for (size_t k = 4; k < n; k++)
		f.push_back(static_cast<unsigned char>(k * 7 + seed));
	return f;
}

} // namespace

/* X.75 on the line as isdn4linux has it: the caller's SABM (P 1) at
   address B (01h), the answerer's UA (F 1) at B, the caller's first I
   frame at B with N(S) 0, the answerer's I frames at A (03h); and in
   the result codes (\V1) CONNECT 64000/X.75 */
TEST(Clear, X75Frames)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\\V1+MS=CLEAR\r"),
		a("answerer", "ATE0\\V1+MS=CLEAR\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 64000/X.75"), std::string::npos)
		<< c.text;
	EXPECT_NE(a.text.find("CONNECT 64000/X.75"), std::string::npos)
		<< a.text;
	ASSERT_TRUE(exchange(l, c, a, 1000, 5));
	auto fc = hdlc_frames(l.tap_ca), fa = hdlc_frames(l.tap_ac);
	ASSERT_GE(fc.size(), 2u);
	ASSERT_GE(fa.size(), 2u);
	EXPECT_EQ(fc[0], (std::vector<unsigned char>{ 0x01, 0x3f }));
	EXPECT_EQ(fa[0], (std::vector<unsigned char>{ 0x01, 0x73 }));
	bool ci = false, ai = false;
	for (auto &f : fc)
		if (f.size() > 2 && !(f[1] & 1)) {
			EXPECT_EQ(f[0], 0x01);
			EXPECT_EQ(f[1] & 0x0e, 0) << "N(S) of the first";
			ci = true;
			break;
		}
	for (auto &f : fa)
		if (f.size() > 2 && !(f[1] & 1)) {
			EXPECT_EQ(f[0], 0x03);
			ai = true;
			break;
		}
	EXPECT_TRUE(ci && ai);
}

/* 0.3 s of the channel lost both ways in the middle of 100 kB each
   way: X.75's T1 and REJ recovery deliver it all, in order */
TEST(Clear, X75Dropout)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3+MS=CLEAR\r"), a("answerer", "ATE0+MS=CLEAR\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	l.drop_at = static_cast<double>(l.t) / 8000 + 2;
	l.drop_s = 0.3;
	EXPECT_TRUE(exchange(l, c, a, 100000, 60)) << c.data.size() << " "
						   << a.data.size();
}

/* ATH at the caller: its DISC, NO CARRIER at the answerer too */
TEST(Clear, X75Release)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3+MS=CLEAR\r"), a("answerer", "ATE0+MS=CLEAR\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	dmodem_hangup(c.m);
	run(l, c, a, 2, [&] { return a.no_carriers > 0; });
	EXPECT_EQ(a.no_carriers, 1) << a.text;
	bool disc = false;
	for (auto &f : hdlc_frames(l.tap_ca))
		disc = disc || (f.size() == 2 && f[0] == 0x01 &&
				(f[1] & ~0x10) == 0x43);
	EXPECT_TRUE(disc);
}

/* PPP (+B2=5): the caller's DTE sends async frames (control characters
   not escaped, as an ACCM of 0 has them), the line carries each as one
   synchronous HDLC frame with its own FCS, the answerer's DTE gets it
   in async framing; and back.  A frame with a bad FCS goes nowhere */
TEST(Clear, PppFrames)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\\V1+MS=CLEAR;+B2=5\r"),
		a("answerer", "ATE0+MS=CLEAR;+B2=5\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 64000/PPP"), std::string::npos)
		<< c.text;
	std::vector<std::vector<unsigned char>> to_a, to_c;
	std::string out_c, out_a;
	for (unsigned k = 0; k < 20; k++) {
		to_a.push_back(ppp_frame(40 + 70 * k, k));
		to_c.push_back(ppp_frame(1500 - 60 * k, k + 100));
		out_c += async_frame(to_a.back(), false);
		out_a += async_frame(to_c.back(), true);
		if (k == 5) {
			std::string bad = async_frame(ppp_frame(100, 9), false);
			bad[10] = static_cast<char>(bad[10] ^ 1);
			out_c += bad;
		}
	}
	c.sent = a.sent = 0;
	c.data.clear();
	a.data.clear();
	run(l, c, a, 10, [&] {
		c.sent += c.dte.write_some(out_c, c.sent);
		a.sent += a.dte.write_some(out_a, a.sent);
		return async_frames(a.data).size() >= to_a.size() &&
			async_frames(c.data).size() >= to_c.size();
	});
	EXPECT_EQ(async_frames(a.data), to_a);
	EXPECT_EQ(async_frames(c.data), to_c);
	/* on the line: just the frames, each once, without our FCS */
	EXPECT_EQ(hdlc_frames(l.tap_ca), to_a);
	EXPECT_EQ(hdlc_frames(l.tap_ac), to_c);
}

namespace {

/* +CBST's <speed> codes and their user rates (27.007 6.7) */
const std::pair<int, unsigned> CBST_RATES[] = {
	{ 65, 300 }, { 66, 1200 }, { 68, 2400 }, { 70, 4800 },
	{ 71, 9600 }, { 75, 14400 }, { 79, 19200 }, { 80, 28800 },
	{ 81, 38400 }, { 83, 56000 },
};

/* V.110 on a channel's octets, decoded independently of libdmodem: the
   octets' first k bits (the intermediate rate's, the rest of each
   octet ones) as 80 bit frames after the first alignment pattern; how
   many frames, how many had a bad pattern, and the E1-E3 and S and X
   of the last */
struct V110Seen {
	int frames = 0, bad = 0, rest_not_ones = 0;
	unsigned e123 = 0, s = 1, x = 1;
};

/* (see V110Seen) line's frames with k bits an octet */
V110Seen v110_seen(const std::vector<unsigned char> &line, int k)
{
	V110Seen v;
	std::vector<unsigned char> bits;
	for (unsigned char o : line) {
		for (int i = 0; i < k; i++)
			bits.push_back(o >> (7 - i) & 1);
		if (k < 8 && (o & (0xff >> k)) != (0xff >> k))
			v.rest_not_ones++;
	}
	auto pattern = [&](size_t at) {
		for (size_t i = 0; i < 80; i++) {
			unsigned b = bits[at + i];
			if (i < 8 ? b != 0 : (i % 8 == 0 && b != 1))
				return false;
		}
		return true;
	};
	size_t at = 0;
	while (at + 80 <= bits.size() && !pattern(at))
		at++;
	for (; at + 80 <= bits.size(); at += 80) {
		if (!pattern(at)) {
			if (getenv("V110_DUMP") && v.bad < 3) {
				fprintf(stderr, "bad at bit %zu of %zu:", at,
					bits.size());
				for (size_t i = 0; i < 80; i++)
					fprintf(stderr, "%s%u", i % 8 ? "" : " ",
						bits[at + i]);
				fprintf(stderr, "\n");
			}
			v.bad++;
			continue;
		}
		v.frames++;
		const unsigned char *f = bits.data() + at;
		v.e123 = f[41] | f[42] << 1 | f[43] << 2;
		v.s = f[15] | f[31] | f[39] | f[55] | f[71] | f[79];
		v.x = f[23] | f[63];
	}
	return v;
}

/* a channel's octets from the pump's first on (the silence before the
   call left out) */
std::vector<unsigned char> from_start(const std::vector<unsigned char> &t)
{
	auto at = std::find_if(t.begin(), t.end(),
			       [](unsigned char o) { return o != 0xD5; });
	return std::vector<unsigned char>(at, t.end());
}

/* an init line with +CBST=speed[,name] */
std::string cbst_init(const char *base, int speed, int name = 0)
{
	return std::string(base) + "+MS=CLEAR;+CBST=" + std::to_string(speed) +
		"," + std::to_string(name) + "\r";
}

} // namespace

/* V.110 asynchronous at each of +CBST's rates, both ends asking for it,
   no V.42 (\N0): CONNECT at the user rate both ways, the DTEs'
   characters through, at that rate */
TEST(Clear, V110EachRate)
{
	debug();
	for (auto [speed, rate] : CBST_RATES) {
		SCOPED_TRACE(rate);
		Channel l;
		End c("caller", cbst_init("ATE0X3\\N0\\V1", speed).c_str()),
			a("answerer", cbst_init("ATE0\\N0", speed).c_str());
		ASSERT_TRUE(call(l, c, a, 2)) << c.text << a.text;
		std::string want = "CONNECT " + std::to_string(rate) + "/V.110";
		EXPECT_NE(c.text.find(want), std::string::npos) << c.text;
		EXPECT_EQ(c.rates(), std::make_pair(rate, rate));
		EXPECT_EQ(a.rates(), std::make_pair(rate, rate));
		/* (the far end's link may still be starting) */
		run(l, c, a, 0.1, [] { return false; });
		size_t n = std::max<size_t>(rate / 40, 8);
		EXPECT_TRUE(exchange(l, c, a, n, 3)) << c.data.size() << " "
						     << a.data.size();
	}
}

/* V.110 on the line as the standard has it: at 9600 bit/s the
   intermediate rate is 16 kbit/s, the first two bits of each octet,
   the rest ones; every frame's alignment pattern good; E1-E3 0, 1, 1
   (N x 4800); S and X ON once in sync */
TEST(Clear, V110Frames)
{
	debug();
	Channel l;
	End c("caller", cbst_init("ATE0X3\\N0", 71).c_str()),
		a("answerer", cbst_init("ATE0\\N0", 71).c_str());
	ASSERT_TRUE(call(l, c, a, 2)) << c.text << a.text;
	exchange(l, c, a, 500, 3);
	V110Seen v = v110_seen(from_start(l.tap_ca), 2);
	EXPECT_EQ(v.rest_not_ones, 0);
	EXPECT_EQ(v.bad, 0);
	EXPECT_GT(v.frames, 100);
	EXPECT_EQ(v.e123, 0x6u);
	EXPECT_EQ(v.s, 0u);
	EXPECT_EQ(v.x, 0u);
	/* 1200 bit/s: 8 kbit/s, E1-E3 0, 1, 0; 14400: 32 kbit/s, 1, 0, 1 */
	for (auto [speed, k, e] : { std::tuple{ 66, 1, 0x2u },
				    std::tuple{ 75, 4, 0x5u },
				    std::tuple{ 81, 8, 0x6u } }) {
		Channel m;
		End c2("caller", cbst_init("ATE0X3\\N0", speed).c_str()),
			a2("answerer", cbst_init("ATE0\\N0", speed).c_str());
		ASSERT_TRUE(call(m, c2, a2, 2)) << c2.text << a2.text;
		run(m, c2, a2, 0.3, [] { return false; });
		V110Seen w = v110_seen(from_start(m.tap_ca), k);
		EXPECT_EQ(w.bad, 0) << speed;
		EXPECT_EQ(w.rest_not_ones, 0) << speed;
		EXPECT_EQ(w.e123, e) << speed;
	}
}

/* an answerer with no V.110 rate of its own (+CBST=0, the default)
   finds the caller's from its frames: CONNECT at that rate */
TEST(Clear, V110FoundByAnswerer)
{
	debug();
	for (auto [speed, rate] : CBST_RATES) {
		if (rate == 300 || rate == 56000)
			continue;       /* (600's frames; no frames) */
		SCOPED_TRACE(rate);
		Channel l;
		End c("caller", cbst_init("ATE0X3\\N0", speed).c_str()),
			a("answerer", "ATE0X3\\N0+MS=CLEAR\r");
		ASSERT_TRUE(call(l, c, a, 2)) << c.text << a.text;
		EXPECT_NE(a.text.find("CONNECT " + std::to_string(rate)),
			  std::string::npos) << a.text;
		run(l, c, a, 0.1, [] { return false; });
		EXPECT_TRUE(exchange(l, c, a, rate / 40, 3));
	}
}

/* V.42 over V.110 (the default \N): LAPM's detection phase and XID on
   the asynchronous bits, CONNECT 19200, data through */
TEST(Clear, V110WithV42)
{
	debug();
	Channel l;
	End c("caller", cbst_init("ATE0X3\\V1", 79).c_str()),
		a("answerer", cbst_init("ATE0", 79).c_str());
	ASSERT_TRUE(call(l, c, a, 5)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 19200/LAPM"), std::string::npos)
		<< c.text;
	EXPECT_TRUE(exchange(l, c, a, 5000, 10));
}

/* V.110 synchronous (+CBST=71,1): +B2's X.75 in HDLC at 9600 bit/s */
TEST(Clear, V110SyncX75)
{
	debug();
	Channel l;
	End c("caller", cbst_init("ATE0X3\\V1", 71, 1).c_str()),
		a("answerer", cbst_init("ATE0", 71, 1).c_str());
	ASSERT_TRUE(call(l, c, a, 5)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 9600/X.75"), std::string::npos)
		<< c.text;
	EXPECT_TRUE(exchange(l, c, a, 3000, 10));
}

/* a far end that is no D-Modem, its HDLC flags and then its frames with
   no ones between (an X.75 TA's SABM): the answerer takes it from the
   frames on */
TEST(Clear, HdlcPeerWithoutOnes)
{
	debug();
	Channel l;
	End a("answerer", "ATE0\\V1+MS=CLEAR\r");
	ASSERT_EQ(dmodem_answer(a.m), 0);
	/* the TA's line: flags (more than the answer's delay takes), then
	   its SABM (01 3F) again and again,
	   each between flags, bit for bit as HDLC sends it */
	std::vector<unsigned char> bits;
	auto octet = [&](unsigned v) {
		for (int k = 0; k < 8; k++)
			bits.push_back(v >> k & 1);
	};
	auto frame = [&](std::vector<unsigned char> f) {
		unsigned fcs = fcs16(f.data(), f.size()) ^ 0xffff;
		f.push_back(static_cast<unsigned char>(fcs));
		f.push_back(static_cast<unsigned char>(fcs >> 8));
		int ones = 0;
		for (unsigned char o : f)
			for (int k = 0; k < 8; k++) {
				unsigned b = o >> k & 1;
				bits.push_back(static_cast<unsigned char>(b));
				ones = b ? ones + 1 : 0;
				if (ones == 5) {
					bits.push_back(0);
					ones = 0;
				}
			}
		octet(0x7e);
	};
	for (int i = 0; i < 400; i++)
		octet(0x7e);
	for (int i = 0; i < 40; i++) {
		frame({ 0x01, 0x3f });
		for (int j = 0; j < 200; j++)
			octet(0x7e);
	}
	std::vector<unsigned char> ta;
	for (size_t i = 0; i + 8 <= bits.size(); i += 8) {
		unsigned v = 0;
		for (int k = 0; k < 8; k++)
			v |= static_cast<unsigned>(bits[i + k]) << (7 - k);
		ta.push_back(static_cast<unsigned char>(v));
	}
	std::vector<unsigned char> out(160), in(160);
	std::vector<unsigned char> from_a;
	for (size_t at = 0; at + 160 <= ta.size(); at += 160) {
		std::copy(ta.begin() + static_cast<long>(at),
			  ta.begin() + static_cast<long>(at + 160), in.begin());
		dmodem_process_g711(a.m, in.data(), out.data(), 160);
		from_a.insert(from_a.end(), out.begin(), out.end());
		a.take();
	}
	EXPECT_NE(a.text.find("CONNECT 64000"), std::string::npos) << a.text;
	/* its UA (03 73) to the SABM */
	bool ua = false;
	for (const auto &f : hdlc_frames(from_a))
		ua = ua || (f.size() == 2 && f[0] == 0x01 && f[1] == 0x73);
	EXPECT_TRUE(ua);
}

/* V.120 (+B2=9): Q.921's frames for logical link 256 (address 08 01
   for the caller's commands, 0A 01 for the answerer's), set up with
   SABME, I frames in extended mode (two control octets) whose
   information starts with the terminal adaption header 0x83; the data
   through byte for byte, CONNECT 64000/V.120; DISC on hangup */
TEST(Clear, V120)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\\V1+MS=CLEAR;+B2=9\r"),
		a("answerer", "ATE0\\V1+MS=CLEAR;+B2=9\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 64000/V.120"), std::string::npos)
		<< c.text;
	EXPECT_NE(a.text.find("CONNECT 64000/V.120"), std::string::npos)
		<< a.text;
	ASSERT_TRUE(exchange(l, c, a, 20000, 10));
	auto fc = hdlc_frames(l.tap_ca), fa = hdlc_frames(l.tap_ac);
	ASSERT_GE(fc.size(), 2u);
	ASSERT_GE(fa.size(), 2u);
	EXPECT_EQ(fc[0], (std::vector<unsigned char>{ 0x08, 0x01, 0x7f }));
	EXPECT_EQ(fa[0], (std::vector<unsigned char>{ 0x08, 0x01, 0x73 }));
	bool ci = false, ai = false;
	for (auto &f : fc)
		if (f.size() > 5 && !(f[2] & 1)) {
			EXPECT_EQ(f[0], 0x08);
			EXPECT_EQ(f[1], 0x01);
			EXPECT_EQ(f[2], 0x00) << "N(S) of the first";
			EXPECT_EQ(f[4], 0x83) << "the header";
			ci = true;
			break;
		}
	for (auto &f : fa)
		if (f.size() > 5 && !(f[2] & 1)) {
			EXPECT_EQ(f[0], 0x0a);
			EXPECT_EQ(f[4], 0x83);
			ai = true;
			break;
		}
	EXPECT_TRUE(ci && ai);
	dmodem_hangup(c.m);
	run(l, c, a, 2, [&] { return a.no_carriers > 0; });
	EXPECT_EQ(a.no_carriers, 1) << a.text;
	bool disc = false;
	for (auto &f : hdlc_frames(l.tap_ca))
		disc = disc || (f.size() == 3 && f[0] == 0x08 &&
				(f[2] & ~0x10) == 0x43);
	EXPECT_TRUE(disc);
}

/* V.120 through a dropout of 0.3 s each way: N(S) and N(R) past 7 (the
   window wraps modulo 128), REJ and T200's polls bring the lost frames
   back, 100 kB each way intact */
TEST(Clear, V120Dropout)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3+MS=CLEAR;+B2=9\r"),
		a("answerer", "ATE0+MS=CLEAR;+B2=9\r");
	ASSERT_TRUE(call(l, c, a, 1)) << c.text << a.text;
	l.drop_at = static_cast<double>(l.t) / 8000 + 2;
	l.drop_s = 0.3;
	EXPECT_TRUE(exchange(l, c, a, 100000, 30)) << c.data.size() << " "
						   << a.data.size();
}

/* V.120 over V.110 at 19200 bit/s (+CBST=79,1, synchronous) */
TEST(Clear, V120OverV110)
{
	debug();
	Channel l;
	End c("caller", "ATE0X3\\V1+MS=CLEAR;+B2=9;+CBST=79,1\r"),
		a("answerer", "ATE0+MS=CLEAR;+B2=9;+CBST=79,1\r");
	ASSERT_TRUE(call(l, c, a, 5)) << c.text << a.text;
	EXPECT_NE(c.text.find("CONNECT 19200/V.120"), std::string::npos)
		<< c.text;
	EXPECT_TRUE(exchange(l, c, a, 5000, 10));
}
