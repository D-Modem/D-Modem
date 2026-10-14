/*
 *    half_broadcom.c - Broadcom's V.92 soft modem for Windows (BCMSM.sys
 *    3.4.19 of 2002, the WDM driver of the "BCM V.92 56K Modem": its
 *    controller and data pumps, i386) as a test peer, as half_sm56 does
 *    for Motorola's Linux module: line audio from fd 3 and to fd 4 (16
 *    bit, BROADCOM_RATE, 8000 by default), AT commands on a pty whose
 *    name is printed ("created broadcom: /dev/pts/N"); BROADCOM_SYNC as
 *    half_sm56's SM56_SYNC.
 *
 *    The driver runs on interop_windows.c, a Windows kernel of our own
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
 *    the line.
 *
 *    The driver is test/broadcom/BCMSM.sys as Broadcom shipped it (sha256
 *    033c6716...), its INF BCMSMwxp.inf; -DDMODEM_BROADCOM_DIR=<dir> for
 *    another copy, BROADCOM_SYS and BROADCOM_INF for other files.
 *    BROADCOM_DEBUG=1 prints the driver's calls of note, the hook, the
 *    rate and the line's levels, 2 every register and port access, 3
 *    every call the driver makes into the kernel.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "half_dte.h"
#include "interop_windows.h"

static int dbg;
#define LOG(fmt, ...) do { if (dbg) fprintf(stderr, "broadcom: " fmt, ##__VA_ARGS__); } while (0)

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
static u32 bcm_reg[BCM_SIZE / 4];

/*
 *    the line side: a Silicon Labs Si3034 DAA behind the chip's serial
 *    port, its registers by commands to 0x38 (address in bits 8..12,
 *    0x2000 a read, the data in bits 0..7) and the read's result at
 *    0x3c (bit 15: busy, never here)
 */
static byte daa[32];
static u32 daa_data;
static unsigned codec_rate = 8000;
static int off_hook;

/* its sample rate from its PLL (registers 7 to 9: N1 - 1, M1 - 1, and
   N2 - 1, M2 - 1 in the high and low nibbles), on a 4 MHz clock:
   4 MHz M1 M2 / (N1 N2) / 5120 */
static void daa_rate(void)
{
	double f = 4000000.0 * (daa[8] + 1) / (daa[7] + 1) *
		((daa[9] & 15) + 1) / ((daa[9] >> 4) + 1) / 5120;
	unsigned r = (unsigned)lrint(f);
	if (r >= 4000 && r <= 48000 && r != codec_rate) {
		LOG("daa rate %u Hz\n", r);
		codec_rate = r;
	}
}

static void daa_cmd(u32 v)
{
	int r = (v >> 8) & 0x1f;
	if (v & 0x2000) {
		daa_data = daa[r];
		if (r == 12)            /* line side: frame detect, loop current
					   (half the scale: 1111 is an overload) */
			daa_data = 0x40 | (off_hook ? 0x08 : 0);
		if (dbg > 1)
			fprintf(stderr, "broadcom: daa %2d r = %02x\n", r, daa_data);
		return;
	}
	if (dbg > 1 || (dbg && r != 5 && daa[r] != (byte)v))
		fprintf(stderr, "broadcom: daa %2d w = %02x\n", r, v & 0xff);
	daa[r] = (byte)v;
	if (r >= 7 && r <= 9)
		daa_rate();
	if (r == 5 && (int)(v & 1) != off_hook) {
		off_hook = v & 1;
		LOG("%s hook\n", off_hook ? "off" : "on");
	}
}

/*
 *    its DMA: two channels at 0x200 and 0x210 (enable, buffer's bus
 *    address, its size, status: state in bits 12..15, the position
 *    below), running while the control register (0) has 0x60; the
 *    interrupt status at 0x20 (written back to clear), its enables at
 *    0x24
 */
static u32 dma_cur[2], dma_pos[2];      /* the current descriptor (its
					   offset), the bytes of it done */
static int dma_run(int c)
{
	return (bcm_reg[0] & 0x60) && (bcm_reg[(0x200 + 0x10 * c) / 4] & 1);
}
/* channel c's descriptors posted: those before its PTR (0x208, 0x218) */
static int dma_idle(int c)
{
	return dma_cur[c] == (bcm_reg[(0x208 + 0x10 * c) / 4] & 0xfff);
}
static u32 dma_status(int c)
{
	return dma_run(c) ? (dma_idle(c) ? 0x2000 : 0x1000) | (dma_cur[c] & 0xfff) : 0;
}

/* channel c's next sample: TX (0) the one it reads, RX (1) the one it
   writes; its descriptors' interrupts, the ring's end */
