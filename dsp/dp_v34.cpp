/*
 *
 *    dp_v34.cpp - ITU V.8 start-up and the V.34 data pump
 *
 *    The pump is started for the default mode (DP_V8) and for
 *    AT+MS=34/90/92.  It runs V.8 (ANSam, CM, JM, CJ) and, when both
 *    modems offer V.34, the V.34 start-up and data mode.  Otherwise it
 *    hands over to the V.32bis pump, which falls back to V.22bis.
 *
 *    V.34 start-up, as the original library runs it:
 *
 *    phase 2 (600 baud DPSK control channel, caller on 1200 Hz, answerer
 *    on 2400 Hz; tones A and B are the unmodulated carriers):
 *
 *    answerer: INFO0a, A, A-bar (on INFO0c), A (40 ms after B-bar)
 *              L1 (160 ms) L2 (until B), A (50 ms), A-bar (10 ms),
 *              silence, A (when the caller's L2 is measured), INFO1a
 *              (on INFO1c), silence
 *    caller:   INFO0c, B, B-bar (40 ms after A-bar, 10 ms), silence,
 *              B (when the answerer's L2 is measured), B-bar (40 ms
 *              after A-bar, 10 ms), L1 (160 ms), L2 (until A), B (two
 *              symbols), INFO1c, silence
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pump.h"
#include "v34.h"
#include "v90.h"
#include "v91.h"
#include "v92.h"

namespace dsp {

#define V34_SRATE       9600
#define CC_BAUD         600
#define CC_SPS          16      /* samples per control channel symbol */
/* the original library's tones, INFO sequences and L2: about 2990 rms */
#define V34_LEVEL       2988.0f
#define V34_TONE_AMP    (V34_LEVEL * 1.41421f)
#define ANSAM_AMP       3200.0f
#define PROBE_PERIOD    64      /* L1/L2 repeat at 150 Hz */
#define TRN3_LEN        5120    /* TRN symbols in phase 3 */
#define TRN4_LEN        5120    /* and in phase 4 (about 1.5 s, as the original) */
/* a reversal reaches the line this long after we decide on it (the
   pulse shaping), and we see the remote's reversal this long after it
   was on the line: answers that must follow 40 ms after a reversal
   wait the rest */
#define CC_TX_DELAY     (4 * CC_SPS)
#define CC_RX_DELAY     (13 * V34_SRATE / 1000)
#define REV_WAIT        (ms2samples(40, V34_SRATE) - CC_TX_DELAY - CC_RX_DELAY)

/* our CM / JM: V-series data call; V.34, V.32bis, V.22bis, V.23 (duplex)
   and V.21; LAPM, PSTN access (a JM: of the modulations, what both have) */
static const unsigned char v8_msg[] = { 0xe0, 0xc1, 0x45, 0x13, 0x94, 0x2a, 0x0d };
/* the trellis code we ask the remote to send: 0, 1, 2 for 16, 32, 64
   states */
#ifndef V34_TRELLIS
#define V34_TRELLIS 2
#endif

/* the least gain, in dB, for which we ask for precoding */
#ifndef V34_PRECODE_MIN
#define V34_PRECODE_MIN 1.0f
#endif

#define V8_MOD1_V34     0x40    /* in the first modulation octet */
#define V8_MOD1_PCM     0x20    /* a PCM modem availability octet follows */
#define V8_CAT_MOD      0x05    /* category tags (bits 0 to 4) */
#define V8_CAT_ACCESS   0x0D
#define V8_CAT_PCM      0x07
#define V8_ACCESS_DIGITAL 0x80
#define V8_PCM_ANALOGUE 0x20    /* V.90 / V.92 analogue modem */
#define V8_PCM_DIGITAL  0x40    /* V.90 / V.92 digital modem */
#define V8_PCM_V91      0x80
/* INFO0d: our phase 2 level (V34_LEVEL) is about -14 dBm0, the PCM
   constellations may go up to -12 dBm0 */
#define V90_P2_POWER    8       /* -6 - n dBm0 */
#define V90_MAX_POWER   23      /* -0.5 - n / 2 dBm0 */
#define V8_MOD2_V32     0x01    /* in the first extension octet */
#define V8_MOD2_V22     0x02
#define V8_MOD3_V23     0x04    /* in the second (duplex) */
#define V8_MOD3_V21     0x80
/* data mode mse (signal point spacing 2) that calls for a retrain */
#define V34_RETRAIN_MSE 0.25f
/* rate renegotiation (11.6/V.34): the mse a rate runs at with a margin
   (the probing's choice on a noisy line), the mse that calls for a
   lower rate, and the most the next higher rate may expect to have */
#define V34_RN_TARGET   0.12f
#define V34_RN_DOWN     0.2f
#define V34_RN_UP       0.08f

enum v34_state {
	/* V.8 answerer */
	V8A_SILENCE,
	V8A_ANSAM,
	V8A_JM,
	V8A_GAP,
	/* V.92 short phase 1, the digital modem answering (9.2.4/V.92) */
	QCA_QCA1D,      /* QCA1d on V.21(H) */
	QCA_GAP,        /* 75 ms */
	QCA_ANSPCM,     /* QTS, QTS-bar, ANSpcm until TONEq */
	QCA_TONEQ,      /* silence until TONEq ends */
	QCA_GAP2,       /* 75 ms, then phase 2 */
	/* V.8 caller */
	V8C_LISTEN,             /* is the answer tone ANSam? */
	V8C_WAIT,
	V8C_CM,
	V8C_CJ,
	V8C_GAP,
	/* phase 2 answerer */
	P2A_INFO0,
	P2A_A,
	P2A_ABAR,
	P2A_ABAR_40,
	P2A_A_10,
	P2A_L1,
	P2A_L2,
	P2A_A_50,
	P2A_ABAR_10,
	P2A_SILENCE,
	P2A_A2,
	P2A_INFO1,
	/* phase 2 caller */
	P2C_INFO0,
	P2C_B,
	P2C_B_40,
	P2C_BBAR_10,
	P2C_SILENCE,
	P2C_B2,
	P2C_B2_40,
	P2C_BBAR2_10,
	P2C_L1,
	P2C_L2,
	P2C_B3,
	P2C_INFO1,
	P2C_WAIT_INFO1,
	/* phases 3 and 4, answerer */
	P3A_SILENCE,
	P3A_S,
	P3A_SBAR,
	P3A_PP,
	P3A_TRN,
	P3A_J,
	P3A_WAIT,
	P4A_S,
	P4A_SBAR,
	P4A_TRN,
	/* phases 3 and 4, caller */
	P3C_WAIT,
	P3C_S,
	P3C_SBAR,
	P3C_PP,
	P3C_TRN,
	P3C_J,
	P4C_TRN,
	/* both */
	P4_MP,
	P4_E,
	V34_DATA,
	V34_RT_SILENCE, /* a retrain: silence, then phase 2 from tone A / B */
	/* a rate renegotiation: S, S-bar, TRN, then P4_MP and P4_E */
	V34_RN_S,
	V34_RN_SBAR,
	V34_RN_TRN,
	/* V.92 modem-on-hold (9.10/V.92): our response to the remote's MH
	   sequence (MHack, MHnack, MHcda), repeated */
	MOH_RESP,
	/* and ours: tone B, then MHreq until MHack or MHnack; then MHfrr
	   or MHcda after an MHnack */
	MOH_REQ,
	MOH_HELD,       /* the remote on hold: silence, then ANSam */
	/* V.90 digital modem, phase 3 */
	P3D_WAIT_JA,
	P3D_PAUSE,
	P3D_SD,         /* Sd, Sd-bar, TRN1d */
	P3D_JD,
	P3D_JDP,        /* Jd' (and DIL): until the remote's next S-bar */
	P3D_DIL_END,    /* the current DIL segment */
	P4D_START,
	P4D_RI,         /* until CPt */
	P4D_TRAIN,      /* TRN2d, MP, MP', Ed, B1d (the frame source) */
	/* V.91 (v91.cpp) from the end of V.8 on */
	V91_RUN,
	V92_RUN,        /* V.92 PCM upstream from phase 3 on */
};

enum v34_txm {
	TXM_SILENCE,
	TXM_ANSAM,
	TXM_FSK,
	TXM_CC,
	TXM_PROBE,
	TXM_PRIMARY,
};

struct BitQ {
	unsigned char b[256];
	int n, pos;

	void reset();
	void put(unsigned val, int nbits);
	void info_start();
	void info_end();
	void put_octet(unsigned char c);
	void make_cj();
	void make_mh(int sig, int info);
};

/* MH sequences (Table 32/V.92), bits 12:15 first bit first */
enum { MH_REQ = 0x3, MH_ACK = 0x5, MH_NACK = 0x7, MH_CLRD = 0x9,
       MH_CDA = 0xB, MH_FRR = 0xD };

/* control channel receiver: non-coherent DPSK at 600 baud */
#define CC_HIST         128

struct CcRx {
	Osc lo;
	cplx box[CC_SPS], old[CC_SPS];
	cplx acc;
	int pos;
	float phase_e[CC_SPS];
	int best;
	long n, last;           /* samples, at the last decision */
	float dmin;
	float rmin;             /* the most reversed step of a steady tone */
	int rmin_pos;           /* at this sampling phase */
	int snap;               /* set the timing at a tone reversal */
	int rbit;               /* a tone reversal in the last symbol */
	int ones;
	float p;                /* |b|^2 of the last symbol */
	int on, on_count;
	int zeros;              /* '0' decisions in a row (tone) */
	unsigned sr;            /* last bits, newest in bit 0 */
	long rev_cand;          /* a 1 after a tone: a reversal if 0 follows */
	int frame_len;          /* > 0: within an INFO frame */
	unsigned char frame[128];
	unsigned char hist[CC_HIST];
	long hist_n;
	/* the decisions at every sampling phase, for INFO frames whatever
	   the symbol timing does */
	unsigned char ph[CC_SPS][CC_HIST];
	long ph_n[CC_SPS];
	int ph_last;            /* the phase of the last sample */

	void init(float fc);
	int sample(float x);
};

struct Info0 {
	int rates;              /* bit 0..: 2743, 2800, 3429, 3000L, 3000H, 3200L, 3200H */
	int allow_3429;
	int power_red;
	int max_diff;
	int cme;
	int c1664;
	int clock;
	int ack;
	int v92, short2;        /* INFO0a (Table 16/V.92): V.92, and short
				   phase 2 asked for */

	void parse(const unsigned char *b);
};

struct Info1c {
	int power_red, add_power_red, md;
	struct { int high, preemph, max_rate; } r[V34_NBAUDS];
	int freq_offset;

	void parse(const unsigned char *b);
};

struct Info1a {
	int power_red, add_power_red, md;
	int high, preemph, max_rate;
	int baud_ac, baud_ca;   /* baud_ca 6: V.90 */
	int uinfo;              /* V.90: the Ucode of the 2 point train */
	int freq_offset;

	void parse(const unsigned char *b);
};

#define EC_NEAR_LEN     256     /* 27 ms */
#define EC_FAR_LEN      256     /* 27 ms around the round trip delay */
#define EC_HIST         16384   /* up to 1.7 s of round trip delay */
#define EC_SEARCH       1024    /* samples correlated for the far echo */
#define EC_TRACK_MU     0.0f

struct V34Echo {
	float hist[EC_HIST], wn[EC_NEAR_LEN], wf[EC_FAR_LEN];
	float rxh[EC_SEARCH];   /* received samples, for the delay search */
	float corr[EC_HIST], win[EC_HIST];      /* scratch of the search */
	int pos, rpos;
	int on, far_on, far_delay, trained;
	int judged, bypass;     /* the echo was measured; none: off */
	long n, since, filled, far_since;
	float mu, dbg_rx, dbg_err, dbg_tx, dbg_est;
	float pw_rx, pw_err;    /* in and out while training */
	float es;               /* the error's short term power */
	float pw_est;           /* the echo taken away */
	long dt;                /* samples frozen for the remote's signal */

	void init();
	void search();
	float run(float tx, float rx);
};

#define V34_FIFO        8

/* (PcmFrames: V.90's phase 4 frames, MP the sequence) */
struct V34Pump : PumpBase<V34Pump, v34_state>, PcmFrames<V34Pump> {
	static constexpr const char *name = "V.8/V.34 data pump";

	/* (frees what the receiver holds) */
	~V34Pump() { cleanup(); }

	/* (srate: of the pump, 9600) */
	int line_rate;          /* 9600, or 8000 through the converters */
	int v90;                /* offer V.90 as the digital modem */
	int pcm;                /* V.90 agreed in V.8 */
	int v91;                /* offer V.91 */
	int v91_on;             /* V.91 agreed in V.8 */
	int v8_low;             /* V.8 for this low speed modulation alone
				   (rt->v8_for), else back to it */
	int v8_dp;              /* the modulation agreed in V.8 when not
				   V.34 (0: none in common) */
	int v92;                /* offer V.92 as the digital modem */
	int v92_on;             /* both INFO0 said V.92 */
	int short2;             /* and both asked for short phase 2 */
	int short2_ok;          /* we ask for it (DSP_V92_SHORT2=0: not) */
	int qc_ok;              /* we answer QC1a (DSP_V92_QC=0: not) */
	V91 v91m;
	int pcmup_ok;           /* we offer PCM upstream (V.92) */
	int quiet_blocks;       /* phase 2: blocks without the remote's tone */
	int p2_gap;             /* waiting for INFO1a: 1 tone, 2 then a gap */
	int rt_info0;           /* retrain: 1 INFO0a in, 2 answered */
	V92Up v92u;
	int alaw;               /* the network's G.711 law */
	V90Tx v90tx;    /* the PCM transmitter */
	int pcm_tx;             /* our output from it */
	int sbar_seen;
	V90Cp cpt, cp;  /* the analogue modem's CPt and CP */
	int mp_h[6];            /* V.90: the precoder our last MP asked for */
	int up_n;               /* upstream rate, 2400 bit/s units */
	/* V.90 rate renegotiation (9.6/V.90) */
	int v90_rn_init;        /* the encoder starts on CPt again */
	int v90_rd_wait;        /* Rd once the remote's S-bar is in */
	int rn_sbar;            /* S-bar count at its S */
	int sil_cp;             /* CP count when our silence began */
	Resampler rs_up, rs_down;
	float fifo[V34_FIFO];   /* converted output, 8000 Hz */
	int fifo_n;
	long out_n;             /* samples sent on the 8000 Hz line */
	Resampler rs_ref;        /* V.90: our PCM signal at 9600 Hz */
	float ref_fifo[V34_FIFO];
	int ref_n;

	/* transmitter */
	enum v34_txm txm;
	Osc ansam, ansam_am;
	float ansam_sign;
	FskBitTx fsk;           /* V.8's V.21 */
	BitQ v8tx;
	int v8tx_repeat;        /* restart the message when it is done */
	QamTx cc;
	float cc_sym;           /* +-1, 0: silence */
	int cc_rev;             /* reverse at the next symbol */
	BitQ cctx;       /* INFO bits, a 1 reverses the phase */
	long cc_syms;
	float probe[PROBE_PERIOD];
	int probe_pos;
	float probe_gain;
	long probe_start;

	/* V.8 receiver */
	FskDemod v21;
	unsigned v8_sr;         /* bit window, newest in bit 19 */
	int v8_sync;
	int v8_bits;
	unsigned char v8_rx[32], v8_last[32];
	int v8_n, v8_last_n;
	int v8_ok;              /* two identical CM / JM */
	int v8_zeros;           /* zero octets in a row (CJ) */
	int v8_cj;
	unsigned char v8_remote[32];
	/* V.92 short phase 1 */
	int qc_n;               /* bits of a QC frame, -1: none */
	unsigned qc_frame;      /* bits 20:29, the first in bit 0 */
	int ev_qc;
	int qc_done;            /* QC1a answered (or tried) */
	int qc_lapm;
	int info0_again;        /* our INFO0 again, acknowledging the
				   remote's (its bit 28) */
	int info0_sent;         /* our INFO0s in this phase 2 */
	int info0_rep;          /* the remote's INFO0 again (since we last
				   looked) */
	int info0_rep_ack;      /* the last one's bit 28 */
	int info0_rec;          /* recovering: our INFO0s back to back */
	int info0_recovered;    /* (in this phase 2) */
	int info0_acked;        /* the remote's acknowledgement in that */
	long info0_tx_at;       /* when ours last went out */
	long info0_at;          /* when the remote's INFO0 last came */
	int lose_info0;         /* test: our first INFO0 spoilt */
	Goertzel toneq_g;       /* TONEq, 980 Hz */
	Goertzel toneq_space_g; /* V.21(L)'s space, 1180 Hz: CM, not TONEq */
	int toneq_blocks;
	int v8_remote_n;
	/* answer tone analysis (caller) */
	Osc ans_lo;
	cplx ans_lp1, ans_lp2;
	double env_sum, env15_re, env15_im;
	float ans_peak;          /* the answer tone's envelope at most */
	int ans_gone;            /* samples of it below a third of that */
	long env_n;
	/* V.32 / V.22 callers during ANSam */
	Goertzel aa_g;
	int aa_blocks;

	/* phase 2 receiver */
	CcRx ccr;
	int ev_info0, ev_info1, ev_rev;
	long rev_time;
	Info0 rinfo0;
	Info1c rinfo1c;
	Info1a rinfo1a;
	long rtd;               /* round trip delay, samples */
	long rev_sent;          /* when our reversal went out */
	/* the remote's L1/L2 */
	Goertzel probe_g[2];
	/* the remote's tone A or B during our own L2: exactly on a bin of
	   the 64 sample probe period, where the echo of L2 has nothing */
	Goertzel tone_g;
	int tone_blocks;
	float pblock_e;
	int probe_blocks;
	long l1_start;
	double pacc[PROBE_PERIOD];
	float pprev[PROBE_PERIOD];
	double ptotal, pdiff;
	long pcount;
	float snr_db;
	float tone_db[26];      /* received level of each 150 Hz tone */
	int rate_of_baud[V34_NBAUDS];   /* receivable rates (N), by probing */

	/* echo canceller (from phase 3 on) */
	V34Echo ec;
	float last_tx;

	/* phases 3, 4 and data */
	V34Tx ptx;
	V34Rx prx;
	int prx_on;

	/* outcome */
	int baud_tx, baud_rx;   /* symbol rate codes */
	int high_tx, high_rx;
	float precode_min;      /* dB: V34_PRECODE_MIN, or a test's */
	int info0_rates;        /* our INFO0's bits 12:19 (rates, carriers,
				   3429 allowed): all, or a test's */
	int preemph_tx;
	int max_rate_tx, max_rate_rx;
	int rate_tx, rate_rx;   /* data mode, bit/s */
	long lost;              /* data mode: samples without the signal */

	/* retrain: the remote's tone A / B in data mode */
	Goertzel rt_g, own_g;    /* the remote's tone, the echo of ours */
	float rt_e;
	int rt_blocks;
	int rt_heard;           /* the tone has been there a while */
	long bad;               /* data mode: samples with a poor mse */
	int retraining;         /* back to data mode without a new CONNECT */
	/* modem-on-hold (9.10/V.92) */
	int moh_t1;             /* the T1 code we grant (Table 33/V.92), 0:
				   we deny it (DSP_V92_MOH) */
	int ev_mh, mh_sig, mh_info;     /* an MH sequence came in */
	long mh_seen;           /* when */
	int moh_resp;           /* what we answer it with */
	int moh_drop;           /* MHcda after our MHnack: disconnect */
	long moh_end;           /* on hold (or reconnecting): disconnect then
				   unless phase 1 has come */
	int moh_req;            /* what we send of ours: MHreq, then MHfrr or
				   MHcda */
	int moh_req_hold;       /* seconds we keep the remote on hold */
	int moh_req_t1;         /* the T1 code of its MHack */
	long moh_req_at;        /* test: ours this far into data mode */
	long moh_req_n;         /* (samples of it so far) */
	unsigned moh_poll;      /* when to look for the DTE's request */
	int connected;          /* CONNECT has been reported */
	int retrains;
	long retrain_at;        /* test: our own retrain this far (samples) */
	/* rate renegotiation */
	int renegotiating;
	int renegs;
	int rn_max;             /* the most we may ask for (the probing's) */
	long rn_hi, rn_lo;      /* data mode: samples with an mse for a lower,
				   a higher rate */
	float mse_good;         /* data mode: the mse while it was fine */
	long reneg_at;          /* test: our own renegotiation this far */
	int reneg_to;           /* test: to this rate (N) */

