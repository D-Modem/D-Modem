/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fake_agere.cpp - an Agere USB 2.0 soft modem for the tests: the part
 *                     of libusb-1.0 that usb_line.cpp and usb_agere.cpp
 *                     use (LD_PRELOAD in front of the real one), with one
 *                     such modem on the bus whose telephone line is
 *                     line_exchange's
 *
 *    What it does is what docs/agere-usb.md says the device does, as far
 *    as dmodemd sees it: USB 047e:2892, interface 0's alternate setting
 *    2 with a bulk OUT (0x01) and IN (0x82) endpoint; vendor commands
 *    (bRequest) on endpoint 0 for its ROM's version (1), the ARM's memory
 *    (2 read, 3 write: the address's high half the value, its low half
 *    the index), the patches (9: index 4 a bulk patch, 6 the clock
 *    change patch, 7 a bulk patch's hook), the codec's registers (0x0a,
 *    0x0b), the samples (0x10) and the clock (0x11, 0x12).  The patches
 *    are the tests' (agere_patch.h's test_patch()), the one the ROM wants
 *    (FAKE_AGERE_ROM, FAKE_AGERE_FLASH): another is refused (a stall).
 *    The ROM's function table at 0x70000a64 says which patches are in.
 *
 *    FAKE_AGERE_SOCKET is the exchange's socket of the line.  Opening the
 *    device connects to it as fake_modem_card.cpp does ('C' the line's
 *    samples, 'P' the modem's, 'K' the switches), its line monitor on:
 *    the Mercury codec hears the line on hook.  The hook is codec
 *    register 0xc0 (0 off hook).  The samples flow once command 0x10 has
 *    said 1 with the bulk patch in and its hook (the function table's
 *    entries 4 and 7 as the patch has them): each IN transfer done takes
 *    48 of the line's samples (8000 Hz) as they come from the exchange,
 *    after a header of their count and a status word whose bit 0 is set
 *    while the ring signal is there; an OUT transfer's samples (as many
 *    as its header says) go to the exchange, and it is done, at once.
 *    The capture socket is libusb's descriptor to poll.  The device is
 *    gone (LIBUSB_TRANSFER_NO_DEVICE) when the exchange is.
 *
 *    FAKE_AGERE_ROM=<hex>: the ROM's version (command 1), 0x121 without
 *    it; 0x106 wants the clock change patch for commands 0x11.
 *    FAKE_AGERE_FLASH=1: a 1.2.1 in flash (the function table's entry 0
 *    bit 29), whose bulk patch is v121_flash; else v121_rom.
 *    FAKE_AGERE_POLARIS=1: the codec does not answer (command 0x0a stalls).
 *    FAKE_AGERE_FULL=1: a full speed device (command 0x10 wanted before
 *    the transfers; at high speed after them).
 *    FAKE_AGERE_LOG=1: the commands on stderr.
 *
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_net.h"

#include <deque>
#include <map>
#include <vector>

#include <libusb.h>

#include "../agere_patch.h"

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

const uint8_t EP_OUT = 0x01, EP_IN = 0x82;
const uint16_t MAX_PACKET = 512;
const int RING_LEVEL = 8000;
const unsigned SAMPLES = 48;
const uint32_t FUNCTABLE = 0x70000a64;

libusb_device the_device;

struct Fake {
	libusb_context *ctx = nullptr;
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	int alt = 0;
	uint16_t rom = 0x121;
	bool flash = false, polaris = false, full = false;
	bool hook = false;              /* off hook */
	bool streaming = false;         /* command 0x10's 1 */
	bool said_stuck = false;
	std::map<uint32_t, uint8_t> mem;
	std::map<uint16_t, uint16_t> codec;
	bool gone = false;
	std::deque<int16_t> in;         /* the line's samples, not yet taken */
	std::deque<bool> ringing;       /* the ring signal there, a sample each */
	int half = -1;
	std::deque<libusb_transfer *> in_q, out_q, cancelled;
	int log = 0;
} fake;

/* a 32-bit word of the ARM's memory */
uint32_t mem32(uint32_t a)
{
	uint32_t v = 0;
	for (int i = 3; i >= 0; i--)
		v = v << 8 | fake.mem[a + (uint32_t)i];
	return v;
}

