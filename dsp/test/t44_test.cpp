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
 *                   segments), and damaged pages; modes 2 and 3
 *                   (Annex A): the SLC form of each layer, text of a
 *                   few colours as a mask and a colour each, and an
 *                   N-layer page written by hand
 *
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
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

/* a SOSt of modes 2 and up: the layers there (one octet) */
void sost_a(Bytes *o, unsigned char type)
{
	o->insert(o->end(), {0xff, 0xed, 0x00, 0x07, 'M', 'R', 'C', 1, type});
}

/* an SLC (coder: Table 1 or 2's bit, image: Table 2, coded), extra
   segments, the EOH and the coded data */
void slc(Bytes *o, unsigned char number, bool coded, bool image, unsigned char bit, u32 res,
	 u32 w, u32 h, const unsigned char *base, u32 x, u32 y, const Bytes &data,
	 const Bytes &extra = Bytes())
{
	o->insert(o->end(), {0xff, 0xed, 0x00, 30, 'M', 'R', 'C', 2, number,
			     (unsigned char)((coded ? 1 : 0) | (image ? 2 : 0)), bit});
	put16(o, res);
	put32(o, w);
	put32(o, h);
	o->insert(o->end(), base, base + 3);
	put32(o, x);
	put32(o, y);
	o->insert(o->end(), extra.begin(), extra.end());
	o->insert(o->end(), {0xff, 0xed, 0x00, 10, 'M', 'R', 'C', 0xff});
	put32(o, (u32)data.size());
	o->insert(o->end(), data.begin(), data.end());
}

/* each SLC of a page: its layer number, in their order */
std::vector<int> slc_numbers(const Bytes &b)
{
	std::vector<int> n;
	for (size_t i = 0; i + 8 < b.size(); i++)
		if (b[i] == 0xff && b[i + 1] == 0xed && b[i + 2] == 0 && b[i + 3] == 30 &&
		    !memcmp(&b[i + 4], "MRC\x02", 4))
			n.push_back(b[i + 8]);
	return n;
}

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

/* the SOP gives 0 for the coder of a layer that no stripe codes (T.44
   9.2.1): black text on white has no image layers, a picture without
   text no mask */
TEST(T44, SopCodersOfLayersNotCoded)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 1728, 512, false, &text);
	for (size_t i = 0; i < text.size(); i++)
		if (text[i])
			memset(&img.pix[i * 3], 0, 3);
	Bytes out;
	t44_encode(img, T44Options(), &out);
	EXPECT_EQ(out[12], T44_MASK_JBIG);
	EXPECT_EQ(out[13], 0x00);

	T44Image pic = page(3, 1728, 300, true, &text);
	for (size_t i = 0; i < text.size(); i++)
		if (text[i])
			memcpy(&pic.pix[i * 3], &pic.pix[(i - 1) * 3], 3);
	t44_encode(pic, T44Options(), &out);
	EXPECT_EQ(out[12], 0x00);
	EXPECT_EQ(out[13], T44_IMAGE_JPEG);
	T44Image d;
	ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &d), 0);
	EXPECT_EQ(d.height, 300u);
}

/* a segment with a 2-octet length of 0: the 4-octet optional length
   comes after the identifier (T.44 9.2) */
