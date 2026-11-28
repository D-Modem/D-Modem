/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jbig_test.cpp - dsp/jbig.h against T.82 clause 7: the arithmetic
 *                    coder's test sequence (7.1) and the byte counts of
 *                    the artificial image's three sequential codings
 *                    (7.2.1, Table 29); round trips through the decoder,
 *                    and the marker segments T.85 lets a sender use
 *
 */

#include <string.h>

#include <algorithm>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "jbig.h"

using namespace dsp;

namespace {

/* the bits of a hex string, the first the top bit of the first digit */
std::vector<int> hex_bits(const char *s)
{
	std::vector<int> v;
	for (; *s; s++) {
		if (*s == ' ')
			continue;
		int d = *s <= '9' ? *s - '0' : *s - 'a' + 10;
		for (int i = 3; i >= 0; i--)
			v.push_back((d >> i) & 1);
	}
	return v;
}

const char *pix_hex = "05e0 0000 8b00 01c4 1700 0034 7fff 1a3f 951b 05d8 1d17 e770 0000 0000 0656 0e6a";
const char *cx_hex = "0fe0 0000 0f00 00f0 ff00 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000";
const unsigned char scd_71[] = {
	0x69, 0x89, 0x99, 0x5c, 0x32, 0xea, 0xfa, 0xa0, 0xd5, 0xff, 0x52, 0x7f, 0xff,
	0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x3f, 0xff, 0x2d, 0x20, 0x82, 0x91,
};

/* T.82 Figure 38's artificial image, 1960 by 1951 */
void artificial(T4Page *p)
{
	p->init(1960, 1);
	u32 prsg = 1;
	int repeat[8] = {0};
	std::vector<unsigned char> row(T4_STRIDE(1960));
	for (int j = 0; j < 1951; j++) {
		memset(row.data(), 0, row.size());
		for (int i = 0; i < 1960; i++) {
			int pix;
			if (j < 192) {
				pix = 0;
			} else if (j < 1023 || ((i >> 3) & 3) == 0) {
				u32 sum = (prsg & 1) + ((prsg >> 2) & 1) + ((prsg >> 11) & 1) + ((prsg >> 15) & 1);
				prsg = ((prsg << 1) + (sum & 1)) & 0xffff;
				pix = (prsg & 3) == 0;
				repeat[i & 7] = pix;
			} else {
				pix = repeat[i & 7];
			}
			if (pix)
				row[i >> 3] |= (unsigned char)(0x80 >> (i & 7));
		}
		p->add_row(row.data());
	}
}

/* black pixels on a page */
long count_black(const T4Page *p)
{
	long n = 0;
	for (size_t i = 0; i < (size_t)p->rows * T4_STRIDE(p->width); i++)
		for (unsigned b = p->bits[i]; b; b &= b - 1)
			n++;
	return n;
}

/* the sizes in a BIE: stripe coded data (unescaped), and stripe data
   entities */
void sizes(const std::vector<unsigned char> &bie, long *scd, long *sde)
{
	*scd = *sde = 0;
	size_t i = 20;
	while (i < bie.size()) {
		if (bie[i] == 0xff && bie[i + 1] == 0x06) {
			i += 8;
			continue;
		}
		size_t start = i;
		for (;;) {
			if (bie[i] == 0xff && bie[i + 1] == 0x00) {
				(*scd)++;
				i += 2;
			} else if (bie[i] == 0xff) {
				i += 2;
				break;
			} else {
				(*scd)++;
				i++;
			}
		}
		*sde += (long)(i - start);
	}
}

/* two pages the same */
void same(const T4Page *a, const T4Page *b)
{
	ASSERT_EQ(a->width, b->width);
	ASSERT_EQ(a->rows, b->rows);
	EXPECT_EQ(memcmp(a->bits.data(), b->bits.data(), (size_t)a->rows * T4_STRIDE(a->width)), 0);
}

/* a page of text-like blocks with blank lines between */
void random_page(T4Page *p, s16 width, s32 rows, unsigned seed)
{
	std::mt19937 rng(seed);
	p->init(width, 1);
	std::vector<unsigned char> row(T4_STRIDE(width));
	for (s32 y = 0; y < rows; y++) {
		memset(row.data(), 0, row.size());
		if ((y / 20) % 3) {
			for (s16 x = 0; x < width; x++)
				if (rng() % 7 == 0)
					row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
			if (rng() % 4 == 0) {
				p->add_row(nullptr);
				continue;
			}
		}
		p->add_row(row.data());
	}
}

} // namespace

