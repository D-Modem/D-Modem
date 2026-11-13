/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_cadmus2.cpp - Conexant's USB HSF modems (the "Cadmus 2" bridge:
 *                      USB 0572:1300 and the others hsfusbcd2 takes) as
 *                      the modem's line: UsbLine's Cadmus 2 chip
 *                      (usb_line.h)
 *
 *    How it works is what Conexant's Linux driver (hsfmodem 7.80.02.06,
 *    its hsfusbcd2 module) does with it, as half_hsf_usb (dsp/test)
 *    showed it, running the module against a simulated bridge
 *    (dsp/test/hw/cadmus2.hpp: docs/interop-details.md); the code is
 *    ours.
 *
 *    - Its firmware is the host's: with none (GET_INFORMATION's third
 *      octet 1), Conexant's (c2firmware.h's ROM_IMAGE, not ours to ship:
 *      DMODEM_CD2_FIRMWARE names the file, the header itself or the
 *      bare octets) goes up in 64-octet blocks (UPLOAD_FIRMWARE) with
 *      the device unconfigured, then configuration 1 again once it
 *      says it runs (3).
 *    - The bridge runs scripts the host sends (CONTROL_SCRIPT, vendor
 *      request 2: wValue 0xff01, or 0xff02 for the one that stays,
 *      wIndex the slot); what one reports comes on the interrupt
 *      endpoint (c1 02 <slot> ff <slot> 00 <n> 00, then n octets: a
 *      reply code first).  The scripts are those the module sends a
 *      plain board (no SmartDAA), as it sent them: the start (1), a
 *      word to the codec (2), the monitor in slot 3 (8), the streams'
 *      start (9, 5).
 *    - The codec's words, as the module writes them: 2004, a208, f200,
 *      aae8, 6040, 35b4 at the start; aac8 off hook and aae8 on (bit
 *      0x0020, it seems, the hook); 35b7 and 35b4 the speaker on and
 *      off (not used here).
 *    - The samples: bulk transfers (0x81 IN, 0x01 OUT), S16
 *      little-endian, mono, at 16000 Hz, 128 octets (64 samples) each;
 *      an OUT transfer for each IN one, of its length.  The modem runs
 *      at 9600 Hz: the samples converted both ways (dsp::Resampler, 3/5
 *      and 5/3 through 48 kHz).  The module
 *      starts them off hook and stops them on hook; here they run from
 *      the start, as the other devices' do, the hook the codec's word.
 *    - The ring: the monitor's notification 08 01 (its channel 4,
 *      which only a plain board's monitor asks for: a guess that it is
 *      the ring detector), two or more a ring; a lone one is not.
 *
 *    A SmartDAA board (the start's reply 01 00 01 01) has its line side
 *    behind other scripts, which the simulated bridge does not run:
 *    not taken.
 *
 *    Tried only on a simulated device (test/fake_cadmus2.cpp), none
 *    real.
 */

#include <algorithm>
#include <cmath>
#include <deque>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dsp.h>
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

/* the USB IDs hsfusbcd2 takes (mod_usbcd2.c) */
const struct { u16 vendor, product; } CD2_IDS[] = {
	{ 0x0572, 0x1300 }, { 0x0572, 0x1301 }, { 0x0572, 0x1302 },
	{ 0x0572, 0x1303 }, { 0x08e3, 0x0111 }, { 0x0803, 0x1300 },
	{ 0x0803, 0x1301 }, { 0x145f, 0x0106 }, { 0x148d, 0x1671 },
	{ 0x148d, 0x1672 },
};

/* its vendor requests */
const u8 GET_INFORMATION = 0, UPLOAD_FIRMWARE = 1, CONTROL_SCRIPT = 2;
const u16 SCRIPT = 0xff01, SCRIPT_STAYS = 0xff02;
const u16 FW_BLOCK = 64;
/* GET_INFORMATION's third octet: no firmware, HCF's, HSF's */
const u8 FW_NONE = 1, FW_HCF = 2, FW_HSF = 3;

/* the interfaces and endpoints (its descriptors' when they say) */
const s16 COMM_IF = 1;
const u8 EP_IN = 0x81, EP_OUT = 0x01, EP_INTR = 0x82;

/* the samples: 64 a transfer each way, at 16000 Hz (the modem's at
   MODEM_RATE); IN transfers in flight (the module's: its 4096-octet
   ring), OUT ones */
const u32 RATE = 16000, MODEM_RATE = 9600;
const s16 FRAME_OCTETS = 128;
const s16 IN_TRANSFERS = 32, OUT_TRANSFERS = 32;

const u16 TIMEOUT_MS = 1000;

/* the scripts, as the module sends them a plain board */
const u8 S_START[] = {
	0x13, 0x01, 0x80, 0x3d, 0x1e, 0x0a, 0x0b, 0x1e, 0x00, 0x27, 0x01, 0x27,
	0x00, 0x07, 0x20, 0x02, 0x1c, 0x17, 0x00, 0x27, 0x01, 0x1b, 0x35, 0x0b,
	0x21, 0x08, 0x2f, 0x00, 0x2f, 0x15, 0x31, 0x00, 0x31, 0x15, 0x13, 0x01,
	0x08, 0x3d, 0x1e, 0x02, 0x17, 0x01, 0x08, 0x2f, 0x02, 0x31, 0x02, 0x2f,
	0x01, 0x31, 0x01, 0x27, 0x00, 0x27, 0x01, 0x28, 0x36,
};
/* (the word's two octets at 15 and 16) */
const u8 S_WORD[] = {
	0x07, 0x1c, 0x80, 0x1c, 0x0c, 0x80, 0x0b, 0x1c, 0xa0, 0x3d, 0x1e, 0x07,
	0x19, 0x17, 0x02, 0x00, 0x00, 0x27, 0x02, 0x27, 0x01, 0x28, 0x36, 0x2a,
	0x02, 0x0f, 0x1a, 0x2a, 0x01, 0x0f, 0x1b, 0x07, 0x04, 0x20, 0x1c, 0x29,
	0x20, 0x22, 0x1f, 0x14, 0x00, 0x0b, 0x04, 0x20, 0x29, 0x00, 0x0f, 0x1a,
	0x0f, 0x1b, 0x36,
};
const size_t S_WORD_AT = 15;
const u8 S_MONITOR[] = {
	0x07, 0x20, 0x02, 0x1d, 0x1c, 0x00, 0x30, 0x04, 0x33, 0x04, 0x1c, 0x08,
	0x00, 0x27, 0x08, 0x26, 0x28, 0x33, 0x04, 0x1d, 0x11, 0x00, 0x27, 0x08,
	0x26, 0x28, 0x1b, 0x08, 0x57, 0x10, 0x06, 0x1c, 0x35, 0x00, 0x27, 0x08,
	0x27, 0x00, 0x28, 0x3d, 0x1e, 0x02, 0x57, 0x1a, 0x77, 0x1d, 0x27, 0x77,
	0x27, 0x08, 0x27, 0x80, 0x28, 0x5b, 0x10, 0x00, 0x3d, 0x1e, 0x02, 0x1b,
	0x1c,
};
const u8 S_STREAMS_9[] = {
	0x34, 0x45, 0x46, 0x47, 0x27, 0x09, 0x27, 0x01, 0x28, 0x35, 0x36,
};
const u8 S_STREAMS_5[] = {
	0x34, 0x0b, 0x21, 0x08, 0x44, 0x46, 0x0b, 0x1c, 0xa0, 0x0b, 0x1d, 0xa0,
	0x0b, 0x1e, 0x44, 0x27, 0x05, 0x27, 0x01, 0x28, 0x35, 0x36,
};
const u8 S_STOP_6[] = {
	0x45, 0x47, 0x17, 0x1e, 0x44, 0x0b, 0x1c, 0x20, 0x0b, 0x1d, 0x20, 0x27,
	0x06, 0x27, 0x01, 0x28, 0x36,
};
const u8 S_STOP_4[] = { 0x31, 0x00, 0x27, 0x04, 0x27, 0x01, 0x28, 0x36 };

/* the slots: the scripts', the monitor's */
const u16 SLOT = 1, SLOT_MONITOR = 3;

/* the codec's words: at the start; off hook and on */
const u16 CODEC_START[] = { 0x2004, 0xa208, 0xf200, 0xaae8, 0x6040, 0x35b4 };
const u16 WORD_OFF_HOOK = 0xaac8, WORD_ON_HOOK = 0xaae8;

/* the replies: the start's, a word's, the monitor's */
const u8 R_START = 0x01, R_WORD = 0x02, R_MONITOR = 0x08;

/* a ring: two of the monitor's reports within RING_GAP_MS (a lone one,
   as at the start, is not), then a pulse every RING_PULSE_MS (the core
   wants 4 in 0.15 s: modem.cpp's RING_PULSES, RING_MIN_MS) until none
   has come for RING_GAP_MS; or dropped when it has gone on longer than
   any ring (Egypt's 2 s, with room) */
const u64 RING_PULSE_MS = 50, RING_GAP_MS = 250, RING_LONGEST_MS = 4000;

/* the firmware in path: c2firmware.h (the octets of its ROM_IMAGE[]'s
   initializer) or the bare octets; empty if neither */
std::vector<u8> read_firmware(const char *path)
{
	std::vector<u8> f;
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return f;
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		s.append(buf, n);
	fclose(fp);
	size_t at = s.find("ROM_IMAGE");
	if (at == std::string::npos) {
		f.assign(s.begin(), s.end());
		return f;
	}
	size_t open = s.find('{', at), close = s.find('}', open);
	if (open == std::string::npos || close == std::string::npos)
		return f;
	const char *p = s.c_str() + open + 1, *end = s.c_str() + close;
	while (p < end) {
		char *q;
		unsigned long v = strtoul(p, &q, 0);
		if (q == p) {
			p++;
			continue;
		}
		f.push_back(static_cast<u8>(v));
		p = q;
	}
	return f;
}

} // namespace

