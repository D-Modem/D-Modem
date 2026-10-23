/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    cx_daa_test.cpp - a Conexant AC'97 codec's SmartDAA (cx_daa.h)
 *                      against a simulated one: the SSD behind codec
 *                      register 0x7a, the LSD behind the SSD
 *
 */

#include <string.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cx_daa.h"

using dmd::CxDaa;
using dmd::CxVariant;

namespace {

/* a codec with a SmartDAA: register 0x7a the SSD's window, the SSD's
   registers 1 to 3 (and the status, 0 or 4) the LSD's, as cx_daa.h
   has them; what went through 0x7a kept */
class FakeCodec : public dmd::Ac97Regs {
public:
	/* the status in SSD register 4 (status4) or 0; the SSD ready, the
	   LSD on hook with a version */
	explicit FakeCodec(bool status4 = false) : st_(status4 ? 4 : 0)
	{
		memset(ssd, 0, sizeof(ssd));
		memset(lsd, 0, sizeof(lsd));
		ssd[0] = CxDaa::ST_READY;
		lsd[0x02] = 0x0f;               /* powered up */
		lsd[0x22] = 0x05;               /* a version */
		lsd[0x15] = 0x47;               /* on hook, other bits */
	}

	/* see dmd::Ac97Regs: 0x7a's latched SSD register, (reg << 8) | its
	   byte (0xffff without the SSD); the vendor ID; others 0 */
	bool read(int reg, uint16_t *v) override
	{
		if (reg == 0x7c) {
			*v = 0x4358;
		} else if (reg == 0x7e) {
			*v = 0x5428;
		} else if (reg == CxDaa::AC97_WINDOW) {
			*v = absent_ssd ? 0xffff :
					  (uint16_t)(latch_ << 8 | ssd_get(latch_));
		} else {
			*v = 0;
		}
		return true;
	}

	/* see dmd::Ac97Regs: 0x7a's writes, bit 15 a read's request */
	bool write(int reg, uint16_t v) override
	{
		if (reg != CxDaa::AC97_WINDOW)
			return true;
		log.push_back(v);
		int r = v >> 8 & 0x3f;
		if (v & 0x8000)
			latch_ = r;
		else
			ssd_set(r, (uint8_t)v);
		return true;
	}

	/* see dmd::Ac97Regs: the delays added up, none waited */
	void delay_us(unsigned us) override { delayed += us; }

	/* the LSD's registers but its status, power and version, all f */
	void fill_lsd(uint8_t f)
	{
		for (int r = 0; r < 64; r++)
			if (r != 0x02 && r != 0x16 && r != 0x1a && r != 0x1e &&
			    r != 0x1f && r != 0x22 && r != 0x23)
				lsd[r] = f;
	}

	/* the LSD writes (register, byte) the SSD passed on, in order */
	std::vector<std::pair<int, uint8_t>> lsd_writes;
	std::vector<uint16_t> log;      /* register 0x7a's writes */
	/* the SSD's own register writes (but 0 to 4 and 0x17) */
	std::vector<std::pair<int, uint8_t>> ssd_writes;
	uint8_t ssd[64], lsd[64];
	unsigned delayed = 0;
	int latency = 3;                /* status reads before done */
	int errors = 0;                 /* commands to fail */
	bool absent_lsd = false;        /* commands never done */
	bool absent_ssd = false;
	bool mute_answer = false;       /* register 1 never changed */

private:
	/* an SSD register's reading: the status's done bit after
	   latency reads, register 0's frame bit toggling at each */
	uint8_t ssd_get(int r)
	{
		if (r == 0) {
			ssd[0] ^= CxDaa::ST_TICK;
		}
		if (r == st_ && busy_) {
			if (--wait_ <= 0) {
				busy_ = false;
				finish();
			}
		}
		return ssd[r];
	}