/* T.82 7.1: the encoder makes the SCD given, the decoder the pixels */
TEST(Jbig, CoderTestSequence)
{
	std::vector<int> pix = hex_bits(pix_hex), cx = hex_bits(cx_hex);
	ASSERT_EQ(pix.size(), 256u);
	std::vector<unsigned char> out;
	QmState st[2] = {0, 0};
	QmEnc enc;
	enc.init(&out);
	for (size_t i = 0; i < pix.size(); i++)
		enc.code(&st[cx[i]], (s16)pix[i]);
	enc.flush();
	while (!out.empty() && out.back() == 0x00)
		out.pop_back();
	ASSERT_EQ(out.size(), sizeof(scd_71));
	EXPECT_EQ(memcmp(out.data(), scd_71, sizeof(scd_71)), 0);

	QmState ds[2] = {0, 0};
	QmDec dec;
	dec.init(scd_71, sizeof(scd_71));
	for (size_t i = 0; i < pix.size(); i++)
		ASSERT_EQ(dec.decode(&ds[cx[i]]), pix[i]) << "pixel " << i;
}

/* T.82 7.2.1: the three codings of the artificial image, by Table 29's
   byte counts, and each decoded back */
TEST(Jbig, ArtificialImage)
{
	T4Page img;
	artificial(&img);
	ASSERT_EQ(count_black(&img), 861965);

	const JbigAtMove move = {9 * 128, 8};
	struct {
		JbigOptions opt;
		long scd, sde, bie;
	} cases[3];
	cases[0].opt.l0 = 1951;
	cases[0].opt.options = 0;
	cases[0].scd = 316094;
	cases[0].sde = 317364;
	cases[0].bie = 317384;
	cases[1].opt.l0 = 1951;
	cases[1].opt.options = JBG_LRLTWO;
	cases[1].scd = 315887;
	cases[1].sde = 317112;
	cases[1].bie = 317132;
	cases[2].opt.l0 = 128;
	cases[2].opt.options = JBG_TPBON;
	cases[2].opt.mx = 8;
	cases[2].opt.moves = &move;
	cases[2].opt.n_moves = 1;
	cases[2].scd = 252557;
	cases[2].sde = 253625;
	cases[2].bie = 253653;
	for (auto &c : cases) {
		std::vector<unsigned char> bie;
		jbig_encode(&img, c.opt, &bie);
		long scd, sde;
		sizes(bie, &scd, &sde);
		EXPECT_EQ(scd, c.scd);
		EXPECT_EQ(sde, c.sde);
		EXPECT_EQ((long)bie.size(), c.bie);
		if (c.opt.n_moves) {
			/* the ATMOVE before stripe 9: yAT 0, its first line */
			const unsigned char at[] = {0xff, 0x06, 0, 0, 0, 0, 8, 0};
			EXPECT_NE(std::search(bie.begin(), bie.end(), at, at + 8), bie.end());
		}

		T4Page out;
		out.fine = 1;
		ASSERT_EQ(jbig_decode(bie.data(), bie.size(), 10000, &out), 0);
		same(&img, &out);
	}
}

/* fax-like pages round trip in T.85's basic coding and its options */
TEST(Jbig, RoundTrip)
{
	for (unsigned seed = 1; seed <= 4; seed++) {
		T4Page img;
		random_page(&img, seed & 1 ? 1728 : 2432, 300 + (s32)seed * 37, seed);
		JbigOptions opt;
		if (seed == 2)
			opt.options |= JBG_LRLTWO;
		if (seed == 3) {
			opt.options = 0;
			opt.l0 = 7;
		}
		std::vector<unsigned char> bie;
		jbig_encode(&img, opt, &bie);
		T4Page out;
		ASSERT_EQ(jbig_decode(bie.data(), bie.size(), 10000, &out), 0) << "seed " << seed;
		same(&img, &out);
	}
}

