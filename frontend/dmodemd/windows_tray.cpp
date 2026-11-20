/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    windows_tray.cpp - dmodemd's icon in the Windows notification area
 *                       and its log window (windows_tray.h)
 *
 *    Three threads of its own: two that read stdout's and stderr's
 *    pipes (each writes what it reads on to where the stream went
 *    before, and adds it to the log), and the tray's (its hidden
 *    window gets the icon's mouse messages; the log window and the
 *    About box are its).  Only ANSI calls (the A functions): Windows
 *    9x has the W ones only as stubs.
 *
 */

#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <io.h>
#include <process.h>

/* (winsock2.h before windows.h: run_loop.h has it) */
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include <dcore.h>

#include "run_loop.h"
#include "windows_tray.h"

namespace dmd {

namespace {

/* the icon's resource (dmodemd.rc) */
const int IDI_DMODEMD = 1;

/* the icon's messages to the tray's window; the log window's "there is
   more" */
const UINT WM_TRAY = WM_APP + 1;
const UINT WM_LOG_MORE = WM_APP + 2;

/* the menu's commands */
enum { CMD_VIEW_LOGS = 1, CMD_ABOUT, CMD_EXIT };

/* the log kept: this much at most, the oldest lines dropped first.
   (Windows 9x's edit control holds 64 KB at most) */
const size_t LOG_MAX = 48 * 1024;

const char TRAY_CLASS[] = "DModemTray";
const char LOG_CLASS[] = "DModemLog";

/* one stream (stdout, stderr) through a pipe: what was there before
   (its descriptor kept, and its handle), the pipe's end to read and
   its reader */
struct Tee {
	int fd = -1;
	DWORD std_id = 0;
	int saved = -1;
	HANDLE orig = INVALID_HANDLE_VALUE;
	HANDLE rd = nullptr;
	HANDLE thread = nullptr;
};

struct State {
	CRITICAL_SECTION lock;
	/* the log ("\r\n" lines), and what the log window hasn't shown */
	std::string log, more;
	bool cr = false;
	Tee out, err;
	HANDLE thread = nullptr;
	HANDLE ready = nullptr;
	bool ok = false;
	HINSTANCE inst = nullptr;
	HWND tray = nullptr, logw = nullptr, edit = nullptr;
	HFONT font = nullptr;
	HICON icon = nullptr, icon_big = nullptr;
	UINT taskbar_created = 0;
	bool in_menu = false, in_about = false;
	std::string name, dte, tip;
};

State *st;

/* "D-Modem 1.0.0" */
std::string product_version()
{
	return std::string(dcore::Modem::product) + " " +
		dcore::Modem::version;
}

/* text read from a stream, into the log ("\n" made "\r\n", as the
   edit control wants it) and to the log window if it is open */
void log_add(const char *p, DWORD n)
{
	bool tell;

	EnterCriticalSection(&st->lock);
	tell = st->more.empty();
	for (DWORD i = 0; i < n; i++) {
		if (p[i] == '\n' && !st->cr) {
			st->log += '\r';
			st->more += '\r';
		}
		st->cr = p[i] == '\r';
		st->log += p[i];
		st->more += p[i];
	}
	/* (the oldest whole lines out) */
	if (st->log.size() > LOG_MAX) {
		size_t cut = st->log.size() - LOG_MAX * 3 / 4;
		size_t nl = st->log.find('\n', cut);
		st->log.erase(0, nl == std::string::npos ? cut : nl + 1);
	}
	if (st->more.size() > LOG_MAX)
		st->more.erase(0, st->more.size() - LOG_MAX);
	LeaveCriticalSection(&st->lock);
	if (tell && st->logw)
		PostMessageA(st->logw, WM_LOG_MORE, 0, 0);
}

/* a stream's reader: until the pipe's other end is closed */
unsigned __stdcall tee_reader(void *arg)
{
	Tee *t = static_cast<Tee *>(arg);
	char buf[1024];
	DWORD n, done;

	while (ReadFile(t->rd, buf, sizeof(buf), &n, nullptr) && n) {
		if (t->orig != INVALID_HANDLE_VALUE)
			WriteFile(t->orig, buf, n, &done, nullptr);
		log_add(buf, n);
	}
	return 0;
}

/* stream fd (std_id its handle's) through a pipe to the log */
bool tee_start(Tee &t, int fd, DWORD std_id)
{
	SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
	HANDLE wr;
	int wfd;

	t.fd = fd;
	t.std_id = std_id;
	if (!CreatePipe(&t.rd, &wr, &sa, 0))
		return false;
	/* (what was there before: kept open, for the reader to write on
	   to; none when the program has no console) */
	t.saved = _dup(fd);
	if (t.saved >= 0)
		t.orig = reinterpret_cast<HANDLE>(_get_osfhandle(t.saved));
	wfd = _open_osfhandle(reinterpret_cast<intptr_t>(wr), _O_TEXT);
	if (wfd < 0) {
		CloseHandle(wr);
		goto fail;
	}
	if (_dup2(wfd, fd) < 0) {
		_close(wfd);
		goto fail;
	}
	_close(wfd);
	SetStdHandle(std_id, reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
	t.thread = reinterpret_cast<HANDLE>(
		_beginthreadex(nullptr, 0, tee_reader, &t, 0, nullptr));
	if (t.thread)
		return true;
	/* (no reader: the stream back as it was) */
	_dup2(t.saved, fd);
	SetStdHandle(std_id, t.orig);
fail:
	if (t.saved >= 0)
		_close(t.saved);
	t.saved = -1;
	t.orig = INVALID_HANDLE_VALUE;
	CloseHandle(t.rd);
	t.rd = nullptr;
	return false;
}

/* the stream as it was; what is in the pipe written on first (a second
   at most: a child process may hold the pipe open) */
void tee_stop(Tee &t)
{
	if (!t.thread)
		return;
	if (t.saved >= 0) {
		_dup2(t.saved, t.fd);
		SetStdHandle(t.std_id, t.orig);
	}
	WaitForSingleObject(t.thread, 1000);
	CloseHandle(t.thread);
	t.thread = nullptr;
	CloseHandle(t.rd);
	t.rd = nullptr;
	if (t.saved >= 0)
		_close(t.saved);
	t.saved = -1;
}

/* the icon in the tray (again, after Explorer starts over) */
bool icon_add()
{
	NOTIFYICONDATAA nid;

	/* (Windows 95's and NT 4.0's size of it: a tip of 64 characters) */
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = NOTIFYICONDATAA_V1_SIZE;
	nid.hWnd = st->tray;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAY;
	nid.hIcon = st->icon;
	lstrcpynA(nid.szTip, st->tip.c_str(), 64);
	return Shell_NotifyIconA(NIM_ADD, &nid) != FALSE;
}

/* the icon out of the tray */
void icon_remove()
{
	NOTIFYICONDATAA nid;

	memset(&nid, 0, sizeof(nid));
	nid.cbSize = NOTIFYICONDATAA_V1_SIZE;
	nid.hWnd = st->tray;
	nid.uID = 1;
	Shell_NotifyIconA(NIM_DELETE, &nid);
}

/* the log window's text: all of the log */
void log_fill()
{
	std::string text;

	EnterCriticalSection(&st->lock);
	text = st->log;
	st->more.clear();
	LeaveCriticalSection(&st->lock);
	SetWindowTextA(st->edit, text.c_str());
	SendMessageA(st->edit, EM_SETSEL, static_cast<WPARAM>(text.size()),
		     static_cast<LPARAM>(text.size()));
	SendMessageA(st->edit, EM_SCROLLCARET, 0, 0);
}

/* the log window's text: what came since, at the end (all of it again
   when the edit control would be too full) */
void log_append()
{
	std::string more;
	int len;

	EnterCriticalSection(&st->lock);
	more.swap(st->more);
	LeaveCriticalSection(&st->lock);
	if (more.empty())
		return;
	len = GetWindowTextLengthA(st->edit);
	if (static_cast<size_t>(len) + more.size() > LOG_MAX + LOG_MAX / 4) {
		log_fill();
		return;
	}
	SendMessageA(st->edit, EM_SETSEL, static_cast<WPARAM>(len),
		     static_cast<LPARAM>(len));
	SendMessageA(st->edit, EM_REPLACESEL, FALSE,
		     reinterpret_cast<LPARAM>(more.c_str()));
}

/* the log window open, in front */
void log_show()
{
	if (!st->logw)
		return;
	if (!IsWindowVisible(st->logw)) {
		log_fill();
		ShowWindow(st->logw, SW_SHOWNORMAL);
	} else if (IsIconic(st->logw)) {
		ShowWindow(st->logw, SW_RESTORE);
	}
	SetForegroundWindow(st->logw);
}

/* the About box: the version, the modem, its DTE (one at a time) */
void about()
{
	std::string text;

	if (st->in_about)
		return;
	st->in_about = true;
	text = product_version() + "\r\n\r\n"
		"A software modem: V.92, V.90, V.34 and the standards "
		"before them.\r\n\r\n"
		"Modem: " + st->name + "\r\n";
	if (!st->dte.empty())
		text += "DTE: " + st->dte + "\r\n";
	text += "Built: " __DATE__ " " __TIME__ "\r\n\r\n"
		"Copyright (C) 2026 D-Modem Authors\r\n"
		"GNU Affero General Public License, version 3\r\n"
		"https://github.com/D-Modem/D-Modem";
	MessageBoxA(st->tray, text.c_str(), "About D-Modem",
		    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
	st->in_about = false;
}

/* the right-click menu, where the mouse is */
void menu()
{
	HMENU m;
	POINT pt;
	int cmd;

	if (st->in_menu)
		return;
	m = CreatePopupMenu();
	if (!m)
		return;
	AppendMenuA(m, MF_STRING, CMD_VIEW_LOGS, "&View Logs");
	AppendMenuA(m, MF_STRING, CMD_ABOUT, "&About");
	AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuA(m, MF_STRING, CMD_EXIT, "E&xit");
	/* (bold: what the double-click does) */
	SetMenuDefaultItem(m, CMD_VIEW_LOGS, FALSE);
	GetCursorPos(&pt);
	/* (in front, so that a click elsewhere closes the menu; the
	   message after it, so that a second right-click opens it again:
	   Microsoft's KB135788) */
	st->in_menu = true;
	SetForegroundWindow(st->tray);
	cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD |
			     TPM_NONOTIFY, pt.x, pt.y, 0, st->tray, nullptr);
	PostMessageA(st->tray, WM_NULL, 0, 0);
	st->in_menu = false;
	DestroyMenu(m);
	switch (cmd) {
	case CMD_VIEW_LOGS:
		log_show();
		break;
	case CMD_ABOUT:
		about();
		break;
	case CMD_EXIT:
		/* (as Ctrl+C: the modem stops, main() saves its data and
		   calls Tray::stop()) */
		fprintf(stderr, "Exit chosen from the tray icon's menu.\n");
		RunLoop::stop();
		break;
	}
}

/* the tray's hidden window: the icon's mouse messages, Explorer's
   restart, WM_CLOSE from Tray::stop() */
LRESULT CALLBACK tray_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TRAY) {
		switch (lp) {
		case WM_LBUTTONDBLCLK:
			log_show();
			break;
		case WM_RBUTTONUP:
		case WM_CONTEXTMENU:
			menu();
			break;
		}
		return 0;
	}
	if (st->taskbar_created && msg == st->taskbar_created) {
		icon_add();
		return 0;
	}
	switch (msg) {
	case WM_CLOSE:
		icon_remove();
		if (st->logw)
			DestroyWindow(st->logw);
		st->logw = nullptr;
		DestroyWindow(w);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(w, msg, wp, lp);
}

/* the log window: its edit control, the text that comes */
LRESULT CALLBACK log_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		st->edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
			ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
			ES_AUTOHSCROLL, 0, 0, 0, 0, w, nullptr, st->inst,
			nullptr);
		if (!st->edit)
			return -1;
		SendMessageA(st->edit, EM_LIMITTEXT, 0x7ffe, 0);
		if (st->font)
			SendMessageA(st->edit, WM_SETFONT,
				     reinterpret_cast<WPARAM>(st->font), FALSE);
		return 0;
	case WM_SIZE:
		MoveWindow(st->edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
		return 0;
	case WM_SETFOCUS:
		SetFocus(st->edit);
		return 0;
	case WM_LOG_MORE:
		if (IsWindowVisible(w))
			log_append();
		else {
			EnterCriticalSection(&st->lock);
			st->more.clear();
			LeaveCriticalSection(&st->lock);
		}
		return 0;
	case WM_CLOSE:
		/* (hidden, not gone: the log goes on) */
		ShowWindow(w, SW_HIDE);
		return 0;
	}
	return DefWindowProcA(w, msg, wp, lp);
}

