/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 */
/*
 *
 *    sip.cpp - dmodemd's telephone line: a SIP user agent on libre
 *              (baresip's re), with SDP, RTP and UDPTL of its own
 *              (sip.h)
 *
 *    One call at a time, the current call.  libre runs in a thread of
 *    ours (the SIP thread), which alone has the SIP side: the modem's
 *    requests (dial, answer) come to it through libre's queue, it writes
 *    the RING, CANCEL, T38 and AUDIO lines, and it alone has the current
 *    call.
 *
 *    A call's audio has a thread of its own on a clock of 20 ms: on each
 *    tick a frame from the jitter buffer to the modem's socket, the
 *    modem's answer to the frame before from it, and that in RTP to the
 *    far end.  G.711 both ways, the modem's samples G.711's own values,
 *    so that its codes pass through as they are.  The jitter buffer is
 *    fixed (60 ms) and drops nothing to shrink (a slip the modem can't
 *    take): it starts over only when the far end does (another source, a
 *    jump in time), when its backlog passes 2 s, or when it has run dry
 *    with the far end's packets late (its clock slower than ours).
 *
 *    Telephone-events (RFC 4733), offered and taken on every call: the
 *    modem's hook flash (sip::flash()) goes as event 16, in place of the
 *    audio while it lasts, and the far end's DTMF digits and RFC 4734's
 *    tones (answer tones, CNG, CT, ...) sent as events reach the modem as
 *    their tones (rtp_events.h).
 *
 *    T.38: an offer of it (a re-INVITE with an image line) is answered at
 *    once while the modem is in fax mode, our image line in its place,
 *    the other lines off.  Our own ask, a second after we answered a fax
 *    call, has our image line in the audio line's place.  On T.38 the
 *    audio stops (its thread stays, for the call's end), and the modem's
 *    fax engine has the call's UDPTL socket (t38()).
 *
 */

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

/* libre's declarations in a namespace of their own: its struct sip
   would be the namespace sip's name (the system headers it has are
   above) */
namespace libre {
#include <re.h>
}

#include "g711.h"
#include "line_recorder.h"
#include "rtp_events.h"
#include "sip.h"

namespace sip {

namespace {

using namespace libre;

/* the modem's audio: 8000 Hz, mono, 16 bit, frames of 20 ms */
const unsigned RATE = 8000;
const unsigned SAMPLES = RATE / 50;
const size_t FRAME = SAMPLES * 2;
const long TICK_NS = 1000000000L / 50;

/* the jitter buffer, fixed (samples): its delay, its longest backlog */
const int32_t JB_DELAY = 60 * RATE / 1000;
const int32_t JB_MAX = 2000 * RATE / 1000;

/* late packets in a row, the buffer dry, for it to start over (a
   moment's lateness only loses their time) */
const unsigned LATE_RUN = 5;

/* the audio thread's clock: behind by more than this (ns), it starts
   over from now */
const long LATE_NS = 200000000L;

/* a ringing call's RING lines, the look at the current call (ms) */
const int RING_MS = 3000;
const int POLL_MS = 200;

/* T.38: our ask, after answering a fax call (ms): one sooner may cross
   the call's ACK */
const int T38_ASK_MS = 1000;

/* the registration's lifetime (s); the longest stop() waits for the
   last transactions (ms) */
const uint32_t REG_EXPIRES = 300;
const int QUIT_MS = 2000;

/* the SIP thread's queue */
enum { MQ_DIAL, MQ_ANSWER, MQ_MODEM_GONE, MQ_QUIT };

/* RFC 4733 telephone-events: the payload type of ours in an offer; the
   events (hook flash), the longest one (its 16 bit duration, in
   samples), the copies of its last packet */
const int TE_PT = 101;
const unsigned EV_FLASH = 16;
const unsigned EV_MAX = 65535;
const int EV_ENDS = 3;

/* start()'s copy of the configuration */
Config cfg_;
std::string user_, password_, domain_;
int log_level = 4;

dmd::LineRecorder rec;
int ctl[2] = { -1, -1 };        /* our lines: written to [0] */
std::atomic<int> law_{0};
/* the current call's telephone-event payload type (the far end's), -1
   if it has none; a hook flash asked for (ms), 0 if none */
std::atomic<int> te_pt_{-1};
std::atomic<unsigned> flash_ms_{0};
std::atomic<bool> fax_{false};
std::atomic<bool> running{false};       /* the SIP thread takes requests */
std::thread sip_thread;

/* the SIP thread's start: 0 under way, 1 up, -1 failed */
std::mutex start_mu;
std::condition_variable start_cv;
int start_state;


/*
 *    helpers
 */

/* a line of the log (stderr) at level (1 errors, 3 the calls, 4 more,
   with the SIP messages), if SIP_LOG_LEVEL has it */
void say(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void say(int level, const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	va_list ap;
	std::vector<char> buf(256);
	int n;

	if (level > log_level)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf.data(), buf.size(), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= buf.size()) {
		buf.resize(n + 1);
		va_start(ap, fmt);
		vsnprintf(buf.data(), buf.size(), fmt, ap);
		va_end(ap);
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	fprintf(stderr, "%02d:%02d:%02d.%03ld sip: %s\n", tm.tm_hour,
		tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, buf.data());
}

std::string str(const struct pl &p)
{
	return std::string(p.p ? p.p : "", p.p ? p.l : 0);
}

std::string lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

/* a's address, without its port */
std::string ip_of(const struct sa *a)
{
	char buf[64] = "";
	if (sa_ntop(a, buf, sizeof(buf)))
		return std::string();
	return buf;
}

/* a's address and port */
std::string addr_of(const struct sa *a)
{
	std::string ip = ip_of(a);
	if (ip.find(':') != std::string::npos)
		ip = "[" + ip + "]";
	return ip + ":" + std::to_string(sa_port(a));
}

/* n bytes from fd into buf, in as many reads as it takes; false if it
   fails or ends before */
bool read_all(int fd, void *buf, size_t n)
{
	char *p = static_cast<char *>(buf);
	while (n) {
		ssize_t r = read(fd, p, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= r;
	}
	return true;
}

/* n bytes of buf to the socket fd, all of them; false if that fails */
bool write_all(int fd, const void *buf, size_t n)
{
	const char *p = static_cast<const char *>(buf);
	while (n) {
		ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= r;
	}
	return true;
}

/* the modem has closed its end of fd (seen without reading from it) */
bool peer_closed(int fd)
{
	struct pollfd p = { fd, POLLRDHUP, 0 };
	return poll(&p, 1, 0) > 0 &&
		(p.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL));
}

/* (SIP thread) a line to main.cpp's control descriptor (RING, CANCEL,
   T38, AUDIO) */
void ctl_line(const char *s)
{
	say(4, "control: %.*s", (int)strlen(s) - 1, s);
	if (!write_all(ctl[0], s, strlen(s)))
		say(1, "control socket: %s", strerror(errno));
}

/* a UDP socket on a port of its own (into *port), both IPv6 and IPv4 if
   it can (its family into *family); -1 if it can't be had */
int open_udp(int *port, int *family)
{
	for (int af : { AF_INET6, AF_INET }) {
		struct sockaddr_storage a = {};
		socklen_t len = af == AF_INET6 ? sizeof(struct sockaddr_in6) :
			sizeof(struct sockaddr_in);
		int off = 0;
		int fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		if (fd < 0)
			continue;
		a.ss_family = (sa_family_t)af;
		if ((af == AF_INET6 &&
		     setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off,
				sizeof(off)) < 0) ||
		    bind(fd, reinterpret_cast<struct sockaddr *>(&a), len) < 0 ||
		    getsockname(fd, reinterpret_cast<struct sockaddr *>(&a),
				&len) < 0) {
			close(fd);
			continue;
		}
		*port = ntohs(af == AF_INET6 ?
			reinterpret_cast<struct sockaddr_in6 *>(&a)->sin6_port :
			reinterpret_cast<struct sockaddr_in *>(&a)->sin_port);
		*family = af;
		return fd;
	}
	say(1, "a UDP socket: %s", strerror(errno));
	return -1;
}

/* the socket address of addr:port for a socket of family (an IPv4
   address mapped for an IPv6 one); false if addr is none of either */
bool sockaddr_of(const std::string &addr, int port, int family,
		 struct sockaddr_storage *a, socklen_t *len)
{
	struct sockaddr_in *a4 = reinterpret_cast<struct sockaddr_in *>(a);
	struct sockaddr_in6 *a6 = reinterpret_cast<struct sockaddr_in6 *>(a);
	struct in_addr in4;

