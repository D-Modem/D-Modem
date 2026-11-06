/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    config.cpp - the configurators' settings model (config.h), AT!R's
 *                 init string, the keys from the terminal, and AT!C's
 *                 look at the terminal: whether it answers ANSI's cursor
 *                 position report (the full-screen configurator, its
 *                 size and whether it takes UTF-8) or not (the command
 *                 line)
 *
 */

#include <cstring>

#include "dcore.h"
#include "config.h"
#include "str.h"

namespace dcore {

namespace {

/* a lone ESC (not an escape sequence's) after this many ms */
const u32 ESC_MS = 120;
/* the terminal's answer to a cursor position request within */
const u32 PROBE_MS = 1500;

/* name in lower case, with '-' for what is not a letter or digit */
std::string token(const std::string &name)
{
	std::string t;
	for (char c : name) {
		if (c >= 'A' && c <= 'Z')
			t += static_cast<char>(c - 'A' + 'a');
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			t += c;
		else if (!t.empty() && t.back() != '-')
			t += '-';
	}
	while (!t.empty() && t.back() == '-')
		t.pop_back();
	return t;
}

/* a setting of choices */
Setting pick(const char *key, const char *group, const char *label,
	     const char *help, const char *form,
	     std::vector<Choice> choices)
{
	Setting s;
	s.key = key;
	s.group = group;
	s.label = label;
	s.help = help;
	s.form = form;
	s.kind = Setting::Pick;
	s.choices = std::move(choices);
	return s;
}

/* a setting of a number from lo to hi */
Setting number(const char *key, const char *group, const char *label,
	       const char *help, const char *form, u32 lo, u32 hi)
{
	Setting s;
	s.key = key;
	s.group = group;
	s.label = label;
	s.help = help;
	s.form = form;
	s.kind = Setting::Number;
	s.lo = lo;
	s.hi = hi;
	return s;
}

/* what the kept parameters are, for people */
const struct {
	const char *name, *label;
} kept_labels[] = {
	{ "+IFC", "DTE flow control" },
	{ "+ICF", "DTE character framing" },
	{ "+IPR", "DTE rate" },
	{ "+ILRR", "Local rate reporting" },
	{ "+IDSR", "DSR control" },
	{ "+ILSD", "Line signal detect" },
	{ "+MR", "Modulation reporting" },
	{ "+ER", "Error control reporting" },
	{ "+DR", "Compression reporting" },
	{ "+ES", "Error control (V.250)" },
	{ "+ESA", "V.80 synchronous access" },
	{ "+ITF", "V.80 transmit buffer" },
	{ "+A8E", "V.8 and V.8 bis" },
	{ "+DSVD", "DSVD (V.70) offered" },
	{ "+ASVD", "ASVD (V.61) offered" },
	{ "+DS", "V.42bis compression" },
	{ "+DS44", "V.44 compression" },
	{ "+DSZ", "zstd compression" },
	{ "+B2", "Clear channel link layer" },
	{ "+CBST", "Bearer service" },
	{ "+CRC", "Cellular result codes" },
	{ "+PCW", "V.92 call waiting" },
	{ "+PIG", "V.92 PCM upstream" },
	{ "+FAA", "Adaptive answer" },
	{ "+FPR", "Fax DTE rate" },
	{ "-SEC", "MNP 10EC" },
	{ "+CVM", "V.cvm" },
};

/* the S-registers that are settings of their own (no command of their
   own sets them): number, key, label, help, highest value */
const struct {
	u8 reg;
	const char *key, *label, *help;
	u32 hi;
} plain_sregs[] = {
	{ 0, "auto-answer", "Rings before answering",
	  "S0: 0 never answers by itself", 255 },
	{ 6, "dial-tone-wait", "Dial tone wait (s)",
	  "S6: seconds to wait for a dial tone", 255 },
	{ 7, "carrier-wait", "Carrier wait (s)",
	  "S7: seconds to wait for the other modem", 255 },
	{ 8, "comma-pause", "Comma pause (s)",
	  "S8: a ',' in the number waits this long", 255 },
	{ 9, "carrier-detect-time", "Carrier detect (0.1 s)",
	  "S9: the carrier must last this long", 255 },
	{ 10, "carrier-loss-time", "Carrier loss (0.1 s)",
	  "S10: hang up after the carrier is gone this long", 255 },
	{ 11, "dtmf-duration", "DTMF tone length (ms)",
	  "S11: each digit's tone and the gap after it", 255 },
	{ 24, "flash-time", "Hook flash (10 ms)",
	  "S24: how long a hook flash lasts", 255 },
	{ 92, "answer-delay", "Answer delay (s)",
	  "S92: from ATA to the answer tone", 255 },
	{ 18, "test-timer", "Test timer (s)",
	  "S18: how long an AT&T test runs, 0 until AT&T0", 255 },
	{ 48, "simulated-carrier", "Simulated carrier (V.13)",
	  "S48: 1 sends V.13's simulated carrier", 1 },
	{ 49, "fast-connect", "Fast connect (bits)",
	  "S49: 1 the short V.22 handshake, 2 with HDLC flags", 3 },
	{ 50, "fast-answer-tone", "Fast answer tone (10 ms)",
	  "S50: the fast connect answer tone, 0 for 400 ms", 255 },
	{ 2, "escape-char", "Escape character",
	  "S2: the +++ escape's character (43 is '+')", 255 },
	{ 12, "escape-guard", "Escape guard time (20 ms)",
	  "S12: the quiet before and after +++", 255 },
	{ 3, "cr-char", "Line end character",
	  "S3: ends a command line (13 is CR)", 127 },
	{ 4, "lf-char", "Line feed character",
	  "S4: follows S3 in responses (10 is LF)", 127 },
	{ 5, "bs-char", "Backspace character",
	  "S5: erases a character (8 is BS)", 127 },
};

} // namespace

/*
 *    the model
 */

/* the settings, in the order they are listed */
std::vector<Setting> Config::model(const Modem &m)
{
	std::vector<Setting> v;
	/* S-register reg's value */
	auto reg = [](u8 r) {
		return [r](const Modem &md) { return dec(md.sregs_[r]); };
	};

	/* the line: the country first, for AT!R (+GCI sets S8 and S24) */
	{
		Setting s = pick("country", "Line", "Country",
				 "+GCI: the line's settings and tones",
				 "+GCI=%s", {});
		for (const struct homolog_set *h = homolog_set; h->name; h++) {
			std::string value = hex(h->id, 2, true);
			bool dup = false;
			for (const auto &c : s.choices)
				dup |= c.value == value;
			if (!dup)
				s.choices.push_back({ value, token(h->name),
						      h->name });
		}
		s.get = [](const Modem &md) {
			return hex(md.homolog_->id, 2, true);
		};
		v.push_back(s);
	}
	v.push_back(pick("dial-mode", "Line", "Dialling", "T tone, P pulse",
			 "%s", { { "T", "tone", "Tone" },
				 { "P", "pulse", "Pulse" } }));
	v.back().get = [](const Modem &md) {
		return std::string(md.sregs_[SREG_TONE_OR_PULSE] ? "T" : "P");
	};
	v.push_back(pick("pulse-ratio", "Line", "Pulse make/break",
			 "&P (S28): kept; the country sets the pulses",
			 "&P%s", { { "0", "0", "&P0" }, { "1", "1", "&P1" },
				   { "2", "2", "&P2" },
				   { "3", "3", "&P3" } }));
	v.back().get = reg(SREG_PULSE_RATIO);
	v.push_back(pick("line-type", "Line", "Line type",
			 "&L (S47): a dialled line, or a leased one",
			 "&L%s", { { "0", "dialup", "Dial-up" },
				   { "1", "leased-2wire", "Leased 2-wire" },
				   { "2", "leased-4wire",
				     "Leased 4-wire" } }));
	v.back().get = reg(SREG_LEASED);
	v.push_back(pick("call-progress", "Line", "Result codes and tones",
			 "X (S56): CONNECT's rate, dial tone, busy",
			 "X%s", { { "0", "basic", "X0 basic" },
				  { "1", "rate", "X1 rate" },
				  { "2", "dial-tone", "X2 dial tone" },
				  { "3", "busy", "X3 busy" },
				  { "4", "full", "X4 all" } }));
	v.back().get = reg(SREG_X_CODE);

	/* the modulation (+MS's four values, one setting each) */
	{
		Setting s = pick("modulation", "Modulation", "Modulation",
				 "+MS: the highest modulation tried",
				 "+MS=%s", {});
		u32 now = m.sregs_[SREG_DP];
		bool has_now = false;
		for (const DpDriver *d = dp_drivers(); d->id; d++) {
			if (!d->code || !ms_may_choose(d->id))
				continue;
			s.choices.push_back({ dec(static_cast<s16>(d->id)),
					      token(d->name), d->name });
			has_now |= d->id == now;
		}
		if (!has_now) {
			const DpDriver *d = dp_driver(now);
			s.choices.push_back({ dec(now), d ? token(d->name) :
					      "dp" + dec(now),
					      d ? d->name : dec(now) });
		}
		s.ms = 0;
		s.get = reg(SREG_DP);
		v.push_back(s);
	}
	v.push_back(pick("automode", "Modulation", "Automode",
			 "+MS, N: fall back to slower modulations",
			 "+MS=,%s", { { "0", "off", "Off" },
				      { "1", "on", "On" } }));
	v.back().ms = 1;
	v.back().get = reg(SREG_AUTOMODE);
	v.push_back(number("min-rate", "Modulation", "Lowest rate (bit/s)",
			   "+MS: hang up rather than go below it",
			   "+MS=,,%s", MODEM_MIN_RATE, MODEM_MAX_RATE));
	v.back().ms = 2;
	v.back().get = [](const Modem &md) { return dec(md.min_rate_); };
	v.push_back(number("max-rate", "Modulation", "Highest rate (bit/s)",
			   "+MS: the fastest the modem tries",
			   "+MS=,,,%s", MODEM_MIN_RATE, MODEM_MAX_RATE));
	v.back().ms = 3;
	v.back().get = [](const Modem &md) { return dec(md.max_rate_); };

	/* error control and compression */
	v.push_back(pick("error-control", "Error control",
			 "Error correction", "\\N (S103): V.42, MNP or none",
			 "\\N%s", { { "0", "normal", "Normal (none)" },
				    { "1", "direct", "Direct (none)" },
				    { "2", "reliable", "Reliable" },
				    { "3", "auto-reliable",
				      "Auto-reliable" },
				    { "4", "lapm", "V.42 LAPM only" },
				    { "5", "mnp", "MNP only" } }));
	v.back().get = reg(SREG_EC);
	v.push_back(pick("compression", "Error control", "Compression",
			 "%C (S104): needs error correction",
			 "%%C%s", { { "0", "off", "None" },
				    { "1", "mnp5", "MNP 5" },
				    { "2", "v42bis-v44", "V.42bis or V.44" },
				    { "3", "auto", "Either" } }));
	v.back().get = reg(SREG_COMP);
	v.push_back(pick("mnp-block", "Error control", "MNP block size",
			 "\\A (S105): MNP's longest block",
			 "\\A%s", { { "0", "64", "64 octets" },
				    { "1", "128", "128 octets" },
				    { "2", "192", "192 octets" },
				    { "3", "256", "256 octets" } }));
	v.back().get = reg(SREG_MNP_BLOCK);
	v.push_back(pick("line-quality", "Error control",
			 "Line quality monitor", "%E (S39): kept",
			 "%%E%s", { { "0", "0", "%E0" }, { "1", "1", "%E1" },
				    { "2", "2", "%E2" } }));
	v.back().get = reg(SREG_LINE_QUALITY_CONTROL);

	/* the terminal */
	v.push_back(pick("echo", "Terminal", "Command echo",
			 "E (S13): the modem echoes what you type",
			 "E%s", { { "0", "off", "Off" },
				  { "1", "on", "On" } }));
	v.back().get = reg(SREG_ECHO);
	v.push_back(pick("quiet", "Terminal", "Result codes",
			 "Q (S14): OK, CONNECT and the others shown or not",
			 "Q%s", { { "0", "shown", "Shown" },
				  { "1", "hidden", "Hidden" } }));
	v.back().get = reg(SREG_QUIET);
	v.push_back(pick("verbose", "Terminal", "Result code form",
			 "V (S15): words or numbers", "V%s",
			 { { "0", "numeric", "Numbers" },
			   { "1", "words", "Words" } }));
	v.back().get = reg(SREG_VERBOSE);
	v.push_back(number("connect-lines", "Terminal",
			   "Lines before CONNECT",
			   "&A (S70): 1 modulation, 2 protocol, 4 rate",
			   "&A%s", 0, 7));
	v.back().get = reg(SREG_CONNNECT_MSG_FORMAT);
	v.push_back(pick("connect-protocol", "Terminal",
			 "Protocol in CONNECT", "\\V (S106): CONNECT 33600/LAPM",
			 "\\V%s", { { "0", "off", "Off" },
				    { "1", "on", "On" },
				    { "2", "on2", "On (\\V2)" } }));
	v.back().get = reg(SREG_CONNECT_PROTOCOL);
	v.push_back(pick("connect-rate", "Terminal", "CONNECT rate source",
			 "&E (S71): kept", "&E%s",
			 { { "0", "0", "&E0" }, { "1", "1", "&E1" } }));
	v.back().get = reg(SREG_CONNNECT_MSG_SPEED_SRC);
	v.push_back(pick("dcd", "Terminal", "DCD", "&C (S60): kept",
			 "&C%s", { { "0", "always-on", "Always on" },
				   { "1", "carrier", "Follows carrier" } }));
	v.back().get = reg(SREG_CD);
	v.push_back(pick("flow-control", "Terminal", "Flow control",
			 "&H (S62): as #UD reports it", "&H%s",
			 { { "0", "none", "None" },
			   { "1", "rts-cts", "RTS/CTS" },
			   { "2", "xon-xoff", "XON/XOFF" },
			   { "3", "both", "Both" } }));
	v.back().get = reg(SREG_FLOW_CONTROL);
	v.push_back(number("y", "Terminal", "Y setting", "Y (S161): kept",
			   "Y%s", 0, 3));
	v.back().get = reg(SREG_DEFAULT_SETTING);

	/* the speaker */
	v.push_back(pick("speaker", "Speaker", "Speaker", "M (S29)", "M%s",
			 { { "0", "off", "Off" },
			   { "1", "until-connect", "On until CONNECT" },
			   { "2", "always", "Always on" },
			   { "3", "answer-only",
			     "On after dialling until CONNECT" } }));
	v.back().get = reg(SREG_SPEAKER_CONTROL);
	v.push_back(pick("volume", "Speaker", "Speaker volume", "L (S30)",
			 "L%s", { { "0", "lowest", "Lowest" },
				  { "1", "low", "Low" },
				  { "2", "medium", "Medium" },
				  { "3", "high", "High" } }));
	v.back().get = reg(SREG_SPEAKER_VOLUME);

	/* fax, voice, V.92 */
	{
		std::vector<Choice> c = { { "0", "data", "Data" },
					  { "1", "fax1", "Fax class 1" },
					  { "1.0", "fax1.0", "Fax class 1.0" } };
		if (m.has_fax2()) {
			c.push_back({ "2", "fax2", "Fax class 2" });
			c.push_back({ "2.0", "fax2.0", "Fax class 2.0" });
		}
		c.push_back({ "8", "voice", "Voice" });
		v.push_back(pick("mode", "Fax and voice", "Mode",
				 "+FCLASS: data, fax or voice",
				 "+FCLASS=%s", c));
		v.back().get = [](const Modem &md) { return md.fclass(); };
	}
	v.push_back(pick("caller-id", "Fax and voice", "Caller ID",
			 "#CID: shown after the first RING", "#CID=%s",
			 { { "0", "off", "Off" },
			   { "1", "formatted", "Formatted" },
			   { "2", "raw", "Unformatted" } }));
	v.back().get = [](const Modem &md) {
		return dec(md.cid_requested_);
	};
	v.push_back(pick("modem-on-hold", "Fax and voice",
			 "V.92 modem-on-hold", "+PMH: granted or refused",
			 "+PMH=%s", { { "0", "on", "Granted" },
				      { "1", "off", "Refused" } }));
	v.back().get = [](const Modem &md) { return dec(md.moh_off_); };
	v.push_back(number("modem-on-hold-time", "Fax and voice",
			   "Modem-on-hold time code",
			   "+PMHT: the longest hold granted (V.92's code)",
			   "+PMHT=%s", 0, 13));
	v.back().get = [](const Modem &md) { return dec(md.moh_timer_); };

	/* the timers and characters */
	for (const auto &r : plain_sregs) {
		std::string form = "S" + dec(r.reg) + "=%s";
		v.push_back(number(r.key, (r.reg == 2 || r.reg == 12 ||
				    (r.reg >= 3 && r.reg <= 5)) ?
				   "Characters" : "Timers", r.label, r.help,
				   "", 0, r.hi));
		v.back().form = form;
		v.back().get = reg(r.reg);
	}

	/* the extended parameters (+DS, +ES, ...) */
	for (s16 i = 0; i < kept_param_count(); i++) {
		const char *name = kept_param_name(i);
		const char *label = name;
		for (const auto &l : kept_labels)
			if (!strcmp(l.name, name))
				label = l.label;
		Setting s;
		s.key = token(name);
		s.group = "Extended";
		s.label = label;
		s.help = std::string(name) + "=" + kept_param_ranges(i);
		s.form = std::string(name) + "=%s";
		s.kind = Setting::Text;
		s.get = [i](const Modem &md) {
			std::string t;
			for (u16 j = 0; j < md.kept_n_[i]; j++)
				t += (j ? "," : "") + dec(md.kept_[i][j]);
			return t;
		};
		v.push_back(s);
	}
	return v;
}

/* each setting's value now */
std::vector<std::string> Config::values(const Modem &m,
					const std::vector<Setting> &s)
{
	std::vector<std::string> v;
	for (const auto &x : s)
		v.push_back(x.get(m));
	return v;
}

/* each setting's default: the values that make it are set on m for the
   time it takes to read them, then m has its own again */
std::vector<std::string> Config::defaults(Modem &m,
					  const std::vector<Setting> &s)
{
	unsigned char sregs[sizeof(m.sregs_)];
	u32 kept[Modem::KEPT_PARAMS][Modem::KEPT_VALUES];
	u16 kept_n[Modem::KEPT_PARAMS];
	const u32 min = m.min_rate_, max = m.max_rate_;
	const s16 moh_off = m.moh_off_, moh_timer = m.moh_timer_;
	const u16 cid = m.cid_requested_;
	const enum MODEM_MODE mode = m.mode_;
	const u8 fax_class = m.fax_class_;

	memcpy(sregs, m.sregs_, sizeof(sregs));
	memcpy(kept, m.kept_, sizeof(kept));
	memcpy(kept_n, m.kept_n_, sizeof(kept_n));
	memset(m.sregs_, 0, sizeof(m.sregs_));
	m.sregs_init();
	m.sregs_[SREG_DIAL_PAUSE_TIME] = m.homolog_->params->DialPauseTime;
	m.sregs_[SREG_FLASH_TIMER] = m.homolog_->params->HookFlashTime;
	m.kept_init();
	m.min_rate_ = MODEM_MIN_RATE;
	m.max_rate_ = MODEM_MAX_RATE;
	m.moh_off_ = 0;
	m.moh_timer_ = 5;
	m.cid_requested_ = 0;
	m.mode_ = MODEM_MODE_DATA;
	m.fax_class_ = 1;
	std::vector<std::string> v = values(m, s);
	/* (the country's default is the USA's, whatever the modem has) */
	for (size_t i = 0; i < s.size(); i++)
		if (s[i].key == "country")
			v[i] = hex(MODEM_DEFAULT_COUNTRY_CODE, 2, true);
	memcpy(m.sregs_, sregs, sizeof(sregs));
	memcpy(m.kept_, kept, sizeof(kept));
	memcpy(m.kept_n_, kept_n, sizeof(kept_n));
	m.min_rate_ = min;
	m.max_rate_ = max;
	m.moh_off_ = moh_off;
	m.moh_timer_ = moh_timer;
	m.cid_requested_ = cid;
	m.mode_ = mode;
	m.fax_class_ = fax_class;
	return v;
}

/* the AT command that sets s to value (without AT) */
std::string Config::command(const Setting &s, const std::string &value)
{
	std::string c;
	for (size_t i = 0; i < s.form.size(); i++) {
		if (s.form[i] == '%' && i + 1 < s.form.size()) {
			i++;
			c += s.form[i] == 's' ? value : std::string(1, s.form[i]);
		} else {
			c += s.form[i];
		}
	}
	return c;
}

/* the command lines that set what differs from the defaults: each
   command in turn (the +MS fields as one +MS, where the first is),
   an extended one ended by ';' when another follows it on its line */
std::vector<std::string> Config::init_lines(
	const std::vector<Setting> &s, const std::vector<std::string> &vals,
	const std::vector<std::string> &defs, size_t width)
{
	std::vector<std::string> cmds, lines;
	s16 ms_at[4] = { -1, -1, -1, -1 };
	bool ms_differs = false, ms_done = false;

	for (size_t i = 0; i < s.size(); i++)
		if (s[i].ms >= 0 && s[i].ms < 4) {
			ms_at[s[i].ms] = static_cast<s16>(i);
			ms_differs |= vals[i] != defs[i];
		}
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i].ms >= 0) {
			if (!ms_differs || ms_done)
				continue;
			std::string c = "+MS=";
			for (s16 f = 0; f < 4; f++)
				c += (f ? "," : "") +
					(ms_at[f] >= 0 ? vals[ms_at[f]] : "");
			cmds.push_back(c);
			ms_done = true;
			continue;
		}
		if (vals[i] != defs[i])
			cmds.push_back(command(s[i], vals[i]));
	}
	std::string line;
	bool ext = false;
	for (const auto &c : cmds) {
		std::string sep = ext ? ";" : "";
		if (!line.empty() && 2 + line.size() + sep.size() + c.size() >
		    width) {
			lines.push_back("AT" + line);
			line.clear();
			sep.clear();
		}
		line += sep + c;
		ext = c[0] == '+' || c[0] == '#' || c[0] == '-';
	}
	if (!line.empty())
		lines.push_back("AT" + line);
	return lines;
}

