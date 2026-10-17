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
	memset(this, 0, sizeof(*this));
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
		if (seg == V92A_RU && n + 1 == 384) {
			segment(V92A_RUBAR);
			return y;
		}
		if (seg == V92A_RUBAR && n + 1 == 24) {
			segment(V92A_TRN1);
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
			segment(V92A_SIL);
			next = 1;
			return y;
		}
		break;
	}
	n++;
	return y;
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