	memset(a, 0, sizeof(*a));
	if (inet_pton(AF_INET, addr.c_str(), &in4) == 1) {
		if (family == AF_INET) {
			a4->sin_family = AF_INET;
			a4->sin_addr = in4;
			a4->sin_port = htons((uint16_t)port);
			*len = sizeof(*a4);
			return true;
		}
		a6->sin6_family = AF_INET6;
		a6->sin6_addr.s6_addr[10] = 0xff;
		a6->sin6_addr.s6_addr[11] = 0xff;
		memcpy(&a6->sin6_addr.s6_addr[12], &in4, 4);
		a6->sin6_port = htons((uint16_t)port);
		*len = sizeof(*a6);
		return true;
	}
	if (family == AF_INET6 &&
	    inet_pton(AF_INET6, addr.c_str(), &a6->sin6_addr) == 1) {
		a6->sin6_family = AF_INET6;
		a6->sin6_port = htons((uint16_t)port);
		*len = sizeof(*a6);
		return true;
	}
	return false;
}


/*
 *    SDP
 */

/* an SDP's media line */
struct SdpMedia {
	std::string type;               /* audio, image */
	int port = 0;
	std::string proto;              /* RTP/AVP, udptl */
	std::vector<std::string> fmts;
	std::string addr;               /* its c= (or the session's) */
	std::vector<std::pair<std::string, std::string>> attrs;

	/* the value of its attribute name (any case), NULL if none */
	const std::string *attr(const char *name) const
	{
		for (const auto &a : attrs)
			if (!strcasecmp(a.first.c_str(), name))
				return &a.second;
		return nullptr;
	}
};

/* an SDP: the session's attributes, the media lines in their order */
struct Sdp {
	std::vector<std::pair<std::string, std::string>> attrs;
	std::vector<SdpMedia> media;
};

/* the SDP of body (n octets) into s; false if it has no media line */
bool sdp_parse(const char *body, size_t n, Sdp *s)
{
	std::string text(body, n), session_addr;
	size_t at = 0;

	*s = Sdp();
	while (at < text.size()) {
		size_t eol = text.find('\n', at);
		std::string line = text.substr(at, eol == std::string::npos ?
					       std::string::npos : eol - at);
		at = eol == std::string::npos ? text.size() : eol + 1;
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
			a = a.substr(0, a.find('/'));
			if (s->media.empty())
				session_addr = a;
			else
				s->media.back().addr = a;
			break;
		}
		case 'm': {
			SdpMedia m;
			char type[32], proto[32];
			int port, k = 0;
			if (sscanf(v.c_str(), "%31s %d%*[^ ] %31s %n", type,
				   &port, proto, &k) < 3 &&
			    sscanf(v.c_str(), "%31s %d %31s %n", type, &port,
				   proto, &k) < 3)
				return false;
			m.type = type;
			m.port = port;
			m.proto = proto;
			m.addr = session_addr;
			for (size_t i = k; i < v.size();) {
				size_t e = v.find(' ', i);
				std::string f = v.substr(i, e == std::string::npos ?
							 std::string::npos : e - i);
				if (!f.empty())
					m.fmts.push_back(f);
				i = e == std::string::npos ? v.size() : e + 1;
			}
			s->media.push_back(m);
			break;
		}
		case 'a': {
			size_t colon = v.find(':');
			auto a = std::make_pair(v.substr(0, colon),
				colon == std::string::npos ? std::string() :
				v.substr(colon + 1));
			if (s->media.empty())
				s->attrs.push_back(a);
			else
				s->media.back().attrs.push_back(a);
			break;
		}
		default:
			break;
		}
	}
	return !s->media.empty();
}

/* the SDP of msg into s; false if it has none, or none we can read */
bool sdp_of(const struct sip_msg *msg, Sdp *s)
{
	if (!mbuf_get_left(msg->mb) ||
	    !msg_ctype_cmp(&msg->ctyp, "application", "sdp"))
		return false;
	return sdp_parse(reinterpret_cast<const char *>(mbuf_buf(msg->mb)),
			 mbuf_get_left(msg->mb), s);
}

/* m is a T.38 line (an image line on UDPTL) */
bool is_t38(const SdpMedia &m)
{
	return lower(m.type) == "image" && lower(m.proto) == "udptl";
}

/* the first line of s on of type audio or (t38) T.38, -1 if none */
int line_of(const Sdp &s, bool t38)
{
	for (size_t i = 0; i < s.media.size(); i++)
		if (s.media[i].port > 0 &&
		    (t38 ? is_t38(s.media[i]) :
		     lower(s.media[i].type) == "audio"))
			return (int)i;
	return -1;
}

/* the first G.711 format of audio line m, its law into *law (1 mu-law,
   2 A-law); -1 if it has none */
int g711_pt(const SdpMedia &m, int *law)
{
	for (const std::string &f : m.fmts) {
		char *end;
		long pt = strtol(f.c_str(), &end, 10);
		std::string enc;

		if (*end || pt < 0 || pt > 127)
			continue;
		for (const auto &a : m.attrs) {
			if (strcasecmp(a.first.c_str(), "rtpmap"))
				continue;
			size_t sp = a.second.find(' ');
			if (sp != std::string::npos &&
			    a.second.substr(0, sp) == f)
				enc = lower(a.second.substr(sp + 1));
		}
		if (enc.empty())
			enc = pt == 0 ? "pcmu/8000" : pt == 8 ? "pcma/8000" : "";
		if (enc.compare(0, 9, "pcmu/8000") == 0)
			*law = 1;
		else if (enc.compare(0, 9, "pcma/8000") == 0)
			*law = 2;
		else
			continue;
		return (int)pt;
	}
	return -1;
}

/* the telephone-event format of audio line m, -1 if it has none */
int te_pt(const SdpMedia &m)
{
	for (const std::string &f : m.fmts) {
		char *end;
		long pt = strtol(f.c_str(), &end, 10);

		if (*end || pt < 96 || pt > 127)
			continue;
		for (const auto &a : m.attrs) {
			if (strcasecmp(a.first.c_str(), "rtpmap"))
				continue;
			size_t sp = a.second.find(' ');
			if (sp != std::string::npos &&
			    a.second.substr(0, sp) == f &&
			    lower(a.second.substr(sp + 1)).compare(
				    0, 20, "telephone-event/8000") == 0)
				return (int)pt;
		}
	}
	return -1;
}

/* the direction of line m of s: its own, else the session's */
std::string direction(const Sdp &s, const SdpMedia &m)
{
	static const char *const dirs[] = { "sendrecv", "sendonly",
					    "recvonly", "inactive" };
	for (const char *d : dirs)
		if (m.attr(d))
			return d;
	for (const char *d : dirs)
		for (const auto &a : s.attrs)
			if (!strcasecmp(a.first.c_str(), d))
				return d;
	return "sendrecv";
}

/* the far end's T.38, from its image line */
struct Peer {
	std::string addr;
	int port = 0;
	int version = 0;
	int max_datagram = 0;
	bool redundancy = false;
};

/* the T.38 settings of image line m */
Peer t38_params(const SdpMedia &m)
{
	Peer p;

	p.addr = m.addr;
	p.port = m.port;
	for (const auto &a : m.attrs) {
		std::string n = lower(a.first);
		if (n == "t38faxversion")
			p.version = atoi(a.second.c_str());
		else if (n == "t38faxmaxdatagram" || n == "t38maxdatagram")
			p.max_datagram = atoi(a.second.c_str());
		else if (n == "t38faxudpec")
			p.redundancy = lower(a.second) == "t38udpredundancy";
	}
	return p;
}

/* a media line of ours: our audio (RTP), our T.38 (UDPTL), or another
   line off */
struct Line {
	enum Kind { AUDIO, IMAGE, OFF } kind = AUDIO;
	int pt = -1;                    /* audio: the format agreed, -1 if
					   none yet (both offered) */
	int law = 0;
	int te = -1;                    /* audio: telephone-event's format
					   agreed, -1 if none */
	std::string dir = "sendrecv";
	int version = 0;                /* image: T38FaxVersion */
	std::string type, proto, fmts;  /* off: the far end's line */
};

/* our audio line in answer to line m of s (with a G.711 format) */
Line audio_answer(const Sdp &s, const SdpMedia &m)
{
	Line l;
	std::string d = direction(s, m);

	l.pt = g711_pt(m, &l.law);
	l.te = te_pt(m);
	l.dir = d == "sendonly" ? "recvonly" : d == "recvonly" ? "sendonly" :
		d == "inactive" ? "inactive" : "sendrecv";
	return l;
}