/* the tray's thread: its windows and the icon, then their messages
   until Tray::stop() */
unsigned __stdcall tray_thread(void *)
{
	WNDCLASSA wc;
	MSG msg;
	std::string title;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = tray_proc;
	wc.hInstance = st->inst;
	wc.lpszClassName = TRAY_CLASS;
	if (!RegisterClassA(&wc))
		goto out;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = log_proc;
	wc.hInstance = st->inst;
	wc.hIcon = st->icon_big;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	wc.lpszClassName = LOG_CLASS;
	if (!RegisterClassA(&wc))
		goto out;

	/* (a top-level window never shown: HWND_MESSAGE is 2000's) */
	st->tray = CreateWindowExA(0, TRAY_CLASS, st->tip.c_str(),
		WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, st->inst,
		nullptr);
	if (!st->tray)
		goto out;
	title = product_version() + " - " + st->name + " - Logs";
	st->logw = CreateWindowExA(0, LOG_CLASS, title.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 420,
		nullptr, nullptr, st->inst, nullptr);
	/* (Explorer's "TaskbarCreated" when it starts over: the icon again) */
	st->taskbar_created = RegisterWindowMessageA("TaskbarCreated");
	if (!icon_add()) {
		DestroyWindow(st->tray);
		st->tray = nullptr;
		goto out;
	}
	st->ok = true;
	SetEvent(st->ready);
	while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
	st->tray = nullptr;
	return 0;
out:
	if (st->logw)
		DestroyWindow(st->logw);
	st->logw = nullptr;
	SetEvent(st->ready);
	return 0;
}

} // namespace

