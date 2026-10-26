/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    usb_agere.cpp - the Agere USB 2.0 soft modem (USB 047E:2892, 047E:2828)
 *                    as the modem's line: UsbLine's Kind::Agere (usb_line.h)
 *
 *    The device streams on its ROM's own isochronous endpoints, as Agere's
 *    Windows driver has it (docs/agere-usb.md: read from the device's ROM
 *    and AGRSM.sys 2.2.98; its start-up as agrsm 2.1.73's agrmodemlib.o
 *    does it, whose symbols name its functions); the code is ours.  In
 *    order:
 *
 *    - SET_CONFIGURATION 2 as the driver sends it (bmRequestType 0x0b),
 *      then interface 0's alternate setting 1: an isochronous IN and OUT
 *      endpoint, a packet every 2 microframes (high speed only);
 *    - the ROM's version (command 1), GPIO set-up and the "enumeration"
 *      LED, the ARM's clock lowered (command 0x12);
 *    - the codec found (its register 0xc0 read: a Mercury; a "Polaris"
 *      DAA, whose register reads fail, is not done here), started (its
 *      registers as UsbMerInitCodec writes them, its two gains), on hook;
 *    - the ROM's function table as the ROM has it (a bulk patch of an
 *      earlier driver's, left in it, would take the samples), the clock
 *      raised, the stream started (command 0x0c), the transfers.
 *
 *    Each OUT packet has a header (a count of samples, a word not looked
 *    at) and that many samples, S16 little-endian at 8000 Hz; the ROM
 *    answers each with an IN packet of the samples it has (a count, at
 *    most 10, or 0xaa55 for none; a status word, bit 0 the ring
 *    detector; the samples).  As many samples go out as have come in,
 *    so that the playback keeps the codec's clock.
 *
 */

#include <algorithm>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <log.h>

#include "dm_types.h"
#include "usb_line.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "usb: " fmt, ##__VA_ARGS__)
#define TRACE(fmt, ...) dcore::log_printf(2, "usb: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

const u16 AGERE_VENDOR = 0x047e;
const u16 AGERE_2892 = 0x2892, AGERE_2828 = 0x2828;

/* its commands (bRequest), and which of them are IN (Usb_Command's
   table: 1, 2, 4, 6 and 10) */
const u8 CMD_VERSION = 0x01, CMD_READ = 0x02, CMD_WRITE = 0x03,
	CMD_CODEC_READ = 0x0a, CMD_CODEC_WRITE = 0x0b,
	CMD_ISO_START = 0x0c, CMD_ISO_STOP = 0x0d, CMD_CHANGE_CLK = 0x12;
/* cmd an IN command (its data from the device) */
bool cmd_in(u8 cmd)
{
	return cmd == 1 || cmd == 2 || cmd == 4 || cmd == 6 || cmd == 10;
}
/* the most a command carries (x_usb_command's) */
const u16 CMD_MAX = 0x4b0;

/* the ARM's memory: the controller's registers (the PLL's state, the
   clock's divider, two set up at start), the GPIOs (directions, inputs,
   outputs; the LED on bits 2 and 3), the ROM's function table, the USB
   controller's speed (1 high) */
const u32 CTRL_PLL = 0xcd000008, CTRL_ARMCDR = 0xcd00000c,
	CTRL_30 = 0xcd000030, CTRL_34 = 0xcd000034;
const u32 GPIO_DIR = 0x30000000, GPIO_OUT = 0x30000008;
/* Windows' first step at start (0x1e820(1), Linux's U2SetRing): the
   controller's 0xcd000044, then codec registers 0x0e0, 0x0a4, 0x0b0 */
const u32 CTRL_44 = 0xcd000044;
const u16 MER_0B0 = 0x00b0;
/* the ROM's ring indices (four words), logged */
const u32 RINGS = 0x7000101c;
/* how long each GPIO is flipped for in the LED probe (agere_led_probe()) */
const int PROBE_MS = 1500, PROBE_GAP_MS = 500;
/* GPIOs 2 and 3: set at start and on stop, as Agere's drivers do
   (U2SetEnumerationLED: the reference design's LEDs) */
const u16 GPIO_LED_MASK = 0xfff3, GPIO_LED = 0x000c;
/* the Hiro H50113's OH LED (D6): GPIO 4, lit while set (the first
   device's LED probe); its READY/DATA (D8) is on none of GPIOs 0 to 15 (16 to 23 are
   still to be probed) */
const u16 GPIO_LED_OH = 0x0010;
const u32 FUNCTABLE = 0x70000a64;
const u32 USB_SPEED = 0x90000018;

/* the ROM's function table as it has it at boot (0x3970): entry 4 its
   deferred commands' handler (0x14b0), entry 2 its own, the others an
   empty function; a patch rewrites entries (a bulk patch 4 and 7) */
const u32 ROM_TABLE[8] = { 0x14af, 0x14af, 0x221c, 0x14af,
			   0x14b1, 0x14af, 0x14af, 0x14af };

/* the Mercury codec's registers: the hook (0 off, 1 on), its ready bit */
const u16 MER_HOOK = 0x00c0, MER_STATUS = 0x02dc;
/* a Polaris DAA's (U2Pol*: the drivers take one when register 0xc0 reads
   with bit 15 set): its link (0xc4 bit 2 once up), its line side's
   control (0xd0), its calibration reading (0xdc), and its DAA registers
   (written as 0x8000 | reg << 8 | value to 0xc8; read by writing reg << 8
   there, then 0xcc) */
const u16 POL_FLAG = 0x8000, POL_LINK = 0x00c4, POL_LINK_UP = 0x0004;
const u16 POL_CTRL = 0x00d0, POL_CAL = 0x00dc;
const u16 POL_DAA = 0x00c8, POL_DAA_READ = 0x00cc;
const u32 CTRL_3C = 0xcd00003c;
const u64 POL_LINK_MS = 500;
/* the OH LED's blinking while the line rings, half a period (ms of the
   line's clock); how long a ring keeps it at it */
const u64 LED_FAST_MS = 125, LED_RING_MS = 1000;
/* its two gains (MerGain1, MerGain2) */
const u16 MER_GAIN1 = 0xc990, MER_GAIN2 = 0x3b0e;

