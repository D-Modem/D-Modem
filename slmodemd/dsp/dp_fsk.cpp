/*
 *
 *    dp_fsk.cpp - FSK data pumps: ITU V.21 (300 bit/s), ITU V.23
 *               (1200/75 bit/s) and Bell 103 (300 bit/s)
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

namespace dsp {


/*
 *    FSK modulator
 */

void FskMod::init(float m, float s, float a, int rate)
{
	*this = FskMod();
	mark = m;
	space = s;
	amp = a;
	srate = rate;
}

float FskMod::next(int bit)
{
	osc.set_freq(bit ? mark : space, srate);
	return amp * osc.next();
}


/*
 *    FSK demodulator: band pass, complex down conversion to the centre
 *    of the channel, low pass, phase difference discriminator and a
 *    simple bit clock DPLL that locks to the data transitions.
 */

void FskDemod::init(float mark, float space, float baud, int rate)
{
	float fc = (mark + space) / 2;
	float shift = fabsf(mark - space);
	float bw = shift / 2 + baud * 0.6f;
	int i;

	*this = FskDemod();
	srate = rate;
	sign = mark > space ? 1 : -1;
	half_shift = shift / 2;
	pre.bandpass(fc, fc / (2 * bw + 1), srate);
	lo.set_freq(-fc, srate);
	for (i = 0; i < 2; i++) {
		lp_re[i].lowpass(bw, 0.707f, srate);
		lp_im[i].lowpass(bw, 0.707f, srate);
	}
	pwr.init(8.0f / baud > 0.02f ? 8.0f / baud : 0.02f, srate);
	dev_alpha = 1.0f / (0.02f * srate);
	/* base band power of a tone of amplitude A is A^2/4 */
	on_level = 200.0f * 200.0f / 4;
	off_level = 140.0f * 140.0f / 4;
	bit_inc = baud / srate;
	bit_gain = 0.25f;
	dev_avg = half_shift * 4;
}

int FskDemod::run(float x)
{
	cplx z, d;
	float dev, p;
	int dec, bit = -1, i;

	x = pre.run(x);
	z = cplx_scale(lo.next_cplx(), x);
	for (i = 0; i < 2; i++) {
		z.re = lp_re[i].run(z.re);
		z.im = lp_im[i].run(z.im);
	}

	/* instantaneous frequency relative to the channel centre */
	d = cplx_mulc(z, prev);
	prev = z;
	dev = atan2f(d.im, d.re) * srate / (2 * (float)M_PI);

	/* carrier detector: level plus a sane frequency deviation, so that
	   e.g. a 2100 Hz answer tone is not taken for a V.21 carrier */
	p = pwr.run(sqrtf(cplx_pow(z)));
	dev_avg += dev_alpha * (fabsf(fabsf(dev) - half_shift) - dev_avg);
	if (!carrier) {
		if (p > on_level && dev_avg < half_shift * 0.75f + 40)
			carrier = 1;
	} else if (p < off_level || dev_avg > half_shift * 1.5f + 80)
		carrier = 0;

	dec = dev * sign > 0;

	/* bit clock: transitions should happen at phase 0 */
	if (dec != last_dec) {
		if (bit_phase < 0.5f)
			bit_phase -= bit_gain * bit_phase;
		else
			bit_phase += bit_gain * (1 - bit_phase);
		last_dec = dec;
	}
	p = bit_phase;
	bit_phase += bit_inc;
	if (p < 0.5f && bit_phase >= 0.5f)
		bit = dec;
	if (bit_phase >= 1)
		bit_phase -= 1;
	return bit;
}


/*
 *    FSK data pump
 */

enum fsk_state {
	FSK_ANS_SILENCE,
	FSK_ANS_TONE,
	FSK_ANS_GAP,
	FSK_WAIT_CARRIER,
	FSK_TRAIN,
	FSK_DATA,
};

