/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v34_modem.cpp - V.34 primary channel: the phase 3 and 4 training
 *                  signals (S, S-bar, PP, TRN, J, J', MP, E) and the
 *                  receiver that trains on them
 *
 *    As the original library sends them:
 *    S        alternating 4 point training symbols at 45 and 135 degrees
 *    S-bar    the same, reversed
 *    PP       6 * the 48 symbol sequence of 10.1.3.6/V.34
 *    TRN      scrambled ones, two bits per symbol, absolute mapping
 *    J, J', MP, E  scrambled, differentially coded with two bits per
 *             symbol (J = 0x8990, J' = 0x899F, least significant bit
 *             first; E = 20 ones)
 *
 *    The caller scrambles with 1 + x^-18 + x^-23, the answerer with
 *    1 + x^-5 + x^-23.
 *
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "bits.h"
#include "v34.h"
#include "v90.h"

namespace dsp {

#define V34_SRATE       9600
#define V34_ALPHA       0.1f
#define V34_SPAN        16
#define V34_EQ_LEN      48
/* line level of the training and data signals, as the original library */
#define V34_PRIMARY_RMS 2790.0f

#define J_4POINT        0x8990
#define J_16POINT       0x89B0  /* bit 5: TRN of 16 points in phase 4 */
#define J_DASH          0x899F

/* the low 16 bits of w in reverse order */
static unsigned rev16(unsigned w)
{
	unsigned r = 0;
	int i;
	for (i = 0; i < 16; i++)
		r |= ((w >> i) & 1) << (15 - i);
	return r;
}


/*
 *    MP
 */

/* where an MP frame's CRC is (bits.h): after its fields, at bit 69 in
   type 0 (88 bits), 171 in type 1 (188) */
static int mp_crc_at(int len)
{
	return len == 188 ? 171 : 69;
}

/* the bits of the MP frame (type 0: 88 bits, type 1 with the precoding
   coefficients: 188), one per byte in time order, frame sync, start
   bits, CRC and fill bits included; returns its length */
int V34Mp::build(unsigned char *b) const
{
	int n = 0, i, len = type ? 188 : 88;

	put_bits(b, &n, 0x1FFFF, 17);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, type, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, rate_ca, 4);
	put_bits(b, &n, rate_ac, 4);
	put_bits(b, &n, aux, 1);
	put_bits(b, &n, trellis, 2);
	put_bits(b, &n, nonlinear, 1);
	put_bits(b, &n, expanded, 1);
	put_bits(b, &n, ack, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, rate_mask, 15);
	put_bits(b, &n, asym, 1);
	if (type)
		for (i = 0; i < 6; i++) {
			put_bits(b, &n, 0, 1);
			put_bits(b, &n, h[i], 16);
		}
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0, 16);
	put_bits(b, &n, 0, 1);
	put_frame_crc(b, &n);
	while (n < len)
		b[n++] = 0;
	return len;
}

/* the fields of an MP frame of len (88 or 188) bits, from its frame sync
   on; returns 0 when the frame is good, -1 (fields unchanged) on a bad
   CRC */
int V34Mp::parse(const unsigned char *b, int len)
{
	int i;
	if (!frame_crc_ok(b, mp_crc_at(len)))
		return -1;
	type = b[18];
	rate_ca = field(b, 20, 4);
	rate_ac = field(b, 24, 4);
	aux = b[28];
	trellis = field(b, 29, 2);
	nonlinear = b[31];
	expanded = b[32];
	ack = b[33];
	rate_mask = field(b, 35, 14);
	asym = b[50];
	for (i = 0; i < 6; i++)
		h[i] = type ? static_cast<short>(field(b, 52 + 17 * i, 16)) : 0;
	return 0;
}


/*
 *    transmitter
 */

/* point idx (0..3) of the 4 point training constellation */
static cplx tx_point(int idx)
{
	return v34_trn4[idx & 3];
}

/* 16 point training (10.1.3.8/V.34, when the remote's J asks for it):
   point q of the quarter-superconstellation, turned as tx_point() turns
   point 0, at the average power of the 4 point signals */
static cplx tx_point16(int idx, int q)
{
	const float s = 0.4472136f;     /* sqrt(2 / 10) */
	cplx z = { v34_quarter[q & 3][0] * s, v34_quarter[q & 3][1] * s };
	int k;
	for (k = 0; k < ((idx + 2) & 3); k++) {
		float x = z.re;         /* clockwise */
		z.re = z.im;
		z.im = -x;
	}
	return z;
}

/* scrambles b0, then b1; returns them as b0 | b1 << 1 */
int V34Tx::dibit(int b0, int b1)
{
	b0 = scramble(&scr, tap, b0);
	b1 = scramble(&scr, tap, b1);
	return b0 | (b1 << 1);
}

/* the next symbol (for qam.sample()) of the current segment, in units
   of the training points (silence: 0); counts it in n, and goes on from
   J' to TRN and from E to data at their ends */
