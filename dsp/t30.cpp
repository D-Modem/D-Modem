/*
 *
 *    t30.cpp - a group 3 fax machine (ITU-T T.30, with or without error
 *            correction mode) on the fax modulations of fax_modems.cpp
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
 *    In error correction mode (T.30 Annex A, T.4 Annex A) the page goes in
 *    blocks of up to 256 HDLC frames (FCD) on the fast modulation, each
 *    block followed by RCP and then PPS on V.21: MCF if all frames came,
 *    else PPR with a map of those to send again.
 *
 */

#include <vector>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "dsp.h"
#include "bits.h"
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
	/* error correction mode */
	FCF_PPS = 0xbe, FCF_PPR = 0xbc, FCF_RR = 0x6e, FCF_RNR = 0xec,
	FCF_CTC = 0x12, FCF_CTR = 0xc4, FCF_EOR = 0xce, FCF_ERR = 0x1c,
	FCF_NULL = 0x00,
	/* on the fast modulation (T.4 Annex A) */
	FCF_FCD = 0x06, FCF_RCP = 0x86,
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

/* error correction mode */
#define ECM_FRAMES      256     /* in a block at most */
#define ECM_SIZE        256     /* octets in our frames */
#define ECM_PPRS        4       /* PPRs for a block before CTC */
#define ECM_RNRS        60      /* RNRs answered (T5: 60 s) */
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
	S_RX_ECM,               /* a block of ECM frames */
	S_WAIT_PPS,             /* the command after the block */
	/* calling (sending) */
	S_CNG,                  /* waiting for the DIS */
	S_TCF,                  /* sending it */
	S_WAIT_CFR,
	S_PAGE,
	S_WAIT_MCF,
	S_ECM_TX,               /* sending a block */
	S_WAIT_PPS_RESP,        /* MCF, PPR, RNR to our PPS */
	S_WAIT_CTR,
	/* both */
	S_GAP,                  /* silence before the next transmission */
	S_TX_FRAMES,
	S_DONE,
};

static const char *state_name[] = {
	"idle", "CED", "wait DCS", "receive TCF", "receive page",
	"wait post-page command", "wait DCN", "receive ECM block",
	"wait PPS", "CNG", "send TCF", "wait CFR", "send page", "wait MCF",
	"send ECM block", "wait PPS response", "wait CTR", "gap",
	"send frames", "done",
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

	/* error correction mode */
	int ecm_on;                     /* this call has it */
	int ecm_size;                   /* octets in a frame: 256 or 64 */
	int ecm_page, ecm_block;        /* the counters of PPS (the pages of
					   the call, the blocks of the page) */
	int ecm_long_train;             /* after CTC */
	/* sending */
	std::vector<unsigned char> ecm_doc;     /* the page's coded data */
	size_t ecm_doc_pos;             /* where the block starts */
	int ecm_frames;                 /* in the block */
	int ecm_last;                   /* the block ends the page */
	unsigned char ecm_send[32];     /* frames to (re)send */
	int ppr_count, rnr_count;
	int etx_frame, etx_rcp;         /* the next frame to look at, RCPs */
	unsigned char etx_buf[ECM_SIZE + 6];
	int etx_len, etx_pos, etx_bit, etx_ones, etx_flags, etx_closing;
	/* receiving */
	std::vector<unsigned char> ecm_rx;      /* the block's frames */
	short ecm_rx_len[ECM_FRAMES];   /* -1: not (yet) received */
	int ecm_rx_frames;              /* in the block, -1: not known */
	int ecm_rcp;
	unsigned char hd_buf[ECM_SIZE + 8];     /* a frame coming */
	int hd_len, hd_nbits, hd_ones, hd_bad;
	unsigned hd_byte;
	/* non-ECM: our last post-page response, to the command post_fcf,
	   and the state it took us to (a repeated command gets it again) */
	int post_fcf, post_resp;
	enum state post_state;
	int page_heard;                 /* S_RX_PAGE: a training was found */
	int lose_fcf, lose_n, tx_mute;  /* test: DSP_T30_LOSE */
	int last_resp;                  /* our last response, for RR */
	unsigned char last_resp_fif[32];
	int last_resp_len;
	enum state last_resp_next;

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
	/* error correction mode */
	int post_command();
	void start_ecm_page();
	void start_ecm_block();
	void send_ecm_burst();
	void send_pps();
	void send_ctc();
	void send_rr();
	void ecm_frame_out();
	void start_ecm_rx();
	void ecm_block_end();
	void ecm_hdlc_bit(int bit);
	void ecm_frame_in(const unsigned char *d, int len);
	void got_pps(const unsigned char *fif, int len);
	void ecm_respond(int fcf, const unsigned char *fif, int len,
			 enum state next_);
	enum state ecm_page_end(int fcf2);
	float tone(float freq);
	float tx_sample();
	void rx_sample(float x);
};


