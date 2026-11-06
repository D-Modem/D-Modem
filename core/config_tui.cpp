/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    config_tui.cpp - AT!CF: the configurator as a full screen in the
 *                     style of Borland's Turbo Vision
 *
 *    A menu bar (F10, Alt and a letter), a blue window on the desktop
 *    with the groups on its left and their settings on its right, gray
 *    dialogs with shadows for a value, and a status line of the keys.
 *    Changes wait until F2 (or the question on leaving) saves them,
 *    through each setting's AT command.
 *
 *    The screen is drawn into a frame of cells, and only the cells that
 *    changed since the last frame go to the terminal (a modem's DTE may
 *    be slow), in ANSI's colours, its box drawing characters in UTF-8 or
 *    CP437 (what AT!C found the terminal takes).
 *
 */

#include <algorithm>

#include "dcore.h"
#include "config.h"
#include "str.h"

namespace dcore {

namespace {

/* colours: CGA's numbers, background << 4 | foreground */
const u8
	MENU = 0x70, MENU_HOT = 0x74, MENU_SEL = 0x20, MENU_SEL_HOT = 0x24,
	DESK = 0x71,
	WIN = 0x1e, WIN_FRAME = 0x1f, WIN_TITLE = 0x1f, WIN_DIM = 0x17,
	WIN_SEL = 0x30, WIN_SEL_DIM = 0x70, WIN_CHANGED = 0x1b,
	WIN_SEL_CHANGED = 0x3f, WIN_HELP = 0x17, SCROLL = 0x31,
	DLG = 0x70, DLG_FRAME = 0x7f, DLG_HOT = 0x7e,
	LIST = 0x30, LIST_SEL = 0x2f,
	INPUT = 0x1f,
	BUTTON = 0x20, BUTTON_FOCUS = 0x2f, BUTTON_HOT = 0x2e,
	SHADOW = 0x08,
	STATUS = 0x70, STATUS_HOT = 0x74,
	ERROR_DLG = 0x4f;

/* the glyphs drawn, in Unicode */
enum : u32 {
	H2 = 0x2550, V2 = 0x2551, TL2 = 0x2554, TR2 = 0x2557, BL2 = 0x255a,
	BR2 = 0x255d, H1 = 0x2500, V1 = 0x2502, TL1 = 0x250c, TR1 = 0x2510,
	BL1 = 0x2514, BR1 = 0x2518, LT1 = 0x251c, RT1 = 0x2524,
	SHADE1 = 0x2591, SHADE2 = 0x2592, BLOCK = 0x25a0, UP = 0x25b2,
	DOWN = 0x25bc, LOWER = 0x2584, UPPER = 0x2580, MENUSIGN = 0x2261,
	BULLET = 0x2219, LT21 = 0x255f, RT21 = 0x2562, BT1 = 0x2534,
	TT21 = 0x2564, RIGHT = 0x25ba, LEFT = 0x25c4,
};

/* a glyph's code in CP437 (0 for none: '?') */
u8 cp437(u32 c)
{
	static const struct {
		u32 u;
		u8 c;
	} map[] = {
		{ H2, 0xcd }, { V2, 0xba }, { TL2, 0xc9 }, { TR2, 0xbb },
		{ BL2, 0xc8 }, { BR2, 0xbc }, { H1, 0xc4 }, { V1, 0xb3 },
		{ TL1, 0xda }, { TR1, 0xbf }, { BL1, 0xc0 }, { BR1, 0xd9 },
		{ LT1, 0xc3 }, { RT1, 0xb4 }, { SHADE1, 0xb0 },
		{ SHADE2, 0xb1 }, { BLOCK, 0xfe }, { UP, '^' }, { DOWN, 'v' },
		{ LOWER, 0xdc }, { UPPER, 0xdf }, { MENUSIGN, 0xf0 },
		{ BULLET, 0xf9 }, { LT21, 0xc7 }, { RT21, 0xb6 }, { BT1, 0xc1 },
		{ TT21, 0xd1 }, { RIGHT, '>' }, { LEFT, '<' },
	};
	for (const auto &m : map)
		if (m.u == c)
			return m.c;
	return 0;
}

/* c in UTF-8 */
std::string utf8(u32 c)
{
	std::string s;
	if (c < 0x80) {
		s += static_cast<char>(c);
	} else if (c < 0x800) {
		s += static_cast<char>(0xc0 | (c >> 6));
		s += static_cast<char>(0x80 | (c & 0x3f));
	} else {
		s += static_cast<char>(0xe0 | (c >> 12));
		s += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
		s += static_cast<char>(0x80 | (c & 0x3f));
	}
	return s;
}

/* a cell of the screen */
struct Cell {
	u32 ch = ' ';
	u8 attr = 0x07;
	/* the same glyph in the same colours */
	bool operator==(const Cell &o) const {
		return ch == o.ch && attr == o.attr;
	}
};

/* the screen: what is drawn, and what the terminal has */
class Screen {
public:
	/* w columns, h rows */
	Screen(s16 w, s16 h, bool u) : w_(w), h_(h), utf8_(u),
		cells_(static_cast<size_t>(w * h)),
		shown_(static_cast<size_t>(w * h)) {}
	/* its size */
	s16 w() const { return w_; }
	/* (see w()) */
	s16 h() const { return h_; }
	/* the terminal to be drawn again in full */
	void invalidate() { fresh_ = true; }
	/* c at x, y (off the screen: nothing) */
	void put(s16 x, s16 y, u32 c, u8 a) {
		if (x < 0 || y < 0 || x >= w_ || y >= h_)
			return;
		cells_[static_cast<size_t>(y * w_ + x)] = { c, a };
	}
	/* a rectangle of c */
	void fill(s16 x, s16 y, s16 w, s16 h, u32 c, u8 a) {
		for (s16 j = 0; j < h; j++)
			for (s16 i = 0; i < w; i++)
				put(static_cast<s16>(x + i),
				    static_cast<s16>(y + j), c, a);
	}
	/* text at x, y, at most max columns (0: all); '~' around a hot
	   letter in hot's colour, when hot is not 0; the columns taken */
	s16 text(s16 x, s16 y, const std::string &s, u8 a, s16 max = 0,
		 u8 hot = 0) {
		s16 n = 0;
		bool in_hot = false;
		for (char c : s) {
			if (hot && c == '~') {
				in_hot = !in_hot;
				continue;
			}
			if (max && n >= max)
				break;
			put(static_cast<s16>(x + n), y,
			    static_cast<unsigned char>(c), in_hot ? hot : a);
			n++;
		}
		return n;
	}
	/* a frame, double or single lined */
	void frame(s16 x, s16 y, s16 w, s16 h, u8 a, bool dbl) {
		const u32 hz = dbl ? H2 : H1, vt = dbl ? V2 : V1;
		for (s16 i = 1; i < w - 1; i++) {
			put(static_cast<s16>(x + i), y, hz, a);
			put(static_cast<s16>(x + i),
			    static_cast<s16>(y + h - 1), hz, a);
		}
		for (s16 j = 1; j < h - 1; j++) {
			put(x, static_cast<s16>(y + j), vt, a);
			put(static_cast<s16>(x + w - 1),
			    static_cast<s16>(y + j), vt, a);
		}
		put(x, y, dbl ? TL2 : TL1, a);
		put(static_cast<s16>(x + w - 1), y, dbl ? TR2 : TR1, a);
		put(x, static_cast<s16>(y + h - 1), dbl ? BL2 : BL1, a);
		put(static_cast<s16>(x + w - 1), static_cast<s16>(y + h - 1),
		    dbl ? BR2 : BR1, a);
	}
	/* the shadow of a w by h box at x, y: two columns on its right,
	   a row below it, what is there darkened */
	void shadow(s16 x, s16 y, s16 w, s16 h) {
		auto dark = [this](s16 i, s16 j) {
			if (i < 0 || j < 0 || i >= w_ || j >= h_)
				return;
			cells_[static_cast<size_t>(j * w_ + i)].attr = SHADOW;
		};
		for (s16 j = 1; j <= h; j++) {
			dark(static_cast<s16>(x + w), static_cast<s16>(y + j));
			dark(static_cast<s16>(x + w + 1),
			     static_cast<s16>(y + j));
		}
		for (s16 i = 2; i < w + 2; i++)
			dark(static_cast<s16>(x + i), static_cast<s16>(y + h));
	}
	/* what changed since the last frame, for the terminal; the cursor
	   at cx, cy shown (cx < 0: hidden) */
	std::string render(s16 cx, s16 cy) {
		static const char ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
		std::string o;
		s16 at_x = -1, at_y = -1;
		s16 attr = -1;
		if (fresh_)
			o += "\x1b[0m\x1b[?25l\x1b[2J";
		for (s16 y = 0; y < h_; y++) {
			for (s16 x = 0; x < w_; x++) {
				/* (not the last cell: the screen would
				   scroll on some terminals) */
				if (x == w_ - 1 && y == h_ - 1)
					continue;
				size_t i = static_cast<size_t>(y * w_ + x);
				const Cell &c = cells_[i];
				if (!fresh_ && c == shown_[i])
					continue;
				if (at_x != x || at_y != y)
					o += "\x1b[" + dec(y + 1) + ";" +
						dec(x + 1) + "H";
				if (attr != c.attr) {
					u8 fg = c.attr & 0xf, bg = (c.attr >> 4) & 7;
					o += std::string("\x1b[0;") +
						(fg & 8 ? "1;" : "") + "3" +
						static_cast<char>('0' + ansi[fg & 7]) +
						";4" +
						static_cast<char>('0' + ansi[bg]) +
						"m";
					attr = c.attr;
				}
				o += glyph(c.ch);
				shown_[i] = c;
				at_x = static_cast<s16>(x + 1);
				at_y = y;
			}
		}
		fresh_ = false;
		if (cx >= 0) {
			o += "\x1b[" + dec(cy + 1) + ";" + dec(cx + 1) +
				"H\x1b[?25h";
			cursor_ = true;
		} else if (cursor_) {
			o += "\x1b[?25l";
			cursor_ = false;
		}
		return o;
	}

private:
	s16 w_, h_;
	bool utf8_;
	std::vector<Cell> cells_, shown_;
	bool fresh_ = true;
	bool cursor_ = false;
	/* a glyph as the terminal takes it */
	std::string glyph(u32 c) const {
		if (c < 0x80)
			return std::string(1, static_cast<char>(c));
		if (utf8_)
			return utf8(c);
		u8 b = cp437(c);
		return std::string(1, b ? static_cast<char>(b) : '?');
	}
};

/* a menu's item: its label ('~' around its hot letter), its key, what
   it does */
struct Item {
	std::string label, keys;
	s16 action;     /* < 0: a group, -1 - its index */
};

enum Action {
	A_NONE, A_ABOUT, A_SAVE, A_INIT, A_DEFAULTS, A_EXIT, A_KEYS,
	A_RESTORE,
};

/* the full-screen configurator */
class Tui final : public Config {
public:
	/* on m, a terminal of rows by cols (in UTF-8 or CP437) */
	Tui(Modem &m, s16 rows, s16 cols, bool u)
		: Config(m),
		  scr_(std::min<s16>(std::max<s16>(cols, 80), 160),
		       std::min<s16>(std::max<s16>(rows, 24), 66), u) {
		model_ = model(m);
		defs_ = defaults(m, model_);
		cur_ = values(m, model_);
		pend_ = cur_;
		for (const auto &s : model_)
			if (std::find(groups_.begin(), groups_.end(), s.group) ==
			    groups_.end())
				groups_.push_back(s.group);
		sel_.assign(groups_.size(), 0);
		top_.assign(groups_.size(), 0);
	}
	/* the first frame */
	void start() override {
		out("\x1b[0m\x1b[?25l\x1b[2J");
		draw();
	}
	/* the terminal as it was: colours, cursor, a clear screen */
	void leave() override {
		out("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
	}

private:
	Screen scr_;
	std::vector<Setting> model_;
	std::vector<std::string> defs_, cur_, pend_, groups_;
	std::vector<s16> sel_, top_;
	size_t group_ = 0;
	bool on_settings_ = true;       /* the focus: settings, or groups */

	/* the dialog open, if any */
	enum class Dlg { None, Menu, Pick, Input, Message, Confirm };
	Dlg dlg_ = Dlg::None;
	/* the menu: which (-1 none), its item */
	s16 menu_ = -1, menu_item_ = 0;
	/* the dialog's setting, its list (Pick) and text (Input), the
	   focus in it (0 the list or line, 1.. its buttons) */
	size_t edit_ = 0;
	s16 pick_ = 0, pick_top_ = 0, focus_ = 0;
	std::string input_;
	/* a message box's title, lines, colours, and what Confirm's Yes
	   does */
	std::string msg_title_;
	std::vector<std::string> msg_;
	bool msg_error_ = false;

	/* the settings of group g, by their index */
	std::vector<size_t> items(size_t g) const {
		std::vector<size_t> v;
		for (size_t i = 0; i < model_.size(); i++)
			if (model_[i].group == groups_[g])
				v.push_back(i);
		return v;
	}
	/* the setting under the bar */
	size_t current() const {
		std::vector<size_t> v = items(group_);
		return v[static_cast<size_t>(sel_[group_])];
	}
	/* there are changes not saved */
	bool dirty() const { return pend_ != cur_; }

	/* the menu bar's menus */
	std::vector<std::vector<Item>> menus() const {
		std::vector<std::vector<Item>> m(4);
		m[0] = { { "~A~bout...", "", A_ABOUT } };
		m[1] = { { "~S~ave", "F2", A_SAVE },
			 { "~I~nit string...", "F3", A_INIT },
			 { "~D~efaults", "F5", A_DEFAULTS },
			 { "~U~ndo changes", "", A_RESTORE },
			 { "", "", A_NONE },
			 { "E~x~it", "Alt-X", A_EXIT } };
		for (size_t g = 0; g < groups_.size(); g++)
			m[2].push_back({ "~" + groups_[g].substr(0, 1) + "~" +
					 groups_[g].substr(1), "",
					 static_cast<s16>(-1 - static_cast<s16>(g)) });
		m[3] = { { "~K~eys...", "F1", A_KEYS },
			 { "~A~bout...", "", A_ABOUT } };
		return m;
	}
	/* the menu bar's titles */
	static std::vector<std::string> menu_titles() {
		return { std::string(" ") + utf8(MENUSIGN) + " ", " ~F~ile ",
			 " ~G~roups ", " ~H~elp " };
	}
	/* where menu i's title begins */
	static s16 menu_x(s16 i) {
		static const s16 x[4] = { 1, 4, 10, 18 };
		return x[i];
	}

	/*
	 *    drawing
	 */

	/* the whole frame, then what changed to the terminal */
	void draw() {
		const s16 W = scr_.w(), H = scr_.h();
		scr_.fill(0, 0, W, H, SHADE1, DESK);
		menubar();
		status();
		window();
		s16 cx = -1, cy = -1;
		switch (dlg_) {
		case Dlg::Menu:
			menu();
			break;
		case Dlg::Pick:
			pick_dialog();
			break;
		case Dlg::Input:
			input_dialog(&cx, &cy);
			break;
		case Dlg::Message:
		case Dlg::Confirm:
			message();
			break;
		case Dlg::None:
			break;
		}
		out(scr_.render(cx, cy));
	}
	/* the menu bar */
	void menubar() {
		scr_.fill(0, 0, scr_.w(), 1, ' ', MENU);
		std::vector<std::string> t = menu_titles();
		for (s16 i = 0; i < 4; i++) {
			bool sel = dlg_ == Dlg::Menu && menu_ == i;
			if (i == 0) {
				scr_.fill(menu_x(0), 0, 3, 1, ' ',
					  sel ? MENU_SEL : MENU);
				scr_.put(static_cast<s16>(menu_x(0) + 1), 0,
					 MENUSIGN, sel ? MENU_SEL : MENU);
				continue;
			}
			scr_.text(menu_x(i), 0, t[i], sel ? MENU_SEL : MENU, 0,
				  sel ? MENU_SEL_HOT : MENU_HOT);
		}
		std::string p = product();
		scr_.text(static_cast<s16>(scr_.w() - 1 -
					   static_cast<s16>(p.size())), 0, p,
			  MENU);
	}
	/* the status line: the keys */
	void status() {
		const s16 y = static_cast<s16>(scr_.h() - 1);
		scr_.fill(0, y, scr_.w(), 1, ' ', STATUS);
		std::string s = dlg_ == Dlg::None ?
			" ~F1~ Help  ~F2~ Save  ~F3~ Init string  ~F5~ Defaults"
			"  ~F10~ Menu  ~Alt-X~ Exit" :
			" ~Enter~ OK  ~Esc~ Cancel  ~Tab~ Next";
		scr_.text(0, y, s, STATUS, static_cast<s16>(scr_.w() - 1),
			  STATUS_HOT);
	}
	/* the main window: the groups, the settings, the help lines */
	void window() {
		const s16 x = 2, y = 2;
		const s16 w = static_cast<s16>(scr_.w() - 6);
		const s16 h = static_cast<s16>(scr_.h() - 5);
		const bool active = dlg_ == Dlg::None;
		scr_.fill(x, y, w, h, ' ', WIN);
		scr_.frame(x, y, w, h, active ? WIN_FRAME : WIN_DIM, active);
		scr_.shadow(x, y, w, h);
		/* the close box, the title, the window's number */
		scr_.text(static_cast<s16>(x + 2), y, "[ ]",
			  active ? WIN_FRAME : WIN_DIM);
		scr_.put(static_cast<s16>(x + 3), y, BLOCK, active ? 0x1a : WIN_DIM);
		const std::string title = " D-Modem Setup ";
		scr_.text(static_cast<s16>(x + (w - static_cast<s16>(
							title.size())) / 2), y,
			  title, WIN_TITLE);
		scr_.text(static_cast<s16>(x + w - 5), y, " 1 ", WIN_FRAME);

		const s16 gw = 22;      /* the groups' column */
		const s16 top = static_cast<s16>(y + 1);
		const s16 rows = static_cast<s16>(h - 5);
		/* the line between the panes and above the help */
		for (s16 j = 0; j < rows; j++)
			scr_.put(static_cast<s16>(x + gw), static_cast<s16>(top + j),
				 V1, WIN_FRAME);
		const s16 hy = static_cast<s16>(top + rows);
		for (s16 i = 1; i < w - 1; i++)
			scr_.put(static_cast<s16>(x + i), hy, H1, WIN_FRAME);
		scr_.put(x, hy, LT21, WIN_FRAME);
		scr_.put(static_cast<s16>(x + w - 1), hy, RT21, WIN_FRAME);
		scr_.put(static_cast<s16>(x + gw), hy, BT1, WIN_FRAME);
		scr_.put(static_cast<s16>(x + gw), y, TT21, active ?
			 WIN_FRAME : WIN_DIM);

		/* the groups */
		for (size_t g = 0; g < groups_.size() &&
			     static_cast<s16>(g) < rows; g++) {
			const bool sel = g == group_;
			u8 a = sel ? (on_settings_ ? WIN_SEL_DIM : WIN_SEL) : WIN;
			const s16 gy = static_cast<s16>(top + static_cast<s16>(g));
			scr_.fill(static_cast<s16>(x + 1), gy,
				  static_cast<s16>(gw - 1), 1, ' ', a);
			std::vector<size_t> it = items(g);
			bool changed = false;
			for (size_t i : it)
				changed |= pend_[i] != defs_[i];
			scr_.text(static_cast<s16>(x + 2), gy, groups_[g], a,
				  static_cast<s16>(gw - 4));
			if (changed)
				scr_.put(static_cast<s16>(x + gw - 2), gy,
					 BULLET, a);
		}

		/* the settings of the group */
		std::vector<size_t> it = items(group_);
		s16 &sel = sel_[group_], &first = top_[group_];
		if (sel >= static_cast<s16>(it.size()))
			sel = static_cast<s16>(it.size() - 1);
		if (sel < first)
			first = sel;
		if (sel >= first + rows)
			first = static_cast<s16>(sel - rows + 1);
		const s16 sx = static_cast<s16>(x + gw + 1);
		const s16 sw = static_cast<s16>(w - gw - 2);
		const s16 lw = std::min<s16>(30, static_cast<s16>(sw / 2));
		for (s16 r = 0; r < rows; r++) {
			size_t k = static_cast<size_t>(first + r);
			if (k >= it.size())
				break;
			size_t i = it[k];
			const bool is_sel = static_cast<s16>(k) == sel;
			const bool changed = pend_[i] != defs_[i];
			const bool unsaved = pend_[i] != cur_[i];
			u8 a = is_sel ? (on_settings_ ? WIN_SEL : WIN_SEL_DIM) : WIN;
			u8 va = unsaved ? (is_sel && on_settings_ ?
					   WIN_SEL_CHANGED : WIN_CHANGED) : a;
			const s16 ry = static_cast<s16>(top + r);
			scr_.fill(sx, ry, sw, 1, ' ', a);
			if (changed)
				scr_.put(sx, ry, BULLET, a);
			scr_.text(static_cast<s16>(sx + 1), ry, model_[i].label,
				  a, static_cast<s16>(lw - 1));
			scr_.text(static_cast<s16>(sx + lw + 1), ry,
				  shown(model_[i], pend_[i]), va,
				  static_cast<s16>(sw - lw - 2));
		}
		/* the scroll bar, on the frame */
		if (static_cast<s16>(it.size()) > rows) {
			const s16 bx = static_cast<s16>(x + w - 1);
			scr_.put(bx, top, UP, SCROLL);
			scr_.put(bx, static_cast<s16>(top + rows - 1), DOWN,
				 SCROLL);
			for (s16 j = 1; j < rows - 1; j++)
				scr_.put(bx, static_cast<s16>(top + j), SHADE1,
					 SCROLL);
			s16 span = static_cast<s16>(rows - 3);
			s16 at = static_cast<s16>(
				1 + (span * sel) /
				std::max<s16>(1, static_cast<s16>(it.size() - 1)));
			scr_.put(bx, static_cast<s16>(top + at), BLOCK, SCROLL);
		}
		/* the help for the setting under the bar */
		const Setting &s = model_[current()];
		std::string l1 = s.label + " (AT" + command(s, pend_[current()]) +
			")";
		std::string l2 = s.help;
		if (pend_[current()] != defs_[current()])
			l2 += "  [default: " + shown(s, defs_[current()]) + "]";
		scr_.text(static_cast<s16>(x + 2), static_cast<s16>(hy + 1), l1,
			  WIN, static_cast<s16>(w - 4));
		scr_.text(static_cast<s16>(x + 2), static_cast<s16>(hy + 2), l2,
			  WIN_HELP, static_cast<s16>(w - 4));
	}
	/* a gray dialog of w by h in the middle, titled; its corner */
	void dialog(s16 w, s16 h, const std::string &title, s16 *x, s16 *y,
		    bool error = false) {
		*x = static_cast<s16>((scr_.w() - w) / 2);
		*y = static_cast<s16>((scr_.h() - h) / 2);
		const u8 bg = error ? ERROR_DLG : DLG;
		const u8 fr = error ? ERROR_DLG : DLG_FRAME;
		scr_.fill(*x, *y, w, h, ' ', bg);
		scr_.frame(*x, *y, w, h, fr, true);
		scr_.shadow(*x, *y, w, h);
		scr_.text(static_cast<s16>(*x + 2), *y, "[ ]", fr);
		scr_.put(static_cast<s16>(*x + 3), *y, BLOCK, error ? 0x4a : 0x7a);
		std::string t = " " + title + " ";
		scr_.text(static_cast<s16>(*x + (w - static_cast<s16>(t.size())) /
					   2), *y, t, fr,
			  static_cast<s16>(w - 8));
	}
	/* a button at x, y: its label ('~' around the hot letter), focused
	   or not; with its shadow */
	void button(s16 x, s16 y, const std::string &label, bool focus,
		    u8 bg = DLG) {
		std::string t = "  " + label + "  ";
		s16 n = scr_.text(x, y, t, focus ? BUTTON_FOCUS : BUTTON, 0,
				  BUTTON_HOT);
		if (focus) {
			scr_.put(x, y, RIGHT, BUTTON_FOCUS);
			scr_.put(static_cast<s16>(x + n - 1), y, LEFT,
				 BUTTON_FOCUS);
		}
		const u8 sh = static_cast<u8>((bg & 0xf0) | 0x0);
		scr_.put(static_cast<s16>(x + n), y, LOWER, sh);
		for (s16 i = 1; i <= n; i++)
			scr_.put(static_cast<s16>(x + i),
				 static_cast<s16>(y + 1), UPPER, sh);
	}
	/* the menu that is open */
	void menu() {
		std::vector<std::vector<Item>> all = menus();
		const std::vector<Item> &m = all[static_cast<size_t>(menu_)];
		s16 w = 0;
		for (const auto &it : m) {
			s16 n = static_cast<s16>(it.label.size() + it.keys.size() + 6);
			w = std::max(w, n);
		}
		const s16 h = static_cast<s16>(m.size() + 2);
		const s16 x = std::min<s16>(static_cast<s16>(menu_x(menu_) - 1),
					    static_cast<s16>(scr_.w() - w - 2));
		const s16 y = 1;
		scr_.fill(x, y, w, h, ' ', MENU);
		scr_.frame(x, y, w, h, MENU, false);
		scr_.shadow(x, y, w, h);
		for (size_t i = 0; i < m.size(); i++) {
			const s16 iy = static_cast<s16>(y + 1 + static_cast<s16>(i));
			if (m[i].label.empty()) {
				for (s16 j = 1; j < w - 1; j++)
					scr_.put(static_cast<s16>(x + j), iy, H1,
						 MENU);
				scr_.put(x, iy, LT1, MENU);
				scr_.put(static_cast<s16>(x + w - 1), iy, RT1,
					 MENU);
				continue;
			}
			const bool sel = static_cast<s16>(i) == menu_item_;
			const u8 a = sel ? MENU_SEL : MENU;
			scr_.fill(static_cast<s16>(x + 1), iy,
				  static_cast<s16>(w - 2), 1, ' ', a);
			scr_.text(static_cast<s16>(x + 2), iy, m[i].label, a, 0,
				  sel ? MENU_SEL_HOT : MENU_HOT);
			scr_.text(static_cast<s16>(x + w - 2 -
						   static_cast<s16>(m[i].keys.size())),
				  iy, m[i].keys, a);
		}
	}
	/* the dialog of a setting's choices */
	void pick_dialog() {
		const Setting &s = model_[edit_];
		s16 lw = static_cast<s16>(s.label.size() + 4);
		for (const auto &c : s.choices)
			lw = std::max<s16>(lw, static_cast<s16>(c.label.size()));
		lw = std::min<s16>(static_cast<s16>(lw + 2),
				   static_cast<s16>(scr_.w() - 12));
		const s16 rows = std::min<s16>(
			static_cast<s16>(s.choices.size()),
			static_cast<s16>(scr_.h() - 11));
		const s16 w = std::max<s16>(static_cast<s16>(lw + 6), 30);
		const s16 h = static_cast<s16>(rows + 7);
		s16 x, y;
		dialog(w, h, s.label, &x, &y);
		scr_.text(static_cast<s16>(x + 3), static_cast<s16>(y + 1),
			  "~V~alues", focus_ == 0 ? DLG_HOT : DLG, 0, DLG_HOT);
		if (pick_ < pick_top_)
			pick_top_ = pick_;
		if (pick_ >= pick_top_ + rows)
			pick_top_ = static_cast<s16>(pick_ - rows + 1);
		const s16 lx = static_cast<s16>(x + 3);
		const s16 ly = static_cast<s16>(y + 2);
		for (s16 r = 0; r < rows; r++) {
			size_t k = static_cast<size_t>(pick_top_ + r);
			const bool sel = static_cast<s16>(k) == pick_;
			u8 a = sel ? (focus_ == 0 ? LIST_SEL : 0x3e) : LIST;
			scr_.fill(lx, static_cast<s16>(ly + r), lw, 1, ' ', a);
			if (k < s.choices.size()) {
				std::string t = s.choices[k].label;
				if (s.choices[k].value == defs_[edit_])
					t += " *";
				scr_.text(static_cast<s16>(lx + 1),
					  static_cast<s16>(ly + r), t, a,
					  static_cast<s16>(lw - 1));
			}
		}
		if (static_cast<s16>(s.choices.size()) > rows) {
			const s16 bx = static_cast<s16>(lx + lw);
			for (s16 r = 0; r < rows; r++)
				scr_.put(bx, static_cast<s16>(ly + r), SHADE1, 0x13);
			scr_.put(bx, ly, UP, 0x13);
			scr_.put(bx, static_cast<s16>(ly + rows - 1), DOWN, 0x13);
			s16 at = static_cast<s16>(
				1 + ((rows - 3) * pick_) /
				std::max<s16>(1, static_cast<s16>(
						      s.choices.size() - 1)));
			scr_.put(bx, static_cast<s16>(ly + at), BLOCK, 0x13);
		}
		scr_.text(static_cast<s16>(x + 3),
			  static_cast<s16>(y + h - 4), "* the default", DLG);
		const s16 by = static_cast<s16>(y + h - 3);
		button(static_cast<s16>(x + w / 2 - 13), by, "O~K~", focus_ == 1);
		button(static_cast<s16>(x + w / 2 + 1), by, "~C~ancel",
		       focus_ == 2);
	}
	/* the dialog of a value typed; the cursor in its line */
	void input_dialog(s16 *cx, s16 *cy) {
		const Setting &s = model_[edit_];
		const s16 w = std::min<s16>(60, static_cast<s16>(scr_.w() - 4));
		const s16 h = 10;
		s16 x, y;
		dialog(w, h, s.label, &x, &y);
		std::string what = s.kind == Setting::Number ?
			"~V~alue (" + dec(s.lo) + "-" + dec(s.hi) + ")" :
			"~V~alues (numbers, commas between)";
		scr_.text(static_cast<s16>(x + 3), static_cast<s16>(y + 2), what,
			  focus_ == 0 ? DLG_HOT : DLG, 0, DLG_HOT);
		const s16 iw = static_cast<s16>(w - 8);
		const s16 ix = static_cast<s16>(x + 3), iy = static_cast<s16>(y + 3);
		scr_.fill(ix, iy, static_cast<s16>(iw + 2), 1, ' ', INPUT);
		std::string t = input_;
		if (static_cast<s16>(t.size()) > iw - 1)
			t = t.substr(t.size() - static_cast<size_t>(iw - 1));
		scr_.text(static_cast<s16>(ix + 1), iy, t, INPUT);
		scr_.text(static_cast<s16>(x + 3), static_cast<s16>(y + 5),
			  "Default: " + defs_[edit_] + "   " + s.help, DLG,
			  static_cast<s16>(w - 6));
		const s16 by = static_cast<s16>(y + h - 3);
		button(static_cast<s16>(x + w / 2 - 13), by, "O~K~", focus_ == 1);
		button(static_cast<s16>(x + w / 2 + 1), by, "~C~ancel",
		       focus_ == 2);
		if (focus_ == 0) {
			*cx = static_cast<s16>(ix + 1 + static_cast<s16>(t.size()));
			*cy = iy;
		}
	}
	/* a message box, or a question (Yes, No, Cancel) */
	void message() {
		s16 w = static_cast<s16>(msg_title_.size() + 10);
		for (const auto &l : msg_)
			w = std::max<s16>(w, static_cast<s16>(l.size() + 6));
		w = std::max<s16>(w, dlg_ == Dlg::Confirm ? 44 : 24);
		w = std::min<s16>(w, static_cast<s16>(scr_.w() - 4));
		const s16 h = static_cast<s16>(msg_.size() + 6);
		s16 x, y;
		dialog(w, h, msg_title_, &x, &y, msg_error_);
		const u8 a = msg_error_ ? ERROR_DLG : DLG;
		for (size_t i = 0; i < msg_.size(); i++)
			scr_.text(static_cast<s16>(x + 3),
				  static_cast<s16>(y + 2 + static_cast<s16>(i)),
				  msg_[i], a, static_cast<s16>(w - 6));
		const s16 by = static_cast<s16>(y + h - 3);
		if (dlg_ == Dlg::Confirm) {
			button(static_cast<s16>(x + w / 2 - 19), by, "~Y~es",
			       focus_ == 0, a);
			button(static_cast<s16>(x + w / 2 - 6), by, "~N~o",
			       focus_ == 1, a);
			button(static_cast<s16>(x + w / 2 + 6), by, "~C~ancel",
			       focus_ == 2, a);
		} else {
			button(static_cast<s16>(x + w / 2 - 4), by, "O~K~", true,
			       a);
		}
	}

	/*
	 *    keys
	 */

	/* a key, to the dialog that is open or to the window */
	void key(const Key &k) override {
		if (k.code == Key::Ctrl && k.ch == 'l') {
			scr_.invalidate();
		} else {
			switch (dlg_) {
			case Dlg::None: window_key(k); break;
			case Dlg::Menu: menu_key(k); break;
			case Dlg::Pick: pick_key(k); break;
			case Dlg::Input: input_key(k); break;
			case Dlg::Message: message_key(k); break;
			case Dlg::Confirm: confirm_key(k); break;
			}
		}
		if (!done())
			draw();
	}
	/* the bar down (d > 0) or up through n rows; *at kept in them */
	static void move(s16 *at, s16 d, s16 n) {
		s16 v = static_cast<s16>(*at + d);
		*at = std::max<s16>(0, std::min<s16>(v, static_cast<s16>(n - 1)));
	}
	/* the window's keys */
	void window_key(const Key &k) {
		std::vector<size_t> it = items(group_);
		s16 &sel = sel_[group_];
		const s16 page = static_cast<s16>(scr_.h() - 11);
		s16 g = static_cast<s16>(group_);
		switch (k.code) {
		case Key::Tab:
		case Key::BackTab:
			on_settings_ = !on_settings_;
			break;
		case Key::Up:
			if (on_settings_)
				move(&sel, -1, static_cast<s16>(it.size()));
			else
				move(&g, -1, static_cast<s16>(groups_.size()));
			break;
		case Key::Down:
			if (on_settings_)
				move(&sel, 1, static_cast<s16>(it.size()));
			else
				move(&g, 1, static_cast<s16>(groups_.size()));
			break;
		case Key::PgUp:
			move(&sel, static_cast<s16>(-page),
			     static_cast<s16>(it.size()));
			break;
		case Key::PgDn:
			move(&sel, page, static_cast<s16>(it.size()));
			break;
		case Key::Home:
			sel = 0;
			break;
		case Key::End:
			sel = static_cast<s16>(it.size() - 1);
			break;
		case Key::Left:
			if (on_settings_ && model_[current()].kind == Setting::Pick)
				cycle(-1);
			else
				on_settings_ = false;
			break;
		case Key::Right:
			if (on_settings_ && model_[current()].kind == Setting::Pick)
				cycle(1);
			else
				on_settings_ = true;
			break;
		case Key::Char:
			if (k.ch == ' ' && on_settings_)
				cycle(1);
			else if ((k.ch == '+' || k.ch == '-') && on_settings_)
				step(k.ch == '+' ? 1 : -1);
			break;
		case Key::Enter:
			if (on_settings_)
				edit();
			else
				on_settings_ = true;
			break;
		case Key::Del:
		case Key::Backspace:
			if (on_settings_)
				pend_[current()] = defs_[current()];
			break;
		case Key::F1:
			act(A_KEYS);
			break;
		case Key::F2:
			act(A_SAVE);
			break;
		case Key::F3:
			act(A_INIT);
			break;
		case Key::F5:
			act(A_DEFAULTS);
			break;
		case Key::F10:
			open_menu(1);
			break;
		case Key::Esc:
			act(A_EXIT);
			break;
		case Key::Alt:
			alt(k.ch);
			break;
		default:
			break;
		}
		group_ = static_cast<size_t>(g);
	}
	/* Alt and a letter: a menu, or Alt-X */
	void alt(char c) {
		switch (c) {
		case 'x': act(A_EXIT); break;
		case 'f': open_menu(1); break;
		case 'g': open_menu(2); break;
		case 'h': open_menu(3); break;
		case ' ': open_menu(0); break;
		default: break;
		}
	}
	/* the setting under the bar to its next (d 1) or last choice */
	void cycle(s16 d) {
		const Setting &s = model_[current()];
		s16 n = static_cast<s16>(s.choices.size()), at = 0;
		for (s16 i = 0; i < n; i++)
			if (s.choices[static_cast<size_t>(i)].value ==
			    pend_[current()])
				at = i;
		at = static_cast<s16>((at + d + n) % n);
		pend_[current()] = s.choices[static_cast<size_t>(at)].value;
	}
	/* a number up or down by one, within its bounds */
	void step(s16 d) {
		const Setting &s = model_[current()];
		if (s.kind != Setting::Number)
			return;
		s32 v = leading_long(pend_[current()]) + d;
		if (v < static_cast<s32>(s.lo) || v > static_cast<s32>(s.hi))
			return;
		pend_[current()] = dec(v);
	}
	/* the dialog for the setting under the bar */
	void edit() {
		edit_ = current();
		focus_ = 0;
		const Setting &s = model_[edit_];
		if (s.kind == Setting::Pick) {
			pick_ = 0;
			pick_top_ = 0;
			for (size_t i = 0; i < s.choices.size(); i++)
				if (s.choices[i].value == pend_[edit_])
					pick_ = static_cast<s16>(i);
			dlg_ = Dlg::Pick;
		} else {
			input_ = pend_[edit_];
			dlg_ = Dlg::Input;
		}
	}
	/* menu m opened */
	void open_menu(s16 m) {
		dlg_ = Dlg::Menu;
		menu_ = m;
		menu_item_ = 0;
	}
	/* the menu's keys */
	void menu_key(const Key &k) {
		std::vector<std::vector<Item>> all = menus();
		const std::vector<Item> &m = all[static_cast<size_t>(menu_)];
		const s16 n = static_cast<s16>(m.size());
		auto skip = [&](s16 d) {
			do
				menu_item_ = static_cast<s16>((menu_item_ + d + n) % n);
			while (m[static_cast<size_t>(menu_item_)].label.empty());
		};
		switch (k.code) {
		case Key::Esc:
		case Key::F10:
			dlg_ = Dlg::None;
			break;
		case Key::Left:
			open_menu(static_cast<s16>((menu_ + 3) % 4));
			break;
		case Key::Right:
			open_menu(static_cast<s16>((menu_ + 1) % 4));
			break;
		case Key::Up:
			skip(-1);
			break;
		case Key::Down:
			skip(1);
			break;
		case Key::Enter:
			dlg_ = Dlg::None;
			act(m[static_cast<size_t>(menu_item_)].action);
			break;
		case Key::Alt:
			dlg_ = Dlg::None;
			alt(k.ch);
			break;
		case Key::Char: {
			char c = static_cast<char>(k.ch >= 'A' && k.ch <= 'Z' ?
						   k.ch - 'A' + 'a' : k.ch);
			for (const auto &it : m) {
				size_t t = it.label.find('~');
				if (t == std::string::npos ||
				    t + 1 >= it.label.size())
					continue;
				char h = it.label[t + 1];
				if (h >= 'A' && h <= 'Z')
					h = static_cast<char>(h - 'A' + 'a');
				if (h == c) {
					dlg_ = Dlg::None;
					act(it.action);
					break;
				}
			}
			break;
		}
		default:
			break;
		}
	}
	/* the choices' keys */
	void pick_key(const Key &k) {
		const Setting &s = model_[edit_];
		const s16 n = static_cast<s16>(s.choices.size());
		switch (k.code) {
		case Key::Esc:
			dlg_ = Dlg::None;
			break;
		case Key::Tab:
			focus_ = static_cast<s16>((focus_ + 1) % 3);
			break;
		case Key::BackTab:
			focus_ = static_cast<s16>((focus_ + 2) % 3);
			break;
		case Key::Up:
			move(&pick_, -1, n);
			break;
		case Key::Down:
			move(&pick_, 1, n);
			break;
		case Key::PgUp:
			move(&pick_, -10, n);
			break;
		case Key::PgDn:
			move(&pick_, 10, n);
			break;
		case Key::Home:
			pick_ = 0;
			break;
		case Key::End:
			pick_ = static_cast<s16>(n - 1);
			break;
		case Key::Enter:
			if (focus_ != 2)
				pend_[edit_] = s.choices[static_cast<size_t>(
					pick_)].value;
			dlg_ = Dlg::None;
			break;
		case Key::Alt:
			if (k.ch == 'k') {
				pend_[edit_] = s.choices[static_cast<size_t>(
					pick_)].value;
				dlg_ = Dlg::None;
			} else if (k.ch == 'c') {
				dlg_ = Dlg::None;
			}
			break;
		case Key::Char: {
			/* the next choice that begins with the letter */
			char c = k.ch;
			for (s16 i = 1; i <= n; i++) {
				s16 j = static_cast<s16>((pick_ + i) % n);
				const std::string &l =
					s.choices[static_cast<size_t>(j)].label;
				if (!l.empty() && same_nocase(l.substr(0, 1),
							      std::string(1, c))) {
					pick_ = j;
					break;
				}
			}
			break;
		}
		default:
			break;
		}
	}
	/* the line's keys */
	void input_key(const Key &k) {
		const Setting &s = model_[edit_];
		switch (k.code) {
		case Key::Esc:
			dlg_ = Dlg::None;
			break;
		case Key::Tab:
			focus_ = static_cast<s16>((focus_ + 1) % 3);
			break;
		case Key::BackTab:
			focus_ = static_cast<s16>((focus_ + 2) % 3);
			break;
		case Key::Backspace:
			if (focus_ == 0 && !input_.empty())
				input_.pop_back();
			break;
		case Key::Char:
			if (focus_ == 0 && input_.size() < 50 &&
			    ((k.ch >= '0' && k.ch <= '9') ||
			     (k.ch == ',' && s.kind == Setting::Text)))
				input_ += k.ch;
			break;
		case Key::Ctrl:
			if (k.ch == 'u' || k.ch == 'y')
				input_.clear();
			break;
		case Key::Enter: {
			if (focus_ == 2) {
				dlg_ = Dlg::None;
				break;
			}
			std::string why = check(s, input_);
			if (!why.empty()) {
				show_message("Invalid value", { s.label + ": " +
								why }, true);
				break;
			}
			pend_[edit_] = input_;
			dlg_ = Dlg::None;
			break;
		}
		default:
			break;
		}
	}
	/* a message box's keys: any of Enter, Esc, Space closes it */
	void message_key(const Key &k) {
		if (k.code == Key::Enter || k.code == Key::Esc ||
		    (k.code == Key::Char && k.ch == ' '))
			dlg_ = Dlg::None;
	}
	/* the question on leaving: Yes saves, No leaves the changes */
	void confirm_key(const Key &k) {
		s16 choice = -1;
		switch (k.code) {
		case Key::Esc:
			dlg_ = Dlg::None;
			return;
		case Key::Tab:
		case Key::Right:
			focus_ = static_cast<s16>((focus_ + 1) % 3);
			return;
		case Key::BackTab:
		case Key::Left:
			focus_ = static_cast<s16>((focus_ + 2) % 3);
			return;
		case Key::Enter:
			choice = focus_;
			break;
		case Key::Char:
		case Key::Alt:
			choice = k.ch == 'y' || k.ch == 'Y' ? 0 :
				k.ch == 'n' || k.ch == 'N' ? 1 :
				k.ch == 'c' || k.ch == 'C' ? 2 : -1;
			break;
		default:
			return;
		}
		if (choice < 0)
			return;
		dlg_ = Dlg::None;
		if (choice == 2)
			return;
		if (choice == 0 && !save())
			return;
		leave();
		finish();
	}

	/*
	 *    what the menus and keys do
	 */

	/* a message box (error: red) */
	void show_message(const std::string &title,
			  std::vector<std::string> lines, bool error = false) {
		msg_title_ = title;
		msg_ = std::move(lines);
		msg_error_ = error;
		dlg_ = Dlg::Message;
	}
	/* what an item does */
	void act(s16 a) {
		if (a < 0) {
			group_ = static_cast<size_t>(-1 - a);
			on_settings_ = true;
			return;
		}
		switch (a) {
		case A_SAVE:
			if (save())
				show_message("Saved", { "The settings are in effect.",
							"F3 shows the init string for them." });
			break;
		case A_INIT: {
			std::vector<std::string> l = init_lines(model_, pend_,
								defs_, 63);
			if (l.empty())
				l.push_back("AT");
			l.insert(l.begin(), "Paste after ATZ in the next session:");
			l.insert(l.begin() + 1, "");
			if (dirty()) {
				l.push_back("");
				l.push_back("(with the changes not yet saved)");
			}
			show_message("Init string", l);
			break;
		}
		case A_DEFAULTS:
			pend_ = defs_;
			break;
		case A_RESTORE:
			pend_ = cur_;
			break;
		case A_EXIT:
			if (dirty()) {
				msg_title_ = "Confirm";
				msg_ = { "Save the changes before leaving?" };
				msg_error_ = false;
				focus_ = 0;
				dlg_ = Dlg::Confirm;
				break;
			}
			leave();
			finish();
			break;
		case A_KEYS:
			show_message("Keys", {
				"Up, Down, PgUp, PgDn   move the bar",
				"Tab                    groups or settings",
				"Enter                  change the setting",
				"Left, Right, Space     the next or last value",
				"+, -                   a number up or down",
				"Del                    the setting's default",
				"F2  save     F3  init string     F5  defaults",
				"F10 menu     Alt-X or Esc  exit",
				"Ctrl-L                 draw the screen again",
			});
			break;
		case A_ABOUT:
			show_message("About", {
				product(),
				"",
				"The modem's settings, full screen.",
				"AT!CT has them as a command line,",
				"AT!R prints the init string.",
			});
			break;
		default:
			break;
		}
	}
	/* the changes made, through their AT commands (+MS's four values
	   as one +MS); false, with a message, if the modem refused any */
	bool save() {
		std::vector<std::string> refused;
		bool ms = false;
		std::string ms_cmd = "+MS=";
		for (s16 f = 0; f < 4; f++)
			for (size_t i = 0; i < model_.size(); i++)
				if (model_[i].ms == f) {
					ms_cmd += (f ? "," : "") + pend_[i];
					ms |= pend_[i] != cur_[i];
				}
		for (size_t i = 0; i < model_.size(); i++) {
			if (pend_[i] == cur_[i] || model_[i].ms >= 0)
				continue;
			if (apply(model_[i], pend_[i]))
				refused.push_back("AT" + command(model_[i],
								 pend_[i]));
		}
		if (ms && run(ms_cmd))
			refused.push_back("AT" + ms_cmd);
		/* (what the modem has now, whatever it took) */
		model_ = model(m_);
		cur_ = values(m_, model_);
		defs_ = defaults(m_, model_);
		if (refused.empty()) {
			pend_ = cur_;
			return true;
		}
		std::vector<std::string> l = { "The modem refused:" };
		l.insert(l.end(), refused.begin(), refused.end());
		show_message("Error", l, true);
		pend_ = cur_;
		return false;
	}
};

} // namespace

/* AT!CF's configurator */
Config *make_screen(Modem &m, s16 rows, s16 cols, bool u)
{
	return new Tui(m, rows, cols, u);
}

} // namespace dcore