static cplx tx_symbol(V34Tx *t)
{
	cplx z = { 0, 0 };
	int k, w;

	if (t->seg == V34_TX_CP && t->pos >= t->nbits) {
		/* a CP frame out: E, or the next frame */
		t->cp_frames++;
		if (t->cp_to_e)
			t->segment(V34_TX_E);
		else if (t->cp_next_n) {
			memcpy(t->bits, t->cp_next, t->cp_next_n);
			t->nbits = t->cp_next_n;
			t->cp_next_n = 0;
			t->cp_frames = 0;
		}
		t->pos = 0;
	}
	switch (t->seg) {
	case V34_TX_SILENCE:
		break;
	case V34_TX_S:
	case V34_TX_SBAR:
		/* 45 and 135 degrees; S-bar continues the alternation reversed
		   (after an S of odd length too) */
		t->idx = ((t->n + t->s_odd) & 1) ? 1 : 2;
		z = tx_point(t->idx);
		if (t->seg == V34_TX_SBAR)
			z = cplx_scale(z, -1);
		break;
	case V34_TX_PP:
		z = cplx_scale(v34_pp(t->n), 1.41421f);
		break;
	case V34_TX_TRN:
		t->idx = t->dibit(1, 1);
		z = tx_point(t->idx);
		if (t->trn16)
			z = tx_point16(t->idx, t->dibit(1, 1));
		break;
	case V34_TX_J:
	case V34_TX_JP:
		w = t->seg == V34_TX_J ? J_4POINT : J_DASH;
		k = 2 * (t->n % 8);
		t->idx = (t->idx + t->dibit((w >> k) & 1, (w >> (k + 1)) & 1)) & 3;
		z = tx_point(t->idx);
		break;
	case V34_TX_MP:
		if (t->pos >= t->nbits) {
			if (t->nbits)
				t->mp_sent++;
			t->mp.ack = t->ack_mp;
			t->nbits = t->mp.build(t->bits);
			t->pos = 0;
		}
		k = t->dibit(t->bits[t->pos], t->bits[t->pos + 1]);
		t->pos += 2;
		t->idx = (t->idx + k) & 3;
		z = tx_point(t->idx);
		if (t->trn16) {
			/* MP lengths (88, 188) are whole 4 bit symbols */
			z = tx_point16(t->idx, t->dibit(t->bits[t->pos],
						       t->bits[t->pos + 1]));
			t->pos += 2;
		}
		break;
	case V34_TX_JA:
	case V34_TX_CP:
		/* the descriptor again and again, as MP */
		if (t->pos >= t->nbits)
			t->pos = 0;
		k = t->dibit(t->bits[t->pos], t->bits[t->pos + 1]);
		t->pos += 2;
		t->idx = (t->idx + k) & 3;
		z = tx_point(t->idx);
		break;
	case V34_TX_DATA:
		if (t->data_on) {
			z = t->dtx.symbol(&t->scr, t->tap);
			break;
		}
		/* fall through */
	case V34_TX_E:
		t->idx = (t->idx + t->dibit(1, 1)) & 3;
		z = tx_point(t->idx);
		if (t->trn16)
			z = tx_point16(t->idx, t->dibit(1, 1));
		break;
	}
	t->n++;
	/* J' and E are single sequences */
	if (t->seg == V34_TX_JP && t->n == 8)
		t->segment(V34_TX_TRN);
	else if (t->seg == V34_TX_E && t->n == (t->trn16 ? 5 : 10))
		t->segment(V34_TX_DATA);
	return z;
}

/* the transmitter, silent, at symbol rate baud_ (V34_BAUD_*) on its low
   or high carrier; the caller scrambles with GPC, the answerer with
   GPA */
void V34Tx::init(int baud_, int high, int caller)
{
	reinit(*this);
	baud = baud_;
	qam.init(V34_SRATE, v34_carrier(baud_, high),
		    v34_symbol_rate(baud_), V34_ALPHA, V34_SPAN, 1.41421f,
		    V34_PRIMARY_RMS * 1.41421f);
	tap = caller ? 18 : 5;
	seg = V34_TX_SILENCE;
}

/* the next segment, from its first symbol */
void V34Tx::segment(enum v34_txseg seg_)
{
	DSP_DBG("v34 tx: segment %d -> %d after %ld symbols\n", seg, seg_, n);
	s_odd = seg_ == V34_TX_SBAR && seg == V34_TX_S ?
			static_cast<int>(n & 1) :
			0;
	seg = seg_;
	n = 0;
	if (seg_ == V34_TX_MP)
		nbits = pos = 0;
	/* (Ja's bits are the caller's) */
	if (seg_ == V34_TX_JA)
		pos = 0;
	if (seg_ == V34_TX_CP) {
		pos = 0;
		cp_next_n = 0;
		cp_frames = 0;
		cp_to_e = 0;
	}
	/* TRN starts with the scrambler cleared, the remote may train with
	   a copy of it */
	if (seg_ == V34_TX_TRN)
		scr = 0;
}

/* the data mode parameters, before the end of E: the symbols after E
   come from the data encoder, link gives the bits of each mapping
   frame */
void V34Tx::start_data(const V34Params *p, CoreBits link)
{
	dtx.init(p);
	dtx.link = link;
	data_on = 1;
}

/* the next line sample, at 9600 Hz */
float V34Tx::sample()
{
	return qam.sample([this] { return tx_symbol(this); });
}


/*
 *    receiver
 */

/* the receiver of the remote's signals at symbol rate baud_ on its low
   or high carrier, waiting for S; caller: we are the caller, the remote
   scrambles with GPA.  Returns -1 when out of memory */
int V34Rx::init(int baud_, int high, int caller)
{
	reinit(*this);
	baud = baud_;
	tap = caller ? 5 : 18;
	stage = V34_RX_IDLE;
	if (!alloc_zeroed(ls_r, V34_EQ_LEN * V34_EQ_LEN) ||
	    !alloc_zeroed(ls_p, V34_EQ_LEN))
		return -1;
	return qam.init(V34_SRATE, v34_carrier(baud_, high),
			   v34_symbol_rate(baud_), V34_ALPHA, V34_SPAN,
			   V34_EQ_LEN, 2.0f);
}

/* the receiver of a V.90 digital modem: Ja instead of J */
int V34Rx::set_v90()
{
	v90 = 1;
	fr.reset(new (std::nothrow) SyncFrames());
	return fr ? 0 : -1;
}

/* frees what init() and set_v90() allocated */
void V34Rx::release()
{
	fr.reset();
	free_buffer(ls_r);
	free_buffer(ls_p);
	qam.release();
}

