/*
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
 *    Its hardware is Broadcom's BCM4212 (PCI 14e4:4212, Dell's subsystem,
 *    CodecType 0x15: this build has no other codec; the AC'97 controllers
 *    it looks for on the PCI bus have no code behind them).  Its
 *    registers are a memory BAR the driver maps.  Its DMA is Broadcom's
 *    (the BCM4401 Ethernet's, as Linux's b44 knows it): descriptor rings
 *    for TX and RX, the samples 16 bit at the line side's rate,
 *    interrupts on descriptors' IOC.  Its line side is a Silicon Labs
 *    Si3034 DAA behind a serial port at 0x38/0x3c: its registers, its
 *    PLL for the rate, its hook and loop current.  The codec runs at the
 *    rate the DAA's PLL sets: a windowed sinc resampler between it and
 *    the line (half::CodecLine).
 *
 *    The driver is test/broadcom/BCMSM.sys as Broadcom shipped it (sha256
 *    033c6716...), its INF BCMSMwxp.inf; -DDMODEM_BROADCOM_DIR=<dir> for
 *    another copy, BROADCOM_SYS and BROADCOM_INF for other files.
 *    BROADCOM_DEBUG=1 prints the driver's calls of note, the hook, the
 *    rate and the line's levels, 2 every register and port access, 3
 *    every call the driver makes into the kernel.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "interop_windows.h"
#include "modem_half.hpp"

using namespace half;

#ifndef BROADCOM_SYS
#define BROADCOM_SYS "BCMSM.sys"
#endif
#ifndef BROADCOM_INF
#define BROADCOM_INF "BCMSMwxp.inf"
#endif

/*
 *    the BCM4212 (PCI 14e4:4212) at 00:0b.0, Dell's (subsystem
 *    1028:0001): its registers a memory BAR of 16 KB
 */
#define BCM_PHYS 0xfebf0000u
#define BCM_SIZE 0x4000
#define IRQ      11
#define DEVFN    (0x0b << 3)

/* its DMA descriptors' end of the ring and interrupt on completion */
#define DESC_EOT 0x10000000
#define DESC_IOC 0x20000000

class Broadcom : public HalfModem {
public:
	Broadcom();

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	/* the device and its driver, for interop_windows.cpp */
	PciConfig pci;
	win_dev dev = {};
	u32 regs[BCM_SIZE / 4] = {};

	/* the line side: a Silicon Labs Si3034 DAA behind the chip's
	   serial port, its registers by commands to 0x38 (address in bits
	   8..12, 0x2000 a read, the data in bits 0..7) and the read's
	   result at 0x3c (bit 15: busy, never here) */
	byte daa[32] = {};
	u32 daa_data = 0;
	unsigned codec_rate = 8000;
	bool off_hook = false;

	/* its DMA: the current descriptor of each channel (its offset), the
	   bytes of it done */
	u32 dma_cur[2] = {}, dma_pos[2] = {};

	/* (BROADCOM_DEBUG: the peaks each way, each second) */
	int lv_n = 0, lv_in = 0, lv_out = 0;

	void daa_rate();
	void daa_cmd(u32 v);
	bool dma_run(int c) const;
	bool dma_idle(int c) const;
	u32 dma_status(int c) const;
	short dma_sample(int c, short in);
	short codec_sample(short adc);
	u32 bcm_read(u32 off, int size);
	void bcm_write(u32 off, int size, u32 v);
	bool bcm_irq() const;
};

static Broadcom broadcom;
#define LOG(...) do { if (broadcom.dbg) broadcom.log(__VA_ARGS__); } while (0)

/* the device for interop_windows.cpp: the INF's model of it, its PCI
   configuration space (memory, bus master; communications, other) */
