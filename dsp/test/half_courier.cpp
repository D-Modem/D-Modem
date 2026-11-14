/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    half_courier.cpp - a USRobotics Courier V.Everything (external,
 *    supervisor 7.3.14 and DSP 3.0.13 of the ID_SDL 3.02 flash image) as
 *    a test peer (modem_half.hpp): line audio from fd 3 and to fd 4 (16
 *    bit, COURIER_RATE, 8000 by default), AT commands on a pty whose name
 *    is printed ("created courier: /dev/pts/N"); COURIER_SYNC as the
 *    others'.
 *
 *    The whole modem is emulated (hw/courier.hpp): its 80C186EB running
 *    the supervisor out of the flash, the ASIC, the TMS320C51 running the
 *    DSP's code from the same image, the codec at the rate the DSP picks
 *    (7200 to 10266.67 Hz, hw/codec_line.hpp between it and the line),
 *    the NVRAM, the DIP switches.  So the Courier's own HST, V.FC, V.34,
 *    V.32bis, ..., and its own V.42 and MNP, are on the line.
 *
 *    Its files are test/usr/: IDSDL302.ROM (the flash), c51-mask.rom
 *    (the C51's own ROM; its first 2 K words are used) and
 *    courier-nvram.bin (the NVRAM: the factory profile with ATQ0 V1 X1
 *    &H1 &R2 stored, and the options' settings that enable V.FC, V.34,
 *    x2 and V.90); -DDMODEM_USR_DIR=<dir> for other copies, COURIER_ROM,
 *    COURIER_MASK and COURIER_NVRAM for other files.  The NVRAM is read,
 *    never written (AT&W changes it for the run only).
 *
 *    COURIER_DIPS=<hex>: the DIP switches on (switch n in bit n - 1;
 *    0x94, switches 3, 5 and 8: result codes, no auto answer, AT
 *    commands).  The Courier's modulations are chosen by its S registers
 *    (ATS27=4 no V.32, ATS56=96 no V.34, ATS56=224 no V.FC either,
 *    ATS58=33 no x2 or V.90: so "ATS27=4S56=224S58=33" HST,
 *    "ATS56=96S58=33" V.FC).
 *
 *    COURIER_DUMP=<file>: the codec's samples, 16 bit triples of the
 *    ADC's, the DAC's and the codec's rate (Hz, rounded), at the codec's
 *    rate.  COURIER_LINE_DUMP=<file>: the
 *    line's samples, the same pairs (in, out) at the line's rate.  In
 *    both names a "%d" is the process's ID (two Couriers in one call).
 *
 *    COURIER_WATCH=<hex>,<hex>,...: the DSP's data words logged when
 *    they change (looked at each codec sample); with
 *    COURIER_WATCH_EVERY=<ms>, all of them that often instead (0: each
 *    line sample), and with COURIER_WATCH_TIME=<t0>:<t1> only between
 *    those line times (s); COURIER_WATCH_TRIGGER=<hex>=<hex>: those
 *    times counted from when that word first has that value;
 *    COURIER_WATCH_PC=<hex>: the words logged each time the DSP comes to
 *    that address instead (the trigger and the time as before).
 *
 *    COURIER_DEBUG=1 prints the DTE's traffic, the hook and the codec's
 *    rate, 2 the supervisor's messages to the DSP and the DSP's to it
 *    too, 3 the CPU's interrupts as well; COURIER_HW_DEBUG=<mask> what
 *    hw/courier.hpp logs (its dbg).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "hw/codec_line.hpp"
#include "hw/courier.hpp"
#include "modem_half.hpp"

using namespace half;

#ifndef COURIER_ROM
#define COURIER_ROM "IDSDL302.ROM"
#endif
#ifndef COURIER_MASK
#define COURIER_MASK "c51-mask.rom"
#endif
#ifndef COURIER_NVRAM
#define COURIER_NVRAM "courier-nvram.bin"
#endif

class CourierPeer : public HalfModem, public CodecLine<CourierPeer> {
public:
	/* the peer is "courier", its variables COURIER_* */
	CourierPeer() : HalfModem("courier", "COURIER") {}

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	Courier m;
	std::vector<byte> out_;         /* to the DTE, since the last poll */
	FILE *dump_ = nullptr;          /* COURIER_DUMP's */
	FILE *line_dump_ = nullptr;     /* COURIER_LINE_DUMP's */
	/* COURIER_WATCH's DSP data words and their last values */
	std::vector<std::pair<unsigned, unsigned>> watch_;
	unsigned long watch_every_ = 0; /* COURIER_WATCH_EVERY's samples */
	/* COURIER_WATCH_TIME=<t0>:<t1> (s): when (always) */
	unsigned long long watch_t0_ = 0, watch_t1_ = ~0ull;
	/* COURIER_WATCH_TRIGGER=<hex>=<hex>: WATCH_TIME counts from when
	   that word first has that value */
	long trig_a_ = -1;
	bool watch_pc_ = false;         /* COURIER_WATCH_PC given */
	/* COURIER_PC_RING's addresses and ACCs */
	std::vector<std::pair<unsigned, unsigned>> ring_;
	size_t ring_n_ = 0;
	unsigned ring_last_ = 0;
	bool ring_shown_ = false;
	unsigned trig_v_ = 0;
	std::string shown_[2];          /* the DTE's lines being logged */

	/* the DTE's traffic logged a line at a time */
	void show(const char *dir, const void *p, size_t n);
};

static CourierPeer courier;

/* the files loaded, the modem booted (2 s of its own time, on hook:
   the supervisor's power-on, the DSP's code downloaded and started) */
int CourierPeer::start()
{
	const char *rom = env("ROM") ? env("ROM") : COURIER_ROM;
	const char *mask = env("MASK") ? env("MASK") : COURIER_MASK;
	const char *nv = env("NVRAM") ? env("NVRAM") : COURIER_NVRAM;
	if (env("DSP_MIPS"))
		m.set_dsp_ips(atof(env("DSP_MIPS")) * 1e6);
	if (env("DIPS"))
		m.dips = (unsigned)strtoul(env("DIPS"), nullptr, 16);
	m.dbg = dbg >= 3 ? 1 | 4 | 8 : dbg >= 2 ? 1 | 8 : 0;
	if (env("HW_DEBUG"))
		m.dbg = (int)strtol(env("HW_DEBUG"), nullptr, 0);
	m.log = [this](const char *s) {
		log("<%07.3f> %s\n", ms() / 1000.0, s);
	};
	m.to_dte = [this](Courier::u8 b) { out_.push_back(b); };
	m.on_hook = [this](bool off) {
		if (dbg)
			log("%s hook\n", off ? "off" : "on");
	};
	m.on_rate = [this](double hz) {
		if (dbg)
			log("codec at %.2f Hz\n", hz);
	};
	if (env("DUMP")) {
		/* "%d" in the name: the process's ID (two Couriers in a
		   call) */
		char name[512];
		snprintf(name, sizeof(name), env("DUMP"), (int)getpid());
		if (!(dump_ = fopen(name, "wb"))) {
			log("%s: cannot write\n", name);
			return -1;
		}
	}
	if (env("LINE_DUMP")) {
		char name[512];
		snprintf(name, sizeof(name), env("LINE_DUMP"), (int)getpid());
		if (!(line_dump_ = fopen(name, "wb"))) {
			log("%s: cannot write\n", name);
			return -1;
		}
	}
	if (env("WATCH_TIME")) {
		watch_t0_ = (unsigned long long)(atof(env("WATCH_TIME")) * 1000);
		const char *c = strchr(env("WATCH_TIME"), ':');
		if (c)
			watch_t1_ = (unsigned long long)(atof(c + 1) * 1000);
	}
	if (env("WATCH_TRIGGER")) {
		trig_a_ = strtol(env("WATCH_TRIGGER"), nullptr, 16) & 0xffff;
		const char *c = strchr(env("WATCH_TRIGGER"), '=');
		trig_v_ = c ? (unsigned)strtoul(c + 1, nullptr, 16) : 0;
	}
	if (env("WATCH_EVERY"))
		watch_every_ = (unsigned long)(atof(env("WATCH_EVERY")) *
					       line_rate / 1000);
	if (env("WATCH_EVERY") && !watch_every_)
		watch_every_ = 1;
	if (env("WATCH")) {
		std::string w = env("WATCH");
		for (size_t i = 0; i < w.size();) {
			size_t j = w.find(',', i);
			if (j == std::string::npos)
				j = w.size();
			watch_.push_back({ (unsigned)strtoul(w.substr(i, j - i).c_str(),
						 nullptr, 16), 0x10000 });
			i = j + 1;
		}
	}
	if (env("COVER")) {
		/* COURIER_COVER=<t0>:<t1>: each DSP address the first
		   time it runs between those line times (s) */
		double t0 = atof(env("COVER")), t1 = t0 + 1;
		const char *c = strchr(env("COVER"), ':');
		if (c)
			t1 = atof(c + 1);
		static std::vector<bool> seen(65536);
		m.dsp.on_step = [this, t0, t1](C5x &d) {
			double t = ms() / 1000.0;
			if (t < t0 || t > t1 || seen[d.pc])
				return;
			seen[d.pc] = true;
			log("<%07.3f> pc %04x acc %08x ar1 %04x\n", t, d.pc,
			    (unsigned)d.acc, d.ar(1));
		};
	}
	if (env("PC_RING")) {
		/* COURIER_PC_RING=<n>: the DSP's last n jumps (and ACC)
		   printed when it stops */
		ring_.resize((size_t)atoi(env("PC_RING")));
		m.dsp.on_step = [this](C5x &d) {
			if (ring_.empty())
				return;
			/* the jumps only: where from (the last address before)
			   and where to */
			unsigned at = d.pc;
			if (at - ring_last_ > 2u)
				ring_[ring_n_++ % ring_.size()] = { ring_last_ << 16 | at,
								     (unsigned)d.acc };
			ring_last_ = at;
		};
	}
	if (env("STORE_WATCH")) {
		/* COURIER_STORE_WATCH=<hex>: each DSP address that stores
		   to that data word, the first time */
		m.dsp.watch = strtol(env("STORE_WATCH"), nullptr, 16) & 0xffff;
		static std::vector<bool> by(65536);
		m.dsp.watch_fn = [this](C5x &d, C5x::w16 v) {
			if (by[d.pc])
				return;
			by[d.pc] = true;
			log("<%07.3f> %04lx = %04x from %04x\n", ms() / 1000.0,
			    d.watch, v, d.pc);
		};
	}
	if (env("WATCH_PC")) {
		/* COURIER_WATCH_PC=<hex>: COURIER_WATCH's words each time
		   the DSP reaches that address, in the watch's time */
		unsigned a = (unsigned)strtoul(env("WATCH_PC"), nullptr, 16);
		watch_pc_ = true;
		m.dsp.hook(a, [this](C5x &d) {
			if (trig_a_ >= 0 || ms() < watch_t0_ || ms() > watch_t1_)
				return;
			std::string l;
			char b[16];
			for (auto &w : watch_) {
				snprintf(b, sizeof(b), " %04x", d.D[w.first & 0xffff]);
				l += b;
			}
			log("<%07.3f> pc%s\n", ms() / 1000.0, l.c_str());
		});
	}
	std::string err;
	if (!m.load(rom, mask, nv, err)) {
		log("%s\n", err.c_str());
		return -1;
	}
	for (long n = (long)(2 * m.rate()); n > 0; n--)
		m.sample(0);
	if (m.cpu.stopped) {
		log("the supervisor stopped at %05x\n", m.cpu.stop_at);
		return -1;
	}
	return 0;
}

/* one line sample each way through the resampler, a codec sample each
   way through the modem; the line is the modem's only off hook */
short CourierPeer::line_sample(short in)
{
	short out = codec_line_sample(in, (double)line_rate / m.rate(),
				      [this](short adc) {
		if (!m.off_hook)
			adc = 0;
		short dac = m.sample(adc);
		if (m.dsp.stopped && !ring_shown_ && !ring_.empty()) {
			ring_shown_ = true;
			for (size_t i = 0; i < ring_.size(); i++) {
				auto &r = ring_[(ring_n_ + i) % ring_.size()];
				log("ring %04x -> %04x acc %08x\n", r.first >> 16,
				    r.first & 0xffff, r.second);
			}
		}
		if (trig_a_ >= 0 && m.dsp.D[trig_a_] == trig_v_) {
			/* COURIER_WATCH_TRIGGER: the time from now */
			watch_t1_ = ms() + (watch_t1_ - watch_t0_);
			watch_t0_ = ms();
			trig_a_ = -1;
		}
		if (trig_a_ < 0 && !watch_pc_ && watch_every_ &&
		    line_n % watch_every_ == 0 &&
		    !watch_.empty() && ms() >= watch_t0_ && ms() <= watch_t1_) {
			std::string l;
			char b[16];
			for (auto &w : watch_) {
				snprintf(b, sizeof(b), " %04x", m.dsp.D[w.first & 0xffff]);
				l += b;
			}
			log("<%07.3f> dsp%s\n", ms() / 1000.0, l.c_str());
		}
		for (auto &w : watch_)
			if (!watch_every_ && m.dsp.D[w.first & 0xffff] != w.second) {
				w.second = m.dsp.D[w.first & 0xffff];
				log("<%07.3f> dsp %04x = %04x (pc %04x)\n",
				    ms() / 1000.0, w.first, w.second, m.dsp.pc);
			}
		if (!m.off_hook)
			dac = 0;
		if (dump_) {
			short s[3] = { adc, dac, (short)(m.rate() + 0.5) };
			fwrite(s, sizeof(s), 1, dump_);
		}
		return dac;
	});
	if (line_dump_) {
		/* at line_n's place, as the log's times */
		short s[2] = { in, out };
		fseek(line_dump_, (long)(line_n * sizeof(s)), SEEK_SET);
		fwrite(s, sizeof(s), 1, line_dump_);
	}
	line_n++;
	return out;
}

/* the modem's output to the pty; the DTE's input to the modem while
   it holds CTS on (&H1), a few octets ahead of its UART */
void CourierPeer::dte_poll()
{
	if (!out_.empty()) {
		show("dte <-", out_.data(), out_.size());
		pty.write(out_.data(), out_.size());
		results.scan(out_.data(), out_.size());
		out_.clear();
	}
	pty.flush();
	if (dte_waits() || !m.cts() || m.dte_queued() >= 16)
		return;
	pty.read(16 - (int)m.dte_queued(), [this](const void *p, int n) {
		show("dte ->", p, (size_t)n);
		for (int i = 0; i < n; i++)
			m.from_dte(((const byte *)p)[i]);
		return n;
	});
}

/* the DTE's traffic logged a line at a time (to its CR or LF) */
void CourierPeer::show(const char *dir, const void *p, size_t n)
{
	if (!dbg)
		return;
	std::string &t = dir[4] == '<' ? shown_[0] : shown_[1];
	for (size_t i = 0; i < n; i++) {
		byte c = ((const byte *)p)[i];
		if (c == '\r' || c == '\n' || t.size() >= 76) {
			if (!t.empty())
				log("%s %s\n", dir, t.c_str());
			t.clear();
			if (c == '\r' || c == '\n')
				continue;
		}
		t += c >= 32 && c < 127 ? (char)c : '.';
	}
}

/* the Courier on the line until it ends (HalfModem::run) */
int main()
{
	return courier.run();
}