TEST(T44, OptionalLength)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 400, 200, true, &text);
	Bytes out;
	t44_encode(img, T44Options(), &out);
	T44Image want;
	ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &want), 0);

	/* each APP13 segment (the SOP, the SOSts) with the long form */
	Bytes b(out.begin(), out.begin() + 2);
	size_t pos = 2;
	while (pos + 4 <= out.size() && out[pos] == 0xff && out[pos + 1] == 0xed) {
		size_t sl = (size_t)out[pos + 2] << 8 | out[pos + 3];
		size_t rest = sl - 2 - 4;   /* the parameters */
		Bytes seg = {0xff, 0xed, 0x00, 0x00};
		seg.insert(seg.end(), out.begin() + pos + 4, out.begin() + pos + 8);
		put32(&seg, (u32)(2 + 4 + 4 + rest));
		seg.insert(seg.end(), out.begin() + pos + 8, out.begin() + pos + 8 + rest);
		b.insert(b.end(), seg.begin(), seg.end());
		pos += 2 + sl;
		/* the TN, or the stripe's layers, up to the next segment */
		size_t next = pos;
		while (next + 1 < out.size() && !(out[next] == 0xff && out[next + 1] == 0xed &&
						   next + 8 <= out.size() &&
						   !memcmp(&out[next + 4], "MRC", 3)))
			next++;
		if (next + 1 >= out.size())
			next = out.size();
		b.insert(b.end(), out.begin() + pos, out.begin() + next);
		pos = next;
	}
	ASSERT_EQ(pos, out.size());
	ASSERT_GT(b.size(), out.size());
	T44Image d;
	ASSERT_EQ(t44_decode(b.data(), b.size(), 10000, &d), 0);
	EXPECT_EQ(d.height, want.height);
	EXPECT_TRUE(d.pix == want.pix);
	/* a length of 1 to 5 is reserved */
	Bytes r = out;
	r[5] = 5;
	EXPECT_EQ(t44_decode(r.data(), r.size(), 10000, &d), -1);
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

	/* mode 4 (shared data, JBIG2) is not taken */
	Bytes m4 = o;
	m4[11] = 4;
	EXPECT_EQ(t44_decode(m4.data(), m4.size(), 10000, &d), -1);
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

/* mode 2: the SOP tells version 0 and mode 2 (A.9.2.1); each stripe
   is a SOSt and an SLC and EOH for each layer, the main mask first,
   then the background and the foreground; the page decodes to the
   same pels as in mode 1 */
TEST(T44, Mode2)
{
	for (s16 comps : {3, 1}) {
		std::vector<unsigned char> text;
		T44Image img = page(comps, 1728, 600, true, &text);
		T44Options o1, o2;
		o2.mode = 2;
		Bytes b1, b2;
		t44_encode(img, o1, &b1);
		t44_encode(img, o2, &b2);
		EXPECT_EQ(b2[10], 0x00);
		EXPECT_EQ(b2[11], 0x02);
		EXPECT_EQ(b2[12], T44_MASK_JBIG);
		EXPECT_EQ(b2[13], T44_IMAGE_JPEG);
		std::vector<int> n = slc_numbers(b2);
		ASSERT_EQ(n.size() % 3, 0u);
		for (size_t i = 0; i < n.size(); i += 3)
			EXPECT_EQ(std::vector<int>(n.begin() + i, n.begin() + i + 3), std::vector<int>({2, 1, 3}));
		T44Image d1, d2;
		ASSERT_EQ(t44_decode(b1.data(), b1.size(), 10000, &d1), 0);
		ASSERT_EQ(t44_decode(b2.data(), b2.size(), 10000, &d2), 0);
		EXPECT_EQ(d2.width, d1.width);
		EXPECT_EQ(d2.height, d1.height);
		EXPECT_EQ(d2.comps, d1.comps);
		EXPECT_TRUE(d2.pix == d1.pix);
		printf("%s: mode 1 %zu octets, mode 2 %zu\n", comps == 3 ? "colour" : "grey", b1.size(),
		       b2.size());
		EXPECT_LT(b2.size(), b1.size() + 3 * 150);    /* (3 stripes) */
	}
	/* a picture without text: a virtual main mask (no coded data)
	   gives the height, the background has the pels */
	std::vector<unsigned char> text;
	T44Image pic = page(3, 400, 100, true, &text);
	for (size_t i = 0; i < text.size(); i++)
		if (text[i])
			memcpy(&pic.pix[i * 3], &pic.pix[(i - 1) * 3], 3);
	T44Options o2;
	o2.mode = 2;
	Bytes b;
	t44_encode(pic, o2, &b);
	EXPECT_EQ(slc_numbers(b), std::vector<int>({2, 1}));
	T44Image d;
	ASSERT_EQ(t44_decode(b.data(), b.size(), 10000, &d), 0);
	EXPECT_EQ(d.height, 100u);
	EXPECT_GT(psnr(pic.pix, d.pix), 38);
}

