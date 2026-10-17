/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v92a.h - V.92 analogue modem: the PCM upstream transmitter
 *             (docs/dsp/v90a.md)
 *
 *    PAM symbols at 8000 a second, one for each sample of the network's
 *    codec, on our line at 9600 Hz (or 8000).  Each symbol is a sinc
 *    pulse cut at 4000 Hz: where the network's codec samples it, the
 *    other symbols' pulses are at their zeros.  The symbol clock is the
 *    network's as our PCM receiver measures it (set_ppm()); V.92's
 *    fractional extensions of Su-bar move the symbols later by part of
 *    a symbol.
 *
 *    Phase 3 (9.5.2/V.92): Ru, Ru-bar, TRN1u, Ja until it is asked to
 *    stop, then silence; Su, Su-bar of 24.5 T, Su until Jp is in,
 *    Su-bar of 24 T and Jp's fraction, the second TRN1u (the data
 *    frames count from its first symbol) until CPt is ready, CPt until
 *    Ri, then E1u.  Phase 4 (9.6.2/V.92): TRN2u on 4 points, then
 *    sequences (SUVu, CPu) on them, each the caller's next or the same
 *    again, E2u, and the data mode: B1u, then the data (6.4/V.92: the
 *    modulus encoder, the trellis code, the precoder and the prefilter
 *    of the digital modem's CPd).
 *
 */

#ifndef V92A_H
#define V92A_H

#include "v92.h"

namespace dsp {

#define V92A_SPAN       16      /* the pulse's symbols either side */
#define V92A_P          64      /* its table's points a symbol */
#define V92A_RING       64      /* symbols kept (a power of 2, past 2
				   V92A_SPAN + 2) */
#define V92A_BITS       2800    /* Ja's DIL descriptor, or CPt */

enum v92a_seg {
	V92A_SIL,
	V92A_RU,                /* 384 T, then Ru-bar 24 T */
	V92A_RUBAR,
	V92A_TRN1,              /* TRN1u: 2040 T, then Ja; the second
				   until trn_stop, then CPt */
	V92A_JA,                /* until ja_stop, at a 12 bit boundary */
	V92A_SU,                /* 144 T; the second until jp_req */
	V92A_SUBAR,             /* 24.5 T; the second 24 T and frac */
	V92A_CPT,               /* 24 ones, CPt until ri_seen */
	V92A_E1U,               /* a data frame of zeros, then TRN2u */
	V92A_TRN2,              /* until trn2_stop, then the sequences */
	V92A_SEQ,               /* bits[] again and again, nxt[] (if
				   any) after each; E2u after one once
				   e2u_req */
	V92A_E2U,               /* a data frame of zeros, then data */
	V92A_DATA,              /* B1u, then link's bits */
};

#define V92A_HIST       (V92_PMAX + 1)  /* the filters' past */

struct V92ATx {
	int rate;               /* the line's, 9600 or 8000 */
	float lu;               /* LU (8.5.7/V.92) on the line */
	double T;               /* line samples a symbol */
	float pulse[2 * V92A_SPAN * V92A_P + 2];
	/* the symbols out, by symbol, at their times (line samples) */
	float a[V92A_RING];
	double at[V92A_RING];
	long nsym;              /* symbols made */
	double tnext;           /* the next one's time */
	double delay;           /* symbols to move it later by */
	long m;                 /* line samples out */

	enum v92a_seg seg;
	long n;                 /* symbols in the segment */
	int second;             /* Su and TRN1u: the second ones */
	long su_k;              /* Su's pattern position */
	unsigned scr;           /* GPA */
	int last_d;             /* the differential encoder: 1 negative */
	unsigned char bits[V92A_BITS];
	int nbits;
	long bn;                /* bits sent of Ja or CPt */
	int ja_stop, jp_req, trn_stop, ri_seen;
	double jp_frac;         /* of a symbol */
	int next;               /* E1u is made */
	double next_at;         /* the time of its last symbol */
	/* phase 4 */
	int trn2_stop;
	unsigned char nxt[V92A_BITS];
	int nxt_n;              /* 0: the current sequence again */
	int pos;
	long seqs;              /* sequences sent whole */
	int e2u_req;
	/* the data mode: the CPd's parameters, the bits, the frames sent
	   (B1u the first 48), the data frame interval, the modulus
	   encoder's K and its sign's memory, the trellis encoder's state
	   and the frame's points, the filters' past (u, x, v by symbol) */
	V92Cpd cpd;
	CoreBits link;
	int K;
	long frames;
	int fi;
	int kq[12];
	int dprev;
	int tstate;
	int eta[4];
	double uh[V92A_HIST], xh[V92A_HIST], vh[V92A_HIST];
	long dn;                /* data symbols */

	/* silent, on a line of rate (9600 or 8000), at level lu */
	void init(int rate_, float lu_);
	/* the network's clock against ours, parts per million (the PCM
	   receiver's) */
	void set_ppm(double ppm);
	/* the next segment from its next symbol */
	void segment(enum v92a_seg s);
	/* the next line sample */
	float sample();
	/* E1u is on the line, its last pulse out (symbols are made
	   V92A_SPAN ahead of it) */
	bool e1u_sent() const
	{
		return next && m > next_at + V92A_SPAN * T;
	}

private:
	float symbol();
	float dbit(int bit);
	float trn();
	float sym4(int sign, int mag);
	float data_symbol();
	void data_frame();
	float point(int c, int e) const;
};

} // namespace dsp

#endif
