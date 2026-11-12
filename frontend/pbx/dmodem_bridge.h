/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * dmodem_bridge.h - the modem call of the PBX modules
 *                   (frontend/asterisk/app_dmodem.c,
 *                   frontend/freeswitch/mod_dmodem.c) on libdmodem's C API
 *
 * What the two modules do alike: dmodem.conf's profile (its options by
 * name and value, as the PBX's configuration has them), the modem with
 * a socketpair as its DTE, and, once the modems are connected, the data
 * between that socketpair and the profile's server (TCP or a unix
 * socket) or program on a pty, until one side goes.  The module keeps
 * the PBX's side: the channel's frames through dmodem_process_g711() (or
 * dmodem_process()), the channel's variables and the call's own log
 * lines, the fax machine that takes a fax caller.  The PBX's logging,
 * fork and threads come through struct dmbridge_host.
 */
#ifndef DMODEM_BRIDGE_H
#define DMODEM_BRIDGE_H

#include <sys/types.h>

#include "dmodem.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* a profile's clear option: the call a clear 64 kbit/s channel (an ISDN
   data call: the modem's clear channel pump, AT+MS=CLEAR, on the
   channel's octets as they are) when the PBX says the call's bearer is
   unrestricted digital, always, or never */
enum dmbridge_clear { DMBRIDGE_CLEAR_AUTO, DMBRIDGE_CLEAR_YES,
		      DMBRIDGE_CLEAR_NO };

/* fax: the PBX's general settings have the defaults, a profile its own */
struct dmbridge_fax {
	int on;                 /* a fax caller is received */
	char dir[256];          /* ... into this directory */
	char ident[21];         /* our number (CSI / TSI) */
	int max_bps;
	int ecm;                /* error correction mode offered */
	int t38;                /* over T.38 when the channel has it */
};

/* a profile's voice_dsvd "open" (see struct dmbridge_profile) */
#define DMBRIDGE_DSVD_OPENS 2

struct dmbridge_profile {
	enum dmbridge_mode mode;
	char host[256];
	char port[16];
	char path[108];         /* mode unix: the server's socket */
	int header;             /* tcp, unix: the program's environment (the
				   call's details) first on the connection */
	int handoff;            /* unix (with header): the modem's DTE a
				   pty, its terminal to the server
				   (SCM_RIGHTS, with the header), the
				   connection then only the session's
				   lifetime: no bytes copied here */
	char command[1024];
	char init[128];         /* extra AT commands, e.g. +MS=34 */
	char answer[128];       /* the command line that answers (ATA) */
	enum dmodem_law law;    /* DMODEM_LAW_UNKNOWN: the channel's */
	enum dmbridge_clear clear;
	int timeout;            /* seconds to connect: the modems, then
				   (tcp, unix) the server */
	struct dmbridge_fax fax;
	/* voice: where a person who calls goes, and a DSVD or ASVD
	   caller's voice channel, as the module's PBX names an extension
	   (exten@context; empty: nowhere, the modem answers at once as
	   ever); listen: the seconds the line is listened to before the
	   modem answers, for speech; dsvd: the modem offers DSVD and ASVD
	   (V.8 bis) and their voice channel goes there, the far end's to
	   open (1), or ours too, opened as soon as the modems are
	   connected (DMBRIDGE_DSVD_OPENS, the option's "open": a call to a
	   DSVD modem with someone on the extension at once) */
	char voice[128];
	int voice_listen;
	int voice_dsvd;
};

/* an option's name is key: the case and '-' or '_' (fax_dir, fax-dir)
   aside */
int dmbridge_option_is(const char *name, const char *key);

/* the built-in fax settings: on, into dir, 14400 bit/s, ECM, T.38 */
void dmbridge_fax_defaults(struct dmbridge_fax *f, const char *dir);
/* a fax option (fax, fax-dir, fax-ident, fax-max-rate, fax-ecm,
   fax-t38); 0 if name is something else */
int dmbridge_fax_option(const struct dmbridge_host *h,
			struct dmbridge_fax *f, const char *name,
			const char *value);

/* a profile's defaults: mode tcp, ATA to answer, 60 s to connect, 3 s
   to listen for a person (with voice), all else empty (the fax settings
   too: the module's general ones go there) */
void dmbridge_profile_init(struct dmbridge_profile *p);
/* a profile's option (mode, host, port, path, header, handoff, command, init,
   answer, law, clear, timeout, voice, voice-listen, voice-dsvd and the fax
   ones); 0 if name is something else.
   where: the profile, for the log ("[bbs]", "profile bbs") */
