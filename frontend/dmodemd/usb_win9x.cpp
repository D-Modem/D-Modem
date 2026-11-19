/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_win9x.cpp - libusb-1.0's calls on dmodem.sys's USB pass-through,
 *                    for Windows 98 SE, ME, 2000 and XP: see usb_win9x.h.
 *
 *    Each submitted transfer has a buffer of its own for its IOCTL (a
 *    struct dmodem_usb_xfer, its isochronous packets, its data), which
 *    stays until the driver gives it back, even if the transfer is
 *    freed or its handle closed before that.
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <new>

#include <libusb.h>

#include "dmodem_usb.h"
#include "usb_win9x.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace usb9x {

namespace {

/* see above */
std::shared_ptr<Io> &the_io()
{
	static std::shared_ptr<Io> io;
	return io;
}

/* a transfer's own state, before its libusb_transfer in one block */
struct Priv {
	std::vector<uint8_t> buf;       /* the IOCTL's */
	Pending *op = nullptr;          /* in flight */
	libusb_context *ctx = nullptr;
	intptr_t h = 0;                 /* its handle's */
	uint32_t tag = 0;
	bool cancelled = false;
	bool orphan = false;            /* freed while in flight */
};

const size_t PRIV = (sizeof(Priv) + 15) & ~static_cast<size_t>(15);

/* see libusb.h */
Priv *priv_of(libusb_transfer *t)
{
	return reinterpret_cast<Priv *>(reinterpret_cast<char *>(t) - PRIV);
}

/* see above */
uint16_t le16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | p[1] << 8);
}

/* a done transfer's status as a synchronous call's return value */
int sync_error(uint32_t status)
{
	switch (status) {
	case DMODEM_USB_COMPLETED:
		return 0;
	case DMODEM_USB_STALL:
		return LIBUSB_ERROR_PIPE;
	case DMODEM_USB_TIMED_OUT:
		return LIBUSB_ERROR_TIMEOUT;
	case DMODEM_USB_NO_DEVICE:
		return LIBUSB_ERROR_NO_DEVICE;
	case DMODEM_USB_OVERFLOW:
		return LIBUSB_ERROR_OVERFLOW;
	case DMODEM_USB_CANCELLED:
		return LIBUSB_ERROR_INTERRUPTED;
	default:
		return LIBUSB_ERROR_IO;
	}
}

} // namespace

/* see above */
void set_io(std::shared_ptr<Io> io)
{
	the_io() = std::move(io);
}

} // namespace usb9x

using usb9x::Priv;
using usb9x::priv_of;
using usb9x::le16;
using usb9x::sync_error;

struct libusb_context {
	std::shared_ptr<usb9x::Io> io;
	/* the transfers in flight, in the order of their submission */
	std::vector<libusb_transfer *> flight;
	/* the transfers out of flight that the driver has still: freed
	   (Priv::orphan), or dropped by libusb_close() (no callback) */
	std::vector<Priv *> orphans;
	uint32_t next_tag = 0;
};

struct libusb_device {
	libusb_context *ctx;
	int refs;
	unsigned index;
	libusb_device_descriptor desc;
	std::vector<uint8_t> cfg;
	uint8_t config;
	int speed;
};

struct libusb_device_handle {
	libusb_device *dev;
	intptr_t h;
};

