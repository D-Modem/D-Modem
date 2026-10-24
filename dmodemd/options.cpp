/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    options.cpp - the command line of the modem programs (options.h):
 *                  the options read into dmd::Options and the log
 *                  levels, the usage, the version line, the countries
 *
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dcore.h>
#include <str.h>
#include <log.h>
#include <tones.h>

#include "options.h"
#include "reinit.h"

namespace dmd {

namespace {

/* the modem's name when the command line gives none */
const char DEFAULT_NAME[] = "dmodem0";

/* what an option takes after it */
enum class Takes { Nothing, Text, Number };

/* one option: how it is written, what it takes (and whether it must
   have it), its line in the usage; then, as the command line is read,
   whether it came and its value (the default until one is given) */
struct Option {
	char letter;            /* 0: a long name only */
	const char *name;
	Takes takes;
	bool mandatory;
	const char *help;
	const char *value;
	bool given;
};

/* the options in the order they are tried and shown; their indices */
enum {
	OPT_HELP, OPT_USAGE, OPT_VERSION, OPT_COUNTRY, OPT_COUNTRYLIST,
	OPT_GROUP, OPT_PERM, OPT_NORT, OPT_DEBUG, OPT_LOG, OPT_LISTEN,
	OPT_ALSA, OPT_ALSA_CTL, OPT_USB, OPT_USB_LEDS, OPT_USB_WAKE,
	OPT_USB_HANDSET, OPT_USB_INTERNATIONAL, OPT_USB_VOICE_CODEC, OPT_USB_RHOM, OPT_USB_BUZZER, OPT_RATE,
#ifdef _WIN32
	OPT_COM, OPT_PIPE, OPT_PIPE_CONNECT, OPT_TCP, OPT_DRIVER,
#endif
	OPT_COUNT
};

const Option OPTIONS[OPT_COUNT] = {
	{ 'h', "help", Takes::Nothing, false, "this text", nullptr, false },
	{ 'u', "usage", Takes::Nothing, false, "this text", nullptr, false },
	{ 'v', "version", Takes::Nothing, false, "the version, and exit",
	  nullptr, false },
	{ 'c', "country", Takes::Text, true,
	  "the country's rules (--countrylist)", "USA", false },
	{ 0, "countrylist", Takes::Nothing, false,
	  "the countries -c takes, and exit", nullptr, false },
	{ 'g', "group", Takes::Text, true, "the pty's group", "dialout",
	  false },
	{ 'p', "perm", Takes::Number, true, "the pty's mode, in octal",
	  "0660", false },
	{ 'n', "nortpriority", Takes::Nothing, false,
#ifdef _WIN32
	  "normal priority, not high", nullptr, false },
#else
	  "no realtime scheduling", nullptr, false },
#endif
	{ 'd', "debug", Takes::Number, false, "debug messages up to level",
	  "0", false },
	{ 'l', "log", Takes::Number, false,
	  "binary log dmodem.log.<name> to level", "5", false },
	{ 'L', "listen", Takes::Nothing, false,
	  "register, and take calls in", nullptr, false },
	{ 'a', "alsa", Takes::Text, true,
	  "the line a modem codec's ALSA PCM (hw:1, hw:0,6), not SIP",
	  "none", false },
	{ 0, "alsa-ctl", Takes::Text, true,
	  "its controls (hw:<card> of -a's)", "-a's", false },
	{ 0, "usb", Takes::Text, true,
	  "the line a Smart Link SmartUSB56 or a pico-serial Line (auto, or "
	  "<bus>:<address>), not SIP; list: those there are", "none",
	  false },
	{ 0, "usb-leds", Takes::Nothing, false,
	  "a SmartUSB56's carrier and data LEDs (GPIO 6 and 13) lit, on a "
	  "board that has them (Creative's Modem Blaster)", nullptr, false },
	{ 0, "usb-wake", Takes::Nothing, false,
	  "a ring on a SmartUSB56's line wakes the computer (its USB remote "
	  "wakeup armed)", nullptr, false },
	{ 0, "usb-handset", Takes::Nothing, false,
	  "a SmartUSB56's handset detector (GPIO 2): the phone on its jack "
	  "off or on hook, <DLE>H and <DLE>h in voice mode", nullptr, false },
	{ 0, "usb-international", Takes::Nothing, false,
	  "a SmartUSB56's international line controls (GPIO 8, 10, 12: the "
	  "country's DC bit, Australia's 350 ms after off hook, pulse "
	  "dialling's)", nullptr, false },
	{ 0, "usb-voice-codec", Takes::Text, true,
	  "a SmartUSB56's second codec (SSI2, alternate setting 3) as a "
	  "handset, +VLS=14 and 15 in voice mode; its octets ulaw, alaw or "
	  "linear", nullptr, false },
	{ 0, "usb-rhom", Takes::Nothing, false,
	  "a SmartUSB56's RHOM bit (FIFO_CONTROL2 bit 15) while the modem "
	  "runs, as Windows' Options.RHOM", nullptr, false },
	{ 0, "usb-buzzer", Takes::Nothing, false,
	  "a SmartUSB56's buzzer (GPIO 11) as the speaker (ATM, ATL)",
	  nullptr, false },
	{ 'r', "rate", Takes::Number, true,
	  "the codec's rate with -a or --usb (8000 or 9600)", "9600", false },
#ifdef _WIN32
	{ 0, "com", Takes::Text, true,
	  "the DTE a COM port (COM5: one end of a com0com pair)", "none",
	  false },
	{ 0, "pipe", Takes::Text, true,
	  "the DTE a named pipe dmodemd makes (\\\\.\\pipe\\<name>, or "
	  "<name>)", "none", false },
	{ 0, "pipe-connect", Takes::Text, true,
	  "the DTE a named pipe another program makes (a VM's COM port)",
	  "none", false },
	{ 0, "tcp", Takes::Text, true,
	  "the DTE a TCP connection on [address:]port",
	  "127.0.0.1:2323", false },
	{ 0, "driver", Takes::Text, true,
	  "the DTE D-Modem's virtual modem driver: its COM port's name "
	  "(COM7), or auto", "none", false },
#endif
};

/* an option of POSIX's alone (the pty's, ALSA's), neither taken nor
   shown on Windows (--usb and -r are both's) */
bool posix_only(int i)
{
#ifdef _WIN32
	return i == OPT_GROUP || i == OPT_PERM || i == OPT_ALSA ||
		i == OPT_ALSA_CTL;
#else
	(void)i;
	return false;
#endif
}

/* one reading of a command line */
class CommandLine {
public:
	/* argc arguments argv, not yet read */
	CommandLine(int argc, char *argv[])
		: argc_(argc), argv_(argv),
		  prog_(argc > 0 && argv[0] ? argv[0] : "dmodemd")
	{
		dsp::copy_array(opts_, OPTIONS);
	}

