/*
 *
 *    dmodem.cpp - the modem as a library (see dmodem.h)
 *
 *    The modem core (../core, dcore::Modem) with this program as its
 *    Host: the audio comes and goes through dmodem_process(), the DTE
 *    through a file descriptor of the caller's, and the time is the
 *    audio's.
 *
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dcore.h>
#include <log.h>
#include "dmodem.h"
#include "dsplibs.h"

extern int  dp_dummy_init(void);
extern int  dp_sinus_init(void);

/* a calling fax machine's CNG: 1100 Hz, 0.5 s on, 3 s off */
struct cng_detect {
	float s1, s2, energy;
	int n, on_blocks, bursts;
};

/* one modem: the core with the library as its Host */
struct dmodem final : public dcore::Host {
	dcore::Modem *m = nullptr;
	int dte_fd = -1;
	enum dmodem_law law = DMODEM_LAW_UNKNOWN;
	int law_set = 0;
	unsigned long long samples = 0; /* the modem's clock: its audio */
	char name[32] = {};
	struct cng_detect cng = {};

	/* the line is the caller's: nothing to start */
	int start(dcore::Modem &) override { return 0; }
	/* nor to stop */
	int stop(dcore::Modem &) override { return 0; }
	/* for ATI3 */
	const char *driver_name() override { return "dmodem"; }
	/* to the DTE's fd (non-blocking): what it took, 0 if nothing */
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		int ret = write(dte_fd, buf, n);
		return ret < 0 ? 0 : ret;
	}
	/* the clock: the samples processed so far, in ms */
	unsigned long now_ms() override {
		return (unsigned long)(samples * 1000 / DMODEM_RATE);
	}
};

/* G.711 (the usual reference algorithms) */

/* the segment of v: the first of the 8 ends in t that v does not pass,
   8 beyond the last */
static int seg_search(int v, const short *t)
{
	int i;
	for (i = 0; i < 8; i++)
		if (v <= t[i])
			return i;
	return 8;
}

/* 16 bit linear to mu-law (14 bits, clipped at 8159) */
static unsigned char lin2ulaw(int x)
{
	static const short seg_end[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF,
					  0x7FF, 0xFFF, 0x1FFF };
	int mask = 0xFF, seg;
	x >>= 2;
	if (x < 0) {
		x = -x;
		mask = 0x7F;
	}
	if (x > 8159)
		x = 8159;
	x += 0x84 >> 2;
	seg = seg_search(x, seg_end);
	if (seg >= 8)
		return (unsigned char)(0x7F ^ mask);
	return (unsigned char)(((seg << 4) | ((x >> (seg + 1)) & 15)) ^ mask);
}

