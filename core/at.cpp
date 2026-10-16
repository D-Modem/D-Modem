/*
 * SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause
 * Copyright (C) 2026 Neo-Desktop
 * Parts from Smart Link's slmodemd: Copyright (c) Smart Link Ltd.,
 * under its BSD licence (LICENSES/BSD-3-Clause.txt)
 *
 *    at.cpp - the AT command interpreter of the modem core
 *
 *    D-Modem's commands (they began as slmodemd's, modem_at.c):
 *
 *      AT  A B D E H I L M N O P Q S T V X Y Z
 *      AT& A C D E F H K P Q R S
 *      AT\ A N    AT% B C E    AT* B    AT# CID
 *      AT+ MS GCI GCAP GMI GMM GMR FCLASS VCID PMH PMHT PMHR, class 1
 *          fax (FTS FRS FTM FRM FTH FRH FMI FMM FMR), voice (+V..., in
 *          voice mode)
 *
 *    and, taken with no effect, the commands other modems' setup
 *    strings send (V.250's, Rockwell's and Conexant's, USRobotics'):
 *    taken_cmds and kept_params below.  ; separates commands.
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsplibs.h>

#include "dcore.h"

namespace dcore {

/* toupper + strncmp */
static int toupper_strncmp(const char *s1, const char *s2, int n)
{
	for (int i = 0; i < n; i++)
		if (toupper(s1[i]) != s2[i])
			return s1[i] - s2[i];
	return 0;
}

/*
 *    command mode: the DTE's characters become command lines
 */

/* the DTE's characters in command mode: the echo (ATE), backspace,
   A/ (the last line again); a line that starts with AT runs, without
   its blanks, its result to the DTE; another is blanked out on the
   DTE's screen; a line too long is an ERROR */
void Modem::at_process()
{
	int echo = sregs_[SREG_ECHO];
	char lf = sregs_[SREG_LF_CHAR], cr = sregs_[SREG_CR_CHAR];
	char bs = sregs_[SREG_BS_CHAR];
	char ch;
	int ret;

	while (get_chars(&ch, 1) > 0) {
		/* A/: the last command again */
		if (ch == '/' && at_count_ == 1 &&
		    toupper(at_line_[0]) == 'A') {
			at_count_ = strlen(at_line_);
			if (echo)
				put_chars(at_line_ + 1, at_count_ - 1);
			ch = cr;
		}
		if (echo)
			put_chars(&ch, 1);
		if (ch == cr || ch == lf) {
			char *p = at_cmd_;
			at_line_[at_count_] = '\0';
			for (unsigned i = 0; i < at_count_; i++) {
				if (at_line_[i] == ' ' || at_line_[i] == '\t')
					continue;
				*p++ = at_line_[i];
			}
			*p = '\0';
			if (strlen(at_cmd_) < 2 || toupper(at_cmd_[0]) != 'A' ||
			    toupper(at_cmd_[1]) != 'T') {
				memset(at_line_, ' ', at_count_);
				if (echo) {
					put_chars(&cr, 1);
					put_chars(at_line_, at_count_);
					put_chars(&cr, 1);
				}
				at_count_ = 0;
				continue;
			}
			if (echo)
				crlf();
			logf("run cmd: %s\n", at_cmd_);
			ret = at_command(at_cmd_);
			if (ret < 0)
				report(Result::Error);
			else if (ret == 0)
				report(Result::Ok);
			at_line_[at_count_] = '\0';
			at_count_ = 0;
			echo = sregs_[SREG_ECHO];
			cr = sregs_[SREG_CR_CHAR];
			lf = sregs_[SREG_LF_CHAR];
			bs = sregs_[SREG_BS_CHAR];
			/* a fax command runs: the rest is the fax engine's */
			if (ret > 0 && !command_ && fax_)
				break;
		} else if (ch == bs && at_count_) {
			at_count_--;
			if (echo) {
				ch = ' ';
				put_chars(&ch, 1);
				put_chars(&bs, 1);
			}
		} else if (at_count_ == sizeof(at_line_) - 1) {
			if (echo)
				crlf();
			at_count_ = 0;
			at_line_[1] = '\0';
			report(Result::Error);
		} else
			at_line_[at_count_++] = ch;
	}
}

/*
 *    the commands: each gets the text after its letters and sets *len
 *    to what it took of it; returns 0 (OK), 1 (no result now) or -1
 *    (ERROR)
 */

/* a one digit setting of S-register num (ATE, ATL, ATM...), lo to hi;
   no digit means 0 */
int Modem::at_set_sreg(int num, char c, int lo, int hi, int *len)
{
	if (!isdigit((unsigned char)c)) {
		*len = 0;
		c = '0';
	} else
		*len = 1;
	if (c >= lo + '0' && c <= hi + '0')
		return set_sreg(num, c - '0');
	return -1;
}

/* ATA: answers (no result now) */
int Modem::at_A(char *, int *)
{
	return answer() ? -1 : 1;
}

/* ATB: V.22 at 1200 bit/s only, without automode */
int Modem::at_B(char *, int *)
{
	sregs_[SREG_DP] = DP_V22;
	sregs_[SREG_AUTOMODE] = 0;
	min_rate_ = 1200;
	max_rate_ = 1200;
	return 0;
}

/* ATD: dials the rest of the line (T, P: tone, pulse; L: the last
   number again) up to a ';', after which it returns to command state
   once the number is dialed and runs what follows on the line (V.250);
   no result now (ERROR for a stored number, S=<n>: there are none) */
int Modem::at_D(char *p, int *)
{
	char *semi = strchr(p, ';');
	dial_rest_[0] = '\0';
	for (const char *q = p; *q && q != semi; q++)
		if (toupper(*q) == 'S' && q[1] == '=' &&
		    isdigit((unsigned char)q[2]))
			return -1;
	switch (toupper(*p)) {
	case 'T':
		set_sreg(SREG_TONE_OR_PULSE, 1);
		break;
	case 'P':
		set_sreg(SREG_TONE_OR_PULSE, 0);
		break;
	case 'L':
		/* the last number again */
		if (dial_string_[0]) {
			put_chars("Dialing ", 8);
			puts(dial_string_);
			put_chars("...", 3);
			crlf();
		}
		p = nullptr;
		semi = nullptr;
		break;
	default:
		break;
	}
	if (semi) {
		snprintf(dial_rest_, sizeof(dial_rest_), "AT%s", semi + 1);
		semi[1] = '\0';
	}
	/* the number keeps its T or P: call progress reads it */
	return dial(p) ? -1 : 1;
}

