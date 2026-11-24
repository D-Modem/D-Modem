/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hayes_test.cpp - tests of the Hayes V-series protocols (link/):
 *                     ADC against the firmware's own output, the XID
 *                     private sets and the selection of the
 *                     compression, AFT framing, and the Express 96
 *                     burst rules on a half-duplex loop-back line
 *
 *    hayes_test                              all of them
 *    hayes_test --gtest_filter=Hayes.Adc*    one group
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "aft.h"
#include "e96_burst.h"
#include "hayes_adc.h"
#include "hayes_adc_vectors.h"
#include "hayes_xid.h"
#include "link.h"

using namespace dlink;

static unsigned rnd_state = 1;

/* a repeatable pseudo-random number (rnd_state), 24 bits */
static unsigned rnd()
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return rnd_state >> 8;
}

/* the octets of a hexadecimal string */
static std::vector<u8> unhex(const char *s)
{
	std::vector<u8> v;
	for (; s[0] && s[1]; s += 2) {
		unsigned b = 0;
		sscanf(s, "%2x", &b);
		v.push_back(static_cast<u8>(b));
	}
	return v;
}

/* the hexadecimal string of v (for the messages of failed checks) */
static std::string hex(const std::vector<u8> &v)
{
	std::string s;
	char b[4];
	for (u8 c : v) {
		snprintf(b, sizeof(b), "%02x", c);
		s += b;
	}
	return s;
}


/*
 *    ADC
 *
 */

/* all the output of an ADC coder */
static std::vector<u8> take(HayesAdc &a)
{
	std::vector<u8> v;
	u8 buf[64];
	s16 n;
	while ((n = a.out(buf, sizeof(buf))) > 0)
		v.insert(v.end(), buf, buf + n);
	return v;
}

/* one frame through an encoder: each character, then the flush */
static std::vector<u8> enc_frame(HayesAdc &e, const std::vector<u8> &in)
{
	for (u8 c : in)
		e.encode(c);
	e.flush_encoder();
	return take(e);
}

/* one received frame through a decoder */
static std::vector<u8> dec_frame(HayesAdc &d, const std::vector<u8> &in)
{
	std::vector<u8> v;
	for (u8 c : in) {
		d.decode(c);
		std::vector<u8> o = take(d);
		v.insert(v.end(), o.begin(), o.end());
	}
	d.flush_decoder();
	std::vector<u8> o = take(d);
	v.insert(v.end(), o.begin(), o.end());
	return v;
}

/* the firmware's vectors: for each connection a new encoder, each
   frame put and flushed, the output the same as the firmware's octet
   for octet; the decoder gives back each frame's input */
TEST(Hayes, AdcFirmwareVectors)
{
	int frames = 0;
	for (const AdcConnectionVector &c : ADC_VECTORS) {
		HayesAdc enc, dec;
		enc.init();
		dec.init();
		for (int i = 0; i < c.count; i++) {
			const std::vector<u8> in = unhex(c.frames[i].in);
			const std::vector<u8> want = unhex(c.frames[i].out);
			const std::vector<u8> got = enc_frame(enc, in);
			EXPECT_EQ(got, want) << c.name << " frame " << i
				<< ": " << hex(got);
			EXPECT_EQ(dec_frame(dec, want), in)
				<< c.name << " frame " << i << " decoded";
			frames++;
		}
	}
	EXPECT_EQ(frames, 43);
}

/* round trips in frames of 128 characters: random octets, long runs
   (more than 15 and more than 30 equal characters), many FAh, a small
   alphabet; the stream goes on from frame to frame */
