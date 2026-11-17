/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    zcrypt.cpp - ZyXEL's data encryption (zcrypt.h)
 *
 *    The addresses in the comments are those of the Elite 2864I 1.11
 *    image: the 68000's real addresses, and the ISDN mode DSP program's
 *    word addresses (DSP ...).
 *
 */

#include <cstring>

#include "zcrypt.h"

namespace dlink {

/*
 *    ATCK (0x188108)
 *
 */

static const u8 ATCK_START[8] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
/* register A at power-on (0x1043AA) */
static const u8 BOOT_KEY[8] = {
	0xab, 0xcd, 0x01, 0x23, 0xef, 0x45, 0x67, 0x89 };

/* A and B from 01 23 45 67 89 AB CD EF; each character added to the
   next byte (A, then B); after the 8th, the bytes of A whose bit is 1 in
   it made negative; after the 16th, the bytes of B whose bit is 1 in it
   inverted, then B added to A byte by byte, and A again */
void zdes_atck(const char *text, size_t n, u8 key[ZDES_KEY])
{
	u8 *a = key, *b = key + 8;
	memcpy(a, ATCK_START, 8);
	memcpy(b, ATCK_START, 8);
	size_t at = 0;
	for (size_t i = 0; i < n; i++) {
		const u8 c = static_cast<u8>(text[i]);
		key[at] = static_cast<u8>(key[at] + c);
		at++;
		if (at == 8) {
			for (int k = 0; k < 8; k++)
				if (c >> k & 1)
					a[k] = static_cast<u8>(-a[k]);
		} else if (at == 16) {
			for (int k = 0; k < 8; k++) {
				if (c >> k & 1)
					b[k] = static_cast<u8>(~b[k]);
				a[k] = static_cast<u8>(a[k] + b[k]);
			}
			at = 0;
		}
	}
}

/* A and B as at power-on */
void zdes_default_key(u8 key[ZDES_KEY])
{
	memcpy(key, BOOT_KEY, 8);
	memcpy(key + 8, BOOT_KEY, 8);
}


/*
 *    PKDS (DSP mode 4, 0x1603): 128-bit numbers, big-endian
 *
 */

namespace {

struct U128 {
	u64 hi = 0, lo = 0;
};

/* p (DSP 0x194E), a prime, and g (DSP 0x1956) */
const U128 P = { 0xa07d403989fe7a69ull, 0x1e99f8221ad64bd5ull };
const U128 G = { 0x9e348b1a11df8d3full, 0x9d426536df4f535bull };

/* 16 big-endian bytes as a number */
U128 load(const u8 b[16])
{
	U128 v;
	for (int i = 0; i < 8; i++) {
		v.hi = v.hi << 8 | b[i];
		v.lo = v.lo << 8 | b[8 + i];
	}
	return v;
}

/* v as 16 big-endian bytes */
void store(const U128 &v, u8 b[16])
{
	for (int i = 0; i < 8; i++) {
		b[i] = static_cast<u8>(v.hi >> (56 - 8 * i));
		b[8 + i] = static_cast<u8>(v.lo >> (56 - 8 * i));
	}
}

/* a >= b */
bool geq(const U128 &a, const U128 &b)
{
	return a.hi != b.hi ? a.hi > b.hi : a.lo >= b.lo;
}

/* a - b, for a >= b */
U128 sub(const U128 &a, const U128 &b)
{
	U128 r;
	r.lo = a.lo - b.lo;
	r.hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0);
	return r;
}

/* (a + b) mod P, for a and b below P */
U128 add_mod(const U128 &a, const U128 &b)
{
	U128 r;
	r.lo = a.lo + b.lo;
	u64 c = r.lo < a.lo ? 1 : 0;
	r.hi = a.hi + b.hi + c;
	const bool carry = r.hi < a.hi || (c && r.hi == a.hi);
	if (carry || geq(r, P))
		r = sub(r, P);
	return r;
}

/* a * b mod P: double and add from b's top bit */
U128 mul_mod(const U128 &a, const U128 &b)
{
	U128 r;
	for (int i = 127; i >= 0; i--) {
		r = add_mod(r, r);
		const u64 w = i >= 64 ? b.hi : b.lo;
		if (w >> (i & 63) & 1)
			r = add_mod(r, a);
	}
	return r;
}

/* base^e mod P: square and multiply from e's lowest bit (as DSP
   0x176B) */
U128 pow_mod(U128 base, const U128 &e)
{
	U128 r;
	r.lo = 1;
	if (geq(base, P))
		base = sub(base, P);
	for (int i = 0; i < 128; i++) {
		const u64 w = i >= 64 ? e.hi : e.lo;
		if (w >> (i & 63) & 1)
			r = mul_mod(r, base);
		base = mul_mod(base, base);
	}
	return r;
}

/* the secret x as an exponent: below P, not 0 */
U128 exponent(const u8 x[ZDES_PKDS])
{
	U128 e = load(x);
	while (geq(e, P))
		e = sub(e, P);
	if (!e.hi && !e.lo)
		e.lo = 1;
	return e;
}

} // namespace