	/* an SSD register written; 3 the LSD's command */
	void ssd_set(int r, uint8_t v)
	{
		if (r == 0)
			v = (uint8_t)((v & ~(CxDaa::ST_TICK | CxDaa::ST_READY)) |
				      (ssd[0] & (CxDaa::ST_TICK |
						 CxDaa::ST_READY)));
		ssd[r] = v;
		if (r > 4 && r != 0x17)
			ssd_writes.push_back({ r, v });
		if (r == CxDaa::SSD_COMMAND && !absent_lsd) {
			/* a new command's status (as SOAR's kind 0x1a,
			   which does not clear it, has it) */
			ssd[st_] &= (uint8_t)~(CxDaa::ST_DONE | CxDaa::ST_ERROR);
			cmd_ = v;
			busy_ = true;
			wait_ = latency;
		}
	}

	/* the command's end: an error, or the LSD's register written or
	   read into SSD register 1 */
	void finish()
	{
		if (errors > 0) {
			errors--;
			ssd[st_] |= CxDaa::ST_ERROR;
			return;
		}
		int reg = cmd_ & 0x3f;
		if (cmd_ & CxDaa::CMD_WRITE) {
			lsd[reg] = ssd[CxDaa::SSD_DATA];
			lsd_writes.push_back({ reg, lsd[reg] });
		}
		if (!mute_answer)
			ssd[CxDaa::SSD_ANSWER] = lsd[reg];
		ssd[st_] |= CxDaa::ST_DONE;
	}

	int st_, latch_ = 0, wait_ = 0;
	uint8_t cmd_ = 0;
	bool busy_ = false;
};

} // namespace

/* the SSD's registers through 0x7a: written (reg << 8) | value, read
   0x8000 | (reg << 8) and 0x7a again; 40 us around register 3 */
TEST(CxDaa, SsdWindow)
{
	FakeCodec c;
	CxDaa d(c);
	uint8_t v;

	ASSERT_TRUE(d.ssd_write(0x14, 0x5a));
	EXPECT_EQ(c.ssd[0x14], 0x5a);
	ASSERT_TRUE(d.ssd_read(0x14, &v));
	EXPECT_EQ(v, 0x5a);
	ASSERT_EQ(c.log.size(), 2u);
	EXPECT_EQ(c.log[0], 0x145a);
	EXPECT_EQ(c.log[1], 0x9400);
	EXPECT_EQ(c.delayed, 0u);
	ASSERT_TRUE(d.ssd_read(3, &v));
	EXPECT_EQ(c.delayed, 80u);
	c.absent_ssd = true;
	EXPECT_FALSE(d.ssd_read(0x14, &v));
}

/* the commands written to SSD 3 */
static std::vector<uint16_t> commands(const FakeCodec &c)
{
	std::vector<uint16_t> cmds;
	for (uint16_t w : c.log)
		if ((w >> 8) == CxDaa::SSD_COMMAND)
			cmds.push_back(w & 0xff);
	return cmds;
}

/* an LSD write: the byte into SSD 2, the status cleared, the command
   (bit 7) into SSD 3, its answer in SSD 1 the byte; a control
   register's (0x15, not 0x17) with SSD 0x17's bit 1 set first */
TEST(CxDaa, LsdWrite)
{
	FakeCodec c;
	CxDaa d(c);

	ASSERT_TRUE(d.lsd_write(0x0a, 0xdf)) << d.error();
	EXPECT_FALSE(c.ssd[0x17] & 0x02);
	ASSERT_TRUE(d.lsd_write(0x17, 0x01)) << d.error();
	EXPECT_FALSE(c.ssd[0x17] & 0x02);
	ASSERT_TRUE(d.lsd_write(0x15, 0x87)) << d.error();
	EXPECT_TRUE(c.ssd[0x17] & 0x02);
	ASSERT_EQ(c.lsd_writes.size(), 3u);
	EXPECT_EQ(c.lsd[0x0a], 0xdf);
	EXPECT_EQ(c.lsd[0x15], 0x87);
	std::vector<uint16_t> cmds = commands(c);
	ASSERT_EQ(cmds.size(), 3u);
	EXPECT_EQ(cmds[0], 0x8a);       /* 0x80 | 0x0a */
	EXPECT_EQ(cmds[1], 0x97);
	EXPECT_EQ(cmds[2], 0x95);
}

/* SOAR's kind 0x1a: a control register's write with the command's bit
   6, SSD 0x17 untouched */
