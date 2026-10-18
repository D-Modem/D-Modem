/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_hda.cpp - the open modem on an HD-audio modem codec, as a test
 *                   peer: an Intel HD Audio controller (hw/hda.hpp) with
 *                   a Silicon Labs Si3054 modem codec on it
 *                   (hw/si3054_hda.hpp), whose line is the call's, and a
 *                   driver of our own for them; on it the modem core with
 *                   the open data pumps, as dmodemd -a runs them
 *
 *    The driver does what Linux's snd-hda-intel and snd-hda-codec-si3054
 *    do for such a codec, from the interface they use:
 *
 *    - the controller out of reset, the codec on SDIN 0, its vendor ID
 *      (one of the Si3054 driver's list, or no driver binds it), its
 *      modem function group found among the root's nodes;
 *    - the codec's init: SET_CODEC_RESET, node 1's stream format 0,
 *      LINE_RATE 9600, both stream numbers 15 (LINE_LEVEL 0xf0f0),
 *      EXTENDED_MID written and read until its ready bits are there,
 *      GPIO_POLARITY 0xffff, GPIO_CFG 0, MISC_AFE 0, LINE_CFG1 0x200,
 *      LINE_STATUS's frame detect, the line side's chip ID;
 *    - the streams' prepare: LINE_RATE the modem's rate, LINE_LEVEL the
 *      capture's stream number (bits 4..7) and the playback's (12..15),
 *      node 1's stream, channel and format, each stream descriptor reset
 *      and given its buffer (four periods of 10 ms, IOC on each), its
 *      length, last valid index, format and number, then both run;
 *    - the switches as ALSA's controls are, GPIO_CONTROL read, its bit
 *      changed and written: "Off-hook Switch" (bit 0) and "Caller ID
 *      Switch" (bit 1).
 *
 *    The line's samples are the codec's at its rate (CodecLine, from the
 *    call's 8000 Hz).  A period captured (LPIB past it) is the modem's
 *    next 10 ms (dmodemd's AlsaLine, on the codec's clock: off hook the
 *    modem's process(), on hook the ring detector on what the line
 *    monitor hears), its answer written two periods ahead of the
 *    capture into the playback's buffer, and the modem's timers.
 *
 *    HDA_DEBUG, HDA_RATE (the call's line, 8000), HDA_SYNC as the other
 *    peers'; HDA_CODEC_RATE the codec's rate (9600, or 8000; 16000 the
 *    codec has, the modem not); HDA_VENDOR the codec's vendor ID (hex:
 *    one outside the list is refused).  dmodemd's options (--debug,
 *    --country).
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <dcore.h>
#include <dsp.h>
#include <dsplibs.h>
#include <log.h>

#include "hw/codec_line.hpp"
#include "hw/hda.hpp"
#include "hw/si3054_hda.hpp"
#include "modem_half.hpp"
#include "options.h"

namespace half {

/* snd-hda-codec-si3054's vendor IDs */
const u32 SI3054_IDS[] = {
	0x163c3055, 0x163c3155, 0x11c13026, 0x11c13055, 0x11c13155,
	0x10573055, 0x10573057, 0x10573155, 0x11063288, 0x15433155,
	0x18540018,
};

class HdaModem final : public HalfModem, public HdaController<HdaModem>,
		       public Si3054Hda<HdaModem>, public CodecLine<HdaModem>,
		       public dcore::Host {
public:
	HdaModem() : HalfModem("hda", "HDA") {}
	~HdaModem() { delete m_; }

	/* the controller's codec on SDIN 0 */
	u32 hda_codec_command(u32 cmd) { return si3054_command(cmd); }

	/* dcore::Host */
	int start(dcore::Modem &) override { running_ = true; return 0; }
	int stop(dcore::Modem &) override { running_ = false; return 0; }
	int set_hook(dcore::Modem &m, int state) override;
	bool analog_line(dcore::Modem &) override { return true; }
	bool can_flash(dcore::Modem &) override { return true; }
	void hook_flash(dcore::Modem &m, unsigned ms) override;
	int io_delay(dcore::Modem &) override { return LEAD * period_; }
	const char *driver_name() override { return "Si3054 (HD audio)"; }
	int dte_write(dcore::Modem &, const char *buf, int n) override
	{
		results.scan(buf, (size_t)n);
		pty.write(buf, (size_t)n);
		return n;
	}
	unsigned long now_ms() override
	{
		return (unsigned long)(samples_ * 1000 / rate_);
	}

	const char *country = nullptr;

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	enum { PERIODS = 4, LEAD = 2, CAP_TAG = 1, PLAY_TAG = 2 };

	dcore::Modem *m_ = nullptr;
	unsigned rate_ = 9600;
	int period_ = 96;               /* samples: 10 ms */
	unsigned mfg_ = 0;              /* the modem function group's node */
	std::vector<short> cap_, play_; /* the streams' buffers */
	/* the buffer descriptor lists, each 128 octet aligned (BDPL's
	   low 7 bits are not there) */
	alignas(128) byte bdl_[2][128];
	static_assert(PERIODS * 16 <= 128, "a list of PERIODS entries");
	u32 cap_pos_ = 0;               /* octets of the capture done */
	bool running_ = false;
	int hook_ = MODEM_HOOK_ON;
	unsigned long long samples_ = 0, flash_end_ = 0;
	dsp::RingDetect ring_;

	/* the controller's registers */
	u32 rd(unsigned off, int size) { return hda_read(off, size); }
	void wr(unsigned off, u32 v, int size) { hda_write(off, v, size); }
	unsigned sd(int d) const { return SD0 + d * SD_SIZE; }

	/* a command to the codec through the immediate command interface:
	   its response; false if it gave none */
	bool command(u32 cmd, u32 *res);
	u32 verb(unsigned nid, unsigned verb, unsigned payload);
	u32 verb4(unsigned nid, unsigned verb, unsigned payload);
	/* the Si3054's registers */
	word get_reg(unsigned r) { return (word)verb4(r, 0x9, 0); }
	void set_reg(unsigned r, word v) { verb4(r, 0x1, v); }
	/* an ALSA control's put: the GPIO_CONTROL bit set or cleared */
	void set_switch(word bit, bool on);

	bool probe();
	void codec_init();
	bool prepare();
	void period(u32 pos);
};

static HdaModem hda;
#define LOG(...) do { if (hda.dbg) hda.log(__VA_ARGS__); } while (0)

bool HdaModem::command(u32 cmd, u32 *res)
{
	for (int i = 0; i < 1000 && (rd(ICIS, 2) & ICIS_ICB); i++)
		;
	wr(ICOI, cmd, 4);
	wr(ICIS, ICIS_IRV, 2);          /* the last response's valid cleared */
	wr(ICIS, ICIS_ICB, 2);          /* sent */
	for (int i = 0; i < 1000; i++)
		if (rd(ICIS, 2) & ICIS_IRV) {
			*res = rd(IRII, 4);
			if (dbg > 2)
				log("verb %08x: %08x\n", cmd, *res);
			return true;
		}
	log("no response to %08x\n", cmd);
	return false;
}

u32 HdaModem::verb(unsigned nid, unsigned v, unsigned payload)
{
	u32 r = 0;
	command(nid << 20 | (v & 0xfff) << 8 | (payload & 0xff), &r);
	return r;
}

u32 HdaModem::verb4(unsigned nid, unsigned v, unsigned payload)
{
	u32 r = 0;
	command(nid << 20 | (v & 0xf) << 16 | (payload & 0xffff), &r);
	return r;
}

void HdaModem::set_switch(word bit, bool on)
{
	word v = get_reg(Si3054Hda::GPIO_CONTROL);
	set_reg(Si3054Hda::GPIO_CONTROL, on ? (word)(v | bit) : (word)(v & ~bit));
}

/* the controller out of reset, the codec there, its IDs and its modem
   function group: false (said why) if it is not one the Si3054 driver
   binds */
bool HdaModem::probe()
{
	wr(GCTL, 0, 4);
	if (rd(GCTL, 4) & GCTL_CRST)
		return log("the controller does not go into reset\n"), false;
	wr(GCTL, GCTL_CRST, 4);
	if (!(rd(GCTL, 4) & GCTL_CRST))
		return log("the controller does not leave reset\n"), false;
	word codecs = (word)rd(STATESTS, 2);
	wr(STATESTS, codecs, 2);
	if (!(codecs & 1))
		return log("no codec on SDIN 0\n"), false;
	u32 vendor = verb(0, 0xf00, 0x00);
	bool listed = false;
	for (u32 id : SI3054_IDS)
		listed = listed || id == vendor;
	if (!listed) {
		log("codec %04x:%04x: not in snd-hda-codec-si3054's list, "
		    "no driver for it\n", vendor >> 16, vendor & 0xffff);
		return false;
	}
	u32 nodes = verb(0, 0xf00, 0x04);
	unsigned first = (nodes >> 16) & 0xff, n = nodes & 0xff;
	for (unsigned i = first; i < first + n && !mfg_; i++)
		if ((verb(i, 0xf00, 0x05) & 0xff) == 0x02)
			mfg_ = i;
	if (!mfg_)
		return log("codec %08x: no modem function group\n", vendor), false;
	u32 rev = verb(0, 0xf00, 0x02), sub = verb(mfg_, 0xf20, 0);
	LOG("codec %04x:%04x, revision %08x, subsystem %08x, modem "
	    "function group %u\n", vendor >> 16, vendor & 0xffff, rev, sub,
	    mfg_);
	return true;
}

/* the Si3054 driver's init */
void HdaModem::codec_init()
{
	verb(0, 0x7ff, 0);                      /* SET_CODEC_RESET */
	verb4(mfg_, 0x2, 0);                    /* stream format 0 */
	set_reg(Si3054Hda::LINE_RATE, 9600);
	set_reg(Si3054Hda::LINE_LEVEL, 0xf0f0);
	set_reg(Si3054Hda::EXTENDED_MID, 0);
	word mid = 0;
	for (int i = 0; i <= 10; i++) {
		mid = get_reg(Si3054Hda::EXTENDED_MID);
		if ((mid & Si3054Hda::MEI_READY) == Si3054Hda::MEI_READY)
			break;
	}
	if ((mid & Si3054Hda::MEI_READY) != Si3054Hda::MEI_READY)
		log("si3054: cannot initialize, EXT MID = %04x\n", mid);
	set_reg(Si3054Hda::GPIO_POLARITY, 0xffff);
	set_reg(Si3054Hda::GPIO_CFG, 0);
	set_reg(Si3054Hda::MISC_AFE, 0);
	set_reg(Si3054Hda::LINE_CFG1, 0x200);
	word st = get_reg(Si3054Hda::LINE_STATUS);
	if (!(st & Si3054Hda::LINE_FDT))
		log("Link Frame Detect(FDT) is not ready (line status: %04x)\n", st);
	word id = get_reg(Si3054Hda::CHIPID);
	LOG("line side chip ID %u, revision %u, codec revision %u%s\n",
	    (id >> 8) & 0xe, (id >> 4) & 15, id & 15,
	    id & 0x100 ? ", international" : "");
}

/* the streams prepared at rate_ and run */
bool HdaModem::prepare()
{
	word fmt = hda_fmt(rate_);
	u32 bytes = (u32)period_ * 2;

	cap_.assign((size_t)(PERIODS * period_), 0);
	play_.assign((size_t)(PERIODS * period_), 0);
	set_reg(Si3054Hda::LINE_RATE, (word)rate_);
	for (int d = 0; d < 2; d++) {
		int tag = d == IN ? CAP_TAG : PLAY_TAG;
		/* LINE_LEVEL: the other direction's byte kept, this one's
		   stream number in its high nibble */
		word lv = get_reg(Si3054Hda::LINE_LEVEL);
		lv = d == IN ? (word)((lv & 0xff00) | tag << 4) :
			(word)((lv & 0x00ff) | tag << 12);
		set_reg(Si3054Hda::LINE_LEVEL, lv);
		verb(mfg_, 0x706, (unsigned)tag << 4);
		verb4(mfg_, 0x2, fmt);

		/* the stream descriptor: reset, its buffer, its format */
		unsigned b = sd(d);
		wr(b + SD_CTL, CTL_SRST, 1);
		if (!(rd(b + SD_CTL, 1) & CTL_SRST))
			return log("stream %d does not go into reset\n", d), false;
		wr(b + SD_CTL, 0, 1);
		if (rd(b + SD_CTL, 1) & CTL_SRST)
			return log("stream %d does not leave reset\n", d), false;
		short *buf = d == IN ? cap_.data() : play_.data();
		for (int i = 0; i < PERIODS; i++) {
			byte *e = bdl_[d] + 16 * i;
			uint64_t a = (uintptr_t)(buf + i * period_);
			for (int k = 0; k < 8; k++)
				e[k] = (byte)(a >> 8 * k);
			for (int k = 0; k < 4; k++)
				e[8 + k] = (byte)(bytes >> 8 * k);
			e[12] = 1;      /* IOC */
			e[13] = e[14] = e[15] = 0;
		}
		uint64_t bdl = (uintptr_t)bdl_[d];
		wr(b + SD_BDPL, (u32)bdl, 4);
		wr(b + SD_BDPU, (u32)(bdl >> 32), 4);
		wr(b + SD_CBL, PERIODS * bytes, 4);
		wr(b + SD_LVI, PERIODS - 1, 2);
		wr(b + SD_FMT, fmt, 2);
		wr(b + SD_CTL + 2, (u32)tag << 4, 1);   /* the stream number */
		wr(b + SD_CTL, CTL_IOCE, 1);
	}
	if (hda_fmt_rate(si3054_fmt) != si3054_rate())
		log("node 1's format %04x is not the line's rate %u\n",
		    si3054_fmt, si3054_rate());
	/* both run: the playback two periods behind what is captured */
	cap_pos_ = 0;
	for (int d = 0; d < 2; d++)
		wr(sd(d) + SD_CTL, CTL_IOCE | CTL_RUN, 1);
	LOG("streams at %u Hz: capture %d, playback %d, %d periods of %d\n",
	    rate_, CAP_TAG, PLAY_TAG, PERIODS, period_);
	return true;
}

int HdaModem::set_hook(dcore::Modem &, int state)
{
	bool off = state == MODEM_HOOK_OFF;
	hook_ = state;
	flash_end_ = 0;
	/* off hook: the line itself, not the monitor; on hook the monitor,
	   for the rings */
	if (off) {
		set_switch(Si3054Hda::GPIO_CID, false);
		set_switch(Si3054Hda::GPIO_OH, true);
	} else {
		set_switch(Si3054Hda::GPIO_OH, false);
		set_switch(Si3054Hda::GPIO_CID, true);
	}
	return 0;
}

void HdaModem::hook_flash(dcore::Modem &, unsigned ms)
{
	if (hook_ != MODEM_HOOK_OFF)
		return;
	set_switch(Si3054Hda::GPIO_OH, false);
	flash_end_ = samples_ + (unsigned long long)ms * rate_ / 1000;
}

/* the period captured at octet pos: the modem's 10 ms, its answer two
   periods on in the playback's buffer, its timers */
void HdaModem::period(u32 pos)
{
	const short *in = cap_.data() + pos / 2;
	u32 n = (u32)cap_.size();
	short *out = play_.data() + (pos / 2 + LEAD * period_) % n;

	if (flash_end_ && samples_ >= flash_end_) {
		flash_end_ = 0;
		if (hook_ == MODEM_HOOK_OFF)
			set_switch(Si3054Hda::GPIO_OH, true);
	}
	if (hook_ != MODEM_HOOK_OFF)
		for (int i = 0; i < period_; i++)
			if (ring_.sample(in[i]))
				m_->ring();
	samples_ += (unsigned long long)period_;
	if (running_ && m_->started())
		m_->process(in, out, period_);
	else
		memset(out, 0, (size_t)period_ * sizeof(*out));
	m_->poll();
}

short HdaModem::line_sample(short in)
{
	short out = codec_line_sample(in, (double)line_rate / si3054_rate(),
		[this](short line) {
			short adc, dac = 0;
			hda_dma(OUT, si3054_out_tag(), &dac);
			short to_line = si3054_sample(line, &adc, dac);
			hda_dma(IN, si3054_in_tag(), &adc);
			return to_line;
		});
	line_n++;
	/* the capture's periods done (its LPIB past them) */
	u32 cbl = (u32)cap_.size() * 2, pb = (u32)period_ * 2;
	u32 lpib = rd(sd(IN) + SD_LPIB, 4);
	while ((lpib + cbl - cap_pos_) % cbl >= pb) {
		period(cap_pos_);
		cap_pos_ = (cap_pos_ + pb) % cbl;
	}
	/* (the completions: polled, not interrupts) */
	for (int d = 0; d < 2; d++)
		if (rd(sd(d) + SD_STS, 1) & STS_BCIS)
			wr(sd(d) + SD_STS, STS_BCIS, 1);
	return out;
}

void HdaModem::dte_poll()
{
	pty.flush();
	if (dte_waits())
		return;
	pty.read(m_->dte_space(), [this](const void *p, int n) {
		return m_->dte_input((const char *)p, n);
	});
}

int HdaModem::start()
{
	const char *s;

	rate_ = (s = env("CODEC_RATE")) ? (unsigned)atoi(s) : 9600;
	if (rate_ != 8000 && rate_ != 9600) {
		log("HDA_CODEC_RATE: 8000 or 9600\n");
		return -1;
	}
	period_ = (int)rate_ / 100;
	if ((s = env("VENDOR")))
		si3054_vendor = (u32)strtoul(s, nullptr, 16);
	if (!probe())
		return -1;
	codec_init();
	ring_.init((int)rate_);
	dcore::Options o;
	o.name = getenv("MODEM_NAME") ? getenv("MODEM_NAME") : name();
	o.dev_name = "hdaudioC0D0";
	o.rate = (int)rate_;
	o.country = country;
	m_ = new dcore::Modem(*this, o);
	if (!m_->ok())
		return -1;
	if (!prepare())
		return -1;
	set_hook(*m_, MODEM_HOOK_ON);
	LOG("%u verbs to the codec\n", (unsigned)hda_commands);
	return 0;
}

} // namespace half

int main(int argc, char *argv[])
{
	dmd::Options opt;

	dcore::log_level = 1;
	dmd::parse_options(argc, argv, opt);
	dcore::log_open("half");
	prop_dp_init();
	half::hda.country = opt.country;
	int r = half::hda.run();
	prop_dp_exit();
	dcore::log_close();
	return r;
}
