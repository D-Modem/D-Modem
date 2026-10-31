/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t30.h - a fax machine: the ITU-T T.30 procedure for group 3 fax on
 *            the fax modulations of fax_modems.cpp, with the pages in
 *            T.4 coding (t4.h)
 *
 *    The calling machine sends the document, the answering one receives
 *    it (no polling), with error correction mode (T.30 Annex A) when both
 *    have it; on V.34 half-duplex (T.30 Annex F) when both have that.
 *
 */

#ifndef __T30_H__
#define __T30_H__


#include "dm_types.h"
#include "t4.h"
#include "t38.h"

struct modem;

namespace dsp {

/* how a fax call ended */
enum t30_result {
	T30_RUNNING,
	T30_OK,                 /* the whole document went through */
	T30_ERR_NO_FAX,         /* no fax machine answered or called */
	T30_ERR_INCOMPATIBLE,   /* nothing in common to do it with */
	T30_ERR_TRAINING,       /* not even the lowest rate trained */
	T30_ERR_NO_RESPONSE,    /* the remote stopped answering */
	T30_ERR_PAGE,           /* a page failed every time it was sent */
	T30_ERR_HANGUP,         /* the remote hung up (DCN) too soon */
	T30_ERR_DOCUMENT,       /* nothing to send: no file, no pages */
	T30_ERR_FILE,           /* the received document can't be written */
	T30_ERR_HOST,           /* its host (t30_config's host) did not go
				   on in time */
};

/* what a machine paced by its host (t30_config's host) tells it, by
   event(ctx, ev, arg) */
enum t30_event {
	T30_EV_DIS,             /* calling: the DIS came (t30_status has
				   what it offers), or answering and polled:
				   the DTC; it waits to send the DCS
				   (T30_WAIT_DCS; a caller may poll instead,
				   t30_poll()) */
	T30_EV_TRAINED,         /* the CFR: sending, it came; receiving, it
				   went out */
	T30_EV_POST,            /* receiving: a page came whole
				   (page_received() had it); arg the command
				   after it, 0 MPS, 1 EOM, 2 EOP */
	T30_EV_PAGE_SENT,       /* sending: the receiver confirmed a page
				   (MCF); arg the command after it, as for
				   T30_EV_POST */
};

/* what a machine paced by its host waits for, until t30_resume() */
enum t30_wait {
	T30_WAIT_NONE,
	T30_WAIT_DCS,           /* to send the DCS (t30_session() first) */
	T30_WAIT_PAGE,          /* the page: next_page() said 2, not yet */
	T30_WAIT_POST,          /* the command after the page: post_page()
				   said -1, not yet */
};

/* a result as text, for the log */
const char *t30_result_text(enum t30_result r);

/* a half-duplex modem for T.30 Annex F (V.34's clause 12: t30_v34()),
   which has the line from V.8 on: the control channel for the frames,
   the primary channel for the ECM blocks.  Its host (the machine): */
struct t30_hdx_host {
	void *ctx;
	/* the next bit to send on the control channel (primary 0) or the
	   primary channel; -1 there ends it */
	s16 (*get_bit)(void *ctx, s16 primary);
	void (*put_bit)(void *ctx, s16 primary, s16 bit);
	void (*event)(void *ctx, s16 ev);       /* enum t30_hdx_event */
};

enum t30_hdx_event {
	T30_HDX_CC_UP,          /* the control channel carries data */
	T30_HDX_PRIMARY_UP,     /* the primary channel does */
	T30_HDX_PRIMARY_DOWN,   /* and has stopped */
};

struct t30_hdx_ops {
	/* calling or answering on modem (t30_config's), at srate; sending:
	   the caller's V.8 says it sends (else it polls).  NULL when it
	   can't start */
	void *(*create)(struct modem *modem, s16 caller, s16 sending,
			s32 srate, const struct t30_hdx_host *h);
	/* n samples in and out: 0, or -1 when it failed (V.8 found no
	   V.34 half-duplex fax, or the line went) */
	s16 (*process)(void *m, const s16 *in, s16 *out, s32 n);
	/* the source: the primary channel, until its get_bit() says -1 */
	void (*primary)(void *m);
	s16 (*on)(void *m);             /* V.8 chose it */
	s16 (*source)(void *m);         /* we send the primary channel */
	s32 (*rate)(void *m);           /* the primary channel's bit/s */
	void (*free)(void *m);
};

/* V.34 half-duplex (v34.h) for t30_config's v34 */
const struct t30_hdx_ops *t30_v34(void);

struct t30_config {
	s16 sending;            /* calls and sends; else answers and
				   receives */
	const char *ident;      /* our identity (TSI/CSI), up to 20 of
				   0-9 + and space */
	s32 max_bps;            /* 14400 (V.17), 9600 (V.29), 4800 (V.27) */
	s16 ecm;                /* offer and take error correction mode */
	s32 srate;              /* sample rate of the audio (8000) */
	/* sending: the next page, 1; no more, 0 */
	s16  (*next_page)(void *ctx, T4Page *page);
	/* receiving: a page that came; it stays the caller's to free */
	void (*page_received)(void *ctx, T4Page *page);
	void *ctx;
	/* T.38 instead of audio: what would go out on the line as IFP
	   packets to t38_tx (paced as the line would have them, by
	   t30_t38_time()), what comes by t30_t38_rx() */
	s16 t38;
	void (*t38_tx)(void *ctx, const struct t38_ifp *p);
	s32 t38_max_data;       /* data octets in a packet (0: 300) */
	/* paced by its host (a class 2 modem: fax_class2.cpp), which has
	   the pages one at a time as its DTE gives them: sending, the
	   machine waits after the DIS to send the DCS, for each page
	   (next_page() may say 2: not yet) and for the command after it
	   (post_page()), until t30_resume(); no first page at the start.
	   It tells the host how the call goes by event() (both ways) */
	s16 host;
	s16 fine;               /* host, sending: the pages in fine
				   resolution (the remote's DIS permitting) */
	/* host, sending: the command after the page, 0 MPS, 1 EOM, 2 EOP,
	   -1 not yet (it may be asked again for the same page) */
	s16 (*post_page)(void *ctx);
	void (*event)(void *ctx, s16 ev, s16 arg);      /* enum t30_event */
	/* host, answering: a document to be polled (DIS bit 9); a DTC then
	   makes it the sender, as a caller after the DIS (T30_EV_DIS) */
	s16 polled;
	const char *poll_ident; /* our CIG when we poll (NULL: ident) */
	/* V.34 half-duplex (T.30 Annex F) offered by V.8, with ECM only
	   (NULL: none; not on T.38): ANSam for CED, CM for CNG; a remote
	   without it gets V.21 and the fax modulations */
	const struct t30_hdx_ops *v34;
	s16 v34_poll;           /* calling: V.8 says we poll (t30_poll()
				   after the DIS) */
	struct modem *modem;    /* the core's, for the V.34 modem (its S
				   registers and settings; NULL: none) */
};

struct t30_status {
	enum t30_result result; /* as soon as it is known: a DCN may still
				   be going out */
	s16 done;               /* the call is over (the DCN out, or in) */
	s16 complete;           /* the document has gone through (the MCF
				   to its EOP sent or received): the
				   result is OK however the call ends */
	s16 pages;              /* sent or received */
	s32 bps;                /* the rate of the pages */
	s16 fine;
	s16 ecm;                /* the call has error correction mode */
	char remote_ident[21];
	const char *phase;      /* what it is doing, for the log */
	s16 waiting;            /* enum t30_wait */
	/* sending: what the remote's DIS offers, its fastest rate (0: no
	   DIS yet) */
	s32 remote_bps;
	s16 remote_fine, remote_ecm;
	s16 remote_poll;        /* it has a document to be polled */
	s16 sending;            /* it sends now (a poller receives, the
				   polled sends) */
	s16 v34;                /* on V.34 half-duplex (bps its rate) */
};

struct T30;

/* a fax machine for one call (cfg is copied); NULL when out of memory */
T30 *t30_create(const struct t30_config *cfg);
/* free it, and the pages it still holds */
void t30_free(T30 *t);
/* audio: n samples in and n out, 16 bit linear */
void t30_process(T30 *t, const s16 *in, s16 *out, s32 n);
/* T.38: n samples' time going by (at srate): the timers, and the
   packets that are due */
void t30_t38_time(T30 *t, s32 n);
/* T.38: an IFP packet that came */
void t30_t38_rx(T30 *t, const struct t38_ifp *p);
/* T.38: an IFP packet lost (UDPTL had it neither itself nor in the
   redundancy of a later one) */
void t30_t38_lost(T30 *t);
/* how the call goes (or went) */
void t30_get_status(T30 *t, struct t30_status *s);
/* the far end hung up */
void t30_hangup(T30 *t);
/* host: what it waits for (t30_status' waiting) is there: on it goes */
void t30_resume(T30 *t);
/* host, calling, at T30_WAIT_DCS: poll instead (CIG and DTC), and
   receive what the remote sends.  0, or -1 if it has nothing to be
   polled (or the machine is not there) */
s16 t30_poll(T30 *t);
/* host, sending: the session of the next DCS (before the t30_resume()
   of T30_WAIT_DCS): fine resolution, the fastest rate, error correction
   mode */
void t30_session(T30 *t, s16 fine, s32 max_bps, s16 ecm);
/* the line becomes T.38 (on; its packets to t38_tx, max_data octets
   of data in one, 0: 300) or the audio again: what was going out starts
   over on it, the receivers go on there */
void t30_set_t38(T30 *t, s16 on, s32 max_data);
/* host: the call ended by it, with a DCN (T30_ERR_HOST) unless it is
   over */
void t30_abort(T30 *t);

} // namespace dsp

#endif /* __T30_H__ */
