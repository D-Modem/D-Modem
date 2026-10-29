/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sip_dial_test.cpp - the SIP user dmodemd calls for a dial string
 *                        (sip::dial_user())
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