/* T.85's unknown length: YD all ones, then NEWLEN after the last
   stripe, a comment between stripes; a cap on the lines; an early end */
TEST(Jbig, MarkersAndLimits)
{
	T4Page img;
	random_page(&img, 1728, 200, 9);
	std::vector<unsigned char> bie;
	jbig_encode(&img, JbigOptions(), &bie);

	/* YD = 0xffffffff, VLENGTH, NEWLEN 200 at the end, a comment after
	   the header */
	std::vector<unsigned char> v(bie.begin(), bie.begin() + 20);
	v[8] = v[9] = v[10] = v[11] = 0xff;
	v[19] |= JBG_VLENGTH;
	const unsigned char comment[] = {0xff, 0x07, 0, 0, 0, 3, 'a', 0xff, 'c'};
	v.insert(v.end(), comment, comment + sizeof(comment));
	v.insert(v.end(), bie.begin() + 20, bie.end());
	const unsigned char newlen[] = {0xff, 0x05, 0, 0, 0, 200};
	v.insert(v.end(), newlen, newlen + sizeof(newlen));
	T4Page out;
	ASSERT_EQ(jbig_decode(v.data(), v.size(), 10000, &out), 0);
	same(&img, &out);

	/* NEWLEN shortening the page into the last stripe */
	v.resize(v.size() - 1);
	v.push_back(150);
	ASSERT_EQ(jbig_decode(v.data(), v.size(), 10000, &out), 0);
	EXPECT_EQ(out.rows, 150);

	/* at most 100 lines kept */
	ASSERT_EQ(jbig_decode(bie.data(), bie.size(), 100, &out), 0);
	EXPECT_EQ(out.rows, 100);

	/* cut short: the lines so far */
	ASSERT_EQ(jbig_decode(bie.data(), bie.size() / 2, 10000, &out), 1);
	EXPECT_GT(out.rows, 0);
	EXPECT_LE(out.rows, 200);

	/* not T.85: two layers, deterministic prediction, no header */
	std::vector<unsigned char> w = bie;
	w[1] = 1;
	EXPECT_EQ(jbig_decode(w.data(), w.size(), 10000, &out), -1);
	w = bie;
	w[19] |= JBG_DPON;
	EXPECT_EQ(jbig_decode(w.data(), w.size(), 10000, &out), -1);
	EXPECT_EQ(jbig_decode(bie.data(), 10, 10000, &out), -1);

	/* garbage after the header ends without a crash */
	std::mt19937 rng(5);
	for (int i = 0; i < 50; i++) {
		w.assign(bie.begin(), bie.begin() + 20);
		for (int k = 0; k < 2000; k++)
			w.push_back((unsigned char)rng());
		s16 r = jbig_decode(w.data(), w.size(), 10000, &out);
		EXPECT_TRUE(r >= -1 && r <= 1);
	}
}

namespace {

/* a page as a one plane image */
JbigImage page_image(const T4Page *p)
{
	JbigImage img;
	img.width = (u32)p->width;
	img.height = (u32)p->rows;
	img.planes = 1;
	img.bits.emplace_back(p->bits.begin(), p->bits.begin() + (long)p->rows * T4_STRIDE(p->width));
	return img;
}

/* the sizes of a BIE's SDEs, by the layer each belongs to, for the
   order "d outer" (SEQ 0, ILEAVE 0 or 1, one plane): sizes(), but for
   each layer of stripes SDEs */
void layer_sizes(const std::vector<unsigned char> &bie, size_t stripes, std::vector<long> *pscd,
		 std::vector<long> *sde)
{
	pscd->clear();
	sde->clear();
	size_t i = 20, n = 0;
	long p = 0, e = 0;
	while (i < bie.size()) {
		if (bie[i] == 0xff && bie[i + 1] == 0x06) {
			i += 8;
			continue;
		}
		size_t start = i;
		for (;;) {
			if (bie[i] == 0xff && bie[i + 1] == 0x00) {
				p += 2;
				i += 2;
			} else if (bie[i] == 0xff) {
				i += 2;
				break;
			} else {
				p++;
				i++;
			}
		}
		e += (long)(i - start);
		if (++n % stripes == 0) {
			pscd->push_back(p);
			sde->push_back(e);
			p = e = 0;
		}
	}
}

} // namespace

