/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    t30.h - a fax machine: the ITU-T T.30 procedure for group 3 fax on
 *            the fax modulations of fax_modems.cpp, with the pages in
 *            T.4 coding (t4.h)
 *
 *    The calling machine sends the document, the answering one receives
 *    it (no polling), with error correction mode (T.30 Annex A) when both
 *    have it.
 *
 */

#ifndef __T30_H__
#define __T30_H__


#include "dm_types.h"
#include "t4.h"
#include "t38.h"

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
};

/* a result as text, for the log */
const char *t30_result_text(enum t30_result r);

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
/* how the call goes (or went) */
void t30_get_status(T30 *t, struct t30_status *s);
/* the far end hung up */
void t30_hangup(T30 *t);

} // namespace dsp

#endif /* __T30_H__ */