/* see usb_line.h */
bool UsbLine::cadmus2_device(u16 vendor, u16 product)
{
	for (const auto &id : CD2_IDS)
		if (id.vendor == vendor && id.product == product)
			return true;
	return false;
}

/* the Cadmus 2: its firmware, its scripts, its samples on bulk
   endpoints */
struct UsbLine::Cadmus2 final : UsbLine::Chip {
	using Chip::Chip;

	/* interface 1 claimed */
	bool comm_claimed = false;
	/* the samples' conversion: 16000 Hz to the modem's 9600, and back;
	   the device's samples converted, not yet sent */
	dsp::Resampler down, up;
	std::deque<s16> up_q;

	/* the ring: ringing, since, its last report, its last pulse */
	bool ringing = false;
	u64 ring_since = 0, ring_last = 0, ring_pulse = 0;

	/* (Chip) in the messages */
	const char *name() const override { return "Conexant USB HSF modem"; }
	/* (Chip) for ATI3 */
	const char *model() const override { return "Conexant USB HSF (Cadmus 2)"; }
	/* (Chip) in "not the endpoints of ..." */
	const char *a_name() const override { return "a Conexant USB HSF modem"; }
	/* its samples are at 16000 Hz: the modem's at 9600 */
	u32 rate(u32 asked) override
	{
		if (asked != MODEM_RATE)
			DBG("Conexant USB HSF modem: %" PRIu32 " Hz, its rate (not %"
			    PRIu32 ")\n", MODEM_RATE, asked);
		return MODEM_RATE;
	}

