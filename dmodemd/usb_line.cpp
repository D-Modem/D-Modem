/*
 * SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause
 * Copyright (C) 2026 Neo-Desktop
 *
 *    usb_line.cpp - the modem's line a Smart Link SmartUSB56 (ST7554)
 *                   or a pico-serial Line stick through libusb
 *                   (usb_line.h)
 *
 *    The ST7554's registers, their values and its start-up are those of
 *    Smart Link's Linux driver for it (drivers/st7554.c of slmodem
 *    2.9.11), whose licence asks for this notice:
 *
 *      Copyright (c) 2002, Smart Link Ltd.
 *      All rights reserved.
 *
 *      Redistribution and use in source and binary forms, with or
 *      without modification, are permitted provided that the following
 *      conditions are met:
 *
 *          1. Redistributions of source code must retain the above
 *             copyright notice, this list of conditions and the
 *             following disclaimer.
 *          2. Redistributions in binary form must reproduce the above
 *             copyright notice, this list of conditions and the
 *             following disclaimer in the documentation and/or other
 *             materials provided with the distribution.
 *          3. Neither the name of the Smart Link Ltd. nor the names of
 *             its contributors may be used to endorse or promote
 *             products derived from this software without specific
 *             prior written permission.
 *
 *      THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *      CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *      INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *      MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *      DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *      BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *      EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *      TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *      DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *      ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *      OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 *      OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 *      OF SUCH DAMAGE.
 *
 *    The rest is ours: the transfers on libusb, the line for the core.
 *    Caller ID (GPIO 5, CID1, while the core snoops on hook) and the
 *    ST75951/2 silicon DAA's start-up (its control words) are what
 *    Smart Link's Windows driver does (docs/smartusb56-windows.md).
 *
 *    Interface 0, alternate setting 1: an interrupt IN endpoint (4
 *    octets: the FIFO's status, the GPIOs' changes), an isochronous OUT
 *    and an isochronous IN one.  ISO_TRANSFERS of ISO_PACKETS packets
 *    (1 ms each) are in flight each way.  An OUT transfer goes when an
 *    IN one is done, with that one's packets' lengths, so the playback
 *    keeps the codec's clock; it takes the modem's samples queued
 *    (silence if there are too few: the queue starts a frame and a
 *    half ahead).
 *
 *    The pico-serial Line stick's protocol is its firmware's
 *    (firmware/PROTOCOL.md "Line stick", github.com/Neo-Desktop/
 *    pico-serial): the same transfers on interface 0, alternate setting
 *    1 (selecting it starts the samples, its rate set before), 8-octet
 *    events on the interrupt endpoint, vendor requests for the rest.
 *    The country's line settings (si3056.h) go into its DAA's registers
 *    16, 26, 30, 31 and 59 (DAA_READ, then DAA_WRITE), at start-up and
 *    when the country changes (+GCI), on hook.
 *
 */

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#ifndef _WIN32
#include <poll.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>

#include <g711.h>
#include <log.h>

#include "dm_types.h"
#include "usb_line.h"

/* (to the log when it has a hook, as the app's: its stderr is lost) */
#define ERR(fmt, ...) do { \
	if (dcore::log_hooked()) \
		dcore::log_printf(0, "usb: error: " fmt, ##__VA_ARGS__); \
	else \
		fprintf(stderr, "error: " fmt, ##__VA_ARGS__); \
} while (0)
#define DBG(fmt, ...) dcore::log_printf(1, "usb: " fmt, ##__VA_ARGS__)

#ifdef _WIN32
/* WinUSB streams (isochronous transfers) from Windows 8.1 on: its
   winusb.dll has WinUsb_ReadIsochPipeAsap then */
static bool winusb_streams()
{
	HMODULE m = LoadLibraryA("winusb.dll");
	bool yes = m && GetProcAddress(m, "WinUsb_ReadIsochPipeAsap") != nullptr;
	if (m)
		FreeLibrary(m);
	return yes;
}

/* libusb's backend on ctx: UsbDk where it is installed and WinUSB can't
   stream (Windows 7 and 8), else WinUSB's (which drives libusbK's
   devices too: Zadig's driver for those Windows).  DMODEM_USBDK=1 takes
   UsbDk whatever the Windows, =0 never.  True if UsbDk it is */
static bool use_usbdk(libusb_context *ctx)
{
	const char *e = getenv("DMODEM_USBDK");
	if (e && *e == '0')
		return false;
	if (!(e && *e == '1') && winusb_streams())
		return false;
	return libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK) ==
		LIBUSB_SUCCESS;
}

#endif

/* what to do when a transfer fails with err: on Windows, a driver that
   can't stream (WinUSB before 8.1); "" */
static const char *no_streams(int err)
{
#ifdef _WIN32
	if (err == LIBUSB_ERROR_NOT_SUPPORTED)
		return " (WinUSB streams only from Windows 8.1 on: on "
			"Windows 7 and 8 give the modem libusbK with Zadig, "
			"or install UsbDk)";
#else
	(void)err;
#endif
	return "";
}

namespace dmd {

namespace {

/* the ST7554 */
const u16 ST7554_VENDOR = 0x0483;
const u16 ST7554_PRODUCT = 0x7554;

/* the pico-serial Line stick (a pid.codes test PID until it has its own) */
const u16 STICK_VENDOR = 0x1209;
const u16 STICK_PRODUCT = 0x0002;
/* its vendor requests (firmware/src/core/line_proto.h) */
const u8 LP_GET_INFO = 0x01;
const u8 LP_GET_STATUS = 0x02;
const u8 LP_SET_RATE = 0x10;
const u8 LP_SET_HOOK = 0x11;
const u8 LP_HOOK_FLASH = 0x12;
const u8 LP_SET_MONITOR = 0x13;
const u8 LP_DAA_READ = 0x20;
const u8 LP_DAA_WRITE = 0x21;
/* its state bits, and the Si3056's interrupt sources it passes on */
const u8 LP_ST_LINK = 0x01;
const u8 LP_INT_OVERLOAD = 0x40;
const u8 LP_INT_LINK_LOST = 0x20;
const u8 LP_INT_DROPOUT = 0x08;
const u8 LP_INT_POLARITY = 0x01;
/* its delay: the playback's head start, the Si3056's filters both ways */
const s16 STICK_FIFO_SAMPLES = 32 + 12 + 12;

/* the Si3056's rates (register 7's) */
bool stick_rate(u32 rate)
{
	static const u32 rates[] = { 7200, 8000, 8229, 8400, 9000, 9600,
					  10286, 12000, 13714, 16000 };
	for (u32 r : rates)
		if (r == rate)
			return true;
	return false;
}

/* its vendor requests (registers) */
const u8 REVISION = 0x00;
const u8 SSI1_CONTROL = 0x01;
const u8 SSI2_CONTROL = 0x02;
const u8 FIFO_MASK = 0x03;
const u8 SSI1_COUNTER = 0x04;
const u8 SSI2_COUNTER = 0x06;
const u8 GPIO_DIR = 0x08;
const u8 GPIO_OUT = 0x0a;
const u8 GPIO_MASK = 0x0c;
const u8 GPIO_INV = 0x0e;
const u8 GPIO_STATUS = 0x15;
const u8 SSI1_CWORD = 0x17;
const u8 FIFO_CONTROL1 = 0x19;
const u8 FIFO_CONTROL2 = 0x1a;

/* their values */
const u16 SSI1_POWERDOWN = 0x0000;
const u16 SSI1_POWERUP = 0x0001;
const u16 SSI1_ST75951 = 0x0002;           /* kept set on an ST75951 */
const u16 SSI1_UNDERRUN = 0x0040;
const u16 SSI1_OVERRUN = 0x0020;
/* the voice codec's (SSI2): its power, as Windows' driver has it
   (KSGSCodecInterface::Power), and its FIFO's errors, the SSI1 bits'
   counterparts (EnableFIFOInterrupts, TranslateInterruptData) */
const u16 SSI2_POWERUP = 0x0021;
const u16 SSI2_UNDERRUN = 0x0004;
const u16 SSI2_OVERRUN = 0x0002;
/* its rate (the only one: K7554VoiceCodecInterface::SetSampleRate), and
   its period, 5 ms (SetPTCPeriod: the rate times the ms, less 1) */
const u32 VOICE_RATE = 8000;
const u16 VOICE_PERIOD = VOICE_RATE * 5 / 1000 - 1;
/* the alternate settings: the codec's streams, and the voice codec's too */
const s16 ALT_MODEM = 1, ALT_VOICE = 3;
const u16 GPIO_RING1 = 0x0001;
const u16 GPIO_HANDSET = 0x0004;           /* the PHONE jack's off hook */
const u16 GPIO_HOOK1 = 0x0008;
const u16 GPIO_CID1 = 0x0020;             /* the codec on the line on hook */
const u16 GPIO_LED_CARRIER = 0x0040;       /* --usb-leds */
const u16 GPIO_LED_DATA = 0x2000;
/* --usb-international's: Smart Link's names (st7554.c) */
const u16 GPIO_RFC = 0x0100;
const u16 GPIO_DP = 0x0400;
const u16 GPIO_DC = 0x1000;
/* --usb-buzzer's: the speaker's buzzer (Windows' BUZEN) */
const u16 GPIO_BUZEN = 0x0800;
/* the Australian timer: RFC after an off hook, ms */
const u16 AUSTRALIAN_MS = 350;

/* a equals b, case aside */
bool same_name(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a | 0x20) != (*b | 0x20))
			return false;
	return *a == *b;
}
const u16 GPIO_LED_HOOK = 0x0080;
const u16 GPIO_MONOLITHIC = 0x4000;        /* an ST75951/2 silicon DAA */
const u16 BYTE_ORDER_LE = 0x8000;
/* FIFO_CONTROL2: both FIFOs' high thresholds (Linux's 0x28), "RHOM"
   for the codec (bit 15) and the voice codec (bit 7), and the first
   revision that has it (Windows' InitializeRegisters) */
