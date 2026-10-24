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
	int K;
	int M[6];
	unsigned char ucode[6][128];    /* label -> Ucode, largest first */
	const int *lin;
	u32 scr;
	int last;                       /* the last sign */

	int init(const V90Cp *cp, int D, const int *lin_);
	/* a data frame of 6 linear values from K + 6 bits, or from K bits
	   and 6 control channel bits (cc, not scrambled) */
	void frame(const unsigned char *bits, const unsigned char *cc,
		   int out[6]);
};

/* what the receiver learnt from the DIL: per data frame interval, the
   received values of the codes that come through distinguishably */
struct V91Map {
	int n[6];
	s16 val[6][256];                /* sorted */
	unsigned char ucode[6][256];
	unsigned char sign[6][256];
	unsigned char ok[6][128];       /* usable Ucodes */

	/* the Ucode and sign nearest to a received value; returns the
	   distance */
	s32 decode(int i, int x, int *u, int *s) const;
};

/* and its decoder, for the constellations of our own CP */
struct V91Dec {
	int K;
	int M[6];
	s16 label[6][128];              /* Ucode -> label, -1: none */
	u32 scr;
	int last;
	s32 errors;                     /* symbols off the constellation */

	int init(const V90Cp *cp, int D);
	/* the bits of a data frame (K + 6, or K with cc); -1 if it cannot
	   be one */
	int frame(const int *u, const int *s, unsigned char *bits,
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
	int alaw;
	int lin[128];           /* Ucode -> linear, ours */
	int caller;
	int want_cc;            /* we ask for the control channel */
	int max_k;              /* the most we take (64 000 needs 42) */
	CoreBits link;          /* the data */
	s32 n;                  /* symbols since the start */
	int esn;
	s32 phase_start;        /* of the last INFO exchange */
	int connected, retraining;
	s32 retrain_at;         /* test: our own retrain (symbols) */
	struct dp_stats *stats; /* the call's statistics (NULL: none) */
	int mute;               /* test: we send nothing */

	/* transmitter */
	enum v91_tx tx;
	s32 tn;                 /* symbols of the segment */
	s32 tk;                 /* since Eu / Em: the data frame interval */
	int t;                  /* the differential encoder: last sign */
	u32 tscr;
	unsigned char tb[V91_CP_MAX];   /* INFO or CP bits */
	int tbn, tbpos;
	int info_ack_sent, cp_ack_sent, cp_sent;
	V90Dil dil;             /* the DIL we send */
	int dil_seg, dil_k;
	V91Enc enc;
	int out[6], opos;
	s32 tframes;            /* data frames sent */
	unsigned char ccseq[96];

	/* receiver */
	enum v91_rx rx;
	s32 rk;                 /* since the remote's Eu / Em */
	int rs;                 /* last sign received */
	u32 rscr;
	unsigned char ib[V91_INFO_BITS];        /* the last bits, oldest first */
	s32 since_info, zeros;
	int got_info, got_info_ack, peer_default, peer_cc, peer_alaw;
	int peer_transparent;
	int have_j;
	V90Dil rdil;            /* the DIL we asked for */
	int rd_seg, rd_k;
	s16 obs[6][128][2];
	unsigned char seen[6][128][2];
	V91Map map;
	V90Cp cp_tx;            /* our CP: what the remote sends us */
	V90Cp cp_rx;            /* the remote's: what we send */
	int our_drn, cp_ready;
	SyncFrames fr;          /* J, then CP */
	int cp_pad;             /* the fill after a CP frame still to come */
	int got_cp, got_cp_ack, got_es;
	u32 sync;               /* the last bits, newest in bit 0 */
	V91Dec dec;
	int ru[6], rsg[6], rpos, bframes;
	s32 rframes;
	s32 err_window, err_count;

	void init(int alaw_, int caller_, int max_k_, int want_cc_);
	int sample(int x, int *y);

	/* transmitter */
	void tx_segment(enum v91_tx s);
	void make_info(int ack);
	void make_cp(int ack);
	int sign_sample(int bit);
	int scr_sample(int bit);
	int dil_sample();
	int tx_sample();
	/* receiver */
	void rx_restart();
	int info_bit(int bit);
	void dil_record(int x);
	int choose();
	int cp_bit(int bit);
	void data_symbol(int x);
	int rx_sample(int x);
	void retrain(const char *why);
};

} // namespace dsp

#endif
