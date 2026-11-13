/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hcf_usb.cpp - a Conexant HCF USB modem's DSP over libusb: its
 *                  registers, status, reset and code download
 *                  (hcf_usb.h)
 *
 *    Tried only on a simulated device (test/fake_hcf.cpp), none real.
 */
#include "hcf_usb.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <libusb.h>

namespace hcf {

namespace {

/* the registers' values after a reset, 0x00-0x3f (the driver's) */
const uint8_t DEFAULTS[0x40] = {
	0x00, 0x08, 0x80, 0x18, 0x60, 0x08, 0x00, 0x80,
	0x00, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
	0x00, 0x00, 0x76, 0x90, 0x00, 0x30, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
};

/* the bits the host owns: a status record leaves them be */
const uint8_t HOST_BITS[0x20] = {
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x00, 0xff, 0x7f, 0x00, 0x00,
};

enum { P_REG = 0, P_DAA = 1, P_CODE = 2, P_TBUF = 3, P_DATA = 4,
       P_STATUS = 5, P_GPIO = 6, P_LED = 7, N_PIPES = 8 };
const unsigned STATUS_LEN = 0xf0, RECORD = 0x3c, REGS = 0x1e;
const size_t REG_BATCH = 64, CODE_CHUNK = 0x140;
const uint8_t MODE_RESET = 0x08, MODE_ON = 0x01;
/* ABCODE's download commands: to CTL, to SPX */
const uint8_t AB_CTL = 0x4f, AB_SPX = 0x5f;

struct Known {
	uint16_t vid, pid;
};
const Known KNOWN[] = {
	{ 0x1690, 0x0103 },             /* Phoebe Pocket 56K (usb0103.inf) */
};

void sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

bool known(uint16_t vid, uint16_t pid)
{
	for (const Known &k : KNOWN)
		if (k.vid == vid && k.pid == pid)
			return true;
	return false;
}

Usb::~Usb()
{
	close();
}

void Usb::defaults()
{
	memcpy(regs_, DEFAULTS, sizeof DEFAULTS);
	memset(regs_ + 0x40, 0, sizeof regs_ - 0x40);
	memset(dirty_, 0, sizeof dirty_);
}

bool Usb::open(uint16_t vid, uint16_t pid, std::string &err)
{
	if (libusb_init(&ctx_) != 0) {
		ctx_ = nullptr;
		err = "libusb: cannot start";
		return false;
	}
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx_, &list);
	libusb_device *dev = nullptr;
	for (ssize_t i = 0; i < n && !dev; i++) {
		libusb_device_descriptor d;
		if (libusb_get_device_descriptor(list[i], &d) != 0)
			continue;
		if (vid ? d.idVendor == vid && d.idProduct == pid :
		    known(d.idVendor, d.idProduct)) {
			dev = list[i];
			vid_ = d.idVendor;
			pid_ = d.idProduct;
		}
	}
	int r = dev ? libusb_open(dev, &h_) : LIBUSB_ERROR_NOT_FOUND;
	libusb_free_device_list(list, 1);
	if (!dev) {
		err = "no Conexant HCF USB modem (USB 1690:0103, ...) found";
		return false;
	}
	if (r != 0) {
		h_ = nullptr;
		err = std::string("cannot open it: ") + libusb_strerror(r);
		return false;
	}
	libusb_set_auto_detach_kernel_driver(h_, 1);
	if ((r = libusb_claim_interface(h_, 0)) != 0) {
		err = std::string("cannot claim its interface 0: ") +
			libusb_strerror(r);
		return false;
	}
	libusb_config_descriptor *c;
	if ((r = libusb_get_active_config_descriptor(libusb_get_device(h_),
						      &c)) != 0) {
		err = std::string("no configuration: ") + libusb_strerror(r);
		return false;
	}
	const libusb_interface_descriptor *a =
		c->bNumInterfaces ? c->interface[0].altsetting : nullptr;
	for (int i = 0; a && i < a->bNumEndpoints; i++) {
		const libusb_endpoint_descriptor &e = a->endpoint[i];
		Pipe p;
		p.ep = e.bEndpointAddress;
		p.type = e.bmAttributes & 3;
		p.max = e.wMaxPacketSize;
		pipes_.push_back(p);
	}
	libusb_free_config_descriptor(c);
	if (pipes_.size() < 7) {
		char b[96];
		snprintf(b, sizeof b, "its interface 0 has %zu endpoints, not "
			 "the 7 or 8 of an HCF modem", pipes_.size());
		err = b;
		return false;
	}
	static const bool in[N_PIPES] = { false, true, false, true, false,
					  true, false, false };
	for (size_t i = 0; i < pipes_.size() && i < N_PIPES; i++)
		if (!!(pipes_[i].ep & 0x80) != in[i]) {
			char b[96];
			snprintf(b, sizeof b, "its endpoint %zu (%02x) is not "
				 "%s as an HCF modem's", i, pipes_[i].ep,
				 in[i] ? "IN" : "OUT");
			err = b;
			return false;
		}
	defaults();
	/* the status read (SetUsbModemPollState 2) */
	for (libusb_transfer *&t : status_) {
		t = libusb_alloc_transfer(0);
		unsigned char *b = (unsigned char *)malloc(STATUS_LEN);
		const Pipe &p = pipes_[P_STATUS];
		if (p.type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
			libusb_fill_interrupt_transfer(t, h_, p.ep, b,
						       STATUS_LEN, status_done,
						       this, 0);
		else
			libusb_fill_bulk_transfer(t, h_, p.ep, b, STATUS_LEN,
						  status_done, this, 0);
		t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		if ((r = libusb_submit_transfer(t)) != 0) {
			err = std::string("cannot read its status: ") +
				libusb_strerror(r);
			return false;
		}
	}
	return true;
}

void Usb::close()
{
	if (h_) {
		for (libusb_transfer *&t : status_)
			if (t)
				libusb_cancel_transfer(t);
		/* (their callbacks free them) */
		for (int i = 0; i < 100 && (status_[0] || status_[1] ||
					    out_busy_); i++) {
			timeval tv = { 0, 10000 };
			libusb_handle_events_timeout_completed(ctx_, &tv,
							       nullptr);
			sleep_ms(1);
		}
		libusb_release_interface(h_, 0);
		libusb_close(h_);
		h_ = nullptr;
	}
	if (ctx_) {
		libusb_exit(ctx_);
		ctx_ = nullptr;
	}
}

void Usb::status_done(libusb_transfer *t)
{
	Usb *u = (Usb *)t->user_data;
	int i = t == u->status_[0] ? 0 : 1;
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		for (int o = 0; o < t->actual_length; o += RECORD) {
			int n = t->actual_length - o;
			u->record(t->buffer + o, n < (int)RECORD ? n : RECORD);
		}
		if (libusb_submit_transfer(t) == 0)
			return;
	}
	if (t->status == LIBUSB_TRANSFER_NO_DEVICE ||
	    t->status == LIBUSB_TRANSFER_ERROR)
		u->gone_ = true;
	u->status_[i] = nullptr;
	libusb_free_transfer(t);
}

void Usb::record(const uint8_t *r, size_t n)
{
	records_++;
	if (log_ > 1) {
		fprintf(stderr, "hcf: status");
		for (size_t i = 0; i < n; i++)
			fprintf(stderr, " %02x", r[i]);
		fprintf(stderr, "\n");
	}
	if (n < REGS)
		return;
	if (reset_state_) {
		/* the ID block (OnNewStatusReceived's reset states) */
		for (unsigned reg = 0x41; reg <= 0x4d; reg++)
			regs_[reg] = r[0x4f - reg];
		if (r[2] == 0x04 && r[3] == 0x51 && r[4] == 0x49 &&
		    r[5] == 0xa5)
			id_seen_ = true;
		return;
	}
	for (unsigned reg = 1; reg < REGS; reg++) {
		uint8_t own = HOST_BITS[reg];
		regs_[reg] = (uint8_t)((regs_[reg] & own) |
				       (r[REGS - reg] & ~own));
	}
	for (size_t p = REGS; p + 1 < n; p += 2) {
		regs_[R_STATUS] = r[p];
		if (r[p] & 1)
			rx_.push_back(r[p + 1]);
	}
}

void Usb::out_done(libusb_transfer *t)
{
	Usb *u = (Usb *)t->user_data;
	if (t->status != LIBUSB_TRANSFER_COMPLETED)
		u->gone_ = true;
	u->out_busy_--;
	libusb_free_transfer(t);
}

bool Usb::out(int pipe, const uint8_t *p, size_t n)
{
	if (gone_ || !h_)
		return false;
	libusb_transfer *t = libusb_alloc_transfer(0);
	unsigned char *b = (unsigned char *)malloc(n);
	memcpy(b, p, n);
	const Pipe &pp = pipes_[pipe];
	if (pp.type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
		libusb_fill_interrupt_transfer(t, h_, pp.ep, b, (int)n,
					       out_done, this, 2000);
	else
		libusb_fill_bulk_transfer(t, h_, pp.ep, b, (int)n, out_done,
					  this, 2000);
	t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
	if (libusb_submit_transfer(t) != 0) {
		libusb_free_transfer(t);
		gone_ = true;
		return false;
	}
	out_busy_++;
	/* one at a time, in order: until it is done */
	return until([this] { return out_busy_ == 0; }, 2000) && !gone_;
}

bool Usb::gpio(uint8_t gpout, uint8_t enable, uint8_t mode)
{
	gpio_[0] = gpout;
	gpio_[1] = enable;
	gpio_[2] = mode;
	gpio_[3] = 0;
	if (log_)
		fprintf(stderr, "hcf: gpio %02x %02x %02x\n", gpout, enable,
			mode);
	if (mode & MODE_RESET)
		defaults();
	return out(P_GPIO, gpio_, sizeof gpio_);
}

unsigned Usb::get(unsigned f) const
{
	unsigned reg = f & 0x7f, shift = (f >> 8) & 7, mask = (f >> 16) & 0xff;
	if (reg >= sizeof regs_)
		return 0;
	return (regs_[reg] & mask) >> shift;
}

void Usb::set(unsigned f, unsigned value)
{
	unsigned reg = f & 0x7f, shift = (f >> 8) & 7, mask = (f >> 16) & 0xff;
	if (reg >= 0x40)
		return;
	/* the soft reset: the host's copy back to the defaults first
	   (udpSetBitField) */
	if (f == F_SOFT_RESET && value)
		defaults();
	regs_[reg] = (uint8_t)((regs_[reg] & ~mask) | ((value << shift) & mask));
	dirty_[reg] = true;
	/* a write (but NEWC's) ends the reset state (CancelResetState) */
	if (reset_state_ && f != F_NEWC && f != F_SOFT_RESET) {
		reset_state_ = false;
		defaults();
		regs_[reg] = (uint8_t)((regs_[reg] & ~mask) |
				       ((value << shift) & mask));
		dirty_[reg] = true;
	}
}

bool Usb::flush()
{
	std::vector<uint8_t> b;
	size_t limit = downloading_ ? REG_BATCH - 1 : REG_BATCH;
	for (unsigned reg = 0; reg < 0x40; reg++) {
		if (!dirty_[reg])
			continue;
		dirty_[reg] = false;
		b.push_back((uint8_t)(tag_ | (reg >= 0x20 ? 0x80 : 0)));
		b.push_back((uint8_t)reg);
		b.push_back(regs_[reg]);
		if (log_)
			fprintf(stderr, "hcf: reg %02x = %02x\n", reg,
				regs_[reg]);
		if (b.size() / 3 == limit || reg == 0x3f) {
			if (downloading_) {
				b.push_back(tag_);
				b.push_back(R_BUFFER);
			}
			if (!out(P_REG, b.data(), b.size()))
				return false;
			b.clear();
		}
	}
	if (b.empty())
		return !gone_;
	if (downloading_) {
		b.push_back(tag_);
		b.push_back(R_BUFFER);
	}
	return out(P_REG, b.data(), b.size());
}

bool Usb::run(int ms)
{
	auto end = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(ms);
	do {
		timeval tv = { 0, 2000 };
		libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
		if (gone_)
			return false;
		sleep_ms(1);
	} while (std::chrono::steady_clock::now() < end);
	return true;
}

bool Usb::until(const std::function<bool()> &cond, int ms)
{
	auto end = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(ms);
	for (;;) {
		if (cond())
			return true;
		if (gone_ || std::chrono::steady_clock::now() >= end)
			return false;
		timeval tv = { 0, 2000 };
		libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
		sleep_ms(1);
	}
}

bool Usb::reset(Id &id, std::string &err)
{
	/* the modem interface (ResetAsicModemInterface) */
	if (!gpio(0, 0, MODE_RESET | MODE_ON) || !run(10) ||
	    !gpio(0, 0, MODE_ON) || !run(100)) {
		err = "the GPIO pipe fails";
		return false;
	}
	/* the DSP's soft reset, then its ID block (DP_SoftReset_Get_SPX_ID) */
	id_seen_ = false;
	set(F_SOFT_RESET, 1);
	set(F_NEWC, 1);
	reset_state_ = true;
	if (!flush()) {
		err = "the register pipe fails";
		return false;
	}
	if (!until([this] { return id_seen_; }, 500)) {
		char b[160];
		snprintf(b, sizeof b, "the DSP gave no signature in 500 ms "
			 "(%u status records; the last 0x4d..0x4a "
			 "%02x %02x %02x %02x)", records_, regs_[0x4d],
			 regs_[0x4c], regs_[0x4b], regs_[0x4a]);
		err = b;
		return false;
	}
	auto w = [this](unsigned lo) {
		return (uint16_t)(regs_[lo] | regs_[lo + 1] << 8);
	};
	id.sig1 = w(0x4c);
	id.sig2 = w(0x4a);
	id.w48 = w(0x48);
	id.w46 = w(0x46);
	id.w44 = w(0x44);
	id.chip = w(0x42);
	id.w41 = (uint16_t)(regs_[0x41] << 8);
	return true;
}

bool Usb::download(const Image &img, const Module &m, bool spx,
		   std::string &err, unsigned *sum)
{
	char b[200];
	uint8_t ab = spx ? AB_SPX : AB_CTL;
	/* which memory (ForceDownload): the DSP echoes it */
	set(reg_byte(R_ABCODE), ab);
	if (!flush() ||
	    !until([&] { return get(reg_byte(R_ABECHO)) == ab; }, 100)) {
		snprintf(b, sizeof b, "%s %s: the DSP did not take ABCODE "
			 "%02x (register 0x1b %02x)", spx ? "SPX" : "CTL",
			 m.name.c_str(), ab, get(reg_byte(R_ABECHO)));
		err = b;
		return false;
	}
	/* the address, then the code (DpDownload) */
	set(F_DOWNLOAD, 1);
	if (!flush())
		goto gone;
	downloading_ = true;
	set(reg_byte(R_DL_LOW), m.address & 0xff);
	set(reg_byte(R_DL_HIGH), (m.address >> 8) & 0xff);
	if (!flush())
		goto gone;
	for (size_t o = 0; o < m.octets; o += CODE_CHUNK) {
		size_t n = m.octets - o < CODE_CHUNK ? m.octets - o : CODE_CHUNK;
		if (!out(P_CODE, img.code(m) + o, n))
			goto gone;
	}
	downloading_ = false;
	if (!until([&] { return !(get(reg_byte(R_ABECHO)) & 0x04); }, 100)) {
		snprintf(b, sizeof b, "%s %s: the DSP still busy (register "
			 "0x1b %02x)", spx ? "SPX" : "CTL", m.name.c_str(),
			 get(reg_byte(R_ABECHO)));
		err = b;
		set(F_DOWNLOAD, 0);
		flush();
		return false;
	}
	set(reg_byte(R_ABCODE), 0);
	if (!flush())
		goto gone;
	if (!until([&] { return get(reg_byte(R_DL_HIGH)) == 0xff; }, 100)) {
		snprintf(b, sizeof b, "%s %s: no checksum from the DSP",
			 spx ? "SPX" : "CTL", m.name.c_str());
		err = b;
		set(F_DOWNLOAD, 0);
		flush();
		return false;
	}
	if (sum)
		*sum = get(reg_byte(R_DL_LOW));
	set(F_DOWNLOAD, 0);
	if (!flush())
		goto gone;
	if (get(reg_byte(R_DL_LOW)) != m.checksum) {
		snprintf(b, sizeof b, "%s %s: the DSP's checksum %02x, not "
			 "%02x", spx ? "SPX" : "CTL", m.name.c_str(),
			 get(reg_byte(R_DL_LOW)), m.checksum);
		err = b;
		return false;
	}
	return true;
gone:
	downloading_ = false;
	err = "the modem is gone";
	return false;
}

} // namespace hcf
