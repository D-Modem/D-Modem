/*
 *
 *    dp_callprog.cpp - call progress data pump (DP_CALLPROG)
 *
 *    Runs on the calling side between going off hook and the start of
 *    the modulation handshake: optional dial tone detection, DTMF
 *    dialing, the V.25 calling tone, busy tone detection and answer
 *    tone detection.  When an answer tone is found it returns
 *    DPSTAT_CHANGEDP and the modem core switches to the configured
 *    data pump.
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

namespace dsp {

#define BLOCK_MS 10

enum cp_state {
	CP_DIALTONE,
	CP_DIAL,
	CP_WAIT_ANSWER,
	CP_DONE,
};

struct cp_tone {
	Goertzel g;
	int blocks;             /* consecutive blocks where the tone is on */
};

struct callprog_dp {
	struct dp dp;
	struct modem *modem;
	struct dp_runtime *rt;
	int srate;
	enum cp_state state;
	long count, total;
	long timeout;

	/* dialing */
	const char *dial;
	int dial_pos;
	long dial_on, dial_off;
	long digit_left, pause_left;
	int digit_on;
	DtmfGen dtmf;

	/* tone detectors on BLOCK_MS blocks */
	int block_len, block_n;
	float block_energy;
	struct cp_tone dialtone[3];     /* 350, 440, 425 Hz */
	struct cp_tone ans2100, ans2225, usb1;
	/* 2225 Hz (Bell) and 2250 Hz (V.22 unscrambled ones) are too
	   close for the blocks: 200 ms ones tell them apart */
	Goertzel fine2225, fine2250;
	int need_dialtone;
	int dialtone_blocks;

	/* busy tone cadence detector */
	Biquad cpt_bp;
	float cpt_energy;
	int cpt_on;
	int cpt_run;            /* blocks in the current on/off run */
	int busy_cycles;

	/* V.25 calling tone, or the T.30 CNG of a fax call */
	int calling_tone;
	int fax;
	Osc ct;
};

static void cp_set_state(struct callprog_dp *c, enum cp_state state)
{
	DSP_DBG("callprog: state %d -> %d at %ld\n", c->state, state, c->total);
	c->state = state;
	c->count = 0;
}

static struct dp *callprog_create(struct modem *m, enum DP_ID id,
				  int caller, int srate, int max_frag,
				  struct dp_operations *op)
{
	struct callprog_dp *c;
	const char *dial;
	int i, v;
	static const float dt_freq[3] = { 350, 440, 425 };

	c = static_cast<callprog_dp*>(calloc(1, sizeof(*c)));
	if (!c)
		return NULL;
	c->dp.id = id;
	c->dp.modem = m;
	c->dp.op = op;
	c->dp.dp_data = c;
	c->modem = m;
	c->srate = srate;
	c->rt = (struct dp_runtime *)modem_get_param(m, MDMPRM_DPRUNTIME);
	if (c->rt) {
		c->rt->answer_tone = ANSTONE_NONE;
		c->rt->answer_tone_end = 0;
	}

	c->block_len = srate * BLOCK_MS / 1000;
	for (i = 0; i < 3; i++)
		c->dialtone[i].g.init(dt_freq[i], c->block_len, srate);
	c->ans2100.g.init(2100, c->block_len, srate);
	c->ans2225.g.init(2225, c->block_len, srate);
	c->usb1.g.init(2250, c->block_len, srate);
	c->fine2225.init(2225, srate / 5, srate);
	c->fine2250.init(2250, srate / 5, srate);
	c->cpt_bp.bandpass(480, 1.2f, srate);

	v = modem_get_sreg(m, SREG_DTMF_DURATION);
	c->dial_on = ms2samples(v > 40 ? v : 95, srate);
	c->dial_off = c->dial_on;

	dial = (const char *)modem_get_param(m, MDMPRM_DIALSTR);
	c->dial = dial ? dial : "";
	/* X2 and X4 wait for a dial tone, the others dial blindly */
	v = modem_get_sreg(m, SREG_X_CODE);
	c->need_dialtone = (v == 2 || v == 4);

