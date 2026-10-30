/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    options.h - the command line of dmodemd, modem_test and the interop
 *                tests' modem (half_open): Smart Link's slmodemd's options
 *                (modem_cmdline.c), less those for modem hardware
 *
 */
#ifndef DMODEMD_OPTIONS_H
#define DMODEMD_OPTIONS_H


#include <sys/types.h>

#include "dm_types.h"

namespace dmd {

struct Options {
	const char *name = nullptr;      /* the modem's (dmodem0) */
	const char *country = nullptr;   /* -c */
	const char *group = "dialout";   /* -g: the pty's group */
	mode_t perm = 0660;              /* -p: and its permissions */
	bool realtime = true;            /* -n: not */
	bool listen = false;             /* -L */
	/* dmodemd: a softmodem's codec through ALSA for its line (-a, its
	   control --alsa-ctl), at rate Hz (-r: line_rate_ok(): 8000 or
	   9600, the voice-band pumps'; 48000, 96000 or 192000, the wideband
	   pump's, DP_VWB) */
	const char *alsa = nullptr;
	const char *alsa_ctl = nullptr;
	/* or a USB modem through libusb (--usb: auto, or <bus>:<address>:
	   a Smart Link SmartUSB56, a pico-serial Line or WB, an Agere USB
	   modem), a SmartUSB56's carrier and data LEDs lit (--usb-leds: on
	   boards that have them) */
	const char *usb = nullptr;
	bool usb_leds = false;
	/* a ring wakes the computer (--usb-wake: the SmartUSB56's USB
	   remote wakeup armed); the local phone sensed (--usb-handset: its
	   GPIO 2) */
	bool usb_wake = false;
	bool usb_handset = false;
	/* the SmartUSB56's international line controls, as Smart Link's
	   Windows driver has them with Options.International
	   (--usb-international) */
	bool usb_international = false;
	/* the SmartUSB56's second codec as a handset, its octets "ulaw",
	   "alaw" or "linear" (--usb-voice-codec); NULL: not used */
	const char *usb_voice_codec = nullptr;
	/* the ST7554's RHOM bit while the modem runs (--usb-rhom) */
	bool usb_rhom = false;
	/* the SmartUSB56's buzzer as the speaker (--usb-buzzer) */
	bool usb_buzzer = false;
	/* a pico-serial WB's transmit level and hybrid input gain, dB
	   (--usb-wb-tx-gain, --usb-wb-rx-gain) */
	double usb_wb_tx_gain = 0, usb_wb_rx_gain = 0;
	/* or an ISDN basic rate interface through Linux's mISDN (--misdn:
	   the device's number or name), calls for an MSN alone
	   (--misdn-msn), the B-channel's octets as they come, not bit
	   reversed (--misdn-noflip) */
	const char *misdn = nullptr;
	const char *misdn_msn = nullptr;
	bool misdn_flip = true;
	/* (-r; given: by -r, else the default, which a pico-serial WB
	   makes 96000) */
	u32 rate = 9600;
	bool rate_given = false;
	/* Windows: the DTE a COM port (--com: one end of a null-modem
	   pair such as com0com's), or TCP connections on [address:]port
	   (--tcp) */
	const char *com = nullptr;
	/* or a named pipe dmodemd makes (--pipe) or another program's
	   (--pipe-connect: a VM's COM port on the host) */
	const char *pipe = nullptr;
	const char *pipe_connect = nullptr;
	const char *tcp = nullptr;
	/* or the COM port of D-Modem's virtual modem driver (--driver:
	   its name, or auto for the first there is) */
	const char *driver = nullptr;
};

/* parses argv (prints the usage and exits on an error, -h, -v,
   --countrylist); sets the log levels (-d, -l) */
void parse_options(int argc, char *argv[], Options &opt);

/* a line's sample rate the modem runs at (-r): 8000 and 9600 for the
   voice-band pumps, 48000, 96000 and 192000 for the wideband one
   (DP_VWB, which refuses less than 48000; the voice-band pumps refuse
   more than 9600) */
inline bool line_rate_ok(long rate)
{
	return rate == 8000 || rate == 9600 || rate == 48000 ||
	       rate == 96000 || rate == 192000;
}

/* the modem's name and version, on stdout and in the log */
void print_version();

} // namespace dmd

#endif
