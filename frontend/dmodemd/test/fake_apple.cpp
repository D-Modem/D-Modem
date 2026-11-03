/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_apple.cpp - an Apple USB Modem for the tests: the part of
 *                     libusb-1.0 that usb_line.cpp and usb_apple.cpp use
 *                     (LD_PRELOAD in front of the real one), with one
 *                     modem on the bus whose telephone line is
 *                     line_exchange's
 *
 *    The modem is as usb_apple.cpp has it (docs/apple-usb-modem-details.md): USB
 *    05AC:1401; interface 0 CDC's Direct Line Control with an interrupt
 *    IN endpoint (0x81) of 8-octet notifications, 1 the vendor interface
 *    of the DAA's commands, 2 audio control, 3 and 4 audio streaming OUT
 *    (0x02) and IN (0x83) in alternate setting 1 (16-bit mono, the rates
 *    of a format type I descriptor), 5 DFU.
 *
 *    FAKE_APPLE_SOCKET is the exchange's socket of the line.  Opening the
 *    device connects to it as fake_modem_card.cpp does ('C' the line's
 *    samples, 'P' the modem's, 'K' the switches: SET_HOOK_STATE is the
 *    hook, the Si3056's register 5 bit ONHM the line monitor).  The IN
 *    stream flows in interface 4's alternate setting 1: each IN transfer
 *    done takes 1 ms of the line's samples a packet at the rate SET_CUR
 *    set, as they come from the exchange; heard off hook, or on hook with
 *    the monitor on (silence otherwise, as the Si3056 on hook); an OUT
 *    transfer's samples go to the exchange, and it is done, at once.  The
 *    ring detector says when the line starts ringing and when it stops
 *    (RING_DETECT, wValue bit 15), on hook.  A DAA read's response is
 *    notified (0x02, wValue 1) and taken with GET_ENCAPSULATED_RESPONSE.
 *    The phone cord is said plugged in (0x29, 5) when the device is
 *    opened.  The capture socket is libusb's descriptor to poll.  The
 *    device is gone (LIBUSB_TRANSFER_NO_DEVICE) when the exchange is.
 *
 *    FAKE_APPLE_NOLINK=1: the line side does not answer (register 12's
 *    FDT clear).
 *    FAKE_APPLE_LOG=1: the requests on stderr.
 *
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_net.h"

#include <array>
#include <deque>
#include <vector>

#include <libusb.h>

struct libusb_context {
	int dummy;
};

struct libusb_device {
	int dummy;
};

struct libusb_device_handle {
	libusb_device *dev;
};

namespace {

const uint8_t EP_NOTIFY = 0x81, EP_OUT = 0x02, EP_IN = 0x83;
const uint16_t MAX_PACKET = 40;
const int RING_LEVEL = 8000;
/* the ring's end: no ring signal for this many of the line's samples */
const unsigned RING_QUIET = 1500;

/* (2 audio control, 5 DFU: nothing of theirs is asked) */
const uint8_t IF_DLC = 0, IF_MGMT = 1, IF_OUT = 3, IF_IN = 4, N_IF = 6;

/* CDC's requests and notifications */
enum {
	SEND_ENCAPSULATED_COMMAND = 0x00, GET_ENCAPSULATED_RESPONSE = 0x01,
	SET_COMM_FEATURE = 0x02, SET_HOOK_STATE = 0x11,
};
const uint8_t N_RESPONSE = 0x02, N_RING_DETECT = 0x03, N_LINE = 0x29;
/* the audio class's SET_CUR */
const uint8_t SET_CUR = 0x01;

const unsigned RATES[] = { 7200, 8000, 8229, 8400, 9000, 9600, 16000 };

libusb_device the_device;

struct Fake {
	libusb_context *ctx = nullptr;
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	unsigned rate = 9600;
	int alt[N_IF] = {};
	bool claimed[N_IF] = {};
	bool hook = false;              /* off hook (SET_HOOK_STATE) */
	bool line_hook = false;         /* what the exchange has */
	bool monitor = false;           /* register 5's ONHM */
	bool ringing = false;
	unsigned long long last_ring = 0;   /* the line's sample of the last ring signal */
	unsigned long long heard = 0;       /* the line's samples heard */
	uint8_t regs[128] = {};
	uint8_t response = 0;
	bool response_due = false;
	std::deque<std::array<uint8_t, 8>> notes;
	bool gone = false;
	bool nolink = false;
	std::deque<int16_t> in;         /* the line's samples, not yet taken */
	int half = -1;
	unsigned long long ms = 0;      /* IN packets given */
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

/* the exchange's hook as on: told only when it changes */
void line_hook(bool on)
{
	if (on != fake.line_hook) {
		fake.line_hook = on;
		switch_to(0, on);
	}
}

/* a notification queued for the interrupt endpoint: code, its value,
   on interface 0 */
void notify(uint8_t code, uint16_t value)
{
	fake.notes.push_back({ 0xa1, code, (uint8_t)value, (uint8_t)(value >> 8),
			       IF_DLC, 0, 0, 0 });
}

/* the line monitor as register 5 has it, told to the exchange */
void monitor_from_regs()
{
	bool on = fake.regs[5] & 0x08;
	if (on != fake.monitor) {
		fake.monitor = on;
		switch_to(1, on);
	}
}

/* the IN stream flows: interface 4's alternate setting 1 */
bool streaming()
{
	return fake.alt[IF_IN] == 1;
}

/* the exchange's samples that have come; the ring detector on them */
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
			if (s > RING_LEVEL && !fake.hook) {
				fake.last_ring = fake.heard;
				if (!fake.ringing) {
					fake.ringing = true;
					notify(N_RING_DETECT, 0x8000);
				}
			} else if (fake.ringing &&
				   (fake.hook || fake.heard - fake.last_ring > RING_QUIET)) {
				fake.ringing = false;
				notify(N_RING_DETECT, 0);
			}
			if (streaming())
				fake.in.push_back(fake.hook || fake.monitor ? s : 0);
		}
	}
	if (n == 0)
		fake.gone = true;
}

