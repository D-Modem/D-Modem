/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_win9x.h - libusb-1.0 for dmodemd on Windows 98 SE, ME, 2000 and
 *                  XP (usb_win9x.cpp): the calls that usb_line.cpp and
 *                  its devices' files make, on the USB pass-through of
 *                  D-Modem's own driver, dmodem.sys (the windows-driver
 *                  repository; its IOCTLs in dmodem_usb.h).  libusb's own
 *                  Windows back ends need WinUSB or UsbDk, which those
 *                  Windows do not have.
 *
 *    usb_line.cpp does not change: it includes libusb.h (libusb's own,
 *    for the types) and links this file in libusb's place (the
 *    dmodem_libusb target, cmake/libusb.cmake).
 *
 *    The devices are dmodem.sys's \\.\DModemUsb<n>: each on bus 1, at
 *    address n + 1 ("dmodemd --usb 1:1" is \\.\DModemUsb0).  The
 *    device's interfaces are all the process's once it is open: claiming
 *    and releasing them does nothing.  Its transfers wait in the
 *    driver; libusb_handle_events_timeout_completed() takes the done
 *    ones and calls their callbacks, in the order of their submission.
 *    There are no descriptors to poll (libusb_get_pollfds() has none),
 *    as with libusb's own Windows back ends.
 *
 *    What reaches the driver goes through usb9x::Io: Win32's
 *    DeviceIoControl() and overlapped I/O (usb9x::win32_io()), or the
 *    tests' own (test/usb_win9x_test.cpp: no Windows needed).
 *
 */
#ifndef DMODEMD_USB_WIN9X_H
#define DMODEMD_USB_WIN9X_H

#include <stdint.h>

#include <memory>
#include <vector>

namespace usb9x {

/* an IOCTL that waits in the driver (IOCTL_DMODEM_USB_TRANSFER) */
class Pending {
public:
	virtual ~Pending() {}
};

/* the way to the driver.  Errors are LIBUSB_ERROR_* */
class Io {
public:
	virtual ~Io() {}
	/* device n (\\.\DModemUsb<n>) opened: 0 and *h, or an error
	   (LIBUSB_ERROR_NOT_FOUND: no device n) */
	virtual int open(unsigned n, intptr_t *h) = 0;
	virtual void close(intptr_t h) = 0;
	/* an IOCTL that does not wait: 0 and its output's length (*got),
	   or an error */
	virtual int ioctl(intptr_t h, uint32_t code, const void *in,
			  uint32_t inlen, void *out, uint32_t outlen,
			  uint32_t *got) = 0;
	/* a transfer started: buf (len octets, a struct dmodem_usb_xfer
	   and what follows it) is its input and, when it is done, its
	   output; it must stay until then.  The pending IOCTL, or nullptr
	   and *err */
	virtual Pending *start(intptr_t h, void *buf, uint32_t len,
			       int *err) = 0;
	/* p done: true, with 0 or an error (*err) */
	virtual bool done(Pending *p, int *err) = 0;
	/* until one of ps is done, ms at most */
	virtual void wait(const std::vector<Pending *> &ps, unsigned ms) = 0;
};

/* the Io of libusb_init()'s contexts from now on (the tests'; nullptr:
   Win32's again) */
void set_io(std::shared_ptr<Io> io);

#ifdef _WIN32
/* Win32's: CreateFile() on \\.\DModemUsb<n>, DeviceIoControl() */
std::shared_ptr<Io> win32_io();
#endif

} // namespace usb9x

#endif
