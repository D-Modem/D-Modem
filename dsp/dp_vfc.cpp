/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_vfc.cpp - Rockwell's V.FC ("V.Fast Class"), with its own start-up
 *
 *    V.FC was never published.  What this pump sends is what the DSP of
 *    a USRobotics Courier V.Everything sends (IDSDL302, overlay 7), as
 *    read from its firmware and checked against it run in an emulator
 *    (docs/dsp/vfc-details.md).  The start-up has four phases:
 *
 *    1  the answerer sends ANS (2100 Hz); the caller, about 1.4 s into
 *       it, sends 800 Hz for 300 ms; the answerer hears it and stops
 *    2  both at 800 baud, full duplex, differential QPSK: reversals,
 *       a probe, then repeated 40-bit messages (the symbol rates each
 *       side has, and other values), until each has the other's last
 *    3  at the symbol rate chosen (the highest both have): the answerer
 *       trains the caller's receiver, then the caller the answerer's
 *    4  both again at once: a short training, a table, the data rates
 *       each allows, and the rate chosen; then the data (vfc_data.cpp)
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
#include "v34.h"

namespace dsp {

#define VFC_TX_AMPLITUDE (4900.0f * 1.41421f)

/* phase 2: 800 baud */
#define VFC_P2_BAUD     800
#define VFC_P2_ALPHA    0.5f
#define VFC_P2_SPAN     6

/* phases 3, 4 and the data */
#define VFC_ALPHA       0.12f
#define VFC_SPAN        16
#define VFC_EQ_LEN      32
/* the fastest rate we offer, in 2400 bit/s: 26400.  Our receiver's
   equalized signal is about 30 dB above its noise, which is not enough
   for 28800 (two D-Modems lose bits there, and so does the D-Modem with
   the Courier's 28800) */
#define VFC_TOP_N       11
#define VFC_POWER       2.0f    /* the training points' */

/* the symbol rates, by the index the capability bits use (V.34's
   order): the Courier's are its codec's rates / 3 */
struct vfc_baud {
	float baud, carrier;
	s16 v34;                /* V.34's index (vfc_data.cpp) */
};

static const struct vfc_baud vfc_bauds[] = {
	{ 2400.0f, 1800.0f, 0 },
	{ 52000.0f / 19, 1828.0f, 1 },
	{ 2800.0f, 1866.7f, 2 },
	{ 56800.0f / 19, 1993.0f, 3 },
	{ 3200.0f, 1920.0f, 4 },
	{ 30800.0f / 9, 30800.0f / 9 * 4 / 7, 5 },
};
#define VFC_NBAUDS (s16)std::size(vfc_bauds)

/* the QPSK points of the training (Courier mask ROM 0x248), in units
   whose mean power is 2 (the data's, v34.h): (3, 1) / sqrt(5) */
static const float vfc_r5 = 0.4472136f;
static const cplx vfc_qpsk[4] = {
	{ -3 * vfc_r5, -1 * vfc_r5 }, { -1 * vfc_r5, 3 * vfc_r5 },
	{ 1 * vfc_r5, -3 * vfc_r5 }, { 3 * vfc_r5, 1 * vfc_r5 },
};

/* the differential code (mask ROM 0x450): the next point from the
   dibit d and the last point p, vfc_diff[d << 2 | p] */
static const s16 vfc_diff[16] = {
	2, 0, 3, 1, 3, 2, 1, 0, 0, 1, 2, 3, 1, 3, 0, 2,
};
/* the dibit of a turn by k quarter turns counter clockwise */
static const s16 vfc_turn_dibit[4] = { 2, 0, 1, 3 };

/* phase 2's probe (0xa74c), points */
static const s16 vfc_probe[16] = {
	0, 0, 0, 0, 0, 2, 3, 1, 0, 3, 0, 3, 0, 1, 3, 2,
};

/* the final rate words (0xb7c7) by data rate index (14400 .. 28800) and
   the rate mask's bit of each */
static const u16 vfc_final[7] = {
	0x004a, 0x010a, 0x020a, 0x040a, 0x100a, 0x200a, 0x400a,
};
static const u16 vfc_rate_bit[7] = {
	0x0040, 0x0100, 0x0200, 0x0400, 0x1000, 0x2000, 0x4000,
};
#define VFC_RATE_BITS   0x7740u
#define VFC_MASK_FIXED  0x8885u
#define VFC_FINAL_FIXED 0x000au

/* phase 2's messages: their sync and the values the Courier sends */
#define VFC_SYNC        0xd849u
#define VFC_T0_OTHER    0x100   /* type 0: besides the symbol rates */
#define VFC_T5          0x255
#define VFC_T1          0x083
#define VFC_T2          0x170
#define VFC_T3          0x000
#define VFC_T7_DONE     0x07f

/* the scrambler (0x8cc2): n bits a symbol into a 32-bit register at
   bit 23, the taps at bits 0 and tap (5: 1 + x^-18 + x^-23, the
   caller's; 18: 1 + x^-5 + x^-23, the answerer's) */
struct VfcScr {
	u32 r;
	s16 tap;

