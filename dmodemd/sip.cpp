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
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* (worker) the port into the bridge and connected both ways with call
   id's slot; false if that fails */
bool connect_port(pjsua_call_id id)
{
	pjsua_conf_port_id call_slot = pjsua_call_get_conf_port(id);
	pjsua_conf_port_id slot;

	if (call_slot == PJSUA_INVALID_ID) {
		PJ_LOG(1, (THIS_FILE, "call %d: no audio in the bridge", id));
		return false;
	}
	if (!ok(pjsua_conf_add_port(port->pool, &port->base, &slot),
		"adding the audio port"))
		return false;
	port_slot = slot;
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
   law noted and the worker to connect its audio */
void on_call_media_state(pjsua_call_id id)
{
	pjsua_call_info ci;
	void *ud;
	int l;

	if (pjsua_call_get_info(id, &ci) != PJ_SUCCESS ||
	    ci.media_status != PJSUA_CALL_MEDIA_ACTIVE)
		return;
	l = stream_law(id, ci);
	ud = pjsua_call_get_user_data(id);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (!is_current(id, ud) || cur.media)
			return;
		cur.media = true;
		law_ = l;
		poked = true;
	}
	PJ_LOG(3, (THIS_FILE, "call %d: media up, %s", id,
		   l == 1 ? "PCMU" : l == 2 ? "PCMA" : "not G.711"));
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
		cur = Call();
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
   timer */
void check_call()
{
	Call c;

	{
		std::lock_guard<std::mutex> lk(mu);
		c = cur;
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
	if (c.media && port && port_slot == PJSUA_INVALID_ID &&
	    !connect_port(c.id)) {
		end_call();
		return;
	}
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

} // namespace sip
