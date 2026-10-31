/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sms_pdu_test.cpp - fixed-line SMS's TPDUs (sms_pdu.cpp): SMS-SUBMIT
 *                       and SMS-DELIVER encoded and decoded, against
 *                       known encodings
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "sms_pdu.h"

using namespace dsp;

/* bytes v in upper case hex */
static std::string hex(const std::vector<u8> &v)
{
	std::string s;
	char b[3];
	for (u8 c : v) {
		snprintf(b, sizeof(b), "%02X", c);
		s += b;
	}
	return s;
}

/* the DTE's lines for the TPDU in hex h */
static std::string dte(const char *h)
{
	std::vector<u8> v;
	EXPECT_TRUE(sms::from_hex(&v, h)) << h;
	return sms::to_dte(v.data(), static_cast<s32>(v.size()));
}

/* the 7-bit alphabet's classic: "hellohello" in 9 octets */
TEST(SmsPdu, SubmitSevenBit)
{
	std::vector<u8> v;
	ASSERT_TRUE(sms::submit(&v, "+12345", "hellohello", 0));
	EXPECT_EQ(hex(v), "010005912143F500000AE8329BFD4697D9EC37");
	EXPECT_EQ(sms::to_dte(v.data(), static_cast<s32>(v.size())),
		  "+SUBMIT: \"+12345\"\r\nhellohello\r\n");
}

/* the escape table: the euro sign is two septets */
TEST(SmsPdu, SubmitEscape)
{
	std::vector<u8> v;
	ASSERT_TRUE(sms::submit(&v, "12", "\xE2\x82\xAC", 7));
	EXPECT_EQ(hex(v), "01070281210000029B32");
	EXPECT_EQ(sms::to_dte(v.data(), static_cast<s32>(v.size())),
		  "+SUBMIT: \"12\"\r\n\xE2\x82\xAC\r\n");
}

/* a character the 7-bit alphabet has not: all in UCS2, one beyond the
   BMP a surrogate pair */
TEST(SmsPdu, SubmitUcs2)
{
	std::vector<u8> v;
	ASSERT_TRUE(sms::submit(&v, "12", "\xD0\x96\xF0\x9F\x98\x80", 0));
	EXPECT_EQ(hex(v), "01000281210008060416D83DDE00");
	EXPECT_EQ(sms::to_dte(v.data(), static_cast<s32>(v.size())),
		  "+SUBMIT: \"12\"\r\n\xD0\x96\xF0\x9F\x98\x80\r\n");
}

/* 160 septets go, 161 do not; 70 UCS2 characters go, 71 do not; no
   number or no text, nothing */
TEST(SmsPdu, Limits)
{
	std::vector<u8> v;
	EXPECT_TRUE(sms::submit(&v, "1", std::string(160, 'a'), 0));
	EXPECT_FALSE(sms::submit(&v, "1", std::string(161, 'a'), 0));
	std::string zhe;
	for (int i = 0; i < 70; i++)
		zhe += "\xD0\x96";
	EXPECT_TRUE(sms::submit(&v, "1", zhe, 0));
	EXPECT_FALSE(sms::submit(&v, "1", zhe + "\xD0\x96", 0));
	EXPECT_FALSE(sms::submit(&v, "", "x", 0));
	EXPECT_FALSE(sms::submit(&v, "1", "", 0));
	EXPECT_FALSE(sms::submit(&v, "ABC", "x", 0));
}

/* a delivery as a telephone sees it: the number, the time stamp (its
   zone in quarter hours), the text */
TEST(SmsPdu, DeliverDecoded)
{
	EXPECT_EQ(dte("040B911346610089F60000208062917314800CC8F71D1496"
		      "9741F977FD07"),
		  "+CMT: \"+31641600986\",,\"02/08/26,19:37:41+08\"\r\n"
		  "How are you?\r\n");
}

/* a delivery made and read back: a zone west of UTC, a name for the
   number */
TEST(SmsPdu, DeliverRoundTrip)
{
	std::vector<u8> v;
	sms::Scts t = { 26, 10, 31, 9, 5, 7, -20 };
	ASSERT_TRUE(sms::deliver(&v, "D-Modem", "Hi {all}", t));
	EXPECT_EQ(sms::to_dte(v.data(), static_cast<s32>(v.size())),
		  "+CMT: \"D-Modem\",,\"26/10/31,09:05:07-20\"\r\nHi {all}\r\n");
	ASSERT_TRUE(sms::deliver(&v, "+4420", "x", t));
	EXPECT_EQ(hex(v).substr(0, 10), "0404914402");
}

/* a user data header (a concatenated message's) is skipped, in septets
   with its fill bit and in octets */
TEST(SmsPdu, HeaderSkipped)
{
	EXPECT_EQ(dte("4100028121000009050003010201" "9069"),
		  "+SUBMIT: \"12\"\r\nHi\r\n");
	EXPECT_EQ(dte("41000281210008080500030102010416"),
		  "+SUBMIT: \"12\"\r\n\xD0\x96\r\n");
}

/* 8-bit data in hex; a validity period skipped; anything else, or a
   TPDU that runs short, as it is */
TEST(SmsPdu, Others)
{
	EXPECT_EQ(dte("010002812100040301FF80"),
		  "+SUBMIT: \"12\"\r\n01FF80\r\n");
	EXPECT_EQ(dte("11000281210000A70461F1980C"),
		  "+SUBMIT: \"12\"\r\nabcd\r\n");
	EXPECT_EQ(dte("0200"), "+PDU: 0200\r\n");
	EXPECT_EQ(dte("0100028121000005E8"), "+PDU: 0100028121000005E8\r\n");
	std::vector<u8> v;
	EXPECT_FALSE(sms::from_hex(&v, "123"));
	EXPECT_FALSE(sms::from_hex(&v, "XY"));
}
