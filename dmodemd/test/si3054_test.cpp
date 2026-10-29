/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    si3054_test.cpp - a country's Si3054 line settings (si3054.h)
 *
 */

#include <gtest/gtest.h>

#include "si3054.h"

using dmd::Si3054Regs;
using dmd::si3054_apply;
using dmd::si3054_country;

/* the names (D-Modem's, case and '_' ignored, the aliases), none for a
   country the datasheet has no row for; the chip IDs it is for */
TEST(Si3054, NamesAndChip)
{
	EXPECT_NE(si3054_country("UK"), nullptr);
	EXPECT_EQ(si3054_country("south africa"), si3054_country("SOUTHAFRICA"));
	EXPECT_NE(si3054_country("CTR21EUROPE"), nullptr);
	EXPECT_NE(si3054_country("BAHREIN"), nullptr);
	EXPECT_NE(si3054_country("KOREA"), nullptr);
	EXPECT_EQ(si3054_country("TUNISIA"), nullptr);
	EXPECT_EQ(si3054_country(nullptr), nullptr);
	EXPECT_EQ(&dmd::si3054_usa(), si3054_country("USA"));
	EXPECT_TRUE(dmd::si3054_chip_ok(0x1155));       /* Si3054, Si3018 */
	EXPECT_TRUE(dmd::si3054_chip_ok(0x1100));
	EXPECT_FALSE(dmd::si3054_chip_ok(0x1007));      /* line side off */
	EXPECT_FALSE(dmd::si3054_chip_ok(0x1207));      /* another one */
	EXPECT_FALSE(dmd::si3054_chip_ok(0x0155));      /* no SID */
}

/* Table 19's rows from the kernel's values (13 0x200, 15 0x6000, 16
   0x400, 18 0): the USA's change nothing; TBR21's countries ACT, ILIM,
   OHS2 (TBR21's own row not OHS2); Egypt's RZ, cleared off hook;
   Australia's OHS and spark quenching, Japan's DC termination; New
   Zealand's and China's complex termination */
TEST(Si3054, Rows)
{
	auto regs = [](const char *country, bool off = false) {
		Si3054Regs r = { 0x0200, 0x6000, 0x0400, 0x0000 };
		si3054_apply(*si3054_country(country), off, r);
		return r;
	};
	Si3054Regs r = regs("USA");
	EXPECT_EQ(r.n13, 0x0200);
	EXPECT_EQ(r.n15, 0x6300);
	EXPECT_EQ(r.n16, 0x0400);
	EXPECT_EQ(r.n18, 0x0000);
	r = regs("GERMANY");
	EXPECT_EQ(r.n13, 0x0220);
	EXPECT_EQ(r.n15, 0x6302);
	EXPECT_EQ(r.n16, 0x0600);
	r = regs("CTR21EUROPE");
	EXPECT_EQ(r.n16, 0x0400);
	EXPECT_EQ(r.n15, 0x6302);
	r = regs("EGYPT");
	EXPECT_EQ(r.n13, 0x0224);
	EXPECT_EQ(r.n16, 0x0400);
	EXPECT_EQ(regs("EGYPT", true).n13, 0x0220);
	r = regs("AUSTRALIA");
	EXPECT_EQ(r.n13, 0x02a0);
	EXPECT_EQ(r.n15, 0x6110);
	EXPECT_EQ(r.n18, 0x5000);
	EXPECT_EQ(regs("NEW_ZEALAND").n13, 0x0220);
	EXPECT_EQ(regs("CHINA").n13, 0x0220);
	EXPECT_EQ(regs("BAHRAIN").n13, 0x0200);
}

/* the bits that are not the country's: kept (IIRE, RFWE, BTE; 15's
   reserved bits; CALD, FOH; RG1 and 18's bit 15), or cleared (ACT2,
   DCR, the calibration commands CALZ and MCAL, OVL, 13's and 16's
   reserved bits) */
TEST(Si3054, OtherBits)
{
	Si3054Regs r = { 0xffff, 0xffff, 0xffff, 0xffff };
	si3054_apply(*si3054_country("USA"), false, r);
	EXPECT_EQ(r.n13, 0x0a40);
	EXPECT_EQ(r.n15, 0xffcc);
	EXPECT_EQ(r.n16, 0x2c00);
	EXPECT_EQ(r.n18, 0x8400);
}
