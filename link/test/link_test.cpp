/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    link_test.cpp - tests of the data link layer (link/), without a
 *                    modem: V.42bis round trips, HDLC framing, and two
 *                    Links back to back, bit by bit (LAPM, MNP, async)
 *
 *    link_test                          all of them
 *    link_test --gtest_filter=Link.Hdlc one
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "link.h"
#include "mnp5.h"
#include "v44.h"

using namespace dlink;

/* a failed check, with a message in printf's format */
static std::string fmt(const char *f, ...) DMODEM_PRINTF(1, 2);
/* f's message */
static std::string fmt(const char *f, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}
/* EXPECT_TRUE with the message */
#define CHECK(cond, ...) EXPECT_TRUE(cond) << fmt(__VA_ARGS__)

static unsigned rnd_state = 12345;
/* a repeatable pseudo-random number (rnd_state), 24 bits */
static unsigned rnd()
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return rnd_state >> 8;
}

/* text that compresses */
static std::vector<uint8_t> text(size_t n)
{
	static const char *words[] = { "the ", "quick ", "brown ", "fox ",
		"jumps ", "over ", "lazy ", "dog ", "modem ", "V.42bis ",
		"\r\n", "compression ", "dictionary ", "string " };
	std::vector<uint8_t> v;
	while (v.size() < n) {
		const char *w = words[rnd() % 14];
		v.insert(v.end(), w, w + strlen(w));
	}
	v.resize(n);
	return v;
}

/* n random octets */
static std::vector<uint8_t> noise(size_t n)
{
	std::vector<uint8_t> v(n);
	for (auto &c : v)
		c = (uint8_t)rnd();
	return v;
}


/*
 *    V.44
 *
 */

/* in through a V.44 encoder (a flush every flush_every characters, 0:
   at the end only) and a decoder with the same parameters: the output
   must be in; returns the compressed octets */
static std::vector<uint8_t> v44_roundtrip(const char *name,
					  const std::vector<uint8_t> &in,
					  int n2, int n7, int n8,
					  int flush_every)
{
	V44 enc, dec;
	std::vector<uint8_t> line, out;
	uint8_t buf[2048];
	int n;

	CHECK(!enc.init(n2, n7, n8) && !dec.init(n2, n7, n8), "init");
	for (size_t i = 0; i < in.size(); i++) {
		enc.encode(in[i]);
		if (flush_every && (i + 1) % flush_every == 0)
			enc.flush_encoder();
		while ((n = enc.out(buf, sizeof(buf))) > 0)
			line.insert(line.end(), buf, buf + n);
	}
	enc.flush_encoder();
	while ((n = enc.out(buf, sizeof(buf))) > 0)
		line.insert(line.end(), buf, buf + n);
	for (uint8_t c : line) {
		CHECK(dec.decode(c) >= 0, "%s: decode error", name);
		while ((n = dec.out(buf, sizeof(buf))) > 0)
			out.insert(out.end(), buf, buf + n);
	}
	CHECK(out == in, "%s: %zu in, %zu out", name, in.size(), out.size());
	printf("  %s: %zu -> %zu octets\n", name, in.size(), line.size());
	return line;
}

/* V.44 round trips (text, flushes, small parameters, noise, repeats,
   a mix), and the decoder's transparent mode */
TEST(Link, V44)
{
	rnd_state = 12345;
	std::vector<uint8_t> t = text(200000), z = noise(50000);
	std::vector<uint8_t> rep, line;
	for (int i = 0; i < 30000; i++)
		rep.push_back("abababababababababcabcabcabc"[i % 28]);
	line = v44_roundtrip("text", t, 4096, 255, 12288, 0);
	CHECK(line.size() < t.size() / 2, "text: %zu octets", line.size());
	v44_roundtrip("text, flush every 100", t, 4096, 255, 12288, 100);
	v44_roundtrip("text, flush every one", text(5000), 4096, 255, 12288, 1);
	v44_roundtrip("text, 256 codewords, strings of 32, history 512", t,
		      256, 32, 512, 0);
	v44_roundtrip("text, 1024 codewords, strings of 100", t, 1024, 100,
		      4096, 37);
	v44_roundtrip("noise (8 bit ordinals)", z, 4096, 255, 12288, 0);
	v44_roundtrip("repeats (extensions)", rep, 4096, 255, 12288, 0);
	std::vector<uint8_t> mixed = text(30000);
	std::vector<uint8_t> z2 = noise(30000);
	mixed.insert(mixed.end(), z2.begin(), z2.end());
	std::vector<uint8_t> t2 = text(30000);
	mixed.insert(mixed.end(), t2.begin(), t2.end());
	v44_roundtrip("text, noise, text", mixed, 4096, 255, 12288, 0);

	/* transparent mode (the decoder only): ETM, then octets with the
	   escape character 0 (EID: the character; ECM: compressed again,
	   from the start) */
	V44 dec;
	CHECK(!dec.init(4096, 255, 12288), "init");
	std::vector<uint8_t> s;
	/* 1 000000 (ETM, 6 bits), then the rest of the octet */
	s.push_back(0x01);
	s.push_back('h');
	s.push_back('i');
	s.push_back(0x00);              /* EID: a 0 */
	s.push_back(0x01);
	s.push_back(0x33);              /* the escape is 0x33 now: ECM */
	s.push_back(0x00);
	/* compressed: ordinal 'A' (0 + 7 bits), then FLUSH (1 + 000001) */
	s.push_back((uint8_t)(('A' << 1) & 0xff));
	s.push_back((uint8_t)(1 | 1 << 1));
	std::vector<uint8_t> o;
	uint8_t buf[64];
	int n;
	for (uint8_t c : s)
		CHECK(dec.decode(c) >= 0, "transparent: decode error");
	while ((n = dec.out(buf, sizeof(buf))) > 0)
		o.insert(o.end(), buf, buf + n);
	std::vector<uint8_t> want = { 'h', 'i', 0, 'A' };
	CHECK(o == want, "transparent: %zu octets", o.size());
	printf("  transparent mode, EID, ECM: ok\n");
}

/*
 *    V.42bis
 *
 */

/* in through a V.42bis encoder (a flush every flush_every characters)
   and a decoder: the output must be in */
static void v42bis_roundtrip(const char *name, const std::vector<uint8_t> &in,
			     int dict, int maxstr, int flush_every)
{
	V42bis enc, dec;
	std::vector<uint8_t> line, out;
	uint8_t buf[2048];
	int n;

	CHECK(!enc.init(dict, maxstr) && !dec.init(dict, maxstr), "init");
	for (size_t i = 0; i < in.size(); i++) {
		enc.encode(in[i]);
		if (flush_every && (i + 1) % flush_every == 0)
			enc.flush_encoder();
		while ((n = enc.out(buf, sizeof(buf))) > 0)
			line.insert(line.end(), buf, buf + n);
	}
	enc.flush_encoder();
	while ((n = enc.out(buf, sizeof(buf))) > 0)
		line.insert(line.end(), buf, buf + n);
	for (uint8_t c : line) {
		CHECK(dec.decode(c) >= 0, "decode error");
		while ((n = dec.out(buf, sizeof(buf))) > 0)
			out.insert(out.end(), buf, buf + n);
	}
	dec.flush_decoder();
	while ((n = dec.out(buf, sizeof(buf))) > 0)
		out.insert(out.end(), buf, buf + n);
	CHECK(out == in, "%s: %zu in, %zu out", name, in.size(), out.size());
	printf("  %s: %zu -> %zu octets\n", name, in.size(), line.size());
}

/* V.42bis round trips, and that it compresses text */
TEST(Link, V42bis)
{
	rnd_state = 12345;
	std::vector<uint8_t> t = text(200000), z = noise(50000);
	std::vector<uint8_t> esc;
	for (int i = 0; i < 20000; i++)
		esc.push_back((uint8_t)(i * 51));       /* the escape steps */
	v42bis_roundtrip("text", t, 2048, 34, 0);
	v42bis_roundtrip("text, flush every 100", t, 2048, 34, 100);
	v42bis_roundtrip("text, 512 codewords, strings of 6", t, 512, 6, 0);
	v42bis_roundtrip("noise", z, 2048, 34, 0);
	v42bis_roundtrip("escape characters", esc, 2048, 34, 7);
	std::vector<uint8_t> mixed = text(30000);
	std::vector<uint8_t> z2 = noise(30000);
	mixed.insert(mixed.end(), z2.begin(), z2.end());
	std::vector<uint8_t> t2 = text(30000);
	mixed.insert(mixed.end(), t2.begin(), t2.end());
	v42bis_roundtrip("text, noise, text", mixed, 2048, 34, 0);

	/* it does compress text */
	V42bis enc;
	enc.init(2048, 34);
	uint8_t buf[2048];
	size_t outn = 0;
	for (uint8_t c : t) {
		enc.encode(c);
		outn += enc.out(buf, sizeof(buf));
	}
	CHECK(outn < t.size() / 2, "text compressed to %zu of %zu", outn,
	      t.size());
}


/* the decoder takes STEPUP up to log2(N2) bits a codeword and no
   further (C-ERROR): with N2 = 2048, 9 -> 10 -> 11, not 12 */
static int v42bis_stepups(int n)
{
	V42bis dec;
	dec.init(2048, 34);
	if (dec.decode(0) < 0 || dec.decode(0) < 0)   /* escape, ECM */
		return -2;
	uint32_t bits = 0;
	int len = 0, size = 9, ret = 0;
	for (int i = 0; i < n; i++) {
		bits |= 2u << len;      /* STEPUP */
		len += size++;
		while (len >= 8 && ret >= 0) {
			ret = dec.decode((uint8_t)bits);
			bits >>= 8;
			len -= 8;
		}
	}
	if (len && ret >= 0)
		ret = dec.decode((uint8_t)bits);
	return ret;
}

/* two STEPUPs taken, a third a C-ERROR */
TEST(Link, V42bisStepup)
{
	CHECK(v42bis_stepups(2) >= 0, "two STEPUPs (to 11 bits) taken");
	CHECK(v42bis_stepups(3) < 0, "a third STEPUP (12 bits) is C-ERROR");
}


/*
 *    HDLC
 *
 */