struct fsk_params {
	const char *name;
	float tx_mark, tx_space, tx_baud;
	float rx_mark, rx_space, rx_baud;
};

/* index 0: caller, 1: answerer */
static const struct fsk_params v21_params[2] = {
	{ "V21", 980, 1180, 300, 1650, 1850, 300 },
	{ "V21", 1650, 1850, 300, 980, 1180, 300 },
};
static const struct fsk_params b103_params[2] = {
	{ "Bell103", 1270, 1070, 300, 2225, 2025, 300 },
	{ "Bell103", 2225, 2025, 300, 1270, 1070, 300 },
};
/* V.23: the answering side sends the 1200 bit/s forward channel */
static const struct fsk_params v23_params[2] = {
	{ "V23", 390, 450, 75, 1300, 2100, 1200 },
	{ "V23", 1300, 2100, 1200, 390, 450, 75 },
};

struct fsk_dp {
	struct dp dp;
	struct modem *modem;
	const struct fsk_params *par;
	int srate;
	int caller;
	enum fsk_state state;
	long count;             /* samples spent in the current state */
	long total;             /* samples since creation */
	long carrier_count;     /* consecutive samples with/without carrier */
	long mark_count;        /* consecutive samples of steady mark */
	long timeout;
	long carrier_loss;
	Osc ans;         /* answer tone */
	FskMod tx;
	FskDemod rx;
	float tx_phase, tx_inc;
	int tx_bit;
	int automode;           /* answering after V.22bis: V.23, Bell 103
				   next */
	Goertzel g1270, g1070;  /* a Bell 103 caller's mark */
	float blk_energy;
	int bell_blocks;
	/* the caller's check that the mark is really at its frequency:
	   another answerer's (V.21's 1650 Hz for V.23's 1300) demodulates
	   as a mark too */
	Goertzel g_mark, g_lo, g_hi;
	int mark_true;
};

static void fsk_set_state(struct fsk_dp *f, enum fsk_state state)
{
	DSP_DBG("%s: state %d -> %d at %ld\n", f->par->name, f->state, state, f->total);
	f->state = state;
	f->count = 0;
}

static struct dp *fsk_create(struct modem *m, enum DP_ID id,
			     int caller, int srate, int max_frag,
			     struct dp_operations *op)
{
	struct fsk_dp *f;
	const struct fsk_params *par;
	int s7, s10;

	switch (id) {
	case DP_V21:  par = v21_params; break;
	case DP_V23:  par = v23_params; break;
	case DP_B103: par = b103_params; break;
	default: return NULL;
	}
	par += caller ? 0 : 1;

	f = static_cast<fsk_dp*>(calloc(1, sizeof(*f)));
	if (!f)
		return NULL;
	f->dp.id = id;
	f->dp.modem = m;
	f->dp.op = op;
	f->dp.dp_data = f;
	f->modem = m;
	f->par = par;
	f->srate = srate;
	f->caller = caller;

	f->tx.init(par->tx_mark, par->tx_space, DSP_TX_AMPLITUDE, srate);
	f->rx.init(par->rx_mark, par->rx_space, par->rx_baud, srate);
	f->tx_inc = par->tx_baud / srate;
	f->tx_bit = 1;
	f->ans.set_freq(2100, srate);
	f->g_mark.init(par->rx_mark, srate / 100, srate);
	f->g_lo.init(par->rx_mark - 350, srate / 100, srate);
	f->g_hi.init(par->rx_mark + 350, srate / 100, srate);

	s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	f->timeout = (long)(s7 > 0 ? s7 : 50) * srate;
	f->carrier_loss = (long)(s10 > 0 ? s10 : 14) * srate / 10;

