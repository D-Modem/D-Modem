/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    link_test.cpp - tests of the data link layer (link/), without a
 *                    modem: V.42bis round trips, HDLC framing, and two
 *                    Links back to back, bit by bit (LAPM, MNP, async)
 *
 *    link_test                          all of them
 *    link_test --gtest_filter=Link.Hdlc one
 *
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "g729a.h"
#include "h245.h"
#include "link.h"
#include "mnp5.h"
#include "v44.h"
#include "v44_preset.h"
#include "zstd_codec.h"
#include "zstd_dict.h"

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
					  int flush_every,
					  const std::vector<uint8_t> *preset =
						  nullptr)
{
	V44 enc, dec;
	std::vector<uint8_t> line, out;
	uint8_t buf[2048];
	int n;

	CHECK(!enc.init(n2, n7, n8) && !dec.init(n2, n7, n8), "init");
	if (preset) {
		s32 a = enc.prime(preset->data(),
				  static_cast<s32>(preset->size()));
		s32 b = dec.prime(preset->data(),
				  static_cast<s32>(preset->size()));
		CHECK(a > 0 && a == b, "%s: primed %d and %d", name, (int)a,
		      (int)b);
	}
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
   a mix), the encoder's transparent mode (7.11.5) and the decoder's */
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
	line = v44_roundtrip("text, flush every one", text(5000), 4096, 255,
			     12288, 1);
	/* FLUSH after each character costs more than the character:
	   transparent mode */
	CHECK(line.size() < 5000 + 16, "flush every one: %zu octets",
	      line.size());
	v44_roundtrip("text, 256 codewords, strings of 32, history 512", t,
		      256, 32, 512, 0);
	v44_roundtrip("text, 1024 codewords, strings of 100", t, 1024, 100,
		      4096, 37);
	line = v44_roundtrip("noise (transparent mode)", z, 4096, 255, 12288, 0);
	/* ETM, and one EID for each escape character (1 in 256) */
	CHECK(line.size() < z.size() * 101 / 100, "noise: %zu octets",
	      line.size());
	CHECK(line.size() > 1 && line[1] != 0, "noise: no ETM");
	v44_roundtrip("repeats (extensions)", rep, 4096, 255, 12288, 0);
	std::vector<uint8_t> mixed = text(30000);
	std::vector<uint8_t> z2 = noise(30000);
	mixed.insert(mixed.end(), z2.begin(), z2.end());
	std::vector<uint8_t> t2 = text(30000);
	mixed.insert(mixed.end(), t2.begin(), t2.end());
	line = v44_roundtrip("text, noise, text", mixed, 4096, 255, 12288, 0);
	/* transparent mode for the noise, then compressed again */
	CHECK(line.size() < 30000 + 2 * 30000 / 2, "text, noise, text: %zu",
	      line.size());
	/* noise, then each escape character value in turn, many times:
	   EID in transparent mode, the escape character 51 more each time */
	std::vector<uint8_t> esc = noise(3000);
	for (int i = 0; i < 3000; i++)
		esc.push_back(static_cast<uint8_t>(i * 51));
	v44_roundtrip("noise, escape characters", esc, 4096, 255, 12288, 0);
	v44_roundtrip("noise, escape characters, flush every 7", esc, 1024,
		      32, 1024, 7);

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

	/* EPM (parameter mode, 7.4.2) is a procedural error: our C0 asks
	   for XID negotiation */
	V44 dec2;
	CHECK(!dec2.init(4096, 255, 12288), "init");
	CHECK(dec2.decode(0x01) >= 0 && dec2.decode(0x00) >= 0 &&
		      dec2.decode(0x02) < 0, "EPM: no error");
	printf("  transparent mode, EPM: an error\n");
}

/* typical short messages of a slow mobile data call */
static std::vector<std::vector<uint8_t>> v44_messages()
{
	static const char *const m[] = {
		"GET /status/today.html HTTP/1.1\r\nHost: www.weather.example.net"
		"\r\nUser-Agent: Mozilla/5.0 (compatible)\r\nAccept: text/html,"
		"application/xhtml+xml\r\nConnection: keep-alive\r\n\r\n",
		"HTTP/1.1 200 OK\r\nDate: Thu, 24 Oct 2026 08:15:02 GMT\r\n"
		"Server: Apache\r\nContent-Type: text/html; charset=UTF-8\r\n"
		"Content-Length: 211\r\n\r\n<!DOCTYPE html>\r\n<html lang=\"en\">"
		"\r\n<head>\r\n<title>Today</title>\r\n</head>\r\n<body>\r\n"
		"<div class=\"report\"><p>Light rain in the morning, then "
		"cloudy.</p></div>\r\n</body>\r\n</html>\r\n",
		"From: <field.unit@example.org>\r\nTo: <office@example.com>\r\n"
		"Subject: Re: the pump report\r\nDate: Thu, 24 Oct 2026 09:00:00 "
		"+0000\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset="
		"\"us-ascii\"\r\n\r\nThank you for your message. The pump at the "
		"north station has been running since yesterday, and the "
		"pressure is normal. Best regards,\r\n",
		"{\"id\": 4711, \"name\": \"station 12\", \"type\": \"meter\", "
		"\"value\": 231, \"status\": \"ok\", \"time\": "
		"\"2026-10-24T09:00:00Z\", \"enabled\": true, \"error\": null}"
		"\r\n",
		"$GPGGA,091502.00,5130.1234,N,00007.5678,W,1,09,0.9,35.2,M,47.0,M,"
		",*5C\r\n$GPRMC,091502.00,A,5130.1234,N,00007.5678,W,0.4,71.2,"
		"241026,,,A*6B\r\n",
		"Dear Sir or Madam,\r\nPlease find the information about your "
		"account and the total of the order below. The service will be "
		"available again tomorrow, after the system update.\r\n",
	};
	std::vector<std::vector<uint8_t>> r;
	for (const char *x : m)
		r.emplace_back(x, x + strlen(x));
	return r;
}

/* V.cvm's preset dictionary (v44_preset.h): round trips primed, with
   REINITs (small parameters, a long text, noise's STEPUP undone), and
   the gain on short messages, each on its own (a call's first) and
   all of them in a row with a flush after each */
TEST(Link, V44Preset)
{
	const std::vector<uint8_t> &p = v44_preset();
	V44 e;
	CHECK(!e.init(4096, 255, 12288), "init");
	CHECK(e.prime(p.data(), static_cast<s32>(p.size())) ==
		      static_cast<s32>(p.size()) && e.primed(),
	      "the whole preset (%zu octets)", p.size());
	CHECK(!e.init(256, 32, 512) &&
		      e.prime(p.data(), static_cast<s32>(p.size())) > 0 &&
		      e.prime(p.data(), static_cast<s32>(p.size())) <
			      static_cast<s32>(p.size()),
	      "part of it with 256 codewords and a history of 512");
	printf("  preset: %zu octets\n", p.size());

	rnd_state = 777;
	std::vector<uint8_t> t = text(100000), z = noise(20000);
	v44_roundtrip("primed: text", t, 4096, 255, 12288, 0, &p);
	v44_roundtrip("primed: text, flush every 50", t, 4096, 255, 12288, 50,
		      &p);
	v44_roundtrip("primed: text, 256 codewords, history 512", t, 256, 32,
		      512, 0, &p);
	v44_roundtrip("primed: text, 1024 codewords, history 2048", t, 1024,
		      100, 2048, 13, &p);
	std::vector<uint8_t> mix = z;
	mix.insert(mix.end(), t.begin(), t.end());
	v44_roundtrip("primed: noise, then text", mix, 4096, 255, 12288, 0, &p);

	size_t plain = 0, primed = 0, in = 0;
	std::vector<uint8_t> all;
	for (auto &m : v44_messages()) {
		size_t a = v44_roundtrip("message", m, 4096, 255, 12288, 0)
				   .size();
		size_t b = v44_roundtrip("message, primed", m, 4096, 255,
					 12288, 0, &p).size();
		in += m.size();
		plain += a;
		primed += b;
		all.insert(all.end(), m.begin(), m.end());
	}
	printf("  the messages one by one: %zu octets, %zu compressed, %zu "
	       "primed\n", in, plain, primed);
	CHECK(primed * 10 < plain * 8, "primed %zu, not primed %zu", primed,
	      plain);
	size_t a = v44_roundtrip("the messages in a row", all, 4096, 255,
				 12288, 0).size();
	size_t b = v44_roundtrip("the messages in a row, primed", all, 4096,
				 255, 12288, 0, &p).size();
	CHECK(b < a, "in a row: primed %zu, not primed %zu", b, a);
}

/*
 *    zstd
 *
 */

/* in through a zstd encoder (a block every flush_every characters, 0:
   at the end only; block: the encoder's largest) and a decoder, both
   from dict (or none): the output must be in; returns the compressed
   octets (the frame's header too) */
static std::vector<uint8_t> zstd_roundtrip(const char *name,
					   const std::vector<uint8_t> &in,
					   int flush_every,
					   const std::vector<uint8_t> *dict =
						   nullptr,
					   int block = ZSTD_BLOCK)
{
	Zstd enc, dec;
	std::vector<uint8_t> line, out;
	uint8_t buf[2048];
	int n;
	const u8 *d = dict ? dict->data() : nullptr;
	s32 dn = dict ? static_cast<s32>(dict->size()) : 0;

	CHECK(!enc.init(ZSTD_WINDOW_LOG, d, dn, block) &&
		      !dec.init(ZSTD_WINDOW_LOG, d, dn), "%s: init", name);
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
	bool bad = false;
	for (uint8_t c : line) {
		if (dec.decode(c) < 0)
			bad = true;
		while ((n = dec.out(buf, sizeof(buf))) > 0)
			out.insert(out.end(), buf, buf + n);
	}
	CHECK(!bad, "%s: decode error", name);
	CHECK(out == in, "%s: %zu in, %zu out", name, in.size(), out.size());
	printf("  %s: %zu -> %zu octets\n", name, in.size(), line.size());
	return line;
}

/* zstd round trips (text, flushes, a large block, noise, repeats, a
   mix, the window slid by far), a whole frame, and the decoder on
   damaged frames (an error, not a crash) */
TEST(Link, Zstd)
{
	rnd_state = 4242;
	std::vector<uint8_t> t = text(200000), z = noise(40000);
	std::vector<uint8_t> rep, line;
	for (int i = 0; i < 30000; i++)
		rep.push_back("abababababababababcabcabcabc"[i % 28]);
	line = zstd_roundtrip("text", t, 0);
	CHECK(line.size() < t.size() / 5, "text: %zu octets", line.size());
	zstd_roundtrip("text, flush every 100", t, 100);
	zstd_roundtrip("text, flush every one", text(3000), 1);
	zstd_roundtrip("text, blocks of 64 kB", t, 0, nullptr, 65536);
	line = zstd_roundtrip("noise", z, 0);
	CHECK(line.size() < z.size() + z.size() / 64, "noise: %zu octets",
	      line.size());
	zstd_roundtrip("repeats", rep, 0);
	std::vector<uint8_t> mixed = text(30000), z2 = noise(30000);
	mixed.insert(mixed.end(), z2.begin(), z2.end());
	std::vector<uint8_t> t2 = text(30000);
	mixed.insert(mixed.end(), t2.begin(), t2.end());
	zstd_roundtrip("text, noise, text", mixed, 37);
	zstd_roundtrip("one character", std::vector<uint8_t>(1, 'x'), 0);
	zstd_roundtrip("one character 5000 times",
		       std::vector<uint8_t>(5000, 'q'), 0);

	/* a whole frame: its last block ends it */
	Zstd enc, dec;
	CHECK(!enc.init() && !dec.init(), "init");
	std::vector<uint8_t> s = text(5000), f, o;
	uint8_t buf[2048];
	int n;
	for (uint8_t c : s)
		enc.encode(c);
	enc.finish();
	while ((n = enc.out(buf, sizeof(buf))) > 0)
		f.insert(f.end(), buf, buf + n);
	CHECK(f.size() > 4 && f[0] == 0x28 && f[1] == 0xB5 && f[2] == 0x2F &&
		      f[3] == 0xFD, "the magic number");
	for (uint8_t c : f) {
		CHECK(dec.decode(c) >= 0, "a whole frame: decode error");
		while ((n = dec.out(buf, sizeof(buf))) > 0)
			o.insert(o.end(), buf, buf + n);
	}
	CHECK(o == s && dec.frame_done(), "a whole frame: %zu of %zu, done %d",
	      o.size(), s.size(), dec.frame_done());

	/* damaged: an octet changed here and there, and noise after the
	   header; the decoder may give garbage, never crash */
	int errors = 0;
	for (int k = 0; k < 200; k++) {
		std::vector<uint8_t> g = f;
		if (k < 100)
			g[6 + rnd() % (g.size() - 6)] ^= 1 << (rnd() % 8);
		else
			for (size_t i = 6; i < g.size(); i++)
				g[i] = (uint8_t)rnd();
		Zstd d2;
		d2.init();
		for (uint8_t c : g) {
			if (d2.decode(c) < 0) {
				errors++;
				break;
			}
			while (d2.out(buf, sizeof(buf)) > 0)
				;
		}
	}
	printf("  damaged frames: %d of 200 found bad\n", errors);
	CHECK(errors >= 100, "damaged frames found bad: %d", errors);
}

/* V.cvm's dictionary (zstd_dict.h): it loads, round trips from it, its
   ID in the frame header, and the gain on short messages, each on its
   own and all of them in a row */
