/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hcf_usb.h - a Conexant (Rockwell) HCF USB modem's DSP, as its USB
 *                side shows it: the Phoebe Pocket 56K (USB 1690:0103)
 *                and the others Conexant's WINACUSB.sys drives
 *
 *    An HCF modem's DSP runs the data pump; the host runs the rest (AT,
 *    V.42, fax).  Over USB the DSP is its registers: the RC-series data
 *    pump's interface memory (0x00-0x1f, a second bank 0x20-0x3f, the
 *    ID block 0x41-0x4d).  What this does is what WINACUSB.sys does
 *    (docs/hardware-details.md#conexant-usb-hcf-modems); the code is
 *    ours.
 *
 *    The pipes are interface 0's endpoints in the order its descriptor
 *    lists them:
 *    0 OUT  register writes: [tag, reg, value] triples (tag the LED
 *           octet, | 0x80 for reg >= 0x20), up to 64 a transfer; while
 *           the DSP's code goes down a [tag, 0x10] pair ends them
 *    1 IN   the line side's registers (SmartDAA boards; not used here)
 *    2 OUT  the DSP's code while downloading (up to 0x140 a transfer)
 *    3 IN   the T-buffer (not used here)
 *    4 OUT  the data bytes the modem sends
 *    5 IN   the status: records of up to 0x3c octets (below)
 *    6 OUT  the GPIO: [gpout, gpout enable, mode, 0]; mode bit 3 holds
 *           the modem interface in reset
 *    7 OUT  the LEDs (not used here)
 *
 *    A status record: [0] flags (bit 7 NEWS, bit 6 NEWC); [1..0x1d]
 *    registers 0x1d..0x01 (register r at 0x1e - r); then pairs, [0x1e
 *    register's value, a received data octet] (the octet is one if the
 *    first has bit 0, RDBF).  While the DSP is in its reset state the
 *    record has its ID block where registers 0x1c..0x10 are: register
 *    r (0x41..0x4d) at 0x4f - r; 0x4d/0x4c 0x0451 and 0x4b/0x4a 0x49a5,
 *    its signature.
 */
#ifndef DMODEMD_HCF_USB_H
#define DMODEMD_HCF_USB_H

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "hcf_image.h"

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace hcf {

/* a register's bit field, as WINACUSB.sys codes them: 0xMMSSRR (MM the
   mask, SS the shift, RR the register) */
constexpr unsigned field(unsigned reg, unsigned shift, unsigned mask)
{
	return mask << 16 | shift << 8 | reg;
}
/* the whole of register reg */
constexpr unsigned reg_byte(unsigned reg) { return field(reg, 0, 0xff); }

/* a few of the interface memory's */
enum : unsigned {
	R_BUFFER = 0x10,                /* TBUFFER/RBUFFER */
	R_CONF = 0x12,                  /* configuration code */
	R_ABCODE = 0x14,
	R_DL_LOW = 0x16,                /* download address, then checksum */
	R_DL_HIGH = 0x17,
	R_ABECHO = 0x1b,
	R_STATUS = 0x1e,                /* RDBF bit 0, TDBE bit 3 */
	F_DOWNLOAD = field(0x04, 4, 0x10),
	F_SOFT_RESET = field(0x1a, 7, 0x80),
	F_NEWC = field(0x1f, 0, 0x01),
};

/* the DSP's ID block, as the reset state reports it */
struct Id {
	uint16_t sig1 = 0, sig2 = 0;    /* 0x0451, 0x49a5 */
	uint16_t w48 = 0, w46 = 0, w44 = 0;
	uint16_t chip = 0;              /* 0x42/0x43: "17", "37", "47" */
	uint16_t w41 = 0;               /* 0x41 (and register 0) */
};

class Usb {
public:
	/* log: each register write, status record and transfer on stderr */
	explicit Usb(int log = 0) : log_(log) {}
	~Usb();
	Usb(const Usb &) = delete;
	Usb &operator=(const Usb &) = delete;

	/* the modem vid:pid (0: the known ones), the first one found:
	   true, or false with why in err */
	bool open(uint16_t vid, uint16_t pid, std::string &err);
	void close();

	/* its endpoints, as found (for the report) */
	struct Pipe {
		uint8_t ep = 0, type = 0;
		uint16_t max = 0;
	};
	/* its pipes, in order */
	const std::vector<Pipe> &pipes() const { return pipes_; }
	/* its vendor ID */
	uint16_t vid() const { return vid_; }
	/* its product ID */
	uint16_t pid() const { return pid_; }

	/* the GPIO octets (pipe 6) */
	bool gpio(uint8_t gpout, uint8_t enable, uint8_t mode);

	/* the field's value in the host's copy */
	unsigned get(unsigned f) const;
	/* the field set (the register then goes at the next flush) */
	void set(unsigned f, unsigned value);
	/* the registers set since, to the device: false if it is gone */
	bool flush();

	/* the device's work for ms (its status read): false if it is
	   gone */
	bool run(int ms);
	/* run until cond, ms at most: whether it held */
	bool until(const std::function<bool()> &cond, int ms);

	/* the reset (WakeupResetUsbDatapump, then DP_SoftReset): the
	   modem interface's, then the DSP's; its ID block once it says
	   it is up (500 ms at most): true, or false with why */
	bool reset(Id &id, std::string &err);

	/* module m of img (SPX if spx) down to the DSP (ForceDownload,
	   DpDownload): true, or false with why (its checksum the DSP's
	   in *sum) */
	bool download(const Image &img, const Module &m, bool spx,
		      std::string &err, unsigned *sum = nullptr);

	/* the status records seen */
	unsigned records() const { return records_; }
	/* the data octets received */
	std::deque<uint8_t> &received() { return rx_; }

private:
	int log_;
	libusb_context *ctx_ = nullptr;
	libusb_device_handle *h_ = nullptr;
	uint16_t vid_ = 0, pid_ = 0;
	std::vector<Pipe> pipes_;
	bool gone_ = false;

	uint8_t regs_[0x50] = {};       /* the host's copy */
	bool dirty_[0x40] = {};
	uint8_t gpio_[4] = {};
	uint8_t tag_ = 0;               /* the LED octet */
	bool reset_state_ = false;      /* the DSP's reset: its ID block */
	bool id_seen_ = false;
	bool downloading_ = false;
	unsigned records_ = 0;
	std::deque<uint8_t> rx_;

	libusb_transfer *status_[2] = {};
	int out_busy_ = 0;              /* OUT transfers in flight */

	bool out(int pipe, const uint8_t *p, size_t n);
	void record(const uint8_t *r, size_t n);
	static void status_done(libusb_transfer *t);
	static void out_done(libusb_transfer *t);
	void defaults();
};

/* the IDs Conexant's drivers take for the HCF USB modems */
bool known(uint16_t vid, uint16_t pid);

} // namespace hcf

#endif
