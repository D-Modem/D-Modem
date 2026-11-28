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
#include "t434.h"

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


/* IFP packet k of a test: its length (3 to 12 octets) and octets
   differ from packet to packet */
static Bytes test_ifp(int k)
{
	Bytes b(3 + (k * 7) % 10);
	for (size_t i = 0; i < b.size(); i++)
		b[i] = (unsigned char)(k * 37 + i * 11 + 1);
	return b;
}

/* got has IFP packet k of test_ifp() for sequence number seq: the same
   octets, maybe with zeros after them (a packet that FEC made again is
   as long as the longest it came from) */
static void expect_ifp(const Got &g, size_t at, int k, unsigned seq)
{
	Bytes want = test_ifp(k);
	ASSERT_LT(at, g.ifp.size());
	EXPECT_EQ(g.seq[at], seq);
	ASSERT_GE(g.ifp[at].size(), want.size()) << "packet " << k;
	EXPECT_EQ(Bytes(g.ifp[at].begin(), g.ifp[at].begin() + want.size()),
		  want) << "packet " << k;
	for (size_t i = want.size(); i < g.ifp[at].size(); i++)
		EXPECT_EQ(g.ifp[at][i], 0) << "packet " << k;
}

/* UDPTL with FEC (Annex C): the octets of the first packets (the FEC
   from fewer packets while there are not n before), then FEC message I
   (of 3) of 3 packets each in the packet of sequence number 9: the
   packets 9 - I, 6 - I and 3 - I as Figure C.3 has them */
TEST(T38Udptl, FecEncoding)
{
	UdptlTx tx;
	unsigned char pkt[600];
	int n, k;

	tx.init(T38_EC_FEC, 3);
	const unsigned char a = 0x10, b[] = { 0x20, 0x21 }, c = 0x30;
	n = tx.packet(&a, 1, pkt, sizeof(pkt));
	EXPECT_EQ(Bytes(pkt, pkt + n),
		  Bytes({ 0x00, 0x00, 0x01, 0x10, 0x80, 0x01, 0x00, 0x00 }));
	n = tx.packet(b, 2, pkt, sizeof(pkt));
	EXPECT_EQ(Bytes(pkt, pkt + n),
		  Bytes({ 0x00, 0x01, 0x02, 0x20, 0x21, 0x80, 0x01, 0x01, 0x01,
			  0x01, 0x10 }));
	n = tx.packet(&c, 1, pkt, sizeof(pkt));
	/* 0x20 0x21 ^ 0x10 */
	EXPECT_EQ(Bytes(pkt, pkt + n),
		  Bytes({ 0x00, 0x02, 0x01, 0x30, 0x80, 0x01, 0x02, 0x01,
			  0x02, 0x30, 0x21 }));
	n = tx.packet(&a, 1, pkt, sizeof(pkt));
	/* 0x30 ^ 0x20 0x21 ^ 0x10 */
	EXPECT_EQ(Bytes(pkt, pkt + n),
		  Bytes({ 0x00, 0x03, 0x01, 0x10, 0x80, 0x01, 0x03, 0x01,
			  0x02, 0x00, 0x21 }));

	tx.init(T38_EC_FEC, 3, 3);
	for (k = 0; k <= 9; k++) {
		unsigned char d = (unsigned char)(1 << (k % 8));
		unsigned char e[2] = { d, (unsigned char)k };
		n = tx.packet(e, 2, pkt, sizeof(pkt));
	}
	/* 9: the primary, then 3 packets in each of 3 messages */
	const unsigned char *f = pkt + 2 + 1 + 2;
	ASSERT_EQ(Bytes(f, f + 4), Bytes({ 0x80, 0x01, 0x03, 0x03 }));
	for (int i = 1; i <= 3; i++) {
		unsigned char x[2] = { 0, 0 };
		for (int j = 0; j < 3; j++) {
			int s = 9 - i - j * 3;
			x[0] ^= (unsigned char)(1 << (s % 8));
			x[1] ^= (unsigned char)s;
		}
		const unsigned char *m = f + 4 + (i - 1) * 3;
		EXPECT_EQ(Bytes(m, m + 3), Bytes({ 0x02, x[0], x[1] }))
			<< "message " << i;
	}
	EXPECT_EQ(n, 5 + 4 + 9);
}

