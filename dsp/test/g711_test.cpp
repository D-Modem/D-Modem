/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    g711_test.cpp - g711.h: every octet back to itself through its law,
 *                    the reference values, and encode_sm()'s sign and
 *                    magnitude (V.92's ANSpcm, the fax tests' line)
 *
 */

#include <gtest/gtest.h>

#include "g711.h"

using namespace dsp;

/* each octet decodes to a value that codes to it again (mu-law's 0x7F,
   negative zero, codes as 0xFF) */
TEST(G711, OctetsRoundTrip)
{
	for (int alaw = 0; alaw < 2; alaw++)
		for (int c = 0; c < 256; c++) {
			if (!alaw && c == 0x7F)
				continue;
			EXPECT_EQ(g711::encode(alaw, g711::decode(alaw, c)), c)
				<< (alaw ? "A-law " : "mu-law ") << c;
		}
	EXPECT_EQ(g711::encode(0, g711::decode(0, 0x7F)), 0xFF);
}

/* the ends and the zeros of each law */
TEST(G711, ReferenceValues)
{
	EXPECT_EQ(g711::ulaw2lin(0xFF), 0);
	EXPECT_EQ(g711::ulaw2lin(0x80), 32124);
	EXPECT_EQ(g711::ulaw2lin(0x00), -32124);
	EXPECT_EQ(g711::alaw2lin(0xD5), 8);
	EXPECT_EQ(g711::alaw2lin(0xAA), 32256);
	EXPECT_EQ(g711::alaw2lin(0x2A), -32256);
	EXPECT_EQ(g711::lin2ulaw(0), 0xFF);
	EXPECT_EQ(g711::lin2ulaw(32767), 0x80);
	EXPECT_EQ(g711::lin2alaw(0), 0xD5);
	EXPECT_EQ(g711::lin2alaw(32767), 0xAA);
}

/* encode_sm(): a value and its negative differ only in the sign bit,
   and decode to values of opposite sign; the reference encoders agree
   with it on positive values and on negative mu-law values that are
   whole 14 bit steps */
TEST(G711, SignAndMagnitude)
{
	for (int alaw = 0; alaw < 2; alaw++)
		for (int x = 1; x <= 32767; x++) {
			unsigned char p = g711::encode_sm(alaw, x),
				n = g711::encode_sm(alaw, -x);
			ASSERT_EQ(p ^ n, 0x80) << alaw << " " << x;
			ASSERT_EQ(g711::decode(alaw, n), -g711::decode(alaw, p));
			ASSERT_EQ(p, g711::encode(alaw, x));
			if (!alaw && x % 4 == 0) {
				ASSERT_EQ(n, g711::encode(0, -x));
			}
		}
	/* the reference rounds a negative value down: -1 is mu-law's
	   first step below 0, but 0 by its magnitude */
	EXPECT_EQ(g711::decode(0, g711::encode(0, -1)), -8);
	EXPECT_EQ(g711::decode(0, g711::encode_sm(0, -1)), 0);
}
