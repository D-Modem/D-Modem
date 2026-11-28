/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t45_test.cpp - dsp/t45.h: the example of T.45 Appendix I, the
 *                   3-octet RUNLEN, runs of 0, long runs, components
 *                   of 2 and 4 octets, and data that is not valid
 *
 */

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "t45.h"

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

/* the stream of T.45 Appendix I: 10 values in 4 runs (the third with
   a 3-octet RUNLEN) */
TEST(T45, AppendixI)
{
	const Bytes in = {0x03, 0x01, 0x00, 0x00, 0x00, 0x0a,
			  0x03, 0xff, 0xff, 0xff,
			  0x02, 0x00, 0x00, 0x00,
			  0x00, 0x00, 0x01, 0xff, 0xff, 0xff,
			  0x04, 0x80, 0x80, 0x00};
	T45Values v;
	size_t used = 0;
	ASSERT_EQ(t45_decode(in.data(), in.size(), &v, &used), 0);
	EXPECT_EQ(used, in.size());
	EXPECT_EQ(v.ncomp, 3);
	EXPECT_EQ(v.complen, 1);
	ASSERT_EQ(v.count(), 10u);
	const u32 want[10][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {0, 0, 0},
				 {0, 0, 0}, {255, 255, 255}, {128, 128, 0}, {128, 128, 0},
				 {128, 128, 0}, {128, 128, 0}};
	for (int i = 0; i < 10; i++)
		for (int c = 0; c < 3; c++)
			EXPECT_EQ(v.vals[i * 3 + c], want[i][c]) << i;
	/* our encoder writes the third run with a 1-octet RUNLEN */
	Bytes out;
	t45_encode(v, &out);
	const Bytes short_form = {0x03, 0x01, 0x00, 0x00, 0x00, 0x0a,
				  0x03, 0xff, 0xff, 0xff,
				  0x02, 0x00, 0x00, 0x00,
				  0x01, 0xff, 0xff, 0xff,
				  0x04, 0x80, 0x80, 0x00};
	EXPECT_EQ(out, short_form);
}

/* a run longer than 255: the 3-octet RUNLEN; longer than 65535: two
   runs; a run of 0 is valid and adds nothing */
TEST(T45, LongRuns)
{
	T45Values v;
	v.ncomp = 1;
	v.vals.assign(300, 7);
	Bytes out;
	t45_encode(v, &out);
	EXPECT_EQ(out, Bytes({0x01, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x01, 0x2c, 0x07}));
	v.vals.assign(70000, 9);
	t45_encode(v, &(out = Bytes()));
	/* 65535, then 4465 */
	EXPECT_EQ(out, Bytes({0x01, 0x01, 0x00, 0x01, 0x11, 0x70, 0x00, 0xff, 0xff, 0x09,
			      0x00, 0x11, 0x71, 0x09}));
	T45Values d;
	ASSERT_EQ(t45_decode(out.data(), out.size(), &d), 0);
	EXPECT_EQ(d.vals, v.vals);

	const Bytes zero = {0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05,
			    0x02, 0x06};
	ASSERT_EQ(t45_decode(zero.data(), zero.size(), &d), 0);
	EXPECT_EQ(d.vals, std::vector<u32>({6, 6}));
	/* no values: the header alone */
	v.vals.clear();
	t45_encode(v, &(out = Bytes()));
	EXPECT_EQ(out, Bytes({0x01, 0x01, 0x00, 0x00, 0x00, 0x00}));
	ASSERT_EQ(t45_decode(out.data(), out.size(), &d), 0);
	EXPECT_EQ(d.count(), 0u);
}

/* components of 2 and of 4 octets (palette indices, 12-bit CIELAB),
   random lists there and back */
TEST(T45, RoundTrip)
{
	std::mt19937 rng(7);
	for (s16 cl : {1, 2, 4})
		for (s16 nc : {1, 3, 4}) {
			T45Values v;
			v.ncomp = nc;
			v.complen = cl;
			u32 mask = cl == 4 ? 0xffffffffu : (1u << (8 * cl)) - 1;
			for (int i = 0; i < 2000;) {
				u32 run = 1 + rng() % (rng() % 4 ? 3 : 600);
				std::vector<u32> c(nc);
				for (auto &x : c)
					x = (u32)rng() & mask & (rng() % 2 ? 0xff : 0xffffffffu);
				for (u32 r = 0; r < run; r++, i++)
					v.vals.insert(v.vals.end(), c.begin(), c.end());
			}
			Bytes out;
			t45_encode(v, &out);
			EXPECT_EQ(out[0], nc);
			EXPECT_EQ(out[1], cl);
			T45Values d;
			size_t used = 0;
			out.push_back(0x55);    /* (data after it is not read) */
			ASSERT_EQ(t45_decode(out.data(), out.size(), &d, &used), 0);
			EXPECT_EQ(used, out.size() - 1);
			EXPECT_EQ(d.ncomp, nc);
			EXPECT_EQ(d.complen, cl);
			EXPECT_EQ(d.vals, v.vals);
		}
}

/* data that is not valid: NCOMP 0, COMPLEN 3, a run past NVALS, the
   data cut short, NVALS over the limit */
TEST(T45, NotValid)
{
	T45Values d;
	const Bytes ok = {0x01, 0x01, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05};
	ASSERT_EQ(t45_decode(ok.data(), ok.size(), &d), 0);
	Bytes b = ok;
	b[0] = 0;
	EXPECT_EQ(t45_decode(b.data(), b.size(), &d), -1);
	b = ok;
	b[1] = 3;
	EXPECT_EQ(t45_decode(b.data(), b.size(), &d), -1);
	b = ok;
	b[6] = 4;
	EXPECT_EQ(t45_decode(b.data(), b.size(), &d), -1);
	for (size_t n = 0; n < ok.size(); n++)
		EXPECT_EQ(t45_decode(ok.data(), n, &d), -1) << n;
	EXPECT_EQ(t45_decode(ok.data(), ok.size(), &d, nullptr, 2), -1);
	const Bytes huge = {0xff, 0x04, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff};
	EXPECT_EQ(t45_decode(huge.data(), huge.size(), &d), -1);
}