/* T.82 7.2.3: the artificial image in seven layers (D 6, L0 2, TPDON,
   TPBON, DPON, the AT moves of Table 31): Table 30's counts for each
   layer, Table 32's byte counts, and the decoder gives the image back */
TEST(Jbig, ProgressiveArtificialImage)
{
	T4Page page;
	artificial(&page);
	JbigImage img = page_image(&page);
	const JbigAtMove moves[2] = {{9 * 128, 8, 6}, {10 * 64, 4, 5}};
	JbigLayerStats st[7];
	JbigOptions opt;
	opt.layers = 6;
	opt.l0 = 2;
	opt.mx = 8;
	opt.options = JBG_TPDON | JBG_TPBON | JBG_DPON;
	opt.moves = moves;
	opt.n_moves = 2;
	opt.stats = st;
	std::vector<unsigned char> bie;
	jbig_encode_image(img, opt, &bie);

	/* Table 30, layer 6 first */
	static const long t30[7][6] = {
		{137, 7033, 375520, 589344, 2859096, 188817},
		{186, 1442, 93120, 128642, 734718, 65584},
		{181, 135, 22792, 30230, 186098, 16565},
		{117, 8, 5406, 7246, 47128, 4994},
		{61, 0, 1238, 1769, 11999, 1430},
		{31, 0, 248, 452, 3082, 370},
		{3, 0, 93, 0, 868, 113},
	};
	for (int d = 0; d <= 6; d++) {
		const long *t = t30[6 - d];
		SCOPED_TRACE(d);
		EXPECT_EQ(st[d].tp_lines, t[0]);
		if (d)
			EXPECT_EQ(st[d].tp_exceptions, t[1]);
		EXPECT_EQ(st[d].tp_pixels, t[2]);
		EXPECT_EQ(st[d].dp_pixels, t[3]);
		EXPECT_EQ(st[d].coded_pixels, t[4]);
		EXPECT_EQ(st[d].scd_bytes, t[5]);
	}

	/* Table 32: the PSCD and SDE of each layer, the BIE */
	static const long t32[7][2] = {
		{114, 146}, {373, 405}, {1434, 1466}, {5010, 5042},
		{16634, 16666}, {65905, 65937}, {189584, 189616},
	};
	std::vector<long> pscd, sde;
	layer_sizes(bie, 16, &pscd, &sde);
	ASSERT_EQ(pscd.size(), 7u);
	for (int d = 0; d <= 6; d++) {
		EXPECT_EQ(pscd[d], t32[d][0]) << "layer " << d;
		EXPECT_EQ(sde[d], t32[d][1]) << "layer " << d;
	}
	EXPECT_EQ((long)bie.size(), 279314);

	JbigImage out;
	size_t used = 0;
	ASSERT_EQ(jbig_decode_image(bie.data(), bie.size(), 10000, 1, &out, &used), 0);
	EXPECT_EQ(used, bie.size());
	ASSERT_EQ(out.width, img.width);
	ASSERT_EQ(out.height, img.height);
	EXPECT_TRUE(out.bits[0] == img.bits[0]);
}