/* the samples of IN packet ms (1 ms each, at the rate) */
unsigned packet_samples(unsigned long long ms)
{
	return (unsigned)((ms + 1) * fake.rate / 1000 - ms * fake.rate / 1000);
}

/* t done with status s: its callback */
void done(libusb_transfer *t, libusb_transfer_status s)
{
	t->status = s;
	t->callback(t);
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
			int off = 0;
			for (int i = 0; i < t->num_iso_packets; i++) {
				libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
				if (fake.play >= 0 && p.length &&
				    !fake_net::send_all(fake.play, t->buffer + off,
							 p.length))
					fake.gone = true;
				p.actual_length = p.length;
				p.status = LIBUSB_TRANSFER_COMPLETED;
				off += (int)p.length;
			}
			t->actual_length = off;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (!fake.notes.empty() && !fake.intr_q.empty()) {
			libusb_transfer *t = fake.intr_q.front();
			fake.intr_q.pop_front();
			auto b = fake.notes.front();
			fake.notes.pop_front();
			int n = t->length < 8 ? t->length : 8;
			memcpy(t->buffer, b.data(), (size_t)n);
			t->actual_length = n;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (!fake.in_q.empty() && streaming()) {
			libusb_transfer *t = fake.in_q.front();
			unsigned need = 0;
			for (int i = 0; i < t->num_iso_packets; i++)
				need += packet_samples(fake.ms + (unsigned)i);
			if (fake.in.size() >= need) {
				fake.in_q.pop_front();
				int off = 0;
				for (int i = 0; i < t->num_iso_packets; i++) {
					libusb_iso_packet_descriptor &p =
						t->iso_packet_desc[i];
					unsigned n = packet_samples(fake.ms++);
					if (n * 2 > p.length)
						n = p.length / 2;
					for (unsigned k = 0; k < n; k++) {
						int16_t s = fake.in.front();
						fake.in.pop_front();
						t->buffer[off + 2 * k] = (uint8_t)s;
						t->buffer[off + 2 * k + 1] =
							(uint8_t)(s >> 8);
					}
					p.actual_length = n * 2;
					p.status = LIBUSB_TRANSFER_COMPLETED;
					off += (int)p.length;
				}
				t->actual_length = off;
				done(t, LIBUSB_TRANSFER_COMPLETED);
				more = true;
			}
		}
	}
}