	const char *tag() const { return "v34"; }
	void fs_reinit(V90Tx *t);
	const V90Cp *fs_data_cp();
	void fs_seq_next(int first);
	bool fs_acked();
	bool fs_silence();
	void fs_ed_end();
	void fs_data(unsigned char *b, int n);
	void set_state(enum v34_state s);
	void make_info0(BitQ *q);
	void make_info1c(BitQ *q);
	void make_info1a(BitQ *q);
	int info_expected();
	int v8_remote_cat(int tag);
	void v8_make_msg(BitQ *q);
	int v8_remote_v34();
	int v8_remote_mod(int k);
	int v8_choice();
	int v8_handover();
	int v8_back(const char *why);
	int v8_v90();
	int v8_v91();
	void v91_start();
	void v92_start();
	int v91_escape();
	void v8_rx_bit(int bit);
	void ans_analyse(float x);
	float ans_am_depth();
	void cc_tone();
	void cc_send();
	int cc_busy();
	void cc_silence();
	long rev_on_line();
	void probe_init();
	int probe_rx_sample(float x);
	void probe_collect(float x);
	int probe_analyse();
	void cc_rx_bit(int bit);
	void mh_check(CcRx *r);
	int mh_event();
	void moh_respond(int sig);
	void moh_hold(int secs);
	void moh_request(int hold_secs);
	void moh_req_test();
	void cc_phase_frame();
	void info_frame(CcRx *r);
	void rt_info0_frame(const unsigned char *h, long n);
	void info0_repeat_frame(const unsigned char *h, long n);
	bool info0_resend(long c);
	bool info0_more();
	void ec_schedule();
	void rt_tone(float x);
	void pump_rx_sample(float x);
	int fallback(const char *why);
	void choose_bauds();
	int primary_start();
	int v90_start();
	void v90_make_mp(int ack);
	int v90_data_setup();
	void tx_next(enum v34_state s, enum v34_txseg seg);
	int data_setup();
	void mp_start();
	void retrain(const char *why);
	int reneg_rate(int up);
	int rate_floor() const;
	void renegotiate(const char *why, int n);
	void v90_renegotiate(const char *why, int n, int remote);
	void v90_rd_start();
	/* the digital modem in V.90 phases 3 and 4 */
	int v90_digital() const
	{
		return state >= P3D_WAIT_JA && state <= P4D_TRAIN;
	}
	int handshake();
	float pump_tx_sample();
	int sample(float x, float *y, const float *ref);
	void start_probe();
	int start(struct modem *m, enum DP_ID id, int caller_, int srate_, int max_frag);
	void cleanup();
	int process(void *in, void *out, int cnt) override;
};

/* to state s: count (samples in the state) starts again from 0; the
   wait for INFO1a starts with no gap seen */
void V34Pump::set_state(enum v34_state s)
{
	PumpBase::set_state(s);
	if (s == P2C_WAIT_INFO1)
		p2_gap = 0;
}

/*
 *    bit queues and INFO sequences
 */

/* empty, from the first bit */
void BitQ::reset()
{
	n = pos = 0;
}

/* nbits of val, least significant first in time (what fits of them) */
void BitQ::put(unsigned val, int nbits)
{
	int i;
	for (i = 0; i < nbits && n < (int)sizeof(b); i++)
		b[n++] = (val >> i) & 1;
}

/* the fill bits, frame sync 01110010 (left first in time) */
void BitQ::info_start()
{
	reset();
	put(0xF, 4);
	put(0x4E, 8);     /* 01110010 sent from the left */
}

/* the CRC of the bits from 12 on (10.1.2.3.2/V.34), its most
   significant bit first, and the fill bits */
void BitQ::info_end()
{
	unsigned crc = v34_crc_bits(b + 12, n - 12, 0xFFFF);
	int i;
	for (i = 15; i >= 0; i--)
		put((crc >> i) & 1, 1);
	put(0xF, 4);
}

/* our INFO0 (Table 14/V.34): all symbol rates, 1664 point
   constellations, no power reduction; as the V.90 / V.92 digital modem
   INFO0d (Table 7/V.90, Table 15/V.92) with our PCM powers and law.
   The acknowledgement: ours again for the remote's INFO0 */
void V34Pump::make_info0(BitQ *q)
{
	q->info_start();
	/* 2743, 2800, 3429; 3000 low and high carrier, 3200 low and high
	   carrier; 3429 allowed */
	q->put(info0_rates, 8);
	q->put(0, 1);        /* power reduction */
	q->put(0, 3);        /* symbol rate difference */
	q->put(0, 1);        /* CME */
	q->put(1, 1);        /* 1664 point constellations */
	if (pcm) {
		q->put(v92 && short2_ok, 1);    /* short phase 2 (Table
						   15/V.92) */
		q->put(v92, 1);      /* V.92 */
	} else
		q->put(0, 2);        /* internal clock */
	q->put(info0_again, 1);      /* acknowledge (of INFO0a) */
	info0_tx_at = total;
	if (pcm) {
		/* INFO0d, Table 7/V.90 */
		q->put(V90_P2_POWER, 4);
		q->put(V90_MAX_POWER, 5);
		q->put(0, 1);        /* power at the terminals */
		q->put(alaw, 1);
		q->put(1, 1);        /* upstream at 3429 */
		q->put(0, 1);
	}
	q->info_end();
	/* test: the first one lost (its CRC's last bit wrong) */
	if (lose_info0) {
		lose_info0 = 0;
		q->b[q->n - 5] ^= 1;
	}
	info0_sent++;
}

/* our INFO1c (Table 15/V.34; INFO1d, Table 17/V.92): at each symbol
   rate the most the probing allows (none at 3429 when the remote cannot
   do it), low carrier, no pre-emphasis; no MD, no power reduction */
void V34Pump::make_info1c(BitQ *q)
{
	int i;
	q->info_start();
	q->put(0, 3);
	q->put(0, 3);
	q->put(0, 7);        /* no MD */
	for (i = 0; i < V34_NBAUDS; i++) {
		int rate = rate_of_baud[i];
		/* (3429 only if both modems have it) */
		if (!(rinfo0.rates & info0_rates & (1 << 2)) &&
		    i == V34_BAUD_3429)
			rate = 0;
		/* low carrier; for 3429, V.92's INFO1d (Table 17/V.92)
		   has bit 70 instead: PCM upstream on this channel */
		q->put(v92_on && pcmup_ok && i == V34_BAUD_3429, 1);
		q->put(0, 4);        /* no pre-emphasis */
		q->put(rate, 4);
	}
	q->put(0, 10);       /* frequency offset */
	q->info_end();
}

/* our INFO1a (Table 16/V.34), the answerer's: the symbol rates
   choose_bauds() picked, the caller's carrier and the most we receive
   (max_rate_rx), no pre-emphasis; no MD, no power reduction */
void V34Pump::make_info1a(BitQ *q)
{
	q->info_start();
	q->put(0, 3);
	q->put(0, 3);
	q->put(0, 7);        /* no MD */
	q->put(high_rx, 1);
	q->put(0, 4);        /* no pre-emphasis */
	q->put(max_rate_rx, 4);
	q->put(baud_tx, 3);       /* answerer -> caller */
	q->put(baud_rx, 3);       /* caller -> answerer */
	q->put(0, 10);
	q->info_end();
}

/* a field of an INFO frame, least significant bit first in time */
static int bits_val(const unsigned char *b, int first, int last)
{
	return field(b, first, last - first + 1);
}

/* b: the frame from bit 0 (fill); 1 when the CRC at crc_pos is good */
static int info_crc_ok(const unsigned char *b, int crc_pos)
{
	return bits_msb_are(b, crc_pos, v34_crc_bits(b + 12, crc_pos - 12,
						     0xFFFF), 16);
}

/* the remote's INFO0 (Table 14/V.34); bits 26 and 27 as INFO0a has
   them (Table 16/V.92): V.92, short phase 2 asked for */
void Info0::parse(const unsigned char *b)
{
	rates = bits_val(b, 12, 18);
	allow_3429 = b[19];
	power_red = b[20];
	max_diff = bits_val(b, 21, 23);
	cme = b[24];
	c1664 = b[25];
	clock = bits_val(b, 26, 27);
	ack = b[28];
	v92 = b[26];
	short2 = b[27];
}

/* the caller's INFO1c (Table 15/V.34): at each symbol rate its
   carrier, pre-emphasis and the most it receives (N, 2400 bit/s steps) */
void Info1c::parse(const unsigned char *b)
{
	int k;
	power_red = bits_val(b, 12, 14);
	add_power_red = bits_val(b, 15, 17);
	md = bits_val(b, 18, 24);
	for (k = 0; k < V34_NBAUDS; k++) {
		int o = 25 + 9 * k;
		r[k].high = b[o];
		r[k].preemph = bits_val(b, o + 1, o + 4);
		r[k].max_rate = bits_val(b, o + 5, o + 8);
	}
	freq_offset = bits_val(b, 79, 88);
}

/* the answerer's INFO1a (Table 16/V.34); with V.90 (code 6 for the
   digital modem's symbol rate, Table 10/V.90) bits 25:31 are UINFO */
void Info1a::parse(const unsigned char *b)
{
	power_red = bits_val(b, 12, 14);
	add_power_red = bits_val(b, 15, 17);
	md = bits_val(b, 18, 24);
	/* V.90/V.92 (bits 37:39 = 6): the analogue modem's carrier in bit
	   33, which V.92's short phase 2 sets (Table 19/V.92; Tables
	   10/V.90 and 18/V.92 have it 0) */
	high = bits_val(b, 37, 39) == 6 ? b[33] : b[25];
	preemph = bits_val(b, 26, 29);
	max_rate = bits_val(b, 30, 33);
	baud_ac = bits_val(b, 34, 36);
	baud_ca = bits_val(b, 37, 39);
	/* Table 10/V.90 */
	uinfo = baud_ca == 6 ? bits_val(b, 25, 31) : 0;
	freq_offset = bits_val(b, 40, 49);
}

/* the length of the INFO frame we expect next, up to the end of its CRC */
int V34Pump::info_expected()
{
	if (!ev_info0)
		return 29 + 16;
	return caller ? 50 + 16 : 89 + 16;
}

/*
 *    V.8
 */

/* a V.8 octet: start bit, the octet from its LSB, stop bit */
void BitQ::put_octet(unsigned char c)
{
	put(0, 1);
	put(c, 8);
	put(1, 1);
}

/* the remote's octet of a V.8 category (tag in the low 4 bits), or -1 */
int V34Pump::v8_remote_cat(int tag)
{
	int i;
	for (i = 1; i < v8_remote_n; i++)
		if ((v8_remote[i] & 0x1F) == tag)
			return v8_remote[i];
	return -1;
}

/* CM (caller) or JM (answerer); a JM repeats what both modems have.  As a
   V.90 digital modem: PCM in the modulation octet, the PCM modem
   availability category and a digital network connection. */
void V34Pump::v8_make_msg(BitQ *q)
{
	int i, k, pcm_ = (v90 ? V8_PCM_DIGITAL : 0) | (v91 ? V8_PCM_V91 : 0);
	q->reset();
	q->put(0x3FF, 10);
	/* in a JM what both have: V.90 when the CM had an analogue V.90
	   modem, V.91 when it had V.91 */
	if (!caller) {
		int c = v8_remote_cat(V8_CAT_PCM);
		if (c < 0)
			c = 0;
		if (!(c & V8_PCM_ANALOGUE))
			pcm_ &= ~V8_PCM_DIGITAL;
		if (!(c & V8_PCM_V91))
			pcm_ &= ~V8_PCM_V91;
	}
	for (i = 0, k = -1; i < (int)sizeof(v8_msg); i++) {
		int c = v8_msg[i];
		/* the modulation octets: in a JM what the CM has too */
		if ((c & 0x1F) == V8_CAT_MOD)
			k = 0;
		else if (k >= 0 && (c & 0x18) == 0x10)
			k++;
		else
			k = -1;
		if (!caller && k >= 0)
			c &= v8_remote_mod(k) | (k ? 0x18 : 0x1F);
		/* for a low speed modulation alone: only its bit */
		if (v8_low && k == 0)
			c &= ~V8_MOD1_V34;
		if (v8_low && k == 1)
			c = 0x10 | (v8_low == DP_V22 || v8_low == DP_V22BIS ?
				    V8_MOD2_V22 : 0);
		if (v8_low && k == 2)
			c = 0x10 | (v8_low == DP_V23 ? V8_MOD3_V23 :
				    v8_low == DP_V21 ? V8_MOD3_V21 : 0);
		if (pcm_ && (c & 0x1F) == V8_CAT_MOD)
			c |= V8_MOD1_PCM;
		if (pcm_ && (c & 0x1F) == V8_CAT_ACCESS)
			c |= V8_ACCESS_DIGITAL;
		q->put_octet(c);
	}
	if (pcm_)
		q->put_octet(V8_CAT_PCM | pcm_);
	v8tx_repeat = 1;
}

/* CJ (V.8): three zero octets, framed */
void BitQ::make_cj()
{
	reset();
	put_octet(0);
	put_octet(0);
	put_octet(0);
}

/* is V.34 in the remote's CM / JM? */
int V34Pump::v8_remote_v34()
{
	int c = v8_remote_cat(V8_CAT_MOD);
	return c >= 0 && (c & V8_MOD1_V34) != 0;
}

/* the remote's modulation octet k (0: the first, 1, 2: its extension
   octets), 0 when it has none */
int V34Pump::v8_remote_mod(int k)
{
	int i, j = -1;
	for (i = 1; i < v8_remote_n; i++) {
		int c = v8_remote[i];
		if ((c & 0x1F) == V8_CAT_MOD)
			j = 0;
		else if (j >= 0 && (c & 0x18) == 0x10)
			j++;
		else
			j = -1;
		if (j == k)
			return c;
	}
	return 0;
}

/* without V.34: the modulation of a JM (the answerer has chosen what
   both have), the best of it */
int V34Pump::v8_choice()
{
	int m2 = v8_remote_mod(1), m3 = v8_remote_mod(2);
	if (v8_low)
		return (v8_low == DP_V21 ? m3 & V8_MOD3_V21 :
			v8_low == DP_V23 ? m3 & V8_MOD3_V23 :
			m2 & V8_MOD2_V22) ? v8_low : 0;
	if (m2 & V8_MOD2_V32)
		return DP_V32BIS;
	if (m2 & V8_MOD2_V22)
		return DP_V22BIS;
	if (m3 & V8_MOD3_V23)
		return DP_V23;
	if (m3 & V8_MOD3_V21)
		return DP_V21;
	return 0;
}

/* the name of a low speed modulation V.8 agreed on, for the log */
static const char *v8_dp_name(int dp)
{
	return dp == DP_V32BIS ? "V.32bis" : dp == DP_V22BIS ? "V.22bis" :
		dp == DP_V22 ? "V.22" : dp == DP_V23 ? "V.23" : "V.21";
}

/* no V.34 after all (why): back to the low speed modulation V.8 ran for
   (v8_low), its pump without V.8; returns DPSTAT_CHANGEDP */
int V34Pump::v8_back(const char *why)
{
	DSP_DBG("v34: %s: back to %s without V.8\n", why,
		v8_dp_name(v8_low));
	if (rt)
		rt->v8_for = 0;
	return hand_over((enum DP_ID)v8_low, 0);
}

/* 75 ms after CJ (both sides, 8.1.2 and 8.2.3/V.8): to the pump of
   v8_dp, what V.8 agreed on without V.34 (the answer tone and V.8 done,
   no automode), which sends its sigC or sigA at once; returns
   DPSTAT_CHANGEDP */
int V34Pump::v8_handover()
{
	if (rt) {
		rt->v8_for = 0;
		/* (calling, only the FSK pumps look at it: the V.32bis and
		   V.22bis callers do not clear it for a later fallback) */
		rt->after_v8 = !caller || v8_dp == DP_V21 || v8_dp == DP_V23;
		if (!caller)
			rt->ans_sent = 1;
		else if (v8_dp == DP_V32BIS)
			rt->v32_automode = 1;
	}
	return hand_over((enum DP_ID)v8_dp, 0);
}

/* V.90 with us as the digital modem: both offer it */
int V34Pump::v8_v90()
{
	int c = v8_remote_cat(V8_CAT_PCM);
	return v90 && c >= 0 && (c & V8_PCM_ANALOGUE) != 0;
}

/* V.91: both offer it (and the remote is not an analogue V.90 modem) */
int V34Pump::v8_v91()
{
	int c = v8_remote_cat(V8_CAT_PCM);
	return v91 && !v8_v90() && c >= 0 && (c & V8_PCM_V91) != 0;
}

/* one bit of the V.21 receiver (V.8 at 300 bit/s): QC1a frames
   (ev_qc, qc_frame), CM / JM (two identical in a row: v8_ok,
   v8_remote) and CJ (v8_cj) */
void V34Pump::v8_rx_bit(int bit)
{
	v8_sr = (v8_sr >> 1) | ((unsigned)bit << 19);
	/* V.92's QC1a before CM (Table 2/V.92): ten ones, 0101010101, then
	   a frame of ten bits, twice */
	if (qc_n >= 0 && ++qc_n == 10) {
		unsigned f = v8_sr >> 10;
		qc_n = -1;
		if (dsplibs_debug_level >= 6)
			DSP_DBG("v34: QC frame %03x\n", f);
		if ((f & 7) == 0 && (f >> 9) == 1) {
			/* sent twice; the first may come before the
			   receiver has the carrier */
			if (!ev_qc) {
				ev_qc = 1;
				qc_lapm = (f >> 3) & 1;
				DSP_DBG("v34: V.92 QC%s%s, LAPM %d, UQTS code %d\n",
					f & 0x4 ? "A" : "", f & 0x2 ? "1d" : "1a",
					(f >> 3) & 1, (f >> 4) & 0x1F);
			}
			qc_frame = f;
		}
	}
	/* its sync, 0101010101, after the ten ones; of the first of the
	   two, the receiver may have only the last few ones yet (the
	   analogue modem starts QC1a right on ANSam's 1 s, and answers
	   only a QCA1d to the first well: the Agere modem went on with
	   CM when ours came after the second) */
	if ((v8_sr & 0xFFFC0) == 0xAABC0 && qc_n < 0) {
		if (dsplibs_debug_level >= 6)
			DSP_DBG("v34: QC sync at %ld ms (%s ones)\n",
				total * 1000 / srate,
				(v8_sr & 0x3F) == 0x3F ? "ten" : "fewer");
		qc_n = 0;
		return;
	}
	/* ten ones, then the CM/JM sync octet framed */
	if (v8_sr == 0xF03FF) {
		if (v8_sync && v8_n > 1 && !v8_ok) {
			if (v8_n == v8_last_n &&
			    !memcmp(v8_rx, v8_last, v8_n)) {
				v8_ok = 1;
				memcpy(v8_remote, v8_rx, v8_n);
				v8_remote_n = v8_n;
				DSP_DBG("v34: V.8 %s received, %d octets:"
					" %02x %02x %02x %02x %02x %02x %02x %02x"
					" %02x %02x\n", caller ? "JM" : "CM",
					v8_n, v8_rx[0], v8_rx[1],
					v8_rx[2], v8_rx[3], v8_rx[4],
					v8_rx[5], v8_rx[6], v8_rx[7],
					v8_rx[8], v8_rx[9]);
			}
			memcpy(v8_last, v8_rx, v8_n);
			v8_last_n = v8_n;
		}
		v8_sync = 1;
		v8_n = 0;
		v8_bits = 0;
		v8_rx[v8_n++] = 0xE0;
		return;
	}
	if (!v8_sync)
		return;
	if (++v8_bits >= 10 && (v8_sr & 0x80400) == 0x80000) {
		unsigned char c = (v8_sr >> 11) & 0xFF;
		v8_bits = 0;
		if (c == 0) {
			if (++v8_zeros >= 3)
				v8_cj = 1;
		} else
			v8_zeros = 0;
		if (v8_n < (int)sizeof(v8_rx))
			v8_rx[v8_n++] = c;
	} else if (v8_bits == 9 && v8_zeros == 2 && !(v8_sr & 0xFF800))
		/* CJ's third zero octet without its stop bit: the HSF
		   modem drops its carrier right after the last data bit */
		v8_cj = 1;
}

