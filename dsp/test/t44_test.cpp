/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t44_test.cpp - dsp/t44.h: the page header's octets (T.44 9.2.1),
 *                   text over a picture there and back in colour and
 *                   grey with either mask coder (the text sharp, the
 *                   page far smaller than in JPEG), stripes of each
 *                   kind, a page written by hand (layer offsets, lower
 *                   resolutions, T.43 and MMR layers, optional
 *                   segments), and damaged pages
 *
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "jbig.h"
#include "jpeg.h"
#include "t4.h"
#include "t43.h"
#include "t44.h"

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

namespace {

/* a light picture (pic: else white) with lines of letters (2 pels
   thick strokes) in black, every third line dark red in colour; *text:
   where the letters are */
T44Image page(s16 comps, u32 w, u32 h, bool pic, std::vector<unsigned char> *text)
{
	T44Image img;
	img.width = w;
	img.height = h;
	img.comps = comps;
	img.pix.assign((size_t)w * h * comps, 255);
	text->assign((size_t)w * h, 0);
	std::mt19937 rng(3);
	for (u32 y = 0; pic && y < h; y++)
		for (u32 x = 0; x < w; x++)
			for (s16 c = 0; c < comps; c++)
				img.pix[((size_t)y * w + x) * comps + c] =
					(unsigned char)(205 + 35 * sin((x + 50.0 * c) / 90) * cos((y + 30.0 * c) / 60));
	for (u32 line = 0; (line + 1) * 40 < h; line++) {
		bool red = comps == 3 && line % 3 == 2;
		for (u32 ch = 0; 30 + (ch + 1) * 14 < w; ch++) {
			unsigned shape = rng();
			u32 x0 = 30 + ch * 14, y0 = 12 + line * 40;
			for (int k = 0; k < 6; k++) {
				if (!(shape >> k & 1))
					continue;
				u32 xa = k < 3 ? x0 : x0 + (k - 3) * 4, ya = k < 3 ? y0 + k * 7 : y0;
				u32 xb = k < 3 ? x0 + 10 : xa + 2, yb = k < 3 ? ya + 2 : y0 + 16;
				for (u32 y = ya; y < yb; y++)
					for (u32 x = xa; x < xb; x++) {
						(*text)[(size_t)y * w + x] = 1;
						for (s16 c = 0; c < comps; c++)
							img.pix[((size_t)y * w + x) * comps + c] =
								red && c == 0 ? 150 : 0;
					}
			}
		}
	}
	return img;
}

/* the PSNR of two runs of samples, dB */
double psnr(const Bytes &a, const Bytes &b)
{
	double e = 0;
	for (size_t i = 0; i < a.size(); i++)
		e += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
	return e ? 10 * log10(255.0 * 255 * a.size() / e) : 99;
}

/* the page in JPEG as T.42 has it */
size_t jpeg_size(const T44Image &img)
{
	JpegImage j;
	j.width = img.width;
	j.height = img.height;
	j.comps = img.comps;
	j.pix.resize(img.pix.size());
	if (img.comps == 3)
		t42_rgb_to_lab(img.pix.data(), j.pix.data(), (size_t)img.width * img.height);
	else
		t42_grey_to_l(img.pix.data(), j.pix.data(), (size_t)img.width * img.height);
	Bytes out;
	jpeg_encode(j, JpegOptions(), &out);
	return out.size();
}

/* a big-endian 16 bit number appended */
void put16(Bytes *o, u32 v)
{
	o->push_back((unsigned char)(v >> 8));
	o->push_back((unsigned char)v);
}

/* ... 32 */
void put32(Bytes *o, u32 v)
{
	put16(o, v >> 16);
	put16(o, v);
}

/* a SOP by hand: mode, the coders, resolution and width */
Bytes sop(unsigned char mode, unsigned char mask, unsigned char image, u32 res, u32 w)
{
	Bytes o = {0xff, 0xd8, 0xff, 0xed, 0x00, 0x10, 'M', 'R', 'C', 0, 2, mode, mask, image};
	put16(&o, res);
	put32(&o, w);
	o.push_back(0xff);
	o.push_back(0xd9);
	return o;
}

/* ... and a SOSt */
void sost(Bytes *o, unsigned char type, const unsigned char *bg, const unsigned char *fg,
	  u32 bgx, u32 bgy, u32 fgx, u32 fgy, u32 h, u32 mask_len)
{
	const unsigned char sost_hdr[] = {0xff, 0xed, 0x00, 37, 'M', 'R', 'C', 1};
	o->insert(o->end(), sost_hdr, sost_hdr + sizeof sost_hdr);
	o->push_back(type);
	o->insert(o->end(), bg, bg + 3);
	o->insert(o->end(), fg, fg + 3);
	put32(o, bgx);
	put32(o, bgy);
	put32(o, fgx);
	put32(o, fgy);
	put32(o, h);
	put32(o, mask_len);
}

const unsigned char white[3] = {0xff, 0x80, 0x60}, black[3] = {0x00, 0x80, 0x60};

} // namespace

