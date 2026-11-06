/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    config_cli.cpp - AT!CT: the configurator as a command line in the
 *                     style of Cisco's IOS
 *
 *    "dmodem#" is the exec mode (show running-config, show settings,
 *    show init-string, configure terminal, exit), "dmodem(config)#" the
 *    configuration mode: "<setting> <value>", "no <setting>", "default
 *    <setting>", "do <exec command>", exit or end (or ^Z).  A word may be
 *    cut short where it is the only one that begins so; '?' lists what
 *    may come next, Tab completes a word, the arrows go through the
 *    lines typed before.  A change takes effect at once, through the
 *    setting's AT command.
 *
 */

#include <algorithm>
#include <cstdio>

#include "dcore.h"
#include "config.h"
#include "str.h"

namespace dcore {

namespace {

const char *const HOST = "dmodem";
const size_t MAX_LINE = 160;
const size_t HISTORY = 20;

/* a word that may come next, and what it does */
struct Word {
	std::string word, help;
};

/* what may come at a point of a command: words, or a setting's value
   (setting), and whether the command may end there (cr) */
struct Node {
	std::vector<Word> words;
	const Setting *setting = nullptr;
	bool cr = false;
};

/* a line parsed: its words in full (the value as its AT text), or where
   it went wrong */
struct Parsed {
	enum Error { None, Invalid, Ambiguous, Incomplete } error = None;
	size_t at = 0;          /* where (Invalid) */
	std::string word;       /* the word (Ambiguous) */
	std::vector<std::string> words;
	const Setting *setting = nullptr;       /* its setting, if any */
};

/* the command line configurator */
class Cli final : public Config {
public:
	/* on m */
	explicit Cli(Modem &m) : Config(m) {
		model_ = model(m);
		defs_ = defaults(m, model_);
		for (const auto &s : model_) {
			bool seen = false;
			for (const auto &g : groups_)
				seen |= g == s.group;
			if (!seen)
				groups_.push_back(s.group);
		}
	}
	/* the banner and the prompt */
	void start() override {
		out("\r\n" + product() + " configuration\r\n");
		out("Type '?' for help, 'exit' to return to AT command mode."
		    "\r\n\r\n");
		prompt();
	}

private:
	std::vector<Setting> model_;
	std::vector<std::string> defs_, groups_;
	bool config_ = false;           /* in configuration mode */
	std::string line_;
	std::vector<std::string> history_;
	size_t hist_at_ = 0;

	/* the prompt for the mode */
	std::string prompt_text() const {
		return std::string(HOST) + (config_ ? "(config)#" : "#");
	}
	/* the prompt and the line so far */
	void prompt() {
		out(prompt_text() + line_);
	}
	/* the line on the screen replaced by s */
	void replace_line(const std::string &s) {
		std::string erase;
		for (size_t i = 0; i < line_.size(); i++)
			erase += "\b \b";
		line_ = s;
		out(erase + line_);
	}
	/* "<setting>"'s value now, as the command line names it */
	std::string named(const Setting &s, const std::string &v) const {
		if (s.kind == Setting::Pick)
			for (const auto &c : s.choices)
				if (c.value == v)
					return c.name;
		return v;
	}
	/* a setting by its key */
	const Setting *find(const std::string &key) const {
		for (const auto &s : model_)
			if (s.key == key)
				return &s;
		return nullptr;
	}
	/* its index */
	size_t index(const Setting *s) const {
		return static_cast<size_t>(s - model_.data());
	}