/* UDPTL with FEC of 3 packets each, one message in a packet: a lost
   packet comes back from the next one, in their order, across the
   sequence numbers' wrap; 2 or 3 in a row are lost (the next packet
   misses 2 of its 3, and the packets after it come at once) */
TEST(T38Udptl, FecRecovers)
{
	UdptlTx tx;
	UdptlRx rx;
	Got g;
	unsigned char pkt[600];
	std::vector<Bytes> sent;
	int n, k;

	tx.init(T38_EC_FEC, 3);
	tx.seq = 65530;
	rx.init();
	for (k = 0; k < 20; k++) {
		Bytes b = test_ifp(k);
		n = tx.packet(b.data(), (int)b.size(), pkt, sizeof(pkt));
		ASSERT_GT(n, 0);
		sent.push_back(Bytes(pkt, pkt + n));
	}
	/* 0 1 2 (3 lost) 4 5 (6 7 lost) 8 9 10 (11 12 13 lost) 14 ... */
	for (k = 0; k < 20; k++)
		if (k != 3 && k != 6 && k != 7 && (k < 11 || k > 13)) {
			ASSERT_EQ(rx.packet(sent[k].data(), (int)sent[k].size(),
					    got, &g), 0);
		}
	ASSERT_EQ(g.ifp.size(), 20u);
	for (k = 0; k < 20; k++) {
		if (k == 6 || k == 7 || (k >= 11 && k <= 13)) {
			EXPECT_TRUE(g.ifp[k].empty()) << k;
			EXPECT_EQ(g.seq[k], (65530u + k) & 0xffff);
			continue;
		}
		expect_ifp(g, k, k, (65530u + k) & 0xffff);
	}
	EXPECT_EQ(rx.recovered, 1);
}

/* FEC 3 messages in each packet, as Annex C has them, of 3 packets each:
   the receiver finds the order from packets it has all of, then 3 lost
   in a row come back from the packet after them */
TEST(T38Udptl, FecBurst)
{
	UdptlTx tx;
	UdptlRx rx;
	Got g;
	unsigned char pkt[600];
	int n, k;

	tx.init(T38_EC_FEC, 3, 3);
	rx.init();
	for (k = 0; k < 30; k++) {
		Bytes b = test_ifp(k);
		n = tx.packet(b.data(), (int)b.size(), pkt, sizeof(pkt));
		ASSERT_GT(n, 0);
		if (k < 20 || k > 22) {
			ASSERT_EQ(rx.packet(pkt, n, got, &g), 0);
		}
	}
	EXPECT_EQ(rx.fec_order, 1);
	ASSERT_EQ(g.ifp.size(), 30u);
	for (k = 0; k < 30; k++)
		expect_ifp(g, k, k, k);
	EXPECT_EQ(rx.recovered, 3);
}

/* a UDPTL packet as spandsp and Asterisk make it with FEC: sequence
   number seq of packets (each of the ones before at its own), span
   packets in each of entries messages, the first message from the
   oldest packets (Annex C's last) */
static Bytes asterisk_fec(const std::vector<Bytes> &packets, int seq,
			  int span, int entries)
{
	Bytes b = { (unsigned char)(seq >> 8), (unsigned char)seq,
		    (unsigned char)packets[seq].size() };
	b.insert(b.end(), packets[seq].begin(), packets[seq].end());
	if (seq < span * entries) {
		entries = seq / span;
		if (seq < span)
			span = 0;
	}
	b.push_back(0x80);
	b.push_back(0x01);
	b.push_back((unsigned char)span);
	b.push_back((unsigned char)entries);
	for (int m = 0; m < entries; m++) {
		Bytes fec;
		for (int i = seq + m - span * entries; i < seq + m;
		     i += entries) {
			if (fec.size() < packets[i].size())
				fec.resize(packets[i].size());
			for (size_t j = 0; j < packets[i].size(); j++)
				fec[j] ^= packets[i][j];
		}
		b.push_back((unsigned char)fec.size());
		b.insert(b.end(), fec.begin(), fec.end());
	}
	return b;
}

