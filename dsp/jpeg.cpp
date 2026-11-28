/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jpeg.cpp - ITU-T T.81 sequential JPEG for colour and grey-scale fax
 *               (T.4 Annex E, T.42)
 *
 * The encoder writes what T.4 E.6.2 lists: SOI, the G3FAX APP1 (version
 * 1994, the resolution), G3FAX1 and G3FAX2 (E.6.6, E.6.7) for a gamut or
 * an illuminant that is not the default, DQT (T.81's K.1 and K.2 tables
 * scaled for the quality), DHT, SOF0 or SOF1, one interleaved scan of
 * 8x8 DCT blocks (L* in four blocks and a*, b* in one each for T.4's
 * 4:1:1), and EOI; optionally DRI with RSTn, and DNL in place of the
 * frame header's line count.  8-bit samples are baseline (SOF0) with
 * K.3 to K.6, the "preferred" tables of DCS bit 70; 12-bit samples
 * (DCS bit 71) are extended (SOF1) with tables made for the image (T.81
 * K.2), as K.3 to K.6 do not have the larger categories.  The decoder
 * takes any baseline or extended Huffman sequential frame of 8 or 12
 * bits: sampling factors up to 4, scans of some components each, 8 or
 * 16-bit quantisation tables, restarts, DNL, the G3FAX segments, other
 * APPn and COM skipped.
 *
 * The colour conversions go from sRGB to T.42 6.2.1's CIELAB (with the
 * Bradford adaptation from D65 to the illuminant's white) or 6.2.2's
 * ITU-YCC (sYCC), and back, as n-bit samples of a gamut's ranges and
 * offsets.
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

/* the inverse DCT into 8x8 samples (level shifted back by mid,
   clamped to 0 to top) */
void idct(const float *f, u16 *out, size_t stride, int mid, int top)
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
			float a = (float)mid;
			for (int v = 0; v < 8; v++)
				a += basis.c[v][y] * t[v * 8 + x];
			int s = (int)lrintf(a);
			out[y * stride + x] = (u16)(s < 0 ? 0 : s > top ? top : s);
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

/* where an encoder pass puts a block's symbols: the bits out (with
   the tables), or (cdc, cac given) only the count of each symbol */
struct Sink {
	BitOut *bo = nullptr;
	const Huff *dc = nullptr, *ac = nullptr;
	u32 *cdc = nullptr, *cac = nullptr;

	/* a DC (ac false) or AC symbol, then len bits of v */
	void sym(bool is_ac, int s, u32 v, int len)
	{
		if (cdc) {
			(is_ac ? cac : cdc)[s]++;
			return;
		}
		const Huff &h = is_ac ? *ac : *dc;
		bo->put(h.code[s], h.size[s]);
		if (len)
			bo->put(v, len);
	}
};

/* one block: its DC as a difference from pred, then the run-lengths of
   its AC coefficients; the AC ones at most of category acmax (10 for 8
   bits, 14 for 12) */
void encode_block(Sink *out, const float *s, const u16 *q, int *pred, int acmax)
{
	float f[64];
	int c[64];
	const int lim = (1 << acmax) - 1;
	fdct(s, f);
	for (int k = 0; k < 64; k++) {
		c[k] = (int)lrintf(f[zz[k]] / q[zz[k]]);
		if (k)
			c[k] = std::max(-lim, std::min(lim, c[k]));
	}
	int d = c[0] - *pred, t = category(d);
	*pred = c[0];
	out->sym(false, t, (u32)(d < 0 ? d - 1 : d), t);
	int run = 0;
	for (int k = 1; k < 64; k++) {
		if (!c[k]) {
			run++;
			continue;
		}
		while (run > 15) {
			out->sym(true, 0xf0, 0, 0);
			run -= 16;
		}
		int sz = category(c[k]), rs = run << 4 | sz;
		out->sym(true, rs, (u32)(c[k] < 0 ? c[k] - 1 : c[k]), sz);
		run = 0;
	}
	if (run)
		out->sym(true, 0, 0, 0);
}

