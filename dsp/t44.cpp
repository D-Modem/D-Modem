/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t44.cpp - ITU-T T.44 mixed raster content, mode 1 (T.4 Annex H)
 *
 * A page is ff d8 and the start of page (SOP: ff ed, its length,
 * "MRC" 0, version 2, mode 1, the mask's and the image layers' coders,
 * the mask's resolution, the width), ff d9, optional segments (ff ed
 * "MRC" n), then its stripes from the top, each a start of stripe
 * (SOSt: ff ed, "MRC" 1, the layers there, the background's and the
 * foreground's colours, their offsets, the height, the mask's length)
 * and its layers: the mask (that many octets), the background, the
 * foreground (each a JPEG or a T.43 BCIE, which ends itself); ff d9 ff d9
 * ends it.  Where the mask is 1 the foreground shows, else the
 * background; a layer missing is its colour, a mask missing all 0 (or
 * 1 with only a foreground).  Colours are T.42's CIELAB octets.
 *
 * Our stripes: the mask has the dark pels at an edge (text and lines),
 * the foreground their colour (one colour, or a JPEG at image_dpi),
 * the background the rest (one colour, or a JPEG at image_dpi; a
 * stripe without text has the background alone, at the mask's
 * resolution).
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
#define M_SOI           0xd8
#define M_EOI           0xd9
#define T43_MAGIC       0xa8
#define MRC_VERSION     2
#define MRC_MODE        1
#define ID_SOP          0
#define ID_SOST         1
#define MAX_WIDTH       4864    /* A3 at 400 pels an inch */

#define MASK_DARK       128     /* a mask pel: its lightness below */
#define MASK_EDGE       80      /* ... and lighter by this near it */
#define MASK_REACH      3       /* ... "near": pels each way */
#define BG_FLAT         12      /* a background in one colour: no
				   sample further from the mean */
#define FG_FLAT         10      /* a foreground: on average, in each
				   component */

/* the CIELAB octets of white and black (T.44 9.3) */
const unsigned char lab_white[3] = {0xff, 0x80, 0x60};
const unsigned char lab_black[3] = {0x00, 0x80, 0x60};

void put16(std::vector<unsigned char> *out, u32 v)
{
	out->push_back((unsigned char)(v >> 8));
	out->push_back((unsigned char)v);
}

void put32(std::vector<unsigned char> *out, u32 v)
{
	put16(out, v >> 16);
	put16(out, v);
}

u32 get16(const unsigned char *p)
{
	return (u32)p[0] << 8 | p[1];
}

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
	std::vector<unsigned char> y, hm, m;
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

