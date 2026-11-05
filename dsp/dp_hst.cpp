/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_hst.cpp - USRobotics HST: a 4800 to 16800 bit/s forward channel
 *                 one way, a 300 bit/s back channel the other
 *
 *    HST ("High Speed Technology", 1986) was never published.  What this
 *    pump sends is what the DSP of a USRobotics Courier V.Everything
 *    sends, as worked out from its firmware by the courier-emu project
 *    (docs/hst.md there; docs/dsp/hst-details.md here):
 *
 *    forward channel: 2400 baud on an 1800 Hz carrier, V.32bis's
 *              constellations and V.32's 8 state trellis code at 7200 to
 *              14400 bit/s, differential QPSK at 4800, and at 16800 a
 *              256 point constellation of its own (the Courier's DSP
 *              mask ROM); the scrambler is 1 + x^-18 + x^-23, n bits a
 *              symbol into a 32-bit register at bit 23
 *    back channel: differential 8-PSK at 200 baud on a 350 Hz carrier,
 *              a data symbol (3 bits) and a silent one in turn: 300
 *              bit/s; scrambled with 1 + x^-14 + x^-17
 *
 *    The forward channel's training is the Courier's: 72 symbols
 *    alternating between two of the 4800 points, then scrambled 4800
 *    symbols whose input is the previous output (a fixed sequence from
 *    the seed 0x001f7310), then 16 symbols at the data rate (24 at
 *    12000 and up), whose input is again the previous output, then the
 *    data.  The back channel's is 17 symbols of the same kind.  Both are
 *    known sequences: the receivers train on them.
 *
 *    How two Couriers choose who sends the forward channel, and at
 *    which rate, is in their supervisor's firmware and not yet read.
 *    D-Modem's start-up, its own until then (docs/dsp/hst.md):
 *
 *    answerer: the answer tone, then the forward channel's training,
 *              its scrambled part until the caller's rate request has
 *              come on the back channel (200 symbols at least, 3096 for
 *              12000 and up), then the request's rate (or its own +MS
 *              limit, if lower) and the data
 *    caller:   trains on the forward channel, starts the back channel,
 *              and after 400 symbols of the scrambled training asks for
 *              the rate its SNR allows (+MS's limit at most): the flag
 *              0x7e, the rate's index and its complement, again and
 *              again; it finds the rate the answerer took from the
 *              symbols that follow (each candidate rate's training is
 *              a known sequence too), and the data after them
 *
 *    The forward channel goes from the answerer to the caller for the
 *    whole call: a BBS sends more than its callers do.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "dm_types.h"
#include "pump.h"
#include "qam.h"

namespace dsp {

/* the forward channel */
#define HST_BAUD        2400
#define HST_CARRIER     1800
#define HST_ALPHA       0.15f   /* narrow: the back channel is below it */
#define HST_SPAN        8
#define HST_EQ_LEN      32
#define HST_POWER       40.0f   /* about the constellations' mean power */
#define HST_TX_AMPLITUDE (2000.0f * 1.41421f)

/* the back channel */
#define HST_BC_BAUD     200
#define HST_BC_CARRIER  350
#define HST_BC_ALPHA    0.2f
#define HST_BC_SPAN     6
#define HST_BC_EQ_LEN   4
#define HST_BC_POWER    0.5f    /* unit points, every other one silent */
#define HST_BC_RATE     300

/* the forward channel's training (symbols): the alternating part, the
   scrambled part at least (and for 12000 and up), the part at the data
   rate (and for 12000 and up); the scrambled symbols the caller trains
   on before it asks for a rate */
#define HST_ALT_SYMS    72
#define HST_TRN_SYMS    200
#define HST_TRN_SYMS_HI 3096
#define HST_RATE_SYMS   16
#define HST_RATE_SYMS_HI 24
#define HST_REQ_AFTER   1200

/* the back channel's training (symbols, the silent ones counted) */
#define HST_BC_TRN_SYMS 17

/* the scramblers' seeds (the Courier's) */
#define HST_SEED        0x001f7310u
#define HST_BC_SEED     0x0001fd28u

/* the rates, by the index the rate request carries */
struct hst_rate {
	s32 bps;
	s16 nbits;
	const cplx *points;
	float snr;              /* the SNR (dB) the caller asks for it at */
};

static const struct hst_rate hst_rates[] = {
	{ 4800, 2, v32_points_4800, 0 },
	{ 7200, 3, v32_points_7200, 15.5f },
	{ 9600, 4, v32_points_9600, 18.5f },
	{ 12000, 5, v32_points_12000, 21.5f },
	{ 14400, 6, v32_points_14400, 24.5f },
	{ 16800, 7, hst_points_16800, 28.0f },
};
#define HST_NRATES (s16)std::size(hst_rates)

/* 4800 bit/s's differential code, as V.32's: the next point from the
   last and the dibit (the first bit in bit 0) */
static const unsigned char hst_4800_enc[4][4] = {
	{ 2, 3, 0, 1 }, { 0, 2, 1, 3 }, { 3, 1, 2, 0 }, { 1, 0, 3, 2 },
};

/* the back channel's phase step (45 degrees) for each 3 bits, and the
   3 bits of each step */
static const s16 hst_bc_step[8] = { 1, 6, 2, 5, 0, 7, 3, 4 };
static const s16 hst_bc_bits[8] = { 4, 0, 2, 6, 7, 3, 1, 5 };

/* the scramblers: n bits a symbol into a 32-bit register at bit `at`,
   the register shifted right by n; the taps at bits 0 and `tap` */
struct HstScr {
	u32 r;
	s16 at, tap;

	/* starts the register at `seed`, inserting at bit `at_`, the
	   second tap at `tap_` */
	void init(u32 seed, s16 at_, s16 tap_)
	{
		r = seed;
		at = at_;
		tap = tap_;
	}
	/* n bits in: n scrambled bits out */
	s16 scramble(s16 in, s16 n)
	{
		s16 s = static_cast<s16>(((r >> tap) ^ r ^
					 static_cast<u32>(in)) & ((1u << n) - 1));
		r = (r | static_cast<u32>(s) << at) >> n;
		return s;
	}
	/* n scrambled bits in: n bits out */
	s16 descramble(s16 s, s16 n)
	{
		s16 d = static_cast<s16>(((r >> tap) ^ r ^
					 static_cast<u32>(s)) & ((1u << n) - 1));
		r = (r | static_cast<u32>(s) << at) >> n;
		return d;
	}
};

/* the forward channel's transmitter, as the Courier's: the symbols of
   its training and data.  The receiver runs one too, to know what the
   training sends. */
struct HstFwd {
	enum Phase { ALT, TRN, RATE, DATA };
	Phase ph;
	s32 syms;               /* in the phase */
	HstScr scr;
	s16 in;                 /* the last symbol's index and its offset,
				   the next input of the training */
	s16 last_s;             /* the last scrambled training symbol */
	s16 idx;                /* the last point's index */
	s16 rate;               /* hst_rates index */
	TrellisEnc tenc;

	/* back to the start of the training */
	void init()
	{
		ph = ALT;
		syms = 0;
		in = 0;
		idx = 0;
		last_s = 0;
		rate = 0;
		tenc = TrellisEnc{};
	}
	/* to the scrambled part */
	void trn()
	{
		ph = TRN;
		syms = 0;
		scr.init(HST_SEED, 23, 5);
	}
	/* to the part at rate r */
	void at_rate(s16 r)
	{
		ph = RATE;
		syms = 0;
		rate = r;
		/* the differential state is the last training symbol, the
		   convolutional one 0 */
		tenc.diff = last_s & 3;
		tenc.conv = 0;
	}
	/* the next point's index; bits: the data's (DATA only) */
	s16 next(s16 bits)
	{
		const struct hst_rate *r = &hst_rates[rate];
		s16 n = r->nbits, s;

		switch (ph) {
		case ALT:
			/* points 0 and 2 in turn, 0 first */
			idx = syms & 1 ? 2 : 0;
			in = idx + 8;
			break;
		case TRN:
			s = scr.scramble(in & 3, 2);
			last_s = s;
			idx = s;
			in = s + 8;
			break;
		case RATE:
		case DATA:
			s = scr.scramble(ph == DATA ? bits : in & ((1 << n) - 1),
					 n);
			if (n == 2) {
				/* the first symbol's last point is 0, the last
				   training symbol's first bit in the dibit's
				   second */
				if (ph == RATE && syms == 0)
					idx = hst_4800_enc[0]
						[(s | (last_s & 1) << 1) & 3];
				else
					idx = hst_4800_enc[idx][s];
				in = idx + 8;
			} else {
				idx = tenc.encode(s, n);
				in = idx + (2 << n);
			}
			break;
		}
		syms++;
		return idx;
	}
	/* the point of index i at the current rate (the training's on the
	   4800 points) */
	cplx point(s16 i) const
	{
		return ph == ALT || ph == TRN ? v32_points_4800[i] :
			hst_rates[rate].points[i];
	}
};

enum hst_state {
	/* answerer */
	H_SILENCE,
	H_ANS,
	H_GAP,
	H_TRAIN,                /* the forward channel's training */
	/* caller */
	H_WAIT,                 /* the forward channel's training */
	/* both */
	H_DATA,
};

/* the caller's forward channel receiver */
enum hst_rxp {
	RX_IDLE,                /* no carrier, or not yet the training */
	RX_ALT,                 /* the alternating symbols */
	RX_TRN,                 /* the scrambled training: known */
	RX_CHANGE,              /* the training changed: which rate? */
	RX_RATE,                /* the training at that rate: known */
	RX_DATA,
};

/* received symbols kept to find the rate in */
#define HST_HIST 48

struct HstPump : PumpBase<HstPump, hst_state> {
	static constexpr const char *name = "HST data pump";

	/* frees the two receivers */
	~HstPump()
	{
		rx.release();
		bc_rx.release();
	}

	s16 max_rate;           /* hst_rates index: +MS's limit */
	s16 rate;               /* the forward channel's, hst_rates index */
	Osc tone;

	/* the forward channel: the answerer's transmitter */
	QamTx tx;
	HstFwd fwd;
	s16 req;                /* the caller's request: index, -1 none */

	/* the caller's receiver */
	QamRx rx;
	CarrierDetect carrier;
	hst_rxp rxp;
	HstFwd model;           /* what the answerer sends */
	cplx last_z;
	s16 last_sgn, alt_run;
	s16 dec[8];             /* the last 4800 decisions, newest first */
	s32 rx_syms;            /* symbols received */
	float rx_mse;
	s16 asked;              /* the request sent: its index, -1 not yet */
	/* the symbols kept for the rate's search, and the model before
	   each */
	struct HistEntry {
		cplx z;
		HstFwd before;
	} hist[HST_HIST];
	s16 nhist;
	s32 change_at;          /* rx_syms of the first symbol off the
				   training, -1 none */
	s16 misses;             /* of the last 3 symbols */
	s32 data_at;            /* rx_syms when the data begins */
	Viterbi vit;
	s32 vit_fed;            /* symbols fed to it */
	HstScr rx_scr;          /* the descrambler for the data */
	s16 rx_idx;             /* 4800: the last point */
	float data_power;
	s16 agc_free;

	/* the back channel: the caller's transmitter */
	QamTx bc_tx;
	HstScr bc_scr;
	s32 bc_syms;
	s16 bc_ph;              /* the last phase (45 degree steps) */
	s16 bc_in;              /* the last scrambled bits: the training's
				   next input */
	u16 req_word;
	s16 req_bit;
	/* its receiver (the answerer's) */
	QamRx bc_rx;
	Biquad bc_bp[2];
	float bc_pwr;
	CarrierDetect bc_carrier;
	HstScr bc_dscr;
	s16 bc_last_k;
	s16 bc_have_k;
	u32 bc_word;            /* the last 32 bits received */
	s16 bc_on;

	/* its name in the log */
	const char *tag() const { return "hst"; }
	/* in data mode (the carrier watched, no handshake timeout) */
	bool in_data() const { return state == H_DATA; }

	cplx fwd_symbol();
	cplx bc_symbol();
	s16 bc_bits();
	void rx_symbol();
	void rx_change();
	void rx_data(cplx z);
	void bc_rx_symbol();
	void bc_rx_bit(s16 b);
	s16 handshake();
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 sample(float x, float *y);
	/* cnt samples in and out, through sample() */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}
};

/*
 *    the forward channel: the answerer
 */

/* the forward channel's next symbol: the training, then the data */
cplx HstPump::fwd_symbol()
{
	s16 bits = 0;

	switch (fwd.ph) {
	case HstFwd::ALT:
		if (fwd.syms >= HST_ALT_SYMS)
			fwd.trn();
		break;
	case HstFwd::TRN:
		if (req >= 0 && fwd.syms >= (req >= 3 && max_rate >= 3 ?
					      HST_TRN_SYMS_HI : HST_TRN_SYMS)) {
			rate = req < max_rate ? req : max_rate;
			fwd.at_rate(rate);
			DSP_DBG("hst: the caller asks for %" PRId32 ", %" PRId32
				" bit/s\n", hst_rates[req].bps,
				hst_rates[rate].bps);
		}
		break;
	case HstFwd::RATE:
		if (fwd.syms >= (rate >= 3 ? HST_RATE_SYMS_HI : HST_RATE_SYMS)) {
			fwd.ph = HstFwd::DATA;
			fwd.syms = 0;
		}
		break;
	case HstFwd::DATA:
		break;
	}
	if (fwd.ph == HstFwd::DATA) {
		u8 b[8];
		s16 n = hst_rates[rate].nbits;
		modem_get_bits(modem, 1, b, n);
		for (s16 k = 0; k < n; k++)
			bits |= (b[k] & 1) << k;
	}
	return fwd.point(fwd.next(bits));
}

/*
 *    the back channel: the caller
 */

/* the back channel's next 3 bits: ones, the rate request while it is
   asked, the link's in data */
s16 HstPump::bc_bits()
{
	s16 v = 0;

	if (state == H_DATA) {
		u8 b[3];
		modem_get_bits(modem, 1, b, 3);
		return (b[0] & 1) | (b[1] & 1) << 1 | (b[2] & 1) << 2;
	}
	if (asked < 0 || rxp >= RX_RATE)
		return 7;
	for (s16 k = 0; k < 3; k++) {
		v |= ((req_word >> req_bit) & 1) << k;
		req_bit = (req_bit + 1) & 15;
	}
	return v;
}

/* the back channel's next symbol: a data symbol and a silent one in
   turn (two data symbols in a row as the training ends, as the
   Courier's); the training's input is its previous output */
cplx HstPump::bc_symbol()
{
	s32 k = bc_syms++;
	bool data = k < HST_BC_TRN_SYMS ? !(k & 1) :
		    k == HST_BC_TRN_SYMS || !((k - HST_BC_TRN_SYMS) & 1);

	if (!data)
		return cplx_make(0, 0);
	s16 in = k < HST_BC_TRN_SYMS ? bc_in : bc_bits();
	bc_in = bc_scr.scramble(in, 3);
	bc_ph = (bc_ph + hst_bc_step[bc_in]) & 7;
	float a = static_cast<float>(M_PI) * (bc_ph + 0.5f) / 4;
	return cplx_make(cosf(a), sinf(a));
}

/*
 *    the forward channel's receiver: the caller
 */

/* the data's points at rate r: the slicer's count */
static s16 hst_npoints(s16 r)
{
	s16 n = hst_rates[r].nbits;
	return n == 2 ? 4 : 1 << (n + 1);
}

/* one symbol of data (or of the training at the rate, which it decodes
   the same way to fill the Viterbi decoder's paths) */
void HstPump::rx_data(cplx z)
{
	const struct hst_rate *r = &hst_rates[rate];
	s16 n = r->nbits, k, q;
	cplx d;

	/* the gain held while the level stays within 3 dB of data's */
	if (state == H_DATA && data_power > 0) {
		if (rx.power > 2 * data_power ||
		    rx.power < 0.5f * data_power) {
			rx.agc_hold = 0;
			agc_free = HST_BAUD / 10;
		} else if (agc_free > 0 && --agc_free == 0)
			rx.agc_hold = 1;
	}
	if (rxp == RX_RATE) {
		/* the training at the rate: known */
		d = model.point(model.next(0));
		k = model.idx;
	} else {
		k = nearest_point(r->points, hst_npoints(rate), z);
		d = r->points[k];
	}
	rx_mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / HST_POWER - rx_mse);
	if (rx_syms % 500 == 0)
		DSP_TRACE("hst: data %" PRId32 " mse %.5f\n", rx_syms, rx_mse);
	rx.adapt(d, rxp == RX_RATE ? 0.01f : 0.004f, 0.02f, 0.0002f);

	s32 sym;                /* the symbol q is of */
	if (n == 2) {
		for (q = 0; q < 4; q++)
			if (hst_4800_enc[rx_idx][q] == k)
				break;
		rx_idx = k;
		sym = rx_syms;
	} else {
		q = vit.decode(z, nullptr);
		sym = vit_fed++ - (VITERBI_DEPTH - 1);
	}
	if (q < 0 || sym < data_at)
		return;
	q = rx_scr.descramble(q & ((1 << n) - 1), n);
	if (state == H_DATA) {
		u8 bits[8];
		for (k = 0; k < n; k++)
			bits[k] = (q >> k) & 1;
		modem_put_bits(modem, 1, bits, n);
	}
}

