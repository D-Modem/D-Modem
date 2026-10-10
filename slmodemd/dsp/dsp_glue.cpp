/*
 *
 *    dsp_glue.cpp - entry points used by the modem core: data pump
 *                 registration, DC removal, the shared runtime object,
 *                 ring detector, caller ID, voice and fax objects.
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "dsp.h"
#include <dsplibs.h>

using namespace dsp;



/*
 *    data pumps
 */

int prop_dp_init(void)
{
	dp_callprog_init();
	dp_fsk_init();
	dp_v22_init();
	dp_v32_init();
	dp_v34_init();
	return 0;
}

void prop_dp_exit(void)
{
	dp_callprog_exit();
	dp_fsk_exit();
	dp_v22_exit();
	dp_v32_exit();
	dp_v34_exit();
}


/*
 *    DC removal, the same algorithm as the original dcr_* code:
 *    skip the first 0.6 s, average the next second for the initial
 *    estimate, then refresh it every 2 s with a 0.9/0.1 smoothing.
 */

#define DCR_ENABLE   0x1
#define DCR_SUBTRACT 0x2
#define DCR_TRACK    0x4

struct DcRemover {
	unsigned flags;
	int state;
	s16 dc;
	s16 threshold;
	int sum;
	int count;
	int skip_len, init_len, update_len;

	void reset();
	int process(void *buf, int len);
};

void *dcr_create(void)
{
	DcRemover *d = new (std::nothrow) DcRemover();
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
	delete static_cast<DcRemover*>(obj);
}

void DcRemover::reset()
{
	dc = 0;
	sum = 0;
	count = 0;
}

void dcr_reset(void *obj)
{
	static_cast<DcRemover*>(obj)->reset();
}

int DcRemover::process(void *buf, int len)
{
	s16 *s = static_cast<s16*>(buf);
	int ret = 0, i;

	if (!(flags & DCR_ENABLE))
		return 0;
	if (state >= 2) {
		/* ignore muted buffers once running */
		for (i = 0; i < len && !s[i]; i++)
			;
		if (i == len)
			return 0;
	}
	switch (state) {
	case 0:
		count += len;
		if (count >= skip_len) {
			state = 1;
			count = 0;
			sum = 0;
		}
		break;
	case 1:
		for (i = 0; i < len; i++)
			sum += s[i];
		count += len;
		if (count >= init_len) {
			dc = sum / count;
			DSP_DBG("dcr: dc %d\n", dc);
			state = (flags & DCR_TRACK) ? 2 : 3;
			sum = 0;
			count = 0;
		}
		break;
	case 2:
		for (i = 0; i < len; i++)
			sum += s[i];
		count += len;
		if (count >= update_len) {
			dc = (dc * 0x7333 + (sum / count) * 0xccd +
				 0x4000) >> 15;
			sum = 0;
			count = 0;
		}
		break;
	default:
		break;
	}
	if (abs(dc) >= threshold)
		ret = 1;
	if (flags & DCR_SUBTRACT && dc)
		for (i = 0; i < len; i++)
			s[i] = dsp_clip16((float)s[i] - dc);
	return ret;
}

int dcr_process(void *obj, void *buf, int len)
{
	if (!obj)
		return 0;
	return static_cast<DcRemover*>(obj)->process(buf, len);
}


/*
 *    runtime object shared by the data pumps of one call
 */

void *dp_runtime_create(struct modem *m)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(calloc(1, sizeof(*rt)));
	if (rt)
		rt->modem = m;
	return rt;
}

/* the G.711 law of the network (dmodem.cpp) */
void dp_runtime_set_law(void *runtime, int law)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(runtime);
	if (rt)
		rt->law = law;
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

struct RingDetector {
	struct modem *modem;
	int srate;
	Biquad lp;
	float env;
	float env_decay;
	int on;
	long on_samples, off_samples;
	int last_sign;
	long crossings;
	long freq, duration;

	int process(void *in, int count);
	void details(long *freq_, long *duration_);
};