/* the stripe's mask coded */
void code_mask(const T44Image &img, const std::vector<unsigned char> &mask, u32 y0, u32 h,
	       s16 coder, std::vector<unsigned char> *out)
{
	T4Page p;
	p.init((s16)img.width, 1);
	s16 stride = T4_STRIDE(p.width);
	std::vector<unsigned char> row(stride);
	for (u32 y = 0; y < h; y++) {
		std::fill(row.begin(), row.end(), 0);
		const unsigned char *m = mask.data() + (size_t)(y0 + y) * img.width;
		for (u32 x = 0; x < img.width; x++)
			if (m[x])
				row[x / 8] |= (unsigned char)(0x80 >> (x % 8));
		p.add_row(row.data());
	}
	if (coder == T44_MASK_MH) {
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

/* an image layer decoded into CIELAB octets: its size, resolution;
   *used its octets.  false when it is none */
struct Layer {
	u32 w = 0, h = 0;
	s16 dpi = 0;
	std::vector<unsigned char> lab;
	bool ok = false;
	bool early = false;
};

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

} // namespace

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

void t44_encode(const T44Image &img, const T44Options &opt, std::vector<unsigned char> *out)
{
	const u32 w = img.width, h = img.height;
	const s16 nc = img.comps == 3 ? 3 : 1;
	const s16 coder = opt.mask_coder == T44_MASK_MH ? T44_MASK_MH : T44_MASK_JBIG;
	const s16 res = img.dpi > 0 ? img.dpi : 200;
	s16 idpi = opt.image_dpi > 0 && opt.image_dpi <= res && res % opt.image_dpi == 0 ?
		opt.image_dpi : res;
	const u32 f = (u32)(res / idpi);
	std::vector<unsigned char> mask;
	find_mask(img, &mask);

	out->clear();
	out->push_back(0xff);
	out->push_back(M_SOI);
	segment(out, 2 + 4 + 6 + 4, ID_SOP);
	out->push_back(MRC_VERSION);
	out->push_back(MRC_MODE);
	out->push_back((unsigned char)coder);
	out->push_back(T44_IMAGE_JPEG);
	put16(out, (u32)res);
	put32(out, w);
	out->push_back(0xff);
	out->push_back(M_EOI);              /* the termination number */

	const u32 sh = opt.stripe ? opt.stripe : 256;
	for (u32 y0 = 0; y0 < h; y0 += sh) {
		const u32 rows = std::min(sh, h - y0);
		unsigned char fgm[3] = {0, 0, 0}, bgm[3] = {255, 255, 255};
		bool fg_flat = true, bg_flat = true;
		bool any_fg = layer_colour(img, mask, y0, rows, 1, FG_FLAT, false, fgm, &fg_flat);
		bool any_bg = layer_colour(img, mask, y0, rows, 0, BG_FLAT, true, bgm, &bg_flat);
		unsigned char type;
		std::vector<unsigned char> m, bg, fg;
		if (!any_fg && !bg_flat) {
			/* no text: the background alone, at full resolution */
			type = 0x01;
			image_layer(img, mask, y0, rows, 2, 1, bgm, opt, res, &bg);
		} else {
			type = 0x02;
			code_mask(img, mask, y0, rows, coder, &m);
			if (any_bg && !bg_flat) {
				type |= 0x01;
				image_layer(img, mask, y0, rows, 0, f, bgm, opt, idpi, &bg);
			}
			if (any_fg && !fg_flat) {
				type |= 0x04;
				image_layer(img, mask, y0, rows, 1, f, fgm, opt, idpi, &fg);
			}
		}
		unsigned char bgc[3], fgc[3];
		if (any_bg)
			to_lab(bgm, nc, bgc, 1);
		else
			memcpy(bgc, lab_white, 3);
		if (any_fg)
			to_lab(fgm, nc, fgc, 1);
		else
			memcpy(fgc, lab_black, 3);
		segment(out, 2 + 4 + 1 + 3 + 3 + 8 + 8 + 4 + 4, ID_SOST);
		out->push_back(type);
		out->insert(out->end(), bgc, bgc + 3);
		out->insert(out->end(), fgc, fgc + 3);
		for (s16 k = 0; k < 4; k++)
			put32(out, 0);          /* the offsets */
		put32(out, rows);
		put32(out, (u32)m.size());
		out->insert(out->end(), m.begin(), m.end());
		out->insert(out->end(), bg.begin(), bg.end());
		out->insert(out->end(), fg.begin(), fg.end());
	}
	out->push_back(0xff);
	out->push_back(M_EOI);
	out->push_back(0xff);
	out->push_back(M_EOI);
}

s16 t44_decode(const unsigned char *data, size_t len, u32 max_rows, T44Image *img)
{
	*img = T44Image();
	if (len < 4 || data[0] != 0xff || data[1] != M_SOI || data[2] != 0xff || data[3] != APP13)
		return -1;
	/* the SOP */
	size_t pos = 4;
	if (pos + 2 > len)
		return -1;
	size_t sl = get16(data + pos), hl = 2;
	if (sl == 0) {
		if (pos + 6 > len)
			return -1;
		sl = get32(data + pos + 2);
		hl = 6;
	}
	if (sl < hl + 4 || pos + sl > len)
		return -1;
	size_t end = pos + sl, p = pos + hl;
	if (memcmp(data + p, "MRC", 3) != 0 || data[p + 3] != ID_SOP)
		return -1;
	p += 4;
	if (p + 2 > end || data[p + 1] != MRC_MODE)
		return -1;              /* (modes 2 and up: their SOSt differs) */
	p += 2;
	u32 mask_coders, image_coders;
	if (!coders(data, end, &p, &mask_coders) || !coders(data, end, &p, &image_coders) || p + 6 > end)
		return -1;
	const s16 res = (s16)get16(data + p);
	const u32 width = get32(data + p + 2);
	if (res <= 0 || width == 0 || width > MAX_WIDTH)
		return -1;
	pos = end;

	img->width = width;
	img->dpi = res;
	std::vector<unsigned char> lab;     /* the page so far */
	bool early = true;
	u32 rows = 0;
	while (rows < max_rows) {
		while (pos < len && data[pos] != 0xff)
			pos++;              /* (fill) */
		if (pos + 2 > len)
			break;
		if (data[pos + 1] == M_EOI) {
			/* the TN, or EOP */
			if (pos + 4 <= len && data[pos + 2] == 0xff && data[pos + 3] == M_EOI) {
				early = false;
				break;
			}
			pos += 2;
			continue;
		}
		if (data[pos + 1] != APP13) {
			pos++;
			continue;
		}
		if (pos + 4 > len)
			break;
		size_t gl = get16(data + pos + 2), ghl = 2;
		if (gl == 0) {
			if (pos + 8 > len)
				break;
			gl = get32(data + pos + 4);
			ghl = 6;
		}
		if (gl < ghl + 4 || pos + 2 + gl > len)
			break;
		const unsigned char *g = data + pos + 2 + ghl;
		size_t gend = pos + 2 + gl;
		if (memcmp(g, "MRC", 3) != 0 || g[3] != ID_SOST) {
			pos = gend;     /* (an optional segment, skipped) */
			continue;
		}
		/* a stripe: its SOSt */
		size_t q = pos + 2 + ghl + 4;
		u32 type;
		if (!coders(data, gend, &q, &type) || q + 6 + 16 + 8 > gend)
			break;
		const unsigned char *bgc = data + q, *fgc = data + q + 3;
		u32 bgo[2] = {get32(data + q + 6), get32(data + q + 10)};
		u32 fgo[2] = {get32(data + q + 14), get32(data + q + 18)};
		u32 sh = get32(data + q + 22), ml = get32(data + q + 26);
		pos = gend;
		sh = std::min(sh, max_rows - rows);
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
			T4Page mp;
			mp.init((s16)width, 1);
			if (mask_coders & T44_MASK_JBIG) {
				if (jbig_decode(data + pos, ml, (s32)sh, &mp) < 0 || mp.width != (s16)width)
					break;
			} else if (mask_coders & T44_MASK_MMR) {
				mp.decode(data + pos, ml, 4, 0, 1, (s32)sh);
			} else if (mask_coders & (T44_MASK_MH | T44_MASK_MR)) {
				mp.decode(data + pos, ml, 3, mask_coders & T44_MASK_MR ? 1 : 0, 1, (s32)sh);
			} else {
				break;          /* (JBIG2) */
			}
			mask.assign((size_t)width * sh, 0);
			s16 stride = T4_STRIDE(mp.width);
			for (u32 y = 0; y < sh && y < (u32)mp.rows; y++)
				for (u32 x = 0; x < width; x++)
					mask[(size_t)y * width + x] =
						(mp.bits[(size_t)y * stride + x / 8] >> (7 - x % 8)) & 1;
			if ((u32)mp.rows < sh)
				part = true;
			mp.release();
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
		lab.resize((size_t)width * (rows + sh) * 3);
		for (u32 y = 0; y < sh; y++)
			for (u32 x = 0; x < width; x++) {
				unsigned char m = mask.empty() ? fixed : mask[(size_t)y * width + x];
				const unsigned char *c = m ? layer_at(fg, x, y, fgo, res) :
					layer_at(bg, x, y, bgo, res);
				if (!c)
					c = m ? fgc : bgc;
				memcpy(&lab[((size_t)(rows + y) * width + x) * 3], c, 3);
			}
		rows += sh;
		if (part)
			break;
	}
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
