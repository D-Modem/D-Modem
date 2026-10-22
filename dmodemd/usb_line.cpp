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

#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <log.h>

#include "usb_line.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "usb: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/* the ST7554 */
const uint16_t ST7554_VENDOR = 0x0483;
const uint16_t ST7554_PRODUCT = 0x7554;

/* the pico-serial Line stick (a pid.codes test PID until it has its own) */
const uint16_t STICK_VENDOR = 0x1209;
const uint16_t STICK_PRODUCT = 0x0002;
/* its vendor requests (firmware/src/core/line_proto.h) */
const uint8_t LP_GET_INFO = 0x01;
const uint8_t LP_GET_STATUS = 0x02;
const uint8_t LP_SET_RATE = 0x10;
const uint8_t LP_SET_HOOK = 0x11;
const uint8_t LP_HOOK_FLASH = 0x12;
const uint8_t LP_SET_MONITOR = 0x13;
const uint8_t LP_DAA_READ = 0x20;
const uint8_t LP_DAA_WRITE = 0x21;
/* its state bits, and the Si3056's interrupt sources it passes on */
const uint8_t LP_ST_LINK = 0x01;
const uint8_t LP_INT_OVERLOAD = 0x40;
const uint8_t LP_INT_LINK_LOST = 0x20;
const uint8_t LP_INT_DROPOUT = 0x08;
const uint8_t LP_INT_POLARITY = 0x01;
/* its delay: the playback's head start, the Si3056's filters both ways */
const int STICK_FIFO_SAMPLES = 32 + 12 + 12;

/* the Si3056's rates (register 7's) */
bool stick_rate(unsigned rate)
{
	static const unsigned rates[] = { 7200, 8000, 8229, 8400, 9000, 9600,
					  10286, 12000, 13714, 16000 };
	for (unsigned r : rates)
		if (r == rate)
			return true;
	return false;
}

/* its vendor requests (registers) */
const uint8_t SSI1_CONTROL = 0x01;
const uint8_t FIFO_MASK = 0x03;
const uint8_t SSI1_COUNTER = 0x04;
const uint8_t SSI2_COUNTER = 0x06;
const uint8_t GPIO_DIR = 0x08;
const uint8_t GPIO_OUT = 0x0a;
const uint8_t GPIO_MASK = 0x0c;
const uint8_t GPIO_INV = 0x0e;
const uint8_t GPIO_STATUS = 0x15;
const uint8_t SSI1_CWORD = 0x17;
const uint8_t FIFO_CONTROL1 = 0x19;
const uint8_t FIFO_CONTROL2 = 0x1a;

/* their values */
const uint16_t SSI1_POWERDOWN = 0x0000;
const uint16_t SSI1_POWERUP = 0x0001;
const uint16_t SSI1_UNDERRUN = 0x0040;
const uint16_t SSI1_OVERRUN = 0x0020;
const uint16_t GPIO_RING1 = 0x0001;
const uint16_t GPIO_HOOK1 = 0x0008;
const uint16_t GPIO_LED_HOOK = 0x0080;
const uint16_t GPIO_MONOLITHIC = 0x4000;   /* an ST75951/2 silicon DAA */
const uint16_t BYTE_ORDER_LE = 0x8000;

/* the FIFO's start: its count, and the octets of its own (and the
   device's) delay */
const uint16_t FRAGSIZE = 96;
const int FIFO_SAMPLES = 64 + 24;

const unsigned TIMEOUT_MS = 1000;