	/* GET_INFORMATION (5 octets, 4 at least: the third the firmware
	   running, the fourth the bridge's kind); false if it fails
	   (logged when said) */
	bool info(u8 *b, u16 attempt, bool say)
	{
		int err = libusb_control_transfer(l.h_, LIBUSB_REQUEST_TYPE_VENDOR |
						  LIBUSB_RECIPIENT_DEVICE |
						  LIBUSB_ENDPOINT_IN, GET_INFORMATION, 0,
						  attempt, b, 5, TIMEOUT_MS);
		if (err < 4) {
			if (say)
				ERR("%s: its information: %s\n", name(),
				    err < 0 ? libusb_strerror(err) : "too short");
			return false;
		}
		TRACE("Cadmus 2: information %02x %02x %02x %02x\n", b[0], b[1],
		      b[2], b[3]);
		return true;
	}

	/* (Chip) before its interfaces are claimed: its firmware, if it
	   has none, as OsUsbFWDownload does: unconfigured, the blocks,
	   until it says it runs it (10 tries, 100 ms apart), configuration
	   1 again */
	bool load() override
	{
		u8 b[5] = {};
		bool got = false;
		for (u16 i = 0; i < 3 && !got; i++)
			got = info(b, i, i == 2);
		if (!got)
			return false;
		if (b[2] == FW_HSF) {
			DBG("%s: its firmware runs\n", name());
			return true;
		}
		if (b[2] == FW_HCF) {
			ERR("%s: it runs HCF firmware (a controller modem's), not "
			    "HSF's: unplug it, and plug it in again\n", name());
			return false;
		}
		if (b[2] != FW_NONE) {
			ERR("%s: its firmware is of a kind not known (%02x)\n",
			    name(), b[2]);
			return false;
		}
		if (b[3] != 3)
			DBG("%s: a bridge of kind %u (the module's firmware is "
			    "for 3, X4)\n", name(), b[3]);
		const char *path = getenv("DMODEM_CD2_FIRMWARE");
		if (!path || !*path) {
			ERR("%s: it has no firmware: DMODEM_CD2_FIRMWARE names "
			    "Conexant's (c2firmware.h, from hsfmodem's "
			    "modules/imported/include)\n", name());
			return false;
		}
		std::vector<u8> fw = read_firmware(path);
		if (fw.empty() || fw.size() > 0xffff) {
			ERR("%s: %s: no firmware in it (c2firmware.h, or its "
			    "octets)\n", name(), path);
			return false;
		}
		DBG("%s: its firmware, %zu octets, from %s\n", name(), fw.size(),
		    path);
		int err = libusb_set_configuration(l.h_, -1);
		if (err < 0)
			DBG("%s: unconfigured: %s; going on\n", name(),
			    libusb_strerror(err));
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		for (size_t at = 0; at < fw.size(); at += FW_BLOCK) {
			u16 n = static_cast<u16>(std::min<size_t>(FW_BLOCK, fw.size() - at));
			err = libusb_control_transfer(l.h_, LIBUSB_REQUEST_TYPE_VENDOR |
						      LIBUSB_RECIPIENT_DEVICE |
						      LIBUSB_ENDPOINT_OUT, UPLOAD_FIRMWARE,
						      static_cast<u16>(fw.size()),
						      static_cast<u16>(at), &fw[at], n,
						      TIMEOUT_MS);
			if (err != n) {
				ERR("%s: its firmware at %zu: %s\n", name(), at,
				    err < 0 ? libusb_strerror(err) : "short");
				return false;
			}
		}
		got = false;
		for (u16 i = 0; i < 10 && !got; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			got = info(b, i, false);
		}
		if (!got || b[2] != FW_HSF) {
			ERR("%s: its firmware does not run (%s %02x)\n", name(),
			    got ? "it says" : "no answer", got ? b[2] : 0);
			return false;
		}
		if ((err = libusb_set_configuration(l.h_, 1)) < 0) {
			ERR("%s: configuration 1: %s\n", name(), libusb_strerror(err));
			return false;
		}
		DBG("%s: its firmware runs\n", name());
		return true;
	}

