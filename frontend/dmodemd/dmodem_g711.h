/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_g711.h - libdmodem's G.711 calls (dmodem_process_g711,
 *                    dmodem_fax_process_g711): the call's octets as 16
 *                    bit linear samples, a block at a time
 *
 */
#ifndef DMODEM_G711_H
#define DMODEM_G711_H


#include "dm_types.h"
#include "g711.h"

/* n octets in, n out: each block of up to 160 (20 ms) decoded, through
   process(const s16 *in, s16 *out, s16 k), encoded */
template <class Process>
void dmodem_g711_blocks(s16 alaw, const unsigned char *in,
			unsigned char *out, s32 n, Process process)
{
	s16 li[160], lo[160];

	while (n > 0) {
		s16 k = n < 160 ? static_cast<s16>(n) : 160;
		for (s16 i = 0; i < k; i++)
			li[i] = static_cast<s16>(
				dsp::g711::decode(alaw, in[i]));
		process(li, lo, k);
		for (s16 i = 0; i < k; i++)
			out[i] = dsp::g711::encode(alaw, lo[i]);
		in += k;
		out += k;
		n -= k;
	}
}

#endif
