/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem.h - the modem as a library: D-Modem's modem core (AT
 *               commands, V.42/LAPM, V.42bis) with the open data pumps,
 *               for a program that has the audio itself (an Asterisk
 *               channel, for one)
 *
 *    One struct dmodem per call; the calls can run in different threads
 *    (each modem in one thread at a time).  No signals, no timers of its
 *    own: dmodem_process() keeps the time.
 *
 *    Audio: 16 bit linear at 8000 Hz, any number of samples per call, the
 *    same number in and out.  For V.90 (the digital modem) the samples go
 *    to the network's G.711 encoder unchanged: give the law in the
 *    configuration (it holds for each call of the modem).
 *
 *    C++: dmodem.hpp wraps this in classes.
 *
 *    DTE: a file descriptor the modem reads its DTE's data and commands
 *    from and writes its responses and received data to: the master of a
 *    pty (pppd on the slave), or one end of a socketpair.  Non-blocking.
 *
 */

#ifndef DMODEM_H
#define DMODEM_H

#ifdef __cplusplus
extern "C" {
#endif

#define DMODEM_RATE     8000

enum dmodem_law { DMODEM_LAW_UNKNOWN, DMODEM_LAW_ULAW, DMODEM_LAW_ALAW };

struct dmodem_config {
	const char *name;       /* for the log */
	int dte_fd;             /* the DTE's end, non-blocking: a pty, a
				   pipe or a stream socket; on Windows a
				   stream SOCKET (portable.h) */
	enum dmodem_law law;
	const char *init;       /* AT commands at the start, as the DTE would
				   send them ("ATE0Q1\r", say), or NULL */
	/* a hook flash of ms on the line (AT+VTS's !), called from
	   dmodem_process(): the program signals it on its call (an RFC 4733
	   event 16, a SIP INFO, an analogue line's loop opened); the modem's
	   audio is silent for as long.  NULL: the line has none, and a
	   +VTS with ! is an ERROR */
	void (*hook_flash)(void *arg, unsigned ms);
	void *hook_flash_arg;
};

enum dmodem_link { DMODEM_IDLE, DMODEM_CONNECTING, DMODEM_ONLINE,
		   DMODEM_HANGUP };

struct dmodem_status {
	enum dmodem_link link;
	int offhook;
	unsigned tx_rate, rx_rate;      /* bit/s, once online */
	unsigned tx_pending;            /* DTE bytes not yet sent (or, with
					   V.42, not acknowledged): 0 when
					   all has reached the remote */
	int fax_caller;                 /* a fax machine's CNG, or its V.8
					   CM of a fax call (the modem then
					   silent), came before a
					   connection: hand the call to a
					   dmodem_fax */
};

struct dmodem;

/* once per process, before the first modem */
int  dmodem_init(void);
/* a modem on hook, cfg->init taken as the DTE's first commands; NULL on
   failure.  cfg->dte_fd stays the caller's (destroy does not close it) */
struct dmodem *dmodem_create(const struct dmodem_config *cfg);
/* hangs up a call still on, frees d (NULL is fine) */
void dmodem_destroy(struct dmodem *d);
/* answer the call now (as ATA); dial (as ATD<number>; -1 if the number
   is too long) */
int  dmodem_answer(struct dmodem *d);
int  dmodem_dial(struct dmodem *d, const char *number);
/* a command line as the DTE would type it ("ATS0=0A", say; the \r
   added), run now; -1 if it is too long.  Its result goes to the DTE
   (none with ATQ1): what it did shows in dmodem_status() */
int  dmodem_command(struct dmodem *d, const char *line);
/* n samples in, n samples out */
void dmodem_process(struct dmodem *d, const short *in, short *out, int n);
/* the same with the network's G.711 octets (the law of the
   configuration, mu-law if unknown) */
void dmodem_process_g711(struct dmodem *d, const unsigned char *in,
			 unsigned char *out, int n);
/* take what the DTE wrote (call it when dte_fd is readable, or with each
   block of samples); returns -1 when the DTE side is closed */
int  dmodem_poll_dte(struct dmodem *d);
/* the call's state now, into *s */
void dmodem_status(struct dmodem *d, struct dmodem_status *s);
/* hang up (as ATH) */
void dmodem_hangup(struct dmodem *d);
/* the log: 0 errors only, higher for more */
void dmodem_set_debug(int level);
/* the log lines go to fn (NULL: stderr); for all the modems, set it
   before the first one */
void dmodem_set_log(void (*fn)(const char *line));

/*
 *    fax calls: a group 3 fax machine (T.30, V.17/V.29/V.27ter, error
 *    correction mode when the other machine has it) that sends a TIFF
 *    file (calling) or receives one (answering), on the audio of the line
 *    or over T.38 (as a T.38 terminal: UDPTL packets, the time going by
 *    told with dmodem_fax_t38_time())
 */

enum dmodem_fax_dir { DMODEM_FAX_RECEIVE, DMODEM_FAX_SEND };

/* T.38's UDPTL error recovery (T38FaxUdpEC) for the packets we send */
enum dmodem_t38_ec { DMODEM_T38_EC_NONE, DMODEM_T38_EC_REDUNDANCY };

struct dmodem_fax_config {
	enum dmodem_law law;
	enum dmodem_fax_dir dir;
	const char *file;               /* the TIFF to send, or to write */
	const char *ident;              /* our number (TSI/CSI), 20 chars */
	int max_bps;                    /* 14400 if 0 */
	int no_ecm;                     /* without error correction mode */
	/* T.38 instead of audio: our UDPTL packets to t38_send, the other
	   end's by dmodem_fax_t38_rx() */
	int t38;
	int t38_version;                /* T38FaxVersion agreed, 0 to 3 */
	enum dmodem_t38_ec t38_ec;
	int t38_redundancy;             /* the packets before in each (with
					   DMODEM_T38_EC_REDUNDANCY; 0: 3) */
	int t38_max_datagram;           /* the other end's T38FaxMaxDatagram
					   (0: 400); with t38_ifp the
					   longest IFP packet it takes */
	int t38_ifp;                    /* the packets are IFP packets, not
					   UDPTL (for a program whose UDPTL
					   is its own: Asterisk's channels):
					   t38_ec and t38_redundancy
					   unused, the packets that come in
					   their order */
	void (*t38_send)(void *ctx, const unsigned char *pkt, int len);
	void *t38_ctx;
};

struct dmodem_fax_status {
	int done;                       /* the call is over (its DCN out or
					   in, a hangup, or an error) */
	int ok;                         /* the document went through */
	int pages;                      /* sent or received */
	int bps, fine;
	char remote_ident[21];
	const char *result;             /* how it ended (or "in progress") */
	const char *phase;
	int ecm;                        /* the call has error correction
					   mode */
	int complete;                   /* the document has gone through
					   (the MCF to its last page): ok,
					   pages and result are final, the
					   received pages are in the file,
					   and the call only ends (the
					   other machine may hang up) */
};

struct dmodem_fax;

/* NULL if the file to send can't be read */
struct dmodem_fax *dmodem_fax_create(const struct dmodem_fax_config *cfg);
/* n samples in, n samples out (16 bit linear, 8000 Hz) */
void dmodem_fax_process(struct dmodem_fax *f, const short *in, short *out,
			int n);
/* the same with G.711 octets (the law of cfg, mu-law if unknown) */
void dmodem_fax_process_g711(struct dmodem_fax *f, const unsigned char *in,
			     unsigned char *out, int n);
/* how the call goes, into *s (done once it has ended) */
void dmodem_fax_status(struct dmodem_fax *f, struct dmodem_fax_status *s);
/* the call went away */
void dmodem_fax_hangup(struct dmodem_fax *f);
/* T.38: a UDPTL packet of len octets that came (with t38_ifp an IFP
   packet) */
void dmodem_fax_t38_rx(struct dmodem_fax *f, const unsigned char *pkt,
		       int len);
/* T.38: n samples' time (at 8000 Hz) going by: the timers, and the
   packets due by now to t38_send */
void dmodem_fax_t38_time(struct dmodem_fax *f, int n);
/* also closes the received file (none if no page came) */
void dmodem_fax_destroy(struct dmodem_fax *f);

#ifdef __cplusplus
}
#endif

#endif