/* the SOP of a page 1728 wide at 200 pels an inch, the mask in T.85,
   the images in JPEG; TN after it; EOP at the end */
TEST(T44, Header)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 1728, 100, true, &text);
	Bytes out;
	t44_encode(img, T44Options(), &out);
	ASSERT_GT(out.size(), 24u);
	EXPECT_EQ(Bytes(out.begin(), out.begin() + 24),
		  Bytes({0xff, 0xd8, 0xff, 0xed, 0x00, 0x10, 'M', 'R', 'C', 0x00, 0x02, 0x01,
			 0x08, 0x01, 0x00, 0xc8, 0x00, 0x00, 0x06, 0xc0, 0xff, 0xd9,
			 0xff, 0xed}));
	EXPECT_EQ(Bytes(out.end() - 4, out.end()), Bytes({0xff, 0xd9, 0xff, 0xd9}));
	T44Options mh;
	mh.mask_coder = T44_MASK_MH;
	t44_encode(img, mh, &out);
	EXPECT_EQ(out[12], 0x01);
}

/* text over a picture, colour and grey, either mask: the text comes
   back close (grey: exact), the picture too; the page a fraction of its
   JPEG; both masks the same page */
TEST(T44, RoundTrip)
{
	for (s16 comps : {3, 1}) {
		std::vector<unsigned char> text;
		T44Image img = page(comps, 1728, 700, true, &text);
		Bytes first;
		for (s16 coder : {T44_MASK_JBIG, T44_MASK_MH}) {
			T44Options o;
			o.mask_coder = coder;
			Bytes out;
			t44_encode(img, o, &out);
			T44Image d;
			ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &d), 0);
			ASSERT_EQ(d.width, img.width);
			ASSERT_EQ(d.height, img.height);
			ASSERT_EQ(d.comps, comps);
			EXPECT_EQ(d.dpi, 200);
			Bytes t, ot, p, op;
			for (size_t i = 0; i < text.size(); i++)
				for (s16 c = 0; c < comps; c++)
					(text[i] ? t : p).push_back(d.pix[i * comps + c]),
					(text[i] ? ot : op).push_back(img.pix[i * comps + c]);
			double qt = psnr(ot, t), qp = psnr(op, p);
			size_t js = jpeg_size(img);
			printf("%s, %s mask: %zu octets (JPEG %zu), text %.1f dB, the rest %.1f dB\n",
			       comps == 3 ? "colour" : "grey", coder == T44_MASK_JBIG ? "T.85" : "MH",
			       out.size(), js, qt, qp);
			EXPECT_GT(qt, comps == 1 ? 45 : 30);
			EXPECT_GT(qp, 40);
			EXPECT_LT(out.size() * (coder == T44_MASK_JBIG ? 4 : 2), js);
			if (first.empty())
				first = d.pix;
			else
				EXPECT_TRUE(first == d.pix);
		}
	}
}

/* black text on white: its stripes the mask alone (the colours fixed),
   exact, and tiny; a picture without text: the background alone at
   full resolution; a white page: masks of nothing */
