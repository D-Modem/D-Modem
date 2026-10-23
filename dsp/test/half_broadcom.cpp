/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_broadcom.cpp - Broadcom's V.92 soft modem for Windows
 *    (BCMSM.sys 3.4.19 of 2002, the WDM driver of the "BCM V.92 56K
 *    Modem": its controller and data pumps, i386) as a test peer
 *    (modem_half.hpp): line audio from fd 3 and to fd 4 (16 bit,
 *    BROADCOM_RATE, 8000 by default), AT commands on a pty whose name is
 *    printed ("created broadcom: /dev/pts/N"); BROADCOM_SYNC as the
 *    others'.
 *
 *    The driver runs on interop_windows.cpp, a Windows kernel of our own
 *    on the line's clock: it loads the driver, starts it on the device
 *    here and opens its port, as Windows' PnP manager and modem.sys
 *    would; the port's reads and writes are the pty's.
 *
 *    Its hardware is Broadcom's BCM4212 (PCI 14e4:4212, hw/bcm4212.hpp;
 *    Dell's subsystem, CodecType 0x15: this build has no other codec; the
 *    AC'97 controllers it looks for on the PCI bus have no code behind
 *    them), its registers a memory BAR the driver maps, its DMA
 *    Broadcom's.  Its line side is a Silicon Labs Si3034 DAA
 *    (hw/si3034.hpp) behind the chip's serial port, whose PLL sets the
 *    codec's rate: a windowed sinc resampler between the codec and the
 *    line (hw/codec_line.hpp).  Its PCI configuration space is
 *    hw/pci.hpp's.
 *
 *    The driver is test/broadcom/BCMSM.sys as Broadcom shipped it (sha256
 *    033c6716...), its INF BCMSMwxp.inf; -DDMODEM_BROADCOM_DIR=<dir> for
 *    another copy, BROADCOM_SYS and BROADCOM_INF for other files.
 *    BROADCOM_DAA=3034 makes the DAA an Si3034 (international: the
 *    driver then sets up the country's registers 16 to 18), 3035 (the
 *    default) an Si3035, which the driver keeps to the USA.
 *    BROADCOM_DEBUG=1 prints the driver's calls of note, the hook, the
 *    rate and the line's levels, 2 every register and port access, 3
 *    every call the driver makes into the kernel.
 */

#include <stdio.h>
#include <string.h>

#include "hw/bcm4212.hpp"
#include "hw/codec_line.hpp"
#include "hw/pci.hpp"
#include "hw/si3034.hpp"
#include "interop_windows.h"
#include "modem_half.hpp"

using namespace half;

#ifndef BROADCOM_SYS
#define BROADCOM_SYS "BCMSM.sys"
#endif
#ifndef BROADCOM_INF
#define BROADCOM_INF "BCMSMwxp.inf"
#endif

/* the BCM4212 at 00:0b.0, its BAR and its interrupt */
#define BCM_PHYS 0xfebf0000u
#define IRQ      11
#define DEVFN    (0x0b << 3)

class Broadcom : public HalfModem, public PciDevice<Broadcom>,
		 public Bcm4212<Broadcom>, public Si3034<Broadcom>,
		 public CodecLine<Broadcom> {
public:
	Broadcom();

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

static Broadcom broadcom;
#define LOG(...) do { if (broadcom.dbg) broadcom.log(__VA_ARGS__); } while (0)

/* the device for interop_windows.cpp: the INF's model of it, its PCI
   configuration space (memory, bus master; communications, other;
   Dell's subsystem 1028:0001) */
Broadcom::Broadcom() :
	HalfModem("broadcom", "BROADCOM")
{
	pci_devfn = DEVFN;
	pci_cfg[0x00 / 4] = 0x14e4 | 0x4212u << 16;
	pci_cfg[0x04 / 4] = 0x02800006;
	pci_cfg[0x08 / 4] = 0x07800002;
	pci_cfg[0x10 / 4] = BCM_PHYS;
	pci_cfg[0x2c / 4] = 0x1028 | 0x0001u << 16;
	pci_cfg[0x3c / 4] = IRQ | 1 << 8;
	pci_bar_size[0] = BCM_SIZE;

	dev.sys = BROADCOM_SYS;
	dev.inf = BROADCOM_INF;
	dev.model = "BCM4211.Modem";
	dev.port = "COM3";
	dev.devfn = pci_devfn;
	dev.cfg = pci_cfg;
	dev.mem_size = BCM_SIZE;
	dev.mem_read = [this](u32 off, int size) { return bcm_read(off, size); };
	dev.mem_write = [this](u32 off, int size, u32 v) { bcm_write(off, size, v); };
	dev.io_in = [this](unsigned a, int size) { return io_in(a, size); };
	dev.io_out = [this](unsigned a, u32 v, int size) { io_out(a, v, size); };
	dev.irq = [this]() { return bcm_irq(); };
	dev.idle = [this](unsigned n) {
		while (n--)
			line_sample(0);
	};
}

/* a port read of size bytes: PCI's configuration ports; all ones
   elsewhere */
u32 Broadcom::io_in(unsigned a, int size)
{
	u32 v;
	if (!pci_io_in(a, size, &v)) {
		LOG("in%d %04x\n", size, a);
		v = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	return v;
}

/* a port write of size bytes, as io_in has them */
void Broadcom::io_out(unsigned a, u32 v, int size)
{
	if (!pci_io_out(a, v, size))
		LOG("out%d %04x = %x\n", size, a, v);
}

/* one line sample each way through the resamplers (a codec sample each
   way through the DMA); then the kernel's time (its timers, the ISR) */
short Broadcom::line_sample(short in)
{
	short out = codec_line_sample(in, (double)line_rate / si3034_rate,
				      [this](short adc) { return bcm_sample(adc); });
	line_n++;
	win_tick();
	return out;
}

/* the modem's output to the pty; the DTE's input to the modem, a write
   at a time; only commands need the fixed timing, so data (online)
   goes in at once */
void Broadcom::dte_poll()
{
	byte buf[WIN_SERIAL_READ];
	u32 n = win_serial_read(buf);

	if (n) {
		pty.write(buf, n);
		results.scan(buf, n);
	}
	pty.flush();
	if (!win_serial_writable() || dte_waits())
		return;
	pty.read(256, win_serial_write);
}

/* the driver loaded and started, its port opened (as Windows' PnP
   manager and modem.sys do) */
int Broadcom::start()
{
	win_debug = dbg;
	if (env("SYS"))
		dev.sys = env("SYS");
	if (env("INF"))
		dev.inf = env("INF");
	if (env("DAA")) {
		if (strcmp(env("DAA"), "3034") && strcmp(env("DAA"), "3035")) {
			fprintf(stderr, "broadcom: BROADCOM_DAA=%s: 3034 or 3035\n",
				env("DAA"));
			return -1;
		}
		si3034_intl = !strcmp(env("DAA"), "3034");
	}
	dev.rate = line_rate;
	return win_start(&dev);
}

/* the Broadcom modem on the line until it ends (HalfModem::run) */
int main()
{
	return broadcom.run();
}
