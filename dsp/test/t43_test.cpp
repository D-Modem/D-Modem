/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t43_test.cpp - dsp/t43.h: T.43's header (its 7.2.2 examples), Gray
 *                   code (Table 6), every image type there and back in
 *                   either interleave, the colours each gives, and
 *                   damaged data; JBIG of more than one bit-plane
 *
 */

#include <string.h>

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "jbig.h"
#include "jpeg.h"
#include "t43.h"

using namespace dsp;

namespace {

/* an image of the type: smooth areas, edges and some noise, each
   sample below 2^bits */
T43Image picture(s16 type, s16 b0, s16 b1, s16 b2, u32 w, u32 h, unsigned seed)
{
	std::mt19937 rng(seed);
	T43Image img;
	img.type = type;
	img.bits[0] = b0;
	img.bits[1] = b1;
	img.bits[2] = b2;
	img.width = w;
	img.height = h;
	s16 comps = img.comps();
	img.pix.resize((size_t)w * h * comps);
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
			for (s16 c = 0; c < comps; c++) {
				s16 bits = type == T43_COLOUR ? img.bits[c] :
					   type < T43_PALETTE ? 1 : b0;
				u32 top = (1u << bits) - 1;
				u32 v = (x / 7 + y / 11 + (u32)c * 3) % (top + 1);
				if ((x / 30 + y / 30) % 4 == 0)
					v = top;
				if (rng() % 50 == 0)
					v = rng() % (top + 1);
				img.pix[((size_t)y * w + x) * comps + c] = (u16)v;
			}
	if (type == T43_PALETTE || type == T43_PALETTE12) {
		u32 n = (1u << b0) - 1 > 2 ? (1u << b0) - 1 : 1u << b0;
		for (u32 i = 0; i < n * 3; i++)
			img.palette.push_back((u16)(rng() % (type == T43_PALETTE12 ? 4096 : 256)));
		for (u16 &v : img.pix)
			v = (u16)(v % n);
	}
	return img;
}

/* encoded and decoded: the same image */
void round_trip(const T43Image &img, const T43Options &opt)
{
	std::vector<unsigned char> d;
	t43_encode(img, opt, &d);
	T43Image out;
	ASSERT_EQ(t43_decode(d.data(), d.size(), 10000, &out), 0) << "type " << img.type;
	EXPECT_EQ(out.type, img.type);
	EXPECT_EQ(out.width, img.width);
	EXPECT_EQ(out.height, img.height);
	EXPECT_EQ(memcmp(out.bits, img.bits, sizeof(img.bits)), 0);
	EXPECT_EQ(out.pix == img.pix, true) << "type " << img.type;
	EXPECT_EQ(out.palette, img.palette);
	EXPECT_EQ(out.dpi, img.dpi);
}

} // namespace

/* T.43 7.2.2: the magic number, G3FAX0 (1997, 200 dpi, JBIG, grey, 8
   planes) and ECIH; the BIE's header (P 8, stripes interleaved, L0
   128); the ending marker */
TEST(T43, Header)
{
	T43Image img = picture(T43_GREY, 8, 0, 0, 1728, 20, 1);
	std::vector<unsigned char> d;
	t43_encode(img, T43Options(), &d);
	const unsigned char head[] = {
		0xff, 0xa8, 0xff, 0xe1, 0x00, 0x12, 'G', '3', 'F', 'A', 'X', 0x00, 0x07, 0xcd,
		0x00, 0xc8, 0x00, 0x20, 0x08, 0x00, 0x00, 0x00,
		0xff, 0xe1, 0x00, 0x08, 'G', '3', 'F', 'A', 'X', 0xff,
		0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06, 0xc0, 0x00, 0x00, 0x00, 0x14,
		0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x03, 0x08,
	};
	ASSERT_GT(d.size(), sizeof(head));
	EXPECT_EQ(memcmp(d.data(), head, sizeof(head)), 0);
	EXPECT_EQ(d[d.size() - 2], 0xff);
	EXPECT_EQ(d.back(), 0xa9);

	/* a palette of 236 entries (Table 4): G3FAX3, its length 16 + 3
	   times 236, table ID 0 */
	img = picture(T43_PALETTE, 8, 0, 0, 64, 8, 2);
	img.palette.resize(236 * 3);
	img.palette[0] = 255;
	img.palette[1] = 128;
	img.palette[2] = 96;
	t43_encode(img, T43Options(), &d);
	const unsigned char pal[] = {
		0xff, 0xe3, 0x00, 0x00, 0x02, 0xd4, 'G', '3', 'F', 'A', 'X', 0x03,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xec, 0xff, 0x80, 0x60,
	};
	EXPECT_EQ(memcmp(d.data() + 22, pal, sizeof(pal)), 0);
	EXPECT_EQ(d[22 + 18 + 236 * 3 + 1], 0xe1);
}