/* T.81 K.2: a table's BITS and HUFFVAL for the symbols' counts, the
   codes at most 16 bits and none of all ones (a reserved symbol of
   count 1 takes it, as in K.2's Figure K.1) */
void optimal_table(const u32 *count, unsigned char *bits, unsigned char *vals)
{
	long long freq[257];
	int size[257], next[257];
	bool any = false;
	for (int i = 0; i < 256; i++) {
		freq[i] = count[i];
		any = any || count[i];
	}
	if (!any)
		freq[0] = 1;
	freq[256] = 1;
	for (int i = 0; i < 257; i++) {
		size[i] = 0;
		next[i] = -1;
	}
	for (;;) {
		/* the two least counts (the higher symbol first on a tie) */
		int c1 = -1, c2 = -1;
		for (int i = 0; i < 257; i++)
			if (freq[i] && (c1 < 0 || freq[i] <= freq[c1]))
				c1 = i;
		for (int i = 0; i < 257; i++)
			if (freq[i] && i != c1 && (c2 < 0 || freq[i] <= freq[c2]))
				c2 = i;
		if (c2 < 0)
			break;
		freq[c1] += freq[c2];
		freq[c2] = 0;
		size[c1]++;
		while (next[c1] >= 0) {
			c1 = next[c1];
			size[c1]++;
		}
		next[c1] = c2;
		size[c2]++;
		while (next[c2] >= 0) {
			c2 = next[c2];
			size[c2]++;
		}
	}
	int n[258] = {0};
	for (int i = 0; i < 257; i++)
		if (size[i])
			n[size[i]]++;
	/* K.2's Figure K.3: the codes longer than 16 bits moved up */
	for (int i = 257; i > 16; i--)
		while (n[i] > 0) {
			int j = i - 2;
			while (n[j] == 0)
				j--;
			n[i] -= 2;
			n[i - 1]++;
			n[j + 1] += 2;
			n[j]--;
		}
	int i = 16;
	while (n[i] == 0)
		i--;
	n[i]--;                         /* (the reserved symbol's code) */
	for (int l = 1; l <= 16; l++)
		bits[l - 1] = (unsigned char)n[l];
	int k = 0;
	for (int l = 1; l <= 256; l++)
		for (int v = 0; v < 256; v++)
			if (size[v] == l)
				vals[k++] = (unsigned char)v;
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
	std::vector<u16> plane;
};

/* one block of P-bit samples decoded into its place; false when its
   codes are broken */