/* the isochronous stream (alternate setting 1): its packets' octets at
   most, and a packet's header; the samples a packet, at most, each way
   (OUT: the packet's room; IN: the ROM's) and as the stream starts; the
   packets a transfer (6 ms, as Windows' driver has them: 24) */
const u8 ISO_ALT = 1;
const s16 ISO_MAX = 16, ISO_HEAD = 4;
const s16 IN_MOST = 10;
/* how far the OUT samples' averaged lead may stray from its settled
   value before a transfer is a sample longer or shorter (agere_fill()) */
const s64 LEAD_SLACK = 6;
const s16 AGERE_PACKETS = 24;
/* the ROM's "no samples" count */
const u16 NONE = 0xaa55;
/* command 0x0c's data (AGRSM.sys's, at high speed): an empty IN
   packet's size, (2 + 2) * 2 octets; the receive ring's start; the
   transmit ring's, 24 samples of silence ahead of the codec; the
   microframes served, every other one */
const u16 START[4] = { 2, 0, 24, 0x2aaa };
const s16 RING_AHEAD = 24;
/* its rate */
const u32 AGERE_RATE = 8000;
/* the rate measured over this long (ms) */
const u64 RATE_CHECK_MS = 10000;

const u16 TIMEOUT_MS = 1000;

/* the system's clock, in ms */
u64 sys_ms()
{
	using namespace std::chrono;
	return static_cast<u64>(duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count());
}

/* a little-endian u32 at b */
u32 le32(const u8 *b)
{
	return u32{b[0]} | u32{b[1]} << 8 | u32{b[2]} << 16 | u32{b[3]} << 24;
}

} // namespace

/* see usb_line.h: an Agere USB modem's USB IDs */
bool UsbLine::agere_device(u16 vendor, u16 product)
{
	return vendor == AGERE_VENDOR &&
		(product == AGERE_2892 || product == AGERE_2828);
}

/* see usb_line.h: Usb_Command's vendor request, to the device: IN for
   the commands that read, len octets into buf (all of them wanted), or
   OUT with buf's; false (logged) */
bool UsbLine::agere_cmd(u8 cmd, u16 len, u16 index, u16 value, u8 *buf)
{
	bool in = cmd_in(cmd);
	if (len > CMD_MAX)
		return false;
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
					  LIBUSB_RECIPIENT_DEVICE |
					  (in ? LIBUSB_ENDPOINT_IN :
						LIBUSB_ENDPOINT_OUT),
					  cmd, value, index, len ? buf : nullptr,
					  len, TIMEOUT_MS);
	TRACE("Agere: command %02x index %04x value %04x, %u octets: %d\n",
	      cmd, index, value, len, err);
	if (err < 0 || (in && err < len)) {
		ERR("Agere USB modem: command %02x (%04x, %04x): %s\n", cmd,
		    index, value, err < 0 ? libusb_strerror(err) : "too short");
		return false;
	}
	return true;
}

/* see usb_line.h: the ARM's memory at addr (command 2: its high half
   the value, its low the index) */
bool UsbLine::agere_read(u32 addr, u8 *buf, u16 len)
{
	return agere_cmd(CMD_READ, len, static_cast<u16>(addr),
			 static_cast<u16>(addr >> 16), buf);
}

/* see usb_line.h: a 16-bit word of the ARM's memory */
bool UsbLine::agere_read16(u32 addr, u16 *v)
{
	u8 b[2] = { 0, 0 };
	if (!agere_read(addr, b, 2))
		return false;
	*v = static_cast<u16>(b[0] | u16{b[1]} << 8);
	return true;
}

/* see usb_line.h: a 16-bit word written (command 3) */
bool UsbLine::agere_write16(u32 addr, u16 v)
{
	u8 b[2] = { static_cast<u8>(v), static_cast<u8>(v >> 8) };
	return agere_cmd(CMD_WRITE, 2, static_cast<u16>(addr),
			 static_cast<u16>(addr >> 16), b);
}

/* see usb_line.h: a codec register (command 0x0a, the register the
   index); quietly false if the codec does not answer (a Polaris) */
bool UsbLine::codec_read(u16 reg, u16 *v)
{
	u8 b[2] = { 0, 0 };
	int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
					  LIBUSB_RECIPIENT_DEVICE |
					  LIBUSB_ENDPOINT_IN, CMD_CODEC_READ,
					  0, reg, b, 2, TIMEOUT_MS);
	TRACE("Agere: codec register %03x: %d (%02x%02x)\n", reg, err, b[1], b[0]);
	if (err < 2)
		return false;
	*v = static_cast<u16>(b[0] | u16{b[1]} << 8);
	return true;
}

/* see usb_line.h: a codec register written (command 0x0b: the register
   the index, the value the value, no data) */
bool UsbLine::codec_write(u16 reg, u16 v)
{
	return agere_cmd(CMD_CODEC_WRITE, 0, reg, v, nullptr);
}


/* see usb_line.h: the device opened as Agere's driver does (the file's
   head), up to its codec on hook; the stream is agere_streams()' */
