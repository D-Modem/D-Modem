/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t44.cpp - ITU-T T.44 mixed raster content, modes 1 to 3 (T.4
 *              Annex H, T.44 Annex A)
 *
 * A page is ff d8 and the start of page (SOP: ff ed, its length,
 * "MRC" 0, the version, the mode, the mask's and the image layers'
 * coders, the mask's resolution, the width), ff d9, optional segments
 * (ff ed "MRC" n), then its stripes from the top; ff d9 ff d9 ends it.
 *
 * Mode 1: each stripe is a start of stripe (SOSt: ff ed, "MRC" 1, the
 * layers there, the background's and the foreground's colours, their
 * offsets, the height, the mask's length) and its layers: the mask
 * (that many octets), the background, the foreground (each a JPEG or a
 * T.43 BCIE, which ends itself).  Where the mask is 1 the foreground
 * shows, else the background; a layer missing is its colour, a mask
 * missing all 0 (or 1 with only a foreground).
 *
 * Modes 2 and 3 (Annex A): the SOSt has only the layers there.  Each
 * layer is a start of layer (SLC: ff ed, "MRC" 2, the layer number,
 * the coder, the resolution, the width and height, the base colour,
 * the offset), encoder segments, the end of header (EOH: ff ed,
 * "MRC" 255, the length of the coded data) and the coded data.  The
 * first SLC is the main mask's (layer 2, coded or not); its height is
 * the stripe's.  Then come the background (1), the foreground (3) and,
 * in mode 3, more pairs of a mask (4, 6, ...) and an image (5, 7, ...).
 * The image layers are drawn from 1 up: each through the mask below it
 * (where the mask is 1), or over its area when it has no mask.
 *
 * Colours are T.42's CIELAB octets.
 *
 * Our stripes: the mask has the dark pels at an edge (text and lines),
 * the foreground their colour (one colour, or a JPEG at image_dpi),
 * the background the rest (one colour, or a JPEG at image_dpi; a
 * stripe without text has the background alone, at the mask's
 * resolution).  In mode 3, text of a few flat colours has a mask and
 * a colour for each colour, and no foreground JPEG.
 *
 */

#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "jbig.h"
#include "jpeg.h"
#include "t4.h"
#include "t43.h"
#include "t44.h"

namespace dsp {

namespace {

#define APP13           0xed
#define APP3            0xe3    /* T.43's G3FAX3 (a 4-octet length) */
#define M_SOI           0xd8
#define M_EOI           0xd9
#define T43_MAGIC       0xa8
#define MRC_VERSION     2       /* mode 1's (T.44 9.2.1) */
#define MRC_VERSION_A   0       /* modes 2 and 3's (A.9.2.1) */
#define ID_SOP          0
#define ID_SOST         1
#define ID_SLC          2
#define ID_EOH          255
#define MAX_WIDTH       4864    /* A3 at 400 pels an inch */
#define MAX_LAYER       28      /* the layers a type of stripe of 4
				   octets can tell */
#define SOP_MASK_CODER  12      /* the octet of the SOP's mask coder
				   (after ff d8, APP13, the length, "MRC" 0,
				   the version and the mode); the image
				   layers' coder after it */
#define SLC_LEN         30      /* an SLC's length (with a coder of 2
				   octets) */
#define EOH_LEN         10

/* Table 1's bit numbers (the SLC's coder) */
#define BIT_MH          0
#define BIT_MR          1
#define BIT_MMR         2
#define BIT_JBIG        3
#define BIT_JBIG2       4

#define MASK_DARK       128     /* a mask pel: its lightness below */
#define MASK_EDGE       80      /* ... and lighter by this near it */
#define MASK_REACH      3       /* ... "near": pels each way */
#define BG_FLAT         12      /* a background in one colour: no
				   sample further from the mean */
#define FG_FLAT         10      /* a foreground: on average, in each
				   component */
#define TEXT_COLOURS    6       /* mode 3: the most text colours in a
				   stripe, a mask each */
#define COLOUR_NEAR     48      /* ... a pel of a colour: its samples
				   this near the colour's (the sum of
				   the differences) */

/* the CIELAB octets of white and black (T.44 9.3) */
const unsigned char lab_white[3] = {0xff, 0x80, 0x60};
const unsigned char lab_black[3] = {0x00, 0x80, 0x60};

/* a big-endian 16 bit number appended */
void put16(std::vector<unsigned char> *out, u32 v)
{
	out->push_back((unsigned char)(v >> 8));
	out->push_back((unsigned char)v);
}

/* ... 32 */
void put32(std::vector<unsigned char> *out, u32 v)
{
	put16(out, v >> 16);
	put16(out, v);
}

/* a big-endian 16 bit number */
u32 get16(const unsigned char *p)
{
	return (u32)p[0] << 8 | p[1];
}

/* ... 32 */
u32 get32(const unsigned char *p)
{
	return get16(p) << 16 | get16(p + 2);
}

/* a segment's start: APP13, its length (with itself) and "MRC" n */
void segment(std::vector<unsigned char> *out, u32 len, unsigned char n)
{
	out->push_back(0xff);
	out->push_back(APP13);
	put16(out, len);
	out->push_back('M');
	out->push_back('R');
	out->push_back('C');
	out->push_back(n);
}

/* the header of the APP13 segment at data[pos] (its ff ed): *seg_len
   its length after the APP13, *params where its parameters start.  A
   length of 0 tells that the 4-octet optional length follows the
   identifier (T.44 9.2); 1 to 5 are reserved.  false when the segment
   is not whole or its length is not valid */
bool seg_head(const unsigned char *data, size_t len, size_t pos, size_t *seg_len, size_t *params)
{
	if (pos + 4 > len || data[pos] != 0xff || data[pos + 1] != APP13)
		return false;
	size_t sl = get16(data + pos + 2), hl = 2 + 4;
	if (sl == 0) {
		if (pos + 12 > len)
			return false;
		sl = get32(data + pos + 8);
		hl = 2 + 4 + 4;
	}
	if (sl < hl || sl > len - pos - 2)
		return false;
	*seg_len = sl;
	*params = pos + 2 + hl;
	return true;
}

/* each pel's lightness */
void lightness(const T44Image &img, std::vector<unsigned char> *y)
{
	size_t n = (size_t)img.width * img.height;
	y->resize(n);
	for (size_t i = 0; i < n; i++) {
		const unsigned char *s = img.pix.data() + i * img.comps;
		(*y)[i] = img.comps == 3 ?
			(unsigned char)((299 * s[0] + 587 * s[1] + 114 * s[2]) / 1000) : s[0];
	}
}

/* the mask: dark pels with a light one within MASK_REACH (the lightest
   around by a max filter, a row's then a column's) */
void find_mask(const T44Image &img, std::vector<unsigned char> *mask)
{
	const u32 w = img.width, h = img.height;
	std::vector<unsigned char> y, hm;
	lightness(img, &y);
	hm.resize(y.size());
	for (u32 r = 0; r < h; r++) {
		const unsigned char *s = y.data() + (size_t)r * w;
		unsigned char *o = hm.data() + (size_t)r * w;
		for (u32 x = 0; x < w; x++) {
			u32 a = x >= MASK_REACH ? x - MASK_REACH : 0, b = std::min(w - 1, x + MASK_REACH);
			o[x] = *std::max_element(s + a, s + b + 1);
		}
	}
	mask->assign(y.size(), 0);
	for (u32 r = 0; r < h; r++) {
		u32 a = r >= MASK_REACH ? r - MASK_REACH : 0, b = std::min(h - 1, r + MASK_REACH);
		for (u32 x = 0; x < w; x++) {
			size_t i = (size_t)r * w + x;
			if (y[i] >= MASK_DARK)
				continue;
			unsigned char mx = 0;
			for (u32 k = a; k <= b; k++)
				mx = std::max(mx, hm[(size_t)k * w + x]);
			if (mx - y[i] >= MASK_EDGE)
				(*mask)[i] = 1;
		}
	}
}

/* sRGB or grey samples to CIELAB octets (grey: L*, a* b* 0) */
void to_lab(const unsigned char *s, s16 comps, unsigned char *lab, size_t n)
{
	if (comps == 3) {
		t42_rgb_to_lab(s, lab, n);
		return;
	}
	std::vector<unsigned char> l(n);
	t42_grey_to_l(s, l.data(), n);
	for (size_t i = 0; i < n; i++) {
		lab[i * 3] = l[i];
		lab[i * 3 + 1] = 0x80;
		lab[i * 3 + 2] = 0x60;
	}
}

/* the pels of a stripe (rows y0 to y0 + h) where the mask is sel: their
   mean, and whether all are within flat of it (max: the furthest, else
   on average).  false when there are none */
bool layer_colour(const T44Image &img, const std::vector<unsigned char> &mask, u32 y0, u32 h,
		  unsigned char sel, s16 flat, bool max, unsigned char *mean, bool *is_flat)
{
	const s16 nc = img.comps;
	u64 sum[3] = {0, 0, 0}, n = 0;
	size_t a = (size_t)y0 * img.width, b = a + (size_t)h * img.width;
	for (size_t i = a; i < b; i++)
		if (mask[i] == sel) {
			for (s16 c = 0; c < nc; c++)
				sum[c] += img.pix[i * nc + c];
			n++;
		}
	if (!n)
		return false;
	for (s16 c = 0; c < nc; c++)
		mean[c] = (unsigned char)((sum[c] + n / 2) / n);
	u64 dev[3] = {0, 0, 0};
	s16 worst = 0;
	for (size_t i = a; i < b; i++)
		if (mask[i] == sel)
			for (s16 c = 0; c < nc; c++) {
				s16 d = (s16)abs((int)img.pix[i * nc + c] - mean[c]);
				dev[c] += d;
				worst = std::max(worst, d);
			}
	*is_flat = max ? worst <= flat :
		dev[0] <= (u64)flat * n && dev[1] <= (u64)flat * n && dev[2] <= (u64)flat * n;
	return true;
}

/* an image layer of the stripe: its pels where the mask is sel (all,
   sel 2), f by f of them averaged, the others filled from the left
   (or above, or the mean); in JPEG at dpi */
void image_layer(const T44Image &img, const std::vector<unsigned char> &mask, u32 y0, u32 h,
		 unsigned char sel, u32 f, const unsigned char *mean, const T44Options &opt,
		 s16 dpi, std::vector<unsigned char> *out)
{
	const s16 nc = img.comps;
	const u32 lw = (img.width + f - 1) / f, lh = (h + f - 1) / f;
	std::vector<unsigned char> s((size_t)lw * lh * nc);
	std::vector<unsigned char> have((size_t)lw * lh, 0);
	for (u32 ly = 0; ly < lh; ly++)
		for (u32 lx = 0; lx < lw; lx++) {
			u32 sum[3] = {0, 0, 0}, n = 0;
			for (u32 y = ly * f; y < std::min(h, ly * f + f); y++)
				for (u32 x = lx * f; x < std::min(img.width, lx * f + f); x++) {
					size_t i = (size_t)(y0 + y) * img.width + x;
					if (sel != 2 && mask[i] != sel)
						continue;
					for (s16 c = 0; c < nc; c++)
						sum[c] += img.pix[i * nc + c];
					n++;
				}
			size_t o = (size_t)ly * lw + lx;
			if (n) {
				have[o] = 1;
				for (s16 c = 0; c < nc; c++)
					s[o * nc + c] = (unsigned char)((sum[c] + n / 2) / n);
			}
		}
	for (u32 ly = 0; ly < lh; ly++)
		for (u32 lx = 0; lx < lw; lx++) {
			size_t o = (size_t)ly * lw + lx;
			if (have[o])
				continue;
			const unsigned char *from = mean;
			if (lx && have[o - 1])
				from = &s[(o - 1) * nc];
			else if (ly && have[o - lw])
				from = &s[(o - lw) * nc];
			memcpy(&s[o * nc], from, nc);
			have[o] = 1;
		}
	JpegImage j;
	j.width = lw;
	j.height = lh;
	j.comps = nc;
	j.pix.resize(s.size());
	if (nc == 3)
		t42_rgb_to_lab(s.data(), j.pix.data(), (size_t)lw * lh);
	else
		t42_grey_to_l(s.data(), j.pix.data(), (size_t)lw * lh);
	JpegOptions jo;
	jo.quality = opt.quality;
	jo.dpi = dpi;
	std::vector<unsigned char> d;
	jpeg_encode(j, jo, &d);
	out->insert(out->end(), d.begin(), d.end());
}

/* coder (or layer) bits as T.44's octets: 7 bits in each, the lowest
   first, bit 7 set where another octet follows */
void put_coders(std::vector<unsigned char> *out, u32 bits)
{
	do {
		unsigned char c = (unsigned char)(bits & 0x7f);
		bits >>= 7;
		if (bits)
			c |= 0x80;
		out->push_back(c);
	} while (bits);
}

/* a bitmap (a pel an octet, 0 or 1; stride octets a row) of w by h
   pels from (x0, y0), coded as Table 1 bit coder (BIT_MH, else
   BIT_JBIG) */
void code_bitmap(const unsigned char *m, u32 stride, u32 x0, u32 y0, u32 w, u32 h, s16 coder,
		 std::vector<unsigned char> *out)
{
	T4Page p;
	p.init((s16)w, 1);
	s16 rs = T4_STRIDE(p.width);
	std::vector<unsigned char> row(rs);
	for (u32 y = 0; y < h; y++) {
		std::fill(row.begin(), row.end(), 0);
		const unsigned char *r = m + (size_t)(y0 + y) * stride + x0;
		for (u32 x = 0; x < w; x++)
			if (r[x])
				row[x / 8] |= (unsigned char)(0x80 >> (x % 8));
		p.add_row(row.data());
	}
	if (coder == BIT_MH) {
		/* EOLs, RTC; its bits from the top of the first octet */
		T4Bits b;
		p.encode_mh(&b, 0, 0, 1);
		out->insert(out->end(), b.buf.begin(), b.buf.begin() + (long)((b.n + 7) / 8));
		b.release();
	} else {
		std::vector<unsigned char> bie;
		jbig_encode(&p, JbigOptions(), &bie);
		out->insert(out->end(), bie.begin(), bie.end());
	}
	p.release();
}

/* a layer of a stripe as the encoder makes it */
struct EncLayer {
	u32 number = 0;
	bool coded = false;
	bool image = false;             /* Table 2's coder (else Table 1's) */
	s16 bit = 0;                    /* the coder's bit number */
	s16 res = 0;
	u32 off[2] = {0, 0};
	u32 w = 0, h = 0;               /* in mask pels */
	unsigned char base[3] = {0, 0, 0};
	std::vector<unsigned char> data;
};

/* mode 3: the colours of the stripe's mask pels (rows y0 to y0 + h), a
   pel near a colour's mean (COLOUR_NEAR) in it, else a new colour.  The
   number of colours (the most pels first), *id each mask pel's colour
   (in the stripe), means[] their sRGB or grey means; 0 when there are
   more than TEXT_COLOURS, or a colour is not flat (FG_FLAT) */
s16 text_colours(const T44Image &img, const std::vector<unsigned char> &mask, u32 y0, u32 h,
		 std::vector<unsigned char> *id, unsigned char means[][3])
{
	const s16 nc = img.comps;
	const size_t a = (size_t)y0 * img.width, n = (size_t)h * img.width;
	u64 sum[TEXT_COLOURS][3] = {}, cnt[TEXT_COLOURS] = {};
	s16 k = 0;
	id->assign(n, 0xff);
	for (size_t i = 0; i < n; i++) {
		if (!mask[a + i])
			continue;
		const unsigned char *s = &img.pix[(a + i) * nc];
		s16 best = -1;
		s32 bd = 0;
		for (s16 c = 0; c < k; c++) {
			s32 d = 0;
			for (s16 j = 0; j < nc; j++)
				d += abs((s32)s[j] - (s32)((sum[c][j] + cnt[c] / 2) / cnt[c]));
			if (best < 0 || d < bd)
				best = c, bd = d;
		}
		if (best < 0 || bd > COLOUR_NEAR) {
			if (k == TEXT_COLOURS)
				return 0;
			best = k++;
		}
		for (s16 j = 0; j < nc; j++)
			sum[best][j] += s[j];
		cnt[best]++;
		(*id)[i] = (unsigned char)best;
	}
	/* the most pels first */
	s16 order[TEXT_COLOURS], rank[TEXT_COLOURS];
	for (s16 c = 0; c < k; c++)
		order[c] = c;
	std::stable_sort(order, order + k, [&](s16 x, s16 y) { return cnt[x] > cnt[y]; });
	for (s16 c = 0; c < k; c++) {
		rank[order[c]] = c;
		for (s16 j = 0; j < 3; j++)
			means[c][j] = j < nc ? (unsigned char)((sum[order[c]][j] + cnt[order[c]] / 2) /
							      cnt[order[c]]) : 0;
	}
	u64 dev[TEXT_COLOURS][3] = {};
	for (size_t i = 0; i < n; i++) {
		if ((*id)[i] == 0xff)
			continue;
		s16 c = rank[(*id)[i]];
		(*id)[i] = (unsigned char)c;
		for (s16 j = 0; j < nc; j++)
			dev[c][j] += (u64)abs((s32)img.pix[(a + i) * nc + j] - means[c][j]);
	}
	for (s16 c = 0; c < k; c++)
		for (s16 j = 0; j < nc; j++)
			if (dev[c][j] > (u64)FG_FLAT * cnt[order[c]])
				return 0;
	return k;
}

/* the SOP, mode 1's or modes 2 and 3's */
void put_sop(std::vector<unsigned char> *out, s16 mode, s16 coder, s16 res, u32 w)
{
	out->push_back(0xff);
	out->push_back(M_SOI);
	segment(out, 2 + 4 + 6 + 4, ID_SOP);
	out->push_back(mode == 1 ? MRC_VERSION : MRC_VERSION_A);
	out->push_back((unsigned char)mode);
	out->push_back((unsigned char)coder);
	out->push_back(T44_IMAGE_JPEG);
	put16(out, (u32)res);
	put32(out, w);
	out->push_back(0xff);
	out->push_back(M_EOI);              /* the termination number */
}

/* a stripe in mode 1: the SOSt (the layers with coded data, the
   background's and the foreground's colours, no offsets), the mask,
   the background, the foreground */
void put_stripe_mode1(std::vector<unsigned char> *out, const std::vector<EncLayer> &ls, u32 rows)
{
	const EncLayer *l1 = nullptr, *l2 = nullptr, *l3 = nullptr;
	for (const EncLayer &l : ls)
		(l.number == 1 ? l1 : l.number == 2 ? l2 : l3) = &l;
	unsigned char type = 0;
	if (l1 && l1->coded)
		type |= 0x01;
	if (l2 && l2->coded)
		type |= 0x02;
	if (l3 && l3->coded)
		type |= 0x04;
	segment(out, 2 + 4 + 1 + 3 + 3 + 8 + 8 + 4 + 4, ID_SOST);
	out->push_back(type);
	out->insert(out->end(), l1 ? l1->base : lab_white, (l1 ? l1->base : lab_white) + 3);
	out->insert(out->end(), l3 ? l3->base : lab_black, (l3 ? l3->base : lab_black) + 3);
	for (s16 k = 0; k < 4; k++)
		put32(out, 0);                  /* the offsets */
	put32(out, rows);
	put32(out, l2 && l2->coded ? (u32)l2->data.size() : 0);
	for (const EncLayer *l : {l2, l1, l3})
		if (l && l->coded)
			out->insert(out->end(), l->data.begin(), l->data.end());
}

/* a stripe in modes 2 and 3: the SOSt (the layers there), then each
   layer's SLC, EOH and coded data: the main mask, the background, the
   foreground, the others in their order */
void put_stripe_slc(std::vector<unsigned char> *out, std::vector<EncLayer> ls)
{
	std::stable_sort(ls.begin(), ls.end(), [](const EncLayer &a, const EncLayer &b) {
		auto key = [](u32 n) { return n == 2 ? 0u : n == 1 ? 1u : n; };
		return key(a.number) < key(b.number);
	});
	u32 type = 0;
	for (const EncLayer &l : ls)
		type |= 1u << (l.number - 1);
	std::vector<unsigned char> t;
	put_coders(&t, type);
	segment(out, (u32)(2 + 4 + t.size()), ID_SOST);
	out->insert(out->end(), t.begin(), t.end());
	for (const EncLayer &l : ls) {
		segment(out, SLC_LEN, ID_SLC);
		out->push_back((unsigned char)l.number);
		out->push_back((unsigned char)((l.coded ? 0x01 : 0) | (l.image ? 0x02 : 0)));
		out->push_back((unsigned char)l.bit);
		put16(out, (u32)l.res);
		put32(out, l.w);
		put32(out, l.h);
		out->insert(out->end(), l.base, l.base + 3);
		put32(out, l.off[0]);
		put32(out, l.off[1]);
		segment(out, EOH_LEN, ID_EOH);
		put32(out, (u32)l.data.size());
		out->insert(out->end(), l.data.begin(), l.data.end());
	}
}

/* the CIELAB octets of an sRGB or grey mean */
void lab_of(const unsigned char *mean, s16 nc, unsigned char *lab)
{
	to_lab(mean, nc, lab, 1);
}

/* an image layer decoded into CIELAB octets: its size, resolution;
   *used its octets.  false when it is none */
struct Layer {
	u32 w = 0, h = 0;
	s16 dpi = 0;
	std::vector<unsigned char> lab;
	bool ok = false;
	bool early = false;
};

/* a JPEG (ff d8) or T.43 BCIE (ff a8) layer at d into l, *used its
   octets; false when it is neither */
bool image_in(const unsigned char *d, size_t len, u32 max_rows, Layer *l, size_t *used)
{
	if (len >= 2 && d[0] == 0xff && d[1] == M_SOI) {
		JpegImage j;
		s16 r = jpeg_decode(d, len, max_rows, &j, used);
		if (r < 0 || !j.width)
			return false;
		l->w = j.width;
		l->h = j.height;
		l->dpi = j.dpi;
		l->early = r > 0;
		size_t n = (size_t)j.width * j.height;
		l->lab.resize(n * 3);
		if (j.comps == 3) {
			l->lab = std::move(j.pix);
		} else {
			for (size_t i = 0; i < n; i++) {
				l->lab[i * 3] = j.pix[i];
				l->lab[i * 3 + 1] = 0x80;
				l->lab[i * 3 + 2] = 0x60;
			}
		}
		l->ok = true;
		return true;
	}
	if (len >= 2 && d[0] == 0xff && d[1] == T43_MAGIC) {
		T43Image t;
		s16 r = t43_decode(d, len, max_rows, &t, used);
		if (r < 0 || !t.width)
			return false;
		std::vector<unsigned char> s;
		s16 comps;
		t43_to_srgb(t, &s, &comps);
		l->w = t.width;
		l->h = t.height;
		l->dpi = t.dpi;
		l->early = r > 0;
		l->lab.resize((size_t)t.width * t.height * 3);
		to_lab(s.data(), comps, l->lab.data(), (size_t)t.width * t.height);
		l->ok = true;
		return true;
	}
	return false;
}

/* a layer's pel at the stripe's (x, y) (mask pels), its offset off and
   resolution against the mask's; NULL outside it */
const unsigned char *layer_at(const Layer &l, u32 x, u32 y, const u32 *off, s16 res)
{
	if (!l.ok || x < off[0] || y < off[1])
		return nullptr;
	s16 dpi = l.dpi > 0 && l.dpi <= res ? l.dpi : res;
	u32 lx = (u32)((u64)(x - off[0]) * dpi / res), ly = (u32)((u64)(y - off[1]) * dpi / res);
	if (lx >= l.w || ly >= l.h)
		return nullptr;
	return &l.lab[((size_t)ly * l.w + lx) * 3];
}

/* a bi-level layer coded with the coder of Table 1 bit `bit` (MH, MR,
   MMR or JBIG), w by at most h pels (JBIG: the BIE's own width), to
   bits (a pel an octet, 0 or 1); *bw its width, *bh the rows decoded.
   -1 when the coder is not known or the data is none, 1 when it ended
   early, else 0 */
s16 mask_in(const unsigned char *d, size_t len, s16 bit, u32 w, u32 h, std::vector<unsigned char> *bits,
	    u32 *bw, u32 *bh)
{
	if (bit != BIT_JBIG && (w == 0 || w > MAX_WIDTH))
		return -1;
	T4Page mp;
	mp.init((s16)(w ? w : 1), 1);
	s16 r = 0;
	if (bit == BIT_JBIG) {
		r = jbig_decode(d, len, (s32)h, &mp);
		if (r < 0) {
			mp.release();
			return -1;
		}
	} else if (bit == BIT_MMR) {
		mp.decode(d, len, 4, 0, 1, (s32)h);
	} else if (bit == BIT_MH || bit == BIT_MR) {
		mp.decode(d, len, 3, bit == BIT_MR ? 1 : 0, 1, (s32)h);
	} else {
		mp.release();
		return -1;              /* (JBIG2, or a coder not defined) */
	}
	*bw = (u32)mp.width;
	*bh = std::min((u32)mp.rows, h);
	bits->assign((size_t)*bw * *bh, 0);
	s16 stride = T4_STRIDE(mp.width);
	for (u32 y = 0; y < *bh; y++)
		for (u32 x = 0; x < *bw; x++)
			(*bits)[(size_t)y * *bw + x] = (mp.bits[(size_t)y * stride + x / 8] >> (7 - x % 8)) & 1;
	mp.release();
	return r > 0 || *bh < h ? 1 : 0;
}

/* coder octets (bit 7: another follows): their bits */
bool coders(const unsigned char *d, size_t len, size_t *pos, u32 *bits)
{
	*bits = 0;
	for (s16 k = 0;; k++) {
		if (*pos >= len)
			return false;
		unsigned char c = d[(*pos)++];
		if (k < 4)
			*bits |= (u32)(c & 0x7f) << (7 * k);
		if (!(c & 0x80))
			return true;
	}
}

/* the marker segment at data[pos] (an ff and a marker) skipped: an
   APP13 segment by its length (and optional length), T.43's G3FAX3
   (APP3) by its 4-octet length, another by its 2-octet length.  *next
   the octet after it; false when it is not whole */
bool skip_segment(const unsigned char *data, size_t len, size_t pos, size_t *next)
{
	if (pos + 4 > len || data[pos] != 0xff)
		return false;
	size_t sl, p;
	if (data[pos + 1] == APP13) {
		if (!seg_head(data, len, pos, &sl, &p))
			return false;
	} else if (data[pos + 1] == APP3 && pos + 11 <= len && !memcmp(data + pos + 6, "G3FAX", 5)) {
		sl = get32(data + pos + 2);
		if (sl < 4 || sl > len - pos - 2)
			return false;
	} else {
		sl = get16(data + pos + 2);
		if (sl < 2 || sl > len - pos - 2)
			return false;
	}
	*next = pos + 2 + sl;
	return true;
}

/* a layer of a stripe in modes 2 and up: its SLC's parameters, and its
   coded data decoded */
struct SlcLayer {
	bool there = false;
	bool coded = false;
	bool image = false;             /* Table 2's coder (else Table 1's) */
	u32 bit = 0;
	s16 res = 0;
	u32 w = 0, h = 0;               /* in main mask pels */
	unsigned char base[3] = {0, 0, 0};
	u32 off[2] = {0, 0};
	Layer img;                      /* an image layer's pels */
	std::vector<unsigned char> bits; /* a mask's */
	u32 bw = 0, bh = 0;
};

/* the parameters of the SLC at data[p] (after its identifier) up to
   end; false when they are not whole */
bool slc_params(const unsigned char *data, size_t end, size_t p, u32 *number, SlcLayer *l)
{
	if (p + 2 > end)
		return false;
	*number = data[p++];
	unsigned char c1 = data[p++];
	l->coded = (c1 & 0x01) != 0;
	l->image = (c1 & 0x02) != 0;
	if (!coders(data, end, &p, &l->bit) || p + 2 + 4 + 4 + 3 + 8 > end)
		return false;
	l->res = (s16)get16(data + p);
	l->w = get32(data + p + 2);
	l->h = get32(data + p + 6);
	memcpy(l->base, data + p + 10, 3);
	l->off[0] = get32(data + p + 13);
	l->off[1] = get32(data + p + 17);
	l->there = true;
	return true;
}

/* a mask layer's pel at the stripe's (x, y) (main mask pels): -1
   outside its area (or no coded data), else 0 or 1 */
s16 slc_mask_at(const SlcLayer &l, u32 x, u32 y, s16 res)
{
	if (!l.there || !l.coded || l.bits.empty() || x < l.off[0] || y < l.off[1])
		return -1;
	s16 r = l.res > 0 && l.res <= res ? l.res : res;
	u32 lx = (u32)((u64)(x - l.off[0]) * r / res), ly = (u32)((u64)(y - l.off[1]) * r / res);
	if (lx >= l.bw || ly >= l.bh)
		return -1;
	return l.bits[(size_t)ly * l.bw + lx];
}

/* an image layer's pel at (x, y): NULL outside its area (or no coded
   data) */
const unsigned char *slc_image_at(const SlcLayer &l, u32 x, u32 y, s16 res)
{
	if (!l.there || !l.coded)
		return nullptr;
	if ((l.w && x >= (u64)l.off[0] + l.w) || (l.h && y >= (u64)l.off[1] + l.h))
		return nullptr;
	return layer_at(l.img, x, y, l.off, res);
}

/* the layers of a stripe drawn into lab (its rows, width pels each),
   from layer 1 up (A.7.4) */
void draw_slc(const std::vector<SlcLayer> &ls, u32 width, u32 sh, s16 res, unsigned char *lab)
{
	const SlcLayer &l1 = ls[1];
	const unsigned char *bg = l1.there ? l1.base : lab_white;
	for (u32 y = 0; y < sh; y++)
		for (u32 x = 0; x < width; x++) {
			const unsigned char *c = slc_image_at(l1, x, y, res);
			memcpy(&lab[((size_t)y * width + x) * 3], c ? c : bg, 3);
		}
	for (size_t k = 3; k < ls.size(); k += 2) {
		const SlcLayer &m = ls[k - 1], &im = ls[k];
		if (!m.there && !im.there)
			continue;
		const unsigned char *base = im.there ? im.base : lab_black;
		const bool mask = m.there && m.coded;
		for (u32 y = 0; y < sh; y++)
			for (u32 x = 0; x < width; x++) {
				const unsigned char *c = slc_image_at(im, x, y, res);
				s16 v = mask ? slc_mask_at(m, x, y, res) : -1;
				/* outside the mask (or no mask): the image over
				   its area */
				if (v == 0 || (v < 0 && !c))
					continue;
				memcpy(&lab[((size_t)y * width + x) * 3], c ? c : base, 3);
			}
	}
}

/* modes 2 and up: the stripes from pos (after the SOP) into lab, at
   most max_rows lines; *rows the lines drawn, *early whether the page
   ended before EOP */
void decode_slc(const unsigned char *data, size_t len, size_t pos, u32 width, s16 res, u32 max_rows,
		std::vector<unsigned char> *lab, u32 *rows, bool *early)
{
	std::vector<SlcLayer> ls;       /* the stripe's, by number */
	bool in_stripe = false, first = true, part = false;
	u32 sh = 0;
	*early = true;
	/* the stripe so far drawn */
	auto finish = [&]() {
		if (in_stripe && sh) {
			lab->resize((size_t)width * (*rows + sh) * 3);
			draw_slc(ls, width, sh, res, lab->data() + (size_t)width * *rows * 3);
			*rows += sh;
		}
		in_stripe = false;
	};
	while (*rows < max_rows && !part) {
		while (pos < len && data[pos] != 0xff)
			pos++;              /* (fill) */
		if (pos + 2 > len)
			break;
		if (data[pos + 1] == M_EOI) {
			/* the TN, or EOP */
			if (pos + 4 <= len && data[pos + 2] == 0xff && data[pos + 3] == M_EOI) {
				*early = false;
				break;
			}
			pos += 2;
			continue;
		}
		if (data[pos + 1] != APP13 || pos + 8 > len || memcmp(data + pos + 4, "MRC", 3) != 0) {
			size_t next;
			if (!skip_segment(data, len, pos, &next))
				pos++;
			else
				pos = next;
			continue;
		}
		size_t gl, q;
		if (!seg_head(data, len, pos, &gl, &q))
			break;
		const unsigned char id = data[pos + 7];
		size_t gend = pos + 2 + gl;
		if (id == ID_SOST) {
			finish();
			ls.assign(MAX_LAYER + 2, SlcLayer());
			in_stripe = true;
			first = true;
			sh = 0;
			pos = gend;
			continue;
		}
		if (id != ID_SLC || !in_stripe) {
			pos = gend;         /* (an optional or encoder segment) */
			continue;
		}
		/* a layer: its SLC, the segments up to its EOH, its data */
		SlcLayer l;
		u32 number;
		if (!slc_params(data, gend, q, &number, &l) || number < 1 || number > MAX_LAYER)
			break;
		if (first) {
			/* the main mask's height is the stripe's */
			first = false;
			sh = std::min(l.h, max_rows - *rows);
		}
		pos = gend;
		size_t dl = 0;
		bool eoh = false;
		while (!eoh) {
			while (pos < len && data[pos] != 0xff)
				pos++;
			size_t el, eq;
			if (pos + 8 <= len && data[pos + 1] == APP13 && !memcmp(data + pos + 4, "MRC", 3) &&
			    data[pos + 7] == ID_EOH) {
				if (!seg_head(data, len, pos, &el, &eq) || eq + 4 > pos + 2 + el)
					break;
				dl = get32(data + eq);
				pos += 2 + el;
				eoh = true;
			} else if (!skip_segment(data, len, pos, &pos)) {
				break;
			}
		}
		if (!eoh)
			break;
		if (dl > len - pos) {
			dl = len - pos;
			part = true;
		}
		if (l.coded && sh) {
			if (l.image) {
				size_t used = 0;
				if (!image_in(data + pos, dl, max_rows, &l.img, &used))
					break;
				if (l.res > 0)
					l.img.dpi = l.res;      /* (the SLC's) */
				part |= l.img.early;
			} else {
				/* the mask's pels: its width and height at its
				   resolution */
				s16 r = l.res > 0 && l.res <= res ? l.res : res;
				u32 mw = number == 2 ? width : l.w;
				u32 h = number == 2 ? sh : l.h;
				u32 pw = (u32)((u64)mw * r / res), ph = (u32)(((u64)h * r + res - 1) / res);
				s16 m = mask_in(data + pos, dl, (s16)l.bit, pw, ph, &l.bits, &l.bw, &l.bh);
				if (m < 0)
					break;
				part |= m > 0 && number == 2;
			}
		}
		pos += dl;
		ls[number] = std::move(l);
	}
	if (in_stripe && sh)
		finish();
}

/* mode 1: the stripes from pos (after the SOP) into lab, at most
   max_rows lines, the mask in the first of mask_coders that we have
   (JBIG, MMR, MR, MH); *rows the lines drawn, *early whether the page
   ended before EOP */
void decode_mode1(const unsigned char *data, size_t len, size_t pos, u32 width, s16 res,
		  u32 mask_coders, u32 max_rows, std::vector<unsigned char> *lab, u32 *rows, bool *early)
{
	const s16 bit = mask_coders & T44_MASK_JBIG ? BIT_JBIG : mask_coders & T44_MASK_MMR ? BIT_MMR :
		mask_coders & T44_MASK_MR ? BIT_MR : mask_coders & T44_MASK_MH ? BIT_MH : -1;
	*early = true;
	while (*rows < max_rows) {
		while (pos < len && data[pos] != 0xff)
			pos++;              /* (fill) */
		if (pos + 2 > len)
			break;
		if (data[pos + 1] == M_EOI) {
			/* the TN, or EOP */
			if (pos + 4 <= len && data[pos + 2] == 0xff && data[pos + 3] == M_EOI) {
				*early = false;
				break;
			}
			pos += 2;
			continue;
		}
		if (data[pos + 1] != APP13) {
			pos++;
			continue;
		}
		size_t gl, q;
		if (!seg_head(data, len, pos, &gl, &q))
			break;
		const unsigned char *g = data + pos + 4;
		size_t gend = pos + 2 + gl;
		if (memcmp(g, "MRC", 3) != 0 || g[3] != ID_SOST) {
			pos = gend;     /* (an optional segment, skipped) */
			continue;
		}
		/* a stripe: its SOSt */
		u32 type;
		if (!coders(data, gend, &q, &type) || q + 6 + 16 + 8 > gend)
			break;
		const unsigned char *bgc = data + q, *fgc = data + q + 3;
		u32 bgo[2] = {get32(data + q + 6), get32(data + q + 10)};
		u32 fgo[2] = {get32(data + q + 14), get32(data + q + 18)};
		u32 sh = get32(data + q + 22), ml = get32(data + q + 26);
		pos = gend;
		sh = std::min(sh, max_rows - *rows);
		if (!sh)
			continue;

		/* the mask */
		std::vector<unsigned char> mask;
		bool part = false;
		if (type & 0x02) {
			if (ml > len - pos) {
				ml = (u32)(len - pos);
				part = true;
			}
			u32 bw, bh;
			s16 m = mask_in(data + pos, ml, bit, width, sh, &mask, &bw, &bh);
			if (m < 0 || bw != width)
				break;
			mask.resize((size_t)width * sh, 0);
			part |= m > 0;
			pos += ml;
		}
		/* the background, the foreground */
		Layer bg, fg;
		size_t used = 0;
		if (type & 0x01) {
			if (!image_in(data + pos, len - pos, max_rows, &bg, &used))
				break;
			pos += used;
			part |= bg.early;
		}
		if (type & 0x04) {
			if (!image_in(data + pos, len - pos, max_rows, &fg, &used))
				break;
			pos += used;
			part |= fg.early;
		}
		/* drawn */
		const unsigned char fixed = !(type & 0x02) && (type & 0x04) ? 1 : 0;
		lab->resize((size_t)width * (*rows + sh) * 3);
		for (u32 y = 0; y < sh; y++)
			for (u32 x = 0; x < width; x++) {
				unsigned char m = mask.empty() ? fixed : mask[(size_t)y * width + x];
				const unsigned char *c = m ? layer_at(fg, x, y, fgo, res) :
					layer_at(bg, x, y, bgo, res);
				if (!c)
					c = m ? fgc : bgc;
				memcpy(&(*lab)[((size_t)(*rows + y) * width + x) * 3], c, 3);
			}
		*rows += sh;
		if (part)
			break;
	}
}

} // namespace

/* (t44.h) a mask of at least 0.1 % of the page */
bool t44_worth(const T44Image &img)
{
	if (!img.width || !img.height || (img.comps != 1 && img.comps != 3))
		return false;
	std::vector<unsigned char> mask;
	find_mask(img, &mask);
	size_t n = 0;
	for (unsigned char m : mask)
		n += m;
	return n * 1000 >= mask.size();         /* 0.1 % */
}

/* (t44.h) SOP, TN, a stripe each opt.stripe lines, EOP */
void t44_encode(const T44Image &img, const T44Options &opt, std::vector<unsigned char> *out)
{
	const u32 w = img.width, h = img.height;
	const s16 nc = img.comps == 3 ? 3 : 1;
	const s16 mode = opt.mode >= 1 && opt.mode <= 3 ? opt.mode : 1;
	const s16 coder = opt.mask_coder == T44_MASK_MH ? T44_MASK_MH : T44_MASK_JBIG;
	const s16 bit = coder == T44_MASK_MH ? BIT_MH : BIT_JBIG;
	const s16 res = img.dpi > 0 ? img.dpi : 200;
	s16 idpi = opt.image_dpi > 0 && opt.image_dpi <= res && res % opt.image_dpi == 0 ?
		opt.image_dpi : res;
	const u32 f = (u32)(res / idpi);
	std::vector<unsigned char> mask;
	find_mask(img, &mask);

	out->clear();
	put_sop(out, mode, coder, res, w);

	const u32 sh = opt.stripe ? opt.stripe : 256;
	bool any_mask = false, any_image = false;
	for (u32 y0 = 0; y0 < h; y0 += sh) {
		const u32 rows = std::min(sh, h - y0);
		unsigned char fgm[3] = {0, 0, 0}, bgm[3] = {255, 255, 255};
		bool fg_flat = true, bg_flat = true;
		bool any_fg = layer_colour(img, mask, y0, rows, 1, FG_FLAT, false, fgm, &fg_flat);
		bool any_bg = layer_colour(img, mask, y0, rows, 0, BG_FLAT, true, bgm, &bg_flat);
		/* the layers: 1 the background, 2 the main mask, 3 the
		   foreground, and in mode 3 more pairs for text colours */
		std::vector<EncLayer> ls(3), more;
		EncLayer &l1 = ls[0], &l2 = ls[1], &l3 = ls[2];
		l1.number = 1;
		l2.number = 2;
		l3.number = 3;
		l1.image = l3.image = true;
		for (EncLayer &l : ls) {
			l.res = res;
			l.w = w;
			l.h = rows;
		}
		if (any_bg)
			lab_of(bgm, nc, l1.base);
		else
			memcpy(l1.base, lab_white, 3);
		if (any_fg)
			lab_of(fgm, nc, l3.base);
		else
			memcpy(l3.base, lab_black, 3);
		if (!any_fg && !bg_flat) {
			/* no text: the background alone, at full resolution */
			l1.coded = true;
			image_layer(img, mask, y0, rows, 2, 1, bgm, opt, res, &l1.data);
			l3.number = 0;          /* (no foreground) */
		} else {
			std::vector<unsigned char> id;
			unsigned char means[TEXT_COLOURS][3];
			s16 k = mode == 3 && any_fg && !fg_flat ?
				text_colours(img, mask, y0, rows, &id, means) : 0;
			l2.coded = true;
			l2.bit = bit;
			if (k >= 2) {
				/* a mask and a colour for each text colour: the
				   first in the main mask and the foreground */
				std::vector<unsigned char> m(id.size());
				for (size_t i = 0; i < id.size(); i++)
					m[i] = id[i] == 0;
				code_bitmap(m.data(), w, 0, 0, w, rows, bit, &l2.data);
				lab_of(means[0], nc, l3.base);
				for (s16 c = 1; c < k; c++) {
					u32 x0 = w, y1 = rows, x1 = 0, y2 = 0;
					for (u32 y = 0; y < rows; y++)
						for (u32 x = 0; x < w; x++)
							if (id[(size_t)y * w + x] == c) {
								x0 = std::min(x0, x);
								x1 = std::max(x1, x);
								y1 = std::min(y1, y);
								y2 = std::max(y2, y);
							}
					for (size_t i = 0; i < id.size(); i++)
						m[i] = id[i] == c;
					EncLayer lm, li;
					lm.number = 2 * (u32)c + 2;
					lm.coded = true;
					lm.bit = bit;
					lm.res = res;
					lm.off[0] = x0;
					lm.off[1] = y1;
					lm.w = x1 - x0 + 1;
					lm.h = y2 - y1 + 1;
					code_bitmap(m.data(), w, x0, y1, lm.w, lm.h, bit, &lm.data);
					li.number = lm.number + 1;
					li.image = true;
					li.res = res;
					li.w = w;
					li.h = rows;
					lab_of(means[c], nc, li.base);
					more.push_back(std::move(lm));
					more.push_back(std::move(li));
				}
			} else {
				code_bitmap(mask.data(), w, 0, y0, w, rows, bit, &l2.data);
				if (any_fg && !fg_flat) {
					l3.coded = true;
					l3.res = idpi;
					image_layer(img, mask, y0, rows, 1, f, fgm, opt, idpi, &l3.data);
				}
			}
			if (any_bg && !bg_flat) {
				l1.coded = true;
				l1.res = idpi;
				image_layer(img, mask, y0, rows, 0, f, bgm, opt, idpi, &l1.data);
			}
		}
		if (!l3.number)
			ls.pop_back();
		for (EncLayer &l : more)
			ls.push_back(std::move(l));
		if (mode == 1)
			put_stripe_mode1(out, ls, rows);
		else
			put_stripe_slc(out, ls);
		for (const EncLayer &l : ls) {
			any_mask |= l.coded && !l.image;
			any_image |= l.coded && l.image;
		}
	}
	/* the SOP's coders: 0 for a layer that no stripe codes (T.44
	   9.2.1) */
	if (!any_mask)
		(*out)[SOP_MASK_CODER] = 0;
	if (!any_image)
		(*out)[SOP_MASK_CODER + 1] = 0;
	out->push_back(0xff);
	out->push_back(M_EOI);
	out->push_back(0xff);
	out->push_back(M_EOI);
}

/* (t44.h) the SOP, then the stripes (mode 1's, or the SLC layers of
   modes 2 and 3) drawn into CIELAB, the rest skipped, until EOP */
s16 t44_decode(const unsigned char *data, size_t len, u32 max_rows, T44Image *img)
{
	*img = T44Image();
	if (len < 4 || data[0] != 0xff || data[1] != M_SOI || data[2] != 0xff || data[3] != APP13)
		return -1;
	/* the SOP */
	size_t sl, p;
	if (!seg_head(data, len, 2, &sl, &p) || memcmp(data + 6, "MRC", 3) != 0 ||
	    data[9] != ID_SOP)
		return -1;
	size_t end = 2 + 2 + sl;
	if (p + 2 > end)
		return -1;
	const s16 mode = data[p + 1];
	if (mode < 1 || mode > 3)
		return -1;              /* (mode 4: shared data, JBIG2) */
	p += 2;
	u32 mask_coders, image_coders;
	if (!coders(data, end, &p, &mask_coders) || !coders(data, end, &p, &image_coders) || p + 6 > end)
		return -1;
	const s16 res = (s16)get16(data + p);
	const u32 width = get32(data + p + 2);
	if (res <= 0 || width == 0 || width > MAX_WIDTH)
		return -1;

	img->width = width;
	img->dpi = res;
	std::vector<unsigned char> lab;     /* the page so far */
	bool early = true;
	u32 rows = 0;
	if (mode == 1)
		decode_mode1(data, len, end, width, res, mask_coders, max_rows, &lab, &rows, &early);
	else
		decode_slc(data, len, end, width, res, max_rows, &lab, &rows, &early);
	if (!rows)
		return early ? -1 : 0;
	img->height = rows;
	size_t n = (size_t)width * rows;
	bool grey = true;
	for (size_t i = 0; i < n && grey; i++)
		grey = lab[i * 3 + 1] == 0x80 && lab[i * 3 + 2] == 0x60;
	if (grey) {
		std::vector<unsigned char> l(n);
		for (size_t i = 0; i < n; i++)
			l[i] = lab[i * 3];
		img->comps = 1;
		img->pix.resize(n);
		t42_l_to_grey(l.data(), img->pix.data(), n);
	} else {
		img->comps = 3;
		img->pix.resize(n * 3);
		t42_lab_to_rgb(lab.data(), img->pix.data(), n);
	}
	return early ? 1 : 0;
}

} // namespace dsp