namespace {

/* the device's INFO (its descriptors) into dev */
int read_info(libusb_context *ctx, intptr_t h, libusb_device *dev,
	      bool *owned)
{
	std::vector<uint8_t> b(sizeof(dmodem_usb_info) + 18 +
			       DMODEM_USB_CONFIG_MAX);
	uint32_t got = 0;
	int err = ctx->io->ioctl(h, IOCTL_DMODEM_USB_INFO, nullptr, 0,
				 b.data(), static_cast<uint32_t>(b.size()),
				 &got);
	if (err)
		return err;
	dmodem_usb_info i;
	if (got < sizeof(i))
		return LIBUSB_ERROR_IO;
	memcpy(&i, b.data(), sizeof(i));
	if (i.version != DMODEM_USB_VERSION || i.device_len != 18 ||
	    i.config_len > DMODEM_USB_CONFIG_MAX ||
	    got < sizeof(i) + i.device_len + i.config_len)
		return LIBUSB_ERROR_IO;
	const uint8_t *d = b.data() + sizeof(i);
	libusb_device_descriptor &dd = dev->desc;
	dd.bLength = d[0];
	dd.bDescriptorType = d[1];
	dd.bcdUSB = le16(d + 2);
	dd.bDeviceClass = d[4];
	dd.bDeviceSubClass = d[5];
	dd.bDeviceProtocol = d[6];
	dd.bMaxPacketSize0 = d[7];
	dd.idVendor = le16(d + 8);
	dd.idProduct = le16(d + 10);
	dd.bcdDevice = le16(d + 12);
	dd.iManufacturer = d[14];
	dd.iProduct = d[15];
	dd.iSerialNumber = d[16];
	dd.bNumConfigurations = d[17];
	dev->cfg.assign(d + 18, d + 18 + i.config_len);
	dev->config = static_cast<uint8_t>(i.config);
	dev->speed = static_cast<int>(i.speed);
	if (owned)
		*owned = i.owned != 0;
	return 0;
}

/* see libusb.h */
void unref(libusb_device *dev)
{
	if (dev && --dev->refs == 0)
		delete dev;
}

/* a transfer's IOCTL buffer: the transfer, its packets, its data (data:
   the data to send, or nullptr) */
std::vector<uint8_t> xfer_buffer(uint8_t type, uint8_t ep,
				 const uint8_t *setup, uint32_t length,
				 const uint32_t *packets, uint16_t npackets,
				 unsigned timeout, const uint8_t *data,
				 uint32_t tag)
{
	std::vector<uint8_t> b(DMODEM_USB_XFER_SIZE(npackets, length));
	dmodem_usb_xfer x;
	memset(&x, 0, sizeof(x));
	x.tag = tag;
	x.type = type;
	x.endpoint = ep;
	x.npackets = npackets;
	x.length = length;
	x.timeout_ms = timeout;
	if (setup)
		memcpy(x.setup, setup, 8);
	memcpy(b.data(), &x, sizeof(x));
	for (uint16_t i = 0; i < npackets; i++) {
		dmodem_usb_packet p = { packets[i], 0, 0 };
		memcpy(b.data() + sizeof(x) + i * sizeof(p), &p, sizeof(p));
	}
	if (data && length)
		memcpy(b.data() + sizeof(x) + npackets * sizeof(dmodem_usb_packet),
		       data, length);
	return b;
}

/* see above */
dmodem_usb_xfer xfer_of(const std::vector<uint8_t> &b)
{
	dmodem_usb_xfer x;
	memcpy(&x, b.data(), sizeof(x));
	return x;
}

/* a transfer run to its end (the synchronous calls): 0 and the buffer
   with its result, or an error */
int run(libusb_device_handle *h, std::vector<uint8_t> &b)
{
	usb9x::Io &io = *h->dev->ctx->io;
	int err = 0;
	usb9x::Pending *p = io.start(h->h, b.data(),
				     static_cast<uint32_t>(b.size()), &err);
	if (!p)
		return err ? err : LIBUSB_ERROR_IO;
	std::vector<usb9x::Pending *> one(1, p);
	while (!io.done(p, &err))
		io.wait(one, 1000);
	delete p;
	return err;
}

/* see libusb.h */
uint32_t next_tag(libusb_context *ctx)
{
	if (++ctx->next_tag == 0)
		ctx->next_tag = 1;
	return ctx->next_tag;
}

/* the transfers (all, or h's) out of flight, cancelled, and waited for
   ms at most; no callbacks.  Those the driver still has are orphans */
void drop(libusb_context *ctx, intptr_t h, bool all, unsigned ms)
{
	std::vector<Priv *> going;
	for (auto it = ctx->flight.begin(); it != ctx->flight.end();) {
		Priv *p = priv_of(*it);
		if (all || p->h == h) {
			if (!p->cancelled) {
				p->cancelled = true;
				ctx->io->ioctl(p->h, IOCTL_DMODEM_USB_CANCEL,
					       &p->tag, sizeof(p->tag), nullptr,
					       0, nullptr);
			}
			going.push_back(p);
			it = ctx->flight.erase(it);
		} else {
			++it;
		}
	}
	for (Priv *p : ctx->orphans)
		if (all || p->h == h) {
			if (!p->cancelled) {
				p->cancelled = true;
				ctx->io->ioctl(p->h, IOCTL_DMODEM_USB_CANCEL,
					       &p->tag, sizeof(p->tag), nullptr,
					       0, nullptr);
			}
			going.push_back(p);
		}
	ctx->orphans.erase(std::remove_if(ctx->orphans.begin(),
					  ctx->orphans.end(),
					  [&](Priv *p) {
						  return all || p->h == h;
					  }),
			   ctx->orphans.end());
	for (unsigned waited = 0;; waited += 10) {
		std::vector<usb9x::Pending *> ops;
		for (auto it = going.begin(); it != going.end();) {
			int err;
			if (ctx->io->done((*it)->op, &err)) {
				delete (*it)->op;
				(*it)->op = nullptr;
				if ((*it)->orphan) {
					(*it)->~Priv();
					free(*it);
				}
				it = going.erase(it);
			} else {
				ops.push_back((*it)->op);
				++it;
			}
		}
		if (going.empty() || waited >= ms)
			break;
		ctx->io->wait(ops, 10);
	}
	/* (still in the driver's hands: their buffers stay; at the
	   context's end, for good) */
	if (!all)
		ctx->orphans.insert(ctx->orphans.end(), going.begin(),
				    going.end());
}

/* a configuration descriptor in libusb's structures */
struct Config : libusb_config_descriptor {
	std::vector<uint8_t> raw;
	std::vector<libusb_interface> itfs;
	std::vector<std::vector<libusb_interface_descriptor>> alts;
	std::vector<std::vector<std::vector<libusb_endpoint_descriptor>>> eps;
};

/* see above */
int parse_config(const std::vector<uint8_t> &raw,
		 libusb_config_descriptor **out)
{
	if (raw.size() < 9 || raw[1] != LIBUSB_DT_CONFIG)
		return LIBUSB_ERROR_NOT_FOUND;
	Config *c = new (std::nothrow) Config();
	if (!c)
		return LIBUSB_ERROR_NO_MEM;
	c->raw = raw;
	const uint8_t *b = c->raw.data();
	size_t len = c->raw.size();
	c->bLength = b[0];
	c->bDescriptorType = b[1];
	c->wTotalLength = le16(b + 2);
	c->bConfigurationValue = b[5];
	c->iConfiguration = b[6];
	c->bmAttributes = b[7];
	c->MaxPower = b[8];

	/* (each interface's number, by first appearance; then where each
	   descriptor's extra goes) */
	std::vector<uint8_t> numbers;
	/* the extras: [owner kind, indexes] -> offset, length */
	struct Extra {
		int kind;       /* 0 config, 1 interface, 2 endpoint */
		size_t i, a, e;
		size_t at, n;
	};
	std::vector<Extra> extras;
	int kind = 0;
	size_t ci = 0, ca = 0, ce = 0;
	size_t at = b[0] < len ? b[0] : len;
	while (at + 2 <= len && b[at] >= 2 && b[at] <= len - at) {
		const uint8_t *d = b + at;
		if (d[1] == LIBUSB_DT_INTERFACE && d[0] >= 9) {
			size_t i = std::find(numbers.begin(), numbers.end(),
					     d[2]) - numbers.begin();
			if (i == numbers.size()) {
				numbers.push_back(d[2]);
				c->alts.emplace_back();
				c->eps.emplace_back();
			}
			libusb_interface_descriptor a;
			memset(&a, 0, sizeof(a));
			a.bLength = d[0];
			a.bDescriptorType = d[1];
			a.bInterfaceNumber = d[2];
			a.bAlternateSetting = d[3];
			a.bNumEndpoints = d[4];
			a.bInterfaceClass = d[5];
			a.bInterfaceSubClass = d[6];
			a.bInterfaceProtocol = d[7];
			a.iInterface = d[8];
			c->alts[i].push_back(a);
			c->eps[i].emplace_back();
			kind = 1;
			ci = i;
			ca = c->alts[i].size() - 1;
		} else if (d[1] == LIBUSB_DT_ENDPOINT && d[0] >= 7 &&
			   kind != 0 &&
			   c->eps[ci][ca].size() < c->alts[ci][ca].bNumEndpoints) {
			libusb_endpoint_descriptor e;
			memset(&e, 0, sizeof(e));
			e.bLength = d[0];
			e.bDescriptorType = d[1];
			e.bEndpointAddress = d[2];
			e.bmAttributes = d[3];
			e.wMaxPacketSize = le16(d + 4);
			e.bInterval = d[6];
			if (d[0] >= 9) {
				e.bRefresh = d[7];
				e.bSynchAddress = d[8];
			}
			c->eps[ci][ca].push_back(e);
			kind = 2;
			ce = c->eps[ci][ca].size() - 1;
		} else if (!extras.empty() && extras.back().kind == kind &&
			   extras.back().i == ci && extras.back().a == ca &&
			   extras.back().e == ce &&
			   extras.back().at + extras.back().n == at) {
			extras.back().n += d[0];
		} else {
			extras.push_back({ kind, ci, ca, ce, at, d[0] });
		}
		at += d[0];
	}
	/* (now that the vectors stay put: the pointers) */
	for (size_t i = 0; i < c->alts.size(); i++) {
		for (size_t a = 0; a < c->alts[i].size(); a++) {
			auto &ad = c->alts[i][a];
			ad.bNumEndpoints = static_cast<uint8_t>(c->eps[i][a].size());
			ad.endpoint = c->eps[i][a].empty() ? nullptr :
				c->eps[i][a].data();
		}
		libusb_interface itf;
		itf.altsetting = c->alts[i].data();
		itf.num_altsetting = static_cast<int>(c->alts[i].size());
		c->itfs.push_back(itf);
	}
	c->bNumInterfaces = static_cast<uint8_t>(c->itfs.size());
	c->interface = c->itfs.empty() ? nullptr : c->itfs.data();
	for (const Extra &x : extras) {
		const unsigned char *p = b + x.at;
		int n = static_cast<int>(x.n);
		if (x.kind == 0) {
			c->extra = p;
			c->extra_length = n;
		} else if (x.kind == 1) {
			c->alts[x.i][x.a].extra = p;
			c->alts[x.i][x.a].extra_length = n;
		} else {
			c->eps[x.i][x.a][x.e].extra = p;
			c->eps[x.i][x.a][x.e].extra_length = n;
		}
	}
	*out = c;
	return 0;
}

/* the done transfers taken from flight; their callbacks called */
void reap(libusb_context *ctx)
{
	std::vector<libusb_transfer *> done;
	std::vector<int> errs;
	for (auto it = ctx->flight.begin(); it != ctx->flight.end();) {
		int err = 0;
		if (ctx->io->done(priv_of(*it)->op, &err)) {
			done.push_back(*it);
			errs.push_back(err);
			it = ctx->flight.erase(it);
		} else {
			++it;
		}
	}
	/* (the orphans the driver gave back: freed, if libusb's user
	   freed them) */
	for (auto it = ctx->orphans.begin(); it != ctx->orphans.end();) {
		int err;
		if (ctx->io->done((*it)->op, &err)) {
			delete (*it)->op;
			(*it)->op = nullptr;
			if ((*it)->orphan) {
				(*it)->~Priv();
				free(*it);
			}
			it = ctx->orphans.erase(it);
		} else {
			++it;
		}
	}
	for (size_t k = 0; k < done.size(); k++) {
		libusb_transfer *t = done[k];
		Priv *p = priv_of(t);
		delete p->op;
		p->op = nullptr;
		dmodem_usb_xfer x = xfer_of(p->buf);
		const uint8_t *pk = p->buf.data() + sizeof(x);
		const uint8_t *data = pk + x.npackets * sizeof(dmodem_usb_packet);
		bool in = t->type == LIBUSB_TRANSFER_TYPE_CONTROL ?
			(x.setup[0] & 0x80) != 0 : (x.endpoint & 0x80) != 0;
		unsigned char *dst = t->buffer +
			(t->type == LIBUSB_TRANSFER_TYPE_CONTROL ? 8 : 0);
		if (errs[k]) {
			t->status = errs[k] == LIBUSB_ERROR_NO_DEVICE ?
				LIBUSB_TRANSFER_NO_DEVICE : p->cancelled ?
				LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_ERROR;
			t->actual_length = 0;
			for (int i = 0; i < t->num_iso_packets; i++) {
				t->iso_packet_desc[i].actual_length = 0;
				t->iso_packet_desc[i].status = t->status;
			}
		} else {
			uint32_t actual = std::min(x.actual, x.length);
			t->status = static_cast<libusb_transfer_status>(x.status);
			t->actual_length = static_cast<int>(actual);
			if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
				for (int i = 0; i < x.npackets; i++) {
					dmodem_usb_packet pp;
					memcpy(&pp, pk + i * sizeof(pp), sizeof(pp));
					t->iso_packet_desc[i].actual_length =
						std::min(pp.actual, pp.length);
					t->iso_packet_desc[i].status =
						static_cast<libusb_transfer_status>(pp.status);
				}
				if (in)
					memcpy(dst, data, x.length);
			} else if (in && actual) {
				memcpy(dst, data, actual);
			}
		}
		uint8_t flags = t->flags;
		if (t->callback)
			t->callback(t);
		if (flags & LIBUSB_TRANSFER_FREE_TRANSFER)
			libusb_free_transfer(t);
	}
}

} // namespace

