/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem_fax.cpp - fax calls of the library (dmodem.h): a T.30 fax
 *                   machine (dsp/t30.cpp) that sends a TIFF file or
 *                   receives into one, on audio or over T.38 (its IFP
 *                   packets in UDPTL, dsp/t38.cpp)
 *
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <unistd.h>

#include "dm_types.h"
#include "dmodem.h"
#include "dmodem_g711.h"
#include "reinit.h"
#include "t30.h"
#include "t38.h"

using namespace dsp;

/* a fax call: the T.30 machine and its TIFF file */
struct dmodem_fax {
	T30 *t30;
	enum dmodem_law law;
	int sending;
	TiffIn in;
	TiffOut out;
	int out_open;
	int pages_written;
	int file_error;
	char path[1024];
	/* T.38 */
	int t38_version, t38_ifp;
	s32 t38_max_datagram;
	UdptlTx tx;
	UdptlRx rx;
	void (*t38_send)(void *ctx, const unsigned char *pkt, s32 len);
	void *t38_ctx;
};

/* T.30's next page to send: 1 with *p filled, 0 when there are no more
   (or on an error) */
static int next_page(void *ctx, T4Page *p)
{
	struct dmodem_fax *f = static_cast<dmodem_fax*>(ctx);
	return f->in.read_page(p) == 1 ? 1 : 0;
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
	int n = t38_ifp_encode(p, f->t38_version, ifp, sizeof(ifp)), m;
	int max = f->t38_max_datagram < static_cast<s32>(sizeof(pkt)) ?
		static_cast<int>(f->t38_max_datagram) :
		static_cast<int>(sizeof(pkt));
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

/* an IFP packet out of UDPTL, to T.30 (one it does not know dropped) */
static void t38_ifp_in(void *ctx, const unsigned char *ifp, int len,
		       unsigned seq)
{
	struct dmodem_fax *f = static_cast<dmodem_fax*>(ctx);
	struct t38_ifp p;
	(void)seq;
	if (!t38_ifp_decode(&p, f->t38_version, ifp, len))
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
	if (f->sending && f->in.open(f->path)) {
		delete f;
		return nullptr;
	}
	c.sending = f->sending;
	c.ident = cfg->ident;
	c.max_bps = cfg->max_bps ? cfg->max_bps : 14400;
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
		f->tx.init(cfg->t38_ec == DMODEM_T38_EC_REDUNDANCY ?
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
		if (f->sending)
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
	auto process = [f](const s16 *li, s16 *lo, int k) {
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
	if (f->sending)
		f->in.close();
	if (f->out_open)
		f->out.close();
	delete f;
}
