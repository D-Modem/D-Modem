/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sip_modem.cpp - the modem core on a SIP line, its DTE a descriptor
 *                    (sip_modem.h)
 *
 */

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <dcore.h>
#include <log.h>
#include <str.h>
#include <t38.h>

#include "dm_types.h"
#include "portable.h"
#ifndef _WIN32
#include "pty.h"
#endif
#include "run_loop.h"
#include "sip.h"
#include "sip_modem.h"

#define ERR(fmt, ...) dcore::log_printf(0, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "dmodemd: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/* the line: 8000 samples a second, in frames of 20 ms */
const s32 RATE = 8000;
const s16 FRAME = RATE / 50;
/* the most samples one read of the call's audio takes */
const s16 AUDIO_MAX = 2048;

/* the SIP user agent's control lines: the longest, with its newline */
const size_t CONTROL_MAX = 255;

/* T.38: the far end's datagrams if it says nothing of them, and the
   least we take; the octets of a datagram an IFP packet's own headers
   need; the IFP packets each one repeats (redundancy); the longest
   datagram we read; the loop's longest wait while on T.38 (ms), and
   the most of its clock made up after a pause (samples) */
const s16 T38_DATAGRAM = 400;
const s16 T38_DATAGRAM_MIN = 32;
const s16 T38_IFP_HEADERS = 16;
const s16 T38_DEPTH = 3;
const s16 T38_RX_MAX = 2048;
const s32 T38_WAIT_MS = 20;
const u64 T38_CATCH_UP = RATE;

} // namespace

/* the modem's Host: its line a call of the SIP user agent (the call's
   audio on our end of a socket pair, or its fax on the call's UDPTL),
   its DTE the SipModem's, its time the monotonic clock */
class SipLine final : public dcore::Host {
public:
	/* the line of dte's modem */
	explicit SipLine(Dte &dte) : dte_(dte) {}
	~SipLine();
	SipLine(const SipLine &) = delete;
	SipLine &operator=(const SipLine &) = delete;

	/* Host */
	s16 start(dcore::Modem &m) override;
	s16 stop(dcore::Modem &m) override;
	/* for ATI3 */
	const char *driver_name() override { return "SIP"; }
	/* SIP says when a call we dial is answered, and why it failed */
	bool signals_answer(dcore::Modem &) override { return true; }
	/* the modem's characters to the DTE: how many it took */
	s32 dte_write(dcore::Modem &, const char *buf, s32 n) override {
		return dte_.write(buf, n);
	}
	void t38_send(dcore::Modem &m, const void *ifp) override;
	/* a call in CLEARMODE, or offered it first: an ISDN data call */
	bool clear_line(dcore::Modem &) override { return sip::clear(); }
	/* a hook flash: an RFC 4733 event on the call, if it has them */
	bool can_flash(dcore::Modem &) override {
		return audio_ >= 0 && sip::can_flash();
	}
	/* (the event of ms) */
	void hook_flash(dcore::Modem &, u16 ms) override {
		sip::flash(ms);
	}
	/* the clock: the monotonic one's ms */
	u64 now_ms() override { return dcore::monotonic_ms(); }
	/* ATM, ATL: to the speaker's owner (SipModem::on_speaker()) */
	void speaker(dcore::Modem &, s16 volume) override {
		speaker_level_ = volume;
		if (speaker_to.volume)
			speaker_to.volume(volume);
	}

	/* the speaker's owner */
	SipModem::Speaker speaker_to;

	/* the call's audio socket, -1 if none */
	int audio_fd() const { return audio_; }
	/* it is readable: its samples through the modem and back; 0, or
	   -1 if the answer can't be written */
	s16 audio(dcore::Modem &m);

	/* the call's UDPTL socket while on T.38, -1 if none */
	int t38_fd() const { return t38_; }
	/* the call went over to T.38: the modem's fax engine on it */
	void t38_begin(dcore::Modem &m);
	/* off T.38: its socket closed */
	void t38_end();
	/* the UDPTL socket is readable: its IFP packets to the modem */
	void t38_receive(dcore::Modem &m);
	/* the fax engine's time on T.38: the samples due by the clock */
	void t38_clock(dcore::Modem &m);

private:
	/* an IFP packet out of UDPTL (UdptlRx::packet()'s callback) */
	static void ifp_in(void *ctx, const unsigned char *ifp, s16 len,
			   u16 seq);

