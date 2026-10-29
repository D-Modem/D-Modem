/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pty.cpp - the DTE's side of a modem program (pty.h): a pseudo
 *              terminal and its slave's group, mode and symbolic link;
 *              the DTE's characters in and the modem's out, its DTR and
 *              its hang-up; and the link's keeper, a process that keeps
 *              root's rights for the link alone
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <grp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifndef __HAIKU__
#include <sys/syscall.h>
#endif
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <dcore.h>
#include <log.h>

#include "dm_types.h"
#include "portable.h"
#include "pty.h"
#include "reinit.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "pty: " fmt, ##__VA_ARGS__)

namespace dmd {

/* the keeper's answer to a pty it was given: each step's errno, 0 if it
   was done or not asked for */
struct keeper_answer {
	int group;
	int mode;
	int link;
};

namespace {

/* the most octets one read of the DTE's takes */
const s16 READ_MAX = 4096;

/* after the DTE closed the slave: the turns the master is left out of
   select() off hook, and on hook */
const u16 CLOSED_PAUSE_OFF_HOOK = 100;
const u16 CLOSED_PAUSE_ON_HOOK = 1;

/* the keeper's socket, and the most descriptors it closes one by one
   (when the kernel has no close_range) */
const int KEEPER_FD = 3;
const s32 KEEPER_FD_SCAN = 65536;

/* a request's octet: the link as well, or only the group and mode */
const char ASK_LINK = 'L';
const char ASK_SLAVE = 'S';
/* the keeper's first word: it holds nothing of ours but its socket */
const char KEEPER_READY = 'R';

/* what the keeper works with, in fixed buffers filled before the fork:
   the group (if it is known) and mode for each slave, the link, and the
   slave the link was last made to */
struct KeeperSetup {
	bool have_gid;
	gid_t gid;
	mode_t perm;
	char link[PATH_MAX];
	char target[PATH_MAX];
};

/* the slave at path given group gid (if have_gid) and mode perm (if not
   0), and the symbolic link link made to it (if link is not null; what
   was there removed): each step's errno into *a.  Async-signal-safe:
   the keeper runs it */
void setup_slave(const char *path, bool have_gid, gid_t gid, mode_t perm,
		 const char *link, keeper_answer *a)
{
	a->group = a->mode = a->link = 0;
	if (have_gid && chown(path, static_cast<uid_t>(-1), gid) < 0)
		a->group = errno;
	if (perm && chmod(path, perm) < 0)
		a->mode = errno;
	if (link) {
		unlink(link);
		if (symlink(path, link) < 0)
			a->link = errno;
	}
}

/* link removed if it still points at target (not if another program
   has put something else there since).  Async-signal-safe */
void remove_link(const char *link, const char *target)
{
	char buf[PATH_MAX];
	ssize_t n = readlink(link, buf, sizeof(buf) - 1);

	if (n < 0)
		return;
	buf[n] = '\0';
	if (!strcmp(buf, target))
		unlink(link);
}

/* the path of the slave of the master fd into path (size octets): 0,
   or an errno.  ptsname_r(), or where there is none (Haiku) ptsname()'s
   copy: only the keeper, single threaded, and open() call it */
int pts_name(int fd, char *path, size_t size)
{
#ifdef __HAIKU__
	const char *p = ptsname(fd);

	if (!p)
		return errno ? errno : EINVAL;
	if (strlen(p) >= size)
		return ERANGE;
	memcpy(path, p, strlen(p) + 1);
	return 0;
#else
	return ptsname_r(fd, path, size);
#endif
}

/* every descriptor from first up closed.  Async-signal-safe */
void close_from(int first)
{
	struct rlimit rl;
	s32 last = KEEPER_FD_SCAN;

#ifdef SYS_close_range
	if (syscall(SYS_close_range, static_cast<unsigned>(first), ~0U, 0U) ==
	    0)
		return;
#endif
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
	    rl.rlim_cur < static_cast<rlim_t>(last))
		last = static_cast<s32>(rl.rlim_cur);
	for (s32 fd = first; fd < last; fd++)
		close(static_cast<int>(fd));
}

/* the keeper's descriptors: its socket sock moved to KEEPER_FD, 0 to 2
   on /dev/null (closed if it can't be opened), none other.  false if
   the socket can't be moved */
bool keeper_descriptors(int sock)
{
	int null;

	if (sock != KEEPER_FD) {
		if (dup2(sock, KEEPER_FD) < 0)
			return false;
		close(sock);
	}
	null = open("/dev/null", O_RDWR);
	for (int fd = 0; fd < 3; fd++) {
		if (null < 0)
			close(fd);
		else if (fd != null)
			dup2(null, fd);
	}
	close_from(KEEPER_FD + 1);
	return true;
}

/* the keeper's request: an octet (ASK_LINK or ASK_SLAVE) into *op and
   the descriptor passed with it into *fd (-1 if none); the octets
   received (0 at the end of the socket), or -1 */
ssize_t keeper_receive(char *op, int *fd)
{
	struct iovec iov = { op, 1 };
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} ctl;
	struct msghdr msg;
	struct cmsghdr *c;
	ssize_t n;

