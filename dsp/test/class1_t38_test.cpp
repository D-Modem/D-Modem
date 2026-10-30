/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    class1_t38_test.cpp - fax class 1 over T.38 (fax_class1.cpp's
 *    FAX_t38_*): a DTE of the test's own goes through T.30 with the
 *    class 1 commands, as fax software does, and the engine's IFP packets
 *    go to and come from our T.30 machine on T.38.  Sending and
 *    receiving, without and with error correction (the DTE's own HDLC
 *    on +FTM / +FRM), both versions of the packets, and T.38 coming
 *    after a while on the audio
 *
 *    DSP_DEBUG=<level>   the engine's and the machine's log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "bits.h"
#include "dsp.h"
#include "fax.h"
#include "fax_util.h"
#include "t30.h"
#include "t38.h"
#include <dsplibs.h>

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

#define DLE 0x10
#define ETX 0x03
#define TICK 160                        /* samples, 20 ms */

/* the DTE's tty: what it sends the modem, and what it gets */
struct Tty {
	std::string in;                 /* to the modem */
	size_t in_pos = 0;
	std::string out;                /* to the DTE */
};

/* the modem's output to its DTE, onto the tty's out: all n of it */
int modem_send_to_tty(struct modem *m, const char *buf, int n)
{
	reinterpret_cast<Tty *>(m)->out.append(buf, n);
	return n;
}

/* up to n of what the DTE sent the modem, from the tty's in: how many */
int modem_recv_from_tty(struct modem *m, char *buf, int n)
{
	Tty *t = reinterpret_cast<Tty *>(m);
	int k = 0;
	while (k < n && t->in_pos < t->in.size())
		buf[k++] = t->in[t->in_pos++];
	return k;
}


/*
 *    the test page
 */

/* a standard resolution page of 240 rows: bars that move along it */
static void make_page(T4Page *p)
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

/* the page in MH with RTC, the octets the first bit lowest (as the DTE
   has them) */
static std::string page_octets(const T4Page &p)
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

/* success if pages a and b are the same size with the same rows; else
   what differs */
static testing::AssertionResult same_page(const T4Page &a, const T4Page &b)
{
	if (a.width != b.width || a.rows != b.rows)
		return testing::AssertionFailure()
			<< a.width << "x" << a.rows << " against " << b.width
			<< "x" << b.rows;
	if (memcmp(a.bits.data(), b.bits.data(), (size_t)a.rows * T4_STRIDE(a.width)))
		return testing::AssertionFailure() << "the rows differ";
	return testing::AssertionSuccess();
}


/*
 *    HDLC, the DTE's own (ECM on +FTM / +FRM)
 */

/* frames as an HDLC bit stream: flags, each frame with its FCS and the
   stuffed zeros and a flag after it, closing flags, ones to the end of
   the octet; the octets the first bit lowest */
static std::string hdlc_encode(const std::vector<std::string> &frames,
			       int lead_flags)
{
	std::vector<int> bits;
	auto flag = [&bits]() {
		for (int k = 0; k < 8; k++)
			bits.push_back(0x7e >> k & 1);
	};
	for (int i = 0; i < lead_flags; i++)
		flag();
	for (const std::string &f : frames) {
		Bytes d(f.begin(), f.end());
		unsigned crc = hdlc_fcs(d.data(), (int)d.size()) ^ 0xffff;
		int ones = 0;
		d.push_back(crc & 0xff);
		d.push_back(crc >> 8);
		for (unsigned char c : d)
			for (int k = 0; k < 8; k++) {
				int bit = c >> k & 1;
				bits.push_back(bit);
				if (bit && ++ones == 5) {
					bits.push_back(0);
					ones = 0;
				} else if (!bit) {
					ones = 0;
				}
			}
		flag();
	}
	flag();
	flag();
	while (bits.size() % 8)
		bits.push_back(1);
	std::string s(bits.size() / 8, '\0');
	for (size_t i = 0; i < bits.size(); i++)
		if (bits[i])
			s[i / 8] |= 1 << (i % 8);
	return s;
}

/* the frames in an HDLC bit stream (octets the first bit lowest), with
   good FCSs (and stripped of them); bad counts the others */