	/* n bits of in scrambled */
	s16 scramble(s16 in, s16 n)
	{
		s16 s = static_cast<s16>(((r >> tap) ^ r ^
					 static_cast<u32>(in)) & ((1u << n) - 1));
		r = (r | static_cast<u32>(s) << 23) >> n;
		return s;
	}
	/* n bits of s descrambled */
	s16 descramble(s16 s, s16 n)
	{
		s16 d = static_cast<s16>(((r >> tap) ^ r ^
					 static_cast<u32>(s)) & ((1u << n) - 1));
		r = (r | static_cast<u32>(s) << 23) >> n;
		return d;
	}
	/* the register as V.34's scrambler holds it (bits.h): the newest
	   bit at bit 0 */
	u32 v34() const
	{
		u32 v = 0;
		for (s16 i = 0; i < 23; i++)
			v |= ((r >> (22 - i)) & 1) << i;
		return v;
	}
};

/* the tap of each side's scrambler as VfcScr has it */
static s16 vfc_tap(s16 caller) { return caller ? 5 : 18; }
/* and as V.34's (bits.h) */
static s16 vfc_v34_tap(s16 caller) { return caller ? 18 : 5; }

/* a phase 2 message: its 20 dibits */
static void vfc_frame(s16 t, s16 v, s16 d[20])
{
	s16 c = static_cast<s16>(~((v >> 4) + (v >> 8) + v + t) & 0xf);
	for (s16 k = 0; k < 8; k++)
		d[k] = (VFC_SYNC >> (2 * k)) & 3;
	const s16 x[12] = {
		static_cast<s16>(t & 3), static_cast<s16>((t >> 2) & 3),
		static_cast<s16>(v & 3), static_cast<s16>((v >> 2) & 3),
		2, 3,
		static_cast<s16>((v >> 4) & 3), static_cast<s16>((v >> 6) & 3),
		static_cast<s16>((v >> 8) & 3), static_cast<s16>((v >> 10) & 3),
		static_cast<s16>(c & 3), static_cast<s16>((c >> 2) & 3),
	};
	for (s16 k = 0; k < 12; k++)
		d[8 + k] = x[k];
}

/*
 *    phase 2's steps: each a signal for a count of symbols, or until
 *    something is received
 */
enum p2_op { P2_SIL, P2_REV, P2_PROBE, P2_ONES0, P2_ONES, P2_MSG, P2_END };
enum vfc_until {
	U_NONE,
	U_REV2,         /* the answerer's second reversals */
	U_HEARD,        /* a message of the other's */
	U_T0,           /* the other's type 0 */
	U_T7,           /* the other's type 7, done */
	U_TRAINED,      /* our receiver trained (phase 4) */
	U_TABLE,        /* the other's table (phase 4) */
	U_MASK,         /* the other's rate mask (phase 4) */
};

struct p2_step {
	p2_op op;
	s16 carrier;
	s16 count;      /* symbols (messages: frames); at least, with until */
	vfc_until until;
	s16 type, value;
};

/* the caller's (0xa64b) */
static const p2_step vfc_p2_caller[] = {
	{ P2_SIL, 0, 8, U_NONE, 0, 0 },
	{ P2_REV, 1825, 80, U_NONE, 0, 0 },
	{ P2_PROBE, 1975, 0, U_REV2, 0, 0 },
	{ P2_SIL, 0, 10, U_NONE, 0, 0 },
	{ P2_REV, 1025, 80, U_NONE, 0, 0 },
	{ P2_ONES0, 1025, 80, U_NONE, 0, 0 },
	{ P2_MSG, 1025, 4, U_T0, 0, -1 },
	{ P2_MSG, 1025, 4, U_NONE, 5, VFC_T5 },
	{ P2_MSG, 1025, 4, U_NONE, 2, VFC_T2 },
	{ P2_MSG, 1025, 4, U_NONE, 3, VFC_T3 },
	{ P2_MSG, 1025, 1, U_T7, 7, VFC_T7_DONE },
	{ P2_END, 0, 0, U_NONE, 0, 0 },
};

/* the answerer's (0xa677) */
static const p2_step vfc_p2_answerer[] = {
	{ P2_SIL, 0, 72, U_NONE, 0, 0 },
	{ P2_REV, 2400, 80, U_NONE, 0, 0 },
	{ P2_PROBE, 1950, 176, U_NONE, 0, 0 },
	{ P2_SIL, 0, 10, U_NONE, 0, 0 },
	{ P2_REV, 2400, 80, U_NONE, 0, 0 },
	{ P2_ONES0, 2400, 80, U_NONE, 0, 0 },
	{ P2_ONES, 2400, 0, U_HEARD, 0, 0 },
	{ P2_MSG, 2400, 4, U_NONE, 0, -1 },
	{ P2_MSG, 2400, 4, U_NONE, 5, VFC_T5 },
	{ P2_MSG, 2400, 4, U_NONE, 1, VFC_T1 },
	{ P2_MSG, 2400, 4, U_NONE, 2, VFC_T2 },
	{ P2_MSG, 2400, 4, U_NONE, 3, VFC_T3 },
	{ P2_MSG, 2400, 4, U_T7, 7, 0 },
	{ P2_MSG, 2400, 3, U_NONE, 7, VFC_T7_DONE },
	{ P2_ONES, 2400, 4, U_NONE, 0, 0 },
	{ P2_END, 0, 0, U_NONE, 0, 0 },
};

/*
 *    phases 3 and 4: the script's steps (0xc576, 0xc58f, 0xc5a2)
 */
enum trn_op {
	T_SIL,          /* 0xc5ea */
	T_S,            /* 0xc601, 0xc60a: points 0 and 2 in turn */
	T_SB,           /* 0xc61a 3131: points 3 and 1 in turn */
	T_TWO,          /* 0xc62b, 0xc62f: the scrambler from 0, 2-phase */
	T_FOUR,         /* 0xc642: 4-phase */
	T_ONES,         /* 0xc660: ones, differential */
	T_ZEROS,        /* 0xc64b: zeros, differential */
	T_WORD,         /* 0xc674, 0xc678, 0xc682: a word, 2 bits a symbol
			   from bit 16 - 2 count up */
	T_MASK,         /* the rate mask (0xc682), a word */
	T_FINAL,        /* the rate chosen (0xc678), a word */
	T_DATA,
	T_END,
};

struct trn_step {
	trn_op op;
	u16 param;
	s16 count;
	vfc_until until;
};

static const trn_step vfc_p3_answerer[] = {
	{ T_SIL, 0, 16, U_NONE },
	{ T_S, 0, 250, U_NONE },
	{ T_S, 0, 256, U_NONE },
	{ T_SB, 0, 16, U_NONE },
	{ T_TWO, 0, 256, U_NONE },
	{ T_FOUR, 0, 1024, U_NONE },
	{ T_ONES, 0, 6912, U_NONE },
	{ T_ZEROS, 0, 344, U_NONE },
	{ T_END, 0, 0, U_NONE },
};

static const trn_step vfc_p3_caller[] = {
	{ T_S, 0, 250, U_NONE },
	{ T_S, 0, 256, U_NONE },
	{ T_SB, 0, 16, U_NONE },
	{ T_TWO, 0, 256, U_NONE },
	{ T_FOUR, 0, 1024, U_NONE },
	{ T_ONES, 0, 6912, U_NONE },
	{ T_ZEROS, 0, 344, U_NONE },
	{ T_END, 0, 0, U_NONE },
};

static const trn_step vfc_p4[] = {
	{ T_S, 0, 256, U_NONE },
	{ T_SB, 0, 16, U_NONE },
	{ T_TWO, 0, 256, U_NONE },
	{ T_FOUR, 0, 1024, U_NONE },
	{ T_ONES, 0, 16, U_NONE },
	{ T_ONES, 0, 1, U_TRAINED },
	/* (the Courier sends its table as soon as its receiver is
	   trained, and takes ours only after that: ours after its) */
	{ T_ONES, 0, 1, U_TABLE },
	{ T_WORD, 0xd9ab, 8, U_NONE }, { T_WORD, 0x0a0a, 4, U_NONE },
	{ T_WORD, 0, 8, U_NONE },
	{ T_WORD, 0xd9ab, 8, U_NONE }, { T_WORD, 0x0909, 4, U_NONE },
	{ T_WORD, 0, 8, U_NONE },
	{ T_WORD, 0xd9ab, 8, U_NONE }, { T_WORD, 0x0808, 4, U_NONE },
	{ T_WORD, 0, 8, U_NONE },
	{ T_WORD, 0xd9ab, 8, U_NONE }, { T_WORD, 0x0202, 4, U_NONE },
	{ T_WORD, 0, 8, U_NONE },
	{ T_WORD, 0xd9ab, 8, U_NONE }, { T_WORD, 0x0101, 4, U_NONE },
	{ T_WORD, 0, 8, U_NONE },
	{ T_WORD, 0xd9ab, 8, U_NONE }, { T_WORD, 0x0000, 4, U_NONE },
	{ T_WORD, 0, 8, U_NONE },
	{ T_ONES, 0, 4, U_NONE },
	{ T_WORD, 0xaaaa, 4, U_NONE },
	{ T_MASK, 0, 64, U_MASK },
	{ T_FINAL, 0, 32, U_NONE },
	{ T_DATA, 0, 0, U_NONE },
};

/* the training's symbols as the script makes them: the transmitter's,
   and the receiver's copy of the other's */
struct VfcTrn {
	VfcScr scr;
	s16 prev;               /* the last point */
	s16 phase;              /* in S and S-bar */

