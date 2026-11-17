/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_v32.cpp - ITU V.32 / V.32bis data pump (4800 to 14400 bit/s),
 *                 and V.32terbo (AT&T's, 16800 and 19200 bit/s)
 *
 *    2400 baud QAM on an 1800 Hz carrier in both directions, separated
 *    by an echo canceller.  4800 bit/s is differentially coded QPSK,
 *    7200 to 19200 bit/s use the 8 state trellis code.  V.32terbo is
 *    V.32bis with two more rates: the rate word's reserved bits B13
 *    (16800) and B14 (19200), 256 and 512 point constellations sent
 *    through a non-linear encoder (docs/dsp/v32terbo.md).
 *
 *    ZyXEL's ZyX modes (DP_ZYX, docs/dsp/zyxel.md) are V.32bis after AA,
 *    AC and CC at 2400 x 8/7 = 2742.857 baud: ZyX 19200 7 bits a symbol
 *    through the trellis code, ZyX 16800 to 7200 an 8 symbol frame with
 *    a ring table (zyx.h), on a 12800/7 = 1828.571 Hz carrier (traced in
 *    the Elite 2864I's ZyX DSP program).  ZyXEL's own detection of the
 *    peer and its scrambler at the new rate are not known yet: D-Modem's
 *    ZyX takes the peer as a ZyX modem and keeps V.32bis's scramblers.
 *    Thus it connects D-Modem to D-Modem, and not yet to a ZyXEL.
 *
 *    Start up sequence (as sent by the original library):
 *
 *    answerer: ANS, AC, CA (on AA), AC (on CC), silence,
 *              S S-bar TRN R1 ... (until the caller's S), silence,
 *              S S-bar TRN R3 ... (after R2, until E), E, data
 *    caller:   AA (after ANS), CC (64 symbols after AC -> CA),
 *              silence (on CA -> AC),
 *              S S-bar TRN R2 ... (after R1, until R3), E, data
 *
 *    TRN is 256 symbols of A/C (one scrambled bit per symbol) followed
 *    by A/B/C/D (two bits per symbol).  The rate words R1, R2, R3 and
 *    E are sent scrambled with the 4800 bit/s coding, most significant
 *    bit first.  The caller scrambles with 1 + x^-18 + x^-23, the
 *    answerer with 1 + x^-5 + x^-23.
 *
 *    A rate renegotiation (8.5/V.32bis) in data, either modem first:
 *
 *    initiator: S S-bar TRN R4 ... (until R5), E, data
 *    responder: (on S) S S-bar TRN R5 ... (after R4, until E), E, data
 *
 *    the equalizers and the echo canceller as data left them; a modem
 *    that answers with a retrain (AA or AC) is followed.
 *
 *    When the remote modem does not do V.32 the pump falls back to
 *    V.22bis (answerer: no AA after the answer tone, caller: V.22
 *    unscrambled ones instead of AC).
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
#include "qam_rx_core.h"
#include "rate_table.h"
#include "zyx.h"

namespace dsp {

#define V32_BAUD        2400
/* ZyX: 2400 x 8/7 (the firmware scales its round trip delay by
   0x4924 / 0x4000), its roll off a guess that keeps the band in the
   telephone channel's */
#define ZYX_BAUD        (2400.0f * 8 / 7)
/* its carrier, 2/3 of its symbol rate (240 degrees a symbol, the high
   carrier of V.34 at 2743 baud): the Elite 2864I's ZyX DSP program
   (docs/dsp/zyxel-details.md) */
#define ZYX_CARRIER     (12800.0f / 7)
#define ZYX_ALPHA       0.20f
#define ZYX_SPAN        8
#define V32_ALPHA       0.25f
#define V32_SPAN        6
#define V32_EQ_LEN      32
#define V32_POWER       40.0f   /* mean power of all the constellations */
#define V32_TRN_LEN     2048    /* 4 point TRN symbols we send */
#define V32_EC_LEN      128
/* the original library sends all V.32 signals at about 2000 rms; its
   fixed point receiver does not cope with much more */
#define V32_TX_AMPLITUDE (2000.0f * 1.41421f)

/* rate word bits, B0 is the most significant bit */
#define RB_HEADER       0xF000  /* 0000: R1/R2/R3, 1111: E */
#define RB_SYNC         0x0111
#define RB_B4           0x0800
#define RB_4800         0x0400
#define RB_9600         0x0200
#define RB_TCM          0x0080
#define RB_7200         0x0040
#define RB_12000        0x0020
#define RB_14400        0x0008
#define RB_16800        0x0004  /* V.32terbo (B13, reserved in V.32bis) */
#define RB_19200        0x0002  /* V.32terbo (B14, reserved in V.32bis) */
#define RB_TERBO        (RB_16800 | RB_19200)
#define RB_RATES        (RB_4800 | RB_9600 | RB_TCM | RB_7200 | RB_12000 | \
			 RB_14400 | RB_TERBO)

struct v32_rate {
	s32 bps;
	u16 bits;
	s16 nbits;
	const cplx *points;
	const ZyxFrameRate *frame = nullptr;    /* ZyX 16800 to 7200's */
};

static const struct v32_rate v32_rates[] = {
	{ 19200, RB_TCM | RB_19200, 8, v32_points_19200.p },
	{ 16800, RB_TCM | RB_16800, 7, v32_points_16800.p },
	{ 14400, RB_TCM | RB_14400, 6, v32_points_14400 },
	{ 12000, RB_TCM | RB_12000, 5, v32_points_12000 },
	{ 9600, RB_TCM | RB_9600, 4, v32_points_9600 },
	{ 7200, RB_TCM | RB_7200, 3, v32_points_7200 },
	{ 4800, RB_4800, 2, v32_points_4800 },
};
#define V32_NRATES (s16)std::size(v32_rates)

/* ZyX's (all at ZYX_BAUD, no 4800): the bits of the rate words as
   V.32terbo's, nbits the trellis encoder's bits a symbol */
static cplx zyx_19200_s[256], zyx_16800_s[256], zyx_14400_s[128],
	zyx_12000_s[64], zyx_9600_s[32], zyx_7200_s[32];
static const struct v32_rate zyx_rates[] = {
	{ 19200, RB_TCM | RB_19200, 7, zyx_19200_s },
	{ 16800, RB_TCM | RB_16800, 7, zyx_16800_s, &zyx_16800 },
	{ 14400, RB_TCM | RB_14400, 6, zyx_14400_s, &zyx_14400 },
	{ 12000, RB_TCM | RB_12000, 5, zyx_12000_s, &zyx_12000 },
	{ 9600, RB_TCM | RB_9600, 4, zyx_9600_s, &zyx_9600 },
	{ 7200, RB_TCM | RB_7200, 4, zyx_7200_s, &zyx_7200 },
};
#define ZYX_NRATES (s16)std::size(zyx_rates)

/* ZyX's constellations scaled to V32_POWER, ZyX 16800 to 7200's at the
   mean power of their rings as the ring table's rows give them (the
   inner rings the more frequent); once */
static void zyx_scale_tables()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	const cplx *src[] = { zyx_points_19200, zyx_points_256,
			      zyx_points_128, zyx_points_64, zyx_points_32,
			      zyx_points_32 };
	for (s16 i = 0; i < ZYX_NRATES; i++) {
		const struct v32_rate *r = &zyx_rates[i];
		s32 n = 1 << (r->nbits + 1);
		double ring_p[4] = { 0.25, 0.25, 0.25, 0.25 };
		if (r->frame) {
			s32 rows = 1 << r->frame->k;
			double c[4] = { 0, 0, 0, 0 };
			for (s32 j = 0; j < rows; j++)
				for (s16 k = 0; k < ZYX_FRAME; k++)
					c[(zyx_rings[j] >> (2 * k)) & 3] += 1;
			for (s16 g = 0; g < 4; g++)
				ring_p[g] = c[g] / (rows * ZYX_FRAME);
		}
		/* each ring a quarter of the points: label bits 3-4 */
		double pw = 0;
		for (s32 j = 0; j < n; j++)
			pw += ring_p[(j >> 3) & 3] * 4 / n *
				(src[i][j].re * src[i][j].re +
				 src[i][j].im * src[i][j].im);
		float g = static_cast<float>(sqrt(V32_POWER / pw));
		cplx *dst = const_cast<cplx *>(r->points);
		for (s32 j = 0; j < n; j++)
			dst[j] = { src[i][j].re * g, src[i][j].im * g };
	}
}

/* 4800 bit/s differential code: next point index from previous index
   and dibit (first bit in bit 0); points indexed as v32_points_4800 */
static const unsigned char v32_4800_enc[4][4] = {
	{ 2, 3, 0, 1 }, { 0, 2, 1, 3 }, { 3, 1, 2, 0 }, { 1, 0, 3, 2 },
};
/* A B C D -> index in v32_points_4800 */
static const s16 abcd_to_4800[4] = { 0, 2, 3, 1 };
/* TRN dibit (first bit << 1 | second bit) -> A B C D */
static const s16 trn_dibit_abcd[4] = { 0, 1, 3, 2 };

enum v32_txm {
	TXM_SILENCE,
	TXM_ANS,
	TXM_AA,
	TXM_CC,
	TXM_AC,
	TXM_S,
	TXM_SBAR,
	TXM_TRN2,
	TXM_TRN4,
	TXM_R,
	TXM_E,
	TXM_DATA,
	/* ZyXEL's peer markers (S115, docs/dsp/zyxel-details.md): A and C
	   in turn every 2 symbols (the caller's: lines at 1200 and 2400 Hz)
	   or every 4 (the answerer's: 1500 and 2100 Hz) */
	TXM_ZMARK_C,
	TXM_ZMARK_A,
};

enum v32_state {
	/* answerer */
	A_SILENCE,
	A_ANS,
	A_GAP,
	A_AC,
	A_CA,
	A_AC2,
	A_QUIET,
	A_ZMARK,                /* our ZyXEL marker, then ZyX */
	A_TRAIN1,               /* S S-bar TRN */
	A_R1,
	A_WAIT_R2,
	A_TRAIN2,
	A_R3,
	A_E,
	/* caller */
	C_AA,
	C_AA_DELAY,
	C_CC,
	C_ZMARK,                /* our ZyXEL marker, listening */
	C_WAIT_R1,
	C_TRAIN,
	C_R2,
	C_E,
	/* a rate renegotiation (8.5/V.32bis), either modem */
	RN_TRAIN,               /* S S-bar TRN */
	RN_R,                   /* R4 (initiator) or R5 (responder) */
	RN_E,
	/* both */
	V32_DATA,
};

/* receiver phases */
enum v32_rxp {
	RX_IDLE,                /* waiting for S */
	RX_S,
	RX_TRN,                 /* S-bar seen: training, looking for R */
	RX_R,                   /* R seen, looking for E (or the next R) */
	RX_DATA,
};

struct V32Pump : PumpBase<V32Pump, v32_state>, QamRxCore<V32Pump> {
	static constexpr const char *name = "V.32/V.32bis data pump";

	/* frees the receiver's and echo canceller's buffers */
	~V32Pump() { cleanup(); }

	s16 automode;
	u16 caps;               /* rate bits we offer */
	u16 r2_caps;            /* caller: rates in R2 */

	/* transmitter */
	QamTx tx;
	enum v32_txm txm;
	s32 tx_syms;            /* symbols sent in the current tx state */
	s16 tx_ac_phase;
	s16 tx_reverse;         /* repeat one AC symbol: phase reversal */
	s16 ca_rev;             /* answerer: the caller's CC reversal seen */
	s32 nt_syms = 0;        /* caller: the round trip delay NT (symbols) */
	u32 tx_scr;
	s16 tx_scr_tap;
	s16 tx_4800;            /* 4800 coding state (v32_points_4800 index) */
	s16 tx_last_abcd;
	u16 tx_word;
	s16 tx_word_bit;
	TrellisEnc tx_trellis;
	const struct v32_rate *tx_rate;
	Osc ans;
	float last_tx;

	/* receiver */
	EchoCanceller ec;
	float ec_mu;
	ToneTrk aa, ac, usb1, ans2100;
	/* ZyXEL's peer markers: their lines; we send ours (S115 1: the
	   V.32bis and ZyX pumps); listening for the remote's, the symbols
	   it has been heard */
	ToneTrk z1200, z2400, z1500, z2100;
	bool zpeer = false;
	bool zlisten = false;
	s32 zseen = 0;
	ToneTrk v21m, v23m;     /* FSK answerers' marks */
	enum v32_rxp rxp;
	s32 run_s;
	s16 last_sgn, s_miss;
	cplx last_z;
	u32 rx_scr;
	s16 rx_scr_tap;
	s16 rx_4800;
	u32 rx_bits;            /* last 32 descrambled bits */
	s32 rx_nbits;
	u16 rx_word, rx_r_word;
	s16 rx_word_align;
	float rx_mse;
	float data_power;       /* the input's power as data mode began */
	s16 rx_e_seen;
	const struct v32_rate *rx_rate;
	Viterbi vit;
	s16 rx_dbits;           /* data bits per symbol */
	/* B1 (the remote's scrambled ones as data begins) known: the
	   remote's scrambler register, its trellis encoder in each of
	   its 32 states (not standardized) and their errors so far, the
	   one chosen (-1: not yet), B1's symbols received */
	u32 b1_scr;
	TrellisEnc b1_enc[32];
	float b1_err[32];
	s16 b1_best;
	s16 b1_syms;
	s16 agc_free;           /* symbols the AGC still follows the input
				   after its level came back */

	/* events from the receiver to the handshake */
	s16 ev_aa, ev_ac, ev_rev, ev_s, ev_r, ev_e, ev_usb1;
	s16 retraining;         /* a retrain in data mode (no new CONNECT) */
	s16 retrains;
	s16 reneg;              /* a rate renegotiation: 1 ours, 2 the
				   remote's (0: none) */
	s16 renegs;
	s32 reneg_end;          /* its time limit (samples) */
	u16 rn_word;            /* the R4 or R5 we send */
	s32 reneg_at;           /* test: our own renegotiation this far
				   into the data, $DSP_V32_RENEG ms ... */
	s32 reneg_to = 0;       /* ... to this rate at most (bit/s) */
	s32 retrain_at;         /* test: our own retrain this far (samples)
				   into the data, $DSP_V32_RETRAIN ms */
	float lost_mse;         /* the data mse past which the receiver has
				   lost its equalization (its rate's) ... */
	s32 lost_syms;          /* ... symbols in a row past it */
	s16 lost_off;           /* (tests: none of that) */
	u16 r_word;             /* last R word received */
	enum DP_ID v32_id;      /* DP_V32, DP_V32BIS, DP_V32TERBO or DP_ZYX */
	/* ZyX: its rates, at ZYX_BAUD after AA, AC and CC; the frames of
	   ZyX 16800 to 7200: the symbol in the frame we send, receive, the
	   data bits of the frame's symbols (the remote's B1 frame's) */
	bool zyx = false;
	float baud = V32_BAUD;
	s16 tx_fpos = 0, rx_fpos = 0;
	s16 tx_fq[ZYX_FRAME], rx_fq[ZYX_FRAME], b1_fq[ZYX_FRAME];

	/* our rate table (V.32's or ZyX's), its length */
	const struct v32_rate *rates() const
	{
		return zyx ? zyx_rates : v32_rates;
	}
	/* (and its length) */
	s16 nrates() const { return zyx ? ZYX_NRATES : V32_NRATES; }
	const struct v32_rate *rate_of_word(u16 w) const;
	void set_baud(float b);
	void zyx_switch(const char *why);
	bool zmark_heard(bool from_caller);
	/* the carrier at the symbol rate now: 1800 Hz, ZyX's at
	   ZYX_BAUD */
	float line_carrier() const {
		return baud == V32_BAUD ? 1800.0f : ZYX_CARRIER;
	}
	void zyx_frame(u32 *scr, s16 tap, const struct v32_rate *r, bool ones,
		       s16 *q);

	u16 offered_caps();

	cplx tx_symbol();
	void set_tx(enum v32_txm m);
	cplx tx_word_symbol(u16 word, s16 data);
	cplx tx_data_symbol();
	void rx_reset();
	void b1_start();
	bool b1_point(s16 n, cplx z, cplx *d);
	void rx_word_bit(s16 bit);
	void rx_symbol();
	void rx_sample(float x);
	/* (QamRxCore's) the far end's carrier come, gone */
	void carrier_came();
	void carrier_went();
	/* its name in the log */
	const char *tag() const { return "v32"; }
	/* in data mode (the carrier watched, no handshake timeout) */
	bool in_data() const { return state == V32_DATA; }
	void start_word(enum v32_txm m, u16 w);
	s16 connect();
	void retrain(bool remote);
	void renegotiate(bool remote);
	/* the remote's retrain: AA alone from a caller, AC alone from an
	   answerer (S, a renegotiation's, has both tones; scrambled data
	   neither) */
	bool remote_retrain() const
	{
		return caller ? ev_ac && !ev_aa : ev_aa && !ev_ac;
	}
	/* the remote's S watched for in data (a renegotiation) */
	bool s_watch() const
	{
		return state == V32_DATA || (reneg == 1 && rxp == RX_DATA);
	}
	s16 fallback_to(enum DP_ID id, const char *mode);
	s16 fallback();
	u16 quality_caps();
	float lost_limit(const struct v32_rate *r);
	const struct v32_rate *choose(u16 w);
	s16 handshake();
	cplx abcd_point(s16 k);
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
 *    helpers
 */

/* the index (A B C D: 0..3) of the training point nearest to z */
static s16 nearest_abcd(cplx z)
{
	return nearest_point(v32_abcd, 4, z);
}

/* the fastest rate (of ours: V.32's or ZyX's) whose bits are all set in
   the rate word w, or NULL */
const struct v32_rate *V32Pump::rate_of_word(u16 w) const
{
	const struct v32_rate *t = rates();
	for (s16 i = 0; i < nrates(); i++)
		if ((w & t[i].bits) == t[i].bits)
			return &t[i];
	return nullptr;
}

/* the transmitter and the receiver at b baud (ZyX: V32_BAUD for AA, AC
   and CC, ZYX_BAUD from S); the receiver from its start */
void V32Pump::set_baud(float b)
{
	if (b == baud)
		return;
	baud = b;
	bool z = b != V32_BAUD;
	float t = tx.t;
	tx.init(srate, line_carrier(), b, z ? ZYX_ALPHA : V32_ALPHA,
		z ? ZYX_SPAN : V32_SPAN, sqrtf(V32_POWER), V32_TX_AMPLITUDE);
	tx.t = t;
	rx.release();
	rx.init(srate, line_carrier(), b, z ? ZYX_ALPHA : V32_ALPHA,
		z ? ZYX_SPAN : V32_SPAN, V32_EQ_LEN, V32_POWER);
	DSP_DBG("v32: %.3f baud\n", b);
}

/* the remote is a ZyXEL (why): ZyX's rates from now on, reported as
   DP_ZYX; the symbol rate changes where the start-up has it */
void V32Pump::zyx_switch(const char *why)
{
	zyx = true;
	zyx_scale_tables();
	caps = offered_caps();
	stat_dp(DP_ZYX);
	DSP_DBG("v32: a ZyXEL %s: ZyX\n", why);
}

/* the remote's ZyXEL marker (the caller's, from_caller, or the
   answerer's) heard for 24 symbols: both its lines, no AA or AC */
bool V32Pump::zmark_heard(bool from_caller)
{
	bool on = from_caller ? z1200.on && z2400.on : z1500.on && z2100.on;
	if (on && !aa.on && !ac.on)
		zseen++;
	else
		zseen = 0;
	return zseen >= 24;
}

/* the next ZyX frame of rate r into q[] (each symbol's bits for the
   trellis encoder): its 7N bits from the link layer (ones: B1)
   scrambled with *scr in their order */
void V32Pump::zyx_frame(u32 *scr, s16 tap, const struct v32_rate *r,
			bool ones, s16 *q)
{
	u8 bits[ZYX_MAX_BITS];
	s16 n = r->frame->frame_bits();
	if (ones)
		std::fill_n(bits, n, 1);
	else
		modem_get_bits(modem, 1, bits, n);
	for (s16 k = 0; k < n; k++)
		bits[k] = static_cast<u8>(scramble(scr, tap, bits[k] & 1));
	r->frame->encode(bits, q);
}

/* the rate word naming r alone (R3, and E with RB_HEADER added) */
static u16 final_word(const struct v32_rate *r)
{
	return RB_B4 | RB_SYNC | r->bits;
}

/*
 *    transmitter (runs at the symbol rate)
 */

/* transmit signal m from its first symbol (tx_syms 0) */
void V32Pump::set_tx(enum v32_txm m)
{
	txm = m;
	tx_syms = 0;
}

/* training point k (A B C D: 0..3), remembered for the 4800 coding
   state that follows TRN */
cplx V32Pump::abcd_point(s16 k)
{
	tx_last_abcd = k;
	return v32_abcd[k];
}

/* B1: the data mode starts with scrambled binary ones for 128 symbols
   (V.32bis 5.4), the remote's receiver may train on them */
#define V32_B1_SYMS 128

/* one 4800 bit/s symbol (two scrambled bits, differentially coded):
   the next two bits of the rate word word (MSB first), or with data two
   bits from the link layer (ones during B1) */
cplx V32Pump::tx_word_symbol(u16 word, s16 data)
{
	s16 b[2], k, q;
	for (k = 0; k < 2; k++) {
		s16 bit;
		if (data) {
			u8 x = 1;
			if (tx_syms >= V32_B1_SYMS)
				modem_get_bits(modem, 1, &x, 1);
			bit = x & 1;
		} else {
			bit = (word >> (15 - tx_word_bit)) & 1;
			tx_word_bit = (tx_word_bit + 1) & 15;
		}
		b[k] = scramble(&tx_scr, tx_scr_tap, bit);
	}
	q = b[0] | b[1] << 1;
	tx_4800 = v32_4800_enc[tx_4800][q];
	return v32_points_4800[tx_4800];
}

/* one data symbol at tx_rate: nbits from the link layer (ones during
   B1), scrambled, through the trellis encoder (7200 and up) */
cplx V32Pump::tx_data_symbol()
{
	const struct v32_rate *r = tx_rate;
	s16 q = 0, k, idx;
	u8 bits[8];

	if (r->nbits == 2)
		return tx_word_symbol(0, 1);
	if (r->frame) {
		/* ZyX 16800 to 7200: a frame of 8 symbols at a time */
		if (tx_fpos == 0)
			zyx_frame(&tx_scr, tx_scr_tap, r, tx_syms < V32_B1_SYMS,
				  tx_fq);
		q = tx_fq[tx_fpos];
		tx_fpos = (tx_fpos + 1) % ZYX_FRAME;
		return r->points[tx_trellis.encode(q, r->nbits)];
	}
	if (tx_syms < V32_B1_SYMS)
		std::fill_n(bits, r->nbits, 1);
	else
		modem_get_bits(modem, 1, bits, r->nbits);
	for (k = 0; k < r->nbits; k++)
		q |= scramble(&tx_scr, tx_scr_tap, bits[k] & 1) << k;
	idx = tx_trellis.encode(q, r->nbits);
	return r->points[idx];
}

/* the next symbol of the current transmit signal (for tx.sample()):
   AA, CC, AC/CA (with tx_reverse for a phase reversal),
   S, S-bar, TRN, the rate words or data; counts tx_syms */
cplx V32Pump::tx_symbol()
{
	cplx z = { 0, 0 };
	s16 b1, b2;

	switch (txm) {
	case TXM_SILENCE:
	case TXM_ANS:
		break;
	case TXM_AA:
		z = abcd_point(0);
		break;
	case TXM_CC:
		z = abcd_point(2);
		break;
	case TXM_AC:
		if (tx_reverse)
			tx_reverse = 0;
		else
			tx_ac_phase ^= 1;
		z = abcd_point(tx_ac_phase ? 2 : 0);
		break;
	case TXM_ZMARK_C:
		z = abcd_point((tx_syms >> 1) & 1 ? 2 : 0);
		break;
	case TXM_ZMARK_A:
		z = abcd_point((tx_syms >> 2) & 1 ? 2 : 0);
		break;
	case TXM_S:
		z = abcd_point(tx_syms & 1);
		break;
	case TXM_SBAR:
		z = abcd_point(2 + (tx_syms & 1));
		break;
	case TXM_TRN2:
		z = abcd_point(scramble(&tx_scr, tx_scr_tap, 1) ? 2 : 0);
		break;
	case TXM_TRN4:
		b1 = scramble(&tx_scr, tx_scr_tap, 1);
		b2 = scramble(&tx_scr, tx_scr_tap, 1);
		z = abcd_point(trn_dibit_abcd[b1 << 1 | b2]);
		tx_4800 = abcd_to_4800[tx_last_abcd];
		break;
	case TXM_R:
	case TXM_E:
		z = tx_word_symbol(tx_word, 0);
		break;
	case TXM_DATA:
		z = tx_data_symbol();
		break;
	}
	tx_syms++;
	return z;
}

/*
 *    receiver
 */

/* the receiver back to waiting for S (equalizer, PLL and word state) */
void V32Pump::rx_reset()
{
	qam_rx_reset();
	agc_free = 0;
	rxp = RX_IDLE;
	run_s = 0;
	rx_nbits = 0;
	rx_e_seen = 0;
}

/* data begins (E received): the remote sends B1, scrambled ones, for
   128 symbols, from the scrambler register our descrambler has now;
   its trellis encoder's state is not known, all 32 are followed */
void V32Pump::b1_start()
{
	s16 i;
	b1_scr = rx_scr;
	for (i = 0; i < 32; i++) {
		b1_enc[i].diff = i & 3;
		b1_enc[i].conv = i >> 2;
		b1_err[i] = 0;
	}
	b1_best = -1;
	b1_syms = 0;
	rx_fpos = 0;
}

/* during B1 the point the remote sent for the received z (n data bits a
   symbol) into *d: once its encoder's state is known (the one of the 32
   nearest z over the first 8 symbols, if near enough); true when *d is
   that point */
bool V32Pump::b1_point(s16 n, cplx z, cplx *d)
{
	const cplx *pts = rx_rate->points;
	s16 q = 0, k, i;

	if (b1_syms >= V32_B1_SYMS)
		return false;
	if (rx_rate->frame) {
		if (b1_syms % ZYX_FRAME == 0)
			zyx_frame(&b1_scr, rx_scr_tap, rx_rate, true, b1_fq);
		q = b1_fq[b1_syms % ZYX_FRAME];
	} else
		for (k = 0; k < n; k++)
			q |= scramble(&b1_scr, rx_scr_tap, 1) << k;
	b1_syms++;
	if (b1_best >= 0) {
		*d = pts[b1_enc[b1_best].encode(q, n)];
		return true;
	}
	for (i = 0; i < 32; i++)
		b1_err[i] += cplx_pow(cplx_sub(z, pts[b1_enc[i].encode(q, n)]));
	if (b1_syms < 8)
		return false;
	for (i = 1, k = 0; i < 32; i++)
		if (b1_err[i] < b1_err[k])
			k = i;
	/* not B1 (or not where we expect it): the decisions alone */
	if (b1_err[k] > 8 * 0.05f * V32_POWER) {
		DSP_DBG("v32: no B1 (error %.3f)\n",
			b1_err[k] / (8 * V32_POWER));
		b1_syms = V32_B1_SYMS;
		return false;
	}
	b1_best = k;
	DSP_DBG("v32: B1 found, trellis state %d (error %.4f)\n", k,
		b1_err[k] / (8 * V32_POWER));
	return false;
}

/* one descrambled bit of the 4800 bit/s rate word stream: a new R word
   (seen twice in a row) sets ev_r and r_word; E on the same 16 bit
   boundaries, with one rate and a clean signal, sets ev_e and starts
   the data receiver at that rate */
void V32Pump::rx_word_bit(s16 bit)
{
	u16 w;

	rx_bits = (rx_bits << 1) | (bit & 1);
	if (++rx_nbits < 32)
		return;
	w = rx_bits & 0xffff;
	if ((w & RB_SYNC) != RB_SYNC)
		return;
	if ((w & RB_HEADER) == 0 && (rx_bits >> 16) == w &&
	    (w & RB_RATES) && rxp == RX_R && w == rx_r_word) {
		/* E has to come on the same 16 bit boundaries as R; follow
		   them, a clock offset can slip a symbol */
		rx_word_align = rx_nbits & 15;
	} else if ((w & RB_HEADER) == 0 && (rx_bits >> 16) == w &&
	    (w & RB_RATES) && w != rx_r_word) {
		rx_word_align = rx_nbits & 15;
		rx_r_word = w;
		r_word = w;
		DSP_DBG("v32: rx quality: mse %.4f (%.1f dB)\n", rx_mse,
			-10 * log10f(rx_mse + 1e-6f));
		ev_r = 1;
		rxp = RX_R;
		DSP_DBG("v32: R word %04X\n", w);
	} else if (rxp == RX_R && (w & RB_HEADER) == RB_HEADER &&
		   (rx_nbits & 15) == rx_word_align &&
		   rate_of_word(w) && (w & RB_RATES) == rate_of_word(w)->bits &&
		   rx_mse < 0.1f && !rx_e_seen) {
		rx_e_seen = 1;
		rx_rate = rate_of_word(w);
		ev_e = 1;
		rxp = RX_DATA;
		vit.init(rx_rate->points, rx_rate->nbits);
		lost_mse = lost_limit(rx_rate);
		lost_syms = 0;
		/* trained: slow timing loop, its jitter hurts the dense
		   constellations */
		rx.kt_p = 0.004f * rx.sps;
		rx.kt_gi = 0.003f;
		/* and the gain held: the AGC's power estimate on a dense
		   constellation jitters by a few per cent, a radial error
		   of the outer points the size of the 512 point
		   constellation's spacing (the equalizer follows slow
		   changes) */
		rx.agc_hold = 1;
		DSP_DBG("v32: E word %04X, rx rate %" PRId32 "\n", w,
			rx_rate->bps);
	}
}

/* one received symbol (rx.z): ignored while we train ourselves; S and
   S-bar detection, training on the ABCD grid with the R / E word
   decoding, or data decisions (4800 differential or Viterbi), adaption
   and the descrambled bits to the link layer once in V32_DATA */
void V32Pump::rx_symbol()
{
	cplx z = rx.z, d, st;
	s16 k, q, sgn;
	float a;

	/* while we send our own training the line carries mostly our echo
	   (the echo canceller is still learning): don't listen */
	if (!reneg && (txm == TXM_S || txm == TXM_SBAR ||
		       txm == TXM_TRN2 || txm == TXM_TRN4)) {
		run_s = 0;
		last_z = z;
		return;
	}

	/* signal quality: mean squared distance to the ABCD grid */
	if (rxp != RX_DATA) {
		k = nearest_abcd(z);
		rx_mse += 0.05f * (cplx_pow(cplx_sub(z, v32_abcd[k])) /
				      V32_POWER - rx_mse);
	}

	/* S: steps of +-90 degrees with alternating sign; the first
	   S-bar symbol (C) repeats the sign.  S can start over at any
	   time before data (a new training), or in data (the remote's
	   rate renegotiation). */
	if (rxp != RX_DATA || s_watch()) {
		st = cplx_mulc(z, last_z);
		a = atan2f(st.im, st.re);
		sgn = a > 0 ? 1 : -1;
		if (fabsf(a) > 0.9f && fabsf(a) < 2.2f && carrier.on) {
			if (sgn != last_sgn) {
				if (++run_s >= 24 && rxp != RX_S) {
					/* whatever was learned on silence or
					   noise is useless: start over (not
					   in a renegotiation: the equalizer
					   goes on from data's) */
					if (rxp != RX_IDLE && rxp != RX_DATA &&
					    !reneg)
						rx.reset();
					rx_e_seen = 0;
					rx.agc_hold = 0;
					rxp = RX_S;
					ev_s = 1;
					DSP_DBG("v32: S detected\n");
				}
			} else if (rxp == RX_S && run_s >= 24) {
				/* S-bar: this symbol is C */
				rx.align_phase(v32_abcd[2]);
				rxp = RX_TRN;
				rx_4800 = abcd_to_4800[2];
				rx_nbits = 0;
				rx_r_word = 0;
				run_s = 0;
				DSP_DBG("v32: S-bar detected\n");
			} else
				run_s = 0;
		} else
			run_s = 0;
		last_sgn = sgn;
		/* noise can hide the S to S-bar step: once S is clearly over,
		   go on with the training anyway (everything after it is
		   differential, the absolute phase is not needed) */
		if (rxp == RX_S) {
			if (run_s >= 24)
				s_miss = 0;
			else if (++s_miss >= 24) {
				rxp = RX_TRN;
				rx_nbits = 0;
				rx_r_word = 0;
				DSP_DBG("v32: end of S\n");
			}
		}
	}

	switch (rxp) {
	case RX_IDLE:
	case RX_S:
		/* keep the phase locked on the ABCD grid, no eq training on
		   the two tone S signal */
		k = nearest_abcd(z);
		rx.adapt(v32_abcd[k], 0, 0.05f, 0);
		break;
	case RX_TRN:
	case RX_R:
		k = nearest_abcd(z);
		/* no training on noise while the remote is silent */
		if (rx_mse < 0.15f)
			rx.adapt(v32_abcd[k], 0.02f, 0.05f, 0.0005f);
		else
			rx.adapt(v32_abcd[k], 0.002f, 0.05f, 0);
		/* 4800 bit/s decoding for the R and E words */
		q = abcd_to_4800[k];
		for (k = 0; k < 4; k++)
			if (v32_4800_enc[rx_4800][k] == q)
				break;
		rx_4800 = q;
		rx_word_bit(descramble(&rx_scr, rx_scr_tap, k & 1));
		rx_word_bit(descramble(&rx_scr, rx_scr_tap, k >> 1));
		if (rxp == RX_DATA) {
			vit.last_y12 = 0;
			b1_start();
		}
		break;
	case RX_DATA:
		{
			const struct v32_rate *r = rx_rate;
			u8 bits[ZYX_MAX_BITS];
			s16 n = r->nbits;
			k = nearest_point(r->points, 1 << (n == 2 ? 2 : n + 1), z);
			d = r->points[k];
			/* B1: train on the points the remote sends */
			bool b1 = n > 2 && b1_point(n, z, &d);
			/* the gain held only while the level stays within 3 dB
			   of data's: through a burst of noise or a level step
			   the AGC follows, and for 100 ms after */
			if (state == V32_DATA && data_power > 0) {
				if (rx.power > 2 * data_power ||
				    rx.power < 0.5f * data_power) {
					rx.agc_hold = 0;
					agc_free = static_cast<s16>(baud / 10);
				} else if (agc_free > 0 && --agc_free == 0)
					rx.agc_hold = 1;
			}
			rx_mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / V32_POWER -
					      rx_mse);
			if (rx_mse > lost_mse)
				lost_syms++;
			else
				lost_syms = 0;
			if (rx.symbols % 50 == 0)
				DSP_TRACE("%s data mse %.4f t %.3f tadj %.5f th %.2f dth %.5f\n",
					  caller ? "C" : "A", rx_mse, rx.t_next,
					  rx.t_adj, rx.theta, rx.dtheta);
			rx.adapt(d, b1 ? 0.01f : 0.004f, 0.02f, 0.0002f);
			if (n == 2) {
				for (q = 0; q < 4; q++)
					if (v32_4800_enc[rx_4800][q] == k)
						break;
				rx_4800 = k;
			} else
				q = vit.decode(z, nullptr);
			if (q >= 0 && r->frame) {
				/* ZyX 16800 to 7200: the frame's 7N bits once
				   its 8 symbols are in */
				rx_fq[rx_fpos++] = q;
				if (rx_fpos == ZYX_FRAME) {
					rx_fpos = 0;
					s16 nb = r->frame->frame_bits();
					if (!r->frame->decode(rx_fq, bits))
						DSP_DBG("v32: ZyX frame: its rings "
							"are no row\n");
					for (k = 0; k < nb; k++)
						bits[k] = static_cast<u8>(descramble(
							&rx_scr, rx_scr_tap, bits[k]));
					if (state == V32_DATA)
						modem_put_bits(modem, 1, bits, nb);
				}
			} else if (q >= 0 && state == V32_DATA) {
				for (k = 0; k < n; k++)
					bits[k] = descramble(&rx_scr,
							     rx_scr_tap, q >> k);
				modem_put_bits(modem, 1, bits, n);
			} else if (q >= 0)
				for (k = 0; k < n; k++)
					descramble(&rx_scr, rx_scr_tap, q >> k);
		}
		break;
	}
	last_z = z;
}

/* one received sample x: echo canceller, the tone trackers (AA / AC
   and their reversals, V.22 unscrambled ones, 2100 Hz, FSK marks) as
   ev_* events, the QAM receiver and the carrier detector */
void V32Pump::rx_sample(float x)
{
	x = ec.run(last_tx, x, ec_mu);

	if (ac.run(x, rx.power, srate) && caller)
		ev_rev = 1;
	if (aa.run(x, rx.power, srate) && !caller)
		ev_rev = 1;
	usb1.run(x, rx.power, srate);
	ans2100.run(x, rx.power, srate);
	if (zlisten)
		for (ToneTrk *t : { &z1200, &z2400, &z1500, &z2100 })
			t->run(x, rx.power, srate);
	if (caller && automode) {
		v21m.run(x, rx.power, srate);
		v23m.run(x, rx.power, srate);
	}
	ev_ac = ac.on;
	ev_aa = aa.on;
	/* V.22 unscrambled ones (2250 Hz), not the 2100 Hz answer tone */
	ev_usb1 = usb1.on && !ac.on &&
		cplx_pow(usb1.lp2) > 4 * cplx_pow(ans2100.lp2);

	qam_rx(x);
}

/* the far end's carrier come (QamRxCore): the receiver from its start if
   it waits for S */
void V32Pump::carrier_came()
{
	if (rxp == RX_IDLE)
		rx.reset();
}

/* and gone: back to waiting for S, but in data (S10 times the loss) */
void V32Pump::carrier_went()
{
	if (rxp != RX_DATA)
		rx_reset();
}

/*
 *    handshake
 */

/* transmit signal m (TXM_R or TXM_E) repeating the word w from its
   first bit */
void V32Pump::start_word(enum v32_txm m, u16 w)
{
	tx_word = w;
	tx_word_bit = 0;
	set_tx(m);
}

/* to data, the rates to the core: DPSTAT_CONNECT, or DPSTAT_OK after a
   retrain */
s16 V32Pump::connect()
{
	set_state(V32_DATA);
	data_power = rx.power;
	if (retraining) {
		/* the link goes on (V.42 knows nothing of it) */
		set_rates(tx_rate->bps, rx_rate->bps);
		retraining = 0;
		DSP_DBG("v32: %s, %" PRId32 "/%" PRId32 "\n",
			reneg ? "renegotiated" : "retrained",
			tx_rate->bps, rx_rate->bps);
		/* S's tones (AA's and AC's) gone: data's own band near
		   1800 Hz keeps a tracker on once on */
		if (reneg)
			for (ToneTrk *t : { &aa, &ac }) {
				t->on = 0;
				t->on_count = 0;
			}
		reneg = 0;
		return DPSTAT_OK;
	}
	return connect_at(tx_rate->bps, rx_rate->bps);
}

/* a retrain (V.32 5.5): the start-up again from AA / AC, the caller's
   from AA, the answerer's from AC; whoever starts it (remote: the
   other modem), the other modem answers the same way when it hears the
   signal */
void V32Pump::retrain(bool remote)
{
	DSP_DBG("v32: retrain %d (%s)\n", ++retrains,
		caller ? "AA" : "AC");
	stat_retrain(remote);
	retraining = 1;
	reneg = 0;
	automode = 0;                /* no falling back now */
	/* the core's limits as they are now (a rate shift's cap) */
	caps = offered_caps();
	/* ZyX: AA, AC and CC at 2400 baud again */
	set_baud(V32_BAUD);
	rx_reset();
	ev_rev = ev_r = ev_e = ev_s = 0;
	ca_rev = 0;
	nt_syms = 0;
	timeout = total + ms2samples(15000, srate);
	tx_syms = 0;
	if (caller) {
		set_state(C_AA);
		set_tx(TXM_AA);
	} else {
		set_state(A_AC);
		set_tx(TXM_AC);
	}
}

/* a rate renegotiation (8.5/V.32bis), the remote's (its S heard in
   data) or ours: S, S-bar and a short TRN, the receiver's equalizer
   and the echo canceller as they are; the initiator's R4 answered by
   the responder's R5 (the rates of R4 it can take now), then E and B1
   at R5's fastest rate.  Both at once (each the other's R4 for an R5)
   end the same way.  A retrain if it is not over in 3 s, or if the
   remote retrains instead (a modem without it) */
void V32Pump::renegotiate(bool remote)
{
	DSP_DBG("v32: rate renegotiation %d (%s)\n", ++renegs,
		remote ? "remote" : "ours");
	stat_reneg(remote);
	retraining = 1;
	reneg = remote ? 2 : 1;
	/* the core's limits as they are now (a rate shift's cap) */
	caps = offered_caps();
	ev_r = ev_e = ev_s = 0;
	rn_word = 0;
	reneg_end = total + ms2samples(3000, srate);
	timeout = total + ms2samples(15000, srate);
	set_state(RN_TRAIN);
	set_tx(TXM_S);
}

/* hands over to the pump id (mode its name for the debug output); an
   answerer's answer tone is marked as sent.  DPSTAT_CHANGEDP */
s16 V32Pump::fallback_to(enum DP_ID id, const char *mode)
{
	DSP_DBG("v32: remote is not V.32, falling back to %s\n", mode);
	return hand_over(id, !caller);
}

/* fallback_to() V.22bis */
s16 V32Pump::fallback()
{
	return fallback_to(DP_V22BIS, "V.22bis");
}

/* the training SNR (dB) each rate of v32_rates[] takes.  V.32terbo's
   from back to back calls (pump_test): our training's estimate tops out
   near 31 dB; 19200 bit/s is clean from about 32 dB of line SNR (an
   estimate of 29.6), 16800 from 24 dB (25.6) */
static const float v32_min_snr[] = { 29.5f, 26.5f, 25, 22, 19, 16, 0 };
/* ZyX's, from back to back calls (pump_test, ZYX_ALPHA 0.2): each
   rate clean from about 1.5 dB under these estimates (16800 from about
   0.5 dB under: its errors come in frames), the framed rates
   (the ring table's shaping) about 3 dB better than V.32bis's of the
   same bits a symbol.  On the 1828.571 Hz carrier the estimate is
   about 1 dB lower than on 1800 Hz (19200 and 16800 down by 1 dB:
   from 27 to 32 dB of line SNR the rate chosen has no errors) */
static const float zyx_min_snr[] = { 26.5f, 26, 23, 19.5f, 18, 0 };

/* the data mse (of V32_POWER) past which a receiver at rate r has
   lost its equalization (rate_lost_limit()) */
float V32Pump::lost_limit(const struct v32_rate *r)
{
	return zyx ? rate_lost_limit(zyx_rates, zyx_min_snr, r) :
		rate_lost_limit(v32_rates, v32_min_snr, r);
}

/* the rates we offer: the pump's (V.32 9600 and 4800 alone,
   V.32terbo's two more only for DP_V32TERBO) within the core's
   limits now (AT+MS, or a rate shift's cap: MNP 10); 4800 at least */
u16 V32Pump::offered_caps()
{
	const s32 min_rate =
		static_cast<s32>(modem_get_param(modem, MDMPRM_MIN_RATE));
	const s32 max_rate =
		static_cast<s32>(modem_get_param(modem, MDMPRM_MAX_RATE));
	u16 c = 0;
	for (s16 i = 0; i < nrates(); i++) {
		const struct v32_rate *r = &rates()[i];
		if (v32_id == DP_V32 && r->bps != 9600 && r->bps != 4800)
			continue;
		/* V.32terbo's rates only when asked for (+MS=V32T; ZyX's
		   own) */
		if (v32_id != DP_V32TERBO && !zyx && (r->bits & RB_TERBO))
			continue;
		if (max_rate > 0 && r->bps > max_rate)
			continue;
		if (min_rate > 0 && r->bps < min_rate)
			continue;
		c |= r->bits;
	}
	/* (ZyX has no 4800: its slowest) */
	if (!c)
		c = zyx ? zyx_rates[ZYX_NRATES - 1].bits : RB_4800;
	return c;
}

/* the rates our receiver can take, judged by the decision error on
   the training signal (4 points) */
u16 V32Pump::quality_caps()
{
	u16 caps_ = zyx ? rate_caps(zyx_rates, zyx_min_snr, rx_mse) :
		rate_caps(v32_rates, v32_min_snr, rx_mse);
	/* TCM is shared by the trellis rates */
	if (caps_ & (RB_7200 | RB_9600 | RB_12000 | RB_14400 | RB_TERBO))
		caps_ |= RB_TCM;
	return caps & (caps_ | (zyx ? zyx_rates[ZYX_NRATES - 1].bits :
				    RB_4800));
}

/* the best common rate of our capabilities and the remote's word */
const struct v32_rate *V32Pump::choose(u16 w)
{
	return rate_of_word(w & quality_caps());
}

/* called once per transmitted symbol, before the symbol is made */
s16 V32Pump::handshake()
{
	s32 sym = tx_syms;
	const struct v32_rate *r;

	switch (state) {
	/* answering modem */
	case A_SILENCE:
		if (count >= ms2samples(ANS_SILENCE_MS, srate)) {
			set_state(A_ANS);
			set_tx(TXM_ANS);
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
			set_state(A_AC);
			set_tx(TXM_AC);
		}
		break;
	case A_AC:
		/* AC for at least 128 symbols and until AA is heard */
		if (sym >= 128 && ev_aa) {
			set_state(A_CA);
			tx_reverse = 1;
			ev_rev = 0;
		} else if (automode && count >= ms2samples(3000, srate))
			return fallback();
		break;
	case A_CA:
		/* the caller answers with CC; our AC again 64 symbols after
		   its reversal (6.2/V.32bis), as the caller's CC after ours:
		   our detector sees it about 26 symbols late.  The caller
		   measures the round trip delay from this (the Intel 537EP,
		   with our reversal at once, misses our S) */
		if (ev_rev && !ca_rev) {
			ev_rev = 0;
			ca_rev = 1;
			tx_syms = 0;
		} else if (ca_rev && sym >= 38) {
			ca_rev = 0;
			set_state(A_AC2);
			tx_reverse = 1;
			tx_syms = 0;
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case A_AC2:
		/* AC after CC's reversal until the caller's tone drops
		   (6.2/V.32bis), 64 symbols at least: the caller stops CC
		   on our reversal, and our S before it has it misses (the
		   HSF modem, with 64 symbols of AC), and never answers R1.
		   1024 at most */
		if ((sym >= 64 && !aa.on) || sym >= 1024) {
			set_state(A_QUIET);
			set_tx(TXM_SILENCE);
			/* ZyX: the training and data at ZYX_BAUD; else (V.32bis,
			   ZyX by markers) a ZyXEL caller's marker heard in the
			   quiet */
			if (zyx && !zpeer) {
				set_baud(ZYX_BAUD);
			} else if (v32_id == DP_V32BIS || v32_id == DP_ZYX) {
				zlisten = true;
				zseen = 0;
			}
		}
		break;
	case A_QUIET:
		if (zlisten) {
			if (zmark_heard(true)) {
				zlisten = false;
				DSP_DBG("v32: a ZyXEL caller's marker%s\n",
					zpeer ? "" : " (S115 0: V.32bis)");
				if (zpeer) {
					zyx_switch("caller");
					set_state(A_ZMARK);
					set_tx(TXM_ZMARK_A);
					break;
				}
			} else if (sym < (zpeer ? 96 : 16) ||
				   ((z1200.on || z2400.on) && sym < 256)) {
				/* (its lines may be coming: with S115 0 the
				   quiet is V.32's own, unless they are on) */
				break;
			}
			zlisten = false;
		}
		if (sym >= 16) {
			if (zpeer && !zyx)
				stat_dp(DP_V32BIS);
			set_state(A_TRAIN1);
			set_tx(TXM_S);
			ev_s = 0;
		}
		break;
	case A_ZMARK:
		/* our marker, then the quiet before S at ZyX's rate */
		if (sym >= 192) {
			set_state(A_QUIET);
			set_tx(TXM_SILENCE);
			set_baud(ZYX_BAUD);
		}
		break;
	case A_TRAIN1:
	case A_TRAIN2:
	case C_TRAIN:
		/* S, S-bar, TRN; our echo canceller learns while the remote
		   is silent.  The caller's S is NT long before the 256 of
		   its conditioning signal (5.4.1): the answerer, the ESS
		   modem's at least, needs it */
		if (txm == TXM_S && sym >= 256 + (state == C_TRAIN ? nt_syms : 0))
			set_tx(TXM_SBAR);
		else if (txm == TXM_SBAR && sym >= 16) {
			set_tx(TXM_TRN2);
			if (state != A_TRAIN2)
				ec_mu = 0.05f;
			/* TRN starts from a fixed scrambler state (as found
			   in recordings of the original library, whose
			   receiver trains on the exact sequence) */
			tx_scr = caller ? 0x673800 : 0x0FF800;
		}
		else if (txm == TXM_TRN2 && sym >= 256) {
			set_tx(TXM_TRN4);
			/* and so does the 4 point part */
			tx_scr = caller ? 0x221837 : 0x683C9F;
			if (state != A_TRAIN2)
				ec_mu = 0.05f;
		} else if (txm == TXM_TRN4 && sym >= V32_TRN_LEN) {
			if (ec_mu > 0) {
				float erle = ec.judge();
				DSP_DBG("v32: echo canceller took %.1f dB "
					"away%s\n", erle, ec.off ? ": off" : "");
			}
			ec_mu = 0;
			if (state == A_TRAIN1) {
				set_state(A_R1);
				start_word(TXM_R, RB_B4 | RB_SYNC | caps);
			} else if (state == A_TRAIN2) {
				set_state(A_R3);
				start_word(TXM_R, final_word(tx_rate));
				ev_e = 0;
			} else {
				set_state(C_R2);
				start_word(TXM_R, RB_B4 | RB_SYNC |
					       (r_word & r2_caps));
				ev_r = 0;
				ev_s = 0;
			}
		}
		break;
	case A_R1:
		/* R1 until the caller starts its training */
		if (ev_s && tx_word_bit == 0) {
			set_state(A_WAIT_R2);
			set_tx(TXM_SILENCE);
			ev_r = 0;
		}
		break;
	case A_WAIT_R2:
		if (ev_r) {
			ev_r = 0;
			r = choose(r_word);
			if (!r) {
				DSP_DBG("v32: no common rate in %04X\n", r_word);
				return DPSTAT_ERROR;
			}
			tx_rate = r;
			set_state(A_TRAIN2);
			set_tx(TXM_S);
		}
		break;
	case A_R3:
		if (ev_e && tx_word_bit == 0) {
			set_state(A_E);
			start_word(TXM_E, final_word(tx_rate) | RB_HEADER);
		}
		break;
	case RN_TRAIN:
		/* S 128, S-bar 16, TRN 256 (no echo canceller training);
		   the responder's TRN until the initiator's R4 */
		if (remote_retrain() && txm != TXM_S && rxp == RX_DATA) {
			retrain(true);
			break;
		}
		if (total >= reneg_end) {
			retrain(false);
			break;
		}
		if (txm == TXM_S && sym >= 128)
			set_tx(TXM_SBAR);
		else if (txm == TXM_SBAR && sym >= 16) {
			set_tx(TXM_TRN2);
			tx_scr = caller ? 0x673800 : 0x0FF800;
		} else if (txm == TXM_TRN2 && sym >= 64) {
			set_tx(TXM_TRN4);
			tx_scr = caller ? 0x221837 : 0x683C9F;
		} else if (txm == TXM_TRN4 && sym >= 256 &&
			   (reneg == 1 || ev_r)) {
			if (reneg == 1) {
				rn_word = RB_B4 | RB_SYNC | quality_caps();
				/* (test: no faster than reneg_to) */
				for (s16 i = 0; i < nrates(); i++) {
					const struct v32_rate &t = rates()[i];
					if (reneg_to && t.bps > reneg_to)
						rn_word &= ~(t.bits & ~RB_TCM);
				}
			} else {
				ev_r = 0;
				rn_word = RB_B4 | RB_SYNC |
					(r_word & quality_caps());
				r = rate_of_word(rn_word);
				if (!r) {
					DSP_DBG("v32: no common rate in %04X\n",
						r_word);
					retrain(false);
					break;
				}
				tx_rate = r;
			}
			DSP_DBG("v32: R%d %04X\n", reneg == 1 ? 4 : 5,
				rn_word);
			set_state(RN_R);
			start_word(TXM_R, rn_word);
			ev_e = 0;
		}
		break;
	case RN_R:
		if (remote_retrain() && rxp == RX_DATA) {
			retrain(true);
			break;
		}
		if (total >= reneg_end) {
			retrain(false);
			break;
		}
		if (tx_word_bit != 0)
			break;
		if (reneg == 1 && ev_r) {
			/* R5 (or the remote's own R4): its fastest rate
			   of ours */
			ev_r = 0;
			r = rate_of_word(r_word & rn_word);
			if (!r) {
				DSP_DBG("v32: no common rate in %04X\n", r_word);
				retrain(false);
				break;
			}
			tx_rate = r;
		} else if (!(reneg == 2 && ev_e))
			break;
		set_state(RN_E);
		start_word(TXM_E, final_word(tx_rate) | RB_HEADER);
		break;
	case RN_E:
	case A_E:
	case C_E:
		if (txm == TXM_E && sym >= 8) {
			set_tx(TXM_DATA);
			tx_trellis.diff = 0;
			tx_trellis.conv = 0;
			tx_fpos = 0;
		}
		if (txm == TXM_DATA && rxp == RX_DATA)
			return connect();
		break;

	/* calling modem */
	case C_AA:
		if (ev_rev && ev_ac) {
			ev_rev = 0;
			set_state(C_AA_DELAY);
			tx_syms = 0;
		} else if (automode && ev_usb1 &&
			   usb1.on_count > ms2samples(100, srate))
			return fallback();
		/* an FSK answerer's steady mark after its answer tone */
		else if (automode &&
			 v21m.on_count > ms2samples(300, srate))
			return fallback_to(DP_V21, "V.21");
		else if (automode && !ans2100.on &&
			 v23m.on_count > ms2samples(300, srate))
			return fallback_to(DP_V23, "V.23");
		break;
	case C_AA_DELAY:
		/* CC goes out 64 symbols after the reversal; our detector
		   sees it about 26 symbols late.  The answerer measures the
		   round trip delay from this, and the original library fails
		   its training when the estimate is too long. */
		if (sym >= 38) {
			set_state(C_CC);
			set_tx(TXM_CC);
			ev_rev = 0;
		}
		break;
	case C_CC:
		if (ev_rev) {
			/* NT (5.4.1): from our reversal to the answerer's,
			   less its 64 symbols */
			nt_syms = sym > 64 ? sym - 64 : 0;
			set_state(C_WAIT_R1);
			set_tx(TXM_SILENCE);
			/* ZyX: the training and data at ZYX_BAUD, NT in its
			   symbols (the firmware's 0x4924 / 0x4000); by the
			   markers (S115 1): ours, and the answerer's decides */
			if (zpeer) {
				set_state(C_ZMARK);
				set_tx(TXM_ZMARK_C);
				zlisten = true;
				zseen = 0;
			} else if (zyx) {
				nt_syms = nt_syms * 8 / 7;
				set_baud(ZYX_BAUD);
			}
			ev_r = 0;
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case C_ZMARK:
	case C_WAIT_R1:
		/* (S115 1) the answerer's marker: ZyX; its R1 first: V.32bis */
		if (zlisten && zmark_heard(false)) {
			zlisten = false;
			zyx_switch("answerer");
			nt_syms = nt_syms * 8 / 7;
			set_baud(ZYX_BAUD);
			ev_r = 0;
		}
		if (state == C_ZMARK && sym >= 192) {
			set_state(C_WAIT_R1);
			set_tx(TXM_SILENCE);
		}
		if (state == C_ZMARK)
			break;
		if (ev_r && zlisten) {
			zlisten = false;
			stat_dp(DP_V32BIS);
		}
		if (ev_r) {
			ev_r = 0;
			r2_caps = quality_caps();
			DSP_DBG("v32: rates we can receive: %04X\n", r2_caps);
			set_state(C_TRAIN);
			set_tx(TXM_S);
		}
		break;
	case C_R2:
		/* R2 until R3 arrives */
		if (ev_r && tx_word_bit == 0) {
			ev_r = 0;
			/* among the rates our R2 offered (not those our
			   receiver's quality allows now: the answerer has
			   chosen from R2, and an estimate a little lower
			   since would leave no rate at all) */
			r = rate_of_word(r_word & r2_caps);
			DSP_DBG("v32: R word %04X in R2 (choice %04X)\n",
				r_word, r ? r->bits : 0);
			/* R1 repeated, until the answerer's second training;
			   after it R3, which may name all the common rates
			   (Agere's does): the fastest of them */
			if (!r || (!ev_s && (r_word & RB_RATES) != r->bits))
				break;
			tx_rate = r;
			set_state(C_E);
			start_word(TXM_E, final_word(r) | RB_HEADER);
		}
		break;

	case V32_DATA:
		/* the remote renegotiates (its S heard), or retrains: AA
		   from a caller, AC from an answerer */
		if (rxp != RX_DATA) {
			renegotiate(true);
			break;
		}
		if (remote_retrain())
			retrain(true);
		else if (retrain_at && count >= retrain_at &&
			 !retrains) {
			retrain(false);
		} else if (reneg_at && count >= reneg_at && !renegs)
			renegotiate(false);
		/* the core's retrain (1) or rate renegotiation (2: V.80's,
		   ATO3, MNP 10's rate shifts), at the rates its limits
		   allow now */
		else if (modem_get_param(modem, MDMPRM_LINK_REQ) > 0) {
			intptr_t q = modem_get_param(modem, MDMPRM_LINK_REQ);
			modem_set_param(modem, MDMPRM_LINK_REQ, 0);
			/* (V.32 has no renegotiation) */
			if (q == 2 && v32_id != DP_V32)
				renegotiate(false);
			else
				retrain(false);
		}
		/* our receiver has lost its equalization (5.5/V.32: either
		   modem may retrain): its error past its rate's limit for
		   2 s (a burst of noise passes: the equalizer comes back
		   by itself, and a retrain in it would fail), the far end's
		   signal still there (not as it fades at a hang up, which
		   S10 is for) */
		else if (lost_syms >= 2 * baud &&
			 rx.power > 0.5f * data_power &&
			 !lost_off) {
			DSP_DBG("v32: equalization lost (mse %.4f)\n",
				rx_mse);
			lost_syms = 0;
			retrain(false);
		}
		/* the receiver's quality, for the call's statistics (while
		   the signal is there as it was: not as it fades at the
		   end) */
		else if (count % ms2samples(100, srate) == 0 &&
			 rx.power > 0.5f * data_power) {
			stat_snr(-10 * log10f(rx_mse + 1e-6f));
			stat_line(baud, line_carrier(), baud, line_carrier());
			stat_freq_offset(rx.dtheta, baud);
			stat_timing_offset(rx.t_adj, rx.sps);
		}
		break;
	}
	return DPSTAT_OK;
}

/* a sample x in, *y out: the handshake (at symbol boundaries, and every
   sample in silence or the answer tone), the transmitter, the receiver.
   DPSTAT_CONNECT once, DPSTAT_CHANGEDP on a fallback, DPSTAT_ERROR on
   a handshake failure (the time limit, S7, and the loss of carrier in
   data, S10, are pump_loop()'s) */
s16 V32Pump::sample(float x, float *y)
{
	s16 r = DPSTAT_OK;

	/* the handshake acts on symbol boundaries */
	if (txm != TXM_ANS && tx.t + 1.0f / tx.sps >= 1) {
		r = handshake();
		if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
			return r;
	} else if (txm == TXM_ANS || txm == TXM_SILENCE) {
		s16 h = handshake();
		if (h == DPSTAT_CHANGEDP || h == DPSTAT_ERROR)
			return h;
	}

	if (txm == TXM_ANS)
		*y = DSP_TX_AMPLITUDE * ans.next();
	else
		*y = tx.sample([this] { return tx_symbol(); });
	last_tx = *y;

	rx_sample(x);
	return r;
}

/* V.32 (4800 and 9600 only) or V.32bis, the rates limited by +MS; the
   transmitter, receiver, echo canceller and tone trackers; timeouts
   from S7 and S10; an answerer after V.8 starts at the gap.  0, or -1
   when out of memory. */
s16 V32Pump::start(struct modem *m, enum DP_ID id, s16 caller_,
		   s32 srate_, s32 max_frag)
{
	pump_setup(m, srate_, caller_);
	automode = (id != DP_V32 && id != DP_V32BIS && id != DP_V32TERBO &&
		    id != DP_ZYX) ||
		(rt && rt->v32_automode);

	/* rates we offer */
	v32_id = id;
	/* ZyXEL's markers (S115 1): ZyX only once they say so */
	zpeer = (id == DP_V32BIS || id == DP_ZYX) &&
		modem_get_sreg(m, SREG_ZYX_PEER) == 1;
	zlisten = false;
	zseen = 0;
	zyx = id == DP_ZYX && !zpeer;
	if (zyx)
		zyx_scale_tables();
	caps = offered_caps();

	tx.init(srate_, 1800, V32_BAUD, V32_ALPHA, V32_SPAN,
		    sqrtf(V32_POWER), V32_TX_AMPLITUDE);
	/* symbol centres a quarter symbol off the sample grid: the
	   original library's receiver fails to train at some timing
	   phases, this one works for all its rates */
	tx.t = 0.25f;
	ans.set_freq(2100, srate_);
	carrier.init(150, 90);
	if (rx.init(srate_, 1800, V32_BAUD, V32_ALPHA, V32_SPAN,
			V32_EQ_LEN, V32_POWER) ||
	    ec.init(V32_EC_LEN))
		return -1;
	aa.init(1800, srate_);
	ac.init(600, srate_);
	usb1.init(2250, srate_);
	ans2100.init(2100, srate_);
	z1200.init(1200, srate_);
	z2400.init(2400, srate_);
	z1500.init(1500, srate_);
	z2100.init(2100, srate_);
	v21m.init(1650, srate_);
	v23m.init(1300, srate_);

	/* the caller scrambles with GPC, the answerer with GPA */
	tx_scr_tap = caller_ ? 18 : 5;
	rx_scr_tap = caller_ ? 5 : 18;
	tx_rate = &rates()[nrates() - 1];
	rx_rate = tx_rate;

	/* test: DSP_V32_RETRAIN=<ms in data mode>[a|c] */
	retrain_at = static_cast<s32>(test_samples("DSP_V32_RETRAIN"));
	/* and DSP_V32_RENEG=<ms in data mode>[:<bit/s>][a|c]: a
	   renegotiation (to that rate at most) */
	reneg_at = static_cast<s32>(test_samples("DSP_V32_RENEG"));
	if (const char *e = pump_test_env("DSP_V32_RENEG", caller_))
		if (const char *c = strchr(e, ':'))
			reneg_to = static_cast<s32>(strtol(c + 1, nullptr, 10));
	/* (tests: DSP_V32_LOST=0, no retrain of our own on a lost
	   equalization) */
	lost_off = env_off("DSP_V32_LOST");

	if (caller_) {
		state = C_AA;
		txm = TXM_AA;
	} else if (rt && rt->ans_sent) {
		/* from V.8: the answer tone (ANSam) is already sent */
		rt->ans_sent = rt->after_v8 = 0;
		state = A_GAP;
		txm = TXM_SILENCE;
	} else {
		state = A_SILENCE;
		txm = TXM_SILENCE;
	}
	DSP_DBG("v32: create %s (dp %d), rates %04X, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", id, caps, srate_);
	return 0;
}

/* frees the buffers rx and ec took */
void V32Pump::cleanup()
{
	rx.release();
	ec.release();
}

/* the V.8/V.34 pump (dp_v34.cpp) hands over to this one, with the
   V.22bis fallback, when the remote modem does not do V.34 */
static const enum DP_ID v32_ids[] = { DP_V32, DP_V32BIS, DP_V32TERBO,
				       DP_ZYX };

/* registers the pump as DP_V32, DP_V32BIS, DP_V32TERBO and DP_ZYX; 0 */
s16 dp_v32_init(void)
{
	u16 i;
	for (i = 0; i < std::size(v32_ids); i++)
		modem_dp_register(v32_ids[i], &PumpOps<V32Pump>::ops);
	return 0;
}

/* deregisters them */
void dp_v32_exit(void)
{
	u16 i;
	for (i = 0; i < std::size(v32_ids); i++)
		modem_dp_deregister(v32_ids[i], &PumpOps<V32Pump>::ops);
}

} // namespace dsp
