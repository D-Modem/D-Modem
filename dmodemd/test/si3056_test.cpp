/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    si3056_test.cpp - a country's Si3056 line settings (si3056.h)
 *
 */

#include <gtest/gtest.h>

#include "si3056.h"

using dmd::Si3056Regs;
using dmd::si3056_apply;
using dmd::si3056_country;

/* the names: D-Modem's, case and '_' ignored, the aliases (TBR21's
   CTR21EUROPE, Bahrain's two spellings, Korea's two); none for a country
   the datasheet has no row for */
TEST(Si3056, Names)
{
	ASSERT_NE(si3056_country("UK"), nullptr);
	EXPECT_EQ(si3056_country("uk"), si3056_country("UK"));
	EXPECT_EQ(si3056_country("NEW_ZEALAND"), si3056_country("New Zealand"));
	EXPECT_NE(si3056_country("CTR21EUROPE"), nullptr);
	EXPECT_NE(si3056_country("BAHREIN"), nullptr);
	EXPECT_NE(si3056_country("BAHRAIN"), nullptr);
	EXPECT_NE(si3056_country("KOREA"), nullptr);
	EXPECT_NE(si3056_country("SOUTHKOREA"), nullptr);
	EXPECT_EQ(si3056_country("TUNISIA"), nullptr);
	EXPECT_EQ(si3056_country(""), nullptr);
	EXPECT_EQ(si3056_country(nullptr), nullptr);
	EXPECT_EQ(&dmd::si3056_usa(), si3056_country("USA"));
}

/* the registers from their reset values (31's FOH 01, 59 unknown: all
   ones) on a Si3018 line side, Table 15's rows: the USA's are the reset
   values; the UK's TBR21 current limit, 3 ms on-hook speed and its own
   complex termination (ACT2); Australia's Japan-mode DC termination,
   spark quenching and complex termination; New Zealand's; South Africa's
   synthesized ringer impedance */
TEST(Si3056, Si3018)
{
	auto regs = [](const char *country) {
		Si3056Regs r = { 0x00, 0x00, 0x00, 0x20, 0xff };
		si3056_apply(*si3056_country(country), false, r);
		return r;
	};
	Si3056Regs r = regs("USA");
	EXPECT_EQ(r.r16, 0x00);
	EXPECT_EQ(r.r26, 0xc0);
	EXPECT_EQ(r.r30, 0x00);         /* (a Si3019's alone: left) */
	EXPECT_EQ(r.r31, 0x20);
	EXPECT_EQ(r.r59, 0x06);         /* RG1, GCE kept; reserved, SQ 0 */
	r = regs("UK");
	EXPECT_EQ(r.r16, 0x80);
	EXPECT_EQ(r.r26, 0xc2);
	EXPECT_EQ(r.r31, 0x28);
	r = regs("AUSTRALIA");
	EXPECT_EQ(r.r16, 0xc0);
	EXPECT_EQ(r.r26, 0x50);
	EXPECT_EQ(r.r31, 0x20);
	EXPECT_EQ(r.r59, 0x56);
	r = regs("NEW_ZEALAND");
	EXPECT_EQ(r.r16, 0xa0);
	r = regs("SOUTHAFRICA");
	EXPECT_EQ(r.r16, 0x22);
	r = regs("CTR21EUROPE");
	EXPECT_EQ(r.r16, 0x80);
	EXPECT_EQ(r.r26, 0xc2);
	EXPECT_EQ(r.r31, 0x20);         /* TBR21's row: OHS2 0 */
}

/* on a Si3019 line side the AC termination is ACIM (register 30, FULL2
   kept), ACT and ACT2 0; the bits that are not the country's kept: IIRE
   (16), the reserved 3:2 of 26 (DCR cleared), FULL, FOH, FILT, LVFD
   (31) */
TEST(Si3056, Si3019)
{
	Si3056Regs r = { 0xff, 0xff, 0xff, 0xff, 0xff };
	si3056_apply(*si3056_country("UK"), true, r);
	EXPECT_EQ(r.r16, 0x10);
	EXPECT_EQ(r.r26, 0xce);
	EXPECT_EQ(r.r30, 0x15);
	EXPECT_EQ(r.r31, 0xff);
	EXPECT_EQ(r.r59, 0x06);
	r = { 0, 0, 0, 0, 0 };
	si3056_apply(*si3056_country("NEW_ZEALAND"), true, r);
	EXPECT_EQ(r.r16, 0x00);
	EXPECT_EQ(r.r30, 0x04);
	r = { 0, 0, 0, 0, 0 };
	si3056_apply(*si3056_country("BULGARIA"), true, r);
	EXPECT_EQ(r.r30, 0x03);
	EXPECT_EQ(r.r31, 0x08);
}
