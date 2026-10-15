/*
 * dmodem_bridge.h - the modem call of the PBX modules (asterisk/app_dmodem.c,
 *                   freeswitch/mod_dmodem.c) on libdmodem's C API
 *
 * What the two modules do alike: dmodem.conf's profile (its options by
 * name and value, as the PBX's configuration has them), the modem with
 * a socketpair as its DTE, and, once the modems are connected, the data
 * between that socketpair and the profile's TCP server or program on a
 * pty, until one side goes.  The module keeps the PBX's side: the
 * channel's frames through dmodem_process_g711() (or dmodem_process()),
 * the channel's variables and the call's own log lines, the fax machine
 * that takes a fax caller.  The PBX's logging, fork and threads come
 * through struct dmb_host.
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2.
 */
#ifndef DMODEM_BRIDGE_H
#define DMODEM_BRIDGE_H

#include <sys/types.h>

#include "dmodem.h"

/* a log line's weight: the call's progress (INFO) or its details */
enum dmb_level { DMB_ERROR, DMB_WARNING, DMB_INFO, DMB_DEBUG };

/* the PBX under the bridge */
struct dmb_host {
	/* one line (no newline) for the PBX's log; call: the module's call
	   as dmb_call_start() had it, NULL for none */
	void (*log)(void *call, enum dmb_level level, const char *line);
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

enum dmb_mode { DMB_TCP, DMB_EXEC };

/* fax: the PBX's general settings have the defaults, a profile its own */
struct dmb_fax {
	int on;                 /* a fax caller is received */
	char dir[256];          /* ... into this directory */
	char ident[21];         /* our number (CSI / TSI) */
	int max_bps;
	int ecm;                /* error correction mode offered */
};

struct dmb_profile {
	enum dmb_mode mode;
	char host[256];
	char port[16];
	char command[1024];
	char init[128];         /* extra AT commands, e.g. +MS=34 */
	enum dmodem_law law;    /* DMODEM_LAW_UNKNOWN: the channel's */
	int timeout;            /* seconds to connect */
	struct dmb_fax fax;
};

/* an option's name is key: the case and '-' or '_' (fax_dir, fax-dir)
   aside */
int dmb_option_is(const char *name, const char *key);

/* the built-in fax settings: on, into dir, 14400 bit/s, ECM */
void dmb_fax_defaults(struct dmb_fax *f, const char *dir);
/* a fax option (fax, fax-dir, fax-ident, fax-max-rate, fax-ecm); 0 if
   name is something else */
int dmb_fax_option(const struct dmb_host *h, struct dmb_fax *f,
		   const char *name, const char *value);

/* a profile's defaults: mode tcp, 60 s to connect, all else empty (the
   fax settings too: the module's general ones go there) */
void dmb_profile_init(struct dmb_profile *p);
/* a profile's option (mode, host, port, command, init, law, timeout and
   the fax ones); 0 if name is something else.  where: the profile, for
   the log ("[bbs]", "profile bbs") */
int dmb_profile_option(const struct dmb_host *h, struct dmb_profile *p,
		       const char *where, const char *name,
		       const char *value);
/* -1 if mode tcp lacks host or port, or mode exec command */
int dmb_profile_check(const struct dmb_host *h, const struct dmb_profile *p,
		      const char *where);

/*
 *    the call
 *
 *    dmb_call_init(); dmb_call_start(); for each block of the line, the
 *    modem (c.d) on it, then dmb_call_step(), and on DMB_CONNECTED
 *    dmb_call_serve() at once; after the last, dmb_call_hangup(), and
 *    dmb_call_end() in any case.
 */

/* the bytes between the modem and the server or program */
struct dmb_pipe {
	char data[4096];
	int len;
};

enum dmb_event {
	DMB_RUN,                /* the call goes on */
	DMB_CONNECTED,          /* the modems are connected (rate): the
				   module's variables and log, then
				   dmb_call_serve() */
	DMB_FAX,                /* a fax machine is calling and the profile
				   receives faxes: the modem is gone, the
				   line is the module's fax receiver's */
	DMB_END,                /* the call is over (status) */
};

struct dmb_call {
	struct dmodem *d;       /* the modem: the line's samples */
	/* NOCARRIER, REMOTE, LOCAL, HANGUP, FAX or ERROR (the module sets
	   HANGUP when the channel goes) */
	const char *status;
	char rate[32];          /* transmit/receive bit/s, once connected */

	/* the bridge's */
	const struct dmb_host *h;
	void *call;
	const struct dmb_profile *p;
	struct dmodem_status st;
	int orig, online;
	int dte[2];             /* the modem's DTE, and ours */
	int fd;                 /* the server's socket or the program's pty */
	int connecting;         /* the TCP connect's result comes on it */
	pid_t pid;              /* the program */
	struct dmb_pipe to_line, to_fd;
	long long samples, deadline, closing;
};

/* nothing yet (status ERROR): dmb_call_end() may follow */
void dmb_call_init(struct dmb_call *c);
/* the modem (named name, on law) answering, or calling if orig, the
   profile p its own until dmb_call_end(); call: the module's, for the
   log.  0 (status NOCARRIER) or -1 */
int dmb_call_start(struct dmb_call *c, const struct dmb_host *h, void *call,
		   const struct dmb_profile *p, const char *name,
		   enum dmodem_law law, int orig);
/* n samples of the line have been through c->d: what now */
enum dmb_event dmb_call_step(struct dmb_call *c, int n);
/* after DMB_CONNECTED: the profile's server (connected in a thread, the
   line going on meanwhile) or program (on a pty, with env's name, value
   pairs, NULL ended, in its environment); DMB_RUN, or DMB_END (ERROR) */
enum dmb_event dmb_call_serve(struct dmb_call *c, const char *const *env);
/* the end on the line: the modem hangs up if it is connected */
void dmb_call_hangup(struct dmb_call *c);
/* the modem, the server or the program (SIGHUP, SIGKILL after 5 s) and
   the descriptors gone */
void dmb_call_end(struct dmb_call *c);

#endif
