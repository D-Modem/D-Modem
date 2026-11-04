/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_apple.cpp - the Apple USB Modem (USB 05AC:1401, and 05AC:8220)
 *                    as the modem's line: UsbLine's Kind::Apple
 *                    (usb_line.h)
 *
 *    The Apple USB Modem (MA034Z/A, A1101) is Motorola's SM56 USB soft
 *    modem.  How it works is what Apple's Mac OS X drivers for it do
 *    (docs/apple-usb-modem-details.md: AppleSM56KUSBModemFamily 1.0.0b20,
 *    AppleSM56KUSBAudio 1.9.9 and MotorolaSM56KUSB 1.5.9, of Mac OS X
 *    10.4.11, whose symbols name their functions); the code is ours.
 *
 *    - Its samples are USB audio (class 1): an audio streaming interface
 *      each way (interfaces 3, IN, and 4, OUT, on a real one), an
 *      isochronous endpoint in alternate setting 1, a format type I
 *      descriptor with its rates; the rate set on each endpoint with the
 *      audio class's SET_CUR (SAMPLING_FREQ_CONTROL), as
 *      AppleSM56KUSBAudioEngine::SetSampleRate does.  S16 little-endian,
 *      mono, is all that is taken here.
 *    - Its line is a CDC Direct Line Control interface (class 2, subclass
 *      3: interface 0): the hook is SET_HOOK_STATE (1 off hook, 0 on), as
 *      MotorolaSM56KUSB's si3056_HW_UpdateHookCtl has it, or, if the
 *      device stalls that (the first real one did), the Si3056's own
 *      (register 5's OH, as AppleSM56KUSBModemFamily::testInterface
 *      writes it); its interrupt
 *      endpoint's notifications (8 octets, CDC's header) are the ring
 *      detector's (RING_DETECT, 0x03: on a real one, one every 1 ms,
 *      wValue's bit 15 its polarity and its low octet a level, as
 *      AppleSM56KUSBModemFamily::intReadComplete splits it; a ring is
 *      both polarities, a steady line one), a command's response (0x02)
 *      and the phone cord (0x29: wValue 4 unplugged, 5 plugged in).
 *    - Its DAA is a Silicon Labs Si3056 (si3056.h) whose registers are
 *      3-octet commands, SEND_ENCAPSULATED_COMMAND to a vendor interface
 *      (class 0xff, subclass 2: interface 1): 0x00 a register written,
 *      0x01, 0x02, 0x03 its bits ORed, ANDed, XORed with the value, 0x80
 *      read; a read's value comes with GET_ENCAPSULATED_RESPONSE (2
 *      octets, the value first) once the response's notification has.
 *      The device's own controller runs the Si3056's serial link (its
 *      PLL, its rate): the line side powered up (register 6), its
 *      revision (11), its link (12), the on-hook line monitor (5, ONHM)
 *      and the country's settings (16, 26, 30, 31, 59) are the host's.
 *
 *    The streams run both ways from the start, as the pico-serial Line
 *    stick's: an OUT transfer goes when an IN one is done, with its
 *    packets' lengths (usb_line.cpp).  On hook the line monitor is on, so
 *    caller ID comes in the samples.
 *
 *    Tried on a real device up to the hook (its start-up's register
 *    reads; docs/apple-usb-modem-details.md's "A real one"), and on a
 *    simulated one (test/fake_apple.cpp).
 *
 */

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

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
#define TRACE(fmt, ...) dcore::log_printf(2, "usb: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

const u16 APPLE_VENDOR = 0x05ac;
/* the Apple USB Modem, and the other product Apple's driver takes
   (its personalities' 5121 and 33312) */
const u16 APPLE_MODEM = 0x1401, APPLE_MODEM_2 = 0x8220;

/* the interfaces' classes: CDC's Direct Line Control, the vendor
   interface of the DAA's commands, audio's control and streaming */
const u8 CLASS_CDC = 0x02, SUBCLASS_DLC = 0x03;
const u8 CLASS_VENDOR = 0xff, SUBCLASS_MGMT = 0x02;
const u8 CLASS_AUDIO = 0x01, SUBCLASS_AC = 0x01, SUBCLASS_AS = 0x02;

/* CDC's requests and notifications */
const u8 SEND_ENCAPSULATED_COMMAND = 0x00, GET_ENCAPSULATED_RESPONSE = 0x01;
const u8 SET_HOOK_STATE = 0x11;
const u8 N_RESPONSE = 0x02, N_RING_DETECT = 0x03, N_LINE = 0x29;

/* the DAA's commands (AppleSM56KUSBModemFamily's write_DAA_Register: apple_write(),
   OR_, AND_, XOR_DAA_Register, get_DAA_Register) */
const u8 DAA_WRITE = 0x00, DAA_OR = 0x01, DAA_AND = 0x02, DAA_READ = 0x80;

/* the audio class's: a format type descriptor, type I, PCM; SET_CUR
   of an endpoint's SAMPLING_FREQ_CONTROL */
const u8 CS_INTERFACE = 0x24, FORMAT_TYPE = 0x02;
const u8 FORMAT_TYPE_I = 0x01;
const u8 SET_CUR = 0x01;
const u16 SAMPLING_FREQ_CONTROL = 0x0100;

/* the Si3056's registers and bits (si3056.h's datasheet) */
const u8 SI_DAA1 = 5, SI_DAA2 = 6, SI_REV = 11, SI_STATUS = 12;
const u8 SI_DAA1_OH = 0x01, SI_DAA1_ONHM = 0x08;
const u8 SI_STATUS_FDT = 0x40;

/* a ring pulse's spacing while it rings (the core wants 4 a burst in
   0.15 s or more: modem.cpp's RING_PULSES, RING_MIN_MS), and a ring
   longer than any cadence's (Egypt's 2 s, with room) */
const u64 RING_PULSE_MS = 50, RING_LONGEST_MS = 4000;
/* ringing: both of the ring detector's polarities seen within this
   (a 20 Hz ring's half wave is 25 ms; a 16 Hz one's 31) */
const u64 RING_POLARITY_MS = 100;

const u16 TIMEOUT_MS = 1000;
/* a read's response: the notification waited for, ms, and how often */
const unsigned RESPONSE_MS = 100;
const int RESPONSE_TRIES = 3;

/* a format type I descriptor's rates (3 octets each): n of them, or
   (n 0) a range from the first to the second */
struct Rates {
	bool found = false;
	u8 channels = 0, subframe = 0, bits = 0;
	u8 n = 0;
	u32 r[32] = {};
};

/* the format type I descriptor in an alternate setting's class-specific
   descriptors (extra) */
Rates format_of(const u8 *extra, int len)
{
	Rates f;
	for (int i = 0; i + 2 <= len && extra[i] >= 2; i += extra[i]) {
		const u8 *d = extra + i;
		if (i + d[0] > len)
			break;
		if (d[1] != CS_INTERFACE || d[2] != FORMAT_TYPE || d[0] < 8 ||
		    d[3] != FORMAT_TYPE_I)
			continue;
		f.found = true;
		f.channels = d[4];
		f.subframe = d[5];
		f.bits = d[6];
		f.n = d[7];
		u8 count = f.n ? f.n : 2;
		for (u8 k = 0; k < count && k < 32 && 8 + 3 * k + 3 <= d[0]; k++)
			f.r[k] = d[8 + 3 * k] | u32{d[9 + 3 * k]} << 8 |
				u32{d[10 + 3 * k]} << 16;
		break;
	}
	return f;
}

/* whether f has rate */
bool has_rate(const Rates &f, u32 rate)
{
	if (!f.n)
		return rate >= f.r[0] && rate <= f.r[1];
	for (u8 k = 0; k < f.n && k < 32; k++)
		if (f.r[k] == rate)
			return true;
	return false;
}

/* f's rates, for the messages */
std::string rates_of(const Rates &f)
{
	char b[24];
	std::string s;
	if (!f.n) {
		snprintf(b, sizeof(b), "%u to %u", (unsigned)f.r[0], (unsigned)f.r[1]);
		return b;
	}
	for (u8 k = 0; k < f.n && k < 32; k++) {
		snprintf(b, sizeof(b), "%s%u", k ? ", " : "", (unsigned)f.r[k]);
		s += b;
	}
	return s;
}

} // namespace