Broadcom::Broadcom() :
	HalfModem("broadcom", "BROADCOM"),
	pci(*this)
{
	pci.devfn = DEVFN;
	pci.cfg[0x00 / 4] = 0x14e4 | 0x4212u << 16;
	pci.cfg[0x04 / 4] = 0x02800006;
	pci.cfg[0x08 / 4] = 0x07800002;
	pci.cfg[0x10 / 4] = BCM_PHYS;
	pci.cfg[0x2c / 4] = 0x1028 | 0x0001u << 16;
	pci.cfg[0x3c / 4] = IRQ | 1 << 8;

	dev.sys = BROADCOM_SYS;
	dev.inf = BROADCOM_INF;
	dev.model = "BCM4211.Modem";
	dev.port = "COM3";
	dev.pci = &pci;
	dev.mem_size = BCM_SIZE;
	dev.mem_read = [this](u32 off, int size) { return bcm_read(off, size); };
	dev.mem_write = [this](u32 off, int size, u32 v) { bcm_write(off, size, v); };
	dev.irq = [this]() { return bcm_irq(); };
	dev.idle = [this](unsigned n) {
		while (n--)
			line_sample(0);
	};
}

/*
 *    the DAA
 */

/* its sample rate from its PLL (registers 7 to 9: N1 - 1, M1 - 1, and
   N2 - 1, M2 - 1 in the high and low nibbles), on a 4 MHz clock:
   4 MHz M1 M2 / (N1 N2) / 5120 */
void Broadcom::daa_rate()
{
	double f = 4000000.0 * (daa[8] + 1) / (daa[7] + 1) *
		((daa[9] & 15) + 1) / ((daa[9] >> 4) + 1) / 5120;
	unsigned r = (unsigned)lrint(f);
	if (r >= 4000 && r <= 48000 && r != codec_rate) {
		LOG("daa rate %u Hz\n", r);
		codec_rate = r;
	}
}

/* a command to its serial port: a register read (its value for 0x3c)
   or write */
void Broadcom::daa_cmd(u32 v)
{
	int r = (v >> 8) & 0x1f;
	if (v & 0x2000) {
		daa_data = daa[r];
		if (r == 12)            /* line side: frame detect, loop current
					   (half the scale: 1111 is an overload) */
			daa_data = 0x40 | (off_hook ? 0x08 : 0);
		if (dbg > 1)
			log("daa %2d r = %02x\n", r, daa_data);
		return;
	}
	if (dbg > 1 || (dbg && r != 5 && daa[r] != (byte)v))
		log("daa %2d w = %02x\n", r, v & 0xff);
	daa[r] = (byte)v;
	if (r >= 7 && r <= 9)
		daa_rate();
	if (r == 5 && (bool)(v & 1) != off_hook) {
		off_hook = v & 1;
		LOG("%s hook\n", off_hook ? "off" : "on");
	}
}

/*
 *    its DMA: two channels at 0x200 and 0x210 (enable, ring's bus
 *    address, its PTR, status: state in bits 12..15, the position
 *    below), running while the control register (0) has 0x60; the
 *    interrupt status at 0x20 (written back to clear), its enables at
 *    0x24
 */

bool Broadcom::dma_run(int c) const
{
	return (regs[0] & 0x60) && (regs[(0x200 + 0x10 * c) / 4] & 1);
}

/* channel c's descriptors posted: those before its PTR (0x208, 0x218) */
bool Broadcom::dma_idle(int c) const
{
	return dma_cur[c] == (regs[(0x208 + 0x10 * c) / 4] & 0xfff);
}

u32 Broadcom::dma_status(int c) const
{
	return dma_run(c) ? (dma_idle(c) ? 0x2000 : 0x1000) | (dma_cur[c] & 0xfff) : 0;
}

/* channel c's next sample: TX (0) the one it reads, RX (1) the one it
   writes; its descriptors' interrupts, the ring's end */
