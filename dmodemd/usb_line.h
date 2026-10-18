/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    usb_line.h - the modem's line a Smart Link SmartUSB56 (ST7554, USB
 *                 0483:7554 with an STLC7550 codec) through libusb: no
 *                 kernel driver
 *
 *    The ST7554 is a USB bridge to a codec and a DAA: its registers are
 *    vendor control requests, its samples isochronous transfers both
 *    ways (S16 little-endian, mono, at 8000, 9600 or 16000 Hz), its
 *    GPIOs the hook (an output) and the ring detector (an input, whose
 *    changes come on its interrupt endpoint).  The samples run both ways
 *    from open() on; each IN transfer's byte count is the next OUT
 *    transfer's, so the modem's samples go out at the codec's own clock.
 *    Each ring signal's change on the ring detector is a pulse to the
 *    modem's ring(); the codec hears nothing on hook, so there is no
 *    caller ID.  The modem dials itself, in DTMF.  Time is the codec's:
 *    the samples it has given.
 *
 */
#ifndef DMODEMD_USB_LINE_H
#define DMODEMD_USB_LINE_H

#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#include <libusb.h>

#include <dcore.h>

#include "pty.h"
#include "run_loop.h"

namespace dmd {

class UsbLine final : public dcore::Host {
public:
	/* the line of pty's modem, not yet open */
	explicit UsbLine(Pty &pty) : pty_(pty) {}
	~UsbLine();
	UsbLine(const UsbLine &) = delete;
	UsbLine &operator=(const UsbLine &) = delete;

	/* the modem dev ("" or "auto": the first SmartUSB56; <bus>:<address>
	   one of them, as lsusb shows them) opened at rate, its samples
	   running, on hook; false with the reason on stderr */
	bool open(const char *dev, unsigned rate);
	/* libusb's descriptors watched this turn */
	void watch(RunLoop &loop);
	/* libusb's events (the transfers done), then the samples that have
	   come, a frame (20 ms) at a time, through the modem; 0, or -1 if
	   the modem has gone (unplugged) */
	int pump(dcore::Modem &m);

	/* Host */
	int start(dcore::Modem &) override { running_ = true; return 0; }
	/* the samples no longer through the modem */
	int stop(dcore::Modem &) override { running_ = false; return 0; }
	int set_hook(dcore::Modem &m, int state) override;
	/* an analogue line, the modem dialling it itself */
	bool analog_line(dcore::Modem &) override { return true; }
	/* a hook flash it can make (hook_flash()) */
	bool can_flash(dcore::Modem &) override { return true; }
	void hook_flash(dcore::Modem &m, unsigned ms) override;
	int pulse_hook(dcore::Modem &m, bool broken) override;
	/* the playback's lead, in samples */
	int io_delay(dcore::Modem &) override { return delay_; }
	/* for ATI3 */
	const char *driver_name() override { return "ST7554"; }
	/* the modem's characters to the pty: how many it took */
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		return pty_.write(buf, n);
	}
	/* the clock: the device's samples so far, in ms */
	unsigned long now_ms() override {
		return (unsigned long)(samples_ * 1000 / rate_);
	}

	/* "<bus>:<address>" into bus and addr; false if it is not that */
	static bool parse_dev(const char *dev, int *bus, int *addr);

private:
	enum { ISO_TRANSFERS = 3, ISO_PACKETS = 5 };

	bool find(const char *dev);
	bool endpoints();
	bool set_reg(uint8_t reg, uint16_t v);
	bool get_reg(uint8_t reg, uint16_t *v);
	bool set_gpio(uint16_t v);
	bool start_streams();
	void close();

	static void LIBUSB_CALL in_done(libusb_transfer *t);
	static void LIBUSB_CALL out_done(libusb_transfer *t);
	static void LIBUSB_CALL intr_done(libusb_transfer *t);
	void in_complete(libusb_transfer *t);
	void out_fill(libusb_transfer *t, const int *lengths);
	void send_out();
	void gone(const char *why);

	Pty &pty_;
	libusb_context *ctx_ = nullptr;
	libusb_device_handle *h_ = nullptr;
	bool claimed_ = false;
	uint8_t ep_intr_ = 0, ep_out_ = 0, ep_in_ = 0;
	int max_out_ = 0, max_in_ = 0;
	libusb_transfer *in_[ISO_TRANSFERS] = {}, *out_[ISO_TRANSFERS] = {},
		*intr_ = nullptr;
	/* each IN transfer's packets' lengths, for the OUT one it frees */
	std::deque<std::vector<int>> out_lengths_;
	std::deque<libusb_transfer *> idle_out_;  /* OUT ones not in flight */
	int in_flight_ = 0;             /* transfers submitted */
	uint8_t intr_buf_[4] = {};
	unsigned rate_ = 9600;
	int frame_ = 192;               /* 20 ms */
	int delay_ = 0;
	std::vector<int16_t> in_buf_;   /* the codec's samples to process */
	std::deque<int16_t> out_buf_;   /* the modem's samples to send */
	std::vector<int16_t> frame_in_, frame_out_;
	unsigned long long samples_ = 0;
	bool running_ = false;
	bool dead_ = false;
	int hook_ = MODEM_HOOK_ON;
	uint16_t gpio_ = 0;
	unsigned long long flash_end_ = 0;
	unsigned rings_ = 0;            /* ring detector changes not given */
};

} // namespace dmd

#endif
