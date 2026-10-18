/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    windows_dte.cpp - dmodemd's DTE on Windows: a TCP connection, a
 *                      COM port or a named pipe (windows_dte.h)
 *
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include <dcore.h>
#include <log.h>

#include "portable.h"
#include "windows_dte.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "dte: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/* a reader thread to the main loop: ESC, then what it means (DSR_OFF:
   the DTE gone, DSR_ON: there) */
const unsigned char ESC = 0xff, DSR_OFF = 0x00, DSR_ON = 0x01;

/* the COM port's reader: how long it sleeps when the port has nothing
   (a read returns at once with what there is; Wine never ends an
   overlapped read that waits for the first character), and how long a
   write waits for the other end, in ms */
const DWORD READ_POLL_MS = 5, WRITE_WAIT_MS = 200;

/* a named pipe's buffers each way, and how often --pipe-connect tries
   a pipe that isn't there (yet), in ms */
const DWORD PIPE_BUFFER = 64 * 1024, CONNECT_RETRY_MS = 500;

/* n octets to the stream socket fd, all of them (it waits) */
bool send_all(int fd, const char *p, int n)
{
	while (n > 0) {
		int r = fd_write(fd, p, n);
		if (r <= 0)
			return false;
		p += r;
		n -= r;
	}
	return true;
}

} // namespace

/*
 *    TCP
 */

/* the sockets closed */
TcpDte::~TcpDte()
{
	if (client_ >= 0)
		fd_close(client_);
	if (listen_ >= 0)
		fd_close(listen_);
}

/* see windows_dte.h */
bool TcpDte::open(const char *spec)
{
	std::string s = spec ? spec : "", host = "127.0.0.1", port = s;
	size_t colon = s.rfind(':');
	struct addrinfo hints = {}, *ai = nullptr;
	int on = 1;

	if (colon != std::string::npos) {
		host = s.substr(0, colon);
		port = s.substr(colon + 1);
		/* ([::1]:2323) */
		if (host.size() > 1 && host.front() == '[' &&
		    host.back() == ']')
			host = host.substr(1, host.size() - 2);
	}
	if (!net_init())
		return false;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	if (getaddrinfo(host.empty() ? nullptr : host.c_str(), port.c_str(),
			&hints, &ai) || !ai) {
		ERR("--tcp %s: no such address\n", spec);
		return false;
	}
	listen_ = socket_cloexec(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
	if (listen_ < 0 ||
	    setsockopt((SOCKET)listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		       (const char *)&on, sizeof(on)) ||
	    bind((SOCKET)listen_, ai->ai_addr, (int)ai->ai_addrlen) ||
	    listen((SOCKET)listen_, 1)) {
		ERR("--tcp %s: can't listen there (error %d)\n", spec,
		    WSAGetLastError());
		freeaddrinfo(ai);
		if (listen_ >= 0)
			fd_close(listen_);
		listen_ = -1;
		return false;
	}
	freeaddrinfo(ai);
	set_nonblocking(listen_);
	where_ = (host.find(':') != std::string::npos ? "[" + host + "]" :
		  host) + ":" + port;
	return true;
}

/* the listener's connection while there is none; the modem's room
   after */
bool TcpDte::want_read(const dcore::Modem &m)
{
	return client_ < 0 || m.dte_space() > 0;
}

/* the DTE gone: a call hung up, its output dropped, the next connection
   awaited */
void TcpDte::drop(dcore::Modem &m, const char *why)
{
	DBG("%s: %s: hanging up\n", where_.c_str(), why);
	if (m.link_state() != dcore::LinkState::Idle)
		m.hangup();
	fd_close(client_);
	client_ = -1;
	out_.clear();
}

/* a connection taken, or its characters into the modem */
int TcpDte::serve(dcore::Modem &m)
{
	char buf[4096];
	int room = m.dte_space(), n;

	if (client_ < 0) {
		SOCKET s = accept((SOCKET)listen_, nullptr, nullptr);
		int on = 1;
		if (s == INVALID_SOCKET)
			return 0;
		client_ = (int)s;
		set_nonblocking(client_);
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
			   sizeof(on));
		DBG("%s: a DTE connected\n", where_.c_str());
		return 0;
	}
	if (room <= 0)
		return 0;
	if (room > (int)sizeof(buf))
		room = sizeof(buf);
	n = fd_read(client_, buf, room);
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		drop(m, "the connection failed");
		return 0;
	}
	if (n == 0) {
		drop(m, "the DTE closed the connection");
		return 0;
	}
	m.dte_input(buf, n);
	return 0;
}

