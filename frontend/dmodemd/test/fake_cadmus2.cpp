/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_cadmus2.cpp - a Conexant USB HSF modem (the Cadmus 2 bridge) for
 *                       the tests: the part of libusb-1.0 that
 *                       usb_line.cpp and usb_cadmus2.cpp use (LD_PRELOAD
 *                       in front of the real one), with one modem on the
 *                       bus whose telephone line is line_exchange's
 *
 *    The modem is as usb_cadmus2.cpp has it: USB 0572:1300; interface 0
 *    the data's (bulk 0x81 IN and 0x01 OUT, the samples; 0x83 and 0x03,
 *    not used), interface 1 the notifications' (interrupt 0x82).  Its
 *    vendor requests: GET_INFORMATION (0: 5 octets, the third 1 with no
 *    firmware, 3 once it runs some), UPLOAD_FIRMWARE (1: wValue the
 *    firmware's size, wIndex the block's offset; only while unconfigured,
 *    the blocks in order) and CONTROL_SCRIPT (2: the script run by the
 *    harness's bridge, dsp/test/hw/cadmus2.hpp, in slot wIndex; only
 *    with its firmware, configured).  The scripts' reports are its
 *    notifications.
 *
 *    FAKE_CD2_SOCKET is the exchange's socket of the line, connected to
 *    as fake_modem_card.cpp does ('C' the line's samples, 'P' the
 *    modem's, 'K' the hook, and the line monitor: on, for the ring
 *    detector).  The hook is the codec's word aaXX: bit
 *    0x20 clear off hook (aac8), set on (aae8).  The bridge's time is
 *    the line's: a ms each 16 of its samples (16000 Hz).  Its samples
 *    flow while the bridge's streams run (op 44, script 5's: 45 stops
 *    them), 64 an IN transfer, the line's off hook and silence on hook;
 *    an OUT transfer's go to the exchange at once.  While the line
 *    rings (on hook) the bridge's channel 4 comes every 40 ms (the
 *    monitor's 08 01).
 *
 *    FAKE_CD2_LOADED=1: its firmware already runs.  FAKE_CD2_SMART=1: a
 *    SmartDAA board (B[0x20] bit 1).  FAKE_CD2_LOG=1: the requests on
 *    stderr (the firmware's size once it runs, the codec's words).
 *
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_net.h"

#include <deque>
#include <vector>

#include "fake_libusb.h"
#include "../../../dsp/test/hw/cadmus2.hpp"

