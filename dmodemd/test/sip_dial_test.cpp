/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sip_dial_test.cpp - the SIP user dmodemd calls for a dial string
 *                        (sip::dial_user()), the caller of a call that
 *                        rings (sip::caller_of())
 *
 */

#include <gtest/gtest.h>

#include "sip.h"

/* a number as dialled: its digits (and DTMF's * # A-D, and +) only,
   the modifiers and the formatting gone */
TEST(DialUser, Numbers)
{
	EXPECT_EQ(sip::dial_user("5551000"), "5551000");
	EXPECT_EQ(sip::dial_user("T5551000"), "5551000");
	EXPECT_EQ(sip::dial_user("P5551000"), "5551000");
	EXPECT_EQ(sip::dial_user("t555-1000"), "5551000");
	EXPECT_EQ(sip::dial_user("(303) 494-4774"), "3034944774");
	EXPECT_EQ(sip::dial_user("9,W5551000"), "95551000");
	EXPECT_EQ(sip::dial_user("T9,,T555.1000;"), "95551000");
	EXPECT_EQ(sip::dial_user("+15551000"), "+15551000");
	EXPECT_EQ(sip::dial_user("*67#123"), "*67#123");
	EXPECT_EQ(sip::dial_user("1234A"), "1234A");
	EXPECT_EQ(sip::dial_user(""), "");
	EXPECT_EQ(sip::dial_user(nullptr), "");
}

/* anything but a number is a user's name, as it is */
TEST(DialUser, Names)
{
	EXPECT_EQ(sip::dial_user("alice"), "alice");
	EXPECT_EQ(sip::dial_user("Tom"), "Tom");
	EXPECT_EQ(sip::dial_user("john.doe"), "john.doe");
	EXPECT_EQ(sip::dial_user("bbs-1"), "bbs-1");
}

/* a caller's number (the URI's user part, a tel: URI's number, without
   parameters or escapes) and name (the display name without quotes) */
TEST(CallerOf, NumberAndName)
{
	sip::Caller c = sip::caller_of("sip:+15551234@example.com",
				       "\"Alice Smith\"", "");
	EXPECT_EQ(c.number, "+15551234");
	EXPECT_EQ(c.name, "Alice Smith");
	EXPECT_FALSE(c.number_withheld);
	EXPECT_FALSE(c.name_withheld);

	c = sip::caller_of("<sip:5551000;isub=1@gw.example.com;user=phone>",
			   "Bob", "");
	EXPECT_EQ(c.number, "5551000");
	EXPECT_EQ(c.name, "Bob");
	c = sip::caller_of("tel:+15551000;phone-context=example.com", "", "");
	EXPECT_EQ(c.number, "+15551000");
	EXPECT_EQ(c.name, "");
	c = sip::caller_of("sip:john%20doe@example.com",
			   "\"Say \\\"hi\\\"\tthere\"", "none");
	EXPECT_EQ(c.number, "johndoe");
	EXPECT_EQ(c.name, "Say \"hi\" there");
	c = sip::caller_of("sip:example.com", "", "");
	EXPECT_EQ(c.number, "");
	EXPECT_FALSE(c.number_withheld);
}

/* withheld: the Privacy header's id, user or header, an anonymous URI
   (RFC 3323), or an anonymous display name (the name alone) */
TEST(CallerOf, Withheld)
{
	sip::Caller c = sip::caller_of("sip:+15551234@example.com", "Alice",
				       "id");
	EXPECT_TRUE(c.number_withheld);
	EXPECT_TRUE(c.name_withheld);
	EXPECT_EQ(c.number, "");
	EXPECT_EQ(c.name, "");
	c = sip::caller_of("sip:anonymous@anonymous.invalid", "Anonymous", "");
	EXPECT_TRUE(c.number_withheld);
	EXPECT_TRUE(c.name_withheld);
	c = sip::caller_of("sip:x@anonymous.invalid", "", "");
	EXPECT_TRUE(c.number_withheld);
	c = sip::caller_of("sip:5551000@example.com", "Anonymous", "");
	EXPECT_FALSE(c.number_withheld);
	EXPECT_EQ(c.number, "5551000");
	EXPECT_TRUE(c.name_withheld);
	c = sip::caller_of("sip:5551000@example.com", "Bob", "header;critical");
	EXPECT_TRUE(c.number_withheld);
}
