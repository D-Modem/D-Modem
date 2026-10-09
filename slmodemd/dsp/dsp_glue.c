/*
 *
 *    dsp_glue.c - entry points used by the modem core: data pump
 *                 registration, DC removal, the shared runtime object,
 *                 ring detector, caller ID, voice and fax objects.
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

extern int modem_send_to_tty(struct modem *m, const char *buf, int n);
extern int modem_recv_from_tty(struct modem *m, char *buf, int n);


/*
 *    data pumps
 */

int prop_dp_init(void)
{
	dp_callprog_init();
	dp_fsk_init();
	dp_v22_init();
	dp_v32_init();
	return 0;
}

void prop_dp_exit(void)
{
	dp_callprog_exit();
	dp_fsk_exit();
	dp_v22_exit();
	dp_v32_exit();
}


/*
 *    DC removal, the same algorithm as the original dcr_* code:
 *    skip the first 0.6 s, average the next second for the initial
 *    estimate, then refresh it every 2 s with a 0.9/0.1 smoothing.
 */

#define DCR_ENABLE   0x1
#define DCR_SUBTRACT 0x2
#define DCR_TRACK    0x4

struct dcr {
	unsigned flags;
	int state;
	s16 dc;
	s16 threshold;
	int sum;
	int count;
	int skip_len, init_len, update_len;
};

void *dcr_create(void)
{
	struct dcr *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->flags = DCR_ENABLE | DCR_SUBTRACT | DCR_TRACK;
	d->state = 0;
	d->skip_len = 5760;
	d->init_len = 9600;
	d->update_len = 19200;
	d->threshold = 3000;
	return d;
}

void dcr_delete(void *obj)
{
	free(obj);
}

void dcr_reset(void *obj)
{
	struct dcr *d = obj;
	d->dc = 0;
	d->sum = 0;
	d->count = 0;
}

int dcr_process(void *obj, void *buf, int len)
{
	struct dcr *d = obj;
	s16 *s = buf;
	int ret = 0, i;

	if (!d || !(d->flags & DCR_ENABLE))
		return 0;
	if (d->state >= 2) {
		/* ignore muted buffers once running */
		for (i = 0; i < len && !s[i]; i++)
			;
		if (i == len)
			return 0;
	}
	switch (d->state) {
	case 0:
		d->count += len;
		if (d->count >= d->skip_len) {
			d->state = 1;
			d->count = 0;
			d->sum = 0;
		}
		break;
	case 1:
		for (i = 0; i < len; i++)
			d->sum += s[i];
		d->count += len;
		if (d->count >= d->init_len) {
			d->dc = d->sum / d->count;
			DSP_DBG("dcr: dc %d\n", d->dc);
			d->state = (d->flags & DCR_TRACK) ? 2 : 3;
			d->sum = 0;
			d->count = 0;
		}
		break;
	case 2:
		for (i = 0; i < len; i++)
			d->sum += s[i];
		d->count += len;
		if (d->count >= d->update_len) {
			d->dc = (d->dc * 0x7333 + (d->sum / d->count) * 0xccd +
				 0x4000) >> 15;
			d->sum = 0;
			d->count = 0;
		}
		break;
	default:
		break;
	}
	if (abs(d->dc) >= d->threshold)
		ret = 1;
	if (d->flags & DCR_SUBTRACT && d->dc)
		for (i = 0; i < len; i++)
			s[i] = dsp_clip16((float)s[i] - d->dc);
	return ret;
}


/*
 *    runtime object shared by the data pumps of one call
 */

void *dp_runtime_create(struct modem *m)
{
	struct dp_runtime *rt = calloc(1, sizeof(*rt));
	if (rt)
		rt->modem = m;
	return rt;
}

void dp_runtime_delete(void *runtime)
{
	free(runtime);
}


/*
 *    Ring detector: the ring signal seen through a snooping codec is a
 *    large low frequency (15..70 Hz) wave.  RD_process returns 1 at the
 *    start and at the end of each ring burst; RD_ring_details then
 *    reports freq == 0 for a start, or the frequency and the length in
 *    ms of the finished burst.
 */

struct ring_detector {
	struct modem *modem;
	int srate;
	struct biquad lp;
	float env;
	float env_decay;
	int on;
	long on_samples, off_samples;
	int last_sign;
	long crossings;
	long freq, duration;
};

