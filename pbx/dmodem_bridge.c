/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 * dmodem_bridge.c - the modem call of the PBX modules on libdmodem's C
 *                   API (see dmodem_bridge.h)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* posix_openpt(), ptsname_r() */
#endif

#include "dmodem_bridge.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* a line for the PBX's log */
static void log_line(const struct dmbridge_host *h, void *call,
		     enum dmbridge_level level, const char *fmt, ...)
	__attribute__ ((format (printf, 4, 5)));
static void log_line(const struct dmbridge_host *h, void *call,
		     enum dmbridge_level level, const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	h->log(call, level, line);
}

/* src into dst of size n, cut to fit */
static void copy(char *dst, const char *src, size_t n)
{
	snprintf(dst, n, "%s", src);
}

/*
 *    the profile
 */

/* (see dmodem_bridge.h) */
int dmbridge_option_is(const char *name, const char *key)
{
	for (;; name++, key++) {
		int a = tolower((unsigned char)*name),
			b = tolower((unsigned char)*key);
		if (a == '_')
			a = '-';
		if (b == '_')
			b = '-';
		if (a != b)
			return 0;
		if (!a)
			return 1;
	}
}

/* (see dmodem_bridge.h) */
void dmbridge_fax_defaults(struct dmbridge_fax *f, const char *dir)
{
	f->on = 1;
	copy(f->dir, dir, sizeof(f->dir));
	f->ident[0] = 0;
	f->max_bps = 14400;
	f->ecm = 1;
	f->t38 = 1;
}

/* (see dmodem_bridge.h) */
int dmbridge_fax_option(const struct dmbridge_host *h,
			struct dmbridge_fax *f, const char *name,
			const char *value)
{
	if (dmbridge_option_is(name, "fax"))
		f->on = h->is_true(value);
	else if (dmbridge_option_is(name, "fax-dir"))
		copy(f->dir, value, sizeof(f->dir));
	else if (dmbridge_option_is(name, "fax-ident"))
		copy(f->ident, value, sizeof(f->ident));
	else if (dmbridge_option_is(name, "fax-max-rate"))
		f->max_bps = atoi(value);
	else if (dmbridge_option_is(name, "fax-ecm"))
		f->ecm = h->is_true(value);
	else if (dmbridge_option_is(name, "fax-t38"))
		f->t38 = h->is_true(value);
	else
		return 0;
	return 1;
}

/* (see dmodem_bridge.h) */
void dmbridge_profile_init(struct dmbridge_profile *p)
{
	memset(p, 0, sizeof(*p));
	p->mode = DMBRIDGE_TCP;
	copy(p->answer, "ATA", sizeof(p->answer));
	p->timeout = 60;
}

/* (see dmodem_bridge.h) */
int dmbridge_profile_option(const struct dmbridge_host *h,
			    struct dmbridge_profile *p, const char *where,
			    const char *name, const char *value)
{
	if (dmbridge_fax_option(h, &p->fax, name, value))
		return 1;
	if (dmbridge_option_is(name, "mode")) {
		if (!strcasecmp(value, "tcp"))
			p->mode = DMBRIDGE_TCP;
		else if (!strcasecmp(value, "unix"))
			p->mode = DMBRIDGE_UNIX;
		else if (!strcasecmp(value, "exec"))
			p->mode = DMBRIDGE_EXEC;
		else
			log_line(h, NULL, DMBRIDGE_WARNING,
				 "%s: unknown mode %s", where, value);
	} else if (dmbridge_option_is(name, "host")) {
		copy(p->host, value, sizeof(p->host));
	} else if (dmbridge_option_is(name, "port")) {
		copy(p->port, value, sizeof(p->port));
	} else if (dmbridge_option_is(name, "path")) {
		if (strlen(value) >= sizeof(p->path))
			log_line(h, NULL, DMBRIDGE_WARNING,
				 "%s: path too long", where);
		copy(p->path, value, sizeof(p->path));
	} else if (dmbridge_option_is(name, "header")) {
		p->header = h->is_true(value);
	} else if (dmbridge_option_is(name, "command")) {
		copy(p->command, value, sizeof(p->command));
	} else if (dmbridge_option_is(name, "init")) {
		if (!strncasecmp(value, "AT", 2))
			value += 2;
		copy(p->init, value, sizeof(p->init));
	} else if (dmbridge_option_is(name, "answer")) {
		/* a command line, AT and all (A alone is ATA; none: ATA) */
		if (!strncasecmp(value, "AT", 2))
			value += 2;
		snprintf(p->answer, sizeof(p->answer), "AT%s",
			 *value ? value : "A");
	} else if (dmbridge_option_is(name, "law")) {
		if (!strcasecmp(value, "ulaw"))
			p->law = DMODEM_LAW_ULAW;
		else if (!strcasecmp(value, "alaw"))
			p->law = DMODEM_LAW_ALAW;
		else
			p->law = DMODEM_LAW_UNKNOWN;
	} else if (dmbridge_option_is(name, "timeout")) {
		p->timeout = atoi(value);
	} else {
		return 0;
	}
	return 1;
}

