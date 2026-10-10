/*
 *
 *    options.h - the command line of slmodemd, modem_test and the interop
 *                tests' modem (modem_half): slmodemd's options, as they
 *                were (modem_cmdline.c)
 *
 */
#ifndef SLMODEMD_OPTIONS_H
#define SLMODEMD_OPTIONS_H

#include <sys/types.h>

namespace slm {

struct Options {
	const char *dev_name = nullptr;  /* the device (its name names the
					    modem: /dev/slamr0) */
	const char *country = nullptr;   /* -c */
	const char *group = "dialout";   /* -g: the pty's group */
	mode_t perm = 0660;              /* -p: and its permissions */
	bool realtime = true;            /* -n: not */
	bool ring_detector = false;      /* -r */
	bool short_buffer = false;       /* -s */
	bool listen = false;             /* -L */
};

/* parses argv (prints the usage and exits on an error, -h, -v,
   --countrylist); sets the log levels (-d, -l) */
void parse_options(int argc, char *argv[], Options &opt);

/* the modem's name and version, on stdout and in the log */
void print_version();

} // namespace slm

#endif