/* the training changed (symbols since change_at in hist): the rate,
   and where its part began, that explain them best; the receiver on
   from there */
void HstPump::rx_change()
{
	s16 best_r = -1, best_back = 0, back, r;
	float best_e;
	s16 have = static_cast<s16>(rx_syms - change_at);

	/* no change at all: noise (the model went on with the training) */
	{
		HstFwd m = hist[have - 1].before;
		best_e = 0;
		for (s16 i = have - 1; i >= 0; i--) {
			cplx p = m.point(m.next(0));
			best_e += cplx_pow(cplx_sub(hist[i].z, p));
		}
		best_e /= have;
	}

	/* the part at the rate began at the first symbol off the training
	   or up to 3 before it (it can begin with what the training would
	   have sent) */
	for (back = 0; back <= 3; back++) {
		s16 first = have + back;        /* hist index: 0 newest */
		if (first > nhist)
			break;
		for (r = 0; r <= asked && r < HST_NRATES; r++) {
			HstFwd m = hist[first - 1].before;
			float e = 0;
			m.at_rate(r);
			for (s16 i = first - 1; i >= 0; i--) {
				cplx p = m.point(m.next(0));
				e += cplx_pow(cplx_sub(hist[i].z, p));
			}
			e /= first;
			if (e < best_e) {
				best_e = e;
				best_r = r;
				best_back = back;
			}
		}
	}
	if (best_r < 0) {
		DSP_DBG("hst: noise, not a change of the training\n");
		misses = 0;
		rxp = RX_TRN;
		return;
	}
	rate = best_r;
	s16 first = have + best_back;
	model = hist[first - 1].before;
	model.at_rate(rate);
	rx_idx = 0;
	vit.init(hst_rates[rate].points, hst_rates[rate].nbits);
	vit.last_y12 = model.tenc.diff;
	vit_fed = static_cast<s32>(rx_syms - first);
	/* replay what came since through the trained model, the Viterbi
	   decoder too */
	rxp = RX_RATE;
	for (s16 i = first - 1; i >= 0; i--) {
		model.next(0);
		if (hst_rates[rate].nbits > 2) {
			vit.decode(hist[i].z, nullptr);
			vit_fed++;
		} else
			rx_idx = model.idx;
	}
	data_at = rx_syms - first +
		(rate >= 3 ? HST_RATE_SYMS_HI : HST_RATE_SYMS);
	DSP_DBG("hst: the answerer's rate %" PRId32 " bit/s (%d symbols "
		"in, error %.3f)\n", hst_rates[rate].bps, first,
		best_e / HST_POWER);
}