/* ANSam: 15 Hz amplitude modulation of the 2100 Hz answer tone */
void V34Pump::ans_analyse(float x)
{
	const float a = 1.0f / (0.004f * V34_SRATE);
	cplx z = cplx_scale(ans_lo.next_cplx(), x);
	float e, w;
	ans_lp1.re += a * (z.re - ans_lp1.re);
	ans_lp1.im += a * (z.im - ans_lp1.im);
	ans_lp2.re += a * (ans_lp1.re - ans_lp2.re);
	ans_lp2.im += a * (ans_lp1.im - ans_lp2.im);
	e = sqrtf(cplx_pow(ans_lp2));
	if (e > ans_peak)
		ans_peak = e;
	ans_gone = e < ans_peak / 3 ? ans_gone + 1 : 0;
	w = 2 * (float)M_PI * 15 * env_n / V34_SRATE;
	env_sum += e;
	env15_re += e * cosf(w);
	env15_im += e * sinf(w);
	env_n++;
}

/* the depth of the 15 Hz modulation, 0.2 for ANSam */
float V34Pump::ans_am_depth()
{
	if (env_sum <= 0)
		return 0;
	return 2 * sqrt(env15_re * env15_re + env15_im * env15_im) /
		env_sum;
}

/*
 *    control channel
 */

/* the control channel modulator's next symbol (for cc.sample()): the
   next INFO / MH bit (a 1 reverses the phase), else the tone, reversed
   once when cc_rev asks (rev_sent: then) */
static cplx cc_symbol(V34Pump *v)
{
	cplx z = { 0, 0 };
	if (v->cctx.pos < v->cctx.n) {
		if (v->cctx.b[v->cctx.pos++])
			v->cc_sym = -v->cc_sym;
	} else if (v->cc_rev) {
		v->cc_sym = -v->cc_sym;
		v->cc_rev = 0;
		v->rev_sent = v->total;
	}
	v->cc_syms++;
	z.re = v->cc_sym;
	return z;
}

/* start the tone (A or B), or keep it going */
void V34Pump::cc_tone()
{
	if (txm != TXM_CC || cc_sym == 0) {
		cc_sym = 1;
		txm = TXM_CC;
	}
}

/* the bits in cctx out from their first, on the tone */
void V34Pump::cc_send()
{
	cc_tone();
	cctx.pos = 0;
}

/* INFO / MH bits or a reversal still to go out */
int V34Pump::cc_busy()
{
	return cctx.pos < cctx.n || cc_rev;
}

/* zero symbols: the pulses in flight die away */
void V34Pump::cc_silence()
{
	cc_sym = 0;
}

/* our last reversal has been on the line for this long */
long V34Pump::rev_on_line()
{
	return cc_rev ? -1 : total - rev_sent - CC_TX_DELAY;
}

/* one 64 sample period of L1/L2 (10.1.2.4/V.34), at V34_LEVEL rms */
void V34Pump::probe_init()
{
	/* Table 17/V.34: 150 Hz multiples, without 900, 1200, 1800 and
	   2400 Hz (the control channel carriers and their neighbours) */
	static const short f[] = { 150, 300, 450, 600, 750, 1050, 1350, 1500,
				   1650, 1950, 2100, 2250, 2550, 2700, 2850,
				   3000, 3150, 3300, 3450, 3600, 3750 };
	static const char inv[] = { 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1,
				    0, 1, 1, 1, 1, 0, 0 };
	double e = 0;
	int i, k;
	for (i = 0; i < PROBE_PERIOD; i++) {
		double x = 0;
		for (k = 0; k < (int)sizeof(f) / (int)sizeof(f[0]); k++)
			x += (inv[k] ? -1 : 1) *
				cos(2 * M_PI * f[k] * i / V34_SRATE);
		probe[i] = x;
		e += x * x;
	}
	e = sqrt(e / PROBE_PERIOD);
	for (i = 0; i < PROBE_PERIOD; i++)
		probe[i] *= V34_LEVEL / e;
}

/* L1 from now, 6 dB up (the handshake takes it to L2's level after
   160 ms) */
void V34Pump::start_probe()
{
	txm = TXM_PROBE;
	probe_pos = 0;
	probe_gain = 2;      /* L1: 6 dB up */
	probe_start = total;
}

/* a receiver for the remote's carrier fc (1200 or 2400 Hz), all of it
   cleared */
void CcRx::init(float fc)
{
	memset(this, 0, sizeof(*this));
	lo.set_freq(-fc, V34_SRATE);
}

/* one sample; returns -1, or a decision (0/1) at a symbol centre */
int CcRx::sample(float x)
{
	cplx z = cplx_scale(lo.next_cplx(), x), b, d, o;
	int i = pos, bit = -1;
	acc = cplx_add(acc, cplx_sub(z, box[i]));
	box[i] = z;
	b = cplx_scale(acc, 1.0f / CC_SPS);
	o = old[i];
	d = cplx_mulc(b, o);
	old[i] = b;
	/* symbol timing: fast between frames (after the probing signals),
	   slow within them */
	/* the most reversed phase step of this symbol at any sampling
	   phase: a reversal of a tone is clear at one of them, wherever the
	   symbol timing is */
	if (cplx_pow(b) > 0 && d.re < dmin * cplx_pow(b))
		dmin = d.re / cplx_pow(b);
	if (snap && cplx_pow(b) > 300.0f * 300.0f / 4 &&
	    cplx_pow(o) > 300.0f * 300.0f / 4) {
		float c = d.re / sqrtf(cplx_pow(b) * cplx_pow(o));
		if (c < rmin) {
			rmin = c;
			rmin_pos = i;
		}
	}
	phase_e[i] += (frame_len > 0 ? 0.02f : 0.2f) *
		(sqrtf(cplx_pow(d)) - phase_e[i]);
	ph[i][ph_n[i]++ % CC_HIST] = cplx_pow(b) > 300.0f * 300.0f / 4 &&
		d.re < 0;
	ph_last = i;
	if (++pos == CC_SPS)
		pos = 0;
	if (i == best && n - last > CC_SPS / 2) {
		/* (once per symbol when the timing moves) */
		last = n;
		/* tone amplitude A gives |b| = A / 2 */
		p = cplx_pow(b);
		if (p > 300.0f * 300.0f / 4)
			on_count = on_count > 0 ? on_count + 1 : 1;
		else
			on_count = on_count < 0 ? on_count - 1 : -1;
		if (on_count >= 4)
			on = 1;
		else if (on_count <= -4)
			on = 0;
		if (p > 300.0f * 300.0f / 4)
			bit = d.re < 0;
		rbit = dmin < -0.5f;
		/* the remote's symbols end where its tone reverses most
		   clearly: the timing from there at once, outside a frame
		   (after INFO0, see cc_rx_bit).  The remote's symbol clock
		   stops while it sends L1/L2, so an INFO1 frame after them
		   comes at a new phase, and a retrain has no INFO0 to find
		   the timing with */
		if (snap && rmin < -0.8f && frame_len <= 0) {
			int e = (rmin_pos - best + CC_SPS + CC_SPS / 2) %
				CC_SPS - CC_SPS / 2;
			if (e > 2 || e < -2)
				best = rmin_pos;
		}
		dmin = 0;
		rmin = 0;
		/* follow the symbol timing, two samples per symbol at most:
		   a jump would lose or repeat a decision (on a tone all
		   phases look alike) */
		{
			int b = best, step;
			float lo_ = phase_e[0];
			for (i = 0; i < CC_SPS; i++) {
				if (phase_e[i] > phase_e[b] * 1.2f)
					b = i;
				if (phase_e[i] < lo_)
					lo_ = phase_e[i];
			}
			/* only where the phase steps show the timing: on a
			   tone all phases look alike, and the timing of the
			   frames before has to stay for its reversal */
			if (phase_e[b] < 1.5f * lo_)
				b = best;
			step = (b - best + CC_SPS + CC_SPS / 2) % CC_SPS -
				CC_SPS / 2;
			if (step > 2)
				step = 2;
			else if (step < -2)
				step = -2;
			best = (best + step + CC_SPS) % CC_SPS;
		}
	}
	n++;
	return bit;
}

/* the remote's L1/L2, once per sample: returns -1, or at the end of a
   block of one period whether two of the probe tones are clearly there
   (each carries 1/21 of the power) */
int V34Pump::probe_rx_sample(float x)
{
	float e;
	int on;
	pblock_e += x * x;
	probe_g[0].run(x);
	if (!probe_g[1].run(x))
		return -1;
	e = pblock_e / PROBE_PERIOD;
	pblock_e = 0;
	on = e > tone_power(300) && probe_g[0].power > e / 60 &&
		probe_g[1].power > e / 60;
	return on;
}

/* collect one sample of the remote's L2 */
void V34Pump::probe_collect(float x)
{
	int i = pcount % PROBE_PERIOD;
	pacc[i] += x;
	ptotal += (double)x * x;
	/* the noise from the differences of successive periods: a sample
	   clock offset does not count as noise then */
	if (pcount >= PROBE_PERIOD)
		pdiff += (double)(x - pprev[i]) * (x - pprev[i]);
	pprev[i] = x;
	pcount++;
}

/* the most N (2400 bit/s steps) at each symbol rate (Table 10/V.34) */
static const int v34_max_rate_of_baud[V34_NBAUDS] = { 9, 11, 11, 12, 13, 14 };

/* the least N at the symbol rate (Table 10/V.34): 2400 bit/s only at
   2400 baud, 4800 at the others */
static int v34_min_rate(int baud)
{
	int n;
	for (n = 1; n < 14 && !v34_rates[baud][n - 1].b; n++)
		;
	return n;
}

/* over the SNR a rate needs, for the receiver's own impairments */
#define PROBE_MARGIN 2	/* dB */

/* the remote's L2: the signal to noise ratio after equalization at
   each symbol rate, and the data rates (in 2400 bit/s steps) they
   allow; returns the best */
int V34Pump::probe_analyse()
{
	double periods = (double)pcount / PROBE_PERIOD, pt, pn, ptone[26];
	int i, k, n, b, best = 1;

	memset(rate_of_baud, 0, sizeof(rate_of_baud));
	if (periods < 4)
		return 1;
	pt = ptotal / pcount;
	pn = pdiff / (pcount - PROBE_PERIOD) / 2;
	if (pn < pt * 1e-6)
		pn = pt * 1e-6;
	snr_db = 10 * log10((pt - pn) / pn);
	for (k = 1; k <= 25; k++) {
		double re = 0, im = 0;
		for (i = 0; i < PROBE_PERIOD; i++) {
			double m = pacc[i] / periods;
			re += m * cos(2 * M_PI * k * i / PROBE_PERIOD);
			im += m * sin(2 * M_PI * k * i / PROBE_PERIOD);
		}
		ptone[k] = (re * re + im * im) * 2 / (PROBE_PERIOD * PROBE_PERIOD);
		tone_db[k] = 10 * log10(ptone[k] + 1e-3);
	}
	for (b = 0; b < V34_NBAUDS; b++) {
		float rs = v34_symbol_rate(b), fc = v34_carrier(b, 0);
		double inv = 0, snr;
		int cnt = 0;
		for (k = 1; k <= 25; k++) {
			float f = 150.0f * k;
			if (k == 6 || k == 8 || k == 12 || k == 16 ||
			    fabsf(f - fc) > 0.45f * rs)
				continue;
			/* the data signal's density over the noise density
			   (white up to half the sample rate) */
			inv += 1 / (ptone[k] * 21 * (V34_SRATE / 2) / (rs * pn) +
				    1e-9);
			cnt++;
		}
		if (!cnt)
			continue;
		/* what the receiver itself achieves: about 40 dB */
		snr = 10 * log10(1 / (inv / cnt + 1e-4));
		/* with the 16 state code the bit error rate stays below 1e-5
		   from 3 dB per bit per symbol + 4 dB, at every symbol rate;
		   plus a margin for the receiver's own impairments */
		for (n = v34_max_rate_of_baud[b];
		     n > 1 && snr < 3 * 2400.0 * n / rs + 4 + PROBE_MARGIN; n--)
			;
		/* not even the symbol rate's least: none there (2400
		   baud stays the last resort) */
		if (n < v34_min_rate(b))
			n = 0;
		rate_of_baud[b] = n;
		DSP_DBG("v34: probe %d baud: SNR %.1f dB -> %d bit/s\n",
			(int)(rs + 0.5f), snr, n * 2400);
		if (n > best)
			best = n;
	}
	DSP_DBG("v34: probe SNR %.1f dB\n", snr_db);
	return best;
}

/*
 *    receiver
 */

/* an INFO frame in r->frame (fill, sync, CRC good) */
void V34Pump::info_frame(CcRx *r)
{
	int len = info_expected();

	/* no reversals within the frame */
	if (ev_rev && total - rev_time <
	    (long)len * CC_SPS)
		ev_rev = 0;
	r->zeros = 0;
	r->rev_cand = 0;
	r->hist_n = 0;
	memset(r->ph_n, 0, sizeof(r->ph_n));
	if (!ev_info0) {
		rinfo0.parse(r->frame);
		ev_info0 = 1;
		info0_at = total;
		v92_on = pcm && v92 && rinfo0.v92;
		short2 = v92_on && rinfo0.short2 &&
			short2_ok && !retraining;
		if (pcm)
			DSP_DBG("v34: INFO0a V.92 %d, short "
				"phase 2 %d%s\n", rinfo0.v92,
				rinfo0.short2,
				v92_on ? ": V.92" : "");
		r->snap = 1;
		DSP_DBG("v34: INFO0 rates %02x 3429 %d "
			"diff %d\n", rinfo0.rates,
			rinfo0.allow_3429,
			rinfo0.max_diff);
	} else {
		if (caller)
			rinfo1a.parse(r->frame);
		else {
			int b;
			rinfo1c.parse(r->frame);
			for (b = 0; b < V34_NBAUDS; b++)
				DSP_DBG("v34: INFO1c baud %d high %d "
					"preemph %d rate %d\n", b,
					rinfo1c.r[b].high,
					rinfo1c.r[b].preemph,
					rinfo1c.r[b].max_rate);
		}
		ev_info1 = 1;
		DSP_DBG("v34: INFO1 received\n");
	}
}

/* INFO0a again during a retrain (the remote goes to phase 2 with it:
   V.92's retrains, 11.5.1.1/V.34): the last INFO0 length of bits of a
   history */
void V34Pump::rt_info0_frame(const unsigned char *h, long n)
{
	unsigned char f[64];
	int len = 29 + 16, k;

	if (!retraining || !caller || !ev_info0 || ev_info1 || rt_info0 ||
	    n < len)
		return;
	for (k = 0; k < len; k++)
		f[k] = k < 4 ? 1 : h[(n - len + k) % CC_HIST];
	for (k = 4; k < 12; k++)
		if (f[k] != ((0x72 >> (11 - k)) & 1))
			return;
	if (!info_crc_ok(f, len - 16))
		return;
	rinfo0.parse(f);
	rt_info0 = 1;
	DSP_DBG("v34: INFO0a in the retrain\n");
}

/* the remote's INFO0 again in phase 2, after its first and before
   INFO1, not in a retrain: it has not taken ours (bit 28, its
   acknowledgement, 0; 11.2.2/V.34, 9.2.1.2/V.90), or repeats it a
   little longer having taken it (1).  A frame is taken once (the
   sampling phases' histories each have it) */
void V34Pump::info0_repeat_frame(const unsigned char *h, long n)
{
	unsigned char f[64];
	int len = 29 + 16, k;

	if (retraining || !ev_info0 || ev_info1 || n < len ||
	    total - info0_at < ms2samples(60, srate))
		return;
	for (k = 0; k < len; k++)
		f[k] = k < 4 ? 1 : h[(n - len + k) % CC_HIST];
	for (k = 4; k < 12; k++)
		if (f[k] != ((0x72 >> (11 - k)) & 1))
			return;
	if (!info_crc_ok(f, len - 16))
		return;
	/* no reversals within the frame */
	if (ev_rev && total - rev_time < (long)len * CC_SPS)
		ev_rev = 0;
	info0_at = total;
	info0_rep++;
	info0_rep_ack = f[28];
	info0_acked |= f[28];
	DSP_DBG("v34: INFO0 again from the remote (acknowledging ours: "
		"%d)\n", f[28]);
}

/* phase 2's recovery (11.2.2.1.1 and 11.2.2.2.1/V.34, 9.2.1.2.1/V.90),
   in our tone (B, or A answering) before the reversals: the remote's
   INFO0 again (it has not taken ours, or it waits for our
   acknowledgement of its), not those it sent before our last can have
   reached it (400 ms), or its tone a while without its INFO0 (we have
   not taken it): ours again, back to back (info0_more()),
   acknowledging the remote's once we have it.  After V.92's quick
   connect the analogue modem may come to phase 2 after our INFO0d is
   over: its first INFO0a, then, and ours once more.  c: the time in the
   tone; whether ours went out again */
bool V34Pump::info0_resend(long c)
{
	bool qc_late = qc_done && ev_info0 && !info0_again;
	bool repeated = info0_rep > 0 &&
		total - info0_tx_at >= ms2samples(400, srate);
	bool missed = !ev_info0 && tone_blocks >= 3 &&
		c >= ms2samples(500, srate);

	info0_rep = 0;
	if (retraining || short2 || info0_sent >= 16 ||
	    !(qc_late || repeated || missed))
		return false;
	DSP_DBG("v34: INFO0 again (%s)\n", qc_late ? "after QC" :
		!repeated ? "no INFO0 from the remote" :
		info0_rep_ack ? "the remote's acknowledges ours" :
		"the remote has not taken ours");
	info0_rec = info0_recovered = 1;
	info0_acked = qc_late || (repeated && info0_rep_ack);
	info0_again = ev_info0;
	cc_rev = 0;
	set_state(caller ? P2C_INFO0 : P2A_INFO0);
	make_info0(&cctx);
	cc_send();
	return true;
}

/* at the end of one of our INFO0s in the recovery: another, until the
   remote acknowledges ours or stops repeating its own (250 ms without
   it: its tone); whether it went out */
bool V34Pump::info0_more()
{
	if (!info0_rec)
		return false;
	if (info0_acked || info0_sent >= 16 ||
	    (ev_info0 && total - info0_at > ms2samples(250, srate))) {
		info0_rec = 0;
		return false;
	}
	info0_again = ev_info0;
	make_info0(&cctx);
	cc_send();
	return true;
}

/* a frame at the phase of the last sample: the frames do not depend on
   the tracked symbol timing, which may slip at the start of one (the
   remote's INFO1 comes at a new phase after its probing signals) */
