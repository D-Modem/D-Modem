/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    usb_agere.cpp - the Agere USB 2.0 soft modem (USB 047E:2892, 047E:2828)
 *                    as the modem's line: UsbLine's Kind::Agere (usb_line.h)
 *
 *    What the device wants is what Agere's drivers do, read from them
 *    (docs/agere-usb.md: agrsm 2.1.73's agrmodemlib.o, whose symbols
 *    name its functions, and AGRSM.sys 2.2.98 where they differ); the
 *    code is ours.  In order, as Agere's Linux driver goes (uss_probe,
 *    Usb_HandleStartDevice, CUsbInit, UsbMerInitCodec, CUsbStart):
 *
 *    - SET_CONFIGURATION 2 as the driver sends it (bmRequestType 0x0b),
 *      then interface 0's alternate setting 2: a bulk IN and a bulk OUT
 *      endpoint;
 *    - the ROM's version (command 1), GPIO set-up and the "enumeration"
 *      LED, the ARM's clock lowered (command 0x12);
 *    - the codec found (its register 0xc0 read: a Mercury; a "Polaris"
 *      DAA, whose register reads fail, is not done here), started (its
 *      registers as UsbMerInitCodec writes them, its two gains), on hook;
 *    - the bulk patch into the ARM (command 9, and 7 to hook it in)
 *      unless it is there (its function table at 0x70000a64 says so), the
 *      clock raised, the endpoints' halts and FIFOs cleared, two bulk
 *      transfers each way, the samples started (command 0x10).
 *
 *    Each IN transfer is 100 octets: the count of samples (48), a status
 *    word (bit 0 the ring detector, the high octet 0), the samples (S16
 *    little-endian, 8000 Hz).  Each OUT one the same, its status 0; one
 *    goes for each IN one, with its count, so that the playback keeps the
 *    codec's clock.
 *
 */

#include <algorithm>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <log.h>

#include "agere_patch.h"
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
	CMD_PATCH = 0x09, CMD_CODEC_READ = 0x0a, CMD_CODEC_WRITE = 0x0b,
	CMD_STREAM = 0x10, CMD_CUSTOM_ASYNC = 0x11, CMD_CHANGE_CLK = 0x12;
/* cmd an IN command (its data from the device) */
bool cmd_in(u8 cmd)
{
	return cmd == 1 || cmd == 2 || cmd == 4 || cmd == 6 || cmd == 10;
}
/* the most a command carries (x_usb_command's) */
const u16 CMD_MAX = 0x4b0;

/* the ARM's memory: the controller's registers (the PLL's state, the
   clock's divider, two set up at start), the GPIOs (directions, inputs,
   outputs; the LED on bits 2 and 3), the ROM's function table, the
   endpoints' FIFOs */
const u32 CTRL_PLL = 0xcd000008, CTRL_ARMCDR = 0xcd00000c,
	CTRL_30 = 0xcd000030, CTRL_34 = 0xcd000034;
const u32 GPIO_DIR = 0x30000000, GPIO_OUT = 0x30000008;
const u16 GPIO_LED_MASK = 0xfff3, GPIO_LED = 0x000c;
const u32 FUNCTABLE = 0x70000a64;
const u32 FIFO_IN = 0x90002048, FIFO_OUT = 0x90004048;
const u16 FIFO_RESET = 0x000f;

/* what the bulk patches put in the function table's entries 4 and 7,
   and the clock change patch in entry 6 (ROM 1.0.6) */
const u32 V106_4 = 0x700013f0, V106_7 = 0x70001400;
const u32 FLASH_4 = 0x700013f0, FLASH_7 = 0x70001400;
const u32 ROM_4 = 0x70001588, ROM_7 = 0x70001598;
const u32 CLOCK_6 = 0x70001140;
/* the bulk patches' command 0x10: the ROM's mailbox (the word at +4: the
   mode in its low half, 2 once answered) and the stream's state the
   patch keeps (2 streaming, 1 stopped), the same in all three */
