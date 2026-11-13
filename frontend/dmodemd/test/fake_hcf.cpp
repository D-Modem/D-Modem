/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_hcf.cpp - a Conexant HCF USB modem (the Phoebe Pocket 56K, USB
 *                   1690:0103) for the tests: the part of libusb-1.0
 *                   that hcf_usb.cpp uses (LD_PRELOAD in front of the
 *                   real one), with one modem on the bus whose DSP is
 *                   its register interface as hcf_usb.h has it, and no
 *                   data pump behind it
 *
 *    Interface 0, its endpoints in the order of the pipes: 0x01 bulk
 *    OUT (registers), 0x82 bulk IN (line side), 0x03 bulk OUT (DSP
 *    code), 0x84 bulk IN (T-buffer), 0x05 bulk OUT (data), 0x86
 *    interrupt IN (status), 0x07 and 0x08 interrupt OUT (GPIO, LEDs).
 *    (The real modem's numbers are not known: its descriptor's order is
 *    what the driver goes by.)
 *
 *    - GPIO mode bit 3 holds the DSP in reset: no status.  Let go, and
 *      after a soft reset (register 0x1a bit 7), it is in its reset
 *      state: its records have the ID block (the signature, the chip:
 *      FAKE_HCF_CHIP, hex, 3731 by default) until a register but 0x1a
 *      and 0x1f is written.
 *    - A status record each 10 ms on each status read: the registers,
 *      then one pair (register 0x1e, 0x08: TDBE; no data).
 *    - NEWC (0x1f bit 0) taken (cleared) at the next record.
 *    - Download: ABCODE (0x14) 0x4f (CTL) or 0x5f (SPX) echoed in 0x1b;
 *      the address in 0x16/0x17 while 0x04 bit 4 is set; the code's
 *      octets on 0x03; ABCODE 0 then ends it: 0x17 0xff, 0x16 the sum
 *      of the octets (mod 256: the stand-in's checksum, the real DSP's
 *      is not known).  0x1b bit 2 (busy, as the echo has it) clears at
 *      the first record once code has come.
 *
 *    FAKE_HCF_LOG=1: the GPIO, the register writes and the modules
 *    loaded on stderr.  FAKE_HCF_MUTE=1: the DSP never gives its
 *    signature.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <deque>
#include <vector>

#include "fake_libusb.h"