/* ---- libusb's calls */

/* see libusb.h */
int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	libusb_context *c = new (std::nothrow) libusb_context();
	if (!c)
		return LIBUSB_ERROR_NO_MEM;
	c->io = usb9x::the_io();
#ifdef _WIN32
	if (!c->io)
		c->io = usb9x::win32_io();
#endif
	if (!c->io) {
		delete c;
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}
	if (ctx)
		*ctx = c;
	return 0;
}

/* see libusb.h */
void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	if (!ctx)
		return;
	drop(ctx, 0, true, 1000);
	delete ctx;
}

/* see libusb.h */
int LIBUSB_CALLV libusb_set_option(libusb_context *, enum libusb_option o,
				   ...)
{
	/* (no UsbDk here: the driver is dmodem.sys) */
	if (o == LIBUSB_OPTION_USE_USBDK)
		return LIBUSB_ERROR_NOT_FOUND;
	return LIBUSB_SUCCESS;
}

/* see libusb.h */
const char *LIBUSB_CALL libusb_strerror(int err)
{
	switch (err) {
	case LIBUSB_SUCCESS:
		return "Success";
	case LIBUSB_ERROR_IO:
		return "Input/Output Error";
	case LIBUSB_ERROR_INVALID_PARAM:
		return "Invalid parameter";
	case LIBUSB_ERROR_ACCESS:
		return "Access denied (insufficient permissions)";
	case LIBUSB_ERROR_NO_DEVICE:
		return "No such device (it may have been disconnected)";
	case LIBUSB_ERROR_NOT_FOUND:
		return "Entity not found";
	case LIBUSB_ERROR_BUSY:
		return "Resource busy";
	case LIBUSB_ERROR_TIMEOUT:
		return "Operation timed out";
	case LIBUSB_ERROR_OVERFLOW:
		return "Overflow";
	case LIBUSB_ERROR_PIPE:
		return "Pipe error";
	case LIBUSB_ERROR_INTERRUPTED:
		return "System call interrupted (perhaps due to signal)";
	case LIBUSB_ERROR_NO_MEM:
		return "Insufficient memory";
	case LIBUSB_ERROR_NOT_SUPPORTED:
		return "Operation not supported or unimplemented on this platform";
	default:
		return "Other error";
	}
}

