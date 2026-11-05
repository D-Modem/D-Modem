/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vfc_test.cpp - V.FC's data mode (vfc_data.cpp): its parameters at
 *    each rate, its points against the Courier's own, and its encoder
 *    to its decoder through noise and a quarter turn
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

#include "dm_types.h"
#include "dsplibs.h"
#include "modem_dp.h"
#include "v34.h"

using namespace dsp;

/* the modem core the pumps link against: nothing registered, S
   registers and parameters 0, the bits all taken and given */
int modem_dp_register(enum DP_ID, struct dp_operations *) { return 0; }
/* (nothing to deregister) */
void modem_dp_deregister(enum DP_ID, struct dp_operations *) {}
/* an S register: 0 */
int modem_get_sreg(struct modem *, unsigned int) { return 0; }
/* a parameter set: 0 */
long modem_set_param(struct modem *, unsigned, int) { return 0; }
/* a parameter: 0 (none) */
intptr_t modem_get_param(struct modem *, unsigned) { return 0; }
/* the bits asked for: n */
int modem_get_bits(struct modem *, int, u8 *, int n) { return n; }
/* the bits given: n */
int modem_put_bits(struct modem *, int, u8 *, int n) { return n; }
/* no DTE */
int modem_send_to_tty(struct modem *, const char *, int n) { return n; }
/* (nothing from it) */
int modem_recv_from_tty(struct modem *, char *, int) { return 0; }

