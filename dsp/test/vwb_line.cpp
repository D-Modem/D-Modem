/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vwb_line.cpp - the simulated bare pair of vwb_line.h
 *
 */

#include <algorithm>
#include <cmath>
#include <complex>

#include "vwb_line.h"

namespace {

const s32 TAPS = 256;
const double TX_LEVEL = 5000;  /* the modem's full-band RMS (vwb.cpp) */

/* a ring of samples by their index, as dsp::VwbHistory */
dsp::VwbHistory ring()
{
	dsp::VwbHistory h;
	h.init(1 << 15);
	return h;
}

} // namespace

/* the cable's (and transformers') impulse response from its frequency
   response, the noise's level, the interferers' phases */
VwbLine::VwbLine(const VwbLineConfig &c)
	: cfg_(c), lat_(512 * c.fs / 96000 + 64), ratio_(1 + c.ppm * 1e-6),
	  rng_(c.seed), gauss_(0, 1)
{
	const s32 m = 8192;
	dsp::VwbFft fft;
	fft.init(m);
	std::vector<std::complex<float>> hf(m);
	double cc = c.k_db * c.km * std::log(10.0) / 20;
	double delay = c.km * 5e-6;
	for (s32 i = 0; i <= m / 2; i++) {
		double f = static_cast<double>(i) * c.fs / m;
		std::complex<double> j(0, 1);
		std::complex<double> h = std::exp(-cc * std::sqrt(f) *
						  std::complex<double>(1, 1));
		std::complex<double> hp = f > 0 ? j * f / (j * f + c.hpf_hz) :
			std::complex<double>(0, 0);
		h *= hp * hp * std::exp(-j * (2 * M_PI * f * delay));
		hf[static_cast<size_t>(i)] = std::complex<float>(h);
		if (i && i < m / 2)
			hf[static_cast<size_t>(m - i)] = std::conj(
				std::complex<float>(h));
	}
	fft.inverse(hf.data());
	h_.resize(TAPS);
	for (s32 i = 0; i < TAPS; i++) {
		double w = i < TAPS - 64 ? 1 :
			0.5 + 0.5 * std::cos(M_PI * (i - (TAPS - 64)) / 64.0);
		h_[static_cast<size_t>(i)] =
			static_cast<float>(hf[static_cast<size_t>(i)].real() / m * w);
	}
	s32 n = 512 * c.fs / 96000;
	s32 tones = std::min<s32>(193, n * 2 / 5 + 1) - 21;
	double scale2 = TX_LEVEL * TX_LEVEL / (2.0 * tones);
	sigma_ = static_cast<float>(std::sqrt(scale2 * n *
					      std::pow(10, -c.snr_db / 10)));
	std::uniform_real_distribution<double> ph(0, 2 * M_PI);
	for (const auto &t : c.tones) {
		tone_amp_.push_back(std::sqrt(2 * TX_LEVEL * TX_LEVEL *
					      std::pow(10, t.db / 10)));
		tone_ph_.push_back(ph(rng_));
	}
	c_hist_ = ring();
	a_hist_ = ring();
	a_line_ = ring();
	ca_ = ring();
	ac_ = ring();
}

/* see vwb_line.h */
double VwbLine::loss_db(double f) const
{
	return cfg_.k_db * cfg_.km * std::sqrt(f);
}

/* a sample of the white noise */
float VwbLine::noise()
{
	return sigma_ * gauss_(rng_);
}

/* the interferers at line time t (s) */
float VwbLine::tones(double t) const
{
	double y = 0;
	for (size_t i = 0; i < tone_amp_.size(); i++)
		y += tone_amp_[i] * std::cos(2 * M_PI * cfg_.tones[i].hz * t +
					     tone_ph_[i]);
	return static_cast<float>(y);
}

/* whether the pair is cut at line time t (samples) */
bool VwbLine::cut(double t) const
{
	double s = t / cfg_.fs;
	return cfg_.drop_s > 0 && s >= cfg_.drop_at &&
		s < cfg_.drop_at + cfg_.drop_s;
}

/* the cable's FIR on x at sample at */
float VwbLine::fir(const dsp::VwbHistory &x, s64 at) const
{
	float y = 0;
	for (s32 i = 0; i < TAPS; i++)
		y += h_[static_cast<size_t>(i)] * x(at - i);
	return y;
}

/* see vwb_line.h */
void VwbLine::inputs(s32 n, std::vector<s16> &c_in, std::vector<s16> &a_in)
{
	const float e = static_cast<float>(std::pow(10, cfg_.echo_db / 20));
	auto clip = [](float v) {
		return static_cast<s16>(std::lround(std::min(std::max(v,
			-32767.0f), 32767.0f)));
	};
	c_in.resize(static_cast<size_t>(n));
	for (s32 i = 0; i < n; i++) {
		s64 t = t_ + i;
		float far = cut(static_cast<double>(t)) ? 0 : ac_(t - lat_);
		float v = far + e * c_hist_(t - lat_) + noise() +
			tones(static_cast<double>(t) / cfg_.fs);
		c_in[static_cast<size_t>(i)] = clip(v);
	}
	s64 a_end = static_cast<s64>(std::floor((t_ + n) * ratio_));
	a_in.resize(static_cast<size_t>(a_end - a_t_));
	auto cah = [this](s64 i) { return ca_(i); };
	for (s64 j = a_t_; j < a_end; j++) {
		double tl = static_cast<double>(j) / ratio_;
		float far = cut(tl) ? 0 : interp_.at(cah, tl - lat_);
		float v = far + e * a_hist_(j - lat_) + noise() +
			tones(tl / cfg_.fs);
		a_in[static_cast<size_t>(j - a_t_)] = clip(v);
	}
	t_ += n;
	a_t_ = a_end;
}

/* see vwb_line.h */
void VwbLine::outputs(const std::vector<s16> &c_out,
		      const std::vector<s16> &a_out)
{
	for (s16 v : c_out) {
		c_hist_.push(v);
		ca_.push(fir(c_hist_, c_hist_.end() - 1));
	}
	for (s16 v : a_out)
		a_hist_.push(v);
	/* the answerer's samples on the line's clock, as far as they go */
	auto ah = [this](s64 i) { return a_hist_(i); };
	for (;;) {
		s64 t = a_line_.end();
		double j = static_cast<double>(t) * ratio_;
		if (std::floor(j) + dsp::VwbInterp::HALF >=
		    static_cast<double>(a_hist_.end()))
			break;
		a_line_.push(interp_.at(ah, j));
		ac_.push(fir(a_line_, a_line_.end() - 1));
	}
}
