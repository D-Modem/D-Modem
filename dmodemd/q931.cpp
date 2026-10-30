/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    q931.cpp - the user side of ISDN's call control on a basic rate
 *               interface (q931.h)
 *
 *    A message: the protocol discriminator 0x08, the call reference
 *    (its length, 1 on a basic rate interface; its flag 0x80 set on
 *    messages from the side that did not make the call), the message
 *    type, then its information elements: one octet for those 0x80 and
 *    up, else the element's identifier, its length and its octets.
 *
 */

#include <cstring>

#include "q931.h"

namespace dmd {

namespace {

/* the protocol discriminator */
const u8 PD = 0x08;

/* information elements */
enum : u8 {
	IE_BEARER = 0x04, IE_CAUSE = 0x08, IE_CALL_STATE = 0x14,
	IE_CHANNEL = 0x18, IE_CALLING = 0x6C, IE_CALLED = 0x70,
	IE_SENDING_COMPLETE = 0xA1,
};

/* the digits of a number to dial: 0-9, * and # */
std::string digits(const char *number)
{
	std::string d;
	for (const char *p = number; p && *p; p++)
		if ((*p >= '0' && *p <= '9') || *p == '*' || *p == '#')
			d += *p;
	return d;
}

/* a number element (calling or called party): id, its octet 3 (type
   and plan, extension bit set), the digits; how many octets */
s16 number_ie(u8 *p, u8 id, const std::string &d)
{
	const size_t n = d.size() < 32 ? d.size() : 32;
	p[0] = id;
	p[1] = static_cast<u8>(n + 1);
	p[2] = 0x81;    /* unknown type, ISDN/telephony numbering plan */
	memcpy(p + 3, d.data(), n);
	return static_cast<s16>(n + 3);
}

/* a number element's digits from octet 3 on (n octets); presentation
   restricted if octet 3a says so */
void number_of(const u8 *p, s16 n, std::string &digits_out,
	       bool *restricted)
{
	s16 i = 0;
	if (n < 1)
		return;
	if (!(p[i] & 0x80)) {
		/* octet 3a: presentation and screening */
		i++;
		if (i < n && restricted)
			*restricted = ((p[i] >> 5) & 3) == 1;
	}
	i++;
	digits_out.clear();
	for (; i < n; i++)
		digits_out += static_cast<char>(p[i] & 0x7F);
}

/* the bearer capability's information transfer capability */
Q931::Bearer bearer_of(const u8 *p, s16 n)
{
	if (n < 1)
		return Q931::Bearer::Other;
	switch (p[0] & 0x1F) {
	case 0x00: return Q931::Bearer::Speech;
	case 0x10: return Q931::Bearer::Audio;
	case 0x08: return Q931::Bearer::Udi;
	default: return Q931::Bearer::Other;
	}
}

} // namespace

/* see q931.h: codeset 0's elements (a locking shift's others skipped,
   a non-locking shift's one) */
bool Q931::parse(const u8 *msg, s16 n, s16 off, Info &info)
{
	u8 codeset = 0, once = 0xFF;
	while (off < n) {
		const u8 id = msg[off];
		u8 cs = once != 0xFF ? once : codeset;
		once = 0xFF;
		if (id & 0x80) {
			/* one octet: a shift, sending complete, ... */
			if ((id & 0xF0) == 0x90) {
				if (id & 0x08)
					once = id & 0x07;
				else
					codeset = id & 0x07;
			}
			off++;
			continue;
		}
		if (off + 2 > n || off + 2 + msg[off + 1] > n)
			return false;
		const u8 *p = msg + off + 2;
		const s16 len = msg[off + 1];
		off = static_cast<s16>(off + 2 + len);
		if (cs != 0)
			continue;
		switch (id) {
		case IE_BEARER:
			info.bearer = true;
			info.bc = bearer_of(p, len);
			break;
		case IE_CAUSE: {
			s16 i = 0;
			if (len >= 1 && !(p[0] & 0x80))
				i++;    /* octet 3a: the recommendation */
			if (len >= i + 2)
				info.cause = p[i + 1] & 0x7F;
			break;
		}
		case IE_CALL_STATE:
			if (len >= 1)
				info.call_state = p[0] & 0x3F;
			break;
		case IE_CHANNEL:
			/* a basic rate interface's: its info channel
			   selection (no interface identifier) */
			if (len >= 1 && !(p[0] & 0x60))
				info.channel = p[0] & 0x03;
			break;
		case IE_CALLING:
			info.calling = true;
			number_of(p, len, info.calling_number, &info.withheld);
			break;
		case IE_CALLED:
			number_of(p, len, info.called_number, nullptr);
			break;
		default:
			break;
		}
	}
	return true;
}

/* see q931.h */
s16 Q931::dial(const char *number, Bearer b, u64 now)
{
	const std::string d = digits(number);
	if (!idle() || d.empty())
		return -1;
	u8 *p = setup_;
	s16 n = 0;
	*p++ = PD;
	*p++ = 1;
	next_cr_ = static_cast<u8>(next_cr_ % 127 + 1);
	*p++ = next_cr_;
	*p++ = SETUP;
	/* the bearer: unrestricted digital information, 64 kbit/s; or
	   3.1 kHz audio, 64 kbit/s, G.711 A-law */
	if (b == Bearer::Udi) {
		static const u8 udi[] = { IE_BEARER, 2, 0x88, 0x90 };
		memcpy(p, udi, sizeof(udi));
		p += sizeof(udi);
	} else {
		static const u8 audio[] = { IE_BEARER, 3, 0x90, 0x90, 0xA3 };
		memcpy(p, audio, sizeof(audio));
		p += sizeof(audio);
	}
	/* any B-channel */
	*p++ = IE_CHANNEL;
	*p++ = 1;
	*p++ = 0x83;
	if (!msn_.empty())
		p += number_ie(p, IE_CALLING, digits(msn_.c_str()));
	p += number_ie(p, IE_CALLED, d);
	*p++ = IE_SENDING_COMPLETE;
	n = static_cast<s16>(p - setup_);
	setup_n_ = n;

	cr_ = next_cr_;
	ours_ = true;
	channel_ = 0;
	told_ = false;
	state_ = U1;
	user_.q931_send(setup_, setup_n_);
	tries_ = 1;
	timer(Timer::T303, T303, now);
	return 0;
}

/* see q931.h */
void Q931::answer(u64 now)
{
	if (state_ != U7)
		return;
	user_.q931_channel(channel_);
	send(CONNECT);
	state_ = U8;
	timer(Timer::T313, T313, now);
}

/* see q931.h */
void Q931::hangup(u64 now)
{
	told_ = true;
	if (idle())
		return;
	u8 ies[4];
	s16 n = cause_ie(ies, NORMAL_CLEARING);
	send(DISCONNECT, ies, n);
	state_ = U11;
	timer(Timer::T305, T305, now);
}

/* see q931.h */
void Q931::receive(const u8 *msg, s16 n, u64 now)
{
	if (n < 3 || msg[0] != PD)
		return;
	/* (a basic rate interface's call references: one octet) */
	const s16 crlen = msg[1] & 0x0F;
	if (crlen > 1 || n < 3 + crlen)
		return;
	const s16 off = static_cast<s16>(2 + crlen + 1);
	const u8 t = msg[2 + crlen];
	const u8 cr = crlen ? msg[2] & 0x7F : 0;
	const bool flag = crlen && (msg[2] & 0x80);

	if (cr == 0) {
		/* the global call reference: a RESTART ends any call */
		if (t == RESTART) {
			if (state_ != U0) {
				ended(TEMPORARY_FAILURE);
				null();
			}
			u8 out[64];
			s16 k = static_cast<s16>(n - off);
			if (k > static_cast<s16>(sizeof(out)) - 3)
				k = static_cast<s16>(sizeof(out)) - 3;
			out[0] = PD;
			out[1] = 0;
			out[2] = RESTART_ACK;
			memcpy(out + 3, msg + off, static_cast<size_t>(k));
			user_.q931_send(out, static_cast<s16>(k + 3));
		}
		return;
	}

	Info info;
	if (!parse(msg, n, off, info))
		return;
	if (t == SETUP && !flag) {
		setup(cr, info);
		return;
	}
	if (state_ == U0 || cr != cr_ || flag != ours_) {
		stray(cr, flag, t);
		return;
	}

	const s16 cause = info.cause >= 0 ? info.cause : NORMAL_CLEARING;
	switch (t) {
	case SETUP_ACK:
		if (state_ == U1) {
			state_ = U2;
			timer(Timer::None, 0, now);
			channel(info);
		}
		break;
	case CALL_PROCEEDING:
		if (state_ == U1 || state_ == U2) {
			state_ = U3;
			timer(Timer::None, 0, now);
			channel(info);
		}
		break;
	case ALERTING:
		if (state_ >= U1 && state_ <= U3) {
			state_ = U4;
			timer(Timer::None, 0, now);
			channel(info);
		}
		break;
	case PROGRESS:
		if (ours_)
			channel(info);
		break;
	case CONNECT:
		if (state_ >= U1 && state_ <= U4) {
			channel(info);
			timer(Timer::None, 0, now);
			send(CONNECT_ACK);
			state_ = U10;
			user_.q931_connected();
		}
		break;
	case CONNECT_ACK:
		if (state_ == U8) {
			timer(Timer::None, 0, now);
			state_ = U10;
			user_.q931_connected();
		}
		break;
	case DISCONNECT:
		if (state_ != U19) {
			ended(cause);
			release(NORMAL_CLEARING, now);
		}
		break;
	case RELEASE:
		ended(cause);
		send(RELEASE_COMPLETE);
		null();
		break;
	case RELEASE_COMPLETE:
		ended(cause);
		null();
		break;
	case STATUS_ENQUIRY: {
		u8 ies[8];
		s16 k = cause_ie(ies, STATUS_ENQUIRY_RESPONSE);
		ies[k++] = IE_CALL_STATE;
		ies[k++] = 1;
		ies[k++] = state_;
		send(STATUS, ies, k);
		break;
	}
	case STATUS:
		/* the network has no call: neither have we */
		if (info.call_state == 0) {
			ended(cause);
			null();
		}
		break;
	default:
		break;
	}
}

/* see q931.h */
void Q931::poll(u64 now)
{
	if (timer_ == Timer::None || now < due_)
		return;
	const Timer t = timer_;
	timer_ = Timer::None;
	switch (t) {
	case Timer::T303:
		if (tries_ < 2) {
			tries_++;
			user_.q931_send(setup_, setup_n_);
			timer(Timer::T303, T303, now);
		} else {
			ended(TIMER_EXPIRY);
			null();
		}
		break;
	case Timer::T305:
		release(NORMAL_CLEARING, now);
		break;
	case Timer::T308:
		if (tries_ < 2) {
			tries_++;
			u8 ies[4];
			send(RELEASE, ies, cause_ie(ies, NORMAL_CLEARING));
			timer(Timer::T308, T308, now);
		} else {
			null();
		}
		break;
	case Timer::T313: {
		ended(TIMER_EXPIRY);
		u8 ies[4];
		send(DISCONNECT, ies, cause_ie(ies, TIMER_EXPIRY));
		state_ = U11;
		timer(Timer::T305, T305, now);
		break;
	}
	case Timer::None:
		break;
	}
}

/* see q931.h */
s32 Q931::next_timer_ms(u64 now) const
{
	if (timer_ == Timer::None)
		return -1;
	return due_ > now ? static_cast<s32>(due_ - now) : 0;
}

/* see q931.h: ours, with the flag of the side we are */
void Q931::send(u8 t, const u8 *ies, s16 n)
{
	send_cr(cr_, !ours_, t, ies, n);
}

/* (see send()) */
void Q931::send_cr(u8 cr, bool flag, u8 t, const u8 *ies, s16 n)
{
	u8 out[64];
	if (n > static_cast<s16>(sizeof(out)) - 4)
		n = static_cast<s16>(sizeof(out)) - 4;
	out[0] = PD;
	out[1] = 1;
	out[2] = static_cast<u8>(cr | (flag ? 0x80 : 0));
	out[3] = t;
	if (n > 0)
		memcpy(out + 4, ies, static_cast<size_t>(n));
	user_.q931_send(out, static_cast<s16>(n + 4));
}

/* see q931.h: CCITT's coding, the user's location */
s16 Q931::cause_ie(u8 *p, u8 c)
{
	p[0] = IE_CAUSE;
	p[1] = 2;
	p[2] = 0x80;
	p[3] = static_cast<u8>(0x80 | c);
	return 4;
}

/* see q931.h */
void Q931::timer(Timer t, s32 ms, u64 now)
{
	timer_ = t;
	due_ = now + static_cast<u64>(ms);
}

/* see q931.h */
void Q931::channel(const Info &info)
{
	if (channel_ == 0 && (info.channel == 1 || info.channel == 2)) {
		channel_ = info.channel;
		user_.q931_channel(channel_);
	}
}

/* see q931.h */
void Q931::ended(s16 cause)
{
	if (told_)
		return;
	told_ = true;
	user_.q931_ended(cause);
}

/* see q931.h */
void Q931::release(u8 cause, u64 now)
{
	u8 ies[4];
	send(RELEASE, ies, cause_ie(ies, cause));
	state_ = U19;
	tries_ = 1;
	timer(Timer::T308, T308, now);
}

/* see q931.h */
void Q931::null()
{
	state_ = U0;
	timer_ = Timer::None;
	channel_ = 0;
}

/* see q931.h: one we can take (a bearer of ours, a B-channel, our MSN)
   rings, or is busy with a call up; others are left to the other
   terminals on the bus */
void Q931::setup(u8 cr, const Info &info)
{
	if (!info.bearer || info.bc == Bearer::Other)
		return;
	if (info.channel != 1 && info.channel != 2)
		return;
	if (!msn_.empty() && !info.called_number.empty()) {
		const std::string &c = info.called_number;
		if (c.size() < msn_.size() ||
		    c.compare(c.size() - msn_.size(), msn_.size(), msn_))
			return;
	}
	if (!idle()) {
		u8 ies[4];
		send_cr(cr, true, RELEASE_COMPLETE, ies,
			cause_ie(ies, USER_BUSY));
		return;
	}
	cr_ = cr;
	ours_ = false;
	channel_ = info.channel;
	told_ = false;
	timer_ = Timer::None;
	state_ = U7;
	const u8 ch[] = { IE_CHANNEL, 1, static_cast<u8>(0x88 | channel_) };
	send(ALERTING, ch, sizeof(ch));
	Call c;
	c.bearer = info.bc;
	c.calling = info.calling_number;
	c.withheld = info.withheld;
	c.called = info.called_number;
	c.channel = channel_;
	user_.q931_offered(c);
}

/* see q931.h: Q.931's 5.8.3.2 */
void Q931::stray(u8 cr, bool flag, u8 t)
{
	u8 ies[8];
	switch (t) {
	case RELEASE:
		send_cr(cr, !flag, RELEASE_COMPLETE, nullptr, 0);
		break;
	case RELEASE_COMPLETE:
	case STATUS:
		break;
	case STATUS_ENQUIRY: {
		s16 k = cause_ie(ies, STATUS_ENQUIRY_RESPONSE);
		ies[k++] = IE_CALL_STATE;
		ies[k++] = 1;
		ies[k++] = U0;
		send_cr(cr, !flag, STATUS, ies, k);
		break;
	}
	default:
		send_cr(cr, !flag, RELEASE_COMPLETE, ies,
			cause_ie(ies, INVALID_CALL_REF));
		break;
	}
}

} // namespace dmd
