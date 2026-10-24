/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    link.cpp - the virtual modem driver's pipe to dmodemd (link.h)
 *
 */

#include "dm_types.h"
#include "link.h"

#include <cstdarg>
#include <cstdio>

#include <windows.h>
#include <sddl.h>

namespace vmodem {

namespace {

const DWORD PIPE_BUFFER = 8192, RETRY_MS = 1000;

/* (the port's clock) */
u64 now_ms()
{
	return GetTickCount64();
}

/* the pipe's access: the system, the administrators, the local service
   (the driver's host) and the users logged on here (dmodemd as a
   program or a service) */
const char PIPE_SDDL[] = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;IU)";

} // namespace

/* see link.h */
Link::Link(void (*log)(const char *)) : port_(this, now_ms), log_(log) {}

/* see link.h */
Link::~Link()
{
	stop();
}

/* see link.h */
std::string Link::pipe_for(const std::string &name)
{
	return "\\\\.\\pipe\\dmodem-vm-" + name;
}

/* a message to the log */
void Link::say(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (!log_)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_(buf);
}

/* see link.h: the pipe made now, so that a second driver or a program
   with the name is seen at once (FIRST_PIPE_INSTANCE) */
bool Link::start(const std::string &name, std::string *why)
{
	SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
	PSECURITY_DESCRIPTOR sd = nullptr;

	name_ = name;
	pipe_name_ = pipe_for(name);
	if (ConvertStringSecurityDescriptorToSecurityDescriptorA(PIPE_SDDL,
			SDDL_REVISION_1, &sd, nullptr))
		sa.lpSecurityDescriptor = sd;
	HANDLE h = CreateNamedPipeA(pipe_name_.c_str(),
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
		FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
		PIPE_REJECT_REMOTE_CLIENTS,
		1, PIPE_BUFFER, PIPE_BUFFER, 0, sd ? &sa : nullptr);
	if (sd)
		LocalFree(sd);
	if (h == INVALID_HANDLE_VALUE) {
		if (why)
			*why = "can't make " + pipe_name_ + " (error " +
				std::to_string(GetLastError()) + ")";
		return false;
	}
	pipe_ = h;
	quit_ev_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	kick_ev_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	io_ev_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	wr_ev_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	thread_ = std::thread([this] { run(); });
	return true;
}

/* see link.h */
void Link::stop()
{
	if (thread_.joinable()) {
		quit_ = true;
		SetEvent((HANDLE)quit_ev_);
		thread_.join();
	}
	for (void **h : { &pipe_, &quit_ev_, &kick_ev_, &io_ev_, &wr_ev_ }) {
		if (*h)
			CloseHandle((HANDLE)*h);
		*h = nullptr;
	}
}

/* see link.h */
void Link::tx_ready()
{
	want_tx_ = true;
	if (kick_ev_)
		SetEvent((HANDLE)kick_ev_);
}

/* see link.h */
void Link::rx_room()
{
	want_room_ = true;
	if (kick_ev_)
		SetEvent((HANDLE)kick_ev_);
}

/* see link.h */
void Link::state_changed()
{
	want_state_ = true;
	if (kick_ev_)
		SetEvent((HANDLE)kick_ev_);
}

/* see link.h */
void Link::timer(u64 due)
{
	due_ = due;
	if (kick_ev_)
		SetEvent((HANDLE)kick_ev_);
}

/* the modem's characters the port had no room for, as it has room */
void Link::flush_rx()
{
	if (rx_held_.empty())
		return;
	size_t k = port_.rx_put((const u8 *)rx_held_.data(),
				rx_held_.size());
	rx_held_.erase(0, k);
}

/* what dmodemd sent, into frames, each done: false if they can't be
   (the stream lost, another version) */
bool Link::feed(const u8 *p, size_t n)
{
	WireReader::Frame f;

	rd_.feed(p, n);
	while (rd_.next(f)) {
		u16 ver;
		u8 lines;
		switch (f.type) {
		case FRAME_HELLO:
			if (!WireReader::hello(f, &ver, nullptr) ||
			    ver != WIRE_VERSION) {
				say("dmodemd's wire version is not ours (%u)",
				    WireReader::hello(f, &ver, nullptr) ?
				    (unsigned)ver : 0u);
				return false;
			}
			break;
		case FRAME_LINES:
			if (WireReader::lines(f, &lines))
				port_.set_lines(lines);
			break;
		case FRAME_DATA:
			if (rx_held_.empty()) {
				size_t k = port_.rx_put(f.p, f.n);
				rx_held_.append((const char *)f.p + k, f.n - k);
			} else {
				rx_held_.append((const char *)f.p, f.n);
			}
			break;
		default:
			break;
		}
	}
	if (rd_.bad()) {
		say("dmodemd's frames are not ours");
		return false;
	}
	return true;
}

/* a client: ConnectNamedPipe done or the client already there (false:
   the pipe can't take one, or quit_); the port's timeouts run while
   waiting */