/* one received symbol of the forward channel (rx.z) */
void HstPump::rx_symbol()
{
	cplx z = rx.z, st;
	s16 k, sgn;
	float a;

	if (!carrier.on && rxp != RX_DATA) {
		last_z = z;
		return;
	}
	k = nearest_point(v32_points_4800, 4, z);
	for (s16 i = 7; i > 0; i--)
		dec[i] = dec[i - 1];
	dec[0] = k;

	switch (rxp) {
	case RX_IDLE:
	case RX_ALT:
		/* steps of +-90 degrees, the sign alternating */
		st = cplx_mulc(z, last_z);
		a = atan2f(st.im, st.re);
		sgn = a > 0 ? 1 : -1;
		/* (a miss now and then: noise) */
		if (fabsf(a) > 0.9f && fabsf(a) < 2.2f && sgn != last_sgn)
			alt_run++;
		else
			alt_run = alt_run > 4 ? alt_run - 4 : 0;
		/* the answer tone before it pulled the timing loop
		   anywhere: start the receiver afresh on the training */
		if (rxp == RX_IDLE && alt_run == 4)
			rx.t_adj = 0;
		last_sgn = sgn;
		rx.adapt(v32_points_4800[k], 0, 0.05f, 0);
		if (rxp == RX_IDLE && alt_run >= 16 && sgn > 0) {
			/* a step of +90 degrees ends on point 2 */
			rx.align_phase(v32_points_4800[2]);
			rxp = RX_ALT;
			DSP_DBG("hst: the forward channel's training\n");
		}
		if (rxp == RX_ALT) {
			/* the scrambled part: its first 8 symbols */
			HstFwd m;
			m.init();
			m.syms = HST_ALT_SYMS;
			m.idx = 2;
			m.in = 10;
			m.trn();
			bool match = true;
			for (s16 i = 7; i >= 0 && match; i--)
				match = m.next(0) == dec[i];
			if (match) {
				model = m;
				rxp = RX_TRN;
				rx_syms = 0;
				nhist = 0;
				DSP_DBG("hst: the scrambled training\n");
			}
		}
		break;
	case RX_TRN:
	case RX_CHANGE: {
		/* keep the symbol and the model before it */
		std::copy_backward(hist, hist + HST_HIST - 1, hist + HST_HIST);
		hist[0].z = z;
		hist[0].before = model;
		if (nhist < HST_HIST)
			nhist++;
		s16 e = model.next(0);
		cplx d = v32_points_4800[e];
		bool miss = e != k;
		misses = static_cast<s16>((misses << 1 | (miss ? 1 : 0)) & 7);
		if (rxp == RX_TRN) {
			rx_mse += 0.02f * (cplx_pow(cplx_sub(z, d)) / HST_POWER -
					   rx_mse);
			/* the equalizer's step smaller as it learns: its
			   noise is what the SNR for the request sees */
			rx.adapt(d, rx_syms < 100 ? 0.1f : rx_syms < 400 ?
				 0.05f : 0.02f, 0.05f, 0.0005f);
			if (rx_syms == 400) {
				/* a slower timing loop: its jitter is what the
				   SNR for the request would see */
				rx.kt_p = 0.01f * rx.sps;
				rx.kt_gi = 0.01f;
			}
			if (rx_syms % 100 == 0)
				DSP_TRACE("hst: training %" PRId32 " mse %.4f\n",
					  rx_syms, rx_mse);
			if (asked < 0 && rx_syms >= HST_REQ_AFTER) {
				float snr = -10 * log10f(rx_mse + 1e-6f);
				s16 r;
				for (r = HST_NRATES - 1; r > 0; r--)
					if (snr >= hst_rates[r].snr)
						break;
				if (r > max_rate)
					r = max_rate;
				asked = r;
				req_word = static_cast<u16>(
					0x7e | r << 8 | ((~r) & 0xf) << 12);
				req_bit = 0;
				DSP_DBG("hst: SNR %.1f dB, asking for %" PRId32
					" bit/s\n", snr, hst_rates[r].bps);
			}
			/* two of three symbols off the training: it changed */
			if (asked >= 0 && ((misses & 1) + (misses >> 1 & 1) +
					   (misses >> 2 & 1)) >= 2) {
				change_at = rx_syms - ((misses & 4) ? 2 : 1);
				rxp = RX_CHANGE;
			}
		} else if (rx_syms + 1 - change_at >= 6) {
			rx_syms++;
			rx_change();
			last_z = z;
			return;
		}
		break;
	}
	case RX_RATE:
	case RX_DATA:
		rx_data(z);
		if (rxp == RX_RATE && rx_syms + 1 >= data_at) {
			rxp = RX_DATA;
			rx_scr = model.scr;
			/* trained: a slow timing loop and the gain held
			   (their jitter hurts the dense constellations; the
			   equalizer follows slow changes) */
			rx.kt_p = 0.004f * rx.sps;
			rx.kt_gi = 0.003f;
			rx.agc_hold = 1;
		}
		break;
	}
	rx_syms++;
	last_z = z;
}