/* see libusb.h */
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx,
					   libusb_device ***list)
{
	std::vector<libusb_device *> devs;
	for (unsigned n = 0; n < DMODEM_USB_MAX; n++) {
		intptr_t h;
		if (ctx->io->open(n, &h))
			continue;
		libusb_device *d = new (std::nothrow) libusb_device();
		if (!d) {
			ctx->io->close(h);
			continue;
		}
		d->ctx = ctx;
		d->refs = 1;
		d->index = n;
		int err = read_info(ctx, h, d, nullptr);
		ctx->io->close(h);
		if (err)
			delete d;
		else
			devs.push_back(d);
	}
	libusb_device **l = static_cast<libusb_device **>(
		calloc(devs.size() + 1, sizeof(*l)));
	if (!l) {
		for (libusb_device *d : devs)
			unref(d);
		return LIBUSB_ERROR_NO_MEM;
	}
	std::copy(devs.begin(), devs.end(), l);
	*list = l;
	return static_cast<ssize_t>(devs.size());
}

/* see libusb.h */
void LIBUSB_CALL libusb_free_device_list(libusb_device **list,
					 int unref_devices)
{
	if (!list)
		return;
	if (unref_devices)
		for (libusb_device **d = list; *d; d++)
			unref(*d);
	free(list);
}