bool UsbLine::open_agere()
{
	int err;
	u8 b[2] = { 0, 0 };

	if (voice_law_ != VoiceLaw::None)
		ERR("Agere USB modem: no voice codec (--usb-voice-codec)\n");
	if (libusb_get_device_speed(libusb_get_device(h_)) != LIBUSB_SPEED_HIGH) {
		ERR("Agere USB modem: not at high speed: it streams only on a "
		    "USB 2.0 port (docs/agere-usb.md)\n");
		return false;
	}
	/* (its one configuration, 1, as its descriptors have it; its
	   answer not looked at) */
	err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_STANDARD |
				      LIBUSB_RECIPIENT_DEVICE,
				      LIBUSB_REQUEST_SET_CONFIGURATION, 1, 0,
				      nullptr, 0, 10);
	TRACE("Agere: SET_CONFIGURATION 1: %d\n", err);
	if ((err = libusb_set_interface_alt_setting(h_, 0, ISO_ALT)) < 0) {
		ERR("Agere USB modem: alternate setting %u: %s\n", ISO_ALT,
		    libusb_strerror(err));
		return false;
	}
	libusb_config_descriptor *cfg;
	if ((err = libusb_get_active_config_descriptor(libusb_get_device(h_),
						       &cfg)) < 0) {
		ERR("Agere USB modem: its configuration: %s\n",
		    libusb_strerror(err));
		return false;
	}
	iso_.ep_in = iso_.ep_out = 0;
	for (s16 i = 0; cfg->bNumInterfaces >= 1 &&
	     i < cfg->interface[0].num_altsetting; i++) {
		const libusb_interface_descriptor &d = cfg->interface[0].altsetting[i];
		if (d.bAlternateSetting != ISO_ALT)
			continue;
		for (s16 k = 0; k < d.bNumEndpoints; k++) {
			const libusb_endpoint_descriptor &e = d.endpoint[k];
			if ((e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
			    LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
				continue;
			s16 size = static_cast<s16>(std::min<int>(e.wMaxPacketSize & 0x7ff,
								  ISO_MAX));
			if (e.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
				iso_.ep_in = e.bEndpointAddress;
				iso_.max_in = size;
			} else {
				iso_.ep_out = e.bEndpointAddress;
				iso_.max_out = size;
			}
		}
	}
	libusb_free_config_descriptor(cfg);
	if (!iso_.ep_in || !iso_.ep_out || iso_.max_in < ISO_HEAD ||
	    iso_.max_out < ISO_HEAD + 2) {
		ERR("Agere USB modem: no isochronous IN and OUT endpoints in "
		    "alternate setting %u\n", ISO_ALT);
		return false;
	}
	DBG("Agere: isochronous out %02x, in %02x, %d octets a packet\n",
	    iso_.ep_out, iso_.ep_in, iso_.max_out);
	/* Usb_HandleStartDevice: the ROM's version (0: not known, as
	   Windows' driver has it; DMODEM_AGERE_ROM=<hex> instead), the
	   GPIOs (U2InitGpio), the LED, the clock lowered */
	if (!agere_cmd(CMD_VERSION, 2, 0, 0, b))
		return false;
	agere_rom_ = static_cast<u16>(b[0] | u16{b[1]} << 8);
	if (!agere_rom_)
		agere_rom_ = 0xffff;
	if (const char *e = getenv("DMODEM_AGERE_ROM"))
		agere_rom_ = static_cast<u16>(strtoul(e, nullptr, 16));
	DBG("Agere: ROM %u.%u.%u (%04x)\n", agere_rom_ >> 8 & 0xf,
	    agere_rom_ >> 4 & 0xf, agere_rom_ & 0xf, agere_rom_);
	u16 v;
	/* Windows' first step (0x1e820(1)): 0xcd000030 bit 2 cleared,
	   0xcd000044 = 3, the codec's 0x0e0, 0x0a4 and 0x0b0 */
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x0004)) ||
	    !agere_write16(CTRL_44, 3))
		return false;
	/* (not a Polaris's: its codec's own start-up is below) */
	if (codec_write(0x0e0, 0x0304))
		codec_write(0x0a4, 0x0004) && codec_write(MER_0B0, 0x000a);
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x0008)) ||
	    !agere_write16(CTRL_34, 2) || !agere_write16(GPIO_DIR, 0xffff) ||
	    !agere_read16(GPIO_OUT, &v) ||
	    !agere_write16(GPIO_OUT, agere_gpio_ = static_cast<u16>(
				   ((v & GPIO_LED_MASK) | GPIO_LED) & ~GPIO_LED_OH)) ||
	    !agere_clock(false))
		return false;
	/* CUsbInit: the codec's: a Mercury answers register 0xc0 */
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x000c)) ||
	    !agere_write16(CTRL_34, 2))
		return false;
	/* (as both drivers: bit 15 a Polaris; a read that fails, 0, a
	   Mercury) */
	agere_polaris_ = codec_read(MER_HOOK, &v) && (v & POL_FLAG);
	DBG("Agere: a %s\n", agere_polaris_ ? "Polaris DAA" : "Mercury codec");
	if (!(agere_polaris_ ? agere_polaris() : agere_mercury()))
		return false;
	agere_led_probe();
	fifo_samples_ = RING_AHEAD;
	return true;
}

/* see usb_line.h: the ARM's clock lowered (at start) or raised (for the
   samples), as Usb_LowerArmClock and Usb_BoostArmClock: USS_CMD_CHANGE_CLK
   (0xf lowered, 3 raised), not with a ROM 1.0.6 (whose clock change wants
   Agere's patch: its clock left as it is) or one whose version is not
   known; raised, the PLL not locked, the clock's source and divider set
   by hand */
bool UsbLine::agere_clock(bool boost)
{
	u16 div = boost ? 3 : 0xf;
	if ((agere_rom_ & 0xfff) == 0x106) {
		if (!boost)
			DBG("Agere: ROM 1.0.6: its clock left as it is\n");
	} else if (agere_rom_ != 0xffff &&
		   !agere_cmd(CMD_CHANGE_CLK, 0, 1, div, nullptr)) {
		return false;
	}
	u16 pll = 0, cdr = 0;
	if (!agere_read16(CTRL_PLL, &pll))
		return false;
	if (boost && !pll) {
		DBG("Agere: its PLL not locked: the clock set by hand\n");
		if (!agere_write16(CTRL_ARMCDR, 2) || !agere_read16(CTRL_PLL, &pll) ||
		    !agere_write16(CTRL_PLL, static_cast<u16>(pll | 2)))
			return false;
	}
	if (!agere_read16(CTRL_ARMCDR, &cdr))
		return false;
	DBG("Agere: ARM clock %s, %d MHz (ARMCDR %x)\n",
	    boost ? "raised" : "lowered",
	    480 / (2 * static_cast<s16>(cdr) + 2), cdr);
	return true;
}

/* see usb_line.h: the Mercury codec started as UsbMerInitCodec does (its
   registers, each gain once it is ready), then on hook */
