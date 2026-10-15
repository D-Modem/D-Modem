/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw_test.cpp - the hardware the harnesses emulate (hw/), on a host of
 *                  the test's own: the ICH's bus masters as the soft
 *                  modems' drivers run them
 *
 *    The bus addresses are offsets into an arena (the mixin's bus
 *    offset), so the test runs as a 64 bit program too.
 *
 */

#include <gtest/gtest.h>

#include <string.h>

#include "hw/ich.hpp"

using namespace half;

namespace {

enum { NAMBAR = 0x1000, NABMBAR = 0x1100, BDL = 0x100 };
/* PCM in's registers */
enum { PI_BDBAR = 0x00, PI_CIV = 0x04, PI_LVI = 0x05, PI_SR = 0x06,
       PI_PICB = 0x08, PI_CR = 0x0b };

/* the host: its bus addresses offsets into its arena; no codec, no
   log */
struct ArenaHost : Ich<ArenaHost> {
	int dbg = 0;
	u32 *arena;

	explicit ArenaHost(u32 *a) :
		Ich(NAMBAR, NABMBAR, false, (uintptr_t)a), arena(a)
	{
		/* the descriptor list at BDL (0: none), buffers 0..3 of 4
		   samples each, IOC */
		for (int i = 0; i < 4; i++) {
			arena[BDL / 4 + 2 * i] = 0x1000 + 0x100 * i;
			arena[BDL / 4 + 2 * i + 1] = 4 | 0x80000000u;
		}
	}
	void log(const char *, ...) {}
	unsigned long long ms() const { return 0; }
	word codec_read(int) { return 0; }
	void codec_write(int, word) {}

	void out(unsigned r, u32 v, int size) { ich_io_out(NABMBAR + r, v, size); }
	u32 in(unsigned r, int size)
	{
		u32 v = 0;
		ich_io_in(NABMBAR + r, size, &v);
		return v;
	}
	/* sample i of buffer b, as PCM in wrote it */
	short at(int b, int i) const
	{
		return ((const short *)((const char *)arena + 0x1000 + 0x100 * b))[i];
	}
	/* PCM in run from a reset, lvi its last valid buffer */
	void run(int lvi, byte cr = CR_RPBM | CR_IOCE | CR_LVBIE)
	{
		out(PI_CR, CR_RR, 1);
		out(PI_BDBAR, BDL, 4);
		out(PI_LVI, lvi, 1);
		out(PI_CR, cr, 1);
	}
	/* n samples into PCM in, numbered from first */
	void feed(int n, short first)
	{
		for (int i = 0; i < n; i++)
			ich_sample(pcm_in, first + i);
	}
};

class Ich_ : public ::testing::Test {
protected:
	alignas(8) u32 arena[4096] = {};
	ArenaHost h{arena};
};

}

/* the buffers in turn, each done with IOC: BCIS and the interrupt; the
   status cleared by writing its bits */
TEST_F(Ich_, BuffersInTurn)
{
	h.run(3);
	h.feed(4, 1);
	EXPECT_EQ(h.at(0, 0), 1);
	EXPECT_EQ(h.at(0, 3), 4);
	EXPECT_TRUE(h.in(PI_SR, 2) & ArenaHost::SR_BCIS);
	EXPECT_TRUE(h.ich_irq());
	EXPECT_EQ(h.in(PI_CIV, 1), 1u);
	h.out(PI_SR, ArenaHost::SR_BCIS, 2);
	EXPECT_FALSE(h.ich_irq());
	h.feed(2, 5);
	EXPECT_EQ(h.at(1, 0), 5);
	EXPECT_EQ(h.in(PI_PICB, 2), 2u);
}

/* done with the last valid buffer it halts (LVBCI, CELV, DCH), and goes
   on with the next once LVI moves past it, as the driver does in the
   interrupt */
TEST_F(Ich_, HaltsAtLviResumesWhenItMoves)
{
	h.run(0);
	h.feed(4, 1);
	u32 sr = h.in(PI_SR, 2);
	EXPECT_TRUE(sr & ArenaHost::SR_LVBCI);
	EXPECT_TRUE(sr & ArenaHost::SR_CELV);
	EXPECT_TRUE(sr & ArenaHost::SR_DCH);
	h.feed(2, 100);                 /* halted: nothing written */
	EXPECT_EQ(h.at(1, 0), 0);
	h.out(PI_SR, ArenaHost::SR_LVBCI | ArenaHost::SR_BCIS, 2);
	h.out(PI_LVI, 2, 1);
	h.feed(4, 5);
	EXPECT_EQ(h.at(1, 0), 5);
	EXPECT_EQ(h.at(1, 3), 8);
	EXPECT_FALSE(h.in(PI_SR, 2) & ArenaHost::SR_DCH);
	EXPECT_EQ(h.in(PI_CIV, 1), 2u);
}

/* paused and run again, it goes on where it was (RPBM keeps the state),
   not from the buffer's start */
TEST_F(Ich_, PauseKeepsItsPlace)
{
	h.run(3);
	h.feed(2, 1);
	h.out(PI_CR, ArenaHost::CR_IOCE, 1);     /* pause */
	EXPECT_TRUE(h.in(PI_SR, 2) & ArenaHost::SR_DCH);
	h.feed(2, 100);                     /* paused: nothing written */
	h.out(PI_CR, ArenaHost::CR_RPBM | ArenaHost::CR_IOCE, 1);
	h.feed(2, 3);
	EXPECT_EQ(h.at(0, 0), 1);
	EXPECT_EQ(h.at(0, 1), 2);
	EXPECT_EQ(h.at(0, 2), 3);
	EXPECT_EQ(h.at(0, 3), 4);
	EXPECT_EQ(h.in(PI_CIV, 1), 1u);
}

/* a reset (RR): back to descriptor 0, halted, the status clear */
TEST_F(Ich_, Reset)
{
	h.run(3);
	h.feed(5, 1);
	h.out(PI_CR, ArenaHost::CR_RR, 1);
	EXPECT_EQ(h.in(PI_CIV, 1), 0u);
	EXPECT_EQ(h.in(PI_SR, 2), (u32)ArenaHost::SR_DCH);
	EXPECT_FALSE(h.ich_irq());
	h.out(PI_CR, ArenaHost::CR_RPBM, 1);
	h.feed(1, 50);
	EXPECT_EQ(h.at(0, 0), 50);
}
