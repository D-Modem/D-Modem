/*
 *
 *    pty.h - the DTE side of a modem program: a pseudo terminal whose
 *            slave the DTE (a terminal program, pppd) opens
 *
 */
#ifndef SLMODEMD_PTY_H
#define SLMODEMD_PTY_H

#include <sys/types.h>
#include <termios.h>

#include <string>

namespace dcore { class Modem; }

namespace slm {

class Pty {
public:
	Pty() = default;
	~Pty();
	Pty(const Pty &) = delete;
	Pty &operator=(const Pty &) = delete;

	/* a raw pty, 115200 bit/s, non-blocking (a new one keeps the old
	   one's settings); group, perm: its slave's; link: a symbolic link
	   to the slave.  -1 on failure. */
	int open(const char *group = nullptr, mode_t perm = 0,
		 const char *link = nullptr);
	/* a new one, as open() made the last (the DTE hung up) */
	int reopen() {
		std::string g = group_, l = link_;
		return open(g.c_str(), perm_, l.c_str());
	}
	void close();
	int fd() const { return fd_; }
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

	/* the modem's characters to the DTE */
	int write(const char *buf, int n);

private:
	int fd_ = -1;
	std::string name_, link_, group_;
	mode_t perm_ = 0;
	struct termios termios_ = {};
	bool have_termios_ = false;
	bool closed_ = false;
	unsigned close_count_ = 0;
};

} // namespace slm

#endif
