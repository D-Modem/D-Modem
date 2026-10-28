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
 *    each on the interrupt endpoint.  On hook the codec hears the line
 *    only with GPIO 5 (CID1) set, as caller ID wants.
 *
 *    The samples flow once the codec is powered up and the FIFO counter
 *    set (SSI1_CONTROL, SSI1_COUNTER): each IN transfer done takes 1 ms
 *    of the line's samples a packet, as they come from the exchange; an
 *    OUT transfer's samples go to the exchange, and it is done, at once.
 *    The capture socket is libusb's descriptor to poll.  The device is
 *    gone (LIBUSB_TRANSFER_NO_DEVICE) when the exchange is.
 *
 *    FAKE_ST7554_DAA=1: the GPIO status says an ST75951/2 (silicon DAA),
 *    whose control words (register 0x17) name one of its 4 registers in
 *    bits 12 to 15: register 0 has the rate; it streams with SSI1_CONTROL
 *    bit 0 set and its start-up done (register 3 = 0x050, SSI1_CONTROL
 *    bit 1).
 *    FAKE_ST7554_LOG=1: the register writes on stderr.
 *    FAKE_ST7554_SUSPEND=<file>: while the file is there the device is
 *    suspended (the computer asleep): its transfers fail
 *    (LIBUSB_TRANSFER_ERROR, then LIBUSB_ERROR_IO), so do its requests,
 *    and a ring with its remote wakeup armed (SET_FEATURE) and the ring
 *    detector's interrupt enabled says "remote wakeup" (once); when the
 *    file has gone it resumes reset (its registers 0, the remote wakeup
 *    cleared).  Its configuration says it can wake.
 *    FAKE_ST7554_HANDSET=<file>: the handset detector (GPIO 2) says off
 *    hook while the file is there; its change an interrupt if enabled.
 *    FAKE_ST7554_VOICE=<file>: a voice codec on SSI2, a handset's: the
 *    configuration has alternate settings 2 (its isochronous endpoints)
 *    and 3 (the codec's and its: five endpoints); it streams once
 *    powered up (SSI2_CONTROL 0x21) and its counter set, 8000 octets a
 *    second of mu-law, its IN transfers kept in step with the codec's.
 *    The microphone's is a 1000 Hz tone; the earpiece's octets go to the
 *    file, as they come.
 *    FAKE_ST7554_REVISION=<hex>: the chip's revision (register 0x00),
 *    0x0200 without it.
 *    FAKE_USB_NO_ISO, FAKE_USB_USBDK: fake_usb_backend.h (Windows 7's
 *    and 8's drivers).
 *
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_net.h"

#include <chrono>
#include <deque>
#include <vector>

#include <libusb.h>

#include <g711.h>

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
/* the voice codec's (FAKE_ST7554_VOICE), its packets' octets a ms, its
   microphone's tone */
const uint8_t EP_VOUT = 0x04, EP_VIN = 0x85;
const uint16_t VOICE_PACKET = 16;
const unsigned VOICE_PER_MS = 8, MIC_HZ = 1000, MIC_LEVEL = 8000;
const uint16_t MAX_PACKET = 32;
const uint16_t GPIO_RING1 = 0x0001, GPIO_HANDSET = 0x0004, GPIO_HOOK1 = 0x0008,
	GPIO_CID1 = 0x0020;
const uint16_t GPIO_MONOLITHIC = 0x4000;
const int RING_LEVEL = 8000;

libusb_device the_device;

struct Fake {
	libusb_context *ctx = nullptr;
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	uint16_t regs[0x20] = {};
	unsigned rate = 0;              /* from the control word */
	bool st75951 = false;           /* FAKE_ST7554_DAA */
	uint16_t cw[4] = {};            /* the ST75951's registers */
	bool hook = false;              /* off hook */
	bool ring_state = false;
	unsigned ring_changes = 0;      /* not yet on the interrupt */
	bool gone = false;
	std::deque<int16_t> in;         /* the line's samples, not yet taken */
	int half = -1;
	unsigned long long ms = 0;      /* IN packets given */
	std::deque<libusb_transfer *> in_q, out_q, intr_q, cancelled;
	int log = 0;
	/* FAKE_ST7554_SUSPEND, FAKE_ST7554_HANDSET's files, their states */
	const char *suspend_file = nullptr, *handset_file = nullptr;
	bool wakeup = false, suspended = false, woke = false;
	uint16_t events = 0;            /* GPIO events not yet on the interrupt */
	/* FAKE_ST7554_UNDERRUNS=<n>: n playback FIFO underruns on the
	   interrupt from 3 s into the stream, 15 ms apart, the ring
	   detector's bit set in each, as the SmartUSB56 on a phone has it
	   (no ring) */
	unsigned underruns = 0;
	unsigned long long underrun_at = 3000;  /* the next one's ms */
	/* FAKE_ST7554_VOICE's: the earpiece's file, the voice transfers
	   queued, the voice IN packets given, the microphone's samples */
	FILE *ear = nullptr;
	std::deque<libusb_transfer *> vin_q, vout_q;
	unsigned long long vms = 0, mic_n = 0;
} fake;

/* the files of FAKE_ST7554_SUSPEND and _HANDSET looked at again: every
   100 ms (a file's open is slow under Wine) */
bool look_again()
{
	using namespace std::chrono;
	static steady_clock::time_point last;
	steady_clock::time_point now = steady_clock::now();
	if (now - last < milliseconds(100))
		return false;
	last = now;
	return true;
}

/* the file path is there */
bool there(const char *path)
{
	FILE *f = path ? fopen(path, "r") : nullptr;
	if (f)
		fclose(f);
	return f != nullptr;
}

/* a connection to the exchange (fake_net.h) */
int connect_exchange(const char *path, char kind)
{
	return fake_net::connect_exchange(path, kind);
}

/* switch which (0 the hook, 1 caller ID) to the exchange
   (the device gone if it can't) */
void switch_to(uint8_t which, bool on)
{
	unsigned char m[2] = { which, (unsigned char)on };
	if (fake.ctl >= 0 && !fake_net::send_all(fake.ctl, m, 2))
		fake.gone = true;
}

/* the samples flow: the codec powered up, the FIFO
   counter set, a rate known (an ST75951 started up too) */
bool streaming()
{
	if (fake.st75951 && (!(fake.regs[0x01] & 2) || fake.cw[3] != 0x050))
		return false;
	return fake.regs[0x01] & 1 && fake.regs[0x04] && fake.rate;
}

/* the voice codec streams: powered up, its counter set */
bool voice_streaming()
{
	return fake.ear && fake.regs[0x02] == 0x21 && fake.regs[0x06];
}

/* the microphone's next sample, in mu-law */
uint8_t mic_sample()
{
	double t = (double)fake.mic_n++ * MIC_HZ / 8000.0;
	return dsp::g711::lin2ulaw((int)(MIC_LEVEL * sin(2 * M_PI * t)));
}

/* the rate of a control word (the STLC7550's, the ST75951's register 0);
   0 if none */
unsigned cword_rate(uint16_t v)
{
	return v == 0x3c8 ? 8000 : v == 0x3c0 ? 9600 : v == 0x3f0 ? 16000 : 0;
}

/* a register written (a vendor request) */
void set_reg(uint8_t reg, uint16_t v)
{
	if (fake.log)
		fprintf(stderr, "fake st7554: %02x = %04x\n", reg, v);
	if (reg >= 0x20)
		return;
	fake.regs[reg] = v;
	if (reg == 0x17 && fake.st75951) {
		fake.cw[(v >> 12) & 3] = v & 0x4fff;
		if (!(v >> 12))
			fake.rate = cword_rate(v & 0xfff);
	} else if (reg == 0x17) {
		fake.rate = cword_rate(v);
	}
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
				if (fake.suspended) {
					if (fake.wakeup && !fake.woke &&
					    fake.regs[0x0c] & GPIO_RING1) {
						fake.woke = true;
						fprintf(stderr, "fake st7554: remote "
							"wakeup\n");
					}
				} else if (fake.regs[0x0c] & GPIO_RING1) {
					fake.ring_changes++;
				}
			}
			if (streaming() && !fake.suspended)
				fake.in.push_back(fake.hook || fake.regs[0x0a] &
						  GPIO_CID1 ? s : 0);
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
	bool look = (fake.suspend_file || fake.handset_file) && look_again();
	bool s = look ? there(fake.suspend_file) : fake.suspended;
	if (s && !fake.suspended) {
		fprintf(stderr, "fake st7554: suspended\n");
		fake.suspended = true;
		fake.woke = false;
		std::deque<libusb_transfer *> all;
		for (auto *q : { &fake.in_q, &fake.out_q, &fake.intr_q,
				 &fake.vin_q, &fake.vout_q }) {
			all.insert(all.end(), q->begin(), q->end());
			q->clear();
		}
		for (libusb_transfer *t : all)
			done(t, LIBUSB_TRANSFER_ERROR);
	} else if (!s && fake.suspended) {
		fprintf(stderr, "fake st7554: resumed (reset)\n");
		fake.suspended = false;
		uint16_t status = fake.regs[0x15], rev = fake.regs[0x00];
		memset(fake.regs, 0, sizeof(fake.regs));
		memset(fake.cw, 0, sizeof(fake.cw));
		fake.regs[0x15] = status;
		fake.regs[0x00] = rev;
		fake.rate = 0;
		fake.wakeup = false;
		fake.ring_changes = 0;
		fake.events = 0;
		fake.in.clear();
		if (fake.hook) {
			fake.hook = false;
			switch_to(0, false);
		}
	}
	bool h = look ? there(fake.handset_file) :
		(bool)(fake.regs[0x15] & GPIO_HANDSET);
	if (h != (bool)(fake.regs[0x15] & GPIO_HANDSET)) {
		fake.regs[0x15] = (uint16_t)(h ? fake.regs[0x15] | GPIO_HANDSET :
					     fake.regs[0x15] & ~GPIO_HANDSET);
		if (fake.regs[0x0c] & GPIO_HANDSET)
			fake.events |= GPIO_HANDSET;
	}
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
			for (auto *q : { &fake.in_q, &fake.out_q, &fake.intr_q,
					 &fake.vin_q, &fake.vout_q }) {
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
		/* the earpiece's octets to its file; the microphone's as
		   the codec's samples have come (its ms) */
		while (!fake.vout_q.empty()) {
			libusb_transfer *t = fake.vout_q.front();
			fake.vout_q.pop_front();
			int off = 0;
			for (int i = 0; i < t->num_iso_packets; i++) {
				libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
				if (voice_streaming() && p.length)
					fwrite(t->buffer + off, 1, p.length, fake.ear);
				p.actual_length = p.length;
				p.status = LIBUSB_TRANSFER_COMPLETED;
				off += (int)p.length;
			}
			fflush(fake.ear);
			t->actual_length = off;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		if (!fake.vin_q.empty() && voice_streaming() &&
		    fake.vms + (unsigned)fake.vin_q.front()->num_iso_packets <= fake.ms) {
			libusb_transfer *t = fake.vin_q.front();
			fake.vin_q.pop_front();
			int off = 0;
			for (int i = 0; i < t->num_iso_packets; i++) {
				libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
				unsigned n = VOICE_PER_MS < p.length ? VOICE_PER_MS : p.length;
				for (unsigned k = 0; k < n; k++)
					t->buffer[off + k] = mic_sample();
				p.actual_length = n;
				p.status = LIBUSB_TRANSFER_COMPLETED;
				off += (int)p.length;
				fake.vms++;
			}
			t->actual_length = off;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
		while ((fake.ring_changes || fake.events ||
			(fake.underruns && fake.ms >= fake.underrun_at)) && !fake.intr_q.empty()) {
			libusb_transfer *t = fake.intr_q.front();
			fake.intr_q.pop_front();
			uint16_t ev = fake.events, fifo = 0;
			fake.events = 0;
			if (fake.ring_changes) {
				fake.ring_changes--;
				ev |= GPIO_RING1;
			} else if (fake.underruns && fake.ms >= fake.underrun_at) {
				fake.underruns--;
				fake.underrun_at = fake.ms + 15;
				fifo = 0x0040;  /* SSI1's underrun */
				ev |= GPIO_RING1;
			}
			uint8_t b[4] = { (uint8_t)fifo, (uint8_t)(fifo >> 8),
					 (uint8_t)ev, (uint8_t)(ev >> 8) };
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
   and 1 (interrupt IN, isochronous OUT and IN); with the voice codec 2
   (interrupt IN, its OUT and IN) and 3 (the five) */
libusb_endpoint_descriptor endpoints[5], voice_endpoints[3];
libusb_interface_descriptor alts[4];
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
	config.bmAttributes = 0xa0;     /* bus powered, remote wakeup */
	alts[0].bInterfaceNumber = 0;
	alts[1].bInterfaceNumber = 0;
	alts[1].bAlternateSetting = 1;
	alts[1].bNumEndpoints = 3;
	alts[1].endpoint = endpoints;
	endpoints[3] = endpoints[1];
	endpoints[3].bEndpointAddress = EP_VOUT;
	endpoints[3].wMaxPacketSize = VOICE_PACKET;
	endpoints[4] = endpoints[2];
	endpoints[4].bEndpointAddress = EP_VIN;
	endpoints[4].wMaxPacketSize = VOICE_PACKET;
	voice_endpoints[0] = endpoints[0];
	voice_endpoints[1] = endpoints[3];
	voice_endpoints[2] = endpoints[4];
	alts[2].bInterfaceNumber = 0;
	alts[2].bAlternateSetting = 2;
	alts[2].bNumEndpoints = 3;
	alts[2].endpoint = voice_endpoints;
	alts[3].bInterfaceNumber = 0;
	alts[3].bAlternateSetting = 3;
	alts[3].bNumEndpoints = 5;
	alts[3].endpoint = endpoints;
	iface.altsetting = alts;
	iface.num_altsetting = fake.ear ? 4 : 2;
	config.bNumInterfaces = 1;
	config.interface = &iface;
}

} // namespace

extern "C" {

#include "fake_usb_backend.h"

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

/* full speed (only the Agere line asks; on Windows this replaces
   libusb, which would answer otherwise) */
int LIBUSB_CALL libusb_get_device_speed(libusb_device *)
{
	return LIBUSB_SPEED_FULL;
}

/* its ports from the root: port 250, which no real hub has (so that
   the kernel's power/wakeup of it is no real device's) */
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *, uint8_t *ports,
					int len)
{
	if (len < 1)
		return LIBUSB_ERROR_OVERFLOW;
	ports[0] = 250;
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
			fake_net::close_fd(fd);
		fake.cap = fake.play = fake.ctl = -1;
		return LIBUSB_ERROR_IO;
	}
	switch_to(1, true);             /* the ring signal heard */
	fake.st75951 = getenv("FAKE_ST7554_DAA") != nullptr;
	fake.suspend_file = getenv("FAKE_ST7554_SUSPEND");
	fake.handset_file = getenv("FAKE_ST7554_HANDSET");
	if (const char *u = getenv("FAKE_ST7554_UNDERRUNS"))
		fake.underruns = (unsigned)atoi(u);
	if (const char *v = getenv("FAKE_ST7554_VOICE")) {
		fake.ear = fopen(v, "wb");
		if (!fake.ear)
			fprintf(stderr, "fake st7554: %s: %s\n", v, strerror(errno));
	}
	if (fake.st75951)
		fake.regs[0x15] = GPIO_MONOLITHIC;
	const char *rev = getenv("FAKE_ST7554_REVISION");
	fake.regs[0x00] = (uint16_t)(rev ? strtoul(rev, nullptr, 16) : 0x0200);
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
	if (fake.ear)
		fclose(fake.ear);
	fake.ear = nullptr;
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

/* interface 0's setting alt, 0 or 1 (to 3 with the voice codec) */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *,
						 int n, int alt)
{
	int most = fake.ear ? 3 : 1;
	if (n == 0 && alt >= 0 && alt <= most) {
		if (fake.log)
			fprintf(stderr, "fake st7554: alternate setting %d\n", alt);
		return 0;
	}
	return LIBUSB_ERROR_NOT_FOUND;
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
	if (fake.suspended)
		return LIBUSB_ERROR_IO;
	/* USB's SET_FEATURE and CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP) */
	if (type == (LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE) &&
	    (request == LIBUSB_REQUEST_SET_FEATURE ||
	     request == LIBUSB_REQUEST_CLEAR_FEATURE) && value == 1) {
		fake.wakeup = request == LIBUSB_REQUEST_SET_FEATURE;
		fprintf(stderr, "fake st7554: remote wakeup %s\n",
			fake.wakeup ? "armed" : "cleared");
		return 0;
	}
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
	if (fake.suspended)
		return LIBUSB_ERROR_IO;
	if (fake_usb_refused(t))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	if (t->endpoint == EP_INTR)
		fake.intr_q.push_back(t);
	else if (t->endpoint == EP_IN)
		fake.in_q.push_back(t);
	else if (t->endpoint == EP_OUT)
		fake.out_q.push_back(t);
	else if (t->endpoint == EP_VIN && fake.ear)
		fake.vin_q.push_back(t);
	else if (t->endpoint == EP_VOUT && fake.ear)
		fake.vout_q.push_back(t);
	else
		return LIBUSB_ERROR_NOT_FOUND;
	return 0;
}

/* t off its queue, done as cancelled in run();
   LIBUSB_ERROR_NOT_FOUND if not queued */
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer *t)
{
	for (auto *q : { &fake.in_q, &fake.out_q, &fake.intr_q, &fake.vin_q,
			 &fake.vout_q })
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
