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
#include "env.h"
#include "k56flex_rx.h"
#include "v90.h"

namespace dsp {

#define R_MIN_RUN       64
#define R_MIN_RMS       30.0
#define EQ_D            K56RX_EQ_D
/* the symbols from R's 265th: TRN1A, Js, the silence after it (its
   least length) */
#define K_TRN1A         24
#define K_TRN1B         (K_TRN1A + 16368)
#define K_TRN1C         (K_TRN1B + 8448)
#define K_JS            (K_TRN1C + 8448)
/* TRN1B's symbols left out of its means (the equaliser settles) */
#define RB_SKIP         240
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
	{
		K56Train r;
		r.init(alaw_);
		r.segment(K56_R2);
		for (s16 j = 0; j < 16; j++)
			r2c[j] = r.next();
	}
}

/* a code's value at our receiver, as the digital modem sends it */
float K56Rx::value(unsigned char code) const
{
	float v = static_cast<float>(lin[code & 0x7f]);
	return code & 0x80 ? v : -v;
}

/* a code's value at our receiver, as it comes after the pad and the
   robbed bits that TRN1B found (symbol kk) */
float K56Rx::expect(unsigned char code, s32 kk) const
{
	if (!det)
		return value(code);
	float v = rv[code & 0x80 ? 0 : 1][(kk - K_TRN1B) % 24][code & 0x7f];
	return code & 0x80 ? v : -v;
}

/* TRN1B's means to the pad and the robbed bits, as SM56's
   MOD_Det_DL_RBS: at each symbol of the cycle the model (pad, a, b)
   whose 6 values fit the means best, each with its own gain; the pad
   the one most symbols fit; then each symbol's model with that pad.
   A symbol is robbed when its model is not (2, 2), symbol k of the 6
   symbol cycle when one of its 4 in the 24 is */
void K56Rx::rbs_detect()
{
	static const unsigned char mag3[6] = {
		0x40, 0x41, 0x4b, 0x4c, 0x50, 0x51
	};
	float t[3][3][4][6];
	s16 pd, a, bb, p, j;
	for (pd = 0; pd < 3; pd++)
		for (a = 1; a < 3; a++)
			for (bb = 0; bb < 4; bb++)
				for (j = 0; j < 6; j++)
					t[pd][a][bb][j] = k56_rbs_value(
						lin, alaw, mag3[j], pd, a, bb);
	double m[24][6];
	for (p = 0; p < 24; p++)
		for (j = 0; j < 6; j++)
			m[p][j] = rb_n[p][j] ? rb_sum[p][j] / rb_n[p][j] : 0;
	/* (the models SM56 has: no a = 1 without a pad, nor with b = 0) */
	auto fit = [&](s16 pp, s16 fa, s16 fb, s16 sym, double *g) {
		double mt = 0, tt = 0, e = 0;
		for (s16 i = 0; i < 6; i++) {
			mt += m[sym][i] * t[pp][fa][fb][i];
			tt += static_cast<double>(t[pp][fa][fb][i]) *
				t[pp][fa][fb][i];
		}
		*g = tt > 0 ? mt / tt : 0;
		for (s16 i = 0; i < 6; i++) {
			double d = m[sym][i] - *g * t[pp][fa][fb][i];
			e += d * d;
		}
		return e;
	};
	auto allowed = [](s16 pp, s16 fa, s16 fb) {
		return fa == 2 || (pp > 0 && fb > 0);
	};
	s16 votes[3] = { 0, 0, 0 };
	for (p = 0; p < 24; p++) {
		double be = 1e300, g;
		s16 bp = 0;
		for (pd = 0; pd < 3; pd++)
			for (a = 1; a < 3; a++)
				for (bb = 0; bb < 4; bb++) {
					if (!allowed(pd, a, bb))
						continue;
					double e = fit(pd, a, bb, p, &g);
					if (e < be) {
						be = e;
						bp = pd;
					}
				}
		votes[bp]++;
	}
	pad = votes[1] > votes[0] ? (votes[2] > votes[1] ? 2 : 1) :
		(votes[2] > votes[0] ? 2 : 0);
	double gs = 0;
	robbed = 0;
	for (p = 0; p < 24; p++) {
		double be = 1e300, g, bg = 1;
		rb_a[p] = rb_b[p] = 2;
		for (a = 1; a < 3; a++)
			for (bb = 0; bb < 4; bb++) {
				if (!allowed(pad, a, bb))
					continue;
				double e = fit(pad, a, bb, p, &g);
				/* (the clean model the one a tie goes to) */
				if (e < be * (a == 2 && bb == 2 ? 1.05 : 1)) {
					be = e;
					bg = g;
					rb_a[p] = a;
					rb_b[p] = bb;
				}
			}
		gs += bg;
		if (rb_a[p] != 2 || rb_b[p] != 2)
			robbed = static_cast<unsigned char>(robbed | 1 << (p % 6));
	}
	gain = static_cast<float>(gs / 24);
	j_rbs = static_cast<unsigned char>(pad << 6 | robbed);
	/* the equaliser's output to the codes' values as they come */
	if (gain > 0)
		for (s16 i = 0; i < K56RX_TAPS; i++)
			c[i] /= gain;
	for (s16 ng = 0; ng < 2; ng++)
		for (p = 0; p < 24; p++)
			for (j = 0; j < 128; j++)
				rv[ng][p][j] = k56_rbs_value(
					lin, alaw, static_cast<unsigned char>(j),
					pad, rb_a[p], rb_b[p], ng);
	det = 1;
	DSP_DBG("k56rx: TRN1B: pad %d dB, robbed bits %#04x (J's octet "
		"%#04x), gain %.3f\n", 3 * pad, robbed, j_rbs, gain);
}