/* see usb_line.h */
bool UsbLine::apple_device(u16 vendor, u16 product)
{
	return vendor == APPLE_VENDOR &&
		(product == APPLE_MODEM || product == APPLE_MODEM_2);
}

/* see usb_line.h: the configuration's interfaces by their classes (the
   numbers Apple's driver has if a class is not there), the interrupt
   endpoint (the Direct Line Control interface's, else the first
   interrupt IN), and each audio stream's alternate setting with an
   isochronous endpoint and a format of S16 mono at rate */
bool UsbLine::apple_find_streams(u32 rate)
{
	libusb_config_descriptor *cfg;
	int err = config(&cfg);
	if (err < 0) {
		ERR("%s: its configuration: %s\n", name_, libusb_strerror(err));
		return false;
	}
	u8 any_intr = 0;
	s16 any_intr_max = 0;
	s16 intr_max = 0;
	bool out_found = false, in_found = false;
	Rates out_f, in_f;
	ep_intr_ = 0;
	for (u8 i = 0; i < cfg->bNumInterfaces; i++) {
		const libusb_interface &itf = cfg->interface[i];
		for (int a = 0; a < itf.num_altsetting; a++) {
			const libusb_interface_descriptor &d = itf.altsetting[a];
			s16 n = d.bInterfaceNumber;
			TRACE("interface %d.%d: class %02x/%02x/%02x, %d endpoints\n",
			      n, d.bAlternateSetting, d.bInterfaceClass,
			      d.bInterfaceSubClass, d.bInterfaceProtocol,
			      d.bNumEndpoints);
			bool dlc = d.bInterfaceClass == CLASS_CDC &&
				d.bInterfaceSubClass == SUBCLASS_DLC;
			if (dlc)
				apple_dlc_ = n;
			if (d.bInterfaceClass == CLASS_VENDOR &&
			    d.bInterfaceSubClass == SUBCLASS_MGMT)
				apple_mgmt_ = n;
			if (d.bInterfaceClass == CLASS_AUDIO &&
			    d.bInterfaceSubClass == SUBCLASS_AC)
				apple_ac_ = n;
			for (u8 k = 0; k < d.bNumEndpoints; k++) {
				const libusb_endpoint_descriptor &e = d.endpoint[k];
				s16 type = e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
				bool in = e.bEndpointAddress & LIBUSB_ENDPOINT_IN;
				s16 max = e.wMaxPacketSize & 0x7ff;
				if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in) {
					if (dlc && !ep_intr_) {
						ep_intr_ = e.bEndpointAddress;
						intr_max = max;
					}
					if (!any_intr) {
						any_intr = e.bEndpointAddress;
						any_intr_max = max;
					}
				}
				if (type != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ||
				    d.bInterfaceClass != CLASS_AUDIO ||
				    d.bInterfaceSubClass != SUBCLASS_AS)
					continue;
				Rates f = format_of(d.extra, d.extra_length);
				TRACE("interface %d.%d: %s %02x (%d), %u channel(s), "
				      "%u-bit in %u octets, rates %s\n", n,
				      d.bAlternateSetting, in ? "IN" : "OUT",
				      e.bEndpointAddress, max, f.channels, f.bits,
				      f.subframe, f.found ? rates_of(f).c_str() : "none");
				bool ok = f.found && f.channels == 1 &&
					f.subframe == 2 && f.bits == 16 &&
					has_rate(f, rate);
				bool &found = in ? in_found : out_found;
				if (found || !ok) {
					if (!found)
						(in ? in_f : out_f) = f;
					continue;
				}
				found = true;
				(in ? in_f : out_f) = f;
				(in ? apple_in_if_ : apple_out_if_) = n;
				(in ? apple_in_alt_ : apple_out_alt_) = d.bAlternateSetting;
				(in ? iso_.ep_in : iso_.ep_out) = e.bEndpointAddress;
				(in ? iso_.max_in : iso_.max_out) = max;
			}
		}
	}
	libusb_free_config_descriptor(cfg);
	if (!ep_intr_) {
		ep_intr_ = any_intr;
		intr_max = any_intr_max;
	}
	/* (a whole packet asked for, as Apple's driver does: less, and a
	   notification longer than that is an overflow) */
	intr_len_ = static_cast<s16>(std::min<s16>(
		intr_max > 0 ? intr_max : 64, static_cast<s16>(sizeof(intr_buf_))));
	for (bool in : { false, true }) {
		if (in ? in_found : out_found)
			continue;
		const Rates &f = in ? in_f : out_f;
		if (!f.found)
			ERR("%s: no audio stream %s (an isochronous endpoint with a "
			    "format type I descriptor)\n", name_, in ? "IN" : "OUT");
		else if (f.channels != 1 || f.subframe != 2 || f.bits != 16)
			ERR("%s: its audio stream %s is %u channel(s) of %u-bit "
			    "samples in %u octets: only 16-bit mono is taken\n",
			    name_, in ? "IN" : "OUT", f.channels, f.bits, f.subframe);
		else
			ERR("%s: no rate of %" PRIu32 " Hz (-r) on its audio stream "
			    "%s: %s\n", name_, rate, in ? "IN" : "OUT",
			    rates_of(f).c_str());
		return false;
	}
	if (!ep_intr_) {
		ERR("%s: no interrupt endpoint (its line's notifications)\n", name_);
		return false;
	}
	DBG("interfaces: line %d (interrupt %02x, %d), DAA %d, audio control %d, "
	    "OUT %d.%d (%02x, %d), IN %d.%d (%02x, %d)\n", apple_dlc_, ep_intr_, intr_len_,
	    apple_mgmt_, apple_ac_, apple_out_if_, apple_out_alt_, iso_.ep_out,
	    iso_.max_out, apple_in_if_, apple_in_alt_, iso_.ep_in, iso_.max_in);
	return true;
}