const u16 FIFO2_THRESHOLDS = 0x2828;
const u16 RHOM_SSI1 = 0x8000, RHOM_SSI2 = 0x0080;
const u16 RHOM_REVISION = 0x0200;

/* the FIFO's start: its count, and the octets of its own (and the
   device's) delay */
const u16 FRAGSIZE = 96;
const s16 FIFO_SAMPLES = 64 + 24;

const u16 TIMEOUT_MS = 1000;

/* the Agere modem's hook: its codec's register (0 off hook, 1 on) */

/* the configuration's remote wakeup bit; USB's DEVICE_REMOTE_WAKEUP */
const u8 CONFIG_REMOTE_WAKEUP = 0x20;
const u16 FEATURE_REMOTE_WAKEUP = 1;

/* the system's clock, in ms (for the retries of a suspended device) */
u64 clock_ms()
{
	using namespace std::chrono;
	return static_cast<u64>(duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count());
}

/* the codec's control word for a rate (the STLC7550's, and the
   ST75951's register 0); 0 if it has none */
u16 cword(u32 rate)
{
	switch (rate) {
	case 8000: return 0x3c8;
	case 9600: return 0x3c0;
	case 16000: return 0x3f0;
	}
	return 0;
}

/* in's n samples at rate from appended to out at rate to, by linear
   interpolation (between the two codecs' rates: the voice codec's and
   the line's); pos (the phase) and last (the sample before in's first)
   kept for the next ones */
void resample(const s16 *in, size_t n, u32 from, u32 to,
	      double &pos, s16 &last, std::deque<s16> &out)
{
	double step = static_cast<double>(from) / to;
	/* (t: 0 at last, k + 1 at in[k]) */
	for (; n && pos < static_cast<double>(n); pos += step) {
		size_t i = static_cast<size_t>(pos);
		double f = pos - static_cast<double>(i);
		double a = i ? in[i - 1] : last, b = in[i];
		out.push_back(static_cast<s16>(a + (b - a) * f));
	}
	if (n) {
		pos -= static_cast<double>(n);
		last = in[n - 1];
	}
}

/* a + b, within 16 bits */
s16 mix(s32 a, s32 b)
{
	s32 x = a + b;
	return static_cast<s16>(x > 32767  ? 32767 :
				    x < -32768 ? -32768 :
						 x);
}

} // namespace

/* the device closed (close()) */
UsbLine::~UsbLine()
{
	close();
}

/* see usb_line.h */
bool UsbLine::parse_dev(const char *dev, int *bus, int *addr)
{
	char end;
	return sscanf(dev, "%d:%d%c", bus, addr, &end) == 2;
}

/* the device dev (usb_line.h) opened in h_; false (and why on stderr) */
bool UsbLine::find(const char *dev)
{
	libusb_device **list;
	int bus = -1, addr = -1;
	s16 found = 0;
	bool any = !dev || !*dev || !strcmp(dev, "auto");
	ssize_t n;

	if (!any && !parse_dev(dev, &bus, &addr)) {
		ERR("--usb %s: auto, or <bus>:<address> (dmodemd --usb list "
		    "has them)\n", dev);
		return false;
	}
	n = libusb_get_device_list(ctx_, &list);
	if (n < 0) {
		ERR("libusb: %s\n", libusb_strerror((int)n));
		return false;
	}
	for (ssize_t i = 0; i < n && !h_; i++) {
		libusb_device_descriptor d;
		if (libusb_get_device_descriptor(list[i], &d) < 0)
			continue;
		bool st7554 = d.idVendor == ST7554_VENDOR && d.idProduct == ST7554_PRODUCT;
		bool stick = d.idVendor == STICK_VENDOR && d.idProduct == STICK_PRODUCT;
		bool agere = agere_device(d.idVendor, d.idProduct);
		if (!st7554 && !stick && !agere)
			continue;
		found++;
		if (!any && (libusb_get_bus_number(list[i]) != bus ||
			     libusb_get_device_address(list[i]) != addr))
			continue;
		kind_ = stick ? Kind::Stick : agere ? Kind::Agere : Kind::St7554;
		name_ = stick ? "pico-serial Line" : agere ? "Agere USB modem" :
			"SmartUSB56";
		int err = libusb_open(list[i], &h_);
		if (err < 0) {
			char idbuf[10];
			snprintf(idbuf, sizeof(idbuf), "%04x:%04x", d.idVendor,
				 d.idProduct);
			const char *id = idbuf;
			std::string hint;
#ifdef _WIN32
			/* (another driver than WinUSB has it: Smart Link's
			   own, or none) */
			if (err == LIBUSB_ERROR_NOT_SUPPORTED ||
			    err == LIBUSB_ERROR_ACCESS)
				hint = std::string(" (it needs WinUSB as its "
					"driver, or libusbK on Windows 7 "
					"and 8: dmodem-driver usb "
					"dmodem-usb.inf, or Zadig, for ") +
					id + ")";
#else
			if (err == LIBUSB_ERROR_ACCESS)
				hint = std::string(" (as root, or a udev rule "
						   "for ") + id + ")";
#endif
			ERR("%s at %03d:%03d: %s%s\n", name_,
			    libusb_get_bus_number(list[i]),
			    libusb_get_device_address(list[i]),
			    libusb_strerror(err), hint.c_str());
			h_ = nullptr;
			break;
		}
		DBG("%s at %03d:%03d\n", name_, libusb_get_bus_number(list[i]),
		    libusb_get_device_address(list[i]));
	}
	libusb_free_device_list(list, 1);
	if (!h_ && !found)
		ERR("no Smart Link SmartUSB56 (USB 0483:7554), pico-serial "
		    "Line (1209:0002) or Agere USB modem (047e:2892, 047e:2828)"
		    "%s%s\n", any ? "" : " at ", any ? "" : dev);
	else if (!h_ && found && !any)
		ERR("no SmartUSB56, pico-serial Line or Agere USB modem at %s "
		    "(%d elsewhere)\n", dev, found);
	return h_ != nullptr;
}

/* see usb_line.h: the first device find() would take an Agere modem */
u32 UsbLine::line_rate(const char *dev, u32 rate)
{
	libusb_context *ctx;
	libusb_device **list;
	int bus = -1, addr = -1;
	bool any = !dev || !*dev || !strcmp(dev, "auto");
	bool agere = false;

	if ((!any && !parse_dev(dev, &bus, &addr)) || libusb_init(&ctx) < 0)
		return rate;
#ifdef _WIN32
	use_usbdk(ctx);
#endif
	ssize_t n = libusb_get_device_list(ctx, &list);
	for (ssize_t i = 0; i < n; i++) {
		libusb_device_descriptor d;
		if (libusb_get_device_descriptor(list[i], &d) < 0)
			continue;
		bool ours = agere_device(d.idVendor, d.idProduct) ||
			(d.idVendor == ST7554_VENDOR && d.idProduct == ST7554_PRODUCT) ||
			(d.idVendor == STICK_VENDOR && d.idProduct == STICK_PRODUCT);
		if (!ours || (!any && (libusb_get_bus_number(list[i]) != bus ||
				       libusb_get_device_address(list[i]) != addr)))
			continue;
		agere = agere_device(d.idVendor, d.idProduct);
		break;
	}
	if (n >= 0)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	if (agere && rate != 8000)
		DBG("Agere USB modem: 8000 Hz, its rate (not %" PRIu32 ")\n", rate);
	return agere ? 8000 : rate;
}

/* see usb_line.h */
s16 UsbLine::list()
{
	libusb_context *ctx;
	libusb_device **devs;
	int err;
	s16 found = 0;

	if ((err = libusb_init(&ctx)) < 0) {
		ERR("libusb: %s\n", libusb_strerror(err));
		return -1;
	}
#ifdef _WIN32
	bool usbdk = use_usbdk(ctx);
#endif
	ssize_t n = libusb_get_device_list(ctx, &devs);
	for (ssize_t i = 0; i < n; i++) {
		libusb_device_descriptor d;
		libusb_device_handle *h;
		if (libusb_get_device_descriptor(devs[i], &d) < 0)
			continue;
		bool st7554 = d.idVendor == ST7554_VENDOR &&
			d.idProduct == ST7554_PRODUCT;
		bool stick = d.idVendor == STICK_VENDOR &&
			d.idProduct == STICK_PRODUCT;
		bool agere = agere_device(d.idVendor, d.idProduct);
		if (!st7554 && !stick && !agere)
			continue;
		found++;
		err = libusb_open(devs[i], &h);
		if (err == 0)
			libusb_close(h);
		printf("%03d:%03d %04x:%04x %s: %s%s\n",
		       libusb_get_bus_number(devs[i]),
		       libusb_get_device_address(devs[i]), d.idVendor,
		       d.idProduct, stick ? "pico-serial Line" :
		       agere ? "Agere USB modem" : "SmartUSB56",
		       err == 0 ? "ready" : libusb_strerror(err),
#ifdef _WIN32
		       err == LIBUSB_ERROR_NOT_SUPPORTED ?
		       " (its driver is not WinUSB or libusbK)" :
#endif
		       "");
	}
	if (n >= 0)
		libusb_free_device_list(devs, 1);
	if (!found)
		printf("no SmartUSB56 (0483:7554), pico-serial Line "
		       "(1209:0002) or Agere USB modem (047e:2892, 047e:2828)\n");
#ifdef _WIN32
	else if (usbdk)
		printf("(through UsbDk)\n");
	else if (!winusb_streams())
		printf("(this Windows' WinUSB can't stream: a modem whose "
		       "driver is WinUSB is listed ready but can't be used;"
		       " libusbK's or UsbDk can)\n");
#endif
	libusb_exit(ctx);
	return 0;
}

/* the endpoints of interface 0's alternate setting alt: interrupt IN,
   then isochronous OUT and IN, the codec's; in ALT_VOICE another OUT and
   IN after them, the voice codec's (as Windows' driver takes them, its
   pipes 3 and 4) */
