/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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
 *    The line is the call's audio, or T.38 (FAX_t38_start(), also in the
 *    middle of a call): the same commands and results, the signals as
 *    IFP packets (t38.h) in their time, which the samples FAX_process is
 *    given still tell.  The fast modulations' data is T.4 data, or the
 *    DTE's own HDLC (error correction mode: fax software frames the ECM
 *    blocks itself), which goes as HDLC frames: taken out of the DTE's
 *    bits, and put into the bits it is given.
 *
 */

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include "bits.h"
#include "env.h"
#include "fax.h"
#include "fax_hdlc.h"
#include "t38.h"
#include <dsplibs.h>

using namespace dsp;


#define DLE 0x10
#define ETX 0x03

#define HDLC_MAX 260            /* address, control, FIF and FCS */
#define ECM_MAX 262             /* an ECM frame (4 + 256 octets) and FCS */
#define FRAME_QUEUE 8
#define TX_BUF 16384            /* FTM data from the DTE */
#define RX_OUT 4096

/* T.38: data octets in a packet (V.21: 7, T.38 7.5), the tails after
   the data (HDLC, T.4) before no-signal, the ECM frames waiting to go */
#define T38_V21_MAX 7
#define T38_DATA_MAX 300
#define T38_HDLC_TAIL_MS 100
#define T38_T4_TAIL_MS 60
#define ECM_QUEUE 8

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

/* T.38: what is going out, and where it is */
enum t38_kind { TK_NONE, TK_V21, TK_FAST };
enum t38_step { TS_TRAIN, TS_DATA, TS_TAIL };
/* FTM's data: not known yet, T.4 data, the DTE's HDLC */
enum t38_fmode { FMODE_UNKNOWN, FMODE_T4, FMODE_HDLC };

struct rx_frame {
	u8 data[HDLC_MAX];
	int len;
	int ok;
};

struct ecm_frame {
	u8 data[ECM_MAX];
	int len;
	int ok;
};

