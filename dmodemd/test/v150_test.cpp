/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v150_test.cpp - V.150.1's modem relay (v150.h): SSE's and CONNECT's
 *                    codings, SPRT's order, acknowledgements, windows and
 *                    retransmission over a lossy network, and two
 *                    sessions (a caller and an answerer) through their
 *                    start, the data and the clear down
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "v150.h"

using namespace dmd::v150;

/* an SSE and its four octets, both ways */
TEST(V150, Sse)
{
	Sse e;
	e.event = SSE_MR;
	e.ric = RIC_CM;
	e.info = OUR_MODS;
	u8 p[4];
	ASSERT_EQ(sse_encode(e, p), 4u);
	EXPECT_EQ(p[0], 0x0C);
	EXPECT_EQ(p[1], 1);
	EXPECT_EQ(p[2] << 8 | p[3], 0x5808);
	Sse d;
	ASSERT_TRUE(sse_decode(p, 4, &d));
	EXPECT_EQ(d.event, SSE_MR);
	EXPECT_EQ(d.ric, RIC_CM);
	EXPECT_EQ(d.info, OUR_MODS);
	EXPECT_FALSE(sse_decode(p, 3, &d));
}

/* CONNECT as Table 25 lays it out */
TEST(V150, Connect)
{
	Connect c;
	c.selmod = SEL_V34;
	c.ec = 1;
	c.tdsr = 31200;
	c.rdsr = 28800;
	std::vector<u8> m = connect_encode(c);
	ASSERT_EQ(m.size(), 9u);
	EXPECT_EQ(m[0], MSG_CONNECT);
	EXPECT_EQ(m[1], 4 << 2);
	EXPECT_EQ(m[2], 1);
	EXPECT_EQ(m[3] << 8 | m[4], 31200);
	EXPECT_EQ(m[5] << 8 | m[6], 28800);
	Connect d;
	ASSERT_TRUE(connect_decode(m.data(), m.size(), &d));
	EXPECT_EQ(d.selmod, SEL_V34);
	EXPECT_EQ(d.ec, 1);
	EXPECT_EQ(d.tdsr, 31200);
	EXPECT_EQ(d.rdsr, 28800);
	Selmod s;
	u16 r;
	ASSERT_TRUE(choose(MOD_V32 | MOD_V21 | MOD_V90A, &s, &r));
	EXPECT_EQ(s, SEL_V32BIS);
	EXPECT_EQ(r, 14400);
	EXPECT_FALSE(choose(MOD_V17 | MOD_V90A, &s, &r));
}

/* the header: TC, SEQUENCE NUMBER, BSN and the acks */
TEST(V150, SprtHeader)
{
	Sprt a(120), b(120);
	const u8 x[] = { 'h', 'i' };
	ASSERT_TRUE(a.send(1, x, 2, 0));
	std::vector<std::vector<u8>> out;
	a.poll(0, &out);
	ASSERT_EQ(out.size(), 1u);
	std::vector<u8> p = out[0];
	ASSERT_EQ(p.size(), 8u);
	EXPECT_EQ(p[0], 0);
	EXPECT_EQ(p[1], 120);
	EXPECT_EQ(p[2], 0x40);          /* TC 1, SEQ 0 */
	EXPECT_EQ(p[3], 0);
	EXPECT_EQ(p[4], 0);             /* no acks, BSN 0 */
	EXPECT_EQ(p[6], 'h');
	EXPECT_FALSE(b.receive(p.data(), 5, 0));
	Sprt other(99);
	EXPECT_FALSE(other.receive(p.data(), p.size(), 0));
	ASSERT_TRUE(b.receive(p.data(), p.size(), 0));
	int tc;
	std::vector<u8> pl;
	ASSERT_TRUE(b.next(&tc, &pl));
	EXPECT_EQ(tc, 1);
	EXPECT_EQ(std::string(pl.begin(), pl.end()), "hi");
	/* b's ack after TA01 (90 ms), in an ack-only packet */
	out.clear();
	b.poll(50, &out);
	EXPECT_TRUE(out.empty());
	b.poll(100, &out);
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].size(), 8u);   /* TC 0, one ack: TC 1, SEQ 0 */
	EXPECT_EQ(out[0][2], 0);
	EXPECT_EQ(out[0][4] >> 6, 1);
	EXPECT_EQ(out[0][6], 0x40);
	EXPECT_EQ(out[0][7], 0);
	/* and after TA02 (130 ms) TC 1 empty, with its BSN: 1 */
	out.clear();
	b.poll(140, &out);
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].size(), 6u);
	EXPECT_EQ(out[0][2], 0x40);
	EXPECT_EQ(out[0][5], 1);
}

