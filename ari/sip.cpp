/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    sip.cpp - dmodem-ari's SIP side for T.38 (sip.h)
 *
 *    Asterisk calls a leg's token (its INVITE's Request-URI user); the
 *    answer is the call's G.711 in RTP.  A re-INVITE of Asterisk's with
 *    an image line (T.38's udptl) gets our UDPTL while the leg takes T.38
 *    (accept_t38), else 488; ours offers T.38 the way Asterisk's offers
 *    do, an image line in the audio's place.  Responses to INVITEs go
 *    again until their ACK, our re-INVITE until a response (T1 doubling
 *    to T2, for 64 T1).
 *
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <random>

#include "sip.h"

namespace ari {

namespace {

#define T1_MS   500
#define T2_MS   4000

long long now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* n random hex digits, for tags, branches and Call-IDs */
std::string random_hex(int n)
{
	static std::mutex lock;
	static std::mt19937_64 rng{std::random_device{}()};
	std::lock_guard<std::mutex> lk(lock);
	std::string s;
	for (int k = 0; k < n; k++)
		s += "0123456789abcdef"[rng() & 15];
	return s;
}

std::string lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

/* a SIP message, its headers by their long lower case names */
struct Msg {
	bool request = false;
	std::string method, uri, reason;
	int code = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;

	std::string get(const char *name) const
	{
		for (const auto &h : headers)
			if (h.first == name)
				return h.second;
		return "";
	}
};

/* a header's long name from what the message has (compact forms) */
std::string header_name(const std::string &n)
{
	static const char *compact[][2] = {
		{ "v", "via" }, { "f", "from" }, { "t", "to" },
		{ "i", "call-id" }, { "m", "contact" }, { "l", "content-length" },
		{ "c", "content-type" }, { "k", "supported" },
	};
	std::string l = lower(n);
	for (const auto &c : compact)
		if (l == c[0])
			return c[1];
	return l;
}

bool parse(const std::string &text, Msg *m)
{
	size_t end = text.find("\r\n\r\n"), at, eol;
	std::string head, line;

	if (end == std::string::npos)
		return false;
	head = text.substr(0, end + 2);
	m->body = text.substr(end + 4);
	eol = head.find("\r\n");
	line = head.substr(0, eol);
	if (!line.compare(0, 8, "SIP/2.0 ")) {
		m->code = atoi(line.c_str() + 8);
		if (m->code < 100)
			return false;
		at = line.find(' ', 8);
		m->reason = at == std::string::npos ? "" : line.substr(at + 1);
	} else {
		size_t a = line.find(' '), b = line.rfind(' ');
		if (a == std::string::npos || b <= a ||
		    line.compare(b + 1, std::string::npos, "SIP/2.0"))
			return false;
		m->request = true;
		m->method = line.substr(0, a);
		m->uri = line.substr(a + 1, b - a - 1);
	}
	for (at = eol + 2; at < head.size(); at = eol + 2) {
		eol = head.find("\r\n", at);
		line = head.substr(at, eol - at);
		if ((line[0] == ' ' || line[0] == '\t') && !m->headers.empty()) {
			m->headers.back().second += " " + trim(line);
			continue;
		}
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		m->headers.push_back({ header_name(trim(line.substr(0, colon))),
				       trim(line.substr(colon + 1)) });
	}
	/* the body as long as its Content-Length says */
	std::string cl = m->get("content-length");
	if (!cl.empty() && (size_t)atoi(cl.c_str()) < m->body.size())
		m->body.resize(atoi(cl.c_str()));
	return true;
}

/* a parameter (;name=value) of a header, outside its <...> */
std::string param(const std::string &h, const char *name)
{
	size_t from = h.find('>');
	std::string key = std::string(";") + name + "=";
	std::string l = lower(h);
	size_t at = l.find(key, from == std::string::npos ? 0 : from);
	if (at == std::string::npos)
		return "";
	at += key.size();
	size_t end = h.find_first_of(";,> \t", at);
	return h.substr(at, end == std::string::npos ? std::string::npos :
			end - at);
}

/* a From or To without its tag */
std::string untagged(const std::string &h)
{
	std::string l = lower(h);
	size_t from = h.find('>'), at = l.find(";tag=",
		from == std::string::npos ? 0 : from);
	if (at == std::string::npos)
		return h;
	size_t end = h.find(';', at + 1);
	return h.substr(0, at) + (end == std::string::npos ? "" :
				  h.substr(end));
}

/* the URI of a header (in <...>, else up to its parameters) */
std::string uri_of(const std::string &h)
{
	size_t a = h.find('<');
	if (a != std::string::npos) {
		size_t b = h.find('>', a);
		return h.substr(a + 1, b == std::string::npos ? std::string::npos :
				b - a - 1);
	}
	return h.substr(0, h.find(';'));
}

/* a sip: URI's user */
std::string user_of(const std::string &uri)
{
	size_t a = uri.find(':'), at = uri.find('@');
	if (a == std::string::npos || at == std::string::npos || at < a)
		return "";
	return uri.substr(a + 1, at - a - 1);
}

/* a sip: URI's host and port into *sa; false if it is no IPv4 one */
bool addr_of(const std::string &uri, struct sockaddr_in *sa)
{
	size_t a = uri.find('@');
	a = a == std::string::npos ? uri.find(':') : a;
	if (a == std::string::npos)
		return false;
	std::string hp = uri.substr(a + 1, uri.find_first_of(";>?", a + 1) -
				    a - 1);
	size_t colon = hp.find(':');
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = htons(colon == std::string::npos ? 5060 :
			     atoi(hp.c_str() + colon + 1));
	return inet_pton(AF_INET, hp.substr(0, colon).c_str(),
			 &sa->sin_addr) == 1;
}

/* a response to req: its Vias, From, To (with our tag if it has none),
   Call-ID and CSeq, then extra headers and body */
std::string response(const Msg &req, int code, const char *reason,
		     const std::string &tag, const std::string &extra,
		     const std::string &body)
{
	char first[64];
	std::string r, to = req.get("to");
	snprintf(first, sizeof(first), "SIP/2.0 %d %s\r\n", code, reason);
	r = first;
	for (const auto &h : req.headers)
		if (h.first == "via")
			r += "Via: " + h.second + "\r\n";
	if (param(to, "tag").empty() && !tag.empty())
		to += ";tag=" + tag;
	r += "From: " + req.get("from") + "\r\n";
	r += "To: " + to + "\r\n";
	r += "Call-ID: " + req.get("call-id") + "\r\n";
	r += "CSeq: " + req.get("cseq") + "\r\n";
	r += extra;
	if (!body.empty())
		r += "Content-Type: application/sdp\r\n";
	r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
	return r + body;
}

/* our T.38 media line's attributes */
std::string t38_attrs(int version)
{
	return "a=T38FaxVersion:" + std::to_string(version) + "\r\n"
		"a=T38MaxBitRate:14400\r\n"
		"a=T38FaxRateManagement:transferredTCF\r\n"
		"a=T38FaxMaxBuffer:2000\r\n"
		"a=T38FaxMaxDatagram:400\r\n"
		"a=T38FaxUdpEC:t38UDPRedundancy\r\n";
}

/* an SDP of ours: its session lines, then the media lines */
std::string sdp(const std::string &host, unsigned version,
		const std::string &media)
{
	return "v=0\r\n"
		"o=dmodem 1 " + std::to_string(version) + " IN IP4 " + host +
		"\r\n"
		"s=dmodem\r\n"
		"c=IN IP4 " + host + "\r\n"
		"t=0 0\r\n" + media;
}

/* our audio line: the leg's G.711 on our RTP port (0: the line off) */
std::string audio_line(const SipLeg &leg, int port)
{
	std::string pt = std::to_string(leg.pt);
	return "m=audio " + std::to_string(port) + " RTP/AVP " + pt + "\r\n"
		"a=rtpmap:" + pt + (leg.pt == 8 ? " PCMA" : " PCMU") +
		"/8000\r\n"
		"a=ptime:20\r\n"
		"a=sendrecv\r\n";
}

/* T.38's settings in a media line */
T38Params t38_params(const SdpMedia &m)
{
	T38Params p;
	for (const auto &a : m.attrs) {
		std::string n = lower(a.first);
		if (n == "t38faxversion")
			p.version = atoi(a.second.c_str());
		else if (n == "t38maxbitrate")
			p.max_bitrate = atoi(a.second.c_str());
		else if (n == "t38faxmaxdatagram" || n == "t38maxdatagram")
			p.max_datagram = atoi(a.second.c_str());
		else if (n == "t38faxudpec")
			p.redundancy = lower(a.second) == "t38udpredundancy";
	}
	p.addr = m.addr;
	p.port = m.port;
	return p;
}

} /* namespace */

const SdpMedia *Sdp::find(const std::string &type) const
{
	for (const SdpMedia &m : media)
		if (m.type == type && m.port > 0)
			return &m;
	return nullptr;
}

bool sdp_parse(const std::string &body, Sdp *s)
{
	std::string session_addr;
	size_t at = 0;

	s->media.clear();
	while (at < body.size()) {
		size_t eol = body.find('\n', at);
		std::string line = body.substr(at, eol == std::string::npos ?
					       std::string::npos : eol - at);
		at = eol == std::string::npos ? body.size() : eol + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.size() < 2 || line[1] != '=')
			continue;
		std::string v = line.substr(2);
		switch (line[0]) {
		case 'c': {
			/* IN IP4 <addr> */
			size_t sp = v.rfind(' ');
			std::string a = sp == std::string::npos ? v :
				v.substr(sp + 1);
			if (s->media.empty())
				session_addr = a;
			else
				s->media.back().addr = a;
			break;
		}
		case 'm': {
			SdpMedia m;
			char type[32], proto[32];
			int port, n = 0;
			if (sscanf(v.c_str(), "%31s %d %31s %n", type, &port,
				   proto, &n) < 3)
				return false;
			m.type = type;
			m.port = port;
			m.proto = proto;
			m.addr = session_addr;
			for (size_t k = n; k < v.size();) {
				size_t e = v.find(' ', k);
				std::string f = v.substr(k, e == std::string::npos ?
							 std::string::npos : e - k);
				if (!f.empty())
					m.fmts.push_back(f);
				k = e == std::string::npos ? v.size() : e + 1;
			}
			s->media.push_back(m);
			break;
		}
		case 'a':
			if (!s->media.empty()) {
				size_t colon = v.find(':');
				s->media.back().attrs[v.substr(0, colon)] =
					colon == std::string::npos ? "" :
					v.substr(colon + 1);
			}
			break;
		default:
			break;
		}
	}
	return !s->media.empty();
}

bool SipAgent::start(const std::string &host, int port)
{
	struct sockaddr_in sa = {};
	socklen_t len = sizeof(sa);

	fd_ = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd_ < 0)
		return false;
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
		errno = EINVAL;
		return false;
	}
	if (bind(fd_, (struct sockaddr *)&sa, sizeof(sa)) ||
	    getsockname(fd_, (struct sockaddr *)&sa, &len))
		return false;
	host_ = host;
	port_ = ntohs(sa.sin_port);
	thread_ = std::thread(&SipAgent::run, this);
	thread_.detach();
	return true;
}