bool decode_block(BitIn *bi, Comp *c, const Huff &dc, const Huff &ac, const u16 *q, u32 bx, u32 by,
		  int P)
{
	float f[64] = {0};
	int t = huff_decode(bi, dc);
	if (t < 0 || t > P + 3)
		return false;
	c->pred += t ? extend(bi->bits(t), t) : 0;
	f[0] = (float)(c->pred * q[0]);
	for (int k = 1; k < 64;) {
		int rs = huff_decode(bi, ac);
		if (rs < 0)
			return false;
		int r = rs >> 4, s = rs & 15;
		if (s > P + 2)
			return false;
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
	const int mid = 1 << (P - 1);
	if (c->plane.size() < need)
		c->plane.resize(need, (u16)mid);
	idct(f, c->plane.data() + (size_t)by * 8 * c->pw + (size_t)bx * 8, c->pw, mid, (1 << P) - 1);
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


/* a 3x3 matrix inverted (by its cofactors) */
void invert3(const double a[3][3], double r[3][3])
{
	double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
		a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
		a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			int i1 = (j + 1) % 3, i2 = (j + 2) % 3;
			int j1 = (i + 1) % 3, j2 = (i + 2) % 3;
			r[i][j] = (a[i1][j1] * a[i2][j2] - a[i1][j2] * a[i2][j1]) / det;
		}
}

/* the product a b of two 3x3 matrices */
void mul3(const double a[3][3], const double b[3][3], double r[3][3])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

/* m v into r */
void apply3(const double m[3][3], const double v[3], double r[3])
{
	for (int i = 0; i < 3; i++)
		r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

/* the x, y chromaticity of a colour temperature: the CIE daylight locus
   (CIE 15) from 4000 K to 25000 K, Kim's cubic fit of the Planckian
   locus from 1667 K; false outside these */
bool temperature_xy(double t, double *x, double *y)
{
	if (t >= 4000 && t <= 25000) {
		double t2 = t * t, t3 = t2 * t;
		*x = t <= 7000 ? -4.6070e9 / t3 + 2.9678e6 / t2 + 0.09911e3 / t + 0.244063 :
			-2.0064e9 / t3 + 1.9018e6 / t2 + 0.24748e3 / t + 0.237040;
		*y = -3.000 * *x * *x + 2.870 * *x - 0.275;
		return true;
	}
	if (t >= 1667 && t < 4000) {
		double t2 = t * t, t3 = t2 * t;
		double xx = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
		double x2 = xx * xx, x3 = x2 * xx;
		*x = xx;
		*y = t <= 2222 ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * xx - 0.20219683 :
			-0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * xx - 0.16748867;
		return true;
	}
	return false;
}

/* a conversion between sRGB and the samples of a T42Colour, made
   ready: the matrices, each component's scale and offset */
struct Conv {
	bool ycc = false;
	double top = 255;               /* 2^bits - 1 */
	double scale[3], off[3];
	double w[3];                    /* CIELAB: the white point */
	double m[3][3], mi[3][3];       /* linear sRGB to XYZ (or sR'G'B' to
					   YCC), and back */

	/* the tables for c; false when c is not one that we convert */
	bool init(const T42Colour &c)
	{
		if (c.bits < 1 || c.bits > 16)
			return false;
		top = (double)((1 << c.bits) - 1);
		if (c.space == T42_YCC) {
			/* 6.2.2.3: Y 0 to 1, Cb and Cr -0.5 to 0.5; the
			   matrix of T.42 Appendix III (III-7) */
			static const double a[3][3] = {
				{0.2990, 0.5870, 0.1140},
				{-0.1687, -0.3313, 0.5000},
				{0.5000, -0.4187, -0.0813},
			};
			ycc = true;
			memcpy(m, a, sizeof(m));
			invert3(m, mi);
			for (int i = 0; i < 3; i++) {
				scale[i] = top;
				off[i] = i ? (double)(1 << (c.bits - 1)) : 0;
			}
			return true;
		}
		if (c.space != T42_CIELAB || !t42_white(c.illuminant, w))
			return false;
		for (int i = 0; i < 3; i++) {
			if (c.gamut.range[i] <= 0)
				return false;
			/* E.6.6: the offset in the 8 most significant bits */
			scale[i] = top / c.gamut.range[i];
			off[i] = c.gamut.offset[i] * pow(2.0, c.bits - 8);
		}
		if (c.illuminant == T42_D50) {
			memcpy(m, colour.m, sizeof(m));
			memcpy(mi, colour.mi, sizeof(mi));
			return true;
		}
		/* sRGB's D65 white adapted to w with the Bradford matrix */
		static const double srgb_m[3][3] = {
			{0.4124564, 0.3575761, 0.1804375},
			{0.2126729, 0.7151522, 0.0721750},
			{0.0193339, 0.1191920, 0.9503041},
		};
		static const double brad[3][3] = {
			{0.8951, 0.2664, -0.1614},
			{-0.7502, 1.7135, 0.0367},
			{0.0389, -0.0685, 1.0296},
		};
		double bi[3][3], d65[3] = {0.9504700, 1.0, 1.0888300}, s[3], d[3];
		invert3(brad, bi);
		apply3(brad, d65, s);
		apply3(brad, w, d);
		double k[3][3] = {{d[0] / s[0], 0, 0}, {0, d[1] / s[1], 0}, {0, 0, d[2] / s[2]}};
		double t1[3][3], t2[3][3];
		mul3(k, brad, t1);
		mul3(bi, t1, t2);
		mul3(t2, srgb_m, m);
		invert3(m, mi);
		return true;
	}

	/* a component's real value to its sample */
	u16 sample(int i, double v) const
	{
		long n = lrint(scale[i] * v + off[i]);
		return (u16)(n < 0 ? 0 : n > (long)top ? (long)top : n);
	}

	/* a component's sample to its real value */
	double value(int i, u16 n) const
	{
		return (n - off[i]) / scale[i];
	}
};
} // namespace

/* the markers, the tables, then the scan's MCUs (for 12 bits, a pass
   that counts the symbols first, for the tables) */
bool jpeg_encode(const JpegImage &img, const JpegOptions &opt, std::vector<unsigned char> *out)
{
	const u32 w = img.width, h = img.height;
	const int nc = img.comps == 3 ? 3 : 1;
	const int sub = nc == 3 && opt.subsample, hmax = sub ? 2 : 1;
	const int P = img.colour.bits, mid = 1 << (P - 1);
	out->clear();
	if (P != 8 && P != 12)
		return false;
	const size_t need = (size_t)w * h * nc;
	if ((P == 8 ? img.pix.size() : img.pix16.size()) < need)
		return false;
	u16 q[2][64];
	scale_q(q_lum, opt.quality, q[0]);
	scale_q(q_chr, opt.quality, q[1]);
	int nt = nc == 3 ? 2 : 1;

	/* the samples of a component, at its own size (a* and b* averaged
	   over 2x2 when subsampled), the edges repeated */
	const u32 mw = 8 * hmax, mcux = (w + mw - 1) / mw, mcuy = (h + mw - 1) / mw;
	auto at = [&](size_t i) -> int {
		return P == 8 ? img.pix[i] : img.pix16[i];
	};
	auto sample = [&](int c, u32 x, u32 y) -> float {
		if (c == 0 || !sub) {
			x = std::min(x, w - 1);
			y = std::min(y, h - 1);
			return (float)at(((size_t)y * w + x) * nc + c);
		}
		int s = 0;
		for (u32 dy = 0; dy < 2; dy++)
			for (u32 dx = 0; dx < 2; dx++) {
				u32 xx = std::min(2 * x + dx, w - 1), yy = std::min(2 * y + dy, h - 1);
				s += at(((size_t)yy * w + xx) * nc + c);
			}
		return s / 4.0f;
	};
	Huff dc[2], ac[2];
	BitOut bo;
	bo.out = out;
	/* the scan's MCUs into sinks (one a table), RSTn between (counted
	   or not) */
	auto scan = [&](Sink *sinks, bool emit) {
		int pred[3] = {0, 0, 0};
		u32 mcus = 0, rst = 0;
		float s[64];
		for (u32 my = 0; my < mcuy && w && h; my++)
			for (u32 mx = 0; mx < mcux; mx++) {
				if (opt.restart && mcus && mcus % opt.restart == 0) {
					if (emit) {
						bo.flush();
						marker(out, M_RST0 + (rst & 7), 0);
					}
					rst++;
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
									s[y * 8 + x] = sample(c, x0 + x, y0 + y) - mid;
							encode_block(&sinks[t], s, q[t], &pred[c], P + 2);
						}
				}
			}
	};
	if (P == 8) {
		dc[0].init(dc_lum_bits, dc_vals);
		ac[0].init(ac_lum_bits, ac_lum_vals);
		dc[1].init(dc_chr_bits, dc_vals);
		ac[1].init(ac_chr_bits, ac_chr_vals);
	} else {
		std::vector<u32> counts(4 * 256, 0);
		Sink cs[2];
		for (int t = 0; t < 2; t++) {
			cs[t].cdc = &counts[(size_t)t * 512];
			cs[t].cac = &counts[(size_t)t * 512 + 256];
		}
		scan(cs, false);
		for (int t = 0; t < nt; t++) {
			unsigned char bits[16], vals[256];
			optimal_table(cs[t].cdc, bits, vals);
			dc[t].init(bits, vals);
			optimal_table(cs[t].cac, bits, vals);
			ac[t].init(bits, vals);
		}
	}

	marker(out, M_SOI, 0);
	if (opt.dpi) {
		static const unsigned char g3[6] = {'G', '3', 'F', 'A', 'X', 0};
		marker(out, M_APP1, 12);
		out->insert(out->end(), g3, g3 + 6);
		put16(out, 0x07ca);
		put16(out, (u32)opt.dpi);
	}
	const T42Colour &col = img.colour;
	if (col.space == T42_CIELAB && (opt.colour_info || col.gamut != T42Gamut())) {
		/* E.6.6: G3FAX1, offset and range of L*, a*, b* */
		static const unsigned char g3[6] = {'G', '3', 'F', 'A', 'X', 1};
		marker(out, M_APP1, 20);
		out->insert(out->end(), g3, g3 + 6);
		for (int i = 0; i < 3; i++) {
			put16(out, (u16)col.gamut.offset[i]);
			put16(out, (u16)col.gamut.range[i]);
		}
	}
	if (col.space == T42_CIELAB && (opt.colour_info || col.illuminant != T42_D50)) {
		/* E.6.7: G3FAX2, the illuminant's code */
		static const unsigned char g3[6] = {'G', '3', 'F', 'A', 'X', 2};
		marker(out, M_APP1, 12);
		out->insert(out->end(), g3, g3 + 6);
		put16(out, col.illuminant >> 16);
		put16(out, col.illuminant & 0xffff);
	}
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
	marker(out, P == 8 ? M_SOF0 : M_SOF1, 8 + 3 * nc);
	out->push_back((unsigned char)P);
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

	Sink es[2];
	for (int t = 0; t < 2; t++) {
		es[t].bo = &bo;
		es[t].dc = &dc[t];
		es[t].ac = &ac[t];
	}
	scan(es, true);
	bo.flush();
	if (opt.dnl) {
		marker(out, M_DNL, 4);
		put16(out, h);
	}
	marker(out, M_EOI, 0);
	return true;
}