namespace {

/* what the Courier's mapper (IDSDL302, overlay 7, run in a simulator)
   made of these line bits, least significant bit of each byte first,
   at symbol rate baud and n * 2400 bit/s: its first 48 points */
struct Vector {
	s16 baud, n, states;
	unsigned char bytes[64];
	s16 pts[48][2];
};

const Vector vectors[] = {
	/* baud 5, 28800 bit/s, 32 states */
	{ 5, 12, 32,
	  {
		0x61, 0x55, 0x3b, 0xfe, 0x2a, 0xe6, 0xe9, 0x62, 0x93, 0xe5,
		0xb0, 0x48, 0x8d, 0x46, 0x62, 0xda, 0xbb, 0xc3, 0x93, 0x02,
		0xc7, 0x3f, 0xa9, 0xff, 0xd6, 0x04, 0x70, 0xf2, 0x27, 0xfc,
		0x26, 0xfa, 0xb9, 0x51, 0x2a, 0x41, 0xa7, 0xb0, 0x9e, 0x95,
		0x0f, 0x1e, 0xc8, 0x96, 0x08, 0x6e, 0xdd, 0x48, 0xd7, 0xbc,
		0x37, 0x12, 0xb9, 0x5c, 0xbb, 0x79, 0x20, 0x8e, 0xab,
	  },
	  {
		{6,3}, {-2,-7}, {-2,3}, {-4,5}, {0,-21}, {-4,5}, {-16,11}, {5,-14},
		{7,-10}, {-8,-11}, {-18,-9}, {-5,6}, {11,-8}, {-1,16}, {10,17}, {-5,6},
		{10,15}, {5,2}, {2,-1}, {4,-17}, {17,0}, {-8,-1}, {-11,0}, {15,-12},
		{0,9}, {-9,10}, {-3,-8}, {-13,6}, {14,3}, {18,7}, {-4,-19}, {-16,-1},
		{5,2}, {16,1}, {8,11}, {4,-17}, {-10,-1}, {-10,9}, {0,3}, {1,-4},
		{9,-16}, {-3,2}, {16,-1}, {7,-16}, {-4,9}, {-20,9}, {-2,3}, {-11,6},
	  } },
	/* baud 3, 26400 bit/s, 8 states */
	{ 3, 11, 8,
	  {
		0xbf, 0xe8, 0x33, 0xa8, 0x34, 0x6e, 0x99, 0x8b, 0x0e, 0x20,
		0xc6, 0xf3, 0xc3, 0x7a, 0x0a, 0xba, 0xae, 0x9f, 0x80, 0x2b,
		0xa2, 0x6d, 0xf4, 0x3e, 0xfc, 0xbf, 0x68, 0xb7, 0xd8, 0x33,
		0xc9, 0xb1, 0xf8, 0xdb, 0x07, 0xaa, 0xf0, 0xda, 0x38, 0x24,
		0x21, 0x37, 0x98, 0x7b, 0xd6, 0xc3, 0xf4, 0x63, 0x5d, 0x3e,
		0xd3, 0x95, 0x07, 0x07, 0xf7, 0xbc, 0x4b, 0xd8, 0x8b, 0x89,
		0x1f, 0x82,
	  },
	  {
		{2,-15}, {-13,8}, {2,-3}, {0,-19}, {11,-4}, {-8,-5}, {12,9}, {-6,-1},
		{-10,15}, {21,-8}, {-8,13}, {6,3}, {23,0}, {-15,-10}, {-6,21}, {8,-3},
		{1,-2}, {4,7}, {15,-10}, {15,-20}, {6,11}, {8,21}, {22,13}, {-1,-10},
		{5,-6}, {-2,-13}, {1,-10}, {-12,11}, {-17,-4}, {22,1}, {18,-1}, {-3,24},
		{2,-11}, {-7,12}, {-24,3}, {19,-16}, {2,1}, {-2,7}, {-7,-8}, {-12,3},
		{-3,-10}, {6,3}, {-24,-1}, {7,2}, {14,13}, {8,-7}, {3,4}, {12,11},
	  } },
	/* baud 0, 14400 bit/s, 32 states */
	{ 0, 6, 32,
	  {
		0xd8, 0x62, 0xc2, 0xe3, 0x6b, 0x0a, 0x42, 0xf7, 0x82, 0x7c,
		0x67, 0xeb, 0xc8, 0xd4, 0x4d, 0xf7, 0x7a, 0x5b, 0x95, 0xe4,
		0xe8, 0x37, 0x81, 0x23, 0x48, 0x23, 0xc1, 0x18, 0x9e, 0xcc,
		0x40, 0xfc, 0xe8, 0x88, 0xfb, 0xb4, 0xcf, 0x9a, 0xe6, 0x25,
		0x4f, 0x19,
	  },
	  {
		{-8,1}, {-6,1}, {-3,-4}, {1,0}, {4,5}, {2,-3}, {1,-8}, {-1,0},
		{6,3}, {-9,-2}, {-2,3}, {-7,-4}, {-3,-8}, {-8,3}, {8,1}, {-1,-6},
		{-1,-6}, {2,-5}, {4,-5}, {5,-4}, {-3,-6}, {3,-8}, {-4,3}, {4,-3},
		{-3,-4}, {-5,4}, {-2,3}, {2,9}, {-4,3}, {-1,2}, {8,-1}, {3,-2},
		{-9,2}, {-5,-4}, {6,3}, {2,-1}, {-3,8}, {-2,-9}, {8,3}, {2,-1},
		{6,3}, {-1,-2}, {-2,7}, {2,9}, {-6,1}, {-3,-8}, {-3,-2}, {-6,-1},
	  } },
	/* baud 2, 19200 bit/s, 8 states */
	{ 2, 8, 8,
	  {
		0x5c, 0x78, 0x7b, 0x48, 0x6a, 0x3a, 0x72, 0x01, 0x68, 0xda,
		0xa8, 0xb6, 0x42, 0x3c, 0xa2, 0x38, 0x02, 0x4b, 0x4d, 0xd0,
		0x55, 0xaa, 0x24, 0xf3, 0xbe, 0x9a, 0x4f, 0x05, 0xca, 0x38,
		0xf4, 0x9a, 0x40, 0x05, 0xe4, 0x27, 0xce, 0x9b, 0xaa, 0xa1,
		0x07, 0x76, 0x74, 0x99, 0xa0, 0xb4, 0x4b, 0xfc,
	  },
	  {
		{4,1}, {4,9}, {-2,3}, {4,-7}, {-7,-6}, {-2,7}, {6,1}, {-11,-2},
		{-6,3}, {10,-5}, {10,-3}, {8,-1}, {-3,2}, {3,-8}, {9,-8}, {2,7},
		{-1,-2}, {-3,2}, {2,1}, {-10,-7}, {0,-3}, {-6,-9}, {6,7}, {-11,2},
		{-3,-4}, {-8,-1}, {-6,1}, {9,-2}, {4,-9}, {-1,12}, {2,-9}, {3,-8},
		{-2,1}, {10,7}, {1,-4}, {-2,-5}, {-8,-1}, {-3,6}, {7,-2}, {4,3},
		{-1,-8}, {0,-1}, {8,-3}, {10,-3}, {5,0}, {-1,6}, {-12,1}, {4,5},
	  } },
	/* baud 1, 24000 bit/s, 32 states */
	{ 1, 10, 32,
	  {
		0x73, 0xdd, 0x8f, 0xdb, 0xec, 0xc7, 0x77, 0x73, 0x82, 0xda,
		0x96, 0x30, 0x2f, 0xcd, 0x83, 0x79, 0xa1, 0x9d, 0xcb, 0x2f,
		0x18, 0x72, 0x4d, 0x24, 0x17, 0x89, 0xcf, 0xe3, 0xb1, 0xa2,
		0x0a, 0x98, 0xfb, 0x65, 0xf6, 0x73, 0xa7, 0xbd, 0x9d, 0xa6,
		0x28, 0x9f, 0x03, 0xd4, 0x87, 0x10, 0x0f, 0x09, 0x30, 0xe1,
		0x3d, 0x99, 0x07, 0xc7, 0x76, 0x53, 0x70, 0x97, 0xd7, 0x32,
		0x84, 0x3b,
	  },
	  {
		{10,-11}, {9,8}, {-3,6}, {11,8}, {1,-24}, {15,4}, {-3,-18}, {5,-12},
		{19,4}, {-4,21}, {17,10}, {3,4}, {-17,-10}, {-5,-6}, {-22,-5}, {-14,-3},
		{-20,-3}, {19,6}, {-5,-10}, {0,-17}, {11,-12}, {18,-5}, {8,-5}, {-16,17},
		{10,-11}, {3,20}, {0,15}, {-2,21}, {-2,17}, {-8,-1}, {22,-7}, {1,-20},
		{4,-7}, {-18,-11}, {-21,10}, {2,-5}, {-10,9}, {-12,-3}, {4,-9}, {-5,-18},
		{-23,0}, {3,-6}, {-11,12}, {-2,-13}, {-19,-8}, {-6,11}, {8,17}, {-2,-9},
	  } },
};

/* line bits in, through a copy of the scrambler: the data bits that the
   transmitter's scrambler turns into them */
struct LineBits {
	std::vector<unsigned char> line;
	size_t pos = 0;
	u32 reg = 0;
	s16 tap = SCR_GPC;
};

/* the next n data bits for the transmitter */
void line_get(void *ctx, unsigned char *bits, s32 n)
{
	auto *l = static_cast<LineBits *>(ctx);
	for (s32 i = 0; i < n; i++) {
		s16 b = l->pos < l->line.size() ? l->line[l->pos] : 1;
		l->pos++;
		bits[i] = static_cast<unsigned char>(descramble(&l->reg, l->tap, b));
	}
}

} // namespace

