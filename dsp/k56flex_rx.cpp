/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_rx.cpp - K56flex's analogue modem: the PCM receiver
 *                     (k56flex_rx.h, docs/dsp/k56flex-details.md)
 *
 *    R repeats + + - - (2000 Hz): the line has x(n) close to x(n - 4)
 *    and to -x(n - 2).  Its reversal, 24 symbols before TRN1A, is where
 *    for 4 samples x(n) is -x(n - 4).  From there the digital modem's
 *    training is K56Train's from the same place, so the equaliser
 *    (V.90's analogue modem's: T/2, solved by least squares, then
 *    NLMS, with its timing loop) trains on what is known up to Js.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstring>

#include "dm_types.h"
#include "bits.h"
#include "dsp.h"
#include "k56flex_rx.h"
#include "v90.h"

namespace dsp {

#define R_MIN_RUN       64
#define R_MIN_RMS       30.0
#define EQ_D            K56RX_EQ_D
/* the symbols from R's 265th: TRN1A, Js, the silence after it (its
   least length) */
#define K_TRN1A         24
#define K_JS            (K_TRN1A + 16368 + 8448 + 8448)
#define K_SIL2          (K_JS + 128)
#define SIL2_MIN        12288
#define REFINE_N        96
#define REFINE_D        20

/* see k56flex_rx.h */
void K56Rx::init(s16 alaw_, s16 rate)
{
	reinit(*this);
	alaw = alaw_;
	for (s16 i = 0; i < 128; i++)
		lin[i] = v90_ucode_linear(alaw_, i);
	level = static_cast<float>(lin[0x4f]);
	ip.init(rate);
	stage = WAIT_R;
	dip = 1;
	k_js = K_JS;
	k_sil2 = K_SIL2;
	k_r2 = k_trn2a = k_trn2b = k_mp = k_data = -1;
	mp_last = -1000;
}

/* a code's value at our receiver, as the digital modem sends it */
float K56Rx::value(unsigned char code) const
{
	float v = static_cast<float>(lin[code & 0x7f]);
	return code & 0x80 ? v : -v;
}

/* R's sums with the newest 8 kHz sample v; R there R_MIN_RUN
   samples, then its reversal (as V90Rx's Sd, at a lag of 4) */
void K56Rx::r_detect(float v)
{
	s32 i = n8 - 1;
	if (!rev.add(v, i, [this](s32 j) { return at8(j); }))
		return;
	bool there = rev.there(R_MIN_RMS);
	double r = rev.r(), q = rev.q();
	if (r_run < R_MIN_RUN) {
		r_run = there ? static_cast<s16>(r_run + 1) : 0;
		if (r_run == R_MIN_RUN)
			DSP_DBG("k56rx: R at sample %" PRId32 "\n", i);
		return;
	}
	if (!r_dip && q < 0.8) {
		r_dip = 1;
		dip = 1;
	}
	if (r_dip && r < dip) {
		dip = r;
		dip_at = i;
	}
	if (r_dip && q > 0.9) {
		if (dip > -0.2) {
			r_dip = 0;
			dip = 1;
			return;
		}
		/* the 4 samples after the reversal end at dip_at */
		b = 2 * (dip_at - 3);
		k = 0;
		stage = TRAIN;
		ev_r = 1;
		ref.init(alaw);
		ref.segment(K56_R1);
		for (s16 j = 0; j < 264; j++)
			ref.next();
		DSP_DBG("k56rx: R's reversal at sample %" PRId32 " (%.2f)\n",
			b, dip);
		return;
	}
	if (r_dip)
		return;
	if (!there && --r_run < R_MIN_RUN / 2) {
		r_run = 0;
		DSP_DBG("k56rx: R lost\n");
	}
}

/* the clock's offset fitted (PcmEqualizer::timing_step()) */
void K56Rx::timing_locked(double) const
{
	DSP_DBG("k56rx: timing %+.2f ppm\n", ppm());
}

/* the delay centred: the shift of b that correlates best with TRN1A's
   first REFINE_N values (either polarity) */
void K56Rx::refine()
{
	double best = -1;
	s16 d, j, bd = 0;
	for (d = -REFINE_D; d <= REFINE_D; d++) {
		double cc = 0;
		for (j = 0; j < REFINE_N; j++)
			cc += trn0[j] * at(b + 2 * (K_TRN1A + j) + d);
		if (fabs(cc) > best) {
			best = fabs(cc);
			bd = d;
		}
	}
	b += bd;
	refined = 1;
	DSP_DBG("k56rx: TRN1A's delay %+d samples\n", bd);
}

/* a descrambled bit of TRN2B on: the last 88 an MP frame (V.34's type
   0, its sync on a symbol's first bit)?  The data follow 12 symbols
   after MP''s 16 frames (704 symbols), which follow MP's 16 */
void K56Rx::mp_bit(s16 bit)
{
	memmove(mpw, mpw + 1, 87);
	mpw[87] = static_cast<unsigned char>(bit);
	mp_bits++;
	if (mp_bits < 88 || mpw[17])
		return;
	for (s16 i = 0; i < 17; i++)
		if (!mpw[i])
			return;
	V34Mp m;
	s32 start = mp_bits - 88;
	if (start & 1 || m.parse(mpw, 88))
		return;
	s32 at = k_trn2b + start / 2;
	if (k_mp < 0) {
		mp = m;
		k_mp = at;
		k_data = at + 704 + 704 + 12;
		ev_mp = 1;
		DSP_DBG("k56rx: MP at symbol %" PRId32 ": rates %d/%d, mask "
			"%#x\n", at - k_trn2b, m.rate_ca, m.rate_ac,
			m.rate_mask);
		bad = m.rate_ac < 1 || m.rate_ac > K56_NRATES ||
			dec.init({ 30000 + 2000 * m.rate_ac, 0, alaw, 0 });
		if (!bad) {
			/* the rate's magnitudes, rising */
			nmags = 0;
			for (s16 i = 0; i < 128; i++)
				if (dec.inv[i])
					mags[nmags++] = static_cast<unsigned char>(i);
			dec_on = 1;
		}
	}
	if (m.ack && !ev_mpack) {
		ev_mpack = 1;
		/* the first MP' after the last MP: MP''s first */
		if (at - 44 == mp_last)
			k_data = at + 704 + 12;
		DSP_DBG("k56rx: MP' at symbol %" PRId32 "\n", at - k_trn2b);
	}
	if (!m.ack)
		mp_last = at;
}

/* a data mode symbol: the rate's nearest code */
void K56Rx::data(float y)
{
	if (!dec_on) {
		timing_free();
		return;
	}
	float ay = fabsf(y);
	s16 lo = 0, hi = static_cast<s16>(nmags - 1);
	while (hi - lo > 1) {
		s16 mid = static_cast<s16>((lo + hi) / 2);
		if (lin[mags[mid]] <= ay)
			lo = mid;
		else
			hi = mid;
	}
	s16 i = fabsf(ay - lin[mags[lo]]) <= fabsf(ay - lin[mags[hi]]) ?
		lo : hi;
	unsigned char m = mags[i];
	float a = y > 0 ? lin[m] : -static_cast<float>(lin[m]);
	float e = a - y;
	adapt(k, e);
	timing(k, y, a);
	dpow += 0.001f * (a * a - dpow);
	de_mse += 0.001f * (e * e - de_mse);
	code[fpos++] = static_cast<unsigned char>((y > 0 ? 0x80 : 0) | m);
	if (fpos < 8)
		return;
	fpos = 0;
	unsigned char bits[K56_MAXBITS];
	s16 nb = dec.frame(code, bits);
	/* (B1: 6 frames of ones first) */
	if (frames++ < 6)
		return;
	for (s16 j = 0; j < nb && qn < K56RX_Q; j++) {
		q[(qh + qn) % K56RX_Q] = bits[j];
		qn++;
	}
}

/* see k56flex_rx.h */
s16 K56Rx::take(unsigned char *bo, s16 nmax)
{
	s16 m = 0;
	while (m < nmax && qn) {
		bo[m++] = q[qh];
		qh = static_cast<s16>((qh + 1) % K56RX_Q);
		qn--;
	}
	return m;
}

/* see k56flex_rx.h */
void K56Rx::line(float v)
{
	float o[3];
	s16 m = ip.run(v, o);
	for (s16 i = 0; i < m; i++)
		sample(o[i]);
}

/* one sample at 16 000 Hz: R's detector, then the symbols */
void K56Rx::sample(float v)
{
	static const unsigned char mag2[4] = { 0x2f, 0x45, 0x51, 0x57 };
	x[n & (K56RX_HIST - 1)] = v;
	n++;
	if (stage == WAIT_R) {
		if (n & 1) {
			x8[n8 & (K56RX_HIST8 - 1)] = v;
			n8++;
			r_detect(v);
		}
		return;
	}
	while (b + 2 * k + EQ_D <= n - 1) {
		float a = 0, y;
		if (k < K_JS) {
			a = value(ref.next());
			if (k >= K_TRN1A && k < K_TRN1A + REFINE_N)
				trn0[k - K_TRN1A] = a;
		}
		if (k == K_TRN1A + REFINE_N && !refined)
			refine();
		if (stage == TRAIN) {
			if (k > K_TRN1A + REFINE_N) {
				ls_add(k, a);
				if (++ls2_n == K56RX_LS) {
					solve();
					ls2_n = 0;
					stage = RUN;
					ev_trained = 1;
					t_on = 1;
					t_pow = static_cast<double>(level) * level;
					DSP_DBG("k56rx: equaliser solved at "
						"symbol %" PRId32 "\n", k);
				}
			}
			k++;
			continue;
		}
		y = equalize(k);
		if (k < K_JS) {
			/* known: once the timing is locked, solved again */
			if (t_n >= TIM_LOCK && ls2_n < K56RX_LS) {
				if (!ls2_n) {
					zero(R);
					zero(p);
				}
				ls_add(k, a);
				if (++ls2_n == K56RX_LS) {
					solve();
					y = equalize(k);
					DSP_DBG("k56rx: equaliser solved again at "
						"symbol %" PRId32 ", timing %+.2f "
						"ppm\n", k, ppm());
				}
			}
			if (k == K_JS - 1)
				dscr = ref.scr;
		} else if (k < K_SIL2) {
			/* Js: J's bits on the even symbols' signs */
			a = y > 0 ? level : -level;
			if (!((k - K_JS) & 1)) {
				s16 d = descramble(&dscr, SCR_GPA, y > 0);
				js_sr = static_cast<u16>(js_sr >> 1 | d << 15);
				if (++js_n == 16)
					js = js_sr;
				if (js_n == 32) {
					ev_js = 1;
					DSP_DBG("k56rx: Js %#06x%s\n", js,
						js == js_sr ? "" :
						" (the next one differs)");
				}
			}
		} else if (k_r2 < 0 || k < k_r2) {
			/* the silence until R: on grid of 8 from its start */
			if (k_r2 < 0 && k >= K_SIL2 + SIL2_MIN - 8 &&
			    fabsf(y) > 0.5f * lin[0x54]) {
				k_r2 = K_SIL2 + (k - K_SIL2 + 4) / 8 * 8;
				k_trn2a = k_r2 + 288;
				k_trn2b = k_trn2a + 16336 + 32;
				r2.init(alaw);
				r2.segment(K56_R2);
				for (s32 j = k_r2; j < k; j++)
					r2.next();
				ev_r2 = 1;
				DSP_DBG("k56rx: R after %" PRId32 " symbols of "
					"silence\n", k_r2 - K_SIL2);
			}
			if (k_r2 < 0 || k < k_r2) {
				timing_free();
				k++;
				continue;
			}
		}
		if (k_r2 >= 0 && k >= k_r2 && k < k_trn2a) {
			a = value(r2.next());
		} else if (k_trn2a >= 0 && k >= k_trn2a && k < k_trn2b) {
			/* TRN2A and MARK: the even symbols' signs scrambled
			   ones, then zeros */
			a = y > 0 ? level : -level;
			if (!((k - k_trn2a) & 1)) {
				s16 d = descramble(&dscr, SCR_GPA, y > 0);
				ones = d ? static_cast<s16>(ones + 1) : 0;
				if (ones == 32 && k < k_trn2b - 32) {
					ev_trn2a = 1;
					DSP_DBG("k56rx: TRN2A\n");
				}
			}
		} else if (k_trn2b >= 0 && k >= k_trn2b &&
			   (k_data < 0 || k < k_data)) {
			/* TRN2B, MP, MP', E: 2 bits a symbol in 4
			   magnitudes */
			float ay = fabsf(y), be = 1e30f;
			s16 j, mi = 0;
			for (j = 0; j < 4; j++) {
				float d = fabsf(ay - lin[mag2[j]]);
				if (d < be) {
					be = d;
					mi = j;
				}
			}
			a = y > 0 ? lin[mag2[mi]] : -static_cast<float>(lin[mag2[mi]]);
			mp_bit(descramble(&dscr, SCR_GPA, mi & 1));
			mp_bit(descramble(&dscr, SCR_GPA, mi >> 1));
		} else if (k_data >= 0 && k >= k_data) {
			if (!ev_data) {
				ev_data = 1;
				DSP_DBG("k56rx: data from symbol %" PRId32
					", equaliser mse %.4f\n", k, mse);
			}
			data(y);
			k++;
			continue;
		}
		{
			float e = a - y;
			adapt(k, e);
			timing(k, y, a);
			err_sum += static_cast<double>(e) * e;
			if (++err_n == 4096) {
				mse = static_cast<float>(
					err_sum / err_n /
					(static_cast<double>(level) * level));
				DSP_DBG("k56rx: equaliser mse %.5f (%.1f dB) at "
					"symbol %" PRId32 ", timing %+.2f ppm\n",
					mse, -10 * log10(mse + 1e-12), k, ppm());
				err_sum = 0;
				err_n = 0;
			}
		}
		k++;
	}
}

} // namespace dsp
