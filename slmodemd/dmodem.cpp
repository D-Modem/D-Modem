/*
 *
 *    dmodem.cpp - the modem as a library (see dmodem.h)
 *
 *    The modem core of slmodemd runs as it does there, with a driver of
 *    its own here: the audio comes and goes through dmodem_process(), the
 *    DTE through a file descriptor of the caller's, the timers through
 *    dmodem_timer.cpp.
 *
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "modem.h"
#include "modem_debug.h"
#include "dmodem.h"
#include "dsplibs.h"

#if MODEM_RATE != DMODEM_RATE
#error "the library is built with MODEM_RATE=8000"
#endif

extern int  dp_dummy_init(void);
extern int  dp_sinus_init(void);
extern void dp_runtime_set_law(void *runtime, int law);
extern void dmodem_run_timers(void *owner);
extern void dmodem_set_time(unsigned long ticks);
extern int  modem_answer(struct modem *m);

#ifndef ENOIOCTLCMD
#define ENOIOCTLCMD 515
#endif

/* a calling fax machine's CNG: 1100 Hz, 0.5 s on, 3 s off */
struct cng_detect {
	float s1, s2, energy;
	int n, on_blocks, bursts;
};

struct dmodem {
	struct modem *m;
	enum dmodem_law law;
	int law_set;
	unsigned long long samples;     /* the modem's clock: its audio */
	char name[32];
	struct cng_detect cng;
};

/*
 *    the driver: nothing to start, the audio is the caller's
 */

static int drv_start(struct modem *m)
{
	return 0;
}

static int drv_stop(struct modem *m)
{
	return 0;
}

static int drv_ioctl(struct modem *m, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case MDMCTL_CAPABILITIES:
		return -EINVAL;
	case MDMCTL_CODECTYPE:
		return 0;
	case MDMCTL_IODELAY:
		return 0;
	case MDMCTL_HOOKSTATE:
	case MDMCTL_SPEED:
	case MDMCTL_GETFMTS:
	case MDMCTL_SETFMT:
	case MDMCTL_SETFRAGMENT:
	case MDMCTL_START:
	case MDMCTL_STOP:
	case MDMCTL_GETSTAT:
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}

static struct modem_driver dmodem_driver = {
	.name = "dmodem",
	.start = drv_start,
	.stop = drv_stop,
	.ioctl = drv_ioctl,
};

/* G.711 (the usual reference algorithms) */
static int seg_search(int v, const short *t)
{
	int i;
	for (i = 0; i < 8; i++)
		if (v <= t[i])
			return i;
	return 8;
}

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

