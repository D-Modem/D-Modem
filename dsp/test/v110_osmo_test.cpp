/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v110_osmo_test.cpp - D-Modem's V.110 (AT+MS=CLEAR with AT+CBST)
 *    against libosmocore's, an implementation of its own (Osmocom's,
 *    which GSM's circuit switched data runs on): a libdmodem modem on one
 *    end of a simulated clear channel, libosmocore's RA1 and frame coding
 *    on the other (osmo_v110_peer.c), with RA2 here (the intermediate
 *    rate's bits first in each octet, the first the most significant,
 *    the rest ones, as Osmocom's csd_ra2 has it).  Each end sends a line
 *    of text as asynchronous characters (start bit, 8 data bits, stop
 *    bit; \N0 at the modem); each must get the other's.
 *
 *    Osmocom's frames to D-Modem as the answerer with no rate of its own
 *    (+CBST=0): every user rate libosmocore has, which D-Modem must find
 *    by the frames' E bits.  D-Modem as the caller with each +CBST rate
 *    libosmocore has: its frames decoded by libosmocore, whose RA1
 *    refuses frames with the wrong E1-E3.
 *
 *    Built when OSMOCORE_SRC names a libosmocore source tree (1.14 or
 *    later); V110_DEBUG=<level>: libdmodem's log
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "osmo_v110_peer.h"
#include "portable.h"

namespace {

/* one libdmodem modem with a DTE on a socket pair */
struct Modem {
	int fd[2];
	struct dmodem *m = nullptr;
	std::string text;
	/* the modem with its init line */
	explicit Modem(const char *init)
	{
		if (dmd::stream_pair(fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			dmd::set_nonblocking(f);
		dmodem_config c{};
		c.name = "dmodem";
		c.dte_fd = fd[0];
		c.init = init;
		c.law = DMODEM_LAW_ALAW;
		m = dmodem_create(&c);
	}
	/* (the modem destroyed, the sockets closed) */
	~Modem()
	{
		dmodem_destroy(m);
		dmd::fd_close(fd[0]);
		dmd::fd_close(fd[1]);
	}
	/* what it wrote to the DTE */
	void take()
	{
		dmodem_poll_dte(m);
		char buf[4096];
		ssize_t n;
		while ((n = dmd::fd_read(fd[1], buf, sizeof(buf))) > 0)
			text.append(buf, static_cast<size_t>(n));
	}
	/* s to it from the DTE */
	void send(const std::string &s)
	{
		dmd::fd_write(fd[1], s.data(), s.size());
	}
};

/* s as asynchronous characters: start bit, 8 data bits (the least
   significant first), stop bit */
std::vector<unsigned char> async_bits(const std::string &s)
{
	std::vector<unsigned char> b;
	for (unsigned char c : s) {
		b.push_back(0);
		for (int k = 0; k < 8; k++)
			b.push_back(c >> k & 1);
		b.push_back(1);
	}
	return b;
}

/* the characters in asynchronous bits (from each start bit, those with
   a good stop bit) */
std::string async_text(const std::vector<unsigned char> &b)
{
	std::string s;
	for (size_t i = 0; i + 10 <= b.size();) {
		if (b[i]) {
			i++;
			continue;
		}
		if (!b[i + 9]) {
			i++;
			continue;
		}
		unsigned c = 0;
		for (int k = 0; k < 8; k++)
			c |= static_cast<unsigned>(b[i + 1 + k]) << k;
		s += static_cast<char>(c);
		i += 10;
	}
	return s;
}

/* libosmocore's end at its rate index i: its frames out (ones as user
   data, then its text), D-Modem's frames in (decoded once it finds
   their alignment) */
struct Osmo {
	int i, rate, ir, chunk, k;
	std::vector<unsigned char> tx_user;     /* user bits still to send */
	std::vector<unsigned char> tx_bits;     /* intermediate rate bits */
	std::vector<unsigned char> rx_bits, rx_user;
	size_t rx_at = 0;
	bool rx_sync = false;
	int frames = 0, refused = 0, sx_on = 0;

	/* libosmocore's rate i: its user rate, intermediate rate, frame's
	   user bits */
	explicit Osmo(int i_) : i(i_)
	{
		rate = osmo_peer_rate(i, &ir, &chunk);
		k = ir / 8000;
	}
	/* an octet to send */
	unsigned char octet()
	{
		while (tx_bits.size() < static_cast<size_t>(k)) {
			std::vector<unsigned char> u(static_cast<size_t>(chunk), 1);
			for (int n = 0; n < chunk && !tx_user.empty(); n++) {
				u[static_cast<size_t>(n)] = tx_user.front();
				tx_user.erase(tx_user.begin());
			}
			unsigned char f[80];
			if (osmo_peer_encode(i, u.data(), chunk, f))
				throw std::runtime_error("osmo_peer_encode");
			tx_bits.insert(tx_bits.end(), f, f + 80);
		}
		unsigned v = 0xff;
		for (int b = 0; b < k; b++)
			if (!tx_bits[static_cast<size_t>(b)])
				v &= ~(0x80u >> b);
		tx_bits.erase(tx_bits.begin(), tx_bits.begin() + k);
		return static_cast<unsigned char>(v);
	}
	/* the 80 bits at at are an alignment pattern */
	bool pattern(size_t at) const
	{
		for (size_t b = 0; b < 80; b++) {
			unsigned v = rx_bits[at + b];
			if (b < 8 ? v != 0 : (b % 8 == 0 && v != 1))
				return false;
		}
		return true;
	}
	/* an octet received */
	void receive(unsigned char o)
	{
		for (int b = 0; b < k; b++)
			rx_bits.push_back(o >> (7 - b) & 1);
		while (rx_at + 80 <= rx_bits.size()) {
			if (!rx_sync) {
				if (!pattern(rx_at)) {
					rx_at++;
					continue;
				}
				rx_sync = true;
			}
			unsigned char u[48];
			int on = 0;
			int n = osmo_peer_decode(i, rx_bits.data() + rx_at, u,
						 48, &on);
			rx_at += 80;
			if (n < 0) {
				refused++;
				continue;
			}
			frames++;
			if (on) {
				sx_on++;
				rx_user.insert(rx_user.end(), u, u + n);
			}
		}
	}
};

/* the modem and Osmocom's end on a clear channel for secs, each end's
   text sent once the modem has connected */
void run(Modem &d, Osmo &o, const std::string &to_osmo,
	 const std::string &to_modem, double secs)
{
	const int n = 160;
	std::vector<unsigned char> in(n), out(n);
	bool sent = false;
	for (long t = 0; t < static_cast<long>(secs * 8000); t += n) {
		for (int j = 0; j < n; j++)
			in[static_cast<size_t>(j)] = o.octet();
		dmodem_process_g711(d.m, in.data(), out.data(), n);
		for (unsigned char v : out)
			o.receive(v);
		d.take();
		if (!sent && d.text.find("CONNECT") != std::string::npos) {
			sent = true;
			/* (a second of ones first: the modem's link
			   starting) */
			o.tx_user.assign(static_cast<size_t>(o.rate), 1);
			std::vector<unsigned char> b = async_bits(to_modem);
			o.tx_user.insert(o.tx_user.end(), b.begin(), b.end());
			d.send(to_osmo);
		}
	}
}

/* the log level from V110_DEBUG */
void debug()
{
	dmodem_init();
	if (getenv("V110_DEBUG"))
		dmodem_set_debug(atoi(getenv("V110_DEBUG")));
}

/* +CBST's <speed> for a user rate, 0 for none */
int cbst_speed(int rate)
{
	switch (rate) {
	case 1200: return 66;
	case 2400: return 68;
	case 4800: return 70;
	case 9600: return 71;
	case 14400: return 75;
	case 19200: return 79;
	case 28800: return 80;
	case 38400: return 81;
	default: return 0;
	}
}

} // namespace

/* libosmocore calls at each of its rates; D-Modem answers with no rate
   of its own, finds it, and the text goes both ways */
TEST(V110Osmo, OsmocomCallsEachRate)
{
	debug();
	for (int i = 0; i < osmo_peer_rates(); i++) {
		Osmo o(i);
		SCOPED_TRACE(o.rate);
		Modem d("ATE0\\N0+MS=CLEAR\r");
		ASSERT_EQ(dmodem_answer(d.m), 0);
		std::string to_osmo = "from D-Modem at " +
			std::to_string(o.rate) + "\r\n";
		std::string to_modem = "from Osmocom at " +
			std::to_string(o.rate) + "\r\n";
		run(d, o, to_osmo, to_modem,
		    3 + 40.0 * (to_osmo.size() + 10) / o.rate);
		EXPECT_NE(d.text.find("CONNECT " + std::to_string(o.rate)),
			  std::string::npos) << d.text;
		EXPECT_NE(d.text.find(to_modem), std::string::npos) << d.text;
		EXPECT_EQ(o.refused, 0);
		EXPECT_GT(o.sx_on, 0);
		EXPECT_NE(async_text(o.rx_user).find(to_osmo),
			  std::string::npos) << async_text(o.rx_user);
	}
}

/* D-Modem calls at each +CBST rate libosmocore has; libosmocore's RA1
   takes its frames (E1-E3 right) and the text goes both ways */
TEST(V110Osmo, DModemCallsEachRate)
{
	debug();
	for (int i = 0; i < osmo_peer_rates(); i++) {
		Osmo o(i);
		int speed = cbst_speed(o.rate);
		if (!speed)
			continue;
		SCOPED_TRACE(o.rate);
		std::string init = "ATE0X3\\N0+MS=CLEAR;+CBST=" +
			std::to_string(speed) + "\r";
		Modem d(init.c_str());
		ASSERT_EQ(dmodem_dial(d.m, "5551000"), 0);
		std::string to_osmo = "D-Modem calling at " +
			std::to_string(o.rate) + "\r\n";
		std::string to_modem = "Osmocom answering at " +
			std::to_string(o.rate) + "\r\n";
		run(d, o, to_osmo, to_modem,
		    3 + 40.0 * (to_osmo.size() + 10) / o.rate);
		EXPECT_NE(d.text.find("CONNECT " + std::to_string(o.rate)),
			  std::string::npos) << d.text;
		EXPECT_NE(d.text.find(to_modem), std::string::npos) << d.text;
		EXPECT_EQ(o.refused, 0);
		EXPECT_GT(o.frames, 0);
		EXPECT_NE(async_text(o.rx_user).find(to_osmo),
			  std::string::npos) << async_text(o.rx_user);
	}
}