/* (see dmodem_bridge.h) */
int dmbridge_profile_check(const struct dmbridge_host *h,
			   const struct dmbridge_profile *p,
			   const char *where)
{
	if (p->mode == DMBRIDGE_TCP && (!*p->host || !*p->port)) {
		log_line(h, NULL, DMBRIDGE_ERROR,
			 "%s: mode tcp needs host and port", where);
		return -1;
	}
	if (p->mode == DMBRIDGE_UNIX && !*p->path) {
		log_line(h, NULL, DMBRIDGE_ERROR, "%s: mode unix needs path",
			 where);
		return -1;
	}
	if (p->mode == DMBRIDGE_EXEC && !*p->command) {
		log_line(h, NULL, DMBRIDGE_ERROR, "%s: mode exec needs command",
			 where);
		return -1;
	}
	return 0;
}

/*
 *    descriptors
 */

/* O_NONBLOCK on fd; -1 on failure */
static int nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* what fd has, read and thrown away */
static void drain(int fd)
{
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

/* move what is there from one fd to the other through b (from -1: only
   what b holds); -1 when the source is closed */
static int relay(const struct dmbridge_host *h, int from, int to,
		 struct dmbridge_pipe *b)
{
	int n, eof = 0;

	if (from >= 0 && b->len < (int)sizeof(b->data)) {
		n = read(from, b->data + b->len, sizeof(b->data) - b->len);
		if (n > 0)
			b->len += n;
		else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			eof = 1;        /* a pty master reads EIO at the end */
			log_line(h, NULL, DMBRIDGE_DEBUG, "fd %d read %d (%s)",
				 from, n, n ? strerror(errno) : "end of file");
		}
	}
	if (b->len > 0) {
		n = write(to, b->data, b->len);
		if (n > 0) {
			memmove(b->data, b->data + n, b->len - n);
			b->len -= n;
		}
	}
	return eof ? -1 : 0;
}

/*
 *    the TCP server
 */

/* host:port, the first address that takes it (a blocking connect, in
   tcp_thread()), non-blocking once connected; -1 on failure */
static int connect_tcp(const struct dmbridge_host *h, const char *host,
		       const char *port)
{
	struct addrinfo hints = { 0 }, *res, *ai;
	int fd = -1, err;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	err = getaddrinfo(host, port, &hints, &res);
	if (err) {
		log_line(h, NULL, DMBRIDGE_WARNING, "%s: %s", host,
			 gai_strerror(err));
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		log_line(h, NULL, DMBRIDGE_WARNING,
			 "cannot connect to %s:%s: %s", host, port,
			 strerror(errno));
		return -1;
	}
	nonblock(fd);
	return fd;
}