bool UsbLine::endpoints(s16 alt)
{
	libusb_config_descriptor *cfg;
	int err = libusb_get_active_config_descriptor(libusb_get_device(h_),
						      &cfg);
	if (err < 0) {
		ERR("%s: its configuration: %s\n", name_, libusb_strerror(err));
		return false;
	}
	const libusb_interface_descriptor *d = nullptr;
	if (cfg->bNumInterfaces >= 1)
		for (s16 i = 0; i < cfg->interface[0].num_altsetting; i++)
			if (cfg->interface[0].altsetting[i].bAlternateSetting == alt)
				d = &cfg->interface[0].altsetting[i];
	for (s16 i = 0; d && i < d->bNumEndpoints; i++) {
		const libusb_endpoint_descriptor &e = d->endpoint[i];
		s16 type = e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
		bool in = e.bEndpointAddress & LIBUSB_ENDPOINT_IN;
		s16 max = e.wMaxPacketSize & 0x7ff;
		if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in) {
			ep_intr_ = e.bEndpointAddress;
		} else if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
			Iso &st = (in ? iso_.ep_in : iso_.ep_out) ? viso_ : iso_;
			(in ? st.ep_in : st.ep_out) = e.bEndpointAddress;
			(in ? st.max_in : st.max_out) = max;
		}
	}
	libusb_free_config_descriptor(cfg);
	if (!ep_intr_ || !iso_.ep_in || !iso_.ep_out || !iso_.max_in ||
	    !iso_.max_out) {
		ERR("%s: not the endpoints of %s\n", name_,
		    kind_ == Kind::Stick ? "a pico-serial Line" : "an ST7554");
		return false;
	}
	if (alt == ALT_VOICE && (!viso_.ep_in || !viso_.ep_out ||
				 !viso_.max_in || !viso_.max_out)) {
		ERR("%s: no voice codec's endpoints in alternate setting %d: "
		    "this board has none (no --usb-voice-codec)\n", name_, alt);
		viso_ = Iso();
		return false;
	}
	DBG("endpoints: interrupt %02x, out %02x (%d), in %02x (%d)\n",
	    ep_intr_, iso_.ep_out, iso_.max_out, iso_.ep_in, iso_.max_in);
	if (viso_.ep_in)
		DBG("the voice codec's: out %02x (%d), in %02x (%d)\n",
		    viso_.ep_out, viso_.max_out, viso_.ep_in, viso_.max_in);
	return true;
}

/* a register of the ST7554's (a vendor request); false (logged) */
bool UsbLine::set_reg(u8 reg, u16 v)
{
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
					  LIBUSB_RECIPIENT_DEVICE |
					  LIBUSB_ENDPOINT_OUT, reg, v, 0,
					  nullptr, 0, TIMEOUT_MS);
	if (err < 0)
		ERR("SmartUSB56: register %02x = %04x: %s\n", reg, v,
		    libusb_strerror(err));
	return err >= 0;
}

/* a register of the ST7554's read into *v (a vendor
   request); false (logged) */
bool UsbLine::get_reg(u8 reg, u16 *v)
{
	u8 b[2] = { 0, 0 };
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
					  LIBUSB_RECIPIENT_DEVICE |
					  LIBUSB_ENDPOINT_IN,
					  reg | LIBUSB_ENDPOINT_IN, 0, 0,
					  b, 2, TIMEOUT_MS);
	if (err < 0) {
		ERR("SmartUSB56: register %02x: %s\n", reg,
		    libusb_strerror(err));
		return false;
	}
	*v = static_cast<u16>(b[0] | u16{b[1]} << 8);
	return true;
}

/* the GPIOs' outputs */
bool UsbLine::set_gpio(u16 v)
{
	if (!set_reg(GPIO_OUT, v))
		return false;
	gpio_ = v;
	return true;
}

/* see usb_line.h: the device, its interface, then its own start-up
   (open_st7554(), open_stick()), on hook, the transfers both ways and
   the interrupt's */
bool UsbLine::open(const char *dev, u32 rate)
{
	int err;

	iso_ = Iso();
	viso_ = Iso();
	iso_.line = viso_.line = this;
	viso_.voice = true;
	tx_gain_ = 4096;
	tx_clipped_ = 0;
	tx_auto_ = false;
	if (const char *g = getenv("DMODEM_USB_TX_GAIN"); g && !strcmp(g, "auto")) {
		tx_auto_ = true;
		tx_auto_start();
		DBG("the modem's samples to the line %+.1f dB to start (DMODEM_USB_TX_GAIN=auto: "
		    "then from the remote's answer tone)\n", tx_auto_db_);
	} else if (g && *g) {
		double db = std::max(-20.0, std::min(20.0, strtod(g, nullptr)));
		tx_gain_ = static_cast<s32>(std::lround(4096.0 * std::pow(10.0, db / 20.0)));
		DBG("the modem's samples to the line %+.1f dB (DMODEM_USB_TX_GAIN)\n", db);
	}
	if ((err = libusb_init(&ctx_)) < 0) {
		ERR("libusb: %s\n", libusb_strerror(err));
		ctx_ = nullptr;
		return false;
	}
#ifdef _WIN32
	if (use_usbdk(ctx_))
		DBG("through UsbDk\n");
#endif
	if (!find(dev)) {
		close();
		return false;
	}
	/* (an Agere modem's samples are at 8000 Hz) */
	if (kind_ == Kind::Agere && rate != 8000) {
		DBG("Agere USB modem: 8000 Hz, its rate (not %" PRIu32 ")\n", rate);
		rate = 8000;
	}
	rate_ = rate;
	frame_ = static_cast<s16>(rate / 50);
	frame_in_.assign(frame_, 0);
	frame_out_.assign(frame_, 0);
	frame_mic_.assign(frame_, 0);
	frame_ear_.assign(frame_, 0);
	/* (slusb, Smart Link's kernel driver, if it is there) */
	libusb_set_auto_detach_kernel_driver(h_, 1);
	if ((err = libusb_claim_interface(h_, 0)) < 0) {
		ERR("%s: its interface: %s\n", name_, libusb_strerror(err));
		close();
		return false;
	}
	claimed_ = true;
	if (!(kind_ == Kind::Stick ? open_stick(rate) :
	      kind_ == Kind::Agere ? open_agere() : open_st7554(rate)) ||
	    !start_streams()) {
		close();
		return false;
	}
	DBG("%" PRIu32 " Hz, %d samples ahead\n", rate, delay_);
	return true;
}

/* the ST7554: the rate's control word, alternate setting 1 (3 with the
   voice codec), its codec checked, its registers as Smart Link's driver
   sets them, on hook */
bool UsbLine::open_st7554(u32 rate)
{
	u16 status;
	int err;
	s16 alt = voice_law_ != VoiceLaw::None ? ALT_VOICE : ALT_MODEM;

	if (!cword(rate)) {
		ERR("SmartUSB56: no rate of %" PRIu32 " Hz (8000, 9600)\n",
		    rate);
		return false;
	}
	if ((err = libusb_set_interface_alt_setting(h_, 0, alt)) < 0) {
		ERR("SmartUSB56: alternate setting %d: %s%s\n", alt,
		    libusb_strerror(err), alt == ALT_VOICE ?
		    " (no voice codec: no --usb-voice-codec)" : "");
		return false;
	}
	if (!endpoints(alt) || !get_reg(GPIO_STATUS, &status))
		return false;
	if (rhom_) {
		u16 rev = 0;
		get_reg(REVISION, &rev);
		rhom_ok_ = rev >= RHOM_REVISION;
		DBG("revision %04x: %s\n", rev, rhom_ok_ ? "RHOM while the modem runs" :
		    "below 0200, no RHOM (as Windows)");
	}
	st75951_ = status & GPIO_MONOLITHIC;
	DBG("codec: %s\n", st75951_ ? "ST75951/2 (silicon DAA)" : "STLC7550");
	gpio_ = 0;
	if (!init_st7554_regs())
		return false;
	for (Iso *st : { &iso_, &viso_ })
		if (st->ep_in) {
			libusb_clear_halt(h_, st->ep_in);
			libusb_clear_halt(h_, st->ep_out);
		}
	intr_len_ = 4;
	fifo_samples_ = FIFO_SAMPLES;
	if (handset_)
		handset_due_ = true;    /* (its state now, at the first pump) */
	if (wake_)
		arm_wake(true, true);
	return true;
}

/* the ST7554's registers as Smart Link's driver sets them, the GPIOs'
   outputs gpio_ (on hook at the start, the hook as it was after a
   resume), the ring detector's interrupt and with set_handset() the
   handset detector's; the voice codec powered up after the codec (as
   Windows' UpdateCodecPowerMode does) if it streams; false (logged) */
bool UsbLine::init_st7554_regs()
{
	u16 mask = static_cast<u16>(GPIO_RING1 |
					      (handset_ ? GPIO_HANDSET : 0));
	return set_reg(GPIO_DIR, 0x3ff8) && set_gpio(gpio_) &&
		set_reg(GPIO_MASK, mask) &&
		set_reg(FIFO_CONTROL1, 0x2828 | BYTE_ORDER_LE) &&
		set_reg(FIFO_CONTROL2, fifo2_ = fifo_control2()) &&
		set_reg(FIFO_MASK, 0) &&
		set_reg(FIFO_MASK, SSI1_UNDERRUN | SSI1_OVERRUN) &&
		set_reg(SSI1_COUNTER, 0) && set_reg(SSI2_COUNTER, 0) &&
		(st75951_ ? start_st75951() : set_reg(SSI1_CONTROL, SSI1_POWERUP)) &&
		set_reg(SSI1_CWORD, cword(rate_)) && set_reg(GPIO_INV, 0) &&
		(!viso_.ep_in || set_reg(SSI2_CONTROL, SSI2_POWERUP));
}