struct Fax : HdlcTx<Fax, HDLC_MAX - 2, HdlcV21>,
	     HdlcRx<Fax, HDLC_MAX, HdlcV21>,
	     HdlcRx<Fax, ECM_MAX, HdlcEcm> {
	/* the receivers: V.21's frames, and (T.38) the DTE's ECM frames */
	using V21Rx = HdlcRx<Fax, HDLC_MAX, HdlcV21>;
	using EcmRx = HdlcRx<Fax, ECM_MAX, HdlcEcm>;

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
	enum fax_mod tx_mod;            /* FTM's */
	int tx_short;

	/* bytes from the DTE */
	int dle;
	int eod;                /* <DLE><ETX> seen */
	u8 buf[TX_BUF];         /* FTM data ring */
	int head, tail;
	int byte, nbits;

	/* the next frame for the HDLC transmitter (FTH) */
	u8 hframe[HDLC_MAX - 2];
	int hlen;               /* bytes of the frame being built */
	int hready;             /* frame complete, waiting to be sent */

	/* receiver */
	FaxRx rx;
	int rx_on;
	enum fax_mod rx_mod;
	FaxRxMemory rx_mem;    /* for V.17 short trains */
	int rx_trained;
	int rx_carrier;
	long rx_quiet;          /* samples of silence (FRS) */
	float in_power;

	/* the caller's CNG (6.6/T.31: 4.2/T.30, 0.5 s of 1100 Hz every 3.5
	   s) in its first +FRH, until the answerer's flags come: samples
	   into it, or -1 when off */
	long cng_n;

	/* frames from the HDLC receiver */
	int rflags;             /* flags seen with the carrier on */
	struct rx_frame frames[FRAME_QUEUE];
	int fhead, ftail;
	int frh_connected;

	/* FRM byte assembly and output to the DTE */
	int rbyte, rbitn;
	char out[RX_OUT + 8];
	int nout;

	/* T.38 */
	int t38;                        /* the line is T.38 */
	void (*t38_tx)(void *ctx, const void *ifp);
	void *t38_ctx;
	int t38_max;                    /* data octets in a packet */
	int t38_last_ind;
	/* sending */
	enum t38_kind t38_kind;
	enum t38_step t38_step;
	enum fax_mod t38_mod;
	int t38_long;                   /* long training */
	long t38_wait;                  /* samples left of the step */
	long t38_acc;                   /* bit time, in 1/srate */
	int t38_due;                    /* octets the line has had time for */
	int t38_debt;                   /* octets of time owed (an FCS) */
	int t38_close;                  /* FTH: no more frames */
	int t38_sent;                   /* FTH: frames out */
	u8 t38_hbuf[HDLC_MAX];          /* FTH: the frame going out, the
					   T.38 way round */
	int t38_hlen, t38_hpos;         /* its length, -1: none */
	int t38_final;
	enum t38_fmode t38_fmode;
	struct ecm_frame ecm_q[ECM_QUEUE];      /* FTM: the DTE's frames */
	int eq_head, eq_tail;
	int eq_pos;                     /* octets of the first one out */
	/* receiving */
	int t38_rx_v21;                 /* V.21 frames are taken */
	int t38_sig;                    /* a signal is on */
	int t38_v21_sig;                /* V.21's, since its preamble */
	u8 t38_rframe[HDLC_MAX];        /* the V.21 frame coming in */
	int t38_rlen;
	int t38_rover;
	u8 t38_eframe[ECM_MAX];         /* FRM: an ECM frame coming in */
	int t38_elen;
	int t38_eover;
	int t38_hdlc_out;               /* FRM: the DTE has had flags */
	int t38_ones;                   /* FRM: ones in a row to the DTE */

	void report_status(int status_);
	void out_flush();
	void out_byte(u8 c);
	void out_end();
	void stop_tx();
	void stop_rx();
	int hdlc_tx_next(HdlcV21, u8 *frame, int max);
	void hdlc_rx_frame(HdlcV21, const u8 *d, int len, bool whole);
	void hdlc_rx_frame(HdlcEcm, const u8 *d, int len, bool whole);
	void read_dte();
	void fth_close();
	bool fth_closing() const;
	void start_tx(enum fax_mod mod, int short_train, fax_get_bit_t get_bit);
	int start_rx(enum fax_mod mod, int short_train, fax_put_bit_t put_bit);
	void set_state(enum c1_state s);
	void begin_fth();
	void begin_frh();
	void begin_ftm(enum fax_mod mod, int short_train);
	int begin_frm(enum fax_mod mod, int short_train);
	void queue_frame(const u8 *d, int len, int ok);

	/* T.38 */
	void t38_ind(int ind);
	void t38_data(int ftype, const u8 *d, int n, int end);
	void t38_tx_start(enum t38_kind kind, enum fax_mod mod, int long_train);
	void t38_tx_stop();
	void t38_tail(int ms);
	void t38_tx_sample();
	void t38_flush();
	void t38_flush_v21();
	void t38_flush_fast();
	void t38_send_ecm();
	void t38_rx(const struct t38_ifp *p);
	void t38_rx_fast(const struct t38_ifp *p);
	void t38_v21_end();
	void t38_fast_end();
	void out_bit(int bit);
	void out_flag();
	void out_hdlc_frame(const u8 *d, int n, int ok);
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

/* <DLE><ETX>, the end of the data, and everything out (but after a
   CONNECT FAX_process is yet to report: T.38's data can end in the
   packet that started it) */
void Fax::out_end()
{
	out[nout++] = DLE;
	out[nout++] = ETX;
	if (!nstatus || status[0] != FAX_STATUS_CONNECT)
		out_flush();
}

/* the transmitter off at once */
void Fax::stop_tx()
{
	tx_on = 0;
	if (t38)
		t38_tx_stop();
}

/* the receiver off, its V.17 equalizer kept for a later short train
   (T.38: V.21 frames no longer taken) */
void Fax::stop_rx()
{
	if (rx_on) {
		rx.save(&rx_mem);
		rx.release();
	}
	rx_on = 0;
	rx_trained = 0;
	t38_rx_v21 = 0;
}

/* a received V.21 frame (with its FCS) into the queue, unless full */
void Fax::queue_frame(const u8 *d, int len, int ok)
{
	struct rx_frame *fr;

	if ((fhead + 1) % FRAME_QUEUE == ftail)
		return;                         /* queue full */
	fr = &frames[fhead];
	fr->len = len;
	memcpy(fr->data, d, len);
	fr->ok = ok;
	DSP_DBG("fax: hdlc frame %d bytes, %s\n", fr->len, fr->ok ? "ok" : "bad fcs");
	fhead = (fhead + 1) % FRAME_QUEUE;
}


/*
 *    HDLC transmitter
 */

/* the HDLC transmitter's next frame for +FTH, the DTE's (1 s of flags
   before the first one, at least 8 flag bits between them, closing flags
   after the final one, the P/F bit set).  A frame going out lets the DTE
   send the next one */
int Fax::hdlc_tx_next(HdlcV21, u8 *frame, int /* max: hframe's size */)
{
	int n = hlen;

	if (!hready)
		return 0;
	memcpy(frame, hframe, n);
	hready = 0;
	hlen = 0;
	/* the DTE may send the next frame now */
	if (n >= 2 && (frame[1] & 0x10))
		hdlc_tx_close();
	else
		report_status(FAX_STATUS_CONNECT);
	return n;
}

/* no more frames for +FTH (an empty frame from the DTE) */
void Fax::fth_close()
{
	if (t38)
		t38_close = 1;
	else
		hdlc_tx_close();
}

/* the frames for +FTH are over */
bool Fax::fth_closing() const
{
	return t38 ? t38_close : hdlc_tx_closing();
}


/*
 *    HDLC receiver
 */

/* a flag from the HDLC receiver (+FRH), with what came before it: into
   the queue if whole octets, at least 4, with its FCS checked */
void Fax::hdlc_rx_frame(HdlcV21, const u8 *d, int len, bool whole)
{
	rflags++;
	if (!whole || len < 4)
		return;
	queue_frame(d, len, hdlc_fcs(d, len) == HDLC_FCS_GOOD);
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
	f->out_bit(bit);
}

/* a bit to the DTE (+FRM), the first of an octet lowest */
void Fax::out_bit(int bit)
{
	rbyte |= (bit & 1) << rbitn;
	if (++rbitn == 8) {
		out_byte(rbyte);
		rbyte = 0;
		rbitn = 0;
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
		    (head + 1) % TX_BUF == tail :
		    hready || fth_closing())
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
					fth_close();
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
 *    T.38: sending
 */

/* n octets of in into out (they may be the same), each one's bits the
   other way round: T.38's octets have the first bit on the line at the
   top, the DTE's and T.30's at the bottom (t38.h's t38_reverse(), which
   is in the T.30 library) */
static void flip_octets(u8 *out, const u8 *in, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		unsigned c = in[i];
		c = (c & 0xf0) >> 4 | (c & 0x0f) << 4;
		c = (c & 0xcc) >> 2 | (c & 0x33) << 2;
		c = (c & 0xaa) >> 1 | (c & 0x55) << 1;
		out[i] = (u8)c;
	}
}

/* a fast modulation's T.38 data type, its indicator (long or short
   training) and training time */
static int mod_t38_data(enum fax_mod mod)
{
	switch (mod) {
	case FM_V27_2400:  return T38_DATA_V27_2400;
	case FM_V27_4800:  return T38_DATA_V27_4800;
	case FM_V29_7200:  return T38_DATA_V29_7200;
	case FM_V29_9600:  return T38_DATA_V29_9600;
	case FM_V17_7200:  return T38_DATA_V17_7200;
	case FM_V17_9600:  return T38_DATA_V17_9600;
	case FM_V17_12000: return T38_DATA_V17_12000;
	case FM_V17_14400: return T38_DATA_V17_14400;
	default:           return T38_DATA_V21;
	}
}

/* T.38's indicator for the training of mod (V.17: long_train or
   short) */
static int mod_t38_ind(enum fax_mod mod, int long_train)
{
	switch (mod) {
	case FM_V21:       return T38_IND_V21_PREAMBLE;
	case FM_V27_2400:  return T38_IND_V27_2400;
	case FM_V27_4800:  return T38_IND_V27_4800;
	case FM_V29_7200:  return T38_IND_V29_7200;
	case FM_V29_9600:  return T38_IND_V29_9600;
	case FM_V17_7200:  return T38_IND_V17_7200_SHORT + long_train;
	case FM_V17_9600:  return T38_IND_V17_9600_SHORT + long_train;
	case FM_V17_12000: return T38_IND_V17_12000_SHORT + long_train;
	case FM_V17_14400: return T38_IND_V17_14400_SHORT + long_train;
	default:           return T38_IND_NO_SIGNAL;
	}
}

/* the ms of mod's training (V.17: long_train or short), for T.38 */
static int mod_train_ms(enum fax_mod mod, int long_train)
{
	switch (mod) {
	case FM_V21:       return 1000;         /* the flags before the
						   first frame */
	case FM_V27_2400:  return 943;
	case FM_V27_4800:  return 708;
	case FM_V29_7200:
	case FM_V29_9600:  return 234;
	default:           return long_train ? 1393 : 142;
	}
}

/* an indicator, unless it is the last one sent */
void Fax::t38_ind(int ind)
{
	struct t38_ifp p;

	if (ind == t38_last_ind)
		return;
	t38_last_ind = ind;
	memset(&p, 0, sizeof(p));
	p.type = ind;
	if (t38_tx)
		t38_tx(t38_ctx, &p);
}

/* a data packet of the transmission's modulation: n octets as field
   type ftype (none if n is 0), then end, a field without data (-1:
   none) */
void Fax::t38_data(int ftype, const u8 *d, int n, int end)
{
	struct t38_ifp p;

	memset(&p, 0, sizeof(p));
	p.data = 1;
	p.type = t38_kind == TK_V21 ? T38_DATA_V21 : mod_t38_data(t38_mod);
	if (n > 0) {
		p.field[p.nfields].type = ftype;
		p.field[p.nfields].data = d;
		p.field[p.nfields++].len = n;
	}
	if (end >= 0)
		p.field[p.nfields++].type = end;
	t38_last_ind = -1;
	if (t38_tx)
		t38_tx(t38_ctx, &p);
}

/* a transmission (V.21 HDLC, or a fast modulation) starting: its
   indicator now, its data once the training (V.21: the flags) has had
   its time; the V.21 receiver off */
void Fax::t38_tx_start(enum t38_kind kind, enum fax_mod mod, int long_train)
{
	t38_rx_v21 = 0;
	t38_kind = kind;
	t38_mod = kind == TK_V21 ? FM_V21 : mod;
	t38_long = long_train;
	t38_ind(mod_t38_ind(t38_mod, long_train));
	t38_step = TS_TRAIN;
	t38_wait = ms2samples(mod_train_ms(t38_mod, long_train), srate);
	t38_acc = 0;
	t38_due = t38_debt = 0;
	t38_close = 0;
	t38_sent = 0;
	t38_hpos = -1;
	t38_fmode = FMODE_UNKNOWN;
	eq_head = eq_tail = eq_pos = 0;
	EcmRx::hdlc_rx_reset();
}

/* the transmission stopped where it is (no-signal) */
void Fax::t38_tx_stop()
{
	if (t38_kind != TK_NONE)
		t38_ind(T38_IND_NO_SIGNAL);
	t38_kind = TK_NONE;
}

/* the data is over: ms of tail, then no-signal and the OK */
void Fax::t38_tail(int ms)
{
	t38_step = TS_TAIL;
	t38_wait = ms2samples(ms, srate);
	t38_due = 0;
}

/* a sample's time of the transmission: the training, the data's bit
   time (the octets the line has had time for), the tail */
void Fax::t38_tx_sample()
{
	switch (t38_step) {
	case TS_TRAIN:
		if (--t38_wait <= 0)
			t38_step = TS_DATA;
		break;
	case TS_DATA:
		t38_acc += fax_mod_bps(t38_mod);
		if (t38_acc >= 8L * srate) {
			t38_acc -= 8L * srate;
			if (t38_debt)
				t38_debt--;
			else if (t38_due < 4 * T38_DATA_MAX)
				t38_due++;
		}
		break;
	case TS_TAIL:
		if (--t38_wait <= 0) {
			t38_ind(T38_IND_NO_SIGNAL);
			t38_kind = TK_NONE;
			if (state == C1_FTH || state == C1_FTM) {
				set_state(C1_IDLE);
				report_status(FAX_STATUS_OK);
			}
		}
		break;
	}
}

/* the octets due, in packets */
void Fax::t38_flush()
{
	if (t38_kind == TK_NONE || t38_step != TS_DATA)
		return;
	if (t38_kind == TK_V21)
		t38_flush_v21();
	else
		t38_flush_fast();
}

/* +FTH: the DTE's frames as they come, an octet each 8/300 s, a frame's
   end with fcs-OK (and sig-end after the final one); a frame taken lets
   the DTE send the next (CONNECT), as on the audio */
void Fax::t38_flush_v21()
{
	while (t38_due > 0) {
		int n, left;
		if (t38_hpos < 0) {
			if (hready) {
				t38_hlen = hlen;
				flip_octets(t38_hbuf, hframe, hlen);
				t38_final = hlen >= 2 && (hframe[1] & 0x10);
				hready = 0;
				hlen = 0;
				t38_hpos = 0;
				if (!t38_final)
					report_status(FAX_STATUS_CONNECT);
			} else if (t38_close) {
				/* an empty frame: the end */
				t38_data(0, nullptr, 0, T38_HDLC_SIG_END);
				t38_tail(T38_HDLC_TAIL_MS);
				return;
			} else {
				/* the DTE's next frame not here yet: flags */
				t38_due = 0;
				return;
			}
		}
		left = t38_hlen - t38_hpos;
		n = t38_due < T38_V21_MAX ? t38_due : T38_V21_MAX;
		if (n < left) {
			t38_data(T38_HDLC_DATA, t38_hbuf + t38_hpos, n, -1);
			t38_hpos += n;
			t38_due -= n;
			continue;
		}
		t38_data(T38_HDLC_DATA, t38_hbuf + t38_hpos, left,
			 t38_final ? T38_HDLC_FCS_OK_SIG_END : T38_HDLC_FCS_OK);
		t38_due -= left;
		t38_debt = 3;           /* its FCS and a flag */
		t38_hpos = -1;
		t38_sent++;
		if (t38_final) {
			t38_tail(T38_HDLC_TAIL_MS);
			return;
		}
	}
}

/* +FTM: the DTE's octets at the modulation's rate: T.4 data (the
   octets the T.38 way round), or, if they start with HDLC flags, its
   HDLC frames taken out of them; the end after <DLE><ETX> */
void Fax::t38_flush_fast()
{
	u8 d[T38_DATA_MAX];
	int max = t38_max < T38_DATA_MAX ? t38_max : T38_DATA_MAX;

	if (t38_fmode == FMODE_UNKNOWN) {
		int avail = (head - tail + TX_BUF) % TX_BUF;
		if (avail >= 2)
			t38_fmode = buf[tail] == 0x7e &&
				buf[(tail + 1) % TX_BUF] == 0x7e ?
				FMODE_HDLC : FMODE_T4;
		else if (eod)
			t38_fmode = FMODE_T4;
		else
			return;
	}
	while (t38_due > 0) {
		int n = 0, ended;
		while (n < max && n < t38_due && head != tail) {
			d[n++] = buf[tail];
			tail = (tail + 1) % TX_BUF;
		}
		ended = eod && head == tail;
		if (!n && !ended) {
			/* the DTE behind: nothing goes for now */
			t38_due = 0;
			return;
		}
		t38_due -= n;
		if (t38_fmode == FMODE_T4) {
			flip_octets(d, d, n);
			t38_data(ended ? T38_T4_SIG_END : T38_T4_DATA, d, n, -1);
		} else {
			int i, k;
			for (i = 0; i < n; i++)
				for (k = 0; k < 8; k++)
					EcmRx::hdlc_rx_bit((d[i] >> k) & 1);
			t38_send_ecm();
			if (ended)
				t38_data(0, nullptr, 0, T38_HDLC_SIG_END);
		}
		if (ended) {
			t38_tail(t38_fmode == FMODE_T4 ? T38_T4_TAIL_MS :
				 T38_HDLC_TAIL_MS);
			return;
		}
	}
}

/* the DTE's HDLC (+FTM): a frame out of its bits, at a flag (whole
   octets, at least the address, control and FCS) */
void Fax::hdlc_rx_frame(HdlcEcm, const u8 *d, int len, bool whole)
{
	struct ecm_frame *fr;

	if (!whole || len < 4)
		return;
	if ((eq_head + 1) % ECM_QUEUE == eq_tail)
		return;
	fr = &ecm_q[eq_head];
	fr->len = len - 2;
	flip_octets(fr->data, d, fr->len);
	fr->ok = hdlc_fcs(d, len) == HDLC_FCS_GOOD;
	eq_head = (eq_head + 1) % ECM_QUEUE;
}

/* the DTE's frames that came out of its bits, each in packets, its end
   fcs-OK (fcs-BAD if the DTE's FCS was wrong) */
void Fax::t38_send_ecm()
{
	int max = t38_max < T38_DATA_MAX ? t38_max : T38_DATA_MAX;

	while (eq_tail != eq_head) {
		struct ecm_frame *fr = &ecm_q[eq_tail];
		int left = fr->len - eq_pos;
		if (left > max) {
			t38_data(T38_HDLC_DATA, fr->data + eq_pos, max, -1);
			eq_pos += max;
			continue;
		}
		t38_data(T38_HDLC_DATA, fr->data + eq_pos, left,
			 fr->ok ? T38_HDLC_FCS_OK : T38_HDLC_FCS_BAD);
		eq_pos = 0;
		eq_tail = (eq_tail + 1) % ECM_QUEUE;
	}
}


/*
 *    T.38: receiving
 */

/* V.21's signal over (sig-end, no-signal) */
void Fax::t38_v21_end()
{
	t38_sig = 0;
	t38_v21_sig = 0;
	t38_rlen = 0;
	t38_rover = 0;
}

/* a fast modulation's signal over: FRM's data ends (closing flags after
   HDLC), NO CARRIER */
void Fax::t38_fast_end()
{
	t38_sig = 0;
	if (state != C1_FRM || !rx_trained)
		return;
	if (t38_hdlc_out) {
		out_flag();
		out_flag();
	}
	out_end();
	set_state(C1_IDLE);
	report_status(FAX_STATUS_NOCARRIER);
	rx_trained = 0;
}

/* a flag to the DTE (+FRM, ECM), its bits unstuffed */
void Fax::out_flag()
{
	int k;
	for (k = 0; k < 8; k++)
		out_bit((0x7e >> k) & 1);
	t38_ones = 0;
}

/* an ECM frame to the DTE as HDLC (+FRM): flags first, then the frame
   (its octets back the line's way round) with its FCS and the stuffed
   zeros, then a flag; a bad one with its FCS spoilt */
void Fax::out_hdlc_frame(const u8 *d, int n, int ok)
{
	u8 fr[ECM_MAX];
	unsigned crc;
	int i, k;

	if (!t38_hdlc_out) {
		for (i = 0; i < 4; i++)
			out_flag();
		t38_hdlc_out = 1;
	}
	flip_octets(fr, d, n);
	crc = hdlc_fcs(fr, n) ^ 0xffff;
	if (!ok)
		crc ^= 0x5a5a;
	fr[n] = crc & 0xff;
	fr[n + 1] = crc >> 8;
	for (i = 0; i < n + 2; i++)
		for (k = 0; k < 8; k++) {
			int bit = (fr[i] >> k) & 1;
			out_bit(bit);
			if (bit) {
				if (++t38_ones == 5) {
					out_bit(0);
					t38_ones = 0;
				}
			} else {
				t38_ones = 0;
			}
		}
	out_flag();
}

/* data of a fast modulation: to the DTE while +FRM waits for it or
   takes it (its first data the CONNECT), T.4 octets as they are, HDLC
   frames as bits */
void Fax::t38_rx_fast(const struct t38_ifp *p)
{
	int k;

	for (k = 0; k < p->nfields; k++) {
		const struct t38_field *fl = &p->field[k];
		int take = state == C1_FRM;
		if (take && !rx_trained && fl->type != T38_HDLC_SIG_END &&
		    fl->type != T38_T4_SIG_END) {
			rx_trained = 1;
			rbyte = rbitn = 0;
			t38_hdlc_out = 0;
			t38_ones = 0;
			t38_elen = t38_eover = 0;
			report_status(FAX_STATUS_CONNECT);
		}
		switch (fl->type) {
		case T38_T4_DATA:
		case T38_T4_SIG_END:
			if (take && rx_trained) {
				int i;
				for (i = 0; i < fl->len; i++) {
					u8 c;
					flip_octets(&c, fl->data + i, 1);
					out_byte(c);
				}
			}
			if (fl->type == T38_T4_SIG_END)
				t38_fast_end();
			break;
		case T38_HDLC_DATA:
			if (t38_elen + fl->len <= ECM_MAX - 2) {
				memcpy(t38_eframe + t38_elen, fl->data, fl->len);
				t38_elen += fl->len;
			} else {
				t38_eover = 1;
			}
			break;
		case T38_HDLC_FCS_OK:
		case T38_HDLC_FCS_BAD:
		case T38_HDLC_FCS_OK_SIG_END:
		case T38_HDLC_FCS_BAD_SIG_END:
			if (take && rx_trained && t38_elen >= 2 && !t38_eover)
				out_hdlc_frame(t38_eframe, t38_elen,
					       fl->type == T38_HDLC_FCS_OK ||
					       fl->type ==
					       T38_HDLC_FCS_OK_SIG_END);
			t38_elen = t38_eover = 0;
			if (fl->type == T38_HDLC_FCS_OK_SIG_END ||
			    fl->type == T38_HDLC_FCS_BAD_SIG_END)
				t38_fast_end();
			break;
		case T38_HDLC_SIG_END:
			t38_elen = t38_eover = 0;
			t38_fast_end();
			break;
		default:
			break;
		}
	}
}

/* an IFP packet that came: the signals on and off, V.21 frames into
   the queue (while FRH takes them), the fast modulations' data to FRM */
void Fax::t38_rx(const struct t38_ifp *p)
{
	int k;

	if (!p->data) {
		switch (p->type) {
		case T38_IND_NO_SIGNAL:
			t38_sig = 0;
			t38_v21_end();
			t38_fast_end();
			break;
		case T38_IND_V21_PREAMBLE:
			t38_sig = 1;
			t38_v21_sig = 1;
			t38_rlen = t38_rover = 0;
			break;
		default:
			t38_sig = 1;
			break;
		}
		return;
	}
	t38_sig = 1;
	if (p->type != T38_DATA_V21) {
		t38_rx_fast(p);
		return;
	}
	t38_v21_sig = 1;
	for (k = 0; k < p->nfields; k++) {
		const struct t38_field *fl = &p->field[k];
		switch (fl->type) {
		case T38_HDLC_DATA:
			if (t38_rlen + fl->len <= HDLC_MAX - 2) {
				flip_octets(t38_rframe + t38_rlen, fl->data,
					    fl->len);
				t38_rlen += fl->len;
			} else {
				t38_rover = 1;
			}
			break;
		case T38_HDLC_FCS_OK:
		case T38_HDLC_FCS_BAD:
		case T38_HDLC_FCS_OK_SIG_END:
		case T38_HDLC_FCS_BAD_SIG_END:
			if (t38_rx_v21 && t38_rlen >= 2 && !t38_rover) {
				/* with its FCS, as the DTE has frames */
				int good = fl->type == T38_HDLC_FCS_OK ||
					fl->type == T38_HDLC_FCS_OK_SIG_END;
				unsigned crc = hdlc_fcs(t38_rframe, t38_rlen) ^
					0xffff;
				if (!good)
					crc ^= 0x5a5a;
				t38_rframe[t38_rlen] = crc & 0xff;
				t38_rframe[t38_rlen + 1] = crc >> 8;
				queue_frame(t38_rframe, t38_rlen + 2, good);
			}
			t38_rlen = t38_rover = 0;
			if (fl->type == T38_HDLC_FCS_OK_SIG_END ||
			    fl->type == T38_HDLC_FCS_BAD_SIG_END)
				t38_v21_end();
			break;
		case T38_HDLC_SIG_END:
			t38_v21_end();
			break;
		default:
			break;
		}
	}
}


/*
 *    commands
 */

/* transmit mod (the receiver stops); get_bit gives the bits */
void Fax::start_tx(enum fax_mod mod, int short_train, fax_get_bit_t get_bit)
{
	stop_rx();
	tx_mod = mod;
	tx_short = short_train;
	eod = 0;
	dle = 0;
	if (t38) {
		t38_tx_start(mod == FM_V21 ? TK_V21 : TK_FAST, mod,
			     !short_train);
		return;
	}
	tx.init(mod, short_train, srate, get_bit, this);
	tx_on = 1;
}

/* receive mod, the bits to put_bit; a V.17 short train starts from the
   last long one.  -1 when out of memory.  T.38: the packets bring the
   bits */
int Fax::start_rx(enum fax_mod mod, int short_train, fax_put_bit_t put_bit)
{
	stop_rx();
	rx_mod = mod;
	rx_trained = 0;
	if (t38) {
		if (mod == FM_V21)
			t38_rx_v21 = 1;
		return 0;
	}
	if (rx.init(mod, short_train, srate, put_bit, this,
			&rx_mem))
		return -1;
	rx_on = 1;
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
	hlen = hready = 0;
	/* 1 s of flags first */
	hdlc_tx_start(300, 16);
	start_tx(FM_V21, 0, hdlc_tx_cb);
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
	if (t38) {
		rx_mod = FM_V21;
		t38_rx_v21 = 1;
	} else if (!rx_on || rx_mod != FM_V21) {
		start_rx(FM_V21, 0, V21Rx::hdlc_rx_cb);
		V21Rx::hdlc_rx_reset();
		rflags = 0;
	}
}

/* +FTM: data at mod, CONNECT at once */
void Fax::begin_ftm(enum fax_mod mod, int short_train)
{
	stop_tx();
	set_state(C1_FTM);
	head = tail = nbits = 0;
	start_tx(mod, short_train, ftm_get_bit);
	report_status(FAX_STATUS_CONNECT);
}

/* +FRM: data at mod, CONNECT once trained (T.38: its first data); -1
   when out of memory */
int Fax::begin_frm(enum fax_mod mod, int short_train)
{
	stop_tx();
	set_state(C1_FRM);
	rbyte = rbitn = 0;
	return start_rx(mod, short_train, frm_put_bit);
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
		f->begin_ftm(mod, short_train);
		return 1;
	case FAX_CMD_FRM:
		if (!fax_mod_from_code(param, &mod, &short_train) || mod == FM_V21)
			return -1;
		return f->begin_frm(mod, short_train) ? -1 : 1;
	default:
		DSP_DBG("fax: bad command: %x\n", cmd);
		return -1;
	}
}


