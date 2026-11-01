/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a_int.h - G.729 Annex A inside: its constants, its tables
 *                  (g729a_tab.cpp) and the parts the encoder and the
 *                  decoder share (g729a_common.cpp).  The names are the
 *                  Recommendation's, so that the code reads beside its
 *                  text
 *
 */
#ifndef DMODEM_LINK_G729A_INT_H
#define DMODEM_LINK_G729A_INT_H

#include "g729_ops.h"

namespace dlink {
namespace g729 {

enum {
	L_TOTAL = 240,          /* the speech buffer */
	L_WINDOW = 240,         /* the LP analysis window */
	L_NEXT = 40,            /* its lookahead */
	L_FRAME = 80,
	L_SUBFR = 40,
	M = 10,                 /* LP order */
	NP = 12,                /* Annex B's autocorrelations' order */
	MP1 = M + 1,
	PIT_MIN = 20,
	PIT_MAX = 143,
	L_INTERPOL = 10 + 1,
	UP_SAMP = 3,
	L_INTER10 = 10,
	NC = M / 2,
	MA_NP = 4,              /* the LSP MA predictor's order */
	MODE = 2,               /* its modes */
	NC0_B = 7, NC1_B = 5,   /* the LSP stages' bits */
	NC0 = 1 << NC0_B, NC1 = 1 << NC1_B,
	GRID_POINTS = 50,
	NCODE1_B = 3, NCODE2_B = 4,
	NCODE1 = 1 << NCODE1_B, NCODE2 = 1 << NCODE2_B,
	NCAN1 = 4, NCAN2 = 8,   /* the gain preselection's candidates */
	L_H = 22,               /* the postfilter's truncated response */
	NB_POS = 8, STEP = 5, MSIZE = 64, DIM_RR = 616,
};

constexpr i16 GAMMA1 = 24576, SHARPMAX = 13017, SHARPMIN = 3277,
	L_LIMIT = 40, M_LIMIT = 25681, GAP1 = 10, GAP2 = 5, GAP3 = 321,
	PI04 = 1029, PI92 = 23677, CONST10 = 10 * (1 << 11), CONST12 = 19661,
	INV_COEF = -17103, GPCLIP = 15564, GPCLIP2 = 481, GP0999 = 16383,
	GAMMAP = 16384, INV_GAMMAP = 21845, GAMMAP_2 = 10923,
	GAMMA2_PST = 18022, GAMMA1_PST = 22938, MU = 26214, AGC_FAC = 29491,
	AGC_FAC1 = 32767 - AGC_FAC;
constexpr i32 L_THRESH_ERR = 983040000;

extern const i16 hamwindow[240];
extern const i16 lag_h[12];
extern const i16 lag_l[12];
extern const i16 table[65];
extern const i16 slope[64];
extern const i16 table2[64];
extern const i16 slope_cos[64];
extern const i16 slope_acos[64];
extern const i16 lspcb1[128][10];
extern const i16 lspcb2[32][10];
extern const i16 fg[2][4][10];
extern const i16 fg_sum[2][10];
extern const i16 fg_sum_inv[2][10];
extern const i16 grid[51];
extern const i16 inter_3l[31];
extern const i16 pred[4];
extern const i16 gbk1[8][2];
extern const i16 gbk2[16][2];
extern const i16 map1[8];
extern const i16 map2[16];
extern const i16 coef[2][2];
extern const i32 L_coef[2][2];
extern const i16 thr1[4];
extern const i16 thr2[8];
extern const i16 imap1[8];
extern const i16 imap2[16];
extern const i16 b100[3];
extern const i16 a100[3];
extern const i16 b140[3];
extern const i16 a140[3];
extern const i16 bitsno[11];
extern const i16 tabpow[33];
extern const i16 tablog[33];
extern const i16 tabsqr[49];
extern const i16 tab_zone[153];

/* the MA predictor's past (3.2.4/G.729), the encoder's and the
   decoder's */
struct LspPast {
	i16 freq_prev[MA_NP][M];
	/* as at the start */
	void reset();
};

/* a second order high-pass filter's memory (3.1, 4.2.5/G.729) */
struct HighPass {
	i16 y2_hi, y2_lo, y1_hi, y1_lo, x0, x1;
	/* as at the start */
	void reset() { y2_hi = y2_lo = y1_hi = y1_lo = x0 = x1 = 0; }
	/* the encoder's (140 Hz, the input halved) on sig */
	void pre(i16 *sig, int n);
	/* the decoder's (100 Hz, the output doubled) on sig */
	void post(i16 *sig, int n);
};

/* LSPs to the LP filter (3.2.6/G.729) */
void lsp_az(const i16 *lsp, i16 *a);
/* LSFs to LSPs, and back (Annex A's tables) */
void lsf_lsp2(const i16 *lsf, i16 *lsp);
/* (see lsf_lsp2) */
void lsp_lsf2(const i16 *lsp, i16 *lsf);
/* the two subframes' LP filters: the old and new LSPs' mean, the new */
void int_qlpc(const i16 *lsp_old, const i16 *lsp_new, i16 *az);
/* a(z/gamma) */
void weight_az(const i16 *a, i16 gamma, i16 *ap);
/* the residual of x (x[-M..-1] known) through a */
void residu(const i16 *a, const i16 *x, i16 *y, int n);
/* x through 1/A(z); mem the last M outputs, updated if update; y may be
   x.  The overflow flag is left as the filtering left it */
void syn_filt(const i16 *a, const i16 *x, i16 *y, int n, i16 *mem,
	      bool update);
/* the adaptive codebook vector at lag t0 + frac/3 into exc[0..n) */
void pred_lt_3(i16 *exc, i16 t0, i16 frac, int n);
/* the parity of the first subframe's pitch index's 6 high bits */
i16 parity_pitch(i16 index);

/* the LSFs (in [0, 0.5) of the frequency, Q15) of lsp: the table's
   (Annex B's VAD) */
void lsp_lsf(const i16 *lsp, i16 *lsf);
/* the next pseudo-random number (4.4.4/G.729) */
i16 random16(i16 &seed);

/* the encoder's parts Annex B's DTX also uses (g729a_enc.cpp) */
/* Levinson-Durbin: r to the LP filter A (Q12), the reflection
   coefficients and (if err) the residual energy; an unstable filter
   gives old_A and old_rc again (err untouched), a stable one becomes
   them */
void levinson(const i16 *Rh, const i16 *Rl, i16 *A, i16 *rc, i16 *err,
	      i16 *old_A, i16 *old_rc);
/* the LP filter a to its LSPs; old_lsp if 10 roots are not found */
void az_lsp(const i16 *a, i16 *lsp, const i16 *old_lsp);
/* the LSFs' weights for the quantiser's distance (3.2.4/G.729) */
void get_wegt(const i16 *flsp, i16 *wegt);
/* taming (3.10/G.729): the excitation's error moved on with a
   subframe's pitch gain and lag */
void update_exc_err(i32 *exc_err, i16 gain_pit, i16 T0);

/* the LSP quantiser's stages to the quantised LSFs, the MA past moved
   on (3.2.4/G.729) */
void lsp_get_quant(i16 code0, i16 code1, i16 code2, i16 mode,
		   LspPast &past, i16 *lspq);
/* the minimum distance between buf's neighbours, over [lo, hi) */
void lsp_expand(i16 *buf, i16 gap, int lo, int hi);
/* the MA predictor's error: what lsp would be coded as */
void lsp_prev_extract(const i16 *lsp, i16 *lsp_ele, i16 mode,
		      const LspPast &past);
/* (see lsp_prev_extract) with these predictor coefficients */
void lsp_prev_extract(const i16 *lsp, i16 *lsp_ele, const i16 (*fgm)[M],
		      const i16 *fg_sum_inv_m, const LspPast &past);
/* the error lsp_ele and the MA past to the LSFs */
void lsp_prev_compose(const i16 *lsp_ele, i16 *lsp, const i16 (*fgm)[M],
		      const i16 *fg_sum_m, const LspPast &past);
/* the LSFs in order and apart (3.2.4/G.729) */
void lsp_stability(i16 *buf);
/* lsp_ele into the MA past */
void lsp_prev_update(const i16 *lsp_ele, LspPast &past);

/* the predicted fixed codebook gain (3.9.1/G.729): gcode0 * 2^-exp */
void gain_predict(const i16 *past_qua_en, const i16 *code, i16 *gcode0,
		  i16 *exp_gcode0);
/* the quantised energies' past, with the chosen gains' sum */
void gain_update(i16 *past_qua_en, i32 L_gbk12);
/* (see gain_update) for a lost frame */
void gain_update_erasure(i16 *past_qua_en);

/* Annex B: the VAD, DTX and comfort noise (g729b.cpp) */
enum {
	NB_SUMACF = 3, NB_CURACF = 2, NB_GAIN = 2,
	SIZ_SUMACF = NB_SUMACF * MP1, SIZ_ACF = NB_CURACF * MP1,
};
constexpr i16 INIT_SEED = 11111;

/* the voice activity detector (B.3/G.729) */
struct Vad {
	i16 MeanLSF[M], Min_buffer[16];
	i16 Prev_Min, Next_Min, Min, MeanE, MeanSE, MeanSLE, MeanSZC;
	i16 prev_energy, count_sil, count_update, count_ext, flag, v_flag,
		less_count;
	/* as at the start */
	void reset();
	/* this frame's decision (true: speech) from its second reflection
	   coefficient, unquantised LSFs, autocorrelations (r[0]'s exponent
	   exp_R0), preprocessed speech, count (1 the first) and the last two
	   frames' decisions */
	bool decide(i16 rc, const i16 *lsf, const i16 *r_h, const i16 *r_l,
		    i16 exp_R0, const i16 *sigpp, i16 frm_count, bool prev,
		    bool pprev);
};

/* the encoder's DTX (B.4/G.729) */
struct Dtx {
	i16 lspSid_q[M], pastCoeff[MP1], RCoeff[MP1], sh_RCoeff;
	i16 Acf[SIZ_ACF], sh_Acf[NB_CURACF];
	i16 sumAcf[SIZ_SUMACF], sh_sumAcf[NB_SUMACF];
	i16 ener[NB_GAIN], sh_ener[NB_GAIN];
	i16 fr_cur, cur_gain, nb_ener, sid_gain, flag_chang, prev_energy,
		count_fr0;
	/* as at the start */
	void reset();
	/* every frame: its autocorrelations r_h[0..M] (exponent exp_r) and
	   its VAD decision */
	void update(const i16 *r_h, i16 exp_r, bool vad);
	/* an inactive frame: whether to send a SID frame (its 4 parameters
	   in ana), the comfort noise excitation into exc, the frame's LP
	   filters into Aq; lsp_old_q, the LSF past, the seed, taming's
	   error and Levinson's last filter moved on */
	bool code(i16 *exc, bool pastVad, i16 *lsp_old_q, i16 *Aq, i16 *ana,
		  LspPast &past, i16 &seed, i32 *exc_err, i16 *old_A,
		  i16 *old_rc);
};

/* the comfort noise excitation for a frame (B.4.4/G.729), at gain
   cur_gain; exc_err: the encoder's taming error, moved on (null at the
   decoder) */
void calc_exc_rand(i16 cur_gain, i16 *exc, i16 &seed, i32 *exc_err);
/* nb_ener energies (0: the decoder's one saved) to the SID gain's index
   and its level in dB */
i16 qua_sidgain(const i16 *ener, const i16 *sh_ener, i16 nb_ener,
		i16 *enerq);
/* the SID frame's quantised gain for an index */
i16 sid_gain_of(i16 index);
/* the SID frame's LSF quantiser: lsp to its three indices, its
   quantised LSPs, the MA past moved on */
void lsfq_noise(const i16 *lsp, i16 *lspq, LspPast &past, i16 *ana);
/* (see lsfq_noise) the indices back to LSPs */
void sid_lsfq_decode(const i16 *index, i16 *lspq, LspPast &past);

} // namespace g729
} // namespace dlink

#endif
