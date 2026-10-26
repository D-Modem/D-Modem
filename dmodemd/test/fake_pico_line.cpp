/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fake_pico_line.cpp - a pico-serial Line stick for the tests: the part
 *                         of libusb-1.0 that usb_line.cpp uses (LD_PRELOAD
 *                         in front of the real one), with one stick on the
 *                         bus whose telephone line is line_exchange's
 *
 *    The stick's protocol is its firmware's (pico-serial's
 *    firmware/PROTOCOL.md "Line stick"): USB 1209:0002, interface 0's
 *    alternate setting 1 with an isochronous OUT (0x01) and IN (0x82)
 *    endpoint and an interrupt IN one (0x83) of 8-octet events; vendor
 *    requests to the interface for the rest.
 *
 *    FAKE_PICO_LINE_SOCKET is the exchange's socket of the line.  Opening
 *    the device connects to it as fake_modem_card.cpp does ('C' the
 *    line's samples, 'P' the modem's, 'K' the switches: SET_HOOK is the
 *    hook, SET_MONITOR the line monitor).  The samples flow in alternate
 *    setting 1 once a rate is set: each IN transfer done takes 1 ms of the
 *    line's samples a packet, as they come from the exchange; heard off
 *    hook, or on hook with the monitor on (silence otherwise, as the
 *    Si3056 on hook); an OUT transfer's samples go to the exchange, and it
 *    is done, at once.  The ring detector (the RGDT pin) changes on each
 *    half cycle of the ring signal on hook, counted in an event on the
 *    interrupt endpoint.  HOOK_FLASH breaks the hook for its length of
 *    the line's samples.  The capture socket is libusb's descriptor to
 *    poll.  The device is gone (LIBUSB_TRANSFER_NO_DEVICE) when the
 *    exchange is.
 *
 *    FAKE_PICO_LINE_NOLINK=1: GET_INFO says the line side does not answer.
 *    FAKE_PICO_LINE_LOG=1: the requests on stderr.
 *    FAKE_USB_NO_ISO, FAKE_USB_USBDK: fake_usb_backend.h (Windows 7's
 *    and 8's drivers).
 *
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_net.h"

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

const uint8_t EP_OUT = 0x01, EP_IN = 0x82, EP_EVENT = 0x83;
const uint16_t MAX_PACKET = 40;
const int RING_LEVEL = 8000;

/* the requests and state bits (firmware/src/core/line_proto.h) */
enum {
	GET_INFO = 0x01, GET_STATUS = 0x02, SET_RATE = 0x10, SET_HOOK = 0x11,
	HOOK_FLASH = 0x12, SET_MONITOR = 0x13, DAA_READ = 0x20, DAA_WRITE = 0x21,
	RESET = 0x30, REBOOT_UF2 = 0x31,
};
const uint8_t ST_LINK = 0x01, ST_OFF_HOOK = 0x02, ST_MONITOR = 0x04,
	ST_STREAMING = 0x08, ST_RINGING = 0x10, ST_FLASH = 0x20;
const uint8_t INT_RING = 0x80;

libusb_device the_device;

struct Fake {
	libusb_context *ctx = nullptr;
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	unsigned rate = 9600;           /* the Si3056's after reset */
	int alt = 0;
	bool hook = false;              /* off hook (SET_HOOK) */
	bool line_hook = false;         /* what the exchange has (a flash breaks it) */
	bool monitor = false;
	unsigned long long flash_end = 0;   /* in IN packets (ms); 0: none */
	bool ring_state = false;
	uint8_t ring_edges = 0;
	uint8_t int_src = 0;
	uint8_t seq = 0;
	bool event_due = false;
	uint8_t regs[128] = {};
	bool gone = false;
	bool nolink = false;
	std::deque<int16_t> in;         /* the line's samples, not yet taken */
	int half = -1;
	unsigned long long ms = 0;      /* IN packets given */
	unsigned long long samples_in = 0, samples_out = 0;
	std::deque<libusb_transfer *> in_q, out_q, intr_q, cancelled;
	int log = 0;
} fake;

/* a connection to the exchange (fake_net.h) */
int connect_exchange(const char *path, char kind)
{
	return fake_net::connect_exchange(path, kind);
}

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

/* the stick's state bits */
uint8_t state()
{
	return (uint8_t)((fake.nolink ? 0 : ST_LINK) | (fake.hook ? ST_OFF_HOOK : 0) |
			 (fake.monitor ? ST_MONITOR : 0) | (fake.alt == 1 ? ST_STREAMING : 0) |
			 (fake.ring_state ? ST_RINGING : 0) | (fake.flash_end ? ST_FLASH : 0));
}

/* the samples flow: alternate setting 1 */
bool streaming()
{
	return fake.alt == 1;
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
			bool r = s > RING_LEVEL;
			if (r != fake.ring_state && !fake.hook) {
				fake.ring_state = r;
				fake.ring_edges++;
				fake.int_src |= INT_RING;
				fake.event_due = true;
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
				fake.samples_out += p.length / 2;
				p.actual_length = p.length;
				p.status = LIBUSB_TRANSFER_COMPLETED;
				off += (int)p.length;
			}
			t->actual_length = off;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (fake.event_due && !fake.intr_q.empty()) {
			libusb_transfer *t = fake.intr_q.front();
			fake.intr_q.pop_front();
			fake.event_due = false;
			uint8_t b[8] = { state(), fake.ring_edges, fake.int_src, ++fake.seq,
					 0, 0, 0, 0 };
			fake.int_src = 0;
			int n = t->length < 8 ? t->length : 8;
			memcpy(t->buffer, b, (size_t)n);
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
					fake.samples_in += n;
					p.actual_length = n * 2;
					p.status = LIBUSB_TRANSFER_COMPLETED;
					off += (int)p.length;
				}
				t->actual_length = off;
				/* a hook flash over: off hook again */
				if (fake.flash_end && fake.ms >= fake.flash_end) {
					fake.flash_end = 0;
					line_hook(fake.hook);
					fake.event_due = true;
				}
				done(t, LIBUSB_TRANSFER_COMPLETED);
				more = true;
			}
		}
	}
}

