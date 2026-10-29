/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v90.h - V.90 digital modem: the PCM transmitter
 *
 */

#ifndef V90_H
#define V90_H

#include <cstring>

#include "dm_types.h"
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
s16  v92_uqts(s16 wxyz);




/* CP (Table 14/V.90): the analogue modem's constellation parameters */
struct V90Cp {
	s16 cp;                 /* 0: CPt, 1: CP */
	s16 drn, silence, sr, ack, alaw;
	s16 rate_mask;          /* analogue -> digital, bit 0: 4800 */
	s16 ld;
	s16 a1, a2, b1, b2;     /* Q1.6 */
	s16 con[6];             /* constellation of each frame interval */
	s16 ncon;
	unsigned char mask[6][128];     /* by Ucode */

	s16 parse(const unsigned char *b, s16 len);
	/* the analogue modem's CP in bits (Table 14/V.90): one per byte,
	   the first in b[0]; how many (to the end of the fill).  The TRN1d
	   rms ratio 1, the codec's constellations the transmitter's */
	s16 build(unsigned char *b) const;
	/* whether the constellations carry data frames of D bits, S of
	   them sign bits, as the encoders (at most 48 bits) take them */
	s16 usable(s16 D, s16 S) const;
};

/* the data mode encoder (5.4/V.90), for TRN2d, MP, Ed, B1d and data.  The
   spectral shaper decides the signs of a shaping frame from the ld frames
   after it (5.4.5.5), so the encoder takes data frames ahead of the ones
   it gives out. */
#define V90_SHQ 24
struct v90_shf {
	s16 fs, mag[6], p;              /* magnitudes, p' bits */
	s16 last;                       /* no look-ahead past it */
};
struct V90Enc {
	s16 K, S, sr, ld;
	s16 M[6];
	unsigned char ucode[6][128];    /* label -> Ucode, largest first */
	const s16 *lin;
	double a1, a2, b1, b2;          /* the spectral shaping filter */
	double x1, y1, v1;
	s16 Q;                          /* shaper trellis state */
	s16 t_prev;                     /* signs of the last shaping frame */
	s16 last;                       /* differential sign coding */
	u32 scr;
	struct v90_shf q[V90_SHQ];      /* shaping frames not decided yet */
	s16 qn;
	s16 sign_acc, mag_acc[6], nsh;  /* the data frame being decided */
	s16 outq[4][6], outn;           /* data frames ready to go */

	/* D = S + K bits per data frame of 6 symbols; returns -1 when the
	   constellations cannot carry K bits.  Scrambler, sign coding and
	   shaping filter start from zero. */
	s16 init(const V90Cp *cp, s16 D, const s16 *lin_);
	double shape_metric(const s16 *mag, s16 n, s16 signs, double *f) const;
	double best_path(s16 k, s16 depth, s16 Q_, s16 tprev, const double *f) const;
	void decide_frame(s16 depth);
	/* the next data frame of D bits (scrambled here); last: no
	   look-ahead past it (the encoder starts again after it) */
	void push(const unsigned char *bits, s16 last_);
	/* a data frame of 6 linear values, when one is ready */
	s16 pop(s16 out[6]);
};

struct V90Tx {
	s16 alaw;
	s16 lin[128];           /* Ucode -> linear value, positive */
	enum v90_seg seg;
	s32 n;                  /* symbols of the segment so far */
	s32 sym;                /* symbols since Sd: the data frame interval
				   is sym % 6 */
	s16 uinfo;
	u32 scr;                /* GPC scrambler */
	s16 sign;               /* the last sign sent, 1: positive */
	unsigned char bits[80]; /* the Jd frame */
	s16 nbits, pos;
	s16 jd_end;             /* complete the Jd frame, then Jd' */
	s32 jd_frames;          /* Jd frames sent */
	V90Dil dil;
	s16 dil_seg, dil_k, dil_end;
	s32 dil_rounds;
	s16 next;               /* after Jd' / DIL: 1 when the next
				   segment may start */
	s32 trn1d_len;          /* then Jd */
	/* V.92 PCM upstream: end_jd() goes to Jp first, and after Jp' comes
	   SCR when there is no DIL */
	s16 v92;
	s16 jp;                 /* Jp frames go out */
	s32 jp_frac;            /* Jp: Su's extension, 1/65536 symbol */
	s16 rd_req;             /* Rd at the next data frame */
	s16 rf_req;             /* Rf at the next data frame */
	s16 sil_req;            /* silence after the encoder's last frame */
	s16 rd_code[6];         /* its codes */
	s16 ri_end;
	/* phase 4 and data: frames of D bits through the encoder */
	V90Enc enc;
	void (*get_frame)(void *ctx, V90Tx *t, unsigned char *bits,
			  s16 *last);
	void *ctx;
	s16 out[6], opos;
	/* V.92 short phase 1 */
	s16 uqts;
	s16 anspcm[301];        /* one period of ANSpcm, linear */
	const s32 *fclock;         /* the line's symbol clock (the caller's
				   count of symbols sent), null: no QTS */
	s32 fbase;                 /* its symbol at the start of QTS: the
				   data frame interval is (*fclock - fbase)
				   % 6 from there on */

