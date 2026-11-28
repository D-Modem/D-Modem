/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jpeg_test.cpp - dsp/jpeg.h: T.4 Annex E's data structure, round
 *                    trips of grey and colour images (with 4:1:1 and
 *                    without, restarts, DNL, odd sizes), damaged data,
 *                    12-bit samples (SOF1), the G3FAX1 and G3FAX2
 *                    segments, and T.42's CIELAB (gamuts, illuminants,
 *                    12 bits) and ITU-YCC conversions
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

/* the 12-bit picture: picture()'s samples times 16, with a pattern in
   the 4 low bits that 8-bit samples cannot hold */
JpegImage picture12(u32 w, u32 h, s16 comps, unsigned seed)
{
	JpegImage img = picture(w, h, comps, seed);
	img.colour.bits = 12;
	img.pix16.resize(img.pix.size());
	for (size_t i = 0; i < img.pix.size(); i++)
		img.pix16[i] = (u16)(img.pix[i] * 16 + (i / 3 / 7) % 16);
	img.pix.clear();
	return img;
}

/* the peak signal to noise ratio between two 12-bit images, dB */
double psnr12(const JpegImage &a, const JpegImage &b)
{
	double e = 0;
	for (size_t i = 0; i < a.pix16.size(); i++) {
		double d = (double)a.pix16[i] - b.pix16[i];
		e += d * d;
	}
	e /= (double)a.pix16.size();
	return e ? 10 * log10(4095.0 * 4095 / e) : 99;
}

/* the offset of the first APP1 segment of a stream that starts "G3FAX"
   and the octet id, or 0 */
size_t g3fax(const std::vector<unsigned char> &j, int id)
{
	size_t p = 2;
	while (p + 11 < j.size() && j[p] == 0xff && j[p + 1] != 0xda) {
		if (j[p + 1] == 0xe1 && !memcmp(&j[p + 4], "G3FAX", 5) && j[p + 9] == id)
			return p;
		p += 2 + (j[p + 2] << 8 | j[p + 3]);
	}
	return 0;
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

/* T.4 E.5, T.30 bit 71: 12-bit samples in an extended (SOF1, P 12)
   frame, its Huffman tables made for the image; back with the low bits
   that 8 bits lose */
TEST(Jpeg, TwelveBit)
{
	for (s16 comps : {1, 3}) {
		JpegImage img = picture12(203, 77, comps, 20), d;
		JpegOptions opt;
		opt.quality = 100;
		opt.subsample = 0;
		std::vector<unsigned char> j;
		ASSERT_TRUE(jpeg_encode(img, opt, &j));
		std::vector<int> m = markers(j);
		ASSERT_EQ(m.back(), 0xda);
		EXPECT_EQ(m[m.size() - 2], 0xc1) << "SOF1";
		size_t p = 2;
		while (j[p + 1] != 0xc1)
			p += 2 + (j[p + 2] << 8 | j[p + 3]);
		EXPECT_EQ(j[p + 4], 12) << "P";
		ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
		EXPECT_EQ(d.colour.bits, 12);
		EXPECT_TRUE(d.pix.empty());
		ASSERT_EQ(d.pix16.size(), img.pix16.size());
		/* within an 8-bit step (16): finer than 8 bits */
		int worst = 0;
		for (size_t i = 0; i < d.pix16.size(); i++)
			worst = std::max(worst, abs((int)d.pix16[i] - img.pix16[i]));
		EXPECT_LT(worst, 16) << comps;
		EXPECT_GT(psnr12(img, d), 60) << comps;

		/* the default quality, 4:1:1, restarts and DNL */
		opt.quality = 75;
		opt.subsample = 1;
		opt.restart = 2;
		opt.dnl = 1;
		ASSERT_TRUE(jpeg_encode(img, opt, &j));
		ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
		EXPECT_EQ(d.height, img.height);
		EXPECT_GT(psnr12(img, d), 30) << comps;
	}

	/* a flat and a black 12-bit picture: the tables of few symbols */
	JpegImage flat;
	flat.width = 16;
	flat.height = 16;
	flat.comps = 1;
	flat.colour.bits = 12;
	flat.pix16.assign(256, 4095);
	std::vector<unsigned char> j;
	JpegImage d;
	ASSERT_TRUE(jpeg_encode(flat, JpegOptions(), &j));
	ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
	EXPECT_EQ(d.pix16, flat.pix16);
	flat.pix16.assign(256, 0);
	ASSERT_TRUE(jpeg_encode(flat, JpegOptions(), &j));
	ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
	EXPECT_EQ(d.pix16, flat.pix16);

	/* no 12 bits in a baseline frame; no 10 bits; too few samples */
	JpegImage img = picture12(32, 16, 1, 21);
	ASSERT_TRUE(jpeg_encode(img, JpegOptions(), &j));
	for (size_t p = 2; p + 4 < j.size(); p += 2 + (j[p + 2] << 8 | j[p + 3]))
		if (j[p + 1] == 0xc1) {
			j[p + 1] = 0xc0;
			break;
		}
	EXPECT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), -1);
	img.colour.bits = 10;
	EXPECT_FALSE(jpeg_encode(img, JpegOptions(), &j));
	img.colour.bits = 12;
	img.pix16.resize(10);
	EXPECT_FALSE(jpeg_encode(img, JpegOptions(), &j));
}

