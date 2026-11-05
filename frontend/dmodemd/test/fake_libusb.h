/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_libusb.h - the part of libusb-1.0 that usb_line.cpp and its
 *                    devices' files use, for the fake USB modems
 *                    (fake_st7554.cpp, fake_pico_line.cpp,
 *                    fake_pico_wb.cpp, fake_agere.cpp, fake_apple.cpp):
 *                    one device on bus 1, whose behaviour is the fake's
 *
 *    A fake derives its device from fake_usb::Device, defines
 *    fake_usb::device() to return it, and includes this once, at the
 *    top (it defines libusb's functions).  What every device does alike
 *    is here: the context, the bus and its list, the transfers'
 *    allocation, the error texts, the poll descriptors; the rest is
 *    Device's to override.
 *
 *    The fakes stand in front of the real libusb (LD_PRELOAD) or are
 *    linked in its place (Windows, macOS), so the functions are those
 *    dmodemd calls, with libusb's own signatures.
 *
 *    FAKE_USB_NO_ISO=1: the isochronous transfers are refused
 *    (LIBUSB_ERROR_NOT_SUPPORTED: refused()), as libusb does on WinUSB
 *    before Windows 8.1.
 *    FAKE_USB_USBDK=1 (Windows): UsbDk is installed, libusb_set_option()
 *    takes LIBUSB_OPTION_USE_USBDK; without it LIBUSB_ERROR_NOT_FOUND, as
 *    libusb answers where there is no UsbDk.
 *
 */
#ifndef DMODEMD_TEST_FAKE_LIBUSB_H
#define DMODEMD_TEST_FAKE_LIBUSB_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fake_net.h"           /* (poll's POLLIN, Winsock's on Windows) */

#include <deque>
#include <initializer_list>

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

namespace fake_usb {

/* the one device on the bus */
class Device {
public:
	/* log_env: the variable that turns its log on (stderr) */
	explicit Device(const char *log_env) : log_env_(log_env) {}

	libusb_context *ctx = nullptr;
	int log = 0;                    /* log_env is set */

	/* libusb_init(): the rest of the device's settings from the
	   environment (log is set) */
	virtual void init() {}
	/* its device descriptor */
	virtual void descriptor(libusb_device_descriptor *d) = 0;
	/* its address on bus 1 */
	virtual uint8_t address() const = 0;
	/* its speed */
	virtual int speed() const { return LIBUSB_SPEED_FULL; }
	/* opened: 0 or a LIBUSB_ERROR_* */
	virtual int open() = 0;
	/* closed */
	virtual void close() = 0;
	/* interface n claimed, released (interface 0 alone by default) */
	virtual int claim(int n) { return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND; }
	/* (see claim()) */
	virtual int release(int n) { return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND; }
	/* interface n's alternate setting alt */
	virtual int set_alt(int n, int alt) = 0;
	/* its configuration (static: never freed) */
	virtual libusb_config_descriptor *config() = 0;
	/* the active one: config() by default */
	virtual int active_config(libusb_config_descriptor **c)
	{
		*c = config();
		return 0;
	}
	/* a control transfer: the octets, or a LIBUSB_ERROR_* */
	virtual int control(uint8_t type, uint8_t request, uint16_t value,
			    uint16_t index, unsigned char *data,
			    uint16_t length) = 0;
	/* t queued, done in run(): 0 or a LIBUSB_ERROR_* */
	virtual int submit(libusb_transfer *t) = 0;
	/* t off its queue, done as cancelled in run(); LIBUSB_ERROR_NOT_FOUND
	   if not queued */
	virtual int cancel(libusb_transfer *t) = 0;
	/* the descriptor dmodemd is to poll, -1 if none */
	virtual int poll_fd() const = 0;
	/* what the device has done since, its transfers completed */
	virtual void run() = 0;
	/* a synchronous interrupt transfer (only the Apple USB Modem's
	   line asks): none by default */
	virtual int interrupt(unsigned char endpoint, unsigned char *data,
			      int length, int *transferred)
	{
		(void)endpoint;
		(void)data;
		(void)length;
		(void)transferred;
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}

	/* (see the constructor) */
	const char *log_env() const { return log_env_; }

protected:
	~Device() = default;

private:
	const char *log_env_;
};

/* the fake's device (the fake defines it) */
Device &device();

/* the transfer t refused as WinUSB before Windows 8.1 would
   (FAKE_USB_NO_ISO=1: isochronous transfers are not supported) */
inline bool refused(const libusb_transfer *t)
{
	return t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS &&
		getenv("FAKE_USB_NO_ISO") != nullptr;
}

/* Device::cancel() for a device with a queue per endpoint: t off
   whichever of qs has it, onto cancelled */
inline int cancel_from(libusb_transfer *t,
		       std::initializer_list<std::deque<libusb_transfer *> *> qs,
		       std::deque<libusb_transfer *> &cancelled)
{
	for (auto *q : qs)
		for (auto i = q->begin(); i != q->end(); ++i)
			if (*i == t) {
				q->erase(i);
				cancelled.push_back(t);
				return 0;
			}
	return LIBUSB_ERROR_NOT_FOUND;
}

/* the one device's libusb_device */
inline libusb_device the_device;

} // namespace fake_usb

extern "C" {

/* the context (the device's log and settings read) */
int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	fake_usb::Device &d = fake_usb::device();
	d.ctx = new libusb_context();
	d.log = getenv(d.log_env()) != nullptr;
	d.init();
	if (ctx)
		*ctx = d.ctx;
	return 0;
}

/* the context freed */
void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	fake_usb::Device &d = fake_usb::device();
	if (ctx == d.ctx) {
		delete d.ctx;
		d.ctx = nullptr;
	}
}