static std::vector<std::string> hdlc_decode(const std::string &s, int *bad)
{
	std::vector<std::string> frames;
	std::string cur;
	int ones = 0, byte = 0, nbits = 0;
	*bad = 0;
	for (size_t i = 0; i < s.size() * 8; i++) {
		int bit = (unsigned char)s[i / 8] >> (i % 8) & 1;
		if (bit) {
			if (++ones > 6) {
				cur.clear(), byte = nbits = 0;
				continue;
			}
		} else {
			if (ones == 6) {
				/* a flag: its 0111111 went in already */
				if (nbits == 7 && cur.size() >= 4) {
					Bytes d(cur.begin(), cur.end());
					if (hdlc_fcs(d.data(), (int)d.size()) ==
					    HDLC_FCS_GOOD)
						frames.push_back(cur.substr(0,
							cur.size() - 2));
					else
						++*bad;
				}
				cur.clear(), byte = nbits = ones = 0;
				continue;
			}
			if (ones == 5) {
				ones = 0;
				continue;
			}
			ones = 0;
		}
		byte |= bit << nbits;
		if (++nbits == 8) {
			cur += (char)byte;
			byte = nbits = 0;
		}
	}
	return frames;
}


/*
 *    T.30's frames
 */

enum {
	FCF_DIS = 0x80, FCF_CSI = 0x40, FCF_DCS = 0x82, FCF_TSI = 0x42,
	FCF_CFR = 0x84, FCF_EOP = 0x2e, FCF_MCF = 0x8c, FCF_DCN = 0xfa,
	FCF_PPS = 0xbe, FCF_FCD = 0x06, FCF_RCP = 0x86,
};

/* a frame: the address, control (final or not), the FCF (with the X
   bit: the caller's), the FIF */
static std::string frame(int fcf, bool final, bool caller,
			 const std::string &fif = "")
{
	std::string f;
	f += '\xff';
	f += final ? '\x13' : '\x03';
	f += (char)(fcf | (caller ? 1 : 0));
	return f + fif;
}

/* frame f's FCF, the caller's bit cleared (-1 if it is too short) */
static int fcf_of(const std::string &f)
{
	return f.size() >= 3 ? (unsigned char)f[2] & 0xfe : -1;
}

/* a CSI / TSI: 20 characters, the last first */
static std::string ident(const char *id)
{
	std::string s(20, ' ');
	int n = (int)strlen(id);
	for (int k = 0; k < n && k < 20; k++)
		s[k] = id[n - 1 - k];
	return s;
}

/* FIF bit bit (from 1, T.30's numbering) set */
static void set_bit(std::string &fif, int bit)
{
	fif[(bit - 1) / 8] |= 1 << ((bit - 1) % 8);
}

/* FIF bit bit (from 1), 0 past its end */
static int get_bit(const std::string &fif, int bit)
{
	if ((size_t)(bit - 1) / 8 >= fif.size())
		return 0;
	return (unsigned char)fif[(bit - 1) / 8] >> ((bit - 1) % 8) & 1;
}

/* DIS: V.27ter, V.29 and V.17, any length, 0 ms; ECM if ecm */
static std::string dis_fif(bool ecm)
{
	std::string fif(ecm ? 4 : 3, '\0');
	set_bit(fif, 10);
	set_bit(fif, 11), set_bit(fif, 12), set_bit(fif, 14);
	set_bit(fif, 19);
	set_bit(fif, 21), set_bit(fif, 22), set_bit(fif, 23);
	if (ecm)
		set_bit(fif, 24), set_bit(fif, 27);
	return fif;
}

/* DCS: V.17 14400, any length, 0 ms; ECM with 256 octet frames */
static std::string dcs_fif(bool ecm)
{
	std::string fif(ecm ? 4 : 3, '\0');
	set_bit(fif, 10);
	set_bit(fif, 14);
	set_bit(fif, 19);
	set_bit(fif, 21), set_bit(fif, 22), set_bit(fif, 23);
	if (ecm)
		set_bit(fif, 24), set_bit(fif, 27);
	return fif;
}

/* the page as FCD frames of 256 octets (the last padded), then RCP three
   times */
static std::vector<std::string> ecm_block(const std::string &doc)
{
	std::vector<std::string> frames;
	for (size_t at = 0, k = 0; at < doc.size(); at += 256, k++) {
		std::string d = doc.substr(at, 256);
		d.resize(256, '\0');
		frames.push_back(frame(FCF_FCD, false, false,
				       std::string(1, (char)k) + d));
	}
	for (int i = 0; i < 3; i++)
		frames.push_back(frame(FCF_RCP, false, false));
	return frames;
}


/*
 *    the engine, our T.30 machine, and the packets between them
 */

struct Params {
	bool engine_sends;      /* the engine's DTE sends, the machine
				   receives; else the other way round */
	bool ecm;
	int version;            /* T38FaxVersion */
	int audio_ms;           /* on the audio before T.38 */
};