/* the unix socket path, non-blocking once connected; -1 on failure */
static int connect_unix(const struct dmbridge_host *h, const char *path)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	copy(sun.sun_path, path, sizeof(sun.sun_path));
	if (fd < 0 || connect(fd, (struct sockaddr *)&sun, sizeof(sun))) {
		log_line(h, NULL, DMBRIDGE_WARNING, "cannot connect to %s: %s",
			 path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	nonblock(fd);
	return fd;
}

/* the server's connect, in a thread of its own so that the line's audio
   goes on meanwhile (DNS and connect() can take seconds): the profile's
   host and port (or path), and the socket its result goes on */
struct tcp_job {
	const struct dmbridge_host *h;
	char host[256];
	char port[16];
	char path[108];         /* a unix socket instead */
	int out;
};

/* the connect; its descriptor (-1: it failed) to the call, or closed if
   the call has gone */
static void *tcp_thread(void *arg)
{
	struct tcp_job *j = arg;
	int fd = *j->path ? connect_unix(j->h, j->path) :
			    connect_tcp(j->h, j->host, j->port);

	if (send(j->out, &fd, sizeof(fd), MSG_NOSIGNAL) != (ssize_t)sizeof(fd) &&
	    fd >= 0)
		close(fd);
	close(j->out);
	free(j);
	return NULL;
}

/* fn(arg) in a detached thread: the host's, else pthread's; 0 or -1 */
static int start_thread(const struct dmbridge_host *h, void *(*fn)(void *),
			void *arg)
{
	pthread_t t;
	pthread_attr_t a;
	int err;

	if (h->thread)
		return h->thread(fn, arg);
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	err = pthread_create(&t, &a, fn, arg);
	pthread_attr_destroy(&a);
	return err ? -1 : 0;
}

/* the profile's connect started; the descriptor its result comes on
   (tcp_done()), or -1 */
static int tcp_start(const struct dmbridge_host *h,
		     const struct dmbridge_profile *p)
{
	struct tcp_job *j = calloc(1, sizeof(*j));
	int sv[2];

	if (!j || socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
		free(j);
		return -1;
	}
	j->h = h;
	copy(j->host, p->host, sizeof(j->host));
	copy(j->port, p->port, sizeof(j->port));
	if (p->mode == DMBRIDGE_UNIX)
		copy(j->path, p->path, sizeof(j->path));
	j->out = sv[1];
	if (start_thread(h, tcp_thread, j)) {
		close(sv[0]);
		close(sv[1]);
		free(j);
		return -1;
	}
	nonblock(sv[0]);
	return sv[0];
}

/* the connect's result, if it has come: 1 and *fd (-1: it failed), else
   0 */
static int tcp_done(int pending, int *fd)
{
	int r = recv(pending, fd, sizeof(*fd), 0);

	if (r == (int)sizeof(*fd))
		return 1;
	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
		*fd = -1;
		return 1;
	}
	return 0;
}

/*
 *    the program
 */

/* in the child: none of the PBX's descriptors (sockets, files) */
static void close_fds_above_2(void)
{
	long fd, max;

#ifdef SYS_close_range
	if (!syscall(SYS_close_range, 3U, ~0U, 0))
		return;
#endif
	max = sysconf(_SC_OPEN_MAX);
	if (max < 0 || max > 65536)
		max = 65536;
	for (fd = 3; fd < max; fd++)
		close(fd);
}

/* the program on the slave of a new pty, the master returned; the child
   is ours to reap (end_exec()) */
static int start_exec(const struct dmbridge_host *h,
		      const struct dmbridge_profile *p, const char *const *env,
		      pid_t *pid)
{
	char slave[64];
	int master, i;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) || unlockpt(master) ||
	    ptsname_r(master, slave, sizeof(slave))) {
		log_line(h, NULL, DMBRIDGE_WARNING, "no pty: %s",
			 strerror(errno));
		if (master >= 0)
			close(master);
		return -1;
	}
	*pid = h->fork ? h->fork() : fork();
	if (*pid < 0) {
		log_line(h, NULL, DMBRIDGE_WARNING, "fork: %s",
			 strerror(errno));
		if (h->fork_done)
			h->fork_done();
		close(master);
		return -1;
	}
	if (*pid == 0) {
		struct termios t;
		sigset_t none;
		int fd;
		/* the PBX's threads block signals and it has handlers of
		   its own: the program has the defaults */
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		for (i = 1; i < NSIG; i++)
			signal(i, SIG_DFL);
		setsid();
		/* the pty becomes the program's controlling tty: closing
		   the master hangs it up */
		fd = open(slave, O_RDWR);
		if (fd < 0)
			_exit(127);
		if (!tcgetattr(fd, &t)) {
			cfmakeraw(&t);
			tcsetattr(fd, TCSANOW, &t);
		}
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		close_fds_above_2();
		for (i = 0; env[i]; i += 2)
			setenv(env[i], env[i + 1], 1);
		execl("/bin/sh", "sh", "-c", p->command, (char *)NULL);
		_exit(127);
	}
	nonblock(master);
	return master;
}