	/* its endpoints from its descriptors (the module's if they do not
	   say): bulk IN and OUT, interrupt IN */
	void endpoints()
	{
		l.iso_.ep_in = EP_IN;
		l.iso_.ep_out = EP_OUT;
		l.iso_.max_in = l.iso_.max_out = 64;
		l.ep_intr_ = EP_INTR;
		s16 intr_max = 64;
		libusb_config_descriptor *cfg;
		if (l.config(&cfg) == 0) {
			for (u8 i = 0; i < cfg->bNumInterfaces; i++) {
				if (!cfg->interface[i].num_altsetting)
					continue;
				const libusb_interface_descriptor &d =
					cfg->interface[i].altsetting[0];
				for (u8 k = 0; k < d.bNumEndpoints; k++) {
					const libusb_endpoint_descriptor &e = d.endpoint[k];
					s16 type = e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
					bool in = e.bEndpointAddress & LIBUSB_ENDPOINT_IN;
					s16 max = e.wMaxPacketSize & 0x7ff;
					if (e.bEndpointAddress == EP_IN || e.bEndpointAddress == EP_OUT)
						(in ? l.iso_.max_in : l.iso_.max_out) = max;
					if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in &&
					    e.bEndpointAddress == EP_INTR)
						intr_max = max;
				}
			}
			libusb_free_config_descriptor(cfg);
		}
		l.intr_len_ = static_cast<s16>(std::min<s16>(
			intr_max > 0 ? intr_max : 64,
			static_cast<s16>(sizeof(l.intr_buf_))));
	}

	/* a script into slot (wValue val); false (logged) */
	bool script(const u8 *code, size_t len, u16 slot, u16 val = SCRIPT)
	{
		/* (an interface's request, as the module's; Windows' WinUSB
		   would put its interface's number in wIndex: the device's
		   there) */
#ifdef _WIN32
		u8 recip = LIBUSB_RECIPIENT_DEVICE;
#else
		u8 recip = LIBUSB_RECIPIENT_INTERFACE;
#endif
		int err = libusb_control_transfer(l.h_, LIBUSB_REQUEST_TYPE_VENDOR |
						  recip | LIBUSB_ENDPOINT_OUT,
						  CONTROL_SCRIPT, val, slot,
						  const_cast<u8 *>(code),
						  static_cast<u16>(len), TIMEOUT_MS);
		TRACE("Cadmus 2: script %02x... (%zu octets) in slot %u: %d\n",
		      code[0], len, slot, err);
		if (err < 0)
			ERR("%s: a script: %s\n", name(), libusb_strerror(err));
		return err >= 0;
	}

	/* a word to the codec (script 2) */
	bool word(u16 w)
	{
		u8 s[sizeof(S_WORD)];
		memcpy(s, S_WORD, sizeof(s));
		s[S_WORD_AT] = static_cast<u8>(w >> 8);
		s[S_WORD_AT + 1] = static_cast<u8>(w);
		TRACE("Cadmus 2: codec word %04x\n", w);
		return script(s, sizeof(s), SLOT);
	}

	/* a notification's report, its n octets (b): code, and the rest */
	void report(u8 slot, const u8 *b, u8 n)
	{
		if (slot == SLOT_MONITOR && n >= 2 && b[0] == R_MONITOR) {
			TRACE("Cadmus 2: monitor %02x\n", b[1]);
			if (b[1] == 0x01)
				ring_report();
			return;
		}
		TRACE("Cadmus 2: slot %u reports %02x%s\n", slot, n ? b[0] : 0,
		      n >= 2 && b[n - 1] == 1 ? "" : " (not done)");
	}

	/* the notifications in b: each one's report; the reply with code
	   (0: none) in slot looked for: whether it came, and its octets
	   (got) */
	bool notes(const u8 *b, s16 len, u8 code = 0, u8 slot = 0,
		   std::vector<u8> *got = nullptr)
	{
		bool found = false;
		s16 at = 0;
		while (at + 8 <= len) {
			const u8 *h = b + at;
			if (h[0] != 0xc1 || h[1] != 0x02) {
				TRACE("Cadmus 2: notification %02x %02x (%d octets)\n",
				      h[0], h[1], len - at);
				break;
			}
			u8 n = static_cast<u8>(std::min<s16>(h[6], static_cast<s16>(len - at - 8)));
			if (code && !found && h[2] == slot && n && h[8] == code) {
				found = true;
				if (got)
					got->assign(h + 8, h + 8 + n);
			} else {
				report(h[2], h + 8, n);
			}
			at = static_cast<s16>(at + 8 + n);
		}
		return found;
	}

	/* the reply code from slot, waited for on the interrupt endpoint
	   (before its transfer is in flight); false (logged) */
	bool reply(u8 code, u8 slot, std::vector<u8> *got = nullptr)
	{
		for (int i = 0; i < 20; i++) {
			u8 b[sizeof(l.intr_buf_)];
			int n = 0;
			int err = libusb_interrupt_transfer(l.h_, l.ep_intr_, b, l.intr_len_,
							    &n, 100);
			if (err == LIBUSB_ERROR_TIMEOUT)
				continue;
			if (err < 0) {
				ERR("%s: its notifications: %s\n", name(),
				    libusb_strerror(err));
				return false;
			}
			if (notes(b, static_cast<s16>(n), code, slot, got))
				return true;
		}
		ERR("%s: no reply to script %u\n", name(), code);
		return false;
	}

	/* a script into slot 1, its reply waited for */
	bool run(const u8 *code, size_t len, u8 rcode)
	{
		return script(code, len, SLOT) && reply(rcode, SLOT);
	}

	/* (Chip) its start-up: interface 1, the start (the board's kind),
	   the codec's words, the monitor, the streams */
	bool open(u32) override
	{
		int err;
		if (l.voice_law_ != VoiceLaw::None)
			ERR("%s: no voice codec (--usb-voice-codec)\n", name());
		if ((err = libusb_claim_interface(l.h_, COMM_IF)) < 0) {
			ERR("%s: its interface %d: %s\n", name(), COMM_IF,
			    libusb_strerror(err));
			return false;
		}
		comm_claimed = true;
		endpoints();
		std::vector<u8> r;
		if (!script(S_START, sizeof(S_START), SLOT) || !reply(R_START, SLOT, &r))
			return false;
		if (r.size() >= 3 && r[2]) {
			ERR("%s: a SmartDAA board, whose line side is not yet "
			    "known here\n", name());
			return false;
		}
		DBG("%s: a plain board (no SmartDAA)\n", name());
		for (u16 w : CODEC_START)
			if (!word(w) || !reply(R_WORD, SLOT))
				return false;
		if (!script(S_MONITOR, sizeof(S_MONITOR), SLOT_MONITOR, SCRIPT_STAYS))
			return false;
		if (!run(S_STREAMS_9, sizeof(S_STREAMS_9), 0x09) ||
		    !run(S_STREAMS_5, sizeof(S_STREAMS_5), 0x05))
			return false;
		ringing = false;
		ring_since = ring_last = ring_pulse = 0;
		down.init(3, 5);
		up.init(5, 3);
		up_q.clear();
		/* (the bridge's own buffering, a guess: a transfer each way;
		   the conversion's filters, 5 ms each way) */
		l.fifo_samples_ = static_cast<s16>(
			(2 * FRAME_OCTETS / 2 + 2 * RATE / 200) * MODEM_RATE / RATE);
		return true;
	}

	/* (Chip) the interrupt's transfer, the IN ones submitted, the OUT
	   ones idle (each sent when an IN one has come) */
	bool start_streams() override
	{
		int err;
		l.iso_.out_buf.assign(static_cast<size_t>(l.frame_ + l.frame_ / 2), 0);
		l.delay_ = static_cast<s16>(l.iso_.out_buf.size() + l.fifo_samples_);
		l.intr_ = libusb_alloc_transfer(0);
		if (!l.intr_)
			return false;
		libusb_fill_interrupt_transfer(l.intr_, l.h_, l.ep_intr_, l.intr_buf_,
					       l.intr_len_, intr_done, &l, 0);
		if ((err = libusb_submit_transfer(l.intr_)) < 0) {
			ERR("%s: its interrupt: %s\n", name(), libusb_strerror(err));
			return false;
		}
		l.in_flight_++;
		for (s16 i = 0; i < std::max(IN_TRANSFERS, OUT_TRANSFERS); i++) {
			for (bool in : { true, false }) {
				if (i >= (in ? IN_TRANSFERS : OUT_TRANSFERS))
					continue;
				libusb_transfer *t = libusb_alloc_transfer(0);
				u8 *b = static_cast<u8 *>(calloc(1, FRAME_OCTETS));
				if (!t || !b) {
					free(b);
					libusb_free_transfer(t);
					return false;
				}
				libusb_fill_bulk_transfer(t, l.h_, in ? l.iso_.ep_in : l.iso_.ep_out,
							  b, FRAME_OCTETS,
							  in ? in_done : out_done, &l.iso_, 0);
				t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
				(in ? l.iso_.in : l.iso_.out)[i] = t;
				if (!in) {
					l.iso_.idle_out.push_back(t);
					continue;
				}
				if ((err = libusb_submit_transfer(t)) < 0) {
					ERR("%s: an IN transfer: %s\n", name(),
					    libusb_strerror(err));
					return false;
				}
				l.in_flight_++;
			}
		}
		return true;
	}

	/* (Chip) its notifications */
	void event(const u8 *b, s16 len) override { notes(b, len); }

	/* (Chip) an IN transfer come: its samples, an OUT transfer of as
	   many (dropped if none is idle, as the module does), it again */
	bool in_come(libusb_transfer *t) override
	{
		s16 n = 0;
		if (t->status == LIBUSB_TRANSFER_COMPLETED) {
			n = static_cast<s16>(t->actual_length / 2);
			for (s16 k = 0; k < n; k++) {
				float y[2];
				s16 m = down.run(static_cast<s16>(
					t->buffer[2 * k] | u16{t->buffer[2 * k + 1]} << 8), y);
				for (s16 j = 0; j < m; j++)
					l.iso_.in_buf.push_back(sample(y[j]));
			}
		} else {
			TRACE("Cadmus 2: IN transfer: status %d\n", t->status);
		}
		if (n)
			send(n);
		int err = libusb_submit_transfer(t);
		if (err == 0)
			l.in_flight_++;
		else
			l.park(t, err);
		return true;
	}

	/* y within 16 bits */
	static s16 sample(float y)
	{
		return static_cast<s16>(y > 32767.f ? 32767 : y < -32768.f ? -32768 :
					std::lround(y));
	}

	/* n of the device's samples: the playback's at 16000 Hz (silence
	   past them) */
	void take_out(s16 n)
	{
		while (up_q.size() < static_cast<size_t>(n)) {
			s16 x = 0;
			if (!l.iso_.out_buf.empty()) {
				x = l.iso_.out_buf.front();
				l.iso_.out_buf.pop_front();
			}
			float y[2];
			s16 m = up.run(x, y);
			for (s16 j = 0; j < m; j++)
				up_q.push_back(sample(y[j]));
		}
	}

	/* an OUT transfer of n samples, the playback's (dropped if no
	   transfer is idle) */
	void send(s16 n)
	{
		n = std::min<s16>(n, FRAME_OCTETS / 2);
		take_out(n);
		if (l.iso_.idle_out.empty()) {
			up_q.erase(up_q.begin(), up_q.begin() + n);
			return;
		}
		libusb_transfer *t = l.iso_.idle_out.front();
		l.iso_.idle_out.pop_front();
		for (s16 k = 0; k < n; k++) {
			s16 x = up_q.front();
			up_q.pop_front();
			t->buffer[2 * k] = static_cast<u8>(x);
			t->buffer[2 * k + 1] = static_cast<u8>(x >> 8);
		}
		t->length = 2 * n;
		int err = libusb_submit_transfer(t);
		if (err == 0)
			l.in_flight_++;
		else
			l.park(t, err);
	}

	/* (Chip) an OUT transfer sent: idle */
	bool out_sent(Iso &s, libusb_transfer *t) override
	{
		if (t->status != LIBUSB_TRANSFER_COMPLETED)
			TRACE("Cadmus 2: OUT transfer: status %d\n", t->status);
		s.idle_out.push_back(t);
		return true;
	}

	/* a report of the ring (the monitor's 08 01): its start, the
	   second within RING_GAP_MS */
	void ring_report()
	{
		u64 now = l.now_ms();
		if (!ringing && ring_last && now - ring_last <= RING_GAP_MS) {
			TRACE("Cadmus 2: the line rings\n");
			ringing = true;
			ring_since = ring_pulse = now;
			l.rings_++;
		}
		/* (0 is never) */
		ring_last = now ? now : 1;
	}

	/* the ring's pulses while it rings; its end once no report has
	   come for RING_GAP_MS (a pulse), or dropped past any ring's
	   length */
	void ring_tick()
	{
		if (!ringing)
			return;
		u64 now = l.now_ms();
		if (now - ring_last > RING_GAP_MS) {
			TRACE("Cadmus 2: the line stopped ringing\n");
			ringing = false;
			l.rings_++;
			return;
		}
		if (now - ring_since > RING_LONGEST_MS) {
			DBG("Cadmus 2: rang %" PRIu64 " ms with no end: not a ring\n",
			    now - ring_since);
			ringing = false;
			ring_last = 0;
			return;
		}
		while (now - ring_pulse >= RING_PULSE_MS) {
			ring_pulse += RING_PULSE_MS;
			l.rings_++;
		}
	}

	/* (Chip) the hook: the codec's word */
	s16 set_hook(s16 state, bool) override
	{
		if (state == MODEM_HOOK_OFF)
			ringing = false;
		return word(state == MODEM_HOOK_OFF ? WORD_OFF_HOOK : WORD_ON_HOOK) ?
			0 : -1;
	}
	/* (Chip) the hook switch off hook or on */
	void hook(bool off) override { word(off ? WORD_OFF_HOOK : WORD_ON_HOOK); }
	/* (Chip) pulse dialling's break or make */
	s16 pulse(bool broken) override
	{
		return word(broken ? WORD_ON_HOOK : WORD_OFF_HOOK) ? 0 : -1;
	}
	/* (Chip) its ring timed on hook */
	void pumped(size_t) override
	{
		if (!l.off_hook())
			ring_tick();
	}
	/* (Chip) on hook, the streams stopped, as the module hangs up */
	void stop() override
	{
		if (!l.claimed_ || !comm_claimed)
			return;
		word(WORD_ON_HOOK);
		script(S_STOP_6, sizeof(S_STOP_6), SLOT);
		script(S_STOP_4, sizeof(S_STOP_4), SLOT);
	}
	/* (Chip) interface 1 released */
	void close() override
	{
		if (comm_claimed)
			libusb_release_interface(l.h_, COMM_IF);
		comm_claimed = false;
	}
};

/* the Cadmus 2 */
std::unique_ptr<UsbLine::Chip> UsbLine::cadmus2_chip(UsbLine &l)
{
	return std::make_unique<Cadmus2>(l);
}

} // namespace dmd