TEST(T44, Stripes)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 1728, 512, false, &text);
	for (size_t i = 0; i < text.size(); i++)
		if (text[i])
			memset(&img.pix[i * 3], 0, 3);
	Bytes out;
	t44_encode(img, T44Options(), &out);
	T44Image d;
	ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &d), 0);
	ASSERT_EQ(d.comps, 1);          /* (black and white: all grey) */
	for (size_t i = 0; i < text.size(); i++)
		ASSERT_EQ(d.pix[i], text[i] ? 0 : 255) << i;
	printf("black text on white: %zu octets\n", out.size());
	EXPECT_LT(out.size(), 20000u);
	/* the SOSts: mask only (type 2) */
	for (size_t i = 0; i + 9 < out.size(); i++)
		if (!memcmp(&out[i], "MRC\x01", 4)) {
			EXPECT_EQ(out[i + 4], 0x02);
		}

	T44Image pic = page(3, 1728, 300, true, &text);
	for (size_t i = 0; i < text.size(); i++)
		if (text[i])
			memcpy(&pic.pix[i * 3], &pic.pix[(i - 1) * 3], 3);
	EXPECT_FALSE(t44_worth(pic));
	t44_encode(pic, T44Options(), &out);
	ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &d), 0);
	EXPECT_GT(psnr(pic.pix, d.pix), 38);
	for (size_t i = 0; i + 9 < out.size(); i++)
		if (!memcmp(&out[i], "MRC\x01", 4)) {
			EXPECT_EQ(out[i + 4], 0x01);
		}

	T44Image blank;
	blank.width = 64;
	blank.height = 10;
	blank.comps = 1;
	blank.pix.assign(640, 255);
	EXPECT_FALSE(t44_worth(blank));
	t44_encode(blank, T44Options(), &out);
	ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &d), 0);
	EXPECT_EQ(d.pix, blank.pix);
}

/* a page as another coder may write it: an optional segment (MRC10),
   a stripe of a background JPEG of 8 by 4 at 100 pels an inch, offset
   (6, 2), over its colour; a stripe of a T.43 foreground (red) through
   an MMR mask of a square; a stripe of only a foreground (the mask 1
   everywhere) beside its colour */