/* the STLC7550's control word for a rate; 0 if it has none */
uint16_t cword(unsigned rate)
{
	switch (rate) {
	case 8000: return 0x3c8;
	case 9600: return 0x3c0;
	case 16000: return 0x3f0;
	}
	return 0;
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
	int bus = -1, addr = -1, found = 0;
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
		if (!st7554 && !stick)
			continue;
		found++;
		if (!any && (libusb_get_bus_number(list[i]) != bus ||
			     libusb_get_device_address(list[i]) != addr))
			continue;
		kind_ = stick ? Kind::Stick : Kind::St7554;
		name_ = stick ? "pico-serial Line" : "SmartUSB56";
		int err = libusb_open(list[i], &h_);
		if (err < 0) {
			const char *id = stick ? "1209:0002" : "0483:7554";
			std::string hint;
#ifdef _WIN32
			/* (another driver than WinUSB has it: Smart Link's
			   own, or none) */
			if (err == LIBUSB_ERROR_NOT_SUPPORTED ||
			    err == LIBUSB_ERROR_ACCESS)
				hint = std::string(" (it needs WinUSB as its "
					"driver: dmodem-driver usb "
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
		ERR("no Smart Link SmartUSB56 (USB 0483:7554) or pico-serial "
		    "Line (1209:0002)%s%s\n", any ? "" : " at ", any ? "" : dev);
	else if (!h_ && found && !any)
		ERR("no SmartUSB56 or pico-serial Line at %s (%d elsewhere)\n",
		    dev, found);
	return h_ != nullptr;
}

/* see usb_line.h */
int UsbLine::list()
{
	libusb_context *ctx;
	libusb_device **devs;
	int err, found = 0;

	if ((err = libusb_init(&ctx)) < 0) {
		ERR("libusb: %s\n", libusb_strerror(err));
		return -1;
	}
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
		if (!st7554 && !stick)
			continue;
		found++;
		err = libusb_open(devs[i], &h);
		if (err == 0)
			libusb_close(h);
		printf("%03d:%03d %04x:%04x %s: %s%s\n",
		       libusb_get_bus_number(devs[i]),
		       libusb_get_device_address(devs[i]), d.idVendor,
		       d.idProduct, stick ? "pico-serial Line" : "SmartUSB56",
		       err == 0 ? "ready" : libusb_strerror(err),
#ifdef _WIN32
		       err == LIBUSB_ERROR_NOT_SUPPORTED ?
		       " (its driver is not WinUSB)" :
#endif
		       "");
	}
	if (n >= 0)
		libusb_free_device_list(devs, 1);
	if (!found)
		printf("no SmartUSB56 (0483:7554) or pico-serial Line "
		       "(1209:0002)\n");
	libusb_exit(ctx);
	return 0;
}

/* the endpoints of interface 0, alternate setting 1: interrupt IN,
   isochronous OUT, isochronous IN */
bool UsbLine::endpoints()
{
	libusb_config_descriptor *cfg;
	int err = libusb_get_active_config_descriptor(libusb_get_device(h_),
						      &cfg);
	if (err < 0) {
		ERR("%s: its configuration: %s\n", name_, libusb_strerror(err));
		return false;
	}
	if (cfg->bNumInterfaces >= 1 && cfg->interface[0].num_altsetting >= 2) {
		const libusb_interface_descriptor &alt =
			cfg->interface[0].altsetting[1];
		for (int i = 0; i < alt.bNumEndpoints; i++) {
			const libusb_endpoint_descriptor &e = alt.endpoint[i];
			int type = e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
			bool in = e.bEndpointAddress & LIBUSB_ENDPOINT_IN;
			if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in)
				ep_intr_ = e.bEndpointAddress;
			else if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && in) {
				ep_in_ = e.bEndpointAddress;
				max_in_ = e.wMaxPacketSize & 0x7ff;
			} else if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
				ep_out_ = e.bEndpointAddress;
				max_out_ = e.wMaxPacketSize & 0x7ff;
			}
		}
	}
	libusb_free_config_descriptor(cfg);
	if (!ep_intr_ || !ep_in_ || !ep_out_ || !max_in_ || !max_out_) {
		ERR("%s: not the endpoints of %s\n", name_,
		    kind_ == Kind::Stick ? "a pico-serial Line" : "an ST7554");
		return false;
	}
	DBG("endpoints: interrupt %02x, out %02x (%d), in %02x (%d)\n",
	    ep_intr_, ep_out_, max_out_, ep_in_, max_in_);
	return true;
}

