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
 *    Phase 3 so far (9.5.2/V.92): Ru, Ru-bar, TRN1u, Ja until it is
 *    asked to stop, then silence; Su, Su-bar of 24.5 T, Su until Jp is
 *    in, Su-bar of 24 T and Jp's fraction, the second TRN1u (the data
 *    frames count from its first symbol) until CPt is ready, CPt until
 *    Ri, then E1u.
 *
 */

#ifndef V92A_H
#define V92A_H

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
	V92A_E1U,               /* a data frame of zeros */
};

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
	int next;               /* E1u is made: what comes next is the
				   caller's */
	double next_at;         /* the time of its last symbol */

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
};

} // namespace dsp

#endif