/* see libusb.h */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev,
					     struct libusb_device_descriptor *desc)
{
	*desc = dev->desc;
	return 0;
}

/* see libusb.h */
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *)
{
	return 1;
}

/* see libusb.h */
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *dev)
{
	return static_cast<uint8_t>(dev->index + 1);
}

/* see libusb.h */
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *, uint8_t *, int)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

/* see libusb.h */
int LIBUSB_CALL libusb_get_device_speed(libusb_device *dev)
{
	return dev->speed;
}

/* see libusb.h */
libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *h)
{
	return h->dev;
}

/* see libusb.h */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **out)
{
	libusb_context *ctx = dev->ctx;
	intptr_t h;
	int err = ctx->io->open(dev->index, &h);
	if (err)
		return err == LIBUSB_ERROR_NOT_FOUND ? LIBUSB_ERROR_NO_DEVICE :
			err;
	bool owned = false;
	if ((err = read_info(ctx, h, dev, &owned)) == 0 && owned)
		err = LIBUSB_ERROR_BUSY;
	/* (the modem ours from now on: a CANCEL of no transfer takes it,
	   or says another process has it) */
	if (!err) {
		uint32_t none = 0;
		int e = ctx->io->ioctl(h, IOCTL_DMODEM_USB_CANCEL, &none,
				       sizeof(none), nullptr, 0, nullptr);
		if (e == LIBUSB_ERROR_BUSY || e == LIBUSB_ERROR_NO_DEVICE)
			err = e;
	}
	libusb_device_handle *dh = err ? nullptr :
		new (std::nothrow) libusb_device_handle();
	if (!dh) {
		ctx->io->close(h);
		return err ? err : LIBUSB_ERROR_NO_MEM;
	}
	dh->dev = dev;
	dh->h = h;
	dev->refs++;
	*out = dh;
	return 0;
}

/* see libusb.h */
void LIBUSB_CALL libusb_close(libusb_device_handle *h)
{
	if (!h)
		return;
	libusb_context *ctx = h->dev->ctx;
	drop(ctx, h->h, false, 1000);
	/* (orphans of this handle that the driver still has: kept, their
	   buffers too; closing the handle aborts them) */
	ctx->io->close(h->h);
	unref(h->dev);
	delete h;
}

/* see libusb.h */
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *dev,
		struct libusb_config_descriptor **config)
{
	if (dev->cfg.empty())
		return LIBUSB_ERROR_NOT_FOUND;
	return parse_config(dev->cfg, config);
}

/* see libusb.h */
int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev,
		uint8_t index, struct libusb_config_descriptor **config)
{
	/* (only the configuration the driver has: the first, or the one
	   selected) */
	if (index != 0 || dev->cfg.empty())
		return LIBUSB_ERROR_NOT_FOUND;
	return parse_config(dev->cfg, config);
}

/* see libusb.h */
void LIBUSB_CALL libusb_free_config_descriptor(
	struct libusb_config_descriptor *config)
{
	delete static_cast<Config *>(config);
}