/*
 *    bits of the facsimile information field
 */

/* set a bit, numbered from 1 as in T.30 Table 2 */
static void set_bit(unsigned char *fif, int bit)
{
	fif[(bit - 1) / 8] |= 1 << ((bit - 1) % 8);
}

/* a bit of a FIF of len octets; 0 beyond its end */
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

/* a received CSI / TSI into id (21 octets): in reading order, the
   spaces around it and anything not printable out */
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

/* to state s: its samples counted from 0, the timer off */
void T30::set_state(enum state s)
{
	DSP_DBG("t30: %s -> %s\n", state_name[state], state_name[s]);
	state = s;
	count = 0;
	timer = 0;
}

/* on_timeout() in ms */
void T30::start_timer(int ms)
{
	timer = ms2samples(ms, srate);
}

static void page_bit(void *ctx, int bit);

/* the fast receiver off, its V.17 equalizer kept in mem */
void T30::stop_fast()
{
	if (fast_on) {
		fast.save(&mem);
		fast.release();
	}
	fast_on = 0;
}

/* the fast receiver on at the call's rate (short_train: V.17 short
   training, from what mem keeps); stays off when out of memory */
void T30::start_fast(int short_train)
{
	stop_fast();
	if (!fast.init(rates[rate].mod, short_train, srate,
			 page_bit, this, &mem))
		fast_on = 1;
}


/* a frame for the next message: address, control, fcf with our X bit
   and len octets of fif (cut to what FRAME_MAX leaves) */
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
		crc = hdlc_fcs(f->data, f->len) ^ 0xffff;
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
	/* test: the nth message of the frame type DSP_T30_LOSE names goes
	   out as silence (lost on the line), its timing kept */
	if (lose_fcf && (frames[nframes - 1].data[2] & 0xfe) == lose_fcf &&
	    --lose_n == 0) {
		DSP_DBG("t30: (test) this one lost\n");
		tx_mute = 1;
		lose_fcf = 0;
	}
}

/* an empty message to add frames to */
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

/* the end of the call, with result r unless one was set before */
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

/* CSI and DIS: we receive, on V.27ter, V.29 and V.17 as far as max_bps
   goes, fine, MR, any length, 0 ms scan time, ECM if configured */
void T30::send_dis()
{
	unsigned char fif[4] = { 0 }, id[20];
	int len = 3;

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
	if (cfg.ecm) {
		set_bit(fif, 24);               /* a fourth octet */
		set_bit(fif, 27);               /* error correction mode */
		len = 4;
	}
	new_frames();
	add_frame(FCF_CSI, id, put_ident(id, cfg.ident));
	add_frame(FCF_DIS, fif, len);
	send_frames(S_WAIT_DCS);
}

/* after our CFR or MCF: a new page coming (ECM: its first block) */
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
	if (ecm_on) {
		/* the blocks of the page */
		ecm_block = 0;
		ecm_rx_frames = -1;
		for (int k = 0; k < ECM_FRAMES; k++)
			ecm_rx_len[k] = -1;
		start_ecm_rx();
		return;
	}
	start_fast(v17);
	page_heard = 0;
	set_state(S_RX_PAGE);
	start_timer(T2_MS * 2);
}

/* fast receiver callback: the bits of the TCF (the longest run of
   zeros), of a page, or of an ECM block */
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
	} else if (t->state == S_RX_ECM) {
		t->ecm_hdlc_bit(bit);
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

/* the end of the TCF: CFR if half of it (750 ms) came as zeros in a
   row, else FTT */
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

/* a DCS: the rate and page settings the sender chose; the TCF follows
   (a DCS with a rate we don't know is ignored) */
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
	ecm_on = cfg.ecm && get_bit(fif, len, 27);
	ecm_size = get_bit(fif, len, 28) ? 64 : 256;
	ecm_long_train = 0;
	DSP_DBG("t30: DCS: %d bit/s, %s, %s%s\n", rates[k].bps,
		fine ? "fine" : "standard", mr ? "MR" : "MH",
		ecm_on ? ", ECM" : "");
	/* the training check follows on the fast modulation */
	zero_run = max_zero_run = tcf_trained = 0;
	memset(&mem, 0, sizeof(mem));
	start_fast(0);
	set_state(S_RX_TCF);
	start_timer(T4_MS + 1500);
}

