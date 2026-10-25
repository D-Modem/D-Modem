/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    zstd_dict.h - V.cvm's dictionary for zstd (Annex F of
 *                  docs/cellular/v_cvm.tex): a formatted Zstandard
 *                  dictionary (RFC 8878 5), its content Annex E's text
 *                  (v44_preset.h), where both DCEs indicated the preset
 *
 *    Its entropy tables are fixed octets (made once by
 *    Zstd::make_dict() from the text, then frozen: the Recommendation
 *    lists them), so that an encoder's changes cannot change them.
 *
 */

#ifndef DMODEM_LINK_ZSTD_DICT_H
#define DMODEM_LINK_ZSTD_DICT_H

#include <vector>

#include "dm_types.h"

namespace dlink {

/* the dictionary's ID: "VCM" and its version, 1 */
constexpr u32 ZSTD_DICT_ID = 0x56434D01;

/* the dictionary: its header (magic, ID, tables, repeat offsets),
   then Annex E's text */
const std::vector<u8> &zstd_dict();

/* the header's octets (zstd_dict() without the text) */
s32 zstd_dict_header();

} // namespace dlink

#endif
