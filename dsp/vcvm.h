/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm.h - the signal set of V.cvm, data over mobile speech paths
 *             (docs/v_cvm.pdf, clause 10)
 *
 *    The comb: tones k * spacing (Table 10-2's N200 ... S400) at the
 *    signal interface's 8000, 16000 or 32000 Hz, each symbol a useful
 *    part of whole cycles of the spacing after a cyclic prefix of its
 *    end; per loaded tone a differential phase (DQPSK or 8-DPSK, Gray
 *    coded) and, with the amplitude loading of 10.11, a ring step of 2
 *    rings 6 dB apart or 4 rings 3 dB apart.  The receiver takes the
 *    symbol timing from the cyclic prefix, follows the clock difference
 *    from the phase errors' slope across the tones, and decides each
 *    tone from two consecutive symbols: no reference, no equalizer.
 *
 *    The parametric mode (10.6): a pulse train whose period, one of 16
 *    lags, is the symbol (P20: 20 ms, lags 40 to 70 samples at 8000 Hz;
 *    P10: 10 ms, 20 to 35), which an ACELP codec keeps as its long-term
 *    predictor's lag.  The receiver takes the timing from the preamble
 *    (8 symbols alternating the lowest and highest lag) and decides each
 *    symbol by normalized autocorrelation; c-bar, its mean over 2 s, is
 *    clause 9.5's measure for the retrains.
 *
 *    These are the modulations alone; the start-up, the framing and the
 *    pump are built on them.
 *
 */

#ifndef DSP_VCVM_H
#define DSP_VCVM_H

#include <complex>
#include <vector>

#include "dm_types.h"

namespace dsp {

#define VCVM_TONES      68      /* the most a comb has (S200) */

/* a comb of Table 10-2 at a signal interface rate */
struct VcvmComb {
	s16 code;               /* 0 N200 ... 8 S400 */
	s32 fs;                 /* the interface's rate: 8000, 16000, 32000 */
	s16 spacing;            /* Hz */
	s16 kmin, kmax;         /* the tones' multiples of the spacing */
	s16 tones;              /* kmax - kmin + 1 */
	bool sym22;             /* the 22.5 ms symbol (the 200 Hz combs) */
	s16 tu, tg;             /* useful part and prefix, samples */
	s16 sps;                /* a symbol, samples */

	/* code (0-8) at fs (the band of the code: N at 8000, W at 16000,
	   S at 32000), sym22 the 22.5 ms symbol (codes 0, 3, 6); 0, or -1
	   if they don't go
	   together */
	s16 init(s16 code, s32 fs, bool sym22 = false);
	/* tone i's frequency, Hz */
	double freq(s16 i) const
	{
		return static_cast<double>(kmin + i) * spacing;
	}
	/* the symbol period in ms */
	double ms() const { return 1000.0 * sps / fs; }
};

/* a loading: per tone (the lowest first) the phase bits (0 not loaded,
   2 DQPSK, 3 8-DPSK) and the ring bits (0 none, 1 two rings, 2 four) */
struct VcvmLoad {
	u8 phase[VCVM_TONES];
	u8 ring[VCVM_TONES];

	/* every tone of n at phase bits pb, no rings */
	void all(s16 n, s16 pb);
	s16 loaded(s16 n) const;                /* tones loaded */
	s16 bits(s16 n) const;                  /* bits a symbol */
};

/* the comb's transmitter: one symbol at a time, sps() samples (floats,
   16 bit scale) */
class VcvmCombTx {
public:
	/* the comb, the level over the loaded tones (dBFS rms, 10.11's
	   -12) */
	s16 init(s16 code, s32 fs, bool sym22 = false,
		 double dbfs = -12);
	void set_load(const VcvmLoad &l);
	/* the comb */
	const VcvmComb &comb() const { return c_; }
	/* a symbol, samples */
	s16 sps() const { return c_.sps; }
	/* the bits a data symbol takes */
	s16 bits() const { return load_.bits(c_.tones); }

	/* the phase reference (10.5): the tones as they are, the rings at
	   0 */
	void reference(float *out);
	/* a data symbol from bits() bits (0/1, the first the lowest tone's
	   first phase bit), Gray coded onto the phase and ring steps */
	void data(const u8 *bits, float *out);
	/* a symbol of the steps themselves: per tone the phase step in
	   eighths of a turn and the ring step (the probing's known steps,
	   the control sequence's half turns) */
	void steps(const u8 *ph8, const u8 *rs, float *out);

private:
	void emit(float *out);

	VcvmComb c_{};
	VcvmLoad load_{};
	double rms_ = 0;                /* the level */
	double amp_[VCVM_TONES] = {};   /* the tone's amplitude now */
	double ph_[VCVM_TONES] = {};    /* its phase now */
	s16 ring_[VCVM_TONES] = {};
	std::vector<float> cos_, sin_;  /* [tone][t] over the useful part */
};

/* one received comb symbol */
struct VcvmCombSym {
	s32 start;                      /* its prefix's first sample */
	s16 nbits;
	u8 bits[VCVM_TONES * 5];        /* the decisions, as data() takes */
	float rel[VCVM_TONES * 5];      /* each bit's reliability, 0..1 */
	float dphi[VCVM_TONES];         /* the phase step, radians, the
					   clock's rotation taken off */
	float ratio_db[VCVM_TONES];     /* the magnitude's step, dB */
	float level[VCVM_TONES];        /* |r|, 16 bit scale */
	u8 ph8[VCVM_TONES];             /* the decided phase step, eighths */
	u8 rs[VCVM_TONES];              /* the decided ring step */
	float perr[VCVM_TONES];         /* the phase error of the decision */
	float tau;                      /* the timing, samples past the
					   window */
	float drift;                    /* samples a symbol */
};

/* the comb's receiver: samples in (push()), symbols out (next()) */
class VcvmCombRx {
public:
	s16 init(s16 code, s32 fs, bool sym22 = false);
	void set_load(const VcvmLoad &l);
	/* the comb */
	const VcvmComb &comb() const { return c_; }

	/* samples from the line (16 bit scale) */
	void push(const float *x, s32 n);
	/* the samples received: the index of the next one pushed */
	s32 received() const
	{
		return base_ + static_cast<s32>(buf_.size());
	}
	/* nothing received, unlocked: the next sample pushed is sample
	   base */
	void restart(s32 base);
	/* the next symbol's prefix (once locked) */
	s32 next_start() const { return next_; }

	/* the symbol timing from the cyclic prefix over 8 symbols from
	   sample `from` on (the first symbol: the one whose prefix starts
	   nearest `first`, the signal's onset; -1 for the first after
	   from): 0, or -1 if the samples aren't in yet */
	s16 acquire(s32 from, s32 first = -1);
	/* the signal's onset: the first sample from `from` at which 5 ms
	   of it reach dbfs (rms); -1 if none yet */
	s32 onset(s32 from, double dbfs = -45) const;
	/* the next symbol (the first after acquire() is the reference,
	   which gives only its levels): true if there was one.  known
	   (optional): the transmitted phase steps in eighths, for the
	   probing, which the timing then follows instead of the
	   decisions */
	bool next(VcvmCombSym *s, const u8 *known = nullptr);
	/* the symbol timing acquired */
	bool locked() const { return locked_; }

private:
	std::complex<double> tone(s32 u, s16 k) const;
	float at(s32 i) const;
	void trim();

	VcvmComb c_{};
	VcvmLoad load_{};
	std::vector<float> buf_;
	s32 base_ = 0;                  /* the absolute index of buf_[0] */
	bool locked_ = false;
	bool have_prev_ = false;
	s32 next_ = 0;                  /* the next symbol's prefix */
	double tau_ = 0, drift_ = 0;    /* timing, samples */
	double rot_ = 0;                /* common rotation a symbol */
	std::complex<double> prev_[VCVM_TONES];
	std::vector<float> cos_, sin_;
};

/* the parametric mode's alphabets (Table 10-3) */
struct VcvmParam {
	s32 fs;                 /* 8000 or 16000 */
	bool p10;               /* the 10 ms symbol */
	s16 sps;                /* the symbol, samples */
	s16 lag[16];            /* samples at fs */

	s16 init(s32 fs, bool p10);
};

/* the parametric transmitter */
class VcvmParamTx {
public:
	/* the alphabet, the level (dBFS rms over a symbol, -15) */
	s16 init(s32 fs, bool p10, double dbfs = -15);
	/* a symbol, samples */
	s16 sps() const { return p_.sps; }
	/* a symbol of index i (0-15) */
	void symbol(s16 i, float *out);
	/* the symbol of 4 bits (the first the most significant), Gray
	   coded onto the index */
	void data(const u8 *bits, float *out);
	/* the preamble: 8 symbols, index 0 and 15 alternately */
	void preamble(float *out);

private:
	float step(float e);

	VcvmParam p_{};
	std::vector<float> h_;          /* the band-pass pulse */
	std::vector<float> fir_;        /* its delay line */
	s16 pos_ = 0;
	float tilt_ = 0;                /* the one-pole's state */
	float gain_[16] = {};
};

/* the parametric receiver */
class VcvmParamRx {
public:
	s16 init(s32 fs, bool p10);
	void push(const float *x, s32 n);
	/* the samples received: the index of the next one pushed */
	s32 received() const
	{
		return base_ + static_cast<s32>(buf_.size());
	}
	/* nothing received, unlocked: the next sample pushed is sample
	   base, the preamble looked for from there */
	void restart(s32 base);

	/* looks for the preamble in what has come: true once found (the
	   next symbol is the first after it) */
	bool search();
	/* the preamble found (or lock_at()) */
	bool locked() const { return locked_; }
	/* locked without the search: the next symbol starts at sample at
	   (the timing known otherwise, as P10's after P20's in L3) */
	void lock_at(s32 at);
	/* the next symbol: its index, the decided lag's normalized
	   autocorrelation and its margin over the next lag's; true if
	   there was one; rms the symbol's level (16 bit scale), second the
	   next best lag's index */
	bool next(s16 *idx, float *c = nullptr, float *margin = nullptr,
		  float *rms = nullptr, s16 *second = nullptr);
	/* the symbol's 4 bits (Gray decoded) */
	static void bits(s16 idx, u8 *b);
	/* c-bar: the decided c's mean over the last 2 s (0 before any) */
	float cbar() const;
	/* the next symbol's first sample (once locked) */
	s32 next_start() const { return next_; }

private:
	float corr(s32 a, s32 b, s16 lag) const;
	s16 decide(s32 a, float *c, float *margin,
		   s16 *second = nullptr) const;
	float at(s32 i) const;
	void trim();

	VcvmParam p_{};
	std::vector<float> buf_;
	s32 base_ = 0;
	bool locked_ = false;
	s32 next_ = 0;
	s32 searched_ = 0;
	std::vector<float> cs_;         /* the last 2 s of c */
	s16 csn_ = 0, csi_ = 0;
	float fit_[3] = {};             /* the timing's fit at -1, 0, +1 */
	s16 fitn_ = 0;
};

/* the Gray code of i, and its inverse */
static inline s16 vcvm_gray(s16 i) { return i ^ (i >> 1); }
/* the inverse of vcvm_gray(): the i whose Gray code is g */
static inline s16 vcvm_ungray(s16 g)
{
	s16 i = 0;
	for (; g; g >>= 1)
		i ^= g;
	return i;
}

} // namespace dsp

#endif /* DSP_VCVM_H */
