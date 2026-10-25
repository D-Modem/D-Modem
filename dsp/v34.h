/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v34.h - ITU V.34 data pump: shared definitions
 *
 */

#ifndef __V34_H__
#define __V34_H__

#include <cstring>
#include <memory>
#include <vector>

#include "dm_types.h"
#include "bits.h"
#include "qam.h"

namespace dsp {

/* symbol rates, in the order of the INFO and MP fields */
enum {
	V34_BAUD_2400,
	V34_BAUD_2743,
	V34_BAUD_2800,
	V34_BAUD_3000,
	V34_BAUD_3200,
	V34_BAUD_3429,
	V34_NBAUDS
};

struct v34_baud {
	s16 nominal;            /* rounded symbol rate */
	s16 a, c;               /* symbol rate = 2400 * a / c */
	s16 d[2], e[2];         /* carrier = symbol rate * d / e (low, high) */
	s16 J, P;               /* data frame: J mapping frames, P per frame */
};
extern const struct v34_baud v34_bauds[V34_NBAUDS];

/* the symbol rate of V34_BAUD_* b, in symbols/s */
static inline float v34_symbol_rate(s16 b)
{
	return 2400.0f * v34_bauds[b].a / v34_bauds[b].c;
}

/* the carrier of symbol rate b, low (0) or high (1), in Hz */
static inline float v34_carrier(s16 b, s16 high)
{
	return v34_symbol_rate(b) * v34_bauds[b].d[high] / v34_bauds[b].e[high];
}

struct v34_rate {
	s16 b, m_min, m_exp;
};
extern const struct v34_rate v34_rates[V34_NBAUDS][14];
extern const signed char v34_quarter[416][2];
extern const cplx v34_trn4[4];
cplx v34_pp(s32 i);

/* MP style frames coming in (bits.h): V.90's Ja and CP, V.92's Ja,
   CPt, CPu, CPus and SUVu.  After a sync of 17 ones and a zero their
   bits go into b, their length from their header, their CRC checked at
   the end (v90.cpp) */
#define SYNC_FRAME_MAX  3000
struct SyncFrames {
	enum Kind {
		DIL90,          /* V.90's Ja: its length at bit 50 */
		DIL92,          /* V.92's Ja */
		CP90,           /* V.90's CP and CPt: at bit 129 */
		CP92,           /* V.92's CPt and CPu, and CPus and SUVu of 51
				   bits */
	};
	/* add() and bit() */
	enum { NONE = 0, GOOD = 1, BAD_CRC = -1, BAD_LENGTH = -2 };

	Kind kind;
	s16 max;                /* the longest frame taken */
	u32 sr;                 /* bit()'s last bits, the newest in bit 0 */
	unsigned char b[SYNC_FRAME_MAX];
	s16 n;                  /* bits in b: 0, none (looking for a sync) */
	s16 len;                /* the frame's length, 0 until known */

	/* cleared, for frames of kind k of max_len bits at most */
	void init(Kind k, s16 max_len = SYNC_FRAME_MAX);
	/* a frame begins: its sync in b */
	void start();
	/* in a frame */
	bool busy() const { return n != 0; }
	/* the frame in progress dropped */
	void drop() { n = 0; }
	/* a bit of the frame start() began: GOOD when it ends with a good
	   CRC (len bits in b), BAD_CRC with a bad one, BAD_LENGTH when its
	   header gives no length it may have (the frame dropped), NONE
	   until then */
	s16 add(s16 x);
	/* a bit, the sync looked for in them (sr) while not busy(); as
	   add() */
	s16 bit(s16 x);
};

/* MP sequence contents (10.1.3.12/V.34) */
struct V34Mp {
	s16 type;
	s16 rate_ca, rate_ac;   /* maximum data rates, N * 2400 */
	s16 aux, trellis, nonlinear, expanded, ack;
	s16 rate_mask;          /* bit 0: 2400 .. bit 13: 33600 */
	s16 asym;
	s16 h[6];               /* precoding coefficients (type 1) */

