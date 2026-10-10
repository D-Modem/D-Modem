/*
 *
 *    t30.cpp - a group 3 fax machine (ITU-T T.30, without error correction
 *            mode) on the fax modulations of fax_modems.cpp
 *
 *    Control messages go as HDLC frames on V.21 channel 2, pages and the
 *    training check on V.27ter, V.29 or V.17.  The calling machine sends:
 *
 *        caller                          answerer
 *        CNG ...                         CED
 *                                        [CSI] DIS
 *        [TSI] DCS, TCF
 *                                        CFR (or FTT: a lower rate)
 *        page
 *        MPS / EOP / EOM
 *                                        MCF (RTP, RTN: retrain)
 *        ...
 *        DCN
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "dsp.h"
#include "fax.h"
#include "t30.h"

namespace dsp {

/* frame control fields as they come off the line (T.30 5.3.6); the
   lowest bit is set in frames of the machine that received the DIS */
enum {
	FCF_DIS = 0x80, FCF_CSI = 0x40, FCF_NSF = 0x20, FCF_DTC = 0x81,
	FCF_DCS = 0x82, FCF_TSI = 0x42, FCF_NSS = 0x22,
	FCF_CFR = 0x84, FCF_FTT = 0x44,
	FCF_EOM = 0x8e, FCF_MPS = 0x4e, FCF_EOP = 0x2e,
	FCF_PRI_EOM = 0x9e, FCF_PRI_MPS = 0x5e, FCF_PRI_EOP = 0x3e,
	FCF_MCF = 0x8c, FCF_RTP = 0xcc, FCF_RTN = 0x4c,
	FCF_PIP = 0xac, FCF_PIN = 0x2c,
	FCF_DCN = 0xfa, FCF_CRP = 0x1a,
};

#define HDLC_ADDR       0xff
#define HDLC_CTRL       0x03
#define HDLC_FINAL      0x10

#define MAX_FRAMES      4
#define FRAME_MAX       64      /* our frames; received ones may be longer */
#define RX_FRAME_MAX    260

/* the timers (T.30 5.4.3) */
#define T1_MS           35000   /* to find the other machine */
#define T2_MS           6000    /* for a command */
#define T4_MS           3000    /* for a response */
#define RESPONSE_GAP_MS 75      /* between a message and the answer */
#define TRIES           3

/* the fast modulations, best first, with their DCS rate bits (11-14 as
   bit 0-3) */
static const struct rate {
	enum fax_mod mod;
	int bps;
	unsigned bits;
	int family;                     /* 2: V.17, 1: V.29, 0: V.27ter */
} rates[] = {
	{ FM_V17_14400, 14400, 0x8, 2 },
	{ FM_V17_12000, 12000, 0xa, 2 },
	{ FM_V17_9600,  9600,  0x9, 2 },
	{ FM_V17_7200,  7200,  0xb, 2 },
	{ FM_V29_9600,  9600,  0x1, 1 },
	{ FM_V29_7200,  7200,  0x3, 1 },
	{ FM_V27_4800,  4800,  0x2, 0 },
	{ FM_V27_2400,  2400,  0x0, 0 },
};
#define NRATES ((int)(sizeof(rates) / sizeof(rates[0])))

enum state {
	S_IDLE,
	/* answering (receiving) */
	S_CED,
	S_WAIT_DCS,             /* after our DIS */
	S_RX_TCF,
	S_RX_PAGE,              /* after CFR or MCF */
	S_WAIT_POST,            /* the post-page command */
	S_WAIT_DCN,
	/* calling (sending) */
	S_CNG,                  /* waiting for the DIS */
	S_TCF,                  /* sending it */
	S_WAIT_CFR,
	S_PAGE,
	S_WAIT_MCF,
	/* both */
	S_GAP,                  /* silence before the next transmission */
	S_TX_FRAMES,
	S_DONE,
};

static const char *state_name[] = {
	"idle", "CED", "wait DCS", "receive TCF", "receive page",
	"wait post-page command", "wait DCN", "CNG", "send TCF", "wait CFR",
	"send page", "wait MCF", "gap", "send frames", "done",
};

struct frame {
	unsigned char data[FRAME_MAX];
	int len;                        /* without the FCS */
};

struct T30 {
	struct t30_config cfg;
	int srate;
	enum state state, after_tx;     /* where to go when the frames went */
	enum t30_result result;
	long count;                     /* samples in this state */
	long timer;                     /* samples left, 0: none */
	long t1;                        /* samples to T1, 0: off */
	int tries;