const u32 PATCH_MAILBOX = 0x70000e24, PATCH_STREAM = 0x70000a0c;
const u8 STREAM_ON = 2;
/* the function table's entry 0: the bit of a 1.2.1 in flash */
const u32 FUNCTABLE_FLASH = 0x20000000;

/* the Mercury codec's registers: the hook (0 off, 1 on), its ready bit */
const u16 MER_HOOK = 0x00c0, MER_STATUS = 0x02dc;
/* its two gains (MerGain1, MerGain2) */
const u16 MER_GAIN1 = 0xc990, MER_GAIN2 = 0x3b0e;

/* the bulk transfers: each way, their octets (a header and 48 samples) */
const s16 BULK_TRANSFERS = 2;
const s16 BULK_LEN = 100, BULK_SAMPLES = 48;
/* the device's own delay, a guess: a packet */
const s16 AGERE_FIFO_SAMPLES = 48;
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

/* the patches, by the hashes in agere_patch.h, found in the file's
   octets d (n of them) into out; DMODEM_AGERE_TEST_PATCHES=1: the
   tests' ones as well (fake_agere.cpp's) */
void find_patches(const u8 *d, size_t n, std::vector<u8> *out)
{
	std::vector<std::pair<int, agere::Patch>> want;
	for (int i = 0; i < agere::PATCHES; i++) {
		want.emplace_back(i, agere::known(i));
		if (getenv("DMODEM_AGERE_TEST_PATCHES")) {
			agere::Patch t = agere::known(i);
			std::vector<u8> p(t.len);
			for (u32 k = 0; k < t.len; k++)
				p[k] = agere::test_patch(i, k);
			t.head = agere::fnv(p.data(), agere::PATCH_HEAD);
			t.all = agere::fnv(p.data(), p.size());
			want.emplace_back(i, t);
		}
	}
	for (size_t off = 0; off + agere::PATCH_HEAD <= n; off++) {
		u64 h = agere::fnv(d + off, agere::PATCH_HEAD);
		for (const auto &w : want) {
			const agere::Patch &p = w.second;
			if (h != p.head || !out[w.first].empty() ||
			    off + p.len > n || agere::fnv(d + off, p.len) != p.all)
				continue;
			out[w.first].assign(d + off, d + off + p.len);
		}
	}
}

/* a file's octets (at most 64 MB); false if it can't be read */
bool read_file(const std::filesystem::path &p, std::vector<u8> &d)
{
	std::error_code ec;
	auto size = std::filesystem::file_size(p, ec);
	if (ec || size > (64u << 20))
		return false;
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return false;
	d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

} // namespace

/* see usb_line.h: an Agere USB modem's USB IDs */
bool UsbLine::agere_device(u16 vendor, u16 product)
{
	return vendor == AGERE_VENDOR &&
		(product == AGERE_2892 || product == AGERE_2828);
}

/* the directories with the Agere patches D-Modem ships
   (firmware/agere): DMODEM_AGERE_FIRMWARE's alone if it is set; else
   the installed one, the source tree's (dmodemd run from its build),
   and on Windows the one beside dmodemd.exe */
std::vector<std::string> UsbLine::agere_firmware_dirs()
{
	std::vector<std::string> dirs;
	if (const char *e = getenv("DMODEM_AGERE_FIRMWARE")) {
		dirs.push_back(e);
		return dirs;
	}
#ifdef DMODEM_FIRMWARE_DIR
	dirs.push_back(DMODEM_FIRMWARE_DIR "/agere");
#endif
#ifdef DMODEM_SOURCE_FIRMWARE_DIR
	dirs.push_back(DMODEM_SOURCE_FIRMWARE_DIR "/agere");
#endif
#ifdef _WIN32
	char exe[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, exe, sizeof(exe));
	if (n > 0 && n < sizeof(exe))
		dirs.push_back((std::filesystem::path(exe).parent_path() /
				"firmware" / "agere").string());
#endif
	return dirs;
}