	/* from the start, with the scrambler's tap */
	void init(s16 tap)
	{
		scr.r = 0;
		scr.tap = tap;
		prev = 0;
		phase = 0;
	}
	/* the next point of op (cnt: the symbols left in the step, from
	   count down to 1) */
	s16 next(trn_op op, u16 word, s16 cnt)
	{
		s16 s;
		switch (op) {
		case T_S:
			phase ^= 1;
			return prev = phase ? 0 : 2;
		case T_SB:
			phase ^= 1;
			return prev = phase ? 3 : 1;
		case T_TWO:
			s = scr.scramble(3, 2);
			return prev = (s & 1) ? 3 : 0;
		case T_FOUR:
			return prev = scr.scramble(3, 2);
		case T_ONES:
			s = scr.scramble(3, 2);
			return prev = vfc_diff[s << 2 | prev];
		case T_ZEROS:
			s = scr.scramble(0, 2);
			return prev = vfc_diff[s << 2 | prev];
		case T_WORD:
		case T_MASK:
		case T_FINAL:
			s = static_cast<s16>(
				(word >> ((-2 * cnt) & 15)) & 3);
			s = scr.scramble(s, 2);
			return prev = vfc_diff[s << 2 | prev];
		default:
			return -1;
		}
	}
};

/* the training the receiver looks for (from S-bar's start) */
#define VFC_KNOWN       (16 + 256 + 1024)
/* matched over this many symbols of the 2-phase part */
#define VFC_MATCH_WIN   24
/* the positions searched */
#define VFC_MATCH_FIND  96

enum vfc_state {
	V_INIT,
	V_SILENCE,      /* the answerer: before ANS */
	V_ANS,          /* the answerer: ANS until the 800 Hz tone */
	V_ANS_END,      /* and 300 ms after the tone began */
	V_WAIT_ANS,     /* the caller: for ANS */
	V_ANS_HEARD,    /* the caller: ANS for 1.4 s */
	V_TONE,         /* the caller: 800 Hz */
	V_P2,           /* phase 2 */
	V_P3_TX,        /* phase 3: we train the other */
	V_P3_RX,        /* phase 3: the other trains us */
	V_P4,           /* phase 4 */
	V_DATA,
};

/* the receiver from phase 3 on */
enum vfc_rxp {
	R_OFF,
	R_WAIT,         /* for S */
	R_S,            /* in S: looking for the 2-phase training */
	R_TRN,          /* on the known training */
	R_DD,           /* trained: decisions, the bits decoded */
	R_DATA,
};

struct VfcPump : PumpBase<VfcPump, vfc_state>, QamRxCore<VfcPump> {
	static constexpr const char *name = "V.FC pump";

	/* (the receiver's buffers freed) */
	~VfcPump()
	{
		rx.release();
		rx2.release();
	}

	s16 max_n;              /* +MS's limit, in 2400 bit/s */
	s16 baud;               /* vfc_bauds index */
	Osc tone;
	Goertzel g_ans, g_800;
	PowerMeter pw;
	s16 tone_blocks;
	s32 tone_at;            /* the answerer: count when the 800 Hz tone
				   was heard */
	s32 ans_wait;           /* the caller: from ANS heard to the tone */

	/* phase 2: transmitter */
	QamTx tx2;
	const p2_step *p2;
	s16 p2_i;
	s32 p2_n;               /* symbols (or frames) of the step sent */
	s16 p2_frm[20], p2_fpos;
	VfcScr p2_scr;
	s16 p2_prev, p2_rev;
	/* phase 2: receiver */
	QamRx rx2;
	cplx rx2_last;
	VfcScr rx2_scr;
	u32 rx2_sr;             /* the last 16 bits descrambled */
	s16 rx2_fbits;          /* of a message after its sync, -1 none */
	u32 rx2_fword;
	s16 rx2_revrun, rx2_gap, rx2_revs;
	bool heard, got_t0, got_t7;
	s16 r_msg[8];           /* the other's messages, -1 none */
	s16 own_t0;