short Broadcom::dma_sample(int c, short in)
{
	u32 *ring = (u32 *)(uintptr_t)regs[(0x204 + 0x10 * c) / 4], *d;
	short *buf, out = 0;
	u32 len;
	if (!dma_run(c) || !ring || dma_idle(c))
		return 0;
	d = ring + dma_cur[c] / 4;
	len = d[0] & 0x1fff;
	buf = (short *)(uintptr_t)d[1];
	if (dma_pos[c] + 2 <= len) {
		if (c)
			buf[dma_pos[c] / 2] = in;
		else
			out = buf[dma_pos[c] / 2];
	}
	dma_pos[c] += 2;
	if (dma_pos[c] >= len) {
		dma_pos[c] = 0;
		if (d[0] & DESC_IOC)
			regs[0x20 / 4] |= c ? 0x10000 : 0x1000000;
		dma_cur[c] = d[0] & DESC_EOT ? 0 : (dma_cur[c] + 8) & 0xfff;
	}
	return out;
}

/*
 *    its registers
 */

/* a register read of size bytes at off: what the device has there */
u32 Broadcom::bcm_read(u32 off, int size)
{
	u32 v;
	if ((off & ~3u) == 0x3c)
		regs[0x3c / 4] = daa_data;
	if ((off & ~3u) == 0x20c)
		regs[0x20c / 4] = dma_status(0);
	if ((off & ~3u) == 0x21c)
		regs[0x21c / 4] = dma_status(1);
	v = regs[(off & (BCM_SIZE - 1)) / 4] >> (8 * (off & 3));
	if (size == 1)
		v &= 0xff;
	else if (size == 2)
		v &= 0xffff;
	return v;
}

/* a register write */
void Broadcom::bcm_write(u32 off, int size, u32 v)
{
	u32 *r = &regs[(off & (BCM_SIZE - 1)) / 4];
	u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	int sh = 8 * (off & 3);
	u32 old = *r;
	if ((off & ~3u) == 0x20) {             /* (written back: cleared) */
		*r &= ~((v & mask) << sh);
		return;
	}
	*r = (*r & ~(mask << sh)) | (v & mask) << sh;
	if (off == 0x38)
		daa_cmd(*r);
	if ((off == 0x200 || off == 0x210) && !(*r & 1)) {
		dma_cur[(off - 0x200) / 0x10] = 0;
		dma_pos[(off - 0x200) / 0x10] = 0;
	}
	if (off == 0 && (old & 0x60) != (*r & 0x60)) {
		dma_cur[0] = dma_cur[1] = dma_pos[0] = dma_pos[1] = 0;
		LOG("dma %s\n", *r & 0x60 ? "on" : "off");
		if (dbg && (*r & 0x60)) {
			for (int c = 0; c < 2; c++) {
				u32 *d = (u32 *)(uintptr_t)regs[(0x204 + 0x10 * c) / 4];
				int i;
				log("ring %d at %p:", c, (void *)d);
				for (i = 0; i < 10; i++)
					fprintf(stderr, " %08x/%08x", d[2 * i], d[2 * i + 1]);
				for (i = 0; i < 512 && !(d[2 * i] & DESC_EOT); i++)
					;
				fprintf(stderr, " ... EOT at %d\n", i);
			}
		}
	}
}

/* its interrupt: a status bit it enables */
bool Broadcom::bcm_irq() const
{
	return (regs[0x20 / 4] & regs[0x24 / 4]) != 0;
}

/*
 *    the line
 */

/* one codec sample each way through the DMA; the DAC's */
short Broadcom::codec_sample(short adc)
{
	short dac = dma_sample(0, 0);
	dma_sample(1, adc);
	if (dbg) {
		if (abs(adc) > lv_in)
			lv_in = abs(adc);
		if (abs(dac) > lv_out)
			lv_out = abs(dac);
		if (++lv_n == (int)codec_rate) {
			log("levels: in %d out %d (tx %03x/%03x rx %03x)\n", lv_in, lv_out,
			    dma_cur[0], regs[0x208 / 4] & 0xfff, dma_cur[1]);
			lv_n = lv_in = lv_out = 0;
		}
	}
	return dac;
}

/* one line sample each way through the resamplers; then the kernel's
   time (its timers, the ISR) */
short Broadcom::line_sample(short in)
{
	short out = codec.sample(in, (double)line_rate / codec_rate,
				 [this](short adc) { return codec_sample(adc); });
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
	dev.rate = line_rate;
	return win_start(&dev);
}

int main()
{
	return broadcom.run();
}