	Dte &dte_;
	int audio_ = -1;
	/* (an odd octet of the audio a read split off: the next read's
	   first; TCP's loopback on Windows splits where it likes) */
	char carry_ = 0;
	s16 carry_n_ = 0;
	s16 speaker_level_ = 0;

	/* T.38: the socket, the far end, the version agreed, the largest
	   datagram it takes; UDPTL each way; the fax engine's clock (when
	   it started, the samples it has had since) */
	int t38_ = -1;
	struct sockaddr_storage peer_ = {};
	socklen_t peer_len_ = 0;
	s32 version_ = 0;
	s32 datagram_ = T38_DATAGRAM;
	dsp::UdptlTx tx_ = {};
	dsp::UdptlRx rx_ = {};
	u64 t38_start_ = 0;
	u64 t38_samples_ = 0;
	/* the modem t38_receive() gives the packets to */
	dcore::Modem *rx_modem_ = nullptr;
};

/* the line's sockets, if they are still open */
SipLine::~SipLine()
{
	if (audio_ >= 0)
		dmd::fd_close(audio_);
	t38_end();
}

/* off hook: a socket pair for the call's audio, ours with a frame of
   silence in it ahead (the user agent reads our answer to each frame it
   writes, and finds this one for its first), the other end to the user
   agent to dial with or to answer the ringing call */
s16 SipLine::start(dcore::Modem &m)
{
	static const s16 silence[FRAME] = {};
	int sv[2];

	if (dmd::socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		ERR("socketpair: %s\n", strerror(errno));
		return -1;
	}
	if (dmd::fd_write(sv[0], silence, sizeof(silence)) !=
	    static_cast<int>(sizeof(silence))) {
		ERR("dev write: %s\n", strerror(errno));
		dmd::fd_close(sv[0]);
		dmd::fd_close(sv[1]);
		return -1;
	}
	audio_ = sv[0];
	carry_n_ = 0;
	sip::set_clear(m.sreg(SREG_DP) == DP_CLEAR);
	if (m.caller()) {
		DBG("off hook: dialling %s\n", m.dial_string());
		sip::dial(m.dial_string(), sv[1]);
	} else {
		DBG("off hook: answering\n");
		sip::answer(sv[1]);
	}
	return 0;
}

/* on hook: our end of the audio closed (the call ends with it), and
   T.38's */
s16 SipLine::stop(dcore::Modem &)
{
	DBG("on hook\n");
	if (audio_ >= 0) {
#ifdef _WIN32
		/* (abortive: the user agent may not have read the frame
		   start() put ahead, and Windows says nothing of a plain
		   close behind data not read; a reset it sees) */
		struct linger l = { 1, 0 };
		setsockopt((SOCKET)audio_, SOL_SOCKET, SO_LINGER,
			   (const char *)&l, sizeof(l));
#endif
		dmd::fd_close(audio_);
		audio_ = -1;
	}
	t38_end();
	return 0;
}

/* the call's samples into the modem, its answer back: as many samples
   as came.  On T.38 the modem's time is the clock's (t38_clock()), and
   the answer silence */
s16 SipLine::audio(dcore::Modem &m)
{
	s16 in[AUDIO_MAX], out[AUDIO_MAX];
	char *bytes = reinterpret_cast<char *>(in);
	if (carry_n_)
		bytes[0] = carry_;
	ssize_t n = dmd::fd_read(audio_, bytes + carry_n_,
				 static_cast<int>(sizeof(in)) - carry_n_), done;
	s16 count;

	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	if (n <= 0) {
		/* the call is over: NO CARRIER */
		if (n < 0 && errno == ECONNRESET)
			DBG("dev read: connection reset\n");
		else if (n < 0)
			ERR("dev read: %s\n", strerror(errno));
		m.line_dropped();
		return 0;
	}
	/* (the user agent writes whole frames; a read may end inside a
	   sample all the same) */
	n += carry_n_;
	carry_n_ = static_cast<s16>(n & 1);
	if (carry_n_)
		carry_ = bytes[n - 1];
	count = static_cast<s16>(n) / 2;
	if (count == 0)
		return 0;
	if (t38_ >= 0) {
		std::fill_n(out, count, 0);
	} else {
		s16 law = sip::law();
		/* (CLEARMODE's octets are A-law samples) */
		if (law)
			m.set_line_law(law == 3 ? 2 : law);
		m.process(in, out, count);
		if (speaker_level_ > 0 && speaker_to.audio)
			speaker_to.audio(in, out, count);
	}
	if (audio_ < 0) {
		DBG("hung up: no answer to the line's samples\n");
		return 0;
	}
	done = dmd::fd_write(audio_, out, 2 * count);
	if (done == 2 * count)
		return 0;
	if (done < 0 && (errno == EPIPE || errno == ECONNRESET)) {
		/* the user agent ended the call as we answered */
		DBG("dev write: %s\n", strerror(errno));
		m.line_dropped();
		return 0;
	}
	ERR("dev write: %s\n", done < 0 ? strerror(errno) : "short write");
	return -1;
}