void *RD_create(struct modem *m, unsigned rate)
{
	struct ring_detector *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->modem = m;
	r->srate = rate;
	biquad_lowpass(&r->lp, 80, 0.707f, rate);
	r->env_decay = 1.0f / (0.1f * rate);
	return r;
}

void RD_delete(void *obj)
{
	free(obj);
}

int RD_process(void *obj, void *in, int count)
{
	struct ring_detector *r = obj;
	s16 *s = in;
	int i, ret = 0;

	for (i = 0; i < count; i++) {
		float x = biquad_run(&r->lp, s[i]);
		int sign = x > 0;
		float a = fabsf(x);

		if (a > r->env)
			r->env = a;
		else
			r->env -= r->env_decay * r->env;
		if (sign != r->last_sign && a > 100) {
			r->crossings++;
			r->last_sign = sign;
		}
		if (!r->on) {
			if (r->env > 4000) {
				if (++r->on_samples > ms2samples(50, r->srate)) {
					r->on = 1;
					r->off_samples = 0;
					r->crossings = 0;
					r->freq = 0;
					r->duration = 0;
					ret = 1;
				}
			} else
				r->on_samples = 0;
		} else {
			r->on_samples++;
			if (r->env < 1500) {
				if (++r->off_samples > ms2samples(150, r->srate)) {
					long len = r->on_samples - r->off_samples;
					r->on = 0;
					r->on_samples = 0;
					r->duration = len * 1000 / r->srate;
					r->freq = len > 0 ?
						r->crossings * r->srate / (2 * len) : 1;
					if (r->freq <= 0)
						r->freq = 1;
					ret = 1;
				}
			} else
				r->off_samples = 0;
		}
	}
	return ret;
}

void RD_ring_details(void *obj, long *freq, long *duration)
{
	struct ring_detector *r = obj;
	*freq = r->freq;
	*duration = r->duration;
}


/*
 *    Caller ID: Bell 202 FSK (1200 bit/s, mark 1200 Hz, space 2200 Hz)
 *    with SDMF (0x04) and MDMF (0x80) messages.  cid_val 1 reports the
 *    formatted fields, other values report the raw message in hex.
 */

#define CID_MAX_MSG 258

struct cid {
	struct modem *modem;
	unsigned format;
	struct fsk_demod fsk;
	/* UART */
	int in_char, nbits;
	unsigned shift;
	int marks;
	/* message */
	u8 msg[CID_MAX_MSG];
	int len;
};

void *CID_create(struct modem *m, unsigned rate, unsigned cid_val)
{
	struct cid *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->modem = m;
	c->format = cid_val;
	fsk_demod_init(&c->fsk, 1200, 2200, 1200, rate);
	DSP_DBG("cid: create, format %u\n", cid_val);
	return c;
}

void CID_delete(void *obj)
{
	free(obj);
}

static void cid_send(struct cid *c, const char *s)
{
	modem_send_to_tty(c->modem, s, strlen(s));
}

static void cid_field(struct cid *c, const char *tag, const u8 *p, int n)
{
	char buf[128];
	int k = snprintf(buf, sizeof(buf), "%s = ", tag);
	while (n-- > 0 && k < (int)sizeof(buf) - 3)
		buf[k++] = (*p >= 0x20 && *p < 0x7f) ? *p : '?', p++;
	buf[k++] = '\r';
	buf[k++] = '\n';
	buf[k] = 0;
	cid_send(c, buf);
}

static void cid_report(struct cid *c)
{
	const u8 *p = c->msg + 2;
	int n = c->msg[1];
	char buf[8];

	cid_send(c, "\r\n");
	if (c->format != 1) {
		int i;
		cid_send(c, "MESG = ");
		for (i = 0; i < c->len; i++) {
			sprintf(buf, "%02X", c->msg[i]);
			cid_send(c, buf);
		}
		cid_send(c, "\r\n");
		return;
	}
	if (c->msg[0] == 0x04) {        /* SDMF: MMDDHHMM number */
		if (n >= 8) {
			cid_field(c, "DATE", p, 4);
			cid_field(c, "TIME", p + 4, 4);
			cid_field(c, "NMBR", p + 8, n - 8);
		}
		return;
	}
	while (n >= 2) {                /* MDMF parameters */
		int t = p[0], l = p[1];
		if (l + 2 > n)
			break;
		switch (t) {
		case 0x01:
			if (l >= 8) {
				cid_field(c, "DATE", p + 2, 4);
				cid_field(c, "TIME", p + 6, 4);
			}
			break;
		case 0x02: cid_field(c, "NMBR", p + 2, l); break;
		case 0x04: cid_field(c, "NMBR", (const u8 *)"O", 1); break;
		case 0x07: cid_field(c, "NAME", p + 2, l); break;
		case 0x08: cid_field(c, "NAME", (const u8 *)"O", 1); break;
		default: break;
		}
		p += l + 2;
		n -= l + 2;
	}
}

