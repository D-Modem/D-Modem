/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 */
/*
 *
 *    sip.cpp - dmodemd's telephone line: a SIP user agent on pjsua
 *              (sip.h)
 *
 *    One call at a time, the current call.  pjsua's callbacks (on its
 *    own threads) only note what happened to it; a worker thread of
 *    ours does the rest: it takes the modem's requests (dial, answer),
 *    writes the RING and CANCEL lines, hangs calls up, and alone puts a
 *    call's audio port in the conference bridge and takes it out again.
 *
 *    The audio port carries the call's audio between the bridge and the
 *    modem's socket, a frame of 20 ms each way on every tick of the
 *    bridge's clock: 8000 Hz, 16 bit linear, nothing resampled, so that
 *    G.711's codes pass through as they are.
 *
 *    T.38: pjsua has no image media of its own, and would make an audio
 *    stream of an image line on in an audio line's place.  An offer of
 *    it (a re-INVITE) is answered here, our image line in its place off
 *    as pjsua has it (port 0: pjsua stops the audio there and makes
 *    nothing of it), the SIP messages with our UDPTL port in it (a
 *    module of ours on their way out); pjsua refuses an offer without
 *    audio itself, so it is told that the answer is ours.  Our own ask
 *    has pjsua's offer with its audio off and our image line on in a
 *    media line of its own, one more than pjsua's (pjsua skips a media
 *    line it has no type for).  What was agreed comes from the SDP
 *    negotiator's active SDPs.  (pjsua_internal.h)
 *
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>

#include <pjsua-lib/pjsua.h>
#include <pjsua-lib/pjsua_internal.h>

#include "line_recorder.h"
#include "sip.h"

#define THIS_FILE "sip.cpp"

namespace sip {

namespace {

/* the modem's audio: 8000 Hz, mono, 16 bit, frames of 20 ms */
const unsigned RATE = 8000;
const unsigned SAMPLES = RATE / 50;
const size_t FRAME = SAMPLES * 2;

/* the jitter buffer, fixed (ms) */
const int JB_DELAY = 60;
const int JB_MAX = 2000;

/* a ringing call's RING lines, and the worker's longest sleep (ms) */
const int RING_MS = 3000;
const int POLL_MS = 200;

/* T.38: our ask, after answering a fax call (ms): one sooner may cross
   the call's ACK */
const int T38_ASK_MS = 1000;

/* the far end's T.38, from its image line */
struct Peer {
	std::string addr;
	int port = 0;
	int version = 0;
	int max_datagram = 0;
	bool redundancy = false;
};

/* the current call, as pjsua's callbacks and the worker share it (under
   lock).  Every call carries a user data of its own (token), by which
   the callbacks know the current call: the id of a call being dialled
   is not known until pjsua_call_make_call() returns, and its callbacks
   may come before that */
struct Call {
	void *token = nullptr;  /* none: the line is free */
	pjsua_call_id id = PJSUA_INVALID_ID;
	bool ringing = false;   /* came in, not answered yet */
	bool media = false;     /* its media came up */
	bool gone = false;      /* disconnected */
	bool incoming = false;  /* it came in */
	bool confirmed = false;
	std::chrono::steady_clock::time_point up;       /* confirmed then */
	/* T.38 */
	bool t38_asked = false;         /* by us or the far end: not again */
	bool t38_offering = false;      /* our re-INVITE's offer: an image
					   line in the audio's place */
	bool t38_mine = false;          /* an image line of ours is in
					   pjsua's SDPs */
	pjmedia_sdp_session *offer = nullptr;   /* the far end's, to answer
						   (in offer_pool) */
	pj_pool_t *offer_pool = nullptr;
	bool t38 = false;               /* the call is on T.38 */
	bool t38_told = false;          /* the T38 line written */
	Peer peer;
	bool audio_again = false;       /* back on the audio: the port to
					   connect, an AUDIO line */
	bool no_media = false;          /* neither: the audio to ask for */
};

/* a modem's request: dial number, or answer; the call's audio on fd */
struct Request {
	bool dial;
	std::string number;
	int fd;
};

/* a call's audio port: the bridge's frames to the modem's socket, the
   modem's from it.  Its memory is its own pool's, which it outlives
   only in port_on_destroy() */
struct Port {
	pjmedia_port base = {};
	pj_pool_t *pool = nullptr;
	int fd = -1;
	pj_uint64_t ts = 0;     /* get_frame()'s timestamp */
	std::atomic<bool> failed{false};
};

/* start()'s copy of the configuration */
Config cfg_;
std::string user_, password_, domain_;

dmd::LineRecorder rec;
int ctl[2] = { -1, -1 };        /* RING and CANCEL: written to [0] */
bool pj_created;
pjsua_acc_id acc = PJSUA_INVALID_ID;
std::atomic<int> law_{0};
std::atomic<uintptr_t> last_token{0};
std::atomic<bool> fax_{false};

/* shared by the worker, pjsua's callbacks and the modem's thread */
std::mutex mu;
std::condition_variable cv;
std::deque<Request> requests;
bool running;           /* the worker takes requests */
bool quit;              /* the worker is to end */
bool poked;             /* something for the worker */
Call cur;
std::thread worker;

/* the worker's own: the current call's audio port, its slot in the
   bridge, the RING timer */
Port *port;
pjsua_conf_port_id port_slot = PJSUA_INVALID_ID;
bool rang;
std::chrono::steady_clock::time_point next_ring;

/* (under mu) the current call's UDPTL socket and its port, and the
   address of our media lines */
int udptl = -1;
int udptl_port;
std::string media_ip;

/* false, with pjsua's message for what failed, unless st is success */
bool ok(pj_status_t st, const char *what)
{
	if (st == PJ_SUCCESS)
		return true;
	pjsua_perror(THIS_FILE, what, st);
	return false;
}

/* a user data no other call has had */
void *new_token()
{
	return reinterpret_cast<void *>(++last_token);
}

