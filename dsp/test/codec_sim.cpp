/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    codec_sim.cpp - a mobile network's speech codec on an 8000 Hz line
 *                    (codec_sim.h): AMR and AMR-WB through OpenCORE's and
 *                    VisualOn's libraries, frame by frame
 *
 */
#include "codec_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DMODEM_HAVE_AMR
#include <opencore-amrnb/interf_dec.h>
#include <opencore-amrnb/interf_enc.h>
#include <opencore-amrwb/dec_if.h>
#include <vo-amrwbenc/enc_if.h>
#endif

namespace dsp {

/* the half band low pass of the 2:1 resampling at 16000 Hz: 63 taps,
   -1 dB at 3400 Hz, -6 dB at 3700 Hz, -20 dB at 4000 Hz (windowed
   sinc, Blackman) */
const float SpeechCodec::taps_[Fir::TAPS] = {
	-1.24328108e-19f, -3.76694278e-06f, -3.96326110e-05f, 1.52787582e-05f,
	1.84222322e-04f, 2.41517394e-05f, -4.67072405e-04f, -2.16131828e-04f,
	8.98182989e-04f, 7.09825047e-04f, -1.43182437e-03f, -1.69747297e-03f,
	1.92547364e-03f, 3.38839429e-03f, -2.10049389e-03f, -5.96159679e-03f,
	1.51411327e-03f, 9.50580721e-03f, 4.56974329e-04f, -1.39628505e-02f,
	-4.64391907e-03f, 1.90930342e-02f, 1.22006326e-02f, -2.44779532e-02f,
	-2.50482004e-02f, 2.95673138e-02f, 4.76124197e-02f, -3.37643304e-02f,
	-9.58345957e-02f, 3.65325929e-02f, 3.14772914e-01f, 4.62497020e-01f,
	3.14772914e-01f, 3.65325929e-02f, -9.58345957e-02f, -3.37643304e-02f,
	4.76124197e-02f, 2.95673138e-02f, -2.50482004e-02f, -2.44779532e-02f,
	1.22006326e-02f, 1.90930342e-02f, -4.64391907e-03f, -1.39628505e-02f,
	4.56974329e-04f, 9.50580721e-03f, 1.51411327e-03f, -5.96159679e-03f,
	-2.10049389e-03f, 3.38839429e-03f, 1.92547364e-03f, -1.69747297e-03f,
	-1.43182437e-03f, 7.09825047e-04f, 8.98182989e-04f, -2.16131828e-04f,
	-4.67072405e-04f, 2.41517394e-05f, 1.84222322e-04f, 1.52787582e-05f,
	-3.96326110e-05f, -3.76694278e-06f, -1.24328108e-19f,
};

/* x into the history (a ring), the filter's output */
float SpeechCodec::Fir::run(float x)
{
	float y = 0;
	int i, k;
	hist[pos] = x;
	for (i = 0, k = pos; i < TAPS; i++) {
		y += taps_[i] * hist[k];
		if (--k < 0)
			k = TAPS - 1;
	}
	if (++pos == TAPS)
		pos = 0;
	return y;
}

/* the modes of each codec, kbit/s */
static const struct {
	const char *name;
	float rates[9];
	int n;
} codecs[] = {
	{ "amr", { 4.75f, 5.15f, 5.9f, 6.7f, 7.4f, 7.95f, 10.2f, 12.2f }, 8 },
	{ "amrwb", { 6.6f, 8.85f, 12.65f, 14.25f, 15.85f, 18.25f, 19.85f,
		     23.05f, 23.85f }, 9 },
};

/* see codec_sim.h */
int SpeechCodec::init(const char *spec, double loss, unsigned seed,
		      const char **err, int rate)
{
	char name[16];
	const char *p = strchr(spec, ':');
	int c, m = -1;

	close();
	*err = nullptr;
	if (!p || (size_t)(p - spec) >= sizeof(name)) {
		*err = "NET_CODEC is <codec>:<kbit/s>[:dtx]";
		return -1;
	}
	memcpy(name, spec, p - spec);
	name[p - spec] = 0;
	for (c = 0; c < 2; c++)
		if (!strcmp(name, codecs[c].name))
			break;
	if (c == 2) {
		*err = "codecs: amr, amrwb";
		return -1;
	}
	float kbps = (float)atof(p + 1);
	for (int i = 0; i < codecs[c].n; i++)
		if (kbps > codecs[c].rates[i] - 0.01f &&
		    kbps < codecs[c].rates[i] + 0.01f)
			m = i;
	if (m < 0) {
		*err = c ? "AMR-WB: 6.6 8.85 12.65 14.25 15.85 18.25 19.85 "
			   "23.05 23.85 kbit/s" :
			   "AMR: 4.75 5.15 5.9 6.7 7.4 7.95 10.2 12.2 kbit/s";
		return -1;
	}
	p = strchr(p + 1, ':');
	dtx_ = p && !strcmp(p + 1, "dtx");
	if (p && !dtx_) {
		*err = "after the rate only :dtx";
		return -1;
	}
	wb_ = c == 1;
	if (rate != 8000 && !(rate == 16000 && wb_)) {
		*err = "the rate is 8000, or 16000 for AMR-WB";
		return -1;
	}
	wide_io_ = rate == 16000;
	frame_ = wide_io_ ? 2 * FRAME : FRAME;
	mode_ = m;
	loss_ = loss / 100;
	rnd_ = seed ? seed : 1;
	snprintf(name_, sizeof(name_), "%s %g kbit/s%s", wb_ ? "AMR-WB" :
		 "AMR", (double)codecs[c].rates[m], dtx_ ? " DTX" : "");
#ifdef DMODEM_HAVE_AMR
	if (wb_) {
		state_ = E_IF_init();
		dec_ = D_IF_init();
	} else {
		state_ = Encoder_Interface_init(dtx_);
		dec_ = Decoder_Interface_init();
	}
	if (!state_ || !dec_) {
		close();
		*err = "the codec did not start";
		return -1;
	}
	up_ = Fir();
	down_ = Fir();
	nin_ = 0;
	nout_ = frame_;
	primed_ = false;
	frames_ = erased_ = 0;
	return 0;
#else
	*err = "not built with the AMR libraries (opencore-amrnb, "
	       "opencore-amrwb, vo-amrwbenc)";
	return -1;
#endif
}

/* the encoder and the decoder freed (none until the next init()) */
void SpeechCodec::close()
{
#ifdef DMODEM_HAVE_AMR
	if (state_) {
		if (wb_)
			E_IF_exit(state_);
		else
			Encoder_Interface_exit(state_);
	}
	if (dec_) {
		if (wb_)
			D_IF_exit(dec_);
		else
			Decoder_Interface_exit(dec_);
	}
#endif
	state_ = dec_ = nullptr;
}

/* a frame is lost with probability loss_ (a 32 bit LCG) */
bool SpeechCodec::erase()
{
	if (loss_ <= 0)
		return false;
	rnd_ = rnd_ * 1664525u + 1013904223u;
	return (rnd_ >> 8) / 16777216.0 < loss_;
}

/* in_[] through the encoder and the decoder into out_[]: at 8000 Hz as
   it is, or at 16000 Hz for AMR-WB, up and down through the half band
   filter */
void SpeechCodec::frame()
{
#ifdef DMODEM_HAVE_AMR
	unsigned char bits[64];
	int bfi = erase();
	frames_++;
	erased_ += bfi;
	if (!wb_) {
		Encoder_Interface_Encode(state_, (enum Mode)mode_, in_, bits,
					 0);
		Decoder_Interface_Decode(dec_, bits, out_, bfi);
		return;
	}
	short wide[2 * FRAME], synth[2 * FRAME];
	int i;
	if (wide_io_) {
		E_IF_encode(state_, mode_, in_, bits, dtx_);
		D_IF_decode(dec_, bits, out_, bfi);
		return;
	}
	for (i = 0; i < FRAME; i++) {
		/* zero stuffing, the filter at twice the gain */
		float a = up_.run(2.0f * in_[i]), b = up_.run(0);
		wide[2 * i] = (short)(a > 32767 ? 32767 : a < -32768 ? -32768 :
				      a);
		wide[2 * i + 1] = (short)(b > 32767 ? 32767 : b < -32768 ?
					  -32768 : b);
	}
	E_IF_encode(state_, mode_, wide, bits, dtx_);
	D_IF_decode(dec_, bits, synth, bfi);
	for (i = 0; i < FRAME; i++) {
		float y = down_.run(synth[2 * i]);
		down_.run(synth[2 * i + 1]);
		out_[i] = (short)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
	}
#endif
}

/* see codec_sim.h */
int SpeechCodec::sample(int x)
{
	int y = 0;
	in_[nin_++] = (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
	if (primed_ && nout_ < frame_)
		y = out_[nout_++];
	if (nin_ == frame_) {
		frame();
		nin_ = 0;
		nout_ = 0;
		primed_ = true;
	}
	return y;
}

} // namespace dsp