TEST(Link, ZstdDict)
{
	const std::vector<uint8_t> &d = zstd_dict();
	CHECK(d.size() == zstd_dict_header() + v44_preset().size(),
	      "%zu octets", d.size());
	Zstd e;
	CHECK(!e.init(ZSTD_WINDOW_LOG, d.data(), static_cast<s32>(d.size())),
	      "it loads");
	/* a damaged header does not */
	std::vector<uint8_t> bad = d;
	bad[9] = 0xFF;
	CHECK(e.init(ZSTD_WINDOW_LOG, bad.data(),
		     static_cast<s32>(bad.size())) < 0, "a damaged one does not");

	rnd_state = 999;
	std::vector<uint8_t> t = text(100000);
	std::vector<uint8_t> line = zstd_roundtrip("dictionary: text", t, 0, &d);
	CHECK(line.size() > 9 && (line[4] & 3) == 3 &&
		      (line[6] | line[7] << 8 | line[8] << 16 |
		       (uint32_t)line[9] << 24) == ZSTD_DICT_ID,
	      "the dictionary's ID in the frame header");
	zstd_roundtrip("dictionary: text, flush every 13", t, 13, &d);
	zstd_roundtrip("dictionary: noise", noise(20000), 0, &d);

	size_t plain = 0, dict = 0, in = 0;
	std::vector<uint8_t> all;
	for (auto &m : v44_messages()) {
		/* (the frame headers, 6 and 10 octets, not counted) */
		plain += zstd_roundtrip("message", m, 0).size() - 6;
		dict += zstd_roundtrip("message, dictionary", m, 0, &d).size() -
			10;
		in += m.size();
		all.insert(all.end(), m.begin(), m.end());
	}
	printf("  the messages one by one: %zu octets, %zu compressed, %zu "
	       "with the dictionary\n", in, plain, dict);
	CHECK(dict * 10 < plain * 6, "dictionary %zu, none %zu", dict, plain);
	size_t a = zstd_roundtrip("the messages in a row", all, 0).size();
	size_t b = zstd_roundtrip("the messages in a row, dictionary", all, 0,
				  &d).size();
	CHECK(b < a, "in a row: dictionary %zu, none %zu", b, a);
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
	/* with each, whether it had an FCS-32 */
	std::vector<bool> rx32;
	/* a copy into rx */
	void rx_complete(Frame *fr) override {
		rx.emplace_back(fr->buf, fr->buf + fr->count);
		rx32.push_back(fr->fcs32);
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
	int room = -1;                  /* characters it takes in all, if >= 0 */
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
		if (room >= 0 && n > room)
			n = static_cast<s16>(room);
		if (room >= 0)
			room -= n;
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
	/* the rate HST was agreed to start at, 0 none */
	int32_t hst_bps = 0;
	/* HST agreed: its rate kept */
	void hst(int32_t bps) override { hst_bps = bps; }
	/* the characters still to send */
	s32 tx_buffered() override {
		return static_cast<s32>(to_send.size() - pos);
	}
	/* the times HST's link turned the channels around */
	int hst_turns = 0;
	/* (see hst_turns) */
	void hst_turn() override { hst_turns++; }
	/* the link's lines (LINK_DEBUG) */
	void log(const char *line) override {
		if (verbose)
			printf("    %s: %s\n", name, line);
	}
	/* the remote feature upgrade's reports */
	std::vector<UsrReport> usr;
	/* (see usr) */
	void usr_feature(const UsrReport &r) override { usr.push_back(r); }
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
	/* ber's bit errors in b */
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

/* LAPM with the detection phase, compression comp (V.42bis's P0), V.44,
   zstd (from V.cvm's dictionary, or none) */
static Config lapm_config(int comp, bool v44 = true, bool zstd = false,
			  bool dict = true)
{
	Config c;
	c.ec = true;
	c.detect = true;
	c.comp = comp;
	c.v44 = v44;
	c.zstd = zstd;
	c.zstd_dict = dict ? ZSTD_DICT_ID : 0;
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

/* +DS's and +DS44's directions (Config::comp42, comp44): V.44 alone,
   V.42bis alone, none in common, V.42bis one way only; the data both
   ways each time */
TEST(Link, LapmDirections)
{
	struct Case {
		const char *name;
		s16 c42, c44, a42, a44;
		bool v44;
		s16 comp_c, comp_a;     /* each side's agreed directions */
	} cases[] = {
		{ "V.44 alone (caller +DS=0)", 0, 3, 3, 3, true, 3, 3 },
		{ "V.42bis alone (both +DS44=0)", 3, 0, 3, 0, false, 3, 3 },
		{ "none in common", 3, 0, 0, 3, false, 0, 0 },
		{ "V.42bis from the caller only", 2, 0, 3, 0, false, 2, 1 },
	};
	for (const Case &k : cases) {
		rnd_state = 4242;
		Config cc = lapm_config(3), ca = lapm_config(3);
		cc.comp42 = k.c42;
		cc.comp44 = k.c44;
		cc.v44 = k.c44 != 0;
		cc.comp = k.c42 | k.c44;
		ca.comp42 = k.a42;
		ca.comp44 = k.a44;
		ca.v44 = k.a44 != 0;
		ca.comp = k.a42 | k.a44;
		Pair p(cc, ca);
		p.start(false, false);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "%s: LAPM up", k.name);
		CHECK(p.lc.config().comp == k.comp_c &&
			      p.la.config().comp == k.comp_a,
		      "%s: directions %d, %d (want %d, %d)", k.name,
		      p.lc.config().comp, p.la.config().comp, k.comp_c,
		      k.comp_a);
		CHECK(!k.comp_c || (p.lc.config().v44 == k.v44 &&
				    p.la.config().v44 == k.v44),
		      "%s: V.44 %d, %d", k.name, p.lc.config().v44,
		      p.la.config().v44);
		transfer(k.name, p, 20000, 20000, 9600 * 40);
	}
}

/* zstd offered in XID by both: it rather than V.44, with the
   dictionary where both name it, text and noise both ways, with the
   detection phase or without; offered by one only: V.44; V.cvm's
   set_zstd() takes the offer or the dictionary away */
TEST(Link, LapmZstd)
{
	for (int k = 0; k < 4; k++) {
		bool dc = k & 1, da = k & 2;
		rnd_state = 2026 + k;
		Pair p(lapm_config(3, true, true, dc),
		       lapm_config(3, true, true, da));
		p.start(k == 1, k == 1);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "LAPM up");
		bool dict = dc && da;
		CHECK(p.lc.config().zstd && p.la.config().zstd &&
			      !p.lc.config().v44 && p.lc.config().comp == 3 &&
			      p.la.config().comp == 3 && p.dc.agreed.zstd &&
			      p.da.agreed.zstd,
		      "zstd both ways (%d, %d)", p.lc.config().zstd,
		      p.la.config().zstd);
		CHECK(p.lc.config().zstd_dict == (dict ? ZSTD_DICT_ID : 0) &&
			      p.la.config().zstd_dict == p.lc.config().zstd_dict,
		      "dictionary %08x, %08x", (unsigned)p.lc.config().zstd_dict,
		      (unsigned)p.la.config().zstd_dict);
		std::string n = std::string("lapm + zstd") +
			(dict ? " + dictionary" : "") + (k == 1 ? " (detection)" : "");
		transfer((n + ", text").c_str(), p, 60000, 40000, 9600 * 60);
		transfer((n + ", noise").c_str(), p, 5000, 5000, 9600 * 20,
			 false);
	}
	for (int side = 0; side < 2; side++) {
		rnd_state = 77 + side;
		Pair p(lapm_config(3, true, side == 0),
		       lapm_config(3, true, side == 1));
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(!p.lc.config().zstd && !p.la.config().zstd &&
			      p.lc.config().v44 && p.la.config().v44,
		      "zstd offered by the %s only: V.44",
		      side ? "answerer" : "caller");
		transfer(side ? "lapm + v44 (zstd: answerer only), text" :
			 "lapm + v44 (zstd: caller only), text", p, 20000,
			 20000, 9600 * 30);
	}
	/* V.cvm: the INFO0s without zstd, then without the preset */
	{
		Pair p(lapm_config(3, true, true), lapm_config(3, true, true));
		p.lc.set_zstd(false, true);
		p.la.set_zstd(false, true);
		p.start(false, false);
		p.run(9600 * 3);
		CHECK(!p.lc.config().zstd && p.lc.config().v44,
		      "set_zstd(false): V.44");
	}
	{
		Pair p(lapm_config(3, true, true), lapm_config(3, true, true));
		p.lc.set_zstd(true, false);
		p.la.set_zstd(true, false);
		p.start(false, false);
		p.run(9600 * 3);
		CHECK(p.lc.config().zstd && !p.lc.config().zstd_dict &&
			      !p.la.config().zstd_dict,
		      "set_zstd(true, false): zstd without the dictionary");
		transfer("lapm + zstd (V.cvm, no preset), text", p, 20000, 20000,
			 9600 * 30);
	}
	/* zstd without V.44 offered, and with one direction only */
	{
		Pair p(lapm_config(1, false, true), lapm_config(3, false, true));
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.lc.config().zstd && p.la.config().zstd &&
			      p.lc.config().comp == p.la.config().comp >> 1 &&
			      p.lc.config().comp != 3,
		      "one direction: comp %d, %d", p.lc.config().comp,
		      p.la.config().comp);
		transfer("lapm + zstd one way, text", p, 20000, 20000,
			 9600 * 30);
	}
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
	inj.set_tx_fcs32(true);         /* what the link has agreed */
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

/* an RR from the answerer (as a frame of noise that passes the FCS on a
   noisy line) with an N(R) past the caller's V(S), while its I frames,
   lost on the line, are outstanding: ignored, not taken to acknowledge
   them, so T401 sends them again and all the data arrives */
TEST(Link, LapmNrPastVs)
{
	rnd_state = 12345;
	Pair p(lapm_config(0), lapm_config(0));
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	p.dc.to_send = text(6000);
	p.dc.pos = 0;
	/* the caller's I frames lost on the line, the answerer's flags
	   through */
	std::vector<uint8_t> b(48);
	for (int k = 0; k < 4800; k += 48) {
		p.lc.get_bits(1, b.data(), 48);
		p.la.get_bits(1, b.data(), 48);
		p.lc.put_bits(1, b.data(), 48);
	}
	NullOwner o;
	Link l;
	l.init(&o, true, Config());
	Hdlc inj;
	FrameSource src;
	inj.start(&l);
	inj.set_framer(&src, true);
	inj.set_tx_fcs32(true);         /* what the link has agreed */
	src.tx.push_back({ 0x03, 0x01, 100 << 1 });    /* RR, N(R) 100 */
	uint8_t bits[600];
	for (int k = 0; k < 2; k++) {
		inj.get_bits(1, bits, sizeof(bits));
		p.lc.put_bits(1, bits, sizeof(bits));
	}
	for (long run = 0; run < 9600L * 30 && p.da.got.size() < 6000;
	     run += 9600)
		p.run(9600);
	CHECK(p.da.got == p.dc.to_send, "caller -> answerer %zu/6000",
	      p.da.got.size());
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

/* LAPM with the Hayes sets (S46 s46) and V.42bis offered or not: no
   V.44 */
static Config hayes_config(u8 s46, bool v42bis)
{
	Config c = lapm_config(3, false);
	c.hayes = s46;
	c.comp42 = v42bis ? 3 : 0;
	return c;
}

/* two Links with the Hayes sets (S46 138 or 136) back to back: ADC both
   ways when V.42bis is not offered, V.42bis when it is, no compression
   without the flag (also with V.42bis); without S46 at either side,
   the usual V.42 rules */
TEST(Link, LapmHayesAdc)
{
	struct Case {
		u8 c46, a46;
		bool v42bis;
		bool adc;
		s16 comp;
	};
	const Case cases[] = {
		{ 138, 138, false, true, 3 },
		{ 138, 138, true, false, 3 },
		{ 138, 136, true, false, 0 },
		{ 136, 136, false, false, 0 },
		{ 0, 138, true, false, 3 },
		{ 0, 0, false, false, 0 },
	};
	for (const Case &k : cases) {
		rnd_state = 12345;
		Pair p(hayes_config(k.c46, k.v42bis),
		       hayes_config(k.a46, k.v42bis));
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "LAPM up (S46 %d/%d)", k.c46,
		      k.a46);
		CHECK(p.lc.config().adc == k.adc && p.la.config().adc == k.adc,
		      "ADC %d/%d, wanted %d (S46 %d/%d)", p.lc.config().adc,
		      p.la.config().adc, k.adc, k.c46, k.a46);
		CHECK(p.lc.config().comp == k.comp &&
		      p.la.config().comp == k.comp,
		      "compression %d/%d, wanted %d (S46 %d/%d)",
		      p.lc.config().comp, p.la.config().comp, k.comp, k.c46,
		      k.a46);
		const std::string name = std::string("lapm, S46 ") +
			std::to_string(k.c46) + "/" + std::to_string(k.a46) +
			(k.adc ? " (adc)" : k.comp ? " (v42bis)" : "");
		transfer(name.c_str(), p, 30000, 20000, 9600 * 60);
		if (k.adc)
			transfer("lapm + adc, noise", p, 5000, 5000, 9600 * 20,
				 false);
	}
}

/* a Hayes modem's XID command (FA 6.9, P0 0: no V.42bis) to an answerer
   with S46 138: its response has the Hayes sets, and the call has ADC;
   to an answerer without S46: no Hayes sets, no ADC.  A caller with S46
   sends the sets in its command */
TEST(Link, LapmHayesPeer)
{
	u8 info[HAYES_XID_LEN];
	const s16 n = hayes_xid_template(info, 7, 0x00, HAYES_S46_LAPM_COMP,
					 0, 0);
	std::vector<uint8_t> cmd = { 0x03, 0xaf };
	cmd.insert(cmd.end(), info, info + n);
	const std::vector<uint8_t> sets = { 0xff, 0x03, 0x48, 0x4d, 0x50,
		0x01, 0x01, 0x8a, 0x02, 0x01, 0x01 };
	auto has = [](const std::vector<uint8_t> &f,
		      const std::vector<uint8_t> &part) {
		return std::search(f.begin(), f.end(), part.begin(),
				   part.end()) != f.end();
	};
	for (int on = 0; on < 2; on++) {
		Dte d("answerer");
		Link l;
		l.init(&d, false, hayes_config(on ? 138 : 0, true));
		l.start(true, false);
		std::vector<std::vector<uint8_t>> out =
			exchange(l, { cmd }, 4);
		CHECK(out.size() == 1 && has(out[0], sets) == (on == 1),
		      "the answerer's XID response, S46 %d", on ? 138 : 0);
		CHECK(l.config().adc == (on == 1), "ADC %d (S46 %d)",
		      l.config().adc, on ? 138 : 0);
	}
	Dte d("caller");
	Link l;
	l.init(&d, true, hayes_config(138, true));
	l.start(true, false);
	std::vector<std::vector<uint8_t>> out = exchange(l, {}, 4);
	CHECK(out.size() == 1 && has(out[0], sets),
	      "the caller's XID has the Hayes sets");
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

/* a Courier's side of the USRobotics remote feature upgrade
   (usr_feature.h), as its firmware has it: UI commands of type 0x0a
   answered (1 the records, 3 the serial number), a set or clear kept
   only when allowed (S30 bit 0 in the builds after 7.1.4) */
struct CourierFeatures {
	uint8_t recs[USR_RECORDS] = { 0, 15, 0, 0, 0, 0 };  /* record 1.. */
	bool allowed = true;
	bool answers = true;
	std::vector<std::vector<uint8_t>> heard;
	/* a frame from the host: the answers to send */
	std::vector<std::vector<uint8_t>> rx(const std::vector<uint8_t> &f) {
		std::vector<std::vector<uint8_t>> out;
		if (f.size() < 4 || f[1] != 0x03 || f[2] != USR_UI_TYPE)
			return out;
		heard.push_back(f);
		const uint8_t m = f[3];
		if (m == 1 && answers) {
			std::vector<uint8_t> a = { 0x03, 0x03, USR_UI_TYPE, 0 };
			for (int i = USR_RECORDS; i >= 1; i--)
				a.push_back(recs[i - 1]);
			out.push_back(a);
		} else if (m == 3 && answers) {
			const char *sn = "7309550013402381";
			std::vector<uint8_t> a = { 0x03, 0x03, USR_UI_TYPE, 4,
						   (uint8_t)strlen(sn) };
			a.insert(a.end(), sn, sn + strlen(sn));
			out.push_back(a);
		} else if ((m == 2 || m == 5) && f.size() >= 5 && allowed) {
			const int rec = f[4] >> 4, bit = f[4] & 15;
			if (rec >= 1 && rec <= USR_RECORDS && bit >= 1) {
				const uint8_t mask = (uint8_t)(1 << (bit - 1));
				if (m == 2)
					recs[rec - 1] |= mask;
				else
					recs[rec - 1] &= (uint8_t)~mask;
			}
		}
		return out;
	}
};

/* a call to the Courier cf with the host's side in mode once LAPM is
   up: its one report.  With then, the program's ask after that report
   (Link::usr_start(), as dmodem_x2up()): the second report */
static UsrReport x2up_call(CourierFeatures &cf, UsrMode mode,
			   uint8_t enable = 16, uint8_t disable = 0,
			   UsrMode then = UsrMode::Off)
{
	rnd_state = 12345;
	/* (the 16-bit FCS: the Courier's frames below have it) */
	Config cc = lapm_config(0), ca = lapm_config(0);
	cc.fcs32 = ca.fcs32 = false;
	ca.usr_feature = mode;
	ca.usr_enable = enable;
	ca.usr_disable = disable;
	Pair p(cc, ca);
	p.start(true, true);
	/* until LAPM is up, less than a second before the first ask */
	for (int k = 0; k < 60 && !(p.dc.up && p.da.up); k++)
		p.run(480);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	/* the Courier from here: frames to the answerer, its frames read */
	NullOwner o;
	Link side;
	side.init(&o, true, Config());
	Hdlc tx, rx;
	FrameSource src, dst;
	tx.start(&side);
	rx.start(&side);
	tx.set_framer(&src, true);
	rx.set_framer(&dst, false);
	uint8_t bits[96];
	size_t seen = 0, want = then == UsrMode::Off ? 1 : 2;
	bool asked = false;
	for (int k = 0; k < 9600 * 60 / 96 && p.da.usr.size() < want; k++) {
		tx.get_bits(1, bits, sizeof(bits));
		p.la.put_bits(1, bits, sizeof(bits));
		p.la.get_bits(1, bits, sizeof(bits));
		rx.put_bits(1, bits, sizeof(bits));
		for (; seen < dst.rx.size(); seen++)
			for (auto &a : cf.rx(dst.rx[seen]))
				src.tx.push_back(a);
		if (want == 2 && k == 0) {
			CHECK(!p.la.usr_start(then, enable, disable),
			      "a second exchange while one goes on");
		}
		if (want == 2 && p.da.usr.size() == 1 && !asked)
			asked = p.la.usr_start(then, enable, disable);
	}
	CHECK(p.da.usr.size() == want && asked == (want == 2),
	      "%zu reports (%zu)", want, p.da.usr.size());
	return p.da.usr.size() < want ? UsrReport() : p.da.usr[want - 1];
}

/* the host's side (+X2UP=2) against a Courier that lets the set
   through, the same again (x2 already), one that does not, and a
   remote that never answers; then +X2UP=1, which only reads */
TEST(Link, UsrFeature)
{
	CourierFeatures cf;
	UsrReport r = x2up_call(cf, UsrMode::Change);
	CHECK(r.result == UsrResult::Enabled, "enabled (%s)",
	      usr_result_name(r.result));
	CHECK(!strcmp(r.serial, "7309550013402381"), "serial %s", r.serial);
	CHECK(r.has_records && r.before[1] == 15 && r.after[1] == 31,
	      "features %u -> %u", r.before[1], r.after[1]);
	CHECK(cf.recs[1] == 31, "the Courier's record 2 is %u", cf.recs[1]);
	/* the set: 0a 02 25 in a UI command of the answerer's (address 1) */
	const std::vector<uint8_t> set = { 0x01, 0x03, 0x0a, 0x02, 0x25 };
	CHECK(std::find(cf.heard.begin(), cf.heard.end(), set) !=
	      cf.heard.end(), "the set frame 01 03 0a 02 25");

	r = x2up_call(cf, UsrMode::Change);
	CHECK(r.result == UsrResult::Already, "already (%s)",
	      usr_result_name(r.result));

	CourierFeatures locked;
	locked.allowed = false;
	r = x2up_call(locked, UsrMode::Change);
	CHECK(r.result == UsrResult::Refused, "refused (%s)",
	      usr_result_name(r.result));
	CHECK(r.after[1] == 15, "features after %u", r.after[1]);

	CourierFeatures mute;
	mute.answers = false;
	r = x2up_call(mute, UsrMode::Change);
	CHECK(r.result == UsrResult::NoReply && !r.has_records,
	      "no reply (%s)", usr_result_name(r.result));
	CHECK(mute.heard.size() == 6, "3 asks for each (%zu)",
	      mute.heard.size());

	CourierFeatures ro;
	r = x2up_call(ro, UsrMode::Query);
	CHECK(r.result == UsrResult::Queried && ro.recs[1] == 15,
	      "query only (%s, %u)", usr_result_name(r.result), ro.recs[1]);
	printf("  x2up: enabled, already, refused, no reply, queried\n");
}

/* the switches (+X2UP=2,<enable>,<disable>): Terbo and V.FC off (two
   clears, 0a 05 23 and 0a 05 24), then Terbo on and x2 off together,
   a disable already done, and the HST and fax bits passed over */
TEST(Link, UsrFeatureSwitches)
{
	CourierFeatures cf;
	cf.recs[1] = 31;
	UsrReport r = x2up_call(cf, UsrMode::Change, 0, 4 | 8);
	CHECK(r.result == UsrResult::Disabled, "disabled (%s)",
	      usr_result_name(r.result));
	CHECK(r.before[1] == 31 && r.after[1] == 19 && cf.recs[1] == 19,
	      "features %u -> %u (%u)", r.before[1], r.after[1], cf.recs[1]);
	const std::vector<uint8_t> c3 = { 0x01, 0x03, 0x0a, 0x05, 0x23 },
		c4 = { 0x01, 0x03, 0x0a, 0x05, 0x24 };
	CHECK(std::find(cf.heard.begin(), cf.heard.end(), c3) !=
	      cf.heard.end() && std::find(cf.heard.begin(), cf.heard.end(),
	      c4) != cf.heard.end(), "the clears 0a 05 23 and 0a 05 24");

	r = x2up_call(cf, UsrMode::Change, 4, 16);
	CHECK(r.result == UsrResult::Changed, "changed (%s)",
	      usr_result_name(r.result));
	CHECK(r.before[1] == 19 && r.after[1] == 7 && cf.recs[1] == 7,
	      "features %u -> %u", r.before[1], r.after[1]);

	r = x2up_call(cf, UsrMode::Change, 0, 16);
	CHECK(r.result == UsrResult::Already, "already off (%s)",
	      usr_result_name(r.result));

	/* HST (1) and fax (2) are not switched: no frame for them */
	CourierFeatures hf;
	hf.recs[1] = 3;
	r = x2up_call(hf, UsrMode::Change, 0, 1 | 2);
	CHECK(r.result == UsrResult::Already && hf.recs[1] == 3,
	      "HST and fax left (%s, %u)", usr_result_name(r.result),
	      hf.recs[1]);

	CourierFeatures locked;
	locked.allowed = false;
	locked.recs[1] = 31;
	r = x2up_call(locked, UsrMode::Change, 0, 16);
	CHECK(r.result == UsrResult::Refused && r.after[1] == 31,
	      "refused (%s, %u)", usr_result_name(r.result), r.after[1]);
	printf("  x2up: disabled, changed, already, HST and fax, refused\n");
}

/* the program's ask in the call (dmodem_x2up(), the caller's choice on
   the host's screen): the call reads the features at the start
   (+X2UP=1), then the change comes on Link::usr_start(); none with the
   mode off */
TEST(Link, UsrFeatureLater)
{
	CourierFeatures cf;
	cf.recs[1] = 15;
	UsrReport r = x2up_call(cf, UsrMode::Query, 16, 8, UsrMode::Change);
	CHECK(r.result == UsrResult::Changed, "changed (%s)",
	      usr_result_name(r.result));
	CHECK(r.before[1] == 15 && r.after[1] == 23 && cf.recs[1] == 23,
	      "features %u -> %u (%u)", r.before[1], r.after[1], cf.recs[1]);

	Link idle;
	NullOwner o;
	idle.init(&o, true, lapm_config(0));
	CHECK(!idle.usr_start(UsrMode::Change, 16, 0), "no LAPM up");
	printf("  x2up: read at the start, changed later\n");
}

/* USRobotics HST in XID as a Courier sends it: the XID command and
   response of a recorded Courier call (docs/dsp/hst.md), whose user data
   subfield has "HST", its capability (options 07, rates fc) and f1 06.
   An answerer offering HST answers with its own and goes to HST on the
   caller's DM; a caller answered with HST sends that DM, with an
   FCS-16, and no SABME: once HST is up its own link starts, with an
   RR of nothing (05 00 08); without HST the same XID is LAPM's as
   before */
TEST(Link, LapmHst)
{
	const std::vector<uint8_t> courier_cmd = { 0x03, 0xaf, 0x82, 0x80,
		0x00, 0x13, 0x03, 0x03, 0x8e, 0x89, 0x00, 0x05, 0x02, 0x07,
		0xa0, 0x06, 0x02, 0x07, 0xa0, 0x07, 0x01, 0x0f, 0x08, 0x01,
		0x0f, 0xf0, 0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32, 0x01,
		0x01, 0x03, 0x02, 0x02, 0x08, 0x00, 0x03, 0x01, 0x20, 0xff,
		0xff, 0x03, 0x48, 0x53, 0x54, 0xc0, 0x02, 0x07, 0xfc, 0xf1,
		0x01, 0x06 };
	std::vector<uint8_t> courier_rsp = courier_cmd;
	courier_rsp[13] = courier_rsp[17] = 0x04;     /* N401 1024 bits */
	courier_rsp[14] = courier_rsp[18] = 0x00;
	const std::vector<uint8_t> dm = { 0x01, 0x0f };
	const std::vector<uint8_t> hst = { 0xff, 0xff, 0x03, 0x48, 0x53, 0x54,
		0xc0, 0x02, 0x01, 0x1c, 0xf1, 0x01, 0x06 };
	auto has = [](const std::vector<uint8_t> &f,
		      const std::vector<uint8_t> &part) {
		return std::search(f.begin(), f.end(), part.begin(),
				   part.end()) != f.end();
	};
	auto is_sabme = [](const std::vector<uint8_t> &f) {
		return f.size() == 2 && (f[1] & 0xef) == 0x6f;
	};
	const std::vector<uint8_t> rr0 = { 0x05, 0x00, 0x08 };

	/* the answerer: its response with HST, 9600 to start; the DM */
	for (int on = 0; on < 2; on++) {
		Dte d("answerer");
		Link l;
		Config c = lapm_config(3, false);
		c.hst = on ? 0x1c : 0;          /* 4800, 7200, 9600 */
		l.init(&d, false, c);
		l.start(true, false);
		std::vector<std::vector<uint8_t>> out =
			exchange(l, { courier_cmd }, 4);
		CHECK(out.size() == 1 && has(out[0], hst) == (on == 1),
		      "the answerer's XID response, HST %d", on);
		CHECK(l.hst_rate() == (on ? 9600 : 0), "HST at %d",
		      l.hst_rate());
		out = exchange(l, { dm }, 4);
		CHECK(d.hst_bps == (on ? 9600 : 0), "to HST at %d",
		      d.hst_bps);
		CHECK(std::any_of(out.begin(), out.end(), is_sabme) ==
		      (on == 0), "an SABME after the DM (HST %d)", on);
	}

	/* the caller: its XID with HST; the response: a DM (FCS-16) and
	   HST; then, started again, HST's RR first */
	for (int on = 0; on < 2; on++) {
		Dte d("caller");
		Link l;
		Config c = lapm_config(3, false);
		c.hst = on ? 0x1c : 0;
		l.init(&d, true, c);
		l.start(true, false);
		std::vector<std::vector<uint8_t>> out = exchange(l, {}, 4);
		CHECK(out.size() == 1 && has(out[0], hst) == (on == 1),
		      "the caller's XID, HST %d", on);
		out = exchange(l, { courier_rsp }, 4);
		CHECK(!out.empty() && (on ? out[0] == dm : is_sabme(out[0])),
		      "after the response (HST %d): %zu frames", on,
		      out.size());
		CHECK(!l.hdlc().tx_fcs32(), "FCS-16 out");
		CHECK(d.hst_bps == (on ? 9600 : 0), "to HST at %d",
		      d.hst_bps);
		if (!on)
			continue;
		l.start(true, true);
		out = exchange(l, {}, 4);
		CHECK(!out.empty() && out[0] == rr0,
		      "HST's RR first once HST is up (%zu frames)",
		      out.size());
		CHECK(l.protocol() == Protocol::Hst, "HST's link");
	}

	/* two Links: 9600 when both offer it, else the lowest both have */
	const u8 offers[][3] = { { 0xfc, 0xfc, 9 }, { 0x0c, 0xfc, 7 },
				 { 0xe0, 0xfc, 12 }, { 0xfc, 0, 0 } };
	for (const auto &o : offers) {
		rnd_state = 12345;
		Config cc = lapm_config(0), ca = lapm_config(0);
		cc.hst = o[0];
		ca.hst = o[1];
		Pair p(cc, ca);
		p.start(true, true);
		p.run(9600 * 3);
		const int32_t want = o[2] == 9 ? 9600 : o[2] == 7 ? 7200 :
			o[2] == 12 ? 12000 : 0;
		CHECK(p.dc.hst_bps == want && p.da.hst_bps == want,
		      "HST %02x/%02x: %d and %d", o[0], o[1], p.dc.hst_bps,
		      p.da.hst_bps);
		CHECK((p.dc.up && p.da.up) == (want == 0),
		      "LAPM up at this rate only without HST (%02x/%02x)",
		      o[0], o[1]);
	}
}

/* HST's own link after the switch (hst_link.h), as two emulated
   Couriers have it: a Courier's first frames to a caller (an RR of
   nothing, a short frame of one octet, data, data with an
   acknowledgement) delivered in order, acknowledged with RRs; then two
   Links back to back, with and without bit errors: the data both ways,
   the caller (on the back channel, with data) asking to turn the
   channels around and the answerer granting it.  An answerer whose
   grant is not heard (nothing comes after it) turns the channels
   back */
TEST(Link, Hst)
{
	{
		Dte d("caller");
		Link l;
		Config c = lapm_config(0);
		c.hst = 0x1c;
		l.init(&d, true, c);
		l.hst_switch();
		l.start(true, true);
		l.set_online(true);
		const std::vector<std::vector<uint8_t>> in = {
			{ 0x05, 0x00, 0x08 },
			{ 0x19, 'a' },
			{ 0x04, 0x02, 'b', 'c' },
			{ 0x04, 0x02, 'b', 'c' },          /* again */
			{ 0x08, 0x03, 0x00, 0x08, 'd' },
			{ 0x4a, 'e', 'f' },          /* short, 4 */
		};
		std::vector<std::vector<uint8_t>> out = exchange(l, in, 6);
		CHECK(d.up, "up on the remote's first frame");
		CHECK(std::string(d.got.begin(), d.got.end()) == "abcdef",
		      "delivered: %s",
		      std::string(d.got.begin(), d.got.end()).c_str());
		const std::vector<uint8_t> rr4 = { 0x05, 0x04, 0x08 };
		CHECK(std::find(out.begin(), out.end(), rr4) != out.end(),
		      "acknowledged up to 4 (%zu frames)", out.size());
	}
	{
		Dte d("answerer");
		Link l;
		Config c = lapm_config(0);
		c.hst = 0x1c;
		l.init(&d, false, c);
		l.hst_switch();
		l.start(true, true);
		l.set_online(true);
		std::vector<std::vector<uint8_t>> out = exchange(l,
			{ { 0x05, 0x00, 0x08 }, { 0x06, 0x01, 0x04 } }, 4);
		const std::vector<uint8_t> grant = { 0x07, 0x01 };
		CHECK(std::find(out.begin(), out.end(), grant) != out.end(),
		      "the ask granted (%zu frames)", out.size());
		CHECK(d.hst_turns >= 1, "turned %d times", d.hst_turns);
		exchange(l, {}, 8);
		CHECK(d.hst_turns == 2, "turned back: %d turns", d.hst_turns);
	}
	for (double ber : { 0.0, 1e-4 }) {
		rnd_state = 12345;
		Config cc = lapm_config(0), ca = lapm_config(0);
		cc.hst = ca.hst = 0x1c;
		Pair p(cc, ca);
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.hst_bps == 9600 && p.da.hst_bps == 9600,
		      "HST agreed");
		p.ber = ber;
		p.start(true, true);
		p.run(9600);
		CHECK(p.dc.up && p.da.up, "HST's link up");
		CHECK(p.lc.protocol() == Protocol::Hst &&
		      p.la.protocol() == Protocol::Hst, "HST's link");
		transfer(ber ? "hst, bit errors" : "hst", p, 6000, 20000,
			 9600 * 120);
		CHECK(p.dc.hst_turns > 0 && p.da.hst_turns == p.dc.hst_turns,
		      "turned around %d and %d times", p.dc.hst_turns,
		      p.da.hst_turns);
	}
}

/* the FCS-32 (8.1.1.6.2/V.42): CRC-32's check value over "123456789",
   the register after a whole good frame; Hdlc's frames with it, past
   zero insertion (octets of 1s), to receivers that take FCS-32, FCS-16
   or both; a bit error in one */
TEST(Link, HdlcFcs32)
{
	u32 r = FCS32_INIT;
	for (const char *c = "123456789"; *c; c++)
		r = fcs32_add(r, static_cast<u8>(*c));
	CHECK(~r == 0xcbf43926u, "check value %08x", ~r);
	const u32 fcs = ~r;
	for (int i = 0; i < 4; i++)
		r = fcs32_add(r, static_cast<u8>(fcs >> 8 * i));
	CHECK(r == FCS32_GOOD, "good frame's register %08x", r);

	const std::vector<std::vector<uint8_t>> frames = {
		{ 0x03, 0x7f },
		std::vector<uint8_t>(130, 0xff),
		{ 0x01, 0x00, 0x7e, 0x7e, 0x55 },
	};
	for (int tx32 = 0; tx32 < 2; tx32++) {
		for (u8 acc = 1; acc <= 3; acc++) {
			NullOwner o;
			Link side;
			side.init(&o, true, Config());
			Hdlc tx, rx;
			FrameSource src, dst;
			tx.start(&side);
			rx.start(&side);
			tx.set_framer(&src, true);
			rx.set_framer(&dst, false);
			tx.set_tx_fcs32(tx32);
			rx.set_rx_fcs(acc);
			src.tx = frames;
			uint8_t bits[600];
			for (int k = 0; k < 8; k++) {
				tx.get_bits(1, bits, sizeof(bits));
				rx.put_bits(1, bits, sizeof(bits));
			}
			const bool taken = acc & (tx32 ? RX_FCS32 : RX_FCS16);
			CHECK(dst.rx == (taken ? frames :
			      std::vector<std::vector<uint8_t>>()),
			      "FCS-%d to a receiver of %d: %zu frames",
			      tx32 ? 32 : 16, acc, dst.rx.size());
			for (bool b : dst.rx32)
				CHECK(b == (tx32 == 1), "Frame::fcs32 %d", b);
		}
	}

	/* one bit of the FCS-32's frame flipped: dropped, counted */
	NullOwner o;
	Link side;
	side.init(&o, true, Config());
	Hdlc tx, rx;
	FrameSource src, dst;
	tx.start(&side);
	rx.start(&side);
	tx.set_framer(&src, true);
	rx.set_framer(&dst, false);
	tx.set_tx_fcs32(true);
	rx.set_rx_fcs(RX_FCS16 | RX_FCS32);
	src.tx = { { 0x03, 0x01, 0x10, 0x20, 0x30, 0x40 } };
	uint8_t bits[600];
	tx.get_bits(1, bits, sizeof(bits));
	bits[8 + 20] ^= 1;
	rx.put_bits(1, bits, sizeof(bits));
	CHECK(dst.rx.empty(), "a frame with a bit error taken");
	CHECK(side.stats().rx_errors == 1, "%u FCS errors",
	      side.stats().rx_errors);
}

/* the HDLC optional functions of an XID frame (its first parameter,
   PL 3), or none */
static std::vector<uint8_t> xid_opt(const std::vector<uint8_t> &f)
{
	if (f.size() < 11 || (f[1] & 0xef) != 0xaf || f[6] != 3 || f[7] != 3)
		return {};
	return { f[8], f[9], f[10] };
}

/* the 32-bit FCS (10 c, 8.10.2/V.42): asked for in the caller's XID
   (bit position 17) unless it has Config::fcs32 off; an answerer that
   agrees clears position 16 in its response, and goes on in the FCS of
   the SABME; between two Links: FCS-32 both ways when both have it, an
   FCS-16 when either has not, and the data through each way */
TEST(Link, LapmFcs32)
{
	const std::vector<uint8_t> ask = { 0x8a, 0x89, 0x01 },
		agree = { 0x8a, 0x09, 0x01 }, none = { 0x8a, 0x89, 0x00 };
	for (int on = 0; on < 2; on++) {
		Dte d("caller");
		Link l;
		Config c = lapm_config(0);
		c.fcs32 = on;
		l.init(&d, true, c);
		l.start(true, false);
		std::vector<std::vector<uint8_t>> out = exchange(l, {}, 4);
		CHECK(!out.empty() && xid_opt(out[0]) == (on ? ask : none),
		      "the caller's XID (fcs32 %d)", on);
	}

	/* an answerer: XID command asking for it, then the SABME with an
	   FCS-16 or an FCS-32 */
	const std::vector<uint8_t> xid = { 0x03, 0xaf, 0x82, 0x80, 0x00, 0x05,
		0x03, 0x03, 0x8a, 0x89, 0x01 };
	const std::vector<uint8_t> sabme = { 0x03, 0x7f }, ua = { 0x03, 0x73 };
	for (int s32 = 0; s32 < 2; s32++) {
		Dte d("answerer");
		Link l;
		l.init(&d, false, lapm_config(0));
		l.start(true, false);
		NullOwner o;
		Link side;
		side.init(&o, true, Config());
		Hdlc tx, rx;
		FrameSource src, dst;
		tx.start(&side);
		rx.start(&side);
		tx.set_framer(&src, true);
		rx.set_framer(&dst, false);
		rx.set_rx_fcs(RX_FCS16 | RX_FCS32);
		uint8_t bits[600];
		auto rounds = [&](int n) {
			for (int k = 0; k < n; k++) {
				tx.get_bits(1, bits, sizeof(bits));
				l.put_bits(1, bits, sizeof(bits));
				l.get_bits(1, bits, sizeof(bits));
				rx.put_bits(1, bits, sizeof(bits));
			}
		};
		src.tx = { xid };
		rounds(4);
		CHECK(dst.rx.size() == 1 && xid_opt(dst.rx[0]) == agree &&
		      !dst.rx32[0], "the answerer's XID response, FCS-16 "
		      "(%zu frames)", dst.rx.size());
		CHECK(l.hdlc().rx_fcs() == (RX_FCS16 | RX_FCS32) &&
		      !l.hdlc().tx_fcs32(), "after its response: either in, "
		      "FCS-16 out");
		tx.set_tx_fcs32(s32);
		src.tx.push_back(sabme);
		rounds(4);
		CHECK(dst.rx.size() == 2 && dst.rx[1] == ua &&
		      dst.rx32[1] == (s32 == 1), "UA with an FCS-%d",
		      s32 ? 32 : 16);
		CHECK(d.up && l.hdlc().tx_fcs32() == (s32 == 1) &&
		      l.hdlc().rx_fcs() == (s32 ? RX_FCS32 : RX_FCS16),
		      "the SABME's FCS both ways");
	}

	/* an answerer without it: no position 17 in its response */
	{
		Dte d("answerer");
		Link l;
		Config c = lapm_config(0);
		c.fcs32 = false;
		l.init(&d, false, c);
		l.start(true, false);
		std::vector<std::vector<uint8_t>> out = exchange(l, { xid }, 4);
		CHECK(out.size() == 1 && xid_opt(out[0]) == none,
		      "the answerer's XID response without it");
		CHECK(l.hdlc().rx_fcs() == RX_FCS16, "FCS-16 only in");
	}

	for (int k = 0; k < 3; k++) {
		rnd_state = 12345;
		Config cc = lapm_config(3), ca = lapm_config(3);
		cc.fcs32 = k != 1;
		ca.fcs32 = k != 2;
		Pair p(cc, ca);
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "LAPM up (%d)", k);
		const bool both = k == 0;
		const u8 rx = both ? RX_FCS32 : RX_FCS16;
		CHECK(p.lc.hdlc().tx_fcs32() == both &&
		      p.la.hdlc().tx_fcs32() == both &&
		      p.lc.hdlc().rx_fcs() == rx && p.la.hdlc().rx_fcs() == rx,
		      "FCS-%d both ways (%d)", both ? 32 : 16, k);
		transfer(both ? "lapm, fcs-32" : "lapm, fcs-16", p, 20000,
			 20000, 9600 * 40);
		p.ber = 1e-4;
		transfer(both ? "lapm, fcs-32, bit errors" :
			 "lapm, fcs-16, bit errors", p, 5000, 5000, 9600 * 60);
	}
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

/* V.44's parameters for each direction (7.4/V.44): the encoder of one
   end has the smaller of its P1T and the other end's P1R, and the same
   for P2 and P3; a caller that offers less for its decoder than for its
   encoder, and the data through each way */
TEST(Link, LapmV44Directions)
{
	rnd_state = 12345;
	Config cc = lapm_config(3), ca = lapm_config(3);
	cc.v44_codewords_r = 1024;
	cc.v44_max_string_r = 64;
	cc.v44_history_r = 4096;
	Pair p(cc, ca);
	p.start(true, true);
	p.run(9600 * 3);
	CHECK(p.dc.up && p.da.up, "LAPM up");
	const Config &c = p.lc.config(), &a = p.la.config();
	CHECK(c.v44 && a.v44 && c.comp == 3 && a.comp == 3, "V.44 both ways");
	CHECK(c.v44_codewords == 4096 && c.v44_max_string == 255 &&
	      c.v44_history == 12288 && c.v44_codewords_r == 1024 &&
	      c.v44_max_string_r == 64 && c.v44_history_r == 4096,
	      "caller: encoder %d/%d/%d, decoder %d/%d/%d",
	      (int)c.v44_codewords, (int)c.v44_max_string, (int)c.v44_history,
	      (int)c.v44_codewords_r, (int)c.v44_max_string_r,
	      (int)c.v44_history_r);
	CHECK(a.v44_codewords == 1024 && a.v44_max_string == 64 &&
	      a.v44_history == 4096 && a.v44_codewords_r == 4096 &&
	      a.v44_max_string_r == 255 && a.v44_history_r == 12288,
	      "answerer: encoder %d/%d/%d, decoder %d/%d/%d",
	      (int)a.v44_codewords, (int)a.v44_max_string, (int)a.v44_history,
	      (int)a.v44_codewords_r, (int)a.v44_max_string_r,
	      (int)a.v44_history_r);
	transfer("lapm + v44, each direction its own", p, 60000, 60000,
		 9600 * 120);
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

/* a DTE that takes a little now and then and at times nothing (a full
   pty), with compression: the decoder's output it has not taken reaches
   it later, the last of it too, when no frame comes after it */
TEST(Link, LapmCompressedSlowDte)
{
	const char *names[] = { "V.44", "V.42bis", "zstd" };
	for (int k = 0; k < 3; k++) {
		rnd_state = 12345;
		Config c = lapm_config(3, k == 0, k == 2);
		Pair p(c, c);
		p.start(true, true);
		p.run(9600 * 3);
		CHECK(p.dc.up && p.da.up, "%s: LAPM up", names[k]);
		const size_t n = 20000;
		p.dc.to_send = text(n);
		p.dc.pos = 0;
		p.da.got.clear();
		for (long run = 0; run < 9600L * 120 && p.da.got.size() < n;
		     run += 480) {
			p.da.room = 40;
			p.run(480);
		}
		p.da.room = -1;
		CHECK(p.da.got == p.dc.to_send, "%s, a DTE at times full: %zu/%zu",
		      names[k], p.da.got.size(), n);
	}
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
	rx.set_rx_fcs(RX_FCS32);        /* what the link has agreed */
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
 *    XID not agreed, and XID offers that change (#196, #197)
 *
 */

/* a scripted remote for the Link l: the frames it sends (send()) and
   the frames that l sent (out()), in rounds of 600 bits each way; it
   takes each FCS */
struct Script {
	NullOwner o;
	Link side;
	Hdlc tx, rx;
	FrameSource src, dst;
	Link &l;
	/* the remote of link */
	explicit Script(Link &link) : l(link) {
		side.init(&o, true, Config());
		tx.start(&side);
		rx.start(&side);
		tx.set_framer(&src, true);
		rx.set_framer(&dst, false);
		rx.set_rx_fcs(RX_FCS16 | RX_FCS32);
	}
	/* the frame f to l */
	void send(const std::vector<uint8_t> &f) { src.tx.push_back(f); }
	/* n rounds each way */
	void rounds(int n) {
		uint8_t bits[600];
		for (int k = 0; k < n; k++) {
			tx.get_bits(1, bits, sizeof(bits));
			l.put_bits(1, bits, sizeof(bits));
			l.get_bits(1, bits, sizeof(bits));
			rx.put_bits(1, bits, sizeof(bits));
		}
	}
	/* the frames that l sent */
	const std::vector<std::vector<uint8_t>> &out() const {
		return dst.rx;
	}
};

/* whether the frame f contains the three characters of id (a set's
   name in an XID) */
static bool names_set(const std::vector<uint8_t> &f, const char *id)
{
	return std::search(f.begin(), f.end(), id, id + 3) != f.end();
}

/* whether f is an XID frame */
static bool is_xid(const std::vector<uint8_t> &f)
{
	return f.size() >= 2 && (f[1] & 0xef) == 0xaf;
}

/* the information fields of the I frames in frames with the address
   addr, in sequence */
static std::vector<uint8_t> info_of(
	const std::vector<std::vector<uint8_t>> &frames, uint8_t addr)
{
	std::vector<uint8_t> v;
	for (auto &f : frames)
		if (f.size() > 3 && f[0] == addr && !(f[1] & 1))
			v.insert(v.end(), f.begin() + 3, f.end());
	return v;
}

/* the link l is up with V.42's defaults (no compression, FCS-16), and
   the scripted remote s and l exchange characters without a change:
   l's I frames have its DTE d's characters, and the remote's I frame
   (address addr) gets to d unchanged */
static void check_plain(const char *name, Script &s, Link &l, Dte &d,
			uint8_t addr)
{
	CHECK(d.up, "%s: up", name);
	CHECK(l.config().comp == 0 && !l.config().v44 && !l.config().zstd,
	      "%s: compression %d (V.44 %d, zstd %d)", name, l.config().comp,
	      l.config().v44, l.config().zstd);
	CHECK(!l.hdlc().tx_fcs32() && l.hdlc().rx_fcs() == RX_FCS16,
	      "%s: FCS-16", name);
	l.set_online(true);
	const std::string out = "from the D-Modem, plain text";
	const std::string in = "from the remote, plain text";
	d.to_send.assign(out.begin(), out.end());
	d.pos = 0;
	d.got.clear();
	std::vector<uint8_t> i = { addr, 0x00, 0x00 };
	i.insert(i.end(), in.begin(), in.end());
	const size_t before = s.out().size();
	s.send(i);
	s.rounds(8);
	std::vector<std::vector<uint8_t>> after(s.out().begin() + before,
						s.out().end());
	const uint8_t own = addr == 0x01 ? 0x03 : 0x01;
	CHECK(d.got == std::vector<uint8_t>(in.begin(), in.end()),
	      "%s: %zu octets in, %zu out", name, in.size(), d.got.size());
	CHECK(info_of(after, own) ==
	      std::vector<uint8_t>(out.begin(), out.end()),
	      "%s: our I frames are not the plain text", name);
}

/* an answerer with V.44 (and zstd off) that gets a SABME with no XID
   before it (7.2.2, 8.3/V.42): V.42's defaults, no V.44 */
TEST(Link, LapmSabmeWithoutXid)
{
	for (int zstd = 0; zstd < 2; zstd++) {
		Dte d("answerer");
		Link l;
		l.init(&d, false, lapm_config(3, true, zstd));
		l.start(true, false);
		Script s(l);
		s.send({ 0x03, 0x7f });
		s.rounds(4);
		CHECK(!s.out().empty() && s.out().back() ==
		      std::vector<uint8_t>({ 0x03, 0x73 }), "UA to the SABME");
		check_plain(zstd ? "sabme, zstd" : "sabme", s, l, d, 0x03);
	}
}

/* an answerer that gets an XID command it cannot read (no general
   format, or nothing), then the SABME: its response names no
   compression, and the link has V.42's defaults */
TEST(Link, LapmBadXidThenSabme)
{
	const std::vector<std::vector<uint8_t>> bad = {
		{ 0x03, 0xaf },
		{ 0x03, 0xaf, 0x81 },
		{ 0x03, 0xbf, 0x00, 0x80, 0x00, 0x00 },
	};
	for (auto &x : bad) {
		Dte d("answerer");
		Link l;
		Config c = lapm_config(3, true, true);
		l.init(&d, false, c);
		l.start(true, false);
		Script s(l);
		s.send(x);
		s.rounds(4);
		CHECK(s.out().size() == 1 && is_xid(s.out()[0]),
		      "an XID response (%zu frames)", s.out().size());
		if (!s.out().empty()) {
			const auto &r = s.out()[0];
			CHECK(!names_set(r, "V42") && !names_set(r, "V44") &&
			      !names_set(r, "ZST"),
			      "the response names compression");
		}
		s.send({ 0x03, 0x7f });
		s.rounds(4);
		check_plain("bad xid", s, l, d, 0x03);
	}
}

/* an originator with V.44 (zstd off) whose only XID gets a DM (F = 0,
   8.8/V.42): the remote agreed to nothing, so V.42's defaults; a DM
   after the XID was sent again, or an XID response that it cannot read,
   releases the link: the remote may have agreed to parameters that we
   do not know */
TEST(Link, LapmCallerXidNotAgreed)
{
	{
		Dte d("caller");
		Link l;
		l.init(&d, true, lapm_config(3));
		l.start(true, false);
		Script s(l);
		s.rounds(3);
		CHECK(s.out().size() == 1 && is_xid(s.out()[0]),
		      "the caller's XID (%zu frames)", s.out().size());
		s.send({ 0x03, 0x0f });
		s.rounds(4);
		CHECK(!s.out().empty() && s.out().back() ==
		      std::vector<uint8_t>({ 0x03, 0x7f }), "SABME after the DM");
		s.send({ 0x03, 0x73 });
		s.rounds(4);
		check_plain("dm", s, l, d, 0x01);
	}
	{
		Dte d("caller");
		Link l;
		l.init(&d, true, lapm_config(3));
		l.start(true, false);
		Script s(l);
		s.rounds(40);
		int xids = 0;
		for (auto &f : s.out())
			xids += is_xid(f);
		CHECK(xids == 2, "%d XIDs before the DM", xids);
		s.send({ 0x03, 0x0f });
		s.rounds(4);
		CHECK(d.released && !d.up, "released on the DM after the XID "
		      "was sent again");
		CHECK(s.out().size() == 2, "%zu frames after the DM (last "
		      "%02x %02x)", s.out().size(), s.out().back()[0],
		      s.out().back()[1]);
	}
	const std::vector<std::vector<uint8_t>> bad = {
		{ 0x03, 0xaf },
		{ 0x03, 0xaf, 0x81, 0x80, 0x00, 0x00 },
	};
	for (auto &x : bad) {
		Dte d("caller");
		Link l;
		l.init(&d, true, lapm_config(3));
		l.start(true, false);
		Script s(l);
		s.rounds(3);
		s.send(x);
		s.rounds(4);
		CHECK(d.released && !d.up, "released on an XID response that "
		      "it cannot read (%zu octets)", x.size());
		CHECK(s.out().size() == 1, "%zu frames after it",
		      s.out().size());
	}
}

/* an answerer with V.44 that gets an XID command with a V.44
   parameter below its minimum (a procedural error, 7.4/V.44: no
   response), then a SABME: the link has no compression, as no XID was
   agreed (#196), with zstd off and on; the XID of #196's report, and
   the caller's own XID with P2R, P1T or P3R too small */
TEST(Link, LapmXidOutOfRangeThenSabme)
{
	std::vector<std::vector<uint8_t>> bad = { {
		0x03, 0xaf, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89,
		0x01, 0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00, 0x07,
		0x01, 0x0f, 0x08, 0x01, 0x0f, 0xf0, 0x00, 0x20, 0x00, 0x03,
		0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02, 0x08, 0x00,
		0x03, 0x01, 0x22, 0x60, 0x03, 0x5a, 0x53, 0x54, 0x61, 0x59,
		0x03, 0x62, 0x01, 0x0f, 0x63, 0x04, 0x56, 0x43, 0x4d, 0x01,
		0xff, 0x40, 0x03, 0x56, 0x34, 0x34, 0x41, 0x01, 0x00, 0x42,
		0x01, 0x03, 0x43, 0x02, 0x10, 0x00, 0x4e, 0x02, 0x39, 0x00,
		0x45, 0x01, 0xff, 0x46, 0x01, 0x05, 0x47, 0x02, 0x30, 0x00,
		0x48, 0x02, 0x30, 0x00 } };
	{
		/* the caller's own XID (no zstd), one parameter changed */
		Dte d("caller");
		Link l;
		Config c = lapm_config(3);
		c.fcs32 = false;
		l.init(&d, true, c);
		l.start(true, false);
		Script s(l);
		s.rounds(3);
		CHECK(s.out().size() == 1 && is_xid(s.out()[0]),
		      "the caller's XID");
		const std::vector<uint8_t> x = s.out().at(0);
		const struct { uint8_t pi; uint32_t v; } low[] = {
			{ 0x46, 0x05 }, { 0x43, 0x00ff }, { 0x48, 0x01ff } };
		for (auto &w : low) {
			std::vector<uint8_t> y = x;
			static const char v44[] = "V44";
			auto at = std::search(y.begin(), y.end(), v44,
					      v44 + 3);
			bool done = false;
			for (size_t i = at - y.begin() + 3; i + 1 < y.size();
			     i += 2 + y[i + 1])
				if (y[i] == w.pi) {
					for (int k = y[i + 1] - 1, b = 0; k >= 0;
					     k--, b += 8)
						y[i + 2 + k] = (uint8_t)(w.v >> b);
					done = true;
					break;
				}
			CHECK(done, "PI %02x in the caller's XID", w.pi);
			bad.push_back(y);
		}
	}
	for (int zstd = 0; zstd < 2; zstd++)
		for (size_t n = 0; n < bad.size(); n++) {
			char name[48];
			snprintf(name, sizeof(name), "xid %zu, zstd %d", n,
				 zstd);
			Dte d("answerer");
			Link l;
			l.init(&d, false, lapm_config(3, true, zstd));
			l.start(true, false);
			Script s(l);
			s.send(bad[n]);
			s.rounds(4);
			CHECK(s.out().empty(), "%s: %zu frames to the XID",
			      name, s.out().size());
			s.send({ 0x03, 0x7f });
			s.rounds(4);
			if (!d.up)
				continue;
			check_plain(name, s, l, d, 0x03);
		}
}

/* the caller's and the answerer's Links, after the detection phase,
   with the frames between them read and sent again: the caller's
   dropped by drop(), the answerer's XID responses delayed delay bits */
struct Relay {
	Pair p;
	NullOwner o;
	Link side;
	Hdlc c_rx, a_tx, a_rx, c_tx;
	FrameSource from_c, to_a, from_a, to_c;
	size_t n_c = 0, n_a = 0;
	long now = 0, delay = 0;
	std::vector<std::pair<long, std::vector<uint8_t>>> late;
	bool (*drop)(const std::vector<uint8_t> &) = nullptr;
	bool relaying = false;

	/* the two Links with their configurations */
	Relay(const Config &cc, const Config &ca) : p(cc, ca) {
		side.init(&o, true, Config());
		for (Hdlc *h : { &c_rx, &a_tx, &a_rx, &c_tx })
			h->start(&side);
		c_rx.set_framer(&from_c, false);
		a_rx.set_framer(&from_a, false);
		a_tx.set_framer(&to_a, true);
		c_tx.set_framer(&to_c, true);
	}
	/* the call starts: V.8 before it or not (far) */
	void start(bool far) {
		p.lc.start(true, true, far);
		p.la.start(true, true, far);
	}
	/* run both ways, 48 bits at a time: bit by bit until the caller's
	   detection phase is done, then frame by frame */
	void run(long bits) {
		std::vector<uint8_t> b(48);
		for (long k = 0; k < bits; k += 48, now += 48) {
			if (!relaying && p.lc.phase_v42())
				relaying = true;
			p.lc.get_bits(1, b.data(), 48);
			if (relaying) {
				c_rx.put_bits(1, b.data(), 48);
				a_tx.get_bits(1, b.data(), 48);
			}
			p.la.put_bits(1, b.data(), 48);
			p.la.get_bits(1, b.data(), 48);
			if (relaying) {
				a_rx.put_bits(1, b.data(), 48);
				c_tx.get_bits(1, b.data(), 48);
			}
			p.lc.put_bits(1, b.data(), 48);
			for (; n_c < from_c.rx.size(); n_c++)
				if (!drop || !drop(from_c.rx[n_c]))
					to_a.tx.push_back(from_c.rx[n_c]);
			for (; n_a < from_a.rx.size(); n_a++) {
				const auto &f = from_a.rx[n_a];
				late.emplace_back(now + (is_xid(f) ? delay : 0),
						  f);
			}
			while (!late.empty() && late.front().first <= now) {
				to_c.tx.push_back(late.front().second);
				late.erase(late.begin());
			}
			if (p.dc.up)
				p.lc.set_online(true);
			if (p.da.up)
				p.la.set_online(true);
		}
	}
};

/* (Relay::drop) an XID with zstd's set: the answerer ignores it, as a
   USRobotics Courier does */
static bool drop_zstd_xid(const std::vector<uint8_t> &f)
{
	return is_xid(f) && names_set(f, "ZST");
}

/* Config::comp seen from the other end: its two bits exchanged */
static int swap_bits(int comp)
{
	return (comp & 1) << 1 | (comp >> 1 & 1);
}

/* the two ends of r agree on the compression, and text and noise go
   through each way */
static void check_agreed(const char *name, Relay &r)
{
	const Config &c = r.p.lc.config(), &a = r.p.la.config();
	CHECK(r.p.dc.up && r.p.da.up && r.p.lc.ec() && r.p.la.ec(),
	      "%s: LAPM up (%d, %d)", name, r.p.dc.up, r.p.da.up);
	CHECK(c.comp == swap_bits(a.comp) && c.v44 == a.v44 &&
	      c.zstd == a.zstd, "%s: caller %d/%d/%d, answerer %d/%d/%d",
	      name, c.comp, c.v44, c.zstd, a.comp, a.v44, a.zstd);
	Pair &p = r.p;
	for (int t = 0; t < 2; t++) {
		p.dc.to_send = t ? noise(3000) : text(3000);
		p.da.to_send = t ? noise(3000) : text(3000);
		p.dc.pos = p.da.pos = 0;
		p.dc.got.clear();
		p.da.got.clear();
		for (long k = 0; k < 9600 * 30 && (p.da.got.size() < 3000 ||
						   p.dc.got.size() < 3000);
		     k += 9600)
			r.run(9600);
		CHECK(p.da.got == p.dc.to_send && p.dc.got == p.da.to_send,
		      "%s: %s, %zu and %zu of 3000 octets", name,
		      t ? "noise" : "text", p.da.got.size(), p.dc.got.size());
	}
}

/* an answerer that ignores an XID with zstd's set (a Courier, #92):
   the caller offers its XID again without it, after V.8 too (from the
   third XID on), and LAPM comes up */
TEST(Link, LapmZstdIgnored)
{
	for (int far = 0; far < 2; far++) {
		rnd_state = 12345;
		Config cc = lapm_config(3, true, true);
		Config ca = lapm_config(3, true, false);
		cc.fcs32 = ca.fcs32 = false;
		Relay r(cc, ca);
		r.drop = drop_zstd_xid;
		r.start(far);
		r.run(9600 * 15);
		check_agreed(far ? "zstd ignored, v.8" : "zstd ignored", r);
	}
}

/* the answerer's XID responses come later than T401: the caller has
   sent its XID again (without zstd after the first), and must not take
   the late response to an earlier offer; the two ends agree, with V.8
   before the call or not */
TEST(Link, LapmXidResponseLate)
{
	for (int far = 0; far < 2; far++)
		for (long delay : { 25000L, 45000L }) {
			rnd_state = 12345;
			Config cc = lapm_config(3, true, true);
			Config ca = lapm_config(3, true, true);
			cc.fcs32 = ca.fcs32 = false;
			Relay r(cc, ca);
			r.delay = delay;
			r.start(far);
			r.run(9600 * 20);
			char name[64];
			snprintf(name, sizeof(name), "late %ld bits%s", delay,
				 far ? ", v.8" : "");
			check_agreed(name, r);
		}
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

/* an XID command with the V.44 set, C0 and P0 only (the user data
   subfield, Table A.1/V.44), and pars after them */
static std::vector<uint8_t> v44_xid(std::vector<uint8_t> pars = {})
{
	std::vector<uint8_t> f = { 0x03, 0xaf, 0x82, 0xff, 0x40, 3, 'V', '4',
				   '4', 0x41, 1, 0, 0x42, 1, 3 };
	f.insert(f.end(), pars.begin(), pars.end());
	return f;
}

/* V.44's parameters that an XID does not have are the defaults of
   Table 10/V.44 (P1 1024, P2 255, P3 3 x P1), not our own: an answerer
   that gets them so */
TEST(Link, LapmV44Defaults)
{
	Dte d("answerer");
	Link l;
	l.init(&d, false, lapm_config(3));
	l.start(true, false);
	Script s(l);
	s.send(v44_xid({ 0x43, 2, 0x08, 0x00 }));       /* P1T 2048 only */
	s.rounds(4);
	CHECK(s.out().size() == 1 && is_xid(s.out()[0]) &&
	      names_set(s.out()[0], "V44"), "an XID response with V.44");
	const Config &c = l.config();
	CHECK(c.v44 && c.v44_codewords == 1024 && c.v44_max_string == 255 &&
	      c.v44_history == 3072 && c.v44_codewords_r == 2048 &&
	      c.v44_max_string_r == 255 && c.v44_history_r == 6144,
	      "encoder %d/%d/%d, decoder %d/%d/%d", (int)c.v44_codewords,
	      (int)c.v44_max_string, (int)c.v44_history,
	      (int)c.v44_codewords_r, (int)c.v44_max_string_r,
	      (int)c.v44_history_r);
}

/* an answerer in the data phase with V.42's defaults (a SABME with no
   XID), its DTE online */
static void up_plain(Script &s, Link &l)
{
	s.send({ 0x03, 0x7f });
	s.rounds(4);
	l.set_online(true);
}

/* the frames of l since before, as UI responses' information fields */
static std::vector<std::vector<uint8_t>> ui_responses(
	const Script &s, size_t before)
{
	std::vector<std::vector<uint8_t>> v;
	for (size_t i = before; i < s.out().size(); i++) {
		const auto &f = s.out()[i];
		if (f.size() >= 3 && f[0] == 0x03 && (f[1] & 0xef) == 0x03)
			v.emplace_back(f.begin() + 2, f.end());
	}
	return v;
}

/* the break (8.13/V.42): each BRK gets a BRKACK with N(RB) = V(RB) and
   F = P; a repeated BRK (N(SB) not V(RB)) gets the BRKACK again; a
   destructive BRK (D 1) resets the sequence variables, so the remote's
   next I frame is N(S) 0 again */
TEST(Link, LapmBreak)
{
	Dte d("answerer");
	Link l;
	l.init(&d, false, lapm_config(3));
	l.start(true, false);
	Script s(l);
	up_plain(s, l);
	CHECK(d.up, "up");

	size_t before = s.out().size();
	s.send({ 0x03, 0x03, 0x40, 0x40 });     /* BRK, N(SB) 0, expedited */
	s.rounds(4);
	s.send({ 0x03, 0x13, 0x40, 0x00, 10 }); /* again, P 1: repeated */
	s.rounds(4);
	s.send({ 0x03, 0x03, 0xc0, 0x00 });     /* N(SB) 1: new */
	s.rounds(4);
	auto acks = ui_responses(s, before);
	CHECK(acks.size() == 3, "%zu BRKACKs", acks.size());
	if (acks.size() == 3) {
		CHECK(acks[0] == std::vector<uint8_t>({ 0xe0 }) &&
		      acks[1] == std::vector<uint8_t>({ 0xe0 }) &&
		      acks[2] == std::vector<uint8_t>({ 0x60 }),
		      "BRKACKs %02x %02x %02x", acks[0][0], acks[1][0],
		      acks[2][0]);
	}
	/* F = P */
	CHECK(s.out().size() >= 2 &&
	      s.out()[s.out().size() - 2][1] == 0x13,
	      "the BRKACK to a BRK with P 1 has F 1");

	/* I frame N(S) 0, then a destructive BRK (N(SB) 0), then N(S) 0
	   again: both I frames to the DTE */
	const std::string a = "before the break", b = "after the break";
	std::vector<uint8_t> i = { 0x03, 0x00, 0x00 };
	i.insert(i.end(), a.begin(), a.end());
	s.send(i);
	s.rounds(6);
	before = s.out().size();
	s.send({ 0x03, 0x03, 0x40, 0xc0, 0 });
	s.rounds(4);
	acks = ui_responses(s, before);
	CHECK(acks.size() == 1 && acks[0] == std::vector<uint8_t>({ 0xe0 }),
	      "destructive: %zu BRKACKs", acks.size());
	i = { 0x03, 0x00, 0x00 };
	i.insert(i.end(), b.begin(), b.end());
	s.send(i);
	s.rounds(6);
	const std::string ab = a + b;
	CHECK(d.got == std::vector<uint8_t>(ab.begin(), ab.end()),
	      "destructive: %zu octets to the DTE", d.got.size());
	CHECK(d.up && !d.released, "destructive: still up");
}

/* LAPM up again in the data phase (a SABME, 8.3.2.1): V.44 starts
   again too (C-INIT on L-ESTABLISH, 7.5/V.44).  The remote's encoder
   is new after the SABME; the answerer's decoder must be new too */
TEST(Link, LapmReestablishCInit)
{
	Dte d("answerer");
	Link l;
	l.init(&d, false, lapm_config(3));
	l.start(true, false);
	Script s(l);
	s.send(v44_xid());              /* the defaults: 1024, 255, 3072 */
	s.rounds(4);
	s.send({ 0x03, 0x7f });
	s.rounds(4);
	l.set_online(true);
	CHECK(d.up && l.config().v44, "up with V.44");
	auto frame = [](const std::string &t, int ns) {
		V44 e;
		e.init(1024, 255, 3072);
		std::vector<uint8_t> f = { 0x03, (uint8_t)(ns << 1), 0x00 };
		for (char c : t)
			e.encode((uint8_t)c);
		e.flush_encoder();
		uint8_t buf[512];
		int n;
		while ((n = e.out(buf, sizeof(buf))) > 0)
			f.insert(f.end(), buf, buf + n);
		return f;
	};
	const std::string a = "the line the line the line of the modem";
	const std::string b = "abcabcabcabc xyzxyzxyz 0101010101";
	s.send(frame(a, 0));
	s.rounds(6);
	s.send({ 0x03, 0x7f });         /* up again */
	s.rounds(4);
	s.send(frame(b, 0));            /* a new encoder, N(S) 0 */
	s.rounds(6);
	const std::string ab = a + b;
	CHECK(d.got == std::vector<uint8_t>(ab.begin(), ab.end()),
	      "%zu octets to the DTE: %.*s", d.got.size(), (int)d.got.size(),
	      (const char *)d.got.data());
}

/* a compression parameter less than its minimum is a procedural error
   (5.1/V.42bis, 7.4/V.44): the connection stops.  An answerer with
   such an XID command sends no response; a caller with such an XID
   response sends no SABME */
TEST(Link, LapmXidOutOfRange)
{
	struct Case {
		const char *name;
		std::vector<uint8_t> xid;
		bool v44;
	};
	/* the private group with V.42bis (Table 9/V.42bis): P0 3, then
	   N2 and N7 */
	auto v42 = [](uint8_t n2_hi, uint8_t n2_lo, uint8_t n7) {
		return std::vector<uint8_t>{ 0x03, 0xaf, 0x82, 0xf0, 0x00,
					     15, 0x00, 3, 'V', '4', '2',
					     0x01, 1, 3, 0x02, 2, n2_hi,
					     n2_lo, 0x03, 1, n7 };
	};
	const Case cases[] = {
		{ "V.44 P1T 255", v44_xid({ 0x43, 2, 0x00, 0xff }), true },
		{ "V.44 P1R 100", v44_xid({ 0x44, 2, 0x00, 100 }), true },
		{ "V.44 P2T 31", v44_xid({ 0x45, 1, 31 }), true },
		{ "V.44 P3R 511", v44_xid({ 0x48, 2, 0x01, 0xff }), true },
		{ "V.42bis N2 511", v42(0x01, 0xff, 32), false },
		{ "V.42bis N7 5", v42(0x08, 0x00, 5), false },
	};
	for (const Case &k : cases) {
		Dte d("answerer");
		Link l;
		l.init(&d, false, lapm_config(3, k.v44));
		l.start(true, false);
		Script s(l);
		s.send(k.xid);
		s.rounds(4);
		CHECK(s.out().empty() && d.released && !d.up,
		      "%s: %zu frames, released %d", k.name, s.out().size(),
		      d.released);
	}
	/* in range: a response */
	{
		Dte d("answerer");
		Link l;
		l.init(&d, false, lapm_config(3));
		l.start(true, false);
		Script s(l);
		s.send(v44_xid({ 0x43, 2, 0x01, 0x00, 0x45, 1, 32, 0x48, 2,
				 0x02, 0x00 }));
		s.rounds(4);
		CHECK(s.out().size() == 1 && is_xid(s.out()[0]) &&
		      !d.released, "in range: no response");
	}
	/* the caller: an XID response with V.44's P1R 200 */
	{
		Dte d("caller");
		Link l;
		l.init(&d, true, lapm_config(3));
		l.start(true, false);
		Script s(l);
		s.rounds(3);
		CHECK(s.out().size() == 1 && is_xid(s.out()[0]),
		      "the caller's XID (%zu frames)", s.out().size());
		std::vector<uint8_t> r = v44_xid({ 0x44, 2, 0x00, 200 });
		r[0] = 0x01;                    /* a response */
		s.send(r);
		s.rounds(4);
		CHECK(s.out().size() == 1 && d.released && !d.up,
		      "caller: %zu frames, released %d", s.out().size(),
		      d.released);
	}
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

/* no data for 20 s after the start: T404 makes the caller send its LR
   again (as for the ESS modem); our answerer stays in the data phase,
   so the data phase of the caller continues and the data goes through */
TEST(Link, MnpIdleStart)
{
	rnd_state = 12345;
	Config cc = mnp_config(true);
	cc.mnp_first = true;
	Pair p(cc, mnp_config(true));
	p.start(true, true);
	p.run(9600 * 20);
	CHECK(p.dc.up && p.da.up, "MNP up");
	CHECK(!p.dc.released && !p.da.released, "not released");
	transfer("mnp, after 20 s idle", p, 20000, 20000, 9600 * 60);
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


/* MNP alone (no LAPM) with V.42bis (comp: Config::comp's directions;
   offered as the initiator, S114) and MNP 5 */
static Config mnp_v42_config(s16 comp, bool mnp5 = true)
{
	Config c = mnp_config(false, mnp5);
	c.comp = comp;
	c.mnp_v42_offer = true;
	return c;
}

/* the first LR the link at l sends (the answerer's after in, its
   initiator's LR, at the line), as its body */
static std::vector<uint8_t> first_lr(Link &l, Dte &d,
				     const std::vector<uint8_t> &in_lr)
{
	std::vector<uint8_t> in(200, 1), out, b(48);
	if (!in_lr.empty())
		char_bits(in, mnp_octets(in_lr));
	in.resize(in.size() + 9600, 1);
	in.resize((in.size() + 47) / 48 * 48, 1);
	for (size_t k = 0; k < in.size(); k += 48) {
		l.get_bits(1, b.data(), 48);
		out.insert(out.end(), b.begin(), b.end());
		l.put_bits(1, in.data() + k, 48);
	}
	(void)d;
	auto frames = mnp_frames(line_chars(out));
	return frames.empty() ? std::vector<uint8_t>() : frames[0];
}

/* the value of parameter type in an LR's body (empty if none) */
static std::vector<uint8_t> lr_param(const std::vector<uint8_t> &lr,
				     uint8_t type)
{
	for (size_t i = 3; i + 2 <= lr.size(); i += 2 + lr[i + 1])
		if (lr[i] == type && i + 2 + lr[i + 1] <= lr.size())
			return std::vector<uint8_t>(lr.begin() + i + 2,
				lr.begin() + i + 2 + lr[i + 1]);
	return {};
}

/* the initiator's offer of V.42bis in its LR, as the SM56 sends it:
   parameter 9 with MNP 5 and the short form (0x10: N2 2048), and
   parameter 14 (P0 3, N2 2048 with the high octet first, N7 34);
   one direction only: parameter 14 alone (the short form is both
   directions) with the initiator's P0 */
TEST(Link, MnpV42bisLr)
{
	Dte d("caller");
	Link l;
	l.init(&d, true, mnp_v42_config(3));
	l.start(true, true);
	auto lr = first_lr(l, d, {});
	CHECK(lr_param(lr, 9) == std::vector<uint8_t>{ 0x1d },
	      "parameter 9: MNP 5 and the short form");
	CHECK(lr_param(lr, 14) ==
	      (std::vector<uint8_t>{ 0x03, 0x08, 0x00, 0x20 }),
	      "parameter 14");

	/* not offered as the initiator (S114 0, the default): the LR of
	   before, MNP 5 alone */
	Config off = mnp_v42_config(3);
	off.mnp_v42_offer = false;
	Dte d0("caller");
	Link l0;
	l0.init(&d0, true, off);
	l0.start(true, true);
	auto lr0 = first_lr(l0, d0, {});
	CHECK(lr_param(lr0, 9) == std::vector<uint8_t>{ 0x01 } &&
	      lr_param(lr0, 14).empty(), "not offered: MNP 5 alone");

	/* our encoder only (Config::comp 2): P0 0x01, the initiator to
	   the responder */
	Dte d2("caller");
	Link l2;
	l2.init(&d2, true, mnp_v42_config(2, false));
	l2.start(true, true);
	lr = first_lr(l2, d2, {});
	CHECK(lr_param(lr, 9).empty(), "no parameter 9");
	CHECK(lr_param(lr, 14) ==
	      (std::vector<uint8_t>{ 0x01, 0x08, 0x00, 0x20 }),
	      "parameter 14, one way");
}

/* our responder against the initiator's offers: the SM56's (MNP 5,
   the short form and parameter 14): parameter 14 back with the smaller
   N7 and no MNP 5; the short form alone: the short form back; MNP 5
   alone: MNP 5 back */
TEST(Link, MnpV42bisResponder)
{
	const std::vector<uint8_t> head = {
		0x00, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08, 0x04, 0x02, 0x40,
		0x00, 0x08, 0x01, 0x03 };
	auto lr_with = [&](const std::vector<uint8_t> &tail) {
		std::vector<uint8_t> lr = head;
		lr.insert(lr.end(), tail.begin(), tail.end());
		lr[0] = static_cast<uint8_t>(lr.size() - 1);
		return lr;
	};
	struct Case {
		const char *name;
		std::vector<uint8_t> tail, p9, p14;
		s16 comp;
		bool mnp5;
	};
	const Case cases[] = {
		{ "SM56", { 0x09, 0x01, 0x11, 0x0e, 0x04, 0x03, 0x08, 0x00,
			    0x20 }, {}, { 0x03, 0x08, 0x00, 0x20 }, 3, false },
		{ "short form", { 0x09, 0x01, 0x09 }, { 0x08 }, {}, 3,
		  false },
		{ "one way", { 0x0e, 0x04, 0x01, 0x02, 0x00, 0x40 }, {},
		  { 0x01, 0x02, 0x00, 0x20 }, 1, false },
		{ "MNP 5", { 0x09, 0x01, 0x01 }, { 0x01 }, {}, 0, true },
	};
	for (const Case &c : cases) {
		Dte d("answerer");
		Link l;
		l.init(&d, false, mnp_v42_config(3));
		l.start(true, true);
		auto ours = first_lr(l, d, lr_with(c.tail));
		CHECK(lr_param(ours, 9) == c.p9, "%s: parameter 9", c.name);
		CHECK(lr_param(ours, 14) == c.p14, "%s: parameter 14",
		      c.name);
		/* the confirming LA */
		std::vector<uint8_t> in(100, 1), b(48);
		char_bits(in, mnp_octets({ 0x03, 0x05, 0x00, 0x08 }));
		in.resize((in.size() + 480 + 47) / 48 * 48, 1);
		for (size_t k = 0; k < in.size(); k += 48) {
			l.get_bits(1, b.data(), 48);
			l.put_bits(1, in.data() + k, 48);
		}
		CHECK(d.up, "%s: up", c.name);
		CHECK(d.agreed.comp == c.comp && d.agreed.mnp5 == c.mnp5,
		      "%s: compression %d, MNP 5 %d", c.name, d.agreed.comp,
		      d.agreed.mnp5);
	}
}

/* two D-Modems: V.42bis both ways in place of MNP 5 (text and noise);
   one way only; a side without V.42bis: MNP 5; an initiator that
   refuses a response with a larger N2 than it offered (LD) */
TEST(Link, MnpV42bis)
{
	rnd_state = 12345;
	Pair p(mnp_v42_config(3), mnp_v42_config(3));
	p.start(true, true);
	p.run(9600 * 2);
	CHECK(p.dc.up && p.da.up, "up");
	CHECK(p.dc.agreed.comp == 3 && p.da.agreed.comp == 3 &&
	      !p.dc.agreed.mnp5 && !p.da.agreed.mnp5,
	      "V.42bis agreed (%d, %d)", p.dc.agreed.comp, p.da.agreed.comp);
	CHECK(p.dc.agreed.dict_size == 2048 && p.dc.agreed.max_string == 32,
	      "N2 %d, N7 %d", (int)p.dc.agreed.dict_size,
	      (int)p.dc.agreed.max_string);
	transfer("mnp + v42bis, text", p, 60000, 40000, 9600 * 60);
	transfer("mnp + v42bis, noise", p, 10000, 10000, 9600 * 60, false);

	/* the caller compresses, the answerer does not */
	Pair q(mnp_v42_config(2), mnp_v42_config(1));
	q.start(true, true);
	q.run(9600 * 2);
	CHECK(q.dc.agreed.comp == 2 && q.da.agreed.comp == 1,
	      "one way (%d, %d)", q.dc.agreed.comp, q.da.agreed.comp);
	transfer("mnp + v42bis one way, text", q, 30000, 20000, 9600 * 60);

	/* the answerer without V.42bis */
	Pair r(mnp_v42_config(3), mnp_v42_config(0));
	r.start(true, true);
	r.run(9600 * 2);
	CHECK(r.dc.agreed.comp == 0 && r.da.agreed.comp == 0 &&
	      r.dc.agreed.mnp5 && r.da.agreed.mnp5, "MNP 5 in its place");
	transfer("mnp5 (no v42bis), text", r, 20000, 20000, 9600 * 60);

	/* a smaller N2 at the caller: the answerer takes it */
	Config small = mnp_v42_config(3);
	small.dict_size = 512;
	Pair s2(small, mnp_v42_config(3));
	s2.start(true, true);
	s2.run(9600 * 2);
	CHECK(s2.dc.agreed.dict_size == 512 && s2.da.agreed.dict_size == 512,
	      "N2 512 (%d, %d)", (int)s2.dc.agreed.dict_size,
	      (int)s2.da.agreed.dict_size);
	transfer("mnp + v42bis 512, text", s2, 20000, 20000, 9600 * 60);
}

/* the initiator refuses a response that has V.42bis with more than it
   offered: an LD, not the data phase */
TEST(Link, MnpV42bisRefused)
{
	Config c = mnp_v42_config(3);
	c.dict_size = 1024;
	Dte d("caller");
	Link l;
	l.init(&d, true, c);
	l.start(true, true);
	auto lr = first_lr(l, d, {});
	CHECK(lr_param(lr, 14) ==
	      (std::vector<uint8_t>{ 0x03, 0x04, 0x00, 0x20 }),
	      "our offer, N2 1024");
	/* a response with N2 2048 */
	std::vector<uint8_t> resp = {
		0x00, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08, 0x04, 0x02, 0x40,
		0x00, 0x08, 0x01, 0x03, 0x0e, 0x04, 0x03, 0x08, 0x00, 0x20 };
	resp[0] = static_cast<uint8_t>(resp.size() - 1);
	Dte d3("caller");
	Link l3;
	l3.init(&d3, true, c);
	l3.start(true, true);
	/* after its LR (64 characters of marks first) */
	std::vector<uint8_t> in(2000, 1), out, b(48);
	char_bits(in, mnp_octets(resp));
	in.resize((in.size() + 9600 + 47) / 48 * 48, 1);
	for (size_t k = 0; k < in.size(); k += 48) {
		l3.get_bits(1, b.data(), 48);
		out.insert(out.end(), b.begin(), b.end());
		l3.put_bits(1, in.data() + k, 48);
	}
	bool ld = false;
	for (auto &f : mnp_frames(line_chars(out)))
		ld |= f.size() >= 2 && f[1] == 0x02;
	CHECK(ld, "LD");
	CHECK(!(d3.up && l3.protocol() == Protocol::Mnp), "no data phase");
}

/*
 *    V.80 Synchronous Access Mode
 *
 */

namespace {


/*
 *    MNP 10
 *
 */

/* a DTE whose "pumps" MNP 10 shifts: the modulations it has, the one
   running and its rate, the line's EQM; a shift asked waits for the
   test (pending) */
struct M10Dte : Dte {
	u32 mods = 0;
	Mod mod = Mod::V32B;
	int32_t rate = 14400;
	s16 eqm = 2;
	bool pending = false;
	Shift asked = {};
	std::vector<Shift> shifts;
	s16 level = -1;
	/* the DTE named n */
	explicit M10Dte(const char *n) : Dte(n) {}
	/* the "pump's" rate */
	int32_t tx_rate() override { return rate; }
	/* the modulations it has */
	u32 mnp10_mods() override { return mods; }
	/* the one running */
	Mod mnp10_mod() override { return mod; }
	/* a line at -20 dBm with eqm's eye quality */
	void mnp10_line(s16 &dbm, s16 &e) override {
		dbm = -20;
		e = eqm;
	}
	/* a shift asked: kept, the change pending */
	bool mnp10_shift(const Shift &s) override {
		asked = s;
		shifts.push_back(s);
		pending = true;
		return true;
	}
	/* the cellular level */
	void mnp10_tx_level(s16 db) override { level = db; }
};

/* two links back to back with MNP 10: a shift's change is the test's
   (both sides asked: both "pumps" change, V.34 to the top it allows) */
struct M10Pair {
	M10Dte dc{"caller"}, da{"answerer"};
	Link lc, la;
	double ber = 0;
	long errors = 0;
	int32_t v34_top = 33600;
	/* the caller with cc, the answerer with ca */
	M10Pair(const Config &cc, const Config &ca) {
		lc.init(&dc, true, cc);
		la.init(&da, false, ca);
		const char *v = getenv("LINK_DEBUG");
		dc.verbose = da.verbose = v && *v;
	}
	/* both links started */
	void start() {
		lc.start(true, true);
		la.start(true, true);
	}
	/* the change both asked: to its modulation and rate */
	static void change(M10Dte &d, Link &l, int32_t v34_top) {
		d.pending = false;
		d.mod = d.asked.mod;
		d.rate = d.asked.mod == Mod::V34 && d.asked.up ?
			std::max(d.asked.rate, std::min(v34_top, d.asked.rate)) :
			d.asked.rate;
		l.shift_done(true);
	}
	/* seconds of the line both ways */
	void run(long seconds) {
		std::vector<uint8_t> b(48);
		for (long t = 0; t < seconds * 100; t++) {
			/* 10 ms of bits at each side's rate */
			for (int32_t k = 0; k < std::max(48, dc.rate / 100);
			     k += 48) {
				lc.get_bits(1, b.data(), 48);
				flip(b);
				if (!da.pending)
					la.put_bits(1, b.data(), 48);
				la.get_bits(1, b.data(), 48);
				flip(b);
				if (!dc.pending)
					lc.put_bits(1, b.data(), 48);
			}
			if (dc.up)
				lc.set_online(true);
			if (da.up)
				la.set_online(true);
			if (dc.pending && da.pending) {
				change(dc, lc, v34_top);
				change(da, la, v34_top);
			}
		}
	}
	/* ber's bit errors in b */
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

/* MNP with MNP 10 (ext: the D-Modem extension) */
static Config m10_config(bool ext = false)
{
	Config c = mnp_config(false);
	c.mnp10 = true;
	c.mnp10_ext = ext;
	return c;
}

/* -K1: a V.42 call whose XIDs carry MNP's LR parameters becomes MNP 10
   in HDLC frames, data both ways; -K0 at either end stays V.42 */
TEST(Link, Mnp10Upgrade)
{
	for (int k = 0; k < 3; k++) {
		rnd_state = 777;
		Config cc = m10_config(), ca = m10_config();
		cc.ec = ca.ec = true;
		cc.mnp10_upgrade = k != 1;
		ca.mnp10_upgrade = k != 2;
		M10Pair p(cc, ca);
		for (M10Dte *d : { &p.dc, &p.da })
			d->mods = 1u << int(Mod::V32B) |
				1u << int(Mod::V22B) | 1u << int(Mod::V22);
		p.start();
		p.run(4);
		CHECK(p.dc.up && p.da.up, "up (%d)", k);
		const bool m10 = p.lc.mnp10() && p.la.mnp10();
		CHECK(m10 == (k == 0), "MNP 10 %s (%d)",
		      k == 0 ? "by XID" : "not agreed", k);
		CHECK((p.lc.protocol() == Protocol::Lapm) == (k != 0) &&
		      (p.la.protocol() == Protocol::Lapm) == (k != 0),
		      "V.42 %s (%d)", k ? "kept" : "left", k);
		p.dc.to_send = text(30000);
		p.da.to_send = text(20000);
		p.run(30);
		CHECK(p.da.got == p.dc.to_send && p.dc.got == p.da.to_send,
		      "data both ways (%d: %zu, %zu)", k, p.da.got.size(),
		      p.dc.got.size());
	}
}

/* our first LR (the caller's) with cfg, the pump on mod at rate */
static std::vector<uint8_t> m10_first_lr(const Config &cfg, u32 mods,
					 Mod mod = Mod::V32B,
					 int32_t rate = 14400)
{
	M10Dte d("caller");
	d.mods = mods;
	d.mod = mod;
	d.rate = rate;
	Link l;
	l.init(&d, true, cfg);
	l.start(true, true);
	std::vector<uint8_t> bits, b(48), idle(48, 1);
	for (int k = 0; k < 14400 * 2 / 48; k++) {
		l.get_bits(1, b.data(), 48);
		bits.insert(bits.end(), b.begin(), b.end());
		l.put_bits(1, idle.data(), 48);
	}
	auto frames = mnp_frames(line_chars(bits));
	return frames.empty() ? std::vector<uint8_t>() : frames[0];
}

/* v in hex, each octet after a space */
static std::string hex_of(const std::vector<uint8_t> &v)
{
	std::string hex;
	for (uint8_t c : v)
		hex += fmt(" %02x", c);
	return hex;
}

/* the LR with MNP 10's parameters: 8's bit 4, 0x0C, 0x0D; 0x11 only
   with S113 bit 2 (the PCTel modem refuses it) */
TEST(Link, Mnp10Lr)
{
	const u32 mods = 1u << int(Mod::V32B) | 1u << int(Mod::V22B) |
		1u << int(Mod::V22);
	std::vector<uint8_t> lr = {
		0x1f, 0x01, 0x02, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x02, 0x01, 0x03, 0x03, 0x01, 0x08, 0x04, 0x02, 0x40,
		0x00, 0x08, 0x01, 0x13,
		0x0c, 0x02, 0x4f, 0x3d,         /* shifts, 1200, 2400; V.32bis */
		0x0d, 0x02, 0x07, 0x06 };       /* -20 dBm, EQM 2 */
	auto got = m10_first_lr(m10_config(), mods);
	CHECK(got == lr, "our LR:%s", hex_of(got).c_str());
	Config c = m10_config();
	c.mnp10_long = true;
	lr[0] = 0x22;
	lr.insert(lr.end(), { 0x11, 0x01, 0x00 });
	got = m10_first_lr(c, mods);
	CHECK(got == lr, "our LR with 0x11:%s", hex_of(got).c_str());
}

/* the caller's LR stays within 38 octets (the SM56 modem refuses a
   longer one): 0xC0 (shorter), 0x0D and 0x11 only where they fit, no
   MNP 10 offer where 0x0C does not fit; S113 bit 2 (mnp10_long) sends
   all of them */
TEST(Link, Mnp10LrLength)
{
	const u32 mods = 1u << int(Mod::V34) | 1u << int(Mod::V32B) |
		1u << int(Mod::V22B) | 1u << int(Mod::V22);
	struct {
		bool mnp5, v42, lng;
		bool m10, p0d;          /* MNP 10 offered, 0x0D sent */
		int c0;                 /* 0xC0's length, 0 for none */
	} cases[] = {
		{ false, false, false, true, false, 6 },
		{ true, false, false, true, false, 3 },
		{ true, false, true, true, true, 7 },
		{ true, true, false, false, false, 0 },
		{ true, true, true, true, true, 7 },
	};
	for (auto &k : cases) {
		Config c = m10_config(true);
		c.mnp10_long = k.lng;
		c.mnp5 = k.mnp5;
		if (k.v42) {
			c.comp = 3;
			c.mnp_v42_offer = true;
		}
		auto lr = m10_first_lr(c, mods, Mod::V34, 28800);
		const std::string h = hex_of(lr);
		CHECK(!lr.empty() && (k.lng || lr.size() <= 38),
		      "%zu octets:%s", lr.size(), h.c_str());
		int c0 = 0, p8 = -1;
		bool p0c = false, p0d = false, p11 = false;
		for (size_t i = 3; i + 2 <= lr.size(); i += 2 + lr[i + 1]) {
			if (lr[i] == 0xc0)
				c0 = lr[i + 1];
			if (lr[i] == 8)
				p8 = lr[i + 2];
			p0c |= lr[i] == 0x0c;
			p0d |= lr[i] == 0x0d;
			p11 |= lr[i] == 0x11;
		}
		CHECK(p0c == k.m10 && (p8 >= 0 && !!(p8 & 0x10) == k.m10),
		      "MNP 10 offered %d:%s", k.m10, h.c_str());
		CHECK(p0d == k.p0d, "0x0D %d:%s", k.p0d, h.c_str());
		CHECK(c0 == k.c0, "0xC0 of %d octets (%d):%s", k.c0, c0,
		      h.c_str());
		CHECK(p11 == k.lng, "0x11 %d:%s", k.lng, h.c_str());
	}
}

/* MNP 10 agreed between two of ours, the probes grow the LTs, data both
   ways, no shift on a clean V.32bis line at its top */
TEST(Link, Mnp10)
{
	rnd_state = 12345;
	M10Pair p(m10_config(), m10_config());
	for (M10Dte *d : { &p.dc, &p.da })
		d->mods = 1u << int(Mod::V32B) | 1u << int(Mod::V22B) |
			1u << int(Mod::V22);
	p.start();
	p.run(3);
	CHECK(p.dc.up && p.da.up, "up");
	CHECK(p.lc.mnp().mnp10() && p.la.mnp().mnp10(), "MNP 10 agreed");
	s16 n = 0;
	p.lc.mnp().ladder(&n);
	CHECK(n == 7, "7 rungs (%d)", n);
	p.dc.to_send = text(100000);
	p.da.to_send = text(60000);
	p.run(90);
	CHECK(p.da.got == p.dc.to_send, "caller -> answerer %zu",
	      p.da.got.size());
	CHECK(p.dc.got == p.da.to_send, "answerer -> caller %zu",
	      p.dc.got.size());
	CHECK(p.lc.mnp().lt_size() == 256, "LTs of 256 (%d)",
	      p.lc.mnp().lt_size());
	CHECK(p.dc.shifts.empty() && p.da.shifts.empty(), "no shift");
}

/* bit errors: the fallback down V.32bis, both sides alike, data intact */
TEST(Link, Mnp10Fallback)
{
	rnd_state = 54321;
	M10Pair p(m10_config(), m10_config());
	for (M10Dte *d : { &p.dc, &p.da })
		d->mods = 1u << int(Mod::V32B) | 1u << int(Mod::V22B) |
			1u << int(Mod::V22);
	p.start();
	p.run(3);
	CHECK(p.lc.mnp().mnp10(), "MNP 10");
	p.ber = 3e-4;
	p.dc.to_send = text(40000);
	p.da.to_send = text(40000);
	p.run(40);
	p.ber = 0;
	p.run(200);
	bool down = false;
	for (auto &s : p.dc.shifts)
		down |= !s.up && s.rate < 14400;
	CHECK(down, "down while the errors lasted");
	CHECK(p.dc.rate == p.da.rate && p.dc.mod == p.da.mod,
	      "both alike (%d %d)", p.dc.rate, p.da.rate);
	CHECK(p.da.got == p.dc.to_send && p.dc.got == p.da.to_send,
	      "data intact (%zu, %zu)", p.da.got.size(), p.dc.got.size());
}

/* between two D-Modems, from V.22 up through V.22bis and V.32bis to
   V.34's top (start-ups between them, V.34's renegotiation in it) */
TEST(Link, Mnp10ClimbAcross)
{
	rnd_state = 777;
	M10Pair p(m10_config(true), m10_config(true));
	for (M10Dte *d : { &p.dc, &p.da }) {
		d->mods = 1u << int(Mod::V34) | 1u << int(Mod::V32B) |
			1u << int(Mod::V22B) | 1u << int(Mod::V22);
		d->mod = Mod::V22;
		d->rate = 1200;
	}
	p.start();
	p.run(3);
	CHECK(p.lc.mnp().mnp10_ext() && p.la.mnp().mnp10_ext(),
	      "the extension");
	p.dc.to_send = text(400000);
	p.da.to_send = text(200000);
	for (int i = 0; i < 40 && p.dc.rate < 33600; i++)
		p.run(30);
	CHECK(p.dc.mod == Mod::V34 && p.dc.rate == 33600,
	      "V.34 33600 (%d, %d)", int(p.dc.mod), p.dc.rate);
	CHECK(p.da.mod == p.dc.mod && p.da.rate == p.dc.rate, "both alike");
	bool restart = false, v22b = false;
	for (auto &s : p.dc.shifts) {
		restart |= s.kind == ShiftKind::Restart;
		v22b |= s.mod == Mod::V22B;
	}
	CHECK(restart && v22b, "start-ups, through V.22bis");
	p.run(120);
	CHECK(p.da.got == p.dc.to_send && p.dc.got == p.da.to_send,
	      "data intact (%zu, %zu)", p.da.got.size(), p.dc.got.size());
}

/* one side without the extension: the call stays in its class */
TEST(Link, Mnp10NoExtension)
{
	M10Pair p(m10_config(true), m10_config(false));
	for (M10Dte *d : { &p.dc, &p.da }) {
		d->mods = 1u << int(Mod::V34) | 1u << int(Mod::V32B) |
			1u << int(Mod::V22B) | 1u << int(Mod::V22);
		d->mod = Mod::V22;
		d->rate = 1200;
	}
	p.start();
	p.run(3);
	CHECK(p.lc.mnp().mnp10() && !p.lc.mnp().mnp10_ext(),
	      "MNP 10 without the extension");
	p.dc.to_send = text(100000);
	for (int i = 0; i < 20; i++)
		p.run(30);
	CHECK(p.dc.mod != Mod::V34 && p.dc.rate <= 14400,
	      "in the V.32bis class (%d)", p.dc.rate);
	CHECK(p.dc.rate == 14400, "at its top (%d)", p.dc.rate);
}

/* cellular: the probes carry G and E, the transmit level moves */
TEST(Link, Mnp10Cellular)
{
	Config cc = m10_config();
	cc.mnp10_cellular = true;
	M10Pair p(cc, m10_config());
	for (M10Dte *d : { &p.dc, &p.da })
		d->mods = 1u << int(Mod::V32B) | 1u << int(Mod::V22B) |
			1u << int(Mod::V22);
	p.start();
	p.run(3);
	CHECK(p.lc.mnp().cellular() && p.la.mnp().cellular(),
	      "cellular both ways");
	CHECK(p.dc.level >= 0 && p.dc.level <= 25 && p.da.level >= 0 &&
	      p.da.level <= 25, "levels (%d, %d)", p.dc.level, p.da.level);
	p.dc.to_send = text(20000);
	p.run(20);
	CHECK(p.da.got == p.dc.to_send, "data");
}

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


/*
 *    V.70's DSVD: H.245's messages (V.75), V.76's multiplexer
 *
 */

/* the encodings of an ASN.1 compiler's (Python's asn1tools, aligned PER,
   H.245's module) */
static const std::vector<u8> OLC_DATA = { 0x03, 0x40, 0x00, 0x00, 0x40,
	0x00, 0x00, 0x40, 0xc5, 0x01, 0x50, 0x41, 0x00, 0x7f, 0x00, 0x1c,
	0x05, 0x03, 0x14, 0x01, 0x50, 0x42, 0x00, 0x7f, 0x00, 0x1c, 0x00 };
static const std::vector<u8> OLC_AUDIO = { 0x03, 0x40, 0x00, 0x01, 0x40,
	0x00, 0x01, 0x35, 0x80, 0x00, 0x41, 0x00, 0x09, 0x02, 0x26, 0xb0,
	0x00, 0x42, 0x00, 0x09, 0x02, 0x00 };
/* a data channel on 7 with no port, forwardLogicalChannelDependency
   (an extension addition), N401 256, k 7, a 32 bit FCS */
static const std::vector<u8> OLC_EXT = { 0x03, 0x40, 0x00, 0x06, 0x90,
	0x31, 0x40, 0x01, 0x50, 0x42, 0x00, 0xff, 0x00, 0x0c, 0x00, 0x30,
	0x02, 0x00, 0x02, 0x50, 0x31, 0x40, 0x01, 0x50, 0x44, 0x00, 0xff,
	0x00, 0x0c, 0x00 };
static const std::vector<u8> TCS = { 0x02, 0x00, 0x05, 0x06, 0x00, 0x08,
	0x81, 0x75, 0x00, 0x01 };

/* a message of type t on channel lcn */
static h245::Msg h245_msg(h245::Type t, u16 lcn)
{
	h245::Msg m;
	m.type = t;
	m.lcn = lcn;
	return m;
}

/* the messages against the compiler's encodings, and back */
TEST(Link, H245Messages)
{
	h245::Msg m = h245_msg(h245::Type::Olc, 1);
	m.ch.lcn = 1;
	m.ch.port = 0;
	m.ch.max_rate = 336;
	CHECK(h245::encode(m) == OLC_DATA, "OpenLogicalChannel (data)");
	h245::Msg back;
	CHECK(h245::decode(OLC_DATA.data(), OLC_DATA.size(), &back) &&
	      back.type == h245::Type::Olc && back.lcn == 1 &&
	      back.ch.port == 0 && back.ch.kind == h245::Kind::Data &&
	      back.ch.proto == h245::Proto::V14Buffered &&
	      back.ch.max_rate == 336 && back.ch.fwd.erm &&
	      back.ch.fwd.window == 15 && back.ch.fwd.n401 == 128 &&
	      back.ch.fwd.crc == 16 && back.ch.has_rev && back.ch.rev.erm,
	      "OpenLogicalChannel (data) read");

	m.ch.lcn = 2;
	m.ch.port = 1;
	m.ch.kind = h245::Kind::Audio;
	m.ch.frames = 1;
	m.ch.fwd.erm = m.ch.rev.erm = false;
	m.ch.fwd.n401 = m.ch.rev.n401 = 10;
	CHECK(h245::encode(m) == OLC_AUDIO, "OpenLogicalChannel (audio)");
	CHECK(h245::decode(OLC_AUDIO.data(), OLC_AUDIO.size(), &back) &&
	      back.ch.kind == h245::Kind::Audio && back.ch.frames == 1 &&
	      !back.ch.fwd.erm && back.ch.fwd.n401 == 10 &&
	      !back.ch.rev.erm, "OpenLogicalChannel (audio) read");

	struct {
		h245::Msg m;
		std::vector<u8> want;
	} cases[] = {
		{ h245_msg(h245::Type::OlcAck, 1), { 0x22, 0x80, 0x00, 0x00 } },
		{ h245_msg(h245::Type::OlcReject, 2),
		  { 0x23, 0x00, 0x00, 0x01, 0x20 } },
		{ h245_msg(h245::Type::Clc, 1),
		  { 0x04, 0x00, 0x00, 0x00, 0x80 } },
		{ h245_msg(h245::Type::ClcAck, 1), { 0x23, 0x80, 0x00, 0x00 } },
		{ h245_msg(h245::Type::TcsAck, 1), { 0x21, 0x80, 0x00 } },
		{ h245_msg(h245::Type::TcsReject, 1), { 0x22, 0x00, 0x05, 0x00 } },
	};
	cases[1].m.cause = h245::TYPE_NOT_SUPPORTED;
	cases[2].m.lcse = true;
	cases[5].m.seq = 5;
	for (auto &c : cases) {
		CHECK(h245::encode(c.m) == c.want, "%s",
		      h245::type_name(c.m.type));
		CHECK(h245::decode(c.want.data(), c.want.size(), &back) &&
		      back.type == c.m.type && back.lcn == c.m.lcn &&
		      back.cause == c.m.cause && back.seq == c.m.seq &&
		      back.lcse == c.m.lcse, "%s read",
		      h245::type_name(c.m.type));
	}
	CHECK(h245::decode(TCS.data(), TCS.size(), &back) &&
	      back.type == h245::Type::Tcs && back.seq == 5,
	      "TerminalCapabilitySet's number");

	/* V.75's user data: FI 133 first */
	std::vector<u8> u = h245::user_data(cases[0].m);
	CHECK(u.size() == 5 && u[0] == 133, "FI 133");
	CHECK(h245::from_user_data(u.data(), u.size(), &back) &&
	      back.type == h245::Type::OlcAck, "user data read");
	u[0] = 130;
	CHECK(!h245::from_user_data(u.data(), u.size(), &back),
	      "user data without FI 133");
}

/* a later encoder's extension additions passed over; a message cut
   short anywhere is not one */
TEST(Link, H245Extensions)
{
	h245::Msg m;
	CHECK(h245::decode(OLC_EXT.data(), OLC_EXT.size(), &m) &&
	      m.type == h245::Type::Olc && m.ch.lcn == 7 &&
	      m.ch.port == -1 && m.ch.kind == h245::Kind::Data &&
	      m.ch.fwd.n401 == 256 && m.ch.fwd.window == 7 &&
	      m.ch.fwd.crc == 32 && m.ch.rev.crc == 32,
	      "OpenLogicalChannel with an extension addition");
	for (size_t n = 0; n < OLC_DATA.size(); n++)
		CHECK(!h245::decode(OLC_DATA.data(), n, &m),
		      "OpenLogicalChannel cut to %zu octets", n);
	for (size_t n = 0; n < OLC_EXT.size(); n++)
		CHECK(!h245::decode(OLC_EXT.data(), n, &m),
		      "OpenLogicalChannel (extended) cut to %zu octets", n);
}

/* the data pumps have connected after V.8 bis chose DSVD: V.76 both
   ends, the answerer the initiator */
static void start_v76(Pair &p)
{
	p.lc.start_v76(false);
	p.la.start_v76(true);
}

/* the data channel opened by the initiator, the data both ways (with
   bit errors too), the responder's DISC */
TEST(Link, V76)
{
	rnd_state = 12345;
	Pair p(lapm_config(3), lapm_config(3));
	start_v76(p);
	p.run(9600);
	CHECK(p.dc.detected && p.da.detected, "detected");
	CHECK(p.lc.protocol() == Protocol::V76 &&
	      p.la.protocol() == Protocol::V76, "V.76");
	CHECK(p.dc.up && p.da.up, "the data channel up");
	CHECK(p.lc.config().comp == 0 && p.la.config().comp == 0,
	      "no compression");
	transfer("v76", p, 20000, 15000, 9600 * 40);
	p.ber = 1e-4;
	transfer("v76, bit errors", p, 10000, 10000, 9600 * 60);
	p.ber = 0;
	p.lc.disconnect();
	p.run(9600 * 2);
	CHECK(p.da.released, "initiator released after the DISC");
	CHECK(p.dc.released, "responder released after the UA");
}

/* a U frame (its address, control and user data) */
static std::vector<u8> u_frame(u8 addr, u8 ctrl, const h245::Msg *m)
{
	std::vector<u8> f = { addr, ctrl };
	if (m) {
		std::vector<u8> u = h245::user_data(*m);
		f.insert(f.end(), u.begin(), u.end());
	}
	return f;
}

/* the frame among out with address addr and control ctrl, its message
   read into m; false if there is none */
static bool find_u(const std::vector<std::vector<u8>> &out, u8 addr,
		   u8 ctrl, h245::Msg *m)
{
	for (const std::vector<u8> &f : out) {
		if (f.size() < 2 || f[0] != addr || f[1] != ctrl)
			continue;
		*m = h245::Msg();
		if (f.size() > 2)
			h245::from_user_data(f.data() + 2, f.size() - 2, m);
		return true;
	}
	return false;
}

/* the responder and the initiator's frames: an audio channel, a DISC
   on a DLC not open, a capability set, a data channel with SREJ's
   parameters it cannot take; then the data channel (on 63) taken, and a
   second one refused (the initiator's commands and the responder's
   responses have C/R 1) */
TEST(Link, V76Refused)
{
	Dte d("responder");
	Link l;
	l.init(&d, true, lapm_config(0));
	l.start_v76(false);

	h245::Msg audio;
	CHECK(h245::decode(OLC_AUDIO.data(), OLC_AUDIO.size(), &audio),
	      "audio");
	h245::Msg data;
	CHECK(h245::decode(OLC_DATA.data(), OLC_DATA.size(), &data), "data");
	h245::Msg crc32;
	CHECK(h245::decode(OLC_EXT.data(), OLC_EXT.size(), &crc32), "crc32");
	std::vector<u8> xid = { 0x0b, 0xbf, 133 };
	xid.insert(xid.end(), TCS.begin(), TCS.end());
	std::vector<std::vector<u8>> out = exchange(l, {
		u_frame(5 << 2 | 3, 0x7f, &audio),      /* SABME, P 1 */
		u_frame(9 << 2 | 3, 0x53, nullptr),     /* DISC, P 1 */
		xid,                                    /* XID on DLCI 2 */
		u_frame(4 << 2 | 3, 0x7f, &crc32),
	}, 8);
	h245::Msg m;
	CHECK(find_u(out, 5 << 2 | 3, 0x1f, &m) &&
	      m.type == h245::Type::OlcReject && m.lcn == 2 &&
	      m.cause == h245::TYPE_NOT_SUPPORTED,
	      "DM with OpenLogicalChannelReject for audio");
	CHECK(find_u(out, 9 << 2 | 3, 0x1f, &m), "DM for the DISC");
	CHECK(find_u(out, 0x0b, 0xbf, &m) &&
	      m.type == h245::Type::TcsReject && m.seq == 5,
	      "XID with TerminalCapabilitySetReject");
	CHECK(find_u(out, 4 << 2 | 3, 0x1f, &m) &&
	      m.type == h245::Type::OlcReject && m.lcn == 7,
	      "DM for a 32 bit FCS");
	CHECK(!d.up, "up without a data channel");

	out = exchange(l, {
		u_frame(63 << 2 | 3, 0x7f, &data),
		u_frame(62 << 2 | 3, 0x7f, &data),
	}, 8);
	CHECK(find_u(out, 63 << 2 | 3, 0x73, &m) &&
	      m.type == h245::Type::OlcAck && m.lcn == 1,
	      "UA with OpenLogicalChannelAck");
	CHECK(d.up && l.protocol() == Protocol::V76, "the data channel up");
	CHECK(find_u(out, 62 << 2 | 3, 0x1f, &m) &&
	      m.type == h245::Type::OlcReject &&
	      m.cause == h245::TYPE_NOT_AVAILABLE,
	      "a second data channel refused");
}

/* a DTE with a voice path: a tone of freq Hz for its speech, the far
   end's kept, at a V.34 rate */
struct VoiceDte : Dte {
	double freq;
	long sent = 0;
	long talk = -1;         /* samples of tone, then quiet (-1: always) */
	u32 rnd = 1;
	bool on = false;
	bool opens = true;      /* opens a voice channel itself */
	int opened = 0, closed = 0;
	std::vector<s16> heard;
	/* the DTE called n, its tone */
	VoiceDte(const char *n, double f) : Dte(n), freq(f) {}
	/* a voice channel's rate */
	int32_t tx_rate() override { return 28800; }
	/* (it has a voice path) */
	bool voice() override { return true; }
	/* (see opens) */
	bool voice_opens() override { return opens; }
	/* the channel open or closed */
	void voice_state(bool o) override {
		on = o;
		(o ? opened : closed)++;
	}
	/* the tone (for talk samples), then a quiet room's noise */
	s16 voice_in(s16 *pcm, s16 n) override {
		for (s16 i = 0; i < n; i++, sent++) {
			if (talk < 0 || sent < talk) {
				pcm[i] = static_cast<s16>(8000 * sin(2 * M_PI *
						freq * sent / 8000));
				continue;
			}
			rnd = rnd * 1103515245u + 12345u;
			pcm[i] = static_cast<s16>(static_cast<int>(rnd >> 16 &
								   0x7f) - 64);
		}
		return n;
	}
	/* the energy of what was heard from sample a to b, a mean square */
	double power(size_t a, size_t b) const {
		double e = 0;
		for (size_t i = a; i < b && i < heard.size(); i++)
			e += double(heard[i]) * heard[i];
		return b > a ? e / double(b - a) : 0;
	}
	/* the far end's */
	void voice_out(const s16 *pcm, s16 n) override {
		heard.insert(heard.end(), pcm, pcm + n);
	}
	/* the share of what was heard (after the first 100 ms) at f Hz,
	   by its energy */
	double tone_share(double f) const {
		double c = 0, s = 0, all = 0;
		for (size_t i = 800; i < heard.size(); i++) {
			c += heard[i] * cos(2 * M_PI * f * i / 8000);
			s += heard[i] * sin(2 * M_PI * f * i / 8000);
			all += double(heard[i]) * heard[i];
		}
		const double n = double(heard.size() - 800);
		return all > 0 ? 2 * (c * c + s * s) / n / all : 0;
	}
};

/* two Links back to back whose DTEs have a voice path */
struct VoicePair {
	VoiceDte dc{"caller", 440}, da{"answerer", 700};
	Link lc, la;
	/* a DSVD call, the answerer the initiator; it opens a voice
	   channel if initiator_opens, else waits for the caller's */
	explicit VoicePair(bool initiator_opens = true) {
		da.opens = initiator_opens;
		lc.init(&dc, true, lapm_config(0));
		la.init(&da, false, lapm_config(0));
		const char *v = getenv("LINK_DEBUG");
		dc.verbose = da.verbose = v && *v;
		lc.start_v76(false);
		la.start_v76(true);
	}
	/* bits both ways */
	void run(long bits, int chunk = 48) {
		std::vector<uint8_t> b(chunk);
		for (long k = 0; k < bits; k += chunk) {
			lc.get_bits(1, b.data(), chunk);
			la.put_bits(1, b.data(), chunk);
			la.get_bits(1, b.data(), chunk);
			lc.put_bits(1, b.data(), chunk);
			if (dc.up)
				lc.set_online(true);
			if (da.up)
				la.set_online(true);
		}
	}
};

/* the voice channel beside the data channel: opened by the initiator on
   DLCI 1, G.729 Annex A both ways at 100 frames a second of the line's
   bits, each side hearing the other's tone, the data still going; the
   hang-up closes it */
TEST(Link, V76Voice)
{
	VoicePair p;
	p.run(28800);
	CHECK(p.dc.up && p.da.up, "the data channel up");
	CHECK(p.dc.on && p.da.on, "the voice channel open (%d, %d)", p.dc.on,
	      p.da.on);
	p.dc.heard.clear();
	p.da.heard.clear();

	/* two seconds of the line's bits: two seconds of speech each way
	   (about 12 kbit/s of the 28.8 with the frames' overhead), and
	   data meanwhile */
	p.dc.to_send = text(2500);
	p.da.to_send = text(2000);
	p.run(28800 * 2);
	printf("  voice: caller heard %zu samples, answerer %zu; data %zu + "
	       "%zu octets\n", p.dc.heard.size(), p.da.heard.size(),
	       p.da.got.size(), p.dc.got.size());
	CHECK(p.dc.heard.size() >= 15000 && p.dc.heard.size() <= 16100,
	      "the caller heard %zu samples in 2 s", p.dc.heard.size());
	CHECK(p.da.heard.size() >= 15000 && p.da.heard.size() <= 16100,
	      "the answerer heard %zu samples in 2 s", p.da.heard.size());
	const double c = p.dc.tone_share(700), a = p.da.tone_share(440);
	printf("  voice: the far end's tone %.0f %% (caller), %.0f %% "
	       "(answerer)\n", 100 * c, 100 * a);
	CHECK(c > 0.8, "the caller hears 700 Hz (%.2f)", c);
	CHECK(a > 0.8, "the answerer hears 440 Hz (%.2f)", a);
	CHECK(p.da.got == p.dc.to_send && p.dc.got == p.da.to_send,
	      "the data beside it (%zu, %zu)", p.da.got.size(),
	      p.dc.got.size());

	p.la.disconnect();
	p.run(28800);
	CHECK(!p.dc.on && !p.da.on && p.dc.closed == 1 && p.da.closed == 1,
	      "the voice channel closed on the hang-up");
	CHECK(p.dc.released && p.da.released, "released");
}

/* a voice channel the responder opens (the initiator, a PBX's, waits
   for one: voice_opens() false), a second after the data channel: each
   side hears the other's tone; the initiator closes it (its extension
   gone), the data going on */
TEST(Link, V76VoiceResponderOpens)
{
	VoicePair p(false);
	p.run(14400);
	CHECK(p.dc.up && p.da.up, "the data channel up");
	CHECK(!p.dc.on && !p.da.on, "no voice channel yet");
	p.run(28800 * 2);
	CHECK(p.dc.on && p.da.on, "the voice channel open (%d, %d)", p.dc.on,
	      p.da.on);
	p.dc.heard.clear();
	p.da.heard.clear();
	p.run(28800 * 2);
	const double c = p.dc.tone_share(700), a = p.da.tone_share(440);
	printf("  voice: caller heard %zu samples (700 Hz %.0f %%), answerer "
	       "%zu (440 Hz %.0f %%)\n", p.dc.heard.size(), 100 * c,
	       p.da.heard.size(), 100 * a);
	CHECK(c > 0.8 && a > 0.8, "each hears the other's tone");

	p.la.close_voice();
	p.run(28800);
	CHECK(!p.dc.on && !p.da.on && p.dc.closed == 1 && p.da.closed == 1,
	      "the voice channel closed (%d, %d)", p.dc.on, p.da.on);
	p.dc.to_send = text(2000);
	p.da.to_send = text(2000);
	p.run(28800 * 2);
	CHECK(p.dc.up && p.da.up && p.da.got == p.dc.to_send &&
	      p.dc.got == p.da.to_send, "the data goes on (%zu, %zu)",
	      p.da.got.size(), p.dc.got.size());
}

/* silence on the voice channel (G.729 Annex B, the audio header both
   ways between us): a side that stops talking sends a SID frame and
   then little else, so its data has more of the line; the far end
   still hears 10 ms every 10 ms, comfort noise near the quiet room's
   level rather than the tone */
TEST(Link, V76VoiceSilence)
{
	VoicePair talk, quiet;
	quiet.dc.talk = 0;
	for (VoicePair *p : { &talk, &quiet }) {
		p->run(28800);
		CHECK(p->dc.on && p->da.on, "the voice channel open");
		p->dc.heard.clear();
		p->da.heard.clear();
		p->dc.to_send = text(20000);
		p->run(28800 * 2);
	}
	const VoiceDte &a = quiet.da;
	printf("  silence: the answerer heard %zu samples; data %zu octets "
	       "(%zu while talking)\n", a.heard.size(), a.got.size(),
	       talk.da.got.size());
	CHECK(a.heard.size() >= 15000 && a.heard.size() <= 16100,
	      "comfort noise all along (%zu samples in 2 s)", a.heard.size());
	/* the room's noise: uniform in -64..63, a mean square of ~1365 */
	const double e = a.power(800, a.heard.size());
	printf("  silence: comfort noise's mean square %.0f (the room's "
	       "1365)\n", e);
	CHECK(e > 1365 / 10.0 && e < 1365 * 10.0,
	      "comfort noise within 10 dB of the room (%.0f)", e);
	CHECK(a.tone_share(440) < 0.05, "no tone (%.2f)", a.tone_share(440));
	CHECK(a.got.size() > talk.da.got.size() * 3 / 2,
	      "the data has the line in silence (%zu octets against %zu)",
	      a.got.size(), talk.da.got.size());
	/* the other way, speech as before */
	CHECK(quiet.dc.tone_share(700) > 0.8, "the caller hears 700 Hz");
}

/* the responder with a voice path takes an audio channel with V.75's
   audio header (clause 9) both ways: its frames carry the header and
   their sequence numbers; a second audio channel, or one in ERM, is
   refused; the remote's DISC closes it */
TEST(Link, V76VoiceHeader)
{
	VoiceDte d("responder", 500);
	Link l;
	l.init(&d, true, lapm_config(0));
	l.start_v76(false);

	h245::Msg m;
	m.type = h245::Type::Olc;
	m.lcn = m.ch.lcn = 3;
	m.ch.port = 1;
	m.ch.kind = h245::Kind::Audio;
	m.ch.fwd.erm = m.ch.rev.erm = false;
	m.ch.fwd.n401 = m.ch.rev.n401 = 11;
	m.ch.fwd.audio_header = m.ch.rev.audio_header = true;
	h245::Msg erm = m;
	erm.ch.fwd.erm = true;
	/* our speech: 3 frames of the encoder's, sequence numbers 0, 1 and
	   then 4 (two lost) */
	g729::Encoder enc;
	std::vector<std::vector<u8>> speech;
	for (u8 seq : { 0, 1, 4 }) {
		s16 pcm[g729::FRAME];
		for (int i = 0; i < g729::FRAME; i++)
			pcm[i] = static_cast<s16>(6000 * sin(2 * M_PI * 500 *
							     (seq * 80 + i) / 8000.0));
		std::vector<u8> f = { 2 << 2 | 3, 0x03,
				      static_cast<u8>(seq << 2) };
		f.resize(3 + g729::FRAME_OCTETS);
		enc.encode(pcm, f.data() + 3);
		speech.push_back(f);
	}
	std::vector<std::vector<std::vector<u8>>> rounds = {
		{ u_frame(4 << 2 | 3, 0x7f, &erm), u_frame(2 << 2 | 3, 0x7f, &m),
		  u_frame(3 << 2 | 3, 0x7f, &m) },
		speech,
	};
	std::vector<std::vector<u8>> out = exchange(l, rounds[0], 8);
	h245::Msg r;
	CHECK(find_u(out, 2 << 2 | 3, 0x73, &r) &&
	      r.type == h245::Type::OlcAck && r.lcn == 3,
	      "UA with OpenLogicalChannelAck for the audio channel");
	CHECK(d.on && d.opened == 1, "the voice channel open");
	CHECK(find_u(out, 3 << 2 | 3, 0x1f, &r) &&
	      r.type == h245::Type::OlcReject &&
	      r.cause == h245::TYPE_NOT_AVAILABLE, "a second one refused");
	CHECK(find_u(out, 4 << 2 | 3, 0x1f, &r) &&
	      r.type == h245::Type::OlcReject &&
	      r.cause == h245::UNSUITABLE_REVERSE, "one in ERM refused");

	out = exchange(l, rounds[1], 40);
	CHECK(d.heard.size() == 5 * g729::FRAME,
	      "3 frames and 2 lost ones heard (%zu samples)", d.heard.size());
	/* our frames: the responder's commands have C/R 0; the header's
	   sequence numbers count up */
	int n = 0, last = -1;
	for (const std::vector<u8> &f : out)
		if (f.size() == 3 + g729::FRAME_OCTETS && f[0] == (2 << 2 | 1) &&
		    f[1] == 0x03) {
			CHECK(last < 0 || f[2] == ((last + 1) & 0x1f) << 2,
			      "frame %d's header %02x after %d", n, f[2], last);
			last = f[2] >> 2;
			n++;
		}
	CHECK(n > 10, "speech sent with the header (%d frames)", n);

	h245::Msg clc;
	clc.type = h245::Type::Clc;
	clc.lcn = 3;
	out = exchange(l, { u_frame(2 << 2 | 3, 0x53, &clc) }, 4);
	CHECK(find_u(out, 2 << 2 | 3, 0x73, &r) &&
	      r.type == h245::Type::ClcAck, "UA with CloseLogicalChannelAck");
	CHECK(!d.on && d.closed == 1, "the voice channel closed");
}