namespace {

/* pixel n of a 13 pixel set (Figure 13's numbers) */
int bit(unsigned v, int n)
{
	return (v >> n) & 1;
}

/* Table 17 for Figure 13's pixels: 0 to 3 low resolution, 4 to 12 high
   resolution; pixel 3 is the result */
int rr13(unsigned p)
{
	unsigned idx = bit(p, 12) | bit(p, 11) << 1 | bit(p, 10) << 2 | bit(p, 9) << 3 |
		       bit(p, 8) << 4 | bit(p, 7) << 5 | bit(p, 6) << 6 | bit(p, 5) << 7 |
		       bit(p, 4) << 8 | bit(p, 2) << 9 | bit(p, 1) << 10 | bit(p, 0) << 11;
	return (jbig_rr_table[idx >> 3] >> (7 - (idx & 7))) & 1;
}

/* a random image of planes, black in runs, with blank bands */
JbigImage random_image(u32 width, u32 height, s16 planes, unsigned seed)
{
	std::mt19937 rng(seed);
	JbigImage img;
	img.width = width;
	img.height = height;
	img.planes = planes;
	img.bits.assign(planes, std::vector<unsigned char>(img.stride() * height, 0));
	for (s16 i = 0; i < planes; i++) {
		for (u32 y = 0; y < height; y++) {
			if ((y / 16) % 4 == 0)
				continue;
			int run = 0, c = 0;
			for (u32 x = 0; x < width; x++) {
				if (run-- <= 0) {
					c = (int)(rng() % 3 == 0);
					run = (int)(rng() % 12);
				}
				if (c)
					img.bits[i][y * img.stride() + x / 8] |= (unsigned char)(0x80 >> (x & 7));
			}
		}
	}
	return img;
}

/* the first rows lines of two images the same */
void same_image(const JbigImage &a, const JbigImage &b, u32 rows)
{
	ASSERT_EQ(a.width, b.width);
	ASSERT_EQ(a.planes, b.planes);
	ASSERT_GE(b.height, rows);
	for (s16 i = 0; i < a.planes; i++)
		EXPECT_EQ(memcmp(a.bits[i].data(), b.bits[i].data(), a.stride() * rows), 0) << "plane " << i;
}

} // namespace

/* T.82 Tables 17 to 22: the samples 6.3 gives, the hits Table 18
   gives, and each DP hit is what Table 17 makes the pixel (when the
   reference pixels can occur at all) */
TEST(Jbig, Tables)
{
	static const int first[8] = {0, 0, 0, 1, 0, 0, 0, 1};
	for (unsigned i = 0; i < 8; i++)
		EXPECT_EQ((jbig_rr_table[0] >> (7 - i)) & 1, first[i]);

	static const unsigned base[4] = {0, 256, 768, 2816};
	static const int target[4] = {8, 9, 11, 12};
	static const int hits[4] = {20, 108, 526, 1044};
	for (int ph = 0; ph < 4; ph++) {
		int t = target[ph], n = 0;
		for (unsigned idx = 0; idx < (1u << t); idx++) {
			unsigned e = base[ph] + idx;
			int v = (jbig_dp_table[e >> 2] >> (6 - 2 * (e & 3))) & 3;
			if (v == 2)
				continue;
			n++;
			/* the pixels of the block after the target: free */
			int seen = 0;
			for (unsigned u = 0; u < 16; u++) {
				unsigned p = idx;
				int k = 0;
				for (int q : {8, 9, 11, 12})
					if (q >= t)
						p |= (unsigned)((u >> k++) & 1) << q;
				if (rr13(p) == bit(p, 3))
					seen |= 1 << bit(p, t);
			}
			if (seen)
				EXPECT_EQ(seen, 1 << v) << "phase " << ph << " index " << idx;
		}
		EXPECT_EQ(n, hits[ph]) << "phase " << ph;
	}
}

/* T.82 6.3: the low resolution image of a black image is black (but
   near the background border at its edges), of a white one white; odd
   sizes round up */
TEST(Jbig, ResolutionReduction)
{
	JbigImage hi, lo;
	hi.width = 13;
	hi.height = 9;
	hi.planes = 2;
	hi.bits.push_back(std::vector<unsigned char>(hi.stride() * 9, 0xff));
	hi.bits.push_back(std::vector<unsigned char>(hi.stride() * 9, 0x00));
	jbig_reduce(hi, &lo);
	EXPECT_EQ(lo.width, 7u);
	EXPECT_EQ(lo.height, 5u);
	ASSERT_EQ(lo.bits.size(), 2u);
	for (u32 y = 0; y < 5; y++)
		for (u32 x = 0; x < 7; x++) {
			if (y > 0 && x > 0 && x < 6)
				EXPECT_EQ((lo.bits[0][y * lo.stride() + x / 8] >> (7 - (x & 7))) & 1, 1);
			EXPECT_EQ((lo.bits[1][y * lo.stride() + x / 8] >> (7 - (x & 7))) & 1, 0);
		}
}

