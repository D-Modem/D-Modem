/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t38_test.cpp - T.38: the IFP packets' encoding (both versions, the
 *    examples of T.38 Appendix V and of the common implementations), UDPTL
 *    with its redundancy, and two of our T.30 machines over T.38, packets
 *    lost and not
 *
 *    DSP_DEBUG=<level>   the machines' log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "dsp.h"
#include "fax_util.h"
#include "t30.h"
#include "t38.h"

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

/* p encoded as version has it */
static Bytes encode(const struct t38_ifp &p, int version)
{
	unsigned char buf[UDPTL_IFP_MAX];
	int n = t38_ifp_encode(&p, version, buf, sizeof(buf));
	return n < 0 ? Bytes() : Bytes(buf, buf + n);
}

/* an IFP packet of indicator ind */
static struct t38_ifp indicator(int ind)
{
	struct t38_ifp p = {};
	p.type = ind;
	return p;
}

/* indicators: the 1998 ones in an octet, the 2002 ones in two (only in
   versions 1 and up) */
TEST(T38Ifp, Indicators)
{
	struct t38_ifp p;
	EXPECT_EQ(encode(indicator(T38_IND_NO_SIGNAL), 0), Bytes({ 0x00 }));
	EXPECT_EQ(encode(indicator(T38_IND_CED), 0), Bytes({ 0x04 }));
	EXPECT_EQ(encode(indicator(T38_IND_V21_PREAMBLE), 0), Bytes({ 0x06 }));
	EXPECT_EQ(encode(indicator(T38_IND_V17_14400_LONG), 2),
		  Bytes({ 0x1e }));
	EXPECT_EQ(encode(indicator(T38_IND_V8_ANSAM), 2), Bytes({ 0x20, 0x00 }));
	EXPECT_EQ(encode(indicator(T38_IND_V33_14400), 2),
		  Bytes({ 0x21, 0x80 }));
	EXPECT_TRUE(encode(indicator(T38_IND_V8_ANSAM), 0).empty());

	const unsigned char ansam[] = { 0x20, 0x00 };
	ASSERT_EQ(t38_ifp_decode(&p, 2, ansam, 2), 0);
	EXPECT_FALSE(p.data);
	EXPECT_EQ(p.type, T38_IND_V8_ANSAM);
	EXPECT_EQ(t38_ifp_decode(&p, 0, ansam, 2), -1);
	const unsigned char cng[] = { 0x02 };
	ASSERT_EQ(t38_ifp_decode(&p, 0, cng, 1), 0);
	EXPECT_EQ(p.type, T38_IND_CNG);
}

/* the start of a DIS (T.38 V.1.1: FF C8 01, the first bit at the top)
   in a V.21 data packet with its FCS-OK-sig-end: the field headers of
   each version, and the octets back as they went */
TEST(T38Ifp, HdlcData)
{
	const unsigned char ours[] = { 0xff, 0x13, 0x80 };      /* as T.30 */
	unsigned char line[3];
	struct t38_ifp p = {}, q;

	t38_reverse(line, ours, 3);
	EXPECT_EQ(Bytes(line, line + 3), Bytes({ 0xff, 0xc8, 0x01 }));
	p.data = 1;
	p.type = T38_DATA_V21;
	p.nfields = 2;
	p.field[0].type = T38_HDLC_DATA;
	p.field[0].data = line;
	p.field[0].len = 3;
	p.field[1].type = T38_HDLC_FCS_OK_SIG_END;
	EXPECT_EQ(encode(p, 0), Bytes({ 0xc0, 0x02, 0x80, 0x00, 0x02,
					0xff, 0xc8, 0x01, 0x40 }));
	EXPECT_EQ(encode(p, 2), Bytes({ 0xc0, 0x02, 0x80, 0x00, 0x02,
					0xff, 0xc8, 0x01, 0x20 }));
	for (int v = 0; v <= 3; v++) {
		Bytes b = encode(p, v);
		ASSERT_EQ(t38_ifp_decode(&q, v, b.data(), (int)b.size()), 0);
		EXPECT_TRUE(q.data);
		EXPECT_EQ(q.type, T38_DATA_V21);
		ASSERT_EQ(q.nfields, 2);
		EXPECT_EQ(q.field[0].type, T38_HDLC_DATA);
		EXPECT_EQ(Bytes(q.field[0].data, q.field[0].data + 3),
			  Bytes(line, line + 3));
		EXPECT_EQ(q.field[1].type, T38_HDLC_FCS_OK_SIG_END);
		EXPECT_EQ(q.field[1].len, 0);
	}
	/* a field without data before another: not ours to send */
	p.field[0].len = 0;
	EXPECT_TRUE(encode(p, 0).empty());
}

/* the 1998 encoding packs fields without data two to an octet: FCS-OK
   (0x2), then FCS-BAD-sig-end (0x5) in the low half */