/* the worker on its way, if it sleeps */
void wake()
{
	{
		std::lock_guard<std::mutex> lk(mu);
		poked = true;
	}
	cv.notify_one();
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

/* a line to main.cpp's control descriptor (RING, CANCEL) */
void ctl_line(const char *s)
{
	PJ_LOG(4, (THIS_FILE, "control: %.*s", (int)strlen(s) - 1, s));
	if (!write_all(ctl[0], s, strlen(s)))
		PJ_LOG(1, (THIS_FILE, "control socket: %s", strerror(errno)));
}

/* the modem has closed its end of fd (seen without reading from it) */
bool peer_closed(int fd)
{
	struct pollfd p = { fd, POLLRDHUP, 0 };
	return poll(&p, 1, 0) > 0 &&
		(p.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL));
}

/* the Port of a pjmedia_port */
Port *port_of(pjmedia_port *base)
{
	return static_cast<Port *>(base->port_data.pdata);
}

/* the socket has failed (what): once, the port marked and the worker
   woken */
void port_failed(Port *p, const char *what)
{
	if (p->failed.exchange(true))
		return;
	PJ_LOG(4, (THIS_FILE, "audio socket %d: %s", p->fd, what));
	wake();
}

/* a frame from the bridge to the modem (the media clock's thread):
   audio recorded and written to the socket, anything else ignored */
pj_status_t port_put_frame(pjmedia_port *base, pjmedia_frame *f)
{
	Port *p = port_of(base);
	char pad[FRAME];
	const void *buf = f->buf;

	if (f->type != PJMEDIA_FRAME_TYPE_AUDIO)
		return PJ_SUCCESS;
	if (f->size < FRAME) {
		/* (not from the bridge: its frames are whole) */
		memcpy(pad, f->buf, f->size);
		memset(pad + f->size, 0, FRAME - f->size);
		buf = pad;
	}
	rec.rx(buf, FRAME);
	if (!p->failed && !write_all(p->fd, buf, FRAME))
		port_failed(p, "write failed");
	return PJ_SUCCESS;
}

/* a frame from the modem to the bridge (the media clock's thread): read
   from the socket, which blocks until the modem has answered the last
   frame it got; silence once the socket has failed */
pj_status_t port_get_frame(pjmedia_port *base, pjmedia_frame *f)
{
	Port *p = port_of(base);

	if (p->failed || !read_all(p->fd, f->buf, FRAME)) {
		port_failed(p, "read failed or ended");
		memset(f->buf, 0, FRAME);
	}
	rec.tx(f->buf, FRAME);
	f->size = FRAME;
	f->type = PJMEDIA_FRAME_TYPE_AUDIO;
	f->timestamp.u64 = p->ts;
	p->ts += SAMPLES;
	return PJ_SUCCESS;
}

/* the last reference to the port gone, the bridge's included: only now
   are the socket closed and the memory released */
pj_status_t port_on_destroy(pjmedia_port *base)
{
	Port *p = port_of(base);
	pj_pool_t *pool = p->pool;

	PJ_LOG(4, (THIS_FILE, "audio socket %d closed", p->fd));
	close(p->fd);
	p->~Port();
	pj_pool_release(pool);
	return PJ_SUCCESS;
}

/* an audio port on the socket fd, with its own pool and group lock (the
   bridge keeps a reference while it may use it); nullptr if it can't be
   made (fd left open) */
Port *make_port(int fd)
{
	pj_pool_t *pool = pjsua_pool_create("dmodem-port", 512, 512);
	pj_str_t name;
	void *mem;
	Port *p;

	if (!pool) {
		PJ_LOG(1, (THIS_FILE, "no pool for the audio port"));
		return nullptr;
	}
	mem = pj_pool_zalloc(pool, sizeof(Port));
	if (!mem) {
		pj_pool_release(pool);
		return nullptr;
	}
	p = new (mem) Port;
	p->pool = pool;
	p->fd = fd;
	pjmedia_port_info_init(&p->base.info, pj_cstr(&name, "dmodem"),
			       PJMEDIA_SIG_CLASS_PORT_AUD('D', 'M'), RATE, 1,
			       16, SAMPLES);
	p->base.port_data.pdata = p;
	p->base.put_frame = &port_put_frame;
	p->base.get_frame = &port_get_frame;
	p->base.on_destroy = &port_on_destroy;
	if (!ok(pjmedia_port_init_grp_lock(&p->base, pool, nullptr),
		"audio port's group lock")) {
		p->~Port();
		pj_pool_release(pool);
		return nullptr;
	}
	return p;
}

/* (worker) the current call's port let go: its socket shut down (a
   read() the media thread waits in returns, and the modem sees the call
   end), out of the bridge, our reference dropped; the bridge may hold
   its own a while longer */
void release_port()
{
	if (!port)
		return;
	shutdown(port->fd, SHUT_RDWR);
	if (port_slot != PJSUA_INVALID_ID)
		ok(pjsua_conf_remove_port(port_slot),
		   "removing the audio port");
	pjmedia_port_destroy(&port->base);
	port = nullptr;
	port_slot = PJSUA_INVALID_ID;
}

/* (worker) a new call's audio on fd: its port, the law not known yet,
   and a frame of silence in the socket ahead of everything (the modem
   keeps one in flight); false (fd closed) if that can't be */
bool attach(int fd)
{
	static const char silence[FRAME] = {};

	law_ = 0;
	port = make_port(fd);
	if (!port) {
		close(fd);
		return false;
	}
	port_slot = PJSUA_INVALID_ID;
	if (!write_all(fd, silence, FRAME)) {
		PJ_LOG(1, (THIS_FILE, "audio socket %d: %s", fd,
			   strerror(errno)));
		release_port();
		return false;
	}
	return true;
}

/* (worker) the port into the bridge (unless it is there: back from
   T.38) and connected both ways with call id's slot; false if that
   fails */