TEST(Hayes, AdcRoundTrip)
{
	rnd_state = 7;
	std::vector<std::vector<u8>> texts;
	std::vector<u8> v;
	for (int i = 0; i < 6000; i++)
		v.push_back(static_cast<u8>(rnd()));
	texts.push_back(v);
	v.clear();
	for (int len : { 4, 15, 16, 17, 30, 31, 32, 45, 200, 3, 1 })
		for (int i = 0; i < len; i++)
			v.push_back(static_cast<u8>('A' + len % 7));
	texts.push_back(v);
	v.assign(500, 0xfa);
	for (int i = 0; i < 300; i++)
		v.push_back(i % 3 ? 0xfa : static_cast<u8>(rnd()));
	texts.push_back(v);
	v.clear();
	static const u8 small[] = { 'a', 'b', ' ', '\r', '\n', 0xfa, 0x03,
				    0xff, 0x00 };
	for (int i = 0; i < 20000; i++)
		v.push_back(small[rnd() % sizeof(small)]);
	texts.push_back(v);

	for (const std::vector<u8> &t : texts) {
		HayesAdc enc, dec;
		enc.init();
		dec.init();
		std::vector<u8> back;
		size_t wire = 0;
		for (size_t i = 0; i < t.size(); i += 128) {
			size_t e = std::min(t.size(), i + 128);
			std::vector<u8> f = enc_frame(enc,
				std::vector<u8>(t.begin() + i, t.begin() + e));
			wire += f.size();
			std::vector<u8> o = dec_frame(dec, f);
			back.insert(back.end(), o.begin(), o.end());
		}
		EXPECT_EQ(back, t) << t.size() << " octets";
		printf("  adc: %zu octets -> %zu octets\n", t.size(), wire);
	}
}

/* FAh 00h and FAh 01h in a stream set the decoder's model to its
   initial values: what a new encoder codes after it decodes again */
TEST(Hayes, AdcDecoderReset)
{
	for (u8 second : { 0x00, 0x01 }) {
		HayesAdc enc, dec;
		enc.init();
		dec.init();
		std::vector<u8> a(400), b(300);
		for (auto &c : a)
			c = static_cast<u8>("etaoin shrdlu"[rnd() % 13]);
		for (auto &c : b)
			c = static_cast<u8>("zyxwvu 0123"[rnd() % 11]);
		std::vector<u8> fa = enc_frame(enc, a);
		EXPECT_EQ(dec_frame(dec, fa), a);
		/* a new connection's encoder after the reset code */
		HayesAdc enc2;
		enc2.init();
		std::vector<u8> fb = { 0xfa, second };
		std::vector<u8> code = enc_frame(enc2, b);
		fb.insert(fb.end(), code.begin(), code.end());
		EXPECT_EQ(dec_frame(dec, fb), b) << "after FAh " << int(second);
	}
}

/* the encoder never ends its output between FAh and the octet after
   it, whatever n out() is asked for */
TEST(Hayes, AdcEscapeNotSplit)
{
	HayesAdc enc;
	enc.init();
	for (int i = 0; i < 2000; i++)
		enc.encode(static_cast<u8>(rnd()));
	enc.flush_encoder();
	u8 buf[7];
	s16 n;
	int pieces = 0;
	while ((n = enc.out(buf, static_cast<s16>(1 + rnd() % 7))) > 0) {
		/* a lone FAh at the end only when nothing is left after it */
		if (buf[n - 1] == 0xfa) {
			EXPECT_EQ(enc.pending(), 0);
		}
		pieces++;
	}
	EXPECT_GT(pieces, 100);
}


/*
 *    XID
 *
 */

/* the Hayes XID with compression on (S69 7, so k 7; P0 03h; S46 138,
   so HMP P1 8Ah and P2 01h) is the template of FA 6.9 octet for octet */
TEST(Hayes, XidTemplate)
{
	const std::vector<u8> want = {
		0x82,
		0x80, 0x00, 0x13,
		0x03, 0x03, 0x8a, 0x89, 0x00,
		0x05, 0x02, 0x04, 0x00,
		0x06, 0x02, 0x04, 0x00,
		0x07, 0x01, 0x07,
		0x08, 0x01, 0x07,
		0xf0, 0x00, 0x0f,
		0x00, 0x03, 0x56, 0x34, 0x32,
		0x01, 0x01, 0x03,
		0x02, 0x02, 0x04, 0x00,
		0x03, 0x01, 0x20,
		0xff,
		0xff, 0x03, 0x41, 0x46, 0x54, 0x01, 0x01, 0x5a, 0x02, 0x01, 0x06,
		0xff, 0x03, 0x48, 0x4d, 0x50, 0x01, 0x01, 0x8a, 0x02, 0x01, 0x01,
	};
	u8 buf[HAYES_XID_LEN];
	s16 n = hayes_xid_template(buf, 7, 0x03, HAYES_S46_LAPM_COMP, 0x5a,
				   0x06);
	EXPECT_EQ(std::vector<u8>(buf, buf + n), want) << hex(
		std::vector<u8>(buf, buf + n));
	EXPECT_EQ(n, HAYES_XID_LEN);
	/* k is limited to 1 to 7; without compression HMP P2 is 0 */
	n = hayes_xid_template(buf, 15, 0x00, HAYES_S46_LAPM, 0, 0);
	EXPECT_EQ(buf[19], 7);
	EXPECT_EQ(buf[22], 7);
	EXPECT_EQ(buf[33], 0);
	EXPECT_EQ(buf[n - 4], HAYES_S46_LAPM);
	EXPECT_EQ(buf[n - 1], 0);
}