	/* the settings as words */
	std::vector<Word> setting_words() const {
		std::vector<Word> w;
		for (const auto &s : model_)
			w.push_back({ s.key, s.label });
		return w;
	}
	/* exec mode's words after path */
	Node exec_node(const std::vector<std::string> &path, size_t from) const {
		Node n;
		size_t k = path.size() - from;
		const std::string first = k ? path[from] : "";
		if (!k) {
			n.words = {
				{ "configure", "Enter configuration mode" },
				{ "exit", "Return to AT command mode" },
				{ "help", "Description of the help system" },
				{ "logout", "Return to AT command mode" },
				{ "show", "Show the modem's settings" },
				{ "write", "Keep the configuration" },
			};
		} else if (first == "configure") {
			if (k == 1)
				n.words = { { "terminal",
					      "Configure from the terminal" } };
			else
				n.cr = k == 2;
		} else if (first == "show") {
			if (k == 1) {
				n.words = {
					{ "init-string",
					  "AT commands that set this configuration" },
					{ "running-config",
					  "Settings that differ from the defaults" },
					{ "settings", "Every setting, by group" },
					{ "version", "The modem's version" },
				};
			} else if (k == 2 && path[from + 1] == "settings") {
				for (const auto &g : groups_)
					n.words.push_back({ token(g), g + " settings" });
				n.cr = true;
			} else {
				n.cr = k == 2 || (k == 3 && path[from + 1] ==
						  "settings");
			}
		} else if (first == "write") {
			if (k == 1)
				n.words = { { "memory",
					      "Keep the configuration" } };
			else
				n.cr = k == 2;
		} else {
			n.cr = k == 1;
		}
		return n;
	}
	/* the words after path, in the mode it is in */
	Node node(const std::vector<std::string> &path) const {
		if (!config_)
			return exec_node(path, 0);
		Node n;
		size_t k = path.size();
		if (!k) {
			n.words = setting_words();
			n.words.push_back({ "default",
					    "Set a setting to its default" });
			n.words.push_back({ "do", "Run an exec command" });
			n.words.push_back({ "end",
					    "Exit from configuration mode" });
			n.words.push_back({ "exit",
					    "Exit from configuration mode" });
			n.words.push_back({ "help",
					    "Description of the help system" });
			n.words.push_back({ "no",
					    "Turn a setting off, or to its default" });
			return n;
		}
		const std::string &first = path[0];
		if (first == "do")
			return exec_node(path, 1);
		if (first == "no" || first == "default") {
			if (k == 1)
				n.words = setting_words();
			else
				n.cr = k == 2;
			return n;
		}
		if (const Setting *s = find(first)) {
			if (k == 1)
				n.setting = s;
			else
				n.cr = k == 2;
			return n;
		}
		n.cr = k == 1;
		return n;
	}
	/* a token for a setting's value: its AT text, "" if it is none */
	static std::string value_of(const Setting &s, const std::string &t) {
		if (s.kind != Setting::Pick)
			return check(s, t).empty() ? t : "";
		for (const auto &c : s.choices)
			if (same_nocase(c.name, t) || c.value == t)
				return c.value;
		const Choice *one = nullptr;
		for (const auto &c : s.choices)
			if (starts_nocase(c.name, t)) {
				if (one)
					return "";
				one = &c;
			}
		return one ? one->value : "";
	}

	/* line's words, each matched */
	Parsed parse(const std::string &line) const {
		Parsed p;
		size_t i = 0;
		while (true) {
			while (i < line.size() && line[i] == ' ')
				i++;
			if (i >= line.size())
				break;
			size_t start = i;
			while (i < line.size() && line[i] != ' ')
				i++;
			const std::string t = line.substr(start, i - start);
			Node n = node(p.words);
			if (n.setting) {
				std::string v = value_of(*n.setting, t);
				if (v.empty()) {
					p.error = Parsed::Invalid;
					p.at = start;
					return p;
				}
				p.setting = n.setting;
				p.words.push_back(v);
				continue;
			}
			std::string match;
			s16 count = 0;
			for (const auto &w : n.words) {
				if (same_nocase(w.word, t)) {
					match = w.word;
					count = 1;
					break;
				}
				if (starts_nocase(w.word, t)) {
					match = w.word;
					count++;
				}
			}
			if (!count) {
				p.error = Parsed::Invalid;
				p.at = start;
				return p;
			}
			if (count > 1) {
				p.error = Parsed::Ambiguous;
				p.word = t;
				return p;
			}
			p.words.push_back(match);
			if (const Setting *s = find(match))
				p.setting = s;
		}
		if (!node(p.words).cr)
			p.error = Parsed::Incomplete;
		return p;
	}

