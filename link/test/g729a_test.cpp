/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a_test.cpp - G.729 Annex A (link/g729a.h): the packing, the
 *                     coder on its own, and bit exactness against the
 *                     Recommendation's test vectors where they are
 *                     (docs/specs/fetch_g729.sh puts them in
 *                     docs/specs/g729a; $DMODEM_G729_VECTORS another
 *                     directory); skipped without them
 *
 *    g729a_test                                all of them
 *    g729a_test --gtest_filter=G729a.Vectors   one
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "g729a.h"

using namespace dlink;

/* the vectors' directory */
static std::string vector_dir()
{
	const char *e = getenv("DMODEM_G729_VECTORS");
	return e && *e ? e : DMODEM_G729_VECTORS;
}

/* file's 16 bit little-endian words (empty if it is not there) */
static std::vector<int16_t> words(const std::string &file)
{
	std::vector<int16_t> w;
	FILE *f = fopen(file.c_str(), "rb");
	if (!f)
		return w;
	unsigned char b[2];
	while (fread(b, 1, 2, f) == 2)
		w.push_back(static_cast<int16_t>(b[0] | b[1] << 8));
	fclose(f);
	return w;
}

enum { SERIAL = 82, SYNC = 0x6b21, BIT_1 = 0x0081, BIT_0 = 0x007f };

/* each parameter's bits */
static const int BITS[g729::PRMS] = { 8, 10, 8, 1, 13, 4, 7, 5, 13, 4, 7 };

/* a frame's parameters to the Recommendation's serial format */
static void to_serial(const int16_t *prm, int16_t *serial)
{
	*serial++ = SYNC;
	*serial++ = 80;
	for (int i = 0; i < g729::PRMS; i++)
		for (int b = BITS[i] - 1; b >= 0; b--)
			*serial++ = prm[i] >> b & 1 ? BIT_1 : BIT_0;
}

/* (see to_serial) back; whether the frame is erased (a zero word) */
static bool from_serial(const int16_t *serial, int16_t *prm)
{
	bool erased = false;
	for (int i = 2; i < SERIAL; i++)
		erased = erased || serial[i] == 0;
	const int16_t *p = serial + 2;
	for (int i = 0; i < g729::PRMS; i++) {
		int v = 0;
		for (int b = 0; b < BITS[i]; b++)
			v = v << 1 | (*p++ == BIT_1);
		prm[i] = static_cast<int16_t>(v);
	}
	return erased;
}

/* the frame's octets are its bits in order, and back */
TEST(G729a, Pack)
{
	const int16_t prm[g729::PRMS] = { 0xa5, 0x2c3, 0x81, 1, 0x1abc, 0x9,
					  0x55, 0x13, 0x0f0f, 0x6, 0x2a };
	int16_t serial[SERIAL];
	to_serial(prm, serial);
	uint8_t oct[g729::FRAME_OCTETS];
	g729::pack(prm, oct);
	for (int i = 0; i < 80; i++)
		EXPECT_EQ(oct[i / 8] >> (7 - i % 8) & 1, serial[2 + i] == BIT_1)
			<< "bit " << i;
	int16_t back[g729::PRMS];
	g729::unpack(oct, back);
	for (int i = 0; i < g729::PRMS; i++)
		EXPECT_EQ(prm[i], back[i]) << "parameter " << i;
}

/* a tone and a chirp through the encoder and decoder: what comes out is
   like what went in (the coder's delay is 5 ms here, 40 samples:
   the look-ahead); a lost frame is concealed, a reset starts again */