std::shared_ptr<SipLeg> SipAgent::expect(const std::string &token, int pt,
					  int rtp_port, int udptl_port)
{
	auto leg = std::make_shared<SipLeg>();
	leg->token = token;
	leg->pt = pt;
	leg->rtp_port = rtp_port;
	leg->udptl_port = udptl_port;
	leg->local_tag = random_hex(16);
	std::lock_guard<std::mutex> lk(lock_);
	legs_.push_back(leg);
	return leg;
}

void SipAgent::forget(const std::shared_ptr<SipLeg> &leg)
{
	std::lock_guard<std::mutex> lk(lock_);
	if (leg->gone || leg->call_id.empty())
		legs_.erase(std::remove(legs_.begin(), legs_.end(), leg),
			    legs_.end());
	else
		leg->forget_at = now_ms() + 64 * T1_MS;
}

bool SipAgent::wait_up(const std::shared_ptr<SipLeg> &leg, int ms)
{
	std::unique_lock<std::mutex> lk(lock_);
	return changed_.wait_for(lk, std::chrono::milliseconds(ms),
				 [&] { return leg->up || leg->gone; }) &&
		leg->up && !leg->gone;
}

SipLeg SipAgent::get(const std::shared_ptr<SipLeg> &leg)
{
	std::lock_guard<std::mutex> lk(lock_);
	return *leg;
}

