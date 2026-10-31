/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex.cpp - K56flex's data mode encoder and decoder (k56flex.h)
 *
 */

#include <string.h>

#include "bits.h"
#include "k56flex.h"
#include "reinit.h"

namespace dsp {

/* a code's value, mu-law codes in 14 bit units, A-law ones in 13 */
s32 k56_value(unsigned char code, s16 alaw)
{
	s32 m = code & 0x7f, seg = m >> 4, v;
	if (alaw)
		v = seg ? (2 * (m & 15) + 33) << (seg - 1) : 2 * m + 1;
	else
		v = ((2 * (m & 15) + 33) << seg) - 33;
	return code & 0x80 ? v : -v;
}

/* a code as a G.711 octet of the law */
unsigned char k56_g711(unsigned char code, s16 alaw)
{
	unsigned char m = code & 0x7f;
	if (alaw)
		return static_cast<unsigned char>(code ^ 0x55);
	return static_cast<unsigned char>((code & 0x80) ? 0xff - m : 0x7f - m);
}

/* a G.711 octet of the law as a code */
unsigned char k56_code(unsigned char g, s16 alaw)
{
	if (alaw)
		return static_cast<unsigned char>(g ^ 0x55);
	return static_cast<unsigned char>((g & 0x80) ? 0x80 | (0xff - g) : 0x7f - g);
}

/* the codec for line l; -1 for a rate or pad that K56flex does not have */
s16 K56Codec::init(const K56Line &l)
{
	s16 i = static_cast<s16>((l.rate - 32000) / 2000), k, c, d;
	reinit(*this);
	if (l.rate < 32000 || l.rate > 56000 || l.rate % 2000 || l.pad < 0 ||
	    l.pad > 2)
		return -1;
	const K56Rate &p = k56_rates[i];
	rate = l.rate;
	alaw = l.alaw;
	K = p.K;
	q = static_cast<s16>(p.u - 1);
	r = p.r;
	M = p.M[l.pad];
	ring = k56_ring[l.pad][i];
	bios = k56_bios[l.pad][i];
	/* robbed bits only above 34000 bit/s; 5 of 6 robs them all */
	unsigned char rb[6] = {};
	if (rate > 34000) {
		for (k = 0; k < 6; k++)
			nrob = static_cast<s16>(nrob + (rb[k] = (l.robbed >> k) & 1));
		if (nrob == 5)
			memset(rb, 1, sizeof rb);
	}
	for (k = 0; k < 24; k++) {
		rob[k] = rb[k % 6];
		nrob_f[k / 8] = static_cast<s16>(nrob_f[k / 8] + rob[k]);
	}
	loss = k56_rbs_loss[nrob];
	/* V.34's shell mapping tables (9.3/V.34): g2, g4, and z8, the
	   cumulative g8 */
	u32 g8[128] = {};
	for (c = 0; c < 2 * M - 1; c++)
		g2[c] = static_cast<u32>(c < M ? c + 1 : 2 * M - 1 - c);
	for (c = 0; c < 4 * M - 3; c++)
		for (d = 0; d <= c; d++)
			if (c - d < 2 * M - 1 && d < 2 * M - 1)
				g4[c] += g2[c - d] * g2[d];
	for (c = 0; c < 8 * M - 7; c++)
		for (d = 0; d <= c; d++)
			if (c - d < 4 * M - 3 && d < 4 * M - 3)
				g8[c] += g4[c - d] * g4[d];
	for (c = 1; c < 8 * M - 7; c++)
		z8[c] = z8[c - 1] + g8[c - 1];
	f = 2;
	return 0;
}

/* on to the next frame of the 3 frame cycle (24 symbols, 4 robbed-bit
   cycles); returns it */
s16 K56Codec::next_frame()
{
	f = static_cast<s16>(f == 2 ? 0 : f + 1);
	return f;
}

/* the bits the shell mapper takes this frame; *adj: the shell index's
   low bits that are left 0 (so that its range stays the same) */
s16 K56Codec::shell_bits(s16 *adj) const
{
	*adj = static_cast<s16>(nrob == 3 ? 0 : loss - nrob_f[f]);
	s16 k = static_cast<s16>(K - *adj);
	if (!half && r == 16)
		k--;
	return k;
}

/* whether the next symbol's sign is the shaping's */
bool K56Codec::shaping_next()
{
	if (++rc < r)
		return false;
	rc = 0;
	return true;
}

/* a code sent (or received): into the running sum */
void K56Codec::sent(unsigned char code)
{
	rds += k56_value(code, alaw);
}

/* the data bits the next frame takes */
s16 K56Enc::frame_bits() const
{
	s16 n = static_cast<s16>(f == 2 ? 0 : f + 1);
	return static_cast<s16>(rate / 1000 - (nrob == 3 ? nrob_f[n] : loss));
}

/* the ring pair of sum s and index idx (9.3/V.34) */
static void ring_pair(s16 M, s16 s, u32 idx, s16 *a, s16 *b)
{
	if (s < M) {
		*a = static_cast<s16>(idx);
		*b = static_cast<s16>(s - *a);
	} else {
		*b = static_cast<s16>(M - 1 - static_cast<s16>(idx));
		*a = static_cast<s16>(s - *b);
	}
}

/* a frame: frame_bits() data bits to 8 codes */
void K56Enc::frame(const unsigned char *bits, unsigned char code[8])
{
	s16 nb = frame_bits(), adj, i;
	next_frame();
	u64 w = 0;
	for (i = 0; i < nb; i++)
		w |= static_cast<u64>(scramble(&scr, SCR_GPA, bits[i] & 1)) << i;
	s16 ks = shell_bits(&adj);
	u32 S = static_cast<u32>(w & ((1ULL << ks) - 1)) << adj;
	u32 U = static_cast<u32>(w >> ks);
	/* the shell mapper (9.3/V.34): the index to 8 rings */
	s16 A = 0, B = 0, C = 0, D = 0, m[8];
	while (z8[A + 1] <= S)
		A++;
	u32 r1 = S - z8[A], t = g4[A] * g4[0];
	while (r1 >= t) {
		r1 -= t;
		B++;
		t = g4[A - B] * g4[B];
	}
	u32 r2 = r1 % g4[B], r3 = r1 / g4[B];
	for (t = g2[0] * g2[B]; r2 >= t; t = g2[B - C] * g2[C]) {
		r2 -= t;
		C++;
	}
	s16 E = static_cast<s16>(A - B);
	for (t = g2[0] * g2[E]; r3 >= t; t = g2[E - D] * g2[D]) {
		r3 -= t;
		D++;
	}
	ring_pair(M, C, r2 % g2[C], &m[0], &m[1]);
	ring_pair(M, static_cast<s16>(B - C), r2 / g2[C], &m[2], &m[3]);
	ring_pair(M, D, r3 % g2[D], &m[4], &m[5]);
	ring_pair(M, static_cast<s16>(E - D), r3 / g2[D], &m[6], &m[7]);
	/* each symbol: its ring's group, u - 1 bits within it (the last one
	   0 on a robbed symbol), and a sign: a data bit, or every r-th
	   symbol against the running sum */
	u32 qm = (1u << q) - 1;
	for (i = 0; i < 8; i++) {
		bool rb = rob[8 * f + i], pos;
		u32 p = static_cast<u32>(ring[m[i]]) << q;
		if (shaping_next()) {
			pos = rds <= 0;
			p |= (rb ? U << 1 : U) & qm;
			U >>= rb ? q - 1 : q;
		} else {
			pos = U & 1;
			p |= (rb ? U & ~1u : U >> 1) & qm;
			U >>= rb ? q : q + 1;
		}
		code[i] = static_cast<unsigned char>(bios[p] | (pos ? 0x80 : 0));
		sent(code[i]);
	}
	half ^= 1;
}

/* the decoder for line l; -1 for a rate or pad that K56flex does not
   have */
s16 K56Dec::init(const K56Line &l)
{
	s16 rv = K56Codec::init(l);
	memset(inv, 0, sizeof inv);
	memset(inv_ring, 0, sizeof inv_ring);
	errors = 0;
	if (rv)
		return rv;
	for (s16 p = 0; p < M << q; p++)
		inv[bios[p]] = static_cast<unsigned char>(p + 1);
	for (s16 k = 0; k < M; k++)
		inv_ring[ring[k]] = static_cast<unsigned char>(k);
	return 0;
}

/* the index within the pair of sum s whose first ring is a (9.3/V.34) */
static u32 pair_index(s16 M, s16 a, s16 b)
{
	return static_cast<u32>(a + b < M ? a : M - 1 - b);
}

/* a frame: 8 received codes to data bits; returns how many */
s16 K56Dec::frame(const unsigned char code[8], unsigned char *bits)
{
	s16 nb = static_cast<s16>(rate / 1000 -
				  (nrob == 3 ? nrob_f[f == 2 ? 0 : f + 1] : loss));
	next_frame();
	s16 adj, ks = shell_bits(&adj), m[8], i, nu = 0;
	u32 U = 0, qm = (1u << q) - 1;
	for (i = 0; i < 8; i++) {
		bool rb = rob[8 * f + i], pos = code[i] & 0x80;
		s16 p = static_cast<s16>(inv[code[i] & 0x7f] - 1);
		/* a robbed symbol's point is even, its code's last bit lost */
		if (rb && (p < 0 || p & 1)) {
			s16 p1 = static_cast<s16>(inv[(code[i] & 0x7f) ^ 1] - 1);
			p = p1 >= 0 && !(p1 & 1) ? p1 : static_cast<s16>(p & ~1);
		}
		if (p < 0) {
			errors++;
			p = 0;
		}
		m[i] = inv_ring[p >> q];
		u32 w = static_cast<u32>(p) & qm;
		if (shaping_next()) {
			if (rb) {
				U |= (w >> 1) << nu;
				nu = static_cast<s16>(nu + q - 1);
			} else {
				U |= w << nu;
				nu = static_cast<s16>(nu + q);
			}
		} else {
			if (rb) {
				U |= (w | (pos ? 1u : 0u)) << nu;
				nu = static_cast<s16>(nu + q);
			} else {
				U |= (w << 1 | (pos ? 1u : 0u)) << nu;
				nu = static_cast<s16>(nu + q + 1);
			}
		}
		sent(code[i]);
	}
	/* the shell index back from the rings */
	s16 C = static_cast<s16>(m[0] + m[1]), C2 = static_cast<s16>(m[2] + m[3]);
	s16 D = static_cast<s16>(m[4] + m[5]), D2 = static_cast<s16>(m[6] + m[7]);
	s16 B = static_cast<s16>(C + C2), E = static_cast<s16>(D + D2);
	s16 A = static_cast<s16>(B + E), k;
	u32 r2 = pair_index(M, m[2], m[3]) * g2[C] + pair_index(M, m[0], m[1]);
	for (k = 0; k < C; k++)
		r2 += g2[B - k] * g2[k];
	u32 r3 = pair_index(M, m[6], m[7]) * g2[D] + pair_index(M, m[4], m[5]);
	for (k = 0; k < D; k++)
		r3 += g2[E - k] * g2[k];
	u32 r1 = r3 * g4[B] + r2;
	for (k = 0; k < B; k++)
		r1 += g4[A - k] * g4[k];
	u32 S = z8[A] + r1;
	if (S & ((1u << adj) - 1) || (S >> adj) >> ks)
		errors++;
	u64 w = (static_cast<u64>(S >> adj) & ((1ULL << ks) - 1)) |
		static_cast<u64>(U) << ks;
	for (i = 0; i < nb; i++)
		bits[i] = static_cast<unsigned char>(
			descramble(&scr, SCR_GPA, static_cast<s16>((w >> i) & 1)));
	half ^= 1;
	return nb;
}

} // namespace dsp