/* returns 0 to continue, 1 when a message was reported, -1 on error */
static int cid_byte(struct cid *c, u8 b)
{
	int i;
	u8 sum = 0;

	if (c->len == 0 && b != 0x04 && b != 0x80)
		return 0;
	c->msg[c->len++] = b;
	if (c->len < 2 || c->len < c->msg[1] + 3)
		return 0;
	for (i = 0; i < c->len; i++)
		sum += c->msg[i];
	if (sum) {
		DSP_DBG("cid: checksum error\n");
		return -1;
	}
	cid_report(c);
	return 1;
}

int CID_process(void *obj, void *in, int count)
{
	struct cid *c = obj;
	s16 *s = in;
	int i, bit, r;

	for (i = 0; i < count; i++) {
		bit = fsk_demod_run(&c->fsk, s[i]);
		if (bit < 0 || !c->fsk.carrier)
			continue;
		if (!c->in_char) {
			/* start bit after the mark preamble */
			if (bit) {
				c->marks++;
				continue;
			}
			if (c->marks < 10 && c->len == 0)
				continue;
			c->in_char = 1;
			c->nbits = 0;
			c->shift = 0;
			continue;
		}
		if (c->nbits < 8) {
			c->shift |= bit << c->nbits;
			c->nbits++;
			continue;
		}
		/* stop bit */
		c->in_char = 0;
		c->marks = 0;
		if (!bit)
			continue;       /* framing error */
		r = cid_byte(c, c->shift);
		if (r)
			return r;
		if (c->len >= CID_MAX_MSG)
			return -1;
	}
	return 0;
}


/*
 *    Voice: IS-101 style streaming of 8 bit unsigned linear samples at
 *    8000 Hz with <DLE> shielding, tone (+VTS) generation.
 */

#define DLE 0x10
#define ETX 0x03
#define DC4 0x14   /* <DLE><DC4> could be sent by the DTE as well */

struct voice {
	struct modem *modem;
	int srate;
	unsigned state;
	int pending;            /* status to return from VOICE_process */
	/* resampling between srate and 8000 Hz */
	double rx_pos, tx_pos;
	float tx_prev, tx_cur;
	int dle;
	int tx_end;
	/* tones */
	long tone_left;
	struct osc t1, t2;
	int tone2;
	struct dtmf_gen dtmf;
	int tone_dtmf;
};

void *VOICE_create(struct modem *m, unsigned srate)
{
	struct voice *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->modem = m;
	v->srate = srate;
	v->state = VOICE_STATE_COMMAND;
	DSP_DBG("voice: VOICE_create...\n");
	return v;
}

void VOICE_delete(void *obj)
{
	DSP_DBG("voice: VOICE_delete...\n");
	free(obj);
}

static struct voice_info *voice_info(struct voice *v)
{
	return (struct voice_info *)modem_get_param(v->modem, MDMPRM_VOICEINFO);
}

int VOICE_command(void *obj, enum VOICE_CMD cmd)
{
	struct voice *v = obj;
	struct voice_info *vi = voice_info(v);

	DSP_DBG("voice: command %d\n", cmd);
	switch (cmd) {
	case VOICE_CMD_STATE_RX:
	case VOICE_CMD_STATE_TX:
	case VOICE_CMD_STATE_DUPLEX:
		v->state = cmd == VOICE_CMD_STATE_RX ? VOICE_STATE_RX :
			cmd == VOICE_CMD_STATE_TX ? VOICE_STATE_TX :
			VOICE_STATE_DUPLEX;
		v->dle = 0;
		v->tx_end = 0;
		v->pending = VOICE_STATUS_CONNECT;
		return 0;
	case VOICE_CMD_BEEP:
	case VOICE_CMD_DTMF:
		if (!vi)
			return -1;
		v->tone_left = (long)vi->tone_duration * v->srate / 100;
		v->tone_dtmf = cmd == VOICE_CMD_DTMF;
		if (v->tone_dtmf) {
			if (dtmf_gen_set(&v->dtmf, vi->dtmf_symbol, v->srate))
				v->tone_left = 0;
		} else {
			osc_set_freq(&v->t1, vi->tone1_freq, v->srate);
			osc_set_freq(&v->t2, vi->tone2_freq, v->srate);
			v->tone2 = vi->tone2_freq != 0;
		}
		if (!v->tone_left)
			v->pending = VOICE_STATUS_OK;
		return 0;
	case VOICE_CMD_ABORT:
	case VOICE_CMD_STATE_COMMAND:
		v->state = VOICE_STATE_COMMAND;
		v->tone_left = 0;
		v->pending = VOICE_STATUS_OK;
		return 0;
	default:
		return -1;
	}
}

