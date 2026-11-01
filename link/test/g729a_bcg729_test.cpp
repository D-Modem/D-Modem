/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a_bcg729_test.cpp - D-Modem's G.729 Annex A (link/g729a.h)
 *                            against bcg729, Belledonne
 *                            Communications' implementation, both
 *                            ways: our encoder to its decoder, its
 *                            encoder to ours, and each side's bits
 *                            and samples beside the other's.  Built
 *                            only where libbcg729 is found; a test
 *                            only (bcg729 is GPL), never part of
 *                            D-Modem's programs
 *
 *    g729a_bcg729_test
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <bcg729/decoder.h>
#include <bcg729/encoder.h>
}

#include "g729a.h"

using namespace dlink;

/* a voice-like test signal: a glottal pulse train whose pitch glides
   through two vowel-like resonances, syllables, pauses and some
   noise */
static std::vector<int16_t> voice(int seconds)
{
	std::vector<int16_t> out;
	double phase = 0, r1 = 0, r1p = 0, r2 = 0, r2p = 0;
	unsigned seed = 12345;
	for (int n = 0; n < 8000 * seconds; n++) {
		const double t = n / 8000.0;
		const double f0 = 110 + 60 * sin(2 * M_PI * 0.7 * t);
		phase += f0 / 8000;
		double x = 0;
		if (phase >= 1) {
			phase -= 1;
			x = 1;
		}
		seed = seed * 1103515245 + 12345;
		x += ((seed >> 16 & 0x7fff) / 32768.0 - 0.5) * 0.05;
		/* two resonators, their frequencies moving */
		const double fa = 700 + 300 * sin(2 * M_PI * 0.9 * t);
		const double fb = 1800 + 600 * sin(2 * M_PI * 0.5 * t);
		const double ra = 0.97, rb = 0.95;
		const double y1 = x + 2 * ra * cos(2 * M_PI * fa / 8000) * r1 -
			ra * ra * r1p;
		r1p = r1;
		r1 = y1;
		const double y2 = y1 + 2 * rb * cos(2 * M_PI * fb / 8000) * r2 -
			rb * rb * r2p;
		r2p = r2;
		r2 = y2;
		/* syllables of about 0.3 s, a pause every second */
		const double env = fmod(t, 1.0) > 0.8 ? 0 :
			0.5 - 0.5 * cos(2 * M_PI * t / 0.3);
		out.push_back(static_cast<int16_t>(
			std::max(-32000.0, std::min(32000.0, 120 * y2 * env))));
	}
	return out;
}

/* the ITU-T vectors' speech, if they are there, else the test signal */
static std::vector<int16_t> input()
{
	const char *e = getenv("DMODEM_G729_VECTORS");
	const std::string file = std::string(e && *e ? e : DMODEM_G729_VECTORS) +
		"/SPEECH.IN";
	std::vector<int16_t> w;
	if (FILE *f = fopen(file.c_str(), "rb")) {
		unsigned char b[2];
		while (fread(b, 1, 2, f) == 2)
			w.push_back(static_cast<int16_t>(b[0] | b[1] << 8));
		fclose(f);
	}
	return w.empty() ? voice(10) : w;
}

/* a and b's signal to difference ratio in dB (a the reference) */
static double sdr(const std::vector<int16_t> &a, const std::vector<int16_t> &b)
{
	double s = 0, d = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); i++) {
		s += double(a[i]) * a[i];
		d += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
	}
	return d == 0 ? 999 : 10 * log10(s / d);
}

/* both encoders on the same speech, each one's frames through both
   decoders */
TEST(G729aBcg729, BothWays)
{
	const std::vector<int16_t> in = input();
	const size_t frames = in.size() / g729::FRAME;

	g729::Encoder our_enc;
	g729::Decoder our_dec_ours, our_dec_theirs;
	bcg729EncoderChannelContextStruct *their_enc =
		initBcg729EncoderChannel(0);
	bcg729DecoderChannelContextStruct *their_dec_ours =
		initBcg729DecoderChannel();
	bcg729DecoderChannelContextStruct *their_dec_theirs =
		initBcg729DecoderChannel();

	size_t same_bits = 0;
	/* [encoder][decoder]: o ours, t theirs */
	std::vector<int16_t> oo, ot, to, tt;
	for (size_t f = 0; f < frames; f++) {
		const int16_t *pcm = &in[f * g729::FRAME];
		uint8_t ours[g729::FRAME_OCTETS], theirs[g729::FRAME_OCTETS];
		uint8_t len = 0;
		our_enc.encode(pcm, ours);
		bcg729Encoder(their_enc, pcm, theirs, &len);
		ASSERT_EQ(len, g729::FRAME_OCTETS);
		bool same = true;
		for (int i = 0; i < g729::FRAME_OCTETS; i++)
			same = same && ours[i] == theirs[i];
		same_bits += same;

		int16_t out[g729::FRAME];
		our_dec_ours.decode(ours, out);
		oo.insert(oo.end(), out, out + g729::FRAME);
		bcg729Decoder(their_dec_ours, ours, g729::FRAME_OCTETS, 0, 0, 0,
			      out);
		ot.insert(ot.end(), out, out + g729::FRAME);
		our_dec_theirs.decode(theirs, out);
		to.insert(to.end(), out, out + g729::FRAME);
		bcg729Decoder(their_dec_theirs, theirs, g729::FRAME_OCTETS, 0, 0,
			      0, out);
		tt.insert(tt.end(), out, out + g729::FRAME);
	}
	closeBcg729EncoderChannel(their_enc);
	closeBcg729DecoderChannel(their_dec_ours);
	closeBcg729DecoderChannel(their_dec_theirs);

	/* bcg729 is not bit exact with the Recommendation (its encoder
	   searches differently, its decoder is not Annex A's), so the
	   bits differ and so do the decoders' samples; what has to hold is
	   that each decodes the other's frames as well as its own */
	const double d_ours = sdr(oo, ot), d_theirs = sdr(tt, to);
	printf("%zu frames, %zu with the same bits; on the same frames the two "
	       "decoders agree to %.1f dB (our frames), %.1f dB (theirs)\n",
	       frames, same_bits, d_ours, d_theirs);
	EXPECT_GT(d_ours, 12.0);
	EXPECT_GT(d_theirs, 12.0);

	/* the speech in against out (the coder's delay is 40 samples) */
	auto against_in = [&in](const std::vector<int16_t> &out) {
		std::vector<int16_t> ref(in.begin(), in.end() - 40);
		std::vector<int16_t> o(out.begin() + 40, out.end());
		return sdr(ref, o);
	};
	const double s_oo = against_in(oo), s_ot = against_in(ot),
		s_to = against_in(to), s_tt = against_in(tt);
	printf("speech in against out: ours alone %.1f dB, theirs alone %.1f, "
	       "ours to theirs %.1f, theirs to ours %.1f\n", s_oo, s_tt, s_ot,
	       s_to);
	EXPECT_GT(s_ot, 3.0);
	EXPECT_GT(s_to, 3.0);
	EXPECT_GT(s_ot, std::min(s_oo, s_tt) - 1.5);
	EXPECT_GT(s_to, std::min(s_oo, s_tt) - 1.5);
}