/* the choice whose value or name is v (the name in any case) */
const Choice *Config::choice(const Setting &s, const std::string &v)
{
	for (const auto &c : s.choices)
		if (c.value == v)
			return &c;
	for (const auto &c : s.choices)
		if (same_nocase(c.name, v))
			return &c;
	return nullptr;
}

/* "" if s takes value, else why not */
std::string Config::check(const Setting &s, const std::string &value)
{
	if (value.empty())
		return "a value is needed";
	switch (s.kind) {
	case Setting::Pick:
		for (const auto &c : s.choices)
			if (c.value == value)
				return "";
		return "not one of its values";
	case Setting::Number: {
		u32 v = 0;
		if (value.size() > 9)
			return "too large";
		for (char c : value) {
			if (c < '0' || c > '9')
				return "a number is needed";
			v = v * 10 + static_cast<u32>(c - '0');
		}
		if (v < s.lo || v > s.hi)
			return "out of range (" + dec(s.lo) + "-" + dec(s.hi) +
				")";
		return "";
	}
	case Setting::Text:
		if (value.size() > 50)
			return "too long";
		for (char c : value)
			if ((c < '0' || c > '9') && c != ',')
				return "numbers separated by commas are needed";
		return "";
	}
	return "";
}

/* value as people read it */
std::string Config::shown(const Setting &s, const std::string &value)
{
	if (s.kind == Setting::Pick)
		for (const auto &c : s.choices)
			if (c.value == value)
				return c.label;
	return value;
}