	s16 build(unsigned char *b) const;
	s16 parse(const unsigned char *b, s16 len);
};

/*
 *    data mode (v34_data.cpp)
 */
#define V34_MAX_RINGS   18
#define V34_G2_LEN      (2 * V34_MAX_RINGS - 1)
#define V34_G4_LEN      (4 * V34_MAX_RINGS - 3)
#define V34_Z8_LEN      (8 * V34_MAX_RINGS - 6)
#define V34_MAX_B       80

struct V34Precoder;

struct V34Params {
	s32 rate;               /* bit/s */
	s16 b;                  /* bits in a high mapping frame */
	s16 K, q, M;            /* shell mapping bits, uncoded bits, rings */
	s16 J, P, r;            /* data frames per superframe, mapping frames
				   per data frame, high frames among them */
	float g;                /* half the point spacing, in units of the
				   training points */
	s16 states;             /* of the trellis code: 16, 32 or 64 */
	s16 h[6];               /* precoding coefficients, 14 fractional
				   bits: h(1) re, im, h(2) re, ... */
	s16 precode, w;         /* h is not zero; c(n) in multiples of
				   2^w */
	float theta;            /* non-linear encoder (9.7/V.34): 0 or
				   0.3125, transmitter only */
	float nl_gain;          /* after it: the power without it */
	double pw_;             /* mean power of the points (integers) */
	u64 g2[V34_G2_LEN], g4[V34_G4_LEN], z8[V34_Z8_LEN];

	s16 shell_tables();
	u64 g2p(s16 i) const;
	u64 g4p(s16 i) const;
	double frame_power(s16 k, double theta = 0, double e0 = 1) const;
	void set_nonlinear(double theta_);
	/* n: the rate in units of 2400 bit/s; returns -1 when not allowed */
	s16 init(s16 baud, s16 n, s16 expanded);
	/* precoding with the coefficients of an MP sequence */
	void set_precoder(const s16 h_[6]);
	void precoder_push(V34Precoder *pc, s16 yr, s16 yi) const;
	void shell_map(u32 r0, s16 m[8]) const;
	s16 pair_index(s16 x, s16 y) const;
	u32 shell_demap(const s16 m[8]) const;
	s16 decide(cplx z, s16 *px, s16 *py) const;
	void subset_points(float u, float v, float dist[8],
			   s32 pt[8]) const;
};

/* the precoder's tap delay line (9.6.2/V.34) */
struct V34Precoder {
	s32 x[3][2];            /* x(n - 1) .. x(n - 3), units of 2^-7 */
	s32 p[2], c[2];         /* p(n) (units of 2^-7) and c(n) */

	s16 c_parity() const;
};

struct V34DataTx {
	V34Params p;
	CoreBits link;          /* the data */
	s16 sbc;                /* high/low frame switching */
	s16 z, state, v0;       /* differential, trellis and sync state */
	s16 half, pat;          /* 4D symbols in the half data frame, sync
				   pattern position */
	V34Precoder pc;
	cplx sym[8];
	s16 pos;

	void init(const V34Params *p_);
	void encode_frame(u32 *scr, s16 tap);
	cplx symbol(u32 *scr, s16 tap);
};


#define V34_STATES      64      /* at most */
#define V34_TB          32      /* Viterbi decoding delay, 4D symbols */

/* the trellis code (9.6.3/V.34): the subset label of a 2D point (odd
   coordinates), the encoder input Y1 | Y2 << 1 | Y3 << 2 | Y4 << 3 of a
   4D symbol from its two labels, and the next encoder state (16, 32 or 64
   states; its low bit is Y0) */
s16 v34_subset_label(s16 x, s16 y);
s16 v34_conv_in(s16 s0, s16 s1);
s16 v34_trellis_next(s16 states, s16 st, s16 y);

/* the Viterbi decoder of the 4D trellis code (9.6.3/V.34; V.92's PCM
   upstream codes the same way), TB 4D symbols behind: in, each 2D half's
   best point in each of its 8 subsets and that point's metric; out, the
   points of the 4D symbol TB - 1 back on the best path */
template <s16 TB>
struct Viterbi4d {
	float metric[V34_STATES];
	unsigned char prev[TB][V34_STATES];
	s32 pa[TB][V34_STATES], pb[TB][V34_STATES];
	s16 t;
	s32 n;                  /* symbols decoded (it grows with the call) */

