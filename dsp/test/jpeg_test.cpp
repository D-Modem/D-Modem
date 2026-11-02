/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jpeg_test.cpp - dsp/jpeg.h: T.4 Annex E's data structure, round
 *                    trips of grey and colour images (with 4:1:1 and
 *                    without, restarts, DNL, odd sizes), damaged data,
 *                    and T.42's CIELAB conversions
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "jpeg.h"

using namespace dsp;

namespace {

/* a test picture: smooth gradients, a few sharp edges, some noise */
JpegImage picture(u32 w, u32 h, s16 comps, unsigned seed)
{
	std::mt19937 rng(seed);
	JpegImage img;
	img.width = w;
	img.height = h;
	img.comps = comps;
	img.pix.resize((size_t)w * h * comps);
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
			for (int c = 0; c < comps; c++) {
				double v = 128 + 80 * sin((x + 3 * c) / 23.0) * cos(y / 31.0 + c);
				if ((x / 40 + y / 40) % 5 == 0)
					v = c == 1 ? 40 : 220;
				v += (int)(rng() % 5) - 2;
				img.pix[((size_t)y * w + x) * comps + c] =
					(unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
			}
	return img;
}

/* the peak signal to noise ratio between two images of one size, dB */
double psnr(const JpegImage &a, const JpegImage &b)
{
	double e = 0;
	for (size_t i = 0; i < a.pix.size(); i++) {
		double d = (double)a.pix[i] - b.pix[i];
		e += d * d;
	}
	e /= (double)a.pix.size();
	return e ? 10 * log10(255.0 * 255 / e) : 99;
}

/* the markers of a stream in order, up to the scan */
std::vector<int> markers(const std::vector<unsigned char> &j)
{
	std::vector<int> m;
	size_t p = 2;
	m.push_back(j[1]);
	while (p + 3 < j.size() && j[p] == 0xff) {
		m.push_back(j[p + 1]);
		if (j[p + 1] == 0xda)
			break;
		p += 2 + (j[p + 2] << 8 | j[p + 3]);
	}
	return m;
}

/* the encoded image decoded: whole, its size, and close */
void round_trip(const JpegImage &img, const JpegOptions &opt, double min_psnr, JpegImage *out = nullptr)
{
	std::vector<unsigned char> j;
	jpeg_encode(img, opt, &j);
	JpegImage d;
	ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
	ASSERT_EQ(d.width, img.width);
	ASSERT_EQ(d.height, img.height);
	ASSERT_EQ(d.comps, img.comps);
	EXPECT_EQ(d.dpi, opt.dpi);
	EXPECT_GT(psnr(img, d), min_psnr) << img.width << "x" << img.height << " q" << opt.quality;
	if (out)
		*out = d;
}

} // namespace

/* T.4 E.6.2: SOI, the G3FAX APP1 (1994, 200 dpi), DQT, DHT, SOF0, SOS;
   the Huffman tables each a whole code of T.81's symbols */
TEST(Jpeg, DataStructure)
{
	JpegImage img = picture(64, 32, 3, 1);
	std::vector<unsigned char> j;
	jpeg_encode(img, JpegOptions(), &j);
	std::vector<int> m = markers(j);
	std::vector<int> want = {0xd8, 0xe1, 0xdb, 0xc4, 0xc0, 0xda};
	EXPECT_EQ(m, want);
	const unsigned char app1[] = {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x0c, 'G', '3', 'F', 'A', 'X', 0x00,
				      0x07, 0xca, 0x00, 0xc8};
	EXPECT_EQ(memcmp(j.data(), app1, sizeof(app1)), 0);
	EXPECT_EQ(j[j.size() - 2], 0xff);
	EXPECT_EQ(j.back(), 0xd9);

	/* the DHT: DC tables of the 12 categories, AC ones of EOB, ZRL and
	   each run and size, prefix-free (Kraft sum below 1: no code of all
	   ones) */
	size_t p = 2;
	while (j[p + 1] != 0xc4)
		p += 2 + (j[p + 2] << 8 | j[p + 3]);
	size_t end = p + 2 + (j[p + 2] << 8 | j[p + 3]);
	p += 4;
	int tables = 0;
	while (p < end) {
		int tc = j[p] >> 4, n = 0;
		double kraft = 0;
		for (int i = 0; i < 16; i++) {
			n += j[p + 1 + i];
			kraft += j[p + 1 + i] / pow(2.0, i + 1);
		}
		EXPECT_LT(kraft, 1.0);
		std::set<int> vals(j.begin() + (long)p + 17, j.begin() + (long)p + 17 + n);
		EXPECT_EQ((int)vals.size(), n);
		if (tc) {
			EXPECT_EQ(n, 162);
			EXPECT_TRUE(vals.count(0x00) && vals.count(0xf0));
			for (int r = 0; r < 16; r++)
				for (int s = 1; s <= 10; s++)
					EXPECT_TRUE(vals.count(r << 4 | s)) << r << "/" << s;
		} else {
			EXPECT_EQ(n, 12);
		}
		p += 17 + n;
		tables++;
	}
	EXPECT_EQ(tables, 4);
}

/* grey and colour pictures, of sizes not a whole number of MCUs, back
   with little loss; more with higher quality */
TEST(Jpeg, RoundTrip)
{
	JpegOptions opt;
	round_trip(picture(1728, 40, 1, 2), opt, 32);
	round_trip(picture(101, 37, 1, 3), opt, 32);
	round_trip(picture(203, 77, 3, 4), opt, 28);
	opt.subsample = 0;
	round_trip(picture(203, 77, 3, 5), opt, 30);
	opt.quality = 98;
	round_trip(picture(203, 77, 3, 6), opt, 40);
	opt.dpi = 0;
	round_trip(picture(9, 9, 1, 7), opt, 40);
}

/* DRI with RSTn, and DNL for the line count: the same pels as without */
TEST(Jpeg, RestartsAndDnl)
{
	JpegImage img = picture(333, 70, 3, 8), plain, other;
	JpegOptions opt;
	round_trip(img, opt, 28, &plain);
	opt.restart = 3;
	opt.dnl = 1;
	round_trip(img, opt, 28, &other);
	EXPECT_EQ(plain.pix, other.pix);

	opt.restart = 0;
	img = picture(100, 50, 1, 9);
	round_trip(img, opt, 30, &other);
	std::vector<unsigned char> j;
	jpeg_encode(img, opt, &j);
	EXPECT_EQ(j[j.size() - 8], 0xff);
	EXPECT_EQ(j[j.size() - 7], 0xdc);           /* DNL, then EOI */
}

/* cut short: the lines so far; capped; broken data and no JPEG at all */
TEST(Jpeg, Damaged)
{
	JpegImage img = picture(400, 300, 3, 10), d;
	std::vector<unsigned char> j;
	jpeg_encode(img, JpegOptions(), &j);
	ASSERT_EQ(jpeg_decode(j.data(), j.size() / 2, 10000, &d), 1);
	EXPECT_GT(d.height, 0u);
	EXPECT_LT(d.height, 300u);
	ASSERT_EQ(jpeg_decode(j.data(), j.size(), 100, &d), 0);
	EXPECT_EQ(d.height, 100u);
	const unsigned char none[] = {1, 2, 3, 4, 5, 6};
	EXPECT_EQ(jpeg_decode(none, sizeof(none), 10000, &d), -1);

	std::mt19937 rng(11);
	for (int i = 0; i < 40; i++) {
		std::vector<unsigned char> k = j;
		for (int e = 0; e < 20; e++)
			k[600 + rng() % (k.size() - 600)] = (unsigned char)rng();
		s16 r = jpeg_decode(k.data(), k.size(), 10000, &d);
		EXPECT_TRUE(r >= -1 && r <= 1);
		k.assign(j.begin(), j.begin() + 400);
		for (int e = 0; e < 3000; e++)
			k.push_back((unsigned char)rng());
		r = jpeg_decode(k.data(), k.size(), 10000, &d);
		EXPECT_TRUE(r >= -1 && r <= 1);
	}
}

/* T.4 E.6.4: white is L 255, a 128, b 96; black L 0; colours back
   within a few levels; grey levels likewise */
TEST(Jpeg, Cielab)
{
	unsigned char rgb[6] = {255, 255, 255, 0, 0, 0}, lab[6], back[6];
	t42_rgb_to_lab(rgb, lab, 2);
	EXPECT_EQ(lab[0], 255);
	EXPECT_EQ(lab[1], 128);
	EXPECT_EQ(lab[2], 96);
	EXPECT_EQ(lab[3], 0);
	EXPECT_EQ(lab[4], 128);
	EXPECT_EQ(lab[5], 96);
	/* pure red: L* about 54, a* 81, b* 70 (D50) */
	unsigned char red[3] = {255, 0, 0};
	t42_rgb_to_lab(red, lab, 1);
	EXPECT_NEAR(lab[0], 54.3 * 255 / 100, 2);
	EXPECT_NEAR(lab[1], 80.8 * 255 / 170 + 128, 2);
	EXPECT_NEAR(lab[2], 69.9 * 255 / 200 + 96, 2);

	std::mt19937 rng(12);
	int worst = 0;
	for (int i = 0; i < 2000; i++) {
		/* (within the default gamut: b* from -75 clips saturated
		   blues) */
		int base = 70 + (int)(rng() % 120);
		for (int k = 0; k < 3; k++)
			rgb[k] = (unsigned char)(base - 30 + (int)(rng() % 61));
		t42_rgb_to_lab(rgb, lab, 1);
		t42_lab_to_rgb(lab, back, 1);
		for (int k = 0; k < 3; k++)
			worst = std::max(worst, abs(rgb[k] - back[k]));
	}
	EXPECT_LE(worst, 4);
	for (int g = 0; g < 256; g++) {
		unsigned char gg = (unsigned char)g, l, b;
		t42_grey_to_l(&gg, &l, 1);
		t42_l_to_grey(&l, &b, 1);
		EXPECT_LE(abs(g - b), g < 16 ? 3 : 2) << g;
	}
}