/* libusb's text for the codes dmodemd sees */
const char *LIBUSB_CALL libusb_strerror(int code)
{
	switch (code) {
	case LIBUSB_SUCCESS: return "Success";
	case LIBUSB_ERROR_IO: return "Input/Output Error";
	case LIBUSB_ERROR_ACCESS: return "Access denied (insufficient permissions)";
	case LIBUSB_ERROR_NO_DEVICE: return "No such device (it may have been disconnected)";
	case LIBUSB_ERROR_NOT_FOUND: return "Entity not found";
	case LIBUSB_ERROR_BUSY: return "Resource busy";
	case LIBUSB_ERROR_TIMEOUT: return "Operation timed out";
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
	l[0] = &fake_usb::the_device;
	*list = l;
	return 1;
}

/* the list freed */
void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int)
{
	free(list);
}

/* the device's descriptor */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *,
					     libusb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	fake_usb::device().descriptor(d);
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

/* its speed (only the Agere line asks; on Windows this replaces
   libusb, which would answer otherwise) */
int LIBUSB_CALL libusb_get_device_speed(libusb_device *)
{
	return fake_usb::device().speed();
}

/* its address */
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *)
{
	return fake_usb::device().address();
}

/* the device opened (Device::open()) */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	int r = fake_usb::device().open();
	if (r)
		return r;
	*h = new libusb_device_handle{ dev };
	return 0;
}

/* the device closed (Device::close()) */
void LIBUSB_CALL libusb_close(libusb_device_handle *h)
{
	fake_usb::device().close();
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

/* interface n claimed (Device::claim()) */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int n)
{
	return fake_usb::device().claim(n);
}

/* interface n released (Device::release()) */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int n)
{
	return fake_usb::device().release(n);
}

/* interface n's alternate setting (Device::set_alt()) */
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *,
						 int n, int alt)
{
	return fake_usb::device().set_alt(n, alt);
}

/* nothing to clear: 0 */
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *, unsigned char)
{
	return 0;
}

/* the active configuration (Device::active_config()), static */
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *,
						    libusb_config_descriptor **c)
{
	return fake_usb::device().active_config(c);
}

/* the configuration by its index (the one: 0) */
int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *, uint8_t index,
					     libusb_config_descriptor **c)
{
	if (index != 0)
		return LIBUSB_ERROR_NOT_FOUND;
	*c = fake_usb::device().config();
	return 0;
}

/* nothing to free (static) */
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor *)
{
}

/* a control transfer (Device::control()) */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *,
					uint8_t type, uint8_t request,
					uint16_t value, uint16_t index,
					unsigned char *data, uint16_t length,
					unsigned int)
{
	return fake_usb::device().control(type, request, value, index, data,
					  length);
}

/* a synchronous interrupt transfer (Device::interrupt()) */
int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *,
					  unsigned char endpoint,
					  unsigned char *data, int length,
					  int *transferred, unsigned int)
{
	return fake_usb::device().interrupt(endpoint, data, length,
					    transferred);
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

/* t freed (its buffer too if it has LIBUSB_TRANSFER_FREE_BUFFER) */
void LIBUSB_CALL libusb_free_transfer(libusb_transfer *t)
{
	if (!t)
		return;
	if (t->flags & LIBUSB_TRANSFER_FREE_BUFFER)
		free(t->buffer);
	free(t);
}

/* t queued (Device::submit()) */
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer *t)
{
	return fake_usb::device().submit(t);
}

/* t cancelled (Device::cancel()) */
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer *t)
{
	return fake_usb::device().cancel(t);
}

/* the descriptors to poll: Device::poll_fd(), if any */
const libusb_pollfd **LIBUSB_CALL libusb_get_pollfds(libusb_context *)
{
	const libusb_pollfd **l =
		(const libusb_pollfd **)calloc(2, sizeof(*l));
	static libusb_pollfd p;
	if (!l)
		return nullptr;
	int fd = fake_usb::device().poll_fd();
	if (fd >= 0) {
		p.fd = fd;
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

/* what the device has done (Device::run()), without waiting (close()'s
   wait too: nothing more will come of the time) */
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *,
						       struct timeval *tv,
						       int *)
{
	fake_usb::device().run();
	(void)tv;
	return 0;
}

#ifdef _WIN32
/* libusb's options: UsbDk if FAKE_USB_USBDK (it is "installed"), the
   others taken and ignored */
int LIBUSB_CALLV libusb_set_option(libusb_context *, enum libusb_option o, ...)
{
	if (o == LIBUSB_OPTION_USE_USBDK)
		return getenv("FAKE_USB_USBDK") ? LIBUSB_SUCCESS :
			LIBUSB_ERROR_NOT_FOUND;
	return LIBUSB_SUCCESS;
}
#endif

} // extern "C"

#endif