/* FEC from spandsp or Asterisk (3 messages of 3 packets, the other way
   round): a burst before the receiver knows the order is lost, not
   made up wrong; once it knows, a burst of 3 comes back */
TEST(T38Udptl, FecOtherOrder)
{
	UdptlRx rx;
	Got g;
	std::vector<Bytes> packets;
	int k;

	for (k = 0; k < 40; k++)
		packets.push_back(test_ifp(k));
	rx.init();
	for (k = 0; k < 40; k++) {
		if ((k >= 10 && k <= 12) || (k >= 30 && k <= 32))
			continue;
		Bytes b = asterisk_fec(packets, k, 3, 3);
		ASSERT_EQ(rx.packet(b.data(), (int)b.size(), got, &g), 0);
		if (k == 9) {
			EXPECT_EQ(rx.fec_order, 2);
		}
	}
	ASSERT_EQ(g.ifp.size(), 40u);
	for (k = 0; k < 40; k++)
		expect_ifp(g, k, k, k);
	EXPECT_EQ(rx.recovered, 6);

	/* the order not known yet: the lost ones lost */
	Got h;
	rx.init();
	for (k = 0; k < 12; k++) {
		if (k == 4 || k == 5)
			continue;
		Bytes b = asterisk_fec(packets, k, 3, 3);
		ASSERT_EQ(rx.packet(b.data(), (int)b.size(), got, &h), 0);
		if (k == 6) {
			EXPECT_EQ(rx.fec_order, 0);
		}
	}
	ASSERT_EQ(h.ifp.size(), 12u);
	EXPECT_TRUE(h.ifp[4].empty());
	EXPECT_TRUE(h.ifp[5].empty());
	EXPECT_EQ(rx.recovered, 0);
	EXPECT_EQ(rx.fec_order, 2);
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
	int burst;      /* each burst'th packet of the fast data starts
			   depth + 1 lost in a row, which the redundancy
			   can't make up (0: none) */
	bool relay;     /* the burst lost before a relay that numbers the
			   packets anew (Asterisk's UDPTL each side): no
			   gap in the sequence numbers */
	bool fec;       /* FEC of depth packets each instead of the
			   redundancy */
};

/* n, for the test's name: the version, ECM, the rate, the loss and the
   redundancy */
static void PrintTo(const Net &n, std::ostream *os)
{
	*os << "v" << n.version << (n.ecm ? " ecm " : " ") << n.bps
	    << " lose " << n.lose << " depth " << n.depth << " burst "
	    << n.burst << (n.relay ? " relay" : "") << (n.fec ? " fec" : "");
}

/* one side: its document, its T.30 machine, and UDPTL each way */
struct Side {
	TiffIn in;
	TiffOut out;
	T30 *t30;
	int version, lose, sent;
	int depth, burst, fast, losing;
	bool relay;
	UdptlTx tx;
	UdptlRx rx;
	std::deque<Bytes> *line;        /* to the other side */
	/* instead of the TIFF files (if not null): the pages and files to
	   send, those received */
	std::deque<T4Page> *list = nullptr;
	std::vector<T4Page> *got = nullptr;
};

/* the side's next page, from its document: 1, 0 when there are no
   more, -1 on an error */
static s16 side_next_page(void *ctx, T4Page *p)
{
	Side *s = static_cast<Side *>(ctx);
	if (s->list) {
		if (s->list->empty())
			return 0;
		*p = std::move(s->list->front());
		s->list->pop_front();
		return 1;
	}
	return s->in.read_page(p);
}

/* a page the side received, written to its TIFF */
static void side_page_received(void *ctx, T4Page *p)
{
	Side *s = static_cast<Side *>(ctx);
	if (s->got)
		s->got->push_back(*p);
	else
		s->out.write_page(p);
}