static int ulaw2lin(unsigned char u)
{
	int t;
	u = ~u;
	t = ((u & 15) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (u & 0x80) ? 0x84 - t : t - 0x84;
}

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

int dmodem_g711_decode(int alaw, unsigned char c)
{
	return alaw ? alaw2lin(c) : ulaw2lin(c);
}

unsigned char dmodem_g711_encode(int alaw, int x)
{
	return alaw ? lin2alaw(x) : lin2ulaw(x);
}

/* 20 ms blocks: most of the power at 1100 Hz for 400 ms or more is a
   CNG burst (a voice or a modem doesn't hold one pure tone) */
#define CNG_BLOCK       160
#define CNG_COEF        1.298896f       /* 2 cos(2 pi 1100 / 8000) */

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

static void init_all(void)
{
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	modem_timer_init();
}

int dmodem_init(void)
{
	pthread_once(&init_once, init_all);
	return 0;
}

void dmodem_set_log(void (*fn)(const char *line))
{
	modem_debug_hook = fn;
}

void dmodem_set_debug(int level)
{
	modem_debug_level = level;
	dsplibs_debug_level = level;
}

/* the core's time (MODEM_HZ ticks) is the audio of this modem */
static void enter(struct dmodem *d)
{
	dmodem_set_time((unsigned long)(d->samples * MODEM_HZ / DMODEM_RATE));
}

static void run_events(struct dmodem *d)
{
	dmodem_run_timers(d->m);
	if (d->m->event)
		modem_event(d->m);
}

struct dmodem *dmodem_create(const struct dmodem_config *cfg)
{
	struct dmodem *d;
	struct modem *m;

	dmodem_init();
	d = static_cast<dmodem*>(calloc(1, sizeof(*d)));
	if (!d)
		return NULL;
	strncpy(d->name, cfg->name ? cfg->name : "dmodem",
		sizeof(d->name) - 1);
	enter(d);
	m = modem_create(&dmodem_driver, d->name);
	if (!m) {
		free(d);
		return NULL;
	}
	d->m = m;
	d->law = cfg->law;
	m->pty = cfg->dte_fd;
	m->dev_data = d;
	m->dev_name = d->name;
	/* the DTE: raw, as a pty in raw mode would be */
	tcgetattr(m->pty, &m->termios);
	if (cfg->init) {
		modem_write(m, cfg->init, strlen(cfg->init));
		run_events(d);
	}
	return d;
}

void dmodem_destroy(struct dmodem *d)
{
	if (!d)
		return;
	enter(d);
	if (d->m->started)
		modem_hangup(d->m);
	modem_delete(d->m);
	free(d);
}

int dmodem_answer(struct dmodem *d)
{
	enter(d);
	d->law_set = 0;
	return modem_answer(d->m);
}

int dmodem_dial(struct dmodem *d, const char *number)
{
	char cmd[128];
	int n = snprintf(cmd, sizeof(cmd), "ATD%s\r", number);
	enter(d);
	d->law_set = 0;
	modem_write(d->m, cmd, n);
	run_events(d);
	return 0;
}

void dmodem_hangup(struct dmodem *d)
{
	enter(d);
	modem_hangup(d->m);
	run_events(d);
}

void dmodem_process(struct dmodem *d, const short *in, short *out, int n)
{
	struct modem *m = d->m;

	enter(d);
	if (!d->law_set && m->dp_runtime) {
		dp_runtime_set_law(m->dp_runtime, d->law);
		d->law_set = 1;
	}
	/* a fax machine calling, while there is no connection (before the
	   modem, which may use the samples in as its scratch) */
	if (modem_link_state(m) != DMODEM_ONLINE) {
		int i;
		for (i = 0; i < n; i++)
			cng_sample(&d->cng, in[i]);
	}
	if (m->started)
		modem_process(m, (void *)in, out, n);
	else
		memset(out, 0, n * sizeof(short));
	d->samples += n;
	enter(d);
	run_events(d);
}

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

int dmodem_poll_dte(struct dmodem *d)
{
	struct modem *m = d->m;
	char buf[1024];
	int n = m->xmit.size - m->xmit.count;

	enter(d);
	run_events(d);
	if (n <= 0)
		return 0;
	if (n > (int)sizeof(buf))
		n = sizeof(buf);
	n = read(m->pty, buf, n);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (n == 0)
		return -1;
	modem_write(m, buf, n);
	run_events(d);
	return n;
}

void dmodem_status(struct dmodem *d, struct dmodem_status *s)
{
	enter(d);
	struct modem *m = d->m;
	s->link = (enum dmodem_link)modem_link_state(m);
	s->offhook = m->started;
	s->tx_rate = m->tx_rate;
	s->rx_rate = m->rx_rate;
	s->tx_pending = m->xmit.count;
	{
		/* V.42: frames not sent yet or not acknowledged (from the
		   oldest unacknowledged to the next free one) */
		struct lapm_state *l = &m->ec.lapm;
		if (l->sent_info && l->info_list &&
		    l->sent_info != l->info_list)
			s->tx_pending++;
	}
	s->fax_caller = d->cng.bursts > 0 || d->cng.on_blocks >= 20;
}
