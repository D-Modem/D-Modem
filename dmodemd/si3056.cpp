/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    si3056.cpp - a country's Si3056 line settings (si3056.h)
 *
 *    Table 15 of the Si3056/18/19/10 datasheet (Rev. 1.03), row for row,
 *    under the names D-Modem's countries have (core/homolog_data.cpp and
 *    the ITU list, tones.cpp).  The datasheet's TBR21 row is
 *    CTR21EUROPE, its South Korea row KOREA's too (T.35's 0x61, the
 *    Republic of Korea) as well as SOUTHKOREA's; China's ACIM is the
 *    data applications' (0000, not
 *    voice's 1010).  Countries D-Modem has and the datasheet has not
 *    are left out (the caller keeps the USA's for them), as are the
 *    datasheet's rows for countries D-Modem has not (Guam, Kazakhstan).
 *
 */

#include "si3056.h"

#include <ctype.h>

namespace dmd {

namespace {

/*                 OHS OHS2 RZ RT ILIM DCV MINI ACIM ACT ACT2 */
#define FCC     0, 0, 0, 0, 0, 3, 0, 0x0, 0, 0
#define TBR21   0, 1, 0, 0, 1, 3, 0, 0x2, 0, 1     /* the countries' (OHS2) */
#define JAPAN   0, 0, 0, 0, 0, 1, 1, 0x0, 0, 0

const Si3056Country countries[] = {
	{ "ARGENTINA", FCC },
	{ "AUSTRALIA", 1, 0, 0, 0, 0, 1, 1, 0x3, 0, 1 },
	{ "AUSTRIA", TBR21 },
	{ "BAHRAIN", TBR21 },
	{ "BAHREIN", TBR21 },
	{ "BELGIUM", TBR21 },
	{ "BRAZIL", FCC },
	{ "BULGARIA", 0, 1, 0, 0, 1, 3, 0, 0x3, 0, 1 },
	{ "CANADA", FCC },
	{ "CHILE", FCC },
	{ "CHINA", FCC },
	{ "COLOMBIA", FCC },
	{ "CROATIA", TBR21 },
	{ "CTR21EUROPE", 0, 0, 0, 0, 1, 3, 0, 0x2, 0, 1 },   /* TBR21 */
	{ "CYPRUS", TBR21 },
	{ "CZECH_REPUBLIC", TBR21 },
	{ "DENMARK", TBR21 },
	{ "ECUADOR", FCC },
	{ "EGYPT", TBR21 },
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
	{ "KOREA", 0, 0, 1, 0, 0, 3, 0, 0x0, 0, 0 },
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
	{ "NEW_ZEALAND", 0, 0, 0, 0, 0, 3, 0, 0x4, 1, 1 },
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
	{ "SOUTHAFRICA", 0, 0, 1, 0, 0, 3, 0, 0x3, 1, 0 },
	{ "SOUTHKOREA", 0, 0, 1, 0, 0, 3, 0, 0x0, 0, 0 },
	{ "SPAIN", TBR21 },
	{ "SWEDEN", TBR21 },
	{ "SWITZERLAND", TBR21 },
	{ "TAIWAN", FCC },
	{ "THAILAND", JAPAN },
	{ "UAE", FCC },
	{ "UK", 0, 1, 0, 0, 1, 3, 0, 0x5, 0, 1 },
	{ "USA", FCC },
	{ "YEMEN", FCC },
};

/* a and b the same name, case and '_' (or ' ') ignored */
bool same(const char *a, const char *b)
{
	for (;;) {
		while (*a == '_' || *a == ' ')
			a++;
		while (*b == '_' || *b == ' ')
			b++;
		if (!*a || !*b)
			return !*a && !*b;
		if (toupper((unsigned char)*a++) != toupper((unsigned char)*b++))
			return false;
	}
}

} // namespace

/* see si3056.h */
const Si3056Country *si3056_country(const char *name)
{
	if (!name)
		return nullptr;
	for (const Si3056Country &c : countries)
		if (same(c.name, name))
			return &c;
	return nullptr;
}

/* see si3056.h */
const Si3056Country &si3056_usa()
{
	return *si3056_country("USA");
}

/* see si3056.h: register 16 ACT2 (7), OHS (6), ACT (5), RZ (1), RT (0),
   IIRE (4) kept; 26 DCV (7:6), MINI (5:4), ILIM (1), DCR (0) 0 (the
   50 ohm DC termination, "for all standard applications"), the reserved
   bits 3:2 kept; 30 ACIM (3:0), FULL2 (4) kept; 31 OHS2 (3);
   59 SQ1 (6) and SQ0 (4) both set with OHS, RG1 (2) and GCE (1) kept,
   the reserved bits written as zeros */
void si3056_apply(const Si3056Country &c, bool si3019, Si3056Regs &r)
{
	uint8_t act = si3019 ? 0 : c.act, act2 = si3019 ? 0 : c.act2;

	r.r16 = (uint8_t)((r.r16 & 0x10) | act2 << 7 | c.ohs << 6 | act << 5 |
			  c.rz << 1 | c.rt);
	r.r26 = (uint8_t)((r.r26 & 0x0c) | c.dcv << 6 | c.mini << 4 |
			  c.ilim << 1);
	if (si3019)
		r.r30 = (uint8_t)((r.r30 & 0x10) | c.acim);
	r.r31 = (uint8_t)((r.r31 & ~0x08) | c.ohs2 << 3);
	r.r59 = (uint8_t)((r.r59 & 0x06) | (c.ohs ? 0x50 : 0));
}

} // namespace dmd