	/* phases 3, 4: transmitter */
	QamTx tx;
	const trn_step *tp;
	s16 tp_i;
	s32 tp_n;
	VfcTrn gen;
	bool tx_on;
	u16 own_mask, final_word;
	s32 trained_at;         /* total when our receiver was trained */
	s32 table_at;           /* and when the other's table came */
	/* receiver */
	vfc_rxp rxp;
	bool p4;                /* the receiver in phase 4 */
	s16 alt_run, last_sgn;
	cplx last_z;
	cplx hist[VFC_MATCH_WIN + VFC_MATCH_FIND];
	s16 known[VFC_KNOWN];
	s32 rx_syms, trn_pos;
	float rx_mse;
	s16 rx_prev;
	VfcScr rx_scr;
	s16 zero_run;
	s32 ones_seen;          /* phase 3: the other's ones received */
	u32 rx_sr;              /* the last 16 bits */
	s16 rx_bits;
	s16 tab_n;              /* table entries, -1 none yet */
	s16 tab_bits;           /* bits of the entry after its sync */
	s16 tab_byte;
	u16 cand;               /* a rate word, to be seen twice */
	bool got_table, got_mask, got_final;
	u16 r_mask;
	s16 r_rate, t_rate;     /* data rate indices (0: 14400) */
	s16 word_bits;          /* after the mask: bits to the word's end */
	s32 data_at;            /* rx_syms when the data begins */
	bool zeros;             /* phase 3: the other's zeros */
	bool trained;
	s16 states;
	/* data */
	V34Params ptx, prx;
	V34DataTx dtx;
	V34DataRx drx;
	u32 dscr_tx, dscr_rx;
	bool tx_data, rx_data;
	bool connected;
	float data_power;

	/* the pump's name in the log */
	const char *tag() const { return "vfc"; }
	/* in the data mode */
	bool in_data() const { return state == V_DATA; }

	s16 handshake();
	s16 sample(float x, float *y);
	cplx p2_symbol();
	void p2_next_step();
	bool until(vfc_until u) const;
	void rx2_symbol();
	void rx2_message(u32 w);
	void p3_start();
	cplx trn_symbol();
	void trn_next_step();
	void rx_start(bool phase4);
	void rx_symbol();
	void rx_bit(s16 b);
	void rx_known();
	void data_start();
	/* (the carrier is the data mode's) */
	void carrier_came() {}
	/* (as carrier_came) */
	void carrier_went() {}
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	/* cnt line samples in, cnt out */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}
};

/*
 *    phase 2
 */

/* whether what step u waits for has come */
bool VfcPump::until(vfc_until u) const
{
	switch (u) {
	case U_NONE:
		return true;
	case U_REV2:
		return rx2_revs >= 2;
	case U_HEARD:
		return heard;
	case U_T0:
		return got_t0;
	case U_T7:
		return got_t7;
	case U_TRAINED:
		return trained;
	case U_TABLE:
		/* the other's table, or a while after we were trained (two
		   D-Modems: the caller waits longer) */
		if (got_table)
			return total - table_at >= ms2samples(20, srate);
		return trained && total - trained_at >
		       ms2samples(caller ? 1000 : 500, srate);
	case U_MASK:
		return got_mask;
	}
	return true;
}

/* to phase 2's next step */
void VfcPump::p2_next_step()
{
	p2_i++;
	p2_n = 0;
	p2_fpos = 0;
	const p2_step *s = &p2[p2_i];
	if (s->carrier)
		tx2.carrier.set_freq(s->carrier, srate);
	if (s->op == P2_ONES0)
		p2_scr.r = 0;
	DSP_DBG("vfc: phase 2 step %d (%d) at %" PRId64 " ms\n", p2_i,
		static_cast<int>(s->op), s64{total} * 1000 / srate);
}

/* phase 2's next symbol */
cplx VfcPump::p2_symbol()
{
	const p2_step *s = &p2[p2_i];
	s16 idx = -1, d;

	/* a step done: the next (messages at a frame's end) */
	for (;;) {
		s = &p2[p2_i];
		if (s->op == P2_END)
			return cplx_make(0, 0);
		if (s->op == P2_MSG && p2_fpos)
			break;
		bool counted = s->count && p2_n >= s->count;
		if (s->until != U_NONE ? (p2_n >= s->count && until(s->until))
				       : counted) {
			p2_next_step();
			continue;
		}
		break;
	}
	switch (s->op) {
	case P2_SIL:
		p2_n++;
		return cplx_make(0, 0);
	case P2_REV:
		p2_rev ^= 1;
		idx = p2_rev ? 3 : 0;
		p2_n++;
		break;
	case P2_PROBE:
		idx = vfc_probe[p2_n & 15];
		p2_n++;
		break;
	case P2_ONES0:
		idx = p2_scr.scramble(3, 2);
		p2_n++;
		break;
	case P2_ONES:
		d = p2_scr.scramble(3, 2);
		idx = vfc_diff[d << 2 | p2_prev];
		p2_n++;
		break;
	case P2_MSG:
		if (!p2_fpos)
			vfc_frame(s->type, s->value < 0 ? own_t0 : s->value,
				  p2_frm);
		d = p2_scr.scramble(p2_frm[p2_fpos], 2);
		idx = vfc_diff[d << 2 | p2_prev];
		if (++p2_fpos == 20) {
			p2_fpos = 0;
			p2_n++;
		}
		break;
	default:
		return cplx_make(0, 0);
	}
	p2_prev = idx;
	return vfc_qpsk[idx];
}

/* a message received (the 24 bits after its sync) */
void VfcPump::rx2_message(u32 w)
{
	s16 d[12];
	for (s16 k = 0; k < 12; k++)
		d[k] = static_cast<s16>((w >> (2 * k)) & 3);
	s16 t = static_cast<s16>(d[0] | d[1] << 2);
	s16 v = static_cast<s16>(d[2] | d[3] << 2 | d[6] << 4 | d[7] << 6 |
				 d[8] << 8 | d[9] << 10);
	s16 c = static_cast<s16>(d[10] | d[11] << 2);
	if (d[4] != 2 || d[5] != 3 ||
	    c != (~((v >> 4) + (v >> 8) + v + t) & 0xf))
		return;
	if (r_msg[t & 7] != v || !heard)
		DSP_DBG("vfc: message type %d value 0x%03x at %" PRId64
			" ms\n", t, v, s64{total} * 1000 / srate);
	heard = true;
	if (t < 8)
		r_msg[t] = v;
	if (t == 0)
		got_t0 = true;
	if (t == 7 && v == VFC_T7_DONE)
		got_t7 = true;
}