/* the markers in turn, each scan's MCUs into the components' planes,
   then the planes to the image's size */
s16 jpeg_decode(const unsigned char *data, size_t len, u32 max_rows, JpegImage *img, size_t *used)
{
	u16 q[4][64];
	Huff dc[4], ac[4];
	bool have_q[4] = {false}, have_dc[4] = {false}, have_ac[4] = {false};
	Comp comps[4];
	int nf = 0, hmax = 1, vmax = 1, P = 8;
	u32 x = 0, y = 0, ri = 0, rows = 0;
	bool frame = false, scanned = false, early = false, y_open = false;
	img->width = img->height = 0;
	img->comps = 0;
	img->pix.clear();
	img->pix16.clear();
	img->dpi = 0;
	img->colour = T42Colour();

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
		if (m == M_EOI) {
			if (used)
				*used = pos;
			break;
		}
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
			/* T.4 E.6.5 to E.6.7: G3FAX (the resolution),
			   G3FAX1 (the gamut), G3FAX2 (the illuminant) */
			if (end - p < 6 || memcmp(p, "G3FAX", 5))
				break;
			if (p[5] == 0 && end - p >= 10) {
				img->dpi = (s16)(p[8] << 8 | p[9]);
			} else if (p[5] == 1 && end - p >= 18) {
				for (int i = 0; i < 3; i++) {
					img->colour.gamut.offset[i] = (s16)(p[6 + 4 * i] << 8 | p[7 + 4 * i]);
					img->colour.gamut.range[i] = (s16)(p[8 + 4 * i] << 8 | p[9 + 4 * i]);
				}
			} else if (p[5] == 2 && end - p >= 10) {
				img->colour.illuminant = (u32)p[6] << 24 | (u32)p[7] << 16 |
							 (u32)p[8] << 8 | p[9];
			}
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
			/* baseline: 8 bits; extended: 8 or 12 */
			if (frame || end - p < 6 || (p[0] != 8 && (m == M_SOF0 || p[0] != 12)))
				return -1;
			P = p[0];
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
										       q[c->tq], mx * bh + bx, my * bv + by,
										       P);
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
	if (early && used)
		*used = len;
	if (y_open && rows > y)
		rows = y;
	img->width = x;
	img->height = std::min(rows, y);
	img->comps = (s16)(nf == 3 ? 3 : 1);
	img->colour.bits = (s16)P;
	const size_t total = (size_t)img->width * img->height * img->comps;
	if (P == 8)
		img->pix.assign(total, 128);
	else
		img->pix16.assign(total, (u16)(1 << (P - 1)));
	for (int c = 0; c < nf; c++) {
		Comp &cp = comps[c];
		size_t prows = cp.pw ? cp.plane.size() / cp.pw : 0;
		for (u32 yy = 0; yy < img->height; yy++) {
			size_t sy = (size_t)yy * cp.v / vmax;
			if (sy >= prows)
				continue;
			const u16 *row = cp.plane.data() + sy * cp.pw;
			size_t o = (size_t)yy * x * img->comps + c;
			for (u32 xx = 0; xx < x; xx++, o += img->comps) {
				u16 v = row[(size_t)xx * cp.h / hmax];
				if (P == 8)
					img->pix[o] = (unsigned char)v;
				else
					img->pix16[o] = v;
			}
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

/* the named illuminants' white points, else a colour temperature's */
bool t42_white(u32 illuminant, double xyz[3])
{
	static const struct {
		u32 code;
		double x, z;
	} named[] = {
		{T42_D50, 0.96422, 0.82521},    /* T.42 6.2.1.2 */
		{T42_D65, 0.95045, 1.08892},    /* T.42 6.2.2.2 */
		{T42_D75, 0.94972, 1.22638},
		{T42_SA, 1.09850, 0.35585},
		{T42_SC, 0.98074, 1.18232},
		{T42_F2, 0.99187, 0.67395},
		{T42_F7, 0.95044, 1.08755},
		{T42_F11, 1.00966, 0.64370},
	};
	for (const auto &n : named)
		if (n.code == illuminant) {
			xyz[0] = n.x;
			xyz[1] = 1;
			xyz[2] = n.z;
			return true;
		}
	double x, y;
	if ((illuminant >> 16) != 0x4354 || !temperature_xy(illuminant & 0xffff, &x, &y))
		return false;
	xyz[0] = x / y;
	xyz[1] = 1;
	xyz[2] = (1 - x - y) / y;
	return true;
}

/* sRGB to XYZ (adapted to the white), to CIELAB; or to sR'G'B', to
   YCC; then each to its sample */
bool t42_from_rgb(const T42Colour &c, const unsigned char *rgb, u16 *out, size_t n)
{
	Conv cv;
	if (!cv.init(c))
		return false;
	for (size_t i = 0; i < n; i++, rgb += 3, out += 3) {
		double v[3], r[3];
		if (cv.ycc) {
			for (int k = 0; k < 3; k++)
				v[k] = rgb[k] / 255.0;
			apply3(cv.m, v, r);
		} else {
			for (int k = 0; k < 3; k++)
				v[k] = colour.lin[rgb[k]];
			apply3(cv.m, v, r);
			double fx = lab_f(r[0] / cv.w[0]), fy = lab_f(r[1] / cv.w[1]), fz = lab_f(r[2] / cv.w[2]);
			r[0] = 116 * fy - 16;
			r[1] = 500 * (fx - fy);
			r[2] = 200 * (fy - fz);
		}
		for (int k = 0; k < 3; k++)
			out[k] = cv.sample(k, r[k]);
	}
	return true;
}

/* each sample to its value, CIELAB to XYZ to sRGB; or YCC to sR'G'B' */
bool t42_to_rgb(const T42Colour &c, const u16 *in, unsigned char *rgb, size_t n)
{
	Conv cv;
	if (!cv.init(c))
		return false;
	for (size_t i = 0; i < n; i++, in += 3, rgb += 3) {
		double v[3], r[3];
		for (int k = 0; k < 3; k++)
			v[k] = cv.value(k, in[k]);
		if (cv.ycc) {
			apply3(cv.mi, v, r);
			for (int k = 0; k < 3; k++)
				rgb[k] = octet(r[k] * 255);
		} else {
			double fy = (v[0] + 16) / 116, fx = fy + v[1] / 500, fz = fy - v[2] / 200;
			double xyz[3] = {cv.w[0] * lab_finv(fx), cv.w[1] * lab_finv(fy), cv.w[2] * lab_finv(fz)};
			apply3(cv.mi, xyz, r);
			for (int k = 0; k < 3; k++)
				rgb[k] = srgb(r[k]);
		}
	}
	return true;
}

/* a grey level to L* (its luminance; the same for any white) or Y */
bool t42_from_grey(const T42Colour &c, const unsigned char *grey, u16 *out, size_t n)
{
	Conv cv;
	if (!cv.init(c))
		return false;
	for (size_t i = 0; i < n; i++)
		out[i] = cv.sample(0, cv.ycc ? grey[i] / 255.0 : 116 * lab_f(colour.lin[grey[i]]) - 16);
	return true;
}

/* L* or Y to a grey level */
bool t42_to_grey(const T42Colour &c, const u16 *in, unsigned char *grey, size_t n)
{
	Conv cv;
	if (!cv.init(c))
		return false;
	for (size_t i = 0; i < n; i++) {
		double v = cv.value(0, in[i]);
		grey[i] = cv.ycc ? octet(v * 255) : srgb(lab_finv((v + 16) / 116));
	}
	return true;
}

/* the samples converted through 16 bits, kept in pix (up to 8 bits) or
   pix16 */
bool t42_image_from_rgb(JpegImage *img, const unsigned char *pix)
{
	const size_t n = (size_t)img->width * img->height, nc = img->comps == 3 ? 3 : 1;
	std::vector<u16> s(n * nc);
	bool ok = nc == 3 ? t42_from_rgb(img->colour, pix, s.data(), n) :
		t42_from_grey(img->colour, pix, s.data(), n);
	if (!ok)
		return false;
	if (img->colour.bits <= 8) {
		img->pix.assign(s.begin(), s.end());
		img->pix16.clear();
	} else {
		img->pix16 = std::move(s);
		img->pix.clear();
	}
	return true;
}

/* pix or pix16 through 16 bits to sRGB or grey levels */
bool t42_image_to_rgb(const JpegImage &img, std::vector<unsigned char> *pix)
{
	const size_t n = (size_t)img.width * img.height, nc = img.comps == 3 ? 3 : 1;
	std::vector<u16> s;
	if (img.colour.bits <= 8) {
		if (img.pix.size() < n * nc)
			return false;
		s.assign(img.pix.begin(), img.pix.begin() + (long)(n * nc));
	} else {
		if (img.pix16.size() < n * nc)
			return false;
		s.assign(img.pix16.begin(), img.pix16.begin() + (long)(n * nc));
	}
	pix->resize(n * nc);
	return nc == 3 ? t42_to_rgb(img.colour, s.data(), pix->data(), n) :
		t42_to_grey(img.colour, s.data(), pix->data(), n);
}

} // namespace dsp
