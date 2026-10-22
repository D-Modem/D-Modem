/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    si3056.h - a country's line settings for a Skyworks (Silicon Labs)
 *               Si3056 DAA and its Si3018 or Si3019 line side, the
 *               pico-serial Line stick's (usb_line.cpp)
 *
 *    The settings are the datasheet's (Si3056/18/19/10 Rev. 1.03, Table
 *    15, "Country Specific Register Settings"): the on-hook speed (OHS,
 *    OHS2), the ringer impedance and threshold (RZ, RT), the loop
 *    current limit (ILIM), the DC termination (DCV, MINI) and the AC
 *    termination, ACT and ACT2 on a Si3018, ACIM on a Si3019.  They go
 *    into registers 16, 26, 30 (Si3019), 31 and 59 (the spark quenching
 *    OHS needs, SQ[1:0] = 11), each read first and only its own bits
 *    changed (the reserved ones "not written").
 *
 */
#ifndef DMODEMD_SI3056_H
#define DMODEMD_SI3056_H

#include <stdint.h>

namespace dmd {

/* a row of Table 15 */
struct Si3056Country {
	const char *name;       /* D-Modem's (-c, +GCI), and its aliases */
	uint8_t ohs, ohs2, rz, rt, ilim, dcv, mini, acim, act, act2;
};

/* the registers a country's settings go into, as read and then as they
   are to be written */
struct Si3056Regs {
	uint8_t r16, r26, r30, r31, r59;
};

/* the row of the country named name (D-Modem's names, case and '_'
   ignored), or nullptr: the datasheet has none (the USA's, the reset
   values, are the caller's) */
const Si3056Country *si3056_country(const char *name);

/* the USA's row (FCC: the Si3056's reset values) */
const Si3056Country &si3056_usa();

/* r, as read, with c's settings: on a Si3019 line side (si3019) ACIM
   in register 30, on a Si3018 ACT and ACT2 in register 16 (30 left as
   it is) */
void si3056_apply(const Si3056Country &c, bool si3019, Si3056Regs &r);

} // namespace dmd

#endif
