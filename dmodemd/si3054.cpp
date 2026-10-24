/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    si3054.cpp - a country's Si3054 line settings (si3054.h)
 *
 *    Table 19 of the Si3054/Si3018 datasheet (Rev. 1.0), its HD audio
 *    columns (OHS, RZ, RT, ACT, OHS2, DCV, MINI, ILIM; DCT and VOL are
 *    AC'97 mode's alone), row for row, under the names D-Modem's
 *    countries have (core/homolog_data.cpp and the ITU list,
 *    tones.cpp).  The datasheet's TBR21 row is CTR21EUROPE, its South
 *    Korea row KOREA's too (T.35's 0x61, the Republic of Korea) as well
 *    as SOUTHKOREA's.  Where the table allows ACT 0 or 1 (the TBR21
 *    countries, Egypt) it is 1, the global complex impedance, the
 *    complex AC termination TBR21 asks for.  Countries D-Modem has and the
 *    datasheet has not are left out (the caller keeps the USA's for
 *    them), as are the datasheet's rows for countries D-Modem has not
 *    (Guam, Kazakhstan).
 *
 */

#include "si3054.h"

#include "daa_name.h"

namespace dmd {

namespace {

/*                 OHS RZ RT ACT OHS2 DCV MINI ILIM */
#define FCC     0, 0, 0, 0, 0, 3, 0, 0
#define TBR21   0, 0, 0, 1, 1, 3, 0, 1      /* the countries' (OHS2) */
#define JAPAN   0, 0, 0, 0, 0, 1, 1, 0

const Si3054Country countries[] = {
	{ "ARGENTINA", FCC },
	{ "AUSTRALIA", 1, 0, 0, 1, 0, 1, 1, 0 },
	{ "AUSTRIA", TBR21 },
	{ "BAHRAIN", 0, 0, 0, 0, 1, 3, 0, 1 },
	{ "BAHREIN", 0, 0, 0, 0, 1, 3, 0, 1 },
	{ "BELGIUM", TBR21 },
	{ "BRAZIL", FCC },
	{ "BULGARIA", TBR21 },
	{ "CANADA", FCC },
	{ "CHILE", FCC },
	{ "CHINA", 0, 0, 0, 1, 0, 3, 0, 0 },
	{ "COLOMBIA", FCC },
	{ "CROATIA", TBR21 },
	{ "CTR21EUROPE", 0, 0, 0, 1, 0, 3, 0, 1 },     /* TBR21 */
	{ "CYPRUS", TBR21 },
	{ "CZECH_REPUBLIC", TBR21 },
	{ "DENMARK", TBR21 },
	{ "ECUADOR", FCC },
	{ "EGYPT", 0, 1, 0, 1, 0, 3, 0, 1 },
	{ "EL_SALVADOR", FCC },
	{ "FINLAND", TBR21 },
	{ "FRANCE", TBR21 },
	{ "GERMANY", TBR21 },
	{ "GREECE", TBR21 },
	{ "HONG_KONG", FCC },
	{ "HUNGARY", TBR21 },
	{ "ICELAND", TBR21 },
	{ "INDIA", FCC },
	{ "INDONESIA", FCC },
	{ "IRELAND", TBR21 },
	{ "ISRAEL", TBR21 },
	{ "ITALY", TBR21 },
	{ "JAPAN", JAPAN },
	{ "JORDAN", JAPAN },
	{ "KOREA", 0, 1, 0, 0, 0, 3, 0, 0 },
	{ "KUWAIT", FCC },
	{ "LATVIA", TBR21 },
	{ "LEBANON", TBR21 },
	{ "LUXEMBOURG", TBR21 },
	{ "MACAO_CHINA", FCC },
	{ "MALAYSIA", JAPAN },
	{ "MALTA", TBR21 },
	{ "MEXICO", FCC },
	{ "MOROCCO", TBR21 },
	{ "NETHERLANDS", TBR21 },
	{ "NEW_ZEALAND", 0, 0, 0, 1, 0, 3, 0, 0 },
	{ "NIGERIA", TBR21 },
	{ "NORWAY", TBR21 },
	{ "OMAN", JAPAN },
	{ "PAKISTAN", JAPAN },
	{ "PERU", FCC },
	{ "PHILIPPINES", JAPAN },
	{ "POLAND", TBR21 },
	{ "PORTUGAL", TBR21 },
	{ "ROMANIA", TBR21 },
	{ "RUSSIA", FCC },
	{ "SAUDIARABIA", FCC },
	{ "SINGAPORE", FCC },
	{ "SLOVAKIA", TBR21 },
	{ "SLOVENIA", TBR21 },
	{ "SOUTHAFRICA", 0, 1, 0, 0, 0, 3, 0, 0 },
	{ "SOUTHKOREA", 0, 1, 0, 0, 0, 3, 0, 0 },
	{ "SPAIN", TBR21 },
	{ "SWEDEN", TBR21 },
	{ "SWITZERLAND", TBR21 },
	{ "TAIWAN", FCC },
	{ "THAILAND", JAPAN },
	{ "UAE", FCC },
	{ "UK", TBR21 },
	{ "USA", FCC },
	{ "YEMEN", FCC },
};

} // namespace

/* see si3054.h */
const Si3054Country *si3054_country(const char *name)
{
	if (!name)
		return nullptr;
	for (const Si3054Country &c : countries)
		if (daa_same_name(c.name, name))
			return &c;
	return nullptr;
}

/* see si3054.h */
const Si3054Country &si3054_usa()
{
	return *si3054_country("USA");
}

/* see si3054.h: node 12's SID (bit 12) and LSID[3:0] (11:8) */
bool si3054_chip_ok(uint16_t chipid)
{
	return (chipid & 0x1000) && ((chipid >> 8) & 0xf) == 1;
}

/* see si3054.h: node 13 ACT2 (8) 0, OHS (7), ACT (5), RZ (2), RT (0),
   IIRE (11), RFWE (9) and BTE (6) kept, the reserved bits 0; 15 DCV
   (9:8), MINI (5:4), ILIM (1), DCR (0) 0 (the 50 ohm DC termination,
   "for all standard applications"), the reserved bits (they read zero
   or one) kept; 16 OHS2 (9), CALD (13) and FOH (11:10) kept, CALZ and
   MCAL (15, 14: calibration commands) and the rest 0; 18 SQ1 (14) and
   SQ0 (12) both set with OHS, RG1 (10) and bit 15 (reads zero or one)
   kept */
void si3054_apply(const Si3054Country &c, bool off_hook, Si3054Regs &r)
{
	uint16_t rz = off_hook ? 0 : c.rz;

	r.n13 = static_cast<uint16_t>((r.n13 & 0x0a40) | c.ohs << 7 |
				      c.act << 5 | rz << 2 | c.rt);
	r.n15 = static_cast<uint16_t>((r.n15 & 0xfccc) | c.dcv << 8 |
				      c.mini << 4 | c.ilim << 1);
	r.n16 = static_cast<uint16_t>((r.n16 & 0x2c00) | c.ohs2 << 9);
	r.n18 = static_cast<uint16_t>((r.n18 & 0x8400) | (c.ohs ? 0x5000 : 0));
}

} // namespace dmd