/* the program's end: SIGHUP (its pty is closed as well), then it is
   collected, by force (SIGKILL) after 5 s */
static void end_exec(const struct dmbridge_host *h, pid_t pid)
{
	int i;

	kill(pid, SIGHUP);
	for (i = 0; i < 50; i++) {
		pid_t r = waitpid(pid, NULL, WNOHANG);
		if (r == pid || (r < 0 && errno == ECHILD))
			goto done;
		usleep(100000);
	}
	log_line(h, NULL, DMBRIDGE_WARNING,
		 "program %d still there after SIGHUP: SIGKILL", (int)pid);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
done:
	if (h->fork_done)
		h->fork_done();
}

/*
 *    the call
 */

/* (see dmodem_bridge.h) */
void dmbridge_call_init(struct dmbridge_call *c)
{
	memset(c, 0, sizeof(*c));
	c->status = "ERROR";
	c->dte[0] = c->dte[1] = -1;
	c->fd = -1;
	c->connecting = -1;
	c->closing = -1;
}

/* (see dmodem_bridge.h) */
int dmbridge_call_start(struct dmbridge_call *c,
			const struct dmbridge_host *h, void *call,
			const struct dmbridge_profile *p, const char *name,
			enum dmodem_law law, int orig)
{
	struct dmodem_config cfg = { 0 };
	struct dmodem_status st;
	char init[192];

	c->h = h;
	c->call = call;
	c->p = p;
	c->orig = orig;
	/* the modem's DTE: one end of a socketpair, the other end ours */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, c->dte)) {
		log_line(h, call, DMBRIDGE_WARNING, "socketpair: %s",
			 strerror(errno));
		c->dte[0] = c->dte[1] = -1;
		return -1;
	}
	nonblock(c->dte[0]);
	nonblock(c->dte[1]);
	/* no echo, no result codes, no auto answer, no +++ escape; the call
	   is there already: no dial tone to wait for */
	snprintf(init, sizeof(init), "ATE0Q1S0=0S2=255X3%s\r", p->init);
	cfg.name = name;
	cfg.law = law;
	cfg.dte_fd = c->dte[0];
	cfg.init = init;
	c->d = dmodem_create(&cfg);
	if (!c->d) {
		log_line(h, call, DMBRIDGE_WARNING, "no modem");
		return -1;
	}
	drain(c->dte[1]);
	if (orig)
		dmodem_dial(c->d, "");
	else {
		/* the profile's answer command line (ATA): the modem is
		   connecting after it, or the line was no answer */
		dmodem_command(c->d, p->answer);
		dmodem_status(c->d, &st);
		if (st.link != DMODEM_CONNECTING) {
			log_line(h, call, DMBRIDGE_WARNING,
				 "\"%s\" did not answer", p->answer);
			return -1;
		}
	}
	c->status = "NOCARRIER";
	c->deadline = (long long)p->timeout * DMODEM_RATE;
	return 0;
}

/* DMBRIDGE_END with status s */
static enum dmbridge_event end(struct dmbridge_call *c, const char *s)
{
	c->status = s;
	return DMBRIDGE_END;
}

/* the descriptors of the call, for the log */
static void log_fds(struct dmbridge_call *c)
{
	log_line(c->h, c->call, DMBRIDGE_DEBUG, "server fd %d, dte fds %d %d",
		 c->fd, c->dte[0], c->dte[1]);
}

/* (see dmodem_bridge.h) */
enum dmbridge_event dmbridge_call_step(struct dmbridge_call *c, int n)
{
	struct dmodem_status *st = &c->st;

