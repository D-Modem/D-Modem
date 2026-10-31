/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    class1_v34_test.cpp - fax class 1.0 on V.34 half-duplex (T.31's
 *    V.34 data mode, fax_class1.cpp's FAX_v34): a DTE of the test's own
 *    goes through T.30 Annex F as fax software does (+F34: and CONNECT,
 *    the frames each to <DLE><ETX>, <DLE><pri> and <DLE><ctrl> for the
 *    channels, <DLE><EOT> at the end), against our T.30 machine on V.34
 *    on the audio, through G.711 with a little noise.  Sending and
 *    receiving
 *
 *    DSP_DEBUG=<level>   the engine's and the machine's log
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bits.h"
#include "dm_types.h"
#include "dsplibs.h"
#include "fax_util.h"
#include "modem_dp.h"
#include "modem_param.h"
#include "t30.h"
#include "t4.h"

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

#define DLE 0x10
#define ETX 0x03
#define EOT 0x04
#define V34_PRI 0x6B
#define V34_CTRL 0x6D
#define V34_C1200 0x6E
#define TICK 160                        /* samples, 20 ms */

/* the DTE's tty: what it sends the modem, and what it gets */
struct Tty {
	std::string in;                 /* to the modem */
	size_t in_pos = 0;
	std::string out;                /* to the DTE */
};

/* the modem core the pumps link against: nothing registered, S
   registers and parameters 0, the bits all taken and given */
int modem_dp_register(enum DP_ID, struct dp_operations *) { return 0; }
/* (nothing to deregister) */
void modem_dp_deregister(enum DP_ID, struct dp_operations *) {}
/* an S register: 0 */
int modem_get_sreg(struct modem *, unsigned int) { return 0; }
/* a parameter set: 0 */
long modem_set_param(struct modem *, unsigned, int) { return 0; }
/* a parameter: 0 (none) */
intptr_t modem_get_param(struct modem *, unsigned) { return 0; }
/* the bits asked for: n */
int modem_get_bits(struct modem *, int, u8 *, int n) { return n; }
/* the bits given: n */
int modem_put_bits(struct modem *, int, u8 *, int n) { return n; }

/* the engine's output to its DTE, onto the tty's out (the machine has
   no tty: its modem is NULL) */
int modem_send_to_tty(struct modem *m, const char *buf, int n)
{
	if (m)
		reinterpret_cast<Tty *>(m)->out.append(buf, n);
	return n;
}

/* up to n of what the DTE sent the engine: how many */
int modem_recv_from_tty(struct modem *m, char *buf, int n)
{
	Tty *t = reinterpret_cast<Tty *>(m);
	int k = 0;
	while (t && k < n && t->in_pos < t->in.size())
		buf[k++] = t->in[t->in_pos++];
	return k;
}

namespace {

/* DSP_DEBUG=<level>: the DSP's log to stderr */
const int debug_level = [] {
	const char *e = getenv("DSP_DEBUG");
	dsplibs_debug_level = e ? static_cast<unsigned>(atoi(e)) : 0;
	return 0;
}();

/* a standard resolution page of 240 rows: bars that move along it */
void make_page(T4Page *p)
{
	unsigned char row[T4_STRIDE(T4_WIDTH)];
	p->init(T4_WIDTH, 0);
	for (int y = 0; y < 240; y++) {
		memset(row, 0, sizeof(row));
		for (int x = 0; x < T4_WIDTH; x++)
			if ((x + 3 * y) % 200 < 20 + y % 30)
				row[x / 8] |= 0x80 >> (x % 8);
		p->add_row(row);
	}
}

/* the page in MH with RTC, the octets the first bit lowest */
std::string page_octets(const T4Page &p)
{
	T4Bits b = {};
	std::string s;
	p.encode_mh(&b, 0, 0, 1);
	s.assign((b.n + 7) / 8, '\0');
	for (size_t i = 0; i < b.n; i++)
		if (t4_bits_get(&b, i))
			s[i / 8] |= 1 << (i % 8);
	b.release();
	return s;
}

/* success if pages a and b have the same size and rows */
testing::AssertionResult same_page(const T4Page &a, const T4Page &b)
{
	if (a.width != b.width || a.rows != b.rows)
		return testing::AssertionFailure()
			<< a.width << "x" << a.rows << " against " << b.width
			<< "x" << b.rows;
	if (memcmp(a.bits.data(), b.bits.data(),
		   (size_t)a.rows * T4_STRIDE(a.width)))
		return testing::AssertionFailure() << "the rows differ";
	return testing::AssertionSuccess();
}

enum {
	FCF_DIS = 0x80, FCF_CSI = 0x40, FCF_DCS = 0x82, FCF_TSI = 0x42,
	FCF_CFR = 0x84, FCF_EOP = 0x2e, FCF_MCF = 0x8c, FCF_DCN = 0xfa,
	FCF_PPS = 0xbe, FCF_FCD = 0x06, FCF_RCP = 0x86,
};

/* a frame: the address, control (final or not), the FCF (with the
   caller's X bit), the FIF */
std::string frame(int fcf, bool final, bool caller,
		  const std::string &fif = "")
{
	std::string f;
	f += '\xff';
	f += final ? '\x13' : '\x03';
	f += (char)(fcf | (caller ? 1 : 0));
	return f + fif;
}

/* frame f's FCF, the caller's bit cleared (-1 if too short) */
int fcf_of(const std::string &f)
{
	return f.size() >= 3 ? (unsigned char)f[2] & 0xfe : -1;
}

/* a CSI / TSI: 20 characters, the last first */
std::string ident(const char *id)
{
	std::string s(20, ' ');
	int n = (int)strlen(id);
	for (int k = 0; k < n && k < 20; k++)
		s[k] = id[n - 1 - k];
	return s;
}

/* FIF bit (from 1, T.30's numbering) set */
void set_bit(std::string &fif, int bit)
{
	fif[(bit - 1) / 8] |= 1 << ((bit - 1) % 8);
}

/* FIF bit (from 1), 0 past its end */
int get_bit(const std::string &fif, int bit)
{
	if ((size_t)(bit - 1) / 8 >= fif.size())
		return 0;
	return (unsigned char)fif[(bit - 1) / 8] >> ((bit - 1) % 8) & 1;
}

/* DIS: V.8 (bit 6), V.27ter, V.29 and V.17, any length, ECM */
std::string dis_fif()
{
	std::string fif(4, '\0');
	set_bit(fif, 6);
	set_bit(fif, 10);
	set_bit(fif, 11), set_bit(fif, 12), set_bit(fif, 14);
	set_bit(fif, 19);
	set_bit(fif, 21), set_bit(fif, 22), set_bit(fif, 23);
	set_bit(fif, 24), set_bit(fif, 27);
	return fif;
}

/* DCS on V.34: no rate (its bits 0), any length, ECM with 256 octet
   frames */
std::string dcs_fif()
{
	std::string fif(4, '\0');
	set_bit(fif, 10);
	set_bit(fif, 19);
	set_bit(fif, 21), set_bit(fif, 22), set_bit(fif, 23);
	set_bit(fif, 24), set_bit(fif, 27);
	return fif;
}

/* what the engine gave its DTE: a frame (its FCS checked and taken
   off), or <DLE> and a code */
struct Item {
	int code;               /* the code after <DLE>; 0: a frame */
	std::string frame;
	bool fcs_ok;
};

struct World {
	Tty tty;
	void *fax = nullptr;
	T30 *t30 = nullptr;
	short to_fax[TICK] = { 0 }, to_t30[TICK] = { 0 };
	std::vector<int> results;
	long now = 0;
	double sigma = 3000 / pow(10, 40 / 20.0);
	T4Page sent = {}, got = {};
	int got_pages = 0;

	/* the engine, the machine and the pages freed */
	~World()
	{
		if (fax)
			FAX_delete(fax);
		if (t30)
			t30_free(t30);
		sent.release();
		got.release();
	}

	/* 20 ms of both, their samples a tick late, through G.711 */
	void tick()
	{
		short a[TICK], b[TICK];
		int st = FAX_process(fax, to_fax, a, TICK);
		if (st)
			results.push_back(st);
		t30_process(t30, to_t30, b, TICK);
		for (int i = 0; i < TICK; i++) {
			to_t30[i] = g711_ulaw((short)(a[i] + sigma * gauss()));
			to_fax[i] = g711_ulaw((short)(b[i] + sigma * gauss()));
		}
		now += TICK;
	}

	/* the next result code (FAX_STATUS_*), 0 if none came in secs */
	int result(double secs)
	{
		for (long end = now + (long)(secs * 8000); results.empty();) {
			if (now >= end)
				return 0;
			tick();
		}
		int r = results.front();
		results.erase(results.begin());
		return r;
	}

	/* the engine's +F34: line, up to its CR LF ("" if none in secs) */
	std::string f34(double secs)
	{
		for (long end = now + (long)(secs * 8000);;) {
			size_t at = tty.out.find("+F34:"), e;
			if (at != std::string::npos &&
			    (e = tty.out.find("\r\n", at)) != std::string::npos) {
				std::string s = tty.out.substr(at, e - at);
				tty.out.erase(0, e + 2);
				return s;
			}
			if (now >= end)
				return "";
			tick();
		}
	}

	/* the next item for the DTE; false if none came in secs */
	bool item(double secs, Item *it)
	{
		for (long end = now + (long)(secs * 8000);;) {
			std::string f;
			size_t i;
			for (i = 0; i + 1 < tty.out.size(); i++) {
				unsigned char c = tty.out[i];
				if (c != DLE) {
					f += (char)c;
					continue;
				}
				c = tty.out[++i];
				if (c == DLE) {
					f += (char)DLE;
					continue;
				}
				if (c == ETX && f.size() >= 2) {
					Bytes d(f.begin(), f.end());
					it->code = 0;
					it->fcs_ok = hdlc_fcs(d.data(),
						(int)d.size()) == HDLC_FCS_GOOD;
					it->frame = f.substr(0, f.size() - 2);
					tty.out.erase(0, i + 1);
					return true;
				}
				if (c != ETX && f.empty()) {
					it->code = c;
					it->frame.clear();
					tty.out.erase(0, i + 1);
					return true;
				}
			}
			if (now >= end)
				return false;
			tick();
		}
	}

	/* the frames of a message, up to the final one (their codes
	   skipped) */
	std::vector<std::string> message(double secs = 10)
	{
		std::vector<std::string> m;
		Item it;
		while (m.size() < 8 && item(secs, &it)) {
			if (it.code)
				continue;
			EXPECT_TRUE(it.fcs_ok);
			m.push_back(it.frame);
			if (it.frame.size() >= 2 && (it.frame[1] & 0x10))
				break;
		}
		return m;
	}

	/* a code from the engine (others before it skipped up to its
	   frames): whether code and then next came */
	bool code(int c, int next, double secs = 10)
	{
		Item it;
		while (item(secs, &it)) {
			if (it.code != c)
				continue;
			if (!next)
				return true;
			return item(1, &it) && it.code == next;
		}
		return false;
	}

	/* a frame to the engine, <DLE> shielded, then <DLE><ETX> */
	void send(const std::string &s)
	{
		for (char c : s) {
			if (c == DLE)
				tty.in += (char)DLE;
			tty.in += c;
		}
		tty.in += (char)DLE;
		tty.in += (char)ETX;
	}

	/* <DLE> and c to the engine */
	void send_code(int c)
	{
		tty.in += (char)DLE;
		tty.in += (char)c;
	}
};

/* the machine's page to send (the test page, kept in sent): 1, or 0
   after the one page */
s16 t30_next_page(void *ctx, T4Page *p)
{
	World *w = static_cast<World *>(ctx);
	if (w->sent.rows)
		return 0;
	make_page(&w->sent);
	make_page(p);
	return 1;
}

/* a page the machine received, copied into got */
void t30_page_received(void *ctx, T4Page *p)
{
	World *w = static_cast<World *>(ctx);
	w->got_pages++;
	w->got.release();
	w->got.init(p->width, p->fine);
	for (int y = 0; y < p->rows; y++)
		w->got.add_row(p->bits.data() +
			       (size_t)y * T4_STRIDE(p->width));
}

/* the machine on V.34 (sending if the engine receives), the engine with
   class 1.0's V.34 up to maxp * 2400 bit/s; CONNECT and +F34: come */
void start(World &w, bool engine_sends, unsigned maxp, int *prate)
{
	struct t30_config c = {};
	c.srate = 8000;
	c.max_bps = 14400;
	c.ecm = 1;
	c.sending = !engine_sends;
	c.ident = engine_sends ? "+1 555 0199" : "+1 555 0100";
	if (c.sending)
		c.next_page = t30_next_page;
	else
		c.page_received = t30_page_received;
	c.ctx = &w;
	c.v34 = t30_v34();
	w.t30 = t30_create(&c);
	ASSERT_TRUE(w.t30);
	w.fax = FAX_create(reinterpret_cast<struct modem *>(&w.tty),
			   engine_sends, 8000);
	ASSERT_TRUE(w.fax);
	ASSERT_EQ(FAX_v34(w.fax, maxp, 1, 0), 0);
	ASSERT_EQ(w.result(30), FAX_STATUS_CONNECT);
	std::string s = w.f34(1);
	ASSERT_EQ(s.substr(0, 5), "+F34:");
	ASSERT_EQ(s.substr(s.size() - 2), ",1");
	*prate = atoi(s.c_str() + 5);
	EXPECT_GE(*prate, 1);
	EXPECT_LE(*prate, (int)maxp);
	/* the control channel at 1200 bit/s */
	ASSERT_TRUE(w.code(V34_CTRL, V34_C1200, 1));
}

/* the machine's call over: how it went */
void finish(World &w, struct t30_status *s)
{
	for (long n = 0; n < 8000L * 20; n += TICK) {
		t30_get_status(w.t30, s);
		if (s->done)
			break;
		w.tick();
	}
	t30_get_status(w.t30, s);
}

/* the engine's DTE sends a page on the primary channel */
void engine_sends(unsigned maxp)
{
	World w;
	struct t30_status s = {};
	T4Page page = {};
	int prate = 0;

	ASSERT_NO_FATAL_FAILURE(start(w, true, maxp, &prate));
	make_page(&page);
	std::string doc = page_octets(page);

	/* the DIS (after the CSI) on the control channel */
	std::vector<std::string> m = w.message(20);
	ASSERT_GE(m.size(), 1u);
	ASSERT_EQ(fcf_of(m.back()), FCF_DIS);
	std::string dis = m.back().substr(3);
	EXPECT_TRUE(get_bit(dis, 6));
	EXPECT_TRUE(get_bit(dis, 27));
	/* TSI and DCS: no TCF, CFR at once */
	w.send(frame(FCF_TSI, false, true, ident("+1 555 0100")));
	w.send(frame(FCF_DCS, true, true, dcs_fif()));
	m = w.message();
	ASSERT_EQ(m.size(), 1u);
	ASSERT_EQ(fcf_of(m[0]), FCF_CFR);
	/* the block on the primary channel: <DLE><pri>, the frames once
	   it is up, <DLE><ctrl> after them */
	w.send_code(V34_PRI);
	ASSERT_TRUE(w.code(V34_PRI, 0x6F + prate));
	int n = 0;
	for (size_t at = 0; at < doc.size(); at += 256, n++) {
		std::string d = doc.substr(at, 256);
		d.resize(256, '\0');
		w.send(frame(FCF_FCD, false, true,
			     std::string(1, (char)n) + d));
	}
	w.send_code(V34_CTRL);
	ASSERT_TRUE(w.code(V34_CTRL, V34_C1200, 60));
	w.send(frame(FCF_PPS, true, true,
		     std::string({ (char)(FCF_EOP | 1), 0, 0,
				   (char)(n - 1) })));
	m = w.message();
	ASSERT_EQ(m.size(), 1u);
	ASSERT_EQ(fcf_of(m[0]), FCF_MCF);
	w.send(frame(FCF_DCN, true, true));
	finish(w, &s);
	/* the DTE's <DLE><EOT>: OK */
	w.send_code(EOT);
	EXPECT_EQ(w.result(2), FAX_STATUS_OK);
	EXPECT_EQ(s.result, T30_OK) << t30_result_text(s.result);
	EXPECT_TRUE(s.v34);
	EXPECT_EQ(s.bps, prate * 2400);
	EXPECT_EQ(s.pages, 1);
	EXPECT_STREQ(s.remote_ident, "+1 555 0100");
	EXPECT_EQ(w.got_pages, 1);
	EXPECT_TRUE(same_page(w.got, page));
	EXPECT_EQ(w.tty.in_pos, w.tty.in.size());
	printf("a page at %d bit/s on V.34 in %.1f s\n", prate * 2400,
	       w.now / 8000.0);
	page.release();
}

/* to 33600 bit/s */
TEST(Class1V34, Sends)
{
	engine_sends(14);
}

/* +F34=6: no more than 14400 bit/s */
TEST(Class1V34, SendsCapped)
{
	engine_sends(6);
}

/* the machine sends, the engine's DTE receives the block */
TEST(Class1V34, Receives)
{
	World w;
	struct t30_status s = {};
	T4Page page = {}, got = {};
	int prate = 0;
	Item it;

	ASSERT_NO_FATAL_FAILURE(start(w, false, 14, &prate));
	make_page(&page);
	w.send(frame(FCF_CSI, false, false, ident("+1 555 0199")));
	w.send(frame(FCF_DIS, true, false, dis_fif()));
	std::vector<std::string> m = w.message(20);
	ASSERT_EQ(m.size(), 2u);
	EXPECT_EQ(fcf_of(m[0]), FCF_TSI);
	ASSERT_EQ(fcf_of(m[1]), FCF_DCS);
	EXPECT_TRUE(get_bit(m[1].substr(3), 27));
	w.send(frame(FCF_CFR, true, false));
	/* the primary channel up, its frames, the control channel back */
	ASSERT_TRUE(w.code(V34_PRI, 0x6F + prate, 20));
	std::string block;
	int fcds = 0;
	for (;;) {
		ASSERT_TRUE(w.item(60, &it));
		if (it.code == V34_CTRL)
			break;
		if (it.code)
			continue;
		EXPECT_TRUE(it.fcs_ok);
		if (fcf_of(it.frame) == FCF_FCD) {
			ASSERT_EQ(it.frame.size(), 4u + 256);
			EXPECT_EQ((unsigned char)it.frame[3], fcds);
			block += it.frame.substr(4);
			fcds++;
		}
	}
	ASSERT_TRUE(w.item(1, &it));
	EXPECT_EQ(it.code, V34_C1200);
	m = w.message();
	ASSERT_EQ(m.size(), 1u);
	ASSERT_EQ(fcf_of(m[0]), FCF_PPS);
	ASSERT_GE(m[0].size(), 7u);
	EXPECT_EQ((unsigned char)m[0][3] & 0xfe, FCF_EOP);
	EXPECT_EQ((unsigned char)m[0][6] + 1, fcds);
	got.init(T4_WIDTH, 0);
	T4Rx rx = {};
	rx.init(&got, 0);
	for (size_t i = 0; i < block.size() * 8 && !rx.done; i++)
		rx.bit((unsigned char)block[i / 8] >> (i % 8) & 1);
	rx.end();
	rx.release();
	EXPECT_TRUE(same_page(got, page));
	w.send(frame(FCF_MCF, true, false));
	m = w.message();
	ASSERT_EQ(m.size(), 1u);
	EXPECT_EQ(fcf_of(m[0]), FCF_DCN);
	w.send_code(EOT);
	EXPECT_EQ(w.result(2), FAX_STATUS_OK);
	finish(w, &s);
	EXPECT_EQ(s.result, T30_OK) << t30_result_text(s.result);
	EXPECT_TRUE(s.v34);
	EXPECT_EQ(s.pages, 1);
	EXPECT_STREQ(s.remote_ident, "+1 555 0199");
	printf("a page at %d bit/s on V.34 in %.1f s\n", prate * 2400,
	       w.now / 8000.0);
	page.release();
	got.release();
}

} // namespace

namespace {

/* the machine without V.34: the engine (with V.34) on class 1's way */
void without_v34(World &w, bool engine_sends)
{
	struct t30_config c = {};
	c.srate = 8000;
	c.max_bps = 14400;
	c.ecm = 1;
	c.sending = !engine_sends;
	c.ident = "+1 555 0199";
	if (c.sending)
		c.next_page = t30_next_page;
	else
		c.page_received = t30_page_received;
	c.ctx = &w;
	w.t30 = t30_create(&c);
	ASSERT_TRUE(w.t30);
	w.fax = FAX_create(reinterpret_cast<struct modem *>(&w.tty),
			   engine_sends, 8000);
	ASSERT_TRUE(w.fax);
	ASSERT_EQ(FAX_v34(w.fax, 14, 1, 0), 0);
}

/* calling: V.21's flags drop the modem; +FRH's CONNECT, the CSI and DIS
   as class 1 has them, no +F34: */
TEST(Class1V34, CallsOneWithout)
{
	World w;
	Item it;
	ASSERT_NO_FATAL_FAILURE(without_v34(w, true));
	ASSERT_EQ(w.result(20), FAX_STATUS_CONNECT);
	ASSERT_TRUE(w.item(10, &it));
	EXPECT_EQ(it.code, 0);
	EXPECT_TRUE(it.fcs_ok);
	EXPECT_EQ(fcf_of(it.frame), FCF_CSI);
	EXPECT_EQ(w.tty.out.find("+F34:"), std::string::npos);
}

/* answering a caller without V.8 (no CM): the ANSam was the CED, then
   +FTH's CONNECT for the DTE's DIS */
TEST(Class1V34, AnswersOneWithout)
{
	World w;
	ASSERT_NO_FATAL_FAILURE(without_v34(w, false));
	ASSERT_EQ(w.result(20), FAX_STATUS_CONNECT);
	EXPECT_EQ(w.tty.out.find("+F34:"), std::string::npos);
	w.send(frame(FCF_DIS, true, false, dis_fif()));
	EXPECT_EQ(w.result(10), FAX_STATUS_OK);
}

} // namespace