/* the device's USB remote wakeup armed (on) or cleared: SET_FEATURE or
   CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP), if its configuration says it can
   wake; with kernel, on Linux the kernel's power/wakeup of it enabled
   too (at open(): it needs root, and dmodemd opens the device before it
   drops root; the kernel keeps it across a resume) */
void UsbLine::arm_wake(bool on, bool kernel)
{
	libusb_config_descriptor *cfg;
	bool can = false;
	if (libusb_get_active_config_descriptor(libusb_get_device(h_), &cfg) == 0) {
		can = cfg->bmAttributes & CONFIG_REMOTE_WAKEUP;
		libusb_free_config_descriptor(cfg);
	}
	if (!can) {
		if (on)
			ERR("%s: its configuration says it cannot wake the "
			    "computer: no wake on ring\n", name_);
		return;
	}
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_STANDARD |
					  LIBUSB_RECIPIENT_DEVICE |
					  LIBUSB_ENDPOINT_OUT, on ?
					  LIBUSB_REQUEST_SET_FEATURE :
					  LIBUSB_REQUEST_CLEAR_FEATURE,
					  FEATURE_REMOTE_WAKEUP, 0, nullptr, 0,
					  TIMEOUT_MS);
	if (err < 0) {
		ERR("%s: its remote wakeup %s: %s\n", name_,
		    on ? "armed" : "cleared", libusb_strerror(err));
		return;
	}
	DBG("remote wakeup %s\n", on ? "armed" : "cleared");
#ifdef __linux__
	if (!on || !kernel)
		return;
	/* /sys/bus/usb/devices/<bus>-<port>[.<port>...]/power/wakeup */
	u8 ports[8];
	libusb_device *d = libusb_get_device(h_);
	int n = libusb_get_port_numbers(d, ports,
					static_cast<int>(sizeof(ports)));
	if (n <= 0)
		return;
	std::string path = "/sys/bus/usb/devices/" +
		std::to_string(libusb_get_bus_number(d)) + "-";
	for (s16 i = 0; i < n; i++)
		path += (i ? "." : "") + std::to_string(ports[i]);
	path += "/power/wakeup";
	FILE *f = fopen(path.c_str(), "w");
	bool ok = f && fputs("enabled", f) >= 0;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok) {
		ERR("%s: %s: %s (as root, or a udev rule's "
		    "ATTR{power/wakeup}=\"enabled\"): the computer may not "
		    "wake on a ring\n", name_, path.c_str(), strerror(errno));
		return;
	}
	DBG("%s enabled\n", path.c_str());
#endif
}

/* the ST75951/2's start-up, as Windows' driver does it: its control
   words (register 0x17: the register in bits 12 to 15, the value
   below) 3 = 0x050 and 0 = 0x3c0 while SSI1_CONTROL is 3, then 2 (kept
   set from then on), then powered up (3); the rate's word goes into its
   register 0 after it */
bool UsbLine::start_st75951()
{
	return set_reg(SSI1_CONTROL, SSI1_ST75951 | SSI1_POWERUP) &&
		set_reg(SSI1_CWORD, 3 << 12 | 0x050) &&
		set_reg(SSI1_CWORD, 0 << 12 | 0x3c0) &&
		set_reg(SSI1_CONTROL, SSI1_ST75951) &&
		set_reg(SSI1_CONTROL, SSI1_ST75951 | SSI1_POWERUP);
}

/* the stick: its version and line side (GET_INFO), the rate, on hook with
   the line monitor on, its ring detector's count so far, then alternate
   setting 1, which starts its samples */
bool UsbLine::open_stick(u32 rate)
{
	u8 info[12], status[20];
	int err;

	if (!stick_rate(rate)) {
		ERR("pico-serial Line: no rate of %" PRIu32 " Hz (8000, 9600, "
		    "16000 and the Si3056's others)\n", rate);
		return false;
	}
	if (!stick_in(LP_GET_INFO, 0, info, sizeof(info)))
		return false;
	if (info[0] != 1) {
		ERR("pico-serial Line: protocol version %u, not 1: newer "
		    "firmware than this dmodemd knows\n", info[0]);
		return false;
	}
	si3019_ = (info[8] >> 4) == 3;
	DBG("pico-serial Line, firmware %u.%u.%u, line side %s rev %u\n",
	    info[1], info[2], info[3], (info[8] >> 4) == 3 ? "Si3019" :
	    (info[8] >> 4) == 1 ? "Si3018" : "unknown", (info[9] >> 2) & 0xf);
	if (!(info[7] & LP_ST_LINK))
		ERR("pico-serial Line: its line side does not answer (the "
		    "isolation link is down); going on\n");
	if (!stick_out(LP_SET_RATE, static_cast<u16>(rate)) ||
	    !stick_out(LP_SET_HOOK, 0) || !stick_out(LP_SET_MONITOR, 1) ||
	    !stick_in(LP_GET_STATUS, 0, status, sizeof(status)))
		return false;
	if (!stick_country())
		return false;
	ring_edges_ = status[18];
	underruns_ = static_cast<u16>(status[6] |
					   u16{status[7]} << 8);
	overruns_ = static_cast<u16>(status[8] |
					  u16{status[9]} << 8);
	if ((err = libusb_set_interface_alt_setting(h_, 0, 1)) < 0) {
		ERR("pico-serial Line: alternate setting 1: %s\n",
		    libusb_strerror(err));
		return false;
	}
	if (voice_law_ != VoiceLaw::None)
		ERR("pico-serial Line: no voice codec (--usb-voice-codec)\n");
	if (!endpoints(ALT_MODEM))
		return false;
	intr_len_ = 8;
	fifo_samples_ = STICK_FIFO_SAMPLES;
	return true;
}

/* see usb_line.h */
bool UsbLine::stick_out(u8 req, u16 value)
{
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
					  LIBUSB_RECIPIENT_INTERFACE |
					  LIBUSB_ENDPOINT_OUT, req, value, 0,
					  nullptr, 0, TIMEOUT_MS);
	if (err < 0)
		ERR("pico-serial Line: request %02x (%u): %s\n", req, value,
		    libusb_strerror(err));
	return err >= 0;
}

/* see usb_line.h */
bool UsbLine::stick_in(u8 req, u16 value, u8 *buf, u16 len)
{
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
					  LIBUSB_RECIPIENT_INTERFACE |
					  LIBUSB_ENDPOINT_IN, req, value, 0,
					  buf, len, TIMEOUT_MS);
	if (err < 0 || err < len) {
		ERR("pico-serial Line: request %02x: %s\n", req, err < 0 ?
		    libusb_strerror(err) : "too short");
		return false;
	}
	return true;
}

/* the transfers: the interrupt's, then each stream's (start_iso()),
   then the FIFOs started */
bool UsbLine::start_streams()
{
	int err;
	/* octets a ms, at most */
	s16 pkt = static_cast<s16>((rate_ * 2 + 999) / 1000);

	if (kind_ == Kind::Agere)
		return agere_streams();
	iso_.out_buf.assign(frame_ + frame_ / 2, 0);
	delay_ = static_cast<s16>(iso_.out_buf.size()) +
		 ISO_TRANSFERS * ISO_PACKETS * static_cast<s32>(rate_) /
			 1000 +
		 fifo_samples_;
	intr_ = libusb_alloc_transfer(0);
	if (!intr_)
		return false;
	libusb_fill_interrupt_transfer(intr_, h_, ep_intr_, intr_buf_,
				       intr_len_, intr_done, this, 0);
	if ((err = libusb_submit_transfer(intr_)) < 0) {
		ERR("%s: its interrupt: %s\n", name_, libusb_strerror(err));
		return false;
	}
	in_flight_++;
	if (!start_iso(iso_, pkt))
		return false;
	if (viso_.ep_in) {
		viso_.out_buf.assign(VOICE_RATE / 50 + VOICE_RATE / 100, 0);
		if (!start_iso(viso_, static_cast<s16>(VOICE_RATE / 1000)))
			return false;
	}
	/* (the stick's samples run since its alternate setting 1) */
	return kind_ == Kind::Stick || start_fifos();
}

/* stream s's transfers: ISO_TRANSFERS IN ones, the OUT ones (each sent
   when an IN one is done; the first ones now, pkt octets a packet of
   what s has to send, silence past it) */