/* the next stage, its symbols counted from 0; a Ja or CP frame in
   progress is dropped */
void V34Rx::set_stage(enum v34_rxstage s)
{
	DSP_DBG("v34 rx: stage %d -> %d at symbol %ld\n", stage, s, sym);
	stage = s;
	n = 0;
	ls_n = 0;
	if (fr)
		fr->drop();     /* no frame across a new signal */
}

/* the index of the 4 point training point nearest to z: its quadrant */
static int nearest4(cplx z)
{
	return z.re < 0 ? (z.im < 0 ? 0 : 1) : (z.im < 0 ? 3 : 2);
}

/* the debug log of a DIL's SP or TP bits, 40 to a line */
static void ja_dump_bits(const char *what, const unsigned char *b, int n)
{
	for (int i = 0; i < n; i += 40) {
		char line[41];
		int k;
		for (k = 0; k < 40 && i + k < n; k++)
			line[k] = '0' + b[i + k];
		line[k] = 0;
		DSP_DBG("v34 rx: DIL %s %s\n", what, line);
	}
}

/* Ja (V.90): DIL descriptors in MP style frames, repeated */
/* a frame begins (its sync seen): Ja until one has come, then CP */
void V34Rx::frame_start()
{
	if (ev_ja)
		fr->init(SyncFrames::CP90, static_cast<int>(sizeof(cpraw)));
	else
		fr->init(SyncFrames::DIL90, V90_JA_MAX);
	fr->start();
}

/* a bit of Ja: a good frame's DIL into dil, ev_ja */
void V34Rx::ja_bit(int bit)
{
	int r = fr->add(bit);
	if (r == SyncFrames::GOOD || r == SyncFrames::BAD_CRC) {
		if (r == SyncFrames::GOOD && dil.parse(fr->b, fr->len) == 0) {
			if (!ev_ja) {
				int i;
				DSP_DBG("v34 rx: Ja at symbol %ld: DIL N %d, "
					"LSP %d, LTP %d\n", sym, dil.n,
					dil.lsp, dil.ltp);
				for (i = 0; i < 8; i++)
					DSP_DBG("v34 rx: DIL H%d %d REF%d %d\n",
						i + 1, dil.h[i], i + 1,
						dil.ref[i]);
				for (i = 0; i < dil.n; i += 16) {
					char line[16 * 4 + 1];
					int k, l = 0;
					/* (no more than the DIL has) */
					for (k = i; k < i + 16 && k < dil.n; k++)
						l += snprintf(line + l, sizeof(line) - l,
							      " %d", dil.ucode[k]);
					line[l] = 0;
					DSP_DBG("v34 rx: DIL Ucodes %d:%s\n", i, line);
				}
				/* SP and TP have their own lengths */
				ja_dump_bits("SP", dil.sp, dil.lsp);
				ja_dump_bits("TP", dil.tp, dil.ltp);
			}
			ev_ja = 1;
		} else
			DSP_DBG("v34 rx: Ja with bad CRC\n");
	}
}

/* CP (V.90): MP style frames of a length given by the number of
   constellations they carry */
void V34Rx::cp_bit(int bit)
{
	int r = fr->add(bit);
	if (r == SyncFrames::GOOD || r == SyncFrames::BAD_CRC) {
		V90Cp cp;
		int ok;
		ok = r == SyncFrames::GOOD && cp.parse(fr->b, fr->len) == 0;
		if (ok && !cp.usable(cp.drn + (cp.cp ? 20 : 8), 6 - cp.sr)) {
			/* our encoder can't use it: no CP (a retrain
			   follows) */
			DSP_DBG("v34 rx: CP%s with constellations that "
				"can't carry drn %d\n", cp.cp ? "" : "t",
				cp.drn);
		} else if (ok) {
			if (!(cp.cp ? ev_cp : ev_cpt) ||
			    (cp.ack && !ev_cpack) ||
			    (cp.cp && cp.silence != cp_silence))
				DSP_DBG("v34 rx: CP%s%s at symbol %ld: drn %d "
					"Sr %d ld %d, %d constellations, "
					"mask %04x\n", cp.cp ? (cp.silence ?
					"s" : "") : "t",
					cp.ack ? "'" : "", sym, cp.drn,
					cp.sr, cp.ld, cp.ncon, cp.rate_mask);
			memcpy(cpraw, fr->b, fr->len);
			if (cp.cp) {
				ev_cp = 1;
				if (cp.ack)
					ev_cpack = 1;
				cp_count++;
				cp_silence = cp.silence;
			} else
				ev_cpt = 1;
			/* E may follow CP' (after CPt comes SCR: ones, and
			   after CPs' too: the analogue modem trains its echo
			   canceller) */
			after_mp = cp.cp && !cp.silence;
			mp_end = sym;
			ones = 0;
		} else
			DSP_DBG("v34 rx: CP with bad CRC\n");
	}
}

/* a descrambled bit after S-bar: Ja and CP frames (V.90), MP frames,
   E (20 ones after an MP or CP' frame, which starts data mode), J
   (twice, and whether it asks for 16 point TRN) and J' */