/* the call is on T.38 now (sip::t38()): its socket and far end kept,
   UDPTL set up as agreed, the fax engine started on it with its clock
   from now */
void SipLine::t38_begin(dcore::Modem &m)
{
	sip::T38 t;

	if (!sip::t38(&t)) {
		DBG("T38: the call is not on T.38\n");
		return;
	}
	t38_end();
	t38_ = t.fd;
#ifdef _WIN32
	/* (no MSG_DONTWAIT) */
	dmd::set_nonblocking(t38_);
#endif
	peer_ = t.peer;
	peer_len_ = t.peer_len;
	version_ = t.version;
	datagram_ = t.max_datagram > 0 ? t.max_datagram : T38_DATAGRAM;
	if (datagram_ < T38_DATAGRAM_MIN)
		datagram_ = T38_DATAGRAM_MIN;
	tx_.init(t.redundancy ? dsp::T38_EC_REDUNDANCY : dsp::T38_EC_NONE,
		 T38_DEPTH);
	rx_.init();
	t38_start_ = dcore::monotonic_ms();
	t38_samples_ = 0;
	if (m.t38_start(datagram_ - T38_IFP_HEADERS) < 0) {
		ERR("T.38: the modem's fax can't go over to it\n");
		t38_end();
		return;
	}
	DBG("T.38 on: version %d, datagrams of %" PRId32 " octets, %s\n",
	    version_, datagram_, t.redundancy ? "redundancy" : "no redundancy");
}

/* off T.38: the UDPTL socket closed */
void SipLine::t38_end()
{
	if (t38_ >= 0) {
		dmd::fd_close(t38_);
		t38_ = -1;
	}
}

/* an IFP packet of the fax engine's: encoded for the version agreed,
   in a UDPTL datagram to the far end (one that can't be made is
   dropped) */
void SipLine::t38_send(dcore::Modem &, const void *ifp)
{
	unsigned char pkt[UDPTL_IFP_MAX], dgram[2 * UDPTL_IFP_MAX];
	s16 n, len, max;

	if (t38_ < 0)
		return;
	n = dsp::t38_ifp_encode(static_cast<const dsp::t38_ifp *>(ifp),
				version_, pkt, sizeof(pkt));
	if (n < 0)
		return;
	max = datagram_ < static_cast<s32>(sizeof(dgram)) ?
		      static_cast<s16>(datagram_) :
		      static_cast<s16>(sizeof(dgram));
	len = tx_.packet(pkt, n, dgram, max);
	if (len <= 0)
		return;
	if (sendto(t38_, reinterpret_cast<const char *>(dgram), len, 0,
		   reinterpret_cast<const struct sockaddr *>(&peer_),
		   peer_len_) < 0)
		DBG("T.38 send: %s\n", strerror(errno));
}

/* an IFP packet UDPTL gave (ctx: the SipLine), decoded for the version
   agreed and, if it is one, to the modem */
void SipLine::ifp_in(void *ctx, const unsigned char *ifp, s16 len,
		     u16)
{
	SipLine *line = static_cast<SipLine *>(ctx);
	dsp::t38_ifp p;

	/* (the modem may have hung up on the packet before; len 0: a
	   packet lost) */
	if (line->t38_ < 0 || !len)
		return;
	if (!dsp::t38_ifp_decode(&p, line->version_, ifp, len))
		line->rx_modem_->t38_rx(&p);
}