/* an Hdlc's framer: frames to send (tx) and the frames received (rx) */
struct FrameSource : Framer {
	std::vector<std::vector<uint8_t>> tx, rx;
	size_t next = 0;
	Frame f = {};
	uint8_t fbuf[600];
	/* the next of tx, NULL once all are out */
	Frame *next_tx_frame() override {
		if (next >= tx.size())
			return nullptr;
		memcpy(fbuf, tx[next].data(), tx[next].size());
		f.buf = fbuf;
		f.size = (int)tx[next].size();
		next++;
		return &f;
	}
	/* nothing to free */
	void tx_complete(Frame *) override {}
	/* a copy into rx */
	void rx_complete(Frame *fr) override {
		rx.emplace_back(fr->buf, fr->buf + fr->count);
	}
};

/* an owner that sends nothing and takes all */
struct NullOwner : Owner {
	/* nothing to send */
	s16 read_chars(uint8_t *, s16) override { return 0; }
	/* all taken */
	s16 write_chars(const uint8_t *, s16 n) override { return n; }
	/* the link's progress ignored */
	void status(Status) override {}
};

/* 200 frames (noise, all 1s, all flags) from one Hdlc to another, bit
   by bit, one bit error: all but the frame or two it hits arrive, in
   order and unchanged */
TEST(Link, Hdlc)
{
	rnd_state = 12345;
	NullOwner o;
	Link l;
	l.init(&o, true, Config());
	Hdlc a, b;
	FrameSource src, dst;
	a.start(&l);
	b.start(&l);
	a.set_framer(&src, true);
	b.set_framer(&dst, false);
	for (int i = 0; i < 200; i++) {
		std::vector<uint8_t> v = noise(3 + rnd() % 130);
		if (i % 7 == 0)
			std::fill(v.begin(), v.end(), 0xff);    /* stuffing */
		if (i % 11 == 0)
			std::fill(v.begin(), v.end(), 0x7e);    /* flags */
		src.tx.push_back(v);
	}
	uint8_t bits[1000];
	int corrupt_at = -1;
	for (int k = 0; k < 4000; k++) {
		a.get_bits(1, bits, 1000);
		/* one bit error in one frame */
		if (k == 50) {
			bits[500] ^= 1;
			corrupt_at = 1;
		}
		b.put_bits(1, bits, 1000);
	}
	CHECK(dst.rx.size() + 1 == src.tx.size() ||
	      dst.rx.size() + 2 == src.tx.size(),
	      "%zu frames sent, %zu received", src.tx.size(), dst.rx.size());
	size_t j = 0, lost = 0;
	for (size_t i = 0; i < src.tx.size() && j < dst.rx.size(); i++) {
		if (src.tx[i] == dst.rx[j])
			j++;
		else
			lost++;
	}
	CHECK(j == dst.rx.size(), "received frames in order, unchanged");
	printf("  hdlc: %zu frames, %zu lost to the bit error (%d)\n",
	       src.tx.size(), lost, corrupt_at);
}


/*
 *    two links back to back
 *
 */

/* a DTE: what it sends and what it got, the link's progress, the
   characters it takes per call (accept) */
struct Dte : Owner {
	const char *name;
	std::vector<uint8_t> to_send, got;
	size_t pos = 0;
	bool up = false, released = false, error = false, detected = false;
	int accept = 1 << 30;           /* characters it takes per call */
	Config agreed;
	bool verbose = false;
	/* the DTE called n in the log */
	explicit Dte(const char *n) : name(n) {}
	/* the rest of to_send */
	s16 read_chars(uint8_t *buf, s16 n) override {
		int k = (int)std::min((size_t)n, to_send.size() - pos);
		memcpy(buf, to_send.data() + pos, k);
		pos += k;
		return k;
	}
	/* up to accept characters into got */
	s16 write_chars(const uint8_t *buf, s16 n) override {
		if (n > accept)
			n = accept;
		got.insert(got.end(), buf, buf + n);
		return n;
	}
	/* the progress as flags */
	void status(Status s) override {
		switch (s) {
		case Status::Detected: detected = true; break;
		case Status::Up: up = true; break;
		case Status::Released: released = true; break;
		case Status::Error: error = true; break;
		}
	}
	/* the pump's rate, for T400 and T401 */
	int32_t tx_rate() override { return 9600; }
	/* what XID agreed */
	void negotiated(const Config &c) override { agreed = c; }
	/* the link's lines (LINK_DEBUG) */
	void log(const char *line) override {
		if (verbose)
			printf("    %s: %s\n", name, line);
	}
};

/* two Links back to back: the caller lc and the answerer la, bit
   errors at the rate ber */
struct Pair {
	Dte dc{"caller"}, da{"answerer"};
	Link lc, la;
	double ber = 0;
	long errors = 0;

	/* a call, each side with its configuration */
	Pair(const Config &cc, const Config &ca) {
		lc.init(&dc, true, cc);
		la.init(&da, false, ca);
		const char *v = getenv("LINK_DEBUG");
		dc.verbose = da.verbose = v && *v;
	}
	/* the data pumps have connected: each with or without the detection
	   phase, error correction if it asked for LAPM or MNP */
	void start(bool detect_c, bool detect_a) {
		lc.start(lc.asked().ec || lc.asked().mnp, detect_c);
		la.start(la.asked().ec || la.asked().mnp, detect_a);
	}
	/* run both ways, n bits at a time */
	void run(long bits, int chunk = 48) {
		std::vector<uint8_t> b(chunk);
		for (long k = 0; k < bits; k += chunk) {
			lc.get_bits(1, b.data(), chunk);
			flip(b);
			la.put_bits(1, b.data(), chunk);
			la.get_bits(1, b.data(), chunk);
			flip(b);
			lc.put_bits(1, b.data(), chunk);
			if (dc.up)
				lc.set_online(true);
			if (da.up)
				la.set_online(true);
		}
	}
	/* bit errors at the rate ber */
	void flip(std::vector<uint8_t> &b) {
		if (ber <= 0)
			return;
		for (auto &x : b)
			if ((rnd() & 0xffffff) < ber * 0x1000000) {
				x ^= 1;
				errors++;
			}
	}
};

/* LAPM with the detection phase, compression comp (V.42bis's P0), V.44 */
static Config lapm_config(int comp, bool v44 = true)
{
	Config c;
	c.ec = true;
	c.detect = true;
	c.comp = comp;
	c.v44 = v44;
	return c;
}

/* n_c characters from the caller and n_a from the answerer (text or
   noise) until both arrived or max_bits: each must get the other's */
static void transfer(const char *name, Pair &p, size_t n_c, size_t n_a,
		     long max_bits, bool text_data = true)
{
	p.dc.to_send = text_data ? text(n_c) : noise(n_c);
	p.da.to_send = text_data ? text(n_a) : noise(n_a);
	p.dc.pos = p.da.pos = 0;
	p.dc.got.clear();
	p.da.got.clear();
	long run = 0;
	while (run < max_bits && (p.da.got.size() < n_c ||
				  p.dc.got.size() < n_a)) {
		p.run(9600);
		run += 9600;
	}
	CHECK(p.da.got == p.dc.to_send, "%s: caller -> answerer %zu/%zu", name,
	      p.da.got.size(), n_c);
	CHECK(p.dc.got == p.da.to_send, "%s: answerer -> caller %zu/%zu", name,
	      p.dc.got.size(), n_a);
	printf("  %s: %zu + %zu octets in %.1f s at 9600 bit/s%s\n", name,
	       n_c, n_a, run / 9600.0,
	       p.errors ? (" (" + std::to_string(p.errors) +
			   " bit errors)").c_str() : "");
}

/* detection, XID, V.44 both ways with text and noise, DISC */
TEST(Link, Lapm)
{
	rnd_state = 12345;
	Pair p(lapm_config(3), lapm_config(3));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.detected && p.da.detected, "detection");
	CHECK(p.lc.ec() && p.la.ec(), "LAPM chosen");
	CHECK(p.dc.up && p.da.up, "LAPM up");
	CHECK(p.lc.config().comp == 3 && p.la.config().comp == 3,
	      "compression both ways (%d, %d)", p.lc.config().comp,
	      p.la.config().comp);
	CHECK(p.lc.config().v44 && p.la.config().v44, "V.44 (%d, %d)",
	      p.lc.config().v44, p.la.config().v44);
	transfer("lapm + v44, text", p, 60000, 40000, 9600 * 60);
	/* compression: well above the line rate */
	transfer("lapm + v44, noise", p, 5000, 5000, 9600 * 20, false);
	p.lc.disconnect();
	p.run(9600 * 2);
	CHECK(p.da.released, "answerer released after DISC");
	CHECK(p.dc.released, "caller released after UA");
}

/* frames LAPM must drop: an I frame of two octets (no second control
   octet; it crashed with a negative length) and one with 300 octets of
   information, more than N401, while the DTE takes nothing (it ran past
   the buffer kept for the DTE); the link goes on */
TEST(Link, LapmBadFrames)
{
	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(0));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	NullOwner o;
	Link l;
	l.init(&o, true, Config());
	Hdlc inj;
	FrameSource src;
	inj.start(&l);
	inj.set_framer(&src, true);
	src.tx.push_back({ 0x03, 0x00 });
	std::vector<uint8_t> big(3 + 300, 0x55);
	big[0] = 0x03;
	big[1] = big[2] = 0x00;         /* N(S) 0, N(R) 0 */
	src.tx.push_back(big);
	p.da.accept = 0;
	uint8_t bits[600];
	for (int k = 0; k < 20; k++) {
		inj.get_bits(1, bits, sizeof(bits));
		p.la.put_bits(1, bits, sizeof(bits));
	}
	p.da.accept = 1 << 30;
	CHECK(p.da.got.empty(), "nothing of them to the DTE");
	transfer("lapm after bad frames", p, 2000, 2000, 9600 * 20);
}

/* frames into l from the line (rounds of 600 bits each way, as from the
   other side), and the frames l sent meanwhile */
