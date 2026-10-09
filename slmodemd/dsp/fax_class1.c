/*
 *
 *    fax_class1.c - T.31 service class 1 for the modem core:
 *                   FAX_create/FAX_process/FAX_delete and the
 *                   +FTH/+FRH/+FTM/+FRM/+FTS/+FRS commands
 *
 *    After dialing the modem behaves as if AT+FRH=3 was given, after
 *    answering it sends CED and then behaves as if AT+FTH=3 was given.
 *    Data from and to the DTE is <DLE> shielded and ends with
 *    <DLE><ETX>.  HDLC frames are passed with their two FCS octets.
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fax.h"

extern int modem_send_to_tty(struct modem *m, const char *buf, int n);
extern int modem_recv_from_tty(struct modem *m, char *buf, int n);

#define DLE 0x10
#define ETX 0x03

#define HDLC_MAX 260            /* address, control, FIF and FCS */
#define FRAME_QUEUE 8
#define TX_BUF 16384            /* FTM data from the DTE */
#define RX_OUT 1024

enum c1_state {
	C1_IDLE,                /* command mode, line quiet or receiving */
	C1_CED,
	C1_FTH,
	C1_FRH,
	C1_FTM,
	C1_FRM,
	C1_FTS,
	C1_FRS,
};

struct rx_frame {
	u8 data[HDLC_MAX];
	int len;
	int ok;
};

struct fax {
	struct modem *modem;
	int srate;
	int caller;
	enum c1_state state;
	long count;
	long duration;

	int status[4];
	int nstatus;

	/* transmitter */
	struct fax_tx tx;
	int tx_on;

	/* bytes from the DTE */
	int dle;
	int eod;                /* <DLE><ETX> seen */
	u8 *buf;                /* FTM data ring */
	int head, tail;
	int byte, nbits;

	/* HDLC transmitter (FTH) */
	u8 hframe[HDLC_MAX];
	int hlen;               /* bytes of the frame being built */
	int hready;             /* frame complete, waiting to be sent */
	u8 hsend[HDLC_MAX];
	int hsend_len, hsend_pos, hsend_bit;
	int hsending;
	int hones;
	int hflag_bit;
	long hflags;            /* flag bits sent since the last frame */
	int hfinal;             /* the frame being sent is the final one */
	int hclosing;           /* final frame sent: closing flags */
	int hfirst;

	/* receiver */
	struct fax_rx rx;
	int rx_on;
	enum fax_mod rx_mod;
	struct fax_rx_memory rx_mem;    /* for V.17 short trains */
	int rx_trained;
	int rx_carrier;
	long rx_quiet;          /* samples of silence (FRS) */
	float in_power;

	/* HDLC receiver */
	u8 rbits[HDLC_MAX * 10];
	int rnbits;
	int rones;
	int rflags;             /* flags seen with the carrier on */
	struct rx_frame frames[FRAME_QUEUE];
	int fhead, ftail;
	int frh_connected;

	/* FRM byte assembly and output to the DTE */
	int rbyte, rbitn;
	char out[RX_OUT + 8];
	int nout;
};


/*
 *    helpers
 */

static void fax_status(struct fax *f, int status)
{
	if (f->nstatus < 4)
		f->status[f->nstatus++] = status;
}

static void out_flush(struct fax *f)
{
	if (f->nout) {
		modem_send_to_tty(f->modem, f->out, f->nout);
		f->nout = 0;
	}
}

static void out_byte(struct fax *f, u8 c)
{
	if (f->nout >= RX_OUT)
		out_flush(f);
	if (c == DLE)
		f->out[f->nout++] = DLE;
	f->out[f->nout++] = c;
}

static void out_end(struct fax *f)
{
	f->out[f->nout++] = DLE;
	f->out[f->nout++] = ETX;
	out_flush(f);
}