	/* the paths cleared: from any state (start < 0: a receiver turned
	   by 90, 180 or 270 degrees sees the code sequence of another start
	   state), or from start alone */
	void init(s16 start)
	{
		for (s16 s = 0; s < V34_STATES; s++)
			metric[s] = start < 0 || s == start ? 0 : 1e30f;
		t = n = 0;
	}

	/* a 4D symbol on a code of states (16, 32 or 64): its halves'
	   subset metrics da, db and points ca, cb; v0 inverts Y0 (V.34's
	   superframe sync, 0 else).  1 with out[] the points of the 4D
	   symbol TB - 1 back, 0 while the paths fill */
	s16 step(s16 states, s16 v0, const float da[8],
		 const s32 ca[8], const float db[8],
		 const s32 cb[8], s32 out[2])
	{
		float gm[32], m2[V34_STATES], best;
		s32 ga[32], gb[32];
		s16 l0, l1, g, s, y, bs, k, tt = t;
		/* the 16 state code only uses Y1 and Y2 */
		s16 ny = states == 16 ? 4 : 16;

		/* the best pair of subsets for each encoder input (Y1 Y2,
		   or Y1 to Y4) and Y0 */
		for (g = 0; g < 2 * ny; g++)
			gm[g] = 1e30f;
		for (l0 = 0; l0 < 8; l0++)
			for (l1 = 0; l1 < 8; l1++) {
				float m = da[l0] + db[l1];
				g = (v34_conv_in(l0, l1) & (ny - 1)) +
				    ((l0 ^ l1) & 1) * ny;
				if (m < gm[g]) {
					gm[g] = m;
					ga[g] = ca[l0];
					gb[g] = cb[l1];
				}
			}
		for (s = 0; s < states; s++)
			m2[s] = 1e30f;
		for (s = 0; s < states; s++) {
			/* U0 = Y0 ^ V0 (no precoding: C0 = 0) */
			s16 y0 = (s ^ v0) & 1;
			if (metric[s] >= 1e29f)
				continue;
			for (y = 0; y < ny; y++) {
				s16 ns = v34_trellis_next(states, s, y);
				float m = metric[s] + gm[y + y0 * ny];
				if (m < m2[ns]) {
					m2[ns] = m;
					prev[tt][ns] =
						static_cast<unsigned char>(s);
					pa[tt][ns] = ga[y + y0 * ny];
					pb[tt][ns] = gb[y + y0 * ny];
				}
			}
		}
		best = m2[0];
		bs = 0;
		for (s = 1; s < states; s++)
			if (m2[s] < best) {
				best = m2[s];
				bs = s;
			}
		for (s = 0; s < states; s++)
			metric[s] = m2[s] - best;

		t = (tt + 1) % TB;
		if (++n < TB)
			return 0;
		/* trace back from the best state to the oldest symbol kept */
		s = bs;
		for (k = 0; k < TB - 1; k++) {
			s = prev[tt][s];
			tt = (tt + TB - 1) % TB;
		}
		out[0] = pa[tt][s];
		out[1] = pb[tt][s];
		return 1;
	}

