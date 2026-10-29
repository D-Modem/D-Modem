/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    link.h - the virtual modem driver's end of its pipe to dmodemd
 *             (Windows): the COM port (port.h) and a thread of its own
 *             that
 *               - makes \\.\pipe\dmodem-vm-<port> (this machine's clients
 *                 only, one at a time) and waits for dmodemd on it;
 *               - gives dmodemd the app's characters and the app's side
 *                 (DATA, STATE: wire.h), and the port the modem's
 *                 characters and lines (DATA, LINES), holding the pipe
 *                 while the port has no room;
 *               - runs the port's timeouts (Port::tick).
 *             dmodemd's going is the port's modem gone (Port::connect).
 *
 *    The UMDF driver (driver.cpp) runs one for its device; vmodem_test
 *    runs one beside dmodemd.exe under Wine, with no driver.
 *
 */
#ifndef DMODEM_WINMODEM_LINK_H
#define DMODEM_WINMODEM_LINK_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "dm_types.h"
#include "port.h"
#include "wire.h"

namespace vmodem {

/* see above */
class Link final : public PortHost {
public:
	/* (log: where its messages go, null for nowhere) */
	explicit Link(void (*log)(const char *msg) = nullptr);
	~Link() override;

	/* the port its app (or modem.sys) uses */
	Port &port() { return port_; }
	/* the pipe for port name (COM7: \\.\pipe\dmodem-vm-COM7) made and the
	   thread started: false, with why, if the pipe can't be made */
	bool start(const std::string &name, std::string *why);
	/* the thread ended, the pipe closed */
	void stop();
	/* the pipe's name */
	const std::string &pipe_name() const { return pipe_name_; }
	/* dmodemd is on the pipe */
	bool connected() const { return up_; }

	/* (PortHost: the thread woken for each) */
	void tx_ready() override;
	void rx_room() override;
	void state_changed() override;
	void timer(u64 due) override;

	/* the pipe's name for port name */
	static std::string pipe_for(const std::string &name);

private:
	void run();
	bool listen();
	void serve();
	bool feed(const u8 *p, size_t n);
	void flush_rx();
	void say(const char *fmt, ...);

	Port port_;
	void (*log_)(const char *);
	std::string name_, pipe_name_;
	std::thread thread_;
	std::atomic<bool> quit_{false}, up_{false};
	std::atomic<bool> want_tx_{false}, want_state_{false}, want_room_{false};
	std::atomic<u64> due_{0};
	void *pipe_ = nullptr;          /* HANDLE */
	void *quit_ev_ = nullptr, *kick_ev_ = nullptr, *io_ev_ = nullptr;
	void *wr_ev_ = nullptr;
	WireReader rd_;
	std::string rx_held_;           /* the modem's, the port had no room */
};

} // namespace vmodem

#endif