/* a symbol of phase 2 received: reversals counted, the messages
   decoded */
void VfcPump::rx2_symbol()
{
	cplx z = rx2.z, st = cplx_mulc(z, rx2_last);
	rx2_last = z;
	float a = atan2f(st.im, st.re);
	s16 k = static_cast<s16>(lrintf(a / (static_cast<float>(M_PI) / 2)))
		& 3;

	/* reversals: steps of 180 degrees, closely */
	if (fabsf(fabsf(a) - static_cast<float>(M_PI)) < 0.26f &&
	    rx2.power > tone_power(100)) {
		if (++rx2_revrun == 24 && rx2_gap >= 40) {
			rx2_revs++;
			DSP_DBG("vfc: reversals %d at %" PRId64 " ms\n",
				rx2_revs, s64{total} * 1000 / srate);
		}
		if (rx2_revrun >= 24)
			rx2_gap = 0;
	} else {
		rx2_revrun = 0;
		if (rx2_gap < 1000)
			rx2_gap++;
	}

	s16 d = rx2_scr.descramble(vfc_turn_dibit[k], 2);
	for (s16 i = 0; i < 2; i++) {
		u32 b = (d >> i) & 1;
		if (rx2_fbits >= 0) {
			rx2_fword |= b << rx2_fbits;
			if (++rx2_fbits == 24) {
				rx2_message(rx2_fword);
				rx2_fbits = -1;
				rx2_sr = 0;
			}
			continue;
		}
		rx2_sr = (rx2_sr >> 1) | b << 15;
		if (rx2_sr == VFC_SYNC) {
			rx2_fbits = 0;
			rx2_fword = 0;
		}
	}
}

/*
 *    phases 3 and 4
 */

/* the symbol rate both have (the highest) and the receiver and
   transmitter at it */
void VfcPump::p3_start()
{
	s16 m = own_t0 & (r_msg[0] < 0 ? 0x3f : r_msg[0]) & 0x3f;
	for (baud = VFC_NBAUDS - 1; baud > 0; baud--)
		if (m & (1 << baud))
			break;
	const vfc_baud *b = &vfc_bauds[baud];
	/* the data rates +MS allows that V.FC has at it, up to VFC_TOP_N */
	own_mask = VFC_MASK_FIXED;
	for (s16 r = 0; r < 7; r++)
		if (r + 6 <= max_n && r + 6 <= VFC_TOP_N &&
		    r + 6 <= vfc_max_n(b->v34))
			own_mask |= vfc_rate_bit[r];
	DSP_DBG("vfc: symbol rate %.1f, carrier %.1f (ours 0x%03x, theirs "
		"0x%03x)\n", b->baud, b->carrier, own_t0, r_msg[0]);
	tx.init(srate, b->carrier, b->baud, VFC_ALPHA, VFC_SPAN,
		sqrtf(VFC_POWER), VFC_TX_AMPLITUDE);
	gen.init(vfc_tap(caller));
	tx_on = false;
	rx_start(false);
}

/* the receiver from its start, for phase 3 or 4, waiting for S (in
   phase 4 too a new equalizer: one kept from phase 3 trains worse) */
void VfcPump::rx_start(bool phase4)
{
	const vfc_baud *b = &vfc_bauds[baud];
	{
		rx.release();
		rx.init(srate, b->carrier, b->baud, VFC_ALPHA, VFC_SPAN,
			VFC_EQ_LEN, VFC_POWER);
		carrier.init(150, 90);
	}
	rx.agc_hold = 0;
	rx.timing_hold = 0;
	p4 = phase4;
	rxp = R_WAIT;
	alt_run = 0;
	last_sgn = 0;
	rx_syms = 0;
	zero_run = 0;
	ones_seen = 0;
	zeros = false;
	trained = false;
	got_table = got_mask = got_final = false;
	tab_n = -1;
	tab_bits = -1;
	rx_sr = 0;
	word_bits = -1;
	data_at = 0x7fffffff;
	/* the other's training */
	VfcTrn m;
	m.init(vfc_tap(!caller));
	for (s16 i = 0; i < VFC_KNOWN; i++)
		known[i] = m.next(i < 16 ? T_SB : i < 16 + 256 ? T_TWO :
				  T_FOUR, 0, 0);
	rx_prev = known[VFC_KNOWN - 1];
	rx_scr = m.scr;
}

/* to the script's next step */
void VfcPump::trn_next_step()
{
	tp_i++;
	tp_n = 0;
	const trn_step *s = &tp[tp_i];
	if (s->op == T_TWO)
		gen.scr.r = 0;
	if (s->op == T_MASK) {
		DSP_DBG("vfc: our rate mask 0x%04x\n", own_mask);
	}
	if (s->op == T_FINAL) {
		u16 m = own_mask & r_mask & VFC_RATE_BITS;
		for (t_rate = 6; t_rate > 0; t_rate--)
			if (m & vfc_rate_bit[t_rate])
				break;
		final_word = vfc_final[t_rate];
		DSP_DBG("vfc: our rate %d bit/s (word 0x%04x)\n",
			(t_rate + 6) * 2400, final_word);
	}
	if (s->op == T_DATA) {
		if (ptx.vfc_init(vfc_bauds[baud].v34, t_rate + 6, states)) {
			DSP_DBG("vfc: no data mode at %d\n", t_rate);
		}
		dtx.init(&ptx);
		dtx.link = CoreBits{ modem };
		/* the quadrant goes on from the training's last point */
		dtx.z = gen.prev;
		dscr_tx = gen.scr.v34();
		tx_data = true;
		DSP_DBG("vfc: our data at %" PRId64 " ms\n",
			s64{total} * 1000 / srate);
	}
}