bool UsbLine::start_iso(Iso &s, s16 pkt)
{
	int err;
	for (s16 i = 0; i < ISO_TRANSFERS; i++) {
		s.in[i] = libusb_alloc_transfer(ISO_PACKETS);
		s.out[i] = libusb_alloc_transfer(ISO_PACKETS);
		if (!s.in[i] || !s.out[i])
			return false;
		u8 *ib =
			static_cast<u8 *>(calloc(ISO_PACKETS, s.max_in));
		u8 *ob =
			static_cast<u8 *>(calloc(ISO_PACKETS, s.max_out));
		if (!ib || !ob) {
			free(ib);
			free(ob);
			return false;
		}
		libusb_fill_iso_transfer(s.in[i], h_, s.ep_in, ib,
					 ISO_PACKETS * s.max_in, ISO_PACKETS,
					 in_done, &s, 0);
		libusb_set_iso_packet_lengths(s.in[i], s.max_in);
		s.in[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_fill_iso_transfer(s.out[i], h_, s.ep_out, ob,
					 ISO_PACKETS * s.max_out, ISO_PACKETS,
					 out_done, &s, 0);
		s.out[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		if ((err = libusb_submit_transfer(s.in[i])) < 0) {
			ERR("%s: an IN transfer: %s%s\n", name_,
			    libusb_strerror(err), no_streams(err));
			return false;
		}
		in_flight_++;
		std::vector<s16> lens(ISO_PACKETS, pkt > s.max_out ? s.max_out : pkt);
		out_fill(s, s.out[i], lens.data());
		if ((err = libusb_submit_transfer(s.out[i])) < 0) {
			ERR("%s: an OUT transfer: %s%s\n", name_,
			    libusb_strerror(err), no_streams(err));
			return false;
		}
		in_flight_++;
	}
	return true;
}

/* the ST7554's FIFOs started (their counters), their errors on the
   interrupt: the codec's, and the voice codec's if it streams; false
   (logged) */
bool UsbLine::start_fifos()
{
	u16 mask = SSI1_UNDERRUN | SSI1_OVERRUN;
	if (viso_.ep_in)
		mask |= SSI2_UNDERRUN | SSI2_OVERRUN;
	return set_reg(SSI1_COUNTER, FRAGSIZE) &&
		(!viso_.ep_in || set_reg(SSI2_COUNTER, VOICE_PERIOD)) &&
		set_reg(FIFO_MASK, mask);
}

/* an OUT transfer of s's packets of lengths octets (whole samples), the
   samples queued in them (silence past them): S16 little-endian, or the
   voice codec's octets in its law */
void UsbLine::out_fill(Iso &s, libusb_transfer *t, const s16 *lengths)
{
	s16 off = 0;
	s16 bps = s.voice ? 1 : 2;
	if (kind_ == Kind::Agere) {
		(void)lengths;
		agere_fill(t);
		return;
	}
	for (s16 i = 0; i < ISO_PACKETS; i++) {
		s16 len = lengths[i] - lengths[i] % bps;
		if (len > s.max_out)
			len = s.max_out - s.max_out % bps;
		t->iso_packet_desc[i].length = static_cast<unsigned>(len);
		for (s16 k = 0; k < len / bps; k++) {
			s16 x = 0;
			if (!s.out_buf.empty()) {
				x = s.out_buf.front();
				s.out_buf.pop_front();
			}
			if (!s.voice) {
				t->buffer[off + 2 * k] =
					static_cast<u8>(x);
				t->buffer[off + 2 * k + 1] =
					static_cast<u8>(x >> 8);
			} else if (voice_law_ == VoiceLaw::Linear) {
				t->buffer[off + k] =
					static_cast<u8>(x >> 8);
			} else {
				t->buffer[off + k] = dsp::g711::encode(
					voice_law_ == VoiceLaw::Alaw, x);
			}
		}
		off += len;
	}
	t->length = off;
}

/* libusb's callbacks: to the line */
void LIBUSB_CALL UsbLine::in_done(libusb_transfer *t)
{
	Iso *s = static_cast<Iso *>(t->user_data);
	s->line->in_complete(*s, t);
}

/* an OUT transfer done: idle again, sent once there are
   lengths for it (the line gone if unplugged) */
void LIBUSB_CALL UsbLine::out_done(libusb_transfer *t)
{
	Iso *s = static_cast<Iso *>(t->user_data);
	UsbLine *l = s->line;
	l->in_flight_--;
	if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
		l->gone("unplugged");
	if (l->dead_ || t->status == LIBUSB_TRANSFER_CANCELLED)
		return;
	/* (the Agere modem's: sent again at once, paced as its samples come:
	   its ROM answers each OUT packet with an IN one) */
	if (l->kind_ == Kind::Agere) {
		if (t->status == LIBUSB_TRANSFER_COMPLETED)
			l->agere_sent(t);
		l->agere_fill(t);
		int err = libusb_submit_transfer(t);
		if (err == 0)
			l->in_flight_++;
		else
			l->park(t, err);
		return;
	}
	/* sent again with the lengths an IN transfer has given, or once
	   one does */
	s->idle_out.push_back(t);
	l->send_out(*s);
}

/* s's idle OUT transfers sent, as many as there are IN transfers'
   lengths for */
void UsbLine::send_out(Iso &s)
{
	while (!s.idle_out.empty() && !s.out_lengths.empty()) {
		libusb_transfer *t = s.idle_out.front();
		s.idle_out.pop_front();
		out_fill(s, t, s.out_lengths.front().data());
		s.out_lengths.pop_front();
		int err = libusb_submit_transfer(t);
		if (err == 0)
			in_flight_++;
		else
			park(t, err);
	}
}

/* the interrupt transfer done: the FIFO's errors logged,
   a ring counted, it again (the line gone if unplugged) */
void LIBUSB_CALL UsbLine::intr_done(libusb_transfer *t)
{
	UsbLine *l = static_cast<UsbLine *>(t->user_data);
	l->in_flight_--;
	if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
		l->gone("unplugged");
		return;
	}
	if (l->dead_ || t->status == LIBUSB_TRANSFER_CANCELLED)
		return;
	if (l->kind_ == Kind::Stick) {
		if (t->status == LIBUSB_TRANSFER_COMPLETED)
			l->stick_event(t->buffer, t->actual_length);
	} else if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length >= 4) {
		u16 fifo =
			static_cast<u16>(t->buffer[0] |
					      u16{t->buffer[1]} << 8);
		u16 gpio =
			static_cast<u16>(t->buffer[2] |
					      u16{t->buffer[3]} << 8);
		if (fifo & SSI1_UNDERRUN)
			DBG("FIFO underrun\n");
		if (fifo & SSI1_OVERRUN)
			DBG("FIFO overrun\n");
		if (fifo & SSI2_UNDERRUN)
			DBG("the voice codec's FIFO underrun\n");
		if (fifo & SSI2_OVERRUN)
			DBG("the voice codec's FIFO overrun\n");
		if (gpio & GPIO_RING1)
			l->rings_++;
		if ((gpio & GPIO_HANDSET) && l->handset_)
			l->handset_due_ = true;
	}
	int err = libusb_submit_transfer(t);
	if (err == 0)
		l->in_flight_++;
	else
		l->park(t, err);
}

/* see usb_line.h: a country the datasheet has no settings for keeps the
   USA's (the Si3056's reset values) */
void UsbLine::country(dcore::Modem &, const char *name)
{
	daa_name_ = name ? name : "USA";
	daa_ = si3056_country(name);
	if (!daa_) {
		DBG("no Si3056 line settings for %s: the USA's\n",
		    daa_name_.c_str());
		daa_ = &si3056_usa();
	}
	daa_due_ = true;
	/* (--usb-international: the country record's byte 90 as Smart
	   Link's table has it, docs/smartusb56-windows.md) */
	const char *n = daa_name_.c_str();
	intl_dc_ = same_name(n, "FRANCE") || same_name(n, "ALGERIA") ||
		same_name(n, "TUNISIA");
	intl_aus_ = same_name(n, "AUSTRALIA");
	if (h_ && kind_ == Kind::Stick && claimed_ && hook_ != MODEM_HOOK_OFF)
		stick_country();
}

/* see usb_line.h: registers 16, 26, 31, 59, and 30 on a Si3019 */
bool UsbLine::stick_country()
{
	Si3056Regs r;
	u8 v;
	struct {
		u8 reg;
		u8 *val;
	} regs[] = { { 16, &r.r16 }, { 26, &r.r26 }, { 30, &r.r30 },
		     { 31, &r.r31 }, { 59, &r.r59 } };

	for (auto &x : regs) {
		if (x.reg == 30 && !si3019_) {
			*x.val = 0;
			continue;
		}
		if (!stick_in(LP_DAA_READ, x.reg, &v, 1))
			return false;
		*x.val = v;
	}
	si3056_apply(*daa_, si3019_, r);
	for (auto &x : regs) {
		if (x.reg == 30 && !si3019_)
			continue;
		if (!stick_out(LP_DAA_WRITE,
			       static_cast<u16>(x.reg << 8 | *x.val)))
			return false;
	}
	daa_due_ = false;
	char r30[4] = "-";
	if (si3019_)
		snprintf(r30, sizeof(r30), "%02x", r.r30);
	DBG("%s's line: registers 16 %02x, 26 %02x, 30 %s, 31 %02x, 59 %02x\n",
	    daa_name_.c_str(), r.r16, r.r26, r30, r.r31, r.r59);
	return true;
}

/* see usb_line.h: the ring detector's changes since the last event a
   ring pulse each (the core checks the cadence, as with the ST7554's
   GPIO), the line's events and the FIFOs' errors logged */
void UsbLine::stick_event(const u8 *b, s16 len)
{
	if (len < 8)
		return;
	u8 edges = static_cast<u8>(b[1] - ring_edges_);
	u16 under = static_cast<u16>(b[4] | u16{b[5]} << 8);
	u16 over = static_cast<u16>(b[6] | u16{b[7]} << 8);

	ring_edges_ = b[1];
	rings_ += edges;
	if (b[2] & LP_INT_LINK_LOST)
		ERR("pico-serial Line: the isolation link was lost\n");
	if (b[2] & LP_INT_OVERLOAD)
		DBG("receive overload\n");
	if (b[2] & LP_INT_DROPOUT)
		DBG("the line dropped out\n");
	if (b[2] & LP_INT_POLARITY)
		DBG("polarity reversal\n");
	if (under != underruns_)
		DBG("playback underrun (%u)\n", (unsigned)(u16)(under - underruns_));
	if (over != overruns_)
		DBG("FIFO overrun (%u)\n", (unsigned)(u16)(over - overruns_));
	underruns_ = under;
	overruns_ = over;
}

/* s's IN transfer done: its samples queued (S16 little-endian, or the
   voice codec's octets in its law), its packets' lengths for an OUT
   transfer (one waiting sent now), and it again */