/* p, for the test's name: who sends, ECM, the version and the audio */
static void PrintTo(const Params &p, std::ostream *os)
{
	*os << (p.engine_sends ? "sends" : "receives")
	    << (p.ecm ? " ecm" : "") << " v" << p.version << " after "
	    << p.audio_ms << " ms";
}

struct World {
	Tty tty;
	void *fax = nullptr;
	T30 *t30 = nullptr;
	int version = 0;
	bool t38 = false;
	std::deque<Bytes> to_fax, to_t30;       /* a tick late */
	std::deque<int> results;
	long now = 0;
	long loud = 0;                  /* samples not silent on T.38 */
	bool log = false;               /* the engine's packets kept */
	std::vector<std::pair<long, Bytes>> sent_log;
	T4Page sent = {}, got = {};     /* the machine's pages */
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

	void tick();
	int result(double secs);
	bool data(double secs, std::string *d);
	void send(const std::string &s);
};

/* an engine's packet, encoded, onto the line */
static void fax_tx(void *ctx, const void *ifp)
{
	World *w = static_cast<World *>(ctx);
	unsigned char buf[UDPTL_IFP_MAX];
	int n = t38_ifp_encode(static_cast<const struct t38_ifp *>(ifp),
			       w->version, buf, sizeof(buf));
	ASSERT_GT(n, 0);
	w->to_t30.push_back(Bytes(buf, buf + n));
	if (w->log)
		w->sent_log.push_back({ w->now, Bytes(buf, buf + n) });
}

/* the machine's packet, encoded, onto the line to the engine */
static void t30_tx(void *ctx, const struct t38_ifp *p)
{
	World *w = static_cast<World *>(ctx);
	unsigned char buf[UDPTL_IFP_MAX];
	int n = t38_ifp_encode(p, w->version, buf, sizeof(buf));
	ASSERT_GT(n, 0);
	w->to_fax.push_back(Bytes(buf, buf + n));
}

/* the machine's page to send (the test page, kept in sent): 1, or 0
   after the one page */
static s16 t30_next_page(void *ctx, T4Page *p)
{
	World *w = static_cast<World *>(ctx);
	if (w->sent.rows)
		return 0;               /* one page */
	make_page(&w->sent);
	make_page(p);
	return 1;
}

/* a page the machine received, copied into got */
static void t30_page_received(void *ctx, T4Page *p)
{
	World *w = static_cast<World *>(ctx);
	w->got_pages++;
	w->got.release();
	w->got.init(p->width, p->fine);
	for (int y = 0; y < p->rows; y++)
		w->got.add_row(p->bits.data() + (size_t)y * T4_STRIDE(p->width));
}

/* 20 ms: the packets of the last tick there, the engine's samples (on
   T.38 its time, silence out), the machine's time */
void World::tick()
{
	std::deque<Bytes> a, b;
	short in[TICK] = { 0 }, out[TICK];
	struct t38_ifp p;
	a.swap(to_fax);
	b.swap(to_t30);
	for (const Bytes &d : a) {
		ASSERT_EQ(t38_ifp_decode(&p, version, d.data(), (int)d.size()),
			  0);
		FAX_t38_rx(fax, &p);
	}
	for (const Bytes &d : b) {
		ASSERT_EQ(t38_ifp_decode(&p, version, d.data(), (int)d.size()),
			  0);
		t30_t38_rx(t30, &p);
	}
	int st = FAX_process(fax, in, out, TICK);
	if (st)
		results.push_back(st);
	if (t38)
		for (int i = 0; i < TICK; i++)
			loud += out[i] != 0;
	t30_t38_time(t30, TICK);
	now += TICK;
}

/* the next result code (FAX_STATUS_*), 0 if none came in secs */
int World::result(double secs)
{
	for (long end = now + (long)(secs * 8000); results.empty(); ) {
		if (now >= end)
			return 0;
		tick();
	}
	int r = results.front();
	results.pop_front();
	return r;
}

/* the modem's data up to <DLE><ETX>, the <DLE>s taken out; false if it
   did not end in secs */
bool World::data(double secs, std::string *d)
{
	size_t i = 0;
	d->clear();
	for (long end = now + (long)(secs * 8000);; ) {
		for (; i < tty.out.size(); i++) {
			unsigned char c = tty.out[i];
			if (c != DLE) {
				*d += (char)c;
				continue;
			}
			if (i + 1 >= tty.out.size())
				break;
			c = tty.out[++i];
			if (c == ETX) {
				tty.out.erase(0, i + 1);
				return true;
			}
			if (c == DLE)
				*d += (char)DLE;
		}
		if (now >= end)
			return false;
		tick();
	}
}