/* the configuration's descriptors */
libusb_endpoint_descriptor ep_notify, ep_out, ep_in;
libusb_interface_descriptor alts[N_IF][2];
libusb_interface ifaces[N_IF];
libusb_config_descriptor config;
/* an audio streaming alternate setting's class-specific descriptors:
   AS_GENERAL, then FORMAT_TYPE I (1 channel, 2 octets, 16 bits, the
   rates) */
std::vector<uint8_t> as_extra;

/* the configuration's descriptors filled in */
void make_config()
{
	as_extra = { 7, 0x24, 0x01, 0, 1, 0x01, 0x00 };
	size_t n = sizeof(RATES) / sizeof(RATES[0]);
	as_extra.insert(as_extra.end(), { (uint8_t)(8 + 3 * n), 0x24, 0x02, 0x01,
					  1, 2, 16, (uint8_t)n });
	for (unsigned r : RATES)
		as_extra.insert(as_extra.end(), { (uint8_t)r, (uint8_t)(r >> 8),
						  (uint8_t)(r >> 16) });
	ep_notify.bEndpointAddress = EP_NOTIFY;
	ep_notify.bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
	ep_notify.wMaxPacketSize = 16;
	ep_notify.bInterval = 8;
	ep_out.bEndpointAddress = EP_OUT;
	ep_out.bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS | 0x04;   /* asynchronous */
	ep_out.wMaxPacketSize = MAX_PACKET;
	ep_out.bInterval = 1;
	ep_in.bEndpointAddress = EP_IN;
	ep_in.bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS | 0x04;
	ep_in.wMaxPacketSize = MAX_PACKET;
	ep_in.bInterval = 1;
	static const uint8_t cls[N_IF][3] = {
		{ 0x02, 0x03, 0x00 }, { 0xff, 0x02, 0xff }, { 0x01, 0x01, 0x00 },
		{ 0x01, 0x02, 0x00 }, { 0x01, 0x02, 0x00 }, { 0xfe, 0x01, 0x01 },
	};
	memset(alts, 0, sizeof(alts));
	for (uint8_t i = 0; i < N_IF; i++) {
		bool as = i == IF_OUT || i == IF_IN;
		for (int a = 0; a < (as ? 2 : 1); a++) {
			libusb_interface_descriptor &d = alts[i][a];
			d.bInterfaceNumber = i;
			d.bAlternateSetting = (uint8_t)a;
			d.bInterfaceClass = cls[i][0];
			d.bInterfaceSubClass = cls[i][1];
			d.bInterfaceProtocol = cls[i][2];
		}
		ifaces[i].altsetting = alts[i];
		ifaces[i].num_altsetting = as ? 2 : 1;
	}
	alts[IF_DLC][0].bNumEndpoints = 1;
	alts[IF_DLC][0].endpoint = &ep_notify;
	for (uint8_t i : { IF_OUT, IF_IN }) {
		alts[i][1].bNumEndpoints = 1;
		alts[i][1].endpoint = i == IF_OUT ? &ep_out : &ep_in;
		alts[i][1].extra = as_extra.data();
		alts[i][1].extra_length = (int)as_extra.size();
	}
	config.bNumInterfaces = N_IF;
	config.interface = ifaces;
}

/* the DAA's 3-octet command: 0 or LIBUSB_ERROR_PIPE */
int daa_command(const unsigned char *b)
{
	uint8_t cmd = b[0], reg = b[1], v = b[2];
	if (reg > 127)
		return LIBUSB_ERROR_PIPE;
	switch (cmd) {
	case 0x00: fake.regs[reg] = v; break;
	case 0x01: fake.regs[reg] |= v; break;
	case 0x02: fake.regs[reg] &= v; break;
	case 0x03: fake.regs[reg] ^= v; break;
	case 0x80:
		fake.response = fake.regs[reg];
		/* (its status as the device has it) */
		if (reg == 12)
			fake.response = (uint8_t)(fake.nolink ? 0 : 0x40);
		fake.response_due = true;
		notify(N_RESPONSE, 1);
		return 0;
	default:
		return LIBUSB_ERROR_PIPE;
	}
	if (reg == 5)
		monitor_from_regs();
	return 0;
}

} // namespace

