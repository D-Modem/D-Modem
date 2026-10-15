/*
 * dmodem_bridge.h - the modem call of the PBX modules (asterisk/app_dmodem.c,
 *                   freeswitch/mod_dmodem.c) on libdmodem's C API
 *
 * What the two modules do alike: dmodem.conf's profile (its options by
 * name and value, as the PBX's configuration has them), the modem with
 * a socketpair as its DTE, and, once the modems are connected, the data
 * between that socketpair and the profile's server (TCP or a unix
 * socket) or program on a pty, until one side goes.  The module keeps the PBX's side: the
 * channel's frames through dmodem_process_g711() (or dmodem_process()),
 * the channel's variables and the call's own log lines, the fax machine
 * that takes a fax caller.  The PBX's logging, fork and threads come
 * through struct dmbridge_host.
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2.
 */
#ifndef DMODEM_BRIDGE_H
#define DMODEM_BRIDGE_H

#include <sys/types.h>

#include "dmodem.h"

/* a log line's weight: the call's progress (INFO) or its details */
enum dmbridge_level {
	DMBRIDGE_ERROR, DMBRIDGE_WARNING, DMBRIDGE_INFO, DMBRIDGE_DEBUG
};

/* the PBX under the bridge */
struct dmbridge_host {
	/* one line (no newline) for the PBX's log; call: the module's call
	   as dmbridge_call_start() had it, NULL for none */
	void (*log)(void *call, enum dmbridge_level level, const char *line);
	/* a configuration value as a boolean, the PBX's way */
	int (*is_true)(const char *value);
	/* a fork for the program (NULL: fork()), and what comes after the
	   program is collected or its fork failed (NULL: nothing) */
	pid_t (*fork)(void);
	void (*fork_done)(void);
	/* fn(arg) in a detached thread: 0, -1 if it did not start (NULL:
	   pthread_create()) */
	int (*thread)(void *(*fn)(void *), void *arg);
};

/*
 *    the profile
 */

enum dmbridge_mode { DMBRIDGE_TCP, DMBRIDGE_UNIX, DMBRIDGE_EXEC };

/* fax: the PBX's general settings have the defaults, a profile its own */
struct dmbridge_fax {
	int on;                 /* a fax caller is received */
	char dir[256];          /* ... into this directory */
	char ident[21];         /* our number (CSI / TSI) */
	int max_bps;
	int ecm;                /* error correction mode offered */
};

struct dmbridge_profile {
	enum dmbridge_mode mode;
	char host[256];
	char port[16];
	char path[108];         /* mode unix: the server's socket */
	int header;             /* tcp, unix: the program's environment (the
				   call's details) first on the connection */
	char command[1024];
	char init[128];         /* extra AT commands, e.g. +MS=34 */
	char answer[128];       /* the command line that answers (ATA) */
	enum dmodem_law law;    /* DMODEM_LAW_UNKNOWN: the channel's */
	int timeout;            /* seconds to connect */
	struct dmbridge_fax fax;
};

/* an option's name is key: the case and '-' or '_' (fax_dir, fax-dir)
   aside */
int dmbridge_option_is(const char *name, const char *key);

/* the built-in fax settings: on, into dir, 14400 bit/s, ECM */
void dmbridge_fax_defaults(struct dmbridge_fax *f, const char *dir);
/* a fax option (fax, fax-dir, fax-ident, fax-max-rate, fax-ecm); 0 if
   name is something else */
int dmbridge_fax_option(const struct dmbridge_host *h,
			struct dmbridge_fax *f, const char *name,
			const char *value);

/* a profile's defaults: mode tcp, ATA to answer, 60 s to connect, all
   else empty (the fax settings too: the module's general ones go
   there) */
void dmbridge_profile_init(struct dmbridge_profile *p);
/* a profile's option (mode, host, port, path, header, command, init,
   answer, law, timeout and the fax ones); 0 if name is something else.  where: the
   profile, for the log ("[bbs]", "profile bbs") */
int dmbridge_profile_option(const struct dmbridge_host *h,
			    struct dmbridge_profile *p, const char *where,
			    const char *name, const char *value);
/* -1 if mode tcp lacks host or port, mode unix path, or mode exec
   command */
int dmbridge_profile_check(const struct dmbridge_host *h,
			   const struct dmbridge_profile *p,
			   const char *where);

/*
 *    the call
 *
 *    dmbridge_call_init(); dmbridge_call_start(); for each block of the
 *    line, the modem (c.d) on it, then dmbridge_call_step(), and on
 *    DMBRIDGE_CONNECTED dmbridge_call_serve() at once; after the last,
 *    dmbridge_call_hangup(), and dmbridge_call_end() in any case.
 */

/* the bytes between the modem and the server or program */
struct dmbridge_pipe {
	char data[4096];
	int len;
};

enum dmbridge_event {
	DMBRIDGE_RUN,                /* the call goes on */
	DMBRIDGE_CONNECTED,          /* the modems are connected (rate): the
				   module's variables and log, then
				   dmbridge_call_serve() */
	DMBRIDGE_FAX,                /* a fax machine is calling and the profile
				   receives faxes: the modem is gone, the
				   line is the module's fax receiver's */
	DMBRIDGE_END,                /* the call is over (status) */
};

struct dmbridge_call {
	struct dmodem *d;       /* the modem: the line's samples */
	/* NOCARRIER, REMOTE, LOCAL, HANGUP, FAX or ERROR (the module sets
	   HANGUP when the channel goes) */
	const char *status;
	char rate[32];          /* transmit/receive bit/s, once connected */

	/* the bridge's */
	const struct dmbridge_host *h;
	void *call;
	const struct dmbridge_profile *p;
	struct dmodem_status st;
	int orig, online;
	int dte[2];             /* the modem's DTE, and ours */
	int fd;                 /* the server's socket or the program's pty */
	int connecting;         /* the TCP connect's result comes on it */
	pid_t pid;              /* the program */
	struct dmbridge_pipe to_line, to_fd;
	long long samples, deadline, closing;
};

/* nothing yet (status ERROR): dmbridge_call_end() may follow */
void dmbridge_call_init(struct dmbridge_call *c);
/* the modem (named name, on law) answering (the profile's answer
   command line), or calling if orig, the profile p its own until
   dmbridge_call_end(); call: the module's, for the log.  0 (status
   NOCARRIER) or -1 (also when the answer command line does not
   answer) */
int dmbridge_call_start(struct dmbridge_call *c,
			const struct dmbridge_host *h, void *call,
			const struct dmbridge_profile *p, const char *name,
			enum dmodem_law law, int orig);
/* n samples of the line have been through c->d: what now */
enum dmbridge_event dmbridge_call_step(struct dmbridge_call *c, int n);
/* after DMBRIDGE_CONNECTED: the profile's server (connected in a thread,
   the line going on meanwhile) or program (on a pty), with env's name,
   value pairs (NULL ended) in the program's environment, or with header
   first to the server as NAME=value lines and an empty line;
   DMBRIDGE_RUN, or DMBRIDGE_END (ERROR) */
enum dmbridge_event dmbridge_call_serve(struct dmbridge_call *c,
					const char *const *env);
/* the end on the line: the modem hangs up if it is connected */
void dmbridge_call_hangup(struct dmbridge_call *c);
/* the modem, the server or the program (SIGHUP, SIGKILL after 5 s) and
   the descriptors gone */
void dmbridge_call_end(struct dmbridge_call *c);

#endif
