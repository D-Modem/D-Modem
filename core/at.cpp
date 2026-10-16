/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    at.cpp - the modem core's AT command interpreter (dcore.h): the
 *             DTE's characters in command state made into command lines
 *             (echo, backspace, A/, the line's end), and each line run
 *             command by command, V.250's and D-Modem's own, and those
 *             of other modems' setup strings, taken with no effect
 *
 *    What a command does to the call (ATD, ATA, ATH, ATO, ATZ, +FCLASS,
 *    the voice engine) and the result codes are modem.cpp's; the
 *    interpreter decides whether a line ends with OK, ERROR or no result
 *    yet, and prints the information commands' text.  The AT manual
 *    (docs/at/at_commands.tex) describes every command.
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dsplibs.h>

#include "dcore.h"

namespace dcore {

namespace {

/* the longest command line: at_line_ holds it and its end */
const unsigned LINE_CHARS = 63;
/* &V's lines at most */
const int PROFILE_COLUMNS = 72;

/* c is a decimal digit */
bool is_digit(char c)
{
	return isdigit((unsigned char)c) != 0;
}

/* c in upper case */
int upper(char c)
{
	return toupper((unsigned char)c);
}

/* c a DTMF digit of V.253: 0-9, *, #, A-D (a-d) */
bool dtmf_digit(char c)
{
	return c && strchr("0123456789*#ABCDabcd", c) != nullptr;
}

/* the number at p as C's strtoul() with base 0 reads it (0x hex, 0
   octal, else decimal), *end after it (p if there are no digits: 0) */
unsigned long number(const char *p, const char **end)
{
	char *e;
	unsigned long v = strtoul(p, &e, 0);
	*end = e;
	return v;
}

/* the decimal digits at p, *end after them (p and 0 if there are
   none) */
unsigned long decimal(const char *p, const char **end)
{
	char *e;
	unsigned long v;
	if (!is_digit(*p)) {
		*end = p;
		return 0;
	}
	v = strtoul(p, &e, 10);
	*end = e;
	return v;
}

/* v is an item of list: single values and ranges a-b, separated by
   commas, up to the list's end or a ')' */
bool in_list(const char *list, unsigned long v)
{
	while (is_digit(*list)) {
		const char *e;
		unsigned long lo = decimal(list, &e), hi = lo;
		if (*e == '-')
			hi = decimal(e + 1, &e);
		if (v >= lo && v <= hi)
			return true;
		if (*e != ',')
			break;
		list = e + 1;
	}
	return false;
}

/* the list of range group pos (from 0) of ranges "(...),(...)", after
   its '('; NULL if there is no such group */
const char *range_group(const char *ranges, unsigned pos)
{
	const char *p = ranges;
	for (unsigned i = 0;; i++) {
		p = strchr(p, '(');
		if (!p)
			return nullptr;
		if (i == pos)
			return p + 1;
		p++;
	}
}

/*
 *    other modems' setup strings
 */

/* basic commands taken with no effect: the name, the values taken (with
   no number 0, but B needs one) */
const struct {
	const char *name, *values;
} taken_cmds[] = {
	{ "B", "0-3,15,16" }, { "C", "1" }, { "F", "1" }, { "W", "0-2" },
	{ "&B", "0-2" }, { "&G", "0-2" }, { "&I", "0-5" }, { "&J", "0-1" },
	{ "&K", "4-6" }, { "&M", "0,4,5" }, { "&N", "0-14" }, { "&W", "0-1" },
	{ "&Y", "0-1" }, { "\\G", "0-1" }, { "\\J", "0-1" }, { "\\K", "0-5" },
	{ "\\Q", "0-3" }, { "\\R", "0-1" }, { "\\T", "0-255" },
	{ "-C", "0-1" }, { "-K", "0-2" }, { "-Q", "0-1" }, { ")M", "0-2" },
	{ "@M", "0-30" }, { "*H", "0-2" },
};

/* the command at p is a taken one: *len its characters */
bool taken(const char *p, int *len)
{
	for (const auto &t : taken_cmds) {
		size_t n = strlen(t.name);
		const char *e;
		unsigned long v;
		if (strncasecmp(p, t.name, n))
			continue;
		if (!is_digit(p[n]) && !strcmp(t.name, "B"))
			continue;
		v = decimal(p + n, &e);
		if (!in_list(t.values, v))
			continue;
		*len = (int)(e - p);
		return true;
	}
	return false;
}

/* extended parameters kept with no effect but for +ES (which modem.cpp
   reads): the name, the values at the start, the range groups (one a
   value) */
const struct Kept {
	const char *name;
	unsigned n;
	unsigned long values[Modem::KEPT_VALUES];
	const char *ranges;
} kept_params[] = {
	{ "+IFC", 2, { 2, 2 }, "(0-3),(0-2)" },
	{ "+ICF", 2, { 3, 3 }, "(1-6),(0-3)" },
	{ "+IPR", 1, { 0 }, "(0,300,1200,2400,4800,9600,19200,38400,57600,"
	  "115200,230400,460800,921600)" },
	{ "+ILRR", 1, { 0 }, "(0,1)" },
	{ "+IDSR", 1, { 0 }, "(0,1)" },
	{ "+ILSD", 1, { 0 }, "(0,1)" },
	{ "+MR", 1, { 0 }, "(0,1)" },
	{ "+ER", 1, { 0 }, "(0,1)" },
	{ "+DR", 1, { 0 }, "(0,1)" },
	{ "+ES", 3, { 3, 0, 2 }, "(0-6),(0-4),(0-8)" },
	{ "+DS", 4, { 3, 0, 2048, 32 }, "(0-3),(0,1),(512-65535),(6-250)" },
	{ "+DS44", 9, { 3, 0, 0, 2048, 2048, 255, 255, 6144, 6144 },
	  "(0-3),(0,1),(0,1),(256-65535),(256-65535),(32-255),(32-255),"
	  "(512-65535),(512-65535)" },
	{ "+PCW", 1, { 2 }, "(0-2)" },
	{ "+PIG", 1, { 1 }, "(0,1)" },
	{ "+FAA", 1, { 0 }, "(0)" },
	{ "+FPR", 1, { 0 }, "(0,1,2,4,8,10,18)" },
	{ "-SEC", 1, { 0 }, "(0,1),(0-30)" },
};
const int KEPT_COUNT = sizeof(kept_params) / sizeof(kept_params[0]);
static_assert(KEPT_COUNT <= Modem::KEPT_PARAMS,
	      "the kept parameters do not fit the modem's storage");

/* the kept parameter whose name (any case) is at p followed by '=' or
   '?'; -1 if none is */
int kept_command(const char *p)
{
	for (int i = 0; i < KEPT_COUNT; i++) {
		size_t n = strlen(kept_params[i].name);
		if (!strncasecmp(p, kept_params[i].name, n) &&
		    (p[n] == '=' || p[n] == '?'))
			return i;
	}
	return -1;
}

/* the kept parameter called name; -1 if there is none */
int kept_index(const char *name)
{
	for (int i = 0; i < KEPT_COUNT; i++)
		if (!strcmp(kept_params[i].name, name))
			return i;
	return -1;
}

/* \N0 to \N5 as +ES's orig_rqst, orig_fbk, ans_fbk */
const unsigned long ec_modes[6][3] = {
	{ 1, 0, 1 }, { 0, 0, 0 }, { 3, 2, 4 }, { 3, 0, 2 }, { 3, 3, 5 },
	{ 4, 4, 6 },
};

/*
 *    +MS
 */

/* +MS's carrier names (V.250 Table 13, USRobotics' Bell ones, K56flex) */
const struct {
	const char *name;
	unsigned id;
} ms_names[] = {
	{ "V21", DP_V21 }, { "V22", DP_V22 }, { "V22B", DP_V22BIS },
	{ "V23C", DP_V23 }, { "V32", DP_V32 }, { "V32B", DP_V32BIS },
	{ "V34", DP_V34 }, { "V90", DP_V90 }, { "V91", DP_V91 },
	{ "V92", DP_V92 }, { "B103", DP_B103 }, { "BELL103", DP_B103 },
	{ "B212", DP_B212 }, { "BELL212A", DP_B212 }, { "K56", DP_K56 },
};

/* pump id is registered */
bool registered(unsigned long id)
{
	for (const DpDriver *d = dp_drivers(); d->id; d++)
		if ((unsigned long)d->id == id)
			return d->op != nullptr;
	return false;
}

/* +MS may choose pump id: it has a code and is registered (K56flex
   while V.90 is, which runs for it) */
bool ms_choosable(unsigned long id)
{
	for (const DpDriver *d = dp_drivers(); d->id; d++)
		if ((unsigned long)d->id == id)
			return d->code && (d->op || (id == DP_K56 &&
						     registered(DP_V90)));
	return false;
}

/* +GMI, +GMM, +GMR (and +FMx) by their last letter: the author, the
   product, the version; NULL for another letter */
const char *identity(char c)
{
	switch (upper(c)) {
	case 'I':
		return Modem::author;
	case 'M':
		return Modem::product;
	case 'R':
		return Modem::version;
	default:
		return nullptr;
	}
}

/*
 *    voice
 */

/* a +V parameter command: the two letters after +V, its values (the
   fields of struct voice_info, none for +VNH and +VLS), their ranges
   (a bound 0: none) */
const struct VoiceSetting {
	const char *name;
	int n;
	unsigned voice_info::*field[2];
	unsigned lo[2], hi[2];
} voice_settings[] = {
	{ "NH", 1, { nullptr, nullptr }, { 0, 0 }, { 1, 0 } },
	{ "GT", 1, { &voice_info::tx_gain, nullptr }, { 0, 0 }, { 128, 0 } },
	{ "GR", 1, { &voice_info::rx_gain, nullptr }, { 0, 0 }, { 128, 0 } },
	{ "IT", 1, { &voice_info::inactivity_timer, nullptr }, { 0, 0 },
	  { 255, 0 } },
	{ "SD", 2, { &voice_info::silence_detect_sensitivity,
		     &voice_info::silence_detect_period }, { 0, 0 },
	  { 255, 255 } },
	{ "SM", 2, { &voice_info::comp_method, &voice_info::sample_rate },
	  { 0, 8000 }, { 1, 8000 } },
	{ "LS", 1, { nullptr, nullptr }, { 0, 0 }, { 1, 0 } },
	{ "TD", 1, { &voice_info::tone_default, nullptr }, { 0, 0 },
	  { 6000, 0 } },
};

/* v within the bounds lo and hi (0: none) */
bool in_bounds(unsigned long v, unsigned lo, unsigned hi)
{
	return !(lo && v < lo) && !(hi && v > hi);
}

} // namespace

/*
 *    command lines
 */

/* the DTE's characters waiting in the buffer as command text, one at a
   time: echoed, edited, made into lines, and each line run when it
   ends; a line that hands the DTE's characters to the fax engine stops
   it there */
void Modem::at_process()
{
	/* the line ends: blanks out, a command line runs (its result), any
	   other is blanked out on the DTE's screen; true if the fax engine
	   now has the DTE's characters */
	auto line_end = [this]() {
		bool echo = sregs_[SREG_ECHO] != 0;
		char cmd[LINE_CHARS + 1];
		unsigned typed = at_count_, k = 0;
		int ret;

		for (unsigned i = 0; i < typed && at_line_[i]; i++)
			if (at_line_[i] != ' ' && at_line_[i] != '\t')
				cmd[k++] = at_line_[i];
		cmd[k] = '\0';
		at_count_ = 0;
		if (k < 2 || upper(cmd[0]) != 'A' || upper(cmd[1]) != 'T') {
			if (echo) {
				const char *cr = reinterpret_cast<const char *>(
					&sregs_[SREG_CR_CHAR]);
				put_chars(cr, 1);
				for (unsigned i = 0; i < typed; i++)
					put_chars(" ", 1);
				put_chars(cr, 1);
			}
			if (typed) {
				memset(at_cmd_, ' ', typed);
				at_cmd_[typed] = '\0';
			}
			return false;
		}
		if (echo)
			crlf();
		memcpy(at_cmd_, at_line_, typed);
		at_cmd_[typed] = '\0';
		logf("run cmd: %s\n", cmd);
		ret = at_command(cmd);
		if (!ret)
			report(Result::Ok);
		else if (ret < 0)
			report(Result::Error);
		return ret > 0 && !command_ && fax_;
	};
	char c;

	while (get_chars(&c, 1) == 1) {
		bool echo = sregs_[SREG_ECHO] != 0;
		char bs = (char)sregs_[SREG_BS_CHAR];

		/* A/: the last line again, with no line end */
		if (c == '/' && at_count_ == 1 && upper(at_line_[0]) == 'A') {
			size_t n = strlen(at_cmd_);
			if (n > 1) {
				memcpy(at_line_ + 1, at_cmd_ + 1, n - 1);
				at_count_ = n;
				if (echo)
					put_chars(at_cmd_ + 1, (int)n - 1);
			}
			if (echo)
				put_chars(reinterpret_cast<const char *>(
						  &sregs_[SREG_CR_CHAR]), 1);
			if (line_end())
				return;
			continue;
		}
		if (echo)
			put_chars(&c, 1);
		if (c == (char)sregs_[SREG_CR_CHAR] ||
		    c == (char)sregs_[SREG_LF_CHAR]) {
			if (line_end())
				return;
			continue;
		}
		if (c == bs && at_count_) {
			at_count_--;
			if (echo) {
				put_chars(" ", 1);
				put_chars(&bs, 1);
			}
			continue;
		}
		if (at_count_ >= LINE_CHARS) {
			/* too long: ERROR now, the rest a new line */
			if (echo)
				crlf();
			at_count_ = 0;
			at_cmd_[0] = '\0';
			report(Result::Error);
			continue;
		}
		at_line_[at_count_++] = c;
	}
}

/* a command line (AT and the commands, without blanks), each command in
   turn until one fails: 0 for OK, < 0 for ERROR, > 0 when the result
   comes later (the line ends there) */
int Modem::at_command(char *cmd)
{
	char *p;

	if (upper(cmd[0]) != 'A' || upper(cmd[1]) != 'T')
		return -1;
	for (p = cmd + 2; *p;) {
		int len = 1, ret = 0;
		/* a one-digit setting in S-register reg, skip characters
		   in */
		auto setting = [this, &p, &len](int reg, int skip, int lo,
						int hi) {
			int k, r = at_set_sreg(reg, p[skip], lo, hi, &k);
			len = skip + k;
			return r;
		};

		if (kept_command(p) >= 0) {
			ret = at_kept(p, &len);
		} else if (taken(p, &len)) {
			ret = 0;
		} else {
			switch (upper(*p)) {
			case ';':
				break;
			case 'A':
				ret = at_A(p, &len);
				break;
			case 'B':
				ret = at_B(p, &len);
				break;
			case 'D':
				ret = at_D(p, &len);
				break;
			case 'E':
				ret = setting(SREG_ECHO, 1, 0, 1);
				break;
			case 'H':
				ret = at_H(p, &len);
				break;
			case 'I':
				ret = at_I(p, &len);
				break;
			case 'L':
				ret = setting(SREG_SPEAKER_VOLUME, 1, 0, 3);
				if (!ret)
					update_speaker();
				break;
			case 'M':
				ret = setting(SREG_SPEAKER_CONTROL, 1, 0, 3);
				if (!ret)
					update_speaker();
				break;
			case 'N':
				ret = setting(SREG_AUTOMODE, 1, 0, 1);
				break;
			case 'O':
				ret = at_O(p, &len);
				break;
			case 'P':
				sregs_[SREG_TONE_OR_PULSE] = 0;
				break;
			case 'Q':
				ret = setting(SREG_QUIET, 1, 0, 1);
				break;
			case 'S':
				ret = at_S(p, &len);
				break;
			case 'T':
				sregs_[SREG_TONE_OR_PULSE] = 1;
				break;
			case 'V':
				ret = setting(SREG_VERBOSE, 1, 0, 1);
				break;
			case 'X':
				ret = setting(SREG_X_CODE, 1, 0, 4);
				break;
			case 'Y':
				ret = setting(SREG_DEFAULT_SETTING, 1, 0, 3);
				break;
			case 'Z':
				ret = at_Z(p, &len);
				break;
			case '#':
				ret = strncasecmp(p, "#CID", 4) ? -1 :
					at_CID(p, &len);
				break;
			case '%':
				switch (upper(p[1])) {
				case 'B':
					len = 2;
					break;
				case 'C':
					ret = setting(SREG_COMP, 2, 0, 3);
					break;
				case 'E':
					ret = setting(SREG_LINE_QUALITY_CONTROL,
						      2, 0, 2);
					break;
				default:
					ret = -1;
					break;
				}
				break;
			case '&':
				switch (upper(p[1])) {
				case 'A':
					ret = setting(SREG_CONNNECT_MSG_FORMAT,
						      2, 0, 7);
					break;
				case 'C':
					ret = setting(SREG_CD, 2, 0, 1);
					break;
				case 'D':
				case 'F':
				case 'R':
				case 'S':
					len = 2 + is_digit(p[2]);
					break;
				case 'E':
					ret = setting(
						SREG_CONNNECT_MSG_SPEED_SRC, 2,
						0, 1);
					break;
				case 'H':
					ret = setting(SREG_FLOW_CONTROL, 2, 0,
						      3);
					break;
				case 'K': {
					/* USRobotics': as %C */
					static const unsigned char comp[] = {
						0, 3, 3, 2,
					};
					ret = setting(SREG_COMP, 2, 0, 3);
					if (!ret)
						sregs_[SREG_COMP] =
							comp[sregs_[SREG_COMP]];
					break;
				}
				case 'P':
					ret = setting(SREG_PULSE_RATIO, 2, 0,
						      3);
					break;
				case 'Q': {
					/* other modems' error control, as
					   \N0 or \N3 */
					int d = is_digit(p[2]) ? p[2] - '0' : 0;
					len = 2 + is_digit(p[2]);
					if (d == 0 || d == 6)
						set_ec_mode(0);
					else if (d == 5 || d == 8 || d == 9)
						set_ec_mode(3);
					else
						ret = -1;
					break;
				}
				case 'V':
					ret = at_ampV(p, &len);
					break;
				default:
					ret = -1;
					break;
				}
				break;
			case '*':
				if (upper(p[1]) == 'B')
					len = 2;
				else
					ret = -1;
				break;
			case '\\':
				switch (upper(p[1])) {
				case 'A':
					ret = setting(SREG_MNP_BLOCK, 2, 0, 3);
					break;
				case 'N':
					ret = setting(SREG_EC, 2, 0, 5);
					if (!ret)
						set_ec_mode(sregs_[SREG_EC]);
					break;
				case 'V':
					ret = setting(SREG_CONNECT_PROTOCOL, 2,
						      0, 2);
					break;
				default:
					ret = -1;
					break;
				}
				break;
			case '+':
				switch (upper(p[1])) {
				case 'F':
					ret = strncasecmp(p, "+FCLASS", 7) ?
						at_fax(p, &len) :
						at_FCLASS(p, &len);
					break;
				case 'G':
					if (!strncasecmp(p, "+GCI", 4)) {
						ret = at_GCI(p, &len);
					} else if (!strncasecmp(p, "+GCAP",
								5)) {
						puts("+GCAP: +FCLASS,+MS");
						crlf();
						len = 5;
					} else if (upper(p[2]) == 'M' &&
						   identity(p[3])) {
						puts(identity(p[3]));
						crlf();
						len = p[4] == '?' ? 5 : 4;
					} else {
						ret = -1;
					}
					break;
				case 'M':
					ret = at_MS(p, &len);
					break;
				case 'P':
					ret = at_PMH(p, &len);
					break;
				case 'V':
					ret = at_voice(p, &len);
					break;
				default:
					ret = -1;
					break;
				}
				break;
			default:
				ret = -1;
				break;
			}
		}
		if (ret)
			return ret;
		p += len;
	}
	return 0;
}

/* a one-digit setting: S-register num to the digit c (none: 0) if it is
   within lo and hi, *len the digits read (0 or 1); 0, or -1 (the
   register unchanged) */
int Modem::at_set_sreg(int num, char c, int lo, int hi, int *len)
{
	int v = 0;
	*len = 0;
	if (is_digit(c)) {
		v = c - '0';
		*len = 1;
	}
	if (v < lo || v > hi)
		return -1;
	sregs_[num] = (unsigned char)v;
	return 0;
}

/*
 *    basic commands
 */

/* ATA: answers (the held call too); its result later, or ERROR */
int Modem::at_A(char *p, int *len)
{
	*len = (int)strlen(p);
	return answer() ? -1 : 1;
}

/* ATB alone: V.22 at 1200 bit/s, no automode (+MS=22,0,1200,1200) */
int Modem::at_B(char *, int *len)
{
	*len = 1;
	sregs_[SREG_DP] = DP_V22;
	sregs_[SREG_AUTOMODE] = 0;
	min_rate_ = max_rate_ = 1200;
	return 0;
}

/* ATD<dial string>: the rest of the line; after its first ';' the rest
   of the line runs once the number is dialled; ATDL the last number
   again; S=<n> (a stored number) ERROR; its result later, or ERROR */
int Modem::at_D(char *p, int *len)
{
	char *s = p + 1, *semi = strchr(s, ';');
	char number[LINE_CHARS + 1];
	size_t n;

	*len = (int)strlen(p);
	dial_rest_[0] = '\0';
	for (char *q = s; *q && q != semi; q++)
		if (upper(q[0]) == 'S' && q[1] == '=' && is_digit(q[2]))
			return -1;
	if (upper(*s) == 'L') {
		if (dial_string_[0]) {
			puts("Dialing ");
			puts(dial_string_);
			puts("...");
			crlf();
		}
		return dial(nullptr) ? -1 : 1;
	}
	if (upper(*s) == 'T')
		sregs_[SREG_TONE_OR_PULSE] = 1;
	else if (upper(*s) == 'P')
		sregs_[SREG_TONE_OR_PULSE] = 0;
	n = semi ? (size_t)(semi + 1 - s) : strlen(s);
	if (n > LINE_CHARS)
		n = LINE_CHARS;
	memcpy(number, s, n);
	number[n] = '\0';
	if (semi) {
		size_t rest = strlen(semi + 1);
		if (rest > sizeof(dial_rest_) - 3)
			rest = sizeof(dial_rest_) - 3;
		memcpy(dial_rest_, "AT", 2);
		memcpy(dial_rest_ + 2, semi + 1, rest);
		dial_rest_[rest + 2] = '\0';
	}
	return dial(number) ? -1 : 1;
}

/* ATH, ATH0 on hook (a call hangs up), ATH1 off hook; ERROR when the
   hook does not change, or for another digit */
int Modem::at_H(char *p, int *len)
{
	int state = MODEM_HOOK_ON;
	*len = 1;
	if (is_digit(p[1])) {
		if (p[1] == '1')
			state = MODEM_HOOK_OFF;
		else if (p[1] != '0' || is_digit(p[2]))
			return -1;
		*len = 2;
	}
	return hook(state) ? -1 : 0;
}

/* ATI<n>: what the modem is, its S-registers, its country, the last
   call's statistics */
int Modem::at_I(char *p, int *len)
{
	const char *e;
	unsigned long n = number(p + 1, &e);
	char line[64];

	*len = (int)(e - p);
	switch (n) {
	case 0:
		puts(product);
		crlf();
		break;
	case 1:
	case 2:
		snprintf(line, sizeof(line), "%s, %s", product, version);
		puts(line);
		crlf();
		puts(author);
		crlf();
		break;
	case 3: {
		const char *driver = host_.driver_name();
		puts(dev_name_ ? dev_name_ : "unknown");
		crlf();
		puts(driver ? driver : "unknown");
		crlf();
		break;
	}
	case 4:
		for (int i = 0; i < 24; i++) {
			snprintf(line, sizeof(line), "s%02d=%03d ", i,
				 sregs_[i]);
			puts(line);
			if (i % 8 == 7)
				crlf();
		}
		crlf();
		break;
	case 5:
	case 6:
		snprintf(line, sizeof(line), "Stored Profile %lu:", n - 5);
		puts(line);
		crlf();
		break;
	case 7:
		puts("Country: ");
		puts(homolog_->name);
		crlf();
		break;
	case 11:
		call_stats();
		break;
	default:
		return -1;
	}
	return 0;
}

/* ATO: back online (or the held call's handshake); its result later, or
   ERROR */
int Modem::at_O(char *p, int *len)
{
	*len = (int)strlen(p);
	return online() ? -1 : 1;
}

/* ATS<n>? reads S-register n, ATS<n>=<v> sets it (S103 as \N) */
int Modem::at_S(char *p, int *len)
{
	const char *e;
	unsigned long n = number(p + 1, &e), v;
	char line[16];

	if (n >= sizeof(sregs_))
		return -1;
	if (*e == '?') {
		snprintf(line, sizeof(line), "%u", sregs_[n]);
		puts(line);
		crlf();
		*len = (int)(e + 1 - p);
		return 0;
	}
	if (*e != '=')
		return -1;
	v = number(e + 1, &e);
	set_sreg((unsigned)n, (int)v);
	if (n == SREG_EC && v <= 5)
		set_ec_mode((int)v);
	*len = (int)(e - p);
	return 0;
}

/* ATZ: the modem as at the start (but the country), the rest of the
   line not read */
int Modem::at_Z(char *p, int *len)
{
	*len = (int)strlen(p);
	return reset();
}

/* AT&V, AT&V0: the active settings, in lines of PROFILE_COLUMNS at
   most */
int Modem::at_ampV(char *p, int *len)
{
	static const unsigned char regs[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24,
		28, 29, 30, 31, 32, 39, 56, 60, 62, 70, 71, 92, 103, 104, 105,
		106, 161,
	};
	char item[96];
	int col = 0;
	/* an item of the current group, on a new line if it does not fit
	   on this one */
	auto put = [this, &col](const char *s) {
		int n = (int)strlen(s);
		if (col && col + 1 + n > PROFILE_COLUMNS) {
			crlf();
			col = 0;
		}
		if (col) {
			puts(" ");
			col++;
		}
		puts(s);
		col += n;
	};
	/* the group ends with its line */
	auto end = [this, &col]() {
		if (col)
			crlf();
		col = 0;
	};

	if (is_digit(p[2]) && p[2] != '0')
		return -1;
	*len = 2 + is_digit(p[2]);
	puts("ACTIVE PROFILE:");
	crlf();
	snprintf(item, sizeof(item), "E%u", sregs_[SREG_ECHO]);
	put(item);
	snprintf(item, sizeof(item), "L%u", sregs_[SREG_SPEAKER_VOLUME]);
	put(item);
	snprintf(item, sizeof(item), "M%u", sregs_[SREG_SPEAKER_CONTROL]);
	put(item);
	snprintf(item, sizeof(item), "N%u", sregs_[SREG_AUTOMODE]);
	put(item);
	snprintf(item, sizeof(item), "Q%u", sregs_[SREG_QUIET]);
	put(item);
	put(sregs_[SREG_TONE_OR_PULSE] ? "T" : "P");
	snprintf(item, sizeof(item), "V%u", sregs_[SREG_VERBOSE]);
	put(item);
	snprintf(item, sizeof(item), "X%u", sregs_[SREG_X_CODE]);
	put(item);
	snprintf(item, sizeof(item), "Y%u", sregs_[SREG_DEFAULT_SETTING]);
	put(item);
	snprintf(item, sizeof(item), "&A%u", sregs_[SREG_CONNNECT_MSG_FORMAT]);
	put(item);
	snprintf(item, sizeof(item), "&C%u", sregs_[SREG_CD]);
	put(item);
	snprintf(item, sizeof(item), "&E%u",
		 sregs_[SREG_CONNNECT_MSG_SPEED_SRC]);
	put(item);
	snprintf(item, sizeof(item), "&H%u", sregs_[SREG_FLOW_CONTROL]);
	put(item);
	snprintf(item, sizeof(item), "&P%u", sregs_[SREG_PULSE_RATIO]);
	put(item);
	snprintf(item, sizeof(item), "\\A%u", sregs_[SREG_MNP_BLOCK]);
	put(item);
	snprintf(item, sizeof(item), "\\N%u", sregs_[SREG_EC]);
	put(item);
	snprintf(item, sizeof(item), "\\V%u", sregs_[SREG_CONNECT_PROTOCOL]);
	put(item);
	snprintf(item, sizeof(item), "%%C%u", sregs_[SREG_COMP]);
	put(item);
	snprintf(item, sizeof(item), "%%E%u",
		 sregs_[SREG_LINE_QUALITY_CONTROL]);
	put(item);
	end();
	for (unsigned char r : regs) {
		snprintf(item, sizeof(item), "S%02u:%03u", r, sregs_[r]);
		put(item);
	}
	end();
	snprintf(item, sizeof(item), "+MS=%u,%u,%u,%u", sregs_[SREG_DP],
		 sregs_[SREG_AUTOMODE], min_rate_, max_rate_);
	put(item);
	snprintf(item, sizeof(item), "+FCLASS=%d", (int)mode_);
	put(item);
	snprintf(item, sizeof(item), "+GCI=%x", (unsigned)homolog_->id);
	put(item);
	snprintf(item, sizeof(item), "+PMH=%d", moh_off_);
	put(item);
	snprintf(item, sizeof(item), "+PMHT=%d", moh_timer_);
	put(item);
	snprintf(item, sizeof(item), "#CID=%u", cid_requested_);
	put(item);
	for (int i = 0; i < KEPT_COUNT; i++) {
		int k = snprintf(item, sizeof(item), "%s=",
				 kept_params[i].name);
		for (unsigned j = 0; j < kept_n_[i]; j++)
			k += snprintf(item + k, sizeof(item) - k, "%s%lu",
				      j ? "," : "", kept_[i][j]);
		put(item);
	}
	end();
	puts("STORED PROFILES: none");
	crlf();
	return 0;
}

/*
 *    extended commands
 */

/* +MS?, +MS=?, +MS=[<mod>][,[<automode>][,[<min>][,[<max>]]]]: the
   modulation (a pump's number or a carrier's name), automode and the
   rates; an item left out stays as it is */
int Modem::at_MS(char *p, int *len)
{
	const char *q = p + 3, *e;
	unsigned long mod = sregs_[SREG_DP], automode = sregs_[SREG_AUTOMODE];
	unsigned long min = min_rate_, max = max_rate_, v;
	char line[128];

	if (upper(p[2]) != 'S')
		return -1;
	if (*q == '?') {
		snprintf(line, sizeof(line), "%u,%u,%u,%u", sregs_[SREG_DP],
			 sregs_[SREG_AUTOMODE], min_rate_, max_rate_);
		puts(line);
		crlf();
		*len = (int)(q + 1 - p);
		return 0;
	}
	if (*q++ != '=')
		return -1;
	if (*q == '?') {
		int k = snprintf(line, sizeof(line), "(");
		for (const DpDriver *d = dp_drivers(); d->id; d++)
			if (d->code && ms_choosable(d->id))
				k += snprintf(line + k, sizeof(line) - k,
					      "%s%d", k > 1 ? "," : "",
					      (int)d->id);
		snprintf(line + k, sizeof(line) - k,
			 "),(0,1),(%d-%d),(%d-%d)", MODEM_MIN_RATE,
			 MODEM_MAX_RATE, MODEM_MIN_RATE, MODEM_MAX_RATE);
		puts(line);
		crlf();
		*len = (int)(q + 1 - p);
		return 0;
	}
	if (isalpha((unsigned char)*q)) {
		size_t n = 0;
		bool found = false;
		while (isalnum((unsigned char)q[n]))
			n++;
		for (const auto &m : ms_names)
			if (strlen(m.name) == n &&
			    !strncasecmp(q, m.name, n)) {
				mod = m.id;
				found = true;
				break;
			}
		if (!found)
			return -1;
		q += n;
	} else {
		v = number(q, &e);
		if (e != q)
			mod = v;
		q = e;
	}
	if (*q == ',') {
		v = number(++q, &e);
		if (e != q)
			automode = v;
		q = e;
		if (*q == ',') {
			v = number(++q, &e);
			if (e != q)
				min = v;
			q = e;
			if (*q == ',') {
				v = number(++q, &e);
				if (e != q)
					max = v;
				q = e;
				if (*q == ',')
					q++;
			}
		}
	}
	if ((mod != sregs_[SREG_DP] && !ms_choosable(mod)) || automode > 1 ||
	    min > max || min < MODEM_MIN_RATE || max > MODEM_MAX_RATE)
		return -1;
	sregs_[SREG_DP] = (unsigned char)mod;
	sregs_[SREG_AUTOMODE] = (unsigned char)automode;
	min_rate_ = (unsigned)min;
	max_rate_ = (unsigned)max;
	*len = (int)(q - p);
	return 0;
}

/* +GCI?, +GCI=?, +GCI=<hex>: the country by its T.35 code */
int Modem::at_GCI(char *p, int *len)
{
	const char *q = p + 4;
	char line[16];

	if (*q == '?') {
		snprintf(line, sizeof(line), "+GCI:%x",
			 (unsigned)homolog_->id);
		puts(line);
		crlf();
		*len = 5;
		return 0;
	}
	if (*q++ != '=')
		return -1;
	if (*q == '?') {
		puts("+GCI:(");
		for (const struct homolog_set *h = homolog_set; h->name; h++) {
			snprintf(line, sizeof(line), "%x,", (unsigned)h->id);
			puts(line);
		}
		puts(")");
		crlf();
		*len = 6;
		return 0;
	}
	{
		char *e;
		unsigned long code = strtoul(q, &e, 16);
		if (e == q || code > 0xffff || homolog_init((int)code, nullptr))
			return -1;
		*len = (int)(e - p);
	}
	return 0;
}

/* +FCLASS=?, +FCLASS?, +FCLASS=<d>: data (0), class 1 fax (1), voice
   (8) */
int Modem::at_FCLASS(char *p, int *len)
{
	const char *q = p + 7;
	char line[8];

	if (q[0] == '=' && q[1] == '?') {
		puts("0,1,8");
		crlf();
		*len = 9;
		return 0;
	}
	if (q[0] == '?') {
		snprintf(line, sizeof(line), "%d", (int)mode_);
		puts(line);
		crlf();
		*len = 8;
		return 0;
	}
	if (q[0] == '=' && (q[1] == '0' || q[1] == '1' || q[1] == '8')) {
		*len = 9;
		return set_mode((enum MODEM_MODE)(q[1] - '0'));
	}
	return -1;
}

/* +FMI, +FMM, +FMR (T.31 Annex A); the class 1 commands +FTS, +FRS
   (silence), +FTM, +FRM (data), +FTH, +FRH (HDLC frames), for the fax
   engine (T.31 8.3) */
int Modem::at_fax(char *p, int *len)
{
	const char *q = p + 2, *e;
	char what = (char)upper(q[0]), kind = (char)upper(q[1]);
	enum FAX_CMD cmd;
	unsigned long v;

	if (what == 'M' && identity(kind)) {
		puts(identity(kind));
		crlf();
		*len = q[2] == '?' ? 5 : 4;
		return 0;
	}
	if ((what != 'T' && what != 'R') ||
	    (kind != 'S' && kind != 'M' && kind != 'H') || q[2] != '=')
		return -1;
	q += 3;
	switch (kind) {
	case 'S':
		cmd = what == 'T' ? FAX_CMD_FTS : FAX_CMD_FRS;
		v = number(q, &e);
		if (e == q)
			return -1;
		break;
	case 'M':
		if (*q == '?') {
			puts("24,48,72,73,74,96,97,98,121,122,145,146");
			crlf();
			*len = (int)(q + 1 - p);
			return 0;
		}
		cmd = what == 'T' ? FAX_CMD_FTM : FAX_CMD_FRM;
		v = number(q, &e);
		if (e == q)
			return -1;
		break;
	default:
		if (*q == '?') {
			puts("3");
			crlf();
			*len = (int)(q + 1 - p);
			return 0;
		}
		if (!is_digit(*q))
			return -1;
		cmd = what == 'T' ? FAX_CMD_FTH : FAX_CMD_FRH;
		v = *q - '0';
		e = q + 1;
		break;
	}
	*len = (int)(e - p);
	return fax_command(cmd, (unsigned)v);
}

/* a class 1 command for the fax engine: once it has started, the DTE's
   characters are the engine's until it reports (1); ERROR without an
   engine or when it refuses */
int Modem::fax_command(enum FAX_CMD cmd, unsigned val)
{
	int ret;
	if (!fax_)
		return -1;
	ret = FAX_class1_command(fax_, cmd, val);
	if (ret > 0) {
		command_ = false;
		return 1;
	}
	return ret < 0 ? -1 : 0;
}

/* V.92 modem-on-hold: +PMH (0 granted, 1 not), +PMHT (the time we
   grant, as V.250's code), +PMHR[=<s>] (we ask for s seconds, 10 if
   not said: online command state of a V.92 call only) */
int Modem::at_PMH(char *p, int *len)
{
	const char *q = p + 4, *e;
	char line[32];
	bool timer;
	unsigned long v;

	if (strncasecmp(p, "+PMH", 4))
		return -1;
	if (upper(*q) == 'R') {
		unsigned long secs = 10;
		q++;
		if (state_ != COMMAND_ONLINE || !dp_ || dp_->id != DP_V92)
			return -1;
		if (q[0] == '=' && is_digit(q[1])) {
			secs = decimal(q + 1, &e);
			if (secs < 1 || secs > 960)
				return -1;
			q = e;
		}
		moh_req_ = (int)secs;
		*len = (int)(q - p);
		return 0;
	}
	timer = upper(*q) == 'T';
	if (timer)
		q++;
	if (q[0] == '=' && q[1] == '?') {
		puts(timer ? "+PMHT: (0-13)" : "+PMH: (0-1)");
		crlf();
		q += 2;
	} else if (q[0] == '?') {
		snprintf(line, sizeof(line), timer ? "+PMHT: %d" : "+PMH: %d",
			 timer ? moh_timer_ : moh_off_);
		puts(line);
		crlf();
		q++;
	} else if (q[0] == '=' && is_digit(q[1])) {
		v = decimal(q + 1, &e);
		if (v > (timer ? 13u : 1u))
			return -1;
		if (timer)
			moh_timer_ = (int)v;
		else
			moh_off_ = (int)v;
		q = e;
	} else {
		return -1;
	}
	*len = (int)(q - p);
	return 0;
}

/* #CID, +VCID (V.253 9.2.3): caller ID, 0 off, 1 formatted, 2 the
   message as it is */
int Modem::at_CID(char *p, int *len)
{
	const char *q = p + (*p == '#' ? 4 : 5);
	char line[8];

	if (q[0] == '=' && q[1] == '?') {
		puts("0,1,2");
		crlf();
		q += 2;
	} else if (q[0] == '?') {
		snprintf(line, sizeof(line), "%u", cid_requested_);
		puts(line);
		crlf();
		q++;
	} else if (q[0] == '=' && q[1] >= '0' && q[1] <= '2') {
		cid_requested_ = q[1] - '0';
		q += 2;
	} else {
		return -1;
	}
	*len = (int)(q - p);
	return 0;
}

/* +V...: caller ID in any mode, the rest in voice mode only: the
   parameters (? =? =<values>), +VIP, +VLS, and what the voice engine
   does: +VTX, +VRX, +VTS (+VTR is not offered) */
int Modem::at_voice(char *p, int *len)
{
	const char *q = p + 4, *e;
	char cmd[3], line[64];

	if (!strncasecmp(p + 2, "CID", 3))
		return at_CID(p, len);
	if (mode_ != MODEM_MODE_VOICE || !p[2] || !p[3])
		return -1;
	cmd[0] = (char)upper(p[2]);
	cmd[1] = (char)upper(p[3]);
	cmd[2] = '\0';
	for (const auto &s : voice_settings) {
		bool device = !strcmp(s.name, "LS");
		unsigned long v[2] = { 0, 0 };
		int ret = 0;

		if (strcmp(s.name, cmd))
			continue;
		for (int i = 0; i < s.n; i++)
			v[i] = s.field[i] ? voice_info_.*s.field[i] :
				device && voice_ ? 1 : 0;
		if (q[0] == '=' && q[1] == '?') {
			int k = snprintf(line, sizeof(line), "+V%s=", s.name);
			for (int i = 0; i < s.n; i++)
				k += snprintf(line + k, sizeof(line) - k,
					      "%s%u-%u", i ? "," : "", s.lo[i],
					      s.hi[i]);
			puts(line);
			crlf();
			q += 2;
		} else if (q[0] == '?') {
			int k = snprintf(line, sizeof(line), "+V%s=", s.name);
			for (int i = 0; i < s.n; i++)
				k += snprintf(line + k, sizeof(line) - k,
					      "%s%lu", i ? "," : "", v[i]);
			puts(line);
			crlf();
			q++;
		} else if (q[0] == '=') {
			q++;
			for (int i = 0; i < s.n; i++) {
				if (i && *q++ != ',')
					return -1;
				v[i] = number(q, &e);
				q = e;
				if (!in_bounds(v[i], s.lo[i], s.hi[i]))
					return -1;
			}
			for (int i = 0; i < s.n; i++)
				if (s.field[i])
					voice_info_.*s.field[i] =
						(unsigned)v[i];
		} else {
			return -1;
		}
		*len = (int)(q - p);
		if (device)
			ret = voice_set_device((unsigned)v[0]);
		return ret;
	}
	if (!strcmp(cmd, "IP")) {
		voice_init();
		*len = 4;
		return 0;
	}
	*len = (int)strlen(p);
	if (!strcmp(cmd, "TX"))
		return voice_command(VOICE_CMD_STATE_TX);
	if (!strcmp(cmd, "RX"))
		return voice_command(VOICE_CMD_STATE_RX);
	if (!strcmp(cmd, "TR"))
		return voice_command(VOICE_CMD_STATE_DUPLEX);
	if (strcmp(cmd, "TS"))
		return -1;
	/* +VTS: the tone settings, or a tone or a DTMF digit played */
	if (q[0] == '?' || (q[0] == '=' && q[1] == '?')) {
		if (q[0] == '?')
			snprintf(line, sizeof(line), "+VTS=%u,%u,%u",
				 voice_info_.tone1_freq,
				 voice_info_.tone2_freq,
				 voice_info_.tone_duration);
		else
			snprintf(line, sizeof(line), "+VTS=0-4000,0-4000,0-6000");
		puts(line);
		crlf();
		*len = q[0] == '?' ? 5 : 6;
		return 0;
	}
	if (*q++ != '=')
		return -1;
	/* V.253's string: elements separated by commas, each a DTMF digit
	   (0-9, *, #, A-D), {s,d} the digit s for d * 10 ms, or [f1,f2,d]
	   a tone of f1 and f2 Hz (0 to 4000, 0: none) for d * 10 ms (0 to
	   6000).  A field left empty is 0 for a frequency, no digit
	   (silence) for s, and +VTD for d, so that a bare digit, {s}, [f1]
	   and [] last +VTD; all of the string checked before any of it
	   plays */
	std::vector<VoiceTone> tones;
	const unsigned dflt = voice_info_.tone_default ?
		voice_info_.tone_default : 100;
	for (;;) {
		VoiceTone t = { 0, 0, 0, dflt };
		if (*q == '[') {
			unsigned long v[3] = { 0, 0, dflt };
			q++;
			for (int i = 0; i < 3 && *q != ']'; i++) {
				if (i && *q++ != ',')
					return -1;
				if (is_digit(*q)) {
					v[i] = number(q, &e);
					q = e;
				}
			}
			if (*q++ != ']' || v[0] > 4000 || v[1] > 4000 ||
			    v[2] > 6000)
				return -1;
			t.f1 = (unsigned)v[0];
			t.f2 = (unsigned)v[1];
			t.d = (unsigned)v[2];
		} else if (*q == '{') {
			q++;
			if (dtmf_digit(*q))
				t.dtmf = (char)upper(*q++);
			if (*q == ',') {
				q++;
				if (is_digit(*q)) {
					unsigned long d = number(q, &e);
					q = e;
					if (d > 6000)
						return -1;
					t.d = (unsigned)d;
				}
			}
			if (*q++ != '}')
				return -1;
		} else if (dtmf_digit(*q)) {
			t.dtmf = (char)upper(*q++);
		} else {
			return -1;
		}
		/* a pause of 50 ms between two digits, so that the same
		   digit twice is two digits to the far end's receiver */
		if (t.dtmf && !tones.empty() && tones.back().dtmf)
			tones.push_back({ 0, 0, 0, 5 });
		tones.push_back(t);
		if (!*q)
			break;
		if (*q++ != ',')
			return -1;
	}
	if (!voice_)
		return -1;
	vts_ = std::move(tones);
	vts_next_ = 0;
	return voice_tone_next();
}

/*
 *    the kept parameters
 */

/* a kept parameter: <name>? its values, <name>=? its ranges,
   <name>=<values> new ones (each in its range group, those left out as
   they are; nothing changes on an error) */
int Modem::at_kept(const char *p, int *len)
{
	int i = kept_command(p);
	const Kept &k = kept_params[i];
	const char *q = p + strlen(k.name), *e;
	unsigned long v[KEPT_VALUES];
	unsigned n = kept_n_[i];
	char line[128];

	if (*q == '?') {
		/* (-SEC, Rockwell's, without its name) */
		int c = 0;
		line[0] = '\0';
		if (strcmp(k.name, "-SEC"))
			c = snprintf(line, sizeof(line), "%s: ", k.name);
		for (unsigned j = 0; j < n; j++)
			c += snprintf(line + c, sizeof(line) - c, "%s%lu",
				      j ? "," : "", kept_[i][j]);
		puts(line);
		crlf();
		*len = (int)(q + 1 - p);
		return 0;
	}
	q++;
	if (*q == '?') {
		snprintf(line, sizeof(line), "%s: %s", k.name, k.ranges);
		puts(line);
		crlf();
		*len = (int)(q + 1 - p);
		return 0;
	}
	if (*q && *q != ',' && *q != ';' && !is_digit(*q))
		return -1;
	memcpy(v, kept_[i], sizeof(v));
	for (unsigned pos = 0;; pos++) {
		if (is_digit(*q)) {
			const char *g = pos < KEPT_VALUES ?
				range_group(k.ranges, pos) : nullptr;
			unsigned long x = decimal(q, &e);
			if (!g || !in_list(g, x))
				return -1;
			v[pos] = x;
			if (n < pos + 1)
				n = pos + 1;
			q = e;
		}
		if (*q != ',')
			break;
		q++;
	}
	memcpy(kept_[i], v, sizeof(v));
	kept_n_[i] = n;
	if (!strcmp(k.name, "+ES"))
		ec_mode_from_es();
	*len = (int)(q - p);
	return 0;
}

/* every kept parameter's values as at the start */
void Modem::kept_init()
{
	for (int i = 0; i < KEPT_COUNT; i++) {
		memcpy(kept_[i], kept_params[i].values,
		       sizeof(kept_params[i].values));
		kept_n_[i] = kept_params[i].n;
	}
}

/* the values of the kept parameter called name (as kept_params spells
   it); NULL for one there is not */
const unsigned long *Modem::kept(const char *name) const
{
	int i = kept_index(name);
	return i < 0 ? nullptr : kept_[i];
}

/* \N<n>: S103, and +ES as V.250 has the same error control */
void Modem::set_ec_mode(int n)
{
	int i = kept_index("+ES");
	sregs_[SREG_EC] = (unsigned char)n;
	memcpy(kept_[i], ec_modes[n], sizeof(ec_modes[n]));
	kept_n_[i] = 3;
}

/* +ES's values as S103's \N, if one has them (else S103 stays) */
void Modem::ec_mode_from_es()
{
	const unsigned long *es = kept_[kept_index("+ES")];
	for (int n = 0; n < 6; n++)
		if (!memcmp(es, ec_modes[n], sizeof(ec_modes[n]))) {
			sregs_[SREG_EC] = (unsigned char)n;
			return;
		}
}

} // namespace dcore
