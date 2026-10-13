/*
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

#include "t4.h"

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
	int sending;            /* calls and sends; else answers and
				   receives */
	const char *ident;      /* our identity (TSI/CSI), up to 20 of
				   0-9 + and space */
	int max_bps;            /* 14400 (V.17), 9600 (V.29), 4800 (V.27) */
	int ecm;                /* offer and take error correction mode */
	int srate;              /* sample rate of the audio (8000) */
	/* sending: the next page, 1; no more, 0 */
	int  (*next_page)(void *ctx, T4Page *page);
	/* receiving: a page that came; it stays the caller's to free */
	void (*page_received)(void *ctx, T4Page *page);
	void *ctx;
};

struct t30_status {
	enum t30_result result;
	int pages;              /* sent or received */
	int bps;                /* the rate of the pages */
	int fine;
	int ecm;                /* the call has error correction mode */
	char remote_ident[21];
	const char *phase;      /* what it is doing, for the log */
};

struct T30;

/* a fax machine for one call (cfg is copied); NULL when out of memory */
T30 *t30_create(const struct t30_config *cfg);
/* free it, and the pages it still holds */
void t30_free(T30 *t);
/* audio: n samples in and n out, 16 bit linear */
void t30_process(T30 *t, const short *in, short *out, int n);
/* how the call goes (or went) */
void t30_get_status(T30 *t, struct t30_status *s);
/* the far end hung up */
void t30_hangup(T30 *t);

} // namespace dsp

#endif /* __T30_H__ */
