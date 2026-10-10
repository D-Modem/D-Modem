/*
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

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

int fax_mod_bps(enum fax_mod mod)
{
	static const int bps[] = { 0, 300, 2400, 4800, 4800, 7200, 9600,
				   7200, 9600, 12000, 14400 };
	return bps[mod];
}

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

static int is_v17(enum fax_mod m) { return m >= FM_V17_7200; }
static int is_v29(enum fax_mod m) { return m >= FM_V29_4800 && m <= FM_V29_9600; }
static int is_v27(enum fax_mod m) { return m == FM_V27_2400 || m == FM_V27_4800; }

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

static const cplx *v17_points(enum fax_mod m)
{
	switch (m) {
	case FM_V17_7200: return v32_points_7200;
	case FM_V17_9600: return v32_points_9600;
	case FM_V17_12000: return v32_points_12000;
	default: return v32_points_14400;
	}
}

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

static float mod_baud(enum fax_mod m)
{
	if (m == FM_V27_2400)
		return 1200;
	if (m == FM_V27_4800)
		return 1600;
	return 2400;
}

static float mod_carrier(enum fax_mod m)
{
	return is_v29(m) ? 1700 : 1800;
}

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

static int ones_len(enum fax_mod m)
{
	return is_v27(m) ? 8 : 48;
}


/*
 *    scramblers
 */