bool UsbLine::agere_mercury()
{
	struct W {
		u16 reg, v;
	};
	static const W first[] = {
		{ 0x0e0, 0x0304 }, { 0x0e4, 0x0070 }, { 0x0e8, 0x042f },
		{ 0x0f0, 0x2242 }, { 0x048, 0x0002 }, { 0x2c0, 0x01ff },
		{ 0x2c4, 0xfaac }, { 0x2d4, 0x1411 }, { 0x2d0, 0x9f02 },
		{ 0x2cc, 0x01c0 }, { 0x048, 0x0003 }, { 0x3cc, 0x0001 },
		{ 0x3c0, 0x187c }, { 0x3d0, 0x0000 }, { 0x3cc, 0x0101 },
		{ 0x3c4, 0x3dd0 }, { 0x3c8, 0x0000 },
	};
	/* (UsbMerInitCodec writes 0, off hook, to the hook, and CUsbInit
	   puts it on hook just after: here on hook at once, not to seize
	   the line for a moment, which an exchange takes for a dialled
	   pulse) */
	static const W last[] = {
		{ 0x048, 0x0002 }, { 0x2cc, 0x41c0 }, { 0x048, 0x0000 },
		{ MER_HOOK, 0x0001 }, { 0x08c, 0x0000 },
	};
	u16 v;
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x0004)) ||
	    !agere_write16(CTRL_34, 2) || !codec_write(0x0b4, 0x1387) ||
	    !codec_read(0x0a4, &v) ||
	    !codec_write(0x0a4, static_cast<u16>(v | 0x0004)))
		return false;
	for (const W &w : first)
		if (!codec_write(w.reg, w.v))
			return false;
	/* the gains: each once the codec says it is ready (bit 0 of
	   register 0x2dc), 200 ms at most */
	const W gains[] = { { 0x0010, MER_GAIN1 }, { 0x0008, MER_GAIN2 } };
	for (const W &g : gains) {
		u64 t0 = sys_ms();
		while (!(codec_read(MER_STATUS, &v) && (v & 1)) &&
		       sys_ms() - t0 < 200)
			;
		if (!(v & 1))
			DBG("Agere: codec not ready for its gain (%04x)\n", v);
		if (!codec_write(0x3d0, g.reg) || !codec_write(0x3c4, g.v))
			return false;
	}
	for (const W &w : last)
		if (!codec_write(w.reg, w.v))
			return false;
	return true;
}


/* see usb_line.h: the ROM's function table as it has it at boot; entries
   a patch has rewritten (a bulk patch of an earlier driver's, there until
   the device is reset: its per-sample function would take the samples)
   put back; false (logged) */
bool UsbLine::agere_table()
{
	u8 t[32];
	if (!agere_read(FUNCTABLE, t, sizeof(t)))
		return false;
	for (s16 i = 0; i < 8; i++) {
		if (le32(t + 4 * i) == ROM_TABLE[i])
			continue;
		DBG("Agere: function table entry %d %08x (a patch's), put back\n",
		    i, le32(t + 4 * i));
		u8 b[4] = { static_cast<u8>(ROM_TABLE[i]),
			    static_cast<u8>(ROM_TABLE[i] >> 8), 0, 0 };
		u32 a = FUNCTABLE + 4u * static_cast<u32>(i);
		if (!agere_cmd(CMD_WRITE, 4, static_cast<u16>(a),
			       static_cast<u16>(a >> 16), b))
			return false;
	}
	return true;
}

/* see usb_line.h: the ROM's stream (command 0x0c, its data as Windows'
   driver sends it at high speed), stopped first (0x0d) */
bool UsbLine::agere_start()
{
	u16 speed = 0;
	agere_read16(USB_SPEED, &speed);
	if (speed != 1)
		DBG("Agere: its USB speed register %04x (1: high)\n", speed);
	u8 d[8];
	for (s16 i = 0; i < 4; i++) {
		d[2 * i] = static_cast<u8>(START[i]);
		d[2 * i + 1] = static_cast<u8>(START[i] >> 8);
	}
	return agere_cmd(CMD_ISO_STOP, 0, 0, 0, nullptr) &&
		agere_cmd(CMD_ISO_START, sizeof(d), 1, 8, d);
}

/* see usb_line.h: the samples started, as CUsbStart does it without a
   bulk patch: the ROM's table, the clock raised, the ROM's stream; then
   the isochronous transfers each way (AGERE_PACKETS packets each, OUT
   ones of silence) */