void V34Rx::got_bit(int bit)
{
	sr = (sr << 1) | bit;
	if (v90 && ev_ja && stage == V34_RX_TRN) {
		if (fr->busy()) {
			cp_bit(bit);
			return;
		}
		if ((sr & 0x3FFFF) == 0x3FFFE) {
			frame_start();
			after_mp = 0;
			return;
		}
		if (after_mp) {
			/* E: 20 ones after CP' */
			ones = bit ? ones + 1 : 0;
			if (ones == 20) {
				ev_e = 1;
				DSP_DBG("v34 rx: E at symbol %ld\n", sym);
				set_stage(V34_RX_DATA);
			}
		}
		return;
	}
	if (v90 && fr && !ev_ja && stage == V34_RX_TRN) {
		if (fr->busy())
			ja_bit(bit);
		else if ((sr & 0x3FFFF) == 0x3FFFE)
			frame_start();
		return;
	}
	if (mp_n) {
		mpbits[mp_n++] = bit;
		if (mp_n == 19)
			mp_len = bit ? 188 : 88;
		if (mp_n == mp_len) {
			V34Mp mp_;
			mp_n = 0;
			if (mp_.parse(mpbits, mp_len) == 0) {
				if (!ev_mp || mp_.ack != mp.ack)
					DSP_DBG("v34 rx: MP%s type %d rates %d/%d mask %04x "
						"trellis %d expanded %d\n",
						mp_.ack ? "'" : "", mp_.type,
						mp_.rate_ca, mp_.rate_ac, mp_.rate_mask,
						mp_.trellis, mp_.expanded);
				mp = mp_;
				ev_mp = 1;
				after_mp = 1;
				mp_end = sym;
				ones = 0;
			} else
				DSP_DBG("v34 rx: MP with bad CRC\n");
		}
		return;
	}
	if (after_mp) {
		/* E: 20 ones after an MP sequence */
		ones = bit ? ones + 1 : 0;
		if (ones == 20) {
			ev_e = 1;
			DSP_DBG("v34 rx: E at symbol %ld (MP end %ld), "
				"mse %.5f\n", sym, mp_end, mse);
			set_stage(V34_RX_DATA);
			return;
		}
	}
	if ((sr & 0x3FFFF) == 0x3FFFE) {
		/* MP frame sync: 17 ones and a start bit */
		int i;
		for (i = 0; i < 17; i++)
			mpbits[i] = 1;
		mpbits[17] = 0;
		mp_n = 18;
		mp_len = 0;
		after_mp = 0;
		return;
	}
	if ((sr & 0xFFFF) == rev16(J_4POINT) ||
	    (sr & 0xFFFF) == rev16(J_16POINT)) {
		if (++j_count >= 2 && !ev_j) {
			ev_j = 1;
			j16 = (sr & 0xFFFF) == rev16(J_16POINT);
			DSP_DBG("v34 rx: J at symbol %ld%s\n", sym,
				j16 ? " (16 point TRN)" : "");
		}
	} else if ((sr & 0xFFFF) == rev16(J_DASH) && ev_j && !ev_jp) {
		ev_jp = 1;
		ls_n = 0;            /* the caller's phase 4 TRN */
		DSP_DBG("v34 rx: J' at symbol %ld\n", sym);
	}
}

/* the 4 point signals after S-bar: J, MP and E are coded differentially */
void V34Rx::diff_decode(cplx z)
{
	int idx_ = nearest4(z), d = (idx_ - idx) & 3;
	idx = idx_;
	got_bit(descramble(&scr, tap, d & 1));
	if (stage == V34_RX_DATA)
		return;
	got_bit(descramble(&scr, tap, d >> 1));
}

/* from the first 48 PP symbols: the offset (-4..4 symbols) at which
   they match the PP sequence best into pp_off, and the carrier phase */
void V34Rx::pp_align()
{
	cplx best = { 0, 0 };
	float bm = -1;
	int off, k;
	for (off = -4; off <= 4; off++) {
		cplx c = { 0, 0 };
		for (k = 0; k < 48; k++)
			c = cplx_add(c, cplx_mulc(pp_buf[k], v34_pp(k + 48 + off)));
		if (cplx_pow(c) > bm) {
			bm = cplx_pow(c);
			best = c;
			pp_off = off;
		}
	}
	/* rotate the receiver by the phase of the correlation */
	qam.theta += atan2f(best.im, best.re);
	DSP_DBG("v34 rx: PP offset %d\n", pp_off);
}

/*
 *    least squares training: on a band limited line the LMS adaptation
 *    takes far too long to reach the accuracy the dense constellations
 *    need.  Over LS_LEN symbols of TRN (known once decided) the
 *    normal equations are collected, then solved for the equalizer.
 */

#define LS_LEN          1500

/* the equalizer input with the target d (the decided symbol) into the
   normal equations R w = p of the least squares equalizer */
void V34Rx::ls_collect(cplx d)
{
	const int n_ = V34_EQ_LEN;
	const cplx *x = qam.eq_buf;
	/* the target before the carrier rotation */
	cplx t = cplx_mulc(d, qam.rot);
	int i, j;
	for (i = 0; i < n_; i++) {
		double xr = x[i].re, xi = -x[i].im;     /* conj(x_i) */
		lscplx *row = ls_r.data() + i * n_;
		for (j = i; j < n_; j++) {
			row[j].re += xr * x[j].re - xi * x[j].im;
			row[j].im += xr * x[j].im + xi * x[j].re;
		}
		ls_p[i].re += xr * t.re - xi * t.im;
		ls_p[i].im += xr * t.im + xi * t.re;
	}
}

/* R w = p by Cholesky (R Hermitian, upper triangle collected), with a
   little diagonal loading, into the equalizer; returns -1 when R is not
   positive definite.  ls_r is overwritten. */