/*
 *    the session
 */

namespace {

/* AT!C and AT!CF: what the terminal answers, then the configurator */
class Probe final : public Config {
public:
	/* auto: the command line for a terminal that does not answer */
	Probe(Modem &m, bool automatic) : Config(m), auto_(automatic) {}
	/* where the cursor is? */
	void start() override {
		out("\x1b[6n");
		after(PROBE_MS);
	}

private:
	enum class Step { Ansi, Size, Utf8 };
	bool auto_;
	Step step_ = Step::Ansi;
	s16 rows_ = 24, cols_ = 80;
	/* the terminal's answers: an ANSI terminal; its size (the cursor
	   as far as it goes); whether é took one column */
	void key(const Key &k) override {
		if (k.code != Key::Cursor)
			return;
		switch (step_) {
		case Step::Ansi:
			step_ = Step::Size;
			out("\x1b[999;999H\x1b[6n");
			after(PROBE_MS);
			break;
		case Step::Size:
			rows_ = k.row;
			cols_ = k.col;
			step_ = Step::Utf8;
			out("\r\xc3\xa9\x1b[6n");
			after(PROBE_MS);
			break;
		case Step::Utf8:
			finish(make_screen(m_, rows_, cols_, k.col == 2));
			break;
		}
	}
	/* no answer: no ANSI (the command line), or what it had */
	void tick() override {
		switch (step_) {
		case Step::Ansi:
			if (auto_) {
				out("\r\n");
				finish(make_cli(m_));
			} else {
				finish(make_screen(m_, 24, 80, true));
			}
			break;
		case Step::Size:
			finish(make_screen(m_, 24, 80, true));
			break;
		case Step::Utf8:
			finish(make_screen(m_, rows_, cols_, true));
			break;
		}
	}
};

} // namespace