bool connect_port(pjsua_call_id id)
{
	pjsua_conf_port_id call_slot = pjsua_call_get_conf_port(id);
	pjsua_conf_port_id slot;

	if (call_slot == PJSUA_INVALID_ID) {
		PJ_LOG(1, (THIS_FILE, "call %d: no audio in the bridge", id));
		return false;
	}
	if (port_slot == PJSUA_INVALID_ID) {
		if (!ok(pjsua_conf_add_port(port->pool, &port->base, &slot),
			"adding the audio port"))
			return false;
		port_slot = slot;
	}
	slot = port_slot;
	/* towards the modem first: until the modem has frames to answer,
	   the bridge's get_frame() would wait for it */
	if (!ok(pjsua_conf_connect(call_slot, slot), "connecting the call") ||
	    !ok(pjsua_conf_connect(slot, call_slot), "connecting the modem"))
		return false;
	PJ_LOG(3, (THIS_FILE, "call %d: audio connected (slots %d, %d)", id,
		   call_slot, slot));
	return true;
}

/* (lock held) whether id, which carries the user data ud, is the current
   call; the id of a call being dialled is learnt here */
bool is_current(pjsua_call_id id, void *ud)
{
	if (!cur.token || ud != cur.token)
		return false;
	if (cur.id == PJSUA_INVALID_ID)
		cur.id = id;
	return true;
}


/*
 *    T.38's SDP
 */

std::string str(const pj_str_t &s)
{
	return std::string(s.ptr ? s.ptr : "", s.slen);
}

std::string lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

/* m is a T.38 line (an image line on UDPTL) */
bool is_t38(const pjmedia_sdp_media *m)
{
	return !pj_stricmp2(&m->desc.media, "image") &&
		!pj_stricmp2(&m->desc.transport, "udptl");
}

/* the first T.38 line of s that is on, -1 if none */
int t38_line(const pjmedia_sdp_session *s)
{
	for (unsigned i = 0; i < s->media_count; i++)
		if (s->media[i]->desc.port && is_t38(s->media[i]))
			return (int)i;
	return -1;
}

/* s has an audio line on */
bool has_audio(const pjmedia_sdp_session *s)
{
	for (unsigned i = 0; i < s->media_count; i++)
		if (s->media[i]->desc.port &&
		    !pj_stricmp2(&s->media[i]->desc.media, "audio"))
			return true;
	return false;
}

/* the address of line i of s: its own, else the session's */
std::string line_addr(const pjmedia_sdp_session *s, unsigned i)
{
	const pjmedia_sdp_conn *c = s->media[i]->conn ? s->media[i]->conn :
		s->conn;
	return c ? str(c->addr) : std::string();
}

/* the T.38 settings of line i of s */
Peer t38_params(const pjmedia_sdp_session *s, unsigned i)
{
	const pjmedia_sdp_media *m = s->media[i];
	Peer p;

	p.addr = line_addr(s, i);
	p.port = m->desc.port;
	for (unsigned k = 0; k < m->attr_count; k++) {
		std::string n = lower(str(m->attr[k]->name));
		std::string v = str(m->attr[k]->value);
		if (n == "t38faxversion")
			p.version = atoi(v.c_str());
		else if (n == "t38faxmaxdatagram" || n == "t38maxdatagram")
			p.max_datagram = atoi(v.c_str());
		else if (n == "t38faxudpec")
			p.redundancy = lower(v) == "t38udpredundancy";
	}
	return p;
}

/* a connection line of addr */
pjmedia_sdp_conn *make_conn(pj_pool_t *pool, const std::string &addr)
{
	pjmedia_sdp_conn *c = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
	pj_strdup2(pool, &c->net_type, "IN");
	pj_strdup2(pool, &c->addr_type,
		   addr.find(':') != std::string::npos ? "IP6" : "IP4");
	pj_strdup2(pool, &c->addr, addr.c_str());
	return c;
}

void add_attr(pj_pool_t *pool, pjmedia_sdp_media *m, const char *name,
	      const std::string &value)
{
	pj_str_t v;
	pj_strdup2(pool, &v, value.c_str());
	pjmedia_sdp_media_add_attr(m, pjmedia_sdp_attr_create(pool, name, &v));
}

/* our T.38 line: UDPTL on port (of addr, unless empty: the session's),
   T38FaxVersion version, with redundancy, datagrams of 400 octets;
   port 0 in pjsua's SDPs (see on_tx_sdp()) */
pjmedia_sdp_media *image_line(pj_pool_t *pool, int port,
			      const std::string &addr, int version)
{
	pjmedia_sdp_media *m = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_media);

	pj_strdup2(pool, &m->desc.media, "image");
	m->desc.port = (pj_uint16_t)port;
	pj_strdup2(pool, &m->desc.transport, "udptl");
	m->desc.fmt_count = 1;
	pj_strdup2(pool, &m->desc.fmt[0], "t38");
	if (!addr.empty())
		m->conn = make_conn(pool, addr);
	add_attr(pool, m, "T38FaxVersion", std::to_string(version));
	add_attr(pool, m, "T38MaxBitRate", "14400");
	add_attr(pool, m, "T38FaxRateManagement", "transferredTCF");
	add_attr(pool, m, "T38FaxMaxBuffer", "2000");
	add_attr(pool, m, "T38FaxMaxDatagram", "400");
	add_attr(pool, m, "T38FaxUdpEC", "t38UDPRedundancy");
	return m;
}

/* (pjsua, with call id's dialog locked) the T.38 that the call's active
   SDPs agree on: our image line (off, as pjsua has it) and the far
   end's on, the far end's (the lower version of the two); false if none
   (pjsua_internal.h: pjsua has no call of its own for them) */
bool active_t38(pjsua_call_id id, Peer *p)
{
	pjsip_inv_session *inv = pjsua_var.calls[id].inv;
	const pjmedia_sdp_session *l, *r;

	if (!inv || !inv->neg ||
	    pjmedia_sdp_neg_get_active_local(inv->neg, &l) != PJ_SUCCESS ||
	    pjmedia_sdp_neg_get_active_remote(inv->neg, &r) != PJ_SUCCESS)
		return false;
	for (unsigned i = 0; i < l->media_count && i < r->media_count; i++) {
		if (!is_t38(l->media[i]) || !is_t38(r->media[i]) ||
		    !r->media[i]->desc.port)
			continue;
		*p = t38_params(r, i);
		p->version = std::min(p->version, t38_params(l, i).version);
		return true;
	}
	return false;
}