Line image_line(int version)
{
	Line l;
	l.kind = Line::IMAGE;
	l.version = version;
	return l;
}

/* our answer to s: l in line i's place, the other lines off */
std::vector<Line> mirror(const Sdp &s, size_t i, const Line &l)
{
	std::vector<Line> v;

	for (size_t k = 0; k < s.media.size(); k++) {
		if (k == i) {
			v.push_back(l);
			continue;
		}
		Line off;
		off.kind = Line::OFF;
		off.type = s.media[k].type;
		off.proto = s.media[k].proto;
		for (const std::string &f : s.media[k].fmts)
			off.fmts += (off.fmts.empty() ? "" : " ") + f;
		if (off.fmts.empty())
			off.fmts = "0";
		v.push_back(off);
	}
	return v;
}

/* the index of the first of lines of kind, -1 if none */
int find(const std::vector<Line> &lines, Line::Kind kind)
{
	for (size_t i = 0; i < lines.size(); i++)
		if (lines[i].kind == kind)
			return (int)i;
	return -1;
}

/* our T.38 line's attributes: version, with redundancy, datagrams of
   400 octets */
std::string t38_attrs(int version)
{
	return "a=T38FaxVersion:" + std::to_string(version) + "\r\n"
		"a=T38MaxBitRate:14400\r\n"
		"a=T38FaxRateManagement:transferredTCF\r\n"
		"a=T38FaxMaxBuffer:2000\r\n"
		"a=T38FaxMaxDatagram:400\r\n"
		"a=T38FaxUdpEC:t38UDPRedundancy\r\n";
}

/* an mbuf of s, at its start; NULL if there is no memory for it */
struct mbuf *mbuf_of(const std::string &s)
{
	struct mbuf *mb = mbuf_alloc(s.size() + 1);

	if (!mb)
		return nullptr;
	if (mbuf_write_mem(mb, reinterpret_cast<const uint8_t *>(s.data()),
			   s.size())) {
		mem_deref(mb);
		return nullptr;
	}
	mbuf_set_pos(mb, 0);
	return mb;
}


/*
 *    the audio
 */

/* the far end's samples by their RTP timestamps, played out at a fixed
   delay behind the first (the audio thread's) */
class Jitter {
public:
	/* empty, to start over with the next packet */
	void reset() { started_ = false; }

	/* a packet of n G.711 octets (alaw: A-law) at ts from source ssrc;
	   why the buffer started over for it, or NULL */
	const char *put(uint32_t ssrc, uint32_t ts, const uint8_t *p,
			size_t n, bool alaw)
	{
		const char *why = nullptr;
		int32_t d;

		if (!n || n > (size_t)JB_MAX)
			return nullptr;
		packets++;
		if (!started_ || ssrc != ssrc_) {
			if (started_)
				why = "another source";
			restart(ssrc, ts);
		}
		d = (int32_t)(ts - play_);
		if (d + (int32_t)n > JB_MAX || d < -JB_MAX) {
			/* a jump, or the backlog too long */
			why = "a jump in time";
			restart(ssrc, ts);
		} else if (d + (int32_t)n <= 0) {
			/* late: dropped (its time played as silence),
			   unless they have been late for a while, the
			   buffer dry (the far end's clock slower than
			   ours) */
			late++;
			late_max = std::max(late_max, (unsigned long)-d);
			if ((int32_t)(end_ - play_) > 0 || ++late_run < LATE_RUN)
				return nullptr;
			why = "run dry";
			restart(ssrc, ts);
		}
		late_run = 0;
		if (why)
			restarts++;
		d = (int32_t)(ts - play_);
		for (size_t k = 0; k < n; k++) {
			uint32_t i;
			if (d + (int32_t)k < 0)
				continue;
			i = (ts + (uint32_t)k) & MASK;
			buf_[i] = (int16_t)dsp::g711::decode(alaw, p[k]);
			valid_[i] = 1;
		}
		if ((int32_t)(ts + (uint32_t)n - end_) > 0)
			end_ = ts + (uint32_t)n;
		return why;
	}

	/* the packets put, those late and the latest of them (samples),
	   the times it started over */
	unsigned long packets = 0, late = 0, late_max = 0, restarts = 0;

	/* the next n samples to play (silence where none came) */
	void get(int16_t *out, size_t n)
	{
		for (size_t k = 0; k < n; k++) {
			uint32_t i = (play_ + (uint32_t)k) & MASK;
			out[k] = started_ && valid_[i] ? buf_[i] : 0;
			valid_[i] = 0;
		}
		if (started_)
			play_ += (uint32_t)n;
	}

private:
	static const uint32_t SIZE = 32768, MASK = SIZE - 1;
	int16_t buf_[SIZE];
	uint8_t valid_[SIZE] = {};
	bool started_ = false;
	unsigned late_run = 0;  /* late packets in a row, the buffer dry */
	uint32_t ssrc_ = 0;
	uint32_t play_ = 0;     /* the timestamp to play next */
	uint32_t end_ = 0;      /* the end of the latest packet */

	void restart(uint32_t ssrc, uint32_t ts)
	{
		memset(valid_, 0, sizeof(valid_));
		started_ = true;
		ssrc_ = ssrc;
		play_ = ts - (uint32_t)JB_DELAY;
		end_ = ts;
	}
};

/* a call's audio: the modem's socket and the call's RTP, a thread on
   the clock between them.  Until active(), and while on T.38, no
   frames pass (the thread looks for the modem's hanging up); a failed
   socket is told to the SIP thread (MQ_MODEM_GONE) */
class Media {
public:
	Media(int modem, int rtp, int family, unsigned serial, struct mqueue *mq)
		: modem_(modem), rtp_(rtp), family_(family), serial_(serial),
		  mq_(mq), seq_(rand_u16()), ts_(rand_u32()), ssrc_(rand_u32())
	{
	}
	~Media() { stop(); }
	Media(const Media &) = delete;
	Media &operator=(const Media &) = delete;

	int family() const { return family_; }

	/* the thread started; false if it can't be */
	bool start()
	{
		try {
			thread_ = std::thread(&Media::run, this);
		} catch (const std::system_error &e) {
			say(1, "the audio thread: %s", e.what());
			return false;
		}
		return true;
	}

	/* the thread ended, the modem's socket shut down first (a read the
	   thread waits in returns, and the modem sees the call end) and
	   closed */
	void stop()
	{
		quit_ = true;
		if (modem_ >= 0)
			shutdown(modem_, SHUT_RDWR);
		if (thread_.joinable()) {
			thread_.join();
			say(4, "call %u: audio: %lu packets in, %lu late "
			    "(by %lu ms at the most), %lu restarts", serial_,
			    jb_.packets, jb_.late, jb_.late_max / (RATE / 1000),
			    jb_.restarts);
		}
		if (modem_ >= 0) {
			say(4, "audio socket %d closed", modem_);
			close(modem_);
		}
		modem_ = -1;
	}

	/* the far end's RTP at a (len), in payload type pt of law, its
	   telephone-events in te (-1: none); send: whether it is to have
	   ours */
	void remote(const struct sockaddr_storage &a, socklen_t len, int pt,
		    int law, int te, bool send)
	{
		std::lock_guard<std::mutex> lk(mu_);
		peer_ = a;
		peer_len_ = len;
		pt_ = pt;
		law_ = law;
		te_ = te;
		send_ = send;
	}

	/* frames pass (on) or not */
	void active(bool on)
	{
		std::lock_guard<std::mutex> lk(mu_);
		active_ = on;
	}

private:
	int modem_, rtp_, family_;
	unsigned serial_;
	struct mqueue *mq_;
	std::thread thread_;
	std::atomic<bool> quit_{false};

	/* (under mu_) as remote() and active() have it */
	std::mutex mu_;
	bool active_ = false, send_ = false;
	struct sockaddr_storage peer_ = {};
	socklen_t peer_len_ = 0;
	int pt_ = 0, law_ = 1, te_ = -1;

	/* the thread's own */
	Jitter jb_;
	uint16_t seq_;
	uint32_t ts_, ssrc_;
	bool marker_ = true;

	/* the hook flash being sent (RFC 4733): its timestamp, length and
	   duration so far (samples), its last packet's copies sent (0 while
	   it lasts), whether one is on */
	uint32_t ev_ts_ = 0;
	unsigned ev_len_ = 0, ev_dur_ = 0;
	int ev_ends_ = 0;
	bool ev_on_ = false, ev_first_ = false;

	/* the far end's telephone-events, played into the modem's audio
	   as the tones they stand for (rtp_events.h) */
	dmd::EventPlayer rx_events_;