	/* the 4D symbols not yet out (up to TB - 1, fewer while the paths
	   were still filling), on the best path, oldest first, into out;
	   how many.  For a transmitter's encoder that starts anew (V.92's
	   B1u in a fast parameter exchange): then init(0) */
	s16 flush(s32 (*out)[2])
	{
		s16 m = static_cast<s16>(n < TB ? n : TB - 1), k, s, bs = 0,
		    tt = static_cast<s16>((t + TB - 1) % TB);
		for (s = 1; s < V34_STATES; s++)
			if (metric[s] < metric[bs])
				bs = s;
		s = bs;
		for (k = m - 1; k >= 0; k--) {
			out[k][0] = pa[tt][s];
			out[k][1] = pb[tt][s];
			s = prev[tt][s];
			tt = static_cast<s16>((tt + TB - 1) % TB);
		}
		return m;
	}
};

struct V34DataRx {
	V34Params p;
	CoreBits link;          /* the data */
	s16 sbc, z;
	s16 e[8];               /* rotation << 9 | point index */
	s16 pos;
	Viterbi4d<V34_TB> vit;
	s16 half, pat, v0;      /* superframe inversions, as the transmitter */
	s16 half2;              /* the first 2D symbol of a 4D one is in: */
	float da[8];
	s32 ca[8];
	V34Precoder pc; /* the receiver's copy of the precoder */

	void viterbi_init();
	void init(const V34Params *p_);
	void decode_frame(u32 *scr, s16 tap);
	void point(s32 pt, u32 *scr, s16 tap);
	cplx symbol(cplx z_, u32 *scr, s16 tap);
};


/*
 *    primary channel transmitter (v34_modem.cpp)
 */
enum v34_txseg {
	V34_TX_SILENCE,
	V34_TX_S,
	V34_TX_SBAR,
	V34_TX_PP,
	V34_TX_TRN,
	V34_TX_J,
	V34_TX_JP,
	V34_TX_MP,
	V34_TX_JA,      /* V.90's analogue modem: Ja, the DIL descriptor in
			   bits (repeated) */
	V34_TX_CP,      /* V.90's analogue modem: CP frames (repeated), the
			   next one or E after a whole frame */
	V34_TX_E,
	V34_TX_DATA,
};

struct V34Tx {
	QamTx qam;
	s16 baud;
	u32 scr;
	s16 tap;
	enum v34_txseg seg;
	s32 n;                  /* symbols sent in this segment */
	s16 idx;                /* last training point, for differential coding */
	s16 s_odd;              /* S-bar after an S of odd length */
	s16 trn16;              /* phase 4 TRN, MP and E of 16 points (the
				   remote's J asked for them) */
	unsigned char bits[2800];       /* MP, or Ja (V90_JA_MAX) */
	s16 nbits, pos;
	V34Mp mp;       /* the MP we send */
	s16 mp_sent;            /* complete MP / MP' sequences sent */
	s16 ack_mp;             /* set the acknowledge bit from the next MP on */
	/* CP: the frame to send from the end of the current one on (0
	   bits: the same again), whole frames of it sent, E instead */
	unsigned char cp_next[1024];
	s16 cp_next_n;
	s32 cp_frames;
	s16 cp_to_e;
	V34DataTx dtx;     /* data mode, when data_on */
	s16 data_on;

	s16 dibit(s16 b0, s16 b1);
	void init(s16 baud_, s16 high, s16 caller);
	void segment(enum v34_txseg seg_);
	void start_data(const V34Params *p, CoreBits link);
	float sample();
};

/*
 *    primary channel receiver
 */
enum v34_rxstage {
	V34_RX_IDLE,            /* waiting for S */
	V34_RX_S,               /* in S, waiting for S-bar */
	V34_RX_SBAR,
	V34_RX_PP,
	V34_RX_TRN,             /* trained on the 4 point signals */
	V34_RX_DATA,
};

/* V.90: the DIL descriptor of Ja (Table 12/V.90) */
struct V90Dil {
	s16 n, lsp, ltp;
	unsigned char sp[128], tp[128];
	s16 h[8], ref[8];
	unsigned char ucode[255];