void *RD_create(struct modem *m, unsigned rate)
{
	RingDetector *r = new (std::nothrow) RingDetector();
	if (!r)
		return NULL;
	r->modem = m;
	r->srate = rate;
	r->lp.lowpass(80, 0.707f, rate);
	r->env_decay = 1.0f / (0.1f * rate);
	return r;
}

void RD_delete(void *obj)
{
	delete static_cast<RingDetector*>(obj);
}

int RingDetector::process(void *in, int count)
{
	s16 *s = static_cast<s16*>(in);
	int i, ret = 0;

	for (i = 0; i < count; i++) {
		float x = lp.run(s[i]);
		int sign = x > 0;
		float a = fabsf(x);

		if (a > env)
			env = a;
		else
			env -= env_decay * env;
		if (sign != last_sign && a > 100) {
			crossings++;
			last_sign = sign;
		}
		if (!on) {
			if (env > 4000) {
				if (++on_samples > ms2samples(50, srate)) {
					on = 1;
					off_samples = 0;
					crossings = 0;
					freq = 0;
					duration = 0;
					ret = 1;
				}
			} else
				on_samples = 0;
		} else {
			on_samples++;
			if (env < 1500) {
				if (++off_samples > ms2samples(150, srate)) {
					long len = on_samples - off_samples;
					on = 0;
					on_samples = 0;
					duration = len * 1000 / srate;
					freq = len > 0 ?
						crossings * srate / (2 * len) : 1;
					if (freq <= 0)
						freq = 1;
					ret = 1;
				}
			} else
				off_samples = 0;
		}
	}
	return ret;
}

int RD_process(void *obj, void *in, int count)
{
	return static_cast<RingDetector*>(obj)->process(in, count);
}

void RingDetector::details(long *freq_, long *duration_)
{
	*freq_ = freq;
	*duration_ = duration;
}

void RD_ring_details(void *obj, long *freq, long *duration)
{
	static_cast<RingDetector*>(obj)->details(freq, duration);
}


/*
 *    Caller ID: Bell 202 FSK (1200 bit/s, mark 1200 Hz, space 2200 Hz)
 *    with SDMF (0x04) and MDMF (0x80) messages.  cid_val 1 reports the
 *    formatted fields, other values report the raw message in hex.
 */

#define CID_MAX_MSG 258

struct CallerId {
	struct modem *modem;
	unsigned format;
	FskDemod fsk;
	/* UART */
	int in_char, nbits;
	unsigned shift;
	int marks;
	/* message */
	u8 msg[CID_MAX_MSG];
	int len;

	void send(const char *s);
	void field(const char *tag, const u8 *p, int n);
	void report();
	int byte(u8 b);
	int process(void *in, int count);
};

void *CID_create(struct modem *m, unsigned rate, unsigned cid_val)
{
	CallerId *c = new (std::nothrow) CallerId();
	if (!c)
		return NULL;
	c->modem = m;
	c->format = cid_val;
	c->fsk.init(1200, 2200, 1200, rate);
	DSP_DBG("cid: create, format %u\n", cid_val);
	return c;
}

void CID_delete(void *obj)
{
	delete static_cast<CallerId*>(obj);
}

void CallerId::send(const char *s)
{
	modem_send_to_tty(modem, s, strlen(s));
}

void CallerId::field(const char *tag, const u8 *p, int n)
{
	char buf[128];
	int k = snprintf(buf, sizeof(buf), "%s = ", tag);
	while (n-- > 0 && k < (int)sizeof(buf) - 3)
		buf[k++] = (*p >= 0x20 && *p < 0x7f) ? *p : '?', p++;
	buf[k++] = '\r';
	buf[k++] = '\n';
	buf[k] = 0;
	send(buf);
}

