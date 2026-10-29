/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    compress_bench.cpp - the link's compressors (V.42bis, V.44 with and
 *                         without V.cvm's preset, zstd with and without
 *                         V.cvm's dictionary) on files, as the link
 *                         runs them: the ratio and the speed, each
 *                         stream decoded again and compared; libzstd's
 *                         as a reference where it is found
 *
 *    compress_bench [--flush N]... [--session N] <file>...
 *        one CSV line per file, compressor and flush interval: the
 *        encoder flushed every N characters (0, the default: only at
 *        the end, as a bulk transfer; the link flushes whenever the DTE
 *        has nothing more, so typing is a flush every few characters);
 *        --session N: each N characters a session of its own, fresh
 *        state (and dictionary) for each, their totals
 *    compress_bench --check
 *        the same on data of its own, a few kB of each kind: exit 1 if
 *        any stream does not come back as it went (ctest's
 *        compress_bench_check)
 *
 *    docs/link.md#compression-benchmark has results.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef DMODEM_HAVE_LIBZSTD
#include <zstd.h>
#endif

#include "v42bis.h"
#include "v44.h"
#include "v44_preset.h"
#include "zstd_codec.h"
#include "zstd_dict.h"

namespace {

using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;

/* the compressors */
enum class Kind { V42bis, V44, V44Preset, Zstd, ZstdDict, Lib3, Lib3Dict,
		  Lib19 };

struct Codec {
	Kind kind;
	const char *name;
};

const Codec codecs[] = {
	{ Kind::V42bis, "v42bis" }, { Kind::V44, "v44" },
	{ Kind::V44Preset, "v44+preset" }, { Kind::Zstd, "zstd" },
	{ Kind::ZstdDict, "zstd+dict" },
#ifdef DMODEM_HAVE_LIBZSTD
	{ Kind::Lib3, "libzstd-3" }, { Kind::Lib3Dict, "libzstd-3+dict" },
	{ Kind::Lib19, "libzstd-19" },
#endif
};

/* one stream: what came out, the seconds each way, and whether it came
   back as it went */
struct Result {
	size_t out = 0;
	double enc_s = 0, dec_s = 0;
	bool ok = true;
};

/* the whole of file p (empty if it cannot be read) */
Bytes slurp(const char *p)
{
	std::ifstream f(p, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), {});
}

/* the seconds since t0 */
double since(Clock::time_point t0)
{
	return std::chrono::duration<double>(Clock::now() - t0).count();
}

/* c's output so far appended to o */
template <class C>
void drain(C &c, Bytes &o)
{
	unsigned char b[4096];
	while (c.has_out()) {
		s16 n = c.out(b, sizeof b);
		if (n <= 0)
			break;
		o.insert(o.end(), b, b + n);
	}
}

/* compressor c of kind k set up as the link does it, both ends alike */
void setup(dlink::V42bis &c, Kind)
{
	c.init(dlink::V42BIS_MAX_CODEWORDS, dlink::V42BIS_MAX_STRING);
}

/* (V.44: XID's largest, V.cvm's preset if asked) */
void setup(dlink::V44 &c, Kind k)
{
	c.init(dlink::V44_MAX_CODEWORDS, dlink::V44_MAX_STRING,
	       dlink::V44_MAX_HISTORY);
	c.config(dlink::V44_MAX_CODEWORDS, dlink::V44_MAX_STRING,
		 dlink::V44_MAX_HISTORY);
	if (k == Kind::V44Preset) {
		const std::vector<u8> &p = dlink::v44_preset();
		c.prime(p.data(), static_cast<s32>(p.size()));
	}
}

/* (zstd: V.cvm's window, its dictionary if asked) */
void setup(dlink::Zstd &c, Kind k)
{
	const std::vector<u8> &d = dlink::zstd_dict();
	if (k == Kind::ZstdDict)
		c.init(dlink::ZSTD_WINDOW_LOG, d.data(),
		       static_cast<s32>(d.size()));
	else
		c.init(dlink::ZSTD_WINDOW_LOG);
}

/* in through a new encoder of kind k, a flush every flush characters
   (0: at the end only), and back through a new decoder */