void SipAgent::accept_t38(const std::shared_ptr<SipLeg> &leg, bool on)
{
	std::lock_guard<std::mutex> lk(lock_);
	leg->accept_t38 = on;
}

bool SipAgent::ask_t38(const std::shared_ptr<SipLeg> &leg)
{
	std::lock_guard<std::mutex> lk(lock_);
	SipLeg &l = *leg;
	if (!l.up || l.gone || l.asking || l.t38 == T38_ON)
		return false;
	l.request_branch = "z9hG4bK" + random_hex(20);
	l.request_cseq = ++l.local_cseq;
	std::string body = sdp(host_, ++l.sdp_version,
			       "m=image " + std::to_string(l.udptl_port) +
			       " udptl t38\r\n" + t38_attrs(0));
	l.request = "INVITE " + l.target + " SIP/2.0\r\n"
		"Via: SIP/2.0/UDP " + host_ + ":" + std::to_string(port_) +
		";branch=" + l.request_branch + ";rport\r\n"
		"Max-Forwards: 70\r\n"
		"From: " + l.local_uri + ";tag=" + l.local_tag + "\r\n"
		"To: " + l.remote_uri + ";tag=" + l.remote_tag + "\r\n"
		"Call-ID: " + l.call_id + "\r\n"
		"CSeq: " + std::to_string(l.request_cseq) + " INVITE\r\n"
		"Contact: <sip:dmodem@" + host_ + ":" + std::to_string(port_) +
		">\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
		body;
	l.asking = true;
	l.t38 = T38_ASKED;
	l.req_ms = T1_MS;
	l.req_at = now_ms() + l.req_ms;
	l.req_end = now_ms() + 64 * T1_MS;
	send_to(l.peer, l.request);
	return true;
}