/* the network between two ends: each packet lost when the pattern says
   so, the rest in order a step later */
struct Net {
	unsigned n = 0;
	unsigned lose_every = 0;        /* 0: none lost */
	/* one end's packets to the other */
	template <class R> void carry(std::vector<std::vector<u8>> &out,
				      R receive)
	{
		for (auto &p : out)
			if (!lose_every || ++n % lose_every)
				receive(p);
		out.clear();
	}
};

/* 5000 octets over TC1 with every third packet lost, a window that
   fills while the receiving user is slow: all of them, in order */
TEST(V150, SprtReliableLossy)
{
	Sprt a(100), b(100);
	Net net;
	net.lose_every = 3;
	std::string sent, got;
	for (int i = 0; i < 5000; i++)
		sent += static_cast<char>(i * 7 + i / 13);
	size_t at = 0;
	std::vector<std::vector<u8>> oa, ob;
	for (u64 now = 0; now < 120000 && got.size() < sent.size();
	     now += 10) {
		while (at < sent.size() && a.can_send(1)) {
			size_t k = std::min<size_t>(100, sent.size() - at);
			ASSERT_TRUE(a.send(1, reinterpret_cast<const u8 *>(
				sent.data() + at), k, now));
			at += k;
		}
		a.poll(now, &oa);
		b.poll(now, &ob);
		net.carry(oa, [&](std::vector<u8> &p) {
			b.receive(p.data(), p.size(), now);
		});
		net.carry(ob, [&](std::vector<u8> &p) {
			a.receive(p.data(), p.size(), now);
		});
		/* the user takes one payload a step */
		int tc;
		std::vector<u8> pl;
		if (b.next(&tc, &pl)) {
			EXPECT_EQ(tc, 1);
			got.append(pl.begin(), pl.end());
		}
	}
	EXPECT_EQ(got, sent);
	EXPECT_GT(a.retransmits(), 0u);
}

/* two sessions: the network, their clocks */
struct Pair {
	Session g1, g2;                 /* the caller, the answerer */
	Net net;
	u64 now = 0;
	/* the far end's SDP mg as each sees it */
	Pair(int mg1 = 2, int mg2 = 2, u16 mods2 = OUR_MODS)
		: g1(true, 101, mg1), g2(false, 101, mg2, mods2) {}
	/* 10 ms: the SSEs and the packets across */
	void step()
	{
		Sse e;
		while (g1.sse_out(&e))
			g2.sse_in(e, now);
		while (g2.sse_out(&e))
			g1.sse_in(e, now);
		std::vector<std::vector<u8>> o1, o2;
		g1.poll(now, &o1);
		g2.poll(now, &o2);
		net.carry(o1, [&](std::vector<u8> &p) {
			g2.packet_in(p.data(), p.size(), now);
		});
		net.carry(o2, [&](std::vector<u8> &p) {
			g1.packet_in(p.data(), p.size(), now);
		});
		now += 10;
	}
	/* steps until cond, at most ms */
	template <class F> bool until(F cond, int ms)
	{
		for (int t = 0; t < ms; t += 10) {
			if (cond())
				return true;
			step();
		}
		return cond();
	}
};

/* the answerer plays ANSam until the caller's SSE:M; both up at the
   other's CONNECT, at V.34 */
TEST(V150, SessionStart)
{
	Pair p;
	s32 tx = 0, rx = 0;
	p.g2.start(0);
	EXPECT_TRUE(p.g2.answer_tone());
	EXPECT_EQ(p.g2.state(&tx, &rx), 0);
	p.step();
	EXPECT_EQ(p.g1.phase(), Session::IDLE);
	p.g1.start(p.now);
	ASSERT_TRUE(p.until([&] {
		return p.g1.phase() == Session::UP &&
			p.g2.phase() == Session::UP;
	}, 2000));
	EXPECT_FALSE(p.g2.answer_tone());
	EXPECT_TRUE(p.g1.relaying());
	ASSERT_EQ(p.g1.state(&tx, &rx), 1);
	EXPECT_EQ(tx, 33600);
	EXPECT_EQ(rx, 33600);
	ASSERT_EQ(p.g2.state(&tx, &rx), 1);
	EXPECT_EQ(tx, 33600);
}