/*
 *    the back channel's receiver: the answerer
 */

/* a bit of the back channel: the rate request found in them before
   the data, the link's after */
void HstPump::bc_rx_bit(s16 b)
{
	if (state == H_DATA) {
		u8 x = static_cast<u8>(b);
		modem_put_bits(modem, 1, &x, 1);
		return;
	}
	bc_word = bc_word >> 1 | static_cast<u32>(b & 1) << 31;
	u16 lo = static_cast<u16>(bc_word), hi = static_cast<u16>(bc_word >> 16);
	if (req < 0 && lo == hi && (lo & 0xff) == 0x7e &&
	    ((lo >> 8) & 0xf) == ((~lo >> 12) & 0xf) &&
	    ((lo >> 8) & 0xf) < HST_NRATES)
		req = (lo >> 8) & 0xf;
}

/* one received symbol of the back channel (bc_rx.z): silent, or a
   data symbol's 3 bits */
void HstPump::bc_rx_symbol()
{
	cplx z = bc_rx.z;

	if (!bc_on) {
		bc_have_k = 0;
		return;
	}
	if (cplx_pow(z) < 0.25f) {
		bc_rx.adapt(cplx_make(0, 0), 0.01f, 0, 0);
		return;
	}
	float a = atan2f(z.im, z.re) / (static_cast<float>(M_PI) / 4) - 0.5f;
	s16 k = static_cast<s16>(lrintf(a)) & 7;
	float ad = static_cast<float>(M_PI) * (k + 0.5f) / 4;
	bc_rx.adapt(cplx_make(cosf(ad), sinf(ad)), 0.01f, 0.1f, 0.001f);
	if (bc_have_k) {
		s16 v = hst_bc_bits[(k - bc_last_k) & 7];
		s16 d = bc_dscr.descramble(v, 3);
		for (s16 i = 0; i < 3; i++)
			bc_rx_bit((d >> i) & 1);
	}
	bc_last_k = k;
	bc_have_k = 1;
}