/*
 *    T.38 from the core
 */

/* (dsplibs.h) the line becomes T.38: the transmission in progress over
   again on it (the frame or data not yet out), the receiver on its
   packets */
int FAX_t38_start(void *obj, void (*tx)(void *ctx, const void *ifp),
		  void *ctx, int max_data)
{
	Fax *f = static_cast<Fax*>(obj);
	int was_tx;

	if (!f)
		return -1;
	DSP_DBG("fax: T.38 from now on, state %d\n", f->state);
	f->t38_tx = tx;
	f->t38_ctx = ctx;
	f->t38_max = max_data > 0 ? max_data : T38_DATA_MAX;
	if (f->t38)
		return 0;
	was_tx = f->tx_on;
	f->tx_on = 0;
	if (f->rx_on) {
		f->rx.release();
		f->rx_on = 0;
	}
	if (f->state == C1_FRM && f->rx_trained) {
		/* the data that was coming in is cut off */
		f->out_end();
		f->set_state(C1_IDLE);
		f->report_status(FAX_STATUS_NOCARRIER);
	}
	f->t38 = 1;
	f->t38_last_ind = -1;
	f->t38_kind = TK_NONE;
	f->t38_sig = f->t38_v21_sig = 0;
	f->t38_rlen = f->t38_rover = 0;
	f->rx_trained = 0;
	switch (f->state) {
	case C1_CED:
		if (f->count >= ms2samples(ANS_SILENCE_MS, f->srate))
			f->t38_ind(T38_IND_CED);
		else
			f->t38_ind(T38_IND_NO_SIGNAL);
		break;
	case C1_FTH:
		f->t38_tx_start(TK_V21, FM_V21, 0);
		break;
	case C1_FTM:
		if (was_tx)
			f->t38_tx_start(TK_FAST, f->tx_mod, !f->tx_short);
		break;
	case C1_FRH:
		f->t38_rx_v21 = 1;
		break;
	default:
		f->t38_ind(T38_IND_NO_SIGNAL);
		break;
	}
	return 0;
}

