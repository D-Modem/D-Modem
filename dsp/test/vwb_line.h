/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vwb_line.h - a bare pair between two wideband modems (V.wb,
 *                 vwb.h) for the tests: the cable's loss rising with
 *                 frequency (k sqrt(f) dB a km, with its phase, and its
 *                 delay), the two line transformers' high pass, white
 *                 noise, narrowband interferers, each side's own signal
 *                 leaking into its receiver through the hybrid, the
 *                 answerer's sample clock apart from the caller's (its
 *                 samples resampled to and from the line's), and a
 *                 dropout (vwb_line.cpp)
 *
 *    The line's time is the caller's clock.  Each way, and each echo,
 *    goes through the same latency (5.3 ms and a little: the two sides'
 *    converters and buffers); a block of either side's samples is no
 *    longer than 5 ms.
 */
#ifndef DMODEM_VWB_LINE_H
#define DMODEM_VWB_LINE_H

#include <random>
#include <vector>

#include "dm_types.h"
#include "vwb.h"

struct VwbLineConfig {
	s32 fs = 96000;
	double km = 0.5;               /* the pair */
	double k_db = 0.025;           /* its loss: dB a km a sqrt(Hz) (24
					  AWG: about 8 dB/km at 100 kHz) */
	double snr_db = 60;            /* white noise below a tone of the
					  transmitter's (as sent), per tone */
	double echo_db = -15;          /* the hybrid's leak of each side's
					  own signal */
	double ppm = 100;              /* the answerer's clock fast by */
	double hpf_hz = 2700;          /* each transformer's corner */
	struct Tone {
		double hz, db;         /* dB to the transmitter's power */
	};
	std::vector<Tone> tones;       /* interferers at both receivers */
	double drop_at = 0, drop_s = 0;  /* the pair cut from drop_at for
					    drop_s (s) */
	unsigned seed = 1;
};

class VwbLine {
public:
	explicit VwbLine(const VwbLineConfig &c);
	/* the samples each side gets for its next block: the caller's n
	   (the line's), the answerer's however many its clock has meanwhile
	   (a_in's size) */
	void inputs(s32 n, std::vector<s16> &c_in, std::vector<s16> &a_in);
	/* what they sent for those blocks */
	void outputs(const std::vector<s16> &c_out,
		     const std::vector<s16> &a_out);
	/* the line's samples so far */
	s64 time() const { return t_; }
	/* the cable's loss at f, dB */
	double loss_db(double f) const;

private:
	float noise();
	float tones(double t) const;
	bool cut(double t) const;
	float fir(const dsp::VwbHistory &x, s64 at) const;

	VwbLineConfig cfg_;
	s64 lat_;                       /* each way's and echo's latency */
	double ratio_;                  /* answerer samples per line sample */
	std::vector<float> h_;          /* the cable and transformers */
	dsp::VwbHistory c_hist_, a_hist_;   /* each side's, by its index */
	dsp::VwbHistory a_line_;        /* the answerer's on the line's clock */
	dsp::VwbHistory ca_, ac_;       /* through the cable, line time */
	s64 t_ = 0, a_t_ = 0;           /* the line's and the answerer's
					   samples given out */
	float sigma_ = 0;
	std::mt19937 rng_;
	std::normal_distribution<float> gauss_;
	std::vector<double> tone_amp_, tone_ph_;
	dsp::VwbInterp interp_;
};

#endif