/* T.43 Table 6: the planes of 4 bit grey 0 to 15 are its Gray code */
TEST(T43, GrayCode)
{
	static const char *gray[16] = {
		"0000", "0001", "0011", "0010", "0110", "0111", "0101", "0100",
		"1100", "1101", "1111", "1110", "1010", "1011", "1001", "1000",
	};
	T43Image img;
	img.type = T43_GREY;
	img.bits[0] = 4;
	img.width = 16;
	img.height = 1;
	for (u16 v = 0; v < 16; v++)
		img.pix.push_back(v);
	std::vector<unsigned char> d;
	t43_encode(img, T43Options(), &d);
	JbigImage j;
	size_t start = 32;      /* (after G3FAX0 and ECIH) */
	ASSERT_EQ(jbig_decode_image(d.data() + start, d.size() - start, 100, 4, &j, nullptr), 0);
	ASSERT_EQ(j.planes, 4);
	for (u32 x = 0; x < 16; x++)
		for (s16 p = 0; p < 4; p++)
			EXPECT_EQ((j.bits[p][x / 8] >> (7 - x % 8)) & 1, gray[x][p] - '0') << x << " plane " << p;
}

/* every image type and number of bits, in stripes of either kind and
   in planes, back the same */
TEST(T43, RoundTrip)
{
	T43Options opt;
	round_trip(picture(T43_GREY, 8, 0, 0, 1728, 300, 3), opt);
	round_trip(picture(T43_GREY, 5, 0, 0, 101, 77, 4), opt);
	round_trip(picture(T43_GREY, 12, 0, 0, 64, 40, 5), opt);
	round_trip(picture(T43_COLOUR, 8, 8, 8, 333, 140, 6), opt);
	round_trip(picture(T43_COLOUR, 7, 4, 4, 99, 50, 7), opt);
	round_trip(picture(T43_PALETTE, 8, 0, 0, 200, 60, 8), opt);
	round_trip(picture(T43_PALETTE, 1, 0, 0, 33, 9, 9), opt);
	round_trip(picture(T43_PALETTE12, 7, 0, 0, 200, 60, 10), opt);
	round_trip(picture(T43_RGB, 3, 0, 0, 150, 70, 11), opt);
	round_trip(picture(T43_CMY, 3, 0, 0, 150, 70, 12), opt);
	round_trip(picture(T43_CMYK, 4, 0, 0, 150, 70, 13), opt);
	opt.plane_interleave = 1;
	round_trip(picture(T43_COLOUR, 8, 8, 8, 333, 140, 14), opt);
	opt.l0 = 7;
	round_trip(picture(T43_CMYK, 4, 0, 0, 50, 30, 15), opt);
	opt.plane_interleave = 0;
	round_trip(picture(T43_GREY, 6, 0, 0, 50, 30, 16), opt);
}

/* the colours: primaries for one bit a colour, CIELAB for the rest
   (white L* 255, a* 128, b* 96) */
TEST(T43, Colours)
{
	std::vector<unsigned char> rgb;
	s16 comps;
	T43Image img;
	img.width = 8;
	img.height = 1;
	img.type = T43_RGB;
	img.bits[0] = 3;
	for (u16 v = 0; v < 8; v++)
		for (s16 c = 0; c < 3; c++)
			img.pix.push_back((v >> (2 - c)) & 1);
	t43_to_srgb(img, &rgb, &comps);
	ASSERT_EQ(comps, 3);
	for (int v = 0; v < 8; v++)
		for (int c = 0; c < 3; c++)
			EXPECT_EQ(rgb[v * 3 + c], (v >> (2 - c)) & 1 ? 255 : 0);
	img.type = T43_CMY;
	t43_to_srgb(img, &rgb, &comps);
	for (int v = 0; v < 8; v++)
		for (int c = 0; c < 3; c++)
			EXPECT_EQ(rgb[v * 3 + c], (v >> (2 - c)) & 1 ? 0 : 255);
	/* CMYK: black over anything */
	img.type = T43_CMYK;
	img.bits[0] = 4;
	img.width = 2;
	img.pix = {0, 0, 0, 0, 1, 0, 0, 1};
	t43_to_srgb(img, &rgb, &comps);
	EXPECT_EQ(rgb, std::vector<unsigned char>({255, 255, 255, 0, 0, 0}));

	img.type = T43_PALETTE12;
	img.bits[0] = 1;
	img.palette = {4095, 2048, 1536, 0, 2048, 1536};
	img.pix = {0, 1};
	t43_to_srgb(img, &rgb, &comps);
	for (int c = 0; c < 3; c++) {
		EXPECT_GE(rgb[c], 253);
		EXPECT_LE(rgb[3 + c], 2);
	}
	img.type = T43_GREY;
	img.bits[0] = 4;
	img.pix = {15, 0};
	t43_to_srgb(img, &rgb, &comps);
	ASSERT_EQ(comps, 1);
	EXPECT_GE(rgb[0], 253);
	EXPECT_LE(rgb[1], 2);
	img.type = T43_COLOUR;
	img.bits[1] = img.bits[2] = 8;
	img.bits[0] = 8;
	img.width = 1;
	img.pix = {255, 128, 96};
	t43_to_srgb(img, &rgb, &comps);
	EXPECT_EQ(rgb, std::vector<unsigned char>({255, 255, 255}));
}