/* see usb_line.h: its interfaces claimed (interface 0, open()'s, aside),
   the streams' alternate settings and the rate on both endpoints, the
   DAA's line side up, on hook with the line monitor on, the country's
   settings */
bool UsbLine::open_apple(u32 rate)
{
	int err;

	if (voice_law_ != VoiceLaw::None)
		ERR("Apple USB Modem: no voice codec (--usb-voice-codec)\n");
	if (!apple_find_streams(rate))
		return false;
	apple_claimed_.clear();
	for (s16 n : { apple_dlc_, apple_mgmt_, apple_ac_, apple_out_if_, apple_in_if_ }) {
		if (n <= 0 || std::find(apple_claimed_.begin(), apple_claimed_.end(), n) !=
		    apple_claimed_.end())
			continue;
		/* (Linux's snd-usb-audio has the audio ones: detached, open()'s
		   libusb_set_auto_detach_kernel_driver()) */
		if ((err = libusb_claim_interface(h_, n)) < 0) {
			ERR("Apple USB Modem: its interface %d: %s\n", n,
			    libusb_strerror(err));
			return false;
		}
		apple_claimed_.push_back(n);
	}
	for (bool in : { false, true }) {
		s16 itf = in ? apple_in_if_ : apple_out_if_;
		s16 alt = in ? apple_in_alt_ : apple_out_alt_;
		u8 ep = in ? iso_.ep_in : iso_.ep_out;
		if ((err = libusb_set_interface_alt_setting(h_, itf, alt)) < 0) {
			ERR("Apple USB Modem: interface %d's alternate setting %d: %s\n",
			    itf, alt, libusb_strerror(err));
			return false;
		}
		u8 f[3] = { static_cast<u8>(rate), static_cast<u8>(rate >> 8),
			    static_cast<u8>(rate >> 16) };
		err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_CLASS |
					      LIBUSB_RECIPIENT_ENDPOINT |
					      LIBUSB_ENDPOINT_OUT, SET_CUR,
					      SAMPLING_FREQ_CONTROL, ep, f, 3,
					      TIMEOUT_MS);
		if (err < 0) {
			ERR("Apple USB Modem: %" PRIu32 " Hz on endpoint %02x: %s\n",
			    rate, ep, libusb_strerror(err));
			return false;
		}
		libusb_clear_halt(h_, ep);
	}
	/* the DAA: its line side (a Si3019's AC termination is register
	   30's), powered up, its link */
	u8 rev = 0, st = 0;
	if (!apple_read(SI_REV, &rev))
		return false;
	si3019_ = (rev >> 4) == 3;
	DBG("Apple USB Modem: Si3056 rev %u, line side %s\n", rev & 0xf,
	    (rev >> 4) == 3 ? "Si3019" : (rev >> 4) == 1 ? "Si3018" : "unknown");
	if (!apple_write(SI_DAA2, 0x00))
		return false;
	for (int i = 0; i < 10 && apple_read(SI_STATUS, &st) && !(st & SI_STATUS_FDT); i++)
		;
	if (!(st & SI_STATUS_FDT))
		ERR("Apple USB Modem: its line side does not answer (the "
		    "isolation link is down); going on\n");
	apple_hook_reg_ = false;
	if (!apple_hook(false) || !apple_monitor(true))
		return false;
	/* the country's registers, as they are now */
	struct {
		u8 reg;
		u8 *val;
	} regs[] = { { 16, &apple_regs_.r16 }, { 26, &apple_regs_.r26 },
		     { 30, &apple_regs_.r30 }, { 31, &apple_regs_.r31 },
		     { 59, &apple_regs_.r59 } };
	for (auto &x : regs) {
		*x.val = 0;
		if (x.reg == 30 && !si3019_)
			continue;
		if (!apple_read(x.reg, x.val))
			return false;
	}
	apple_regs_read_ = true;
	if (!apple_country())
		return false;
	apple_ringing_ = false;
	apple_ring_since_ = apple_ring_pulse_ = 0;
	apple_ring_neg_ = apple_ring_pos_ = 0;
	apple_ring_reports_ = 0;
	/* (its controller's own buffering: a guess, 2 ms each way) */
	fifo_samples_ = static_cast<s16>(rate * 4 / 1000);
	return true;
}

