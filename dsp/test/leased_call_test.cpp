/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    leased_call_test.cpp - experiment
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "portable.h"

namespace {

struct Dte {
	int fd[2];
	Dte()
	{
		if (dmd::stream_pair(fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			dmd::set_nonblocking(f);
	}
	~Dte()
	{
		dmd::fd_close(fd[0]);
		dmd::fd_close(fd[1]);
	}
	std::string read_all()
	{
		std::string s;
		char buf[4096];
		ssize_t n;
		while ((n = dmd::fd_read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, static_cast<size_t>(n));
		return s;
	}
	size_t write_some(const std::string &s, size_t at)
	{
		if (at >= s.size())
			return 0;
		ssize_t n = dmd::fd_write(fd[1], s.data() + at,
					  std::min<size_t>(s.size() - at, 4096));
		return n > 0 ? static_cast<size_t>(n) : 0;
	}
};

struct End {
	Dte dte;
	struct dmodem *m = nullptr;
	std::string text, data;
	bool online = false;
	int connects = 0, no_carriers = 0;
	size_t sent = 0;
	End(const char *name, const char *init, int32_t rate)
	{
		dmodem_config c{};
		c.name = name;
		c.dte_fd = dte.fd[0];
		c.init = init;
		m = dmodem_create_rate(&c, rate);
	}
	~End() { dmodem_destroy(m); }
	void take()
	{
		dmodem_poll_dte(m);
		std::string s = dte.read_all();
		if (s.empty())
			return;
		text += s;
		if (!online) {
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
};

/* a voice band line between the two: each way delay samples, the far
   end's signal at gain, each end's own at echo (2-wire: its hybrid's
   leak; 4-wire: none), white noise */
struct Line {
	double gain = 0.5, echo = 0, noise = 3;
	size_t delay = 96;
	std::deque<s16> ca, ac, ce, ae;
	std::mt19937 rng{1};
	std::normal_distribution<float> g{0, 1};
	double drop_at = -1, drop_s = 0;
	long t = 0;
	s32 rate;
	explicit Line(s32 r) : rate(r) {}
	s16 mix(s16 far, s16 own)
	{
		double s = static_cast<double>(t) / rate;
		if (s >= drop_at && s < drop_at + drop_s)
			far = 0;
		double v = gain * far + echo * own + noise * g(rng);
		return static_cast<s16>(std::lround(std::max(-32767.0,
					std::min(32767.0, v))));
	}
};

template <class Stop>
void run(Line &l, End &c, End &a, double secs, Stop stop)
{
	const s32 n = l.rate / 200;
	std::vector<s16> ci(n), ai(n), co(n), ao(n);
	long end = l.t + static_cast<long>(secs * l.rate);
	if (l.ca.empty()) {
		l.ca.assign(l.delay, 0);
		l.ac.assign(l.delay, 0);
		l.ce.assign(l.delay, 0);
		l.ae.assign(l.delay, 0);
	}
	while (l.t < end) {
		for (s32 i = 0; i < n; i++) {
			ci[i] = l.mix(l.ac.front(), l.ce.front());
			ai[i] = l.mix(l.ca.front(), l.ae.front());
			l.ac.pop_front(); l.ce.pop_front();
			l.ca.pop_front(); l.ae.pop_front();
			l.t++;
		}
		dmodem_process(c.m, ci.data(), co.data(), n);
		dmodem_process(a.m, ai.data(), ao.data(), n);
		for (s32 i = 0; i < n; i++) {
			l.ca.push_back(co[i]);
			l.ce.push_back(co[i]);
			l.ac.push_back(ao[i]);
			l.ae.push_back(ao[i]);
		}
		c.take();
		a.take();
		if (stop())
			break;
	}
}

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

} // namespace

class Leased : public ::testing::TestWithParam<const char *> {};

TEST_P(Leased, Connects)
{
	dmodem_init();
	if (getenv("LEASED_DEBUG"))
		dmodem_set_debug(atoi(getenv("LEASED_DEBUG")));
	std::string ms = GetParam();
	Line l(9600);
	l.echo = getenv("LEASED_ECHO") ? atof(getenv("LEASED_ECHO")) : 0.1;
	std::string ci = "ATE0X4&L1" + ms + "\r", ai = "ATE0X4&L1S0=1" + ms + "\r";
	End c("caller", ci.c_str(), 9600), a("answerer", ai.c_str(), 9600);
	ASSERT_TRUE(c.m && a.m);
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	run(l, c, a, 40, [&] { return c.online && a.online; });
	printf("%s: %.1f s\ncaller: %s\nanswerer: %s\n", ms.c_str(),
	       l.t / 9600.0, c.text.c_str(), a.text.c_str());
	ASSERT_TRUE(c.online && a.online);
	struct dmodem_status st;
	dmodem_status(c.m, &st);
	/* some 5 s of data */
	const size_t n = std::max<size_t>(200, std::min(st.tx_rate, st.rx_rate) * 5 / 10);
	std::string to_a = bulk(n, 1), to_c = bulk(n, 2);
	run(l, c, a, 30, [&] {
		c.sent += c.dte.write_some(to_a, c.sent);
		a.sent += a.dte.write_some(to_c, a.sent);
		return a.data.size() >= n && c.data.size() >= n;
	});
	EXPECT_TRUE(a.data == to_a);
	EXPECT_TRUE(c.data == to_c);
}

INSTANTIATE_TEST_SUITE_P(Mods, Leased, ::testing::Values(
	"", "+MS=34", "+MS=132", "+MS=32", "+MS=122", "+MS=22", "+MS=212",
	"+MS=21", "+MS=23", "+MS=103"));