static int voice_get_tx_sample(struct voice *v, float *x)
{
	char ch;
	while (!v->tx_end && modem_recv_from_tty(v->modem, &ch, 1) == 1) {
		u8 c = ch;
		if (v->dle) {
			v->dle = 0;
			if (c == ETX) {
				v->tx_end = 1;
				break;
			}
			if (c == DLE) {
				*x = ((int)c - 128) * 256.0f;
				return 1;
			}
			if (c == '!' || c == 0x18) {    /* abort, <CAN> */
				v->tx_end = 1;
				break;
			}
			continue;
		}
		if (c == DLE) {
			v->dle = 1;
			continue;
		}
		*x = ((int)c - 128) * 256.0f;
		return 1;
	}
	return 0;
}

int VOICE_process(void *obj, void *in, void *out, int count)
{
	struct voice *v = obj;
	s16 *src = in, *dst = out;
	double step = 8000.0 / v->srate;
	char rxbuf[512];
	int nrx = 0, i, status;

	for (i = 0; i < count; i++) {
		float y = 0;

		if (v->tone_left > 0) {
			if (v->tone_dtmf)
				y = dtmf_gen_next(&v->dtmf);
			else {
				y = DSP_TX_AMPLITUDE * osc_next(&v->t1);
				if (v->tone2)
					y = 0.5f * y + 0.5f * DSP_TX_AMPLITUDE *
						osc_next(&v->t2);
			}
			if (--v->tone_left == 0)
				v->pending = VOICE_STATUS_OK;
		} else if (v->state & VOICE_STATE_TX && !v->tx_end) {
			/* linear interpolation from 8000 Hz */
			v->tx_pos += step;
			while (v->tx_pos >= 1) {
				float x;
				v->tx_pos -= 1;
				v->tx_prev = v->tx_cur;
				if (voice_get_tx_sample(v, &x))
					v->tx_cur = x;
				else if (v->tx_end) {
					DSP_DBG("voice: dle command: ETX\n");
					v->state &= ~VOICE_STATE_TX;
					v->pending = VOICE_STATUS_OK;
					break;
				}
			}
			y = v->tx_prev + (float)v->tx_pos * (v->tx_cur - v->tx_prev);
		}
		dst[i] = dsp_clip16(y);

		if (v->state & VOICE_STATE_RX) {
			v->rx_pos += step;
			if (v->rx_pos >= 1) {
				int c = (src[i] >> 8) + 128;
				v->rx_pos -= 1;
				if (nrx < (int)sizeof(rxbuf) - 2) {
					rxbuf[nrx++] = c;
					if (c == DLE)
						rxbuf[nrx++] = DLE;
				}
			}
		}
	}
	if (nrx)
		modem_send_to_tty(v->modem, rxbuf, nrx);

	/* any character from the DTE ends receive only mode */
	if (v->state == VOICE_STATE_RX) {
		char ch;
		if (modem_recv_from_tty(v->modem, &ch, 1) == 1) {
			static const char end[2] = { DLE, ETX };
			modem_send_to_tty(v->modem, end, 2);
			v->state = VOICE_STATE_COMMAND;
			v->pending = VOICE_STATUS_OK;
		}
	}

	status = v->pending;
	v->pending = 0;
	return status;
}


/*
 *    Fax: class 1 fax modulations (V.27ter, V.29, V.17 and V.21 HDLC)
 *    are not implemented by this library.
 */

void *FAX_create(struct modem *m, unsigned caller, unsigned srate)
{
	DSP_DBG("fax: not supported\n");
	return NULL;
}

void FAX_delete(void *obj)
{
	free(obj);
}

int FAX_process(void *obj, void *in, void *out, int count)
{
	memset(out, 0, count * sizeof(s16));
	return FAX_STATUS_ERROR;
}

int FAX_class1_command(void *obj, unsigned cmd, unsigned param)
{
	return -1;
}