/* see usb_line.h: SEND_ENCAPSULATED_COMMAND to the DAA's interface;
   false (logged) */
bool UsbLine::apple_cmd(u8 cmd, u8 reg, u8 value)
{
	u8 b[3] = { cmd, reg, value };
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_CLASS |
					  LIBUSB_RECIPIENT_INTERFACE |
					  LIBUSB_ENDPOINT_OUT,
					  SEND_ENCAPSULATED_COMMAND, 0,
					  static_cast<u16>(apple_mgmt_), b, 3,
					  TIMEOUT_MS);
	TRACE("Apple: DAA command %02x %u %02x: %d\n", cmd, reg, value, err);
	if (err < 0)
		ERR("Apple USB Modem: DAA command %02x (register %u, %02x): %s\n",
		    cmd, reg, value, libusb_strerror(err));
	return err >= 0;
}

/* see usb_line.h: command 0x00 */
bool UsbLine::apple_write(u8 reg, u8 value)
{
	return apple_cmd(DAA_WRITE, reg, value);
}

/* see usb_line.h: the read sent; its response's notification waited for
   (others taken as they come), then the response (if the notification
   did not come in time, asked for all the same); false (logged) */
bool UsbLine::apple_read(u8 reg, u8 *value)
{
	if (!apple_cmd(DAA_READ, reg, 0))
		return false;
	for (int i = 0; i < RESPONSE_TRIES; i++) {
		u8 n[sizeof(intr_buf_)];
		int got = 0;
		int err = libusb_interrupt_transfer(h_, ep_intr_, n, intr_len_,
						    &got, RESPONSE_MS);
		if (err == LIBUSB_ERROR_TIMEOUT)
			break;
		if (err == LIBUSB_ERROR_NO_DEVICE) {
			ERR("Apple USB Modem: its notifications: %s\n",
			    libusb_strerror(err));
			return false;
		}
		if (err < 0) {
			/* (the response asked for all the same: one left
			   unread holds up the device's other requests) */
			DBG("Apple USB Modem: its notifications: %s (%d of %d "
			    "octets); the response asked for anyway\n",
			    libusb_strerror(err), got, intr_len_);
			break;
		}
		if (got >= 4 && n[1] == N_RESPONSE && (n[2] & 1))
			break;
		apple_event(n, static_cast<s16>(got));
	}
	u8 r[2] = { 0, 0 };
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_CLASS |
					  LIBUSB_RECIPIENT_INTERFACE |
					  LIBUSB_ENDPOINT_IN,
					  GET_ENCAPSULATED_RESPONSE, 0,
					  static_cast<u16>(apple_mgmt_), r, 2,
					  TIMEOUT_MS);
	if (err < 1) {
		ERR("Apple USB Modem: DAA register %u: %s\n", reg,
		    err < 0 ? libusb_strerror(err) : "no response");
		return false;
	}
	*value = r[0];
	TRACE("Apple: DAA register %u = %02x\n", reg, r[0]);
	return true;
}