/* ATH, ATH0: on hook (hanging up), ATH1: off hook */
int Modem::at_H(char *p, int *len)
{
	int ret;
	switch (*p) {
	case '1':
		*len = 1;
		ret = hook(MODEM_HOOK_OFF);
		break;
	case '0':
		*len = 1;
		p++;
		/* fall through */
	default:
		if (isdigit((unsigned char)*p))
			return -1;
		ret = hook(MODEM_HOOK_ON);
		break;
	}
	return ret ? -1 : 0;
}

/* ATI0 to ATI7: the product, its version, the device and driver, the
   S-registers 0 to 23, the stored profiles (none), the country; ATI11
   the last call's statistics */
int Modem::at_I(char *p, int *len)
{
	char *end;
	const char *s;
	int c = strtoul(p, &end, 0);
	*len = end - p;
	switch (c) {
	case 0:
		puts(product);
		crlf();
		break;
	case 1:
	case 2:
		puts(product);
		put_chars(", ", 2);
		puts(version);
		crlf();
		puts(author);
		crlf();
		break;
	case 3:
		s = dev_name_ ? dev_name_ : "unknown";
		puts(s);
		crlf();
		s = host_.driver_name();
		puts(s ? s : "unknown");
		crlf();
		break;
	case 4:
		for (int i = 0; i < 24; i++) {
			char str[24];
			snprintf(str, sizeof(str), "s%.2d=%.3d ", i, sregs_[i]);
			puts(str);
			if (!((i + 1) % 8))
				crlf();
		}
		crlf();
		break;
	case 5:
		put_chars("Stored Profile 0:", 17);
		crlf();
		break;
	case 6:
		put_chars("Stored Profile 1:", 17);
		crlf();
		break;
	case 7:
		put_chars("Country: ", 9);
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

/* ATO: back online (no result now: CONNECT) */
int Modem::at_O(char *, int *)
{
	return online() ? -1 : 1;
}

/* ATSn?, ATSn=v: S-register n read or set */
int Modem::at_S(char *p, int *len)
{
	char *end;
	unsigned n = strtoul(p, &end, 0);
	int val;
	*len = end - p + 1;
	p = end;
	if (n >= sizeof(sregs_))
		return -1;
	switch (*p++) {
	case '?': {
		char s[16];
		val = sreg(n);
		if (val < 0)
			return -1;
		snprintf(s, sizeof(s), "%d", val);
		puts(s);
		crlf();
		return 0;
	}
	case '=':
		val = strtoul(p, &end, 0);
		*len += end - p;
		/* S103 is \N: +ES with it */
		if (n == SREG_EC && val >= 0 && val <= 5) {
			set_ec_mode(val);
			return 0;
		}
		return set_sreg(n, val);
	default:
		return -1;
	}
}

/* ATZ, ATZ0 to 3: reset (there are no profiles) */
int Modem::at_Z(char *p, int *len)
{
	switch (*p) {
	case '0':
	case '1':
	case '2':
	case '3':
		*len = 1;
		break;
	default:
		*len = 0;
		break;
	}
	reset();
	return 0;
}

/* AT+GCI=<code in hex>, +GCI=?, +GCI?: the country by its T.35 code */
int Modem::at_GCI(char *p, int *len)
{
	char s[16];
	char *end;
	unsigned long n;
	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {        /* the country codes */
			*len += 1;
			puts("+GCI:");
			put_chars("(", 1);
			for (const struct homolog_set *h = homolog_set;
			     h->name; h++) {
				snprintf(s, sizeof(s), "%x,", h->id);
				puts(s);
			}
			put_chars(")", 1);
			crlf();
		} else {
			n = strtoul(p, &end, 16);
			if (end == p)
				return -1;
			*len += end - p;
			if (homolog_init(n, nullptr))
				return -1;
		}
		break;
	case '?':
		snprintf(s, sizeof(s), "%x", homolog_->id);
		put_chars("+GCI:", 5);
		puts(s);
		crlf();
		*len = 1;
		break;
	default:
		*len = 0;
		return -1;
	}
	return 0;
}

/* V.92 modem-on-hold (V.250's +PMH, +PMHT, +PMHR): +PMH=0 or 1 lets
   the remote put us on hold or not, +PMHT=<0..13> is the T1 code we
   grant (Table 33/V.92; 0 denies), +PMHR asks the remote to go on hold
   (in online command mode, on a V.92 connection) */
int Modem::at_PMH(char *p, int *len)
{
	char s[32];
	int *v = &moh_off_, hi = 1;
	const char *name = "+PMH";

	*len = 0;
	switch (toupper(*p)) {
	case 'R':
		/* (=<s>: how long we keep it on hold, 10 s otherwise) */
		*len = 1;
		if (state_ != COMMAND_ONLINE || !dp_ || dp_->id != DP_V92)
			return -1;
		moh_req_ = 10;
		if (p[1] == '=' && isdigit((unsigned char)p[2])) {
			char *end;
			long n = strtol(p + 2, &end, 10);
			if (n < 1 || n > 960)
				return -1;
			moh_req_ = (int)n;
			*len += end - (p + 1);
		}
		return 0;
	case 'T':
		v = &moh_timer_;
		hi = 13;
		name = "+PMHT";
		p++;
		*len = 1;
		break;
	}
	switch (*p) {
	case '=':
		if (p[1] == '?') {
			snprintf(s, sizeof(s), "%s: (0-%d)", name, hi);
			puts(s);
			crlf();
			*len += 2;
			return 0;
		}
		if (!isdigit((unsigned char)p[1]))
			return -1;
		{
			char *end;
			long n = strtol(p + 1, &end, 10);
			if (n < 0 || n > hi)
				return -1;
			*v = (int)n;
			*len += end - p;
		}
		return 0;
	case '?':
		snprintf(s, sizeof(s), "%s: %d", name, *v);
		puts(s);
		crlf();
		*len += 1;
		return 0;
	}
	return -1;
}

/* AT+MS's carriers by V.250's names (and USRobotics' Bell ones): the
   pump ids */
static const struct {
	const char *name;
	unsigned id;
} ms_names[] = {
	{ "V21", DP_V21 }, { "V22", DP_V22 }, { "V22B", DP_V22BIS },
	{ "V23C", DP_V23 }, { "V32", DP_V32 }, { "V32B", DP_V32BIS },
	{ "V34", DP_V34 }, { "V90", DP_V90 }, { "V91", DP_V91 },
	{ "V92", DP_V92 }, { "B103", DP_B103 }, { "BELL103", DP_B103 },
	{ "B212", DP_B212 }, { "BELL212A", DP_B212 }, { "K56", DP_K56 },
};

/* AT+MS can ask for pump d: one registered, or K56flex while V.90 is
   (change_dp() runs V.90 for it: K56flex is not there) */
static bool ms_choice(const DpDriver *d)
{
	if (!d->code)
		return false;
	if (d->op)
		return true;
	if (d->id != DP_K56)
		return false;
	for (const DpDriver *v = dp_drivers(); v->id; v++)
		if (v->id == DP_V90)
			return v->op != nullptr;
	return false;
}

/* AT+MS=[mod][,[automode][,[min][,[max]]]] (mod: a registered pump's
   id, or 56, the rates in bit/s), +MS=?, +MS? */
int Modem::at_MS(char *p, int *len)
{
	char s[32];
	char *end;
	unsigned long n;
	unsigned dp_id;
	int auto_mode, min_rate, max_rate, found = 0;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			*len += 1;
			put_chars("(", 1);
			for (DpDriver *d = dp_drivers(); d->id; d++) {
				if (!ms_choice(d))
					continue;
				if (found++ > 0)
					put_chars(",", 1);
				snprintf(s, sizeof(s), "%d", d->id);
				puts(s);
			}
			put_chars("),(0,1),", 8);
			snprintf(s, sizeof(s), "(%u-%u),", MODEM_MIN_RATE,
				 MODEM_MAX_RATE);
			puts(s);
			put_chars(s, strlen(s) - 1);
			crlf();
			break;
		}
		dp_id = sregs_[SREG_DP];
		auto_mode = sregs_[SREG_AUTOMODE];
		min_rate = min_rate_;
		max_rate = max_rate_;
		/* AT+MS=[dp][,[automode][,[min][,[max]]]], dp a number or a
		   name */
		{
			int *items[3] = { &auto_mode, &min_rate, &max_rate };
			int i = 0;
			if (isalpha((unsigned char)*p)) {
				size_t n = strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						  "abcdefghijklmnopqrstuvwxyz"
						  "0123456789");
				bool known = false;
				for (const auto &m : ms_names)
					if (strlen(m.name) == n &&
					    !toupper_strncmp(p, m.name, n)) {
						dp_id = m.id;
						known = true;
					}
				if (!known)
					return -1;
				*len += n;
				p += n;
				i = 1;
				if (*p == ',') {
					*len += 1;
					p++;
				} else
					i = 4;
			}
			for (; i < 4; i++) {
				n = strtoul(p, &end, 0);
				if (end != p) {
					if (i == 0)
						dp_id = n;
					else
						*items[i - 1] = n;
				}
				*len += end - p;
				p = end;
				if (*p != ',')
					break;
				*len += 1;
				p++;
			}
		}
		if (dp_id != sregs_[SREG_DP]) {
			for (DpDriver *d = dp_drivers(); d->id; d++)
				if (ms_choice(d) && (unsigned)d->id == dp_id) {
					found++;
					break;
				}
			if (!found)
				return -1;
		}
		if ((auto_mode != 0 && auto_mode != 1) || min_rate > max_rate ||
		    min_rate < MODEM_MIN_RATE || max_rate > MODEM_MAX_RATE)
			return -1;
		sregs_[SREG_DP] = dp_id;
		sregs_[SREG_AUTOMODE] = auto_mode;
		min_rate_ = min_rate;
		max_rate_ = max_rate;
		break;
	case '?':
		snprintf(s, sizeof(s), "%d,%d,%u,%u", sregs_[SREG_DP],
			 sregs_[SREG_AUTOMODE], min_rate_, max_rate_);
		puts(s);
		crlf();
		*len = 1;
		break;
	default:
		*len = 0;
		return -1;
	}
	return 0;
}

