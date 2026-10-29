/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pcm_cp_test.cpp - the PCM encoders (V.90's, V.91's) against the
 *    analogue modem's CP: one whose constellations cannot carry its rate
 *    is unusable, and the encoder made from it gives silence instead of
 *    dividing by zero or writing past its frame
 */

#include <string.h>

#include <gtest/gtest.h>

#include "v91.h"

using namespace dsp;

/* a CP of drn with every constellation the n largest Ucodes (none for
   n = 0) */
static V90Cp make_cp(int drn, int n)
{
	V90Cp cp;
	memset(&cp, 0, sizeof(cp));
	cp.cp = 1;
	cp.drn = drn;
	cp.ncon = 1;
	for (int u = 127; u > 127 - n; u--)
		cp.mask[0][u] = 1;
	return cp;
}

static s16 lin[128];

/* drn 22 (56 000 bit/s) on 128 points: K = 36 bits of 42 */
TEST(PcmCp, FullConstellationsAreUsable)
{
	V90Cp cp = make_cp(22, 128);
	V90Enc e;
	EXPECT_TRUE(cp.usable(cp.drn + 20, 6));
	EXPECT_EQ(e.init(&cp, cp.drn + 20, lin), 0);
	EXPECT_EQ(e.K, 36);
}

/* empty constellations, too few points, or more than 48 bits a frame */
TEST(PcmCp, UnusableCps)
{
	V90Cp empty = make_cp(10, 0), few = make_cp(22, 2),
		big = make_cp(29, 128);
	EXPECT_FALSE(empty.usable(empty.drn + 20, 6));
	EXPECT_FALSE(few.usable(few.drn + 20, 6));
	EXPECT_FALSE(big.usable(big.drn + 20, 6));
}

/* V.90: a failed init leaves an encoder that takes frames without a
   division by zero */
TEST(PcmCp, V90EncoderSilentWhenUnusable)
{
	V90Cp cp = make_cp(10, 0);
	V90Enc e;
	unsigned char bits[48];
	s16 out[6];

	memset(bits, 1, sizeof(bits));
	EXPECT_EQ(e.init(&cp, cp.drn + 20, lin), -1);
	EXPECT_EQ(e.K, 0);
	e.push(bits, 1);
	ASSERT_EQ(e.pop(out), 1);
	for (int i = 0; i < 6; i++)
		EXPECT_EQ(out[i], 0);
}

/* V.91 likewise, also for a drn past its 48 bit frames */
TEST(PcmCp, V91EncoderSilentWhenUnusable)
{
	for (V90Cp cp : { make_cp(10, 0), make_cp(31, 128) }) {
		V91Enc e;
		unsigned char bits[48];
		s16 out[6];

		memset(bits, 1, sizeof(bits));
		EXPECT_EQ(e.init(&cp, cp.drn + 20, lin), -1);
		EXPECT_EQ(e.K, 0);
		e.frame(bits, NULL, out);
		for (int i = 0; i < 6; i++)
			EXPECT_EQ(out[i], 0);
	}
}
