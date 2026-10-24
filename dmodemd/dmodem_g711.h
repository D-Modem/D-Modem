/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem_g711.h - libdmodem's G.711 calls (dmodem_process_g711,
 *                    dmodem_fax_process_g711): the call's octets as 16
 *                    bit linear samples, a block at a time
 *
 */
#ifndef DMODEM_G711_H
#define DMODEM_G711_H

#include <cstdint>

#include "g711.h"

/* n octets in, n out: each block of up to 160 (20 ms) decoded, through
   process(const int16_t *in, int16_t *out, int k), encoded */
template <class Process>
void dmodem_g711_blocks(int alaw, const unsigned char *in,
			unsigned char *out, int32_t n, Process process)
{
	int16_t li[160], lo[160];

	while (n > 0) {
		int k = n < 160 ? static_cast<int>(n) : 160;
		for (int i = 0; i < k; i++)
			li[i] = static_cast<int16_t>(
				dsp::g711::decode(alaw, in[i]));
		process(li, lo, k);
		for (int i = 0; i < k; i++)
			out[i] = dsp::g711::encode(alaw, lo[i]);
		in += k;
		out += k;
		n -= k;
	}
}

#endif