	/* the socket has failed (what): told once, the thread to end */
	void failed(const char *what)
	{
		say(4, "audio socket %d: %s", modem_, what);
		mqueue_push(mq_, MQ_MODEM_GONE,
			    reinterpret_cast<void *>((uintptr_t)serial_));
	}

	/* a telephone-event packet's payload b (n octets) at timestamp ts
	   to the player; what began, told */
	void event_in(const uint8_t *b, size_t n, uint32_t ts)
	{
		dmd::EventPlayer::Began e = rx_events_.packet(b, n, ts);

		switch (e.kind) {
		case dmd::EventPlayer::PLAYED:
			say(4, "call %u: %s (RFC 4733 event %d), played as its "
			    "tone", serial_, e.name, e.event);
			break;
		case dmd::EventPlayer::FLASH:
			say(3, "call %u: the far end's hook flash (RFC 4733 "
			    "event 16): no signal of a line stands for it, "
			    "ignored", serial_);
			break;
		case dmd::EventPlayer::NOT_PLAYED:
			say(4, "call %u: %s (RFC 4733 event %d): its signal is "
			    "in the audio", serial_, e.name, e.event);
			break;
		case dmd::EventPlayer::UNKNOWN:
			say(3, "call %u: RFC 4733 event %d, unknown: ignored",
			    serial_, e.event);
			break;
		default:
			break;
		}
	}

	/* the packets that have come: into the jitter buffer if on, those
	   in G.711 (pt of law, or a static one); telephone-events (te) to
	   event_in() */
	void drain(bool on, int pt, int law, int te)
	{
		uint8_t b[2048];

		for (;;) {
			ssize_t n = recv(rtp_, b, sizeof(b), MSG_DONTWAIT);
			size_t h, end;
			int p, l;

			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0)
				return;
			if (!on || n < 12 || (b[0] >> 6) != 2)
				continue;
			h = 12 + 4 * (size_t)(b[0] & 15);
			if (b[0] & 0x10) {
				if ((size_t)n < h + 4)
					continue;
				h += 4 + 4 * (size_t)((b[h + 2] << 8) | b[h + 3]);
			}
			end = (size_t)n;
			if ((b[0] & 0x20) && b[n - 1] <= end)
				end -= b[n - 1];
			if (h >= end)
				continue;
			p = b[1] & 0x7f;
			if (p == te && te >= 0) {
				event_in(b + h, end - h,
					 (uint32_t)b[4] << 24 | b[5] << 16 |
					 b[6] << 8 | b[7]);
				continue;
			}
			if (p == pt)
				l = law;
			else if (p == 0 || p == 8)
				l = p == 8 ? 2 : 1;
			else
				continue;
			const char *why = jb_.put((uint32_t)b[8] << 24 |
				b[9] << 16 | b[10] << 8 | b[11],
				(uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 |
				b[7], b + h, end - h, l == 2);
			if (why)
				say(3, "call %u: the jitter buffer starts over "
				    "(%s)", serial_, why);
		}
	}

	/* an RTP header into p: payload type pt, the marker if marker,
	   timestamp ts, the next sequence number */
	void header(uint8_t *p, int pt, bool marker, uint32_t ts)
	{
		p[0] = 0x80;
		p[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7f));
		p[2] = (uint8_t)(seq_ >> 8);
		p[3] = (uint8_t)seq_;
		p[4] = (uint8_t)(ts >> 24);
		p[5] = (uint8_t)(ts >> 16);
		p[6] = (uint8_t)(ts >> 8);
		p[7] = (uint8_t)ts;
		p[8] = (uint8_t)(ssrc_ >> 24);
		p[9] = (uint8_t)(ssrc_ >> 16);
		p[10] = (uint8_t)(ssrc_ >> 8);
		p[11] = (uint8_t)ssrc_;
		seq_++;
	}

	/* a frame of the modem's (out) in RTP to peer */
	void send_rtp(const int16_t *out, int pt, int law,
		      const struct sockaddr_storage &peer, socklen_t len)
	{
		uint8_t p[12 + SAMPLES];

		header(p, pt, marker_, ts_);
		for (unsigned i = 0; i < SAMPLES; i++)
			p[12 + i] = dsp::g711::encode(law == 2, out[i]);
		(void)sendto(rtp_, p, sizeof(p), MSG_DONTWAIT,
			     reinterpret_cast<const struct sockaddr *>(&peer),
			     len);
		marker_ = false;
	}

	/* the hook flash's packet in payload type te to peer (RFC 4733:
	   event 16, its duration so far, the end bit on its last; the
	   marker on its first) */
	void send_flash(int te, const struct sockaddr_storage &peer,
			socklen_t len)
	{
		uint8_t p[12 + 4];
		bool end = ev_ends_ > 0;

		header(p, te, ev_first_, ev_ts_);
		ev_first_ = false;
		p[12] = EV_FLASH;
		p[13] = end ? 0x80 : 0;
		p[14] = (uint8_t)(ev_dur_ >> 8);
		p[15] = (uint8_t)ev_dur_;
		(void)sendto(rtp_, p, sizeof(p), MSG_DONTWAIT,
			     reinterpret_cast<const struct sockaddr *>(&peer),
			     len);
	}

	/* this frame's part of a hook flash: one asked for started (if
	   the far end takes telephone-events and has our RTP; one asked
	   for while another is on waits for its end), its packet while it
	   lasts (true: no audio with it), the copies of its last after
	   it */
	bool flash(int te, bool send, const struct sockaddr_storage &peer,
		   socklen_t len)
	{
		unsigned ms = ev_on_ ? 0 : flash_ms_.exchange(0);

		if (ms) {
			if (te < 0 || !send) {
				say(3, "call %u: hook flash of %u ms not sent "
				    "(%s)", serial_, ms, te < 0 ?
				    "no telephone-event" :
				    "the far end has no audio of ours");
			} else {
				ev_on_ = true;
				ev_ts_ = ts_;
				ev_len_ = std::min(ms * (RATE / 1000), EV_MAX);
				ev_dur_ = 0;
				ev_ends_ = 0;
				ev_first_ = true;
				say(3, "call %u: hook flash, %u ms (RFC 4733)",
				    serial_, ev_len_ / (RATE / 1000));
			}
		}
		if (!ev_on_ || !send || te < 0) {
			ev_on_ = false;
			return false;
		}
		if (ev_ends_ == 0) {
			ev_dur_ = std::min(ev_dur_ + SAMPLES, ev_len_);
			if (ev_dur_ >= ev_len_)
				ev_ends_ = 1;
			send_flash(te, peer, len);
			return true;
		}
		send_flash(te, peer, len);
		if (++ev_ends_ >= EV_ENDS)
			ev_on_ = false;
		return false;
	}

	/* every 20 ms: the packets taken in; if active, a frame to the
	   modem, the modem's answer to the frame before back (it keeps one
	   in flight), and that to the far end */
	void run()
	{
		struct timespec next;
		bool was_on = false;

		clock_gettime(CLOCK_MONOTONIC, &next);
		while (!quit_) {
			struct timespec now;
			struct sockaddr_storage peer;
			socklen_t len;
			int16_t in[SAMPLES], out[SAMPLES];
			bool on, send;
			int pt, law, te;

			next.tv_nsec += TICK_NS;
			if (next.tv_nsec >= 1000000000L) {
				next.tv_nsec -= 1000000000L;
				next.tv_sec++;
			}
			while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
					       &next, nullptr) == EINTR)
				;
			if (quit_)
				break;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - next.tv_sec) * 1000000000L +
			    (now.tv_nsec - next.tv_nsec) > LATE_NS)
				next = now;
			{
				std::lock_guard<std::mutex> lk(mu_);
				on = active_;
				send = send_ && peer_len_;
				peer = peer_;
				len = peer_len_;
				pt = pt_;
				law = law_;
				te = te_;
			}
			drain(on, pt, law, te);
			if (!on) {
				was_on = false;
				ts_ += SAMPLES;
				if (peer_closed(modem_)) {
					failed("closed by the modem");
					break;
				}
				continue;
			}
			if (!was_on) {
				jb_.reset();
				rx_events_.reset();
				marker_ = true;
				was_on = true;
			}
			jb_.get(in, SAMPLES);
			rx_events_.fill(in, SAMPLES);
			rec.rx(in, FRAME);
			if (!write_all(modem_, in, FRAME)) {
				failed("write failed");
				break;
			}
			if (!read_all(modem_, out, FRAME)) {
				failed("read failed or ended");
				break;
			}
			rec.tx(out, FRAME);
			if (!flash(te, send, peer, len) && send)
				send_rtp(out, pt, law, peer, len);
			ts_ += SAMPLES;
		}
	}
};