/* a configurator of kind k on m */
Config *Config::make(Modem &m, Kind k)
{
	if (k == Kind::Cli)
		return make_cli(m);
	return new Probe(m, k == Kind::Auto);
}

/* the DTE's characters as keys: escape sequences (ANSI's and VT100's)
   whole, CR LF as one Enter */
void Config::input(const char *buf, s32 n)
{
	for (s32 i = 0; i < n && !done_; i++) {
		char c = buf[i];
		Key k;
		if (in_esc_) {
			if (esc_.empty() && c == 0x1b) {
				/* ESC ESC: the first alone */
				k.code = Key::Esc;
				key(k);
				esc_at_ = now();
				continue;
			}
			esc_ += c;
			bool csi = esc_[0] == '[', ss3 = esc_[0] == 'O';
			if (esc_.size() == 1 && (csi || ss3))
				continue;
			if (csi && !(c >= 0x40 && c <= 0x7e) &&
			    esc_.size() < 16)
				continue;
			esc_end(false);
			continue;
		}
		if (c == 0x1b) {
			in_esc_ = true;
			esc_.clear();
			esc_at_ = now();
			continue;
		}
		if (c == '\n' && cr_) {
			cr_ = false;
			continue;
		}
		cr_ = c == '\r';
		if (c == '\r' || c == '\n') {
			k.code = Key::Enter;
		} else if (c == '\t') {
			k.code = Key::Tab;
		} else if (c == 0x7f || c == 0x08) {
			k.code = Key::Backspace;
		} else if (c > 0 && c < 0x20) {
			k.code = Key::Ctrl;
			k.ch = static_cast<char>(c + 'a' - 1);
		} else if (static_cast<unsigned char>(c) >= 0x20) {
			k.code = Key::Char;
			k.ch = c;
		} else {
			continue;
		}
		key(k);
	}
}

