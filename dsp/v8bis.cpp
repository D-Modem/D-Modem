/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v8bis.cpp - V.8 bis's messages, signals and their line coding (see
 *                v8bis.h)
 *
 */

#include <algorithm>

#include "v8bis.h"

namespace dsp {
namespace v8bis {

/*
 *    the parameter trees (8.2)
 */

/* a block of octets of bits data bits each (7 or 6), delimiting bit
   delim (0x80 or 0x40) in the last; extra (0x80) also in the last; an
   empty block as one octet with no parameters */
static void put_block(const std::vector<u8> &b, u8 bits, u8 delim, u8 extra,
		      std::vector<u8> *out)
{
	u8 mask = static_cast<u8>((1u << bits) - 1);
	size_t n = b.empty() ? 1 : b.size();
	for (size_t i = 0; i < n; i++) {
		u8 o = b.empty() ? 0 : static_cast<u8>(b[i] & mask);
		if (i == n - 1)
			o |= delim | extra;
		out->push_back(o);
	}
}

/* see v8bis.h */
void tree_encode(const Tree &t, std::vector<u8> *out)
{
	put_block(t.npar1, 7, 0x80, 0, out);
	put_block(t.spar1, 7, 0x80, 0, out);
	for (const Tree::Par2 &p : t.par2) {
		bool spars = !p.spar2.empty();
		/* NPar(2) alone: bits 7 and 8 in its last octet */
		put_block(p.npar2, 6, 0x40, spars ? 0 : 0x80, out);
		if (!spars)
			continue;
		put_block(p.spar2, 6, 0x40, p.npar3.empty() ? 0x80 : 0, out);
		for (size_t k = 0; k < p.npar3.size(); k++)
			put_block(p.npar3[k], 6, 0x40,
				  k == p.npar3.size() - 1 ? 0x80 : 0, out);
	}
}

/* a block delimited by delim from p (n octets) at *pos into *b, its
   octets' bits data bits; the last octet's other bits in *last: false
   when the octets end first */
static bool get_block(const u8 *p, size_t n, size_t *pos, u8 bits, u8 delim,
		      std::vector<u8> *b, u8 *last)
{
	u8 mask = static_cast<u8>((1u << bits) - 1);
	b->clear();
	while (*pos < n) {
		u8 o = p[(*pos)++];
		b->push_back(o & mask);
		if (o & delim) {
			*last = o;
			return true;
		}
	}
	return false;
}

/* the bits set in a block's octets (bits data bits each) */
static size_t set_bits(const std::vector<u8> &b)
{
	size_t k = 0;
	for (u8 o : b)
		for (; o; o &= static_cast<u8>(o - 1))
			k++;
	return k;
}

/* see v8bis.h */
bool tree_decode(const u8 *p, size_t n, size_t *pos, Tree *t)
{
	u8 last;
	*t = Tree();
	if (!get_block(p, n, pos, 7, 0x80, &t->npar1, &last) ||
	    !get_block(p, n, pos, 7, 0x80, &t->spar1, &last))
		return false;
	size_t blocks = set_bits(t->spar1);
	for (size_t i = 0; i < blocks; i++) {
		Tree::Par2 q;
		if (!get_block(p, n, pos, 6, 0x40, &q.npar2, &last))
			return false;
		if (!(last & 0x80)) {
			if (!get_block(p, n, pos, 6, 0x40, &q.spar2, &last))
				return false;
			size_t m = (last & 0x80) ? 0 : set_bits(q.spar2);
			for (size_t k = 0; k < m; k++) {
				std::vector<u8> b;
				if (!get_block(p, n, pos, 6, 0x40, &b, &last))
					return false;
				q.npar3.push_back(b);
				if (last & 0x80)
					break;
			}
		}
		t->par2.push_back(q);
	}
	return true;
}

/*
 *    the messages
 */

/* ACK and NAK: the type and revision octet alone (8.3.3, 8.4.1) */
static bool has_fields(u8 type)
{
	return type == MS || type == CL || type == CLR;
}

/* see v8bis.h */
std::vector<u8> msg_encode(const Msg &m)
{
	std::vector<u8> out;
	out.push_back(static_cast<u8>((m.type & 0x0F) | (m.rev & 0x0F) << 4));
	if (has_fields(m.type)) {
		tree_encode(m.id, &out);
		tree_encode(m.s, &out);
	}
	return out;
}

/* see v8bis.h */
bool msg_decode(const u8 *p, size_t n, Msg *m)
{
	static const u8 known[] = { MS, CL, CLR, ACK1, ACK2, NAK1, NAK2,
				    NAK3, NAK4 };
	size_t pos = 1;
	*m = Msg();
	if (!n)
		return false;
	m->type = p[0] & 0x0F;
	m->rev = p[0] >> 4;
	if (std::find(std::begin(known), std::end(known), m->type) ==
	    std::end(known))
		return false;
	if (!has_fields(m->type))
		return true;
	return tree_decode(p, n, &pos, &m->id) &&
		tree_decode(p, n, &pos, &m->s);
}

/* see v8bis.h */
const char *type_name(u8 type)
{
	switch (type) {
	case MS: return "MS";
	case CL: return "CL";
	case CLR: return "CLR";
	case ACK1: return "ACK(1)";
	case ACK2: return "ACK(2)";
	case NAK1: return "NAK(1)";
	case NAK2: return "NAK(2)";
	case NAK3: return "NAK(3)";
	case NAK4: return "NAK(4)";
	}
	return "?";
}

/* v's bits as octets of six, as few as hold the highest set (one at
   least) */
static std::vector<u8> six_bits(u32 v)
{
	std::vector<u8> b;
	do {
		b.push_back(static_cast<u8>(v & 0x3F));
		v >>= 6;
	} while (v);
	return b;
}

/* (see six_bits()) back */
static u32 of_six_bits(const std::vector<u8> &b)
{
	u32 v = 0;
	for (size_t i = 0; i < b.size() && i < 5; i++)
		v |= static_cast<u32>(b[i] & 0x3F) << (6 * i);
	return v;
}

/* see v8bis.h */
Msg msg_of(u8 type, const Caps &c)
{
	Msg m;
	m.type = type;
	if (!has_fields(type))
		return m;
	m.id.npar1.push_back(c.id & 0x7F);
	m.id.spar1.push_back(0);        /* the analogue PSTN */
	m.s.npar1.push_back(0);
	m.s.spar1.push_back(c.modes & 0x7F);
	/* a level 2 block for each mode, in their bits' order */
	for (u8 bit = 1; bit < 0x80; bit = static_cast<u8>(bit << 1)) {
		if (!(c.modes & bit))
			continue;
		Tree::Par2 p;
		if (bit == MODE_DATA)
			p.npar2 = six_bits(c.data);
		else if (bit == MODE_SVD)
			p.npar2 = six_bits(c.svd);
		else
			p.npar2.push_back(0);
		m.s.par2.push_back(p);
	}
	return m;
}

/* see v8bis.h */
Caps caps_of(const Msg &m)
{
	Caps c;
	if (!m.id.npar1.empty())
		c.id = m.id.npar1[0];
	if (m.s.spar1.empty())
		return c;
	c.modes = m.s.spar1[0];
	size_t k = 0;
	for (u8 bit = 1; bit < 0x80; bit = static_cast<u8>(bit << 1)) {
		if (!(c.modes & bit))
			continue;
		if (k >= m.s.par2.size())
			break;
		const Tree::Par2 &p = m.s.par2[k++];
		if (bit == MODE_DATA)
			c.data = of_six_bits(p.npar2);
		else if (bit == MODE_SVD)
			c.svd = of_six_bits(p.npar2);
	}
	return c;
}

/*
 *    the signals (7.1)
 */

/* see v8bis.h */
const char *sig_name(u8 s)
{
	static const char *const names[] = { "none", "MRe", "MRd", "CRe",
					     "CRd", "ESi", "ESr" };
	return s < std::size(names) ? names[s] : "?";
}

/* segment 2's tones (Table 2), by signal */
static const float seg2_hz[] = { 0, 650, 1150, 400, 1900, 980, 1650 };
/* the detector's tones: the two pairs of segment 1 (Table 1), then
   segment 2's */
static const float rx_hz[SigRx::NG] = { 1375, 2002, 1529, 2225,
					650, 1150, 400, 1900, 980, 1650 };

/* see v8bis.h */
void SigTx::init(u8 s, bool initiating, float amp_, s32 srate)
{
	*this = SigTx();
	a.set_freq(initiating ? 1375 : 1529, srate);
	b.set_freq(initiating ? 2002 : 2225, srate);
	c.set_freq(s < std::size(seg2_hz) ? seg2_hz[s] : 0, srate);
	amp = amp_;
	seg1 = ms2samples(400, srate);
	seg2 = s == SIG_ESI || s == SIG_ESR ? 0 : ms2samples(100, srate);
}

/* see v8bis.h */
float SigTx::next()
{
	if (done())
		return 0;
	float y = n < seg1 ? amp * 0.7071f * (a.next() + b.next()) :
		amp * c.next();
	n++;
	return y;
}

/* see v8bis.h */
void SigRx::init(s32 srate, s16 pairs_)
{
	*this = SigRx();
	len = static_cast<s16>(srate / 100);
	pairs = pairs_;
	for (s16 i = 0; i < NG; i++)
		g[i].init(rx_hz[i], len, srate);
}

/* see v8bis.h */
u8 SigRx::run(float x)
{
	s16 done = 0;
	for (s16 i = 0; i < NG; i++)
		done = g[i].run(x);
	/* the block's power alongside, from the same samples */
	energy += x * x;
	if (!done)
		return SIG_NONE;
	u8 s = block(energy / len);
	energy = 0;
	return s;
}

/* one 10 ms block: segment 1 on (both tones of a pair the most of the
   block (bp its mean square), neither weak), or segment 2's tone after
   it */
u8 SigRx::block(float bp)
{
	const float min = tone_power(150);
	s16 on = 0;
	for (s16 k = 0; k < 2; k++) {
		if (!(pairs & (k ? PAIR_RESP : PAIR_INIT)))
			continue;
		float p1 = g[2 * k].power, p2 = g[2 * k + 1].power;
		if (p1 + p2 > 0.7f * bp && std::min(p1, p2) > 0.15f * bp &&
		    p1 + p2 > min) {
			on = static_cast<s16>(k + 1);
			break;
		}
	}
	if (on) {
		if (on != pair)
			seg1 = 0;
		pair = on;
		seg1++;
		after = 0;
		seg2 = 0;
		return SIG_NONE;
	}
	if (!seg1_on()) {
		seg1 = 0;
		pair = 0;
		return SIG_NONE;
	}
	/* segment 2: one tone the most of the block */
	s16 t = 4;
	for (s16 i = 5; i < NG; i++)
		if (g[i].power > g[t].power)
			t = i;
	if (g[t].power > 0.7f * bp && g[t].power > min) {
		seg2 = t == tone ? static_cast<s16>(seg2 + 1) : 1;
		tone = t;
	} else {
		seg2 = 0;
	}
	if (seg2 >= 4) {
		static const u8 init_sig[] = { SIG_MRE, SIG_MRD, SIG_CRE,
					       SIG_CRD, SIG_ESI, SIG_NONE };
		static const u8 resp_sig[] = { SIG_NONE, SIG_MRD, SIG_NONE,
					       SIG_CRD, SIG_NONE, SIG_ESR };
		u8 s = (pair == 1 ? init_sig : resp_sig)[tone - 4];
		seg1 = 0;
		pair = 0;
		seg2 = 0;
		return s;
	}
	/* segment 2 starts within 80 ms of segment 1's end */
	if (++after > 8) {
		seg1 = 0;
		pair = 0;
		seg2 = 0;
	}
	return SIG_NONE;
}

/*
 *    the messages on the line (7.2)
 */

/* see v8bis.h */
void MsgTx::init(bool high_, float amp_, s32 srate_)
{
	q.clear();
	at = 0;
	preamble = 0;
	framed = false;
	amp = amp_;
	srate = srate_;
	high = high_;
}

/* see v8bis.h */
void MsgTx::send(const std::vector<u8> &m)
{
	bool was_idle = idle();
	q.push_back(m);
	if (was_idle)
		start();
}

/* the message at at: its preamble, then its flags and frame */
void MsgTx::start()
{
	/* 100 ms of mark: 30 bits */
	preamble = 30;
	framed = false;
	hdlc_tx_start(24, 16);
	fsk.init(high ? 1650 : 980, high ? 1850 : 1180, amp, 300, srate, 1);
}

/* see v8bis.h */
s16 MsgTx::hdlc_tx_next(HdlcV8bis, unsigned char *buf, s16 max)
{
	if (framed || at >= q.size())
		return -1;
	const std::vector<u8> &m = q[at];
	s16 n = static_cast<s16>(std::min<size_t>(m.size(),
						  static_cast<size_t>(max)));
	std::copy_n(m.begin(), n, buf);
	framed = true;
	return n;
}

/* see v8bis.h */
s16 MsgTx::bit()
{
	if (preamble > 0) {
		preamble--;
		return 1;
	}
	s16 b = hdlc_tx_bit();
	return b == FAX_BIT_END ? -1 : b;
}

/* see v8bis.h */
float MsgTx::next()
{
	if (idle())
		return 0;
	float y = fsk.sample([this] { return bit(); });
	if (fsk.ended) {
		/* the next message straight after (CL-MS) */
		if (++at < q.size()) {
			start();
		} else {
			q.clear();
			at = 0;
		}
	}
	return y;
}

/* see v8bis.h */
void MsgRx::init(bool high, s32 srate)
{
	demod.init(high ? 1650 : 980, high ? 1850 : 1180, 300, srate);
	msgs.clear();
	bad = 0;
	hdlc_rx_reset();
}

/* see v8bis.h */
void MsgRx::run(float x)
{
	s16 b = demod.run(x);
	if (b < 0)
		return;
	/* (no carrier: marks, which the receiver takes as idle) */
	hdlc_rx_bit(demod.carrier ? b : 1);
}

/* see v8bis.h */
void MsgRx::hdlc_rx_frame(HdlcV8bis, const unsigned char *d, s16 len,
			  bool whole)
{
	/* (flags back to back, a few bits before the first flag) */
	if (len < 3)
		return;
	if (!whole || hdlc_fcs(d, len) != HDLC_FCS_GOOD) {
		bad++;
		return;
	}
	msgs.emplace_back(d, d + len - 2);
}

} // namespace v8bis
} // namespace dsp