/*
 *    the SIP side (all of it the SIP thread's)
 */

/* the current call */
struct Call {
	unsigned serial;                /* for the log */
	struct sipsess *sess = nullptr;
	bool incoming = false;
	bool ringing = false;           /* came in, not answered yet */
	bool confirmed = false;
	bool closed = false;            /* by the far end, or failed */
	uint64_t up = 0;                /* confirmed then (ms) */
	struct tmr ring_tmr, poll_tmr;
	struct tmr tell_tmr;            /* the T38 and AUDIO lines, after
					   the SIP message that has them */
	std::string tell;

	std::unique_ptr<Media> media;   /* from dial or answer on */
	int rtp = -1, rtp_port = 0;
	std::string ip;                 /* our address, in our SDP */
	bool audio_up = false;          /* its audio agreed, once */

	/* the INVITE's offer, answered by answer() */
	bool have_offer = false;
	Sdp offer;

	/* our SDP: its lines as agreed, and as we last offered them; its
	   session id and version, the media lines of the version */
	std::vector<Line> lines, offered;
	std::string sdp_id;
	unsigned sdp_version = 0;
	std::string sdp_media;

	/* T.38 */
	bool t38_asked = false;         /* by us or the far end: not again */
	bool t38 = false;               /* the call is on T.38 */
	bool restore = false;           /* our ask refused, the audio off:
					   to ask for the audio again */
	int udptl = -1, udptl_port = 0, udptl_family = AF_INET;

	explicit Call(unsigned s) : serial(s)
	{
		tmr_init(&ring_tmr);
		tmr_init(&poll_tmr);
		tmr_init(&tell_tmr);
		sdp_id = std::to_string(rand_u32());
	}
	~Call()
	{
		tmr_cancel(&ring_tmr);
		tmr_cancel(&poll_tmr);
		tmr_cancel(&tell_tmr);
	}
	Call(const Call &) = delete;
	Call &operator=(const Call &) = delete;

	/* our SDP of lines: its version a new one if its media lines are */
	std::string sdp(const std::vector<Line> &v)
	{
		std::string m, in = ip.find(':') != std::string::npos ?
			"IN IP6 " : "IN IP4 ";

		for (const Line &l : v) {
			switch (l.kind) {
			case Line::AUDIO:
				m += "m=audio " + std::to_string(rtp_port) +
					" RTP/AVP ";
				if (l.pt < 0) {
					std::string te = std::to_string(TE_PT);
					m += "0 8 " + te + "\r\n"
						"a=rtpmap:0 PCMU/8000\r\n"
						"a=rtpmap:8 PCMA/8000\r\n"
						"a=rtpmap:" + te +
						" telephone-event/8000\r\n"
						"a=fmtp:" + te + " 0-16\r\n";
				} else {
					std::string pt = std::to_string(l.pt);
					std::string te = std::to_string(l.te);
					m += pt + (l.te >= 0 ? " " + te : "") +
						"\r\na=rtpmap:" + pt +
						(l.law == 2 ? " PCMA" : " PCMU") +
						"/8000\r\n";
					if (l.te >= 0)
						m += "a=rtpmap:" + te +
							" telephone-event/8000\r\n"
							"a=fmtp:" + te + " 0-16\r\n";
				}
				m += "a=ptime:20\r\na=" + l.dir + "\r\n";
				break;
			case Line::IMAGE:
				m += "m=image " + std::to_string(udptl_port) +
					" udptl t38\r\n" + t38_attrs(l.version);
				break;
			case Line::OFF:
				m += "m=" + l.type + " 0 " + l.proto + " " +
					l.fmts + "\r\n";
				break;
			}
		}
		if (m != sdp_media) {
			sdp_media = m;
			sdp_version++;
		}
		return "v=0\r\n"
			"o=dmodem " + sdp_id + " " +
			std::to_string(sdp_version) + " " + in + ip + "\r\n"
			"s=dmodem\r\n"
			"c=" + in + ip + "\r\n"
			"t=0 0\r\n" + m;
	}
};

struct sip *sip_;
struct dnsc *dnsc_;
struct sipsess_sock *sock_;
struct sip_lsnr *lsnr_;
struct sipreg *reg_;
struct mqueue *mq_;
struct tmr quit_tmr;
bool quitting;
bool sip_closing, sip_closed;
bool registered;
unsigned last_serial;
Call *cur;

/* (under mu) the current call's T.38, for t38() */
std::mutex mu;
bool t38_on_;
int t38_fd_ = -1, t38_family_;
Peer t38_peer_;

/* a modem's request: dial number, or answer; the call's audio on fd */
struct Request {
	std::string number;
	int fd;
};

/* SIP's credentials, ours whatever the realm */
int on_auth(char **user, char **pass, const char *, void *)
{
	int err = str_dup(user, user_.c_str());
	return err ? err : str_dup(pass, password_.c_str());
}

/* the call's UDPTL socket, once; false if it can't be had */
bool open_udptl(Call *c)
{
	if (c->udptl < 0)
		c->udptl = open_udp(&c->udptl_port, &c->udptl_family);
	return c->udptl >= 0;
}

/* the current call ended: its audio first (the modem sees the call
   end), hung up unless it is over, the line free */
void end_call()
{
	Call *c = cur;

	if (!c)
		return;
	cur = nullptr;
	te_pt_ = -1;
	flash_ms_ = 0;
	tmr_cancel(&c->ring_tmr);
	tmr_cancel(&c->poll_tmr);
	tmr_cancel(&c->tell_tmr);
	if (c->media)
		c->media->stop();
	if (c->sess) {
		if (!c->closed)
			say(3, "call %u: hanging up", c->serial);
		mem_deref(c->sess);
		c->sess = nullptr;
	}
	{
		std::lock_guard<std::mutex> lk(mu);
		t38_on_ = false;
		t38_fd_ = -1;
	}
	c->media.reset();
	if (c->rtp >= 0)
		close(c->rtp);
	if (c->udptl >= 0)
		close(c->udptl);
	delete c;
}

/* the lines the call has for main.cpp */
void on_tell(void *arg)
{
	Call *c = static_cast<Call *>(arg);

	if (c != cur)
		return;
	for (size_t at = 0; at < c->tell.size();) {
		size_t nl = c->tell.find('\n', at);
		ctl_line(c->tell.substr(at, nl + 1 - at).c_str());
		at = nl + 1;
	}
	c->tell.clear();
}

/* line for main.cpp once the SIP message being answered (or acked) has
   gone: the far end has our answer before our UDPTL comes */
void tell(Call *c, const char *line)
{
	c->tell += line;
	tmr_start(&c->tell_tmr, 0, on_tell, c);
}

/* the call's audio on the modem's socket fd: its RTP socket, its
   thread, a frame of silence in the socket ahead of everything (the
   modem keeps one in flight); false (fd closed) if that can't be */
bool attach(Call *c, int fd)
{
	static const char silence[FRAME] = {};
	int family;

	law_ = 0;
	te_pt_ = -1;
	flash_ms_ = 0;
	if (!write_all(fd, silence, FRAME)) {
		say(1, "audio socket %d: %s", fd, strerror(errno));
		close(fd);
		return false;
	}
	c->rtp = open_udp(&c->rtp_port, &family);
	if (c->rtp < 0) {
		close(fd);
		return false;
	}
	c->media.reset(new Media(fd, c->rtp, family, c->serial, mq_));
	return c->media->start();
}

/* the far end's audio, line m of s: where our RTP goes, in what, and
   the audio on (from T.38 back to it: an AUDIO line); false if it has
   none we can take */
bool take_audio(Call *c, const Sdp &s, const SdpMedia &m)
{
	struct sockaddr_storage a;
	socklen_t len;
	std::string d = direction(s, m);
	int law = 0, pt = g711_pt(m, &law);

	if (pt < 0 || !c->media)
		return false;
	if (!sockaddr_of(m.addr, m.port, c->media->family(), &a, &len)) {
		say(1, "call %u: no address in `%s'", c->serial, m.addr.c_str());
		return false;
	}
	int te = te_pt(m);
	c->media->remote(a, len, pt, law, te,
			 d != "sendonly" && d != "inactive" &&
			 m.addr != "0.0.0.0");
	law_ = law;
	te_pt_ = te;
	if (!c->audio_up) {
		c->audio_up = true;
		say(3, "call %u: media up%s, %s, to %s:%d", c->serial,
		    c->incoming || c->confirmed ? "" : " (early media)",
		    law == 1 ? "PCMU" : "PCMA", m.addr.c_str(), m.port);
	}
	c->media->active(true);
	if (c->t38) {
		c->t38 = false;
		{
			std::lock_guard<std::mutex> lk(mu);
			t38_on_ = false;
		}
		say(3, "call %u: back on the audio", c->serial);
		tell(c, "AUDIO\n");
	}
	return true;
}

