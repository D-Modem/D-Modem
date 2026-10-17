/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    rs.cpp - the Reed-Solomon code of rs.h: systematic encoding by the
 *             generator's division, decoding of errors and erasures by
 *             the syndromes, Berlekamp-Massey from the erasures' locator,
 *             the Chien search and Forney's values
 *
 */

#include <string.h>

#include "rs.h"

namespace dsp {

int ReedSolomon::init(int n, int k)
{
	if (n < 2 || n > MAX_N || k < 1 || k >= n || n - k > MAX_CHECK)
		return -1;
	n_ = n;
	k_ = k;
	nc_ = n - k;
	/* the field: a the root of x^8 + x^4 + x^3 + x^2 + 1 (0x11d) */
	int x = 1;
	for (int i = 0; i < 255; i++) {
		exp_[i] = exp_[i + 255] = (uint8_t)x;
		log_[x] = i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11d;
	}
	exp_[510] = exp_[511] = exp_[0];
	log_[0] = -1;
	/* g(x) = prod (x + a^i), i = 0 .. nc - 1, gen_[0] the x^nc term */
	uint8_t g[MAX_CHECK + 1] = { 1 };
	int deg = 0;
	for (int i = 0; i < nc_; i++) {
		/* g *= (x + a^i) */
		g[deg + 1] = 0;
		for (int j = deg + 1; j > 0; j--)
			g[j] ^= mul(g[j - 1], exp_[i]);
		deg++;
	}
	memcpy(gen_, g, (size_t)nc_ + 1);
	return 0;
}

void ReedSolomon::encode(const uint8_t *msg, uint8_t *cw) const
{
	uint8_t p[MAX_CHECK] = {};
	for (int i = 0; i < k_; i++) {
		uint8_t fb = msg[i] ^ p[0];
		for (int j = 0; j < nc_ - 1; j++)
			p[j] = p[j + 1] ^ mul(fb, gen_[j + 1]);
		p[nc_ - 1] = mul(fb, gen_[nc_]);
	}
	if (cw != msg)
		memmove(cw, msg, (size_t)k_);
	memcpy(cw + k_, p, (size_t)nc_);
}

int ReedSolomon::decode(uint8_t *cw, const uint8_t *erased) const
{
	const int n = n_, nc = nc_;
	/* S_j = r(a^j); r's coefficient of x^(n - 1 - i) is cw[i] */
	uint8_t S[MAX_CHECK];
	bool any = false;
	for (int j = 0; j < nc; j++) {
		uint8_t s = 0;
		for (int i = 0; i < n; i++)
			s = mul(s, exp_[j]) ^ cw[i];
		S[j] = s;
		any |= s != 0;
	}
	if (!any)
		return 0;

	/* the erasures' locator: prod (1 + X x), X = a^(n - 1 - i) */
	uint8_t lam[MAX_CHECK + 1] = { 1 }, b[MAX_CHECK + 1], t[MAX_CHECK + 1];
	int ne = 0;
	for (int i = 0; erased && i < n; i++) {
		if (!erased[i])
			continue;
		if (++ne > nc)
			return -1;
		uint8_t X = exp_[n - 1 - i];
		for (int j = ne; j > 0; j--)
			lam[j] ^= mul(lam[j - 1], X);
	}
	memcpy(b, lam, sizeof(lam));
	int L = ne;
	for (int r = ne; r < nc; r++) {
		uint8_t d = 0;
		for (int i = 0; i <= r && i <= nc; i++)
			d ^= mul(lam[i], S[r - i]);
		if (!d) {
			memmove(b + 1, b, (size_t)nc);
			b[0] = 0;
			continue;
		}
		t[0] = lam[0];
		for (int i = 1; i <= nc; i++)
			t[i] = lam[i] ^ mul(d, b[i - 1]);
		if (2 * L <= r + ne) {
			L = r + 1 + ne - L;
			uint8_t inv = exp_[255 - log_[d]];
			for (int i = 0; i <= nc; i++)
				b[i] = mul(lam[i], inv);
		} else {
			memmove(b + 1, b, (size_t)nc);
			b[0] = 0;
		}
		memcpy(lam, t, sizeof(t));
	}
	int deg = 0;
	for (int i = 0; i <= nc; i++)
		if (lam[i])
			deg = i;
	if (deg > nc)
		return -1;

	/* the roots: a^-p for the error at the coefficient of x^p */
	int pos[MAX_CHECK], nr = 0;
	for (int p = 0; p < n && nr <= deg; p++) {
		uint8_t v = 0;
		int xi = (255 - p) % 255;       /* log of a^-p */
		for (int i = deg; i >= 0; i--)
			v = (uint8_t)(lam[i] ^ (v ? exp_[log_[v] + xi] : 0));
		if (!v) {
			if (nr == MAX_CHECK)
				return -1;
			pos[nr++] = p;
		}
	}
	if (nr != deg)
		return -1;

	/* Omega = S Lambda mod x^nc; the values X Omega(1/X) / Lambda'(1/X) */
	uint8_t om[MAX_CHECK];
	for (int i = 0; i < nc; i++) {
		uint8_t v = 0;
		for (int j = 0; j <= i && j <= deg; j++)
			v ^= mul(lam[j], S[i - j]);
		om[i] = v;
	}
	uint8_t save[MAX_N];
	memcpy(save, cw, (size_t)n);
	int fixed = 0;
	for (int k = 0; k < nr; k++) {
		int p = pos[k];
		int xi = (255 - p) % 255;
		uint8_t o = 0, dl = 0;
		for (int i = nc - 1; i >= 0; i--)
			o = (uint8_t)(om[i] ^ (o ? exp_[log_[o] + xi] : 0));
		/* Lambda'(x) = the odd terms, lambda_i x^(i - 1) */
		for (int i = 1; i <= deg; i += 2)
			if (lam[i])
				dl ^= exp_[(log_[lam[i]] + xi * (i - 1)) % 255];
		if (!dl) {
			memcpy(cw, save, (size_t)n);
			return -1;
		}
		uint8_t e = o ? exp_[(log_[o] + p + 255 - log_[dl]) % 255] : 0;
		if (e) {
			cw[n - 1 - p] ^= e;
			fixed++;
		}
	}
	/* a decoding the syndromes don't confirm is a failure */
	for (int j = 0; j < nc; j++) {
		uint8_t s = 0;
		for (int i = 0; i < n; i++)
			s = mul(s, exp_[j]) ^ cw[i];
		if (s) {
			memcpy(cw, save, (size_t)n);
			return -1;
		}
	}
	return fixed;
}

} // namespace dsp
