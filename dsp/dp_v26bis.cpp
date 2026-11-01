/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_v26bis.cpp - ITU-T V.26 bis data pump: 2400 or 1200 bit/s one
 *                    way on the switched network, with the 75 baud
 *                    backward channel the other way
 *
 *    The forward channel is 1200 baud differential PSK on an 1800 Hz
 *    carrier (2.1): at 2400 bit/s a dibit a symbol, V.26's alternative B
 *    (phase changes of 45, 135, 225 and 315 degrees for 00, 01, 11 and
 *    10, Table 1, the first bit of the pair on the left); at 1200 bit/s a
 *    bit a symbol, 90 degrees for 0 and 270 for 1 (2.5.1).  The data is
 *    not scrambled.  The backward channel is FSK at 75 baud, 390 Hz for
 *    a mark and 450 Hz for a space (4.1), the mark when there is nothing
 *    to send.
 *
 *    V.26 bis lets the forward channel go either way, half duplex, or
 *    one way only (Table 2/V.26 bis).  D-Modem runs it one way, as it
 *    runs V.23: the answering modem sends the forward channel and the
 *    calling modem the backward channel, both at once for the whole
 *    call, so that there is no turnaround to time.
 *
 *    Start-up (the answerer: silence, the answer tone, 75 ms of silence):
 *
 *    answerer: the synchronizing signal (2.4.2: the continuous dibit 11,
 *              225 degrees a symbol; at 1200 bit/s, where V.26 bis gives
 *              none, D-Modem sends continuous ones, 270 degrees a symbol)
 *              for 1 s (106's 750 to 1400 ms during call establishment,
 *              Table 3), and on until the caller's mark has come for
 *              150 ms; then CONNECT and the data
 *    caller:   on 32 symbols of the synchronizing signal in a row, the
 *              rate it shows, 2400 or 1200 bit/s (V.26 bis has no rate
 *              signal: the answerer's +MS picks it); the backward
 *              channel's mark, CONNECT once it has gone out for 150
 *              ms, and the link's data 1.5 s later (the mark until
 *              then)
 *
 *    The forward channel's receiver uses no training: its equalizer and
 *    carrier loop adapt to its decisions on the eight points from the
 *    synchronizing signal on.  The forward channel's raised cosine has
 *    a roll-off of 1 (V.26 bis sets none): its spectrum, 600 to 3000 Hz,
 *    leaves the backward channel's band clear.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "dm_types.h"
#include "pump.h"
#include "qam.h"

namespace dsp {

#define V26B_BAUD       1200
#define V26B_CARRIER    1800
#define V26B_ALPHA      1.0f
#define V26B_SPAN       4
#define V26B_EQ_LEN     16
#define V26B_POWER      2.0f    /* the points' radius sqrt(2) */
#define V26B_TX_AMPLITUDE (2000.0f * 1.41421f)

/* the backward channel (4.1) */
#define V26B_BC_MARK    390
#define V26B_BC_SPACE   450
#define V26B_BC_BAUD    75

/* the answerer's synchronizing signal at least (ms); the caller's mark
   that it waits for, and that the caller sends before its CONNECT (ms);
   the caller's mark after its CONNECT (ms: the answerer, at the end of
   its synchronizing signal and later on a long line, takes the data
   after it); the symbols of the synchronizing signal in a row that find
   it */
#define V26B_SYNC_MS    1000
#define V26B_MARK_MS    150
#define V26B_HOLD_MS    1500
#define V26B_SYNC_RUN   32

/* the 8 points at 45 degree steps, radius sqrt(2): the forward channel's
   phase changes are odd steps at 2400 bit/s, even ones at 1200 */
static const cplx v26b_points[8] = {
	{ 1.41421f, 0 }, { 1, 1 }, { 0, 1.41421f }, { -1, 1 },
	{ -1.41421f, 0 }, { -1, -1 }, { 0, -1.41421f }, { 1, -1 },
};

enum v26b_state {
	/* answerer */
	B_SILENCE,
	B_ANS,
	B_GAP,
	B_SYNC,                 /* the synchronizing signal */
	/* caller */
	B_WAIT_SYNC,            /* the answerer's synchronizing signal */
	B_MARK,                 /* our mark before CONNECT */
	/* both */
	B_DATA,
};

struct V26bisPump : PumpBase<V26bisPump, v26b_state> {
	static constexpr const char *name = "V.26 bis data pump";

	/* frees the receiver's buffers */
	~V26bisPump() { rx.release(); }

	s32 rate;               /* the forward channel's, bit/s */

	/* the forward channel's transmitter (the answerer's) */
	QamTx tx;
	s16 tx_k;               /* the last point (v26b_points index) */
	Osc tone;