void V34Pump::cc_phase_frame()
{
	CcRx *r = &ccr;
	int i = r->ph_last, len = info_expected(), k;
	long n = r->ph_n[i];
	const unsigned char *h = r->ph[i];

	rt_info0_frame(h, n);
	info0_repeat_frame(h, n);
	if (ev_info1 || n < len)
		return;
	for (k = 0; k < 8; k++)
		if (h[(n - len + 4 + k) % CC_HIST] != ((0x72 >> (7 - k)) & 1))
			return;
	for (k = 0; k < len; k++)
		r->frame[k] = k < 4 ? 1 : h[(n - len + k) % CC_HIST];
	if (!info_crc_ok(r->frame, len - 16) ||
	    (ev_info0 && info_crc_ok(r->frame, 29)))
		return;
	if (dsplibs_debug_level >= 6)
		DSP_DBG("v34: INFO frame at sampling phase %d (timing at "
			"%d)\n", i, r->best);
	r->frame_len = 0;
	info_frame(r);
}

/* one control channel decision (phase 2, retrains): INFO frames (the
   last bits, a few of the fill bits may be missing), MH sequences, and
   a reversal of the remote's tone (ev_rev at rev_time): a reversed
   symbol after six or more steady ones, then a steady one */
void V34Pump::cc_rx_bit(int bit)
{
	CcRx *r = &ccr;

	if (dsplibs_debug_level >= 6)
		DSP_DBG("v34: cc %s %ld %d p %.0f t %d\n", caller ? "C" : "A",
			total * 1000 / srate, bit, r->p, r->best);
	r->sr = (r->sr << 1) | bit;
	r->hist[r->hist_n++ % CC_HIST] = bit;
	rt_info0_frame(r->hist, r->hist_n);
	info0_repeat_frame(r->hist, r->hist_n);
	if (r->frame_len > 0)
		r->frame_len--;
	mh_check(r);
	/* a frame sync: slow timing adaptation for the rest of the frame */
	if ((r->sr & 0xFF) == 0x72)
		r->frame_len = info_expected();
	/* whether the last bits are a frame (fill, sync, CRC): the timing
	   may need a few of the fill bits after the probing signals */
	if (!ev_info1) {
		int len = info_expected(), i, ok;
		if (r->hist_n >= len - 3) {
			for (i = 0; i < len; i++)
				r->frame[i] = i < 4 ? 1 :
					r->hist[(r->hist_n - len + i) % CC_HIST];
			ok = info_crc_ok(r->frame, len - 16);
			for (i = 4; i < 12 && ok; i++)
				ok = r->frame[i] == ((0x72 >> (11 - i)) & 1);
			/* (not an INFO1: the remote's INFO0 again and what
			   follows it) */
			if (ok && ev_info0 && info_crc_ok(r->frame, 29))
				ok = 0;
			if (ok) {
				info_frame(r);
				return;
			}
		}
	}
	/* a reversal after a tone; its phase steps can fall into two
	   decisions */
	bit = r->rbit;
	if (bit) {
		if (r->zeros >= 6) {
			r->rev_cand = total;
			r->ones = 1;
		} else if (r->rev_cand && r->ones == 1)
			r->ones = 2;
		else
			r->rev_cand = 0;
		r->zeros = 0;
	} else {
		if (r->rev_cand && r->zeros == 0) {
			ev_rev = 1;
			rev_time = r->rev_cand;
			DSP_DBG("v34: reversal at %ld ms\n",
				r->rev_cand * 1000 / srate);
		}
		r->rev_cand = 0;
		r->zeros++;
	}
}

/* V.92's MH sequences (Table 32/V.92): fill, frame sync, the signal
   (4 bits), its information (4 bits), the CRC: 36 bits up to the CRC's
   end, on the modulation of INFO */
void V34Pump::mh_check(CcRx *r)
{
	unsigned char f[36];
	int i;

	if (r->hist_n < 36)
		return;
	for (i = 0; i < 36; i++)
		f[i] = i < 4 ? 1 : r->hist[(r->hist_n - 36 + i) % CC_HIST];
	for (i = 4; i < 12; i++)
		if (f[i] != ((0x72 >> (11 - i)) & 1))
			return;
	if (!info_crc_ok(f, 20))
		return;
	ev_mh = 1;
	mh_sig = f[12] << 3 | f[13] << 2 | f[14] << 1 | f[15];
	mh_info = f[16] << 3 | f[17] << 2 | f[18] << 1 | f[19];
	mh_seen = total;
	if (dsplibs_debug_level >= 4)
		DSP_DBG("v34: MH %x %x at %ld ms\n", mh_sig, mh_info,
			total * 1000 / srate);
}

/* an MH sequence: fill, sync, the signal and its information (both
   written first bit first in time, as in Table 32/V.92), CRC, fill */
void BitQ::make_mh(int sig, int info)
{
	int i;
	info_start();
	for (i = 3; i >= 0; i--)
		put((sig >> i) & 1, 1);
	for (i = 3; i >= 0; i--)
		put((info >> i) & 1, 1);
	info_end();
}

/* for the log */
static const char *mh_name(int sig)
{
	switch (sig) {
	case MH_REQ: return "MHreq";
	case MH_ACK: return "MHack";
	case MH_NACK: return "MHnack";
	case MH_CLRD: return "MHclrd";
	case MH_CDA: return "MHcda";
	case MH_FRR: return "MHfrr";
	}
	return "MH?";
}

/* the T1 of Table 33/V.92, in seconds (0: no limit; a code outside 1
   to 13: 60 s) */
static int moh_t1_secs(int code)
{
	static const short t[] = { 0, 10, 20, 30, 40, 60, 120, 180, 240, 360,
				   480, 720, 960, 0 };
	return code > 0 && code < 14 ? t[code] : 60;
}

/* an MH sequence from the remote, in a retrain (that is how it begins:
   its tone A, then MH sequences instead of the reversal) or while we
   answer one */
int V34Pump::mh_event()
{
	int in_retrain = retraining && !prx_on &&
		(state == V34_RT_SILENCE ||
		 (state >= P2A_INFO0 && state <= P2C_WAIT_INFO1));

	if (!connected || !v92)
		return DPSTAT_OK;
	if (state == MOH_REQ) {
		/* the answer to ours (Table 34/V.92) */
		if (moh_req == MH_REQ && mh_sig == MH_ACK) {
			moh_req_t1 = mh_info;
			DSP_DBG("v34: MHack, T1 %d s: the remote on hold for "
				"%d s\n", moh_t1_secs(mh_info), moh_req_hold);
			/* then our silence (9.10.2.1/V.92), for longer than
			   a retrain's start-up may take */
			cc_silence();
			txm = TXM_SILENCE;
			timeout = total + ms2samples(1000L * moh_req_hold +
						     60000, srate);
			set_state(MOH_HELD);
		} else if (moh_req == MH_REQ && mh_sig == MH_NACK) {
			/* denied: a fast reconnect (or a cleardown,
			   with DSP_V92_MOH_REQ's hold time 0) */
			moh_req = moh_req_hold ? MH_FRR : MH_CDA;
			DSP_DBG("v34: MHnack: %s\n", mh_name(moh_req));
			cctx.make_mh(moh_req, moh_req);
			cc_send();
			count = 0;
		}
		return DPSTAT_OK;
	}
	if (!in_retrain && state != MOH_RESP)
		return DPSTAT_OK;
	switch (mh_sig) {
	case MH_REQ:
		if (state == MOH_RESP)
			break;
		moh_respond(moh_t1 ? MH_ACK : MH_NACK);
		break;
	case MH_CLRD:
		if (state != MOH_RESP || moh_resp != MH_CDA)
			moh_respond(MH_CDA);
		break;
	case MH_FRR:
		/* its fast reconnect: ANSam, phase 1 (9.10.2.3/V.92) */
		if (state != MOH_RESP || moh_resp == MH_NACK) {
			DSP_DBG("v34: MHfrr: fast reconnect\n");
			moh_hold(10);
		}
		break;
	case MH_CDA:
		/* after our MHnack it would rather clear down */
		if (state == MOH_RESP && moh_resp == MH_NACK) {
			DSP_DBG("v34: MHcda: cleardown\n");
			moh_drop = 1;
		}
		break;
	}
	return DPSTAT_OK;
}

/* answer the remote's MH sequence with sig (MHack with our T1 code,
   MHnack or MHcda), repeated in MOH_RESP */
void V34Pump::moh_respond(int sig)
{
	DSP_DBG("v34: %s: %s%s\n", mh_name(mh_sig), mh_name(sig),
		sig == MH_ACK ? " (on hold)" : "");
	moh_resp = sig;
	moh_drop = 0;
	cctx.make_mh(sig, sig == MH_ACK ? moh_t1 : sig);
	cc_send();
	rt_blocks = rt_heard = quiet_blocks = 0;
	set_state(MOH_RESP);
}

/* on hold (or a fast reconnect): ANSam, and phase 1 as the answering
   modem when the remote comes back (QC1a or CM), or a disconnect after
   secs (0: no limit) */
void V34Pump::moh_hold(int secs)
{
	DSP_DBG("v34: on hold%s, ANSam for %d s\n",
		moh_resp == MH_ACK ? "" : " (reconnect)", secs);
	moh_end = secs ? total + (long)secs * srate : -1;
	/* phase 1 from scratch (9.10.2.1/V.92: what earlier phase 1
	   signals said does not count) */
	caller = 0;
	pcm = 0;
	v8_ok = v8_cj = v8_sync = 0;
	v8_n = v8_last_n = 0;
	ev_qc = qc_done = 0;
	qc_n = -1;
	aa_blocks = toneq_blocks = 0;
	cc_silence();
	cctx.pos = cctx.n;
	ansam_sign = 1;
	txm = TXM_ANSAM;
	/* the start-up after it as after a retrain */
	timeout = (moh_end > 0 ? moh_end : total + (long)srate * 86400) +
		ms2samples(60000, srate);
	set_state(V8A_ANSAM);
}

/* modem-on-hold of ours in data mode: the DTE's (+PMHR, the remote on
   hold for its seconds), or test: DSP_V92_MOH_REQ=<ms>[:<s>], that far
   into data mode (on hold for s seconds, default 10; 0: MHcda if it
   denies it) */
void V34Pump::moh_req_test()
{
	/* the DTE's (+PMHR), once a block */
	if (moh_poll++ % 160 == 0 && connected && v92 && !retraining &&
	    (state == V34_DATA || state == V92_RUN)) {
		long secs = modem_get_param(modem, MDMPRM_MOH_REQ);
		if (secs > 0) {
			modem_set_param(modem, MDMPRM_MOH_REQ, 0);
			moh_request((int)secs);
			return;
		}
	}
	/* (once a sample: V.92's data mode keeps no total) */
	if (!moh_req_at || !connected || !v92 || retraining ||
	    (state != V34_DATA && state != V92_RUN))
		return;
	if (++moh_req_n >= moh_req_at) {
		moh_req_at = 0;
		moh_request(moh_req_hold);
	}
}

/* ours (9.10.2.1/V.92): out of data mode as for a retrain, tone B, then
   MHreq (MOH_REQ); the remote on hold for hold_secs (at most its T1) if
   it grants it */
void V34Pump::moh_request(int hold_secs)
{
	retrain("modem-on-hold request");
	moh_req = MH_REQ;
	moh_req_hold = hold_secs;
	moh_req_t1 = 0;
	cc_tone();
	set_state(MOH_REQ);
}

/*
 *    echo canceller
 *
 *    Real pass band NLMS on the line samples, as the original V.34 modems
 *    do it: a near end part over the last EC_NEAR_LEN transmitted samples
 *    and a far end part over EC_FAR_LEN samples around the round trip
 *    delay (the echo from the other end's hybrid; over VoIP it comes back
 *    after the network delay).  Both share one error.  They train while
 *    we send our phase 3 sequences and the remote is silent, fast, then
 *    slower, and track slowly in full duplex.
 */
/* off, taps and powers cleared */
void V34Echo::init()
{
	memset(this, 0, sizeof(*this));
}

/* qsort()'s order of floats: ascending */
static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

/* where the far echo is: the received signal of the last EC_SEARCH
   samples against our (scrambled, nearly white) transmitted signal at
   every delay beyond the near part.  The far part then covers
   EC_FAR_LEN samples from a little before the strongest echo. */
void V34Echo::search()
{
	const int mask = EC_HIST - 1;
	float best = 0, run = 0, c, floor;
	int d, k, best_d = -1;
	/* only where our signal has been sent */
	long n_ = filled - EC_SEARCH;

	if (n_ > EC_HIST - EC_SEARCH)
		n_ = EC_HIST - EC_SEARCH;
	if (n_ < 2 * EC_NEAR_LEN) {
		far_on = 0;
		return;
	}
	float *corr_ = corr, *win_ = win;

	for (d = 0; d < n_; d++) {
		c = 0;
		for (k = 0; k < EC_SEARCH; k++)
			c += rxh[(rpos - k) & (EC_SEARCH - 1)] *
			     hist[(pos - k - d) & mask];
		corr_[d] = c * c;
	}
	/* the echo energy in windows of 32 delays, beyond the near part */
	for (d = 0; d < n_; d++) {
		run += corr_[d];
		if (d >= 32)
			run -= corr_[d - 32];
		win_[d] = run;
		if (d >= EC_NEAR_LEN && run > best) {
			best = run;
			best_d = d - 16;
		}
	}
	/* against the median window: the floor without any echo */
	qsort(win_, (size_t)n_, sizeof(float), cmp_float);
	floor = win_[n_ / 2] + 1e-9f;
	if (best_d < 0 || best < 16 * floor) {
		far_on = 0;
		DSP_DBG("v34: no far echo (best at %d, %.1f dB above the "
			"floor)\n", best_d, 10 * log10f(best / floor));
		return;
	}
	d = best_d - EC_FAR_LEN / 4;
	if (d < EC_NEAR_LEN)
		d = EC_NEAR_LEN;
	if (d > EC_HIST - EC_FAR_LEN - 1)
		d = EC_HIST - EC_FAR_LEN - 1;
	far_delay = d;
	far_on = 1;
	far_since = 0;
	DSP_DBG("v34: far echo at %d samples (%.1f dB above the floor)\n",
		best_d, 10 * log10f(best / floor));
}

/* one sample: tx what went out on the line, rx what came in; returns
   rx with the echo estimate taken away, and adapts the taps at mu
   (NLMS) unless the remote's signal is in the error.  Off: rx as it is
   (kept for the far echo search) */
float V34Echo::run(float tx, float rx)
{
	const int mask = EC_HIST - 1;
	float est = 0, en = 1, err, g, h;
	int i, p;

	pos = (pos + 1) & mask;
	hist[pos] = tx;
	filled++;
	rpos = (rpos + 1) & (EC_SEARCH - 1);
	if (!on) {
		rxh[rpos] = rx;
		return rx;
	}
	for (i = 0, p = pos; i < EC_NEAR_LEN; i++, p = (p - 1) & mask) {
		h = hist[p];
		est += wn[i] * h;
		en += h * h;
	}
	if (far_on)
		for (i = 0, p = (pos - far_delay) & mask; i < EC_FAR_LEN;
		     i++, p = (p - 1) & mask) {
			h = hist[p];
			est += wf[i] * h;
			en += h * h;
		}
	err = rx - est;
	pw_est += 0.001f * (est * est - pw_est);
	/* what the near part leaves, for the far echo search */
	rxh[rpos] = err;
	/* the remote's signal (its Su at the end of our training) in the
	   error: no adaptation on it and no part in the measured ERLE */
	es += 0.03f * (err * err - es);
	if (mu >= 0.05f && mu < 0.5f && es > 8 * pw_err + 1000) {
		if (!dt++)
			DSP_DBG("v34: echo canceller: the remote's signal "
				"(%.0f rms against %.0f), adaptation off\n",
				sqrtf(es), sqrtf(pw_err));
		return err;
	}
	/* normalized by the energy in the taps; not on our own silence */
	if (mu > 0 && en > 1e4f * (EC_NEAR_LEN + EC_FAR_LEN)) {
		g = mu * err / en;
		for (i = 0, p = pos; i < EC_NEAR_LEN; i++, p = (p - 1) & mask)
			wn[i] += g * hist[p];
		if (far_on)
			for (i = 0, p = (pos - far_delay) & mask;
			     i < EC_FAR_LEN; i++, p = (p - 1) & mask)
				wf[i] += g * hist[p];
	}
	if (mu > 0) {
		pw_rx += 0.001f * (rx * rx - pw_rx);
		pw_err += 0.001f * (err * err - pw_err);
	}
	if (dsplibs_debug_level >= 6) {
		dbg_rx += 0.001f * (rx * rx - dbg_rx);
		dbg_err += 0.001f * (err * err - dbg_err);
		dbg_tx += 0.001f * (tx * tx - dbg_tx);
		dbg_est += 0.001f * (est * est - dbg_est);
		if ((pos & 4095) == 0)
			DSP_DBG("v34: echo canceller mu %.3f in %.0f out %.0f "
				"(%.1f dB), ours %.0f, the echo taken %.0f\n",
				mu, dbg_rx, dbg_err,
				10 * log10f((dbg_rx + 1) / (dbg_err + 1)),
				dbg_tx, dbg_est);
	}
	return err;
}

/* the adaptation, per sample: train while our own phase 3 sequences go
   out (the remote is silent then), then track */
void V34Pump::ec_schedule()
{
	V34Echo *e = &ec;
	int ours = state == P3A_S || state == P3A_SBAR ||
		state == P3A_PP || state == P3A_TRN ||
		state == P3C_S || state == P3C_SBAR ||
		state == P3C_PP || state == P3C_TRN ||
		/* V.90: Sd to Jd, the analogue modem is silent after Ja */
		state == P3D_SD || state == P3D_JD ||
		/* V.92 PCM upstream: the same, until Su */
		(state == V92_RUN && v92u.ec_train());
	int training;

	if (ours && !e->on) {
		e->on = 1;
		e->n = e->since = e->filled = 0;
		e->far_on = 0;
		e->judged = e->bypass = e->trained = 0;
		e->pw_rx = e->pw_err = e->es = e->pw_est = 0;
		e->dt = 0;
		DSP_DBG("v34: echo canceller on\n");
	}
	if (ours)
		e->since++;
	/* the answerer sends its J until it has heard the caller's S */
	training = ours && (!caller ||
		e->since > rtd + ms2samples(150, srate));
	if (training) {
		e->n++;
		/* the far echo is found after some of our signal went out
		   (and came back) */
		if (!e->far_on && (e->n == EC_SEARCH + ms2samples(50, srate) ||
				   e->n == ms2samples(250, srate) ||
				   e->n == ms2samples(500, srate) ||
				   e->n == ms2samples(1000, srate)))
			e->search();
		/* fast at first, also for a far part found late */
		e->far_since++;
		{
			long t = e->n;
			if (e->far_on && e->far_since < t)
				t = e->far_since;
			e->mu = t < ms2samples(300, srate) ? 0.5f :
				t < ms2samples(600, srate) ? 0.2f : 0.05f;
		}
		e->trained = 1;
	} else if (e->trained) {
		e->mu = EC_TRACK_MU;
		/* what the training took away: with no echo to cancel
		   (a 4-wire network, or the remote did not stay silent)
		   the canceller would only add noise */
		if (!e->judged) {
			float erle = 10 * log10f((e->pw_rx + 1) /
						 (e->pw_err + 1));
			e->judged = 1;
			/* V.90: the receiver's error so far had our echo
			   in it; the precoder our MP asks for is to be found
			   from what comes now */
			if (v90_digital())
				prx.err_reset();
			/* (DSP_EC=0: off anyway, for tests) */
			e->bypass = erle < 6 || (getenv("DSP_EC") &&
						 !atoi(getenv("DSP_EC")));
			DSP_DBG("v34: echo canceller took %.1f dB away%s\n",
				erle, e->bypass ? ": off" : "");
		}
	}
}

