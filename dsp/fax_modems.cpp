/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fax_modems.cpp - half duplex fax modulations: V.21 channel 2
 *                   (300 bit/s HDLC), V.27ter (2400/4800), V.29
 *                   (4800/7200/9600) and V.17 (7200 to 14400)
 *
 *    The training sequences follow the recommendations: segment
 *    lengths, scrambler polynomials and seeds.  The receivers locate
 *    the pseudo random part of the training by correlating the phase
 *    steps with a locally generated copy, which gives the exact symbol
 *    count to the start of data, the absolute carrier phase and known
 *    symbols to train the equalizer on.
 *
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "bits.h"
#include "fax.h"

namespace dsp {

enum {
	SEG_CARRIER,            /* V.27ter: unmodulated carrier */
	SEG_SILENCE,
	SEG_REVERSALS,          /* V.27ter: regular reversals */
	SEG_ABAB,
	SEG_TRN,                /* the pseudo random training part */
	SEG_BRIDGE,             /* V.17 */
	SEG_ONES,               /* scrambled ones through the data path */
	SEG_DATA,
	SEG_SHUTDOWN,           /* ones after the data */
	SEG_TAIL,               /* silence after it */
	SEG_DONE,
};

#define V17_BRIDGE_WORD 0x8880
/* how much slower the V.17 AGC is in the data than in the training */
#define V17_DATA_AGC_SLOWER 16

static const int cdba_to_abcd[4] = { 2, 3, 1, 0 };
static const int dibit_to_step[4] = { 1, 0, 2, 3 };
/* phase steps in 45 degree units */
static const int steps_8[8] = { 1, 0, 2, 3, 6, 7, 5, 4 };
static const int steps_4[4] = { 0, 2, 6, 4 };

static const cplx v27_points[8] = {
	{ 1.414f, 0 }, { 1, 1 }, { 0, 1.414f }, { -1, 1 },
	{ -1.414f, 0 }, { -1, -1 }, { 0, -1.414f }, { 1, -1 },
};

static const cplx v29_points[16] = {
	{ 3, 0 }, { 1, 1 }, { 0, 3 }, { -1, 1 },
	{ -3, 0 }, { -1, -1 }, { 0, -3 }, { 1, -1 },
	{ 5, 0 }, { 3, 3 }, { 0, 5 }, { -3, 3 },
	{ -5, 0 }, { -3, -3 }, { 0, -5 }, { 3, -3 },
};
/* training points, [rate offset + 0/1]: 9600, 7200, 4800 */
static const cplx v29_abab[6] = {
	{ 3, -3 }, { -3, 0 }, { 1, -1 }, { -3, 0 }, { 0, -3 }, { -3, 0 },
};
static const cplx v29_cdcd[6] = {
	{ 3, 0 }, { -3, 3 }, { 3, 0 }, { -1, 1 }, { 3, 0 }, { 0, 3 },
};

/* the T.31 class 1 <mod> code (3, 24 ... 146) to a modulation; the
   V.17 codes one above a rate (74, 98, 122, 146) set short_train.  1, or
   0 for a code that has none */
int fax_mod_from_code(int code, enum fax_mod *mod, int *short_train)
{
	*short_train = 0;
	switch (code) {
	case 3:   *mod = FM_V21; break;
	case 24:  *mod = FM_V27_2400; break;
	case 48:  *mod = FM_V27_4800; break;
	case 72:  *mod = FM_V29_7200; break;
	case 96:  *mod = FM_V29_9600; break;
	case 73:  *mod = FM_V17_7200; break;
	case 74:  *mod = FM_V17_7200; *short_train = 1; break;
	case 97:  *mod = FM_V17_9600; break;
	case 98:  *mod = FM_V17_9600; *short_train = 1; break;
	case 121: *mod = FM_V17_12000; break;
	case 122: *mod = FM_V17_12000; *short_train = 1; break;
	case 145: *mod = FM_V17_14400; break;
	case 146: *mod = FM_V17_14400; *short_train = 1; break;
	default: return 0;
	}
	return 1;
}

/* the data rate of a modulation, bit/s */
int fax_mod_bps(enum fax_mod mod)
{
	static const int bps[] = { 0, 300, 2400, 4800, 4800, 7200, 9600,
				   7200, 9600, 12000, 14400 };
	return bps[mod];
}

/* the transmit level of a modulation, rms in 16 bit sample units */
float fax_tx_rms(enum fax_mod mod)
{
	switch (mod) {
	case FM_V21:       return 4096;
	case FM_V27_2400:
	case FM_V27_4800:  return 3333;
	case FM_V29_4800:
	case FM_V29_7200:
	case FM_V29_9600:  return 3446;
	default:           return 3920;     /* V.17 */
	}
}

/* the modulation families */
static int is_v17(enum fax_mod m) { return m >= FM_V17_7200; }
/* (see is_v17()) */
static int is_v29(enum fax_mod m) { return m >= FM_V29_4800 && m <= FM_V29_9600; }
/* (see is_v17()) */
static int is_v27(enum fax_mod m) { return m == FM_V27_2400 || m == FM_V27_4800; }

/* data bits per symbol (V.17: those into the trellis encoder, which
   adds one) */
static int data_bits(enum fax_mod m)
{
	switch (m) {
	case FM_V27_2400: return 2;
	case FM_V27_4800: return 3;
	case FM_V29_4800: return 2;
	case FM_V29_7200: return 3;
	case FM_V29_9600: return 4;
	case FM_V17_7200: return 3;
	case FM_V17_9600: return 4;
	case FM_V17_12000: return 5;
	case FM_V17_14400: return 6;
	default: return 1;
	}
}

/* the V.17 constellation of a rate (the V.32 trellis coded ones) */
static const cplx *v17_points(enum fax_mod m)
{
	switch (m) {
	case FM_V17_7200: return v32_points_7200;
	case FM_V17_9600: return v32_points_9600;
	case FM_V17_12000: return v32_points_12000;
	default: return v32_points_14400;
	}
}

/* where the points of a V.29 rate are in v29_abab[] and v29_cdcd[] */
static int v29_offset(enum fax_mod m)
{
	return m == FM_V29_9600 ? 0 : m == FM_V29_7200 ? 2 : 4;
}

/* mean power of the data constellation */
static float mod_power(enum fax_mod m)
{
	if (is_v17(m))
		return 40.0f;
	if (is_v27(m))
		return 2.0f;
	switch (m) {
	case FM_V29_9600: return 13.5f;
	case FM_V29_7200: return 5.5f;
	default: return 9.0f;
	}
}

/* mean power of the data symbols: V.17's constellations at 14400 and
   12000 are a little stronger than their training (A, B, C and D,
   mod_power()) */
static float data_mean_power(enum fax_mod m)
{
	switch (m) {
	case FM_V17_14400: return 41.0f;
	case FM_V17_12000: return 42.0f;
	default: return mod_power(m);
	}
}

/* the symbol rate, baud */
static float mod_baud(enum fax_mod m)
{
	if (m == FM_V27_2400)
		return 1200;
	if (m == FM_V27_4800)
		return 1600;
	return 2400;
}

/* the carrier frequency, Hz */
static float mod_carrier(enum fax_mod m)
{
	return is_v29(m) ? 1700 : 1800;
}

/* the roll-off of the raised cosine pulse */
static float mod_alpha(enum fax_mod m)
{
	return is_v17(m) ? 0.25f : 0.5f;
}

/* training lengths in symbols */
static int trn_len(enum fax_mod m, int short_train)
{
	if (is_v17(m))
		return short_train ? 38 : 2976;
	if (is_v29(m))
		return 384;
	return 1074;
}

/* the scrambled ones before the data, symbols (V.27ter segment 5,
   V.29 and V.17 segment 4) */
static int ones_len(enum fax_mod m)
{
	return is_v27(m) ? 8 : 48;
}


/*
 *    scramblers
 */

/* 1 + x^-6 + x^-7 with the V.27ter repeated pattern guard */
static int scr7(unsigned *reg, int *count, int bit)
{
	int out = (bit ^ (*reg >> 5) ^ (*reg >> 6)) & 1;
	if (*count >= 33) {
		out ^= 1;
		*count = 0;
	} else if (((*reg >> 7) ^ out) & ((*reg >> 8) ^ out) &
		   ((*reg >> 11) ^ out) & 1)
		*count = 0;
	else
		(*count)++;
	*reg = (*reg << 1) | out;
	return out;
}

/* its descrambler, the guard counting on the received bits as the
   scrambler's does on the sent ones */
static int descr7(unsigned *reg, int *count, int bit)
{
	int out = (bit ^ (*reg >> 5) ^ (*reg >> 6)) & 1;
	if (*count >= 33) {
		out ^= 1;
		*count = 0;
	} else if (((*reg >> 7) ^ bit) & ((*reg >> 8) ^ bit) &
		   ((*reg >> 11) ^ bit) & 1)
		*count = 0;
	else
		(*count)++;
	*reg = (*reg << 1) | (bit & 1);
	return out;
}

/* V.29 training sequence generator (1 + x^-6 + x^-7, seed 0x2A) */
static int v29_trn_bit(unsigned *reg)
{
	int bit = *reg & 1;
	*reg >>= 1;
	*reg |= ((bit ^ *reg) & 1) << 6;
	return bit;
}


/*
 *    transmitter
 */

/* the next bit into the scrambler: the caller's in the data segment,
   else a one; the caller's FAX_BIT_END sets ending (a one goes out) */
int FaxTx::data_bit()
{
	int b;
	if (seg != SEG_DATA)
		return 1;
	b = get_bit(ctx);
	if (b == FAX_BIT_END) {
		ending = 1;
		return 1;
	}
	return b & 1;
}

/* the next symbol of the data path: the bits scrambled, then V.17
   through the trellis encoder, V.29 and V.27ter as a phase change from
   the last state (V.29 9600: the first bit picks the amplitude) */
cplx FaxTx::data_symbol()
{
	int n = data_bits(mod), q = 0, k, amp = 0, bits;

	if (is_v17(mod)) {
		for (k = 0; k < n; k++)
			q |= scramble(&scr, SCR_GPC, data_bit()) << k;
		return v17_points(mod)[trellis.encode(q, n)];
	}
	if (is_v29(mod)) {
		if (mod == FM_V29_9600 && scramble(&scr, SCR_GPC, data_bit()))
			amp = 8;
		bits = scramble(&scr, SCR_GPC, data_bit());
		bits = (bits << 1) | scramble(&scr, SCR_GPC, data_bit());
		if (mod == FM_V29_4800)
			bits = steps_4[bits];
		else
			bits = steps_8[(bits << 1) |
				       scramble(&scr, SCR_GPC, data_bit())];
		state = (state + bits) & 7;
		return v29_points[amp | state];
	}
	/* V.27ter */
	bits = scr7(&scr, &scr_count, data_bit());
	bits = (bits << 1) | scr7(&scr, &scr_count, data_bit());
	if (mod == FM_V27_4800)
		bits = steps_8[(bits << 1) | scr7(&scr, &scr_count, data_bit())];
	else
		bits = steps_4[bits];
	state = (state + bits) & 7;
	return v27_points[state];
}

/* on to training segment seg_, counting its symbols from 0 */
void FaxTx::next_segment(int seg_)
{
	seg = seg_;
	seg_sym = 0;
}

/* the next symbol (for qam.sample()).  The training segments one after
   the other (their lengths in symbols), the ones, the data until the
   caller's FAX_BIT_END, the shutdown ones and silence; done at the end */
static cplx fax_tx_symbol(FaxTx *t)
{
	cplx z = { 0, 0 };
	int b1, b2, off, shift;

	/* segment changes */
	switch (t->seg) {
	case SEG_CARRIER:
		if (t->seg_sym >= 320)
			t->next_segment(SEG_SILENCE);
		break;
	case SEG_SILENCE:
		if (t->seg_sym >= (is_v27(t->mod) ? 32 : 48))
			t->next_segment(is_v27(t->mod) ? SEG_REVERSALS : SEG_ABAB);
		break;
	case SEG_REVERSALS:
		if (t->seg_sym >= 50)
			t->next_segment(SEG_TRN);
		break;
	case SEG_ABAB:
		if (t->seg_sym >= (is_v17(t->mod) ? 256 : 128))
			t->next_segment(SEG_TRN);
		break;
	case SEG_TRN:
		if (t->seg_sym >= trn_len(t->mod, t->short_train))
			t->next_segment(is_v17(t->mod) && !t->short_train ?
					SEG_BRIDGE : SEG_ONES);
		break;
	case SEG_BRIDGE:
		if (t->seg_sym >= 64)
			t->next_segment(SEG_ONES);
		break;
	case SEG_ONES:
		/* after a V.17 short train and with V.27ter the original
		   library sends some 60 ms of ones, and its receiver needs
		   them: depending on where its 20 ms frames fall it starts
		   the data early, with up to 200 symbols to converge at
		   14400.  100 ms of them cover that. */
		if (t->seg_sym >= ((is_v17(t->mod) && t->short_train) ||
				    is_v27(t->mod) ?
				    (int)mod_baud(t->mod) / 10 :
				    ones_len(t->mod)))
			t->next_segment(SEG_DATA);
		break;
	case SEG_DATA:
		if (t->ending)
			t->next_segment(SEG_SHUTDOWN);
		break;
	case SEG_SHUTDOWN:
		/* V.17: 20 ms more of ones, the original library's receiver
		   may lose the end of the data with its last frame */
		if (t->seg_sym >= (is_v17(t->mod) ? 80 : 32))
			t->next_segment(SEG_TAIL);
		break;
	case SEG_TAIL:
		if (t->seg_sym >= 48) {
			t->next_segment(SEG_DONE);
			t->done = 1;
		}
		break;
	}

	switch (t->seg) {
	case SEG_CARRIER:
		z = v27_points[0];
		break;
	case SEG_REVERSALS:
		t->state = (t->state + 4) & 7;
		z = v27_points[t->state];
		break;
	case SEG_ABAB:
		if (is_v17(t->mod))
			z = v32_abcd[t->seg_sym & 1];
		else
			z = v29_abab[v29_offset(t->mod) + !(t->seg_sym & 1)];
		break;
	case SEG_TRN:
		if (is_v17(t->mod)) {
			b1 = scramble(&t->scr, SCR_GPC, 1);
			b2 = scramble(&t->scr, SCR_GPC, 1);
			t->state = cdba_to_abcd[b1 << 1 | b2];
			z = v32_abcd[t->state];
		} else if (is_v29(t->mod)) {
			off = v29_offset(t->mod);
			z = v29_cdcd[off + v29_trn_bit(&t->tscr)];
		} else {
			b1 = scr7(&t->scr, &t->scr_count, 1);
			scr7(&t->scr, &t->scr_count, 1);
			scr7(&t->scr, &t->scr_count, 1);
			t->state = (t->state + (b1 << 2)) & 7;
			z = v27_points[t->state];
		}
		break;
	case SEG_BRIDGE:
		shift = (t->seg_sym & 7) << 1;
		b1 = scramble(&t->scr, SCR_GPC, V17_BRIDGE_WORD >> shift);
		b2 = scramble(&t->scr, SCR_GPC,
			      V17_BRIDGE_WORD >> (shift + 1));
		t->state = (t->state + dibit_to_step[b1 << 1 | b2]) & 3;
		z = v32_abcd[t->state];
		break;
	case SEG_ONES:
	case SEG_DATA:
	case SEG_SHUTDOWN:
		z = t->data_symbol();
		break;
	default:
		break;
	}
	t->seg_sym++;
	return z;
}

/* a transmitter for mod_ at srate_ samples/s, short_train_ for the V.17
   short training; get_bit_(ctx_) gives the data bits.  V.21 sends the
   caller's bits from the start (the HDLC flags are the caller's) */
void FaxTx::init(enum fax_mod mod_, int short_train_, int srate_, fax_get_bit_t get_bit_, void *ctx_)
{
	reinit(*this);
	mod = mod_;
	short_train = short_train_;
	srate = srate_;
	get_bit = get_bit_;
	ctx = ctx_;
	if (mod_ == FM_V21) {
		fsk.init(1650, 1850, fax_tx_rms(mod_) * 1.41421f, 300.0f,
			 srate_);
		seg = SEG_DATA;
		return;
	}
	qam.init(srate_, mod_carrier(mod_), mod_baud(mod_),
		    mod_alpha(mod_), 6, sqrtf(mod_power(mod_)),
		    fax_tx_rms(mod_) * 1.41421f);
	/* the receivers of the original library want the symbols at this
	   phase of the sample clock (as with V.32) */
	qam.t = is_v17(mod_) ? 0.25f : 0.5f;
	if (is_v17(mod_)) {
		scr = 0x2ECDD5;
		/* after a short train the original library goes on with
		   segment 4 at once, without the bridge, its trellis encoder
		   in this state (its receiver expects it) */
		trellis.diff = short_train_ ? 3 : 1;
		trellis.conv = short_train_ ? 7 : 0;
		seg = SEG_ABAB;
	} else if (is_v29(mod_)) {
		tscr = 0x2A;
		seg = SEG_SILENCE;
	} else {
		scr = 0x3C;
		seg = SEG_CARRIER;
	}
}

/* the next output sample, 16 bit scale; 0 once done */
float FaxTx::sample()
{
	if (done)
		return 0;
	if (mod == FM_V21) {
		float y = fsk.sample([this] { return get_bit(ctx); });
		if (fsk.ended)
			done = 1;
		return y;
	}
	return qam.sample([this] { return fax_tx_symbol(this); });
}


/*
 *    receiver
 */

/* the phase state of a received symbol: V.17 the nearest of A, B, C, D
   (0-3), else the nearest multiple of 45 degrees (0-7) */
int FaxRx::state_of(cplx z)
{
	if (is_v17(mod))
		return nearest_point(v32_abcd, 4, z);
	return (int)lrintf(atan2f(z.im, z.re) / (float)(M_PI / 4)) & 7;
}

/* the absolute phase state of a training point */
int FaxRx::point_state(cplx p)
{
	return state_of(p);
}

/* the point reference symbol k is sent as */
cplx FaxRx::ref_point(int k)
{
	int s = ref[k];
	if (is_v17(mod))
		return v32_abcd[s];
	if (is_v29(mod)) {
		/* the C/D points of this rate */
		int off = v29_offset(mod);
		return s == point_state(v29_cdcd[off]) ?
			v29_cdcd[off] : v29_cdcd[off + 1];
	}
	return v27_points[s];
}

/* the training symbols as phase states, and the scrambler state at the
   start of the data path */
int FaxRx::make_reference()
{
	int n = trn_len(mod, short_train), k, b1, b2, shift, state_ = 0;
	unsigned scr_ = 0, tscr = 0x2A;
	int count = 0;
	signed char *ref_;

	if (is_v17(mod))
		n += 64;                /* the bridge is known as well */
	if (!alloc_zeroed(ref, n))
		return -1;
	ref_ = ref.data();
	ref_len = n;
	if (is_v17(mod)) {
		scr_ = 0x2ECDD5;
		for (k = 0; k < n; k++) {
			if (k < trn_len(mod, short_train)) {
				b1 = scramble(&scr_, SCR_GPC, 1);
				b2 = scramble(&scr_, SCR_GPC, 1);
				state_ = cdba_to_abcd[b1 << 1 | b2];
			} else {
				shift = ((k - trn_len(mod, short_train)) & 7) << 1;
				b1 = scramble(&scr_, SCR_GPC,
					      V17_BRIDGE_WORD >> shift);
				b2 = scramble(&scr_, SCR_GPC,
					      V17_BRIDGE_WORD >> (shift + 1));
				state_ = (state_ + dibit_to_step[b1 << 1 | b2]) & 3;
			}
			ref_[k] = state_;
		}
		scr = scr_;
	} else if (is_v29(mod)) {
		int off = v29_offset(mod);
		for (k = 0; k < n; k++)
			ref_[k] = point_state(v29_cdcd[off + v29_trn_bit(&tscr)]);
		scr = 0;
	} else {
		/* the regular reversals leave the state at 50 * 4 = 0 mod 8 */
		scr_ = 0x3C;
		for (k = 0; k < n; k++) {
			b1 = scr7(&scr_, &count, 1);
			scr7(&scr_, &count, 1);
			scr7(&scr_, &count, 1);
			state_ = (state_ + (b1 << 2)) & 7;
			ref_[k] = state_;
		}
		scr = scr_;
		scr_count = count;
	}
	return 0;
}

/* a receiver for mod_ at srate_ samples/s, short_train_ for the V.17
   short training; put_bit_(ctx_) gets the descrambled data bits once it
   has trained.  mem: what a V.17 long train left (may be NULL).  0, or
   -1 when out of memory */
int FaxRx::init(enum fax_mod mod_, int short_train_, int srate_, fax_put_bit_t put_bit_, void *ctx_, const FaxRxMemory *mem)
{
	reinit(*this);
	mod = mod_;
	short_train = short_train_;
	srate = srate_;
	put_bit = put_bit_;
	ctx = ctx_;
	if (mod_ == FM_V21) {
		fsk.init(1650, 1850, 300, srate_);
		return 0;
	}
	if (qam.init(srate_, mod_carrier(mod_), mod_baud(mod_),
			mod_alpha(mod_), 6, 32, mod_power(mod_)))
		return -1;
	det_alpha = 1.0f / (0.005f * srate_);
	det_hold = srate_ / 25;       /* learn the noise floor first */
	det_floor = 1e12f;
	agc_alpha = qam.agc_alpha;
	alt_end = -1;
	nstates = is_v17(mod_) ? 4 : 8;
	if (make_reference()) {
		qam.release();
		return -1;
	}
	if (is_v17(mod_))
		vit.init(v17_points(mod_), data_bits(mod_));
	/* a short train only touches up what the long one learned */
	if (short_train_ && mem && mem->valid && is_v17(mem->mod)) {
		memcpy(mem_eq, mem->eq, sizeof(mem_eq));
		mem_gain = mem->agc_gain;
		use_mem = 1;
	}
	return 0;
}

/* keep the trained equalizer for a later short train (V.17, when a good
   one was seen; else mem stays as it was) */
void FaxRx::save(FaxRxMemory *mem)
{
	if (!is_v17(mod) || !have_good)
		return;
	mem->mod = mod;
	memcpy(mem->eq, good_eq, sizeof(mem->eq));
	mem->agc_gain = good_gain;
	mem->valid = 1;
}

/* free the demodulator and the reference */
void FaxRx::release()
{
	if (mod == FM_V21 || mod == FM_NONE)
		return;
	qam.release();
	free_buffer(ref);
}

/* the n bits of a symbol's value q (the highest first if msb_first)
   through the descrambler, to put_bit once trained */
void FaxRx::bits_out(int q, int n, int msb_first)
{
	int k;
	for (k = 0; k < n; k++) {
		int bit = msb_first ? (q >> (n - 1 - k)) & 1 : (q >> k) & 1;
		bit = is_v27(mod) ? descr7(&scr, &scr_count, bit) :
			descramble(&scr, SCR_GPC, bit);
		if (trained)
			put_bit(ctx, bit);
	}
}

/* V.17: the loops at the start of the data.  The training leaves the
   level and the timing known; what is left, the data itself moves:
   - the AGC (30 ms, some 70 symbols) follows the mean power of the
     symbols it has just seen, and at 14400 the corners are some 4
     times the mean: a run of outer or inner points moves the gain by a
     few per cent, which a page's rows of the same few patterns do,
     and the outer points then land a good part of their spacing off.
     16 times slower, the equalizer and the gain correction in
     data_symbol() follow the rest.
   - the AGC aims at the data's mean power, a little higher at 14400
     and 12000 than the training's; its estimate moves with it, so the
     gain does not jump.
   - Gardner's timing error on the data is noisier than on the
     training: half the training's gain (and a sixth of it after 1000
     symbols). */
void FaxRx::data_start()
{
	float r;

	if (!is_v17(mod))
		return;
	r = data_mean_power(mod) / mod_power(mod);
	qam.agc_target *= r;
	qam.agc_pwr *= r;
	qam.agc_alpha = agc_alpha / V17_DATA_AGC_SLOWER;
	qam.kt_p = 0.015f * qam.sps;
}

/* a data symbol: the decision (V.17: the Viterbi decoder's, late), the
   bits out, the error and power estimates, the V.17 gain correction, the
   equalizer's adaptation and the copies of a good one for save() */
void FaxRx::data_symbol()
{
	cplx z = qam.z, d;
	int n = data_bits(mod), k, best, phase, dp, bits;

	if (is_v17(mod)) {
		const cplx *pts = v17_points(mod);
		int q;
		best = nearest_point(pts, 1 << (n + 1), z);
		d = pts[best];
		q = vit.decode(z, nullptr);
		data_sym++;
		if (q >= 0) {
			/* symbols leave the decoder VITERBI_DEPTH - 1 late */
			if (data_sym - (VITERBI_DEPTH - 1) > ones_len(mod))
				trained = 1;
			bits_out(q, n, 0);
		}
	} else {
		const cplx *pts = is_v29(mod) ? v29_points : v27_points;
		int npts = mod == FM_V29_9600 ? 16 : 8;
		/* (V.29 4800: the even points) */
		best = nearest_point(pts, npts, z,
				     mod == FM_V29_4800 ? 2 : 1);
		d = pts[best];
		phase = best & 7;
		dp = (phase - state) & 7;
		state = phase;
		if (++data_sym > ones_len(mod))
			trained = 1;
		if (n == 2 && mod != FM_V27_2400) {
			/* V.29 4800 */
			for (bits = 0; bits < 4 && steps_4[bits] != dp; bits++)
				;
			bits_out(bits & 3, 2, 1);
		} else if (mod == FM_V27_2400) {
			for (bits = 0; bits < 4 && steps_4[bits] != dp; bits++)
				;
			bits_out(bits & 3, 2, 1);
		} else {
			for (bits = 0; bits < 8 && steps_8[bits] != dp; bits++)
				;
			bits &= 7;
			if (mod == FM_V29_9600)
				bits_out((best >> 3) << 3 | bits, 4, 1);
			else
				bits_out(bits, 3, 1);
		}
	}
	mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / data_mean_power(mod) - mse);
	zpow += 0.01f * (cplx_pow(z) - zpow);
	/* V.17: the original library's data constellation comes some 1.4 dB
	   below its training; decisions on the dense rates then miss and
	   the equalizer cannot pull the gain back.  Scrambled data has the
	   constellation's mean power: follow it. */
	/* slowly: the power of a few hundred data symbols wanders by some
	   per cent, and every step disturbs a good signal */
	if (data_sym == 150)
		zpow_slow = zpow;
	zpow_slow += 0.002f * (cplx_pow(z) - zpow_slow);
	if (is_v17(mod) && data_sym >= 600 && (data_sym & 255) == 0 &&
	    zpow_slow > mse * data_mean_power(mod)) {
		/* the signal's part of the power: less the noise around the
		   decisions */
		float f = sqrtf(data_mean_power(mod) /
				(zpow_slow - mse * data_mean_power(mod)));
		/* only a clear error */
		if (fabsf(f - 1) > 0.06f) {
			f = 1 + 0.5f * (f - 1);
			if (f > 1.1f)
				f = 1.1f;
			if (f < 0.9f)
				f = 0.9f;
			for (k = 0; k < qam.eq_len; k++)
				qam.eq[k] = cplx_scale(qam.eq[k], f);
			zpow *= f * f;
			zpow_slow *= f * f;
			DSP_DBG("fax rx: gain %.3f at symbol %ld\n", f,
				(long)data_sym);
		}
	}
	if (data_sym % 2400 == 0)
		DSP_DBG("fax rx: data mse %.5f (%.1f dB), power %.3f of the "
			"constellation's\n", mse, -10 * log10f(mse + 1e-9f),
			zpow / data_mean_power(mod));
	/* the timing loop slows down once it has surely settled (a short
	   train leaves it little time) */
	if (data_sym == 1000) {
		qam.kt_p = 0.005f * qam.sps;
		qam.kt_gi = 0.003f;
	}
	/* nothing to learn once the carrier is gone */
	if (carrier)
		/* the training of V.29 and V.27ter only covers one axis: the
		   equalizer still learns fast at the start of the data */
		qam.adapt(d, is_v17(mod) ? 0.005f :
			     data_sym < 100 ? 0.03f :
			     data_sym < 300 ? 0.015f : 0.005f, 0.02f, 0.0002f);
	/* keep a copy of a good equalizer for a later short train (the
	   end of the signal would spoil the live one) */
	mse_fast += 0.1f * (cplx_pow(cplx_sub(z, d)) / data_mean_power(mod) -
			       mse_fast);
	if (data_sym == 1)
		data_power = qam.power;
	if (is_v17(mod) && mse < 0.01f && mse_fast < 0.01f &&
	    qam.power > 0.5f * data_power && (data_sym & 127) == 0) {
		/* the newest copy may already see the end of the signal, the
		   saved one is the copy before it */
		memcpy(good_eq, new_eq, sizeof(good_eq));
		good_gain = new_gain;
		have_good = have_new;
		memcpy(new_eq, qam.eq, sizeof(new_eq));
		new_gain = qam.agc_gain;
		have_new = 1;
	}
}