	/* the transmitter: frames, a modem or a tone */
	int tx_on;
	FaxTx tx;
	struct frame frames[MAX_FRAMES];
	int nframes, cur_frame, cur_pos, cur_bit, ones;
	int flags_left, closing, preamble;
	long gap;                       /* S_GAP: samples left */
	void (T30::*after_gap)();
	long tcf_bits;                  /* to send */
	T4Bits page_bits;
	size_t page_pos;

	/* receivers: V.21 for frames, a fast one for TCF and pages */
	FaxRx v21;
	FaxRx fast;
	int fast_on;
	FaxRxMemory mem;
	unsigned char rbits[RX_FRAME_MAX * 9];
	int rnbits, rones;
	unsigned char msg[MAX_FRAMES * 2][RX_FRAME_MAX];
	int msg_len[MAX_FRAMES * 2], nmsg;
	int msg_ready;

	/* the session */
	int remote_dis_ok;
	unsigned char dis[16];          /* the remote's DIS FIF */
	int dis_len;
	int rate;                       /* index in rates[] */
	int fine, mr, min_ms;
	char remote_ident[21];

	/* sending */
	T4Page page, next;
	int have_page, have_next;
	int page_tries;
	int last_post;                  /* MPS, EOM, EOP */
	int cng;                        /* S_CNG with the tone */
	int need_retrain;

	/* receiving */
	T4Page rx_page;
	T4Rx t4rx;
	int rx_page_on;
	int page_ok;
	int eop_acked;                  /* MCF to an EOP sent: all came */
	long zero_run, max_zero_run, tcf_trained;

	int pages;

	void set_state(enum state s);
	void start_timer(int ms);
	void stop_fast();
	void start_fast(int short_train);
	void restart_v21();
	void add_frame(int fcf, const unsigned char *fif, int len);
	void send_frames(enum state next_);
	void new_frames();
	void gap_then(int ms, void (T30::*f)());
	void finish(enum t30_result r);
	void send_dcn(enum t30_result r);
	void send_dis();
	void start_page_rx();
	void page_done();
	void tcf_done();
	void got_dcs(const unsigned char *fif, int len);
	void rx_message();
	void encode_page();
	void send_tcf();
	void send_dcs();
	void send_page();
	int load_next();
	void send_post_page();
	int choose();
	void next_page_or_end();
	void page_settings();
	void sender_message();
	void frame_end();
	void on_timeout();
	void tx_done();
	float tone(float freq);
	float tx_sample();
	void rx_sample(float x);
};


/*
 *    bits of the facsimile information field
 */

static void set_bit(unsigned char *fif, int bit)
{
	fif[(bit - 1) / 8] |= 1 << ((bit - 1) % 8);
}

static int get_bit(const unsigned char *fif, int len, int bit)
{
	if ((bit - 1) / 8 >= len)
		return 0;
	return (fif[(bit - 1) / 8] >> ((bit - 1) % 8)) & 1;
}

/* the minimum scan line time a DIS asks for (bits 21-23), ms */
static int dis_min_ms(const unsigned char *fif, int len)
{
	static const int ms[8] = { 20, 40, 10, 10, 5, 40, 20, 0 };
	int c = get_bit(fif, len, 21) << 2 | get_bit(fif, len, 22) << 1 |
		get_bit(fif, len, 23);
	return ms[c];
}

/* ... and its code in a DCS */
static void dcs_min_ms(unsigned char *fif, int ms)
{
	switch (ms) {
	case 40: set_bit(fif, 23); break;
	case 10: set_bit(fif, 22); break;
	case 5:  set_bit(fif, 21); break;
	case 0:  set_bit(fif, 21); set_bit(fif, 22); set_bit(fif, 23); break;
	default: break;                 /* 20 ms */
	}
}

/* the rates a DIS offers */
static int dis_rate_ok(const unsigned char *fif, int len, const struct rate *r)
{
	unsigned v = get_bit(fif, len, 11) | get_bit(fif, len, 12) << 1 |
		     get_bit(fif, len, 13) << 2 | get_bit(fif, len, 14) << 3;
	switch (r->family) {
	case 2:
		return v == 0xb || v == 0xf || (v & 0x8);
	case 1:
		return v & 0x1;
	default:
		return r->bps == 2400 || (v & 0x2);
	}
}