TEST(CxDaa, LsdWriteControlBit6)
{
	CxVariant v = dmd::cx_variant(0x1a);
	EXPECT_TRUE(v.control_bit6);
	EXPECT_FALSE(dmd::cx_variant(0x12).control_bit6);

	FakeCodec c(v.status4);
	CxDaa d(c, v);
	ASSERT_TRUE(d.lsd_write(0x15, 0x87)) << d.error();
	ASSERT_TRUE(d.lsd_write(0x0a, 0xdf)) << d.error();
	EXPECT_EQ(c.lsd[0x15], 0x87);
	EXPECT_FALSE(c.ssd[0x17] & 0x02);
	std::vector<uint16_t> cmds = commands(c);
	ASSERT_EQ(cmds.size(), 2u);
	EXPECT_EQ(cmds[0], 0xd5);       /* 0xc0 | 0x15 */
	EXPECT_EQ(cmds[1], 0x8a);
}

/* an error over the barrier: tried again, 5 times at most (0x17
   once); an answer not the byte written is no good */
TEST(CxDaa, LsdWriteRetries)
{
	FakeCodec c;
	CxDaa d(c);

	c.errors = 2;
	EXPECT_TRUE(d.lsd_write(0x0c, 0x01)) << d.error();
	c.errors = 5;
	EXPECT_FALSE(d.lsd_write(0x0c, 0x02));
	c.errors = 1;
	EXPECT_FALSE(d.lsd_write(0x17, 0x02));
	c.errors = 0;
	c.mute_answer = true;
	EXPECT_FALSE(d.lsd_write(0x0c, 0x03));
	EXPECT_NE(d.error().find("answered"), std::string::npos)
		<< d.error();
	c.mute_answer = false;
	c.absent_lsd = true;
	EXPECT_FALSE(d.lsd_write(0x0c, 0x04));
	EXPECT_NE(d.error().find("9 frames"), std::string::npos)
		<< d.error();
}

/* an LSD read: the register's number into SSD 3, its byte from SSD 1;
   the version as the probe */
TEST(CxDaa, LsdReadAndProbe)
{
	FakeCodec c;
	CxDaa d(c);
	uint8_t v = 0;

	c.lsd[0x16] = 0x9c;
	ASSERT_TRUE(d.lsd_read(0x16, &v)) << d.error();
	EXPECT_EQ(v, 0x9c);
	ASSERT_TRUE(d.probe(&v));
	EXPECT_EQ(v, 0x05);
	EXPECT_TRUE(c.lsd_writes.empty());
	c.absent_lsd = true;
	EXPECT_FALSE(d.probe(&v));
}

/* echo (SOAR's kinds 0x0f to 0x1e): SSD 1 primed with 0x55 before a
   read, an answer still 0x55 none; a write primed with the inverse.
   The status in SSD 4 for some */
TEST(CxDaa, EchoAndStatus4)
{
	CxVariant v = dmd::cx_variant(0x11);
	EXPECT_TRUE(v.echo);
	EXPECT_TRUE(v.status4);
	EXPECT_FALSE(dmd::cx_variant(0x1f).echo);
	EXPECT_FALSE(dmd::cx_variant(0x1f).status4);
	EXPECT_TRUE(dmd::cx_variant(0x05).cmd_first);
	EXPECT_FALSE(dmd::cx_variant(99).echo);

	FakeCodec c(true);
	CxDaa d(c, v);
	uint8_t x = 0;
	ASSERT_TRUE(d.lsd_read(0x22, &x)) << d.error();
	EXPECT_EQ(x, 0x05);
	ASSERT_TRUE(d.lsd_write(0x0a, 0x11)) << d.error();
	EXPECT_EQ(c.lsd[0x0a], 0x11);
	c.mute_answer = true;
	c.ssd[CxDaa::SSD_ANSWER] = 0;
	EXPECT_FALSE(d.lsd_read(0x22, &x));
	EXPECT_NE(d.error().find("no answer"), std::string::npos)
		<< d.error();
}

/* the hook: LSD 0x15's bits 7:6 (01 on, 10 off, 00 make, 11 break),
   its others kept; no write when it is already so */