	c->samples += n;
	dmodem_poll_dte(c->d);
	dmodem_status(c->d, st);
	if (!c->online) {
		/* (not what the link delivered with ONLINE: the remote's
		   first data) */
		if (st->link != DMODEM_ONLINE)
			drain(c->dte[1]);
		if (!c->orig && c->p->fax.on && st->fax_caller) {
			/* a fax machine calling: a fax receiver instead of
			   the modem */
			dmodem_destroy(c->d);
			c->d = NULL;
			c->status = "FAX";
			return DMBRIDGE_FAX;
		}
		if (st->link == DMODEM_ONLINE) {
			c->online = 1;
			snprintf(c->rate, sizeof(c->rate), "%u/%u",
				 st->tx_rate, st->rx_rate);
			return DMBRIDGE_CONNECTED;
		}
		if (st->link == DMODEM_IDLE || st->link == DMODEM_HANGUP ||
		    c->samples > c->deadline)
			return DMBRIDGE_END;
		return DMBRIDGE_RUN;
	}
	/* online; the server once it is connected (what the remote sends
	   meanwhile waits in the DTE's socketpair) */
	if (c->connecting >= 0) {
		if (st->link != DMODEM_ONLINE)
			return end(c, "REMOTE");
		if (!tcp_done(c->connecting, &c->fd))
			return DMBRIDGE_RUN;
		close(c->connecting);
		c->connecting = -1;
		if (c->fd < 0)
			return end(c, "ERROR");
		log_fds(c);
	}
	relay(c->h, c->dte[1], c->fd, &c->to_fd);
	if (st->link != DMODEM_ONLINE)
		return end(c, "REMOTE");
	if (c->closing < 0) {
		if (relay(c->h, c->fd, c->dte[1], &c->to_line) < 0) {
			log_line(c->h, c->call, DMBRIDGE_INFO,
				 "the server or program ended the session");
			c->closing = c->samples;
		}
		return DMBRIDGE_RUN;
	}
	/* the server is gone: once what it sent last is on the line, hang
	   up */
	relay(c->h, -1, c->dte[1], &c->to_line);
	if ((!c->to_line.len && !st->tx_pending &&
	     c->samples - c->closing > DMODEM_RATE) ||
	    c->samples - c->closing > 30 * DMODEM_RATE) {
		if (c->to_line.len || st->tx_pending)
			log_line(c->h, c->call, DMBRIDGE_WARNING,
				 "%d bytes not sent at the hangup",
				 c->to_line.len + (int)st->tx_pending);
		return end(c, "LOCAL");
	}
	return DMBRIDGE_RUN;
}

/* the header of a server (header = yes): env's NAME=value lines, values
   on one line, then an empty line; into b, ahead of the caller's bytes */
static void put_header(struct dmbridge_pipe *b, const char *const *env)
{
	int i, n;

	for (i = 0; env[i]; i += 2) {
		char *at = b->data + b->len, *c;
		n = snprintf(at, sizeof(b->data) - b->len, "%s=%s\n", env[i],
			     env[i + 1]);
		if (n < 0 || n >= (int)sizeof(b->data) - b->len - 1)
			break;
		/* a value of more than one line would end the header */
		for (c = at + strlen(env[i]) + 1; c < at + n - 1; c++)
			if (*c == '\r' || *c == '\n')
				*c = ' ';
		b->len += n;
	}
	b->data[b->len++] = '\n';
}

/* (see dmodem_bridge.h) */
enum dmbridge_event dmbridge_call_serve(struct dmbridge_call *c,
					const char *const *env)
{
	if (c->p->mode != DMBRIDGE_EXEC) {
		c->connecting = tcp_start(c->h, c->p);
		if (c->connecting < 0)
			return end(c, "ERROR");
		/* sent once the server is connected, before what the
		   remote modem sends */
		if (c->p->header && env)
			put_header(&c->to_fd, env);
		return DMBRIDGE_RUN;
	}
	c->fd = start_exec(c->h, c->p, env, &c->pid);
	if (c->fd < 0)
		return end(c, "ERROR");
	log_fds(c);
	return DMBRIDGE_RUN;
}

/* (see dmodem_bridge.h) */
void dmbridge_call_hangup(struct dmbridge_call *c)
{
	if (c->d && c->online && c->st.link == DMODEM_ONLINE)
		dmodem_hangup(c->d);
}

/* (see dmodem_bridge.h) */
void dmbridge_call_end(struct dmbridge_call *c)
{
	if (c->d)
		dmodem_destroy(c->d);
	c->d = NULL;
	if (c->fd >= 0)
		close(c->fd);
	if (c->connecting >= 0)
		close(c->connecting);   /* (the thread closes what it gets) */
	if (c->pid > 0)
		end_exec(c->h, c->pid);
	if (c->dte[0] >= 0)
		close(c->dte[0]);
	if (c->dte[1] >= 0)
		close(c->dte[1]);
	c->fd = c->connecting = c->dte[0] = c->dte[1] = -1;
	c->pid = 0;
}
