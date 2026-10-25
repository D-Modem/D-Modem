/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    zstd_ref_test.cpp - the link's zstd (zstd_codec.h) against the
 *                        reference library, libzstd, both ways: our
 *                        frames through its decoder, its frames (levels
 *                        -5 to 19, with checksums) through ours, without
 *                        a dictionary, with a raw one and with V.cvm's
 *                        (zstd_dict.h); built where libzstd is found
 *
 */

#include <stdio.h>
#include <string.h>

#include <vector>

#include <gtest/gtest.h>
#include <zstd.h>

#include "zstd_codec.h"
#include "zstd_dict.h"
#include "v44_preset.h"

using B = std::vector<unsigned char>;

static unsigned rs = 1;
/* a repeatable pseudo-random number (rs), 16 bits */
static unsigned rnd()
{
	rs = rs * 1103515245 + 12345;
	return rs >> 16;
}

/* text-like data: n characters of a few words and lines */
static B text(size_t n)
{
	static const char *w[] = { "the ", "modem ", "data ", "call ", "of ",
		"and ", "link ", "compression ", "zstd ", "window ", "\r\n",
		"HTTP/1.1 200 OK\r\n", "Content-Type: text/html\r\n",
		"<div class=\"x\">", "</div>" };
	B o;
	while (o.size() < n) {
		const char *s = w[rnd() % 15];
		o.insert(o.end(), s, s + strlen(s));
	}
	o.resize(n);
	return o;
}

/* n random octets */
static B noise(size_t n)
{
	B o(n);
	for (auto &c : o)
		c = static_cast<unsigned char>(rnd());
	return o;
}

/* binary-like data: a pattern, every 7th octet random */
static B binary(size_t n)
{
	B o(n);
	for (size_t i = 0; i < n; i++)
		o[i] = i % 7 == 0 ? static_cast<unsigned char>(rnd()) :
			static_cast<unsigned char>(i * 13 % 251);
	return o;
}

/* a formatted dictionary (by its magic number) */
static bool formatted(const B *d)
{
	return d && d->size() > 4 && (*d)[0] == 0x37 && (*d)[1] == 0xA4 &&
	       (*d)[2] == 0x30 && (*d)[3] == 0xEC;
}

/* in through our encoder (a block every flush_every characters) as a
   whole frame */
static B ours_enc(const B &in, const B *dict, int flush_every)
{
	dlink::Zstd z;
	EXPECT_EQ(z.init(15, dict ? dict->data() : nullptr,
			 dict ? static_cast<s32>(dict->size()) : 0), 0);
	B o;
	unsigned char b[4096];
	int n;
	for (size_t i = 0; i < in.size(); i++) {
		z.encode(in[i]);
		if (flush_every && (i + 1) % flush_every == 0)
			z.flush_encoder();
		while ((n = z.out(b, sizeof(b))) > 0)
			o.insert(o.end(), b, b + n);
	}
	z.finish();
	while ((n = z.out(b, sizeof(b))) > 0)
		o.insert(o.end(), b, b + n);
	return o;
}

/* c through libzstd's decoder; false on its error */
static bool ref_dec(const B &c, const B *dict, B *out)
{
	ZSTD_DCtx *d = ZSTD_createDCtx();
	if (dict && formatted(dict))
		ZSTD_DCtx_loadDictionary(d, dict->data(), dict->size());
	else if (dict)
		ZSTD_DCtx_refPrefix(d, dict->data(), dict->size());
	out->assign(4 << 20, 0);
	size_t r = ZSTD_decompressDCtx(d, out->data(), out->size(), c.data(),
				       c.size());
	ZSTD_freeDCtx(d);
	if (ZSTD_isError(r)) {
		printf("  libzstd: %s\n", ZSTD_getErrorName(r));
		return false;
	}
	out->resize(r);
	return true;
}