/* a register of the ST7554's (a vendor request); false (logged) */
bool UsbLine::set_reg(uint8_t reg, uint16_t v)
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
bool UsbLine::get_reg(uint8_t reg, uint16_t *v)
{
	uint8_t b[2] = { 0, 0 };
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
	*v = (uint16_t)(b[0] | b[1] << 8);
	return true;
}

/* the GPIOs' outputs */
bool UsbLine::set_gpio(uint16_t v)
{
	if (!set_reg(GPIO_OUT, v))
		return false;
	gpio_ = v;
	return true;
}

/* see usb_line.h: the device, its interface, then its own start-up
   (open_st7554(), open_stick()), on hook, the transfers both ways and
   the interrupt's */
bool UsbLine::open(const char *dev, unsigned rate)
{
	int err;

	rate_ = rate;
	frame_ = (int)rate / 50;
	frame_in_.assign(frame_, 0);
	frame_out_.assign(frame_, 0);
	if ((err = libusb_init(&ctx_)) < 0) {
		ERR("libusb: %s\n", libusb_strerror(err));
		ctx_ = nullptr;
		return false;
	}
	if (!find(dev)) {
		close();
		return false;
	}
	/* (slusb, Smart Link's kernel driver, if it is there) */
	libusb_set_auto_detach_kernel_driver(h_, 1);
	if ((err = libusb_claim_interface(h_, 0)) < 0) {
		ERR("%s: its interface: %s\n", name_, libusb_strerror(err));
		close();
		return false;
	}
	claimed_ = true;
	if (!(kind_ == Kind::Stick ? open_stick(rate) : open_st7554(rate)) ||
	    !start_streams()) {
		close();
		return false;
	}
	DBG("%u Hz, %d samples ahead\n", rate, delay_);
	return true;
}

/* the ST7554: the rate's control word, alternate setting 1, its codec
   checked, its registers as Smart Link's driver sets them, on hook */
bool UsbLine::open_st7554(unsigned rate)
{
	uint16_t status;
	int err;

	if (!cword(rate)) {
		ERR("SmartUSB56: no rate of %u Hz (8000, 9600)\n", rate);
		return false;
	}
	if ((err = libusb_set_interface_alt_setting(h_, 0, 1)) < 0) {
		ERR("SmartUSB56: alternate setting 1: %s\n",
		    libusb_strerror(err));
		return false;
	}
	if (!endpoints() || !get_reg(GPIO_STATUS, &status))
		return false;
	if (status & GPIO_MONOLITHIC) {
		ERR("SmartUSB56: an ST75951/2 silicon DAA codec, which Smart "
		    "Link's driver did not support either\n");
		return false;
	}
	gpio_ = 0;
	if (!set_reg(GPIO_DIR, 0x3ff8) || !set_gpio(0) ||
	    !set_reg(GPIO_MASK, GPIO_RING1) ||
	    !set_reg(FIFO_CONTROL1, 0x2828 | BYTE_ORDER_LE) ||
	    !set_reg(FIFO_CONTROL2, 0x2828) || !set_reg(FIFO_MASK, 0) ||
	    !set_reg(FIFO_MASK, SSI1_UNDERRUN | SSI1_OVERRUN) ||
	    !set_reg(SSI1_COUNTER, 0) || !set_reg(SSI2_COUNTER, 0) ||
	    !set_reg(SSI1_CONTROL, SSI1_POWERUP) ||
	    !set_reg(SSI1_CWORD, cword(rate)) || !set_reg(GPIO_INV, 0))
		return false;
	libusb_clear_halt(h_, ep_in_);
	libusb_clear_halt(h_, ep_out_);
	intr_len_ = 4;
	fifo_samples_ = FIFO_SAMPLES;
	return true;
}