/* an IFP packet of the machine's: in UDPTL onto the line, unless it is
   one of those lost */
static void side_tx(void *ctx, const struct t38_ifp *p)
{
	Side *s = static_cast<Side *>(ctx);
	unsigned char ifp[UDPTL_IFP_MAX], pkt[1500];
	int n = t38_ifp_encode(p, s->version, ifp, sizeof(ifp)), m;
	bool lost;
	ASSERT_GT(n, 0);
	if (s->burst && p->data && p->type != T38_DATA_V21 &&
	    ++s->fast % s->burst == 0)
		s->losing = s->depth + 1;
	lost = s->losing > 0;
	if (lost)
		s->losing--;
	if (lost && s->relay)
		return;
	m = s->tx.packet(ifp, n, pkt, sizeof(pkt));
	ASSERT_GT(m, 0);
	if (lost || (s->lose && ++s->sent % s->lose == 0))
		return;
	s->line->push_back(Bytes(pkt, pkt + m));
}

/* an IFP packet off the line, to the machine */
static void side_rx(void *ctx, const unsigned char *ifp, s16 len, u16)
{
	Side *s = static_cast<Side *>(ctx);
	struct t38_ifp p;
	if (!len) {
		t30_t38_lost(s->t30);
		return;
	}
	ASSERT_EQ(t38_ifp_decode(&p, s->version, ifp, len), 0);
	t30_t38_rx(s->t30, &p);
}

class T38Pair : public testing::TestWithParam<Net> {};

/* the document from a sender to a receiver over T.38 (10 minutes at
   most, the packets one 20 ms tick late): both OK, 2 pages, the same;
   lost packets the redundancy has back don't matter, and ECM frames
   that lost some of their data are asked for again */
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
		s->depth = net.depth;
		s->burst = net.burst;
		s->fast = s->losing = 0;
		s->relay = net.relay;
		s->tx.init(!net.depth ? T38_EC_NONE :
			   net.fec ? T38_EC_FEC : T38_EC_REDUNDANCY,
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
	Net{ 0, false, 14400, 5, 3 }, Net{ 2, true, 14400, 5, 3 },
	/* ECM: bursts the redundancy can't make up in the frames' data */
	Net{ 2, true, 14400, 0, 3, 40, false },
	Net{ 0, true, 9600, 0, 3, 25, false },
	/* (a gap in a page's last frame can still go by: it may be short,
	   as spandsp's is) */
	Net{ 2, true, 14400, 0, 3, 40, true },
	Net{ 3, true, 14400, 0, 3, 33, true },
	/* FEC: each packet lost comes back */
	Net{ 2, false, 14400, 5, 3, 0, false, true },
	Net{ 0, true, 9600, 4, 3, 0, false, true },
	Net{ 2, true, 14400, 0, 3, 40, false, true }),
	/* v<version>[_ecm]_<bps>[_lose<n>][_burst<n>][_relay][_fec] */
	[](const testing::TestParamInfo<Net> &i) {
		return "v" + std::to_string(i.param.version) +
			(i.param.ecm ? "_ecm_" : "_") +
			std::to_string(i.param.bps) +
			(i.param.lose ? "_lose" + std::to_string(i.param.lose) :
			 std::string()) +
			(i.param.burst ? "_burst" + std::to_string(i.param.burst) :
			 std::string()) +
			(i.param.relay ? "_relay" : "") +
			(i.param.fec ? "_fec" : "");
	});

/* binary files over T.38 with ECM, packets lost in bursts the
   redundancy can't make up: a file of 100000 random octets (its last
   frame short) and one of 3 come whole, a page between them */
