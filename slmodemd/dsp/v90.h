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

struct v90_tx;

enum v90_seg {
	V90_SILENCE,
	V90_SD,         /* Sd, 384 symbols, then Sd-bar (48), then TRN1d */
	V90_SDBAR,
	V90_TRN1D,      /* until the next segment, in whole data frames */
	V90_JD,         /* Jd frames until v90_tx_jd_end() */
	V90_JDP,        /* Jd', then DIL (or silence when N is 0) */
	V90_DIL,        /* until v90_tx_dil_end() */
	V90_RI,         /* Ri until v90_tx_ri_end(), then Ri-bar (24) */
	V90_RIBAR,
	V90_ENC,        /* data frames from the encoder */
};



void v90_tx_init(struct v90_tx *t, int alaw, int uinfo);
void v90_tx_segment(struct v90_tx *t, enum v90_seg seg);
void v90_tx_jd_end(struct v90_tx *t);
void v90_tx_dil_end(struct v90_tx *t);
void v90_tx_ri_end(struct v90_tx *t);
int  v90_tx_sample(struct v90_tx *t);   /* the next PCM sample (linear) */

/* CP (Table 14/V.90): the analogue modem's constellation parameters */
struct v90_cp {
	int cp;                 /* 0: CPt, 1: CP */
	int drn, silence, sr, ack, alaw;
	int rate_mask;          /* analogue -> digital, bit 0: 4800 */
	int ld;
	int a1, a2, b1, b2;     /* Q1.6 */
	int con[6];             /* constellation of each frame interval */
	int ncon;
	unsigned char mask[6][128];     /* by Ucode */
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
struct v90_enc {
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
};

struct v90_tx {
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
	struct v90_dil dil;
	int dil_seg, dil_k, dil_end, dil_rounds;
	int next;               /* after Jd' / DIL: 1 when the next
				   segment may start */
	long trn1d_len;         /* then Jd */
	int ri_end;
	/* phase 4 and data: frames of D bits through the encoder */
	struct v90_enc enc;
	void (*get_frame)(void *ctx, struct v90_tx *t, unsigned char *bits,
			  int *last);
	void *ctx;
	int out[6], opos;
};

/* D = S + K bits per data frame of 6 symbols; returns -1 when the
   constellations cannot carry K bits.  Scrambler, sign coding and shaping
   filter start from zero. */
int  v90_enc_init(struct v90_enc *e, const struct v90_cp *cp, int D,
		  const int *lin);
/* the next data frame of D bits (scrambled here); last: no look-ahead
   past it (the encoder starts again after it) */
void v90_enc_push(struct v90_enc *e, const unsigned char *bits, int last);
/* a data frame of 6 linear values, when one is ready */
int  v90_enc_pop(struct v90_enc *e, int out[6]);

int  v90_cp_length(const unsigned char *b, int n);
int  v90_cp_parse(struct v90_cp *cp, const unsigned char *b, int len);

/* MP of the digital modem (Table 16/V.90); returns its length in bits */
int  v90_mp_build(unsigned char *b, int drn_max, int trellis, int expanded,
		  int ack, int rate_mask, const int *h);

/* the linear value of a PCM codeword and its octet */
int  v90_ucode_linear(int alaw, int ucode);
int  v90_ucode_octet(int alaw, int ucode, int positive);

/* the DIL descriptor in the bits of Ja (from its frame sync on); returns
   the frame length once the bits up to 49 are in, -1 before */
int  v90_dil_length(const unsigned char *b, int n);
int  v90_dil_parse(struct v90_dil *d, const unsigned char *b, int len);

} // namespace dsp

#endif
