/*
 *
 *    link_test.cpp - tests of the data link layer (link/), without a
 *                    modem: V.42bis round trips, HDLC framing, and two
 *                    Links back to back, bit by bit
 *
 *    link_test            all of them
 *    link_test <name>     one (v42bis, hdlc, async, lapm, lapm_errors,
 *                         lapm_busy, lapm_nocomp, v14)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "link.h"

using namespace dlink;

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
	printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static unsigned rnd_state = 12345;
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

static std::vector<uint8_t> noise(size_t n)
{
	std::vector<uint8_t> v(n);
	for (auto &c : v)
		c = (uint8_t)rnd();
	return v;
}


/*
 *    V.42bis
 *
 */

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

static void test_v42bis()
{
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


/*
 *    HDLC
 *
 */

struct FrameSource : Framer {
	std::vector<std::vector<uint8_t>> tx, rx;
	size_t next = 0;
	Frame f = {};
	uint8_t fbuf[600];
	Frame *next_tx_frame() override {
		if (next >= tx.size())
			return nullptr;
		memcpy(fbuf, tx[next].data(), tx[next].size());
		f.buf = fbuf;
		f.size = (int)tx[next].size();
		next++;
		return &f;
	}
	void tx_complete(Frame *) override {}
	void rx_complete(Frame *fr) override {
		rx.emplace_back(fr->buf, fr->buf + fr->count);
	}
};

struct NullOwner : Owner {
	int read_chars(uint8_t *, int) override { return 0; }
	int write_chars(const uint8_t *, int n) override { return n; }
	void status(Status) override {}
};

static void test_hdlc()
{
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

struct Dte : Owner {
	const char *name;
	std::vector<uint8_t> to_send, got;
	size_t pos = 0;
	bool up = false, released = false, error = false, detected = false;
	int accept = 1 << 30;           /* characters it takes per call */
	Config agreed;
	bool verbose = false;
	explicit Dte(const char *n) : name(n) {}
	int read_chars(uint8_t *buf, int n) override {
		int k = (int)std::min((size_t)n, to_send.size() - pos);
		memcpy(buf, to_send.data() + pos, k);
		pos += k;
		return k;
	}
	int write_chars(const uint8_t *buf, int n) override {
		if (n > accept)
			n = accept;
		got.insert(got.end(), buf, buf + n);
		return n;
	}
	void status(Status s) override {
		switch (s) {
		case Status::Detected: detected = true; break;
		case Status::Up: up = true; break;
		case Status::Released: released = true; break;
		case Status::Error: error = true; break;
		}
	}
	int tx_rate() override { return 9600; }
	void negotiated(const Config &c) override { agreed = c; }
	void log(const char *line) override {
		if (verbose)
			printf("    %s: %s\n", name, line);
	}
};

struct Pair {
	Dte dc{"caller"}, da{"answerer"};
	Link lc, la;
	double ber = 0;
	long errors = 0;

	Pair(const Config &cc, const Config &ca) {
		lc.init(&dc, true, cc);
		la.init(&da, false, ca);
		const char *v = getenv("LINK_DEBUG");
		dc.verbose = da.verbose = v && *v;
	}
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

static Config lapm_config(int comp)
{
	Config c;
	c.ec = true;
	c.detect = true;
	c.comp = comp;
	return c;
}

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

static void test_lapm()
{
	Pair p(lapm_config(3), lapm_config(3));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.detected && p.da.detected, "detection");
	CHECK(p.lc.ec() && p.la.ec(), "LAPM chosen");
	CHECK(p.dc.up && p.da.up, "LAPM up");
	CHECK(p.lc.config().comp == 3 && p.la.config().comp == 3,
	      "V.42bis both ways (%d, %d)", p.lc.config().comp,
	      p.la.config().comp);
	transfer("lapm + v42bis, text", p, 60000, 40000, 9600 * 60);
	/* compression: well above the line rate */
	transfer("lapm + v42bis, noise", p, 5000, 5000, 9600 * 20, false);
	p.lc.disconnect();
	p.run(9600 * 2);
	CHECK(p.da.released, "answerer released after DISC");
	CHECK(p.dc.released, "caller released after UA");
}

static void test_lapm_nocomp()
{
	Pair p(lapm_config(0), lapm_config(3));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	CHECK(p.lc.config().comp == 0 && p.la.config().comp == 0,
	      "no V.42bis when one side has none (%d, %d)",
	      p.lc.config().comp, p.la.config().comp);
	transfer("lapm, no v42bis", p, 20000, 20000, 9600 * 60);
}

static void test_lapm_errors()
{
	Pair p(lapm_config(3), lapm_config(3));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	p.ber = 2e-4;
	transfer("lapm + v42bis, bit errors 2e-4", p, 30000, 30000,
		 9600 * 120);
	CHECK(p.errors > 10, "errors were injected (%ld)", p.errors);
	CHECK(!p.dc.error && !p.da.error, "no link error");
}

static void test_lapm_busy()
{
	Pair p(lapm_config(0), lapm_config(0));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	/* a slow DTE: RNR, the rest later */
	p.da.accept = 7;
	transfer("lapm, slow DTE", p, 20000, 2000, 9600 * 120);
}

static void test_async()
{
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
static void test_v14()
{
	Config c;
	Pair p(c, lapm_config(3));
	p.start(false, true);
	p.run(9600 * 3);
	CHECK(p.da.detected && !p.la.ec(), "answerer: no LAPM");
	CHECK(p.dc.up && p.da.up, "both up (async)");
	transfer("lapm answerer, async caller", p, 3000, 3000, 9600 * 10,
		 false);
}

int main(int argc, char **argv)
{
	struct { const char *name; void (*fn)(); } tests[] = {
		{ "v42bis", test_v42bis },
		{ "hdlc", test_hdlc },
		{ "async", test_async },
		{ "v14", test_v14 },
		{ "lapm", test_lapm },
		{ "lapm_nocomp", test_lapm_nocomp },
		{ "lapm_errors", test_lapm_errors },
		{ "lapm_busy", test_lapm_busy },
	};
	for (auto &t : tests) {
		if (argc > 1 && strcmp(argv[1], t.name))
			continue;
		printf("%s\n", t.name);
		t.fn();
	}
	printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
	return failures ? 1 : 0;
}