	v = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	c->timeout = (long)(v > 0 ? v : 50) * srate;

	c->calling_tone = modem_get_param(m, GetCallingToneFlag) > 0;
	c->fax = m->mode == MODEM_MODE_FAX;
	c->ct.set_freq(c->fax ? 1100 : 1300, srate);

	/* like the original, X2/X4 insist on a dial tone even when there
	   is nothing to dial (D-Modem users run ATX3 for SIP lines) */
	if (*c->dial || c->need_dialtone)
		c->state = CP_DIALTONE;
	else
		c->state = CP_WAIT_ANSWER;
	DSP_DBG("callprog: create, dial '%s', srate %d\n", c->dial, srate);
	return &c->dp;
}

static int callprog_delete(struct dp *dp)
{
	free(dp->dp_data);
	return 0;
}

/* one tone is "on" when it dominates the block energy */
static int cp_tone_block(struct cp_tone *t, float block_power, float min_power)
{
	if (t->g.power > min_power && t->g.power > 0.5f * block_power)
		t->blocks++;
	else
		t->blocks = 0;
	return t->blocks;
}

/* process one complete detector block, returns a DPSTAT_ code */
static int cp_block(struct callprog_dp *c)
{
	float bp = c->block_energy / c->block_len;
	float min = tone_power(150);    /* about -43 dBm0 */
	float dt;
	int i, on;

	c->block_energy = 0;

	/* dial tone: any of the usual frequencies (or the 350+440 pair) */
	dt = 0;
	for (i = 0; i < 3; i++)
		dt += c->dialtone[i].g.power;
	if (dt > 0.6f * bp && bp > min)
		c->dialtone_blocks++;
	else
		c->dialtone_blocks = 0;

	if (c->state != CP_WAIT_ANSWER)
		return DPSTAT_OK;

	/* answer tones: 300 ms of 2100 Hz, 2225 Hz or V.22 unscrambled ones */
	if (cp_tone_block(&c->ans2100, bp, min) >= 300 / BLOCK_MS ||
	    cp_tone_block(&c->ans2225, bp, min) >= 300 / BLOCK_MS ||
	    cp_tone_block(&c->usb1, bp, min) >= 300 / BLOCK_MS) {
		int tone = c->ans2225.blocks >= 300 / BLOCK_MS &&
			c->fine2225.power > c->fine2250.power ? ANSTONE_2225 :
			ANSTONE_2100;
		DSP_DBG("callprog: answer tone %s detected\n",
			tone == ANSTONE_2225 ? "2225" : "2100");
		/* the runtime comes with modem_start(), after we were
		   created for a dial */
		if (!c->rt)
			c->rt = (struct dp_runtime *)
				modem_get_param(c->modem, MDMPRM_DPRUNTIME);
		if (c->rt) {
			c->rt->answer_tone = tone;
			c->rt->answer_tone_end = c->total;
		}
		cp_set_state(c, CP_DONE);
		return DPSTAT_CHANGEDP;
	}

	/* busy / congestion: 4 cycles of 0.2..0.8 s on and off */
	on = c->cpt_energy / c->block_len > min &&
		c->cpt_energy > 0.5f * c->block_len * bp;
	c->cpt_energy = 0;
	if (on == c->cpt_on) {
		c->cpt_run++;
	} else {
		int len = c->cpt_run * BLOCK_MS;
		if (len >= 200 && len <= 800)
			c->busy_cycles += c->cpt_on ? 1 : 0;
		else
			c->busy_cycles = 0;
		c->cpt_on = on;
		c->cpt_run = 1;
		if (c->busy_cycles >= 4) {
			DSP_DBG("callprog: busy\n");
			return DPSTAT_BUSY;
		}
	}
	return DPSTAT_OK;
}