/* the call over to T.38 with the far end's p: its audio stopped, a T38
   line */
void take_t38(Call *c, const Peer &p)
{
	say(3, "call %u: T.38 on, to %s:%d (version %d%s)", c->serial,
	    p.addr.c_str(), p.port, p.version,
	    p.redundancy ? ", redundancy" : "");
	if (c->media)
		c->media->active(false);
	{
		std::lock_guard<std::mutex> lk(mu);
		t38_on_ = true;
		t38_fd_ = c->udptl;
		t38_family_ = c->udptl_family;
		t38_peer_ = p;
	}
	if (!c->t38) {
		c->t38 = true;
		tell(c, "T38\n");
	}
}

/* our offer of the outgoing call's audio (the call's address now ours
   towards the far end) */
int on_desc(struct mbuf **descp, const struct sa *src, const struct sa *,
	    void *arg)
{
	Call *c = static_cast<Call *>(arg);

	c->ip = ip_of(src);
	c->offered = { Line() };
	*descp = mbuf_of(c->sdp(c->offered));
	return *descp ? 0 : ENOMEM;
}

/* the far end's offer in a re-INVITE (or UPDATE), our answer into
   *descp: T.38 taken while the modem is in fax mode (our image line in
   its place, the rest off), else its audio; an error is a 488.  An
   INVITE without one has our offer */
int on_offer(struct mbuf **descp, const struct sip_msg *msg, void *arg)
{
	Call *c = static_cast<Call *>(arg);
	Sdp s;
	int i, law;

	if (c != cur)
		return EPROTO;
	if (!mbuf_get_left(msg->mb)) {
		c->offered = c->lines.empty() ?
			std::vector<Line>{ Line() } : c->lines;
		*descp = mbuf_of(c->sdp(c->offered));
		return *descp ? 0 : ENOMEM;
	}
	if (!sdp_of(msg, &s)) {
		say(1, "call %u: an offer we can't read", c->serial);
		return EBADMSG;
	}
	i = line_of(s, true);
	if (i >= 0) {
		if (cfg_.t38 && fax_ && open_udptl(c)) {
			Peer p = t38_params(s.media[i]);
			p.version = std::min(p.version, 3);
			c->t38_asked = true;
			c->lines = mirror(s, i, image_line(p.version));
			*descp = mbuf_of(c->sdp(c->lines));
			if (!*descp)
				return ENOMEM;
			say(3, "call %u: T.38 offered, taken (UDPTL port %d)",
			    c->serial, c->udptl_port);
			take_t38(c, p);
			return 0;
		}
		say(3, "call %u: T.38 offered, not taken", c->serial);
	}
	i = line_of(s, false);
	if (i < 0 || g711_pt(s.media[i], &law) < 0) {
		say(3, "call %u: an offer without G.711 audio, refused",
		    c->serial);
		return EPROTO;
	}
	c->lines = mirror(s, i, audio_answer(s, s.media[i]));
	*descp = mbuf_of(c->sdp(c->lines));
	if (!*descp)
		return ENOMEM;
	take_audio(c, s, s.media[i]);
	return 0;
}

/* the far end's answer to our offer: to our T.38 ask its image line on
   (T.38), or off (the audio to ask for again); else its audio.  An
   answer without G.711 audio ends a call that is not up yet */
int on_answer(const struct sip_msg *msg, void *arg)
{
	Call *c = static_cast<Call *>(arg);
	Sdp s;
	int i;

	if (c != cur)
		return 0;
	if (!sdp_of(msg, &s)) {
		say(1, "call %u: an answer we can't read", c->serial);
		return c->confirmed ? 0 : EBADMSG;
	}
	i = find(c->offered, Line::IMAGE);
	if (i >= 0) {
		c->lines = c->offered;
		if ((size_t)i < s.media.size() && is_t38(s.media[i]) &&
		    s.media[i].port > 0) {
			Peer p = t38_params(s.media[i]);
			p.version = std::min(p.version, c->offered[i].version);
			take_t38(c, p);
		} else if (!c->t38) {
			say(3, "call %u: T.38 refused", c->serial);
			c->restore = true;
		}
		return 0;
	}
	i = line_of(s, false);
	if (i < 0 || (size_t)i >= c->offered.size() ||
	    c->offered[i].kind != Line::AUDIO) {
		say(1, "call %u: an answer without our audio", c->serial);
		return c->confirmed ? 0 : EPROTO;
	}
	Line l = c->offered[i];
	l.pt = g711_pt(s.media[i], &l.law);
	l.te = te_pt(s.media[i]);
	if (l.pt < 0 || !take_audio(c, s, s.media[i])) {
		say(1, "call %u: an answer without G.711", c->serial);
		return c->confirmed ? 0 : EPROTO;
	}
	c->lines = c->offered;
	c->lines[i] = l;
	return 0;
}

/* the outgoing call's provisional responses */
void on_progress(const struct sip_msg *msg, void *arg)
{
	Call *c = static_cast<Call *>(arg);

	say(3, "call %u: %u %s", c->serial, msg->scode,
	    str(msg->reason).c_str());
}

/* the call up (its 2xx, or its ACK): an ANSWERED line for a call we
   made */
void on_estab(const struct sip_msg *, void *arg)
{
	Call *c = static_cast<Call *>(arg);

	if (c != cur)
		return;
	c->confirmed = true;
	c->up = tmr_jiffies();
	say(3, "call %u: CONFIRMED", c->serial);
	if (!c->incoming)
		ctl_line("ANSWERED\n");
}

/* the call over (the far end hung up, or the call failed): CANCEL if
   it still rang, the call ended */
void on_close(int err, const struct sip_msg *msg, void *arg)
{
	Call *c = static_cast<Call *>(arg);

	if (c != cur)
		return;
	if (msg && msg->scode)
		say(3, "call %u: DISCONNECTED (%u %s)", c->serial, msg->scode,
		    str(msg->reason).c_str());
	else if (msg)
		say(3, "call %u: DISCONNECTED (%s)", c->serial,
		    str(msg->met).c_str());
	else
		say(3, "call %u: DISCONNECTED (%s)", c->serial,
		    err == ECONNRESET ? "BYE" : err ? strerror(err) : "closed");
	c->closed = true;
	if (c->ringing)
		ctl_line("CANCEL\n");
	/* a call we made that never came up: why, ahead of its audio's
	   end */
	if (!c->incoming && !c->confirmed) {
		char line[32];
		snprintf(line, sizeof(line), "FAILED %u\n",
			 msg ? msg->scode : 0);
		ctl_line(line);
	}
	end_call();
}

/* our T.38 ask: a re-INVITE with our image line in the audio's place
   (once only, taken or not) */
void ask_t38(Call *c)
{
	struct mbuf *mb;
	int i = find(c->lines, Line::AUDIO), err;

	c->t38_asked = true;
	if (i < 0 || !open_udptl(c))
		return;
	std::vector<Line> v = c->lines;
	v[i] = image_line(0);
	mb = mbuf_of(c->sdp(v));
	err = mb ? sipsess_modify(c->sess, mb) : ENOMEM;
	mem_deref(mb);
	if (err) {
		say(1, "call %u: asking for T.38: %s", c->serial, strerror(err));
		return;
	}
	c->offered = v;
	say(3, "call %u: asking for T.38 (UDPTL port %d)", c->serial,
	    c->udptl_port);
}

/* our ask refused with the image line off: a re-INVITE with our audio
   in its place */
void ask_audio(Call *c)
{
	struct mbuf *mb;
	int i = find(c->lines, Line::IMAGE), err;

	if (i < 0)
		return;
	std::vector<Line> v = c->lines;
	v[i] = Line();
	mb = mbuf_of(c->sdp(v));
	err = mb ? sipsess_modify(c->sess, mb) : ENOMEM;
	mem_deref(mb);
	if (err) {
		say(1, "call %u: asking for the audio: %s", c->serial,
		    strerror(err));
		return;
	}
	c->offered = v;
	say(3, "call %u: asking for the audio again", c->serial);
}

/* the look at the current call every POLL_MS: the audio to ask for
   again, our T.38 ask a second after we answered a fax call */
