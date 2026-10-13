/*
 *
 *    v90.h - V.90 digital modem: the PCM transmitter
 *
 */

#ifndef V90_H
#define V90_H

#include "v34.h"

namespace dsp {

#define V90_JA_MAX      2800    /* bits of a DIL descriptor at most */

struct V90Tx;

enum v90_seg {
	V90_SILENCE,
	V90_SD,         /* Sd, 384 symbols, then Sd-bar (48), then TRN1d */
	V90_SDBAR,
	V90_TRN1D,      /* until the next segment, in whole data frames */
	V90_JD,         /* Jd frames until end_jd() */
	V90_JDP,        /* Jd', then DIL (or silence when N is 0) */
	V90_DIL,        /* until end_dil() */
	V90_RI,         /* Ri until end_ri(), then Ri-bar (24) */
	V90_RIBAR,
	V90_ENC,        /* data frames from the encoder */
	/* V.92 short phase 1 (8.3/V.92) */
	V90_QTS,        /* QTS (768), QTS-bar (48), then ANSpcm */
	V90_QTSBAR,
	V90_ANSPCM,     /* until the next segment */
	/* V.92 PCM upstream (8.6/V.92) */
	V90_SCR,        /* SCR, instead of DIL, until end_dil() */
	V90_RD,         /* Rd (384), Rd-bar (24), then the encoder again */
	V90_RDBAR,
	V90_RF,         /* Rf (384), Rf-bar (24): + + - - (V.92 9.9) */
	V90_RFBAR,
};

/* V.92: the Ucode of QTS for WXYZ of QC1a / QC2a (Table 2/V.92); -1 for
   1111, a cleardown from on-hold */
int  v92_uqts(int wxyz);



int  v90_tx_sample(V90Tx *t);   /* the next PCM sample (linear) */

/* CP (Table 14/V.90): the analogue modem's constellation parameters */
struct V90Cp {
	int cp;                 /* 0: CPt, 1: CP */
	int drn, silence, sr, ack, alaw;
	int rate_mask;          /* analogue -> digital, bit 0: 4800 */
	int ld;
	int a1, a2, b1, b2;     /* Q1.6 */
	int con[6];             /* constellation of each frame interval */
	int ncon;
	unsigned char mask[6][128];     /* by Ucode */

	int parse(const unsigned char *b, int len);
};

/* the data mode encoder (5.4/V.90), for TRN2d, MP, Ed, B1d and data.  The
   spectral shaper decides the signs of a shaping frame from the ld frames
   after it (5.4.5.5), so the encoder takes data frames ahead of the ones
   it gives out. */
#define V90_SHQ 24
struct v90_shf {
	int fs, mag[6], p;              /* magnitudes, p' bits */
	int last;                       /* no look-ahead past it */
};
struct V90Enc {
	int K, S, sr, ld;
	int M[6];
	unsigned char ucode[6][128];    /* label -> Ucode, largest first */
	const int *lin;
	double a1, a2, b1, b2;          /* the spectral shaping filter */
	double x1, y1, v1;
	int Q;                          /* shaper trellis state */
	int t_prev;                     /* signs of the last shaping frame */
	int last;                       /* differential sign coding */
	unsigned scr;
	struct v90_shf q[V90_SHQ];      /* shaping frames not decided yet */
	int qn;
	int sign_acc, mag_acc[6], nsh;  /* the data frame being decided */
	int outq[4][6], outn;           /* data frames ready to go */

	/* D = S + K bits per data frame of 6 symbols; returns -1 when the
	   constellations cannot carry K bits.  Scrambler, sign coding and
	   shaping filter start from zero. */
	int init(const V90Cp *cp, int D, const int *lin_);
	double shape_metric(const int *mag, int n, int signs, double *f) const;
	double best_path(int k, int depth, int Q_, int tprev, const double *f) const;
	void decide_frame(int depth);
	/* the next data frame of D bits (scrambled here); last: no
	   look-ahead past it (the encoder starts again after it) */
	void push(const unsigned char *bits, int last_);
	/* a data frame of 6 linear values, when one is ready */
	int pop(int out[6]);
};

struct V90Tx {
	int alaw;
	int lin[128];           /* Ucode -> linear value, positive */
	enum v90_seg seg;
	long n;                 /* symbols of the segment so far */
	long sym;               /* symbols since Sd: the data frame interval
				   is sym % 6 */
	int uinfo;
	unsigned scr;           /* GPC scrambler */
	int sign;               /* the last sign sent, 1: positive */
	unsigned char bits[80]; /* the Jd frame */
	int nbits, pos;
	int jd_end;             /* complete the Jd frame, then Jd' */
	int jd_frames;          /* Jd frames sent */
	V90Dil dil;
	int dil_seg, dil_k, dil_end, dil_rounds;
	int next;               /* after Jd' / DIL: 1 when the next
				   segment may start */
	long trn1d_len;         /* then Jd */
	/* V.92 PCM upstream: end_jd() goes to Jp first, and after Jp' comes
	   SCR when there is no DIL */
	int v92;
	int jp;                 /* Jp frames go out */
	int jp_frac;            /* Jp: Su's extension, 1/65536 symbol */
	int rd_req;             /* Rd at the next data frame */
	int rf_req;             /* Rf at the next data frame */
	int sil_req;            /* silence after the encoder's last frame */
	int rd_code[6];         /* its codes */
	int ri_end;
	/* phase 4 and data: frames of D bits through the encoder */
	V90Enc enc;
	void (*get_frame)(void *ctx, V90Tx *t, unsigned char *bits,
			  int *last);
	void *ctx;
	int out[6], opos;
	/* V.92 short phase 1 */
	int uqts;
	short anspcm[301];      /* one period of ANSpcm, linear */
	const long *fclock;     /* the line's symbol clock (the caller's
				   count of symbols sent), null: no QTS */
	long fbase;             /* its symbol at the start of QTS: the
				   data frame interval is (*fclock - fbase)
				   % 6 from there on */

	void make_jd();
	void make_jp();
	void anspcm_init(int level);    /* 0..3: -9.5 to -18 dBm0 */

	void init(int alaw_, int uinfo_);
	void segment(enum v90_seg seg_);
	void end_jd();
	void end_dil();
	void end_ri();
	int diff_bit(int bit);
	int dil_sample();
	int sample();
};

/* the length of a CP frame up to the end of its CRC from its first 129
   bits (0: not a CP frame, -1 before they are in) */
int  v90_cp_length(const unsigned char *b, int n);

/* MP of the digital modem (Table 16/V.90); returns its length in bits */
int  v90_mp_build(unsigned char *b, int drn_max, int trellis, int expanded,
		  int ack, int rate_mask, const int *h);

/* the linear value of a PCM codeword and its octet */
int  v90_ucode_linear(int alaw, int ucode);
int  v90_ucode_octet(int alaw, int ucode, int positive);

/* the DIL descriptor in the bits of Ja (from its frame sync on); returns
   the frame length once the bits up to 49 are in, -1 before */
int  v90_dil_length(const unsigned char *b, int n);

} // namespace dsp

#endif
