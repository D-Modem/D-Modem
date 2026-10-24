/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    pty.h - the DTE side of a modem program: a pseudo terminal whose
 *            slave the DTE (a terminal program, pppd) opens
 *
 */
#ifndef DMODEMD_PTY_H
#define DMODEMD_PTY_H

#include <sys/types.h>
#include <termios.h>

#include <string>

#include "dm_types.h"

namespace dcore { class Modem; }

namespace dmd {

struct keeper_answer;

class Pty {
public:
	Pty() = default;
	/* close() */
	~Pty();
	Pty(const Pty &) = delete;
	Pty &operator=(const Pty &) = delete;

	/* a raw pty, 115200 bit/s, non-blocking (a new one keeps the old
	   one's settings); group, perm: its slave's; link: a symbolic link
	   to the slave.  -1 on failure. */
	s16 open(const char *group = nullptr, mode_t perm = 0,
		 const char *link = nullptr);
	/* a new one, as open() made the last (the DTE hung up) */
	s16 reopen() {
		std::string g = group_, l = link_;
		return open(g.c_str(), perm_, l.c_str());
	}
	/* the master closed, the symbolic link removed (by the keeper,
	   fork_keeper(), which ends then) */
	void close();
	/* from now on the slave's group and mode, and the link to it, set
	   by a process forked to keep this one's rights (root's:
	   drop_privileges() comes next), with no other descriptor than its
	   socket to this one: for each new pty (reopen()) it is passed the
	   master, and it removes the link when the socket closes, at
	   close() or this process's end, even killed.  Called before any
	   thread is made.  0, or -1 if there is no link or no keeper (all
	   done here as before then) */
	s16 fork_keeper();
	/* the master, -1 if none */
	int fd() const { return fd_; }
	/* the slave's path */
	const char *name() const { return name_.c_str(); }
	/* the symbolic link, if open() made one */
	const char *link() const { return link_.c_str(); }
	/* the DTE hangs up when it closes the slave */
	bool hupcl() const { return termios_.c_cflag & HUPCL; }

	/* select() on it now?  Not when the modem has no room, nor for a
	   while after the DTE closed the slave */
	bool want_read(const dcore::Modem &m);

	enum Read { DATA, AGAIN, CLOSED, ERROR };
	/* it is readable: the DTE's settings (dropping DTR hangs up), then
	   its characters into the modem.  CLOSED: the DTE has just closed
	   the slave. */
	Read read(dcore::Modem &m);
	/* it is readable: read(), and the DTE closing the slave with HUPCL
	   hangs m up (with new_pty, a new pty then stands in for this one,
	   as the next line would).  -1 on an error */
	s16 serve(dcore::Modem &m, bool new_pty = false);

	/* the modem's characters to the DTE */
	s32 write(const char *buf, s32 n);

private:
	int fd_ = -1;
	std::string name_, link_, group_;
	mode_t perm_ = 0;
	struct termios termios_ = {};
	bool have_termios_ = false;
	bool closed_ = false;
	u16 close_count_ = 0;
	/* the keeper (fork_keeper()): the socket to it, its pid */
	int keeper_ = -1;
	pid_t keeper_pid_ = -1;

	void set_slave(const char *group, mode_t perm, const char *link);
	s16 keeper_ask(struct keeper_answer *a);
	void end_keeper();
};

} // namespace dmd

#endif
