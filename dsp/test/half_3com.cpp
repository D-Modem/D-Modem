/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    half_3com.cpp - a 3Com/Megahertz XJ5560 / CC5560 PC Card modem
 *    (firmware TC110, the 3Com Mobile Products Group's update of
 *    1998-01) as a test peer (modem_half.hpp): line audio from fd 3 and
 *    to fd 4 (16 bit, TC110_RATE, 8000 by default), AT commands on a pty
 *    whose name is printed ("created 3com: /dev/pts/N"); TC110_SYNC as
 *    the others'.
 *
 *    The card is emulated (hw/tc110.hpp): its TMS320C5x running the
 *    flash's code, the host UART, the line sense, the codec at 7200 Hz
 *    (hw/codec_line.hpp between it and the line).  Its file is
 *    test/3com/tc110.bin (the flash's 8 pages, from TC110.UPF);
 *    -DDMODEM_3COM_DIR=<dir> for another copy, TC110_FLASH for another
 *    file.
 *
 *    Not all of the DSP's ROM is known (hw/tc110.hpp): the DSP stops
 *    at a ROM address not known, and this says which.
 *
 *    TC110_DUMP=<file>: the codec's samples, 16 bit pairs of the ADC's
 *    and the DAC's, at the codec's rate ("%d" in the name: the
 *    process's ID).  TC110_DEBUG=1 prints the DTE's traffic and the
 *    codec's rate.
 *    TC110_CORE=<prefix>: when the DSP stops, its program RAM and its
 *    data to <prefix>.pr and <prefix>.d.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "hw/codec_line.hpp"
#include "hw/tc110.hpp"
#include "modem_half.hpp"

using namespace half;

#ifndef TC110_FLASH
#define TC110_FLASH "tc110.bin"
#endif

class Tc110Peer : public HalfModem, public CodecLine<Tc110Peer> {
public:
	/* the peer is "3com", its variables TC110_* */
	Tc110Peer() : HalfModem("3com", "TC110") {}

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	Tc110 m;
	std::vector<unsigned char> out_; /* to the DTE, since the last poll */
	FILE *dump_ = nullptr;          /* TC110_DUMP's */
	bool stop_shown_ = false;
	bool core_done_ = false;
	long prof_left_ = 0;            /* codec samples still profiled */        /* TC110_CORE_AT's done */
	std::string shown_[2];          /* the DTE's lines being logged */

	/* the DTE's traffic logged a line at a time */
	void show(const char *dir, const void *p, size_t n);
	/* TC110_CORE=<prefix>: the DSP's program RAM and data written */
	void core();
};

static Tc110Peer tc110;

/* the flash loaded, the modem booted (0.5 s of its own time: its
   start-up, then DTR on) */
int Tc110Peer::start()
{
	const char *flash = env("FLASH") ? env("FLASH") : TC110_FLASH;
	if (env("CARRIER"))
		m.carrier_sign = atoi(env("CARRIER"));
	if (env("DSP_MIPS"))
		m.ips = atof(env("DSP_MIPS")) * 1e6;
	m.log = [this](const char *s) {
		log("<%07.3f> %s\n", ms() / 1000.0, s);
	};
	m.to_dte = [this](unsigned char b) { out_.push_back(b); };
	m.dbg = dbg;
	if (env("DUMP")) {
		char name[512];
		snprintf(name, sizeof(name), env("DUMP"), (int)getpid());
		if (!(dump_ = fopen(name, "wb"))) {
			log("%s: cannot write\n", name);
			return -1;
		}
	}
	if (env("WATCH")) {
		/* TC110_WATCH=<pc>,<addr>,<n>[,<from ms>]: n data words
		   from addr each time the DSP is at pc */
		unsigned pc = 0, a = 0, n = 1;
		long from = 0;
		sscanf(env("WATCH"), "%x,%x,%u,%ld", &pc, &a, &n, &from);
		m.watch_pc = pc;
		m.hist_on = env("HIST") != nullptr;
		m.on_watch = [this, a, n, from]() {
			if ((long)ms() < from)
				return;
			if (m.hist_on) {
				/* TC110_HIST: the PCs before, once */
				std::string h;
				char w[16];
				for (unsigned i = m.hist_n - 256; i != m.hist_n; i++) {
					snprintf(w, sizeof(w), " %x", m.hist[i & 255] & 0xffff);
					h += w;
				}
				log("<%07.3f> before:%s\n", ms() / 1000.0, h.c_str());
				m.hist_on = false;
			}
			std::string t;
			char w[8];
			for (unsigned i = 0; i < n; i++) {
				snprintf(w, sizeof(w), " %04x", m.dsp.D[(a + i) & 0xffff]);
				t += w;
			}
			log("<%07.3f> watch (%04x) dp %d pm %d acc %08x%s\n",
			    ms() / 1000.0, m.dsp.top(), m.dsp.dp, m.dsp.pm,
			    (unsigned)m.dsp.acc, t.c_str());
		};
	}
	if (env("DWRITE")) {
		/* TC110_DWRITE=<lo>,<hi>,<from ms>,<n>: n data writes in lo
		   to hi, with the PC */
		unsigned lo = 0, hi = 0;
		long from = 0, n = 0;
		sscanf(env("DWRITE"), "%x,%x,%ld,%ld", &lo, &hi, &from, &n);
		m.dsp.d_wr_lo = lo;
		m.dsp.d_wr_hi = hi;
		m.dsp.d_wr = [this, from, n](unsigned a, unsigned short v) mutable {
			if ((long)ms() < from || n <= 0)
				return;
			n--;
			log("<%07.3f> data %04x = %04x at %d:%04x\n",
			    ms() / 1000.0, a, v, m.page, m.dsp.pc);
		};
	}
	if (env("DXR")) {
		/* TC110_DXR=<from ms>,<n>: n DAC words, with the PC */
		long from = 0, n = 0;
		sscanf(env("DXR"), "%ld,%ld", &from, &n);
		m.on_dxr = [this, from, n](unsigned short v) mutable {
			if ((long)ms() < from || n <= 0)
				return;
			n--;
			log("<%07.3f> dxr %04x at %04x (%04x) n %ld\n",
			    ms() / 1000.0, v, m.dsp.pc, m.dsp.top(), line_n);
		};
	}
	if (env("PWRITE")) {
		/* TC110_PWRITE=<lo>,<hi>: the program words written in
		   lo to hi, once for each PC */
		static std::map<unsigned, int> pw_seen;
		unsigned lo = 0, hi = 0;
		sscanf(env("PWRITE"), "%x,%x", &lo, &hi);
		m.dsp.pr_wr = [this, lo, hi](unsigned a) {
			if (a < lo || a > hi || pw_seen[m.dsp.pc]++)
				return;
			log("<%07.3f> program %04x written at %d:%04x\n",
			    ms() / 1000.0, a, m.page, m.dsp.pc);
		};
	}
	if (env("ROM_UNKNOWN")) {
		/* TC110_ROM_UNKNOWN=1: each ROM word not known that the DSP
		   reads, once */
		static std::map<unsigned, int> seen;
		m.dsp.rom_unknown = [this](unsigned a) {
			unsigned k = a << 16 | m.dsp.pc;
			if (seen[k]++)
				return;
			log("<%07.3f> ROM %04x read (not known) at %d:%04x\n",
			    ms() / 1000.0, a, m.page, m.dsp.pc);
		};
	}
	std::string err;
	if (!m.load(flash, err)) {
		log("%s\n", err.c_str());
		return -1;
	}
	long n = (long)(m.codec_rate / 4);
	for (long k = 0; k < n; k++)
		m.sample(0);
	m.set_dtr(true);
	for (long k = 0; k < n; k++)
		m.sample(0);
	/* (its sign-on, if any, not for the DTE) */
	out_.clear();
	if (m.dsp.stopped || m.rom_miss >= 0) {
		log("the DSP stopped at %04x\n", m.dsp.pc);
		return -1;
	}
	return 0;
}

/* one line sample each way through the resampler, a codec sample each
   way through the modem */
short Tc110Peer::line_sample(short in)
{
	short out = codec_line_sample(in, (double)line_rate / m.codec_rate,
				      [this](short adc) {
		short dac = m.sample(adc);
		if ((m.dsp.stopped || m.rom_miss >= 0) && !stop_shown_) {
			stop_shown_ = true;
			log("<%07.3f> the DSP stopped at %d:%04x\n",
			    ms() / 1000.0, m.page, m.dsp.pc);
			core();
		}
		if (env("CORE_AT") && !core_done_ &&
		    ms() >= (unsigned long long)atol(env("CORE_AT"))) {
			/* TC110_CORE_AT=<ms>: the memory then, and where the
			   DSP is over the next 4000 codec samples */
			core_done_ = true;
			core();
			m.prof = new std::map<unsigned, long>;
			prof_left_ = 4000;
		}
		if (m.prof && --prof_left_ < 0) {
			std::vector<std::pair<long, unsigned>> v;
			for (auto &e : *m.prof)
				v.push_back({ -e.second, e.first });
			std::sort(v.begin(), v.end());
			for (size_t i = 0; i < v.size() && i < 40; i++)
				log("  %d:%04x %ld\n", v[i].second >> 16,
				    v[i].second & 0xffff, -v[i].first);
			delete m.prof;
			m.prof = nullptr;
			log("<%07.3f> pc %d:%04x intm %d imr %04x 6d %04x 39b %04x wait %d\n",
			    ms() / 1000.0, m.page, m.dsp.pc, m.dsp.intm, m.dsp.D[4],
			    m.dsp.D[0x6d], m.dsp.D[0x39b], m.wait_left());
		}
		if (dump_) {
			short s[2] = { adc, dac };
			fwrite(s, sizeof(s), 1, dump_);
		}
		return dac;
	});
	line_n++;
	return out;
}

/* the modem's output to the pty; the DTE's input to the modem, a few
   octets ahead of its UART */
void Tc110Peer::dte_poll()
{
	if (!out_.empty()) {
		show("dte <-", out_.data(), out_.size());
		pty.write(out_.data(), out_.size());
		results.scan(out_.data(), out_.size());
		out_.clear();
	}
	pty.flush();
	if (dte_waits() || m.dte_queued() >= 1)
		return;
	pty.read(1, [this](const void *p, int n) {
		show("dte ->", p, (size_t)n);
		for (int i = 0; i < n; i++)
			m.from_dte(((const unsigned char *)p)[i]);
		return n;
	});
}

/* the DSP's memory to <TC110_CORE>.pr and .d (16 bit words) */
void Tc110Peer::core()
{
	if (!env("CORE"))
		return;
	std::string b = env("CORE");
	FILE *f = fopen((b + ".pr").c_str(), "wb");
	if (f) {
		fwrite(m.dsp.PR.data(), 2, m.dsp.PR.size(), f);
		fclose(f);
	}
	f = fopen((b + ".d").c_str(), "wb");
	if (f) {
		fwrite(m.dsp.D.data(), 2, m.dsp.D.size(), f);
		fclose(f);
	}
}

/* the DTE's traffic logged a line at a time (to its CR or LF) */
void Tc110Peer::show(const char *dir, const void *p, size_t n)
{
	if (!dbg)
		return;
	std::string &t = dir[4] == '<' ? shown_[0] : shown_[1];
	for (size_t i = 0; i < n; i++) {
		unsigned char c = ((const unsigned char *)p)[i];
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

/* the modem on the line until it ends (HalfModem::run) */
int main()
{
	return tc110.run();
}
