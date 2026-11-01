/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v61_audio.cpp - V.61's audio to and from the symbol rate
 *                    (v61_audio.h)
 */

#include <cmath>

#include "v61_audio.h"

namespace dsp {

/* the zeroth order modified Bessel function (Kaiser's window) */
static double bessel_i0(double x)
{
	double s = 1, t = 1;
	for (int k = 1; k < 30; k++) {
		t *= (x / (2 * k)) * (x / (2 * k));
		s += t;
	}
	return s;
}

/* a low pass of cut off fc (of the sample rate) at t samples from its
   centre, Kaiser windowed (beta 5.65, about 60 dB) over +-half */
static double kaiser_sinc(double t, double fc, double half)
{
	const double beta = 5.65;
	double r = t / half;
	if (r <= -1 || r >= 1)
		return 0;
	double x = 2 * M_PI * fc * t;
	double s = t == 0 ? 2 * fc : sin(x) / (M_PI * t);
	return s * bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
}

/* the audio carrier (5.5.1.3) and Table 4's band, by symbol rate */
static double audio_carrier(s32 baud)
{
	return baud == 2800 ? 1493.333 : 1600;
}

/* (see the header) */
void V61AudioTx::init(s32 baud)
{
	den_ = baud / 100;
	w_ = audio_carrier(baud) / 8000;
	/* half the symbol rate less Table 4's margin: A to B */
	const double fc = (baud == 2800 ? 1330.0 : 1425.0) / 8000;
	h_.assign(static_cast<size_t>(den_) * (2 * K + 1), 0);
	for (s32 r = 0; r < den_; r++) {
		double sum = 0;
		for (s32 k = -K; k <= K; k++)
			sum += kaiser_sinc(k, fc, K + 1);
		for (s32 k = -K; k <= K; k++)
			h_[r * (2 * K + 1) + k + K] = static_cast<float>(
				kaiser_sinc(static_cast<double>(r) / den_ - k,
					    fc, K + 1) / sum);
	}
	hist_.assign(1024, cplx_make(0, 0));
	nin_ = 0;
	pos_ = 0;
}

/* (see the header) */
void V61AudioTx::push(s16 x)
{
	double ph = 2 * M_PI * fmod(w_ * static_cast<double>(nin_), 1.0);
	float v = x / 16384.0f;
	hist_[nin_ & 1023] = cplx_make(v * static_cast<float>(cos(ph)),
				       v * static_cast<float>(sin(ph)));
	nin_++;
}

/* (see the header) */
s32 V61AudioTx::wanted() const
{
	s64 c = pos_ / den_;
	s64 need = c + K + 1 - nin_;
	return need > 0 ? static_cast<s32>(need) : 0;
}

/* (see the header) */
cplx V61AudioTx::next()
{
	s64 c = pos_ / den_;
	s32 r = static_cast<s32>(pos_ % den_);
	const float *h = &h_[r * (2 * K + 1)];
	cplx s = cplx_make(0, 0);
	pos_ += 80;
	for (s32 k = -K; k <= K; k++) {
		s64 i = c + k;
		if (i < 0 || i >= nin_ || i < nin_ - 1024)
			continue;
		s.re += h[k + K] * hist_[i & 1023].re;
		s.im += h[k + K] * hist_[i & 1023].im;
	}
	return s;
}

/* (see the header) */
void V61AudioRx::init(s32 baud)
{
	den_ = baud / 100;
	w_ = audio_carrier(baud) / 8000;
	const double fc = 0.5;  /* of the symbol rate */
	g_.assign(80 * (2 * K + 1), 0);
	for (s32 r = 0; r < 80; r++) {
		double sum = 0;
		for (s32 k = -K; k <= K; k++)
			sum += kaiser_sinc(k, fc * 0.95, K + 1);
		for (s32 k = -K; k <= K; k++)
			g_[r * (2 * K + 1) + k + K] = static_cast<float>(
				kaiser_sinc(r / 80.0 - k, fc * 0.95, K + 1) /
				sum);
	}
	hist_.assign(64, cplx_make(0, 0));
	nsym_ = 0;
	out_ = 0;
}

/* (see the header) */
s16 V61AudioRx::push(cplx a, s16 *out)
{
	s16 n = 0;
	hist_[nsym_ & 63] = a;
	nsym_++;
	for (;;) {
		s64 t = out_ * den_;
		s64 c = t / 80;
		s32 r = static_cast<s32>(t % 80);
		if (c + K >= nsym_ || n >= 4)
			break;
		const float *g = &g_[r * (2 * K + 1)];
		cplx s = cplx_make(0, 0);
		for (s32 k = -K; k <= K; k++) {
			s64 i = c + k;
			if (i < 0)
				continue;
			s.re += g[k + K] * hist_[i & 63].re;
			s.im += g[k + K] * hist_[i & 63].im;
		}
		double ph = 2 * M_PI * fmod(w_ * static_cast<double>(out_),
					    1.0);
		double y = 2 * 16384.0 * (s.re * cos(ph) + s.im * sin(ph));
		out[n++] = static_cast<s16>(y > 32767 ? 32767 :
					    y < -32768 ? -32768 : lrint(y));
		out_++;
	}
	return n;
}

} // namespace dsp
