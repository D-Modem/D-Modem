/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v150.h - V.150.1's modem relay (MoIP) as a SIP line's far end sees
 *             it: the modem is its own gateway, the far end a gateway
 *             (or another such modem) that terminates the modulation of
 *             its own modem
 *
 *    Sse        Annex C's state signalling events (RTP "v150fw"): the
 *               switch to modem relay (event 3) with its reason (RIC)
 *    Sprt       Annex B's simple packet relay transport over UDP: four
 *               transport channels, TC1 (data) and TC2 (control)
 *               reliable and sequenced, with acknowledgements, windows
 *               (the flow control) and retransmission
 *    Session    the MoIP messages of clause 15.4 over Sprt: INIT, the
 *               answerer's JM_INFO, CONNECT both ways, the data as
 *               I_OCTET (octets without formatting, the default of an
 *               error-controlled call) and CLEARDOWN
 *
 *    The calls go as Figure 21 has them, the modem in the place of the
 *    gateway and its modem both.  The caller (G1) sends SSE:M with the
 *    reason CM and the modulations it has (once its call progress heard
 *    the answer tone); the answerer (G2) sends ANSam as RFC 2833 events
 *    until then, and SSE:M (p') back.  Both then send INIT on TC2; the
 *    answerer JM_INFO (the modulation it chose from the caller's) and
 *    CONNECT; the caller CONNECT back, at the answerer's rates.  The
 *    relay is up at the other's CONNECT: its rates are the far modem's
 *    with its gateway.  Each declares V.42/LAPM and no compression (a
 *    modem relay endpoint has no PSTN link of its own), so the data
 *    goes as I_OCTET both ways.  We are a double trans-compression
 *    gateway (D-TCX) with nothing to compress with on IP, so a D-TCX far
 *    end sends its octets plain whatever its modem compresses; one that
 *    is not (N-TCX, S-TCX) and whose modem compresses would send them
 *    compressed for ours to undo, and is cleared down (reason 3).
 *    I_RAW-OCTET and I_CHAR-STAT/DYN are taken too.
 *
 *    Everything here is plain data: the caller's (sip.cpp's) clock in
 *    ms, packets in and out as octets.
 */
#ifndef DMODEMD_V150_H
#define DMODEMD_V150_H

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "dm_types.h"

namespace dmd {
namespace v150 {

/* Annex C's events and Table 12's reasons that a modem relay uses */
enum SseEvent : u8 {
	SSE_AUDIO = 1, SSE_VBD = 2, SSE_MR = 3, SSE_FR = 4, SSE_TR = 5,
};
enum Ric : u8 {
	RIC_NULL = 0, RIC_CM = 1, RIC_JM = 2, RIC_TIMEOUT = 18,
	RIC_P_PRIME = 19, RIC_CLEARDOWN = 20, RIC_ANSAM = 22,
};

/* Table 13's modulations (the CM and JM bits of an SSE's information,
   b16 the most significant) */
enum Mods : u16 {
	MOD_PCM = 0x8000, MOD_V34 = 0x4000, MOD_V34HD = 0x2000,
	MOD_V32 = 0x1000, MOD_V22 = 0x0800, MOD_V17 = 0x0400,
	MOD_V29HD = 0x0200, MOD_V27TER = 0x0100, MOD_V26TER = 0x0080,
	MOD_V26BIS = 0x0040, MOD_V23 = 0x0020, MOD_V23HD = 0x0010,
	MOD_V21 = 0x0008, MOD_V90A = 0x0004, MOD_V90D = 0x0002,
	MOD_V91 = 0x0001,
};
/* ours: what the relay carries is the modem's own octets, at any rate
   the far modem trains to; these four are the ones SDP's mrmods names
   that a data call (not fax) of a modem without PCM has */
constexpr u16 OUR_MODS = MOD_V34 | MOD_V32 | MOD_V22 | MOD_V21;

/* an SSE payload (no extension) */
struct Sse {
	u8 event = 0;
	u8 ric = 0;
	u16 info = 0;
};
/* e into p (4 octets): 4 */
size_t sse_encode(const Sse &e, u8 *p);
/* p (n octets) into *e: false when it is short */
bool sse_decode(const u8 *p, size_t n, Sse *e);

/* SPRT: one end of the transport, on one UDP socket */
class Sprt {
public:
	/* Table B.2's defaults, Table B.3's timers */
	static constexpr size_t TC1_PAYLOAD = 132, TC2_PAYLOAD = 132;
	static constexpr u16 TC1_WINDOW = 32, TC2_WINDOW = 8;
	static constexpr u64 TA01 = 90, TR03 = 500;
	static constexpr u16 SEQ_MASK = 0x3FFF;

	/* its payload type pt (the SDP's sprtmap) */
	explicit Sprt(u8 pt);

	/* a packet from the far end, at now (ms): false when it is not
	   SPRT's (too short, or another payload type) */
	bool receive(const u8 *p, size_t n, u64 now);
	/* the next payload in order for the user (TC2's first), its
	   channel in *tc: false for none */
	bool next(int *tc, std::vector<u8> *payload);
	/* a payload on tc (1, 2 or 3) at now: false when the window is
	   full or it is too long */
	bool send(int tc, const u8 *p, size_t n, u64 now);
	/* tc has room in its window */
	bool can_send(int tc) const;
	/* the packets due at now (new ones, retransmissions, acks), into
	   *out */
	void poll(u64 now, std::vector<std::vector<u8>> *out);
	/* packets sent again so far */
	unsigned retransmits() const { return retransmits_; }

private:
	struct Sent {
		std::vector<u8> payload;
		u64 at;
	};
	struct Tx {
		u16 next = 0;           /* the next SEQUENCE NUMBER */
		u16 peer_bsn = 0;       /* the far end's BASE SEQUENCE NUMBER */
		std::map<u16, Sent> unacked;
		u64 last = 0;           /* a packet sent on it last */
		u16 bsn_sent = 0;       /* our BSN as it last went */
	};
	struct Rx {
		u16 expected = 0;       /* the next in order */
		u16 user_next = 0;      /* our BSN: the user's next */
		std::map<u16, std::vector<u8>> early;
		std::deque<std::vector<u8>> ready;
	};
	struct Ack {
		u16 tcn, sqn;
		u64 at;
	};
	std::vector<u8> packet(int tc, u16 seq, const std::vector<u8> &pl);
	u16 window(int tc) const;

	u8 pt_;
	Tx tx_[4];
	Rx rx_[4];
	bool tc3_any_ = false;          /* TC3's last sequence number set */
	u16 tc3_last_ = 0;
	std::deque<Ack> acks_;
	std::deque<std::vector<u8>> out_;
	unsigned retransmits_ = 0;
};

/* MoIP's messages (Table 16) */
enum Msg : u8 {
	MSG_INIT = 1, MSG_XID_XCHG = 2, MSG_JM_INFO = 3, MSG_START_JM = 4,
	MSG_CONNECT = 5, MSG_BREAK = 6, MSG_BREAKACK = 7, MSG_MR_EVENT = 8,
	MSG_CLEARDOWN = 9, MSG_PROF_XCHG = 10, MSG_I_RAW_OCTET = 16,
	MSG_I_RAW_BIT = 17, MSG_I_OCTET = 18, MSG_I_CHAR_STAT = 19,
	MSG_I_CHAR_DYN = 20, MSG_I_FRAME = 21,
};
/* CONNECT's SELMOD (Table 25) for the modulations above, and the rate
   each has at its best */
enum Selmod : u8 {
	SEL_V34 = 4, SEL_V32BIS = 5, SEL_V22BIS = 7, SEL_V21 = 15,
};

/* a CONNECT's fields (Table 25) */
struct Connect {
	u8 selmod = 0;
	u8 comp_dir = 0, comp = 0, ec = 0;
	u16 tdsr = 0, rdsr = 0;
};
/* c as a CONNECT message (its header octet first) */
std::vector<u8> connect_encode(const Connect &c);
/* a CONNECT message p (n octets) into *c: false when it is short */
bool connect_decode(const u8 *p, size_t n, Connect *c);
/* the best modulation of mods (Table 13's bits) we have, its SELMOD
   and rate; false when there is none */
bool choose(u16 mods, Selmod *sel, u16 *rate);

/* a modem relay session, the caller (G1) or the answerer (G2) */
class Session {
public:
	enum Phase { IDLE, CALLING, RELAY, UP, OVER };

	/* SPRT's payload type pt; peer_mg: the far end's SDP mg (0 N-TCX,
	   1 S-TCX, 2 D-TCX); mods: the modulations we offer */
	Session(bool caller, u8 pt, int peer_mg, u16 mods = OUR_MODS);

	/* the modem's relay pump has started at now: the caller's SSE:M
	   (CM) goes, the answerer's answer tone starts */
	void start(u64 now);
	/* the far end's SSE at now */
	void sse_in(const Sse &e, u64 now);
	/* an SSE to send (each three times, sip.cpp's), false for none */
	bool sse_out(Sse *e);
	/* the answerer sends ANSam (RFC 2833's event 34) now */
	bool answer_tone() const { return !caller_ && phase_ == IDLE &&
					  started_; }
	/* SPRT's packets: the far end's, ours due at now */
	void packet_in(const u8 *p, size_t n, u64 now);
	void poll(u64 now, std::vector<std::vector<u8>> *out);
	/* modem relay is on: the RTP audio stops, SPRT goes */
	bool relaying() const { return phase_ == RELAY || phase_ == UP; }

	/* struct dp_relay's: 0 not up yet, 1 up (the rates each way into
	   *tx, *rx), -1 over */
	int state(s32 *tx, s32 *rx) const;
	/* the far end's octets, up to max into buf: how many */
	size_t read(u8 *buf, size_t max);
	/* n octets to the far end at now: how many it took */
	size_t write(const u8 *buf, size_t n, u64 now);
	/* our end hangs up: CLEARDOWN (on-hook) when the relay is on */
	void hangup(u64 now);
	/* where it is */
	Phase phase() const { return phase_; }
	/* why it is over, for the log */
	const std::string &why() const { return why_; }
	/* the SPRT end (tests) */
	const Sprt &sprt() const { return sprt_; }

	/* octets held from the far end before read() stops taking SPRT's
	   payloads (so its window fills: the flow control) */
	static constexpr size_t RX_HOLD = 1024;

private:
	void relay_on(u64 now);
	void message(const std::vector<u8> &m, u64 now);
	void control(const std::vector<u8> &m, u64 now);
	void send_connect(u64 now);
	void over(const char *why);
	void pull(u64 now);
	void ctl(std::vector<u8> m);

	bool caller_;
	int peer_mg_;
	u16 mods_;
	u64 now_ = 0;
	Sprt sprt_;
	Phase phase_ = IDLE;
	bool started_ = false;
	bool init_in_ = false, connect_sent_ = false, connect_in_ = false;
	u16 peer_mods_ = 0;             /* the caller's CM (the answerer's) */
	Connect peer_, ours_;
	std::deque<Sse> sse_q_;
	std::deque<std::vector<u8>> ctl_q_;   /* TC2's, waiting for room */
	std::deque<u8> rx_;
	std::string why_;
};

} // namespace v150
} // namespace dmd

#endif