/* see usb_line.h: SET_HOOK_STATE to the line's interface; if the device
   stalls it, register 5's OH from then on (AppleSM56KUSBModemFamily's
   testInterface hooks with both); false (logged) */
bool UsbLine::apple_hook(bool off)
{
	if (!apple_hook_reg_) {
		int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_CLASS |
						  LIBUSB_RECIPIENT_INTERFACE |
						  LIBUSB_ENDPOINT_OUT, SET_HOOK_STATE,
						  off ? 1 : 0,
						  static_cast<u16>(apple_dlc_), nullptr,
						  0, TIMEOUT_MS);
		if (err >= 0)
			return true;
		if (err != LIBUSB_ERROR_PIPE) {
			ERR("Apple USB Modem: %s: %s\n", off ? "off hook" : "on hook",
			    libusb_strerror(err));
			return false;
		}
		DBG("Apple USB Modem: SET_HOOK_STATE stalled: the hook is the "
		    "DAA's own (register 5's OH) from now on\n");
		apple_hook_reg_ = true;
	}
	return off ? apple_cmd(DAA_OR, SI_DAA1, SI_DAA1_OH) :
		apple_cmd(DAA_AND, SI_DAA1, static_cast<u8>(~SI_DAA1_OH));
}

/* see usb_line.h: register 5's ONHM set or cleared (its other bits the
   device's: ORed, ANDed) */