/* T.4 E.6.6 and E.6.7: G3FAX1 and G3FAX2 as the examples give them, for
   the defaults only when asked; read back by the decoder */
TEST(Jpeg, G3faxSegments)
{
	JpegImage img = picture(32, 16, 3, 22), d;
	std::vector<unsigned char> j;
	ASSERT_TRUE(jpeg_encode(img, JpegOptions(), &j));
	EXPECT_EQ(g3fax(j, 1), 0u);
	EXPECT_EQ(g3fax(j, 2), 0u);

	JpegOptions opt;
	opt.colour_info = 1;
	ASSERT_TRUE(jpeg_encode(img, opt, &j));
	const unsigned char gamut[] = {0xff, 0xe1, 0x00, 0x14, 'G', '3', 'F', 'A', 'X', 0x01, 0x00, 0x00,
				       0x00, 0x64, 0x00, 0x80, 0x00, 0xaa, 0x00, 0x60, 0x00, 0xc8};
	size_t p = g3fax(j, 1);
	ASSERT_NE(p, 0u);
	EXPECT_EQ(memcmp(&j[p], gamut, sizeof(gamut)), 0);
	const unsigned char d50[] = {0xff, 0xe1, 0x00, 0x0c, 'G', '3', 'F', 'A', 'X', 0x02, 0x00, 0x44,
				     0x35, 0x30};
	p = g3fax(j, 2);
	ASSERT_NE(p, 0u);
	EXPECT_EQ(memcmp(&j[p], d50, sizeof(d50)), 0);
	EXPECT_EQ(g3fax(j, 0), 2u) << "G3FAX directly after SOI";

	/* T.42 6.2.1.3's optional gamut and a 7500 K illuminant */
	img.colour.gamut.offset[2] = 128;
	img.colour.gamut.range[1] = 255;
	img.colour.gamut.range[2] = 255;
	img.colour.illuminant = T42_CT(7500);
	ASSERT_TRUE(jpeg_encode(img, JpegOptions(), &j));
	const unsigned char ct[] = {0xff, 0xe1, 0x00, 0x0c, 'G', '3', 'F', 'A', 'X', 0x02, 0x43, 0x54,
				    0x1d, 0x4c};
	p = g3fax(j, 2);
	ASSERT_NE(p, 0u);
	EXPECT_EQ(memcmp(&j[p], ct, sizeof(ct)), 0);
	ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
	EXPECT_TRUE(d.colour.gamut == img.colour.gamut);
	EXPECT_EQ(d.colour.illuminant, T42_CT(7500));
	EXPECT_EQ(d.dpi, 200);

	/* no segments: the defaults */
	ASSERT_TRUE(jpeg_encode(picture(16, 16, 1, 23), JpegOptions(), &j));
	ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
	EXPECT_TRUE(d.colour.gamut == T42Gamut());
	EXPECT_EQ(d.colour.illuminant, T42_D50);
	EXPECT_EQ(d.colour.bits, 8);

	/* YCC: no G3FAX1 or G3FAX2 (T.4 Annex I has none) */
	img.colour.space = T42_YCC;
	opt.colour_info = 1;
	ASSERT_TRUE(jpeg_encode(img, opt, &j));
	EXPECT_EQ(g3fax(j, 1), 0u);
	EXPECT_EQ(g3fax(j, 2), 0u);
}

