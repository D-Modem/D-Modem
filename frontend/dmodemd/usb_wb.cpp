/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_wb.cpp - the pico-serial WB stick (USB 1209:0003) as the modem's
 *                 line: UsbLine's Kind::Wb (usb_line.h)
 *
 *    The stick's protocol is its firmware's (pico-serial's
 *    firmware/PROTOCOL.md "WB stick", src/core/wb_proto.h): the Line
 *    stick's interface 0 (class 0xff, subclass 0x57), its alternate
 *    setting 1 with an isochronous OUT (0x01) and IN (0x82) endpoint of 1
 *    ms packets (at most 480 octets: S16 little-endian mono at 48000,
 *    96000 or 192000 Hz) and an interrupt IN one (0x83) of 8-octet
 *    events; vendor requests to the interface for the rest.  The
 *    transfers are the Line stick's (usb_line.cpp: an OUT transfer with
 *    the lengths of an IN one, so the playback keeps the codec's clock).
 *
 *    It has no hook, ring detector or caller ID: it sits on a bare pair,
 *    for the wideband mode (V.wb, DP_VWB, which the core runs by default
 *    at these rates) on a leased line (AT&L1).  Its LINK LED is the
 *    modem's carrier: SET_CARRIER on while the call is online, off
 *    otherwise (its firmware's own "auto", lit while the samples run,
 *    is put back at the end).  The hook's changes are only logged.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdio>

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

