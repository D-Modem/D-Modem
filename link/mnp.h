/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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
 */

#ifndef DMODEM_LINK_MNP_H
#define DMODEM_LINK_MNP_H

#include <cstdint>

#include "hdlc.h"

namespace dlink {

enum {
	MNP_MAX_K = 8,          /* the LT frames outstanding we offer */
	MNP_MAX_INFO = 256,     /* the largest information field (N401) */
	MNP_DFLT_INFO = 64,     /* without parameter 4 */
};

struct Config;
class Mnp;

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
	int tx_idle(int nbits, uint8_t mask, uint8_t *&buf, int &n);
	void rx_scan();
	void rx_char(uint8_t ch);
	void tx_char(uint8_t ch);
	void rx_restart(Rx state);

	Mnp *mnp_ = nullptr;
	Frame *tx_frame_ = nullptr;
	Tx tx_state_ = Tx::Idle;
	int tx_step_ = 0;               /* within Start and End */
	bool tx_dle_ = false;           /* a DLE to double */
	uint16_t tx_fcs_ = 0;
	Rx rx_state_ = Rx::Hunt;
	Frame rx_frame_ = {};
	uint8_t rx_buf_[3 + 300];
	uint16_t rx_fcs_ = 0;
	uint8_t rx_fcs_lo_ = 0;
};

/* the procedure: establishment as initiator (the originator) or
   responder, the data phase, LD */
class Mnp : public Framer {
public:
	/* a call: what we offer (mnp_mode, mnp_k, mnp_info, mnp5 of cfg) */
	void init(Link *link, const Config &cfg);
	/* the end of the call */
	void exit();
	/* the establishment: the initiator sends its LR after some marks,
	   the responder waits for one */
	void start(bool initiator);
	/* we hang up: LD, then released once it is out */
	void disconnect();
	/* characters the DTE did not take: try again */
	void push_rest();
	/* LT frames not sent yet or not acknowledged */
	bool tx_pending() const;
	/* the timers: bits more have gone out */
	void clock(int32_t bits);
	/* the data phase's frames as HDLC frames (framing mode 3) */
	bool bit_tx() const { return bit_tx_; }
	/* (see bit_tx()) */
	bool bit_rx() const { return mode_ == 3 && phase_ != Phase::Idle &&
					      phase_ != Phase::Init; }

	/* what the LRs agreed (the framing mode, k, N401, the data phase
	   optimization, MNP 5) */
	int mode() const { return mode_; }
	/* (see mode()) */
	int k() const { return k_; }
	/* (see mode()) */
	int n401() const { return n401_; }
	/* (see mode()) */
	int optimization() const { return opt_; }
	/* (see mode()) */
	bool mnp5() const { return mnp5_; }

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
		uint8_t buf[5 + MNP_MAX_INFO];
	};

	Frame *next_frame();
	Frame *ctrl(int len);
	Frame *build_lr();
	Frame *build_la();
	Frame *build_ld();
	Frame *build_lna();
	Frame *next_lt();
	void rx_frame(Frame *f, bool bit);
	bool rx_lr(const uint8_t *h, int len);
	void rx_lt(const uint8_t *h, int len, const uint8_t *info, int n);
	void rx_la(const uint8_t *h, int len);
	void rx_ln(const uint8_t *h, int len);
	void rx_ld(const uint8_t *h, int len);
	void data_phase();
	void fail(int reason);
	void fallback();
	void t401_expired();
	bool can_send(uint8_t ns) const;
	int32_t t401_est() const;
	int32_t t401_data() const;
	int32_t rate() const;

	Link *link_ = nullptr;
	Phase phase_ = Phase::Idle;
	bool initiator_ = false;
	bool bit_tx_ = false;
	/* our offer, then what was agreed */
	int our_mode_ = 3, our_k_ = MNP_MAX_K, our_n401_ = MNP_MAX_INFO;
	int our_opt_ = 3;
	bool our_mnp5_ = false;
	int mode_ = 2, k_ = 1, n401_ = MNP_DFLT_INFO, opt_ = 0;
	bool mnp5_ = false;
	int lr_n401_ = MNP_DFLT_INFO;   /* N401 agreed (before the 256-octet
					   optimization) */
	/* the parameters the initiator's LR had (the responder echoes
	   those it understood) */
	bool lr_opt_ = false;
	/* establishment */
	int tries_ = 0;
	bool heard_ = false;            /* a frame came, good or bad */
	int ld_reason_ = 0;
	/* control frames to send when the framer asks */
	bool lr_due_ = false, ld_due_ = false;
	/* timers: bits left, 0 when stopped */
	int32_t t_start_ = 0, t401_ = 0, t402_ = 0, t404_ = 0;
	/* sending: V(S) the next new N(S), the oldest not acknowledged,
	   the next to (re)send; the remote's last N(R) and N(k) */
	uint8_t vs_ = 1, una_ = 1, nxt_ = 1;
	uint8_t nr_ = 0, nk_ = 0;
	bool have_la_ = false;          /* an LA came in the data phase */
	int rtx_count_ = 0;
	int dup_ignore_ = 0;            /* duplicate LAs from frames sent
					   before our retransmission */
	/* receiving: V(R), LTs not acknowledged, an LA due, V(R) - 1 seen
	   once, the DTE busy (no credit) */
	uint8_t vr_ = 1, vra_ = 1;
	int unacked_ = 0;
	bool la_due_ = false;
	bool dup_seen_ = false;
	bool busy_ = false;
	/* an LNA due, with this N(RA) */
	bool lna_due_ = false;
	uint8_t lna_nr_ = 0;
	/* received characters the DTE has not taken */
	int rx_count_ = 0, rx_head_ = 0;
	uint8_t rx_buf_[MNP_MAX_INFO];
	/* the call's LTs: sent, sent again, received in sequence */
	int32_t lt_sent_ = 0, lt_again_ = 0, lt_rcvd_ = 0;
	/* the control frame being sent, the LT frames */
	Frame ctrl_ = {};
	uint8_t ctrl_buf_[64];
	Slot slot_[MNP_MAX_K];
};

} // namespace dlink

#endif