/* T.42 6.2.1.3 and T.4 E.6.4: 12-bit white 4095, 2048, 1536; black 0;
   red as in 8 bits, 16 times finer; the 8-bit default the same as
   t42_rgb_to_lab */
TEST(Jpeg, Cielab12)
{
	T42Colour c;
	c.bits = 12;
	const unsigned char rgb[9] = {255, 255, 255, 0, 0, 0, 255, 0, 0};
	u16 lab[9];
	ASSERT_TRUE(t42_from_rgb(c, rgb, lab, 3));
	EXPECT_EQ(lab[0], 4095);
	EXPECT_EQ(lab[1], 2048);
	EXPECT_EQ(lab[2], 1536);
	EXPECT_EQ(lab[3], 0);
	EXPECT_EQ(lab[4], 2048);
	EXPECT_EQ(lab[5], 1536);
	EXPECT_NEAR(lab[6], 54.29 * 4095 / 100, 4);
	EXPECT_NEAR(lab[7], 80.80 * 4095 / 170 + 2048, 6);
	EXPECT_NEAR(lab[8], 69.89 * 4095 / 200 + 1536, 6);

	std::mt19937 rng(24);
	T42Colour c8;
	for (int i = 0; i < 500; i++) {
		unsigned char px[3], o8[3];
		u16 o16[3];
		for (auto &k : px)
			k = (unsigned char)rng();
		t42_rgb_to_lab(px, o8, 1);
		ASSERT_TRUE(t42_from_rgb(c8, px, o16, 1));
		for (int k = 0; k < 3; k++)
			EXPECT_LE(abs(o8[k] - o16[k]), 1);
	}
	/* 12 bits back closer than 8 bits */
	int worst8 = 0, worst12 = 0;
	for (int i = 0; i < 2000; i++) {
		int base = 70 + (int)(rng() % 120);
		unsigned char px[3], back[3];
		for (int k = 0; k < 3; k++)
			px[k] = (unsigned char)(base - 30 + (int)(rng() % 61));
		u16 s[3];
		ASSERT_TRUE(t42_from_rgb(c, px, s, 1));
		ASSERT_TRUE(t42_to_rgb(c, s, back, 1));
		for (int k = 0; k < 3; k++)
			worst12 = std::max(worst12, abs(px[k] - back[k]));
		ASSERT_TRUE(t42_from_rgb(c8, px, s, 1));
		ASSERT_TRUE(t42_to_rgb(c8, s, back, 1));
		for (int k = 0; k < 3; k++)
			worst8 = std::max(worst8, abs(px[k] - back[k]));
	}
	EXPECT_LE(worst12, 1);
	EXPECT_LT(worst12, worst8);
	for (int g = 0; g < 256; g++) {
		unsigned char gg = (unsigned char)g, b;
		u16 l;
		ASSERT_TRUE(t42_from_grey(c, &gg, &l, 1));
		ASSERT_TRUE(t42_to_grey(c, &l, &b, 1));
		EXPECT_LE(abs(g - b), 1) << g;
	}
}

/* T.42 6.2.1.3's optional gamut (a* and b* -128 to 127, offsets 128):
   red's a* and b* in it; saturated blue, which the default clips, back */