static float cp_dial_sample(struct callprog_dp *c)
{
	char ch;

	for (;;) {
		if (c->pause_left > 0) {
			c->pause_left--;
			return 0;
		}
		if (c->digit_left > 0) {
			c->digit_left--;
			if (c->digit_on) {
				if (c->digit_left == 0) {
					c->digit_on = 0;
					c->digit_left = c->dial_off;
				}
				return c->dtmf.next();
			}
			return 0;
		}
		ch = c->dial[c->dial_pos];
		if (!ch) {
			cp_set_state(c, CP_WAIT_ANSWER);
			return 0;
		}
		c->dial_pos++;
		if (ch == ',') {
			int s8 = modem_get_sreg(c->modem, SREG_DIAL_PAUSE_TIME);
			c->pause_left = (long)(s8 > 0 ? s8 : 2) * c->srate;
		} else if (ch == 'W' || ch == 'w') {
			c->pause_left = 2L * c->srate;
		} else if (!c->dtmf.set(ch, c->srate)) {
			c->digit_on = 1;
			c->digit_left = c->dial_on;
		}
		/* other modifiers (T, P, spaces, ...) are ignored */
	}
}

static int callprog_process(struct dp *dp, void *in, void *out, int cnt)
{
	struct callprog_dp *c = static_cast<callprog_dp*>(dp->dp_data);
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int ret = DPSTAT_OK;
	int i;

	for (i = 0; i < cnt; i++) {
		float x = src[i], y = 0, b;
		int k;

		for (k = 0; k < 3; k++)
			c->dialtone[k].g.run(x);
		c->ans2100.g.run(x);
		c->ans2225.g.run(x);
		c->usb1.g.run(x);
		c->fine2225.run(x);
		c->fine2250.run(x);
		b = c->cpt_bp.run(x);
		c->cpt_energy += b * b;
		c->block_energy += x * x;
		if (++c->block_n >= c->block_len) {
			c->block_n = 0;
			k = cp_block(c);
			if (k != DPSTAT_OK && ret == DPSTAT_OK)
				ret = k;
		}

		switch (c->state) {
		case CP_DIALTONE:
			if (c->dialtone_blocks * BLOCK_MS >= 1000 ||
			    (!c->need_dialtone &&
			     c->count >= ms2samples(2000, c->srate)))
				cp_set_state(c, CP_DIAL);
			else if (c->count >= (long)modem_get_sreg(c->modem,
						SREG_DIAL_TONE_WAIT_TIME) * c->srate &&
				 c->count >= ms2samples(2000, c->srate)) {
				DSP_DBG("callprog: no dial tone\n");
				if (ret == DPSTAT_OK)
					ret = DPSTAT_NODIALTONE;
			}
			break;
		case CP_DIAL:
			y = cp_dial_sample(c);
			break;
		case CP_WAIT_ANSWER:
			if (c->fax) {
				/* CNG: 0.5 s of 1100 Hz every 3.5 s */
				if (c->count % ms2samples(3500, c->srate) <
				    ms2samples(500, c->srate))
					y = 0.5f * DSP_TX_AMPLITUDE * c->ct.next();
			} else if (c->calling_tone &&
				   c->count >= ms2samples(500, c->srate) &&
				   (c->count % ms2samples(2500, c->srate)) <
				   ms2samples(600, c->srate))
				y = 0.5f * DSP_TX_AMPLITUDE * c->ct.next();
			if (c->count >= c->timeout && ret == DPSTAT_OK) {
				DSP_DBG("callprog: no answer\n");
				ret = DPSTAT_NOANSWER;
			}
			break;
		case CP_DONE:
			break;
		}
		dst[i] = dsp_clip16(y);
		c->count++;
		c->total++;
	}
	return ret;
}

static struct dp_operations callprog_operations = {
	.name = "Call progress",
	.create = callprog_create,
	.destroy = callprog_delete,
	.process = callprog_process,
};

int dp_callprog_init(void)
{
	return modem_dp_register(DP_CALLPROG, &callprog_operations);
}

void dp_callprog_exit(void)
{
	modem_dp_deregister(DP_CALLPROG, &callprog_operations);
}

} // namespace dsp