	if (caller) {
		struct dp_runtime *rt = (struct dp_runtime *)
			modem_get_param(m, MDMPRM_DPRUNTIME);
		f->state = FSK_WAIT_CARRIER;
		if (rt && rt->ans_sent && id == DP_B103) {
			/* Bell 212A fell back: the answerer's 2225 Hz mark
			   was heard, answer it at once */
			rt->ans_sent = 0;
			f->state = FSK_TRAIN;
		}
	} else {
		struct dp_runtime *rt = (struct dp_runtime *)
			modem_get_param(m, MDMPRM_DPRUNTIME);
		f->state = FSK_ANS_SILENCE;
		if (rt && rt->ans_sent) {
			/* automode fallback: the answer tone was sent */
			rt->ans_sent = 0;
			f->state = FSK_ANS_GAP;
			f->automode = 1;
			f->g1270.init(1270, srate / 50, srate);
			f->g1070.init(1070, srate / 50, srate);
		}
	}
	DSP_DBG("%s: create %s, srate %d\n", par->name,
		caller ? "caller" : "answerer", srate);
	return &f->dp;
}

static int fsk_delete(struct dp *dp)
{
	free(dp->dp_data);
	return 0;
}

static int fsk_hangup(struct dp *dp)
{
	return 0;
}

static float fsk_tx_sample(struct fsk_dp *f, int data)
{
	f->tx_phase += f->tx_inc;
	if (f->tx_phase >= 1) {
		f->tx_phase -= 1;
		if (data) {
			u8 b = 1;
			modem_get_bits(f->modem, 1, &b, 1);
			f->tx_bit = b & 1;
		} else
			f->tx_bit = 1;
	}
	return f->tx.next(f->tx_bit);
}

