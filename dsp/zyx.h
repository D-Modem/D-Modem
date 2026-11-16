/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    zyx.h - ZyXEL's ZyX modes (the U-1496 family, docs/dsp/zyxel.md):
 *            V.32bis at 2400 x 8/7 = 2742.857 baud, and the 8 symbol
 *            frame with its ring table (shell mapping) of ZyX 16800 to
 *            ZyX 7200
 *
 *    ZyX 19200 is V.32bis's trellis code with 7 data bits a symbol on
 *    zyx_points_19200.  The other rates read 7 groups of N bits a frame
 *    of 8 symbols (N = 7 to 3 for 16800 to 7200): the 7N bits in order,
 *    the first K the index of a row of zyx_rings (the least significant
 *    bit first), then M bits each symbol, its Q1 Q2 first and its
 *    uncoded bits after.  The row gives each symbol its ring (0 to 3),
 *    label bits 3 and 4.
 *
 */
#ifndef DSP_ZYX_H
#define DSP_ZYX_H

#include "dm_types.h"
#include "dsp.h"

namespace dsp {

#define ZYX_FRAME 8             /* symbols a frame */
#define ZYX_GROUPS 7            /* groups of N bits a frame */
#define ZYX_MAX_BITS 49         /* 7 x 7, ZyX 16800's frame */

extern const cplx zyx_points_32[32];
extern const cplx zyx_points_64[64];
extern const cplx zyx_points_128[128];
extern const cplx zyx_points_256[256];
extern const cplx zyx_points_19200[256];
/* in order of the cost (the sum of the rings): row i, symbol k's ring in
   bits 2k and 2k+1 */
extern const u16 zyx_rings[4096];

/* a framed rate: N bits a group, K index bits, M bits a symbol (Q1 Q2
   and M - 2 uncoded); its points are 2^(M + 3) */
struct ZyxFrameRate {
	s16 n, k, m;

	/* the bits a frame carries: 7N */
	s16 frame_bits() const { return ZYX_GROUPS * n; }
	/* the trellis encoder's bits a symbol: Q1 Q2, the ring, the
	   uncoded bits */
	s16 sym_bits() const { return m + 2; }

	/* the 7N bits of a frame (0 or 1, in order) to its 8 symbols' data
	   bits q[] for TrellisEnc::encode() (Q1 Q2 in bits 0-1, the ring in
	   bits 2-3, the uncoded bits from bit 4) */
	void encode(const u8 *bits, s16 *q) const
	{
		s16 i = 0, s, b;
		u16 row = 0;
		for (b = 0; b < k; b++)
			row |= static_cast<u16>((bits[i++] & 1) << b);
		u16 rings = zyx_rings[row];
		for (s = 0; s < ZYX_FRAME; s++) {
			s16 v = 0;
			for (b = 0; b < m; b++)
				v |= static_cast<s16>((bits[i++] & 1) << b);
			q[s] = static_cast<s16>((v & 3) |
						((rings >> (2 * s)) & 3) << 2 |
						(v >> 2) << 4);
		}
	}

	/* the inverse: the 8 decoded symbols' q[] to the 7N bits; false (the
	   bits as they come) when the rings are no row of this rate's */
	bool decode(const s16 *q, u8 *bits) const
	{
		u16 rings = 0;
		s16 i = k, s, b;
		for (s = 0; s < ZYX_FRAME; s++) {
			rings |= static_cast<u16>(((q[s] >> 2) & 3) << (2 * s));
			s16 v = static_cast<s16>((q[s] & 3) | (q[s] >> 4) << 2);
			for (b = 0; b < m; b++)
				bits[i++] = (v >> b) & 1;
		}
		s16 row = row_of(rings);
		for (b = 0; b < k; b++)
			bits[b] = row < 0 ? 0 : (row >> b) & 1;
		return row >= 0;
	}

	/* the row of the rings among this rate's 2^K first, or -1 */
	s16 row_of(u16 rings) const
	{
		/* the table's index: a 64K map built once */
		static s16 *inv = [] {
			static s16 t[65536];
			for (s32 j = 0; j < 65536; j++)
				t[j] = -1;
			for (s16 j = 0; j < 4096; j++)
				t[zyx_rings[j]] = j;
			return t;
		}();
		s16 r = inv[rings];
		return r >= 0 && r < (1 << k) ? r : -1;
	}
};

/* ZyX 16800, 14400, 12000, 9600, 7200 */
static constexpr ZyxFrameRate zyx_16800 = { 7, 9, 5 };
static constexpr ZyxFrameRate zyx_14400 = { 6, 10, 4 };
static constexpr ZyxFrameRate zyx_12000 = { 5, 11, 3 };
static constexpr ZyxFrameRate zyx_9600 = { 4, 12, 2 };
static constexpr ZyxFrameRate zyx_7200 = { 3, 5, 2 };

} // namespace dsp

#endif /* DSP_ZYX_H */
