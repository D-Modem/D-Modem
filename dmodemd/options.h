/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    options.h - the command line of dmodemd, modem_test and the interop
 *                tests' modem (half_open): Smart Link's slmodemd's options
 *                (modem_cmdline.c), less those for modem hardware
 *
 */
#ifndef DMODEMD_OPTIONS_H
#define DMODEMD_OPTIONS_H

#include <sys/types.h>

namespace dmd {

struct Options {
	const char *name = nullptr;      /* the modem's (dmodem0) */
	const char *country = nullptr;   /* -c */
	const char *group = "dialout";   /* -g: the pty's group */
	mode_t perm = 0660;              /* -p: and its permissions */
	bool realtime = true;            /* -n: not */
	bool listen = false;             /* -L */
	/* dmodemd: a softmodem's codec through ALSA for its line (-a, its
	   control --alsa-ctl), at rate Hz (-r: 8000 or 9600, the data
	   pumps') */
	const char *alsa = nullptr;
	const char *alsa_ctl = nullptr;
	/* or a Smart Link SmartUSB56 through libusb (--usb: auto, or
	   <bus>:<address>) */
	const char *usb = nullptr;
	unsigned rate = 9600;
	/* Windows: the DTE a COM port (--com: one end of a null-modem
	   pair such as com0com's), or TCP connections on [address:]port
	   (--tcp) */
	const char *com = nullptr;
	/* or a named pipe dmodemd makes (--pipe) or another program's
	   (--pipe-connect: a VM's COM port on the host) */
	const char *pipe = nullptr;
	const char *pipe_connect = nullptr;
	const char *tcp = nullptr;
};

/* parses argv (prints the usage and exits on an error, -h, -v,
   --countrylist); sets the log levels (-d, -l) */
void parse_options(int argc, char *argv[], Options &opt);

/* the modem's name and version, on stdout and in the log */
void print_version();

} // namespace dmd

#endif