/* the next symbol of phase 3 or 4 */
cplx VfcPump::trn_symbol()
{
	const trn_step *s;
	if (tx_data) {
		return dtx.symbol(&dscr_tx, vfc_v34_tap(caller));
	}
	for (;;) {
		s = &tp[tp_i];
		if (s->op == T_END)
			return cplx_make(0, 0);
		if (s->op == T_DATA)
			return dtx.symbol(&dscr_tx, vfc_v34_tap(caller));
		bool word = s->op == T_WORD || s->op == T_MASK ||
			    s->op == T_FINAL;
		if (tp_n >= s->count && (!word || s->until == U_NONE ||
					 (tp_n % 8) == 0) &&
		    until(s->until)) {
			trn_next_step();
			continue;
		}
		break;
	}
	u16 w = s->param;
	if (s->op == T_MASK)
		w = own_mask;
	else if (s->op == T_FINAL)
		w = final_word;
	s16 cnt = static_cast<s16>(s->count - (tp_n % s->count));
	if (s->op == T_MASK)
		cnt = static_cast<s16>(8 - (tp_n % 8));
	tp_n++;
	if (s->op == T_SIL)
		return cplx_make(0, 0);
	return vfc_qpsk[gen.next(s->op, w, cnt)];
}

/* the data's receiver and the data from the symbol data_at on */
void VfcPump::data_start()
{
	if (prx.vfc_init(vfc_bauds[baud].v34, r_rate + 6, states))
		DSP_DBG("vfc: no data mode at %d (receiver)\n", r_rate);
	drx.init(&prx);
	drx.link = CoreBits{ modem };
	/* (only the first symbol's: the decoder finds the rest) */
	drx.z = rx_prev;
	dscr_rx = rx_scr.v34();
	rxp = R_DATA;
	rx_data = true;
	rx.agc_hold = 1;
	rx.kt_p = 0.004f * rx.sps;
	rx.kt_gi = 0.003f;
	data_power = rx.power;
	DSP_DBG("vfc: their data at %" PRId64 " ms, %d bit/s (dtheta %.6f, "
		"mse %.5f)\n", s64{total} * 1000 / srate, (r_rate + 6) * 2400,
		rx.dtheta, rx_mse);
}

/* a bit decoded after the training: the other's table, rate mask and
   final word found in them */
void VfcPump::rx_bit(s16 b)
{
	rx_sr = (rx_sr >> 1) | static_cast<u32>(b & 1) << 15;
	if (!p4)
		return;
	if (tab_bits >= 0) {
		/* the byte and word after the sync */
		if (++tab_bits == 24) {
			tab_n++;
			tab_bits = -1;
			DSP_DBG("vfc: table entry %d: 0x%02x 0x%04x\n", tab_n,
				tab_byte, rx_sr);
			if (tab_byte == 0 || tab_n >= 5) {
				if (!got_table)
					DSP_DBG("vfc: their table at %" PRId64
						" ms\n",
						s64{total} * 1000 / srate);
				got_table = true;
				table_at = total;
			}
		} else if (tab_bits == 8)
			tab_byte = static_cast<s16>(rx_sr >> 8);
		return;
	}
	if (rx_sr == 0xd9ab && !got_table) {
		tab_bits = 0;
		return;
	}
	if (!got_table)
		return;
	if (word_bits > 0) {
		if (--word_bits)
			return;
		word_bits = 16;
		if (!got_mask) {
			/* the same mask again a word later */
			if (rx_sr == cand) {
				r_mask = cand;
				got_mask = true;
				DSP_DBG("vfc: their rate mask 0x%04x at %"
					PRId64 " ms\n", r_mask,
					s64{total} * 1000 / srate);
			} else
				word_bits = -1;
			return;
		}
		if ((rx_sr & ~VFC_RATE_BITS & 0xffff) == VFC_FINAL_FIXED &&
		    !got_final) {
			for (r_rate = 6; r_rate > 0; r_rate--)
				if (rx_sr & vfc_rate_bit[r_rate])
					break;
			got_final = true;
			/* 4 words of it, this the first */
			data_at = rx_syms + 24;
			DSP_DBG("vfc: their rate word 0x%04x: %d bit/s\n",
				rx_sr, (r_rate + 6) * 2400);
		}
		return;
	}
	if ((rx_sr & ~VFC_RATE_BITS & 0xffff) == VFC_MASK_FIXED) {
		cand = static_cast<u16>(rx_sr);
		word_bits = 16;
	}
}

/* the received symbol's place in the known training: the best match of
   the last symbols with it (the 2-phase part), the phase turned to it */
void VfcPump::rx_known()
{
	float best = 0;
	s16 bp = -1;
	cplx bc = { 0, 0 };
	float e = 0;
	for (s16 i = 0; i < VFC_MATCH_WIN; i++)
		e += cplx_pow(hist[i]);
	for (s16 p = 16; p < 16 + VFC_MATCH_FIND; p++) {
		cplx c = { 0, 0 };
		/* hist[0] the newest: known[p + WIN - 1] */
		for (s16 i = 0; i < VFC_MATCH_WIN; i++)
			c = cplx_add(c, cplx_mulc(hist[i],
				vfc_qpsk[known[p + VFC_MATCH_WIN - 1 - i]]));
		float m = cplx_pow(c);
		if (m > best) {
			best = m;
			bp = p;
			bc = c;
		}
	}
	/* |c|^2 against what a perfect match gives: (W * 2)^2 at power 2,
	   e = W * 2 */
	if (bp < 0 || best < 0.85f * 0.85f * e * VFC_MATCH_WIN * VFC_POWER)
		return;
	rx.theta = remainderf(rx.theta + atan2f(bc.im, bc.re),
			      2 * static_cast<float>(M_PI));
	trn_pos = bp + VFC_MATCH_WIN;
	rxp = R_TRN;
	rx_mse = 0;
	DSP_DBG("vfc: %s training found (symbol %d, match %.2f) at %" PRId64
		" ms\n", p4 ? "phase 4" : "phase 3", trn_pos,
		sqrtf(best / (e * VFC_MATCH_WIN * VFC_POWER)),
		s64{total} * 1000 / srate);
}