void on_poll(void *arg)
{
	Call *c = static_cast<Call *>(arg);

	if (c != cur)
		return;
	tmr_start(&c->poll_tmr, POLL_MS, on_poll, c);
	if (c->restore) {
		c->restore = false;
		ask_audio(c);
	}
	if (cfg_.t38 && fax_ && c->incoming && c->confirmed && !c->t38_asked &&
	    !c->t38 && tmr_jiffies() >= c->up + T38_ASK_MS)
		ask_t38(c);
}

/* a ringing call's RING lines */
void on_ring(void *arg)
{
	Call *c = static_cast<Call *>(arg);

	if (c != cur || !c->ringing)
		return;
	ctl_line("RING\n");
	tmr_start(&c->ring_tmr, RING_MS, on_ring, c);
}

/* an INVITE: 480 unless listening, 486 if the line is busy (a call
   being dialled too), 488 if its offer has no G.711 audio; else the
   call is current and rings (180) */
void on_invite(const struct sip_msg *msg, void *)
{
	std::string from = str(msg->from.auri);
	Sdp s;
	bool sdp = mbuf_get_left(msg->mb) > 0;
	int law, i = -1, err;
	Call *c;

	if (sdp && sdp_of(msg, &s))
		i = line_of(s, false);
	if (!cfg_.listen || quitting) {
		say(3, "call from %s: 480", from.c_str());
		sip_treply(nullptr, sip_, msg, 480, "Temporarily Unavailable");
		return;
	}
	if (cur) {
		say(3, "call from %s: 486", from.c_str());
		sip_treply(nullptr, sip_, msg, 486, "Busy Here");
		return;
	}
	if (sdp && (i < 0 || g711_pt(s.media[i], &law) < 0)) {
		say(3, "call from %s: no G.711 audio, 488", from.c_str());
		sip_treply(nullptr, sip_, msg, 488, "Not Acceptable Here");
		return;
	}
	c = new Call(++last_serial);
	c->incoming = c->ringing = true;
	c->have_offer = sdp;
	c->offer = s;
	c->ip = ip_of(&msg->dst);
	cur = c;
	err = sipsess_accept(&c->sess, sock_, msg, 180, "Ringing",
			     REL100_DISABLED, user_.c_str(), "application/sdp",
			     nullptr, on_auth, nullptr, false, on_offer,
			     on_answer, on_estab, nullptr, nullptr, on_close, c,
			     nullptr);
	if (err) {
		say(1, "call from %s: accepting it: %s", from.c_str(),
		    strerror(err));
		c->sess = nullptr;
		c->closed = true;
		end_call();
		sip_treply(nullptr, sip_, msg, 500, "Server Internal Error");
		return;
	}
	say(3, "call %u from %s: ringing", c->serial, from.c_str());
	on_ring(c);
	tmr_start(&c->poll_tmr, POLL_MS, on_poll, c);
}

/* dial number with the call's audio on fd, unless the line is busy (fd
   closed) */
void do_dial(const std::string &number, int fd)
{
	std::string uri = "sip:" + dial_user(number.c_str()) + "@" + domain_;
	std::string from = "sip:" + user_ + "@" + domain_;
	Call *c;
	int err;

	if (cur) {
		say(3, "dial %s: the line is busy", number.c_str());
		close(fd);
		return;
	}
	c = cur = new Call(++last_serial);
	if (!attach(c, fd)) {
		end_call();
		return;
	}
	say(3, "call %u: dialling %s", c->serial, uri.c_str());
	err = sipsess_connect(&c->sess, sock_, uri.c_str(), nullptr,
			      from.c_str(), user_.c_str(), nullptr, 0,
			      "application/sdp", on_auth, nullptr, false,
			      nullptr, on_desc, on_offer, on_answer,
			      on_progress, on_estab, nullptr, nullptr,
			      on_close, c, nullptr);
	if (err) {
		say(1, "call %u: making the call: %s", c->serial,
		    strerror(err));
		c->closed = true;
		end_call();
		return;
	}
	tmr_start(&c->poll_tmr, POLL_MS, on_poll, c);
}

/* answer the call that rings with its audio on fd (fd closed if none
   rings): our answer to its offer, or our offer if it had none */
void do_answer(int fd)
{
	Call *c = cur;
	std::vector<Line> v;
	struct mbuf *mb;
	int i = -1, err;

	if (!c || !c->ringing) {
		say(3, "answer: no call rings");
		close(fd);
		return;
	}
	c->ringing = false;
	tmr_cancel(&c->ring_tmr);
	if (!attach(c, fd)) {
		end_call();
		return;
	}
	say(3, "call %u: answering", c->serial);
	if (c->have_offer) {
		i = line_of(c->offer, false);
		v = mirror(c->offer, i, audio_answer(c->offer,
						      c->offer.media[i]));
	} else {
		v = { Line() };
	}
	mb = mbuf_of(c->sdp(v));
	err = mb ? sipsess_answer(c->sess, 200, "OK", mb, nullptr) : ENOMEM;
	mem_deref(mb);
	if (err) {
		say(1, "call %u: answering: %s", c->serial, strerror(err));
		end_call();
		return;
	}
	if (c->have_offer) {
		c->lines = v;
		take_audio(c, c->offer, c->offer.media[i]);
	} else {
		c->offered = v;
	}
}

/* the stack's last transactions done (sip_close()) */
void on_sip_exit(void *)
{
	sip_closed = true;
	re_cancel();
}

/* the end: the call hung up, unregistered, the stack closed once its
   last transactions are done (QUIT_MS at the most) */
void quit()
{
	quitting = true;
	end_call();
	reg_ = static_cast<struct sipreg *>(mem_deref(reg_));
	lsnr_ = static_cast<struct sip_lsnr *>(mem_deref(lsnr_));
	sock_ = static_cast<struct sipsess_sock *>(mem_deref(sock_));
	tmr_start(&quit_tmr, QUIT_MS, [](void *) { re_cancel(); }, nullptr);
	sip_closing = true;
	sip_close(sip_, false);
}

/* the SIP thread's queue: the modem's requests, its audio socket
   failed, the end */
void on_mq(int id, void *data, void *)
{
	switch (id) {
	case MQ_DIAL:
	case MQ_ANSWER: {
		std::unique_ptr<Request> r(static_cast<Request *>(data));
		if (quitting)
			close(r->fd);
		else if (id == MQ_DIAL)
			do_dial(r->number, r->fd);
		else
			do_answer(r->fd);
		break;
	}
	case MQ_MODEM_GONE:
		if (cur && (uintptr_t)cur->serial ==
		    reinterpret_cast<uintptr_t>(data)) {
			say(3, "call %u: the modem has gone", cur->serial);
			end_call();
		}
		break;
	case MQ_QUIT:
		if (!quitting)
			quit();
		break;
	}
}

/* requests outside a call: OPTIONS answered */
bool on_request(const struct sip_msg *msg, void *)
{
	if (pl_strcmp(&msg->met, "OPTIONS"))
		return false;
	sip_treplyf(nullptr, nullptr, sip_, msg, false, 200, "OK",
		    "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, UPDATE\r\n"
		    "Accept: application/sdp\r\n"
		    "Content-Length: 0\r\n"
		    "\r\n");
	return true;
}

/* the registration's responses */
void on_reg(int err, const struct sip_msg *msg, void *)
{
	if (err)
		say(1, "registration with %s: %s", domain_.c_str(),
		    strerror(err));
	else if (msg->scode >= 300)
		say(1, "registration with %s: %u %s", domain_.c_str(),
		    msg->scode, str(msg->reason).c_str());
	else if (msg->scode >= 200) {
		say(registered ? 4 : 3, "registered with %s (%u %s)",
		    domain_.c_str(), msg->scode, str(msg->reason).c_str());
		registered = true;
	}
}

/* the SIP messages, into the log */
void on_trace(bool tx, enum sip_transp, const struct sa *src,
	      const struct sa *dst, const uint8_t *pkt, size_t len, void *)
{
	say(4, "%s %s:\n%.*s", tx ? "TX to" : "RX from",
	    addr_of(tx ? dst : src).c_str(), (int)len,
	    reinterpret_cast<const char *>(pkt));
}

/* SIP over UDP on each address of ours (the local port, as the far end
   has it, then the address it came to) */