static std::vector<std::vector<uint8_t>> exchange(Link &l,
	const std::vector<std::vector<uint8_t>> &frames, int rounds)
{
	NullOwner o;
	Link side;
	side.init(&o, true, Config());
	Hdlc tx, rx;
	FrameSource src, dst;
	tx.start(&side);
	rx.start(&side);
	tx.set_framer(&src, true);
	rx.set_framer(&dst, false);
	src.tx = frames;
	uint8_t bits[600];
	for (int k = 0; k < rounds; k++) {
		tx.get_bits(1, bits, sizeof(bits));
		l.put_bits(1, bits, sizeof(bits));
		l.get_bits(1, bits, sizeof(bits));
		rx.put_bits(1, bits, sizeof(bits));
	}
	return dst.rx;
}

/* an unsolicited DM with F = 0 while LAPM is idle: the remote asks for
   the link (8.8/V.42), so an SABME, not the link up without one; once
   the link was up and released, it is ignored */
TEST(Link, LapmUnsolicitedDm)
{
	const std::vector<uint8_t> dm = { 0x01, 0x0f };  /* the caller's DM, F 0 */
	const std::vector<uint8_t> sabme = { 0x01, 0x7f }; /* the answerer's, P 1 */
	Dte d("answerer");
	Link l;
	l.init(&d, false, lapm_config(0));
	l.start(true, false);
	std::vector<std::vector<uint8_t>> out = exchange(l, { dm }, 4);
	CHECK(!d.up, "the link up on an unsolicited DM");
	CHECK(std::find(out.begin(), out.end(), sabme) != out.end(),
	      "no SABME after the DM (%zu frames)", out.size());

	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(0));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	p.lc.disconnect();
	p.run(9600 * 2);
	CHECK(p.da.released, "answerer released after DISC");
	p.da.up = false;
	out = exchange(p.la, { dm }, 4);
	CHECK(!p.da.up, "up again on a DM after the release");
	CHECK(std::find(out.begin(), out.end(), sabme) == out.end(),
	      "an SABME after the release");
}

/* V.42bis when one of them has no V.44 (as the caller, as the
   answerer) */
TEST(Link, LapmV42bis)
{
	rnd_state = 12345;
	for (int side = 0; side < 2; side++) {
		Pair p(lapm_config(3, side == 1), lapm_config(3, side == 0));
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "LAPM up");
		CHECK(p.lc.config().comp == 3 && p.la.config().comp == 3 &&
		      !p.lc.config().v44 && !p.la.config().v44,
		      "V.42bis both ways");
		transfer(side ? "lapm + v42bis (answerer without V.44)" :
			 "lapm + v42bis (caller without V.44)", p, 60000,
			 40000, 9600 * 60);
	}
}

/* no compression when the caller offers none */
TEST(Link, LapmNoCompression)
{
	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(3));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	CHECK(p.lc.config().comp == 0 && p.la.config().comp == 0,
	      "no V.42bis when one side has none (%d, %d)",
	      p.lc.config().comp, p.la.config().comp);
	transfer("lapm, no v42bis", p, 20000, 20000, 9600 * 60);
}

/* one direction only: an answerer that only decodes (P0 counts from the
   caller, the XID's initiator: it answers 1, which is the caller's
   encoder); with V.42bis and with V.44 */
TEST(Link, LapmOneDirection)
{
	rnd_state = 12345;
	for (int v44 = 0; v44 < 2; v44++) {
		Pair p(lapm_config(3, v44), lapm_config(1, v44));
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "LAPM up");
		CHECK(p.lc.config().comp == 2 && p.la.config().comp == 1,
		      "the caller compresses, the answerer decodes (%d, %d)",
		      p.lc.config().comp, p.la.config().comp);
		transfer(v44 ? "lapm, v44 caller to answerer only" :
			 "lapm, v42bis caller to answerer only", p, 20000,
			 20000, 9600 * 60);
	}
}

/* V.42bis parameters: each the smaller of the two (6.4/V.42bis), also
   when the caller's N2 is the larger and its N7 the smaller */
TEST(Link, LapmV42bisParameters)
{
	rnd_state = 12345;
	Config cc = lapm_config(3, false), ca = lapm_config(3, false);
	cc.dict_size = 2048;
	cc.max_string = 16;
	ca.dict_size = 1024;
	ca.max_string = 34;
	Pair p(cc, ca);
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	CHECK(p.lc.config().comp == 3 && p.la.config().comp == 3,
	      "V.42bis both ways");
	CHECK(p.lc.config().dict_size == 1024 &&
	      p.la.config().dict_size == 1024 &&
	      p.lc.config().max_string == 16 &&
	      p.la.config().max_string == 16,
	      "N2 1024, N7 16 on both (%d/%d, %d/%d)",
	      p.lc.config().dict_size, p.lc.config().max_string,
	      p.la.config().dict_size, p.la.config().max_string);
	transfer("lapm, v42bis 1024/16", p, 20000, 20000, 9600 * 60);
}

/* bit errors: REJ and T401 recovery, and no link error */
TEST(Link, LapmBitErrors)
{
	rnd_state = 12345;
	for (int v44 = 1; v44 >= 0; v44--) {
		Pair p(lapm_config(3, v44), lapm_config(3, v44));
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "LAPM up");
		p.ber = 2e-4;
		transfer(v44 ? "lapm + v44, bit errors 2e-4" :
			 "lapm + v42bis, bit errors 2e-4", p, 30000, 30000,
			 9600 * 120);
		CHECK(p.errors > 10, "errors were injected (%ld)", p.errors);
		CHECK(!p.dc.error && !p.da.error, "no link error");
	}
}

/* the bits to the data pump: each item nbits wide, nothing above them,
   in every mode (idle marks, async characters, the detection phase's
   ODPs, LAPM's flags and frames) */
TEST(Link, BitsMasked)
{
	for (int nbits : { 1, 2, 4, 7, 8 }) {
		const unsigned top = 1u << nbits;
		std::vector<uint8_t> b(48);
		auto check = [&](Link &l, const char *mode, int calls) {
			unsigned bad = 0;
			for (int k = 0; k < calls; k++) {
				l.get_bits(nbits, b.data(), (int)b.size());
				for (uint8_t x : b)
					bad |= x & ~(top - 1);
			}
			CHECK(!bad, "%s, %d bit items: bits above them %02x", mode,
			      nbits, bad);
		};
		Config async;
		Dte d("dte");
		d.to_send = text(500);
		Link idle, chars, odp, lapm;
		idle.init(&d, true, async);
		check(idle, "idle", 10);
		chars.init(&d, true, async);
		chars.start(false, false);
		chars.set_online(true);         /* (the DTE's characters) */
		check(chars, "async", 200);
		CHECK(d.pos > 0, "async: the DTE's characters went out");
		odp.init(&d, true, lapm_config(0));
		odp.start(true, true);
		check(odp, "detection", 200);
		lapm.init(&d, true, lapm_config(0));
		lapm.start(true, false);
		check(lapm, "lapm", 200);
	}
}

/* a hangup in the detection phase (the remote has no V.42: no ADP, so
   T400 runs): the timer must not bring the link up afterwards */
TEST(Link, DetectHangup)
{
	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(0));
	p.la.start(true, true);         /* the answerer only: no ODPs */
	std::vector<uint8_t> b(48);
	for (int k = 0; k < 20; k++)
		p.la.get_bits(1, b.data(), 48);
	p.la.disconnect();
	for (int k = 0; k < 9600 * 3 / 48; k++)
		p.la.get_bits(1, b.data(), 48);
	CHECK(!p.da.up && !p.da.detected, "no link after the hangup");
}

/* the originator's detection phase against an answerer whose ADPs are
   given (pairs of characters, each pair repeated times): marks, then the
   patterns as start-stop characters with 10 marks between; the caller's
   Link as the phase leaves it */
static void detect_against(Pair &p,
			   std::vector<std::pair<std::string, int>> adps)
{
	std::vector<uint8_t> line(64 * 8 + 96, 1), b(48);
	for (auto &[pat, times] : adps)
		for (int t = 0; t < times; t++)
			for (unsigned char c : pat) {
				line.push_back(0);
				for (int i = 0; i < 8; i++)
					line.push_back((c >> i) & 1);
				line.insert(line.end(), 10, 1);
			}
	line.resize(line.size() + 9600 * 2, 1);
	p.lc.start(true, true);
	for (size_t k = 0; k + 48 <= line.size(); k += 48) {
		p.lc.get_bits(1, b.data(), 48);
		p.lc.put_bits(1, line.data() + k, 48);
	}
}

/* Appendix VI/V.42: a cellular modem's answerer (MNP 10EC's) sends "EM"
   five times or more, then V.42's "EC": the originator takes LAPM; "EM"
   alone is not V.42, and T400 ends the phase without it */
TEST(Link, AdpCellular)
{
	rnd_state = 12345;
	Pair p(lapm_config(0), Config());
	detect_against(p, { { "EM", 5 }, { "EC", 10 } });
	CHECK(p.dc.detected && p.lc.ec(), "EM then EC: LAPM");

	Pair q(lapm_config(0), Config());
	detect_against(q, { { "EM", 16 } });
	CHECK(q.dc.detected && !q.lc.ec(), "EM alone: no LAPM");
}

/* a DTE that takes 7 characters at a time: RNR, the rest later */
TEST(Link, LapmSlowDte)
{
	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(0));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	/* a slow DTE: RNR, the rest later */
	p.da.accept = 7;
	transfer("lapm, slow DTE", p, 20000, 2000, 9600 * 120);
}

/* no LAPM: async both ways */
TEST(Link, Async)
{
	rnd_state = 12345;
	Config c;               /* no LAPM */
	Pair p(c, c);
	p.start(false, false);
	p.run(480);
	CHECK(p.dc.up && p.da.up, "async up");
	CHECK(!p.lc.ec() && !p.la.ec(), "async chosen");
	transfer("async", p, 5000, 5000, 9600 * 10, false);
}

/* the answerer offers LAPM, the caller does not (it sends no ODP): the
   answerer falls back to async after T400 */
TEST(Link, V14)
{
	rnd_state = 12345;
	Config c;
	Pair p(c, lapm_config(3));
	p.start(false, true);
	/* T400, 750 ms at the pump's rate */
	p.run(9600);
	CHECK(p.da.detected && !p.la.ec(), "answerer: no LAPM within 1 s");
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "both up (async)");
	transfer("lapm answerer, async caller", p, 3000, 3000, 9600 * 10,
		 false);
}