	/* the keys */
	void key(const Key &k) override {
		switch (k.code) {
		case Key::Char:
			if (k.ch == '?') {
				out("?\r\n");
				help();
				prompt();
			} else if (line_.size() < MAX_LINE) {
				line_ += k.ch;
				out(std::string(1, k.ch));
			}
			break;
		case Key::Backspace:
			if (!line_.empty()) {
				line_.pop_back();
				out("\b \b");
			}
			break;
		case Key::Tab:
			complete();
			break;
		case Key::Enter: {
			out("\r\n");
			std::string l = line_;
			line_.clear();
			run(l);
			if (!done())
				prompt();
			break;
		}
		case Key::Up:
			if (hist_at_ > 0)
				replace_line(history_[--hist_at_]);
			break;
		case Key::Down:
			if (hist_at_ < history_.size()) {
				hist_at_++;
				replace_line(hist_at_ < history_.size() ?
					     history_[hist_at_] : "");
			}
			break;
		case Key::Ctrl:
			ctrl(k.ch);
			break;
		default:
			break;
		}
	}
	/* ^C, ^Z (out of configuration mode), ^U (the line erased), ^W
	   (its last word), ^P and ^N (the history) */
	void ctrl(char c) {
		Key k;
		switch (c) {
		case 'c':
		case 'z':
			out(c == 'z' ? "^Z\r\n" : "^C\r\n");
			line_.clear();
			config_ = false;
			prompt();
			break;
		case 'u':
			replace_line("");
			break;
		case 'w': {
			std::string l = line_;
			while (!l.empty() && l.back() == ' ')
				l.pop_back();
			while (!l.empty() && l.back() != ' ')
				l.pop_back();
			replace_line(l);
			break;
		}
		case 'p':
			k.code = Key::Up;
			key(k);
			break;
		case 'n':
			k.code = Key::Down;
			key(k);
			break;
		default:
			break;
		}
	}
	/* Tab: the last word in full, when one word begins so */
	void complete() {
		size_t sp = line_.find_last_of(' ');
		std::string head = sp == std::string::npos ? "" :
			line_.substr(0, sp + 1);
		std::string part = line_.substr(head.size());
		if (part.empty())
			return;
		Parsed p = parse(head);
		if (p.error != Parsed::None && p.error != Parsed::Incomplete)
			return;
		Node n = node(p.words);
		std::vector<std::string> names;
		if (n.setting)
			for (const auto &c : n.setting->choices)
				names.push_back(c.name);
		else
			for (const auto &w : n.words)
				names.push_back(w.word);
		std::string one;
		s16 count = 0;
		for (const auto &w : names)
			if (starts_nocase(w, part)) {
				one = w;
				count++;
			}
		if (count == 1)
			replace_line(head + one + " ");
	}
	/* '?': the words that may come next (after a blank), or those
	   that begin as the last word does */
	void help() {
		const bool after_word = !line_.empty() && line_.back() != ' ';
		size_t sp = line_.find_last_of(' ');
		std::string head = after_word ? (sp == std::string::npos ? "" :
						 line_.substr(0, sp + 1)) : line_;
		std::string part = after_word ? line_.substr(head.size()) : "";
		Parsed p = parse(head);
		if (p.error == Parsed::Invalid || p.error == Parsed::Ambiguous) {
			out("% Unrecognized command\r\n");
			return;
		}
		Node n = node(p.words);
		std::vector<Word> list;
		if (n.setting) {
			const Setting &s = *n.setting;
			if (s.kind == Setting::Pick) {
				for (const auto &c : s.choices)
					list.push_back({ c.name, c.label });
			} else if (s.kind == Setting::Number) {
				list.push_back({ "<" + dec(s.lo) + "-" +
						 dec(s.hi) + ">", s.label });
			} else {
				list.push_back({ "LINE", s.help });
			}
		} else {
			list = n.words;
		}
		if (after_word) {
			std::string l;
			for (const auto &w : list)
				if (starts_nocase(w.word, part))
					l += w.word + "  ";
			out((l.empty() ? std::string("% Unrecognized command") :
			     l) + "\r\n\r\n");
			return;
		}
		for (const auto &w : list) {
			std::string word = "  " + w.word;
			if (word.size() < 24)
				word.resize(24, ' ');
			else
				word += "  ";
			out(word + w.help + "\r\n");
		}
		if (n.cr)
			out("  <cr>\r\n");
		out("\r\n");
	}

