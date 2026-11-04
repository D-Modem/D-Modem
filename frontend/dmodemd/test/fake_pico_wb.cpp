/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_pico_wb.cpp - a pico-serial WB stick for the tests: the part of
 *                       libusb-1.0 that usb_line.cpp and usb_wb.cpp use
 *                       (LD_PRELOAD in front of the real one), with one
 *                       stick on the bus whose bare pair is wb_pair's
 *
 *    The stick's protocol is its firmware's (pico-serial's
 *    firmware/PROTOCOL.md "WB stick", src/core/wb_proto.h): USB
 *    1209:0003, interface 0 (class 0xff, subclass 0x57) whose alternate
 *    setting 1 has an isochronous OUT (0x01) and IN (0x82) endpoint of
 *    480 octets and an interrupt IN one (0x83) of 8-octet events; vendor
 *    requests to the interface for the rest.
 *
 *    FAKE_PICO_WB_SOCKET is wb_pair's socket of this end of the pair.
 *    Opening the device connects to it as fake_pico_line.cpp does to the
 *    exchange ('C' the pair's samples, 'P' the modem's; there are no
 *    switches).  The samples flow in alternate setting 1: each IN
 *    transfer done takes 1 ms of the pair's samples a packet, at the rate
 *    set (96000 Hz after reset; SET_RATE in kHz: 48, 96 or 192), as they
 *    come from wb_pair (its clock is the stick's); an OUT transfer's
 *    samples go to wb_pair (which plays them as the stick does, after a
 *    head start), and it is done, at once.  SET_LOOPBACK 1 (the
 *    firmware's) sends the OUT samples back on IN instead of the pair's.
 *    An event goes on the interrupt endpoint when the samples start and
 *    whenever the state (the CARRIER bit: SET_CARRIER) changes.  The
 *    capture socket is libusb's descriptor to poll.  The device is gone
 *    (LIBUSB_TRANSFER_NO_DEVICE) when wb_pair is.
 *
 *    FAKE_PICO_WB_NOCODEC=1: the codec does not come up (its state bit
 *    clear).
 *    FAKE_PICO_WB_LOG=1: the requests on stderr.
 *    FAKE_USB_NO_ISO, FAKE_USB_USBDK: fake_libusb.h (Windows 7's
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

#include "fake_libusb.h"

namespace {

const uint8_t EP_OUT = 0x01, EP_IN = 0x82, EP_EVENT = 0x83;
const uint16_t MAX_PACKET = 480;

/* the requests and state bits (firmware/src/core/wb_proto.h) */
enum {
	GET_INFO = 0x01, GET_STATUS = 0x02, SET_RATE = 0x10, SET_CARRIER = 0x11,
	SET_TX_GAIN = 0x12, SET_RX_GAIN = 0x13, SET_INPUT = 0x14,
	SET_LOOPBACK = 0x15, CODEC_READ = 0x20, CODEC_WRITE = 0x21,
	RESET = 0x30, REBOOT_UF2 = 0x31,
};
enum { CARRIER_OFF, CARRIER_ON, CARRIER_AUTO };
enum { LOOP_OFF, LOOP_FIRMWARE, LOOP_CODEC };
const uint8_t ST_CODEC = 0x01, ST_CARRIER = 0x02, ST_LOOPBACK = 0x04,
	ST_STREAMING = 0x08;

struct Fake {
	bool open = false;
	int cap = -1, play = -1;
	unsigned rate = 96000;          /* the stick's after reset */
	int alt = 0;
	uint8_t carrier = CARRIER_AUTO;
	uint8_t input = 0, loopback = LOOP_OFF;
	int8_t tx_gain = 0;
	uint8_t rx_gain = 0;
	uint8_t sent_state = 0;         /* the last event's */
	uint8_t seq = 0;
	bool event_due = false;
	uint8_t regs[2][128] = {};      /* the codec's pages 0 and 1 */
	bool gone = false;
	bool nocodec = false;
	std::deque<int16_t> in;         /* the pair's samples, not yet taken */
	int half = -1;
	unsigned long long ms = 0;      /* IN packets given */
	unsigned long long samples_in = 0, samples_out = 0;
	uint16_t peak_tx = 0;
	std::deque<libusb_transfer *> in_q, out_q, intr_q, cancelled;
	int log = 0;
} fake;

/* the samples flow: alternate setting 1 */
bool streaming()
{
	return fake.alt == 1;
}

/* the stick's state bits */
uint8_t state()
{
	bool lit = fake.carrier == CARRIER_ON ||
		(fake.carrier == CARRIER_AUTO && streaming());
	return (uint8_t)((fake.nocodec ? 0 : ST_CODEC) | (lit ? ST_CARRIER : 0) |
			 (fake.loopback ? ST_LOOPBACK : 0) |
			 (streaming() ? ST_STREAMING : 0));
}

/* an event due if the state has changed since the last one */
void state_changed()
{
	if (state() != fake.sent_state)
		fake.event_due = true;
}

/* the pair's samples that have come (none kept in the firmware's
   loopback: the OUT samples are IN's then) */
void take()
{
	unsigned char b[8192];
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
			if (streaming() && fake.loopback != LOOP_FIRMWARE)
				fake.in.push_back(s);
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
				const uint8_t *b = t->buffer + off;
				for (unsigned k = 0; k + 1 < p.length; k += 2) {
					int16_t s = (int16_t)(b[k] | b[k + 1] << 8);
					uint16_t a = (uint16_t)(s < 0 ? -(int)s : s);
					if (a > fake.peak_tx)
						fake.peak_tx = a;
					if (fake.loopback == LOOP_FIRMWARE &&
					    streaming())
						fake.in.push_back(s);
				}
				if (fake.play >= 0 && p.length &&
				    !fake_net::send_all(fake.play, b, p.length))
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
			fake.sent_state = state();
			uint8_t b[8] = { fake.sent_state, ++fake.seq, 0, 0, 0, 0, 0, 0 };
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
	for (int a = 0; a < 2; a++) {
		alts[a].bInterfaceNumber = 0;
		alts[a].bInterfaceClass = 0xff;
		alts[a].bInterfaceSubClass = 0x57;
		alts[a].bInterfaceProtocol = 0x01;
	}
	alts[1].bAlternateSetting = 1;
	alts[1].bNumEndpoints = 3;
	alts[1].endpoint = endpoints;
	iface.altsetting = alts;
	iface.num_altsetting = 2;
	config.bNumInterfaces = 1;
	config.interface = &iface;
}

/* v little-endian into b, n octets */
void put_le(uint8_t *b, unsigned long long v, int n)
{
	for (int i = 0; i < n; i++)
		b[i] = (uint8_t)(v >> (8 * i));
}

/* a vendor request's reply into data: its length, or LIBUSB_ERROR_PIPE */
int request_in(uint8_t req, uint16_t value, uint16_t index,
	       unsigned char *data, uint16_t length)
{
	uint8_t b[28] = {};
	int n;

	switch (req) {
	case GET_INFO:
		b[0] = 1;                       /* version */
		b[1] = 0; b[2] = 1; b[3] = 0;   /* firmware 0.1.0 */
		put_le(b + 4, fake.rate, 4);
		put_le(b + 8, 240, 2);
		b[10] = state();
		b[11] = 4;                      /* the WB stick */
		b[12] = fake.input;
		b[13] = fake.loopback;
		b[14] = (uint8_t)fake.tx_gain;
		b[15] = fake.rx_gain;
		n = 16;
		break;
	case GET_STATUS:
		put_le(b + 4, fake.peak_tx, 2);
		b[8] = fake.nocodec ? 0 : 0xc4; /* the PGAs at gain, the ADCs up */
		b[9] = fake.nocodec ? 0 : 0xa2; /* the left DAC, HPL, HPR */
		b[10] = state();
		put_le(b + 16, fake.samples_in, 4);
		put_le(b + 20, fake.samples_out, 4);
		fake.peak_tx = 0;
		n = 28;
		break;
	case CODEC_READ:
		if (value > 127 || (index >> 8) > 1)
			return LIBUSB_ERROR_PIPE;
		b[0] = fake.regs[index >> 8][value];
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
int request_out(uint8_t req, uint16_t value, uint16_t index)
{
	switch (req) {
	case SET_RATE:
		if (value != 48 && value != 96 && value != 192)
			return LIBUSB_ERROR_PIPE;
		fake.rate = 1000u * value;
		fake.in.clear();
		return 0;
	case SET_CARRIER:
		if (value > CARRIER_AUTO)
			return LIBUSB_ERROR_PIPE;
		fake.carrier = (uint8_t)value;
		state_changed();
		return 0;
	case SET_TX_GAIN:
		if ((int16_t)value < -127 || (int16_t)value > 48)
			return LIBUSB_ERROR_PIPE;
		fake.tx_gain = (int8_t)(int16_t)value;
		return 0;
	case SET_RX_GAIN:
		if (value > 95)
			return LIBUSB_ERROR_PIPE;
		fake.rx_gain = (uint8_t)value;
		return 0;
	case SET_INPUT:
		if (value > 1)
			return LIBUSB_ERROR_PIPE;
		fake.input = (uint8_t)value;
		return 0;
	case SET_LOOPBACK:
		if (value > LOOP_CODEC)
			return LIBUSB_ERROR_PIPE;
		fake.loopback = (uint8_t)value;
		fake.in.clear();
		state_changed();
		return 0;
	case CODEC_WRITE:
		if ((value >> 8) == 0 || (value >> 8) > 127 || (index >> 8) > 1)
			return LIBUSB_ERROR_PIPE;
		fake.regs[index >> 8][value >> 8] = (uint8_t)value;
		return 0;
	case RESET:
		return 0;
	case REBOOT_UF2:
		fake.gone = true;
		return 0;
	}
	return LIBUSB_ERROR_PIPE;
}

/* the device on libusb's bus (fake_libusb.h) */
struct PicoWb final : fake_usb::Device {
	/* its log on stderr with FAKE_PICO_WB_LOG */
	PicoWb() : Device("FAKE_PICO_WB_LOG") {}

	/* the settings from the environment: the log's, the only one */
	void init() override
	{
		fake.log = log;
	}

	/* the stick's descriptor (1209:0003, USB 2.1: its BOS descriptor) */
	void descriptor(libusb_device_descriptor *d) override
	{
		d->bLength = LIBUSB_DT_DEVICE_SIZE;
		d->bDescriptorType = LIBUSB_DT_DEVICE;
		d->bcdUSB = 0x0210;
		d->idVendor = 0x1209;
		d->idProduct = 0x0003;
		d->bNumConfigurations = 1;
	}

	/* address 9 */
	uint8_t address() const override
	{
		return 9;
	}

	/* the device opened: its connections to wb_pair at FAKE_PICO_WB_SOCKET
	   (LIBUSB_ERROR_BUSY if open, _ACCESS if no socket, _IO if no pair) */
	int open() override
	{
		const char *path = getenv("FAKE_PICO_WB_SOCKET");

		if (fake.open)
			return LIBUSB_ERROR_BUSY;
		if (!path)
			return LIBUSB_ERROR_ACCESS;
		fake.cap = fake_net::connect_exchange(path, 'C');
		fake.play = fake_net::connect_exchange(path, 'P');
		if (fake.cap < 0 || fake.play < 0) {
			fprintf(stderr, "fake pico-serial WB: no pair at %s\n", path);
			for (int fd : { fake.cap, fake.play })
				fake_net::close_fd(fd);
			fake.cap = fake.play = -1;
			return LIBUSB_ERROR_IO;
		}
		fake.nocodec = getenv("FAKE_PICO_WB_NOCODEC") != nullptr;
		fake.open = true;
		return 0;
	}

	/* the connections closed, the device free again */
	void close() override
	{
		for (int fd : { fake.cap, fake.play })
			fake_net::close_fd(fd);
		fake.cap = fake.play = -1;
		fake.open = false;
	}

	/* interface 0's setting alt, 0 or 1: 1 starts the samples (the FIFOs
	   emptied either way), and an event says so */
	int set_alt(int n, int alt) override
	{
		if (fake.log)
			fprintf(stderr, "fake pico-serial WB: alternate setting %d\n", alt);
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

	/* the configuration (make_config()), static */
	libusb_config_descriptor *config() override
	{
		make_config();
		return &::config;
	}

	/* a vendor request to interface 0 (wIndex's low octet; its high one a
	   codec page) or the device: request_in() or request_out();
	   LIBUSB_ERROR_NO_DEVICE once gone */
	int control(uint8_t type, uint8_t request, uint16_t value,
		    uint16_t index, unsigned char *data,
		    uint16_t length) override
	{
		int r;

		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		if ((type & LIBUSB_REQUEST_TYPE_VENDOR) != LIBUSB_REQUEST_TYPE_VENDOR ||
		    (index & 0xff) != 0)
			return LIBUSB_ERROR_PIPE;
		r = type & LIBUSB_ENDPOINT_IN ?
			request_in(request, value, index, data, length) :
			request_out(request, value, index);
		if (fake.log)
			fprintf(stderr, "fake pico-serial WB: request %02x %s %u: %d\n",
				request, type & LIBUSB_ENDPOINT_IN ? "in" : "out", value, r);
		return r;
	}

	/* t queued at its endpoint, done in run() */
	int submit(libusb_transfer *t) override
	{
		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		if (fake_usb::refused(t))
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
	static PicoWb d;
	return d;
}
