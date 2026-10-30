/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    capi_test.cpp - dmodem-capid's CAPI controller on a D-Modem: two
 *    of libdmodem's modems on a simulated line, the controller on A's
 *    DTE with an application of the test's, B driven by the test as a
 *    terminal adapter would be.  Calls out and in on a clear channel
 *    (X.75; V.120; frames in sync PPP's framing; V.110; the caller's
 *    number and a second application that loses the call), a modem
 *    call (B1 7), data both ways, hangups from either end, a call
 *    nobody answers, a call that stops ringing, a B protocol the
 *    controller does not have.  Then the remote CAPI server over TCP, in libcapi20's
 *    rcapi protocol, with a modem of the test's.
 *
 *    CAPI_DEBUG=<level>   libdmodem's log
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "capi_controller.h"
#include "dmodem.h"
#include "portable.h"
#include "rcapi_server.h"

using namespace capi;

namespace {

/* the log level from CAPI_DEBUG */
void debug()
{
	dmodem_init();
	if (getenv("CAPI_DEBUG"))
		dmodem_set_debug(atoi(getenv("CAPI_DEBUG")));
}

/* a socket pair, both ends non-blocking */
struct Dte {
	int fd[2];
	/* (made) */
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
	/* what the modem wrote so far */
	std::string read_all()
	{
		std::string s;
		char buf[4096];
		ssize_t n;
		while ((n = dmd::fd_read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, static_cast<size_t>(n));
		return s;
	}
};

/* B: a modem the test drives, its DTE's text and its data online */
struct End {
	Dte dte;
	struct dmodem *m = nullptr;
	std::string text, data, out;
	size_t sent = 0;
	bool online = false;
	int no_carriers = 0;
	/* a modem with its init line */
	End(const char *init, bool clear, dmodem_law law)
	{
		dmodem_config c{};
		c.name = "B";
		c.dte_fd = dte.fd[0];
		c.init = init;
		c.law = law;
		c.clear = clear;
		m = dmodem_create(&c);
	}
	/* (the modem destroyed) */
	~End() { dmodem_destroy(m); }
	/* the DTE's text: CONNECT, data, NO CARRIER; out to the modem */
	void take()
	{
		dmodem_poll_dte(m);
		std::string s = dte.read_all();
		if (sent < out.size()) {
			ssize_t n = dmd::fd_write(dte.fd[1], out.data() + sent,
						  std::min<size_t>(out.size() - sent,
								   4096));
			if (n > 0)
				sent += static_cast<size_t>(n);
		}
		if (s.empty())
			return;
		text += s;
		if (!online) {
			size_t at = s.find("CONNECT");
			if (at == std::string::npos)
				return;
			size_t e = s.find("\r\n", at);
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

/* the test's application: what the controller sends it, its data in;
   the responses an application owes sent at once */
struct Bench : Controller::Out {
	Dte dte;                        /* A's */
	struct dmodem *a = nullptr;
	End b;
	Controller ctrl;
	long t = 0;                     /* samples */
	std::deque<unsigned char> ab, ba;
	std::deque<Msg> got;
	std::string rx;                 /* DATA_B3_IND's data */
	std::vector<std::vector<u8>> rx_blocks;
	std::vector<u16> confs;         /* DATA_B3_CONF's handles */
	u16 num = 100;
	bool auto_b3_resp = true;

	/* A (clear: a clear channel's line, as SIP's CLEARMODE) and B with
	   its init; extra after the controller's setup */
	Bench(const char *b_init, bool clear = false,
	      dmodem_law law = DMODEM_LAW_ALAW, const std::string &extra = "")
		: b(b_init, clear, law), ctrl(*this, extra)
	{
		dmodem_config c{};
		c.name = "A";
		c.dte_fd = dte.fd[0];
		c.law = law;
		c.clear = clear;
		a = dmodem_create(&c);
		ab.assign(240, 0xD5);
		ba.assign(240, 0xD5);
		ctrl.add_app(1, 7, 2048);
	}
	/* (A destroyed) */
	~Bench() { dmodem_destroy(a); }

	/* the controller's clock: the samples' */
	u64 now() const { return static_cast<u64>(t) * 1000 / 8000; }

	/* see Controller::Out */
	void to_app(const Msg &m) override
	{
		if (m.cmd == DATA_B3 && m.sub == IND) {
			rx.append(m.data.begin(), m.data.end());
			rx_blocks.push_back(m.data);
			Reader r(m.params);
			r.d();
			r.d();
			r.w();
			u16 h = r.w();
			std::vector<u8> p;
			Writer(p).d(NCCI).w(h);
			resp(m.appl, DATA_B3, p);
			return;
		}
		if (m.cmd == DATA_B3 && m.sub == CONF) {
			Reader r(m.params);
			r.d();
			confs.push_back(r.w());
			return;
		}
		if (m.sub == IND) {
			Reader r(m.params);
			std::vector<u8> p;
			Writer(p).d(r.d());
			if (m.cmd == CONNECT_ACTIVE ||
			    m.cmd == CONNECT_B3_ACTIVE ||
			    m.cmd == DISCONNECT_B3 || m.cmd == DISCONNECT)
				resp(m.appl, m.cmd, p);
		}
		got.push_back(m);
	}
	/* see Controller::Out */
	size_t to_modem(const u8 *p, size_t n) override
	{
		ssize_t w = dmd::fd_write(dte.fd[1], p, static_cast<int>(n));
		return w > 0 ? static_cast<size_t>(w) : 0;
	}

	/* a RESP of appl's (after the IND being handled) */
	void resp(u16 appl, u8 cmd, const std::vector<u8> &p)
	{
		Msg m;
		m.appl = appl;
		m.cmd = cmd;
		m.sub = RESP;
		m.num = num++;
		m.params = p;
		pending_.push_back(m);
	}
	/* a message of appl's to the controller now */
	void send(u8 cmd, u8 sub, const std::vector<u8> &p,
		  const std::vector<u8> &data = {}, u16 appl = 1)
	{
		Msg m;
		m.appl = appl;
		m.cmd = cmd;
		m.sub = sub;
		m.num = num++;
		m.params = p;
		m.data = data;
		ctrl.message(m, now());
		flush();
	}
	/* the RESPs owed, sent */
	void flush()
	{
		while (!pending_.empty()) {
			Msg m = pending_.front();
			pending_.pop_front();
			ctrl.message(m, now());
		}
	}

	/* one block of the line (20 ms) and what follows from it */
	void step()
	{
		const int32_t n = 160;
		unsigned char ai[n], bi[n], ao[n], bo[n];
		for (int32_t i = 0; i < n; i++) {
			ai[i] = ba.front();
			bi[i] = ab.front();
			ba.pop_front();
			ab.pop_front();
		}
		t += n;
		dmodem_process_g711(a, ai, ao, n);
		dmodem_process_g711(b.m, bi, bo, n);
		ab.insert(ab.end(), ao, ao + n);
		ba.insert(ba.end(), bo, bo + n);
		if (ctrl.wants_modem_input()) {
			dmodem_poll_dte(a);
			char buf[4096];
			ssize_t k;
			while ((k = dmd::fd_read(dte.fd[1], buf,
						 sizeof(buf))) > 0)
				ctrl.modem_input(reinterpret_cast<u8 *>(buf),
						 static_cast<size_t>(k), now());
		}
		ctrl.poll(now());
		flush();
		b.take();
	}
	/* secs of the line, or until stop() */
	void run(double secs, const std::function<bool()> &stop = nullptr)
	{
		long end = t + static_cast<long>(secs * 8000);
		while (t < end) {
			step();
			if (stop && stop())
				return;
		}
	}
	/* the next message cmd/sub to appl (any: 0) within secs, taken
	   from those got; an empty one (cmd 0) if none came */
	Msg expect(u8 cmd, u8 sub, double secs, u16 appl = 0)
	{
		Msg found;
		auto look = [&] {
			for (auto it = got.begin(); it != got.end(); ++it)
				if (it->cmd == cmd && it->sub == sub &&
				    (!appl || it->appl == appl)) {
					found = *it;
					got.erase(it);
					return true;
				}
			return false;
		};
		if (!look())
			run(secs, look);
		return found;
	}
	/* until the controller has set the modem up */
	bool ready()
	{
		run(3, [this] { return ctrl.ready(); });
		return ctrl.ready();
	}
private:
	std::deque<Msg> pending_;
};

/* a B protocol struct: B1, B2, B3 and B1's configuration */
std::vector<u8> bprot(u16 b1, u16 b2, u16 b3,
		      const std::vector<u8> &b1cfg = {})
{
	std::vector<u8> p;
	Writer(p).w(b1).w(b2).w(b3).s(b1cfg).empty().empty();
	return p;
}

/* CONNECT_REQ's parameters: controller 1, cip, the number, bp */
std::vector<u8> connect_req(u16 cip, const std::string &number,
			    const std::vector<u8> &bp)
{
	std::vector<u8> called = { 0x80 };
	called.insert(called.end(), number.begin(), number.end());
	std::vector<u8> p;
	Writer(p).d(1).w(cip).s(called).empty().empty().empty().s(bp)
		.empty().empty().empty().empty();
	return p;
}

/* LISTEN_REQ's parameters: controller 1, the CIP mask */
std::vector<u8> listen_req(u32 cip_mask)
{
	std::vector<u8> p;
	Writer(p).d(1).d(0).d(cip_mask).d(0).empty().empty();
	return p;
}

/* a message's first dword and the word after it (PLCI and info) */
std::pair<u32, u16> ident_info(const Msg &m)
{
	Reader r(m.params);
	u32 d = r.d();
	return { d, r.w() };
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

/* data the application sends in blocks of up to len, at most 7
   unconfirmed, until B has it all or secs pass; and B's to it */
bool exchange(Bench &t, size_t n, double secs, size_t len = 512)
{
	std::string to_b = bulk(n, 1), to_a = bulk(n, 2);
	t.b.data.clear();
	t.b.out = to_a;
	t.b.sent = 0;
	t.rx.clear();
	t.confs.clear();
	size_t off = 0;
	u16 handle = 0;
	t.run(secs, [&] {
		while (off < to_b.size() && handle - t.confs.size() < 7) {
			size_t k = std::min(len, to_b.size() - off);
			std::vector<u8> d(to_b.begin() + off,
					  to_b.begin() + off + k);
			std::vector<u8> p;
			Writer(p).d(NCCI).d(0).w(static_cast<u16>(k))
				.w(handle++).w(0);
			t.send(DATA_B3, REQ, p, d);
			off += k;
		}
		return t.b.data.size() >= n && t.rx.size() >= n;
	});
	EXPECT_EQ(t.b.data.size(), n);
	EXPECT_EQ(t.rx.size(), n);
	EXPECT_EQ(t.confs.size(), handle);
	return t.b.data == to_b && t.rx == to_a;
}

/* a call out from the application with bp, answered by B: up to
   CONNECT_B3_ACTIVE_IND */
void call_out(Bench &t, const std::vector<u8> &bp, double secs = 3)
{
	ASSERT_TRUE(t.ready());
	t.send(CONNECT, REQ, connect_req(CIP_UDI, "5551000", bp));
	Msg c = t.expect(CONNECT, CONF, 0);
	ASSERT_EQ(c.cmd, CONNECT);
	EXPECT_EQ(ident_info(c), std::make_pair(u32(PLCI), u16(0)));
	t.run(0.2);
	ASSERT_EQ(dmodem_answer(t.b.m), 0);
	Msg a = t.expect(CONNECT_ACTIVE, IND, secs);
	ASSERT_EQ(a.cmd, CONNECT_ACTIVE) << t.b.text;
	std::vector<u8> p;
	Writer(p).d(PLCI).empty();
	t.send(CONNECT_B3, REQ, p);
	Msg b3 = t.expect(CONNECT_B3, CONF, 0);
	EXPECT_EQ(ident_info(b3), std::make_pair(u32(NCCI), u16(0)));
	ASSERT_EQ(t.expect(CONNECT_B3_ACTIVE, IND, 0).cmd, CONNECT_B3_ACTIVE);
	t.run(5, [&] { return t.b.online; });
	ASSERT_TRUE(t.b.online) << t.b.text;
}

/* the application hangs up: DISCONNECT_B3_IND, DISCONNECT_IND with
   reason 0, and B's NO CARRIER if its link tells (X.75's DISC, V.42's;
   not PPP's nor plain characters', which the line's end ends) */
void hang_up(Bench &t, bool b_sees_it = true)
{
	std::vector<u8> p;
	Writer(p).d(PLCI).empty();
	t.send(DISCONNECT, REQ, p);
	EXPECT_EQ(ident_info(t.expect(DISCONNECT, CONF, 0)),
		  std::make_pair(u32(PLCI), u16(0)));
	EXPECT_EQ(t.expect(DISCONNECT_B3, IND, 0).cmd, DISCONNECT_B3);
	Msg d = t.expect(DISCONNECT, IND, 5);
	ASSERT_EQ(d.cmd, DISCONNECT);
	EXPECT_EQ(ident_info(d).second, 0);
	EXPECT_FALSE(t.ctrl.busy());
	if (!b_sees_it) {
		dmodem_hangup(t.b.m);
		t.b.online = false;
		return;
	}
	t.run(3, [&] { return !t.b.online; });
	EXPECT_FALSE(t.b.online) << t.b.text.substr(0, 200);
}

/* the FCS-16 of n octets */
unsigned fcs16(const unsigned char *p, size_t n, unsigned fcs = 0xffff)
{
	while (n--) {
		fcs ^= *p++;
		for (int i = 0; i < 8; i++)
			fcs = fcs & 1 ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
	}
	return fcs;
}

/* a frame in RFC 1662's async framing (control characters escaped) */
std::string async_frame(const std::vector<unsigned char> &f)
{
	std::vector<unsigned char> all = f;
	unsigned fcs = ~fcs16(f.data(), f.size()) & 0xffff;
	all.push_back(static_cast<unsigned char>(fcs));
	all.push_back(static_cast<unsigned char>(fcs >> 8));
	std::string s = "\x7e";
	for (unsigned char c : all) {
		if (c == 0x7e || c == 0x7d || c < 0x20) {
			s += '\x7d';
			c ^= 0x20;
		}
		s += static_cast<char>(c);
	}
	return s + "\x7e";
}

/* the good frames in async framing in s */
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
		} else if (c == 0x7d) {
			esc = true;
		} else {
			f.push_back(esc ? c ^ 0x20 : c);
			esc = false;
		}
	}
	return frames;
}

/* a PPP frame of n octets, every value from seed (7Eh and 7Dh too) */
std::vector<unsigned char> ppp_frame(size_t n, unsigned seed)
{
	std::vector<unsigned char> f = { 0xff, 0x03, 0x00, 0x21 };
	for (size_t k = 4; k < n; k++)
		f.push_back(static_cast<unsigned char>(k * 7 + seed));
	return f;
}

} // namespace

/* the B protocols in AT commands, and those it has not */
TEST(Capi, Modes)
{
	EXPECT_EQ(mode_of({}).at, "+MS=CLEAR;+CBST=0;+B2=0");
	EXPECT_EQ(mode_of(bprot(0, 0, 0)).at, "+MS=CLEAR;+CBST=0;+B2=0");
	Mode ppp = mode_of(bprot(0, 1, 0));
	EXPECT_EQ(ppp.at, "+MS=CLEAR;+CBST=0;+B2=5");
	EXPECT_TRUE(ppp.frames);
	EXPECT_EQ(mode_of(bprot(0, 9, 0)).at, "+MS=CLEAR;+CBST=0;+B2=9");
	std::vector<u8> cfg;
	Writer(cfg).w(9600).w(8).w(0).w(0);
	EXPECT_EQ(mode_of(bprot(2, 1, 0, cfg)).at,
		  "\\N0+MS=CLEAR;+CBST=71,0;+B2=7");
	EXPECT_EQ(mode_of(bprot(3, 0, 0, cfg)).at,
		  "+MS=CLEAR;+CBST=71,1;+B2=0");
	EXPECT_EQ(mode_of(bprot(7, 7, 0)).at, "\\N3");
	EXPECT_FALSE(mode_of(bprot(7, 7, 0)).clear);
	EXPECT_EQ(mode_of(bprot(8, 1, 0)).at, "\\N0");
	EXPECT_EQ(mode_of(bprot(4, 4, 4)).info, B1_NOT_SUPPORTED);
	EXPECT_EQ(mode_of(bprot(0, 3, 0)).info, B2_NOT_SUPPORTED);
	EXPECT_EQ(mode_of(bprot(0, 0, 2)).info, B3_NOT_SUPPORTED);
	std::vector<u8> odd;
	Writer(odd).w(7200).w(8).w(0).w(0);
	EXPECT_EQ(mode_of(bprot(2, 1, 0, odd)).info, B1_PARAM_NOT_SUPPORTED);
	std::vector<u8> prof = profile();
	ASSERT_EQ(prof.size(), 64u);
	Reader r(prof);
	EXPECT_EQ(r.w(), 1);
	EXPECT_EQ(r.w(), 1);
	r.d();
	EXPECT_EQ(r.d(), u32(PROFILE_B1));
}

/* a call out over X.75 (B1 0, B2 0, B3 0): 20 kB each way, the
   application hangs up */
TEST(Capi, CallOutX75)
{
	debug();
	Bench t("ATE0+MS=CLEAR\r");
	call_out(t, bprot(0, 0, 0));
	EXPECT_NE(t.b.text.find("CONNECT 64000"), std::string::npos);
	EXPECT_TRUE(exchange(t, 20000, 20));
	hang_up(t);
	/* and a second call on the same controller */
	t.b.text.clear();
	call_out(t, {});
	EXPECT_TRUE(exchange(t, 2000, 10));
	hang_up(t);
}

/* V.120 (B1 0, B2 9): the same */
TEST(Capi, CallOutV120)
{
	debug();
	Bench t("ATE0\\V1+MS=CLEAR;+B2=9\r");
	call_out(t, bprot(0, 9, 0));
	EXPECT_NE(t.b.text.find("CONNECT 64000/V.120"), std::string::npos)
		<< t.b.text.substr(0, 200);
	EXPECT_TRUE(exchange(t, 5000, 10));
	hang_up(t);
}

/* a call in on a clear channel: CONNECT_IND with CIP 2 and the
   caller's number to both listening applications; the first to take it
   has it, the other a DISCONNECT_IND; CONNECT_B3_IND; data; the far end
   hangs up (reason 0x3490) */
TEST(Capi, CallInClear)
{
	debug();
	Bench t("ATE0X3+MS=CLEAR\r", true);
	t.ctrl.add_app(2, 7, 2048);
	ASSERT_TRUE(t.ready());
	t.send(LISTEN, REQ, listen_req(1u << CIP_UDI));
	EXPECT_EQ(ident_info(t.expect(LISTEN, CONF, 0)),
		  std::make_pair(1u, u16(0)));
	t.send(LISTEN, REQ, listen_req(1u), {}, 2);
	t.expect(LISTEN, CONF, 0);
	ASSERT_EQ(dmodem_dial(t.b.m, "5551000"), 0);
	dmodem_ring(t.a, 1, "5551234");
	Msg ind = t.expect(CONNECT, IND, 1, 1);
	ASSERT_EQ(ind.cmd, CONNECT);
	Reader r(ind.params);
	EXPECT_EQ(r.d(), u32(PLCI));
	EXPECT_EQ(r.w(), CIP_UDI);
	r.s();
	std::vector<u8> calling = r.s();
	EXPECT_EQ(std::string(calling.begin() + std::min<size_t>(2,
			      calling.size()), calling.end()), "5551234");
	ASSERT_EQ(t.expect(CONNECT, IND, 0, 2).cmd, CONNECT);
	std::vector<u8> p;
	Writer(p).d(PLCI).w(0).s(bprot(0, 0, 0)).empty().empty().empty()
		.empty();
	t.send(CONNECT, RESP, p);
	Msg lost = t.expect(DISCONNECT, IND, 0, 2);
	ASSERT_EQ(lost.cmd, DISCONNECT);
	EXPECT_EQ(ident_info(lost).second, 0x3304);
	ASSERT_EQ(t.expect(CONNECT_ACTIVE, IND, 3, 1).cmd, CONNECT_ACTIVE)
		<< t.b.text;
	Msg b3 = t.expect(CONNECT_B3, IND, 0, 1);
	ASSERT_EQ(b3.cmd, CONNECT_B3);
	std::vector<u8> rp;
	Writer(rp).d(NCCI).w(0).empty();
	t.send(CONNECT_B3, RESP, rp);
	ASSERT_EQ(t.expect(CONNECT_B3_ACTIVE, IND, 0).cmd, CONNECT_B3_ACTIVE);
	t.run(5, [&] { return t.b.online; });
	ASSERT_TRUE(t.b.online) << t.b.text;
	EXPECT_TRUE(exchange(t, 20000, 20));
	dmodem_hangup(t.b.m);
	EXPECT_EQ(t.expect(DISCONNECT_B3, IND, 5).cmd, DISCONNECT_B3);
	Msg d = t.expect(DISCONNECT, IND, 1);
	ASSERT_EQ(d.cmd, DISCONNECT);
	EXPECT_EQ(ident_info(d).second, CAUSE_NORMAL);
	EXPECT_FALSE(t.ctrl.busy());
}

/* frames (B1 0, B2 1: what pppd's capiplugin asks for): each DATA_B3 a
   frame, in RFC 1662's framing at B's DTE (+B2=5), both ways */
TEST(Capi, Frames)
{
	debug();
	Bench t("ATE0+MS=CLEAR;+B2=5\r");
	call_out(t, bprot(0, 1, 0));
	std::vector<std::vector<unsigned char>> to_b, to_a;
	t.b.out.clear();
	t.b.sent = 0;
	t.b.data.clear();
	t.rx_blocks.clear();
	for (unsigned k = 0; k < 20; k++) {
		to_b.push_back(ppp_frame(40 + 70 * k, k));
		to_a.push_back(ppp_frame(1500 - 60 * k, k + 50));
		t.b.out += async_frame(to_a.back());
		std::vector<u8> p;
		Writer(p).d(NCCI).d(0).w(static_cast<u16>(to_b.back().size()))
			.w(static_cast<u16>(k)).w(0);
		t.send(DATA_B3, REQ, p, to_b.back());
		t.run(0.02);
	}
	t.run(10, [&] {
		return async_frames(t.b.data).size() >= to_b.size() &&
			t.rx_blocks.size() >= to_a.size();
	});
	EXPECT_EQ(async_frames(t.b.data), to_b);
	ASSERT_EQ(t.rx_blocks.size(), to_a.size());
	for (size_t k = 0; k < to_a.size(); k++)
		EXPECT_EQ(std::vector<unsigned char>(t.rx_blocks[k].begin(),
						     t.rx_blocks[k].end()),
			  to_a[k]) << k;
	hang_up(t, false);
}

/* V.110 at 9600 bit/s, asynchronous (B1 2, B2 1) */
TEST(Capi, V110)
{
	debug();
	Bench t("ATE0\\N0+MS=CLEAR;+CBST=71,0;+B2=7\r");
	std::vector<u8> cfg;
	Writer(cfg).w(9600).w(8).w(0).w(0);
	call_out(t, bprot(2, 1, 0, cfg));
	EXPECT_NE(t.b.text.find("CONNECT 9600"), std::string::npos)
		<< t.b.text;
	t.run(0.3);
	EXPECT_TRUE(exchange(t, 3000, 10));
	hang_up(t, false);
}

/* a modem call (B1 7, B2 7): V.91 or V.90 between the two on G.711,
   V.42; the application hangs up (+++ and ATH) */
TEST(Capi, ModemCall)
{
	debug();
	Bench t("ATE0\r", false, DMODEM_LAW_ULAW);
	call_out(t, bprot(7, 7, 0), 30);
	EXPECT_TRUE(exchange(t, 20000, 30));
	hang_up(t);
}

/* nobody answers a clear call (S7=3): DISCONNECT_IND, the controller
   free again; a B protocol it has not: CONNECT_CONF's info */
TEST(Capi, Failures)
{
	debug();
	Bench t("ATE0\r", false, DMODEM_LAW_ALAW, "S7=3");
	ASSERT_TRUE(t.ready());
	t.send(CONNECT, REQ, connect_req(CIP_UDI, "5551000",
					 bprot(4, 4, 4)));
	EXPECT_EQ(ident_info(t.expect(CONNECT, CONF, 0)),
		  std::make_pair(1u, u16(B1_NOT_SUPPORTED)));
	t.send(CONNECT, REQ, connect_req(CIP_UDI, "5551000", {}));
	EXPECT_EQ(ident_info(t.expect(CONNECT, CONF, 0)).second, 0);
	/* (one call at a time) */
	t.send(CONNECT, REQ, connect_req(CIP_UDI, "5551001", {}));
	EXPECT_EQ(ident_info(t.expect(CONNECT, CONF, 0)).second,
		  OUT_OF_PLCI);
	Msg d = t.expect(DISCONNECT, IND, 10);
	ASSERT_EQ(d.cmd, DISCONNECT);
	EXPECT_NE(ident_info(d).second, 0);
	EXPECT_FALSE(t.ctrl.busy());
	t.send(FACILITY, REQ, { 1, 0, 0, 0, 1, 0, 0 });
	Msg f = t.expect(FACILITY, CONF, 0);
	EXPECT_EQ(ident_info(f).second, FACILITY_NOT_SUPPORTED);
}

/* a call in nobody takes: offered, then the rings stop (the caller
   gave up): DISCONNECT_IND; with no listener, nothing at all */
TEST(Capi, RingsStop)
{
	debug();
	Bench t("ATE0\r", false, DMODEM_LAW_ULAW);
	ASSERT_TRUE(t.ready());
	dmodem_ring(t.a, 1, nullptr);
	t.run(2);
	EXPECT_TRUE(t.got.empty());
	t.run(12);
	t.send(LISTEN, REQ, listen_req(1u << CIP_AUDIO));
	t.expect(LISTEN, CONF, 0);
	dmodem_ring(t.a, 1, nullptr);
	Msg ind = t.expect(CONNECT, IND, 1);
	ASSERT_EQ(ind.cmd, CONNECT);
	Reader r(ind.params);
	r.d();
	EXPECT_EQ(r.w(), CIP_AUDIO);
	Msg d = t.expect(DISCONNECT, IND, 15);
	ASSERT_EQ(d.cmd, DISCONNECT);
	EXPECT_FALSE(t.ctrl.busy());
}

namespace {

/* a modem of the test's on a socket: OK to each command, +MS?'s line,
   CONNECT to ATD; what it got */
struct FakeModem {
	int fd;
	std::string got, line;
	bool online = false;
	/* each command in what came */
	void take()
	{
		char buf[4096];
		ssize_t n;
		while ((n = read(fd, buf, sizeof(buf))) > 0) {
			for (ssize_t k = 0; k < n; k++) {
				got += buf[k];
				if (online)
					continue;
				if (buf[k] != '\r') {
					line += buf[k];
					continue;
				}
				answer(line);
				line.clear();
			}
		}
	}
	/* the response to a command line */
	void answer(const std::string &l)
	{
		std::string r = "\r\nOK\r\n";
		if (l == "AT+MS?")
			r = "\r\n92,1,300,56000\r\n\r\nOK\r\n";
		else if (l.compare(0, 3, "ATD") == 0) {
			r = "\r\nCONNECT 64000\r\n";
			online = true;
		}
		put(r);
	}
	/* s to the controller */
	void put(const std::string &s)
	{
		if (write(fd, s.data(), s.size()) != ssize_t(s.size()))
			ADD_FAILURE() << "short write";
	}
};

/* an application over TCP, as libcapi20's rcapi module is */
struct Client {
	int fd = -1;
	std::vector<u8> in;
	/* connected to port */
	explicit Client(int port)
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(static_cast<u16>(port));
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(fd, reinterpret_cast<struct sockaddr *>(&a),
			    sizeof(a)))
			throw std::runtime_error("connect");
		dmd::set_nonblocking(fd);
	}
	/* (closed) */
	~Client() { close_it(); }
	/* the connection closed */
	void close_it()
	{
		if (fd >= 0)
			close(fd);
		fd = -1;
	}
	/* a message: its length, then the octets */
	void put(const std::vector<u8> &msg)
	{
		std::vector<u8> f = { static_cast<u8>((msg.size() + 2) >> 8),
				      static_cast<u8>(msg.size() + 2) };
		f.insert(f.end(), msg.begin(), msg.end());
		if (write(fd, f.data(), f.size()) != ssize_t(f.size()))
			ADD_FAILURE() << "short write";
	}
	/* an RCAPI command: header, controller, then p */
	void rcapi(u16 cmd, u32 ctrl, const std::vector<u8> &p = {})
	{
		std::vector<u8> m;
		Writer(m).w(0).w(0).b(static_cast<u8>(cmd >> 8))
			.b(static_cast<u8>(cmd)).w(0).d(ctrl);
		m.insert(m.end(), p.begin(), p.end());
		put(m);
	}
	/* a whole message from the server, if there is one */
	bool next(std::vector<u8> &m)
	{
		u8 buf[4096];
		ssize_t n;
		while ((n = read(fd, buf, sizeof(buf))) > 0)
			in.insert(in.end(), buf, buf + n);
		if (in.size() < 2)
			return false;
		size_t len = static_cast<size_t>(in[0] << 8 | in[1]);
		if (in.size() < len)
			return false;
		m.assign(in.begin() + 2, in.begin() + len);
		in.erase(in.begin(), in.begin() + len);
		return true;
	}
};

/* the server, the modem and the client until the client has a message
   (within 3 s) */
std::vector<u8> await(Server &s, FakeModem &fm, Client &c)
{
	std::vector<u8> m;
	for (int k = 0; k < 300 && !c.next(m); k++) {
		s.step(10);
		fm.take();
	}
	return m;
}

} // namespace

