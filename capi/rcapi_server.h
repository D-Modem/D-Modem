/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    rcapi_server.h - the controller's applications over TCP, in the
 *                     remote CAPI protocol of libcapi20's "rcapi" module
 *                     (and rcapid's): /etc/capi20.conf's
 *                     REMOTE rcapi <host> <port>
 *
 *    One connection an application.  Each message on it: its length
 *    (a word, big endian, itself included), then a CAPI message as
 *    CAPI has it, a DATA_B3's data after it.  The protocol's own
 *    commands (REGISTER, GET_PROFILE and the rest, RCAPI_* in
 *    capi_msg.h) have a CAPI header with length 0, a dword (the
 *    controller), then theirs; their CONFs a header, then theirs.
 *
 *    The modem: a file descriptor (its pty, or a TCP connection to
 *    dmodemd's DTE), non-blocking.
 *
 */

#ifndef DMODEM_CAPI_RCAPI_SERVER_H
#define DMODEM_CAPI_RCAPI_SERVER_H

#include <map>
#include <string>
#include <vector>

#include "capi_controller.h"

namespace capi {

class Server : public Controller::Out {
public:
	/* the controller on the modem at modem_fd (the server's from now
	   on), init_extra after its setup commands */
	Server(int modem_fd, const std::string &init_extra = "");
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	/* listen at addr (NULL: every address) and port (0: one the
	   system picks); the port, or -1 */
	int listen(const char *addr, int port);
	/* the connections, the modem and the timers for timeout_ms at
	   most (-1: until something happens); false once the modem is
	   gone */
	bool step(int timeout_ms);
	/* the controller (for a test) */
	Controller &controller() { return ctrl_; }

	/* Controller::Out's */
	void to_app(const Msg &m) override;
	size_t to_modem(const u8 *p, size_t n) override;

private:
	struct Conn {
		u16 appl = 0;
		bool registered = false;
		std::vector<u8> in, out;
	};
	void accept_conn(u64 now);
	bool read_conn(int fd, Conn &c, u64 now);
	void handle(int fd, Conn &c, const Msg &m, u64 now);
	void reply(int fd, Conn &c, const Msg &req, u16 cmd,
		   const std::vector<u8> &payload);
	void flush(int fd, Conn &c);
	void close_conn(int fd, u64 now);

	int modem_fd_;
	int listen_fd_ = -1;
	Controller ctrl_;
	std::map<int, Conn> conns_;
	u16 next_appl_ = 1;
};

/* the modem's file descriptor: a pty or serial device by its path
   (raw), or "tcp:host:port"; non-blocking; -1 on failure */
int open_modem(const std::string &where);

} // namespace capi

#endif