/* every datagram waiting, until the socket is closed meanwhile */
void SipLine::t38_receive(dcore::Modem &m)
{
	unsigned char buf[T38_RX_MAX];

	rx_modem_ = &m;
	while (t38_ >= 0) {
		ssize_t n = recv(t38_, reinterpret_cast<char *>(buf),
				 sizeof(buf), MSG_DONTWAIT);
		if (n < 0)
			break;
		rx_.packet(buf, static_cast<int>(n), ifp_in, this);
	}
}

/* the samples the clock says are due since T.38 began (a second of
   them at most made up), through the modem 160 at a time on silence:
   its fax engine's timers and packets */
void SipLine::t38_clock(dcore::Modem &m)
{
	static const s16 silence[FRAME] = {};
	s16 out[FRAME];
	u64 due;

	if (t38_ < 0)
		return;
	due = (dcore::monotonic_ms() - t38_start_) * (RATE / 1000);
	if (due - t38_samples_ > T38_CATCH_UP)
		t38_samples_ = due - T38_CATCH_UP;
	while (due - t38_samples_ >= static_cast<u64>(FRAME) &&
	       t38_ >= 0 && m.started()) {
		m.process(silence, out, FRAME);
		t38_samples_ += FRAME;
	}
}

namespace {

/* the result for a call we dialled that failed with SIP status code: busy
   (486 Busy Here, 600 Busy Everywhere, 603 Decline), no answer (408
   Request Timeout, 480 Temporarily Unavailable), else no carrier */
dcore::Result failure(s32 code)
{
	switch (code) {
	case 486:
	case 600:
	case 603:
		return dcore::Result::Busy;
	case 408:
	case 480:
		return dcore::Result::NoAnswer;
	default:
		return dcore::Result::NoCarrier;
	}
}

/* the SIP user agent's control descriptor: its lines (RING, CANCEL,
   T38, AUDIO, ANSWERED, FAILED), each acted on as it is complete */
class Control {
public:
	/* the control on fd (the caller's) */
	explicit Control(int fd) : fd_(fd) {}

	/* the descriptor to watch */
	int fd() const { return fd_; }
	/* it is readable: what came; -1 if the user agent has gone */
	s16 serve(dcore::Modem &m, SipLine &line);

private:
	void act(const char *s, dcore::Modem &m, SipLine &line);

	int fd_;
	char buf_[CONTROL_MAX];
	size_t len_ = 0;
};

/* what came, onto the line kept from before: each complete line acted
   on (a line that fills the buffer without its end dropped) */
s16 Control::serve(dcore::Modem &m, SipLine &line)
{
	char chunk[CONTROL_MAX];
	ssize_t n = dmd::fd_read(fd_, chunk, sizeof(chunk));

	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	if (n <= 0) {
		ERR("the SIP user agent has gone%s%s\n", n < 0 ? ": " : "",
		    n < 0 ? strerror(errno) : "");
		return -1;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (chunk[i] == '\n') {
			buf_[len_] = '\0';
			act(buf_, m, line);
			len_ = 0;
			continue;
		}
		buf_[len_++] = chunk[i];
		if (len_ == sizeof(buf_)) {
			DBG("SIP: a control line too long, dropped\n");
			len_ = 0;
		}
	}
	return 0;
}

/* CALLER's fields, "<date>\t<number>\t<name>": a number or name is
   '=' and its text, or '-' when withheld */
void caller(std::string_view f, dcore::Modem &m)
{
	std::string v[3];
	bool withheld[3] = {};

	for (int i = 0; i < 3; i++) {
		size_t tab = f.find('\t');
		std::string_view x = f.substr(0, tab);
		if (i > 0) {
			withheld[i] = !x.empty() && x[0] == '-';
			if (!x.empty())
				x.remove_prefix(1);
		}
		v[i] = x;
		f = tab == std::string_view::npos ? std::string_view() :
						    f.substr(tab + 1);
	}
	m.caller_id(v[0].c_str(), withheld[1] ? nullptr : v[1].c_str(),
		    withheld[2] ? nullptr : v[2].c_str());
}

/* a control line */
void Control::act(const char *s, dcore::Modem &m, SipLine &line)
{
	const std::string_view c = s;
	DBG("SIP: %s\n", s);
	if (c == "RING") {
		m.ring_signal(true);
	} else if (dcore::starts_with(c, "CALLER ")) {
		caller(c.substr(7), m);
	} else if (c == "CANCEL") {
		m.ring_signal(false);
	} else if (c == "T38") {
		if (m.started())
			line.t38_begin(m);
	} else if (c == "AUDIO") {
		m.t38_stop();
		line.t38_end();
	} else if (c == "ANSWERED") {
		m.line_answered();
	} else if (dcore::starts_with(c, "FAILED ")) {
		m.line_failed(failure(
			static_cast<s32>(dcore::leading_long(c.substr(7)))));
	}
}

} // namespace