int V34Rx::ls_solve()
{
	const int n_ = V34_EQ_LEN;
	lscplx *a = ls_r.data(), *p = ls_p.data(), w[V34_EQ_LEN];
	double load = 0;
	int i, j, k;

	for (i = 0; i < n_; i++)
		load += a[i * n_ + i].re;
	load = 1e-4 * load / n_;
	for (i = 0; i < n_; i++) {
		a[i * n_ + i].re += load;
		for (j = 0; j < i; j++) {       /* lower triangle */
			a[i * n_ + j].re = a[j * n_ + i].re;
			a[i * n_ + j].im = -a[j * n_ + i].im;
		}
	}
	/* a = L L^H, L in the lower triangle */
	for (j = 0; j < n_; j++) {
		double s = a[j * n_ + j].re;
		for (k = 0; k < j; k++)
			s -= a[j * n_ + k].re * a[j * n_ + k].re +
			     a[j * n_ + k].im * a[j * n_ + k].im;
		if (s <= 0)
			return -1;
		s = sqrt(s);
		a[j * n_ + j].re = s;
		a[j * n_ + j].im = 0;
		for (i = j + 1; i < n_; i++) {
			double re = a[i * n_ + j].re, im = a[i * n_ + j].im;
			for (k = 0; k < j; k++) {
				/* - L_ik conj(L_jk) */
				re -= a[i * n_ + k].re * a[j * n_ + k].re +
				      a[i * n_ + k].im * a[j * n_ + k].im;
				im -= a[i * n_ + k].im * a[j * n_ + k].re -
				      a[i * n_ + k].re * a[j * n_ + k].im;
			}
			a[i * n_ + j].re = re / s;
			a[i * n_ + j].im = im / s;
		}
	}
	/* L y = p */
	for (i = 0; i < n_; i++) {
		double re = p[i].re, im = p[i].im;
		for (k = 0; k < i; k++) {
			re -= a[i * n_ + k].re * w[k].re - a[i * n_ + k].im * w[k].im;
			im -= a[i * n_ + k].re * w[k].im + a[i * n_ + k].im * w[k].re;
		}
		w[i].re = re / a[i * n_ + i].re;
		w[i].im = im / a[i * n_ + i].re;
	}
	/* L^H w = y */
	for (i = n_ - 1; i >= 0; i--) {
		double re = w[i].re, im = w[i].im;
		for (k = i + 1; k < n_; k++) {
			/* conj(L_ki) w_k */
			re -= a[k * n_ + i].re * w[k].re + a[k * n_ + i].im * w[k].im;
			im -= a[k * n_ + i].re * w[k].im - a[k * n_ + i].im * w[k].re;
		}
		w[i].re = re / a[i * n_ + i].re;
		w[i].im = im / a[i * n_ + i].re;
	}
	for (i = 0; i < n_; i++)
		qam.eq[i] = cplx_make(static_cast<float>(w[i].re),
				      static_cast<float>(w[i].im));
	return 0;
}

/* phase 3: two solutions LS_GAP symbols apart measure the sample clock
   offset (the timing is fixed from the first on); phase 4: one more */
#define LS_GAP          1600

/* a least squares solution over TRN symbols start to start + LS_LEN - 1
   (counted in ls_n): at its end the new equalizer, its delay and the
   sample clock offset from the delay of the solution before */
void V34Rx::ls_window(cplx d, long start)
{
	if (ls_n == start) {
		std::fill(ls_r.begin(), ls_r.end(), lscplx());
		std::fill(ls_p.begin(), ls_p.end(), lscplx());
	}
	if (ls_n >= start && ls_n < start + LS_LEN)
		ls_collect(d);
	if (ls_n == start + LS_LEN) {
		float before = mse;
		long sym_ = qam.symbols - LS_LEN / 2;
		if (ls_solve())
			return;
		qam.recompute();
		have_ls = 1;
		/* the error of the new solution from here on */
		memset(err_r, 0, sizeof(err_r));
		memset(err_h, 0, sizeof(err_h));
		err_n = 0;
		c0 = eq_delay();
		/* the sample clock offset from the delays of successive
		   solutions: the input comes later when the equalizer delay
		   goes down */
		if (ls_sym)
			t_freq -= (c0 - ls_d) / (sym_ - ls_sym);
		ls_d = c0;
		ls_sym = sym_;
		DSP_DBG("v34 rx: least squares equalizer (mse was %.5f), "
			"delay %.3f, clock %.2e\n", before, c0, t_freq);
	}
}

/* per TRN symbol, d its decision: in phase 3 the timing is fixed at
   symbol 800 and two solutions follow, in phase 4 one from symbol
   1200 */
void V34Rx::ls_train(cplx d)
{
	ls_n++;
	if (!phase4_ls) {
		if (ls_n == 800 && !timing_fixed)
			timing_fix();
		if (timing_fixed) {
			ls_window(d, 800);
			ls_window(d, 800 + LS_GAP);
			if (ls_n == 800 + LS_GAP + LS_LEN)
				phase4_ls = 1;
		}
	} else
		ls_window(d, 1200);
}

/* the delay of the equalizer, in samples: the phase slope of its
   response between -baud/4 and +baud/4 */
float V34Rx::eq_delay() const
{
	cplx hp = { 0, 0 }, hm = { 0, 0 }, c;
	float w = 2 * static_cast<float>(M_PI) * 0.25f / 2;  /* per T/2 tap */
	int i;
	for (i = 0; i < qam.eq_len; i++) {
		c = cplx_make(cosf(w * i), sinf(w * i));
		hp = cplx_add(hp, cplx_mulc(qam.eq[i], c));
		hm = cplx_add(hm, cplx_mul(qam.eq[i], c));
	}
	/* phase(H(f)) - phase(H(-f)) = -2 pi 2f delay */
	return -atan2f(cplx_mulc(hp, hm).im, cplx_mulc(hp, hm).re) /
		(2 * static_cast<float>(M_PI) * 0.5f) * qam.sps;
}

/* trained: from now on the timing follows the equalizer, the
   Gardner detector is too noisy for the dense constellations */
void V34Rx::timing_fix()
{
	timing_fixed = 1;
	c0 = eq_delay();
	t_freq = 0;
}

/* decision directed timing: the error against the slope of the
   equalizer output over time (from the T/2 samples), for any
   constellation; d is the decision */