bool UsbLine::agere_streams()
{
	int err;
	if (!agere_table() || !agere_clock(true) || !agere_start())
		return false;
	agere_regs("streaming");
	if (getenv("DMODEM_AGERE_DUMP"))
		agere_dump();
	agere_transfers_ = 16;
	if (const char *e = getenv("DMODEM_AGERE_TRANSFERS"))
		agere_transfers_ = static_cast<s16>(std::max(2L, std::min(
			static_cast<long>(MAX_TRANSFERS), strtol(e, nullptr, 10))));
	iso_.out_buf.assign(frame_ + frame_ / 2, 0);
	agere_lead_ = agere_lead_lo_ = agere_lead_hi_ = 0;
	agere_avg_ = agere_ref_ = 0;
	agere_settled_ = false;
	agere_longer_ = agere_shorter_ = 0;
	agere_last_in_ms_ = 0;
	agere_stopped_ = false;
	agere_ahead_ = static_cast<s32>(AGERE_RATE);
	delay_ = static_cast<s16>(iso_.out_buf.size() + RING_AHEAD +
				  agere_transfers_ * AGERE_PACKETS * 2);
	for (s16 i = 0; i < agere_transfers_; i++) {
		iso_.in[i] = libusb_alloc_transfer(AGERE_PACKETS);
		iso_.out[i] = libusb_alloc_transfer(AGERE_PACKETS);
		u8 *ib = static_cast<u8 *>(calloc(AGERE_PACKETS, iso_.max_in));
		u8 *ob = static_cast<u8 *>(calloc(AGERE_PACKETS, iso_.max_out));
		if (!iso_.in[i] || !iso_.out[i] || !ib || !ob) {
			free(ib);
			free(ob);
			return false;
		}
		libusb_fill_iso_transfer(iso_.in[i], h_, iso_.ep_in, ib,
					 AGERE_PACKETS * iso_.max_in, AGERE_PACKETS,
					 in_done, &iso_, 0);
		libusb_set_iso_packet_lengths(iso_.in[i], iso_.max_in);
		iso_.in[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_fill_iso_transfer(iso_.out[i], h_, iso_.ep_out, ob,
					 AGERE_PACKETS * iso_.max_out, AGERE_PACKETS,
					 out_done, &iso_, 0);
		iso_.out[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		if ((err = libusb_submit_transfer(iso_.in[i])) < 0) {
			ERR("Agere USB modem: an IN transfer: %s\n",
			    libusb_strerror(err));
			return false;
		}
		in_flight_++;
	}
	for (s16 i = 0; i < agere_transfers_; i++) {
		agere_fill(iso_.out[i]);
		if ((err = libusb_submit_transfer(iso_.out[i])) < 0) {
			ERR("Agere USB modem: an OUT transfer: %s\n",
			    libusb_strerror(err));
			return false;
		}
		in_flight_++;
	}
	agere_t0_ = 0;
	return true;
}

/* see usb_line.h: DMODEM_AGERE_DUMP: the configuration's alternate
   settings and endpoints, then the ARM's memory as hex, 128 octets a
   line (command 2, not traced): the ROM from 0 (up to 64 KB, until a read
   fails), its RAM (0x70000000, 8 KB: the patches, the ROM's data), the
   controller's registers, the codec interface's, the endpoints' FIFOs'
   registers; while streaming */
void UsbLine::agere_dump()
{
	libusb_config_descriptor *cfg;
	if (libusb_get_active_config_descriptor(libusb_get_device(h_), &cfg) == 0) {
		DBG("Agere: dump: configuration %u, %u interfaces\n",
		    cfg->bConfigurationValue, cfg->bNumInterfaces);
		for (u8 i = 0; i < cfg->bNumInterfaces; i++)
			for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
				const libusb_interface_descriptor &d =
					cfg->interface[i].altsetting[a];
				for (u8 k = 0; k < d.bNumEndpoints; k++) {
					const libusb_endpoint_descriptor &e = d.endpoint[k];
					DBG("Agere: dump: interface %u alt %u: endpoint %02x, "
					    "attributes %02x, %u octets, interval %u\n",
					    i, d.bAlternateSetting, e.bEndpointAddress,
					    e.bmAttributes, e.wMaxPacketSize, e.bInterval);
				}
				if (!d.bNumEndpoints)
					DBG("Agere: dump: interface %u alt %u: no endpoints\n",
					    i, d.bAlternateSetting);
			}
		libusb_free_config_descriptor(cfg);
	}
	auto dump = [&](u32 base, u32 len) {
		const u16 LINE = 128;
		for (u32 off = 0; off < len; off += LINE) {
			u8 b[LINE];
			u32 a = base + off;
			int err = libusb_control_transfer(h_, LIBUSB_REQUEST_TYPE_VENDOR |
							  LIBUSB_RECIPIENT_DEVICE |
							  LIBUSB_ENDPOINT_IN, CMD_READ,
							  static_cast<u16>(a >> 16),
							  static_cast<u16>(a), b, LINE,
							  TIMEOUT_MS);
			if (err < LINE) {
				DBG("Agere: dump: %08x: %d, stopped\n", a, err);
				return;
			}
			char hex[2 * LINE + 1];
			for (u16 k = 0; k < LINE; k++)
				snprintf(hex + 2 * k, 3, "%02x", b[k]);
			DBG("Agere: dump %08x %s\n", a, hex);
		}
	};
	DBG("Agere: dump (DMODEM_AGERE_DUMP)\n");
	dump(0x00000000, 0x10000);
	dump(0x70000000, 0x2000);
	dump(0xcd000000, 0x80);
	dump(0xe0000000, 0x200);
	dump(0x90002000, 0x80);
	dump(0x90004000, 0x80);
	dump(0x90006000, 0x80);
	DBG("Agere: dump over\n");
}


/* see usb_line.h: an IN transfer's packets (in_complete()): each one's
   samples queued (none for 0xaa55), owed to the OUT packets; the ring
   detector's bit (a pulse a packet while it is set) */
void UsbLine::agere_in(libusb_transfer *t)
{
	if (t->status != LIBUSB_TRANSFER_COMPLETED) {
		DBG("Agere: IN transfer: status %d\n", t->status);
		return;
	}
	u64 now = sys_ms();
	s32 got = 0;
	for (int i = 0; i < t->num_iso_packets; i++) {
		const libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
		/* (a packet that failed, or came short: counted, for the log) */
		if (p.status != LIBUSB_TRANSFER_COMPLETED) {
			if (agere_in_bad_++ < 5)
				DBG("Agere: an IN packet failed: %d (%u octets)\n", p.status,
				    p.actual_length);
			continue;
		}
		if (p.actual_length < static_cast<unsigned>(ISO_HEAD)) {
			agere_in_short_++;
			continue;
		}
		const u8 *b = libusb_get_iso_packet_buffer_simple(t, static_cast<unsigned>(i));
		u16 count = static_cast<u16>(b[0] | u16{b[1]} << 8);
		u16 status = static_cast<u16>(b[2] | u16{b[3]} << 8);
		if (count == NONE)
			count = 0;
		/* (a count that isn't one: the packet said, a few times) */
		if (count > IN_MOST || 2u * count + ISO_HEAD > p.actual_length) {
			if (agere_odd_++ < 5)
				DBG("Agere: an odd IN packet (%u octets): %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    p.actual_length, b[0], b[1], b[2], b[3],
				    p.actual_length > 4 ? b[4] : 0, p.actual_length > 5 ? b[5] : 0,
				    p.actual_length > 6 ? b[6] : 0, p.actual_length > 7 ? b[7] : 0);
		}
		s16 n = static_cast<s16>(std::min<unsigned>(
			std::min<unsigned>(count, IN_MOST),
			(p.actual_length - ISO_HEAD) / 2));
		for (s16 k = 0; k < n; k++) {
			s16 x = static_cast<s16>(b[ISO_HEAD + 2 * k] |
						 u16{b[ISO_HEAD + 1 + 2 * k]} << 8);
			iso_.in_buf.push_back(x);
			/* (their range, for the rate's log line) */
			if (agere_t0_ && !agere_rate_said_) {
				agere_lo_ = agere_seen_ ? std::min(agere_lo_, x) : x;
				agere_hi_ = agere_seen_ ? std::max(agere_hi_, x) : x;
				agere_seen_ = true;
			}
		}
		agere_lead_ -= n;
		agere_lead_lo_ = std::min(agere_lead_lo_, agere_lead_);
		agere_in_n_ += static_cast<u64>(n);
		got += n;
		if (status != agere_status_)
			TRACE("Agere: status %04x\n", status);
		agere_status_ = status;
		agere_ring_ = (status & 1) != 0;
		if (agere_ring_)
			agere_ring_ms_ = now_ms();
		if (agere_ring_)
			rings_++;
	}
	/* (every 10 s: the samples each way, the most owed, the OUT packets
	   short of what was owed; the stream's stopping, once) */
	if (got)
		agere_last_in_ms_ = now;
	if (!agere_stats_ms_)
		agere_stats_ms_ = now;
	bool stopped = agere_last_in_ms_ && now - agere_last_in_ms_ > 500;
	if (stopped && !agere_stopped_)
		DBG("Agere: its samples stopped %" PRIu64 " ms ago: %" PRIu64 " in, %" PRIu64
		    " out, lead %" PRId64 " (%" PRId64 " to %" PRId64 "), %" PRIu64
		    " packets of silence, %zu to play\n",
		    now - agere_last_in_ms_, agere_in_n_, agere_out_n_, agere_lead_,
		    agere_lead_lo_, agere_lead_hi_, agere_short_, iso_.out_buf.size());
	agere_stopped_ = stopped;
	if (now - agere_stats_ms_ >= 10000) {
		agere_stats_ms_ = now;
		DBG("Agere: %" PRIu64 " samples in, %" PRIu64 " out, lead %" PRId64 " (%" PRId64
		    " to %" PRId64 "), averaged %+" PRId64 " from its settled value, transfers %" PRIu64
		    " longer and %" PRIu64 " shorter, %" PRIu64 " packets of silence, %zu to play; IN "
		    "packets %" PRIu64 " failed, %" PRIu64 " short, %u odd\n",
		    agere_in_n_, agere_out_n_, agere_lead_, agere_lead_lo_, agere_lead_hi_,
		    agere_settled_ ? (agere_avg_ - agere_ref_) / 256 : 0, agere_longer_,
		    agere_shorter_, agere_short_, iso_.out_buf.size(), agere_in_bad_,
		    agere_in_short_, agere_odd_);
		agere_lead_lo_ = agere_lead_hi_ = agere_lead_;
	}
}

/* see usb_line.h: an OUT transfer filled: 2 samples a packet, 48 a
   transfer, the codec's 2 a packet's time, so the modem's transmit ring
   keeps the lead of 24 that 0x0c gave it. That lead is the samples sent
   (OUT transfers done) less those come (IN), but for a bias of how the
   two kinds of transfer complete, and that count jumps by a transfer's
   48 as they do: so it is averaged, its average once the stream has
   settled (2 s) is its reference, and a transfer is one sample shorter
   (47: a packet of 1) or longer (49: a packet of 3) only while the
   average is more than LEAD_SLACK from it: for the drift between the
   USB's clock and the codec's, a sample or so a second. (Windows'
   driver's own, 46 or 50 by the count's sign at each transfer, AGRSM.sys
   0x1ba37, makes it wander past the ring's 24 either way: the samples
   played chopped.) The playback's samples, silence if it has none */
void UsbLine::agere_fill(libusb_transfer *t)
{
	s16 total = AGERE_PACKETS * 2;
	agere_avg_ += (agere_lead_ * 256 - agere_avg_) / 64;
	if (!agere_settled_ && agere_in_n_ >= 2 * AGERE_RATE) {
		agere_settled_ = true;
		agere_ref_ = agere_avg_;
		DBG("Agere: the stream settled, its lead %" PRId64 "\n", agere_ref_ / 256);
	}
	if (agere_settled_) {
		s64 off = (agere_avg_ - agere_ref_) / 256;
		if (off > LEAD_SLACK) {
			total--;
			agere_shorter_++;
		} else if (off < -LEAD_SLACK) {
			total++;
			agere_longer_++;
		}
	}
	s16 off = 0;
	for (int i = 0; i < t->num_iso_packets; i++) {
		s16 n = 2;
		if (i == t->num_iso_packets - 1)
			n = static_cast<s16>(total - 2 * (t->num_iso_packets - 1));
		u8 *b = t->buffer + off;
		b[0] = static_cast<u8>(n);
		b[1] = static_cast<u8>(n >> 8);
		b[2] = b[3] = 0;
		bool silence = false;
		for (s16 k = 0; k < n; k++) {
			s16 x = 0;
			if (!iso_.out_buf.empty()) {
				x = iso_.out_buf.front();
				iso_.out_buf.pop_front();
			} else {
				silence = true;
			}
			b[ISO_HEAD + 2 * k] = static_cast<u8>(x);
			b[ISO_HEAD + 1 + 2 * k] = static_cast<u8>(x >> 8);
		}
		if (silence)
			agere_short_++;
		s16 len = static_cast<s16>(ISO_HEAD + 2 * n);
		t->iso_packet_desc[i].length = static_cast<unsigned>(len);
		off = static_cast<s16>(off + len);
	}
	t->length = off;
}

/* see usb_line.h */
void UsbLine::agere_sent(libusb_transfer *t)
{
	s64 n = 0;
	for (int i = 0; i < t->num_iso_packets; i++)
		if (t->iso_packet_desc[i].length >= static_cast<unsigned>(ISO_HEAD))
			n += (t->iso_packet_desc[i].length - ISO_HEAD) / 2;
	agere_lead_ += n;
	agere_lead_hi_ = std::max(agere_lead_hi_, agere_lead_);
	agere_out_n_ += static_cast<u64>(n);
}

/* see usb_line.h (update_leds()): the modem's OH LED (D6, GPIO 4, lit
   while set): lit off hook, blinking fast while the line rings; written
   only when it changes. (READY/DATA, D8, is not on GPIOs 0 to 15: not
   driven) */
void UsbLine::agere_leds(dcore::Modem &m)
{
	u64 now = now_ms();
	online_seen_ = online_seen_ ||
		m.link_state() == dcore::LinkState::Online;
	bool oh = hook_ == MODEM_HOOK_OFF;
	if (!oh && agere_ring_ms_ && now - agere_ring_ms_ < LED_RING_MS)
		oh = now / LED_FAST_MS % 2 == 0;
	u16 v = static_cast<u16>((agere_gpio_ & ~GPIO_LED_OH) |
				 (oh ? GPIO_LED_OH : 0));
	if (v == agere_gpio_)
		return;
	if (agere_write16(GPIO_OUT, v))
		agere_gpio_ = v;
}

/* see usb_line.h: the ROM's stream stopped (command 0x0d), at close */
void UsbLine::agere_stop()
{
	agere_cmd(CMD_ISO_STOP, 0, 0, 0, nullptr);
}

/* see usb_line.h: the hook (codec register 0xc0: a Mercury's 0 off hook,
   a Polaris's 1). (Not the ROM's ring indices as the drivers write them,
   0x70001020 = {24, 0}: on the first device that stopped its stream and
   then its commands) */
bool UsbLine::agere_hook(bool off)
{
	return codec_write(MER_HOOK, off == agere_polaris_ ? 1 : 0);
}

/* see usb_line.h: a Polaris's caller ID (DAA register 2: 8 on, 4 off;
   U2PolEnableCallerID); nothing on a Mercury, which hears the line on
   hook */
bool UsbLine::agere_caller_id(bool on)
{
	return !agere_polaris_ || daa_write(2, on ? 0x08 : 0x04);
}

/* see usb_line.h: a Polaris's DAA register written (through 0xc8) */
bool UsbLine::daa_write(u8 reg, u8 v)
{
	return codec_write(POL_DAA, static_cast<u16>(0x8000 | reg << 8 | v));
}

/* see usb_line.h: a Polaris's DAA register read: reg << 8 to 0xc8, then
   0xcc once its high octet is reg (5 ms apart, 10 tries) */
bool UsbLine::daa_read(u8 reg, u8 *v)
{
	for (s16 i = 0; i < 10; i++) {
		u16 r;
		if (!codec_write(POL_DAA, static_cast<u16>(reg << 8)))
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (codec_read(POL_DAA_READ, &r) && (r >> 8) == reg) {
			*v = static_cast<u8>(r);
			return true;
		}
	}
	return false;
}

/* see usb_line.h: a Polaris DAA started, as U2PolInitCodec (Windows'
   0x1f330), then its country's registers as U2PolConfigureCountry
   (0x1f720; but the pulse dialling's timings and the country's own
   register 3), on hook with caller ID on */
bool UsbLine::agere_polaris()
{
	auto ms = [](int n) {
		std::this_thread::sleep_for(std::chrono::milliseconds(n));
	};
	struct W {
		u16 reg, v;
	};
	static const W init[] = {
		{ 0x0ac, 0x0000 }, { 0x048, 0x0002 }, { 0x2c0, 0x0007 },
		{ 0x2c4, 0xfffe }, { 0x2d4, 0x1411 }, { 0x2d0, 0x9f02 },
		{ 0x2cc, 0x01c0 }, { 0x2cc, 0x41c0 }, { 0x048, 0x0000 },
		{ 0x0b4, 0x11ff },
	};
	u16 v;
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x000c)) ||
	    !agere_write16(CTRL_34, 2) || !agere_read16(CTRL_3C, &v) ||
	    !agere_write16(CTRL_3C, 5))
		return false;
	for (const W &w : init)
		if (!codec_write(w.reg, w.v))
			return false;
	/* its link: pulsed until 0xc4 says it is up (500 ms at most) */
	u64 t0 = sys_ms();
	u16 link = 0;
	do {
		if (!codec_write(0x0a4, 0) || !agere_write16(CTRL_3C, 7))
			return false;
		ms(5);
		agere_write16(CTRL_3C, 5);
		ms(5);
		codec_write(POL_CTRL, 0x2fd2);
		ms(5);
		codec_write(POL_CTRL, 0x1fd2);
		codec_write(POL_CTRL, 0xd7d2);
		codec_write(POL_CTRL, 0x17d2);
		ms(5);
		if (!codec_read(POL_LINK, &link))
			return false;
	} while (!(link & POL_LINK_UP) && sys_ms() - t0 < POL_LINK_MS);
	DBG("Agere: Polaris link %s (%04x)\n",
	    link & POL_LINK_UP ? "up" : "not up", link);
	/* its calibration: 16 readings of 0xdc's bits 10 to 14 (Gray coded)
	   that are not 0, 100 at most, their mean + 3 into 0xd0's bits 6 to
	   10 if 7 to 20 */
	u32 sum = 0;
	s16 got = 0;
	for (s16 i = 0; i < 100 && got < 16; i++) {
		if (!codec_read(POL_CAL, &v))
			break;
		u32 g = v >> 10 & 0x1f;
		g ^= g >> 1;
		g ^= g >> 2;
		g ^= g >> 4;
		if (g) {
			sum += g;
			got++;
		}
	}
	u32 n = (sum >> 4) + 3;
	DBG("Agere: Polaris calibration %u (%d readings)\n", n, got);
	if (got == 16 && n > 7 && n < 20) {
		codec_write(POL_CTRL, static_cast<u16>((0x17d2 & 0xf83f) |
						       (n | 0x20) << 6));
	} else {
		codec_write(POL_CTRL, 0xdfd2);
		codec_write(POL_CTRL, 0x1fd2);
	}
	/* U2SetRing(2), on hook */
	if (!codec_write(0x08c, 0) || !agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x0004)) ||
	    !agere_write16(CTRL_44, 3) || !codec_write(0x0e0, 0x0304) ||
	    !codec_write(0x0a4, 0) || !codec_write(MER_0B0, 0xbb80) ||
	    !agere_hook(false))
		return false;
	/* the country's: its DAA's registers 1 and 2, its revision (0x16)
	   for register 5, 6, 7, 0x1b, 4 */
	u8 rev = 0;
	daa_write(1, 0x80);
	daa_write(1, 0x01);
	daa_write(2, 0x04);
	ms(5);
	if (!daa_read(0x16, &rev))
		DBG("Agere: Polaris DAA: no revision\n");
	u8 r17 = 0;
	bool ok = daa_write(5, rev > 2 ? 0x50 : 0x54) && daa_write(6, 0x0a) &&
		daa_write(7, 0x0a) && daa_write(0x1b, 0x01) &&
		codec_write(0x0e0, 0x0304) && daa_write(4, 0x00);
	ms(5);
	daa_read(0x17, &r17);
	DBG("Agere: Polaris DAA revision %u, register 0x17 %02x\n", rev, r17);
	return ok && agere_caller_id(true);
}

