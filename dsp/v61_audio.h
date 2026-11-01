/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v61_audio.h - V.61's audio signal elements (5.5/V.61): the voice
 *                  path's speech (8000 samples a second) to complex
 *                  samples at the symbol rate and back
 *
 *    The transmitter's audio is moved down by the audio carrier w
 *    (1600 Hz at 3000 symbols/s, 1493.3 Hz at 2800): inphase x cos(wt),
 *    quadrature x sin(wt), low pass filtered to half the symbol rate
 *    (Table 4) and taken at each symbol (5.5.2.2).  The note to 5.5.1.2
 *    allows any equivalent method: here the speech is mixed at its own
 *    8000 Hz and the filter, a Kaiser windowed sinc, is evaluated at the
 *    symbol instants directly (3 or 7 phases of it).  The pre-emphasis
 *    filter is the fixed flat one, index 20 (Tables 2 and 3: 1, 0, 0).
 *
 *    The receiver does the reverse: the symbols' audio interpolated to
 *    8000 Hz (an interpolating low pass at the symbol rate) and moved
 *    back up, x = 2 Re(a exp(-jwt)).  Both ends count time from the
 *    first symbol after E, so the two mixers keep the same phase.
 *
 *    Audio is scaled so that a full scale sine (32767) gives a complex
 *    sample of magnitude 2 (1 for 16384).
 */

#ifndef DSP_V61_AUDIO_H
#define DSP_V61_AUDIO_H

#include <vector>

#include "dm_types.h"
#include "dsp.h"

namespace dsp {

/* the speech to symbols */
class V61AudioTx {
public:
	/* at baud symbols/s (3000 or 2800); the history cleared */
	void init(s32 baud);
	/* the audio sample of the next symbol from the speech samples
	   push()ed so far (zeros where there are none yet) */
	cplx next();
	/* one 8000 Hz sample of speech */
	void push(s16 x);
	/* the samples next() still needs ahead of the next symbol */
	s32 wanted() const;

private:
	static constexpr s32 K = 88;    /* filter half length, samples */
	s32 den_ = 30;                  /* baud / 100 */
	std::vector<float> h_;          /* den_ phases of 2K+1 taps */
	std::vector<cplx> hist_;        /* mixed samples by number, ring */
	s64 nin_ = 0;                   /* samples pushed */
	s64 pos_ = 0;                   /* next symbol's position, 1/den_
					   samples */
	double w_ = 0;                  /* audio carrier, cycles a sample */
};

/* the symbols back to speech */
class V61AudioRx {
public:
	/* at baud symbols/s; the history cleared */
	void init(s32 baud);
	/* one symbol's audio sample in; speech samples (8000 Hz) into out
	   (room for 4), how many */
	s16 push(cplx a, s16 *out);

private:
	static constexpr s32 K = 16;    /* filter half length, symbols */
	s32 den_ = 30;
	std::vector<float> g_;          /* 80 phases of 2K+1 taps */
	std::vector<cplx> hist_;        /* symbols by number, ring */
	s64 nsym_ = 0;                  /* symbols pushed */
	s64 out_ = 0;                   /* next output sample's number */
	double w_ = 0;
};

} // namespace dsp

#endif
