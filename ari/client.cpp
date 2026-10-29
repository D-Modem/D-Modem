/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    client.cpp - ARI's HTTP and WebSocket transport (see client.h)
 *
 */
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "dm_types.h"
#include "client.h"

/* (sends without SIGPIPE: where there is no MSG_NOSIGNAL, the host
   ignores SIGPIPE as Asterisk, FreeSWITCH and dmodem-ari do) */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace ari {

namespace {

/* the longest a request waits for Asterisk, ms */
const s16 REQUEST_MS = 10000;

/* RFC 4648 base64 of s */
std::string base64(const std::string &s)
{
	static const char abc[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
		"0123456789+/";
	std::string out;
	size_t i;

	for (i = 0; i + 2 < s.size(); i += 3) {
		u32 v = u32{static_cast<u8>(s[i])} << 16 |
			     u32{static_cast<u8>(s[i + 1])} << 8 |
			     static_cast<u8>(s[i + 2]);
		out += abc[v >> 18];
		out += abc[v >> 12 & 63];
		out += abc[v >> 6 & 63];
		out += abc[v & 63];
	}
	if (i + 1 == s.size()) {
		u32 v = u32{static_cast<u8>(s[i])} << 16;
		out += abc[v >> 18];
		out += abc[v >> 12 & 63];
		out += "==";
	} else if (i + 2 == s.size()) {
		u32 v = u32{static_cast<u8>(s[i])} << 16 |
			     u32{static_cast<u8>(s[i + 1])} << 8;
		out += abc[v >> 18];
		out += abc[v >> 12 & 63];
		out += abc[v >> 6 & 63];
		out += '=';
	}
	return out;
}

/* a TCP connection to the server, -1 if none */
int connect_to(const Server &srv)
{
	struct addrinfo hints = {}, *res, *ai;
	char port[16];
	int fd = -1;

	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof(port), "%" PRId32, srv.port);
	if (getaddrinfo(srv.host.c_str(), port, &hints, &res))
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		/* (closed on exec: fcntl(), as Haiku has no SOCK_CLOEXEC) */
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		::close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

/* all of s to fd; false if that fails */
bool send_all(int fd, const std::string &s)
{
	size_t off = 0;
	while (off < s.size()) {
		ssize_t n = send(fd, s.data() + off, s.size() - off,
				 MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		off += n;
	}
	return true;
}

/* what fd has within timeout_ms onto buf: the bytes read, 0 at its end
   or on a timeout (*timed_out), -1 on an error */
ssize_t recv_some(int fd, std::string &buf, s16 timeout_ms, bool *timed_out)
{
	struct pollfd p = { fd, POLLIN, 0 };
	char tmp[4096];
	int r;

	*timed_out = false;
	do
		r = poll(&p, 1, timeout_ms);
	while (r < 0 && errno == EINTR);
	if (r == 0) {
		*timed_out = true;
		return 0;
	}
	if (r < 0)
		return -1;
	ssize_t n;
	do
		n = recv(fd, tmp, sizeof(tmp), 0);
	while (n < 0 && errno == EINTR);
	if (n > 0)
		buf.append(tmp, n);
	return n;
}

/* the request line and the headers every request has */
std::string head(const Server &srv, const char *method,
		 const std::string &path)
{
	std::string h = std::string(method) + " " + path + " HTTP/1.1\r\n" +
		"Host: " + srv.host + ":" + std::to_string(srv.port) + "\r\n";
	if (!srv.user.empty())
		h += "Authorization: Basic " +
			base64(srv.user + ":" + srv.password) + "\r\n";
	return h;
}

/* a header's value in the headers h (name in lower case), "" if none */
std::string header(const std::string &h, const char *name)
{
	size_t pos = 0, n = strlen(name);
	while ((pos = h.find("\r\n", pos)) != std::string::npos) {
		pos += 2;
		if (h.size() - pos > n && !strncasecmp(h.c_str() + pos, name, n) &&
		    h[pos + n] == ':') {
			size_t v = pos + n + 1, e = h.find("\r\n", v);
			while (v < h.size() && h[v] == ' ')
				v++;
			return h.substr(v, e == std::string::npos ? e : e - v);
		}
	}
	return std::string();
}

} // namespace

/* see client.h */
bool Server::set_url(const std::string &url)
{
	std::string rest;
	size_t colon;

	if (url.compare(0, 7, "http://"))
		return false;
	rest = url.substr(7);
	if (!rest.empty() && rest.back() == '/')
		rest.pop_back();
	colon = rest.rfind(':');
	if (colon == std::string::npos) {
		host = rest;
		port = 80;
	} else {
		host = rest.substr(0, colon);
		port = static_cast<s32>(
			strtol(rest.c_str() + colon + 1, nullptr, 10));
	}
	return !host.empty() && port > 0;
}

/* see client.h */
std::string url_encode(const std::string &s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || strchr("-_.~", c)) {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

/* see client.h */
s32 request(const Server &srv, const char *method, const std::string &path,
	    std::string *body)
{
	std::string reply;
	int fd = connect_to(srv), status = -1;
	size_t end;
	bool timed_out = false;

	if (body)
		body->clear();
	if (fd < 0)
		return -1;
	if (!send_all(fd, head(srv, method, path) +
		      "Content-Length: 0\r\nConnection: close\r\n\r\n")) {
		::close(fd);
		return -1;
	}
	/* to the end of the connection, or of the body as long as the
	   headers say */
	for (;;) {
		ssize_t n = recv_some(fd, reply, REQUEST_MS, &timed_out);
		if (n <= 0)
			break;
		end = reply.find("\r\n\r\n");
		if (end != std::string::npos) {
			std::string len = header(reply.substr(0, end + 2),
						 "content-length");
			if (!len.empty() &&
			    reply.size() >= end + 4 + strtoul(len.c_str(), nullptr, 10))
				break;
		}
	}
	::close(fd);
	if (sscanf(reply.c_str(), "HTTP/1.%*d %d", &status) != 1)
		return -1;
	end = reply.find("\r\n\r\n");
	if (body && end != std::string::npos)
		*body = reply.substr(end + 4);
	return status;
}

/* see client.h */
bool WebSocket::open(const Server &srv, const std::string &path)
{
	std::random_device rd;
	std::string key, reply;
	bool timed_out;

	close();
	fd_ = connect_to(srv);
	if (fd_ < 0)
		return false;
	for (s16 i = 0; i < 16; i++)
		key += static_cast<char>(rd() & 0xff);
	if (!send_all(fd_, head(srv, "GET", path) +
		      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
		      "Sec-WebSocket-Key: " + base64(key) + "\r\n"
		      "Sec-WebSocket-Version: 13\r\n\r\n")) {
		close();
		return false;
	}
	/* the 101; what follows the headers is the first frames */
	while (reply.find("\r\n\r\n") == std::string::npos) {
		if (recv_some(fd_, reply, REQUEST_MS, &timed_out) <= 0) {
			close();
			return false;
		}
	}
	size_t end = reply.find("\r\n\r\n");
	int status = 0;
	if (sscanf(reply.c_str(), "HTTP/1.%*d %d", &status) != 1 ||
	    status != 101) {
		fprintf(stderr, "dmodem-ari: %s: %.*s\n", path.c_str(),
			static_cast<int>(reply.find("\r\n")), reply.c_str());
		close();
		return false;
	}
	in_ = reply.substr(end + 4);
	partial_.clear();
	return true;
}

/* see client.h */
void WebSocket::close()
{
	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	in_.clear();
	partial_.clear();
}

/* a frame of ours: masked, as a client's must be */
bool WebSocket::send_frame(s16 opcode, const std::string &payload)
{
	std::random_device rd;
	std::string f;
	u8 mask[4];

	f += static_cast<char>(0x80 | opcode);
	if (payload.size() < 126) {
		f += static_cast<char>(0x80 | payload.size());
	} else {
		f += static_cast<char>(0x80 | 126);
		f += static_cast<char>(payload.size() >> 8);
		f += static_cast<char>(payload.size() & 0xff);
	}
	for (auto &m : mask) {
		m = rd() & 0xff;
		f += static_cast<char>(m);
	}
	for (size_t i = 0; i < payload.size(); i++)
		f += static_cast<char>(payload[i] ^ mask[i & 3]);
	return send_all(fd_, f);
}

/* see client.h */
s16 WebSocket::frame(s16 &opcode, bool &fin, std::string &payload)
{
	const u8 *b = reinterpret_cast<const u8 *>(in_.data());
	size_t have = in_.size(), hdr = 2;
	u64 len;
	bool masked;

	if (have < 2)
		return 0;
	fin = b[0] & 0x80;
	opcode = b[0] & 0x0f;
	masked = b[1] & 0x80;
	len = b[1] & 0x7f;
	if (len == 126) {
		if (have < 4)
			return 0;
		len = static_cast<u64>(b[2]) << 8 | b[3];
		hdr = 4;
	} else if (len == 127) {
		if (have < 10)
			return 0;
		len = 0;
		for (s16 i = 0; i < 8; i++)
			len = len << 8 | b[2 + i];
		hdr = 10;
	}
	if (len > (UINT32_C(1) << 24))
		return -1;      /* (no event is that long) */
	if (masked)
		hdr += 4;
	if (have < hdr + len)
		return 0;
	payload.assign(in_, hdr, len);
	if (masked)
		for (size_t i = 0; i < len; i++)
			payload[i] ^= b[hdr - 4 + (i & 3)];
	in_.erase(0, hdr + len);
	return 1;
}

/* see client.h */
s16 WebSocket::read(std::string &msg, s16 timeout_ms)
{
	if (fd_ < 0)
		return -1;
	for (;;) {
		s16 opcode, r;
		bool fin, timed_out;
		std::string payload;

		while ((r = frame(opcode, fin, payload)) == 1) {
			switch (opcode) {
			case 0x0:       /* the rest of a message */
			case 0x1:       /* text */
			case 0x2:       /* binary: as text */
				partial_ += payload;
				if (fin) {
					msg.swap(partial_);
					partial_.clear();
					return 1;
				}
				break;
			case 0x8:       /* close: ours back */
				send_frame(0x8, payload.substr(0, 2));
				close();
				return -1;
			case 0x9:       /* ping */
				if (!send_frame(0xa, payload)) {
					close();
					return -1;
				}
				break;
			default:        /* pong, and the rest */
				break;
			}
		}
		if (r < 0) {
			close();
			return -1;
		}
		ssize_t n = recv_some(fd_, in_, timeout_ms, &timed_out);
		if (timed_out)
			return 0;
		if (n <= 0) {
			close();
			return -1;
		}
	}
}

} // namespace ari