TEST(T44, HandWritten)
{
	const u32 w = 32;
	Bytes o = sop(1, T44_MASK_MMR, T44_IMAGE_JPEG | T44_IMAGE_T43, 200, w);
	const unsigned char gamut[] = {0xff, 0xed, 0x00, 0x12, 'M', 'R', 'C', 0x0a,
				       0, 0, 0, 100, 0, 128, 0, 170, 0, 96, 0, 200};
	o.insert(o.end(), gamut, gamut + sizeof gamut);

	/* 1: the background, mid grey, 8 by 4 at 100 */
	const unsigned char beige[3] = {0xe0, 0x88, 0x70};
	JpegImage j;
	j.width = 8;
	j.height = 4;
	j.comps = 1;
	j.pix.assign(32, 0x80);
	JpegOptions jo;
	jo.dpi = 100;
	Bytes jb;
	jpeg_encode(j, jo, &jb);
	sost(&o, 0x01, beige, black, 6, 2, 0, 0, 12, 0);
	o.insert(o.end(), jb.begin(), jb.end());

	/* 2: an MMR mask, a square of 8 at (4, 2), the foreground red in
	   T.43 (a palette of one colour) under it */
	T4Page m;
	m.init((s16)w, 1);
	for (u32 y = 0; y < 12; y++) {
		unsigned char row[4] = {0, 0, 0, 0};
		if (y >= 2 && y < 10)
			row[0] = 0x0f, row[1] = 0xf0;
		m.add_row(row);
	}
	/* (MMR by hand: its rows by T.6's vertical modes) */
	Bytes mmr;
	{
		/* each row against the one above: V0 for every change
		   (1), but where the square starts and ends: pass and
		   horizontal codes, simpler as the MH-coded rows of T.4
		   2D, which MMR is without EOLs; here the T.4 decoder's
		   own MMR is checked against rows we know, so code them
		   with horizontal mode: 001 + white run + black run */
		std::vector<int> bits;
		auto put = [&](const char *s) { for (; *s; s++) bits.push_back(*s - '0'); };
		for (u32 y = 0; y < 12; y++) {
			if (y >= 2 && y < 10) {
				/* H: white 4 (1011), black 8 (000101); then H:
				   white 20 (0001000), black 0 (0000110111) */
				put("001"); put("1011"); put("000101");
				put("001"); put("0001000"); put("0000110111");
			} else {
				/* H: white 32 (00011011), black 0 */
				put("001"); put("00011011"); put("0000110111");
			}
		}
		put("000000000001000000000001");      /* EOFB */
		mmr.assign((bits.size() + 7) / 8, 0);
		for (size_t i = 0; i < bits.size(); i++)
			if (bits[i])
				mmr[i / 8] |= (unsigned char)(0x80 >> (i % 8));
	}
	T43Image t;
	t.type = T43_PALETTE;
	t.bits[0] = 1;
	t.width = 16;
	t.height = 12;
	t.dpi = 200;
	t.palette = {0x80, 0xe0, 0xc0, 0x80, 0xe0, 0xc0};   /* (two entries the same) */
	t.pix.assign(16 * 12, 0);
	Bytes tb;
	t43_encode(t, T43Options(), &tb);
	sost(&o, 0x06, white, black, 0, 0, 0, 0, 12, (u32)mmr.size());
	o.insert(o.end(), mmr.begin(), mmr.end());
	o.insert(o.end(), tb.begin(), tb.end());

	/* 3: only a foreground, the JPEG at (16, 0); the mask 1: its
	   colour (blue) beside it */
	const unsigned char blue[3] = {0x50, 0x90, 0x20};
	sost(&o, 0x04, white, blue, 0, 0, 16, 0, 4, 0);
	o.insert(o.end(), jb.begin(), jb.end());
	o.insert(o.end(), {0xff, 0xd9, 0xff, 0xd9});

	T44Image d;
	ASSERT_EQ(t44_decode(o.data(), o.size(), 10000, &d), 0);
	ASSERT_EQ(d.width, w);
	ASSERT_EQ(d.height, 28u);
	ASSERT_EQ(d.comps, 3);
	unsigned char rgb_beige[3], rgb_red[3], rgb_blue[3], rgb_grey[3];
	const unsigned char lab_grey[3] = {0x80, 0x80, 0x60}, lab_red[3] = {0x80, 0xe0, 0xc0};
	t42_lab_to_rgb(beige, rgb_beige, 1);
	t42_lab_to_rgb(lab_red, rgb_red, 1);
	t42_lab_to_rgb(blue, rgb_blue, 1);
	t42_lab_to_rgb(lab_grey, rgb_grey, 1);
	auto at = [&](u32 x, u32 y) { return &d.pix[((size_t)y * w + x) * 3]; };
	/* 1: the JPEG's 8 by 4 at 100 is 16 by 8 at 200, from (6, 2) */
	for (u32 y = 0; y < 12; y++)
		for (u32 x = 0; x < w; x++) {
			bool in = x >= 6 && x < 22 && y >= 2 && y < 10;
			const unsigned char *e = in ? rgb_grey : rgb_beige;
			for (int c = 0; c < 3; c++)
				ASSERT_NEAR(at(x, y)[c], e[c], in ? 3 : 0) << x << "," << y;
		}
	/* 2: red in the square (the T.43 covers it), white around */
	for (u32 y = 0; y < 12; y++)
		for (u32 x = 0; x < w; x++) {
			bool in = x >= 4 && x < 12 && y >= 2 && y < 10;
			for (int c = 0; c < 3; c++)     /* (T.43 through sRGB) */
				ASSERT_NEAR(at(x, 12 + y)[c], in ? rgb_red[c] : 255, in ? 2 : 0) << x << "," << y;
		}
	/* 3: blue, the grey JPEG from x 16 */
	for (u32 y = 0; y < 4; y++)
		for (u32 x = 0; x < w; x++)
			for (int c = 0; c < 3; c++)
				ASSERT_NEAR(at(x, 24 + y)[c], x >= 16 ? rgb_grey[c] : rgb_blue[c], x >= 16 ? 3 : 0)
					<< x << "," << y;

	/* mode 2's SOSt is another: not taken */
	Bytes m2 = o;
	m2[11] = 2;
	EXPECT_EQ(t44_decode(m2.data(), m2.size(), 10000, &d), -1);
	m.release();
}

/* damaged pages: cut short (the stripes so far), changed at random;
   never more than max_rows */
TEST(T44, Damaged)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 400, 600, true, &text);
	T44Options o;
	o.stripe = 100;
	Bytes out;
	t44_encode(img, o, &out);
	T44Image d;
	EXPECT_EQ(t44_decode(out.data(), out.size(), 250, &d), 1);
	EXPECT_EQ(d.height, 250u);
	ASSERT_EQ(t44_decode(out.data(), out.size() / 2, 10000, &d), 1);
	EXPECT_GT(d.height, 0u);
	EXPECT_LT(d.height, 600u);
	EXPECT_EQ(d.height % 100, 0u);
	EXPECT_EQ(t44_decode(out.data(), 10, 10000, &d), -1);
	std::mt19937 rng(5);
	for (int k = 0; k < 300; k++) {
		Bytes b = out;
		for (int e = 0; e < 1 + k % 5; e++)
			b[rng() % b.size()] = (unsigned char)rng();
		s16 r = t44_decode(b.data(), b.size(), 1000, &d);
		if (r >= 0) {
			EXPECT_LE(d.height, 1000u);
			EXPECT_EQ(d.pix.size(), (size_t)d.width * d.height * d.comps);
		}
	}
}