/* what waits, as much as the connection takes now */
void TcpDte::flush()
{
	while (client_ >= 0 && !out_.empty()) {
		int n = fd_write(client_, out_.data(), (int)out_.size());
		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EINTR)
				out_.clear();
			return;
		}
		out_.erase(0, n);
	}
}

/* the modem's characters: to the connection, what it can't take now
   kept (up to BUFFER); with no connection they go nowhere */
int TcpDte::write(const char *buf, int n)
{
	int took = 0;

	if (n <= 0)
		return 0;
	if (client_ < 0)
		return n;
	flush();
	if (out_.empty()) {
		int done = fd_write(client_, buf, n);
		if (done < 0 && errno != EAGAIN && errno != EINTR)
			return 0;
		if (done > 0) {
			buf += done;
			n -= done;
			took = done;
		}
	}
	if (n > (int)(BUFFER - out_.size()))
		n = (int)(BUFFER - out_.size());
	if (n > 0)
		out_.append(buf, n);
	return took + (n > 0 ? n : 0);
}

/*
 *    a DTE read by a thread of its own
 */

/* the thread stopped, the pair and the event closed */
ThreadDte::~ThreadDte()
{
	stop();
	for (int fd : pair_)
		if (fd >= 0)
			fd_close(fd);
	if (quit_ev_)
		CloseHandle((HANDLE)quit_ev_);
}

/* see windows_dte.h */
bool ThreadDte::start()
{
	quit_ev_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!quit_ev_ || stream_pair(pair_) < 0) {
		ERR("%s: no socket pair\n", name_.c_str());
		return false;
	}
	set_nonblocking(pair_[0]);
	thread_ = std::thread([this] { reader(); });
	return true;
}

/* see windows_dte.h */
void ThreadDte::stop()
{
	if (!thread_.joinable())
		return;
	quit_ = true;
	SetEvent((HANDLE)quit_ev_);
	wake();
	thread_.join();
}

/* see windows_dte.h: p with ESC doubled */
bool ThreadDte::put(const unsigned char *p, int n)
{
	char out[2 * 512];
	int k = 0;

	for (int i = 0; i < n; i++) {
		out[k++] = (char)p[i];
		if (p[i] == ESC)
			out[k++] = (char)ESC;
		if (k >= (int)sizeof(out) - 1 || i == n - 1) {
			if (!send_all(pair_[1], out, k))
				return false;
			k = 0;
		}
	}
	return true;
}

/* see windows_dte.h: ESC and DSR_ON or DSR_OFF */
bool ThreadDte::put_state(bool up)
{
	const char out[2] = { (char)ESC, (char)(up ? DSR_ON : DSR_OFF) };
	return send_all(pair_[1], out, 2);
}

/* see windows_dte.h */
bool ThreadDte::nap(unsigned ms)
{
	return WaitForSingleObject((HANDLE)quit_ev_, ms) == WAIT_TIMEOUT &&
		!quit_;
}

/* the carrier to the DTE's control lines as it changes; the modem's
   room */
bool ThreadDte::want_read(const dcore::Modem &m)
{
	bool on = m.link_state() == dcore::LinkState::Online;
	if (on != carrier_) {
		carrier_ = on;
		carrier(on);
	}
	return m.dte_space() > 0;
}

/* the DTE's characters into the modem; its going hangs it up */
int ThreadDte::serve(dcore::Modem &m)
{
	unsigned char raw[4096];
	char buf[sizeof(raw)];
	int room = m.dte_space(), n, k = 0;
	bool gone = false;

	if (room <= 0)
		return 0;
	if (room > (int)sizeof(raw))
		room = sizeof(raw);
	n = fd_read(pair_[0], raw, room);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (n == 0) {
		ERR("%s: the reader has stopped\n", name_.c_str());
		return 1;
	}
	for (int i = 0; i < n; i++) {
		unsigned char c = raw[i];
		if (!esc_) {
			if (c == ESC)
				esc_ = true;
			else
				buf[k++] = (char)c;
			continue;
		}
		esc_ = false;
		if (c == ESC) {
			buf[k++] = (char)c;
		} else if (c == DSR_OFF) {
			DBG("%s: the DTE has gone\n", name_.c_str());
			gone = true;
		} else if (c == DSR_ON) {
			DBG("%s: the DTE is there\n", name_.c_str());
		}
	}
	if (k > 0)
		m.dte_input(buf, k);
	if (gone && m.link_state() != dcore::LinkState::Idle)
		m.hangup();
	return 0;
}

/*
 *    COM port
 */

/* the thread stopped, DTR and RTS dropped, the port
   closed */
