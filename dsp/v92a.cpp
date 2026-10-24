/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v92a.cpp - V.92 analogue modem: the PCM upstream transmitter
 *               (see v92a.h)
 *
 */

#include <math.h>
#include <string.h>

#include "bits.h"
#include "dsp.h"
#include "v92a.h"

namespace dsp {

/* see v92a.h: the pulse a Blackman windowed sinc over V92A_SPAN symbols
   either side, tabulated by symbol from -V92A_SPAN */
void V92ATx::init(int rate_, float lu_)
{
	int i;
	reinit(*this);
	rate = rate_;
	lu = lu_;
	for (i = 0; i <= 2 * V92A_SPAN * V92A_P; i++) {
		double u = (double)i / V92A_P - V92A_SPAN;
		double x = M_PI * u / V92A_SPAN;
		double h = fabs(u) < 1e-12 ? 1 : sin(M_PI * u) / (M_PI * u);
		pulse[i] = (float)(h * (0.42 + 0.5 * cos(x) +
					0.08 * cos(2 * x)));
	}
	pulse[2 * V92A_SPAN * V92A_P + 1] = 0;
	set_ppm(0);
	seg = V92A_SIL;
}

/* see v92a.h */
void V92ATx::set_ppm(double ppm)
{
	T = rate / 8000.0 * (1 + ppm * 1e-6);
}

/* see v92a.h: Ru, Su and TRN1u restart their patterns (the first Su
   and the second TRN1u the counts of the second ones), Ja and CPt
   their bits */
void V92ATx::segment(enum v92a_seg s)
{
	DSP_DBG("v92a tx: segment %d -> %d after %ld symbols\n", seg, s, n);
	if (s == V92A_SU && seg != V92A_SUBAR) {
		su_k = 0;
		second = 0;
	}
	if (s == V92A_TRN1)
		scr = 0;
	if (s == V92A_JA || s == V92A_CPT)
		bn = 0;
	if (s == V92A_RU)
		second = 0;
	if (s == V92A_TRN2)
		scr = 0;
	if (s == V92A_SEQ)
		pos = 0;
	if (s == V92A_DATA) {
		fi = 0;
		dn = 0;
		data_reset();
	}
	seg = s;
	n = 0;
}

/* a bit scrambled (GPA) and differentially encoded: its symbol */
float V92ATx::dbit(int bit)
{
	last_d ^= scramble(&scr, SCR_GPA, bit);
	return last_d ? -lu : lu;
}

/* a symbol of TRN1u: scrambled ones, 0 positive */
float V92ATx::trn()
{
	last_d = scramble(&scr, SCR_GPA, 1);
	return last_d ? -lu : lu;
}

/* the next symbol, and the segment after it once it is the last */
float V92ATx::symbol()
{
	static const float su[6] = { 1, 0, 1, -1, 0, -1 };
	const float a15 = 1.2247449f * lu;
	float y = 0;
	int k;

	switch (seg) {
	case V92A_SIL:
		break;
	case V92A_RU:
	case V92A_RUBAR:
		k = (int)(n % 6);
		y = (k < 3) == (seg == V92A_RU) ? lu : -lu;
		/* (TRN2u's differential encoder goes on from its sign) */
		last_d = y < 0;
		if (seg == V92A_RU && n + 1 == 384) {
			segment(V92A_RUBAR);
			return y;
		}
		if (seg == V92A_RUBAR && n + 1 == 24) {
			/* a renegotiation's: TRN2u (9.8.2/V.92) */
			segment(rn ? V92A_TRN2 : V92A_TRN1);
			return y;
		}
		break;
	case V92A_TRN1:
		y = trn();
		if (!second && n + 1 == 2040) {
			segment(V92A_JA);
			return y;
		}
		if (second && trn_stop && n + 1 >= 2040 && (n + 1) % 12 == 0) {
			segment(V92A_CPT);
			return y;
		}
		break;
	case V92A_JA:
		y = dbit(bn < 24 ? 1 : bits[(bn - 24) % nbits]);
		bn++;
		if (ja_stop && bn % 12 == 0) {
			segment(V92A_SIL);
			return y;
		}
		break;
	case V92A_SU:
	case V92A_SUBAR:
		y = su[su_k % 6] * (seg == V92A_SU ? a15 : -a15);
		su_k++;
		if (seg == V92A_SU && !second && n + 1 == 144) {
			segment(V92A_SUBAR);
			return y;
		}
		if (seg == V92A_SU && second && jp_req && su_k % 12 == 0) {
			segment(V92A_SUBAR);
			return y;
		}
		if (seg == V92A_SUBAR && n + 1 == 24) {
			/* the first: half a symbol more, Su again; the
			   second: Jp's fraction, TRN1u */
			if (!second) {
				delay += 0.5;
				segment(V92A_SU);
				second = 1;
			} else {
				delay += jp_frac;
				segment(V92A_TRN1);
				second = 1;
			}
			return y;
		}
		break;
	case V92A_CPT:
		y = dbit(bn < 24 ? 1 : bits[(bn - 24) % nbits]);
		bn++;
		if (ri_seen && bn > 24 && (bn - 24) % nbits == 0) {
			segment(V92A_E1U);
			return y;
		}
		break;
	case V92A_E1U:
		y = dbit(0);
		if (n + 1 == 12) {
			segment(V92A_TRN2);
			next = 1;
			return y;
		}
		break;
	case V92A_TRN2:
		y = sym4(1, 1);
		if (trn2_stop && (n + 1) % 12 == 0) {
			segment(V92A_SEQ);
			return y;
		}
		break;
	case V92A_SEQ:
		y = sym4(bits[pos], bits[pos + 1]);
		pos += 2;
		if (pos >= nbits) {
			seqs++;
			pos = 0;
			if (e2u_req) {
				segment(V92A_E2U);
				return y;
			}
			if (nxt_n) {
				memcpy(bits, nxt, nxt_n);
				nbits = nxt_n;
				nxt_n = 0;
			}
		}
		break;
	case V92A_E2U:
		y = sym4(0, 0);
		if (n + 1 == 12 + cpd.e2u_extend) {
			segment(V92A_DATA);
			return y;
		}
		break;
	case V92A_DATA:
		if (ru_req && fi == 0) {
			ru_req = 0;
			rn = 1;
			segment(V92A_RU);
			return symbol();
		}
		return data_symbol();
	}
	n++;
	return y;
}

/* a symbol of the 4 points of TRN2u (Table 28/V.92), from two bits:
   the sign's scrambled and differentially encoded, then the
   magnitude's scrambled (3 / sqrt 5 for 1) */
float V92ATx::sym4(int sign, int mag)
{
	int m;
	last_d ^= scramble(&scr, SCR_GPA, sign);
	m = scramble(&scr, SCR_GPA, mag);
	float l = (m ? 1.3416408f : 0.4472136f) * lu;
	return last_d ? -l : l;
}

/* point e (-LC .. LC - 1, negative ones negative) of constellation c */
float V92ATx::point(int c, int e) const
{
	return e >= 0 ? (float)cpd.pts[c][e] : -(float)cpd.pts[c][-e - 1];
}

/* an unsigned integer of 4 words, least significant first: the
   modulus encoder's R and the product of the moduli (up to 2^96) */
namespace {
struct Wide {
	unsigned w[4];