void CallerId::report()
{
	const u8 *p = msg + 2;
	int n = msg[1];
	char buf[8];

	send("\r\n");
	if (format != 1) {
		int i;
		send("MESG = ");
		for (i = 0; i < len; i++) {
			sprintf(buf, "%02X", msg[i]);
			send(buf);
		}
		send("\r\n");
		return;
	}
	if (msg[0] == 0x04) {        /* SDMF: MMDDHHMM number */
		if (n >= 8) {
			field("DATE", p, 4);
			field("TIME", p + 4, 4);
			field("NMBR", p + 8, n - 8);
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
				field("DATE", p + 2, 4);
				field("TIME", p + 6, 4);
			}
			break;
		case 0x02: field("NMBR", p + 2, l); break;
		case 0x04: field("NMBR", (const u8 *)"O", 1); break;
		case 0x07: field("NAME", p + 2, l); break;
		case 0x08: field("NAME", (const u8 *)"O", 1); break;
		default: break;
		}
		p += l + 2;
		n -= l + 2;
	}
}

/* returns 0 to continue, 1 when a message was reported, -1 on error */
int CallerId::byte(u8 b)
{
	int i;
	u8 sum = 0;

	if (len == 0 && b != 0x04 && b != 0x80)
		return 0;
	msg[len++] = b;
	if (len < 2 || len < msg[1] + 3)
		return 0;
	for (i = 0; i < len; i++)
		sum += msg[i];
	if (sum) {
		DSP_DBG("cid: checksum error\n");
		return -1;
	}
	report();
	return 1;
}

int CallerId::process(void *in, int count)
{
	s16 *s = static_cast<s16*>(in);
	int i, bit, r;

	for (i = 0; i < count; i++) {
		bit = fsk.run(s[i]);
		if (bit < 0 || !fsk.carrier)
			continue;
		if (!in_char) {
			/* start bit after the mark preamble */
			if (bit) {
				marks++;
				continue;
			}
			if (marks < 10 && len == 0)
				continue;
			in_char = 1;
			nbits = 0;
			shift = 0;
			continue;
		}
		if (nbits < 8) {
			shift |= bit << nbits;
			nbits++;
			continue;
		}
		/* stop bit */
		in_char = 0;
		marks = 0;
		if (!bit)
			continue;       /* framing error */
		r = byte(shift);
		if (r)
			return r;
		if (len >= CID_MAX_MSG)
			return -1;
	}
	return 0;
}

int CID_process(void *obj, void *in, int count)
{
	return static_cast<CallerId*>(obj)->process(in, count);
}


/*
 *    Voice: IS-101 style streaming of 8 bit unsigned linear samples at
 *    8000 Hz with <DLE> shielding, tone (+VTS) generation.
 */

#define DLE 0x10
#define ETX 0x03
#define DC4 0x14   /* <DLE><DC4> could be sent by the DTE as well */

struct Voice {
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
	Osc t1, t2;
	int tone2;
	DtmfGen dtmf;
	int tone_dtmf;

	struct voice_info *info();
	int get_tx_sample(float *x);
	int command(enum VOICE_CMD cmd);
	int process(void *in, void *out, int count);
};

void *VOICE_create(struct modem *m, unsigned srate)
{
	Voice *v = new (std::nothrow) Voice();
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
	delete static_cast<Voice*>(obj);
}

struct voice_info *Voice::info()
{
	return (struct voice_info *)modem_get_param(modem, MDMPRM_VOICEINFO);
}