/* see usb_line.h: with DMODEM_AGERE_LED_PROBE (in the environment, or
   built in), each of GPIO outputs 0 to 15 flipped in turn for PROBE_MS,
   what was written and read back logged: which lights an LED says
   which drives it */
void UsbLine::agere_led_probe()
{
	const char *e = getenv("DMODEM_AGERE_LED_PROBE");
#ifndef DMODEM_AGERE_LED_PROBE
	if (!e)
		return;
#endif
	/* (its value, if more than 1: the ms each step lasts) */
	int ms = e && atoi(e) > 1 ? atoi(e) : PROBE_MS;
	int gap = ms * PROBE_GAP_MS / PROBE_MS;
	/* (words: the GPIOs' 32 bits, a word written by the ROM as one) */
	auto get = [this](u32 a) {
		u8 b[4] = { 0, 0, 0, 0 };
		agere_read(a, b, 4);
		return b[0] | u32{b[1]} << 8 | u32{b[2]} << 16 | u32{b[3]} << 24;
	};
	auto put = [this](u32 a, u32 v) {
		u8 b[4] = { static_cast<u8>(v), static_cast<u8>(v >> 8),
			    static_cast<u8>(v >> 16), static_cast<u8>(v >> 24) };
		agere_cmd(CMD_WRITE, 4, static_cast<u16>(a),
			  static_cast<u16>(a >> 16), b);
	};
	u32 dir = get(GPIO_DIR), in = get(GPIO_DIR + 4), out = get(GPIO_OUT);
	DBG("Agere: LED probe: GPIO directions %08x, inputs %08x, outputs "
	    "%08x; each output flipped for %d ms in turn\n", dir, in, out,
	    ms);
	if (dir != 0xffffffff) {
		put(GPIO_DIR, 0xffffffff);
		DBG("Agere: LED probe: directions %08x read back\n",
		    get(GPIO_DIR));
	}
	for (int k = 0; k < 32; k++) {
		u32 v = out ^ (1u << k);
		put(GPIO_OUT, v);
		DBG("Agere: LED probe step %d: GPIO %d %s (wrote %08x, read "
		    "%08x)\n", k + 1, k, v >> k & 1 ? "high" : "low", v,
		    get(GPIO_OUT));
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		put(GPIO_OUT, out);
		std::this_thread::sleep_for(std::chrono::milliseconds(gap));
	}
	put(GPIO_DIR, dir);
	put(GPIO_OUT, out);
	DBG("Agere: LED probe over\n");
}

