/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_win9x_test.cpp - usb_win9x.cpp, libusb's calls on dmodem.sys,
 *                         on a fake driver (FakeDriver, a usb9x::Io): the
 *                         device list and its descriptors, the modem
 *                         taken by one process only, control requests
 *                         (the USB stack's own as the IOCTLs), alternate
 *                         settings, isochronous streams both ways in
 *                         their order, an interrupt transfer cancelled,
 *                         one timed out, a stall, transfers freed or
 *                         their handle closed while the driver has them.
 *                         No Windows needed.
 *
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <libusb.h>

#include "dmodem_usb.h"
#include "usb_win9x.h"

namespace {

int fails, checks;

#define CHECK(e) do { checks++; if (!(e)) { fails++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), \
	b_ = (long long)(b); checks++; if (a_ != b_) { fails++; \
	fprintf(stderr, "%s:%d: %s == %s: %lld != %lld\n", __FILE__, \
		__LINE__, #a, #b, a_, b_); } } while (0)

/* a SmartUSB56's configuration, with a class descriptor after alternate
   setting 1's interface and one after its interrupt endpoint */
const uint8_t CFG[] = {
	9, 2, 59, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 0, 0xff, 0, 0, 0,
	9, 4, 0, 1, 3, 0xff, 0, 0, 0,
	4, 0x24, 1, 2,
	7, 5, 0x81, 1, 32, 0, 1,
	7, 5, 0x02, 1, 32, 0, 1,
	7, 5, 0x83, 3, 8, 0, 16,
	7, 0x25, 1, 0, 0, 0, 0,
};
const uint8_t DEV[18] = { 18, 1, 0x10, 0x01, 0, 0, 0, 8, 0x83, 0x04,
			  0x54, 0x75, 0, 1, 1, 2, 0, 1 };

class FakeDriver;

struct Op : usb9x::Pending {
	~Op() override;
	FakeDriver *d = nullptr;
	intptr_t h;
	uint8_t *buf;
	uint32_t len;
	dmodem_usb_xfer x;
	bool ready = false, hung = false;
	int waits = 0;
	int err = 0;
};

/* the driver: one modem, \\.\DModemUsb0 */
class FakeDriver : public usb9x::Io {
public:
	intptr_t next = 100, owner = 0;
	std::map<intptr_t, bool> open_;
	std::vector<Op *> ops;          /* (the ones not yet taken) */
	uint32_t config = 1, set_config = 0, alt_itf = 99, alt = 99;
	uint32_t reset_ep = 0;
	std::vector<uint8_t> sent;      /* OUT data, in order */
	std::vector<uint8_t> last_setup;
	int cancels = 0;

	/* see usb9x::Io (usb_win9x.h) */
	int open(unsigned n, intptr_t *h) override
	{
		if (n != 0)
			return LIBUSB_ERROR_NOT_FOUND;
		*h = next++;
		open_[*h] = true;
		return 0;
	}

	/* see usb9x::Io (usb_win9x.h) */
	void close(intptr_t h) override
	{
		open_.erase(h);
		if (owner == h) {
			owner = 0;
			/* (the driver aborts the owner's transfers) */
			for (Op *o : ops)
				if (o->h == h && !o->ready)
					finish(o, DMODEM_USB_CANCELLED);
		}
	}

	/* h owns the modem from now on, if no other handle does */
	bool take(intptr_t h)
	{
		if (owner && owner != h)
			return false;
		owner = h;
		return true;
	}

	/* the transfer's state into its IOCTL buffer */
	static void put_xfer(Op *o)
	{
		memcpy(o->buf, &o->x, sizeof(o->x));
	}

	/* o done with status */
	void finish(Op *o, uint32_t status)
	{
		o->x.status = status;
		o->ready = true;
		o->hung = false;
		put_xfer(o);
	}

	/* see above */
	int ioctl(intptr_t h, uint32_t code, const void *in, uint32_t inlen,
		  void *out, uint32_t outlen, uint32_t *got) override
	{
		if (!open_.count(h))
			return LIBUSB_ERROR_NO_DEVICE;
		if (code == IOCTL_DMODEM_USB_INFO) {
			dmodem_usb_info i;
			memset(&i, 0, sizeof(i));
			i.version = DMODEM_USB_VERSION;
			i.config = config;
			i.speed = DMODEM_USB_SPEED_FULL;
			i.device_len = 18;
			i.config_len = sizeof(CFG);
			i.owned = owner && owner != h;
			if (outlen < sizeof(i) + 18 + sizeof(CFG))
				return LIBUSB_ERROR_OVERFLOW;
			uint8_t *b = static_cast<uint8_t *>(out);
			memcpy(b, &i, sizeof(i));
			memcpy(b + sizeof(i), DEV, 18);
			memcpy(b + sizeof(i) + 18, CFG, sizeof(CFG));
			*got = sizeof(i) + 18 + sizeof(CFG);
			return 0;
		}
		if (!take(h))
			return LIBUSB_ERROR_BUSY;
		uint32_t a[2] = { 0, 0 };
		memcpy(a, in, std::min<uint32_t>(inlen, sizeof(a)));
		switch (code) {
		case IOCTL_DMODEM_USB_CANCEL:
			for (Op *o : ops)
				if (o->x.tag == a[0] && !o->ready) {
					cancels++;
					finish(o, DMODEM_USB_CANCELLED);
					return 0;
				}
			return LIBUSB_ERROR_NOT_FOUND;
		case IOCTL_DMODEM_USB_SET_CONFIG:
			set_config = a[0];
			config = a[0];
			return 0;
		case IOCTL_DMODEM_USB_SET_ALT:
			alt_itf = a[0];
			alt = a[1];
			return 0;
		case IOCTL_DMODEM_USB_RESET_PIPE:
			reset_ep = a[0];
			return 0;
		default:
			return LIBUSB_ERROR_IO;
		}
	}

	/* see usb9x::Io (usb_win9x.h) */
	usb9x::Pending *start(intptr_t h, void *buf, uint32_t len,
			      int *err) override
	{
		if (!take(h)) {
			*err = LIBUSB_ERROR_BUSY;
			return nullptr;
		}
		Op *o = new Op();
		o->d = this;
		o->h = h;
		o->buf = static_cast<uint8_t *>(buf);
		o->len = len;
		memcpy(&o->x, buf, sizeof(o->x));
		if (len != DMODEM_USB_XFER_SIZE(o->x.npackets, o->x.length)) {
			delete o;
			*err = LIBUSB_ERROR_INVALID_PARAM;
			return nullptr;
		}
		auto *pk = reinterpret_cast<dmodem_usb_packet *>(o->buf +
								 sizeof(o->x));
		uint8_t *data = o->buf + sizeof(o->x) +
			o->x.npackets * sizeof(dmodem_usb_packet);
		switch (o->x.type) {
		case DMODEM_USB_XFER_CONTROL:
			last_setup.assign(o->x.setup, o->x.setup + 8);
			if (o->x.setup[0] & 0x80)
				for (uint32_t i = 0; i < o->x.length; i++)
					data[i] = static_cast<uint8_t>(0xa0 + i);
			else
				sent.insert(sent.end(), data, data + o->x.length);
			o->x.actual = o->x.length;
			finish(o, DMODEM_USB_COMPLETED);
			break;
		case DMODEM_USB_XFER_ISO:
			/* (done at the next wait) */
			for (uint32_t i = 0, at = 0; i < o->x.npackets; i++) {
				pk[i].actual = (o->x.endpoint & 0x80) && i == 1 ?
					pk[i].length - 2 : pk[i].length;
				pk[i].status = DMODEM_USB_COMPLETED;
				if (o->x.endpoint & 0x80)
					for (uint32_t k = 0; k < pk[i].length; k++)
						data[at + k] = static_cast<uint8_t>(
							o->x.tag * 16 + i);
				else
					sent.insert(sent.end(), data + at,
						    data + at + pk[i].length);
				o->x.actual += pk[i].actual;
				at += pk[i].length;
			}
			o->x.status = DMODEM_USB_COMPLETED;
			break;
		case DMODEM_USB_XFER_INTERRUPT:
		case DMODEM_USB_XFER_BULK:
			if (o->x.endpoint == 0x84) {
				finish(o, DMODEM_USB_STALL);
			} else {
				/* (an interrupt that does not come) */
				o->hung = true;
			}
			break;
		}
		ops.push_back(o);
		return o;
	}

	/* see usb9x::Io (usb_win9x.h) */
	bool done(usb9x::Pending *p, int *err) override
	{
		Op *o = static_cast<Op *>(p);
		if (!o->ready)
			return false;
		*err = o->err;
		return true;
	}

	/* see usb9x::Io (usb_win9x.h) */
	void wait(const std::vector<usb9x::Pending *> &ps, unsigned ms) override
	{
		for (usb9x::Pending *p : ps) {
			Op *o = static_cast<Op *>(p);
			if (o->ready)
				continue;
			o->waits += ms ? 1 : 0;
			if (o->hung && o->x.timeout_ms && ms)
				finish(o, DMODEM_USB_TIMED_OUT);
			else if (!o->hung && ms)
				finish(o, o->x.status);
		}
	}

	/* an Op usb_win9x.cpp deleted: out of the list */
	void forget(Op *o)
	{
		for (auto it = ops.begin(); it != ops.end(); ++it)
			if (*it == o) {
				ops.erase(it);
				return;
			}
	}
};

/* an Op usb_win9x.cpp deleted: out of the driver's list */
Op::~Op()
{
	d->forget(this);
}

std::shared_ptr<FakeDriver> drv;

/* see libusb.h */
libusb_device_handle *open_modem(libusb_context *ctx, libusb_device **dev)
{
	libusb_device **list = nullptr;
	libusb_device_handle *h = nullptr;
	ssize_t n = libusb_get_device_list(ctx, &list);
	CHECK_EQ(n, 1);
	if (n == 1) {
		CHECK_EQ(libusb_open(list[0], &h), 0);
		if (dev)
			*dev = list[0];
	}
	libusb_free_device_list(list, 1);
	return h;
}

/* see above */
void test_list_and_descriptors()
{
	libusb_context *ctx;
	CHECK_EQ(libusb_init(&ctx), 0);
	libusb_device **list = nullptr;
	CHECK_EQ(libusb_get_device_list(ctx, &list), 1);
	libusb_device_descriptor d;
	CHECK_EQ(libusb_get_device_descriptor(list[0], &d), 0);
	CHECK_EQ(d.idVendor, 0x0483);
	CHECK_EQ(d.idProduct, 0x7554);
	CHECK_EQ(d.bcdUSB, 0x0110);
	CHECK_EQ(d.bNumConfigurations, 1);
	CHECK_EQ(libusb_get_bus_number(list[0]), 1);
	CHECK_EQ(libusb_get_device_address(list[0]), 1);
	CHECK_EQ(libusb_get_device_speed(list[0]), LIBUSB_SPEED_FULL);
	uint8_t ports[8];
	CHECK(libusb_get_port_numbers(list[0], ports, 8) <= 0);
	CHECK(list[1] == nullptr);

	libusb_config_descriptor *c = nullptr;
	CHECK_EQ(libusb_get_active_config_descriptor(list[0], &c), 0);
	if (c) {
		CHECK_EQ(c->bConfigurationValue, 1);
		CHECK_EQ(c->bNumInterfaces, 1);
		CHECK_EQ(c->interface[0].num_altsetting, 2);
		const libusb_interface_descriptor &a1 =
			c->interface[0].altsetting[1];
		CHECK_EQ(a1.bAlternateSetting, 1);
		CHECK_EQ(a1.bNumEndpoints, 3);
		CHECK_EQ(a1.extra_length, 4);
		CHECK(a1.extra && a1.extra[1] == 0x24);
		CHECK_EQ(a1.endpoint[0].bEndpointAddress, 0x81);
		CHECK_EQ(a1.endpoint[0].bmAttributes & 3,
			 LIBUSB_TRANSFER_TYPE_ISOCHRONOUS);
		CHECK_EQ(a1.endpoint[0].wMaxPacketSize, 32);
		CHECK_EQ(a1.endpoint[2].bEndpointAddress, 0x83);
		CHECK_EQ(a1.endpoint[2].bInterval, 16);
		CHECK_EQ(a1.endpoint[2].extra_length, 7);
		CHECK_EQ(a1.endpoint[1].extra_length, 0);
		CHECK_EQ(c->interface[0].altsetting[0].bNumEndpoints, 0);
		libusb_free_config_descriptor(c);
	}
	libusb_config_descriptor *c2 = nullptr;
	CHECK_EQ(libusb_get_config_descriptor(list[0], 1, &c2),
		 LIBUSB_ERROR_NOT_FOUND);
	libusb_free_device_list(list, 1);
	libusb_exit(ctx);
}

/* see above */
void test_one_owner()
{
	libusb_context *a, *b;
	CHECK_EQ(libusb_init(&a), 0);
	CHECK_EQ(libusb_init(&b), 0);
	libusb_device_handle *ha = open_modem(a, nullptr);
	CHECK(ha != nullptr);
	libusb_device **list = nullptr;
	/* (another process lists it all the same, and can't open it) */
	CHECK_EQ(libusb_get_device_list(b, &list), 1);
	libusb_device_handle *hb = nullptr;
	CHECK_EQ(libusb_open(list[0], &hb), LIBUSB_ERROR_BUSY);
	CHECK(hb == nullptr);
	libusb_close(ha);
	CHECK_EQ(libusb_open(list[0], &hb), 0);
	libusb_close(hb);
	libusb_free_device_list(list, 1);
	libusb_exit(a);
	libusb_exit(b);
}

/* see above */
void test_control()
{
	libusb_context *ctx;
	CHECK_EQ(libusb_init(&ctx), 0);
	libusb_device *dev = nullptr;
	libusb_device_handle *h = open_modem(ctx, &dev);
	if (!h)
		return;
	uint8_t b[4] = { 0, 0, 0, 0 };
	CHECK_EQ(libusb_control_transfer(h, 0xc0, 0x10, 0x1234, 0x0005, b, 4,
					 1000), 4);
	CHECK_EQ(b[0], 0xa0);
	CHECK_EQ(b[3], 0xa3);
	CHECK_EQ(drv->last_setup[2], 0x34);
	CHECK_EQ(drv->last_setup[3], 0x12);
	CHECK_EQ(drv->last_setup[6], 4);
	uint8_t w[2] = { 0x55, 0x66 };
	drv->sent.clear();
	CHECK_EQ(libusb_control_transfer(h, 0x40, 0x11, 0, 0, w, 2, 1000), 2);
	CHECK_EQ(drv->sent.size(), 2);
	CHECK_EQ(drv->sent[1], 0x66);

	/* the USB stack's own: as the IOCTLs */
	CHECK_EQ(libusb_control_transfer(h, 0x00,
					 LIBUSB_REQUEST_SET_CONFIGURATION, 1, 0,
					 nullptr, 0, 10), 0);
	CHECK_EQ(drv->set_config, 1);
	CHECK_EQ(libusb_set_configuration(h, -1), 0);
	CHECK_EQ(drv->set_config, 0);
	CHECK_EQ(libusb_set_configuration(h, 1), 0);
	CHECK_EQ(libusb_control_transfer(h, 0x01, LIBUSB_REQUEST_SET_INTERFACE,
					 1, 0, nullptr, 0, 10), 0);
	CHECK_EQ(drv->alt_itf, 0);
	CHECK_EQ(drv->alt, 1);
	CHECK_EQ(libusb_set_interface_alt_setting(h, 0, 0), 0);
	CHECK_EQ(drv->alt, 0);
	CHECK_EQ(libusb_clear_halt(h, 0x81), 0);
	CHECK_EQ(drv->reset_ep, 0x81);
	CHECK_EQ(libusb_claim_interface(h, 0), 0);
	CHECK_EQ(libusb_release_interface(h, 0), 0);
	CHECK(libusb_get_device(h) == dev);
	libusb_close(h);
	libusb_exit(ctx);
}

/* see libusb.h */
void LIBUSB_CALL order_cb(libusb_transfer *t)
{
	auto *v = static_cast<std::vector<libusb_transfer *> *>(t->user_data);
	v->push_back(t);
}

/* see above */
void test_iso()
{
	libusb_context *ctx;
	CHECK_EQ(libusb_init(&ctx), 0);
	libusb_device_handle *h = open_modem(ctx, nullptr);
	if (!h)
		return;
	std::vector<libusb_transfer *> got;
	libusb_transfer *in[3], *out;
	for (int k = 0; k < 3; k++) {
		in[k] = libusb_alloc_transfer(5);
		uint8_t *buf = static_cast<uint8_t *>(calloc(1, 5 * 32));
		libusb_fill_iso_transfer(in[k], h, 0x81, buf, 5 * 32, 5,
					 order_cb, &got, 0);
		libusb_set_iso_packet_lengths(in[k], 32);
		in[k]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		CHECK_EQ(libusb_submit_transfer(in[k]), 0);
	}
	CHECK_EQ(libusb_submit_transfer(in[0]), LIBUSB_ERROR_BUSY);
	out = libusb_alloc_transfer(4);
	uint8_t *ob = static_cast<uint8_t *>(calloc(1, 4 * 16));
	for (int i = 0; i < 64; i++)
		ob[i] = static_cast<uint8_t>(i);
	libusb_fill_iso_transfer(out, h, 0x02, ob, 64, 4, order_cb, &got, 0);
	libusb_set_iso_packet_lengths(out, 16);
	out->flags = LIBUSB_TRANSFER_FREE_BUFFER;
	drv->sent.clear();
	CHECK_EQ(libusb_submit_transfer(out), 0);
	CHECK_EQ(drv->sent.size(), 64);
	CHECK_EQ(drv->sent[63], 63);

	/* (nothing done before the driver says so) */
	struct timeval zero = { 0, 0 }, tv = { 0, 10000 };
	libusb_handle_events_timeout_completed(ctx, &zero, nullptr);
	CHECK_EQ(got.size(), 0);
	libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
	CHECK_EQ(got.size(), 4);
	if (got.size() == 4) {
		CHECK(got[0] == in[0] && got[1] == in[1] && got[2] == in[2] &&
		      got[3] == out);
		libusb_transfer *t = got[1];
		CHECK_EQ(t->status, LIBUSB_TRANSFER_COMPLETED);
		CHECK_EQ(t->actual_length, 5 * 32 - 2);
		CHECK_EQ(t->iso_packet_desc[1].actual_length, 30);
		CHECK_EQ(t->iso_packet_desc[2].actual_length, 32);
		CHECK_EQ(t->iso_packet_desc[2].status,
			 LIBUSB_TRANSFER_COMPLETED);
		/* (each packet's data where libusb has it) */
		const uint8_t *p2 = libusb_get_iso_packet_buffer_simple(t, 2);
		const uint8_t *p0 = libusb_get_iso_packet_buffer_simple(got[0], 0);
		CHECK_EQ(p2[0] & 15, 2);
		CHECK_EQ(p2[31] & 15, 2);
		CHECK((p0[0] >> 4) != (p2[0] >> 4));
		CHECK_EQ(got[3]->iso_packet_desc[3].actual_length, 16);
	}
	/* again: resubmitted from the list, in order */
	got.clear();
	CHECK_EQ(libusb_submit_transfer(in[2]), 0);
	CHECK_EQ(libusb_submit_transfer(in[0]), 0);
	libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
	CHECK(got.size() == 2 && got[0] == in[2] && got[1] == in[0]);
	for (int k = 0; k < 3; k++)
		libusb_free_transfer(in[k]);
	libusb_free_transfer(out);
	libusb_close(h);
	libusb_exit(ctx);
}

int intr_done;
libusb_transfer_status intr_status;

/* see libusb.h */
void LIBUSB_CALL intr_cb(libusb_transfer *t)
{
	intr_done++;
	intr_status = t->status;
}

/* see above */
void test_interrupt()
{
	libusb_context *ctx;
	CHECK_EQ(libusb_init(&ctx), 0);
	libusb_device_handle *h = open_modem(ctx, nullptr);
	if (!h)
		return;
	uint8_t buf[8];
	libusb_transfer *t = libusb_alloc_transfer(0);
	libusb_fill_interrupt_transfer(t, h, 0x83, buf, 8, intr_cb, nullptr,
				       0);
	CHECK_EQ(libusb_submit_transfer(t), 0);
	struct timeval tv = { 0, 10000 };
	libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
	CHECK_EQ(intr_done, 0);
	CHECK_EQ(libusb_cancel_transfer(t), 0);
	CHECK_EQ(libusb_cancel_transfer(t), LIBUSB_ERROR_NOT_FOUND);
	libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
	CHECK_EQ(intr_done, 1);
	CHECK_EQ(intr_status, LIBUSB_TRANSFER_CANCELLED);
	CHECK_EQ(libusb_cancel_transfer(t), LIBUSB_ERROR_NOT_FOUND);

	/* a timeout, a stall */
	int n = -1;
	CHECK_EQ(libusb_interrupt_transfer(h, 0x83, buf, 8, &n, 50),
		 LIBUSB_ERROR_TIMEOUT);
	CHECK_EQ(n, 0);
	CHECK_EQ(libusb_interrupt_transfer(h, 0x84, buf, 8, &n, 50),
		 LIBUSB_ERROR_PIPE);

	/* freed while the driver has it: kept until it gives it back */
	CHECK_EQ(libusb_submit_transfer(t), 0);
	libusb_free_transfer(t);
	/* closed with one in flight: cancelled, no callback */
	libusb_transfer *t2 = libusb_alloc_transfer(0);
	libusb_fill_interrupt_transfer(t2, h, 0x83, buf, 8, intr_cb, nullptr,
				       0);
	CHECK_EQ(libusb_submit_transfer(t2), 0);
	int before = drv->cancels;
	libusb_close(h);
	CHECK_EQ(drv->cancels, before + 2);
	CHECK_EQ(intr_done, 1);
	libusb_free_transfer(t2);
	libusb_exit(ctx);
}

} // namespace

/* see above */
int main()
{
	drv = std::make_shared<FakeDriver>();
	usb9x::set_io(drv);
	test_list_and_descriptors();
	test_one_owner();
	test_control();
	test_iso();
	test_interrupt();
	/* (every transfer given back) */
	CHECK_EQ(drv->ops.size(), 0);
	usb9x::set_io(nullptr);
	drv.reset();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
