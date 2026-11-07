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
   the digits, the MD signs of the smallest levels as changes */
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
			if (!car[i] && (best < 0 || r.lv[i][d[i]] >
					r.lv[best][d[best]]))
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

/* random frames at both start-up rates and the next ones back to their
   bits */
TEST(X2Map, DecodesBack)
{
	for (const X2Rate *r : { &x2_rate_a, &x2_rate_b, &x2_rate_c,
				      &x2_rate_d }) {
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
			ASSERT_EQ(nb, r == &x2_rate_c ? 41 : r == &x2_rate_d ? 39 : 24);
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

/* the I-modem's own line octets, 10 frames of the ones after its rate
   change (from frame 30 of its rate D, frame 68 of its rate C, in two
   courier-emu calls): back to ones, frames whose signs go to other
   positions by the digits than by the levels among them */
TEST(X2Map, DecodesIModemOnes)
{
	static const unsigned char d[60] = {
	0x30, 0x35, 0xac, 0xa8, 0xd1, 0x4b,
	0xb2, 0x47, 0xaf, 0x38, 0x25, 0xcf,
	0x41, 0x60, 0x9f, 0xc1, 0xbd, 0x33,
	0x23, 0x3a, 0x34, 0xc7, 0xd1, 0x23,
	0x37, 0x7a, 0xa6, 0x35, 0x2f, 0xf0,
	0x47, 0x5a, 0xa1, 0xa6, 0xc1, 0xa1,
	0x38, 0xb4, 0xd4, 0x4b, 0x41, 0x45,
	0x26, 0xb7, 0xba, 0xcf, 0x2e, 0xa7,
	0x43, 0x3d, 0xa3, 0xb3, 0x20, 0x43,
	0x24, 0xb5, 0xb7, 0x2f, 0xbe, 0xba,
	};
	static const unsigned char c[60] = {
	0xbc, 0x9f, 0x4f, 0x21, 0x2f, 0x4c,
	0x3b, 0x51, 0xb6, 0xc3, 0xa0, 0xf4,
	0xbc, 0x1f, 0x4a, 0xb4, 0xaf, 0x31,
	0xa8, 0x26, 0x30, 0xa9, 0x51, 0xc7,
	0xad, 0x3c, 0x4b, 0xd1, 0x47, 0xee,
	0xbb, 0x4b, 0xd7, 0xe2, 0x6e, 0xea,
	0x74, 0xe6, 0x6a, 0xbc, 0x66, 0x3c,
	0x34, 0xdf, 0x3d, 0xb5, 0x4b, 0xba,
	0x51, 0xc9, 0x4b, 0xa8, 0x4b, 0x45,
	0xcc, 0x3b, 0x3d, 0x53, 0x33, 0xac,
	};
	const struct {
		const X2Rate *r;
		const unsigned char *o;
	} calls[] = { { &x2_rate_d, d }, { &x2_rate_c, c } };
	for (const auto &t : calls) {
		u32 dscr = 0;
		s16 last = 1;
		for (int f = 0; f < 10; f++) {
			unsigned char code[X2_FRAME], line[X2_MAXBITS];
			for (int i = 0; i < X2_FRAME; i++) {
				unsigned char o = t.o[6 * f + i];
				code[i] = static_cast<unsigned char>(
					k56_code(o | 0x80, 0) &
					(o & 0x80 ? 0xff : 0x7f));
			}
			unmap(*t.r, code, &last, line);
			s16 nb = static_cast<s16>(t.r->B + t.r->MD);
			for (s16 i = 0; i < nb; i++) {
				unsigned char b = static_cast<unsigned char>(
					descramble(&dscr, SCR_GPC, line[i]));
				/* (the descrambler from the first frame) */
				if (f > 0)
					EXPECT_EQ(b, 1) << "frame " << f
							<< " bit " << i;
			}
		}
	}
}

/* the next rate by the most the client takes */
TEST(X2Map, RateFor)
{
	EXPECT_EQ(x2_rate_for(1), nullptr);
	EXPECT_EQ(x2_rate_for(11), nullptr);
	EXPECT_EQ(x2_rate_for(12), &x2_rate_d);
	EXPECT_EQ(x2_rate_for(13), nullptr);
	EXPECT_EQ(x2_rate_for(14), &x2_rate_c);
	EXPECT_EQ(x2_rate_for(15), nullptr);
	/* enough levels for the bits, and each list the largest first */
	for (const X2Rate *r : { &x2_rate_c, &x2_rate_d }) {
		double v = 1;
		for (int i = 0; i < X2_FRAME; i++) {
			v *= r->n[i];
			for (int j = 1; j < r->n[i]; j++)
				EXPECT_LT(r->lv[i][j - 1], r->lv[i][j]);
		}
		EXPECT_GE(v, static_cast<double>(INT64_C(1) << r->B));
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
