/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fake_usb_backend.h - what the fake USB modems (fake_st7554.cpp,
 *                         fake_pico_line.cpp) share to stand for
 *                         Windows 7's and 8's drivers:
 *
 *    FAKE_USB_NO_ISO=1: the isochronous transfers are refused
 *    (LIBUSB_ERROR_NOT_SUPPORTED), as libusb does on WinUSB before
 *    Windows 8.1.
 *    FAKE_USB_USBDK=1 (Windows): UsbDk is installed, libusb_set_option()
 *    takes LIBUSB_OPTION_USE_USBDK; without it LIBUSB_ERROR_NOT_FOUND, as
 *    libusb answers where there is no UsbDk.
 *
 *    Included once, inside each fake's extern "C" block.
 */
#ifndef DMODEMD_TEST_FAKE_USB_BACKEND_H
#define DMODEMD_TEST_FAKE_USB_BACKEND_H

#include <stdlib.h>

/* the transfer t refused as WinUSB before Windows 8.1 would (FAKE_USB_NO_ISO) */
static inline bool fake_usb_refused(const libusb_transfer *t)
{
	return t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS &&
		getenv("FAKE_USB_NO_ISO") != nullptr;
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

#endif