	/* every argument read (the usage and exit 1 on an error) */
	void read();
	/* the options' effects on opt and the log levels, or the exits of
	   -h, -v and --countrylist */
	void apply(Options &opt) const;

private:
	/* the option text (an argument without its dashes) is: its index,
	   and in *value what was glued to it (nullptr if nothing); -1 if
	   none */
	int match(const char *text, const char **value) const;
	/* the usage on stderr, exit 1 */
	[[noreturn]] void usage() const;

	int argc_;
	char **argv_;
	const char *prog_;
	const char *name_ = nullptr;
	Option opts_[OPT_COUNT];
};

/* rest is what follows an option's name or letter: it is the option's
   if nothing follows, or (for one that takes a value) '=' and the value,
   or a value of digits; the value into *value */
bool fits(const Option &o, const char *rest, const char **value)
{
	*value = nullptr;
	if (!*rest)
		return true;
	if (o.takes == Takes::Nothing)
		return false;
	if (*rest == '=') {
		*value = *(rest + 1) ? rest + 1 : nullptr;
		return true;
	}
	if (isdigit(static_cast<unsigned char>(*rest))) {
		*value = rest;
		return true;
	}
	return false;
}

/* each option in turn: by its whole long name, else by its letter */
int CommandLine::match(const char *text, const char **value) const
{
	for (int i = 0; i < OPT_COUNT; i++) {
		const Option &o = opts_[i];
		size_t len = std::string_view(o.name).size();
		if (posix_only(i))
			continue;
		if (dcore::starts_with(text, o.name) &&
		    fits(o, text + len, value))
			return i;
		if (o.letter && *text == o.letter &&
		    fits(o, text + 1, value))
			return i;
	}
	return -1;
}

/* the arguments in order: options (a value glued on, or the next
   argument if it may be one) and the name */
void CommandLine::read()
{
	for (int i = 1; i < argc_; i++) {
		const char *arg = argv_[i], *text = arg, *value;
		int k;

		if (*arg != '-') {
			if (name_)
				usage();
			name_ = arg;
			continue;
		}
		while (*text == '-')
			text++;
		k = match(text, &value);
		if (k < 0)
			usage();
		Option &o = opts_[k];
		if (o.takes != Takes::Nothing && !value && i + 1 < argc_) {
			const char *next = argv_[i + 1];
			if (*next != '-' &&
			    (o.takes != Takes::Number ||
			     isdigit(static_cast<unsigned char>(*next)))) {
				value = next;
				i++;
			}
		}
		if (o.takes != Takes::Nothing && o.mandatory && !value)
			usage();
		o.given = true;
		if (value)
			o.value = value;
	}
}

/* see above */
void CommandLine::usage() const
{
	fprintf(stderr,
		"Usage: %s [option...] [name]\n"
		"\n"
#ifdef _WIN32
		"The name is the modem's (default %s): its data file\n"
		"%%LOCALAPPDATA%%\\dmodem\\data.<name>, its log\n"
		"dmodem.log.<name>.  The DTE talks to it on a COM port\n"
		"(--com), a named pipe (--pipe, --pipe-connect), D-Modem's\n"
		"virtual modem driver (--driver) or by TCP (--tcp, the\n"
		"default).\n"
#else
		"The name is the modem's (default %s): its pty's link is\n"
		"/dev/ttyDM<n> (n the digits it ends with), its data file\n"
		"/var/lib/dmodem/data.<name>, its log dmodem.log.<name>.\n"
#endif
		"\n"
		"Options:\n", prog_, DEFAULT_NAME);
	for (int i = 0; i < OPT_COUNT; i++) {
		const Option &o = opts_[i];
		if (posix_only(i))
			continue;
		const char *eq = o.takes != Takes::Nothing ? "=VAL" : "";
		std::string head = o.letter ?
			std::string("  -") + o.letter + ", --" : "      --";

		head += std::string(o.name) + eq;
		/* the help from column 24, a space after the option at
		   least */
		head.append(head.size() + 1 > 24 ? 1 : 24 - head.size(), ' ');
		fprintf(stderr, "%s%s", head.c_str(), o.help);
		if (o.takes != Takes::Nothing)
			fprintf(stderr, " (default `%s')", o.value);
		fputc('\n', stderr);
	}
	fprintf(stderr,
		"\n"
		"Environment:\n"
#ifdef _WIN32
		"  SIP_LOGIN=user:password@domain  the SIP account "
		"(required)\n"
#else
		"  SIP_LOGIN=user:password@domain  the SIP account "
		"(required without -a)\n"
#endif
		"  SIP_PORT=port         the local SIP port (5060)\n"
		"  SIP_LOG_LEVEL=n       the SIP log's level (4: with the "
		"SIP messages,\n"
		"                        3: the calls)\n");
	exit(1);
}

/* the countries of homolog_set, their T.35 code and name, then those
   of the ITU's list of tones (tones.h) that it has not, on stderr */
void list_countries()
{
	for (const struct homolog_set *h = homolog_set; h->name; h++)
		fprintf(stderr, "%02x: %s\n", static_cast<unsigned>(h->id),
			h->name);
	fprintf(stderr, "and with the USA's line settings and their own "
		"busy tones:\n");
	for (const tones::Country *c = tones::COUNTRIES; c->name; c++) {
		const struct homolog_set *h = homolog_set;
		while (h->name && tones::find(h->name) != c)
			h++;
		if (!h->name)
			fprintf(stderr, "    %s\n", c->name);
	}
}

/* see above */
void CommandLine::apply(Options &opt) const
{
	const Option *o = opts_;

	if (o[OPT_HELP].given || o[OPT_USAGE].given)
		usage();
	if (o[OPT_VERSION].given) {
		print_version();
		exit(0);
	}
	if (o[OPT_COUNTRYLIST].given) {
		list_countries();
		exit(0);
	}
	if (o[OPT_COUNTRY].given)
		opt.country = o[OPT_COUNTRY].value;
	if (o[OPT_GROUP].given)
		opt.group = o[OPT_GROUP].value;
	if (o[OPT_PERM].given) {
		int32_t perm = 0;
		if (!dcore::parse_long(o[OPT_PERM].value, perm, 8) || perm <= 0)
			usage();
		opt.perm = static_cast<mode_t>(perm);
	}
	if (o[OPT_NORT].given)
		opt.realtime = false;
	if (o[OPT_DEBUG].given) {
		int32_t level = 0;
		dcore::parse_long(o[OPT_DEBUG].value, level, 0);
		dcore::log_level = level > 0 ? static_cast<unsigned>(level) : 1;
	}
	if (o[OPT_LOG].given) {
		int32_t level = 0;
		dcore::parse_long(o[OPT_LOG].value, level, 0);
		dcore::log_data_level =
			level > 0 ? static_cast<unsigned>(level) : 5;
	}
	if (o[OPT_LISTEN].given)
		opt.listen = true;
	if (o[OPT_ALSA].given)
		opt.alsa = o[OPT_ALSA].value;
	if (o[OPT_ALSA_CTL].given)
		opt.alsa_ctl = o[OPT_ALSA_CTL].value;
	if (o[OPT_USB].given)
		opt.usb = o[OPT_USB].value;
	if (o[OPT_USB_LEDS].given)
		opt.usb_leds = true;
	if (o[OPT_USB_WAKE].given)
		opt.usb_wake = true;
	if (o[OPT_USB_HANDSET].given)
		opt.usb_handset = true;
	if (o[OPT_USB_INTERNATIONAL].given)
		opt.usb_international = true;
	if (o[OPT_USB_VOICE_CODEC].given) {
		const char *v = o[OPT_USB_VOICE_CODEC].value;
		const std::string_view c = v;
		if (c != "ulaw" && c != "alaw" && c != "linear") {
			fprintf(stderr, "%s: --usb-voice-codec: ulaw, alaw or "
				"linear\n", prog_);
			exit(1);
		}
		opt.usb_voice_codec = v;
	}
	if (o[OPT_USB_RHOM].given)
		opt.usb_rhom = true;
	if (o[OPT_USB_BUZZER].given)
		opt.usb_buzzer = true;
	if (opt.alsa && opt.usb) {
		fprintf(stderr, "%s: -a or --usb, not both\n", prog_);
		exit(1);
	}
	if (o[OPT_RATE].given) {
		int32_t rate = 0;
		dcore::parse_long(o[OPT_RATE].value, rate);
		if (rate != 8000 && rate != 9600) {
			fprintf(stderr, "%s: -r: 8000 or 9600\n", prog_);
			exit(1);
		}
		opt.rate = static_cast<uint32_t>(rate);
	}
#ifdef _WIN32
	if (o[OPT_COM].given)
		opt.com = o[OPT_COM].value;
	if (o[OPT_PIPE].given)
		opt.pipe = o[OPT_PIPE].value;
	if (o[OPT_PIPE_CONNECT].given)
		opt.pipe_connect = o[OPT_PIPE_CONNECT].value;
	if (o[OPT_TCP].given)
		opt.tcp = o[OPT_TCP].value;
	if (o[OPT_DRIVER].given)
		opt.driver = o[OPT_DRIVER].value;
#endif
	opt.name = name_ ? name_ : DEFAULT_NAME;
}

} // namespace

/* the whole command line read first, then acted on: -h and -u before
   -v, -v before --countrylist, then the settings */
void parse_options(int argc, char *argv[], Options &opt)
{
	CommandLine cl(argc, argv);

	cl.read();
	cl.apply(opt);
}

/* "<product>: version <version> <date> <time>", the date and time this
   file was compiled, on stdout and at level 1 in the log */
void print_version()
{
	const std::string line = std::string(dcore::Modem::product) +
		": version " + dcore::Modem::version + " " + __DATE__ + " " +
		__TIME__;
	printf("%s\n", line.c_str());
	fflush(stdout);
	dcore::log_printf(1, "%s\n", line.c_str());
}

} // namespace dmd