/* the frame sizes, uncoded bits, shell mapping bits and rings the
   Courier sets up at each symbol rate and rate (its 0x3a4, 0x3a5,
   0x3a7, 0x3a6 + 1 and 0x3a2) */
TEST(Vfc, Params)
{
	struct { s16 baud, n, b, q, k, M, pat; } want[] = {
		{ 0, 6, 48, 2, 0, 4, 0 }, { 0, 8, 64, 4, 0, 4, 0 },
		{ 1, 6, 42, 1, 2, 5, 0 }, { 1, 9, 63, 3, 7, 8, 0 },
		{ 2, 6, 42, 1, 2, 5, 0x7e }, { 2, 8, 55, 2, 7, 8, 0x40 },
		{ 2, 10, 69, 4, 5, 7, 0x70 }, { 3, 6, 39, 0, 7, 8, 0x1c },
		{ 3, 9, 58, 3, 2, 5, 0x18 }, { 3, 11, 71, 4, 7, 8, 0x1c },
		{ 4, 6, 36, 0, 4, 6, 0 }, { 4, 12, 72, 5, 0, 4, 0 },
		{ 5, 6, 34, 0, 2, 5, 0x18 }, { 5, 9, 51, 2, 3, 6, 0x1c },
		{ 5, 12, 68, 4, 4, 6, 0x1e },
	};
	for (const auto &w : want) {
		V34Params p;
		ASSERT_EQ(p.vfc_init(w.baud, w.n, 32), 0) << w.baud << " " << w.n;
		EXPECT_EQ(p.b, w.b);
		EXPECT_EQ(p.q, w.q);
		EXPECT_EQ(p.vfc_k, w.k);
		EXPECT_EQ(p.M, w.M);
		EXPECT_EQ(p.vfc_pat, w.pat) << w.baud << " " << w.n;
	}
	V34Params p;
	/* above each symbol rate's most, and below 14400 */
	EXPECT_LT(p.vfc_init(0, 9, 32), 0);
	EXPECT_LT(p.vfc_init(1, 11, 32), 0);
	EXPECT_LT(p.vfc_init(3, 12, 32), 0);
	EXPECT_LT(p.vfc_init(5, 5, 32), 0);
}

