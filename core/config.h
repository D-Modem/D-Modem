/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    config.h - the built-in configurators: AT!CF (a full-screen one in
 *               the style of Borland's Turbo Vision), AT!CT (a command
 *               line in the style of Cisco's IOS), AT!C (the one that
 *               suits the terminal), and AT!R (the init string that
 *               sets what differs from the defaults)
 *
 *    All four share one settings model (Config::model()): each setting
 *    has its AT command, its values and how to read it from the modem.
 *    A configurator changes a setting by running its AT command, so it
 *    takes exactly what the AT interpreter takes, and AT!R prints those
 *    same commands.
 *
 */
#ifndef DMODEM_CORE_CONFIG_H
#define DMODEM_CORE_CONFIG_H

#include <functional>
#include <string>
#include <vector>

#include "dm_types.h"

namespace dcore {

class Modem;

/* one of a setting's values: its AT text, its name on the command line,
   and what it means */
struct Choice {
	std::string value, name, label;
};

/* a setting of the modem */
struct Setting {
	/* a value from a list, a number, or a list of numbers (an extended
	   parameter's) */
	enum Kind { Pick, Number, Text };
	std::string key;        /* its name on the command line: "echo" */
	std::string group;      /* the group it is listed in */
	std::string label;      /* what it is, for people */
	std::string help;       /* one line more on it */
	std::string form;       /* its AT command, "%s" the value: "E%s" */
	Kind kind = Number;
	std::vector<Choice> choices;    /* Pick's */
	u32 lo = 0, hi = 255;           /* Number's */
	/* the field of +MS it is (0 the modulation to 3 the highest
	   rate), -1 none: AT!R puts the four in one +MS */
	s16 ms = -1;
	/* its value now, as its AT command takes it */
	std::function<std::string(const Modem &)> get;
};

/* a key from the terminal (Config::input()) */
struct Key {
	enum Code {
		None, Char, Enter, Esc, Tab, BackTab, Backspace, Up, Down,
		Left, Right, Home, End, PgUp, PgDn, Ins, Del, F1, F2, F3, F4,
		F5, F6, F7, F8, F9, F10, Alt, Ctrl,
		/* the terminal's cursor position report (row, col) */
		Cursor,
	};
	Code code = None;
	char ch = 0;            /* Char's, Alt's, Ctrl's (its letter) */
	s16 row = 0, col = 0;   /* Cursor's */
};

/* a configurator on the DTE: it has the DTE's characters while it runs
   (the modem idle and on hook), and ends with OK */
class Config {
public:
	/* which one AT asked for */
	enum class Kind { Auto, Screen, Cli };

	/* the settings, in the order they are listed */
	static std::vector<Setting> model(const Modem &m);
	/* each setting's value now */
	static std::vector<std::string> values(const Modem &m,
					       const std::vector<Setting> &s);
	/* each setting's default: its value after ATZ (S0 and the others
	   ATZ keeps as at the start), in the country the modem is in */
	static std::vector<std::string> defaults(Modem &m,
						 const std::vector<Setting> &s);
	/* the command lines (AT...) that set the values that differ from
	   the defaults, none for none; each at most width characters */
	static std::vector<std::string> init_lines(
		const std::vector<Setting> &s,
		const std::vector<std::string> &vals,
		const std::vector<std::string> &defs, size_t width);
	/* "" if value is one setting s takes, else why not */
	static std::string check(const Setting &s, const std::string &value);
	/* value as people read it: a choice's label, else the value */
	static std::string shown(const Setting &s, const std::string &value);
	/* the choice whose value or name is v, or null */
	static const Choice *choice(const Setting &s, const std::string &v);
	/* the AT command that sets s to value (without AT) */
	static std::string command(const Setting &s, const std::string &value);

	/* a configurator of kind k on m (not started) */
	static Config *make(Modem &m, Kind k);
	virtual ~Config() = default;
	/* it begins: its first screen or prompt */
	virtual void start() = 0;
	/* the DTE's characters */
	void input(const char *buf, s32 n);
	/* the time has come for what is due (the ms now) */
	void poll(u64 now);
	/* the ms until something is due, -1 none */
	s32 next_ms(u64 now) const;
	/* it has ended; with another configurator to go on in (next()) */
	bool done() const { return done_; }
	/* (see done()) */
	Config *next() { Config *n = next_; next_ = nullptr; return n; }
	/* the terminal is left as it should be (the modem ends it early:
	   a RING, DTR off) */
	virtual void leave() {}

protected:
	/* on m */
	explicit Config(Modem &m) : m_(m) {}
	/* a key, after input() decoded it */
	virtual void key(const Key &k) = 0;
	/* text to the DTE */
	void out(const std::string &s);
	/* the ms now */
	u64 now() const;
	/* the setting set to value through its AT command: 0, or -1 (the
	   modem said ERROR, or it is no longer idle) */
	s16 apply(const Setting &s, const std::string &value);
	/* an AT command (without AT) run: 0, or -1 (ERROR, or the modem
	   no longer idle) */
	s16 run(const std::string &cmd);
	/* the modem is idle and on hook */
	bool idle() const;
	/* it ends (OK to the DTE), or goes on in n */
	void finish(Config *n = nullptr) { done_ = true; next_ = n; }
	/* a timer at ms from now for tick(), 0: none */
	void after(u32 ms);
	/* the timer's time has come */
	virtual void tick() {}
	/* the modem's product name and version */
	static std::string product();

	Modem &m_;

private:
	/* the escape sequence being read: its characters after ESC */
	std::string esc_;
	bool in_esc_ = false;
	bool cr_ = false;               /* the last character was CR */
	u64 esc_at_ = 0;                /* when ESC came */
	u64 timer_at_ = 0;              /* after()'s, 0 none */
	bool done_ = false;
	Config *next_ = nullptr;
	/* the escape sequence's end: its key */
	void esc_end(bool timeout);
};

/* the full-screen one, the command line one (config_tui.cpp,
   config_cli.cpp); utf8 false: the screen in CP437 */
Config *make_screen(Modem &m, s16 rows, s16 cols, bool utf8);
Config *make_cli(Modem &m);

/* the kept parameters' table of at.cpp: how many, a parameter's name,
   how many values, its values at the start, its ranges */
s16 kept_param_count();
const char *kept_param_name(s16 i);
u16 kept_param_n(s16 i);
const u32 *kept_param_values(s16 i);
const char *kept_param_ranges(s16 i);
/* +MS may choose pump id */
bool ms_may_choose(u32 id);

} // namespace dcore

#endif