int dmbridge_profile_option(const struct dmbridge_host *h,
			    struct dmbridge_profile *p, const char *where,
			    const char *name, const char *value);
/* -1 if mode tcp lacks host or port, mode unix path, mode exec
   command, or handoff mode unix and header */
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
 *
 *    With the profile's voice, an answered call is listened to first
 *    (the modem silent, on hook): a person speaking is DMBRIDGE_VOICE,
 *    and the module puts the caller through to the profile's voice
 *    extension, its own leg's audio to the caller, the caller's both to
 *    that leg and through the modem (c.d), then dmbridge_voice_step().
 *    The leg ends when the extension hangs up or its dialplan ends, or
 *    at DMBRIDGE_DATA (the caller's modem asks for data: the leg hung
 *    up); then dmbridge_call_resume(), and the modem answers on the
 *    call as at the start.  Online with the profile's voice_dsvd, a
 *    DSVD or ASVD caller's voice channel opening is
 *    DMBRIDGE_DSVD_OPEN: the module calls the extension and moves the
 *    audio with dmbridge_dsvd_ear() and dmbridge_dsvd_mic() while the
 *    data goes on; its closing is DMBRIDGE_DSVD_CLOSED (the leg hung
 *    up), and the leg's end dmbridge_dsvd_close().
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
	DMBRIDGE_VOICE,              /* a person is calling: the voice leg
				   (the modem on hook, silent) */
	DMBRIDGE_DATA,               /* in the voice leg, the caller's modem
				   asks for data (V.25's CT, V.8's CI,
				   V.8 bis's CRe, a fax's CNG): the leg
				   ends, dmbridge_call_resume() */
	DMBRIDGE_DSVD_OPEN,          /* online, a voice channel opened (DSVD,
				   ASVD): its leg to the extension */
	DMBRIDGE_DSVD_CLOSED,        /* and closed: that leg hung up */
};

/* a voice channel's samples one way (16 bit, 8000 a second): a ring of
   1 s, the oldest dropped when it is full */
struct dmbridge_ring {
	int16_t buf[DMODEM_RATE];
	int head, len;
};

/* n samples into ring r, the oldest dropped to make room */
void dmbridge_ring_put(struct dmbridge_ring *r, const int16_t *pcm, int n);
/* up to max samples out of ring r: how many */
int dmbridge_ring_get(struct dmbridge_ring *r, int16_t *pcm, int max);

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
	/* the module's: the call's bearer is unrestricted digital (with
	   the profile's clear auto, a clear channel), before
	   dmbridge_call_start() */
	int digital;
	int dte[2];             /* the modem's DTE, and ours */
	int fd;                 /* the server's socket or the program's pty */
	int connecting;         /* the TCP connect's result comes on it */
	pid_t pid;              /* the program */
	struct dmbridge_pipe to_line, to_fd;
	long long samples, deadline, closing;
	long long connect_by;   /* the server connected by then (samples) */
	int handed;             /* the DTE is the server's (handoff) */
	/* voice: the line listened to until then (samples; 0: not
	   listening); the voice leg on; a DSVD or ASVD voice channel open
	   and its change not yet told; its far end's speech (ear) and ours
	   (mic) */
	long long listen_until;
	int voice_leg;
	int dsvd_on, dsvd_changed;
	struct dmbridge_ring ear, mic;
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
/* in the voice leg, n samples of the caller's through the modem (c->d)
   since the last: DMBRIDGE_RUN, or DMBRIDGE_DATA */
enum dmbridge_event dmbridge_voice_step(struct dmbridge_call *c, int n);
/* the voice leg is over (the module's leg gone): the modem answers on
   the call now, the time to connect from now; 0, or -1 if the answer
   command line did not answer (status ERROR) */
int dmbridge_call_resume(struct dmbridge_call *c);
/* a voice channel's far end speech for the extension's leg: up to max
   samples, how many */
int dmbridge_dsvd_ear(struct dmbridge_call *c, int16_t *pcm, int max);
/* the extension's speech to the voice channel: n samples */
void dmbridge_dsvd_mic(struct dmbridge_call *c, const int16_t *pcm, int n);
/* the extension's leg has gone: a DSVD voice channel closed (the data
   going on); an ASVD one stays, silent */
void dmbridge_dsvd_close(struct dmbridge_call *c);
/* the end on the line: the modem hangs up if it is connected */
void dmbridge_call_hangup(struct dmbridge_call *c);
/* the modem, the server or the program (SIGHUP, SIGKILL after 5 s) and
   the descriptors gone */
void dmbridge_call_end(struct dmbridge_call *c);

#ifdef __cplusplus
}
#endif

#endif