/* data to the modem, <DLE> shielded, then <DLE><ETX> */
void World::send(const std::string &s)
{
	for (char c : s) {
		if (c == DLE)
			tty.in += (char)DLE;
		tty.in += c;
	}
	tty.in += (char)DLE;
	tty.in += (char)ETX;
}

/* the DTE: class 1 commands, as fax software gives them */
struct Dte {
	World &w;

	/* a command, its first result */
	int cmd(unsigned c, unsigned param, double secs = 10)
	{
		if (FAX_class1_command(w.fax, c, param) != 1)
			return -1;
		return w.result(secs);
	}

	/* +FRH=3: a frame (its FCS checked and taken off), "" on failure */
	std::string frh(double secs = 10)
	{
		std::string d;
		int r = cmd(FAX_CMD_FRH, 3, secs);
		EXPECT_EQ(r, FAX_STATUS_CONNECT) << "+FRH=3";
		if (r != FAX_STATUS_CONNECT || !w.data(5, &d))
			return "";
		r = w.result(1);
		EXPECT_EQ(r, FAX_STATUS_OK) << "+FRH=3's frame";
		if (r != FAX_STATUS_OK || d.size() < 5)
			return "";
		Bytes b(d.begin(), d.end());
		EXPECT_EQ(hdlc_fcs(b.data(), (int)b.size()), HDLC_FCS_GOOD);
		return d.substr(0, d.size() - 2);
	}

	/* the frames of a message, up to the final one */
	std::vector<std::string> message(double secs = 10)
	{
		std::vector<std::string> m;
		for (int k = 0; k < 8; k++) {
			std::string f = frh(secs);
			if (f.empty())
				break;
			m.push_back(f);
			if (f[1] & 0x10)
				break;
		}
		return m;
	}

	/* the frames out with +FTH=3 (the result after the first given:
	   CED's FTH has its CONNECT already) */
	bool fth(const std::vector<std::string> &frames, bool command = true)
	{
		int r = command ? cmd(FAX_CMD_FTH, 3) : w.result(10);
		EXPECT_EQ(r, FAX_STATUS_CONNECT) << "+FTH=3";
		if (r != FAX_STATUS_CONNECT)
			return false;
		for (size_t k = 0; k < frames.size(); k++) {
			w.send(frames[k]);
			r = w.result(10);
			int want = k + 1 < frames.size() ? FAX_STATUS_CONNECT :
				FAX_STATUS_OK;
			EXPECT_EQ(r, want) << "+FTH=3, frame " << k;
			if (r != want)
				return false;
		}
		return w.tty.in_pos == w.tty.in.size();
	}

	/* +FTM=mod: data, then OK */
	bool ftm(unsigned mod, const std::string &d)
	{
		int r = cmd(FAX_CMD_FTM, mod);
		EXPECT_EQ(r, FAX_STATUS_CONNECT) << "+FTM=" << mod;
		if (r != FAX_STATUS_CONNECT)
			return false;
		w.send(d);
		r = w.result(120);
		EXPECT_EQ(r, FAX_STATUS_OK) << "+FTM=" << mod << "'s end";
		return r == FAX_STATUS_OK;
	}

	/* +FRM=mod: the data, up to NO CARRIER */
	bool frm(unsigned mod, std::string *d)
	{
		int r = cmd(FAX_CMD_FRM, mod);
		EXPECT_EQ(r, FAX_STATUS_CONNECT) << "+FRM=" << mod;
		if (r != FAX_STATUS_CONNECT || !w.data(120, d))
			return false;
		r = w.result(1);
		EXPECT_EQ(r, FAX_STATUS_NOCARRIER) << "+FRM=" << mod << "'s end";
		return r == FAX_STATUS_NOCARRIER;
	}

	/* +FTS=ms10: whether it gives OK */
	bool fts(unsigned ms10)
	{
		return cmd(FAX_CMD_FTS, ms10) == FAX_STATUS_OK;
	}
};


/*
 *    the calls
 */

class Class1T38 : public testing::TestWithParam<Params> {};

/* our T.30 machine on T.38, the engine on the audio for p.audio_ms
   first (the machine hearing nothing of it) */
