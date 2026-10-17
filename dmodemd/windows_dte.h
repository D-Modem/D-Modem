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
 *    ComDte   a COM port: one end of a virtual null-modem pair (com0com)
 *             whose other end the DTE (dial-up networking, a terminal or
 *             fax program) opens as its modem.  The other end's DTR,
 *             our DSR, dropped hangs the modem up; our RTS follows the
 *             carrier (com0com can give it to the other end as DCD).
 *
 *    Both stand on a socket for the main loop's select(), which on
 *    Windows takes nothing else: the TCP connection itself, or for the
 *    COM port one end of a socket pair a thread of its own fills from
 *    the port.
 *
 */
#ifndef DMODEMD_WINDOWS_DTE_H
#define DMODEMD_WINDOWS_DTE_H

#include <atomic>
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

/* see above */
class ComDte final : public Dte {
public:
	~ComDte();
	/* the port (COM5, or \\.\COM12), opened at 115200 8N1 with DTR and
	   RTS's flow control off: false if it can't be */
	bool open(const char *port);
	const std::string &where() const { return name_; }

	int fd() const override { return pair_[0]; }
	bool want_read(const dcore::Modem &m) override;
	int serve(dcore::Modem &m) override;
	int write(const char *buf, int n) override;

private:
	void reader();
	void set_rts(bool on);

	std::string name_;
	void *port_ = nullptr;          /* HANDLE */
	/* [1] the reader thread's, [0] the main loop's */
	int pair_[2] = { -1, -1 };
	std::thread thread_;
	std::atomic<bool> quit_{false};
	/* in-band on the pair: ESC ESC an ESC of the port's, ESC DSR_OFF
	   and ESC DSR_ON the other end's DTR */
	bool esc_ = false;
	bool rts_ = false;
};

} // namespace dmd

#endif