template <class C>
Result run_ours(const Bytes &in, size_t flush, Kind k)
{
	Result r;
	Bytes cmp, back;
	cmp.reserve(in.size() + 1024);
	auto e = std::make_unique<C>();
	setup(*e, k);
	Clock::time_point t0 = Clock::now();
	for (size_t i = 0; i < in.size(); i++) {
		e->encode(in[i]);
		drain(*e, cmp);
		if (flush && (i + 1) % flush == 0) {
			e->flush_encoder();
			drain(*e, cmp);
		}
	}
	e->flush_encoder();
	drain(*e, cmp);
	r.enc_s = since(t0);
	auto d = std::make_unique<C>();
	setup(*d, k);
	t0 = Clock::now();
	for (unsigned char c : cmp) {
		if (d->decode(c) < 0)
			break;
		drain(*d, back);
	}
	d->flush_decoder();
	drain(*d, back);
	r.dec_s = since(t0);
	r.out = cmp.size();
	r.ok = back == in;
	return r;
}

#ifdef DMODEM_HAVE_LIBZSTD
/* in through libzstd at level, V.cvm's window, a flush (ZSTD_e_flush)
   every flush characters, V.cvm's dictionary if dict; and back */
Result run_lib(const Bytes &in, size_t flush, int level, bool dict)
{
	Result r;
	Bytes cmp;
	const std::vector<u8> &d = dlink::zstd_dict();
	ZSTD_CCtx *c = ZSTD_createCCtx();
	ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
	ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, dlink::ZSTD_WINDOW_LOG);
	if (dict)
		ZSTD_CCtx_loadDictionary(c, d.data(), d.size());
	Bytes ob(ZSTD_CStreamOutSize());
	Clock::time_point t0 = Clock::now();
	size_t step = flush ? flush : in.size();
	for (size_t p = 0; p < in.size() || p == 0; p += step) {
		size_t n = std::min(step, in.size() - p);
		ZSTD_inBuffer ib{ in.data() + p, n, 0 };
		ZSTD_EndDirective how = p + n >= in.size() ? ZSTD_e_end :
			ZSTD_e_flush;
		size_t left;
		do {
			ZSTD_outBuffer o{ ob.data(), ob.size(), 0 };
			left = ZSTD_compressStream2(c, &o, &ib, how);
			cmp.insert(cmp.end(), ob.data(), ob.data() + o.pos);
		} while (left && !ZSTD_isError(left));
		if (!n)
			break;
	}
	r.enc_s = since(t0);
	ZSTD_freeCCtx(c);
	Bytes back(in.size() + 64);
	ZSTD_DCtx *x = ZSTD_createDCtx();
	if (dict)
		ZSTD_DCtx_loadDictionary(x, d.data(), d.size());
	t0 = Clock::now();
	size_t n = ZSTD_decompressDCtx(x, back.data(), back.size(),
				       cmp.data(), cmp.size());
	r.dec_s = since(t0);
	ZSTD_freeDCtx(x);
	back.resize(ZSTD_isError(n) ? 0 : n);
	r.out = cmp.size();
	r.ok = back == in;
	return r;
}
#endif

/* in through compressor k, flushed every flush characters */
Result run(Kind k, const Bytes &in, size_t flush)
{
	switch (k) {
	case Kind::V42bis:
		return run_ours<dlink::V42bis>(in, flush, k);
	case Kind::V44:
	case Kind::V44Preset:
		return run_ours<dlink::V44>(in, flush, k);
	case Kind::Zstd:
	case Kind::ZstdDict:
		return run_ours<dlink::Zstd>(in, flush, k);
#ifdef DMODEM_HAVE_LIBZSTD
	case Kind::Lib3:
		return run_lib(in, flush, 3, false);
	case Kind::Lib3Dict:
		return run_lib(in, flush, 3, true);
	case Kind::Lib19:
		return run_lib(in, flush, 19, false);
#endif
	default:
		return Result{};
	}
}

/* in through compressor k: as one stream, or with session a stream
   every session characters, their totals; the fastest of three
   runs */