/* the remote's tone A / B, not data: nearly all the power in its bin
   (data spreads over some 20) */
void V34Pump::rt_tone(float x)
{
	rt_e += x * x;
	own_g.run(x);
	if (rt_g.run(x)) {
		/* without the echo of our own tone */
		float e = rt_e / PROBE_PERIOD - own_g.power;
		rt_e = 0;
		/* the Agere modem's tone A when it asks for a retrain in
		   V.90 has as much again at 1800 Hz */
		rt_blocks = e > tone_power(150) &&
			rt_g.power > (pcm ? 0.4f : 0.6f) * e ?
			rt_blocks + 1 : 0;
		if (dsplibs_debug_level >= 5 && retraining)
			DSP_DBG("v34: rt %ld e %.0f tone %.0f own %.0f\n",
				total * 1000 / srate, e, rt_g.power,
				own_g.power);
		if (rt_blocks >= 5 && !rt_heard) {
			rt_heard = 1;
			DSP_DBG("v34: tone %s heard\n", caller ? "A" : "B");
		}
	}
}

/* one received sample at 9600 Hz: before phase 2 V.8 (V.21, ANSam,
   QC1a, TONEq) and INFO0 after it; from phase 3 on the primary channel
   receiver after the echo canceller; otherwise the control channel, the
   remote's tone and L1/L2 (phase 2, retrains) */
void V34Pump::pump_rx_sample(float x)
{
	int bit, k;

	if (state < P2A_INFO0) {
		bit = v21.run(x);
		if (bit >= 0 && v21.carrier)
			v8_rx_bit(bit);
		if (state == V8A_JM || state == V8A_GAP ||
		    state == V8C_CJ || state == V8C_GAP ||
		    state == QCA_TONEQ || state == QCA_GAP2) {
			bit = ccr.sample(x);
			if (bit >= 0)
				cc_rx_bit(bit);
			cc_phase_frame();
		}
		if (state == V8C_LISTEN)
			ans_analyse(x);
		if (aa_g.run(x)) {
			if (aa_g.power > tone_power(300))
				aa_blocks++;
			else
				aa_blocks = 0;
		}
		if (state == QCA_ANSPCM || state == QCA_TONEQ) {
			toneq_space_g.run(x);
			if (toneq_g.run(x))
				toneq_blocks = toneq_g.power > tone_power(150) &&
					toneq_space_g.power < toneq_g.power / 10 ?
					toneq_blocks + 1 : 0;
		}
		return;
	}
	if (prx_on) {
		ec_schedule();
		if (!ec.bypass)
			x = ec.run(last_tx, x);
		prx.sample(x);
		if (state == V34_DATA || v90_digital() || renegotiating)
			rt_tone(x);
		return;
	}
	if (retraining)
		rt_tone(x);
	bit = ccr.sample(x);
	if (bit >= 0)
		cc_rx_bit(bit);
	cc_phase_frame();
	if (tone_g.run(x)) {
		tone_blocks = tone_g.power > tone_power(150) ?
			tone_blocks + 1 : 0;
		quiet_blocks = tone_g.power < tone_power(30) ?
			quiet_blocks + 1 : 0;
	}
	k = probe_rx_sample(x);
	if (k >= 0) {
		probe_blocks = k ? probe_blocks + 1 : 0;
		if (probe_blocks == 2 && !l1_start &&
		    (state == P2A_SILENCE || state == P2C_SILENCE)) {
			l1_start = total - 2 * PROBE_PERIOD;
			DSP_DBG("v34: remote L1 at %ld ms\n",
				l1_start * 1000 / srate);
		}
	}
	/* measure L2: from 20 ms after L1 to the end of our wait */
	if (l1_start && total - l1_start >= ms2samples(180, srate) &&
	    total - l1_start < ms2samples(460, srate))
		probe_collect(x);
}

/*
 *    handshake, once per sample
 */

/* no V.34: back to the low speed modulation V.8 ran for, Bell 212A
   after a 2225 Hz answer tone, else V.32bis (automode); returns
   DPSTAT_CHANGEDP */
int V34Pump::fallback(const char *why)
{
	if (v8_low)
		return v8_back(why);
	if (caller && rt && rt->answer_tone == ANSTONE_2225) {
		/* a Bell answerer: 212A (or 103, see dp_v22.cpp) */
		DSP_DBG("v34: %s, Bell answer tone: Bell 212A\n", why);
		return hand_over(DP_B212, 0);
	}
	DSP_DBG("v34: %s, falling back to V.32bis\n", why);
	if (rt)
		rt->v32_automode = 1;
	return hand_over(DP_V32BIS, !caller);
}

/* whether INFO0 bits 12:19 (rates) allow symbol rate b, and for our
   transmitter its carrier high (-1: either, as for receiving) */
static int v34_info0_allows(int rates, int b, int high)
{
	switch (b) {
	case V34_BAUD_2743: return rates & 0x01;
	case V34_BAUD_2800: return rates & 0x02;
	case V34_BAUD_3429:
		return (rates & 0x04) && (high < 0 || (rates & 0x80));
	case V34_BAUD_3000:
		return rates & (high < 0 ? 0x18 : high ? 0x10 : 0x08);
	case V34_BAUD_3200:
		return rates & (high < 0 ? 0x60 : high ? 0x40 : 0x20);
	default: return 1;
	}
}

/* the carrier the caller can transmit on at symbol rate b (INFO0c bits
   15:18): 0 low (if it can), 1 high, -1 neither */
static int v34_caller_carrier(int rates, int b)
{
	int low = b == V34_BAUD_3000 ? 0x08 : 0x20;
	if (b != V34_BAUD_3000 && b != V34_BAUD_3200)
		return 0;       /* (no choice there) */
	return rates & low ? 0 : rates & (low << 1) ? 1 : -1;
}

/* the answerer picks the symbol rates: the fastest both modems allow,
   at a carrier the caller can transmit on (11.2/V.34, Table 16/V.34) */
void V34Pump::choose_bauds()
{
	int b, best = V34_BAUD_2400, best_rate = 0, best_high = 0;

	probe_analyse();
	for (b = V34_BAUD_2400; b <= V34_BAUD_3429; b++) {
		int rate = rate_of_baud[b], high;
		if (b == V34_BAUD_3429 && (!rinfo0.allow_3429 ||
					   !(rinfo0.rates & 0x04)))
			continue;
		if (b == V34_BAUD_2743 || b == V34_BAUD_2800)
			continue;
		high = v34_caller_carrier(rinfo0.rates, b);
		if (high < 0)
			continue;
		/* the caller can receive at most this */
		if (rinfo1c.r[b].max_rate < rate)
			rate = rinfo1c.r[b].max_rate;
		/* (none that the line or the caller allows) */
		if (!rate)
			continue;
		if (rate >= best_rate) {
			best_rate = rate;
			best = b;
			best_high = high;
		}
	}
	baud_tx = baud_rx = best;
	high_rx = best_high;
	max_rate_rx = rate_of_baud[best] ? rate_of_baud[best] :
		v34_min_rate(best);
}

/* the primary channel at the agreed symbol rates; -1 if the receiver
   can't be set up (out of memory) */
int V34Pump::primary_start()
{
	/* the carriers as the receiving modems asked for them in INFO1c
	   (answerer -> caller) and INFO1a (caller -> answerer) */
	int high = caller ? high_tx : rinfo1c.r[baud_tx].high;
	ptx.init(baud_tx, high, caller);
	if (prx.init(baud_rx, caller ? 0 : high_rx, caller))
		return -1;
	prx_on = 1;
	txm = TXM_PRIMARY;
	return 0;
}

/* V.90 phase 3: the remote's V.34 signals in, our PCM signals out */
int V34Pump::v90_start()
{
	baud_rx = rinfo1a.baud_ac;
	if (rate_of_baud[baud_rx])
		max_rate_rx = rate_of_baud[baud_rx];
	/* the low carrier our INFO1d asked for.  In V.92's short phase 2
	   (no INFO1d) INFO1a's bit 33 names the carrier (Table 19/V.92),
	   but the modems that set it send on the low one all the same:
	   the SM56 modem at 3200 baud (its signal centred where it is
	   after a full phase 2, 90 Hz below the high carrier) and the
	   Agere modem at 3429 (one carrier) */
	if (prx.init(baud_rx, 0, 1) || prx.set_v90())
		return -1;
	prx_on = 1;
	txm = TXM_SILENCE;
	{
		/* the data frame alignment of QTS */
		const long *fc = v90tx.fclock;
		long fb = v90tx.fbase;
		v90tx.init(alaw, rinfo1a.uinfo);
		v90tx.fclock = fc;
		v90tx.fbase = fb;
	}
	pcm_tx = 1;
	set_state(P3D_WAIT_JA);
	return 0;
}


/* without probing (V.92's short phase 2): the fastest rate the mse of
   the receiver's training allows, as probe_analyse() asks of the SNR */
static int v34_rate_of_mse(int baud, float mse)
{
	float rs = v34_symbol_rate(baud), snr;
	int n;
	if (mse < 1e-4f)
		mse = 1e-4f;
	/* the 4 point signals have twice the power of the error per
	   dimension the mse counts */
	snr = 10 * log10f(1 / mse);
	for (n = v34_max_rate_of_baud[baud];
	     n > v34_min_rate(baud) &&
	     snr < 3 * 2400.0 * n / rs + 4 + PROBE_MARGIN; n--)
		;
	return n;
}

/* our MP (Table 16/V.90), filled to whole data frames (D bits): the
   most upstream rate we receive (from the training's mse without
   probing) and the precoder, found once per exchange; ack: its
   acknowledgement bit */
void V34Pump::v90_make_mp(int ack)
{
	if (!max_rate_rx) {
		max_rate_rx = v34_rate_of_mse(baud_rx, prx.mse);
		DSP_DBG("v34: upstream at most %d bit/s (mse %.5f)\n",
			max_rate_rx * 2400, prx.mse);
	}
	/* (V.90's upstream starts at 4800 bit/s, Table 17/V.90) */
	if (max_rate_rx < rate_floor())
		max_rate_rx = rate_floor();
	/* the precoder once for the exchange: every MP of it asks for the
	   same (the analogue modem may take the first, or the last), and
	   the data mode receiver takes it */
	if (!seq_len) {
		float gain = prx.precoder(mp_h);
		DSP_DBG("v34: V.90 precoding would gain %.2f dB (over %ld "
			"symbols): h %d %d %d %d %d %d\n", gain, prx.err_n,
			mp_h[0], mp_h[1], mp_h[2], mp_h[3], mp_h[4], mp_h[5]);
		if (gain < precode_min)
			memset(mp_h, 0, sizeof(mp_h));
	}
	seq_len = v90_mp_build(seq, max_rate_rx, V34_TRELLIS, 1, ack,
				 0x1FFF, mp_h);
	while (seq_len % D)
		seq[seq_len++] = 0;
	seq_pos = 0;
	seq_ack = ack;
}

/* PcmFrames' hooks for V.90 (the digital modem's data frames): after
   Rd or Rt, TRN2d, MP and Ed on CPt's constellations, from cleared
   scrambler and filters */
void V34Pump::fs_reinit(V90Tx *t)
{
	if (v90_rn_init) {
		v90_rn_init = 0;
		D = cpt.drn + 8;
		t->enc.init(&cpt, D, t->lin);
	}
}

/* CP's: B1d and data */
const V90Cp *V34Pump::fs_data_cp()
{
	return &cp;
}

/* MP, again until CP's acknowledgement (or E) */
void V34Pump::fs_seq_next(int)
{
	v90_make_mp(prx.ev_cp);
}

bool V34Pump::fs_acked()
{
	return prx.ev_cpack || prx.ev_e;
}

/* CPs': Ed, then silence until CP (9.6.1.2.5/V.90) */
bool V34Pump::fs_silence()
{
	return prx.cp_silence;
}

void V34Pump::fs_ed_end()
{
	sil_cp = prx.cp_count;
}

void V34Pump::fs_data(unsigned char *b, int n)
{
	CoreBits{ modem }.get(b, n);
}

/* the upstream data mode, as our MP asked for it */
int V34Pump::v90_data_setup()
{
	V34Params p;
	int n, mask = cp.rate_mask & 0x1FFF;

	for (n = max_rate_rx; n >= 2; n--)
		if ((mask >> (n - 2)) & 1 && v34_rates[baud_rx][n - 1].b)
			break;
	if (n < 2 || p.init(baud_rx, n, 1))
		return -1;
	p.states = 16 << V34_TRELLIS;
	/* the analogue modem precodes with what our last MP said, not
	   with what the equalizer has adapted to since */
	p.set_precoder(mp_h);
	prx.start_data(&p, CoreBits{ modem });
	up_n = n;
	rate_rx = p.rate;
	rate_tx = (cp.drn + 20) * 8000 / 6;
	DSP_DBG("v34: V.90 rates: down %d, up %d\n", rate_tx, rate_rx);
	return 0;
}

/* to state s, the transmitter on to segment seg */
void V34Pump::tx_next(enum v34_state s, enum v34_txseg seg)
{
	set_state(s);
	ptx.segment(seg);
}

/* the fastest rate allowed by both MP sequences at the symbol rate */
static int v34_pick_rate(int baud, int max, int mask)
{
	int n;
	for (n = max; n >= 1; n--)
		if ((mask >> (n - 1)) & 1 && v34_rates[baud][n - 1].b)
			return n;
	return 0;
}

/* both MP sequences are known: set up the data mode */
int V34Pump::data_setup()
{
	const V34Mp *l = &ptx.mp, *r = &prx.mp;
	int mask = l->rate_mask & r->rate_mask, ntx, nrx;
	int ca = l->rate_ca < r->rate_ca ? l->rate_ca : r->rate_ca;
	int ac = l->rate_ac < r->rate_ac ? l->rate_ac : r->rate_ac;
	V34Params p;

	/* bit 50 of MP: one modem without asymmetric rates, one rate */
	if (!l->asym || !r->asym) {
		if (ca > ac)
			ca = ac;
		ac = ca;
	}
	DSP_DBG("v34: MP max rates (ours, the remote's): %d/%d, %d/%d, "
		"asymmetric %d/%d, non-linear %d\n", l->rate_ca, l->rate_ac,
		r->rate_ca, r->rate_ac, l->asym, r->asym, r->nonlinear);
	ntx = v34_pick_rate(ptx.baud, caller ? ca : ac, mask);
	nrx = v34_pick_rate(prx.baud, caller ? ac : ca, mask);
	if (!ntx || !nrx || r->trellis > 2 || l->trellis > 2 ||
	    p.init(ptx.baud, ntx, r->expanded))
		return -1;
	/* the remote's receiver asks for the non-linear encoder */
	if (r->nonlinear)
		p.set_nonlinear(0.3125);
	/* each MP asks for the trellis code of the other's transmitter */
	p.states = 16 << r->trellis;
	p.set_precoder(r->h);
	ptx.start_data(&p, CoreBits{ modem });
	rate_tx = p.rate;
	if (p.init(prx.baud, nrx, l->expanded))
		return -1;
	p.states = 16 << l->trellis;
	p.set_precoder(l->h);
	prx.start_data(&p, CoreBits{ modem });
	rate_rx = p.rate;
	DSP_DBG("v34: data rates tx %d rx %d, trellis tx %d rx %d states%s\n",
		rate_tx, rate_rx, 16 << r->trellis, 16 << l->trellis,
		r->nonlinear ? ", non-linear tx" : "");
	return 0;
}

/* phase 4 or a renegotiation: our MP (10.1.3.9/V.34), type 1 with the
   precoder, the most we receive (max_rate_rx, at most DSP_V34_RX_MAX),
   anything the other way */
void V34Pump::mp_start()
{
	V34Mp *mp = &ptx.mp;
	memset(mp, 0, sizeof(*mp));
	mp->type = 1;
	/* test: DSP_V34_RX_MAX=<n> asks for at most n * 2400 bit/s */
	if (getenv("DSP_V34_RX_MAX") && atoi(getenv("DSP_V34_RX_MAX")) > 0 &&
	    max_rate_rx > atoi(getenv("DSP_V34_RX_MAX")))
		max_rate_rx = atoi(getenv("DSP_V34_RX_MAX"));
	/* (no less than the symbol rate has: no common rate else) */
	if (max_rate_rx < rate_floor())
		max_rate_rx = rate_floor();
	mp->rate_ca = caller ? 14 : max_rate_rx;
	mp->rate_ac = caller ? max_rate_rx : 14;
	mp->expanded = 1;
	mp->trellis = V34_TRELLIS;
	mp->rate_mask = 0x3FFF;
	mp->asym = 1;
	/* precoding where it gains enough over the power of its dither */
	{
		float gain = prx.precoder(mp->h);
		DSP_DBG("v34: precoding would gain %.2f dB: h %d %d %d %d %d "
			"%d\n", gain, mp->h[0], mp->h[1], mp->h[2], mp->h[3],
			mp->h[4], mp->h[5]);
		if (gain < precode_min)
			memset(mp->h, 0, sizeof(mp->h));
	}
	tx_next(P4_MP, V34_TX_MP);
}

/* a retrain: both modems go back to phase 2 at tones A and B
   (the INFO0 sequences stand), probe the line again and train the
   primary channel anew; whoever starts it with silence and its tone, the
   other modem answers the same way when it loses the data signal or
   hears the tone */
void V34Pump::retrain(const char *why)
{
	DSP_DBG("v34: retrain %d (%s)\n", ++retrains, why);
	retraining = 1;
	renegotiating = 0;
	short2 = 0;
	if (prx_on) {
		prx.release();
		prx_on = 0;
	}
	txm = TXM_SILENCE;
	/* V.90: V.34 signals again until the next INFO1a */
	if (pcm_tx) {
		pcm_tx = 0;
		ref_n = 0;
		v90tx.seg = V90_SILENCE;
		v90tx.rd_req = v90tx.sil_req = 0;
		v90_rn_init = v90_rd_wait = 0;
		up_n = 0;
		seq_len = 0;
	}
	ec.on = 0;
	/* the control channel as after INFO0 */
	ccr.init(caller ? 2400 : 1200);
	ccr.snap = 1;
	cctx.pos = cctx.n;
	cc_rev = 0;
	cc_sym = 0;
	ev_info1 = ev_rev = 0;
	rt_info0 = 0;
	info0_sent = info0_rep = info0_rec = info0_recovered = 0;
	rtd = 0;
	/* the probing */
	tone_g.init(caller ? 2400 : 1200, PROBE_PERIOD,
		      srate);
	probe_g[0].init(1050, PROBE_PERIOD, srate);
	probe_g[1].init(3000, PROBE_PERIOD, srate);
	tone_blocks = probe_blocks = 0;
	pblock_e = 0;
	l1_start = 0;
	memset(pacc, 0, sizeof(pacc));
	memset(pprev, 0, sizeof(pprev));
	ptotal = pdiff = 0;
	pcount = 0;
	rt_blocks = rt_heard = 0;
	lost = bad = 0;
	/* V.90's phase 3 (Jd, the DIL) takes about 8 s more */
	timeout = total + ms2samples(pcm ? 30000 : 15000, srate);
	set_state(V34_RT_SILENCE);
}

/* the rate (N) to receive at after a renegotiation, from the data mode
   mse: up < 0 lower (by as many steps as the mse asks for, at least
   one), > 0 one higher, 0 whatever the mse says (when the remote
   started it).  At the symbol rate, a step of 2400 bit/s takes about
   3 dB per bit per symbol. */