/* a 32-bit word into the ARM's memory */
void set32(uint32_t a, uint32_t v)
{
	for (uint32_t i = 0; i < 4; i++)
		fake.mem[a + i] = (uint8_t)(v >> (8 * i));
}

/* a 16-bit word into the ARM's memory */
void set16(uint32_t a, uint16_t v)
{
	fake.mem[a] = (uint8_t)v;
	fake.mem[a + 1] = (uint8_t)(v >> 8);
}

/* the bulk patch the ROM wants, and what it puts in entries 4 and 7 */
int wanted(uint32_t *e4, uint32_t *e7)
{
	if (fake.rom == 0x106) {
		*e4 = 0x700013f0;
		*e7 = 0x70001400;
		return agere::V106;
	}
	if (fake.flash) {
		*e4 = 0x700013f0;
		*e7 = 0x70001400;
		return agere::V121_FLASH;
	}
	*e4 = 0x70001588;
	*e7 = 0x70001598;
	return agere::V121_ROM;
}

/* the bulk patch in and hooked: the function table says so */
bool patched()
{
	uint32_t e4, e7;
	wanted(&e4, &e7);
	return mem32(FUNCTABLE + 16) == e4 && mem32(FUNCTABLE + 28) == e7;
}

/* data (n octets) the tests' patch i */
bool is_patch(int i, const unsigned char *data, uint16_t n)
{
	if (n != agere::known(i).len)
		return false;
	for (uint32_t k = 0; k < n; k++)
		if (data[k] != agere::test_patch(i, k))
			return false;
	return true;
}

/* the ARM's memory and the codec after reset: the ROM's function table
   (entry 0's bit 29 a 1.2.1 in flash), the PLL locked, the clock's
   divider, the codec ready */
void reset()
{
	fake.mem.clear();
	fake.codec.clear();
	for (uint32_t i = 0; i < 8; i++)
		set32(FUNCTABLE + 4 * i, 0x00001000 + 0x40 * i);
	set32(FUNCTABLE, fake.flash ? 0x20001000 : 0x00001000);
	set16(0xcd000008, 1);
	set16(0xcd00000c, 3);
	set16(0xcd000030, 0x000f);
	fake.codec[0x2dc] = 1;
	fake.codec[0xc0] = 1;
	fake.streaming = false;
}

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

/* the exchange's samples that have come; the ring signal on them */
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
			if (fake.streaming) {
				fake.in.push_back(s);
				fake.ringing.push_back(!fake.hook &&
						       (s > RING_LEVEL || s < -RING_LEVEL));
			}
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
			for (auto *q : { &fake.in_q, &fake.out_q }) {
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
			unsigned n = t->length >= 4 ?
				(unsigned)(t->buffer[0] | t->buffer[1] << 8) : 0;
			if (n > (unsigned)(t->length - 4) / 2)
				n = (unsigned)(t->length - 4) / 2;
			if (fake.play >= 0 && n &&
			    !fake_net::send_all(fake.play, t->buffer + 4, n * 2))
				fake.gone = true;
			t->actual_length = t->length;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (fake.streaming && !patched() && !fake.said_stuck) {
			fprintf(stderr, "fake Agere: streaming without the bulk "
				"patch: no samples\n");
			fake.said_stuck = true;
		}
		if (!fake.in_q.empty() && fake.streaming && patched() &&
		    fake.in.size() >= SAMPLES) {
			libusb_transfer *t = fake.in_q.front();
			fake.in_q.pop_front();
			bool ring = false;
			for (unsigned k = 0; k < SAMPLES; k++) {
				int16_t s = fake.in.front();
				fake.in.pop_front();
				ring = ring || fake.ringing.front();
				fake.ringing.pop_front();
				t->buffer[4 + 2 * k] = (uint8_t)s;
				t->buffer[5 + 2 * k] = (uint8_t)(s >> 8);
			}
			t->buffer[0] = SAMPLES;
			t->buffer[1] = 0;
			t->buffer[2] = ring ? 1 : 0;
			t->buffer[3] = 0;
			t->actual_length = 4 + 2 * SAMPLES;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
	}
}

/* the configuration: interface 0, alternate settings 0 (no endpoints),
   1 (the ROM's isochronous ones, not used here: none) and 2 (bulk OUT
   and IN) */