	/* its receiver (the caller's) */
	QamRx rx;
	CarrierDetect carrier;
	s16 rx_on;              /* the far end's carrier */
	s16 rx_k;               /* the last point decided */
	cplx rx_last;           /* and the symbol */
	s16 run225, run270;     /* symbols of either synchronizing signal
				   in a row */
	float rx_mse;
	float data_power;
	s32 data_sym;           /* rx.symbols at CONNECT */

	/* the backward channel: the caller's transmitter, the answerer's
	   receiver */
	FskBitTx bc_tx;
	FskDemod bc_rx;
	s32 mark_run;           /* samples of the caller's mark in a row */
	s16 mark_seen;          /* 150 ms of it */

	cplx tx_symbol();
	s16 link_bit();
	void rx_symbol();
	/* its name in the log */
	const char *tag() const { return "v26bis"; }
	/* in data mode (the carrier watched, no handshake timeout) */
	bool in_data() const { return state == B_DATA; }
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

/* the link's next bit: ones until the data (the caller's until 1.5 s
   into it) */
s16 V26bisPump::link_bit()
{
	u8 b = 1;
	if (state == B_DATA &&
	    (!caller || count >= ms2samples(V26B_HOLD_MS, srate)))
		modem_get_bits(modem, 1, &b, 1);
	return b & 1;
}

/* the forward channel's next symbol: the synchronizing signal (dibit 11,
   or ones at 1200) until the data, then the link's bits as phase changes
   (in 45 degree steps) */
cplx V26bisPump::tx_symbol()
{
	s16 c;

	if (rate == 2400) {
		s16 b1 = link_bit();
		s16 b2 = link_bit();
		/* 00 45, 01 135, 11 225, 10 315 degrees */
		c = b1 ? (b2 ? 5 : 7) : (b2 ? 3 : 1);
	} else
		c = link_bit() ? 6 : 2;
	tx_k = (tx_k + c) & 7;
	return v26b_points[tx_k];
}

/* one received symbol of the forward channel (rx.z): the receiver adapted
   to the point decided; the phase change found (the synchronizing
   signal's, and its rate, before the data) and decoded */
void V26bisPump::rx_symbol()
{
	cplx z = rx.z, d;
	s16 k;
	float a;

	k = nearest_point(v26b_points, 8, z);
	if (!rx_on) {
		rx_k = k;
		rx_last = z;
		return;
	}
	rx_mse += 0.02f * (cplx_pow(cplx_sub(z, v26b_points[k])) / V26B_POWER -
			   rx_mse);
	/* the synchronizing signal is a tone (a steady phase change): the
	   carrier loop alone on it, the equalizer from the data on */
	rx.adapt(v26b_points[k], state != B_DATA ? 0 :
		 rx.symbols - data_sym < 256 ? 0.03f : 0.01f, 0.05f, 0.0005f);
	rx_k = k;
	d = cplx_mulc(z, rx_last);
	rx_last = z;
	/* the phase change in degrees, 0 to 360 */
	a = atan2f(d.im, d.re) * (180.0f / static_cast<float>(M_PI));
	if (a < 0)
		a += 360;

	if (state == B_WAIT_SYNC) {
		run225 = fabsf(a - 225) < 22.5f ? run225 + 1 : 0;
		run270 = fabsf(a - 270) < 22.5f ? run270 + 1 : 0;
		return;
	}
	if (state != B_DATA)
		return;
	u8 b[2];
	if (rate == 2400) {
		/* 45, 135, 225, 315: the quadrant */
		static const u8 dibit[4][2] = {
			{ 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 },
		};
		s16 q = static_cast<s16>(a / 90) & 3;
		b[0] = dibit[q][0];
		b[1] = dibit[q][1];
		modem_put_bits(modem, 1, b, 2);
	} else {
		/* 90 or 270 */
		b[0] = a >= 180;
		modem_put_bits(modem, 1, b, 1);
	}
}

/* the start-up, a sample at a time */
s16 V26bisPump::handshake()
{
	switch (state) {
	/* answering modem */
	case B_SILENCE:
		if (count >= ms2samples(ANS_SILENCE_MS, srate))
			set_state(B_ANS);
		break;
	case B_ANS:
		if (count >= ms2samples(ANS_TONE_MS, srate))
			set_state(B_GAP);
		break;
	case B_GAP:
		if (count >= ms2samples(ANS_GAP_MS, srate))
			set_state(B_SYNC);
		break;
	case B_SYNC:
		if (mark_run >= ms2samples(V26B_MARK_MS, srate))
			mark_seen = 1;
		if (count >= ms2samples(V26B_SYNC_MS, srate) && mark_seen) {
			set_state(B_DATA);
			return connect_at(rate, V26B_BC_BAUD);
		}
		break;

	/* calling modem */
	case B_WAIT_SYNC:
		if (run225 >= V26B_SYNC_RUN || run270 >= V26B_SYNC_RUN) {
			rate = run225 >= V26B_SYNC_RUN ? 2400 : 1200;
			DSP_DBG("v26bis: the synchronizing signal, %" PRId32
				" bit/s\n", rate);
			set_state(B_MARK);
		}
		break;
	case B_MARK:
		if (count >= ms2samples(V26B_MARK_MS, srate)) {
			set_state(B_DATA);
			data_power = rx.power;
			data_sym = rx.symbols;
			return connect_at(V26B_BC_BAUD, rate);
		}
		break;

	case B_DATA:
		if (caller && count % ms2samples(100, srate) == 0 &&
		    rx.power > 0.5f * data_power) {
			stat_snr(-10 * log10f(rx_mse + 1e-6f));
			stat_line(V26B_BC_BAUD, 0, V26B_BAUD, V26B_CARRIER);
			stat_freq_offset(rx.dtheta, V26B_BAUD);
			stat_timing_offset(rx.t_adj, rx.sps);
		}
		break;
	}
	return DPSTAT_OK;
}

/* a sample x in, *y out: the handshake, then the answerer's forward
   channel and backward channel receiver, or the caller's forward channel
   receiver and backward channel */
s16 V26bisPump::sample(float x, float *y)
{
	s16 r = handshake();

	*y = 0;
	if (!caller) {
		if (state == B_ANS)
			*y = DSP_TX_AMPLITUDE * tone.next();
		else if (state == B_SYNC || state == B_DATA)
			*y = tx.sample([this] { return tx_symbol(); });

		s16 bit = bc_rx.run(x);
		carrier_tick(bc_rx.carrier);
		mark_run = bc_rx.carrier && bc_rx.last_dec ? mark_run + 1 : 0;
		if (state == B_DATA && bit >= 0 && bc_rx.carrier) {
			u8 b = static_cast<u8>(bit);
			modem_put_bits(modem, 1, &b, 1);
		}
		return r;
	}

	if (state == B_MARK || state == B_DATA)
		*y = bc_tx.sample([this] { return link_bit(); });

	if (rx.sample(x))
		rx_symbol();
	switch (carrier.run(rx.power)) {
	case 1:
		if (state != B_DATA) {
			rx.reset();
			run225 = run270 = 0;
		}
		rx_on = 1;
		break;
	case -1:
		if (state != B_DATA) {
			rx_on = 0;
			run225 = run270 = 0;
		}
		break;
	}
	carrier_tick(carrier.on);
	return r;
}

/* V.26 bis: the answerer sends the forward channel at +MS's highest rate
   (1200 when that is below 2400), the caller the backward channel;
   timeouts from S7 and S10.  0, or -1 when out of memory. */
s16 V26bisPump::start(struct modem *m, enum DP_ID id, s16 caller_,
		      s32 srate_, s32 max_frag)
{
	s32 max_rate;

	(void)id;
	(void)max_frag;
	pump_setup(m, srate_, caller_);

	max_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));
	rate = max_rate > 0 && max_rate < 2400 ? 1200 : 2400;

