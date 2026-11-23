/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_embed.h - dmodemd as a library: the modem in a thread of the
 *                     program, on a SIP, USB, ALSA or mISDN line
 *
 *    A program (the D-Modem desktop app) uses this library in place of a
 *    dmodemd process.  dmodem_embed_start() starts the modem in a thread
 *    and returns a descriptor: the DTE of the modem.  The program writes
 *    AT commands and data to the descriptor, and reads the responses of
 *    the modem from it, as it does with the pty of dmodemd.  There is no
 *    pty, no /dev/ttyDM0 link and no root.
 *
 *    Each process has one modem at a time: the core has one log and one
 *    set of tables for each process.
 *
 *    The callbacks occur on the thread of the modem.  They must not call
 *    dmodem_embed_stop() or dmodem_embed_free().
 */
#ifndef DMODEM_EMBED_H
#define DMODEM_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

/* the kinds of line (dmodem_embed_config.line) */
enum {
	DMODEM_EMBED_SIP = 0,   /* a SIP account, or no account */
	DMODEM_EMBED_USB,       /* a USB modem (dmodemd --usb) */
	DMODEM_EMBED_ALSA,      /* a softmodem codec (dmodemd -a) */
	DMODEM_EMBED_MISDN      /* an ISDN card (dmodemd --misdn) */
};

/* the settings of the modem.  dmodem_embed_config_init() gives each
   field the value that dmodemd uses when it has no option for it.  The
   library copies the strings: they must stay valid only during the call
   to dmodem_embed_start().  NULL and "" are the same */
typedef struct dmodem_embed_config {
	int line;               /* DMODEM_EMBED_* */
	const char *country;    /* -c: the country of the line ("US") */
	int log_level;          /* -d: 0 (none) to 5 */
	const char *data_file;  /* the file that keeps the line's
				   measurements from call to call; none:
				   the modem measures again on each start */

	/* DMODEM_EMBED_SIP: the account.  No user and no domain: no account
	   (SIP_LOGIN=anonymous) */
	const char *sip_user;
	const char *sip_password;
	const char *sip_domain; /* host[:port] */
	int sip_port;           /* SIP_PORT: the local UDP port (5060) */
	int sip_listen;         /* -L: register, and take calls (0) */
	int sip_t38;            /* SIP_T38 (1) */
	int sip_v152;           /* SIP_V152 (1) */
	int sip_v150;           /* SIP_V150 (0) */
	int sip_clearmode;      /* SIP_CLEARMODE (1) */
	int sip_tcp;            /* SIP_TCP (1) */
	int sip_srtp;           /* SIP_SRTP: 0 never, 1 if offered (1),
				   2 always */
	int sip_red;            /* SIP_RED: 0 to 3 (2) */
	int sip_jitter_ms;      /* SIP_JITTER: 20 to 1000 (60) */
	const char *sip_dns;    /* SIP_DNS: the DNS servers; none: the
				   system's */
	int sip_log_level;      /* SIP_LOG_LEVEL: 0 to 4; -1: 4 (-1) */

	/* DMODEM_EMBED_USB: the device, as dmodemd --usb has it ("" or
	   "auto" for the first that is found) */
	const char *usb;

	/* DMODEM_EMBED_ALSA: the PCM and the control device of the codec
	   (dmodemd -a and --alsa-ctl) */
	const char *alsa;
	const char *alsa_ctl;
	/* DMODEM_EMBED_USB and DMODEM_EMBED_ALSA: the sample rate (dmodemd
	   -r: 8000, 9600, 48000, 96000 or 192000); 0: as dmodemd with no
	   -r (9600, or the rate of the device) (0) */
	unsigned rate;

	/* DMODEM_EMBED_MISDN: the card, the MSN, and the bits of the
	   B-channel reversed (dmodemd --misdn and --misdn-msn; 0 is
	   --misdn-noflip) (1) */
	const char *misdn;
	const char *misdn_msn;
	int misdn_flip;
} dmodem_embed_config;

typedef struct dmodem_embed dmodem_embed;

/* a line of the log of the modem (with no newline) */
typedef void (*dmodem_embed_log_fn)(void *ctx, const char *line);
/* the modem stopped: 0 after dmodem_embed_stop() or when the program
   closed its descriptor, -1 on an error (the line failed) */
typedef void (*dmodem_embed_exit_fn)(void *ctx, int status);

/* the version of D-Modem ("1.0.0") */
const char *dmodem_embed_version(void);
/* the lines in this build: bit (1 << DMODEM_EMBED_*) for each */
int dmodem_embed_lines(void);

/* each field of cfg as dmodemd has it with no options */
void dmodem_embed_config_init(dmodem_embed_config *cfg);

/* a modem, not started; NULL if there is no memory */
dmodem_embed *dmodem_embed_new(void);
/* dmodem_embed_stop(), then the modem freed */
void dmodem_embed_free(dmodem_embed *e);

/* the callbacks, before dmodem_embed_start() (NULL: none) */
void dmodem_embed_on_log(dmodem_embed *e, dmodem_embed_log_fn fn, void *ctx);
void dmodem_embed_on_exit(dmodem_embed *e, dmodem_embed_exit_fn fn,
			  void *ctx);

/* the modem started in a thread on the line of cfg.  Returns the
   descriptor of the DTE (a socket, non-blocking: the program closes it
   when the modem has stopped), or -1: an error that
   dmodem_embed_error() gives.  The line opens in the thread: if it
   fails, the exit callback gives -1 */
int dmodem_embed_start(dmodem_embed *e, const dmodem_embed_config *cfg);
/* the modem stopped, and its thread ended (it hangs up a call) */
void dmodem_embed_stop(dmodem_embed *e);
/* the thread of the modem runs */
int dmodem_embed_running(dmodem_embed *e);
/* the last error of dmodem_embed_start() */
const char *dmodem_embed_error(dmodem_embed *e);

#ifdef __cplusplus
}
#endif

#endif