void SipAgent::send_to(const struct sockaddr_in &to, const std::string &msg)
{
	sendto(fd_, msg.data(), msg.size(), 0, (const struct sockaddr *)&to,
	       sizeof(to));
}

void SipAgent::run()
{
	char buf[8192];
	for (;;) {
		struct pollfd p = { fd_, POLLIN, 0 };
		int r = poll(&p, 1, 50);
		std::lock_guard<std::mutex> lk(lock_);
		if (r > 0) {
			struct sockaddr_in from;
			socklen_t len = sizeof(from);
			ssize_t n = recvfrom(fd_, buf, sizeof(buf) - 1, 0,
					     (struct sockaddr *)&from, &len);
			if (n > 0)
				received(std::string(buf, n), from);
		}
		timers();
	}
}

/* the responses and requests that go again, and those that give up;
   the forgotten legs whose BYE never came */
void SipAgent::timers()
{
	long long now = now_ms();
	legs_.erase(std::remove_if(legs_.begin(), legs_.end(),
				   [now](const std::shared_ptr<SipLeg> &l) {
					   return l->forget_at &&
						   now >= l->forget_at;
				   }), legs_.end());
	for (auto &leg : legs_) {
		SipLeg &l = *leg;
		if (!l.acked && now >= l.retx_at) {
			if (now >= l.retx_end) {
				l.acked = true;         /* (no ACK came) */
			} else {
				send_to(l.response_to, l.response);
				l.retx_ms = std::min(l.retx_ms * 2, (long long)T2_MS);
				l.retx_at = now + l.retx_ms;
			}
		}
		if (l.asking && now >= l.req_end) {
			l.asking = false;
			l.t38 = T38_REFUSED;
			changed_.notify_all();
		} else if (l.asking && l.req_ms && now >= l.req_at) {
			send_to(l.peer, l.request);
			l.req_ms = std::min(l.req_ms * 2, (long long)T2_MS);
			l.req_at = now + l.req_ms;
		}
	}
}

