/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    rtp_red_test.cpp - RFC 2198's redundant audio (rtp_red.h)
 *
 */

#include <gtest/gtest.h>

#include <vector>

#include "rtp_red.h"

namespace red = dmd::red;

namespace {

/* a frame of 160 octets, each its number's */
std::vector<uint8_t> frame(uint8_t k)
{
	return std::vector<uint8_t>(160, k);
}

} // namespace

/* RFC 2198's own example (section 3): a redundant block of 5 (offset
   256, 4 octets), the primary of 0 */
TEST(RtpRed, Rfc2198Layout)
{
	uint8_t r[4] = { 1, 2, 3, 4 }, p[3] = { 9, 9, 9 };
	red::Block b[1] = { { 5, 1000 - 256, r, 4 } };
	red::Block pri = { 0, 1000, p, 3 };
	uint8_t out[64];

	size_t n = red::encode(b, 1, pri, out, sizeof(out));
	ASSERT_EQ(n, 4u + 1 + 4 + 3);
	/* F=1, pt 5; offset 256 (14 bits), length 4 (10 bits); F=0 pt 0 */
	EXPECT_EQ(out[0], 0x85);
	EXPECT_EQ(out[1], 256 >> 6);
	EXPECT_EQ(out[2], (256 & 0x3f) << 2);
	EXPECT_EQ(out[3], 4);
	EXPECT_EQ(out[4], 0);
	EXPECT_EQ(out[5], 1);
	EXPECT_EQ(out[9], 9);
}

/* two frames again and the primary, back as they went */
TEST(RtpRed, RoundTrip)
{
	auto f1 = frame(1), f2 = frame(2), f3 = frame(3);
	red::Block b[2] = { { 0, 1000, f1.data(), 160 },
			    { 0, 1160, f2.data(), 160 } };
	red::Block pri = { 0, 1320, f3.data(), 160 };
	uint8_t out[600];
	std::vector<red::Block> v;

	size_t n = red::encode(b, 2, pri, out, sizeof(out));
	ASSERT_EQ(n, 4u * 2 + 1 + 3 * 160);
	ASSERT_TRUE(red::decode(out, n, 1320, &v));
	ASSERT_EQ(v.size(), 3u);
	EXPECT_EQ(v[0].ts, 1000u);
	EXPECT_EQ(v[1].ts, 1160u);
	EXPECT_EQ(v[2].ts, 1320u);
	for (int i = 0; i < 3; i++) {
		EXPECT_EQ(v[i].pt, 0);
		ASSERT_EQ(v[i].n, 160u);
		EXPECT_EQ(v[i].data[0], i + 1);
		EXPECT_EQ(v[i].data[159], i + 1);
	}
}

/* the timestamp's wrap: an offset back across it */
TEST(RtpRed, Wrap)
{
	auto f1 = frame(1), f2 = frame(2);
	red::Block b[1] = { { 8, 0xffffff60u, f1.data(), 160 } };
	red::Block pri = { 8, 0, f2.data(), 160 };
	uint8_t out[400];
	std::vector<red::Block> v;

	size_t n = red::encode(b, 1, pri, out, sizeof(out));
	ASSERT_TRUE(red::decode(out, n, 0, &v));
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0].ts, 0xffffff60u);
	EXPECT_EQ(v[0].pt, 8);
}

/* blocks a header can't have are left out: an offset of 0 or past 14
   bits */
TEST(RtpRed, LeftOut)
{
	auto f = frame(7);
	red::Block b[3] = { { 0, 100000 - 20000, f.data(), 160 },
			    { 0, 100000, f.data(), 160 },
			    { 0, 100000 - 160, f.data(), 160 } };
	red::Block pri = { 0, 100000, f.data(), 160 };
	uint8_t out[1000];
	std::vector<red::Block> v;

	size_t n = red::encode(b, 3, pri, out, sizeof(out));
	ASSERT_EQ(n, 4u + 1 + 2 * 160);
	ASSERT_TRUE(red::decode(out, n, 100000, &v));
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0].ts, 100000u - 160);
}

/* nothing again: the primary alone, its header one octet */
TEST(RtpRed, PrimaryAlone)
{
	auto f = frame(4);
	red::Block pri = { 0, 5, f.data(), 160 };
	uint8_t out[200];
	std::vector<red::Block> v;

	size_t n = red::encode(nullptr, 0, pri, out, sizeof(out));
	ASSERT_EQ(n, 161u);
	EXPECT_EQ(out[0], 0);
	ASSERT_TRUE(red::decode(out, n, 5, &v));
	ASSERT_EQ(v.size(), 1u);
	EXPECT_EQ(v[0].n, 160u);
}

/* too little room: 0 */
TEST(RtpRed, NoRoom)
{
	auto f = frame(4);
	red::Block b[1] = { { 0, 0, f.data(), 160 } };
	red::Block pri = { 0, 160, f.data(), 160 };
	uint8_t out[300];

	EXPECT_EQ(red::encode(b, 1, pri, out, sizeof(out)), 0u);
}

/* malformed payloads: headers past the end, lengths past it */
TEST(RtpRed, Malformed)
{
	std::vector<red::Block> v;
	uint8_t none[1] = { 0x80 };
	uint8_t short_hdr[3] = { 0x80, 0, 0 };
	/* a block of 200 octets in a payload of 10 */
	uint8_t long_len[10] = { 0x80, 0, 0 | 0, 200, 0 };

	EXPECT_FALSE(red::decode(none, 0, 0, &v));
	EXPECT_FALSE(red::decode(none, 1, 0, &v));
	EXPECT_FALSE(red::decode(short_hdr, 3, 0, &v));
	EXPECT_FALSE(red::decode(long_len, sizeof(long_len), 0, &v));
	EXPECT_TRUE(v.empty());
}

/* a=fmtp values read, and ours */
TEST(RtpRed, Fmtp)
{
	EXPECT_EQ(red::fmtp_types("0/0/0"),
		  (std::vector<int16_t>{ 0, 0, 0 }));
	EXPECT_EQ(red::fmtp_types(" 8/8 "), (std::vector<int16_t>{ 8, 8 }));
	EXPECT_EQ(red::fmtp_types("97"), (std::vector<int16_t>{ 97 }));
	EXPECT_TRUE(red::fmtp_types("").empty());
	EXPECT_TRUE(red::fmtp_types("0//0").empty());
	EXPECT_TRUE(red::fmtp_types("0/200").empty());
	EXPECT_TRUE(red::fmtp_types("0/0;x").empty());
	EXPECT_EQ(red::fmtp(0, 2), "0/0/0");
	EXPECT_EQ(red::fmtp(8, 0), "8");
}