/* (PI, PV, PL) of a user data subfield (after its GI) to the parser */
static HayesSets parse_user(const std::vector<u8> &u)
{
	HayesSetParser p;
	for (size_t i = 0; i + 2 <= u.size();) {
		const u8 pi = u[i], pl = u[i + 1];
		if (i + 2 + pl > u.size())
			break;
		p.param(pi, u.data() + i + 2, pl);
		i += 2 + pl;
	}
	return p.sets();
}

/* the private part with the two sets, with an unknown set between them
   and a V.44 set after them, and without the sets */
TEST(Hayes, XidParser)
{
	u8 buf[HAYES_XID_LEN];
	const s16 n = hayes_xid_template(buf, 7, 0x03, HAYES_S46_LAPM_COMP,
					 0x5a, 0x06);
	/* the part after GI FFh */
	std::vector<u8> u(buf + 42, buf + n);
	HayesSets s = parse_user(u);
	EXPECT_TRUE(s.aft && s.hmp);
	EXPECT_EQ(s.aft_p1, 0x5a);
	EXPECT_EQ(s.aft_p2, 0x06);
	EXPECT_EQ(s.hmp_p1, 0x8a);
	EXPECT_EQ(s.hmp_p2, 0x01);

	/* an unknown set "XYZ" (its P1 and P2 ignored) between them, and
	   V.44's set (PI 40h) with a P1 and P2 of its own after HMP */
	std::vector<u8> v(u.begin(), u.begin() + 11);
	const u8 xyz[] = { 0xff, 0x03, 'X', 'Y', 'Z', 0x01, 0x01, 0xee,
			   0x02, 0x01, 0xee };
	v.insert(v.end(), xyz, xyz + sizeof(xyz));
	v.insert(v.end(), u.begin() + 11, u.end());
	const u8 v44[] = { 0x40, 0x03, 'V', '4', '4', 0x01, 0x01, 0x77,
			   0x02, 0x01, 0x77 };
	v.insert(v.end(), v44, v44 + sizeof(v44));
	s = parse_user(v);
	EXPECT_TRUE(s.aft && s.hmp);
	EXPECT_EQ(s.aft_p1, 0x5a);
	EXPECT_EQ(s.hmp_p1, 0x8a);
	EXPECT_EQ(s.hmp_p2, 0x01);

	/* only an unknown set: not a Hayes modem */
	s = parse_user(std::vector<u8>(xyz, xyz + sizeof(xyz)));
	EXPECT_FALSE(s.aft || s.hmp);
	s = parse_user({});
	EXPECT_FALSE(s.aft || s.hmp);
}

/* the results and the three rules of the selection (FA 6.8.1, 6.9) */
TEST(Hayes, XidSelection)
{
	const HayesSets comp = hayes_local(HAYES_S46_LAPM_COMP, 0x0f, 0x02);
	const HayesSets plain = hayes_local(HAYES_S46_LAPM, 0x03, 0x04);
	HayesAgreed a = hayes_agree(comp, plain);
	EXPECT_TRUE(a.hayes);
	EXPECT_EQ(a.aft_caps, 0x03);
	EXPECT_EQ(a.aft_opts, 0x06);
	EXPECT_TRUE(a.aft);
	EXPECT_FALSE(a.comp);
	/* AFT only when the AND of AFT P1 is not 0 */
	EXPECT_FALSE(hayes_agree(hayes_local(HAYES_S46_LAPM, 0x01, 0),
				 hayes_local(HAYES_S46_LAPM, 0x02, 0)).aft);
	/* 1: no compression flag, none (also with V.42bis agreed) */
	EXPECT_EQ(hayes_select(a, true), HayesComp::None);
	EXPECT_EQ(hayes_comp_result(hayes_select(a, false)), 69);
	/* 2 and 3: the flag, V.42bis if agreed, else ADC */
	a = hayes_agree(comp, comp);
	EXPECT_TRUE(a.comp);
	EXPECT_EQ(hayes_select(a, true), HayesComp::V42bis);
	EXPECT_EQ(hayes_comp_result(HayesComp::V42bis), 67);
	EXPECT_EQ(hayes_select(a, false), HayesComp::Adc);
	EXPECT_EQ(hayes_comp_result(HayesComp::Adc), 68);
	/* a remote without the HMP set is not a Hayes modem */
	a = hayes_agree(comp, HayesSets());
	EXPECT_FALSE(a.hayes);
	EXPECT_EQ(hayes_select(a, false), HayesComp::None);
}