/* see windows_tray.h */
bool Tray::start(const char *name)
{
	if (st)
		return st->ok;
	st = new State;
	InitializeCriticalSection(&st->lock);
	st->name = name ? name : "";
	st->tip = product_version();
	st->inst = GetModuleHandleA(nullptr);
	st->icon = static_cast<HICON>(LoadImageA(st->inst,
		MAKEINTRESOURCEA(IDI_DMODEMD), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		0));
	st->icon_big = LoadIconA(st->inst, MAKEINTRESOURCEA(IDI_DMODEMD));
	if (!st->icon)
		st->icon = LoadIconA(nullptr, MAKEINTRESOURCEA(32512));
	st->font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");

	st->ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!st->ready)
		return false;
	st->thread = reinterpret_cast<HANDLE>(
		_beginthreadex(nullptr, 0, tray_thread, nullptr, 0, nullptr));
	if (!st->thread)
		return false;
	/* (the icon there, or not, before the output goes through the
	   pipes) */
	WaitForSingleObject(st->ready, 10000);
	if (!st->ok)
		return false;
	fflush(stdout);
	fflush(stderr);
	tee_start(st->out, 1, STD_OUTPUT_HANDLE);
	tee_start(st->err, 2, STD_ERROR_HANDLE);
	return true;
}

/* see windows_tray.h */
void Tray::set_dte(const char *where)
{
	if (st)
		st->dte = where ? where : "";
}

/* see windows_tray.h */
void Tray::stop()
{
	if (!st)
		return;
	fflush(stdout);
	fflush(stderr);
	tee_stop(st->out);
	tee_stop(st->err);
	if (st->thread) {
		if (st->tray)
			PostMessageA(st->tray, WM_CLOSE, 0, 0);
		WaitForSingleObject(st->thread, 5000);
		CloseHandle(st->thread);
		st->thread = nullptr;
	}
	if (st->ready)
		CloseHandle(st->ready);
	if (st->font)
		DeleteObject(st->font);
	/* (st itself kept: a reader that didn't end in time may still
	   use it) */
}

} // namespace dmd
