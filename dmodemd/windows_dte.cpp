/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    windows_dte.cpp - dmodemd's DTE on Windows: a TCP connection or a
 *                      COM port (windows_dte.h)
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

/* the COM port's thread to the main loop: ESC, then what it means */
const unsigned char ESC = 0xff, DSR_OFF = 0x00, DSR_ON = 0x01;

/* the COM port's reader: how long it sleeps when the port has nothing
   (a read returns at once with what there is; Wine never ends an
   overlapped read that waits for the first character), and how long a
   write waits for the other end, in ms */
const DWORD READ_POLL_MS = 5, WRITE_WAIT_MS = 200;

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
 *    COM port
 */

ComDte::~ComDte()
{
	quit_ = true;
	if (port_)
		CancelIoEx((HANDLE)port_, nullptr);
	if (thread_.joinable())
		thread_.join();
	if (port_) {
		EscapeCommFunction((HANDLE)port_, CLRDTR);
		EscapeCommFunction((HANDLE)port_, CLRRTS);
		CloseHandle((HANDLE)port_);
	}
	for (int fd : pair_)
		if (fd >= 0)
			fd_close(fd);
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
	if (stream_pair(pair_) < 0) {
		ERR("--com %s: no socket pair\n", port);
		return false;
	}
	set_nonblocking(pair_[0]);
	thread_ = std::thread([this] { reader(); });
	return true;
}

/* the reader thread: the port's characters (ESC doubled) and its DSR's
   changes into the pair, until the port closes */
void ComDte::reader()
{
	OVERLAPPED ov = {};
	unsigned char buf[512];
	char out[2 * sizeof(buf) + 2];
	int dsr = -1;

	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	while (!quit_) {
		DWORD got = 0, st = 0;
		int n = 0;

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
		for (DWORD i = 0; i < got; i++) {
			out[n++] = (char)buf[i];
			if (buf[i] == ESC)
				out[n++] = (char)ESC;
		}
		if (GetCommModemStatus((HANDLE)port_, &st)) {
			int now = (st & MS_DSR_ON) ? 1 : 0;
			if (now != dsr) {
				out[n++] = (char)ESC;
				out[n++] = (char)(now ? DSR_ON : DSR_OFF);
				dsr = now;
			}
		}
		if (n > 0 && !send_all(pair_[1], out, n))
			break;
		if (got == 0)
			Sleep(READ_POLL_MS);
	}
	CloseHandle(ov.hEvent);
}

/* our RTS on while a call is up (com0com gives it to the other end as
   DCD when set up to); the modem's room */
bool ComDte::want_read(const dcore::Modem &m)
{
	bool carrier = m.link_state() == dcore::LinkState::Online;
	if (carrier != rts_)
		set_rts(carrier);
	return m.dte_space() > 0;
}

void ComDte::set_rts(bool on)
{
	rts_ = on;
	EscapeCommFunction((HANDLE)port_, on ? SETRTS : CLRRTS);
}

/* the port's characters into the modem; the other end's DTR dropped
   hangs it up */
int ComDte::serve(dcore::Modem &m)
{
	unsigned char raw[4096];
	char buf[sizeof(raw)];
	int room = m.dte_space(), n, k = 0;
	bool dropped = false;

	if (room <= 0)
		return 0;
	if (room > (int)sizeof(raw))
		room = sizeof(raw);
	n = fd_read(pair_[0], raw, room);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (n == 0) {
		ERR("%s: the port's reader has stopped\n", name_.c_str());
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
			DBG("%s: the DTE dropped DTR\n", name_.c_str());
			dropped = true;
		} else if (c == DSR_ON) {
			DBG("%s: the DTE raised DTR\n", name_.c_str());
		}
	}
	if (k > 0)
		m.dte_input(buf, k);
	if (dropped && m.link_state() != dcore::LinkState::Idle)
		m.hangup();
	return 0;
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

} // namespace dmd
