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
