/*
 *
 *    pty.cpp - the DTE's pseudo terminal (pty.h); slmodemd's create_pty()
 *              and the pty part of its main loop, and the process that
 *              sets up each new pty with root's rights once slmodemd has
 *              dropped them (the keeper)
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

/* link removed if it still points at target (not another program's
   since) */
static void remove_link(const char *link, const char *target)
{
	char buf[PATH_MAX];
	ssize_t n = readlink(link, buf, sizeof(buf) - 1);
	if (n < 0)
		return;
	buf[n] = 0;
	if (!strcmp(buf, target))
		unlink(link);
}

/* every descriptor from fd on closed */
static void close_from(int fd)
{
	struct rlimit rl;
	long max = 1024;
#ifdef SYS_close_range
	if (!syscall(SYS_close_range, (unsigned)fd, ~0u, 0))
		return;
#endif
	if (!getrlimit(RLIMIT_NOFILE, &rl))
		max = rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 1 << 20 ?
			1 << 20 : (long)rl.rlim_cur;
	for (; fd < max; fd++)
		::close(fd);
}

/* the keeper's answer: the errno of each step, 0 if done (or not
   asked for) */
struct keeper_answer {
	int group, mode, link;
};

/* the keeper (Pty::fork_keeper()), in the process forked for it: for
   each message on sock, the slave of the master that came with it given
   group gid (-1: left as is) and mode perm (0: left as is), and the link
   pointed at it, a keeper_answer the answer; at the socket's end, the
   link removed if it is still the one made last (made: the slave it
   points at already).  Only system calls once forked: no other thread's
   lock is held here.  Signals from the terminal and SIGTERM are the
   modem's (it ends, then this) */
[[noreturn]] static void keeper(int sock, const char *link, char *made,
				gid_t gid, mode_t perm)
{
	static const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM,
				    SIGPIPE };
	int nul;

	for (int s : sigs)
		signal(s, SIG_IGN);
	/* the socket as 3, the standard ones /dev/null, nothing else (the
	   pty's master least of all: it would keep the pty) */
	if (sock != 3) {
		if (dup2(sock, 3) != 3)
			_exit(1);
		sock = 3;
	}
	nul = open("/dev/null", O_RDWR);
	for (int fd = 0; fd < 3; fd++) {
		if (fd == nul)
			continue;
		if (nul >= 0)
			dup2(nul, fd);
		else
			::close(fd);
	}
	close_from(4);

	for (;;) {
		char c, name[PATH_MAX];
		union {
			struct cmsghdr h;
			char buf[CMSG_SPACE(sizeof(int))];
		} u;
		struct iovec iov = { &c, 1 };
		struct msghdr msg = {};
		struct keeper_answer a = {};
		struct cmsghdr *cm;
		int fd = -1;
		ssize_t n;

		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = u.buf;
		msg.msg_controllen = sizeof(u.buf);
		n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		cm = CMSG_FIRSTHDR(&msg);
		if (cm && cm->cmsg_level == SOL_SOCKET &&
		    cm->cmsg_type == SCM_RIGHTS &&
		    cm->cmsg_len == CMSG_LEN(sizeof(int)))
			memcpy(&fd, CMSG_DATA(cm), sizeof(int));
		if (fd < 0)
			a.link = EINVAL;
		else {
			/* the master's own slave: ENOTTY if it is no
			   pty's master */
			a.link = ptsname_r(fd, name, sizeof(name));
			::close(fd);
		}
		if (!a.link) {
			if (gid != (gid_t)-1 && chown(name, -1, gid))
				a.group = errno;
			if (perm && chmod(name, perm & 0777))
				a.mode = errno;
			unlink(link);
			if (symlink(name, link))
				a.link = errno;
			else
				strcpy(made, name);
		}
		send(sock, &a, sizeof(a), MSG_NOSIGNAL);
	}
	remove_link(link, made);
	_exit(0);
}

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
	if (keeper_ >= 0)
		end_keeper();
	else if (!link_.empty())
		remove_link(link_.c_str(), name_.c_str());
	link_.clear();
}

