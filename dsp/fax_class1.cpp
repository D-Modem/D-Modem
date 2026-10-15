/*
 *
 *    fax_class1.cpp - T.31 service class 1 for the modem core:
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

#include <new>

#include "bits.h"
#include "fax.h"
#include <dsplibs.h>

using namespace dsp;


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

struct Fax {
	struct modem *modem;
	int srate;
	int caller;
	enum c1_state state;
	long count;
	long duration;

	int status[4];
	int nstatus;

	/* transmitter */
	FaxTx tx;
	int tx_on;

	/* bytes from the DTE */
	int dle;
	int eod;                /* <DLE><ETX> seen */
	u8 buf[TX_BUF];         /* FTM data ring */
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
	FaxRx rx;
	int rx_on;
	enum fax_mod rx_mod;
	FaxRxMemory rx_mem;    /* for V.17 short trains */
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

	void report_status(int status_);
	void out_flush();
	void out_byte(u8 c);
	void out_end();
	void stop_tx();
	void stop_rx();
	void hdlc_frame_end();
	void read_dte();
	void start_tx(enum fax_mod mod, int short_train, fax_get_bit_t get_bit);
	int start_rx(enum fax_mod mod, int short_train, fax_put_bit_t put_bit);
	void set_state(enum c1_state s);
	void begin_fth();
	void begin_frh();
	float ced_sample();
};


/*
 *    helpers
 */

/* queue a FAX_STATUS_* result for FAX_process to return (4 at most) */
void Fax::report_status(int status_)
{
	if (nstatus < 4)
		status[nstatus++] = status_;
}

/* the data gathered for the DTE out to the tty */
void Fax::out_flush()
{
	if (nout) {
		modem_send_to_tty(modem, out, nout);
		nout = 0;
	}
}

/* a byte to the DTE, a <DLE> doubled */
void Fax::out_byte(u8 c)
{
	if (nout >= RX_OUT)
		out_flush();
	if (c == DLE)
		out[nout++] = DLE;
	out[nout++] = c;
}

/* <DLE><ETX>, the end of the data, and everything out */
void Fax::out_end()
{
	out[nout++] = DLE;
	out[nout++] = ETX;
	out_flush();
}

/* the transmitter off at once */
void Fax::stop_tx()
{
	tx_on = 0;
}

/* the receiver off, its V.17 equalizer kept for a later short train */
void Fax::stop_rx()
{
	if (rx_on) {
		rx.save(&rx_mem);
		rx.release();
	}
	rx_on = 0;
	rx_trained = 0;
}


/*
 *    HDLC transmitter
 */

/* FaxTx callback for +FTH: the flags (1 s of them first), the DTE's
   frames with their FCS and the stuffed zeros, at least 8 flag bits
   between them; after the final frame (P/F bit) closing flags, then
   FAX_BIT_END.  A frame going out lets the DTE send the next one */