/* a message came (its frames up to the final one): the remote's
   identity, DCN, the DIS for the sender, and the answering machine's
   handling of the sender's commands */
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
	fif_len = msg_len[last] > 3 ? msg_len[last] - 3 : 0;
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
	case FCF_PPS:
		if (state == S_RX_ECM)
			ecm_block_end();
		got_pps(fif, fif_len);
		break;
	case FCF_CTC:
		/* continue to correct, maybe at another rate: the frames
		   again after a long training */
		if (fif_len >= 2) {
			unsigned v = (fif[1] >> 2) & 0xf;
			for (k = 0; k < NRATES; k++)
				if (rates[k].bits == v) {
					rate = k;
					break;
				}
		}
		if (state == S_RX_ECM)
			ecm_block_end();
		ecm_long_train = 1;
		ecm_respond(FCF_CTR, NULL, 0, S_RX_ECM);
		break;
	case FCF_EOR:
		/* the sender gives up on the block: what came of it is
		   lost; the page ends if it says so */
		if (state == S_RX_ECM)
			ecm_block_end();
		for (k = 0; k < ECM_FRAMES; k++)
			ecm_rx_len[k] = -1;
		ecm_rx_frames = -1;
		ecm_block++;
		ecm_respond(FCF_ERR, NULL, 0,
			    fif_len >= 1 && (fif[0] & 0xfe) != FCF_NULL ?
			    ecm_page_end(fif[0] & 0xfe) : S_RX_ECM);
		break;
	case FCF_RR:
		/* the sender missed our answer */
		if (last_resp)
			ecm_respond(last_resp, last_resp_fif, last_resp_len,
				    last_resp_next);
		break;
	case FCF_MPS: case FCF_EOP: case FCF_EOM:
	case FCF_PRI_MPS: case FCF_PRI_EOP: case FCF_PRI_EOM:
		/* the same command again with nothing since our MCF (no
		   page's training found; the fast receiver's carrier detector
		   also takes the V.21 command for a carrier): the sender
		   missed our MCF, and gets it again, as spandsp does (not
		   RTN, which has it send the page we have once more) */
		if (post_resp == FCF_MCF && fcf == post_fcf &&
		    state == post_state &&
		    (state != S_RX_PAGE || !page_heard)) {
			DSP_DBG("t30: the post-page command again: our %02x "
				"again\n", post_resp);
			new_frames();
			add_frame(post_resp, NULL, 0);
			send_frames(post_state == S_WAIT_DCS ? S_CED :
				    post_state);
			break;
		}
		if (state == S_RX_PAGE && rx_page_on) {
			/* the page ended without our seeing the carrier go */
			page_done();
		}
		new_frames();
		add_frame(page_ok ? FCF_MCF : FCF_RTN, NULL, 0);
		post_fcf = fcf;
		post_resp = page_ok ? FCF_MCF : FCF_RTN;
		if (!page_ok) {
			post_state = S_WAIT_DCS;
			send_frames(S_WAIT_DCS);     /* it retrains */
		} else if (fcf == FCF_MPS || fcf == FCF_PRI_MPS) {
			post_state = S_RX_PAGE;
			send_frames(S_RX_PAGE);
		} else if (fcf == FCF_EOP || fcf == FCF_PRI_EOP) {
			eop_acked = 1;
			post_state = S_WAIT_DCN;
			send_frames(S_WAIT_DCN);
		} else {
			post_state = S_WAIT_DCS;     /* after DIS */
			send_frames(S_CED);          /* EOM: phase B again */
		}
		page_ok = 0;
		break;
	default:
		break;
	}
}


/*
 *    sending: the calling machine
 */

/* the page in MH for the line: rows filled to the minimum scan line
   time at the call's rate, RTC at the end */
void T30::encode_page()
{
	int bps = rates[rate].bps;
	page_bits.release();
	page.encode_mh(&page_bits, min_ms * bps / 1000, 0, 1);
	page_pos = 0;
}

/* fast transmitter callback: the bits of the page */
static int page_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	if (t->page_pos >= t->page_bits.n)
		return FAX_BIT_END;
	return t4_bits_get(&t->page_bits, t->page_pos++);
}

/* fast transmitter callback: the zeros of the TCF */
static int tcf_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	if (t->tcf_bits <= 0)
		return FAX_BIT_END;
	t->tcf_bits--;
	return 0;
}

/* the training check: 1.5 s of zeros at the chosen rate */
void T30::send_tcf()
{
	tcf_bits = rates[rate].bps * 3 / 2;       /* 1.5 s */
	tx.init(rates[rate].mod, 0, srate, tcf_get_bit, this);
	tx_on = 1;
	set_state(S_TCF);
}

/* TSI and DCS: the rate, resolution, length and scan time chosen, and
   ECM; the TCF follows */