/* y = g^x mod p */
void zdes_public(const u8 x[ZDES_PKDS], u8 y[ZDES_PKDS])
{
	store(pow_mod(G, exponent(x)), y);
}

/* K = y_remote^x mod p */
void zdes_shared(const u8 y_remote[ZDES_PKDS], const u8 x[ZDES_PKDS],
		 u8 k[ZDES_PKDS])
{
	store(pow_mod(load(y_remote), exponent(x)), k);
}


/*
 *    the DSP's key check (DSP 0x16D1 to 0x1736)
 *
 */

/* DSP 0x195E: 64 weak, semi-weak and possibly weak keys, packed (byte 7
   0).  Three are not DES's: entries 2, 3 and 42 (1E1E.., E0E0..,
   FE801EE0FE000EF0 as standard keys); the firmware's table is copied
   as it is */
static const u16 WEAK[64][4] = {
	{ 0x0000, 0x0000, 0x0000, 0x0000 }, { 0xffff, 0xffff, 0xffff, 0xff00 },
	{ 0x1e3c, 0x78f1, 0xe3c7, 0x8f00 }, { 0xe1c3, 0x870e, 0x1c38, 0x7000 },
	{ 0x01fc, 0x07f0, 0x1fc0, 0x7f00 }, { 0x1fc0, 0x7f00, 0xfe03, 0xf800 },
	{ 0x01c0, 0x0700, 0x1e00, 0x7800 }, { 0x1ffc, 0x7ff0, 0xffc3, 0xff00 },
	{ 0x003c, 0x00f0, 0x01c0, 0x0700 }, { 0xe1ff, 0x87ff, 0x1ffc, 0x7f00 },
	{ 0xfe03, 0xf80f, 0xe03f, 0x8000 }, { 0xe03f, 0x80ff, 0x01fc, 0x0700 },
	{ 0xe003, 0x800f, 0x003c, 0x0000 }, { 0xfe3f, 0xf8ff, 0xe1ff, 0x8700 },
	{ 0x1e00, 0x7800, 0xe003, 0x8000 }, { 0xffc3, 0xff0f, 0xfe3f, 0xf800 },
	{ 0x1e3c, 0x0000, 0xe1c0, 0x0000 }, { 0x003c, 0x7800, 0x01c3, 0x8000 },
	{ 0x1e00, 0x00f0, 0xe000, 0x0700 }, { 0x0000, 0x78f0, 0x0003, 0x8700 },
	{ 0xe1c0, 0x000f, 0x1e00, 0x0000 }, { 0xfffc, 0x000f, 0xffc0, 0x0000 },
	{ 0xffc0, 0x780f, 0xfe03, 0x8000 }, { 0xe1fc, 0x780f, 0x1fc3, 0x8000 },
	{ 0xffc0, 0x00ff, 0xfe00, 0x0700 }, { 0xe1fc, 0x00ff, 0x1fc0, 0x0700 },
	{ 0xe1c0, 0x78ff, 0x1e03, 0x8700 }, { 0xfffc, 0x78ff, 0xffc3, 0x8700 },
	{ 0xfe3f, 0x800f, 0xe1fc, 0x0000 }, { 0xe03f, 0xf80f, 0x01ff, 0x8000 },
	{ 0xfe03, 0x80ff, 0xe03c, 0x0700 }, { 0xe003, 0xf8ff, 0x003f, 0x8700 },
	{ 0x01c3, 0x8000, 0x1e3c, 0x0000 }, { 0x1fff, 0x8000, 0xfffc, 0x0000 },
	{ 0x1fc3, 0xf800, 0xfe3f, 0x8000 }, { 0x01ff, 0xf800, 0x1fff, 0x8000 },
	{ 0x1fc3, 0x80f0, 0xfe3c, 0x0700 }, { 0x01ff, 0x80f0, 0x1ffc, 0x0700 },
	{ 0x01c3, 0xf8f0, 0x1e3f, 0x8700 }, { 0x1fff, 0xf8f0, 0xffff, 0x8700 },
	{ 0xe000, 0x070f, 0x0000, 0x7800 }, { 0xfe3c, 0x070f, 0xe1c0, 0x7800 },
	{ 0xff00, 0x7f0f, 0xe003, 0xf800 }, { 0xe03c, 0x7f0f, 0x01c3, 0xf800 },
	{ 0xfe00, 0x07ff, 0xe000, 0x7f00 }, { 0xe03c, 0x07ff, 0x01c0, 0x7f00 },
	{ 0xe000, 0x7fff, 0x0003, 0xff00 }, { 0xfe3c, 0x7fff, 0xe1c3, 0xff00 },
	{ 0x1ffc, 0x0700, 0xffc0, 0x7800 }, { 0x01fc, 0x7f00, 0x1fc3, 0xf800 },
	{ 0x1fc0, 0x07f0, 0xfe00, 0x7f00 }, { 0x01c0, 0x7ff0, 0x1e03, 0xff00 },
	{ 0x0003, 0x8700, 0x003c, 0x7800 }, { 0x1e3f, 0x8700, 0xe1fc, 0x7800 },
	{ 0x1e03, 0xff00, 0xe03f, 0xf800 }, { 0x003f, 0xff00, 0x01ff, 0xf800 },
	{ 0x1e03, 0x87f0, 0xe03c, 0x7f00 }, { 0x003f, 0x87f0, 0x01fc, 0x7f00 },
	{ 0x0003, 0xfff0, 0x003f, 0xff00 }, { 0x1e3f, 0xfff0, 0xe1ff, 0xff00 },
	{ 0xffff, 0x870f, 0xfffc, 0x7800 }, { 0xe1ff, 0xff0f, 0x1fff, 0xf800 },
	{ 0xffc3, 0x87ff, 0xfe3c, 0x7f00 }, { 0xe1c3, 0xffff, 0x1e3f, 0xff00 },
};