extern "C" {

#define FAKE_USB_INTERRUPT_TRANSFER
#include "fake_usb_backend.h"

/* the context (FAKE_APPLE_LOG read) */
int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	fake.ctx = new libusb_context();
	fake.log = getenv("FAKE_APPLE_LOG") != nullptr;
	if (ctx)
		*ctx = fake.ctx;
	return 0;
}

/* the context freed */
void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	if (ctx == fake.ctx) {
		delete fake.ctx;
		fake.ctx = nullptr;
	}
}

/* libusb's text for the codes usb_line.cpp sees */
const char *LIBUSB_CALL libusb_strerror(int code)
{
	switch (code) {
	case LIBUSB_SUCCESS: return "Success";
	case LIBUSB_ERROR_IO: return "Input/Output Error";
	case LIBUSB_ERROR_ACCESS: return "Access denied (insufficient permissions)";
	case LIBUSB_ERROR_NO_DEVICE: return "No such device (it may have been disconnected)";
	case LIBUSB_ERROR_NOT_FOUND: return "Entity not found";
	case LIBUSB_ERROR_BUSY: return "Resource busy";
	case LIBUSB_ERROR_TIMEOUT: return "Operation timed out";
	case LIBUSB_ERROR_PIPE: return "Pipe error";
	case LIBUSB_ERROR_INVALID_PARAM: return "Invalid parameter";
	case LIBUSB_ERROR_NOT_SUPPORTED: return "Operation not supported or unimplemented on this platform";
	}
	return "Other error";
}

/* the bus: the one device */
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *,
					   libusb_device ***list)
{
	libusb_device **l = (libusb_device **)calloc(2, sizeof(*l));
	if (!l)
		return LIBUSB_ERROR_NO_MEM;
	l[0] = &the_device;
	*list = l;
	return 1;
}

/* the list freed */
void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int)
{
	free(list);
}

/* the modem's descriptor (05ac:1401, USB 1.1) */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *,
					     libusb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->bLength = LIBUSB_DT_DEVICE_SIZE;
	d->bDescriptorType = LIBUSB_DT_DEVICE;
	d->bcdUSB = 0x0110;
	d->idVendor = 0x05ac;
	d->idProduct = 0x1401;
	d->bNumConfigurations = 1;
	return 0;
}

/* bus 1 */
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *)
{
	return 1;
}

/* its one port on the bus */
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *, uint8_t *ports,
					int len)
{
	if (len < 1)
		return LIBUSB_ERROR_OVERFLOW;
	ports[0] = 250;
	return 1;
}

/* full speed */
int LIBUSB_CALL libusb_get_device_speed(libusb_device *)
{
	return LIBUSB_SPEED_FULL;
}

/* address 8 */
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *)
{
	return 8;
}

/* the device opened: its connections to the exchange at
   FAKE_APPLE_SOCKET (LIBUSB_ERROR_BUSY if open, _ACCESS if no socket,
   _IO if no exchange); the phone cord said plugged in */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	const char *path = getenv("FAKE_APPLE_SOCKET");

	if (fake.open)
		return LIBUSB_ERROR_BUSY;
	if (!path)
		return LIBUSB_ERROR_ACCESS;
	fake.cap = fake_net::connect_exchange(path, 'C');
	fake.play = fake_net::connect_exchange(path, 'P');
	fake.ctl = fake_net::connect_exchange(path, 'K');
	if (fake.cap < 0 || fake.play < 0 || fake.ctl < 0) {
		fprintf(stderr, "fake Apple USB Modem: no exchange at %s\n", path);
		for (int fd : { fake.cap, fake.play, fake.ctl })
			fake_net::close_fd(fd);
		fake.cap = fake.play = fake.ctl = -1;
		return LIBUSB_ERROR_IO;
	}
	fake.nolink = getenv("FAKE_APPLE_NOLINK") != nullptr;
	/* the Si3056 rev E, a Si3018 line side; DAA1's ONHM clear */
	memset(fake.regs, 0, sizeof(fake.regs));
	fake.regs[11] = 0x15;
	fake.notes.clear();
	notify(N_LINE, 5);
	fake.open = true;
	*h = new libusb_device_handle{ dev };
	return 0;
}