namespace {

const uint8_t EP_IN = 0x81, EP_OUT = 0x01, EP_DCP = 0x83, EP_DL = 0x03,
	EP_NOTIFY = 0x82;
const uint16_t MAX_PACKET = 64, NOTIFY_PACKET = 64;
const uint8_t IF_DATA = 0, IF_COMM = 1, N_IF = 2;
const unsigned RATE = 16000, PER_MS = RATE / 1000;
const unsigned FRAME = 64;
const int RING_LEVEL = 8000;
/* the ring's end: no ring signal for this many of the line's samples */
const unsigned RING_QUIET = 3000;
/* channel 4 while it rings, ms */
const unsigned RING_NOTE_MS = 40;
/* its vendor requests */
enum { GET_INFORMATION = 0, UPLOAD_FIRMWARE = 1, CONTROL_SCRIPT = 2 };

struct Fake {
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	bool claimed[N_IF] = {};
	int config = 1;
	bool loaded = false;
	std::vector<uint8_t> fw;
	half::Cadmus2 *bridge = nullptr;
	size_t words = 0;               /* the codec's words seen */
	bool hook = false;              /* off hook (the codec's word) */
	bool line_hook = false;         /* what the exchange has */
	bool ringing = false;
	unsigned long long last_ring = 0;
	unsigned long long heard = 0;
	std::deque<int16_t> in;
	int half = -1;
	bool gone = false;
	std::deque<libusb_transfer *> in_q, out_q, intr_q, cancelled;
	int log = 0;
} fake;

/* switch which (0 the hook, 1 the line monitor) to the exchange
   (the device gone if it can't) */
void switch_to(uint8_t which, bool on)
{
	unsigned char m[2] = { which, (unsigned char)on };
	if (fake.ctl >= 0 && !fake_net::send_all(fake.ctl, m, 2))
		fake.gone = true;
}

/* the exchange's hook as off: told only when it changes */
void line_hook(bool off)
{
	if (off != fake.line_hook) {
		fake.line_hook = off;
		switch_to(0, off);
	}
}

/* the codec's words the bridge has written since: the hook's */
void words()
{
	auto &w = fake.bridge->codec_words;
	for (; fake.words < w.size(); fake.words++) {
		/* (the bridge's B[0x1a], B[0x1b]: the script's two octets
		   the other way round) */
		unsigned x = w[fake.words];
		unsigned word = (x & 0xff) << 8 | x >> 8;
		if (fake.log)
			fprintf(stderr, "fake Cadmus 2: codec word %04x\n", word);
		if ((word & 0xff00) == 0xaa00) {
			fake.hook = !(word & 0x20);
			if (fake.hook)
				fake.ringing = false;
			line_hook(fake.hook);
		}
	}
}

/* a ms of the bridge's time */
void tick()
{
	fake.bridge->tick();
	words();
}

/* the exchange's samples that have come: the bridge's time, the ring
   detector, the IN stream's */
void take()
{
	unsigned char b[4096];
	long n;

	if (fake.cap < 0)
		return;
	while ((n = fake_net::recv_now(fake.cap, b, sizeof(b))) > 0) {
		for (long i = 0; i < n; i++) {
			if (fake.half < 0) {
				fake.half = b[i];
				continue;
			}
			int16_t s = (int16_t)(fake.half | b[i] << 8);
			fake.half = -1;
			fake.heard++;
			bool was = fake.ringing;
			if ((s > RING_LEVEL || s < -RING_LEVEL) && !fake.hook) {
				fake.last_ring = fake.heard;
				fake.ringing = true;
			} else if (fake.ringing &&
				   (fake.hook || fake.heard - fake.last_ring > RING_QUIET))
				fake.ringing = false;
			if (fake.log && was != fake.ringing)
				fprintf(stderr, "fake Cadmus 2: %s\n", fake.ringing ?
					"the line rings" : "the line stopped ringing");
			if (fake.heard % PER_MS == 0) {
				if (fake.ringing &&
				    fake.heard % (PER_MS * RING_NOTE_MS) == 0)
					fake.bridge->raise(4);
				if (fake.loaded)
					tick();
			}
			if (fake.bridge->streams)
				fake.in.push_back(fake.hook ? s : 0);
		}
	}
	if (n == 0)
		fake.gone = true;
}

/* t done with status s: its callback */
void done(libusb_transfer *t, libusb_transfer_status s)
{
	t->status = s;
	t->callback(t);
}

/* a notification for t (one of the bridge's) */
void note_to(libusb_transfer *t)
{
	std::vector<uint8_t> b = fake.bridge->notes.front();
	fake.bridge->notes.pop_front();
	int n = (int)b.size() < t->length ? (int)b.size() : t->length;
	memcpy(t->buffer, b.data(), (size_t)n);
	t->actual_length = n;
}

/* what the device has done since: the transfers done, their callbacks
   (which may submit more, done in the same turn) */
void run()
{
	take();
	for (bool more = true; more;) {
		more = false;
		while (!fake.cancelled.empty()) {
			libusb_transfer *t = fake.cancelled.front();
			fake.cancelled.pop_front();
			done(t, LIBUSB_TRANSFER_CANCELLED);
			more = true;
		}
		if (fake.gone) {
			std::deque<libusb_transfer *> all;
			for (auto *q : { &fake.in_q, &fake.out_q, &fake.intr_q }) {
				all.insert(all.end(), q->begin(), q->end());
				q->clear();
			}
			for (libusb_transfer *t : all)
				done(t, LIBUSB_TRANSFER_NO_DEVICE);
			return;
		}
		while (!fake.out_q.empty()) {
			libusb_transfer *t = fake.out_q.front();
			fake.out_q.pop_front();
			if (fake.play >= 0 && t->length > 0 &&
			    !fake_net::send_all(fake.play, t->buffer, (size_t)t->length))
				fake.gone = true;
			t->actual_length = t->length;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (!fake.bridge->notes.empty() && !fake.intr_q.empty()) {
			libusb_transfer *t = fake.intr_q.front();
			fake.intr_q.pop_front();
			note_to(t);
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (!fake.in_q.empty() && fake.in.size() >= FRAME) {
			libusb_transfer *t = fake.in_q.front();
			fake.in_q.pop_front();
			unsigned n = FRAME;
			if (n * 2 > (unsigned)t->length)
				n = (unsigned)t->length / 2;
			for (unsigned k = 0; k < n; k++) {
				int16_t s = fake.in.front();
				fake.in.pop_front();
				t->buffer[2 * k] = (uint8_t)s;
				t->buffer[2 * k + 1] = (uint8_t)(s >> 8);
			}
			t->actual_length = (int)(2 * n);
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
	}
}

/* the configuration's descriptors */
libusb_endpoint_descriptor eps_data[4], ep_notify;
libusb_interface_descriptor alts[N_IF];
libusb_interface ifaces[N_IF];
libusb_config_descriptor config;

/* the configuration's descriptors filled in */
void make_config()
{
	const uint8_t addr[4] = { EP_IN, EP_OUT, EP_DCP, EP_DL };
	for (int i = 0; i < 4; i++) {
		eps_data[i].bEndpointAddress = addr[i];
		eps_data[i].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
		eps_data[i].wMaxPacketSize = MAX_PACKET;
	}
	ep_notify.bEndpointAddress = EP_NOTIFY;
	ep_notify.bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
	ep_notify.wMaxPacketSize = NOTIFY_PACKET;
	ep_notify.bInterval = 1;
	memset(alts, 0, sizeof(alts));
	for (uint8_t i = 0; i < N_IF; i++) {
		alts[i].bInterfaceNumber = i;
		alts[i].bInterfaceClass = 0xff;
		ifaces[i].altsetting = &alts[i];
		ifaces[i].num_altsetting = 1;
	}
	alts[IF_DATA].bNumEndpoints = 4;
	alts[IF_DATA].endpoint = eps_data;
	alts[IF_COMM].bNumEndpoints = 1;
	alts[IF_COMM].endpoint = &ep_notify;
	config.bConfigurationValue = 1;
	config.bNumInterfaces = N_IF;
	config.interface = ifaces;
}

/* the device on libusb's bus (fake_libusb.h) */
struct Cd2 final : fake_usb::Device {
	/* its log on stderr with FAKE_CD2_LOG */
	Cd2() : Device("FAKE_CD2_LOG") {}

	/* the settings from the environment: the log's */
	void init() override
	{
		fake.log = log;
	}

	/* the modem's descriptor (0572:1300, USB 1.1) */
	void descriptor(libusb_device_descriptor *d) override
	{
		d->bLength = LIBUSB_DT_DEVICE_SIZE;
		d->bDescriptorType = LIBUSB_DT_DEVICE;
		d->bcdUSB = 0x0110;
		d->idVendor = 0x0572;
		d->idProduct = 0x1300;
		d->bNumConfigurations = 1;
	}

	/* address 8 */
	uint8_t address() const override
	{
		return 8;
	}

	/* the device opened: its connections to the exchange at
	   FAKE_CD2_SOCKET (LIBUSB_ERROR_BUSY if open, _ACCESS if no
	   socket, _IO if no exchange); a bridge anew */
	int open() override
	{
		const char *path = getenv("FAKE_CD2_SOCKET");

		if (fake.open)
			return LIBUSB_ERROR_BUSY;
		if (!path)
			return LIBUSB_ERROR_ACCESS;
		fake.cap = fake_net::connect_exchange(path, 'C');
		fake.play = fake_net::connect_exchange(path, 'P');
		fake.ctl = fake_net::connect_exchange(path, 'K');
		if (fake.cap < 0 || fake.play < 0 || fake.ctl < 0) {
			fprintf(stderr, "fake Cadmus 2: no exchange at %s\n", path);
			for (int fd : { fake.cap, fake.play, fake.ctl })
				fake_net::close_fd(fd);
			fake.cap = fake.play = fake.ctl = -1;
			return LIBUSB_ERROR_IO;
		}
		delete fake.bridge;
		fake.bridge = new half::Cadmus2();
		fake.bridge->asked_comes = false;
		fake.bridge->dbg = fake.log;
		if (getenv("FAKE_CD2_SMART"))
			fake.bridge->B[0x20] |= 2;
		fake.words = 0;
		fake.loaded = getenv("FAKE_CD2_LOADED") != nullptr;
		fake.fw.clear();
		fake.config = 1;
		fake.hook = fake.line_hook = fake.ringing = false;
		/* (its ring detector hears the line on hook: the exchange's
		   monitor on, the samples silence) */
		switch_to(1, true);
		fake.open = true;
		return 0;
	}

	/* the connections closed, the device free again */
	void close() override
	{
		for (int fd : { fake.cap, fake.play, fake.ctl })
			fake_net::close_fd(fd);
		fake.cap = fake.play = fake.ctl = -1;
		fake.open = false;
	}

	/* interface n claimed (configured; LIBUSB_ERROR_BUSY if it is) */
	int claim(int n) override
	{
		if (n < 0 || n >= N_IF || fake.config != 1)
			return LIBUSB_ERROR_NOT_FOUND;
		if (fake.claimed[n])
			return LIBUSB_ERROR_BUSY;
		fake.claimed[n] = true;
		return 0;
	}

	/* interface n released */
	int release(int n) override
	{
		if (n < 0 || n >= N_IF || !fake.claimed[n])
			return LIBUSB_ERROR_NOT_FOUND;
		fake.claimed[n] = false;
		return 0;
	}

	/* (alternate setting 0 alone) */
	int set_alt(int n, int alt) override
	{
		return n >= 0 && n < N_IF && alt == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
	}

	/* configuration 1, or none (-1, 0): not with an interface
	   claimed */
	int set_config(int c) override
	{
		if (fake.log)
			fprintf(stderr, "fake Cadmus 2: configuration %d\n", c);
		if (c != 1 && c != 0 && c != -1)
			return LIBUSB_ERROR_NOT_FOUND;
		for (bool cl : fake.claimed)
			if (cl)
				return LIBUSB_ERROR_BUSY;
		fake.config = c == 1 ? 1 : 0;
		return 0;
	}

	/* the configuration (make_config()), static */
	libusb_config_descriptor *config() override
	{
		make_config();
		return &::config;
	}

	/* the vendor requests (see the top); LIBUSB_ERROR_PIPE for the rest,
	   LIBUSB_ERROR_NO_DEVICE once gone */
	int control(uint8_t type, uint8_t request, uint16_t value,
		    uint16_t index, unsigned char *data,
		    uint16_t length) override
	{
		int r = LIBUSB_ERROR_PIPE;
		bool in = type & LIBUSB_ENDPOINT_IN;

		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		if ((type & 0x60) != LIBUSB_REQUEST_TYPE_VENDOR)
			;
		else if (in && request == GET_INFORMATION && length >= 4) {
			uint8_t info[5] = { 0x00, 0x01, (uint8_t)(fake.loaded ? 3 : 1), 3, 0 };
			r = length < 5 ? length : 5;
			memcpy(data, info, (size_t)r);
		} else if (!in && request == UPLOAD_FIRMWARE && !fake.loaded &&
			   fake.config == 0 && index == fake.fw.size() &&
			   length && length <= 64 && index + length <= value) {
			fake.fw.insert(fake.fw.end(), data, data + length);
			if (fake.fw.size() == value) {
				fake.loaded = true;
				if (fake.log)
					fprintf(stderr, "fake Cadmus 2: firmware of %zu "
						"octets runs\n", fake.fw.size());
			}
			r = length;
		} else if (!in && request == CONTROL_SCRIPT && fake.loaded &&
			   fake.config == 1 && (value == 0xff01 || value == 0xff02)) {
			fake.bridge->script(value, index,
					    std::vector<uint8_t>(data, data + length));
			words();
			r = length;
		}
		if (fake.log)
			fprintf(stderr, "fake Cadmus 2: request %02x %02x %04x %u (%u): %d\n",
				type, request, value, index, length, r);
		return r;
	}

	/* a notification (the oldest), the bridge's time run on for one
	   (200 ms at most, without the line's samples) if none has come:
	   LIBUSB_ERROR_TIMEOUT if none */
	int interrupt(unsigned char endpoint, unsigned char *data,
		      int length, int *transferred) override
	{
		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		if (endpoint != EP_NOTIFY || !fake.claimed[IF_COMM])
			return LIBUSB_ERROR_PIPE;
		take();
		for (int i = 0; i < 200 && fake.bridge->notes.empty(); i++)
			tick();
		if (fake.bridge->notes.empty())
			return LIBUSB_ERROR_TIMEOUT;
		std::vector<uint8_t> b = fake.bridge->notes.front();
		fake.bridge->notes.pop_front();
		int n = (int)b.size() < length ? (int)b.size() : length;
		memcpy(data, b.data(), (size_t)n);
		if (transferred)
			*transferred = n;
		return 0;
	}

	/* t queued at its endpoint, done in run() */
	int submit(libusb_transfer *t) override
	{
		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		if (t->endpoint == EP_NOTIFY && fake.claimed[IF_COMM])
			fake.intr_q.push_back(t);
		else if (t->endpoint == EP_IN && fake.claimed[IF_DATA])
			fake.in_q.push_back(t);
		else if (t->endpoint == EP_OUT && fake.claimed[IF_DATA])
			fake.out_q.push_back(t);
		else
			return LIBUSB_ERROR_NOT_FOUND;
		return 0;
	}

	/* t off its queue, done as cancelled in run() */
	int cancel(libusb_transfer *t) override
	{
		return fake_usb::cancel_from(t, { &fake.in_q, &fake.out_q,
						  &fake.intr_q }, fake.cancelled);
	}

	/* the descriptor to poll: the capture socket (-1 if not open) */
	int poll_fd() const override { return fake.cap; }

	/* what the device has done (run()) */
	void run() override { ::run(); }
};

} // namespace

/* see fake_libusb.h */
fake_usb::Device &fake_usb::device()
{
	static Cd2 d;
	return d;
}
