/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    bits_test.cpp - the frame helpers of bits.h: fields, the V.34 CRC
 *                    and MP style frames' CRC, the HDLC FCS
 *
 */

#include <gtest/gtest.h>

#include <string.h>

#include "bits.h"

using namespace dsp;

/* the check values of the two CRC-16s over "123456789": X.25's FCS
   (0x906E after its inversion), and CCITT-FALSE (V.34's, its bits most
   significant first) 0x29B1 */
TEST(Bits, CrcCheckValues)
{
	static const char s[] = "123456789";
	unsigned char bits[72];
	int n = 0;

	EXPECT_EQ(hdlc_fcs((const unsigned char *)s, 9) ^ 0xFFFF, 0x906Eu);
	for (int i = 0; i < 9; i++)
		put_bits_msb(bits, &n, (unsigned char)s[i], 8);
	EXPECT_EQ(v34_crc_bits(bits, 72, 0xFFFF), 0x29B1u);
}

/* a frame and its FCS (low octet first) give HDLC_FCS_GOOD */
TEST(Bits, HdlcFcsOverFrame)
{
	unsigned char f[8] = { 0xFF, 0x13, 0x80, 0x00, 0xEE };
	unsigned fcs = hdlc_fcs(f, 5) ^ 0xFFFF;
	f[5] = fcs & 0xFF;
	f[6] = fcs >> 8;
	EXPECT_EQ(hdlc_fcs(f, 7), (unsigned)HDLC_FCS_GOOD);
	f[2] ^= 4;
	EXPECT_NE(hdlc_fcs(f, 7), (unsigned)HDLC_FCS_GOOD);
}

/* fields back as put, least significant bit first; msb first for the
   CRCs */
TEST(Bits, Fields)
{
	unsigned char b[64];
	int n = 0;
	put_bits(b, &n, 0x2D, 7);
	put_bits_msb(b, &n, 0xB, 4);
	EXPECT_EQ(n, 11);
	EXPECT_EQ(b[0], 1);
	EXPECT_EQ(field(b, 0, 7), 0x2D);
	EXPECT_EQ(b[7], 1);
	EXPECT_EQ(b[8], 0);
	EXPECT_TRUE(bits_msb_are(b, 7, 0xB, 4));
	EXPECT_FALSE(bits_msb_are(b, 7, 0xA, 4));
}

/* an MP style frame: sync, two fields with their start bits, its CRC;
   checked as it is, and not with a bit of a field changed */
TEST(Bits, FrameCrc)
{
	unsigned char b[128];
	int n = 0, at;
	put_bits(b, &n, 0x1FFFF, 17);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0xBEEF, 16);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0x1234, 16);
	put_bits(b, &n, 0, 1);
	at = n;
	put_frame_crc(b, &n);
	EXPECT_EQ(n, at + 16);
	EXPECT_TRUE(frame_crc_ok(b, at));
	b[40] ^= 1;
	EXPECT_FALSE(frame_crc_ok(b, at));
	b[40] ^= 1;
	/* the start bits are not covered */
	b[34] ^= 1;
	EXPECT_TRUE(frame_crc_ok(b, at));
}