/* the connections closed, the device free again */
void LIBUSB_CALL libusb_close(libusb_device_handle *h)
{
	for (int fd : { fake.cap, fake.play, fake.ctl })
		fake_net::close_fd(fd);
	fake.cap = fake.play = fake.ctl = -1;
	fake.open = false;
	delete h;
}

/* h's device */
libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *h)
{
	return h->dev;
}

/* no kernel driver to detach: 0 */
int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *,
						     int)
{
	return 0;
}

/* interface n claimed (LIBUSB_ERROR_BUSY if it is) */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int n)
{
	if (n < 0 || n >= N_IF)
		return LIBUSB_ERROR_NOT_FOUND;
	if (fake.claimed[n])
		return LIBUSB_ERROR_BUSY;
	fake.claimed[n] = true;
	if (fake.log)
		fprintf(stderr, "fake Apple USB Modem: interface %d claimed\n", n);
	return 0;
}

/* interface n released */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int n)
{
	if (n < 0 || n >= N_IF || !fake.claimed[n])
		return LIBUSB_ERROR_NOT_FOUND;
	fake.claimed[n] = false;
	fake.alt[n] = 0;
	return 0;
}

/* a streaming interface's (3, 4) setting alt, 0 or 1 (claimed): 4's 1
   starts the IN samples */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *,
						 int n, int alt)
{
	if (fake.log)
		fprintf(stderr, "fake Apple USB Modem: interface %d's alternate setting %d\n",
			n, alt);
	if (n < 0 || n >= N_IF || !fake.claimed[n] ||
	    (alt != 0 && (alt != 1 || (n != IF_OUT && n != IF_IN))))
		return LIBUSB_ERROR_NOT_FOUND;
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	fake.alt[n] = alt;
	if (n == IF_IN) {
		fake.in.clear();
		fake.ms = 0;
	}
	return 0;
}

/* nothing to clear: 0 */
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *, unsigned char)
{
	return 0;
}

/* the configuration (make_config()), static */
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *,
						    libusb_config_descriptor **c)
{
	make_config();
	*c = &config;
	return 0;
}

/* the configuration by its index (the one: 0) */
int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *, uint8_t index,
					     libusb_config_descriptor **c)
{
	if (index != 0)
		return LIBUSB_ERROR_NOT_FOUND;
	make_config();
	*c = &config;
	return 0;
}

/* nothing to free (static) */
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor *)
{
}

/* the class requests: CDC's to interfaces 0 and 1 (the DAA's commands,
   their responses, the hook, the country), the audio class's SET_CUR
   of a streaming endpoint's rate; LIBUSB_ERROR_PIPE for the rest,
   LIBUSB_ERROR_NO_DEVICE once gone */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *,
					uint8_t type, uint8_t request,
					uint16_t value, uint16_t index,
					unsigned char *data, uint16_t length,
					unsigned int)
{
	int r = LIBUSB_ERROR_PIPE;
	bool in = type & LIBUSB_ENDPOINT_IN;
	uint8_t recipient = type & 0x1f;

	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if ((type & 0x60) != LIBUSB_REQUEST_TYPE_CLASS)
		;
	else if (recipient == LIBUSB_RECIPIENT_ENDPOINT && !in &&
		 request == SET_CUR && value == 0x0100 &&
		 (index == EP_OUT || index == EP_IN) && length == 3) {
		unsigned rate = data[0] | data[1] << 8 | data[2] << 16;
		for (unsigned x : RATES)
			if (x == rate) {
				if (index == EP_IN) {
					fake.rate = rate;
					fake.ms = 0;
				}
				r = 0;
			}
	} else if (recipient == LIBUSB_RECIPIENT_INTERFACE && index == IF_MGMT &&
		   fake.claimed[IF_MGMT]) {
		if (!in && request == SEND_ENCAPSULATED_COMMAND && length == 3)
			r = daa_command(data);
		else if (in && request == GET_ENCAPSULATED_RESPONSE && length >= 1) {
			data[0] = fake.response;
			if (length >= 2)
				data[1] = 0;
			fake.response_due = false;
			r = length >= 2 ? 2 : 1;
		}
	} else if (recipient == LIBUSB_RECIPIENT_INTERFACE && index == IF_DLC &&
		   !in && request == SET_HOOK_STATE && value <= 1) {
		fake.hook = value;
		if (fake.hook && fake.ringing) {
			fake.ringing = false;
			notify(N_RING_DETECT, 0);
		}
		line_hook(fake.hook);
		r = 0;
	}
	if (fake.log)
		fprintf(stderr, "fake Apple USB Modem: request %02x %02x %s %04x %u: %d\n",
			type, request, in ? "in" : "out", value, index, r);
	if (fake.log && !in && request == SEND_ENCAPSULATED_COMMAND && length == 3)
		fprintf(stderr, "fake Apple USB Modem: DAA %02x %u %02x\n",
			data[0], data[1], data[2]);
	return r;
}