/* a CSI / TSI: 20 characters, the last first */
static int put_ident(unsigned char *d, const char *id)
{
	int k, n = id ? (int)strlen(id) : 0;
	if (n > 20)
		n = 20;
	for (k = 0; k < 20; k++)
		d[k] = k < n ? (unsigned char)id[n - 1 - k] : ' ';
	return 20;
}

static void get_ident(char *id, const unsigned char *d, int len)
{
	int k, n = 0;
	if (len > 20)
		len = 20;
	for (k = len - 1; k >= 0; k--)
		id[n++] = d[k] >= 32 && d[k] < 127 ? d[k] : ' ';
	while (n && id[n - 1] == ' ')
		n--;
	id[n] = 0;
	for (k = 0; id[k] == ' '; k++)
		;
	memmove(id, id + k, n - k + 1);
}


/*
 *    the transmitter
 */

void T30::set_state(enum state s)
{
	DSP_DBG("t30: %s -> %s\n", state_name[state], state_name[s]);
	state = s;
	count = 0;
	timer = 0;
}

void T30::start_timer(int ms)
{
	timer = ms2samples(ms, srate);
}

static void page_bit(void *ctx, int bit);

void T30::stop_fast()
{
	if (fast_on) {
		fast.save(&mem);
		fast.release();
	}
	fast_on = 0;
}

void T30::start_fast(int short_train)
{
	stop_fast();
	if (!fast.init(rates[rate].mod, short_train, srate,
			 page_bit, this, &mem))
		fast_on = 1;
}


void T30::add_frame(int fcf, const unsigned char *fif, int len)
{
	struct frame *f;
	if (nframes == MAX_FRAMES)
		return;
	f = &frames[nframes++];
	f->data[0] = HDLC_ADDR;
	f->data[1] = HDLC_CTRL;
	/* the lowest bit says who sent it (the one that got the DIS) */
	f->data[2] = (unsigned char)(fcf | (cfg.sending ? 1 : 0));
	if (len > FRAME_MAX - 3)
		len = FRAME_MAX - 3;
	if (len)
		memcpy(f->data + 3, fif, len);
	f->len = 3 + len;
}

static unsigned crc16(const unsigned char *p, int n)
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

/* HDLC: flags, the frames (the last with the final bit) with their FCS
   and bit stuffing, closing flags */
static int frames_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	struct frame *f;
	int bit;

	if (t->ones == 5) {
		t->ones = 0;
		return 0;               /* stuffed (also after the FCS) */
	}
	if (t->flags_left > 0 || t->cur_frame >= t->nframes) {
		if (t->flags_left <= 0) {
			if (t->closing)
				return FAX_BIT_END;
			t->closing = 1;
			t->flags_left = 2 * 8;
		}
		bit = (0x7e >> (7 - ((t->flags_left - 1) & 7))) & 1;
		t->flags_left--;
		t->ones = 0;
		return bit;
	}
	f = &t->frames[t->cur_frame];
	bit = (f->data[t->cur_pos] >> t->cur_bit) & 1;
	if (++t->cur_bit == 8) {
		t->cur_bit = 0;
		if (++t->cur_pos == f->len + 2) {
			/* the next frame after a flag */
			t->cur_pos = 0;
			t->cur_frame++;
			t->flags_left = 8;
		}
	}
	t->ones = bit ? t->ones + 1 : 0;
	return bit;
}

/* send the frames added; then on to state next */
void T30::send_frames(enum state next_)
{
	int k;
	for (k = 0; k < nframes; k++) {
		struct frame *f = &frames[k];
		unsigned crc;
		if (k == nframes - 1)
			f->data[1] |= HDLC_FINAL;
		crc = crc16(f->data, f->len) ^ 0xffff;
		f->data[f->len] = crc & 0xff;
		f->data[f->len + 1] = crc >> 8;
	}
	cur_frame = cur_pos = cur_bit = ones = 0;
	closing = 0;
	/* a second of flags first (T.30 5.3.1) */
	flags_left = (int)(1.0 * 300) / 8 * 8;
	tx.init(FM_V21, 0, srate, frames_get_bit, this);
	tx_on = 1;
	after_tx = next_;
	set_state(S_TX_FRAMES);
	DSP_DBG("t30: send %d frames, fcf %02x\n", nframes,
		frames[nframes - 1].data[2]);
}

void T30::new_frames()
{
	nframes = 0;
}

/* silence, then f */
void T30::gap_then(int ms, void (T30::*f)())
{
	set_state(S_GAP);
	gap = ms2samples(ms, srate);
	after_gap = f;
}

