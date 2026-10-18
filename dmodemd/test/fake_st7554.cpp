/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fake_st7554.cpp - a SmartUSB56 for the tests: the part of libusb-1.0
 *                      that usb_line.cpp uses (LD_PRELOAD in front of the
 *                      real one), with one ST7554 on the bus whose
 *                      telephone line is line_exchange's
 *
 *    FAKE_ST7554_SOCKET is the exchange's socket of the line.  Opening
 *    the device connects to it as fake_modem_card.cpp does ('C' the
 *    line's samples, 'P' the modem's, 'K' the switches), with the line
 *    monitor on, so that the exchange sends the ring signal: the fake's
 *    ring detector (GPIO 0) changes on each half cycle of it, a change
 *    each on the interrupt endpoint.  On hook the codec hears nothing.
 *
 *    The samples flow once the codec is powered up and the FIFO counter
 *    set (SSI1_CONTROL, SSI1_COUNTER): each IN transfer done takes 1 ms
 *    of the line's samples a packet, as they come from the exchange; an
 *    OUT transfer's samples go to the exchange, and it is done, at once.
 *    The capture socket is libusb's descriptor to poll.  The device is
 *    gone (LIBUSB_TRANSFER_NO_DEVICE) when the exchange is.
 *
 *    FAKE_ST7554_DAA=1: the GPIO status says an ST75951/2 (silicon DAA).
 *    FAKE_ST7554_LOG=1: the register writes on stderr.
 *
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

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

const uint8_t EP_INTR = 0x81, EP_OUT = 0x02, EP_IN = 0x83;
const uint16_t MAX_PACKET = 32;
const uint16_t GPIO_RING1 = 0x0001, GPIO_HOOK1 = 0x0008;
const uint16_t GPIO_MONOLITHIC = 0x4000;
const int RING_LEVEL = 8000;

libusb_device the_device;

struct Fake {
	libusb_context *ctx = nullptr;
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	uint16_t regs[0x20] = {};
	unsigned rate = 0;              /* from the control word */
	bool hook = false;              /* off hook */
	bool ring_state = false;
	unsigned ring_changes = 0;      /* not yet on the interrupt */
	bool gone = false;
	std::deque<int16_t> in;         /* the line's samples, not yet taken */
	int half = -1;
	unsigned long long ms = 0;      /* IN packets given */
	std::deque<libusb_transfer *> in_q, out_q, intr_q, cancelled;
	int log = 0;
} fake;

/* a connection to the exchange at path, its first octet kind; -1 */
int connect_exchange(const char *path, char kind)
{
	struct sockaddr_un a = {};
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0 ||
	    write(fd, &kind, 1) != 1) {
		close(fd);
		return -1;
	}
	return fd;
}

/* switch which (0 the hook, 1 caller ID) to the exchange
   (the device gone if it can't) */
void switch_to(uint8_t which, bool on)
{
	unsigned char m[2] = { which, (unsigned char)on };
	if (fake.ctl >= 0 && write(fake.ctl, m, 2) != 2)
		fake.gone = true;
}

/* the samples flow: the codec powered up, the FIFO
   counter set, a rate known */
bool streaming()
{
	return fake.regs[0x01] & 1 && fake.regs[0x04] && fake.rate;
}

/* a register written (a vendor request) */
void set_reg(uint8_t reg, uint16_t v)
{
	if (fake.log)
		fprintf(stderr, "fake st7554: %02x = %04x\n", reg, v);
	if (reg >= 0x20)
		return;
	fake.regs[reg] = v;
	if (reg == 0x17)
		fake.rate = v == 0x3c8 ? 8000 : v == 0x3c0 ? 9600 :
			v == 0x3f0 ? 16000 : 0;
	if (reg == 0x0a) {
		bool off = v & GPIO_HOOK1;
		if (off != fake.hook) {
			fake.hook = off;
			switch_to(0, off);
		}
	}
}

/* the exchange's samples that have come; the ring detector on them */
void take()
{
	unsigned char b[4096];
	ssize_t n;

	if (fake.cap < 0)
		return;
	while ((n = recv(fake.cap, b, sizeof(b), MSG_DONTWAIT)) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (fake.half < 0) {
				fake.half = b[i];
				continue;
			}
			int16_t s = (int16_t)(fake.half | b[i] << 8);
			fake.half = -1;
			bool r = s > RING_LEVEL;
			if (r != fake.ring_state && !fake.hook) {
				fake.ring_state = r;
				if (fake.regs[0x0c] & GPIO_RING1)
					fake.ring_changes++;
			}
			if (streaming())
				fake.in.push_back(fake.hook ? s : 0);
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
				    send(fake.play, t->buffer + off, p.length,
					 MSG_NOSIGNAL) != (ssize_t)p.length)
					fake.gone = true;
				p.actual_length = p.length;
				p.status = LIBUSB_TRANSFER_COMPLETED;
				off += (int)p.length;
			}
			t->actual_length = off;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		while (fake.ring_changes && !fake.intr_q.empty()) {
			libusb_transfer *t = fake.intr_q.front();
			fake.intr_q.pop_front();
			fake.ring_changes--;
			uint8_t b[4] = { 0, 0, (uint8_t)GPIO_RING1, 0 };
			memcpy(t->buffer, b, t->length < 4 ? (size_t)t->length : 4);
			t->actual_length = t->length < 4 ? t->length : 4;
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

/* the configuration: interface 0, alternate settings 0 (no endpoints)
   and 1 (interrupt IN, isochronous OUT and IN) */
libusb_endpoint_descriptor endpoints[3];
libusb_interface_descriptor alts[2];
libusb_interface iface;
libusb_config_descriptor config;

/* the configuration's descriptors filled in */
void make_config()
{
	endpoints[0].bEndpointAddress = EP_INTR;
	endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
	endpoints[0].wMaxPacketSize = 4;
	endpoints[0].bInterval = 1;
	endpoints[1].bEndpointAddress = EP_OUT;
	endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
	endpoints[1].wMaxPacketSize = MAX_PACKET;
	endpoints[1].bInterval = 1;
	endpoints[2].bEndpointAddress = EP_IN;
	endpoints[2].bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
	endpoints[2].wMaxPacketSize = MAX_PACKET;
	endpoints[2].bInterval = 1;
	alts[0].bInterfaceNumber = 0;
	alts[1].bInterfaceNumber = 0;
	alts[1].bAlternateSetting = 1;
	alts[1].bNumEndpoints = 3;
	alts[1].endpoint = endpoints;
	iface.altsetting = alts;
	iface.num_altsetting = 2;
	config.bNumInterfaces = 1;
	config.interface = &iface;
}

} // namespace