/* the caller offers LAPM with the detection phase, the answerer has no
   V.42 (no ADP, no flags): the caller's ODPs go unanswered and it falls
   back to async after T400 (7.2/V.42) */
TEST(Link, OdpUnanswered)
{
	rnd_state = 12345;
	Config c;
	Pair p(lapm_config(3), c);
	p.start(true, false);
	/* 64 octets of marks, then T400 (750 ms at the pump's rate) */
	p.run(9600);
	CHECK(p.dc.detected && !p.lc.ec(), "caller: no LAPM within 1 s");
	CHECK(p.dc.up && p.da.up, "both up (async)");
	transfer("odp caller, async answerer", p, 3000, 3000, 9600 * 10,
		 false);
}

/* the caller goes to LAPM without the detection phase (as after V.8), the
   answerer has no V.42 at all: its XIDs unanswered N400 times, the caller
   goes on in async */
TEST(Link, LapmCallerAsyncRemote)
{
	rnd_state = 12345;
	Config c;
	Pair p(lapm_config(3), c);
	p.start(false, false);          /* (the caller: LAPM, no detection) */
	p.run(9600 * 8);
	CHECK(!p.dc.up, "caller: not up while it tries LAPM");
	p.run(9600 * 10);
	CHECK(p.dc.up && !p.lc.ec(), "caller: async after N400 XIDs");
	transfer("lapm caller, async answerer", p, 3000, 3000, 9600 * 10,
		 false);
}

/* N400 (5) retransmissions, no more (8.3.2.2, 8.4.8, 8.10/V.42): a
   caller's XID to a remote that never answers goes out once and again
   five times, then async; in the data phase, with the line cut, five
   polls (RR with P = 1), then DISC */
TEST(Link, LapmN400)
{
	Dte d("caller");
	Link l;
	l.init(&d, true, lapm_config(0));
	l.start(true, false);
	std::vector<std::vector<uint8_t>> out = exchange(l, {}, 400);
	int xids = 0;
	for (auto &f : out)
		if (f.size() >= 2 && f[0] == 0x03 && (f[1] & 0xef) == 0xaf)
			xids++;
	CHECK(xids == 6, "%d XIDs (the first and N400 again)", xids);
	CHECK(d.up && !l.ec(), "async after them");

	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(0));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	/* the caller's frames go nowhere, nothing comes back */
	p.dc.to_send = text(300);
	p.dc.pos = 0;
	NullOwner o;
	Link side;
	side.init(&o, true, Config());
	Hdlc rx;
	FrameSource dst;
	rx.start(&side);
	rx.set_framer(&dst, false);
	std::vector<uint8_t> b(600), idle(600, 1);
	for (int k = 0; k < 400; k++) {
		p.lc.get_bits(1, b.data(), (int)b.size());
		rx.put_bits(1, b.data(), (int)b.size());
		p.lc.put_bits(1, idle.data(), (int)idle.size());
	}
	int polls = 0, discs = 0;
	for (auto &f : dst.rx) {
		if (f.size() == 3 && f[0] == 0x03 && f[1] == 0x01 && (f[2] & 1))
			polls++;
		if (f.size() == 2 && f[0] == 0x03 && f[1] == 0x53)
			discs++;
	}
	CHECK(polls == 5, "%d polls before the DISC", polls);
	CHECK(discs >= 1, "no DISC after the polls");
}


/*
 *    MNP
 *
 */

/* MNP after LAPM (lapm: V.42 with the detection phase first), or MNP
   alone */
static Config mnp_config(bool lapm, bool mnp5 = false)
{
	Config c = lapm_config(lapm ? 3 : 0);
	c.ec = lapm;
	c.mnp = true;
	c.mnp5 = mnp5;
	return c;
}

/* an MNP frame in start-stop octets (A.3/V.42): SYN DLE STX, the body
   with DLE doubled, DLE ETX, the FCS (x^16 + x^15 + x^2 + 1) */
static std::vector<uint8_t> mnp_octets(const std::vector<uint8_t> &body)
{
	std::vector<uint8_t> out = { 0x16, 0x10, 0x02 };
	uint16_t crc = 0;
	auto add = [&crc](uint8_t c) {
		crc ^= c;
		for (int i = 0; i < 8; i++)
			crc = crc & 1 ? (uint16_t)(crc >> 1 ^ 0xa001) :
				(uint16_t)(crc >> 1);
	};
	for (uint8_t c : body) {
		out.push_back(c);
		if (c == 0x10)
			out.push_back(c);
		add(c);
	}
	out.push_back(0x10);
	out.push_back(0x03);
	add(0x03);
	out.push_back((uint8_t)crc);
	out.push_back((uint8_t)(crc >> 8));
	return out;
}

/* characters as start-stop line bits, appended to line */
static void char_bits(std::vector<uint8_t> &line,
		      const std::vector<uint8_t> &chars)
{
	for (uint8_t c : chars) {
		line.push_back(0);
		for (int i = 0; i < 8; i++)
			line.push_back((c >> i) & 1);
		line.push_back(1);
	}
}

/* the start-stop characters in line bits (marks between them) */
static std::vector<uint8_t> line_chars(const std::vector<uint8_t> &bits)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 10 <= bits.size();) {
		if (bits[i]) {
			i++;
			continue;
		}
		uint8_t c = 0;
		for (int k = 0; k < 8; k++)
			c |= (uint8_t)(bits[i + 1 + k] << k);
		out.push_back(c);
		i += 10;
	}
	return out;
}

/* the bodies of the MNP octet frames in chars (good FCS only) */
static std::vector<std::vector<uint8_t>> mnp_frames(
	const std::vector<uint8_t> &chars)
{
	std::vector<std::vector<uint8_t>> out;
	for (size_t i = 0; i + 3 <= chars.size(); i++) {
		if (chars[i] != 0x16 || chars[i + 1] != 0x10 ||
		    chars[i + 2] != 0x02)
			continue;
		std::vector<uint8_t> body;
		size_t j = i + 3;
		for (; j + 1 < chars.size(); j++) {
			if (chars[j] == 0x10 && chars[j + 1] == 0x10) {
				body.push_back(0x10);
				j++;
			} else if (chars[j] == 0x10 && chars[j + 1] == 0x03)
				break;
			else
				body.push_back(chars[j]);
		}
		if (j + 3 >= chars.size())
			break;
		if (mnp_octets(body) == std::vector<uint8_t>(
			    chars.begin() + i, chars.begin() + j + 4))
			out.push_back(body);
		i = j + 3;
	}
	return out;
}

/* our LR (A.6.4.1/V.42) as the peers' (the Agere, PCTel, Broadcom
   modems'): framing mode 3, k 8, N401 64 and both optimizations (with
   the first, 256-octet information fields), MNP 5; three of them
   (T401), then async (A.7.2.2: no answer at all) */
TEST(Link, MnpLr)
{
	const std::vector<uint8_t> lr = {
		0x1a, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08, 0x04, 0x02, 0x40,
		0x00, 0x08, 0x01, 0x03, 0x09, 0x01, 0x01 };
	/* the Agere modem's LR as it came from it, FCS and all */
	const std::vector<uint8_t> agere = {
		0x16, 0x10, 0x02, 0x1a, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00,
		0x00, 0x00, 0x00, 0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08,
		0x04, 0x02, 0x40, 0x00, 0x08, 0x01, 0x03, 0x09, 0x01, 0x01,
		0x10, 0x03, 0xfa, 0x0a };
	std::vector<uint8_t> agere_body(agere.begin() + 3, agere.end() - 4);
	CHECK(mnp_octets(agere_body) == agere, "the FCS as the Agere modem's");

	Dte d("caller");
	Link l;
	l.init(&d, true, mnp_config(false, true));
	l.start(true, true);
	std::vector<uint8_t> bits, b(48), idle(48, 1);
	for (int k = 0; k < 9600 * 8 / 48; k++) {
		l.get_bits(1, b.data(), 48);
		bits.insert(bits.end(), b.begin(), b.end());
		l.put_bits(1, idle.data(), 48);
	}
	auto frames = mnp_frames(line_chars(bits));
	CHECK(frames.size() == 3, "%zu LRs", frames.size());
	CHECK(!frames.empty() && frames[0] == lr, "our LR");
	CHECK(!frames.empty() && mnp_octets(frames[0]) == agere,
	      "our LR, the Agere modem's");
	CHECK(d.up && l.protocol() == Protocol::None,
	      "async after the LRs went unanswered");
}

/* our responder against the Agere modem's LR, as it came from it
   (MNP 5 asked: not here): our LR in answer (what it understood), the
   LA (fixed fields), the data phase in HDLC frames: an LT's data to the
   DTE and an LA for it, an LN answered with LNA, an LD the end */