TEST(CxDaa, Hook)
{
	FakeCodec c;
	CxDaa d(c);

	ASSERT_TRUE(d.set_hook(CxDaa::HOOK_OFF)) << d.error();
	EXPECT_EQ(c.lsd[0x15], 0x87);
	ASSERT_TRUE(d.set_hook(CxDaa::PULSE_BREAK));
	EXPECT_EQ(c.lsd[0x15], 0xc7);
	ASSERT_TRUE(d.set_hook(CxDaa::PULSE_MAKE));
	EXPECT_EQ(c.lsd[0x15], 0x07);
	ASSERT_TRUE(d.set_hook(CxDaa::HOOK_ON));
	EXPECT_EQ(c.lsd[0x15], 0x47);
	size_t n = c.lsd_writes.size();
	ASSERT_TRUE(d.set_hook(CxDaa::HOOK_ON));
	EXPECT_EQ(c.lsd_writes.size(), n);
}

/* the codecs it is for: "CX" 0x54xx, "RS" 0x53xx */
TEST(CxDaa, Codecs)
{
	EXPECT_TRUE(dmd::cx_codec(0x4358, 0x5428));
	EXPECT_TRUE(dmd::cx_codec(0x5253, 0x5301));
	EXPECT_FALSE(dmd::cx_codec(0x4358, 0x5928));
	EXPECT_FALSE(dmd::cx_codec(0x5349, 0x4c27));    /* Silicon Labs */
	EXPECT_EQ(dmd::cx_kind(0x5429), 0x12);
	EXPECT_EQ(dmd::cx_kind(0x5428), 0x0d);
	EXPECT_EQ(dmd::cx_kind(0x5431), 0x20);
	EXPECT_EQ(dmd::cx_kind(0x5424), -1);
	EXPECT_EQ(dmd::cx_kind(0x5401), 0x05);
}

/* "reg=byte ..." as (register, byte)s */
static std::vector<std::pair<int, uint8_t>> writes(const char *s)
{
	std::vector<std::pair<int, uint8_t>> w;
	unsigned r, v;
	int n;

	while (sscanf(s, " %x=%x%n", &r, &v, &n) == 2) {
		w.push_back({ (int)r, (uint8_t)v });
		s += n;
	}
	return w;
}

/* "reg=byte ..." of the writes w */
static std::string text(const std::vector<std::pair<int, uint8_t>> &w)
{
	std::string t;
	char b[8];

	for (auto &x : w) {
		snprintf(b, sizeof(b), "%s%02x=%02x", t.empty() ? "" : " ",
			 x.first, x.second);
		t += b;
	}
	return t;
}

/* Conexant's library's LSD writes in the HSF harness (half_hsf,
   HSF_DEBUG=1, docs/hardware.md): its start, an answer, a hangup;
   the LSD's registers first 0, and first 0xff (HSF_DAA_FILL=0xff) */
static const char *const lib_start[2] = {
	"22=05 03=00 04=2f 20=00 25=29 24=28 21=80 19=80 08=00 0a=d9 "
	"0e=00 0c=07 12=00 10=00 19=c0 1f=08 15=40 1c=78 14=e0 07=04 18=43 "
	"19=cb 06=02 0c=06 12=60 1c=30 0e=00 18=43 10=00 1c=68 1c=6a 0c=02 "
	"0a=df 10=00 0e=02 10=00",
	"22=05 1c=fe 03=00 04=2f 20=00 25=29 24=28 21=80 19=ff 08=00 0a=d9 "
	"0e=00 0c=07 12=9f 10=00 19=ff 1f=08 15=6f 1c=fa 15=68 14=e7 07=e4 "
	"18=43 19=cb 07=64 14=e3 15=40 06=e2 06=62 0c=06 12=ff 1c=b2 0e=00 "
	"18=43 10=00 1c=ea 0c=02 0a=df 10=00 0e=02 10=00",
};
static const char *const lib_off_hook[2] = {
	"15=41 12=40 0e=00 14=20 18=43 10=00 21=82 1c=32 15=40 15=80 19=8b",
	"15=41 12=df 0e=00 14=23 18=43 10=00 21=82 1c=b2 15=40 15=80 19=8b",
};
static const char *const lib_on_hook[2] = {
	"12=60 15=40 19=cb 0e=00 21=80 14=e0 18=43 10=00 1c=6a 10=00 0e=02 "
	"10=00",
	"12=ff 15=40 19=cb 0e=00 21=80 14=e3 18=43 10=00 1c=ea 10=00 0e=02 "
	"10=00",
};

