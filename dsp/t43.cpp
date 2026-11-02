/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t43.cpp - ITU-T T.43 lossless colour and grey-scale fax pages
 *
 * A T.43 page (a BCIE) is the magic number ff a8, entries each
 * starting ff e1 (or, with a 4 octet length, ff e3) and "G3FAX" and a
 * number: 0 the image's type, resolution and bit-planes, 3 a palette,
 * 255 the header's end; then one JBIG BIE of every bit-plane of the
 * image, and ff a9.  Grey (L*) and colour (L*, a*, b*) samples are Gray
 * coded first, so that a small step in a sample changes one plane.
 * The planes come L*'s first (its most significant), then a*'s, then
 * b*'s; a palette's index from its most significant; one bit a colour
 * as red, green, blue (cyan, magenta, yellow, black).
 *
 */

#include <string.h>

#include <algorithm>

#include "jbig.h"
#include "jpeg.h"
#include "t43.h"

namespace dsp {

namespace {

#define T43_MAGIC       0xa8    /* after ff: the BCIH */
#define T43_END         0xa9    /* ... the end of the BCIE */
#define T43_APP1        0xe1    /* an entry, 2 octets of length */
#define T43_APP3        0xe3    /* ... 4 */
#define T43_VERSION     1997

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

/* an entry's start: its marker, length (with itself) and identifier */
void entry(std::vector<unsigned char> *out, bool four, u32 len, unsigned char n)
{
	static const unsigned char id[5] = {'G', '3', 'F', 'A', 'X'};
	out->push_back(0xff);
	out->push_back(four ? T43_APP3 : T43_APP1);
	if (four)
		put32(out, len);
	else
		put16(out, len);
	out->insert(out->end(), id, id + 5);
	out->push_back(n);
}

/* the planes of component c */
s16 comp_bits(const T43Image &img, s16 c)
{
	switch (img.type) {
	case T43_RGB:
	case T43_CMY:
	case T43_CMYK:
		return 1;
	case T43_COLOUR:
		return img.bits[c];
	default:
		return img.bits[0];
	}
}

/* the type and its bits allowed (T.43 Table 7, T.4 Table G.1) */
bool type_ok(const T43Image &img)
{
	switch (img.type) {
	case T43_RGB:
	case T43_CMY:
		return img.bits[0] == 3;
	case T43_CMYK:
		return img.bits[0] == 4;
	case T43_PALETTE:
	case T43_PALETTE12:
		return img.bits[0] >= 1 && img.bits[0] <= 16;
	case T43_GREY:
		return img.bits[0] >= 1 && img.bits[0] <= 12;
	case T43_COLOUR:
		for (s16 c = 0; c < 3; c++)
			if (img.bits[c] < 1 || img.bits[c] > 12)
				return false;
		return true;
	default:
		return false;
	}
}

/* the samples Gray coded (T.43 7.3.1) */
bool gray_coded(s16 type)
{
	return type == T43_GREY || type == T43_COLOUR;
}

/* an n bit sample as 8 bits: the top 8 of 12 (rounded, as T.42's 12 bit
   CIELAB is its 8 bit one times 16), fewer stretched */
unsigned char to8(u32 v, s16 n)
{
	if (n == 8)
		return (unsigned char)v;
	if (n > 8)
		return (unsigned char)std::min<u32>((v + (1u << (n - 9))) >> (n - 8), 255);
	return (unsigned char)((v * 255 + ((1u << n) - 1) / 2) / ((1u << n) - 1));
}

} // namespace

/* the samples of a pel */
s16 T43Image::comps() const
{
	switch (type) {
	case T43_RGB:
	case T43_CMY:
	case T43_COLOUR:
		return 3;
	case T43_CMYK:
		return 4;
	default:
		return 1;
	}
}

/* the header, the planes in one BIE, the end */
void t43_encode(const T43Image &img, const T43Options &opt, std::vector<unsigned char> *out)
{
	out->clear();
	out->push_back(0xff);
	out->push_back(T43_MAGIC);
	entry(out, false, 18, 0);
	put16(out, T43_VERSION);
	put16(out, (u32)img.dpi);
	out->push_back(0);                      /* JBIG */
	out->push_back((unsigned char)img.type);
	for (s16 c = 0; c < 4; c++)
		out->push_back((unsigned char)img.bits[c]);
	if (img.type == T43_PALETTE || img.type == T43_PALETTE12) {
		s16 mb = img.type == T43_PALETTE12 ? 2 : 1;
		u32 n = (u32)img.palette.size() / 3;
		entry(out, true, 16 + 3 * n * mb, 3);
		put16(out, mb == 2 ? 4 : 0);    /* the table ID */
		put32(out, n);
		for (u16 v : img.palette) {
			if (mb == 2)
				put16(out, v);
			else
				out->push_back((unsigned char)v);
		}
	}
	entry(out, false, 8, 0xff);             /* ECIH */

	/* the bit-planes, each component's from its most significant */
	JbigImage j;
	j.width = img.width;
	j.height = img.height;
	s16 comps = img.comps();
	size_t stride = j.stride(), n = (size_t)img.width * img.height;
	bool gray = gray_coded(img.type);
	for (s16 c = 0; c < comps; c++)
		for (s16 b = comp_bits(img, c) - 1; b >= 0; b--) {
			std::vector<unsigned char> plane(stride * img.height, 0);
			for (size_t i = 0; i < n; i++) {
				u32 v = img.pix[i * comps + c];
				if (gray)
					v ^= v >> 1;
				if ((v >> b) & 1) {
					size_t y = i / img.width, x = i % img.width;
					plane[y * stride + x / 8] |= (unsigned char)(0x80 >> (x % 8));
				}
			}
			j.bits.push_back(std::move(plane));
		}
	j.planes = (s16)j.bits.size();
	JbigOptions o;
	o.l0 = opt.l0;
	o.order = opt.plane_interleave ? JBG_ILEAVE : JBG_ILEAVE | JBG_SMID;
	std::vector<unsigned char> bie;
	jbig_encode_image(j, o, &bie);
	out->insert(out->end(), bie.begin(), bie.end());
	out->push_back(0xff);
	out->push_back(T43_END);
}

/* the BCIH's entries, then the BIE */
s16 t43_decode(const unsigned char *data, size_t len, u32 max_rows, T43Image *img)
{
	*img = T43Image();
	if (len < 2 || data[0] != 0xff || data[1] != T43_MAGIC)
		return -1;
	size_t pos = 2;
	bool have0 = false;
	for (;;) {
		if (pos + 4 > len || data[pos] != 0xff ||
		    (data[pos + 1] != T43_APP1 && data[pos + 1] != T43_APP3))
			return -1;
		bool four = data[pos + 1] == T43_APP3;
		size_t hl = four ? 4 : 2;
		if (pos + 2 + hl > len)
			return -1;
		u32 el = four ? get32(data + pos + 2) : get16(data + pos + 2);
		if (el < hl + 6 || el > len - pos - 2)
			return -1;
		const unsigned char *e = data + pos + 2 + hl, *id = e;
		size_t dl = el - hl - 6;
		const unsigned char *d = e + 6;
		pos += 2 + el;
		if (memcmp(id, "G", 1) != 0 || (id[1] != '3' && id[1] != '4') ||
		    memcmp(id + 2, "FAX", 3) != 0)
			continue;               /* (not ours) */
		if (id[5] == 0xff)
			break;                  /* ECIH */
		if (id[5] == 0) {
			if (dl < 10 || get16(d) != T43_VERSION || d[4] != 0)
				return -1;
			img->dpi = (s16)get16(d + 2);
			img->type = d[5];
			for (s16 c = 0; c < 4; c++)
				img->bits[c] = d[6 + c];
			if (!type_ok(*img))
				return -1;
			have0 = true;
		} else if (id[5] == 3) {
			if (dl < 6)
				return -1;
			s16 mb = get16(d) == 4 ? 2 : 1;
			u32 n = get32(d + 2);
			if (n == 0 || n > 65536 || (size_t)n * 3 * mb > dl - 6)
				return -1;
			img->palette.resize((size_t)n * 3);
			for (size_t i = 0; i < img->palette.size(); i++)
				img->palette[i] = mb == 2 ? (u16)get16(d + 6 + 2 * i) : d[6 + i];
		}
		/* (1, the gamut, and 2, the illuminant: the defaults
		   assumed) */
	}
	if (!have0)
		return -1;
	bool pal = img->type == T43_PALETTE || img->type == T43_PALETTE12;
	if (pal && img->palette.empty())
		return -1;

	s16 comps = img->comps(), planes = 0;
	for (s16 c = 0; c < comps; c++)
		planes += comp_bits(*img, c);
	JbigImage j;
	s16 r = jbig_decode_image(data + pos, len - pos, max_rows, planes, &j, nullptr);
	if (r < 0)
		return -1;
	if (j.planes != planes)
		return -1;
	img->width = j.width;
	img->height = j.height;
	size_t n = (size_t)j.width * j.height, stride = j.stride();
	img->pix.assign(n * comps, 0);
	s16 k = 0;
	for (s16 c = 0; c < comps; c++)
		for (s16 b = comp_bits(*img, c) - 1; b >= 0; b--, k++) {
			const unsigned char *p = j.bits[k].data();
			for (size_t y = 0, i = 0; y < j.height; y++)
				for (size_t x = 0; x < j.width; x++, i++)
					if ((p[y * stride + x / 8] >> (7 - x % 8)) & 1)
						img->pix[i * comps + c] |= (u16)(1u << b);
			std::vector<unsigned char>().swap(j.bits[k]);
		}
	if (gray_coded(img->type))
		for (u16 &v : img->pix)
			for (u16 s = v >> 1; s; s >>= 1)
				v ^= s;
	return r;
}

/* through T.42's CIELAB for grey, colour and a palette; the primaries
   as they are for one bit a colour */
void t43_to_srgb(const T43Image &img, std::vector<unsigned char> *out, s16 *comps)
{
	size_t n = (size_t)img.width * img.height;
	s16 ic = img.comps();
	if (img.type == T43_GREY) {
		std::vector<unsigned char> l(n);
		for (size_t i = 0; i < n; i++)
			l[i] = to8(img.pix[i], img.bits[0]);
		out->resize(n);
		t42_l_to_grey(l.data(), out->data(), n);
		*comps = 1;
		return;
	}
	*comps = 3;
	out->resize(n * 3);
	if (img.type == T43_COLOUR) {
		std::vector<unsigned char> lab(n * 3);
		for (size_t i = 0; i < n * 3; i++)
			lab[i] = to8(img.pix[i], img.bits[i % 3]);
		t42_lab_to_rgb(lab.data(), out->data(), n);
		return;
	}
	if (img.type == T43_PALETTE || img.type == T43_PALETTE12) {
		size_t entries = img.palette.size() / 3;
		std::vector<unsigned char> lab(entries * 3), rgb(entries * 3);
		for (size_t i = 0; i < lab.size(); i++)
			lab[i] = to8(img.palette[i], img.type == T43_PALETTE12 ? 12 : 8);
		t42_lab_to_rgb(lab.data(), rgb.data(), entries);
		for (size_t i = 0; i < n; i++) {
			size_t e = img.pix[i] < entries ? img.pix[i] : 0;
			memcpy(out->data() + i * 3, rgb.data() + e * 3, 3);
		}
		return;
	}
	for (size_t i = 0; i < n; i++) {
		const u16 *s = img.pix.data() + i * ic;
		unsigned char *o = out->data() + i * 3;
		bool k = img.type == T43_CMYK && s[3];
		for (s16 c = 0; c < 3; c++)
			o[c] = img.type == T43_RGB ? (s[c] ? 255 : 0) :
			       k || s[c] ? 0 : 255;
	}
}

} // namespace dsp