/* (worker) the call's UDPTL socket, on a port of its own, of the
   family of our media's address; false if it can't be had */
bool open_udptl()
{
	struct sockaddr_storage a = {};
	socklen_t len = sizeof(a);
	bool v6;
	int fd, port;

	{
		std::lock_guard<std::mutex> lk(mu);
		if (udptl >= 0)
			return true;
		v6 = media_ip.find(':') != std::string::npos;
	}
	a.ss_family = v6 ? AF_INET6 : AF_INET;
	fd = socket(a.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || bind(fd, reinterpret_cast<struct sockaddr *>(&a),
			   v6 ? sizeof(struct sockaddr_in6) :
			   sizeof(struct sockaddr_in)) < 0 ||
	    getsockname(fd, reinterpret_cast<struct sockaddr *>(&a),
			&len) < 0) {
		PJ_LOG(1, (THIS_FILE, "the UDPTL socket: %s", strerror(errno)));
		if (fd >= 0)
			close(fd);
		return false;
	}
	port = ntohs(v6 ? reinterpret_cast<struct sockaddr_in6 *>(&a)->sin6_port :
		     reinterpret_cast<struct sockaddr_in *>(&a)->sin_port);
	std::lock_guard<std::mutex> lk(mu);
	udptl = fd;
	udptl_port = port;
	return true;
}

/* an outgoing SIP message: in its SDP our image line, off as pjsua has
   it, gets our UDPTL port (the SDP pjsip's own, its body still to be
   printed: a copy of it changed) */
pj_status_t on_tx_sdp(pjsip_tx_data *tdata)
{
	pjsip_msg_body *b = tdata->msg ? tdata->msg->body : nullptr;
	pjmedia_sdp_session *sdp;
	int port;

	if (!b || b->len || !b->data ||
	    pj_stricmp2(&b->content_type.type, "application") ||
	    pj_stricmp2(&b->content_type.subtype, "sdp"))
		return PJ_SUCCESS;
	{
		std::lock_guard<std::mutex> lk(mu);
		if (!cur.token || !cur.t38_mine || udptl < 0)
			return PJ_SUCCESS;
		port = udptl_port;
	}
	sdp = pjmedia_sdp_session_clone(tdata->pool,
		static_cast<const pjmedia_sdp_session *>(b->data));
	for (unsigned i = 0; i < sdp->media_count; i++)
		if (is_t38(sdp->media[i]) && !sdp->media[i]->desc.port)
			sdp->media[i]->desc.port = (pj_uint16_t)port;
	b->data = sdp;
	return PJ_SUCCESS;
}

pjsip_module mod_t38 = {
	nullptr, nullptr,                       /* prev, next */
	{ const_cast<char *>("mod-dmodem-t38"), 14 },
	-1,                                     /* id */
	PJSIP_MOD_PRIORITY_APPLICATION,         /* before the message is
						   printed */
	nullptr, nullptr, nullptr, nullptr,     /* load, start, stop,
						   unload */
	nullptr, nullptr,                       /* on_rx_request, response */
	&on_tx_sdp, &on_tx_sdp,                 /* on_tx_request, response */
	nullptr,                                /* on_tsx_state */
};

/* (pjsua, call id's dialog locked) an SDP of ours: the address of its
   audio noted.  The offer of our T.38 ask (the worker's re-INVITE): its
   audio off, our T.38 line after the rest, on, for a media line of
   pjsua's that has no type yet (none at all if there is none) */
void on_call_sdp_created(pjsua_call_id id, pjmedia_sdp_session *sdp,
			 pj_pool_t *pool, const pjmedia_sdp_session *rem_sdp)
{
	void *ud = pjsua_call_get_user_data(id);
	pjsua_call *call = &pjsua_var.calls[id];
	std::lock_guard<std::mutex> lk(mu);
	unsigned k = call->med_prov_cnt;
	int audio = -1;
	std::string a;

	if (!is_current(id, ud))
		return;
	for (unsigned i = 0; i < sdp->media_count && audio < 0; i++)
		if (sdp->media[i]->desc.port &&
		    !pj_stricmp2(&sdp->media[i]->desc.media, "audio"))
			audio = (int)i;
	if (audio < 0)
		return;
	a = line_addr(sdp, audio);
	if (!a.empty())
		media_ip = a;
	if (rem_sdp || !cur.t38_offering || udptl < 0)
		return;
	if (k != sdp->media_count || k >= PJSUA_MAX_CALL_MEDIA ||
	    k >= PJMEDIA_MAX_SDP_MEDIA || call->media_prov[k].tp ||
	    call->media_prov[k].type != PJMEDIA_TYPE_NONE) {
		PJ_LOG(1, (THIS_FILE, "call %d: no media line for T.38", id));
		return;
	}
	pjmedia_sdp_media_deactivate(pool, sdp->media[audio]);
	sdp->media[sdp->media_count++] = image_line(pool, udptl_port,
		sdp->media[audio]->conn ? a : std::string(), 0);
	call->med_prov_cnt = k + 1;
	cur.t38_mine = true;
	PJ_LOG(3, (THIS_FILE, "call %d: asking for T.38 (UDPTL port %d)", id,
		   udptl_port));
}

/* (pjsua) a re-INVITE: an offer of T.38 (an image line) taken while the
   modem is in fax mode, for the worker to answer (pjsua would refuse
   one without audio: told that the answer is ours, which
   pjsua_call_answer_with_sdp() gives later); else one without audio
   refused (488), the rest pjsua's */
