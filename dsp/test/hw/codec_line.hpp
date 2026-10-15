/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/codec_line.hpp - the line and a codec at a rate of its own (as
 *                        the modem's driver sets it): a mixin of a
 *                        harness
 *
 *    A windowed sinc resampler each way, and the codec's input noise:
 *    a line is never digitally silent, and the SM56 modem's answering
 *    automode needs it not to be.  Its rms is 5 by default (about -76 dB
 *    of full scale), <PREFIX>_NOISE else.
 *
 *    The harness (Host, a half::HalfModem) calls codec_line_sample() for
 *    each line sample, with the line's rate over the codec's and what
 *    a codec sample does (its ADC's in, its DAC's out).
 *
 */

#ifndef HW_CODEC_LINE_HPP
#define HW_CODEC_LINE_HPP

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class CodecLine {
public:
	/* one line sample: in (and the noise) resampled to the codec's
	   samples due within it, each one's ADC sample through codec(adc),
	   which gives the DAC's back; the line's sample out.  r is the
	   line's rate over the codec's */
	template <class F> short codec_line_sample(short in, double r, F &&codec)
	{
		if (noise_rms_ < 0) {
			const char *s = host().env("NOISE");
			noise_rms_ = s ? (float)atof(s) : 5;
		}
		push(up_, in + noise());
		while (up_ph_ < 1) {
			short dac = codec(sat(at(up_, up_ph_, r > 1 ? 1 / r : 1)));
			push(down_, dac);
			up_ph_ += r;
		}
		up_ph_ -= 1;
		/* (the newest codec sample was at up_ph_ - r) */
		return sat(at(down_, 1 - up_ph_ / r, r < 1 ? r : 1));
	}

private:
	enum { TAPS = 32 };
	float up_[TAPS] = {}, down_[TAPS] = {};  /* the newest last */
	double up_ph_ = 0;      /* the next codec sample's time after the
				   line's sample before the newest, in line
				   samples */
	float noise_rms_ = -1;  /* (not read yet) */
	u32 seed_ = 1;

	Host &host() { return static_cast<Host &>(*this); }

	float noise()
	{
		seed_ = seed_ * 1664525u + 1013904223u; /* (the same every run) */
		return noise_rms_ * 3.4641f * ((float)(seed_ >> 8) / (1 << 24) - 0.5f);
	}

	/* x rounded and clipped to 16 bits */
	static short sat(double x)
	{
		return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrint(x));
	}

	/* the signal of h at f (0..1 or a little over) after its sample
	   TAPS / 2 - 1, lowpass at cut (<= 1) of its Nyquist */
	static float at(const float *h, double f, double cut)
	{
		double y = 0;
		for (int k = 0; k < TAPS; k++) {
			double t = k - (TAPS / 2 - 1) - f, x = M_PI * t * cut;
			double w = fabs(t) < TAPS / 2 ?
				0.5 + 0.5 * cos(M_PI * t / (TAPS / 2)) : 0;
			y += h[k] * cut * (fabs(x) < 1e-9 ? 1 : sin(x) / x) * w;
		}
		return (float)y;
	}

	/* x in as the newest */
	static void push(float *h, float x)
	{
		memmove(h, h + 1, (TAPS - 1) * sizeof(float));
		h[TAPS - 1] = x;
	}
};

}

#endif