void T30::send_dcs()
{
	unsigned char fif[4] = { 0 }, id[20];
	unsigned v = rates[rate].bits;
	int len = 3;

	set_bit(fif, 10);
	if (v & 1) set_bit(fif, 11);
	if (v & 2) set_bit(fif, 12);
	if (v & 4) set_bit(fif, 13);
	if (v & 8) set_bit(fif, 14);
	if (fine)
		set_bit(fif, 15);
	if (get_bit(dis, dis_len, 19))
		set_bit(fif, 19);               /* any length */
	/* with ECM the rows need no fill */
	dcs_min_ms(fif, ecm_on ? 0 : min_ms);
	if (ecm_on) {
		set_bit(fif, 24);
		set_bit(fif, 27);               /* bit 28 0: 256 octets */
		len = 4;
	}
	DSP_DBG("t30: DCS: %d bit/s, %s, min scan %d ms%s\n",
		rates[rate].bps, fine ? "fine" : "standard",
		ecm_on ? 0 : min_ms, ecm_on ? ", ECM" : "");
	new_frames();
	add_frame(FCF_TSI, id, put_ident(id, cfg.ident));
	add_frame(FCF_DCS, fif, len);
	send_frames(S_TCF);          /* the TCF after a gap */
}

/* the page on the fast modulation (V.17 with the short training) */
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

/* the next page of the document into next; 1 if there is one */
int T30::load_next()
{
	if (have_next)
		return 1;
	if (cfg.next_page && cfg.next_page(cfg.ctx, &next) == 1)
		have_next = 1;
	return have_next;
}

/* what follows the page: EOP, MPS, or EOM (another resolution needs
   another DCS: phase B again) */
int T30::post_command()
{
	int more = load_next();
	return !more ? FCF_EOP :
		(next.fine != 0) != (fine != 0) ? FCF_EOM : FCF_MPS;
}

/* the command after a page (EOP, MPS or EOM), MCF and the rest wanted */
void T30::send_post_page()
{
	last_post = post_command();
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
	ecm_on = cfg.ecm && get_bit(dis, dis_len, 27);
	ecm_size = ECM_SIZE;
	return 0;
}

/* the next page (if any) becomes the one to send */
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

/* the calling machine: a message came, what follows from it in each
   state */
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
			ecm_long_train = 0;
			gap_then(RESPONSE_GAP_MS, ecm_on ? &T30::start_ecm_page :
				 &T30::send_page);
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
	case S_WAIT_PPS_RESP:
		if (fcf == FCF_MCF) {
			tries = 0;
			rnr_count = 0;
			if (!ecm_last) {
				/* the next block of the page */
				ecm_doc_pos += (size_t)ecm_frames * ecm_size;
				ecm_block++;
				gap_then(RESPONSE_GAP_MS,
					 &T30::start_ecm_block);
				break;
			}
			/* the page is through */
			pages++;
			ecm_page++;
			page_tries = 0;
			next_page_or_end();
			if (last_post == FCF_EOP) {
				send_dcn(T30_OK);
			} else if (last_post == FCF_EOM) {
				cng = 0;
				set_state(S_CNG);
				start_timer(T1_MS);
			} else {
				page_settings();
				gap_then(RESPONSE_GAP_MS,
					 &T30::start_ecm_page);
			}
		} else if (fcf == FCF_PPR) {
			int k, any = 0;
			const unsigned char *map = msg[nmsg - 1] + 3;
			int map_len = msg_len[nmsg - 1] - 3;
			tries = 0;
			rnr_count = 0;
			memset(ecm_send, 0, sizeof(ecm_send));
			for (k = 0; k < ecm_frames && k / 8 < map_len; k++)
				if (map[k / 8] >> (k % 8) & 1) {
					ecm_send[k / 8] |= 1 << (k % 8);
					any = 1;
				}
			DSP_DBG("t30: PPR %d for block %d\n", ppr_count + 1,
				ecm_block);
			if (!any) {
				/* nothing it lacks: ask again */
				gap_then(RESPONSE_GAP_MS, &T30::send_pps);
			} else if (++ppr_count >= ECM_PPRS) {
				gap_then(RESPONSE_GAP_MS, &T30::send_ctc);
			} else {
				gap_then(RESPONSE_GAP_MS,
					 &T30::send_ecm_burst);
			}
		} else if (fcf == FCF_RNR) {
			/* not ready: ask again, for T5 */
			if (++rnr_count >= ECM_RNRS)
				send_dcn(T30_ERR_NO_RESPONSE);
			else
				gap_then(1000, &T30::send_rr);
		} else if (fcf == FCF_CRP) {
			gap_then(RESPONSE_GAP_MS, &T30::send_pps);
		} else if (fcf == FCF_RTN || fcf == FCF_PIN) {
			/* not an ECM receiver after all */
			send_dcn(T30_ERR_PAGE);
		}
		break;
	case S_WAIT_CTR:
		if (fcf == FCF_CTR) {
			tries = 0;
			ppr_count = 0;
			ecm_long_train = 1;
			gap_then(RESPONSE_GAP_MS, &T30::send_ecm_burst);
		}
		break;
	default:
		break;
	}
}