/* D-Modem's points are the Courier's */
TEST(Vfc, CourierPoints)
{
	for (const auto &v : vectors) {
		V34Params p;
		ASSERT_EQ(p.vfc_init(v.baud, v.n, v.states), 0);
		LineBits l;
		for (unsigned char b : v.bytes)
			for (s16 i = 0; i < 8; i++)
				l.line.push_back((b >> i) & 1);
		V34DataTx tx;
		tx.init(&p);
		tx.link.hget = line_get;
		tx.link.hctx = &l;
		u32 scr = 0;
		for (s16 i = 0; i < 48; i++) {
			cplx z = tx.symbol(&scr, l.tap);
			EXPECT_EQ(lrintf(z.re / p.g), v.pts[i][0])
				<< "baud " << v.baud << " n " << v.n << " symbol " << i;
			EXPECT_EQ(lrintf(z.im / p.g), v.pts[i][1])
				<< "baud " << v.baud << " n " << v.n << " symbol " << i;
		}
	}
}

namespace {

struct Bits {
	std::vector<unsigned char> b;
	size_t pos = 0;
};

/* the next n bits to send (then ones) */
void bits_get(void *ctx, unsigned char *bits, s32 n)
{
	auto *s = static_cast<Bits *>(ctx);
	for (s32 i = 0; i < n; i++)
		bits[i] = s->pos < s->b.size() ? s->b[s->pos++] : 1;
}

/* n bits the receiver gave */
void bits_put(void *ctx, const unsigned char *bits, s32 n)
{
	auto *s = static_cast<Bits *>(ctx);
	s->b.insert(s->b.end(), bits, bits + n);
}

/* random bits from the encoder to the decoder through noise at snr dB
   and turned by quarter turns; the bits that came out right */
double through(s16 baud, s16 n, s16 states, double snr, s16 quarter)
{
	V34Params p;
	EXPECT_EQ(p.vfc_init(baud, n, states), 0);
	Bits in, out;
	std::mt19937 rng(baud * 100 + n);
	for (s32 i = 0; i < 20000; i++)
		in.b.push_back(rng() & 1);
	V34DataTx tx;
	tx.init(&p);
	tx.link.hget = bits_get;
	tx.link.hctx = &in;
	V34DataRx rx;
	rx.init(&p);
	rx.link.hput = bits_put;
	rx.link.hctx = &out;
	u32 st = 0, sr = 0;
	std::normal_distribution<float> noise(0, static_cast<float>(
		sqrt(2 / pow(10, snr / 10) / 2)));
	s32 syms = static_cast<s32>(20000 / (p.rate / v34_symbol_rate(baud))) + 64;
	for (s32 i = 0; i < syms; i++) {
		cplx z = tx.symbol(&st, SCR_GPC);
		for (s16 k = 0; k < quarter; k++)
			z = cplx_make(z.im, -z.re);
		z = cplx_make(z.re + noise(rng), z.im + noise(rng));
		rx.symbol(z, &sr, SCR_GPC);
	}
	/* (the descrambler and the differential decoding settle in the
	   first frame) */
	size_t ok = 0, m = std::min(in.b.size(), out.b.size());
	EXPECT_GT(m, 19000u);
	for (size_t i = 100; i < m; i++)
		ok += in.b[i] == out.b[i];
	return m > 100 ? static_cast<double>(ok) / (m - 100) : 0;
}

} // namespace

/* every rate, both codes, clean and turned */
TEST(Vfc, Through)
{
	for (s16 baud = 0; baud < V34_NBAUDS; baud++)
		for (s16 n = 6; n <= 12; n++) {
			V34Params p;
			if (p.vfc_init(baud, n, 32))
				continue;
			EXPECT_EQ(through(baud, n, 32, 60, 0), 1.0)
				<< "baud " << baud << " n " << n;
			EXPECT_EQ(through(baud, n, 8, 60, 1), 1.0)
				<< "baud " << baud << " n " << n;
			EXPECT_EQ(through(baud, n, 32, 60, 3), 1.0)
				<< "baud " << baud << " n " << n;
		}
}

/* at 28800 bit/s, 3429 baud: no errors at 30 dB */
TEST(Vfc, Noise)
{
	EXPECT_EQ(through(5, 12, 32, 30, 2), 1.0);
	EXPECT_EQ(through(5, 12, 8, 30, 3), 1.0);
}
