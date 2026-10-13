/*
 *
 *    pty.cpp - the DTE's pseudo terminal (pty.h); slmodemd's create_pty()
 *              and the pty part of its main loop
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dcore.h>
#include <log.h>

#include "pty.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define INFO(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "main: " fmt, ##__VA_ARGS__)

namespace slm {

/* the DTE closed the slave: not read again for so many loops */
static const unsigned CLOSE_COUNT_MAX = 100;

/* the master closed, the link removed */
Pty::~Pty()
{
	close();
}

/* see pty.h (open() makes them again) */
void Pty::close()
{
	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	if (!link_.empty()) {
		unlink(link_.c_str());
		link_.clear();
	}
}

/* see pty.h; an old master is closed first.  A group, mode or link
   that can't be set is only logged */
int Pty::open(const char *group, mode_t perm, const char *link)
{
	struct termios t;
	int pty;

	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty < 0 || grantpt(pty) < 0 || unlockpt(pty) < 0) {
		ERR("getpt: %s\n", strerror(errno));
		if (pty >= 0)
			::close(pty);
		return -1;
	}
	if (have_termios_)
		t = termios_;
	else {
		tcgetattr(pty, &t);
		/* non canonical raw tty */
		cfmakeraw(&t);
		cfsetispeed(&t, B115200);
		cfsetospeed(&t, B115200);
	}
	if (tcsetattr(pty, TCSANOW, &t)) {
		ERR("tcsetattr: %s\n", strerror(errno));
		::close(pty);
		return -1;
	}
	fcntl(pty, F_SETFL, O_NONBLOCK);
	fd_ = pty;
	name_ = ptsname(pty);
	group_ = group ? group : "";
	perm_ = perm;
	termios_ = t;
	have_termios_ = true;
	closed_ = false;
	close_count_ = 0;

	if (group && *group) {
		struct group *grp = getgrnam(group);
		if (!grp)
			ERR("cannot find group '%s': %s\n", group,
			    strerror(errno));
		else if (chown(name_.c_str(), -1, grp->gr_gid) < 0)
			ERR("cannot chown '%s' to ':%s': %s\n", name_.c_str(),
			    group, strerror(errno));
	}
	if (perm && chmod(name_.c_str(), perm) < 0)
		ERR("cannot chmod '%s' to %o: %s\n", name_.c_str(), perm,
		    strerror(errno));
	if (link && *link) {
		unlink(link);
		if (symlink(name_.c_str(), link)) {
			ERR("cannot create symbolink link `%s' -> `%s': %s\n",
			    link, name_.c_str(), strerror(errno));
			link_.clear();
		} else {
			INFO("symbolic link `%s' -> `%s' created.\n", link,
			     name_.c_str());
			link_ = link;
		}
	}
	return 0;
}

/* see pty.h: after the DTE closed the slave, not for CLOSE_COUNT_MAX
   loops while off hook, nor for one loop on hook */
bool Pty::want_read(const dcore::Modem &m)
{
	if (closed_ && close_count_) {
		if (!m.started() || ++close_count_ > CLOSE_COUNT_MAX)
			close_count_ = 0;
		return false;
	}
	return m.dte_space() > 0;
}

/* see pty.h: up to the core's room (4096 at most); EIO (no slave open)
   is CLOSED the first time, AGAIN after */
Pty::Read Pty::read(dcore::Modem &m)
{
	struct termios t;
	char buf[4096];
	int count;

	/* the DTE's settings: a speed of 0 drops DTR */
	tcgetattr(fd_, &t);
	if (memcmp(&t, &termios_, sizeof(t))) {
		DBG("termios changed.\n");
		if (cfgetispeed(&t) == B0 || cfgetospeed(&t) == B0) {
			DBG("modem_update_termios: hangup.\n");
			if (m.link_state() != dcore::LinkState::Idle)
				m.hangup();
		}
		termios_ = t;
	}
	count = m.dte_space();
	if (count == 0)
		return AGAIN;
	if (count > (int)sizeof(buf))
		count = sizeof(buf);
	count = ::read(fd_, buf, count);
	if (count < 0) {
		if (errno == EAGAIN)
			return AGAIN;
		if (errno == EIO) {
			bool first = !closed_;
			if (first)
				DBG("pty closed.\n");
			closed_ = true;
			close_count_ = 1;
			return first ? CLOSED : AGAIN;
		}
		ERR("pty read: %s\n", strerror(errno));
		return ERROR;
	}
	if (count == 0)
		DBG("pty read = 0\n");
	closed_ = false;
	m.dte_input(buf, count);
	return DATA;
}

/* what the master took, 0 if nothing (full, or an error) */
int Pty::write(const char *buf, int n)
{
	int ret = ::write(fd_, buf, n);
	return ret < 0 ? 0 : ret;
}

} // namespace slm