/* AT+FCLASS=0, 1, 8 (data, fax class 1, voice), =?, ? */
int Modem::at_FCLASS(char *p, int *len)
{
	if (*p == '=') {
		p++;
		*len += 2;
		if (*p == '?') {
			put_chars("0,1,8", 5);
			crlf();
		} else if (isdigit((unsigned char)*p)) {
			int val = *p - '0';
			if (val != 0 && val != 1 && val != 8)
				return -1;
			return set_mode((enum MODEM_MODE)val);
		} else
			return -1;
	} else if (*p == '?') {
		char ch = '0' + mode_;
		*len += 1;
		put_chars(&ch, 1);
		crlf();
	} else
		return -1;
	return 0;
}

/* AT#CID=0 to 2 (and AT+VCID): caller ID off or on, =?, ? */
int Modem::at_CID(char *p, int *len)
{
	if (*p == '=') {
		p++;
		*len += 2;
		if (*p == '?') {
			put_chars("0,1,2", 5);
			crlf();
		} else if (isdigit((unsigned char)*p) && *p - '0' < 3)
			cid_requested_ = *p - '0';
		else
			return -1;
	} else if (*p == '?') {
		char ch = '0' + cid_requested_;
		*len += 1;
		put_chars(&ch, 1);
		crlf();
	} else
		return -1;
	return 0;
}

/*
 *    voice
 */

/* a numeric parameter of a voice command: where it goes, its range
   (0: no bound) */
struct Modem::VoiceParam {
	unsigned *val;
	unsigned min, max;
};

/* the parameters of voice command cmd: =? the ranges, ? the values,
   =a,b,c new values (each in its range, between prefix and suffix
   when given), set only if all are good */