libusb_endpoint_descriptor endpoints[2];
libusb_interface_descriptor alts[3];
libusb_interface iface;
libusb_config_descriptor config;

/* the configuration's descriptors filled in */
void make_config()
{
	endpoints[0].bEndpointAddress = EP_OUT;
	endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
	endpoints[0].wMaxPacketSize = MAX_PACKET;
	endpoints[1].bEndpointAddress = EP_IN;
	endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
	endpoints[1].wMaxPacketSize = MAX_PACKET;
	for (int i = 0; i < 3; i++) {
		alts[i].bInterfaceNumber = 0;
		alts[i].bInterfaceClass = 0xff;
		alts[i].bAlternateSetting = (uint8_t)i;
	}
	alts[2].bNumEndpoints = 2;
	alts[2].endpoint = endpoints;
	iface.altsetting = alts;
	iface.num_altsetting = 3;
	config.bNumInterfaces = 1;
	config.interface = &iface;
}

/* a vendor command with data in (into data): its length, or
   LIBUSB_ERROR_PIPE (a stall) */
int command_in(uint8_t cmd, uint16_t index, uint16_t value,
	       unsigned char *data, uint16_t length)
{
	uint32_t a = (uint32_t)value << 16 | index;
	switch (cmd) {
	case 0x01:
		if (length < 2)
			return LIBUSB_ERROR_PIPE;
		data[0] = (uint8_t)fake.rom;
		data[1] = (uint8_t)(fake.rom >> 8);
		return 2;
	case 0x02:
		for (uint16_t i = 0; i < length; i++)
			data[i] = fake.mem[a + i];
		return length;
	case 0x0a:
		if (fake.polaris || length < 2)
			return LIBUSB_ERROR_PIPE;
		data[0] = (uint8_t)fake.codec[index];
		data[1] = (uint8_t)(fake.codec[index] >> 8);
		return 2;
	}
	return LIBUSB_ERROR_PIPE;
}

/* a vendor command with data out (data, length octets): 0, or
   LIBUSB_ERROR_PIPE (a stall) */
int command_out(uint8_t cmd, uint16_t index, uint16_t value,
		const unsigned char *data, uint16_t length)
{
	uint32_t a = (uint32_t)value << 16 | index;
	uint32_t e4, e7;
	switch (cmd) {
	case 0x03:
		for (uint16_t i = 0; i < length; i++)
			fake.mem[a + i] = data[i];
		return 0;
	case 0x09:
		if (index == 4) {
			int want = wanted(&e4, &e7);
			if (!is_patch(want, data, length)) {
				fprintf(stderr, "fake Agere: not %s\n",
					agere::known(want).name);
				return LIBUSB_ERROR_PIPE;
			}
			set32(FUNCTABLE + 16, e4);
			return 0;
		}
		if (index == 7) {
			wanted(&e4, &e7);
			if (length != 12 || mem32(FUNCTABLE + 16) != e4 ||
			    data[4] != 2)
				return LIBUSB_ERROR_PIPE;
			set32(FUNCTABLE + 28, (uint32_t)data[8] | (uint32_t)data[9] << 8 |
			      (uint32_t)data[10] << 16 | (uint32_t)data[11] << 24);
			return 0;
		}
		if (index == 6) {
			if (fake.rom != 0x106 ||
			    !is_patch(agere::ASYNC_CLOCKCHANGE, data, length))
				return LIBUSB_ERROR_PIPE;
			set32(FUNCTABLE + 24, 0x70001140);
			return 0;
		}
		return LIBUSB_ERROR_PIPE;
	case 0x0b:
		if (fake.polaris)
			return LIBUSB_ERROR_PIPE;
		fake.codec[index] = value;
		if (index == 0xc0 && (value == 0) != fake.hook) {
			fake.hook = value == 0;
			switch_to(0, fake.hook);
		}
		return 0;
	case 0x10:
		if (value == 1) {
			fake.streaming = true;
			fake.in.clear();
			fake.ringing.clear();
		}
		return 0;
	case 0x11:
		return fake.rom == 0x106 && mem32(FUNCTABLE + 24) == 0x70001140 ?
			0 : LIBUSB_ERROR_PIPE;
	case 0x12:
		return fake.rom != 0x106 ? 0 : LIBUSB_ERROR_PIPE;
	}
	return LIBUSB_ERROR_PIPE;
}

} // namespace

