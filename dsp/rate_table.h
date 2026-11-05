/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    rate_table.h - a QAM pump's choice of rates on its training (V.32,
 *                   V.61): each rate of its table (Rate, with its bit
 *                   in the rate words as bits) has the training SNR it
 *                   takes, the last (its slowest) 0, which any line
 *                   takes
 *
 */

#ifndef DSP_RATE_TABLE_H
#define DSP_RATE_TABLE_H

#include <cmath>
#include <cstddef>

#include "dm_types.h"

namespace dsp {

/* the rate bits of rates that a training with decision error mse (of
   the constellation's power) takes */
template <class Rate, std::size_t N>
inline u16 rate_caps(const Rate (&rates)[N], const float (&min_snr)[N],
		     float mse)
{
	float snr = -10 * log10f(mse + 1e-6f);
	u16 caps = 0;
	for (std::size_t i = 0; i < N; i++)
		if (snr >= min_snr[i])
			caps |= rates[i].bits;
	return caps;
}

/* the data mse past which a receiver at rate r (of rates) has lost
   its equalization: 4 dB short of the SNR the rate takes (the slowest:
   7 dB), so that a line that only just took the rate does not retrain
   again and again */
template <class Rate, std::size_t N>
inline float rate_lost_limit(const Rate (&rates)[N],
			     const float (&min_snr)[N], const Rate *r)
{
	std::size_t i = static_cast<std::size_t>(r - rates);
	float snr = min_snr[i] > 0 ? min_snr[i] - 4 : 7;
	return powf(10, -snr / 10);
}

} // namespace dsp

#endif
