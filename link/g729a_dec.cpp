/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a_dec.cpp - G.729 Annex A's decoder (g729a.h): the parameters
 *                    to the excitation and the synthesis, the
 *                    concealment of lost frames, Annex A's postfilter
 *                    and the output's high-pass filter
 *
 */

#include "g729a.h"
#include "g729a_int.h"

namespace dlink {
namespace g729 {

/* the decoder's memory from frame to frame */
struct DecState {
	i16 old_exc[L_FRAME + PIT_MAX + L_INTERPOL];
	i16 lsp_old[M];
	i16 mem_syn[M];
	i16 sharp, old_T0, gain_code, gain_pitch;
	LspPast lsp;
	i16 prev_ma, prev_lsp[M];       /* the last good frame's LSFs */
	i16 past_qua_en[4];
	i16 seed_fer;                   /* lost frames' random codebook */
	/* Annex B's comfort noise */
	i16 seed, past_ftyp, sid_sav, sh_sid_sav, cur_gain, sid_gain;
	i16 lspSid[M];
	i16 synth_buf[L_FRAME + M];     /* the synthesis, M before it */
	/* the postfilter's */
	i16 res2_buf[PIT_MAX + L_SUBFR], scal_res2_buf[PIT_MAX + L_SUBFR];
	i16 mem_syn_pst[M];
	i16 mem_pre, past_gain;
	HighPass hp;
};

/* the LSFs decoded, or a lost frame's: the last ones again, the MA
   past moved on with what they would be coded as (4.4.1/G.729) */
static void d_lsp(DecState &s, const i16 *prm, i16 *lsp_q, bool erase)
{
	i16 lsf_q[M];
	if (!erase) {
		const i16 mode = static_cast<i16>(shr(prm[0], NC0_B) & 1);
		const i16 code0 = static_cast<i16>(prm[0] & (NC0 - 1));
		const i16 code1 = static_cast<i16>(shr(prm[1], NC1_B) & (NC1 - 1));
		const i16 code2 = static_cast<i16>(prm[1] & (NC1 - 1));
		lsp_get_quant(code0, code1, code2, mode, s.lsp, lsf_q);
		for (int i = 0; i < M; i++)
			s.prev_lsp[i] = lsf_q[i];
		s.prev_ma = mode;
	} else {
		i16 buf[M];
		for (int i = 0; i < M; i++)
			lsf_q[i] = s.prev_lsp[i];
		lsp_prev_extract(s.prev_lsp, buf, s.prev_ma, s.lsp);
		lsp_prev_update(buf, s.lsp);
	}
	lsf_lsp2(lsf_q, lsp_q);
}

/* the pitch lag from its index (4.1.3/G.729); the second subframe's
   relative to the first's T0 */
static void dec_lag3(i16 index, int i_subfr, i16 *T0, i16 *T0_frac)
{
	if (i_subfr == 0) {
		if (sub(index, 197) < 0) {
			*T0 = add(mult(add(index, 2), 10923), 19);
			const i16 i = add(add(*T0, *T0), *T0);
			*T0_frac = add(sub(index, i), 58);
		} else {
			*T0 = sub(index, 112);
			*T0_frac = 0;
		}
	} else {
		i16 T0_min = sub(*T0, 5);
		if (sub(T0_min, PIT_MIN) < 0)
			T0_min = PIT_MIN;
		const i16 T0_max = add(T0_min, 9);
		if (sub(T0_max, PIT_MAX) > 0)
			T0_min = sub(PIT_MAX, 9);
		i16 i = sub(mult(add(index, 2), 10923), 1);
		*T0 = add(i, T0_min);
		i = add(add(i, i), i);
		*T0_frac = sub(sub(index, 2), i);
	}
}

/* the fixed codevector from its positions' index and signs */
static void decod_acelp(i16 sign, i16 index, i16 *cod)
{
	i16 pos[4];
	i16 i = static_cast<i16>(index & 7);
	pos[0] = add(i, shl(i, 2));
	index = shr(index, 3);
	i = static_cast<i16>(index & 7);
	i = add(i, shl(i, 2));
	pos[1] = add(i, 1);
	index = shr(index, 3);
	i = static_cast<i16>(index & 7);
	i = add(i, shl(i, 2));
	pos[2] = add(i, 2);
	index = shr(index, 3);
	const i16 j = static_cast<i16>(index & 1);
	index = shr(index, 1);
	i = static_cast<i16>(index & 7);
	i = add(i, shl(i, 2));
	i = add(i, 3);
	pos[3] = add(i, j);
	for (int n = 0; n < L_SUBFR; n++)
		cod[n] = 0;
	for (int n = 0; n < 4; n++) {
		cod[pos[n]] = (sign & 1) ? 8191 : -8192;
		sign = shr(sign, 1);
	}
}

/* the gains from their index (4.1.5/G.729), or a lost frame's: the
   last ones attenuated (4.4.2/G.729) */
static void dec_gain(DecState &s, i16 index, const i16 *code, bool bfi)
{
	if (bfi) {
		s.gain_pitch = mult(s.gain_pitch, 29491);
		if (sub(s.gain_pitch, 29491) > 0)
			s.gain_pitch = 29491;
		s.gain_code = mult(s.gain_code, 32111);
		gain_update_erasure(s.past_qua_en);
		return;
	}
	const i16 index1 = imap1[shr(index, NCODE2_B)];
	const i16 index2 = imap2[index & (NCODE2 - 1)];
	s.gain_pitch = add(gbk1[index1][0], gbk2[index2][0]);
	i16 gcode0, exp_gcode0;
	gain_predict(s.past_qua_en, code, &gcode0, &exp_gcode0);
	const i32 gbk12 = L_add(L_deposit_l(gbk1[index1][1]),
				L_deposit_l(gbk2[index2][1]));
	const i16 tmp = extract_l(L_shr(gbk12, 1));
	i32 t = L_mult(tmp, gcode0);
	t = L_shl(t, add(negate(exp_gcode0), -12 - 1 + 1 + 16));
	s.gain_code = extract_h(t);
	gain_update(s.past_qua_en, gbk12);
}

/* the synthesis of a frame's excitation exc through A_t's two
   filters; on an overflow the excitation's past is scaled down and it
   is done again */
static void synthesis(DecState &s, i16 *exc, const i16 *Az, i16 *synth,
		      int i_subfr)
{
	overflow() = false;
	syn_filt(Az, &exc[i_subfr], &synth[i_subfr], L_SUBFR, s.mem_syn, false);
	if (overflow()) {
		for (int i = 0; i < PIT_MAX + L_INTERPOL + L_FRAME; i++)
			s.old_exc[i] = shr(s.old_exc[i], 2);
		syn_filt(Az, &exc[i_subfr], &synth[i_subfr], L_SUBFR, s.mem_syn,
			 true);
	} else {
		for (int i = 0; i < M; i++)
			s.mem_syn[i] = synth[i_subfr + L_SUBFR - M + i];
	}
}

/* Annex B's comfort noise for a SID frame (parm its parameters) or a
   frame not sent (B.4.4, B.5/G.729) */
static void dec_cng(DecState &s, const i16 *parm, bool sid, i16 *exc,
		    i16 *A_t)
{
	const bool first = s.past_ftyp == SPEECH;
	if (sid) {
		s.sid_gain = sid_gain_of(parm[3]);
		sid_lsfq_decode(parm, s.lspSid, s.lsp);
	} else if (first) {
		/* the first SID frame lost: the last speech's energy */
		i16 enerq;
		s.sid_gain = sid_gain_of(qua_sidgain(&s.sid_sav, &s.sh_sid_sav,
						     0, &enerq));
	}
	if (first) {
		s.cur_gain = s.sid_gain;
	} else {
		s.cur_gain = mult_r(s.cur_gain, 28672);
		s.cur_gain = add(s.cur_gain, mult_r(s.sid_gain, 4096));
	}
	calc_exc_rand(s.cur_gain, exc, s.seed, nullptr);
	int_qlpc(s.lsp_old, s.lspSid, A_t);
	for (int i = 0; i < M; i++)
		s.lsp_old[i] = s.lspSid[i];
}

/* a frame's end: the energy a lost first SID frame would have (Annex
   B), the excitation's past moved on */
static int frame_end(DecState &s, const i16 *exc, bool bfi, int ftyp)
{
	if (!bfi) {
		i32 t = 0;
		for (int i = 0; i < L_FRAME; i++)
			t = L_mac(t, exc[i], exc[i]);
		s.sh_sid_sav = norm_l(t);
		s.sid_sav = round16(L_shl(t, s.sh_sid_sav));
		s.sh_sid_sav = sub(16, s.sh_sid_sav);
	}
	for (int i = 0; i < PIT_MAX + L_INTERPOL; i++)
		s.old_exc[i] = s.old_exc[i + L_FRAME];
	s.past_ftyp = static_cast<i16>(ftyp);
	return ftyp;
}

/* one frame to its synthesis (4.1/G.729): the parameters after the bad
   frame indicator, as the Recommendation's prm[1..11] with the parity
   bit replaced by its check (non-zero: bad), or a SID frame's; the
   filters and lags for the postfilter; the frame's type (a lost one's
   as the last) */
static int decod_ld8a(DecState &s, i16 *parm, bool bfi, int ftyp,
		      i16 *synth, i16 *A_t, i16 *T2)
{
	i16 lsp_new[M], code[L_SUBFR];
	i16 *exc = s.old_exc + PIT_MAX + L_INTERPOL;

	if (bfi)
		ftyp = s.past_ftyp == SPEECH ? SPEECH : NONE;
	if (ftyp != SPEECH) {
		dec_cng(s, parm, ftyp == SID, exc, A_t);
		for (int i_subfr = 0; i_subfr < L_FRAME; i_subfr += L_SUBFR) {
			synthesis(s, exc, A_t + (i_subfr / L_SUBFR) * MP1, synth,
				  i_subfr);
			*T2++ = s.old_T0;
		}
		s.sharp = SHARPMIN;
		return frame_end(s, exc, bfi, ftyp);
	}
	s.seed = INIT_SEED;

	d_lsp(s, parm, lsp_new, bfi);
	parm += 2;
	int_qlpc(s.lsp_old, lsp_new, A_t);
	for (int i = 0; i < M; i++)
		s.lsp_old[i] = lsp_new[i];

	const i16 *Az = A_t;
	i16 T0 = 0, T0_frac = 0;
	for (int i_subfr = 0; i_subfr < L_FRAME; i_subfr += L_SUBFR) {
		const i16 index = *parm++;
		bool bad_pitch = bfi;
		if (i_subfr == 0)
			bad_pitch = add(static_cast<i16>(bfi), *parm++) != 0;
		if (!bad_pitch) {
			dec_lag3(index, i_subfr, &T0, &T0_frac);
			s.old_T0 = T0;
		} else {
			T0 = s.old_T0;
			T0_frac = 0;
			s.old_T0 = add(s.old_T0, 1);
			if (sub(s.old_T0, PIT_MAX) > 0)
				s.old_T0 = PIT_MAX;
		}
		*T2++ = T0;

		pred_lt_3(&exc[i_subfr], T0, T0_frac, L_SUBFR);

		if (bfi) {
			parm[0] = static_cast<i16>(random16(s.seed_fer) & 0x1fff);
			parm[1] = static_cast<i16>(random16(s.seed_fer) & 0x000f);
		}
		decod_acelp(parm[1], parm[0], code);
		parm += 2;
		const i16 j = shl(s.sharp, 1);
		if (sub(T0, L_SUBFR) < 0)
			for (int i = T0; i < L_SUBFR; i++)
				code[i] = add(code[i], mult(code[i - T0], j));

		dec_gain(s, *parm++, code, bfi);
		s.sharp = s.gain_pitch;
		if (sub(s.sharp, SHARPMAX) > 0)
			s.sharp = SHARPMAX;
		if (sub(s.sharp, SHARPMIN) < 0)
			s.sharp = SHARPMIN;

		for (int i = 0; i < L_SUBFR; i++) {
			i32 t = L_mult(exc[i + i_subfr], s.gain_pitch);
			t = L_mac(t, code[i], s.gain_code);
			t = L_shl(t, 1);
			exc[i + i_subfr] = round16(t);
		}

		synthesis(s, exc, Az, synth, i_subfr);
		Az += MP1;
	}
	return frame_end(s, exc, bfi, SPEECH);
}

/* the long-term postfilter (A.4.2.1/G.729): the residual's best integer
   lag about the decoder's, its harmonic added if strong enough */
static void pit_pst_filt(const i16 *signal, const i16 *scal_sig,
			 i16 t0_min, i16 t0_max, i16 *signal_pst)
{
	const i16 *deb_sig = &scal_sig[-t0_min];
	i32 cor_max = MIN32;
	i16 t0 = t0_min;
	for (i16 i = t0_min; i <= t0_max; i++) {
		i32 corr = 0;
		for (int j = 0; j < L_SUBFR; j++)
			corr = L_mac(corr, scal_sig[j], deb_sig[j]);
		if (L_sub(corr, cor_max) > 0) {
			cor_max = corr;
			t0 = i;
		}
		deb_sig--;
	}
	i32 ener = 1;
	for (int i = 0; i < L_SUBFR; i++)
		ener = L_mac(ener, scal_sig[i - t0], scal_sig[i - t0]);
	i32 ener0 = 1;
	for (int i = 0; i < L_SUBFR; i++)
		ener0 = L_mac(ener0, scal_sig[i], scal_sig[i]);
	if (cor_max < 0)
		cor_max = 0;
	i32 temp = cor_max;
	if (ener > temp)
		temp = ener;
	if (ener0 > temp)
		temp = ener0;
	const i16 j = norm_l(temp);
	i16 cmax = round16(L_shl(cor_max, j));
	i16 en = round16(L_shl(ener, j));
	const i16 en0 = round16(L_shl(ener0, j));

	temp = L_mult(cmax, cmax);
	temp = L_sub(temp, L_shr(L_mult(en, en0), 1));
	if (temp < 0) {
		for (int i = 0; i < L_SUBFR; i++)
			signal_pst[i] = signal[i];
		return;
	}
	i16 g0, gain;
	if (sub(cmax, en) > 0) {
		g0 = INV_GAMMAP;
		gain = GAMMAP_2;
	} else {
		cmax = shr(mult(cmax, GAMMAP), 1);
		en = shr(en, 1);
		const i16 i = add(cmax, en);
		if (i > 0) {
			gain = div_s(cmax, i);
			g0 = sub(32767, gain);
		} else {
			g0 = 32767;
			gain = 0;
		}
	}
	for (int i = 0; i < L_SUBFR; i++)
		signal_pst[i] = add(mult(g0, signal[i]),
				    mult(gain, signal[i - t0]));
}

/* the tilt compensation: 1 - g z^-1 on sig */
static void preemphasis(i16 &mem_pre, i16 *sig, i16 g)
{
	const i16 temp = sig[L_SUBFR - 1];
	for (int i = L_SUBFR - 1; i > 0; i--)
		sig[i] = sub(sig[i], mult(g, sig[i - 1]));
	sig[0] = sub(sig[0], mult(g, mem_pre));
	mem_pre = temp;
}

/* the adaptive gain control (A.4.2.4/G.729): sig_out to sig_in's
   energy, smoothly */
static void agc(i16 &past_gain, const i16 *sig_in, i16 *sig_out)
{
	i16 signal[L_SUBFR];
	for (int i = 0; i < L_SUBFR; i++)
		signal[i] = shr(sig_out[i], 2);
	i32 s = 0;
	for (int i = 0; i < L_SUBFR; i++)
		s = L_mac(s, signal[i], signal[i]);
	if (s == 0) {
		past_gain = 0;
		return;
	}
	i16 exp = sub(norm_l(s), 1);
	const i16 gain_out = round16(L_shl(s, exp));

	for (int i = 0; i < L_SUBFR; i++)
		signal[i] = shr(sig_in[i], 2);
	s = 0;
	for (int i = 0; i < L_SUBFR; i++)
		s = L_mac(s, signal[i], signal[i]);
	i16 g0;
	if (s == 0) {
		g0 = 0;
	} else {
		const i16 n = norm_l(s);
		const i16 gain_in = round16(L_shl(s, n));
		exp = sub(exp, n);
		s = L_deposit_l(div_s(gain_out, gain_in));
		s = L_shl(s, 7);
		s = L_shr(s, exp);
		s = Inv_sqrt(s);
		const i16 g = round16(L_shl(s, 9));
		g0 = mult(g, AGC_FAC1);
	}
	i16 gain = past_gain;
	for (int i = 0; i < L_SUBFR; i++) {
		gain = mult(gain, AGC_FAC);
		gain = add(gain, g0);
		sig_out[i] = extract_h(L_shl(L_mult(sig_out[i], gain), 3));
	}
	past_gain = gain;
}

/* Annex A's postfilter (A.4.2/G.729) on syn (syn[-M..-1] the last
   frame's): long-term, short-term, tilt, gain control */
static void post_filter(DecState &s, i16 *syn, const i16 *Az_4,
			const i16 *T, bool vad)
{
	i16 res2_pst[L_SUBFR], syn_pst[L_FRAME], Ap3[MP1], Ap4[MP1], h[L_H];
	i16 *res2 = s.res2_buf + PIT_MAX;
	i16 *scal_res2 = s.scal_res2_buf + PIT_MAX;
	const i16 *Az = Az_4;
	for (int i_subfr = 0; i_subfr < L_FRAME; i_subfr += L_SUBFR) {
		i16 t0_min = sub(*T++, 3);
		i16 t0_max = add(t0_min, 6);
		if (sub(t0_max, PIT_MAX) > 0) {
			t0_max = PIT_MAX;
			t0_min = sub(t0_max, 6);
		}
		weight_az(Az, GAMMA2_PST, Ap3);
		weight_az(Az, GAMMA1_PST, Ap4);
		residu(Ap3, &syn[i_subfr], res2, L_SUBFR);
		for (int j = 0; j < L_SUBFR; j++)
			scal_res2[j] = shr(res2[j], 2);
		if (vad)
			pit_pst_filt(res2, scal_res2, t0_min, t0_max, res2_pst);
		else
			for (int j = 0; j < L_SUBFR; j++)
				res2_pst[j] = res2[j];

		/* the tilt from the filter's impulse response */
		for (int i = 0; i <= M; i++)
			h[i] = Ap3[i];
		for (int i = M + 1; i < L_H; i++)
			h[i] = 0;
		syn_filt(Ap4, h, h, L_H, &h[M + 1], false);
		i32 t = L_mult(h[0], h[0]);
		for (int i = 1; i < L_H; i++)
			t = L_mac(t, h[i], h[i]);
		const i16 temp1 = extract_h(t);
		t = L_mult(h[0], h[1]);
		for (int i = 1; i < L_H - 1; i++)
			t = L_mac(t, h[i], h[i + 1]);
		i16 temp2 = extract_h(t);
		if (temp2 <= 0) {
			temp2 = 0;
		} else {
			temp2 = mult(temp2, MU);
			temp2 = div_s(temp2, temp1);
		}
		preemphasis(s.mem_pre, res2_pst, temp2);

		syn_filt(Ap4, res2_pst, &syn_pst[i_subfr], L_SUBFR,
			 s.mem_syn_pst, true);
		agc(s.past_gain, &syn[i_subfr], &syn_pst[i_subfr]);

		for (int i = 0; i < PIT_MAX; i++) {
			res2[i - PIT_MAX] = res2[L_SUBFR - PIT_MAX + i];
			scal_res2[i - PIT_MAX] = scal_res2[L_SUBFR - PIT_MAX + i];
		}
		Az += MP1;
	}
	for (int i = 0; i < M; i++)
		syn[i - M] = syn[L_FRAME - M + i];
	for (int i = 0; i < L_FRAME; i++)
		syn[i] = syn_pst[i];
}

/* (see g729a.h) */
Decoder::Decoder() : st_(new DecState())
{
	reset();
}

/* (see g729a.h) */
Decoder::~Decoder() = default;

/* (see g729a.h) */
void Decoder::reset()
{
	static const i16 lsp_init[M] = { 30000, 26000, 21000, 15000, 8000, 0,
					 -8000, -15000, -21000, -26000 };
	DecState &s = *st_;
	s = DecState();
	for (int i = 0; i < M; i++)
		s.lsp_old[i] = lsp_init[i];
	s.sharp = SHARPMIN;
	s.old_T0 = 60;
	s.lsp.reset();
	s.prev_ma = 0;
	for (int i = 0; i < M; i++)
		s.prev_lsp[i] = s.lsp.freq_prev[0][i];
	for (i16 &e : s.past_qua_en)
		e = -14336;
	s.seed_fer = 21845;
	s.past_gain = 4096;
	s.hp.reset();
	static const i16 lsp_sid[M] = { 31441, 27566, 21458, 13612, 4663,
					-4663, -13612, -21458, -27566, -31441 };
	for (int i = 0; i < M; i++)
		s.lspSid[i] = lsp_sid[i];
	s.seed = INIT_SEED;
	s.past_ftyp = SPEECH;
	s.sid_sav = 0;
	s.sh_sid_sav = 1;
	s.sid_gain = sid_gain_of(0);
}

/* the frame (bfi: erased; type: its type, Annex B) through the
   decoder, the postfilter and the high-pass filter */
static void decode_frame(DecState &s, int type, const int16_t *prm,
			 bool bfi, int16_t pcm[FRAME])
{
	i16 parm[PRMS] = {}, Az_dec[MP1 * 2], T2[2];
	if (type == SPEECH) {
		for (int i = 0; i < PRMS; i++)
			parm[i] = prm[i];
		/* the parity check (4.1.2/G.729): 1 when it fails */
		parm[3] = static_cast<i16>(add(parity_pitch(parm[2]), parm[3]) &
					   1);
	} else if (type == SID) {
		for (int i = 0; i < SID_PRMS; i++)
			parm[i] = prm[i];
	}
	i16 *synth = s.synth_buf + M;
	const int ftyp = decod_ld8a(s, parm, bfi, type, synth, Az_dec, T2);
	post_filter(s, synth, Az_dec, T2, ftyp == SPEECH);
	s.hp.post(synth, L_FRAME);
	for (int i = 0; i < L_FRAME; i++)
		pcm[i] = synth[i];
}

/* (see g729a.h) */
void Decoder::decode(const int16_t prm[PRMS], bool erased,
		     int16_t pcm[FRAME])
{
	decode_frame(*st_, SPEECH, prm, erased, pcm);
}

/* (see g729a.h) */
void Decoder::decode(int type, const int16_t *prm, bool erased,
		     int16_t pcm[FRAME])
{
	if (type != SPEECH && type != SID)
		type = NONE;
	decode_frame(*st_, type, prm, erased, pcm);
}

/* (see g729a.h) */
void Decoder::decode(const uint8_t in[FRAME_OCTETS], int16_t pcm[FRAME])
{
	int16_t prm[PRMS];
	unpack(in, prm);
	decode_frame(*st_, SPEECH, prm, false, pcm);
}

/* (see g729a.h) */
void Decoder::decode(const uint8_t *in, int n, int16_t pcm[FRAME])
{
	int16_t prm[PRMS] = {};
	if (n >= FRAME_OCTETS) {
		unpack(in, prm);
		decode_frame(*st_, SPEECH, prm, false, pcm);
	} else if (n >= SID_OCTETS) {
		unpack_sid(in, prm);
		decode_frame(*st_, SID, prm, false, pcm);
	} else {
		decode_frame(*st_, NONE, prm, false, pcm);
	}
}

/* (see g729a.h) */
void Decoder::erase(int16_t pcm[FRAME])
{
	const int16_t prm[PRMS] = {};
	decode_frame(*st_, SPEECH, prm, true, pcm);
}

} // namespace g729
} // namespace dlink