/* see usb_line.h: the registers that say how the codec runs, logged
   (when: "streaming", "after 10 s") */
void UsbLine::agere_regs(const char *when)
{
	static const u32 ctrl[] = { CTRL_PLL, CTRL_ARMCDR, CTRL_30, CTRL_34,
				    CTRL_44, GPIO_DIR, GPIO_DIR + 4,
				    GPIO_OUT };
	static const u16 codec[] = { 0x080, 0x0a4, 0x0a8, 0x0ac, MER_0B0,
				     0x0b8, MER_HOOK, 0x2d4, MER_STATUS };
	char line[512];
	int n = snprintf(line, sizeof(line), "Agere: %s:", when);
	for (u32 a : ctrl) {
		u8 b[4] = { 0, 0, 0, 0 };
		if (n < static_cast<int>(sizeof(line)) && agere_read(a, b, 4))
			n += snprintf(line + n, sizeof(line) - n, " %08x %08x", a,
				      b[0] | u32{b[1]} << 8 | u32{b[2]} << 16 |
					      u32{b[3]} << 24);
	}
	for (u16 r : codec) {
		u16 v;
		if (n < static_cast<int>(sizeof(line)) && codec_read(r, &v))
			n += snprintf(line + n, sizeof(line) - n, ", codec %03x %04x",
				      r, v);
	}
	u8 b[16] = {};
	if (n < static_cast<int>(sizeof(line)) && agere_read(RINGS, b, 16))
		n += snprintf(line + n, sizeof(line) - n,
			      ", rings %u %u %u %u %u %u %u %u",
			      b[0] | b[1] << 8, b[2] | b[3] << 8, b[4] | b[5] << 8,
			      b[6] | b[7] << 8, b[8] | b[9] << 8, b[10] | b[11] << 8,
			      b[12] | b[13] << 8, b[14] | b[15] << 8);
	DBG("%s\n", line);
}

/* see usb_line.h (pump()): the samples' rate, once, measured over
   RATE_CHECK_MS, said if it is not AGERE_RATE's */
void UsbLine::agere_rate_check()
{
	if (agere_rate_said_)
		return;
	u64 now = sys_ms();
	if (!agere_t0_) {
		agere_t0_ = now;
		agere_n0_ = samples_;
		return;
	}
	if (now - agere_t0_ < RATE_CHECK_MS)
		return;
	agere_rate_said_ = true;
	u64 hz = (samples_ - agere_n0_) * 1000 / (now - agere_t0_);
	DBG("Agere: %" PRIu64 " samples a second, from %d to %d\n", hz,
	    agere_lo_, agere_hi_);
	agere_regs("after 10 s");
	if (hz < AGERE_RATE * 95 / 100 || hz > AGERE_RATE * 105 / 100)
		ERR("Agere USB modem: %" PRIu64 " samples a second, not %" PRIu32
		    ": tell D-Modem (docs/agere-usb.md)\n", hz, AGERE_RATE);
}

} // namespace dmd
