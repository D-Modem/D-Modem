/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_v61.cpp - ITU-T V.61 data pump: analogue simultaneous voice plus
 *                 data (ASVD), 4800 bit/s with speech, data alone at
 *                 4800 to 14400 bit/s
 *
 *    3000 symbols/s on an 1800 Hz carrier (or 2800 on 1680 Hz) both
 *    ways, separated by an echo canceller.  The data alone is uncoded
 *    QAM (4 to 64 points, Figures 1 to 5) with V.32's differential
 *    quadrant code (Table 1).  With speech (QADM, 5.5) the data is 4800
 *    bit/s on 4 points (Figure 6) and the speech, moved down to the
 *    symbol rate (v61_audio.h), is added to each symbol inside its data
 *    point's quadrant: scaled per frame (5.5.2.3), every other symbol
 *    inverted (5.5.2.4), its negative half scaled by half the positive
 *    (5.5.2.5) and turned to the data point (Table 5).  The receiver
 *    decides the quadrant and takes the rest as the speech.
 *
 *    Data mode is in frames of 70 symbols (5.3): the user data, then a
 *    control segment (14 symbols at 3000/s, 10 at 2800) that says how the
 *    next frame is sent: with speech or data alone (this end goes to
 *    data alone while its speech is silent, 5.3.2, where the far end's
 *    rate word 3 allows it), the speech's gain.
 *
 *    Start-up (clause 9, Figure 9), after V.8 bis or without it:
 *
 *    answerer: ANS (until the caller's S), silence, S (256T, after the
 *              caller's DCBA), silence, F (on G), its reversal (after
 *              128T and 64T of G), its second (64T after G's reversal),
 *              silence (when G stops), S S-bar TRN R1 (until the caller's
 *              S), silence, S S-bar TRN R3 (after R2, until E), E, data
 *    caller:   ANS heard, S (256T) DCBA, G (on the answerer's S), its
 *              reversal (64T after F's), silence (on F's second),
 *              S (NT + 256T) S-bar TRN R2 (after R1, until R3), E, data
 *
 *    The symbol rate determination signal S is A B A B ... (3000/s), AA
 *    A A ... (2800/s); D-Modem sends S, and goes to 2800 if the other end
 *    sends AA ($DSP_V61_BAUD=2800: we ask for it).  R1, R2 and R3 are
 *    rate words 1, 2 and 3 (Tables 10 to 12) one after the other,
 *    repeated, scrambled with the 4 point differential code, most
 *    significant bit (B0) first.
 *
 *    A retrain (clause 10) starts again from G and F: on the far end's
 *    tone for 128T in data, or when the receiver has lost its
 *    equalization.  The rate renegotiation of clause 11 is not done.
 *
 *    The speech comes from and goes to the voice path (MDMPRM_VOICE:
 *    DSVD's, a SmartUSB56's handset or dmodemd's --dsvd-audio); without
 *    one, rate word 2 does not offer voice and every frame is data alone.
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
#include "v61_audio.h"

namespace dsp {

#define V61_ALPHA       0.12f
#define V61_SPAN        16      /* pulse half length: a 0.12 roll off
				   needs it long, 6 leaves ISI at -30 dB */
#define V61_EQ_LEN      48
#define V61_POWER       10.0f   /* mean power of every data constellation
				   (the 4 points at 1, -3 ...) */
#define V61_TRN_LEN     2048    /* TRN symbols (1280 to 8192, 8.6.3) */
#define V61_EC_LEN      256
#define V61_TX_AMPLITUDE (2000.0f * 1.41421f)
#define V61_FRAME       70
#define V61_B1_SYMS     128     /* ones after E (circuit 106 after it) */
#define V61_CT_DELAY    50      /* E to the first control segment */

/* rate word bits, B0 the most significant */
#define RB_HEADER       0xF000  /* 0000: a rate word, 1111: E */
#define RB_SYNC         0x0111  /* B7, B11, B15 */
#define RB_B4           0x0800
#define RB_B8           0x0080
#define RB_ID           (RB_B4 | RB_B8)
#define RB_RESERVED     0x0006  /* B13, B14 of word 1 */
/* word 1 (B4 B8 = 1 1) */
#define RB_4800         0x0400
#define RB_9600         0x0200
#define RB_7200         0x0040
#define RB_12000        0x0020
#define RB_14400        0x0008
#define RB_RATES        (RB_4800 | RB_9600 | RB_7200 | RB_12000 | RB_14400)
/* word 2 (1 0): voice plus data at 4800 bit/s */
#define RB_AD           0x0400
/* word 3 (0 1): silence detection in the receiving end's transmitter */
#define RB_SILENCE      0x0002

#define W1(r)   (RB_ID | RB_SYNC | (r))
#define W2(v)   (RB_B4 | RB_SYNC | ((v) ? RB_AD : 0))
#define W3(s)   (RB_B8 | RB_SYNC | ((s) ? RB_SILENCE : 0))

struct v61_rate {
	s32 bps;
	u16 bit;
	s16 nbits;
};

static const struct v61_rate v61_rates[] = {
	{ 14400, RB_14400, 6 },
	{ 12000, RB_12000, 5 },
	{ 9600, RB_9600, 4 },
	{ 7200, RB_7200, 3 },
	{ 4800, RB_4800, 2 },
};
#define V61_NRATES (s16)std::size(v61_rates)

/* the training SNR (dB) each rate takes (uncoded, a symbol error rate
   near 1e-5); the last, 4800, always */
static const float v61_min_snr[] = { 26, 23, 20, 17, 0 };

/* Figures 1 to 5: the points by index, Y1 in bit 0, Y2 in bit 1, then
   Q3 ... (the figures' labels read left to right are Y1 Y2 Q3 ...).
   Each is the one before with more points around it, and each turns
   into itself by 90 degrees with Y1 Y2 alone changed. */
static const s8 v61_grid[64][2] = {
	{ 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 },
	{ -3, 1 }, { 1, 3 }, { -1, -3 }, { 3, -1 },
	{ 1, -3 }, { -3, -1 }, { 3, 1 }, { -1, 3 },
	{ -3, -3 }, { -3, 3 }, { 3, -3 }, { 3, 3 },
	{ 1, 5 }, { 5, -1 }, { -5, 1 }, { -1, -5 },
	{ 5, 1 }, { 1, -5 }, { -1, 5 }, { -5, -1 },
	{ -3, 5 }, { 5, 3 }, { -5, -3 }, { 3, -5 },
	{ 5, -3 }, { -3, -5 }, { 3, 5 }, { -5, 3 },
	{ 5, 5 }, { 5, -5 }, { -5, 5 }, { -5, -5 },
	{ -7, 1 }, { 1, 7 }, { -1, -7 }, { 7, -1 },
	{ 1, -7 }, { -7, -1 }, { 7, 1 }, { -1, 7 },
	{ -7, -3 }, { -3, 7 }, { 3, -7 }, { 7, 3 },
	{ -3, -7 }, { -7, 3 }, { 7, -3 }, { 3, 7 },
	{ -7, 5 }, { 5, 7 }, { -5, -7 }, { 7, -5 },
	{ 5, -7 }, { -7, -5 }, { 7, 5 }, { -5, 7 },
	{ 1, 9 }, { 9, -1 }, { -9, 1 }, { -1, -9 },
};
/* 4800 bit/s, Figure 5 (and Figure 6's data points): Y1 Y2 00 (A),
   10 (D), 01 (B), 11 (C) */
static const s8 v61_grid4[4][2] = { { 1, -3 }, { -3, -1 }, { 3, 1 },
				    { -1, 3 } };

/* the differential quadrant code (Table 1): the quadrant (A B C D: 0 1 2
   3) of a Y1 Y2 index, its index; the quadrants turned (+90 degrees
   each) by Q1 Q2 (Q1 in bit 0), and back */
static const s16 quad_of_y[4] = { 0, 3, 1, 2 };
static const s16 y_of_quad[4] = { 0, 2, 3, 1 };
static const s16 turn_of_q[4] = { 1, 2, 0, 3 };
static const s16 q_of_turn[4] = { 2, 0, 1, 3 };
/* TRN after its first 256 symbols (Table 9): the dibit, first bit in
   bit 1, to A B C D */
static const s16 trn_quad[4] = { 0, 1, 3, 2 };

enum v61_txm {
	TXM_SILENCE,
	TXM_ANS,
	TXM_SRD,                /* S or AA (3000/s) */
	TXM_DCBA,
	TXM_TONE,               /* G or F */
	TXM_S,
	TXM_SBAR,
	TXM_TRN,
	TXM_R,
	TXM_E,
	TXM_DATA,
};

enum v61_state {
	/* answerer */
	A_SILENCE,
	A_ANS,                  /* until the caller's S or AA */
	A_SRD_END,              /* silence until it turns DCBA */
	A_SRD,                  /* ours */
	A_WAIT_G,
	A_F,                    /* F until 128T and G for 64T */
	A_F_REV,                /* reversed: until G's reversal */
	A_F_REV2,               /* the second, 64T after it */
	A_F2,                   /* until G stops */
	A_QUIET,
	A_TRAIN1,
	A_R1,
	A_WAIT_R2,
	A_TRAIN2,
	A_R3,
	A_E,
	/* caller */
	C_ANS,
	C_SRD,
	C_DCBA,                 /* until the answerer's S or AA */
	C_SRD_KIND,             /* which of them */
	C_G,                    /* until F's reversal */
	C_G_REV,                /* ours, 64T after it */
	C_G2,                   /* until F's second */
	C_WAIT_R1,
	C_TRAIN,
	C_R2,
	C_E,
	/* both */
	V61_DATA,
};

/* receiver phases */
enum v61_rxp {
	RX_IDLE,
	RX_S,
	RX_TRN,
	RX_R,
	RX_DATA,
};

struct V61Pump : PumpBase<V61Pump, v61_state> {
	static constexpr const char *name = "V.61 data pump";

	/* frees the receiver's and echo canceller's buffers */
	~V61Pump() { cleanup(); }

	s32 baud;               /* 3000 or 2800 */
	s16 want_2800;          /* $DSP_V61_BAUD=2800 */
	s16 ctrl_at;            /* a frame's first control symbol: 56, 60 */
	u16 caps;               /* rate word 1 bits we offer */
	u16 r2_caps;            /* caller: rates in R2 */
	s16 voice_ok;           /* we have a voice path (word 2's B5) */
	struct dp_voice *voice; /* the core's, or NULL */
	s16 voice_open;
	cplx pts[V61_NRATES][64];       /* the constellations, V61_POWER */
	cplx pts4[4];           /* training and rate words */
	cplx pts_ad[4];         /* Figure 6's, -2.5 dB */
	cplx rot_ad[4];         /* Table 5 */
	float aud_neg;          /* 5.5.2.5: the negative half's scale */

	/* transmitter */
	QamTx tx;
	enum v61_txm txm;
	s32 tx_syms;
	s16 tx_srd_s;           /* our S or AA is S */
	Osc tone;
	float tone_freq;
	float tone_sign;
	Osc ans;
	float last_tx;
	u32 tx_scr;
	s16 tx_scr_tap;
	s16 tx_quad;            /* the differential code's last quadrant */
	u16 tx_words[3];
	s16 tx_nwords, tx_word, tx_word_bit;
	const struct v61_rate *tx_rate;
	s16 tx_ad_on;           /* frames with speech allowed (R3) */
	s16 tx_silence_ok;      /* the far end's word 3: variable rate */
	/* the data mode's frames */
	s32 tx_n;               /* symbols since E */
	s16 tx_pos;             /* in the frame */
	s16 tx_cur_ad, tx_next_ad;
	s16 tx_gi, tx_gq;       /* this frame's gain steps */
	s16 tx_ngi, tx_ngq;     /* the next one's */
	s16 tx_lock;
	u8 tx_ctrl[29];         /* the control segment's bits 1 ... */
	s16 tx_quiet;           /* silent frames in a row */
	V61AudioTx aud_tx;
	cplx aud_ring[256];     /* the speech by symbol, V61_LOOK ahead */
	s16 mic_buf[1024];
	s32 mic_n, mic_head;
	s16 mic_primed;
	s64 frames_ad, frames_d;

	/* receiver */
	EchoCanceller ec;
	float ec_mu;
	QamRx rx;
	ToneTrk t1800, t3300, t300, t1050, ans2100;
	ToneTrk g1500, g1400, f2100, f1960;
	CarrierDetect carrier;
	enum v61_rxp rxp;
	s32 run_s;
	s16 last_sgn, s_miss;
	cplx last_z;
	u32 rx_scr;
	s16 rx_scr_tap;
	s16 rx_quad;
	u64 rx_bits;
	s32 rx_nbits;
	s16 rx_word_align;
	u16 rx_r[3];            /* the R signal's words, last seen */
	u16 r_words[3];         /* and given to the handshake */
	float rx_mse;
	float data_power;
	s16 rx_e_seen;
	const struct v61_rate *rx_rate;
	s32 rx_n;               /* symbols since E */
	s16 rx_pos;
	s16 rx_cur_ad, rx_next_ad, rx_lock;
	float rx_g[2];          /* this frame's gains (I, Q) */
	float rx_ng[2];
	u8 rx_ctrl[29];
	s32 rx_bad_ctrl;        /* control segments in a row that fail
				   their checks */
	s16 rx_dcopies;         /* the D bit's copies in its third bits */
	s16 rx_dones;           /* (those that say data alone) */
	V61AudioRx aud_rx;
	s16 ear_buf[160];
	s16 ear_n;
	s16 agc_free;

	/* events from the receiver */
	s16 ev_r, ev_e, ev_s;
	s16 ev_frev, ev_grev;
	s16 retraining, retrains;
	s32 retrain_at;
	float lost_mse;
	s32 lost_syms;
	s16 lost_off;
	s32 mt_syms, nt_syms;   /* round trip delays (symbols) */
	s32 g_on_syms;          /* answerer: G heard (symbols) */
	s32 srd_seen;           /* samples of the far end's S or AA */

	void make_points();
	void init_rate(s32 b);
	/* the 4 point constellation's point in quadrant quad */
	cplx point4(s16 quad) const { return pts4[quad]; }
	cplx tx_symbol();
	void set_tx(enum v61_txm m);
	void start_words(enum v61_txm m, u16 w1, u16 w2, u16 w3);
	cplx tx_word_symbol();
	cplx tx_data_symbol();
	void tx_frame_plan();
	cplx tx_audio(s16 quad);
	void mic_fill();
	void rx_reset();
	void rx_word_bit(s16 bit);
	void rx_symbol();
	void rx_data_symbol(cplx z);
	void rx_control_end();
	void ear_put(cplx a);
	void rx_sample(float x);
	/* its name in the log */
	const char *tag() const { return "v61"; }
	/* connected (PumpBase's statistics) */
	bool in_data() const { return state == V61_DATA; }
	s16 connect();
	void retrain(bool remote);
	u16 quality_caps();
	float lost_limit(const struct v61_rate *r);
	void voice_state(bool on);
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

/* the symbols the speech is taken ahead of its own: the next frame's
   gain is chosen from it during this frame's control segment */
#define V61_LOOK 96

/* the speech's peak, of its full swing (0.7 of the way to the quadrant's
   edge, 5.5.2.5): the gain is ours to choose, and a sine at 0.95 of it
   costs the data its margin below 30 dB SNR; at 0.6 (4 dB of headroom)
   4800 bit/s is clean to 20 dB */
#define V61_PEAK 0.6f

/* the fastest rate whose bit is set in w, or NULL */
static const struct v61_rate *rate_of_word(u16 w)
{
	for (s16 i = 0; i < V61_NRATES; i++)
		if (w & v61_rates[i].bit)
			return &v61_rates[i];
	return nullptr;
}

/* a valid rate word of kind id (RB_ID, RB_B4 or RB_B8) */
static bool word_ok(u16 w, u16 id)
{
	return (w & RB_HEADER) == 0 && (w & RB_SYNC) == RB_SYNC &&
		(w & RB_ID) == id;
}

/* a valid E naming one rate */
static bool e_ok(u16 w)
{
	u16 r = w & RB_RATES;
	return (w & RB_HEADER) == RB_HEADER && (w & RB_SYNC) == RB_SYNC &&
		(w & RB_ID) == RB_ID && r && !(r & (r - 1));
}

/* the far end's S (A B A B ...: 1800 Hz with 300 and 3300 Hz beside it,
   each about 9 dB under) rather than AA (1800 Hz alone) */
static bool srd_is_s(const ToneTrk &c, const ToneTrk &lo, const ToneTrk &hi)
{
	return cplx_pow(lo.lp2) + cplx_pow(hi.lp2) > 0.03f * cplx_pow(c.lp2);
}

/* the gain of n steps of the fourth root of 2 (5.5.2.3) */
static float gain_of(s16 n)
{
	return powf(2.0f, n / 4.0f);
}

/*
 *    set up
 */

/* the constellations at V61_POWER, Figure 6's 2.5 dB under, Table 5's
   turns, 5.5.2.5's scale */
void V61Pump::make_points()
{
	for (s16 r = 0; r < V61_NRATES; r++) {
		s16 n = static_cast<s16>(1 << v61_rates[r].nbits);
		const s8 (*g)[2] = n == 4 ? v61_grid4 : v61_grid;
		float p = 0;
		for (s16 i = 0; i < n; i++)
			p += g[i][0] * g[i][0] + g[i][1] * g[i][1];
		float s = sqrtf(V61_POWER * n / p);
		for (s16 i = 0; i < n; i++)
			pts[r][i] = cplx_make(g[i][0] * s, g[i][1] * s);
	}
	/* (the 4 points have power 10 as they are) */
	float ad = powf(10.0f, -2.5f / 20);
	for (s16 q = 0; q < 4; q++) {
		s16 y = y_of_quad[q];
		pts4[q] = cplx_make(v61_grid4[y][0], v61_grid4[y][1]);
		pts_ad[q] = cplx_scale(pts4[q], ad);
		/* the point's angle less 45 degrees */
		float a = atan2f(pts4[q].im, pts4[q].re) -
			static_cast<float>(M_PI) / 4;
		rot_ad[q] = cplx_make(cosf(a), sinf(a));
	}
	/* 0.7 of the distance to the quadrant's boundary, sqrt(5) */
	aud_neg = 0.7f * sqrtf(5.0f) * ad;
}

/* the line at b symbols/s: transmitter, receiver, frame layout */
void V61Pump::init_rate(s32 b)
{
	baud = b;
	ctrl_at = b == 2800 ? 60 : 56;
	float fc = b == 2800 ? 1680 : 1800;
	float t = tx.t;
	tx.init(srate, fc, static_cast<float>(b), V61_ALPHA, V61_SPAN,
		sqrtf(V61_POWER), V61_TX_AMPLITUDE);
	tx.t = t;
	rx.release();
	rx.init(srate, fc, static_cast<float>(b), V61_ALPHA, V61_SPAN,
		V61_EQ_LEN, V61_POWER);
	rx_reset();
	tone_freq = 0;
	DSP_DBG("v61: %" PRId32 " symbols/s\n", b);
}

/*
 *    transmitter (at the symbol rate)
 */

/* signal m from its first symbol */
void V61Pump::set_tx(enum v61_txm m)
{
	txm = m;
	tx_syms = 0;
}

/* the rate signal (m TXM_R: w1 w2 w3 repeated) or E (TXM_E: w1 once)
   from its first bit */
void V61Pump::start_words(enum v61_txm m, u16 w1, u16 w2, u16 w3)
{
	tx_words[0] = w1;
	tx_words[1] = w2;
	tx_words[2] = w3;
	tx_nwords = m == TXM_E ? 1 : 3;
	tx_word = 0;
	tx_word_bit = 0;
	set_tx(m);
}

/* the next 4 point symbol of the rate signal: two bits, scrambled,
   differentially coded */
cplx V61Pump::tx_word_symbol()
{
	s16 b[2];
	for (s16 k = 0; k < 2; k++) {
		u16 w = tx_words[tx_word];
		s16 bit = (w >> (15 - tx_word_bit)) & 1;
		if (++tx_word_bit == 16) {
			tx_word_bit = 0;
			tx_word = static_cast<s16>((tx_word + 1) % tx_nwords);
		}
		b[k] = scramble(&tx_scr, tx_scr_tap, bit);
	}
	tx_quad = (tx_quad + turn_of_q[b[0] | b[1] << 1]) & 3;
	return point4(tx_quad);
}

/* the microphone's samples into its buffer, 40 ms of them at most; the
   speech starts once 20 ms are there */
void V61Pump::mic_fill()
{
	if (!voice || !voice_open)
		return;
	while (mic_n < 320) {
		s16 tmp[160];
		s32 room = 320 - mic_n;
		int n = voice->mic(voice->ctx, tmp, room > 160 ? 160 : room);
		if (n <= 0)
			break;
		for (int i = 0; i < n; i++)
			mic_buf[(mic_head + mic_n + i) & 1023] = tmp[i];
		mic_n += n;
	}
	if (mic_n >= 160)
		mic_primed = 1;
}

/* the frame after this one: with speech or data alone, its gains, the
   control segment's bits (Table 6), at the first control symbol */
void V61Pump::tx_frame_plan()
{
	/* the next frame's symbols in aud_ring */
	s32 n0 = tx_n + (V61_FRAME - ctrl_at);
	float pi = 0, pq = 0, e = 0;
	for (s32 k = 0; k < V61_FRAME; k++) {
		cplx a = aud_ring[(n0 + k) & 255];
		pi = fmaxf(pi, fabsf(a.re));
		pq = fmaxf(pq, fabsf(a.im));
		e += cplx_pow(a);
	}
	e /= V61_FRAME;
	/* silence: under -50 dB of full scale for three frames */
	if (e < 1e-5f)
		tx_quiet++;
	else
		tx_quiet = 0;
	tx_next_ad = tx_ad_on && !(tx_silence_ok && tx_quiet >= 3);
	if (tx_next_ad)
		frames_ad++;
	else
		frames_d++;

	/* the steps that keep each peak within V61_PEAK (31 at most), the
	   larger within 3 of the smaller */
	auto steps = [](float p) -> s16 {
		if (p < 1e-6f)
			return 31;
		s16 k = static_cast<s16>(floorf(4 * log2f(V61_PEAK / p)));
		return k < 0 ? 0 : k > 31 ? 31 : k;
	};
	s16 ki = steps(pi), kq = steps(pq);
	s16 base = ki < kq ? ki : kq;
	s16 delta = static_cast<s16>((ki > kq ? ki : kq) - base);
	if (delta > 3)
		delta = 3;
	s16 gf = kq < ki;       /* the quadrature signal is the base */
	tx_ngi = gf ? static_cast<s16>(base + delta) : base;
	tx_ngq = gf ? base : static_cast<s16>(base + delta);
	/* a frame after data alone has no gain sent: 0 */
	if (!tx_cur_ad)
		tx_ngi = tx_ngq = 0;
	tx_lock = fmaxf(pi * gain_of(tx_ngi), pq * gain_of(tx_ngq)) > 0.25f;

	u8 *c = tx_ctrl;
	const s16 D = !tx_next_ad, L = tx_lock;
	const s16 P = 20;       /* the flat pre-emphasis */
	auto put = [&c](s16 at, s16 v, s16 nb) {
		for (s16 i = 0; i < nb; i++)
			c[at + i] = (v >> (nb - 1 - i)) & 1;
	};
	std::memset(tx_ctrl, 0, sizeof(tx_ctrl));
	if (baud == 3000) {
		put(1, 0x6E, 7);        /* 1101110 */
		if (tx_cur_ad) {
			put(8, P, 6);
			c[14] = D;
			c[15] = L;
			put(16, base, 5);
			put(21, delta, 2);
			c[23] = !D;
			c[24] = !L;
			put(25, (~base >> 2) & 7, 3);
			c[28] = gf;
		} else {
			put(8, 0xE, 4); /* 1110 */
			c[12] = 0;      /* AC: no auxiliary data */
			c[13] = !tx_ad_on;      /* DB */
			c[14] = D;
			put(15, 0x77, 8);
			c[23] = !D;
			put(24, (~0x77 >> 4) & 15, 4);
			c[28] = 1;
		}
	} else {
		if (tx_cur_ad) {
			put(1, P, 6);
			c[7] = D;
			c[8] = L;
			put(9, base, 5);
			put(14, delta, 2);
			c[16] = !D;
			c[17] = !L;
			put(18, (~base >> 3) & 3, 2);
			c[20] = gf;
		} else {
			put(1, 0xE, 4);
			c[5] = 0;
			c[6] = !tx_ad_on;
			c[7] = D;
			put(8, 0x77, 8);
			c[16] = !D;
			put(17, (~0x77 >> 5) & 7, 3);
			c[20] = 1;
		}
	}
}

/* this symbol's speech for data point quad (5.5.2.3 to 5.5.2.6) */
cplx V61Pump::tx_audio(s16 quad)
{
	cplx a = aud_ring[tx_n & 255];
	float u = a.re * gain_of(tx_gi), v = a.im * gain_of(tx_gq);
	u = fminf(fmaxf(u, -1), 1);
	v = fminf(fmaxf(v, -1), 1);
	if (tx_n & 1) {
		u = -u;
		v = -v;
	}
	u *= u > 0 ? 2 * aud_neg : aud_neg;
	v *= v > 0 ? 2 * aud_neg : aud_neg;
	return cplx_mul(cplx_make(u, v), rot_ad[quad]);
}

/* the next data mode symbol: the speech's pipeline, the frame's data or
   control bits through the scrambler, its point (with the speech in a
   frame that has it) */
cplx V61Pump::tx_data_symbol()
{
	/* the speech of the symbol V61_LOOK ahead */
	mic_fill();
	for (s32 need = aud_tx.wanted(); need > 0; need--) {
		s16 x = 0;
		if (mic_primed && mic_n > 0) {
			x = mic_buf[mic_head];
			mic_head = (mic_head + 1) & 1023;
			mic_n--;
		}
		aud_tx.push(x);
	}
	aud_ring[(tx_n + V61_LOOK) & 255] = aud_tx.next();

	if (tx_pos == ctrl_at)
		tx_frame_plan();
	const s16 n = tx_cur_ad ? 2 : tx_rate->nbits;
	u8 bits[8];
	if (tx_pos < ctrl_at) {
		if (tx_n < V61_B1_SYMS)
			std::fill_n(bits, n, 1);
		else
			modem_get_bits(modem, 1, bits, n);
	} else {
		/* bits 2k+2 then 2k+1, the D bit, zeros */
		s16 k = static_cast<s16>(tx_pos - ctrl_at);
		bits[0] = tx_ctrl[2 * k + 2];
		bits[1] = tx_ctrl[2 * k + 1];
		for (s16 i = 2; i < n; i++)
			bits[i] = i == 2 ? !tx_next_ad : 0;
	}
	s16 q = 0;
	for (s16 i = 0; i < n; i++)
		q |= static_cast<s16>(scramble(&tx_scr, tx_scr_tap,
					       bits[i] & 1) << i);
	tx_quad = (tx_quad + turn_of_q[q & 3]) & 3;
	cplx z;
	if (tx_cur_ad)
		z = cplx_add(pts_ad[tx_quad], tx_audio(tx_quad));
	else
		z = pts[tx_rate - v61_rates][y_of_quad[tx_quad] | (q & ~3)];

	tx_n++;
	if (++tx_pos == V61_FRAME) {
		tx_pos = 0;
		tx_cur_ad = tx_next_ad;
		tx_gi = tx_ngi;
		tx_gq = tx_ngq;
	}
	return z;
}

/* the next symbol of the current transmit signal; counts tx_syms */
cplx V61Pump::tx_symbol()
{
	cplx z = { 0, 0 };
	s16 b1, b2;

	switch (txm) {
	case TXM_SILENCE:
	case TXM_ANS:
	case TXM_TONE:
		break;
	case TXM_SRD:
		/* S: A B A B ..., AA: A A ... */
		z = point4(tx_srd_s ? (tx_syms & 1) : 0);
		break;
	case TXM_DCBA:
		z = point4(3 - (tx_syms & 3));
		break;
	case TXM_S:
		z = point4(tx_syms & 1);
		break;
	case TXM_SBAR:
		z = point4(2 + (tx_syms & 1));
		break;
	case TXM_TRN:
		/* scrambled ones from an all zero register: A or C by the
		   first bit of each dibit for 256 symbols, then Table 9 */
		b1 = scramble(&tx_scr, tx_scr_tap, 1);
		b2 = scramble(&tx_scr, tx_scr_tap, 1);
		tx_quad = tx_syms < 256 ? (b1 ? 2 : 0) : trn_quad[b1 << 1 | b2];
		z = point4(tx_quad);
		break;
	case TXM_R:
	case TXM_E:
		z = tx_word_symbol();
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

/* back to waiting for S */
void V61Pump::rx_reset()
{
	rx.reset();
	/* the timing loop as fast as before data (QamRx::init()'s) */
	rx.kt_p = 0.03f * rx.sps;
	rx.kt_gi = 0.03f;
	rx.agc_hold = 0;
	agc_free = 0;
	rxp = RX_IDLE;
	run_s = 0;
	rx_nbits = 0;
	rx_e_seen = 0;
}

/* one descrambled bit of the rate signal: R (rate word 1 twice, 48 bits
   apart, words 2 and 3 between them, 8.7.1) sets ev_r and r_words when
   it is new; E on the same 16 bit boundaries, after R, with a clean
   signal, sets ev_e and starts the data receiver */
void V61Pump::rx_word_bit(s16 bit)
{
	rx_bits = (rx_bits << 1) | static_cast<u64>(bit & 1);
	if (++rx_nbits < 16)
		return;
	u16 w = static_cast<u16>(rx_bits);
	if (rx_nbits >= 64 && word_ok(w, RB_ID) &&
	    !(w & RB_RESERVED) && static_cast<u16>(rx_bits >> 48) == w) {
		u16 w2 = static_cast<u16>(rx_bits >> 32);
		u16 w3 = static_cast<u16>(rx_bits >> 16);
		if (!word_ok(w2, RB_B4) || !word_ok(w3, RB_B8))
			return;
		rx_word_align = static_cast<s16>(rx_nbits & 15);
		if (rxp == RX_R && w == rx_r[0] && w2 == rx_r[1] &&
		    w3 == rx_r[2])
			return;
		rx_r[0] = w;
		rx_r[1] = w2;
		rx_r[2] = w3;
		r_words[0] = w;
		r_words[1] = w2;
		r_words[2] = w3;
		ev_r = 1;
		rxp = RX_R;
		DSP_DBG("v61: R %04X %04X %04X, mse %.4f (%.1f dB)\n", w, w2,
			w3, rx_mse, -10 * log10f(rx_mse + 1e-6f));
	} else if (rxp == RX_R && e_ok(w) &&
		   (rx_nbits & 15) == rx_word_align && rx_mse < 0.1f &&
		   !rx_e_seen) {
		rx_e_seen = 1;
		rx_rate = rate_of_word(w);
		ev_e = 1;
		rxp = RX_DATA;
		lost_mse = lost_limit(rx_rate);
		lost_syms = 0;
		/* the frames: E, 50 symbols of B1, the first control segment */
		rx_n = 0;
		rx_pos = static_cast<s16>(ctrl_at - V61_CT_DELAY);
		rx_cur_ad = 0;
		rx_next_ad = 0;
		rx_g[0] = rx_g[1] = 1;
		rx_bad_ctrl = 0;
		aud_rx.init(baud);
		ear_n = 0;
		rx.kt_p = 0.004f * rx.sps;
		rx.kt_gi = 0.003f;
		rx.agc_hold = 1;
		DSP_DBG("v61: E %04X, rx rate %" PRId32 "\n", w,
			rx_rate->bps);
	}
}

/* the far end's speech of one symbol to the voice path, 10 ms at a
   time */
void V61Pump::ear_put(cplx a)
{
	s16 out[4];
	s16 n = aud_rx.push(a, out);
	if (!voice || !voice_open)
		return;
	for (s16 i = 0; i < n; i++) {
		ear_buf[ear_n++] = out[i];
		if (ear_n == 80) {
			voice->ear(voice->ctx, ear_buf, 80);
			ear_n = 0;
		}
	}
}

/* a control segment received (rx_ctrl, laid out as frame rx_cur_ad's):
   the next frame's kind and gains, its checks */
void V61Pump::rx_control_end()
{
	const u8 *c = rx_ctrl;
	auto get = [c](s16 at, s16 nb) -> s16 {
		s16 v = 0;
		for (s16 i = 0; i < nb; i++)
			v = static_cast<s16>(v << 1 | c[at + i]);
		return v;
	};
	s16 d, dbar, ok;
	s16 base = 0, delta = 0, gf = 0, lock = 0;
	if (baud == 3000) {
		ok = get(1, 7) == 0x6E;
		d = c[14];
		dbar = c[23];
		if (rx_cur_ad) {
			lock = c[15] | !c[24];
			/* G4 G3 G2 and their inverse: towards the larger */
			base = static_cast<s16>(get(16, 5) |
						((~get(25, 3) & 7) << 2));
			delta = get(21, 2);
			gf = c[28];
			ok = ok && c[15] != c[24];
		} else
			ok = ok && get(8, 4) == 0xE;
	} else {
		d = c[7];
		dbar = c[16];
		ok = 1;
		if (rx_cur_ad) {
			lock = c[8] | !c[17];
			base = static_cast<s16>(get(9, 5) |
						((~get(18, 2) & 3) << 3));
			delta = get(14, 2);
			gf = c[20];
			ok = c[8] != c[17];
		} else
			ok = get(1, 4) == 0xE;
	}
	ok = ok && d != dbar;
	/* the D bit, its inverse and its copies: the majority, a tie data
	   alone (Note 1 of Table 6) */
	s16 votes = static_cast<s16>(d + !dbar + rx_dones);
	rx_next_ad = 2 * votes < 2 + rx_dcopies;
	/* after data alone (no lock bit sent): locked */
	rx_lock = rx_cur_ad ? lock : 1;
	if (rx_cur_ad) {
		float gb = gain_of(base), gd = gain_of(static_cast<s16>(
						     base + delta));
		rx_ng[0] = gf ? gd : gb;
		rx_ng[1] = gf ? gb : gd;
	} else
		rx_ng[0] = rx_ng[1] = 1;
	rx_bad_ctrl = ok ? 0 : rx_bad_ctrl + 1;
	if (!ok)
		DSP_TRACE("v61: control segment fails its checks (%s)\n",
			  rx_cur_ad ? "speech" : "data alone");
}

/* one data mode symbol z */
void V61Pump::rx_data_symbol(cplx z)
{
	const s16 ri = static_cast<s16>(rx_rate - v61_rates);
	const bool ctrl = rx_pos >= ctrl_at;
	s16 n = rx_cur_ad ? 2 : rx_rate->nbits;
	s16 q, quad, k;
	cplx d;
	cplx aud = { 0, 0 };

	if (rx_cur_ad) {
		/* the quadrant: the point nearest in angle */
		float best = -1e30f;
		quad = 0;
		for (k = 0; k < 4; k++) {
			float p = z.re * pts_ad[k].re + z.im * pts_ad[k].im;
			if (p > best) {
				best = p;
				quad = k;
			}
		}
		d = pts_ad[quad];
		/* the speech: back from the data point's turn, the
		   asymmetric scale, the alternate inversion and the gain */
		cplx e = cplx_mulc(cplx_sub(z, d), rot_ad[quad]);
		float u = e.re / (e.re > 0 ? 2 * aud_neg : aud_neg);
		float v = e.im / (e.im > 0 ? 2 * aud_neg : aud_neg);
		if (rx_n & 1) {
			u = -u;
			v = -v;
		}
		aud = cplx_make(u / rx_g[0], v / rx_g[1]);
		k = y_of_quad[quad];
	} else {
		k = nearest_point(pts[ri], static_cast<s16>(1 << n), z);
		d = pts[ri][k];
		quad = quad_of_y[k & 3];
	}
	/* B1: the far end's scrambled ones, known (its scrambler is our
	   descrambler's register, its differential code our last
	   quadrant) */
	const bool b1 = rx_n < V61_B1_SYMS && !ctrl;
	if (b1) {
		u32 reg = rx_scr;
		s16 qb = 0;
		for (s16 i = 0; i < n; i++)
			qb |= static_cast<s16>(scramble(&reg, rx_scr_tap, 1)
					       << i);
		s16 bq = (rx_quad + turn_of_q[qb & 3]) & 3;
		if (!rx_cur_ad)
			d = pts[ri][y_of_quad[bq] | (qb & ~3)];
	}

	/* adaption: data alone at full speed; with speech the equalizer
	   only while the speech is weak (the lock bit clear), the carrier
	   slowly */
	if (!rx_cur_ad) {
		rx_mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / V61_POWER -
				   rx_mse);
		if (rx_mse > lost_mse)
			lost_syms++;
		else
			lost_syms = 0;
		rx.adapt(d, b1 ? 0.04f : 0.02f, 0.02f, 0.0002f);
	} else if (!rx_lock)
		rx.adapt(d, 0.002f, 0.01f, 0.0001f);
	else
		rx.adapt(d, 0, 0.003f, 0);

	/* the bits */
	s16 turn = (quad - rx_quad) & 3;
	rx_quad = quad;
	q = static_cast<s16>(q_of_turn[turn] | (k & ~3));
	u8 bits[8];
	for (s16 i = 0; i < n; i++)
		bits[i] = static_cast<u8>(descramble(&rx_scr, rx_scr_tap,
						     (q >> i) & 1));
	if (ctrl) {
		s16 j = static_cast<s16>(rx_pos - ctrl_at);
		rx_ctrl[2 * j + 2] = bits[0];
		rx_ctrl[2 * j + 1] = bits[1];
		if (n > 2) {
			rx_dcopies++;
			rx_dones = static_cast<s16>(rx_dones + bits[2]);
		}
	} else if (state == V61_DATA)
		modem_put_bits(modem, 1, bits, n);

	ear_put(aud);

	rx_n++;
	if (++rx_pos == V61_FRAME) {
		rx_control_end();
		rx_dcopies = rx_dones = 0;
		rx_pos = 0;
		rx_cur_ad = rx_next_ad;
		rx_g[0] = rx_ng[0];
		rx_g[1] = rx_ng[1];
	}
}

/* one received symbol (rx.z) */
void V61Pump::rx_symbol()
{
	cplx z = rx.z, st;
	s16 k, sgn;
	float a;

	/* our own training on the line (the echo canceller learns) */
	if (txm == TXM_S || txm == TXM_SBAR || txm == TXM_TRN) {
		run_s = 0;
		last_z = z;
		return;
	}
	if (rxp != RX_DATA) {
		k = nearest_point(pts4, 4, z);
		rx_mse += 0.05f * (cplx_pow(cplx_sub(z, pts4[k])) /
				   V61_POWER - rx_mse);
		/* S: +-90 degree steps of alternating sign; the first S-bar
		   symbol (C) repeats the sign */
		st = cplx_mulc(z, last_z);
		a = atan2f(st.im, st.re);
		sgn = a > 0 ? 1 : -1;
		if (fabsf(a) > 0.9f && fabsf(a) < 2.2f && carrier.on &&
		    state != C_DCBA && state != C_SRD_KIND && state != A_ANS &&
		    state != A_SRD_END) {
			if (sgn != last_sgn) {
				if (++run_s >= 24 && rxp != RX_S) {
					if (rxp != RX_IDLE)
						rx.reset();
					rx.agc_hold = 0;
					rxp = RX_S;
					ev_s = 1;
					DSP_DBG("v61: S detected\n");
				}
			} else if (rxp == RX_S && run_s >= 24) {
				rx.align_phase(pts4[2]);
				rxp = RX_TRN;
				rx_quad = 2;
				rx_nbits = 0;
				rx_r[0] = rx_r[1] = rx_r[2] = 0;
				run_s = 0;
				DSP_DBG("v61: S-bar detected\n");
			} else
				run_s = 0;
		} else
			run_s = 0;
		last_sgn = sgn;
		if (rxp == RX_S) {
			if (run_s >= 24)
				s_miss = 0;
			else if (++s_miss >= 24) {
				rxp = RX_TRN;
				rx_nbits = 0;
				rx_r[0] = rx_r[1] = rx_r[2] = 0;
				DSP_DBG("v61: end of S\n");
			}
		}
	}

	switch (rxp) {
	case RX_IDLE:
	case RX_S:
		k = nearest_point(pts4, 4, z);
		rx.adapt(pts4[k], 0, 0.05f, 0);
		break;
	case RX_TRN:
	case RX_R: {
		k = nearest_point(pts4, 4, z);
		if (rx_mse < 0.15f)
			rx.adapt(pts4[k], rx_mse > 0.01f ? 0.04f : 0.02f,
				 0.05f, 0.0005f);
		else
			rx.adapt(pts4[k], 0.002f, 0.05f, 0);
		s16 turn = (k - rx_quad) & 3;
		rx_quad = k;
		s16 q = q_of_turn[turn];
		rx_word_bit(descramble(&rx_scr, rx_scr_tap, q & 1));
		rx_word_bit(descramble(&rx_scr, rx_scr_tap, q >> 1));
		break;
	}
	case RX_DATA:
		rx_data_symbol(z);
		if (rx_n % 300 == 0)
			DSP_TRACE("%s data mse %.4f t %.3f tadj %.5f th %.2f "
				  "dth %.5f\n", caller ? "C" : "A", rx_mse,
				  rx.t_next, rx.t_adj, rx.theta, rx.dtheta);
		break;
	}
	last_z = z;
}

/* one received sample: the echo canceller, the tone trackers, the QAM
   receiver, the carrier */
void V61Pump::rx_sample(float x)
{
	x = ec.run(last_tx, x, ec_mu);

	/* our answer tone's echo is not what the caller sends: the
	   trackers judge the caller's tones against the rest */
	float pw = rx.power;
	if (txm == TXM_ANS)
		pw = fmaxf(pw - 2 * cplx_pow(ans2100.lp2), 0.05f * pw);
	t1800.run(x, pw, srate);
	t3300.run(x, pw, srate);
	t300.run(x, pw, srate);
	t1050.run(x, rx.power, srate);
	ans2100.run(x, rx.power, srate);
	if (caller) {
		if (f2100.run(x, rx.power, srate) && baud == 3000)
			ev_frev = 1;
		if (f1960.run(x, rx.power, srate) && baud == 2800)
			ev_frev = 1;
	} else {
		if (g1500.run(x, rx.power, srate) && baud == 3000)
			ev_grev = 1;
		if (g1400.run(x, rx.power, srate) && baud == 2800)
			ev_grev = 1;
	}

	if (rx.sample(x))
		rx_symbol();

	switch (carrier.run(rx.power)) {
	case 1:
		if (rxp == RX_IDLE)
			rx.reset();
		break;
	case -1:
		if (rxp != RX_DATA)
			rx_reset();
		break;
	}
	carrier_tick(carrier.on);
}

/*
 *    handshake
 */

/* the voice path on or off (once each) */
void V61Pump::voice_state(bool on)
{
	if (!voice || (voice_open != 0) == on)
		return;
	voice_open = on;
	voice->state(voice->ctx, on);
	DSP_DBG("v61: voice %s\n", on ? "on" : "off");
}

/* to data, the rates to the core */
s16 V61Pump::connect()
{
	set_state(V61_DATA);
	data_power = rx.power;
	s32 t = tx_ad_on ? 4800 : tx_rate->bps;
	s32 r = tx_ad_on ? 4800 : rx_rate->bps;
	if (tx_ad_on)
		voice_state(true);
	if (retraining) {
		set_rates(t, r);
		retraining = 0;
		DSP_DBG("v61: retrained, %" PRId32 "/%" PRId32 "\n", t, r);
		return DPSTAT_OK;
	}
	DSP_DBG("v61: %s, data alone at %" PRId32 "\n",
		tx_ad_on ? "voice plus data" : "data", tx_rate->bps);
	return connect_at(t, r);
}

/* a retrain (clause 10): the caller's G, the answerer's F; the far end
   answers in kind when it hears it */
void V61Pump::retrain(bool remote)
{
	DSP_DBG("v61: retrain %d (%s)\n", ++retrains, caller ? "G" : "F");
	stat_retrain(remote);
	retraining = 1;
	rx_reset();
	ev_r = ev_e = ev_s = ev_frev = ev_grev = 0;
	timeout = total + ms2samples(15000, srate);
	tone.set_freq(tone_freq = caller ? (baud == 2800 ? 1400.0f : 1500.0f)
				  : (baud == 2800 ? 1960.0f : 2100.0f), srate);
	tone_sign = 1;
	g_on_syms = 0;
	set_tx(TXM_TONE);
	set_state(caller ? C_G : A_F);
}

/* (see the table) the data mse past which the receiver at rate r has
   lost its equalization: 4 dB short of its SNR (4800: 7 dB) */
float V61Pump::lost_limit(const struct v61_rate *r)
{
	s16 i = static_cast<s16>(r - v61_rates);
	float snr = v61_min_snr[i] > 0 ? v61_min_snr[i] - 4 : 7;
	return powf(10, -snr / 10);
}

/* the rates our receiver can take, judged on the training */
u16 V61Pump::quality_caps()
{
	float snr = -10 * log10f(rx_mse + 1e-6f);
	u16 c = 0;
	for (s16 i = 0; i < V61_NRATES; i++)
		if (snr >= v61_min_snr[i])
			c |= v61_rates[i].bit;
	return caps & (c | RB_4800);
}

/* once a symbol, before it is made (every sample in the answer tone) */
s16 V61Pump::handshake()
{
	s32 sym = tx_syms;
	const struct v61_rate *r;

	switch (state) {
	/* answering modem */
	case A_SILENCE:
		if (count >= ms2samples(ANS_SILENCE_MS, srate)) {
			set_state(A_ANS);
			set_tx(TXM_ANS);
		}
		break;
	case A_ANS:
		/* until the caller's S or AA (1800 Hz) has been there for
		   20 ms; then silence */
		if (t1800.on && cplx_pow(t1800.lp2) >
		    0.1f * cplx_pow(ans2100.lp2)) {
			if (++srd_seen > ms2samples(30, srate)) {
				bool s = srd_is_s(t1800, t300, t3300);
				tx_srd_s = s && !want_2800;
				DSP_DBG("v61: the caller's %s\n", s ? "S" : "AA");
				set_state(A_SRD_END);
				set_tx(TXM_SILENCE);
			}
		} else
			srd_seen = 0;
		if (count >= ms2samples(6000, srate))
			return DPSTAT_ERROR;
		break;
	case A_SRD_END:
		/* the caller's S or AA turns DCBA (1050 Hz alone) */
		if (!t1800.on && t1050.on) {
			set_state(A_SRD);
			set_tx(TXM_SRD);
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case A_SRD:
		if (sym >= 256) {
			set_state(A_WAIT_G);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_WAIT_G:
		/* G: 1500 Hz for 3000 symbols/s, 1400 for 2800 */
		if ((g1500.on || g1400.on) && count > ms2samples(10, srate)) {
			s32 b = cplx_pow(g1400.lp2) > cplx_pow(g1500.lp2) ?
				2800 : 3000;
			if (b == 3000 && !tx_srd_s)
				b = 2800;
			if (b != baud)
				init_rate(b);
			DSP_DBG("v61: G at %d Hz\n", b == 2800 ? 1400 : 1500);
			tone.set_freq(tone_freq = b == 2800 ? 1960.0f : 2100.0f,
				      srate);
			tone_sign = 1;
			g_on_syms = 0;
			set_state(A_F);
			set_tx(TXM_TONE);
		} else if (count >= ms2samples(3000, srate))
			return DPSTAT_ERROR;
		break;
	case A_F:
		/* F for 128T at least and G heard for 64T: the reversal,
		   the round trip's count from it */
		if ((baud == 2800 ? g1400.on : g1500.on))
			g_on_syms++;
		else
			g_on_syms = 0;
		if (sym >= 128 && g_on_syms >= 64) {
			tone_sign = -tone_sign;
			ev_grev = 0;
			mt_syms = 0;
			set_state(A_F_REV);
		} else if (count >= ms2samples(5000, srate))
			return DPSTAT_ERROR;
		break;
	case A_F_REV:
		mt_syms++;
		if (ev_grev) {
			ev_grev = 0;
			tx_syms = 0;
			DSP_DBG("v61: MT %" PRId32 " symbols\n", mt_syms);
			set_state(A_F_REV2);
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case A_F_REV2:
		/* 64T after G's reversal reached the line (our detector
		   sees it about 30 symbols late) */
		if (sym >= 34) {
			tone_sign = -tone_sign;
			set_state(A_F2);
		}
		break;
	case A_F2:
		/* until the caller's G stops */
		if (!(baud == 2800 ? g1400.on : g1500.on) && count >
		    ms2samples(20, srate)) {
			set_state(A_QUIET);
			set_tx(TXM_SILENCE);
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case A_QUIET:
		if (sym >= 16) {
			set_state(A_TRAIN1);
			set_tx(TXM_S);
			ev_s = 0;
		}
		break;
	case A_TRAIN1:
	case A_TRAIN2:
	case C_TRAIN:
		if (txm == TXM_S && sym >= 256 + (state == C_TRAIN ? nt_syms : 0))
			set_tx(TXM_SBAR);
		else if (txm == TXM_SBAR && sym >= 16) {
			set_tx(TXM_TRN);
			tx_scr = 0;
			if (state != A_TRAIN2)
				ec_mu = 0.25f;
		} else if (txm == TXM_TRN && ec_mu > 0 &&
			   sym == V61_TRN_LEN / 2) {
			/* the canceller's step smaller for the fine part */
			ec_mu = 0.05f;
		} else if (txm == TXM_TRN && ec_mu > 0 &&
			   sym == 3 * V61_TRN_LEN / 4) {
			ec_mu = 0.02f;
		} else if (txm == TXM_TRN && sym >= V61_TRN_LEN) {
			if (ec_mu > 0) {
				float erle = ec.judge();
				DSP_DBG("v61: echo canceller took %.1f dB "
					"away%s\n", erle, ec.off ? ": off" : "");
			}
			ec_mu = 0;
			if (state == A_TRAIN1) {
				set_state(A_R1);
				start_words(TXM_R, W1(caps), W2(voice_ok),
					    W3(1));
			} else if (state == A_TRAIN2) {
				set_state(A_R3);
				start_words(TXM_R, W1(tx_rate->bit),
					    W2(tx_ad_on), W3(1));
				ev_e = 0;
			} else {
				set_state(C_R2);
				start_words(TXM_R, W1(r_words[0] & r2_caps),
					    W2(voice_ok && (r_words[1] & RB_AD)),
					    W3(1));
				ev_r = 0;
				ev_s = 0;
			}
		}
		break;
	case A_R1:
		if (ev_s && tx_word_bit == 0) {
			set_state(A_WAIT_R2);
			set_tx(TXM_SILENCE);
			ev_r = 0;
		}
		break;
	case A_WAIT_R2:
		if (ev_r) {
			ev_r = 0;
			r = rate_of_word(r_words[0] & quality_caps());
			if (!r) {
				DSP_DBG("v61: no common rate in %04X\n",
					r_words[0]);
				return DPSTAT_ERROR;
			}
			tx_rate = r;
			tx_ad_on = voice_ok && (r_words[1] & RB_AD);
			tx_silence_ok = (r_words[2] & RB_SILENCE) != 0;
			set_state(A_TRAIN2);
			set_tx(TXM_S);
		}
		break;
	case A_R3:
		if (ev_e && tx_word_bit == 0) {
			set_state(A_E);
			start_words(TXM_E, W1(tx_rate->bit) | RB_HEADER, 0, 0);
		}
		break;
	case A_E:
	case C_E:
		if (txm == TXM_E && sym >= 8) {
			set_tx(TXM_DATA);
			tx_n = 0;
			tx_pos = static_cast<s16>(ctrl_at - V61_CT_DELAY);
			tx_cur_ad = tx_next_ad = 0;
			tx_gi = tx_gq = tx_ngi = tx_ngq = 0;
			tx_quiet = 0;
			aud_tx.init(baud);
			for (cplx &a : aud_ring)
				a = cplx_make(0, 0);
			mic_n = mic_head = 0;
			mic_primed = 0;
		}
		if (txm == TXM_DATA && rxp == RX_DATA)
			return connect();
		break;

	/* calling modem */
	case C_ANS:
		/* the answer tone for 1 s (call progress has heard some of
		   it), then our S */
		if (ans2100.on && ans2100.on_count > ms2samples(
			    rt && rt->answer_tone ? 900 : 1000, srate)) {
			set_state(C_SRD);
			set_tx(TXM_SRD);
		} else if (count >= ms2samples(8000, srate))
			return DPSTAT_ERROR;
		break;
	case C_SRD:
		if (sym >= 256) {
			set_state(C_DCBA);
			set_tx(TXM_DCBA);
			/* (not our own S's echo) */
			srd_seen = -256;
		}
		break;
	case C_DCBA:
		/* the answer tone over, 256T, then the answerer's S or AA
		   (1800 Hz) for 30 ms */
		if (ans2100.on)
			srd_seen = -256;
		else if (srd_seen < 0)
			srd_seen++;
		else if (t1800.on && t1800.on_count > ms2samples(30, srate))
			set_state(C_SRD_KIND);
		if (count >= ms2samples(5000, srate))
			return DPSTAT_ERROR;
		break;
	case C_SRD_KIND: {
		bool s = srd_is_s(t1800, t300, t3300);
		{
			s32 b = s && !want_2800 ? 3000 : 2800;
			DSP_DBG("v61: the answerer's %s\n", s ? "S" : "AA");
			if (b != baud)
				init_rate(b);
			tone.set_freq(tone_freq = b == 2800 ? 1400.0f : 1500.0f,
				      srate);
			tone_sign = 1;
			ev_frev = 0;
			set_state(C_G);
			set_tx(TXM_TONE);
		}
		break;
	}
	case C_G:
		/* F, then its reversal */
		if (ev_frev) {
			ev_frev = 0;
			tx_syms = 0;
			set_state(C_G_REV);
		} else if (count >= ms2samples(5000, srate))
			return DPSTAT_ERROR;
		break;
	case C_G_REV:
		/* ours 64T after F's reached the line */
		if (sym >= 34) {
			tone_sign = -tone_sign;
			tx_syms = 0;
			ev_frev = 0;
			/* the next reversal may come within the trackers'
			   hold */
			f2100.hold = f1960.hold = 0;
			set_state(C_G2);
		}
		break;
	case C_G2:
		/* until F's second reversal: NT, then silence */
		if (ev_frev) {
			nt_syms = sym;
			DSP_DBG("v61: NT %" PRId32 " symbols\n", nt_syms);
			set_state(C_WAIT_R1);
			set_tx(TXM_SILENCE);
			ev_r = 0;
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case C_WAIT_R1:
		if (ev_r) {
			ev_r = 0;
			r2_caps = quality_caps();
			DSP_DBG("v61: rates we can receive: %04X\n", r2_caps);
			set_state(C_TRAIN);
			set_tx(TXM_S);
		}
		break;
	case C_R2:
		/* R2 until R3 (R1 again until the answerer's second
		   training): its one rate */
		if (ev_r && tx_word_bit == 0) {
			ev_r = 0;
			r = rate_of_word(r_words[0] & r2_caps);
			if (!r || !ev_s ||
			    (r_words[0] & RB_RATES) != r->bit)
				break;
			tx_rate = r;
			tx_ad_on = voice_ok && (r_words[1] & RB_AD);
			tx_silence_ok = (r_words[2] & RB_SILENCE) != 0;
			set_state(C_E);
			start_words(TXM_E, W1(r->bit) | RB_HEADER, 0, 0);
		}
		break;

	case V61_DATA:
		/* the far end retrains: its G or F for 128T */
		if ((!caller && (baud == 2800 ? g1400 : g1500).on_count >
		     srate * 128 / baud) ||
		    (caller && (baud == 2800 ? f1960 : f2100).on_count >
		     srate * 128 / baud))
			retrain(true);
		else if (retrain_at && count >= retrain_at && !retrains)
			retrain(false);
		/* the equalization lost: 2 s past its rate's limit, or the
		   control segments failing for 1 s, the signal there */
		else if ((lost_syms >= 2 * baud || rx_bad_ctrl >= baud / 70) &&
			 rx.power > 0.5f * data_power && !lost_off) {
			DSP_DBG("v61: equalization lost (mse %.4f, %" PRId32
				" bad control segments)\n", rx_mse,
				rx_bad_ctrl);
			lost_syms = 0;
			rx_bad_ctrl = 0;
			retrain(false);
		} else if (count % ms2samples(100, srate) == 0 &&
			   rx.power > 0.5f * data_power) {
			float b = static_cast<float>(baud);
			float fc = baud == 2800 ? 1680.0f : 1800.0f;
			stat_snr(-10 * log10f(rx_mse + 1e-6f));
			stat_line(b, fc, b, fc);
			stat_freq_offset(rx.dtheta, b);
			stat_timing_offset(rx.t_adj, rx.sps);
		}
		break;
	}
	return DPSTAT_OK;
}

/* a sample in, one out */
s16 V61Pump::sample(float x, float *y)
{
	s16 r = DPSTAT_OK;

	if (txm != TXM_ANS && tx.t + tx.step >= 1) {
		r = handshake();
		if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
			return r;
	} else if (txm == TXM_ANS || txm == TXM_SILENCE) {
		s16 h = handshake();
		if (h == DPSTAT_CHANGEDP || h == DPSTAT_ERROR)
			return h;
	}

	if (txm == TXM_ANS)
		*y = V61_TX_AMPLITUDE * ans.next();
	else {
		float q = tx.sample([this] { return tx_symbol(); });
		if (txm == TXM_TONE)
			*y = tone_sign * V61_TX_AMPLITUDE * tone.next();
		else if (txm == TXM_SILENCE)
			*y = 0;
		else
			*y = q;
	}
	last_tx = *y;

	rx_sample(x);
	return r;
}

/* V.61, the rates limited by +MS; 0, or -1 when out of memory */
s16 V61Pump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_,
		   s32)
{
	pump_setup(m, srate_, caller_);

	s32 min_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MIN_RATE));
	s32 max_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));
	for (s16 i = 0; i < V61_NRATES; i++) {
		const struct v61_rate *r = &v61_rates[i];
		if (max_rate > 0 && r->bps > max_rate)
			continue;
		if (min_rate > 0 && r->bps < min_rate)
			continue;
		caps |= r->bit;
	}
	if (!caps)
		caps = RB_4800;

	intptr_t v = modem_get_param(m, MDMPRM_VOICE);
	voice = v == -1 || v == 0 ? nullptr :
		reinterpret_cast<struct dp_voice *>(v);
	voice_ok = voice != nullptr && !env_off("DSP_V61_VOICE");
	want_2800 = env_int("DSP_V61_BAUD") == 2800;

	make_points();
	tx_srd_s = !want_2800;
	ans.set_freq(2100, srate_);
	carrier.init(150, 90);
	baud = 3000;
	ctrl_at = 56;
	tx.init(srate_, 1800, 3000, V61_ALPHA, V61_SPAN, sqrtf(V61_POWER),
		V61_TX_AMPLITUDE);
	tx.t = 0.25f;
	if (rx.init(srate_, 1800, 3000, V61_ALPHA, V61_SPAN, V61_EQ_LEN,
		    V61_POWER) ||
	    ec.init(V61_EC_LEN))
		return -1;
	t1800.init(1800, srate_);
	t3300.init(3300, srate_);
	t300.init(300, srate_);
	t1050.init(1050, srate_);
	ans2100.init(2100, srate_);
	g1500.init(1500, srate_);
	g1400.init(1400, srate_);
	f2100.init(2100, srate_);
	f1960.init(1960, srate_);

	tx_scr_tap = caller_ ? SCR_GPC : SCR_GPA;
	rx_scr_tap = caller_ ? SCR_GPA : SCR_GPC;
	tx_rate = &v61_rates[V61_NRATES - 1];
	rx_rate = tx_rate;

	/* tests: DSP_V61_RETRAIN=<ms in data>[a|c], DSP_V61_LOST=0 */
	retrain_at = static_cast<s32>(test_samples("DSP_V61_RETRAIN"));
	lost_off = env_off("DSP_V61_LOST");

	if (caller_) {
		state = C_ANS;
		txm = TXM_SILENCE;
	} else if (rt && rt->ans_sent) {
		rt->ans_sent = rt->after_v8 = 0;
		state = A_ANS;
		txm = TXM_ANS;
	} else {
		state = A_SILENCE;
		txm = TXM_SILENCE;
	}
	DSP_DBG("v61: create %s, rates %04X, voice %s, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", caps,
		voice_ok ? "yes" : "no", srate_);
	return 0;
}

/* the voice path closed, the buffers freed */
void V61Pump::cleanup()
{
	voice_state(false);
	rx.release();
	ec.release();
}

/* registers the pump as DP_V61 */
s16 dp_v61_init(void)
{
	return modem_dp_register(DP_V61, &PumpOps<V61Pump>::ops);
}

/* and no more */
void dp_v61_exit(void)
{
	modem_dp_deregister(DP_V61, &PumpOps<V61Pump>::ops);
}

} // namespace dsp