/* the escape sequence read (or, timeout, all there will be) as a key */
void Config::esc_end(bool timeout)
{
	Key k;
	const std::string s = esc_;
	in_esc_ = false;
	esc_.clear();
	if (s.empty()) {
		k.code = Key::Esc;
	} else if (timeout && (s[0] == '[' || s[0] == 'O')) {
		/* (a sequence cut short) */
		k.code = Key::Esc;
	} else if (s[0] != '[' && s[0] != 'O') {
		k.code = Key::Alt;
		k.ch = s[0] >= 'A' && s[0] <= 'Z' ?
			static_cast<char>(s[0] - 'A' + 'a') : s[0];
	} else {
		char f = s.back();
		/* the first number of a CSI, and its second (after ';') */
		s32 a = 0, b = 0;
		size_t i = 1;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9')
			a = a * 10 + (s[i++] - '0');
		if (i < s.size() && s[i] == ';')
			for (i++; i < s.size() && s[i] >= '0' && s[i] <= '9';
			     i++)
				b = b * 10 + (s[i] - '0');
		switch (f) {
		case 'A': k.code = Key::Up; break;
		case 'B': k.code = Key::Down; break;
		case 'C': k.code = Key::Right; break;
		case 'D': k.code = Key::Left; break;
		case 'H': k.code = Key::Home; break;
		case 'F': k.code = Key::End; break;
		case 'Z': k.code = Key::BackTab; break;
		case 'P': k.code = Key::F1; break;
		case 'Q': k.code = Key::F2; break;
		case 'S': k.code = Key::F4; break;
		case 'R':
			if (s[0] == 'O' || !b) {
				k.code = Key::F3;
			} else {
				k.code = Key::Cursor;
				k.row = static_cast<s16>(a);
				k.col = static_cast<s16>(b);
			}
			break;
		case '~':
			switch (a) {
			case 1: case 7: k.code = Key::Home; break;
			case 4: case 8: k.code = Key::End; break;
			case 2: k.code = Key::Ins; break;
			case 3: k.code = Key::Del; break;
			case 5: k.code = Key::PgUp; break;
			case 6: k.code = Key::PgDn; break;
			case 11: k.code = Key::F1; break;
			case 12: k.code = Key::F2; break;
			case 13: k.code = Key::F3; break;
			case 14: k.code = Key::F4; break;
			case 15: k.code = Key::F5; break;
			case 17: k.code = Key::F6; break;
			case 18: k.code = Key::F7; break;
			case 19: k.code = Key::F8; break;
			case 20: k.code = Key::F9; break;
			case 21: k.code = Key::F10; break;
			default: return;
			}
			break;
		default:
			return;
		}
	}
	key(k);
}