/* (dsplibs.h) back to the audio: a transmission ends (OK), a receiver
   starts again on it */
void FAX_t38_stop(void *obj)
{
	Fax *f = static_cast<Fax*>(obj);

	if (!f || !f->t38)
		return;
	DSP_DBG("fax: T.38 over, state %d\n", f->state);
	f->t38 = 0;
	f->t38_kind = TK_NONE;
	f->t38_rx_v21 = 0;
	switch (f->state) {
	case C1_FTH:
	case C1_FTM:
		f->set_state(C1_IDLE);
		f->report_status(FAX_STATUS_OK);
		break;
	case C1_FRH:
		f->start_rx(FM_V21, 0, Fax::V21Rx::hdlc_rx_cb);
		break;
	case C1_FRM:
		f->start_rx(f->rx_mod, 0, frm_put_bit);
		break;
	default:
		break;
	}
}

/* (dsplibs.h) a packet that came */
void FAX_t38_rx(void *obj, const void *ifp)
{
	Fax *f = static_cast<Fax*>(obj);

	if (f && f->t38)
		f->t38_rx(static_cast<const struct t38_ifp *>(ifp));
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
		return nullptr;
	f->modem = m;
	f->srate = srate;
	f->caller = caller;
	f->t38_last_ind = -1;
	f->cng_n = -1;
	DSP_DBG("fax: create, %s\n", caller ? "caller" : "answerer");
	if (caller) {
		f->begin_frh();           /* the called fax sends DIS */
		/* its CNG (tests: DSP_FAX_CNG=0, none) */
		if (env_on("DSP_FAX_CNG"))
			f->cng_n = 0;
	} else {
		f->set_state(C1_CED);
		f->state = C1_CED;
	}
	return f;
}

