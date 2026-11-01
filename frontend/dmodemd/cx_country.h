/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    cx_country.h - a country's line setting and the board's parameters
 *                   for a Conexant AC'97 codec's SmartDAA (cx_daa.h, the
 *                   ALSA line's)
 *
 *    Both are Conexant's, from its HSF driver's files (dsp/test/hsf/):
 *    the country's from hsf.cty (each region's SMART_RELAYS), the
 *    board's from hsfmc97ich.inf (SmartDAAParams, SmartDAAParamsK3), as
 *    far as Conexant's library is seen to use them in the HSF harness
 *    (docs/hardware.md): the country LSD register 0x12's bits 6:5, the
 *    board three bytes of the SSD's setup (registers 0x06, 0x0b, 0x21).
 *
 */
#ifndef DMODEMD_CX_COUNTRY_H
#define DMODEMD_CX_COUNTRY_H


#include "dm_types.h"

namespace dmd {

/* a region of hsf.cty */
struct CxCountry {
	const char *name;       /* hsf.cty's (spaces '_'), or D-Modem's */
	u8 idle;                /* LSD 0x12's bits 6:5 on hook */
	u8 off;                 /* and off hook */
};

/* a board's SmartDAA parameters, an .inf block as it is */
struct CxBoard {
	const char *name;       /* the .inf's value name */
	u8 p[95];

	/* the bytes the SSD's setup takes: register 0x21, 0x80 | p[87] */
	u8 ssd21() const { return static_cast<u8>(0x80 | p[87]); }
	/* register 0x0b, p[93] */
	u8 ssd0b() const { return p[93]; }
	/* register 0x06's bits 7:4, p[94] */
	u8 ssd06() const { return static_cast<u8>(p[94] << 4); }
};

/* the row of the country named name (case, ' ' and '_' ignored), or
   nullptr: hsf.cty has none */
const CxCountry *cx_country(const char *name);

/* the USA's row */
const CxCountry &cx_usa();

/* the board block the ICH driver gives a codec of DAA type daa_type
   (register 0x7e's low byte): SmartDAAParamsK3 for 0x22 to 0x24, 0x29,
   0x30 and 0x31, SmartDAAParams for the others */
const CxBoard &cx_board(u8 daa_type);

} // namespace dmd

#endif