/* a symbol received in phases 3 and 4 and the data */
void VfcPump::rx_symbol()
{
	cplx z = rx.z, st;
	float a;
	s16 sgn;

	if (rxp == R_OFF)
		return;
	std::copy_backward(hist, hist + std::size(hist) - 1,
			   hist + std::size(hist));
	hist[0] = z;

	switch (rxp) {
	case R_OFF:
		break;
	case R_WAIT:
	case R_S:
		if (rx.power < tone_power(150)) {
			alt_run = 0;
			if (rxp == R_S)
				rxp = R_WAIT;
			break;
		}
		/* S: steps of +-90 degrees, the sign alternating */
		st = cplx_mulc(z, last_z);
		a = atan2f(st.im, st.re);
		sgn = a > 0 ? 1 : -1;
		if (fabsf(a) > 0.9f && fabsf(a) < 2.2f && sgn != last_sgn)
			alt_run++;
		else if (rxp == R_WAIT)
			alt_run = alt_run > 4 ? alt_run - 4 : 0;
		if (rxp == R_WAIT && alt_run == 4)
			rx.t_adj = 0;
		last_sgn = sgn;
		if (rxp == R_WAIT && alt_run >= 24) {
			DSP_DBG("vfc: %s S at %" PRId64 " ms\n",
				p4 ? "phase 4" : "phase 3",
				s64{total} * 1000 / srate);
			rxp = R_S;
			rx_syms = 0;
		}
		if (rxp == R_S) {
			/* a step of -90 degrees ends on point 0 */
			cplx d = vfc_qpsk[sgn < 0 ? 0 : 2];
			if (!rx_syms)
				rx.align_phase(d);
			rx.adapt(d, 0, 0.05f, 0);
			if (++rx_syms >= 8)
				rx_known();
		}
		break;
	case R_TRN: {
		cplx d = vfc_qpsk[known[trn_pos]];
		rx_mse += 0.02f * (cplx_pow(cplx_sub(z, d)) / VFC_POWER -
				   rx_mse);
		s32 n = trn_pos;
		rx.adapt(d, n < 300 ? 0.08f : n < 800 ? 0.03f : 0.01f, 0.05f,
			 n < 400 ? 0.0005f : 0.00005f);
		if (++trn_pos >= VFC_KNOWN) {
			DSP_DBG("vfc: trained, mse %.4f (%.1f dB) at %" PRId64
				" ms\n", rx_mse, -10 * log10f(rx_mse + 1e-6f),
				s64{total} * 1000 / srate);
			rxp = R_DD;
			rx_syms = 0;
			rx.kt_p = 0.01f * rx.sps;
			rx.kt_gi = 0.01f;
			if (p4) {
				trained = true;
				trained_at = total;
			}
		}
		break;
	}
	case R_DD: {
		/* decisions; the bits differential and scrambled */
		s16 k = nearest_point(vfc_qpsk, 4, z), t;
		cplx d = vfc_qpsk[k];
		rx_mse += 0.02f * (cplx_pow(cplx_sub(z, d)) / VFC_POWER -
				   rx_mse);
		rx.adapt(d, 0.005f, 0.05f, 0.00002f);
		for (t = 0; t < 4; t++)
			if (vfc_diff[t << 2 | rx_prev] == k)
				break;
		rx_prev = k;
		s16 b = rx_scr.descramble(t & 3, 2);
		rx_syms++;
		if (!p4) {
			if (b == 0) {
				if (++zero_run == 32) {
					zeros = true;
					DSP_DBG("vfc: their zeros at %" PRId64
						" ms (ones %" PRId32 ")\n",
						s64{total} * 1000 / srate,
						ones_seen);
				}
			} else {
				zero_run = 0;
				if (b == 3)
					ones_seen++;
			}
		}
		rx_bit(b & 1);
		rx_bit(b >> 1);
		if (rx_syms + 0 >= data_at)
			data_start();
		break;
	}
	case R_DATA: {
		cplx d = drx.vfc_symbol(z, &dscr_rx, vfc_v34_tap(!caller));
		rx_mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / VFC_POWER -
				   rx_mse);
		rx.adapt(d, 0.002f, 0.02f, 0.00002f);
		rx_syms++;
		if (rx_syms % 1000 == 0)
			DSP_DBG("vfc: data %" PRId32 " mse %.5f\n", rx_syms,
				rx_mse);
		break;
	}
	}
	last_z = z;
}

/*
 *    the handshake
 */

/* the start-up's state machine, once a sample: DPSTAT_OK, or the
   connect */
s16 VfcPump::handshake()
{
	switch (state) {
	case V_INIT:
		break;
	case V_SILENCE:
		if (count >= ms2samples(ANS_SILENCE_MS, srate))
			set_state(V_ANS);
		break;
	case V_ANS:
		if (tone_at >= 0) {
			set_state(V_ANS_END);
		}
		break;
	case V_ANS_END:
		if (count >= ms2samples(250, srate)) {
			p2 = vfc_p2_answerer;
			p2_i = -1;
			p2_next_step();
			set_state(V_P2);
		}
		break;
	case V_WAIT_ANS:
		break;
	case V_ANS_HEARD:
		/* the tone 1.43 s after ANS began, as the Courier's: the
		   answerer takes the round trip delay from when it comes */
		if (count >= ans_wait) {
			tone.set_freq(800, srate);
			set_state(V_TONE);
		}
		break;
	case V_TONE:
		if (count >= ms2samples(300, srate)) {
			p2 = vfc_p2_caller;
			p2_i = -1;
			p2_next_step();
			set_state(V_P2);
		}
		break;
	case V_P2:
		if (p2[p2_i].op == P2_END) {
			set_state(caller ? V_P3_RX : V_P3_TX);
			p3_start();
			if (!caller) {
				tp = vfc_p3_answerer;
				tp_i = -1;
				trn_next_step();
				tx_on = true;
			}
		}
		break;
	case V_P3_TX:
		/* the answerer: its training sent, the caller's received
		   until its zeros (then 50 ms) */
		if (tp[tp_i].op == T_END)
			tx_on = false;
		if (!caller && tp[tp_i].op == T_END && zeros &&
		    zero_run >= 32 + 170) {
			tp = vfc_p4;
			tp_i = -1;
			trn_next_step();
			tx_on = true;
			rx_start(true);
			set_state(V_P4);
		}
		/* the caller: after its zeros, phase 4 (its receiver
		   already waiting for it) */
		if (caller && tp[tp_i].op == T_END) {
			tp = vfc_p4;
			tp_i = -1;
			trn_next_step();
			tx_on = true;
			set_state(V_P4);
		}
		break;
	case V_P3_RX:
		/* the caller: the answerer's training received, until its
		   zeros; then its own, 50 ms into them */
		if (zeros && zero_run >= 32 + 140) {
			tp = vfc_p3_caller;
			tp_i = -1;
			trn_next_step();
			tx_on = true;
			rx_start(true);
			set_state(V_P3_TX);
		}
		break;
	case V_P4:
		if (tx_data && rx_data) {
			set_state(V_DATA);
			s32 bt = (t_rate + 6) * 2400, br = (r_rate + 6) * 2400;
			stat_line(vfc_bauds[baud].baud, vfc_bauds[baud].carrier,
				  vfc_bauds[baud].baud,
				  vfc_bauds[baud].carrier);
			stat_coding(false, states, false, false, false);
			stat_coding(true, states, false, false, false);
			connected = true;
			return connect_at(bt, br);
		}
		break;
	case V_DATA:
		if (count % ms2samples(100, srate) == 0) {
			stat_snr(-10 * log10f(rx_mse + 1e-6f));
			stat_freq_offset(rx.dtheta, vfc_bauds[baud].baud);
			stat_timing_offset(rx.t_adj, rx.sps);
		}
		break;
	}
	return DPSTAT_OK;
}