/*
 *    error correction mode: sending
 */

/* the page coded (MH, no fill: ECM has no minimum row time), its bits
   packed into octets in the order they go out, the first in the lowest
   bit (T.4 A.3.6.2), then the first block */
void T30::start_ecm_page()
{
	size_t k;
	page_bits.release();
	page.encode_mh(&page_bits, 0, 0, 1);
	ecm_doc.assign((page_bits.n + 7) / 8, 0);
	for (k = 0; k < page_bits.n; k++)
		if (t4_bits_get(&page_bits, k))
			ecm_doc[k / 8] |= 1 << (k % 8);
	page_bits.release();
	ecm_doc_pos = 0;
	ecm_block = 0;
	DSP_DBG("t30: page %d: %d rows, %zu octets in ECM\n", pages + 1,
		page.rows, ecm_doc.size());
	start_ecm_block();
}

/* the next block of the page from ecm_doc_pos: up to 256 frames, all
   of them to send */
void T30::start_ecm_block()
{
	size_t left = ecm_doc.size() - ecm_doc_pos;
	size_t n = (left + ecm_size - 1) / ecm_size;
	if (n == 0)
		n = 1;
	if (n > ECM_FRAMES)
		n = ECM_FRAMES;
	ecm_frames = (int)n;
	ecm_last = ecm_doc_pos + n * ecm_size >= ecm_doc.size();
	memset(ecm_send, 0, sizeof(ecm_send));
	for (int k = 0; k < ecm_frames; k++)
		ecm_send[k / 8] |= 1 << (k % 8);
	ppr_count = 0;
	rnr_count = 0;
	send_ecm_burst();
}

/* the next frame to send into etx_buf: an FCD still to send, else RCP
   three times (T.4 A.1); etx_len 0 when there is none */
void T30::ecm_frame_out()
{
	unsigned crc;
	etx_len = 0;
	while (etx_frame < ecm_frames &&
	       !(ecm_send[etx_frame / 8] >> (etx_frame % 8) & 1))
		etx_frame++;
	if (etx_frame < ecm_frames) {
		size_t at = ecm_doc_pos + (size_t)etx_frame * ecm_size;
		size_t n = at < ecm_doc.size() ? ecm_doc.size() - at : 0;
		if (n > (size_t)ecm_size)
			n = ecm_size;
		etx_buf[0] = HDLC_ADDR;
		etx_buf[1] = HDLC_CTRL;
		etx_buf[2] = FCF_FCD;
		etx_buf[3] = (unsigned char)etx_frame;
		/* the last frame padded to the size, as most receivers
		   expect */
		memset(etx_buf + 4, 0, ecm_size);
		if (n)
			memcpy(etx_buf + 4, &ecm_doc[at], n);
		etx_len = 4 + ecm_size;
		etx_frame++;
	} else if (etx_rcp < 3) {
		etx_buf[0] = HDLC_ADDR;
		etx_buf[1] = HDLC_CTRL;
		etx_buf[2] = FCF_RCP;
		etx_len = 3;
		etx_rcp++;
	} else {
		return;
	}
	crc = hdlc_fcs(etx_buf, etx_len) ^ 0xffff;
	etx_buf[etx_len++] = crc & 0xff;
	etx_buf[etx_len++] = crc >> 8;
}

/* HDLC on the fast modulation: flags, the frames with a flag between
   them, closing flags */
static int ecm_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	int bit;

	if (t->etx_ones == 5) {
		t->etx_ones = 0;
		return 0;               /* stuffed */
	}
	if (t->etx_flags > 0) {
		bit = (0x7e >> (7 - ((t->etx_flags - 1) & 7))) & 1;
		t->etx_flags--;
		t->etx_ones = 0;
		return bit;
	}
	if (t->etx_pos >= t->etx_len) {
		if (t->etx_closing)
			return FAX_BIT_END;
		t->ecm_frame_out();
		t->etx_pos = t->etx_bit = 0;
		if (!t->etx_len) {
			t->etx_closing = 1;
			t->etx_flags = 3 * 8;
		} else {
			t->etx_flags = 8;
		}
		return ecm_get_bit(ctx);
	}
	bit = (t->etx_buf[t->etx_pos] >> t->etx_bit) & 1;
	if (++t->etx_bit == 8) {
		t->etx_bit = 0;
		t->etx_pos++;
	}
	t->etx_ones = bit ? t->etx_ones + 1 : 0;
	return bit;
}

/* the frames of ecm_send, on the page's modulation (V.17: the short
   training, but after CTC) */