/* mu-law to 16 bit linear */
static int ulaw2lin(unsigned char u)
{
	int t;
	u = ~u;
	t = ((u & 15) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (u & 0x80) ? 0x84 - t : t - 0x84;
}

/* 16 bit linear to A-law (13 bits, even bits inverted) */
static unsigned char lin2alaw(int x)
{
	static const short seg_end[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF,
					  0x3FF, 0x7FF, 0xFFF };
	int mask = 0xD5, seg, aval;
	x >>= 3;
	if (x < 0) {
		mask = 0x55;
		x = -x - 1;
	}
	seg = seg_search(x, seg_end);
	if (seg >= 8)
		return (unsigned char)(0x7F ^ mask);
	aval = seg << 4;
	aval |= seg < 2 ? (x >> 1) & 15 : (x >> seg) & 15;
	return (unsigned char)(aval ^ mask);
}

/* A-law to 16 bit linear */
static int alaw2lin(unsigned char a)
{
	int t, seg;
	a ^= 0x55;
	t = (a & 15) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else if (seg == 1)
		t += 0x108;
	else
		t = (t + 0x108) << (seg - 1);
	return (a & 0x80) ? t : -t;
}

/* for dmodem_fax.cpp: a G.711 octet (A-law if alaw, else mu-law) to
   16 bit linear */
int dmodem_g711_decode(int alaw, unsigned char c)
{
	return alaw ? alaw2lin(c) : ulaw2lin(c);
}

/* and 16 bit linear to a G.711 octet */
unsigned char dmodem_g711_encode(int alaw, int x)
{
	return alaw ? lin2alaw(x) : lin2ulaw(x);
}

/* 20 ms blocks: most of the power at 1100 Hz for 400 ms or more is a
   CNG burst (a voice or a modem doesn't hold one pure tone) */
#define CNG_BLOCK       160
#define CNG_COEF        1.298896f       /* 2 cos(2 pi 1100 / 8000) */

/* one sample of the line into the CNG detector: a Goertzel filter at
   1100 Hz over blocks of CNG_BLOCK samples; a block mostly of the tone
   (60 % of its energy, rms over 100) is on, and 20 to 33 of them in a
   row (0.4 to 0.66 s) ended by one off make a burst (c->bursts) */
static void cng_sample(struct cng_detect *c, float x)
{
	float s = x + CNG_COEF * c->s1 - c->s2;
	c->s2 = c->s1;
	c->s1 = s;
	c->energy += x * x;
	if (++c->n < CNG_BLOCK)
		return;
	{
		float tone = c->s1 * c->s1 + c->s2 * c->s2 -
			CNG_COEF * c->s1 * c->s2;
		/* the tone's share of the block's energy */
		float share = c->energy > 0 ?
			2 * tone / (CNG_BLOCK * c->energy) : 0;
		int on = share > 0.6f && c->energy / CNG_BLOCK > 100 * 100;
		if (on) {
			c->on_blocks++;
		} else {
			if (c->on_blocks >= 20 && c->on_blocks <= 33)
				c->bursts++;
			c->on_blocks = 0;
		}
	}
	c->s1 = c->s2 = c->energy = 0;
	c->n = 0;
}

/*
 *    the library
 */

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* the data pumps registered (once: pthread_once) */
static void init_all(void)
{
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
}

/* the pumps, once whatever the threads; 0 */
int dmodem_init(void)
{
	pthread_once(&init_once, init_all);
	return 0;
}

/* the core's log hook: one for all the modems of the process */
void dmodem_set_log(void (*fn)(const char *line))
{
	dcore::log_set_hook(fn);
}

/* the core's and the pumps' log levels (globals of the process) */
void dmodem_set_debug(int level)
{
	dcore::log_level = level;
	dsplibs_debug_level = level;
}

/* the core with d as its Host at DMODEM_RATE, the name cut to 31
   characters; cfg->init goes in as the DTE's input */
struct dmodem *dmodem_create(const struct dmodem_config *cfg)
{
	struct dmodem *d;
	dcore::Options o;

	dmodem_init();
	d = new dmodem;
	strncpy(d->name, cfg->name ? cfg->name : "dmodem",
		sizeof(d->name) - 1);
	d->dte_fd = cfg->dte_fd;
	d->law = cfg->law;
	o.name = d->name;
	o.dev_name = d->name;
	o.rate = DMODEM_RATE;
	d->m = new dcore::Modem(*d, o);
	if (!d->m->ok()) {
		delete d->m;
		delete d;
		return NULL;
	}
	if (cfg->init) {
		d->m->dte_input(cfg->init, strlen(cfg->init));
		d->m->poll();
	}
	return d;
}

/* on hook first if a call is on; d->dte_fd is left open */
void dmodem_destroy(struct dmodem *d)
{
	if (!d)
		return;
	if (d->m->started())
		d->m->hangup();
	delete d->m;
	delete d;
}

/* ATA: the core's answer(); the law goes to the pumps again with the
   next block */
int dmodem_answer(struct dmodem *d)
{
	d->law_set = 0;
	return d->m->answer();
}

/* ATD<number>\r as the DTE's command line; 0 (how it goes is the
   link's state), -1 for a number too long for a command line */
int dmodem_dial(struct dmodem *d, const char *number)
{
	char cmd[128];
	int n = snprintf(cmd, sizeof(cmd), "ATD%s\r", number);
	if (n < 0 || n >= (int)sizeof(cmd))
		return -1;
	d->law_set = 0;
	d->m->dte_input(cmd, n);
	d->m->poll();
	return 0;
}

/* ATH: the core's hangup(), its events run */
void dmodem_hangup(struct dmodem *d)
{
	d->m->hangup();
	d->m->poll();
}

/* n samples of the line: the law to the pumps once they exist, the CNG
   detector while not online, the core (silence out while on hook); the
   clock moves by n samples and the timers run */
void dmodem_process(struct dmodem *d, const short *in, short *out, int n)
{
	dcore::Modem *m = d->m;

	if (!d->law_set && m->dp_runtime()) {
		dp_runtime_set_law(m->dp_runtime(), d->law);
		d->law_set = 1;
	}
	/* a fax machine calling, while there is no connection */
	if (m->link_state() != dcore::LinkState::Online) {
		int i;
		for (i = 0; i < n; i++)
			cng_sample(&d->cng, in[i]);
	}
	if (m->started())
		m->process(in, out, n);
	else
		memset(out, 0, n * sizeof(short));
	d->samples += n;
	m->poll();
}

/* G.711 octets through dmodem_process(), 160 at a time */
void dmodem_process_g711(struct dmodem *d, const unsigned char *in,
			 unsigned char *out, int n)
{
	short lin_in[160], lin_out[160];
	int alaw = d->law == DMODEM_LAW_ALAW, i, k;

	while (n > 0) {
		k = n < 160 ? n : 160;
		for (i = 0; i < k; i++)
			lin_in[i] = alaw ? alaw2lin(in[i]) : ulaw2lin(in[i]);
		dmodem_process(d, lin_in, lin_out, k);
		for (i = 0; i < k; i++)
			out[i] = alaw ? lin2alaw(lin_out[i]) :
				lin2ulaw(lin_out[i]);
		in += k;
		out += k;
		n -= k;
	}
}

/* what the DTE wrote, up to the room the core has (1024 at most): the
   bytes taken, 0 if none (or no room), -1 at end of file or on an
   error */
int dmodem_poll_dte(struct dmodem *d)
{
	dcore::Modem *m = d->m;
	char buf[1024];
	int n = m->dte_space();

	m->poll();
	if (n <= 0)
		return 0;
	if (n > (int)sizeof(buf))
		n = sizeof(buf);
	n = read(d->dte_fd, buf, n);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (n == 0)
		return -1;
	m->dte_input(buf, n);
	m->poll();
	return n;
}

/* the core's state into *s: tx_pending one more while V.42 has frames
   unacknowledged; fax_caller latched by a CNG burst (or 0.4 s of one) */
void dmodem_status(struct dmodem *d, struct dmodem_status *s)
{
	dcore::Modem *m = d->m;
	s->link = (enum dmodem_link)m->link_state();
	s->offhook = m->started();
	s->tx_rate = m->tx_rate();
	s->rx_rate = m->rx_rate();
	s->tx_pending = m->xmit_count();
	/* V.42: frames not sent yet or not acknowledged */
	if (m->link_tx_pending())
		s->tx_pending++;
	s->fax_caller = d->cng.bursts > 0 || d->cng.on_blocks >= 20;
}