/* in through libzstd's encoder at level, with a checksum (ck) */
static B ref_enc(const B &in, int level, bool ck, const B *dict)
{
	ZSTD_CCtx *c = ZSTD_createCCtx();
	ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
	ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, ck);
	if (dict && formatted(dict))
		ZSTD_CCtx_loadDictionary(c, dict->data(), dict->size());
	else if (dict)
		ZSTD_CCtx_refPrefix(c, dict->data(), dict->size());
	B o(ZSTD_compressBound(in.size()));
	size_t r = ZSTD_compress2(c, o.data(), o.size(), in.data(), in.size());
	ZSTD_freeCCtx(c);
	EXPECT_FALSE(ZSTD_isError(r)) << ZSTD_getErrorName(r);
	o.resize(ZSTD_isError(r) ? 0 : r);
	return o;
}

/* c through our decoder, an octet at a time; false on its error */
static bool ours_dec(const B &c, const B *dict, B *out)
{
	dlink::Zstd z;
	z.init(23, dict ? dict->data() : nullptr,
	       dict ? static_cast<s32>(dict->size()) : 0);
	unsigned char b[4096];
	int n;
	out->clear();
	for (unsigned char x : c) {
		if (z.decode(x) < 0)
			return false;
		while ((n = z.out(b, sizeof(b))) > 0)
			out->insert(out->end(), b, b + n);
	}
	return true;
}

/* the data sets: text, noise, binary, repeats, a mix, a little */
static std::vector<std::pair<const char *, B>> inputs()
{
	rs = 1;
	B rep(30000), mix = text(20000), z = noise(20000), t = text(20000);
	for (size_t i = 0; i < rep.size(); i++)
		rep[i] = static_cast<unsigned char>("abababababcabcabc"[i % 17]);
	mix.insert(mix.end(), z.begin(), z.end());
	mix.insert(mix.end(), t.begin(), t.end());
	return { { "text", text(200000) }, { "noise", noise(30000) },
		 { "binary", binary(50000) }, { "repeats", rep },
		 { "mixed", mix }, { "small", text(300) },
		 { "one", B(1, 'x') }, { "rle", B(5000, 'q') } };
}

/* our frames through libzstd: no dictionary, raw, V.cvm's; flushes
   none, every 100 and every 1 (a little) */
TEST(ZstdRef, OursToLibzstd)
{
	const B raw = dlink::v44_preset(), &fd = dlink::zstd_dict();
	const B *dicts[3] = { nullptr, &raw, &fd };
	for (auto &[name, in] : inputs())
		for (const B *d : dicts)
			for (int fe : { 0, 100, 1 }) {
				if (fe == 1 && in.size() > 5000)
					continue;
				B c = ours_enc(in, d, fe), o;
				EXPECT_TRUE(ref_dec(c, d, &o) && o == in)
					<< name << ", dictionary "
					<< (d == &fd ? "V.cvm" : d ? "raw" : "none")
					<< ", flush " << fe;
			}
}

/* libzstd's frames through ours: levels -5 to 19, checksums, the same
   dictionaries */
TEST(ZstdRef, LibzstdToOurs)
{
	const B raw = dlink::v44_preset(), &fd = dlink::zstd_dict();
	const B *dicts[3] = { nullptr, &raw, &fd };
	for (auto &[name, in] : inputs())
		for (const B *d : dicts)
			for (int lvl : { -5, 1, 3, 9, 19 }) {
				B c = ref_enc(in, lvl, lvl & 1, d), o;
				EXPECT_TRUE(ours_dec(c, d, &o) && o == in)
					<< name << ", level " << lvl
					<< ", dictionary "
					<< (d == &fd ? "V.cvm" : d ? "raw" : "none");
			}
}

/* V.cvm's dictionary as libzstd reads it: its ID and header size */
TEST(ZstdRef, Dictionary)
{
	const B &fd = dlink::zstd_dict();
	EXPECT_EQ(ZSTD_getDictID_fromDict(fd.data(), fd.size()),
		  dlink::ZSTD_DICT_ID);
	ZSTD_CDict *cd = ZSTD_createCDict(fd.data(), fd.size(), 3);
	EXPECT_NE(cd, nullptr) << "libzstd's compressor takes it";
	ZSTD_freeCDict(cd);
}