void T30::send_ecm_burst()
{
	int v17 = rates[rate].family == 2;
	etx_frame = etx_rcp = 0;
	etx_len = etx_pos = etx_bit = etx_ones = etx_closing = 0;
	/* 200 ms of flags first */
	etx_flags = rates[rate].bps / 5 / 8 * 8;
	tx.init(rates[rate].mod, v17 && !ecm_long_train, srate, ecm_get_bit,
		this);
	ecm_long_train = 0;
	tx_on = 1;
	set_state(S_ECM_TX);
	DSP_DBG("t30: ECM page %d block %d: %d frames%s\n", ecm_page,
		ecm_block, ecm_frames, ppr_count ? " (again)" : "");
}

/* PPS: the command after the block, the page and block counters, the
   frames in the block less one (T.30 A.4.3) */
void T30::send_pps()
{
	unsigned char fif[4];
	if (ecm_last)
		last_post = post_command();
	fif[0] = ecm_last ? (unsigned char)(last_post | (cfg.sending ? 1 : 0)) :
		FCF_NULL;
	fif[1] = (unsigned char)ecm_page;
	fif[2] = (unsigned char)ecm_block;
	fif[3] = (unsigned char)(ecm_frames - 1);
	new_frames();
	add_frame(FCF_PPS, fif, 4);
	send_frames(S_WAIT_PPS_RESP);
}

/* continue to correct, at the same rate: the frames still missing after
   CTR */
void T30::send_ctc()
{
	unsigned char fif[2];
	fif[0] = 0;
	fif[1] = (unsigned char)(rates[rate].bits << 2);
	new_frames();
	add_frame(FCF_CTC, fif, 2);
	send_frames(S_WAIT_CTR);
}

/* RR: is the receiver ready now (after its RNR) */
void T30::send_rr()
{
	new_frames();
	add_frame(FCF_RR, NULL, 0);
	send_frames(S_WAIT_PPS_RESP);
}


/*
 *    error correction mode: receiving
 */

/* a block of ECM frames coming on the fast modulation (V.17: the short
   training, but after CTC) */
void T30::start_ecm_rx()
{
	int v17 = rates[rate].family == 2;
	hd_len = hd_nbits = hd_ones = hd_bad = 0;
	hd_byte = 0;
	ecm_rcp = 0;
	start_fast(v17 && !ecm_long_train);
	ecm_long_train = 0;
	set_state(S_RX_ECM);
	start_timer(T2_MS * 2);
}

/* the block's carrier went (or RCP came): the command follows on V.21 */
void T30::ecm_block_end()
{
	stop_fast();
	set_state(S_WAIT_PPS);
	start_timer(T2_MS);
}

/* a frame off the fast modulation, FCS checked */
void T30::ecm_frame_in(const unsigned char *d, int len)
{
	if (d[0] != HDLC_ADDR || len < 3)
		return;
	if ((d[2] & 0xfe) == FCF_FCD && len >= 5) {
		int fn = d[3], n = len - 4;
		if (n > ECM_SIZE)
			n = ECM_SIZE;
		memcpy(&ecm_rx[(size_t)fn * ECM_SIZE], d + 4, n);
		ecm_rx_len[fn] = (short)n;
	} else if ((d[2] & 0xfe) == FCF_RCP) {
		if (!ecm_rcp++)
			DSP_DBG("t30: RCP\n");
	}
}

/* the bits of the fast modulation in ECM: HDLC, flags, stuffing */
void T30::ecm_hdlc_bit(int bit)
{
	if (bit) {
		if (++hd_ones > 6) {            /* idle or abort */
			hd_len = hd_nbits = 0;
			hd_bad = 0;
			return;
		}
	} else {
		if (hd_ones == 6) {             /* a flag: the frame ends */
			/* the flag's 0111111 went in already: a whole
			   number of octets before it */
			if (hd_nbits == 7 && !hd_bad && hd_len >= 5 &&
			    hdlc_fcs(hd_buf, hd_len) == HDLC_FCS_GOOD)
				ecm_frame_in(hd_buf, hd_len - 2);
			hd_len = hd_nbits = hd_ones = hd_bad = 0;
			hd_byte = 0;
			return;
		}
		if (hd_ones == 5) {             /* stuffed */
			hd_ones = 0;
			return;
		}
		hd_ones = 0;
	}
	hd_byte |= (unsigned)bit << hd_nbits;
	if (++hd_nbits == 8) {
		if (hd_len < (int)sizeof(hd_buf))
			hd_buf[hd_len++] = (unsigned char)hd_byte;
		else
			hd_bad = 1;
		hd_nbits = 0;
		hd_byte = 0;
	}
}

/* a response to the sender, remembered for its RR */
void T30::ecm_respond(int fcf, const unsigned char *fif, int len,
		      enum state next_)
{
	last_resp = fcf;
	last_resp_len = len;
	if (len)
		memcpy(last_resp_fif, fif, len);
	last_resp_next = next_;
	new_frames();
	add_frame(fcf, fif, len);
	send_frames(next_);
}