void V34Rx::timing_dd(cplx d)
{
	cplx zd = { 0, 0 }, e;
	float dt, p;
	int i;
	if (!timing_fixed || !have_ls)
		return;
	for (i = 1; i < qam.eq_len - 1; i++) {
		cplx x = cplx_sub(qam.eq_buf[i - 1], qam.eq_buf[i + 1]);
		zd = cplx_add(zd, cplx_mul(qam.eq[i], x));
	}
	/* per sample */
	zd = cplx_scale(cplx_mul(zd, qam.rot), 1 / qam.sps);
	p = cplx_pow(zd);
	if (p < 1e-6f)
		return;
	e = cplx_sub(d, qam.z);
	dt = (e.re * zd.re + e.im * zd.im) / p;
	if (dt > 0.2f)
		dt = 0.2f;
	else if (dt < -0.2f)
		dt = -0.2f;
	t_freq += 1e-5f * dt;
	qam.t_next += 0.01f * dt;
}


/* whether the equalizer input is at the trained level: normalized
   LMS steps grow without bounds on a fading signal */
int V34Rx::level_ok()
{
	float e = 0;
	int i;
	for (i = 0; i < qam.eq_len; i++)
		e += cplx_pow(qam.eq_buf[i]);
	if (qam.power < 0.5f * ref_power)
		return 0;
	if (x_ref == 0)
		x_ref = e;
	if (e < 0.5f * x_ref)
		return 0;
	x_ref += 0.002f * (e - x_ref);
	return 1;
}


/* the autocorrelation of the equalizer's error, for the precoder */
void V34Rx::err_collect(cplx e)
{
	int k;
	err_r[0] += cplx_pow(e);
	for (k = 1; k <= 3; k++) {
		cplx c = cplx_mulc(e, err_h[k - 1]);
		err_r[2 * k] += c.re;
		err_r[2 * k + 1] += c.im;
	}
	err_h[2] = err_h[1];
	err_h[1] = err_h[0];
	err_h[0] = e;
	err_n++;
}

/* a symbol from the demodulator (qam.z): S detection, the training
   stages of phases 3 and 4, and in data mode the decoder, the adaptation
   and the remote's S of a renegotiation */
void V34Rx::symbol()
{
	cplx z = qam.z, d;
	float p, a, b, e;

	sym++;
	n++;
	if (dsplibs_debug_level >= 5 && sym % 500 == 0)
		DSP_DBG("v34 rx%d: %ld st %d mse %.5f d %.3f/%.3f f %.2e "
			"dth %.2e tadj %.2e\n", tap, sym, stage, mse,
			eq_delay(), c0, t_freq, qam.dtheta,
			qam.t_adj);
	p = cplx_pow(z) + cplx_pow(z2) + 1e-3f;
	a = cplx_pow(cplx_sub(z, z2));
	b = cplx_pow(cplx_add(z, z2));
	s_score += 0.25f * (a / p - s_score);
	z2 = z1;
	z1 = z;

	switch (stage) {
	case V34_RX_IDLE:
		if (dsplibs_debug_level >= 7)
			DSP_DBG("v34 rx%d idle %ld z %.2f %.2f s %.3f pw %.0f/%.0f\n",
				tap, sym, z.re, z.im, s_score, qam.power,
				ref_power);
		/* S: the symbols repeat every other symbol (and are there:
		   silence repeats as well) */
		if (s_score < 0.05f && cplx_pow(z) > 0.5f &&
		    cplx_pow(z2) > 0.5f) {
			if (++s_count >= 24) {
				ev_s = 1;
				DSP_DBG("v34 rx: S at symbol %ld\n", sym);
				set_stage(V34_RX_S);
			}
		} else
			s_count = 0;
		break;
	case V34_RX_S:
		/* S-bar: reversed against two symbols ago */
		if (b < 0.15f * p && cplx_pow(z) > 0.5f) {
			set_stage(V34_RX_SBAR);
			sbar_count++;
		}
		break;
	case V34_RX_SBAR:
		if (n == 16) {
			set_stage(phase4 ? V34_RX_TRN : V34_RX_PP);
			idx = nearest4(z);
			if (phase4 && have_good) {
				/* the same channel as in phase 3 (or in data
				   mode, without the precoder's 1 + H(z)) */
				memcpy(qam.eq, good_eq, sizeof(qam.eq));
				qam.eq_len = V34_EQ_LEN;
				mse = 0.05f;
			}
		}
		break;
	case V34_RX_PP:
		if (n <= 48) {
			/* find where PP really starts (S-bar is seen a symbol
			   or two late) and its phase */
			pp_buf[n - 1] = z;
			if (n == 48)
				pp_align();
			break;
		}
		d = cplx_scale(v34_pp(n - 1 + pp_off), 1.41421f);
		qam.adapt(d, 0.02f, 0.05f, 0.0005f);
		if (n == 288 - pp_off) {
			set_stage(V34_RX_TRN);
			phase4 = 1;
			/* the level stays, silence before phase 4 must not
			   upset the gain */
			ref_power = qam.power;
			qam.agc_hold = 1;
		}
		break;
	case V34_RX_TRN:
		/* V.90: S straight after TRN (scrambled TRN repeats
		   every other symbol only by chance) */
		if (s_in_trn && s_score < 0.05f) {
			if (++s_count >= 24) {
				ev_s = 1;
				DSP_DBG("v34 rx: S in TRN at symbol %ld\n", sym);
				set_stage(V34_RX_S);
				break;
			}
		} else
			s_count = 0;
		d = v34_trn4[nearest4(z)];
		e = cplx_pow(cplx_sub(z, d)) / 2;
		mse += 0.01f * (e - mse);
		/* not on the fading end of a signal; smaller steps once
		   trained, the data constellations are dense */
		if (level_ok()) {
			/* finer once the equalizer was solved */
			if (timing_fixed && have_ls) {
				timing_dd(d);
				qam.adapt(d, 0.01f, 0.02f, 0.0002f);
			}
			else
				qam.adapt(d, 0.01f, 0.05f, 0.001f);
			if (n == 1000) {
				qam.kt_p *= 0.2f;
				qam.kt_gi *= 0.2f;
			}
		}
		if (mse < 0.02f && (n & 255) == 0) {
			memcpy(good_eq, qam.eq, sizeof(good_eq));
			have_good = 1;
		}
		ls_train(d);
		if (have_ls && level_ok())
			err_collect(cplx_sub(z, d));
		last_d[2] = last_d[1];
		last_d[1] = last_d[0];
		last_d[0] = d;
		diff_decode(z);
		/* E is over: the next symbol is data */
		if (stage == V34_RX_DATA && data_on && drx.p.precode)
			precode_eq(&drx.p);
		break;
	case V34_RX_DATA:
		/* the remote's S: a rate renegotiation (data never repeats
		   every other symbol for long); 1 + H(z) keeps S periodic */
		if (data_on && s_score < 0.05f && cplx_pow(z) > 0.5f &&
		    cplx_pow(z2) > 0.5f) {
			if (++s_count >= 24) {
				reneg_s();
				break;
			}
		} else
			s_count = 0;
		if (data_on) {
			float g2 = drx.p.g * drx.p.g;
			if (drx.p.precode && n <= 3)
				z = precode_start(z);
			d = drx.symbol(z, &scr, tap);
			if (!level_ok())
				break;
			if (dsplibs_debug_level >= 8)
				DSP_DBG("v34 rx%d: data %ld %.5f %.5f %.5f %.5f\n",
					tap, n, z.re, z.im, d.re, d.im);
			e = cplx_pow(cplx_sub(z, d)) / g2;
			mse += 0.01f * (e - mse);
			sig += 0.01f * (cplx_pow(d) / g2 - sig);
			timing_dd(d);
			qam.adapt(d, 0.004f, 0.02f, 0.0002f);
			/* the equalizer to go back to in a renegotiation (with
			   precoding, the one from the training) */
			if (!drx.p.precode && mse < 0.15f && (n & 255) == 0)
				memcpy(good_eq, qam.eq, sizeof(good_eq));
			if ((n & 63) == 0) {
				mse_old = mse_mid;
				mse_mid = mse;
			}
			if (n % 2000 == 0)
				DSP_DBG("v34 rx%d: data mse %.3f\n", tap, mse);
		}
		break;
	}
}

