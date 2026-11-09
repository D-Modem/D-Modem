/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    rtp_red.h - RFC 2198's redundant audio: each RTP packet carries the
 *                frames before it again, so that a packet lost on the
 *                way is had from the next (V.152's assured VBD mode,
 *                for a lossy trunk)
 *
 *    A RED payload is its blocks' headers, then their data in the same
 *    order: the redundant blocks, oldest first, each with a header of
 *    four octets (F=1, its payload type, its timestamp as an offset
 *    back from the packet's, 14 bits, and its length, 10 bits), and the
 *    primary last, its header one octet (F=0, its payload type), its
 *    timestamp the packet's and its length the rest.
 *
 *    In SDP the format is red/8000, its a=fmtp the payload types of the
 *    blocks, primary first, apart by slashes ("0/0/0": PCMU, and two
 *    frames before it again).
 */
#ifndef DMODEMD_RTP_RED_H
#define DMODEMD_RTP_RED_H

#include <cstddef>
#include <string>
#include <vector>

#include "dm_types.h"

namespace dmd {
namespace red {

/* a block: its payload type, timestamp, data (n octets) */
struct Block {
	u8 pt;
	u32 ts;
	const u8 *data;
	size_t n;
};

/* the longest offset back and length a redundant block's header has */
const u32 MAX_OFFSET = (1u << 14) - 1;
const size_t MAX_LEN = (1u << 10) - 1;

/* RED payload p (n octets) of a packet at timestamp ts into out, the
   redundant blocks first (oldest first), the primary last; false (out
   empty) if it is malformed: its headers run past its end, or their
   lengths do */
bool decode(const u8 *p, size_t n, u32 ts, std::vector<Block> *out);

/* a RED payload into out (max octets): the redundant blocks red (nred
   of them, oldest first), the ones that can't go in a header (an
   offset back from primary's timestamp of 0 or past MAX_OFFSET, or a
   length past MAX_LEN) left out, then primary; its length, 0 if it
   won't fit in max */
size_t encode(const Block *red, size_t nred, const Block &primary, u8 *out,
	      size_t max);

/* an a=fmtp value's payload types ("0/0/0"), primary first; empty if
   it is not one (a payload type past 127, or none) */
std::vector<s16> fmtp_types(const std::string &v);

/* our a=fmtp value for payload type pt and level frames again:
   "<pt>/<pt>..." (level + 1 of them) */
std::string fmtp(s16 pt, s16 level);

} // namespace red
} // namespace dmd

#endif
