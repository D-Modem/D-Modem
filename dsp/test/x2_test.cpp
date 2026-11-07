/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    x2_test.cpp - x2's downstream (dsp/x2.h): the records (ours and the
 *                  client's MP as the I-modem and a Courier sent them), the
 *                  mapper decoded back to its bits, and the training's
 *                  segments
 *
 */

#include <stdlib.h>
#include <string.h>

#include <gtest/gtest.h>

#include "bits.h"
#include "k56flex.h"
#include "x2.h"

using namespace dsp;

namespace {

/* our record, bit for bit as the I-modem sent it */
TEST(X2Record, ServerRecordBits)
{
	/* the I-modem's record (F33C 7FFF 0000) as it went down the line */
	const char *line = "11111111111111111" "0"
		"0011110011001111" "0" "1111111111111110" "0"
		"0000000000000000" "0" "1100111111001110";
	const u16 w[3] = { 0xf33c, 0x7fff, 0x0000 };
	unsigned char b[96];
	s16 n = x2_record(w, 3, b);
	ASSERT_EQ(n, static_cast<s16>(strlen(line)));
	for (s16 i = 0; i < n; i++)
		EXPECT_EQ(b[i], line[i] - '0') << "bit " << i;
}

/* the Courier's MP back from its bits; a bad bit refused */
TEST(X2Record, ClientMpRoundTrip)
{
	/* a Courier's MP: 0344 03FE 0000 0500 */
	const u16 w[4] = { 0x0344, 0x03fe, 0x0000, 0x0500 };
	unsigned char b[128];
	u16 r[4];
	x2_record(w, 4, b);
	ASSERT_EQ(x2_record_parse(b, 4, r), 0);
	for (int i = 0; i < 4; i++)
		EXPECT_EQ(r[i], w[i]);
	/* a bit wrong: its CRC fails */
	b[30] ^= 1;
	EXPECT_NE(x2_record_parse(b, 4, r), 0);
	b[30] ^= 1;
	/* a separator wrong */
	b[18 + 16] ^= 1;
	EXPECT_NE(x2_record_parse(b, 4, r), 0);
}

/* a frame's codes back to its MD + B line bits (before descrambling):
   the digits, the MD signs of the highest ones as changes */
void unmap(const X2Rate &r, const unsigned char code[X2_FRAME], s16 *last,
	   unsigned char *bits)
{
	s16 d[X2_FRAME], car[X2_FRAME] = {}, i, j, k = 0;
	for (i = 0; i < X2_FRAME; i++) {
		unsigned char g = static_cast<unsigned char>(code[i] | 0x80);
		d[i] = -1;
		for (j = 0; j < r.n[i]; j++)
			if (k56_code(r.lv[i][j], 0) == g)
				d[i] = j;
		ASSERT_GE(d[i], 0) << "code " << int(code[i]);
	}
	for (j = 0; j < r.MD; j++) {
		s16 best = -1;
		for (i = 0; i < X2_FRAME; i++)
			if (!car[i] && (best < 0 || d[i] > d[best]))
				best = i;
		car[best] = 1;
	}
	for (i = 0; i < X2_FRAME; i++)
		if (car[i]) {
			s16 s = code[i] & 0x80 ? 1 : 0;
			bits[k++] = static_cast<unsigned char>(s ^ *last);
			*last = s;
		}
	u64 x = 0;
	for (i = X2_FRAME - 1; i >= 0; i--)
		x = x * static_cast<u64>(r.n[i]) + static_cast<u64>(d[i]);
	for (i = 0; i < r.B; i++)
		bits[k++] = (x >> i) & 1;
}

/* random frames at both start-up rates back to their bits */
TEST(X2Map, DecodesBack)
{
	for (const X2Rate *r : { &x2_rate_a, &x2_rate_b }) {
		X2Map m{};
		m.init(*r);
		m.last = 1;
		u32 dscr = 0;
		s16 last = 1;
		s32 rds_max = 0;
		srand(1);
		for (int f = 0; f < 2000; f++) {
			unsigned char in[X2_MAXBITS], line[X2_MAXBITS],
				code[X2_FRAME];
			s16 nb = m.frame_bits();
			ASSERT_EQ(nb, 24);
			for (s16 i = 0; i < nb; i++)
				in[i] = static_cast<unsigned char>(rand() & 1);
			m.frame(in, code);
			unmap(*r, code, &last, line);
			for (s16 i = 0; i < nb; i++)
				ASSERT_EQ(descramble(&dscr, SCR_GPC, line[i]),
					  in[i]) << "frame " << f << " bit " << i;
			if (abs(m.rds) > rds_max)
				rds_max = abs(m.rds);
		}
		/* the free sign holds the running sum down */
		EXPECT_LT(rds_max, 40000);
	}
}

/* the training's segments, their lengths and levels */
TEST(X2Train, Segments)
{
	X2Train t;
	t.init();
	EXPECT_EQ(t.seg, X2_SIL);
	EXPECT_EQ(t.next(), 0);
	t.segment(X2_PRE);
	/* the preamble's first code: the I-modem's 0xab */
	EXPECT_EQ(t.next(), k56_code(0xab, 0));
	s32 n = 1;
	while (t.seg != X2_SYNC) {
		s16 seg = t.seg;
		unsigned char c = t.next();
		if (seg == X2_TRN || seg == X2_JS)
			EXPECT_EQ(c | 0x80, k56_code(0xb1, 0) | 0x80);
		n++;
	}
	EXPECT_EQ(n, 363 + 20004 + 1200 + 6000 + 1152);
	/* held until told; then on to the record at a frame */
	for (int i = 0; i < 6 * 1667; i++)
		t.next();
	EXPECT_EQ(t.seg, X2_SYNC);
	t.next();
	t.go = 1;
	while (t.seg == X2_SYNC)
		t.next();
	EXPECT_EQ(t.seg, X2_REC);
	EXPECT_EQ(t.pos, 0);
	t.go = 1;
	while (t.seg == X2_REC)
		t.next();
	EXPECT_EQ(t.rec_n, 1);
	EXPECT_EQ(t.seg, X2_START);
	for (int i = 0; i < 4086; i++)
		t.next();
	EXPECT_EQ(t.seg, X2_DATA);
}

} // namespace