int Modem::voice_params(char *p, const char *cmd, VoiceParam *param,
			int *len, const char *prefix, const char *suffix)
{
	char s[32];
	unsigned val[3];
	VoiceParam *prm;
	char *end;
	int i;

	*len += 1;
	if (*p == '=') {
		p++;
		if (*p == '?') {
			*len += 1;
			snprintf(s, sizeof(s), "+%s=", cmd);
			puts(s);
			if (prefix)
				puts(prefix);
			for (prm = param; prm->val; ) {
				snprintf(s, sizeof(s), "%d-%d", prm->min,
					 prm->max);
				puts(s);
				prm++;
				if (prm->val)
					put_chars(",", 1);
			}
			if (suffix)
				puts(suffix);
			crlf();
			return 0;
		}
		if (prefix) {
			if (strncmp(p, prefix, strlen(prefix)))
				return -1;
			p += strlen(prefix);
			*len += strlen(prefix);
		}
		prm = param;
		for (i = 0; prm->val && i < 3; i++) {
			val[i] = strtoul(p, &end, 0);
			if ((prm->min && val[i] < prm->min) ||
			    (prm->max && val[i] > prm->max))
				return -1;
			*len += end - p;
			p = end;
			prm++;
			if (prm->val) {
				if (*p != ',')
					return -1;
				*len += 1;
				p++;
			}
		}
		if (suffix) {
			if (strncmp(p, suffix, strlen(suffix)))
				return -1;
			*len += strlen(suffix);
		}
		prm = param;
		for (i = 0; prm->val && i < 3; i++, prm++)
			*prm->val = val[i];
		return 0;
	} else if (*p == '?') {
		snprintf(s, sizeof(s), "+%s=", cmd);
		puts(s);
		if (prefix)
			puts(prefix);
		for (prm = param; prm->val; ) {
			snprintf(s, sizeof(s), "%d", *prm->val);
			puts(s);
			prm++;
			if (prm->val)
				put_chars(",", 1);
		}
		if (suffix)
			puts(suffix);
		crlf();
		return 0;
	}
	return -1;
}

/* AT+V... in voice mode: +VIP, +VNH, +VGT, +VGR, +VIT, +VSD, +VSM,
   +VLS, +VTX, +VRX, +VTR, +VTS (a tone [f1,f2,d], a DTMF digit {s,d}
   or s) */
int Modem::at_voice(char *p, int *len)
{
	voice_info &v = voice_info_;
	*len += 2;
	if (!toupper_strncmp(p, "IP", 2)) {             /* +VIP */
		voice_init();
		return 0;
	} else if (!toupper_strncmp(p, "NH", 2)) {      /* +VNH=0,1 */
		unsigned val = 0;
		VoiceParam params[] = { { &val, 0, 1 }, {} };
		return voice_params(p + 2, "VNH", params, len, nullptr, nullptr);
	} else if (!toupper_strncmp(p, "GT", 2)) {      /* +VGT=127 */
		VoiceParam params[] = { { &v.tx_gain, 0, 128 }, {} };
		return voice_params(p + 2, "VGT", params, len, nullptr, nullptr);
	} else if (!toupper_strncmp(p, "GR", 2)) {      /* +VGR=127 */
		VoiceParam params[] = { { &v.rx_gain, 0, 128 }, {} };
		return voice_params(p + 2, "VGR", params, len, nullptr, nullptr);
	} else if (!toupper_strncmp(p, "IT", 2)) {      /* +VIT= */
		VoiceParam params[] = { { &v.inactivity_timer, 0, 255 }, {} };
		return voice_params(p + 2, "VIT", params, len, nullptr, nullptr);
	} else if (!toupper_strncmp(p, "SD", 2)) {      /* +VSD=128,50 */
		VoiceParam params[] = {
			{ &v.silence_detect_sensitivity, 0, 255 },
			{ &v.silence_detect_period, 0, 255 }, {} };
		return voice_params(p + 2, "VSD", params, len, nullptr, nullptr);
	} else if (!toupper_strncmp(p, "SM", 2)) {      /* +VSM=1,8000 */
		VoiceParam params[] = {
			{ &v.comp_method, 0, 1 },
			{ &v.sample_rate, 8000, 8000 }, {} };
		return voice_params(p + 2, "VSM", params, len, nullptr, nullptr);
	} else if (!toupper_strncmp(p, "LS", 2)) {      /* +VLS=1 */
		unsigned dev = voice_ != nullptr;
		VoiceParam params[] = { { &dev, 0, 1 }, {} };
		int ret = voice_params(p + 2, "VLS", params, len, nullptr,
				       nullptr);
		if (ret)
			return ret;
		return voice_set_device(dev);
	} else if (!toupper_strncmp(p, "TX", 2))
		return voice_command(VOICE_CMD_STATE_TX);
	else if (!toupper_strncmp(p, "RX", 2))
		return voice_command(VOICE_CMD_STATE_RX);
	else if (!toupper_strncmp(p, "TR", 2))
		return voice_command(VOICE_CMD_STATE_DUPLEX);
	else if (!toupper_strncmp(p, "TS", 2)) {        /* +VTS=[933,,150] */
		int orig_len, ret;
		p += 2;
		orig_len = *len;
		{
			VoiceParam params[] = {
				{ &v.tone1_freq, 0, 0 },
				{ &v.tone2_freq, 0, 0 },
				{ &v.tone_duration, 0, 6000 }, {} };
			if (*p == '?' || (*p == '=' && p[1] == '?'))
				return voice_params(p, "VTS", params, len,
						    nullptr, nullptr);
			ret = voice_params(p, "VTS", params, len, "[", "]");
			if (!ret)
				return voice_command(VOICE_CMD_BEEP);
		}
		{
			VoiceParam params[] = {
				{ &v.dtmf_symbol, 0, 0 },
				{ &v.tone_duration, 0, 100 }, {} };
			*len = orig_len;
			ret = voice_params(p, "VTS", params, len, "{", "}");
			if (!ret)
				return voice_command(VOICE_CMD_DTMF);
		}
		{
			VoiceParam params[] = { { &v.dtmf_symbol, 0, 0 }, {} };
			*len = orig_len;
			ret = voice_params(p, "VTS", params, len, nullptr,
					   nullptr);
			if (!ret)
				return voice_command(VOICE_CMD_DTMF);
		}
		return -1;
	}
	return -1;
}

/*
 *    fax class 1
 */

/* a class 1 action command for the fax engine: once it runs, what the
   DTE sends is the engine's until its result (its data, or the
   character that aborts a receive command: T.31 8.3), commands after
   that */
