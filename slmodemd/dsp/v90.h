/*
 *
 *    v90.h - V.90 digital modem: the PCM transmitter
 *
 */

#ifndef V90_H
#define V90_H

#include "v34.h"

#define V90_JA_MAX      2800    /* bits of a DIL descriptor at most */

enum v90_seg {
	V90_SILENCE,
	V90_SD,         /* Sd, 384 symbols, then Sd-bar (48), then TRN1d */
	V90_SDBAR,
	V90_TRN1D,      /* until the next segment, in whole data frames */
	V90_JD,         /* Jd frames until v90_tx_jd_end() */
	V90_JDP,        /* Jd', then DIL (or silence when N is 0) */
	V90_DIL,        /* until v90_tx_dil_end() */
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
};

void v90_tx_init(struct v90_tx *t, int alaw, int uinfo);
void v90_tx_segment(struct v90_tx *t, enum v90_seg seg);
void v90_tx_jd_end(struct v90_tx *t);
void v90_tx_dil_end(struct v90_tx *t);
int  v90_tx_sample(struct v90_tx *t);   /* the next PCM sample (linear) */

/* the linear value of a PCM codeword and its octet */
int  v90_ucode_linear(int alaw, int ucode);
int  v90_ucode_octet(int alaw, int ucode, int positive);

/* the DIL descriptor in the bits of Ja (from its frame sync on); returns
   the frame length once the bits up to 49 are in, -1 before */
int  v90_dil_length(const unsigned char *b, int n);
int  v90_dil_parse(struct v90_dil *d, const unsigned char *b, int len);

#endif
