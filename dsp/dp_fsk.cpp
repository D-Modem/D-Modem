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
#include "pump.h"

namespace dsp {

/*
 *    FSK modulator
 */

/* mark m and space s (Hz), peak amplitude a, at rate samples/s */
void FskMod::init(float m, float s, float a, int rate)
{
	*this = FskMod();
	mark = m;
	space = s;
	amp = a;
	srate = rate;
}

/* the next sample of the mark (bit 1) or space (bit 0) tone, phase
   continuous */
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

/* mark and space (Hz), baud (bit/s): pre filter around the channel
   centre, base band low pass of shift / 2 + 0.6 baud, carrier on and off
   levels for 200 and 140 amplitude tones */
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

/* one sample x: updates carrier (on with level and a deviation near
   +-shift / 2) and last_dec (1: mark); returns the bit decided at the
   centre of a bit, or -1 */
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

struct FskPump : Pump {
	static constexpr const char *name = "FSK (V.21/V.23/Bell103) data pump";

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
	int after_v8;           /* calling after V.8: the answerer's JM may
				   still be in its channel */
	Goertzel g1270, g1070;  /* a Bell 103 caller's mark */
	float blk_energy;
	int bell_blocks;
	/* the caller's check that the mark is really at its frequency:
	   another answerer's (V.21's 1650 Hz for V.23's 1300) demodulates
	   as a mark too */
	Goertzel g_mark, g_lo, g_hi;
	int mark_true;

	void set_state(enum fsk_state state_);
	float tx_sample(int data);
	int start(struct modem *m, enum DP_ID id, int caller_, int srate_, int max_frag);
	int process(void *in, void *out, int cnt) override;
};

/* to state_, count (samples in the state) from 0 */
void FskPump::set_state(enum fsk_state state_)
{
	DSP_DBG("%s: state %d -> %d at %ld\n", par->name, state, state_, total);
	state = state_;
	count = 0;
}

/* V.21, V.23 or Bell 103 by id, the channels by caller_; timeouts from
   S7 and S10; the first state from the runtime (after V.8, after a Bell
   212A or automode fallback the answer tone or the mark is already
   there).  0, or -1 for another id. */
int FskPump::start(struct modem *m, enum DP_ID id, int caller_, int srate_, int max_frag)
{
	const struct fsk_params *par_;
	int s7, s10;

	switch (id) {
	case DP_V21:  par_ = v21_params; break;
	case DP_V23:  par_ = v23_params; break;
	case DP_B103: par_ = b103_params; break;
	default: return -1;
	}
	par_ += caller_ ? 0 : 1;

	modem = m;
	par = par_;
	srate = srate_;
	caller = caller_;

	tx.init(par_->tx_mark, par_->tx_space, DSP_TX_AMPLITUDE, srate_);
	rx.init(par_->rx_mark, par_->rx_space, par_->rx_baud, srate_);
	tx_inc = par_->tx_baud / srate_;
	tx_bit = 1;
	ans.set_freq(2100, srate_);
	g_mark.init(par_->rx_mark, srate_ / 100, srate_);
	g_lo.init(par_->rx_mark - 350, srate_ / 100, srate_);
	g_hi.init(par_->rx_mark + 350, srate_ / 100, srate_);

	s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	timeout = (long)(s7 > 0 ? s7 : 50) * srate_;
	carrier_loss = (long)(s10 > 0 ? s10 : 14) * srate_ / 10;

	if (caller_) {
		struct dp_runtime *rt = (struct dp_runtime *)
			modem_get_param(m, MDMPRM_DPRUNTIME);
		state = FSK_WAIT_CARRIER;
		if (rt && rt->after_v8) {
			/* after V.8's CJ the caller's carrier comes first
			   (the answerer waits for it) */
			rt->after_v8 = 0;
			after_v8 = 1;
			state = FSK_TRAIN;
		} else if (rt && rt->ans_sent && id == DP_B103) {
			/* Bell 212A fell back: the answerer's 2225 Hz mark
			   was heard, answer it at once */
			rt->ans_sent = 0;
			state = FSK_TRAIN;
		}
	} else {
		struct dp_runtime *rt = (struct dp_runtime *)
			modem_get_param(m, MDMPRM_DPRUNTIME);
		state = FSK_ANS_SILENCE;
		if (rt && rt->after_v8) {
			/* after V.8: our mark at once, the caller's
			   carrier comes */
			rt->after_v8 = rt->ans_sent = 0;
			state = FSK_TRAIN;
		} else if (rt && rt->ans_sent) {
			/* automode fallback: the answer tone was sent */
			rt->ans_sent = 0;
			state = FSK_ANS_GAP;
			automode = 1;
			g1270.init(1270, srate_ / 50, srate_);
			g1070.init(1070, srate_ / 50, srate_);
		}
	}
	DSP_DBG("%s: create %s, srate %d\n", par_->name,
		caller_ ? "caller" : "answerer", srate_);
	return 0;
}

/* the next transmit sample; at each bit boundary a new bit from the
   link layer when data, else a mark */
float FskPump::tx_sample(int data)
{
	tx_phase += tx_inc;
	if (tx_phase >= 1) {
		tx_phase -= 1;
		if (data) {
			u8 b = 1;
			modem_get_bits(modem, 1, &b, 1);
			tx_bit = b & 1;
		} else
			tx_bit = 1;
	}
	return tx.next(tx_bit);
}

/* cnt samples: the receiver and the detectors, then the handshake
   (answerer: 1 s silence, 3.3 s of 2100 Hz, 75 ms gap; both: the mark
   until the remote's carrier or mark is steady) and data.  Returns
   DPSTAT_CONNECT once, DPSTAT_CHANGEDP for V.8 or an automode fallback,
   DPSTAT_ERROR on a timeout (S7) or the loss of carrier (S10). */
int FskPump::process(void *in, void *out, int cnt)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int ret = DPSTAT_OK;
	int i, bit;