/* what is due: a lone ESC, the timer */
void Config::poll(u64 t)
{
	if (in_esc_ && t - esc_at_ >= ESC_MS)
		esc_end(true);
	if (!done_ && timer_at_ && static_cast<s64>(t - timer_at_) >= 0) {
		timer_at_ = 0;
		tick();
	}
}

/* the ms until poll() has something to do, -1 none */
s32 Config::next_ms(u64 t) const
{
	s64 left = -1;
	if (in_esc_)
		left = static_cast<s64>(esc_at_ + ESC_MS) -
			static_cast<s64>(t);
	if (timer_at_) {
		s64 l = static_cast<s64>(timer_at_) - static_cast<s64>(t);
		if (left < 0 && !in_esc_)
			left = l;
		else if (l < left)
			left = l;
	}
	if (!in_esc_ && !timer_at_)
		return -1;
	return static_cast<s32>(left < 0 ? 0 : left);
}

/* text to the DTE */
void Config::out(const std::string &s)
{
	m_.put_chars(s.data(), static_cast<s32>(s.size()));
}

/* the ms now */
u64 Config::now() const
{
	return m_.host_.now_ms();
}

/* a timer for tick() */
void Config::after(u32 ms)
{
	timer_at_ = ms ? now() + ms : 0;
	if (ms && !timer_at_)
		timer_at_ = 1;
}