/*
 *    the DTEs
 */

#ifndef _WIN32
/* see sip_modem.h */
int PtyDte::fd() const
{
	return pty_.fd();
}

/* see sip_modem.h */
bool PtyDte::want_read(const dcore::Modem &m)
{
	return pty_.want_read(m);
}

/* see sip_modem.h: the DTE closing the slave with HUPCL hangs up, and a
   new pty stands in for it */
s16 PtyDte::serve(dcore::Modem &m)
{
	return pty_.serve(m, true) < 0 ? -1 : 0;
}

/* see sip_modem.h */
s32 PtyDte::write(const char *buf, s32 n)
{
	return pty_.write(buf, n);
}
#endif

/* see sip_modem.h */
FdDte::FdDte(int fd) : fd_(fd)
{
	dmd::set_nonblocking(fd_);
}

/* see sip_modem.h */
bool FdDte::want_read(const dcore::Modem &m)
{
	return fd_ >= 0 && m.dte_space() > 0;
}

/* up to the modem's room of the DTE's characters into it; its end of
   the descriptor closed: the call hung up, and the DTE gone */
s16 FdDte::serve(dcore::Modem &m)
{
	char buf[4096];
	s32 room = m.dte_space();
	ssize_t n;

	if (room <= 0)
		return 0;
	if (room > static_cast<s32>(sizeof(buf)))
		room = sizeof(buf);
	n = dmd::fd_read(fd_, buf, room);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		ERR("DTE read: %s\n", strerror(errno));
		return -1;
	}
	if (n == 0) {
		DBG("the DTE has gone\n");
		if (m.link_state() != dcore::LinkState::Idle)
			m.hangup_now();
		return 1;
	}
	m.dte_input(buf, static_cast<int>(n));
	return 0;
}

/* what waits, as much as the descriptor takes now */
void FdDte::flush()
{
	while (!out_.empty()) {
		ssize_t n = dmd::fd_write(fd_, out_.data(),
					  static_cast<int>(out_.size()));
		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR) {
				DBG("DTE write: %s: output dropped\n",
				    strerror(errno));
				out_.clear();
			}
			return;
		}
		out_.erase(0, n);
	}
}

/* buf after what waits: written now as far as it goes, the rest kept
   while there is room */
s32 FdDte::write(const char *buf, s32 n)
{
	s32 took;

	if (fd_ < 0 || n <= 0)
		return 0;
	flush();
	if (out_.empty()) {
		ssize_t done = dmd::fd_write(fd_, buf, n);
		if (done < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR)
			return 0;
		if (done < 0)
			done = 0;
		buf += done;
		n -= static_cast<s32>(done);
		took = static_cast<s32>(done);
	} else {
		took = 0;
	}
	if (n > static_cast<s32>(BUFFER - out_.size()))
		n = static_cast<s32>(BUFFER - out_.size());
	if (n > 0)
		out_.append(buf, n);
	return took + (n > 0 ? n : 0);
}


/*
 *    the modem
 */

/* see sip_modem.h */
SipModem::SipModem(Dte &dte, const dcore::Options &opt)
	: dte_(dte), line_(new SipLine(dte))
{
	m_.reset(new dcore::Modem(*line_, opt));
	/* (a socket pair: on Windows select() takes sockets alone) */
	if (dmd::socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, wake_) < 0) {
		ERR("socketpair: %s\n", strerror(errno));
		wake_[0] = wake_[1] = -1;
		return;
	}
	for (int fd : wake_)
		dmd::set_nonblocking(fd);
}

