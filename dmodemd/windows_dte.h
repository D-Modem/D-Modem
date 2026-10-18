/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    windows_dte.h - dmodemd's DTE on Windows, which has no ptys:
 *
 *    TcpDte   TCP connections on [address:]port, one at a time: a
 *             terminal (PuTTY's raw mode, telnet) or a program connects
 *             and talks AT to the modem; its going hangs the modem up,
 *             as a pty's DTE closing it with HUPCL does, and the next
 *             connection is taken.
 *    PipeDte  a named pipe, made (--pipe) or connected to (--pipe-connect):
 *             a program or a VM's COM port on the host as the DTE, one
 *             at a time; its going hangs the modem up.
 *    ComDte   a COM port: one end of a virtual null-modem pair (com0com)
 *             whose other end the DTE (dial-up networking, a terminal or
 *             fax program) opens as its modem.  The other end's DTR,
 *             our DSR, dropped hangs the modem up; our RTS follows the
 *             carrier (com0com can give it to the other end as DCD).
 *
 *    All stand on a socket for the main loop's select(), which on
 *    Windows takes nothing else: the TCP connection itself, or for the
 *    COM port and the pipe one end of a socket pair a thread of its
 *    own fills from them.
 *
 */
#ifndef DMODEMD_WINDOWS_DTE_H
#define DMODEMD_WINDOWS_DTE_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "sip_modem.h"

namespace dmd {

/* see above */
class TcpDte final : public Dte {
public:
	enum { BUFFER = 64 * 1024 };
	~TcpDte();
	/* listening on spec ([address:]port; 127.0.0.1 if no address):
	   false if it can't */
	bool open(const char *spec);
	/* where it listens, for the user ("127.0.0.1:2323") */
	const std::string &where() const { return where_; }

	int fd() const override { return client_ >= 0 ? client_ : listen_; }
	bool want_read(const dcore::Modem &m) override;
	bool want_write() const override {
		return client_ >= 0 && !out_.empty();
	}
	int serve(dcore::Modem &m) override;
	void flush() override;
	int write(const char *buf, int n) override;

private:
	void drop(dcore::Modem &m, const char *why);

	int listen_ = -1, client_ = -1;
	std::string where_, out_;
};

/* a DTE a thread of its own reads (a COM port, a named pipe): what it
   reads goes into one end of a socket pair, the main loop's descriptor
   the other.  The DTE's going (DSR dropped, the pipe's client gone)
   and coming travel in-band, escaped; its going hangs the modem up */
class ThreadDte : public Dte {
public:
	int fd() const override { return pair_[0]; }
	bool want_read(const dcore::Modem &m) override;
	int serve(dcore::Modem &m) override;
	const std::string &where() const { return name_; }

protected:
	/* (the derived class's destructor calls stop()) */
	~ThreadDte() override;
	/* the pair made and reader() started: false if it can't be */
	bool start();
	/* reader() told to end (quit_, wake()), and waited for */
	void stop();
	/* the thread: the DTE's characters and state, by put() and
	   put_state(), until quit_ */
	virtual void reader() = 0;
	/* (the reader stopped in what it waits for: quit_ev_ is set) */
	virtual void wake() {}
	/* the modem's carrier, on or off: for the DTE's control lines */
	virtual void carrier(bool) {}
	/* (reader thread) characters, and the DTE come or gone, to the
	   main loop: false when that can't be */
	bool put(const unsigned char *p, int n);
	bool put_state(bool up);
	/* (reader thread) ms in which quit_ breaks the wait: false
	   then */
	bool nap(unsigned ms);

	std::string name_;
	std::atomic<bool> quit_{false};
	/* set by stop(), for the reader's waits (an event HANDLE) */
	void *quit_ev_ = nullptr;

private:
	/* [1] the reader thread's, [0] the main loop's */
	int pair_[2] = { -1, -1 };
	std::thread thread_;
	/* in-band on the pair: ESC ESC an ESC of the DTE's, ESC DOWN and
	   ESC UP the DTE gone and come */
	bool esc_ = false;
	bool carrier_ = false;
};

/* see above */
class ComDte final : public ThreadDte {
public:
	~ComDte() override;
	/* the port (COM5, or \\.\COM12), opened at 115200 8N1 with DTR on and
	   RTS's flow control off: false if it can't be */
	bool open(const char *port);
	int write(const char *buf, int n) override;

private:
	void reader() override;
	void carrier(bool on) override;

	void *port_ = nullptr;          /* HANDLE */
};

/* a named pipe as the DTE: one dmodemd makes (--pipe), whose client is
   a program or a VM's COM port (Hyper-V's, VirtualBox's "connect to
   existing pipe"), or one another program makes and dmodemd connects
   to (--pipe-connect: QEMU's -serial pipe:, VirtualBox's own pipe).
   One client at a time; its going hangs the modem up, and the next is
   waited for (made: the next client; connected: the pipe again).  A
   pipe has no control lines: the DTE hangs up by ATH or by going */
class PipeDte final : public ThreadDte {
public:
	~PipeDte() override;
	/* name (\\.\pipe\x, or x for that) made (connect false: made
	   now, so that its name is ours) or connected to (true: whenever
	   it is there): false if it can't be */
	bool open(const char *name, bool connect);
	int write(const char *buf, int n) override;

private:
	void reader() override;
	void wake() override;
	/* (reader thread) the client's characters until it goes */
	void serve_client(void *h);
	/* (reader thread) the overlapped call on ov done, or quit_
	   (false: it is cancelled) */
	bool finish(void *h, void *ov, unsigned long *got);

	bool connect_ = false;
	std::mutex mu_;
	/* the pipe (HANDLE): made, or while connected; and whether a
	   client is there (under mu_) */
	void *pipe_ = nullptr;
	bool up_ = false;
};

} // namespace dmd

#endif