/* mode 3: black and dark red text on white has a mask and a colour
   for each colour (layers 2 and 3, 4 and 5), no foreground JPEG; the
   text comes back exact, the page smaller than in mode 1.  Text on a
   picture keeps its background JPEG */
TEST(T44, Mode3TextColours)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 1728, 512, false, &text);
	T44Options o1, o3;
	o3.mode = 3;
	Bytes b1, b3;
	t44_encode(img, o1, &b1);
	t44_encode(img, o3, &b3);
	EXPECT_EQ(b3[11], 0x03);
	std::vector<int> n = slc_numbers(b3);
	EXPECT_EQ(std::vector<int>(n.begin(), n.begin() + 5), std::vector<int>({2, 1, 3, 4, 5}));
	T44Image d1, d3;
	ASSERT_EQ(t44_decode(b1.data(), b1.size(), 10000, &d1), 0);
	ASSERT_EQ(t44_decode(b3.data(), b3.size(), 10000, &d3), 0);
	ASSERT_EQ(d3.comps, 3);
	ASSERT_EQ(d3.height, 512u);
	/* (the red through CIELAB octets and back) */
	unsigned char red[3] = {150, 0, 0}, lab[3], red2[3];
	t42_rgb_to_lab(red, lab, 1);
	t42_lab_to_rgb(lab, red2, 1);
	size_t bad = 0;
	for (size_t i = 0; i < text.size(); i++)
		for (int c = 0; c < 3; c++) {
			int want = img.pix[i * 3 + c];
			if (want == 150 || (img.pix[i * 3] == 150 && c))
				want = red2[c];
			bad += abs((int)d3.pix[i * 3 + c] - want) > 2;
		}
	EXPECT_EQ(bad, 0u);
	Bytes t1, t3, ot;
	for (size_t i = 0; i < text.size(); i++)
		if (text[i])
			for (int c = 0; c < 3; c++) {
				t1.push_back(d1.pix[i * 3 + c]);
				t3.push_back(d3.pix[i * 3 + c]);
				ot.push_back(img.pix[i * 3 + c]);
			}
	printf("two text colours: mode 1 %zu octets (text %.1f dB), mode 3 %zu (text %.1f dB)\n",
	       b1.size(), psnr(ot, t1), b3.size(), psnr(ot, t3));
	EXPECT_GT(psnr(ot, t3), psnr(ot, t1) + 5);
	EXPECT_LT(b3.size() * 2, b1.size());

	/* on a picture: the background JPEG stays, the text colours
	   have their masks */
	T44Image pic = page(3, 1728, 300, true, &text);
	Bytes bp;
	t44_encode(pic, o3, &bp);
	n = slc_numbers(bp);
	EXPECT_EQ(std::vector<int>(n.begin(), n.begin() + 5), std::vector<int>({2, 1, 3, 4, 5}));
	T44Image dp;
	ASSERT_EQ(t44_decode(bp.data(), bp.size(), 10000, &dp), 0);
	Bytes p, op;
	for (size_t i = 0; i < text.size(); i++)
		if (!text[i])
			for (int c = 0; c < 3; c++)
				p.push_back(dp.pix[i * 3 + c]), op.push_back(pic.pix[i * 3 + c]);
	EXPECT_GT(psnr(op, p), 40);
	/* one colour only (grey text): as mode 2 */
	T44Image g = page(1, 1728, 300, false, &text);
	t44_encode(g, o3, &bp);
	n = slc_numbers(bp);
	EXPECT_EQ(std::vector<int>(n.begin(), n.begin() + 3), std::vector<int>({2, 1, 3}));
	EXPECT_EQ(std::count(n.begin(), n.end(), 4), 0);
}