	/* a line typed */
	void run(const std::string &l) {
		std::string t = l;
		while (!t.empty() && t.back() == ' ')
			t.pop_back();
		size_t b = t.find_first_not_of(' ');
		if (b == std::string::npos)
			return;
		if (history_.empty() || history_.back() != t) {
			history_.push_back(t);
			if (history_.size() > HISTORY)
				history_.erase(history_.begin());
		}
		hist_at_ = history_.size();
		Parsed p = parse(t);
		switch (p.error) {
		case Parsed::Invalid:
			out(std::string(prompt_text().size() + p.at, ' ') +
			    "^\r\n% Invalid input detected at '^' marker."
			    "\r\n\r\n");
			return;
		case Parsed::Ambiguous:
			out("% Ambiguous command:  \"" + p.word + "\"\r\n");
			return;
		case Parsed::Incomplete:
			out("% Incomplete command.\r\n\r\n");
			return;
		case Parsed::None:
			break;
		}
		if (!config_) {
			exec(p.words, 0);
			return;
		}
		const std::string &first = p.words[0];
		if (first == "do") {
			exec(p.words, 1);
		} else if (first == "end" || first == "exit") {
			config_ = false;
		} else if (first == "help") {
			help_text();
		} else if (first == "no" || first == "default") {
			const Setting *s = find(p.words[1]);
			std::string v = defs_[index(s)];
			if (first == "no" && s->kind == Setting::Pick)
				for (const auto &c : s->choices)
					if (c.name == "off")
						v = c.value;
			set(*s, v);
		} else if (p.setting) {
			set(*p.setting, p.words[1]);
		}
	}
	/* s to v, at once */
	void set(const Setting &s, const std::string &v) {
		if (!idle()) {
			out("% The modem is busy\r\n");
			return;
		}
		if (apply(s, v))
			out("% The modem refused " + command(s, v) + "\r\n");
		/* (a country has its own S8 and S24) */
		defs_ = defaults(m_, model_);
	}
	/* an exec command (its words from from) */
	void exec(const std::vector<std::string> &w, size_t from) {
		const std::string &first = w[from];
		if (first == "configure") {
			out("Enter configuration commands, one per line.  "
			    "End with CNTL/Z.\r\n");
			config_ = true;
		} else if (first == "exit" || first == "logout") {
			if (from)
				config_ = false;
			else
				finish();
		} else if (first == "help") {
			help_text();
		} else if (first == "write") {
			out("% The modem keeps no stored profile: paste "
			    "'show init-string' into the next session.\r\n");
		} else if (first == "show") {
			const std::string &what = w[from + 1];
			if (what == "running-config")
				running();
			else if (what == "init-string")
				init_string();
			else if (what == "version")
				version();
			else
				settings(w.size() > from + 2 ? w[from + 2] : "");
		}
	}
	/* help */
	void help_text() {
		out("Help may be requested at any point in a command by "
		    "entering\r\n"
		    "a question mark '?'.  If nothing matches, the help list "
		    "will\r\n"
		    "be empty and you must backup until entering a '?' shows "
		    "the\r\n"
		    "available options.\r\n"
		    "Two styles of help are provided:\r\n"
		    "1. Full help is available when you are ready to enter a\r\n"
		    "   command argument (e.g. 'show ?') and describes each "
		    "possible\r\n"
		    "   argument.\r\n"
		    "2. Partial help is provided when an abbreviated argument "
		    "is entered\r\n"
		    "   and you want to know what arguments match the input\r\n"
		    "   (e.g. 'show r?'.)\r\n\r\n");
	}
	/* show running-config: what differs from the defaults */
	void running() {
		const std::vector<std::string> v = values(m_, model_);
		out("Building configuration...\r\n\r\nCurrent configuration:"
		    "\r\n!\r\n");
		for (size_t i = 0; i < model_.size(); i++)
			if (v[i] != defs_[i])
				out(model_[i].key + " " + named(model_[i], v[i]) +
				    "\r\n");
		out("!\r\nend\r\n\r\n");
	}
	/* show init-string */
	void init_string() {
		std::vector<std::string> lines = init_lines(
			model_, values(m_, model_), defs_, 63);
		out("! Paste after ATZ in the next session:\r\n");
		if (lines.empty())
			lines.push_back("AT");
		for (const auto &l : lines)
			out(l + "\r\n");
		out("\r\n");
	}
	/* show version */
	void version() {
		const std::vector<std::string> v = values(m_, model_);
		size_t changed = 0;
		for (size_t i = 0; i < v.size(); i++)
			changed += v[i] != defs_[i];
		out(product() + "\r\n" + dec(model_.size()) + " settings, " +
		    dec(changed) + " changed from the defaults\r\n\r\n");
	}
	/* show settings [group]: each with its value, '*' where it is not
	   the default */
	void settings(const std::string &group) {
		const std::vector<std::string> v = values(m_, model_);
		for (const auto &g : groups_) {
			if (!group.empty() && token(g) != group)
				continue;
			out(g + "\r\n");
			for (size_t i = 0; i < model_.size(); i++) {
				const Setting &s = model_[i];
				if (s.group != g)
					continue;
				std::string l = (v[i] != defs_[i] ? " * " :
						 "   ") + s.key;
				l.resize(std::max<size_t>(l.size() + 1, 24), ' ');
				std::string val = named(s, v[i]);
				l += val;
				l.resize(std::max<size_t>(l.size() + 1, 44), ' ');
				l += s.label;
				out(l + "\r\n");
			}
		}
		out("\r\n");
	}
	/* the token the command line gives a group */
	static std::string token(const std::string &g) {
		std::string t;
		for (char c : g)
			t += c == ' ' ? '-' : static_cast<char>(
				c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		return t;
	}
};

} // namespace

/* AT!CT's configurator */
Config *make_cli(Modem &m)
{
	return new Cli(m);
}

} // namespace dcore