/* see usb_line.h: set_agere_driver()'s file, or the files of its
   directory and under it, searched for the patches (all of them, or
   those there are); without it the patches D-Modem ships
   (agere_firmware_dirs()); false (said) if there are none */
bool UsbLine::load_agere_patches()
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (agere_driver_.empty()) {
		for (const std::string &d : agere_firmware_dirs())
			if (fs::is_directory(d, ec)) {
				agere_driver_ = d;
				break;
			}
	}
	if (agere_driver_.empty()) {
		ERR("Agere USB modem: no bulk patch: D-Modem's firmware/agere "
		    "is not installed (or DMODEM_AGERE_FIRMWARE's directory is "
		    "not there); --usb-agere-driver <Agere's AGRSM.sys, "
		    "AGRSM64.sys, agrmodem.ko or agrmodemlib.o, or a directory "
		    "with one> takes it from Agere's driver (docs/agere-usb.md)\n");
		return false;
	}
	std::vector<fs::path> files;
	fs::path root(agere_driver_);
	if (fs::is_directory(root, ec)) {
		for (fs::recursive_directory_iterator it(root, ec), end;
		     !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec))
				files.push_back(it->path());
		std::sort(files.begin(), files.end());
	} else {
		files.push_back(root);
	}
	std::vector<u8> d;
	for (const fs::path &p : files) {
		bool all = true;
		for (const auto &q : agere_patch_)
			all = all && !q.empty();
		if (all)
			break;
		if (!read_file(p, d))
			continue;
		std::vector<u8> found[agere::PATCHES];
		find_patches(d.data(), d.size(), found);
		for (int i = 0; i < agere::PATCHES; i++)
			if (agere_patch_[i].empty() && !found[i].empty()) {
				agere_patch_[i].swap(found[i]);
				DBG("Agere: %s in %s\n", agere::known(i).name,
				    p.string().c_str());
			}
	}
	for (int i = 0; i < agere::V121_ROM + 1; i++)
		if (!agere_patch_[i].empty())
			return true;
	ERR("Agere USB modem: no bulk patch in %s (its driver's AGRSM.sys, "
	    "AGRSM64.sys, agrmodem.ko or agrmodemlib.o has it)\n",
	    agere_driver_.c_str());
	return false;
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
   head), up to its codec on hook; the streams are agere_streams()' */