/* the remote's S in data mode: the data stops, S-bar, maybe TRN, then
   MP, MP' and E follow as in phase 4, on the 4 point signals */
void V34Rx::reneg_s()
{
	/* the mse of the data before S */
	if (mse_old > 0)
		mse = mse_old;
	DSP_DBG("v34 rx: S in data mode at symbol %ld, mse %.3f\n", sym, mse);
	data_on = 0;
	mse_mid = mse_old = 0;
	ev_reneg = ev_s = 1;
	ev_mp = ev_e = 0;
	/* V.90: CP, CP' and E of the renegotiation to come */
	ev_cp = ev_cpack = 0;
	cp_silence = 0;
	if (fr)
		fr->drop();
	mp_n = 0;
	after_mp = 0;
	ones = 0;
	memset(&mp, 0, sizeof(mp));
	s_count = 0;
	set_stage(V34_RX_S);
}

/* with precoding the equalizer gets the response 1 + H(z): its output is
   the channel output y(n) of the remote's precoder plus whitened noise */
void V34Rx::precode_eq(const V34Params *p)
{
	cplx eq[QAM_MAX_EQ];
	int i, k, len;

	/* never on top of an earlier 1 + H(z): from the equalizer
	   without it */
	if (qam.eq_len > V34_EQ_LEN && have_good) {
		memcpy(qam.eq, good_eq, sizeof(qam.eq));
		qam.eq_len = V34_EQ_LEN;
	}
	/* the one a renegotiation goes back to (S-bar), when the training
	   kept none: this one, as trained */
	if (!have_good) {
		memcpy(good_eq, qam.eq, sizeof(good_eq));
		for (i = qam.eq_len; i < QAM_MAX_EQ; i++)
			good_eq[i] = cplx_make(0, 0);
		have_good = 1;
	}
	len = qam.eq_len + 6;

	if (len > QAM_MAX_EQ)
		len = QAM_MAX_EQ;
	for (i = 0; i < len; i++) {
		eq[i] = i < qam.eq_len ? qam.eq[i] : cplx_make(0, 0);
		for (k = 1; k <= 3; k++) {
			cplx h = cplx_make(p->h[2 * k - 2] / 16384.0f,
					   p->h[2 * k - 1] / 16384.0f);
			if (i - 2 * k >= 0 && i - 2 * k < qam.eq_len)
				eq[i] = cplx_add(eq[i],
						 cplx_mul(h, qam.eq[i - 2 * k]));
		}
	}
	DSP_DBG("v34 rx: the precoder's 1 + H(z) into the equalizer: %d "
		"taps -> %d\n", qam.eq_len, len);
	memcpy(qam.eq, eq, len * sizeof(cplx));
	qam.eq_len = len;
}

/* the precoder starts from zeros with B1: not the symbols before */
cplx V34Rx::precode_start(cplx z)
{
	const V34Params *p = &drx.p;
	int k, n_ = static_cast<int>(n) - 1;

	for (k = n_ + 1; k <= 3; k++) {
		cplx h = cplx_make(p->h[2 * k - 2] / 16384.0f,
				   p->h[2 * k - 1] / 16384.0f);
		z = cplx_sub(z, cplx_mul(h, last_d[k - n_ - 1]));
	}
	return z;
}