/* one line sample x in, *y out */
s16 VfcPump::sample(float x, float *y)
{
	s16 r = handshake();

	*y = 0;
	switch (state) {
	case V_ANS:
	case V_ANS_END:
		*y = DSP_TX_AMPLITUDE * tone.next();
		if (state == V_ANS && g_800.run(x)) {
			float p = pw.power;
			if (g_800.power > 0.5f * p && p > tone_power(100)) {
				if (++tone_blocks == 4) {
					tone_at = count;
					DSP_DBG("vfc: 800 Hz heard at %" PRId64
						" ms\n",
						s64{total} * 1000 / srate);
				}
			} else
				tone_blocks = 0;
		}
		pw.run(x);
		break;
	case V_WAIT_ANS:
		pw.run(x);
		if (g_ans.run(x)) {
			float p = pw.power;
			if (g_ans.power > 0.5f * p && p > tone_power(100)) {
				if (++tone_blocks == 10) {
					DSP_DBG("vfc: ANS heard at %" PRId64
						" ms\n",
						s64{total} * 1000 / srate);
					set_state(V_ANS_HEARD);
				}
			} else
				tone_blocks = 0;
		}
		break;
	case V_TONE:
		*y = 0.7f * DSP_TX_AMPLITUDE * tone.next();
		break;
	case V_P2:
		*y = tx2.sample([this] { return p2_symbol(); });
		if (rx2.sample(x))
			rx2_symbol();
		break;
	case V_P3_TX:
	case V_P3_RX:
	case V_P4:
	case V_DATA:
		if (tx_on)
			*y = tx.sample([this] { return trn_symbol(); });
		qam_rx(x);
		break;
	default:
		break;
	}
	return r;
}

/* V.FC: the answerer sends ANS, the caller listens for it; the symbol
   rates and data rates up to +MS's limit; timeouts from S7 and S10.
   0, or -1 when out of memory. */
s16 VfcPump::start(struct modem *m, enum DP_ID id, s16 caller_,
		   s32 srate_, s32 max_frag)
{
	(void)id;
	(void)max_frag;
	pump_setup(m, srate_, caller_);

	s32 max_bps = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));
	max_n = max_bps > 0 ? static_cast<s16>(max_bps / 2400) : 12;
	if (max_n > 12)
		max_n = 12;
	if (max_n < 6)
		max_n = 6;
	own_t0 = VFC_T0_OTHER | 0x3f;
	states = 32;
	if (const char *e = pump_test_env("VFC_STATES", caller_))
		states = static_cast<s16>(strtol(e, nullptr, 10)) == 8 ? 8 :
									32;
	if (const char *e = pump_test_env("VFC_BAUDS", caller_))
		own_t0 = static_cast<s16>(VFC_T0_OTHER |
				(strtol(e, nullptr, 16) & 0x3f));

	tx2.init(srate_, 1025, VFC_P2_BAUD, VFC_P2_ALPHA, VFC_P2_SPAN,
		 sqrtf(VFC_POWER), VFC_TX_AMPLITUDE);
	if (rx2.init(srate_, caller_ ? 2400 : 1025, VFC_P2_BAUD,
		     VFC_P2_ALPHA, VFC_P2_SPAN, 4, VFC_POWER))
		return -1;
	rx2_scr.r = 0;
	rx2_scr.tap = vfc_tap(!caller_);
	p2_scr.r = 0;
	p2_scr.tap = vfc_tap(caller_);
	rx2_fbits = -1;
	rx2_gap = 1000;
	for (auto &v : r_msg)
		v = -1;
	tone_at = -1;
	rxp = R_OFF;
	pw.init(0.02f, srate_);
	g_ans.init(2100, static_cast<s16>(srate_ / 100), srate_);
	g_800.init(800, static_cast<s16>(srate_ / 100), srate_);
	tone.set_freq(2100, srate_);
	/* (ANS heard 100 ms after it began; or by the call progress pump,
	   about 340 ms after) */
	ans_wait = ms2samples(1330, srate_);
	if (caller_ && rt && rt->answer_tone == ANSTONE_2100) {
		ans_wait = ms2samples(1090, srate_);
		state = V_ANS_HEARD;
	} else if (caller_)
		state = V_WAIT_ANS;
	else if (rt && rt->ans_sent) {
		rt->ans_sent = 0;
		state = V_ANS;
	} else
		state = V_SILENCE;
	DSP_DBG("vfc: create %s, up to %d bit/s, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", max_n * 2400, srate_);
	return 0;
}

/* registers the pump as DP_VFC; 0 */
s16 dp_vfc_init(void)
{
	modem_dp_register(DP_VFC, &PumpOps<VfcPump>::ops);
	return 0;
}

/* deregisters it */
void dp_vfc_exit(void)
{
	modem_dp_deregister(DP_VFC, &PumpOps<VfcPump>::ops);
}

} // namespace dsp
