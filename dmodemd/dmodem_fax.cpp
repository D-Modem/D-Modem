/*
 *
 *    dmodem_fax.cpp - fax calls of the library (dmodem.h): a T.30 fax
 *                   machine (dsp/t30.cpp) that sends a TIFF file or
 *                   receives into one
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dmodem.h"
#include "dmodem_g711.h"
#include "t30.h"

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

/* the TIFF to send opened now, the one to receive into only when a page
   comes; NULL without a file name, if the file to send can't be read or
   if T.30 can't be made */
struct dmodem_fax *dmodem_fax_create(const struct dmodem_fax_config *cfg)
{
	struct dmodem_fax *f;
	struct t30_config c = { 0 };

	dmodem_init();
	if (!cfg->file)
		return NULL;
	f = static_cast<dmodem_fax*>(calloc(1, sizeof(*f)));
	if (!f)
		return NULL;
	f->law = cfg->law;
	f->sending = cfg->dir == DMODEM_FAX_SEND;
	snprintf(f->path, sizeof(f->path), "%s", cfg->file);
	if (f->sending && f->in.open(f->path)) {
		free(f);
		return NULL;
	}
	c.sending = f->sending;
	c.ident = cfg->ident;
	c.max_bps = cfg->max_bps ? cfg->max_bps : 14400;
	c.ecm = !cfg->no_ecm;
	c.srate = DMODEM_RATE;
	c.next_page = next_page;
	c.page_received = page_received;
	c.ctx = f;
	f->t30 = t30_create(&c);
	if (!f->t30) {
		if (f->sending)
			f->in.close();
		free(f);
		return NULL;
	}
	return f;
}

/* n samples through T.30 */
void dmodem_fax_process(struct dmodem_fax *f, const short *in, short *out,
			int n)
{
	t30_process(f->t30, in, out, n);
}

/* G.711 octets through T.30, 160 at a time */
void dmodem_fax_process_g711(struct dmodem_fax *f, const unsigned char *in,
			     unsigned char *out, int n)
{
	auto process = [f](const short *li, short *lo, int k) {
		t30_process(f->t30, li, lo, k);
	};
	dmodem_g711_blocks(f->law == DMODEM_LAW_ALAW, in, out, n, process);
}

/* T.30's status; a file error makes it a failure (T30_ERR_FILE) */
void dmodem_fax_status(struct dmodem_fax *f, struct dmodem_fax_status *s)
{
	struct t30_status ts;
	t30_get_status(f->t30, &ts);
	memset(s, 0, sizeof(*s));
	s->done = ts.result != T30_RUNNING;
	s->ok = ts.result == T30_OK && !f->file_error;
	s->pages = ts.pages;
	s->bps = ts.bps;
	s->fine = ts.fine;
	s->ecm = ts.ecm;
	memcpy(s->remote_ident, ts.remote_ident, sizeof(s->remote_ident));
	s->result = f->file_error ? t30_result_text(T30_ERR_FILE) :
		t30_result_text(ts.result);
	s->phase = ts.phase;
}

/* T.30 learns the far end has gone */
void dmodem_fax_hangup(struct dmodem_fax *f)
{
	t30_hangup(f->t30);
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
	free(f);
}