/* the precoding coefficients to ask for (9.6.2/V.34): h(k) = -a(k), with
   a(k) the third order linear predictor of the equalizer's error, so
   that the error through 1 + H(z) is white.  Returns the gain in dB of
   the decisions' SNR (0 when there is too little data to tell). */
float V34Rx::precoder(int h[6]) const
{
	typedef struct { double re, im; } dc;
	dc R[4], A[3][4], a[3];
	double eps, s;
	int i, j, k;

	memset(h, 0, 6 * sizeof(int));
	if (err_n < 500 || err_r[0] <= 0)
		return 0;
	for (k = 0; k < 4; k++) {
		R[k].re = err_r[2 * k] / err_n;
		R[k].im = k ? err_r[2 * k + 1] / err_n : 0;
	}
	/* sum over k of a(k) R(j - k) = R(j), j = 1..3; R(-m) = R(m)* */
	for (j = 0; j < 3; j++) {
		for (k = 0; k < 3; k++) {
			int m = j - k;
			A[j][k].re = R[m < 0 ? -m : m].re;
			A[j][k].im = m < 0 ? -R[-m].im : R[m].im;
		}
		A[j][3] = R[j + 1];
	}
	for (i = 0; i < 3; i++) {
		int piv = i;
		for (j = i + 1; j < 3; j++)
			if (A[j][i].re * A[j][i].re + A[j][i].im * A[j][i].im >
			    A[piv][i].re * A[piv][i].re + A[piv][i].im * A[piv][i].im)
				piv = j;
		for (k = 0; k < 4; k++) {
			dc t = A[i][k];
			A[i][k] = A[piv][k];
			A[piv][k] = t;
		}
		s = A[i][i].re * A[i][i].re + A[i][i].im * A[i][i].im;
		if (s <= 0)
			return 0;
		for (j = 0; j < 3; j++) {
			dc f;
			if (j == i)
				continue;
			/* f = A[j][i] / A[i][i] */
			f.re = (A[j][i].re * A[i][i].re + A[j][i].im * A[i][i].im) / s;
			f.im = (A[j][i].im * A[i][i].re - A[j][i].re * A[i][i].im) / s;
			for (k = i; k < 4; k++) {
				A[j][k].re -= f.re * A[i][k].re - f.im * A[i][k].im;
				A[j][k].im -= f.re * A[i][k].im + f.im * A[i][k].re;
			}
		}
	}
	for (i = 0; i < 3; i++) {
		s = A[i][i].re * A[i][i].re + A[i][i].im * A[i][i].im;
		a[i].re = (A[i][3].re * A[i][i].re + A[i][3].im * A[i][i].im) / s;
		a[i].im = (A[i][3].im * A[i][i].re - A[i][3].re * A[i][i].im) / s;
	}
	/* |y(n)| <= 255 (9.6.2/V.34) with points up to 45 and |x(n)| up to
	   47 on each axis: at most 4 for the sum of |h| over the components */
	s = 0;
	for (k = 0; k < 3; k++)
		s += fabs(a[k].re) + fabs(a[k].im);
	if (s > 4)
		for (k = 0; k < 3; k++) {
			a[k].re *= 4 / s;
			a[k].im *= 4 / s;
		}
	for (k = 0; k < 3; k++) {
		double hr = -a[k].re, hi = -a[k].im;
		hr = hr > 1.99 ? 1.99 : hr < -1.99 ? -1.99 : hr;
		hi = hi > 1.99 ? 1.99 : hi < -1.99 ? -1.99 : hi;
		h[2 * k] = static_cast<int>(lrint(hr * 16384));
		h[2 * k + 1] = static_cast<int>(lrint(hi * 16384));
		a[k].re = -h[2 * k] / 16384.0;
		a[k].im = -h[2 * k + 1] / 16384.0;
	}
	/* the error power through 1 + H(z) with the coefficients sent:
	   R(0) - 2 Re sum a(k)* R(k) + sum a(j) a(k)* R(k - j) */
	eps = R[0].re;
	for (k = 0; k < 3; k++)
		eps -= 2 * (a[k].re * R[k + 1].re + a[k].im * R[k + 1].im);
	for (j = 0; j < 3; j++)
		for (k = 0; k < 3; k++) {
			int m = k - j;
			dc rr = R[m < 0 ? -m : m], p;
			if (m < 0)
				rr.im = -rr.im;
			/* a(j) conj(a(k)) */
			p.re = a[j].re * a[k].re + a[j].im * a[k].im;
			p.im = a[j].im * a[k].re - a[j].re * a[k].im;
			eps += p.re * rr.re - p.im * rr.im;
		}
	if (eps <= 0)
		return 0;
	return static_cast<float>(10 * log10(R[0].re / eps));
}

/* the data mode parameters, before the end of E (precode_eq() applies
   at its end): link takes the bits of each mapping frame */
void V34Rx::start_data(const V34Params *p, CoreBits link)
{
	drx.init(p);
	drx.link = link;
	mse = 0;
	data_on = 1;
}

/* a line sample at 9600 Hz: signal detection (its loss in training
   goes back to waiting for S), and a symbol when one is complete */
void V34Rx::sample(float x)
{
	int on_;
	/* the timing must not wander on the noise between the signals */
	qam.timing_freeze = !on || timing_fixed;
	if (!qam.sample(x))
		return;
	/* the sample clock offset, also through the silences */
	if (timing_fixed)
		qam.t_next += t_freq;
	on_ = qam.power > 300.0f * 300.0f / 2 &&
		(!ref_power || qam.power > 0.25f * ref_power);
	if (on && !on_ && stage != V34_RX_IDLE && stage != V34_RX_DATA) {
		DSP_DBG("v34 rx%d: signal lost at symbol %ld\n", tap, sym);
		set_stage(V34_RX_IDLE);
		s_count = 0;
	}
	on = on_;
	if (on_)
		symbol();
}

} // namespace dsp