TEST(T38Ifp, Version0HalfOctets)
{
	const unsigned char b[] = { 0xc0, 0x02, 0x25 };
	struct t38_ifp p;
	ASSERT_EQ(t38_ifp_decode(&p, 0, b, 3), 0);
	ASSERT_EQ(p.nfields, 2);
	EXPECT_EQ(p.field[0].type, T38_HDLC_FCS_OK);
	EXPECT_EQ(p.field[1].type, T38_HDLC_FCS_BAD_SIG_END);
	/* a data packet without fields, and one cut short */
	const unsigned char none[] = { 0x51 };
	ASSERT_EQ(t38_ifp_decode(&p, 2, none, 1), 0);
	EXPECT_TRUE(p.data);
	EXPECT_EQ(p.type, T38_DATA_V17_14400);
	EXPECT_EQ(p.nfields, 0);
	const unsigned char cut[] = { 0xd0, 0x01, 0x80, 0x00, 0x05, 0x01 };
	EXPECT_EQ(t38_ifp_decode(&p, 2, cut, sizeof(cut)), -1);
}

struct Got {
	std::vector<Bytes> ifp;
	std::vector<unsigned> seq;
};

/* an IFP packet UDPTL delivered, with its sequence number, kept in g */
static void got(void *ctx, const unsigned char *ifp, s16 len, u16 seq)
{
	Got *g = static_cast<Got *>(ctx);
	g->ifp.push_back(Bytes(ifp, ifp + len));
	g->seq.push_back(seq);
}

/* UDPTL: the packet as Asterisk's has it without error recovery; with
   redundancy the lost ones come back from the next, in their order, a
   repeated one once only, across the sequence numbers' wrap */
TEST(T38Udptl, Redundancy)
{
	UdptlTx tx;
	UdptlRx rx;
	Got g;
	unsigned char pkt[600];
	std::vector<Bytes> sent;
	int n, k;

	tx.init(T38_EC_NONE, 0);
	const unsigned char ifp[] = { 0x06 };
	n = tx.packet(ifp, 1, pkt, sizeof(pkt));
	EXPECT_EQ(Bytes(pkt, pkt + n), Bytes({ 0x00, 0x00, 0x01, 0x06, 0x00,
					       0x00 }));

	tx.init(T38_EC_REDUNDANCY, 3);
	tx.seq = 65533;
	rx.init();
	for (k = 0; k < 8; k++) {
		unsigned char d = (unsigned char)(0x10 + k);
		n = tx.packet(&d, 1, pkt, sizeof(pkt));
		ASSERT_GT(n, 0);
		sent.push_back(Bytes(pkt, pkt + n));
	}
	/* 0 1 (2 3 lost) 4 4 (5 lost) 7 (6 late) 6 */
	const int order[] = { 0, 1, 4, 4, 7, 6 };
	for (int i : order)
		ASSERT_EQ(rx.packet(sent[i].data(), (int)sent[i].size(), got, &g),
			  0);
	ASSERT_EQ(g.ifp.size(), 8u);
	for (k = 0; k < 8; k++) {
		EXPECT_EQ(g.ifp[k], Bytes({ (unsigned char)(0x10 + k) })) << k;
		EXPECT_EQ(g.seq[k], (65533u + k) & 0xffff) << k;
	}
	const unsigned char junk[] = { 0x00, 0x01, 0x05, 0x06 };
	EXPECT_EQ(rx.packet(junk, sizeof(junk), got, &g), -1);
}


/*
 *    two of our machines over T.38
 */

struct Net {
	int version;
	bool ecm;
	int bps;
	int lose;       /* every lose'th packet lost (0: none), both ways */
	int depth;      /* UDPTL's redundancy */
};

/* n, for the test's name: the version, ECM, the rate, the loss and the
   redundancy */
static void PrintTo(const Net &n, std::ostream *os)
{
	*os << "v" << n.version << (n.ecm ? " ecm " : " ") << n.bps
	    << " lose " << n.lose << " depth " << n.depth;
}

/* one side: its document, its T.30 machine, and UDPTL each way */
struct Side {
	TiffIn in;
	TiffOut out;
	T30 *t30;
	int version, lose, sent;
	UdptlTx tx;
	UdptlRx rx;
	std::deque<Bytes> *line;        /* to the other side */
};

/* the side's next page, from its document: 1, 0 when there are no
   more, -1 on an error */
static s16 side_next_page(void *ctx, T4Page *p)
{
	return static_cast<Side *>(ctx)->in.read_page(p);
}

/* a page the side received, written to its TIFF */
static void side_page_received(void *ctx, T4Page *p)
{
	static_cast<Side *>(ctx)->out.write_page(p);
}

/* an IFP packet of the machine's: in UDPTL onto the line, unless it is
   one of those lost */