/* the configuration: interface 0, alternate settings 0 (no endpoints)
   and 1 (isochronous OUT and IN, interrupt IN) */
libusb_endpoint_descriptor endpoints[3];
libusb_interface_descriptor alts[2];
libusb_interface iface;
libusb_config_descriptor config;

/* the configuration's descriptors filled in */
void make_config()
{
	endpoints[0].bEndpointAddress = EP_OUT;
	endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS | 0x04;   /* asynchronous */
	endpoints[0].wMaxPacketSize = MAX_PACKET;
	endpoints[0].bInterval = 1;
	endpoints[1].bEndpointAddress = EP_IN;
	endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS | 0x04;
	endpoints[1].wMaxPacketSize = MAX_PACKET;
	endpoints[1].bInterval = 1;
	endpoints[2].bEndpointAddress = EP_EVENT;
	endpoints[2].bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
	endpoints[2].wMaxPacketSize = 8;
	endpoints[2].bInterval = 4;
	alts[0].bInterfaceNumber = 0;
	alts[0].bInterfaceClass = 0xff;
	alts[1].bInterfaceNumber = 0;
	alts[1].bInterfaceClass = 0xff;
	alts[1].bAlternateSetting = 1;
	alts[1].bNumEndpoints = 3;
	alts[1].endpoint = endpoints;
	iface.altsetting = alts;
	iface.num_altsetting = 2;
	config.bNumInterfaces = 1;
	config.interface = &iface;
}

/* a vendor request's reply into data: its length, or LIBUSB_ERROR_PIPE */
int request_in(uint8_t req, uint16_t value, unsigned char *data, uint16_t length)
{
	uint8_t b[20] = {};
	int n;

	switch (req) {
	case GET_INFO:
		b[0] = 1;                       /* version */
		b[1] = 0; b[2] = 1; b[3] = 0;   /* firmware 0.1.0 */
		b[4] = (uint8_t)fake.rate; b[5] = (uint8_t)(fake.rate >> 8);
		b[6] = 20;
		b[7] = state();
		b[8] = 0x15;                    /* a Si3018, the Si3056 rev E */
		b[9] = 0x05 << 2;               /* the line side rev E */
		n = 12;
		break;
	case GET_STATUS:
		b[0] = fake.nolink ? 0 : 0x40;  /* FDT */
		b[3] = (uint8_t)((fake.monitor ? 0x08 : 0) | 0x02 | (fake.hook ? 0x01 : 0));
		b[4] = fake.int_src;
		b[5] = state();
		b[10] = (uint8_t)fake.samples_in; b[11] = (uint8_t)(fake.samples_in >> 8);
		b[12] = (uint8_t)(fake.samples_in >> 16); b[13] = (uint8_t)(fake.samples_in >> 24);
		b[14] = (uint8_t)fake.samples_out; b[15] = (uint8_t)(fake.samples_out >> 8);
		b[16] = (uint8_t)(fake.samples_out >> 16); b[17] = (uint8_t)(fake.samples_out >> 24);
		b[18] = fake.ring_edges;
		fake.int_src = 0;
		n = 20;
		break;
	case DAA_READ:
		if (value > 127)
			return LIBUSB_ERROR_PIPE;
		b[0] = fake.regs[value];
		n = 1;
		break;
	default:
		return LIBUSB_ERROR_PIPE;
	}
	if (length < n)
		n = length;
	memcpy(data, b, (size_t)n);
	return n;
}