ComDte::~ComDte()
{
	stop();
	if (port_) {
		EscapeCommFunction((HANDLE)port_, CLRDTR);
		EscapeCommFunction((HANDLE)port_, CLRRTS);
		CloseHandle((HANDLE)port_);
	}
}

/* see windows_dte.h */
bool ComDte::open(const char *port)
{
	std::string path = port;
	DCB dcb = {};
	COMMTIMEOUTS to = {};
	HANDLE h;

	if (path.compare(0, 4, "\\\\.\\") != 0)
		path = "\\\\.\\" + path;
	name_ = port;
	h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
			nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		ERR("--com %s: can't open it (error %lu)\n", port,
		    (unsigned long)GetLastError());
		return false;
	}
	port_ = h;
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(h, &dcb)) {
		ERR("--com %s: not a COM port (error %lu)\n", port,
		    (unsigned long)GetLastError());
		return false;
	}
	dcb.BaudRate = CBR_115200;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fNull = FALSE;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;
	dcb.fAbortOnError = FALSE;
	if (!SetCommState(h, &dcb)) {
		ERR("--com %s: can't set it up (error %lu)\n", port,
		    (unsigned long)GetLastError());
		return false;
	}
	/* a read returns at once with the characters there are */
	to.ReadIntervalTimeout = MAXDWORD;
	to.ReadTotalTimeoutMultiplier = 0;
	to.ReadTotalTimeoutConstant = 0;
	to.WriteTotalTimeoutConstant = WRITE_WAIT_MS;
	SetCommTimeouts(h, &to);
	PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
	return start();
}

/* the reader thread: the port's characters and its DSR's changes, until
   the port closes */
void ComDte::reader()
{
	OVERLAPPED ov = {};
	unsigned char buf[512];
	int dsr = -1;

	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	while (!quit_) {
		DWORD got = 0, st = 0;

		ResetEvent(ov.hEvent);
		if (!ReadFile((HANDLE)port_, buf, sizeof(buf), &got, &ov)) {
			if (GetLastError() != ERROR_IO_PENDING ||
			    !GetOverlappedResult((HANDLE)port_, &ov, &got,
						 TRUE)) {
				if (quit_)
					break;
				DWORD errs;
				ClearCommError((HANDLE)port_, &errs, nullptr);
				got = 0;
			}
		}
		if (got > 0 && !put(buf, (int)got))
			break;
		if (GetCommModemStatus((HANDLE)port_, &st)) {
			int now = (st & MS_DSR_ON) ? 1 : 0;
			if (now != dsr) {
				if (!put_state(now))
					break;
				dsr = now;
			}
		}
		if (got == 0 && !nap(READ_POLL_MS))
			break;
	}
	CloseHandle(ov.hEvent);
}

/* our RTS on while a call is up (com0com gives it to the other end as
   DCD when set up to) */
void ComDte::carrier(bool on)
{
	EscapeCommFunction((HANDLE)port_, on ? SETRTS : CLRRTS);
}

/* to the port: what it took within WRITE_WAIT_MS */
int ComDte::write(const char *buf, int n)
{
	OVERLAPPED ov = {};
	DWORD done = 0;

	if (n <= 0 || !port_)
		return 0;
	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!WriteFile((HANDLE)port_, buf, (DWORD)n, &done, &ov)) {
		if (GetLastError() == ERROR_IO_PENDING)
			GetOverlappedResult((HANDLE)port_, &ov, &done, TRUE);
		else
			done = 0;
	}
	CloseHandle(ov.hEvent);
	return (int)done;
}

/*
 *    named pipe
 */

/* the thread stopped, the pipe closed */
PipeDte::~PipeDte()
{
	stop();
	if (pipe_)
		CloseHandle((HANDLE)pipe_);
}

/* see windows_dte.h */
bool PipeDte::open(const char *name, bool connect)
{
	std::string path = name;

	if (path.compare(0, 2, "\\\\") != 0)
		path = "\\\\.\\pipe\\" + path;
	name_ = path;
	connect_ = connect;
	if (!connect) {
		/* one instance, ours alone (FIRST_PIPE_INSTANCE: no other
		   program has the name), for this machine's clients */
		HANDLE h = CreateNamedPipeA(path.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
			FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
			PIPE_REJECT_REMOTE_CLIENTS,
			1, PIPE_BUFFER, PIPE_BUFFER, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			ERR("--pipe %s: can't make it (error %lu)\n",
			    path.c_str(), (unsigned long)GetLastError());
			return false;
		}
		pipe_ = h;
	}
	return start();
}

/* (the waits end on quit_ev_; this ends a pending call at once) */
void PipeDte::wake()
{
	std::lock_guard<std::mutex> lk(mu_);
	if (pipe_)
		CancelIoEx((HANDLE)pipe_, nullptr);
}