extern "C" {

#include "fake_usb_backend.h"

/* the context (FAKE_AGERE_* read) */
int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	fake.ctx = new libusb_context();
	fake.log = getenv("FAKE_AGERE_LOG") != nullptr;
	if (const char *r = getenv("FAKE_AGERE_ROM"))
		fake.rom = (uint16_t)strtoul(r, nullptr, 16);
	fake.flash = getenv("FAKE_AGERE_FLASH") != nullptr;
	fake.polaris = getenv("FAKE_AGERE_POLARIS") != nullptr;
	fake.full = getenv("FAKE_AGERE_FULL") != nullptr;
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

/* the modem's descriptor (047e:2892, USB 2.0) */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *,
					     libusb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->bLength = LIBUSB_DT_DEVICE_SIZE;
	d->bDescriptorType = LIBUSB_DT_DEVICE;
	d->bcdUSB = 0x0200;
	d->idVendor = 0x047e;
	d->idProduct = 0x2892;
	d->bNumConfigurations = 1;
	return 0;
}

/* bus 1 */
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *)
{
	return 1;
}

/* its one port on the bus (only Linux's remote wakeup asks, through
   /sys; the fake linked in libusb's place answers it too) */
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *, uint8_t *ports,
					int len)
{
	if (len < 1)
		return LIBUSB_ERROR_OVERFLOW;
	ports[0] = 250;
	return 1;
}

/* address 9 */
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *)
{
	return 9;
}

/* high speed, or full (FAKE_AGERE_FULL) */
int LIBUSB_CALL libusb_get_device_speed(libusb_device *)
{
	return fake.full ? LIBUSB_SPEED_FULL : LIBUSB_SPEED_HIGH;
}

/* the device opened: its connections to the exchange at
   FAKE_AGERE_SOCKET, the line monitor on (LIBUSB_ERROR_BUSY if open,
   _ACCESS if no socket, _IO if no exchange) */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	const char *path = getenv("FAKE_AGERE_SOCKET");

	if (fake.open)
		return LIBUSB_ERROR_BUSY;
	if (!path)
		return LIBUSB_ERROR_ACCESS;
	fake.cap = connect_exchange(path, 'C');
	fake.play = connect_exchange(path, 'P');
	fake.ctl = connect_exchange(path, 'K');
	if (fake.cap < 0 || fake.play < 0 || fake.ctl < 0) {
		fprintf(stderr, "fake Agere: no exchange at %s\n", path);
		for (int fd : { fake.cap, fake.play, fake.ctl })
			fake_net::close_fd(fd);
		fake.cap = fake.play = fake.ctl = -1;
		return LIBUSB_ERROR_IO;
	}
	reset();
	switch_to(1, true);
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

/* interface 0 claimed */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int n)
{
	return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* interface 0 released */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int n)
{
	return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* interface 0's setting alt, 0 to 2 */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *,
						 int n, int alt)
{
	if (fake.log)
		fprintf(stderr, "fake Agere: alternate setting %d\n", alt);
	if (n != 0 || alt < 0 || alt > 2)
		return LIBUSB_ERROR_NOT_FOUND;
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	fake.alt = alt;
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

/* a vendor command to the device: command_in() or command_out(); the
   driver's odd SET_CONFIGURATION (bmRequestType 0x0b) stalls;
   LIBUSB_ERROR_NO_DEVICE once gone */
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
	    (type & 0x1f) != LIBUSB_RECIPIENT_DEVICE)
		r = LIBUSB_ERROR_PIPE;
	else
		r = type & LIBUSB_ENDPOINT_IN ?
			command_in(request, index, value, data, length) :
			command_out(request, index, value, data, length);
	if (fake.log)
		fprintf(stderr, "fake Agere: %02x command %02x index %04x value "
			"%04x %u: %d\n", type, request, index, value, length, r);
	return r;
}

/* a transfer, zeroed */
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

/* t queued at its endpoint (bulk ones only, in alternate setting 2),
   done in run() */
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer *t)
{
	if (fake.gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (t->type != LIBUSB_TRANSFER_TYPE_BULK || fake.alt != 2)
		return LIBUSB_ERROR_NOT_FOUND;
	if (t->endpoint == EP_IN)
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
	for (auto *q : { &fake.in_q, &fake.out_q })
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
