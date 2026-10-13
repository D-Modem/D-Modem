/*
 *
 *    at.cpp - the AT command interpreter of the modem core
 *
 *    slmodemd's modem_at.c: the same commands, parsed the same way.
 *
 *      AT  A B D E H I L M N O P Q S T V X Y Z
 *      AT& A C D E F H K P R S (V W: ERROR)
 *      AT\ A N    AT% C E    AT* B    AT# CID
 *      AT+ MS GCI GMI GMM GMR FCLASS IFC, class 1 fax (FTS FRS FTM FRM
 *          FTH FRH), voice (+V..., in voice mode)
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
   number again); no result now */
int Modem::at_D(char *p, int *)
{
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
		break;
	default:
		break;
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
   S-registers 0 to 23, the stored profiles (none), the country */
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

/* AT+MS=[mod][,[automode][,[min][,[max]]]] (mod: a registered pump's
   id, the rates in bit/s), +MS=?, +MS? */
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
				if (!d->code || !d->op)
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
		/* AT+MS=[dp][,[automode][,[min][,[max]]]] */
		{
			int *items[3] = { &auto_mode, &min_rate, &max_rate };
			for (int i = 0; i < 4; i++) {
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
				if (d->op && d->code && (unsigned)d->id == dp_id) {
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
		return FAX_class1_command(fax_, cmd, val);
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
		return FAX_class1_command(fax_, cmd, val);
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
			return FAX_class1_command(fax_, cmd, *p - '0');
		return -1;
	}
	return -1;
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
		c = toupper(*p++);
		len = 0;
		switch (c) {
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
			ret = at_set_sreg(SREG_SPEAKER_CONTROL, *p, 0, 2, &len);
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
				ret = at_set_sreg(SREG_FLOW_CONTROL, *p, 0, 1,
						  &len);
				break;
			case 'K':
				ret = at_set_sreg(SREG_COMP, *p, 0, 3, &len);
				break;
			case 'P':
				ret = at_set_sreg(SREG_PULSE_RATIO, *p, 0, 3,
						  &len);
				break;
			case 'V':
			case 'W':
				ret = -1;
				break;
			default:
				return -1;
			}
			break;
		case '*':
			c = toupper(*p++);
			if (c != 'B')
				return -1;
			break;
		case '+':
			c = toupper(*p++);
			switch (c) {
			case 'G':
				c = toupper(*p++);
				switch (c) {
				case 'C':       /* +GCI */
					if (toupper(*p++) == 'I')
						ret = at_GCI(p, &len);
					else
						return -1;
					break;
				case 'M':       /* +GMI, +GMM, +GMR */
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
				} else
					ret = at_fax(p, &len);
				break;
			case 'I':
				/* +IFC=2,2 (flow control: taken, not done) */
				if (!toupper_strncmp(p, "FC=2,2", 6)) {
					p += 6;
					ret = 0;
				} else
					ret = -1;
				break;
			case 'M':
				c = toupper(*p++);
				if (c == 'S')
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
				if (mode_ != MODEM_MODE_VOICE)
					return -1;
				if (!toupper_strncmp(p, "CID", 3)) {
					p += 3;
					ret = at_CID(p, &len);
				} else
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
				if (isdigit((unsigned char)*p))
					len++;
				break;
			case 'N':
				ret = at_set_sreg(SREG_EC, *p, 0, 5, &len);
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