TEST(Link, MnpResponder)
{
	const std::vector<uint8_t> agere_lr = {
		0x1a, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08, 0x04, 0x02, 0x40,
		0x00, 0x08, 0x01, 0x03, 0x09, 0x01, 0x01 };
	const std::vector<uint8_t> our_lr = {
		0x17, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08, 0x04, 0x02, 0x40,
		0x00, 0x08, 0x01, 0x03 };
	Dte d("answerer");
	Link l;
	l.init(&d, false, mnp_config(true));
	l.start(true, true);
	std::vector<uint8_t> in(200, 1), out, b(48);
	char_bits(in, mnp_octets(agere_lr));
	in.resize(in.size() + 9600 / 2, 1);
	/* the run so far: what we sent */
	auto run = [&](std::vector<uint8_t> &line) {
		line.resize((line.size() + 47) / 48 * 48, 1);
		for (size_t k = 0; k < line.size(); k += 48) {
			l.get_bits(1, b.data(), 48);
			out.insert(out.end(), b.begin(), b.end());
			l.put_bits(1, line.data() + k, 48);
		}
	};
	run(in);
	CHECK(d.detected && l.protocol() == Protocol::Mnp, "MNP detected");
	auto frames = mnp_frames(line_chars(out));
	CHECK(frames.size() == 1 && frames[0] == our_lr, "our LR (%zu)",
	      frames.size());
	CHECK(!d.up, "not up before the LA");

	/* the confirming LA: N(R) 0, credit 8 */
	in.assign(100, 1);
	char_bits(in, mnp_octets({ 0x03, 0x05, 0x00, 0x08 }));
	in.resize(in.size() + 480, 1);
	run(in);
	CHECK(d.up, "up after the LA");
	l.set_online(true);

	/* HDLC frames from here: an LT, an LN */
	NullOwner o;
	Link side;
	side.init(&o, true, Config());
	Hdlc tx, rx;
	FrameSource src, dst;
	tx.start(&side);
	tx.set_framer(&src, true);
	rx.start(&side);
	rx.set_framer(&dst, false);
	src.tx.push_back({ 0x02, 0x04, 0x01, 'h', 'e', 'l', 'l', 'o' });
	src.tx.push_back({ 0x07, 0x06, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01 });
	src.tx.push_back({ 0x04, 0x02, 0x01, 0x01, 0xff });
	std::vector<uint8_t> tb(48);
	for (int k = 0; k < 9600 / 48 && !d.released; k++) {
		tx.get_bits(1, tb.data(), 48);
		l.put_bits(1, tb.data(), 48);
		l.get_bits(1, b.data(), 48);
		rx.put_bits(1, b.data(), 48);
	}
	CHECK(std::string(d.got.begin(), d.got.end()) == "hello",
	      "the LT's data");
	bool la = false, lna = false;
	for (auto &f : dst.rx) {
		la |= f == std::vector<uint8_t>{ 0x03, 0x05, 0x01, 0x08 };
		lna |= f == std::vector<uint8_t>{ 0x04, 0x07, 0x01, 0x01,
						  0x01 };
	}
	CHECK(la, "LA for the LT");
	CHECK(lna, "LNA for the LN");
	CHECK(d.released, "released by the LD");
}

/* SYN DLE STX in the detection phase, and no LR after it (a caller
   without error correction whose data has them; DLE ETX and a bad FCS
   end it): after the responder's wait, the call goes on without error
   correction, nothing sent (no LD) */
TEST(Link, MnpResponderNoLr)
{
	Dte d("answerer");
	Link l;
	l.init(&d, false, mnp_config(true));
	l.start(true, true);
	std::vector<uint8_t> in(200, 1), out, b(48);
	char_bits(in, { 0x16, 0x10, 0x02, 'n', 'o', 't', ' ', 'a', 'n', ' ',
			'L', 'R', 0x10, 0x03, 0x00, 0x00 });
	in.resize(9600 * 10, 1);
	for (size_t k = 0; k + 48 <= in.size(); k += 48) {
		l.get_bits(1, b.data(), 48);
		out.insert(out.end(), b.begin(), b.end());
		l.put_bits(1, in.data() + k, 48);
	}
	CHECK(d.up && l.protocol() == Protocol::None,
	      "async after the wait");
	CHECK(!d.released, "not released");
	CHECK(line_chars(out).empty(), "nothing sent");
}

/* MNP first (+ES=4) at the caller, both ways in HDLC frames with the
   optimizations: text, noise, then our LD */
TEST(Link, Mnp)
{
	rnd_state = 12345;
	Config cc = mnp_config(true);
	cc.mnp_first = true;
	Pair p(cc, mnp_config(true));
	p.start(true, true);
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "MNP up");
	CHECK(p.lc.protocol() == Protocol::Mnp &&
	      p.la.protocol() == Protocol::Mnp, "MNP chosen");
	transfer("mnp, text", p, 60000, 40000, 9600 * 120);
	transfer("mnp, noise", p, 20000, 20000, 9600 * 60, false);
	p.lc.disconnect();
	p.run(9600);
	CHECK(p.dc.released && p.da.released, "released by the LD");
}

/* the caller has V.42 and MNP, the answerer MNP alone: it passes over
   the ODPs, the caller sends its LR after T400 (7.2.1.2/V.42) */
TEST(Link, MnpAfterOdp)
{
	rnd_state = 12345;
	Pair p(mnp_config(true), mnp_config(false));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "up");
	CHECK(p.lc.protocol() == Protocol::Mnp &&
	      p.la.protocol() == Protocol::Mnp, "MNP chosen");
	transfer("mnp after odp", p, 5000, 5000, 9600 * 20);
}

/* framing mode 2 (start-stop octets in the data phase too), and
   without the optimizations (variable-field LT and LA, N401 64) */
TEST(Link, MnpOctets)
{
	rnd_state = 12345;
	Config cc = mnp_config(false);
	cc.mnp_mode = 2;
	Pair p(cc, mnp_config(true));
	p.start(true, true);
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "up in mode 2");
	/* DLE in the data: noise has it */
	transfer("mnp mode 2, noise", p, 10000, 10000, 9600 * 60, false);

	Config ca = mnp_config(true);
	ca.mnp_opt = 0;
	ca.mnp_info = 64;
	Pair q(mnp_config(false), ca);
	q.start(true, true);
	q.run(9600 * 2);
	CHECK(q.dc.up && q.da.up, "up without the optimizations");
	transfer("mnp, variable fields", q, 10000, 10000, 9600 * 60, false);
}

/* bit errors in both framing modes: LT frames lost or damaged come
   again (out-of-sequence LAs, T401) */
TEST(Link, MnpBitErrors)
{
	for (int mode = 3; mode >= 2; mode--) {
		rnd_state = 12345;
		Config cc = mnp_config(false);
		cc.mnp_mode = mode;
		Pair p(cc, mnp_config(false));
		p.start(true, true);
		p.run(9600 * 2);
		CHECK(p.dc.up && p.da.up, "mode %d: up", mode);
		p.ber = 2e-5;
		transfer(mode == 3 ? "mnp, bit errors" :
			 "mnp mode 2, bit errors", p, 30000, 30000,
			 9600 * 300);
		CHECK(p.errors > 0, "no bit errors");
	}
}

/* a DTE that takes 7 characters at a time: no credit (LA with N(k) 0),
   the rest later */
TEST(Link, MnpSlowDte)
{
	rnd_state = 12345;
	Pair p(mnp_config(false), mnp_config(false));
	p.start(true, true);
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "up");
	p.da.accept = 7;
	transfer("mnp, slow DTE", p, 20000, 2000, 9600 * 120);
}

/* MNP required (+ES=4,4 / \N5) against a modem without error
   correction: the LRs unanswered, the link fails rather than going
   async */
TEST(Link, MnpRequired)
{
	rnd_state = 12345;
	Config cc = mnp_config(false);
	cc.required = true;
	Pair p(cc, Config());
	p.start(true, false);
	p.run(9600 * 10);
	CHECK(p.dc.released && !p.dc.up, "caller: released, not up");
}

/* N400 (12): with the line cut in the data phase an LT goes out again
   twelve times, then LD */
TEST(Link, MnpN400)
{
	rnd_state = 12345;
	Pair p(mnp_config(false), mnp_config(false));
	p.start(true, true);
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "up");
	p.dc.to_send = text(100);
	p.dc.pos = 0;
	p.lc.set_online(true);
	NullOwner o;
	Link side;
	side.init(&o, true, Config());
	Hdlc rx;
	FrameSource dst;
	rx.start(&side);
	rx.set_framer(&dst, false);
	std::vector<uint8_t> b(480), idle(480, 1);
	for (int k = 0; k < 9600 * 120 / 480 && !p.dc.released; k++) {
		p.lc.get_bits(1, b.data(), (int)b.size());
		rx.put_bits(1, b.data(), (int)b.size());
		p.lc.put_bits(1, idle.data(), (int)idle.size());
	}
	int lts = 0, lds = 0;
	for (auto &f : dst.rx) {
		if (f.size() > 3 && f[0] == 0x02 && f[1] == 0x04 && f[2] == 1)
			lts++;
		if (f.size() == 5 && f[1] == 0x02)
			lds++;
	}
	CHECK(lts == 13, "LT 1 sent %d times (once and N400 again)", lts);
	CHECK(lds == 1 && p.dc.released, "LD, released");
}


/*
 *    MNP 5
 *
 */

/* in through an MNP 5 encoder (a flush where flush_at says, and at the
   end) and its output through a decoder: the same characters again;
   the encoder's octets */
static size_t mnp5_roundtrip(const char *name, const std::vector<uint8_t> &in,
			     size_t flush_every = 0)
{
	Mnp5 enc, dec;
	enc.init();
	dec.init();
	std::vector<uint8_t> coded, out;
	uint8_t buf[1100];
	for (size_t i = 0; i < in.size(); i++) {
		enc.encode(in[i]);
		if (flush_every && (i + 1) % flush_every == 0)
			enc.flush_encoder();
		int n;
		while ((n = enc.out(buf, sizeof(buf))) > 0)
			coded.insert(coded.end(), buf, buf + n);
	}
	enc.flush_encoder();
	/* a second flush with nothing coded since: nothing */
	enc.flush_encoder();
	int n;
	while ((n = enc.out(buf, sizeof(buf))) > 0)
		coded.insert(coded.end(), buf, buf + n);
	for (uint8_t c : coded) {
		dec.decode(c);
		while ((n = dec.out(buf, sizeof(buf))) > 0)
			out.insert(out.end(), buf, buf + n);
	}
	CHECK(out == in, "%s: %zu characters back of %zu", name, out.size(),
	      in.size());
	printf("  mnp5 %s: %zu -> %zu octets\n", name, in.size(), coded.size());
	return coded.size();
}

/* MNP 5 round trips: text (smaller), noise (larger, by up to a quarter:
   ranks of 128 and up take 10 bits), runs of every length up to 600
   (counted after three; 250 at most, then a new run), every value, the
   flush within runs; and what the Agere modem sent (MNP 5 as PCTel's
   code has it) decoded */