int Modem::fax_command(enum FAX_CMD cmd, unsigned val)
{
	int ret = FAX_class1_command(fax_, cmd, val);
	if (ret > 0)
		command_ = false;
	return ret;
}

/* class 1 fax: AT+FTS=, +FRS= (silence, 10 ms units), +FTM=, +FRM=
   (data at a modulation, =? the list), +FTH=, +FRH= (HDLC frames, 3:
   V.21) */
int Modem::at_fax(char *p, int *len)
{
	static const char rates[] = "24,48,72,73,74,96,97,98,121,122,145,146";
	unsigned val;
	char *end;
	*len += 2;
	if (!toupper_strncmp(p, "TS=", 3) || !toupper_strncmp(p, "RS=", 3)) {
		/* +FTS=, +FRS= */
		enum FAX_CMD cmd = toupper(*p) == 'T' ? FAX_CMD_FTS :
			FAX_CMD_FRS;
		*len += 1;
		p += 3;
		val = strtoul(p, &end, 0);
		if (p == end)
			return -1;
		*len += end - p;
		return fax_command(cmd, val);
	} else if (!toupper_strncmp(p, "TM=", 3) ||
		   !toupper_strncmp(p, "RM=", 3)) {
		/* +FTM=, +FRM= */
		enum FAX_CMD cmd = toupper(*p) == 'T' ? FAX_CMD_FTM :
			FAX_CMD_FRM;
		*len += 2;
		p += 3;
		if (*p == '?') {
			puts(rates);
			crlf();
			return 0;
		}
		val = strtoul(p, &end, 0);
		if (p == end)
			return -1;
		*len += end - p - 1;
		return fax_command(cmd, val);
	} else if (!toupper_strncmp(p, "RH=", 3) ||
		   !toupper_strncmp(p, "TH=", 3)) {
		/* +FRH=, +FTH= */
		enum FAX_CMD cmd = toupper(*p) == 'T' ? FAX_CMD_FTH :
			FAX_CMD_FRH;
		*len += 2;
		p += 3;
		if (*p == '?') {
			put_chars("3", 1);
			crlf();
			return 0;
		} else if (isdigit((unsigned char)*p))
			return fax_command(cmd, *p - '0');
		return -1;
	}
	return -1;
}

/*
 *    the commands of other modems' setup strings (V.250's, Rockwell's
 *    and Conexant's, USRobotics'), taken with no effect so that such a
 *    setup string does not end in ERROR: flow control (the pty does
 *    its own), reports, profiles, tones and settings the modulations
 *    have no use for
 */

/* v is in values, a list of numbers and ranges ("0-3,15,16") */
static bool in_values(const char *values, unsigned long v)
{
	while (*values) {
		char *end;
		unsigned long lo = strtoul(values, &end, 10), hi = lo;
		if (end == values)
			break;
		if (*end == '-')
			hi = strtoul(end + 1, &end, 10);
		if (v >= lo && v <= hi)
			return true;
		values = *end == ',' ? end + 1 : end;
	}
	return false;
}

/* a basic command taken: its letters and its values (without a number:
   0, unless it needs one) */
static const struct {
	const char *name, *values;
	bool number;
} taken_cmds[] = {
	{ "B", "0-3,15,16", true },     /* ITU or Bell at 1200 and 300 bit/s
					   (ATB alone: V.22) */
	{ "C", "1", false },            /* the carrier on */
	{ "F", "1", false },            /* no echo of data online */
	{ "W", "0-2", false },          /* what CONNECT says (&A, &E here) */
	{ "&B", "0-2", false },         /* V.32 retrains (USRobotics': the
					   DTE's rate fixed) */
	{ "&G", "0-2", false },         /* the V.22bis guard tone */
	{ "&I", "0-5", false },         /* XON/XOFF (USRobotics') */
	{ "&J", "0-1", false },         /* the auxiliary relay */
	{ "&K", "4-6", false },         /* flow control (&K0 to 3: here the
					   compression) */
	{ "&M", "0,4,5", false },       /* asynchronous (USRobotics': error
					   control: \N's here) */
	{ "&N", "0-14", false },        /* the rate (USRobotics') */
	{ "&W", "0-1", false },         /* the profile stored: none is */
	{ "&Y", "0-1", false },         /* the profile at power on */
	{ "\\G", "0-1", false },        /* XON/XOFF to the other modem */
	{ "\\J", "0-1", false },        /* the DTE's rate */
	{ "\\K", "0-5", false },        /* breaks */
	{ "\\Q", "0-3", false },        /* flow control */
	{ "\\R", "0-1", false },        /* the ring indicator */
	{ "\\T", "0-255", false },      /* the inactivity timer */
	{ "-C", "0-1", false },         /* the calling tone */
	/* Rockwell's MNP 10 and 10EC (cellular): power levels, the link's
	   speed, the V.22bis fallback, MNP Extended Services */
	{ "-K", "0-2", false },
	{ "-Q", "0-1", false },
	{ ")M", "0-2", false },
	{ "@M", "0-30", false },
	{ "*H", "0-2", false },
};

/* p at a command's letters: true if it is a basic command taken, *len
   its letters and number; false if not (another command, or a value
   the command has a meaning for here, or ERROR) */
static bool at_taken(const char *p, int *len)
{
	for (const auto &t : taken_cmds) {
		int n = strlen(t.name);
		unsigned long v = 0;
		char *end = (char *)p + n;
		if (toupper_strncmp(p, t.name, n))
			continue;
		if (isdigit((unsigned char)p[n]))
			v = strtoul(p + n, &end, 10);
		else if (t.number)
			return false;
		if (!in_values(t.values, v))
			return false;
		*len = end - p;
		return true;
	}
	return false;
}

/* an extended parameter kept: its name, its default and its ranges (as
   =? gives them); ? gives the values after the name and a colon, or
   (-SEC, Rockwell's) alone */
