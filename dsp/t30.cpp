/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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
 *    With T.38 (t30_config.t38) the same machine runs on IFP packets
 *    instead of the modulations, as a T.38 terminal: each transmission
 *    is a no-signal indicator and 75 ms, the modulation's indicator and
 *    its training's time (V.21: a second of flags), then its data, paced
 *    as the modulation would carry it (HDLC frames whole, without FCS;
 *    T.4 data and the TCF's zeros), the last with its end, a tail, and
 *    no-signal; what comes in is taken the same way, a sig-end or
 *    no-signal for the end of the carrier.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "env.h"
#include "bits.h"
#include "fax.h"
#include "fax_hdlc.h"
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
#define ECM_RX_MAX      (ECM_SIZE + 8)  /* a received frame and its FCS */
#define ECM_PPRS        4       /* PPRs for a block before CTC */
#define ECM_RNRS        60      /* RNRs answered (T5: 60 s) */
/* the fast modulations, best first, with their DCS rate bits (11-14 as
   bit 0-3); for T.38 their data and training indicator (V.17: the short
   training, the long one is the next) and the training's time */
static const struct rate {
	enum fax_mod mod;
	s32 bps;
	u16 bits;
	s16 family;                     /* 2: V.17, 1: V.29, 0: V.27ter */
	s16 t38_data, t38_ind;
	s16 train_ms, long_train_ms;
} rates[] = {
	{ FM_V17_14400, 14400, 0x8, 2, T38_DATA_V17_14400,
	  T38_IND_V17_14400_SHORT, 142, 1393 },
	{ FM_V17_12000, 12000, 0xa, 2, T38_DATA_V17_12000,
	  T38_IND_V17_12000_SHORT, 142, 1393 },
	{ FM_V17_9600,  9600,  0x9, 2, T38_DATA_V17_9600,
	  T38_IND_V17_9600_SHORT, 142, 1393 },
	{ FM_V17_7200,  7200,  0xb, 2, T38_DATA_V17_7200,
	  T38_IND_V17_7200_SHORT, 142, 1393 },
	{ FM_V29_9600,  9600,  0x1, 1, T38_DATA_V29_9600,
	  T38_IND_V29_9600, 234, 234 },
	{ FM_V29_7200,  7200,  0x3, 1, T38_DATA_V29_7200,
	  T38_IND_V29_7200, 234, 234 },
	{ FM_V27_4800,  4800,  0x2, 0, T38_DATA_V27_4800,
	  T38_IND_V27_4800, 708, 708 },
	{ FM_V27_2400,  2400,  0x0, 0, T38_DATA_V27_2400,
	  T38_IND_V27_2400, 943, 943 },
};
#define NRATES ((s16)std::size(rates))

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

/* T.38: what is going out, and its steps */
enum t38_tx {
	T38_TX_NONE, T38_TX_V21, T38_TX_TCF, T38_TX_PAGE, T38_TX_ECM,
};
enum t38_step {
	T38_STEP_SILENCE,               /* no-signal, 75 ms */
	T38_STEP_TRAIN,                 /* the indicator, the training */
	T38_STEP_DATA,
	T38_STEP_TAIL,                  /* after the end, then no-signal */
};
#define T38_CHUNK_MAX   300             /* data octets in a packet */
#define T38_V21_MAX     7               /* V.21 ones (T.38 7.5) */

static const char *state_name[] = {
	"idle", "CED", "wait DCS", "receive TCF", "receive page",
	"wait post-page command", "wait DCN", "receive ECM block",
	"wait PPS", "CNG", "send TCF", "wait CFR", "send page", "wait MCF",
	"send ECM block", "wait PPS response", "wait CTR", "gap",
	"send frames", "done",
};

struct frame {
	unsigned char data[FRAME_MAX];
	s16 len;                        /* without the FCS */
};

/* HDLC: frames on V.21, ECM frames (FCD, RCP) on the fast modulation */
struct T30 : HdlcTx<T30, FRAME_MAX, HdlcV21>,
	     HdlcTx<T30, 4 + ECM_SIZE, HdlcEcm>,
	     HdlcRx<T30, RX_FRAME_MAX, HdlcV21>,
	     HdlcRx<T30, ECM_RX_MAX, HdlcEcm> {
	typedef HdlcTx<T30, FRAME_MAX, HdlcV21> V21Tx;
	typedef HdlcTx<T30, 4 + ECM_SIZE, HdlcEcm> EcmTx;
	typedef HdlcRx<T30, RX_FRAME_MAX, HdlcV21> V21Rx;
	typedef HdlcRx<T30, ECM_RX_MAX, HdlcEcm> EcmRx;

	struct t30_config cfg;
	s32 srate;
	enum state state, after_tx;     /* where to go when the frames went */
	enum t30_result result;
	s32 count;                      /* samples in this state */
	s32 timer;                      /* samples left, 0: none */
	s32 t1;                         /* samples to T1, 0: off */
	s16 tries;

	/* the transmitter: frames, a modem or a tone */
	s16 tx_on;
	FaxTx tx;
	struct frame frames[MAX_FRAMES];
	s16 nframes, cur_frame;         /* the next one to send */
	s32 gap;                        /* S_GAP: samples left */
	void (T30::*after_gap)();
	s32 tcf_bits;                   /* to send */
	T4Bits page_bits;
	size_t page_pos;

	/* receivers: V.21 for frames, a fast one for TCF and pages */
	FaxRx v21;
	FaxRx fast;
	s16 fast_on;
	FaxRxMemory mem;
	unsigned char msg[MAX_FRAMES * 2][RX_FRAME_MAX];
	s16 msg_len[MAX_FRAMES * 2], nmsg;
	s16 msg_ready;

	/* the session */
	s16 remote_dis_ok;
	unsigned char dis[16];          /* the remote's DIS FIF */
	s16 dis_len;
	s16 rate;                       /* index in rates[] */
	s16 fine, mr, min_ms;
	char remote_ident[21];

	/* sending */
	T4Page page, next;
	s16 have_page, have_next;
	s16 page_tries;
	s16 last_post;                  /* MPS, EOM, EOP */
	s16 cng;                        /* S_CNG with the tone */
	s16 need_retrain;

	/* receiving */
	T4Page rx_page;
	T4Rx t4rx;
	s16 rx_page_on;
	s16 page_ok;
	s16 eop_acked;                  /* MCF to an EOP sent: all came */
	s32 zero_run, max_zero_run, tcf_trained;

	s16 pages;

	/* error correction mode */
	s16 ecm_on;                     /* this call has it */
	s16 ecm_size;                   /* octets in a frame: 256 or 64 */
	s16 ecm_page, ecm_block;        /* the counters of PPS (the pages of
					   the call, the blocks of the page) */
	s16 ecm_long_train;             /* after CTC */
	/* sending */
	std::vector<unsigned char> ecm_doc;     /* the page's coded data */
	size_t ecm_doc_pos;             /* where the block starts */
	s16 ecm_frames;                 /* in the block */
	s16 ecm_last;                   /* the block ends the page */
	unsigned char ecm_send[32];     /* frames to (re)send */
	s16 ppr_count, rnr_count;
	s16 etx_frame, etx_rcp;         /* the next frame to look at, RCPs */
	/* receiving */
	std::vector<unsigned char> ecm_rx;      /* the block's frames */
	s16 ecm_rx_len[ECM_FRAMES]; /* -1: not (yet) received */
	s16 ecm_rx_frames;              /* in the block, -1: not known */
	s16 ecm_rcp;
	/* non-ECM: our last post-page response, to the command post_fcf,
	   and the state it took us to (a repeated command gets it again) */
	s16 post_fcf, post_resp;
	enum state post_state;
	s16 page_heard;                 /* S_RX_PAGE: a training was found */
	s16 lose_fcf, tx_mute;          /* test: DSP_T30_LOSE */
	s32 lose_n;
	s16 last_resp;                  /* our last response, for RR */
	unsigned char last_resp_fif[32];
	s16 last_resp_len;
	enum state last_resp_next;

	/* T.38: the transmission going out as packets */
	s16 t38_kind;                   /* enum t38_tx */
	s16 t38_step;                   /* enum t38_step */
	s32 t38_wait;                   /* samples left in the step, -1:
					   it starts */
	s32 t38_acc;                    /* the data's time: bits x srate */
	s16 t38_due;                    /* octets the line had time for */
	s16 t38_debt;                   /* octets of time to go first (the
					   frame's FCS and a flag) */
	s16 t38_last_ind;               /* the indicator sent last */
	s16 t38_long_train;             /* V.17: the long training */
	unsigned char t38_frame[4 + ECM_SIZE], t38_next[4 + ECM_SIZE];
	s16 t38_frame_len, t38_frame_pos, t38_next_len; /* -1: none */
	size_t t38_t4_pos, t38_t4_len;  /* T.4 octets sent, in all */
	/* receiving */
	unsigned char t38_hdlc[RX_FRAME_MAX];
	s16 t38_hdlc_len;                 /* the V.21 frame so far */
	unsigned char t38_ecm[ECM_RX_MAX];
	s16 t38_ecm_len;                /* the ECM frame so far */
	s16 t38_heard;                  /* the fast modulation came */

	void set_state(enum state s);
	void start_timer(s32 ms);
	void stop_fast();
	void start_fast(s16 short_train);
	void restart_v21();
	void add_frame(s16 fcf, const unsigned char *fif, s16 len);
	void send_frames(enum state next_);
	void new_frames();
	void gap_then(s32 ms, void (T30::*f)());
	void finish(enum t30_result r);
	void send_dcn(enum t30_result r);
	void send_dis();
	void start_page_rx();
	void page_done();
	void tcf_done();
	void got_dcs(const unsigned char *fif, s16 len);
	void rx_message();
	void encode_page();
	void send_tcf();
	void send_dcs();
	void send_page();
	s16 load_next();
	void send_post_page();
	s16 choose();
	void next_page_or_end();
	void page_settings();
	void sender_message();
	s16 hdlc_tx_next(HdlcV21, unsigned char *buf, s16 max);
	void hdlc_rx_frame(HdlcV21, const unsigned char *d, s16 len,
			   bool whole);
	void on_timeout();
	void tx_done();
	/* error correction mode */
	s16 post_command();
	void start_ecm_page();
	void start_ecm_block();
	void send_ecm_burst();
	void send_pps();
	void send_ctc();
	void send_rr();
	s16 hdlc_tx_next(HdlcEcm, unsigned char *buf, s16 max);
	void start_ecm_rx();
	void ecm_block_end();
	void hdlc_rx_frame(HdlcEcm, const unsigned char *d, s16 len,
			   bool whole);
	void ecm_frame_in(const unsigned char *d, s16 len);
	void got_pps(const unsigned char *fif, s16 len);
	void ecm_respond(s16 fcf, const unsigned char *fif, s16 len,
			 enum state next_);
	enum state ecm_page_end(s16 fcf2);
	float tone(float freq);
	float tx_sample();
	void rx_sample(float x);
	void take_message();
	/* T.38 */
	void t38_start(s16 kind);
	void t38_ind(s16 ind);
	void t38_data(s16 type, const unsigned char *d, s16 n, s16 end);
	s16 t38_get_frame(unsigned char *buf);
	void t38_flush();
	void t38_tx_sample();
	void t38_sample();
	void t38_rx(const struct t38_ifp *p);
	void t38_frame_in(const unsigned char *d, s16 len);
	void t38_t4_in(const unsigned char *d, s32 n);
	void t38_signal_end();
};


/*
 *    bits of the facsimile information field
 */

/* set a bit, numbered from 1 as in T.30 Table 2 */
static void set_bit(unsigned char *fif, s16 bit)
{
	fif[(bit - 1) / 8] |= 1 << ((bit - 1) % 8);
}

/* a bit of a FIF of len octets; 0 beyond its end */
static s16 get_bit(const unsigned char *fif, s16 len, s16 bit)
{
	if ((bit - 1) / 8 >= len)
		return 0;
	return (fif[(bit - 1) / 8] >> ((bit - 1) % 8)) & 1;
}

/* the minimum scan line time a DIS asks for (bits 21-23), ms */
static s16 dis_min_ms(const unsigned char *fif, s16 len)
{
	static const s16 ms[8] = { 20, 40, 10, 10, 5, 40, 20, 0 };
	s16 c = get_bit(fif, len, 21) << 2 | get_bit(fif, len, 22) << 1 |
		get_bit(fif, len, 23);
	return ms[c];
}

/* ... and its code in a DCS */
static void dcs_min_ms(unsigned char *fif, s16 ms)
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
static s16 dis_rate_ok(const unsigned char *fif, s16 len, const struct rate *r)
{
	u16 v = get_bit(fif, len, 11) | get_bit(fif, len, 12) << 1 |
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
static s16 put_ident(unsigned char *d, const char *id)
{
	s16 k;
	s32 n = id ? static_cast<s32>(strlen(id)) : 0;
	if (n > 20)
		n = 20;
	for (k = 0; k < 20; k++)
		d[k] = k < n ? static_cast<unsigned char>(id[n - 1 - k]) : ' ';
	return 20;
}

/* a received CSI / TSI into id (21 octets): in reading order, the
   spaces around it and anything not printable out */
static void get_ident(char *id, const unsigned char *d, s16 len)
{
	s16 k, n = 0;
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
void T30::start_timer(s32 ms)
{
	timer = ms2samples(ms, srate);
}

static void page_bit(void *ctx, s16 bit);

/* the fast receiver off, its V.17 equalizer kept in mem */
void T30::stop_fast()
{
	if (fast_on && !cfg.t38) {
		fast.save(&mem);
		fast.release();
	}
	fast_on = 0;
}

/* the fast receiver on at the call's rate (short_train: V.17 short
   training, from what mem keeps); stays off when out of memory (T.38:
   none, its data comes in packets) */
void T30::start_fast(s16 short_train)
{
	stop_fast();
	t38_heard = 0;
	t38_ecm_len = 0;
	if (cfg.t38)
		fast_on = 1;
	else if (!fast.init(rates[rate].mod, short_train, srate,
			      page_bit, this, &mem))
		fast_on = 1;
}


/* a frame for the next message: address, control, fcf with our X bit
   and len octets of fif (cut to what FRAME_MAX leaves) */
void T30::add_frame(s16 fcf, const unsigned char *fif, s16 len)
{
	struct frame *f;
	if (nframes == MAX_FRAMES)
		return;
	f = &frames[nframes++];
	f->data[0] = HDLC_ADDR;
	f->data[1] = HDLC_CTRL;
	/* the lowest bit says who sent it (the one that got the DIS) */
	f->data[2] = static_cast<unsigned char>(fcf | (cfg.sending ? 1 : 0));
	if (len > FRAME_MAX - 3)
		len = FRAME_MAX - 3;
	if (len)
		memcpy(f->data + 3, fif, len);
	f->len = 3 + len;
}

/* the HDLC transmitter's next V.21 frame (FCS and flags are its); -1
   after the last */
s16 T30::hdlc_tx_next(HdlcV21, unsigned char *buf, s16 /* max */)
{
	struct frame *f;
	if (cur_frame >= nframes)
		return -1;
	f = &frames[cur_frame++];
	memcpy(buf, f->data, f->len);
	return f->len;
}

/* send the frames added; then on to state next */
void T30::send_frames(enum state next_)
{
	if (nframes)
		frames[nframes - 1].data[1] |= HDLC_FINAL;
	cur_frame = 0;
	if (cfg.t38) {
		t38_start(T38_TX_V21);
	} else {
		/* a second of flags first (T.30 5.3.1), three after the
		   last frame */
		V21Tx::hdlc_tx_start(300 / 8 * 8, 3 * 8);
		tx.init(FM_V21, 0, srate, V21Tx::hdlc_tx_cb, this);
	}
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
void T30::gap_then(s32 ms, void (T30::*f)())
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
	add_frame(FCF_DCN, nullptr, 0);
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
	s16 len = 3;

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
	s16 v17 = rates[rate].family == 2;
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
		for (s16 k = 0; k < ECM_FRAMES; k++)
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
static void page_bit(void *ctx, s16 bit)
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
		t->EcmRx::hdlc_rx_bit(bit);
	}
}

/* the response to the page: MCF if it came, RTN if not */
void T30::page_done()
{
	T4Page *p = &rx_page;
	t4rx.end();
	page_ok = p->rows > 100 && p->bad_rows * 20 < p->rows;
	DSP_DBG("t30: page of %" PRId32 " rows, %" PRId32 " bad: %s\n",
		p->rows, p->bad_rows, page_ok ? "ok" : "bad");
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
	s32 bps = rates[rate].bps;
	s16 ok = max_zero_run >= bps * 3 / 4;        /* 750 ms */
	DSP_DBG("t30: TCF: %" PRId32 " zeros in a row (of %" PRId32 "): %s\n",
		max_zero_run, bps * 3 / 2, ok ? "ok" : "failed");
	stop_fast();
	new_frames();
	add_frame(ok ? FCF_CFR : FCF_FTT, nullptr, 0);
	if (ok) {
		tries = 0;
		send_frames(S_RX_PAGE);      /* start_page_rx after */
	} else {
		send_frames(S_WAIT_DCS);
	}
}

/* a DCS: the rate and page settings the sender chose; the TCF follows
   (a DCS with a rate we don't know is ignored) */
void T30::got_dcs(const unsigned char *fif, s16 len)
{
	u16 v = get_bit(fif, len, 11) | get_bit(fif, len, 12) << 1 |
		     get_bit(fif, len, 13) << 2 | get_bit(fif, len, 14) << 3;
	s16 k;
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
	DSP_DBG("t30: DCS: %" PRId32 " bit/s, %s, %s%s\n", rates[k].bps,
		fine ? "fine" : "standard", mr ? "MR" : "MH",
		ecm_on ? ", ECM" : "");
	/* the training check follows on the fast modulation */
	zero_run = max_zero_run = tcf_trained = 0;
	zero(mem);
	start_fast(0);
	set_state(S_RX_TCF);
	start_timer(T4_MS + 1500);
}

/* a message came (its frames up to the final one): the remote's
   identity, DCN, the DIS for the sender, and the answering machine's
   handling of the sender's commands */
void T30::rx_message()
{
	s16 k, fcf = 0, last = nmsg - 1;
	const unsigned char *fif = nullptr;
	s16 fif_len = 0;

	/* the identity, then the command */
	for (k = 0; k < nmsg; k++) {
		s16 f = msg[k][2] & 0xfe;
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
			if (fif_len > static_cast<s16>(sizeof(dis)))
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
			u16 v = (fif[1] >> 2) & 0xf;
			for (k = 0; k < NRATES; k++)
				if (rates[k].bits == v) {
					rate = k;
					break;
				}
		}
		if (state == S_RX_ECM)
			ecm_block_end();
		ecm_long_train = 1;
		ecm_respond(FCF_CTR, nullptr, 0, S_RX_ECM);
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
		ecm_respond(FCF_ERR, nullptr, 0,
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
			add_frame(post_resp, nullptr, 0);
			send_frames(post_state == S_WAIT_DCS ? S_CED :
				    post_state);
			break;
		}
		if (state == S_RX_PAGE && rx_page_on) {
			/* the page ended without our seeing the carrier go */
			page_done();
		}
		new_frames();
		add_frame(page_ok ? FCF_MCF : FCF_RTN, nullptr, 0);
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
	s32 bps = rates[rate].bps;
	page_bits.release();
	page.encode_mh(&page_bits, min_ms * bps / 1000, 0, 1);
	page_pos = 0;
}

/* fast transmitter callback: the bits of the page */
static s16 page_get_bit(void *ctx)
{
	T30 *t = static_cast<T30*>(ctx);
	if (t->page_pos >= t->page_bits.n)
		return FAX_BIT_END;
	return t4_bits_get(&t->page_bits, t->page_pos++);
}

/* fast transmitter callback: the zeros of the TCF */
static s16 tcf_get_bit(void *ctx)
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
	if (cfg.t38)
		t38_start(T38_TX_TCF);
	else
		tx.init(rates[rate].mod, 0, srate, tcf_get_bit, this);
	tx_on = 1;
	set_state(S_TCF);
}

/* TSI and DCS: the rate, resolution, length and scan time chosen, and
   ECM; the TCF follows */
void T30::send_dcs()
{
	unsigned char fif[4] = { 0 }, id[20];
	u16 v = rates[rate].bits;
	s16 len = 3;

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
	DSP_DBG("t30: DCS: %" PRId32 " bit/s, %s, min scan %d ms%s\n",
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
	DSP_DBG("t30: page %d: %" PRId32 " rows, %zu bits\n", pages + 1,
		page.rows, page_bits.n);
	if (cfg.t38)
		t38_start(T38_TX_PAGE);
	else
		tx.init(rates[rate].mod, rates[rate].family == 2, srate,
			page_get_bit, this);
	tx_on = 1;
	set_state(S_PAGE);
}

/* the next page of the document into next; 1 if there is one */
s16 T30::load_next()
{
	if (have_next)
		return 1;
	if (cfg.next_page && cfg.next_page(cfg.ctx, &next) == 1)
		have_next = 1;
	return have_next;
}

/* what follows the page: EOP, MPS, or EOM (another resolution needs
   another DCS: phase B again) */
s16 T30::post_command()
{
	s16 more = load_next();
	return !more ? FCF_EOP :
		(next.fine != 0) != (fine != 0) ? FCF_EOM : FCF_MPS;
}

/* the command after a page (EOP, MPS or EOM), MCF and the rest wanted */
void T30::send_post_page()
{
	last_post = post_command();
	new_frames();
	add_frame(last_post, nullptr, 0);
	send_frames(S_WAIT_MCF);
}

/* the DIS came: what the call can do */
s16 T30::choose()
{
	s16 k;
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
		page = std::move(next);
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
		s32 y;
		s16 s = T4_STRIDE(page.width);
		p.init(page.width, 0);
		for (y = 0; y < page.rows; y += 2)
			p.add_row(page.bits.data() +
				  static_cast<size_t>(y) * s);
		page.release();
		page = std::move(p);
	}
}

/* the calling machine: a message came, what follows from it in each
   state */
void T30::sender_message()
{
	s16 fcf = msg[nmsg - 1][2] & 0xfe;

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
			s16 k;
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
				ecm_doc_pos += static_cast<size_t>(ecm_frames) *
					       ecm_size;
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
			s16 k, any = 0;
			const unsigned char *map = msg[nmsg - 1] + 3;
			s16 map_len = msg_len[nmsg - 1] - 3;
			tries = 0;
			rnr_count = 0;
			zero(ecm_send);
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
	DSP_DBG("t30: page %d: %" PRId32 " rows, %zu octets in ECM\n",
		pages + 1, page.rows, ecm_doc.size());
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
	ecm_frames = static_cast<s16>(n);
	ecm_last = ecm_doc_pos + n * ecm_size >= ecm_doc.size();
	zero(ecm_send);
	for (s16 k = 0; k < ecm_frames; k++)
		ecm_send[k / 8] |= 1 << (k % 8);
	ppr_count = 0;
	rnr_count = 0;
	send_ecm_burst();
}

/* the HDLC transmitter's next ECM frame into buf (its FCS and flags are
   the transmitter's): an FCD still to send, else RCP three times (T.4
   A.1); -1 after them */
s16 T30::hdlc_tx_next(HdlcEcm, unsigned char *buf, s16 /* max */)
{
	while (etx_frame < ecm_frames &&
	       !(ecm_send[etx_frame / 8] >> (etx_frame % 8) & 1))
		etx_frame++;
	if (etx_frame < ecm_frames) {
		size_t at =
			ecm_doc_pos + static_cast<size_t>(etx_frame) * ecm_size;
		size_t n = at < ecm_doc.size() ? ecm_doc.size() - at : 0;
		if (n > static_cast<size_t>(ecm_size))
			n = ecm_size;
		buf[0] = HDLC_ADDR;
		buf[1] = HDLC_CTRL;
		buf[2] = FCF_FCD;
		buf[3] = static_cast<unsigned char>(etx_frame);
		/* the last frame padded to the size, as most receivers
		   expect */
		memset(buf + 4, 0, ecm_size);
		if (n)
			memcpy(buf + 4, &ecm_doc[at], n);
		etx_frame++;
		return 4 + ecm_size;
	}
	if (etx_rcp < 3) {
		buf[0] = HDLC_ADDR;
		buf[1] = HDLC_CTRL;
		buf[2] = FCF_RCP;
		etx_rcp++;
		return 3;
	}
	return -1;
}

/* the frames of ecm_send, on the page's modulation (V.17: the short
   training, but after CTC) */
void T30::send_ecm_burst()
{
	s16 v17 = rates[rate].family == 2;
	etx_frame = etx_rcp = 0;
	if (cfg.t38) {
		t38_start(T38_TX_ECM);
	} else {
		/* 200 ms of flags and one more first, three after the last
		   frame */
		EcmTx::hdlc_tx_start(rates[rate].bps / 5 / 8 * 8 + 8, 3 * 8);
		tx.init(rates[rate].mod, v17 && !ecm_long_train, srate,
			EcmTx::hdlc_tx_cb, this);
	}
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
	fif[0] = ecm_last ? static_cast<unsigned char>(last_post |
						       (cfg.sending ? 1 : 0)) :
			    FCF_NULL;
	fif[1] = static_cast<unsigned char>(ecm_page);
	fif[2] = static_cast<unsigned char>(ecm_block);
	fif[3] = static_cast<unsigned char>(ecm_frames - 1);
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
	fif[1] = static_cast<unsigned char>(rates[rate].bits << 2);
	new_frames();
	add_frame(FCF_CTC, fif, 2);
	send_frames(S_WAIT_CTR);
}

/* RR: is the receiver ready now (after its RNR) */
void T30::send_rr()
{
	new_frames();
	add_frame(FCF_RR, nullptr, 0);
	send_frames(S_WAIT_PPS_RESP);
}


/*
 *    error correction mode: receiving
 */

/* a block of ECM frames coming on the fast modulation (V.17: the short
   training, but after CTC) */
void T30::start_ecm_rx()
{
	s16 v17 = rates[rate].family == 2;
	EcmRx::hdlc_rx_reset();
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
void T30::ecm_frame_in(const unsigned char *d, s16 len)
{
	if (d[0] != HDLC_ADDR || len < 3)
		return;
	if ((d[2] & 0xfe) == FCF_FCD && len >= 5) {
		s16 fn = d[3], n = len - 4;
		if (n > ECM_SIZE)
			n = ECM_SIZE;
		memcpy(&ecm_rx[static_cast<size_t>(fn) * ECM_SIZE], d + 4, n);
		ecm_rx_len[fn] = static_cast<s16>(n);
	} else if ((d[2] & 0xfe) == FCF_RCP) {
		if (!ecm_rcp++)
			DSP_DBG("t30: RCP\n");
	}
}

/* a flag from the ECM HDLC receiver: the frame before it, if whole
   octets with a good FCS */
void T30::hdlc_rx_frame(HdlcEcm, const unsigned char *d, s16 len,
			bool whole)
{
	if (whole && len >= 5 && hdlc_fcs(d, len) == HDLC_FCS_GOOD)
		ecm_frame_in(d, len - 2);
}

/* a response to the sender, remembered for its RR */
void T30::ecm_respond(s16 fcf, const unsigned char *fif, s16 len,
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
enum state T30::ecm_page_end(s16 fcf2)
{
	T4Page *p = &rx_page;
	if (rx_page_on) {
		t4rx.end();
		page_ok = p->rows > 0 && p->bad_rows * 20 < p->rows;
		DSP_DBG("t30: ECM page of %" PRId32 " rows, %" PRId32
			" bad: %s\n", p->rows, p->bad_rows,
			page_ok ? "ok" : "bad");
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
void T30::got_pps(const unsigned char *fif, s16 len)
{
	unsigned char map[32];
	s16 fcf2, page_no, block, n, k, missing = 0;

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
			ecm_respond(FCF_MCF, nullptr, 0, last_resp_next);
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
	zero(map);
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
		const unsigned char *d =
			&ecm_rx[static_cast<size_t>(k) * ECM_SIZE];
		for (s16 i = 0; i < ecm_rx_len[k] * 8 && !t4rx.done; i++)
			t4rx.bit(d[i / 8] >> (i % 8) & 1);
	}
	for (k = 0; k < ECM_FRAMES; k++)
		ecm_rx_len[k] = -1;
	ecm_rx_frames = -1;
	ecm_block++;
	if (fcf2 == FCF_NULL) {
		ecm_respond(FCF_MCF, nullptr, 0, S_RX_ECM);
		return;
	}
	/* the frames came without errors: MCF, whatever the page */
	enum state next_ = ecm_page_end(fcf2);
	ecm_respond(FCF_MCF, nullptr, 0, next_);
}


/*
 *    the V.21 receiver: HDLC frames into messages
 */

/* a flag from the V.21 HDLC receiver: the frame before it into msg[]
   if whole octets (address, control, FCF and FCS at least) with a good
   FCS; the final bit makes the message ready */
void T30::hdlc_rx_frame(HdlcV21, const unsigned char *d, s16 len,
			bool whole)
{
	if (!whole || len < 5)
		return;
	if (nmsg >= MAX_FRAMES * 2)
		return;
	if (hdlc_fcs(d, len) != HDLC_FCS_GOOD || d[0] != HDLC_ADDR) {
		DSP_DBG("t30: frame of %d bytes with a bad FCS\n", len);
		return;
	}
	memcpy(msg[nmsg], d, len);
	msg_len[nmsg++] = len - 2;
	if (d[1] & HDLC_FINAL)
		msg_ready = 1;
}

/* a fresh V.21 receiver, no message pending */
void T30::restart_v21()
{
	if (!cfg.t38)
		v21.init(FM_V21, 0, srate, V21Rx::hdlc_rx_cb, this, nullptr);
	V21Rx::hdlc_rx_reset();
	nmsg = 0;
	msg_ready = 0;
	t38_hdlc_len = 0;
}


/*
 *    T.38: IFP packets instead of the modulations
 */

/* the transmission of kind on its way (its packets by t38_tx_sample()):
   kind's indicator and training; V.17 pages and ECM blocks with the
   short one (but after CTC), the TCF with the long one */
void T30::t38_start(s16 kind)
{
	t38_kind = kind;
	t38_step = T38_STEP_SILENCE;
	t38_wait = -1;
	t38_acc = 0;
	t38_due = 0;
	t38_debt = 0;
	t38_long_train = kind == T38_TX_TCF ||
		(kind == T38_TX_ECM && ecm_long_train);
	t38_frame_len = t38_next_len = -1;
	t38_frame_pos = 0;
	t38_t4_pos = 0;
	t38_t4_len = kind == T38_TX_TCF ? static_cast<size_t>(tcf_bits) / 8 :
		kind == T38_TX_PAGE ? (page_bits.n + 7) / 8 : 0;
}

/* an indicator, unless it is the last one sent (none while the
   transmission is lost for a test) */
void T30::t38_ind(s16 ind)
{
	struct t38_ifp p;
	if (ind == t38_last_ind)
		return;
	t38_last_ind = ind;
	zero(p);
	p.type = ind;
	if (!tx_mute && cfg.t38_tx)
		cfg.t38_tx(cfg.ctx, &p);
}

/* a data packet of the transmission's modulation: n octets as field
   type (none if n is 0), then end, a field without data (-1: none) */
void T30::t38_data(s16 type, const unsigned char *d, s16 n, s16 end)
{
	struct t38_ifp p;
	zero(p);
	p.data = 1;
	p.type = t38_kind == T38_TX_V21 ? T38_DATA_V21 : rates[rate].t38_data;
	if (n > 0) {
		p.field[p.nfields].type = type;
		p.field[p.nfields].data = d;
		p.field[p.nfields++].len = n;
	}
	if (end >= 0)
		p.field[p.nfields++].type = end;
	if (!tx_mute && cfg.t38_tx)
		cfg.t38_tx(cfg.ctx, &p);
}

/* the transmission's next HDLC frame into buf, its octets the T.38 way
   round: our V.21 frames, or the ECM block's (FCD, RCP); its length,
   -1 after the last */
s16 T30::t38_get_frame(unsigned char *buf)
{
	s16 n;
	if (t38_kind == T38_TX_V21) {
		if (cur_frame >= nframes)
			return -1;
		n = frames[cur_frame].len;
		memcpy(buf, frames[cur_frame++].data, n);
	} else {
		n = hdlc_tx_next(HdlcEcm(), buf, 4 + ECM_SIZE);
		if (n < 0)
			return -1;
	}
	t38_reverse(buf, buf, n);
	return n;
}

/* the data the line has had time for (t38_due octets), in packets of
   the modulation's size; the last with its end, then the tail */
void T30::t38_flush()
{
	unsigned char buf[T38_CHUNK_MAX];
	s16 max = t38_kind == T38_TX_V21 ? T38_V21_MAX :
		cfg.t38_max_data > 0 && cfg.t38_max_data < T38_CHUNK_MAX ?
		cfg.t38_max_data : T38_CHUNK_MAX;

	while (tx_on && t38_step == T38_STEP_DATA && t38_due > 0) {
		s16 n = t38_due < max ? t38_due : max;
		if (t38_kind == T38_TX_V21 || t38_kind == T38_TX_ECM) {
			s16 left;
			if (t38_frame_len < 0) {
				t38_frame_len = t38_get_frame(t38_frame);
				t38_frame_pos = 0;
				if (t38_frame_len < 0) {
					/* (no frame at all) */
					t38_data(0, nullptr, 0, T38_HDLC_SIG_END);
					t38_step = T38_STEP_TAIL;
					t38_wait = ms2samples(100, srate);
					break;
				}
			}
			left = t38_frame_len - t38_frame_pos;
			if (n < left) {
				t38_data(T38_HDLC_DATA, t38_frame + t38_frame_pos,
					 n, -1);
				t38_frame_pos += n;
				t38_due -= n;
				continue;
			}
			/* the rest of the frame: is it the last? */
			t38_next_len = t38_get_frame(t38_next);
			t38_data(T38_HDLC_DATA, t38_frame + t38_frame_pos, left,
				 t38_next_len >= 0 ? T38_HDLC_FCS_OK :
				 T38_HDLC_FCS_OK_SIG_END);
			t38_due -= left;
			t38_debt = 3;           /* its FCS and a flag */
			if (t38_next_len < 0) {
				t38_step = T38_STEP_TAIL;
				t38_wait = ms2samples(100, srate);
				break;
			}
			memcpy(t38_frame, t38_next, t38_next_len);
			t38_frame_len = t38_next_len;
			t38_frame_pos = 0;
		} else {
			/* T.4: the TCF's zeros, or the page's octets */
			size_t left = t38_t4_len - t38_t4_pos;
			s16 last = static_cast<size_t>(n) >= left, k;
			if (last)
				n = static_cast<s16>(left);
			memset(buf, 0, n);
			if (t38_kind == T38_TX_PAGE)
				for (k = 0; k < n * 8; k++) {
					size_t b = (t38_t4_pos + k / 8) * 8 + k % 8;
					if (b < page_bits.n &&
					    t4_bits_get(&page_bits, b))
						buf[k / 8] |= 0x80 >> (k % 8);
				}
			t38_data(last ? T38_T4_SIG_END : T38_T4_DATA, buf, n,
				 -1);
			t38_t4_pos += n;
			t38_due -= n;
			if (last) {
				t38_step = T38_STEP_TAIL;
				t38_wait = ms2samples(60, srate);
				break;
			}
		}
	}
	if (t38_step != T38_STEP_DATA)
		t38_due = 0;
}

/* a sample's time of the transmission: its steps, and in its data the
   octets the modulation would have sent by now */
void T30::t38_tx_sample()
{
	const struct rate *r = &rates[rate];
	s16 fast = t38_kind != T38_TX_V21;

	if (t38_step == T38_STEP_DATA) {
		t38_acc += fast ? r->bps : 300;
		if (t38_acc >= 8L * srate) {
			t38_acc -= 8L * srate;
			if (t38_debt)
				t38_debt--;
			else
				t38_due++;
			if (t38_due >= T38_CHUNK_MAX)
				t38_flush();
		}
		return;
	}
	if (t38_step == T38_STEP_SILENCE && t38_wait < 0) {
		t38_ind(T38_IND_NO_SIGNAL);
		t38_wait = ms2samples(75, srate);
	}
	if (--t38_wait > 0)
		return;
	switch (t38_step) {
	case T38_STEP_SILENCE:
		/* the indicator, and the training's time (HDLC: and its
		   flags': V.21 a second, ECM 200 ms) */
		t38_ind(!fast ? T38_IND_V21_PREAMBLE :
			r->t38_ind + (r->family == 2 && t38_long_train));
		t38_step = T38_STEP_TRAIN;
		t38_wait = ms2samples(!fast ? 1000 :
				      (t38_long_train ? r->long_train_ms :
				       r->train_ms) +
				      (t38_kind == T38_TX_ECM ? 200 : 0), srate);
		break;
	case T38_STEP_TRAIN:
		t38_step = T38_STEP_DATA;
		break;
	default:
		/* the tail is over */
		t38_ind(T38_IND_NO_SIGNAL);
		t38_kind = T38_TX_NONE;
		tx_done();
		break;
	}
}

/* a sample's time without a transmission: CED as its indicator (after
   200 ms of no-signal), and the end of a gap.  No CNG: the call is over
   on T.38 because it is a fax call, and a gateway that plays a CNG
   holds what comes after it until it hears the other machine's V.21
   (spandsp's: a DIS already on its way when the CNG came keeps our DCS
   back till the DIS after it, into which it goes) */
void T30::t38_sample()
{
	if (tx_on) {
		t38_tx_sample();
		return;
	}
	switch (state) {
	case S_CNG:
		if (cng && count == 0)
			t38_ind(T38_IND_NO_SIGNAL);
		break;
	case S_CED:
		if (count == 0)
			t38_ind(T38_IND_NO_SIGNAL);
		else if (count == ms2samples(ANS_SILENCE_MS, srate))
			t38_ind(T38_IND_CED);
		else if (count >= ms2samples(FAX_CED_END_MS, srate))
			send_dis();
		break;
	case S_GAP:
		if (--gap <= 0)
			(this->*after_gap)();
		break;
	default:
		break;
	}
}

/* a frame that came whole (the other end saw its FCS good) into msg[];
   the final one makes the message ready */
void T30::t38_frame_in(const unsigned char *d, s16 len)
{
	if (len < 3 || nmsg >= MAX_FRAMES * 2 || d[0] != HDLC_ADDR)
		return;
	memcpy(msg[nmsg], d, len);
	msg_len[nmsg++] = len;
	if (d[1] & HDLC_FINAL)
		msg_ready = 1;
}

/* T.4 octets (the first bit at the top): the TCF's longest run of zeros,
   or the page's bits up to its RTC */
void T30::t38_t4_in(const unsigned char *d, s32 n)
{
	for (s32 k = 0; k < n * 8; k++) {
		s16 bit = d[k / 8] >> (7 - k % 8) & 1;
		if (state == S_RX_TCF) {
			tcf_trained = 1;
			if (bit)
				zero_run = 0;
			else if (++zero_run > max_zero_run)
				max_zero_run = zero_run;
		} else if (state == S_RX_PAGE && rx_page_on) {
			page_heard = 1;
			t4rx.bit(bit);
			if (t4rx.done) {
				/* RTC: the rest is no page */
				page_done();
				return;
			}
		} else {
			return;
		}
	}
	if (state == S_RX_PAGE)
		start_timer(T2_MS);     /* (a page may take minutes) */
}

/* the carrier went (a sig-end, or no-signal): the TCF, page or ECM block
   is over, and a message without its final frame ends */
void T30::t38_signal_end()
{
	if (state == S_RX_TCF && tcf_trained)
		tcf_done();
	else if (state == S_RX_PAGE && rx_page_on && page_heard)
		page_done();
	else if (state == S_RX_ECM && t38_heard)
		ecm_block_end();
	if (nmsg)
		take_message();
}

/* an IFP packet: indicators for the carrier, V.21 frames into messages,
   the fast modulation's data to the TCF, the page or the ECM block */
void T30::t38_rx(const struct t38_ifp *p)
{
	s16 k, end = 0;

	if (!p->data) {
		if (p->type == T38_IND_NO_SIGNAL) {
			t38_signal_end();
		} else if (p->type == T38_IND_V21_PREAMBLE) {
			t38_hdlc_len = 0;
		} else if (p->type >= T38_IND_V27_2400 &&
			   p->type <= T38_IND_V17_14400_LONG) {
			t38_heard = 1;
			if (state == S_RX_PAGE)
				page_heard = 1;
		}
		return;
	}
	if (p->type == T38_DATA_V21) {
		for (k = 0; k < p->nfields; k++) {
			const struct t38_field *f = &p->field[k];
			switch (f->type) {
			case T38_HDLC_DATA:
				if (t38_hdlc_len + f->len <= RX_FRAME_MAX) {
					t38_reverse(t38_hdlc + t38_hdlc_len, f->data,
						    f->len);
					t38_hdlc_len += f->len;
				}
				break;
			case T38_HDLC_FCS_OK:
			case T38_HDLC_FCS_OK_SIG_END:
				t38_frame_in(t38_hdlc, t38_hdlc_len);
				t38_hdlc_len = 0;
				end |= f->type == T38_HDLC_FCS_OK_SIG_END;
				break;
			case T38_HDLC_FCS_BAD:
			case T38_HDLC_FCS_BAD_SIG_END:
			case T38_HDLC_SIG_END:
				DSP_DBG("t30: T.38 frame: %s\n",
					f->type == T38_HDLC_SIG_END ? "end" :
					"bad FCS");
				t38_hdlc_len = 0;
				end |= f->type != T38_HDLC_FCS_BAD;
				break;
			default:
				break;
			}
		}
		if (msg_ready || (end && nmsg))
			take_message();
		return;
	}
	/* the fast modulation */
	t38_heard = 1;
	for (k = 0; k < p->nfields && state != S_DONE; k++) {
		const struct t38_field *f = &p->field[k];
		switch (f->type) {
		case T38_T4_DATA:
		case T38_T4_SIG_END:
			t38_t4_in(f->data, f->len);
			if (f->type == T38_T4_SIG_END)
				t38_signal_end();
			break;
		case T38_HDLC_DATA:
			if (state == S_RX_ECM &&
			    t38_ecm_len + f->len <= ECM_RX_MAX) {
				t38_reverse(t38_ecm + t38_ecm_len, f->data,
					    f->len);
				t38_ecm_len += f->len;
				start_timer(T2_MS);
			}
			break;
		case T38_HDLC_FCS_OK:
		case T38_HDLC_FCS_OK_SIG_END:
			if (state == S_RX_ECM)
				ecm_frame_in(t38_ecm, t38_ecm_len);
			t38_ecm_len = 0;
			if (f->type == T38_HDLC_FCS_OK_SIG_END)
				t38_signal_end();
			break;
		case T38_HDLC_FCS_BAD:
		case T38_HDLC_FCS_BAD_SIG_END:
		case T38_HDLC_SIG_END:
			t38_ecm_len = 0;
			if (f->type != T38_HDLC_FCS_BAD)
				t38_signal_end();
			break;
		default:
			break;
		}
	}
	/* RCP: the block is over */
	if (state == S_RX_ECM && ecm_rcp)
		ecm_block_end();
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
		sinf(2 * static_cast<float>(M_PI) * freq * count / srate);
}

/* the next output sample: the transmitter, else CNG, CED or silence;
   the end of a gap starts what follows it */
float T30::tx_sample()
{
	float y = 0;
	s32 c = count;

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
		y = fax_ced_sample(c, srate);
		if (c >= ms2samples(FAX_CED_END_MS, srate))
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
	/* a whole message (or what came before the carrier went) */
	if (msg_ready || (nmsg && !v21.carrier && V21Rx::hdlc_rx_idle()))
		take_message();
}

/* a message came (its frames up to the final one, or those before the
   carrier went): to the answering machine's handling, or the calling
   one's */
void T30::take_message()
{
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

/* n samples of 16 bit audio in and out (in NULL: silence, out NULL:
   none wanted), the timers by them */
void t30_process(T30 *t, const s16 *in, s16 *out, s32 n)
{
	s32 i;
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

/* n samples' time of T.38: what goes out by then, and the timers */
void t30_t38_time(T30 *t, s32 n)
{
	s32 i;
	for (i = 0; i < n && t->state != S_DONE; i++) {
		t->t38_sample();
		t->count++;
		if (t->timer && --t->timer == 0)
			t->on_timeout();
		if (t->t1 && --t->t1 == 0 && t->state == S_CNG)
			t->finish(T30_ERR_NO_FAX);
	}
	if (t->state != S_DONE)
		t->t38_flush();
}

/* a T.38 packet that came */
void t30_t38_rx(T30 *t, const struct t38_ifp *p)
{
	if (t->state != S_DONE)
		t->t38_rx(p);
}

/* a fax machine for one call; a sender with no first page ends at once
   (T30_ERR_DOCUMENT).  NULL when out of memory */
T30 *t30_create(const struct t30_config *cfg)
{
	T30 *t = new (std::nothrow) T30();
	if (!t)
		return nullptr;
	t->cfg = *cfg;
	t->srate = cfg->srate ? cfg->srate : 8000;
	t->t38_last_ind = -1;
	if (!t->cfg.max_bps)
		t->cfg.max_bps = 14400;
	t->result = T30_RUNNING;
	/* test: DSP_T30_LOSE=<fcf in hex>[:<n>], see send_frames() */
	if (const char *v = env("DSP_T30_LOSE")) {
		char *e;
		t->lose_fcf = static_cast<int>(strtol(v, &e, 16)) &
			0xfe;
		t->lose_n = *e == ':' ? atoi(e + 1) : 1;
		if (t->lose_n < 1)
			t->lose_fcf = 0;
	}
	if (!cfg->sending && cfg->ecm)
		t->ecm_rx.assign(static_cast<size_t>(ECM_FRAMES) * ECM_SIZE, 0);
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
	zero(*s);
	s->result = t->result;
	s->done = t->state == S_DONE;
	/* sending: the MCF to our EOP made it OK (send_dcn()); receiving:
	   our MCF to its EOP (t30_hangup() and the T2 of S_WAIT_DCN end it
	   OK) */
	s->complete = t->cfg.sending ? t->result == T30_OK :
		t->eop_acked && (t->result == T30_RUNNING ||
				 t->result == T30_OK);
	s->pages = t->pages;
	s->bps = rates[t->rate].bps;
	s->fine = t->fine;
	s->ecm = t->ecm_on;
	copy_array(s->remote_ident, t->remote_ident);
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