namespace {

const uint8_t EP_REG = 0x01, EP_DAA = 0x82, EP_CODE = 0x03, EP_TBUF = 0x84,
	EP_DATA = 0x05, EP_STATUS = 0x86, EP_GPIO = 0x07, EP_LED = 0x08;
const int N_EP = 8;
const unsigned RECORD = 0x3c, REGS = 0x1e;

struct Fake {
	bool open = false, claimed = false;
	int log = 0;
	bool mute = false;
	uint16_t chip = 0x3731;
	bool held = true;               /* GPIO mode bit 3 */
	bool reset_state = false;
	uint8_t regs[0x40] = {};
	/* the download */
	uint8_t ab = 0;
	std::vector<uint8_t> code;
	unsigned loads = 0;
	std::chrono::steady_clock::time_point next;
	std::deque<libusb_transfer *> status_q, out_q, in_q, cancelled;
} fake;

/* t done with status s: its callback */
void done(libusb_transfer *t, libusb_transfer_status s)
{
	t->status = s;
	t->callback(t);
}

/* the DSP's soft reset: its reset state */
void soft_reset()
{
	memset(fake.regs, 0, sizeof fake.regs);
	fake.regs[0x1e] = 0x08;
	fake.reset_state = true;
	fake.ab = 0;
	fake.code.clear();
}

/* register reg written with v */
void write_reg(uint8_t reg, uint8_t v)
{
	if (fake.log)
		fprintf(stderr, "fake HCF: reg %02x = %02x\n", reg, v);
	if (reg >= 0x40)
		return;
	if (reg == 0x1a && (v & 0x80)) {
		soft_reset();
		return;
	}
	if (reg != 0x1f && reg != 0x1a)
		fake.reset_state = false;
	fake.regs[reg] = v;
	if (reg == 0x14) {
		if (v == 0x4f || v == 0x5f) {
			fake.ab = v;
			fake.code.clear();
			fake.regs[0x1b] = v;
		} else if (v == 0 && fake.ab) {
			unsigned sum = 0;
			for (uint8_t c : fake.code)
				sum += c;
			unsigned addr = fake.regs[0x16] | fake.regs[0x17] << 8;
			if (fake.log)
				fprintf(stderr, "fake HCF: %s module at %04x, %zu "
					"octets, checksum %02x\n",
					fake.ab == 0x5f ? "SPX" : "CTL", addr,
					fake.code.size(), sum & 0xff);
			fake.loads++;
			fake.regs[0x16] = (uint8_t)sum;
			fake.regs[0x17] = 0xff;
			fake.regs[0x1b] = 0;
			fake.ab = 0;
		}
	}
}

/* a status record into t */
void record_to(libusb_transfer *t)
{
	uint8_t r[REGS + 2] = {};
	if (fake.reset_state) {
		uint8_t id[0x50] = {};
		id[0x4d] = 0x04;
		id[0x4c] = 0x51;
		id[0x4b] = 0x49;
		id[0x4a] = 0xa5;
		id[0x43] = (uint8_t)(fake.chip >> 8);
		id[0x42] = (uint8_t)fake.chip;
		id[0x48] = 0x12;
		id[0x49] = 0x34;
		if (fake.mute)
			id[0x4d] = 0;
		for (unsigned reg = 0x41; reg <= 0x4d; reg++)
			r[0x4f - reg] = id[reg];
	} else {
		for (unsigned reg = 1; reg < REGS; reg++)
			r[REGS - reg] = fake.regs[reg];
	}
	r[REGS] = fake.regs[0x1e];
	r[REGS + 1] = 0;
	/* (NEWC taken; the code that has come taken: 0x1b bit 2 clear) */
	fake.regs[0x1f] &= (uint8_t)~1;
	if (fake.ab && !fake.code.empty())
		fake.regs[0x1b] = (uint8_t)(fake.ab & ~4);
	size_t n = sizeof r < (size_t)t->length ? sizeof r : (size_t)t->length;
	memcpy(t->buffer, r, n);
	t->actual_length = (int)n;
	(void)RECORD;
}

/* an OUT transfer's octets taken */
void take_out(libusb_transfer *t)
{
	const uint8_t *b = t->buffer;
	int n = t->length;
	switch (t->endpoint) {
	case EP_REG:
		/* [tag, reg, value] triples (a [tag, 0x10] pair at the
		   end while the code goes down: left) */
		for (int i = 0; i + 3 <= n; i += 3)
			write_reg(b[i + 1], b[i + 2]);
		break;
	case EP_CODE:
		if (fake.ab && (fake.regs[0x04] & 0x10))
			fake.code.insert(fake.code.end(), b, b + n);
		break;
	case EP_GPIO:
		if (n >= 3) {
			bool held = b[2] & 0x08;
			if (fake.log)
				fprintf(stderr, "fake HCF: gpio %02x %02x %02x\n",
					b[0], b[1], b[2]);
			if (fake.held && !held)
				soft_reset();
			fake.held = held;
		}
		break;
	default:
		break;
	}
}

/* what the device has done since */
void run()
{
	auto now = std::chrono::steady_clock::now();
	for (bool more = true; more;) {
		more = false;
		while (!fake.cancelled.empty()) {
			libusb_transfer *t = fake.cancelled.front();
			fake.cancelled.pop_front();
			done(t, LIBUSB_TRANSFER_CANCELLED);
			more = true;
		}
		while (!fake.out_q.empty()) {
			libusb_transfer *t = fake.out_q.front();
			fake.out_q.pop_front();
			take_out(t);
			t->actual_length = t->length;
			done(t, LIBUSB_TRANSFER_COMPLETED);
			more = true;
		}
	}
	if (now < fake.next || fake.held)
		return;
	fake.next = now + std::chrono::milliseconds(10);
	std::deque<libusb_transfer *> q;
	q.swap(fake.status_q);
	for (libusb_transfer *t : q) {
		record_to(t);
		done(t, LIBUSB_TRANSFER_COMPLETED);
	}
}

libusb_endpoint_descriptor eps[N_EP];
libusb_interface_descriptor alt;
libusb_interface iface;
libusb_config_descriptor config;

/* the configuration's descriptors filled in */
void make_config()
{
	const uint8_t addr[N_EP] = { EP_REG, EP_DAA, EP_CODE, EP_TBUF,
				     EP_DATA, EP_STATUS, EP_GPIO, EP_LED };
	for (int i = 0; i < N_EP; i++) {
		eps[i].bEndpointAddress = addr[i];
		eps[i].bmAttributes = i >= 5 ? LIBUSB_TRANSFER_TYPE_INTERRUPT :
			LIBUSB_TRANSFER_TYPE_BULK;
		eps[i].wMaxPacketSize = 64;
		eps[i].bInterval = 1;
	}
	memset(&alt, 0, sizeof alt);
	alt.bInterfaceClass = 0xff;
	alt.bNumEndpoints = N_EP;
	alt.endpoint = eps;
	iface.altsetting = &alt;
	iface.num_altsetting = 1;
	config.bConfigurationValue = 1;
	config.bNumInterfaces = 1;
	config.interface = &iface;
}

/* the device on libusb's bus (fake_libusb.h) */
struct Hcf final : fake_usb::Device {
	Hcf() : Device("FAKE_HCF_LOG") {}