void UsbLine::in_complete(Iso &s, libusb_transfer *t)
{
	std::vector<s16> lens(ISO_PACKETS, 0);

	in_flight_--;
	if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
		gone("unplugged");
		return;
	}
	if (dead_ || t->status == LIBUSB_TRANSFER_CANCELLED)
		return;
	if (kind_ == Kind::Agere) {
		agere_in(t);
		int err = libusb_submit_transfer(t);
		if (err == 0)
			in_flight_++;
		else
			park(t, err);
		return;
	}
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		for (s16 i = 0; i < t->num_iso_packets; i++) {
			const libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
			const u8 *b = libusb_get_iso_packet_buffer_simple(t, i);
			if (p.status != LIBUSB_TRANSFER_COMPLETED)
				continue;
			if (!s.voice)
				for (u32 k = 0; k + 1 < p.actual_length; k += 2)
					s.in_buf.push_back(static_cast<s16>(
						b[k] |
						u16{b[k + 1]} << 8));
			else if (voice_law_ == VoiceLaw::Linear)
				for (u16 k = 0; k < p.actual_length; k++)
					s.in_buf.push_back(static_cast<s16>(
						static_cast<s8>(b[k]) *
						256));
			else
				for (u16 k = 0; k < p.actual_length; k++)
					s.in_buf.push_back(static_cast<s16>(
						dsp::g711::decode(
							voice_law_ ==
								VoiceLaw::Alaw,
							b[k])));
			lens[i] = static_cast<s16>(p.actual_length);
		}
	}
	s.out_lengths.push_back(lens);
	send_out(s);
	int err = libusb_submit_transfer(t);
	if (err == 0)
		in_flight_++;
	else
		park(t, err);
}

/* a transfer that could not be submitted again (err): the device gone
   if unplugged; else (suspended, the computer asleep) kept for
   resume() */
void UsbLine::park(libusb_transfer *t, int err)
{
	if (err == LIBUSB_ERROR_NO_DEVICE) {
		gone("unplugged");
		return;
	}
	if (parked_.empty()) {
		DBG("transfers fail (%s): suspended?\n", libusb_strerror(err));
		parked_since_ = resume_tried_ = clock_ms();
	}
	parked_.push_back(t);
}

/* see usb_line.h (pump()): every RESUME_MS the device set up again (its
   registers, its remote wakeup, the FIFO started; a resume may have
   reset it), the streams started afresh, then the parked transfers
   submitted again; gone after RESUME_GIVE_UP_MS of failures */
void UsbLine::resume()
{
	u64 now = clock_ms();
	if (now - resume_tried_ < RESUME_MS)
		return;
	resume_tried_ = now;
	if (now - parked_since_ > RESUME_GIVE_UP_MS) {
		gone("its transfers fail");
		return;
	}
	if (kind_ == Kind::St7554) {
		if (!init_st7554_regs() || !start_fifos())
			return;
		if (wake_)
			arm_wake(true);
		handset_due_ = handset_;
	}
	/* (the Agere modem's stream may have gone with a reset) */
	if (kind_ == Kind::Agere && (!agere_table() || !agere_start()))
		return;
	agere_lead_ = 0;
	/* the streams from their start again: nothing kept from before
	   the suspend, the playback's lead a frame and a half */
	for (Iso *st : { &iso_, &viso_ }) {
		st->in_buf.clear();
		st->out_lengths.clear();
	}
	iso_.out_buf.assign(frame_ + frame_ / 2, 0);
	viso_.out_buf.assign(VOICE_RATE / 50 + VOICE_RATE / 100, 0);
	mic_.clear();
	std::vector<libusb_transfer *> again;
	again.swap(parked_);
	for (libusb_transfer *t : again) {
		if (t == intr_) {
			int err = libusb_submit_transfer(t);
			if (err == 0)
				in_flight_++;
			else
				park(t, err);
		} else if (!(t->endpoint & LIBUSB_ENDPOINT_IN) &&
			   kind_ != Kind::Agere) {
			static_cast<Iso *>(t->user_data)->idle_out.push_back(t);
		} else {
			if (!(t->endpoint & LIBUSB_ENDPOINT_IN))
				agere_fill(t);
			int err = libusb_submit_transfer(t);
			if (err == 0)
				in_flight_++;
			else
				park(t, err);
		}
	}
	if (parked_.empty())
		DBG("resumed\n");
	send_out(iso_);
	send_out(viso_);
}

/* the device has gone: no more transfers */
void UsbLine::gone(const char *why)
{
	if (!dead_)
		ERR("%s: %s\n", name_, why);
	dead_ = true;
}

/* see usb_line.h: the stick's line monitor on while on hook (rings and
   caller ID heard), off off hook, as an ALSA codec's "Caller ID Switch";
   the ST7554's CID1 GPIO on only while the core snoops for caller ID
   (MODEM_HOOK_SNOOPING), cleared before the hook changes, as Windows'
   driver does */
s16 UsbLine::set_hook(dcore::Modem &, s16 state)
{
	bool off = state == MODEM_HOOK_OFF;

	bool was_off = hook_ == MODEM_HOOK_OFF;
	DBG("%s\n", off ? "off hook" : "on hook");
	hook_ = state;
	if (off)
		rec_start();
	else
		rec_stop();
	if (off && tx_auto_)
		tx_auto_start();
	/* (the Agere's ring registers' changes said again after a call) */
	agere_poll_said_ = 0;
	/* (after a call: not caller ID's listening between rings) */
	if (!off && was_off && kind_ == Kind::Agere)
		agere_onhook_ms_ = clock_ms();
	flash_end_ = 0;
	if (kind_ == Kind::Agere)   /* (it hears the line on hook) */
		return agere_hook(off) && agere_caller_id(!off) ? 0 : -1;
	if (kind_ == Kind::Stick)
		return off ? (stick_out(LP_SET_HOOK, 1) &&
			      stick_out(LP_SET_MONITOR, 0) ? 0 : -1) :
			(stick_out(LP_SET_HOOK, 0) &&
			 stick_out(LP_SET_MONITOR, 1) &&
			 (!daa_due_ || stick_country()) ? 0 : -1);
	bool cid = state == MODEM_HOOK_SNOOPING;
	bool now_off = off && !(gpio_ & GPIO_HOOK1);
	if ((gpio_ & GPIO_CID1) && !cid &&
	    !set_gpio(static_cast<u16>(gpio_ & ~GPIO_CID1)))
		return -1;
	u16 v = off ? static_cast<u16>(gpio_ | GPIO_HOOK1 |
						 GPIO_LED_HOOK) :
			   static_cast<u16>(
				   gpio_ & ~(GPIO_HOOK1 | GPIO_LED_HOOK |
					     GPIO_LED_CARRIER | GPIO_LED_DATA));
	if (!off)
		data_seen_ = online_seen_ = pulse_active_ = false;
	if (intl_)
		v = intl_hook(v, off, now_off);
	if (cid)
		v |= GPIO_CID1;
	return v == gpio_ || set_gpio(v) ? 0 : -1;
}

/* see usb_line.h: GPIO 11 set for any volume but 0, as Windows'
   SetSpeakerVolume with Options.Buzzer */
void UsbLine::speaker(dcore::Modem &, s16 volume)
{
	if (!buzzer_ || kind_ != Kind::St7554 || !h_ || dead_)
		return;
	u16 v = volume > 0 ? static_cast<u16>(gpio_ | GPIO_BUZEN) :
		static_cast<u16>(gpio_ & ~GPIO_BUZEN);
	if (v == gpio_)
		return;
	DBG("buzzer %s (volume %d)\n", volume > 0 ? "on" : "off", volume);
	set_gpio(v);
}

/* the Agere modem's LED (agere_leds(): always); the SmartUSB56's LEDs
   (set_leds()): the carrier's while the call is
   online, the data one once data has passed (the DTE's waiting, or the
   modem's to it after the pump that saw the call online), until on
   hook */
void UsbLine::update_leds(dcore::Modem &m)
{
	if (kind_ == Kind::Agere) {
		agere_leds(m);
		return;
	}
	if (!leds_ || kind_ != Kind::St7554 || hook_ != MODEM_HOOK_OFF)
		return;
	bool online = m.link_state() == dcore::LinkState::Online;
	if (online && m.tx_pending())
		data_seen_ = true;
	online_seen_ = online_seen_ || online;
	u16 v = static_cast<u16>(gpio_ &
					   ~(GPIO_LED_CARRIER | GPIO_LED_DATA));
	if (online)
		v |= GPIO_LED_CARRIER;
	if (data_seen_)
		v |= GPIO_LED_DATA;
	if (v != gpio_)
		set_gpio(v);
}

/* see dcore.h: on hook for ms, then off again (if it still is) */
void UsbLine::hook_flash(dcore::Modem &, u16 ms)
{
	if (hook_ != MODEM_HOOK_OFF)
		return;
	DBG("hook flash, %u ms\n", ms);
	if (kind_ == Kind::Stick) {   /* the stick times it */
		stick_out(LP_HOOK_FLASH,
			  static_cast<u16>(ms < 1	  ? 1 :
						ms > 2000 ? 2000 :
							    ms));
		return;
	}
	if (kind_ == Kind::Agere)
		agere_hook(false);
	else
		set_gpio(static_cast<u16>(gpio_ & ~GPIO_HOOK1));
	flash_end_ =
		samples_ + static_cast<u64>(ms) * rate_ / 1000;
}

/* see dcore.h: the hook GPIO cleared (broken) or set again */
s16 UsbLine::pulse_hook(dcore::Modem &, bool broken)
{
	if (hook_ != MODEM_HOOK_OFF)
		return -1;
	flash_end_ = 0;
	if (kind_ == Kind::Stick)
		return stick_out(LP_SET_HOOK, broken ? 0 : 1) ? 0 : -1;
	if (kind_ == Kind::Agere)
		return agere_hook(!broken) ? 0 : -1;
	u16 v = broken ? static_cast<u16>(gpio_ & ~GPIO_HOOK1) :
		static_cast<u16>(gpio_ | GPIO_HOOK1);
	if (intl_)
		v = intl_hook(v, !broken, false);
	return set_gpio(v) ? 0 : -1;
}

/* the international line controls (set_international()) of v, the GPIOs'
   outputs to be, as Windows' driver has them in its SetHookOn: off (off
   hook) or not, now_off a change to off hook.  Outside pulse dialling
   DP set off hook, DP and RFC cleared on hook; DC off hook as the
   country has it (the other way on an ST75951), else cleared; on hook
   the Australian timer cancelled, off hook (a change) started in
   Australia, RFC set till it ends */
