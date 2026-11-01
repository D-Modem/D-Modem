/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v150.cpp - V.150.1's modem relay: SSE, SPRT and the MoIP session
 *               (v150.h)
 */

#include "v150.h"

#include <algorithm>

namespace dmd {
namespace v150 {

/* see v150.h */
size_t sse_encode(const Sse &e, u8 *p)
{
	p[0] = static_cast<u8>((e.event & 0x3F) << 2);
	p[1] = e.ric;
	p[2] = static_cast<u8>(e.info >> 8);
	p[3] = static_cast<u8>(e.info);
	return 4;
}

/* see v150.h */
bool sse_decode(const u8 *p, size_t n, Sse *e)
{
	if (n < 4)
		return false;
	e->event = static_cast<u8>(p[0] >> 2);
	e->ric = p[1];
	e->info = static_cast<u16>(p[2] << 8 | p[3]);
	return true;
}

/* see v150.h */
Sprt::Sprt(u8 pt) : pt_(static_cast<u8>(pt & 0x7F)) {}

/* the window of reliable channel tc */
u16 Sprt::window(int tc) const
{
	return tc == 2 ? TC2_WINDOW : TC1_WINDOW;
}

/* a packet on tc with seq and payload pl, our BSN for tc and up to
   three of the acks owed */
std::vector<u8> Sprt::packet(int tc, u16 seq, const std::vector<u8> &pl)
{
	u16 bsn = 0;
	if (tc == 1 || tc == 2) {
		bsn = rx_[tc].user_next;
		tx_[tc].bsn_sent = bsn;
	}
	size_t noa = std::min<size_t>(acks_.size(), 3);
	std::vector<u8> p;
	p.reserve(6 + 2 * noa + pl.size());
	p.push_back(0);                 /* X 0, SSID 0 */
	p.push_back(pt_);
	p.push_back(static_cast<u8>(tc << 6 | (seq >> 8 & 0x3F)));
	p.push_back(static_cast<u8>(seq));
	p.push_back(static_cast<u8>(noa << 6 | (bsn >> 8 & 0x3F)));
	p.push_back(static_cast<u8>(bsn));
	for (size_t i = 0; i < noa; i++) {
		const Ack &a = acks_.front();
		p.push_back(static_cast<u8>(a.tcn << 6 | (a.sqn >> 8 & 0x3F)));
		p.push_back(static_cast<u8>(a.sqn));
		acks_.pop_front();
	}
	p.insert(p.end(), pl.begin(), pl.end());
	return p;
}

/* see v150.h */
bool Sprt::receive(const u8 *p, size_t n, u64 now)
{
	if (n < 6 || (p[1] & 0x7F) != pt_)
		return false;
	int tc = p[2] >> 6;
	u16 seq = static_cast<u16>((p[2] & 0x3F) << 8 | p[3]);
	size_t noa = p[4] >> 6;
	u16 bsn = static_cast<u16>((p[4] & 0x3F) << 8 | p[5]);
	if (n < 6 + 2 * noa)
		return false;
	for (size_t i = 0; i < noa; i++) {
		const u8 *a = p + 6 + 2 * i;
		int tcn = a[0] >> 6;
		u16 sqn = static_cast<u16>((a[0] & 0x3F) << 8 | a[1]);
		if (tcn == 1 || tcn == 2)
			tx_[tcn].unacked.erase(sqn);
	}
	const u8 *pl = p + 6 + 2 * noa;
	size_t len = n - 6 - 2 * noa;

	if (tc == 1 || tc == 2) {
		Tx &t = tx_[tc];
		/* (a BSN not behind the one we have) */
		if (((bsn - t.peer_bsn) & SEQ_MASK) <= window(tc))
			t.peer_bsn = bsn;
		if (!len)
			return true;
		acks_.push_back({ static_cast<u16>(tc), seq, now });
		Rx &r = rx_[tc];
		if (((seq - r.user_next) & SEQ_MASK) >= window(tc) ||
		    ((seq - r.expected) & SEQ_MASK) >= window(tc))
			return true;        /* (a copy, or beyond the window) */
		r.early.emplace(seq, std::vector<u8>(pl, pl + len));
		for (auto it = r.early.find(r.expected); it != r.early.end();
		     it = r.early.find(r.expected)) {
			r.ready.push_back(std::move(it->second));
			r.early.erase(it);
			r.expected = static_cast<u16>((r.expected + 1) &
						      SEQ_MASK);
		}
	} else if (tc == 3 && len) {
		if (tc3_any_ && (((seq - tc3_last_) & SEQ_MASK) == 0 ||
				 ((seq - tc3_last_) & SEQ_MASK) >= 0x2000))
			return true;        /* (older than the last) */
		tc3_any_ = true;
		tc3_last_ = seq;
		rx_[3].ready.emplace_back(pl, pl + len);
	}
	return true;
}

/* see v150.h */
bool Sprt::next(int *tc, std::vector<u8> *payload)
{
	for (int c : { 2, 1, 3 }) {
		Rx &r = rx_[c];
		if (r.ready.empty())
			continue;
		*tc = c;
		*payload = std::move(r.ready.front());
		r.ready.pop_front();
		if (c != 3)
			r.user_next = static_cast<u16>((r.user_next + 1) &
						       SEQ_MASK);
		return true;
	}
	return false;
}

/* see v150.h */
bool Sprt::can_send(int tc) const
{
	if (tc == 3)
		return true;
	if (tc != 1 && tc != 2)
		return false;
	const Tx &t = tx_[tc];
	return ((t.next - t.peer_bsn) & SEQ_MASK) < window(tc) &&
	       t.unacked.size() < window(tc);
}

/* see v150.h */
bool Sprt::send(int tc, const u8 *p, size_t n, u64 now)
{
	if (tc < 1 || tc > 3 || !n || n > (tc == 2 ? TC2_PAYLOAD :
					   TC1_PAYLOAD) || !can_send(tc))
		return false;
	Tx &t = tx_[tc];
	u16 seq = t.next;
	t.next = static_cast<u16>((t.next + 1) & SEQ_MASK);
	std::vector<u8> pl(p, p + n);
	out_.push_back(packet(tc, seq, pl));
	if (tc != 3)
		t.unacked[seq] = { std::move(pl), now };
	t.last = now;
	return true;
}

/* see v150.h */
void Sprt::poll(u64 now, std::vector<std::vector<u8>> *out)
{
	for (int tc : { 2, 1 }) {
		Tx &t = tx_[tc];
		for (auto &[seq, s] : t.unacked) {
			if (now - s.at < TR03)
				continue;
			out_.push_back(packet(tc, seq, s.payload));
			s.at = now;
			t.last = now;
			retransmits_++;
		}
	}
	for (auto &p : out_)
		out->push_back(std::move(p));
	out_.clear();
	while (acks_.size() >= 3 ||
	       (!acks_.empty() && now - acks_.front().at >= TA01))
		out->push_back(packet(0, 0, {}));
	/* (TA02: our BSN moved on and nothing carried it) */
	for (int tc : { 1, 2 }) {
		Tx &t = tx_[tc];
		u64 ta02 = tc == 1 ? 130 : 500;
		if (rx_[tc].user_next != t.bsn_sent && now - t.last >= ta02) {
			out->push_back(packet(tc, 0, {}));
			t.last = now;
		}
	}
}

/* see v150.h */
std::vector<u8> connect_encode(const Connect &c)
{
	return {
		MSG_CONNECT,
		static_cast<u8>((c.selmod & 0x3F) << 2 | (c.comp_dir & 3)),
		static_cast<u8>((c.comp & 0xF) << 4 | (c.ec & 0xF)),
		static_cast<u8>(c.tdsr >> 8), static_cast<u8>(c.tdsr),
		static_cast<u8>(c.rdsr >> 8), static_cast<u8>(c.rdsr),
		0, 0,                   /* no DLCI; no optional data types */
	};
}

/* see v150.h */
bool connect_decode(const u8 *p, size_t n, Connect *c)
{
	if (n < 7)
		return false;
	c->selmod = static_cast<u8>(p[1] >> 2);
	c->comp_dir = static_cast<u8>(p[1] & 3);
	c->comp = static_cast<u8>(p[2] >> 4);
	c->ec = static_cast<u8>(p[2] & 0xF);
	c->tdsr = static_cast<u16>(p[3] << 8 | p[4]);
	c->rdsr = static_cast<u16>(p[5] << 8 | p[6]);
	return true;
}

/* see v150.h */
bool choose(u16 mods, Selmod *sel, u16 *rate)
{
	static const struct {
		u16 mod;
		Selmod sel;
		u16 rate;
	} best[] = {
		{ MOD_V34, SEL_V34, 33600 }, { MOD_V32, SEL_V32BIS, 14400 },
		{ MOD_V22, SEL_V22BIS, 2400 }, { MOD_V21, SEL_V21, 300 },
	};
	for (const auto &b : best)
		if (mods & b.mod) {
			*sel = b.sel;
			*rate = b.rate;
			return true;
		}
	return false;
}

/* the rate a CONNECT's SELMOD has at its best, for a rate of 0 */
static u16 selmod_rate(u8 sel)
{
	switch (sel) {
	case SEL_V34: return 33600;
	case SEL_V32BIS: return 14400;
	case 6: return 9600;            /* V.32 */
	case SEL_V22BIS: return 2400;
	case 8: return 1200;            /* V.22 */
	case SEL_V21: return 300;
	case 1: case 2: case 3: return 33600;   /* V.92, V.91, V.90 */
	default: return 2400;
	}
}

/* see v150.h */
Session::Session(bool caller, u8 pt, int peer_mg, u16 mods)
	: caller_(caller), peer_mg_(peer_mg), mods_(mods), sprt_(pt)
{
}

/* see v150.h */
void Session::start(u64 now)
{
	now_ = now;
	if (started_)
		return;
	started_ = true;
	if (caller_ && phase_ == IDLE) {
		phase_ = CALLING;
		sse_q_.push_back({ SSE_MR, RIC_CM, mods_ });
	}
}

/* a control message m to TC2, in turn */
void Session::ctl(std::vector<u8> m)
{
	ctl_q_.push_back(std::move(m));
}

/* modem relay from now: INIT on TC2 */
void Session::relay_on(u64 now)
{
	now_ = now;
	phase_ = RELAY;
	ctl({ MSG_INIT, 0, 0 });
}

/* see v150.h */
void Session::sse_in(const Sse &e, u64 now)
{
	now_ = now;
	if (phase_ == OVER)
		return;
	if (e.event == SSE_MR) {
		if (relaying())
			return;         /* (its copies) */
		if (caller_) {
			if (phase_ == CALLING)
				relay_on(now);
			return;
		}
		peer_mods_ = e.ric == RIC_CM ? e.info : 0;
		sse_q_.push_back({ SSE_MR, RIC_P_PRIME, 0 });
		relay_on(now);
		return;
	}
	if (phase_ == IDLE)
		return;                 /* (audio as it was) */
	if (e.ric == RIC_CLEARDOWN)
		over("the far end cleared down (SSE)");
	else if (e.event == SSE_AUDIO)
		over("the far end went back to audio");
	else if (e.event == SSE_VBD)
		over("the far end chose voice band data, not modem relay");
	else
		over("the far end left modem relay");
}

/* see v150.h */
bool Session::sse_out(Sse *e)
{
	if (sse_q_.empty())
		return false;
	*e = sse_q_.front();
	sse_q_.pop_front();
	return true;
}

/* see v150.h */
void Session::packet_in(const u8 *p, size_t n, u64 now)
{
	now_ = now;
	if (phase_ == OVER)
		return;
	if (!sprt_.receive(p, n, now))
		return;
	/* (SPRT before the far end's SSE came: it is relaying) */
	if (phase_ == IDLE || phase_ == CALLING)
		relay_on(now);
	pull(now);
}

/* the far end's payloads in order, while the octets held from it are
   fewer than RX_HOLD */
void Session::pull(u64 now)
{
	int tc;
	std::vector<u8> m;
	while (phase_ != OVER && rx_.size() < RX_HOLD && sprt_.next(&tc, &m))
		message(m, now);
}

/* a MoIP message m from the far end */
void Session::message(const std::vector<u8> &m, u64 now)
{
	if (m.empty())
		return;
	switch (m[0] & 0x7F) {
	case MSG_I_OCTET:
		rx_.insert(rx_.end(), m.begin() + 1, m.end());
		return;
	case MSG_I_CHAR_STAT:
	case MSG_I_CHAR_DYN:
		if (m.size() > 2)
			rx_.insert(rx_.end(), m.begin() + 2, m.end());
		return;
	case MSG_I_RAW_OCTET:
		/* segments: 0|L and L+1 octets; 1|L, N and L+1 octets
		   N+2 times */
		for (size_t i = 1; i < m.size();) {
			bool rep = m[i] & 0x80;
			size_t l = (m[i] & 0x7F) + 1u, times = 1;
			i++;
			if (rep) {
				if (i >= m.size())
					return;
				times = m[i++] + 2u;
			}
			if (i + l > m.size())
				return;
			for (size_t k = 0; k < times; k++)
				rx_.insert(rx_.end(), m.begin() + i,
					   m.begin() + i + l);
			i += l;
		}
		return;
	default:
		control(m, now);
	}
}

/* our CONNECT: the answerer's at the best modulation the caller's CM
   and we have; the caller's at the answerer's, its rates the other way
   round */
void Session::send_connect(u64 now)
{
	now_ = now;
	if (connect_sent_)
		return;
	ours_ = Connect();
	ours_.ec = 1;                   /* V.42/LAPM */
	if (caller_) {
		ours_.selmod = peer_.selmod;
		ours_.tdsr = peer_.rdsr;
		ours_.rdsr = peer_.tdsr;
	} else {
		Selmod sel;
		u16 rate;
		u16 m = peer_mods_ ? static_cast<u16>(peer_mods_ & mods_) :
				     mods_;
		if (!choose(m, &sel, &rate)) {
			ctl({ MSG_CLEARDOWN, 4, 0, 0 });
			over("no modulation in common with the far end");
			return;
		}
		ours_.selmod = sel;
		ours_.tdsr = ours_.rdsr = rate;
		/* JM_INFO: data, the modulation, LAPM */
		u16 jm = static_cast<u16>((static_cast<u16>(m) >> 3) & 0xFFF);
		u16 cats[] = { 0x8600, static_cast<u16>(0xA000 | jm), 0x5800 };
		std::vector<u8> j = { MSG_JM_INFO };
		for (u16 c : cats) {
			j.push_back(static_cast<u8>(c >> 8));
			j.push_back(static_cast<u8>(c));
		}
		ctl(j);
	}
	ctl(connect_encode(ours_));
	connect_sent_ = true;
}

/* a control message m (TC2's, or another channel's that is not data) */
void Session::control(const std::vector<u8> &m, u64 now)
{
	switch (m[0] & 0x7F) {
	case MSG_INIT:
		init_in_ = true;
		if (!caller_)
			send_connect(now);
		return;
	case MSG_CONNECT:
		if (connect_in_ || !connect_decode(m.data(), m.size(), &peer_))
			return;
		connect_in_ = true;
		if (peer_.comp && peer_mg_ != 2) {
			ctl({ MSG_CLEARDOWN, 3, 0, 0 });
			over("the far end's modem compresses, and its "
			     "gateway does not undo it (not D-TCX)");
			return;
		}
		if (caller_)
			send_connect(now);
		if (phase_ == RELAY)
			phase_ = UP;
		return;
	case MSG_CLEARDOWN:
		over("the far end cleared down");
		return;
	case MSG_BREAK:
		ctl({ MSG_BREAKACK });
		return;
	default:
		/* (JM_INFO, XID_XCHG, MR_EVENT, PROF_XCHG, START_JM: as
		   an endpoint we have nothing to do with them) */
		return;
	}
}

/* see v150.h */
void Session::poll(u64 now, std::vector<std::vector<u8>> *out)
{
	now_ = now;
	while (!ctl_q_.empty() && sprt_.can_send(2)) {
		const std::vector<u8> &m = ctl_q_.front();
		sprt_.send(2, m.data(), m.size(), now);
		ctl_q_.pop_front();
	}
	pull(now);
	sprt_.poll(now, out);
}

/* see v150.h */
int Session::state(s32 *tx, s32 *rx) const
{
	if (phase_ == OVER)
		return -1;
	if (phase_ != UP)
		return 0;
	*tx = peer_.tdsr ? peer_.tdsr : selmod_rate(peer_.selmod);
	*rx = peer_.rdsr ? peer_.rdsr : selmod_rate(peer_.selmod);
	return 1;
}

/* see v150.h */
size_t Session::read(u8 *buf, size_t max)
{
	size_t n = std::min(max, rx_.size());
	std::copy_n(rx_.begin(), n, buf);
	rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(n));
	pull(now_);
	return n;
}

/* see v150.h */
size_t Session::write(const u8 *buf, size_t n, u64 now)
{
	now_ = now;
	size_t done = 0;
	if (phase_ != UP)
		return 0;
	while (done < n && sprt_.can_send(1)) {
		size_t k = std::min(n - done, Sprt::TC1_PAYLOAD - 1);
		std::vector<u8> m = { MSG_I_OCTET };
		m.insert(m.end(), buf + done, buf + done + k);
		sprt_.send(1, m.data(), m.size(), now);
		done += k;
	}
	return done;
}

/* see v150.h */
void Session::hangup(u64 now)
{
	now_ = now;
	if (relaying())
		ctl({ MSG_CLEARDOWN, 5, 0, 0 });
	over("we hung up");
}

/* the session over, why */
void Session::over(const char *why)
{
	if (phase_ != OVER)
		why_ = why;
	phase_ = OVER;
}

} // namespace v150
} // namespace dmd