/* see libusb.h */
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *h, int c)
{
	libusb_context *ctx = h->dev->ctx;
	uint32_t v = c < 0 ? 0 : static_cast<uint32_t>(c);
	int err = ctx->io->ioctl(h->h, IOCTL_DMODEM_USB_SET_CONFIG, &v,
				 sizeof(v), nullptr, 0, nullptr);
	if (err)
		return err;
	/* (its descriptor again: a new firmware's can differ) */
	read_info(ctx, h->h, h->dev, nullptr);
	return 0;
}

/* see libusb.h */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int)
{
	return 0;
}

/* see libusb.h */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int)
{
	return 0;
}

/* see libusb.h */
int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *,
						     int)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

/* see libusb.h */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *h,
						 int itf, int alt)
{
	uint32_t v[2] = { static_cast<uint32_t>(itf),
			  static_cast<uint32_t>(alt) };
	return h->dev->ctx->io->ioctl(h->h, IOCTL_DMODEM_USB_SET_ALT, v,
				      sizeof(v), nullptr, 0, nullptr);
}

/* see libusb.h */
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *h,
				  unsigned char endpoint)
{
	uint32_t v = endpoint;
	return h->dev->ctx->io->ioctl(h->h, IOCTL_DMODEM_USB_RESET_PIPE, &v,
				      sizeof(v), nullptr, 0, nullptr);
}

/* see libusb.h */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *h,
	uint8_t request_type, uint8_t request, uint16_t value,
	uint16_t index, unsigned char *data, uint16_t length,
	unsigned int timeout)
{
	/* (the USB stack's own requests: the IOCTLs') */
	if (request_type == 0x00 && request == LIBUSB_REQUEST_SET_CONFIGURATION)
		return libusb_set_configuration(h, value);
	if (request_type == 0x01 && request == LIBUSB_REQUEST_SET_INTERFACE)
		return libusb_set_interface_alt_setting(h, index, value);
	if (request_type == 0x02 && request == LIBUSB_REQUEST_CLEAR_FEATURE &&
	    value == 0)
		return libusb_clear_halt(h, static_cast<unsigned char>(index));
	uint8_t setup[8] = { request_type, request,
		static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
		static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8),
		static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8) };
	bool in = (request_type & 0x80) != 0;
	std::vector<uint8_t> b = xfer_buffer(DMODEM_USB_XFER_CONTROL, 0, setup,
					     length, nullptr, 0, timeout,
					     in ? nullptr : data,
					     next_tag(h->dev->ctx));
	int err = run(h, b);
	if (err)
		return err;
	dmodem_usb_xfer x = xfer_of(b);
	if ((err = sync_error(x.status)) != 0)
		return err;
	uint32_t n = std::min<uint32_t>(x.actual, length);
	if (in && n)
		memcpy(data, b.data() + sizeof(x), n);
	return static_cast<int>(n);
}

/* a synchronous bulk or interrupt transfer */
static int sync_transfer(libusb_device_handle *h, uint8_t type,
			 unsigned char ep, unsigned char *data, int length,
			 int *transferred, unsigned timeout)
{
	if (length < 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	bool in = (ep & 0x80) != 0;
	std::vector<uint8_t> b = xfer_buffer(type, ep, nullptr,
					     static_cast<uint32_t>(length),
					     nullptr, 0, timeout,
					     in ? nullptr : data,
					     next_tag(h->dev->ctx));
	int err = run(h, b);
	if (transferred)
		*transferred = 0;
	if (err)
		return err;
	dmodem_usb_xfer x = xfer_of(b);
	uint32_t n = std::min<uint32_t>(x.actual, static_cast<uint32_t>(length));
	if (in && n)
		memcpy(data, b.data() + sizeof(x), n);
	if (transferred)
		*transferred = static_cast<int>(n);
	return sync_error(x.status);
}

/* see libusb.h */
int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *h,
	unsigned char endpoint, unsigned char *data, int length,
	int *transferred, unsigned int timeout)
{
	return sync_transfer(h, DMODEM_USB_XFER_INTERRUPT, endpoint, data,
			     length, transferred, timeout);
}

/* see libusb.h */
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *h,
	unsigned char endpoint, unsigned char *data, int length,
	int *transferred, unsigned int timeout)
{
	return sync_transfer(h, DMODEM_USB_XFER_BULK, endpoint, data, length,
			     transferred, timeout);
}

/* see libusb.h */
struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	if (iso_packets < 0)
		return nullptr;
	size_t size = usb9x::PRIV + sizeof(libusb_transfer) +
		static_cast<size_t>(iso_packets) *
		sizeof(libusb_iso_packet_descriptor);
	void *mem = calloc(1, size);
	if (!mem)
		return nullptr;
	new (mem) Priv();
	libusb_transfer *t = reinterpret_cast<libusb_transfer *>(
		static_cast<char *>(mem) + usb9x::PRIV);
	t->num_iso_packets = iso_packets;
	return t;
}

