/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vcvm_line.h - the line V.cvm's tests stream through: a delay, a
 *                  gain, a clock difference and a codec of codec_sim
 *                  (where the AMR libraries are)
 *
 */

#ifndef DSP_TEST_VCVM_LINE_H
#define DSP_TEST_VCVM_LINE_H

#include <math.h>

#include <deque>
#include <vector>

#include <gtest/gtest.h>

#ifdef DMODEM_HAVE_AMR
#include "codec_sim.h"
#endif

namespace vcvm_test {

#ifdef DMODEM_HAVE_AMR
using dsp::SpeechCodec;
#endif

/* whether the speech codecs are built in (DMODEM_HAVE_AMR) */
inline bool have_codec()
{
#ifdef DMODEM_HAVE_AMR
	return true;
#else
	return false;
#endif
}

/* the samples a path adds to its delay, a margin for the clock
   difference (12 ms) */
const int PATH_MARGIN = 96;

/* a direction of the line, streaming: a delay (and 12 ms), a gain, the
   far clock ppm off (windowed sinc), a codec */
class Path {
public:
	/* a path at fs Hz: delay samples of delay, gain, the far clock ppm off,
	   codec (a codec_sim.h spec, "" or null: none) */
	Path(int fs, int delay, double gain, double ppm, const char *codec)
		: fs_(fs), gain_(gain), r_(1 + ppm * 1e-6)
	{
		/* the delay on the far side of the resampler, and a margin
		   for the clock difference on this side (10 ms more) */
		in_.assign((size_t)delay, 0.0f);
		out_.assign(PATH_MARGIN, 0.0f);
#ifdef DMODEM_HAVE_AMR
		if (codec && *codec) {
			const char *err = nullptr;
			EXPECT_EQ(codec_.init(codec, 0, 1, &err, fs), 0)
				<< (err ? err : "");
		}
#else
		(void)codec;
#endif
	}

	/* n samples x in: resampled to the far clock, through the gain and the
	   codec, onto those to pull() */
	void push(const float *x, int n)
	{
		in_.insert(in_.end(), x, x + n);
		const int H = 16;
		long end = base_ + (long)in_.size();
		while ((long)floor(p_) + H < end) {
			long i = (long)floor(p_);
			double f = p_ - i, v = 0;
			for (int k = -H + 1; k <= H; k++) {
				long j = i + k;
				if (j < base_)
					continue;
				double t = k - f;
				double s = fabs(t) < 1e-9 ? 1 : sin(M_PI * t) / (M_PI * t);
				v += in_[(size_t)(j - base_)] * s *
					(0.5 + 0.5 * cos(M_PI * t / H));
			}
			out_.push_back(shape(v * gain_));
			p_ += r_;
		}
		long keep = (long)floor(p_) - 2 * H;
		if (keep - base_ > 4096) {
			in_.erase(in_.begin(), in_.begin() + (keep - base_));
			base_ = keep;
		}
	}

	/* n samples out: zeros when there are none yet */
	void pull(float *y, int n)
	{
		for (int i = 0; i < n; i++) {
			if (out_.empty())
				y[i] = 0;
			else {
				y[i] = out_.front();
				out_.pop_front();
			}
		}
	}

private:
	/* the resampler's sample v through the codec (clipped to 16 bits), if
	   there is one; else v as it is */
	float shape(double v)
	{
#ifdef DMODEM_HAVE_AMR
		if (codec_.on()) {
			int s = (int)lrint(v);
			s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
			return (float)codec_.sample(s);
		}
#endif
		return (float)v;
	}

	[[maybe_unused]] int fs_;      /* (the rate it was made for) */
	double gain_, r_, p_ = 0;
	std::vector<float> in_;
	long base_ = 0;
	std::deque<float> out_;
#ifdef DMODEM_HAVE_AMR
	SpeechCodec codec_;
#endif
};

/* a signal through a path, whole */
inline std::vector<float> through(const std::vector<float> &x, int fs, int delay,
			   double gain, double ppm, const char *codec)
{
	Path p(fs, delay, gain, ppm, codec);
	std::vector<float> xx(x);
	xx.insert(xx.end(), (size_t)fs / 2, 0.0f);
	p.push(xx.data(), (int)xx.size());
	std::vector<float> y(xx.size());
	p.pull(y.data(), (int)y.size());
	return y;
}

} // namespace vcvm_test

#endif
