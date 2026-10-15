/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    client.h - ARI's transport: REST requests over HTTP/1.1, and the
 *               event stream over a WebSocket (RFC 6455)
 *
 *    Plain HTTP only (no TLS): Asterisk's HTTP server on the same host
 *    or a trusted network.  Each request has a connection of its own
 *    (Connection: close), so that any thread may make one.
 *
 */
#ifndef DMODEM_ARI_CLIENT_H
#define DMODEM_ARI_CLIENT_H

#include <string>

namespace ari {

/* Asterisk's HTTP server and the ARI user */
struct Server {
	std::string host = "127.0.0.1";
	int port = 8088;
	std::string user, password;

	/* http://host[:port] into host and port; false if it is not that */
	bool set_url(const std::string &url);
};

/* s with what a query string cannot carry as %XX */
std::string url_encode(const std::string &s);

/* method path (with its query) on the server; the reply's status (-1 if
   none came) and body */
int request(const Server &srv, const char *method, const std::string &path,
	    std::string *body);

/* a WebSocket client: the event stream of /ari/events */
class WebSocket {
public:
	~WebSocket() { close(); }

	/* the handshake for path; false (and closed) if it fails */
	bool open(const Server &srv, const std::string &path);
	/* the next text message into msg, waiting up to timeout_ms (-1:
	   for good); 1 a message, 0 none yet, -1 the connection is gone */
	int read(std::string &msg, int timeout_ms);
	void close();

private:
	int fd_ = -1;
	std::string in_;        /* bytes read, not yet a whole frame */
	std::string partial_;   /* a fragmented message so far */

	bool send_frame(int opcode, const std::string &payload);
	/* a whole frame from in_, if there is one: 1, 0, -1 */
	int frame(int &opcode, bool &fin, std::string &payload);
};

} // namespace ari

#endif