/* how well the last 32 phase steps fit the reference ending at symbol k */
int FaxRx::window_match(int k)
{
	int i, match = 0;
	for (i = 0; i < 32; i++)
		match += steps[i] == ((ref[k - 31 + i] - ref[k - 32 + i]) &
					 (nstates - 1));
	return match;
}

/* the V.29 and V.27ter training sequences repeat every 127 symbols: the
   end of the alternations / reversals before them tells which period is
   being received */
void FaxRx::track_alternations(cplx z)
{
	float d1 = cplx_pow(cplx_sub(z, zh[0]));
	float d2 = cplx_pow(cplx_sub(z, zh[1]));
	if (d2 < 0.25f * d1) {
		alt_run++;
	} else {
		if (alt_run >= 16)
			alt_end = onset;
		alt_run = 0;
	}
	zh[1] = zh[0];
	zh[0] = z;
}

/* a symbol from the demodulator: search for the training (the phase
   steps against the reference, which gives the symbol's place in it and
   the absolute phase), train on the known symbols, then the data */
void FaxRx::symbol()
{
	cplx z = qam.z;
	int s, k, match;

	switch (stage) {
	case RXS_SEARCH:
		if (carrier && ++onset == 6)
			qam.timing_hold = 0;
		if (onset == 48)
			qam.agc_alpha = agc_alpha;
		/* on the alternations, the symbol samples are the strong
		   ones; when the mid samples are, the timing started half a
		   symbol off, where the timing loop is slow to leave */
		if (onset > 8 && onset <= 24) {
			sym_pow += cplx_pow(qam.y);
			mid_pow += cplx_pow(qam.mid);
		}
		if (onset == 24 && mid_pow > 1.5f * sym_pow) {
			qam.half ^= 1;
			DSP_DBG("fax rx: timing moved half a symbol\n");
		}
		track_alternations(z);
		s = state_of(z);
		memmove(steps, steps + 1, 31 * sizeof(int));
		steps[31] = (s - last_state) & (nstates - 1);
		last_state = s;
		if (nsteps < 32)
			nsteps++;
		/* hold the phase on the training grid */
		qam.adapt(is_v17(mod) ? v32_abcd[s] :
			     cplx_scale(v27_points[s], sqrtf(mod_power(mod) / 2)),
			     0, 0.05f, 0);
		if (nsteps < 32 || !carrier)
			break;
		{
			/* where in the training is this symbol? */
			int lo = 32, hi = 32, best = 32, m;
			if (alt_end >= 0) {
				lo = onset - alt_end - 4;
				hi = onset - alt_end + 8;
				if (lo < 32)
					lo = 32;
				if (hi > ref_len - 2)
					hi = ref_len - 2;
			}
			for (k = lo, match = -1; k <= hi; k++)
				if ((m = window_match(k)) > match) {
					match = m;
					best = k;
				}
			k = best;
		}
		if (match >= 29) {
			/* this symbol is reference symbol k */
			if (use_mem) {
				/* short train: continue with the equalizer of
				   the long train, timing has settled by now */
				memcpy(qam.eq, mem_eq, sizeof(qam.eq));
				qam.recompute();
				/* and its gain: the level here is the one of the
				   alternations, whose power is in the band edges
				   and comes out some 0.6 dB off.  A clearly
				   different level is a new one, kept. */
				if (mem_gain > 0 && qam.agc_gain > 0 &&
				    fabsf(20 * log10f(qam.agc_gain / mem_gain)) <
				    1.5f) {
					qam.agc_pwr = qam.agc_target /
						(mem_gain * mem_gain);
					qam.agc_gain = mem_gain;
					qam.agc_alpha = agc_alpha /
						V17_DATA_AGC_SLOWER;
				}
				DSP_DBG("fax rx: equalizer of the long train\n");
			}
			qam.align_phase(ref_point(k));
			seg_sym = k + 1;
			stage = RXS_TRAIN;
			bridge_hits = 0;
			DSP_DBG("fax rx: training found at %d\n", k);
		}
		break;
	case RXS_TRAIN: {
		cplx d = ref_point(seg_sym);
		int trn = trn_len(mod, short_train);
		/* V.17 short train: some modems send the 64 symbol bridge as
		   after a long train, the original library and spandsp go on
		   with the scrambled ones (segment 4) at once.  Its first
		   symbols tell; without the bridge they are data, and a few
		   of the ones are not missed. */
		if (is_v17(mod) && short_train &&
		    seg_sym >= trn && seg_sym < trn + 8) {
			if (cplx_pow(cplx_sub(z, d)) < 0.2f * cplx_pow(d))
				bridge_hits++;
			if (++seg_sym == trn + 8 && bridge_hits < 6) {
				DSP_DBG("fax rx: short train without the "
					"bridge\n");
				stage = RXS_DATA;
				data_sym = 8;
				data_start();
				vit.last_y12 = 0;
				DSP_DBG("fax rx: data\n");
			}
			break;
		}
		/* V.17 bridge: Agere's is not the one of the reference, and
		   a wrong reference winds the carrier loop up to a frequency
		   offset of some hertz.  Its points are A, B, C or D, so the
		   nearest one is the decision. */
		if (is_v17(mod) && seg_sym >= trn)
			d = v32_abcd[state_of(z)];
		/* the original library's V.27ter training has stray 180
		   degree flips: follow them, its points are all on one axis */
		if (is_v27(mod) &&
		    cplx_pow(cplx_add(z, d)) < cplx_pow(cplx_sub(z, d)))
			d = cplx_scale(d, -1);
		qam.adapt(d, 0.02f, 0.05f, 0.0005f);
		if (++seg_sym >= ref_len) {
			stage = RXS_DATA;
			data_sym = 0;
			data_start();
			/* the data path continues from the last training
			   state (V.27ter, V.17 bridge) or from 0 (V.29) */
			if (is_v17(mod))
				vit.last_y12 = short_train ? 0 : 1;
			else if (is_v27(mod))
				state = state_of(d);
			else
				state = 0;
			DSP_DBG("fax rx: data\n");
		}
		break;
	}
	case RXS_DATA:
		data_symbol();
		break;
	}
}