extern "C" {

/* the context (FAKE_ST7554_LOG read) */
int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	fake.ctx = new libusb_context();
	fake.log = getenv("FAKE_ST7554_LOG") != nullptr;
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
	case LIBUSB_ERROR_INVALID_PARAM: return "Invalid parameter";
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

/* an ST7554's descriptor (0483:7554, USB 1.1) */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *,
					     libusb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->bLength = LIBUSB_DT_DEVICE_SIZE;
	d->bDescriptorType = LIBUSB_DT_DEVICE;
	d->bcdUSB = 0x0110;
	d->idVendor = 0x0483;
	d->idProduct = 0x7554;
	d->bNumConfigurations = 1;
	return 0;
}

/* bus 1 */
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *)
{
	return 1;
}

/* address 7 */
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *)
{
	return 7;
}

/* the device opened: its connections to the exchange
   at FAKE_ST7554_SOCKET, the line monitor on
   (LIBUSB_ERROR_BUSY if open, _ACCESS if no socket, _IO
   if no exchange) */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	const char *path = getenv("FAKE_ST7554_SOCKET");

	if (fake.open)
		return LIBUSB_ERROR_BUSY;
	if (!path)
		return LIBUSB_ERROR_ACCESS;
	fake.cap = connect_exchange(path, 'C');
	fake.play = connect_exchange(path, 'P');
	fake.ctl = connect_exchange(path, 'K');
	if (fake.cap < 0 || fake.play < 0 || fake.ctl < 0) {
		fprintf(stderr, "fake st7554: no exchange at %s\n", path);
		for (int fd : { fake.cap, fake.play, fake.ctl })
			if (fd >= 0)
				close(fd);
		fake.cap = fake.play = fake.ctl = -1;
		return LIBUSB_ERROR_IO;
	}
	switch_to(1, true);             /* the ring signal heard */
	if (getenv("FAKE_ST7554_DAA"))
		fake.regs[0x15] = GPIO_MONOLITHIC;
	fake.open = true;
	*h = new libusb_device_handle{ dev };
	return 0;
}

/* the connections closed, the device free again */
void LIBUSB_CALL libusb_close(libusb_device_handle *h)
{
	for (int fd : { fake.cap, fake.play, fake.ctl })
		if (fd >= 0)
			close(fd);
	fake.cap = fake.play = fake.ctl = -1;
	fake.open = false;
	delete h;
}

/* h's device */
libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *h)
{
	return h->dev;
}

/* nothing to detach: 0 */
int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *,
						     int)
{
	return 0;
}

/* interface 0 claimed (no other) */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int n)
{
	return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* interface 0 released (no other) */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int n)
{
	return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* interface 0's setting alt, 0 or 1 */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *,
						 int n, int alt)
{
	return n == 0 && (alt == 0 || alt == 1) ? 0 : LIBUSB_ERROR_NOT_FOUND;
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

/* a vendor request: a register read (2 octets) or
   written (set_reg()); LIBUSB_ERROR_NO_DEVICE once gone */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *,
					uint8_t type, uint8_t request,
					uint16_t value, uint16_t index,
					unsigned char *data, uint16_t length,
					unsigned int)
{
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if ((type & LIBUSB_REQUEST_TYPE_VENDOR) != LIBUSB_REQUEST_TYPE_VENDOR ||
	    index != 0)
		return LIBUSB_ERROR_PIPE;
	if (type & LIBUSB_ENDPOINT_IN) {
		uint8_t reg = request & 0x7f;
		uint16_t v = reg < 0x20 ? fake.regs[reg] : 0;
		if (length < 2)
			return LIBUSB_ERROR_OVERFLOW;
		data[0] = (uint8_t)v;
		data[1] = (uint8_t)(v >> 8);
		return 2;
	}
	set_reg(request, value);
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

/* t queued at its endpoint, done in run() */
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer *t)
{
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (t->endpoint == EP_INTR)
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
	/* (close()'s wait: nothing more will come of the time) */
	(void)tv;
	return 0;
}

} // extern "C"
