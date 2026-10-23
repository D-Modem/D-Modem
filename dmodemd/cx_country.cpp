/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    cx_country.cpp - a country's SmartDAA line setting and the board's
 *                     parameters for a Conexant AC'97 codec (cx_country.h)
 *
 *    The countries are hsf.cty's (Conexant's country file for its HSF
 *    modems, version 7.72.00, dsp/test/hsf/), all 242 of its regions
 *    under its names (BAHREIN, SOUTHKOREA and UAE, D-Modem's names, as
 *    BAHRAIN, KOREA and UNITED_ARAB_EMIRATES too).  Each region's
 *    SMART_RELAYS (its own or its REFERENCE profile's) is twelve words,
 *    the first 0xffff, then one per line state; bits 7:6 of the low
 *    byte of the first (off hook) and the eighth (on hook) are what LSD
 *    register 0x12's bits 6:5 become, the only line setting the country
 *    changes when Conexant's library runs in the HSF harness (the
 *    countries of each of the file's 14 SMART_RELAYS tables tried).
 *    The rows have three values: 11 on hook and 10 off hook (US: the
 *    USA, Japan, ...), 10 and 10 (ITU, most), 00 and 00 (TBR21, its
 *    countries).  New Zealand and South Africa have 11 in two other
 *    states (dialling ones, it seems), not taken here.
 *
 *    The boards are hsfmc97ich.inf's SmartDAAParams and
 *    SmartDAAParamsK3 (95 bytes each; the ICH driver hands the engine's
 *    copies of them to the library).
 *
 */

#include "cx_country.h"

#include "daa_name.h"

namespace dmd {

namespace {

/* LSD register 0x12's bits 6:5 on hook, off hook */
#define US      3, 2
#define ITU     2, 2
#define TBR21   0, 0

const CxCountry countries[] = {
	{ "AFGHANISTAN", ITU },
	{ "ALBANIA", ITU },
	{ "ALGERIA", ITU },
	{ "AMERICAN_SAMOA", ITU },
	{ "ANDORRA", ITU },
	{ "ANGOLA", ITU },
	{ "ANGUILLA", ITU },
	{ "ANTIGUA_AND_BARBUDA", ITU },
	{ "ARGENTINA", ITU },
	{ "ARMENIA", ITU },
	{ "ARUBA", ITU },
	{ "ASCENSION_ISLAND", ITU },
	{ "AUSTRALIA", ITU },
	{ "AUSTRIA", TBR21 },
	{ "AZERBAIJAN", ITU },
	{ "BAHAMAS", ITU },
	{ "BAHRAIN", ITU },
	{ "BAHREIN", ITU },
	{ "BANGLADESH", ITU },
	{ "BARBADOS", ITU },
	{ "BELARUS", ITU },
	{ "BELGIUM", TBR21 },
	{ "BELIZE", ITU },
	{ "BENIN", ITU },
	{ "BERMUDA", ITU },
	{ "BHUTAN", ITU },
	{ "BOLIVIA", ITU },
	{ "BOSNIA_AND_HERZEGOVINA", TBR21 },
	{ "BOTSWANA", ITU },
	{ "BRAZIL", ITU },
	{ "BRITISH_VIRGIN_ISLANDS", ITU },
	{ "BRUNEI", ITU },
	{ "BULGARIA", TBR21 },
	{ "BURKINA_FASO", ITU },
	{ "BURUNDI", ITU },
	{ "CAMBODIA", ITU },
	{ "CAMEROON", ITU },
	{ "CANADA", ITU },
	{ "CAPE_VERDE", ITU },
	{ "CAYMAN_ISLANDS", ITU },
	{ "CENTRAL_AFRICAN_REPUBLIC", ITU },
	{ "CHAD", ITU },
	{ "CHILE", ITU },
	{ "CHINA", ITU },
	{ "CHRISTMAS_ISLAND", ITU },
	{ "COCOS_KEELING_ISLANDS", ITU },
	{ "COLOMBIA", ITU },
	{ "COMOROS", ITU },
	{ "CONGO_DEMOCRATIC_REPUBLIC_OF_THE", ITU },
	{ "CONGO_REPUBLIC_OF_THE", ITU },
	{ "COOK_ISLANDS", ITU },
	{ "COSTA_RICA", ITU },
	{ "COTE_DIVOIRE", ITU },
	{ "CROATIA", TBR21 },
	{ "CUBA", ITU },
	{ "CYPRUS", TBR21 },
	{ "CZECH_REPUBLIC", TBR21 },
	{ "DEMOCRATIC_PEOPLES_REPUBLIC_OF_KOREA", ITU },
	{ "DENMARK", TBR21 },
	{ "DIEGO_GARCIA", ITU },
	{ "DJIBOUTI", ITU },
	{ "DOMINICA", ITU },
	{ "DOMINICAN_REPUBLIC", ITU },
	{ "EAST_TIMOR", ITU },
	{ "ECUADOR", ITU },
	{ "EGYPT", ITU },
	{ "EL_SALVADOR", US },
	{ "EQUATORIAL_GUINEA", ITU },
	{ "ERITREA", ITU },
	{ "ESTONIA", TBR21 },
	{ "ETHIOPIA", ITU },
	{ "FALKLAND_ISLANDS", ITU },
	{ "FAROE_ISLANDS", ITU },
	{ "FIJI", ITU },
	{ "FINLAND", TBR21 },
	{ "FRANCE", TBR21 },
	{ "FRENCH_ANTILLES", ITU },
	{ "FRENCH_GUIANA", ITU },
	{ "FRENCH_POLYNESIA", TBR21 },
	{ "GABON", ITU },
	{ "GAMBIA", ITU },
	{ "GEORGIA", ITU },
	{ "GERMANY", TBR21 },
	{ "GHANA", ITU },
	{ "GIBRALTAR", ITU },
	{ "GREECE", TBR21 },
	{ "GREENLAND", ITU },
	{ "GRENADA", ITU },
	{ "GUADELOUPE", ITU },
	{ "GUAM", ITU },
	{ "GUANTANAMO_BAY", ITU },
	{ "GUATEMALA", ITU },
	{ "GUINEA", ITU },
	{ "GUINEABISSAU", ITU },
	{ "GUYANA", ITU },
	{ "HAITI", ITU },
	{ "HONDURAS", ITU },
	{ "HONG_KONG", ITU },
	{ "HUNGARY", TBR21 },
	{ "ICELAND", TBR21 },
	{ "INDIA", US },
	{ "INDONESIA", ITU },
	{ "INMARSAT", ITU },
	{ "INMARSAT_ATLANTIC_EAST", ITU },
	{ "INMARSAT_ATLANTIC_WEST", ITU },
	{ "INMARSAT_INDIAN", ITU },
	{ "INMARSAT_PACIFIC", ITU },
	{ "INTERNATIONAL_FREEPHONE_SERVICE", ITU },
	{ "IRAN", ITU },
	{ "IRAQ", ITU },
	{ "IRELAND", TBR21 },
	{ "ISRAEL", ITU },
	{ "ITALY", TBR21 },
	{ "JAMAICA", ITU },
	{ "JAPAN", US },
	{ "JORDAN", ITU },
	{ "KAZAKHSTAN", ITU },
	{ "KENYA", ITU },
	{ "KIRIBATI", ITU },
	{ "KOREA", ITU },
	{ "KUWAIT", ITU },
	{ "KYRGYZSTAN", ITU },
	{ "LAOS", ITU },
	{ "LATVIA", TBR21 },
	{ "LEBANON", ITU },
	{ "LESOTHO", ITU },
	{ "LIBERIA", ITU },
	{ "LIBYA", ITU },
	{ "LIECHTENSTEIN", TBR21 },
	{ "LITHUANIA", TBR21 },
	{ "LUXEMBOURG", TBR21 },
	{ "MACAO", ITU },
	{ "MACEDONIA_FORMER_YUGOSLAV_REPUBLIC_OF", ITU },
	{ "MADAGASCAR", ITU },
	{ "MALAWI", ITU },
	{ "MALAYSIA", US },
	{ "MALDIVES", ITU },
	{ "MALI", ITU },
	{ "MALTA", TBR21 },
	{ "MARSHALL_ISLANDS", ITU },
	{ "MARTINIQUE", ITU },
	{ "MAURITANIA", ITU },
	{ "MAURITIUS", ITU },
	{ "MAYOTTE", ITU },
	{ "MEXICO", ITU },
	{ "MICRONESIA_FEDERATED_STATES_OF", ITU },
	{ "MOLDOVA", TBR21 },
	{ "MONACO", ITU },
	{ "MONGOLIA", ITU },
	{ "MONTENEGRO", ITU },
	{ "MONTSERRAT", ITU },
	{ "MOROCCO", ITU },
	{ "MOZAMBIQUE", ITU },
	{ "MYANMAR", ITU },
	{ "NAMIBIA", ITU },
	{ "NAURU", ITU },
	{ "NEPAL", ITU },
	{ "NETHERLANDS", TBR21 },
	{ "NETHERLANDS_ANTILLES", TBR21 },
	{ "NEW_CALEDONIA", ITU },
	{ "NEW_ZEALAND", ITU },
	{ "NICARAGUA", ITU },
	{ "NIGER", ITU },
	{ "NIGERIA", ITU },
	{ "NIUE", ITU },
	{ "NORFOLK_ISLAND", ITU },
	{ "NORWAY", TBR21 },
	{ "OMAN", ITU },
	{ "PAKISTAN", ITU },
	{ "PALAU", ITU },
	{ "PANAMA", ITU },
	{ "PAPUA_NEW_GUINEA", ITU },
	{ "PARAGUAY", ITU },
	{ "PERU", ITU },
	{ "PHILIPPINES", ITU },
	{ "POLAND", ITU },
	{ "PORTUGAL", TBR21 },
	{ "PUERTO_RICO", ITU },
	{ "QATAR", ITU },
	{ "REUNION_ISLAND", ITU },
	{ "ROMANIA", TBR21 },
	{ "ROTA_ISLAND", ITU },
	{ "RUSSIA", ITU },
	{ "RWANDA", ITU },
	{ "SAINT_HELENA_AND_ASCENSION", ITU },
	{ "SAINT_KITTS_AND_NEVIS", ITU },
	{ "SAINT_LUCIA", ITU },
	{ "SAINT_PIERRE_AND_MIQUELON", ITU },
	{ "SAINT_VINCENT_AND_THE_GRENADINES", ITU },
	{ "SAIPAN_ISLAND", ITU },
	{ "SAMOA", ITU },
	{ "SAN_MARINO", ITU },
	{ "SAO_TOME_AND_PRINCIPE", ITU },
	{ "SAUDI_ARABIA", ITU },
	{ "SENEGAL", ITU },
	{ "SERBIA", ITU },
	{ "SEYCHELLES", ITU },
	{ "SIERRA_LEONE", ITU },
	{ "SINGAPORE", ITU },
	{ "SLOVAKIA", ITU },
	{ "SLOVENIA", ITU },
	{ "SOLOMON_ISLANDS", ITU },
	{ "SOMALIA", ITU },
	{ "SOUTHKOREA", ITU },
	{ "SOUTH_AFRICA", ITU },
	{ "SPAIN", TBR21 },
	{ "SRI_LANKA", ITU },
	{ "SUDAN", ITU },
	{ "SURINAME", ITU },
	{ "SWAZILAND", ITU },
	{ "SWEDEN", TBR21 },
	{ "SWITZERLAND", TBR21 },
	{ "SYRIA", ITU },
	{ "TAIWAN", US },
	{ "TAJIKISTAN", ITU },
	{ "TANZANIA", ITU },
	{ "THAILAND", ITU },
	{ "TINIAN_ISLAND", ITU },
	{ "TOGO", ITU },
	{ "TOKELAU", ITU },
	{ "TONGA", ITU },
	{ "TRINIDAD_AND_TOBAGO", ITU },
	{ "TUNISIA", ITU },
	{ "TURKEY", TBR21 },
	{ "TURKMENISTAN", ITU },
	{ "TURKS_AND_CAICOS_ISLANDS", ITU },
	{ "TUVALU", ITU },
	{ "UAE", TBR21 },
	{ "UGANDA", ITU },
	{ "UK", TBR21 },
	{ "UKRAINE", ITU },
	{ "UNITED_ARAB_EMIRATES", TBR21 },
	{ "URUGUAY", ITU },
	{ "USA", US },
	{ "US_VIRGIN_ISLAND", ITU },
	{ "UZBEKISTAN", ITU },
	{ "VANUATU", ITU },
	{ "VATICAN_CITY", ITU },
	{ "VENEZUELA", ITU },
	{ "VIETNAM", ITU },
	{ "WALLIS_AND_FUTUNA", ITU },
	{ "YEMEN", ITU },
	{ "ZAMBIA", ITU },
	{ "ZIMBABWE", ITU },
};

#undef US
#undef ITU
#undef TBR21

const CxBoard boards[2] = {
	{ "SmartDAAParams", {
		0x90, 0x1a, 0x00, 0x00, 0x39, 0x03, 0x00, 0x00,
		0x18, 0x00, 0x00, 0x00, 0x32, 0x02, 0x00, 0x00,
		0x4a, 0x01, 0x00, 0x00, 0x96, 0x00, 0x00, 0x00,
		0x4a, 0x01, 0x00, 0x00, 0xd0, 0x07, 0x00, 0x00,
		0x03, 0x0c, 0x03, 0x03, 0x0a, 0x0a, 0x14, 0x1d,
		0x1e, 0x0a, 0x0e, 0x13, 0x1e, 0x20, 0x0d, 0x0e,
		0x12, 0x18, 0x50, 0x0a, 0x0a, 0x0c, 0x20, 0x24,
		0x0a, 0x0b, 0x15, 0x1d, 0x1e, 0x00, 0x15, 0x26,
		0x35, 0x96, 0x00, 0x14, 0x28, 0x3b, 0x96, 0x00,
		0x15, 0x24, 0x2e, 0x36, 0x00, 0x17, 0x22, 0x43,
		0x78, 0x00, 0x14, 0x26, 0x33, 0x96, 0x28, 0x28,
		0x28, 0xb8, 0x28, 0x04, 0x00, 0x3f, 0x05,
	} },
	{ "SmartDAAParamsK3", {
		0x90, 0x1a, 0x00, 0x00, 0x39, 0x03, 0x00, 0x00,
		0x18, 0x00, 0x00, 0x00, 0x32, 0x02, 0x00, 0x00,
		0x4a, 0x01, 0x00, 0x00, 0x96, 0x00, 0x00, 0x00,
		0x4a, 0x01, 0x00, 0x00, 0xd0, 0x07, 0x00, 0x00,
		0x03, 0x0c, 0x03, 0x03, 0x0a, 0x0a, 0x14, 0x1d,
		0x1e, 0x0a, 0x0e, 0x13, 0x1e, 0x20, 0x0d, 0x0e,
		0x12, 0x18, 0x50, 0x0a, 0x0a, 0x0c, 0x20, 0x24,
		0x0a, 0x0b, 0x15, 0x1d, 0x1e, 0x00, 0x15, 0x26,
		0x35, 0x96, 0x00, 0x14, 0x28, 0x3b, 0x96, 0x00,
		0x15, 0x24, 0x2e, 0x36, 0x00, 0x17, 0x22, 0x43,
		0x78, 0x00, 0x14, 0x26, 0x33, 0x96, 0x28, 0x28,
		0x28, 0xb8, 0x28, 0x04, 0x00, 0x5f, 0x0c,
	} },
};

} // namespace

/* see cx_country.h */
const CxCountry *cx_country(const char *name)
{
	if (!name)
		return nullptr;
	for (const CxCountry &c : countries)
		if (daa_same_name(c.name, name))
			return &c;
	return nullptr;
}

/* see cx_country.h */
const CxCountry &cx_usa()
{
	return *cx_country("USA");
}

/* see cx_country.h: hsfmc97ich.o's choice (hsfmc97ich152_), types 0x22
   to 0x24, 0x29, 0x30 and 0x31 K3's */
const CxBoard &cx_board(uint8_t daa_type)
{
	switch (daa_type) {
	case 0x22: case 0x23: case 0x24: case 0x29: case 0x30: case 0x31:
		return boards[1];
	default:
		return boards[0];
	}
}

} // namespace dmd