bool UsbLine::open_agere()
{
	int err;
	u8 b[2] = { 0, 0 };

	if (voice_law_ != VoiceLaw::None)
		ERR("Agere USB modem: no voice codec (--usb-voice-codec)\n");
	if (!load_agere_patches())
		return false;
	/* (uss_probe's: SET_CONFIGURATION 2 as it sends it, its answer
	   not looked at) */
	err = libusb_control_transfer(h_, 0x0b, LIBUSB_REQUEST_SET_CONFIGURATION,
				      2, 0, nullptr, 0, 10);
	TRACE("Agere: SET_CONFIGURATION 2 (0x0b): %d\n", err);
	if ((err = libusb_set_interface_alt_setting(h_, 0, 2)) < 0) {
		ERR("Agere USB modem: alternate setting 2: %s\n",
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
	for (s16 i = 0; cfg->bNumInterfaces >= 1 &&
	     i < cfg->interface[0].num_altsetting; i++) {
		const libusb_interface_descriptor &d = cfg->interface[0].altsetting[i];
		if (d.bAlternateSetting != 2)
			continue;
		for (s16 k = 0; k < d.bNumEndpoints; k++) {
			const libusb_endpoint_descriptor &e = d.endpoint[k];
			if ((e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
			    LIBUSB_TRANSFER_TYPE_BULK)
				continue;
			if (e.bEndpointAddress & LIBUSB_ENDPOINT_IN)
				bulk_in_ = bulk_in_ ? bulk_in_ : e.bEndpointAddress;
			else
				bulk_out_ = bulk_out_ ? bulk_out_ : e.bEndpointAddress;
		}
	}
	libusb_free_config_descriptor(cfg);
	if (!bulk_in_ || !bulk_out_) {
		ERR("Agere USB modem: no bulk IN and OUT endpoints in "
		    "alternate setting 2\n");
		return false;
	}
	DBG("Agere: bulk out %02x, in %02x, %s speed\n", bulk_out_, bulk_in_,
	    libusb_get_device_speed(libusb_get_device(h_)) == LIBUSB_SPEED_HIGH ?
	    "high" : "full");
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
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x0008)) ||
	    !agere_write16(CTRL_34, 2) || !agere_write16(GPIO_DIR, 0xffff) ||
	    !agere_read16(GPIO_OUT, &v) ||
	    !agere_write16(GPIO_OUT, static_cast<u16>((v & GPIO_LED_MASK) |
							   GPIO_LED)) ||
	    !agere_clock(false))
		return false;
	/* CUsbInit: the codec's: a Mercury answers register 0xc0 */
	if (!agere_read16(CTRL_30, &v) ||
	    !agere_write16(CTRL_30, static_cast<u16>(v & ~0x000c)) ||
	    !agere_write16(CTRL_34, 2))
		return false;
	if (!codec_read(MER_HOOK, &v)) {
		ERR("Agere USB modem: its codec does not answer register 0xc0: "
		    "a \"Polaris\" DAA, which dmodemd does not drive yet "
		    "(docs/agere-usb.md)\n");
		return false;
	}
	if (!agere_mercury())
		return false;
	fifo_samples_ = AGERE_FIFO_SAMPLES;
	return true;
}

/* see usb_line.h: the ARM's clock lowered (at start) or raised (for the
   samples), as Usb_LowerArmClock and Usb_BoostArmClock: a ROM 1.0.6 its
   clock change patch and USS_CMD_CUSTOM_ASYNC, the others
   USS_CMD_CHANGE_CLK, none if its version is not known; raised, the PLL
   not locked, the clock's source and divider set by hand */
bool UsbLine::agere_clock(bool boost)
{
	return agere_clock_div(boost ? 3 : 0xf, boost ? "raised" : "lowered");
}

