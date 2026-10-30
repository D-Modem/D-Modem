/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "dm_types.h"
#include "pump.h"
#include "v34.h"
#include "v90.h"
#include "v90a.h"
#include "v91.h"
#include "v92.h"
#include "v92a.h"
#include "vcvm_startup.h"

namespace dsp {

#define V34_SRATE       9600
#define CC_BAUD         600
#define CC_SPS          16      /* samples per control channel symbol */
/* the original library's tones, INFO sequences and L2: about 2990 rms */
#define V34_LEVEL       2988.0f
#define V34_TONE_AMP    (V34_LEVEL * 1.41421f)
/* V.92's PCM upstream LU: about our V.34 signal's power */
#define V92A_LU         3000.0f
#define ANSAM_AMP       3200.0f
#define PROBE_PERIOD    64      /* L1/L2 repeat at 150 Hz */
#define TRN3_LEN        5120    /* TRN symbols in phase 3 */
#define TRN4_LEN        5120    /* and in phase 4 (about 1.5 s, as the original) */
/* a reversal reaches the line this long after we decide on it (the
   pulse shaping), and we see the remote's reversal this long after it
   was on the line: answers that must follow 40 ms after a reversal
   wait the rest */
#define CC_TX_DELAY     (4 * CC_SPS)
#define CC_RX_DELAY     (13 * static_cast<s32>(V34_SRATE) / 1000)
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
#define V8_CAT_CALLF    0x01    /* the call function (Table 3/V.8) */
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

/* a transmit / receive pair of bit rates, for the log */
#define RATE_FMT        "%" PRId32 "/%" PRId32

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
	/* V.92 short phase 1, we the analogue modem calling (9.2.1.1/V.92):
	   QC1a ahead of CM; after the digital modem's QCA1d silence until
	   its ANSpcm, TONEq until ANSpcm ends, 75 ms, phase 2 */
	QCC_QC1A,
	QCC_WAIT,
	QCC_TONEQ,
	QCC_GAP,
	MOH_BACK,               /* back from holding the remote: its ANSam,
				   then V.8 as the call modem */
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
	/* V.92's short phase 2 as the analogue modem (9.4.2/V.92): our A
	   reversed 40 ms after its B's, 10 ms more of it, then INFO1a */
	P2A_S2_40,
	P2A_S2_10,
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
	/* V.90 analogue modem, phase 3 (9.3.2/V.90) */
	P3V_SILENCE,    /* 70 ms after INFO1a */
	P3V_S,
	P3V_SBAR,
	P3V_PP,
	P3V_TRN,
	P3V_JA,         /* Ja until Sd-bar */
	P3V_WAIT_JD,    /* silence until Jd */
	P3V_S2,         /* S until Jd' */
	P3V_SBAR2,
	P3V_DIL,        /* silence while the DIL is measured */
	P3V_S3,         /* S, S-bar: enough of it */
	P3V_SBAR3,
	P4V_CPT,        /* CPt until Ri-bar */
	P4V_SCR,        /* SCR while TRN2d shows the noise */
	P4V_CP,         /* CP until MP */
	P4V_CPACK,      /* CP' until MP' or Ed */
	P4V_E,          /* E, then ones until B1d is in */
	V90A_DATA,      /* the analogue modem's data mode */
	V90A_RN_S,      /* a rate renegotiation: S 128 T (9.6.2/V.90) */
	V90A_RN_SBAR,   /* S-bar 16 T, then SCR and phase 4's CP ... */
	/* V.92 analogue modem, PCM upstream phase 3 (9.5.2/V.92): the
	   PAM transmitter's segments follow each other (v92a.h), these
	   wait for what moves them on */
	P3U_SILENCE,    /* 70 ms after INFO1a */
	P3U_JA,         /* Ru, Ru-bar, TRN1u, Ja until Sd-bar */
	P3U_WAIT_JD,    /* silence until Jd */
	P3U_SU,         /* Su, Su-bar, Su until Jp */
	P3U_TRN1,       /* Su-bar, TRN1u while the DIL is measured */
	P3U_CPT,        /* CPt until Ri */
	P3U_E1U,
	/* phase 4 (9.6.2/V.92) */
	P4U_TRN2,       /* TRN2u until SUVd (or 12000 T) */
	P4U_SEQ,        /* SUVu, CPu after SUVd, the acknowledged ones
			   after CPd, until SUVd' or CPd' or Ed */
	P4U_DATA,       /* E2u, B1u, data; until B1d */
	P4U_RN,         /* a rate renegotiation (9.8.2/V.92): Ru, Ru-bar,
			   then P4U_TRN2 */
	P4U_FSEQ,       /* a fast parameter exchange (9.9.2.2/V.92): RM,
			   RM', SUVu, CPus, ... in the data mode */
	P4U_FDATA,      /* E2u, FB1u, B1u; until B1d */
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
	TXM_PAM,        /* V.92's PCM upstream (patx) */
};

struct BitQ {
	unsigned char b[256];
	s16 n, pos;

	void reset();
	void put(u32 val, s16 nbits);
	void info_start();
	void info_end();
	void put_octet(unsigned char c);
	void make_cj();
	void make_mh(s16 sig, s16 info);
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
	s16 pos;
	float phase_e[CC_SPS];
	s16 best;
	s32 n, last;            /* samples, at the last decision */
	float dmin;
	float rmin;             /* the most reversed step of a steady tone */
	s16 rmin_pos;           /* at this sampling phase */
	s16 snap;               /* set the timing at a tone reversal */
	s16 rbit;               /* a tone reversal in the last symbol */
	s16 ones;
	float p;                /* |b|^2 of the last symbol */
	s16 on;
	s32 on_count;
	s32 zeros;              /* '0' decisions in a row (tone) */
	u32 sr;                 /* last bits, newest in bit 0 */
	s32 rev_cand;           /* a 1 after a tone: a reversal if 0 follows */
	s16 frame_len;          /* > 0: within an INFO frame */
	unsigned char frame[128];
	unsigned char hist[CC_HIST];
	s32 hist_n;
	/* the decisions at every sampling phase, for INFO frames whatever
	   the symbol timing does */
	unsigned char ph[CC_SPS][CC_HIST];
	s32 ph_n[CC_SPS];
	s16 ph_last;            /* the phase of the last sample */

	void init(float fc);
	s16 sample(float x);
};

struct Info0 {
	s16 rates;              /* bit 0..: 2743, 2800, 3429, 3000L, 3000H, 3200L, 3200H */
	s16 allow_3429;
	s16 power_red;
	s16 max_diff;
	s16 cme;
	s16 c1664;
	s16 clock;
	s16 ack;
	s16 v92, short2;        /* INFO0a (Table 16/V.92): V.92, and short
				   phase 2 asked for */
	/* INFO0d (Table 7/V.90), the digital modem's: its phase 2 power
	   (-6 - n dBm0), its most (-0.5 - n / 2 dBm0), measured at the
	   codec's output, the network's law (1: A-law), 3429 upstream */
	s16 p2_power, max_power, codec_power, law, up3429;

	void parse(const unsigned char *b);
	void parse_d(const unsigned char *b);
};

struct Info1c {
	s16 power_red, add_power_red, md;
	struct { s16 high, preemph, max_rate; } r[V34_NBAUDS];
	s16 freq_offset;

	void parse(const unsigned char *b);
};

struct Info1a {
	s16 power_red, add_power_red, md;
	s16 high, preemph, max_rate;
	s16 baud_ac, baud_ca;   /* baud_ca 6: V.90 */
	s16 uinfo;              /* V.90: the Ucode of the 2 point train */
	s16 freq_offset;

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
	s16 pos, rpos;
	s16 on, far_on, far_delay, trained;
	s16 judged, bypass;     /* the echo was measured; none: off */
	/* the caller: the training held for the remote's J */
	s16 waited;
	s32 n, since, filled, far_since;
	float mu, dbg_rx, dbg_err, dbg_tx, dbg_est;
	float pw_rx, pw_err;    /* in and out while training */
	float es;               /* the error's short term power */
	float pw_est;           /* the echo taken away */
	float pw_near, pw_far;  /* that of the near, the far part */
	s32 dt;                 /* samples frozen for the remote's signal */
	/* ls: the near part solved by least squares at the end of the
	   training (V.90's analogue modem: its PCM receiver wants more of
	   the echo away than NLMS takes in the time) on what it stored of
	   it: our samples (EC_NEAR_LEN - 1 before its first too), the line */
	s16 ls;
	std::vector<float> ls_tx, ls_rx;
	s32 ls_n;

	void init();
	void search();
	float run(float tx, float rx);
	/* the least squares solution in place of NLMS's when it leaves
	   less: the dB it takes away, or 0 (none stored, or a far part) */
	float ls_solve();
	void ls_free();         /* what was stored */
};

#define EC_LS_MAX       (2 * 9600)      /* samples stored, at most */

#define V34_FIFO        8

/* DSP_TEST_ECHO=<dB>:<ms>[a|c] (tests): the echo of what the modem sends
   in what it receives, as a 2-wire line's hybrid gives it back on a
   9600 Hz line: <ms> late, <dB> down, through a high pass at 300 Hz (the
   line transformer) and a low pass at 3400 Hz */
struct TestEcho {
	enum { MAX = 1024 };
	float g;
	s32 d, pos;
	float buf[MAX];
	float hx, hy, ly;

	/* from e (<dB>:<ms>, NULL: no echo) at srate */
	void init(const char *e, s32 srate)
	{
		float db = 0, ms = 0;
		reinit(*this);
		if (!e)
			return;
		sscanf(e, "%f:%f", &db, &ms);
		g = powf(10, db / 20);
		d = static_cast<s32>(ms * srate / 1000);
		if (d < 1)
			d = 1;
		if (d >= MAX)
			d = MAX - 1;
	}
	/* the echo in the next sample received */
	float echo()
	{
		const float a = 0.82f;          /* exp(-2 pi 300 / 9600) */
		const float b = 0.63f;          /* 1 - exp(-2 pi 1500 / 9600) */
		float x = buf[(pos - d) & (MAX - 1)];
		hy = a * (hy + x - hx);
		hx = x;
		ly += b * (hy - ly);
		return g * ly;
	}
	/* the sample sent */
	void push(float y)
	{
		buf[pos & (MAX - 1)] = y;
		pos++;
	}
};

/* modem-on-hold (9.10/V.92): the remote's MH sequences and our answers
   to them, our own request (MHreq, then MHfrr or MHcda) */
template <class P>
struct V34Moh {
	s16 moh_t1;             /* the T1 code we grant (Table 33/V.92), 0:
				   we deny it (DSP_V92_MOH) */
	s16 ev_mh, mh_sig, mh_info;     /* an MH sequence came in */
	s32 mh_seen;            /* when */
	s16 moh_resp;           /* what we answer it with */
	s16 moh_drop;           /* MHcda after our MHnack: disconnect */
	s32 moh_end;            /* on hold (or reconnecting): disconnect then
				   unless phase 1 has come */
	s16 moh_req;            /* what we send of ours: MHreq, then MHfrr or
				   MHcda */
	s32 moh_req_hold;       /* seconds we keep the remote on hold */
	s16 moh_req_t1;         /* the T1 code of its MHack */
	s32 moh_req_at;         /* test: ours this far into data mode */
	s32 moh_req_n;          /* (samples of it so far) */
	u32 moh_poll;           /* when to look for the DTE's request */
	void mh_check(CcRx *r);
	s16 mh_event();
	void moh_respond(s16 sig);
	void moh_hold(s16 secs);
	void moh_back();
	s32 moh_ans;            /* MOH_BACK: samples of the remote's ANSam */
	s16 moh_backed;         /* we took the remote back (for the log) */
	void moh_request(s32 hold_secs);
	void moh_req_test();
	s16 moh_step(s32 c);
};

/* V.8 (and V.92's short phase 1 around it): ANSam, CM / JM / CJ out
   on V.21 and in, what they agree on; the caller's analysis of the
   answer tone, and the V.32 / V.22 callers it may hear */
template <class P>
struct V34V8 {
	s16 v8_low;             /* V.8 for this low speed modulation alone
				   (rt->v8_for), else back to it */
	s16 v8_dp;              /* the modulation agreed in V.8 when not
				   V.34 (0: none in common) */
	/* ANSam and V.8's transmitter */
	Osc ansam, ansam_am;
	float ansam_sign;
	FskBitTx fsk;           /* V.8's V.21 */
	BitQ v8tx;
	s16 v8tx_repeat;        /* restart the message when it is done */
	/* V.8 receiver */
	FskDemod v21;
	u32 v8_sr;              /* bit window, newest in bit 19 */
	s16 v8_sync;
	s32 v8_bits;
	unsigned char v8_rx[32], v8_last[32];
	s16 v8_n, v8_last_n;
	s16 v8_ok;              /* two identical CM / JM */
	s16 v8_zeros;           /* zero octets in a row (CJ) */
	s16 v8_cj;
	/* answerer, while JM goes out: 10 ms blocks of the line's energy,
	   the CM's (the loudest block), and quiet blocks in a row (30 dB
	   under it) */
	float v8_blk_e, v8_cm_e;
	s16 v8_blk_n, v8_quiet;
	unsigned char v8_remote[32];
	s16 v8_remote_n;
	/* answer tone analysis (caller) */
	Osc ans_lo;
	cplx ans_lp1, ans_lp2;
	double env_sum, env15_re, env15_im;
	float ans_peak;          /* the answer tone's envelope at most */
	s32 ans_gone;            /* samples of it below a third of that */
	s32 env_n;
	/* V.32 / V.22 callers during ANSam */
	Goertzel aa_g;
	s16 aa_blocks;
	/* adaptive answer (MDMPRM_ADAPTIVE): a CM of a fax call is told
	   (DPSTAT_FAX) */
	s16 v8_adaptive;
	s16 v8_remote_cat(s16 tag);
	void v8_make_msg(BitQ *q);
	s16 v8_fax_call();
	s16 v8_remote_v34();
	s16 v8_remote_mod(s16 k);
	s16 v8_choice();
	s16 v8_handover();
	s16 v8_back(const char *why);
	s16 v8_v90();
	s16 v8_v90a();
	s16 v8_v91();
	s16 v8_vcvm();
	void v8_rx_bit(s16 bit);
	void v8_quiet_run(float x);
	void ans_analyse(float x);
	float ans_am_depth();
	s16 v8_step(s32 c);
};

/* our INFO0 and the remote's (11.2/V.34): what ours offers, its
   repetition and acknowledgement, phase 2's recovery from a lost one
   (11.2.2/V.34), and INFO0a again in a retrain */
template <class P>
struct V34Info0 {
	s16 rt_info0;           /* retrain: 1 INFO0a in, 2 answered */
	s16 info0_again;        /* our INFO0 again, acknowledging the
				   remote's (its bit 28) */
	s16 info0_sent;         /* our INFO0s in this phase 2 */
	s16 info0_rep;          /* the remote's INFO0 again (since we last
				   looked) */
	s16 info0_rep_ack;      /* the last one's bit 28 */
	s16 info0_rec;          /* recovering: our INFO0s back to back */
	s16 info0_recovered;    /* (in this phase 2) */
	s16 info0_acked;        /* the remote's acknowledgement in that */
	s32 info0_tx_at;        /* when ours last went out */
	s32 info0_at;           /* when the remote's INFO0 last came */
	s16 lose_info0;         /* test: our first INFO0 spoilt */
	s16 info0_rates;        /* our INFO0's bits 12:19 (rates, carriers,
				   3429 allowed): all, or a test's */
	void make_info0(BitQ *q);
	void rt_info0_frame(const unsigned char *h, s32 n);
	void info0_repeat_frame(const unsigned char *h, s32 n);
	bool info0_resend(s32 c);
	bool info0_more();
};

/* line probing (10.1.2.4, 10.1.3.6/V.34): our L1 and L2, the
   remote's analysed (the level of each tone, the noise, the rates each
   symbol rate may take), and its tone A or B during our L2 */
template <class P>
struct V34Probe {
	float probe[PROBE_PERIOD];
	s16 probe_pos;
	float probe_gain;
	s32 probe_start;
	/* the remote's L1/L2 */
	Goertzel probe_g[2];
	/* the remote's tone A or B during our own L2: exactly on a bin of
	   the 64 sample probe period, where the echo of L2 has nothing */
	Goertzel tone_g;
	s16 tone_blocks;
	float pblock_e;
	s16 probe_blocks;
	s32 l1_start;
	double pacc[PROBE_PERIOD];
	float pprev[PROBE_PERIOD];
	double ptotal, pdiff;
	s32 pcount;
	float snr_db;
	float tone_db[26];      /* received level of each 150 Hz tone */
	s16 rate_of_baud[V34_NBAUDS];   /* receivable rates (N), by probing */
	void probe_init();
	s16 probe_rx_sample(float x);
	void probe_collect(float x);
	s16 probe_analyse();
	void start_probe();
};

/* the control channel of phase 2 and of the retrains (10.1.2/V.34):
   its DPSK out (the tones, INFO, the reversals, MH) and in, the INFO
   frames received, the round trip delay from the reversals */
template <class P>
struct V34Cc {
	QamTx cc;
	float cc_sym;           /* +-1, 0: silence */
	s16 cc_rev;             /* reverse at the next symbol */
	BitQ cctx;       /* INFO bits, a 1 reverses the phase */
	s32 cc_syms;
	/* phase 2 receiver */
	CcRx ccr;
	s16 ev_info0, ev_info1, ev_rev;
	s32 rev_time;
	Info0 rinfo0;
	Info1c rinfo1c;
	Info1a rinfo1a;
	s32 rtd;                /* round trip delay, samples */
	s32 rev_sent;           /* when our reversal went out */
	void cc_tone();
	void cc_send();
	s16 cc_busy();
	void cc_silence();
	s32 rev_on_line();
	void cc_rx_bit(s16 bit);
	void cc_phase_frame();
	void info_frame(CcRx *r);
};

/* who starts a retrain, for the call's statistics: we, the remote, or
   neither (modem-on-hold leaves data mode as a retrain does) */
enum { RT_OURS, RT_REMOTE, RT_HOLD };

/* retrains (11.5/V.34) and rate renegotiations (11.6/V.34): the
   remote's tone A or B in data mode, the line's quality that asks for
   either, the tests' own */
template <class P>
struct V34Retrain {
	Goertzel rt_g, own_g;    /* the remote's tone, the echo of ours */
	Goertzel mate_g;         /* 1800 Hz: what the Agere, Broadcom and
				    HSF modems send with their tone A */
	float rt_e;
	s16 rt_blocks;
	s16 rt_heard;           /* the tone has been there a while */
	s32 rt_since;           /* where the blocks that made it heard
				   began (0: not by its blocks) */
	s32 bad;                /* data mode: samples with a poor mse */
	s16 retraining;         /* back to data mode without a new CONNECT */
	s16 retrains;
	s32 retrain_at;         /* test: our own retrain this far (samples) */
	/* rate renegotiation */
	s16 renegotiating;
	s16 renegs;
	s16 rn_max;             /* the most we may ask for (the probing's) */
	s32 rn_hi, rn_lo;       /* data mode: samples with an mse for a lower,
				   a higher rate */
	float mse_good;         /* data mode: the mse while it was fine */
	s32 reneg_at;           /* test: our own renegotiation this far */
	bool reneg_test;        /* (done, whatever the remote's before it) */
	s16 reneg_to;           /* test: to this rate (N) */
	void rt_tone(float x);
	void retrain(const char *why, s16 by = RT_OURS);
	s16 reneg_rate(s16 up);
	void renegotiate(const char *why, s16 n, bool remote = false);
	s16 rt_step(s32 c);
};

/* V.92's quick connect (short phase 1, 9.2.4/V.92): QC1a and QCA1d,
   TONEq, ANSpcm (the steps are handshake()'s QCA_ states) */
template <class P>
struct V34Qc {
	s16 qc_ok;              /* we answer QC1a (DSP_V92_QC=0: not) */
	s16 qc_n;               /* bits of a QC frame, -1: none */
	u16 qc_frame;           /* bits 20:29, the first in bit 0 */
	s16 ev_qc;
	s16 qc_done;            /* QC1a answered (or tried) */
	s16 qc_lapm;
	Goertzel toneq_g;       /* TONEq, 980 Hz */
	Goertzel toneq_space_g; /* V.21(L)'s space, 1180 Hz: CM, not TONEq */
	s16 toneq_blocks;
	/* the analogue modem calling: we send QC1a (DSP_V92A_QC=0: not),
	   did, ANSpcm's samples */
	s16 qca_ok;
	s16 qcc_sent;
	s32 qcc_ans;
	s16 qc_step(s32 c);
};