static int fsk_process(struct dp *dp, void *in, void *out, int cnt)
{
	struct fsk_dp *f = static_cast<fsk_dp*>(dp->dp_data);
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int ret = DPSTAT_OK;
	int i, bit;

	for (i = 0; i < cnt; i++) {
		float y = 0;

		bit = f->rx.run(src[i]);
		if (f->rx.carrier)
			f->carrier_count = f->carrier_count > 0 ? f->carrier_count + 1 : 1;
		else
			f->carrier_count = f->carrier_count < 0 ? f->carrier_count - 1 : -1;

		if (f->automode && !f->caller) {
			f->blk_energy += (float)src[i] * src[i];
			f->g1070.run(src[i]);
			if (f->g1270.run(src[i])) {
				float mean = f->blk_energy / f->g1270.len;
				/* mark and space: Bell 103's originate band */
				f->bell_blocks = f->g1270.power + f->g1070.power > 0.6f * mean &&
					mean > tone_power(150) ?
					f->bell_blocks + 1 : 0;
				f->blk_energy = 0;
			}
		}
		f->g_lo.run(src[i]);
		f->g_hi.run(src[i]);
		if (f->g_mark.run(src[i]))
			f->mark_true = f->g_mark.power > 4 * f->g_lo.power &&
				f->g_mark.power > 4 * f->g_hi.power;
		if (f->rx.carrier && f->rx.last_dec &&
		    (!f->caller || f->state != FSK_WAIT_CARRIER || f->mark_true))
			f->mark_count++;
		else
			f->mark_count = 0;

		switch (f->state) {
		case FSK_ANS_SILENCE:
			if (f->count >= ms2samples(1000, f->srate)) {
				/* Bell 103 uses its 2225 Hz mark as answer tone */
				fsk_set_state(f, f->dp.id == DP_B103 ?
					      FSK_TRAIN : FSK_ANS_TONE);
			}
			break;
		case FSK_ANS_TONE:
			y = DSP_TX_AMPLITUDE * f->ans.next();
			if (f->count >= ms2samples(3300, f->srate))
				fsk_set_state(f, FSK_ANS_GAP);
			break;
		case FSK_ANS_GAP:
			if (f->count >= ms2samples(75, f->srate))
				fsk_set_state(f, FSK_TRAIN);
			break;
		case FSK_WAIT_CARRIER:
			/* caller: silent until the answerer's mark is heard;
			   for V.23 the 2100 Hz answer tone is a valid space
			   tone, so only a steady mark counts */
			if (f->mark_count >= ms2samples(150, f->srate))
				fsk_set_state(f, FSK_TRAIN);
			break;
		case FSK_TRAIN:
			y = fsk_tx_sample(f, 0);
			/* the caller already saw the answerer's steady mark,
			   which may be data by now; the answerer waits for
			   the caller's mark */
			/* (a Bell 103 caller found in automode may be online
			   already, sending data: its carrier will do) */
			if (f->count >= ms2samples(200, f->srate) &&
			    (f->caller || (f->automode && f->dp.id == DP_B103) ?
			     f->carrier_count : f->mark_count) >=
			    ms2samples(150, f->srate)) {
				fsk_set_state(f, FSK_DATA);
				modem_set_param(f->modem, MDMPRM_TX_RATE, (int)f->par->tx_baud);
				modem_set_param(f->modem, MDMPRM_RX_RATE, (int)f->par->rx_baud);
				DSP_DBG("%s: connect %d/%d\n", f->par->name,
					(int)f->par->tx_baud, (int)f->par->rx_baud);
				ret = DPSTAT_CONNECT;
			} else if (!f->caller && f->automode &&
				   f->dp.id != DP_B103 &&
				   (f->bell_blocks >= 5 ||
				    (f->count >= ms2samples(3000, f->srate) &&
				     !f->rx.carrier))) {
				/* a Bell 103 caller's mark; or no caller:
				   after V.21 a V.23 one waits for our 1300 Hz
				   mark, after V.23 a Bell 103 one for 2225 Hz */
				struct dp_runtime *rt = (struct dp_runtime *)
					modem_get_param(f->modem,
							MDMPRM_DPRUNTIME);
				enum DP_ID next = f->bell_blocks >= 5 ||
					f->dp.id == DP_V23 ? DP_B103 : DP_V23;
				DSP_DBG("%s: %s, falling back to %s\n",
					f->par->name, f->bell_blocks >= 5 ?
					"Bell 103 caller" : "no answer",
					next == DP_B103 ? "Bell 103" : "V.23");
				if (rt)
					rt->ans_sent = 1;
				modem_set_param(f->modem, MDMPRM_DP_REQUESTED,
						next);
				return DPSTAT_CHANGEDP;
			}
			break;
		case FSK_DATA:
			/* the answerer keeps its mark for a second: the
			   caller links up only 0.8 s after it starts its own
			   carrier and would miss the first characters */
			y = fsk_tx_sample(f, f->caller ||
					  f->count >= ms2samples(1000, f->srate));
			if (bit >= 0 && f->rx.carrier) {
				u8 b = bit;
				modem_put_bits(f->modem, 1, &b, 1);
			}
			if (-f->carrier_count >= f->carrier_loss) {
				DSP_DBG("%s: carrier lost\n", f->par->name);
				return DPSTAT_ERROR;
			}
			break;
		}
		if (f->state != FSK_DATA && f->total >= f->timeout) {
			DSP_DBG("%s: handshake timeout\n", f->par->name);
			return DPSTAT_ERROR;
		}
		dst[i] = dsp_clip16(y);
		f->count++;
		f->total++;
	}
	return ret;
}


static struct dp_operations fsk_operations = {
	.name = "FSK (V.21/V.23/Bell103) data pump",
	.create = fsk_create,
	.destroy = fsk_delete,
	.process = fsk_process,
	.hangup = fsk_hangup,
};

int dp_fsk_init(void)
{
	modem_dp_register(DP_V21, &fsk_operations);
	modem_dp_register(DP_V23, &fsk_operations);
	modem_dp_register(DP_B103, &fsk_operations);
	return 0;
}

void dp_fsk_exit(void)
{
	modem_dp_deregister(DP_V21, &fsk_operations);
	modem_dp_deregister(DP_V23, &fsk_operations);
	modem_dp_deregister(DP_B103, &fsk_operations);
}

} // namespace dsp