/* see libusb.h */
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t)
{
	if (!t)
		return;
	Priv *p = priv_of(t);
	if (t->flags & LIBUSB_TRANSFER_FREE_BUFFER) {
		free(t->buffer);
		t->buffer = nullptr;
	}
	if (p->op && p->ctx) {
		/* (in flight still: its buffer stays until the driver gives
		   it back) */
		libusb_context *ctx = p->ctx;
		ctx->flight.erase(std::remove(ctx->flight.begin(),
					      ctx->flight.end(), t),
				  ctx->flight.end());
		p->orphan = true;
		if (std::find(ctx->orphans.begin(), ctx->orphans.end(), p) ==
		    ctx->orphans.end())
			ctx->orphans.push_back(p);
		return;
	}
	p->~Priv();
	free(p);
}

/* see libusb.h */
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t)
{
	Priv *p = priv_of(t);
	if (p->op)
		return LIBUSB_ERROR_BUSY;
	if (!t->dev_handle || t->length < 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	libusb_device_handle *h = t->dev_handle;
	libusb_context *ctx = h->dev->ctx;
	const uint8_t *setup = nullptr;
	const uint8_t *data = t->buffer;
	uint32_t length = static_cast<uint32_t>(t->length);
	std::vector<uint32_t> packets;
	bool in;
	switch (t->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		if (length < 8 || !t->buffer)
			return LIBUSB_ERROR_INVALID_PARAM;
		setup = t->buffer;
		data = t->buffer + 8;
		length = std::min<uint32_t>(le16(t->buffer + 6), length - 8);
		in = (setup[0] & 0x80) != 0;
		break;
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: {
		uint32_t sum = 0;
		if (t->num_iso_packets <= 0 ||
		    t->num_iso_packets > static_cast<int>(DMODEM_USB_PACKETS_MAX))
			return LIBUSB_ERROR_INVALID_PARAM;
		for (int i = 0; i < t->num_iso_packets; i++) {
			packets.push_back(t->iso_packet_desc[i].length);
			sum += t->iso_packet_desc[i].length;
		}
		if (sum > length)
			return LIBUSB_ERROR_INVALID_PARAM;
		length = sum;
		in = (t->endpoint & 0x80) != 0;
		break;
	}
	case LIBUSB_TRANSFER_TYPE_BULK:
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
		in = (t->endpoint & 0x80) != 0;
		break;
	default:
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}
	if (length > DMODEM_USB_DATA_MAX)
		return LIBUSB_ERROR_INVALID_PARAM;
	p->tag = next_tag(ctx);
	p->buf = xfer_buffer(t->type, t->type == LIBUSB_TRANSFER_TYPE_CONTROL ?
			     0 : t->endpoint, setup, length,
			     packets.empty() ? nullptr : packets.data(),
			     static_cast<uint16_t>(packets.size()), t->timeout,
			     in ? nullptr : data, p->tag);
	int err = 0;
	p->op = ctx->io->start(h->h, p->buf.data(),
			       static_cast<uint32_t>(p->buf.size()), &err);
	if (!p->op)
		return err ? err : LIBUSB_ERROR_IO;
	p->ctx = ctx;
	p->h = h->h;
	p->cancelled = false;
	ctx->flight.push_back(t);
	return 0;
}

/* see libusb.h */
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t)
{
	Priv *p = priv_of(t);
	if (!p->op || !p->ctx || p->cancelled)
		return LIBUSB_ERROR_NOT_FOUND;
	p->cancelled = true;
	/* (not found: done already, its callback to come) */
	int err = p->ctx->io->ioctl(p->h, IOCTL_DMODEM_USB_CANCEL, &p->tag,
				    sizeof(p->tag), nullptr, 0, nullptr);
	return err == LIBUSB_ERROR_NO_DEVICE ? err : 0;
}

/* see libusb.h */
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx,
	struct timeval *tv, int *completed)
{
	if (completed && *completed)
		return 0;
	unsigned ms = tv ? static_cast<unsigned>(tv->tv_sec * 1000 +
						  tv->tv_usec / 1000) : 60000;
	std::vector<usb9x::Pending *> ops;
	for (libusb_transfer *t : ctx->flight)
		ops.push_back(priv_of(t)->op);
	for (Priv *p : ctx->orphans)
		ops.push_back(p->op);
	if (!ops.empty())
		ctx->io->wait(ops, ms);
	reap(ctx);
	return 0;
}

/* see libusb.h */
const struct libusb_pollfd **LIBUSB_CALL libusb_get_pollfds(libusb_context *)
{
	return nullptr;
}

/* see libusb.h */
void LIBUSB_CALL libusb_free_pollfds(const struct libusb_pollfd **)
{
}

/* ---- Win32's way to the driver */

