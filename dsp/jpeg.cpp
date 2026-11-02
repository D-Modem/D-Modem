/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jpeg.cpp - ITU-T T.81 baseline JPEG for colour and grey-scale fax
 *               (T.4 Annex E, T.42)
 *
 * The encoder writes what T.4 E.6.2 lists: SOI, the G3FAX APP1 (version
 * 1994, the resolution), DQT (T.81's K.1 and K.2 tables scaled for the
 * quality), DHT (K.3 to K.6, the "preferred" tables of DCS bit 70), SOF0,
 * one interleaved scan of 8x8 DCT blocks (L* in four blocks and a*, b*
 * in one each for T.4's 4:1:1), and EOI; optionally DRI with RSTn, and
 * DNL in place of the frame header's line count.  The decoder takes any
 * baseline (or extended 8-bit Huffman) sequential frame: sampling
 * factors up to 4, scans of some components each, 8 or 16-bit
 * quantisation tables, restarts, DNL, APPn and COM skipped.
 *
 */

#include <math.h>
#include <string.h>

#include <algorithm>

#include "jpeg.h"

namespace dsp {

namespace {

/* the natural (row-major) index of each zig-zag position */
const unsigned char zz[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* T.81 K.1 and K.2: lightness and chrominance, natural order */
const unsigned char q_lum[64] = {
	16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
const unsigned char q_chr[64] = {
	17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* T.81 K.3 to K.6: the codes of each length, then the values */
const unsigned char dc_lum_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const unsigned char dc_chr_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const unsigned char dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const unsigned char ac_lum_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const unsigned char ac_lum_vals[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa,
};
const unsigned char ac_chr_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const unsigned char ac_chr_vals[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa,
};

#define M_SOF0  0xc0
#define M_SOF1  0xc1
#define M_DHT   0xc4
#define M_RST0  0xd0
#define M_SOI   0xd8
#define M_EOI   0xd9
#define M_SOS   0xda
#define M_DQT   0xdb
#define M_DNL   0xdc
#define M_DRI   0xdd
#define M_APP1  0xe1

/* a Huffman table: for the encoder each value's code, for the decoder
   T.81 F.2.2.3's MAXCODE, VALPTR and MINCODE */
struct Huff {
	unsigned char bits[16];
	unsigned char vals[256];
	s16 n = 0;
	u16 code[256];
	unsigned char size[256];
	s32 maxcode[18], mincode[17], valptr[17];

	/* the table from its counts and values (T.81 Annex C); false when
	   they do not make a code */
	bool init(const unsigned char *bits_, const unsigned char *vals_)
	{
		memcpy(bits, bits_, 16);
		n = 0;
		for (int i = 0; i < 16; i++)
			n = (s16)(n + bits[i]);
		if (n > 256)
			return false;
		memcpy(vals, vals_, n);
		memset(size, 0, sizeof(size));
		u32 c = 0;
		s16 k = 0;
		for (int l = 1; l <= 16; l++) {
			valptr[l] = k;
			mincode[l] = (s32)c;
			for (int i = 0; i < bits[l - 1]; i++, k++) {
				code[vals[k]] = (u16)c;
				size[vals[k]] = (unsigned char)l;
				c++;
			}
			maxcode[l] = bits[l - 1] ? (s32)c - 1 : -1;
			if (c > (1u << l))
				return false;
			c <<= 1;
		}
		maxcode[17] = 0x7fffffff;
		return true;
	}
};

/* T.81 A.3.3's DCT basis: C(u)/2 cos((2x + 1) u pi / 16) */
struct Basis {
	float c[8][8];

	/* the table */
	Basis()
	{
		for (int u = 0; u < 8; u++)
			for (int x = 0; x < 8; x++)
				c[u][x] = (float)((u ? 0.5 : 0.5 / sqrt(2.0)) *
						  cos((2 * x + 1) * u * 3.14159265358979323846 / 16));
	}
};

const Basis basis;

/* the forward DCT of 8x8 samples (level shifted), natural order */
void fdct(const float *s, float *f)
{
	float t[64];
	for (int v = 0; v < 8; v++)
		for (int x = 0; x < 8; x++) {
			float a = 0;
			for (int y = 0; y < 8; y++)
				a += basis.c[v][y] * s[y * 8 + x];
			t[v * 8 + x] = a;
		}
	for (int v = 0; v < 8; v++)
		for (int u = 0; u < 8; u++) {
			float a = 0;
			for (int x = 0; x < 8; x++)
				a += basis.c[u][x] * t[v * 8 + x];
			f[v * 8 + u] = a;
		}
}

/* the inverse DCT into 8x8 samples (level shifted back, clamped) */
void idct(const float *f, unsigned char *out, size_t stride)
{
	float t[64];
	for (int v = 0; v < 8; v++)
		for (int x = 0; x < 8; x++) {
			float a = 0;
			for (int u = 0; u < 8; u++)
				a += basis.c[u][x] * f[v * 8 + u];
			t[v * 8 + x] = a;
		}
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			float a = 128;
			for (int v = 0; v < 8; v++)
				a += basis.c[v][y] * t[v * 8 + x];
			int s = (int)lrintf(a);
			out[y * stride + x] = (unsigned char)(s < 0 ? 0 : s > 255 ? 255 : s);
		}
}

/* entropy-coded bits out, 0xff stuffed with 0x00 */
struct BitOut {
	std::vector<unsigned char> *out;
	u32 acc = 0;
	int n = 0;

	/* len bits of v, the highest first */
	void put(u32 v, int len)
	{
		acc = (acc << len) | (v & ((1u << len) - 1));
		n += len;
		while (n >= 8) {
			unsigned char b = (unsigned char)(acc >> (n - 8));
			out->push_back(b);
			if (b == 0xff)
				out->push_back(0);
			n -= 8;
		}
		acc &= (1u << n) - 1;
	}

	/* the last byte filled with ones */
	void flush()
	{
		if (n)
			put(0x7f, 8 - n);
	}
};

/* a marker and its segment's length */
void marker(std::vector<unsigned char> *out, int m, size_t len)
{
	out->push_back(0xff);
	out->push_back((unsigned char)m);
	if (len) {
		out->push_back((unsigned char)(len >> 8));
		out->push_back((unsigned char)len);
	}
}

/* a 16-bit number appended */
void put16(std::vector<unsigned char> *out, u32 v)
{
	out->push_back((unsigned char)(v >> 8));
	out->push_back((unsigned char)v);
}

/* the bits of v's magnitude category, and the category */
int category(int v)
{
	int a = v < 0 ? -v : v, s = 0;
	while (a) {
		s++;
		a >>= 1;
	}
	return s;
}

/* one block: its DC as a difference from pred, then the run-lengths of
   its AC coefficients */
void encode_block(BitOut *bo, const float *s, const u16 *q, const Huff &dc, const Huff &ac, int *pred)
{
	float f[64];
	int c[64];
	fdct(s, f);
	for (int k = 0; k < 64; k++)
		c[k] = (int)lrintf(f[zz[k]] / q[zz[k]]);
	int d = c[0] - *pred, t = category(d);
	*pred = c[0];
	bo->put(dc.code[t], dc.size[t]);
	if (t)
		bo->put((u32)(d < 0 ? d - 1 : d), t);
	int run = 0;
	for (int k = 1; k < 64; k++) {
		if (!c[k]) {
			run++;
			continue;
		}
		while (run > 15) {
			bo->put(ac.code[0xf0], ac.size[0xf0]);
			run -= 16;
		}
		int sz = category(c[k]), rs = run << 4 | sz;
		bo->put(ac.code[rs], ac.size[rs]);
		bo->put((u32)(c[k] < 0 ? c[k] - 1 : c[k]), sz);
		run = 0;
	}
	if (run)
		bo->put(ac.code[0], ac.size[0]);
}

/* the quantisation table for a quality, as libjpeg scales T.81's */
void scale_q(const unsigned char *base, s16 quality, u16 *q)
{
	int s = quality < 1 ? 5000 : quality < 50 ? 5000 / quality :
		quality > 100 ? 0 : 200 - quality * 2;
	for (int i = 0; i < 64; i++) {
		int v = (base[i] * s + 50) / 100;
		q[i] = (u16)(v < 1 ? 1 : v > 255 ? 255 : v);
	}
}

/* entropy-coded bits in, as T.81 F.2.2.5 reads them: 0xff 0x00 is
   0xff, a marker gives zeros (and is left there) */
struct BitIn {
	const unsigned char *d;
	size_t len, pos;
	u32 acc = 0;
	int n = 0;
	bool at_marker = false, ended = false;

	/* one bit */
	int bit()
	{
		if (!n) {
			unsigned char b = 0;
			if (pos >= len) {
				ended = true;
			} else if (d[pos] != 0xff) {
				b = d[pos++];
			} else if (pos + 1 < len && d[pos + 1] == 0) {
				b = 0xff;
				pos += 2;
			} else {
				at_marker = true;
			}
			acc = b;
			n = 8;
		}
		n--;
		return (acc >> n) & 1;
	}

	/* s bits, the highest first */
	int bits(int s)
	{
		int v = 0;
		while (s--)
			v = v << 1 | bit();
		return v;
	}

	/* the remaining bits of the byte dropped (before a marker) */
	void align()
	{
		n = 0;
	}

	/* the scan's data has ended at a marker other than RSTn: only
	   padding left before it */
	bool scan_end() const
	{
		if (pos >= len)
			return true;
		return n < 8 && d[pos] == 0xff && pos + 1 < len && d[pos + 1] != 0 &&
		       (d[pos + 1] & 0xf8) != M_RST0;
	}
};

/* a Huffman-coded value, -1 when no code matches */
int huff_decode(BitIn *bi, const Huff &h)
{
	s32 code = 0;
	for (int l = 1; l <= 16; l++) {
		code = code << 1 | bi->bit();
		if (h.maxcode[l] >= 0 && code <= h.maxcode[l] && code >= h.mincode[l])
			return h.vals[h.valptr[l] + code - h.mincode[l]];
	}
	return -1;
}

/* T.81 F.2.2.1's EXTEND */
int extend(int v, int t)
{
	return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v;
}

/* a frame component */
struct Comp {
	int id, h, v, tq;
	int td = 0, ta = 0;
	int pred = 0;
	u32 pw = 0;                     /* the plane's width (padded) */
	std::vector<unsigned char> plane;
};

/* one block decoded into its place; false when its codes are broken */
bool decode_block(BitIn *bi, Comp *c, const Huff &dc, const Huff &ac, const u16 *q, u32 bx, u32 by)
{
	float f[64] = {0};
	int t = huff_decode(bi, dc);
	if (t < 0 || t > 11)
		return false;
	c->pred += t ? extend(bi->bits(t), t) : 0;
	f[0] = (float)(c->pred * q[0]);
	for (int k = 1; k < 64;) {
		int rs = huff_decode(bi, ac);
		if (rs < 0)
			return false;
		int r = rs >> 4, s = rs & 15;
		if (!s) {
			if (r != 15)
				break;
			k += 16;
			continue;
		}
		k += r;
		if (k > 63)
			return false;
		f[zz[k]] = (float)(extend(bi->bits(s), s) * q[zz[k]]);
		k++;
	}
	size_t need = ((size_t)by * 8 + 8) * c->pw;
	if (c->plane.size() < need)
		c->plane.resize(need, 128);
	idct(f, c->plane.data() + (size_t)by * 8 * c->pw + (size_t)bx * 8, c->pw);
	return true;
}

/* the colour tables: sRGB's transfer function, the matrices between
   linear sRGB and XYZ for D50 (Bradford adapted) */
struct Colour {
	float lin[256];
	double m[3][3], mi[3][3];

	/* the tables */
	Colour()
	{
		for (int i = 0; i < 256; i++) {
			double c = i / 255.0;
			lin[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
		}
		const double a[3][3] = {
			{0.4360747, 0.3850649, 0.1430804},
			{0.2225045, 0.7168786, 0.0606169},
			{0.0139322, 0.0971045, 0.7141733},
		};
		memcpy(m, a, sizeof(m));
		double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
			a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
			a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) {
				int i1 = (j + 1) % 3, i2 = (j + 2) % 3;
				int j1 = (i + 1) % 3, j2 = (i + 2) % 3;
				mi[i][j] = (a[i1][j1] * a[i2][j2] - a[i1][j2] * a[i2][j1]) / det;
			}
	}
};

const Colour colour;

/* D50's white point (T.4 E.6.4) */
const double xn = 0.96422, yn = 1.0, zn = 0.82521;

/* CIELAB's f() */
double lab_f(double t)
{
	const double e = 6.0 / 29;
	return t > e * e * e ? cbrt(t) : t / (3 * e * e) + 4.0 / 29;
}

/* CIELAB's f() inverted */
double lab_finv(double t)
{
	const double e = 6.0 / 29;
	return t > e ? t * t * t : 3 * e * e * (t - 4.0 / 29);
}

/* a linear value to an sRGB octet */
unsigned char srgb(double v)
{
	v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
	long s = lrint(v * 255);
	return (unsigned char)(s < 0 ? 0 : s > 255 ? 255 : s);
}

/* a value rounded and clamped to an octet */
unsigned char octet(double v)
{
	long s = lrint(v);
	return (unsigned char)(s < 0 ? 0 : s > 255 ? 255 : s);
}

} // namespace

/* the markers, the tables, then the scan's MCUs */
void jpeg_encode(const JpegImage &img, const JpegOptions &opt, std::vector<unsigned char> *out)
{
	const u32 w = img.width, h = img.height;
	const int nc = img.comps == 3 ? 3 : 1;
	const int sub = nc == 3 && opt.subsample, hmax = sub ? 2 : 1;
	u16 q[2][64];
	scale_q(q_lum, opt.quality, q[0]);
	scale_q(q_chr, opt.quality, q[1]);
	Huff dc[2], ac[2];
	dc[0].init(dc_lum_bits, dc_vals);
	ac[0].init(ac_lum_bits, ac_lum_vals);
	dc[1].init(dc_chr_bits, dc_vals);
	ac[1].init(ac_chr_bits, ac_chr_vals);

	out->clear();
	marker(out, M_SOI, 0);
	if (opt.dpi) {
		static const unsigned char g3[6] = {'G', '3', 'F', 'A', 'X', 0};
		marker(out, M_APP1, 12);
		out->insert(out->end(), g3, g3 + 6);
		put16(out, 0x07ca);
		put16(out, (u32)opt.dpi);
	}
	int nt = nc == 3 ? 2 : 1;
	marker(out, M_DQT, 2 + 65 * nt);
	for (int t = 0; t < nt; t++) {
		out->push_back((unsigned char)t);
		for (int k = 0; k < 64; k++)
			out->push_back((unsigned char)q[t][zz[k]]);
	}
	size_t dl = 2;
	for (int t = 0; t < nt; t++)
		dl += 2 * 17 + dc[t].n + ac[t].n;
	marker(out, M_DHT, dl);
	for (int t = 0; t < nt; t++) {
		const Huff *hs[2] = {&dc[t], &ac[t]};
		for (int k = 0; k < 2; k++) {
			out->push_back((unsigned char)(k << 4 | t));
			out->insert(out->end(), hs[k]->bits, hs[k]->bits + 16);
			out->insert(out->end(), hs[k]->vals, hs[k]->vals + hs[k]->n);
		}
	}
	if (opt.restart) {
		marker(out, M_DRI, 4);
		put16(out, opt.restart);
	}
	marker(out, M_SOF0, 8 + 3 * nc);
	out->push_back(8);
	put16(out, opt.dnl ? 0 : h);
	put16(out, w);
	out->push_back((unsigned char)nc);
	for (int c = 0; c < nc; c++) {
		int f = c == 0 ? hmax : 1;
		out->push_back((unsigned char)c);
		out->push_back((unsigned char)(f << 4 | f));
		out->push_back((unsigned char)(c ? 1 : 0));
	}
	marker(out, M_SOS, 6 + 2 * nc);
	out->push_back((unsigned char)nc);
	for (int c = 0; c < nc; c++) {
		out->push_back((unsigned char)c);
		out->push_back((unsigned char)(c ? 0x11 : 0x00));
	}
	out->push_back(0);
	out->push_back(63);
	out->push_back(0);

	/* the samples of a component, at its own size (a* and b* averaged
	   over 2x2 when subsampled), the edges repeated */
	const u32 mw = 8 * hmax, mcux = (w + mw - 1) / mw, mcuy = (h + mw - 1) / mw;
	auto sample = [&](int c, u32 x, u32 y) -> float {
		if (c == 0 || !sub) {
			x = std::min(x, w - 1);
			y = std::min(y, h - 1);
			return img.pix[((size_t)y * w + x) * nc + c];
		}
		int s = 0;
		for (u32 dy = 0; dy < 2; dy++)
			for (u32 dx = 0; dx < 2; dx++) {
				u32 xx = std::min(2 * x + dx, w - 1), yy = std::min(2 * y + dy, h - 1);
				s += img.pix[((size_t)yy * w + xx) * nc + c];
			}
		return s / 4.0f;
	};
	BitOut bo;
	bo.out = out;
	int pred[3] = {0, 0, 0};
	u32 mcus = 0, rst = 0;
	float s[64];
	for (u32 my = 0; my < mcuy && w && h; my++)
		for (u32 mx = 0; mx < mcux; mx++) {
			if (opt.restart && mcus && mcus % opt.restart == 0) {
				bo.flush();
				marker(out, M_RST0 + (rst++ & 7), 0);
				pred[0] = pred[1] = pred[2] = 0;
			}
			mcus++;
			for (int c = 0; c < nc; c++) {
				int f = c == 0 ? hmax : 1, t = c ? 1 : 0;
				for (int by = 0; by < f; by++)
					for (int bx = 0; bx < f; bx++) {
						u32 x0 = (mx * f + bx) * 8, y0 = (my * f + by) * 8;
						for (int y = 0; y < 8; y++)
							for (int x = 0; x < 8; x++)
								s[y * 8 + x] = sample(c, x0 + x, y0 + y) - 128;
						encode_block(&bo, s, q[t], dc[t], ac[t], &pred[c]);
					}
			}
		}
	bo.flush();
	if (opt.dnl) {
		marker(out, M_DNL, 4);
		put16(out, h);
	}
	marker(out, M_EOI, 0);
}

/* the markers in turn, each scan's MCUs into the components' planes,
   then the planes to the image's size */
s16 jpeg_decode(const unsigned char *data, size_t len, u32 max_rows, JpegImage *img)
{
	u16 q[4][64];
	Huff dc[4], ac[4];
	bool have_q[4] = {false}, have_dc[4] = {false}, have_ac[4] = {false};
	Comp comps[4];
	int nf = 0, hmax = 1, vmax = 1;
	u32 x = 0, y = 0, ri = 0, rows = 0;
	bool frame = false, scanned = false, early = false, y_open = false;
	img->width = img->height = 0;
	img->comps = 0;
	img->pix.clear();
	img->dpi = 0;

	if (len < 4 || data[0] != 0xff || data[1] != M_SOI)
		return -1;
	size_t pos = 2;
	for (;;) {
		while (pos < len && data[pos] != 0xff)
			pos++;              /* (junk between markers) */
		while (pos < len && data[pos] == 0xff)
			pos++;
		if (pos >= len) {
			early = true;
			break;
		}
		int m = data[pos++];
		if (m == M_EOI)
			break;
		if ((m & 0xf8) == M_RST0 || m == 0x01)
			continue;
		if (pos + 2 > len) {
			early = true;
			break;
		}
		size_t sl = (size_t)data[pos] << 8 | data[pos + 1];
		if (sl < 2 || pos + sl > len) {
			early = true;
			break;
		}
		const unsigned char *p = data + pos + 2, *end = data + pos + sl;
		pos += sl;
		switch (m) {
		case M_APP1:
			if (end - p >= 10 && !memcmp(p, "G3FAX", 6))
				img->dpi = (s16)(p[8] << 8 | p[9]);
			break;
		case M_DQT:
			while (p < end) {
				int pq = *p >> 4, tq = *p & 3;
				p++;
				if (end - p < (pq ? 128 : 64))
					return -1;
				for (int k = 0; k < 64; k++) {
					q[tq][zz[k]] = pq ? (u16)(p[0] << 8 | p[1]) : p[0];
					p += pq ? 2 : 1;
				}
				have_q[tq] = true;
			}
			break;
		case M_DHT:
			while (p < end) {
				int tc = *p >> 4, th = *p & 3;
				p++;
				if (end - p < 16 || tc > 1)
					return -1;
				int n = 0;
				for (int i = 0; i < 16; i++)
					n += p[i];
				if (end - p < 16 + n)
					return -1;
				Huff &hf = tc ? ac[th] : dc[th];
				if (!hf.init(p, p + 16))
					return -1;
				(tc ? have_ac : have_dc)[th] = true;
				p += 16 + n;
			}
			break;
		case M_DRI:
			if (end - p >= 2)
				ri = (u32)(p[0] << 8 | p[1]);
			break;
		case M_SOF0:
		case M_SOF1:
			if (frame || end - p < 6 || p[0] != 8)
				return -1;
			y = (u32)(p[1] << 8 | p[2]);
			x = (u32)(p[3] << 8 | p[4]);
			nf = p[5];
			if ((nf != 1 && nf != 3) || end - p < 6 + 3 * nf || !x || x > 16384)
				return -1;
			for (int c = 0; c < nf; c++) {
				comps[c].id = p[6 + 3 * c];
				comps[c].h = p[7 + 3 * c] >> 4;
				comps[c].v = p[7 + 3 * c] & 15;
				comps[c].tq = p[8 + 3 * c] & 3;
				if (comps[c].h < 1 || comps[c].h > 4 || comps[c].v < 1 || comps[c].v > 4)
					return -1;
				hmax = std::max(hmax, comps[c].h);
				vmax = std::max(vmax, comps[c].v);
			}
			if (!y) {
				y_open = true;
				y = max_rows;
			}
			y = std::min(y, max_rows);
			for (int c = 0; c < nf; c++)
				comps[c].pw = (x + 8 * hmax - 1) / (8 * hmax) * comps[c].h * 8;
			frame = true;
			break;
		case M_DNL:
			if (end - p >= 2 && y_open) {
				u32 n = (u32)(p[0] << 8 | p[1]);
				if (n && n < rows)
					rows = n;
				y = rows;
				y_open = false;
			}
			break;
		case M_SOS: {
			if (!frame || end - p < 1)
				return -1;
			int ns = p[0];
			if (ns < 1 || ns > nf || end - p < 4 + 2 * ns)
				return -1;
			Comp *sc[4];
			for (int i = 0; i < ns; i++) {
				int id = p[1 + 2 * i], k;
				for (k = 0; k < nf && comps[k].id != id; k++)
					;
				if (k == nf)
					return -1;
				sc[i] = &comps[k];
				sc[i]->td = p[2 + 2 * i] >> 4 & 3;
				sc[i]->ta = p[2 + 2 * i] & 3;
				sc[i]->pred = 0;
				if (!have_dc[sc[i]->td] || !have_ac[sc[i]->ta] || !have_q[sc[i]->tq])
					return -1;
			}
			BitIn bi;
			bi.d = data;
			bi.len = len;
			bi.pos = pos;
			/* the MCUs: of all ns components (each its h by v
			   blocks) if interleaved, else one block each */
			u32 mcux, mcuy, lines;
			if (ns > 1) {
				mcux = (x + 8 * hmax - 1) / (8 * hmax);
				mcuy = (y + 8 * vmax - 1) / (8 * vmax);
				lines = 8 * vmax;
			} else {
				u32 cw = (x * sc[0]->h + hmax - 1) / hmax;
				u32 ch = (y * sc[0]->v + vmax - 1) / vmax;
				mcux = (cw + 7) / 8;
				mcuy = (ch + 7) / 8;
				lines = 8 * vmax / sc[0]->v;
			}
			u32 mcus = 0, done_rows = 0, rst = 0;
			bool broken = false;
			for (u32 my = 0; my < mcuy && !broken; my++) {
				for (u32 mx = 0; mx < mcux && !broken; mx++) {
					if (ri && mcus && mcus % ri == 0) {
						/* RSTn: the bits up to it dropped */
						bi.align();
						size_t k = bi.pos;
						while (k + 1 < len && !(data[k] == 0xff &&
							 data[k + 1] == M_RST0 + (rst & 7)))
							k++;
						bi.pos = k + 2;
						bi.at_marker = false;
						rst++;
						for (int i = 0; i < ns; i++)
							sc[i]->pred = 0;
					}
					mcus++;
					for (int i = 0; i < ns && !broken; i++) {
						Comp *c = sc[i];
						int bh = ns > 1 ? c->h : 1, bv = ns > 1 ? c->v : 1;
						for (int by = 0; by < bv && !broken; by++)
							for (int bx = 0; bx < bh && !broken; bx++)
								broken = !decode_block(&bi, c, dc[c->td], ac[c->ta],
										       q[c->tq], mx * bh + bx, my * bv + by);
					}
				}
				if (bi.ended) {
					early = true;
					break;
				}
				if (!broken)
					done_rows = std::min(y, (my + 1) * lines);
				if (y_open && bi.scan_end())
					break;
			}
			if (broken)
				early = true;
			rows = scanned ? std::min(rows, done_rows) : done_rows;
			scanned = true;
			/* on to the marker after the scan's data */
			pos = bi.pos;
			while (pos + 1 < len && !(data[pos] == 0xff && data[pos + 1] != 0 &&
						  (data[pos + 1] & 0xf8) != M_RST0))
				pos++;
			break;
		}
		default:
			if (m >= 0xc2 && m <= 0xcf && m != M_DHT && m != 0xc8 && m != 0xcc)
				return -1;  /* (progressive, lossless, arithmetic) */
			break;          /* APPn, COM and the rest skipped */
		}
		if (early)
			break;
	}
	if (!frame || !scanned)
		return -1;
	if (y_open && rows > y)
		rows = y;
	img->width = x;
	img->height = std::min(rows, y);
	img->comps = (s16)(nf == 3 ? 3 : 1);
	img->pix.assign((size_t)img->width * img->height * img->comps, 128);
	for (int c = 0; c < nf; c++) {
		Comp &cp = comps[c];
		size_t prows = cp.pw ? cp.plane.size() / cp.pw : 0;
		for (u32 yy = 0; yy < img->height; yy++) {
			size_t sy = (size_t)yy * cp.v / vmax;
			if (sy >= prows)
				continue;
			const unsigned char *row = cp.plane.data() + sy * cp.pw;
			unsigned char *o = img->pix.data() + (size_t)yy * x * img->comps + c;
			for (u32 xx = 0; xx < x; xx++)
				o[(size_t)xx * img->comps] = row[(size_t)xx * cp.h / hmax];
		}
	}
	return early ? 1 : 0;
}

/* sRGB to XYZ (D50), to CIELAB, to T.4's octets */
void t42_rgb_to_lab(const unsigned char *rgb, unsigned char *lab, size_t n)
{
	for (size_t i = 0; i < n; i++, rgb += 3, lab += 3) {
		double r = colour.lin[rgb[0]], g = colour.lin[rgb[1]], b = colour.lin[rgb[2]];
		double xx = colour.m[0][0] * r + colour.m[0][1] * g + colour.m[0][2] * b;
		double yy = colour.m[1][0] * r + colour.m[1][1] * g + colour.m[1][2] * b;
		double zz_ = colour.m[2][0] * r + colour.m[2][1] * g + colour.m[2][2] * b;
		double fx = lab_f(xx / xn), fy = lab_f(yy / yn), fz = lab_f(zz_ / zn);
		lab[0] = octet((116 * fy - 16) * 255 / 100);
		lab[1] = octet(500 * (fx - fy) * 255 / 170 + 128);
		lab[2] = octet(200 * (fy - fz) * 255 / 200 + 96);
	}
}

/* T.4's octets to CIELAB, to XYZ (D50), to sRGB */
void t42_lab_to_rgb(const unsigned char *lab, unsigned char *rgb, size_t n)
{
	for (size_t i = 0; i < n; i++, rgb += 3, lab += 3) {
		double l = lab[0] * 100.0 / 255, a = (lab[1] - 128) * 170.0 / 255,
		       b = (lab[2] - 96) * 200.0 / 255;
		double fy = (l + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
		double xx = xn * lab_finv(fx), yy = yn * lab_finv(fy), zz_ = zn * lab_finv(fz);
		for (int k = 0; k < 3; k++)
			rgb[k] = srgb(colour.mi[k][0] * xx + colour.mi[k][1] * yy + colour.mi[k][2] * zz_);
	}
}

/* a grey level's luminance to L* */
void t42_grey_to_l(const unsigned char *grey, unsigned char *l, size_t n)
{
	for (size_t i = 0; i < n; i++)
		l[i] = octet((116 * lab_f(colour.lin[grey[i]]) - 16) * 255 / 100);
}

/* L* to a grey level */
void t42_l_to_grey(const unsigned char *l, unsigned char *grey, size_t n)
{
	for (size_t i = 0; i < n; i++)
		grey[i] = srgb(lab_finv((l[i] * 100.0 / 255 + 16) / 116));
}

} // namespace dsp
