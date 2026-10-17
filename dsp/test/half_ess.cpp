/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_ess.cpp - ESS's V.92 soft modem for Windows (es56hpi.sys
 *    5.43.070 of 2003, the WDM driver of the "ESS ES56H-PI Data Fax
 *    Modem": its controller and data pumps, i386) as a test peer
 *    (modem_half.hpp): line audio from fd 3 and to fd 4 (16 bit,
 *    ESS_RATE, 8000 by default), AT commands on a pty whose name is
 *    printed ("created ess: /dev/pts/N"); ESS_SYNC as the others'.
 *
 *    The driver runs on interop_windows.cpp, as half_broadcom's does.
 *    Its hardware is ESS's ES2838 (PCI 125d:2838, hw/es2838.hpp), its
 *    registers 256 ports (BAR 0), its samples through a FIFO the
 *    driver's ISR reads and writes a word at a time.
 *
 *    The driver is test/ess/es56hpi.sys as ESS shipped it (sha256
 *    dc0f3b36...), its INF mdmzn.inf; -DDMODEM_ESS_DIR=<dir> for another
 *    copy, ESS_SYS and ESS_INF for other files.  ESS_DEBUG=1 prints the
 *    driver's calls of note, the codec's rate, the line's levels and the
 *    DTE's traffic, 2 every port access and the driver's own DbgPrint()s,
 *    3 every call the driver makes into the kernel; ESS_DUMP=<file> the
 *    codec's samples (hw/es2838.hpp).
 */

#include <stdio.h>

#include <string>

#include "hw/es2838.hpp"
#include "hw/codec_line.hpp"
#include "hw/pci.hpp"
#include "interop_windows.h"
#include "modem_half.hpp"

using namespace half;

#ifndef ESS_SYS
#define ESS_SYS "es56hpi.sys"
#endif
#ifndef ESS_INF
#define ESS_INF "mdmzn.inf"
#endif

/* the ES2838 at 00:0b.0, its ports and its interrupt */
#define ESS_PORT 0xe800u
#define IRQ      11
#define DEVFN    (0x0b << 3)

class Ess : public HalfModem, public PciDevice<Ess>, public Es2838<Ess>,
	    public CodecLine<Ess> {
public:
	Ess();

	u32 io_in(unsigned a, int size);
	void io_out(unsigned a, u32 v, int size);

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	/* the device and its driver, for interop_windows.cpp */
	win_dev dev = {};
};

static Ess ess;
#define LOG(...) do { if (ess.dbg) ess.log(__VA_ARGS__); } while (0)

/* the device for interop_windows.cpp: the INF's model of it (ESS's own
   board, 125d:2838), its PCI configuration space (ports; communications,
   modem) */
Ess::Ess() :
	HalfModem("ess", "ESS")
{
	pci_devfn = DEVFN;
	pci_cfg[0x00 / 4] = 0x125d | 0x2838u << 16;
	pci_cfg[0x04 / 4] = 0x02800005;
	pci_cfg[0x08 / 4] = 0x07030001;
	pci_cfg[0x10 / 4] = ESS_PORT | 1;
	pci_cfg[0x2c / 4] = 0x125d | 0x2838u << 16;
	pci_cfg[0x3c / 4] = IRQ | 1 << 8;
	pci_bar_size[0] = ES_SIZE;

	dev.sys = ESS_SYS;
	dev.inf = ESS_INF;
	dev.model = "ModemPV";
	dev.port = "COM3";
	dev.devfn = pci_devfn;
	dev.cfg = pci_cfg;
	dev.io_size = ES_SIZE;
	dev.io_in = [this](unsigned a, int size) { return io_in(a, size); };
	dev.io_out = [this](unsigned a, u32 v, int size) { io_out(a, v, size); };
	dev.irq = [this]() { return es_irq(); };
	dev.idle = [this](unsigned n) {
		while (n--)
			line_sample(0);
	};
}

/* a port read of size bytes: the chip's, PCI's configuration ports; all
   ones elsewhere */
u32 Ess::io_in(unsigned a, int size)
{
	u32 v;
	u32 base = pci_cfg[0x10 / 4] & ~3u;
	if (a >= base && a < base + ES_SIZE)
		return es_in(a - base, size);
	if (!pci_io_in(a, size, &v)) {
		LOG("in%d %04x\n", size, a);
		v = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	return v;
}

/* a port write of size bytes, as io_in has them */
void Ess::io_out(unsigned a, u32 v, int size)
{
	u32 base = pci_cfg[0x10 / 4] & ~3u;
	if (a >= base && a < base + ES_SIZE)
		es_out(a - base, v, size);
	else if (!pci_io_out(a, v, size))
		LOG("out%d %04x = %x\n", size, a, v);
}

/* one line sample each way through the resampler (a codec sample each
   way through the FIFO); then the kernel's time (its timers, the ISR) */
short Ess::line_sample(short in)
{
	short out = codec_line_sample(in, (double)line_rate / es_rate(),
				      [this](short adc) { return es_sample(adc); });
	line_n++;
	win_tick();
	return out;
}

/* the modem's output to the pty; the DTE's input to the modem, a write
   at a time; only commands need the fixed timing, so data (online)
   goes in at once */
void Ess::dte_poll()
{
	byte buf[WIN_SERIAL_READ];
	u32 n = win_serial_read(buf);

	if (n) {
		if (dbg) {
			std::string t;
			for (u32 i = 0; i < n; i++)
				t += buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '.';
			log("dte <- %s\n", t.c_str());
		}
		pty.write(buf, n);
		results.scan(buf, n);
	}
	pty.flush();
	if (!win_serial_writable() || dte_waits())
		return;
	pty.read(256, [this](const void *p, int n) {
		int k = win_serial_write(p, n);
		if (dbg && k) {
			std::string t((const char *)p, (size_t)k);
			for (char &c : t)
				if (c < 32 || c > 126)
					c = '.';
			log("dte -> %s\n", t.c_str());
		}
		return k;
	});
}

/* the driver loaded and started, its port opened (as Windows' PnP
   manager and modem.sys do) */
int Ess::start()
{
	win_debug = dbg;
	if (env("SYS"))
		dev.sys = env("SYS");
	if (env("INF"))
		dev.inf = env("INF");
	dev.rate = line_rate;
	/* (ESS_DEBUG=2: the driver's own DbgPrint()s too) */
	if (dbg >= 2)
		dev.addreg = "HKLM,SYSTEM\\CurrentControlSet\\Services\\Edspport,"
			"DebugLevel,0x00010001,0xffff";
	return win_start(&dev);
}

int main()
{
	return ess.run();
}