static void side_tx(void *ctx, const struct t38_ifp *p)
{
	Side *s = static_cast<Side *>(ctx);
	unsigned char ifp[UDPTL_IFP_MAX], pkt[1500];
	int n = t38_ifp_encode(p, s->version, ifp, sizeof(ifp)), m;
	ASSERT_GT(n, 0);
	m = s->tx.packet(ifp, n, pkt, sizeof(pkt));
	ASSERT_GT(m, 0);
	if (s->lose && ++s->sent % s->lose == 0)
		return;
	s->line->push_back(Bytes(pkt, pkt + m));
}

/* an IFP packet off the line, to the machine */
static void side_rx(void *ctx, const unsigned char *ifp, s16 len, u16)
{
	Side *s = static_cast<Side *>(ctx);
	struct t38_ifp p;
	ASSERT_EQ(t38_ifp_decode(&p, s->version, ifp, len), 0);
	t30_t38_rx(s->t30, &p);
}

class T38Pair : public testing::TestWithParam<Net> {};

/* the document from a sender to a receiver over T.38 (10 minutes at
   most, the packets one 20 ms tick late): both OK, 2 pages, the same;
   lost packets the redundancy has back don't matter */
TEST_P(T38Pair, SendsTheDocument)
{
	const Net &net = GetParam();
	std::string in = fax_file("in.tif"), out = fax_file("out.tif");
	std::deque<Bytes> ab, ba;
	Side a, b;
	struct t30_config c = {};
	struct t30_status sa = {}, sb = {};
	long n;

	ASSERT_TRUE(write_test_doc(in, true));
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	ASSERT_EQ(a.in.open(in.c_str()), 0);
	ASSERT_EQ(b.out.open(out.c_str()), 0);
	for (Side *s : { &a, &b }) {
		s->version = net.version;
		s->lose = net.lose;
		s->sent = 0;
		s->tx.init(net.depth ? T38_EC_REDUNDANCY : T38_EC_NONE,
			   net.depth);
		s->rx.init();
	}
	a.line = &ab;
	b.line = &ba;
	c.srate = 8000;
	c.max_bps = net.bps;
	c.ecm = net.ecm;
	c.t38 = 1;
	c.t38_tx = side_tx;
	c.sending = 1;
	c.ident = "+1 555 0100";
	c.next_page = side_next_page;
	c.ctx = &a;
	a.t30 = t30_create(&c);
	c.sending = 0;
	c.ident = "+1 555 0199";
	c.next_page = NULL;
	c.page_received = side_page_received;
	c.ctx = &b;
	b.t30 = t30_create(&c);

	for (n = 0; n < 8000L * 600; n += 160) {
		/* what went out last tick arrives now */
		std::deque<Bytes> to_b, to_a;
		to_b.swap(ab);
		to_a.swap(ba);
		for (const Bytes &p : to_b)
			b.rx.packet(p.data(), (int)p.size(), side_rx, &b);
		for (const Bytes &p : to_a)
			a.rx.packet(p.data(), (int)p.size(), side_rx, &a);
		t30_t38_time(a.t30, 160);
		t30_t38_time(b.t30, 160);
		t30_get_status(a.t30, &sa);
		t30_get_status(b.t30, &sb);
		if (sa.done && sb.done)
			break;
	}
	b.out.close();
	a.in.close();
	printf("%d pages at %d bit/s%s in %.1f s\n", sa.pages, sa.bps,
	       sa.ecm ? " (ECM)" : "", n / 8000.0);
	EXPECT_EQ(sa.result, T30_OK) << "sender: " << t30_result_text(sa.result);
	EXPECT_EQ(sb.result, T30_OK) << "receiver: " << t30_result_text(sb.result);
	EXPECT_TRUE(sa.done && sb.done);
	EXPECT_EQ(sa.pages, 2);
	EXPECT_EQ(sb.pages, 2);
	EXPECT_EQ(sa.bps, net.bps);
	EXPECT_EQ(!!sa.ecm, net.ecm);
	EXPECT_STREQ(sa.remote_ident, "+1 555 0199");
	EXPECT_STREQ(sb.remote_ident, "+1 555 0100");
	t30_free(a.t30);
	t30_free(b.t30);
	EXPECT_TRUE(same_pages(in, out));
}

INSTANTIATE_TEST_SUITE_P(Nets, T38Pair, testing::Values(
	Net{ 0, false, 14400, 0, 0 }, Net{ 0, true, 14400, 0, 0 },
	Net{ 2, false, 9600, 0, 0 }, Net{ 3, true, 4800, 0, 3 },
	Net{ 0, false, 14400, 5, 3 }, Net{ 2, true, 14400, 5, 3 }),
	/* v<version>[_ecm]_<bps>[_lose<n>] */
	[](const testing::TestParamInfo<Net> &i) {
		return "v" + std::to_string(i.param.version) +
			(i.param.ecm ? "_ecm_" : "_") +
			std::to_string(i.param.bps) +
			(i.param.lose ? "_lose" + std::to_string(i.param.lose) :
			 std::string());
	});