Result measure(Kind k, const Bytes &in, size_t flush, size_t session)
{
	Result best;
	for (int rep = 0; rep < 3; rep++) {
		Result sum;
		size_t step = session ? session : in.size();
		for (size_t p = 0; p < in.size(); p += step) {
			Bytes part(in.begin() + p, in.begin() +
				   std::min(in.size(), p + step));
			Result r = run(k, part, flush);
			sum.out += r.out;
			sum.enc_s += r.enc_s;
			sum.dec_s += r.dec_s;
			sum.ok = sum.ok && r.ok;
		}
		if (!rep || sum.enc_s < best.enc_s)
			best.enc_s = sum.enc_s;
		if (!rep || sum.dec_s < best.dec_s)
			best.dec_s = sum.dec_s;
		best.out = sum.out;
		best.ok = best.ok && sum.ok;
	}
	return best;
}

/* MB/s of n characters in s seconds (0 when too quick to tell) */
double mbps(size_t n, double s)
{
	return s > 0 ? n / 1e6 / s : 0;
}

/* name's data, in, through every compressor at every flush interval
   as CSV lines; false if a stream did not come back */
bool bench(const char *name, const Bytes &in,
	   const std::vector<size_t> &flushes, size_t session)
{
	bool ok = true;
	for (size_t f : flushes)
		for (const Codec &c : codecs) {
			Result r = measure(c.kind, in, f, session);
			printf("%s,%zu,%zu,%s,%zu,%zu,%.3f,%.2f,%.2f,%s\n", name,
			       in.size(), session, c.name, f, r.out,
			       r.out ? (double)in.size() / r.out : 0,
			       mbps(in.size(), r.enc_s),
			       mbps(in.size(), r.dec_s),
			       r.ok ? "ok" : "MISMATCH");
			fflush(stdout);
			ok = ok && r.ok;
		}
	return ok;
}

/* --check's data of kind: text, a binary's mix, an ANSI screen, noise,
   n characters, the same every time */
Bytes sample(int kind, size_t n)
{
	static const char *words[] = { "the ", "modem ", "call ", "data ",
		"of ", "link ", "compression ", "dictionary ", "\r\n" };
	unsigned long long x = 0x9E3779B97F4A7C15ull + kind;
	Bytes b;
	while (b.size() < n) {
		x = x * 6364136223846793005ull + 1442695040888963407ull;
		unsigned r = (unsigned)(x >> 33);
		switch (kind) {
		case 0:
			for (const char *p = words[r % 9]; *p; p++)
				b.push_back(static_cast<unsigned char>(*p));
			break;
		case 1:
			b.push_back(r % 4 ? 0 : static_cast<unsigned char>(r >> 8));
			break;
		case 2: {
			char line[64];
			snprintf(line, sizeof line, "\x1b[1;%um[%c] Menu %u\r\n",
				 31 + r % 7, 'A' + r % 26, r % 100);
			b.insert(b.end(), line, line + strlen(line));
			break;
		}
		default:
			b.push_back(static_cast<unsigned char>(r >> 7));
		}
	}
	b.resize(n);
	return b;
}

} // namespace

/* the CSV of the files given (see the top), or --check */
int main(int argc, char *argv[])
{
	std::vector<size_t> flushes;
	size_t session = 0;
	std::vector<const char *> files;
	bool check = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--check"))
			check = true;
		else if (!strcmp(argv[i], "--flush") && i + 1 < argc)
			flushes.push_back(strtoul(argv[++i], nullptr, 10));
		else if (!strcmp(argv[i], "--session") && i + 1 < argc)
			session = strtoul(argv[++i], nullptr, 10);
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: %s [--flush N]... [--session N] "
				"<file>... | --check\n", argv[0]);
			return 2;
		} else
			files.push_back(argv[i]);
	}
	if (flushes.empty())
		flushes.push_back(0);
	printf("file,bytes,session,codec,flush,out,ratio,enc_MBps,dec_MBps,"
	       "ok\n");
	bool ok = true;
	if (check) {
		static const char *names[] = { "text", "binary", "ansi",
			"noise" };
		const size_t fl[] = { 0, 64, 1500 };
		for (int k = 0; k < 4; k++)
			ok = bench(names[k], sample(k, 6000),
				   { fl, fl + 3 }, 0) && ok;
		ok = bench("text-sessions", sample(0, 6000), { 0 }, 512) && ok;
		return ok ? 0 : 1;
	}
	for (const char *f : files) {
		Bytes in = slurp(f);
		const char *base = strrchr(f, '/');
		ok = bench(base ? base + 1 : f, in, flushes, session) && ok;
	}
	return ok ? 0 : 1;
}
