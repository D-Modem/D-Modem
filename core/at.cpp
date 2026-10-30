/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <strings.h>
#include <dsplibs.h>

#include "dm_types.h"
#include "dcore.h"
#include "str.h"
#include "reinit.h"

namespace dcore {

namespace {

/* the longest command line: at_line_ holds it and its end */
const u16 LINE_CHARS = 63;
/* &V's lines at most */
const s16 PROFILE_COLUMNS = 72;

/* c is a decimal digit */
bool is_digit(char c)
{
	return isdigit(static_cast<unsigned char>(c)) != 0;
}

/* c in upper case */
s16 upper(char c)
{
	return toupper(static_cast<unsigned char>(c));
}

/* c a DTMF digit of V.253: 0-9, *, #, A-D (a-d) */
bool dtmf_digit(char c)
{
	return c && strchr("0123456789*#ABCDabcd", c) != nullptr;
}

/* the number at p as C's strtoul() with base 0 reads it (0x hex, 0
   octal, else decimal), *end after it (p if there are no digits: 0) */
u32 number(const char *p, const char **end)
{
	const char *q = p;
	bool neg = false;
	s16 base = 10;
	u32 v;

	while (isspace(static_cast<unsigned char>(*q)))
		q++;
	if (*q == '+' || *q == '-')
		neg = *q++ == '-';
	if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X') &&
	    isxdigit(static_cast<unsigned char>(q[2]))) {
		base = 16;
		q += 2;
	} else if (q[0] == '0') {
		base = 8;
	}
	v = to_ulong(q, q + strlen(q), end, base);
	if (*end == q) {
		*end = p;
		return 0;
	}
	return neg ? 0 - v : v;
}

/* the decimal digits at p, *end after them (p and 0 if there are
   none) */
u32 decimal(const char *p, const char **end)
{
	if (!is_digit(*p)) {
		*end = p;
		return 0;
	}
	return to_ulong(p, p + strlen(p), end);
}

/* v is an item of list: single values and ranges a-b, separated by
   commas, up to the list's end or a ')' */
bool in_list(const char *list, u32 v)
{
	while (is_digit(*list)) {
		const char *e;
		u32 lo = decimal(list, &e), hi = lo;
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
const char *range_group(const char *ranges, u16 pos)
{
	const char *p = ranges;
	for (u16 i = 0;; i++) {
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
bool taken(const char *p, s16 *len)
{
	for (const auto &t : taken_cmds) {
		std::string_view name = t.name;
		size_t n = name.size();
		const char *e;
		u32 v;
		if (!starts_nocase(p, name))
			continue;
		if (!is_digit(p[n]) && name == "B")
			continue;
		v = decimal(p + n, &e);
		if (!in_list(t.values, v))
			continue;
		*len = static_cast<s16>(e - p);
		return true;
	}
	return false;
}

/* extended parameters kept with no effect but for +ES, +ESA, +ITF,
   +IFC (in V.80's Synchronous Access Mode) and +FAA (which modem.cpp
   reads): the name, the values at the start, the range groups
   (one a value) */
const struct Kept {
	const char *name;
	u16 n;
	u32 values[Modem::KEPT_VALUES];
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
	/* +ES=6 (8 answering): V.80 Synchronous Access Mode, modem.cpp */
	{ "+ES", 3, { 3, 0, 2 }, "(0-4,6),(0-4),(0-6,8)" },
	/* V.80's Synchronous Access Mode (Table 8/V.80): <trans_idle>,
	   <framed_idle>, <framed_un_ov>, <hd_auto>, <crc_type>, <nrzi_en>,
	   <syn1>, <syn2>; and its transmit buffer (8.4): <off>, <on>
	   (<off> above <on>), <report_period> */
	{ "+ESA", 8, { 0, 0, 0, 0, 0, 0, 255, 255 },
	  "(0-2),(0,1),(0,1),(0),(0-2),(0),(0-255),(0-255)" },
	{ "+ITF", 3, { 383, 128, 0 }, "(1-4095),(0-4094),(0-500)" },
	/* V.42bis and V.44 (6.6.1, 6.6.2/V.250): the directions and largest
	   parameters offered, by default ours (V42BIS_MAX_*, V44_MAX_*);
	   modem.cpp's link_init() reads them */
	{ "+DS", 4, { 3, 0, 2048, 34 }, "(0-3),(0,1),(512-65535),(6-250)" },
	{ "+DS44", 9, { 3, 0, 0, 4096, 4096, 255, 255, 12288, 12288 },
	  "(0-3),(0,1),(0,1),(256-65535),(256-65535),(32-255),(32-255),"
	  "(512-65535),(512-65535)" },
	/* D-Modem's own: zstd offered in XID (<on>), with V.cvm's
	   dictionary (<dict>); modem.cpp reads it (Annex F/V.cvm) */
	{ "+DSZ", 2, { 1, 1 }, "(0,1),(0,1)" },
	/* D-Modem's own: the link layer on a clear channel (an ISDN
	   B-channel), as CAPI numbers its B2 protocols: 0 X.75 SLP, 5 PPP
	   in synchronous HDLC, 7 the modem's (\N, +ES), 9 V.120
	   (asynchronous); modem.cpp reads it */
	{ "+B2", 1, { 0 }, "(0,5,7,9)" },
	/* the bearer service (27.007 6.7): <speed> a V.110 rate on a clear
	   channel (0: none, the answerer takes the caller's), <name> 0
	   asynchronous (the DTE's characters), 1 synchronous (+B2's link);
	   <ce> has no use here.  modem.cpp reads it */
	{ "+CBST", 3, { 0, 0, 0 },
	  "(0,65,66,68,70,71,75,79,80,81,83),(0,1),(0,1)" },
	{ "+PCW", 1, { 2 }, "(0-2)" },
	{ "+PIG", 1, { 1 }, "(0,1)" },
	/* +FAA=1, adaptive answer: modem.cpp reads it */
	{ "+FAA", 1, { 0 }, "(0,1)" },
	{ "+FPR", 1, { 0 }, "(0,1,2,4,8,10,18)" },
	{ "-SEC", 1, { 0 }, "(0,1),(0-30)" },
	/* V.cvm (Table 12-1/V.cvm): the modes, the signal interface's rate,
	   +CVMR */
	{ "+CVM", 3, { 63, 8000, 0 }, "(0-63),(8000,16000),(0,1)" },
};
const s16 KEPT_COUNT = static_cast<s16>(std::size(kept_params));
static_assert(KEPT_COUNT <= Modem::KEPT_PARAMS,
	      "the kept parameters do not fit the modem's storage");

/* the kept parameter whose name (any case) is at p followed by '=' or
   '?'; -1 if none is */
s16 kept_command(const char *p)
{
	for (s16 i = 0; i < KEPT_COUNT; i++) {
		std::string_view name = kept_params[i].name;
		size_t n = name.size();
		if (starts_nocase(p, name) &&
		    (p[n] == '=' || p[n] == '?'))
			return i;
	}
	return -1;
}

/* the kept parameter called name; -1 if there is none */
s16 kept_index(const char *name)
{
	for (s16 i = 0; i < KEPT_COUNT; i++)
		if (std::string_view(kept_params[i].name) == name)
			return i;
	return -1;
}

/* \N0 to \N5 as +ES's orig_rqst, orig_fbk, ans_fbk */
const u32 ec_modes[6][3] = {
	{ 1, 0, 1 }, { 0, 0, 0 }, { 3, 2, 4 }, { 3, 0, 2 }, { 3, 3, 5 },
	{ 4, 4, 6 },
};

/*
 *    +MS
 */

/* +MS's carrier names (V.250 Table 13, USRobotics' Bell ones, K56flex) */
const struct {
	const char *name;
	u16 id;
} ms_names[] = {
	{ "V21", DP_V21 }, { "V22", DP_V22 }, { "V22B", DP_V22BIS },
	{ "V23C", DP_V23 }, { "V32", DP_V32 }, { "V32B", DP_V32BIS },
	{ "V34", DP_V34 }, { "V90", DP_V90 }, { "V91", DP_V91 },
	{ "V92", DP_V92 }, { "B103", DP_B103 }, { "BELL103", DP_B103 },
	{ "B212", DP_B212 }, { "BELL212A", DP_B212 }, { "K56", DP_K56 },
	{ "V_CVM", DP_VCVM }, { "VWB", DP_VWB }, { "V27TC", DP_V27TER },
	{ "V29", DP_V29 }, { "V33", DP_V33 }, { "CLEAR", DP_CLEAR },
};

/* pump id is registered */
bool registered(u32 id)
{
	for (const DpDriver *d = dp_drivers(); d->id; d++)
		if (static_cast<u32>(d->id) == id)
			return d->op != nullptr;
	return false;
}

/* +MS may choose pump id: it has a code and is registered (K56flex
   while V.90 is, which runs for it) */
bool ms_choosable(u32 id)
{
	for (const DpDriver *d = dp_drivers(); d->id; d++)
		if (static_cast<u32>(d->id) == id)
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
   fields of struct voice_info, none for +VNH, +VLS and +VEM), their
   ranges (a bound 0: none) */
const struct VoiceSetting {
	const char *name;
	s16 n;
	unsigned voice_info::*field[2];
	u16 lo[2], hi[2];
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
	{ "LS", 1, { nullptr, nullptr }, { 0, 0 }, { 15, 0 } },
	{ "EM", 1, { nullptr, nullptr }, { 0, 0 }, { 1, 0 } },
	{ "TD", 1, { &voice_info::tone_default, nullptr }, { 0, 0 },
	  { 6000, 0 } },
	{ "RA", 1, { &voice_info::ring_after, nullptr }, { 0, 0 },
	  { 255, 0 } },
	{ "RN", 1, { &voice_info::ring_never, nullptr }, { 0, 0 },
	  { 255, 0 } },
};

/* +VLS=?: the devices of +VLS (V.253 10.2.4.2): label, primitives, and
   the events reported in the transmit, receive and command states, a
   bit each (event n of V.253's Table 1 the nth from the left of 32).
   On hook (0): caller ID (0, the <DLE>X packet) and RING (3).  On the
   line (1), in every state: DTMF (4), the calling tones of a fax
   machine (6, <DLE>c) and of a data modem (7, <DLE>e), loop current
   interruption (13, <DLE>l), ringing tone (18, <DLE>r), busy (19,
   <DLE>b), and the answer tones of 2100 Hz (25, <DLE>a) and of 2225 Hz
   (26, <DLE>f).  A line that senses the local phone (Host's
   senses_local_phone()) adds its going off or on hook (8, <DLE>H and
   <DLE>h) to both in every state (LOCAL_PHONE_EVENT).  A Host with a
   handset (has_handset()) adds it alone (14, "H", on hook: its DTMF
   digits, 4) and on the call (15, "HT": the line's events). */
static const struct {
	const char *label;
	u32 events[3];
	bool handset;
} voice_devices[] = {
	{ "0,\"\"", { 0x00000000, 0x00000000, 0x90000000 }, false },
	{ "1,\"T\"", { 0x0B043060, 0x0B043060, 0x0B043060 }, false },
	{ "14,\"H\"", { 0x08000000, 0x08000000, 0x08000000 }, true },
	{ "15,\"HT\"", { 0x0B043060, 0x0B043060, 0x0B043060 }, true },
};
const u32 LOCAL_PHONE_EVENT = 0x00800000;

/* v within the bounds lo and hi (0: none) */
bool in_bounds(u32 v, u16 lo, u16 hi)
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
		u16 typed = at_count_, k = 0;
		s16 ret;

		for (u16 i = 0; i < typed && at_line_[i]; i++)
			if (at_line_[i] != ' ' && at_line_[i] != '\t')
				cmd[k++] = at_line_[i];
		cmd[k] = '\0';
		at_count_ = 0;
		if (k < 2 || upper(cmd[0]) != 'A' || upper(cmd[1]) != 'T') {
			if (echo) {
				const char *cr = reinterpret_cast<const char *>(
					&sregs_[SREG_CR_CHAR]);
				put_chars(cr, 1);
				for (u16 i = 0; i < typed; i++)
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
		char bs = static_cast<char>(sregs_[SREG_BS_CHAR]);

		/* A/: the last line again, with no line end */
		if (c == '/' && at_count_ == 1 && upper(at_line_[0]) == 'A') {
			size_t n = strlen(at_cmd_);
			if (n > 1) {
				memcpy(at_line_ + 1, at_cmd_ + 1, n - 1);
				at_count_ = n;
				if (echo)
					put_chars(at_cmd_ + 1,
						  static_cast<s32>(n) - 1);
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
		if (c == static_cast<char>(sregs_[SREG_CR_CHAR]) ||
		    c == static_cast<char>(sregs_[SREG_LF_CHAR])) {
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
s16 Modem::at_command(char *cmd)
{
	char *p;

	if (upper(cmd[0]) != 'A' || upper(cmd[1]) != 'T')
		return -1;
	/* (a leased line may answer by itself again: S0, AT&L1) */
	leased_auto_ = true;
	for (p = cmd + 2; *p;) {
		s16 len = 1, ret = 0;
		/* a one-digit setting in S-register reg, skip characters
		   in */
		auto setting = [this, &p, &len](s16 reg, s16 skip, s16 lo,
						s16 hi) {
			s16 k, r = at_set_sreg(reg, p[skip], lo, hi, &k);
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
				if (starts_nocase(p, "#UD")) {
					/* Unimodem's diagnostics */
					unimodem_diag();
					len = 3;
					break;
				}
				ret = !starts_nocase(p, "#CID") ? -1 :
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
				case 'L':
					/* a dialled line (0), a 2-wire leased
					   one (1) or a 4-wire one (2) */
					ret = setting(SREG_LEASED, 2, 0, 2);
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
					s16 d = is_digit(p[2]) ? p[2] - '0' : 0;
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
				case 'T':
					ret = at_ampT(p, &len);
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
					ret = !starts_nocase(p, "+FCLASS") ?
						at_fax(p, &len) :
						at_FCLASS(p, &len);
					break;
				case 'G':
					if (starts_nocase(p, "+GCI")) {
						ret = at_GCI(p, &len);
					} else if (starts_nocase(p, "+GCAP")) {
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
s16 Modem::at_set_sreg(s16 num, char c, s16 lo, s16 hi, s16 *len)
{
	s16 v = 0;
	*len = 0;
	if (is_digit(c)) {
		v = c - '0';
		*len = 1;
	}
	if (v < lo || v > hi)
		return -1;
	sregs_[num] = static_cast<unsigned char>(v);
	return 0;
}

/*
 *    basic commands
 */

/* ATA: answers (the held call too); its result later, or ERROR */
s16 Modem::at_A(char *p, s16 *len)
{
	*len = static_cast<s16>(strlen(p));
	return answer() ? -1 : 1;
}

/* ATB alone: V.22 at 1200 bit/s, no automode (+MS=22,0,1200,1200) */
s16 Modem::at_B(char *, s16 *len)
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
s16 Modem::at_D(char *p, s16 *len)
{
	char *s = p + 1, *semi = strchr(s, ';');
	char number[LINE_CHARS + 1];
	size_t n;

	*len = static_cast<s16>(strlen(p));
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
		if (mode_ == MODEM_MODE_VOICE)
			return voice_dial(nullptr);
		return dial(nullptr) ? -1 : 1;
	}
	if (upper(*s) == 'T')
		sregs_[SREG_TONE_OR_PULSE] = 1;
	else if (upper(*s) == 'P')
		sregs_[SREG_TONE_OR_PULSE] = 0;
	n = semi ? static_cast<size_t>(semi + 1 - s) : strlen(s);
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
	if (mode_ == MODEM_MODE_VOICE)
		return voice_dial(number);
	return dial(number) ? -1 : 1;
}

/* ATD in voice mode (V.253 9.1.1): the line taken as the caller (an
   implied +VLS=1), the number dialled in DTMF where the modem dials it
   (Host::analog_line(), after 2 s for the dial tone), OK when the call
   is answered, BUSY, NO ANSWER (S7) or ERROR when it fails.  With the line already taken, the
   number's digits are dialled on it as DTMF (S11 ms each, S11 ms
   apart), as on a line off hook, and OK follows them.  number NULL: the
   last one (ATDL).  1 (the result to come), 0 (OK now), or -1 */
s16 Modem::voice_dial(const char *number)
{
	if (voice_) {
		std::vector<VoiceTone> tones =
			dtmf_tones(number ? number : dial_string_);
		if (tones.empty())
			return 0;
		vts_ = std::move(tones);
		vts_next_ = 0;
		return voice_tone_next();
	}
	if (number)
		keep_dial_string(number);
	else if (!dial_string_[0])
		return -1;
	caller_ = true;
	logf("voice dial: %s\n", dial_string_);
	if (voice_start()) {
		caller_ = false;
		return -1;
	}
	voice_dialing_ = true;
	voice_dial_since_ = host_.now_ms();
	if (host_.analog_line(*this)) {
		/* a telephone line's: 2 s for the dial tone, then the
		   number in DTMF (the engine's first OK, the line taken,
		   starts them; voice_process()) */
		vts_ = dtmf_tones(dial_string_);
		vts_.insert(vts_.begin(), { 0, 0, 0, 200, false });
		vts_next_ = 0;
		voice_dial_digits_ = true;
	}
	command_ = false;
	return 1;
}

/* the DTMF digits of number as +VTS elements: S11 ms each, S11 ms
   apart */
std::vector<Modem::VoiceTone> Modem::dtmf_tones(const char *number)
{
	std::vector<VoiceTone> tones;
	u16 d = sregs_[SREG_DTMF_DURATION] / 10;

	for (const char *p = number; *p; p++) {
		if (!dtmf_digit(*p))
			continue;
		if (!tones.empty())
			tones.push_back({ 0, 0, 0, d, false });
		tones.push_back(
			{ static_cast<char>(upper(*p)), 0, 0, d, false });
	}
	return tones;
}

/* ATH, ATH0 on hook (a call hangs up), ATH1 off hook; ERROR when the
   hook does not change, or for another digit */
s16 Modem::at_H(char *p, s16 *len)
{
	s16 state = MODEM_HOOK_ON;
	*len = 1;
	if (is_digit(p[1])) {
		if (p[1] == '1')
			state = MODEM_HOOK_OFF;
		else if (p[1] != '0' || is_digit(p[2]))
			return -1;
		*len = 2;
	}
	/* in voice mode, ATH is +VLS=0 and back to data mode (V.253
	   9.1.2) */
	if (mode_ == MODEM_MODE_VOICE && state == MODEM_HOOK_ON) {
		if (voice_)
			voice_stop();
		set_mode(MODEM_MODE_DATA);
		return 0;
	}
	return hook(state) ? -1 : 0;
}

/* ATI<n>: what the modem is, its S-registers, its country, the last
   call's statistics (ATI6 and ATI11) */
s16 Modem::at_I(char *p, s16 *len)
{
	const char *e;
	u32 n = number(p + 1, &e);

	*len = static_cast<s16>(e - p);
	switch (n) {
	case 0:
		puts(product);
		crlf();
		break;
	case 1:
	case 2:
		puts(std::string(product) + ", " + version);
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
		for (s16 i = 0; i < 24; i++) {
			puts("s" + dec(i, 2) + "=" + dec(sregs_[i], 3) + " ");
			if (i % 8 == 7)
				crlf();
		}
		crlf();
		break;
	case 5:
		puts("Stored Profile 0:");
		crlf();
		break;
	case 6:
	case 11:
		/* (ATI6: USRobotics' link diagnostics) */
		call_stats();
		break;
	case 7:
		puts("Country: ");
		puts(country_name().c_str());
		crlf();
		break;
	default:
		return -1;
	}
	return 0;
}

/* ATO: back online (or the held call's handshake); its result later, or
   ERROR */
s16 Modem::at_O(char *p, s16 *len)
{
	/* O, O0; O1 with a retrain (V.250), O3 with a rate renegotiation
	   (Rockwell's) */
	const char *e;
	u32 n = number(p + 1, &e);
	*len = static_cast<s16>(strlen(p));
	if (e != p + 1 && n != 0 && n != 1 && n != 3)
		return -1;
	if (online())
		return -1;
	if (e != p + 1 && n)
		link_req(n == 1 ? 1 : 2);
	return 1;
}

/* AT&T<n>: V.54's loop tests (Hayes' numbers): 0 (or none) the test
   ends (AT&T7's and AT&T8's bit errors, then OK); 1 loop 3, 8 with the
   self-test (offline, AT&L with +MS=V29, V33 or V27TC); 3 loop 2 here;
   4 the far end may ask for loop 2, 5 it may not; 6 loop 2 at the far
   end, 7 with the self-test (online command state, no error
   correction) */
s16 Modem::at_ampT(char *p, s16 *len)
{
	const char *e;
	u32 n = number(p + 2, &e);
	bool online = state_ == COMMAND_ONLINE;

	*len = static_cast<s16>(e - p);
	switch (e == p + 2 ? 0 : n) {
	case 0:
		return test_end(false);
	case 1:
	case 8:
		return loop3_start(n == 8);
	case 3:
		if (!online || !link_plain() || test_.active())
			return -1;
		logf("loop 2 here\n");
		test_.local2();
		return 0;
	case 4:
	case 5:
		test_.set_grant(n == 4);
		return 0;
	case 6:
	case 7:
		if (!online || !link_plain() || test_.active())
			return -1;
		logf("loop 2 at the far end%s\n",
		     n == 7 ? " with the self-test" : "");
		test_.remote(n == 7);
		return 1;
	default:
		return -1;
	}
}

/* ATS<n>? reads S-register n, ATS<n>=<v> sets it (S103 as \N); n and v
   in decimal, a leading 0 no octal (ATS08=1, ATS0=010) */
s16 Modem::at_S(char *p, s16 *len)
{
	const char *e;
	u32 n = decimal(p + 1, &e), v;

	if (n >= sizeof(sregs_))
		return -1;
	if (*e == '?') {
		if (n == SREG_VCVM_TONES || n == SREG_VCVM_RENEGS)
			vcvm_refresh();
		puts(dec(sregs_[n]));
		crlf();
		*len = static_cast<s16>(e + 1 - p);
		return 0;
	}
	if (*e != '=')
		return -1;
	v = decimal(e + 1, &e);
	/* S44 and S45 (V.cvm's) are read only */
	if (n == SREG_VCVM_TONES || n == SREG_VCVM_RENEGS)
		return -1;
	set_sreg(static_cast<u32>(n), static_cast<s32>(v));
	if (n == SREG_EC && v <= 5)
		set_ec_mode(static_cast<s16>(v));
	*len = static_cast<s16>(e - p);
	return 0;
}

/* ATZ: the modem as at the start (but the country), the rest of the
   line not read */
s16 Modem::at_Z(char *p, s16 *len)
{
	*len = static_cast<s16>(strlen(p));
	return reset();
}

/* AT&V, AT&V0: the active settings, in lines of PROFILE_COLUMNS at
   most; AT&V1: the last call's statistics, as ATI11 */
s16 Modem::at_ampV(char *p, s16 *len)
{
	static const unsigned char regs[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24,
		28, 29, 30, 31, 32, 39, 56, 60, 62, 70, 71, 92, 103, 104, 105,
		106, 161,
	};
	s16 col = 0;
	/* an item of the current group, on a new line if it does not fit
	   on this one */
	auto put = [this, &col](const std::string &s) {
		s16 n = static_cast<s16>(s.size());
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

	if (p[2] == '1') {
		/* AT&V1: the last call's statistics (Rockwell's) */
		*len = 3;
		call_stats();
		return 0;
	}
	if (is_digit(p[2]) && p[2] != '0')
		return -1;
	*len = 2 + is_digit(p[2]);
	puts("ACTIVE PROFILE:");
	crlf();
	put("E" + dec(sregs_[SREG_ECHO]));
	put("L" + dec(sregs_[SREG_SPEAKER_VOLUME]));
	put("M" + dec(sregs_[SREG_SPEAKER_CONTROL]));
	put("N" + dec(sregs_[SREG_AUTOMODE]));
	put("Q" + dec(sregs_[SREG_QUIET]));
	put(sregs_[SREG_TONE_OR_PULSE] ? "T" : "P");
	put("V" + dec(sregs_[SREG_VERBOSE]));
	put("X" + dec(sregs_[SREG_X_CODE]));
	put("Y" + dec(sregs_[SREG_DEFAULT_SETTING]));
	put("&A" + dec(sregs_[SREG_CONNNECT_MSG_FORMAT]));
	put("&C" + dec(sregs_[SREG_CD]));
	put("&E" + dec(sregs_[SREG_CONNNECT_MSG_SPEED_SRC]));
	put("&H" + dec(sregs_[SREG_FLOW_CONTROL]));
	put("&P" + dec(sregs_[SREG_PULSE_RATIO]));
	put("\\A" + dec(sregs_[SREG_MNP_BLOCK]));
	put("\\N" + dec(sregs_[SREG_EC]));
	put("\\V" + dec(sregs_[SREG_CONNECT_PROTOCOL]));
	put("%C" + dec(sregs_[SREG_COMP]));
	put("%E" + dec(sregs_[SREG_LINE_QUALITY_CONTROL]));
	end();
	for (unsigned char r : regs) {
		put("S" + dec(r, 2) + ":" + dec(sregs_[r], 3));
	}
	end();
	put("+MS=" + dec(sregs_[SREG_DP]) + "," +
	    dec(sregs_[SREG_AUTOMODE]) + "," + dec(min_rate_) + "," +
	    dec(max_rate_));
	put("+FCLASS=" + dec(static_cast<s16>(mode_)));
	put("+GCI=" + hex(homolog_->id));
	put("+PMH=" + dec(moh_off_));
	put("+PMHT=" + dec(moh_timer_));
	put("#CID=" + dec(cid_requested_));
	for (s16 i = 0; i < KEPT_COUNT; i++) {
		std::string item = std::string(kept_params[i].name) + "=";
		for (u16 j = 0; j < kept_n_[i]; j++)
			item += (j ? "," : "") + dec(kept_[i][j]);
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
s16 Modem::at_MS(char *p, s16 *len)
{
	const char *q = p + 3, *e;
	u32 mod = sregs_[SREG_DP], automode = sregs_[SREG_AUTOMODE];
	u32 min = min_rate_, max = max_rate_, v;

	if (upper(p[2]) != 'S')
		return -1;
	if (*q == '?') {
		puts(dec(sregs_[SREG_DP]) + "," + dec(sregs_[SREG_AUTOMODE]) +
		     "," + dec(min_rate_) + "," + dec(max_rate_));
		crlf();
		*len = static_cast<s16>(q + 1 - p);
		return 0;
	}
	if (*q++ != '=')
		return -1;
	if (*q == '?') {
		std::string line = "(";
		for (const DpDriver *d = dp_drivers(); d->id; d++)
			if (d->code && ms_choosable(d->id))
				line += (line.size() > 1 ? "," : "") +
					dec(static_cast<s16>(d->id));
		const std::string rates = "(" + dec(MODEM_MIN_RATE) + "-" +
			dec(MODEM_MAX_RATE) + ")";
		puts(line + "),(0,1)," + rates + "," + rates);
		crlf();
		*len = static_cast<s16>(q + 1 - p);
		return 0;
	}
	if (isalpha(static_cast<unsigned char>(*q))) {
		size_t n = 0;
		bool found = false;
		while (isalnum(static_cast<unsigned char>(q[n])) || q[n] == '_')
			n++;
		for (const auto &m : ms_names)
			if (same_nocase(std::string_view(q, n), m.name)) {
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
	sregs_[SREG_DP] = static_cast<unsigned char>(mod);
	sregs_[SREG_AUTOMODE] = static_cast<unsigned char>(automode);
	min_rate_ = min;
	max_rate_ = max;
	max_rx_rate_ = 0;
	*len = static_cast<s16>(q - p);
	return 0;
}

/* +GCI?, +GCI=?, +GCI=<hex>: the country by its T.35 code */
s16 Modem::at_GCI(char *p, s16 *len)
{
	const char *q = p + 4;

	if (*q == '?') {
		puts("+GCI:" + hex(homolog_->id));
		crlf();
		*len = 5;
		return 0;
	}
	if (*q++ != '=')
		return -1;
	if (*q == '?') {
		puts("+GCI:(");
		for (const struct homolog_set *h = homolog_set; h->name; h++) {
			puts(hex(h->id) + ",");
		}
		puts(")");
		crlf();
		*len = 6;
		return 0;
	}
	{
		const char *e;
		u32 code = to_ulong(q, q + strlen(q), &e, 16);
		if (e == q || code > 0xffff ||
		    homolog_init(static_cast<s32>(code), nullptr))
			return -1;
		*len = static_cast<s16>(e - p);
	}
	return 0;
}

/* +FCLASS=?, +FCLASS?, +FCLASS=<d>: data (0), class 1 fax (1), voice
   (8) */
s16 Modem::at_FCLASS(char *p, s16 *len)
{
	const char *q = p + 7;

	if (q[0] == '=' && q[1] == '?') {
		puts("0,1,8");
		crlf();
		*len = 9;
		return 0;
	}
	if (q[0] == '?') {
		puts(dec(static_cast<s16>(mode_)));
		crlf();
		*len = 8;
		return 0;
	}
	if (q[0] == '=' && (q[1] == '0' || q[1] == '1' || q[1] == '8')) {
		*len = 9;
		return set_mode(static_cast<enum MODEM_MODE>(q[1] - '0'));
	}
	return -1;
}

/* +FMI, +FMM, +FMR (T.31 Annex A); the class 1 commands +FTS, +FRS
   (silence), +FTM, +FRM (data), +FTH, +FRH (HDLC frames), for the fax
   engine (T.31 8.3) */
s16 Modem::at_fax(char *p, s16 *len)
{
	const char *q = p + 2, *e;
	char what = static_cast<char>(upper(q[0])),
	     kind = static_cast<char>(upper(q[1]));
	enum FAX_CMD cmd;
	u32 v;

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
			*len = static_cast<s16>(q + 1 - p);
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
			*len = static_cast<s16>(q + 1 - p);
			return 0;
		}
		if (!is_digit(*q))
			return -1;
		cmd = what == 'T' ? FAX_CMD_FTH : FAX_CMD_FRH;
		v = *q - '0';
		e = q + 1;
		break;
	}
	*len = static_cast<s16>(e - p);
	return fax_command(cmd, static_cast<u32>(v));
}

/* a class 1 command for the fax engine: once it has started, the DTE's
   characters are the engine's until it reports (1); ERROR without an
   engine or when it refuses */
s16 Modem::fax_command(enum FAX_CMD cmd, u32 val)
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
s16 Modem::at_PMH(char *p, s16 *len)
{
	const char *q = p + 4, *e;
	bool timer;
	u32 v;

	if (!starts_nocase(p, "+PMH"))
		return -1;
	if (upper(*q) == 'R') {
		u32 secs = 10;
		q++;
		if (state_ != COMMAND_ONLINE || !dp_ || dp_->id != DP_V92)
			return -1;
		if (q[0] == '=' && is_digit(q[1])) {
			secs = decimal(q + 1, &e);
			if (secs < 1 || secs > 960)
				return -1;
			q = e;
		}
		moh_req_ = static_cast<s32>(secs);
		*len = static_cast<s16>(q - p);
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
		puts(std::string(timer ? "+PMHT: " : "+PMH: ") +
		     dec(timer ? moh_timer_ : moh_off_));
		crlf();
		q++;
	} else if (q[0] == '=' && is_digit(q[1])) {
		v = decimal(q + 1, &e);
		if (v > (timer ? 13u : 1u))
			return -1;
		if (timer)
			moh_timer_ = static_cast<s16>(v);
		else
			moh_off_ = static_cast<s16>(v);
		q = e;
	} else {
		return -1;
	}
	*len = static_cast<s16>(q - p);
	return 0;
}

/* #CID, +VCID (V.253 9.2.3): caller ID, 0 off, 1 formatted, 2 the
   message as it is */
s16 Modem::at_CID(char *p, s16 *len)
{
	const char *q = p + (*p == '#' ? 4 : 5);

	if (q[0] == '=' && q[1] == '?') {
		puts("0,1,2");
		crlf();
		q += 2;
	} else if (q[0] == '?') {
		puts(dec(cid_requested_));
		crlf();
		q++;
	} else if (q[0] == '=' && q[1] >= '0' && q[1] <= '2') {
		cid_requested_ = q[1] - '0';
		q += 2;
	} else {
		return -1;
	}
	*len = static_cast<s16>(q - p);
	return 0;
}

/* +V...: caller ID in any mode, the rest in voice mode only: the
   parameters (? =? =<values>), +VIP, +VLS, and what the voice engine
   does: +VTX, +VRX, +VTR, +VTS */
s16 Modem::at_voice(char *p, s16 *len)
{
	const char *q = p + 4, *e;
	char cmd[3];

	if (starts_nocase(p + 2, "CID"))
		return at_CID(p, len);
	if (mode_ != MODEM_MODE_VOICE || !p[2] || !p[3])
		return -1;
	cmd[0] = static_cast<char>(upper(p[2]));
	cmd[1] = static_cast<char>(upper(p[3]));
	cmd[2] = '\0';
	for (const auto &s : voice_settings) {
		std::string_view name = s.name;
		bool device = name == "LS";
		bool events = name == "EM";
		u32 v[2] = { 0, 0 };
		s16 ret = 0;

		if (name != cmd)
			continue;
		for (s16 i = 0; i < s.n; i++)
			v[i] = s.field[i] ? voice_info_.*s.field[i] :
				events ? voice_events_ :
				device && voice_ ? voice_device_ : 0;
		if (device && q[0] == '=' && q[1] == '?') {
			u32 more = host_.senses_local_phone(*this) ?
				LOCAL_PHONE_EVENT : 0;
			bool handset = host_.has_handset(*this);
			for (const auto &d : voice_devices) {
				if (d.handset && !handset)
					continue;
				puts(std::string(d.label) + "," +
				     hex(d.events[0] | more, 8, true) + "," +
				     hex(d.events[1] | more, 8, true) + "," +
				     hex(d.events[2] | more, 8, true));
				crlf();
			}
			q += 2;
		} else if (q[0] == '=' && q[1] == '?') {
			std::string line = "+V" + std::string(name) + "=";
			for (s16 i = 0; i < s.n; i++)
				line += (i ? "," : "") + dec(s.lo[i]) + "-" +
					dec(s.hi[i]);
			puts(line);
			crlf();
			q += 2;
		} else if (q[0] == '?') {
			std::string line = "+V" + std::string(name) + "=";
			for (s16 i = 0; i < s.n; i++)
				line += (i ? "," : "") + dec(v[i]);
			puts(line);
			crlf();
			q++;
		} else if (q[0] == '=') {
			q++;
			for (s16 i = 0; i < s.n; i++) {
				if (i && *q++ != ',')
					return -1;
				v[i] = number(q, &e);
				q = e;
				if (!in_bounds(v[i], s.lo[i], s.hi[i]))
					return -1;
			}
			for (s16 i = 0; i < s.n; i++)
				if (s.field[i])
					voice_info_.*s.field[i] =
						static_cast<unsigned>(v[i]);
			if (events)
				voice_events_ = static_cast<u16>(v[0]);
		} else {
			return -1;
		}
		*len = static_cast<s16>(q - p);
		if (device)
			ret = voice_set_device(static_cast<u16>(v[0]));
		return ret;
	}
	const std::string_view c = cmd;
	if (c == "IP") {
		voice_init();
		*len = 4;
		return 0;
	}
	*len = static_cast<s16>(strlen(p));
	if (c == "TX")
		return voice_command(VOICE_CMD_STATE_TX);
	if (c == "RX")
		return voice_command(VOICE_CMD_STATE_RX);
	if (c == "TR")
		return voice_command(VOICE_CMD_STATE_DUPLEX);
	if (c != "TS")
		return -1;
	/* +VTS: the tone settings, or a tone or a DTMF digit played */
	if (q[0] == '?' || (q[0] == '=' && q[1] == '?')) {
		if (q[0] == '?')
			puts("+VTS=" + dec(voice_info_.tone1_freq) + "," +
			     dec(voice_info_.tone2_freq) + "," +
			     dec(voice_info_.tone_duration));
		else
			puts("+VTS=0-4000,0-4000,0-6000");
		crlf();
		*len = q[0] == '?' ? 5 : 6;
		return 0;
	}
	if (*q++ != '=')
		return -1;
	/* V.253's string: elements separated by commas, each a DTMF digit
	   (0-9, *, #, A-D) or ! (a hook flash), {s,d} the digit or flash s
	   for d * 10 ms, or [f1,f2,d] a tone of f1 and f2 Hz (0 to 4000, 0:
	   none) for d * 10 ms (0 to 6000).  A field left empty is 0 for a
	   frequency, no digit (silence) for s, and +VTD for d, so that a
	   bare digit, !, {s}, [f1] and [] last +VTD; all of the string
	   checked before any of it plays, a flash on a line that can have
	   none an ERROR */
	std::vector<VoiceTone> tones;
	const u16 dflt = voice_info_.tone_default ?
		voice_info_.tone_default : 100;
	for (;;) {
		VoiceTone t = { 0, 0, 0, dflt, false };
		if (*q == '[') {
			u32 v[3] = { 0, 0, dflt };
			q++;
			for (s16 i = 0; i < 3 && *q != ']'; i++) {
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
			t.f1 = static_cast<u16>(v[0]);
			t.f2 = static_cast<u16>(v[1]);
			t.d = static_cast<u16>(v[2]);
		} else if (*q == '{') {
			q++;
			if (dtmf_digit(*q)) {
				t.dtmf = static_cast<char>(upper(*q++));
			} else if (*q == '!') {
				q++;
				t.flash = true;
			}
			if (*q == ',') {
				q++;
				if (is_digit(*q)) {
					u32 d = number(q, &e);
					q = e;
					if (d > 6000)
						return -1;
					t.d = static_cast<u16>(d);
				}
			}
			if (*q++ != '}')
				return -1;
		} else if (dtmf_digit(*q)) {
			t.dtmf = static_cast<char>(upper(*q++));
		} else if (*q == '!') {
			q++;
			t.flash = true;
		} else {
			return -1;
		}
		if (t.flash && !host_.can_flash(*this))
			return -1;
		/* a pause of 50 ms between two digits, so that the same
		   digit twice is two digits to the far end's receiver */
		if (t.dtmf && !tones.empty() && tones.back().dtmf)
			tones.push_back({ 0, 0, 0, 5, false });
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
s16 Modem::at_kept(const char *p, s16 *len)
{
	s16 i = kept_command(p);
	const Kept &k = kept_params[i];
	const std::string_view name = k.name;
	const char *q = p + name.size(), *e;
	u32 v[KEPT_VALUES];
	u16 n = kept_n_[i];

	if (*q == '?') {
		/* (-SEC, Rockwell's, without its name) */
		std::string line;
		if (name != "-SEC")
			line = std::string(name) + ": ";
		for (u16 j = 0; j < n; j++)
			line += (j ? "," : "") + dec(kept_[i][j]);
		puts(line);
		crlf();
		/* +CVM? while connected: the connection's figures */
		if (name == "+CVM" && vcvm_.on) {
			static const char *rates[3] = { "1/2", "3/4", "7/8" };
			puts("+CVM: " + dec(vcvm_.raw_tx) + "," +
			     dec(vcvm_.raw_rx) + "," + dec(vcvm_.coded_tx) +
			     "," + dec(vcvm_.coded_rx) + "," +
			     dec(vcvm_.sym_tenths / 10) + "." +
			     dec(vcvm_.sym_tenths % 10) + "," +
			     dec(vcvm_.tones_tx) + "," + dec(vcvm_.tones_rx) +
			     "," + rates[vcvm_.code_rate % 3] + "," +
			     dec(vcvm_.renegs) + "," + dec(vcvm_.erasures));
			crlf();
		}
		*len = static_cast<s16>(q + 1 - p);
		return 0;
	}
	q++;
	if (*q == '?') {
		puts(std::string(name) + ": " + k.ranges);
		crlf();
		*len = static_cast<s16>(q + 1 - p);
		return 0;
	}
	if (*q && *q != ',' && *q != ';' && !is_digit(*q))
		return -1;
	dsp::copy_array(v, kept_[i]);
	for (u16 pos = 0;; pos++) {
		if (is_digit(*q)) {
			const char *g = pos < KEPT_VALUES ?
				range_group(k.ranges, pos) : nullptr;
			u32 x = decimal(q, &e);
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
	/* +ITF: <off> above <on> (8.4/V.80), else ERROR */
	if (name == "+ITF" && v[0] <= v[1])
		return -1;
	dsp::copy_array(kept_[i], v);
	kept_n_[i] = n;
	if (name == "+ES")
		ec_mode_from_es();
	*len = static_cast<s16>(q - p);
	return 0;
}

/* every kept parameter's values as at the start */
void Modem::kept_init()
{
	for (s16 i = 0; i < KEPT_COUNT; i++) {
		dsp::copy_array(kept_[i], kept_params[i].values);
		kept_n_[i] = kept_params[i].n;
	}
}

/* the values of the kept parameter called name (as kept_params spells
   it); NULL for one there is not */
const u32 *Modem::kept(const char *name) const
{
	s16 i = kept_index(name);
	return i < 0 ? nullptr : kept_[i];
}

/* \N<n>: S103, and +ES as V.250 has the same error control */
void Modem::set_ec_mode(s16 n)
{
	s16 i = kept_index("+ES");
	sregs_[SREG_EC] = static_cast<unsigned char>(n);
	std::copy(std::begin(ec_modes[n]), std::end(ec_modes[n]), kept_[i]);
	kept_n_[i] = 3;
}

/* +ES's values as S103's \N, if one has them (else S103 stays) */
void Modem::ec_mode_from_es()
{
	const u32 *es = kept_[kept_index("+ES")];
	for (s16 n = 0; n < 6; n++)
		if (!memcmp(es, ec_modes[n], sizeof(ec_modes[n]))) {
			sregs_[SREG_EC] = static_cast<unsigned char>(n);
			return;
		}
}

} // namespace dcore
