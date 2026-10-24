/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v91.h - ITU-T V.91: two digital modems on a 4-wire digital connection
 *            (here: two D-Modems on a G.711 VoIP call), PCM both ways at
 *            28 000 to 62 667 bit/s
 *
 */

#ifndef V91_H
#define V91_H


#include "dm_types.h"
#include "v90.h"

namespace dsp {

/* the data mode encoder of V.91 (5.4/V.90 with Sr = 0, S = 6): K bits
   through the modulus encoder, 6 sign bits coded differentially */
struct V91Enc {
	s16 K;
	s16 M[6];
	unsigned char ucode[6][128];    /* label -> Ucode, largest first */
	const s16 *lin;
	u32 scr;
	s16 last;                       /* the last sign */

	s16 init(const V90Cp *cp, s16 D, const s16 *lin_);
	/* a data frame of 6 linear values from K + 6 bits, or from K bits
	   and 6 control channel bits (cc, not scrambled) */
	void frame(const unsigned char *bits, const unsigned char *cc,
		   s16 out[6]);
};

/* what the receiver learnt from the DIL: per data frame interval, the
   received values of the codes that come through distinguishably */
struct V91Map {
	s16 n[6];
	s16 val[6][256];                /* sorted */
	unsigned char ucode[6][256];
	unsigned char sign[6][256];
	unsigned char ok[6][128];       /* usable Ucodes */

	/* the Ucode and sign nearest to a received value; returns the
	   distance */
	s32 decode(s16 i, s16 x, s16 *u, s16 *s) const;
};

/* and its decoder, for the constellations of our own CP */
struct V91Dec {
	s16 K;
	s16 M[6];
	s16 label[6][128];              /* Ucode -> label, -1: none */
	u32 scr;
	s16 last;
	s32 errors;                     /* symbols off the constellation */

	s16 init(const V90Cp *cp, s16 D);
	/* the bits of a data frame (K + 6, or K with cc); -1 if it cannot
	   be one */
	s16 frame(const s16 *u, const s16 *s, unsigned char *bits,
		  unsigned char *cc);
};

enum v91_tx {
	V91T_EZ, V91T_INFO, V91T_EU, V91T_PHIL, V91T_EM, V91T_DIL,
	V91T_SCR, V91T_CP, V91T_ES, V91T_B1, V91T_DATA,
};

enum v91_rx {
	V91R_INFO, V91R_J, V91R_EM, V91R_DIL, V91R_CP, V91R_ES, V91R_B1,
	V91R_DATA,
};

/* events of V91::sample() */
enum {
	V91_NONE,
	V91_CONNECT,            /* data mode both ways, the first time */
	V91_ESCAPE,             /* go to V.34 phase 2 (8.3) */
	V91_FAIL,               /* cleardown, or a retrain that failed */
};

#define V91_INFO_BITS   62
#define V91_CP_MAX      1024

struct V91 {
	s16 alaw;
	s16 lin[128];           /* Ucode -> linear, ours */
	s16 caller;
	s16 want_cc;            /* we ask for the control channel */
	s16 max_k;              /* the most we take (64 000 needs 42) */
	CoreBits link;          /* the data */
	s32 n;                  /* symbols since the start */
	s16 esn;
	s32 phase_start;        /* of the last INFO exchange */
	s16 connected, retraining;
	s32 retrain_at;         /* test: our own retrain (symbols) */
	struct dp_stats *stats; /* the call's statistics (NULL: none) */
	s16 mute;               /* test: we send nothing */

	/* transmitter */
	enum v91_tx tx;
	s32 tn;                 /* symbols of the segment */
	s32 tk;                 /* since Eu / Em: the data frame interval */
	s16 t;                  /* the differential encoder: last sign */
	u32 tscr;
	unsigned char tb[V91_CP_MAX];   /* INFO or CP bits */
	s16 tbn, tbpos;
	s16 info_ack_sent, cp_ack_sent, cp_sent;
	V90Dil dil;             /* the DIL we send */
	s16 dil_seg, dil_k;
	V91Enc enc;
	s16 out[6], opos;
	s32 tframes;            /* data frames sent */
	unsigned char ccseq[96];

	/* receiver */
	enum v91_rx rx;
	s32 rk;                 /* since the remote's Eu / Em */
	s16 rs;                 /* last sign received */
	u32 rscr;
	unsigned char ib[V91_INFO_BITS];        /* the last bits, oldest first */
	s32 since_info, zeros;
	s16 got_info, got_info_ack, peer_default, peer_cc, peer_alaw;
	s16 peer_transparent;
	s16 have_j;
	V90Dil rdil;            /* the DIL we asked for */
	s16 rd_seg, rd_k;
	s16 obs[6][128][2];
	unsigned char seen[6][128][2];
	V91Map map;
	V90Cp cp_tx;            /* our CP: what the remote sends us */
	V90Cp cp_rx;            /* the remote's: what we send */
	s16 our_drn, cp_ready;
	SyncFrames fr;          /* J, then CP */
	s16 cp_pad;             /* the fill after a CP frame still to come */
	s16 got_cp, got_cp_ack, got_es;
	u32 sync;               /* the last bits, newest in bit 0 */
	V91Dec dec;
	s16 ru[6], rsg[6], rpos, bframes;
	s32 rframes;
	s32 err_window, err_count;

	void init(s16 alaw_, s16 caller_, s32 max_k_, s16 want_cc_);
	s16 sample(s16 x, s16 *y);

	/* transmitter */
	void tx_segment(enum v91_tx s);
	void make_info(s16 ack);
	void make_cp(s16 ack);
	s16 sign_sample(s16 bit);
	s16 scr_sample(s16 bit);
	s16 dil_sample();
	s16 tx_sample();
	/* receiver */
	void rx_restart();
	s16 info_bit(s16 bit);
	void dil_record(s16 x);
	s16 choose();
	s16 cp_bit(s16 bit);
	void data_symbol(s16 x);
	s16 rx_sample(s16 x);
	void retrain(const char *why);
};

} // namespace dsp

#endif