	/* the value v */
	void set(unsigned v) { memset(w, 0, sizeof(w)); w[0] = v; }
	/* this times m (the carry past 2^128 lost) */
	void mul(unsigned m)
	{
		unsigned long long c = 0;
		for (int i = 0; i < 4; i++) {
			c += (unsigned long long)w[i] * m;
			w[i] = (unsigned)c;
			c >>= 32;
		}
	}
	/* this / m, the remainder returned */
	unsigned div(unsigned m)
	{
		unsigned long long r = 0;
		for (int i = 3; i >= 0; i--) {
			r = r << 32 | w[i];
			w[i] = (unsigned)(r / m);
			r %= m;
		}
		return (unsigned)r;
	}
	/* a - this */
	void rsub(const Wide &a)
	{
		long long br = 0;
		for (int i = 0; i < 4; i++) {
			long long d = (long long)a.w[i] - w[i] - br;
			br = d < 0;
			w[i] = (unsigned)d;
		}
	}
	/* -1, 0 or 1 as this is less than, equal to or more than a */
	int cmp(const Wide &a) const
	{
		for (int i = 3; i >= 0; i--)
			if (w[i] != a.w[i])
				return w[i] < a.w[i] ? -1 : 1;
		return 0;
	}
};
}

/* B1u's start (8.7.1/V.92): the scrambler, the modulus encoder, the
   trellis code, the precoder and the prefilter from zero, B1u in data
   frame interval 0 */
void V92ATx::data_reset()
{
	scr = 0;
	K = 2 * (cpd.drn + 17);
	frames = 0;
	dprev = 0;
	tstate = 0;
	dsrc = V92A_D_LINK;
	memset(uh, 0, sizeof(uh));
	memset(xh, 0, sizeof(xh));
	memset(vh, 0, sizeof(vh));
}

/* a data frame's K bits (B1u's ones the first 48 frames; in a fast
   parameter exchange the sequences', E2u's zeros, FB1u's ones; RM and
   RM' their own Ki) scrambled, through the modulus encoder (6.4.1/
   V.92) into kq[]: the sign of R (above (M - 1) / 2) differentially
   encoded, R turned over (M - 1 - R) when the last frame's was 1 */
void V92ATx::data_frame()
{
	/* Table 25 and 26/V.92: RM's Ki, 1 for M - 1 */
	static const char rm[12] = { 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0 };
	static const char rmp[12] = { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
	unsigned char b[80];
	Wide R, Mt, R2, M1;
	int j, s;

	if (b1u_next) {
		b1u_next = 0;
		cpd = cpd_new;
		e2u_req = 0;
		data_reset();
		DSP_DBG("v92a tx: FPE: B1u, %d bits a frame\n", K);
	}
	if (dsrc == V92A_D_LINK && fpe_req) {
		fpe_req = 0;
		dsrc = V92A_D_RM;
		dsrc_n = 0;
	}
	if (dsrc == V92A_D_RM) {
		/* 384 T of RM, 24 T of RM' */
		for (j = 0; j < 12; j++)
			kq[j] = (dsrc_n < 32 ? rm[j] : rmp[j]) ?
				cpd.M[j] - 1 : 0;
		if (++dsrc_n == 34) {
			/* SUVu next, from cleared scrambler and sign */
			dsrc = V92A_D_SEQ;
			dsrc_n = 0;
			pos = 0;
			scr = 0;
			dprev = 0;
			if (fpe_reset) {
				memset(uh, 0, sizeof(uh));
				memset(xh, 0, sizeof(xh));
				memset(vh, 0, sizeof(vh));
			}
		}
		return;
	}
	if (dsrc == V92A_D_SEQ) {
		for (j = 0; j < K; j++) {
			b[j] = bits[pos++];
			if (pos >= nbits) {
				seqs++;
				pos = 0;
				if (nxt_n) {
					memcpy(bits, nxt, nxt_n);
					nbits = nxt_n;
					nxt_n = 0;
				}
			}
		}
		/* (whole sequences end on a frame's end: E2u after) */
		if (e2u_req && pos == 0) {
			dsrc = V92A_D_E2U;
			dsrc_n = 0;
		}
	} else if (dsrc == V92A_D_E2U) {
		memset(b, 0, K);
		dsrc = V92A_D_FB1U;
		dsrc_n = 0;
	} else if (dsrc == V92A_D_FB1U) {
		memset(b, 1, K);
		/* B1u on the new CPd after the 48th, everything anew */
		if (++dsrc_n == 48)
			b1u_next = 1;
	} else if (frames < 48)
		memset(b, 1, K);
	else
		link.get(b, K);
	if (dsrc == V92A_D_LINK)
		frames++;
	R.set(0);
	for (j = 0; j < K; j++)
		if (scramble(&scr, SCR_GPA, b[j]))
			R.w[j >> 5] |= 1u << (j & 31);
	Mt.set(1);
	for (j = 0; j < 12; j++)
		Mt.mul((unsigned)cpd.M[j]);
	/* M - 1, and 2 R */
	M1.set(1);
	M1.rsub(Mt);
	R2 = R;
	R2.mul(2);
	s = R2.cmp(M1) > 0;
	if (dprev)
		R.rsub(M1);
	dprev ^= s;
	for (j = 0; j < 12; j++)
		kq[j] = (int)R.div((unsigned)cpd.M[j]);
}

/* a data mode symbol (6.4.2/V.92): of the class of its K (on a trellis
   frame's 4th symbol with the parity the trellis code's Y0 asks for)
   the point that keeps the precoder's output least; the precoder's
   output through the prefilter, times G, in LU */
float V92ATx::data_symbol()
{
	const short *z1 = cpd.coef[0], *p1 = cpd.coef[1];
	const short *z2 = cpd.coef[2], *p2 = cpd.coef[3];
	int c, lc, M, k = fi % 4, e, step, best = 0, i;
	double fb = 0, x, v = 0, bd = 1e300;

	/* (a new CPd comes in at a frame's start) */
	if (fi == 0)
		data_frame();
	c = cpd.con[fi % 6];
	lc = cpd.lc[c];
	M = cpd.M[fi];
	/* the past's part of the precoder's output */
	for (i = 1; i <= cpd.len[0]; i++)
		fb += z1[i - 1] / 32768.0 * uh[i - 1];
	for (i = 1; i <= cpd.len[1]; i++)
		fb += p1[i - 1] / 16384.0 * xh[i - 1];
	if (k < 3) {
		e = kq[fi];
		step = M;
	} else {
		e = 2 * kq[fi] +
			((eta[0] + eta[1] + eta[2] + (tstate & 1)) & 1);
		step = 2 * M;
	}
	/* the class's members from -LC up */
	e -= (e + lc + step - 1) / step * step;
	while (e < -lc)
		e += step;
	for (best = e; e <= lc - 1; e += step) {
		double d = fabs(point(c, e) + fb);
		if (d < bd) {
			bd = d;
			best = e;
		}
	}
	eta[k] = best;
	x = point(c, best) + fb;
	/* the prefilter */
	memmove(uh + 1, uh, (V92A_HIST - 1) * sizeof(*uh));
	memmove(xh + 1, xh, (V92A_HIST - 1) * sizeof(*xh));
	uh[0] = point(c, best);
	xh[0] = x;
	for (i = 0; i < cpd.len[2]; i++)
		v += z2[i] / 32768.0 * xh[i];
	for (i = 1; i <= cpd.len[3]; i++)
		v += p2[i - 1] / 16384.0 * vh[i - 1];
	memmove(vh + 1, vh, (V92A_HIST - 1) * sizeof(*vh));
	vh[0] = v;
	if (k == 3) {
		int l0 = v34_subset_label(2 * eta[0] + 1, 2 * eta[1] + 1);
		int l1 = v34_subset_label(2 * eta[2] + 1, 2 * eta[3] + 1);
		tstate = v34_trellis_next(16 << cpd.trellis, tstate,
					  v34_conv_in(l0, l1));
	}
	fi = (fi + 1) % 12;
	dn++;
	n++;
	return (float)(lu * cpd.g * v);
}

/* see v92a.h: the symbols whose pulses reach this sample made, then
   their sum at it */
float V92ATx::sample()
{
	double reach = V92A_SPAN * T;
	float y = 0;
	long j;

	while (tnext <= m + reach) {
		int r = (int)(nsym & (V92A_RING - 1));
		int was = next;
		a[r] = symbol();
		at[r] = tnext;
		if (next && !was)
			next_at = tnext;
		nsym++;
		tnext += T * (1 + delay);
		delay = 0;
	}
	for (j = nsym - 1; j >= 0 && j >= nsym - V92A_RING; j--) {
		int r = (int)(j & (V92A_RING - 1));
		double u = (m - at[r]) / T;
		double f;
		int i;
		if (u >= V92A_SPAN)
			break;
		if (u <= -V92A_SPAN || a[r] == 0)
			continue;
		f = (u + V92A_SPAN) * V92A_P;
		i = (int)f;
		y += a[r] * (pulse[i] + (float)(f - i) *
			     (pulse[i + 1] - pulse[i]));
	}
	m++;
	return y;
}

} // namespace dsp