/* see pty.h */
int Pty::fork_keeper()
{
	char link[PATH_MAX], made[PATH_MAX];
	gid_t gid = (gid_t)-1;
	int sv[2];
	pid_t pid;

	if (link_.empty() || keeper_ >= 0 || link_.size() >= sizeof(link) ||
	    name_.size() >= sizeof(made))
		return -1;
	if (!group_.empty()) {
		struct group *grp = getgrnam(group_.c_str());
		if (grp)
			gid = grp->gr_gid;
	}
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
		ERR("socketpair: %s\n", strerror(errno));
		return -1;
	}
	/* the keeper allocates nothing once forked */
	strcpy(link, link_.c_str());
	strcpy(made, name_.c_str());
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		ERR("fork: %s\n", strerror(errno));
		::close(sv[0]);
		::close(sv[1]);
		return -1;
	}
	if (pid == 0) {
		::close(sv[0]);
		keeper(sv[1], link, made, gid, perm_);
	}
	::close(sv[1]);
	keeper_ = sv[0];
	keeper_pid_ = pid;
	DBG("the pty's group, mode and link `%s' kept by process %ld\n", link,
	    (long)pid);
	return 0;
}

/* the keeper given this master (its slave's group, mode and link):
   0 with its answer in *a, or the errno of the exchange (EPIPE: the
   keeper is gone) */
int Pty::keeper_ask(struct keeper_answer *a)
{
	union {
		struct cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} u = {};
	char c = 'P';
	struct iovec iov = { &c, 1 };
	struct msghdr msg = {};
	struct cmsghdr *cm;
	ssize_t n;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = u.buf;
	msg.msg_controllen = sizeof(u.buf);
	cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd_, sizeof(int));
	do
		n = sendmsg(keeper_, &msg, MSG_NOSIGNAL);
	while (n < 0 && errno == EINTR);
	if (n < 0)
		return errno;
	do
		n = recv(keeper_, a, sizeof(*a), 0);
	while (n < 0 && errno == EINTR);
	if (n == (ssize_t)sizeof(*a))
		return 0;
	return n < 0 ? errno : EPIPE;
}

/* the keeper's socket closed: it removes the link and ends, reaped
   here */
void Pty::end_keeper()
{
	::close(keeper_);
	keeper_ = -1;
	while (waitpid(keeper_pid_, nullptr, 0) < 0 && errno == EINTR)
		;
	keeper_pid_ = -1;
}

/* the slave's group and mode, and link to it, as open() has them: set
   here, or by the keeper while there is one; what can't be done is
   logged, a link not made forgotten */
void Pty::set_slave(const char *group, mode_t perm, const char *link)
{
	struct keeper_answer a = {};
	gid_t gid = (gid_t)-1;
	int err;

	if (group && *group) {
		struct group *grp = getgrnam(group);
		if (!grp)
			ERR("cannot find group '%s': %s\n", group,
			    strerror(errno));
		else
			gid = grp->gr_gid;
	}
	if (keeper_ >= 0 && (err = keeper_ask(&a))) {
		ERR("the pty's keeper: %s\n", strerror(err));
		end_keeper();
		a = {};
	}
	if (keeper_ < 0) {
		if (gid != (gid_t)-1 && chown(name_.c_str(), -1, gid) < 0)
			a.group = errno;
		if (perm && chmod(name_.c_str(), perm) < 0)
			a.mode = errno;
		if (link && *link) {
			unlink(link);
			if (symlink(name_.c_str(), link))
				a.link = errno;
		}
	}
	if (a.group)
		ERR("cannot chown '%s' to ':%s': %s\n", name_.c_str(), group,
		    strerror(a.group));
	if (a.mode)
		ERR("cannot chmod '%s' to %o: %s\n", name_.c_str(), perm,
		    strerror(a.mode));
	if (!link || !*link)
		return;
	if (a.link) {
		ERR("cannot create symbolink link `%s' -> `%s': %s\n", link,
		    name_.c_str(), strerror(a.link));
		link_.clear();
	} else {
		INFO("symbolic link `%s' -> `%s' created.\n", link,
		     name_.c_str());
		link_ = link;
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

	set_slave(group, perm, link);
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