bool add_transport(const char *ifname, const struct sa *a, void *arg)
{
	auto *added = static_cast<std::vector<struct sa> *>(arg);
	struct sa l;
	int err;

	if (sa_is_linklocal(a))
		return false;
	sa_cpy(&l, a);
	sa_set_port(&l, (uint16_t)cfg_.port);
	for (const struct sa &o : *added)
		if (sa_cmp(&o, &l, SA_ADDR))
			return false;
	err = sip_transp_add(sip_, SIP_TRANSP_UDP, &l);
	if (err) {
		say(1, "SIP on %s (%s): %s", addr_of(&l).c_str(), ifname,
		    strerror(err));
		return false;
	}
	added->push_back(l);
	say(4, "SIP on %s (%s)", addr_of(&l).c_str(), ifname);
	return false;
}

/* libre and the stack up: its transports, calls taken, the queue, the
   registration when listening; false (with a message) if any of it
   fails */
bool init()
{
	std::vector<struct sa> added;
	struct sa nsv[8];
	uint32_t nsn = 8;
	char dom[256] = "";
	int err;

	err = libre_init();
	if (err) {
		say(1, "libre: %s", strerror(err));
		return false;
	}
	/* (its OpenSSL's set-up put a handler of its own) */
	signal(SIGPIPE, SIG_IGN);
	tmr_init(&quit_tmr);

	/* names by the system's resolver, SRV and NAPTR by its servers */
	if (dns_srv_get(dom, sizeof(dom), nsv, &nsn))
		nsn = 0;
	err = dnsc_alloc(&dnsc_, nullptr, nsn ? nsv : nullptr, nsn);
	if (err) {
		say(1, "DNS client: %s", strerror(err));
		return false;
	}
	dnsc_getaddrinfo(dnsc_, true);

	err = sip_alloc(&sip_, dnsc_, 32, 32, 32, "D-Modem " DMODEM_VERSION,
			on_sip_exit, nullptr);
	if (err) {
		say(1, "SIP stack: %s", strerror(err));
		return false;
	}
	if (log_level >= 4)
		sip_set_trace_handler(sip_, on_trace);
	net_if_apply(add_transport, &added);
	if (added.empty()) {
		say(1, "no address for SIP on port %d", cfg_.port);
		return false;
	}
	err = sipsess_listen(&sock_, sip_, 32, on_invite, nullptr);
	if (!err)
		err = sip_listen(&lsnr_, sip_, true, on_request, nullptr);
	if (!err)
		err = mqueue_alloc(&mq_, on_mq, nullptr);
	if (err) {
		say(1, "SIP: %s", strerror(err));
		return false;
	}
	if (cfg_.listen) {
		std::string uri = "sip:" + domain_;
		std::string aor = "sip:" + user_ + "@" + domain_;
		err = sipreg_alloc(&reg_, sip_, uri.c_str(), aor.c_str(),
				   nullptr, aor.c_str(), REG_EXPIRES,
				   user_.c_str(), nullptr, 0, 0, on_auth,
				   nullptr, false, on_reg, nullptr, nullptr,
				   nullptr);
		if (!err)
			err = sipreg_send(reg_);
		if (err)
			say(1, "registration with %s: %s", domain_.c_str(),
			    strerror(err));
	}
	return true;
}

/* what init() and the main loop leave, libre closed */
void cleanup()
{
	tmr_cancel(&quit_tmr);
	end_call();
	reg_ = static_cast<struct sipreg *>(mem_deref(reg_));
	lsnr_ = static_cast<struct sip_lsnr *>(mem_deref(lsnr_));
	sock_ = static_cast<struct sipsess_sock *>(mem_deref(sock_));
	if (sip_ && sip_closing && !sip_closed)
		/* (QUIT_MS passed: its own reference given up) */
		sip_close(sip_, true);
	else
		mem_deref(sip_);
	sip_ = nullptr;
	dnsc_ = static_cast<struct dnsc *>(mem_deref(dnsc_));
	mq_ = static_cast<struct mqueue *>(mem_deref(mq_));
	libre_close();
}

/* the SIP thread: libre's main loop, until quit() */
void serve()
{
	bool ok = init();

	{
		std::lock_guard<std::mutex> lk(start_mu);
		start_state = ok ? 1 : -1;
	}
	start_cv.notify_one();
	if (ok)
		re_main(nullptr);
	cleanup();
}

/* a request for the SIP thread; without one, fd closed at once */
void queue(int id, Request *r)
{
	if (!running || mqueue_push(mq_, id, r)) {
		close(r->fd);
		delete r;
	}
}

} // namespace

/* see sip.h */
bool config_from_env(Config &cfg, bool listen)
{
	static std::string user, password, domain, record;
	const char *login = getenv("SIP_LOGIN");
	const char *port = getenv("SIP_PORT");
	const char *rec_base = getenv("DMODEM_REC");
	const char *t38 = getenv("SIP_T38");
	std::string s = login ? login : "";
	size_t at = s.rfind('@');
	size_t colon = s.find(':');

	/* the password may have an @: the domain is after the last */
	if (!login || at == std::string::npos ||
	    colon == std::string::npos || colon > at) {
		fprintf(stderr, "error: SIP_LOGIN=user:password@domain "
			"needed\n");
		return false;
	}
	user = s.substr(0, colon);
	password = s.substr(colon + 1, at - colon - 1);
	domain = s.substr(at + 1);
	record = rec_base && *rec_base ? rec_base : "";

	cfg.user = user.c_str();
	cfg.password = password.c_str();
	cfg.domain = domain.c_str();
	cfg.port = port && *port ? atoi(port) : 5060;
	cfg.listen = listen;
	cfg.record = record.empty() ? nullptr : record.c_str();
	cfg.t38 = !t38 || !(!strcasecmp(t38, "no") || !strcmp(t38, "0") ||
			    !strcasecmp(t38, "off") ||
			    !strcasecmp(t38, "false"));
	return true;
}

/* see sip.h */
int start(const Config &cfg)
{
	const char *level = getenv("SIP_LOG_LEVEL");

	cfg_ = cfg;
	user_ = cfg.user ? cfg.user : "";
	password_ = cfg.password ? cfg.password : "";
	domain_ = cfg.domain ? cfg.domain : "";
	log_level = level && *level ? atoi(level) : 4;
	if (cfg.record)
		rec.open(cfg.record, ".rx", ".tx");

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) < 0) {
		fprintf(stderr, "error: socketpair: %s\n", strerror(errno));
		ctl[0] = ctl[1] = -1;
		rec.close();
		return -1;
	}
	start_state = 0;
	try {
		sip_thread = std::thread(serve);
	} catch (const std::system_error &e) {
		fprintf(stderr, "error: the SIP thread: %s\n", e.what());
	}
	if (sip_thread.joinable()) {
		std::unique_lock<std::mutex> lk(start_mu);
		start_cv.wait(lk, [] { return start_state != 0; });
		if (start_state > 0) {
			running = true;
			return ctl[1];
		}
		lk.unlock();
		sip_thread.join();
	}
	close(ctl[0]);
	close(ctl[1]);
	ctl[0] = ctl[1] = -1;
	rec.close();
	return -1;
}

/* see sip.h */
void stop()
{
	if (!sip_thread.joinable())
		return;
	running = false;
	if (mqueue_push(mq_, MQ_QUIT, nullptr))
		fprintf(stderr, "error: the SIP thread can't be stopped\n");
	else
		sip_thread.join();
	rec.close();
	if (ctl[0] >= 0)
		close(ctl[0]);
	ctl[0] = -1;
}

/* see sip.h */
void dial(const char *number, int audio_fd)
{
	queue(MQ_DIAL, new Request{ number ? number : "", audio_fd });
}

/* see sip.h */
void answer(int audio_fd)
{
	queue(MQ_ANSWER, new Request{ std::string(), audio_fd });
}

/* see sip.h */
int law()
{
	return law_;
}

/* see sip.h */
void set_fax(bool fax)
{
	fax_ = fax;
}

/* see sip.h */
bool can_flash()
{
	return te_pt_ >= 0;
}

/* see sip.h */
void flash(unsigned ms)
{
	if (ms)
		flash_ms_ = ms;
}

/* see sip.h */
bool t38(T38 *t)
{
	std::lock_guard<std::mutex> lk(mu);

	if (!t38_on_ || t38_fd_ < 0 || t38_peer_.port <= 0)
		return false;
	if (!sockaddr_of(t38_peer_.addr, t38_peer_.port, t38_family_,
			 &t->peer, &t->peer_len)) {
		say(1, "T.38: no address in `%s'", t38_peer_.addr.c_str());
		return false;
	}
	t->fd = fcntl(t38_fd_, F_DUPFD_CLOEXEC, 0);
	if (t->fd < 0)
		return false;
	t->version = t38_peer_.version;
	t->max_datagram = t38_peer_.max_datagram;
	t->redundancy = t38_peer_.redundancy;
	return true;
}

} // namespace sip
