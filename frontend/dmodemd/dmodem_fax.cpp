/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_fax.cpp - fax calls of the library (dmodem.h): a T.30 fax
 *                   machine (dsp/t30.cpp) that sends a TIFF file or
 *                   receives into one, on audio or over T.38 (its IFP
 *                   packets in UDPTL, dsp/t38.cpp); fax detection (a
 *                   fax machine's CNG, dsp.h's CngDetect)
 *
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include <unistd.h>

#include "dm_types.h"
#include "dmodem.h"
#include "dmodem_g711.h"
#include "dsp.h"
#include "fax_header.h"
#include "reinit.h"
#include "t30.h"
#include "t38.h"

using namespace dsp;

/* a fax call: the T.30 machine and its TIFF file */
struct dmodem_fax {
	T30 *t30;
	enum dmodem_law law;
	s16 sending;
	TiffIn in;
	TiffOut out;
	s16 out_open;
	s16 pages_written;
	s16 file_error;
	char path[1024];
	/* sending: the files to send (path, & between them), the one open
	   at next */
	char files[4096];
	size_t next;
	s16 in_done;            /* the last page of the last file has gone */
	/* the header line: on (header given), its text before the page
	   number, the pages sent so far */
	s16 header_on;
	char header[160];
	s32 page_no;
	/* T.38 */
	s32 t38_version, t38_ifp;
	s32 t38_max_datagram;
	UdptlTx tx;
	UdptlRx rx;
	void (*t38_send)(void *ctx, const unsigned char *pkt, s32 len);
	void *t38_ctx;
};

/* every file of the list (names with & between them) can be read: 0,
   else -1 (none at all is -1 too) */
static s16 files_readable(const char *list)
{
	char name[1024];
	s16 any = 0;

	while (*list) {
		const char *amp = strchr(list, '&');
		size_t n = amp ? static_cast<size_t>(amp - list) : strlen(list);
		if (n) {
			if (n >= sizeof(name))
				return -1;
			memcpy(name, list, n);
			name[n] = 0;
			FILE *fp = fopen(name, "rb");
			if (!fp)
				return -1;
			fclose(fp);
			any = 1;
		}
		list += n;
		if (*list == '&')
			list++;
	}
	return any ? 0 : -1;
}

/* the next file of the list (files from next) open in f->in: 0, or -1
   if there are no more or it can't be read */
static s16 open_next_file(struct dmodem_fax *f)
{
	const char *s = f->files + f->next;
	const char *amp;
	size_t n;

	while (*s == '&')
		s++;
	if (!*s)
		return -1;
	amp = strchr(s, '&');
	n = amp ? static_cast<size_t>(amp - s) : strlen(s);
	if (n >= sizeof(f->path))
		return -1;
	memcpy(f->path, s, n);
	f->path[n] = 0;
	f->next = static_cast<size_t>(s - f->files) + n;
	return f->in.open(f->path) ? -1 : 0;
}

/* T.30's next page to send: 1 with *p filled, 0 when there are no more
   (or on an error); the next file's pages after the last of one */
static s16 next_page(void *ctx, T4Page *p)
{
	struct dmodem_fax *f = static_cast<dmodem_fax*>(ctx);
	while (!f->in_done) {
		if (f->in.read_page(p) == 1) {
			if (f->header_on) {
				char line[200];
				snprintf(line, sizeof(line), "%s  P.%d",
					 f->header, static_cast<int>(++f->page_no));
				/* (out of memory: the page without it) */
				fax_header_add(p, line);
			}
			return 1;
		}
		f->in.close();
		if (open_next_file(f))
			f->in_done = 1;
	}
	return 0;
}

/* a T.30 result as a short code (dmodem_fax_status' error) */
static const char *result_code(enum t30_result r)
{
	switch (r) {
	case T30_RUNNING: return "RUNNING";
	case T30_OK: return "NO_ERROR";
	case T30_ERR_NO_FAX: return "NO_FAX";
	case T30_ERR_INCOMPATIBLE: return "INCOMPATIBLE";
	case T30_ERR_TRAINING: return "TRAINING";
	case T30_ERR_NO_RESPONSE: return "NO_RESPONSE";
	case T30_ERR_PAGE: return "PAGE";
	case T30_ERR_HANGUP: return "HANGUP";
	case T30_ERR_DOCUMENT: return "DOCUMENT";
	case T30_ERR_FILE: return "FILE";
	case T30_ERR_HOST: return "HOST";
	}
	return "UNKNOWN";
}

/* a page received: into the TIFF file, opened at the first page; a
   failure is kept in file_error (the call goes on) */
static void page_received(void *ctx, T4Page *p)
{
	struct dmodem_fax *f = static_cast<dmodem_fax*>(ctx);
	if (!f->out_open) {
		if (f->out.open(f->path)) {
			f->file_error = 1;
			return;
		}
		f->out_open = 1;
	}
	if (f->out.write_page(p))
		f->file_error = 1;
	else
		f->pages_written++;
}