/* the modem is idle and on hook */
bool Config::idle() const
{
	return m_.conf_idle();
}

/* the setting set through its AT command */
s16 Config::apply(const Setting &s, const std::string &value)
{
	if (!check(s, value).empty())
		return -1;
	return run(command(s, value));
}

/* an AT command run, the modem idle */
s16 Config::run(const std::string &cmd)
{
	if (!idle())
		return -1;
	std::string c = "AT" + cmd;
	std::vector<char> buf(c.begin(), c.end());
	buf.push_back('\0');
	m_.logf("config: %s\n", c.c_str());
	return m_.at_command(buf.data()) ? -1 : 0;
}

/* the product and its version */
std::string Config::product()
{
	return std::string(Modem::product) + " " + Modem::version;
}

/*
 *    the modem's side (dcore.h)
 */

/* AT!R: the init string; AT!C, AT!CF, AT!CT: a configurator, with the
   modem idle and on hook (the rest of the line not read) */
s16 Modem::at_bang(char *p, s16 *len)
{
	char c = upper_ascii(p[1]);
	if (c == 'R') {
		const std::vector<Setting> s = Config::model(*this);
		const std::vector<std::string> defs = Config::defaults(*this, s);
		std::vector<std::string> lines = Config::init_lines(
			s, Config::values(*this, s), defs, 63);
		if (lines.empty())
			lines.push_back("AT");
		for (const auto &l : lines) {
			puts(l);
			crlf();
		}
		*len = 2;
		return 0;
	}
	if (c != 'C')
		return -1;
	Config::Kind k = Config::Kind::Auto;
	char d = upper_ascii(p[2]);
	if (d == 'F')
		k = Config::Kind::Screen;
	else if (d == 'T')
		k = Config::Kind::Cli;
	else if ((d >= 'A' && d <= 'Z') || (d >= '0' && d <= '9'))
		return -1;
	if (!conf_idle() || conf_)
		return -1;
	*len = static_cast<s16>(strlen(p));
	logf("configurator\n");
	conf_ = Config::make(*this, k);
	conf_->start();
	return 1;
}