/* the page is complete (PPS or EOR with MPS, EOP, EOM): to the page's
   receiver; the state after our answer */
enum state T30::ecm_page_end(int fcf2)
{
	T4Page *p = &rx_page;
	if (rx_page_on) {
		t4rx.end();
		page_ok = p->rows > 0 && p->bad_rows * 20 < p->rows;
		DSP_DBG("t30: ECM page of %d rows, %d bad: %s\n", p->rows,
			p->bad_rows, page_ok ? "ok" : "bad");
		if (page_ok) {
			pages++;
			if (cfg.page_received)
				cfg.page_received(cfg.ctx, p);
		}
		t4rx.release();
		p->release();
		rx_page_on = 0;
	}
	ecm_page++;
	/* (a repeated PPS for the page's last block, after our MCF got
	   lost, is page - 1, block 0: see got_pps()) */
	ecm_block = 0;
	if (fcf2 == FCF_MPS || fcf2 == FCF_PRI_MPS)
		return S_RX_PAGE;
	if (fcf2 == FCF_EOP || fcf2 == FCF_PRI_EOP) {
		eop_acked = 1;
		return S_WAIT_DCN;
	}
	return S_CED;                   /* EOM: phase B again */
}

/* a PPS (T.30 A.4.3): PPR with the map of the frames missing, else MCF
   and the block into the page, which ends if the PPS says so */
void T30::got_pps(const unsigned char *fif, int len)
{
	unsigned char map[32];
	int fcf2, page_no, block, n, k, missing = 0;

	if (len < 4) {
		DSP_DBG("t30: PPS of %d octets\n", len);
		return;
	}
	fcf2 = fif[0] & 0xfe;
	page_no = fif[1];
	block = fif[2];
	n = fif[3] + 1;
	DSP_DBG("t30: PPS %02x page %d block %d, %d frames\n", fcf2, page_no,
		block, n);
	if (page_no != (ecm_page & 0xff) || block != (ecm_block & 0xff)) {
		/* the sender missed our MCF to the last block: again */
		if (last_resp == FCF_MCF &&
		    ((page_no == (ecm_page & 0xff) &&
		      block == ((ecm_block - 1) & 0xff)) ||
		     (page_no == ((ecm_page - 1) & 0xff) && ecm_block == 0))) {
			ecm_respond(FCF_MCF, NULL, 0, last_resp_next);
			return;
		}
		DSP_DBG("t30: PPS for page %d block %d, not %d/%d\n",
			page_no, block, ecm_page & 0xff, ecm_block & 0xff);
		send_dcn(T30_ERR_PAGE);
		return;
	}
	/* the frame count: the block's on its first PPS, maybe only the
	   burst's on the later ones */
	if (n > ecm_rx_frames)
		ecm_rx_frames = n;
	memset(map, 0, sizeof(map));
	for (k = 0; k < ecm_rx_frames; k++)
		if (ecm_rx_len[k] < 0) {
			map[k / 8] |= 1 << (k % 8);
			missing++;
		}
	if (missing) {
		DSP_DBG("t30: %d of %d frames missing: PPR\n", missing,
			ecm_rx_frames);
		ecm_respond(FCF_PPR, map, 32, S_RX_ECM);
		return;
	}
	/* the block to the page, bit by bit as on the line */
	for (k = 0; k < ecm_rx_frames && rx_page_on && !t4rx.done; k++) {
		const unsigned char *d = &ecm_rx[(size_t)k * ECM_SIZE];
		for (int i = 0; i < ecm_rx_len[k] * 8 && !t4rx.done; i++)
			t4rx.bit(d[i / 8] >> (i % 8) & 1);
	}
	for (k = 0; k < ECM_FRAMES; k++)
		ecm_rx_len[k] = -1;
	ecm_rx_frames = -1;
	ecm_block++;
	if (fcf2 == FCF_NULL) {
		ecm_respond(FCF_MCF, NULL, 0, S_RX_ECM);
		return;
	}
	/* the frames came without errors: MCF, whatever the page */
	enum state next_ = ecm_page_end(fcf2);
	ecm_respond(FCF_MCF, NULL, 0, next_);
}


/*
 *    the V.21 receiver: HDLC frames into messages
 */

/* a flag ended a V.21 frame: into msg[] if whole octets with a good
   FCS; the final bit makes the message ready */
void T30::frame_end()
{
	int n = rnbits - 7, k, len;
	unsigned char *d;

	rnbits = 0;
	/* address, control, FCF and FCS at least */
	if (n < 40 || n % 8 || n / 8 > RX_FRAME_MAX)
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
	if (hdlc_fcs(d, len) != HDLC_FCS_GOOD || d[0] != HDLC_ADDR) {
		DSP_DBG("t30: frame of %d bytes with a bad FCS\n", len);
		return;
	}
	msg_len[nmsg++] = len - 2;
	if (d[1] & HDLC_FINAL)
		msg_ready = 1;
}