/*
 *    both
 */

/* the start-up, a sample at a time */
s16 HstPump::handshake()
{
	switch (state) {
	case H_SILENCE:
		if (count >= ms2samples(ANS_SILENCE_MS, srate))
			set_state(H_ANS);
		break;
	case H_ANS:
		if (count >= ms2samples(ANS_TONE_MS, srate))
			set_state(H_GAP);
		break;
	case H_GAP:
		if (count >= ms2samples(ANS_GAP_MS, srate)) {
			fwd.init();
			set_state(H_TRAIN);
		}
		break;
	case H_TRAIN:
		if (fwd.ph == HstFwd::DATA) {
			set_state(H_DATA);
			stat_line(HST_BAUD, HST_CARRIER, HST_BC_BAUD,
				  HST_BC_CARRIER);
			return connect_at(hst_rates[rate].bps, HST_BC_RATE);
		}
		break;
	case H_WAIT:
		if (rxp == RX_DATA) {
			set_state(H_DATA);
			data_power = rx.power;
			stat_line(HST_BC_BAUD, HST_BC_CARRIER, HST_BAUD,
				  HST_CARRIER);
			return connect_at(HST_BC_RATE, hst_rates[rate].bps);
		}
		break;
	case H_DATA:
		if (caller && count % ms2samples(100, srate) == 0) {
			stat_snr(-10 * log10f(rx_mse + 1e-6f));
			stat_freq_offset(rx.dtheta, HST_BAUD);
			stat_timing_offset(rx.t_adj, rx.sps);
		}
		break;
	}
	return DPSTAT_OK;
}