/* see usb_line.h: agere_clock()'s, with its divider (0xf lowered) */
bool UsbLine::agere_clock_div(u16 div, const char *what)
{
	bool boost = div != 0xf;
	if (agere_rom_ != 0xffff && (agere_rom_ & 0xf) == 6) {
		u8 t[32];
		if (!agere_read(FUNCTABLE, t, sizeof(t)))
			return false;
		if (le32(t + 24) != CLOCK_6) {
			std::vector<u8> &p = agere_patch_[agere::ASYNC_CLOCKCHANGE];
			if (p.empty()) {
				ERR("Agere USB modem: ROM 1.0.6 wants the clock "
				    "change patch (async_clockchange), not in "
				    "%s\n", agere_driver_.c_str());
				return false;
			}
			DBG("Agere: clock change patch\n");
			if (!agere_cmd(CMD_PATCH, static_cast<u16>(p.size()), 6, 0,
				       p.data()))
				return false;
		}
		if (!agere_cmd(CMD_CUSTOM_ASYNC, 0, 1, div, nullptr))
			return false;
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
	DBG("Agere: ARM clock %s, %d MHz (ARMCDR %x)\n", what,
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
	/* (read back, those that differ logged: some are banked by 0x048,
	   or read other bits than they take; for the first device, whose
	   samples are all 0x8000) */
	for (const W &w : first)
		if (codec_read(w.reg, &v) && v != w.v)
			DBG("Agere: codec register %03x reads %04x (%04x written)\n",
			    w.reg, v, w.v);
	return true;
}

/* see usb_line.h: the bulk patch for the ROM (1.0.6's; a 1.2.1's in
   flash or in ROM, as its function table's entry 0 says) unless its
   entries 4 and 7 say it is in, as Uss_InitForUsingBulkEps: downloaded
   (command 9, index 4), hooked in (index 7: entry 7's function, with
   the word at 0) */
bool UsbLine::agere_bulk_patch()
{
	u8 t[32];
	if (!agere_read(FUNCTABLE, t, sizeof(t)))
		return false;
	int which;
	u32 e4, e7;
	if ((agere_rom_ & 0xfff) == 0x106) {
		which = agere::V106;
		e4 = V106_4;
		e7 = V106_7;
	} else if (le32(t) & FUNCTABLE_FLASH) {
		which = agere::V121_FLASH;
		e4 = FLASH_4;
		e7 = FLASH_7;
	} else {
		which = agere::V121_ROM;
		e4 = ROM_4;
		e7 = ROM_7;
	}
	if (le32(t + 16) == e4 && le32(t + 28) == e7) {
		DBG("Agere: bulk patch in already\n");
		return true;
	}
	std::vector<u8> &p = agere_patch_[which];
	if (p.empty()) {
		ERR("Agere USB modem: its ROM wants %s, not in %s\n",
		    agere::known(which).name, agere_driver_.c_str());
		return false;
	}
	DBG("Agere: %s\n", agere::known(which).name);
	u16 w0;
	if (!agere_cmd(CMD_PATCH, static_cast<u16>(p.size()), 4, 0, p.data()) ||
	    !agere_read16(0, &w0))
		return false;
	u8 hook[12] = { 0, 0, 0, 0, 2, 0, static_cast<u8>(w0),
			static_cast<u8>(w0 >> 8), static_cast<u8>(e7),
			static_cast<u8>(e7 >> 8), static_cast<u8>(e7 >> 16),
			static_cast<u8>(e7 >> 24) };
	if (!agere_cmd(CMD_PATCH, sizeof(hook), 7, 0, hook) ||
	    !agere_read(FUNCTABLE, t, sizeof(t)))
		return false;
	if (le32(t + 16) != e4 || le32(t + 28) != e7)
		DBG("Agere: after the patch its function table has %08x and "
		    "%08x, not %08x and %08x\n", le32(t + 16), le32(t + 28),
		    e4, e7);
	return true;
}

/* see usb_line.h: the samples started, as CUsbStart: the patch, the
   clock raised, the endpoints' halts and the FIFOs cleared, the
   transfers (IN ones, then OUT ones of silence), command 0x10 (at full
   speed 1 before them, at high speed 4 and 1 after them) */
bool UsbLine::agere_streams()
{
	u16 v;
	int err;
	bool high = libusb_get_device_speed(libusb_get_device(h_)) ==
		LIBUSB_SPEED_HIGH;

	if (!agere_read16(CTRL_PLL, &v) || !agere_bulk_patch() || !agere_clock(true))
		return false;
	if (!high && !agere_cmd(CMD_STREAM, 0, 1, 0, nullptr))
		return false;
	libusb_clear_halt(h_, bulk_out_);
	libusb_clear_halt(h_, bulk_in_);
	if (!agere_write16(FIFO_OUT, FIFO_RESET) || !agere_write16(FIFO_IN, FIFO_RESET))
		return false;
	iso_.out_buf.assign(frame_ + frame_ / 2, 0);
	delay_ = static_cast<s16>(iso_.out_buf.size() + BULK_TRANSFERS * BULK_SAMPLES +
				  fifo_samples_);
	for (s16 i = 0; i < BULK_TRANSFERS; i++) {
		iso_.in[i] = libusb_alloc_transfer(0);
		iso_.out[i] = libusb_alloc_transfer(0);
		u8 *ib = static_cast<u8 *>(calloc(1, BULK_LEN));
		u8 *ob = static_cast<u8 *>(calloc(1, BULK_LEN));
		if (!iso_.in[i] || !iso_.out[i] || !ib || !ob) {
			free(ib);
			free(ob);
			return false;
		}
		libusb_fill_bulk_transfer(iso_.in[i], h_, bulk_in_, ib, BULK_LEN,
					  in_done, &iso_, 0);
		iso_.in[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_fill_bulk_transfer(iso_.out[i], h_, bulk_out_, ob, BULK_LEN,
					  out_done, &iso_, 0);
		iso_.out[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		if ((err = libusb_submit_transfer(iso_.in[i])) < 0) {
			ERR("Agere USB modem: an IN transfer: %s\n",
			    libusb_strerror(err));
			return false;
		}
		in_flight_++;
	}
	for (s16 i = 0; i < BULK_TRANSFERS; i++) {
		agere_fill(iso_.out[i], BULK_SAMPLES);
		if ((err = libusb_submit_transfer(iso_.out[i])) < 0) {
			ERR("Agere USB modem: an OUT transfer: %s\n",
			    libusb_strerror(err));
			return false;
		}
		in_flight_++;
	}
	if (high && (!agere_cmd(CMD_STREAM, 0, 4, 0, nullptr) ||
		     !agere_cmd(CMD_STREAM, 0, 1, 0, nullptr)))
		return false;
	agere_stream_check(high);
	if (getenv("DMODEM_AGERE_CLOCK_SWEEP"))
		agere_clock_sweep();
	agere_t0_ = 0;
	return true;
}

/* see usb_line.h: the samples' rate measured at several ARM clocks while
   streaming (after a moment to settle, over SWEEP_MS each), logged, then
   the clock raised again: what the codec's rate follows */
void UsbLine::agere_clock_sweep()
{
	static const u16 divs[] = { 3, 2, 4, 5, 6, 8, 0xf };
	const u64 SETTLE_MS = 300, SWEEP_MS = 1500;
	auto run = [&](u64 ms) {
		u64 t0 = sys_ms();
		while (!dead_ && sys_ms() - t0 < ms) {
			struct timeval tv = { 0, 10000 };
			libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
		}
	};
	DBG("Agere: clock sweep (DMODEM_AGERE_CLOCK_SWEEP)\n");
	for (u16 d : divs) {
		u16 cdr = 0;
		if (!agere_clock_div(d, "for the sweep") ||
		    !agere_read16(CTRL_ARMCDR, &cdr))
			break;
		run(SETTLE_MS);
		size_t n0 = iso_.in_buf.size();
		u64 t0 = sys_ms();
		run(SWEEP_MS);
		u64 dt = sys_ms() - t0;
		u64 n = iso_.in_buf.size() - n0;
		s16 lo = 0, hi = 0;
		for (size_t k = n0; k < iso_.in_buf.size(); k++) {
			s16 x = iso_.in_buf[k];
			if (k == n0 || x < lo)
				lo = x;
			if (k == n0 || x > hi)
				hi = x;
		}
		DBG("Agere: clock sweep: divider %x, %d MHz (ARMCDR %x): %" PRIu64
		    " samples a second, from %d to %d\n", d,
		    480 / (2 * static_cast<s16>(cdr) + 2), cdr,
		    dt ? n * 1000 / dt : 0, lo, hi);
		iso_.in_buf.clear();
		if (dead_)
			return;
	}
	agere_clock(true);
}

/* see usb_line.h: the patch's mailbox and stream state read back after
   command 0x10 (logged); not streaming: command 0x10 again with its mode
   in wValue instead of wIndex (the ROM leaves wIndex in the mailbox: the
   first device's, seen streaming with it there; wValue as the doc first
   had it, for a ROM that differs) */
void UsbLine::agere_stream_check(bool high)
{
	u8 mb[8], st[4];
	auto look = [&](const char *when) {
		if (!agere_read(PATCH_MAILBOX, mb, sizeof(mb)) ||
		    !agere_read(PATCH_STREAM, st, sizeof(st)))
			return false;
		DBG("Agere: %s: the mailbox %02x %02x %02x %02x %02x %02x %02x "
		    "%02x, the stream's state %u (%u: streaming)\n", when,
		    mb[0], mb[1], mb[2], mb[3], mb[4], mb[5], mb[6], mb[7],
		    st[0], STREAM_ON);
		return true;
	};
	if (!look("after command 0x10") || st[0] == STREAM_ON)
		return;
	ERR("Agere USB modem: the bulk patch has not started the stream: "
	    "command 0x10 again, its mode as wValue\n");
	if (high && !agere_cmd(CMD_STREAM, 0, 0, 4, nullptr))
		return;
	if (!agere_cmd(CMD_STREAM, 0, 0, 1, nullptr) ||
	    !look("after command 0x10, its mode as wValue"))
		return;
	if (st[0] == STREAM_ON)
		DBG("Agere: streaming, its mode as wValue: tell D-Modem "
		    "(docs/agere-usb.md)\n");
	else
		ERR("Agere USB modem: still not streaming: tell D-Modem, with "
		    "this log (docs/agere-usb.md)\n");
}

/* see usb_line.h: an IN transfer's header and samples (in_complete()):
   the samples queued, their count for the OUT transfer that answers it,
   the ring detector's bit (a pulse a packet while it is set) */
void UsbLine::agere_in(libusb_transfer *t)
{
	s16 n = 0;
	if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length >= 4) {
		const u8 *b = t->buffer;
		u16 count = static_cast<u16>(b[0] | u16{b[1]} << 8);
		u16 status = static_cast<u16>(b[2] | u16{b[3]} << 8);
		if (count == 0xaa55) {
			DBG("Agere: AA55 from the device\n");
			count = 0;
		}
		u16 room = static_cast<u16>((t->actual_length - 4) / 2);
		n = static_cast<s16>(std::min(count, room));
		for (s16 k = 0; k < n; k++) {
			s16 x = static_cast<s16>(b[4 + 2 * k] | u16{b[5 + 2 * k]} << 8);
			iso_.in_buf.push_back(x);
			/* (their range, for the rate's log line) */
			if (agere_t0_ && !agere_rate_said_) {
				agere_lo_ = agere_seen_ ? std::min(agere_lo_, x) : x;
				agere_hi_ = agere_seen_ ? std::max(agere_hi_, x) : x;
				agere_seen_ = true;
			}
		}
		if (status != agere_status_)
			TRACE("Agere: status %04x (%u samples)\n", status, count);
		agere_status_ = status;
		agere_ring_ = !(status >> 8) && (status & 1);
		if (agere_ring_)
			rings_++;
	} else if (t->status != LIBUSB_TRANSFER_COMPLETED) {
		DBG("Agere: IN transfer: status %d\n", t->status);
	}
	iso_.out_lengths.push_back(std::vector<s16>(1, n));
}

/* see usb_line.h: an OUT transfer of 48 samples queued, n asked for whatever (silence if there
   are too few): its header (n, 0), the samples, 100 octets in all */
void UsbLine::agere_fill(libusb_transfer *t, s16 n)
{
	/* (the bulk patch plays 48 samples whatever the count: always 48) */
	(void)n;
	n = BULK_SAMPLES;
	memset(t->buffer, 0, BULK_LEN);
	t->buffer[0] = static_cast<u8>(n);
	t->buffer[1] = static_cast<u8>(n >> 8);
	for (s16 k = 0; k < n; k++) {
		s16 x = 0;
		if (!iso_.out_buf.empty()) {
			x = iso_.out_buf.front();
			iso_.out_buf.pop_front();
		}
		t->buffer[4 + 2 * k] = static_cast<u8>(x);
		t->buffer[5 + 2 * k] = static_cast<u8>(x >> 8);
	}
	t->length = BULK_LEN;
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
	if (hz < AGERE_RATE * 95 / 100 || hz > AGERE_RATE * 105 / 100)
		ERR("Agere USB modem: %" PRIu64 " samples a second, not %" PRIu32
		    ": tell D-Modem (docs/agere-usb.md)\n", hz, AGERE_RATE);
}

} // namespace dmd