bool UsbLine::apple_monitor(bool on)
{
	return on ? apple_cmd(DAA_OR, SI_DAA1, SI_DAA1_ONHM) :
		apple_cmd(DAA_AND, SI_DAA1, static_cast<u8>(~SI_DAA1_ONHM));
}

/* see usb_line.h: the ring detector's report (a ring's start when both
   polarities have come within RING_POLARITY_MS: a ring pulse, and a
   pulse every RING_PULSE_MS after, apple_ring_tick(), which ends it),
   the phone cord's state said; the rest traced */
void UsbLine::apple_event(const u8 *b, s16 len)
{
	if (len < 4)
		return;
	u16 value = static_cast<u16>(b[2] | u16{b[3]} << 8);
	switch (b[1]) {
	case N_RING_DETECT: {
		/* (1000 a second: the first and every 1000th traced) */
		if (apple_ring_reports_++ % 1000 == 0)
			TRACE("Apple: ring detector %04x (report %" PRIu64 ")\n",
			      value, apple_ring_reports_);
		u64 now = now_ms();
		/* (0 is never: a polarity's time is 1 or more) */
		(value & 0x8000 ? apple_ring_neg_ : apple_ring_pos_) = now ? now : 1;
		if (!apple_ringing_ && apple_ring_neg_ && apple_ring_pos_ &&
		    now - apple_ring_neg_ <= RING_POLARITY_MS &&
		    now - apple_ring_pos_ <= RING_POLARITY_MS) {
			TRACE("Apple: the line rings\n");
			apple_ringing_ = true;
			apple_ring_since_ = apple_ring_pulse_ = now;
			rings_++;
		}
		break;
	}
	case N_LINE:
		TRACE("Apple: notification %02x, %04x\n", b[1], value);
		if (value == 4)
			ERR("Apple USB Modem: its phone cord is unplugged\n");
		else if (value == 5)
			DBG("Apple USB Modem: its phone cord is plugged in\n");
		break;
	default:
		TRACE("Apple: notification %02x, %04x\n", b[1], value);
		break;
	}
}

