/*
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
};

/* parses argv (prints the usage and exits on an error, -h, -v,
   --countrylist); sets the log levels (-d, -l) */
void parse_options(int argc, char *argv[], Options &opt);

/* the modem's name and version, on stdout and in the log */
void print_version();

} // namespace dmd

#endif