	void make_jd();
	void make_jp();
	void anspcm_init(s16 level);    /* 0..3: -9.5 to -18 dBm0 */

	void init(s16 alaw_, s16 uinfo_);
	void segment(enum v90_seg seg_);
	void end_jd();
	void end_dil();
	void end_ri();
	s16 diff_bit(s16 bit);
	s16 dil_sample();
	s16 sample();
};

/* what the digital modem's encoder sends in phase 4 and data mode:
   TRN2d, a sequence (V.90's MP, V.92's CPd or SUVd) until the remote
   acknowledges it, Ed, then B1d and data, or the silence the remote
   asked for in a renegotiation */
enum pcm_fmode { PCM_TRN, PCM_SEQ, PCM_ED, PCM_B1, PCM_DATA, PCM_SIL };

#define PCM_SEQ_MAX     8192

/* the frames of D bits for V90Tx (its get_frame, pcm_frame_cb, with the
   host as ctx), in that order.  The host H has
 *
 *     void fs_reinit(V90Tx *t)         before each frame: the encoder
 *                                      on other constellations (and D)
 *                                      when they changed
 *     const V90Cp *fs_data_cp()        the data mode's constellations
 *     void fs_seq_next(s16 first)      the next sequence into seq[]
 *                                      (first: after TRN2d)
 *     bool fs_acked()                  the remote acknowledged ours
 *     bool fs_silence()                a silence after Ed
 *     void fs_data(unsigned char *b, s16 n)    the data bits
 *
 * and may have fs_seq_frame() (a frame of the sequence went out),
 * fs_to_ed() and fs_ed_end() */
template <class H>
struct PcmFrames {
	enum pcm_fmode fmode;
	s32 fcount;             /* frames in the mode */
	s16 D;                  /* bits per frame */
	unsigned char seq[PCM_SEQ_MAX]; /* one bit each, in whole frames */
	s16 seq_len, seq_pos;
	s16 seq_ack;            /* the sequence acknowledges the remote's */
	s16 ack_sent;           /* one that did went out */

	/* V90Tx's get_frame */
	static void pcm_frame_cb(void *ctx, V90Tx *t, unsigned char *b,
				 s16 *last)
	{
		static_cast<H*>(ctx)->PcmFrames::pcm_frame(t, b, last);
	}

	/* the defaults, for a host without its own (see above) */
	void fs_seq_frame() {}
	/* (see fs_seq_frame()) */
	void fs_to_ed() {}
	/* (see fs_seq_frame()) */
	void fs_ed_end() {}

	/* the next frame into b; *last at the end of Ed (the encoder
	   starts again after it) */
	void pcm_frame(V90Tx *t, unsigned char *b, s16 *last)
	{
		H &h = static_cast<H&>(*this);

		h.fs_reinit(t);
		if (fmode == PCM_B1 && fcount == 0) {
			/* B1d with the data mode parameters, from cleared
			   scrambler, signs and shaping filter */
			const V90Cp *cp = h.fs_data_cp();
			D = cp->drn + 20;
			t->enc.init(cp, D, t->lin);
		}
		fcount++;
		switch (fmode) {
		case PCM_TRN:
			std::fill_n(b, D, 1);
			/* TRN2d: 2160 symbols */
			if (fcount >= 360) {
				fmode = PCM_SEQ;
				h.fs_seq_next(1);
			}
			break;
		case PCM_SEQ:
			memcpy(b, seq + seq_pos, D);
			seq_pos += D;
			h.fs_seq_frame();
			if (seq_pos < seq_len)
				break;
			if (seq_ack)
				ack_sent = 1;
			if (ack_sent && h.fs_acked()) {
				h.fs_to_ed();
				fmode = PCM_ED;
				fcount = 0;
			} else
				h.fs_seq_next(0);
			break;
		case PCM_ED:
			/* 2 frames of zeros */
			std::fill_n(b, D, 0);
			if (fcount >= 2) {
				*last = 1;
				fcount = 0;
				if (h.fs_silence()) {
					fmode = PCM_SIL;
					t->sil_req = 1;
				} else
					fmode = PCM_B1;
				h.fs_ed_end();
			}
			break;
		case PCM_SIL:
			std::fill_n(b, D, 0);
			break;
		case PCM_B1:
			std::fill_n(b, D, 1);
			if (fcount >= 48)
				fmode = PCM_DATA;
			break;
		case PCM_DATA:
			h.fs_data(b, D);
			break;
		}
	}
};

/* the length of a CP frame up to the end of its CRC from its first 129
   bits (0: not a CP frame, -1 before they are in) */
s16  v90_cp_length(const unsigned char *b, s16 n);

/* MP of the digital modem (Table 16/V.90); returns its length in bits */
s16  v90_mp_build(unsigned char *b, s16 drn_max, s16 trellis, s16 expanded,
		  s16 ack, s16 rate_mask, const s16 *h);

/* the linear value of a PCM codeword and its octet */
s16  v90_ucode_linear(s16 alaw, s16 ucode);
s16  v90_ucode_octet(s16 alaw, s16 ucode, s16 positive);

/* the DIL descriptor in the bits of Ja (from its frame sync on); returns
   the frame length once the bits up to 49 are in, -1 before */
s16  v90_dil_length(const unsigned char *b, s16 n);

} // namespace dsp

#endif