/* the core first: it may still hang up on the line */
SipModem::~SipModem()
{
	m_.reset();
	line_.reset();
	for (int fd : wake_)
		if (fd >= 0)
			dmd::fd_close(fd);
}

/* see sip_modem.h */
bool SipModem::ok() const
{
	return m_ && m_->ok() && wake_[0] >= 0;
}

/* see sip_modem.h */
void SipModem::stop()
{
	char c = 0;

	stopped_ = true;
	if (wake_[1] >= 0 && dmd::fd_write(wake_[1], &c, 1) < 0) {
		/* (full: it is awake anyway) */
	}
}

/* see sip_modem.h */
void SipModem::on_speaker(Speaker s)
{
	line_->speaker_to = std::move(s);
}

/* the status to status_fn_ if it changed */
void SipModem::report()
{
	Status s;

	if (!status_fn_)
		return;
	s.link = m_->link_state();
	s.offhook = m_->started();
	if (s.link == dcore::LinkState::Online) {
		s.tx_rate = m_->tx_rate();
		s.rx_rate = m_->rx_rate();
	}
	if (s != status_) {
		status_ = s;
		status_fn_(s);
	}
}

/* see sip_modem.h */
s16 SipModem::run(const sip::Config &cfg)
{
	int ctl;
	s16 ret;

	if (!ok())
		return -1;
	if (stopped_)
		return 0;
	ctl = sip::start(cfg);
	if (ctl < 0) {
		ERR("cannot start the SIP user agent\n");
		return -1;
	}
	DBG("SIP: %s@%s%s\n", cfg.user, cfg.domain,
	    cfg.listen ? ", taking calls" : "");
	ret = serve(ctl);
	if (m_->link_state() != dcore::LinkState::Idle)
		m_->hangup_now();
	sip::stop();
	dmd::fd_close(ctl);
	dte_.flush();
	return ret;
}

/* until stop() or a signal (0), the DTE gone (0) or an error (-1), each
   turn: the modem's timers, then select() on the call's audio, the
   control lines, the DTE (if the modem takes its characters now, and for
   its output that waits), T.38 and stop()'s pipe */
s16 SipModem::serve(int ctl)
{
	dcore::Modem &m = *m_;
	dcore::Modem *const modems[] = { &m };
	SipLine &line = *line_;
	Control control(ctl);
	RunLoop loop;

	while (RunLoop::running() && !stopped_) {
		s32 wait = RunLoop::poll(modems, 1);
		int audio = m.started() ? line.audio_fd() : -1;
		int dte = dte_.fd();
		int ret;

		report();
		sip::set_fax(m.mode() == MODEM_MODE_FAX);
		sip::set_t38_offers(m.adaptive_answering());
		loop.clear();
		if (audio >= 0)
			loop.watch(audio);
		loop.watch(control.fd());
		loop.watch(wake_[0]);
		if (dte >= 0 && dte_.want_read(m))
			loop.watch(dte);
		if (dte >= 0 && dte_.want_write())
			loop.watch_write(dte);
		if (line.t38_fd() >= 0) {
			loop.watch(line.t38_fd());
			if (wait > T38_WAIT_MS)
				wait = T38_WAIT_MS;
		}
		ret = loop.wait(wait);
		if (ret < 0) {
			ERR("select: %s\n", strerror(errno));
			return -1;
		}
		if (loop.ready(line.t38_fd()))
			line.t38_receive(m);
		line.t38_clock(m);
		if (ret == 0)
			continue;
		if (loop.ready(wake_[0])) {
			char buf[16];
			while (dmd::fd_read(wake_[0], buf, sizeof(buf)) > 0)
				;
		}
		if (loop.ready(control.fd()) && control.serve(m, line) < 0)
			return -1;
		/* (the call it watched, not one started meanwhile) */
		if (audio >= 0 && audio == line.audio_fd() &&
		    loop.ready(audio) && line.audio(m) < 0)
			return -1;
		if (loop.ready_write(dte))
			dte_.flush();
		if (loop.ready(dte)) {
			ret = dte_.serve(m);
			if (ret < 0)
				return -1;
			if (ret > 0)
				return 0;
		}
	}
	return 0;
}

} // namespace dmd
