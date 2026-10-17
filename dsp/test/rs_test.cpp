/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    rs_test.cpp - dsp/rs.h: the generator's roots, and the decoder on
 *                  random codewords with errors and erasures up to the
 *                  code's power (V.cvm's RS(32,28), (32,24), (32,16) and
 *                  G.975's RS(255,239)) and beyond it
 *
 */

#include <string.h>

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "rs.h"

using namespace dsp;

namespace {

/* a word's value at a^j (the first octet the highest coefficient) */
uint8_t eval(const uint8_t *cw, int n, int j)
{
	/* the field of rs.h, built again */
	static uint8_t ex[512];
	static int lg[256];
	static bool done = false;
	if (!done) {
		int x = 1;
		for (int i = 0; i < 255; i++) {
			ex[i] = ex[i + 255] = (uint8_t)x;
			lg[x] = i;
			x <<= 1;
			if (x & 0x100)
				x ^= 0x11d;
		}
		done = true;
	}
	uint8_t s = 0;
	for (int i = 0; i < n; i++)
		s = (uint8_t)((s ? ex[lg[s] + j] : 0) ^ cw[i]);
	return s;
}

struct Trial {
	int ok = 0, failed = 0, wrong = 0;
};

/* random codewords with v errors and e erasures */
Trial trials(int n, int k, int v, int e, int count, unsigned seed)
{
	ReedSolomon rs;
	EXPECT_EQ(rs.init(n, k), 0);
	std::mt19937 rng(seed);
	Trial t;
	for (int c = 0; c < count; c++) {
		std::vector<uint8_t> msg(k), cw(n), rx;
		for (auto &m : msg)
			m = (uint8_t)rng();
		rs.encode(msg.data(), cw.data());
		rx = cw;
		std::vector<uint8_t> er(n, 0);
		std::vector<int> pos(n);
		for (int i = 0; i < n; i++)
			pos[i] = i;
		std::shuffle(pos.begin(), pos.end(), rng);
		for (int i = 0; i < v + e; i++) {
			rx[pos[i]] ^= (uint8_t)(1 + rng() % 255);
			if (i >= v)
				er[pos[i]] = 1;
		}
		int r = rs.decode(rx.data(), e ? er.data() : nullptr);
		if (r < 0)
			t.failed++;
		else if (rx == cw)
			t.ok++;
		else
			t.wrong++;
	}
	return t;
}

} // namespace

TEST(ReedSolomon, Roots)
{
	for (int k : { 28, 24, 16 }) {
		ReedSolomon rs;
		ASSERT_EQ(rs.init(32, k), 0);
		uint8_t msg[32], cw[32];
		for (int i = 0; i < k; i++)
			msg[i] = (uint8_t)(i * 29 + 7);
		rs.encode(msg, cw);
		EXPECT_EQ(memcmp(msg, cw, (size_t)k), 0);
		for (int j = 0; j < 32 - k; j++)
			EXPECT_EQ(eval(cw, 32, j), 0) << k << " " << j;
		/* a^(32 - k) is not a root */
		EXPECT_NE(eval(cw, 32, 32 - k), 0);
		EXPECT_EQ(rs.decode(cw), 0);
	}
	ReedSolomon bad;
	EXPECT_EQ(bad.init(256, 200), -1);
	EXPECT_EQ(bad.init(32, 32), -1);
}

TEST(ReedSolomon, Corrects)
{
	/* 2v + e <= n - k: corrected */
	struct { int k, v, e; } cases[] = {
		{ 28, 2, 0 }, { 28, 0, 4 }, { 28, 1, 2 },
		{ 24, 4, 0 }, { 24, 0, 8 }, { 24, 2, 4 }, { 24, 3, 2 },
		{ 16, 8, 0 }, { 16, 0, 16 }, { 16, 5, 6 }, { 16, 1, 14 },
	};
	for (const auto &c : cases) {
		Trial t = trials(32, c.k, c.v, c.e, 300, (unsigned)(c.k * 100 + c.v * 10 + c.e));
		EXPECT_EQ(t.ok, 300) << "RS(32," << c.k << ") v " << c.v << " e " << c.e
				     << ": failed " << t.failed << " wrong " << t.wrong;
	}
	Trial t = trials(255, 239, 8, 0, 50, 1);
	EXPECT_EQ(t.ok, 50);
	t = trials(255, 239, 3, 10, 50, 2);
	EXPECT_EQ(t.ok, 50);
}

TEST(ReedSolomon, Beyond)
{
	/* one error too many: almost always a failure, seldom wrong (or,
	   by luck, right) */
	struct { int k, v, e; } cases[] = {
		{ 28, 3, 0 }, { 24, 5, 0 }, { 16, 9, 0 }, { 24, 3, 3 },
	};
	for (const auto &c : cases) {
		Trial t = trials(32, c.k, c.v, c.e, 500, (unsigned)(c.k + c.v));
		EXPECT_LT(t.ok, 10);
		EXPECT_LT(t.wrong, 25) << "RS(32," << c.k << ") v " << c.v;
		EXPECT_EQ(t.ok + t.failed + t.wrong, 500);
	}
	/* more erasures than check octets: refused */
	ReedSolomon rs;
	rs.init(32, 28);
	uint8_t cw[32] = {}, er[32] = {};
	for (int i = 0; i < 5; i++)
		er[i] = 1;
	cw[0] = 1;
	EXPECT_EQ(rs.decode(cw, er), -1);
}
