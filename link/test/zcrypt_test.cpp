/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    zcrypt_test.cpp - ZyXEL's encryption (zcrypt.h): DES's known answer,
 *                      ATCK's key, PKDS, the key check, the IV and CFB
 *
 */

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "zcrypt.h"

using namespace dlink;

namespace {

/* n bytes as lower-case hex */
std::string hex(const u8 *b, size_t n)
{
	static const char d[] = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; i++) {
		s += d[b[i] >> 4];
		s += d[b[i] & 15];
	}
	return s;
}

/* a standard DES key (with its parity bits) in the firmware's packed
   form */
void pack(u64 k, u8 out[8])
{
	u64 p = 0;
	for (int i = 0; i < 8; i++)
		p = p << 7 | (k >> (57 - 8 * i) & 0x7f);
	for (int i = 0; i < 7; i++)
		out[i] = static_cast<u8>(p >> (48 - 8 * i));
	out[7] = 0;
}

} // namespace

/* FIPS 46's worked example: key 133457799BBCDFF1, 0123456789ABCDEF to
   85E813540F0AB405, and back */
TEST(Zcrypt, DesKnownAnswer)
{
	u8 key[8], out[8], back[8];
	const u8 pt[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
	pack(0x133457799bbcdff1ull, key);
	Des d;
	d.set_key(key);
	d.encrypt(pt, out);
	EXPECT_EQ(hex(out, 8), "85e813540f0ab405");
	d.decrypt(out, back);
	EXPECT_EQ(hex(back, 8), hex(pt, 8));
}

/* ATCK: the characters added to 01 23 .. EF; after the 8th and 16th
   the negations, inversions and B added to A (values from a separate
   model of firmware-trace.md 8.4) */
TEST(Zcrypt, AtckKey)
{
	u8 k[ZDES_KEY];
	zdes_atck("ZyXEL", 5, k);
	EXPECT_EQ(hex(k, 16), "5b9c9dacd5abcdef0123456789abcdef");
	zdes_atck("0123456789abcdefXY", 18, k);
	EXPECT_EQ(hex(k, 16), "60a8e2632f10d07b39a359c9ecf0cd55");
	zdes_default_key(k);
	EXPECT_EQ(hex(k, 8), "abcd0123ef456789");
}

/* PKDS: g^x as Python has it, both ends come to the same value,
   below p, and 1^x is 1 */
TEST(Zcrypt, PkdsShared)
{
	u8 xa[16], xb[16], ya[16], yb[16], ka[16], kb[16];
	for (int i = 0; i < 16; i++) {
		xa[i] = static_cast<u8>(i * 37 + 5);
		xb[i] = static_cast<u8>(0xff - i * 11);
	}
	zdes_public(xa, ya);
	zdes_public(xb, yb);
	/* (Python's pow(g, x mod p, p)) */
	EXPECT_EQ(hex(ya, 16), "83666e003c35399e78c8fa336de7e415");
	EXPECT_NE(hex(ya, 16), hex(yb, 16));
	zdes_shared(yb, xa, ka);
	zdes_shared(ya, xb, kb);
	EXPECT_EQ(hex(ka, 16), hex(kb, 16));
	EXPECT_LT(hex(ka, 16), "a07d403989fe7a691e99f8221ad64bd5");
	u8 one[16] = {}, r[16];
	one[15] = 1;
	zdes_shared(one, xa, r);
	EXPECT_EQ(hex(r, 16), "00000000000000000000000000000001");
}

/* the key check: byte 7 of each key 0; with two keys, B's last word
   went up by 1 before (as the DSP does it); a weak key of the table
   changed */
TEST(Zcrypt, KeyCheck)
{
	u8 k[16];
	for (int i = 0; i < 16; i++)
		k[i] = static_cast<u8>(0x10 + i);
	u8 one[16];
	memcpy(one, k, 16);
	zdes_key_check(one, false);
	EXPECT_EQ(hex(one, 16), "101112131415160018191a1b1c1d1e1f");
	zdes_key_check(k, true);
	EXPECT_EQ(hex(k, 16), "101112131415160018191a1b1c1d1e00");
	u8 w[16] = {};
	zdes_key_check(w, false);
	EXPECT_NE(hex(w, 8), "0000000000000000");
	EXPECT_EQ(w[7], 0);
}

/* the IV: A's first word less B's, A's second plus B's (modulo
   2^32) */
TEST(Zcrypt, Iv)
{
	const u8 k[16] = { 0, 0, 0, 1, 0xff, 0xff, 0xff, 0xff,
			   0, 0, 0, 2, 0, 0, 0, 2 };
	u8 iv[8];
	zdes_iv(k, iv);
	EXPECT_EQ(hex(iv, 8), "ffffffff00000001");
}

/* CFB: frames of all lengths (with tails below 8) come back as they
   were, one key and triple; the cipher of a frame depends on the frames
   before it */
TEST(Zcrypt, CfbFrames)
{
	u8 key[16];
	zdes_atck("secret", 6, key);
	zdes_key_check(key, true);
	u8 iv[8];
	zdes_iv(key, iv);
	for (bool triple : { false, true }) {
		ZCfb tx, rx;
		tx.init(key, triple, iv);
		rx.init(key, triple, iv);
		for (size_t n = 0; n < 40; n++) {
			std::vector<u8> p(n);
			for (size_t i = 0; i < n; i++)
				p[i] = static_cast<u8>(i * 3 + n);
			std::vector<u8> c = p;
			tx.encrypt(c.data(), c.size());
			if (n >= 8)
				EXPECT_NE(c, p);
			rx.decrypt(c.data(), c.size());
			EXPECT_EQ(c, p) << n << (triple ? " triple" : "");
		}
	}
	ZCfb a, b;
	a.init(key, false, iv);
	b.init(key, false, iv);
	u8 x[8] = {}, y[8] = {}, z[3] = {};
	b.encrypt(z, 3);
	a.encrypt(x, 8);
	b.encrypt(y, 8);
	EXPECT_NE(hex(x, 8), hex(y, 8));
}
