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
#include "pump.h"

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

struct CallProgPump : Pump {
	static constexpr const char *name = "Call progress";

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

	void set_state(enum cp_state state_);
	int block();
	float dial_sample();
	int start(struct modem *m, enum DP_ID id, int caller, int srate_, int max_frag);
	int process(void *in, void *out, int cnt) override;
};

void CallProgPump::set_state(enum cp_state state_)
{
	DSP_DBG("callprog: state %d -> %d at %ld\n", state, state_, total);
	state = state_;
	count = 0;
}

int CallProgPump::start(struct modem *m, enum DP_ID id, int caller, int srate_, int max_frag)
{
	const char *dial_;
	int i, v;
	static const float dt_freq[3] = { 350, 440, 425 };

	modem = m;
	srate = srate_;
	rt = (struct dp_runtime *)modem_get_param(m, MDMPRM_DPRUNTIME);
	if (rt) {
		rt->answer_tone = ANSTONE_NONE;
		rt->answer_tone_end = 0;
	}

	block_len = srate_ * BLOCK_MS / 1000;
	for (i = 0; i < 3; i++)
		dialtone[i].g.init(dt_freq[i], block_len, srate_);
	ans2100.g.init(2100, block_len, srate_);
	ans2225.g.init(2225, block_len, srate_);
	usb1.g.init(2250, block_len, srate_);
	fine2225.init(2225, srate_ / 5, srate_);
	fine2250.init(2250, srate_ / 5, srate_);
	cpt_bp.bandpass(480, 1.2f, srate_);

	v = modem_get_sreg(m, SREG_DTMF_DURATION);
	dial_on = ms2samples(v > 40 ? v : 95, srate_);
	dial_off = dial_on;

	dial_ = (const char *)modem_get_param(m, MDMPRM_DIALSTR);
	dial = dial_ ? dial_ : "";
	/* X2 and X4 wait for a dial tone, the others dial blindly */
	v = modem_get_sreg(m, SREG_X_CODE);
	need_dialtone = (v == 2 || v == 4);

	v = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	timeout = (long)(v > 0 ? v : 50) * srate_;

	calling_tone = modem_get_param(m, GetCallingToneFlag) > 0;
	fax = modem_get_param(m, MDMPRM_MODE) == MODEM_MODE_FAX;
	ct.set_freq(fax ? 1100 : 1300, srate_);

	/* like the original, X2/X4 insist on a dial tone even when there
	   is nothing to dial (D-Modem users run ATX3 for SIP lines) */
	if (*dial || need_dialtone)
		state = CP_DIALTONE;
	else
		state = CP_WAIT_ANSWER;
	DSP_DBG("callprog: create, dial '%s', srate %d\n", dial, srate_);
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
int CallProgPump::block()
{
	float bp = block_energy / block_len;
	float min = tone_power(150);    /* about -43 dBm0 */
	float dt;
	int i, on;

	block_energy = 0;

	/* dial tone: any of the usual frequencies (or the 350+440 pair) */
	dt = 0;
	for (i = 0; i < 3; i++)
		dt += dialtone[i].g.power;
	if (dt > 0.6f * bp && bp > min)
		dialtone_blocks++;
	else
		dialtone_blocks = 0;

	if (state != CP_WAIT_ANSWER)
		return DPSTAT_OK;

	/* answer tones: 300 ms of 2100 Hz, 2225 Hz or V.22 unscrambled ones */
	if (cp_tone_block(&ans2100, bp, min) >= 300 / BLOCK_MS ||
	    cp_tone_block(&ans2225, bp, min) >= 300 / BLOCK_MS ||
	    cp_tone_block(&usb1, bp, min) >= 300 / BLOCK_MS) {
		int tone = ans2225.blocks >= 300 / BLOCK_MS &&
			fine2225.power > fine2250.power ? ANSTONE_2225 :
			ANSTONE_2100;
		DSP_DBG("callprog: answer tone %s detected\n",
			tone == ANSTONE_2225 ? "2225" : "2100");
		/* the runtime comes with modem_start(), after we were
		   created for a dial */
		if (!rt)
			rt = (struct dp_runtime *)
				modem_get_param(modem, MDMPRM_DPRUNTIME);
		if (rt) {
			rt->answer_tone = tone;
			rt->answer_tone_end = total;
		}
		set_state(CP_DONE);
		return DPSTAT_CHANGEDP;
	}

	/* busy / congestion: 4 cycles of 0.2..0.8 s on and off */
	on = cpt_energy / block_len > min &&
		cpt_energy > 0.5f * block_len * bp;
	cpt_energy = 0;
	if (on == cpt_on) {
		cpt_run++;
	} else {
		int len = cpt_run * BLOCK_MS;
		if (len >= 200 && len <= 800)
			busy_cycles += cpt_on ? 1 : 0;
		else
			busy_cycles = 0;
		cpt_on = on;
		cpt_run = 1;
		if (busy_cycles >= 4) {
			DSP_DBG("callprog: busy\n");
			return DPSTAT_BUSY;
		}
	}
	return DPSTAT_OK;
}

float CallProgPump::dial_sample()
{
	char ch;

	for (;;) {
		if (pause_left > 0) {
			pause_left--;
			return 0;
		}
		if (digit_left > 0) {
			digit_left--;
			if (digit_on) {
				if (digit_left == 0) {
					digit_on = 0;
					digit_left = dial_off;
				}
				return dtmf.next();
			}
			return 0;
		}
		ch = dial[dial_pos];
		if (!ch) {
			set_state(CP_WAIT_ANSWER);
			return 0;
		}
		dial_pos++;
		if (ch == ',') {
			int s8 = modem_get_sreg(modem, SREG_DIAL_PAUSE_TIME);
			pause_left = (long)(s8 > 0 ? s8 : 2) * srate;
		} else if (ch == 'W' || ch == 'w') {
			pause_left = 2L * srate;
		} else if (!dtmf.set(ch, srate)) {
			digit_on = 1;
			digit_left = dial_on;
		}
		/* other modifiers (T, P, spaces, ...) are ignored */
	}
}

int CallProgPump::process(void *in, void *out, int cnt)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int ret = DPSTAT_OK;
	int i;

	for (i = 0; i < cnt; i++) {
		float x = src[i], y = 0, b;
		int k;

		for (k = 0; k < 3; k++)
			dialtone[k].g.run(x);
		ans2100.g.run(x);
		ans2225.g.run(x);
		usb1.g.run(x);
		fine2225.run(x);
		fine2250.run(x);
		b = cpt_bp.run(x);
		cpt_energy += b * b;
		block_energy += x * x;
		if (++block_n >= block_len) {
			block_n = 0;
			k = block();
			if (k != DPSTAT_OK && ret == DPSTAT_OK)
				ret = k;
		}

		switch (state) {
		case CP_DIALTONE:
			if (dialtone_blocks * BLOCK_MS >= 1000 ||
			    (!need_dialtone &&
			     count >= ms2samples(2000, srate)))
				set_state(CP_DIAL);
			else if (count >= (long)modem_get_sreg(modem,
						SREG_DIAL_TONE_WAIT_TIME) * srate &&
				 count >= ms2samples(2000, srate)) {
				DSP_DBG("callprog: no dial tone\n");
				if (ret == DPSTAT_OK)
					ret = DPSTAT_NODIALTONE;
			}
			break;
		case CP_DIAL:
			y = dial_sample();
			break;
		case CP_WAIT_ANSWER:
			if (fax) {
				/* CNG: 0.5 s of 1100 Hz every 3.5 s */
				if (count % ms2samples(3500, srate) <
				    ms2samples(500, srate))
					y = 0.5f * DSP_TX_AMPLITUDE * ct.next();
			} else if (calling_tone &&
				   count >= ms2samples(500, srate) &&
				   (count % ms2samples(2500, srate)) <
				   ms2samples(600, srate))
				y = 0.5f * DSP_TX_AMPLITUDE * ct.next();
			if (count >= timeout && ret == DPSTAT_OK) {
				DSP_DBG("callprog: no answer\n");
				ret = DPSTAT_NOANSWER;
			}
			break;
		case CP_DONE:
			break;
		}
		dst[i] = dsp_clip16(y);
		count++;
		total++;
	}
	return ret;
}

int dp_callprog_init(void)
{
	return modem_dp_register(DP_CALLPROG, &PumpOps<CallProgPump>::ops);
}

void dp_callprog_exit(void)
{
	modem_dp_deregister(DP_CALLPROG, &PumpOps<CallProgPump>::ops);
}

} // namespace dsp
