/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    codec_sim.h - a mobile network's speech codec on an 8000 Hz line:
 *                  AMR (3GPP TS 26.090, the codec of GSM and VoLTE
 *                  narrowband calls) and AMR-WB (TS 26.190, ITU-T
 *                  G.722.2, VoLTE wideband), each frame of 20 ms encoded
 *                  and decoded, with frame erasures (the decoder's
 *                  concealment); netsim puts it on the line
 *                  (NET_CODEC), codec_probe runs files through it
 *
 *    The codecs are OpenCORE's AMR-NB and AMR-WB decoders and VisualOn's
 *    AMR-WB encoder (Debian: libopencore-amrnb-dev, libopencore-amrwb-dev,
 *    libvo-amrwbenc-dev).  Without them (DMODEM_HAVE_AMR undefined)
 *    init() fails with a message.
 *
 */
#ifndef DMODEM_TEST_CODEC_SIM_H
#define DMODEM_TEST_CODEC_SIM_H

#include <stdint.h>

namespace dsp {

/* 8000 Hz samples in, one at a time, the codec's out, one frame (20 ms)
   later: zeros for the first frame.  AMR-WB may take its own 16000 Hz
   samples instead (rate 16000: no resampling, 320 a frame). */
class SpeechCodec {
public:
	enum { FRAME = 160 };   /* samples at 8000 Hz */

	SpeechCodec() = default;
	~SpeechCodec() { close(); }
	SpeechCodec(const SpeechCodec &) = delete;
	SpeechCodec &operator=(const SpeechCodec &) = delete;

	/* spec: "amr:<kbit/s>[:dtx]" (4.75 5.15 5.9 6.7 7.4 7.95 10.2
	   12.2) or "amrwb:<kbit/s>[:dtx]" (6.6 8.85 12.65 14.25 15.85 18.25
	   19.85 23.05 23.85); loss: the frames erased, percent, at random
	   (seed); rate: the samples', 8000, or 16000 for AMR-WB as it
	   is; 0, or -1 with the reason in *err */
	int init(const char *spec, double loss, unsigned seed,
		 const char **err, int rate = 8000);
	void close();
	bool on() const { return state_ != nullptr; }
	const char *name() const { return name_; }

	/* one sample in (at the rate of init()), the codec's one out
	   (int, 16 bits) */
	int sample(int x);

	/* the frames encoded so far, and those erased */
	long frames() const { return frames_; }
	long erased() const { return erased_; }

private:
	/* a direction's 2:1 FIR for AMR-WB's 16000 Hz: y = up(x) (two
	   out) or down (one out per two in) */
	struct Fir {
		enum { TAPS = 63 };
		float hist[TAPS] = {};
		int pos = 0;
		float run(float x);     /* x in, the filter's output */
	};
	static const float taps_[Fir::TAPS];

	void frame();           /* in_[] encoded and decoded into out_[] */
	bool erase();           /* this frame is lost */

	void *state_ = nullptr;         /* the encoder */
	void *dec_ = nullptr;           /* the decoder */
	bool wb_ = false;
	bool wide_io_ = false;          /* 16000 Hz samples in and out */
	int mode_ = 0;
	int dtx_ = 0;
	double loss_ = 0;
	uint32_t rnd_ = 1;
	char name_[32] = "";
	short in_[2 * FRAME], out_[2 * FRAME];
	int frame_ = FRAME;             /* samples a frame at the rate */
	int nin_ = 0, nout_ = FRAME;    /* nout_ == frame_: out_ spent */
	bool primed_ = false;           /* a frame has been decoded */
	Fir up_, down_;
	long frames_ = 0, erased_ = 0;
};

} // namespace dsp

#endif /* DMODEM_TEST_CODEC_SIM_H */