/* a notification (the oldest queued), at once: LIBUSB_ERROR_TIMEOUT if
   none (the time not waited) */
int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *,
					  unsigned char endpoint,
					  unsigned char *data, int length,
					  int *transferred, unsigned int)
{
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (endpoint != EP_NOTIFY)
		return LIBUSB_ERROR_PIPE;
	take();
	if (fake.notes.empty())
		return LIBUSB_ERROR_TIMEOUT;
	auto b = fake.notes.front();
	fake.notes.pop_front();
	int n = length < 8 ? length : 8;
	memcpy(data, b.data(), (size_t)n);
	if (transferred)
		*transferred = n;
	return 0;
}

/* a transfer of iso_packets packets, zeroed */
libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	size_t n = sizeof(libusb_transfer) +
		sizeof(libusb_iso_packet_descriptor) * (size_t)iso_packets;
	libusb_transfer *t = (libusb_transfer *)calloc(1, n);
	if (t)
		t->num_iso_packets = iso_packets;
	return t;
}

/* t freed (its buffer too if it has
   LIBUSB_TRANSFER_FREE_BUFFER) */
void LIBUSB_CALL libusb_free_transfer(libusb_transfer *t)
{
	if (!t)
		return;
	if (t->flags & LIBUSB_TRANSFER_FREE_BUFFER)
		free(t->buffer);
	free(t);
}

/* t queued at its endpoint, done in run(); an audio endpoint only in its
   interface's alternate setting 1 */
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer *t)
{
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (fake_usb_refused(t))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	if (t->endpoint == EP_NOTIFY)
		fake.intr_q.push_back(t);
	else if (t->endpoint == EP_IN && fake.alt[IF_IN] == 1)
		fake.in_q.push_back(t);
	else if (t->endpoint == EP_OUT && fake.alt[IF_OUT] == 1)
		fake.out_q.push_back(t);
	else
		return LIBUSB_ERROR_NOT_FOUND;
	return 0;
}

/* t off its queue, done as cancelled in run();
   LIBUSB_ERROR_NOT_FOUND if not queued */
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer *t)
{
	for (auto *q : { &fake.in_q, &fake.out_q, &fake.intr_q })
		for (auto i = q->begin(); i != q->end(); ++i)
			if (*i == t) {
				q->erase(i);
				fake.cancelled.push_back(t);
				return 0;
			}
	return LIBUSB_ERROR_NOT_FOUND;
}

/* the descriptors to poll: the capture socket (if open) */
const libusb_pollfd **LIBUSB_CALL libusb_get_pollfds(libusb_context *)
{
	const libusb_pollfd **l =
		(const libusb_pollfd **)calloc(2, sizeof(*l));
	static libusb_pollfd p;
	if (!l)
		return nullptr;
	if (fake.cap >= 0) {
		p.fd = fake.cap;
		p.events = POLLIN;
		l[0] = &p;
	}
	return l;
}

/* the list freed */
void LIBUSB_CALL libusb_free_pollfds(const libusb_pollfd **l)
{
	free(l);
}

/* what the device has done (run()), without waiting */
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *,
						       struct timeval *tv,
						       int *)
{
	run();
	(void)tv;
	return 0;
}

} // extern "C"