static const struct {
	const char *name, *dflt, *ranges;
} kept_params[] = {
	{ "+IFC", "2,2", "(0-3),(0-2)" },       /* flow control */
	{ "+ICF", "3,3", "(1-6),(0-3)" },       /* character framing */
	{ "+IPR", "0", "(0,300,1200,2400,4800,9600,19200,38400,57600,"
		"115200,230400,460800,921600)" },       /* the DTE's rate */
	{ "+ILRR", "0", "(0,1)" },              /* the DTE's rate reported */
	{ "+IDSR", "0", "(0,1)" },              /* DSR */
	{ "+ILSD", "0", "(0,1)" },              /* long space disconnect */
	{ "+MR", "0", "(0,1)" },                /* modulation reported */
	{ "+ER", "0", "(0,1)" },                /* error control reported */
	{ "+DR", "0", "(0,1)" },                /* compression reported */
	{ "+ES", "3,0,2", "(0-6),(0-4),(0-8)" },        /* error control */
	{ "+DS", "3,0,2048,32", "(0-3),(0,1),(512-65535),(6-250)" },
	{ "+DS44", "3,0,0,2048,2048,255,255,6144,6144",
		"(0-3),(0,1),(0,1),(256-65535),(256-65535),(32-255),"
		"(32-255),(512-65535),(512-65535)" },
	{ "+PCW", "2", "(0-2)" },               /* call waiting */
	{ "+PIG", "1", "(0,1)" },               /* V.92 PCM upstream */
	{ "+FAA", "0", "(0)" },                 /* fax adaptive answer */
	{ "+FPR", "0", "(0,1,2,4,8,10,18)" },   /* the fax DTE's rate */
	{ "-SEC", "0", "(0,1),(0-30)" },        /* MNP 10EC, tx level */
};
static_assert(sizeof(kept_params) / sizeof(kept_params[0]) <=
	      Modem::KEPT_PARAMS, "KEPT_PARAMS");

/* the values of a list ("3,0,2") into v, how many */
static unsigned kept_values(const char *s, unsigned long *v)
{
	unsigned n = 0;
	while (*s && n < Modem::KEPT_VALUES) {
		char *end;
		v[n++] = strtoul(s, &end, 10);
		s = *end == ',' ? end + 1 : end;
	}
	return n;
}

/* v is in group k of ranges ("(0-3),(0-2)") */
static bool kept_in_range(const char *ranges, unsigned k, unsigned long v)
{
	char group[128];
	const char *g = ranges;
	for (unsigned i = 0; g && i < k; i++)
		g = strchr(g + 1, '(');
	if (!g || *g != '(')
		return false;
	size_t n = strcspn(g + 1, ")");
	if (n >= sizeof(group))
		return false;
	memcpy(group, g + 1, n);
	group[n] = '\0';
	return in_values(group, v);
}

/* the kept parameters' defaults (ATZ) */
void Modem::kept_init()
{
	for (size_t i = 0; i < sizeof(kept_params) / sizeof(kept_params[0]);
	     i++)
		kept_n_[i] = kept_values(kept_params[i].dflt, kept_[i]);
}

/* p at a command's letters: a kept parameter's =<values>, ? or =?
   (0, or -1 for a value out of its range, *len what it took), or -2
   if p is not one */
int Modem::at_kept(const char *p, int *len)
{
	for (size_t i = 0; i < sizeof(kept_params) / sizeof(kept_params[0]);
	     i++) {
		const char *name = kept_params[i].name;
		int n = strlen(name);
		bool bare = name[0] == '-';
		char s[160];
		if (toupper_strncmp(p, name, n) || (p[n] != '=' && p[n] != '?'))
			continue;
		const char *q = p + n;
		if (*q == '?' || q[1] == '?') {
			size_t m = 0;
			if (*q == '?') {        /* the values */
				if (!bare)
					m = snprintf(s, sizeof(s), "%s: ", name);
				for (unsigned k = 0; k < kept_n_[i] &&
				     m < sizeof(s); k++)
					m += snprintf(s + m, sizeof(s) - m,
						      k ? ",%lu" : "%lu",
						      kept_[i][k]);
			} else                  /* the ranges */
				snprintf(s, sizeof(s), "%s: %s", name,
					 kept_params[i].ranges);
			puts(s);
			crlf();
			*len = q - p + (*q == '?' ? 1 : 2);
			return 0;
		}
		/* =<values>, each in its range; one left out stays */
		unsigned long v[KEPT_VALUES];
		unsigned count = kept_n_[i], k = 0;
		memcpy(v, kept_[i], sizeof(v));
		q++;
		if (*q && *q != ',' && *q != ';' &&
		    !isdigit((unsigned char)*q))
			return -1;
		for (;; k++) {
			if (isdigit((unsigned char)*q)) {
				char *end;
				unsigned long val = strtoul(q, &end, 10);
				if (k >= KEPT_VALUES ||
				    !kept_in_range(kept_params[i].ranges, k,
						   val))
					return -1;
				v[k] = val;
				if (k >= count)
					count = k + 1;
				q = end;
			}
			if (*q != ',')
				break;
			q++;
		}
		memcpy(kept_[i], v, sizeof(v));
		kept_n_[i] = count;
		*len = q - p;
		if (!strcmp(name, "+ES"))
			ec_mode_from_es();
		return 0;
	}
	return -2;
}

/* a kept parameter's values; its name must be in kept_params */
const unsigned long *Modem::kept(const char *name) const
{
	for (size_t i = 0; i < sizeof(kept_params) / sizeof(kept_params[0]);
	     i++)
		if (!strcmp(kept_params[i].name, name))
			return kept_[i];
	abort();
}

/* \N0 to \N5 (Smart Link's, Rockwell's) as +ES (V.250's orig_rqst,
   orig_fbk, ans_fbk): normal and direct without error correction,
   reliable (LAPM or MNP, required), auto-reliable (LAPM, MNP or none),
   LAPM only, MNP only */
static const unsigned long ec_modes[6][3] = {
	{ 1, 0, 1 }, { 0, 0, 0 }, { 3, 2, 4 }, { 3, 0, 2 }, { 3, 3, 5 },
	{ 4, 4, 6 },
};

/* \N<n>: S103 and +ES */
void Modem::set_ec_mode(int n)
{
	sregs_[SREG_EC] = (unsigned char)n;
	for (size_t i = 0; i < sizeof(kept_params) / sizeof(kept_params[0]);
	     i++)
		if (!strcmp(kept_params[i].name, "+ES")) {
			memcpy(kept_[i], ec_modes[n], sizeof(ec_modes[n]));
			kept_n_[i] = 3;
		}
}