void T30::finish(enum t30_result r)
{
	if (result == T30_RUNNING) {
		result = r;
		DSP_DBG("t30: finished: %s\n", t30_result_text(r));
	}
	stop_fast();
	set_state(S_DONE);
}

/* a DCN, then the end */
void T30::send_dcn(enum t30_result r)
{
	if (result == T30_RUNNING)
		result = r;
	new_frames();
	add_frame(FCF_DCN, NULL, 0);
	send_frames(S_DONE);
}


/*
 *    receiving: the answering machine
 */

void T30::send_dis()
{
	unsigned char fif[4] = { 0 }, id[20];

	set_bit(fif, 10);                       /* receives */
	/* V.27ter, V.29 and V.17 as far as the configuration goes */
	if (cfg.max_bps > 9600) {
		set_bit(fif, 11), set_bit(fif, 12), set_bit(fif, 14);
	} else if (cfg.max_bps >= 7200) {
		set_bit(fif, 11), set_bit(fif, 12);
	} else {
		set_bit(fif, 12);
	}
	set_bit(fif, 15);                       /* fine */
	set_bit(fif, 16);                       /* MR */
	set_bit(fif, 19);                       /* any length */
	set_bit(fif, 21), set_bit(fif, 22), set_bit(fif, 23);   /* 0 ms */
	new_frames();
	add_frame(FCF_CSI, id, put_ident(id, cfg.ident));
	add_frame(FCF_DIS, fif, 3);
	send_frames(S_WAIT_DCS);
}

void T30::start_page_rx()
{
	int v17 = rates[rate].family == 2;
	if (rx_page_on) {
		t4rx.release();
		rx_page.release();
	}
	rx_page.init(T4_WIDTH, fine);
	t4rx.init(&rx_page, mr);
	rx_page_on = 1;
	start_fast(v17);
	set_state(S_RX_PAGE);
	start_timer(T2_MS * 2);
}

static void page_bit(void *ctx, int bit)
{
	T30 *t = static_cast<T30*>(ctx);
	if (t->state == S_RX_TCF) {
		if (bit) {
			t->zero_run = 0;
		} else if (++t->zero_run > t->max_zero_run) {
			t->max_zero_run = t->zero_run;
		}
		t->tcf_trained = 1;
	} else if (t->state == S_RX_PAGE && t->rx_page_on) {
		t->t4rx.bit(bit);
	}
}

/* the response to the page: MCF if it came, RTN if not */
void T30::page_done()
{
	T4Page *p = &rx_page;
	t4rx.end();
	page_ok = p->rows > 100 && p->bad_rows * 20 < p->rows;
	DSP_DBG("t30: page of %d rows, %d bad: %s\n", p->rows, p->bad_rows,
		page_ok ? "ok" : "bad");
	if (page_ok) {
		pages++;
		if (cfg.page_received)
			cfg.page_received(cfg.ctx, p);
	}
	t4rx.release();
	p->release();
	rx_page_on = 0;
	stop_fast();
	set_state(S_WAIT_POST);
	start_timer(T2_MS);
}

void T30::tcf_done()
{
	int bps = rates[rate].bps;
	int ok = max_zero_run >= bps * 3 / 4;        /* 750 ms */
	DSP_DBG("t30: TCF: %ld zeros in a row (of %d): %s\n",
		max_zero_run, bps * 3 / 2, ok ? "ok" : "failed");
	stop_fast();
	new_frames();
	add_frame(ok ? FCF_CFR : FCF_FTT, NULL, 0);
	if (ok) {
		tries = 0;
		send_frames(S_RX_PAGE);      /* start_page_rx after */
	} else {
		send_frames(S_WAIT_DCS);
	}
}

void T30::got_dcs(const unsigned char *fif, int len)
{
	unsigned v = get_bit(fif, len, 11) | get_bit(fif, len, 12) << 1 |
		     get_bit(fif, len, 13) << 2 | get_bit(fif, len, 14) << 3;
	int k;
	for (k = 0; k < NRATES; k++)
		if (rates[k].bits == v)
			break;
	if (k == NRATES) {
		DSP_DBG("t30: DCS with unknown rate %x\n", v);
		return;
	}
	rate = k;
	fine = get_bit(fif, len, 15);
	mr = get_bit(fif, len, 16);
	DSP_DBG("t30: DCS: %d bit/s, %s, %s\n", rates[k].bps,
		fine ? "fine" : "standard", mr ? "MR" : "MH");
	/* the training check follows on the fast modulation */
	zero_run = max_zero_run = tcf_trained = 0;
	memset(&mem, 0, sizeof(mem));
	start_fast(0);
	set_state(S_RX_TCF);
	start_timer(T4_MS + 1500);
}