static void start(World &w, const Params &p)
{
	struct t30_config c = {};
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	w.version = p.version;
	c.sending = !p.engine_sends;
	c.ident = p.engine_sends ? "+1 555 0199" : "+1 555 0100";
	c.max_bps = 14400;
	c.ecm = p.ecm;
	c.srate = 8000;
	if (c.sending)
		c.next_page = t30_next_page;
	else
		c.page_received = t30_page_received;
	c.ctx = &w;
	c.t38 = 1;
	c.t38_tx = t30_tx;
	w.t30 = t30_create(&c);
	ASSERT_TRUE(w.t30);
	w.fax = FAX_create(reinterpret_cast<struct modem *>(&w.tty),
			   p.engine_sends, 8000);
	ASSERT_TRUE(w.fax);
	for (long n = 0; n < p.audio_ms * 8; n += TICK) {
		short in[TICK] = { 0 }, out[TICK];
		ASSERT_EQ(FAX_process(w.fax, in, out, TICK), 0);
		/* (the machine's packets go nowhere) */
		w.to_fax.clear();
		t30_t38_time(w.t30, TICK);
	}
	FAX_t38_start(w.fax, fax_tx, &w, 0);
	w.t38 = true;
}

/* the machine's call over: how it went */
static void finish(World &w, struct t30_status *s)
{
	for (long n = 0; n < 8000L * 20; n += TICK) {
		t30_get_status(w.t30, s);
		if (s->done)
			break;
		w.tick();
	}
	t30_get_status(w.t30, s);
}

/* the engine's DTE sends a page to the machine */
TEST_P(Class1T38, Call)
{
	const Params &p = GetParam();
	World w;
	Dte dte{ w };
	struct t30_status s = {};
	T4Page page = {};
	std::string d;

	ASSERT_NO_FATAL_FAILURE(start(w, p));
	make_page(&page);
	std::string doc = page_octets(page);

	if (p.engine_sends) {
		/* the DIS (after the CSI) */
		std::vector<std::string> m = dte.message(40);
		ASSERT_EQ(m.size(), 2u);
		EXPECT_EQ(fcf_of(m[0]), FCF_CSI);
		ASSERT_EQ(fcf_of(m[1]), FCF_DIS);
		std::string dis = m[1].substr(3);
		EXPECT_TRUE(get_bit(dis, 14));
		EXPECT_EQ(get_bit(dis, 27), p.ecm);
		ASSERT_TRUE(dte.fth({ frame(FCF_TSI, false, true,
					    ident("+1 555 0100")),
				      frame(FCF_DCS, true, true,
					    dcs_fif(p.ecm)) }));
		ASSERT_TRUE(dte.fts(8));
		/* the TCF: 1.5 s of zeros */
		ASSERT_TRUE(dte.ftm(145, std::string(14400 * 3 / 2 / 8, '\0')));
		m = dte.message();
		ASSERT_EQ(m.size(), 1u);
		ASSERT_EQ(fcf_of(m[0]), FCF_CFR);
		if (p.ecm) {
			ASSERT_TRUE(dte.ftm(146, hdlc_encode(
				ecm_block(doc), 240)));
			ASSERT_TRUE(dte.fts(8));
			int n = (int)(doc.size() + 255) / 256;
			ASSERT_TRUE(dte.fth({ frame(FCF_PPS, true, true,
				std::string({ (char)(FCF_EOP | 1), 0, 0,
					      (char)(n - 1) })) }));
		} else {
			ASSERT_TRUE(dte.ftm(146, doc));
			ASSERT_TRUE(dte.fts(8));
			ASSERT_TRUE(dte.fth({ frame(FCF_EOP, true, true) }));
		}
		m = dte.message();
		ASSERT_EQ(m.size(), 1u);
		ASSERT_EQ(fcf_of(m[0]), FCF_MCF);
		ASSERT_TRUE(dte.fth({ frame(FCF_DCN, true, true) }));
		finish(w, &s);
		EXPECT_EQ(s.result, T30_OK) << t30_result_text(s.result);
		EXPECT_EQ(s.pages, 1);
		EXPECT_EQ(s.bps, 14400);
		EXPECT_EQ(!!s.ecm, p.ecm);
		EXPECT_STREQ(s.remote_ident, "+1 555 0100");
		EXPECT_EQ(w.got_pages, 1);
		EXPECT_TRUE(same_page(w.got, page));
	} else {
		/* CED, then the FTH=3 CSI and DIS go with */
		ASSERT_TRUE(dte.fth({ frame(FCF_CSI, false, false,
					    ident("+1 555 0199")),
				      frame(FCF_DIS, true, false,
					    dis_fif(p.ecm)) }, false));
		std::vector<std::string> m = dte.message(10);
		ASSERT_EQ(m.size(), 2u);
		EXPECT_EQ(fcf_of(m[0]), FCF_TSI);
		ASSERT_EQ(fcf_of(m[1]), FCF_DCS);
		std::string dcs = m[1].substr(3);
		/* V.17 14400 */
		EXPECT_TRUE(get_bit(dcs, 14) && !get_bit(dcs, 11) &&
			    !get_bit(dcs, 12) && !get_bit(dcs, 13));
		EXPECT_EQ(get_bit(dcs, 27), p.ecm);
		ASSERT_TRUE(dte.frm(145, &d));
		/* the TCF: zeros, as many as 1.5 s has */
		EXPECT_EQ(d.size(), 14400u * 3 / 2 / 8);
		EXPECT_EQ(d.find_first_not_of('\0'), std::string::npos);
		ASSERT_TRUE(dte.fth({ frame(FCF_CFR, true, false) }));
		ASSERT_TRUE(dte.frm(146, &d));
		T4Page got = {};
		got.init(T4_WIDTH, 0);
		if (p.ecm) {
			int bad;
			std::vector<std::string> fr = hdlc_decode(d, &bad);
			EXPECT_EQ(bad, 0);
			std::string block;
			int fcds = 0, rcps = 0;
			for (const std::string &f : fr) {
				if (fcf_of(f) == FCF_FCD) {
					ASSERT_EQ(f.size(), 4u + 256);
					EXPECT_EQ((unsigned char)f[3], fcds);
					block += f.substr(4);
					fcds++;
				} else if (fcf_of(f) == FCF_RCP) {
					rcps++;
				}
			}
			EXPECT_EQ(rcps, 3);
			d = block;
			m = dte.message();
			ASSERT_EQ(m.size(), 1u);
			ASSERT_EQ(fcf_of(m[0]), FCF_PPS);
			ASSERT_GE(m[0].size(), 7u);
			EXPECT_EQ((unsigned char)m[0][3] & 0xfe, FCF_EOP);
			EXPECT_EQ((unsigned char)m[0][6] + 1, fcds);
		} else {
			m = dte.message();
			ASSERT_EQ(m.size(), 1u);
			ASSERT_EQ(fcf_of(m[0]), FCF_EOP);
		}
		T4Rx rx = {};
		rx.init(&got, 0);
		for (size_t i = 0; i < d.size() * 8 && !rx.done; i++)
			rx.bit((unsigned char)d[i / 8] >> (i % 8) & 1);
		rx.end();
		rx.release();
		EXPECT_TRUE(same_page(got, page));
		got.release();
		ASSERT_TRUE(dte.fth({ frame(FCF_MCF, true, false) }));
		m = dte.message();
		ASSERT_EQ(m.size(), 1u);
		EXPECT_EQ(fcf_of(m[0]), FCF_DCN);
		finish(w, &s);
		EXPECT_EQ(s.result, T30_OK) << t30_result_text(s.result);
		EXPECT_EQ(s.pages, 1);
		EXPECT_EQ(!!s.ecm, p.ecm);
		EXPECT_STREQ(s.remote_ident, "+1 555 0199");
	}
	EXPECT_EQ(w.loud, 0) << "audio on T.38";
	EXPECT_EQ(w.tty.in_pos, w.tty.in.size());
	page.release();
}