/* an IFP packet of T.30's: in UDPTL (or as it is, with t38_ifp) to
   t38_send */
static void t38_tx(void *ctx, const struct t38_ifp *p)
{
	struct dmodem_fax *f = static_cast<dmodem_fax*>(ctx);
	unsigned char ifp[UDPTL_IFP_MAX], pkt[UDPTL_IFP_MAX * 2];
	s16 n = t38_ifp_encode(p, f->t38_version, ifp, sizeof(ifp)), m;
	s16 max = f->t38_max_datagram < static_cast<s32>(sizeof(pkt)) ?
		static_cast<s16>(f->t38_max_datagram) :
		static_cast<s16>(sizeof(pkt));
	if (n < 0)
		return;
	if (f->t38_ifp) {
		if (f->t38_send)
			f->t38_send(f->t38_ctx, ifp, n);
		return;
	}
	m = f->tx.packet(ifp, n, pkt, max);
	if (m > 0 && f->t38_send)
		f->t38_send(f->t38_ctx, pkt, m);
}

/* an IFP packet out of UDPTL, to T.30 (one it does not know dropped;
   len 0: one lost) */
static void t38_ifp_in(void *ctx, const unsigned char *ifp, s16 len,
		       u16 seq)
{
	struct dmodem_fax *f = static_cast<dmodem_fax*>(ctx);
	struct t38_ifp p;
	(void)seq;
	if (!len)
		t30_t38_lost(f->t30);
	else if (!t38_ifp_decode(&p, f->t38_version, ifp, len))
		t30_t38_rx(f->t30, &p);
}

/* the TIFF to send opened now, the one to receive into only when a page
   comes; NULL without a file name, if the file to send can't be read or
   if T.30 can't be made */
struct dmodem_fax *dmodem_fax_create(const struct dmodem_fax_config *cfg)
{
	struct dmodem_fax *f;
	struct t30_config c = { 0 };

	dmodem_init();
	if (!cfg->file)
		return nullptr;
	f = new (std::nothrow) dmodem_fax();
	if (!f)
		return nullptr;
	f->law = cfg->law;
	f->sending = cfg->dir == DMODEM_FAX_SEND;
	snprintf(f->path, sizeof(f->path), "%s", cfg->file);
	if (f->sending) {
		/* the first file of the list */
		if (strlen(cfg->file) >= sizeof(f->files)) {
			delete f;
			return nullptr;
		}
		snprintf(f->files, sizeof(f->files), "%s", cfg->file);
		f->next = 0;
		if (files_readable(f->files) || open_next_file(f)) {
			delete f;
			return nullptr;
		}
	}
	if (f->sending && cfg->header) {
		/* the date and time now, the sender's name, our number */
		char when[32];
		time_t now = time(nullptr);
		struct tm tm_now;
#ifdef _WIN32
		localtime_s(&tm_now, &now);
#else
		localtime_r(&now, &tm_now);
#endif
		strftime(when, sizeof(when), "%d-%b-%Y %H:%M", &tm_now);
		snprintf(f->header, sizeof(f->header), "%s  %s  %s", when,
			 cfg->header, cfg->ident ? cfg->ident : "");
		f->header_on = 1;
	}
	c.sending = f->sending;
	c.ident = cfg->ident;
	c.max_bps = cfg->max_bps ? cfg->max_bps : 14400;
	c.min_bps = cfg->min_bps > 0 ? cfg->min_bps : 0;
	c.modems = static_cast<u16>(cfg->modems & (DMODEM_FAX_V27 |
						    DMODEM_FAX_V29 |
						    DMODEM_FAX_V17));
	c.ecm = !cfg->no_ecm;
	c.srate = DMODEM_RATE;
	c.next_page = next_page;
	c.page_received = page_received;
	c.ctx = f;
	if (cfg->t38) {
		f->t38_version = cfg->t38_version;
		/* the datagrams no longer than the other end takes: the data
		   in a packet leaves room for the headers (the redundancy
		   goes where it fits) */
		f->t38_max_datagram = cfg->t38_max_datagram > 0 ?
			cfg->t38_max_datagram : 400;
		if (f->t38_max_datagram < 32)
			f->t38_max_datagram = 32;
		f->tx.init(cfg->t38_ec == DMODEM_T38_EC_FEC ? T38_EC_FEC :
			   cfg->t38_ec == DMODEM_T38_EC_REDUNDANCY ?
			   T38_EC_REDUNDANCY : T38_EC_NONE,
			   cfg->t38_redundancy > 0 ? cfg->t38_redundancy : 3);
		f->rx.init();
		f->t38_ifp = cfg->t38_ifp;
		f->t38_send = cfg->t38_send;
		f->t38_ctx = cfg->t38_ctx;
		c.t38 = 1;
		c.t38_tx = t38_tx;
		/* an IFP packet's headers: its type and count, and each
		   field's type and length (a data field and the end's) */
		c.t38_max_data = f->t38_max_datagram - (f->t38_ifp ? 12 : 16);
	}
	f->t30 = t30_create(&c);
	if (!f->t30) {
		if (f->sending && !f->in_done)
			f->in.close();
		delete f;
		return nullptr;
	}
	return f;
}