	dsp::zero(ctl);
	dsp::zero(msg);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctl.buf;
	msg.msg_controllen = sizeof(ctl.buf);
	*fd = -1;
#ifdef MSG_CMSG_CLOEXEC
	n = recvmsg(KEEPER_FD, &msg, MSG_CMSG_CLOEXEC);
#else
	n = recvmsg(KEEPER_FD, &msg, 0);
#endif
	c = n > 0 ? CMSG_FIRSTHDR(&msg) : nullptr;
	if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
	    c->cmsg_len >= CMSG_LEN(sizeof(int))) {
		memcpy(fd, CMSG_DATA(c), sizeof(int));
#ifndef MSG_CMSG_CLOEXEC
		fcntl(*fd, F_SETFD, FD_CLOEXEC);
#endif
	}
	return n;
}

/* the keeper (the child of Pty::fork_keeper()), on its socket sock: it
   holds nothing else, then serves each pty it is passed until the
   socket ends, and removes the link if it is still its own.  Only
   async-signal-safe calls: the program it was forked from may have had
   anything locked */
[[noreturn]] void keeper_run(int sock, KeeperSetup &k)
{
	static const int ignored[] = {
		SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE
	};
	struct sigaction sa;

	dsp::zero(sa);
	sa.sa_handler = SIG_IGN;
	for (int sig : ignored)
		sigaction(sig, &sa, nullptr);
	if (!keeper_descriptors(sock) ||
	    send(KEEPER_FD, &KEEPER_READY, 1, dmd::SEND_NOSIGNAL) != 1)
		_exit(1);
	for (;;) {
		char op, path[PATH_MAX];
		keeper_answer a = {};
		int fd, err;
		ssize_t n = keeper_receive(&op, &fd);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		/* the slave of a master only: nothing else of the system's
		   is ours to touch */
		err = fd < 0 ? EBADF : pts_name(fd, path, sizeof(path));
		if (fd >= 0)
			close(fd);
		if (err) {
			a.link = err;
		} else {
			bool link = op == ASK_LINK;
			setup_slave(path, k.have_gid, k.gid, k.perm & 0777,
				    link ? k.link : nullptr, &a);
			if (link && !a.link)
				memcpy(k.target, path, strlen(path) + 1);
		}
		if (send(KEEPER_FD, &a, sizeof(a), dmd::SEND_NOSIGNAL) < 0 &&
		    errno != EINTR)
			break;
	}
	remove_link(k.link, k.target);
	_exit(0);
}

/* a new pty: its master (non-blocking), its slave's path into path; the
   settings *t if keep, else its own made raw at 115200 bit/s, and in *t
   what it has then.  -1 (reported) if it can't be had */