INSTANTIATE_TEST_SUITE_P(Calls, Class1T38, testing::Values(
	Params{ true, false, 0, 0 }, Params{ true, true, 0, 0 },
	Params{ false, false, 0, 0 }, Params{ false, true, 0, 0 },
	Params{ true, false, 2, 0 }, Params{ false, true, 3, 0 },
	Params{ false, false, 0, 1000 }, Params{ true, true, 2, 500 }),
	/* <sends|receives>[_ecm]_v<version>[_after<ms>] */
	[](const testing::TestParamInfo<Params> &i) {
		return std::string(i.param.engine_sends ? "sends" :
				   "receives") +
			(i.param.ecm ? "_ecm" : "") + "_v" +
			std::to_string(i.param.version) +
			(i.param.audio_ms ? "_after" +
			 std::to_string(i.param.audio_ms) : std::string());
	});

/* the packets themselves: FTH=3's frame in V.21 packets of 7 octets at
   most a second after the preamble's indicator, its end FCS-OK-sig-end,
   no-signal 100 ms after it; FTM's T.4 data the T.38 way round after
   the training's indicator and time, its end T.4-sig-end; the times as
   the line's */
TEST(Class1T38Packets, FthAndFtm)
{
	World w;
	Params p{ false, false, 0, 0 };
	struct t38_ifp q;

	ASSERT_NO_FATAL_FAILURE(start(w, p));
	w.log = true;
	Dte dte{ w };
	/* CED's FTH: a DIS alone */
	ASSERT_EQ(w.result(10), FAX_STATUS_CONNECT);
	std::string dis = frame(FCF_DIS, true, false, dis_fif(false));
	w.send(dis);
	ASSERT_EQ(w.result(10), FAX_STATUS_OK);
	ASSERT_TRUE(dte.fts(8));
	std::string t4 = "\x01\x80\x10\xff\x00\x5a";
	t4 += std::string(1800, '\x55');
	ASSERT_TRUE(dte.ftm(146, t4));
	w.tick();

	/* the indicators and data in their order, with their times */
	long pre = -1, first = -1, end = -1, nosig = -1, ced = -1;
	long train = -1, t4_first = -1, t4_end = -1, nosig2 = -1;
	std::string hdlc, t4got;
	for (auto &e : w.sent_log) {
		ASSERT_EQ(t38_ifp_decode(&q, 0, e.second.data(),
					 (int)e.second.size()), 0);
		if (!q.data) {
			if (q.type == T38_IND_CED)
				ced = e.first;
			else if (q.type == T38_IND_V21_PREAMBLE)
				pre = e.first;
			else if (q.type == T38_IND_V17_14400_SHORT)
				train = e.first;
			else if (q.type == T38_IND_NO_SIGNAL) {
				if (end >= 0 && nosig < 0)
					nosig = e.first;
				else if (t4_end >= 0 && nosig2 < 0)
					nosig2 = e.first;
			}
			continue;
		}
		for (int k = 0; k < q.nfields; k++) {
			const struct t38_field &f = q.field[k];
			unsigned char b[UDPTL_IFP_MAX];
			if (q.type == T38_DATA_V21) {
				if (f.type == T38_HDLC_DATA) {
					EXPECT_LE(f.len, 7);
					if (first < 0)
						first = e.first;
					t38_reverse(b, f.data, f.len);
					hdlc.append((const char *)b, f.len);
				} else {
					EXPECT_EQ(f.type,
						  T38_HDLC_FCS_OK_SIG_END);
					end = e.first;
				}
			} else {
				ASSERT_EQ(q.type, T38_DATA_V17_14400);
				ASSERT_TRUE(f.type == T38_T4_DATA ||
					    f.type == T38_T4_SIG_END);
				if (t4_first < 0)
					t4_first = e.first;
				t38_reverse(b, f.data, f.len);
				t4got.append((const char *)b, f.len);
				if (f.type == T38_T4_SIG_END)
					t4_end = e.first;
			}
		}
	}
	/* CED after 200 ms of no-signal, 2.6 s of it before the preamble */
	EXPECT_NEAR(ced, 1600, TICK);
	EXPECT_NEAR(pre - ced, 8 * (FAX_CED_END_MS - ANS_SILENCE_MS), TICK);
	EXPECT_NEAR(first - pre, 8000, 2 * TICK);
	EXPECT_EQ(hdlc, dis);
	/* the frame's octets at 300 bit/s */
	EXPECT_NEAR(end - first, (long)dis.size() * 8 * 8000 / 300, 4 * TICK);
	EXPECT_NEAR(nosig - end, 800, 2 * TICK);
	/* +FTS=8, the training's 142 ms, the data at 14400 bit/s */
	EXPECT_GE(train - nosig, 640);
	EXPECT_NEAR(t4_first - train, 142 * 8, 2 * TICK);
	EXPECT_EQ(t4got, t4);
	EXPECT_NEAR(t4_end - t4_first, (long)t4.size() * 8 * 8000 / 14400,
		    3 * TICK);
	EXPECT_NEAR(nosig2 - t4_end, 480, 2 * TICK);
}