	/* the settings from the environment */
	void init() override
	{
		fake.log = log;
		fake.mute = getenv("FAKE_HCF_MUTE") != nullptr;
		if (const char *c = getenv("FAKE_HCF_CHIP"))
			fake.chip = (uint16_t)strtoul(c, nullptr, 16);
	}

	/* the Phoebe's descriptor (1690:0103, USB 1.1) */
	void descriptor(libusb_device_descriptor *d) override
	{
		d->bLength = LIBUSB_DT_DEVICE_SIZE;
		d->bDescriptorType = LIBUSB_DT_DEVICE;
		d->bcdUSB = 0x0110;
		d->idVendor = 0x1690;
		d->idProduct = 0x0103;
		d->bNumConfigurations = 1;
	}

	uint8_t address() const override { return 5; }

	int open() override
	{
		if (fake.open)
			return LIBUSB_ERROR_BUSY;
		fake.open = true;
		fake.held = true;
		return 0;
	}

	void close() override { fake.open = false; }

	int claim(int n) override
	{
		if (n != 0)
			return LIBUSB_ERROR_NOT_FOUND;
		if (fake.claimed)
			return LIBUSB_ERROR_BUSY;
		fake.claimed = true;
		return 0;
	}

	int release(int n) override
	{
		if (n != 0 || !fake.claimed)
			return LIBUSB_ERROR_NOT_FOUND;
		fake.claimed = false;
		return 0;
	}

	int set_alt(int n, int a) override
	{
		return n == 0 && a == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
	}

	libusb_config_descriptor *config() override
	{
		make_config();
		return &::config;
	}

	/* no control requests of its own */
	int control(uint8_t, uint8_t, uint16_t, uint16_t, unsigned char *,
		    uint16_t) override
	{
		return LIBUSB_ERROR_PIPE;
	}

	/* t queued at its endpoint, done in run() */
	int submit(libusb_transfer *t) override
	{
		if (!fake.claimed)
			return LIBUSB_ERROR_NOT_FOUND;
		if (t->endpoint == EP_STATUS)
			fake.status_q.push_back(t);
		else if (t->endpoint == EP_DAA || t->endpoint == EP_TBUF)
			fake.in_q.push_back(t);     /* (never done) */
		else if (!(t->endpoint & 0x80))
			fake.out_q.push_back(t);
		else
			return LIBUSB_ERROR_NOT_FOUND;
		return 0;
	}

	int cancel(libusb_transfer *t) override
	{
		return fake_usb::cancel_from(t, { &fake.status_q, &fake.out_q,
						  &fake.in_q }, fake.cancelled);
	}

	int poll_fd() const override { return -1; }

	void run() override { ::run(); }
};

} // namespace

/* see fake_libusb.h */
fake_usb::Device &fake_usb::device()
{
	static Hcf d;
	return d;
}