/* +ES set: S103 the \N that has its values (as it was if none has) */
void Modem::ec_mode_from_es()
{
	const unsigned long *es = kept("+ES");
	for (int n = 0; n < 6; n++)
		if (!memcmp(es, ec_modes[n], sizeof(ec_modes[n])))
			sregs_[SREG_EC] = (unsigned char)n;
}

/* AT&V, AT&V0: the active settings (no profiles are stored): the basic
   commands, the S-registers in use, the extended settings and the
   parameters kept for other modems' setup strings, in lines of 72
   columns at most */
int Modem::at_ampV(char *p, int *len)
{
	static const struct {
		const char *fmt;
		int reg;
	} basic[] = {
		{ "E%d", SREG_ECHO }, { "L%d", SREG_SPEAKER_VOLUME },
		{ "M%d", SREG_SPEAKER_CONTROL }, { "N%d", SREG_AUTOMODE },
		{ "Q%d", SREG_QUIET }, { nullptr, SREG_TONE_OR_PULSE },
		{ "V%d", SREG_VERBOSE }, { "X%d", SREG_X_CODE },
		{ "Y%d", SREG_DEFAULT_SETTING },
		{ "&A%d", SREG_CONNNECT_MSG_FORMAT }, { "&C%d", SREG_CD },
		{ "&E%d", SREG_CONNNECT_MSG_SPEED_SRC },
		{ "&H%d", SREG_FLOW_CONTROL }, { "&P%d", SREG_PULSE_RATIO },
		{ "\\A%d", SREG_MNP_BLOCK }, { "\\N%d", SREG_EC },
		{ "\\V%d", SREG_CONNECT_PROTOCOL }, { "%%C%d", SREG_COMP },
		{ "%%E%d", SREG_LINE_QUALITY_CONTROL },
	};
	static const unsigned char regs[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24,
		28, 29, 30, 31, 32, 39, 56, 60, 62, 70, 71, 92, 103, 104, 105,
		106, 161,
	};
	char line[160], item[160];
	size_t n = 0;
	/* an item on the line, or on the next one */
	auto add = [&](const char *it) {
		size_t k = strlen(it);
		if (n && n + 1 + k > 72) {
			puts(line);
			crlf();
			n = 0;
		}
		n += snprintf(line + n, sizeof(line) - n, n ? " %s" : "%s", it);
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
	};
	/* the line so far */
	auto flush = [&]() {
		if (n) {
			puts(line);
			crlf();
		}
		n = 0;
	};

	*len = 0;
	if (isdigit((unsigned char)*p)) {
		if (*p != '0')
			return -1;
		*len = 1;
	}
	puts("ACTIVE PROFILE:");
	crlf();
	for (const auto &b : basic) {
		if (b.fmt)
			snprintf(item, sizeof(item), b.fmt, sregs_[b.reg]);
		else
			snprintf(item, sizeof(item), "%s",
				 sregs_[b.reg] ? "T" : "P");
		add(item);
	}
	flush();
	for (unsigned r : regs) {
		snprintf(item, sizeof(item), "S%02u:%03u", r, sregs_[r]);
		add(item);
	}
	flush();
	snprintf(item, sizeof(item), "+MS=%d,%d,%u,%u", sregs_[SREG_DP],
		 sregs_[SREG_AUTOMODE], min_rate_, max_rate_);
	add(item);
	snprintf(item, sizeof(item), "+FCLASS=%d", mode_);
	add(item);
	snprintf(item, sizeof(item), "+GCI=%x", homolog_->id);
	add(item);
	snprintf(item, sizeof(item), "+PMH=%d", moh_off_);
	add(item);
	snprintf(item, sizeof(item), "+PMHT=%d", moh_timer_);
	add(item);
	snprintf(item, sizeof(item), "#CID=%u", cid_requested_);
	add(item);
	for (size_t i = 0; i < sizeof(kept_params) / sizeof(kept_params[0]);
	     i++) {
		size_t m = snprintf(item, sizeof(item), "%s=",
				    kept_params[i].name);
		for (unsigned k = 0; k < kept_n_[i] && m < sizeof(item); k++)
			m += snprintf(item + m, sizeof(item) - m,
				      k ? ",%lu" : "%lu", kept_[i][k]);
		add(item);
	}
	flush();
	puts("STORED PROFILES: none");
	crlf();
	return 0;
}

/*
 *    a command line
 */

/* a command line (without blanks): its commands in turn until one
   fails or has no result now; returns 0 (OK), 1 (no result now: A, D,
   O, voice...) or -1 (ERROR) */