void T30::rx_message()
{
	int k, fcf = 0, last = nmsg - 1;
	const unsigned char *fif = NULL;
	int fif_len = 0;

	/* the identity, then the command */
	for (k = 0; k < nmsg; k++) {
		int f = msg[k][2] & 0xfe;
		if ((f == FCF_TSI || f == FCF_CSI) && msg_len[k] > 3)
			get_ident(remote_ident, msg[k] + 3,
				  msg_len[k] - 3);
	}
	fcf = msg[last][2] & 0xfe;
	fif = msg[last] + 3;
	fif_len = msg_len[last] - 3;
	DSP_DBG("t30: received fcf %02x (%d frames) in %s\n", fcf, nmsg,
		state_name[state]);

	if (fcf == FCF_DCN) {
		stop_fast();
		if (cfg.sending)
			finish(T30_ERR_HANGUP);
		else
			finish(pages ? T30_OK : T30_ERR_HANGUP);
		return;
	}
	if (cfg.sending) {
		/* the calling machine */
		switch (fcf) {
		case FCF_DIS:
			if (fif_len > (int)sizeof(dis))
				fif_len = sizeof(dis);
			memcpy(dis, fif, fif_len);
			dis_len = fif_len;
			break;
		}
		return;         /* the rest in the sender's own handler */
	}

	/* the answering machine */
	switch (fcf) {
	case FCF_DCS:
		got_dcs(fif, fif_len);
		break;
	case FCF_MPS: case FCF_EOP: case FCF_EOM:
	case FCF_PRI_MPS: case FCF_PRI_EOP: case FCF_PRI_EOM:
		if (state == S_RX_PAGE && rx_page_on) {
			/* the page ended without our seeing the carrier go */
			page_done();
		}
		new_frames();
		add_frame(page_ok ? FCF_MCF : FCF_RTN, NULL, 0);
		if (!page_ok)
			send_frames(S_WAIT_DCS);     /* it retrains */
		else if (fcf == FCF_MPS || fcf == FCF_PRI_MPS)
			send_frames(S_RX_PAGE);
		else if (fcf == FCF_EOP || fcf == FCF_PRI_EOP) {
			eop_acked = 1;
			send_frames(S_WAIT_DCN);
		}
		else
			send_frames(S_CED);          /* EOM: phase B again */
		page_ok = 0;
		break;
	default:
		break;
	}
}


/*
 *    sending: the calling machine
 */

void T30::encode_page()
{
	int bps = rates[rate].bps;
	page_bits.release();
	page.encode_mh(&page_bits, min_ms * bps / 1000, 0, 1);
	page_pos = 0;
}

static int page_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	if (t->page_pos >= t->page_bits.n)
		return FAX_BIT_END;
	return t4_bits_get(&t->page_bits, t->page_pos++);
}

static int tcf_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	if (t->tcf_bits <= 0)
		return FAX_BIT_END;
	t->tcf_bits--;
	return 0;
}

void T30::send_tcf()
{
	tcf_bits = rates[rate].bps * 3 / 2;       /* 1.5 s */
	tx.init(rates[rate].mod, 0, srate, tcf_get_bit, this);
	tx_on = 1;
	set_state(S_TCF);
}

void T30::send_dcs()
{
	unsigned char fif[3] = { 0 }, id[20];
	unsigned v = rates[rate].bits;

	set_bit(fif, 10);
	if (v & 1) set_bit(fif, 11);
	if (v & 2) set_bit(fif, 12);
	if (v & 4) set_bit(fif, 13);
	if (v & 8) set_bit(fif, 14);
	if (fine)
		set_bit(fif, 15);
	if (get_bit(dis, dis_len, 19))
		set_bit(fif, 19);               /* any length */
	dcs_min_ms(fif, min_ms);
	DSP_DBG("t30: DCS: %d bit/s, %s, min scan %d ms\n", rates[rate].bps,
		fine ? "fine" : "standard", min_ms);
	new_frames();
	add_frame(FCF_TSI, id, put_ident(id, cfg.ident));
	add_frame(FCF_DCS, fif, 3);
	send_frames(S_TCF);          /* the TCF after a gap */
}

void T30::send_page()
{
	encode_page();
	DSP_DBG("t30: page %d: %d rows, %zu bits\n", pages + 1,
		page.rows, page_bits.n);
	tx.init(rates[rate].mod,
		    rates[rate].family == 2, srate, page_get_bit, this);
	tx_on = 1;
	set_state(S_PAGE);
}