#define DESC_EOT 0x10000000
#define DESC_IOC 0x20000000
static short dma_sample(int c, short in)
{
	u32 *ring = (u32 *)(uintptr_t)bcm_reg[(0x204 + 0x10 * c) / 4], *d;
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
			bcm_reg[0x20 / 4] |= c ? 0x10000 : 0x1000000;
		dma_cur[c] = d[0] & DESC_EOT ? 0 : (dma_cur[c] + 8) & 0xfff;
	}
	return out;
}

/* a register read of size bytes at off: what the device has there */
static u32 bcm_read(u32 off, int size)
{
	u32 v;
	if ((off & ~3u) == 0x3c)
		bcm_reg[0x3c / 4] = daa_data;
	if ((off & ~3u) == 0x20c)
		bcm_reg[0x20c / 4] = dma_status(0);
	if ((off & ~3u) == 0x21c)
		bcm_reg[0x21c / 4] = dma_status(1);
	v = bcm_reg[(off & (BCM_SIZE - 1)) / 4] >> (8 * (off & 3));
	if (size == 1)
		v &= 0xff;
	else if (size == 2)
		v &= 0xffff;
	return v;
}

/* a register write */
static void bcm_write(u32 off, int size, u32 v)
{
	u32 *r = &bcm_reg[(off & (BCM_SIZE - 1)) / 4];
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
			int c, i;
			for (c = 0; c < 2; c++) {
				u32 *d = (u32 *)(uintptr_t)bcm_reg[(0x204 + 0x10 * c) / 4];
				fprintf(stderr, "broadcom: ring %d at %p:", c, (void *)d);
				for (i = 0; i < 10; i++)
					fprintf(stderr, " %08x/%08x", d[2 * i], d[2 * i + 1]);
				for (i = 0; i < 512 && !(d[2 * i] & 0x10000000); i++)
					;
				fprintf(stderr, " ... EOT at %d\n", i);
			}
		}
	}
}

/* its interrupt: a status bit it enables */
static int bcm_irq(void)
{
	return (bcm_reg[0x20 / 4] & bcm_reg[0x24 / 4]) != 0;
}

static void line_idle(unsigned n);

/* the device and its driver, for interop_windows.c: the INF's model of
   it, its PCI configuration space (memory, bus master; communications,
   other) */
static struct win_dev dev = {
	.sys = BROADCOM_SYS, .inf = BROADCOM_INF, .model = "BCM4211.Modem",
	.port = "COM3",
	.devfn = DEVFN,
	.cfg = {
		[0x00 / 4] = 0x14e4 | 0x4212u << 16,
		[0x04 / 4] = 0x02800006,
		[0x08 / 4] = 0x07800002,
		[0x10 / 4] = BCM_PHYS,
		[0x2c / 4] = 0x1028 | 0x0001u << 16,
		[0x3c / 4] = IRQ | 1 << 8,
	},
	.mem_size = BCM_SIZE, .mem_read = bcm_read, .mem_write = bcm_write,
	.irq = bcm_irq,
	.rate = 8000, .idle = line_idle,
};

/*
 *    the DTE: a pty, the port's reads and writes
 */

static int pty;
static long sync_n;
static int online;

static void result_scan(unsigned char c)
{
	static const char *const pat[] = { "CONNECT", "NO CARRIER",
					   "\r\nOK\r\n" };
	static unsigned at[3];
	int i;

	for (i = 0; i < 3; i++) {
		if (c != (unsigned char)pat[i][at[i]])
			at[i] = c == (unsigned char)pat[i][0];
		else
			at[i]++;
		if (!pat[i][at[i]]) {
			at[i] = 0;
			online = i == 0;
		}
	}
}

/* the modem's output to the pty; the DTE's input to the modem, a write
   at a time (with BROADCOM_SYNC, in command state, every sync_n line
   samples) */
static void dte_poll(void)
{
	byte buf[WIN_SERIAL_READ];
	u32 k, n = win_serial_read(buf);

	if (n) {
		dte_write(pty, buf, n);
		for (k = 0; k < n; k++)
			result_scan(buf[k]);
	}
	dte_flush(pty);
	if (!win_serial_writable())
		return;
	if (sync_n && win_line_n % sync_n && !online)
		return;
	dte_read(pty, 256, win_serial_write);
}

/*
 *    the codec's samples: a windowed sinc resampler each way between the
 *    line's rate and the codec's
 */