int Modem::at_command(char *cmd)
{
	char c, *p = cmd;
	int len = 0, ret = -1;

	if (toupper(*p++) != 'A' || toupper(*p++) != 'T')
		return -1;

	while (*p) {
		/* other modems' commands, taken */
		len = 0;
		ret = at_kept(p, &len);
		if (ret == -1)
			return -1;
		if (ret == 0 || at_taken(p, &len)) {
			p += len;
			continue;
		}
		c = toupper(*p++);
		len = 0;
		switch (c) {
		case ';':
			/* between commands (V.250's after an extended one) */
			ret = 0;
			break;
		case 'A':
			return at_A(p, &len);
		case 'B':
			ret = at_B(p, &len);
			break;
		case 'D':
			return at_D(p, &len);
		case 'E':
			ret = at_set_sreg(SREG_ECHO, *p, 0, 1, &len);
			break;
		case 'H':
			ret = at_H(p, &len);
			break;
		case 'I':
			ret = at_I(p, &len);
			break;
		case 'L':
			ret = at_set_sreg(SREG_SPEAKER_VOLUME, *p, 0, 3, &len);
			if (!ret)
				update_speaker();
			break;
		case 'M':
			ret = at_set_sreg(SREG_SPEAKER_CONTROL, *p, 0, 3, &len);
			if (!ret)
				update_speaker();
			break;
		case 'N':
			ret = at_set_sreg(SREG_AUTOMODE, *p, 0, 1, &len);
			break;
		case 'O':
			ret = at_O(p, &len);
			break;
		case 'P':
			ret = set_sreg(SREG_TONE_OR_PULSE, 0);
			break;
		case 'Q':
			ret = at_set_sreg(SREG_QUIET, *p, 0, 1, &len);
			break;
		case 'S':
			ret = at_S(p, &len);
			break;
		case 'T':
			ret = set_sreg(SREG_TONE_OR_PULSE, 1);
			break;
		case 'V':
			ret = at_set_sreg(SREG_VERBOSE, *p, 0, 1, &len);
			break;
		case 'X':
			ret = at_set_sreg(SREG_X_CODE, *p, 0, 4, &len);
			break;
		case 'Y':
			ret = at_set_sreg(SREG_DEFAULT_SETTING, *p, 0, 3, &len);
			break;
		case 'Z':
			return at_Z(p, &len);
		case '#':
			if (!toupper_strncmp(p, "CID", 3)) {
				p += 3;
				ret = at_CID(p, &len);
			} else
				ret = -1;
			break;
		case '%':
			c = toupper(*p++);
			switch (c) {
			case 'C':
				ret = at_set_sreg(SREG_COMP, *p, 0, 3, &len);
				break;
			case 'E':
				ret = at_set_sreg(SREG_LINE_QUALITY_CONTROL, *p,
						  0, 2, &len);
				break;
			case 'B':
				/* %B as *B: the blacklisted numbers (none) */
				ret = 0;
				break;
			default:
				return -1;
			}
			break;
		case '&':
			c = toupper(*p++);
			switch (c) {
			case 'A':
				ret = at_set_sreg(SREG_CONNNECT_MSG_FORMAT, *p, 0,
						  7, &len);
				break;
			case 'C':
				ret = at_set_sreg(SREG_CD, *p, 0, 1, &len);
				break;
			case 'D':
			case 'R':
			case 'S':
			case 'F':
				len = isdigit((unsigned char)*p) ? 1 : 0;
				ret = 0;
				break;
			case 'E':
				ret = at_set_sreg(SREG_CONNNECT_MSG_SPEED_SRC, *p,
						  0, 1, &len);
				break;
			case 'H':
				ret = at_set_sreg(SREG_FLOW_CONTROL, *p, 0, 3,
						  &len);
				break;
			case 'K': {
				/* USRobotics': &K0 none, &K1 and &K2 both, &K3
				   V.42bis (or V.44) without MNP 5 */
				static const uint8_t comp[4] = { 0, 3, 3, 2 };
				ret = at_set_sreg(SREG_COMP, *p, 0, 3, &len);
				if (!ret)
					sregs_[SREG_COMP] =
						comp[sregs_[SREG_COMP]];
				break;
			}
			case 'P':
				ret = at_set_sreg(SREG_PULSE_RATIO, *p, 0, 3,
						  &len);
				break;
			case 'V':
				ret = at_ampV(p, &len);
				break;
			case 'Q':
				/* &Q0, &Q6: \N0; &Q5, &Q8, &Q9: \N3 (V.42,
				   then MNP) */
				len = isdigit((unsigned char)*p) ? 1 : 0;
				switch (len ? *p : '0') {
				case '0':
				case '6':
					set_ec_mode(0);
					ret = 0;
					break;
				case '5':
				case '8':
				case '9':
					set_ec_mode(3);
					ret = 0;
					break;
				default:
					return -1;
				}
				break;
			default:
				return -1;
			}
			break;
		case '*':
			/* *B: the blacklisted numbers (none kept), always OK */
			c = toupper(*p++);
			if (c != 'B')
				return -1;
			ret = 0;
			break;
		case '+':
			c = toupper(*p++);
			switch (c) {
			case 'G':
				c = toupper(*p++);
				switch (c) {
				case 'C':       /* +GCI, +GCAP */
					if (toupper(*p) == 'I')
						ret = at_GCI(++p, &len);
					else if (!toupper_strncmp(p, "AP", 2)) {
						p += 2;
						puts("+GCAP: +FCLASS,+MS");
						crlf();
						ret = 0;
					} else
						return -1;
					break;
				case 'M':       /* +GMI, +GMM, +GMR (and ?) */
					c = toupper(*p++);
					switch (c) {
					case 'I':
						puts(author);
						break;
					case 'M':
						puts(product);
						break;
					case 'R':
						puts(version);
						break;
					default:
						return -1;
					}
					crlf();
					len = *p == '?' ? 1 : 0;
					ret = 0;
					break;
				default:
					return -1;
				}
				break;
			case 'F':
				if (!toupper_strncmp(p, "CLASS", 5)) {
					p += 5;
					ret = at_FCLASS(p, &len);
				} else if (toupper(p[0]) == 'M' && p[1] &&
					   strchr("IMR", toupper(p[1]))) {
					/* +FMI?, +FMM?, +FMR?: as +GMI... */
					c = toupper(p[1]);
					p += 2;
					puts(c == 'I' ? author :
					     c == 'M' ? product : version);
					crlf();
					len = *p == '?' ? 1 : 0;
					ret = 0;
				} else
					ret = at_fax(p, &len);
				break;
			case 'M':
				c = toupper(*p++);
				if (c != 'S')
					return -1;
				ret = at_MS(p, &len);
				break;
			case 'P':
				if (!toupper_strncmp(p, "MH", 2)) {
					p += 2;
					ret = at_PMH(p, &len);
				} else
					ret = -1;
				break;
			case 'V':
				if (!toupper_strncmp(p, "CID", 3)) {
					/* +VCID: #CID's caller ID, in any
					   mode */
					p += 3;
					ret = at_CID(p, &len);
				} else if (mode_ != MODEM_MODE_VOICE)
					return -1;
				else
					ret = at_voice(p, &len);
				break;
			default:
				return -1;
			}
			break;
		case '\\':
			c = toupper(*p++);
			switch (c) {
			case 'A':
				/* \A0..3: MNP's block size, 64 to 256 */
				ret = at_set_sreg(SREG_MNP_BLOCK, *p, 0, 3, &len);
				break;
			case 'N':
				ret = at_set_sreg(SREG_EC, *p, 0, 5, &len);
				if (!ret)
					set_ec_mode(sregs_[SREG_EC]);
				break;
			case 'V':
				/* \V1 (\V2 the same): the protocol after
				   CONNECT's rate */
				ret = at_set_sreg(SREG_CONNECT_PROTOCOL, *p, 0, 2,
						  &len);
				break;
			default:
				return -1;
			}
			break;
		default:
			return -1;
		}
		if (ret)
			return ret;
		p += len;
	}
	return 0;
}

} // namespace dcore