TEST(Link, Mnp5)
{
	rnd_state = 12345;
	CHECK(mnp5_roundtrip("text", text(20000)) < 20000 * 3 / 4,
	      "text compresses");
	CHECK(mnp5_roundtrip("noise", noise(20000)) < 20000 * 5 / 4,
	      "noise grows by a quarter at most");
	std::vector<uint8_t> runs;
	for (int len = 1; len <= 600; len++)
		runs.insert(runs.end(), (size_t)len, (uint8_t)(len * 7));
	CHECK(mnp5_roundtrip("runs", runs) < runs.size() / 20, "runs");
	std::vector<uint8_t> all;
	for (int k = 0; k < 40; k++)
		for (int c = 0; c < 256; c++)
			all.push_back((uint8_t)(c * 3 + k));
	mnp5_roundtrip("every value", all);
	mnp5_roundtrip("runs, flushed", runs, 37);
	mnp5_roundtrip("text, flushed", text(5000), 1);

	/* the Agere modem's first octets, sending "000000  !\"#$%..." (the
	   STREAM of interop_call) */
	static const uint8_t agere[] = {
		0xb0, 0x00, 0x3a, 0x11, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
		0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0x0b,
		0x1b, 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b, 0x8b, 0x9b, 0xab,
		0xbb, 0xcb, 0xdb, 0xeb, 0xfc, 0x06, 0x07, 0x05, 0x83, 0xc2,
		0x61, 0x70, 0xd8, 0x7c, 0x46, 0x27, 0x15, 0x8b, 0xc6, 0x63,
		0x71, 0xd8, 0xfc, 0x86, 0x47, 0x25, 0x93, 0xca, 0x65, 0x72,
		0xd9, 0x7c, 0xc7, 0xff,
	};
	std::string want = "000000 ";
	for (char c = ' '; c <= 'X'; c++)
		want += c;
	Mnp5 dec;
	dec.init();
	std::string got;
	uint8_t buf[600];
	for (uint8_t c : agere) {
		dec.decode(c);
		int n = dec.out(buf, sizeof(buf));
		got.append((const char *)buf, n);
	}
	CHECK(got == want, "the Agere modem's MNP 5: %s", got.c_str());
	/* our encoder: the same octets for the same text (the whole octets
	   it has: the Agere modem's go on with the next characters) */
	Mnp5 enc;
	enc.init();
	std::vector<uint8_t> coded;
	for (char c : want) {
		enc.encode((uint8_t)c);
		int n = enc.out(buf, sizeof(buf));
		coded.insert(coded.end(), buf, buf + n);
	}
	CHECK(coded.size() >= 60 &&
	      !memcmp(coded.data(), agere, coded.size()),
	      "our MNP 5 as the Agere modem's (%zu octets)", coded.size());
}

/* MNP with MNP 5 both ways, two Links back to back: text and noise */
TEST(Link, MnpMnp5)
{
	rnd_state = 12345;
	Pair p(mnp_config(false, true), mnp_config(true, true));
	p.start(true, true);
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "up");
	CHECK(p.dc.agreed.mnp5 && p.da.agreed.mnp5, "MNP 5 agreed");
	transfer("mnp5, text", p, 60000, 40000, 9600 * 120);
	transfer("mnp5, noise", p, 10000, 10000, 9600 * 60, false);
	/* one side without: none */
	Pair q(mnp_config(false, true), mnp_config(true, false));
	q.start(true, true);
	q.run(9600 * 2);
	CHECK(q.dc.up && q.da.up && !q.dc.agreed.mnp5 && !q.da.agreed.mnp5,
	      "MNP 5 not agreed");
}


/*
 *    V.80 Synchronous Access Mode
 *
 */

namespace {

/* a V.80 DTE: what it sends (EM-coded already) and what it got, the
   in-band commands it gave the DCE, the octets it says wait in its
   buffer */
struct SyncDte : Owner {
	std::vector<uint8_t> to_send, got;
	size_t pos = 0;
	bool up = false;
	int accept = 1 << 30;
	std::vector<int> commands;      /* SyncCommand, then rate codes */
	int32_t buffered = 0;
	/* the rest of to_send */
	s16 read_chars(uint8_t *buf, s16 n) override {
		int k = (int)std::min((size_t)n, to_send.size() - pos);
		memcpy(buf, to_send.data() + pos, k);
		pos += k;
		return k;
	}
	/* up to accept characters into got */
	s16 write_chars(const uint8_t *buf, s16 n) override {
		if (n > accept)
			n = accept;
		got.insert(got.end(), buf, buf + n);
		return n;
	}
	/* Up */
	void status(Status s) override {
		if (s == Status::Up)
			up = true;
	}
	/* 9600 bit/s */
	int32_t tx_rate() override { return 9600; }
	/* the command (and <EM><rate>'s codes) */
	void sync_command(SyncCommand c, u8 tx, u8 rx) override {
		commands.push_back((int)c);
		if (c == SyncCommand::Rate) {
			commands.push_back(tx);
			commands.push_back(rx);
		}
	}
	/* buffered */
	int32_t tx_buffered() override { return buffered; }
	/* LINK_DEBUG */
	void log(const char *line) override {
		const char *v = getenv("LINK_DEBUG");
		if (v && *v)
			printf("    %s\n", line);
	}
};

/* two Links in Synchronous Access Mode back to back, online from the
   start; flip: the bit of the A to B stream to invert (-1 none) */
struct SyncPair {
	SyncDte da, db;
	Link la, lb;
	long flip = -1, sent = 0;

	/* the call, each with its +ESA */
	SyncPair(const SyncConfig &ca, const SyncConfig &cb) {
		Config c;
		c.sync = true;
		c.sync_cfg = ca;
		la.init(&da, true, c);
		c.sync_cfg = cb;
		lb.init(&db, false, c);
	}
	/* connected: the DTEs online */
	void start() {
		la.start(false, false);
		lb.start(false, false);
		la.set_online(true);
		lb.set_online(true);
	}
	/* bits both ways; B's transmitter first (its DTE's commands, such
	   as <EM><flag>, before A's bits come in) */
	void run(long bits, int chunk = 48) {
		std::vector<uint8_t> b(chunk);
		for (long k = 0; k < bits; k += chunk) {
			lb.get_bits(1, b.data(), chunk);
			la.put_bits(1, b.data(), chunk);
			la.get_bits(1, b.data(), chunk);
			for (auto &x : b)
				if (sent++ == flip)
					x ^= 1;
			lb.put_bits(1, b.data(), chunk);
		}
	}
};

/* in for the DCE: EM, 99h (and DC1, DC3 if dc) shielded with T1 to T4 */
std::vector<uint8_t> shield(const std::vector<uint8_t> &in, bool dc = false)
{
	std::vector<uint8_t> v;
	for (uint8_t c : in) {
		int t = c == v80::EM ? v80::T1 : c == v80::EM8 ? v80::T2 :
			dc && c == v80::DC1 ? v80::T3 :
			dc && c == v80::DC3 ? v80::T4 : -1;
		if (t >= 0) {
			v.push_back(v80::EM);
			v.push_back((uint8_t)t);
		} else
			v.push_back(c);
	}
	return v;
}

/* <EM><code> */
void em(std::vector<uint8_t> &v, uint8_t code)
{
	v.push_back(v80::EM);
	v.push_back(code);
}

/* what the DCE gave the DTE: octets as they are (0 to 255),
   indications as 256 + code with their parameter octets after them */
std::vector<int> events(const std::vector<uint8_t> &got)
{
	std::vector<int> e;
	for (size_t i = 0; i < got.size(); i++) {
		uint8_t c = got[i];
		if (c != v80::EM || i + 1 == got.size()) {
			e.push_back(c);
			continue;
		}
		uint8_t code = got[++i];
		static const uint8_t single[4] = { v80::T1, v80::T2, v80::T3,
						   v80::T4 };
		static const uint8_t octet[4] = { v80::EM, v80::EM8, v80::DC1,
						  v80::DC3 };
		bool done = false;
		for (int k = 0; k < 4; k++)
			if (code == single[k]) {
				e.push_back(octet[k]);
				done = true;
			}
		if (done)
			continue;
		e.push_back(256 + code);
		int par = code == v80::BNUM || code == v80::UNUM ||
			code == v80::RATE ? 2 : 0;
		for (int k = 0; k < par && i + 1 < got.size(); k++)
			e.push_back(got[++i]);
	}
	return e;
}

/* events as text, for a failed check: indications by their codes */
std::string dump(const std::vector<int> &e, size_t max = 40)
{
	std::string s;
	for (size_t i = 0; i < e.size() && i < max; i++) {
		char b[16];
		snprintf(b, sizeof(b), e[i] >= 256 ? " <%02x>" : " %02x",
			 e[i] & 0xff);
		s += b;
	}
	if (e.size() > max)
		s += " ...";
	return s;
}

/* the indication code as an event */
int ind(uint8_t code)
{
	return 256 + code;
}

/* the events without the idle octets before the first other one
   (transparent: what the line had before the data) */
std::vector<int> after_idle(const std::vector<int> &e, int idle)
{
	size_t i = 0;
	while (i < e.size() && e[i] == idle)
		i++;
	return std::vector<int>(e.begin() + (long)i, e.end());
}

/* frames for the DTE to send: <EM><flag> before the first and after
   each */
std::vector<uint8_t> framed(const std::vector<std::vector<uint8_t>> &f)
{
	std::vector<uint8_t> v;
	em(v, v80::FLAG);
	for (const auto &x : f) {
		std::vector<uint8_t> s = shield(x);
		v.insert(v.end(), s.begin(), s.end());
		em(v, v80::FLAG);
	}
	return v;
}

/* the events of good frames f as B's DTE gets them after the hunt:
   <EM><err> for the first flag, then each frame and <EM><flag> */
std::vector<int> good_frames(const std::vector<std::vector<uint8_t>> &f)
{
	std::vector<int> e = { ind(v80::ERR) };
	for (const auto &x : f) {
		e.insert(e.end(), x.begin(), x.end());
		e.push_back(ind(v80::FLAG));
	}
	return e;
}

/* a frame's octets: n random ones, with the octets that need care
   (EM, 99h, DC1, DC3, a flag, 1s) among them */
std::vector<uint8_t> frame_octets(size_t n)
{
	std::vector<uint8_t> v = noise(n);
	static const uint8_t hard[] = { 0x19, 0x99, 0x11, 0x13, 0x7e, 0xff,
					0xff, 0x7d, 0x3f };
	for (size_t i = 0; i < sizeof(hard) && i * 7 < n; i++)
		v[i * 7] = hard[i];
	return v;
}

} // namespace

