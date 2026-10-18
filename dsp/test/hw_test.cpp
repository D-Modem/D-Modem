/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw_test.cpp - the hardware the harnesses emulate (hw/), on a host of
 *                  the test's own: the ICH's bus masters as the soft
 *                  modems' drivers run them; the HD Audio controller's
 *                  immediate commands and streams, with the Si3054 modem
 *                  codec on it
 *
 *    The bus addresses are offsets into an arena (the mixin's bus
 *    offset), so the test runs as a 64 bit program too.
 *
 */

#include <gtest/gtest.h>

#include <string.h>

#include "hw/hda.hpp"
#include "hw/ich.hpp"
#include "hw/si3054_hda.hpp"

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

/*
 *    the HD Audio controller and the Si3054
 */

namespace {

struct HdaHost : HdaController<HdaHost>, Si3054Hda<HdaHost> {
	int dbg = 0;
	void log(const char *, ...) {}
	u32 hda_codec_command(u32 cmd) { return si3054_command(cmd); }

	/* a command through the immediate command interface */
	u32 cmd(unsigned nid, unsigned verb, unsigned payload, bool four = false)
	{
		hda_write(ICOI, nid << 20 | (four ? (verb & 0xf) << 16 | (payload & 0xffff) :
					     (verb & 0xfff) << 8 | (payload & 0xff)), 4);
		hda_write(ICIS, ICIS_IRV, 2);
		hda_write(ICIS, ICIS_ICB, 2);
		EXPECT_TRUE(hda_read(ICIS, 2) & ICIS_IRV);
		return hda_read(IRII, 4);
	}
	u32 reg(unsigned r) { return cmd(r, 0x9, 0, true); }
	void set(unsigned r, unsigned v) { cmd(r, 0x1, v, true); }
};

class Hda_ : public ::testing::Test {
protected:
	HdaHost h;
	alignas(128) byte bdl[128] = {};
	short buf[8] = {};

	void SetUp() override
	{
		h.hda_write(HdaHost::GCTL, HdaHost::GCTL_CRST, 4);
		/* two entries of four samples each, IOC on the second */
		for (int i = 0; i < 2; i++) {
			uint64_t a = (uintptr_t)(buf + 4 * i);
			for (int k = 0; k < 8; k++)
				bdl[16 * i + k] = (byte)(a >> 8 * k);
			bdl[16 * i + 8] = 8;
			bdl[16 * i + 12] = i == 1;
		}
	}
	/* stream d set up on bdl with stream number tag, and run */
	void run(int d, int tag)
	{
		unsigned b = HdaHost::SD0 + d * HdaHost::SD_SIZE;
		uint64_t a = (uintptr_t)bdl;
		h.hda_write(b + HdaHost::SD_CTL, HdaHost::CTL_SRST, 1);
		EXPECT_TRUE(h.hda_read(b + HdaHost::SD_CTL, 1) & HdaHost::CTL_SRST);
		h.hda_write(b + HdaHost::SD_CTL, 0, 1);
		h.hda_write(b + HdaHost::SD_BDPL, (u32)a, 4);
		h.hda_write(b + HdaHost::SD_BDPU, (u32)(a >> 32), 4);
		h.hda_write(b + HdaHost::SD_CBL, 16, 4);
		h.hda_write(b + HdaHost::SD_LVI, 1, 2);
		h.hda_write(b + HdaHost::SD_FMT, HdaHost::hda_fmt(9600), 2);
		h.hda_write(b + HdaHost::SD_CTL + 2, (u32)tag << 4, 1);
		h.hda_write(b + HdaHost::SD_CTL, HdaHost::CTL_IOCE | HdaHost::CTL_RUN, 1);
	}
};

}

/* out of reset the codec is on SDIN 0, and answers: its IDs, its modem
   function group, its registers (EXTENDED_MID ready once written, the
   link's frame detect, CHIPID read only) */
TEST_F(Hda_, CommandsReachTheCodec)
{
	EXPECT_EQ(h.hda_read(HdaHost::STATESTS, 2), 1u);
	EXPECT_EQ(h.cmd(0, 0xf00, 0x00), 0x163c3055u);
	EXPECT_EQ(h.cmd(0, 0xf00, 0x04), 0x00010001u);
	EXPECT_EQ(h.cmd(1, 0xf00, 0x05) & 0xff, 0x02u);
	EXPECT_EQ(h.reg(HdaHost::EXTENDED_MID) & 0xf, 0u);
	h.set(HdaHost::EXTENDED_MID, 0);
	EXPECT_EQ(h.reg(HdaHost::EXTENDED_MID) & 0xf, 0xfu);
	EXPECT_TRUE(h.reg(HdaHost::LINE_STATUS) & 0x40);
	u32 id = h.reg(HdaHost::CHIPID);
	h.set(HdaHost::CHIPID, 0);
	EXPECT_EQ(h.reg(HdaHost::CHIPID), id);
	h.set(HdaHost::LINE_RATE, 8000);
	EXPECT_EQ(h.si3054_rate(), 8000u);
	h.cmd(0, 0x7ff, 0);             /* the codec's reset */
	EXPECT_EQ(h.si3054_rate(), 9600u);
	EXPECT_EQ(h.si3054_bad, 0ul);
	h.cmd(1, 0x123, 0);             /* no such verb */
	EXPECT_EQ(h.si3054_bad, 1ul);
	/* the controller's reset: STATESTS cleared by a write of 1 */
	h.hda_write(HdaHost::STATESTS, 1, 2);
	EXPECT_EQ(h.hda_read(HdaHost::STATESTS, 2), 0u);
}