#ifdef _WIN32
namespace usb9x {

namespace {

/* a Win32 error as libusb's */
int win32_error(DWORD e)
{
	switch (e) {
	case ERROR_BUSY:
		return LIBUSB_ERROR_BUSY;
	case ERROR_NOT_FOUND:
		return LIBUSB_ERROR_NOT_FOUND;
	case ERROR_INVALID_PARAMETER:
		return LIBUSB_ERROR_INVALID_PARAM;
	case ERROR_ACCESS_DENIED:
		return LIBUSB_ERROR_ACCESS;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_DEV_NOT_EXIST:
	case ERROR_DEVICE_REMOVED:
	case ERROR_NOT_READY:
		return LIBUSB_ERROR_NO_DEVICE;
	case ERROR_OPERATION_ABORTED:
		return LIBUSB_ERROR_INTERRUPTED;
	case ERROR_INSUFFICIENT_BUFFER:
		return LIBUSB_ERROR_OVERFLOW;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_NO_SYSTEM_RESOURCES:
		return LIBUSB_ERROR_NO_MEM;
	default:
		return LIBUSB_ERROR_IO;
	}
}

class Win32Pending : public Pending {
public:
	/* an overlapped IOCTL's state, its event made */
	explicit Win32Pending(HANDLE h_) : h(h_)
	{
		memset(&ov, 0, sizeof(ov));
		ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	}
	/* an overlapped IOCTL's state, its event made */
	~Win32Pending() override
	{
		if (ov.hEvent)
			CloseHandle(ov.hEvent);
	}
	HANDLE h;
	OVERLAPPED ov;
	DWORD got = 0;
};

class Win32Io : public Io {
public:
	/* see usb9x::Io */
	int open(unsigned n, intptr_t *h) override
	{
		char name[32];
		snprintf(name, sizeof(name), "\\\\.\\" DMODEM_USB_NAME "%u", n);
		HANDLE f = CreateFileA(name, GENERIC_READ | GENERIC_WRITE,
				       FILE_SHARE_READ | FILE_SHARE_WRITE,
				       nullptr, OPEN_EXISTING,
				       FILE_FLAG_OVERLAPPED, nullptr);
		if (f == INVALID_HANDLE_VALUE) {
			int err = win32_error(GetLastError());
			return err == LIBUSB_ERROR_NO_DEVICE ?
				LIBUSB_ERROR_NOT_FOUND : err;
		}
		*h = reinterpret_cast<intptr_t>(f);
		return 0;
	}

	/* see usb9x::Io */
	void close(intptr_t h) override
	{
		CloseHandle(reinterpret_cast<HANDLE>(h));
	}

	/* see above */
	int ioctl(intptr_t h, uint32_t code, const void *in, uint32_t inlen,
		  void *out, uint32_t outlen, uint32_t *got) override
	{
		Win32Pending p(reinterpret_cast<HANDLE>(h));
		if (!p.ov.hEvent)
			return LIBUSB_ERROR_NO_MEM;
		BOOL ok = DeviceIoControl(p.h, code, const_cast<void *>(in),
					  inlen, out, outlen, &p.got, &p.ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING)
			ok = GetOverlappedResult(p.h, &p.ov, &p.got, TRUE);
		if (!ok)
			return win32_error(GetLastError());
		if (got)
			*got = p.got;
		return 0;
	}

	/* see usb9x::Io */
	Pending *start(intptr_t h, void *buf, uint32_t len, int *err) override
	{
		Win32Pending *p = new (std::nothrow)
			Win32Pending(reinterpret_cast<HANDLE>(h));
		if (!p || !p->ov.hEvent) {
			delete p;
			*err = LIBUSB_ERROR_NO_MEM;
			return nullptr;
		}
		if (!DeviceIoControl(p->h, IOCTL_DMODEM_USB_TRANSFER, buf, len,
				     buf, len, &p->got, &p->ov) &&
		    GetLastError() != ERROR_IO_PENDING) {
			*err = win32_error(GetLastError());
			delete p;
			return nullptr;
		}
		return p;
	}

	/* see usb9x::Io */
	bool done(Pending *pending, int *err) override
	{
		Win32Pending *p = static_cast<Win32Pending *>(pending);
		if (WaitForSingleObject(p->ov.hEvent, 0) != WAIT_OBJECT_0)
			return false;
		*err = GetOverlappedResult(p->h, &p->ov, &p->got, FALSE) ? 0 :
			win32_error(GetLastError());
		return true;
	}

	/* see usb9x::Io */
	void wait(const std::vector<Pending *> &ps, unsigned ms) override
	{
		HANDLE ev[MAXIMUM_WAIT_OBJECTS];
		DWORD n = 0;
		for (Pending *p : ps)
			if (n < MAXIMUM_WAIT_OBJECTS)
				ev[n++] = static_cast<Win32Pending *>(p)->ov.hEvent;
		if (n)
			WaitForMultipleObjects(n, ev, FALSE, ms);
		else if (ms)
			Sleep(ms);
	}
};

} // namespace

/* see usb_win9x.h */
std::shared_ptr<Io> win32_io()
{
	static std::shared_ptr<Io> io = std::make_shared<Win32Io>();
	return io;
}

} // namespace usb9x
#endif