/* (dsplibs.h) an answerer whose answer tone has been on the line for
   ms already (adaptive answer: the data answer's ANSam, before the call
   was found to be a fax call): CED for what is left of its 3 s (none
   past them), then its gap and the frames */
void FAX_answer_tone(void *obj, unsigned ms)
{
	Fax *f = static_cast<Fax*>(obj);

	if (!f || f->caller || f->state != C1_CED)
		return;
	if (ms > FAX_CED_MS)
		ms = FAX_CED_MS;
	f->count = ms2samples(ANS_SILENCE_MS + ms, f->srate);
	DSP_DBG("fax: %u ms of answer tone sent already\n", ms);
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

/* count samples of 16 bit audio in and out: the transmitter, the
   receiver and the state of the command, the data to and from the DTE.
   On T.38 the samples are its time, the audio silence.  The next result
   (FAX_STATUS_*), or 0 */
int FAX_process(void *obj, void *in, void *out, int count)
{
	Fax *f = static_cast<Fax*>(obj);
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int i, status = 0;

	if (f->state == C1_FTH || f->state == C1_FTM)
		f->read_dte();

	for (i = 0; i < count; i++) {
		float y = 0, x = f->t38 ? 0 : src[i];

		f->in_power += 0.002f * (x * x - f->in_power);
		if (f->t38) {
			if (f->t38_kind != TK_NONE)
				f->t38_tx_sample();
		} else if (f->state == C1_CED)
			y = fax_ced_sample(f->count, f->srate);
		else if (f->cng_n >= 0) {
			if (f->cng_n % ms2samples(3500, f->srate) <
			    ms2samples(500, f->srate))
				y = FAX_CED_RMS * 1.41421f *
					sinf(2 * (float)M_PI * 1100 *
					     (float)(f->cng_n % f->srate) /
					     f->srate);
			f->cng_n++;
		} else if (f->tx_on) {
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

		/* the answerer's flags, or the first +FRH over: no more
		   CNG */
		if (f->cng_n >= 0 && (f->state != C1_FRH || f->rflags))
			f->cng_n = -1;

		switch (f->state) {
		case C1_CED:
			if (f->t38) {
				if (f->count == 1)
					f->t38_ind(T38_IND_NO_SIGNAL);
				else if (f->count ==
					 ms2samples(ANS_SILENCE_MS, f->srate))
					f->t38_ind(T38_IND_CED);
			}
			if (f->count >= ms2samples(FAX_CED_END_MS, f->srate))
				f->begin_fth();
			break;
		case C1_FTS:
			if (f->count >= f->duration) {
				f->set_state(C1_IDLE);
				f->report_status(FAX_STATUS_OK);
			}
			break;
		case C1_FRS:
			if (f->t38 ? !f->t38_sig :
			    f->in_power < tone_power(150))
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
			} else if (!f->frh_connected &&
				   (f->t38 ? f->t38_v21_sig :
				    f->rflags >= 2 && f->rx.carrier)) {
				f->frh_connected = 1;
				f->report_status(FAX_STATUS_CONNECT);
			} else if (f->frh_connected &&
				   (f->t38 ? !f->t38_v21_sig :
				    f->rx.carrier_run <
				    -ms2samples(200, f->srate))) {
				/* the carrier went away before a frame */
				f->out_end();
				f->set_state(C1_IDLE);
				f->report_status(FAX_STATUS_NOCARRIER);
			}
			break;
		case C1_FRM:
			if (f->t38)
				break;  /* (t38_rx_fast) */
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
	if (f->t38)
		f->t38_flush();

	/* data received after a CONNECT follows it on the tty */
	if (!f->nstatus || f->status[0] != FAX_STATUS_CONNECT)
		f->out_flush();

	/* the DTE stops FRH, FRM and FRS with any character but XON and
	   XOFF, before their CONNECT too (T.31 8.3.2, 8.3.4, 8.3.6) */
	if (f->state == C1_FRM || f->state == C1_FRH || f->state == C1_FRS) {
		char c;
		while (modem_recv_from_tty(f->modem, &c, 1) == 1) {
			if (c == 0x11 || c == 0x13)
				continue;
			DSP_DBG("fax: receive aborted by the DTE\n");
			if (f->state == C1_FRM)
				f->stop_rx();
			f->set_state(C1_IDLE);
			f->report_status(FAX_STATUS_OK);
			break;
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