/* the stick: its version and line side (GET_INFO), the rate, on hook with
   the line monitor on, its ring detector's count so far, then alternate
   setting 1, which starts its samples */
bool UsbLine::open_stick(unsigned rate)
{
	uint8_t info[12], status[20];
	int err;

	if (!stick_rate(rate)) {
		ERR("pico-serial Line: no rate of %u Hz (8000, 9600, 16000 and "
		    "the Si3056's others)\n", rate);
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
	if (!stick_out(LP_SET_RATE, (uint16_t)rate) ||
	    !stick_out(LP_SET_HOOK, 0) || !stick_out(LP_SET_MONITOR, 1) ||
	    !stick_in(LP_GET_STATUS, 0, status, sizeof(status)))
		return false;
	if (!stick_country())
		return false;
	ring_edges_ = status[18];
	underruns_ = (uint16_t)(status[6] | status[7] << 8);
	overruns_ = (uint16_t)(status[8] | status[9] << 8);
	if ((err = libusb_set_interface_alt_setting(h_, 0, 1)) < 0) {
		ERR("pico-serial Line: alternate setting 1: %s\n",
		    libusb_strerror(err));
		return false;
	}
	if (!endpoints())
		return false;
	intr_len_ = 8;
	fifo_samples_ = STICK_FIFO_SAMPLES;
	return true;
}

/* see usb_line.h */
bool UsbLine::stick_out(uint8_t req, uint16_t value)
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
bool UsbLine::stick_in(uint8_t req, uint16_t value, uint8_t *buf, uint16_t len)
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

/* the transfers: the interrupt's, ISO_TRANSFERS IN ones, the OUT ones
   (each sent when an IN one is done; the first ones now, a frame's
   worth of silence each), then the FIFO started */
bool UsbLine::start_streams()
{
	int err;
	int pkt = (int)((rate_ * 2 + 999) / 1000);   /* octets a ms, at most */

	out_buf_.assign(frame_ + frame_ / 2, 0);
	delay_ = (int)out_buf_.size() + ISO_TRANSFERS * ISO_PACKETS *
		(int)rate_ / 1000 + fifo_samples_;
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
	for (int i = 0; i < ISO_TRANSFERS; i++) {
		in_[i] = libusb_alloc_transfer(ISO_PACKETS);
		out_[i] = libusb_alloc_transfer(ISO_PACKETS);
		if (!in_[i] || !out_[i])
			return false;
		uint8_t *ib = (uint8_t *)calloc(ISO_PACKETS, max_in_);
		uint8_t *ob = (uint8_t *)calloc(ISO_PACKETS, max_out_);
		if (!ib || !ob) {
			free(ib);
			free(ob);
			return false;
		}
		libusb_fill_iso_transfer(in_[i], h_, ep_in_, ib,
					 ISO_PACKETS * max_in_, ISO_PACKETS,
					 in_done, this, 0);
		libusb_set_iso_packet_lengths(in_[i], max_in_);
		in_[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_fill_iso_transfer(out_[i], h_, ep_out_, ob,
					 ISO_PACKETS * max_out_, ISO_PACKETS,
					 out_done, this, 0);
		out_[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		if ((err = libusb_submit_transfer(in_[i])) < 0) {
			ERR("%s: an IN transfer: %s\n", name_,
			    libusb_strerror(err));
			return false;
		}
		in_flight_++;
		std::vector<int> lens(ISO_PACKETS, pkt > max_out_ ? max_out_ : pkt);
		out_fill(out_[i], lens.data());
		if ((err = libusb_submit_transfer(out_[i])) < 0) {
			ERR("%s: an OUT transfer: %s\n", name_,
			    libusb_strerror(err));
			return false;
		}
		in_flight_++;
	}
	/* the ST7554's FIFO: started, its errors on the interrupt (the
	   stick's samples run since its alternate setting 1) */
	return kind_ == Kind::Stick || (set_reg(SSI1_COUNTER, FRAGSIZE) &&
					set_reg(FIFO_MASK, SSI1_UNDERRUN | SSI1_OVERRUN));
}

/* an OUT transfer's packets of lengths octets (even), the modem's
   samples queued in them (silence past them) */
void UsbLine::out_fill(libusb_transfer *t, const int *lengths)
{
	int off = 0;
	for (int i = 0; i < ISO_PACKETS; i++) {
		int len = lengths[i] & ~1;
		if (len > max_out_)
			len = max_out_;
		t->iso_packet_desc[i].length = (unsigned)len;
		for (int k = 0; k < len / 2; k++) {
			int16_t s = 0;
			if (!out_buf_.empty()) {
				s = out_buf_.front();
				out_buf_.pop_front();
			}
			t->buffer[off + 2 * k] = (uint8_t)s;
			t->buffer[off + 2 * k + 1] = (uint8_t)(s >> 8);
		}
		off += len;
	}
	t->length = off;
}

/* libusb's callbacks: to the line */
void LIBUSB_CALL UsbLine::in_done(libusb_transfer *t)
{
	static_cast<UsbLine *>(t->user_data)->in_complete(t);
}

/* an OUT transfer done: idle again, sent once there are
   lengths for it (the line gone if unplugged) */
void LIBUSB_CALL UsbLine::out_done(libusb_transfer *t)
{
	UsbLine *l = static_cast<UsbLine *>(t->user_data);
	l->in_flight_--;
	if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
		l->gone("unplugged");
	if (l->dead_ || t->status == LIBUSB_TRANSFER_CANCELLED)
		return;
	/* sent again with the lengths an IN transfer has given, or once
	   one does */
	l->idle_out_.push_back(t);
	l->send_out();
}

/* the idle OUT transfers sent, as many as there are IN transfers' lengths
   for */
void UsbLine::send_out()
{
	while (!idle_out_.empty() && !out_lengths_.empty()) {
		libusb_transfer *t = idle_out_.front();
		idle_out_.pop_front();
		out_fill(t, out_lengths_.front().data());
		out_lengths_.pop_front();
		int err = libusb_submit_transfer(t);
		if (err == 0)
			in_flight_++;
		else
			gone(err == LIBUSB_ERROR_NO_DEVICE ? "unplugged" :
			     "an OUT transfer failed");
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
		uint16_t fifo = (uint16_t)(t->buffer[0] | t->buffer[1] << 8);
		uint16_t gpio = (uint16_t)(t->buffer[2] | t->buffer[3] << 8);
		if (fifo & SSI1_UNDERRUN)
			DBG("FIFO underrun\n");
		if (fifo & SSI1_OVERRUN)
			DBG("FIFO overrun\n");
		if (gpio & GPIO_RING1)
			l->rings_++;
	}
	if (libusb_submit_transfer(t) == 0)
		l->in_flight_++;
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
	if (h_ && kind_ == Kind::Stick && claimed_ && hook_ != MODEM_HOOK_OFF)
		stick_country();
}

/* see usb_line.h: registers 16, 26, 31, 59, and 30 on a Si3019 */
bool UsbLine::stick_country()
{
	Si3056Regs r;
	uint8_t v;
	struct {
		uint8_t reg;
		uint8_t *val;
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
		if (!stick_out(LP_DAA_WRITE, (uint16_t)(x.reg << 8 | *x.val)))
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
void UsbLine::stick_event(const uint8_t *b, int len)
{
	if (len < 8)
		return;
	uint8_t edges = (uint8_t)(b[1] - ring_edges_);
	uint16_t under = (uint16_t)(b[4] | b[5] << 8);
	uint16_t over = (uint16_t)(b[6] | b[7] << 8);

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
		DBG("playback underrun (%u)\n", (unsigned)(uint16_t)(under - underruns_));
	if (over != overruns_)
		DBG("FIFO overrun (%u)\n", (unsigned)(uint16_t)(over - overruns_));
	underruns_ = under;
	overruns_ = over;
}

/* an IN transfer done: its samples queued for the modem, its packets'
   lengths for an OUT transfer (one waiting sent now), and it again */
void UsbLine::in_complete(libusb_transfer *t)
{
	std::vector<int> lens(ISO_PACKETS, 0);

	in_flight_--;
	if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
		gone("unplugged");
		return;
	}
	if (dead_ || t->status == LIBUSB_TRANSFER_CANCELLED)
		return;
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		for (int i = 0; i < t->num_iso_packets; i++) {
			const libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
			const uint8_t *b = libusb_get_iso_packet_buffer_simple(t, i);
			if (p.status != LIBUSB_TRANSFER_COMPLETED)
				continue;
			for (unsigned k = 0; k + 1 < p.actual_length; k += 2)
				in_buf_.push_back((int16_t)(b[k] | b[k + 1] << 8));
			lens[i] = (int)p.actual_length;
		}
	}
	out_lengths_.push_back(lens);
	send_out();
	int err = libusb_submit_transfer(t);
	if (err == 0)
		in_flight_++;
	else
		gone(err == LIBUSB_ERROR_NO_DEVICE ? "unplugged" :
		     "an IN transfer failed");
}

/* the device has gone: no more transfers */
void UsbLine::gone(const char *why)
{
	if (!dead_)
		ERR("%s: %s\n", name_, why);
	dead_ = true;
}

/* see usb_line.h: the stick's line monitor on while on hook (rings and
   caller ID heard), off off hook, as an ALSA codec's "Caller ID Switch" */
int UsbLine::set_hook(dcore::Modem &, int state)
{
	bool off = state == MODEM_HOOK_OFF;

	DBG("%s\n", off ? "off hook" : "on hook");
	hook_ = state;
	flash_end_ = 0;
	if (kind_ == Kind::Stick)
		return off ? (stick_out(LP_SET_HOOK, 1) &&
			      stick_out(LP_SET_MONITOR, 0) ? 0 : -1) :
			(stick_out(LP_SET_HOOK, 0) &&
			 stick_out(LP_SET_MONITOR, 1) &&
			 (!daa_due_ || stick_country()) ? 0 : -1);
	return set_gpio(off ? (uint16_t)(gpio_ | GPIO_HOOK1 | GPIO_LED_HOOK) :
			(uint16_t)(gpio_ & ~(GPIO_HOOK1 | GPIO_LED_HOOK))) ? 0 : -1;
}

/* see dcore.h: on hook for ms, then off again (if it still is) */
void UsbLine::hook_flash(dcore::Modem &, unsigned ms)
{
	if (hook_ != MODEM_HOOK_OFF)
		return;
	DBG("hook flash, %u ms\n", ms);
	if (kind_ == Kind::Stick) {   /* the stick times it */
		stick_out(LP_HOOK_FLASH, (uint16_t)(ms < 1 ? 1 : ms > 2000 ? 2000 : ms));
		return;
	}
	set_gpio((uint16_t)(gpio_ & ~GPIO_HOOK1));
	flash_end_ = samples_ + (unsigned long long)ms * rate_ / 1000;
}

/* see dcore.h: the hook GPIO cleared (broken) or set again */
int UsbLine::pulse_hook(dcore::Modem &, bool broken)
{
	if (hook_ != MODEM_HOOK_OFF)
		return -1;
	flash_end_ = 0;
	if (kind_ == Kind::Stick)
		return stick_out(LP_SET_HOOK, broken ? 0 : 1) ? 0 : -1;
	return set_gpio(broken ? (uint16_t)(gpio_ & ~GPIO_HOOK1) :
			(uint16_t)(gpio_ | GPIO_HOOK1)) ? 0 : -1;
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
	for (int i = 0; fds[i]; i++)
		if (fds[i]->events & POLLIN)
			loop.watch(fds[i]->fd);
		else if (fds[i]->events & POLLOUT)
			loop.watch_write(fds[i]->fd);
	libusb_free_pollfds(fds);
}

/* see usb_line.h */
int UsbLine::pump(dcore::Modem &m)
{
	struct timeval zero = { 0, 0 };

	if (dead_)
		return -1;
	libusb_handle_events_timeout_completed(ctx_, &zero, nullptr);
	if (dead_)
		return -1;
	while (rings_) {
		rings_--;
		if (hook_ != MODEM_HOOK_OFF)
			m.ring();
	}
	size_t used = 0;
	while (in_buf_.size() - used >= (size_t)frame_) {
		memcpy(frame_in_.data(), in_buf_.data() + used,
		       frame_ * sizeof(int16_t));
		used += frame_;
		if (flash_end_ && samples_ >= flash_end_) {
			flash_end_ = 0;
			if (hook_ == MODEM_HOOK_OFF)
				set_gpio((uint16_t)(gpio_ | GPIO_HOOK1));
		}
		samples_ += frame_;
		if (running_ && m.started())
			m.process(frame_in_.data(), frame_out_.data(), frame_);
		else
			memset(frame_out_.data(), 0, frame_ * sizeof(int16_t));
		out_buf_.insert(out_buf_.end(), frame_out_.begin(),
				frame_out_.end());
	}
	in_buf_.erase(in_buf_.begin(), in_buf_.begin() + used);
	/* (a playback that has drifted ahead: dropped back to its lead) */
	size_t most = (size_t)(3 * frame_);
	if (out_buf_.size() > most) {
		DBG("playback ahead: %zu samples dropped\n",
		    out_buf_.size() - most);
		out_buf_.erase(out_buf_.begin(),
			       out_buf_.begin() + (out_buf_.size() - most));
	}
	return 0;
}

/* the transfers cancelled (and their callbacks run), on hook, the codec
   powered down, the interface released, libusb closed */
void UsbLine::close()
{
	if (h_ && !dead_ && kind_ == Kind::St7554) {
		set_reg(FIFO_MASK, 0);
		set_reg(SSI1_COUNTER, 0);
	}
	libusb_transfer *all[2 * ISO_TRANSFERS + 1];
	int n = 0;
	for (int i = 0; i < ISO_TRANSFERS; i++) {
		all[n++] = in_[i];
		all[n++] = out_[i];
	}
	all[n++] = intr_;
	dead_ = true;
	for (int i = 0; i < n; i++)
		if (all[i])
			libusb_cancel_transfer(all[i]);
	for (int tries = 0; ctx_ && in_flight_ > 0 && tries < 100; tries++) {
		struct timeval tv = { 0, 10000 };
		libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
	}
	for (int i = 0; i < n; i++)
		if (all[i])
			libusb_free_transfer(all[i]);
	for (int i = 0; i < ISO_TRANSFERS; i++)
		in_[i] = out_[i] = nullptr;
	intr_ = nullptr;
	if (h_ && kind_ == Kind::Stick) {
		/* on hook, the samples stopped (alternate setting 0) */
		if (claimed_) {
			libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
						LIBUSB_RECIPIENT_INTERFACE |
						LIBUSB_ENDPOINT_OUT, LP_SET_HOOK, 0,
						0, nullptr, 0, TIMEOUT_MS);
			libusb_set_interface_alt_setting(h_, 0, 0);
		}
	} else if (h_) {
		set_reg(FIFO_MASK, 0);
		set_reg(GPIO_MASK, 0);
		set_reg(GPIO_OUT, 0);
		set_reg(SSI1_CONTROL, SSI1_POWERDOWN);
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