/* see windows_dte.h */
bool PipeDte::finish(void *h, void *ovp, unsigned long *got)
{
	OVERLAPPED *ov = (OVERLAPPED *)ovp;
	HANDLE w[2] = { ov->hEvent, (HANDLE)quit_ev_ };
	DWORD n = 0;
	BOOL ok;

	if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0)
		CancelIoEx((HANDLE)h, ov);
	ok = GetOverlappedResult((HANDLE)h, ov, &n, TRUE);
	if (got)
		*got = n;
	/* (a message-mode pipe's longer message: the rest comes next) */
	return (ok || GetLastError() == ERROR_MORE_DATA) && !quit_;
}

/* the reader thread: a client at a time (made: connected to our pipe;
   connecting: the pipe opened when it is there), its characters until
   it goes */
void PipeDte::reader()
{
	OVERLAPPED ov = {};
	bool waiting = false;

	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	while (!quit_) {
		HANDLE h;

		if (connect_) {
			h = CreateFileA(name_.c_str(),
					GENERIC_READ | GENERIC_WRITE, 0,
					nullptr, OPEN_EXISTING,
					FILE_FLAG_OVERLAPPED, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				if (!waiting)
					DBG("%s: waiting for the pipe\n",
					    name_.c_str());
				waiting = true;
				if (!nap(CONNECT_RETRY_MS))
					break;
				continue;
			}
			waiting = false;
			std::lock_guard<std::mutex> lk(mu_);
			pipe_ = h;
		} else {
			h = (HANDLE)pipe_;
			ResetEvent(ov.hEvent);
			if (!ConnectNamedPipe(h, &ov)) {
				DWORD e = GetLastError();
				if (e == ERROR_IO_PENDING) {
					if (!finish(h, &ov, nullptr))
						break;
				} else if (e != ERROR_PIPE_CONNECTED) {
					ERR("%s: no client (error %lu)\n",
					    name_.c_str(), (unsigned long)e);
					if (!nap(CONNECT_RETRY_MS))
						break;
					continue;
				}
			}
		}
		{
			std::lock_guard<std::mutex> lk(mu_);
			up_ = true;
		}
		if (!put_state(true))
			break;
		serve_client(h);
		{
			std::lock_guard<std::mutex> lk(mu_);
			up_ = false;
			if (connect_) {
				CloseHandle(h);
				pipe_ = nullptr;
			} else {
				DisconnectNamedPipe(h);
			}
		}
		if (quit_ || !put_state(false))
			break;
	}
	CloseHandle(ov.hEvent);
}

/* see windows_dte.h */
void PipeDte::serve_client(void *h)
{
	OVERLAPPED ov = {};
	unsigned char buf[512];

	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	while (!quit_) {
		DWORD got = 0;

		ResetEvent(ov.hEvent);
		if (!ReadFile((HANDLE)h, buf, sizeof(buf), &got, &ov)) {
			DWORD e = GetLastError();
			if (e == ERROR_IO_PENDING) {
				unsigned long n = 0;
				if (!finish(h, &ov, &n))
					break;
				got = n;
			} else if (e != ERROR_MORE_DATA) {
				break;          /* (broken: the client went) */
			}
		}
		/* (a zero-length read: a zero-length message, not an end) */
		if (got > 0 && !put(buf, (int)got))
			break;
	}
	CloseHandle(ov.hEvent);
}

/* to the client: what it took within WRITE_WAIT_MS; without one the
   modem's characters go nowhere */
int PipeDte::write(const char *buf, int n)
{
	std::lock_guard<std::mutex> lk(mu_);
	OVERLAPPED ov = {};
	DWORD done = 0;

	if (n <= 0)
		return 0;
	if (!pipe_ || !up_)
		return n;
	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!WriteFile((HANDLE)pipe_, buf, (DWORD)n, &done, &ov)) {
		if (GetLastError() == ERROR_IO_PENDING) {
			if (WaitForSingleObject(ov.hEvent, WRITE_WAIT_MS) !=
			    WAIT_OBJECT_0)
				CancelIoEx((HANDLE)pipe_, &ov);
			if (!GetOverlappedResult((HANDLE)pipe_, &ov, &done,
						 TRUE) &&
			    GetLastError() != ERROR_OPERATION_ABORTED)
				done = (DWORD)n;        /* (gone: dropped) */
		} else {
			done = (DWORD)n;                /* (gone: dropped) */
		}
	}
	CloseHandle(ov.hEvent);
	return (int)done;
}

} // namespace dmd