/* the stream formats of the Si3054's rates: 48 kHz over 6, 5, 3 */
TEST_F(Hda_, Formats)
{
	EXPECT_EQ(HdaHost::hda_fmt(8000), 0x0510);
	EXPECT_EQ(HdaHost::hda_fmt(9600), 0x0410);
	EXPECT_EQ(HdaHost::hda_fmt(16000), 0x0210);
	for (unsigned r : { 8000u, 9600u, 16000u, 48000u, 96000u })
		EXPECT_EQ(HdaHost::hda_fmt_rate(HdaHost::hda_fmt(r)), r);
}

/* an input stream: its samples in its entries in turn, LPIB round the
   cyclic buffer, BCIS (an interrupt with IOCE and GIE) at the IOC
   entry's end; only its own stream number's samples */
TEST_F(Hda_, InputStreamWalksItsList)
{
	run(HdaHost::IN, 1);
	short s = 7;
	EXPECT_FALSE(h.hda_dma(HdaHost::IN, 2, &s));
	for (short i = 1; i <= 7; i++)
		EXPECT_TRUE(h.hda_dma(HdaHost::IN, 1, &i));
	EXPECT_EQ(h.hda_read(HdaHost::SD0 + HdaHost::SD_LPIB, 4), 14u);
	EXPECT_FALSE(h.hda_read(HdaHost::SD0 + HdaHost::SD_STS, 1) & HdaHost::STS_BCIS);
	s = 8;
	h.hda_dma(HdaHost::IN, 1, &s);
	for (int i = 0; i < 8; i++)
		EXPECT_EQ(buf[i], i + 1);
	EXPECT_EQ(h.hda_read(HdaHost::SD0 + HdaHost::SD_LPIB, 4), 0u);
	EXPECT_TRUE(h.hda_read(HdaHost::SD0 + HdaHost::SD_STS, 1) & HdaHost::STS_BCIS);
	EXPECT_FALSE(h.hda_irq());
	h.hda_write(HdaHost::INTCTL, HdaHost::INTCTL_GIE, 4);
	EXPECT_TRUE(h.hda_irq());
	h.hda_write(HdaHost::SD0 + HdaHost::SD_STS, HdaHost::STS_BCIS, 1);
	EXPECT_FALSE(h.hda_irq());
	s = 9;                          /* round again, from entry 0 */
	h.hda_dma(HdaHost::IN, 1, &s);
	EXPECT_EQ(buf[0], 9);
}

/* an output stream reads its buffer; stopped, nothing */
TEST_F(Hda_, OutputStream)
{
	for (int i = 0; i < 8; i++)
		buf[i] = (short)(100 + i);
	run(HdaHost::OUT, 3);
	short s = 0;
	EXPECT_TRUE(h.hda_dma(HdaHost::OUT, 3, &s));
	EXPECT_EQ(s, 100);
	EXPECT_TRUE(h.hda_dma(HdaHost::OUT, 3, &s));
	EXPECT_EQ(s, 101);
	h.hda_write(HdaHost::SD0 + HdaHost::SD_SIZE + HdaHost::SD_CTL, 0, 1);
	s = 0;
	EXPECT_FALSE(h.hda_dma(HdaHost::OUT, 3, &s));
	EXPECT_EQ(s, 0);
}

/* the codec's line: off hook both ways; on hook the ADC hears the line
   only with the monitor on, and the DAC is not on the line */
TEST_F(Hda_, HookAndMonitor)
{
	short adc;
	EXPECT_EQ(h.si3054_sample(500, &adc, 300), 0);
	EXPECT_EQ(adc, 0);
	h.set(HdaHost::GPIO_CONTROL, HdaHost::GPIO_CID);
	EXPECT_EQ(h.si3054_sample(500, &adc, 300), 0);
	EXPECT_EQ(adc, 500);
	h.set(HdaHost::GPIO_CONTROL, HdaHost::GPIO_OH);
	EXPECT_EQ(h.si3054_sample(500, &adc, 300), 300);
	EXPECT_EQ(adc, 500);
	/* the stream numbers of LINE_LEVEL */
	h.set(HdaHost::LINE_LEVEL, 0x2010);
	EXPECT_EQ(h.si3054_in_tag(), 1);
	EXPECT_EQ(h.si3054_out_tag(), 2);
}