/* see k56flex_rx.h.  A rate if 91.4 times 0.675 times the error's power
   is below the square of its least distance between two codes' values
   as they come (SM56's MOD_rate_det; its distances in 14 bit units for
   mu-law, 13 for A-law).  SM56 has them in tables for a clean line; here
   they are the codes' values at each symbol (rv: the pad and the robbed
   bits), a code and its last bit flipped the same at a robbed symbol
   (the decoder takes them so) */
double K56Rx::dmin(s16 i) const
{
	double u = alaw ? 8 : 4;
	K56Dec d;
	if (d.init({ 32000 + 2000 * i, static_cast<s16>(det ? pad : 0), alaw,
		     det ? robbed : static_cast<unsigned char>(0) }))
		return 0;
	double dm = 1e30;
	for (s16 ng = 0; ng < (det ? 2 : 1); ng++)
		for (s16 p = 0; p < (det ? 24 : 1); p++) {
			bool rob = det && (robbed >> (p % 6)) & 1;
			float last = -1;
			s16 lm = -1;
			for (s16 m = 0; m < 128; m++) {
				if (!d.inv[m])
					continue;
				float v = det ? rv[ng][p][m] : lin[m];
				if (lm >= 0 && !(rob && (lm ^ m) == 1)) {
					double g = (v - last) / u;
					if (g < dm)
						dm = g;
				}
				last = v;
				lm = m;
			}
		}
	return dm;
}

/* see k56flex_rx.h */
double K56Rx::data_err() const
{
	double u = alaw ? 8 : 4;
	return static_cast<double>(de_mse) / (u * u);
}