int Voice::command(enum VOICE_CMD cmd)
{
	struct voice_info *vi = info();

	DSP_DBG("voice: command %d\n", cmd);
	switch (cmd) {
	case VOICE_CMD_STATE_RX:
	case VOICE_CMD_STATE_TX:
	case VOICE_CMD_STATE_DUPLEX:
		state = cmd == VOICE_CMD_STATE_RX ? VOICE_STATE_RX :
			cmd == VOICE_CMD_STATE_TX ? VOICE_STATE_TX :
			VOICE_STATE_DUPLEX;
		dle = 0;
		tx_end = 0;
		pending = VOICE_STATUS_CONNECT;
		return 0;
	case VOICE_CMD_BEEP:
	case VOICE_CMD_DTMF:
		if (!vi)
			return -1;
		tone_left = (long)vi->tone_duration * srate / 100;
		tone_dtmf = cmd == VOICE_CMD_DTMF;
		if (tone_dtmf) {
			if (dtmf.set(vi->dtmf_symbol, srate))
				tone_left = 0;
		} else {
			t1.set_freq(vi->tone1_freq, srate);
			t2.set_freq(vi->tone2_freq, srate);
			tone2 = vi->tone2_freq != 0;
		}
		if (!tone_left)
			pending = VOICE_STATUS_OK;
		return 0;
	case VOICE_CMD_ABORT:
	case VOICE_CMD_STATE_COMMAND:
		state = VOICE_STATE_COMMAND;
		tone_left = 0;
		pending = VOICE_STATUS_OK;
		return 0;
	default:
		return -1;
	}
}

int VOICE_command(void *obj, enum VOICE_CMD cmd)
{
	return static_cast<Voice*>(obj)->command(cmd);
}

int Voice::get_tx_sample(float *x)
{
	char ch;
	while (!tx_end && modem_recv_from_tty(modem, &ch, 1) == 1) {
		u8 c = ch;
		if (dle) {
			dle = 0;
			if (c == ETX) {
				tx_end = 1;
				break;
			}
			if (c == DLE) {
				*x = ((int)c - 128) * 256.0f;
				return 1;
			}
			if (c == '!' || c == 0x18) {    /* abort, <CAN> */
				tx_end = 1;
				break;
			}
			continue;
		}
		if (c == DLE) {
			dle = 1;
			continue;
		}
		*x = ((int)c - 128) * 256.0f;
		return 1;
	}
	return 0;
}

int Voice::process(void *in, void *out, int count)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	double step = 8000.0 / srate;
	char rxbuf[512];
	int nrx = 0, i, status;

	for (i = 0; i < count; i++) {
		float y = 0;

		if (tone_left > 0) {
			if (tone_dtmf)
				y = dtmf.next();
			else {
				y = DSP_TX_AMPLITUDE * t1.next();
				if (tone2)
					y = 0.5f * y + 0.5f * DSP_TX_AMPLITUDE *
						t2.next();
			}
			if (--tone_left == 0)
				pending = VOICE_STATUS_OK;
		} else if (state & VOICE_STATE_TX && !tx_end) {
			/* linear interpolation from 8000 Hz */
			tx_pos += step;
			while (tx_pos >= 1) {
				float x;
				tx_pos -= 1;
				tx_prev = tx_cur;
				if (get_tx_sample(&x))
					tx_cur = x;
				else if (tx_end) {
					DSP_DBG("voice: dle command: ETX\n");
					state &= ~VOICE_STATE_TX;
					pending = VOICE_STATUS_OK;
					break;
				}
			}
			y = tx_prev + (float)tx_pos * (tx_cur - tx_prev);
		}
		dst[i] = dsp_clip16(y);

		if (state & VOICE_STATE_RX) {
			rx_pos += step;
			if (rx_pos >= 1) {
				int c = (src[i] >> 8) + 128;
				rx_pos -= 1;
				if (nrx < (int)sizeof(rxbuf) - 2) {
					rxbuf[nrx++] = c;
					if (c == DLE)
						rxbuf[nrx++] = DLE;
				}
			}
		}
	}
	if (nrx)
		modem_send_to_tty(modem, rxbuf, nrx);

	/* any character from the DTE ends receive only mode */
	if (state == VOICE_STATE_RX) {
		char ch;
		if (modem_recv_from_tty(modem, &ch, 1) == 1) {
			static const char end[2] = { DLE, ETX };
			modem_send_to_tty(modem, end, 2);
			state = VOICE_STATE_COMMAND;
			pending = VOICE_STATUS_OK;
		}
	}

	status = pending;
	pending = 0;
	return status;
}

int VOICE_process(void *obj, void *in, void *out, int count)
{
	return static_cast<Voice*>(obj)->process(in, out, count);
}