/* idle and on hook: no call, no line, nothing running */
bool Modem::conf_idle() const
{
	return state_ == IDLE && !started_ && hook_ == MODEM_HOOK_ON &&
		!dp_ && !voice_ && !voice_held_ && !dialed_ && !fax_ &&
		!test_end_ms_ && !voice_dialing_;
}

/* the DTE's characters to the configurator, one at a time (what comes
   after its end is the AT interpreter's) */
void Modem::conf_feed()
{
	char c;
	conf_check();
	while (conf_ && get_chars(&c, 1) == 1) {
		conf_->input(&c, 1);
		conf_check();
	}
}

/* a configurator that has ended: the next one, or OK and the commands
   that wait */
void Modem::conf_check()
{
	while (conf_ && conf_->done()) {
		Config *n = conf_->next();
		delete conf_;
		conf_ = n;
		if (n) {
			n->start();
			continue;
		}
		logf("configurator: done\n");
		report(Result::Ok);
		if (command_ && xmit_count_ > 0)
			at_process();
	}
}

/* the configurator ends early (a RING, DTR off), with no result */
void Modem::conf_close()
{
	if (!conf_)
		return;
	Config *c = conf_;
	conf_ = nullptr;
	c->leave();
	delete c;
	logf("configurator: closed\n");
}

/* the configurator's timers */
void Modem::conf_poll()
{
	if (!conf_)
		return;
	conf_->poll(host_.now_ms());
	conf_check();
}

/* the ms until the configurator has something due, -1 none */
s32 Modem::conf_next_ms() const
{
	return conf_ ? conf_->next_ms(host_.now_ms()) : -1;
}

} // namespace dcore
