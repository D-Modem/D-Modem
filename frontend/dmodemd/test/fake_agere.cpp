/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_agere.cpp - an Agere USB 2.0 soft modem for the tests: the part
 *                     of libusb-1.0 that usb_line.cpp and usb_agere.cpp
 *                     use (LD_PRELOAD in front of the real one), with one
 *                     such modem on the bus whose telephone line is
 *                     line_exchange's
 *
 *    What it does is what docs/agere-usb.md says the device does, as far
 *    as dmodemd sees it: USB 047e:2892 at high speed, interface 0's
 *    alternate setting 1 with an isochronous OUT (0x02) and IN (0x82)
 *    endpoint of 16 octets; vendor commands (bRequest) on endpoint 0 for
 *    its ROM's version (1), the ARM's memory (2 read, 3 write: the
 *    address's high half the value, its low half the index), the codec's
 *    registers (0x0a, 0x0b), the ROM's stream (0x0c with its 8 octets as
 *    Windows' driver sends them, 0x0d) and the clock (0x12).  No patch is
 *    wanted: command 9 stalls.  The ROM's function table at 0x70000a64 is
 *    as the ROM has it at boot; with an entry of a patch's in it (one left
 *    by an earlier driver: FAKE_AGERE_PATCHED) the stream gives nothing.
 *
 *    FAKE_AGERE_SOCKET is the exchange's socket of the line.  Opening the
 *    device connects to it as fake_modem_card.cpp does ('C' the line's
 *    samples, 'P' the modem's, 'K' the switches), its line monitor on:
 *    the codec hears the line on hook.  The hook is codec register 0xc0
 *    (a Mercury's 0 off hook; a Polaris's 1).  The line's samples (8000 Hz) pace the
 *    device: each 2 of them a packet's time (250 us, a packet every 2
 *    microframes).  In each, the next OUT packet queued is taken (its
 *    header's count of samples to the exchange) and, streaming, answered
 *    with an IN packet of the samples come since (in a ring of 32; at
 *    most 10), after their count (0xaa55 if none) and a status word whose
 *    bit 0 is set while the ring signal is there.  A packet's time with
 *    no OUT packet queued is lost.  The capture socket is libusb's
 *    descriptor to poll.  The device is gone (LIBUSB_TRANSFER_NO_DEVICE)
 *    when the exchange is.
 *
 *    FAKE_AGERE_ROM=<hex>: the ROM's version (command 1), 0x121 without
 *    it; 0x106 has no command 0x12 (a stall).
 *    FAKE_AGERE_POLARIS=1: a Polaris DAA (register 0xc0's bit 15 set; the
 *    hook 1 off; its samples 0x8000 until its link is pulsed up; its DAA's
 *    registers through 0xc8 and 0xcc), as on the Hiro H50113.
 *    FAKE_AGERE_FULL=1: a full speed device.
 *    FAKE_AGERE_PATCHED=1: a bulk patch's entries 4 and 7 in the function
 *    table, as an earlier driver left them.
 *    The OH LED (at 0x30000008: GPIO 4, lit while set, as on the Hiro
 *    H50113) is said on stderr as it goes on and off.
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

#include "fake_libusb.h"

namespace {

const uint8_t EP_OUT = 0x02, EP_IN = 0x82;
const uint8_t BULK_OUT = 0x01, BULK_IN = 0x81, INTR_IN = 0x82;
const uint16_t ISO_PACKET = 16, BULK_PACKET = 512;
const int RING_LEVEL = 8000;
const uint32_t FUNCTABLE = 0x70000a64, USB_SPEED = 0x90000018;
const uint32_t GPIO_OUT = 0x30000008;
/* the ROM's function table at boot */
const uint32_t ROM_TABLE[8] = { 0x14af, 0x14af, 0x221c, 0x14af,
				0x14b1, 0x14af, 0x14af, 0x14af };
/* a packet's time, in the line's samples; the receive ring; the
   samples an IN packet, at most; the ROM's "none" */
const unsigned SLOT = 2, RING = 32, IN_MOST = 10, TX_RING = 48, TX_LEAD = 24;
const uint16_t NONE = 0xaa55;
/* command 0x0c's data, as Windows' driver sends it at high speed */
const uint16_t START[4] = { 2, 0, 24, 0x2aaa };

struct Fake {
	bool open = false;
	int cap = -1, play = -1, ctl = -1;
	int alt = 0;
	uint16_t rom = 0x121;
	bool polaris = false, full = false, patched = false;
	bool hook = false;              /* off hook */
	bool link = false;              /* a Polaris's link up */
	std::map<uint8_t, uint8_t> daa; /* a Polaris's DAA registers */
	bool streaming = false;         /* command 0x0c, not 0x0d since */
	bool said_stuck = false;
	int led = -1;        /* the LEDs lit (-1 not yet written) */
	std::map<uint32_t, uint8_t> mem;
	std::map<uint16_t, uint16_t> codec;
	bool gone = false;
	std::deque<int16_t> in;         /* the receive ring */
	bool ringing = false;           /* the ring signal on what came */
	/* the samples come, and the last that rang (a Polaris's 0x0a8 bit 1
	   for 100 ms after) */
	unsigned long long samples = 0, rang = 0;
	/* FAKE_AGERE_RING_STICKS: a Polaris's 0x0a8 bit 1 left set for 10 s
	   after a call, as the first device's (no ring in 0x0c4); until when;
	   0x0c4's ring halves (alternating each read while it rings) */
	bool sticks = false;
	unsigned long long stuck_until = 0;
	unsigned ac = 0;
	unsigned since = 0;             /* the samples come since a slot */
	/* the transmit ring (48, a lead of 24 at 0x0c), the codec playing
	   from it each slot what came in it; a slot every SLOT samples, or
	   one more or fewer as FAKE_AGERE_DRIFT (ppm: the codec's clock
	   faster than the USB's) has it; the ring's overruns and underruns */
	std::deque<int16_t> tx;
	unsigned slot_len = SLOT;
	long drift = 0, drift_acc = 0;
	unsigned overruns = 0, underruns = 0;
	int half = -1;
	std::deque<libusb_transfer *> in_q, out_q, cancelled;
	std::map<libusb_transfer *, int> next;   /* a transfer's next packet */
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

/* the function table as the ROM has it */
bool table_as_rom()
{
	for (uint32_t i = 0; i < 8; i++)
		if (mem32(FUNCTABLE + 4 * i) != ROM_TABLE[i])
			return false;
	return true;
}

/* the ARM's memory and the codec after reset: the ROM's function table
   (a bulk patch's entries in it with FAKE_AGERE_PATCHED), the PLL locked,
   the clock's divider, the USB speed, the codec ready */
void reset()
{
	fake.mem.clear();
	fake.codec.clear();
	for (uint32_t i = 0; i < 8; i++)
		set32(FUNCTABLE + 4 * i, ROM_TABLE[i]);
	if (fake.patched) {
		set32(FUNCTABLE + 16, 0x70001588);
		set32(FUNCTABLE + 28, 0x70001598);
	}
	set16(0xcd000008, 1);
	set16(0xcd00000c, 3);
	set16(0xcd000030, 0x000f);
	set16(USB_SPEED, fake.full ? 0 : 1);
	fake.codec[0x2dc] = 1;
	fake.codec[0xc0] = 1;
	/* (a Polaris: 0xc0's bit 15 set, its link down, a calibration
	   reading of 10 Gray coded, its DAA's revision 3) */
	fake.link = false;
	fake.daa.clear();
	if (fake.polaris) {
		fake.codec[0xc0] = 0x8000;
		fake.codec[0xdc] = 15 << 10;
		fake.daa[0x16] = 3;
		fake.daa[0x17] = 0x5a;
	}
	fake.streaming = false;
	fake.led = -1;
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

void tick();
void play(unsigned n);

/* the exchange's samples that have come: into the receive ring while
   streaming (its oldest lost when it is full), the ring signal on them;
   a packet's time (tick()) for each SLOT of them */
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
			fake.samples++;
			/* (a Polaris's link down: its data register's 0x8000) */
			if (fake.polaris && !fake.link)
				s = -32768;
			if (fake.streaming) {
				fake.in.push_back(s);
				if (fake.in.size() > RING)
					fake.in.pop_front();
				if (!fake.hook && (s > RING_LEVEL || s < -RING_LEVEL))
				{
					fake.ringing = true;
					fake.rang = fake.samples;
				}
			}
			/* (a packet's time each SLOT samples, as they come; one
			   more or fewer now and then, FAKE_AGERE_DRIFT) */
			if (++fake.since >= fake.slot_len) {
				unsigned played = fake.since;
				fake.since = 0;
				tick();
				play(played);
				fake.slot_len = SLOT;
				fake.drift_acc += fake.drift * (long)SLOT;
				if (fake.drift_acc >= 1000000) {
					fake.drift_acc -= 1000000;
					fake.slot_len = SLOT + 1;
				} else if (fake.drift_acc <= -1000000) {
					fake.drift_acc += 1000000;
					fake.slot_len = SLOT - 1;
				}
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

/* t's next packet, and whether it has one (its cursor moved on) */
bool next_packet(libusb_transfer *t, unsigned char **buf, int *i)
{
	int k = fake.next[t];
	if (k >= t->num_iso_packets)
		return false;
	unsigned off = 0;
	for (int j = 0; j < k; j++)
		off += t->iso_packet_desc[j].length;
	*buf = t->buffer + off;
	*i = k;
	fake.next[t] = k + 1;
	return true;
}

/* a packet's time: the next OUT packet's samples to the exchange; then,
   streaming, an IN packet into the next IN transfer of what the ring
   has (kept if no IN transfer is queued); false if no OUT packet was
   queued (the time lost) */
bool slot()
{
	if (fake.out_q.empty())
		return false;
	libusb_transfer *t = fake.out_q.front();
	unsigned char *b;
	int i;
	if (!next_packet(t, &b, &i))
		return false;
	unsigned len = t->iso_packet_desc[i].length;
	unsigned n = len >= 4 ? (unsigned)(b[0] | b[1] << 8) : 0;
	if (n > (len - 4) / 2)
		n = (len - 4) / 2;
	/* (into the transmit ring: one too many written over its oldest) */
	if (fake.streaming)
		for (unsigned j = 0; j < n; j++) {
			fake.tx.push_back((int16_t)(b[4 + 2 * j] | b[5 + 2 * j] << 8));
			if (fake.tx.size() > TX_RING) {
				fake.tx.pop_front();
				if (fake.overruns++ < 3)
					fprintf(stderr, "fake Agere: transmit ring overrun\n");
			}
		}
	t->iso_packet_desc[i].actual_length = len;
	t->iso_packet_desc[i].status = LIBUSB_TRANSFER_COMPLETED;
	if (fake.next[t] == t->num_iso_packets) {
		fake.out_q.pop_front();
		fake.next.erase(t);
		t->actual_length = t->length;
		done(t, LIBUSB_TRANSFER_COMPLETED);
	}
	if (!fake.streaming || fake.in_q.empty())
		return true;
	libusb_transfer *r = fake.in_q.front();
	if (!next_packet(r, &b, &i))
		return true;
	unsigned k = fake.in.size() < IN_MOST ? (unsigned)fake.in.size() : IN_MOST;
	unsigned room = (r->iso_packet_desc[i].length - 4) / 2;
	if (k > room)
		k = room;
	uint16_t count = k ? (uint16_t)k : NONE;
	b[0] = (uint8_t)count;
	b[1] = (uint8_t)(count >> 8);
	/* (a Polaris's ring detector isn't in it: its 0x0a8's bit 1 is) */
	b[2] = fake.ringing && !fake.polaris ? 1 : 0;
	b[3] = 0;
	fake.ringing = false;
	for (unsigned j = 0; j < k; j++) {
		int16_t s = fake.in.front();
		fake.in.pop_front();
		b[4 + 2 * j] = (uint8_t)s;
		b[5 + 2 * j] = (uint8_t)(s >> 8);
	}
	r->iso_packet_desc[i].actual_length = 4 + 2 * (k ? k : 2);
	r->iso_packet_desc[i].status = LIBUSB_TRANSFER_COMPLETED;
	if (fake.next[r] == r->num_iso_packets) {
		fake.in_q.pop_front();
		fake.next.erase(r);
		done(r, LIBUSB_TRANSFER_COMPLETED);
	}
	return true;
}

/* what the device has done since: the cancelled transfers' callbacks,
   then the line's samples come, and with them each packet's time
   (tick()), whose callbacks may submit more */
void run()
{
	while (!fake.cancelled.empty()) {
		libusb_transfer *t = fake.cancelled.front();
		fake.cancelled.pop_front();
		done(t, LIBUSB_TRANSFER_CANCELLED);
	}
	if (fake.gone) {
		std::deque<libusb_transfer *> all;
		for (auto *q : { &fake.in_q, &fake.out_q }) {
			all.insert(all.end(), q->begin(), q->end());
			q->clear();
		}
		fake.next.clear();
		for (libusb_transfer *t : all)
			done(t, LIBUSB_TRANSFER_NO_DEVICE);
		return;
	}
	if (fake.streaming && !table_as_rom() && !fake.said_stuck) {
		fprintf(stderr, "fake Agere: streaming with a patch's entry in "
			"the function table: no samples\n");
		fake.said_stuck = true;
	}
	take();
}

/* a packet's time (take()'s): slot(), unless a patch's entry in the
   function table takes the samples */
void tick()
{
	if (fake.gone)
		return;
	if (fake.streaming && !table_as_rom())
		fake.in.clear();
	else
		slot();
}

/* the codec's n samples played from the transmit ring to the exchange,
   streaming (silence for any it hasn't) */
void play(unsigned n)
{
	if (!fake.streaming || fake.play < 0 || fake.gone)
		return;
	unsigned char b[2 * (SLOT + 1)];
	for (unsigned j = 0; j < n; j++) {
		int16_t x = 0;
		if (!fake.tx.empty()) {
			x = fake.tx.front();
			fake.tx.pop_front();
		} else if (fake.underruns++ < 3) {
			fprintf(stderr, "fake Agere: transmit ring underrun\n");
		}
		b[2 * j] = (uint8_t)x;
		b[2 * j + 1] = (uint8_t)(x >> 8);
	}
	if (!fake_net::send_all(fake.play, b, n * 2))
		fake.gone = true;
}

/* the configuration: interface 0, alternate settings 0 (no endpoints),
   1 (isochronous OUT and IN, a packet every 2 microframes), 2 (bulk OUT
   and IN, an interrupt IN) and 3 (isochronous, every microframe) */
libusb_endpoint_descriptor iso1[2], bulk2[3], iso3[2];
libusb_interface_descriptor alts[4];
libusb_interface iface;
libusb_config_descriptor config;

/* an endpoint's descriptor */
void endpoint(libusb_endpoint_descriptor &e, uint8_t addr, uint8_t type,
	      uint16_t size, uint8_t interval)
{
	e.bEndpointAddress = addr;
	e.bmAttributes = type;
	e.wMaxPacketSize = size;
	e.bInterval = interval;
}

/* the configuration's descriptors filled in */
void make_config()
{
	endpoint(iso1[0], EP_IN, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, ISO_PACKET, 2);
	endpoint(iso1[1], EP_OUT, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, ISO_PACKET, 2);
	endpoint(bulk2[0], BULK_IN, LIBUSB_TRANSFER_TYPE_BULK, BULK_PACKET, 1);
	endpoint(bulk2[1], BULK_OUT, LIBUSB_TRANSFER_TYPE_BULK, BULK_PACKET, 1);
	endpoint(bulk2[2], INTR_IN, LIBUSB_TRANSFER_TYPE_INTERRUPT, 16, 16);
	endpoint(iso3[0], EP_IN, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, ISO_PACKET, 1);
	endpoint(iso3[1], EP_OUT, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, ISO_PACKET, 1);
	for (int i = 0; i < 4; i++) {
		alts[i].bInterfaceNumber = 0;
		alts[i].bInterfaceClass = 0xff;
		alts[i].bAlternateSetting = (uint8_t)i;
	}
	alts[1].bNumEndpoints = 2;
	alts[1].endpoint = iso1;
	alts[2].bNumEndpoints = 3;
	alts[2].endpoint = bulk2;
	alts[3].bNumEndpoints = 2;
	alts[3].endpoint = iso3;
	iface.altsetting = alts;
	iface.num_altsetting = 4;
	config.bConfigurationValue = 2;
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
	case 0x0a: {
		if (length < 2)
			return LIBUSB_ERROR_PIPE;
		uint16_t v = fake.codec[index];
		/* (a Polaris ringing: 0x0a8's bit 1, as the first device's) */
		bool rings = fake.rang && fake.samples - fake.rang < 800;
		if (fake.polaris && index == 0xa8) {
			v = 0x0010;
			if (rings || fake.samples < fake.stuck_until)
				v |= 0x0002;
		}
		if (fake.polaris && index == 0xc4 && rings)
			v = (uint16_t)(0x000c | (++fake.ac & 1 ? 0x80 : 0x40));
		/* (the line settling after a call: bit 6 alone, as the first
		   device's, FAKE_AGERE_RING_STICKS) */
		else if (fake.polaris && index == 0xc4 && fake.samples < fake.stuck_until)
			v = (uint16_t)(0x000c | (++fake.ac & 1 ? 0x40 : 0));
		data[0] = (uint8_t)v;
		data[1] = (uint8_t)(v >> 8);
		return 2;
	}
	}
	return LIBUSB_ERROR_PIPE;
}

/* a vendor command with data out (data, length octets): 0, or
   LIBUSB_ERROR_PIPE (a stall) */
int command_out(uint8_t cmd, uint16_t index, uint16_t value,
		const unsigned char *data, uint16_t length)
{
	uint32_t a = (uint32_t)value << 16 | index;
	switch (cmd) {
	case 0x03: {
		for (uint16_t i = 0; i < length; i++)
			fake.mem[a + i] = data[i];
		/* (the ROM's ring indices, as the drivers write them off hook) */
		if (a == 0x70001020 && length == 8)
			fprintf(stderr, "fake Agere: rings %u %u\n",
				data[0] | data[1] << 8 | data[2] << 16 |
					(unsigned)data[3] << 24,
				data[4] | data[5] << 8 | data[6] << 16 |
					(unsigned)data[7] << 24);
		/* (the OH LED: GPIO 4, lit while set; said as it changes) */
		if (a <= GPIO_OUT && a + length > GPIO_OUT) {
			int lit = (fake.mem[GPIO_OUT] & 0x10) != 0;
			if (lit != fake.led) {
				fake.led = lit;
				fprintf(stderr, "fake Agere: LED OH %s\n",
					lit ? "on" : "off");
			}
		}
		return 0;
	}
	case 0x09:
		fprintf(stderr, "fake Agere: a patch (command 9, slot %u): not "
			"wanted\n", index);
		return LIBUSB_ERROR_PIPE;
	case 0x0b:
		fake.codec[index] = value;
		/* (a Polaris: its link up once its line side is pulsed; its
		   DAA's registers through 0xc8, read back in 0xcc) */
		if (fake.polaris && index == 0xd0 && value == 0x17d2 && !fake.link) {
			fake.link = true;
			fake.codec[0xc4] = 4;
			fprintf(stderr, "fake Agere: Polaris link up\n");
		}
		if (fake.polaris && index == 0xc8) {
			uint8_t r = (uint8_t)(value >> 8 & 0x7f);
			if (value & 0x8000)
				fake.daa[r] = (uint8_t)value;
			else
				fake.codec[0xcc] = (uint16_t)(r << 8 | fake.daa[r]);
		}
		if (fake.polaris && index == 0xc0)
			fake.codec[0xc0] = (uint16_t)(0x8000 | value);
		if (index == 0xc0 && (fake.polaris ? value == 1 : value == 0) != fake.hook) {
			fake.hook = fake.polaris ? value == 1 : value == 0;
			/* (on hook after a call: the ring bit stuck, FAKE_AGERE_RING_STICKS) */
			if (fake.polaris && fake.sticks && !fake.hook)
				fake.stuck_until = fake.samples + 80000;
			switch_to(0, fake.hook);
		}
		return 0;
	case 0x0c:
		if (index != 1 || value != 8 || length != 8) {
			fprintf(stderr, "fake Agere: command 0x0c (%04x, %04x, %u "
				"octets): not as Windows' driver sends it\n",
				index, value, length);
			return LIBUSB_ERROR_PIPE;
		}
		for (int i = 0; i < 4; i++)
			if ((data[2 * i] | data[2 * i + 1] << 8) != START[i]) {
				fprintf(stderr, "fake Agere: command 0x0c's data "
					"not Windows' driver's\n");
				return LIBUSB_ERROR_PIPE;
			}
		fake.streaming = true;
		fake.tx.assign(TX_LEAD, 0);
		fake.in.clear();
		fake.ringing = false;
		return 0;
	case 0x0d:
		fake.streaming = false;
		return 0;
	case 0x10:
		return 0;
	case 0x12:
		return fake.rom != 0x106 ? 0 : LIBUSB_ERROR_PIPE;
	}
	return LIBUSB_ERROR_PIPE;
}

/* the device on libusb's bus (fake_libusb.h) */
struct Agere final : fake_usb::Device {
	/* its log on stderr with FAKE_AGERE_LOG */
	Agere() : Device("FAKE_AGERE_LOG") {}

	/* the settings from the environment: the log's, and the device's */
	void init() override
	{
		fake.log = log;
		if (const char *r = getenv("FAKE_AGERE_ROM"))
			fake.rom = (uint16_t)strtoul(r, nullptr, 16);
		fake.polaris = getenv("FAKE_AGERE_POLARIS") != nullptr;
		fake.sticks = getenv("FAKE_AGERE_RING_STICKS") != nullptr;
		if (const char *e = getenv("FAKE_AGERE_DRIFT"))
			fake.drift = strtol(e, nullptr, 10);
		fake.full = getenv("FAKE_AGERE_FULL") != nullptr;
		fake.patched = getenv("FAKE_AGERE_PATCHED") != nullptr;
	}

	/* the modem's descriptor (047e:2892, USB 2.0) */
	void descriptor(libusb_device_descriptor *d) override
	{
		d->bLength = LIBUSB_DT_DEVICE_SIZE;
		d->bDescriptorType = LIBUSB_DT_DEVICE;
		d->bcdUSB = 0x0200;
		d->idVendor = 0x047e;
		d->idProduct = 0x2892;
		d->bNumConfigurations = 1;
	}

	/* address 9 */
	uint8_t address() const override
	{
		return 9;
	}

	/* high speed, or full (FAKE_AGERE_FULL) */
	int speed() const override
	{
		return fake.full ? LIBUSB_SPEED_FULL : LIBUSB_SPEED_HIGH;
	}

	/* the device opened: its connections to the exchange at
	   FAKE_AGERE_SOCKET, the line monitor on (LIBUSB_ERROR_BUSY if open,
	   _ACCESS if no socket, _IO if no exchange) */
	int open() override
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

	/* interface 0's setting alt, 0 to 3 */
	int set_alt(int n, int alt) override
	{
		if (fake.log)
			fprintf(stderr, "fake Agere: alternate setting %d\n", alt);
		if (n != 0 || alt < 0 || alt > 3)
			return LIBUSB_ERROR_NOT_FOUND;
		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		fake.alt = alt;
		return 0;
	}

	/* the configuration (make_config()), static */
	libusb_config_descriptor *config() override
	{
		make_config();
		return &::config;
	}

	/* (FAKE_AGERE_NO_ACTIVE: none said, as Android's libusb had it for the
	   first device started again) */
	int active_config(libusb_config_descriptor **c) override
	{
		if (getenv("FAKE_AGERE_NO_ACTIVE"))
			return LIBUSB_ERROR_NOT_FOUND;
		*c = config();
		return 0;
	}

	/* a vendor command to the device: command_in() or command_out(); the
	   standard requests (SET_CONFIGURATION) stall;
	   LIBUSB_ERROR_NO_DEVICE once gone */
	int control(uint8_t type, uint8_t request, uint16_t value,
		    uint16_t index, unsigned char *data,
		    uint16_t length) override
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

	/* t queued at its endpoint (isochronous ones only, in alternate
	   setting 1), done in run() */
	int submit(libusb_transfer *t) override
	{
		if (fake.gone)
			return LIBUSB_ERROR_NO_DEVICE;
		if (t->type != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS || fake.alt != 1)
			return LIBUSB_ERROR_NOT_FOUND;
		if (t->endpoint == EP_IN)
			fake.in_q.push_back(t);
		else if (t->endpoint == EP_OUT)
			fake.out_q.push_back(t);
		else
			return LIBUSB_ERROR_NOT_FOUND;
		fake.next[t] = 0;
		return 0;
	}

	/* t off its queue, done as cancelled in run();
	   LIBUSB_ERROR_NOT_FOUND if not queued */
	int cancel(libusb_transfer *t) override
	{
		for (auto *q : { &fake.in_q, &fake.out_q })
			for (auto i = q->begin(); i != q->end(); ++i)
				if (*i == t) {
					q->erase(i);
					fake.next.erase(t);
					fake.cancelled.push_back(t);
					return 0;
				}
		return LIBUSB_ERROR_NOT_FOUND;
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
	static Agere d;
	return d;
}
