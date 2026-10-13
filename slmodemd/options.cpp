/*
 *
 *    options.cpp - the command line (options.h); slmodemd's
 *                  modem_cmdline.c
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dcore.h>
#include <log.h>

#include "options.h"

/* the usage's text, to stderr */
#define PR_INFO(fmt...) fprintf(stderr, fmt)

namespace slm {

static const char default_dev_name[] = "/dev/slamr0";

enum {
	OPT_HELP = 0, OPT_USAGE, OPT_VERSION, OPT_COUNTRY, OPT_COUNTRYLIST,
	OPT_ALSA, OPT_GROUP, OPT_PERM, OPT_RINGDET, OPT_USER, OPT_SHORTBUF,
	OPT_DEBUG, OPT_LOG, OPT_LISTEN, OPT_LAST
};

enum opt_arg { NONE = 0, MANDATORY, OPTIONAL };
enum opt_type { INTEGER, STRING };

/* an option: its letter, long name, help line, argument (kind, type,
   value: the default, then what was given) and how often it was seen */
struct opt {
	int ch;
	const char *name;
	const char *desc;
	enum opt_arg arg;
	enum opt_type arg_type;
	const char *arg_val;
	unsigned found;
};

static struct opt opt_list[] = {
	{ 'h', "help", "this usage", NONE, INTEGER, nullptr, 0 },
	{ 'u', "usage", "this usage", NONE, INTEGER, nullptr, 0 },
	{ 'v', "version", "show version and exit", NONE, INTEGER, nullptr, 0 },
	{ 'c', "country", "default modem country name", MANDATORY, STRING,
	  "USA", 0 },
	{ 0, "countrylist", "show list of supported countries", NONE, INTEGER,
	  nullptr, 0 },
	{ 'a', "alsa", "ALSA mode (see README for howto)", NONE, INTEGER,
	  nullptr, 0 },
	{ 'g', "group", "Modem TTY group", MANDATORY, STRING, "dialout", 0 },
	{ 'p', "perm", "Modem TTY permission", MANDATORY, INTEGER, "0660", 0 },
	{ 'r', "ringdetector", "with internal ring detector (software)", NONE,
	  INTEGER, nullptr, 0 },
	{ 'n', "nortpriority", "run with regular priority", NONE, INTEGER,
	  nullptr, 0 },
	{ 's', "shortbuffer", "use short buffer (4 periods length)", NONE,
	  INTEGER, nullptr, 0 },
	{ 'd', "debug", "debug level (developers only, for ./sl...)", OPTIONAL,
	  INTEGER, "0", 0 },
	{ 'l', "log", "logging mode", OPTIONAL, INTEGER, "5", 0 },
	{ 'L', "listen", "register with the SIP server and take calls in "
	  "(RING, ATA or ATS0=n)", NONE, INTEGER, nullptr, 0 },
	{ 0, nullptr, nullptr, NONE, INTEGER, nullptr, 0 },
};

/* see options.h */
void print_version()
{
	printf("%s: version %s %s\n", dcore::Modem::product,
	       dcore::Modem::version, __DATE__ " " __TIME__);
	dcore::log_printf(1, "%s: version %s %s\n", dcore::Modem::product,
			  dcore::Modem::version, __DATE__ " " __TIME__);
}

/* the options and the environment on stderr, then exit(1) */
static void usage(const char *prog_name)
{
	PR_INFO("Usage: %s [option...] <device>\n"
		"Where 'device' is name of modem device (default `%s')\n"
		"  and 'option' may be:\n", prog_name, default_dev_name);
	for (struct opt *opt = opt_list; opt->name; opt++) {
		int n = 0;
		if (opt->ch)
			n = PR_INFO("  -%c, ", opt->ch);
		else
			n = PR_INFO("      ");
		n += PR_INFO("--%s%s ", opt->name, opt->arg ? "=VAL" : "");
		n += PR_INFO("%*s%s", 24 - n, "", opt->desc);
		if (opt->arg)
			PR_INFO(" (default `%s')", opt->arg_val);
		PR_INFO("\n");
	}
	PR_INFO("Environment:\n"
		"  SIP_LOGIN=user:password@domain   the SIP account (required)\n"
		"  SIP_PORT=port                    the local SIP port (5060)\n"
		"  SIP_LOG_LEVEL=n                  pjsip's log level (4)\n");
	exit(1);
}

/* argv into opt_list (found, arg_val): -x or --name (any number of
   dashes), the value after '=', glued on (digits only), or the next
   argument if it is no option (digits only for an INTEGER).  Returns the
   one other argument (the device) or NULL; usage() on anything else */
static const char *opt_parse(int argc, char *argv[])
{
	const char *prog_name = argv[0];
	const char *dev_name = nullptr;

	for (int i = 1; i < argc; i++) {
		char *p = argv[i], *arg = nullptr;
		int found = 0;
		if (*p != '-') {
			if (dev_name)
				usage(prog_name);
			dev_name = p;
			continue;
		}
		do {
			p++;
		} while (*p == '-');
		for (struct opt *opt = opt_list; opt->name; opt++) {
			if (!strncmp(p, opt->name, strlen(opt->name)) ||
			    (opt->ch && *p == opt->ch)) {
				char *q;
				if (!strncmp(p, opt->name, strlen(opt->name)))
					q = p + strlen(opt->name);
				else
					q = p + 1;
				if (*q == '\0')
					found = 1;
				else if (*q == '=' && opt->arg) {
					arg = q + 1;
					found = 1;
				} else if (isdigit((unsigned char)*q) && opt->arg) {
					arg = q;
					found = 1;
				}
			}
			if (!found)
				continue;
			if (arg && !opt->arg)
				usage(prog_name);
			if (!arg && opt->arg &&
			    (i < argc - 1 && *argv[i + 1] != '-') &&
			    (opt->arg_type != INTEGER ||
			     isdigit((unsigned char)*argv[i + 1])))
				arg = argv[++i];
			if (!arg && opt->arg == MANDATORY)
				usage(prog_name);
			opt->found++;
			if (opt->arg && arg)
				opt->arg_val = arg;
			break;
		}
		if (!found)
			usage(prog_name);
	}
	return dev_name;
}

/* opt_list's findings into o and the log levels (-d, -l); exits for -h,
   -v, --countrylist, -a and a bad -p */
void parse_options(int argc, char *argv[], Options &o)
{
	const char *prog_name = argv[0];
	int val;

	o.dev_name = opt_parse(argc, argv);
	if (opt_list[OPT_HELP].found || opt_list[OPT_USAGE].found)
		usage(prog_name);
	if (opt_list[OPT_VERSION].found) {
		print_version();
		exit(0);
	}
	if (opt_list[OPT_COUNTRYLIST].found) {
		for (const struct homolog_set *s = homolog_set; s->name; s++)
			PR_INFO("%02x: %s\n", s->id, s->name);
		exit(0);
	}
	if (opt_list[OPT_COUNTRY].found)
		o.country = opt_list[OPT_COUNTRY].arg_val;
	if (opt_list[OPT_ALSA].found) {
		PR_INFO("ALSA support is not compiled in (see README for howto).\n");
		exit(1);
	}
	if (opt_list[OPT_GROUP].found)
		o.group = opt_list[OPT_GROUP].arg_val;
	if (opt_list[OPT_PERM].found) {
		val = strtol(opt_list[OPT_PERM].arg_val, nullptr, 8);
		if (val <= 0)
			usage(prog_name);
		o.perm = val;
	}
	if (opt_list[OPT_RINGDET].found)
		o.ring_detector = true;
	if (opt_list[OPT_USER].found)
		o.realtime = false;
	if (opt_list[OPT_SHORTBUF].found)
		o.short_buffer = true;
	if (opt_list[OPT_DEBUG].found) {
		dcore::log_level = 1;
		if (opt_list[OPT_DEBUG].arg_val &&
		    (val = strtol(opt_list[OPT_DEBUG].arg_val, nullptr, 0)) > 0)
			dcore::log_level = val;
	}
	if (opt_list[OPT_LOG].found) {
		dcore::log_data_level = 5;
		if (opt_list[OPT_LOG].arg_val &&
		    (val = strtol(opt_list[OPT_LOG].arg_val, nullptr, 0)) > 0)
			dcore::log_data_level = val;
	}
	if (opt_list[OPT_LISTEN].found)
		o.listen = true;
	if (!o.dev_name)
		o.dev_name = default_dev_name;
}

} // namespace slm