/* transparent: the DTE's octets (EM and 99h shielded) go out as they
   are; the line's octets come back to the other DTE, the idle SYN
   octets too; each DTE hears of its first underrun */
TEST(Link, V80Transparent)
{
	rnd_state = 4321;
	SyncConfig c;
	c.syn1 = 0xa5;
	SyncPair p(c, c);
	std::vector<uint8_t> data = frame_octets(500);
	p.da.to_send = shield(data);
	p.start();
	p.run(6000);
	CHECK(p.da.up && p.db.up, "up");
	CHECK(p.la.sync() && !p.la.ec(), "synchronous access");
	std::vector<int> a = events(p.da.got), b = events(p.db.got);
	/* B's DTE: its own underrun first, then A's octets, then A's
	   idle */
	CHECK(!b.empty() && b[0] == ind(v80::UNDER), "B's underrun");
	b.erase(b.begin());
	CHECK(b.size() > data.size(), "B got %zu", b.size());
	b.resize(std::min(b.size(), data.size() + 4));
	std::vector<int> want(data.begin(), data.end());
	want.insert(want.end(), 4, 0xa5);
	CHECK(b == want, "B's octets:%s", dump(b).c_str());
	/* A's DTE: B's idle octets, and its own underrun after its data */
	CHECK(std::count(a.begin(), a.end(), ind(v80::UNDER)) == 1,
	      "one underrun");
	a.erase(std::remove(a.begin(), a.end(), ind(v80::UNDER)), a.end());
	CHECK(after_idle(a, 0xa5).empty(), "A got only B's idle octets:%s",
	      dump(a).c_str());
}

/* the 20 shielding codes from the DTE (both forms of EM), and DC1/DC3
   shielded towards a DTE with DC1/DC3 flow control */
TEST(Link, V80Shielding)
{
	SyncConfig ca, cb;
	cb.shield_dc = true;
	SyncPair p(ca, cb);
	std::vector<uint8_t> s, want;
	static const uint8_t oct[4] = { 0x19, 0x99, 0x11, 0x13 };
	for (int i = 0; i < 4; i++) {
		s.push_back(i & 1 ? v80::EM8 : v80::EM);
		s.push_back((uint8_t)(v80::T1 + (i == 1 ? 0x1a :
						 i ? 0x44 + i - 2 : 0)));
		want.push_back(oct[i]);
	}
	/* T5 to T8 */
	const uint8_t twice[4] = { v80::T5, v80::T6, v80::T7, v80::T8 };
	for (int i = 0; i < 4; i++) {
		em(s, twice[i]);
		want.push_back(oct[i]);
		want.push_back(oct[i]);
	}
	/* T9 to T20 */
	int code = v80::T9;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			if (i != j) {
				em(s, (uint8_t)code++);
				want.push_back(oct[i]);
				want.push_back(oct[j]);
			}
	CHECK(code == v80::T20 + 1, "codes");
	p.da.to_send = s;
	p.start();
	p.run(2000);
	std::vector<int> b = events(p.db.got);
	b.erase(b.begin());             /* B's own underrun */
	b.resize(std::min(b.size(), want.size()));
	CHECK(b == std::vector<int>(want.begin(), want.end()),
	      "B got the octets");
	/* every one shielded: no bare EM, 99h, DC1 or DC3 */
	bool bare = false;
	for (size_t i = 0; i < p.db.got.size(); i++) {
		uint8_t c = p.db.got[i];
		if (c == 0x99 || c == 0x11 || c == 0x13)
			bare = true;
		if (c == 0x19)
			i++;
	}
	CHECK(!bare, "B's octets shielded");
	/* DC1 and DC3 without flow control go to the DTE as they are */
	SyncPair q(ca, ca);
	q.da.to_send = { 0x11, 0x13 };
	q.start();
	q.run(500);
	std::vector<uint8_t> g(q.db.got.begin() + 2, q.db.got.end());
	CHECK(g.size() >= 2 && g[0] == 0x11 && g[1] == 0x13, "DC1 DC3 bare");
	/* a DTE with DC1/DC3 flow control: its bare DC1 and DC3 are not
	   data */
	SyncConfig cf;
	cf.dc_flow = true;
	SyncPair r(cf, ca);
	r.da.to_send = { 'a', 0x11, 'b', 0x13, 'c' };
	r.start();
	r.run(500);
	std::vector<int> e = events(r.db.got);
	CHECK(e.size() >= 4 && e[1] == 'a' && e[2] == 'b' && e[3] == 'c',
	      "flow control characters dropped");
}

/* framed: frames with the octets that need care, with no FCS, FCS-16
   and FCS-32, each received whole, between <EM><err> (the hunt's first
   flag) and <EM><flag>s */
TEST(Link, V80Framed)
{
	for (int crc = 0; crc <= 2; crc++) {
		for (int idle = 0; idle <= 1; idle++) {
			rnd_state = 777 + crc;
			SyncConfig c;
			c.crc = (s16)crc;
			c.framed_idle = (s16)idle;
			SyncPair p(c, c);
			std::vector<std::vector<uint8_t>> f = {
				frame_octets(1), frame_octets(64),
				frame_octets(300), frame_octets(2) };
			p.da.to_send = framed(f);
			em(p.db.to_send, v80::FLAG);
			p.start();
			p.run(20000);
			std::vector<int> b = events(p.db.got), want;
			want = good_frames(f);
			/* the frames go back to back (a flag between
			   them); with marks after the last, <EM><mark> */
			if (idle)
				want.push_back(ind(v80::MARK));
			CHECK(b == want, "crc %d, idle %d: %zu events, %zu "
			      "wanted:%s", crc, idle, b.size(), want.size(),
			      dump(b, 400).c_str());
			CHECK(p.lb.stats().rx_frames == f.size(),
			      "crc %d: %u frames", crc,
			      p.lb.stats().rx_frames);
			/* A's DTE: B's flags as octets until A's own
			   <EM><flag> made it framed, then B's idle flags
			   after its hunt (or its marks) */
			std::vector<int> a = after_idle(events(p.da.got), 0x7e);
			a = after_idle(a, 0xff);
			CHECK(a == (idle ? std::vector<int>{} :
				    std::vector<int>{ ind(v80::ERR) }),
			      "crc %d: A's events:%s", crc, dump(a).c_str());
		}
	}
}

/* a bit inverted in a frame: <EM><err> at its end (the FCS checked),
   the next frame good */
TEST(Link, V80BadFcs)
{
	for (int crc = 1; crc <= 2; crc++) {
		rnd_state = 99;
		SyncConfig c;
		c.crc = (s16)crc;
		SyncPair p(c, c);
		std::vector<std::vector<uint8_t>> f = {
			std::vector<uint8_t>(200, 0x55), frame_octets(50) };
		p.da.to_send = framed(f);
		em(p.db.to_send, v80::FLAG);
		p.flip = 48 + 8 * 100;          /* in the first frame */
		p.start();
		p.run(8000);
		std::vector<int> b = events(p.db.got);
		std::vector<int> tail = { ind(v80::ERR) };
		tail.insert(tail.end(), f[1].begin(), f[1].end());
		tail.push_back(ind(v80::FLAG));
		CHECK(b.size() > tail.size() &&
		      std::vector<int>(b.end() - (long)tail.size(), b.end()) ==
		      tail, "crc %d: the bad frame's <err>, the good one",
		      crc);
		CHECK(b[0] == ind(v80::ERR), "crc %d: the hunt", crc);
		CHECK(std::count(b.begin(), b.end(), ind(v80::FLAG)) == 1,
		      "crc %d: one good frame", crc);
		CHECK(p.lb.stats().rx_errors == 1 && p.lb.stats().rx_frames == 1,
		      "crc %d: counts", crc);
	}
}

/* the DTE's abort (<EM><err>) in a frame: the remote's DTE gets what
   came, <EM><mark>, then the hunt's <EM><err> at the next flag */
TEST(Link, V80Abort)
{
	SyncConfig c;
	c.crc = 1;
	SyncPair p(c, c);
	std::vector<uint8_t> part = { 1, 2, 3, 4, 5 };
	em(p.da.to_send, v80::FLAG);
	p.da.to_send.insert(p.da.to_send.end(), part.begin(), part.end());
	em(p.da.to_send, v80::ERR);
	std::vector<uint8_t> f = { 9, 8, 7 };
	p.da.to_send.insert(p.da.to_send.end(), f.begin(), f.end());
	em(p.da.to_send, v80::FLAG);
	em(p.db.to_send, v80::FLAG);
	p.start();
	p.run(4000);
	/* the abort's octets that came are held back (they may be the
	   FCS): only those before the last two reach the DTE */
	std::vector<int> want = { ind(v80::ERR), 1, 2, 3, ind(v80::MARK),
				  ind(v80::ERR), 9, 8, 7, ind(v80::FLAG) };
	CHECK(events(p.db.got) == want, "abort, then a frame");
}

/* the DTE runs out in a frame: framed_un_ov 0, an abort; 1, a flag and
   the DTE's octets dropped until <EM><resume>, which <EM><unum>
   answers with their count */
