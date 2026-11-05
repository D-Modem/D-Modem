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
 *    COURIER_DUMP=<file>: the codec's samples, 16 bit pairs of the ADC's
 *    and the DAC's, at the codec's rate.
 *
 *    COURIER_DEBUG=1 prints the DTE's traffic, the hook and the codec's
 *    rate, 2 the supervisor's messages to the DSP and the DSP's to it
 *    too, 3 the CPU's interrupts as well; COURIER_HW_DEBUG=<mask> what
 *    hw/courier.hpp logs (its dbg).
 */

#include <stdio.h>
#include <stdlib.h>

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
	if (env("DUMP") && !(dump_ = fopen(env("DUMP"), "wb"))) {
		log("%s: cannot write\n", env("DUMP"));
		return -1;
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
		if (!m.off_hook)
			dac = 0;
		if (dump_) {
			short s[2] = { adc, dac };
			fwrite(s, sizeof(s), 1, dump_);
		}
		return dac;
	});
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
