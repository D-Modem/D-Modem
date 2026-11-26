/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    x2_rx.cpp - x2's client: the PCM receiver (x2_rx.h,
 *                docs/dsp/x2-details.md)
 *
 *    The preamble repeats 22 codes: the line has x(n) close to
 *    x(n - 22).  Its last 11 codes have their signs turned, where for
 *    11 samples x(n) is -x(n - 22); the last of them is code 362.  From
 *    there the server's training is X2Train's from its preamble, so the
 *    equaliser (as K56Rx's: T/2, solved by least squares, then NLMS,
 *    with its timing loop) trains on what is known up to the sync
 *    frames.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstring>

#include "dm_types.h"
#include "bits.h"
#include "dsp.h"
#include "dsplibs.h"
#include "g711.h"
#include "k56flex.h"
#include "v90.h"
#include "x2_rx.h"

namespace dsp {

#define P_MIN_RUN       88      /* samples of the preamble (4 periods) */
#define P_MIN_RMS       30.0
#define P_TURN          362     /* the turn's last code */
#define EQ_D            X2RX_EQ_D
#define REFINE_N        96
#define REFINE_D        20
/* the sync frames' ones in a row for ev_sync */
#define SYNC_ONES       24
/* the start's frames of ones in the data levels before the data */
#define START_ONES      680

/* the pads' factors (none, 3 dB, 6 dB) */
static const double x2rx_pads[X2_MODES] = { 1, 0.7079457843841379,
					    0.5011872336272722 };

/* a positive mu-law octet's value across pad m (as the network's: the
   value times the pad, to the law again) */
static double pad_value(unsigned char octet, s16 m)
{
	double v = fabs(static_cast<double>(g711::ulaw2lin(octet)));
	if (!m)
		return v;
	return fabs(static_cast<double>(g711::ulaw2lin(g711::lin2ulaw(
		static_cast<s32>(lrint(v * x2rx_pads[m]))))));
}

/* the equaliser's gain across pad m: TRN's octet at its own value */
static double pad_gain(s16 m)
{
	return pad_value(0xb1, 0) / pad_value(0xb1, m);
}

/* see x2_rx.h: a rate with octets of its own for the pad (tx) has the
   levels received in lv; one without (A, B, mode 0's) sends lv */
void X2Levels::init(const X2Rate *rate, s16 m)
{
	double g = pad_gain(m);
	r = rate;
	for (s16 i = 0; i < X2_FRAME; i++)
		for (s16 d = 0; d < rate->n[i]; d++)
			mag[i][d] = static_cast<float>(g * (rate->tx[i] ?
				pad_value(rate->lv[i][d], 0) :
				pad_value(rate->lv[i][d], m)));
}

/* see x2_rx.h: the magnitudes fall with the digit */
s16 X2Levels::decide(s16 i, float y, float *e) const
{
	float ay = fabsf(y);
	s16 lo = 0, hi = static_cast<s16>(r->n[i] - 1);
	while (hi - lo > 1) {
		s16 mid = static_cast<s16>((lo + hi) / 2);
		if (mag[i][mid] >= ay)
			lo = mid;
		else
			hi = mid;
	}
	s16 d = fabsf(ay - mag[i][lo]) <= fabsf(ay - mag[i][hi]) ? lo : hi;
	float a = y > 0 ? mag[i][d] : -mag[i][d];
	*e = (a - y) * (a - y);
	return d;
}

/* see x2_rx.h */
void X2Rx::init(s16 alaw_, s16 rate, const X2Rate *data)
{
	reinit(*this);
	alaw = alaw_;
	/* (the server sends mu-law's values, whatever the law: x2_sample()) */
	for (s16 i = 0; i < 128; i++)
		lin[i] = v90_ucode_linear(0, i);
	level = static_cast<float>(lin[k56_code(0xb1, 0) & 0x7f]);
	ip.init(rate);
	stage = WAIT_PRE;
	dip = 1;
	lv_a.init(&x2_rate_a);
	lv_b.init(data ? data : x2_rate_for(1));
	lv_d = lv_b;
	cur = &lv_a;
	last = 1;
	dfr = -1;
}

/* a code's value at our receiver, as the server sends it */
float X2Rx::value(unsigned char c) const
{
	float v = static_cast<float>(lin[c & 0x7f]);
	return c & 0x80 ? v : -v;
}

/* the preamble's sums with the newest 8 kHz sample: there
   P_MIN_RUN samples, then its turn (the lag 22 correlation over 11
   samples at its least) */
void X2Rx::pre_detect(float)
{
	s32 i = n8 - 1, j;
	if (i < 44 + 22)
		return;
	e44 = a44 = e11 = a11 = 0;
	for (j = 0; j < 44; j++) {
		double v = at8(i - j), u = at8(i - j - 22);
		e44 += v * v;
		a44 += v * u;
		if (j < 11) {
			e11 += v * v;
			a11 += v * u;
		}
	}
	bool there = e44 > 44 * P_MIN_RMS * P_MIN_RMS && a44 > 0.8 * e44;
	if (dsplibs_debug_level >= 9 && i % 400 == 0)
		DSP_DBG("x2rx: %" PRId32 ": rms %.1f, lag 22 %.2f\n", i,
			sqrt(e44 / 44), e44 > 0 ? a44 / e44 : 0);
	double r = e11 > 0 ? a11 / e11 : 0;
	if (!p_on) {
		p_run = there ? static_cast<s16>(p_run + 1) : 0;
		if (p_run == P_MIN_RUN) {
			p_on = 1;
			p_lost = 0;
			DSP_DBG("x2rx: preamble at sample %" PRId32 "\n", 2 * i);
		}
		return;
	}
	if (!p_dip && r < -0.5) {
		p_dip = 1;
		dip = 1;
	}
	if (p_dip && r < dip) {
		dip = r;
		dip_at = i;
	}
	if (p_dip && (r > -0.2 || i - dip_at > 22)) {
		if (dip > -0.7) {
			p_dip = 0;
			dip = 1;
			return;
		}
		b = 2 * (dip_at - P_TURN);
		k = 0;
		stage = TRAIN;
		ev_pre = 1;
		ref.init();
		ref.segment(X2_PRE);
		DSP_DBG("x2rx: the preamble's turn at sample %" PRId32
			" (%.2f)\n", 2 * dip_at, dip);
		return;
	}
	if (p_dip)
		return;
	p_lost = there ? 0 : static_cast<s16>(p_lost + 1);
	if (p_lost > 44) {
		p_on = 0;
		p_run = 0;
		DSP_DBG("x2rx: preamble lost\n");
	}
}

/* the clock's offset fitted (PcmEqualizer::timing_step()) */
void X2Rx::timing_locked(double) const
{
	DSP_DBG("x2rx: timing %+.2f ppm\n", ppm());
}

/* the delay centred: the shift of b that correlates best with TRN's
   first REFINE_N values */
void X2Rx::refine()
{
	double best = -1;
	s16 d, j, bd = 0;
	for (d = -REFINE_D; d <= REFINE_D; d++) {
		double cc = 0;
		for (j = 0; j < REFINE_N; j++)
			cc += trn0[j] * at(b + 2 * (X2RX_K_TRN + j) + d);
		if (cc > best) {
			best = cc;
			bd = d;
		}
	}
	b += bd;
	refined = 1;
	DSP_DBG("x2rx: TRN's delay %+d samples\n", bd);
}

/* code k's output y against a known (or decided) value a: the equaliser
   and the timing follow it */
void X2Rx::known(float y, float a)
{
	float e = a - y;
	adapt(k, e);
	timing(k, y, a);
	err_sum += static_cast<double>(e) * e;
	if (++err_n == 4096) {
		mse = static_cast<float>(err_sum / err_n /
					 (static_cast<double>(level) * level));
		DSP_DBG("x2rx: equaliser mse %.5f (%.1f dB) at code %" PRId32
			", timing %+.2f ppm\n", mse, -10 * log10(mse + 1e-12),
			k, ppm());
		err_sum = 0;
		err_n = 0;
	}
}

/* TRN's and the J words' signs: GPC's descrambler from TRN on, the J
   words' 12 bits (09B0 99 times, then 099F) */
void X2Rx::js_sign(float y)
{
	s16 d = descramble(&dscr, SCR_GPC, y < 0);
	if (k < X2RX_K_JS)
		return;
	s32 j = k - X2RX_K_JS;
	jw = static_cast<u16>(jw | d << (j % 12));
	if (j % 12 != 11)
		return;
	if (j / 12 < 99) {
		if (jw == 0x09b0)
			jw_good++;
		else
			jw_bad++;
	} else {
		jw_end = jw == 0x099f;
		ev_js = jw_good >= 90 && jw_end ? 1 : -1;
		DSP_DBG("x2rx: J words: %d good, %d not, the last %#05x\n",
			jw_good, jw_bad, jw);
	}
	jw = 0;
}

/* a frame's decisions y back to its bits (the descrambler s and the
   last sign lst on from it); their squared error into err */
s16 X2Rx::unmap(const X2Levels &L, const float *y, u32 *s, s16 *lst,
		unsigned char *bits, float *err) const
{
	const X2Rate &r = *L.r;
	s16 d[X2_FRAME], car[X2_FRAME] = {}, i, j, nb = 0;
	float e;
	*err = 0;
	for (i = 0; i < X2_FRAME; i++) {
		d[i] = L.decide(i, y[i], &e);
		*err += e;
	}
	for (j = 0; j < r.MD; j++) {
		s16 best = -1;
		for (i = 0; i < X2_FRAME; i++)
			if (!car[i] && (best < 0 || r.lv[i][d[i]] >
					r.lv[best][d[best]]))
				best = i;
		car[best] = 1;
	}
	for (i = 0; i < X2_FRAME; i++)
		if (car[i]) {
			s16 sg = y[i] > 0 ? 1 : 0;
			bits[nb++] = static_cast<unsigned char>(
				descramble(s, SCR_GPC, sg ^ *lst));
			*lst = sg;
		}
	u64 xx = 0;
	for (i = X2_FRAME - 1; i >= 0; i--)
		xx = xx * static_cast<u64>(r.n[i]) + static_cast<u64>(d[i]);
	for (i = 0; i < r.B; i++)
		bits[nb++] = static_cast<unsigned char>(
			descramble(s, SCR_GPC, (xx >> i) & 1));
	return nb;
}

/* a bit of the frames in levels A: the last 85 the server's record (17
   ones, a zero, 3 words, the CRC)? */
void X2Rx::rec_bit(unsigned char bit)
{
	memmove(rbits, rbits + 1, 84);
	rbits[84] = bit;
	if (++rbit_n < 85 || rbits[17])
		return;
	for (s16 i = 0; i < 17; i++)
		if (!rbits[i])
			return;
	u16 w[3];
	if (x2_record_parse(rbits, 3, w))
		return;
	if (!rec_n || w[0] != rec_w[0])
		DSP_DBG("x2rx: the server's record %04x %04x %04x (frame %"
			PRId32 ")\n", w[0], w[1], w[2], frames);
	memcpy(rec_w, w, sizeof rec_w);
	rec_n++;
	ev_rec = 1;
}

/* a whole frame of decisions (fy) */
void X2Rx::frame()
{
	unsigned char bits[X2_MAXBITS], bb[X2_MAXBITS];
	float err, eb;
	s16 nb, i, ones;
	u32 s = sh;
	s16 l = last;
	frames++;
	if (dfr < 0) {
		nb = unmap(lv_a, fy, &s, &l, bits, &err);
		for (i = 0, ones = 0; i < nb; i++)
			ones = static_cast<s16>(ones + bits[i]);
		if (e_sent) {
			/* the start: its frames of ones in the data levels */
			u32 s2 = sh;
			s16 l2 = last, nb2, ones2 = 0;
			nb2 = unmap(lv_b, fy, &s2, &l2, bb, &eb);
			for (i = 0; i < nb2; i++)
				ones2 = static_cast<s16>(ones2 + bb[i]);
			if (ones2 == nb2 && eb < 0.5f * err) {
				sh = s2;
				last = l2;
				dfr = 1;
				cur = &lv_b;
				ev_start = 1;
				DSP_DBG("x2rx: the start's ones in the data "
					"levels (frame %" PRId32 ")\n",
					frames);
				return;
			}
		}
		sh = s;
		last = l;
		if (ones == nb) {
			if (++ones_run == SYNC_ONES && !ev_sync) {
				ev_sync = 1;
				DSP_DBG("x2rx: the sync frames' ones (frame %"
					PRId32 ")\n", frames);
			}
			nz_sum += err;
			for (i = 0; i < X2_FRAME; i++) {
				float e;
				s16 d = lv_a.decide(i, fy[i], &e);
				sg_sum += static_cast<double>(fy[i]) * fy[i];
				pad_sum[d] += fabsf(fy[i]);
				pad_n[d]++;
			}
			nz_n++;
		} else
			ones_run = 0;
		for (i = 0; i < nb; i++)
			rec_bit(bits[i]);
		return;
	}
	if (dfr >= START_ONES) {
		data_frame();
		return;
	}
	nb = unmap(*cur, fy, &sh, &last, bits, &err);
	dfr++;
	for (i = 0, ones = 0; i < nb; i++)
		ones = static_cast<s16>(ones + bits[i]);
	if (ones < nb)
		err_frames++;
	if (dfr == START_ONES) {
		cur = &lv_d;
		ev_data = 1;
		DSP_DBG("x2rx: data from frame %" PRId32 ", %" PRId32
			" of the start's ones wrong, snr %.1f dB\n",
			frames, err_frames, snr_db());
	}
}

/* a frame of the marker: each position at its largest level, the
   signs + + + - - - (turned: - - - + + +) */
bool X2Rx::marker_frame(bool turned) const
{
	for (s16 i = 0; i < X2_FRAME; i++) {
		float e;
		if (cur->decide(i, fy[i], &e) != 0 ||
		    (fy[i] > 0) != ((i < 3) != turned))
			return false;
	}
	return true;
}

/* a frame in data mode: its bits for the caller; or the stages of a
   rate change */
void X2Rx::data_frame()
{
	unsigned char bits[X2_MAXBITS];
	float err;
	s16 nb, i, ones;
	switch (rn) {
	case RN_NONE:
		/* (a marker's frame carries no data) */
		if (marker_frame(false)) {
			if (++rn_mark == 4) {
				rn = RN_MARK;
				rn_fr = 0;
				ev_marker = 1;
				DSP_DBG("x2rx: the server's marker (frame %"
					PRId32 ")\n", frames);
			}
			return;
		}
		rn_mark = 0;
		break;
	case RN_MARK:
		if (marker_frame(true)) {
			/* its records next: the scrambler from zero, the
			   start-up's data levels */
			rn = RN_REC;
			rn_fr = 0;
			sh = 0;
			last = 1;
			cur = &lv_b;
			rbit_n = 0;
			return;
		}
		if (++rn_fr > 16) {
			DSP_DBG("x2rx: no end to the marker\n");
			rn = RN_NONE;
			rn_fr = 0;
		}
		return;
	case RN_REC:
		nb = unmap(*cur, fy, &sh, &last, bits, &err);
		for (i = 0, ones = 0; i < nb; i++) {
			ones = static_cast<s16>(ones + bits[i]);
			s16 r0 = rec_n;
			rec_bit(bits[i]);
			if (rec_n != r0)
				ev_rn_rec = 1;
		}
		/* after its 43 records (172 frames) a frame of ones */
		if (++rn_fr >= 170 && ones == nb) {
			rn = RN_RATE;
			rn_fr = 0;
		}
		if (rn_fr > 400) {
			DSP_DBG("x2rx: no end to the server's records\n");
			rn = RN_RATE;
			rn_fr = 0;
		}
		return;
	case RN_RATE: {
		/* the rate whose levels decode the frame to ones */
		s16 best = 0, k, n;
		float be = 1e30f;
		u32 bs = 0;
		s16 bl = 1;
		for (k = 0; k <= 15; k++) {
			/* (the one asked for first, then from 15 down) */
			n = !k ? rn_want : static_cast<s16>(16 - k);
			if (!n || (k && n == rn_want))
				continue;
			const X2Rate *r = x2_rate_for(n, mode);
			if (!r)
				continue;
			lv_n.init(r, mode);
			u32 s2 = sh;
			s16 l2 = last;
			nb = unmap(lv_n, fy, &s2, &l2, bits, &err);
			for (i = 0, ones = 0; i < nb; i++)
				ones = static_cast<s16>(ones + bits[i]);
			if (ones == nb && err < be) {
				be = err;
				best = n;
				bs = s2;
				bl = l2;
				if (n == rn_want)
					break;
			}
		}
		if (!best) {
			if (++rn_fr > 8) {
				DSP_DBG("x2rx: no next rate's ones\n");
				rn = RN_NONE;
				cur = &lv_d;
			}
			/* (the scrambler on, as with any of them) */
			return;
		}
		lv_n.init(x2_rate_for(best, mode), mode);
		lv_d = lv_n;
		cur = &lv_d;
		sh = bs;
		last = bl;
		rn_down = best;
		rn = RN_ONES;
		rn_fr = 1;
		ev_rn_rate = 1;
		DSP_DBG("x2rx: the next rate: %d (%d bit/s, frame %" PRId32
			")\n", best, (lv_d.r->B + lv_d.r->MD) * 8000 /
			X2_FRAME, frames);
		return;
	}
	case RN_ONES:
		nb = unmap(*cur, fy, &sh, &last, bits, &err);
		if (++rn_fr == X2RX_NEXT_ONES) {
			rn = RN_NONE;
			rn_mark = 0;
			ev_rn_data = 1;
			DSP_DBG("x2rx: data at the next rate (frame %" PRId32
				")\n", frames);
		}
		return;
	}
	nb = unmap(*cur, fy, &sh, &last, bits, &err);
	for (i = 0; i < nb && qn < X2RX_Q; i++) {
		q[(qh + qn) % X2RX_Q] = bits[i];
		qn++;
	}
}

/* a code of the sync segment on: its decision (the frame's levels, the
   position's), the equaliser and timing on it */
void X2Rx::code(float y)
{
	float e;
	s16 d = cur->decide(fpos, y, &e);
	float a = y > 0 ? cur->mag[fpos][d] : -cur->mag[fpos][d];
	/* (in the data, a gap of silence: no training on it, no error
	   counted) */
	bool held = dfr >= 0 && gap.step(y, dpow);
	if (!held)
		known(y, a);
	if (dfr >= 0) {
		dpow += 0.001f * (a * a - dpow);
		if (!held)
			de_mse += 0.001f * (e - de_mse);
	}
	fy[fpos++] = y;
	if (fpos == X2_FRAME) {
		fpos = 0;
		frame();
	}
}

/* see x2_rx.h */
double X2Rx::snr_db() const
{
	return nz_sum > 0 ? 10 * log10(sg_sum / nz_sum) : 99;
}

/* see x2_rx.h */
s16 X2Rx::best_rate() const
{
	/* the data's decisions once there (the equaliser has settled on
	   the pad's levels by then), else the sync frames' */
	if (nz_n <= 0 && !ev_data)
		return 1;
	double sd = ev_data ? sqrt(static_cast<double>(de_mse)) :
		sqrt(nz_sum / (nz_n * X2_FRAME));
	for (s16 n = 15; n > 1; n--) {
		const X2Rate *r = x2_rate_for(n, mode);
		if (!r)
			continue;
		X2Levels L;
		L.init(r, mode);
		float dmin = 1e30f;
		for (s16 i = 0; i < X2_FRAME; i++) {
			s16 m = r->n[i];
			for (s16 d = 0; d + 1 < m; d++)
				dmin = fminf(dmin, L.mag[i][d] -
						   L.mag[i][d + 1]);
			dmin = fminf(dmin, 2 * L.mag[i][m - 1]);
		}
		if (dmin / 2 >= 5.5 * sd)
			return n;
	}
	return 1;
}

/* see x2_rx.h: each pad's levels A with one gain fitted to the means
   (least squares), the best fit's pad */
s16 X2Rx::measure_pad()
{
	s16 d, m, best = 0;
	for (d = 0; d < 9; d++)
		if (pad_n[d] < 20)
			return -1;
	for (m = 0; m < X2_MODES; m++) {
		X2Levels L;
		L.init(&x2_rate_a, m);
		double se = 0, sm = 0, sme = 0, smm = 0;
		for (d = 0; d < 9; d++) {
			double mv = pad_sum[d] / pad_n[d], e = L.mag[0][d];
			se += e * e;
			sme += mv * e;
			smm += mv * mv;
		}
		double a = sme / se;
		for (d = 0; d < 9; d++) {
			double mv = pad_sum[d] / pad_n[d], e = L.mag[0][d];
			sm += (mv - a * e) * (mv - a * e);
		}
		pad_res[m] = sm / smm;
		if (pad_res[m] < pad_res[best])
			best = m;
	}
	DSP_DBG("x2rx: the pad: %s (fits %.2e, %.2e, %.2e)\n",
		best == 0 ? "none" : best == 1 ? "3 dB" : "6 dB", pad_res[0],
		pad_res[1], pad_res[2]);
	if (best != mode) {
		mode = best;
		lv_a.init(&x2_rate_a, mode);
		lv_b.init(x2_rate_for(1, mode), mode);
		lv_d = lv_b;
		/* the noise again in these levels */
		nz_sum = sg_sum = 0;
		nz_n = 0;
	}
	return mode;
}

/* see x2_rx.h */
s16 X2Rx::take(unsigned char *bo, s16 nmax)
{
	s16 m = 0;
	while (m < nmax && qn) {
		bo[m++] = q[qh];
		qh = static_cast<s16>((qh + 1) % X2RX_Q);
		qn--;
	}
	return m;
}

/* see x2_rx.h */
void X2Rx::line(float v)
{
	float o[3];
	s16 m = ip.run(v, o);
	for (s16 i = 0; i < m; i++)
		sample(o[i]);
}

/* one sample at 16 000 Hz: the preamble's detector, then the codes */
void X2Rx::sample(float v)
{
	x[n & (X2RX_HIST - 1)] = v;
	n++;
	if (stage == WAIT_PRE) {
		if (n & 1) {
			x8[n8 & (X2RX_HIST8 - 1)] = v;
			n8++;
			pre_detect(v);
		}
		return;
	}
	while (b + 2 * k + EQ_D <= n - 1) {
		float a = 0, y;
		if (k < X2RX_K_SYNC) {
			a = value(ref.next());
			if (k >= X2RX_K_TRN && k < X2RX_K_TRN + REFINE_N)
				trn0[k - X2RX_K_TRN] = a;
		}
		if (k == X2RX_K_TRN + REFINE_N && !refined)
			refine();
		if (stage == TRAIN) {
			if (k > X2RX_K_TRN + REFINE_N) {
				ls_add(k, a);
				if (++ls2_n == X2RX_LS) {
					solve();
					ls2_n = 0;
					stage = RUN;
					ev_trained = 1;
					t_on = 1;
					t_pow = static_cast<double>(level) *
						level;
					DSP_DBG("x2rx: equaliser solved at "
						"code %" PRId32 "\n", k);
				}
			}
			k++;
			continue;
		}
		y = equalize(k);
		if (k < X2RX_K_SYNC) {
			/* known: once the timing is locked, solved again */
			if (t_n >= TIM_LOCK && ls2_n < X2RX_LS &&
			    k < X2RX_K_JS) {
				if (!ls2_n) {
					zero(R);
					zero(p);
				}
				ls_add(k, a);
				if (++ls2_n == X2RX_LS) {
					solve();
					y = equalize(k);
					DSP_DBG("x2rx: equaliser solved again "
						"at code %" PRId32 ", timing "
						"%+.2f ppm\n", k, ppm());
				}
			}
			if (k >= X2RX_K_TRN && k < X2RX_K_ONES)
				js_sign(y);
			known(y, a);
			k++;
			continue;
		}
		code(y);
		k++;
	}
}

} // namespace dsp