static int hdlc_get_bit(void *ctx)
{
	Fax *f = static_cast<Fax*>(ctx);
	int bit;

	/* a stuffed zero, also after the FCS's last bit */
	if (f->hones == 5) {
		f->hones = 0;
		return 0;
	}
	if (f->hsending) {
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
			unsigned crc = hdlc_fcs(f->hframe, f->hlen) ^ 0xffff;
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
				f->report_status(FAX_STATUS_CONNECT);
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

/* a flag ended a frame: into the queue if it has whole octets (the
   flag's 0111111 went into rbits too), at least 4, with its FCS
   checked */
void Fax::hdlc_frame_end()
{
	int n = rnbits - 7, k;
	struct rx_frame *fr;

	rnbits = 0;
	if (n < 32 || n % 8 || n / 8 > HDLC_MAX)
		return;
	if ((fhead + 1) % FRAME_QUEUE == ftail)
		return;                         /* queue full */
	fr = &frames[fhead];
	fr->len = n / 8;
	for (k = 0; k < fr->len; k++) {
		int b, v = 0;
		for (b = 0; b < 8; b++)
			v |= rbits[k * 8 + b] << b;
		fr->data[k] = v;
	}
	fr->ok = hdlc_fcs(fr->data, fr->len) == HDLC_FCS_GOOD;
	DSP_DBG("fax: hdlc frame %d bytes, %s\n", fr->len, fr->ok ? "ok" : "bad fcs");
	fhead = (fhead + 1) % FRAME_QUEUE;
}

/* FaxRx callback for +FRH: flags, stuffed zeros and aborts (7 ones) */
static void hdlc_put_bit(void *ctx, int bit)
{
	Fax *f = static_cast<Fax*>(ctx);

	if (bit) {
		if (++f->rones > 6) {           /* idle or abort */
			f->rnbits = 0;
			return;
		}
	} else {
		if (f->rones == 6) {            /* flag */
			f->rflags++;
			f->hdlc_frame_end();
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

/* FaxTx callback for +FTM: the DTE's bytes, the lowest bit first; zeros
   when it falls behind, FAX_BIT_END once <DLE><ETX> came */
static int ftm_get_bit(void *ctx)
{
	Fax *f = static_cast<Fax*>(ctx);
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

/* FaxRx callback for +FRM: bytes to the DTE, the first bit lowest */
static void frm_put_bit(void *ctx, int bit)
{
	Fax *f = static_cast<Fax*>(ctx);

	/* the receiver passes on bits once it is trained; the CONNECT is
	   reported by the time they reach the DTE */
	if (f->state != C1_FRM)
		return;
	f->rbyte |= (bit & 1) << f->rbitn;
	if (++f->rbitn == 8) {
		f->out_byte(f->rbyte);
		f->rbyte = 0;
		f->rbitn = 0;
	}
}

/* DTE data for FTM / FTH, with the <DLE> codes taken out, a character at
   a time: nothing after <DLE><ETX> is taken, so the next frame waits
   until this one goes out, and what follows the last one (or FTM's data)
   is left for the commands.  FTH: an empty frame (<DLE><ETX> alone) ends
   the transmission with OK (8.3.5/T.31); a frame longer than HDLC_MAX - 2
   octets loses the rest */
void Fax::read_dte()
{
	char ch;

	if (eod)
		return;
	for (;;) {
		u8 c;
		if (state == C1_FTM ?
		    (head + 1) % TX_BUF == tail : hready || hclosing)
			return;
		if (modem_recv_from_tty(modem, &ch, 1) != 1)
			return;
		c = ch;
		if (dle) {
			dle = 0;
			if (c == ETX) {
				if (state == C1_FTM)
					eod = 1;
				else if (hlen)
					hready = 1;
				else
					hclosing = 1;
				return;
			}
			if (c != DLE)
				continue;
		} else if (c == DLE) {
			dle = 1;
			continue;
		}
		if (state == C1_FTM) {
			buf[head] = c;
			head = (head + 1) % TX_BUF;
		} else if (hlen < HDLC_MAX - 2)
			hframe[hlen++] = c;
	}
}


/*
 *    commands
 */

/* transmit mod (the receiver stops); get_bit gives the bits */
void Fax::start_tx(enum fax_mod mod, int short_train, fax_get_bit_t get_bit)
{
	stop_rx();
	tx.init(mod, short_train, srate, get_bit, this);
	tx_on = 1;
	eod = 0;
	dle = 0;
}

/* receive mod, the bits to put_bit; a V.17 short train starts from the
   last long one.  -1 when out of memory */
int Fax::start_rx(enum fax_mod mod, int short_train, fax_put_bit_t put_bit)
{
	stop_rx();
	if (rx.init(mod, short_train, srate, put_bit, this,
			&rx_mem))
		return -1;
	rx_on = 1;
	rx_mod = mod;
	rx_trained = 0;
	return 0;
}

/* to state s, counting its samples from 0 */
void Fax::set_state(enum c1_state s)
{
	DSP_DBG("fax: state %d -> %d\n", state, s);
	state = s;
	count = 0;
}

/* +FTH=3: HDLC on V.21 channel 2, CONNECT at once for the first
   frame */
void Fax::begin_fth()
{
	stop_tx();
	set_state(C1_FTH);
	hlen = hready = hsending = hclosing = 0;
	hflags = 0;
	hflag_bit = 0;
	hfirst = 1;
	start_tx(FM_V21, 0, hdlc_get_bit);
	report_status(FAX_STATUS_CONNECT);
}

/* +FRH=3: HDLC from V.21 channel 2; CONNECT once flags come with a
   carrier */
void Fax::begin_frh()
{
	stop_tx();
	set_state(C1_FRH);
	frh_connected = 0;
	/* keep a running V.21 receiver (frames may already be queued) */
	if (!rx_on || rx_mod != FM_V21) {
		start_rx(FM_V21, 0, hdlc_put_bit);
		rnbits = rones = rflags = 0;
	}
}

/* a class 1 command (FAX_CMD_*); param: the T.31 <mod>, or the time in
   10 ms for +FTS / +FRS.  1 when it started (the result codes come from
   FAX_process), -1 for ERROR */
int FAX_class1_command(void *obj, unsigned cmd, unsigned param)
{
	Fax *f = static_cast<Fax*>(obj);
	enum fax_mod mod;
	int short_train;

	if (!f)
		return -1;
	DSP_DBG("fax: command %u, %u\n", cmd, param);
	switch (cmd) {
	case FAX_CMD_FTS:
		f->stop_tx();
		f->set_state(C1_FTS);
		f->duration = (long)param * f->srate / 100;
		return 1;
	case FAX_CMD_FRS:
		f->stop_tx();
		f->set_state(C1_FRS);
		f->duration = (long)param * f->srate / 100;
		f->rx_quiet = 0;
		return 1;
	case FAX_CMD_FTH:
		if (param != 3)
			return -1;
		f->begin_fth();
		return 1;
	case FAX_CMD_FRH:
		if (param != 3)
			return -1;
		f->begin_frh();
		return 1;
	case FAX_CMD_FTM:
		if (!fax_mod_from_code(param, &mod, &short_train) || mod == FM_V21)
			return -1;
		f->stop_tx();
		f->set_state(C1_FTM);
		f->head = f->tail = f->nbits = 0;
		f->start_tx(mod, short_train, ftm_get_bit);
		f->report_status(FAX_STATUS_CONNECT);
		return 1;
	case FAX_CMD_FRM:
		if (!fax_mod_from_code(param, &mod, &short_train) || mod == FM_V21)
			return -1;
		f->stop_tx();
		f->set_state(C1_FRM);
		f->rbyte = f->rbitn = 0;
		if (f->start_rx(mod, short_train, frm_put_bit))
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

/* class 1 on modem m at srate samples/s once the call is up: the caller
   receives the DIS, the answerer sends CED first.  NULL when out of
   memory */
void *FAX_create(struct modem *m, unsigned caller, unsigned srate)
{
	Fax *f = new (std::nothrow) Fax();
	if (!f)
		return NULL;
	f->modem = m;
	f->srate = srate;
	f->caller = caller;
	DSP_DBG("fax: create, %s\n", caller ? "caller" : "answerer");
	if (caller)
		f->begin_frh();           /* the called fax sends DIS */
	else {
		f->set_state(C1_CED);
		f->state = C1_CED;
	}
	return f;
}

/* free what FAX_create made */
void FAX_delete(void *obj)
{
	Fax *f = static_cast<Fax*>(obj);
	if (!f)
		return;
	DSP_DBG("fax: delete\n");
	f->stop_rx();
	delete f;
}

/* the answer tone (CED), by the samples since the answer */
float Fax::ced_sample()
{
	/* 200 ms silence, 3 s of 2100 Hz, 75 ms silence */
	long t = count;
	if (t < ms2samples(200, srate) || t >= ms2samples(3200, srate))
		return 0;
	return FAX_CED_RMS * 1.41421f * sinf(2 * (float)M_PI * 2100 * t / srate);
}

/* count samples of 16 bit audio in and out: the transmitter, the
   receiver and the state of the command, the data to and from the DTE.
   The next result (FAX_STATUS_*), or 0 */
int FAX_process(void *obj, void *in, void *out, int count)
{
	Fax *f = static_cast<Fax*>(obj);
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int i, status = 0;

	if (f->state == C1_FTH || f->state == C1_FTM)
		f->read_dte();

	for (i = 0; i < count; i++) {
		float y = 0, x = src[i];

		f->in_power += 0.002f * (x * x - f->in_power);
		if (f->state == C1_CED)
			y = f->ced_sample();
		else if (f->tx_on) {
			y = f->tx.sample();
			if (f->tx.done) {
				f->tx_on = 0;
				if (f->state == C1_FTH || f->state == C1_FTM) {
					f->set_state(C1_IDLE);
					f->report_status(FAX_STATUS_OK);
				}
			}
		}
		dst[i] = dsp_clip16(y);

		if (f->rx_on && !f->tx_on)
			f->rx.sample(x);
		f->count++;

		switch (f->state) {
		case C1_CED:
			if (f->count >= ms2samples(3275, f->srate))
				f->begin_fth();
			break;
		case C1_FTS:
			if (f->count >= f->duration) {
				f->set_state(C1_IDLE);
				f->report_status(FAX_STATUS_OK);
			}
			break;
		case C1_FRS:
			if (f->in_power < tone_power(150))
				f->rx_quiet++;
			else
				f->rx_quiet = 0;
			if (f->rx_quiet >= f->duration) {
				f->set_state(C1_IDLE);
				f->report_status(FAX_STATUS_OK);
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
					f->report_status(FAX_STATUS_CONNECT);
					break;  /* data after the CONNECT */
				}
				for (k = 0; k < fr->len; k++)
					f->out_byte(fr->data[k]);
				f->out_end();
				f->ftail = (f->ftail + 1) % FRAME_QUEUE;
				f->set_state(C1_IDLE);
				f->report_status(fr->ok ? FAX_STATUS_OK : FAX_STATUS_FRAME_ERROR);
			} else if (!f->frh_connected && f->rflags >= 2 &&
				   f->rx.carrier) {
				f->frh_connected = 1;
				f->report_status(FAX_STATUS_CONNECT);
			} else if (f->frh_connected && f->rx.carrier_run <
				   -ms2samples(200, f->srate)) {
				/* the carrier went away before a frame */
				f->out_end();
				f->set_state(C1_IDLE);
				f->report_status(FAX_STATUS_NOCARRIER);
			}
			break;
		case C1_FRM:
			if (!f->rx_trained && f->rx.trained) {
				f->rx_trained = 1;
				f->report_status(FAX_STATUS_CONNECT);
			} else if (f->rx_trained && !f->rx.carrier) {
				f->out_end();
				f->stop_rx();
				f->set_state(C1_IDLE);
				f->report_status(FAX_STATUS_NOCARRIER);
			}
			break;
		default:
			break;
		}
	}
	/* data received after a CONNECT follows it on the tty */
	if (!f->nstatus || f->status[0] != FAX_STATUS_CONNECT)
		f->out_flush();

	/* the DTE stops FRH / FRM with any character */
	if ((f->state == C1_FRM && f->rx_trained) ||
	    (f->state == C1_FRH && f->frh_connected)) {
		char c;
		if (modem_recv_from_tty(f->modem, &c, 1) == 1) {
			DSP_DBG("fax: receive aborted by the DTE\n");
			if (f->state == C1_FRM)
				f->stop_rx();
			f->set_state(C1_IDLE);
			f->report_status(FAX_STATUS_OK);
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