int V34Pump::reneg_rate(int up)
{
	float step = 3 * 2400.0f / v34_symbol_rate(prx.baud), mse = prx.mse;
	int n = rate_rx / 2400, k;

	if (up > 0 || (!up && mse * powf(10, step / 10) < V34_RN_UP))
		return n < rn_max ? n + 1 : n;
	if (up < 0 || mse > V34_RN_DOWN) {
		k = (int)ceilf(10 * log10f(mse / V34_RN_TARGET) / step);
		n -= k < 1 ? 1 : k;
	}
	return n < rate_floor() ? rate_floor() : n;
}

/* the least N we may receive at: the symbol rate's least (Table
   10/V.34), and 4800 bit/s for V.90's upstream (Table 17/V.90) */
int V34Pump::rate_floor() const
{
	int lo = v34_min_rate(prx_on ? prx.baud : baud_rx);
	return pcm_tx && lo < 2 ? 2 : lo;
}

/* a rate renegotiation (11.6/V.34), by us or in answer to the remote's
   S: S, S-bar, TRN, then MP with the new rate we can receive, MP', E
   and data at the new rates, all with the equalizers as they are; the
   link (V.42) carries on */
void V34Pump::renegotiate(const char *why, int n)
{
	DSP_DBG("v34: renegotiation %d (%s): receive at most %d, mse %.3f\n",
		++renegs, why, n * 2400, prx.mse);
	renegotiating = 1;
	max_rate_rx = n;
	ptx.data_on = 0;
	ptx.ack_mp = 0;
	ptx.mp_sent = 0;
	ptx.trn16 = 0;
	rn_hi = rn_lo = bad = 0;
	/* no E 2500 ms plus the round trips after S-bar: a retrain */
	timeout = total + ms2samples(3000, srate) + 3 * rtd;
	tx_next(V34_RN_S, V34_TX_S);
}

/* a V.90 rate renegotiation (9.6/V.90), ours or in answer to the
   analogue modem's S: Rd, Rd-bar (on a data frame boundary, after its
   S-bar when it started), TRN2d, MP with the upstream rate we can
   receive now, MP', Ed, B1d; the analogue modem sends S, S-bar, SCR, CP,
   CP', E and B1.  Its CPs asks for a silent period first: Ed, silence
   until CP, Rt, Rt-bar, MP. */
void V34Pump::v90_renegotiate(const char *why, int n, int remote)
{
	DSP_DBG("v34: V.90 renegotiation %d (%s): upstream at most %d, mse "
		"%.3f\n", ++renegs, why, n * 2400, prx.mse);
	renegotiating = 1;
	max_rate_rx = n;
	up_n = 0;
	seq_len = 0;
	ack_sent = 0;
	prx.ev_cp = prx.ev_cpack = prx.ev_e = 0;
	prx.cp_silence = 0;
	rn_hi = rn_lo = bad = 0;
	v90_rd_wait = remote;
	rn_sbar = prx.sbar_count;
	if (!remote)
		v90_rd_start();
	/* no E 5000 ms plus 2 round trips after Rd-bar: a retrain */
	timeout = total + ms2samples(5500, srate) + 3 * rtd;
	set_state(P4D_TRAIN);
}

/* our Rd (9.6/V.90): the encoder starts again on CPt's constellations
   with TRN2d */
void V34Pump::v90_rd_start()
{
	DSP_DBG("v34: V.90 Rd\n");
	v90tx.rd_req = 1;
	v90_rn_init = 1;
	fmode = PCM_TRN;
	fcount = 0;
}

/* V.92 PCM upstream: phases 3 and 4 and the data on the 8000 Hz samples,
   in process() */
void V34Pump::v92_start()
{
	V92Info1a ia;
	ia.parse(ccr.frame);
	v92u.init(alaw, &ia, v90tx.fclock, v90tx.fbase);
	v92u.link.modem = modem;
	if (rt)
		rt->pcm = 1;
	set_state(V92_RUN);
}

/* V.91 after V.8: v91.cpp from here on, at 8000 Hz */
void V34Pump::v91_start()
{
	const char *k = getenv("DSP_V91_K");
	DSP_DBG("v34: V.91\n");
	v91m.init(alaw, caller, k ? atoi(k) : 0, getenv("DSP_V91_CC") != NULL);
	v91m.link.modem = modem;
	if (rt)
		rt->pcm = 1;
	set_state(V91_RUN);
}

/* 8.3/V.91: V.34 from phase 2 on instead */
int V34Pump::v91_escape()
{
	DSP_DBG("v34: V.91 does not work, V.34\n");
	v91_on = 0;
	if (rt)
		rt->pcm = 0;
	if (caller) {
		set_state(P2C_INFO0);
		cc.init(srate, 1200, CC_BAUD, 0.5f, 4, 1, V34_TONE_AMP);
	} else {
		set_state(P2A_INFO0);
		cc.init(srate, 2400, CC_BAUD, 0.5f, 4, 1, V34_TONE_AMP);
	}
	make_info0(&cctx);
	cc_send();
	return 0;
}

/* the start-up and data mode, once a sample before it goes out: V.8,
   phases 2 to 4 of V.34 / V.90, retrains, renegotiations and
   modem-on-hold; returns DPSTAT_OK, DPSTAT_CONNECT, DPSTAT_CHANGEDP or
   DPSTAT_ERROR */