/* the answerer has only V.22 bis: both at 2400; with nothing in common
   it clears down */
TEST(V150, SessionModulation)
{
	Pair p(2, 2, MOD_V22 | MOD_V17);
	p.g2.start(0);
	p.g1.start(0);
	ASSERT_TRUE(p.until([&] { return p.g1.phase() == Session::UP; },
			    2000));
	s32 tx, rx;
	ASSERT_EQ(p.g1.state(&tx, &rx), 1);
	EXPECT_EQ(tx, 2400);
	Pair q(2, 2, MOD_V17);
	q.g2.start(0);
	q.g1.start(0);
	ASSERT_TRUE(q.until([&] {
		return q.g1.phase() == Session::OVER &&
			q.g2.phase() == Session::OVER;
	}, 2000));
	EXPECT_EQ(q.g1.why(), "the far end cleared down");
}

/* data both ways through lost packets, the reader slow: all of it */
TEST(V150, SessionData)
{
	Pair p;
	p.net.lose_every = 5;
	p.g2.start(0);
	p.g1.start(0);
	ASSERT_TRUE(p.until([&] {
		return p.g1.phase() == Session::UP &&
			p.g2.phase() == Session::UP;
	}, 5000));
	std::string a, b, got_a, got_b;
	for (int i = 0; i < 20000; i++) {
		a += static_cast<char>(i);
		b += static_cast<char>(i * 31 + 5);
	}
	size_t at_a = 0, at_b = 0;
	ASSERT_TRUE(p.until([&] {
		at_a += p.g1.write(reinterpret_cast<const u8 *>(a.data()) +
				   at_a, a.size() - at_a, p.now);
		at_b += p.g2.write(reinterpret_cast<const u8 *>(b.data()) +
				   at_b, b.size() - at_b, p.now);
		u8 buf[200];
		size_t n = p.g2.read(buf, sizeof(buf));
		got_b.append(reinterpret_cast<char *>(buf), n);
		n = p.g1.read(buf, sizeof(buf));
		got_a.append(reinterpret_cast<char *>(buf), n);
		return got_b.size() == a.size() && got_a.size() == b.size();
	}, 120000)) << got_a.size() << " " << got_b.size();
	EXPECT_EQ(got_b, a);
	EXPECT_EQ(got_a, b);
}

/* one end hangs up: CLEARDOWN, the other over too */
TEST(V150, SessionHangup)
{
	Pair p;
	p.g2.start(0);
	p.g1.start(0);
	ASSERT_TRUE(p.until([&] { return p.g2.phase() == Session::UP; },
			    2000));
	p.g1.hangup(p.now);
	s32 tx, rx;
	EXPECT_EQ(p.g1.state(&tx, &rx), -1);
	ASSERT_TRUE(p.until([&] { return p.g2.phase() == Session::OVER; },
			    2000));
	EXPECT_EQ(p.g2.why(), "the far end cleared down");
}

/* a far end that is not D-TCX and whose modem compresses: cleared down;
   a D-TCX one: up */
TEST(V150, SessionCompression)
{
	Session g2(false, 101, 0);
	g2.start(0);
	Sse e;
	e.event = SSE_MR;
	e.ric = RIC_CM;
	e.info = OUR_MODS;
	g2.sse_in(e, 0);
	ASSERT_TRUE(g2.relaying());
	/* the caller's CONNECT as its own SPRT end would send it */
	Sprt far(101);
	Connect c;
	c.selmod = SEL_V34;
	c.comp_dir = 3;
	c.comp = 1;
	c.ec = 1;
	c.tdsr = c.rdsr = 33600;
	std::vector<u8> m = connect_encode(c);
	ASSERT_TRUE(far.send(2, m.data(), m.size(), 0));
	std::vector<std::vector<u8>> out;
	far.poll(0, &out);
	for (auto &pk : out)
		g2.packet_in(pk.data(), pk.size(), 0);
	EXPECT_EQ(g2.phase(), Session::OVER);

	Session d(false, 101, 2);
	d.start(0);
	d.sse_in(e, 0);
	for (auto &pk : out)
		d.packet_in(pk.data(), pk.size(), 0);
	EXPECT_EQ(d.phase(), Session::UP);
}
