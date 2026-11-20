/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_usb.h - the USB pass-through of dmodem.sys: how dmodemd
 *                   (its usb_win9x.cpp, libusb-1.0's calls on these
 *                   IOCTLs) drives a USB modem whose function driver is
 *                   dmodem.sys, on Windows 98 SE, ME, 2000 and XP.  For
 *                   the driver and dmodemd both: C, no Windows header
 *                   needed.  D-Modem keeps a copy
 *                   (frontend/dmodemd/dmodem_usb.h): change both.
 *
 *    Each USB modem that dmodem.sys drives (inf/dmusb.inf) is the device
 *    \\.\DModemUsb<n>, n from 0 to DMODEM_USB_MAX - 1 (the lowest one
 *    free when the modem starts).  The driver selects the modem's first
 *    configuration, every interface at alternate setting 0, when it
 *    starts.  Any number of handles can open the device and read its
 *    INFO; the first handle that sends anything else owns the modem
 *    until it closes, and the others get STATUS_DEVICE_BUSY.  Closing
 *    the owner's handle aborts all its transfers.
 *
 *    All the IOCTLs are METHOD_BUFFERED.  Open the device with
 *    FILE_FLAG_OVERLAPPED: a TRANSFER waits until the USB stack
 *    completes it.
 *
 *    IOCTL_DMODEM_USB_INFO (out: struct dmodem_usb_info, then the device
 *        descriptor, then the configuration descriptor, all of it)
 *    IOCTL_DMODEM_USB_TRANSFER (in and out: the same buffer, a struct
 *        dmodem_usb_xfer, then npackets struct dmodem_usb_packet, then
 *        the data)
 *        one transfer: control (endpoint 0, the setup packet in
 *        setup[]), bulk, interrupt or isochronous.  The data of an
 *        isochronous transfer is its packets' data one after the other,
 *        as libusb has it.  The IOCTL succeeds when the transfer is
 *        done, whatever its result: status, actual and the packets'
 *        actual and status give it.  It fails only if the request is
 *        bad (STATUS_INVALID_PARAMETER), the modem is gone
 *        (STATUS_DEVICE_REMOVED) or another handle owns the modem.
 *    IOCTL_DMODEM_USB_CANCEL (in: a dmodem_u32, a transfer's tag)
 *        that transfer cancelled (it completes with
 *        DMODEM_USB_CANCELLED); STATUS_NOT_FOUND if no transfer has the
 *        tag.
 *    IOCTL_DMODEM_USB_SET_CONFIG (in: a dmodem_u32, the configuration
 *        value; 0: unconfigured)
 *        the configuration selected again (its descriptor read again:
 *        a modem whose firmware the engine loaded can have a new one),
 *        every interface at alternate setting 0.
 *    IOCTL_DMODEM_USB_SET_ALT (in: two dmodem_u32, the interface and the
 *        alternate setting)
 *    IOCTL_DMODEM_USB_RESET_PIPE (in: a dmodem_u32, the endpoint's
 *        address)
 *        its halt cleared, on the device and in the host.
 *
 *    The control requests that the USB stack keeps for itself
 *    (SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE) go through the
 *    IOCTLs above, not through TRANSFER.
 *
 */
#ifndef DMODEM_USB_H
#define DMODEM_USB_H

/* (dmodem_ioctl.h's, for a file that has only this header) */
#ifndef DMODEM_IOCTL
#define DMODEM_IOCTL(fn) (0x00220000u | ((unsigned)(fn) << 2))
#endif

typedef unsigned int dmodem_u32;
typedef unsigned short dmodem_u16;
typedef unsigned char dmodem_u8;

#define IOCTL_DMODEM_USB_INFO        DMODEM_IOCTL(0x840)
#define IOCTL_DMODEM_USB_TRANSFER    DMODEM_IOCTL(0x841)
#define IOCTL_DMODEM_USB_CANCEL      DMODEM_IOCTL(0x842)
#define IOCTL_DMODEM_USB_SET_CONFIG  DMODEM_IOCTL(0x843)
#define IOCTL_DMODEM_USB_SET_ALT     DMODEM_IOCTL(0x844)
#define IOCTL_DMODEM_USB_RESET_PIPE  DMODEM_IOCTL(0x845)

/* the devices: L"\\Device\\DModemUsb" and L"\\DosDevices\\DModemUsb",
   then the number */
#define DMODEM_USB_NAME "DModemUsb"
#define DMODEM_USB_MAX 16u

/* this header's version (struct dmodem_usb_info's) */
#define DMODEM_USB_VERSION 1u

/* the longest configuration descriptor the driver keeps */
#define DMODEM_USB_CONFIG_MAX 4096u
/* the most packets in one isochronous transfer, the most data in one
   transfer */
#define DMODEM_USB_PACKETS_MAX 255u
#define DMODEM_USB_DATA_MAX 65536u

/* the speeds (libusb's numbers) */
#define DMODEM_USB_SPEED_UNKNOWN 0u
#define DMODEM_USB_SPEED_LOW     1u
#define DMODEM_USB_SPEED_FULL    2u
#define DMODEM_USB_SPEED_HIGH    3u

struct dmodem_usb_info {
	dmodem_u32 version;     /* DMODEM_USB_VERSION */
	dmodem_u32 index;       /* n of \\.\DModemUsb<n> */
	dmodem_u32 config;      /* the configuration value now (0: none) */
	dmodem_u32 speed;       /* DMODEM_USB_SPEED_* */
	dmodem_u32 device_len;  /* the device descriptor's length (18) */
	dmodem_u32 config_len;  /* the configuration descriptor's (all of
				   it, wTotalLength), 0 if none */
	dmodem_u32 owned;       /* another handle owns the modem */
	dmodem_u32 reserved;
};

/* the transfer types (libusb's numbers) */
#define DMODEM_USB_XFER_CONTROL     0u
#define DMODEM_USB_XFER_ISO         1u
#define DMODEM_USB_XFER_BULK        2u
#define DMODEM_USB_XFER_INTERRUPT   3u

/* the results (libusb's enum libusb_transfer_status) */
#define DMODEM_USB_COMPLETED  0u
#define DMODEM_USB_ERROR      1u
#define DMODEM_USB_TIMED_OUT  2u
#define DMODEM_USB_CANCELLED  3u
#define DMODEM_USB_STALL      4u
#define DMODEM_USB_NO_DEVICE  5u
#define DMODEM_USB_OVERFLOW   6u

struct dmodem_usb_xfer {
	/* in */
	dmodem_u32 tag;         /* the engine's, for CANCEL */
	dmodem_u8 type;         /* DMODEM_USB_XFER_* */
	dmodem_u8 endpoint;     /* its address (0x80: IN); control: 0, the
				   direction from setup[0] */
	dmodem_u16 npackets;    /* isochronous: its packets, else 0 */
	dmodem_u32 length;      /* the data's octets (control: wLength) */
	dmodem_u32 timeout_ms;  /* 0: none */
	dmodem_u8 setup[8];     /* control: the setup packet */
	/* out */
	dmodem_u32 status;      /* DMODEM_USB_* */
	dmodem_u32 usbd;        /* the USB stack's status (USBD_STATUS_*) */
	dmodem_u32 actual;      /* the octets moved */
	dmodem_u32 reserved;
};

struct dmodem_usb_packet {
	dmodem_u32 length;      /* in: its octets */
	dmodem_u32 actual;      /* out: the octets moved */
	dmodem_u32 status;      /* out: DMODEM_USB_* */
};

/* the IOCTL buffer's size for a transfer */
#define DMODEM_USB_XFER_SIZE(npackets, length) \
	(sizeof(struct dmodem_usb_xfer) + \
	 (npackets) * sizeof(struct dmodem_usb_packet) + (length))

#endif
