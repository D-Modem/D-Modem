/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    usb_line.h - the modem's line a Smart Link SmartUSB56 (ST7554, USB
 *                 0483:7554 with an STLC7550 codec) or a pico-serial Line
 *                 stick (USB 1209:0002, a Si3056 DAA behind an RP2040)
 *                 through libusb: no kernel driver
 *
 *    The ST7554 is a USB bridge to a codec and a DAA: its registers are
 *    vendor control requests, its samples isochronous transfers both
 *    ways (S16 little-endian, mono, at 8000, 9600 or 16000 Hz), its
 *    GPIOs the hook (an output) and the ring detector (an input, whose
 *    changes come on its interrupt endpoint).  The samples run both ways
 *    from open() on; each IN transfer's byte count is the next OUT
 *    transfer's, so the modem's samples go out at the codec's own clock.
 *    Each ring signal's change on the ring detector is a pulse to the
 *    modem's ring().  On hook the codec hears the line only with GPIO 5
 *    (CID1) set: while the modem listens for caller ID
 *    (MODEM_HOOK_SNOOPING, AT#CID=1, from the first ring).  The codec is
 *    an STLC7550 or an ST75951/2 silicon DAA (GPIO_STATUS bit 14), whose
 *    start-up differs (docs/smartusb56-windows.md).  The modem dials
 *    itself, in DTMF.  Time is the codec's: the samples it has given.
 *
 *    The pico-serial Line stick (github.com/Neo-Desktop/pico-serial,
 *    SPEC-L.md and firmware/PROTOCOL.md "Line stick") is shaped the same
 *    way on purpose: its samples isochronous both ways in alternate
 *    setting 1 (S16 little-endian, mono, at any of the Si3056's rates,
 *    9600 Hz by default), its events on an interrupt endpoint (the ring
 *    detector's changes counted, a pulse each to ring()), its hook, hook
 *    flash and line monitor vendor requests.  It hears the line on hook
 *    with its monitor on, so caller ID comes in the samples as on an ALSA
 *    codec.  The country's line settings (termination, current limit,
 *    ringer, on-hook speed: si3056.h) go into its DAA's registers.
 *
 */
#ifndef DMODEMD_USB_LINE_H
#define DMODEMD_USB_LINE_H

#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#ifdef _WIN32
/* (Winsock 2 before libusb.h's windows.h, which would take Winsock 1) */
#include <winsock2.h>
#endif
#include <libusb.h>

#include <dcore.h>

#include "dte.h"
#include "run_loop.h"
#include "si3056.h"

namespace dmd {

class UsbLine final : public dcore::Host {
public:
	/* the line of dte's modem (a pty, or on Windows any of its DTEs),
	   not yet open */
	explicit UsbLine(Dte &dte) : dte_(dte) {}
	~UsbLine();
	UsbLine(const UsbLine &) = delete;
	UsbLine &operator=(const UsbLine &) = delete;

	/* a SmartUSB56's LEDs, GPIO 6 (carrier: the call online) and 13
	   (data: some has passed in the call), lit, as Smart Link's
	   Windows driver lights them with Options.ExtraLeds; both off on
	   hook.  Not every board has them: off by default */
	void set_leds(bool on) { leds_ = on; }
	/* a ring wakes the computer: the SmartUSB56's USB remote wakeup
	   armed (SET_FEATURE) at open() and after a resume, cleared at
	   the end; on Linux the kernel's power/wakeup of the device enabled
	   too (as root).  The ring detector's interrupt stays enabled on
	   hook, as always */
	void set_wake(bool on) { wake_ = on; }
	/* the SmartUSB56's handset detector (GPIO 2, its interrupt
	   enabled): a phone on its PHONE jack off hook (the GPIO set) or on
	   hook, to the modem's local_phone() */
	void set_handset(bool on) { handset_ = on; }
	/* the modem dev ("" or "auto": the first SmartUSB56 or pico-serial
	   Line; <bus>:<address> one of them, as lsusb shows them) opened at
	   rate, its samples running, on hook; false with the reason on
	   stderr */
	bool open(const char *dev, unsigned rate);
	/* libusb's descriptors watched this turn (POSIX: Windows' libusb
	   has none, and the caller polls every POLL_MS instead) */
	void watch(RunLoop &loop);
	enum { POLL_MS = 2 };
	/* libusb's events (the transfers done), then the samples that have
	   come, a frame (20 ms) at a time, through the modem; 0, or -1 if
	   the modem has gone (unplugged) */
	int pump(dcore::Modem &m);

	/* Host */
	int start(dcore::Modem &) override { running_ = true; return 0; }
	/* the samples no longer through the modem */
	int stop(dcore::Modem &) override { running_ = false; return 0; }
	int set_hook(dcore::Modem &m, int state) override;
	/* the local phone sensed (set_handset(), a SmartUSB56) */
	bool senses_local_phone(dcore::Modem &) override {
		return handset_ && kind_ == Kind::St7554;
	}
	/* an analogue line, the modem dialling it itself */
	bool analog_line(dcore::Modem &) override { return true; }
	/* a hook flash it can make (hook_flash()) */
	bool can_flash(dcore::Modem &) override { return true; }
	void hook_flash(dcore::Modem &m, unsigned ms) override;
	int pulse_hook(dcore::Modem &m, bool broken) override;
	/* the playback's lead, in samples */
	int io_delay(dcore::Modem &) override { return delay_; }
	/* the country: the stick's line settings (si3056.h), written now
	   on hook, else once the line is on hook again */
	void country(dcore::Modem &m, const char *name) override;
	/* for ATI3 */
	const char *driver_name() override {
		return kind_ == Kind::Stick ? "pico-serial Line (Si3056)" : "ST7554";
	}
	/* the modem's characters to the DTE: how many it took (data, for
	   the data LED, once the call has been seen online: not its
	   CONNECT) */
	int dte_write(dcore::Modem &m, const char *buf, int n) override {
		if (n > 0 && online_seen_ &&
		    m.link_state() == dcore::LinkState::Online)
			data_seen_ = true;
		return dte_.write(buf, n);
	}
	/* the clock: the device's samples so far, in ms */
	unsigned long now_ms() override {
		return (unsigned long)(samples_ * 1000 / rate_);
	}

	/* "<bus>:<address>" into bus and addr; false if it is not that */
	static bool parse_dev(const char *dev, int *bus, int *addr);
	/* --usb list: each SmartUSB56 and pico-serial Line on stdout, its
	   <bus>:<address> and whether it can be opened (on Windows: whether
	   WinUSB is its driver); 0, or -1 if libusb can't start */
	static int list();

private:
	enum { ISO_TRANSFERS = 3, ISO_PACKETS = 5 };
	/* the two devices it drives */
	enum class Kind { St7554, Stick };

	bool find(const char *dev);
	bool endpoints();
	bool set_reg(uint8_t reg, uint16_t v);
	bool get_reg(uint8_t reg, uint16_t *v);
	bool set_gpio(uint16_t v);
	/* the ST7554's start-up (its registers), and the stick's (its rate,
	   the line side checked) */
	bool open_st7554(unsigned rate);
	bool start_st75951();
	bool init_st7554_regs();
	void arm_wake(bool on, bool kernel = false);
	void park(libusb_transfer *t, int err);
	void resume();
	bool open_stick(unsigned rate);
	/* a vendor request to the stick's interface: OUT with value, or IN
	   into buf (len octets, all of them wanted); false (logged) */
	bool stick_out(uint8_t req, uint16_t value);
	bool stick_in(uint8_t req, uint16_t value, uint8_t *buf, uint16_t len);
	/* an event from the stick's interrupt endpoint */
	void stick_event(const uint8_t *b, int len);
	/* the country's settings into the stick's DAA (each register read,
	   its own bits changed); false (logged) */
	bool stick_country();
	bool start_streams();
	void close();

	static void LIBUSB_CALL in_done(libusb_transfer *t);
	static void LIBUSB_CALL out_done(libusb_transfer *t);
	static void LIBUSB_CALL intr_done(libusb_transfer *t);
	void in_complete(libusb_transfer *t);
	void out_fill(libusb_transfer *t, const int *lengths);
	void send_out();
	void gone(const char *why);
	void update_leds(dcore::Modem &m);

	Dte &dte_;
	Kind kind_ = Kind::St7554;
	const char *name_ = "SmartUSB56";   /* in the messages */
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
	uint8_t intr_buf_[8] = {};      /* the ST7554's 4 octets, the stick's 8 */
	int intr_len_ = 4;
	int fifo_samples_ = 0;          /* the device's own delay */
	/* the stick: its ring detector's changes (mod 256) so far, its
	   errors last seen */
	uint8_t ring_edges_ = 0;
	uint16_t underruns_ = 0, overruns_ = 0;
	/* its line side a Si3019 (else a Si3018); the country's settings,
	   its name, and whether they are still to be written */
	bool si3019_ = false;
	const Si3056Country *daa_ = &si3056_usa();
	std::string daa_name_ = "USA";
	bool daa_due_ = true;
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
	bool st75951_ = false;          /* the ST7554's codec an ST75951/2 */
	bool leds_ = false;             /* set_leds() */
	bool data_seen_ = false;        /* data in this call (the data LED) */
	bool online_seen_ = false;      /* the call online at a pump's end */
	bool wake_ = false;             /* set_wake() */
	bool handset_ = false;          /* set_handset() */
	bool handset_due_ = false;      /* GPIO 2 changed: to be read */
	/* transfers that could not be submitted again (the device
	   suspended): retried every RESUME_MS by resume(), the time of the
	   first (ms of the system's clock) */
	std::vector<libusb_transfer *> parked_;
	unsigned long long parked_since_ = 0, resume_tried_ = 0;
	enum { RESUME_MS = 1000, RESUME_GIVE_UP_MS = 30000 };
	unsigned long long flash_end_ = 0;
	unsigned rings_ = 0;            /* ring detector changes not given */
};

} // namespace dmd

#endif