/* a sample x in, *y out: the answerer sends the forward channel and
   receives the back channel, the caller the other way round */
s16 HstPump::sample(float x, float *y)
{
	s16 r = handshake();

	*y = 0;
	if (!caller) {
		if (state == H_ANS)
			*y = DSP_TX_AMPLITUDE * tone.next();
		else if (state == H_TRAIN || state == H_DATA)
			*y = tx.sample([this] { return fwd_symbol(); });

		/* the back channel's band, its own forward channel's echo
		   kept out */
		float b = bc_bp[1].run(bc_bp[0].run(x));
		bc_pwr += 0.005f * (b * b - bc_pwr);
		switch (bc_carrier.run(bc_pwr)) {
		case 1:
			if (state != H_DATA)
				bc_rx.reset();
			bc_on = 1;
			break;
		case -1:
			bc_on = 0;
			break;
		}
		carrier_tick(bc_carrier.on);
		if (bc_rx.sample(x))
			bc_rx_symbol();
		return r;
	}

	if (rxp >= RX_ALT)
		*y = bc_tx.sample([this] { return bc_symbol(); });

	if (rx.sample(x))
		rx_symbol();
	switch (carrier.run(rx.power)) {
	case 1:
		if (rxp != RX_DATA) {
			rx.reset();
			rxp = RX_IDLE;
			alt_run = 0;
		}
		break;
	case -1:
		if (rxp != RX_DATA && rxp != RX_RATE) {
			rxp = RX_IDLE;
			alt_run = 0;
		}
		break;
	}
	carrier_tick(carrier.on);
	return r;
}

