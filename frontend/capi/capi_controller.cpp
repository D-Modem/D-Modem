/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    capi_controller.cpp - a CAPI 2.0 controller on a D-Modem
 *                          (capi_controller.h)
 *
 */

#include <algorithm>
#include <cstdio>

#include "capi_controller.h"

namespace capi {

namespace {

/* the modem's guard before and after +++ (the core's, 500 ms, with a
   margin), its timeouts */
const u32 GUARD_MS = 1100, INIT_MS = 2000, SETUP_MS = 3000,
	DIAL_MS = 120000, ANSWER_MS = 60000, ESCAPE_MS = 3000,
	HANGUP_MS = 5000, ABORT_MS = 5000;
/* the caller's number after the first +CRING, the rings over after
   (the modem rings each 6 s), the tail held as NO CARRIER's start, the
   data left to the application after the far end hung up */
const u32 OFFER_MS = 300, RINGS_GONE_MS = 10000, HELD_MS = 50,
	DRAIN_MS = 5000;
/* the modem's output held for the applications at most */
const size_t RX_MAX = 65536;
/* how a call that is on ends, in the modem's output (D-Modem's: no
   line end before it) */
const std::string NO_CARRIER = "NO CARRIER\r\n";
/* a reason: another application took the call */
const u16 OTHER_APP = 0x3304;
/* an info value: the message not now (a state's) */
const u16 NOT_NOW = 0x2001;
/* an info value: no reset in this protocol */
const u16 NO_RESET = 0x300D;

/* the FCS-16 (RFC 1662) after one more octet */
u16 fcs16(u16 fcs, u8 c)
{
	fcs ^= c;
	for (int i = 0; i < 8; i++)
		fcs = fcs & 1 ? static_cast<u16>(fcs >> 1 ^ 0x8408) :
			static_cast<u16>(fcs >> 1);
	return fcs;
}

/* V.110's user rate in +CBST's speed, 0 for one it has not */
int cbst_speed(u16 rate)
{
	switch (rate) {
	case 0: return 0;
	case 300: return 65;
	case 1200: return 66;
	case 2400: return 68;
	case 4800: return 70;
	case 9600: return 71;
	case 14400: return 75;
	case 19200: return 79;
	case 28800: return 80;
	case 38400: return 81;
	case 56000: return 83;
	default: return -1;
	}
}

/* a number's digits (a party number struct's after its first octets) */
std::string digits(const std::vector<u8> &s, size_t skip)
{
	std::string d;
	for (size_t k = skip; k < s.size(); k++) {
		char c = static_cast<char>(s[k]);
		if ((c >= '0' && c <= '9') || c == '*' || c == '#' ||
		    c == '+' || c == ',')
			d += c;
	}
	return d;
}

/* l begins with p */
bool starts(const std::string &l, const char *p)
{
	return l.compare(0, std::char_traits<char>::length(p), p) == 0;
}

} // namespace

/* see capi_controller.h */
std::vector<u8> profile()
{
	std::vector<u8> p;
	Writer(p).w(1).w(1).d(0).d(PROFILE_B1).d(PROFILE_B2).d(PROFILE_B3);
	p.resize(64, 0);
	return p;
}

/* see capi_controller.h */
Mode mode_of(const std::vector<u8> &bprot)
{
	Reader r(bprot);
	u16 b1 = r.w(), b2 = r.w(), b3 = r.w();
	std::vector<u8> b1cfg = r.s();
	if (bprot.empty())
		b1 = b2 = b3 = 0;
	Mode m;
	auto b2_at = [&m](u16 p) -> const char * {
		switch (p) {
		case 0: return "+B2=0";
		case 1: m.frames = true; return "+B2=5";
		case 9: return "+B2=9";
		default: return nullptr;
		}
	};
	Reader c(b1cfg);
	u16 rate = c.w();
	int speed = cbst_speed(rate);
	const char *b2s;
	switch (b1) {
	case 0:
		m.clear = true;
		if (!(b2s = b2_at(b2))) {
			m.info = B2_NOT_SUPPORTED;
			break;
		}
		m.at = std::string("+MS=CLEAR;+CBST=0;") + b2s;
		break;
	case 2:
	case 3:
		m.clear = true;
		if (speed < 0) {
			m.info = B1_PARAM_NOT_SUPPORTED;
			break;
		}
		if (b1 == 2) {
			if (b2 != 1) {
				m.info = B2_NOT_SUPPORTED;
				break;
			}
			m.at = "\\N0+MS=CLEAR;+CBST=" +
				std::to_string(speed) + ",0;+B2=7";
			break;
		}
		if (!(b2s = b2_at(b2))) {
			m.info = B2_NOT_SUPPORTED;
			break;
		}
		m.at = "+MS=CLEAR;+CBST=" + std::to_string(speed) + ",1;" +
			b2s;
		break;
	case 7:
	case 8:
		if (b2 == 7 && b1 == 7)
			m.at = "\\N3";
		else if (b2 == 1)
			m.at = "\\N0";
		else
			m.info = B2_NOT_SUPPORTED;
		break;
	default:
		m.info = B1_NOT_SUPPORTED;
		break;
	}
	if (m.info == NO_ERROR && b3 != 0 && !(b3 == 7 && b1 == 7))
		m.info = B3_NOT_SUPPORTED;
	if (m.info != NO_ERROR)
		m.at.clear();
	return m;
}

/* see capi_controller.h */
Controller::Controller(Out &out, const std::string &init_extra)
	: out_(out), init_("E0V1Q0X3+CRC=1;+VCID=1S0=0" + init_extra)
{
}

/* see capi_controller.h */
void Controller::add_app(u16 appl, u16 blocks, u16 len)
{
	App &a = apps_[appl];
	a.blocks = static_cast<u16>(std::min<u16>(std::max<u16>(blocks, 1), 7));
	a.len = len ? len : 2048;
}

/* see capi_controller.h */
void Controller::remove_app(u16 appl, u64 now)
{
	if (!apps_.count(appl))
		return;
	offered_.erase(appl);
	if (call_ == Call::Offered && offered_.empty()) {
		call_ = Call::None;
		ignored_ = true;
	}
	bool own = owner_ == appl && call_ != Call::None &&
		call_ != Call::Offered;
	apps_.erase(appl);
	if (own) {
		ncci_ = b3_up_ = b3_wait_ = false;
		if (call_ != Call::Clearing) {
			call_ = Call::Clearing;
			clear_reason_ = 0;
			hangup(now);
		}
	}
}

/* a message to appl, if it is still there */
void Controller::send(u16 appl, u8 cmd, u8 sub, u16 num,
		      const std::vector<u8> &params,
		      const std::vector<u8> &data)
{
	if (!apps_.count(appl))
		return;
	Msg m;
	m.appl = appl;
	m.cmd = cmd;
	m.sub = sub;
	m.num = num;
	m.params = params;
	m.data = data;
	out_.to_app(m);
}

/* m's CONF: ident (its controller, PLCI or NCCI) and info */
void Controller::conf(const Msg &m, u32 ident, u16 info)
{
	std::vector<u8> p;
	Writer(p).d(ident).w(info);
	send(m.appl, m.cmd, CONF, m.num, p);
}

/* see capi_controller.h */
void Controller::message(const Msg &m, u64 now)
{
	if (!apps_.count(m.appl))
		return;
	Reader r(m.params);
	if (m.sub == REQ) {
		switch (m.cmd) {
		case LISTEN:
			listen_req(m);
			return;
		case CONNECT:
			connect_req(m, now);
			return;
		case CONNECT_B3: {
			u32 plci = r.d();
			if (call_ == Call::Active && owner_ == m.appl &&
			    !ncci_ && plci == PLCI) {
				conf(m, NCCI, NO_ERROR);
				ncci_ = true;
				b3_active();
			} else {
				conf(m, plci, NOT_NOW);
			}
			return;
		}
		case DATA_B3:
			data_b3_req(m, now);
			return;
		case DISCONNECT_B3: {
			u32 ncci = r.d();
			if (!ncci_ || owner_ != m.appl || ncci != NCCI) {
				conf(m, ncci, ILLEGAL_IDENT);
				return;
			}
			conf(m, NCCI, NO_ERROR);
			ncci_ = b3_up_ = b3_wait_ = false;
			rx_.clear();
			rx_frames_.clear();
			rx_out_.clear();
			std::vector<u8> p;
			Writer(p).d(NCCI).w(0).empty();
			send(owner_, DISCONNECT_B3, IND, next_num(), p);
			return;
		}
		case DISCONNECT: {
			u32 plci = r.d();
			if (plci != PLCI) {
				conf(m, plci, ILLEGAL_IDENT);
				return;
			}
			if (call_ == Call::Offered && offered_.count(m.appl)) {
				conf(m, PLCI, NO_ERROR);
				offered_.erase(m.appl);
				std::vector<u8> p;
				Writer(p).d(PLCI).w(0);
				send(m.appl, DISCONNECT, IND, next_num(), p);
				if (offered_.empty()) {
					call_ = Call::None;
					ignored_ = true;
					ringing_ = false;
				}
				return;
			}
			if (call_ == Call::None || call_ == Call::Offered ||
			    owner_ != m.appl) {
				conf(m, PLCI, ILLEGAL_IDENT);
				return;
			}
			conf(m, PLCI, NO_ERROR);
			if (call_ == Call::Clearing)
				return;
			if (ncci_) {
				std::vector<u8> p;
				Writer(p).d(NCCI).w(0).empty();
				send(owner_, DISCONNECT_B3, IND, next_num(), p);
			}
			ncci_ = b3_up_ = b3_wait_ = false;
			call_ = Call::Clearing;
			clear_reason_ = 0;
			hangup(now);
			return;
		}
		case FACILITY:
			facility_req(m);
			return;
		case ALERT:
		case INFO:
			conf(m, r.d(), NO_ERROR);
			return;
		case SELECT_B_PROTOCOL:
			conf(m, r.d(), NOT_NOW);
			return;
		case RESET_B3:
			conf(m, r.d(), NO_RESET);
			return;
		default:
			fprintf(stderr, "capi: %s not done\n",
				name(m.cmd, m.sub).c_str());
			return;
		}
	}
	if (m.sub != RESP)
		return;
	switch (m.cmd) {
	case CONNECT:
		connect_resp(m, now);
		return;
	case CONNECT_B3: {
		u32 ncci = r.d();
		u16 reject = r.w();
		if (!b3_wait_ || owner_ != m.appl || ncci != NCCI)
			return;
		b3_wait_ = false;
		if (reject == 0) {
			b3_active();
			return;
		}
		ncci_ = false;
		std::vector<u8> p;
		Writer(p).d(NCCI).w(0).empty();
		send(owner_, DISCONNECT_B3, IND, next_num(), p);
		return;
	}
	case DATA_B3: {
		r.d();
		u16 handle = r.w();
		if (owner_ == m.appl && rx_out_.erase(handle))
			push_rx();
		return;
	}
	default:
		return;
	}
}

/* LISTEN_REQ: the CIP mask the application takes calls for */
void Controller::listen_req(const Msg &m)
{
	Reader r(m.params);
	u32 ctrl = r.d();
	r.d();
	u32 cip = r.d();
	if ((ctrl & 0x7F) != CONTROLLER) {
		conf(m, ctrl, ILLEGAL_CONTROLLER);
		return;
	}
	App &a = apps_[m.appl];
	a.cip_mask = cip;
	a.listening = cip != 0;
	conf(m, ctrl, NO_ERROR);
}

/* CONNECT_REQ: a call out, the number dialled once the modem is set
   up for its B protocol */
void Controller::connect_req(const Msg &m, u64 now)
{
	Reader r(m.params);
	u32 ctrl = r.d();
	r.w();                                  /* CIP */
	std::vector<u8> called = r.s();
	r.s();                                  /* calling number */
	r.s();
	r.s();                                  /* subaddresses */
	std::vector<u8> bprot = r.s();
	if ((ctrl & 0x7F) != CONTROLLER) {
		conf(m, ctrl, ILLEGAL_CONTROLLER);
		return;
	}
	if (call_ != Call::None) {
		conf(m, ctrl, OUT_OF_PLCI);
		return;
	}
	Mode mode = mode_of(bprot);
	if (mode.info != NO_ERROR) {
		conf(m, ctrl, mode.info);
		return;
	}
	conf(m, PLCI, NO_ERROR);
	call_ = Call::Out;
	owner_ = m.appl;
	mode_ = mode;
	number_ = digits(called, 1);
	clear_reason_ = 0;
	if (state_ == State::Idle)
		start_call(now);
}

/* CONNECT_RESP: an application takes the call offered, or not */
void Controller::connect_resp(const Msg &m, u64 now)
{
	Reader r(m.params);
	u32 plci = r.d();
	u16 reject = r.w();
	std::vector<u8> bprot = r.s();
	if (call_ != Call::Offered || !offered_.count(m.appl) ||
	    plci != PLCI)
		return;
	auto drop = [this](u16 appl, u16 reason) {
		std::vector<u8> p;
		Writer(p).d(PLCI).w(reason);
		send(appl, DISCONNECT, IND, next_num(), p);
		offered_.erase(appl);
	};
	Mode mode = reject == 0 ? mode_of(bprot) : Mode();
	if (reject != 0 || mode.info != NO_ERROR) {
		drop(m.appl, reject != 0 ? 0 : mode.info);
		if (offered_.empty()) {
			call_ = Call::None;
			ignored_ = true;
			ringing_ = false;
		}
		return;
	}
	offered_.erase(m.appl);
	while (!offered_.empty())
		drop(*offered_.begin(), OTHER_APP);
	call_ = Call::Accepted;
	owner_ = m.appl;
	mode_ = mode;
	ringing_ = false;
	clear_reason_ = 0;
	if (state_ == State::Idle)
		start_call(now);
}

/* DATA_B3_REQ: the data to the modem, its CONF once written */
void Controller::data_b3_req(const Msg &m, u64 now)
{
	(void)now;
	Reader r(m.params);
	u32 ncci = r.d();
	r.d();
	r.w();
	u16 handle = r.w();
	std::vector<u8> p;
	if (ncci != NCCI || !b3_up_ || owner_ != m.appl ||
	    state_ != State::Online) {
		Writer(p).d(ncci).w(handle).w(ILLEGAL_IDENT);
		send(m.appl, DATA_B3, CONF, m.num, p);
		return;
	}
	Block b;
	b.appl = m.appl;
	b.num = m.num;
	b.handle = handle;
	b.conf = true;
	if (mode_.frames) {
		u16 fcs = 0xFFFF;
		std::vector<u8> f = m.data;
		for (u8 c : m.data)
			fcs = fcs16(fcs, c);
		fcs ^= 0xFFFF;
		f.push_back(static_cast<u8>(fcs));
		f.push_back(static_cast<u8>(fcs >> 8));
		b.bytes.push_back(0x7E);
		for (u8 c : f) {
			if (c < 0x20 || c == 0x7D || c == 0x7E) {
				b.bytes.push_back(0x7D);
				c ^= 0x20;
			}
			b.bytes.push_back(c);
		}
		b.bytes.push_back(0x7E);
	} else {
		b.bytes = m.data;
	}
	tx_.push_back(std::move(b));
	push_tx();
}

/* FACILITY_REQ: supplementary services' GetSupportedServices (none),
   else not supported */
void Controller::facility_req(const Msg &m)
{
	Reader r(m.params);
	u32 ident = r.d();
	u16 sel = r.w();
	std::vector<u8> fpar = r.s();
	Reader f(fpar);
	std::vector<u8> p;
	if (sel == 3 && f.w() == 0) {
		std::vector<u8> svc, par;
		Writer(svc).w(0).d(0);
		Writer(par).w(0).s(svc);
		Writer(p).d(ident).w(NO_ERROR).w(sel).s(par);
	} else {
		Writer(p).d(ident).w(FACILITY_NOT_SUPPORTED).w(sel).empty();
	}
	send(m.appl, FACILITY, CONF, m.num, p);
}

/* "AT" line to the modem, then the state s for timeout ms at most */
void Controller::command(const std::string &line, State s, u64 now,
			 u32 timeout_ms)
{
	Block b;
	std::string c = "AT" + line + "\r";
	b.bytes.assign(c.begin(), c.end());
	tx_.push_back(std::move(b));
	state_ = s;
	deadline_ = now + timeout_ms;
	push_tx();
}

/* the call's modem commands (its B protocol), then ATD or ATA */
void Controller::start_call(u64 now)
{
	std::string at = mode_.at;
	if (!mode_.clear && !ms_.empty())
		at += "+MS=" + ms_ + ";+CBST=0";
	command(at, State::Setup, now, SETUP_MS);
}

/* the call that is on ends (call_ Clearing): the modem hangs up */
void Controller::hangup(u64 now)
{
	switch (state_) {
	case State::Init:
	case State::Query:
	case State::Idle:
		call_ended(0);
		return;
	case State::Dialling:
	case State::Answering: {
		/* any character stops the call's start */
		Block b;
		b.bytes = { '\r' };
		tx_.push_back(std::move(b));
		push_tx();
		deadline_ = now + ABORT_MS;
		return;
	}
	case State::Online:
		drop_tx();
		state_ = State::Escape;
		escape_at_ = now + GUARD_MS;
		deadline_ = escape_at_ + ESCAPE_MS;
		return;
	case State::Setup:
	case State::Escape:
	case State::Hangup:
		return;
	}
}

/* the call is over: to the application, DISCONNECT_B3_IND (with B3)
   and DISCONNECT_IND with reason (0 if it hung up itself) */
void Controller::call_ended(u16 reason)
{
	if (call_ == Call::Clearing)
		reason = clear_reason_;
	std::vector<u8> p;
	if (call_ == Call::Offered) {
		Writer(p).d(PLCI).w(reason);
		for (u16 a : offered_)
			send(a, DISCONNECT, IND, next_num(), p);
		offered_.clear();
	} else if (call_ != Call::None) {
		if (ncci_) {
			std::vector<u8> b;
			Writer(b).d(NCCI).w(0).empty();
			send(owner_, DISCONNECT_B3, IND, next_num(), b);
		}
		Writer(p).d(PLCI).w(reason);
		send(owner_, DISCONNECT, IND, next_num(), p);
	}
	call_ = Call::None;
	owner_ = 0;
	ncci_ = b3_up_ = b3_wait_ = ending_ = false;
	rx_.clear();
	rx_frames_.clear();
	rx_frame_.clear();
	rx_esc_ = false;
	rx_out_.clear();
	held_.clear();
	drop_tx();
}

/* the modem is back in command state with no call: the call (if any)
   ends with reason, the remote's data delivered first */
void Controller::finish(u16 reason, u64 now)
{
	state_ = State::Idle;
	deadline_ = 0;
	escape_at_ = 0;
	if (call_ == Call::Active && b3_up_ &&
	    (!rx_.empty() || !rx_frames_.empty() || !rx_out_.empty())) {
		ending_ = true;
		end_reason_ = reason;
		end_by_ = now + DRAIN_MS;
		return;
	}
	if (call_ != Call::None && call_ != Call::Offered)
		call_ended(reason);
}

/* the call is connected: CONNECT_ACTIVE_IND, and for a call in,
   CONNECT_B3_IND */
void Controller::connected()
{
	bool in = call_ == Call::Accepted;
	call_ = Call::Active;
	std::vector<u8> p;
	Writer(p).d(PLCI).empty().empty().empty();
	send(owner_, CONNECT_ACTIVE, IND, next_num(), p);
	if (in) {
		std::vector<u8> b;
		Writer(b).d(NCCI).empty();
		ncci_ = b3_wait_ = true;
		send(owner_, CONNECT_B3, IND, next_num(), b);
	}
}

/* B3 is on: CONNECT_B3_ACTIVE_IND, then the data held */
void Controller::b3_active()
{
	std::vector<u8> p;
	Writer(p).d(NCCI).empty();
	send(owner_, CONNECT_B3_ACTIVE, IND, next_num(), p);
	b3_up_ = true;
	push_rx();
}

/* see capi_controller.h */
void Controller::modem_input(const u8 *p, size_t n, u64 now)
{
	for (size_t k = 0; k < n; k++) {
		char c = static_cast<char>(p[k]);
		if (skip_lf_ && c == '\n') {
			skip_lf_ = false;
			continue;
		}
		skip_lf_ = false;
		if (state_ == State::Online) {
			online_input(p + k, n - k, now);
			return;
		}
		if (c == '\r' || c == '\n') {
			if (!line_.empty()) {
				std::string l;
				l.swap(line_);
				skip_lf_ = c == '\r';
				result(l, now);
			}
			continue;
		}
		if (line_.size() < 256)
			line_ += c;
	}
}

/* a line of the modem's in command state */
void Controller::result(const std::string &l, u64 now)
{
	if (starts(l, "+CRING:")) {
		ring(l.find("SYNC") != std::string::npos, now);
		return;
	}
	if (l == "RING") {
		ring(false, now);
		return;
	}
	if (starts(l, "NMBR = ")) {
		if (ringing_ && offer_at_)
			caller_ = l.substr(7);
		return;
	}
	const bool ok = l == "OK", error = l == "ERROR";
	const bool nc = l == "NO CARRIER";
	switch (state_) {
	case State::Init:
		if (ok) {
			command("+MS?", State::Query, now, INIT_MS);
		} else if (error && init_.find("+CRC=1;") != std::string::npos) {
			/* a D-Modem from before AT+CRC: RING, every call
			   taken for an audio call's */
			init_.erase(init_.find("+CRC=1;"), 7);
			command(init_, State::Init, now, INIT_MS);
		}
		return;
	case State::Query:
		if (ok || error) {
			state_ = State::Idle;
			deadline_ = 0;
			if (call_ == Call::Out || call_ == Call::Accepted)
				start_call(now);
			else if (call_ == Call::Clearing)
				call_ended(0);
		} else if (l.find(',') != std::string::npos) {
			ms_ = l;
		}
		return;
	case State::Idle:
	case State::Online:
		return;
	case State::Setup:
		if (!ok && !error)
			return;
		state_ = State::Idle;
		deadline_ = 0;
		if (call_ == Call::Clearing || error) {
			finish(error ? B1_NOT_SUPPORTED : 0, now);
			return;
		}
		if (call_ == Call::Out)
			command("D" + number_, State::Dialling, now, DIAL_MS);
		else if (call_ == Call::Accepted)
			command("A", State::Answering, now, ANSWER_MS);
		return;
	case State::Dialling:
	case State::Answering:
		if (starts(l, "CONNECT")) {
			state_ = State::Online;
			deadline_ = 0;
			if (call_ == Call::Clearing) {
				hangup(now);
				return;
			}
			connected();
			return;
		}
		if (l == "BUSY")
			finish(CAUSE_BUSY, now);
		else if (l == "NO ANSWER")
			finish(CAUSE_NO_ANSWER, now);
		else if (l == "NO DIALTONE" || l == "NO DIAL TONE")
			finish(NO_LAYER1, now);
		else if (nc)
			finish(CAUSE_NORMAL, now);
		else if (error)
			finish(CAUSE_UNSPECIFIED, now);
		return;
	case State::Escape:
		if (ok && !escape_at_)
			command("H", State::Hangup, now, HANGUP_MS);
		else if (nc)
			finish(CAUSE_NORMAL, now);
		return;
	case State::Hangup:
		if (ok || nc || error)
			finish(CAUSE_NORMAL, now);
		return;
	}
}

/* the modem's output online: the data, up to NO CARRIER */
void Controller::online_input(const u8 *p, size_t n, u64 now)
{
	std::string s = held_;
	s.append(reinterpret_cast<const char *>(p), n);
	held_.clear();
	size_t at = s.find(NO_CARRIER);
	if (at != std::string::npos) {
		deliver(reinterpret_cast<const u8 *>(s.data()), at);
		std::string rest = s.substr(at + NO_CARRIER.size());
		finish(CAUSE_NORMAL, now);
		modem_input(reinterpret_cast<const u8 *>(rest.data()),
			    rest.size(), now);
		return;
	}
	/* the longest tail that NO CARRIER's line begins with */
	size_t keep = std::min(s.size(), NO_CARRIER.size() - 1);
	while (keep && s.compare(s.size() - keep, keep, NO_CARRIER, 0,
				 keep) != 0)
		keep--;
	held_ = s.substr(s.size() - keep);
	held_at_ = now;
	deliver(reinterpret_cast<const u8 *>(s.data()), s.size() - keep);
}

/* the remote's data, to the application (whole frames with frames) */
void Controller::deliver(const u8 *p, size_t n)
{
	if (!n)
		return;
	if (!mode_.frames) {
		rx_.insert(rx_.end(), p, p + n);
		push_rx();
		return;
	}
	for (size_t k = 0; k < n; k++) {
		u8 c = p[k];
		if (c == 0x7E) {
			u16 fcs = 0xFFFF;
			for (u8 o : rx_frame_)
				fcs = fcs16(fcs, o);
			if (rx_frame_.size() > 2 && fcs == 0xF0B8) {
				rx_frame_.resize(rx_frame_.size() - 2);
				rx_frames_.push_back(rx_frame_);
			}
			rx_frame_.clear();
			rx_esc_ = false;
		} else if (c == 0x7D) {
			rx_esc_ = true;
		} else if (rx_frame_.size() < 2050) {
			rx_frame_.push_back(rx_esc_ ? c ^ 0x20 : c);
			rx_esc_ = false;
		}
	}
	push_rx();
}

/* DATA_B3_INDs while the application's window has room */
void Controller::push_rx()
{
	if (!b3_up_ || !apps_.count(owner_))
		return;
	const App &a = apps_[owner_];
	while (rx_out_.size() < a.blocks) {
		std::vector<u8> d;
		if (mode_.frames) {
			if (rx_frames_.empty())
				break;
			d.swap(rx_frames_.front());
			rx_frames_.pop_front();
			if (d.size() > a.len)
				continue;
		} else {
			if (rx_.empty())
				break;
			size_t n = std::min<size_t>(rx_.size(), a.len);
			d.assign(rx_.begin(), rx_.begin() + n);
			rx_.erase(rx_.begin(), rx_.begin() + n);
		}
		u16 h = rx_handle_++;
		std::vector<u8> p;
		Writer(p).d(NCCI).d(0).w(static_cast<u16>(d.size())).w(h).w(0)
			.d(0).d(0);
		rx_out_.insert(h);
		send(owner_, DATA_B3, IND, next_num(), p, d);
	}
	if (ending_ && rx_.empty() && rx_frames_.empty() && rx_out_.empty())
		call_ended(end_reason_);
}

/* the blocks to the modem, as far as it takes them; each DATA_B3's
   CONF once all of it is written */
void Controller::push_tx()
{
	while (!tx_.empty()) {
		Block &b = tx_.front();
		if (b.conf && state_ != State::Online) {
			drop_tx();
			return;
		}
		if (b.done < b.bytes.size())
			b.done += out_.to_modem(b.bytes.data() + b.done,
						b.bytes.size() - b.done);
		if (b.done < b.bytes.size())
			return;
		if (b.conf) {
			std::vector<u8> p;
			Writer(p).d(NCCI).w(b.handle).w(NO_ERROR);
			send(b.appl, DATA_B3, CONF, b.num, p);
		}
		tx_.pop_front();
	}
}

/* the data blocks not yet written, dropped (their CONFs sent: the
   call is ending) */
void Controller::drop_tx()
{
	std::deque<Block> keep;
	for (Block &b : tx_) {
		if (!b.conf) {
			keep.push_back(std::move(b));
			continue;
		}
		std::vector<u8> p;
		Writer(p).d(NCCI).w(b.handle).w(NO_ERROR);
		send(b.appl, DATA_B3, CONF, b.num, p);
	}
	/* a block half written stays, as the modem has its start */
	tx_.swap(keep);
}

/* a ring of a call in: clear (+CRING: SYNC) or not; offered once its
   caller's number has had time to come */
void Controller::ring(bool clear, u64 now)
{
	ring_last_ = now;
	if (call_ != Call::None || ignored_ || ringing_)
		return;
	ringing_ = true;
	ring_clear_ = clear;
	caller_.clear();
	offer_at_ = now + OFFER_MS;
}

/* CONNECT_IND to each application that listens for the call's CIP */
void Controller::offer(u64 now)
{
	(void)now;
	std::vector<u8> calling;
	if (caller_ == "P")
		calling = { 0x00, 0xA0 };
	else if (!caller_.empty() && caller_ != "O")
		calling = { 0x00, 0x80 };
	std::string num = digits(std::vector<u8>(caller_.begin(),
						 caller_.end()), 0);
	if (calling.size() == 2 && calling[1] == 0x80)
		calling.insert(calling.end(), num.begin(), num.end());
	for (auto &e : apps_) {
		const App &a = e.second;
		if (!a.listening)
			continue;
		u16 cip = 0;
		if (ring_clear_) {
			if (a.cip_mask & (1u << CIP_UDI | 1u))
				cip = CIP_UDI;
		} else {
			for (u16 c : { u16(CIP_AUDIO), u16(16), u16(CIP_SPEECH) })
				if (!cip && a.cip_mask & 1u << c)
					cip = c;
			if (!cip && a.cip_mask & 1u)
				cip = CIP_AUDIO;
		}
		if (!cip)
			continue;
		std::vector<u8> p;
		Writer(p).d(PLCI).w(cip).empty().s(calling).empty().empty()
			.empty().empty().empty().empty().empty();
		offered_.insert(e.first);
		send(e.first, CONNECT, IND, next_num(), p);
	}
	if (offered_.empty()) {
		ignored_ = true;
		ringing_ = false;
		return;
	}
	call_ = Call::Offered;
}

/* see capi_controller.h */
bool Controller::wants_modem_input() const
{
	return rx_.size() < RX_MAX && rx_frames_.size() < 64;
}

/* see capi_controller.h */
void Controller::poll(u64 now)
{
	if (state_ == State::Init && !deadline_) {
		command(init_, State::Init, now, INIT_MS);
	} else if (state_ == State::Init && now >= deadline_) {
		/* no OK: the modem may be online (an earlier run's call) */
		if (++init_tries_ % 2) {
			Block b;
			b.bytes = { '+', '+', '+' };
			tx_.push_back(std::move(b));
			deadline_ = now + INIT_MS;
		} else {
			command("H" + init_, State::Init, now, INIT_MS);
		}
	}
	if (deadline_ && now >= deadline_) {
		switch (state_) {
		case State::Query:
		case State::Setup:
		case State::Hangup:
			finish(CAUSE_UNSPECIFIED, now);
			if (state_ == State::Idle &&
			    (call_ == Call::Out || call_ == Call::Accepted))
				call_ended(CAUSE_UNSPECIFIED);
			break;
		case State::Dialling:
		case State::Answering:
			if (call_ != Call::Clearing) {
				call_ = Call::Clearing;
				clear_reason_ = CAUSE_NO_ANSWER;
				hangup(now);
			} else {
				finish(0, now);
			}
			break;
		case State::Escape:
			escape_at_ = 0;
			command("H", State::Hangup, now, HANGUP_MS);
			break;
		default:
			break;
		}
	}
	if (state_ == State::Escape && escape_at_ && now >= escape_at_) {
		escape_at_ = 0;
		Block b;
		b.bytes = { '+', '+', '+' };
		tx_.push_back(std::move(b));
	}
	if (ringing_ && offer_at_ && now >= offer_at_) {
		offer_at_ = 0;
		offer(now);
	}
	if ((ringing_ || ignored_) && now >= ring_last_ + RINGS_GONE_MS) {
		if (call_ == Call::Offered)
			call_ended(CAUSE_NORMAL);
		ringing_ = ignored_ = false;
	}
	if (!held_.empty() && now >= held_at_ + HELD_MS) {
		std::string h;
		h.swap(held_);
		deliver(reinterpret_cast<const u8 *>(h.data()), h.size());
	}
	if (ending_ && now >= end_by_)
		call_ended(end_reason_);
	push_tx();
}

/* see capi_controller.h */
s64 Controller::next_ms(u64 now) const
{
	s64 next = -1;
	auto at = [&next, now](u64 t) {
		s64 d = t > now ? static_cast<s64>(t - now) : 0;
		if (next < 0 || d < next)
			next = d;
	};
	if (state_ == State::Init && !deadline_)
		at(now);
	if (deadline_)
		at(deadline_);
	if (escape_at_)
		at(escape_at_);
	if (ringing_ && offer_at_)
		at(offer_at_);
	if (ringing_ || ignored_)
		at(ring_last_ + RINGS_GONE_MS);
	if (!held_.empty())
		at(held_at_ + HELD_MS);
	if (ending_)
		at(end_by_);
	if (!tx_.empty())
		at(now + 10);
	return next;
}

} // namespace capi