/* DSP 0x191C on the 4 words at w: first the word 7 after w goes up by
   1 (as the DSP does it), then w (its last byte not compared) against
   the table */
static bool weak(u16 *w, u16 *after)
{
	if (after)
		*after = static_cast<u16>(*after + 1);
	for (const auto &e : WEAK)
		if (w[0] == e[0] && w[1] == e[1] && w[2] == e[2] &&
		    (w[3] & 0xff00) == e[3])
			return true;
	return false;
}

/* DSP 0x1740: 128 steps of a 128-bit LFSR (a rotation right by one;
   when the bit that went round is 1, 0x4000 into the first word and
   0x21 into the last), then the first word less 0xA07D when it is not
   below it */
static void lfsr(u16 w[8])
{
	for (int step = 0; step < 128; step++) {
		u16 c = w[7] & 1;
		for (int i = 0; i < 8; i++) {
			const u16 out = w[i] & 1;
			w[i] = static_cast<u16>(w[i] >> 1 | c << 15);
			c = out;
		}
		if (w[0] & 0x8000) {
			w[0] ^= 0x4000;
			w[7] ^= 0x21;
		}
	}
	if (w[0] >= 0xa07d)
		w[0] = static_cast<u16>(w[0] - 0xa07d);
}

/* the check until no key is weak, then byte 7 of each key 0 */
void zdes_key_check(u8 key[ZDES_KEY], bool two)
{
	/* one key: the DSP's words after it are 0, 0, 0, 1 (DSP 0x1730),
	   not B */
	u16 w[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
	for (int i = 0; i < (two ? 8 : 4); i++)
		w[i] = static_cast<u16>(key[2 * i] << 8 | key[2 * i + 1]);
	/* (the second check's word 7 after is outside the key) */
	auto check = [&]() {
		bool bad = weak(w, &w[7]);
		if (two && weak(w + 4, nullptr))
			bad = true;
		return bad;
	};
	while (check())
		lfsr(w);
	w[3] &= 0xff00;
	if (two)
		w[7] &= 0xff00;
	for (int i = 0; i < (two ? 8 : 4); i++) {
		key[2 * i] = static_cast<u8>(w[i] >> 8);
		key[2 * i + 1] = static_cast<u8>(w[i]);
	}
}

/* IV = (A - B, A + B) in 32-bit words */
void zdes_iv(const u8 key[ZDES_KEY], u8 iv[8])
{
	auto be32 = [](const u8 *p) {
		return static_cast<u32>(p[0]) << 24 | p[1] << 16 | p[2] << 8 |
			p[3];
	};
	const u32 hi = be32(key) - be32(key + 8);
	const u32 lo = be32(key + 4) + be32(key + 12);
	for (int i = 0; i < 4; i++) {
		iv[i] = static_cast<u8>(hi >> (24 - 8 * i));
		iv[4 + i] = static_cast<u8>(lo >> (24 - 8 * i));
	}
}


/*
 *    DES
 *
 */

namespace {

const u8 IP[64] = {
	58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
	62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
	57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
const u8 FP[64] = {
	40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
	38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
	36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
	34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
const u8 E[48] = {
	32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13,
	12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
	24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
const u8 PERM[32] = {
	16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
	2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
const u8 PC1[56] = {
	57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
	10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
	63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
	14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
const u8 PC2[48] = {
	14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
	23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
	41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
const u8 SHIFTS[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
const u8 SBOX[8][64] = {
	{ 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
	  0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
	  4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
	  15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
	{ 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
	  3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
	  0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
	  13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
	{ 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
	  13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
	  13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
	  1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
	{ 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
	  13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
	  10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
	  3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
	{ 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
	  14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
	  4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
	  11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
	{ 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
	  10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
	  9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
	  4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
	{ 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
	  13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
	  1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
	  6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
	{ 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
	  1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
	  7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
	  2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 },
};

/* the bits of v (width in, bit 1 its top) in the order of table t */
u64 permute(u64 v, int in, const u8 *t, int out)
{
	u64 r = 0;
	for (int i = 0; i < out; i++)
		r = r << 1 | (v >> (in - t[i]) & 1);
	return r;
}

/* 8 big-endian bytes as a number */
u64 load64(const u8 b[8])
{
	u64 v = 0;
	for (int i = 0; i < 8; i++)
		v = v << 8 | b[i];
	return v;
}

} // namespace

/* the packed key: 7 bits from bytes 0 to 6 to each standard key byte's
   top 7 bits (its parity bit 0, which DES does not use) */
void Des::set_key(const u8 key[8])
{
	u64 packed = 0;
	for (int i = 0; i < 7; i++)
		packed = packed << 8 | key[i];
	u64 k = 0;
	for (int i = 0; i < 8; i++)
		k = k << 8 | (packed >> (49 - 7 * i) & 0x7f) << 1;
	u64 cd = permute(k, 64, PC1, 56);
	u32 c = static_cast<u32>(cd >> 28), d = static_cast<u32>(cd & 0xfffffff);
	for (int r = 0; r < 16; r++) {
		for (int s = 0; s < SHIFTS[r]; s++) {
			c = (c << 1 | c >> 27) & 0xfffffff;
			d = (d << 1 | d >> 27) & 0xfffffff;
		}
		sub_[r] = permute(static_cast<u64>(c) << 28 | d, 56, PC2, 48);
	}
}

/* one block: the 16 rounds, the subkeys in reverse to decrypt */
void Des::crypt(const u8 in[8], u8 out[8], bool dec) const
{
	u64 v = permute(load64(in), 64, IP, 64);
	u32 l = static_cast<u32>(v >> 32), r = static_cast<u32>(v);
	for (int i = 0; i < 16; i++) {
		u64 x = permute(r, 32, E, 48) ^ sub_[dec ? 15 - i : i];
		u32 s = 0;
		for (int b = 0; b < 8; b++) {
			const u8 six = static_cast<u8>(x >> (42 - 6 * b) & 0x3f);
			const u8 row = static_cast<u8>((six >> 4 & 2) | (six & 1));
			const u8 col = static_cast<u8>(six >> 1 & 0xf);
			s = s << 4 | SBOX[b][row * 16 + col];
		}
		const u32 f = static_cast<u32>(permute(s, 32, PERM, 32));
		const u32 t = r;
		r = l ^ f;
		l = t;
	}
	v = permute(static_cast<u64>(r) << 32 | l, 64, FP, 64);
	for (int i = 0; i < 8; i++)
		out[i] = static_cast<u8>(v >> (56 - 8 * i));
}

/* one block encrypted */
void Des::encrypt(const u8 in[8], u8 out[8]) const
{
	crypt(in, out, false);
}

/* one block decrypted */
void Des::decrypt(const u8 in[8], u8 out[8]) const
{
	crypt(in, out, true);
}


/*
 *    64-bit CFB (0x1A9BCC transmit, 0x1A9D6C receive)
 *
 */

/* the keys (A, and B for triple DES) and the register at the IV */
void ZCfb::init(const u8 key[ZDES_KEY], bool triple, const u8 iv[8])
{
	k1_.set_key(key);
	k2_.set_key(key + 8);
	triple_ = triple;
	memcpy(reg_, iv, 8);
}

/* the keystream block: E(A), or E(A) D(B) E(A) */
void ZCfb::block(const u8 in[8], u8 out[8]) const
{
	k1_.encrypt(in, out);
	if (!triple_)
		return;
	u8 t[8];
	k2_.decrypt(out, t);
	k1_.encrypt(t, out);
}

/* n octets at p through the cipher, 8 at a time; the register takes
   the cipher octets */
void ZCfb::run(u8 *p, size_t n, bool dec)
{
	while (n) {
		u8 ks[8];
		block(reg_, ks);
		const size_t m = n < 8 ? n : 8;
		u8 c[8];
		for (size_t i = 0; i < m; i++) {
			const u8 in = p[i];
			p[i] = static_cast<u8>(in ^ ks[i]);
			c[i] = dec ? in : p[i];
		}
		/* the register: m octets to the left, the cipher after */
		memmove(reg_, reg_ + m, 8 - m);
		memcpy(reg_ + 8 - m, c, m);
		p += m;
		n -= m;
	}
}

/* n octets at p encrypted in place */
void ZCfb::encrypt(u8 *p, size_t n)
{
	run(p, n, false);
}

/* n octets at p decrypted in place */
void ZCfb::decrypt(u8 *p, size_t n)
{
	run(p, n, true);
}

} // namespace dlink