void on_call_rx_reinvite(pjsua_call_id id, const pjmedia_sdp_session *offer,
			 pjsip_rx_data *, void *, pj_bool_t *async,
			 pjsip_status_code *code, pjsua_call_setting *)
{
	void *ud = pjsua_call_get_user_data(id);
	pj_pool_t *pool;
	bool take;

	if (t38_line(offer) < 0)
		return;
	{
		std::lock_guard<std::mutex> lk(mu);
		take = is_current(id, ud) && cfg_.t38 && fax_;
		/* (pjsua's answer would have more lines than its media) */
		if (take && offer->media_count > pjsua_var.calls[id].med_cnt) {
			PJ_LOG(3, (THIS_FILE, "call %d: T.38 offered with %u "
				   "media lines, the call has %u", id,
				   offer->media_count,
				   pjsua_var.calls[id].med_cnt));
			take = false;
		}
		if (!take && is_current(id, ud))
			cur.t38_mine = false;
	}
	pool = take ? pjsua_pool_create("dmodem-t38", 1024, 1024) : nullptr;
	if (!pool) {
		PJ_LOG(3, (THIS_FILE, "call %d: T.38 offered, not taken", id));
		if (!has_audio(offer))
			*code = PJSIP_SC_NOT_ACCEPTABLE_HERE;
		return;
	}
	PJ_LOG(3, (THIS_FILE, "call %d: T.38 offered", id));
	{
		std::lock_guard<std::mutex> lk(mu);
		if (cur.offer_pool)
			pj_pool_release(cur.offer_pool);
		cur.offer = pjmedia_sdp_session_clone(pool, offer);
		cur.offer_pool = pool;
		cur.t38_asked = true;
		poked = true;
	}
	cv.notify_one();
	*async = PJ_TRUE;
	pjsua_var.calls[id].rx_reinv_async = PJ_TRUE;
}

/* (worker) the far end's T.38 offer answered: our T.38 line in its
   image line's place (the version the offer's, up to 3), the other
   lines off; 488 if there is no UDPTL socket for it */
void answer_t38(pjsua_call_id id)
{
	pjmedia_sdp_session *offer, *sdp = nullptr;
	pj_pool_t *pool;
	pj_sockaddr origin;
	pj_str_t ip_str;
	std::string ip;
	int i, port = 0;

	{
		std::lock_guard<std::mutex> lk(mu);
		offer = cur.offer;
		pool = cur.offer_pool;
		cur.offer = nullptr;
		cur.offer_pool = nullptr;
		ip = media_ip;
	}
	if (!offer)
		return;
	i = t38_line(offer);
	if (open_udptl()) {
		std::lock_guard<std::mutex> lk(mu);
		port = udptl_port;
		cur.t38_mine = true;
	}
	if (port && !ip.empty() &&
	    pj_sockaddr_parse(pj_AF_UNSPEC(), 0, pj_cstr(&ip_str, ip.c_str()),
			      &origin) == PJ_SUCCESS &&
	    pjmedia_endpt_create_base_sdp(pjsua_get_pjmedia_endpt(), pool,
					  nullptr, &origin, &sdp) == PJ_SUCCESS) {
		sdp->conn = make_conn(pool, ip);
		for (unsigned k = 0; k < offer->media_count; k++)
			sdp->media[sdp->media_count++] = (int)k == i ?
				image_line(pool, 0, std::string(),
					   std::min(t38_params(offer, i).version,
						    3)) :
				pjmedia_sdp_media_clone_deactivate(pool,
							offer->media[k]);
		PJ_LOG(3, (THIS_FILE, "call %d: T.38 taken (UDPTL port %d)", id,
			   port));
		ok(pjsua_call_answer_with_sdp(id, sdp, nullptr, PJSIP_SC_OK,
					      nullptr, nullptr),
		   "answering the T.38 offer");
	} else {
		ok(pjsua_call_answer(id, PJSIP_SC_NOT_ACCEPTABLE_HERE, nullptr,
				     nullptr), "refusing the T.38 offer");
	}
	pj_pool_release(pool);
}

/* (worker) our T.38 ask: a re-INVITE whose offer on_call_sdp_created()
   makes T.38's (once only, taken or not) */
void ask_t38(pjsua_call_id id)
{
	{
		std::lock_guard<std::mutex> lk(mu);
		cur.t38_asked = true;
	}
	if (!open_udptl())
		return;
	{
		std::lock_guard<std::mutex> lk(mu);
		cur.t38_offering = true;
	}
	ok(pjsua_call_reinvite2(id, nullptr, nullptr), "asking for T.38");
	std::lock_guard<std::mutex> lk(mu);
	cur.t38_offering = false;
}


/* (pjsua) an INVITE: 480 unless listening, 486 if the line is busy
   (a call being dialled too), else the call is current and rings */
void on_incoming_call(pjsua_acc_id, pjsua_call_id id, pjsip_rx_data *)
{
	void *token = new_token();
	pjsua_call_info ci;
	unsigned code;

	pjsua_call_set_user_data(id, token);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (!cfg_.listen || !running) {
			code = PJSIP_SC_TEMPORARILY_UNAVAILABLE;
		} else if (cur.token) {
			code = PJSIP_SC_BUSY_HERE;
		} else {
			cur = Call();
			cur.token = token;
			cur.id = id;
			cur.ringing = true;
			cur.incoming = true;
			code = PJSIP_SC_RINGING;
		}
	}
	if (pjsua_call_get_info(id, &ci) == PJ_SUCCESS)
		PJ_LOG(3, (THIS_FILE, "call %d from %.*s: %u", id,
			   (int)ci.remote_info.slen, ci.remote_info.ptr, code));
	ok(pjsua_call_answer(id, code, nullptr, nullptr),
	   "answering the INVITE");
	if (code == PJSIP_SC_RINGING)
		wake();
}

/* (pjsua) a call's state; the current call disconnected is the worker's
   to end */