TEST(T38Pair, SendsFiles)
{
	std::deque<Bytes> ab, ba;
	std::deque<T4Page> list;
	std::vector<T4Page> got;
	Side a, b;
	struct t30_config c = {};
	struct t30_status sa = {}, sb = {};
	BftFile f[2];
	f[0].name = "big.bin";
	f[0].data.resize(100000);
	for (size_t i = 0; i < f[0].data.size(); i++)
		f[0].data[i] = (unsigned char)(i * 2654435761u >> 13);
	f[1].name = "small";
	f[1].data = {1, 2, 3};
	T4Page fp, page;
	t434_encode(f[0], &fp.file);
	list.push_back(std::move(fp));
	test_page(&page, 0, 4, 20);
	list.push_back(std::move(page));
	t434_encode(f[1], &fp.file);
	list.push_back(std::move(fp));

	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	for (Side *s : { &a, &b }) {
		s->version = 2;
		s->lose = 0;
		s->sent = 0;
		s->depth = 3;
		s->burst = 40;
		s->fast = s->losing = 0;
		s->relay = false;
		s->tx.init(T38_EC_REDUNDANCY, 3);
		s->rx.init();
	}
	a.line = &ab;
	b.line = &ba;
	a.list = &list;
	b.got = &got;
	c.srate = 8000;
	c.max_bps = 14400;
	c.ecm = 1;
	c.bft = 1;
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
	for (long n = 0; n < 8000L * 600; n += 160) {
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
	t30_free(a.t30);
	t30_free(b.t30);
	EXPECT_EQ(sa.result, T30_OK) << "sender: " << t30_result_text(sa.result);
	EXPECT_EQ(sb.result, T30_OK) << "receiver: " << t30_result_text(sb.result);
	EXPECT_EQ(sb.files, 2);
	EXPECT_EQ(sb.pages, 1);
	ASSERT_EQ(got.size(), 3u);
	for (int k = 0; k < 2; k++) {
		const T4Page &g = got[k * 2];
		std::vector<BftFile> fs;
		ASSERT_EQ(t434_decode(g.file.data(), g.file.size(), &fs), 0);
		ASSERT_EQ(fs.size(), 1u);
		EXPECT_EQ(fs[0].name, f[k].name);
		EXPECT_TRUE(fs[0].data == f[k].data) << "file " << k;
	}
	EXPECT_TRUE(got[1].file.empty());
	EXPECT_GT(got[1].rows, 0);
}

/* T38FaxUdpEC (Annex D.2.3, Table D.2): an answer gives the offered
   value when we have it (t38UDPNoEC, t38UDPRedundancy, t38UDPFEC),
   else t38UDPRedundancy; we send FEC for t38UDPFEC, the packets before
   again for t38UDPRedundancy */
TEST(T38Sdp, UdpEc)
{
	EXPECT_STREQ(t38_udp_ec_answer("t38UDPNoEC"), "t38UDPNoEC");
	EXPECT_STREQ(t38_udp_ec_answer("T38UDPNOEC"), "t38UDPNoEC");
	EXPECT_STREQ(t38_udp_ec_answer("t38UDPRedundancy"),
		     "t38UDPRedundancy");
	EXPECT_STREQ(t38_udp_ec_answer("t38UDPFEC"), "t38UDPFEC");
	EXPECT_STREQ(t38_udp_ec_answer("T38UDPfec"), "t38UDPFEC");
	EXPECT_STREQ(t38_udp_ec_answer(""), "t38UDPRedundancy");
	EXPECT_STREQ(t38_udp_ec_answer(nullptr), "t38UDPRedundancy");
	EXPECT_TRUE(t38_udp_ec_redundancy("t38UDPRedundancy"));
	EXPECT_TRUE(t38_udp_ec_redundancy("t38udpfec"));
	EXPECT_FALSE(t38_udp_ec_redundancy("t38UDPNoEC"));
	EXPECT_FALSE(t38_udp_ec_redundancy(""));
	EXPECT_FALSE(t38_udp_ec_redundancy(nullptr));
	EXPECT_EQ(t38_udp_ec_tx("t38UDPFEC"), T38_EC_FEC);
	EXPECT_EQ(t38_udp_ec_tx("t38UDPRedundancy"), T38_EC_REDUNDANCY);
	EXPECT_EQ(t38_udp_ec_tx("t38UDPNoEC"), T38_EC_NONE);
	EXPECT_EQ(t38_udp_ec_tx(nullptr), T38_EC_NONE);
}
