/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    rs.h - a Reed-Solomon code over GF(2^8): the field of x^8 + x^4 +
 *           x^3 + x^2 + 1 and the generator prod (x - a^i), i = 0 ..
 *           2t - 1, of G.975's RS(255,239), shortened to n octets (V.cvm's
 *           inner code, docs/v_cvm.pdf Annex C)
 *
 *    A codeword is its k information octets then its n - k check octets,
 *    the first octet the highest order coefficient.  The decoder
 *    corrects v errors and e erasures where 2v + e <= n - k.
 *
 */

#ifndef DSP_RS_H
#define DSP_RS_H

#include <stdint.h>

namespace dsp {

class ReedSolomon {
public:
	enum { MAX_N = 255, MAX_CHECK = 64 };

	/* n octets a codeword, k of them information: 0, or -1 */
	int init(int n, int k);
	/* (see init()) */
	int n() const { return n_; }
	/* (see init()) */
	int k() const { return k_; }

	/* the codeword of k octets (cw may be msg): its n octets */
	void encode(const uint8_t *msg, uint8_t *cw) const;
	/* the codeword corrected in place, erased[i] (or none) the octets
	   known to be unreliable: the octets corrected, or -1 if it could
	   not be (cw is then as it came) */
	int decode(uint8_t *cw, const uint8_t *erased = nullptr) const;

private:
	/* a times b in the field */
	uint8_t mul(uint8_t a, uint8_t b) const
	{
		return a && b ? exp_[log_[a] + log_[b]] : 0;
	}

	int n_ = 0, k_ = 0, nc_ = 0;    /* nc_ check octets, 2t */
	uint8_t gen_[MAX_CHECK + 1] = {};       /* the generator, x^nc_ first */
	uint8_t exp_[512] = {};
	int log_[256] = {};
};

} // namespace dsp

#endif /* DSP_RS_H */