	if (caller_) {
		carrier.init(150, 90);
		if (rx.init(srate_, V26B_CARRIER, V26B_BAUD, V26B_ALPHA,
			    V26B_SPAN, V26B_EQ_LEN, V26B_POWER))
			return -1;
		bc_tx.init(V26B_BC_MARK, V26B_BC_SPACE, DSP_TX_AMPLITUDE,
			   V26B_BC_BAUD, srate_);
		state = B_WAIT_SYNC;
	} else {
		tx.init(srate_, V26B_CARRIER, V26B_BAUD, V26B_ALPHA, V26B_SPAN,
			sqrtf(V26B_POWER), V26B_TX_AMPLITUDE);
		tone.set_freq(2100, srate_);
		bc_rx.init(V26B_BC_MARK, V26B_BC_SPACE, V26B_BC_BAUD, srate_);
		if (rt && rt->ans_sent) {
			rt->ans_sent = 0;
			state = B_GAP;
		} else
			state = B_SILENCE;
	}
	DSP_DBG("v26bis: create %s, %" PRId32 " bit/s, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", rate, srate_);
	return 0;
}

/* registers the pump as DP_V26BIS; 0 */
s16 dp_v26bis_init(void)
{
	modem_dp_register(DP_V26BIS, &PumpOps<V26bisPump>::ops);
	return 0;
}

/* deregisters it */
void dp_v26bis_exit(void)
{
	modem_dp_deregister(DP_V26BIS, &PumpOps<V26bisPump>::ops);
}

} // namespace dsp