/* a vendor request with no data: 0, or LIBUSB_ERROR_PIPE (a stall) */
int request_out(uint8_t req, uint16_t value)
{
	static const unsigned rates[] = { 7200, 8000, 8229, 8400, 9000, 9600,
					  10286, 12000, 13714, 16000 };
	switch (req) {
	case SET_RATE:
		for (unsigned r : rates)
			if (r == value) {
				fake.rate = value;
				return 0;
			}
		return LIBUSB_ERROR_PIPE;
	case SET_HOOK:
		if (value > 1)
			return LIBUSB_ERROR_PIPE;
		fake.hook = value;
		fake.flash_end = 0;
		line_hook(fake.hook);
		fake.event_due = true;
		return 0;
	case HOOK_FLASH:
		if (value < 1 || value > 2000)
			return LIBUSB_ERROR_PIPE;
		if (fake.hook) {
			line_hook(false);
			fake.flash_end = fake.ms + value;
		}
		return 0;
	case SET_MONITOR:
		if (value > 1)
			return LIBUSB_ERROR_PIPE;
		if ((bool)value != fake.monitor) {
			fake.monitor = value;
			switch_to(1, fake.monitor);
		}
		return 0;
	case DAA_WRITE:
		if ((value >> 8) > 127)
			return LIBUSB_ERROR_PIPE;
		fake.regs[value >> 8] = (uint8_t)value;
		return 0;
	case RESET:
		fake.hook = false;
		fake.flash_end = 0;
		line_hook(false);
		return 0;
	case REBOOT_UF2:
		fake.gone = true;
		return 0;
	}
	return LIBUSB_ERROR_PIPE;
}

} // namespace

extern "C" {

#include "fake_usb_backend.h"

/* the context (FAKE_PICO_LINE_LOG read) */
int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	fake.ctx = new libusb_context();
	fake.log = getenv("FAKE_PICO_LINE_LOG") != nullptr;
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

/* the stick's descriptor (1209:0002, USB 2.0) */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *,
					     libusb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->bLength = LIBUSB_DT_DEVICE_SIZE;
	d->bDescriptorType = LIBUSB_DT_DEVICE;
	d->bcdUSB = 0x0200;
	d->idVendor = 0x1209;
	d->idProduct = 0x0002;
	d->bNumConfigurations = 1;
	return 0;
}

/* bus 1 */
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *)
{
	return 1;
}

/* full speed (only the Agere line asks; on Windows this replaces
   libusb, which would answer otherwise) */
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
   FAKE_PICO_LINE_SOCKET (LIBUSB_ERROR_BUSY if open, _ACCESS if no
   socket, _IO if no exchange) */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	const char *path = getenv("FAKE_PICO_LINE_SOCKET");

	if (fake.open)
		return LIBUSB_ERROR_BUSY;
	if (!path)
		return LIBUSB_ERROR_ACCESS;
	fake.cap = connect_exchange(path, 'C');
	fake.play = connect_exchange(path, 'P');
	fake.ctl = connect_exchange(path, 'K');
	if (fake.cap < 0 || fake.play < 0 || fake.ctl < 0) {
		fprintf(stderr, "fake pico-serial Line: no exchange at %s\n", path);
		for (int fd : { fake.cap, fake.play, fake.ctl })
			fake_net::close_fd(fd);
		fake.cap = fake.play = fake.ctl = -1;
		return LIBUSB_ERROR_IO;
	}
	fake.nolink = getenv("FAKE_PICO_LINE_NOLINK") != nullptr;
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

/* interface 0 claimed (the stick's other, the RP2040's reset
   interface, is not for dmodemd) */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int n)
{
	return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* interface 0 released */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int n)
{
	return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* interface 0's setting alt, 0 or 1: 1 starts the samples (the FIFOs
   emptied either way), and an event says so */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *,
						 int n, int alt)
{
	if (fake.log)
		fprintf(stderr, "fake pico-serial Line: alternate setting %d\n", alt);
	if (n != 0 || (alt != 0 && alt != 1))
		return LIBUSB_ERROR_NOT_FOUND;
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	fake.alt = alt;
	fake.in.clear();
	fake.ms = 0;
	fake.event_due = alt == 1;
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

/* nothing to free (static) */
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor *)
{
}

/* a vendor request to interface 0 (or the device): request_in() or
   request_out(); LIBUSB_ERROR_NO_DEVICE once gone */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *,
					uint8_t type, uint8_t request,
					uint16_t value, uint16_t index,
					unsigned char *data, uint16_t length,
					unsigned int)
{
	int r;

	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if ((type & LIBUSB_REQUEST_TYPE_VENDOR) != LIBUSB_REQUEST_TYPE_VENDOR ||
	    index != 0)
		return LIBUSB_ERROR_PIPE;
	r = type & LIBUSB_ENDPOINT_IN ? request_in(request, value, data, length) :
		request_out(request, value);
	if (fake.log)
		fprintf(stderr, "fake pico-serial Line: request %02x %s %u: %d\n",
			request, type & LIBUSB_ENDPOINT_IN ? "in" : "out", value, r);
	return r;
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

/* t queued at its endpoint, done in run() */
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer *t)
{
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (fake_usb_refused(t))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	if (t->endpoint == EP_EVENT)
		fake.intr_q.push_back(t);
	else if (t->endpoint == EP_IN)
		fake.in_q.push_back(t);
	else if (t->endpoint == EP_OUT)
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