/* (PcmFrames: V.90's phase 4 frames, MP the sequence) */
struct V34Pump : PumpBase<V34Pump, v34_state>, PcmFrames<V34Pump>,
	V34Moh<V34Pump>,
	V34V8<V34Pump>,
	V34Info0<V34Pump>,
	V34Probe<V34Pump>,
	V34Cc<V34Pump>,
	V34Retrain<V34Pump>,
	V34Qc<V34Pump> {
	static constexpr const char *name = "V.8/V.34 data pump";

	/* (frees what the receiver holds) */
	~V34Pump() { cleanup(); }

	/* (srate: of the pump, 9600) */
	s32 line_rate;          /* 9600, or 8000 through the converters */
	s16 analog;             /* an analogue line: 9600 Hz, or 8000 Hz
				   with dp_runtime_set_analog() */
	s16 v90;                /* offer V.90 as the digital modem */
	s16 pcm;                /* V.90 agreed in V.8 */
	s16 v91;                /* offer V.91 */
	s16 vcvm;               /* offer V.cvm (its information field in
				   CM / JM's non-standard facilities),
				   with V.22 and V.21 for the fallback */
	VcvmCaps vcvm_caps;
	s16 v91_on;             /* V.91 agreed in V.8 */
	s16 v90a;               /* offer V.90 as the analogue modem */
	s16 v90a_on;            /* V.90 agreed in V.8, we the analogue
				   modem */
	s16 v90a_uinfo;         /* the 2 point train's Ucode we ask for */
	s16 v90a_rx;            /* the line into v90rx */
	V90Rx v90rx;    /* the analogue modem's PCM receiver */
	V90Dil v90a_dil;        /* the DIL our Ja asks for */
	V90Cp v90a_cpt, v90a_cp;        /* the constellations we ask for */
	TestEcho test_echo;     /* DSP_TEST_ECHO */
	s16 v92;                /* offer V.92 as the digital modem */
	s16 v92_on;             /* both INFO0 said V.92 */
	s16 v92a;               /* V.92 as the analogue modem (+MS=92 on an
				   analogue line; DSP_V92A=0: V.90) */
	s16 v92a_on;            /* and the digital modem's INFO0d said V.92
				   (for the call) */
	s16 v92a_up;            /* we ask for PCM upstream where INFO1d
				   offers it (DSP_V92A_UP=0: not) */
	s16 v92a_noup;          /* it failed in this call: V.90 next */
	s16 v92a_s2_req;        /* our INFO0a asks for short phase 2 */
	s16 v92a_renegs;        /* PCM upstream's rate renegotiations */
	s16 v92a_short2;        /* and the digital modem's INFO0d too */
	s16 v92a_up_on;         /* this phase 3 is PCM upstream's */
	V92ATx patx;            /* its transmitter */
	/* its phase 4: our CPu sent (when, in samples), our acknowledged
	   sequences queued (sent whole once patx.seqs reaches
	   v92a_ack_seqs) */
	s16 v92a_cpu, v92a_cpu_n;
	s32 v92a_cpu_at;
	s16 v92a_acked;
	s32 v92a_ack_seqs;
	/* a fast parameter exchange: 1 the digital modem's (its Rf first),
	   2 ours (our RM first, a CPu designed anew); how many, and ours
	   this far into data mode (test: DSP_V92A_FPE=<ms>[a|c]) */
	s16 v92a_fpe_who, v92a_fpes;
	s32 v92a_fpe_at;
	s16 short2;             /* and both asked for short phase 2 */
	s16 short2_ok;          /* we ask for it (DSP_V92_SHORT2=0: not) */
	V91 v91m;
	s16 pcmup_ok;           /* we offer PCM upstream (V.92) */
	s16 pcmup_plain;        /* the next try without the prefilter (the
				   last failed with it) */
	s32 quiet_blocks;       /* phase 2: blocks without the remote's tone */
	s16 p2_gap;             /* waiting for INFO1a: 1 tone, 2 then a gap */
	V92Up v92u;
	u32 link_poll;          /* when to look for the DTE's retrain or
				   renegotiation (MDMPRM_LINK_REQ) */
	void link_req_poll();
	s16 rate_cap(unsigned param) const;
	s16 alaw;               /* the network's G.711 law */
	V90Tx v90tx;    /* the PCM transmitter */
	s16 pcm_tx;             /* our output from it */
	s16 sbar_seen;
	V90Cp cpt, cp;  /* the analogue modem's CPt and CP */
	s16 mp_h[6];            /* V.90: the precoder our last MP asked for */
	s16 up_n;               /* upstream rate, 2400 bit/s units */
	/* V.90 rate renegotiation (9.6/V.90) */
	s16 v90_rn_init;        /* the encoder starts on CPt again */
	s16 v90_rd_wait;        /* Rd once the remote's S-bar is in */
	s16 rn_sbar;            /* S-bar count at its S */
	s16 sil_cp;             /* CP count when our silence began */
	Resampler rs_up, rs_down;
	float fifo[V34_FIFO];   /* converted output, 8000 Hz */
	s16 fifo_n;
	s32 out_n;                 /* samples sent on the 8000 Hz line */
	Resampler rs_ref;        /* V.90: our PCM signal at 9600 Hz */
	float ref_fifo[V34_FIFO];
	s16 ref_n;

	/* transmitter: what goes out now */
	enum v34_txm txm;

	/* echo canceller (from phase 3 on) */
	V34Echo ec;
	float last_tx;

	/* phases 3, 4 and data */
	V34Tx ptx;
	V34Rx prx;
	s16 prx_on;
	/* loop timing (the caller's, DSP_V34_LOOP_TIMING=0 for none): our
	   symbols on the remote's clock, the offset our receiver measures
	   (prx.t_freq) followed at most TX_CLK_SLEW a sample */
	s16 tx_loop, tx_loop_on;
	float tx_clk;

	/* outcome */
	s16 baud_tx, baud_rx;   /* symbol rate codes */
	s16 high_tx, high_rx;
	float precode_min;      /* dB: V34_PRECODE_MIN, or a test's */
	s16 preemph_tx;
	s16 max_rate_tx, max_rate_rx;
	s32 rate_tx, rate_rx; /* data mode, bit/s */
	s32 lost;               /* data mode: samples without the signal */

	s16 connected;          /* CONNECT has been reported */

	/* its name in the log */
	const char *tag() const { return "v34"; }
	void fs_reinit(V90Tx *t);
	const V90Cp *fs_data_cp();
	void fs_seq_next(s16 first);
	bool fs_acked();
	bool fs_silence();
	void fs_ed_end();
	void fs_data(unsigned char *b, s16 n);
	void set_state(enum v34_state s);
	void make_info1c(BitQ *q);
	void make_info1a(BitQ *q);
	void make_info1a_v90(BitQ *q);
	/* the length of the remote's INFO0 (V.90's INFO0a longer), its
	   sync and CRC included */
	s16 info0_rx_len() const { return v90a_on ? 42 + 16 : 29 + 16; }
	s16 info_expected();
	void v91_start(s16 leased = 0);
	void v92_start();
	s16 v91_escape();
	void ec_schedule();
	bool remote_j() const;
	bool ec_far_late() const;
	void pump_rx_sample(float x);
	s16 fallback(const char *why);
	void choose_bauds();
	void v90a_choose();
	void v90a_start();
	s16 v90a_phase4();
	void make_info1a_v92(BitQ *q);
	s16 v90a_pick_uinfo();
	void v92a_up_start();
	s16 v92a_cpt();
	s16 v92a_up_fail(const char *why);
	s16 v92a_next_seq(s16 fpe = 0);
	s16 v92a_cpd_ok(const V92Cpd &d);
	void v92a_fpe(s16 who, const char *why = "");
	void v92a_reneg(s16 who, const char *why);
	void v90a_reneg_ours(const char *why);
	s16 v90a_cp_design(s16 fpe = 0);
	s16 v90a_data_setup();
	s32 v90a_quiet;         /* data mode: samples of the PCM signal gone */
	float v90a_cp_noise;    /* the noise CP was designed on */
	s32 v90a_bad, v90a_good;        /* data mode: samples of the error
					   far above it, or well below */
	s16 v90a_rn;            /* a rate renegotiation: 1 the digital
				   modem's (its Rd first), 2 ours */
	s32 v90a_rn_at;         /* test: ours this far into data mode
				   (DSP_V90A_RENEG=<ms>[a|c]) */
	void v90a_reneg(s16 who, const char *why = "");
	s16 v90a_v34;           /* V.90 failed: V.34 after the next retrain */
	s16 v90a_fail(s16 no_pcm);
	double v90a_limit, v90a_cpt_pw;	/* Table 15's, CPt's power */
	s16 primary_start();
	s16 v90_start();
	void v90_make_mp(s16 ack);
	s16 v90_data_setup();
	void tx_next(enum v34_state s, enum v34_txseg seg);
	s16 data_setup();
	void stat_link();
	void mp_start();
	s16 rate_floor() const;
	void v90_renegotiate(const char *why, s16 n, s16 remote);
	void v90_rd_start();
	/* the digital modem in V.90 phases 3 and 4 */
	s16 v90_digital() const
	{
		return state >= P3D_WAIT_JA && state <= P4D_TRAIN;
	}
	s16 handshake();
	float pump_tx_sample();
	s16 sample(float x, float *y, const float *ref);
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_, s32 max_frag);
	void cleanup();
	s16 process(void *in, void *out, s32 cnt) override;
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
void BitQ::put(u32 val, s16 nbits)
{
	s16 i;
	for (i = 0; i < nbits && n < static_cast<s16>(sizeof(b)); i++)
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
	u16 crc = v34_crc_bits(b + 12, n - 12, 0xFFFF);
	s16 i;
	for (i = 15; i >= 0; i--)
		put((crc >> i) & 1, 1);
	put(0xF, 4);
}

/* our INFO0 (Table 14/V.34): all symbol rates, 1664 point
   constellations, no power reduction; as the V.90 / V.92 digital modem
   INFO0d (Table 7/V.90, Table 15/V.92) with our PCM powers and law.
   The acknowledgement: ours again for the remote's INFO0 */