int new_master(struct termios *t, bool keep, char *path, size_t size)
{
	const char *failed = nullptr;
	int fd, flags;

	fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (fd < 0) {
		ERR("cannot open a pty: %s\n", strerror(errno));
		return -1;
	}
	if (grantpt(fd) < 0 || unlockpt(fd) < 0) {
		failed = "unlock";
	} else if ((errno = pts_name(fd, path, size)) != 0) {
		failed = "name";
	} else if (!keep && tcgetattr(fd, t) < 0) {
		failed = "read the settings of";
	} else {
		if (!keep) {
			cfmakeraw(t);
			cfsetispeed(t, B115200);
			cfsetospeed(t, B115200);
		}
		flags = fcntl(fd, F_GETFL);
		if (tcsetattr(fd, TCSANOW, t) < 0)
			failed = "set up";
		else if (flags < 0 ||
			 fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
			failed = "make non-blocking";
		else
			tcgetattr(fd, t);
	}
	if (failed) {
		ERR("cannot %s the pty: %s\n", failed, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* the terminal settings that matter the same in a and b */
bool same_settings(const struct termios &a, const struct termios &b)
{
	return a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag &&
		a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag &&
		cfgetispeed(&a) == cfgetispeed(&b) &&
		cfgetospeed(&a) == cfgetospeed(&b) &&
		!memcmp(a.c_cc, b.c_cc, sizeof(a.c_cc));
}

} // namespace

/* the master closed, the link removed */
Pty::~Pty()
{
	close();
}

/* the old master closed first, so that the new slave normally has its
   number; then a new pty as the DTE had the last one (or the first one
   raw at 115200 bit/s), and its slave's group, mode and link (what of
   these fails is only reported) */
s16 Pty::open(const char *group, mode_t perm, const char *link)
{
	/* (link may be link_'s own string) */
	std::string want_link = link ? link : "";
	struct termios t = termios_;
	char path[PATH_MAX];
	int fd;

	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
	fd = new_master(&t, have_termios_, path, sizeof(path));
	if (fd < 0)
		return -1;
	fd_ = fd;
	name_ = path;
	group_ = group ? group : "";
	perm_ = perm;
	termios_ = t;
	have_termios_ = true;
	closed_ = false;
	close_count_ = 0;
	DBG("%s opened\n", path);
	set_slave(group, perm, want_link.c_str());
	return 0;
}

/* the slave's group, mode and link: by the keeper if there is one (the
   group, mode and link fork_keeper() took), else here; what failed is
   reported, the link remembered if it was made */
void Pty::set_slave(const char *group, mode_t perm, const char *link)
{
	keeper_answer a = {};
	bool have_gid = false;
	gid_t gid = 0;

	link_ = link ? link : "";
	if (group && *group) {
		struct group *g;
		errno = 0;
		g = getgrnam(group);
		if (g) {
			have_gid = true;
			gid = g->gr_gid;
		} else {
			ERR("cannot find group '%s': %s\n", group,
			    errno ? strerror(errno) : "no such group");
		}
	}
	if (keeper_ >= 0 && keeper_ask(&a) < 0) {
		ERR("the keeper of %s is gone (%s): its pty set up here\n",
		    link_.empty() ? name_.c_str() : link_.c_str(),
		    strerror(errno));
		end_keeper();
	}
	if (keeper_ < 0)
		setup_slave(name_.c_str(), have_gid, gid, perm,
			    link_.empty() ? nullptr : link_.c_str(), &a);
	if (a.group)
		ERR("cannot give %s the group '%s': %s\n", name_.c_str(),
		    group, strerror(a.group));
	if (a.mode)
		ERR("cannot give %s the mode %04o: %s\n", name_.c_str(),
		    (unsigned)perm, strerror(a.mode));
	if (link_.empty())
		return;
	if (a.link) {
		ERR("cannot create the symbolic link '%s' -> '%s': %s\n",
		    link_.c_str(), name_.c_str(), strerror(a.link));
		link_.clear();
		return;
	}
	fprintf(stderr, "symbolic link `%s' -> `%s' created.\n",
		link_.c_str(), name_.c_str());
}

/* the master to the keeper with an octet that says whether the link is
   wanted (link_), its answer into *a; 0, or -1 (errno) if the keeper
   could not be asked or did not answer */
s16 Pty::keeper_ask(struct keeper_answer *a)
{
	char op = link_.empty() ? ASK_SLAVE : ASK_LINK;
	struct iovec iov = { &op, 1 };
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} ctl;
	struct msghdr msg;
	struct cmsghdr *c;
	ssize_t n;

	dsp::zero(ctl);
	dsp::zero(msg);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctl.buf;
	msg.msg_controllen = sizeof(ctl.buf);
	c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd_, sizeof(int));
	do
		n = sendmsg(keeper_, &msg, dmd::SEND_NOSIGNAL);
	while (n < 0 && errno == EINTR);
	if (n != 1)
		return -1;
	do
		n = recv(keeper_, a, sizeof(*a), 0);
	while (n < 0 && errno == EINTR);
	if (n != static_cast<ssize_t>(sizeof(*a))) {
		if (n >= 0)
			errno = ECONNRESET;
		return -1;
	}
	return 0;
}

/* the keeper's socket closed (it removes the link and ends then), the
   keeper waited for */
void Pty::end_keeper()
{
	if (keeper_ >= 0) {
		::close(keeper_);
		keeper_ = -1;
	}
	if (keeper_pid_ > 0) {
		while (waitpid(keeper_pid_, nullptr, 0) < 0 && errno == EINTR)
			;
		keeper_pid_ = -1;
	}
}

/* the master closed; the link removed by the keeper (which ends), or
   here if there is none */
void Pty::close()
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
	if (keeper_ >= 0 || keeper_pid_ > 0)
		end_keeper();
	else if (!link_.empty())
		remove_link(link_.c_str(), name_.c_str());
	link_.clear();
}

/* the keeper forked, with the link, group and mode of the current pty;
   it has closed all it had of ours when this returns 0 */