/* V.21 receiver callback: HDLC flags, stuffed zeros and aborts */
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

/* a fresh V.21 receiver, no message pending */
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

/* the timer ran out: a command again, or the end of the call */
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
	case S_WAIT_PPS:
		send_dcn(T30_ERR_NO_RESPONSE);
		break;
	case S_RX_ECM:
		/* no block, or the end of one went by: the command may still
		   come */
		ecm_block_end();
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
	case S_WAIT_PPS_RESP:
		if (++tries >= TRIES)
			send_dcn(T30_ERR_NO_RESPONSE);
		else
			gap_then(0, rnr_count ? &T30::send_rr : &T30::send_pps);
		break;
	case S_WAIT_CTR:
		if (++tries >= TRIES)
			send_dcn(T30_ERR_NO_RESPONSE);
		else
			gap_then(0, &T30::send_ctc);
		break;
	default:
		break;
	}
}

/* the transmitter finished */
void T30::tx_done()
{
	tx_on = 0;
	tx_mute = 0;
	restart_v21();
	switch (state) {
	case S_TX_FRAMES:
		if (after_tx == S_DONE) {
			finish(result == T30_RUNNING ? T30_OK : result);
		} else if (after_tx == S_TCF) {
			gap_then(RESPONSE_GAP_MS, &T30::send_tcf);
		} else if (after_tx == S_RX_PAGE) {
			start_page_rx();
		} else if (after_tx == S_RX_ECM) {
			start_ecm_rx();
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
	case S_ECM_TX:
		gap_then(RESPONSE_GAP_MS, &T30::send_pps);
		break;
	default:
		break;
	}
}

/* a sample of a tone of freq Hz at the CED level, by the state's count */
float T30::tone(float freq)
{
	return FAX_CED_RMS * 1.41421f *
		sinf(2 * (float)M_PI * freq * count / srate);
}

/* the next output sample: the transmitter, else CNG, CED or silence;
   the end of a gap starts what follows it */
float T30::tx_sample()
{
	float y = 0;
	long c = count;

	if (tx_on) {
		y = tx_mute ? (tx.sample(), 0.0f) : tx.sample();
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

/* an input sample to the receivers (none while transmitting): the ends
   of the TCF, a page or an ECM block, and the messages that came */
void T30::rx_sample(float x)
{
	if (tx_on || state == S_DONE)
		return;
	v21.sample(x);
	if (fast_on) {
		fast.sample(x);
		if (state == S_RX_PAGE && fast.stage != FaxRx::RXS_SEARCH)
			page_heard = 1;
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
		} else if (state == S_RX_ECM &&
			   (ecm_rcp || (fast.trained &&
			    fast.carrier_run < -ms2samples(100, srate)))) {
			/* RCP: the block is over, before the receiver can
			   take the V.21 command after it for its signal
			   (what it keeps trains the next block) */
			ecm_block_end();
		} else if (state == S_RX_ECM && fast.trained) {
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

/* n samples of 16 bit audio in and out (in NULL: silence, out NULL:
   none wanted), the timers by them */
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

/* a fax machine for one call; a sender with no first page ends at once
   (T30_ERR_DOCUMENT).  NULL when out of memory */
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
	/* test: DSP_T30_LOSE=<fcf in hex>[:<n>], see send_frames() */
	if (getenv("DSP_T30_LOSE")) {
		char *e;
		t->lose_fcf = (int)strtol(getenv("DSP_T30_LOSE"), &e, 16) &
			0xfe;
		t->lose_n = *e == ':' ? atoi(e + 1) : 1;
		if (t->lose_n < 1)
			t->lose_fcf = 0;
	}
	if (!cfg->sending && cfg->ecm)
		t->ecm_rx.assign((size_t)ECM_FRAMES * ECM_SIZE, 0);
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

/* free the machine and the pages it holds */
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

/* how the call goes, into s */
void t30_get_status(T30 *t, struct t30_status *s)
{
	memset(s, 0, sizeof(*s));
	s->result = t->result;
	s->pages = t->pages;
	s->bps = rates[t->rate].bps;
	s->fine = t->fine;
	s->ecm = t->ecm_on;
	memcpy(s->remote_ident, t->remote_ident, sizeof(s->remote_ident));
	s->phase = state_name[t->state];
}

/* the far end hung up: the end of the call */
void t30_hangup(T30 *t)
{
	/* the sender may hang up as soon as its DCN is out, before the end
	   of it got here: the document is complete once its EOP had our
	   MCF */
	if (t->state != S_DONE)
		t->finish(!t->cfg.sending && t->eop_acked ? T30_OK :
		       T30_ERR_HANGUP);
}

/* a result for the log */
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