void on_call_state(pjsua_call_id id, pjsip_event *)
{
	pjsua_call_info ci;
	void *ud;

	if (pjsua_call_get_info(id, &ci) != PJ_SUCCESS)
		return;
	if (ci.state != PJSIP_INV_STATE_DISCONNECTED) {
		PJ_LOG(3, (THIS_FILE, "call %d: %.*s", id,
			   (int)ci.state_text.slen, ci.state_text.ptr));
		if (ci.state != PJSIP_INV_STATE_CONFIRMED)
			return;
		ud = pjsua_call_get_user_data(id);
		{
			std::lock_guard<std::mutex> lk(mu);
			if (!is_current(id, ud) || cur.confirmed)
				return;
			cur.confirmed = true;
			cur.up = std::chrono::steady_clock::now();
			poked = true;
		}
		cv.notify_one();
		return;
	}
	PJ_LOG(3, (THIS_FILE, "call %d: %.*s (%d %.*s)", id,
		   (int)ci.state_text.slen, ci.state_text.ptr, ci.last_status,
		   (int)ci.last_status_text.slen, ci.last_status_text.ptr));
	ud = pjsua_call_get_user_data(id);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (!is_current(id, ud))
			return;
		cur.gone = true;
		poked = true;
	}
	cv.notify_one();
}

/* the G.711 law of call id's audio stream (as law()), 0 if neither */
int stream_law(pjsua_call_id id, const pjsua_call_info &ci)
{
	for (unsigned i = 0; i < ci.media_cnt; i++) {
		pjsua_stream_info si;
		if (ci.media[i].type != PJMEDIA_TYPE_AUDIO ||
		    ci.media[i].status != PJSUA_CALL_MEDIA_ACTIVE ||
		    pjsua_call_get_stream_info(id, i, &si) != PJ_SUCCESS ||
		    si.type != PJMEDIA_TYPE_AUDIO)
			continue;
		if (!pj_stricmp2(&si.info.aud.fmt.encoding_name, "PCMU"))
			return 1;
		if (!pj_stricmp2(&si.info.aud.fmt.encoding_name, "PCMA"))
			return 2;
	}
	return 0;
}

/* (pjsua) a call's media: the first time the current call's is up, its
   law noted and the worker to connect its audio; over to T.38, and
   back to the audio (the port to connect again); neither after our
   T.38 ask (its image line refused with its port 0, the audio gone
   with it), the worker to ask for the audio */
void on_call_media_state(pjsua_call_id id)
{
	pjsua_call_info ci;
	Peer p;
	bool t38, audio;
	void *ud;
	int l = 0;

	if (pjsua_call_get_info(id, &ci) != PJ_SUCCESS)
		return;
	t38 = active_t38(id, &p);
	if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE)
		l = stream_law(id, ci);
	ud = pjsua_call_get_user_data(id);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (!is_current(id, ud))
			return;
		/* (an image line off of pjsua's own, an offer not taken) */
		t38 = t38 && cur.t38_mine;
		audio = !t38 && ci.media_status == PJSUA_CALL_MEDIA_ACTIVE;
		if (t38) {
			if (cur.t38)
				return;
			cur.t38 = true;
			cur.t38_told = false;
			cur.peer = p;
		} else if (audio && cur.t38) {
			cur.t38 = false;
			cur.t38_mine = false;
			cur.audio_again = true;
		} else if (audio && !cur.media) {
			cur.media = true;
			law_ = l;
		} else if (!audio && cur.media && cur.t38_asked && !cur.t38 &&
			   (ci.media_status == PJSUA_CALL_MEDIA_NONE ||
			    ci.media_status == PJSUA_CALL_MEDIA_ERROR)) {
			/* our image line stays off in the ask for the audio */
			cur.t38_mine = false;
			cur.no_media = true;
		} else {
			return;
		}
		poked = true;
	}
	if (t38)
		PJ_LOG(3, (THIS_FILE, "call %d: T.38 on, to %s:%d (version %d%s)",
			   id, p.addr.c_str(), p.port, p.version,
			   p.redundancy ? ", redundancy" : ""));
	else if (audio)
		PJ_LOG(3, (THIS_FILE, "call %d: media up, %s", id,
			   l == 1 ? "PCMU" : l == 2 ? "PCMA" : "not G.711"));
	else
		PJ_LOG(3, (THIS_FILE, "call %d: no media", id));
	cv.notify_one();
}

/* (worker) the current call ended: hung up if it is still there, its
   port released, the line free */
void end_call()
{
	Call c;

	{
		std::lock_guard<std::mutex> lk(mu);
		c = cur;
	}
	/* the socket first: the media thread may be waiting on it, and
	   the bridge with it */
	if (port)
		shutdown(port->fd, SHUT_RDWR);
	if (c.token && c.id != PJSUA_INVALID_ID && !c.gone &&
	    pjsua_call_get_user_data(c.id) == c.token) {
		PJ_LOG(3, (THIS_FILE, "call %d: hanging up", c.id));
		ok(pjsua_call_hangup(c.id, 0, nullptr, nullptr), "hanging up");
	}
	release_port();
	{
		std::lock_guard<std::mutex> lk(mu);
		if (cur.offer_pool)
			pj_pool_release(cur.offer_pool);
		cur = Call();
		if (udptl >= 0)
			close(udptl);
		udptl = -1;
	}
	rang = false;
}

/* (worker) dial number with the call's audio on fd, unless the line is
   busy (fd closed) */
void do_dial(const std::string &number, int fd)
{
	void *token = new_token();
	std::string uri = "sip:" + dial_user(number.c_str()) + "@" + domain_;
	pjsua_call_id id = PJSUA_INVALID_ID;
	pj_str_t dst;
	pj_status_t st;

	{
		std::lock_guard<std::mutex> lk(mu);
		if (!cur.token) {
			/* busy from here on, the INVITE not yet sent */
			cur = Call();
			cur.token = token;
		} else {
			token = nullptr;
		}
	}
	if (!token) {
		PJ_LOG(3, (THIS_FILE, "dial %s: the line is busy",
			   number.c_str()));
		close(fd);
		return;
	}
	if (!attach(fd)) {
		end_call();
		return;
	}
	PJ_LOG(3, (THIS_FILE, "dialling %s", uri.c_str()));
	st = pjsua_call_make_call(acc, pj_cstr(&dst, uri.c_str()), nullptr,
				  token, nullptr, &id);
	if (!ok(st, "making the call")) {
		end_call();
		return;
	}
	std::lock_guard<std::mutex> lk(mu);
	if (cur.token == token)
		cur.id = id;
}