/* n samples through T.30 */
void dmodem_fax_process(struct dmodem_fax *f, const s16 *in,
			s16 *out, s32 n)
{
	t30_process(f->t30, in, out, n);
}

/* G.711 octets through T.30, 160 at a time */
void dmodem_fax_process_g711(struct dmodem_fax *f, const unsigned char *in,
			     unsigned char *out, s32 n)
{
	auto process = [f](const s16 *li, s16 *lo, s16 k) {
		t30_process(f->t30, li, lo, k);
	};
	dmodem_g711_blocks(f->law == DMODEM_LAW_ALAW, in, out, n, process);
}

/* T.30's status (once the document is complete its result is OK, as
   the call's end will have it); a file error makes it a failure
   (T30_ERR_FILE) */
void dmodem_fax_status(struct dmodem_fax *f, struct dmodem_fax_status *s)
{
	struct t30_status ts;
	enum t30_result r;

	t30_get_status(f->t30, &ts);
	r = ts.complete ? T30_OK : ts.result;
	zero(*s);
	s->done = ts.done;
	s->complete = ts.complete;
	s->ok = r == T30_OK && !f->file_error;
	s->pages = ts.pages;
	s->bps = ts.bps;
	s->fine = ts.fine;
	s->ecm = ts.ecm;
	copy_array(s->remote_ident, ts.remote_ident);
	s->result = f->file_error ? t30_result_text(T30_ERR_FILE) :
		t30_result_text(r);
	s->error = f->file_error ? result_code(T30_ERR_FILE) :
		result_code(r);
	s->phase = ts.phase;
}

/* T.30 learns the far end has gone */
void dmodem_fax_hangup(struct dmodem_fax *f)
{
	t30_hangup(f->t30);
}

/* a UDPTL packet: its IFP packets to T.30, in their order (an IFP
   packet: to T.30) */
void dmodem_fax_t38_rx(struct dmodem_fax *f, const unsigned char *pkt,
		       s32 len)
{
	if (f->t38_ifp)
		t38_ifp_in(f, pkt, len, 0);
	else
		f->rx.packet(pkt, len, t38_ifp_in, f);
}

/* T.38's time going by */
void dmodem_fax_t38_time(struct dmodem_fax *f, s32 n)
{
	t30_t38_time(f->t30, n);
}

/* T.30 freed, the files closed (NULL is fine) */
void dmodem_fax_destroy(struct dmodem_fax *f)
{
	if (!f)
		return;
	t30_free(f->t30);
	if (f->sending && !f->in_done)
		f->in.close();
	if (f->out_open)
		f->out.close();
	delete f;
}

/*
 *    fax detection
 */

/* a CNG detector at 8000 Hz, and the law of its octets */
struct dmodem_faxdetect {
	CngDetect cng;
	enum dmodem_law law;
	s16 heard;
};

/* a detector that has heard nothing */
struct dmodem_faxdetect *dmodem_faxdetect_create(enum dmodem_law law)
{
	auto *d = new (std::nothrow) dmodem_faxdetect();
	if (!d)
		return nullptr;
	d->cng.init(DMODEM_RATE);
	d->law = law;
	return d;
}

/* the samples to the CNG detector: 1 once it has heard a burst, or a
   tone of 400 ms that goes on */
int dmodem_faxdetect_process(struct dmodem_faxdetect *d, const s16 *in,
			     s32 n)
{
	for (s32 i = 0; i < n && !d->heard; i++) {
		d->cng.sample(in[i]);
		if (d->cng.bursts > 0 || d->cng.run_ms() >= 400)
			d->heard = 1;
	}
	return d->heard;
}

/* G.711 octets, 160 at a time, to dmodem_faxdetect_process() */
int dmodem_faxdetect_process_g711(struct dmodem_faxdetect *d,
				  const unsigned char *in, s32 n)
{
	unsigned char out[160];
	s32 done = 0;
	auto process = [d](const s16 *li, s16 *lo, s16 k) {
		memset(lo, 0, sizeof(*lo) * static_cast<size_t>(k));
		dmodem_faxdetect_process(d, li, k);
	};
	while (done < n) {
		s32 k = n - done > 160 ? 160 : n - done;
		dmodem_g711_blocks(d->law == DMODEM_LAW_ALAW, in + done, out, k,
				   process);
		done += k;
	}
	return d->heard;
}

/* (NULL is fine) */
void dmodem_faxdetect_destroy(struct dmodem_faxdetect *d)
{
	delete d;
}