TEST(G729a, RoundTrip)
{
	g729::Encoder enc;
	g729::Decoder dec;
	std::vector<int16_t> in, out;
	for (int n = 0; n < 8000; n++) {
		const double t = n / 8000.0;
		const double f = 300 + 400 * t;
		in.push_back(static_cast<int16_t>(
			6000 * sin(2 * M_PI * f * t) +
			3000 * sin(2 * M_PI * 1.7 * f * t)));
	}
	for (size_t i = 0; i + g729::FRAME <= in.size(); i += g729::FRAME) {
		uint8_t oct[g729::FRAME_OCTETS];
		int16_t pcm[g729::FRAME];
		enc.encode(&in[i], oct);
		dec.decode(oct, pcm);
		out.insert(out.end(), pcm, pcm + g729::FRAME);
	}
	/* the signal's energy against the error's, once settled */
	double sig = 0, err = 0;
	for (size_t i = 800; i + 40 < out.size(); i++) {
		sig += double(in[i]) * in[i];
		const double d = double(out[i + 40]) - in[i];
		err += d * d;
	}
	EXPECT_GT(10 * log10(sig / err), 3.0);

	/* a lost frame: something like the last, not silence or noise */
	int16_t pcm[g729::FRAME];
	dec.erase(pcm);
	double e = 0;
	for (int16_t v : pcm)
		e += double(v) * v;
	EXPECT_GT(e, 0);

	/* the same input from a reset gives the same bits */
	uint8_t a[g729::FRAME_OCTETS], b[g729::FRAME_OCTETS];
	enc.reset();
	enc.encode(&in[0], a);
	g729::Encoder fresh;
	fresh.encode(&in[0], b);
	for (int i = 0; i < g729::FRAME_OCTETS; i++)
		EXPECT_EQ(a[i], b[i]);
}

/* the Recommendation's vectors: each .IN through our encoder gives its
   .BIT, each .BIT through our decoder its .PST, word for word */
TEST(G729a, Vectors)
{
	const std::string dir = vector_dir();
	if (words(dir + "/SPEECH.BIT").empty())
		GTEST_SKIP() << "no G.729 Annex A test vectors in " << dir
			     << " (docs/specs/fetch_g729.sh)";

	static const char *const enc_names[] = {
		"ALGTHM", "FIXED", "LSP", "PITCH", "SPEECH", "TAME", "TEST",
	};
	for (const char *name : enc_names) {
		const auto in = words(dir + "/" + name + ".IN");
		const auto bit = words(dir + "/" + name + ".BIT");
		ASSERT_FALSE(in.empty()) << name;
		const size_t frames = in.size() / g729::FRAME;
		ASSERT_EQ(bit.size(), frames * SERIAL) << name;
		g729::Encoder enc;
		size_t bad = 0, first = 0;
		for (size_t f = 0; f < frames; f++) {
			int16_t prm[g729::PRMS], serial[SERIAL];
			enc.encode(&in[f * g729::FRAME], prm);
			to_serial(prm, serial);
			bool same = true;
			for (int i = 0; i < SERIAL; i++)
				same = same && serial[i] == bit[f * SERIAL + i];
			if (!same && bad++ == 0)
				first = f;
		}
		EXPECT_EQ(bad, 0u) << name << ".IN: " << bad << " of " << frames
				   << " frames differ, the first " << first;
	}

	static const char *const dec_names[][2] = {
		{ "ALGTHM", "PST" }, { "ERASURE", "PST" }, { "FIXED", "PST" },
		{ "LSP", "PST" }, { "OVERFLOW", "PST" }, { "PARITY", "PST" },
		{ "PITCH", "PST" }, { "SPEECH", "PST" }, { "TAME", "PST" },
		{ "TEST", "pst" },
	};
	for (const auto &n : dec_names) {
		const auto bit = words(dir + "/" + n[0] + ".BIT");
		const auto pst = words(dir + "/" + n[0] + "." + n[1]);
		ASSERT_FALSE(bit.empty()) << n[0];
		const size_t frames = bit.size() / SERIAL;
		ASSERT_EQ(pst.size(), frames * g729::FRAME) << n[0];
		g729::Decoder dec;
		size_t bad = 0, first = 0;
		for (size_t f = 0; f < frames; f++) {
			int16_t prm[g729::PRMS], pcm[g729::FRAME];
			const bool erased = from_serial(&bit[f * SERIAL], prm);
			dec.decode(prm, erased, pcm);
			bool same = true;
			for (int i = 0; i < g729::FRAME; i++)
				same = same && pcm[i] == pst[f * g729::FRAME + i];
			if (!same && bad++ == 0)
				first = f;
		}
		EXPECT_EQ(bad, 0u) << n[0] << ".BIT: " << bad << " of " << frames
				   << " frames differ, the first " << first;
	}
}