bool Link::listen()
{
	OVERLAPPED ov = {};
	ov.hEvent = (HANDLE)io_ev_;
	ResetEvent(ov.hEvent);
	if (ConnectNamedPipe((HANDLE)pipe_, &ov))
		return true;
	DWORD e = GetLastError();
	if (e == ERROR_PIPE_CONNECTED)
		return true;
	if (e != ERROR_IO_PENDING) {
		say("%s: no client (error %lu)", pipe_name_.c_str(),
		    (unsigned long)e);
		WaitForSingleObject((HANDLE)quit_ev_, RETRY_MS);
		return false;
	}
	for (;;) {
		HANDLE w[3] = { (HANDLE)quit_ev_, ov.hEvent, (HANDLE)kick_ev_ };
		u64 due = due_, now = now_ms();
		DWORD wait = due ? (due > now ? (DWORD)(due - now) : 0) :
			INFINITE;
		DWORD r = WaitForMultipleObjects(3, w, FALSE, wait);
		if (r == WAIT_OBJECT_0 || quit_) {
			CancelIoEx((HANDLE)pipe_, &ov);
			DWORD n;
			GetOverlappedResult((HANDLE)pipe_, &ov, &n, TRUE);
			return false;
		}
		if (due && now_ms() >= due)
			port_.tick();
		/* (no dmodemd: the app's characters and state wait for
		   none) */
		want_tx_ = false;
		want_state_ = false;
		if (r == WAIT_OBJECT_0 + 1) {
			DWORD n;
			return GetOverlappedResult((HANDLE)pipe_, &ov, &n,
						   FALSE) != 0;
		}
	}
}

/* dmodemd on the pipe until it goes: our HELLO and the app's state,
   then both ways.  A write at a time, waited for as long as dmodemd
   takes (meanwhile the app's characters wait in the port, whose writes
   are held when it is full: the flow control) */
void Link::serve()
{
	OVERLAPPED rov = {}, wov = {};
	u8 buf[WIRE_MAX_DATA + WIRE_HEAD];
	bool reading = false, writing = false;
	std::string wbuf;

	rd_ = WireReader();
	rx_held_.clear();
	wire_hello(wbuf, name_);
	wire_state(wbuf, port_.app_state(), port_.baud());
	want_state_ = false;
	up_ = true;
	port_.connect(true);
	say("%s: dmodemd is there", pipe_name_.c_str());
	rov.hEvent = (HANDLE)io_ev_;
	wov.hEvent = (HANDLE)wr_ev_;
	while (!quit_) {
		/* a read whenever the port can take what comes */
		if (!reading && rx_held_.empty()) {
			ResetEvent(rov.hEvent);
			/* (done at once or not, the event says so: the
			   result is taken below either way) */
			if (!ReadFile((HANDLE)pipe_, buf, sizeof(buf), nullptr,
				      &rov) && GetLastError() != ERROR_IO_PENDING)
				break;
			reading = true;
		}
		/* the app's state and characters, when no write is out */
		if (!writing) {
			if (want_state_.exchange(false))
				wire_state(wbuf, port_.app_state(),
					   port_.baud());
			if (want_tx_.exchange(false)) {
				u8 tx[WIRE_MAX_DATA];
				size_t n;
				while (wbuf.size() < 4 * WIRE_MAX_DATA &&
				       (n = port_.tx_take(tx, sizeof(tx))) > 0)
					wire_data(wbuf, tx, n);
				/* (more there: next time) */
				if (wbuf.size() >= 4 * WIRE_MAX_DATA)
					want_tx_ = true;
			}
			if (!wbuf.empty()) {
				ResetEvent(wov.hEvent);
				if (!WriteFile((HANDLE)pipe_, wbuf.data(),
					       (DWORD)wbuf.size(), nullptr,
					       &wov) &&
				    GetLastError() != ERROR_IO_PENDING)
					break;
				writing = true;
			}
		}
		HANDLE w[4] = { (HANDLE)quit_ev_, (HANDLE)kick_ev_ };
		DWORD nw = 2;
		if (reading)
			w[nw++] = rov.hEvent;
		if (writing)
			w[nw++] = wov.hEvent;
		u64 due = due_, now = now_ms();
		DWORD wait = due ? (due > now ? (DWORD)(due - now) : 0) :
			INFINITE;
		if (WaitForMultipleObjects(nw, w, FALSE, wait) == WAIT_OBJECT_0)
			break;
		if (due && now_ms() >= due)
			port_.tick();
		if (reading && WaitForSingleObject(rov.hEvent, 0) ==
		    WAIT_OBJECT_0) {
			DWORD got = 0;
			reading = false;
			if (!GetOverlappedResult((HANDLE)pipe_, &rov, &got,
						 FALSE) ||
			    !feed(buf, got))
				break;
		}
		if (writing && WaitForSingleObject(wov.hEvent, 0) ==
		    WAIT_OBJECT_0) {
			DWORD done = 0;
			writing = false;
			if (!GetOverlappedResult((HANDLE)pipe_, &wov, &done,
						 FALSE))
				break;
			/* (what it didn't take goes again) */
			wbuf.erase(0, done);
		}
		if (want_room_.exchange(false))
			flush_rx();
	}
	if (reading || writing) {
		DWORD n;
		CancelIoEx((HANDLE)pipe_, nullptr);
		if (reading)
			GetOverlappedResult((HANDLE)pipe_, &rov, &n, TRUE);
		if (writing)
			GetOverlappedResult((HANDLE)pipe_, &wov, &n, TRUE);
	}
	up_ = false;
	port_.connect(false);
	say("%s: dmodemd has gone", pipe_name_.c_str());
}

/* the thread: dmodemd after dmodemd, until stop() */
void Link::run()
{
	while (!quit_) {
		if (!listen())
			continue;
		serve();
		DisconnectNamedPipe((HANDLE)pipe_);
	}
}

} // namespace vmodem