/*
 *    AFT
 *
 */

/* frames to send (tx) and the frames received (rx) */
struct FrameQueue : Framer {
	std::vector<std::vector<u8>> tx, rx;
	size_t next = 0;
	int errors = 0;
	Frame f = {};
	u8 fbuf[2100];
	/* the next of tx, NULL once all are out */
	Frame *next_tx_frame() override {
		if (next >= tx.size())
			return nullptr;
		memcpy(fbuf, tx[next].data(), tx[next].size());
		f.buf = fbuf;
		f.size = static_cast<s16>(tx[next].size());
		next++;
		return &f;
	}
	/* nothing to free */
	void tx_complete(Frame *) override {}
	/* a copy into rx */
	void rx_complete(Frame *fr) override {
		rx.emplace_back(fr->buf, fr->buf + fr->count);
	}
	/* a frame with a bad FCS, counted */
	void rx_error() override { errors++; }
};

/* an owner that sends nothing and takes all */
struct NullOwner : Owner {
	/* nothing to send */
	s16 read_chars(u8 *, s16) override { return 0; }
	/* all taken */
	s16 write_chars(const u8 *, s16 n) override { return n; }
	/* the link's progress ignored */
	void status(Status) override {}
};

/* the characters of start-stop bits (one bit in each item), after
   marks */
static std::vector<u8> chars_of(const std::vector<u8> &bits)
{
	std::vector<u8> v;
	for (size_t i = 0; i + 10 <= bits.size();) {
		if (bits[i]) {
			i++;
			continue;
		}
		u8 c = 0;
		for (int k = 0; k < 8; k++)
			c = static_cast<u8>(c | bits[i + 1 + k] << k);
		v.push_back(c);
		i += 10;
	}
	return v;
}

/* the FCS of RFC 1662 (Appendix C): "123456789" gives the complement
   906Eh, and a frame with its FCS (low octet first) gives F0B8h */
TEST(Hayes, AftFcs)
{
	const char *s = "123456789";
	u16 fcs = FCS16_INIT;
	for (const char *p = s; *p; p++)
		fcs = fcs16_add(fcs, static_cast<u8>(*p));
	fcs = static_cast<u16>(~fcs);
	EXPECT_EQ(fcs, 0x906e);
	u16 chk = FCS16_INIT;
	for (const char *p = s; *p; p++)
		chk = fcs16_add(chk, static_cast<u8>(*p));
	chk = fcs16_add(chk, static_cast<u8>(fcs));
	chk = fcs16_add(chk, static_cast<u8>(fcs >> 8));
	EXPECT_EQ(chk, FCS16_GOOD);
}

/* one frame of the 256 octet values on the line: the characters are
   the flag, the escaped octets, the FCS and the flag; only the map's
   octets escaped, none of them without 7Dh; for both escape modes */
TEST(Hayes, AftLine)
{
	for (u8 opts : { u8(0), u8(AFT_OPT_CTRL) }) {
		NullOwner o;
		Link l;
		l.init(&o, true, Config());
		Aft a;
		a.start(&l);
		a.set_options(opts);
		FrameQueue q;
		std::vector<u8> all(256);
		for (int i = 0; i < 256; i++)
			all[i] = static_cast<u8>(i);
		q.tx.push_back(all);
		a.set_framer(&q);
		std::vector<u8> bits(10 * 700);
		a.get_bits(1, bits.data(), static_cast<s32>(bits.size()));
		std::vector<u8> line = chars_of(bits);
		int esc = 0;
		for (int c = 0; c < 256; c++)
			esc += a.escaped(static_cast<u8>(c));
		EXPECT_EQ(esc, opts ? 2 + 32 + 1 + 32 + 1 : 2);
		ASSERT_GE(line.size(), 2u + 256 + esc + 2);
		EXPECT_EQ(line.front(), 0x7e);
		std::vector<u8> body;
		size_t i = 1;
		for (; i < line.size() && line[i] != 0x7e; i++) {
			EXPECT_FALSE(line[i] != 0x7d && a.escaped(line[i]))
				<< "octet " << int(line[i]) << " not escaped";
			if (line[i] == 0x7d)
				body.push_back(line[++i] ^ 0x20);
			else
				body.push_back(line[i]);
		}
		ASSERT_EQ(body.size(), 258u);
		EXPECT_EQ(std::vector<u8>(body.begin(), body.begin() + 256),
			  all);
		u16 fcs = FCS16_INIT;
		for (u8 c : body)
			fcs = fcs16_add(fcs, c);
		EXPECT_EQ(fcs, FCS16_GOOD);
	}
}