/* kind 0x12's setup and hook as the library's: the same LSD writes from
   either start, so the same bits kept; the SSD's own as its */
TEST(CxDaa, Kind12LikeTheLibrary)
{
	for (int f = 0; f < 2; f++) {
		SCOPED_TRACE(f ? "LSD from 0xff" : "LSD from 0");
		FakeCodec c(true);
		c.fill_lsd(f ? 0xff : 0x00);
		CxDaa d(c, dmd::cx_variant(0x12), 0x12);
		uint8_t v = 0;

		ASSERT_TRUE(d.power_up(&v)) << d.error();
		EXPECT_EQ(v, 0x05);
		EXPECT_EQ(text(c.lsd_writes), text(writes(lib_start[f])));
		c.lsd_writes.clear();
		ASSERT_TRUE(d.go_off_hook()) << d.error();
		EXPECT_EQ(text(c.lsd_writes), text(writes(lib_off_hook[f])));
		EXPECT_EQ(c.lsd[0x15] & CxDaa::HOOK_MASK, CxDaa::HOOK_OFF);
		EXPECT_EQ(c.ssd[0x14] & 0x03, 0x01);
		c.lsd_writes.clear();
		ASSERT_TRUE(d.go_on_hook()) << d.error();
		EXPECT_EQ(text(c.lsd_writes), text(writes(lib_on_hook[f])));
		EXPECT_EQ(c.lsd[0x15] & CxDaa::HOOK_MASK, CxDaa::HOOK_ON);
		EXPECT_EQ(c.ssd[0x14] & 0x03, 0x00);
		EXPECT_GE(c.delayed, 100000u);
	}

	/* the library's own SSD writes from 0 (but 0 to 4, 0x17): its
	   start, the line's (the hook's 0x14 aside) */
	FakeCodec c(true);
	CxDaa d(c, dmd::cx_variant(0x12), 0x12);
	uint8_t v;
	ASSERT_TRUE(d.power_up(&v)) << d.error();
	EXPECT_EQ(text(c.ssd_writes),
		  text(writes("1c=40 14=08 0b=5f 06=c0 19=20 05=00 07=00 "
			      "08=00 09=00 0a=00 0d=10 1e=00 19=28 1c=7f "
			      "1d=00 1b=00 1a=00 25=5a 23=43 24=80 22=5a "
			      "20=63 21=a8 07=00 08=00 09=00 0a=00 0c=14 "
			      "1d=00 1b=00 1a=00")));
}

/* kind 0x12's power up: an LSD not powered, or of a version the
   library does not take, fails; another kind's is the probe alone, and
   its hook the bits alone */
TEST(CxDaa, PowerUp)
{
	{
		FakeCodec c(true);
		c.lsd[0x02] = 0x07;
		CxDaa d(c, dmd::cx_variant(0x12), 0x12);
		uint8_t v;
		EXPECT_FALSE(d.power_up(&v));
		EXPECT_NE(d.error().find("not powered"), std::string::npos)
			<< d.error();
	}
	{
		FakeCodec c(true);
		c.lsd[0x22] = 0x31;
		CxDaa d(c, dmd::cx_variant(0x12), 0x12);
		uint8_t v;
		EXPECT_FALSE(d.power_up(&v));
		EXPECT_NE(d.error().find("version"), std::string::npos)
			<< d.error();
	}
	FakeCodec c;
	CxDaa d(c, dmd::cx_variant(0x05), 0x05);
	uint8_t v = 0;
	ASSERT_TRUE(d.power_up(&v)) << d.error();
	EXPECT_EQ(v, 0x05);
	EXPECT_TRUE(c.lsd_writes.empty());
	EXPECT_TRUE(c.ssd_writes.empty());
	ASSERT_TRUE(d.go_off_hook()) << d.error();
	ASSERT_EQ(c.lsd_writes.size(), 1u);
	EXPECT_EQ(c.lsd_writes[0].first, 0x15);
	EXPECT_EQ(c.lsd_writes[0].second, 0x87);
}