/* frames from the far end over T.38 to +FRH=3: CONNECT at the preamble,
   each frame with its FCS (a bad one's spoilt, ERROR), NO CARRIER when
   the signal goes without one; +FRS by the signals */
TEST(Class1T38Packets, Frh)
{
	World w;
	Params p{ true, false, 0, 0 };
	struct t38_ifp q = {};
	ASSERT_NO_FATAL_FAILURE(start(w, p));
	Dte dte{ w };
	std::string f1 = frame(FCF_CSI, false, false, ident("1234")),
		f2 = frame(FCF_DIS, true, false, dis_fif(false));
	unsigned char b1[64], b2[64];
	t38_reverse(b1, (const unsigned char *)f1.data(), (int)f1.size());
	t38_reverse(b2, (const unsigned char *)f2.data(), (int)f2.size());

	/* the engine is in +FRH=3 since its call: CONNECT at the preamble */
	auto send = [&w](const struct t38_ifp &x) {
		unsigned char buf[UDPTL_IFP_MAX];
		int n = t38_ifp_encode(&x, 0, buf, sizeof(buf));
		ASSERT_GT(n, 0);
		w.to_fax.push_back(Bytes(buf, buf + n));
	};
	q.type = T38_IND_V21_PREAMBLE;
	send(q);
	EXPECT_EQ(w.result(1), FAX_STATUS_CONNECT);
	q = {};
	q.data = 1;
	q.type = T38_DATA_V21;
	q.nfields = 2;
	q.field[0] = { T38_HDLC_DATA, b1, (int)f1.size() };
	q.field[1] = { T38_HDLC_FCS_OK, NULL, 0 };
	send(q);
	/* the second one bad */
	q.field[0] = { T38_HDLC_DATA, b2, (int)f2.size() };
	q.field[1] = { T38_HDLC_FCS_BAD_SIG_END, NULL, 0 };
	send(q);
	std::string d;
	ASSERT_TRUE(w.data(1, &d));
	EXPECT_EQ(w.result(1), FAX_STATUS_OK);
	ASSERT_EQ(d.size(), f1.size() + 2);
	EXPECT_EQ(d.substr(0, f1.size()), f1);
	Bytes bd(d.begin(), d.end());
	EXPECT_EQ(hdlc_fcs(bd.data(), (int)bd.size()), HDLC_FCS_GOOD);
	EXPECT_EQ(dte.cmd(FAX_CMD_FRH, 3), FAX_STATUS_CONNECT);
	ASSERT_TRUE(w.data(1, &d));
	EXPECT_EQ(w.result(1), FAX_STATUS_FRAME_ERROR);
	ASSERT_EQ(d.size(), f2.size() + 2);
	EXPECT_EQ(d.substr(0, f2.size()), f2);
	bd.assign(d.begin(), d.end());
	EXPECT_NE(hdlc_fcs(bd.data(), (int)bd.size()), HDLC_FCS_GOOD);

	/* a preamble, no-signal, nothing in between: NO CARRIER */
	EXPECT_EQ(FAX_class1_command(w.fax, FAX_CMD_FRH, 3), 1);
	q = {};
	q.type = T38_IND_V21_PREAMBLE;
	send(q);
	EXPECT_EQ(w.result(1), FAX_STATUS_CONNECT);
	q.type = T38_IND_NO_SIGNAL;
	send(q);
	ASSERT_TRUE(w.data(1, &d));
	EXPECT_EQ(d, "");
	EXPECT_EQ(w.result(1), FAX_STATUS_NOCARRIER);

	/* +FRS=20: 200 ms of no signal; a signal starts it over */
	q.type = T38_IND_V17_14400_SHORT;
	send(q);
	w.tick();
	EXPECT_EQ(FAX_class1_command(w.fax, FAX_CMD_FRS, 20), 1);
	long t = w.now;
	for (int k = 0; k < 20; k++)
		w.tick();
	EXPECT_TRUE(w.results.empty());
	q.type = T38_IND_NO_SIGNAL;
	send(q);
	EXPECT_EQ(w.result(1), FAX_STATUS_OK);
	EXPECT_NEAR(w.now - t, 8 * (400 + 200), 2 * TICK);
}