/* see k56flex_rx.h */
s16 K56Rx::max_rate() const
{
	double u = alaw ? 8 : 4;
	double e = static_cast<double>(mse) * level * level / (u * u);
	for (s16 i = K56_NRATES - 1; i >= 0; i--) {
		/* (SM56: A-law with a 3 dB pad 36000 at most) */
		if (alaw && det && pad == 1 && i > 2)
			continue;
		double dm = dmin(i);
		if (91.4 * 0.675 * e < dm * dm && dm >= 2)
			return static_cast<s16>(i + 1);
	}
	return 0;
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
	/* (2 bits a symbol, 1 in a renegotiation: its E 24 symbols) */
	s16 nb = renegs ? 1 : 2;
	s32 e_len = renegs ? 24 : 12;
	s32 start = mp_bits - 88;
	if (start % nb || m.parse(mpw, 88))
		return;
	s32 at = k_trn2b + start / nb;
	if (k_mp < 0) {
		mp = m;
		k_mp = at;
		k_data = at + 704 + 704 + e_len;
		ev_mp = 1;
		DSP_DBG("k56rx: MP at symbol %" PRId32 ": rates %d/%d, mask "
			"%#x\n", at - k_trn2b, m.rate_ca, m.rate_ac,
			m.rate_mask);
		bad = m.rate_ac < 1 || m.rate_ac > K56_NRATES ||
			dec.init({ 30000 + 2000 * m.rate_ac, pad, alaw,
				   robbed });
		if (!bad) {
			/* the error each rate allows (SM56's rule, as
			   max_rate) */
			for (s16 i = 0; i < K56_NRATES; i++) {
				double dm = dmin(i);
				lim[i] = dm >= 2 && !(alaw && det && pad == 1 &&
						      i > 2) ?
					dm * dm / (91.4 * 0.675) : 0;
			}
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
		if (at - 88 / nb == mp_last)
			k_data = at + 704 + e_len;
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
	/* (the values as they come at this symbol: pad and robbed bits;
	   they rise with the magnitude) */
	const float *rl = det ? rv[y > 0 ? 0 : 1][(k - K_TRN1B) % 24] :
		nullptr;
	auto val = [&](unsigned char mg) {
		return rl ? rl[mg] : static_cast<float>(lin[mg]);
	};
	float ay = fabsf(y);
	s16 lo = 0, hi = static_cast<s16>(nmags - 1);
	while (hi - lo > 1) {
		s16 mid = static_cast<s16>((lo + hi) / 2);
		if (val(mags[mid]) <= ay)
			lo = mid;
		else
			hi = mid;
	}
	s16 i = fabsf(ay - val(mags[lo])) <= fabsf(ay - val(mags[hi])) ?
		lo : hi;
	unsigned char m = mags[i];
	float a = y > 0 ? val(m) : -val(m);
	float e = a - y;
	adapt(k, e);
	timing(k, y, a);
	dpow += 0.001f * (a * a - dpow);
	de_mse += 0.001f * (e * e - de_mse);
	dy[k & 15] = y;
	code[fpos++] = static_cast<unsigned char>((y > 0 ? 0x80 : 0) | m);
	if (fpos < 8)
		return;
	fpos = 0;
	/* R from a frame's start, two frames of it: the digital modem
	   renegotiates (or answers our S) */
	if (frames >= 2) {
		s32 s0 = k - 15;
		s16 j;
		for (j = 0; j < 16; j++) {
			float ex = expect(r2c[j], s0 + j);
			if (fabsf(dy[(s0 + j) & 15] - ex) > 0.25f * fabsf(ex))
				break;
		}
		if (j == 16) {
			reneg_r(s0);
			return;
		}
	}
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

/* R in data mode from symbol s (k now): R2, TRN2A, MARK and TRN2B
   from there as after the silence, the MP and the data again */
void K56Rx::reneg_r(s32 s)
{
	k_r2 = s;
	k_trn2a = k_r2 + 288;
	/* (TRN2A's length the digital modem's: TRN2B after MARK) */
	k_trn2b = INT32_C(0x3fffffff);
	zeros = 0;
	k_mp = k_data = -1;
	r2.init(alaw);
	r2.segment(K56_R2);
	for (s32 j = k_r2; j <= k; j++)
		r2.next();
	ones = 0;
	ev_trn2a = 0;
	mp_bits = 0;
	mp_last = -1000;
	ev_mp = ev_mpack = ev_data = 0;
	bad = 0;
	dec_on = 0;
	frames = 0;
	fpos = 0;
	err_sum = 0;
	err_n = 0;
	ev_reneg_r = 1;
	renegs++;
	DSP_DBG("k56rx: R in data mode at symbol %" PRId32 " (renegotiation "
		"%" PRId32 "), error %.2f\n", s, renegs, data_err());
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
		unsigned char rc = 0;
		bool hold = false;
		if (k < K_JS) {
			rc = ref.next();
			a = expect(rc, k);
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
			/* TRN1B: the means of its codes at each symbol of
			   the cycle, the equaliser held (its errors are the
			   pad's and the robbed bits') */
			if (k >= K_TRN1B && k < K_TRN1C) {
				hold = true;
				if (k >= K_TRN1B + RB_SKIP) {
					s16 mg = rc & 0x7f, j = -1;
					switch (mg) {
					case 0x40: j = 0; break;
					case 0x41: j = 1; break;
					case 0x4b: j = 2; break;
					case 0x4c: j = 3; break;
					case 0x50: j = 4; break;
					case 0x51: j = 5; break;
					}
					s16 pp = static_cast<s16>((k - K_TRN1B) % 24);
					if (j >= 0) {
						rb_sum[pp][j] += rc & 0x80 ? y : -y;
						rb_n[pp][j]++;
					}
				}
				if (k == K_TRN1C - 1 &&
				    !env_int("DSP_K56_NORBS", 0))
					rbs_detect();
			}
			if (k == K_JS - 1)
				dscr = ref.scr;
		} else if (k < K_SIL2) {
			/* Js: J's bits on the even symbols' signs */
			a = expect(static_cast<unsigned char>(
					   (y > 0 ? 0x80 : 0) | 0x4f), k);
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
			a = expect(r2.next(), k);
		} else if (k_trn2a >= 0 && k >= k_trn2a && k < k_trn2b) {
			/* TRN2A and MARK: the even symbols' signs scrambled
			   ones, then zeros */
			a = expect(static_cast<unsigned char>(
					   (y > 0 ? 0x80 : 0) | 0x4f), k);
			if (!((k - k_trn2a) & 1)) {
				s16 d = descramble(&dscr, SCR_GPA, y > 0);
				ones = d ? static_cast<s16>(ones + 1) : 0;
				if (ones == 32 && k < k_trn2b - 32) {
					ev_trn2a = 1;
					DSP_DBG("k56rx: TRN2A\n");
				}
				/* a renegotiation's: MARK's 16 zeros after
				   the ones, TRN2B next */
				zeros = !d && ev_trn2a ?
					static_cast<s16>(zeros + 1) : 0;
				if (renegs && zeros == 16 &&
				    k_trn2b == INT32_C(0x3fffffff)) {
					k_trn2b = k + 2;
					DSP_DBG("k56rx: MARK, TRN2B at symbol %"
						PRId32 " (TRN2A %" PRId32 ")\n",
						k_trn2b, k_trn2b - 32 - k_trn2a);
				}
			}
		} else if (k_trn2b >= 0 && k >= k_trn2b &&
			   (k_data < 0 || k < k_data)) {
			/* TRN2B, MP, MP', E: 2 bits a symbol in 4
			   magnitudes; in a renegotiation MP, MP' and E 1
			   bit in 2 (the reference the nearest of all 6) */
			/* (the pad's magnitudes, as they come) */
			const unsigned char *m2 = k56_trn2b_mag[det ? pad : 0];
			const unsigned char *m1 = k56_trn2b_mag1[det ? pad : 0];
			float ay = fabsf(y), be = 1e30f, be1 = 1e30f;
			s16 j, mi = 0, mi1 = 0;
			unsigned char mg = m2[0];
			for (j = 0; j < 6; j++) {
				unsigned char c = j < 4 ? m2[j] : m1[j - 4];
				float d = fabsf(ay - expect(
					static_cast<unsigned char>(0x80 | c), k));
				if (j < 4 && d < be) {
					be = d;
					mi = j;
				}
				if (j >= 4 && d < be1) {
					be1 = d;
					mi1 = static_cast<s16>(j - 4);
				}
			}
			if (!renegs)
				mg = m2[mi];
			else
				mg = be <= be1 ? m2[mi] : m1[mi1];
			a = expect(static_cast<unsigned char>(
					   (y > 0 ? 0x80 : 0) | mg), k);
			if (renegs) {
				mp_bit(descramble(&dscr, SCR_GPA, mi1));
			} else {
				mp_bit(descramble(&dscr, SCR_GPA, mi & 1));
				mp_bit(descramble(&dscr, SCR_GPA, mi >> 1));
			}
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
		if (hold) {
			timing(k, y, a);
		} else {
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
