/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    rcapi_server.cpp - the controller's applications over TCP
 *                       (rcapi_server.h)
 *
 */

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "rcapi_server.h"

namespace capi {

namespace {

/* a message on a connection at most (a DATA_B3 of 2048 and more) */
const size_t FRAME_MAX = 8192;

/* CLOCK_MONOTONIC in ms */
u64 now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<u64>(ts.tv_sec) * 1000 +
		static_cast<u64>(ts.tv_nsec) / 1000000;
}

/* fd non-blocking */
void nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* a struct of 64 octets: s, the rest zeros */
std::vector<u8> struct64(const char *s)
{
	std::vector<u8> v(65, 0);
	v[0] = 64;
	memcpy(v.data() + 1, s, std::min<size_t>(strlen(s), 63));
	return v;
}

} // namespace

/* see rcapi_server.h */
Server::Server(int modem_fd, const std::string &init_extra)
	: modem_fd_(modem_fd), ctrl_(*this, init_extra)
{
}

/* (the connections, the socket and the modem closed) */
Server::~Server()
{
	for (auto &c : conns_)
		close(c.first);
	if (listen_fd_ >= 0)
		close(listen_fd_);
	if (modem_fd_ >= 0)
		close(modem_fd_);
}

/* see rcapi_server.h */
int Server::listen(const char *addr, int port)
{
	struct addrinfo hints, *res = nullptr;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	std::string p = std::to_string(port);
	if (getaddrinfo(addr, p.c_str(), &hints, &res) || !res)
		return -1;
	int fd = socket(res->ai_family, SOCK_STREAM, 0);
	int on = 1;
	if (fd < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) ||
	    bind(fd, res->ai_addr, res->ai_addrlen) || ::listen(fd, 8)) {
		if (fd >= 0)
			close(fd);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);
	nonblock(fd);
	struct sockaddr_storage a;
	socklen_t len = sizeof(a);
	getsockname(fd, reinterpret_cast<struct sockaddr *>(&a), &len);
	listen_fd_ = fd;
	if (a.ss_family == AF_INET6)
		return ntohs(reinterpret_cast<struct sockaddr_in6 *>(&a)
			     ->sin6_port);
	return ntohs(reinterpret_cast<struct sockaddr_in *>(&a)->sin_port);
}

/* see rcapi_server.h */
bool Server::step(int timeout_ms)
{
	u64 now = now_ms();
	ctrl_.poll(now);
	s64 next = ctrl_.next_ms(now);
	if (next >= 0 && (timeout_ms < 0 || next < timeout_ms))
		timeout_ms = static_cast<int>(next);
	std::vector<struct pollfd> fds;
	fds.push_back({ modem_fd_,
			static_cast<short>(ctrl_.wants_modem_input() ?
					   POLLIN : 0), 0 });
	if (listen_fd_ >= 0)
		fds.push_back({ listen_fd_, POLLIN, 0 });
	for (auto &c : conns_)
		fds.push_back({ c.first, static_cast<short>(
				POLLIN | (c.second.out.empty() ? 0 : POLLOUT)),
				0 });
	if (poll(fds.data(), fds.size(), timeout_ms) < 0 && errno != EINTR)
		return false;
	now = now_ms();
	if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
		u8 buf[4096];
		ssize_t n = read(modem_fd_, buf, sizeof(buf));
		if (n > 0)
			ctrl_.modem_input(buf, static_cast<size_t>(n), now);
		else if (n == 0 || (errno != EAGAIN && errno != EINTR))
			return false;
	}
	for (size_t k = 1; k < fds.size(); k++) {
		if (!fds[k].revents)
			continue;
		if (fds[k].fd == listen_fd_) {
			accept_conn(now);
			continue;
		}
		auto it = conns_.find(fds[k].fd);
		if (it == conns_.end())
			continue;
		if (fds[k].revents & POLLOUT)
			flush(it->first, it->second);
		if ((fds[k].revents & (POLLIN | POLLHUP | POLLERR)) &&
		    !read_conn(it->first, it->second, now))
			close_conn(fds[k].fd, now);
	}
	ctrl_.poll(now);
	return true;
}

/* a new connection: an application once it registers */
void Server::accept_conn(u64 now)
{
	(void)now;
	int fd = accept(listen_fd_, nullptr, nullptr);
	if (fd < 0)
		return;
	nonblock(fd);
	int on = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	conns_[fd];
}

/* what the connection has sent, each whole message handled; false
   when it is closed or sends nonsense */
bool Server::read_conn(int fd, Conn &c, u64 now)
{
	u8 buf[4096];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		return false;
	if (n > 0)
		c.in.insert(c.in.end(), buf, buf + n);
	while (c.in.size() >= 2) {
		size_t len = static_cast<size_t>(c.in[0] << 8 | c.in[1]);
		if (len < 2 + 8 || len > FRAME_MAX)
			return false;
		if (c.in.size() < len)
			break;
		Msg m;
		bool ok = decode(c.in.data() + 2, len - 2, m);
		std::vector<u8> rest(c.in.begin() + len, c.in.end());
		c.in.swap(rest);
		if (!ok)
			return false;
		handle(fd, c, m, now);
		if (!conns_.count(fd))
			return true;
	}
	return true;
}