/* progressive BIEs of more planes and odd sizes round trip in each
   order of Table 11, with HITOLO, and with and without TPDON, DPON
   and the two line template */
TEST(Jbig, ProgressiveRoundTrip)
{
	static const s16 orders[] = {0, JBG_ILEAVE, JBG_ILEAVE | JBG_SMID, JBG_SEQ, JBG_SEQ | JBG_SMID,
				     JBG_SEQ | JBG_ILEAVE};
	static const s16 opts[] = {0, JBG_TPDON, JBG_DPON, JBG_TPDON | JBG_DPON | JBG_TPBON,
				   JBG_LRLTWO | JBG_TPDON | JBG_DPON};
	unsigned seed = 1;
	for (s16 hitolo : {0, JBG_HITOLO})
		for (s16 order : orders)
			for (s16 o : opts) {
				JbigImage img = random_image(77 + seed % 13, 61 + seed % 7, 1 + seed % 3, seed);
				JbigOptions opt;
				opt.layers = 1 + seed % 3;
				opt.l0 = 1 + seed % 4;
				opt.order = order | hitolo;
				opt.options = o;
				std::vector<unsigned char> bie;
				jbig_encode_image(img, opt, &bie);
				EXPECT_EQ(bie[1], opt.layers);
				EXPECT_EQ(bie[18], order | hitolo);
				EXPECT_EQ(bie[19], o);
				JbigImage out;
				size_t used = 0;
				ASSERT_EQ(jbig_decode_image(bie.data(), bie.size(), 10000, 3, &out, &used), 0)
					<< "seed " << seed;
				EXPECT_EQ(used, bie.size());
				ASSERT_EQ(out.height, img.height);
				same_image(img, out, img.height);
				seed++;
			}
}

/* a progressive BIE: a cap on the lines, an early end, an ABORT, a
   NEWLEN, a private DP table (here the default one), and the AT pixel
   moved in a differential layer */