	/* silence where an early return (another pump, an error) leaves
	   the block unwritten */
	memset(dst, 0, cnt * sizeof(*dst));
	/* V.21 only: after V.8 the PCTel modem takes V.23's 75 bit/s
	   side as the answerer, the HSF modem the 1200 bit/s one, and both
	   answer V.22, V.22bis and V.23 without V.8 */
	if (caller && !total && state == FSK_WAIT_CARRIER &&
	    dp.id == DP_V21 && dp_v8_first(modem, dp.id))
		return DPSTAT_CHANGEDP;
	for (i = 0; i < cnt; i++) {
		float y = 0;

		bit = rx.run(src[i]);
		if (rx.carrier)
			carrier_count = carrier_count > 0 ? carrier_count + 1 : 1;
		else
			carrier_count = carrier_count < 0 ? carrier_count - 1 : -1;

		if (automode && !caller) {
			blk_energy += (float)src[i] * src[i];
			g1070.run(src[i]);
			if (g1270.run(src[i])) {
				float mean = blk_energy / g1270.len;
				/* mark and space: Bell 103's originate band */
				bell_blocks = g1270.power + g1070.power > 0.6f * mean &&
					mean > tone_power(150) ?
					bell_blocks + 1 : 0;
				blk_energy = 0;
			}
		}
		g_lo.run(src[i]);
		g_hi.run(src[i]);
		if (g_mark.run(src[i]))
			mark_true = g_mark.power > 4 * g_lo.power &&
				g_mark.power > 4 * g_hi.power;
		if (rx.carrier && rx.last_dec &&
		    (!caller || state != FSK_WAIT_CARRIER || mark_true))
			mark_count++;
		else
			mark_count = 0;

		switch (state) {
		case FSK_ANS_SILENCE:
			if (count >= ms2samples(1000, srate)) {
				/* Bell 103 uses its 2225 Hz mark as answer tone */
				set_state(dp.id == DP_B103 ?
					      FSK_TRAIN : FSK_ANS_TONE);
			}
			break;
		case FSK_ANS_TONE:
			y = DSP_TX_AMPLITUDE * ans.next();
			if (count >= ms2samples(3300, srate))
				set_state(FSK_ANS_GAP);
			break;
		case FSK_ANS_GAP:
			if (count >= ms2samples(75, srate))
				set_state(FSK_TRAIN);
			break;
		case FSK_WAIT_CARRIER:
			/* caller: silent until the answerer's mark is heard;
			   for V.23 the 2100 Hz answer tone is a valid space
			   tone, so only a steady mark counts */
			if (mark_count >= ms2samples(150, srate))
				set_state(FSK_TRAIN);
			break;
		case FSK_TRAIN:
			y = tx_sample(0);
			/* the caller already saw the answerer's steady mark,
			   which may be data by now; the answerer waits for
			   the caller's mark */
			/* (a Bell 103 caller found in automode may be online
			   already, sending data: its carrier will do) */
			/* (after V.8 the caller too waits for the mark: the
			   answerer's JM, in the same channel, is carrier;
			   and 400 ms of it, the answerer starting its own as
			   it starts listening to ours, which it needs 150
			   ms of) */
			if (count >= ms2samples(200, srate) &&
			    ((caller && !after_v8) ||
			     (automode && dp.id == DP_B103) ?
			     carrier_count : mark_count) >=
			    ms2samples(after_v8 ? 400 : 150, srate)) {
				set_state(FSK_DATA);
				modem_set_param(modem, MDMPRM_TX_RATE, (int)par->tx_baud);
				modem_set_param(modem, MDMPRM_RX_RATE, (int)par->rx_baud);
				DSP_DBG("%s: connect %d/%d\n", par->name,
					(int)par->tx_baud, (int)par->rx_baud);
				ret = DPSTAT_CONNECT;
			} else if (!caller && automode &&
				   dp.id != DP_B103 &&
				   (bell_blocks >= 5 ||
				    (count >= ms2samples(3000, srate) &&
				     !rx.carrier))) {
				/* a Bell 103 caller's mark; or no caller:
				   after V.21 a V.23 one waits for our 1300 Hz
				   mark, after V.23 a Bell 103 one for 2225 Hz */
				struct dp_runtime *rt = (struct dp_runtime *)
					modem_get_param(modem,
							MDMPRM_DPRUNTIME);
				enum DP_ID next = bell_blocks >= 5 ||
					dp.id == DP_V23 ? DP_B103 : DP_V23;
				DSP_DBG("%s: %s, falling back to %s\n",
					par->name, bell_blocks >= 5 ?
					"Bell 103 caller" : "no answer",
					next == DP_B103 ? "Bell 103" : "V.23");
				if (rt)
					rt->ans_sent = 1;
				modem_set_param(modem, MDMPRM_DP_REQUESTED,
						next);
				return DPSTAT_CHANGEDP;
			}
			break;
		case FSK_DATA:
			/* the answerer keeps its mark for a second: the
			   caller links up only 0.8 s after it starts its own
			   carrier and would miss the first characters */
			y = tx_sample(caller ||
					  count >= ms2samples(1000, srate));
			if (bit >= 0 && rx.carrier) {
				u8 b = bit;
				modem_put_bits(modem, 1, &b, 1);
			}
			if (-carrier_count >= carrier_loss) {
				DSP_DBG("%s: carrier lost\n", par->name);
				return DPSTAT_ERROR;
			}
			break;
		}
		if (state != FSK_DATA && total >= timeout) {
			DSP_DBG("%s: handshake timeout\n", par->name);
			return DPSTAT_ERROR;
		}
		dst[i] = dsp_clip16(y);
		count++;
		total++;
	}
	return ret;
}

/* registers the pump as DP_V21, DP_V23 and DP_B103; 0 */
int dp_fsk_init(void)
{
	modem_dp_register(DP_V21, &PumpOps<FskPump>::ops);
	modem_dp_register(DP_V23, &PumpOps<FskPump>::ops);
	modem_dp_register(DP_B103, &PumpOps<FskPump>::ops);
	return 0;
}

/* deregisters them */
void dp_fsk_exit(void)
{
	modem_dp_deregister(DP_V21, &PumpOps<FskPump>::ops);
	modem_dp_deregister(DP_V23, &PumpOps<FskPump>::ops);
	modem_dp_deregister(DP_B103, &PumpOps<FskPump>::ops);
}

} // namespace dsp