template <class P>
void V34Info0<P>::make_info0(BitQ *q)
{
	P &self = static_cast<P &>(*this);
	q->info_start();
	/* 2743, 2800, 3429; 3000 low and high carrier, 3200 low and high
	   carrier; 3429 allowed */
	q->put(info0_rates, 8);
	q->put(0, 1);        /* power reduction */
	q->put(0, 3);        /* symbol rate difference */
	q->put(0, 1);        /* CME */
	q->put(1, 1);        /* 1664 point constellations */
	if (self.pcm) {
		/* short phase 2 (Table 15/V.92), not in a retrain (where
		   we do the full one) */
		q->put(self.v92 && self.short2_ok && !self.retraining, 1);
		q->put(self.v92, 1);      /* V.92 */
	} else if (self.v90a_on && self.v92a) {
		/* INFO0a (Table 16/V.92): V.92, and short phase 2 where we
		   go for PCM upstream (9.4/V.92), not in a retrain */
		self.v92a_s2_req = self.v92a_up && !self.v92a_noup &&
			self.short2_ok && !self.retraining;
		q->put(1, 1);
		q->put(self.v92a_s2_req, 1);
	} else
		q->put(0, 2);        /* internal clock */
	q->put(info0_again, 1);      /* acknowledge (of INFO0a) */
	info0_tx_at = self.total;
	if (self.pcm) {
		/* INFO0d, Table 7/V.90 */
		q->put(V90_P2_POWER, 4);
		q->put(V90_MAX_POWER, 5);
		q->put(0, 1);        /* power at the terminals */
		q->put(self.alaw, 1);
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
	s16 i;
	q->info_start();
	q->put(0, 3);
	q->put(0, 3);
	q->put(0, 7);        /* no MD */
	for (i = 0; i < V34_NBAUDS; i++) {
		s16 rate = rate_of_baud[i];
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

/* our INFO1a as the analogue modem asking for V.90 (Table 10/V.90): no
   MD, the 2 point train's Ucode (v90a_uinfo), the upstream symbol rate
   v90a_choose() picked (its carrier and pre-emphasis INFO1d's), 6 for
   the digital modem's 8000; no frequency offset (as our INFO1a and
   INFO1c) */
void V34Pump::make_info1a_v90(BitQ *q)
{
	q->info_start();
	q->put(0, 6);
	q->put(0, 7);        /* no MD */
	q->put(v90a_uinfo, 7);
	q->put(0, 2);
	q->put(baud_tx, 3);
	q->put(6, 3);
	q->put(0, 10);       /* frequency offset */
	q->info_end();
}

/* a field of an INFO frame, least significant bit first in time */
static s16 bits_val(const unsigned char *b, s16 first, s16 last)
{
	return field(b, first, last - first + 1);
}

/* b: the frame from bit 0 (fill); 1 when the CRC at crc_pos is good */
static s16 info_crc_ok(const unsigned char *b, s16 crc_pos)
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

/* the digital modem's INFO0d (Table 7/V.90): INFO0's bits and its
   powers, law and upstream 3429 */
void Info0::parse_d(const unsigned char *b)
{
	parse(b);
	p2_power = bits_val(b, 29, 32);
	max_power = bits_val(b, 33, 37);
	codec_power = b[38];
	law = b[39];
	up3429 = b[40];
	/* (INFO0d has them the other way round, Table 15/V.92) */
	short2 = b[26];
	v92 = b[27];
}

/* the caller's INFO1c (Table 15/V.34): at each symbol rate its
   carrier, pre-emphasis and the most it receives (N, 2400 bit/s steps) */
void Info1c::parse(const unsigned char *b)
{
	s16 k;
	power_red = bits_val(b, 12, 14);
	add_power_red = bits_val(b, 15, 17);
	md = bits_val(b, 18, 24);
	for (k = 0; k < V34_NBAUDS; k++) {
		s16 o = 25 + 9 * k;
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
s16 V34Pump::info_expected()
{
	if (!ev_info0)
		return info0_rx_len();
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
template <class P>
s16 V34V8<P>::v8_remote_cat(s16 tag)
{
	s16 i;
	for (i = 1; i < v8_remote_n; i++)
		if ((v8_remote[i] & 0x1F) == tag)
			return v8_remote[i];
	return -1;
}

/* CM (caller) or JM (answerer); a JM repeats what both modems have.  As a
   V.90 digital modem: PCM in the modulation octet, the PCM modem
   availability category and a digital network connection; as the
   analogue modem the first two. */
template <class P>
void V34V8<P>::v8_make_msg(BitQ *q)
{
	P &self = static_cast<P &>(*this);
	s16 i, k, pcm_ = (self.v90 ? V8_PCM_DIGITAL : 0) |
		(self.v91 ? V8_PCM_V91 : 0) |
		(self.v90a ? V8_PCM_ANALOGUE : 0);
	q->reset();
	q->put(0x3FF, 10);
	/* in a JM what both have: V.90 when the CM had the other end of
	   it, V.91 when it had V.91 */
	if (!self.caller) {
		s16 c = v8_remote_cat(V8_CAT_PCM);
		if (c < 0)
			c = 0;
		if (!(c & V8_PCM_ANALOGUE))
			pcm_ &= ~V8_PCM_DIGITAL;
		if (!(c & V8_PCM_DIGITAL))
			pcm_ &= ~V8_PCM_ANALOGUE;
		if (!(c & V8_PCM_V91))
			pcm_ &= ~V8_PCM_V91;
	}
	for (i = 0, k = -1; i < static_cast<s16>(sizeof(v8_msg)); i++) {
		s16 c = v8_msg[i];
		/* the modulation octets: in a JM what the CM has too */
		if ((c & 0x1F) == V8_CAT_MOD)
			k = 0;
		else if (k >= 0 && (c & 0x18) == 0x10)
			k++;
		else
			k = -1;
		if (!self.caller && k >= 0)
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
		/* V.cvm: V.22 and V.21 its fallback, nothing above V.22
		   (8.3/V.cvm: a legacy DCE then takes V.22 at once) */
		if (self.vcvm && k == 0)
			c &= ~V8_MOD1_V34;
		if (self.vcvm && k == 1)
			c &= ~V8_MOD2_V32;
		if (self.vcvm && k == 2)
			c &= ~V8_MOD3_V23;
		if (pcm_ && (c & 0x1F) == V8_CAT_MOD)
			c |= V8_MOD1_PCM;
		if ((pcm_ & (V8_PCM_DIGITAL | V8_PCM_V91)) &&
		    (c & 0x1F) == V8_CAT_ACCESS)
			c |= V8_ACCESS_DIGITAL;
		q->put_octet(c);
	}
	if (pcm_)
		q->put_octet(V8_CAT_PCM | pcm_);
	/* V.cvm's field (Annex A/V.cvm); in a JM where the CM had it */
	if (self.vcvm && (self.caller || v8_vcvm())) {
		u8 ns[24];
		s16 n = vcvm_v8_ns(self.vcvm_caps, ns);
		for (s16 j = 0; j < n; j++)
			q->put_octet(ns[j]);
	}
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

/* is the remote's CM a fax call's (its call function, Table 3/V.8:
   transmit or receive facsimile, T.30)? */
template <class P>
s16 V34V8<P>::v8_fax_call()
{
	s16 c = v8_remote_cat(V8_CAT_CALLF);
	return c >= 0 && ((c >> 5) == 4 || (c >> 5) == 5);
}

/* is V.34 in the remote's CM / JM? */
template <class P>
s16 V34V8<P>::v8_remote_v34()
{
	s16 c = v8_remote_cat(V8_CAT_MOD);
	return c >= 0 && (c & V8_MOD1_V34) != 0;
}

/* the remote's modulation octet k (0: the first, 1, 2: its extension
   octets), 0 when it has none */
template <class P>
s16 V34V8<P>::v8_remote_mod(s16 k)
{
	s16 i, j = -1;
	for (i = 1; i < v8_remote_n; i++) {
		s16 c = v8_remote[i];
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
template <class P>
s16 V34V8<P>::v8_choice()
{
	s16 m2 = v8_remote_mod(1), m3 = v8_remote_mod(2);
	if (v8_low)
		return (v8_low == DP_V21 ? m3 & V8_MOD3_V21 :
			v8_low == DP_V23 ? m3 & V8_MOD3_V23 :
			m2 & V8_MOD2_V22) ? v8_low : 0;
	if (v8_vcvm())
		return DP_VCVM;
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
static const char *v8_dp_name(s16 dp)
{
	return dp == DP_VCVM ? "V.cvm" :
		dp == DP_V32BIS ? "V.32bis" : dp == DP_V22BIS ? "V.22bis" :
		dp == DP_V22 ? "V.22" : dp == DP_V23 ? "V.23" : "V.21";
}

/* no V.34 after all (why): back to the low speed modulation V.8 ran for
   (v8_low), its pump without V.8; returns DPSTAT_CHANGEDP */
template <class P>
s16 V34V8<P>::v8_back(const char *why)
{
	P &self = static_cast<P &>(*this);
	DSP_DBG("v34: %s: back to %s without V.8\n", why,
		v8_dp_name(v8_low));
	if (self.rt)
		self.rt->v8_for = 0;
	return self.hand_over(static_cast<enum DP_ID>(v8_low), 0);
}

/* 75 ms after CJ (both sides, 8.1.2 and 8.2.3/V.8): to the pump of
   v8_dp, what V.8 agreed on without V.34 (the answer tone and V.8 done,
   no automode), which sends its sigC or sigA at once; returns
   DPSTAT_CHANGEDP */
template <class P>
s16 V34V8<P>::v8_handover()
{
	P &self = static_cast<P &>(*this);
	if (self.rt) {
		self.rt->v8_for = 0;
		/* (calling, only the FSK pumps look at it: the V.32bis and
		   V.22bis callers do not clear it for a later fallback) */
		self.rt->after_v8 = !self.caller || v8_dp == DP_V21 ||
			v8_dp == DP_V23;
		if (!self.caller)
			self.rt->ans_sent = 1;
		else if (v8_dp == DP_V32BIS)
			self.rt->v32_automode = 1;
		/* V.cvm: the peer's information field for its pump */
		VcvmCaps pc;
		self.rt->vcvm_peer = v8_dp == DP_VCVM &&
			vcvm_v8_find(v8_remote, v8_remote_n, &pc);
		if (self.rt->vcvm_peer)
			pc.encode(self.rt->vcvm_info);
	}
	return self.hand_over(static_cast<enum DP_ID>(v8_dp), 0);
}

/* V.90 with us as the digital modem: both offer it */
template <class P>
s16 V34V8<P>::v8_v90()
{
	P &self = static_cast<P &>(*this);
	s16 c = v8_remote_cat(V8_CAT_PCM);
	return self.v90 && c >= 0 && (c & V8_PCM_ANALOGUE) != 0;
}

/* V.90 with us as the analogue modem: the remote is its digital one */
template <class P>
s16 V34V8<P>::v8_v90a()
{
	P &self = static_cast<P &>(*this);
	s16 c = v8_remote_cat(V8_CAT_PCM);
	return self.v90a && c >= 0 && (c & V8_PCM_DIGITAL) != 0;
}

/* V.91: both offer it (and the remote is not an analogue V.90 modem) */
template <class P>
s16 V34V8<P>::v8_v91()
{
	P &self = static_cast<P &>(*this);
	s16 c = v8_remote_cat(V8_CAT_PCM);
	return self.v91 && !v8_v90() && c >= 0 && (c & V8_PCM_V91) != 0;
}

/* V.cvm: both offer it (its field in the remote's CM or JM) */
template <class P>
s16 V34V8<P>::v8_vcvm()
{
	P &self = static_cast<P &>(*this);
	VcvmCaps c;
	return self.vcvm && vcvm_v8_find(v8_remote, v8_remote_n, &c);
}

/* one bit of the V.21 receiver (V.8 at 300 bit/s): QC1a frames
   (ev_qc, qc_frame), CM / JM (two identical in a row: v8_ok,
   v8_remote) and CJ (v8_cj) */
template <class P>
void V34V8<P>::v8_rx_bit(s16 bit)
{
	P &self = static_cast<P &>(*this);
	v8_sr = (v8_sr >> 1) | (static_cast<u32>(bit) << 19);
	/* V.92's QC1a before CM (Table 2/V.92): ten ones, 0101010101, then
	   a frame of ten bits, twice */
	if (self.qc_n >= 0 && ++self.qc_n == 10) {
		u16 f = v8_sr >> 10;
		self.qc_n = -1;
		if (dsplibs_debug_level >= 6)
			DSP_DBG("v34: QC frame %03x\n", f);
		/* (QC1a for the digital modem, QCA1d for the analogue:
		   bits 21 and 22 say which) */
		if ((f & 1) == 0 && (f >> 9) == 1) {
			/* sent twice; the first may come before the
			   receiver has the carrier */
			if (!self.ev_qc) {
				self.ev_qc = 1;
				self.qc_lapm = (f >> 3) & 1;
				DSP_DBG("v34: V.92 QC%s%s, LAPM %d, UQTS code %d\n",
					f & 0x4 ? "A" : "", f & 0x2 ? "1d" : "1a",
					(f >> 3) & 1, (f >> 4) & 0x1F);
			}
			self.qc_frame = f;
		}
	}
	/* its sync, 0101010101, after the ten ones; of the first of the
	   two, the receiver may have only the last few ones yet (the
	   analogue modem starts QC1a right on ANSam's 1 s, and answers
	   only a QCA1d to the first well: the Agere modem went on with
	   CM when ours came after the second) */
	if ((v8_sr & 0xFFFC0) == 0xAABC0 && self.qc_n < 0) {
		if (dsplibs_debug_level >= 6)
			DSP_DBG("v34: QC sync at %ld ms (%s ones)\n",
				static_cast<long>(self.total) * 1000 /
				self.srate,
				(v8_sr & 0x3F) == 0x3F ? "ten" : "fewer");
		self.qc_n = 0;
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
					" %02x %02x\n",
					self.caller ? "JM" : "CM",
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
		if (v8_n < static_cast<s16>(sizeof(v8_rx)))
			v8_rx[v8_n++] = c;
	} else if (v8_bits == 9 && v8_zeros == 2 && !(v8_sr & 0xFF800))
		/* CJ's third zero octet without its stop bit: the HSF
		   modem drops its carrier right after the last data bit */
		v8_cj = 1;
}

/* the answerer's watch on the line while JM goes out (x at 9600 Hz):
   the V.21 receiver's own carrier detector is too slow to see the
   175 ms between CJ and V.cvm's INFO0 */
template <class P>
void V34V8<P>::v8_quiet_run(float x)
{
	v8_blk_e += x * x;
	if (++v8_blk_n < V34_SRATE / 100)
		return;
	if (v8_blk_e > v8_cm_e)
		v8_cm_e = v8_blk_e;
	v8_quiet = v8_blk_e < v8_cm_e * 1e-3f ? v8_quiet + 1 : 0;
	v8_blk_e = 0;
	v8_blk_n = 0;
}

/* ANSam: 15 Hz amplitude modulation of the 2100 Hz answer tone */
template <class P>
void V34V8<P>::ans_analyse(float x)
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
	w = 2 * static_cast<float>(M_PI) * 15 * env_n / V34_SRATE;
	env_sum += e;
	env15_re += e * cosf(w);
	env15_im += e * sinf(w);
	env_n++;
}

/* the depth of the 15 Hz modulation, 0.2 for ANSam */
template <class P>
float V34V8<P>::ans_am_depth()
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
template <class P>
void V34Cc<P>::cc_tone()
{
	P &self = static_cast<P &>(*this);
	if (self.txm != TXM_CC || cc_sym == 0) {
		cc_sym = 1;
		self.txm = TXM_CC;
	}
}

/* the bits in cctx out from their first, on the tone */
template <class P>
void V34Cc<P>::cc_send()
{
	cc_tone();
	cctx.pos = 0;
}

/* INFO / MH bits or a reversal still to go out */
template <class P>
s16 V34Cc<P>::cc_busy()
{
	return cctx.pos < cctx.n || cc_rev;
}

/* zero symbols: the pulses in flight die away */
template <class P>
void V34Cc<P>::cc_silence()
{
	cc_sym = 0;
}

/* our last reversal has been on the line for this long */
template <class P>
s32 V34Cc<P>::rev_on_line()
{
	P &self = static_cast<P &>(*this);
	return cc_rev ? -1 : self.total - rev_sent - CC_TX_DELAY;
}

/* one 64 sample period of L1/L2 (10.1.2.4/V.34), at V34_LEVEL rms */
template <class P>
void V34Probe<P>::probe_init()
{
	/* Table 17/V.34: 150 Hz multiples, without 900, 1200, 1800 and
	   2400 Hz (the control channel carriers and their neighbours) */
	static const s16 f[] = { 150, 300, 450, 600, 750, 1050, 1350,
				     1500, 1650, 1950, 2100, 2250, 2550, 2700,
				     2850, 3000, 3150, 3300, 3450, 3600, 3750 };
	static const char inv[] = { 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1,
				    0, 1, 1, 1, 1, 0, 0 };
	double e = 0;
	s16 i, k;
	for (i = 0; i < PROBE_PERIOD; i++) {
		double x = 0;
		for (k = 0; k < static_cast<s16>(sizeof(f)) /
					static_cast<s16>(sizeof(f[0]));
		     k++)
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
template <class P>
void V34Probe<P>::start_probe()
{
	P &self = static_cast<P &>(*this);
	self.txm = TXM_PROBE;
	probe_pos = 0;
	probe_gain = 2;      /* L1: 6 dB up */
	probe_start = self.total;
}

/* a receiver for the remote's carrier fc (1200 or 2400 Hz), all of it
   cleared */
void CcRx::init(float fc)
{
	reinit(*this);
	lo.set_freq(-fc, V34_SRATE);
}

/* one sample; returns -1, or a decision (0/1) at a symbol centre */
s16 CcRx::sample(float x)
{
	cplx z = cplx_scale(lo.next_cplx(), x), b, d, o;
	s16 i = pos, bit = -1;
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
			s16 e = (rmin_pos - best + CC_SPS + CC_SPS / 2) %
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
			s16 b = best, step;
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
template <class P>
s16 V34Probe<P>::probe_rx_sample(float x)
{
	float e;
	s16 on;
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
template <class P>
void V34Probe<P>::probe_collect(float x)
{
	s16 i = pcount % PROBE_PERIOD;
	pacc[i] += x;
	ptotal += static_cast<double>(x) * x;
	/* the noise from the differences of successive periods: a sample
	   clock offset does not count as noise then */
	if (pcount >= PROBE_PERIOD)
		pdiff += static_cast<double>(x - pprev[i]) * (x - pprev[i]);
	pprev[i] = x;
	pcount++;
}

/* the most N (2400 bit/s steps) at each symbol rate (Table 10/V.34) */
static const s16 v34_max_rate_of_baud[V34_NBAUDS] = { 9, 11, 11, 12, 13, 14 };

/* the least N at the symbol rate (Table 10/V.34): 2400 bit/s only at
   2400 baud, 4800 at the others */
static s16 v34_min_rate(s16 baud)
{
	s16 n;
	for (n = 1; n < 14 && !v34_rates[baud][n - 1].b; n++)
		;
	return n;
}

/* over the SNR a rate needs, for the receiver's own impairments */
#define PROBE_MARGIN 2	/* dB */

/* the remote's L2: the signal to noise ratio after equalization at
   each symbol rate, and the data rates (in 2400 bit/s steps) they
   allow; returns the best */
template <class P>
s16 V34Probe<P>::probe_analyse()
{
	double periods = static_cast<double>(pcount) / PROBE_PERIOD, pt, pn,
	       ptone[26];
	s16 i, k, n, b, best = 1;

	zero(rate_of_baud);
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
		s16 cnt = 0;
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
		DSP_DBG("v34: probe %d baud: SNR %.1f dB -> %" PRId32
			" bit/s\n", (s16)(rs + 0.5f), snr,
			n * INT32_C(2400));
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
template <class P>
void V34Cc<P>::info_frame(CcRx *r)
{
	P &self = static_cast<P &>(*this);
	s16 len = self.info_expected();

	/* no reversals within the frame */
	if (ev_rev && self.total - rev_time <
	    static_cast<s32>(len) * CC_SPS)
		ev_rev = 0;
	r->zeros = 0;
	r->rev_cand = 0;
	r->hist_n = 0;
	zero(r->ph_n);
	if (!ev_info0) {
		if (self.v90a_on) {
			/* the digital modem's INFO0d: its network's law is
			   what our PCM receiver decodes */
			rinfo0.parse_d(r->frame);
			self.alaw = rinfo0.law;
			/* V.92 both ways: modem-on-hold */
			if (self.v92a && rinfo0.v92 && !self.v92a_on) {
				self.v92a_on = 1;
				DSP_DBG("v34: INFO0d V.92: V.92 as the analogue "
					"modem\n");
			}
			self.v92a_short2 = self.v92a_on &&
				self.v92a_s2_req && rinfo0.short2;
			if (self.v92a_short2)
				DSP_DBG("v34: V.92 short phase 2\n");
			DSP_DBG("v34: INFO0d %s, phase 2 at %d dBm0, at most "
				"%.1f dBm0%s, 3429 upstream %d\n",
				rinfo0.law ? "A-law" : "u-law",
				-6 - rinfo0.p2_power,
				-0.5 - rinfo0.max_power / 2.0,
				rinfo0.codec_power ? " (at the codec)" : "",
				rinfo0.up3429);
		} else
			rinfo0.parse(r->frame);
		ev_info0 = 1;
		self.info0_at = self.total;
		self.v92_on = self.pcm && self.v92 && rinfo0.v92;
		self.short2 = self.v92_on && rinfo0.short2 &&
			self.short2_ok && !self.retraining;
		if (self.pcm)
			DSP_DBG("v34: INFO0a V.92 %d, short "
				"phase 2 %d%s\n", rinfo0.v92,
				rinfo0.short2,
				self.v92_on ? ": V.92" : "");
		r->snap = 1;
		DSP_DBG("v34: INFO0 rates %02x 3429 %d "
			"diff %d\n", rinfo0.rates,
			rinfo0.allow_3429,
			rinfo0.max_diff);
	} else {
		if (self.caller) {
			rinfo1a.parse(r->frame);
			DSP_DBG("v34: INFO1a: power reduction %d (+%d) dB, "
				"preemph %d, max rate %d, md %d\n",
				rinfo1a.power_red, rinfo1a.add_power_red,
				rinfo1a.preemph, rinfo1a.max_rate, rinfo1a.md);
		} else {
			s16 b;
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
template <class P>
void V34Info0<P>::rt_info0_frame(const unsigned char *h, s32 n)
{
	P &self = static_cast<P &>(*this);
	unsigned char f[64];
	s16 len = self.info0_rx_len(), k;

	if (!self.retraining || !self.caller || !self.ev_info0 ||
	    self.ev_info1 || rt_info0 || n < len)
		return;
	for (k = 0; k < len; k++)
		f[k] = k < 4 ? 1 : h[(n - len + k) % CC_HIST];
	for (k = 4; k < 12; k++)
		if (f[k] != ((0x72 >> (11 - k)) & 1))
			return;
	if (!info_crc_ok(f, len - 16))
		return;
	self.rinfo0.parse(f);
	rt_info0 = 1;
	DSP_DBG("v34: INFO0a in the retrain\n");
}

/* the remote's INFO0 again in phase 2, after its first and before
   INFO1, not in a retrain: it has not taken ours (bit 28, its
   acknowledgement, 0; 11.2.2/V.34, 9.2.1.2/V.90), or repeats it a
   little longer having taken it (1).  A frame is taken once (the
   sampling phases' histories each have it) */
template <class P>
void V34Info0<P>::info0_repeat_frame(const unsigned char *h, s32 n)
{
	P &self = static_cast<P &>(*this);
	unsigned char f[64];
	s16 len = self.info0_rx_len(), k;

	if (self.retraining || !self.ev_info0 || self.ev_info1 || n < len ||
	    self.total - info0_at < ms2samples(60, self.srate))
		return;
	for (k = 0; k < len; k++)
		f[k] = k < 4 ? 1 : h[(n - len + k) % CC_HIST];
	for (k = 4; k < 12; k++)
		if (f[k] != ((0x72 >> (11 - k)) & 1))
			return;
	if (!info_crc_ok(f, len - 16))
		return;
	/* no reversals within the frame */
	if (self.ev_rev &&
	    self.total - self.rev_time < static_cast<s32>(len) * CC_SPS)
		self.ev_rev = 0;
	info0_at = self.total;
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
template <class P>
bool V34Info0<P>::info0_resend(s32 c)
{
	P &self = static_cast<P &>(*this);
	bool qc_late = self.qc_done && self.ev_info0 && !info0_again;
	bool repeated = info0_rep > 0 &&
		self.total - info0_tx_at >= ms2samples(400, self.srate);
	bool missed = !self.ev_info0 && self.tone_blocks >= 3 &&
		c >= ms2samples(500, self.srate);

	info0_rep = 0;
	if (self.retraining || self.short2 || info0_sent >= 16 ||
	    !(qc_late || repeated || missed))
		return false;
	DSP_DBG("v34: INFO0 again (%s)\n", qc_late ? "after QC" :
		!repeated ? "no INFO0 from the remote" :
		info0_rep_ack ? "the remote's acknowledges ours" :
		"the remote has not taken ours");
	info0_rec = info0_recovered = 1;
	info0_acked = qc_late || (repeated && info0_rep_ack);
	info0_again = self.ev_info0;
	self.cc_rev = 0;
	self.set_state(self.caller ? P2C_INFO0 : P2A_INFO0);
	make_info0(&self.cctx);
	self.cc_send();
	return true;
}

/* at the end of one of our INFO0s in the recovery: another, until the
   remote acknowledges ours or stops repeating its own (250 ms without
   it: its tone); whether it went out */
template <class P>
bool V34Info0<P>::info0_more()
{
	P &self = static_cast<P &>(*this);
	if (!info0_rec)
		return false;
	if (info0_acked || info0_sent >= 16 ||
	    (self.ev_info0 &&
	     self.total - info0_at > ms2samples(250, self.srate))) {
		info0_rec = 0;
		return false;
	}
	info0_again = self.ev_info0;
	make_info0(&self.cctx);
	self.cc_send();
	return true;
}

/* a frame at the phase of the last sample: the frames do not depend on
   the tracked symbol timing, which may slip at the start of one (the
   remote's INFO1 comes at a new phase after its probing signals) */
template <class P>
void V34Cc<P>::cc_phase_frame()
{
	P &self = static_cast<P &>(*this);
	CcRx *r = &ccr;
	s16 i = r->ph_last, len = self.info_expected(), k;
	s32 n = r->ph_n[i];
	const unsigned char *h = r->ph[i];

	self.rt_info0_frame(h, n);
	self.info0_repeat_frame(h, n);
	if (ev_info1 || n < len)
		return;
	for (k = 0; k < 8; k++)
		if (h[(n - len + 4 + k) % CC_HIST] != ((0x72 >> (7 - k)) & 1))
			return;
	for (k = 0; k < len; k++)
		r->frame[k] = k < 4 ? 1 : h[(n - len + k) % CC_HIST];
	if (!info_crc_ok(r->frame, len - 16) ||
	    (ev_info0 && info_crc_ok(r->frame, self.info0_rx_len() - 16)))
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
template <class P>
void V34Cc<P>::cc_rx_bit(s16 bit)
{
	P &self = static_cast<P &>(*this);
	CcRx *r = &ccr;

	if (dsplibs_debug_level >= 6)
		DSP_DBG("v34: cc %s %ld %d p %.0f t %d\n",
			self.caller ? "C" : "A",
			static_cast<long>(self.total) * 1000 / self.srate, bit,
			r->p, r->best);
	r->sr = (r->sr << 1) | bit;
	r->hist[r->hist_n++ % CC_HIST] = bit;
	self.rt_info0_frame(r->hist, r->hist_n);
	self.info0_repeat_frame(r->hist, r->hist_n);
	if (r->frame_len > 0)
		r->frame_len--;
	self.mh_check(r);
	/* a frame sync: slow timing adaptation for the rest of the frame */
	if ((r->sr & 0xFF) == 0x72)
		r->frame_len = self.info_expected();
	/* whether the last bits are a frame (fill, sync, CRC): the timing
	   may need a few of the fill bits after the probing signals */
	if (!ev_info1) {
		s16 len = self.info_expected(), i, ok;
		if (r->hist_n >= len - 3) {
			for (i = 0; i < len; i++)
				r->frame[i] = i < 4 ? 1 :
					r->hist[(r->hist_n - len + i) % CC_HIST];
			ok = info_crc_ok(r->frame, len - 16);
			for (i = 4; i < 12 && ok; i++)
				ok = r->frame[i] == ((0x72 >> (11 - i)) & 1);
			/* (not an INFO1: the remote's INFO0 again and what
			   follows it) */
			if (ok && ev_info0 &&
			    info_crc_ok(r->frame, self.info0_rx_len() - 16))
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
			r->rev_cand = self.total;
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
				static_cast<long>(r->rev_cand) * 1000 /
				self.srate);
		}
		r->rev_cand = 0;
		r->zeros++;
	}
}

/* V.92's MH sequences (Table 32/V.92): fill, frame sync, the signal
   (4 bits), its information (4 bits), the CRC: 36 bits up to the CRC's
   end, on the modulation of INFO */
template <class P>
void V34Moh<P>::mh_check(CcRx *r)
{
	P &self = static_cast<P &>(*this);
	unsigned char f[36];
	s16 i;

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
	mh_seen = self.total;
	if (dsplibs_debug_level >= 4)
		DSP_DBG("v34: MH %x %x at %ld ms\n", mh_sig, mh_info,
			static_cast<long>(self.total) * 1000 / self.srate);
}

/* an MH sequence: fill, sync, the signal and its information (both
   written first bit first in time, as in Table 32/V.92), CRC, fill */
void BitQ::make_mh(s16 sig, s16 info)
{
	s16 i;
	info_start();
	for (i = 3; i >= 0; i--)
		put((sig >> i) & 1, 1);
	for (i = 3; i >= 0; i--)
		put((info >> i) & 1, 1);
	info_end();
}

/* for the log */
static const char *mh_name(s16 sig)
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
static s16 moh_t1_secs(s16 code)
{
	static const s16 t[] = { 0, 10, 20, 30, 40, 60, 120, 180, 240,
				     360, 480, 720, 960, 0 };
	return code > 0 && code < 14 ? t[code] : 60;
}

/* an MH sequence from the remote, in a retrain (that is how it begins:
   its tone A, then MH sequences instead of the reversal) or while we
   answer one */
template <class P>
s16 V34Moh<P>::mh_event()
{
	P &self = static_cast<P &>(*this);
	s16 in_retrain = self.retraining && !self.prx_on &&
		(self.state == V34_RT_SILENCE ||
		 (self.state >= P2A_INFO0 && self.state <= P2C_WAIT_INFO1));

	if (!self.connected || !(self.v92 || self.v92a_on))
		return DPSTAT_OK;
	if (self.state == MOH_REQ) {
		/* the answer to ours (Table 34/V.92) */
		if (moh_req == MH_REQ && mh_sig == MH_ACK) {
			moh_req_t1 = mh_info;
			DSP_DBG("v34: MHack, T1 %d s: the remote on hold for "
				"%d s\n", moh_t1_secs(mh_info), moh_req_hold);
			/* then our silence (9.10.2.1/V.92), for longer than
			   a retrain's start-up may take */
			self.cc_silence();
			self.txm = TXM_SILENCE;
			self.timeout = self.total +
				ms2samples(1000L * moh_req_hold + 60000,
					   self.srate);
			self.set_state(MOH_HELD);
		} else if (moh_req == MH_REQ && mh_sig == MH_NACK) {
			/* denied: a fast reconnect (or a cleardown,
			   with DSP_V92_MOH_REQ's hold time 0) */
			moh_req = moh_req_hold ? MH_FRR : MH_CDA;
			DSP_DBG("v34: MHnack: %s\n", mh_name(moh_req));
			self.cctx.make_mh(moh_req, moh_req);
			self.cc_send();
			self.count = 0;
		}
		return DPSTAT_OK;
	}
	if (!in_retrain && self.state != MOH_RESP)
		return DPSTAT_OK;
	switch (mh_sig) {
	case MH_REQ:
		if (self.state == MOH_RESP)
			break;
		moh_respond(moh_t1 ? MH_ACK : MH_NACK);
		break;
	case MH_CLRD:
		if (self.state != MOH_RESP || moh_resp != MH_CDA)
			moh_respond(MH_CDA);
		break;
	case MH_FRR:
		/* its fast reconnect: ANSam, phase 1 (9.10.2.3/V.92) */
		if (self.state != MOH_RESP || moh_resp == MH_NACK) {
			DSP_DBG("v34: MHfrr: fast reconnect\n");
			moh_hold(10);
		}
		break;
	case MH_CDA:
		/* after our MHnack it would rather clear down */
		if (self.state == MOH_RESP && moh_resp == MH_NACK) {
			DSP_DBG("v34: MHcda: cleardown\n");
			moh_drop = 1;
		}
		break;
	}
	return DPSTAT_OK;
}

/* answer the remote's MH sequence with sig (MHack with our T1 code,
   MHnack or MHcda), repeated in MOH_RESP */
template <class P>
void V34Moh<P>::moh_respond(s16 sig)
{
	P &self = static_cast<P &>(*this);
	DSP_DBG("v34: %s: %s%s\n", mh_name(mh_sig), mh_name(sig),
		sig == MH_ACK ? " (on hold)" : "");
	moh_resp = sig;
	moh_drop = 0;
	self.cctx.make_mh(sig, sig == MH_ACK ? moh_t1 : sig);
	self.cc_send();
	self.rt_blocks = self.rt_heard = self.quiet_blocks = 0;
	self.set_state(MOH_RESP);
}

/* on hold (or a fast reconnect): ANSam, and phase 1 as the answering
   modem when the remote comes back (QC1a or CM), or a disconnect after
   secs (0: no limit) */
template <class P>
void V34Moh<P>::moh_hold(s16 secs)
{
	P &self = static_cast<P &>(*this);
	DSP_DBG("v34: on hold%s, ANSam for %d s\n",
		moh_resp == MH_ACK ? "" : " (reconnect)", secs);
	moh_end = secs ? self.total + static_cast<s32>(secs) * self.srate :
		-1;
	/* phase 1 from scratch (9.10.2.1/V.92: what earlier phase 1
	   signals said does not count) */
	self.caller = 0;
	self.pcm = self.v90a_on = 0;
	self.v8_ok = self.v8_cj = self.v8_sync = 0;
	self.v8_n = self.v8_last_n = 0;
	self.ev_qc = self.qc_done = 0;
	self.qc_n = -1;
	self.aa_blocks = self.toneq_blocks = 0;
	self.cc_silence();
	self.cctx.pos = self.cctx.n;
	/* CM comes on V.21's channel 1 (the call's caller, V.90's
	   analogue modem, held its receiver on channel 2) */
	self.v21.init(980, 1180, 300, self.srate);
	self.ansam_sign = 1;
	self.txm = TXM_ANSAM;
	/* the start-up after it as after a retrain */
	self.timeout = (moh_end > 0 ? moh_end :
			self.total + static_cast<s32>(self.srate) * 86400) +
		ms2samples(60000, self.srate);
	self.set_state(V8A_ANSAM);
}

/* back from holding the remote (its MHack to our MHreq): phase 1 from
   scratch as the call modem once its ANSam is there (MOH_BACK) */
template <class P>
void V34Moh<P>::moh_back()
{
	P &self = static_cast<P &>(*this);
	DSP_DBG("v34: taking the remote back from hold\n");
	self.caller = 1;
	self.pcm = self.v90a_on = 0;
	self.v8_ok = self.v8_cj = self.v8_sync = 0;
	self.v8_n = self.v8_last_n = 0;
	self.ev_qc = self.qc_done = 0;
	self.qc_n = -1;
	self.cc_silence();
	self.cctx.pos = self.cctx.n;
	self.txm = TXM_SILENCE;
	/* CM on V.21's channel 1, JM on 2 */
	self.v21.init(1650, 1850, 300, self.srate);
	self.ans_peak = 0;
	self.ans_gone = 0;
	moh_ans = 0;
	moh_backed = 1;
	self.timeout = self.total + ms2samples(90000, self.srate);
	self.set_state(MOH_BACK);
}

/* modem-on-hold of ours in data mode: the DTE's (+PMHR, the remote on
   hold for its seconds), or test: DSP_V92_MOH_REQ=<ms>[:<s>], that far
   into data mode (on hold for s seconds, default 10; 0: MHcda if it
   denies it) */
template <class P>
void V34Moh<P>::moh_req_test()
{
	P &self = static_cast<P &>(*this);
	/* the DTE's (+PMHR), once a block */
	if (moh_poll++ % 160 == 0 && self.connected &&
	    (self.v92 || self.v92a_on) && !self.retraining &&
	    (self.state == V34_DATA || self.state == V92_RUN ||
	     self.state == V90A_DATA)) {
		s32 secs = static_cast<s32>(
			modem_get_param(self.modem, MDMPRM_MOH_REQ));
		if (secs > 0) {
			modem_set_param(self.modem, MDMPRM_MOH_REQ, 0);
			moh_request(static_cast<s32>(secs));
			return;
		}
	}
	/* (once a sample: V.92's data mode keeps no total) */
	if (!moh_req_at || !self.connected || !(self.v92 || self.v92a_on) ||
	    self.retraining ||
	    (self.state != V34_DATA && self.state != V92_RUN &&
	     self.state != V90A_DATA))
		return;
	if (++moh_req_n >= moh_req_at) {
		moh_req_at = 0;
		moh_request(moh_req_hold);
	}
}

/* ours (9.10.2.1/V.92): out of data mode as for a retrain, tone B, then
   MHreq (MOH_REQ); the remote on hold for hold_secs (at most its T1) if
   it grants it */
template <class P>
void V34Moh<P>::moh_request(s32 hold_secs)
{
	P &self = static_cast<P &>(*this);
	self.retrain("modem-on-hold request", RT_HOLD);
	moh_req = MH_REQ;
	moh_req_hold = hold_secs;
	moh_req_t1 = 0;
	self.cc_tone();
	self.set_state(MOH_REQ);
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
	reinit(*this);
}

/* see V34Echo */
void V34Echo::ls_free()
{
	free_buffer(ls_tx);
	free_buffer(ls_rx);
}

/* see V34Echo: the normal equations of the near part over the n stored
   samples, its matrix's rows each the one before moved on a sample (the
   window's first in, its last out), solved by Cholesky */
float V34Echo::ls_solve()
{
	const s16 L = EC_NEAR_LEN, T = EC_NEAR_LEN - 1;
	std::vector<double> R_, ch_;
	double *R, *ch, p[EC_NEAR_LEN], w[EC_NEAR_LEN], z[EC_NEAR_LEN];
	double ryy = 0, rest, tr = 0, lam;
	const float *t = ls_tx.data(), *y = ls_rx.data();
	s32 n, N = ls_n;
	s16 i, j, m;
	float gain = 0;

	if (ls_tx.empty() || ls_rx.empty() || N < 4 * L || far_on)
		goto out;
	if (!alloc_zeroed(R_, static_cast<size_t>(L) * L) ||
	    !alloc_zeroed(ch_, static_cast<size_t>(L) * L))
		goto out;
	R = R_.data();
	ch = ch_.data();
	/* t[T + n] is our sample with the line's y[n] */
	for (j = 0; j < L; j++) {
		double r = 0, q = 0;
		for (n = 0; n < N; n++) {
			r += static_cast<double>(t[T + n]) * t[T + n - j];
			q += static_cast<double>(y[n]) * t[T + n - j];
		}
		R[j] = r;
		p[j] = q;
	}
	for (n = 0; n < N; n++)
		ryy += static_cast<double>(y[n]) * y[n];
	for (i = 0; i < L - 1; i++)
		for (j = i; j < L - 1; j++)
			R[(i + 1) * L + j + 1] =
				R[i * L + j] +
				static_cast<double>(t[T - 1 - i]) *
					t[T - 1 - j] -
				static_cast<double>(t[T + N - 1 - i]) *
					t[T + N - 1 - j];
	for (i = 0; i < L; i++)
		tr += R[i * L + i];
	/* a little on the diagonal: our signal leaves the band's edges
	   empty */
	lam = 1e-7 * tr / L + 1e-6;
	for (i = 0; i < L; i++)
		for (j = 0; j <= i; j++) {
			double v = j < i ? R[j * L + i] : R[i * L + i] + lam;
			for (m = 0; m < j; m++)
				v -= ch[i * L + m] * ch[j * L + m];
			if (i == j)
				ch[i * L + i] = sqrt(v > 1e-12 ? v : 1e-12);
			else
				ch[i * L + j] = v / ch[j * L + j];
		}
	for (i = 0; i < L; i++) {
		double v = p[i];
		for (m = 0; m < i; m++)
			v -= ch[i * L + m] * z[m];
		z[i] = v / ch[i * L + i];
	}
	for (i = L - 1; i >= 0; i--) {
		double v = z[i];
		for (m = i + 1; m < L; m++)
			v -= ch[m * L + i] * w[m];
		w[i] = v / ch[i * L + i];
	}
	rest = ryy;
	for (i = 0; i < L; i++)
		rest -= w[i] * p[i];
	gain = static_cast<float>(10 *
				  log10((ryy + 1) / (rest > 0 ? rest + 1 : 1)));
	/* in place of NLMS's when it does better */
	if (gain > 10 * log10f((pw_rx + 1) / (pw_err + 1))) {
		for (i = 0; i < L; i++)
			wn[i] = static_cast<float>(w[i]);
		pw_err = pw_rx * powf(10, -gain / 10);
	}
out:
	ls_free();
	return gain;
}

/* where the far echo is: the received signal of the last EC_SEARCH
   samples against our (scrambled, nearly white) transmitted signal at
   every delay beyond the near part.  The far part then covers
   EC_FAR_LEN samples from a little before the strongest echo. */
void V34Echo::search()
{
	const s16 mask = EC_HIST - 1;
	float best = 0, run = 0, c, floor;
	s16 d, k, best_d = -1;
	/* only where our signal has been sent */
	s32 n_ = filled - EC_SEARCH;

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
	std::sort(win_, win_ + n_);
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
	const s16 mask = EC_HIST - 1;
	float est = 0, est_far = 0, en = 1, err, g, h;
	s16 i, p;

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
	pw_near += 0.001f * (est * est - pw_near);
	if (far_on)
		for (i = 0, p = (pos - far_delay) & mask; i < EC_FAR_LEN;
		     i++, p = (p - 1) & mask) {
			h = hist[p];
			est_far += wf[i] * h;
			en += h * h;
		}
	pw_far += 0.001f * (est_far * est_far - pw_far);
	est += est_far;
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
	/* stored for ls_solve() while it trains */
	if (ls && mu > 0) {
		if (ls_tx.empty() && ls_rx.empty() && !ls_n) {
			if (alloc_zeroed(ls_tx, EC_LS_MAX + EC_NEAR_LEN))
				alloc_zeroed(ls_rx, EC_LS_MAX);
			if (!ls_tx.empty())
				for (i = 0; i < EC_NEAR_LEN - 1; i++)
					ls_tx[i] = hist[(pos - (EC_NEAR_LEN -
							 1) + i) & mask];
		}
		if (!ls_tx.empty() && !ls_rx.empty() && ls_n < EC_LS_MAX) {
			ls_tx[EC_NEAR_LEN - 1 + ls_n] = tx;
			ls_rx[ls_n++] = rx;
		}
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

/* the caller in phase 3: our receiver still decodes the remote's J (one
   in the last three J's time) */
bool V34Pump::remote_j() const
{
	return prx.on && prx.stage == V34_RX_TRN &&
		prx.bit_n - prx.j_last <= 48;
}

/* phase 3: the far echo found only late in our training (a long round
   trip), its part of the canceller not trained for half a second yet */
bool V34Pump::ec_far_late() const
{
	return ec.on && ec.far_on && ec.far_since < ms2samples(500, srate);
}

/* the adaptation, per sample: train while our own phase 3 sequences go
   out (the remote is silent then), then track */
void V34Pump::ec_schedule()
{
	V34Echo *e = &ec;
	s16 ours = state == P3A_S || state == P3A_SBAR ||
		state == P3A_PP || state == P3A_TRN ||
		state == P3C_S || state == P3C_SBAR ||
		state == P3C_PP || state == P3C_TRN ||
		/* V.90: Sd to Jd, the analogue modem is silent after Ja */
		state == P3D_SD || state == P3D_JD ||
		/* V.92 PCM upstream: the same, until Su */
		(state == V92_RUN && v92u.ec_train()) ||
		/* V.90's analogue modem: S to Ja, until the digital
		   modem's Sd */
		(v90a_rx && (state == P3V_S || state == P3V_SBAR ||
			     state == P3V_PP || state == P3V_TRN ||
			     (state == P3V_JA &&
			      v90rx.stage == V90Rx::WAIT_SD))) ||
		/* and V.92's: Ru to Ja */
		(v90a_rx && state == P3U_JA &&
		 v90rx.stage == V90Rx::WAIT_SD);
	s16 training;

	if (ours && !e->on) {
		e->on = 1;
		e->ls = v90a_rx;
		e->ls_free();
		e->ls_n = 0;
		e->n = e->since = e->filled = 0;
		e->far_on = 0;
		e->judged = e->bypass = e->trained = 0;
		e->waited = 0;
		e->pw_rx = e->pw_err = e->es = e->pw_est = 0;
		e->dt = 0;
		DSP_DBG("v34: echo canceller on\n");
	}
	if (ours)
		e->since++;
	/* the answerer sends its J until it has heard the caller's S.  The
	   caller: not before its J is over, as long as our receiver still
	   decodes it.  The round trip measured in phase 2 can be well short
	   of the line's (a reversal of the remote's phase 2 recovery taken
	   for its answer to ours), and the remote takes a while to hear our
	   S: trained on with the remote's J in the error, the canceller
	   takes next to nothing away (and is turned off), and what it gives
	   the receiver meanwhile drags its equalizer and carrier off, so
	   that it misses the remote's S of phase 4.  When the echo hides the
	   J from the receiver, at the round trip measured as before */
	training = ours && (!caller || v90a_rx ||
		(e->since > rtd + ms2samples(150, srate) &&
		 (e->n || !remote_j())));
	if (ours && caller && !v90a_rx && !training &&
	    e->since > rtd + ms2samples(150, srate) && !e->waited) {
		e->waited = 1;
		DSP_DBG("v34: echo canceller: the remote's J still there, "
			"its end awaited\n");
	}
	if (training && !e->n && e->waited)
		DSP_DBG("v34: echo canceller: the remote's J over after "
			"%ld ms of ours\n",
			static_cast<long>(e->since) * 1000 / srate);
	if (training) {
		e->n++;
		/* the far echo is found after some of our signal went out
		   (and came back); on a long round trip (a second, as over
		   VoIP and the USB modems' tests) only late in the training:
		   looked for again each 250 ms after the first second, while
		   the training lasts */
		if (!e->far_on && (e->n == EC_SEARCH + ms2samples(50, srate) ||
				   e->n == ms2samples(250, srate) ||
				   e->n == ms2samples(500, srate) ||
				   (e->n >= ms2samples(1000, srate) &&
				    e->n % ms2samples(250, srate) == 0)))
			e->search();
		/* fast at first, also for a far part found late */
		e->far_since++;
		{
			s32 t = e->n;
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
						 (e->pw_err + 1)), g_ls;
			if (e->ls && (g_ls = e->ls_solve()) != 0) {
				DSP_DBG("v34: echo canceller: least squares "
					"%.1f dB, NLMS %.1f dB\n", g_ls, erle);
				if (g_ls > erle)
					erle = g_ls;
			}
			e->judged = 1;
			/* V.90: the receiver's error so far had our echo
			   in it; the precoder our MP asks for is to be found
			   from what comes now */
			if (v90_digital())
				prx.err_reset();
			/* (DSP_EC=0: off anyway, for tests) */
			e->bypass = erle < 6 || env_off("DSP_EC");
			DSP_DBG("v34: echo canceller took %.1f dB away%s\n",
				erle, e->bypass ? ": off" : "");
		}
	}
	/* the echoes it takes away, for the call's statistics */
	if (stats && e->on && e->judged && !e->bypass &&
	    total % ms2samples(100, srate) == 0) {
		stats->near_echo = 10 * log10f(e->pw_near / DP_DBM0_POWER +
					       1e-12f);
		stats->have |= DP_STATS_NEAR_ECHO;
		if (e->far_on) {
			stats->far_echo = 10 * log10f(e->pw_far /
						      DP_DBM0_POWER + 1e-12f);
			stats->have |= DP_STATS_FAR_ECHO;
		}
	}
}

/* the remote's tone A / B, not data: nearly all the power in its bin
   (data spreads over some 20) */
template <class P>
void V34Retrain<P>::rt_tone(float x)
{
	P &self = static_cast<P &>(*this);
	rt_e += x * x;
	own_g.run(x);
	mate_g.run(x);
	if (rt_g.run(x)) {
		/* without the echo of our own tone, and without 1800 Hz: the
		   Agere, Broadcom and HSF modems answer a retrain with tone A
		   and as much again at 1800 Hz (and Agere's asks for one so
		   in V.90) */
		float all = rt_e / PROBE_PERIOD - own_g.power;
		float e = all - mate_g.power;
		rt_e = 0;
		if (e < 0)
			e = 0;
		rt_blocks = all > tone_power(150) &&
			rt_g.power > (self.pcm ? 0.4f : 0.6f) * e ?
			rt_blocks + 1 : 0;
		if (dsplibs_debug_level >= 5 && retraining)
			DSP_DBG("v34: rt %ld e %.0f tone %.0f own %.0f\n",
				static_cast<long>(self.total) * 1000 /
				self.srate, e, rt_g.power, own_g.power);
		if (rt_blocks >= 5 && !rt_heard) {
			rt_heard = 1;
			rt_since = self.total - rt_blocks * PROBE_PERIOD;
			DSP_DBG("v34: tone %s heard\n",
				self.caller ? "A" : "B");
		}
	}
}

/* one received sample at 9600 Hz: before phase 2 V.8 (V.21, ANSam,
   QC1a, TONEq) and INFO0 after it; from phase 3 on the primary channel
   receiver after the echo canceller; otherwise the control channel, the
   remote's tone and L1/L2 (phase 2, retrains) */
void V34Pump::pump_rx_sample(float x)
{
	s16 bit, k;

	if (state < P2A_INFO0) {
		bit = v21.run(x);
		if (bit >= 0 && v21.carrier)
			v8_rx_bit(bit);
		if (state == V8A_JM)
			v8_quiet_run(x);
		if (state == V8A_JM || state == V8A_GAP ||
		    state == V8C_CJ || state == V8C_GAP ||
		    state == QCA_TONEQ || state == QCA_GAP2 ||
		    state == QCC_GAP) {
			bit = ccr.sample(x);
			if (bit >= 0)
				cc_rx_bit(bit);
			cc_phase_frame();
		}
		if (state == V8C_LISTEN || state == MOH_BACK ||
		    state == QCC_WAIT || state == QCC_TONEQ)
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
	if (v90a_rx) {
		/* the digital modem's PCM (V90Rx takes the line at its
		   9600 Hz), our echo taken away; on an 8000 Hz line it has
		   the line's own samples in process() (the converter to
		   9600 Hz would cut the top of the band) */
		if (line_rate == V34_SRATE) {
			ec_schedule();
			if (!ec.bypass)
				x = ec.run(last_tx, x);
			v90rx.line(x);
		}
		/* the digital modem's tone B: a retrain (9.5.2.2/V.90) */
		rt_tone(x);
		/* the data mode's bits to the core */
		if (state == V90A_DATA && v90rx.qn) {
			unsigned char b[V90RX_Q];
			s16 m = v90rx.take(b, V90RX_Q);
			CoreBits{ modem }.put(b, m);
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
				static_cast<long>(l1_start) * 1000 / srate);
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
s16 V34Pump::fallback(const char *why)
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
static s16 v34_info0_allows(s16 rates, s16 b, s16 high)
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
static s16 v34_caller_carrier(s16 rates, s16 b)
{
	s16 low = b == V34_BAUD_3000 ? 0x08 : 0x20;
	if (b != V34_BAUD_3000 && b != V34_BAUD_3200)
		return 0;       /* (no choice there) */
	return rates & low ? 0 : rates & (low << 1) ? 1 : -1;
}

/* the answerer picks the symbol rates: the fastest both modems allow,
   at a carrier the caller can transmit on (11.2/V.34, Table 16/V.34) */
void V34Pump::choose_bauds()
{
	s16 b, best = V34_BAUD_2400, best_rate = 0, best_high = 0;

	probe_analyse();
	for (b = V34_BAUD_2400; b <= V34_BAUD_3429; b++) {
		s16 rate = rate_of_baud[b], high;
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

/* the analogue modem of V.90 after INFO1d (9.2.2.1.9, Table 10/V.90):
   its upstream symbol rate, of 3000, 3200 and 3429 (when INFO0d allows
   it) the one INFO1d gives the most for, on the carrier INFO1d asks for
   if our INFO0a has it; the 2 point train's Ucode (UINFO, above 66),
   the loudest within the digital modem's most (INFO0d), as 0 dBm0 is
   3.17 dB (A-law 3.14) below the sine at G.711's overload.  Without
   a symbol rate (INFO1d has none): V.34 instead, v90a_on 0 */
void V34Pump::v90a_choose()
{
	s16 b, best = -1, best_rate = 0, u;

	for (b = V34_BAUD_3000; b <= V34_BAUD_3429; b++) {
		s16 rate = rinfo1c.r[b].max_rate;
		if (b == V34_BAUD_3429 && !rinfo0.up3429)
			continue;
		if (!v34_info0_allows(info0_rates, b, b == V34_BAUD_3429 ? -1 :
				      rinfo1c.r[b].high))
			continue;
		if (rate && rate >= best_rate) {
			best_rate = rate;
			best = b;
		}
	}
	if (best < 0) {
		DSP_DBG("v34: INFO1d: no upstream symbol rate for V.90, "
			"V.34\n");
		v90a_on = 0;
		return;
	}
	baud_tx = best;
	u = v90a_pick_uinfo();
	DSP_DBG("v34: V.90 upstream %d baud (%" PRId32 " bit/s), UINFO %d\n",
		v34_bauds[best].nominal, best_rate * INT32_C(2400), u);
}

/* the 2 point train's Ucode (v90a_uinfo): the loudest within INFO0d's
   most power, 68 at least (Table 10/V.90) */
s16 V34Pump::v90a_pick_uinfo()
{
	double most = -0.5 - rinfo0.max_power / 2.0, rms0;
	s16 u;

	rms0 = (alaw ? 32768 : 32636) / M_SQRT2 /
		pow(10, (alaw ? 3.14 : 3.17) / 20);
	for (u = 127; u > 67 &&
	     20 * log10(v90_ucode_linear(alaw, u) / rms0) > most; u--)
		;
	v90a_uinfo = u;
	DSP_DBG("v34: UINFO %d (%.1f dBm0)\n", u,
		20 * log10(v90_ucode_linear(alaw, u) / rms0));
	return u;
}

/* the primary channel at the agreed symbol rates; -1 if the receiver
   can't be set up (out of memory) */
s16 V34Pump::primary_start()
{
	/* the carriers as the receiving modems asked for them in INFO1c
	   (answerer -> caller) and INFO1a (caller -> answerer) */
	s16 high = caller ? high_tx : rinfo1c.r[baud_tx].high;
	ptx.init(baud_tx, high, caller);
	if (prx.init(baud_rx, caller ? 0 : high_rx, caller))
		return -1;
	prx_on = 1;
	txm = TXM_PRIMARY;
	return 0;
}

/* the analogue modem's phase 3 (9.3.2/V.90): our V.34 transmitter at
   the upstream symbol rate on the carrier INFO1d asked for (the answer
   modem's scrambler), Ja with the DIL descriptor, the PCM receiver on
   the line at 8000 Hz.  The DIL: every Ucode, from the loudest, 36
   symbols each (H 5: 6
   in each frame interval), no reference symbols (TP 1), signs SP of 12
   whose second half is the first's opposite (each frame interval has
   3 of each sign in a segment); DSP_V90A_DIL=0 asks for none (N 0: the
   digital modem goes to phase 4 after Jd') */
void V34Pump::v90a_start()
{
	static const unsigned char sp[12] = { 1, 0, 1, 1, 0, 0,
					      0, 1, 0, 0, 1, 1 };
	s16 i;
	ptx.init(baud_tx, rinfo1c.r[baud_tx].high, 0);
	zero(v90a_dil);
	v90a_dil.lsp = v90a_dil.ltp = 1;
	if (env_on("DSP_V90A_DIL")) {
		v90a_dil.n = 128;
		/* the loudest first: what clips spills over only into
		   the louder ones after it */
		for (i = 0; i < 128; i++)
			v90a_dil.ucode[i] = static_cast<unsigned char>(127 - i);
		for (i = 0; i < 8; i++) {
			v90a_dil.h[i] = 5;
			v90a_dil.ref[i] = v90a_uinfo;
		}
		v90a_dil.lsp = 12;
		std::copy(std::begin(sp), std::end(sp), v90a_dil.sp);
		v90a_dil.tp[0] = 1;
	}
	ptx.nbits = v90a_dil.build(ptx.bits);
	txm = TXM_PRIMARY;
	v90rx.init(alaw, v90a_uinfo, line_rate);
	v90rx.dil_set(v90a_dil);
	v90a_rx = 1;
	/* the PCM codes' DC is theirs: the core's DC remover would take
	   away a stepwise estimate of it, which no equaliser undoes */
	if (rt)
		rt->pcm = 1;
}

/* the analogue modem's phase 4 (9.4.2/V.90): the constellations of
   CPt and CP from what the PCM receiver found, within the power
   INFO0d allows (Table 15/V.90); CPt from cleared scrambler and
   differential encoder (8.5.2/V.90), the PCM receiver waiting for Ri.
   -1 when there are no constellations to ask for */
s16 V34Pump::v90a_phase4()
{
	static const s16 lim[32] = {
		15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108,
		9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380, 6020, 5684,
		5368, 5068, 4784, 4516, 4264, 4024, 3800, 3588, 3388, 3196,
		3020, 2852, 2692, 2540 };
	float lv[6][128];
	s16 m = rinfo0.max_power;
	V90Con c;

	v90a_limit = lim[m < 0 ? 0 : m > 31 ? 31 : m];
	v90rx.levels(lv);
	if (v90a_design_cpt(&v90a_cpt, alaw, v90rx.lin, lv, v90a_limit) ||
	    v90rx.phase4(v90a_cpt)) {
		DSP_DBG("v34: V.90: no CPt constellations\n");
		return -1;
	}
	c.init(v90a_cpt, v90a_cpt.drn + 8, lv);
	v90a_cpt_pw = c.power(v90rx.lin);
	ptx.nbits = v90a_cpt.build(ptx.bits);
	ptx.scr = 0;
	ptx.idx = 0;
	tx_next(P4V_CPT, V34_TX_CP);
	return 0;
}

/* our INFO1a asking for PCM upstream (Table 18/V.92): the precoder's
   and prefilter's p1 and z2 with the fewest coefficients, no MD, the
   2 point train's Ucode, 8000 symbols a second both ways */
void V34Pump::make_info1a_v92(BitQ *q)
{
	q->info_start();
	q->put(0, 2);        /* p1, z2 */
	q->put(0, 2);        /* Ltot 192 */
	q->put(0, 2);        /* Lmax 128 */
	q->put(0, 7);        /* no MD */
	q->put(v90a_uinfo, 7);
	q->put(0, 2);
	q->put(6, 3);
	q->put(6, 3);
	q->put(0x3FF, 10);
	q->info_end();
}

/* V.92's PCM upstream phase 3 as the analogue modem: V.90's PCM
   receiver and DIL (v90a_start()), Jp in Jd's place, our signals from
   the PAM transmitter; Ja's DIL descriptor asks for every upstream rate
   (bits 0 to 18: 24 000 to 48 000 bit/s) */
void V34Pump::v92a_up_start()
{
	v90a_start();
	v90rx.v92 = 1;
	patx.init(line_rate, V92A_LU);
	patx.nbits = v92_dil_build(patx.bits, v90a_dil, 0x7FFFF);
	txm = TXM_PAM;
	DSP_DBG("v34: V.92: PCM upstream phase 3, LU %.0f\n", V92A_LU);
}

/* CPt's constellations (as V.90's analogue modem designs them) into
   the transmitter's bits in V.92's form; the PCM receiver waits for
   Ri.  -1 when there are none */
s16 V34Pump::v92a_cpt()
{
	static const s16 lim[32] = {
		15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108,
		9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380, 6020, 5684,
		5368, 5068, 4784, 4516, 4264, 4024, 3800, 3588, 3388, 3196,
		3020, 2852, 2692, 2540 };
	float lv[6][128];
	s16 m = rinfo0.max_power;
	V90Con c;

	v90a_limit = lim[m < 0 ? 0 : m > 31 ? 31 : m];
	v90rx.levels(lv);
	if (v90a_design_cpt(&v90a_cpt, alaw, v90rx.lin, lv, v90a_limit) ||
	    v90rx.phase4(v90a_cpt))
		return -1;
	c.init(v90a_cpt, v90a_cpt.drn + 8, lv);
	v90a_cpt_pw = c.power(v90rx.lin);
	patx.nbits = v92_cpt_build(patx.bits, v90a_cpt);
	DSP_DBG("v34: V.92: CPt, drn %d (%d bits)\n", v90a_cpt.drn,
		patx.nbits);
	return 0;
}

/* phase 4's next sequence after the one going out (9.6.2.1/V.92):
   CPu once SUVd is in (again if no acknowledgement 100 ms and a round
   trip after it went), else SUVu; acknowledged once CPd is in.  -1
   when there are no constellations for CPu.  In a fast parameter
   exchange (fpe) the sequences are in the data mode modulation, CPus
   in the digital modem's (fpe 1: the downstream as it is), CPu in ours
   (2: designed on the data mode's error) */
s16 V34Pump::v92a_next_seq(s16 fpe)
{
	s16 ack = v90rx.ev_cpd > 0, fill = fpe ? patx.K : 24;
	s32 again = ms2samples(100, srate) + rtd +
		ms2samples(1000L * v92a_cpu_n / 2 / 8000, srate);
	if (v90rx.ev_suvd && v90rx.ev_trn2 &&
	    (!v92a_cpu || (!v90rx.ev_mpack && total - v92a_cpu_at > again))) {
		if (fpe == 1) {
			/* the downstream as it is: CPus */
			patx.nxt_n = v92_cpus_build(patx.nxt, v90a_cp.drn, ack,
						    fill);
			if (!v92a_cpu)
				DSP_DBG("v34: V.92: FPE: CPus\n");
		} else {
			if (!v92a_cpu) {
				if (v90a_cp_design(fpe))
					return -1;
				DSP_DBG("v34: V.92: %sCPu, drn %d\n",
					fpe ? "FPE: " : "", v90a_cp.drn);
			}
			v90a_cp.ack = ack;
			patx.nxt_n = v92_cpu_build(patx.nxt, v90a_cp, fill);
		}
		v92a_cpu_n = patx.nxt_n;
		v92a_cpu++;
		v92a_cpu_at = total;
	} else
		patx.nxt_n = v92_suvu_build(patx.nxt, ack, fill);
	if (ack && !v92a_acked) {
		/* (after the one going out and this one) */
		v92a_acked = 1;
		v92a_ack_seqs = patx.seqs + 2;
	}
	return 0;
}

/* a rate renegotiation of PCM upstream as the analogue modem (9.8.2/
   V.92): who 1 the digital modem's (its Rd-bar is in), 2 ours; our Ru
   at the next data frame's start, Ru-bar, TRN2u, then phase 4 again
   (its TRN2d, SUVd, CPd, Ed, B1d on the PCM receiver: V.90's
   renegotiation frames), the data on at the new rates.  No B1d in 25 s
   (the digital modem's own limit): a retrain */
void V34Pump::v92a_reneg(s16 who, const char *why)
{
	DSP_DBG("v34: V.92 renegotiation %d (%s%s%s), downstream error %.2f "
		"rms\n", ++v92a_renegs,
		who == 1 ? "the digital modem's Rd" : "ours", *why ? ": " : "",
		why, sqrtf(v90rx.data_mse()));
	v90a_bad = v90a_good = 0;
	stat_reneg(who == 1);
	v90a_rn = who;
	timeout = total + ms2samples(25000, srate);
	patx.fpe_req = 0;
	patx.ru_req = 1;
	set_state(P4U_RN);
}

/* see v92a_reneg(): a CPd our transmitter can take (its moduli enough
   for its bits, each class within its points) */
s16 V34Pump::v92a_cpd_ok(const V92Cpd &d)
{
	double lm = 0;
	s16 i, ok = d.drn >= 1 && d.ncon >= 1 && d.g > 0;
	for (i = 0; i < 12 && ok; i++) {
		ok = d.M[i] >= 1 && d.lc[d.con[i % 6]] > 0 &&
			d.M[i] <= 2 * d.lc[d.con[i % 6]];
		lm += log2(static_cast<double>(d.M[i]));
	}
	return ok && lm >= 2 * (d.drn + 17) - 1e-9;
}

/* a fast parameter exchange (9.9.2/V.92): who 1 the digital modem's
   (its Rf-bar is in), 2 ours (9.9.2.1: the digital modem answers our
   RM, RM' with Rf, Rf-bar).  RM at the next data frame's start, SUVu
   after RM' (P4U_FSEQ).  With a prefilter our transmitter's filters
   start anew at SUVu, as the digital modem's receiver has them */
void V34Pump::v92a_fpe(s16 who, const char *why)
{
	const V92Cpd &d = patx.cpd;
	s16 i, pf = d.coef[2][0] != 32767;
	for (i = 1; i < d.len[2]; i++)
		pf |= d.coef[2][i] != 0;
	for (i = 0; i < d.len[3]; i++)
		pf |= d.coef[3][i] != 0;
	DSP_DBG("v34: V.92 fast parameter exchange %d (%s%s%s)%s, "
		"downstream error %.2f rms\n", ++v92a_fpes,
		who == 1 ? "the digital modem's Rf" : "ours",
		*why ? ": " : "", why, pf ? ", the prefilter anew" : "",
		sqrtf(v90rx.data_mse()));
	stat_reneg(who == 1);
	v90a_bad = v90a_good = 0;
	v90a_rn = who;
	v92a_fpe_who = who;
	if (who == 2)
		v90rx.fpe_wait();
	timeout = total + ms2samples(25000, srate);
	patx.fpe_reset = pf;
	patx.nbits = v92_suvu_build(patx.bits, 0, patx.K);
	patx.nxt_n = 0;
	patx.e2u_req = 0;
	patx.fpe_req = 1;
	v92a_cpu = v92a_acked = 0;
	set_state(P4U_FSEQ);
}

/* our own rate renegotiation in the data mode: V.92's with PCM
   upstream, else V.90's */
void V34Pump::v90a_reneg_ours(const char *why)
{
	if (v92a_up_on)
		v92a_reneg(2, why);
	else
		v90a_reneg(2, why);
}

/* PCM upstream did not work: V.90 for the rest of the call, after a
   retrain */
s16 V34Pump::v92a_up_fail(const char *why)
{
	DSP_DBG("v34: V.92: PCM upstream: %s\n", why);
	v92a_noup = 1;
	return v90a_fail(0);
}

/* the upstream data mode (9.4.2.4/V.90): the fastest rate our CP
   enabled at or below the most the digital modem's MP receives (its
   rule: v90_data_setup()), with MP's trellis code, precoder and
   constellation; V.34's data encoder after our E */
s16 V34Pump::v90a_data_setup()
{
	const V90Mp &mp = v90rx.mp;
	s16 mask = v90a_cp.rate_mask & 0x1FFF, n;
	V34Params p;

	for (n = mp.rate_max; n >= 2; n--)
		if ((mask >> (n - 2)) & 1 && v34_rates[baud_tx][n - 1].b)
			break;
	if (n < 2 || mp.trellis > 2 || p.init(baud_tx, n, mp.expanded)) {
		DSP_DBG("v34: V.90: no upstream rate (MP's most %" PRId32 ")\n",
			mp.rate_max * INT32_C(2400));
		return -1;
	}
	if (mp.nonlinear)
		p.set_nonlinear(0.3125);
	p.states = 16 << mp.trellis;
	p.set_precoder(mp.h);
	ptx.start_data(&p, CoreBits{ modem });
	stat_coding(false, p.states, mp.nonlinear, p.precode, mp.expanded);
	rate_tx = p.rate;
	rate_rx = (v90a_cp.drn + 20) * INT32_C(8000) / 6;
	DSP_DBG("v34: V.90 rates: up %" PRId32 " (%d states%s), down %" PRId32
		"\n", rate_tx, p.states, mp.type ? ", precoded" : "", rate_rx);
	return 0;
}

/* CP's constellations (8.5.2/V.90), once TRN2d has shown the noise
   with our own signal going out: its echo, what the canceller leaves
   of it, is noise to the data mode as well (phase 3's measure had us
   silent); -1 when there are none.  For our fast parameter exchange
   (fpe) the noise is the data mode's decisions' error, and the
   receiver takes the constellations after its Ed */
s16 V34Pump::v90a_cp_design(s16 fpe)
{
	float lv[6][128], nz = v90rx.noise();
	/* (in a renegotiation the noise as it is now: what its frames'
	   decisions show) */
	if (v90rx.trn2_noise > nz || v90rx.renegs)
		nz = v90rx.trn2_noise;
	if (fpe)
		nz = sqrtf(v90rx.data_mse());
	v90a_cp_noise = nz;
	v90rx.levels(lv);
	if (v90a_design_cp(&v90a_cp, alaw, v90rx.lin, lv, nz, v90a_limit,
			   v90a_cpt_pw, v90rx.jd.rate_mask) ||
	    (fpe ? v90rx.data_cp_next(v90a_cp) : v90rx.data_cp(v90a_cp))) {
		DSP_DBG("v34: V.90: no CP constellations (noise %.1f)\n", nz);
		return -1;
	}
	return 0;
}

/* a rate renegotiation as the analogue modem (9.6.2/V.90): who 1 the
   digital modem's (its Rd-bar is in), 2 ours; S 128 T, S-bar 16 T, SCR
   until its TRN2d (or MP) shows the noise, then CP (designed again),
   CP', E, B1 as in phase 4, on its Rd's frames.  No Ed within 5000 ms
   plus 2 round trips of our S-bar: the call ends (a retrain is to
   come) */
void V34Pump::v90a_reneg(s16 who, const char *why)
{
	DSP_DBG("v34: V.90 renegotiation %d (%s%s%s), downstream error %.2f "
		"rms, CP's noise %.2f\n", v90rx.renegs + 1,
		who == 1 ? "the digital modem's Rd" : "ours", *why ? ": " : "",
		why, sqrtf(v90rx.data_mse()), v90a_cp_noise);
	v90a_bad = v90a_good = 0;
	stat_reneg(who == 1);
	v90a_rn = who;
	timeout = total +
		ms2samples(5000 + 128 * INT32_C(1000) / 2400, srate) +
		3 * rtd;
	tx_next(V90A_RN_S, V34_TX_S);
}

/* V.90 as the analogue modem did not get through phase 3 or 4, or a
   renegotiation: a retrain (9.5.2.1/V.90: silence, tone A, phase 2);
   with no constellations or upstream rate to use (no_pcm), or after
   two retrains, the next phase 2's INFO1a asks for V.34; after four
   the call ends */
s16 V34Pump::v90a_fail(s16 no_pcm)
{
	if (retrains >= 4) {
		DSP_DBG("v34: V.90: %d retrains, giving up\n", retrains);
		return DPSTAT_ERROR;
	}
	if (no_pcm || retrains >= 2)
		v90a_v34 = 1;
	retrain(v90a_v34 ? "V.90 failed, V.34 next" : "V.90 failed");
	return DPSTAT_OK;
}

/* V.90 phase 3: the remote's V.34 signals in, our PCM signals out */
s16 V34Pump::v90_start()
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
		const auto *fc = v90tx.fclock;
		auto fb = v90tx.fbase;
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
static s16 v34_rate_of_mse(s16 baud, float mse)
{
	float rs = v34_symbol_rate(baud), snr;
	s16 n;
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
void V34Pump::v90_make_mp(s16 ack)
{
	if (!max_rate_rx) {
		max_rate_rx = v34_rate_of_mse(baud_rx, prx.mse);
		DSP_DBG("v34: upstream at most %" PRId32 " bit/s (mse %.5f)\n",
			max_rate_rx * INT32_C(2400), prx.mse);
	}
	/* (V.90's upstream starts at 4800 bit/s, Table 17/V.90) */
	if (max_rate_rx < rate_floor())
		max_rate_rx = rate_floor();
	/* the precoder once for the exchange: every MP of it asks for the
	   same (the analogue modem may take the first, or the last), and
	   the data mode receiver takes it */
	if (!seq_len) {
		float gain = prx.precoder(mp_h);
		DSP_DBG("v34: V.90 precoding would gain %.2f dB (over %" PRId32
			" symbols): h %d %d %d %d %d %d\n", gain, prx.err_n,
			mp_h[0], mp_h[1], mp_h[2], mp_h[3], mp_h[4], mp_h[5]);
		if (gain < precode_min)
			zero(mp_h);
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
void V34Pump::fs_seq_next(s16)
{
	v90_make_mp(prx.ev_cp);
}

/* CPs': the remote acknowledged ours (or its E came) */
bool V34Pump::fs_acked()
{
	return prx.ev_cpack || prx.ev_e;
}

/* CPs': Ed, then silence until CP (9.6.1.2.5/V.90) */
bool V34Pump::fs_silence()
{
	return prx.cp_silence;
}

/* Ed's end: the CPs received by then kept (sil_cp) */
void V34Pump::fs_ed_end()
{
	sil_cp = prx.cp_count;
}

/* data mode: the core's data bits */
void V34Pump::fs_data(unsigned char *b, s16 n)
{
	CoreBits{ modem }.get(b, n);
}

/* the upstream data mode, as our MP asked for it */
s16 V34Pump::v90_data_setup()
{
	V34Params p;
	s16 n, mask = cp.rate_mask & 0x1FFF;

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
	stat_coding(true, p.states, false, p.precode, true);
	up_n = n;
	rate_rx = p.rate;
	rate_tx = (cp.drn + 20) * INT32_C(8000) / 6;
	DSP_DBG("v34: V.90 rates: down %" PRId32 ", up %" PRId32 "\n",
		rate_tx, rate_rx);
	return 0;
}

/* to state s, the transmitter on to segment seg */
void V34Pump::tx_next(enum v34_state s, enum v34_txseg seg)
{
	set_state(s);
	ptx.segment(seg);
}

/* the fastest rate allowed by both MP sequences at the symbol rate: up
   to max and in both rate masks; if the masks leave none up to max (a
   Motorola SM56 asks for 7200 bit/s from us with only 19200 in its
   mask), the fastest up to max that the symbol rate has, as a slow
   connection does better than none */
static s16 v34_pick_rate(s16 baud, s16 max, s16 mask)
{
	s16 n;
	for (n = max; n >= 1; n--)
		if ((mask >> (n - 1)) & 1 && v34_rates[baud][n - 1].b)
			return n;
	for (n = max; n >= 1; n--)
		if (v34_rates[baud][n - 1].b) {
			DSP_DBG("v34: no rate up to %d in the masks (%04x): "
				"%d\n", max, mask, n);
			return n;
		}
	return 0;
}

/* the call's statistics (ATI11) while connected: each direction's
   symbol rate and carrier (PCM's 8000 and none), the receiver's carrier
   and clock offsets (a V.34 receiver's, in data mode), the network's
   law and what the analogue modem's DIL showed of it */
void V34Pump::stat_link()
{
	if (!stats || !(connected || state == V91_RUN))
		return;
	bool pcm_both = state == V91_RUN || state == V92_RUN;
	bool tx_pcm = pcm_both || (v90a_on ? v92a_up_on != 0 : pcm != 0);
	bool rx_pcm = pcm_both || v90a_on;
	float sym_tx = 8000, car_tx = 0, sym_rx = 8000, car_rx = 0;

	if (!tx_pcm) {
		sym_tx = v34_symbol_rate(ptx.baud);
		car_tx = static_cast<float>(fabs(ptx.qam.carrier.inc)) *
			V34_SRATE;
	} else {
		stat_coding(false, 0, false, false, false);
	}
	if (!rx_pcm) {
		sym_rx = v34_symbol_rate(prx.baud);
		car_rx = static_cast<float>(fabs(prx.qam.lo.inc)) *
			V34_SRATE;
		if (prx.data_on) {
			stat_freq_offset(prx.qam.dtheta, prx.qam.baud);
			/* (the equalizer's clock once trained; the timing
			   loop's integrator before) */
			stat_timing_offset(prx.timing_fixed ? prx.t_freq :
					   prx.qam.t_adj, prx.qam.sps);
		}
	} else {
		stat_coding(true, 0, false, false, false);
		/* the analogue modem's PCM receiver: the network's clock
		   against ours */
		if (v90a_on && stats) {
			stats->timing_offset =
				static_cast<float>(-v90rx.ppm());
			stats->have |= DP_STATS_TIMING_OFFSET;
		}
	}
	stat_line(sym_tx, car_tx, sym_rx, car_rx);
	if (tx_pcm || rx_pcm) {
		stats->pcm_alaw = alaw;
		stats->have |= DP_STATS_PCM;
	}
	if (v90a_on && v90rx.dil_done) {
		stats->digital_loss = v90rx.dil_pad_db;
		stats->robbed_bits = v90rx.dil_robbed;
		stats->have |= DP_STATS_DIL;
	}
}

/* both MP sequences are known: set up the data mode */
s16 V34Pump::data_setup()
{
	const V34Mp *l = &ptx.mp, *r = &prx.mp;
	s16 mask = l->rate_mask & r->rate_mask, ntx, nrx;
	s16 ca = l->rate_ca < r->rate_ca ? l->rate_ca : r->rate_ca;
	s16 ac = l->rate_ac < r->rate_ac ? l->rate_ac : r->rate_ac;
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
	stat_coding(false, p.states, r->nonlinear, p.precode, r->expanded);
	if (p.init(prx.baud, nrx, l->expanded))
		return -1;
	p.states = 16 << l->trellis;
	p.set_precoder(l->h);
	prx.start_data(&p, CoreBits{ modem });
	rate_rx = p.rate;
	stat_coding(true, p.states, l->nonlinear, p.precode, l->expanded);
	DSP_DBG("v34: data rates tx %" PRId32 " rx %" PRId32
		", trellis tx %d rx %d states%s\n",
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
	zero(*mp);
	mp->type = 1;
	/* test: DSP_V34_RX_MAX=<n> asks for at most n * 2400 bit/s */
	if (s32 m = env_int("DSP_V34_RX_MAX"); m > 0 && max_rate_rx > m)
		max_rate_rx = m;
	/* the core's highest rates (+MS, V.80's <EM><rate>) */
	s16 tx_max = rate_cap(MDMPRM_MAX_RATE);
	if (s16 m = rate_cap(MDMPRM_MAX_RX_RATE); max_rate_rx > m)
		max_rate_rx = m;
	/* (no less than the symbol rate has: no common rate else) */
	if (max_rate_rx < rate_floor())
		max_rate_rx = rate_floor();
	if (tx_max < v34_min_rate(baud_tx))
		tx_max = v34_min_rate(baud_tx);
	mp->rate_ca = caller ? tx_max : max_rate_rx;
	mp->rate_ac = caller ? max_rate_rx : tx_max;
	/* test: DSP_V34_MP_EXPANDED=0 asks for no constellation expansion
	   (a V.92 modem's answer did, with precoding) */
	mp->expanded = env_int("DSP_V34_MP_EXPANDED", 1) ? 1 : 0;
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
			zero(mp->h);
	}
	tx_next(P4_MP, V34_TX_MP);
}

/* the core's highest rate param (MDMPRM_MAX_RATE, MDMPRM_MAX_RX_RATE)
   as V.34's N (rate / 2400), 14 (33600) when it gives none or more */
s16 V34Pump::rate_cap(unsigned param) const
{
	intptr_t r = modem_get_param(modem, param);
	if (r <= 0 || r >= 14 * 2400)
		return 14;
	return static_cast<s16>(r / 2400);
}

/* the DTE's retrain or rate renegotiation (MDMPRM_LINK_REQ: V.80's
   <EM><rtn> and <EM><rrn>, ATO1, ATO3), looked for once a block in data
   mode, as modem-on-hold's request is: a retrain (11.4/V.34, 9.5.2/V.90)
   from any data mode; a renegotiation, V.34's or V.90's (the most we
   receive from the data mode's error, within the core's highest rate),
   V.92's upstream one as its digital modem, or as the analogue modem
   V.90's (V.92's with PCM upstream) */
void V34Pump::link_req_poll()
{
	if (link_poll++ % 160 || !connected || retraining || renegotiating ||
	    (state != V34_DATA && state != V92_RUN && state != V90A_DATA))
		return;
	intptr_t r = modem_get_param(modem, MDMPRM_LINK_REQ);
	if (r != 1 && r != 2)
		return;
	modem_set_param(modem, MDMPRM_LINK_REQ, 0);
	if (r == 1) {
		retrain("the DTE's");
		return;
	}
	if (state == V92_RUN) {
		DSP_DBG("v34: V.92 renegotiation (the DTE's)\n");
		v92u.rn_req = 1;
	} else if (state == V90A_DATA) {
		v90rx.reneg_wait();
		v90a_reneg_ours("the DTE's");
	} else {
		s16 n = reneg_rate(0);
		if (s16 m = rate_cap(MDMPRM_MAX_RX_RATE); n > m)
			n = m;
		if (n < rate_floor())
			n = rate_floor();
		if (pcm)
			v90_renegotiate("the DTE's", n, 0);
		else
			renegotiate("the DTE's", n);
	}
}

/* a retrain: both modems go back to phase 2 at tones A and B
   (the INFO0 sequences stand), probe the line again and train the
   primary channel anew; whoever starts it with silence and its tone, the
   other modem answers the same way when it loses the data signal or
   hears the tone; by: whose it is (RT_OURS, RT_REMOTE, RT_HOLD) */
template <class P>
void V34Retrain<P>::retrain(const char *why, s16 by)
{
	P &self = static_cast<P &>(*this);
	DSP_DBG("v34: retrain %d (%s)\n", ++retrains, why);
	if (by != RT_HOLD)
		self.stat_retrain(by == RT_REMOTE);
	retraining = 1;
	renegotiating = 0;
	self.short2 = 0;
	self.v92a_short2 = 0;
	if (self.prx_on) {
		self.prx.release();
		self.prx_on = 0;
	}
	self.txm = TXM_SILENCE;
	/* V.90: V.34 signals again until the next INFO1a */
	if (self.pcm_tx) {
		self.pcm_tx = 0;
		self.ref_n = 0;
		self.v90tx.seg = V90_SILENCE;
		self.v90tx.rd_req = self.v90tx.sil_req = 0;
		self.v90_rn_init = self.v90_rd_wait = 0;
		self.up_n = 0;
		self.seq_len = 0;
	}
	self.ec.on = 0;
	/* V.90's analogue modem: the PCM receiver off (phase 2 is the
	   V.34 receiver's), the core's DC removal back */
	if (self.v90a_rx) {
		self.v90a_rx = 0;
		self.v90a_rn = 0;
		if (self.rt)
			self.rt->pcm = 0;
	}
	/* the control channel as after INFO0 */
	self.ccr.init(self.caller ? 2400 : 1200);
	self.ccr.snap = 1;
	self.cctx.pos = self.cctx.n;
	self.cc_rev = 0;
	self.cc_sym = 0;
	self.ev_info1 = self.ev_rev = 0;
	self.rt_info0 = 0;
	self.info0_sent = self.info0_rep = self.info0_rec = 0;
	self.info0_recovered = 0;
	self.rtd = 0;
	/* the probing */
	self.tone_g.init(self.caller ? 2400 : 1200, PROBE_PERIOD,
		      self.srate);
	self.probe_g[0].init(1050, PROBE_PERIOD, self.srate);
	self.probe_g[1].init(3000, PROBE_PERIOD, self.srate);
	self.tone_blocks = self.probe_blocks = 0;
	self.pblock_e = 0;
	self.l1_start = 0;
	zero(self.pacc);
	zero(self.pprev);
	self.ptotal = self.pdiff = 0;
	self.pcount = 0;
	rt_blocks = rt_heard = 0;
	rt_since = 0;
	self.lost = bad = 0;
	/* V.90's phase 3 (Jd, the DIL) takes about 8 s more */
	self.timeout = self.total +
		ms2samples(self.pcm || self.v90a_on ? 30000 : 15000,
			   self.srate);
	self.set_state(V34_RT_SILENCE);
}

/* the rate (N) to receive at after a renegotiation, from the data mode
   mse: up < 0 lower (by as many steps as the mse asks for, at least
   one), > 0 one higher, 0 whatever the mse says (when the remote
   started it).  At the symbol rate, a step of 2400 bit/s takes about
   3 dB per bit per symbol. */
template <class P>
s16 V34Retrain<P>::reneg_rate(s16 up)
{
	P &self = static_cast<P &>(*this);
	float step = 3 * 2400.0f / v34_symbol_rate(self.prx.baud);
	float mse = self.prx.mse;
	s16 n = self.rate_rx / 2400, k;

	if (up > 0 || (!up && mse * powf(10, step / 10) < V34_RN_UP))
		return n < rn_max ? n + 1 : n;
	if (up < 0 || mse > V34_RN_DOWN) {
		k = static_cast<s16>(
			ceilf(10 * log10f(mse / V34_RN_TARGET) / step));
		n -= k < 1 ? 1 : k;
	}
	return n < self.rate_floor() ? self.rate_floor() : n;
}

/* the least N we may receive at: the symbol rate's least (Table
   10/V.34), and 4800 bit/s for V.90's upstream (Table 17/V.90) */
s16 V34Pump::rate_floor() const
{
	s16 lo = v34_min_rate(prx_on ? prx.baud : baud_rx);
	return pcm_tx && lo < 2 ? 2 : lo;
}

/* a rate renegotiation (11.6/V.34), by us or in answer to the remote's
   S: S, S-bar, TRN, then MP with the new rate we can receive, MP', E
   and data at the new rates, all with the equalizers as they are; the
   link (V.42) carries on */
template <class P>
void V34Retrain<P>::renegotiate(const char *why, s16 n, bool remote)
{
	P &self = static_cast<P &>(*this);
	DSP_DBG("v34: renegotiation %d (%s): receive at most %" PRId32
		", mse %.3f\n", ++renegs, why, n * INT32_C(2400),
		self.prx.mse);
	self.stat_reneg(remote);
	renegotiating = 1;
	self.max_rate_rx = n;
	self.ptx.data_on = 0;
	self.ptx.ack_mp = 0;
	self.ptx.mp_sent = 0;
	self.ptx.trn16 = 0;
	rn_hi = rn_lo = bad = 0;
	/* no E 2500 ms plus the round trips after S-bar: a retrain */
	self.timeout = self.total + ms2samples(3000, self.srate) + 3 * self.rtd;
	self.tx_next(V34_RN_S, V34_TX_S);
}

/* a V.90 rate renegotiation (9.6/V.90), ours or in answer to the
   analogue modem's S: Rd, Rd-bar (on a data frame boundary, after its
   S-bar when it started), TRN2d, MP with the upstream rate we can
   receive now, MP', Ed, B1d; the analogue modem sends S, S-bar, SCR, CP,
   CP', E and B1.  Its CPs asks for a silent period first: Ed, silence
   until CP, Rt, Rt-bar, MP. */
void V34Pump::v90_renegotiate(const char *why, s16 n, s16 remote)
{
	DSP_DBG("v34: V.90 renegotiation %d (%s): upstream at most %" PRId32
		", mse %.3f\n", ++renegs, why, n * INT32_C(2400), prx.mse);
	stat_reneg(remote);
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
	if (pcmup_plain) {
		v92u.plain = 1;
		v92u.pf_ok = 0;
	}
	v92u.link.modem = modem;
	if (rt)
		rt->pcm = 1;
	set_state(V92_RUN);
}

/* V.91 after V.8 (or, leased, on a 4-wire leased circuit with no V.8):
   v91.cpp from here on, at 8000 Hz */
void V34Pump::v91_start(s16 leased)
{
	DSP_DBG("v34: V.91%s\n", leased ? " (leased)" : "");
	v91m.init(alaw, caller, env_int("DSP_V91_K"), env("DSP_V91_CC") != nullptr,
		  leased);
	v91m.link.modem = modem;
	v91m.stats = stats;
	if (rt)
		rt->pcm = 1;
	set_state(V91_RUN);
}

/* 8.3/V.91: V.34 from phase 2 on instead */
s16 V34Pump::v91_escape()
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

/* handshake()'s V.8 states (c: the samples in the state): the
   answerer's ANSam and JM, the caller's CM and CJ, then what they
   agreed on (another modulation, V.91, phase 2) */
template <class P>
s16 V34V8<P>::v8_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	switch (self.state) {
	/* V.8 answerer */
	case V8A_SILENCE:
		/* V.8's 0.2 s at least (8.2/V.8): the core's answer delay (its
		   S-register, 2 s) is V.25's silent interval of 1.8 to 2.5 s
		   (4.3/V.25), the answer tone then within it */
		if (c >= ms2samples(200, self.srate)) {
			self.set_state(V8A_ANSAM);
			self.txm = TXM_ANSAM;
		}
		break;
	case V8A_ANSAM:
		/* V.92's QC1a ahead of CM: short phase 1 (9.2.4.1/V.92) */
		if (self.ev_qc && !(self.qc_frame & 6) && self.v92 &&
		    self.qc_ok && !self.qc_done && !v8_ok) {
			s16 wxyz = ((self.qc_frame >> 4) & 1) << 3 |
				((self.qc_frame >> 6) & 1) << 2 |
				((self.qc_frame >> 7) & 1) << 1 |
				((self.qc_frame >> 8) & 1);
			self.qc_done = 1;
			if (v92_uqts(wxyz) < 0) {
				DSP_DBG("v34: QC1a: a cleardown\n");
				return DPSTAT_ERROR;
			}
			DSP_DBG("v34: V.92 quick connect, UQTS %d\n",
				v92_uqts(wxyz));
			self.v90tx.init(self.alaw, 0);
			self.v90tx.uqts = v92_uqts(wxyz);
			self.v90tx.anspcm_init(1);   /* -12 dBm0 */
			/* QCA1d (Table 13/V.92): bits 20:29 0 1 1 P 0 0 0 L M 1,
			   LM 01 for -12 dBm0 */
			v8tx.reset();
			for (s16 r = 0; r < 2; r++) {
				v8tx.put(0x3FF, 10);
				v8tx.put(0x2AA, 10);    /* 0101010101 */
				v8tx.put(0x6 | self.qc_lapm << 3 | 1 << 8 |
					 1 << 9, 10);
			}
			v8tx.put(0x3FF, 10);
			v8tx_repeat = 0;
			fsk.init(1650, 1850, V34_TONE_AMP, 300, self.srate, 1);
			self.txm = TXM_FSK;
			self.set_state(QCA_QCA1D);
			break;
		}
		if (v8_ok && v8_adaptive && v8_fax_call()) {
			/* a fax machine's CM (adaptive answer): the core
			   takes it on as a fax call */
			DSP_DBG("v34: V.8: a fax call's CM\n");
			return DPSTAT_FAX;
		}
		if (v8_ok) {
			/* back from hold with a CM of no modulations: a
			   cleardown (9.10.2.1/V.92) */
			if (self.moh_end && !v8_remote_v34()) {
				DSP_DBG("v34: on hold: CM without modulations\n");
				return DPSTAT_ERROR;
			}
			v8_dp = 0;
			if (!v8_remote_v34()) {
				/* JM (what both have), CJ, then the best of it
				   (our CM's offer is all of them) */
				v8_dp = v8_choice();
				if (!v8_dp)
					return self.fallback("no V.34 in CM");
				DSP_DBG("v34: V.8 agreed on %s\n",
					v8_dp_name(v8_dp));
				self.set_state(V8A_JM);
				v8_make_msg(&v8tx);
				fsk.init(1650, 1850, V34_TONE_AMP, 300,
					 self.srate, 1);
				self.txm = TXM_FSK;
				break;
			}
			self.pcm = v8_v90();
			self.v91_on = v8_v91();
			self.v90a_on = v8_v90a();
			if (self.v90a_on)
				DSP_DBG("v34: V.90 with us as the analogue "
					"modem\n");
			self.set_state(V8A_JM);
			/* the caller's INFO0c follows its CJ by only 75 ms:
			   listen for it already; with V.90 the analogue
			   modem's INFO0a */
			self.ccr.init(self.pcm ? 2400 : 1200);
			v8_make_msg(&v8tx);
			fsk.init(1650, 1850, V34_TONE_AMP, 300, self.srate, 1);
			self.txm = TXM_FSK;
		} else if (self.moh_end) {
			/* on hold: until T1, for phase 1 only */
			if (self.moh_end > 0 && self.total >= self.moh_end) {
				DSP_DBG("v34: on hold for T1: disconnect\n");
				return DPSTAT_ERROR;
			}
		} else if (aa_blocks >= 10)
			return self.fallback("V.32 caller");
		else if (c >= ms2samples(5000, self.srate)) {
			/* a caller silent through ANSam: a V.32, V.22,
			   V.21 or Bell 103 modem waiting for its own
			   answer tone, or a fax machine waiting for the
			   DIS; the automode chain tells them apart (its
			   last stage says DPSTAT_FAX) */
			return self.fallback("no CM");
		}
		break;
	case V8A_JM:
		/* V.cvm: the caller's CM gone quiet is its CJ too.  A codec
		   (AMR 7.4) can garble CJ's three zero octets, and the
		   caller's INFO0 follows only 175 ms after CJ */
		if (!v8_cj && v8_dp == DP_VCVM && v8_quiet >= 3) {
			DSP_DBG("v34: CM ended without a CJ: taken as CJ\n");
			v8_cj = 1;
		}
		if (v8_cj) {
			self.set_state(V8A_GAP);
			self.txm = TXM_SILENCE;
		}
		break;
	case V8A_GAP:
		if (c >= ms2samples(75, self.srate) && v8_dp)
			return v8_handover();
		if (c >= ms2samples(75, self.srate) && self.v91_on) {
			self.v91_start();
			break;
		}
		if (c >= ms2samples(75, self.srate) && self.pcm) {
			/* the digital modem of V.90 takes the part of the
			   V.34 call modem from phase 2 on */
			DSP_DBG("v34: V.90 with us as the digital modem\n");
			self.caller = 1;
			self.tone_g.init(2400, PROBE_PERIOD,
				      self.srate);
			/* a retrain: the analogue modem's tone A, ours B */
			self.rt_g.init(2400, PROBE_PERIOD, self.srate);
			self.own_g.init(1200, PROBE_PERIOD, self.srate);
			self.set_state(V8C_GAP);
			self.count = c = ms2samples(75, self.srate);
		}
		if (c >= ms2samples(75, self.srate) && !self.pcm) {
			self.set_state(P2A_INFO0);
			self.cc.init(self.srate, 2400, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			self.make_info0(&self.cctx);
			self.cc_send();
		}
		break;

	/* V.8 caller */
	case V8C_LISTEN:
		/* for a forced low speed caller: a tone that ends before
		   500 ms (ANSam lasts 2.6 s at least) is ANS, and the
		   answerer's modulation comes at once */
		if (v8_low && ans_gone >= ms2samples(20, self.srate))
			return v8_back("answer tone ended");
		if (c >= ms2samples(500, self.srate)) {
			float d = ans_am_depth();
			DSP_DBG("v34: answer tone AM depth %.2f\n", d);
			if (d < 0.1f)
				return self.fallback(
					"answer tone is not ANSam");
			self.set_state(V8C_WAIT);
		}
		break;
	case V8C_WAIT:
		/* V.92's QC1a first (Table 2/V.92): bits 20:29 0 0 0 P W 0
		   X Y Z 1, P 0 (no LAPM without detection: ours always
		   does V.42's), WXYZ 1010 for UQTS 79; twice */
		if (self.total >= ms2samples(1000, self.srate) &&
		    self.v92a && self.qca_ok && !self.moh_backed &&
		    !self.retraining) {
			v8tx.reset();
			for (s16 r = 0; r < 2; r++) {
				v8tx.put(0x3FF, 10);
				v8tx.put(0x2AA, 10);
				v8tx.put(1 << 4 | 0 << 6 | 1 << 7 | 0 << 8 |
					 1 << 9, 10);
			}
			v8tx_repeat = 0;
			self.qcc_sent = 1;
			fsk.init(980, 1180, V34_TONE_AMP, 300, self.srate, 1);
			self.txm = TXM_FSK;
			self.set_state(QCC_QC1A);
			DSP_DBG("v34: V.92 QC1a\n");
			break;
		}
		if (self.total >= ms2samples(1000, self.srate)) {
			self.set_state(V8C_CM);
			v8_make_msg(&v8tx);
			fsk.init(980, 1180, V34_TONE_AMP, 300, self.srate, 1);
			self.txm = TXM_FSK;
		}
		break;
	case V8C_CM:
		/* V.92's QCA1d: CM ends at once (9.2.1.1/V.92) */
		if (self.qcc_sent && self.ev_qc &&
		    (self.qc_frame & 6) == 6) {
			DSP_DBG("v34: V.92 QCA1d: quick connect\n");
			self.txm = TXM_SILENCE;
			self.ans_peak = 0;
			self.ans_gone = 0;
			self.qcc_ans = 0;
			self.set_state(QCC_WAIT);
			break;
		}
		/* finish the octet, then CJ */
		if (v8_ok && fsk.edge() && v8tx.pos % 10 == 0) {
			v8_dp = 0;
			if (!v8_remote_v34()) {
				/* CJ, then what the JM has (9.2/V.8) */
				v8_dp = v8_choice();
				if (!v8_dp)
					return self.fallback("no V.34 in JM");
				DSP_DBG("v34: V.8 agreed on %s\n",
					v8_dp_name(v8_dp));
				self.set_state(V8C_CJ);
				v8tx.make_cj();
				v8tx_repeat = 0;
				break;
			}
			self.pcm = v8_v90();
			self.v91_on = v8_v91();
			self.v90a_on = v8_v90a();
			if (self.pcm)
				DSP_DBG("v34: V.90 with us as the digital modem\n");
			if (self.v90a_on)
				DSP_DBG("v34: V.90 with us as the analogue "
					"modem\n");
			self.set_state(V8C_CJ);
			v8tx.make_cj();
			v8tx_repeat = 0;
			/* (the analogue modem is the V.34 answerer: the
			   digital modem's INFO0d comes at 1200 Hz) */
			self.ccr.init(self.v90a_on ? 1200 : 2400);
		} else if (c >= ms2samples(5000, self.srate))
			return self.fallback("no JM");
		break;
	case V8C_CJ:
		if (v8tx.pos >= v8tx.n && fsk.edge()) {
			self.set_state(V8C_GAP);
			self.txm = TXM_SILENCE;
		}
		break;
	case V8C_GAP:
		/* 75 ms without signal after CJ, then sigC (8.1.2/V.8): the
		   PCTel modem takes CJ only with the gap after it */
		if (c >= ms2samples(75, self.srate) && v8_dp)
			return v8_handover();
		if (c >= ms2samples(75, self.srate) && self.v91_on) {
			self.v91_start();
			break;
		}
		if (c >= ms2samples(75, self.srate) && self.v90a_on) {
			/* the analogue modem of V.90 takes the part of the
			   V.34 answer modem from phase 2 on (9.2.2/V.90) */
			self.caller = 0;
			self.tone_g.init(1200, PROBE_PERIOD, self.srate);
			/* a retrain: the digital modem's tone B, ours A */
			self.rt_g.init(1200, PROBE_PERIOD, self.srate);
			self.own_g.init(2400, PROBE_PERIOD, self.srate);
			self.set_state(P2A_INFO0);
			self.cc.init(self.srate, 2400, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			self.make_info0(&self.cctx);
			self.cc_send();
			break;
		}
		if (c >= ms2samples(75, self.srate)) {
			self.set_state(P2C_INFO0);
			self.cc.init(self.srate, 1200, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			self.make_info0(&self.cctx);
			self.cc_send();
		}
		break;
	default:
		break;
	}
	return DPSTAT_OK;
}

/* handshake()'s states of V.92's short phase 1, the digital modem
   answering (9.2.4/V.92): QCA1d, then QTS and ANSpcm until TONEq, then
   phase 2 (c: the samples in the state) */
template <class P>
s16 V34Qc<P>::qc_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	switch (self.state) {
	/* V.92 short phase 1, answering */
	/* V.92 short phase 1, calling: QC1a, then CM (V8C_CM, where a
	   QCA1d brings us to QCC_WAIT) */
	case QCC_QC1A:
		if (self.v8tx.pos >= self.v8tx.n && self.fsk.edge()) {
			self.v8_make_msg(&self.v8tx);
			self.set_state(V8C_CM);
		}
		break;
	case QCC_WAIT:
		/* the digital modem's QTS, QTS-bar (not looked for: our
		   PCM receiver finds Sd later) and ANSpcm (2100 Hz):
		   TONEq once it has been there 50 ms (we heard ANSam for a
		   second already, 9.2.1.3/V.92) */
		self.qcc_ans = self.ans_peak > 300 && self.ans_gone == 0 ?
			self.qcc_ans + 1 : 0;
		if (self.qcc_ans >= ms2samples(50, self.srate)) {
			DSP_DBG("v34: V.92 ANSpcm: TONEq\n");
			self.v8tx.reset();
			self.v8tx_repeat = 0;
			self.fsk.init(980, 1180, V34_TONE_AMP, 300,
				      self.srate, 1);
			self.txm = TXM_FSK;
			self.set_state(QCC_TONEQ);
			break;
		}
		if (c >= ms2samples(2000, self.srate)) {
			/* no ANSpcm: CM again, V.8 */
			DSP_DBG("v34: V.92 no ANSpcm: V.8\n");
			self.qcc_sent = 0;
			self.v8_make_msg(&self.v8tx);
			self.fsk.init(980, 1180, V34_TONE_AMP, 300,
				      self.srate, 1);
			self.txm = TXM_FSK;
			self.set_state(V8C_CM);
		}
		break;
	case QCC_TONEQ:
		/* until ANSpcm ends (50 ms at least), then 75 ms of
		   silence */
		if (c >= ms2samples(50, self.srate) &&
		    self.ans_gone >= ms2samples(20, self.srate)) {
			DSP_DBG("v34: V.92 ANSpcm ended\n");
			self.txm = TXM_SILENCE;
			self.ccr.init(1200);
			self.set_state(QCC_GAP);
			break;
		}
		if (c >= ms2samples(3000, self.srate))
			return self.fallback("ANSpcm did not end");
		break;
	case QCC_GAP:
		if (c >= ms2samples(75, self.srate)) {
			/* phase 2 as after V.8 for V.90, we the analogue
			   modem, as the V.34 answerer (9.2.2/V.90) */
			DSP_DBG("v34: V.92 quick connect: V.90 with us as the "
				"analogue modem\n");
			self.pcm = 0;
			self.v91_on = 0;
			self.v90a_on = 1;
			self.caller = 0;
			self.tone_g.init(1200, PROBE_PERIOD, self.srate);
			self.rt_g.init(1200, PROBE_PERIOD, self.srate);
			self.own_g.init(2400, PROBE_PERIOD, self.srate);
			self.set_state(P2A_INFO0);
			self.cc.init(self.srate, 2400, CC_BAUD, 0.5f, 4, 1,
				     V34_TONE_AMP);
			self.make_info0(&self.cctx);
			self.cc_send();
		}
		break;
	case QCA_QCA1D:
		if (self.v8tx.pos >= self.v8tx.n) {
			self.txm = TXM_SILENCE;
			self.set_state(QCA_GAP);
		}
		break;
	case QCA_GAP:
		if (c >= ms2samples(75, self.srate)) {
			/* QTS from the next 8000 Hz symbol on */
			self.out_n++;
			self.v90tx.fclock = &self.out_n;
			self.v90tx.segment(V90_QTS);
			self.out_n--;
			self.pcm_tx = 1;
			toneq_g.init(980, PROBE_PERIOD, self.srate);
			toneq_space_g.init(1180, PROBE_PERIOD, self.srate);
			toneq_blocks = 0;
			self.set_state(QCA_ANSPCM);
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
		if (self.v90tx.seg != V90_ANSPCM)
			toneq_blocks = 0;
		if (self.v8_ok) {
			/* CM: it missed our QCA1d and went on with V.8
			   (9.2.1.1/V.92); answer that with JM */
			DSP_DBG("v34: CM after QCA1d: V.8\n");
			self.pcm_tx = 0;
			self.v90tx.fclock = nullptr;
			self.txm = TXM_ANSAM;
			self.set_state(V8A_ANSAM);
		} else if (toneq_blocks >= 16) {
			DSP_DBG("v34: TONEq heard\n");
			self.v90tx.segment(V90_SILENCE);
			/* the analogue modem's INFO0a follows its 75 ms of
			   silence */
			self.ccr.init(2400);
			self.set_state(QCA_TONEQ);
		} else if (c >= ms2samples(2000, self.srate)) {
			DSP_DBG("v34: no TONEq: V.8\n");
			self.pcm_tx = 0;
			self.v90tx.fclock = nullptr;
			self.v8_ok = self.v8_cj = 0;
			self.v8_sync = 0;
			self.txm = TXM_ANSAM;
			self.set_state(V8A_ANSAM);
		}
		break;
	case QCA_TONEQ:
		/* the analogue modem ends TONEq when ANSpcm ends, then 75 ms
		   of silence: ours from then on, so that it hears all of
		   INFO0d */
		if (!toneq_blocks || c >= ms2samples(500, self.srate)) {
			DSP_DBG("v34: TONEq ended\n");
			self.set_state(QCA_GAP2);
		}
		break;
	case QCA_GAP2:
		if (c >= ms2samples(75, self.srate)) {
			/* phase 2 as after V.8 for V.90 */
			self.pcm_tx = 0;
			self.pcm = 1;
			DSP_DBG("v34: V.90 with us as the digital modem\n");
			self.caller = 1;
			self.tone_g.init(2400, PROBE_PERIOD, self.srate);
			self.rt_g.init(2400, PROBE_PERIOD, self.srate);
			self.own_g.init(1200, PROBE_PERIOD, self.srate);
			self.set_state(V8C_GAP);
			self.count = c = ms2samples(75, self.srate);
		}
		break;
	default:
		break;
	}
	return DPSTAT_OK;
}

/* handshake()'s modem-on-hold states (9.10/V.92; c: the samples in the
   state): our MHreq and its answer, on hold, our answer to the
   remote's */
template <class P>
s16 V34Moh<P>::moh_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	switch (self.state) {
	case MOH_REQ:
		/* tone B for 600 ms (50 at least, 9.10.1/V.92: longer, so
		   that a remote that answers it with a retrain's reversal
		   has done so before the sequence, whose phase steps it
		   would take for our reversal), then the sequence,
		   repeated, until the answer (9.10.1.1/V.92) */
		if (c >= ms2samples(600, self.srate) && !self.cc_busy()) {
			self.cctx.make_mh(moh_req, moh_req);
			self.cc_send();
		}
		if (moh_req == MH_REQ) {
			/* the remote took our tone for a retrain's (it may,
			   9.10.1.1/V.92): its tone A reversed, or its INFO0a.
			   That retrain then, at once (it waits for our
			   reversal): phase 2 from tone B */
			if (!self.rt_heard)
				/* (one before its tone is the data's end) */
				self.ev_rev = 0;
			if (self.ev_rev || self.rt_info0 == 1) {
				DSP_DBG("v34: the remote retrains instead of "
					"answering MHreq\n");
				moh_req = 0;
				self.set_state(self.caller ? P2C_B : P2A_A);
				self.cc_tone();
				break;
			}
			/* none in 2 s and a round trip: retrain */
			if (c >= ms2samples(600 + 2500, self.srate)) {
				DSP_DBG("v34: no answer to MHreq\n");
				self.retrain("no answer to MHreq");
			}
		} else if (moh_req == MH_CDA) {
			/* (what the remote asked for: once it is out) */
			if (c >= ms2samples(500, self.srate)) {
				DSP_DBG("v34: cleared down\n");
				return DPSTAT_ERROR;
			}
		} else if (c >= ms2samples(300, self.srate)) {
			/* MHfrr: then ANSam (9.10.2.3/V.92), phase 1 as
			   the answering modem */
			moh_resp = MH_NACK;
			moh_hold(10);
		}
		break;
	case MOH_HELD:
		/* silent until we take the remote back, within its T1: it
		   sends ANSam meanwhile, and we call it again (CM, phase 1
		   as the call modem, 9.10.2.1/V.92) */
		if (c >= ms2samples(1000L * moh_req_hold, self.srate) ||
		    (moh_t1_secs(moh_req_t1) && moh_req_t1 != 13 &&
		     c >= ms2samples(1000L * moh_t1_secs(moh_req_t1) - 2000,
				     self.srate)))
			moh_back();
		break;
	case MOH_BACK:
		/* its ANSam there 200 ms: V.8 as the call modem (the tone's
		   modulation measured anew); none in 5 s: it is gone (it
		   sends ANSam all through its T1) */
		moh_ans = self.ans_peak > 300 && self.ans_gone == 0 ?
			moh_ans + 1 : 0;
		if (moh_ans >= ms2samples(200, self.srate)) {
			DSP_DBG("v34: back from hold: ANSam, V.8 as the call "
				"modem\n");
			self.ans_peak = 0;
			self.env_sum = self.env15_re = self.env15_im = 0;
			self.env_n = 0;
			self.set_state(V8C_LISTEN);
		} else if (c >= ms2samples(5000, self.srate)) {
			DSP_DBG("v34: back from hold: no ANSam\n");
			return DPSTAT_ERROR;
		}
		break;
	case MOH_RESP:
		/* repeated, each right after the one before (9.10.1/V.92) */
		if (!self.cc_busy()) {
			self.cctx.make_mh(moh_resp,
				     moh_resp == MH_ACK ? moh_t1 : moh_resp);
			self.cc_send();
		}
		if (moh_drop)
			return DPSTAT_ERROR;
		switch (moh_resp) {
		case MH_ACK:
			/* then its tone for 100 ms or silence for 2 s: on hold
			   (9.10.2.1/V.92) */
			if (self.rt_blocks >= 15 || self.quiet_blocks >= 300)
				moh_hold(moh_t1 == 13 ? 0 :
					 moh_t1_secs(moh_t1));
			else if (self.total - mh_seen >=
				 ms2samples(35000, self.srate))
				return DPSTAT_ERROR;
			break;
		case MH_CDA:
			/* until its tone, silence, or no MHclrd for 200 ms
			   (9.10.2.2/V.92) */
			if (self.rt_blocks >= 15 || self.quiet_blocks >= 30 ||
			    self.total - mh_seen >=
			    ms2samples(200, self.srate)) {
				DSP_DBG("v34: cleared down\n");
				return DPSTAT_ERROR;
			}
			break;
		case MH_NACK:
			/* MHcda or MHfrr within 10 s */
			if (c >= ms2samples(12000, self.srate)) {
				DSP_DBG("v34: nothing after MHnack\n");
				return DPSTAT_ERROR;
			}
			break;
		}
		break;
	default:
		break;
	}
	return DPSTAT_OK;
}

/* handshake()'s states of a rate renegotiation (S, S-bar, TRN, then
   MP) and of a retrain's silence (c: the samples in the state) */
template <class P>
s16 V34Retrain<P>::rt_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	switch (self.state) {
	case V34_RN_S:
		if (self.ptx.n >= 128)
			self.tx_next(V34_RN_SBAR, V34_TX_SBAR);
		break;
	case V34_RN_SBAR:
		if (self.ptx.n >= 16)
			self.tx_next(V34_RN_TRN, V34_TX_TRN);
		break;
	case V34_RN_TRN:
		/* MP once the remote's training has been there a while
		   (both started theirs), at most 2 s */
		if ((self.ptx.n >= 512 && self.prx.stage == V34_RX_TRN &&
		     self.prx.n >= 64) ||
		    c >= ms2samples(2000, self.srate))
			self.mp_start();
		break;
	case V34_RT_SILENCE:
		if (c >= ms2samples(70, self.srate)) {
			self.set_state(self.caller ? P2C_B : P2A_A);
			self.cc_tone();
		}
		break;
	default:
		break;
	}
	return DPSTAT_OK;
}

/* the start-up and data mode, once a sample before it goes out: V.8,
   phases 2 to 4 of V.34 / V.90, retrains, renegotiations and
   modem-on-hold; returns DPSTAT_OK, DPSTAT_CONNECT, DPSTAT_CHANGEDP or
   DPSTAT_ERROR */
s16 V34Pump::handshake()
{
	moh_req_test();
	link_req_poll();
	s32 c = count;

	/* V.90: the analogue modem asks for a retrain in phase 3 or 4 with
	   silence and its tone A (it is the V.34 answerer in phase 2) */
	if (v90_digital() && rt_blocks >= 6)
		retrain("tone A", RT_REMOTE);
	/* the analogue modem: the digital modem's tone B, in phase 3, 4,
	   data mode or a renegotiation (9.5.2.2/V.90) */
	if (v90a_rx && rt_blocks >= 6)
		retrain("tone B", RT_REMOTE);
	/* the remote retrains instead (11.6.2/V.34) */
	if (renegotiating && rt_blocks >= 6)
		retrain(caller ? "tone A" : "tone B", RT_REMOTE);
	if (ev_mh) {
		ev_mh = 0;
		mh_event();
		/* (it may have started a state) */
		c = count;
	}

	switch (state) {
	/* V.8, both sides */
	case V8A_SILENCE:
	case V8A_ANSAM:
	case V8A_JM:
	case V8A_GAP:
	case V8C_LISTEN:
	case V8C_WAIT:
	case V8C_CM:
	case V8C_CJ:
	case V8C_GAP:
		return v8_step(c);
	/* V.92 short phase 1, calling */
	case QCC_QC1A:
	case QCC_WAIT:
	case QCC_TONEQ:
	case QCC_GAP:
		return qc_step(c);
	/* V.92 short phase 1, answering */
	case QCA_QCA1D:
	case QCA_GAP:
	case QCA_ANSPCM:
	case QCA_TONEQ:
	case QCA_GAP2:
		return qc_step(c);
	case V91_RUN:           /* process() runs it */
	case V92_RUN:
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
				      c < ms2samples(200, srate) ||
				      /* V.92's analogue modem: the remote's
				         tone B may be a modem-on-hold
				         request's, its MHreq some 600 ms
				         on (9.10.1.1/V.92) */
				      (v92a_on && c < ms2samples(1000, srate)))) {
			if (c >= ms2samples(3000, srate)) {
				DSP_DBG("v34: retrain: no tone B\n");
				return DPSTAT_ERROR;
			}
			break;
		}
		/* phase 2's recovery: our INFO0 again */
		if (info0_resend(c))
			break;
		/* V.92's short phase 2: the digital modem reverses B
		   first, ours 40 ms after it (9.4.2.1.3/V.92) */
		if (v92a_short2) {
			if (ev_info0 && ev_rev && rev_time > info0_at) {
				set_state(P2A_S2_40);
				count = total - rev_time;
				break;
			}
			if (c >= ms2samples(2500, srate)) {
				v92a_short2 = 0;
				retrain("short phase 2: no tone B reversal");
			}
			break;
		}
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
				static_cast<long>(rtd) * 1000 / srate);
			stat_rtd(rtd);
			set_state(P2A_ABAR_40);
			count = total - rev_time;
		}
		break;
	case P2A_S2_40:
		if (c >= REV_WAIT) {
			set_state(P2A_S2_10);
			cc_rev = 1;
		}
		break;
	case P2A_S2_10:
		/* then INFO1a: PCM upstream's (no INFO1d's bit 70 to go
		   by: the digital modem offering short phase 2 for it) */
		if (rev_on_line() >= ms2samples(10, srate)) {
			v90a_pick_uinfo();
			v92a_up_on = 1;
			set_state(P2A_INFO1);
			make_info1a_v92(&cctx);
			cc_send();
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
			/* the analogue modem of V.90 asks for it in INFO1a,
			   or for V.34 (and after V.90 failed: v90a_fail()) */
			if (v90a_on && v90a_v34) {
				DSP_DBG("v34: V.34 this time (V.90 failed)\n");
				v90a_on = 0;
			}
			if (v90a_on)
				v90a_choose();
			if (!v90a_on)
				choose_bauds();
			set_state(P2A_INFO1);
			/* V.92's PCM upstream where INFO1d offers it (bit
			   70, Table 17/V.92) */
			v92a_up_on = v90a_on && v92a_on && v92a_up &&
				!v92a_noup && rinfo1c.r[V34_BAUD_3429].high;
			if (v92a_up_on)
				make_info1a_v92(&cctx);
			else if (v90a_on)
				make_info1a_v90(&cctx);
			else
				make_info1a(&cctx);
			cc_send();
		}
		break;
	case P2A_INFO1:
		if (!cc_busy()) {
			cc_silence();
			if (v92a_up_on) {
				set_state(P3U_SILENCE);
				v92a_up_start();
				break;
			}
			if (v90a_on) {
				set_state(P3V_SILENCE);
				v90a_start();
				break;
			}
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
		/* nor one before the tone, or as it began: the remote's
		   data going over to tone A can look like one (taken for
		   its reversal we would stop tone B, which it is still
		   waiting for) */
		if (retraining && ev_rev &&
		    rev_time - rt_since < ms2samples(20, srate)) {
			DSP_DBG("v34: retrain: a reversal before tone A, "
				"not its\n");
			ev_rev = 0;
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
				static_cast<long>(rtd) * 1000 / srate);
			stat_rtd(rtd);
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
			retrain("the remote retrains in phase 2", RT_REMOTE);
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

	/* V.90 analogue modem, phase 3 (9.3.2/V.90): S, S-bar, PP, TRN,
	   Ja until the digital modem's Sd-bar, silence while TRN1d trains
	   the PCM receiver, S once Jd is in, S-bar after Jd'.  The
	   failures that would retrain (9.5.2.1/V.90) end the call so far */
	case P3V_SILENCE:
		if (c >= ms2samples(70, srate))
			tx_next(P3V_S, V34_TX_S);
		break;
	case P3V_S:
		if (ptx.n >= 128)
			tx_next(P3V_SBAR, V34_TX_SBAR);
		break;
	case P3V_SBAR:
		if (ptx.n >= 16)
			tx_next(P3V_PP, V34_TX_PP);
		break;
	case P3V_PP:
		if (ptx.n >= 288)
			tx_next(P3V_TRN, V34_TX_TRN);
		break;
	case P3V_TRN:
		/* 512 T at least: the digital modem trains on PP and
		   them, then takes Ja */
		if (ptx.n >= 2048)
			tx_next(P3V_JA, V34_TX_JA);
		break;
	case P3V_JA:
		if (v90rx.ev_sdbar) {
			tx_next(P3V_WAIT_JD, V34_TX_SILENCE);
			break;
		}
		if (c >= ms2samples(3000, srate)) {
			DSP_DBG("v34: V.90: no Sd-bar after Ja\n");
			return v90a_fail(0);
		}
		break;
	case P3V_WAIT_JD:
		/* S once Jd is in and the PCM receiver is ready (we may
		   wait 5 s, 9.3.2.7/V.90) */
		if (v90rx.ev_jd && (v90rx.ev_ready ||
				    c >= ms2samples(4000, srate))) {
			DSP_DBG("v34: V.90: Jd, PCM receiver mse %.4f\n",
				v90rx.mse);
			tx_next(P3V_S2, V34_TX_S);
			break;
		}
		if (c >= ms2samples(4500, srate)) {
			DSP_DBG("v34: V.90: no Jd (%s)\n",
				v90rx.ev_trained ? "trained" : "not trained");
			return v90a_fail(0);
		}
		break;
	case P3V_S2:
		if (v90rx.ev_jdp) {
			tx_next(P3V_SBAR2, V34_TX_SBAR);
			break;
		}
		if (c >= ms2samples(5000, srate)) {
			DSP_DBG("v34: V.90: no Jd'\n");
			return v90a_fail(0);
		}
		break;
	case P3V_SBAR2:
		if (ptx.n < 16)
			break;
		if (!v90a_dil.n) {
			DSP_DBG("v34: V.90: phase 3 done (no DIL)\n");
			if (v90a_phase4())
				return v90a_fail(1);
		} else
			tx_next(P3V_DIL, V34_TX_SILENCE);
		break;
	case P3V_DIL:
		/* the DIL's rounds measured (within 5 s of S-bar,
		   9.3.2.10/V.90): S, S-bar end it */
		if (v90rx.ev_dil >= V90RX_DIL_ROUNDS) {
			v90rx.dil_analyse();
			tx_next(P3V_S3, V34_TX_S);
			break;
		}
		if (c >= ms2samples(4500, srate)) {
			DSP_DBG("v34: V.90: the DIL did not come\n");
			return v90a_fail(0);
		}
		break;
	case P3V_S3:
		if (ptx.n >= 128)
			tx_next(P3V_SBAR3, V34_TX_SBAR);
		break;
	case P3V_SBAR3:
		if (ptx.n >= 16) {
			DSP_DBG("v34: V.90: phase 3 done\n");
			if (v90a_phase4())
				return v90a_fail(1);
		}
		break;

	/* V.92 analogue modem, PCM upstream phase 3 (9.5.2/V.92): the
	   transmitter goes from Ru to Ja by itself; Ja until the digital
	   modem's Sd-bar, silence while TRN1d trains the PCM receiver, Su
	   once Jd is in, Su-bar of 24 T and Jp's fraction once Jp is in,
	   TRN1u while the DIL is measured, CPt until Ri, E1u */
	case P3U_SILENCE:
		if (c >= ms2samples(70, srate)) {
			patx.segment(V92A_RU);
			set_state(P3U_JA);
		}
		break;
	case P3U_JA:
		if (v90rx.ev_sdbar) {
			patx.ja_stop = 1;
			set_state(P3U_WAIT_JD);
			break;
		}
		/* Ru and TRN1u, then 1500 ms of Ja (9.5.2.2.1/V.92) */
		if (c >= ms2samples(2000, srate))
			return v92a_up_fail("no Sd-bar after Ja");
		break;
	case P3U_WAIT_JD:
		/* Su once Jd is in and the PCM receiver is ready (we may
		   wait 5 s, 9.5.2.1.6/V.92) */
		if (v90rx.ev_jd && (v90rx.ev_ready ||
				    c >= ms2samples(4000, srate))) {
			DSP_DBG("v34: V.92: Jd, PCM receiver mse %.4f\n",
				v90rx.mse);
			patx.segment(V92A_SU);
			set_state(P3U_SU);
			break;
		}
		if (c >= ms2samples(4500, srate))
			return v92a_up_fail(v90rx.ev_trained ?
					    "no Jd (trained)" :
					    "no Jd (not trained)");
		break;
	case P3U_SU:
		if (v90rx.ev_jp) {
			patx.jp_frac = v90rx.jp_frac;
			patx.jp_req = 1;
			set_state(P3U_TRN1);
			break;
		}
		if (c >= ms2samples(5000, srate))
			return v92a_up_fail("no Jp");
		break;
	case P3U_TRN1:
		/* the DIL's rounds measured (within 5 s of Su-bar,
		   9.5.2.1.11/V.92): CPt after 2040 T of TRN1u at least */
		if (v90rx.ev_dil >= V90RX_DIL_ROUNDS ||
		    (!v90a_dil.n && v90rx.ev_jdp)) {
			if (v90a_dil.n)
				v90rx.dil_analyse();
			if (v92a_cpt())
				return v92a_up_fail("no CPt constellations");
			patx.trn_stop = 1;
			set_state(P3U_CPT);
			break;
		}
		if (c >= ms2samples(5000, srate))
			return v92a_up_fail(v90rx.ev_jdp ?
					    "the DIL did not come" :
					    "no Jp'");
		break;
	case P3U_CPT:
		if (v90rx.ev_ri) {
			patx.ri_seen = 1;
			set_state(P3U_E1U);
			break;
		}
		if (c >= ms2samples(5000, srate))
			return v92a_up_fail("no Ri");
		break;
	case P3U_E1U:
		/* TRN2u after it */
		if (patx.next) {
			DSP_DBG("v34: V.92: PCM upstream phase 3 done\n");
			v92a_cpu = v92a_acked = 0;
			set_state(P4U_TRN2);
			break;
		}
		if (c >= ms2samples(1000, srate))
			return v92a_up_fail("E1u did not go");
		break;

	/* V.92 analogue modem, PCM upstream phase 4 (9.6.2/V.92): TRN2u
	   until SUVd, or 12000 T of it, once TRN2d has shown the noise
	   (for CPu's design), then SUVu; one CPu after SUVd (again if no
	   acknowledgement comes 100 ms and a round trip after it), all
	   acknowledged once CPd is in; E2u after the current sequence
	   once one of ours acknowledged has gone and SUVd' or CPd' or Ed
	   is in; B1u and data on CPd, B1d on our CPu's constellations */
	case P4U_TRN2:
		if (v90rx.ev_trn2 && patx.seg == V92A_TRN2 &&
		    (v90rx.ev_suvd || patx.n >= 12000)) {
			patx.nbits = v92_suvu_build(patx.bits, 0, 24);
			patx.nxt_n = 0;
			patx.trn2_stop = 1;
			set_state(P4U_SEQ);
			break;
		}
		if (c >= ms2samples(4000, srate))
			return v92a_up_fail(v90rx.ev_trn2 ? "no SUVd" :
					    "no TRN2d");
		break;
	case P4U_SEQ:
		if (patx.seg == V92A_SEQ && !patx.nxt_n && v92a_next_seq())
			return v92a_up_fail("no CP constellations");
		if (v92a_acked && patx.seqs >= v92a_ack_seqs &&
		    (v90rx.ev_mpack || v90rx.ev_ed)) {
			const V92Cpd &d = v90rx.cpd;
			if (!v92a_cpd_ok(d))
				return v92a_up_fail("CPd we can't take");
			patx.cpd = d;
			patx.link = CoreBits{ modem };
			patx.e2u_req = 1;
			rate_tx = (d.drn + 17) * INT32_C(8000) / 6;
			rate_rx = (v90a_cp.drn + 20) * INT32_C(8000) / 6;
			DSP_DBG("v34: V.92: E2u, B1u at %" PRId32 " bit/s\n",
				rate_tx);
			set_state(P4U_DATA);
			break;
		}
		if (c >= ms2samples(5000, srate))
			return v92a_up_fail(!v90rx.ev_suvd ? "no SUVd" :
					    !v90rx.ev_cpd ? "no CPd" :
					    "no SUVd' or CPd'");
		break;
	/* a fast parameter exchange (9.9.2/V.92): the digital modem's,
	   after its Rf-bar our RM, RM', then the sequences as in phase 4
	   in the data mode modulation (CPus: the downstream as it is),
	   E2u, FB1u's 48 frames and B1u on its new CPd, its B1d on our
	   constellations as they were; ours, RM, RM' and SUVu until its
	   Rf, Rf-bar and SUVd, then the same with a CPu, its B1d on that.
	   Its Rd before then: its renegotiation instead (9.9.2.1.2) */
	case P4U_FSEQ:
		if (v90rx.ev_rdbar) {
			v92a_reneg(1, "");
			break;
		}
		if (patx.dsrc == V92A_D_SEQ && !patx.nxt_n &&
		    v92a_next_seq(v92a_fpe_who))
			return v92a_up_fail(v92a_fpe_who == 1 ?
					    "FPE: no CPus" : "FPE: no CPu");
		if (v92a_acked && patx.seqs >= v92a_ack_seqs &&
		    (v90rx.ev_mpack || v90rx.ev_ed)) {
			if (!v92a_cpd_ok(v90rx.cpd))
				return v92a_up_fail("FPE: CPd we can't take");
			patx.cpd_new = v90rx.cpd;
			patx.e2u_req = 1;
			rate_tx = (v90rx.cpd.drn + 17) * INT32_C(8000) / 6;
			rate_rx = (v90a_cp.drn + 20) * INT32_C(8000) / 6;
			DSP_DBG("v34: V.92: FPE: E2u, FB1u, B1u at %" PRId32
				" bit/s\n",
				rate_tx);
			set_state(P4U_FDATA);
			break;
		}
		if (c >= ms2samples(5000, srate))
			return v92a_up_fail(!v90rx.ev_suvd ? "FPE: no SUVd" :
					    !v90rx.ev_cpd ? "FPE: no CPd" :
					    "FPE: no SUVd' or CPd'");
		break;
	case P4U_FDATA:
		if (v90rx.ev_b1d && patx.dsrc == V92A_D_LINK &&
		    !patx.b1u_next && patx.frames > 48) {
			set_state(V90A_DATA);
			set_rates(rate_tx, rate_rx);
			v90a_quiet = 0;
			v90a_rn = 0;
			DSP_DBG("v34: V.92 fast parameter exchange done "
				RATE_FMT " (analogue modem)\n", rate_tx,
				rate_rx);
			break;
		}
		if (c >= ms2samples(4000, srate))
			return v92a_up_fail(v90rx.ev_ed ? "FPE: no B1d" :
					    "FPE: no Ed");
		break;
	case P4U_RN:
		/* our Ru and Ru-bar out: TRN2u and phase 4's sequences
		   again */
		if (patx.seg == V92A_TRN2) {
			patx.trn2_stop = patx.e2u_req = 0;
			patx.nxt_n = 0;
			v92a_cpu = v92a_acked = 0;
			set_state(P4U_TRN2);
			break;
		}
		if (c >= ms2samples(1000, srate))
			return v92a_up_fail("Ru did not go");
		break;
	case P4U_DATA:
		/* B1d in and our B1u out: CONNECT (or the data on after a
		   renegotiation) */
		if (v90rx.ev_b1d && patx.seg == V92A_DATA &&
		    patx.frames > 48 && v90a_rn) {
			set_state(V90A_DATA);
			set_rates(rate_tx, rate_rx);
			v90a_quiet = 0;
			v90a_rn = 0;
			v90rx.ev_rd = v90rx.ev_rdbar = v90rx.rd_run = 0;
			DSP_DBG("v34: V.92 renegotiated " RATE_FMT " (analogue "
				"modem)\n", rate_tx, rate_rx);
			break;
		}
		if (v90rx.ev_b1d && patx.seg == V92A_DATA &&
		    patx.frames > 48) {
			set_state(V90A_DATA);
			set_rates(rate_tx, rate_rx);
			v90a_quiet = 0;
			if (retraining && connected) {
				retraining = 0;
				DSP_DBG("v34: V.92 %s " RATE_FMT
					" (analogue modem)\n",
					moh_end || moh_backed ?
					"back from hold," : "retrained",
					rate_tx, rate_rx);
				moh_end = moh_backed = 0;
				break;
			}
			retraining = 0;
			connected = 1;
			DSP_DBG("v34: V.92 connect " RATE_FMT
				" (analogue modem)\n",
				rate_tx, rate_rx);
			stat_dp(DP_V92);
			return DPSTAT_CONNECT;
		}
		if (c >= ms2samples(4000, srate))
			return v92a_up_fail(v90rx.ev_ed ? "no B1d" :
					    "no Ed");
		break;

	/* V.90 analogue modem, phase 4 (9.4.2/V.90): CPt until the
	   digital modem's Ri-bar, CP (no SCR) after the CPt frame going
	   out, CP' after the one going out once MP is in, E after a whole
	   CP' once MP' or Ed is in; B1d on CP's constellations.  A
	   timeout, here or in phase 3, retrains (9.5.2.1/V.90:
	   v90a_fail()) */
	case P4V_CPT:
		/* SCR (ones, as CP is sent: a frame of 2 bits) after the
		   CPt going out */
		if (v90rx.ev_ribar) {
			std::fill_n(ptx.cp_next, 2, 1);
			ptx.cp_next_n = 2;
			set_state(P4V_SCR);
			break;
		}
		if (c >= ms2samples(5000, srate)) {
			DSP_DBG("v34: V.90: no Ri-bar (Ri %s)\n",
				v90rx.ev_ri ? "seen" : "not seen");
			return v90a_fail(0);
		}
		break;
	case P4V_SCR:
		/* CP once TRN2d's known part is in (SCR for at most
		   4000 ms, 9.4.2.2/V.90) */
		if (v90rx.ev_trn2) {
			if (v90a_cp_design())
				return v90a_fail(1);
			v90a_cp.ack = 0;
			ptx.cp_next_n = v90a_cp.build(ptx.cp_next);
			set_state(P4V_CP);
			break;
		}
		if (c >= ms2samples(3500, srate)) {
			DSP_DBG("v34: V.90: no TRN2d\n");
			return v90a_fail(0);
		}
		break;
	case P4V_CP:
		/* CP' after a CP has gone out whole */
		if (v90rx.ev_mp && !ptx.cp_next_n && ptx.cp_frames >= 1) {
			v90a_cp.ack = 1;
			ptx.cp_next_n = v90a_cp.build(ptx.cp_next);
			set_state(P4V_CPACK);
			break;
		}
		if (c >= ms2samples(4000, srate)) {
			DSP_DBG("v34: V.90: no MP (TRN2d %ld of %ld "
				"symbols wrong)\n",
				static_cast<long>(v90rx.trn_sym_err),
				static_cast<long>(v90rx.trn_sym));
			return v90a_fail(0);
		}
		break;
	case P4V_CPACK:
		if (!ptx.cp_next_n && ptx.cp_frames >= 1 &&
		    (v90rx.ev_mpack || v90rx.ev_ed)) {
			/* the data mode's encoder takes over after E */
			if (v90a_data_setup())
				return v90a_fail(1);
			ptx.cp_to_e = 1;
			set_state(P4V_E);
			break;
		}
		if (c >= ms2samples(4000, srate)) {
			DSP_DBG("v34: V.90: no MP'\n");
			return v90a_fail(0);
		}
		break;
	case P4V_E:
		/* B1d in and our E out (B1, the encoder's first frame of
		   ones, after it): CONNECT */
		if (v90rx.ev_b1d && ptx.seg == V34_TX_DATA &&
		    v90a_rn) {
			/* back from a rate renegotiation */
			set_state(V90A_DATA);
			set_rates(rate_tx, rate_rx);
			v90a_quiet = 0;
			v90a_rn = 0;
			v90rx.ev_rd = v90rx.ev_rdbar = v90rx.rd_run = 0;
			DSP_DBG("v34: V.90 renegotiated " RATE_FMT " (analogue "
				"modem)\n", rate_tx, rate_rx);
			break;
		}
		if (v90rx.ev_b1d && ptx.seg == V34_TX_DATA &&
		    retraining && connected) {
			/* back from a retrain: no new CONNECT */
			set_state(V90A_DATA);
			set_rates(rate_tx, rate_rx);
			v90a_quiet = 0;
			retraining = 0;
			DSP_DBG("v34: V.90 %s " RATE_FMT " (analogue modem)\n",
				moh_end || moh_backed ? "back from hold," :
				"retrained", rate_tx, rate_rx);
			moh_end = 0;
			moh_backed = 0;
			break;
		}
		if (v90rx.ev_b1d && ptx.seg == V34_TX_DATA) {
			set_state(V90A_DATA);
			set_rates(rate_tx, rate_rx);
			v90a_quiet = 0;
			retraining = 0;
			connected = 1;
			DSP_DBG("v34: V.90 connect " RATE_FMT
				" (analogue modem)\n",
				rate_tx, rate_rx);
			stat_dp(DP_V90);
			return DPSTAT_CONNECT;
		}
		if (c >= ms2samples(4000, srate)) {
			DSP_DBG("v34: V.90: no B1d (Ed %s)\n",
				v90rx.ev_ed ? "seen" : "not seen");
			return v90a_fail(0);
		}
		break;
	case V90A_DATA:
		/* the PCM signal gone (the digital modem hung up, or the
		   line went): carrier lost.  Retrains and renegotiations
		   are stage F's */
		v90a_quiet = v90rx.dpow < 1e-3f * v90rx.level * v90rx.level ?
			v90a_quiet + 1 : 0;
		if (v90a_quiet >= carrier_loss) {
			DSP_DBG("v34: V.90: carrier lost\n");
			return DPSTAT_ERROR;
		}
		if (c % ms2samples(100, srate) == 0 && v90rx.fe_n)
			stat_snr(10 * log10f(v90rx.dpow /
					     (v90rx.data_mse() + 1e-6f)));
		if (c % ms2samples(1000, srate) == 0)
			DSP_DBG("v34: V.90 data: error %.2f rms (%.1f dB), "
				"timing %+.2f ppm, phase %+.3f\n",
				sqrtf(v90rx.data_mse()),
				10 * log10f(v90rx.dpow /
					    (v90rx.data_mse() + 1e-6f)),
				v90rx.ppm(), v90rx.t_phase);
		/* the digital modem's rate renegotiation: its Rd (no data
		   from it on), our S after Rd-bar (9.6.2.2/V.90) */
		if (v90rx.ev_rdbar && v92a_up_on) {
			v92a_reneg(1, "");
			break;
		}
		if (v90rx.fpe_on && v92a_up_on) {
			v92a_fpe(1);
			break;
		}
		if (v90rx.ev_rdbar) {
			v90a_reneg(1);
			break;
		}
		/* our own (9.6.2.1/V.90): the downstream's error 2.5
		   times the noise CP was designed for (8 dB worse) for a
		   second, or under half of it for 20 s with a faster CP to
		   be had; a CP designed anew on what the line is now */
		{
			float er = sqrtf(v90rx.data_mse());
			s16 settled = c >= ms2samples(2000, srate);
			v90a_bad = settled && er > 2.5f * v90a_cp_noise ?
				v90a_bad + 1 : 0;
			v90a_good = settled && er < 0.5f * v90a_cp_noise &&
				v90a_cp.drn < 22 ? v90a_good + 1 : 0;
		}
		if (v90rx.ev_rd)
			break;
		if (v90a_bad >= ms2samples(1000, srate)) {
			v90rx.reneg_wait();
			v90a_reneg_ours("errors");
		} else if (v90a_good >= ms2samples(20000, srate)) {
			/* with PCM upstream a fast parameter exchange: no
			   training, the downstream's CPu designed on the
			   data mode's error */
			if (v92a_up_on)
				v92a_fpe(2, "margin");
			else {
				v90rx.reneg_wait();
				v90a_reneg(2, "margin");
			}
		} else if (v92a_fpe_at && c >= v92a_fpe_at && v92a_up_on &&
			   !v92a_fpes) {
			/* test: DSP_V92A_FPE */
			v92a_fpe(2, "test");
		} else if (v90a_rn_at && c >= v90a_rn_at && !v90rx.renegs) {
			/* test: DSP_V90A_RENEG */
			v90rx.reneg_wait();
			v90a_reneg_ours("test");
		} else if (retrain_at && c >= retrain_at && !retrains)
			/* test: DSP_V34_RETRAIN (9.5.2.1/V.90) */
			retrain("test");
		break;
	case V90A_RN_S:
		if (ptx.n >= 128)
			tx_next(V90A_RN_SBAR, V34_TX_SBAR);
		break;
	case V90A_RN_SBAR:
		/* then SCR (as in phase 4: ones, as CP is sent) while
		   TRN2d comes, and CP once it shows the noise */
		if (ptx.n >= 16) {
			ptx.bits[0] = ptx.bits[1] = 1;
			ptx.nbits = 2;
			ptx.scr = 0;
			ptx.idx = 0;
			tx_next(P4V_SCR, V34_TX_CP);
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
		/* (longer, up to another TRN3_LEN, while a far echo found
		   late trains) */
		if (ptx.n >= TRN3_LEN &&
		    (!ec_far_late() || ptx.n >= 2 * TRN3_LEN)) {
			tx_next(P3A_J, V34_TX_J);
			prx.ev_s = 0;
			/* the caller starts its phase 3 on our J: whatever the
			   receiver took for its S until now was our own S, PP
			   and TRN's echo, trained on (the gain held, phase 4
			   expected) */
			if (prx.stage != V34_RX_IDLE || prx.phase4)
				prx.restart();
		}
		break;
	case P3A_J:
		/* until the caller's S */
		if (prx.ev_s && ptx.n % 8 == 0)
			tx_next(P3A_WAIT, V34_TX_SILENCE);
		break;
	case P3A_WAIT:
		/* until the caller's J; or its MP, when its J was missed
		   (the equalizer still training on its short phase 3): it
		   has gone on to phase 4, so on to ours too, our TRN 4
		   point without its J's say */
		if (prx.ev_j || prx.ev_mp) {
			if (!prx.ev_j)
				DSP_DBG("v34: the caller's MP without its J: "
					"on to phase 4\n");
			tx_next(P4A_S, V34_TX_S);
		}
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
		/* (longer, up to another TRN3_LEN, when the echo canceller
		   waited for the remote's J: a second of its training; and
		   while a far echo found late trains) */
		if (ptx.n >= TRN3_LEN &&
		    (((!ec.waited || ec.n >= ms2samples(1000, srate)) &&
		      !ec_far_late()) || ptx.n >= 2 * TRN3_LEN)) {
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
				s16 i, n = 0;
				for (i = 0; i < v90tx.enc.M[0] && n < 380; i++)
					n += snprintf(buf + n, sizeof(buf) - n,
						      " %d",
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
			s16 k;
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
				DSP_DBG("v34: V.90 renegotiated " RATE_FMT "\n",
					rate_tx, rate_rx);
				if (rate_rx / 2400 < max_rate_rx)
					rn_max = rate_rx / 2400;
				break;
			}
			rn_max = rate_rx / 2400;
			if (retraining && connected) {
				retraining = 0;
				DSP_DBG("v34: V.90 %s, " RATE_FMT "\n",
					moh_end || moh_backed ?
					"back from hold" : "retrained",
					rate_tx, rate_rx);
				moh_end = 0;
				moh_backed = 0;
				break;
			}
			retraining = 0;
			connected = 1;
			DSP_DBG("v34: V.90 connect " RATE_FMT "\n", rate_tx,
				rate_rx);
			stat_dp(v92_on ? DP_V92 : DP_V90);
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
		   ours: with its MP', or with its E (a V.92 modem whose MP
		   came before ours, our first one an MP' then, went from
		   its MP to E without an MP' of its own) */
		if (prx.ev_mp && (prx.mp.ack || prx.ev_e) && ptx.mp_sent >= 1 &&
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
				DSP_DBG("v34: renegotiated, " RATE_FMT "\n",
					rate_tx, rate_rx);
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
				DSP_DBG("v34: %s, " RATE_FMT "\n",
					moh_end || moh_backed ?
					"back from hold" : "retrained",
					rate_tx, rate_rx);
				moh_end = 0;
				moh_backed = 0;
				break;
			}
			retraining = 0;
			connected = 1;
			DSP_DBG("v34: connect " RATE_FMT "\n", rate_tx,
				rate_rx);
			if (tx_loop_on)
				DSP_DBG("v34 tx: on the remote's clock (loop "
					"timing), ours %+.0f ppm off it\n",
					1e6 * tx_clk / ptx.qam.sps);
			stat_dp(DP_V34);
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
				retrain("tone A", RT_REMOTE);
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
		/* the receiver's quality, for the call's statistics */
		if (prx.data_on && !lost && c % ms2samples(100, srate) == 0)
			stat_snr(10 * log10f(prx.sig / (prx.mse + 1e-6f)));
		if (rt_blocks >= 6)
			retrain(caller ? "tone A" : "tone B", RT_REMOTE);
		else if (prx.ev_reneg) {
			prx.ev_reneg = 0;
			if (pcm)
				v90_renegotiate("the remote's S",
						reneg_rate(0), 1);
			else
				renegotiate("the remote's S", reneg_rate(0),
					    true);
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
		} else if (reneg_at && c >= reneg_at && !reneg_test) {
			reneg_test = true;
			if (pcm)
				v90_renegotiate("test", reneg_to, 0);
			else
				renegotiate("test", reneg_to);
		} else if (retrain_at && c >= retrain_at && !retrains)
			/* test: DSP_V34_RETRAIN (V.90's digital modem too) */
			retrain("test");
		else if (pcm)
			/* (carrier loss above) */
			;
		else if (bad >= ms2samples(1000, srate) &&
			 c >= ms2samples(5000, srate))
			/* (not in a loop on a line that stays marginal) */
			retrain("errors");
		else if (lost >= ms2samples(30, srate))
			retrain("signal lost");
		break;
	/* renegotiations and retrains */
	case V34_RN_S:
	case V34_RN_SBAR:
	case V34_RN_TRN:
	case V34_RT_SILENCE:
		return rt_step(c);
	/* modem-on-hold */
	case MOH_REQ:
	case MOH_HELD:
	case MOH_RESP:
	case MOH_BACK:
		return moh_step(c);
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
			return v8tx.pos < v8tx.n ?
				       static_cast<s16>(v8tx.b[v8tx.pos++]) :
				       1;
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
		if (tx_loop) {
			const float TX_CLK_SLEW = 2e-7f, TX_CLK_PPM = 80;
			/* (from an offset of TX_CLK_PPM on, for the call:
			   less is in every modem's tolerance) */
			if (prx.timing_fixed && !tx_loop_on &&
			    fabsf(prx.t_freq / ptx.qam.sps) > TX_CLK_PPM * 1e-6f)
				tx_loop_on = 1;
			float want = tx_loop_on && prx.timing_fixed ? prx.t_freq : tx_clk;
			tx_clk += std::max(-TX_CLK_SLEW, std::min(TX_CLK_SLEW, want - tx_clk));
			ptx.qam.set_clock(tx_clk);
		}
		y = ptx.sample();
		break;
	case TXM_PAM:
		/* on the network's clock, once the PCM receiver has it */
		if (v90rx.t_on)
			patx.set_ppm(v90rx.ppm());
		y = patx.sample();
		break;
	}
	return y;
}

/* one sample at 9600 Hz each way; ref: what goes out on the line instead
   (the PCM signal, for the echo canceller) */
s16 V34Pump::sample(float x, float *y, const float *ref)
{
	s16 r = handshake();
	if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
		return r;
	*y = dsp_clip16(pump_tx_sample());
	last_tx = ref ? *ref : *y;
	pump_rx_sample(x);
	if (renegotiating && total >= timeout)
		retrain("renegotiation timed out");
	/* no Ed within 5000 ms plus 2 round trips of our S-bar
	   (9.6.2/V.90) */
	if (v90a_rn && total >= timeout) {
		DSP_DBG("v34: V.90: renegotiation timed out\n");
		if (v90a_fail(0) == DPSTAT_ERROR)
			return DPSTAT_ERROR;
	}
	if (state != V34_DATA && state != V90A_DATA && total >= timeout) {
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
s16 V34Pump::process(void *in, void *out, s32 cnt)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	s16 ret = DPSTAT_OK, r, k, n;
	s32 i;
	float y, up[2], down[2], pcm_ = 0;

	/* silence where an early return (V.8 done, another pump, an error)
	   leaves the block unwritten */
	std::fill_n(dst, cnt, 0);
	stat_link();
	for (i = 0; i < cnt; i++) {
		if (state == V91_RUN) {
			s16 y91, ev = v91m.sample(src[i], &y91);
			dst[i] = static_cast<s16>(y91);
			if (ev == V91_CONNECT) {
				rate_tx = (v91m.cp_rx.drn + 20) *
					INT32_C(8000) / 6;
				rate_rx = (v91m.our_drn + 20) *
					INT32_C(8000) / 6;
				set_rates(rate_tx, rate_rx);
				DSP_DBG("v34: V.91 connect " RATE_FMT "\n",
					rate_tx, rate_rx);
				stat_dp(DP_V91);
				ret = DPSTAT_CONNECT;
			} else if (ev == V91_FAIL)
				return DPSTAT_ERROR;
			else if (ev == V91_ESCAPE)
				v91_escape();
			continue;
		}
		/* (handshake() does not run here; ours leaves V92_RUN) */
		if (state == V92_RUN) {
			moh_req_test();
			link_req_poll();
		}
		if (state == V92_RUN) {
			/* the echo of our signal from the analogue modem's
			   hybrid, after the network's round trip */
			float x92 = src[i];
			s16 y92;
			s16 ev;
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
			ev = v92u.sample(static_cast<s16>(lrintf(x92)), &y92);
			last_tx = static_cast<float>(y92);
			dst[i] = static_cast<s16>(y92);
			out_n++;
			if (ev == V92_CONNECT) {
				rate_tx = (v92u.cpu.drn + 20) *
					INT32_C(8000) / 6;
				rate_rx = (v92u.cpd.drn + 17) *
					INT32_C(8000) / 6;
				set_rates(rate_tx, rate_rx);
				if (retraining && connected) {
					/* after a retrain, or back from hold:
					   the link goes on */
					DSP_DBG("v34: V.92 %s, " RATE_FMT "\n",
						moh_end || moh_backed ?
						"back from hold" : "retrained",
						rate_tx, rate_rx);
					moh_backed = 0;
				} else {
					DSP_DBG("v34: V.92 connect " RATE_FMT
						"\n",
						rate_tx, rate_rx);
					stat_dp(DP_V92);
					ret = DPSTAT_CONNECT;
				}
				retraining = 0;
				connected = 1;
				moh_end = 0;
			} else if (ev == V92_RENEG) {
				rate_tx = (v92u.cpu.drn + 20) *
					INT32_C(8000) / 6;
				rate_rx = (v92u.cpd.drn + 17) *
					INT32_C(8000) / 6;
				set_rates(rate_tx, rate_rx);
				DSP_DBG("v34: V.92 renegotiated " RATE_FMT "\n",
					rate_tx, rate_rx);
			} else if (ev == V92_FAIL || ev == V92_RETRAIN) {
				/* before data, V.34 upstream next time; a
				   renegotiation that fails retrains */
				/* (with the prefilter, and filters p1 and
				   z2 alone in its INFO1a: once more without
				   the prefilter, which the ESS modem takes
				   no B1u on, see v92.cpp) */
				if (!v92u.connected && ev == V92_FAIL &&
				    v92u.pf_on && !v92u.ia.sections &&
				    !pcmup_plain) {
					pcmup_plain = 1;
					DSP_DBG("v34: PCM upstream once more, "
						"without the prefilter\n");
				} else if (!v92u.connected) {
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
			float x = src[i];
			if (test_echo.g)
				x += test_echo.echo();
			r = sample(x, &y, nullptr);
			if (test_echo.g)
				test_echo.push(y);
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
			if (r != DPSTAT_OK)
				ret = r;
			dst[i] = static_cast<s16>(y);
			continue;
		}
		/* an 8000 Hz line: the pump runs at 9600 Hz in between */
		if (test_echo.g)
			src[i] = dsp_clip16(src[i] + test_echo.echo());
		if (pcm_tx) {
			/* V.90: exact PCM codes at 8000 Hz; at 9600 Hz for the
			   echo canceller */
			float ref[2];
			s16 m;
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
				std::copy(ref_fifo + 1, ref_fifo + ref_n,
					  ref_fifo);
				ref_n--;
			}
			r = sample(up[k], &y, pcm_tx ? &rf : nullptr);
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
			std::copy(fifo + 1, fifo + fifo_n, fifo);
			fifo_n--;
		}
		if (pcm_tx)
			y = pcm_;
		dst[i] = dsp_clip16(y);
		if (test_echo.g)
			test_echo.push(dst[i]);
		/* V.90's analogue modem: the PCM receiver on the line's
		   samples, our echo (of what goes out with them) taken
		   away at 8000 Hz */
		if (v90a_rx) {
			float x = src[i];
			ec_schedule();
			if (!ec.bypass)
				x = ec.run(dst[i], x);
			v90rx.line(x);
		}
		out_n++;
	}
	return ret;
}

/* PumpOps' create: what V.8 offers for id (V.90, V.92 and V.91 only
   at 8000 Hz), the DSP_* test settings, S7 and S10; srate_ 9600, or
   8000 through converters (-1 for any other) */
s16 V34Pump::start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_, s32 max_frag)
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
		env_on("DSP_V92");
	qc_n = -1;
	short2_ok = env_on("DSP_V92_SHORT2");
	/* quick connect: DSP_V92_QC=0 answers QC1a with V.8 instead */
	qc_ok = env_on("DSP_V92_QC");
	qca_ok = env_on("DSP_V92A_QC");
	qcc_sent = 0;
	/* modem-on-hold: the T1 code (Table 33/V.92) we grant, +PMHT's
	   (0 with +PMH=1), or DSP_V92_MOH=<code> (1 10 s ... 12 16 min, 13
	   no limit; 0 denies it); 1 min if neither says */
	moh_t1 = env("DSP_V92_MOH") ? env_int("DSP_V92_MOH") & 15 :
		static_cast<s16>(modem_get_param(m, MDMPRM_MOH_T1));
	/* adaptive answer: the core's (+FAA, or its program's own fax
	   receiver), for an answerer */
	{
		intptr_t a = caller ? 0 : modem_get_param(m, MDMPRM_ADAPTIVE);
		v8_adaptive = a != 0;
	}
	if (const char *e = env("DSP_V92_MOH_REQ")) {
		moh_req_at = ms2samples(atoi(e), srate_);
		moh_req_hold = strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 10;
	}
	if (moh_t1 < 0 || moh_t1 > 13)
		moh_t1 = 5;
	ev_mh = moh_resp = moh_drop = 0;
	moh_end = 0;
	/* PCM upstream: DSP_V92_PCMUP=0 does not offer it */
	pcmup_ok = env_on("DSP_V92_PCMUP");
	pcmup_plain = 0;
	/* V.91 with another digital modem: DSP_V91=0 leaves it out */
	v91 = (id == DP_V90 || id == DP_V92 || id == DP_V91) &&
		line_rate == 8000 &&
		env_on("DSP_V91");
	/* V.cvm (+MS=V_CVM): its field after +CVM's mode set, the pump
	   at 8000 Hz (a 9600 Hz line through converters) */
	vcvm = id == DP_VCVM;
	{
		intptr_t v = modem_get_param(m, MDMPRM_VCVM);
		const struct dp_vcvm *vi = v == -1 || v == 0 ? nullptr :
			reinterpret_cast<const struct dp_vcvm *>(v);
		vcvm_caps = vcvm_caps_for(vi ? vi->modes : VCVM_M_ALL, 8000);
	}
	/* the G.711 law of the network (D-Modem knows it from the codec of
	   the call) */
	pump_setup(m, srate_, caller_);
	tx_loop = caller_ && env_int("DSP_V34_LOOP_TIMING", 1);
	tx_clk = 0;
	tx_loop_on = 0;
	alaw = rt && rt->law ? rt->law == 2 :
		env_is("DSP_G711", "alaw");
	v8_low = caller_ && rt ? rt->v8_for : 0;
	if (rt)
		rt->vcvm_peer = 0;
	if (v8_low)
		vcvm = 0;
	/* an analogue line (dp_runtime_set_analog()), at 9600 or 8000 Hz,
	   has none of the digital modem's modes */
	analog = line_rate == 9600 || (rt && rt->analog);
	if (analog)
		v90 = v91 = v92 = 0;
	/* V.90 as the analogue modem, on an analogue line at 9600 Hz
	   (DSP_V90A=0 leaves it out).  At 8000 Hz only with DSP_V90A=1:
	   there the band's top folds onto itself in the codec, and half a
	   sample off the network's symbols it cancels (21 dB at best), so
	   a codec whose clock is not the network's, its phase drifting,
	   loses the PCM signal every so often (each 1.25 s at 100 ppm) */
	{
		const char *e = env("DSP_V90A");
		v90a = (id == DP_V90 || id == DP_V92) && analog && !v8_low &&
			(line_rate == V34_SRATE ? !(e && !atoi(e)) :
			 e && atoi(e));
	}
	v90a_on = v90a_rx = 0;
	v92a = v90a && id == DP_V92 &&
		env_on("DSP_V92A");
	v92a_on = 0;
	v92a_up = env_on("DSP_V92A_UP");
	v92a_noup = v92a_up_on = 0;
	v92a_s2_req = v92a_short2 = 0;
	v92a_renegs = 0;
	v92a_fpes = 0;
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
	mate_g.init(1800, PROBE_PERIOD, srate_);
	/* test: DSP_V34_RETRAIN=<ms in data mode>[a|c], c or a: only the
	   caller or the answerer starts it */
	retrain_at = test_samples("DSP_V34_RETRAIN");
	v90a_rn_at = test_samples("DSP_V90A_RENEG");
	v92a_fpe_at = test_samples("DSP_V92A_FPE");
	v90a_rn = 0;
	v90a_v34 = 0;
	/* test: DSP_V34_PRECODE_MIN=<dB>: ask for precoding from that gain
	   on (V34_PRECODE_MIN) */
	precode_min = static_cast<float>(env_double("DSP_V34_PRECODE_MIN",
						      V34_PRECODE_MIN));
	/* test: DSP_V34_INFO0=<hex>[,a|,c]: our INFO0's bits 12:19 (what
	   it offers of the symbol rates and carriers), on both sides or
	   only the answerer's or the caller's */
	info0_rates = 0xFF;
	if (const char *e = env("DSP_V34_INFO0")) {
		char *side;
		long v = strtol(e, &side, 16);
		if (*side == ',')
			side++;
		if (!*side || *side == (caller_ ? 'c' : 'a'))
			info0_rates = static_cast<s16>(v & 0xFF);
	}
	/* test: DSP_V34_LOSE_INFO0=[a|c]: our first INFO0 with a bad CRC,
	   lost to the remote (phase 2's recovery, 11.2.2/V.34), only the
	   answerer's or the caller's (anything else: either) */
	lose_info0 = pump_test_env("DSP_V34_LOSE_INFO0", caller_) != nullptr;
	reneg_test = false;
	test_echo.init(pump_test_env("DSP_TEST_ECHO", caller_), line_rate);
	/* test: DSP_V34_RENEG=<ms in data mode>:<N>[a|c]: a rate
	   renegotiation to N * 2400 bit/s (what we receive) */
	if (const char *e = pump_test_env("DSP_V34_RENEG", caller_)) {
		s32 n = strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
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
	/* +MS=V91 on a 4-wire leased circuit (AT&L2, a digital line, the
	   core starting us with no call progress): V.91 from INFO on, with
	   no V.8 (8.8/V.91) */
	if (id == DP_V91 && v91 && modem_get_sreg(m, SREG_LEASED) == 2) {
		v91_on = 1;
		v91_start(1);
	}
	return 0;
}

/* frees what the receiver holds; PCM off for the next pump */
void V34Pump::cleanup()
{
	if (rt)
		rt->pcm = 0;
	if (prx_on)
		prx.release();
	ec.ls_free();
}

/* DP_V8 is what the modem core starts for the default (fastest) mode;
   V.90 (the digital modem) and V.91 run from V.8 on this pump too, and
   V.92 starts it as well (V.90 or V.34) */
static const enum DP_ID v34_ids[] = { DP_V8, DP_V34, DP_V90, DP_V91, DP_V92 };

/* this pump for v34_ids */
s16 dp_v34_init(void)
{
	s16 i;
	for (i = 0; i < static_cast<s16>(std::size(v34_ids)); i++)
		modem_dp_register(v34_ids[i], &PumpOps<V34Pump>::ops);
	return 0;
}

/* and no more */
void dp_v34_exit(void)
{
	s16 i;
	for (i = 0; i < static_cast<s16>(std::size(v34_ids)); i++)
		modem_dp_deregister(v34_ids[i], &PumpOps<V34Pump>::ops);
}

} // namespace dsp