s16 Pty::fork_keeper()
{
	KeeperSetup k;
	int sv[2];
	pid_t pid;
	char word;
	ssize_t n;

	if (link_.empty() || keeper_ >= 0 || link_.size() >= sizeof(k.link) ||
	    name_.size() >= sizeof(k.target))
		return -1;
	dsp::zero(k);
	memcpy(k.link, link_.c_str(), link_.size() + 1);
	memcpy(k.target, name_.c_str(), name_.size() + 1);
	k.perm = perm_;
	if (!group_.empty()) {
		struct group *g = getgrnam(group_.c_str());
		if (g) {
			k.have_gid = true;
			k.gid = g->gr_gid;
		}
	}
	if (dmd::socketpair_cloexec(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
		ERR("socketpair: %s\n", strerror(errno));
		return -1;
	}
	fflush(nullptr);
	pid = fork();
	if (pid < 0) {
		ERR("fork: %s\n", strerror(errno));
		::close(sv[0]);
		::close(sv[1]);
		return -1;
	}
	if (pid == 0)
		keeper_run(sv[1], k);
	::close(sv[1]);
	/* not on before the keeper holds nothing but its socket (one may
	   look at its descriptors as soon as we go on) */
	do
		n = recv(sv[0], &word, 1, 0);
	while (n < 0 && errno == EINTR);
	if (n != 1 || word != KEEPER_READY) {
		ERR("the keeper of %s did not start\n", link_.c_str());
		::close(sv[0]);
		while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
			;
		return -1;
	}
	keeper_ = sv[0];
	keeper_pid_ = pid;
	DBG("%s: its keeper is process %d\n", link_.c_str(), (int)pid);
	return 0;
}

/* the modem's room, unless the DTE has closed the slave: then a pause
   of some calls first (each call counts) */
bool Pty::want_read(const dcore::Modem &m)
{
	if (closed_) {
		u16 pause = m.started() ? CLOSED_PAUSE_OFF_HOOK :
			CLOSED_PAUSE_ON_HOOK;
		if (close_count_ < pause) {
			close_count_++;
			return false;
		}
	}
	return m.dte_space() > 0;
}

/* the DTE's settings (DTR dropped: speed 0), then up to the modem's
   room of its characters into the modem */
Pty::Read Pty::read(dcore::Modem &m)
{
	struct termios t;
	char buf[READ_MAX];
	s32 room;
	ssize_t n;

	if (tcgetattr(fd_, &t) == 0 && !same_settings(t, termios_)) {
		DBG("%s: the DTE's settings: cflag 0%o, lflag 0%o\n",
		    name_.c_str(), (unsigned)t.c_cflag, (unsigned)t.c_lflag);
		if (cfgetospeed(&t) == B0 || cfgetispeed(&t) == B0) {
			DBG("%s: the DTE dropped DTR\n", name_.c_str());
			if (m.link_state() != dcore::LinkState::Idle)
				m.hangup();
		}
		termios_ = t;
	}
	room = m.dte_space();
	if (room <= 0)
		return AGAIN;
	if (room > READ_MAX)
		room = READ_MAX;
	n = ::read(fd_, buf, room);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return AGAIN;
		if (errno == EIO) {
			/* no one has the slave open: the pause starts over */
			close_count_ = 0;
			if (closed_)
				return AGAIN;
			closed_ = true;
			DBG("%s: the DTE closed it\n", name_.c_str());
			return CLOSED;
		}
		ERR("pty read: %s\n", strerror(errno));
		return ERROR;
	}
	if (n == 0)
		DBG("%s: read 0 octets\n", name_.c_str());
	closed_ = false;
	if (n > 0)
		m.dte_input(buf, static_cast<int>(n));
	return DATA;
}

/* read(); the DTE closing the slave with HUPCL hangs up, and with
   new_pty a new pty takes this one's place */
s16 Pty::serve(dcore::Modem &m, bool new_pty)
{
	switch (read(m)) {
	case ERROR:
		return -1;
	case CLOSED:
		if (!hupcl())
			return 0;
		DBG("%s: closed with HUPCL: hanging up\n", name_.c_str());
		m.hangup();
		if (new_pty && reopen() < 0) {
			ERR("cannot re-create PTY.\n");
			return -1;
		}
		return 0;
	default:
		return 0;
	}
}

/* to the master as much of buf as it takes now */
s32 Pty::write(const char *buf, s32 n)
{
	ssize_t done;

	if (fd_ < 0 || n <= 0)
		return 0;
	done = ::write(fd_, buf, n);
	return done > 0 ? static_cast<s32>(done) : 0;
}

} // namespace dmd