TEST(Jpeg, OptionalGamut)
{
	T42Colour c;
	c.gamut.offset[2] = 128;
	c.gamut.range[1] = 255;
	c.gamut.range[2] = 255;
	const unsigned char red[3] = {255, 0, 0}, blue[3] = {0, 0, 255};
	u16 s[3];
	ASSERT_TRUE(t42_from_rgb(c, red, s, 1));
	EXPECT_NEAR(s[1], 80.80 + 128, 1.5);
	EXPECT_NEAR(s[2], 69.89 + 128, 1.5);
	unsigned char back[3], back_def[3];
	ASSERT_TRUE(t42_from_rgb(c, blue, s, 1));
	ASSERT_TRUE(t42_to_rgb(c, s, back, 1));
	T42Colour def;
	ASSERT_TRUE(t42_from_rgb(def, blue, s, 1));
	EXPECT_EQ(s[2], 0) << "the default clips b* -107";
	ASSERT_TRUE(t42_to_rgb(def, s, back_def, 1));
	int e = 0, e_def = 0;
	for (int k = 0; k < 3; k++) {
		e = std::max(e, abs(back[k] - blue[k]));
		e_def = std::max(e_def, abs(back_def[k] - blue[k]));
	}
	EXPECT_LE(e, 3);
	EXPECT_GT(e_def, 20);
	/* a range of 0 is no gamut */
	c.gamut.range[0] = 0;
	EXPECT_FALSE(t42_from_rgb(c, red, s, 1));
}

/* T.4 E.6.7: the illuminants' white points; sRGB red in CIELAB D65 (L*
   53.24, a* 80.09, b* 67.20); white neutral under each illuminant */
TEST(Jpeg, Illuminants)
{
	double w[3];
	ASSERT_TRUE(t42_white(T42_D65, w));
	EXPECT_DOUBLE_EQ(w[0], 0.95045);
	EXPECT_DOUBLE_EQ(w[2], 1.08892);
	ASSERT_TRUE(t42_white(T42_SA, w));
	EXPECT_NEAR(w[0], 1.0985, 1e-4);
	/* 6504 K on the daylight locus is D65 */
	ASSERT_TRUE(t42_white(T42_CT(6504), w));
	EXPECT_NEAR(w[0], 0.9505, 2e-3);
	EXPECT_NEAR(w[2], 1.0889, 4e-3);
	/* 2856 K (Planckian) is illuminant A */
	ASSERT_TRUE(t42_white(T42_CT(2856), w));
	EXPECT_NEAR(w[0], 1.0985, 4e-3);
	EXPECT_NEAR(w[2], 0.3558, 4e-3);
	EXPECT_FALSE(t42_white(T42_CT(1000), w));
	EXPECT_FALSE(t42_white(0x12345678, w));

	T42Colour c;
	c.bits = 12;
	c.illuminant = T42_D65;
	const unsigned char red[3] = {255, 0, 0};
	u16 s[3];
	ASSERT_TRUE(t42_from_rgb(c, red, s, 1));
	EXPECT_NEAR(s[0], 53.24 * 4095 / 100, 4);
	EXPECT_NEAR(s[1], 80.09 * 4095 / 170 + 2048, 6);
	EXPECT_NEAR(s[2], 67.20 * 4095 / 200 + 1536, 6);
	for (u32 il : {T42_D50, T42_D65, T42_D75, T42_SA, T42_SC, T42_F2, T42_F7, T42_F11, T42_CT(5000)}) {
		c.illuminant = il;
		const unsigned char white[3] = {255, 255, 255}, px[3] = {200, 120, 60};
		ASSERT_TRUE(t42_from_rgb(c, white, s, 1)) << std::hex << il;
		EXPECT_NEAR(s[0], 4095, 2);
		EXPECT_NEAR(s[1], 2048, 2);
		EXPECT_NEAR(s[2], 1536, 2);
		unsigned char back[3];
		ASSERT_TRUE(t42_from_rgb(c, px, s, 1));
		ASSERT_TRUE(t42_to_rgb(c, s, back, 1));
		for (int k = 0; k < 3; k++)
			EXPECT_LE(abs(back[k] - px[k]), 1) << std::hex << il;
	}
	c.illuminant = 0x12345678;
	EXPECT_FALSE(t42_from_rgb(c, red, s, 1));
}

