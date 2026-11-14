/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    x2_rx.h - x2's client (the analogue modem): the PCM receiver
 *
 *    The server's training (X2Train's codes, as their voltages through
 *    the network's codec and our line), then its frames (X2Map's).  The
 *    preamble repeats every 22 codes and its last 11 have their signs
 *    turned: where they are gives the codes' grid.  From there the
 *    training is known code for code up to the sync frames, so the
 *    equaliser (T/2, as K56Rx's) is solved on TRN and follows it, and
 *    the J words are decoded from TRN's signs.  From the sync segment
 *    on the frames are decided to the levels of their rate (A, then the
 *    data's) and decoded back to their bits (X2Map's inverse and GPC's
 *    descrambler): ones, the server's record, the start's ones in the
 *    data levels, then the data.  See docs/dsp/x2-details.md.
 *
 */

#ifndef X2_RX_H
#define X2_RX_H


#include "dm_types.h"
#include "pcm_eq.h"
#include "v90a.h"
#include "x2.h"

namespace dsp {

#define X2RX_HIST       2048    /* samples kept at 16 000 Hz */
#define X2RX_HIST8      1024    /* the preamble's: every other one */
#define X2RX_TAPS       128     /* T/2: 64 codes */
#define X2RX_EQ_D       (X2RX_TAPS / 2)
#define X2RX_LS         1020    /* codes the equaliser is solved on */
#define X2RX_Q          2048    /* data bits held for the caller */
/* where the segments start, in codes from the preamble's first */
/* a rate change's frames of ones at the next rate before its data */
#define X2RX_NEXT_ONES  681
#define X2RX_K_TRN      363
#define X2RX_K_JS       (X2RX_K_TRN + 20004)
#define X2RX_K_ONES     (X2RX_K_JS + 1200)
#define X2RX_K_ALIGN    (X2RX_K_ONES + 6000)
#define X2RX_K_SYNC     (X2RX_K_ALIGN + 1152)

/* a rate's levels as our receiver has them across the digital pad of
   mode m (x2_rates' index): each position's magnitudes, the largest
   first, in the equaliser's units (TRN's octet at its own value) */
struct X2Levels {
	const X2Rate *r;
	float mag[X2_FRAME][X2_MAXLEVELS];

	void init(const X2Rate *rate, s16 m = 0);
	/* the nearest digit of position i to y's magnitude; its squared
	   error in e */
	s16 decide(s16 i, float y, float *e) const;
};

struct X2Rx : PcmEqualizer<X2Rx, X2RX_TAPS> {
	enum Stage {
		WAIT_PRE,       /* until the preamble and its turn */
		TRAIN,          /* TRN's start: the equaliser's solution */
		RUN,            /* codes decided */
	};

	s16 alaw;
	s16 lin[128];           /* code magnitude -> linear value */
	enum Stage stage;

	V90Interp ip;
	float x[X2RX_HIST];
	s32 n;
	float x8[X2RX_HIST8];
	s32 n8;

	/* the preamble (22 codes over and over): the lag 22 correlation
	   over 44 samples, and over the last 11 (where the turn is: -1) */
	double e44, a44, e11, a11;
	s16 p_run, p_on, p_lost, p_dip;
	double dip;
	s32 dip_at;

	/* code k (0: the preamble's first) at sample b + 2 k */
	s32 b, k;
	s16 refined;
	X2Train ref;            /* the server's training as it sends it */
	float trn0[96];
	float level;            /* TRN's: its octet's value */
	double err_sum;
	s32 err_n;
	float mse;              /* of late, to level's power */
	s16 ls2_n;

	/* the J words: TRN's signs descrambled from its start */
	u32 dscr;
	u16 jw;
	s16 jw_good, jw_bad;    /* 09B0s decoded, other words */
	s16 jw_end;             /* 099F after them */

	/* the frames from the sync segment on: their rate's levels (A,
	   then B, then the data's), the codes' decisions; the scrambled
	   bits' descrambler and the last sign of a data bit */
	X2Levels lv_a, lv_b, lv_d;
	const X2Levels *cur;
	float fy[X2_FRAME];
	s16 fpos;
	s32 frames;             /* since the sync segment's first */
	u32 sh;
	s16 last;
	/* the sync frames' ones in a row (the frames decoded right) */
	s32 ones_run;
	/* the noise: the decisions' error in levels A and the start's
	   ones (its power), and the signal's */
	double nz_sum, sg_sum;
	s32 nz_n;
	/* the server's record: its bits (the last 85), its words */
	unsigned char rbits[96];
	s32 rbit_n;
	u16 rec_w[3];
	s16 rec_n;              /* good ones decoded */
	/* the start segment: after our E (e_sent), its first frame in the
	   data levels; frames since (the data from X2_START_FR on) */
	s16 e_sent;
	s32 dfr;                /* -1 before */
	s32 err_frames;         /* data frames whose decisions were far */
	s16 mode;               /* the digital pad (x2_rates' index) */
	/* the sync frames' magnitudes by level (A's 9): their sums and
	   counts, for the pad */
	double pad_sum[9];
	s32 pad_n[9];
	double pad_res[X2_MODES];       /* each pad's fit */

	/* a rate change in the data: the server's marker (8 frames of
	   +++--- at the largest level, then ---+++), its records in the
	   start-up's data levels (lv_b, the scrambler from zero), a frame
	   of ones, then ones in the next rate's levels (which: the rate
	   whose levels decode them to ones; want, the one asked for,
	   first), its data after X2_ONES_AFTER of them */
	enum RnStage {
		RN_NONE,        /* data */
		RN_MARK,        /* the marker's frames */
		RN_REC,         /* its records */
		RN_RATE,        /* the next rate's first frame of ones */
		RN_ONES,        /* its ones */
	} rn;
	s16 rn_mark;            /* marker frames in a row */
	s32 rn_fr;              /* frames since the stage began */
	s16 rn_want;            /* the rate asked for (1 to 15), 0 none */
	s16 rn_down;            /* the rate taken (1 to 15), 0 the start-up's */
	X2Levels lv_n;          /* the next rate's, while RN_ONES */

	/* data mode: the bits for the caller; the signal's power and
	   the decisions' error of late */
	unsigned char q[X2RX_Q];
	s16 qh, qn;
	float dpow, de_mse;

	/* events (cleared by the caller) */
	s16 ev_pre;             /* the preamble's turn */
	s16 ev_trained;
	s16 ev_js;              /* the J words: 1 good, -1 not */
	s16 ev_sync;            /* the sync frames decode (ones) */
	s16 ev_rec;             /* the server's record (rec_w) */
	s16 ev_start;           /* its start segment (the data levels) */
	s16 ev_data;            /* the data frames have begun */
	s16 ev_marker;          /* the server's marker in the data */
	s16 ev_rn_rec;          /* a record after it */
	s16 ev_rn_rate;         /* the next rate's ones (rn_down) */
	s16 ev_rn_data;         /* its data */

	/* the line at rate (9600 Hz, or 8000), the law; the data levels
	   (the start-up's) */
	void init(s16 alaw_, s16 rate = 9600, const X2Rate *data = nullptr);
	/* one sample of the line */
	void line(float v);
	/* data mode: up to n of its bits into b, oldest first (how many) */
	s16 take(unsigned char *b, s16 n);
	/* the signal to noise ratio the decisions show (dB) */
	double snr_db() const;
	/* the highest rate (1 to 15) whose levels are far enough apart for
	   the noise the decisions show (the data's, or before it the sync
	   frames'): half the least distance
	   between two of a position's values, or a value's from zero, at
	   least 5.5 times the noise's standard deviation */
	s16 best_rate() const;
	/* the digital pad the sync frames' levels show (the shape of
	   levels A received: across none, 3 dB or 6 dB), mode set to it
	   and the levels from there on its; -1 without enough frames */
	s16 measure_pad();

private:
	friend struct PcmEqualizer<X2Rx, X2RX_TAPS>;
	float at(s32 i) const { return x[i & (X2RX_HIST - 1)]; }
	float at8(s32 i) const { return x8[i & (X2RX_HIST8 - 1)]; }
	float xin(s32 kk, s16 i, s16 d = 0) const
	{
		return at(b + 2 * kk + X2RX_EQ_D + d - i);
	}
	void sample(float v);
	void pre_detect(float v);
	void refine();
	void timing_locked(double loop_ppm) const;
	float value(unsigned char code) const;
	void known(float y, float a);
	void js_sign(float y);
	void code(float y);
	s16 unmap(const X2Levels &L, const float *y, u32 *s, s16 *lst,
		  unsigned char *bits, float *err) const;
	void frame();
	void rec_bit(unsigned char bit);
	void data_frame();
	bool marker_frame(bool turned) const;
};

} // namespace dsp

#endif
