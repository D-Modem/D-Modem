/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vwb_call_test.cpp - V.wb under the whole modem: two of libdmodem's
 *    modems at 96000 Hz on a simulated bare pair (vwb_line.h), AT&L1 (a
 *    leased line: ATD dials nothing, S0=1 answers with no ring), V.42
 *    and V.44 on top: bulk data both ways at once, byte for byte; a
 *    dropout the pump retrains through under LAPM; a long cut, NO
 *    CARRIER, and the call again by itself; the mode refused at 9600 Hz
 *
 *    VWB_DEBUG=<level>   libdmodem's log
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "portable.h"
#include "vwb_line.h"

namespace {

/* a DTE: a non-blocking socketpair, fd[0] the modem's end */
struct Dte {
	int fd[2];
	/* the pair */
	Dte()
	{
		if (dmd::stream_pair(fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			dmd::set_nonblocking(f);
	}
	/* both ends closed */
	~Dte()
	{
		dmd::fd_close(fd[0]);
		dmd::fd_close(fd[1]);
	}
	Dte(const Dte &) = delete;
	Dte &operator=(const Dte &) = delete;
	/* what the modem wrote */
	std::string read_all()
	{
		std::string s;
		char buf[4096];
		ssize_t n;
		while ((n = dmd::fd_read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, static_cast<size_t>(n));
		return s;
	}
	/* as much of s from at as the modem takes: how much */
	size_t write_some(const std::string &s, size_t at)
	{
		if (at >= s.size())
			return 0;
		ssize_t n = dmd::fd_write(fd[1], s.data() + at,
					  std::min<size_t>(s.size() - at, 4096));
		return n > 0 ? static_cast<size_t>(n) : 0;
	}
};

/* bulk data that does not compress, side's own */
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

/* one end: the modem and its DTE, what the DTE got in command state
   (results) and online (data) */
struct End {
	Dte dte;
	struct dmodem *m = nullptr;
	std::string text;               /* everything it wrote */
	std::string data;               /* since its last CONNECT */
	bool online = false;
	int connects = 0, no_carriers = 0;
	size_t sent = 0;
	/* the modem at rate, its first commands init */
	End(const char *name, const char *init, int32_t rate = 96000)
	{
		dmodem_config c{};
		c.name = name;
		c.dte_fd = dte.fd[0];
		c.init = init;
		m = dmodem_create_rate(&c, rate);
	}
	/* (see End()) */
	~End() { dmodem_destroy(m); }
	/* what the DTE has now: results counted, data once online */
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

/* the two ends on line for secs of line time (stop() true ends it
   sooner), each step of 5 ms */
template <class Stop>
void run(VwbLine &line, End &c, End &a, double secs, Stop stop)
{
	std::vector<s16> c_in, a_in, c_out, a_out;
	s64 end = line.time() + static_cast<s64>(secs * 96000);
	while (line.time() < end) {
		line.inputs(480, c_in, a_in);
		c_out.resize(c_in.size());
		a_out.resize(a_in.size());
		dmodem_process(c.m, c_in.data(), c_out.data(),
			       static_cast<int32_t>(c_in.size()));
		dmodem_process(a.m, a_in.data(), a_out.data(),
			       static_cast<int32_t>(a_in.size()));
		line.outputs(c_out, a_out);
		c.take();
		a.take();
		if (stop())
			break;
	}
}

/* the log level from VWB_DEBUG */
void debug()
{
	if (getenv("VWB_DEBUG"))
		dmodem_set_debug(atoi(getenv("VWB_DEBUG")));
}

} // namespace

/* a leased pair: CONNECT both ways (the answerer by S0=1, no ring),
   then 400 kB each way at once through V.42 and V.44, byte for byte, at
   about the pump's rate */
TEST(VwbCall, LapmBothWays)
{
	dmodem_init();
	debug();
	VwbLineConfig lc;
	lc.km = 1;
	VwbLine line(lc);
	End c("caller", "ATE0X4&L1+MS=VWB\r"), a("answerer",
					       "ATE0X4&L1S0=1+MS=VWB\r");
	ASSERT_TRUE(c.m && a.m);
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	run(line, c, a, 15, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online) << c.text;
	ASSERT_TRUE(a.online) << a.text;
	printf("caller: %s", c.text.c_str());
	printf("answerer: %s", a.text.c_str());
	struct dmodem_status st;
	dmodem_status(c.m, &st);
	EXPECT_GT(st.tx_rate, 100000u);
	EXPECT_GT(st.rx_rate, 100000u);

	const size_t n = 400000;
	std::string to_a = bulk(n, 1), to_c = bulk(n, 2);
	double t0 = static_cast<double>(line.time()) / 96000;
	run(line, c, a, 60, [&] {
		c.sent += c.dte.write_some(to_a, c.sent);
		a.sent += a.dte.write_some(to_c, a.sent);
		return a.data.size() >= n && c.data.size() >= n;
	});
	double t = static_cast<double>(line.time()) / 96000 - t0;
	EXPECT_EQ(a.data.size(), n);
	EXPECT_EQ(c.data.size(), n);
	EXPECT_TRUE(a.data == to_a) << "caller to answerer: bytes differ";
	EXPECT_TRUE(c.data == to_c) << "answerer to caller: bytes differ";
	printf("%zu bytes each way in %.1f s: %.0f bit/s each way through "
	       "V.42 (the pump %u/%u)\n", n, t, n * 8 / t, st.tx_rate,
	       st.rx_rate);
	EXPECT_GT(n * 8 / t, 0.7 * st.tx_rate);
}

/* the pair cut for half a second in the middle of the data: the pump
   retrains, LAPM sends again what was lost; no NO CARRIER, every byte */
TEST(VwbCall, RetrainUnderLapm)
{
	dmodem_init();
	debug();
	VwbLineConfig lc;
	lc.km = 1;
	lc.drop_at = 9;
	lc.drop_s = 0.5;
	VwbLine line(lc);
	End c("caller", "ATE0X4&L1+MS=VWB\r"), a("answerer",
					       "ATE0X4&L1S0=1+MS=VWB\r");
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	run(line, c, a, 15, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online && a.online);
	const size_t n = 300000;
	std::string to_a = bulk(n, 3);
	run(line, c, a, 60, [&] {
		c.sent += c.dte.write_some(to_a, c.sent);
		return a.data.size() >= n;
	});
	EXPECT_GT(line.time(), 10 * 96000) << "done before the cut";
	EXPECT_EQ(a.data.size(), n);
	EXPECT_TRUE(a.data == to_a) << "bytes differ";
	EXPECT_EQ(c.no_carriers + a.no_carriers, 0) << c.text << a.text;
	EXPECT_EQ(c.connects, 1);
}

/* AT&L1: a cut longer than S7 (10 s): NO CARRIER at both ends, then
   each starts the call again by itself (the caller dials, the answerer
   answers), CONNECT again once the pair is back */
TEST(VwbCall, LeasedLineCallsAgain)
{
	dmodem_init();
	debug();
	VwbLineConfig lc;
	lc.km = 1;
	lc.drop_at = 12;
	lc.drop_s = 16;
	VwbLine line(lc);
	End c("caller", "ATE0X4&L1S7=10+MS=VWB\r"),
		a("answerer", "ATE0X4&L1S0=1S7=10+MS=VWB\r");
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	run(line, c, a, 15, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online && a.online) << "no first call";
	run(line, c, a, 60, [&] { return c.connects >= 2 && a.connects >= 2; });
	EXPECT_GE(c.no_carriers, 1) << c.text;
	EXPECT_GE(a.no_carriers, 1) << a.text;
	EXPECT_GE(c.connects, 2) << c.text;
	EXPECT_GE(a.connects, 2) << a.text;
	printf("caller: %s\nanswerer: %s\n", c.text.c_str(), a.text.c_str());
	/* and data on the new call */
	std::string msg = "after the cut\r\n";
	c.dte.write_some(msg, 0);
	run(line, c, a, 5, [&] { return a.data.find(msg) != std::string::npos; });
	EXPECT_NE(a.data.find(msg), std::string::npos);
}

/* V.wb wants 48000 Hz and more: at 9600 the pump is not made (NO
   CARRIER) */
TEST(VwbCall, RefusedAt9600)
{
	dmodem_init();
	debug();
	Dte d;
	dmodem_config cfg{};
	cfg.name = "modem";
	cfg.dte_fd = d.fd[0];
	cfg.init = "ATE0X4&L1+MS=VWB\r";
	struct dmodem *m = dmodem_create_rate(&cfg, 9600);
	ASSERT_NE(m, nullptr);
	d.read_all();
	ASSERT_EQ(dmodem_dial(m, ""), 0);
	std::vector<short> in(192), out(192);
	std::string got;
	for (int i = 0; i < 200 && got.find("NO CARRIER") ==
	     std::string::npos; i++) {
		dmodem_process(m, in.data(), out.data(), 192);
		dmodem_poll_dte(m);
		got += d.read_all();
	}
	EXPECT_NE(got.find("NO CARRIER"), std::string::npos) << got;
	dmodem_destroy(m);
}