/* a message of the connection's: RCAPI's own, or the controller's */
void Server::handle(int fd, Conn &c, const Msg &m, u64 now)
{
	Reader r(m.params);
	std::vector<u8> p;
	switch (m.command()) {
	case RCAPI_REGISTER_REQ: {
		r.d();
		r.w();
		r.w();
		u16 blocks = r.w(), len = r.w();
		if (!c.registered) {
			c.appl = next_appl_++;
			if (!next_appl_)
				next_appl_ = 1;
			c.registered = true;
			ctrl_.add_app(c.appl, blocks, len);
		}
		Writer(p).w(NO_ERROR);
		reply(fd, c, m, RCAPI_REGISTER_CONF, p);
		return;
	}
	case RCAPI_GET_MANUFACTURER_REQ:
		reply(fd, c, m, RCAPI_GET_MANUFACTURER_CONF, struct64("D-Modem"));
		return;
	case RCAPI_GET_SERIAL_NUMBER_REQ:
		reply(fd, c, m, RCAPI_GET_SERIAL_NUMBER_CONF, struct64("0000001"));
		return;
	case RCAPI_GET_VERSION_REQ:
		/* (libcapi20 takes CAPI's four dwords from the second
		   octet on) */
		Writer(p).b(16).d(2).d(0).d(1).d(0);
		reply(fd, c, m, RCAPI_GET_VERSION_CONF, p);
		return;
	case RCAPI_GET_PROFILE_REQ: {
		u32 ctrl = r.d();
		if (ctrl == 0) {
			Writer(p).w(NO_ERROR).w(1);
			p.resize(66, 0);
		} else if (ctrl == CONTROLLER) {
			Writer(p).w(NO_ERROR);
			std::vector<u8> prof = profile();
			p.insert(p.end(), prof.begin(), prof.end());
		} else {
			Writer(p).w(ILLEGAL_CONTROLLER);
			p.resize(66, 0);
		}
		reply(fd, c, m, RCAPI_GET_PROFILE_CONF, p);
		return;
	}
	case RCAPI_AUTH_USER_REQ:
		Writer(p).w(0).w(0x19).w(0);
		reply(fd, c, m, RCAPI_AUTH_USER_CONF, p);
		return;
	default:
		break;
	}
	if (!c.registered)
		return;
	Msg a = m;
	a.appl = c.appl;
	ctrl_.message(a, now);
}

/* a CONF of RCAPI's: the request's header with cmd, then payload */
void Server::reply(int fd, Conn &c, const Msg &req, u16 cmd,
		   const std::vector<u8> &payload)
{
	Msg m;
	m.appl = req.appl;
	m.cmd = static_cast<u8>(cmd >> 8);
	m.sub = static_cast<u8>(cmd);
	m.num = req.num;
	m.params = payload;
	std::vector<u8> b = encode(m);
	size_t len = b.size() + 2;
	c.out.push_back(static_cast<u8>(len >> 8));
	c.out.push_back(static_cast<u8>(len));
	c.out.insert(c.out.end(), b.begin(), b.end());
	flush(fd, c);
}

/* see rcapi_server.h */
void Server::to_app(const Msg &m)
{
	for (auto &e : conns_) {
		Conn &c = e.second;
		if (!c.registered || c.appl != m.appl)
			continue;
		bool ind = m.cmd == DATA_B3 && m.sub == IND;
		std::vector<u8> b = encode(m, ind ? 30 : 0);
		size_t len = b.size() + 2;
		c.out.push_back(static_cast<u8>(len >> 8));
		c.out.push_back(static_cast<u8>(len));
		c.out.insert(c.out.end(), b.begin(), b.end());
		flush(e.first, c);
		return;
	}
}

/* see rcapi_server.h */
size_t Server::to_modem(const u8 *p, size_t n)
{
	ssize_t w = write(modem_fd_, p, n);
	return w > 0 ? static_cast<size_t>(w) : 0;
}

/* the connection's output, as much as it takes now */
void Server::flush(int fd, Conn &c)
{
	while (!c.out.empty()) {
		ssize_t n = write(fd, c.out.data(), c.out.size());
		if (n <= 0)
			return;
		c.out.erase(c.out.begin(), c.out.begin() + n);
	}
}

/* the connection gone, its application with it */
void Server::close_conn(int fd, u64 now)
{
	auto it = conns_.find(fd);
	if (it == conns_.end())
		return;
	u16 appl = it->second.registered ? it->second.appl : 0;
	conns_.erase(it);
	close(fd);
	if (appl)
		ctrl_.remove_app(appl, now);
}

/* see rcapi_server.h */
int open_modem(const std::string &where)
{
	if (where.compare(0, 4, "tcp:") == 0) {
		std::string hp = where.substr(4);
		size_t colon = hp.rfind(':');
		if (colon == std::string::npos)
			return -1;
		std::string host = hp.substr(0, colon);
		std::string port = hp.substr(colon + 1);
		struct addrinfo hints, *res = nullptr;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) ||
		    !res)
			return -1;
		int fd = socket(res->ai_family, SOCK_STREAM, 0);
		if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen)) {
			close(fd);
			fd = -1;
		}
		freeaddrinfo(res);
		if (fd >= 0) {
			int on = 1;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on,
				   sizeof(on));
			nonblock(fd);
		}
		return fd;
	}
	int fd = open(where.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	struct termios t;
	if (isatty(fd) && tcgetattr(fd, &t) == 0) {
		/* (raw, as cfmakeraw() has it, which not every system has) */
		t.c_iflag &= ~static_cast<tcflag_t>(IGNBRK | BRKINT | PARMRK |
						    ISTRIP | INLCR | IGNCR |
						    ICRNL | IXON);
		t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
		t.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON |
						    ISIG | IEXTEN);
		t.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB);
		t.c_cflag |= CS8 | CLOCAL | CREAD;
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &t);
	}
	return fd;
}

} // namespace capi