/* 1 + x^-18 + x^-23 (V.17, V.29) */
static int scr23(unsigned *reg, int bit)
{
	int out = (bit ^ (*reg >> 17) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | out;
	return out;
}

static int descr23(unsigned *reg, int bit)
{
	int out = (bit ^ (*reg >> 17) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | (bit & 1);
	return out;
}

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

static int tx_data_bit(struct fax_tx *t)
{
	int b;
	if (t->seg != SEG_DATA)
		return 1;
	b = t->get_bit(t->ctx);
	if (b == FAX_BIT_END) {
		t->ending = 1;
		return 1;
	}
	return b & 1;
}

static cplx tx_data_symbol(struct fax_tx *t)
{
	int n = data_bits(t->mod), q = 0, k, amp = 0, bits;

	if (is_v17(t->mod)) {
		for (k = 0; k < n; k++)
			q |= scr23(&t->scr, tx_data_bit(t)) << k;
		return v17_points(t->mod)[t->trellis.encode(q, n)];
	}
	if (is_v29(t->mod)) {
		if (t->mod == FM_V29_9600 && scr23(&t->scr, tx_data_bit(t)))
			amp = 8;
		bits = scr23(&t->scr, tx_data_bit(t));
		bits = (bits << 1) | scr23(&t->scr, tx_data_bit(t));
		if (t->mod == FM_V29_4800)
			bits = steps_4[bits];
		else
			bits = steps_8[(bits << 1) | scr23(&t->scr, tx_data_bit(t))];
		t->state = (t->state + bits) & 7;
		return v29_points[amp | t->state];
	}
	/* V.27ter */
	bits = scr7(&t->scr, &t->scr_count, tx_data_bit(t));
	bits = (bits << 1) | scr7(&t->scr, &t->scr_count, tx_data_bit(t));
	if (t->mod == FM_V27_4800)
		bits = steps_8[(bits << 1) | scr7(&t->scr, &t->scr_count, tx_data_bit(t))];
	else
		bits = steps_4[bits];
	t->state = (t->state + bits) & 7;
	return v27_points[t->state];
}

static void tx_next_seg(struct fax_tx *t, int seg)
{
	t->seg = seg;
	t->seg_sym = 0;
}

static cplx fax_tx_symbol(void *ctx)
{
	struct fax_tx *t = static_cast<fax_tx*>(ctx);
	cplx z = { 0, 0 };
	int b1, b2, off, shift;

	/* segment changes */
	switch (t->seg) {
	case SEG_CARRIER:
		if (t->seg_sym >= 320)
			tx_next_seg(t, SEG_SILENCE);
		break;
	case SEG_SILENCE:
		if (t->seg_sym >= (is_v27(t->mod) ? 32 : 48))
			tx_next_seg(t, is_v27(t->mod) ? SEG_REVERSALS : SEG_ABAB);
		break;
	case SEG_REVERSALS:
		if (t->seg_sym >= 50)
			tx_next_seg(t, SEG_TRN);
		break;
	case SEG_ABAB:
		if (t->seg_sym >= (is_v17(t->mod) ? 256 : 128))
			tx_next_seg(t, SEG_TRN);
		break;
	case SEG_TRN:
		if (t->seg_sym >= trn_len(t->mod, t->short_train))
			tx_next_seg(t, is_v17(t->mod) ? SEG_BRIDGE : SEG_ONES);
		break;
	case SEG_BRIDGE:
		if (t->seg_sym >= 64)
			tx_next_seg(t, SEG_ONES);
		break;
	case SEG_ONES:
		if (t->seg_sym >= ones_len(t->mod))
			tx_next_seg(t, SEG_DATA);
		break;
	case SEG_DATA:
		if (t->ending)
			tx_next_seg(t, SEG_SHUTDOWN);
		break;
	case SEG_SHUTDOWN:
		if (t->seg_sym >= 32)
			tx_next_seg(t, SEG_TAIL);
		break;
	case SEG_TAIL:
		if (t->seg_sym >= 48) {
			tx_next_seg(t, SEG_DONE);
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
			b1 = scr23(&t->scr, 1);
			b2 = scr23(&t->scr, 1);
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
		b1 = scr23(&t->scr, V17_BRIDGE_WORD >> shift);
		b2 = scr23(&t->scr, V17_BRIDGE_WORD >> (shift + 1));
		t->state = (t->state + dibit_to_step[b1 << 1 | b2]) & 3;
		z = v32_abcd[t->state];
		break;
	case SEG_ONES:
	case SEG_DATA:
	case SEG_SHUTDOWN:
		z = tx_data_symbol(t);
		break;
	default:
		break;
	}
	t->seg_sym++;
	return z;
}

void fax_tx_init(struct fax_tx *t, enum fax_mod mod, int short_train,
		 int srate, fax_get_bit_t get_bit, void *ctx)
{
	memset(t, 0, sizeof(*t));
	t->mod = mod;
	t->short_train = short_train;
	t->srate = srate;
	t->get_bit = get_bit;
	t->ctx = ctx;
	if (mod == FM_V21) {
		t->fsk.init(1650, 1850, fax_tx_rms(mod) * 1.41421f, srate);
		t->bit_inc = 300.0f / srate;
		t->fsk_bit = 1;
		t->seg = SEG_DATA;
		return;
	}
	t->qam.init(srate, mod_carrier(mod), mod_baud(mod),
		    mod_alpha(mod), 6, sqrtf(mod_power(mod)),
		    fax_tx_rms(mod) * 1.41421f);
	/* the receivers of the original library want the symbols at this
	   phase of the sample clock (as with V.32) */
	t->qam.t = is_v17(mod) ? 0.25f : 0.5f;
	t->qam.get_symbol = fax_tx_symbol;
	t->qam.ctx = t;
	if (is_v17(mod)) {
		t->scr = 0x2ECDD5;
		t->trellis.diff = short_train ? 0 : 1;
		t->seg = SEG_ABAB;
	} else if (is_v29(mod)) {
		t->tscr = 0x2A;
		t->seg = SEG_SILENCE;
	} else {
		t->scr = 0x3C;
		t->seg = SEG_CARRIER;
	}
}

float fax_tx_sample(struct fax_tx *t)
{
	if (t->done)
		return 0;
	if (t->mod == FM_V21) {
		t->bit_phase += t->bit_inc;
		if (t->bit_phase >= 1) {
			int b;
			t->bit_phase -= 1;
			b = t->get_bit(t->ctx);
			if (b == FAX_BIT_END) {
				t->done = 1;
				return 0;
			}
			t->fsk_bit = b & 1;
		}
		return t->fsk.next(t->fsk_bit);
	}
	return t->qam.sample();
}


/*
 *    receiver
 */

enum {
	RXS_SEARCH,
	RXS_TRAIN,
	RXS_DATA,
};

static int state_of(struct fax_rx *r, cplx z)
{
	if (is_v17(r->mod)) {
		int k, best = 0;
		float bd = 1e30f;
		for (k = 0; k < 4; k++) {
			float d = cplx_pow(cplx_sub(z, v32_abcd[k]));
			if (d < bd) {
				bd = d;
				best = k;
			}
		}
		return best;
	}
	return (int)lrintf(atan2f(z.im, z.re) / (float)(M_PI / 4)) & 7;
}

/* the absolute phase state of a training point */
static int point_state(struct fax_rx *r, cplx p)
{
	return state_of(r, p);
}

static cplx ref_point(struct fax_rx *r, int k)
{
	int s = r->ref[k];
	if (is_v17(r->mod))
		return v32_abcd[s];
	if (is_v29(r->mod)) {
		/* the C/D points of this rate */
		int off = v29_offset(r->mod);
		return s == point_state(r, v29_cdcd[off]) ?
			v29_cdcd[off] : v29_cdcd[off + 1];
	}
	return v27_points[s];
}

/* the training symbols as phase states, and the scrambler state at the
   start of the data path */
static int make_reference(struct fax_rx *r)
{
	int n = trn_len(r->mod, r->short_train), k, b1, b2, shift, state = 0;
	unsigned scr = 0, tscr = 0x2A;
	int count = 0;
	signed char *ref;

	if (is_v17(r->mod))
		n += 64;                /* the bridge is known as well */
	ref = r->ref = static_cast<signed char*>(malloc(n));
	if (!ref)
		return -1;
	r->ref_len = n;
	if (is_v17(r->mod)) {
		scr = 0x2ECDD5;
		for (k = 0; k < n; k++) {
			if (k < trn_len(r->mod, r->short_train)) {
				b1 = scr23(&scr, 1);
				b2 = scr23(&scr, 1);
				state = cdba_to_abcd[b1 << 1 | b2];
			} else {
				shift = ((k - trn_len(r->mod, r->short_train)) & 7) << 1;
				b1 = scr23(&scr, V17_BRIDGE_WORD >> shift);
				b2 = scr23(&scr, V17_BRIDGE_WORD >> (shift + 1));
				state = (state + dibit_to_step[b1 << 1 | b2]) & 3;
			}
			ref[k] = state;
		}
		r->scr = scr;
	} else if (is_v29(r->mod)) {
		int off = v29_offset(r->mod);
		for (k = 0; k < n; k++)
			ref[k] = point_state(r, v29_cdcd[off + v29_trn_bit(&tscr)]);
		r->scr = 0;
	} else {
		/* the regular reversals leave the state at 50 * 4 = 0 mod 8 */
		scr = 0x3C;
		for (k = 0; k < n; k++) {
			b1 = scr7(&scr, &count, 1);
			scr7(&scr, &count, 1);
			scr7(&scr, &count, 1);
			state = (state + (b1 << 2)) & 7;
			ref[k] = state;
		}
		r->scr = scr;
		r->scr_count = count;
	}
	return 0;
}

int fax_rx_init(struct fax_rx *r, enum fax_mod mod, int short_train,
		int srate, fax_put_bit_t put_bit, void *ctx,
		const struct fax_rx_memory *mem)
{
	memset(r, 0, sizeof(*r));
	r->mod = mod;
	r->short_train = short_train;
	r->srate = srate;
	r->put_bit = put_bit;
	r->ctx = ctx;
	if (mod == FM_V21) {
		r->fsk.init(1650, 1850, 300, srate);
		return 0;
	}
	if (r->qam.init(srate, mod_carrier(mod), mod_baud(mod),
			mod_alpha(mod), 6, 32, mod_power(mod)))
		return -1;
	r->det_alpha = 1.0f / (0.005f * srate);
	r->det_hold = srate / 25;       /* learn the noise floor first */
	r->det_floor = 1e12f;
	r->agc_alpha = r->qam.agc_alpha;
	r->alt_end = -1;
	r->nstates = is_v17(mod) ? 4 : 8;
	if (make_reference(r)) {
		r->qam.release();
		return -1;
	}
	if (is_v17(mod))
		r->vit.init(v17_points(mod), data_bits(mod));
	/* a short train only touches up what the long one learned */
	if (short_train && mem && mem->valid && is_v17(mem->mod)) {
		memcpy(r->mem_eq, mem->eq, sizeof(r->mem_eq));
		r->mem_gain = mem->agc_gain;
		r->use_mem = 1;
	}
	return 0;
}

void fax_rx_save(struct fax_rx *r, struct fax_rx_memory *mem)
{
	if (!is_v17(r->mod) || !r->have_good)
		return;
	mem->mod = r->mod;
	memcpy(mem->eq, r->good_eq, sizeof(mem->eq));
	mem->agc_gain = r->good_gain;
	mem->valid = 1;
}

void fax_rx_free(struct fax_rx *r)
{
	if (r->mod == FM_V21 || r->mod == FM_NONE)
		return;
	r->qam.release();
	free(r->ref);
	r->ref = NULL;
}

static void rx_bits_out(struct fax_rx *r, int q, int n, int msb_first)
{
	int k;
	for (k = 0; k < n; k++) {
		int bit = msb_first ? (q >> (n - 1 - k)) & 1 : (q >> k) & 1;
		bit = is_v27(r->mod) ? descr7(&r->scr, &r->scr_count, bit) :
			descr23(&r->scr, bit);
		if (r->trained)
			r->put_bit(r->ctx, bit);
	}
}

static void rx_data_symbol(struct fax_rx *r)
{
	cplx z = r->qam.z, d;
	int n = data_bits(r->mod), k, best, phase, dp, bits;
	float bd;

	if (is_v17(r->mod)) {
		const cplx *pts = v17_points(r->mod);
		int q;
		for (k = 0, best = 0, bd = 1e30f; k < (1 << (n + 1)); k++) {
			float e = cplx_pow(cplx_sub(z, pts[k]));
			if (e < bd) {
				bd = e;
				best = k;
			}
		}
		d = pts[best];
		q = r->vit.decode(z, NULL);
		r->data_sym++;
		if (q >= 0) {
			/* symbols leave the decoder VITERBI_DEPTH - 1 late */
			if (r->data_sym - (VITERBI_DEPTH - 1) > ones_len(r->mod))
				r->trained = 1;
			rx_bits_out(r, q, n, 0);
		}
	} else {
		const cplx *pts = is_v29(r->mod) ? v29_points : v27_points;
		int npts = r->mod == FM_V29_9600 ? 16 : 8;
		for (k = 0, best = 0, bd = 1e30f; k < npts; k++) {
			float e;
			if (r->mod == FM_V29_4800 && (k & 1))
				continue;
			e = cplx_pow(cplx_sub(z, pts[k]));
			if (e < bd) {
				bd = e;
				best = k;
			}
		}
		d = pts[best];
		phase = best & 7;
		dp = (phase - r->state) & 7;
		r->state = phase;
		if (++r->data_sym > ones_len(r->mod))
			r->trained = 1;
		if (n == 2 && r->mod != FM_V27_2400) {
			/* V.29 4800 */
			for (bits = 0; bits < 4 && steps_4[bits] != dp; bits++)
				;
			rx_bits_out(r, bits & 3, 2, 1);
		} else if (r->mod == FM_V27_2400) {
			for (bits = 0; bits < 4 && steps_4[bits] != dp; bits++)
				;
			rx_bits_out(r, bits & 3, 2, 1);
		} else {
			for (bits = 0; bits < 8 && steps_8[bits] != dp; bits++)
				;
			bits &= 7;
			if (r->mod == FM_V29_9600)
				rx_bits_out(r, (best >> 3) << 3 | bits, 4, 1);
			else
				rx_bits_out(r, bits, 3, 1);
		}
	}
	r->mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / mod_power(r->mod) - r->mse);
	r->zpow += 0.01f * (cplx_pow(z) - r->zpow);
	/* V.17: the original library's data constellation comes some 1.4 dB
	   below its training; decisions on the dense rates then miss and
	   the equalizer cannot pull the gain back.  Scrambled data has the
	   constellation's mean power: follow it. */
	/* slowly: the power of a few hundred data symbols wanders by some
	   per cent, and every step disturbs a good signal */
	if (r->data_sym == 150)
		r->zpow_slow = r->zpow;
	r->zpow_slow += 0.002f * (cplx_pow(z) - r->zpow_slow);
	if (is_v17(r->mod) && r->data_sym >= 600 && (r->data_sym & 255) == 0 &&
	    r->zpow_slow > r->mse * mod_power(r->mod)) {
		/* the signal's part of the power: less the noise around the
		   decisions */
		float f = sqrtf(mod_power(r->mod) /
				(r->zpow_slow - r->mse * mod_power(r->mod)));
		/* only a clear error */
		if (fabsf(f - 1) > 0.06f) {
			f = 1 + 0.5f * (f - 1);
			if (f > 1.1f)
				f = 1.1f;
			if (f < 0.9f)
				f = 0.9f;
			for (k = 0; k < r->qam.eq_len; k++)
				r->qam.eq[k] = cplx_scale(r->qam.eq[k], f);
			r->zpow *= f * f;
			r->zpow_slow *= f * f;
			DSP_DBG("fax rx: gain %.3f at symbol %ld\n", f,
				(long)r->data_sym);
		}
	}
	if (r->data_sym % 2400 == 0)
		DSP_DBG("fax rx: data mse %.5f (%.1f dB), power %.3f of the "
			"constellation's\n", r->mse, -10 * log10f(r->mse + 1e-9f),
			r->zpow / mod_power(r->mod));
	/* the timing loop slows down once it has surely settled (a short
	   train leaves it little time) */
	if (r->data_sym == 1000) {
		r->qam.kt_p = 0.005f * r->qam.sps;
		r->qam.kt_gi = 0.003f;
	}
	/* nothing to learn once the carrier is gone */
	if (r->carrier)
		/* the training of V.29 and V.27ter only covers one axis: the
		   equalizer still learns fast at the start of the data */
		r->qam.adapt(d, is_v17(r->mod) ? 0.005f :
			     r->data_sym < 100 ? 0.03f :
			     r->data_sym < 300 ? 0.015f : 0.005f, 0.02f, 0.0002f);
	/* keep a copy of a good equalizer for a later short train (the
	   end of the signal would spoil the live one) */
	r->mse_fast += 0.1f * (cplx_pow(cplx_sub(z, d)) / mod_power(r->mod) -
			       r->mse_fast);
	if (r->data_sym == 1)
		r->data_power = r->qam.power;
	if (is_v17(r->mod) && r->mse < 0.01f && r->mse_fast < 0.01f &&
	    r->qam.power > 0.5f * r->data_power && (r->data_sym & 127) == 0) {
		/* the newest copy may already see the end of the signal, the
		   saved one is the copy before it */
		memcpy(r->good_eq, r->new_eq, sizeof(r->good_eq));
		r->good_gain = r->new_gain;
		r->have_good = r->have_new;
		memcpy(r->new_eq, r->qam.eq, sizeof(r->new_eq));
		r->new_gain = r->qam.agc_gain;
		r->have_new = 1;
	}
}

/* how well the last 32 phase steps fit the reference ending at symbol k */
static int window_match(struct fax_rx *r, int k)
{
	int i, match = 0;
	for (i = 0; i < 32; i++)
		match += r->steps[i] == ((r->ref[k - 31 + i] - r->ref[k - 32 + i]) &
					 (r->nstates - 1));
	return match;
}

/* the V.29 and V.27ter training sequences repeat every 127 symbols: the
   end of the alternations / reversals before them tells which period is
   being received */
static void track_alternations(struct fax_rx *r, cplx z)
{
	float d1 = cplx_pow(cplx_sub(z, r->zh[0]));
	float d2 = cplx_pow(cplx_sub(z, r->zh[1]));
	if (d2 < 0.25f * d1) {
		r->alt_run++;
	} else {
		if (r->alt_run >= 16)
			r->alt_end = r->onset;
		r->alt_run = 0;
	}
	r->zh[1] = r->zh[0];
	r->zh[0] = z;
}

static void rx_symbol(struct fax_rx *r)
{
	cplx z = r->qam.z;
	int s, k, match;

	switch (r->stage) {
	case RXS_SEARCH:
		if (r->carrier && ++r->onset == 6)
			r->qam.timing_hold = 0;
		if (r->onset == 48)
			r->qam.agc_alpha = r->agc_alpha;
		/* on the alternations, the symbol samples are the strong
		   ones; when the mid samples are, the timing started half a
		   symbol off, where the timing loop is slow to leave */
		if (r->onset > 8 && r->onset <= 24) {
			r->sym_pow += cplx_pow(r->qam.y);
			r->mid_pow += cplx_pow(r->qam.mid);
		}
		if (r->onset == 24 && r->mid_pow > 1.5f * r->sym_pow) {
			r->qam.half ^= 1;
			DSP_DBG("fax rx: timing moved half a symbol\n");
		}
		track_alternations(r, z);
		s = state_of(r, z);
		memmove(r->steps, r->steps + 1, 31 * sizeof(int));
		r->steps[31] = (s - r->last_state) & (r->nstates - 1);
		r->last_state = s;
		if (r->nsteps < 32)
			r->nsteps++;
		/* hold the phase on the training grid */
		r->qam.adapt(is_v17(r->mod) ? v32_abcd[s] :
			     cplx_scale(v27_points[s], sqrtf(mod_power(r->mod) / 2)),
			     0, 0.05f, 0);
		if (r->nsteps < 32 || !r->carrier)
			break;
		{
			/* where in the training is this symbol? */
			int lo = 32, hi = 32, best = 32, m;
			if (r->alt_end >= 0) {
				lo = r->onset - r->alt_end - 4;
				hi = r->onset - r->alt_end + 8;
				if (lo < 32)
					lo = 32;
				if (hi > r->ref_len - 2)
					hi = r->ref_len - 2;
			}
			for (k = lo, match = -1; k <= hi; k++)
				if ((m = window_match(r, k)) > match) {
					match = m;
					best = k;
				}
			k = best;
		}
		if (match >= 29) {
			/* this symbol is reference symbol k */
			if (r->use_mem) {
				/* short train: continue with the equalizer of
				   the long train, timing has settled by now */
				memcpy(r->qam.eq, r->mem_eq, sizeof(r->qam.eq));
				r->qam.recompute();
				DSP_DBG("fax rx: equalizer of the long train\n");
			}
			r->qam.align_phase(ref_point(r, k));
			r->seg_sym = k + 1;
			r->stage = RXS_TRAIN;
			r->bridge_hits = 0;
			DSP_DBG("fax rx: training found at %d\n", k);
		}
		break;
	case RXS_TRAIN: {
		cplx d = ref_point(r, r->seg_sym);
		int trn = trn_len(r->mod, r->short_train);
		/* V.17 short train: the original library sends the 64 symbol
		   bridge as after a long train, spandsp goes on with the
		   scrambled ones (segment 4) at once.  Its first symbols tell;
		   without the bridge they are data, and a few of the 48 ones
		   are not missed. */
		if (is_v17(r->mod) && r->short_train &&
		    r->seg_sym >= trn && r->seg_sym < trn + 8) {
			if (cplx_pow(cplx_sub(z, d)) < 0.2f * cplx_pow(d))
				r->bridge_hits++;
			if (++r->seg_sym == trn + 8 && r->bridge_hits < 6) {
				DSP_DBG("fax rx: short train without the "
					"bridge\n");
				r->stage = RXS_DATA;
				r->data_sym = 8;
				r->vit.last_y12 = 0;
				DSP_DBG("fax rx: data\n");
			}
			break;
		}
		/* the original library's V.27ter training has stray 180
		   degree flips: follow them, its points are all on one axis */
		if (is_v27(r->mod) &&
		    cplx_pow(cplx_add(z, d)) < cplx_pow(cplx_sub(z, d)))
			d = cplx_scale(d, -1);
		r->qam.adapt(d, 0.02f, 0.05f, 0.0005f);
		if (++r->seg_sym >= r->ref_len) {
			r->stage = RXS_DATA;
			r->data_sym = 0;
			/* the data path continues from the last training
			   state (V.27ter, V.17 bridge) or from 0 (V.29) */
			if (is_v17(r->mod))
				r->vit.last_y12 = r->short_train ? 0 : 1;
			else if (is_v27(r->mod))
				r->state = state_of(r, d);
			else
				r->state = 0;
			DSP_DBG("fax rx: data\n");
		}
		break;
	}
	case RXS_DATA:
		rx_data_symbol(r);
		break;
	}
}

void fax_rx_sample(struct fax_rx *r, float x)
{
	if (r->mod == FM_V21) {
		int bit = r->fsk.run(x);
		r->carrier = r->fsk.carrier;
		r->trained = r->carrier;
		if (bit >= 0 && r->carrier)
			r->put_bit(r->ctx, bit);
		return;
	}
	if (r->qam.sample(x))
		rx_symbol(r);
	/* the 20 ms power of the demodulator would bridge the short gap
	   between V.21 and the training */
	r->det_pwr += r->det_alpha * (x * x - r->det_pwr);
	/* thresholds relative to the noise floor and the signal level */
	if (r->carrier)
		r->det_level += 0.001f * (r->det_pwr - r->det_level);
	else if (r->det_hold > 0) {
		/* the end of a signal dies away: its minimum so far */
		if (r->det_pwr < r->det_floor)
			r->det_floor = r->det_pwr;
	} else
		r->det_floor += 0.002f * (r->det_pwr - r->det_floor);
	if (r->det_hold > 0)
		r->det_hold--;
	if (!r->carrier && r->det_pwr > tone_power(150) &&
	    r->det_pwr > 4 * r->det_floor && r->det_hold < r->srate / 100) {
		r->carrier = 1;
		r->det_level = r->det_pwr;
		if (r->stage == RXS_SEARCH) {
			/* acquire the level quickly, and hold the timing
			   until it is known */
			r->qam.reset();
			r->qam.agc_pwr = 0;
			r->qam.agc_alpha = 1.0f / (0.003f * r->srate);
			r->qam.timing_hold = 1;
			r->onset = 0;
			r->alt_run = 0;
			r->alt_end = -1;
			r->sym_pow = r->mid_pow = 0;
		}
	} else if (r->carrier && (r->det_pwr < tone_power(100) ||
				  r->det_pwr < 0.06f * r->det_level)) {
		r->carrier = 0;
		/* the end of the signal dies away (V.27ter has only 20 ms
		   of silence before its training) */
		r->det_hold = r->srate / 50;
		r->det_floor = r->det_pwr;
		r->qam.agc_alpha = r->agc_alpha;
		r->qam.timing_hold = 0;
		if (r->stage != RXS_DATA) {
			r->stage = RXS_SEARCH;
			r->nsteps = 0;
		}
	}
	if (r->carrier)
		r->carrier_run = r->carrier_run > 0 ? r->carrier_run + 1 : 1;
	else
		r->carrier_run = r->carrier_run < 0 ? r->carrier_run - 1 : -1;
}

} // namespace dsp