	s16 parse(const unsigned char *b, s16 len);
	/* the descriptor's bits into b (the analogue modem's Ja): how
	   many (even) */
	s16 build(unsigned char *b) const;
};

typedef struct { double re, im; } lscplx;

struct V34Rx {
	QamRx qam;
	s16 baud;
	s16 tap;                /* the remote's scrambler */
	s16 phase4;             /* the next S starts phase 4 (no PP) */
	enum v34_rxstage stage;
	s32 n;                  /* symbols in this stage */
	s32 sym;
	cplx z1, z2;            /* the last symbols, for S detection */
	float s_score, sbar_score;
	s16 s_count;
	s16 on;                 /* signal present */
	float mse;
	float sig;              /* data mode: the points' power, in the
				   units of mse (its SNR: sig / mse) */
	float ref_power;        /* input power when trained */
	cplx good_eq[QAM_MAX_EQ];       /* the equalizer at its best */
	cplx pp_buf[48];        /* the first PP symbols, for the alignment */
	s16 pp_off;
	s16 have_good;
	s16 timing_fixed;       /* the timing follows the equalizer */
	float c0, t_freq;       /* trained equalizer delay, clock offset */
	std::vector<lscplx> ls_r, ls_p; /* least squares training */
	s16 phase4_ls, have_ls;
	float ls_d;             /* delay of the last solution */
	s32 ls_sym;             /* at this symbol */
	s32 ls_n;               /* TRN symbols */
	float x_ref;            /* equalizer input energy when trained */
	/* differential decoder for J, MP and E */
	s16 idx;
	u32 scr;
	u32 sr;                 /* last decoded bits, newest in bit 0 */
	s16 j_count, ev_j, ev_jp, ev_s, ev_e;
	s16 j16;                /* the remote asked for 16 point TRN */
	s16 jword;
	unsigned char mpbits[200];
	/* V.90 digital modem: Ja carries the DIL descriptor */
	s16 v90, ev_ja;
	s16 sbar_count;         /* S to S-bar transitions seen */
	s16 s_in_trn;           /* V.90: S may follow TRN directly (the
				   analogue modem's TRN1a during DIL) */
	/* and CP in phase 4 */
	s16 ev_cpt, ev_cp, ev_cpack;
	s16 cp_count;           /* good CP frames (not CPt) so far */
	s16 cp_silence;         /* the last one asked for silence (CPs) */
	unsigned char cpraw[1800];      /* the last good CP frame */
	std::unique_ptr<SyncFrames> fr; /* Ja, then CP (set_v90()) */
	V90Dil dil;
	s16 mp_n, mp_len;       /* collecting an MP frame */
	s16 ones;               /* ones since the end of the last MP frame */
	s16 after_mp;
	s16 ev_mp;
	V34Mp mp;       /* the remote's MP */
	s32 mp_end;             /* symbol of the end of the last MP frame */
	V34DataRx drx;     /* data mode, when data_on */
	cplx last_d[3];         /* the last symbols before data mode */
	double err_r[8];        /* error autocorrelation, lags 0 to 3 */
	cplx err_h[3];
	s32 err_n;
	s16 data_on;
	/* rate renegotiation (11.6/V.34): the remote's S in data mode */
	s16 ev_reneg;
	float mse_mid, mse_old; /* data mode mse 64 and 128 symbols ago */

	s16 init(s16 baud_, s16 high, s16 caller);
	s16 set_v90();
	void release();
	void set_stage(enum v34_rxstage s);
	void frame_start();
	void ja_bit(s16 bit);
	void cp_bit(s16 bit);
	void got_bit(s16 bit);
	void diff_decode(cplx z);
	void pp_align();
	float eq_delay() const;
	void timing_fix();
	void ls_collect(cplx d);
	s16 ls_solve();
	void ls_window(cplx d, s32 start);
	void ls_train(cplx d);
	void timing_dd(cplx d);
	s16 level_ok();
	void precode_eq(const V34Params *p);
	void reneg_s();
	void err_collect(cplx e);
	cplx precode_start(cplx z);
	void symbol();
	float precoder(s16 h[6]) const;
	/* clears the error autocorrelation that precoder() works from */
	void err_reset()
	{
		zero(err_r);
		zero(err_h);
		err_n = 0;
	}
	void start_data(const V34Params *p, CoreBits link);
	void sample(float x);
};


} // namespace dsp

#endif /* __V34_H__ */
