/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    h245.cpp - H.245's messages for V.75 in aligned PER (h245.h)
 *
 *    X.691's aligned variant, as far as these messages need it: a
 *    SEQUENCE's extension bit and the bits of its optional components,
 *    a CHOICE's extension bit and its index in as few bits as its root
 *    alternatives need, a constrained whole number in a bit field (a
 *    range up to 255), an octet (256) or two (up to 65536) aligned on an
 *    octet, or in as many aligned octets as a 2 bit length before them
 *    says (INTEGER (0..4294967295)), BOOLEAN a bit, NULL nothing.  What
 *    an encoder of a later H.245 adds in a SEQUENCE's or CHOICE's
 *    extension (an open type, its length first) is passed over.
 *
 */

#include "h245.h"

namespace dlink {
namespace h245 {

namespace {

/* the alternatives' indices (H.245's MultimediaSystemControlMessage,
   RequestMessage, ResponseMessage) and the root alternatives' counts */
enum {
	MSG_REQUEST = 0, MSG_RESPONSE = 1, MSG_ROOTS = 4,
	REQ_TCS = 2, REQ_OLC = 3, REQ_CLC = 4, REQ_ROOTS = 11,
	RSP_TCS_ACK = 3, RSP_TCS_REJECT = 4, RSP_OLC_ACK = 5,
	RSP_OLC_REJECT = 6, RSP_CLC_ACK = 7, RSP_ROOTS = 19,
	DT_AUDIO = 3, DT_DATA = 4, DT_ROOTS = 6,
	AUDIO_G729A = 11, AUDIO_ROOTS = 14,
	APP_NONSTANDARD = 0, APP_USERDATA = 3, APP_T84 = 4, APP_NLPID = 7,
	APP_DSVD = 8, APP_ROOTS = 10,
	PROTO_ROOTS = 7,
	MUX_V76_FWD = 2, MUX_FWD_ROOTS = 3,
	MUX_V76_REV = 1, MUX_REV_ROOTS = 2,
	CRC_ROOTS = 3, SR_ROOTS = 3, MODE_ROOTS = 2, REC_ROOTS = 3,
	CAUSE_ROOTS = 6, TCS_CAUSE_ROOTS = 4,
};

/* the bits a constrained whole number of range values takes in a bit
   field */
int field_bits(u32 range)
{
	int n = 0;
	while (n < 32 && (u64{1} << n) < range)
		n++;
	return n;
}

/* the octets v takes, at least 1 */
int octets(u32 v)
{
	int n = 1;
	while (n < 4 && v >> (8 * n))
		n++;
	return n;
}

/* aligned PER's bits out */
class Writer {
public:
	/* bit b */
	void bit(bool b)
	{
		if (!(n_ & 7))
			buf_.push_back(0);
		if (b)
			buf_.back() |= static_cast<u8>(0x80 >> (n_ & 7));
		n_++;
	}
	/* the k low bits of v, the most significant first */
	void bits(u32 v, int k)
	{
		for (int i = k - 1; i >= 0; i--)
			bit((v >> i) & 1);
	}
	/* on to an octet's start */
	void align()
	{
		n_ = (n_ + 7) & ~size_t{7};
	}
	/* v in lo..hi, a constrained whole number (X.691 11.5.7) */
	void cint(u32 v, u32 lo, u32 hi)
	{
		const u64 range = u64{hi} - lo + 1;
		v -= lo;
		if (range == 1)
			return;
		if (range <= 255) {
			bits(v, field_bits(static_cast<u32>(range)));
		} else if (range <= 65536) {
			align();
			bits(v, range == 256 ? 8 : 16);
		} else {
			const int max = octets(static_cast<u32>(range - 1));
			const int k = octets(v);
			cint(static_cast<u32>(k), 1, static_cast<u32>(max));
			align();
			bits(v, 8 * k);
		}
	}
	/* a CHOICE with an extension marker: root alternative i of n */
	void choice(u32 i, u32 n)
	{
		bit(false);
		cint(i, 0, n - 1);
	}
	/* the octets, at least one (X.691 11.1) */
	std::vector<u8> done()
	{
		if (buf_.empty())
			buf_.push_back(0);
		return buf_;
	}

private:
	std::vector<u8> buf_;
	size_t n_ = 0;
};

/* aligned PER's bits in; ok() false once past the end */
class Reader {
public:
	/* the n octets at p */
	Reader(const u8 *p, size_t n) : p_(p), len_(8 * n) {}
	/* the next bit (0 past the end) */
	bool bit()
	{
		if (pos_ >= len_) {
			ok_ = false;
			return false;
		}
		bool b = (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
		pos_++;
		return b;
	}
	/* k bits, the most significant first */
	u32 bits(int k)
	{
		u32 v = 0;
		for (int i = 0; i < k; i++)
			v = v << 1 | (bit() ? 1 : 0);
		return v;
	}
	/* on to an octet's start */
	void align()
	{
		pos_ = (pos_ + 7) & ~size_t{7};
	}
	/* a constrained whole number in lo..hi */
	u32 cint(u32 lo, u32 hi)
	{
		const u64 range = u64{hi} - lo + 1;
		if (range == 1)
			return lo;
		if (range <= 255)
			return lo + bits(field_bits(static_cast<u32>(range)));
		if (range <= 65536) {
			align();
			return lo + bits(range == 256 ? 8 : 16);
		}
		const int max = octets(static_cast<u32>(range - 1));
		const int k = static_cast<int>(cint(1, static_cast<u32>(max)));
		align();
		return lo + bits(8 * k);
	}
	/* a length determinant (X.691 11.9.3.6, 11.9.3.7; no fragments) */
	u32 length()
	{
		align();
		if (!bit())
			return bits(7);
		if (!bit())
			return bits(14);
		ok_ = false;
		return 0;
	}
	/* a normally small non-negative whole number (11.6) */
	u32 small()
	{
		if (!bit())
			return bits(6);
		return length() ? bits(8) : 0;
	}
	/* an open type passed over */
	void skip_open()
	{
		u32 n = length();
		pos_ += 8 * size_t{n};
		if (pos_ > len_)
			ok_ = false;
	}
	/* a SEQUENCE's extension additions passed over (its extension bit
	   set) */
	void skip_ext()
	{
		u32 n = small() + 1;
		u32 present = 0;
		for (u32 i = 0; i < n; i++)
			present += bit() ? 1 : 0;
		for (u32 i = 0; i < present && ok_; i++)
			skip_open();
	}
	/* a CHOICE with an extension marker: its root alternative, or -1
	   for an extension's (passed over) */
	int choice(u32 n)
	{
		if (bit()) {
			small();
			skip_open();
			return -1;
		}
		return static_cast<int>(cint(0, n - 1));
	}
	/* (see the class) */
	bool ok() const { return ok_; }
	/* mark the input as not what was expected */
	void fail() { ok_ = false; }

private:
	const u8 *p_;
	size_t len_, pos_ = 0;
	bool ok_ = true;
};

/* a direction's V76LogicalChannelParameters */
void put_v76(Writer &w, const V76Params &v)
{
	w.bit(false);                           /* V76LogicalChannelParameters */
	w.bit(false);                           /* V76HDLCParameters */
	w.choice(v.crc == 8 ? 0 : v.crc == 32 ? 2 : 1, CRC_ROOTS);
	w.cint(v.n401, 1, 4095);
	w.bit(false);                           /* loopbackTestProcedure */
	w.choice(0, SR_ROOTS);                  /* noSuspendResume */
	w.bit(false);                           /* uIH */
	w.choice(v.erm ? 0 : 1, MODE_ROOTS);
	if (v.erm) {
		w.bit(false);
		w.cint(v.window, 1, 127);
		w.choice(0, REC_ROOTS);         /* rej */
	}
	w.bit(false);                           /* V75Parameters */
	w.bit(v.audio_header);
}

/* (see put_v76) read; what is not ours (SREJ, suspend/resume, UIH,
   loop-back) is read and kept out of v */
void get_v76(Reader &r, V76Params *v)
{
	const bool ext = r.bit();
	const bool hext = r.bit();
	int crc = r.choice(CRC_ROOTS);
	v->crc = static_cast<u8>(crc == 0 ? 8 : crc == 2 ? 32 : 16);
	v->n401 = static_cast<u16>(r.cint(1, 4095));
	r.bit();
	if (hext)
		r.skip_ext();
	r.choice(SR_ROOTS);
	r.bit();
	int mode = r.choice(MODE_ROOTS);
	v->erm = mode == 0;
	if (mode == 0) {
		const bool eext = r.bit();
		v->window = static_cast<u8>(r.cint(1, 127));
		r.choice(REC_ROOTS);
		if (eext)
			r.skip_ext();
	} else if (mode < 0) {
		r.fail();
	}
	const bool vext = r.bit();
	v->audio_header = r.bit();
	if (vext)
		r.skip_ext();
	if (ext)
		r.skip_ext();
}

/* a DataType: G.729 Annex A audio, or data */
void put_type(Writer &w, const Channel &c)
{
	if (c.kind == Kind::Audio) {
		w.choice(DT_AUDIO, DT_ROOTS);
		w.choice(AUDIO_G729A, AUDIO_ROOTS);
		w.cint(c.frames, 1, 256);
		return;
	}
	w.choice(DT_DATA, DT_ROOTS);
	w.bit(false);                           /* DataApplicationCapability */
	if (c.kind == Kind::Control) {
		w.choice(APP_DSVD, APP_ROOTS);
	} else {
		w.choice(APP_USERDATA, APP_ROOTS);
		w.choice(static_cast<u32>(c.proto), PROTO_ROOTS);
	}
	w.cint(c.max_rate, 0, 0xffffffffu);
}

/* (see put_type) read into c: an application or protocol we cannot
   read past (nonStandard, T.84, NLPID) fails r */
void get_type(Reader &r, Channel *c)
{
	int t = r.choice(DT_ROOTS);
	if (t == DT_AUDIO) {
		int a = r.choice(AUDIO_ROOTS);
		if (a != AUDIO_G729A) {
			/* the INTEGER (1..256) of most of them */
			c->kind = Kind::Other;
			if (a < 1 || a == 7 || a > 10)
				r.fail();
			else
				r.cint(1, 256);
			return;
		}
		c->kind = Kind::Audio;
		c->frames = static_cast<u16>(r.cint(1, 256));
		return;
	}
	if (t != DT_DATA) {
		c->kind = Kind::Other;
		if (t != 1)                     /* only nullData is empty */
			r.fail();
		return;
	}
	const bool ext = r.bit();
	int app = r.choice(APP_ROOTS);
	if (app < 0 || app == APP_NONSTANDARD || app == APP_T84 ||
	    app == APP_NLPID) {
		c->kind = Kind::Other;
		if (app >= 0)
			r.fail();
	} else if (app == APP_DSVD) {
		c->kind = Kind::Control;
	} else {
		int p = r.choice(PROTO_ROOTS);
		if (p == 0)
			r.fail();
		c->kind = app == APP_USERDATA ? Kind::Data : Kind::Other;
		c->proto = p < 0 ? Proto::Other : static_cast<Proto>(p);
	}
	c->max_rate = r.cint(0, 0xffffffffu);
	if (ext)
		r.skip_ext();
}

/* OpenLogicalChannel */
void put_olc(Writer &w, const Channel &c)
{
	w.bit(false);
	w.bit(c.has_rev);
	w.cint(c.lcn, 1, 65535);
	/* forwardLogicalChannelParameters */
	w.bit(false);
	w.bit(c.port >= 0);
	if (c.port >= 0)
		w.cint(static_cast<u32>(c.port), 0, 65535);
	put_type(w, c);
	w.choice(MUX_V76_FWD, MUX_FWD_ROOTS);
	put_v76(w, c.fwd);
	if (!c.has_rev)
		return;
	/* reverseLogicalChannelParameters */
	w.bit(false);
	w.bit(true);
	put_type(w, c);
	w.choice(MUX_V76_REV, MUX_REV_ROOTS);
	put_v76(w, c.rev);
}

/* (see put_olc) read; the multiplex parameters must be V.76's */
void get_olc(Reader &r, Msg *m)
{
	Channel &c = m->ch;
	const bool ext = r.bit();
	c.has_rev = r.bit();
	c.lcn = static_cast<u16>(r.cint(1, 65535));
	m->lcn = c.lcn;
	const bool fext = r.bit();
	const bool port = r.bit();
	c.port = port ? static_cast<s32>(r.cint(0, 65535)) : -1;
	get_type(r, &c);
	if (!r.ok())
		return;
	if (r.choice(MUX_FWD_ROOTS) != MUX_V76_FWD) {
		r.fail();
		return;
	}
	get_v76(r, &c.fwd);
	if (fext)
		r.skip_ext();
	if (c.has_rev) {
		const bool rext = r.bit();
		const bool mux = r.bit();
		Channel rc;
		get_type(r, &rc);
		if (!r.ok())
			return;
		if (mux) {
			if (r.choice(MUX_REV_ROOTS) != MUX_V76_REV) {
				r.fail();
				return;
			}
			get_v76(r, &c.rev);
		}
		if (rext)
			r.skip_ext();
	}
	if (ext)
		r.skip_ext();
}

} // namespace

/* (see h245.h) */
std::vector<u8> encode(const Msg &m)
{
	Writer w;
	switch (m.type) {
	case Type::Olc:
		w.choice(MSG_REQUEST, MSG_ROOTS);
		w.choice(REQ_OLC, REQ_ROOTS);
		put_olc(w, m.ch);
		break;
	case Type::Clc:
		w.choice(MSG_REQUEST, MSG_ROOTS);
		w.choice(REQ_CLC, REQ_ROOTS);
		w.bit(false);
		w.cint(m.lcn, 1, 65535);
		w.bit(m.lcse);                  /* source: user or lcse */
		break;
	case Type::OlcAck:
		w.choice(MSG_RESPONSE, MSG_ROOTS);
		w.choice(RSP_OLC_ACK, RSP_ROOTS);
		w.bit(false);
		w.bit(false);                   /* no reverse parameters */
		w.cint(m.lcn, 1, 65535);
		break;
	case Type::OlcReject:
		w.choice(MSG_RESPONSE, MSG_ROOTS);
		w.choice(RSP_OLC_REJECT, RSP_ROOTS);
		w.bit(false);
		w.cint(m.lcn, 1, 65535);
		w.choice(m.cause, CAUSE_ROOTS);
		break;
	case Type::ClcAck:
		w.choice(MSG_RESPONSE, MSG_ROOTS);
		w.choice(RSP_CLC_ACK, RSP_ROOTS);
		w.bit(false);
		w.cint(m.lcn, 1, 65535);
		break;
	case Type::TcsAck:
		w.choice(MSG_RESPONSE, MSG_ROOTS);
		w.choice(RSP_TCS_ACK, RSP_ROOTS);
		w.bit(false);
		w.cint(m.seq, 0, 255);
		break;
	case Type::TcsReject:
		w.choice(MSG_RESPONSE, MSG_ROOTS);
		w.choice(RSP_TCS_REJECT, RSP_ROOTS);
		w.bit(false);
		w.cint(m.seq, 0, 255);
		w.choice(m.cause < TCS_CAUSE_ROOTS ? m.cause : 0,
			 TCS_CAUSE_ROOTS);
		break;
	case Type::Tcs:
	case Type::None:
	case Type::Other:
		return {};
	}
	return w.done();
}

/* (see h245.h) */
bool decode(const u8 *p, size_t n, Msg *m)
{
	Reader r(p, n);
	*m = Msg();
	m->type = Type::Other;
	int top = r.choice(MSG_ROOTS);
	if (top == MSG_REQUEST) {
		switch (r.choice(REQ_ROOTS)) {
		case REQ_OLC:
			m->type = Type::Olc;
			get_olc(r, m);
			break;
		case REQ_CLC:
			m->type = Type::Clc;
			r.bit();
			m->lcn = static_cast<u16>(r.cint(1, 65535));
			m->lcse = r.bit();
			break;
		case REQ_TCS:
			m->type = Type::Tcs;
			r.bit();
			r.bits(3);
			m->seq = static_cast<u8>(r.cint(0, 255));
			break;
		default:
			break;
		}
	} else if (top == MSG_RESPONSE) {
		switch (r.choice(RSP_ROOTS)) {
		case RSP_OLC_ACK:
			m->type = Type::OlcAck;
			r.bit();
			r.bit();
			m->lcn = static_cast<u16>(r.cint(1, 65535));
			break;
		case RSP_OLC_REJECT: {
			m->type = Type::OlcReject;
			r.bit();
			m->lcn = static_cast<u16>(r.cint(1, 65535));
			int c = r.choice(CAUSE_ROOTS);
			m->cause = static_cast<u8>(c < 0 ? UNSPECIFIED : c);
			break;
		}
		case RSP_CLC_ACK:
			m->type = Type::ClcAck;
			r.bit();
			m->lcn = static_cast<u16>(r.cint(1, 65535));
			break;
		case RSP_TCS_ACK:
			m->type = Type::TcsAck;
			r.bit();
			m->seq = static_cast<u8>(r.cint(0, 255));
			break;
		case RSP_TCS_REJECT: {
			m->type = Type::TcsReject;
			r.bit();
			m->seq = static_cast<u8>(r.cint(0, 255));
			int c = r.choice(TCS_CAUSE_ROOTS);
			m->cause = static_cast<u8>(c < 0 ? UNSPECIFIED : c);
			break;
		}
		default:
			break;
		}
	}
	return r.ok();
}

/* (see h245.h) */
std::vector<u8> user_data(const Msg &m)
{
	std::vector<u8> u = encode(m);
	u.insert(u.begin(), FI_V75);
	return u;
}

/* (see h245.h) */
bool from_user_data(const u8 *p, size_t n, Msg *m)
{
	*m = Msg();
	if (n < 2 || p[0] != FI_V75)
		return false;
	return decode(p + 1, n - 1, m);
}

/* (see h245.h) */
const char *type_name(Type t)
{
	switch (t) {
	case Type::None:
		return "none";
	case Type::Olc:
		return "OpenLogicalChannel";
	case Type::OlcAck:
		return "OpenLogicalChannelAck";
	case Type::OlcReject:
		return "OpenLogicalChannelReject";
	case Type::Clc:
		return "CloseLogicalChannel";
	case Type::ClcAck:
		return "CloseLogicalChannelAck";
	case Type::Tcs:
		return "TerminalCapabilitySet";
	case Type::TcsAck:
		return "TerminalCapabilitySetAck";
	case Type::TcsReject:
		return "TerminalCapabilitySetReject";
	case Type::Other:
		break;
	}
	return "another message";
}

} // namespace h245
} // namespace dlink