/* (worker) answer the call that rings with its audio on fd; fd closed
   if none rings */
void do_answer(int fd)
{
	pjsua_call_id id = PJSUA_INVALID_ID;

	{
		std::lock_guard<std::mutex> lk(mu);
		if (cur.token && cur.ringing && !cur.gone) {
			cur.ringing = false;
			id = cur.id;
		}
	}
	if (id == PJSUA_INVALID_ID) {
		PJ_LOG(3, (THIS_FILE, "answer: no call rings"));
		close(fd);
		return;
	}
	if (!attach(fd)) {
		end_call();
		return;
	}
	PJ_LOG(3, (THIS_FILE, "call %d: answering", id));
	if (!ok(pjsua_call_answer(id, PJSIP_SC_OK, nullptr, nullptr),
		"answering the call"))
		end_call();
}

/* (worker) what has happened to the current call: disconnected (CANCEL
   if it still rang), the modem's socket failed, its media up, the RING
   timer; T.38: an offer to answer, our ask, T38 and AUDIO lines */
void check_call()
{
	Call c;
	bool tell_t38 = false, again = false, restore = false;

	{
		std::lock_guard<std::mutex> lk(mu);
		c = cur;
		if (cur.t38 && !cur.t38_told)
			tell_t38 = cur.t38_told = true;
		again = cur.audio_again;
		restore = cur.no_media;
		cur.audio_again = cur.no_media = false;
	}
	if (!c.token)
		return;
	if (c.gone) {
		if (c.ringing)
			ctl_line("CANCEL\n");
		end_call();
		return;
	}
	if (port && !port->failed && peer_closed(port->fd))
		port_failed(port, "closed by the modem");
	if (port && port->failed) {
		PJ_LOG(3, (THIS_FILE, "call %d: the modem has gone", c.id));
		end_call();
		return;
	}
	if (c.media && port && (port_slot == PJSUA_INVALID_ID || again) &&
	    !connect_port(c.id)) {
		end_call();
		return;
	}
	if (c.offer)
		answer_t38(c.id);
	if (tell_t38)
		ctl_line("T38\n");
	if (again)
		ctl_line("AUDIO\n");
	if (restore) {
		PJ_LOG(3, (THIS_FILE, "call %d: asking for the audio again",
			   c.id));
		ok(pjsua_call_reinvite2(c.id, nullptr, nullptr),
		   "asking for the audio");
	}
	if (cfg_.t38 && fax_ && c.incoming && c.confirmed && !c.t38_asked &&
	    std::chrono::steady_clock::now() >=
	    c.up + std::chrono::milliseconds(T38_ASK_MS))
		ask_t38(c.id);
	if (c.ringing) {
		auto now = std::chrono::steady_clock::now();
		if (!rang || now >= next_ring) {
			ctl_line("RING\n");
			rang = true;
			next_ring = now + std::chrono::milliseconds(RING_MS);
		}
	}
}

/* the worker thread: the modem's requests and the current call's events
   as they come, and a look at the call every POLL_MS at least; at the
   end the call ended */
void serve()
{
	pj_thread_desc desc;
	pj_thread_t *self = nullptr;
	std::unique_lock<std::mutex> lk(mu, std::defer_lock);

	pj_bzero(desc, sizeof(desc));
	if (pj_thread_register("dmodem-sip", desc, &self) != PJ_SUCCESS) {
		fprintf(stderr, "error: the SIP thread can't use pjlib\n");
		return;
	}
	lk.lock();
	while (!quit) {
		std::deque<Request> todo;

		cv.wait_for(lk, std::chrono::milliseconds(POLL_MS), [] {
			return quit || poked || !requests.empty();
		});
		if (quit)
			break;
		poked = false;
		todo.swap(requests);
		lk.unlock();
		check_call();
		for (const Request &r : todo) {
			if (r.dial)
				do_dial(r.number, r.fd);
			else
				do_answer(r.fd);
		}
		lk.lock();
	}
	lk.unlock();
	end_call();
}

/* a request for the worker; without one, fd closed at once */
void queue(Request r)
{
	int fd = r.fd;
	bool queued = false;

	{
		std::lock_guard<std::mutex> lk(mu);
		if (running) {
			requests.push_back(std::move(r));
			poked = queued = true;
		}
	}
	if (queued)
		cv.notify_one();
	else
		close(fd);
}

/* only G.711, mu-law first: every other codec disabled */
bool set_codecs()
{
	pjsua_codec_info ci[PJMEDIA_CODEC_MGR_MAX_CODECS];
	unsigned n = PJ_ARRAY_SIZE(ci);
	pj_str_t id;

	if (!ok(pjsua_enum_codecs(ci, &n), "listing the codecs"))
		return false;
	for (unsigned i = 0; i < n; i++)
		pjsua_codec_set_priority(&ci[i].codec_id, 0);
	return ok(pjsua_codec_set_priority(pj_cstr(&id, "PCMU/8000/1"),
					   PJMEDIA_CODEC_PRIO_HIGHEST),
		  "enabling PCMU") &&
	       ok(pjsua_codec_set_priority(pj_cstr(&id, "PCMA/8000/1"),
					   PJMEDIA_CODEC_PRIO_NEXT_HIGHER),
		  "enabling PCMA");
}

/* the account, the default: user@domain, registered with the domain
   only when listening */
bool add_account()
{
	std::string id = "sip:" + user_ + "@" + domain_;
	std::string reg = "sip:" + domain_;
	pjsua_acc_config ac;

	pjsua_acc_config_default(&ac);
	pj_cstr(&ac.id, id.c_str());
	pj_cstr(&ac.reg_uri, reg.c_str());
	ac.register_on_acc_add = cfg_.listen ? PJ_TRUE : PJ_FALSE;
	ac.cred_count = 1;
	pj_cstr(&ac.cred_info[0].realm, "*");
	pj_cstr(&ac.cred_info[0].scheme, "digest");
	pj_cstr(&ac.cred_info[0].username, user_.c_str());
	ac.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
	pj_cstr(&ac.cred_info[0].data, password_.c_str());
	return ok(pjsua_acc_add(&ac, PJ_TRUE, &acc), "adding the account");
}