/* frames from one Aft to another, bit by bit, in both escape modes:
   random frames, frames of all 256 values, all flags, all escapes; then
   a bit error (that frame lost, the next ones good), an abort (7Dh
   7Eh), and XON and XOFF from the line without 7Dh (ignored with
   AFT_OPT_CTRL) */
TEST(Hayes, AftLoopBack)
{
	rnd_state = 3;
	for (u8 opts : { u8(0), u8(AFT_OPT_CTRL) }) {
		NullOwner o;
		Link l;
		l.init(&o, true, Config());
		Aft tx, rx;
		tx.start(&l);
		rx.start(&l);
		tx.set_options(opts);
		rx.set_options(opts);
		FrameQueue src, dst;
		tx.set_framer(&src);
		rx.set_framer(&dst);
		for (int i = 0; i < 60; i++) {
			std::vector<u8> v(3 + rnd() % 200);
			for (auto &c : v)
				c = static_cast<u8>(rnd());
			if (i % 5 == 1)
				for (size_t k = 0; k < v.size(); k++)
					v[k] = static_cast<u8>(k);
			if (i % 7 == 2)
				std::fill(v.begin(), v.end(), 0x7e);
			if (i % 7 == 3)
				std::fill(v.begin(), v.end(), 0x7d);
			src.tx.push_back(v);
		}
		std::vector<u8> bits(4000);
		for (int k = 0; k < 200; k++) {
			s32 n = tx.get_bits(1, bits.data(),
					    static_cast<s32>(bits.size()));
			rx.put_bits(1, bits.data(), n);
		}
		EXPECT_EQ(dst.rx, src.tx) << "options " << int(opts);
		EXPECT_EQ(dst.errors, 0);

		/* a bit error in a frame's data: an FCS error, the next
		   frame good */
		dst.rx.clear();
		src.tx = { std::vector<u8>(40, 0x55),
			   std::vector<u8>(40, 0x66) };
		src.next = 0;
		tx.get_bits(1, bits.data(), static_cast<s32>(bits.size()));
		bits[10 * 10 + 3] ^= 1;
		rx.put_bits(1, bits.data(), static_cast<s32>(bits.size()));
		EXPECT_EQ(dst.errors, 1);
		ASSERT_EQ(dst.rx.size(), 1u);
		EXPECT_EQ(dst.rx[0], src.tx[1]);

		/* characters made by hand: an abort, then XON, XOFF and
		   a frame */
		auto put = [&](const std::vector<u8> &cs) {
			std::vector<u8> b;
			for (u8 c : cs) {
				b.push_back(0);
				for (int k = 0; k < 8; k++)
					b.push_back((c >> k) & 1);
				b.push_back(1);
			}
			rx.put_bits(1, b.data(), static_cast<s32>(b.size()));
		};
		dst.rx.clear();
		dst.errors = 0;
		put({ 0x7e, 0x03, 0x01, 0x02, 0x7d, 0x7e });
		EXPECT_TRUE(dst.rx.empty());
		EXPECT_EQ(dst.errors, 0);
		std::vector<u8> fr = { 0x03, 0x01, 0x41, 0x42 };
		u16 fcs = FCS16_INIT;
		for (u8 c : fr)
			fcs = fcs16_add(fcs, c);
		fcs = static_cast<u16>(~fcs);
		std::vector<u8> cs = { 0x03, 0x11, 0x01, 0x13, 0x41, 0x42,
				       static_cast<u8>(fcs),
				       static_cast<u8>(fcs >> 8), 0x7e };
		/* the FCS octets escaped where the map has them */
		std::vector<u8> line;
		for (u8 c : cs) {
			if (c != 0x7e && c != 0x11 && c != 0x13 &&
			    tx.escaped(c)) {
				line.push_back(0x7d);
				line.push_back(c ^ 0x20);
			} else {
				line.push_back(c);
			}
		}
		put(line);
		if (opts) {
			ASSERT_EQ(dst.rx.size(), 1u);
			EXPECT_EQ(dst.rx[0], fr);
		} else {
			EXPECT_TRUE(dst.rx.empty());
			EXPECT_EQ(dst.errors, 1);
		}
	}
}