/* HST: the answerer sends the forward channel at the caller's request
   (+MS's highest rate at most), the caller the back channel; timeouts
   from S7 and S10.  0, or -1 when out of memory. */
s16 HstPump::start(struct modem *m, enum DP_ID id, s16 caller_,
		   s32 srate_, s32 max_frag)
{
	s32 max_bps;

	(void)id;
	(void)max_frag;
	pump_setup(m, srate_, caller_);

	max_bps = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));
	for (max_rate = HST_NRATES - 1; max_rate > 0; max_rate--)
		if (max_bps <= 0 || hst_rates[max_rate].bps <= max_bps)
			break;
	req = asked = -1;
	change_at = -1;
	data_at = 0x7fffffff;

	if (caller_) {
		carrier.init(150, 90);
		if (rx.init(srate_, HST_CARRIER, HST_BAUD, HST_ALPHA,
			    HST_SPAN, HST_EQ_LEN, HST_POWER))
			return -1;
		bc_tx.init(srate_, HST_BC_CARRIER, HST_BC_BAUD, HST_BC_ALPHA,
			   HST_BC_SPAN, sqrtf(HST_BC_POWER), HST_TX_AMPLITUDE);
		bc_scr.init(HST_BC_SEED, 17, 3);
		rxp = RX_IDLE;
		state = H_WAIT;
	} else {
		tx.init(srate_, HST_CARRIER, HST_BAUD, HST_ALPHA, HST_SPAN,
			sqrtf(HST_POWER), HST_TX_AMPLITUDE);
		tone.set_freq(2100, srate_);
		if (bc_rx.init(srate_, HST_BC_CARRIER, HST_BC_BAUD,
			       HST_BC_ALPHA, HST_BC_SPAN, HST_BC_EQ_LEN,
			       HST_BC_POWER))
			return -1;
		bc_bp[0].bandpass(HST_BC_CARRIER, 2, srate_);
		bc_bp[1].bandpass(HST_BC_CARRIER, 2, srate_);
		bc_carrier.init(60, 40);
		bc_dscr.init(0, 17, 3);
		if (rt && rt->ans_sent) {
			rt->ans_sent = 0;
			state = H_GAP;
		} else
			state = H_SILENCE;
	}
	DSP_DBG("hst: create %s, up to %" PRId32 " bit/s, srate %" PRId32
		"\n", caller_ ? "caller" : "answerer",
		hst_rates[max_rate].bps, srate_);
	return 0;
}

/* registers the pump as DP_HST; 0 */
s16 dp_hst_init(void)
{
	modem_dp_register(DP_HST, &PumpOps<HstPump>::ops);
	return 0;
}

/* deregisters it */
void dp_hst_exit(void)
{
	modem_dp_deregister(DP_HST, &PumpOps<HstPump>::ops);
}

} // namespace dsp