/* pjsua created, set up for a modem's calls and started, with its
   account; false (with pjsua's message) if any of it fails */
bool start_pjsua()
{
	pjsua_config ua;
	pjsua_logging_config lc;
	pjsua_media_config mc;
	pjsua_transport_config tc;
	const char *level = getenv("SIP_LOG_LEVEL");

	if (!ok(pjsua_create(), "pjsua_create"))
		return false;
	pj_created = true;

	pjsua_config_default(&ua);
	ua.cb.on_incoming_call = &on_incoming_call;
	ua.cb.on_call_state = &on_call_state;
	ua.cb.on_call_media_state = &on_call_media_state;
	ua.cb.on_call_sdp_created = &on_call_sdp_created;
	ua.cb.on_call_rx_reinvite = &on_call_rx_reinvite;

	pjsua_logging_config_default(&lc);
	lc.console_level = level && *level ? atoi(level) : 4;
	if (lc.level < lc.console_level)
		lc.level = lc.console_level;

	/* the modem's samples as they are: the bridge at their rate and
	   frame, no VAD, no echo canceller; a fixed jitter buffer that
	   never drops a frame to shrink (a slip the modem can't take) */
	pjsua_media_config_default(&mc);
	mc.clock_rate = RATE;
	mc.snd_clock_rate = RATE;
	mc.channel_count = 1;
	mc.audio_frame_ptime = 1000 * SAMPLES / RATE;
	mc.no_vad = PJ_TRUE;
	mc.ec_tail_len = 0;
	mc.jb_init = JB_DELAY;
	mc.jb_min_pre = JB_DELAY;
	mc.jb_max_pre = JB_DELAY;
	mc.jb_max = JB_MAX;
	mc.jb_discard_algo = PJMEDIA_JB_DISCARD_NONE;

	if (!ok(pjsua_init(&ua, &lc, &mc), "pjsua_init") ||
	    !ok(pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &mod_t38),
		"T.38's module") ||
	    !ok(pjsua_set_ec(0, 0), "turning the echo canceller off") ||
	    !ok(pjsua_set_null_snd_dev(), "the null sound device") ||
	    !set_codecs())
		return false;

	pjsua_transport_config_default(&tc);
	tc.port = cfg_.port;
	if (!ok(pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tc, nullptr),
		"the UDP transport") ||
	    !ok(pjsua_start(), "pjsua_start"))
		return false;
	return add_account();
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
	cfg_ = cfg;
	user_ = cfg.user ? cfg.user : "";
	password_ = cfg.password ? cfg.password : "";
	domain_ = cfg.domain ? cfg.domain : "";
	if (cfg.record)
		rec.open(cfg.record, ".rx", ".tx");

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) < 0) {
		fprintf(stderr, "error: socketpair: %s\n", strerror(errno));
		ctl[0] = ctl[1] = -1;
		rec.close();
		return -1;
	}
	if (start_pjsua()) {
		std::lock_guard<std::mutex> lk(mu);
		cur = Call();
		quit = poked = false;
		running = true;
		try {
			worker = std::thread(serve);
			return ctl[1];
		} catch (const std::system_error &e) {
			fprintf(stderr, "error: the SIP thread: %s\n",
				e.what());
			running = false;
		}
	}
	if (pj_created)
		pjsua_destroy();
	pj_created = false;
	close(ctl[0]);
	close(ctl[1]);
	ctl[0] = ctl[1] = -1;
	rec.close();
	return -1;
}

/* see sip.h */
void stop()
{
	{
		std::lock_guard<std::mutex> lk(mu);
		quit = true;
		running = false;
	}
	cv.notify_one();
	if (worker.joinable())
		worker.join();
	{
		/* what the worker did not get to */
		std::lock_guard<std::mutex> lk(mu);
		for (const Request &r : requests)
			close(r.fd);
		requests.clear();
	}
	if (pj_created)
		pjsua_destroy();
	pj_created = false;
	rec.close();
	if (ctl[0] >= 0)
		close(ctl[0]);
	ctl[0] = -1;
}

/* see sip.h */
void dial(const char *number, int audio_fd)
{
	queue(Request{ true, number ? number : "", audio_fd });
}

/* see sip.h */
void answer(int audio_fd)
{
	queue(Request{ false, std::string(), audio_fd });
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
bool t38(T38 *t)
{
	std::lock_guard<std::mutex> lk(mu);
	struct sockaddr_in *a4 = reinterpret_cast<struct sockaddr_in *>(&t->peer);
	struct sockaddr_in6 *a6 =
		reinterpret_cast<struct sockaddr_in6 *>(&t->peer);

	if (!cur.token || !cur.t38 || udptl < 0 || cur.peer.port <= 0)
		return false;
	memset(&t->peer, 0, sizeof(t->peer));
	if (inet_pton(AF_INET, cur.peer.addr.c_str(), &a4->sin_addr) == 1) {
		a4->sin_family = AF_INET;
		a4->sin_port = htons((uint16_t)cur.peer.port);
		t->peer_len = sizeof(*a4);
	} else if (inet_pton(AF_INET6, cur.peer.addr.c_str(),
			     &a6->sin6_addr) == 1) {
		a6->sin6_family = AF_INET6;
		a6->sin6_port = htons((uint16_t)cur.peer.port);
		t->peer_len = sizeof(*a6);
	} else {
		PJ_LOG(1, (THIS_FILE, "T.38: no address in %s",
			   cur.peer.addr.c_str()));
		return false;
	}
	t->fd = fcntl(udptl, F_DUPFD_CLOEXEC, 0);
	if (t->fd < 0)
		return false;
	t->version = cur.peer.version;
	t->max_datagram = cur.peer.max_datagram;
	t->redundancy = cur.peer.redundancy;
	return true;
}

} // namespace sip