TEST(Jbig, ProgressiveMarkersAndLimits)
{
	JbigImage img = random_image(200, 160, 1, 3);
	const JbigAtMove move = {40, 5, 2};
	JbigOptions opt;
	opt.layers = 2;
	opt.l0 = 8;
	opt.mx = 8;
	opt.options = JBG_TPBON | JBG_TPDON | JBG_DPON;
	opt.moves = &move;
	opt.n_moves = 1;
	std::vector<unsigned char> bie;
	jbig_encode_image(img, opt, &bie);
	const unsigned char at[] = {0xff, 0x06, 0, 0, 0, 40 - 32, 5, 0};
	EXPECT_NE(std::search(bie.begin(), bie.end(), at, at + 8), bie.end());
	JbigImage out;
	ASSERT_EQ(jbig_decode_image(bie.data(), bie.size(), 10000, 1, &out, nullptr), 0);
	same_image(img, out, 160);

	/* at most 50 lines */
	ASSERT_EQ(jbig_decode_image(bie.data(), bie.size(), 50, 1, &out, nullptr), 0);
	EXPECT_EQ(out.height, 50u);
	same_image(img, out, 50);

	/* cut short, or ABORT: the lines so far */
	ASSERT_EQ(jbig_decode_image(bie.data(), bie.size() * 3 / 4, 10000, 1, &out, nullptr), 1);
	EXPECT_LT(out.height, 160u);
	const unsigned char sdnorm[] = {0xff, 0x02};
	auto half = std::search(bie.begin() + (long)bie.size() / 2, bie.end(), sdnorm, sdnorm + 2);
	ASSERT_NE(half, bie.end());
	std::vector<unsigned char> v(bie.begin(), half + 2);
	v.push_back(0xff);
	v.push_back(0x04);
	ASSERT_EQ(jbig_decode_image(v.data(), v.size(), 10000, 1, &out, nullptr), 1);

	/* YD 160 in the header, NEWLEN 150 after the last SDE (as the
	   same stripes, coded as 150 lines) */
	JbigImage short_img = img;
	short_img.height = 150;
	short_img.bits[0].resize(short_img.stride() * 150);
	jbig_encode_image(short_img, opt, &v);
	v[11] = 160;
	v[19] |= JBG_VLENGTH;
	const unsigned char newlen[] = {0xff, 0x05, 0, 0, 0, 150};
	v.insert(v.end(), newlen, newlen + sizeof(newlen));
	ASSERT_EQ(jbig_decode_image(v.data(), v.size(), 10000, 1, &out, nullptr), 0);
	EXPECT_EQ(out.height, 150u);
	same_image(img, out, 150);

	/* DPPRIV with the table after the header; DPLAST (no table kept) */
	v.assign(bie.begin(), bie.begin() + 20);
	v[19] |= JBG_DPPRIV;
	v.insert(v.end(), jbig_dp_table, jbig_dp_table + 1728);
	v.insert(v.end(), bie.begin() + 20, bie.end());
	ASSERT_EQ(jbig_decode_image(v.data(), v.size(), 10000, 1, &out, nullptr), 0);
	same_image(img, out, 160);
	v = bie;
	v[19] |= JBG_DPPRIV | JBG_DPLAST;
	EXPECT_EQ(jbig_decode_image(v.data(), v.size(), 10000, 1, &out, nullptr), -1);

	/* a progressive BIE is no T.85 page */
	T4Page page;
	EXPECT_EQ(jbig_decode(bie.data(), bie.size(), 10000, &page), -1);

	/* garbage after the header ends without a crash */
	std::mt19937 rng(7);
	for (int i = 0; i < 30; i++) {
		v.assign(bie.begin(), bie.begin() + 20);
		for (int k = 0; k < 3000; k++)
			v.push_back((unsigned char)rng());
		s16 r = jbig_decode_image(v.data(), v.size(), 10000, 1, &out, nullptr);
		EXPECT_TRUE(r >= -1 && r <= 1);
	}
}

/* SDRST: the stripe after it is coded as the top of an image, so the
   lines above it are background (T.82 6.2.5) */
TEST(Jbig, StripeReset)
{
	JbigImage img = random_image(96, 64, 1, 11);
	JbigImage top = img, bottom = img;
	top.height = bottom.height = 32;
	top.bits[0].resize(top.stride() * 32);
	bottom.bits[0].erase(bottom.bits[0].begin(), bottom.bits[0].begin() + (long)top.stride() * 32);
	JbigOptions opt;
	opt.l0 = 32;
	std::vector<unsigned char> a, b;
	jbig_encode_image(top, opt, &a);
	jbig_encode_image(bottom, opt, &b);
	ASSERT_EQ(a[a.size() - 1], 0x02);
	a[a.size() - 1] = 0x03;
	a[11] = 64;     /* YD */
	a.insert(a.end(), b.begin() + 20, b.end());
	JbigImage out;
	ASSERT_EQ(jbig_decode_image(a.data(), a.size(), 10000, 1, &out, nullptr), 0);
	ASSERT_EQ(out.height, 64u);
	same_image(img, out, 64);
}

/* the sequential encoder's counts: every line coded or predicted, and
   the SCD sizes of 7.2.2 */
TEST(Jbig, SequentialStats)
{
	T4Page page;
	artificial(&page);
	JbigImage img = page_image(&page);
	const JbigAtMove move = {9 * 128, 8};
	JbigLayerStats st;
	JbigOptions opt;
	opt.mx = 8;
	opt.moves = &move;
	opt.n_moves = 1;
	opt.stats = &st;
	std::vector<unsigned char> bie;
	jbig_encode_image(img, opt, &bie);
	EXPECT_EQ(st.scd_bytes, 252557);
	EXPECT_EQ(st.tp_pixels + st.coded_pixels, 1960L * 1951);
	EXPECT_EQ(st.tp_pixels, st.tp_lines * 1960);
}