int T30::load_next()
{
	if (have_next)
		return 1;
	if (cfg.next_page && cfg.next_page(cfg.ctx, &next) == 1)
		have_next = 1;
	return have_next;
}

void T30::send_post_page()
{
	int more = load_next();
	/* another resolution needs another DCS: EOM, then phase B again */
	last_post = !more ? FCF_EOP :
		(next.fine != 0) != (fine != 0) ? FCF_EOM : FCF_MPS;
	new_frames();
	add_frame(last_post, NULL, 0);
	send_frames(S_WAIT_MCF);
}

/* the DIS came: what the call can do */
int T30::choose()
{
	int k;
	for (k = 0; k < NRATES; k++)
		if (rates[k].bps <= cfg.max_bps &&
		    dis_rate_ok(dis, dis_len, &rates[k]))
			break;
	if (k == NRATES)
		return -1;
	rate = k;
	min_ms = dis_min_ms(dis, dis_len);
	return 0;
}

void T30::next_page_or_end()
{
	if (have_page)
		page.release();
	have_page = 0;
	if (have_next) {
		page = next;
		have_page = 1;
		have_next = 0;
	}
}

/* the page settings to tell in the DCS */
void T30::page_settings()
{
	fine = page.fine && get_bit(dis, dis_len, 15);
	if (page.fine && !fine) {
		/* the remote takes only standard: every other row */
		T4Page p;
		int y, s = T4_STRIDE(page.width);
		p.init(page.width, 0);
		for (y = 0; y < page.rows; y += 2)
			p.add_row(page.bits + (size_t)y * s);
		page.release();
		page = p;
	}
}

void T30::sender_message()
{
	int fcf = msg[nmsg - 1][2] & 0xfe;

	switch (state) {
	case S_CNG:
		if (fcf != FCF_DIS)
			break;
		t1 = 0;
		if (choose()) {
			send_dcn(T30_ERR_INCOMPATIBLE);
			break;
		}
		if (!have_page && !load_next()) {
			send_dcn(T30_ERR_DOCUMENT);
			break;
		}
		if (!have_page)
			next_page_or_end();
		page_settings();
		tries = 0;
		gap_then(RESPONSE_GAP_MS, &T30::send_dcs);
		break;
	case S_WAIT_CFR:
		if (fcf == FCF_CFR) {
			tries = 0;
			page_tries = 0;
			gap_then(RESPONSE_GAP_MS, &T30::send_page);
		} else if (fcf == FCF_FTT) {
			/* a lower rate the remote has */
			int k;
			for (k = rate + 1; k < NRATES; k++)
				if (dis_rate_ok(dis, dis_len, &rates[k]))
					break;
			if (k == NRATES) {
				send_dcn(T30_ERR_TRAINING);
				break;
			}
			rate = k;
			tries = 0;
			gap_then(RESPONSE_GAP_MS, &T30::send_dcs);
		} else if (fcf == FCF_DIS) {
			/* it didn't get our DCS */
			gap_then(RESPONSE_GAP_MS, &T30::send_dcs);
		}
		break;
	case S_WAIT_MCF:
		if (fcf == FCF_MCF || fcf == FCF_RTP || fcf == FCF_PIP) {
			pages++;
			tries = 0;
			page_tries = 0;
			next_page_or_end();
			if (last_post == FCF_EOP) {
				send_dcn(T30_OK);
			} else if (last_post == FCF_EOM) {
				/* the receiver sends a DIS again */
				cng = 0;
				set_state(S_CNG);
				start_timer(T1_MS);
			} else if (fcf == FCF_MCF) {
				page_settings();
				gap_then(RESPONSE_GAP_MS, &T30::send_page);
			} else {
				page_settings();
				gap_then(RESPONSE_GAP_MS, &T30::send_dcs);
			}
		} else if (fcf == FCF_RTN || fcf == FCF_PIN) {
			/* send it again, after training again */
			if (++page_tries >= TRIES) {
				send_dcn(T30_ERR_PAGE);
				break;
			}
			tries = 0;
			gap_then(RESPONSE_GAP_MS, &T30::send_dcs);
		} else if (fcf == FCF_CRP) {
			gap_then(RESPONSE_GAP_MS, &T30::send_post_page);
		}
		break;
	default:
		break;
	}
}


/*
 *    the V.21 receiver: HDLC frames into messages
 */