namespace dmd {

namespace {

/* the WB stick (a pid.codes test PID until it has its own) */
const u16 WB_VENDOR = 0x1209;
const u16 WB_PRODUCT = 0x0003;
/* GET_INFO's version and board (wb_proto.h's WP_VERSION, WP_BOARD) */
const u8 WP_VERSION = 1;
const u8 WP_BOARD = 4;
/* its vendor requests (wb_proto.h) */
const u8 WP_GET_INFO = 0x01;
const u8 WP_GET_STATUS = 0x02;
const u8 WP_SET_RATE = 0x10;
const u8 WP_SET_CARRIER = 0x11;
const u8 WP_SET_TX_GAIN = 0x12;
const u8 WP_SET_RX_GAIN = 0x13;
const u8 WP_SET_INPUT = 0x14;
const u8 WP_SET_LOOPBACK = 0x15;
/* SET_CARRIER's, SET_INPUT's and SET_LOOPBACK's values */
const u16 WP_CARRIER_OFF = 0, WP_CARRIER_ON = 1, WP_CARRIER_AUTO = 2;
const u16 WP_INPUT_HYBRID = 0;
const u16 WP_LOOP_OFF = 0;
/* its state bits */
const u8 WP_ST_CODEC = 0x01;
const u8 WP_ST_LOOPBACK = 0x04;
/* its codec's flags in an event or the status: the DACs' and ADCs'
   overflows (page 0 register 42), the drivers' over current (44) */
const u8 WP_OVF_DAC = 0xc0, WP_OVF_ADC = 0x0c;
const u8 WP_OC = 0xc0;
/* the gains' ranges, in its 0.5 dB steps */
const s16 TX_GAIN_MIN = -127, TX_GAIN_MAX = 48;
const s16 RX_GAIN_MAX = 95;
/* its delay at rate: the playback's head start (3 ms) and a DMA block of
   32 frames each way (the codec's filters not counted: not measured) */
s16 wb_fifo_samples(u32 rate)
{
	return static_cast<s16>(rate * 3 / 1000 + 2 * 32);
}

/* db in half dB steps, within lo..hi */
s16 half_db(double db, s16 lo, s16 hi)
{
	long v = std::lround(db * 2);
	return static_cast<s16>(v < lo ? lo : v > hi ? hi : v);
}

/* a little-endian u16 at b */
u16 le16(const u8 *b)
{
	return static_cast<u16>(b[0] | u16{b[1]} << 8);
}

} // namespace

/* see usb_line.h */
bool UsbLine::wb_device(u16 vendor, u16 product)
{
	return vendor == WB_VENDOR && product == WB_PRODUCT;
}

/* see usb_line.h */
bool UsbLine::wb_rate(u32 rate)
{
	return rate == 48000 || rate == 96000 || rate == 192000;
}

/* see usb_line.h: the requests are the Line stick's (stick_out(),
   stick_in()), each logged when it fails */
bool UsbLine::open_wb(u32 rate)
{
	u8 info[16], status[28];
	int err;

	if (!wb_rate(rate)) {
		ERR("pico-serial WB: no rate of %" PRIu32 " Hz (48000, 96000 or "
		    "192000: it is for the wideband mode)\n", rate);
		return false;
	}
	if (!stick_in(WP_GET_INFO, 0, info, sizeof(info)))
		return false;
	if (info[0] != WP_VERSION) {
		ERR("pico-serial WB: protocol version %u, not %u: newer "
		    "firmware than this dmodemd knows\n", info[0], WP_VERSION);
		return false;
	}
	if (info[11] != WP_BOARD) {
		ERR("pico-serial WB: board %u, not the WB stick's (%u)\n",
		    info[11], WP_BOARD);
		return false;
	}
	DBG("pico-serial WB, firmware %u.%u.%u, %" PRIu32 " Hz\n", info[1],
	    info[2], info[3], static_cast<u32>(info[4] | u32{info[5]} << 8 |
					       u32{info[6]} << 16 |
					       u32{info[7]} << 24));
	s16 tx = half_db(wb_tx_db_, TX_GAIN_MIN, TX_GAIN_MAX);
	s16 rx = half_db(wb_rx_db_, 0, RX_GAIN_MAX);
	if (!stick_out(WP_SET_LOOPBACK, WP_LOOP_OFF) ||
	    !stick_out(WP_SET_INPUT, WP_INPUT_HYBRID) ||
	    !stick_out(WP_SET_RATE, static_cast<u16>(rate / 1000)) ||
	    !stick_out(WP_SET_TX_GAIN, static_cast<u16>(tx)) ||
	    !stick_out(WP_SET_RX_GAIN, static_cast<u16>(rx)) ||
	    !stick_out(WP_SET_CARRIER, WP_CARRIER_OFF) ||
	    !stick_in(WP_GET_STATUS, 0, status, sizeof(status)))
		return false;
	wb_carrier_ = 0;
	if (tx || rx)
		DBG("pico-serial WB: transmit %+.1f dB, hybrid input +%.1f dB\n",
		    tx / 2.0, rx / 2.0);
	if (!(status[10] & WP_ST_CODEC))
		ERR("pico-serial WB: its codec does not run (no answer, or no "
		    "clock); going on\n");
	if (status[10] & WP_ST_LOOPBACK)
		ERR("pico-serial WB: a loopback stays on\n");
	underruns_ = le16(status + 12);
	overruns_ = le16(status + 14);
	if ((err = libusb_set_interface_alt_setting(h_, 0, 1)) < 0) {
		ERR("pico-serial WB: alternate setting 1: %s\n",
		    libusb_strerror(err));
		return false;
	}
	if (voice_law_ != VoiceLaw::None)
		ERR("pico-serial WB: no voice codec (--usb-voice-codec)\n");
	if (!endpoints(1))
		return false;
	intr_len_ = 8;
	fifo_samples_ = wb_fifo_samples(rate);
	return true;
}

/* see usb_line.h: the codec's overflows and over current, and the
   FIFOs' errors, logged (the state is the stick's own) */
void UsbLine::wb_event(const u8 *b, s16 len)
{
	if (len < 8)
		return;
	u16 under = le16(b + 4);
	u16 over = le16(b + 6);

	if (b[3] & WP_OC)
		ERR("pico-serial WB: over current on the line's drivers "
		    "(%02x)\n", b[3]);
	if (b[2] & WP_OVF_DAC)
		DBG("the DAC overflowed: the transmit level too high\n");
	if (b[2] & WP_OVF_ADC)
		DBG("the ADC overflowed: the line too loud (the hybrid's gain "
		    "too high?)\n");
	if (under != underruns_)
		DBG("playback underrun (%u)\n", (unsigned)(u16)(under - underruns_));
	if (over != overruns_)
		DBG("FIFO overrun (%u)\n", (unsigned)(u16)(over - overruns_));
	underruns_ = under;
	overruns_ = over;
}

/* see usb_line.h */
void UsbLine::wb_carrier(dcore::Modem &m)
{
	s16 on = m.link_state() == dcore::LinkState::Online ? 1 : 0;
	if (on == wb_carrier_ || !parked_.empty())
		return;
	DBG("LINK LED %s\n", on ? "on" : "off");
	if (stick_out(WP_SET_CARRIER, on ? WP_CARRIER_ON : WP_CARRIER_OFF))
		wb_carrier_ = on;
}

/* see usb_line.h: (requests that may fail: the stick may be gone) */
void UsbLine::wb_close()
{
	libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
				LIBUSB_RECIPIENT_INTERFACE |
				LIBUSB_ENDPOINT_OUT, WP_SET_CARRIER,
				WP_CARRIER_AUTO, 0, nullptr, 0, 1000);
	libusb_set_interface_alt_setting(h_, 0, 0);
}

} // namespace dmd
