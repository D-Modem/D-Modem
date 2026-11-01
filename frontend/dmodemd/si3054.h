/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    si3054.h - a country's line settings for a Silicon Labs Si3054
 *               HD-audio modem codec and its Si3018 line side (the ALSA
 *               line's, alsa_line.cpp, through hda_codec.h)
 *
 *    The settings are the datasheet's (Si3054/Si3018 Rev. 1.0, Table 19,
 *    "Country Specific Register Settings", its HD audio columns): the
 *    on-hook speed (OHS, OHS2), the ringer impedance and threshold (RZ,
 *    RT), the AC termination (ACT), the DC termination (DCV, MINI) and
 *    the loop current limit (ILIM).  They go into its registers, each a
 *    node of the codec's: 13 (Line-Side Configuration 1), 15 (DC
 *    Termination), 16 (Line-Side Configuration) and 18 (Spark
 *    Quenching Control, SQ[1:0] = 11 with OHS), 16 bits each, each read
 *    first and only its own bits changed.
 *
 */
#ifndef DMODEMD_SI3054_H
#define DMODEMD_SI3054_H


#include "dm_types.h"

namespace dmd {

/* a row of Table 19 (HD audio mode) */
struct Si3054Country {
	const char *name;       /* D-Modem's (-c, +GCI), and its aliases */
	u8 ohs, rz, rt, act, ohs2, dcv, mini, ilim;
};

/* the registers (nodes) a country's settings go into, as read and then
   as they are to be written */
struct Si3054Regs {
	u16 n13, n15, n16, n18;
};

/* the nodes */
enum { SI3054_CHIPID = 12, SI3054_LINE_CFG1 = 13, SI3054_DC_TERM = 15,
       SI3054_LINE_CFG = 16, SI3054_SQ_CONTROL = 18 };

/* the row of the country named name (D-Modem's names, case and '_'
   ignored), or nullptr: the datasheet has none */
const Si3054Country *si3054_country(const char *name);

/* the USA's row (FCC) */
const Si3054Country &si3054_usa();

/* whether chipid (node 12) is a Si3054's with a Si3018 line side: the
   codec the table is for (SID 1, LSID 0001) */
bool si3054_chip_ok(u16 chipid);

/* r, as read, with c's settings; off hook (off_hook) RZ cleared, as the
   datasheet has it during pulse dialling, set again on hook */
void si3054_apply(const Si3054Country &c, bool off_hook, Si3054Regs &r);

} // namespace dmd

#endif