void T30::frame_end()
{
	int n = rnbits - 7, k, len;
	unsigned char *d;

	rnbits = 0;
	if (n < 32 || n % 8 || n / 8 > RX_FRAME_MAX)
		return;
	len = n / 8;
	if (nmsg >= MAX_FRAMES * 2)
		return;
	d = msg[nmsg];
	for (k = 0; k < len; k++) {
		int b, v = 0;
		for (b = 0; b < 8; b++)
			v |= rbits[k * 8 + b] << b;
		d[k] = v;
	}
	if (crc16(d, len) != 0xf0b8 || d[0] != HDLC_ADDR) {
		DSP_DBG("t30: frame of %d bytes with a bad FCS\n", len);
		return;
	}
	msg_len[nmsg++] = len - 2;
	if (d[1] & HDLC_FINAL)
		msg_ready = 1;
}

static void v21_bit(void *ctx, int bit)
{
	T30 *t = static_cast<T30*>(ctx);
	if (bit) {
		if (++t->rones > 6) {           /* idle or abort */
			t->rnbits = 0;
			return;
		}
	} else {
		if (t->rones == 6) {            /* a flag */
			t->frame_end();
			t->rones = 0;
			return;
		}
		if (t->rones == 5) {            /* stuffed */
			t->rones = 0;
			return;
		}
		t->rones = 0;
	}
	if (t->rnbits < (int)sizeof(t->rbits))
		t->rbits[t->rnbits++] = bit;
}

void T30::restart_v21()
{
	v21.init(FM_V21, 0, srate, v21_bit, this, NULL);
	rnbits = rones = 0;
	nmsg = 0;
	msg_ready = 0;
}


/*
 *    the machine
 */

void T30::on_timeout()
{
	switch (state) {
	case S_WAIT_DCS:
		/* no answer to the DIS: again, for T1 */
		if (++tries >= T1_MS / (T4_MS + 2000))
			send_dcn(T30_ERR_NO_RESPONSE);
		else
			send_dis();
		break;
	case S_RX_TCF:
		/* the training check never came right */
		tcf_done();
		break;
	case S_RX_PAGE:
	case S_WAIT_POST:
		send_dcn(T30_ERR_NO_RESPONSE);
		break;
	case S_WAIT_DCN:
		finish(T30_OK);
		break;
	case S_CNG:
		send_dcn(T30_ERR_NO_RESPONSE);
		break;
	case S_WAIT_CFR:
		if (++tries >= TRIES)
			send_dcn(T30_ERR_NO_RESPONSE);
		else
			gap_then(0, &T30::send_dcs);
		break;
	case S_WAIT_MCF:
		if (++tries >= TRIES)
			send_dcn(T30_ERR_NO_RESPONSE);
		else
			gap_then(0, &T30::send_post_page);
		break;
	default:
		break;
	}
}

/* the transmitter finished */
void T30::tx_done()
{
	tx_on = 0;
	restart_v21();
	switch (state) {
	case S_TX_FRAMES:
		if (after_tx == S_DONE) {
			finish(result == T30_RUNNING ? T30_OK : result);
		} else if (after_tx == S_TCF) {
			gap_then(RESPONSE_GAP_MS, &T30::send_tcf);
		} else if (after_tx == S_RX_PAGE) {
			start_page_rx();
		} else if (after_tx == S_CED) {
			gap_then(RESPONSE_GAP_MS, &T30::send_dis);
		} else {
			set_state(after_tx);
			start_timer(after_tx == S_WAIT_DCN ? T2_MS :
				    after_tx == S_WAIT_DCS ? T4_MS : T4_MS);
		}
		break;
	case S_TCF:
		set_state(S_WAIT_CFR);
		start_timer(T4_MS);
		break;
	case S_PAGE:
		gap_then(RESPONSE_GAP_MS, &T30::send_post_page);
		break;
	default:
		break;
	}
}

float T30::tone(float freq)
{
	return FAX_CED_RMS * 1.41421f *
		sinf(2 * (float)M_PI * freq * count / srate);
}

float T30::tx_sample()
{
	float y = 0;
	long c = count;

	if (tx_on) {
		y = tx.sample();
		if (tx.done)
			tx_done();
		return y;
	}
	switch (state) {
	case S_CNG:
		/* 1100 Hz, 0.5 s on, 3 s off */
		if (cng && c % ms2samples(3500, srate) < ms2samples(500, srate))
			y = tone(1100) * 0.5f;
		break;
	case S_CED:
		/* 200 ms of silence, 3 s of 2100 Hz, then 75 ms silence */
		if (c >= ms2samples(200, srate) &&
		    c < ms2samples(3200, srate))
			y = tone(2100);
		else if (c >= ms2samples(3275, srate))
			send_dis();
		break;
	case S_GAP:
		if (--gap <= 0)
			(this->*after_gap)();
		break;
	default:
		break;
	}
	return y;
}

