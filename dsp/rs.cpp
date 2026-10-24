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

#include <cstring>

#include "dm_types.h"
#include "reinit.h"
#include "rs.h"

namespace dsp {

/* see rs.h */
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
		exp_[i] = exp_[i + 255] = static_cast<u8>(x);
		log_[x] = i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11d;
	}
	exp_[510] = exp_[511] = exp_[0];
	log_[0] = -1;
	/* g(x) = prod (x + a^i), i = 0 .. nc - 1, gen_[0] the x^nc term */
	u8 g[MAX_CHECK + 1] = { 1 };
	int deg = 0;
	for (int i = 0; i < nc_; i++) {
		/* g *= (x + a^i) */
		g[deg + 1] = 0;
		for (int j = deg + 1; j > 0; j--)
			g[j] ^= mul(g[j - 1], exp_[i]);
		deg++;
	}
	memcpy(gen_, g, static_cast<size_t>(nc_) + 1);
	return 0;
}

/* see rs.h */
void ReedSolomon::encode(const u8 *msg, u8 *cw) const
{
	u8 p[MAX_CHECK] = {};
	for (int i = 0; i < k_; i++) {
		u8 fb = msg[i] ^ p[0];
		for (int j = 0; j < nc_ - 1; j++)
			p[j] = p[j + 1] ^ mul(fb, gen_[j + 1]);
		p[nc_ - 1] = mul(fb, gen_[nc_]);
	}
	if (cw != msg)
		memmove(cw, msg, static_cast<size_t>(k_));
	memcpy(cw + k_, p, static_cast<size_t>(nc_));
}

/* see rs.h */
int ReedSolomon::decode(u8 *cw, const u8 *erased) const
{
	const int n = n_, nc = nc_;
	/* S_j = r(a^j); r's coefficient of x^(n - 1 - i) is cw[i] */
	u8 S[MAX_CHECK];
	bool any = false;
	for (int j = 0; j < nc; j++) {
		u8 s = 0;
		for (int i = 0; i < n; i++)
			s = mul(s, exp_[j]) ^ cw[i];
		S[j] = s;
		any |= s != 0;
	}
	if (!any)
		return 0;

	/* the erasures' locator: prod (1 + X x), X = a^(n - 1 - i) */
	u8 lam[MAX_CHECK + 1] = { 1 }, b[MAX_CHECK + 1], t[MAX_CHECK + 1];
	int ne = 0;
	for (int i = 0; erased && i < n; i++) {
		if (!erased[i])
			continue;
		if (++ne > nc)
			return -1;
		u8 X = exp_[n - 1 - i];
		for (int j = ne; j > 0; j--)
			lam[j] ^= mul(lam[j - 1], X);
	}
	copy_array(b, lam);
	int L = ne;
	for (int r = ne; r < nc; r++) {
		u8 d = 0;
		for (int i = 0; i <= r && i <= nc; i++)
			d ^= mul(lam[i], S[r - i]);
		if (!d) {
			memmove(b + 1, b, static_cast<size_t>(nc));
			b[0] = 0;
			continue;
		}
		t[0] = lam[0];
		for (int i = 1; i <= nc; i++)
			t[i] = lam[i] ^ mul(d, b[i - 1]);
		if (2 * L <= r + ne) {
			L = r + 1 + ne - L;
			u8 inv = exp_[255 - log_[d]];
			for (int i = 0; i <= nc; i++)
				b[i] = mul(lam[i], inv);
		} else {
			memmove(b + 1, b, static_cast<size_t>(nc));
			b[0] = 0;
		}
		copy_array(lam, t);
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
		u8 v = 0;
		int xi = (255 - p) % 255;       /* log of a^-p */
		for (int i = deg; i >= 0; i--)
			v = static_cast<u8>(lam[i] ^
						 (v ? exp_[log_[v] + xi] : 0));
		if (!v) {
			if (nr == MAX_CHECK)
				return -1;
			pos[nr++] = p;
		}
	}
	if (nr != deg)
		return -1;

	/* Omega = S Lambda mod x^nc; the values X Omega(1/X) / Lambda'(1/X) */
	u8 om[MAX_CHECK];
	for (int i = 0; i < nc; i++) {
		u8 v = 0;
		for (int j = 0; j <= i && j <= deg; j++)
			v ^= mul(lam[j], S[i - j]);
		om[i] = v;
	}
	u8 save[MAX_N];
	memcpy(save, cw, static_cast<size_t>(n));
	int fixed = 0;
	for (int k = 0; k < nr; k++) {
		int p = pos[k];
		int xi = (255 - p) % 255;
		u8 o = 0, dl = 0;
		for (int i = nc - 1; i >= 0; i--)
			o = static_cast<u8>(om[i] ^
						 (o ? exp_[log_[o] + xi] : 0));
		/* Lambda'(x) = the odd terms, lambda_i x^(i - 1) */
		for (int i = 1; i <= deg; i += 2)
			if (lam[i])
				dl ^= exp_[(log_[lam[i]] + xi * (i - 1)) % 255];
		if (!dl) {
			memcpy(cw, save, static_cast<size_t>(n));
			return -1;
		}
		u8 e = o ? exp_[(log_[o] + p + 255 - log_[dl]) % 255] : 0;
		if (e) {
			cw[n - 1 - p] ^= e;
			fixed++;
		}
	}
	/* a decoding the syndromes don't confirm is a failure */
	for (int j = 0; j < nc; j++) {
		u8 s = 0;
		for (int i = 0; i < n; i++)
			s = mul(s, exp_[j]) ^ cw[i];
		if (s) {
			memcpy(cw, save, static_cast<size_t>(n));
			return -1;
		}
	}
	return fixed;
}

} // namespace dsp