/*
 *    Express 96 link rules
 *
 */

/* the frames of one side: its data frames (each with its number), and
   what it received */
struct BurstData : FrameQueue {
	/* n frames of 3 to 128 octets */
	void make(int n, u8 tag) {
		for (int i = 0; i < n; i++) {
			std::vector<u8> v(3 + rnd() % 126);
			for (auto &c : v)
				c = static_cast<u8>(rnd());
			v[0] = tag;
			v[1] = static_cast<u8>(i);
			tx.push_back(v);
		}
	}
};

/* two E96Lines on one half-duplex line, a millisecond at a time: the
   bits of the side that transmits go to the other; both at once is a
   collision (counted, the line then garbled) */
struct E96Pair {
	NullOwner o;
	Link l;
	BurstData dc, da;
	E96Line c, a;
	s32 bps;
	long collisions = 0, ms = 0;
	double acc = 0;
	/* the 1s at the end of each side's line bits, the fewest at the
	   end of a burst, and the bursts that ended */
	long ones_c = 0, ones_a = 0, last_c = 0, last_a = 0;
	long trail_c = 1 << 30, trail_a = 1 << 30;
	long ends_c = 0, ends_a = 0;
	bool was_c = false, was_a = false;

	/* a connection at bps with the firmware's times for that rate */
	explicit E96Pair(s32 rate) : bps(rate) {
		l.init(&o, true, Config());
		const E96Times t = E96Times::for_rate(rate);
		c.start(&l, &dc, t, true, rate);
		a.start(&l, &da, t, false, rate);
	}
	/* n milliseconds */
	void run(long n) {
		std::vector<u8> bc, ba, line;
		for (long k = 0; k < n; k++, ms++) {
			c.tick(1);
			a.tick(1);
			acc += bps / 1000.0;
			const s32 bits = static_cast<s32>(acc);
			acc -= bits;
			bc.resize(bits);
			ba.resize(bits);
			line.resize(bits);
			const bool tc = c.transmitting(), ta = a.transmitting();
			c.get_bits(bc.data(), bits);
			a.get_bits(ba.data(), bits);
			if (tc && ta)
				collisions++;
			/* a burst that ended in the last millisecond: the
			   last run of 1s (only 0s, no carrier, after it) */
			for (s32 i = 0; i < bits; i++) {
				if (bc[i])
					ones_c++;
				else if (ones_c)
					last_c = ones_c, ones_c = 0;
				if (ba[i])
					ones_a++;
				else if (ones_a)
					last_a = ones_a, ones_a = 0;
			}
			const bool nc = c.transmitting(), na = a.transmitting();
			if (was_c && !nc) {
				trail_c = std::min(trail_c,
						   ones_c ? ones_c : last_c);
				ends_c++;
			}
			if (was_a && !na) {
				trail_a = std::min(trail_a,
						   ones_a ? ones_a : last_a);
				ends_a++;
			}
			was_c = nc;
			was_a = na;
			for (s32 i = 0; i < bits; i++)
				line[i] = tc && ta ? (rnd() & 1) :
					static_cast<u8>(bc[i] | ba[i]);
			c.put_bits(line.data(), bits);
			a.put_bits(line.data(), bits);
		}
	}
};

/* the times of FA 6.6.3 for 4800 and 9600 bit/s */
TEST(Hayes, E96Times)
{
	const E96Times a = E96Times::for_rate(4800), b = E96Times::for_rate(9600);
	EXPECT_EQ(a.silence, 20);
	EXPECT_EQ(a.rx_delay, 15);
	EXPECT_EQ(a.null_tx, 1);
	EXPECT_EQ(a.tx_off, 12);
	EXPECT_EQ(b.silence, 25);
	EXPECT_EQ(b.rx_delay, 30);
	EXPECT_EQ(b.null_tx, 4);
	EXPECT_EQ(b.tx_off, 15);
}