/* an N-layer page as another coder may write it (A.6.4, A.7.4): a
   stripe of 16 lines, 32 pels wide.  Layer 1: a grey JPEG of 8 by 4
   at 100 pels an inch at (8, 4) over a beige base colour.  Layer 2: an
   MMR main mask, the right half 1.  Layer 3: no coded data, red.
   Layer 4: an MH mask at 100 pels an inch, 8 by 4 (16 by 8 here) at
   (16, 8), all 1, with an encoder segment and foreign segments
   (APP1, T.43's APP3) before its EOH.  Layer 5: no coded data, blue.
   Layer 7: a JPEG at (0, 12), 8 by 4, no mask under it: drawn over its
   area */
TEST(T44, Mode3HandWritten)
{
	const u32 w = 32;
	Bytes o = sop(3, T44_MASK_MMR | T44_MASK_MH, T44_IMAGE_JPEG, 200, w);
	o[10] = 0;
	sost_a(&o, 0x7f);

	/* layer 2: MMR, white 16 black 16 on each of 16 rows (the first
	   in H mode: 001, white 16 = 101010, black 16 = 0000010111; the
	   others V0 V0) */
	Bytes mmr;
	{
		std::vector<int> bits;
		auto put = [&](const char *s) { for (; *s; s++) bits.push_back(*s - '0'); };
		for (int y = 0; y < 16; y++) {
			if (y == 0) {
				put("001"); put("101010"); put("0000010111");
			} else {
				put("1"); put("1");     /* V0, V0 */
			}
		}
		put("000000000001000000000001");
		mmr.assign((bits.size() + 7) / 8, 0);
		for (size_t i = 0; i < bits.size(); i++)
			if (bits[i])
				mmr[i / 8] |= (unsigned char)(0x80 >> (i % 8));
	}
	const unsigned char none[3] = {0, 0, 0};
	slc(&o, 2, true, false, 2, 200, w, 16, none, 0, 0, mmr);

	JpegImage j;
	j.width = 8;
	j.height = 4;
	j.comps = 1;
	j.pix.assign(32, 0x80);
	JpegOptions jo;
	jo.dpi = 100;
	Bytes jb;
	jpeg_encode(j, jo, &jb);
	const unsigned char beige[3] = {0xe0, 0x88, 0x70}, red[3] = {0x80, 0xe0, 0xc0},
			    blue[3] = {0x50, 0x90, 0x20};
	slc(&o, 1, true, true, 0, 100, 16, 8, beige, 8, 4, jb);
	slc(&o, 3, false, true, 0, 200, w, 16, red, 0, 0, Bytes());

	/* layer 4: MH, 8 by 4 all black at 100 pels an inch */
	T4Page m;
	m.init(8, 1);
	for (int y = 0; y < 4; y++) {
		unsigned char row[1] = {0xff};
		m.add_row(row);
	}
	T4Bits mb;
	m.encode_mh(&mb, 0, 0, 1);
	Bytes mh(mb.buf.begin(), mb.buf.begin() + (long)((mb.n + 7) / 8));
	mb.release();
	m.release();
	Bytes extra = {0xff, 0xed, 0x00, 0x08, 'M', 'R', 'C', 0x30, 0x12, 0x34,   /* EMSe */
		       0xff, 0xe1, 0x00, 0x04, 0xaa, 0xbb,                         /* APP1 */
		       0xff, 0xe3, 0x00, 0x00, 0x00, 0x0c, 'G', '3', 'F', 'A', 'X', 3,
		       0xff, 0xed};                                                /* APP3 */
	slc(&o, 4, true, false, 0, 100, 16, 8, none, 16, 8, mh, extra);
	slc(&o, 5, false, true, 0, 200, w, 16, blue, 0, 0, Bytes());
	/* layer 7 (no layer 6): the JPEG at 200, 8 by 4, at (0, 12) */
	jo.dpi = 200;
	Bytes jb2;
	j.pix.assign(32, 0x20);
	jpeg_encode(j, jo, &jb2);
	slc(&o, 7, true, true, 0, 200, 8, 4, white, 0, 12, jb2);
	o.insert(o.end(), {0xff, 0xd9, 0xff, 0xd9});

	T44Image d;
	ASSERT_EQ(t44_decode(o.data(), o.size(), 10000, &d), 0);
	ASSERT_EQ(d.width, w);
	ASSERT_EQ(d.height, 16u);
	ASSERT_EQ(d.comps, 3);
	unsigned char rgb_beige[3], rgb_red[3], rgb_blue[3], rgb_grey[3], rgb_dark[3];
	const unsigned char lab_grey[3] = {0x80, 0x80, 0x60}, lab_dark[3] = {0x20, 0x80, 0x60};
	t42_lab_to_rgb(beige, rgb_beige, 1);
	t42_lab_to_rgb(red, rgb_red, 1);
	t42_lab_to_rgb(blue, rgb_blue, 1);
	t42_lab_to_rgb(lab_grey, rgb_grey, 1);
	t42_lab_to_rgb(lab_dark, rgb_dark, 1);
	for (u32 y = 0; y < 16; y++)
		for (u32 x = 0; x < w; x++) {
			const unsigned char *e;
			int tol = 0;
			if (x < 8 && y >= 12)
				e = rgb_dark, tol = 3;          /* layer 7 */
			else if (x >= 16 && x < 32 && y >= 8)
				e = rgb_blue;                   /* layer 4's mask: 5 */
			else if (x >= 16)
				e = rgb_red;                    /* the main mask: 3 */
			else if (x >= 8 && y >= 4 && y < 12)
				e = rgb_grey, tol = 3;          /* layer 1's JPEG */
			else
				e = rgb_beige;                  /* layer 1's base */
			for (int c = 0; c < 3; c++)
				ASSERT_NEAR(d.pix[((size_t)y * w + x) * 3 + c], e[c], tol) << x << "," << y;
		}

	/* the same with the optional (4-octet) length in each SLC */
	Bytes b;
	for (size_t i = 0; i < o.size();) {
		if (i + 8 <= o.size() && o[i] == 0xff && o[i + 1] == 0xed && o[i + 2] == 0 &&
		    o[i + 3] == 30 && !memcmp(&o[i + 4], "MRC\x02", 4)) {
			b.insert(b.end(), {0xff, 0xed, 0x00, 0x00, 'M', 'R', 'C', 2});
			put32(&b, 30 + 4);
			b.insert(b.end(), o.begin() + (long)i + 8, o.begin() + (long)i + 32);
			i += 32;
		} else {
			b.push_back(o[i++]);
		}
	}
	T44Image d2;
	ASSERT_EQ(t44_decode(b.data(), b.size(), 10000, &d2), 0);
	EXPECT_TRUE(d2.pix == d.pix);
	/* no EOP: the stripe, early */
	EXPECT_EQ(t44_decode(o.data(), o.size() - 4, 10000, &d2), 1);
	EXPECT_EQ(d2.height, 16u);
}

/* modes 2 and 3, damaged: cut short, changed at random; never more
   than max_rows */
TEST(T44, Mode3Damaged)
{
	std::vector<unsigned char> text;
	T44Image img = page(3, 400, 400, true, &text);
	T44Options o;
	o.stripe = 100;
	o.mode = 3;
	Bytes out;
	t44_encode(img, o, &out);
	T44Image d;
	ASSERT_EQ(t44_decode(out.data(), out.size(), 10000, &d), 0);
	EXPECT_EQ(t44_decode(out.data(), out.size(), 150, &d), 1);
	EXPECT_EQ(d.height, 150u);
	ASSERT_EQ(t44_decode(out.data(), out.size() / 2, 10000, &d), 1);
	EXPECT_GT(d.height, 0u);
	EXPECT_LT(d.height, 400u);
	std::mt19937 rng(9);
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