u16 UsbLine::intl_hook(u16 v, bool off, bool now_off)
{
	if (!pulse_active_)
		v = off ? static_cast<u16>(v | GPIO_DP) :
			  static_cast<u16>(v & ~(GPIO_RFC | GPIO_DP));
	bool dc = st75951_ ? !intl_dc_ : intl_dc_;
	v = off && dc ? static_cast<u16>(v | GPIO_DC) :
			static_cast<u16>(v & ~GPIO_DC);
	if (!off) {
		aus_end_ = 0;
	} else if (now_off && intl_aus_ && !pulse_active_) {
		v |= GPIO_RFC;
		aus_end_ = samples_ +
			   static_cast<u64>(rate_) *
				   AUSTRALIAN_MS / 1000 +
			   1;
		DBG("Australian timer: RFC for %u ms\n", AUSTRALIAN_MS);
	}
	return v;
}

/* see usb_line.h: as Windows' driver, RFC set through pulse dialling
   (the Australian timer cancelled) and cleared after it */
void UsbLine::pulse_dialling(dcore::Modem &, bool on)
{
	if (!intl_ || kind_ != Kind::St7554 || on == pulse_active_)
		return;
	pulse_active_ = on;
	if (on)
		aus_end_ = 0;
	DBG("pulse dialling %s: RFC %s\n", on ? "begins" : "over", on ? "on" : "off");
	set_gpio(on ? static_cast<u16>(gpio_ | GPIO_RFC) :
		      static_cast<u16>(gpio_ & ~GPIO_RFC));
}

/* see usb_line.h */
void UsbLine::watch(RunLoop &loop)
{
	const libusb_pollfd **fds;

	if (!ctx_)
		return;
#ifdef _WIN32
	(void)loop;
	(void)fds;
	return;
#endif
	fds = libusb_get_pollfds(ctx_);
	if (!fds)
		return;
	for (s16 i = 0; fds[i]; i++)
		if (fds[i]->events & POLLIN)
			loop.watch(fds[i]->fd);
		else if (fds[i]->events & POLLOUT)
			loop.watch_write(fds[i]->fd);
	libusb_free_pollfds(fds);
}

/* see usb_line.h */
s16 UsbLine::pump(dcore::Modem &m)
{
	struct timeval zero = { 0, 0 };

	if (dead_)
		return -1;
	libusb_handle_events_timeout_completed(ctx_, &zero, nullptr);
	if (!dead_ && !parked_.empty())
		resume();
	if (dead_)
		return -1;
	if (handset_due_ && parked_.empty()) {
		u16 status;
		handset_due_ = false;
		if (get_reg(GPIO_STATUS, &status))
			m.local_phone(status & GPIO_HANDSET);
	}
	while (rings_) {
		rings_--;
		if (hook_ != MODEM_HOOK_OFF)
			m.ring();
	}
	if (viso_.ep_in) {
		/* the handset's microphone at the line's rate */
		resample(viso_.in_buf.data(), viso_.in_buf.size(), VOICE_RATE,
			 rate_, mic_pos_, mic_last_, mic_);
		viso_.in_buf.clear();
	}
	size_t used = 0;
	while (iso_.in_buf.size() - used >= static_cast<size_t>(frame_)) {
		std::copy_n(iso_.in_buf.data() + used, frame_,
			    frame_in_.data());
		used += frame_;
		if (aus_end_ && samples_ >= aus_end_) {
			aus_end_ = 0;
			DBG("Australian timer over: RFC off\n");
			set_gpio(static_cast<u16>(gpio_ & ~GPIO_RFC));
		}
		if (flash_end_ && samples_ >= flash_end_) {
			flash_end_ = 0;
			if (hook_ == MODEM_HOOK_OFF && kind_ == Kind::Agere)
				agere_hook(true);
			else if (hook_ == MODEM_HOOK_OFF)
				set_gpio(static_cast<u16>(gpio_ |
							       GPIO_HOOK1));
		}
		samples_ += frame_;
		if (viso_.ep_in)
			voice_frame(m);
		else if (running_ && m.started())
			m.process(frame_in_.data(), frame_out_.data(), frame_);
		else
			std::fill_n(frame_out_.data(), frame_, 0);
		/* (DMODEM_USB_TX_GAIN: louder or softer to the line) */
		if (tx_auto_ && hook_ == MODEM_HOOK_OFF)
			tx_auto_frame();
		if (tx_gain_ != 4096)
			for (s16 k = 0; k < frame_; k++) {
				s32 x = (frame_out_[k] * tx_gain_) / 4096;
				if (x > 32767 || x < -32768) {
					x = x > 0 ? 32767 : -32768;
					if (tx_clipped_++ == 0)
						DBG("the modem's samples clipped: DMODEM_USB_TX_GAIN too high\n");
				}
				frame_out_[k] = static_cast<s16>(x);
			}
		if (rec_)
			rec_frame(frame_in_.data(), frame_out_.data(), frame_);
		iso_.out_buf.insert(iso_.out_buf.end(), frame_out_.begin(),
				    frame_out_.end());
	}
	iso_.in_buf.erase(iso_.in_buf.begin(), iso_.in_buf.begin() + used);
	if (kind_ == Kind::Agere && used)
		agere_rate_check();
	if (kind_ == Kind::Agere && hook_ != MODEM_HOOK_OFF)
		agere_ring_poll();
	update_leds(m);
	/* (a playback that has drifted ahead: dropped back to its lead;
	   so is the handset's, and its microphone's samples not taken) */
	size_t most = static_cast<size_t>(3 * frame_);
	/* (the Agere modem's playback catches up after a burst of its
	   samples: agere_ahead_ more) */
	if (kind_ == Kind::Agere)
		most += static_cast<size_t>(agere_ahead_);
	if (iso_.out_buf.size() > most) {
		DBG("playback ahead: %zu samples dropped\n",
		    iso_.out_buf.size() - most);
		iso_.out_buf.erase(iso_.out_buf.begin(), iso_.out_buf.begin() +
				   (iso_.out_buf.size() - most));
	}
	size_t vmost = 3 * VOICE_RATE / 50;
	if (viso_.out_buf.size() > vmost)
		viso_.out_buf.erase(viso_.out_buf.begin(), viso_.out_buf.begin() +
				    (viso_.out_buf.size() - vmost));
	if (mic_.size() > most)
		mic_.erase(mic_.begin(), mic_.begin() + (mic_.size() - most));
	return 0;
}

/* FIFO_CONTROL2 as it is to be: the thresholds, and with set_rhom()
   while the modem runs RHOM for the codec, and for the voice codec if
   it streams (Windows' ConfigureRHOM) */
u16 UsbLine::fifo_control2() const
{
	u16 v = FIFO2_THRESHOLDS;
	if (rhom_ && rhom_ok_ && running_)
		v |= RHOM_SSI1 | (viso_.ep_in ? RHOM_SSI2 : 0);
	return v;
}

/* RHOM set or cleared (start(), stop()): FIFO_CONTROL2 written if it
   changes */
void UsbLine::update_rhom()
{
	u16 v = fifo_control2();
	if (kind_ != Kind::St7554 || !h_ || dead_ || v == fifo2_)
		return;
	DBG("RHOM %s\n", v & RHOM_SSI1 ? "on" : "off");
	if (set_reg(FIFO_CONTROL2, v))
		fifo2_ = v;
}

/* see dcore.h: the next frames' samples routed so (voice_frame()) */
s16 UsbLine::voice_route(dcore::Modem &, u16 device)
{
	if (device != VOICE_DEVICE_LINE && !viso_.ep_in)
		return -1;
	if (device != route_)
		DBG("voice: %s\n", device == VOICE_DEVICE_HANDSET ?
		    "the handset's audio, the line's left" :
		    device == VOICE_DEVICE_HANDSET_LINE ?
		    "the handset on the call" : "the line's audio");
	route_ = device;
	return 0;
}

/* a frame with the voice codec (pump(), frame_in_ the line's): the
   modem's samples the line's (frame_out_ to it, the handset silent),
   the handset's (+VLS=14: its microphone in, the modem's out to its
   earpiece, the line silent), or with the handset on the call (15: the
   modem hears the line and the microphone, the line gets the modem's
   and the microphone, the earpiece the line and the modem); the
   earpiece's samples to the voice codec at its rate */
void UsbLine::voice_frame(dcore::Modem &m)
{
	bool on = running_ && m.started();
	for (s16 i = 0; i < frame_; i++) {
		if (mic_.empty()) {
			frame_mic_[i] = 0;
		} else {
			frame_mic_[i] = mic_.front();
			mic_.pop_front();
		}
	}
	if (!on) {
		std::fill_n(frame_out_.data(), frame_, 0);
		std::fill_n(frame_ear_.data(), frame_, 0);
	} else if (route_ == VOICE_DEVICE_HANDSET) {
		m.process(frame_mic_.data(), frame_ear_.data(), frame_);
		std::fill_n(frame_out_.data(), frame_, 0);
	} else if (route_ == VOICE_DEVICE_HANDSET_LINE) {
		std::vector<s16> &both = frame_ear_;
		for (s16 i = 0; i < frame_; i++)
			both[i] = mix(frame_in_[i], frame_mic_[i]);
		m.process(both.data(), frame_out_.data(), frame_);
		for (s16 i = 0; i < frame_; i++) {
			frame_ear_[i] = mix(frame_in_[i], frame_out_[i]);
			frame_out_[i] = mix(frame_out_[i], frame_mic_[i]);
		}
	} else {
		m.process(frame_in_.data(), frame_out_.data(), frame_);
		std::fill_n(frame_ear_.data(), frame_, 0);
	}
	resample(frame_ear_.data(), static_cast<size_t>(frame_), rate_,
		 VOICE_RATE, ear_pos_, ear_last_, viso_.out_buf);
}

