/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v44_preset.h - V.cvm's preset dictionary for V.44 (Annex E of
 *                   docs/cellular/v_cvm.tex): text both DCEs take into
 *                   V.44's dictionary and history before the data
 *                   (V44::prime()), where both indicated it
 *
 *    Its lines (v44_preset.cpp), each ended by CR LF, in that order: the
 *    phrases of the protocols and formats that go over a slow mobile
 *    data call (HTTP, mail, HTML, JSON and XML, NMEA telemetry, logins)
 *    and common English, 7-bit characters only (so that V.44's ordinals
 *    stay 7 bits).
 *
 */

#ifndef DMODEM_LINK_V44_PRESET_H
#define DMODEM_LINK_V44_PRESET_H

#include <vector>

#include "dm_types.h"

namespace dlink {

/* the preset's version, as the capabilities indicate it (1) */
enum { V44_PRESET_VERSION = 1 };

/* its text: the lines, each with CR LF */
const std::vector<u8> &v44_preset();

} // namespace dlink

#endif
