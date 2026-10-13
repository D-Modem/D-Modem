/*
 *
 *    link_test.cpp - tests of the data link layer (link/), without a
 *                    modem: V.42bis round trips, HDLC framing, and two
 *                    Links back to back, bit by bit
 *
 *    link_test                          all of them
 *    link_test --gtest_filter=Link.Hdlc one
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "link.h"
#include "v44.h"

using namespace dlink;

/* a failed check, with a message in printf's format */
static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
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
	int read_chars(uint8_t *, int) override { return 0; }
	int write_chars(const uint8_t *, int n) override { return n; }
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
	explicit Dte(const char *n) : name(n) {}
	/* the rest of to_send */
	int read_chars(uint8_t *buf, int n) override {
		int k = (int)std::min((size_t)n, to_send.size() - pos);
		memcpy(buf, to_send.data() + pos, k);
		pos += k;
		return k;
	}
	/* up to accept characters into got */
	int write_chars(const uint8_t *buf, int n) override {
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
	int tx_rate() override { return 9600; }
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
	   phase */
	void start(bool detect_c, bool detect_a) {
		lc.start(lc.config().ec, detect_c);
		la.start(la.config().ec, detect_a);
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