/* see usb_line.h: the ring ended when a polarity has not come for
   RING_POLARITY_MS (a ring pulse), or dropped when it has gone on
   longer than any ring */
void UsbLine::apple_ring_tick()
{
	if (!apple_ringing_)
		return;
	u64 now = now_ms();
	if (now - apple_ring_neg_ > RING_POLARITY_MS ||
	    now - apple_ring_pos_ > RING_POLARITY_MS) {
		TRACE("Apple: the line stopped ringing\n");
		apple_ringing_ = false;
		rings_++;
		return;
	}
	if (now - apple_ring_since_ > RING_LONGEST_MS) {
		DBG("Apple: rang %" PRIu64 " ms with no end: not a ring\n",
		    now - apple_ring_since_);
		apple_ringing_ = false;
		/* (not again until both polarities come anew) */
		apple_ring_neg_ = apple_ring_pos_ = 0;
		return;
	}
	while (now - apple_ring_pulse_ >= RING_PULSE_MS) {
		apple_ring_pulse_ += RING_PULSE_MS;
		rings_++;
	}
}

/* see usb_line.h: si3056_apply() on the registers as read at the start,
   written (30 only on a Si3019) */
bool UsbLine::apple_country()
{
	if (!apple_regs_read_)
		return true;
	Si3056Regs r = apple_regs_;
	si3056_apply(*daa_, si3019_, r);
	if (!apple_write(16, r.r16) || !apple_write(26, r.r26) ||
	    (si3019_ && !apple_write(30, r.r30)) || !apple_write(31, r.r31) ||
	    !apple_write(59, r.r59))
		return false;
	daa_due_ = false;
	char r30[4] = "-";
	if (si3019_)
		snprintf(r30, sizeof(r30), "%02x", r.r30);
	DBG("%s's line: registers 16 %02x, 26 %02x, 30 %s, 31 %02x, 59 %02x\n",
	    daa_name_.c_str(), r.r16, r.r26, r30, r.r31, r.r59);
	return true;
}

/* see usb_line.h: on hook, the line monitor off, the streams' alternate
   settings 0, its interfaces (but 0, close()'s) released */
void UsbLine::apple_close()
{
	if (claimed_ && !dead_) {
		apple_hook(false);
		apple_monitor(false);
	}
	if (claimed_)
		for (s16 itf : { apple_out_if_, apple_in_if_ })
			libusb_set_interface_alt_setting(h_, itf, 0);
	for (s16 n : apple_claimed_)
		libusb_release_interface(h_, n);
	apple_claimed_.clear();
	apple_regs_read_ = false;
}

} // namespace dmd