void SipAgent::received(const std::string &text, const struct sockaddr_in &from)
{
	Msg m;
	std::shared_ptr<SipLeg> leg;
	std::string call_id;

	if (!parse(text, &m))
		return;
	call_id = m.get("call-id");
	for (auto &l : legs_)
		if (!l->call_id.empty() && l->call_id == call_id)
			leg = l;
	std::string allow = "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, INFO, "
		"UPDATE\r\n";
	std::string contact = "Contact: <sip:dmodem@" + host_ + ":" +
		std::to_string(port_) + ">\r\n";

	if (!m.request) {
		/* a response to our re-INVITE */
		if (!leg || atoi(m.get("cseq").c_str()) !=
		    (int)leg->request_cseq || m.get("cseq").find("INVITE") ==
		    std::string::npos)
			return;
		SipLeg &l = *leg;
		if (m.code < 200) {
			l.req_ms = 0;           /* (it is on its way) */
			return;
		}
		/* its ACK: for a 2xx a request of its own, else in the
		   INVITE's transaction */
		std::string branch = m.code < 300 ? "z9hG4bK" + random_hex(20) :
			l.request_branch;
		std::string ack = "ACK " + l.target + " SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + host_ + ":" +
			std::to_string(port_) + ";branch=" + branch + ";rport\r\n"
			"Max-Forwards: 70\r\n"
			"From: " + l.local_uri + ";tag=" + l.local_tag + "\r\n"
			"To: " + m.get("to") + "\r\n"
			"Call-ID: " + l.call_id + "\r\n"
			"CSeq: " + std::to_string(l.request_cseq) + " ACK\r\n"
			"Content-Length: 0\r\n\r\n";
		send_to(l.peer, ack);
		if (!l.asking)
			return;                 /* (a 2xx again) */
		l.asking = false;
		Sdp s;
		const SdpMedia *image = nullptr;
		if (m.code < 300 && sdp_parse(m.body, &s))
			image = s.find("image");
		if (image) {
			l.t38 = T38_ON;
			l.t38_peer = t38_params(*image);
		} else {
			l.t38 = T38_REFUSED;
		}
		changed_.notify_all();
		return;
	}

	if (m.method == "INVITE" && !leg) {
		/* a call to one of our legs */
		std::string token = user_of(m.uri);
		for (auto &l : legs_)
			if (l->call_id.empty() && l->token == token)
				leg = l;
		if (!leg) {
			send_to(from, response(m, 404, "Not Found",
					       random_hex(16), "", ""));
			return;
		}
		SipLeg &l = *leg;
		Sdp s;
		const SdpMedia *audio = nullptr;
		if (sdp_parse(m.body, &s))
			audio = s.find("audio");
		if (audio && std::find(audio->fmts.begin(), audio->fmts.end(),
				       std::to_string(l.pt)) == audio->fmts.end()) {
			/* not the law we wanted: the other one, if offered */
			int other = l.pt == 8 ? 0 : 8;
			if (std::find(audio->fmts.begin(), audio->fmts.end(),
				      std::to_string(other)) != audio->fmts.end())
				l.pt = other;
			else
				audio = nullptr;
		}
		if (!audio) {
			send_to(from, response(m, 488, "Not Acceptable Here",
					       l.local_tag, "", ""));
			return;
		}
		l.call_id = call_id;
		l.remote_tag = param(m.get("from"), "tag");
		l.remote_uri = untagged(m.get("from"));
		l.local_uri = untagged(m.get("to"));
		l.target = uri_of(m.get("contact"));
		if (l.target.empty() || !addr_of(l.target, &l.peer))
			l.peer = from;
		l.rtp_addr = audio->addr;
		l.rtp_peer_port = audio->port;
		l.rx_cseq = atoi(m.get("cseq").c_str());
		l.response = response(m, 200, "OK", l.local_tag,
				      contact + allow,
				      sdp(host_, l.sdp_version,
					  audio_line(l, l.rtp_port)));
		l.response_to = from;
		l.acked = false;
		l.retx_ms = T1_MS;
		l.retx_at = now_ms() + T1_MS;
		l.retx_end = now_ms() + 64 * T1_MS;
		l.up = true;
		send_to(from, l.response);
		changed_.notify_all();
		return;
	}
	if (!leg) {
		if (m.method == "OPTIONS")
			send_to(from, response(m, 200, "OK", random_hex(16),
					       allow, ""));
		else if (m.method != "ACK")
			send_to(from, response(m, 481,
					       "Call/Transaction Does Not Exist",
					       random_hex(16), "", ""));
		return;
	}

	SipLeg &l = *leg;
	unsigned cseq = atoi(m.get("cseq").c_str());
	if (m.method == "ACK") {
		if (cseq == l.rx_cseq)
			l.acked = true;
	} else if (m.method == "INVITE") {
		if (cseq == l.rx_cseq) {
			send_to(from, l.response);      /* it again */
			return;
		}
		if (l.asking) {
			/* ours is on its way: theirs again later */
			l.rx_cseq = cseq;
			l.response = response(m, 491, "Request Pending",
					      l.local_tag, "", "");
		} else {
			/* a re-INVITE: T.38 (an image line) if the leg takes
			   it, else 488; audio again */
			Sdp s;
			const SdpMedia *image = nullptr, *audio = nullptr;
			if (sdp_parse(m.body, &s)) {
				image = s.find("image");
				audio = s.find("audio");
			}
			l.rx_cseq = cseq;
			if (image && l.accept_t38) {
				T38Params p = t38_params(*image);
				std::string media;
				int version = std::min(p.version, 3);
				for (const SdpMedia &sm : s.media)
					media += sm.type == "image" ?
						"m=image " +
						std::to_string(l.udptl_port) +
						" udptl t38\r\n" +
						t38_attrs(version) :
						audio_line(l, 0);
				p.version = version;
				l.t38 = T38_ON;
				l.t38_peer = p;
				l.response = response(m, 200, "OK", l.local_tag,
						      contact + allow,
						      sdp(host_, ++l.sdp_version,
							  media));
			} else if (image || !audio) {
				l.response = response(m, 488,
						      "Not Acceptable Here",
						      l.local_tag, "", "");
			} else {
				l.rtp_addr = audio->addr;
				l.rtp_peer_port = audio->port;
				if (l.t38 == T38_ON)
					l.t38 = T38_NONE;
				l.response = response(m, 200, "OK", l.local_tag,
						      contact + allow,
						      sdp(host_, ++l.sdp_version,
							  audio_line(l,
								     l.rtp_port)));
			}
		}
		l.response_to = from;
		l.acked = false;
		l.retx_ms = T1_MS;
		l.retx_at = now_ms() + T1_MS;
		l.retx_end = now_ms() + 64 * T1_MS;
		send_to(from, l.response);
		changed_.notify_all();
	} else if (m.method == "BYE") {
		send_to(from, response(m, 200, "OK", l.local_tag, "", ""));
		l.gone = true;
		l.acked = true;
		l.asking = false;
		if (l.forget_at)
			legs_.erase(std::remove(legs_.begin(), legs_.end(), leg),
				    legs_.end());
		changed_.notify_all();
	} else {
		/* CANCEL (we answered at once), OPTIONS, INFO, UPDATE: OK */
		send_to(from, response(m, 200, "OK", l.local_tag, allow, ""));
	}
}

} /* namespace ari */