/* the remote CAPI server: REGISTER, the profile, the manufacturer, the
   version; LISTEN; a call out to the test's modem, data both ways
   (DATA_B3_IND 30 octets long, its data after); the application gone:
   the modem hangs up */
TEST(Capi, RcapiServer)
{
	int sv[2];
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	dmd::set_nonblocking(sv[0]);
	dmd::set_nonblocking(sv[1]);
	FakeModem fm{ sv[1] };
	Server s(sv[0]);
	int port = s.listen("127.0.0.1", 0);
	ASSERT_GT(port, 0);
	Client c(port);
	std::vector<u8> reg;
	Writer(reg).w(2048).w(1).w(7).w(2048).b(2);
	c.rcapi(RCAPI_REGISTER_REQ, 0, reg);
	std::vector<u8> m = await(s, fm, c);
	ASSERT_EQ(m.size(), 10u);
	EXPECT_EQ(m[4], 0xF3);
	EXPECT_EQ(m[5], 0xFF);
	EXPECT_EQ(m[8] | m[9] << 8, 0);

	c.rcapi(RCAPI_GET_PROFILE_REQ, 1);
	m = await(s, fm, c);
	ASSERT_EQ(m.size(), 8u + 66);
	EXPECT_EQ(m[4], 0xE1);
	EXPECT_EQ(m[8] | m[9] << 8, 0);
	EXPECT_EQ(m[10], 1);    /* controllers */
	c.rcapi(RCAPI_GET_PROFILE_REQ, 0);
	m = await(s, fm, c);
	ASSERT_GE(m.size(), 12u);
	EXPECT_EQ(m[10], 1);
	c.rcapi(RCAPI_GET_MANUFACTURER_REQ, 1);
	m = await(s, fm, c);
	ASSERT_GE(m.size(), 8u + 1 + 7);
	EXPECT_EQ(std::string(m.begin() + 9, m.begin() + 16), "D-Modem");
	c.rcapi(RCAPI_GET_VERSION_REQ, 1);
	m = await(s, fm, c);
	ASSERT_GE(m.size(), 8u + 17);
	EXPECT_EQ(m[9], 2);     /* CAPI 2.0 */

	/* the modem set up */
	for (int k = 0; k < 100 && !s.controller().ready(); k++) {
		s.step(10);
		fm.take();
	}
	ASSERT_TRUE(s.controller().ready()) << fm.got;
	EXPECT_NE(fm.got.find("+CRC=1"), std::string::npos);

	Msg req;
	req.appl = 77;          /* (the server's own id replaces it) */
	req.cmd = LISTEN;
	req.sub = REQ;
	req.num = 5;
	req.params = listen_req(1u << CIP_UDI);
	c.put(encode(req));
	m = await(s, fm, c);
	Msg conf;
	ASSERT_TRUE(decode(m.data(), m.size(), conf));
	EXPECT_EQ(conf.cmd, LISTEN);
	EXPECT_EQ(conf.sub, CONF);
	EXPECT_EQ(conf.num, 5);
	EXPECT_EQ(ident_info(conf), std::make_pair(1u, u16(0)));

	req.cmd = CONNECT;
	req.num = 6;
	req.params = connect_req(CIP_UDI, "5551000", {});
	c.put(encode(req));
	m = await(s, fm, c);
	ASSERT_TRUE(decode(m.data(), m.size(), conf));
	EXPECT_EQ(conf.cmd, CONNECT);
	EXPECT_EQ(ident_info(conf), std::make_pair(u32(PLCI), u16(0)));
	m = await(s, fm, c);
	ASSERT_TRUE(decode(m.data(), m.size(), conf));
	EXPECT_EQ(conf.cmd, CONNECT_ACTIVE);
	EXPECT_EQ(conf.sub, IND);
	EXPECT_NE(fm.got.find("ATD5551000\r"), std::string::npos) << fm.got;

	req.cmd = CONNECT_B3;
	req.num = 7;
	req.params.clear();
	Writer(req.params).d(PLCI).empty();
	c.put(encode(req));
	m = await(s, fm, c);
	ASSERT_TRUE(decode(m.data(), m.size(), conf));
	EXPECT_EQ(conf.cmd, CONNECT_B3);
	m = await(s, fm, c);
	ASSERT_TRUE(decode(m.data(), m.size(), conf));
	EXPECT_EQ(conf.cmd, CONNECT_B3_ACTIVE);

	/* DATA_B3_REQ as libcapi20 sends it: 22 octets, the data after */
	fm.got.clear();
	req.cmd = DATA_B3;
	req.num = 8;
	req.params.clear();
	Writer(req.params).d(NCCI).d(0xDEADBEEF).w(5).w(42).w(0);
	req.data = { 'h', 'e', 'l', 'l', 'o' };
	c.put(encode(req));
	m = await(s, fm, c);
	ASSERT_TRUE(decode(m.data(), m.size(), conf));
	EXPECT_EQ(conf.cmd, DATA_B3);
	EXPECT_EQ(conf.sub, CONF);
	Reader cr(conf.params);
	cr.d();
	EXPECT_EQ(cr.w(), 42);
	EXPECT_EQ(cr.w(), 0);
	EXPECT_EQ(fm.got, "hello");

	fm.put("world");
	m = await(s, fm, c);
	ASSERT_EQ(m.size(), 30u + 5);
	EXPECT_EQ(m[0] | m[1] << 8, 30);
	EXPECT_EQ(m[4], DATA_B3);
	EXPECT_EQ(m[5], IND);
	EXPECT_EQ(m[16] | m[17] << 8, 5);
	EXPECT_EQ(std::string(m.begin() + 30, m.end()), "world");

	/* the application goes: +++ after the guard, then ATH */
	c.close_it();
	fm.got.clear();
	for (int k = 0; k < 300 && fm.got.find("+++") == std::string::npos;
	     k++) {
		s.step(10);
		fm.take();
	}
	EXPECT_EQ(fm.got, "+++");
	fm.online = false;
	fm.put("\r\nOK\r\n");
	for (int k = 0; k < 100 && fm.got.find("ATH\r") == std::string::npos;
	     k++) {
		s.step(10);
		fm.take();
	}
	EXPECT_NE(fm.got.find("ATH\r"), std::string::npos) << fm.got;
	for (int k = 0; k < 20; k++) {
		s.step(10);
		fm.take();
	}
	EXPECT_FALSE(s.controller().busy());
}