/* data both ways over the half-duplex loop-back at 4800 and 9600
   bit/s: every frame arrives in order, no burst has more than 7 frames,
   each burst ends with 7 or more marks (the other side sees each end),
   the first 5 bursts are empty, and the two sides never transmit at
   the same time */
TEST(Hayes, E96LoopBack)
{
	for (s32 rate : { 4800, 9600 }) {
		rnd_state = 11;
		E96Pair p(rate);
		p.dc.make(60, 'c');
		p.da.make(45, 'a');
		long limit = 60000;
		while (p.ms < limit && (p.da.rx.size() < p.dc.tx.size() ||
					p.dc.rx.size() < p.da.tx.size()))
			p.run(100);
		/* a few turnarounds more, with nothing to send */
		p.run(500);
		EXPECT_TRUE(p.da.rx == p.dc.tx) << rate << ": caller to answerer, "
			<< p.da.rx.size() << " of " << p.dc.tx.size();
		EXPECT_TRUE(p.dc.rx == p.da.tx) << rate << ": answerer to caller, "
			<< p.dc.rx.size() << " of " << p.da.tx.size();
		EXPECT_EQ(p.collisions, 0) << rate;
		EXPECT_LE(p.c.max_frames(), 7);
		EXPECT_LE(p.a.max_frames(), 7);
		EXPECT_EQ(p.c.max_frames(), 7);
		EXPECT_GE(p.c.burst().empty_bursts(), 5);
		EXPECT_FALSE(p.c.failed() || p.a.failed());
		/* each burst that ended was seen to end at the other side
		   (the last one may still be on the line) */
		EXPECT_LE(p.c.bursts_ended() - p.a.remote_ends(), 1);
		EXPECT_LE(p.a.bursts_ended() - p.c.remote_ends(), 1);
		EXPECT_GT(p.a.remote_ends(), 10);
		EXPECT_EQ(p.c.burst().failures(), 0);
		/* on the line: each burst ends with 7 or more 1s */
		EXPECT_GT(p.ends_c, 10);
		EXPECT_GT(p.ends_a, 10);
		EXPECT_GE(p.trail_c, 7);
		EXPECT_GE(p.trail_a, 7);
		printf("  e96 %d bit/s: %zu + %zu frames in %ld ms, %d + %d "
		       "bursts\n", (int)rate, p.dc.tx.size(), p.da.tx.size(),
		       p.ms, (int)p.c.burst().bursts(),
		       (int)p.a.burst().bursts());
	}
}

/* the budget: 7 frames in a burst, 4 with error bit 0, 2 with bit 1 */
TEST(Hayes, E96Budget)
{
	for (u8 err : { u8(0), u8(1), u8(2), u8(3) }) {
		rnd_state = 5;
		E96Pair p(9600);
		p.c.burst().set_errors(err);
		p.dc.make(40, 'c');
		p.run(8000);
		const s16 want = err & 2 ? 2 : err & 1 ? 4 : 7;
		EXPECT_EQ(p.c.max_frames(), want) << "errors " << int(err);
		EXPECT_TRUE(p.da.rx == p.dc.tx) << "errors " << int(err) << ": "
			<< p.da.rx.size() << " of " << p.dc.tx.size();
	}
}

/* a remote that does not answer: failures counted, a new burst after
   each, and the link fails after max_failures in a row */
TEST(Hayes, E96Failure)
{
	NullOwner o;
	Link l;
	l.init(&o, true, Config());
	BurstData d;
	E96Line c;
	E96Times t = E96Times::for_rate(9600);
	t.max_failures = 3;
	c.start(&l, &d, t, true, 9600);
	std::vector<u8> bits(10);
	std::vector<u8> zeros(10, 0);
	s32 bursts = 0;
	for (int ms = 0; ms < 60000 && !c.failed(); ms++) {
		c.tick(1);
		c.get_bits(bits.data(), 10);
		c.put_bits(zeros.data(), 10);
		bursts = c.burst().bursts();
	}
	EXPECT_TRUE(c.failed());
	EXPECT_EQ(c.burst().state(), E96Burst::State::Failed);
	EXPECT_EQ(bursts, 4);
}