TEST(Link, V80Underrun)
{
	SyncConfig c;
	c.crc = 1;
	SyncPair p(c, c);
	em(p.da.to_send, v80::FLAG);
	for (int i = 0; i < 10; i++)
		p.da.to_send.push_back((uint8_t)(0x40 + i));
	em(p.db.to_send, v80::FLAG);
	p.start();
	p.run(2000);
	CHECK(after_idle(events(p.da.got), 0x7e) == (std::vector<int>{
		      ind(v80::ERR), ind(v80::UNDER) }),
	      "A: abort's underrun:%s", dump(events(p.da.got)).c_str());
	std::vector<int> b = events(p.db.got);
	CHECK(b.size() >= 2 && b[b.size() - 2] == ind(v80::MARK) &&
	      b.back() == ind(v80::ERR), "B: the abort, the hunt");

	c.framed_un_ov = 1;
	SyncPair q(c, c);
	em(q.da.to_send, v80::FLAG);
	for (int i = 0; i < 10; i++)
		q.da.to_send.push_back((uint8_t)(0x40 + i));
	em(q.db.to_send, v80::FLAG);
	q.start();
	q.run(2000);
	CHECK(after_idle(events(q.da.got), 0x7e) == (std::vector<int>{
		      ind(v80::ERR), ind(v80::UNDER) }),
	      "A: flag's underrun:%s", dump(events(q.da.got)).c_str());
	b = events(q.db.got);
	CHECK(!b.empty() && b.back() == ind(v80::ERR),
	      "B: the cut frame's FCS is bad");
	/* octets after the underrun are dropped, then <EM><resume> */
	std::vector<uint8_t> drop;
	for (int i = 0; i < 300; i++)
		drop.push_back((uint8_t)i);
	drop = shield(drop);
	q.da.to_send.insert(q.da.to_send.end(), drop.begin(), drop.end());
	em(q.da.to_send, v80::RESUME);
	size_t nb = q.db.got.size();
	q.run(4000);
	std::vector<int> a = after_idle(events(q.da.got), 0x7e);
	CHECK(a.size() == 5 && a[2] == ind(v80::UNUM) &&
	      a[3] == ((300 & 0x7f) << 1) && a[4] == ((300 >> 7) << 1),
	      "A: <unum> 300:%s", dump(a).c_str());
	CHECK(q.db.got.size() == nb, "B: nothing more");
	/* then a frame goes */
	std::vector<uint8_t> f = { 'o', 'k' };
	q.da.to_send.insert(q.da.to_send.end(), f.begin(), f.end());
	em(q.da.to_send, v80::FLAG);
	q.run(2000);
	b = events(q.db.got);
	CHECK(b.size() >= 3 && b[b.size() - 3] == 'o' &&
	      b[b.size() - 2] == 'k' && b.back() == ind(v80::FLAG),
	      "B: the frame after the resume");
}

/* back to transparent (<EM><mark>) and framed again; the DTE's other
   commands to the owner; <EM><bnum> every report period */
TEST(Link, V80Commands)
{
	SyncConfig c;
	c.report_period = 1;            /* 10 ms: 96 bits at 9600 bit/s */
	SyncPair p(c, SyncConfig());
	std::vector<uint8_t> &s = p.da.to_send;
	em(s, v80::ECS);
	em(s, v80::RTN);
	em(s, v80::RRN);
	s.push_back(v80::EM);
	s.push_back(v80::RATE);
	s.push_back(0x2c);
	s.push_back(0x24);
	em(s, v80::EOT);
	em(s, 0x60);                    /* not one: ignored */
	s.push_back('x');
	p.da.buffered = 300;
	p.start();
	p.run(96 * 10);
	std::vector<int> want = { (int)SyncCommand::Ecs,
		(int)SyncCommand::Retrain, (int)SyncCommand::Reneg,
		(int)SyncCommand::Rate, 0x2c, 0x24, (int)SyncCommand::Eot };
	CHECK(p.da.commands == want, "commands");
	std::vector<int> a = events(p.da.got);
	long reports = 0;
	for (size_t i = 0; i + 2 < a.size(); i++)
		if (a[i] == ind(v80::BNUM)) {
			reports++;
			CHECK(a[i + 1] == ((300 & 0x7f) << 1) &&
			      a[i + 2] == ((300 >> 7) << 1), "bnum 300");
		}
	CHECK(reports >= 9 && reports <= 11, "%ld reports", reports);
	CHECK(v80::rate_code(33600) == 0x2e && v80::rate_code(1200) == 0x20 &&
	      v80::rate_code(28800) == 0x2c && v80::rate_code(56000) == 0x2e &&
	      v80::code_rate(0x24) == 9600 && v80::code_rate(0x30) == 0,
	      "rate codes");
	/* framed, then transparent: B's octets as they come again */
	SyncPair q{SyncConfig(), SyncConfig()};
	q.da.to_send = framed({ { 1, 2, 3 } });
	em(q.da.to_send, v80::MARK);
	q.da.to_send.insert(q.da.to_send.end(), 100, 0x42);
	em(q.db.to_send, v80::FLAG);
	q.start();
	q.run(2000);
	em(q.db.to_send, v80::MARK);
	q.run(2000);
	std::vector<int> b = events(q.db.got);
	CHECK(b.size() > 5 && b[0] == ind(v80::ERR) && b[1] == 1 &&
	      b[4] == ind(v80::FLAG), "B: the frame");
	/* A idles with SYN octets (FFh) after its 42hs: B transparent
	   again gets them as octets */
	CHECK(b.size() > 20 &&
	      std::count(b.end() - 20, b.end(), 0xff) == 20,
	      "B: transparent octets:%s", dump(b, 400).c_str());
}

namespace {

/* line bits (the first on the line first) as the DTE's transparent
   octets, least significant bit first, the last one padded with 1s */
std::vector<uint8_t> pack_bits(const std::vector<int> &bits)
{
	std::vector<uint8_t> v((bits.size() + 7) / 8, 0xff);
	for (size_t i = 0; i < bits.size(); i++)
		if (!bits[i])
			v[i / 8] &= (uint8_t)~(1 << (i % 8));
	return v;
}

/* n 1s, then octets' bits, least significant first */
void add_bits(std::vector<int> &bits, int ones,
	      const std::vector<uint8_t> &octets)
{
	bits.insert(bits.end(), (size_t)ones, 1);
	for (uint8_t c : octets)
		for (int k = 0; k < 8; k++)
			bits.push_back(c >> k & 1);
}

/* the events without the DCE's underrun indications */
std::vector<int> no_under(std::vector<int> e)
{
	e.erase(std::remove(e.begin(), e.end(), ind(v80::UNDER)), e.end());
	return e;
}

} // namespace

/* the transparent receiver's hunt (<trans_idle> 1, 2): nothing until
   the SYN, the octets aligned on it from there (the SYN first); 8 bits
   find a lone <syn1>, 16 only <syn1> <syn2>; <EM><hunt> hunts again */
TEST(Link, V80SynHunt)
{
	rnd_state = 2468;
	std::vector<uint8_t> data = frame_octets(40);
	for (int idle = 1; idle <= 2; idle++) {
		SyncConfig cb;
		cb.trans_idle = (s16)idle;
		cb.syn1 = 0x16;
		cb.syn2 = 0x96;
		SyncPair p(SyncConfig(), cb);
		/* 13 1s (off the octets), a lone <syn1>, 3 1s, the whole
		   SYN, the data */
		std::vector<int> bits;
		add_bits(bits, 13, { 0x16, 0x00 });
		add_bits(bits, 3, { 0x16, 0x96 });
		add_bits(bits, 0, data);
		p.da.to_send = shield(pack_bits(bits));
		p.start();
		p.run(2000);
		std::vector<int> b = no_under(events(p.db.got));
		if (idle == 1) {
			CHECK(b.size() > 2 && b[0] == 0x16 && b[1] == 0x00,
			      "8 bits: the lone syn1:%s", dump(b).c_str());
			continue;
		}
		std::vector<int> want = { 0x16, 0x96 };
		want.insert(want.end(), data.begin(), data.end());
		CHECK(b.size() > want.size() &&
		      std::vector<int>(b.begin(), b.begin() +
				       (long)want.size()) == want,
		      "16 bits: the SYN, the data:%s", dump(b).c_str());
	}

	/* <EM><hunt>: A's FFh idle is not the SYN, nothing comes until
	   its next SYN, 7 bits off from the last */
	SyncConfig cb;
	cb.trans_idle = 1;
	cb.syn1 = 0x16;
	SyncPair q(SyncConfig(), cb);
	std::vector<int> bits;
	add_bits(bits, 5, { 0x16, 1, 2, 3 });
	q.da.to_send = shield(pack_bits(bits));
	q.start();
	q.run(1000);
	std::vector<int> b = no_under(events(q.db.got));
	CHECK(b.size() > 4 && b[0] == 0x16 && b[1] == 1 && b[3] == 3,
	      "first SYN:%s", dump(b).c_str());
	em(q.db.to_send, v80::HUNT);
	q.run(1000);
	size_t nb = q.db.got.size();
	bits.clear();
	add_bits(bits, 7, { 0x16 });
	add_bits(bits, 0, data);
	std::vector<uint8_t> more = shield(pack_bits(bits));
	q.da.to_send.insert(q.da.to_send.end(), more.begin(), more.end());
	q.run(2000);
	b = no_under(events(std::vector<uint8_t>(q.db.got.begin() + (long)nb,
						 q.db.got.end())));
	std::vector<int> want = { 0x16 };
	want.insert(want.end(), data.begin(), data.end());
	CHECK(b.size() > want.size() &&
	      std::vector<int>(b.begin(), b.begin() + (long)want.size()) ==
	      want, "after the hunt:%s", dump(b).c_str());

	/* <trans_idle> 2 idles with <syn1> <syn2> in turn; <EM><hunt> with
	   <trans_idle> 0 does nothing */
	SyncConfig ca;
	ca.trans_idle = 2;
	ca.syn1 = 0xa5;
	ca.syn2 = 0x5a;
	SyncPair r(ca, SyncConfig());
	em(r.db.to_send, v80::HUNT);
	r.start();
	r.run(1000);
	b = no_under(events(r.db.got));
	bool alt = b.size() > 40;
	for (size_t i = 0; alt && i < 40; i++)
		alt = b[i] == (i & 1 ? 0x5a : 0xa5);
	CHECK(alt, "syn1 syn2 in turn:%s", dump(b).c_str());
}

/* the DTE takes nothing for a while: the ring fills, the octets after
   are lost and <EM><rover> marks the place */
TEST(Link, V80ReceiveOverrun)
{
	SyncPair p{SyncConfig(), SyncConfig()};
	p.da.to_send.assign(6000, 0x42);
	p.db.accept = 0;
	p.start();
	p.run(8 * 3000);
	p.db.accept = 1 << 30;
	p.run(8 * 4000);
	std::vector<int> b = events(p.db.got);
	long rovers = std::count(b.begin(), b.end(), ind(v80::ROVER));
	CHECK(rovers == 1, "%ld <rover>s", rovers);
	auto r = std::find(b.begin(), b.end(), ind(v80::ROVER));
	CHECK(r - b.begin() > 2000 && r - b.begin() < 2100,
	      "<rover> after the ring's octets (%ld)", (long)(r - b.begin()));
}