/* CRC-16 CCITT as used by HDLC (reflected, init 0xffff) */
static unsigned crc16(const u8 *p, int n)
{
	unsigned crc = 0xffff;
	int k;
	while (n-- > 0) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

static void stop_tx(struct fax *f)
{
	f->tx_on = 0;
}

static void stop_rx(struct fax *f)
{
	if (f->rx_on) {
		fax_rx_save(&f->rx, &f->rx_mem);
		fax_rx_free(&f->rx);
	}
	f->rx_on = 0;
	f->rx_trained = 0;
}


/*
 *    HDLC transmitter
 */

static int hdlc_get_bit(void *ctx)
{
	struct fax *f = ctx;
	int bit;

	if (f->hsending) {
		if (f->hones == 5) {            /* stuffed zero */
			f->hones = 0;
			return 0;
		}
		bit = (f->hsend[f->hsend_pos] >> f->hsend_bit) & 1;
		if (++f->hsend_bit == 8) {
			f->hsend_bit = 0;
			if (++f->hsend_pos == f->hsend_len) {
				f->hsending = 0;
				f->hflags = 0;
				f->hflag_bit = 0;
			}
		}
		f->hones = bit ? f->hones + 1 : 0;
		return bit;
	}

	/* at a flag boundary: start a frame, finish, or send another flag */
	if (f->hflag_bit == 0) {
		long min = f->hfirst ? 300 : 8;         /* 1 s preamble first */
		if (f->hclosing && f->hflags >= 16)
			return FAX_BIT_END;
		if (!f->hclosing && f->hready && f->hflags >= min) {
			unsigned crc = crc16(f->hframe, f->hlen) ^ 0xffff;
			memcpy(f->hsend, f->hframe, f->hlen);
			f->hsend[f->hlen] = crc & 0xff;
			f->hsend[f->hlen + 1] = crc >> 8;
			f->hsend_len = f->hlen + 2;
			f->hsend_pos = f->hsend_bit = 0;
			f->hones = 0;
			f->hsending = 1;
			f->hfinal = f->hlen >= 2 && (f->hframe[1] & 0x10);
			f->hready = 0;
			f->hlen = 0;
			f->hfirst = 0;
			/* the DTE may send the next frame now */
			if (!f->hfinal)
				fax_status(f, FAX_STATUS_CONNECT);
			else
				f->hclosing = 1;
			return hdlc_get_bit(f);
		}
	}
	bit = (0x7e >> f->hflag_bit) & 1;
	f->hflag_bit = (f->hflag_bit + 1) & 7;
	f->hflags++;
	return bit;
}


/*
 *    HDLC receiver
 */

static void hdlc_frame_end(struct fax *f)
{
	int n = f->rnbits - 7, k;
	struct rx_frame *fr;

	f->rnbits = 0;
	if (n < 32 || n % 8 || n / 8 > HDLC_MAX)
		return;
	if ((f->fhead + 1) % FRAME_QUEUE == f->ftail)
		return;                         /* queue full */
	fr = &f->frames[f->fhead];
	fr->len = n / 8;
	for (k = 0; k < fr->len; k++) {
		int b, v = 0;
		for (b = 0; b < 8; b++)
			v |= f->rbits[k * 8 + b] << b;
		fr->data[k] = v;
	}
	fr->ok = crc16(fr->data, fr->len) == 0xf0b8;
	DSP_DBG("fax: hdlc frame %d bytes, %s\n", fr->len, fr->ok ? "ok" : "bad fcs");
	f->fhead = (f->fhead + 1) % FRAME_QUEUE;
}

static void hdlc_put_bit(void *ctx, int bit)
{
	struct fax *f = ctx;

	if (bit) {
		if (++f->rones > 6) {           /* idle or abort */
			f->rnbits = 0;
			return;
		}
	} else {
		if (f->rones == 6) {            /* flag */
			f->rflags++;
			hdlc_frame_end(f);
			f->rones = 0;
			return;
		}
		if (f->rones == 5) {            /* stuffed zero */
			f->rones = 0;
			return;
		}
		f->rones = 0;
	}
	if (f->rnbits < (int)sizeof(f->rbits))
		f->rbits[f->rnbits++] = bit;
}


/*
 *    FTM / FRM data
 */

static int ftm_get_bit(void *ctx)
{
	struct fax *f = ctx;
	int bit;

	if (!f->nbits) {
		if (f->head == f->tail) {
			if (f->eod)
				return FAX_BIT_END;
			return 0;               /* underrun: fill */
		}
		f->byte = f->buf[f->tail];
		f->tail = (f->tail + 1) % TX_BUF;
		f->nbits = 8;
	}
	bit = f->byte & 1;
	f->byte >>= 1;
	f->nbits--;
	return bit;
}

static void frm_put_bit(void *ctx, int bit)
{
	struct fax *f = ctx;

	/* the receiver passes on bits once it is trained; the CONNECT is
	   reported by the time they reach the DTE */
	if (f->state != C1_FRM)
		return;
	f->rbyte |= (bit & 1) << f->rbitn;
	if (++f->rbitn == 8) {
		out_byte(f, f->rbyte);
		f->rbyte = 0;
		f->rbitn = 0;
	}
}

/* DTE data for FTM / FTH, with the <DLE> codes taken out */
static void read_dte(struct fax *f)
{
	char tmp[512];
	int n, i, room;

	if (f->eod)
		return;
	if (f->state == C1_FTM)
		room = (f->tail - f->head - 1 + TX_BUF) % TX_BUF;
	else
		room = f->hready ? 0 : HDLC_MAX - 2 - f->hlen;
	if (room <= 0)
		return;
	if (room > (int)sizeof(tmp))
		room = sizeof(tmp);
	n = modem_recv_from_tty(f->modem, tmp, room);
	for (i = 0; i < n; i++) {
		u8 c = tmp[i];
		if (f->dle) {
			f->dle = 0;
			if (c == ETX) {
				if (f->state == C1_FTH) {
					/* one frame complete */
					if (f->hlen >= 2)
						f->hready = 1;
					continue;
				}
				f->eod = 1;
				break;
			}
			if (c != DLE)
				continue;
		} else if (c == DLE) {
			f->dle = 1;
			continue;
		}
		if (f->state == C1_FTM) {
			f->buf[f->head] = c;
			f->head = (f->head + 1) % TX_BUF;
		} else if (f->hlen < HDLC_MAX - 2)
			f->hframe[f->hlen++] = c;
	}
}


/*
 *    commands
 */

static void start_tx(struct fax *f, enum fax_mod mod, int short_train,
		     fax_get_bit_t get_bit)
{
	stop_rx(f);
	fax_tx_init(&f->tx, mod, short_train, f->srate, get_bit, f);
	f->tx_on = 1;
	f->eod = 0;
	f->dle = 0;
}

static int start_rx(struct fax *f, enum fax_mod mod, int short_train,
		    fax_put_bit_t put_bit)
{
	stop_rx(f);
	if (fax_rx_init(&f->rx, mod, short_train, f->srate, put_bit, f,
			&f->rx_mem))
		return -1;
	f->rx_on = 1;
	f->rx_mod = mod;
	f->rx_trained = 0;
	return 0;
}

static void set_state(struct fax *f, enum c1_state s)
{
	DSP_DBG("fax: state %d -> %d\n", f->state, s);
	f->state = s;
	f->count = 0;
}

static void begin_fth(struct fax *f)
{
	stop_tx(f);
	set_state(f, C1_FTH);
	f->hlen = f->hready = f->hsending = f->hclosing = 0;
	f->hflags = 0;
	f->hflag_bit = 0;
	f->hfirst = 1;
	start_tx(f, FM_V21, 0, hdlc_get_bit);
	fax_status(f, FAX_STATUS_CONNECT);
}

static void begin_frh(struct fax *f)
{
	stop_tx(f);
	set_state(f, C1_FRH);
	f->frh_connected = 0;
	/* keep a running V.21 receiver (frames may already be queued) */
	if (!f->rx_on || f->rx_mod != FM_V21) {
		start_rx(f, FM_V21, 0, hdlc_put_bit);
		f->rnbits = f->rones = f->rflags = 0;
	}
}

int FAX_class1_command(void *obj, unsigned cmd, unsigned param)
{
	struct fax *f = obj;
	enum fax_mod mod;
	int short_train;

	if (!f)
		return -1;
	DSP_DBG("fax: command %u, %u\n", cmd, param);
	switch (cmd) {
	case FAX_CMD_FTS:
		stop_tx(f);
		set_state(f, C1_FTS);
		f->duration = (long)param * f->srate / 100;
		return 1;
	case FAX_CMD_FRS:
		stop_tx(f);
		set_state(f, C1_FRS);
		f->duration = (long)param * f->srate / 100;
		f->rx_quiet = 0;
		return 1;
	case FAX_CMD_FTH:
		if (param != 3)
			return -1;
		begin_fth(f);
		return 1;
	case FAX_CMD_FRH:
		if (param != 3)
			return -1;
		begin_frh(f);
		return 1;
	case FAX_CMD_FTM:
		if (!fax_mod_from_code(param, &mod, &short_train) || mod == FM_V21)
			return -1;
		stop_tx(f);
		set_state(f, C1_FTM);
		f->head = f->tail = f->nbits = 0;
		start_tx(f, mod, short_train, ftm_get_bit);
		fax_status(f, FAX_STATUS_CONNECT);
		return 1;
	case FAX_CMD_FRM:
		if (!fax_mod_from_code(param, &mod, &short_train) || mod == FM_V21)
			return -1;
		stop_tx(f);
		set_state(f, C1_FRM);
		f->rbyte = f->rbitn = 0;
		if (start_rx(f, mod, short_train, frm_put_bit))
			return -1;
		return 1;
	default:
		DSP_DBG("fax: bad command: %x\n", cmd);
		return -1;
	}
}


/*
 *    object
 */

void *FAX_create(struct modem *m, unsigned caller, unsigned srate)
{
	struct fax *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	f->buf = malloc(TX_BUF);
	if (!f->buf) {
		free(f);
		return NULL;
	}
	f->modem = m;
	f->srate = srate;
	f->caller = caller;
	DSP_DBG("fax: create, %s\n", caller ? "caller" : "answerer");
	if (caller)
		begin_frh(f);           /* the called fax sends DIS */
	else {
		set_state(f, C1_CED);
		f->state = C1_CED;
	}
	return f;
}

void FAX_delete(void *obj)
{
	struct fax *f = obj;
	if (!f)
		return;
	DSP_DBG("fax: delete\n");
	stop_rx(f);
	free(f->buf);
	free(f);
}

static float ced_sample(struct fax *f)
{
	/* 200 ms silence, 3 s of 2100 Hz, 75 ms silence */
	long t = f->count;
	if (t < ms2samples(200, f->srate) || t >= ms2samples(3200, f->srate))
		return 0;
	return FAX_CED_RMS * 1.41421f * sinf(2 * (float)M_PI * 2100 * t / f->srate);
}

int FAX_process(void *obj, void *in, void *out, int count)
{
	struct fax *f = obj;
	s16 *src = in, *dst = out;
	int i, status = 0;

	if (f->state == C1_FTH || f->state == C1_FTM)
		read_dte(f);

	for (i = 0; i < count; i++) {
		float y = 0, x = src[i];

		f->in_power += 0.002f * (x * x - f->in_power);
		if (f->state == C1_CED)
			y = ced_sample(f);
		else if (f->tx_on) {
			y = fax_tx_sample(&f->tx);
			if (f->tx.done) {
				f->tx_on = 0;
				if (f->state == C1_FTH || f->state == C1_FTM) {
					set_state(f, C1_IDLE);
					fax_status(f, FAX_STATUS_OK);
				}
			}
		}
		dst[i] = dsp_clip16(y);

		if (f->rx_on && !f->tx_on)
			fax_rx_sample(&f->rx, x);
		f->count++;

		switch (f->state) {
		case C1_CED:
			if (f->count >= ms2samples(3275, f->srate))
				begin_fth(f);
			break;
		case C1_FTS:
			if (f->count >= f->duration) {
				set_state(f, C1_IDLE);
				fax_status(f, FAX_STATUS_OK);
			}
			break;
		case C1_FRS:
			if (f->in_power < tone_power(150))
				f->rx_quiet++;
			else
				f->rx_quiet = 0;
			if (f->rx_quiet >= f->duration) {
				set_state(f, C1_IDLE);
				fax_status(f, FAX_STATUS_OK);
			}
			break;
		case C1_FRH:
			if (f->nstatus)
				break;  /* the CONNECT goes out first */
			if (f->ftail != f->fhead) {
				struct rx_frame *fr = &f->frames[f->ftail];
				int k;
				if (!f->frh_connected) {
					f->frh_connected = 1;
					fax_status(f, FAX_STATUS_CONNECT);
					break;  /* data after the CONNECT */
				}
				for (k = 0; k < fr->len; k++)
					out_byte(f, fr->data[k]);
				out_end(f);
				f->ftail = (f->ftail + 1) % FRAME_QUEUE;
				set_state(f, C1_IDLE);
				fax_status(f, fr->ok ? FAX_STATUS_OK : FAX_STATUS_FRAME_ERROR);
			} else if (!f->frh_connected && f->rflags >= 2 &&
				   f->rx.carrier) {
				f->frh_connected = 1;
				fax_status(f, FAX_STATUS_CONNECT);
			} else if (f->frh_connected && f->rx.carrier_run <
				   -ms2samples(200, f->srate)) {
				/* the carrier went away before a frame */
				out_end(f);
				set_state(f, C1_IDLE);
				fax_status(f, FAX_STATUS_NOCARRIER);
			}
			break;
		case C1_FRM:
			if (!f->rx_trained && f->rx.trained) {
				f->rx_trained = 1;
				fax_status(f, FAX_STATUS_CONNECT);
			} else if (f->rx_trained && !f->rx.carrier) {
				out_end(f);
				stop_rx(f);
				set_state(f, C1_IDLE);
				fax_status(f, FAX_STATUS_NOCARRIER);
			}
			break;
		default:
			break;
		}
	}
	/* data received after a CONNECT follows it on the tty */
	if (!f->nstatus || f->status[0] != FAX_STATUS_CONNECT)
		out_flush(f);

	/* the DTE stops FRH / FRM with any character */
	if ((f->state == C1_FRM && f->rx_trained) ||
	    (f->state == C1_FRH && f->frh_connected)) {
		char c;
		if (modem_recv_from_tty(f->modem, &c, 1) == 1) {
			DSP_DBG("fax: receive aborted by the DTE\n");
			if (f->state == C1_FRM)
				stop_rx(f);
			set_state(f, C1_IDLE);
			fax_status(f, FAX_STATUS_OK);
		}
	}

	if (f->nstatus) {
		status = f->status[0];
		memmove(f->status, f->status + 1, (f->nstatus - 1) * sizeof(int));
		f->nstatus--;
		DSP_DBG("fax: status %d\n", status);
	}
	return status;
}