#define RS_TAPS 32
struct rs {
	float hist[RS_TAPS];    /* the newest last */
};
static float rs_at(const float *h, double f, double cut)
{
	double y = 0;
	int k;
	for (k = 0; k < RS_TAPS; k++) {
		double t = k - (RS_TAPS / 2 - 1) - f, x = M_PI * t * cut;
		double w = fabs(t) < RS_TAPS / 2 ?
			0.5 + 0.5 * cos(M_PI * t / (RS_TAPS / 2)) : 0;
		y += h[k] * cut * (fabs(x) < 1e-9 ? 1 : sin(x) / x) * w;
	}
	return (float)y;
}
static void rs_push(struct rs *r, float x)
{
	memmove(r->hist, r->hist + 1, (RS_TAPS - 1) * sizeof(float));
	r->hist[RS_TAPS - 1] = x;
}

static struct rs up, down;

/* the codec's input noise (BROADCOM_NOISE, rms; 5 by default): a line
   is never digitally silent */
static float noise_rms = 5;
static float noise(void)
{
	static u32 x = 1;
	x = x * 1664525u + 1013904223u;
	return noise_rms * 3.4641f * ((float)(x >> 8) / (1 << 24) - 0.5f);
}
static double up_ph;

static short sat(double x)
{
	return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrint(x));
}

/* one codec sample each way through the DMA */
static void codec_sample(short adc)
{
	short dac = dma_sample(0, 0);
	dma_sample(1, adc);
	if (dbg) {
		/* (each second: the peaks each way) */
		static int n, pin, pout;
		if (abs(adc) > pin) pin = abs(adc);
		if (abs(dac) > pout) pout = abs(dac);
		if (++n == (int)codec_rate) {
			LOG("levels: in %d out %d (tx %03x/%03x rx %03x)\n", pin, pout,
			    dma_cur[0], bcm_reg[0x208 / 4] & 0xfff, dma_cur[1]);
			n = pin = pout = 0;
		}
	}
	rs_push(&down, dac);
}

/* one line sample each way through the resamplers; then the kernel's
   time (its timers, the ISR) */
static short line_sample(short in)
{
	double r = (double)dev.rate / codec_rate;
	double out;

	rs_push(&up, in + noise());
	while (up_ph < 1) {
		codec_sample(sat(rs_at(up.hist, up_ph, r > 1 ? 1 / r : 1)));
		up_ph += r;
	}
	up_ph -= 1;
	out = rs_at(down.hist, 1 - up_ph / r, r < 1 ? r : 1);
	win_tick();
	return sat(out);
}

/* the line run on n samples (silent) while the kernel waits */
static void line_idle(unsigned n)
{
	while (n--)
		line_sample(0);
}

/*
 *    main
 */

int main(int argc, char **argv)
{
	short in[480], out[480];
	char *name;
	int i, n;

	(void)argc; (void)argv;
	dbg = getenv("BROADCOM_DEBUG") ? atoi(getenv("BROADCOM_DEBUG")) : 0;
	win_debug = dbg;
	if (getenv("BROADCOM_RATE"))
		dev.rate = atoi(getenv("BROADCOM_RATE"));
	if (getenv("BROADCOM_NOISE"))
		noise_rms = (float)atof(getenv("BROADCOM_NOISE"));
	sync_n = getenv("BROADCOM_SYNC") ? (atol(getenv("BROADCOM_SYNC")) + 47) / 48 * 48 : 0;
	if (getenv("BROADCOM_SYS"))
		dev.sys = getenv("BROADCOM_SYS");
	if (getenv("BROADCOM_INF"))
		dev.inf = getenv("BROADCOM_INF");
	setvbuf(stdout, NULL, _IONBF, 0);

	pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty < 0 || grantpt(pty) || unlockpt(pty)) {
		perror("pty");
		return 1;
	}
	{
		struct termios t;
		tcgetattr(pty, &t);
		cfmakeraw(&t);
		tcsetattr(pty, TCSANOW, &t);
	}
	fcntl(pty, F_SETFL, O_NONBLOCK);
	name = ptsname(pty);

	/* the driver loaded and started, its port opened (as Windows'
	   PnP manager and modem.sys do) */
	if (win_start(&dev))
		return 1;
	printf("created broadcom: %s\n", name);

	memset(out, 0, sizeof(out));
	if (write(4, out, 48 * 2 * 4) < 0) {
		perror("broadcom: the line (fd 4)");
		return 1;
	}
	for (;;) {
		static int odd;
		n = (int)read(3, (char *)in + odd, sizeof(in) - odd);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		n += odd;
		odd = n & 1;
		n /= 2;
		for (i = 0; i < n; i++) {
			out[i] = line_sample(in[i]);
			if (win_line_n % 48 == 0)
				dte_poll();
		}
		if (write(4, out, n * 2) != n * 2)
			break;
		if (odd)
			((char *)in)[0] = ((char *)in)[n * 2];
		if (!sync_n)
			dte_poll();
	}
	return 0;
}
