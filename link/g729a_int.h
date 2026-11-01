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
extern const i16 lag_h[10];
extern const i16 lag_l[10];
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

/* the LSP quantiser's stages to the quantised LSFs, the MA past moved
   on (3.2.4/G.729) */
void lsp_get_quant(i16 code0, i16 code1, i16 code2, i16 mode,
		   LspPast &past, i16 *lspq);
/* the minimum distance between buf's neighbours, over [lo, hi) */
void lsp_expand(i16 *buf, i16 gap, int lo, int hi);
/* the MA predictor's error: what lsp would be coded as */
void lsp_prev_extract(const i16 *lsp, i16 *lsp_ele, i16 mode,
		      const LspPast &past);
/* lsp_ele into the MA past */
void lsp_prev_update(const i16 *lsp_ele, LspPast &past);

/* the predicted fixed codebook gain (3.9.1/G.729): gcode0 * 2^-exp */
void gain_predict(const i16 *past_qua_en, const i16 *code, i16 *gcode0,
		  i16 *exp_gcode0);
/* the quantised energies' past, with the chosen gains' sum */
void gain_update(i16 *past_qua_en, i32 L_gbk12);
/* (see gain_update) for a lost frame */
void gain_update_erasure(i16 *past_qua_en);

} // namespace g729
} // namespace dlink

#endif
