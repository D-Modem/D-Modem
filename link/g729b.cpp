/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729b.cpp - G.729 Annex B for Annex A (g729a.h): the voice activity
 *                detector, the encoder's discontinuous transmission
 *                (when to send a SID frame, and what), the SID frame's
 *                quantisers and the comfort noise both ends make in
 *                its place
 *
 */

#include "g729a.h"
#include "g729a_int.h"

namespace dlink {
namespace g729 {

namespace {

/* the VAD's low band filter's correlations */
const i16 lbf_corr[NP + 1] = { 7869, 7011, 4838, 2299, 321, -660, -782,
			       -484, -164, 3, 39, 21, 4 };
/* 1/n as a shift and a factor, for the first frames' means */
const i16 shift_fx[33] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
			   1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5,
			   0 };
const i16 factor_fx[33] = { 32767, 16913, 17476, 18079, 18725, 19418,
			    20165, 20972, 21845, 22795, 23831, 24966,
			    26214, 27594, 29127, 30840, 32767, 17476,
			    18725, 20165, 21845, 23831, 26214, 29127,
			    32767, 18725, 21845, 26214, 32767, 21845,
			    32767, 32767, 0 };
/* the SID gain's quantiser */
const i16 fact[NB_GAIN + 1] = { 410, 26, 13 };
const i16 marg[NB_GAIN + 1] = { 0, 0, 1 };
const i16 tab_Sidgain[32] = {
	2, 5, 8, 13, 20, 32, 50, 64, 80, 101, 127, 160, 201, 253, 318, 401,
	505, 635, 800, 1007, 1268, 1596, 2010, 2530, 3185, 4009, 5048, 6355,
	8000, 10071, 12679, 15962,
};
/* the SID frame's LSF quantiser: its predictors and codebooks (the
   speech quantiser's, a subset) */
const i16 noise_fg_sum[MODE][M] = {
	{ 7798, 8447, 8205, 8293, 8126, 8477, 8447, 8703, 9043, 8604 },
	{ 10514, 12402, 12833, 11914, 11447, 11670, 11132, 11311, 11844,
	  11447 },
};
const i16 noise_fg_sum_inv[MODE][M] = {
	{ 17210, 15888, 16357, 16183, 16516, 15833, 15888, 15421, 14840,
	  15597 },
	{ 12764, 10821, 10458, 11264, 11724, 11500, 12056, 11865, 11331,
	  11724 },
};
const i16 PtrTab_1[32] = { 96, 52, 20, 54, 86, 114, 82, 68, 36, 121, 48,
			   92, 18, 120, 94, 124, 50, 125, 4, 100, 28, 76,
			   12, 117, 81, 22, 90, 116, 127, 21, 108, 66 };
const i16 PtrTab_2[2][16] = {
	{ 31, 21, 9, 3, 10, 2, 19, 26, 4, 3, 11, 29, 15, 27, 21, 12 },
	{ 16, 1, 0, 0, 8, 25, 22, 20, 19, 23, 20, 31, 4, 31, 20, 31 },
};
const i16 Mp[MODE] = { 8644, 16572 };

constexpr i16 FR_SID_MIN = 3, FRAC_THRESH1 = 4855, FRAC_THRESH2 = 3161,
	A_GAIN0 = 28672, A_GAIN1 = 4096, FRAC1 = 19043, K0 = 24576,
	G_MAX = 5000;
enum { R_LSFQ = 10, INIT_FRAME = 32, INIT_COUNT = 20, ZC_START = 120,
       ZC_END = 200 };

/* the SID quantiser's MA predictors: the first mode's, and the two
   speech modes' 0.6 to 0.4 */
struct NoiseFg {
	i16 t[MODE][MA_NP][M];
	/* the tables made from fg */
	NoiseFg()
	{
		for (int i = 0; i < MA_NP; i++)
			for (int j = 0; j < M; j++) {
				t[0][i][j] = fg[0][i][j];
				i32 acc = L_mult(fg[0][i][j], 19660);
				acc = L_mac(acc, fg[1][i][j], 13107);
				t[1][i][j] = extract_h(acc);
			}
	}
};

/* (see NoiseFg) */
const i16 (*noise_fg(int mode))[M]
{
	static const NoiseFg n;
	return n.t[mode];
}

} // namespace

/* (see g729a_int.h) */
void Vad::reset()
{
	*this = Vad();
	flag = 1;
	Min = MAX16;
}

/* the VAD's decision from the differences between the frame's
   parameters and their means (B.3.6/G.729) */
static bool make_dec(i16 dSLE, i16 dSE, i16 SD, i16 dSZC)
{
	i32 acc = L_mult(dSZC, -14680);
	acc = L_mac(acc, 8192, -28521);
	acc = L_shr(acc, 8);
	acc = L_add(acc, L_deposit_h(SD));
	if (acc > 0)
		return true;
	acc = L_mult(dSZC, 19065);
	acc = L_mac(acc, 8192, -19446);
	acc = L_shr(acc, 7);
	acc = L_add(acc, L_deposit_h(SD));
	if (acc > 0)
		return true;
	acc = L_mult(dSZC, 20480);
	acc = L_mac(acc, 8192, 16384);
	acc = L_shr(acc, 2);
	acc = L_add(acc, L_deposit_h(dSE));
	if (acc < 0)
		return true;
	acc = L_mult(dSZC, -16384);
	acc = L_mac(acc, 8192, 19660);
	acc = L_shr(acc, 2);
	acc = L_add(acc, L_deposit_h(dSE));
	if (acc < 0)
		return true;
	acc = L_mult(dSE, 32767);
	acc = L_mac(acc, 1024, 30802);
	if (acc < 0)
		return true;
	acc = L_mult(SD, -28160);
	acc = L_mac(acc, 64, 19988);
	acc = L_mac(acc, dSE, 512);
	if (acc < 0)
		return true;
	acc = L_mult(SD, 32767);
	acc = L_mac(acc, 32, -30199);
	if (acc > 0)
		return true;
	acc = L_mult(dSZC, -20480);
	acc = L_mac(acc, 8192, 22938);
	acc = L_shr(acc, 2);
	acc = L_add(acc, L_deposit_h(dSE));
	if (acc < 0)
		return true;
	acc = L_mult(dSZC, 23831);
	acc = L_mac(acc, 4096, 31576);
	acc = L_shr(acc, 2);
	acc = L_add(acc, L_deposit_h(dSE));
	if (acc < 0)
		return true;
	acc = L_mult(dSE, 32767);
	acc = L_mac(acc, 2048, 17367);
	if (acc < 0)
		return true;
	acc = L_mult(SD, -22400);
	acc = L_mac(acc, 32, 25395);
	acc = L_mac(acc, dSLE, 256);
	if (acc < 0)
		return true;
	acc = L_mult(dSE, -30427);
	acc = L_mac(acc, 256, -29959);
	acc = L_add(acc, L_deposit_h(dSLE));
	if (acc > 0)
		return true;
	acc = L_mult(dSE, -23406);
	acc = L_mac(acc, 512, 28087);
	acc = L_add(acc, L_deposit_h(dSLE));
	if (acc < 0)
		return true;
	acc = L_mult(dSE, 24576);
	acc = L_mac(acc, 1024, 29491);
	acc = L_mac(acc, dSLE, 16384);
	return acc < 0;
}

/* an energy (r0 with exponent exp_R0) in the VAD's log domain */
static i16 vad_energy(i32 r0, i16 exp_R0)
{
	i16 exp, frac;
	Log2(r0, &exp, &frac);
	i32 acc = Mpy_32_16(exp, frac, 9864);
	const i16 i = sub(sub(exp_R0, 1), 1);
	acc = L_mac(acc, 9864, i);
	acc = L_shl(acc, 11);
	return sub(extract_h(acc), 4875);
}

/* (see g729a_int.h): B.3/G.729 */
bool Vad::decide(i16 rc, const i16 *lsf, const i16 *r_h, const i16 *r_l,
		 i16 exp_R0, const i16 *sigpp, i16 frm_count, bool prev,
		 bool pprev)
{
	/* the parameters: the full and low band energies, the spectral
	   distortion, the zero crossings */
	const i16 ENERGY = vad_energy(L_Comp(r_h[0], r_l[0]), exp_R0);
	i32 acc = 0;
	for (int i = 1; i <= NP; i++)
		acc = L_mac(acc, r_h[i], lbf_corr[i]);
	acc = L_shl(acc, 1);
	acc = L_mac(acc, r_h[0], lbf_corr[0]);
	const i16 ENERGY_low = vad_energy(acc, exp_R0);
	acc = 0;
	for (int i = 0; i < M; i++) {
		const i16 j = sub(lsf[i], MeanLSF[i]);
		acc = L_mac(acc, j, j);
	}
	const i16 SD = extract_h(acc);
	i16 ZC = 0;
	for (int i = ZC_START + 1; i <= ZC_END; i++)
		if (mult(sigpp[i - 1], sigpp[i]) < 0)
			ZC = add(ZC, 410);

	/* the minimum energy over the last 128 frames */
	if (sub(frm_count, 129) < 0) {
		if (sub(ENERGY, Min) < 0) {
			Min = ENERGY;
			Prev_Min = ENERGY;
		}
		if ((frm_count & 0x0007) == 0) {
			Min_buffer[sub(shr(frm_count, 3), 1)] = Min;
			Min = MAX16;
		}
	}
	if ((frm_count & 0x0007) == 0) {
		Prev_Min = Min_buffer[0];
		for (int i = 1; i < 16; i++)
			if (sub(Min_buffer[i], Prev_Min) < 0)
				Prev_Min = Min_buffer[i];
	}
	if (sub(frm_count, 129) >= 0) {
		if (((frm_count & 0x0007) ^ 0x0001) == 0) {
			Min = Prev_Min;
			Next_Min = MAX16;
		}
		if (sub(ENERGY, Min) < 0)
			Min = ENERGY;
		if (sub(ENERGY, Next_Min) < 0)
			Next_Min = ENERGY;
		if ((frm_count & 0x0007) == 0) {
			for (int i = 0; i < 15; i++)
				Min_buffer[i] = Min_buffer[i + 1];
			Min_buffer[15] = Next_Min;
			Prev_Min = Min_buffer[0];
			for (int i = 1; i < 16; i++)
				if (sub(Min_buffer[i], Prev_Min) < 0)
					Prev_Min = Min_buffer[i];
		}
	}

	/* the first frames: the means of those above the floor */
	bool marker = false;
	if (sub(frm_count, INIT_FRAME) <= 0) {
		if (sub(ENERGY, 3072) < 0) {
			marker = false;
			less_count++;
		} else {
			marker = true;
			acc = L_deposit_h(MeanE);
			acc = L_mac(acc, ENERGY, 1024);
			MeanE = extract_h(acc);
			acc = L_deposit_h(MeanSZC);
			acc = L_mac(acc, ZC, 1024);
			MeanSZC = extract_h(acc);
			for (int i = 0; i < M; i++) {
				acc = L_deposit_h(MeanLSF[i]);
				acc = L_mac(acc, lsf[i], 1024);
				MeanLSF[i] = extract_h(acc);
			}
		}
	}

	if (sub(frm_count, INIT_FRAME) >= 0) {
		if (sub(frm_count, INIT_FRAME) == 0) {
			acc = L_mult(MeanE, factor_fx[less_count]);
			acc = L_shl(acc, shift_fx[less_count]);
			MeanE = extract_h(acc);
			acc = L_mult(MeanSZC, factor_fx[less_count]);
			acc = L_shl(acc, shift_fx[less_count]);
			MeanSZC = extract_h(acc);
			for (int i = 0; i < M; i++) {
				acc = L_mult(MeanLSF[i], factor_fx[less_count]);
				acc = L_shl(acc, shift_fx[less_count]);
				MeanLSF[i] = extract_h(acc);
			}
			MeanSE = sub(MeanE, 2048);
			MeanSLE = sub(MeanE, 2458);
		}

		const i16 dSE = sub(MeanSE, ENERGY);
		const i16 dSLE = sub(MeanSLE, ENERGY_low);
		const i16 dSZC = sub(MeanSZC, ZC);
		if (sub(ENERGY, 3072) < 0)
			marker = false;
		else
			marker = make_dec(dSLE, dSE, SD, dSZC);

		/* smoothing (B.3.7/G.729) */
		v_flag = 0;
		if (prev && !marker && add(dSE, 410) < 0 &&
		    sub(ENERGY, 3072) > 0) {
			marker = true;
			v_flag = 1;
		}
		if (flag == 1) {
			if (pprev && prev && !marker &&
			    sub(abs_s(sub(prev_energy, ENERGY)), 614) <= 0) {
				count_ext++;
				marker = true;
				v_flag = 1;
				if (sub(count_ext, 4) <= 0) {
					flag = 1;
				} else {
					count_ext = 0;
					flag = 0;
				}
			}
		} else {
			flag = 1;
		}
		if (!marker)
			count_sil++;
		if (marker && sub(count_sil, 10) > 0 &&
		    sub(sub(ENERGY, prev_energy), 614) <= 0) {
			marker = false;
			count_sil = 0;
		}
		if (marker)
			count_sil = 0;
		if (sub(sub(ENERGY, 614), MeanSE) < 0 &&
		    sub(frm_count, 128) > 0 && !v_flag && sub(rc, 19661) < 0)
			marker = false;

		/* the means follow the background noise (B.3.8/G.729) */
		if (sub(sub(ENERGY, 614), MeanSE) < 0 && sub(rc, 24576) < 0 &&
		    sub(SD, 83) < 0) {
			count_update++;
			i16 COEF, C_COEF, COEFZC, C_COEFZC, COEFSD, C_COEFSD;
			if (sub(count_update, INIT_COUNT) < 0) {
				COEF = 24576; C_COEF = 8192;
				COEFZC = 26214; C_COEFZC = 6554;
				COEFSD = 19661; C_COEFSD = 13017;
			} else if (sub(count_update, INIT_COUNT + 10) < 0) {
				COEF = 31130; C_COEF = 1638;
				COEFZC = 30147; C_COEFZC = 2621;
				COEFSD = 21299; C_COEFSD = 11469;
			} else if (sub(count_update, INIT_COUNT + 20) < 0) {
				COEF = 31785; C_COEF = 983;
				COEFZC = 30802; C_COEFZC = 1966;
				COEFSD = 22938; C_COEFSD = 9830;
			} else if (sub(count_update, INIT_COUNT + 30) < 0) {
				COEF = 32440; C_COEF = 328;
				COEFZC = 31457; C_COEFZC = 1311;
				COEFSD = 24576; C_COEFSD = 8192;
			} else if (sub(count_update, INIT_COUNT + 40) < 0) {
				COEF = 32604; C_COEF = 164;
				COEFZC = 32440; C_COEFZC = 328;
				COEFSD = 24576; C_COEFSD = 8192;
			} else {
				COEF = 32604; C_COEF = 164;
				COEFZC = 32702; C_COEFZC = 66;
				COEFSD = 24576; C_COEFSD = 8192;
			}
			acc = L_mult(COEF, MeanSE);
			acc = L_mac(acc, C_COEF, ENERGY);
			MeanSE = extract_h(acc);
			acc = L_mult(COEF, MeanSLE);
			acc = L_mac(acc, C_COEF, ENERGY_low);
			MeanSLE = extract_h(acc);
			acc = L_mult(COEFZC, MeanSZC);
			acc = L_mac(acc, C_COEFZC, ZC);
			MeanSZC = extract_h(acc);
			for (int i = 0; i < M; i++) {
				acc = L_mult(COEFSD, MeanLSF[i]);
				acc = L_mac(acc, C_COEFSD, lsf[i]);
				MeanLSF[i] = extract_h(acc);
			}
		}
		if (sub(frm_count, 128) > 0 &&
		    ((sub(MeanSE, Min) < 0 && sub(SD, 83) < 0) ||
		     sub(MeanSE, Min) > 2048)) {
			MeanSE = Min;
			count_update = 0;
		}
	}
	prev_energy = ENERGY;
	return marker;
}

/* nb autocorrelations (each M + 1, exponents sh_acf) summed into sum,
   its exponent sh_sum */
static void calc_sum_acf(const i16 *acf, const i16 *sh_acf, i16 *sum,
			 i16 *sh_sum, int nb)
{
	i16 sh0 = sh_acf[0];
	for (int i = 1; i < nb; i++)
		if (sub(sh_acf[i], sh0) < 0)
			sh0 = sh_acf[i];
	sh0 = add(sh0, 14);
	i32 tab[MP1] = {};
	const i16 *p = acf;
	for (int i = 0; i < nb; i++) {
		const i16 temp = sub(sh0, sh_acf[i]);
		for (int j = 0; j < MP1; j++)
			tab[j] = L_add(tab[j], L_shl(L_deposit_l(*p++), temp));
	}
	i16 temp = norm_l(tab[0]);
	for (int i = 0; i <= M; i++)
		sum[i] = extract_h(L_shl(tab[i], temp));
	temp = sub(temp, 16);
	*sh_sum = add(sh0, temp);
}

/* the filter Coeff's autocorrelation, for the Itakura distance */
static void calc_rcoeff(const i16 *Coeff, i16 *RCoeff, i16 *sh_RCoeff)
{
	i32 acc = 0;
	for (int j = 0; j <= M; j++)
		acc = L_mac(acc, Coeff[j], Coeff[j]);
	const i16 sh1 = norm_l(acc);
	acc = L_shl(acc, sh1);
	RCoeff[0] = round16(acc);
	for (int i = 1; i <= M; i++) {
		acc = 0;
		for (int j = 0; j <= M - i; j++)
			acc = L_mac(acc, Coeff[j], Coeff[j + i]);
		acc = L_shl(acc, sh1);
		RCoeff[i] = round16(acc);
	}
	*sh_RCoeff = sh1;
}

/* whether the filter RCoeff is far (the Itakura distance over the
   threshold) from the autocorrelation acf, whose residual energy is
   alpha */
static bool cmp_filt(const i16 *RCoeff, i16 sh_RCoeff, const i16 *acf,
		     i16 alpha, i16 FracThresh)
{
	i16 sh[2] = { 0, 0 };
	int ind = 1;
	i32 t0;
	for (;;) {
		overflow() = false;
		i16 temp1 = shr(RCoeff[0], sh[0]);
		i16 temp2 = shr(acf[0], sh[1]);
		t0 = L_shr(L_mult(temp1, temp2), 1);
		for (int i = 1; i <= M; i++) {
			temp1 = shr(RCoeff[i], sh[0]);
			temp2 = shr(acf[i], sh[1]);
			t0 = L_mac(t0, temp1, temp2);
		}
		if (!overflow())
			break;
		sh[ind] = add(sh[ind], 1);
		ind = 1 - ind;
	}
	i16 temp1 = mult_r(alpha, FracThresh);
	i32 t1 = L_add(L_deposit_l(temp1), L_deposit_l(alpha));
	temp1 = add(sh_RCoeff, 9);
	const i16 temp2 = add(sh[0], sh[1]);
	temp1 = sub(temp1, temp2);
	t1 = L_shl(t1, temp1);
	t0 = L_sub(t0, t1);
	return t0 > 0;
}

/* (see g729a_int.h) */
void Dtx::reset()
{
	*this = Dtx();
	for (i16 &s : sh_sumAcf)
		s = 40;
	for (i16 &s : sh_Acf)
		s = 40;
	for (i16 &s : sh_ener)
		s = 40;
}

/* the last NB_CURACF frames' autocorrelations into the sum's past */
static void update_sum_acf(Dtx &d)
{
	for (int i = SIZ_SUMACF - 1; i >= MP1; i--)
		d.sumAcf[i] = d.sumAcf[i - MP1];
	for (int i = NB_SUMACF - 1; i >= 1; i--)
		d.sh_sumAcf[i] = d.sh_sumAcf[i - 1];
	calc_sum_acf(d.Acf, d.sh_Acf, d.sumAcf, d.sh_sumAcf, NB_CURACF);
}

/* (see g729a_int.h) */
void Dtx::update(const i16 *r_h, i16 exp_r, bool vad)
{
	for (int i = SIZ_ACF - 1; i >= MP1; i--)
		Acf[i] = Acf[i - MP1];
	for (int i = NB_CURACF - 1; i >= 1; i--)
		sh_Acf[i] = sh_Acf[i - 1];
	sh_Acf[0] = negate(add(16, exp_r));
	for (int i = 0; i < MP1; i++)
		Acf[i] = r_h[i];
	fr_cur = add(fr_cur, 1);
	if (sub(fr_cur, NB_CURACF) == 0) {
		fr_cur = 0;
		if (vad)
			update_sum_acf(*this);
	}
}

/* (see g729a_int.h): B.4.1 to B.4.3/G.729 */
bool Dtx::code(i16 *exc, bool pastVad, i16 *lsp_old_q, i16 *Aq, i16 *ana,
	       LspPast &past, i16 &seed, i32 *exc_err, i16 *old_A,
	       i16 *old_rc)
{
	for (int i = NB_GAIN - 1; i >= 1; i--) {
		ener[i] = ener[i - 1];
		sh_ener[i] = sh_ener[i - 1];
	}

	/* the last frames' filter and its residual energy */
	i16 curAcf[MP1], curCoeff[MP1] = {}, bid[M], zero[MP1] = {};
	calc_sum_acf(Acf, sh_Acf, curAcf, &sh_ener[0], NB_CURACF);
	if (curAcf[0] == 0)
		ener[0] = 0;
	else
		levinson(curAcf, zero, curCoeff, bid, &ener[0], old_A, old_rc);

	/* SID or nothing */
	bool sid;
	i16 energyq, cur_igain;
	if (pastVad) {
		sid = true;
		count_fr0 = 0;
		nb_ener = 1;
		cur_igain = qua_sidgain(ener, sh_ener, nb_ener, &energyq);
	} else {
		nb_ener = add(nb_ener, 1);
		if (sub(nb_ener, NB_GAIN) > 0)
			nb_ener = NB_GAIN;
		cur_igain = qua_sidgain(ener, sh_ener, nb_ener, &energyq);
		if (cmp_filt(RCoeff, sh_RCoeff, curAcf, ener[0], FRAC_THRESH1))
			flag_chang = 1;
		i16 temp = abs_s(sub(prev_energy, energyq));
		temp = sub(temp, 2);
		if (temp > 0)
			flag_chang = 1;
		count_fr0 = add(count_fr0, 1);
		if (sub(count_fr0, FR_SID_MIN) < 0) {
			sid = false;
		} else {
			sid = flag_chang != 0;
			count_fr0 = FR_SID_MIN;
		}
	}

	if (sid) {
		count_fr0 = 0;
		flag_chang = 0;
		/* the past's average filter, or this one if they differ */
		i16 s_sumAcf[MP1], temp;
		calc_sum_acf(sumAcf, sh_sumAcf, s_sumAcf, &temp, NB_SUMACF);
		if (s_sumAcf[0] == 0) {
			pastCoeff[0] = 4096;
			for (int i = 1; i <= M; i++)
				pastCoeff[i] = 0;
		} else {
			levinson(s_sumAcf, zero, pastCoeff, bid, nullptr, old_A,
				 old_rc);
		}
		calc_rcoeff(pastCoeff, RCoeff, &sh_RCoeff);
		const i16 *lpcCoeff;
		if (!cmp_filt(RCoeff, sh_RCoeff, curAcf, ener[0],
			      FRAC_THRESH2)) {
			lpcCoeff = pastCoeff;
		} else {
			lpcCoeff = curCoeff;
			calc_rcoeff(curCoeff, RCoeff, &sh_RCoeff);
		}
		i16 lsp_new[M];
		az_lsp(lpcCoeff, lsp_new, lsp_old_q);
		lsfq_noise(lsp_new, lspSid_q, past, &ana[0]);
		prev_energy = energyq;
		ana[3] = cur_igain;
		sid_gain = tab_Sidgain[cur_igain];
	}

	/* the comfort noise */
	if (pastVad) {
		cur_gain = sid_gain;
	} else {
		cur_gain = mult_r(cur_gain, A_GAIN0);
		cur_gain = add(cur_gain, mult_r(sid_gain, A_GAIN1));
	}
	calc_exc_rand(cur_gain, exc, seed, exc_err);
	int_qlpc(lsp_old_q, lspSid_q, Aq);
	for (int i = 0; i < M; i++)
		lsp_old_q[i] = lspSid_q[i];
	if (fr_cur == 0)
		update_sum_acf(*this);
	return sid;
}

/* a Gaussian number, 512 the deviation, from 12 uniform ones */
static i16 gauss(i16 &seed)
{
	i32 acc = 0;
	for (int i = 0; i < 12; i++)
		acc = L_add(acc, L_deposit_l(random16(seed)));
	acc = L_shr(acc, 7);
	return extract_l(acc);
}

/* sqrt(num / 2) */
static i16 sqrt_half(i32 num)
{
	i16 rez = 0, exp = 0x4000;
	for (int i = 0; i < 14; i++) {
		const i32 acc = L_mult(add(rez, exp), add(rez, exp));
		if (L_sub(num, acc) >= 0)
			rez = add(rez, exp);
		exp = shr(exp, 1);
	}
	return rez;
}

/* (see g729a_int.h): B.4.4/G.729 */
void calc_exc_rand(i16 cur_gain, i16 *exc, i16 &seed, i32 *exc_err)
{
	if (cur_gain == 0) {
		for (int i = 0; i < L_FRAME; i++)
			exc[i] = 0;
		if (exc_err)
			for (int i_subfr = 0; i_subfr < L_FRAME;
			     i_subfr += L_SUBFR)
				update_exc_err(exc_err, 0, L_SUBFR + 1);
		return;
	}

	i16 *cur_exc = exc;
	for (int i_subfr = 0; i_subfr < L_FRAME; i_subfr += L_SUBFR) {
		/* a random adaptive and fixed codebook */
		i16 pos[4], sign[4];
		i16 temp1 = random16(seed);
		i16 frac = sub(static_cast<i16>(temp1 & 0x0003), 1);
		if (sub(frac, 2) == 0)
			frac = 0;
		temp1 = shr(temp1, 2);
		const i16 t0 = add(static_cast<i16>(temp1 & 0x003F), 40);
		temp1 = shr(temp1, 6);
		i16 temp2 = static_cast<i16>(temp1 & 0x0007);
		pos[0] = add(shl(temp2, 2), temp2);
		temp1 = shr(temp1, 3);
		sign[0] = static_cast<i16>(temp1 & 0x0001);
		temp1 = shr(temp1, 1);
		temp2 = static_cast<i16>(temp1 & 0x0007);
		temp2 = add(shl(temp2, 2), temp2);
		pos[1] = add(temp2, 1);
		temp1 = shr(temp1, 3);
		sign[1] = static_cast<i16>(temp1 & 0x0001);
		temp1 = random16(seed);
		temp2 = static_cast<i16>(temp1 & 0x0007);
		temp2 = add(shl(temp2, 2), temp2);
		pos[2] = add(temp2, 2);
		temp1 = shr(temp1, 3);
		sign[2] = static_cast<i16>(temp1 & 0x0001);
		temp1 = shr(temp1, 1);
		temp2 = static_cast<i16>(temp1 & 0x000F);
		pos[3] = add(static_cast<i16>(temp2 & 1), 3);
		temp2 = static_cast<i16>(shr(temp2, 1) & 7);
		temp2 = add(shl(temp2, 2), temp2);
		pos[3] = add(pos[3], temp2);
		temp1 = shr(temp1, 4);
		sign[3] = static_cast<i16>(temp1 & 0x0001);
		i16 Gp = static_cast<i16>(random16(seed) & 0x1FFF);
		const i16 Gp2 = shl(Gp, 1);

		/* the Gaussian excitation at the gain */
		i16 excg[L_SUBFR], excs[L_SUBFR];
		i32 acc = 0;
		for (int i = 0; i < L_SUBFR; i++) {
			temp1 = gauss(seed);
			acc = L_mac(acc, temp1, temp1);
			excg[i] = temp1;
		}
		acc = Inv_sqrt(L_shr(acc, 1));
		i16 hi, lo;
		L_Extract(acc, &hi, &lo);
		temp1 = mult_r(cur_gain, FRAC1);
		temp1 = add(cur_gain, temp1);
		acc = Mpy_32_16(hi, lo, temp1);
		i16 sh = norm_l(acc);
		temp1 = extract_h(L_shl(acc, sh));
		sh = sub(sh, 14);
		for (int i = 0; i < L_SUBFR; i++) {
			temp2 = mult_r(excg[i], temp1);
			excg[i] = shr_r(temp2, sh);
		}

		/* plus the adaptive codebook's */
		pred_lt_3(cur_exc, t0, frac, L_SUBFR);
		i16 max = 0;
		for (int i = 0; i < L_SUBFR; i++) {
			temp1 = mult_r(cur_exc[i], Gp2);
			temp1 = add(temp1, excg[i]);
			cur_exc[i] = temp1;
			temp1 = abs_s(temp1);
			if (sub(temp1, max) > 0)
				max = temp1;
		}
		if (max == 0) {
			sh = 0;
		} else {
			sh = sub(3, norm_s(max));
			if (sh <= 0)
				sh = 0;
		}
		for (int i = 0; i < L_SUBFR; i++)
			excs[i] = shr(cur_exc[i], sh);

		/* the fixed codebook's gain: 4x^2 + 2bx + c = 0 */
		i32 ener = 0;
		for (int i = 0; i < L_SUBFR; i++)
			ener = L_mac(ener, excs[i], excs[i]);
		i16 inter_exc = 0;
		for (int i = 0; i < 4; i++) {
			const int j = pos[i];
			if (sign[i] == 0)
				inter_exc = sub(inter_exc, excs[j]);
			else
				inter_exc = add(inter_exc, excs[j]);
		}
		acc = L_mult(cur_gain, L_SUBFR);
		acc = L_shr(acc, 6);
		temp1 = extract_l(acc);
		const i32 k = L_mult(cur_gain, temp1);
		temp1 = add(1, shl(sh, 1));
		acc = L_shr(k, temp1);
		acc = L_sub(acc, ener);
		inter_exc = shr(inter_exc, 1);
		acc = L_mac(acc, inter_exc, inter_exc);
		sh = add(sh, 1);
		if (acc < 0) {
			/* no adaptive codebook */
			for (int i = 0; i < L_SUBFR; i++)
				cur_exc[i] = excg[i];
			temp1 = static_cast<i16>(abs_s(excg[pos[0]]) |
						 abs_s(excg[pos[1]]));
			temp2 = static_cast<i16>(abs_s(excg[pos[2]]) |
						 abs_s(excg[pos[3]]));
			temp1 = static_cast<i16>(temp1 | temp2);
			sh = (temp1 & 0x4000) == 0 ? 1 : 2;
			inter_exc = 0;
			for (int i = 0; i < 4; i++) {
				temp1 = shr(excg[pos[i]], sh);
				if (sign[i] == 0)
					inter_exc = sub(inter_exc, temp1);
				else
					inter_exc = add(inter_exc, temp1);
			}
			L_Extract(k, &hi, &lo);
			acc = Mpy_32_16(hi, lo, K0);
			temp1 = sub(shl(sh, 1), 1);
			acc = L_shr(acc, temp1);
			acc = L_mac(acc, inter_exc, inter_exc);
			Gp = 0;
		}
		temp2 = sqrt_half(acc);
		i16 x1 = sub(temp2, inter_exc);
		const i16 x2 = negate(add(inter_exc, temp2));
		if (sub(abs_s(x2), abs_s(x1)) < 0)
			x1 = x2;
		temp1 = sub(2, sh);
		i16 g = shr_r(x1, temp1);
		if (g >= 0) {
			if (sub(g, G_MAX) > 0)
				g = G_MAX;
		} else {
			if (add(g, G_MAX) < 0)
				g = negate(G_MAX);
		}
		for (int i = 0; i < 4; i++) {
			const int j = pos[i];
			if (sign[i] != 0)
				cur_exc[j] = add(cur_exc[j], g);
			else
				cur_exc[j] = sub(cur_exc[j], g);
		}
		if (exc_err)
			update_exc_err(exc_err, Gp, t0);
		cur_exc += L_SUBFR;
	}
}

/* an energy L_x (exponent sh) to the SID gain's index, its level in
   dB in enerq */
static i16 quant_energy(i32 L_x, i16 sh, i16 *enerq)
{
	i16 exp, frac;
	Log2(L_x, &exp, &frac);
	i16 temp = sub(exp, sh);
	i16 e_tmp = shl(temp, 10);
	e_tmp = add(e_tmp, mult_r(frac, 1024));
	temp = sub(e_tmp, -2721);
	if (temp <= 0) {
		*enerq = -12;
		return 0;
	}
	temp = sub(e_tmp, 22111);
	if (temp > 0) {
		*enerq = 66;
		return 31;
	}
	temp = sub(e_tmp, 4762);
	if (temp <= 0) {
		e_tmp = add(e_tmp, 3401);
		i16 index = mult(e_tmp, 24);
		if (index < 1)
			index = 1;
		*enerq = sub(shl(index, 2), 8);
		return index;
	}
	e_tmp = sub(e_tmp, 340);
	i16 index = sub(shr(mult(e_tmp, 193), 2), 1);
	if (index < 6)
		index = 6;
	*enerq = add(shl(index, 1), 4);
	return index;
}

/* (see g729a_int.h) */
i16 qua_sidgain(const i16 *ener, const i16 *sh_ener, i16 nb_ener,
		i16 *enerq)
{
	i32 L_x;
	i16 sh1, hi, lo;
	if (nb_ener == 0) {
		i32 acc = L_deposit_l(*ener);
		acc = L_shl(acc, *sh_ener);
		L_Extract(acc, &hi, &lo);
		L_x = Mpy_32_16(hi, lo, fact[0]);
		sh1 = 0;
	} else {
		sh1 = sh_ener[0];
		for (int i = 1; i < nb_ener; i++)
			if (sh_ener[i] < sh1)
				sh1 = sh_ener[i];
		sh1 = add(sh1, static_cast<i16>(16 - marg[nb_ener]));
		L_x = 0;
		for (int i = 0; i < nb_ener; i++) {
			const i16 temp = sub(sh1, sh_ener[i]);
			L_x = L_add(L_x, L_shl(L_deposit_l(ener[i]), temp));
		}
		L_Extract(L_x, &hi, &lo);
		L_x = Mpy_32_16(hi, lo, fact[nb_ener]);
	}
	return quant_energy(L_x, sh1, enerq);
}

/* (see g729a_int.h) */
i16 sid_gain_of(i16 index)
{
	return tab_Sidgain[index & 31];
}

/* the first stage's K best of J predictions' errors d_data against
   its MQ codewords (PtrTab into lspcb1): their errors new_d_data, which
   codeword and which prediction each */
static void ml_search_1(const i16 *d_data, int J, i16 *new_d_data, int K,
			i16 *best_indx, i16 *ptr_back, const i16 *PtrTab,
			int MQ)
{
	i16 sum[R_LSFQ * R_LSFQ], min[R_LSFQ], min_p[R_LSFQ], min_m[R_LSFQ];
	for (int q = 0; q < K; q++)
		min[q] = MAX16;
	for (int p = 0; p < J; p++)
		for (int m = 0; m < MQ; m++) {
			i32 acc = 0;
			for (int l = 0; l < M; l++) {
				const i16 tmp = sub(d_data[p * M + l],
						    lspcb1[PtrTab[m]][l]);
				acc = L_mac(acc, tmp, tmp);
			}
			sum[p * MQ + m] = mult(extract_h(acc), Mp[p]);
		}
	for (int q = 0; q < K; q++) {
		min_p[q] = 0;
		min_m[q] = 0;
		for (int p = 0; p < J; p++)
			for (int m = 0; m < MQ; m++)
				if (sub(sum[p * MQ + m], min[q]) < 0) {
					min[q] = sum[p * MQ + m];
					min_p[q] = static_cast<i16>(p);
					min_m[q] = static_cast<i16>(m);
				}
		sum[min_p[q] * MQ + min_m[q]] = MAX16;
	}
	for (int q = 0; q < K; q++) {
		for (int l = 0; l < M; l++)
			new_d_data[q * M + l] = sub(d_data[min_p[q] * M + l],
						    lspcb1[PtrTab[min_m[q]]][l]);
		ptr_back[q] = min_p[q];
		best_indx[q] = min_m[q];
	}
}

/* (see ml_search_1) the second stage, weighted, its two halves'
   codewords from PtrTab's two rows of lspcb2 */
static void ml_search_2(const i16 *d_data, const i16 *weight, int J,
			int K, i16 *best_indx, const i16 *ptr_prd,
			i16 *ptr_back, const i16 (*PtrTab)[16], int MQ)
{
	i16 sum[R_LSFQ * R_LSFQ], min[R_LSFQ], min_p[R_LSFQ], min_m[R_LSFQ];
	for (int q = 0; q < K; q++)
		min[q] = MAX16;
	for (int p = 0; p < J; p++)
		for (int m = 0; m < MQ; m++) {
			i32 acc = 0;
			for (int l = 0; l < M; l++) {
				const i16 *cb = lspcb2[PtrTab[l < M / 2 ? 0 : 1][m]];
				const i16 fs = noise_fg_sum[ptr_prd[p]][l];
				i16 tmp1 = extract_h(L_shl(L_mult(fs, fs), 2));
				tmp1 = mult(tmp1, weight[l]);
				const i16 tmp2 = sub(d_data[p * M + l], cb[l]);
				tmp1 = extract_h(L_shl(L_mult(tmp1, tmp2), 3));
				acc = L_mac(acc, tmp1, tmp2);
			}
			sum[p * MQ + m] = extract_h(acc);
		}
	for (int q = 0; q < K; q++) {
		min_p[q] = 0;
		min_m[q] = 0;
		for (int p = 0; p < J; p++)
			for (int m = 0; m < MQ; m++)
				if (sub(sum[p * MQ + m], min[q]) < 0) {
					min[q] = sum[p * MQ + m];
					min_p[q] = static_cast<i16>(p);
					min_m[q] = static_cast<i16>(m);
				}
		sum[min_p[q] * MQ + min_m[q]] = MAX16;
	}
	for (int q = 0; q < K; q++) {
		ptr_back[q] = min_p[q];
		best_indx[q] = min_m[q];
	}
}

/* the SID codebooks' two stages for indices (cluster) into qlsf */
static void sid_codeword(const i16 *cluster, i16 *qlsf)
{
	for (int i = 0; i < M; i++)
		qlsf[i] = lspcb1[PtrTab_1[cluster[0]]][i];
	for (int i = 0; i < M / 2; i++)
		qlsf[i] = add(qlsf[i], lspcb2[PtrTab_2[0][cluster[1]]][i]);
	for (int i = M / 2; i < M; i++)
		qlsf[i] = add(qlsf[i], lspcb2[PtrTab_2[1][cluster[1]]][i]);
}

/* the quantised error, its MA past and its LSPs (B.4.2.1/G.729) */
static void sid_compose(i16 *tmpbuf, i16 mode, i16 *lspq, LspPast &past)
{
	i16 lsfq[M];
	lsp_prev_compose(tmpbuf, lsfq, noise_fg(mode), noise_fg_sum[mode],
			 past);
	lsp_prev_update(tmpbuf, past);
	lsp_stability(lsfq);
	lsf_lsp2(lsfq, lspq);
}

/* (see g729a_int.h) */
void lsfq_noise(const i16 *lsp, i16 *lspq, LspPast &past, i16 *ana)
{
	i16 lsf[M], weight[M], errlsf[M * MODE];
	lsp_lsf2(lsp, lsf);
	if (lsf[0] < L_LIMIT)
		lsf[0] = L_LIMIT;
	for (int i = 0; i < M - 1; i++)
		if (sub(lsf[i + 1], lsf[i]) < 2 * GAP3)
			lsf[i + 1] = add(lsf[i], 2 * GAP3);
	if (lsf[M - 1] > M_LIMIT)
		lsf[M - 1] = M_LIMIT;
	if (lsf[M - 1] < lsf[M - 2])
		lsf[M - 2] = sub(lsf[M - 1], GAP3);
	get_wegt(lsf, weight);
	for (int mode = 0; mode < MODE; mode++)
		lsp_prev_extract(lsf, errlsf + mode * M, noise_fg(mode),
				 noise_fg_sum_inv[mode], past);

	/* two stages, 4 candidates after the first */
	i16 d_data[R_LSFQ * M], best0[R_LSFQ], best1[R_LSFQ];
	i16 back0[R_LSFQ], back1[R_LSFQ];
	ml_search_1(errlsf, MODE, d_data, 4, best0, back0, PtrTab_1, 32);
	ml_search_2(d_data, weight, 4, 1, best1, back0, back1, PtrTab_2, 16);
	i16 cluster[2];
	cluster[1] = best1[0];
	const i16 ptr = back1[0];
	cluster[0] = best0[ptr];
	const i16 mode = back0[ptr];
	ana[0] = mode;
	ana[1] = cluster[0];
	ana[2] = cluster[1];

	i16 tmpbuf[M];
	sid_codeword(cluster, tmpbuf);
	lsp_expand(tmpbuf, 10, 1, M);
	sid_compose(tmpbuf, mode, lspq, past);
}

/* (see g729a_int.h) */
void sid_lsfq_decode(const i16 *index, i16 *lspq, LspPast &past)
{
	i16 tmpbuf[M];
	const i16 cluster[2] = { static_cast<i16>(index[1] & 31),
				 static_cast<i16>(index[2] & 15) };
	sid_codeword(cluster, tmpbuf);
	for (int j = 1; j < M; j++) {
		i32 acc = L_mult(tmpbuf[j - 1], 16384);
		acc = L_mac(acc, tmpbuf[j], -16384);
		acc = L_mac(acc, 10, 16384);
		const i16 k = extract_h(acc);
		if (k > 0) {
			tmpbuf[j - 1] = sub(tmpbuf[j - 1], k);
			tmpbuf[j] = add(tmpbuf[j], k);
		}
	}
	sid_compose(tmpbuf, static_cast<i16>(index[0] & 1), lspq, past);
}

} // namespace g729
} // namespace dlink