/* an input sample (16 bit scale): V.21 bits straight to put_bit, else
   the demodulator and the carrier detector, on 6 dB over the noise
   floor, off 12 dB under the signal; carrier_run counts the samples
   since it changed, positive with a carrier, negative without */
void FaxRx::sample(float x)
{
	if (mod == FM_V21) {
		int bit = fsk.run(x);
		carrier = fsk.carrier;
		trained = carrier;
		if (bit >= 0 && carrier)
			put_bit(ctx, bit);
		carrier_run = carrier ? (carrier_run > 0 ? carrier_run + 1 : 1) :
			(carrier_run < 0 ? carrier_run - 1 : -1);
		return;
	}
	if (qam.sample(x))
		symbol();
	/* the 20 ms power of the demodulator would bridge the short gap
	   between V.21 and the training */
	det_pwr += det_alpha * (x * x - det_pwr);
	/* thresholds relative to the noise floor and the signal level */
	if (carrier)
		det_level += 0.001f * (det_pwr - det_level);
	else if (det_hold > 0) {
		/* the end of a signal dies away: its minimum so far */
		if (det_pwr < det_floor)
			det_floor = det_pwr;
	} else
		det_floor += 0.002f * (det_pwr - det_floor);
	if (det_hold > 0)
		det_hold--;
	if (!carrier && det_pwr > tone_power(150) &&
	    det_pwr > 4 * det_floor && det_hold < srate / 100) {
		carrier = 1;
		det_level = det_pwr;
		if (stage == RXS_SEARCH) {
			/* acquire the level quickly, and hold the timing
			   until it is known */
			qam.reset();
			qam.agc_pwr = 0;
			qam.agc_alpha = 1.0f / (0.003f * srate);
			qam.timing_hold = 1;
			onset = 0;
			alt_run = 0;
			alt_end = -1;
			sym_pow = mid_pow = 0;
		}
	} else if (carrier && (det_pwr < tone_power(100) ||
				  det_pwr < 0.06f * det_level)) {
		carrier = 0;
		/* the end of the signal dies away (V.27ter has only 20 ms
		   of silence before its training) */
		det_hold = srate / 50;
		det_floor = det_pwr;
		qam.agc_alpha = agc_alpha;
		qam.timing_hold = 0;
		if (stage != RXS_DATA) {
			stage = RXS_SEARCH;
			nsteps = 0;
		}
	}
	if (carrier)
		carrier_run = carrier_run > 0 ? carrier_run + 1 : 1;
	else
		carrier_run = carrier_run < 0 ? carrier_run - 1 : -1;
}

} // namespace dsp