void T30::rx_sample(float x)
{
	if (tx_on || state == S_DONE)
		return;
	v21.sample(x);
	if (fast_on) {
		fast.sample(x);
		if (state == S_RX_TCF && tcf_trained &&
		    !fast.carrier) {
			tcf_done();
		} else if (state == S_RX_PAGE && fast.trained &&
			   fast.carrier_run < -ms2samples(100, srate)) {
			page_done();
		} else if (state == S_RX_PAGE && fast.trained &&
			   rx_page_on && t4rx.done) {
			/* RTC: the rest is no page */
			page_done();
		} else if (state == S_RX_PAGE && fast.trained) {
			/* the page may take minutes: the timer only for
			   the start of it */
			start_timer(T2_MS);
		}
	}
	if (msg_ready ||
	    (nmsg && !v21.carrier && rnbits == 0)) {
		/* a whole message (or what came before the carrier went) */
		if (nmsg) {
			if (cfg.sending) {
				rx_message();
				if (state != S_DONE)
					sender_message();
			} else {
				rx_message();
			}
		}
		nmsg = 0;
		msg_ready = 0;
	}
}

void t30_process(T30 *t, const short *in, short *out, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		float x = in ? in[i] : 0;
		float y = 0;
		if (t->state != S_DONE) {
			y = t->tx_sample();
			t->rx_sample(x);
			t->count++;
			if (t->timer && --t->timer == 0)
				t->on_timeout();
			if (t->t1 && --t->t1 == 0 && t->state == S_CNG)
				t->finish(T30_ERR_NO_FAX);
		}
		if (out)
			out[i] = dsp_clip16(y);
	}
}

T30 *t30_create(const struct t30_config *cfg)
{
	T30 *t = new (std::nothrow) T30();
	if (!t)
		return NULL;
	t->cfg = *cfg;
	t->srate = cfg->srate ? cfg->srate : 8000;
	if (!t->cfg.max_bps)
		t->cfg.max_bps = 14400;
	t->result = T30_RUNNING;
	t->restart_v21();
	if (cfg->sending) {
		t->set_state(S_CNG);
		t->cng = 1;
		t->t1 = ms2samples(T1_MS, t->srate);
		/* the first page now: no document, no call */
		if (!t->load_next())
			t->finish(T30_ERR_DOCUMENT);
		else
			t->next_page_or_end();
	} else {
		t->set_state(S_CED);
	}
	return t;
}

void t30_free(T30 *t)
{
	if (!t)
		return;
	t->stop_fast();
	if (t->rx_page_on) {
		t->t4rx.release();
		t->rx_page.release();
	}
	if (t->have_page)
		t->page.release();
	if (t->have_next)
		t->next.release();
	t->page_bits.release();
	delete t;
}

void t30_get_status(T30 *t, struct t30_status *s)
{
	memset(s, 0, sizeof(*s));
	s->result = t->result;
	s->pages = t->pages;
	s->bps = rates[t->rate].bps;
	s->fine = t->fine;
	memcpy(s->remote_ident, t->remote_ident, sizeof(s->remote_ident));
	s->phase = state_name[t->state];
}

void t30_hangup(T30 *t)
{
	/* the sender may hang up as soon as its DCN is out, before the end
	   of it got here: the document is complete once its EOP had our
	   MCF */
	if (t->state != S_DONE)
		t->finish(!t->cfg.sending && t->eop_acked ? T30_OK :
		       T30_ERR_HANGUP);
}

const char *t30_result_text(enum t30_result r)
{
	switch (r) {
	case T30_RUNNING: return "in progress";
	case T30_OK: return "OK";
	case T30_ERR_NO_FAX: return "no fax machine";
	case T30_ERR_INCOMPATIBLE: return "incompatible";
	case T30_ERR_TRAINING: return "training failed";
	case T30_ERR_NO_RESPONSE: return "no response";
	case T30_ERR_PAGE: return "page not received";
	case T30_ERR_HANGUP: return "hung up";
	case T30_ERR_DOCUMENT: return "no document";
	case T30_ERR_FILE: return "cannot write the document";
	}
	return "?";
}

} // namespace dsp