/* the transfers cancelled (and their callbacks run), on hook, the codec
   powered down, the interface released, libusb closed */
/* see usb_line.h: a WAV file's header for frames of 2 channels at the
   line's rate (sizes 0 until rec_stop()) */
static void wav_header(FILE *f, u32 rate, u64 frames)
{
	auto le = [f](u32 v, int n) {
		for (int i = 0; i < n; i++)
			fputc(static_cast<int>(v >> (8 * i) & 0xff), f);
	};
	u32 data = static_cast<u32>(std::min<u64>(frames * 4, 0xffffffd3u));
	fputs("RIFF", f);
	le(36 + data, 4);
	fputs("WAVEfmt ", f);
	le(16, 4);
	le(1, 2);
	le(2, 2);
	le(rate, 4);
	le(rate * 4, 4);
	le(4, 2);
	le(16, 2);
	fputs("data", f);
	le(data, 4);
}

/* see usb_line.h */
void UsbLine::rec_start()
{
	const char *p = getenv("DMODEM_USB_REC");
	if (rec_ || !p || !*p)
		return;
	char name[512], when[32];
	time_t t = time(nullptr);
	struct tm tm;
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &tm);
	snprintf(name, sizeof(name), "%s-%s.wav", p, when);
	rec_ = fopen(name, "wb");
	if (!rec_) {
		ERR("the line's recording: %s: %s\n", name, strerror(errno));
		return;
	}
	rec_frames_ = 0;
	wav_header(rec_, rate_, 0);
	DBG("recording the line to %s\n", name);
}

/* see usb_line.h */
/* DMODEM_USB_TX_GAIN=auto: the remote's answer tone (2100 Hz, while the
   modem is quiet: the caller's side) heard at TONE_REF dBFS wants
   GAIN_REF dB (the Hiro H50113 on its first line: +6 and +10 connected),
   half a dB more for each dB softer (a longer line), GAIN_MIN to GAIN_MAX;
   GAIN_START until a tone is heard (the answerer's side: the last call's
   tone's) */
namespace {
const double TONE_REF = -14.8, GAIN_REF = 8.0, GAIN_SLOPE = 0.5,
	GAIN_MIN = 0.0, GAIN_MAX = 12.0, GAIN_START = 6.0;
/* the last tone's level (dBFS; NaN: none yet), for the next call */
double last_tone_db = std::nan("");

double gain_for(double tone_db)
{
	return std::max(GAIN_MIN, std::min(GAIN_MAX,
		GAIN_REF + GAIN_SLOPE * (TONE_REF - tone_db)));
}

/* a frame's level, dBFS */
double level_db(const s16 *x, s16 n)
{
	double p = 0;
	for (s16 i = 0; i < n; i++)
		p += double(x[i]) * x[i];
	return 10.0 * std::log10(p / n / (32768.0 * 32768.0) + 1e-12);
}

/* the share of a frame's power at f Hz (1: all of it) */
double share_at(const s16 *x, s16 n, double f, u32 rate)
{
	double re = 0, im = 0, p = 0, w = 2.0 * 3.14159265358979323846 * f / rate;
	for (s16 i = 0; i < n; i++) {
		re += x[i] * std::cos(w * i);
		im -= x[i] * std::sin(w * i);
		p += double(x[i]) * x[i];
	}
	return p > 0 ? (re * re + im * im) / (double(n) * p / 2.0) : 0;
}
} // namespace

void UsbLine::tx_gain_db(double db)
{
	tx_auto_db_ = db;
	tx_gain_ = static_cast<s32>(std::lround(4096.0 * std::pow(10.0, db / 20.0)));
}

void UsbLine::tx_auto_start()
{
	tx_gain_db(std::isnan(last_tone_db) ? GAIN_START : gain_for(last_tone_db));
	tx_auto_set_ = false;
	tx_tone_n_ = 0;
	tx_clip_mark_ = tx_clipped_;
	tx_clip_frames_ = 0;
}

void UsbLine::tx_auto_frame()
{
	const s16 TONE_FRAMES = sizeof(tx_tone_db_) / sizeof(tx_tone_db_[0]);
	if (!tx_auto_set_ && frame_ > 0) {
		double rx = level_db(frame_in_.data(), frame_);
		if (rx > -45.0 && level_db(frame_out_.data(), frame_) < -50.0 &&
		    share_at(frame_in_.data(), frame_, 2100.0, rate_) > 0.6) {
			tx_tone_db_[tx_tone_n_++] = rx;
			if (tx_tone_n_ == TONE_FRAMES) {
				std::sort(tx_tone_db_, tx_tone_db_ + TONE_FRAMES);
				last_tone_db = tx_tone_db_[TONE_FRAMES / 2];
				tx_gain_db(gain_for(last_tone_db));
				tx_auto_set_ = true;
				DBG("the remote's answer tone at %.1f dBFS: the modem's samples to "
				    "the line %+.1f dB (auto)\n", last_tone_db, tx_auto_db_);
			}
		}
	}
	/* (a dB softer for each second of clipping, more than 8 samples) */
	if (++tx_clip_frames_ >= 50) {
		if (tx_clipped_ - tx_clip_mark_ > 8 && tx_auto_db_ > GAIN_MIN) {
			tx_gain_db(std::max(GAIN_MIN, tx_auto_db_ - 1.0));
			DBG("%" PRIu64 " samples clipped in a second: the modem's samples to "
			    "the line %+.1f dB (auto)\n", tx_clipped_ - tx_clip_mark_, tx_auto_db_);
		}
		tx_clip_mark_ = tx_clipped_;
		tx_clip_frames_ = 0;
	}
}

void UsbLine::rec_frame(const s16 *in, const s16 *out, s16 n)
{
	u8 b[4 * 512];
	s16 k = 0;
	while (k < n) {
		s16 m = static_cast<s16>(std::min<int>(n - k, 512));
		for (s16 i = 0; i < m; i++) {
			u16 l = static_cast<u16>(in[k + i]), r = static_cast<u16>(out[k + i]);
			b[4 * i] = static_cast<u8>(l);
			b[4 * i + 1] = static_cast<u8>(l >> 8);
			b[4 * i + 2] = static_cast<u8>(r);
			b[4 * i + 3] = static_cast<u8>(r >> 8);
		}
		fwrite(b, 4, static_cast<size_t>(m), rec_);
		k = static_cast<s16>(k + m);
	}
	rec_frames_ += static_cast<u64>(n);
}

/* see usb_line.h */
void UsbLine::rec_stop()
{
	if (!rec_)
		return;
	fflush(rec_);
	rewind(rec_);
	wav_header(rec_, rate_, rec_frames_);
	fclose(rec_);
	rec_ = nullptr;
	DBG("the line recorded: %" PRIu64 " s\n", rate_ ? rec_frames_ / rate_ : 0);
}

void UsbLine::close()
{
	rec_stop();
	if (h_ && !dead_ && kind_ == Kind::Agere && claimed_ && agere_codec_) {
		/* the stream stopped, on hook, GPIOs 2 and 3 as CUsbStop leaves
		   them, the OH LED (GPIO 4) out (not before its codec is known:
		   a Polaris's hook is the other way round) */
		u16 v;
		agere_stop();
		agere_hook(false);
		if (agere_read16(0x30000008, &v))
			agere_write16(0x30000008, static_cast<u16>(
				((v & 0xfff3) | 0x000c) & ~0x0010));
	}
	if (h_ && !dead_ && kind_ == Kind::St7554) {
		set_reg(FIFO_MASK, 0);
		set_reg(SSI1_COUNTER, 0);
		if (viso_.ep_in)
			set_reg(SSI2_COUNTER, 0);
		if (wake_)
			arm_wake(false);
	}
	libusb_transfer *all[4 * MAX_TRANSFERS + 1];
	s16 n = 0;
	for (Iso *st : { &iso_, &viso_ })
		for (s16 i = 0; i < MAX_TRANSFERS; i++) {
			all[n++] = st->in[i];
			all[n++] = st->out[i];
		}
	all[n++] = intr_;
	dead_ = true;
	for (s16 i = 0; i < n; i++)
		if (all[i])
			libusb_cancel_transfer(all[i]);
	for (s16 tries = 0; ctx_ && in_flight_ > 0 && tries < 100; tries++) {
		struct timeval tv = { 0, 10000 };
		libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
	}
	for (s16 i = 0; i < n; i++)
		if (all[i])
			libusb_free_transfer(all[i]);
	for (Iso *st : { &iso_, &viso_ })
		for (s16 i = 0; i < MAX_TRANSFERS; i++)
			st->in[i] = st->out[i] = nullptr;
	intr_ = nullptr;
	if (h_ && kind_ == Kind::Agere && claimed_)
		/* (no stream to the next open) */
		libusb_set_interface_alt_setting(h_, 0, 0);
	else if (h_ && kind_ == Kind::Stick) {
		/* on hook, the samples stopped (alternate setting 0) */
		if (claimed_) {
			libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
						LIBUSB_RECIPIENT_INTERFACE |
						LIBUSB_ENDPOINT_OUT, LP_SET_HOOK, 0,
						0, nullptr, 0, TIMEOUT_MS);
			libusb_set_interface_alt_setting(h_, 0, 0);
		}
	} else if (h_ && kind_ == Kind::St7554) {
		set_reg(FIFO_MASK, 0);
		set_reg(GPIO_MASK, 0);
		set_reg(GPIO_OUT, 0);
		set_reg(SSI1_CONTROL, st75951_ ? SSI1_ST75951 : SSI1_POWERDOWN);
		if (viso_.ep_in)
			set_reg(SSI2_CONTROL, 0);
	}
	if (h_) {
		if (claimed_)
			libusb_release_interface(h_, 0);
		libusb_close(h_);
		h_ = nullptr;
	}
	claimed_ = false;
	if (ctx_) {
		libusb_exit(ctx_);
		ctx_ = nullptr;
	}
}

} // namespace dmd