/* T.42 6.2.2: ITU-YCC, 8-bit white 255, 128, 128; red's Y, Cb, Cr from
   III-7; 12 bits; back; grey as Y */
TEST(Jpeg, Ycc)
{
	T42Colour c;
	c.space = T42_YCC;
	const unsigned char rgb[9] = {255, 255, 255, 0, 0, 0, 255, 0, 0};
	u16 s[9];
	ASSERT_TRUE(t42_from_rgb(c, rgb, s, 3));
	EXPECT_EQ(s[0], 255);
	EXPECT_EQ(s[1], 128);
	EXPECT_EQ(s[2], 128);
	EXPECT_EQ(s[3], 0);
	EXPECT_EQ(s[4], 128);
	EXPECT_EQ(s[5], 128);
	EXPECT_EQ(s[6], 76);                    /* 0.299 x 255 */
	EXPECT_EQ(s[7], 85);                    /* -0.1687 x 255 + 128 */
	EXPECT_EQ(s[8], 255);                   /* 0.5 x 255 + 128, clipped */
	c.bits = 12;
	ASSERT_TRUE(t42_from_rgb(c, rgb, s, 1));
	EXPECT_EQ(s[0], 4095);
	EXPECT_EQ(s[1], 2048);
	EXPECT_EQ(s[2], 2048);
	std::mt19937 rng(25);
	int worst = 0;
	for (int i = 0; i < 2000; i++) {
		unsigned char px[3], back[3];
		for (auto &k : px)
			k = (unsigned char)rng();
		ASSERT_TRUE(t42_from_rgb(c, px, s, 1));
		ASSERT_TRUE(t42_to_rgb(c, s, back, 1));
		for (int k = 0; k < 3; k++)
			worst = std::max(worst, abs(px[k] - back[k]));
	}
	EXPECT_LE(worst, 1);
	c.bits = 8;
	for (int g = 0; g < 256; g++) {
		unsigned char gg = (unsigned char)g, b;
		u16 y;
		ASSERT_TRUE(t42_from_grey(c, &gg, &y, 1));
		EXPECT_EQ(y, g);
		ASSERT_TRUE(t42_to_grey(c, &y, &b, 1));
		EXPECT_EQ(b, g);
	}
}

/* sRGB through t42_image_from_rgb, JPEG and t42_image_to_rgb: 12-bit
   CIELAB in another gamut and illuminant, and 8-bit YCC */
TEST(Jpeg, ImageRoundTrip)
{
	JpegImage rgb = picture(120, 64, 3, 26);
	T42Colour cs[2];
	cs[0].bits = 12;
	cs[0].gamut.offset[2] = 128;
	cs[0].gamut.range[1] = 255;
	cs[0].gamut.range[2] = 255;
	cs[0].illuminant = T42_D65;
	cs[1].space = T42_YCC;
	for (const T42Colour &c : cs) {
		JpegImage img;
		img.width = rgb.width;
		img.height = rgb.height;
		img.comps = 3;
		img.colour = c;
		ASSERT_TRUE(t42_image_from_rgb(&img, rgb.pix.data()));
		std::vector<unsigned char> j;
		JpegOptions opt;
		opt.quality = 95;
		opt.subsample = 0;
		ASSERT_TRUE(jpeg_encode(img, opt, &j));
		JpegImage d;
		ASSERT_EQ(jpeg_decode(j.data(), j.size(), 10000, &d), 0);
		EXPECT_EQ(d.colour.bits, c.bits);
		EXPECT_TRUE(d.colour.gamut == c.gamut || c.space == T42_YCC);
		/* (T.30 gives YCC, not the JPEG) */
		d.colour.space = c.space;
		JpegImage back = rgb;
		ASSERT_TRUE(t42_image_to_rgb(d, &back.pix));
		EXPECT_GT(psnr(rgb, back), 34) << c.space;
	}
}