/* cut short: the lines so far; a cap; no T.43, bad types, garbage */
TEST(T43, Damaged)
{
	T43Image img = picture(T43_COLOUR, 8, 8, 8, 400, 300, 17), out;
	std::vector<unsigned char> d;
	t43_encode(img, T43Options(), &d);
	ASSERT_EQ(t43_decode(d.data(), d.size() / 2, 10000, &out), 1);
	EXPECT_GT(out.height, 0u);
	EXPECT_LT(out.height, 300u);
	ASSERT_EQ(t43_decode(d.data(), d.size(), 100, &out), 0);
	EXPECT_EQ(out.height, 100u);

	std::vector<unsigned char> k = d;
	k[17] = 7;                              /* image type 7 */
	EXPECT_EQ(t43_decode(k.data(), k.size(), 10000, &out), -1);
	k = d;
	k[18] = 13;                             /* 13 bits of L* */
	EXPECT_EQ(t43_decode(k.data(), k.size(), 10000, &out), -1);
	k = d;
	k[34] = 23;                             /* the BIE's P not 24 */
	EXPECT_EQ(t43_decode(k.data(), k.size(), 10000, &out), -1);
	const unsigned char jpeg[] = {0xff, 0xd8, 0xff, 0xe1, 0, 8, 'G', '3', 'F', 'A', 'X', 0};
	EXPECT_EQ(t43_decode(jpeg, sizeof(jpeg), 10000, &out), -1);

	std::mt19937 rng(18);
	for (int i = 0; i < 40; i++) {
		k = d;
		for (int e = 0; e < 20; e++)
			k[rng() % k.size()] = (unsigned char)rng();
		s16 r = t43_decode(k.data(), k.size(), 1000, &out);
		EXPECT_TRUE(r >= -1 && r <= 1);
		k.assign(d.begin(), d.begin() + 60);
		for (int e = 0; e < 3000; e++)
			k.push_back((unsigned char)rng());
		r = t43_decode(k.data(), k.size(), 1000, &out);
		EXPECT_TRUE(r >= -1 && r <= 1);
	}
}

/* JBIG with planes: T.82 Table 11's orders (the two not allowed
   refused), one plane only for T.85 */
TEST(T43, JbigPlanes)
{
	JbigImage img;
	img.width = 77;
	img.height = 300;
	img.planes = 3;
	std::mt19937 rng(19);
	for (s16 p = 0; p < 3; p++) {
		img.bits.emplace_back(img.stride() * img.height);
		for (size_t i = 0; i < img.bits[p].size(); i++)
			img.bits[p][i] = (i / 40) % (p + 2) ? 0 : (unsigned char)rng();
		/* (the bits right of the last pixel 0) */
		for (u32 y = 0; y < img.height; y++)
			img.bits[p][y * img.stride() + img.stride() - 1] &= 0xf8;
	}
	static const s16 orders[] = {0, JBG_ILEAVE, JBG_ILEAVE | JBG_SMID, JBG_SEQ, JBG_SEQ | JBG_SMID,
				     JBG_SEQ | JBG_ILEAVE};
	for (s16 order : orders) {
		JbigOptions opt;
		opt.order = order;
		opt.l0 = 64;
		std::vector<unsigned char> bie;
		jbig_encode_image(img, opt, &bie);
		EXPECT_EQ(bie[18], order);
		JbigImage out;
		size_t used = 0;
		ASSERT_EQ(jbig_decode_image(bie.data(), bie.size(), 1000, 3, &out, &used), 0) << order;
		EXPECT_EQ(used, bie.size());
		EXPECT_EQ(out.height, img.height);
		EXPECT_TRUE(out.bits == img.bits) << order;
		T4Page page;
		EXPECT_EQ(jbig_decode(bie.data(), bie.size(), 1000, &page), -1);
		bie[18] = order | JBG_SMID;
		if (order == 0 || order == (JBG_SEQ | JBG_ILEAVE))
			EXPECT_EQ(jbig_decode_image(bie.data(), bie.size(), 1000, 3, &out, nullptr), -1);
	}
}
