/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    mnp.h - MNP 2 to 4: V.42's alternative procedure (Annex A of V.42
 *            (10/96)), the error correction of modems from before LAPM
 *
 *    The establishment exchange (LR, LR, LA) goes in start-stop octets
 *    (MnpAsync: SYN DLE STX, the header and information with DLE
 *    doubled, DLE ETX, an FCS of x^16 + x^15 + x^2 + 1); the data phase
 *    in what the LRs agreed, octets (framing mode 2) or HDLC frames
 *    (mode 3, Hdlc: flags, zero insertion, LAPM's FCS).  LT frames
 *    carry the data, numbered modulo 256, LA frames acknowledge them
 *    and give credit (up to k outstanding), LN and LNA carry a break,
 *    LD ends the connection.  The timers count transmitted bits at the
 *    data pump's rate, as LAPM's T401.
 *
 *    MNP 10 (Microcom's, as its DeskPorte FAST firmware has it:
 *    docs/link.md) rides on the same LRs and frames: parameter 8's
 *    bit 4 and parameter 0x0C agree on it, type 8 frames carry its
 *    rate-shift signals and LMI probes, type 9 is an LA with a probe's
 *    acknowledgement.  The probes grow the LTs' size while they come
 *    back, retransmissions shrink it; the line's quality moves the call
 *    up and down a ladder of rates, the pumps retraining, renegotiating
 *    or changing modulation (Owner::mnp10_shift()) while the link stays
 *    up.  Between two D-Modems, LR parameter 0xC0 opens the ladder to
 *    every modulation both can run on the line.
 *
 */

#ifndef DMODEM_LINK_MNP_H
#define DMODEM_LINK_MNP_H


#include "backlog.h"
#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

enum {
	MNP_MAX_K = 8,          /* the LT frames outstanding we offer */
	MNP_MAX_INFO = 256,     /* the largest information field (N401) */
	MNP_DFLT_INFO = 64,     /* without parameter 4 */
};

struct Config;
class Mnp;

/* the modulations of MNP 10's rate ladder; D-Modem's extension (LR
   parameter 0xC0) names them by these numbers, as bits */
enum class Mod : u8 {
	V92, V90, K56, X2,      /* PCM, each a rung of its own: the pump
				   picks its rates */
	V34, VFC, V32T, V32B, HST,
	V26T, V22B, V22, B212, V23, V21, B103,
	V33, V29, V27T, V26, B208, B201, B202,     /* 4-wire leased */
	Count
};

/* what a rate shift asks of the pumps: a retrain or a rate
   renegotiation of the one running, or the call's start-up again in
   another modulation (or the same one), the link staying up; at rate
   (0: the pump's best), up or down the ladder */
enum class ShiftKind : u8 { Retrain, Reneg, Restart };
struct Shift {
	ShiftKind kind;
	Mod mod;
	s32 rate;
	bool up;
	/* we asked for it (the responder's pump follows the initiator's
	   retrain rather than starting one of its own) */
	bool lead;
};

/* MNP 10's rung: a modulation at a rate */
struct Rung {
	Mod mod;
	s32 rate;
};

/* A.3/V.42: start-stop octets (as Async), each frame SYN DLE STX, its
   body (header and information) with each DLE doubled, DLE ETX and the
   FCS (x^16 + x^15 + x^2 + 1 over the body and the ETX, low octet
   first); marks between frames */
class MnpAsync : public BitFramer<MnpAsync> {
public:
	/* MNP's frames: none being sent, the receiver hunting for SYN */
	void start(Link *link, Mnp *mnp);
	/* the detection phase took SYN DLE STX: the frame's body follows,
	   the first of it in rx (the detector's bits) */
	void start_in_frame(const BitQueue &rx);

private:
	friend class BitFramer<MnpAsync>;
	enum class Tx { Idle, Start, Body, End, Done };
	enum class Rx { Hunt, Syn, Dle, Body, BodyDle, Fcs1, Fcs2 };
	bool tx_fill();
	s32 tx_idle(s16 nbits, u8 mask, u8 *&buf, s32 &n);
	void rx_scan();
	void rx_char(u8 ch);
	void tx_char(u8 ch);
	void rx_restart(Rx state);

	Mnp *mnp_ = nullptr;
	Frame *tx_frame_ = nullptr;
	Tx tx_state_ = Tx::Idle;
	s16 tx_step_ = 0;               /* within Start and End */
	bool tx_dle_ = false;           /* a DLE to double */
	u16 tx_fcs_ = 0;
	Rx rx_state_ = Rx::Hunt;
	Frame rx_frame_ = {};
	u8 rx_buf_[3 + 300];
	u16 rx_fcs_ = 0;
	u8 rx_fcs_lo_ = 0;
};

/* the procedure: establishment as initiator (the originator) or
   responder, the data phase, LD */
class Mnp : public Framer, public LinkProto {
public:
	/* a call: what we offer (mnp_mode, mnp_k, mnp_info, mnp5 of cfg) */
	void init(Link *link, const Config &cfg);
	/* the end of the call */
	void exit();
	/* the establishment: the initiator sends its LR after some marks,
	   the responder waits for one */
	void start(bool initiator);
	/* we hang up: LD, then released once it is out */
	void disconnect() override;
	/* characters the DTE did not take: try again */
	void push_rest() override;
	/* LT frames not sent yet or not acknowledged */
	bool tx_pending() const override;
	/* the timers: bits more have gone out */
	void clock(s32 bits);
	/* the data phase's frames as HDLC frames (framing mode 3) */
	bool bit_tx() const { return bit_tx_; }
	/* (see bit_tx()) */
	bool bit_rx() const { return mode_ == 3 && phase_ != Phase::Idle &&
					      phase_ != Phase::Init; }

	/* what the LRs agreed (the framing mode, k, N401, the data phase
	   optimization, MNP 5) */
	s16 mode() const { return mode_; }
	/* (see mode()) */
	s16 k() const { return k_; }
	/* (see mode()) */
	s16 n401() const { return n401_; }
	/* (see mode()) */
	s16 optimization() const { return opt_; }
	/* (see mode()) */
	bool mnp5() const { return mnp5_; }
	/* MNP 10 agreed; and its cellular mode */
	bool mnp10() const { return m10_; }
	/* (see mnp10()) */
	bool cellular() const { return cell_; }
	/* D-Modem's extension agreed (LR parameter 0xC0 both ways) */
	bool mnp10_ext() const { return ext_; }
	/* the rate ladder and the rung the call is on (-1: none) */
	const Rung *ladder(s16 *n) const { *n = nrungs_; return rungs_; }
	/* (see ladder()) */
	s16 rung() const { return cur_; }
	/* our LT information field's size now */
	s16 lt_size() const;
	/* a rate shift's physical change is over (ok: the pump connected
	   at the rate it reports), from the owner (Link::shift_done()) */
	void shift_done(bool ok);
	/* a shift is waiting for the pumps */
	bool shifting() const { return sh_ == Sh::Pump; }
	/* MNP 10's parameters (3 and 8 on) of our LR for V.42's XID
	   (-K1): up to n octets into b, how many */
	s16 upgrade_params(u8 *b, s16 n, bool initiator);
	/* the remote's in its XID: agreed (as by an LR); the link then
	   runs MNP in HDLC frames, the initiator's LA first */
	bool upgrade_rx(const u8 *p, s16 len, bool initiator);
	/* the data phase's start after the XIDs agreed it */
	void upgrade_start(bool initiator);

	/* MnpAsync's side: its frames, and one with a bad FCS (bit: an
	   HDLC frame's) */
	Frame *next_octet_frame();
	void rx_octet_frame(Frame *f);
	void rx_bad_frame(bool bit);
	/* Hdlc's (the data phase in mode 3) */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;
	/* a frame with a bad FCS from Hdlc (rx_bad_frame(true)) */
	void rx_error() override { rx_bad_frame(true); }

private:
	enum class Phase {
		Idle,
		Init,           /* the initiator's marks before its LR */
		Request,        /* the initiator's LR out, waiting for one */
		Confirm,        /* its LA to go (then the data phase) */
		Listen,         /* the responder waiting for an LR */
		Respond,        /* its LR out, waiting for the LA */
		Data,
		Disc,           /* LD to go, then released */
	};
	enum {
		N400 = 12,              /* retransmissions */
		EST_TRIES = 3,          /* LRs sent before giving up */
	};
	/* frame types */
	enum { LR = 1, LD = 2, LT = 4, LA = 5, LN = 6, LNA = 7 };
	struct Slot {
		Frame frame;
		u8 buf[5 + MNP_MAX_INFO];
	};

	Frame *next_frame();
	Frame *ctrl(s16 len);
	Frame *build_lr();
	Frame *build_la();
	Frame *build_ld();
	Frame *build_lna();
	Frame *next_lt();
	void rx_frame(Frame *f, bool bit);
	bool rx_lr(const u8 *h, s16 len);
	void rx_lt(const u8 *h, s16 len, const u8 *info, s16 n);
	void rx_la(const u8 *h, s16 len);
	void rx_ln(const u8 *h, s16 len);
	void rx_ld(const u8 *h, s16 len);
	void data_phase();
	void fail(s16 reason);
	void fallback();
	void t401_expired();
	bool can_send(u8 ns) const;
	s32 t401_est() const;
	s32 t401_data() const;
	s32 rate() const;

	/* MNP 10 (mnp10.cpp) */
	enum class Sh {
		Idle,
		FfReq,          /* our fall forward asked (04), the answer
				   awaited */
		FbReq,          /* our fallback asked (02) */
		Go,             /* the pumps' change once our signal is out */
		Pump,           /* the pumps changing (Owner::mnp10_shift()) */
		Resync,         /* the responder's wait for a frame after */
	};
	enum { MAX_RUNGS = 64 };
	s16 m10_lr(u8 *b, s16 n);
	void m10_rx_lr(const u8 *h, s16 len);
	bool m10_agree(bool initiator);
	void m10_local_0c();
	void m10_ladder();
	s16 m10_find(Mod mod, s32 rate) const;
	s16 m10_next(bool up) const;
	void m10_start();
	void m10_reset();
	void m10_tick();
	void m10_second();
	Frame *m10_frame();
	Frame *build_t8(u8 sub);
	Frame *build_probe();
	Frame *build_t9();
	void rx_t8(const u8 *h, s16 len);
	void rx_probe(const u8 *h, s16 len);
	void rx_ack(u8 r);
	void rx_report(s16 g, s16 e);
	void m10_heard(s16 type);
	void m10_sent(const Frame *f);
	void m10_lt_again();
	void p_grow(s16 d);
	void p_shrink();
	bool ff_test(bool responder) const;
	void ff_start();
	void fb_start();
	void go(s16 target, bool up);
	void pump(s16 target, bool up, bool restart);
	void shift_failed();
	void shift_over();
	void tx_level_report(s16 g, s16 e);
	static u8 g_code(s16 dbm);
	static u8 e_code(s16 eqm);

	/* what we offer, the remote's, what was agreed */
	bool m10_offer_ = false;
	bool m10_ = false, cell_ = false, ext_ = false;
	bool upgraded_ = false;         /* from V.42's XID (-K1) */
	u8 m10_ours_[4] = {}, m10_theirs_[4] = {}, m10_agreed_[4] = {};
	s16 m10_ours_n_ = 0;
	bool m10_seen_ = false;         /* the remote's LR had 0x0C */
	s16 m10_opt_ = 0;               /* the remote's parameter 8 */
	u32 ext_ours_ = 0, ext_theirs_ = 0;
	u8 ext_flags_ours_ = 0, ext_flags_theirs_ = 0;
	bool p11_ = false;              /* the remote's LR had 0x11 */
	bool fb_ok_ = false, ff_ok_ = false;
	s16 first_g_ = -1, first_e_ = -1;       /* parameter 0x0D */
	/* the ladder, the rung the call is on, modulations that failed */
	Rung rungs_[MAX_RUNGS] = {};
	s16 nrungs_ = 0, cur_ = -1;
	u32 failed_mods_ = 0;
	/* ticks (12.5 ms of bits at the pump's rate) */
	s32 tick_bits_ = 0;
	s32 ticks_ = 0;                 /* since the link's start or a shift */
	s16 sec_ticks_ = 0;
	/* the LMI probes: S, A, R; P; a type 9 due; the probe timer */
	u8 lmi_s_ = 0, lmi_a_ = 0, lmi_r_ = 0;
	s16 pkt_p_ = 0x20;
	bool t9_due_ = false, probe_due_ = false;
	s16 probe_ticks_ = 0;
	/* the error average X, the packet size's health H, an LT sent
	   again since X's last update; X high this many seconds (cellular) */
	s16 err_x_ = 0, health_ = 9;
	bool lt_again_since_ = false;
	s16 x_high_secs_ = 0;
	/* type 8 signals to send (subtype, copies), the signal sent last */
	u8 sig_ = 0;
	s16 sig_copies_ = 0;
	/* rate shifts: the state, its time left (ticks), the hold-off
	   (seconds left, and its length), the post-shift hold, the rung
	   asked for and the one before, a shift asked by the remote,
	   retries after a failure */
	Sh sh_ = Sh::Idle;
	s32 sh_ticks_ = 0;
	s32 holdoff_ = 30, holdoff_len_ = 30, post_hold_ = 10;
	s16 sh_target_ = -1, sh_from_ = -1;
	bool sh_up_ = false, sh_restart_ = false, sh_responder_ = false;
	bool ff_failed_ = false;
	s16 sh_fails_ = 0, cell_retries_ = 2;
	s32 shifts_ = 0;
	/* the line: our G and E now, the EQM's windows (16 ticks each, the
	   last four), the remote's reports */
	s16 g_ = 0xff, e_ = 0xff;
	s32 eqm_sum_ = 0;
	s16 eqm_n_ = 0;
	s16 eqm_win_[4] = {}, eqm_wins_ = 0;
	/* ticks in a row the pump gave no EQM (an FSK pump has none): the
	   fall forward then on P and X alone */
	s16 eqm_none_ = 0;
	s16 peer_g_ = 0xff, peer_e_ = 0xff;
	/* cellular transmit level: L, the report before, the level then,
	   the level's change pending */
	s16 lvl_ = 0, prev_e_ = -1, prev_lvl_ = -1;
	bool lvl_first_ = true;
	/* :T19: ticks since an LA, type 9 or type 8 at 1200 bit/s */
	s32 t19_ticks_ = 0;
	/* our settings */
	bool cfg_cell_ = false, cfg_t19_ = true, cfg_ext_ = true;
	s16 cfg_lowest_ = 2, cfg_level_ = 0;
	s32 cfg_top_ = 33600;

	Link *link_ = nullptr;
	Phase phase_ = Phase::Idle;
	bool initiator_ = false;
	bool bit_tx_ = false;
	/* our offer, then what was agreed */
	s16 our_mode_ = 3, our_k_ = MNP_MAX_K, our_n401_ = MNP_MAX_INFO;
	s16 our_opt_ = 3;
	bool our_mnp5_ = false;
	s16 mode_ = 2, k_ = 1, n401_ = MNP_DFLT_INFO, opt_ = 0;
	bool mnp5_ = false;
	s16 lr_n401_ = MNP_DFLT_INFO;   /* N401 agreed (before the 256-octet
					   optimization) */
	/* the parameters the initiator's LR had (the responder echoes
	   those it understood) */
	bool lr_opt_ = false;
	/* establishment */
	s16 tries_ = 0;
	bool heard_ = false;            /* a frame came, good or bad */
	s16 ld_reason_ = 0;
	/* control frames to send when the framer asks */
	bool lr_due_ = false, ld_due_ = false;
	/* timers: bits left, 0 when stopped */
	s32 t_start_ = 0, t401_ = 0, t402_ = 0, t404_ = 0;
	/* sending: V(S) the next new N(S), the oldest not acknowledged,
	   the next to (re)send; the remote's last N(R) and N(k) */
	u8 vs_ = 1, una_ = 1, nxt_ = 1;
	u8 nr_ = 0, nk_ = 0;
	bool have_la_ = false;          /* an LA came in the data phase */
	s16 rtx_count_ = 0;
	s16 dup_ignore_ = 0;            /* duplicate LAs from frames sent
					   before our retransmission */
	/* receiving: V(R), LTs not acknowledged, an LA due, V(R) - 1 seen
	   once, the DTE busy (no credit) */
	u8 vr_ = 1, vra_ = 1;
	s16 unacked_ = 0;
	bool la_due_ = false;
	bool dup_seen_ = false;
	bool busy_ = false;
	/* an LNA due, with this N(RA) */
	bool lna_due_ = false;
	u8 lna_nr_ = 0;
	/* received characters the DTE has not taken */
	DteBacklog<MNP_MAX_INFO> backlog_;
	/* the call's LTs: sent, sent again, received in sequence */
	s32 lt_sent_ = 0, lt_again_ = 0, lt_rcvd_ = 0;
	/* the control frame being sent, the LT frames */
	Frame ctrl_ = {};
	u8 ctrl_buf_[64];
	Slot slot_[MNP_MAX_K];
};

} // namespace dlink

#endif