/* the DTE waits for silence (+FRS=1, as efax does after its TCF) while
   the far end's frames are already coming: the wait lasts until they
   are over, and the +FRH=3 that follows still gets them.  Here the
   machine's TSI and DCS come during it, after our DIS */
TEST(Class1T38Packets, FramesDuringFrs)
{
	World w;
	Params p{ false, false, 0, 0 };

	ASSERT_NO_FATAL_FAILURE(start(w, p));
	Dte dte{ w };
	ASSERT_EQ(w.result(10), FAX_STATUS_CONNECT);
	w.send(frame(FCF_DIS, true, false, dis_fif(false)));
	ASSERT_EQ(w.result(10), FAX_STATUS_OK);
	/* +FRS=1 once the machine's preamble is on its way */
	bool pre = false;
	for (long n = 0; n < 8000L * 5 && !pre; n += TICK) {
		w.tick();
		for (const Bytes &d : w.to_fax) {
			struct t38_ifp q;
			ASSERT_EQ(t38_ifp_decode(&q, 0, d.data(),
						 (int)d.size()), 0);
			pre = pre || (!q.data &&
				      q.type == T38_IND_V21_PREAMBLE);
		}
	}
	ASSERT_TRUE(pre) << "the machine's preamble";
	EXPECT_EQ(dte.cmd(FAX_CMD_FRS, 1), FAX_STATUS_OK);
	std::vector<std::string> m = dte.message(1);
	ASSERT_FALSE(m.empty()) << "the frames that came during +FRS=1";
	EXPECT_EQ(fcf_of(m.back()), FCF_DCS);
}