int V34Pump::handshake()
{
	moh_req_test();
	long c = count;

	/* V.90: the analogue modem asks for a retrain in phase 3 or 4 with
	   silence and its tone A (it is the V.34 answerer in phase 2) */
	if (v90_digital() && rt_blocks >= 6)
		retrain("tone A");
	/* the remote retrains instead (11.6.2/V.34) */
	if (renegotiating && rt_blocks >= 6)
		retrain(caller ? "tone A" : "tone B");
	if (ev_mh) {
		ev_mh = 0;
		mh_event();
		/* (it may have started a state) */
		c = count;
	}

	switch (state) {
	/* V.8 answerer */
	case V8A_SILENCE:
		/* V.8's 0.2 s at least (8.2/V.8): the core's answer delay (its
		   S-register, 2 s) is V.25's silent interval of 1.8 to 2.5 s
		   (4.3/V.25), the answer tone then within it */
		if (c >= ms2samples(200, srate)) {
			set_state(V8A_ANSAM);
			txm = TXM_ANSAM;
		}
		break;
	case V8A_ANSAM:
		/* V.92's QC1a ahead of CM: short phase 1 (9.2.4.1/V.92) */
		if (ev_qc && v92 && qc_ok && !qc_done && !v8_ok) {
			int wxyz = ((qc_frame >> 4) & 1) << 3 |
				((qc_frame >> 6) & 1) << 2 |
				((qc_frame >> 7) & 1) << 1 |
				((qc_frame >> 8) & 1);
			qc_done = 1;
			if (v92_uqts(wxyz) < 0) {
				DSP_DBG("v34: QC1a: a cleardown\n");
				return DPSTAT_ERROR;
			}
			DSP_DBG("v34: V.92 quick connect, UQTS %d\n",
				v92_uqts(wxyz));
			v90tx.init(alaw, 0);
			v90tx.uqts = v92_uqts(wxyz);
			v90tx.anspcm_init(1);   /* -12 dBm0 */
			/* QCA1d (Table 13/V.92): bits 20:29 0 1 1 P 0 0 0 L M 1,
			   LM 01 for -12 dBm0 */
			v8tx.reset();
			for (int r = 0; r < 2; r++) {
				v8tx.put(0x3FF, 10);
				v8tx.put(0x2AA, 10);    /* 0101010101 */
				v8tx.put(0x6 | qc_lapm << 3 | 1 << 8 | 1 << 9,
					 10);
			}
			v8tx.put(0x3FF, 10);
			v8tx_repeat = 0;
			fsk.init(1650, 1850, V34_TONE_AMP, 300, srate, 1);
			txm = TXM_FSK;
			set_state(QCA_QCA1D);
			break;
		}
		if (v8_ok) {
			/* back from hold with a CM of no modulations: a
			   cleardown (9.10.2.1/V.92) */
			if (moh_end && !v8_remote_v34()) {
				DSP_DBG("v34: on hold: CM without modulations\n");
				return DPSTAT_ERROR;
			}
			v8_dp = 0;
			if (!v8_remote_v34()) {
				/* JM (what both have), CJ, then the best of it
				   (our CM's offer is all of them) */
				v8_dp = v8_choice();
				if (!v8_dp)
					return fallback("no V.34 in CM");
				DSP_DBG("v34: V.8 agreed on %s\n",
					v8_dp_name(v8_dp));
				set_state(V8A_JM);
				v8_make_msg(&v8tx);
				fsk.init(1650, 1850, V34_TONE_AMP, 300, srate, 1);
				txm = TXM_FSK;
				break;
			}
			pcm = v8_v90();
			v91_on = v8_v91();
			set_state(V8A_JM);
			/* the caller's INFO0c follows its CJ by only 75 ms:
			   listen for it already; with V.90 the analogue
			   modem's INFO0a */
			ccr.init(pcm ? 2400 : 1200);
			v8_make_msg(&v8tx);
			fsk.init(1650, 1850, V34_TONE_AMP, 300, srate, 1);
			txm = TXM_FSK;
		} else if (moh_end) {
			/* on hold: until T1, for phase 1 only */
			if (moh_end > 0 && total >= moh_end) {
				DSP_DBG("v34: on hold for T1: disconnect\n");
				return DPSTAT_ERROR;
			}
		} else if (aa_blocks >= 10)
			return fallback("V.32 caller");
		else if (c >= ms2samples(5000, srate))
			return fallback("no CM");
		break;
	case V8A_JM:
		if (v8_cj) {
			set_state(V8A_GAP);
			txm = TXM_SILENCE;
		}
		break;
	case V8A_GAP:
		if (c >= ms2samples(75, srate) && v8_dp)
			return v8_handover();
		if (c >= ms2samples(75, srate) && v91_on) {
			v91_start();
			break;
		}
		if (c >= ms2samples(75, srate) && pcm) {
			/* the digital modem of V.90 takes the part of the
			   V.34 call modem from phase 2 on */
			DSP_DBG("v34: V.90 with us as the digital modem\n");
			caller = 1;
			tone_g.init(2400, PROBE_PERIOD,
				      srate);
			/* a retrain: the analogue modem's tone A, ours B */
			rt_g.init(2400, PROBE_PERIOD, srate);
			own_g.init(1200, PROBE_PERIOD, srate);
			set_state(V8C_GAP);
			count = c = ms2samples(75, srate);
		}
		if (c >= ms2samples(75, srate) && !pcm) {
			set_state(P2A_INFO0);
			cc.init(srate, 2400, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			make_info0(&cctx);
			cc_send();
		}
		break;

	/* V.92 short phase 1, answering */
	case QCA_QCA1D:
		if (v8tx.pos >= v8tx.n) {
			txm = TXM_SILENCE;
			set_state(QCA_GAP);
		}
		break;
	case QCA_GAP:
		if (c >= ms2samples(75, srate)) {
			/* QTS from the next 8000 Hz symbol on */
			out_n++;
			v90tx.fclock = &out_n;
			v90tx.segment(V90_QTS);
			out_n--;
			pcm_tx = 1;
			toneq_g.init(980, PROBE_PERIOD, srate);
			toneq_space_g.init(1180, PROBE_PERIOD, srate);
			toneq_blocks = 0;
			set_state(QCA_ANSPCM);
		}
		break;
	case QCA_ANSPCM:
		/* TONEq: the analogue modem has heard ANSpcm (and checked
		   it); within 2 s of QCA1d, or V.8 after all.  TONEq is
		   V.21's low channel mark (980 Hz), so 128 ms of it without
		   that channel's space (CM has runs of marks), and not
		   before ANSpcm: the end of the analogue modem's CM, as it
		   stops for our QCA1d, has it as well (we cut QTS short, and
		   the original library then hung, waiting for QTS-bar) */
		if (v90tx.seg != V90_ANSPCM)
			toneq_blocks = 0;
		if (v8_ok) {
			/* CM: it missed our QCA1d and went on with V.8
			   (9.2.1.1/V.92); answer that with JM */
			DSP_DBG("v34: CM after QCA1d: V.8\n");
			pcm_tx = 0;
			v90tx.fclock = NULL;
			txm = TXM_ANSAM;
			set_state(V8A_ANSAM);
		} else if (toneq_blocks >= 16) {
			DSP_DBG("v34: TONEq heard\n");
			v90tx.segment(V90_SILENCE);
			/* the analogue modem's INFO0a follows its 75 ms of
			   silence */
			ccr.init(2400);
			set_state(QCA_TONEQ);
		} else if (c >= ms2samples(2000, srate)) {
			DSP_DBG("v34: no TONEq: V.8\n");
			pcm_tx = 0;
			v90tx.fclock = NULL;
			v8_ok = v8_cj = 0;
			v8_sync = 0;
			txm = TXM_ANSAM;
			set_state(V8A_ANSAM);
		}
		break;
	case QCA_TONEQ:
		/* the analogue modem ends TONEq when ANSpcm ends, then 75 ms
		   of silence: ours from then on, so that it hears all of
		   INFO0d */
		if (!toneq_blocks || c >= ms2samples(500, srate)) {
			DSP_DBG("v34: TONEq ended\n");
			set_state(QCA_GAP2);
		}
		break;
	case QCA_GAP2:
		if (c >= ms2samples(75, srate)) {
			/* phase 2 as after V.8 for V.90 */
			pcm_tx = 0;
			pcm = 1;
			DSP_DBG("v34: V.90 with us as the digital modem\n");
			caller = 1;
			tone_g.init(2400, PROBE_PERIOD, srate);
			rt_g.init(2400, PROBE_PERIOD, srate);
			own_g.init(1200, PROBE_PERIOD, srate);
			set_state(V8C_GAP);
			count = c = ms2samples(75, srate);
		}
		break;

	/* V.8 caller */
	case V8C_LISTEN:
		/* for a forced low speed caller: a tone that ends before
		   500 ms (ANSam lasts 2.6 s at least) is ANS, and the
		   answerer's modulation comes at once */
		if (v8_low && ans_gone >= ms2samples(20, srate))
			return v8_back("answer tone ended");
		if (c >= ms2samples(500, srate)) {
			float d = ans_am_depth();
			DSP_DBG("v34: answer tone AM depth %.2f\n", d);
			if (d < 0.1f)
				return fallback("answer tone is not ANSam");
			set_state(V8C_WAIT);
		}
		break;
	case V8C_WAIT:
		if (total >= ms2samples(1000, srate)) {
			set_state(V8C_CM);
			v8_make_msg(&v8tx);
			fsk.init(980, 1180, V34_TONE_AMP, 300, srate, 1);
			txm = TXM_FSK;
		}
		break;
	case V8C_CM:
		/* finish the octet, then CJ */
		if (v8_ok && fsk.edge() && v8tx.pos % 10 == 0) {
			v8_dp = 0;
			if (!v8_remote_v34()) {
				/* CJ, then what the JM has (9.2/V.8) */
				v8_dp = v8_choice();
				if (!v8_dp)
					return fallback("no V.34 in JM");
				DSP_DBG("v34: V.8 agreed on %s\n",
					v8_dp_name(v8_dp));
				set_state(V8C_CJ);
				v8tx.make_cj();
				v8tx_repeat = 0;
				break;
			}
			pcm = v8_v90();
			v91_on = v8_v91();
			if (pcm)
				DSP_DBG("v34: V.90 with us as the digital modem\n");
			set_state(V8C_CJ);
			v8tx.make_cj();
			v8tx_repeat = 0;
			ccr.init(2400);
		} else if (c >= ms2samples(5000, srate))
			return fallback("no JM");
		break;
	case V8C_CJ:
		if (v8tx.pos >= v8tx.n && fsk.edge()) {
			set_state(V8C_GAP);
			txm = TXM_SILENCE;
		}
		break;
	case V91_RUN:           /* process() runs it */
	case V92_RUN:
		break;
	case V8C_GAP:
		/* 75 ms without signal after CJ, then sigC (8.1.2/V.8): the
		   PCTel modem takes CJ only with the gap after it */
		if (c >= ms2samples(75, srate) && v8_dp)
			return v8_handover();
		if (c >= ms2samples(75, srate) && v91_on) {
			v91_start();
			break;
		}
		if (c >= ms2samples(75, srate)) {
			set_state(P2C_INFO0);
			cc.init(srate, 1200, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			make_info0(&cctx);
			cc_send();
		}
		break;

	/* phase 2 answerer */
	case P2A_INFO0:
		if (!cc_busy() && !info0_more())
			set_state(P2A_A);
		break;
	case P2A_A:
		/* in a retrain: once the caller's B has been there a while,
		   and A as long (200 ms, as the original modems): the caller
		   takes a reversal only after it has heard A */
		if (retraining && (rt_blocks < 15 ||
				      c < ms2samples(200, srate))) {
			if (c >= ms2samples(3000, srate)) {
				DSP_DBG("v34: retrain: no tone B\n");
				return DPSTAT_ERROR;
			}
			break;
		}
		/* phase 2's recovery: our INFO0 again */
		if (info0_resend(c))
			break;
		/* the reversal once the caller's tone B is there
		   (11.2.1.2.3/V.34): after the recovery, its INFO0s over */
		if (c >= ms2samples(50, srate) && ev_info0 &&
		    (!info0_recovered ||
		     total - info0_at >= ms2samples(100, srate))) {
			set_state(P2A_ABAR);
			cc_rev = 1;
			ev_rev = 0;
		}
		break;
	case P2A_ABAR:
		/* (the caller's INFO0c again: it has not taken ours) */
		if (!ev_rev && info0_resend(c))
			break;
		if (ev_rev && rev_time > rev_sent) {
			rtd = rev_time - rev_sent - CC_TX_DELAY -
				CC_RX_DELAY - ms2samples(40, srate);
			DSP_DBG("v34: round trip %ld ms\n",
				rtd * 1000 / srate);
			set_state(P2A_ABAR_40);
			count = total - rev_time;
		}
		break;
	case P2A_ABAR_40:
		if (c >= REV_WAIT) {
			set_state(P2A_A_10);
			cc_rev = 1;
		}
		break;
	case P2A_A_10:
		if (rev_on_line() >= ms2samples(10, srate)) {
			set_state(P2A_L1);
			start_probe();
		}
		break;
	case P2A_L1:
		if (c >= ms2samples(160, srate)) {
			set_state(P2A_L2);
			probe_gain = 1;
		}
		break;
	case P2A_L2:
		/* until the caller answers with B */
		if (tone_blocks >= 3 || c >= ms2samples(2000, srate)) {
			set_state(P2A_A_50);
			cc_tone();
		}
		break;
	case P2A_A_50:
		if (c >= ms2samples(50, srate)) {
			set_state(P2A_ABAR_10);
			cc_rev = 1;
		}
		break;
	case P2A_ABAR_10:
		if (rev_on_line() >= ms2samples(10, srate)) {
			set_state(P2A_SILENCE);
			cc_silence();
		}
		break;
	case P2A_SILENCE:
		if (l1_start && total - l1_start >=
		    ms2samples(465, srate)) {
			set_state(P2A_A2);
			cc_tone();
		}
		break;
	case P2A_A2:
		if (ev_info1) {
			choose_bauds();
			set_state(P2A_INFO1);
			make_info1a(&cctx);
			cc_send();
		}
		break;
	case P2A_INFO1:
		if (!cc_busy()) {
			cc_silence();
			set_state(P3A_SILENCE);
			if (primary_start())
				return DPSTAT_ERROR;
		}
		break;

	/* phase 2 caller */
	case P2C_INFO0:
		if (!cc_busy() && !info0_more()) {
			set_state(P2C_B);
			ev_rev = 0;
		}
		break;
	case P2C_B:
		/* phase 2's recovery: our INFO0 again */
		if (info0_resend(c))
			break;
		/* the remote's INFO0a in a retrain: ours, acknowledging
		   it (11.8.1/V.34) */
		if (retraining && rt_info0 == 1) {
			rt_info0 = 2;
			info0_again = 1;
			set_state(P2C_INFO0);
			make_info0(&cctx);
			cc_send();
			break;
		}
		if (retraining && !rt_heard) {
			/* a reversal counts when the tone is there around it
			   (an answerer may reverse A after 200 ms, heard or
			   not); without, it was the remote's data */
			if (ev_rev && total - rev_time >
			    ms2samples(100, srate))
				ev_rev = 0;
			if (c >= ms2samples(3000, srate)) {
				DSP_DBG("v34: retrain: no tone A\n");
				return DPSTAT_ERROR;
			}
			break;
		}
		/* V.92's short phase 2 (9.4.1/V.92): we reverse B once A
		   is there and B has been for 50 ms; no probing, no INFO1d */
		if (short2 && tone_blocks >= 3 &&
		    c >= ms2samples(50, srate)) {
			DSP_DBG("v34: V.92 short phase 2\n");
			set_state(P2C_BBAR_10);
			cc_rev = 1;
			ev_rev = 0;
			break;
		}
		if (ev_info0 && ev_rev && !short2) {
			set_state(P2C_B_40);
			count = total - rev_time;
		}
		break;
	case P2C_B_40:
		if (c >= REV_WAIT) {
			set_state(P2C_BBAR_10);
			cc_rev = 1;
			ev_rev = 0;
		}
		break;
	case P2C_BBAR_10:
		if (rev_on_line() >= ms2samples(10, srate)) {
			set_state(P2C_SILENCE);
			cc_silence();
		}
		break;
	case P2C_SILENCE:
		if (ev_rev && !rtd) {
			rtd = rev_time - rev_sent - CC_TX_DELAY -
				CC_RX_DELAY - ms2samples(40, srate);
			DSP_DBG("v34: round trip %ld ms\n",
				rtd * 1000 / srate);
			if (short2) {
				/* the upstream rate from phase 3's
				   training, not from probing */
				max_rate_rx = 0;
				set_state(P2C_WAIT_INFO1);
				break;
			}
		}
		if (l1_start && total - l1_start >=
		    ms2samples(465, srate)) {
			max_rate_rx = probe_analyse();
			set_state(P2C_B2);
			cc_tone();
			ev_rev = 0;
		}
		break;
	case P2C_B2:
		if (ev_rev) {
			set_state(P2C_B2_40);
			count = total - rev_time;
		}
		break;
	case P2C_B2_40:
		if (c >= REV_WAIT) {
			set_state(P2C_BBAR2_10);
			cc_rev = 1;
		}
		break;
	case P2C_BBAR2_10:
		if (rev_on_line() >= ms2samples(10, srate)) {
			set_state(P2C_L1);
			start_probe();
		}
		break;
	case P2C_L1:
		if (c >= ms2samples(160, srate)) {
			set_state(P2C_L2);
			probe_gain = 1;
		}
		break;
	case P2C_L2:
		if (tone_blocks >= 3 || c >= ms2samples(2000, srate)) {
			set_state(P2C_B3);
			cc_tone();
		}
		break;
	case P2C_B3:
		/* B for two symbols, INFO1c's phase reference: straight
		   after L2 its first bit would be a phase change from our
		   last symbol before L1, and PCTel's answerer does not find
		   the frame (it retrains phase 2 instead of INFO1a) */
		if (c >= 2 * srate / CC_BAUD) {
			set_state(P2C_INFO1);
			make_info1c(&cctx);
			cc_send();
		}
		break;
	case P2C_INFO1:
		if (!cc_busy()) {
			cc_silence();
			set_state(P2C_WAIT_INFO1);
		}
		break;
	case P2C_WAIT_INFO1:
		/* the remote retrains instead of INFO1a (silence, then tone
		   A): an analogue modem without PCM upstream may do so when
		   INFO1d offers it (the original library's "V.92 Lite"); the
		   next phase 2 goes without the offer */
		if (p2_gap == 0 && tone_blocks >= 3)
			p2_gap = 1;
		if (p2_gap == 1 && quiet_blocks >= 8)
			p2_gap = 2;
		if (!ev_info1 && p2_gap == 2 && tone_blocks >= 15) {
			if (v92_on && pcmup_ok) {
				pcmup_ok = 0;
				DSP_DBG("v34: no PCM upstream offer from now "
					"on\n");
			}
			retrain("the remote retrains in phase 2");
			break;
		}
		/* symbol rate codes past the table (6 only for PCM: both
		   for V.92's upstream, downstream for V.90's): the CRC
		   was right, the content isn't */
		if (ev_info1 && !(pcm && rinfo1a.baud_ca == 6 &&
				  rinfo1a.baud_ac == 6) &&
		    (rinfo1a.baud_ac >= V34_NBAUDS ||
		     (rinfo1a.baud_ca >= V34_NBAUDS &&
		      !(pcm && rinfo1a.baud_ca == 6)))) {
			retrain("INFO1a: no such symbol rate");
			break;
		}
		/* PCM upstream (Table 18/V.92); without our offer in
		   INFO1d: a full phase 2, where INFO1d says so */
		if (ev_info1 && pcm && rinfo1a.baud_ca == 6 &&
		    rinfo1a.baud_ac == 6) {
			if (!v92_on || !pcmup_ok) {
				retrain("PCM upstream asked for");
				break;
			}
			v92_start();
			break;
		}
		if (ev_info1 && pcm && rinfo1a.baud_ca == 6) {
			DSP_DBG("v34: INFO1a: V.90, upstream %d baud%s, "
				"UINFO %d, MD %d\n",
				v34_bauds[rinfo1a.baud_ac].nominal,
				short2 && rinfo1a.high ?
				" (says: high carrier)" : "",
				rinfo1a.uinfo, rinfo1a.md);
			if (v90_start())
				return DPSTAT_ERROR;
			break;
		}
		/* what our INFO0c ruled out (only a test's restricts it) */
		if (ev_info1 &&
		    (!v34_info0_allows(info0_rates, rinfo1a.baud_ca,
				       rinfo1a.high) ||
		     !v34_info0_allows(info0_rates, rinfo1a.baud_ac, -1))) {
			retrain("INFO1a: a symbol rate or carrier our INFO0 "
				"ruled out");
			break;
		}
		if (ev_info1) {
			baud_tx = rinfo1a.baud_ca;
			baud_rx = rinfo1a.baud_ac;
			high_tx = rinfo1a.high;
			preemph_tx = rinfo1a.preemph;
			if (rate_of_baud[baud_rx])
				max_rate_rx = rate_of_baud[baud_rx];
			DSP_DBG("v34: symbol rates %d/%d%s\n",
				v34_bauds[baud_tx].nominal,
				v34_bauds[baud_rx].nominal,
				high_tx ? ", we send on the high carrier" : "");
			set_state(P3C_WAIT);
			if (primary_start())
				return DPSTAT_ERROR;
		}
		break;

	/* phase 3 answerer */
	case P3A_SILENCE:
		if (c >= ms2samples(125, srate))
			tx_next(P3A_S, V34_TX_S);
		break;
	case P3A_S:
		if (ptx.n >= 128)
			tx_next(P3A_SBAR, V34_TX_SBAR);
		break;
	case P3A_SBAR:
		if (ptx.n >= 16)
			tx_next(P3A_PP, V34_TX_PP);
		break;
	case P3A_PP:
		if (ptx.n >= 288)
			tx_next(P3A_TRN, V34_TX_TRN);
		break;
	case P3A_TRN:
		if (ptx.n >= TRN3_LEN) {
			tx_next(P3A_J, V34_TX_J);
			prx.ev_s = 0;
		}
		break;
	case P3A_J:
		/* until the caller's S */
		if (prx.ev_s && ptx.n % 8 == 0)
			tx_next(P3A_WAIT, V34_TX_SILENCE);
		break;
	case P3A_WAIT:
		/* until the caller's J */
		if (prx.ev_j)
			tx_next(P4A_S, V34_TX_S);
		break;
	case P4A_S:
		if (ptx.n >= 128)
			tx_next(P4A_SBAR, V34_TX_SBAR);
		break;
	case P4A_SBAR:
		if (ptx.n >= 16) {
			ptx.trn16 = prx.j16;
			tx_next(P4A_TRN, V34_TX_TRN);
		}
		break;
	case P4A_TRN:
		if (ptx.n >= TRN4_LEN)
			mp_start();
		break;

	/* phase 3 caller */
	case P3C_WAIT:
		/* the answerer's training has trained our receiver, and it
		   listens from its J on */
		if (prx.stage == V34_RX_TRN && prx.ev_j)
			tx_next(P3C_S, V34_TX_S);
		break;
	case P3C_S:
		if (ptx.n >= 128)
			tx_next(P3C_SBAR, V34_TX_SBAR);
		break;
	case P3C_SBAR:
		if (ptx.n >= 16)
			tx_next(P3C_PP, V34_TX_PP);
		break;
	case P3C_PP:
		if (ptx.n >= 288)
			tx_next(P3C_TRN, V34_TX_TRN);
		break;
	case P3C_TRN:
		if (ptx.n >= TRN3_LEN) {
			tx_next(P3C_J, V34_TX_J);
			prx.ev_s = 0;
			prx.ev_j = prx.j_count = 0;
		}
		break;
	case P3C_J:
		/* until the answerer's S of phase 4, then J' and TRN */
		if (prx.ev_s && ptx.n % 8 == 0) {
			ptx.trn16 = prx.j16;
			tx_next(P4C_TRN, V34_TX_JP);
		}
		break;
	case P4C_TRN:
		if (ptx.seg == V34_TX_TRN && ptx.n >= TRN4_LEN)
			mp_start();
		break;

	/* V.90 digital modem, phase 3 */
	case P3D_WAIT_JA:
		if (prx.ev_ja) {
			v90tx.dil = prx.dil;
			set_state(P3D_PAUSE);
		}
		break;
	case P3D_PAUSE:
		/* the analogue modem stops Ja when it sees Sd */
		if (c >= ms2samples(50, srate)) {
			set_state(P3D_SD);
			/* the Agere modem trains on TRN1d for 0.6 s and
			   then decides on it for 0.9 s before it looks for
			   Jd; the original library takes 0.3 s */
			v90tx.trn1d_len = 8000;      /* 1 s */
			v90tx.segment(V90_SD);
			prx.ev_s = 0;
		}
		break;
	case P3D_SD:
		if (v90tx.seg == V90_JD) {
			set_state(P3D_JD);
			prx.ev_s = 0;
		}
		break;
	case P3D_JD:
		/* until the remote's S */
		if (prx.ev_s) {
			DSP_DBG("v34: V.90 S after %d Jd\n",
				v90tx.jd_frames);
			v90tx.end_jd();
			set_state(P3D_JDP);
			sbar_seen = prx.sbar_count;
			/* the analogue modem may send TRN1a instead of
			   silence while it takes in the DIL */
			prx.s_in_trn = 1;
		}
		break;
	case P3D_JDP:
		/* S-bar answers Jd'; with DIL a second S to S-bar ends it */
		if (v90tx.next && !v90tx.dil.n)
			set_state(P4D_START);
		else if (v90tx.seg == V90_DIL &&
			 prx.sbar_count >= sbar_seen + 2) {
			DSP_DBG("v34: V.90 DIL ends after %d rounds\n",
				v90tx.dil_rounds);
			prx.s_in_trn = 0;
			v90tx.end_dil();
			set_state(P3D_DIL_END);
		}
		break;
	case P3D_DIL_END:
		if (v90tx.next)
			set_state(P4D_START);
		break;
	case P4D_START:
		/* Ri until CPt */
		v90tx.segment(V90_RI);
		set_state(P4D_RI);
		break;
	case P4D_RI:
		if (prx.ev_cpt && v90tx.n >= 192 && !v90tx.ri_end) {
			cpt.parse(prx.cpraw,
				     v90_cp_length(prx.cpraw, 129));
			D = cpt.drn + 8;
			if (v90tx.enc.init(&cpt, D,
					 v90tx.lin)) {
				DSP_DBG("v34: V.90 CPt does not fit\n");
				return DPSTAT_ERROR;
			}
			v90tx.get_frame = pcm_frame_cb;
			v90tx.ctx = this;
			fmode = PCM_TRN;
			fcount = 0;
			ack_sent = 0;
			v90tx.end_ri();
			DSP_DBG("v34: V.90 CPt: D %d, K %d, M %d %d %d %d %d "
				"%d, Sr %d, ld %d\n", D, v90tx.enc.K,
				v90tx.enc.M[0], v90tx.enc.M[1],
				v90tx.enc.M[2], v90tx.enc.M[3],
				v90tx.enc.M[4], v90tx.enc.M[5],
				cpt.sr, cpt.ld);
			{
				char buf[400];
				int i, n = 0;
				for (i = 0; i < v90tx.enc.M[0] && n < 380; i++)
					n += sprintf(buf + n, " %d",
						     v90tx.enc.ucode[0][i]);
				DSP_DBG("v34: V.90 CPt ucodes%s; shaper a1 %d a2 %d b1 %d b2 %d\n",
					buf, cpt.a1, cpt.a2, cpt.b1, cpt.b2);
			}
			set_state(P4D_TRAIN);
		}
		break;
	case P4D_TRAIN:
		/* a renegotiation the analogue modem started: Rd after its
		   S-bar */
		if (v90_rd_wait && prx.sbar_count != rn_sbar) {
			v90_rd_wait = 0;
			v90_rd_start();
		}
		/* after the silence it asked for: CP (bit 30 clear), then
		   Rt (of CPt's constellations), Rt-bar, MP
		   (9.6.1.2.6/V.90) */
		if (fmode == PCM_SIL && v90tx.seg == V90_SILENCE &&
		    prx.cp_count > sil_cp && !prx.cp_silence &&
		    v90tx.sym % 6 == 0) {
			int k;
			DSP_DBG("v34: V.90 Rt\n");
			for (k = 0; k < 6; k++)
				v90tx.rd_code[k] = v90tx.enc.ucode[k][0];
			v90tx.segment(V90_RD);
			v90_rn_init = 1;
			prx.ev_cpack = prx.ev_e = 0;
			up_n = 0;
			ack_sent = 0;
			fmode = PCM_SEQ;
			fcount = 0;
			seq_len = 0;
			v90_make_mp(1);
			timeout = total + ms2samples(5500, srate) + 3 * rtd;
		}
		if (prx.ev_cp && !up_n) {
			cp.parse(prx.cpraw,
				     v90_cp_length(prx.cpraw, 129));
			if (v90_data_setup()) {
				/* in a call: not V.32bis now */
				if (connected) {
					retrain("no common upstream rate");
					break;
				}
				return fallback("no common upstream rate");
			}
		}
		if (fmode == PCM_DATA && prx.ev_e) {
			set_state(V34_DATA);
			set_rates(rate_tx, rate_rx);
			rt_blocks = 0;
			lost = 0;
			if (renegotiating) {
				renegotiating = 0;
				prx.ev_reneg = 0;
				DSP_DBG("v34: V.90 renegotiated %d/%d\n",
					rate_tx, rate_rx);
				if (rate_rx / 2400 < max_rate_rx)
					rn_max = rate_rx / 2400;
				break;
			}
			rn_max = rate_rx / 2400;
			if (retraining && connected) {
				retraining = 0;
				DSP_DBG("v34: V.90 %s, %d/%d\n", moh_end ?
					"back from hold" : "retrained",
					rate_tx, rate_rx);
				moh_end = 0;
				break;
			}
			retraining = 0;
			connected = 1;
			DSP_DBG("v34: V.90 connect %d/%d\n", rate_tx,
				rate_rx);
			return DPSTAT_CONNECT;
		}
		break;

	/* phase 4, both */
	case P4_MP:
		/* a cleardown (11.7/V.34): MP with no rates */
		if (renegotiating && prx.ev_mp && !prx.mp.rate_ca &&
		    !prx.mp.rate_ac) {
			DSP_DBG("v34: cleardown by the remote\n");
			return DPSTAT_ERROR;
		}
		if (prx.ev_mp && !ptx.ack_mp) {
			/* before the remote's E: its data follows at once */
			if (data_setup()) {
				/* in a call: not V.32bis now */
				if (connected) {
					retrain("no common data rate");
					break;
				}
				return fallback("no common data rate");
			}
			ptx.ack_mp = 1;
		}
		/* E after a complete MP' once the remote has acknowledged
		   ours */
		if (prx.ev_mp && prx.mp.ack && ptx.mp_sent >= 1 &&
		    ptx.mp.ack && ptx.pos >= ptx.nbits)
			tx_next(P4_E, V34_TX_E);
		break;
	case P4_E:
		if (ptx.seg == V34_TX_DATA && prx.ev_e) {
			set_state(V34_DATA);
			set_rates(rate_tx, rate_rx);
			rt_blocks = 0;
			lost = 0;
			if (renegotiating) {
				renegotiating = 0;
				/* the remote's answering S, or its own */
				prx.ev_reneg = 0;
				DSP_DBG("v34: renegotiated, %d/%d\n", rate_tx,
					rate_rx);
				/* the remote does not let us go as high */
				if (rate_rx / 2400 < max_rate_rx)
					rn_max = rate_rx / 2400;
				break;
			}
			/* a renegotiation may go back up to this */
			rn_max = rate_rx / 2400;
			if (retraining && connected) {
				/* the link goes on (V.42 knows nothing of it) */
				retraining = 0;
				DSP_DBG("v34: %s, %d/%d\n", moh_end ?
					"back from hold" : "retrained",
					rate_tx, rate_rx);
				moh_end = 0;
				break;
			}
			retraining = 0;
			connected = 1;
			DSP_DBG("v34: connect %d/%d\n", rate_tx, rate_rx);
			return DPSTAT_CONNECT;
		}
		break;
	case V34_DATA:
		if (prx.on)
			lost = 0;
		else
			lost++;
		if (pcm) {
			if (rt_blocks >= 6) {
				retrain("tone A");
				break;
			}
			if (lost >= carrier_loss) {
				DSP_DBG("v34: carrier lost\n");
				return DPSTAT_ERROR;
			}
		}
		/* the data far worse than its rate needs (the mse is in
		   units of the signal points' spacing, the same for all
		   rates) */
		if (prx.data_on && prx.mse > V34_RETRAIN_MSE)
			bad++;
		else
			bad = 0;
		/* the mse for a lower rate, or one that leaves room for a
		   higher one */
		if (prx.data_on) {
			float step = 3 * 2400.0f / v34_symbol_rate(prx.baud);
			if (c < ms2samples(1000, srate))
				mse_good = 0;
			else if (!mse_good)
				mse_good = prx.mse;
			else if (prx.mse < V34_RN_DOWN)
				mse_good += 1e-4f * (prx.mse - mse_good);
			rn_hi = prx.mse > V34_RN_DOWN ? rn_hi + 1 : 0;
			rn_lo = rate_rx / 2400 < rn_max &&
				prx.mse * powf(10, step / 10) < V34_RN_UP ?
				rn_lo + 1 : 0;
		}
		if (rt_blocks >= 6)
			retrain(caller ? "tone A" : "tone B");
		else if (prx.ev_reneg) {
			prx.ev_reneg = 0;
			if (pcm)
				v90_renegotiate("the remote's S",
						reneg_rate(0), 1);
			else
				renegotiate("the remote's S", reneg_rate(0));
		} else if (rn_hi >= ms2samples(300, srate) &&
			   c >= ms2samples(2000, srate) && rate_rx > 2400) {
			/* the line got much worse (6 dB and more): both ways,
			   most likely; a retrain probes it anew, as it does
			   when there is no lower rate at this symbol rate */
			if (prx.mse > 4 * mse_good ||
			    rate_rx / 2400 <= rate_floor())
				retrain("errors");
			else if (pcm)
				v90_renegotiate("errors", reneg_rate(-1), 0);
			else
				renegotiate("errors", reneg_rate(-1));
		}
		else if (rn_lo >= ms2samples(10000, srate)) {
			if (pcm)
				v90_renegotiate("margin", reneg_rate(1), 0);
			else
				renegotiate("margin", reneg_rate(1));
		} else if (reneg_at && c >= reneg_at && !renegs) {
			if (pcm)
				v90_renegotiate("test", reneg_to, 0);
			else
				renegotiate("test", reneg_to);
		} else if (pcm)
			/* (carrier loss above) */
			;
		else if (bad >= ms2samples(1000, srate) &&
			 c >= ms2samples(5000, srate))
			/* (not in a loop on a line that stays marginal) */
			retrain("errors");
		else if (lost >= ms2samples(30, srate))
			retrain("signal lost");
		else if (retrain_at && c >= retrain_at && !retrains)
			retrain("test");
		break;
	case V34_RN_S:
		if (ptx.n >= 128)
			tx_next(V34_RN_SBAR, V34_TX_SBAR);
		break;
	case V34_RN_SBAR:
		if (ptx.n >= 16)
			tx_next(V34_RN_TRN, V34_TX_TRN);
		break;
	case V34_RN_TRN:
		/* MP once the remote's training has been there a while
		   (both started theirs), at most 2 s */
		if ((ptx.n >= 512 && prx.stage == V34_RX_TRN && prx.n >= 64) ||
		    c >= ms2samples(2000, srate))
			mp_start();
		break;
	case MOH_REQ:
		/* tone B for 600 ms (50 at least, 9.10.1/V.92: longer, so
		   that a remote that answers it with a retrain's reversal
		   has done so before the sequence, whose phase steps it
		   would take for our reversal), then the sequence,
		   repeated, until the answer (9.10.1.1/V.92) */
		if (c >= ms2samples(600, srate) && !cc_busy()) {
			cctx.make_mh(moh_req, moh_req);
			cc_send();
		}
		if (moh_req == MH_REQ) {
			/* the remote took our tone for a retrain's (it may,
			   9.10.1.1/V.92): its tone A reversed, or its INFO0a.
			   That retrain then, at once (it waits for our
			   reversal): phase 2 from tone B */
			if (!rt_heard)
				/* (one before its tone is the data's end) */
				ev_rev = 0;
			if (ev_rev || rt_info0 == 1) {
				DSP_DBG("v34: the remote retrains instead of "
					"answering MHreq\n");
				moh_req = 0;
				set_state(P2C_B);
				cc_tone();
				break;
			}
			/* none in 2 s and a round trip: retrain */
			if (c >= ms2samples(600 + 2500, srate)) {
				DSP_DBG("v34: no answer to MHreq\n");
				retrain("no answer to MHreq");
			}
		} else if (moh_req == MH_CDA) {
			/* (what the remote asked for: once it is out) */
			if (c >= ms2samples(500, srate)) {
				DSP_DBG("v34: cleared down\n");
				return DPSTAT_ERROR;
			}
		} else if (c >= ms2samples(300, srate)) {
			/* MHfrr: then ANSam (9.10.2.3/V.92), phase 1 as
			   the answering modem */
			moh_resp = MH_NACK;
			moh_hold(10);
		}
		break;
	case MOH_HELD:
		/* silent until we take the remote back, within its T1:
		   ANSam, and phase 1 as after its own hold */
		if (c >= ms2samples(1000L * moh_req_hold, srate) ||
		    (moh_t1_secs(moh_req_t1) && moh_req_t1 != 13 &&
		     c >= ms2samples(1000L * moh_t1_secs(moh_req_t1) - 2000,
				     srate))) {
			moh_resp = MH_ACK;
			moh_hold(moh_req_t1 == 13 ? 0 :
				 moh_t1_secs(moh_req_t1));
		}
		break;
	case MOH_RESP:
		/* repeated, each right after the one before (9.10.1/V.92) */
		if (!cc_busy()) {
			cctx.make_mh(moh_resp,
				     moh_resp == MH_ACK ? moh_t1 : moh_resp);
			cc_send();
		}
		if (moh_drop)
			return DPSTAT_ERROR;
		switch (moh_resp) {
		case MH_ACK:
			/* then its tone for 100 ms or silence for 2 s: on hold
			   (9.10.2.1/V.92) */
			if (rt_blocks >= 15 || quiet_blocks >= 300)
				moh_hold(moh_t1 == 13 ? 0 :
					 moh_t1_secs(moh_t1));
			else if (total - mh_seen >= ms2samples(35000, srate))
				return DPSTAT_ERROR;
			break;
		case MH_CDA:
			/* until its tone, silence, or no MHclrd for 200 ms
			   (9.10.2.2/V.92) */
			if (rt_blocks >= 15 || quiet_blocks >= 30 ||
			    total - mh_seen >= ms2samples(200, srate)) {
				DSP_DBG("v34: cleared down\n");
				return DPSTAT_ERROR;
			}
			break;
		case MH_NACK:
			/* MHcda or MHfrr within 10 s */
			if (c >= ms2samples(12000, srate)) {
				DSP_DBG("v34: nothing after MHnack\n");
				return DPSTAT_ERROR;
			}
			break;
		}
		break;
	case V34_RT_SILENCE:
		if (c >= ms2samples(70, srate)) {
			set_state(caller ? P2C_B : P2A_A);
			cc_tone();
		}
		break;
	}
	return DPSTAT_OK;
}

/* the next sample out at 9600 Hz, of what txm says */
float V34Pump::pump_tx_sample()
{
	float y = 0;

	switch (txm) {
	case TXM_SILENCE:
		break;
	case TXM_ANSAM:
		/* 15 Hz, 20 % amplitude modulation, reversals every 450 ms */
		if (count % ms2samples(450, srate) == 0 && count)
			ansam_sign = -ansam_sign;
		y = ansam_sign * ANSAM_AMP * (1 + 0.2f * ansam_am.next()) *
			ansam.next();
		break;
	case TXM_FSK:
		y = fsk.sample([this] {
			if (v8tx.pos >= v8tx.n && v8tx_repeat)
				v8tx.pos = 0;
			return v8tx.pos < v8tx.n ? (int)v8tx.b[v8tx.pos++] : 1;
		});
		break;
	case TXM_CC:
		y = cc.sample([this] { return cc_symbol(this); });
		break;
	case TXM_PROBE:
		y = probe_gain * probe[probe_pos];
		if (++probe_pos == PROBE_PERIOD)
			probe_pos = 0;
		break;
	case TXM_PRIMARY:
		y = ptx.sample();
		break;
	}
	return y;
}

/* one sample at 9600 Hz each way; ref: what goes out on the line instead
   (the PCM signal, for the echo canceller) */
int V34Pump::sample(float x, float *y, const float *ref)
{
	int r = handshake();
	if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
		return r;
	*y = dsp_clip16(pump_tx_sample());
	last_tx = ref ? *ref : *y;
	pump_rx_sample(x);
	if (renegotiating && total >= timeout)
		retrain("renegotiation timed out");
	if (state != V34_DATA && total >= timeout) {
		DSP_DBG("v34: start-up timeout in state %d\n", state);
		return DPSTAT_ERROR;
	}
	count++;
	total++;
	return r;
}

/* cnt line samples each way (s16 at line_rate): V.91 and V.92's PCM
   upstream at 8000 Hz; the rest at 9600 Hz, through the converters on
   an 8000 Hz line (V.90's PCM codes go out as they are); returns
   DPSTAT_OK or DPSTAT_CONNECT, or DPSTAT_CHANGEDP / DPSTAT_ERROR at
   once */
int V34Pump::process(void *in, void *out, int cnt)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int ret = DPSTAT_OK, r, i, k, n;
	float y, up[2], down[2], pcm_ = 0;

	/* silence where an early return (V.8 done, another pump, an error)
	   leaves the block unwritten */
	memset(dst, 0, cnt * sizeof(*dst));
	for (i = 0; i < cnt; i++) {
		if (state == V91_RUN) {
			int y91, ev = v91m.sample(src[i], &y91);
			dst[i] = (s16)y91;
			if (ev == V91_CONNECT) {
				rate_tx = (v91m.cp_rx.drn + 20) * 8000 / 6;
				rate_rx = (v91m.our_drn + 20) * 8000 / 6;
				set_rates(rate_tx, rate_rx);
				DSP_DBG("v34: V.91 connect %d/%d\n", rate_tx,
					rate_rx);
				ret = DPSTAT_CONNECT;
			} else if (ev == V91_FAIL)
				return DPSTAT_ERROR;
			else if (ev == V91_ESCAPE)
				v91_escape();
			continue;
		}
		/* (handshake() does not run here; ours leaves V92_RUN) */
		if (state == V92_RUN)
			moh_req_test();
		if (state == V92_RUN) {
			/* the echo of our signal from the analogue modem's
			   hybrid, after the network's round trip */
			float x92 = src[i];
			int y92, ev;
			ec_schedule();
			if (ec.on && !ec.bypass)
				x92 = ec.run(last_tx, x92);
			/* for the PCM upstream design: what the codec
			   quantizes with the analogue modem's signal */
			v92u.pf_echo = ec.on && !ec.bypass && ec.judged ?
				sqrtf(ec.pw_est) : 0;
			/* and what the canceller leaves of it */
			v92u.pf_echo_res = v92u.pf_echo *
				sqrtf((ec.pw_err + 1) / (ec.pw_rx + 1));
			ev = v92u.sample((int)lrintf(x92), &y92);
			last_tx = (float)y92;
			dst[i] = (s16)y92;
			out_n++;
			if (ev == V92_CONNECT) {
				rate_tx = (v92u.cpu.drn + 20) * 8000 / 6;
				rate_rx = (v92u.cpd.drn + 17) * 8000 / 6;
				set_rates(rate_tx, rate_rx);
				if (retraining && connected) {
					/* after a retrain, or back from hold:
					   the link goes on */
					DSP_DBG("v34: V.92 %s, %d/%d\n",
						moh_end ? "back from hold" :
						"retrained", rate_tx, rate_rx);
				} else {
					DSP_DBG("v34: V.92 connect %d/%d\n",
						rate_tx, rate_rx);
					ret = DPSTAT_CONNECT;
				}
				retraining = 0;
				connected = 1;
				moh_end = 0;
			} else if (ev == V92_RENEG) {
				rate_tx = (v92u.cpu.drn + 20) * 8000 / 6;
				rate_rx = (v92u.cpd.drn + 17) * 8000 / 6;
				set_rates(rate_tx, rate_rx);
				DSP_DBG("v34: V.92 renegotiated %d/%d\n",
					rate_tx, rate_rx);
			} else if (ev == V92_FAIL || ev == V92_RETRAIN) {
				/* before data, V.34 upstream next time; a
				   renegotiation that fails retrains */
				if (!v92u.connected) {
					pcmup_ok = 0;
					DSP_DBG("v34: no PCM upstream offer "
						"from now on\n");
				}
				if (rt)
					rt->pcm = 0;
				retrain(ev == V92_FAIL ?
					"V.92 PCM upstream failed" : "tone A");
				if (ev == V92_RETRAIN)
					rt_heard = 1;
			}
			continue;
		}
		if (line_rate == V34_SRATE) {
			r = sample(src[i], &y, NULL);
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
			if (r != DPSTAT_OK)
				ret = r;
			dst[i] = (s16)y;
			continue;
		}
		/* an 8000 Hz line: the pump runs at 9600 Hz in between */
		if (pcm_tx) {
			/* V.90: exact PCM codes at 8000 Hz; at 9600 Hz for the
			   echo canceller */
			float ref[2];
			int m;
			pcm_ = v90tx.sample();
			m = rs_ref.run(pcm_, ref);
			for (k = 0; k < m && ref_n < V34_FIFO; k++)
				ref_fifo[ref_n++] = ref[k];
		}
		n = rs_up.run(src[i], up);
		for (k = 0; k < n; k++) {
			float rf = 0;
			if (pcm_tx && ref_n) {
				rf = ref_fifo[0];
				memmove(ref_fifo, ref_fifo + 1,
					--ref_n * sizeof(float));
			}
			r = sample(up[k], &y, pcm_tx ? &rf : NULL);
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
			if (r != DPSTAT_OK)
				ret = r;
			if (rs_down.run(y, down) &&
			    fifo_n < V34_FIFO)
				fifo[fifo_n++] = down[0];
		}
		y = 0;
		if (fifo_n) {
			y = fifo[0];
			memmove(fifo, fifo + 1,
				--fifo_n * sizeof(fifo[0]));
		}
		if (pcm_tx)
			y = pcm_;
		dst[i] = dsp_clip16(y);
		out_n++;
	}
	return ret;
}

/* PumpOps' create: what V.8 offers for id (V.90, V.92 and V.91 only
   at 8000 Hz), the DSP_* test settings, S7 and S10; srate_ 9600, or
   8000 through converters (-1 for any other) */
int V34Pump::start(struct modem *m, enum DP_ID id, int caller_, int srate_, int max_frag)
{
	if (srate_ != V34_SRATE && srate_ != 8000) {
		DSP_DBG("v34: sample rate %d not supported\n", srate_);
		return -1;
	}
	/* at 8000 Hz through sample rate converters */
	line_rate = srate_;
	srate_ = V34_SRATE;
	rs_up.init(6, 5);
	rs_down.init(5, 6);
	rs_ref.init(6, 5);
	fifo_n = 2;          /* so that it never runs dry */
	/* a V.90 digital modem needs the 8000 Hz G.711 network */
	v90 = (id == DP_V90 || id == DP_V92) && line_rate == 8000;
	/* V.92 (the digital modem's side): DSP_V92=0 leaves it out */
	v92 = id == DP_V92 && v90 &&
		!(getenv("DSP_V92") && !atoi(getenv("DSP_V92")));
	qc_n = -1;
	short2_ok = !(getenv("DSP_V92_SHORT2") &&
		      !atoi(getenv("DSP_V92_SHORT2")));
	/* quick connect: DSP_V92_QC=0 answers QC1a with V.8 instead */
	qc_ok = !(getenv("DSP_V92_QC") && !atoi(getenv("DSP_V92_QC")));
	/* modem-on-hold: the T1 code (Table 33/V.92) we grant, +PMHT's
	   (0 with +PMH=1), or DSP_V92_MOH=<code> (1 10 s ... 12 16 min, 13
	   no limit; 0 denies it); 1 min if neither says */
	moh_t1 = getenv("DSP_V92_MOH") ? atoi(getenv("DSP_V92_MOH")) & 15 :
		(int)modem_get_param(m, MDMPRM_MOH_T1);
	if (getenv("DSP_V92_MOH_REQ")) {
		const char *e = getenv("DSP_V92_MOH_REQ");
		moh_req_at = ms2samples(atoi(e), srate_);
		moh_req_hold = strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 10;
	}
	if (moh_t1 < 0 || moh_t1 > 13)
		moh_t1 = 5;
	ev_mh = moh_resp = moh_drop = 0;
	moh_end = 0;
	/* PCM upstream: DSP_V92_PCMUP=0 does not offer it */
	pcmup_ok = !(getenv("DSP_V92_PCMUP") &&
		     !atoi(getenv("DSP_V92_PCMUP")));
	/* V.91 with another digital modem: DSP_V91=0 leaves it out */
	v91 = (id == DP_V90 || id == DP_V92 || id == DP_V91) &&
		line_rate == 8000 &&
		!(getenv("DSP_V91") && !atoi(getenv("DSP_V91")));
	/* the G.711 law of the network (D-Modem knows it from the codec of
	   the call) */
	pump_setup(m, srate_, caller_);
	alaw = rt && rt->law ? rt->law == 2 :
		getenv("DSP_G711") && !strcmp(getenv("DSP_G711"), "alaw");
	v8_low = caller_ && rt ? rt->v8_for : 0;
	if (v8_low)
		v90 = v91 = v92 = 0;

	ec.init();
	ansam.set_freq(2100, srate_);
	ansam_am.set_freq(15, srate_);
	ansam_sign = 1;
	ans_lo.set_freq(-2100, srate_);
	aa_g.init(1800, srate_ / 50, srate_);
	tone_g.init(caller_ ? 2400 : 1200, PROBE_PERIOD, srate_);
	rt_g.init(caller_ ? 2400 : 1200, PROBE_PERIOD, srate_);
	own_g.init(caller_ ? 1200 : 2400, PROBE_PERIOD, srate_);
	/* test: DSP_V34_RETRAIN=<ms in data mode>[a|c], c or a: only the
	   caller or the answerer starts it */
	retrain_at = test_samples("DSP_V34_RETRAIN");
	/* test: DSP_V34_PRECODE_MIN=<dB>: ask for precoding from that gain
	   on (V34_PRECODE_MIN) */
	precode_min = getenv("DSP_V34_PRECODE_MIN") ?
		(float)atof(getenv("DSP_V34_PRECODE_MIN")) : V34_PRECODE_MIN;
	/* test: DSP_V34_INFO0=<hex>[,a|,c]: our INFO0's bits 12:19 (what
	   it offers of the symbol rates and carriers), on both sides or
	   only the answerer's or the caller's */
	info0_rates = 0xFF;
	if (getenv("DSP_V34_INFO0")) {
		const char *e = getenv("DSP_V34_INFO0");
		char *side;
		long v = strtol(e, &side, 16);
		if (*side == ',')
			side++;
		if (!*side || *side == (caller_ ? 'c' : 'a'))
			info0_rates = (int)(v & 0xFF);
	}
	/* test: DSP_V34_LOSE_INFO0=[a|c]: our first INFO0 with a bad CRC,
	   lost to the remote (phase 2's recovery, 11.2.2/V.34), only the
	   answerer's or the caller's (anything else: either) */
	lose_info0 = pump_test_env("DSP_V34_LOSE_INFO0", caller_) != NULL;
	/* test: DSP_V34_RENEG=<ms in data mode>:<N>[a|c]: a rate
	   renegotiation to N * 2400 bit/s (what we receive) */
	if (const char *e = pump_test_env("DSP_V34_RENEG", caller_)) {
		int n = strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
		if (n > 0) {
			reneg_at = ms2samples(atoi(e), srate_);
			reneg_to = n;
		}
	}
	probe_g[0].init(1050, PROBE_PERIOD, srate_);
	probe_g[1].init(3000, PROBE_PERIOD, srate_);
	probe_init();
	if (caller_) {
		/* the answer tone was heard: CM goes back on V.21 channel 1,
		   JM comes on channel 2 */
		v21.init(1650, 1850, 300, srate_);
		state = V8C_LISTEN;
	} else {
		v21.init(980, 1180, 300, srate_);
		state = V8A_SILENCE;
	}
	txm = TXM_SILENCE;
	DSP_DBG("v34: create %s (dp %d)\n", caller_ ? "caller" : "answerer", id);
	return 0;
}

/* frees what the receiver holds; PCM off for the next pump */
void V34Pump::cleanup()
{
	if (rt)
		rt->pcm = 0;
	if (prx_on)
		prx.release();
}

/* DP_V8 is what the modem core starts for the default (fastest) mode;
   V.90 (the digital modem) and V.91 run from V.8 on this pump too, and
   V.92 starts it as well (V.90 or V.34) */
static const enum DP_ID v34_ids[] = { DP_V8, DP_V34, DP_V90, DP_V91, DP_V92 };

/* this pump for v34_ids */
int dp_v34_init(void)
{
	int i;
	for (i = 0; i < (int)(sizeof(v34_ids) / sizeof(v34_ids[0])); i++)
		modem_dp_register(v34_ids[i], &PumpOps<V34Pump>::ops);
	return 0;
}

/* and no more */
void dp_v34_exit(void)
{
	int i;
	for (i = 0; i < (int)(sizeof(v34_ids) / sizeof(v34_ids[0])); i++)
		modem_dp_deregister(v34_ids[i], &PumpOps<V34Pump>::ops);
}

} // namespace dsp
