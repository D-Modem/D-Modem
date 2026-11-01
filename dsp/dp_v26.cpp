/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_v26.cpp - ITU-T V.26 ter data pump: 2400 and 1200 bit/s duplex
 *                 on a 2-wire line (the switched network)
 *
 *    1200 baud differential PSK on an 1800 Hz carrier both ways (100%
 *    raised cosine), the two directions separated by an echo canceller:
 *    a dibit a symbol at 2400 bit/s (phase changes 0, 90, 180 and 270
 *    degrees for 00, 01, 11, 10, Table 1), a bit at 1200 (0 or 180,
 *    Table 2).  The data is scrambled, the caller with GPC (1 + x^-18 +
 *    x^-23), the answerer with GPA (1 + x^-5 + x^-23), as V.32.
 *
 *    Start-up (6.3, Figure 1/V.26 ter):
 *
 *    A  answerer: silence, ANS (2100 Hz), 75 ms of silence
 *    B  half duplex, at 1200 bit/s:
 *       answerer: the synchronizing signal (2.7: 32 phase reversals,
 *                 then scrambled ones from Appendix I's register) and
 *                 256 bits of its rate pattern (6.1.3: an octet
 *                 repeated, scrambled), then silence until the caller's
 *                 (again after 2 s without it)
 *       caller:   on 4 octets of the answerer's pattern, 250 ms of
 *                 silence, the synchronizing signal and the pattern of
 *                 the rate chosen
 *       answerer: 250 ms of silence, 2100 Hz for 500 ms (G.164: echo
 *                 suppressors off), 75 ms of silence
 *    C  at the rate chosen:
 *       answerer: its echo cancelling sequence (scrambled ones) while
 *                 its canceller learns, 25 ms of silence, the
 *                 synchronizing signal and scrambled zeros; silent once
 *                 the caller's signal has come for 50 ms
 *       caller:   on 64 of those zeros, its own echo cancelling
 *                 sequence, 25 ms of silence, the synchronizing signal
 *                 and scrambled zeros
 *       answerer: on 64 of them, the synchronizing signal and zeros
 *                 again (both send now), on 64 more ones
 *       caller:   on 64 zeros, 109 ON: ones for 128 bits, then data
 *       answerer: on 64 ones, 109 ON: data
 *
 *    The received data reaches the link once ones have come (the zeros
 *    of the start-up would be breaks to an asynchronous DTE).  V.26 ter
 *    has no retrain: a line that goes bad ends the call (S10).  The
 *    optional half-duplex mode (clause 7) is not done.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "dm_types.h"
#include "bits.h"
#include "pump.h"
#include "qam.h"

namespace dsp {

#define V26_BAUD        1200
#define V26_CARRIER     1800
#define V26_ALPHA       1.0f    /* 100% raised cosine (2.4) */
#define V26_SPAN        4
#define V26_EQ_LEN      16
#define V26_POWER       2.0f    /* the points (+-1, +-1) */
#define V26_EC_LEN      128
#define V26_TX_AMPLITUDE (2000.0f * 1.41421f)

/* the start-up's lengths: segment 1 of the synchronizing signal
   (symbols), segment 2 (bits), the rate pattern (bits), the echo
   cancelling sequence (ms: at least 650 for the network's cancellers,
   note 3 of 6.3), the runs of zeros and ones looked for, the ones before
   the caller's data */
#define V26_SEG1        32
#define V26_SEG2        64
#define V26_PATTERN     256
#define V26_EC_MS       1000
#define V26_EC_HOLD_MS  150     /* the caller's: the far end still
				   sending as ours starts */
#define V26_EC_MU       0.1f
#define V26_RUN         64
#define V26_ONES        128

/* the rate pattern octets (Table 7): 1200, 2400, both; 4800 alone and
   2400 with 4800 (a 4800 bit/s modem's) */
#define RP_1200         0x01
#define RP_2400         0x03
#define RP_BOTH         0x07
#define RP_4800         0x05
#define RP_2400_4800    0x09

/* the 4 points, counterclockwise: a phase change of 90 degrees is one
   step */
static const cplx v26_points[4] = {
	{ 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 },
};

enum v26_txm {
	TXM_SILENCE,
	TXM_TONE,               /* 2100 Hz: ANS, and G.164's */
	TXM_REV,                /* segment 1: phase reversals */
	TXM_BITS,               /* scrambled bits from tx_src */
};

/* what TXM_BITS sends */
enum v26_src {
	SRC_ONES,
	SRC_ZEROS,
	SRC_PATTERN,            /* the octet tx_octet, LSB first */
	SRC_DATA,               /* the link's (ones for the first bits) */
};

enum v26_state {
	/* answerer */
	A_SILENCE,
	A_ANS,
	A_GAP,
	A_PATTERN,              /* sync and our rate pattern */
	A_WAIT_PATTERN,         /* the caller's */
	A_QUIET1,               /* 250 ms */
	A_TONE,                 /* 500 ms of 2100 Hz */
	A_QUIET2,               /* 75 ms */
	A_EC,                   /* our echo cancelling sequence */
	A_QUIET3,               /* 25 ms */
	A_ZEROS1,               /* sync and zeros until the caller's signal */
	A_WAIT_ZEROS,           /* silent: the caller's zeros */
	A_ZEROS2,               /* sync and zeros, both ways */
	A_ONES,                 /* ones until the caller's */
	/* caller */
	C_WAIT_PATTERN,
	C_QUIET1,               /* 250 ms */
	C_PATTERN,              /* sync and the rate chosen */
	C_WAIT_ZEROS,
	C_EC,
	C_QUIET2,               /* 25 ms */
	C_ZEROS,                /* sync and zeros until the answerer's */
	C_ONES,                 /* 128 ones */
	/* both */
	V26_DATA,
};

struct V26Pump : PumpBase<V26Pump, v26_state> {
	static constexpr const char *name = "V.26 ter data pump";

	/* frees the receiver's and the echo canceller's buffers */
	~V26Pump() { cleanup(); }

	u8 offer;               /* our rates: RP_1200, RP_2400 or RP_BOTH */
	s16 nbits;              /* bits a symbol: 1 (1200) or 2 (2400) */
	s32 rate;               /* the rate chosen (0: none yet) */

	/* transmitter */
	QamTx tx;
	enum v26_txm txm;
	enum v26_src tx_src;
	s32 tx_syms;            /* symbols sent in the current tx state */
	s32 tx_bits;            /* of them, bits (TXM_BITS) */
	s16 tx_seg2;            /* segment 2's bits still to send */
	s16 tx_k;               /* the last point (v26_points index) */
	s16 tx_nbits;           /* bits a symbol now */
	u8 tx_octet;
	u32 tx_scr;
	s16 tx_scr_tap;
	Osc tone;
	float last_tx;

	/* receiver */
	EchoCanceller ec;
	float ec_mu;
	QamRx rx;
	CarrierDetect carrier;
	s16 rx_on;              /* decoding (the far end's carrier) */
	s16 rx_k;               /* the last point decided */
	cplx rx_last;           /* and the symbol */
	s16 rx_nbits;
	u32 rx_scr;
	s16 rx_scr_tap;
	u32 rx_hist;            /* the last 32 descrambled bits, newest in
				   bit 0 */
	s32 rx_count;           /* bits since the carrier came */
	s32 run0, run1;         /* zeros and ones in a row */
	s16 rx_pattern;         /* a rate pattern seen (its octet), -1 */
	s16 rx_ones;            /* the data reaches the link */
	s16 ones_seen;          /* answerer: the caller's ones have come */
	float rx_mse;
	float rx_zpow;          /* the last symbols' power (of V26_POWER) */
	s32 rx_good;            /* symbols of the far end's signal ... */
	s16 rx_gap;             /* ... and of a gap after them */
	float data_power;

	cplx tx_symbol();
	s16 tx_bit();
	void set_tx(enum v26_txm m);
	void send_sync(enum v26_src then);
	void rx_reset();
	void rx_bit(s16 bit);
	void rx_symbol();
	void rx_sample(float x);
	/* its name in the log */
	const char *tag() const { return "v26"; }
	/* in data mode (the carrier watched, no handshake timeout) */
	bool in_data() const { return state == V26_DATA; }
	bool listening() const;
	void use_rate(s32 r);
	s16 connect();
	s16 handshake();
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	void cleanup();
	s16 sample(float x, float *y);
	/* cnt samples in and out, through sample() */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}
};

/*
 *    rate patterns
 */

/* the octet o rotated so that it is one of Table 7's (6.1.3: a rotation
   stands for the octet), or -1 */
static s16 pattern_octet(u8 o)
{
	static const u8 known[] = {
		RP_1200, RP_2400, RP_BOTH, RP_4800, RP_2400_4800,
	};
	for (s16 r = 0; r < 8; r++) {
		u8 x = static_cast<u8>((o >> r) | (o << (8 - r)));
		for (u8 k : known)
			if (x == k)
				return k;
	}
	return -1;
}

/* the rate of 1200 and 2400 a pattern octet p allows that we offer too
   (the faster), 0 for none */
static s32 pattern_rate(s16 p, u8 offer)
{
	bool r1200 = p == RP_1200 || p == RP_BOTH;
	bool r2400 = p == RP_2400 || p == RP_BOTH || p == RP_2400_4800;
	if (r2400 && offer != RP_1200)
		return 2400;
	if (r1200 && offer != RP_2400)
		return 1200;
	return 0;
}

/*
 *    transmitter (runs at the symbol rate)
 */

/* transmit m from its first symbol */
void V26Pump::set_tx(enum v26_txm m)
{
	txm = m;
	tx_syms = 0;
	tx_bits = 0;
}

/* the synchronizing signal (2.7): 32 reversals, then segment 2 (64
   scrambled ones), then the bits of then */
void V26Pump::send_sync(enum v26_src then)
{
	set_tx(TXM_REV);
	tx_src = then;
}

/* the next data bit to scramble */
s16 V26Pump::tx_bit()
{
	s16 b = 1;
	if (tx_seg2 > 0) {
		tx_seg2--;
		return 1;
	}
	switch (tx_src) {
	case SRC_ONES:
		break;
	case SRC_ZEROS:
		b = 0;
		break;
	case SRC_PATTERN:
		b = tx_octet >> (tx_bits & 7) & 1;
		break;
	case SRC_DATA:
		{
			u8 x = 1;
			modem_get_bits(modem, 1, &x, 1);
			b = x & 1;
		}
		break;
	}
	tx_bits++;
	return b;
}

/* the next symbol of the current transmit signal (for tx.sample()) */
cplx V26Pump::tx_symbol()
{
	cplx z = { 0, 0 };
	s16 c = 0;

	switch (txm) {
	case TXM_SILENCE:
	case TXM_TONE:
		tx_syms++;
		return z;
	case TXM_REV:
		c = 2;
		if (tx_syms + 1 >= V26_SEG1) {
			/* segment 2 next, from Appendix I's register: the
			   one listed, a step on (its first output bit is the
			   register's own) */
			txm = TXM_BITS;
			tx_bits = 0;
			tx_seg2 = V26_SEG2;
			tx_scr = caller ? 0x4FFF83 : 0x307070;
			scramble(&tx_scr, tx_scr_tap, 1);
		}
		break;
	case TXM_BITS:
		if (tx_nbits == 2) {
			s16 b1 = scramble(&tx_scr, tx_scr_tap, tx_bit());
			s16 b2 = scramble(&tx_scr, tx_scr_tap, tx_bit());
			/* 00 0, 01 90, 11 180, 10 270 degrees */
			c = b1 ? (b2 ? 2 : 3) : (b2 ? 1 : 0);
		} else
			c = scramble(&tx_scr, tx_scr_tap, tx_bit()) ? 2 : 0;
		break;
	}
	tx_k = (tx_k + c) & 3;
	tx_syms++;
	return v26_points[tx_k];
}

/*
 *    receiver
 */

/* the receiver back to waiting for the far end's carrier */
void V26Pump::rx_reset()
{
	rx.reset();
	rx.agc_hold = 0;
	rx_zpow = V26_POWER;
	rx_good = rx_gap = 0;
	rx_on = 0;
	rx_count = 0;
	run0 = run1 = 0;
	rx_hist = 0;
}

/* we hear the far end now: not while we send in a half duplex part of
   the start-up (our echo, the canceller not yet trained) */
bool V26Pump::listening() const
{
	switch (state) {
	case A_PATTERN:
	case A_TONE:
	case A_EC:
	case C_PATTERN:
	case C_EC:
		return false;
	default:
		return true;
	}
}

/* one descrambled bit: the runs of zeros and ones, the rate pattern
   (4 octets alike: 32 bits), the data to the link once ones have come */
void V26Pump::rx_bit(s16 bit)
{
	rx_hist = rx_hist << 1 | (bit & 1);
	rx_count++;
	if (bit) {
		run1++;
		run0 = 0;
	} else {
		run0++;
		run1 = 0;
	}
	if (rx_count >= 32 && (rx_hist >> 8 & 0xffffff) == (rx_hist & 0xffffff)) {
		/* the octet in the order sent: its first bit is the oldest */
		u8 o = 0;
		for (s16 i = 0; i < 8; i++)
			o = static_cast<u8>(o | (rx_hist >> (7 - i) & 1) << i);
		s16 p = pattern_octet(o);
		if (p >= 0 && p != rx_pattern) {
			DSP_DBG("v26: rate pattern %02X\n", p);
			rx_pattern = p;
		}
	}
	if (state != V26_DATA && state != C_ONES)
		return;
	if (!rx_ones && run1 >= 16)
		rx_ones = 1;
	if (rx_ones) {
		u8 b = static_cast<u8>(bit & 1);
		modem_put_bits(modem, 1, &b, 1);
	}
}

/* one received symbol (rx.z): the point decided, the receiver adapted
   to it, the phase change decoded and descrambled */
void V26Pump::rx_symbol()
{
	cplx z = rx.z;
	s16 k, c;

	if (!listening() || !rx_on) {
		rx_k = nearest_point(v26_points, 4, z);
		rx_last = z;
		return;
	}
	/* a gap in the far end's signal (the 25 ms of silence in sequence
	   C, our hearing of it as the carrier detector holds on): the
	   gain, the equalizer and the decisions held, so that the signal
	   coming back is not overdriven */
	rx_zpow += 0.5f * (cplx_pow(z) - rx_zpow);
	if (rx_zpow < 0.2f * V26_POWER && rx_good >= 100 &&
	    ++rx_gap < 200) {
		rx.agc_hold = 1;
		rx_last = z;
		return;
	}
	if (rx_gap) {
		rx_gap = 0;
		rx_good = 0;
	}
	rx_good++;
	if (state != V26_DATA)
		rx.agc_hold = 0;
	k = nearest_point(v26_points, 4, z);
	rx_mse += 0.02f * (cplx_pow(cplx_sub(z, v26_points[k])) / V26_POWER -
			   rx_mse);
	rx.adapt(v26_points[k], rx.symbols < 64 ? 0.03f : 0.01f, 0.05f,
		 0.0005f);
	c = (k - rx_k) & 3;
	rx_k = k;
	cplx last = rx_last;
	rx_last = z;
	if (rx_nbits == 2) {
		rx_bit(descramble(&rx_scr, rx_scr_tap, c >= 2));
		rx_bit(descramble(&rx_scr, rx_scr_tap, c == 1 || c == 2));
	} else {
		/* 0 or 180 degrees; a quarter turn (an error) by the nearer */
		s16 b = c == 2;
		if (c & 1) {
			cplx st = cplx_mulc(z, last);
			b = st.re < 0;
		}
		rx_bit(descramble(&rx_scr, rx_scr_tap, b));
	}
}

/* one received sample x: the echo canceller, the receiver, the carrier */
void V26Pump::rx_sample(float x)
{
	x = ec.run(last_tx, x, ec_mu);

	if (rx.sample(x))
		rx_symbol();

	switch (carrier.run(rx.power)) {
	case 1:
		if (state != V26_DATA)
			rx_reset();
		rx_on = 1;
		break;
	case -1:
		if (state != V26_DATA)
			rx_reset();
		break;
	}
	carrier_tick(carrier.on);
}

/*
 *    handshake
 */

/* both directions at rate r from sequence C on */
void V26Pump::use_rate(s32 r)
{
	rate = r;
	nbits = r == 2400 ? 2 : 1;
	tx_nbits = rx_nbits = nbits;
}

/* to data: DPSTAT_CONNECT */
s16 V26Pump::connect()
{
	set_state(V26_DATA);
	data_power = rx.power;
	return connect_at(rate, rate);
}

/* called once a symbol (and every sample while silent or in a tone),
   before the symbol is made */
s16 V26Pump::handshake()
{
	switch (state) {
	/* answering modem: sequence A */
	case A_SILENCE:
		if (count >= ms2samples(ANS_SILENCE_MS, srate)) {
			set_state(A_ANS);
			set_tx(TXM_TONE);
		}
		break;
	case A_ANS:
		if (count >= ms2samples(ANS_TONE_MS, srate)) {
			set_state(A_GAP);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_GAP:
		if (count >= ms2samples(ANS_GAP_MS, srate)) {
			set_state(A_PATTERN);
			tx_nbits = rx_nbits = 1;
			tx_octet = offer;
			send_sync(SRC_PATTERN);
		}
		break;
	/* sequence B */
	case A_PATTERN:
		if (txm == TXM_BITS && tx_seg2 == 0 &&
		    tx_bits >= V26_PATTERN) {
			set_state(A_WAIT_PATTERN);
			set_tx(TXM_SILENCE);
			rx_pattern = -1;
		}
		break;
	case A_WAIT_PATTERN:
		if (rx_pattern >= 0) {
			s32 r = pattern_rate(rx_pattern, offer);
			if (!r) {
				DSP_DBG("v26: the caller's pattern %02X is no "
					"rate of ours\n", rx_pattern);
				return DPSTAT_ERROR;
			}
			use_rate(r);
			DSP_DBG("v26: rate %" PRId32 "\n", rate);
			set_state(A_QUIET1);
		} else if (count >= ms2samples(2000, srate)) {
			/* again from the start of sequence B */
			set_state(A_PATTERN);
			send_sync(SRC_PATTERN);
		}
		break;
	case A_QUIET1:
		if (count >= ms2samples(250, srate)) {
			set_state(A_TONE);
			set_tx(TXM_TONE);
		}
		break;
	case A_TONE:
		if (count >= ms2samples(500, srate)) {
			set_state(A_QUIET2);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_QUIET2:
		if (count >= ms2samples(75, srate)) {
			/* sequence C: our canceller learns on our scrambled
			   ones while the caller is silent */
			set_state(A_EC);
			set_tx(TXM_BITS);
			tx_src = SRC_ONES;
			ec_mu = V26_EC_MU;
		}
		break;
	case A_EC:
	case C_EC:
		/* the caller's canceller learns once the answerer, which
		   goes silent 50 ms after it hears us, is quiet */
		if (state == C_EC && !ec_mu &&
		    count >= ms2samples(V26_EC_HOLD_MS, srate))
			ec_mu = V26_EC_MU;
		if (count >= ms2samples(V26_EC_MS, srate)) {
			float erle = ec.judge();
			DSP_DBG("v26: echo canceller took %.1f dB away%s\n",
				erle, ec.off ? ": off" : "");
			ec_mu = 0;
			set_state(state == A_EC ? A_QUIET3 : C_QUIET2);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_QUIET3:
		if (count >= ms2samples(25, srate)) {
			set_state(A_ZEROS1);
			send_sync(SRC_ZEROS);
		}
		break;
	case A_ZEROS1:
		/* the caller's echo cancelling sequence for 50 ms */
		if (carrier.on && carrier_run >= ms2samples(50, srate)) {
			set_state(A_WAIT_ZEROS);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_WAIT_ZEROS:
		if (run0 >= V26_RUN) {
			set_state(A_ZEROS2);
			send_sync(SRC_ZEROS);
			run0 = 0;
		}
		break;
	case A_ZEROS2:
		/* 64 more zeros, both sending (or the caller's ones: it has
		   had 64 of ours); 256 of ours at least, for the caller to
		   find 64 of them after our synchronizing signal */
		if ((run0 >= V26_RUN || run1 >= V26_RUN) && txm == TXM_BITS &&
		    tx_seg2 == 0 && tx_bits >= 4 * V26_RUN) {
			set_state(A_ONES);
			tx_src = SRC_ONES;
			tx_bits = 0;
			ones_seen = run1 >= V26_RUN;
		}
		break;
	case A_ONES:
		/* the caller's 64 ones (they may have come already), and 64
		   of ours, for the caller to know where our data starts */
		if (run1 >= V26_RUN)
			ones_seen = 1;
		if (ones_seen && tx_bits >= V26_RUN) {
			rx_ones = 1;
			tx_src = SRC_DATA;
			return connect();
		}
		break;

	/* calling modem: sequence B */
	case C_WAIT_PATTERN:
		if (rx_pattern >= 0) {
			s32 r = pattern_rate(rx_pattern, offer);
			if (!r) {
				DSP_DBG("v26: no common rate in the "
					"answerer's pattern %02X\n",
					rx_pattern);
				return DPSTAT_ERROR;
			}
			use_rate(r);
			DSP_DBG("v26: rate %" PRId32 "\n", rate);
			set_state(C_QUIET1);
		}
		break;
	case C_QUIET1:
		if (count >= ms2samples(250, srate)) {
			set_state(C_PATTERN);
			tx_nbits = 1;
			tx_octet = rate == 2400 ? RP_2400 : RP_1200;
			send_sync(SRC_PATTERN);
		}
		break;
	case C_PATTERN:
		if (txm == TXM_BITS && tx_seg2 == 0 &&
		    tx_bits >= V26_PATTERN) {
			set_state(C_WAIT_ZEROS);
			set_tx(TXM_SILENCE);
			tx_nbits = rx_nbits = nbits;
			run0 = 0;
		}
		break;
	/* sequence C */
	case C_WAIT_ZEROS:
		if (run0 >= V26_RUN) {
			set_state(C_EC);
			set_tx(TXM_BITS);
			tx_src = SRC_ONES;
		}
		break;
	case C_QUIET2:
		if (count >= ms2samples(25, srate)) {
			set_state(C_ZEROS);
			send_sync(SRC_ZEROS);
			run0 = 0;
		}
		break;
	case C_ZEROS:
		/* the answerer's zeros again (it sends while we do) */
		if (run0 >= V26_RUN && txm == TXM_BITS && tx_seg2 == 0) {
			set_state(C_ONES);
			tx_src = SRC_ONES;
			tx_bits = 0;
		}
		break;
	case C_ONES:
		if (tx_bits >= V26_ONES) {
			tx_src = SRC_DATA;
			return connect();
		}
		break;

	case V26_DATA:
		if (count % ms2samples(100, srate) == 0 &&
		    rx.power > 0.5f * data_power) {
			stat_snr(-10 * log10f(rx_mse + 1e-6f));
			stat_line(V26_BAUD, V26_CARRIER, V26_BAUD, V26_CARRIER);
			stat_freq_offset(rx.dtheta, V26_BAUD);
			stat_timing_offset(rx.t_adj, rx.sps);
		}
		break;
	}
	return DPSTAT_OK;
}

/* a sample x in, *y out: the handshake (at symbol boundaries, and every
   sample in silence or a tone), the transmitter, the receiver */
s16 V26Pump::sample(float x, float *y)
{
	s16 r = DPSTAT_OK;

	if (txm == TXM_SILENCE || txm == TXM_TONE ||
	    tx.t + 1.0f / tx.sps >= 1) {
		r = handshake();
		if (r == DPSTAT_ERROR)
			return r;
	}

	if (txm == TXM_TONE)
		*y = DSP_TX_AMPLITUDE * tone.next();
	else
		*y = tx.sample([this] { return tx_symbol(); });
	last_tx = *y;

	rx_sample(x);
	return r;
}

/* V.26 ter, the rates limited by +MS; the transmitter, receiver and
   echo canceller; timeouts from S7 and S10.  0, or -1 when out of
   memory. */
s16 V26Pump::start(struct modem *m, enum DP_ID id, s16 caller_,
		   s32 srate_, s32 max_frag)
{
	s32 min_rate, max_rate;

	(void)id;
	(void)max_frag;
	pump_setup(m, srate_, caller_);

	min_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MIN_RATE));
	max_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));
	if (max_rate > 0 && max_rate < 2400)
		offer = RP_1200;
	else if (min_rate > 1200)
		offer = RP_2400;
	else
		offer = RP_BOTH;

	tx.init(srate_, V26_CARRIER, V26_BAUD, V26_ALPHA, V26_SPAN,
		sqrtf(V26_POWER), V26_TX_AMPLITUDE);
	tone.set_freq(2100, srate_);
	carrier.init(150, 90);
	if (rx.init(srate_, V26_CARRIER, V26_BAUD, V26_ALPHA, V26_SPAN,
		    V26_EQ_LEN, V26_POWER) ||
	    ec.init(V26_EC_LEN))
		return -1;

	/* the caller scrambles with GPC, the answerer with GPA (6.1.1) */
	tx_scr_tap = caller_ ? SCR_GPC : SCR_GPA;
	rx_scr_tap = caller_ ? SCR_GPA : SCR_GPC;
	tx_nbits = rx_nbits = nbits = 1;
	rx_pattern = -1;

	if (caller_) {
		state = C_WAIT_PATTERN;
		txm = TXM_SILENCE;
	} else if (rt && rt->ans_sent) {
		rt->ans_sent = 0;
		state = A_GAP;
		txm = TXM_SILENCE;
	} else {
		state = A_SILENCE;
		txm = TXM_SILENCE;
	}
	DSP_DBG("v26: create %s, rates %02X, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", offer, srate_);
	return 0;
}

/* frees the buffers rx and ec took */
void V26Pump::cleanup()
{
	rx.release();
	ec.release();
}

/* registers the pump as DP_V26TER; 0 */
s16 dp_v26_init(void)
{
	modem_dp_register(DP_V26TER, &PumpOps<V26Pump>::ops);
	return 0;
}

/* deregisters it */
void dp_v26_exit(void)
{
	modem_dp_deregister(DP_V26TER, &PumpOps<V26Pump>::ops);
}

} // namespace dsp
