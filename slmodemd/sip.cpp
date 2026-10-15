/*
 * Copyright (C) 2021 Aon plc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *
 *    sip.cpp - slmodemd's telephone line: the SIP user agent of the
 *              former d-modem program (pjsua), now a part of slmodemd
 *
 *    pjsua runs its own threads: the SIP stack's, and the media clock
 *    that moves the call's audio through the conference bridge, through
 *    our port and the modem's audio socket, a frame of 20 ms at a time.
 *    One more thread of ours, the service thread, does what the modem
 *    asks for (dial, answer) and looks after the call: it rings the modem
 *    while a call waits, puts the call's port in the bridge, and cleans
 *    up when the call or the modem's socket goes away.  The current
 *    call (cur_call) is the SIP thread's and the service thread's; the
 *    port and its place in the bridge are the service thread's alone.
 *
 *    Each call has a port of its own, with its own pool and group lock:
 *    the bridge removes a port in its own time (pjproject 2.15 on), and
 *    frees it once the last user is done, so the port's socket is closed
 *    then (on_destroy), not under the media clock.
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
#include <new>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <pjsua-lib/pjsua.h>

#include "sip.h"

namespace sip {

namespace {

constexpr unsigned rate = 8000;
constexpr unsigned frame_samples = rate / 50;   /* 20 ms */
constexpr int frame_bytes = frame_samples * 2;

/* the modem's side of the bridge for one call: the call's audio
   socket, in a pool of its own (freed with the port, by on_destroy) */
struct CallPort {
	pjmedia_port base;      /* (first: the media callbacks get it) */
	pj_pool_t *pool;
	pj_timestamp timestamp;
	int sock;
	std::atomic<bool> failed{false};        /* the socket failed */
};

/* what the modem asks for, done in the service thread */
struct Request {
	enum Kind { DIAL, ANSWER } kind;
	std::string number;
	int fd;
};

/* cur_call while pjsua_call_make_call() runs: the line is busy, and the
   call is ours if its user data is dial_tag (an id no call has) */
constexpr pjsua_call_id DIALING = PJSUA_MAX_CALLS;

Config cfg;
pj_pool_t *pool;
pj_caching_pool cpool;
pjsua_acc_id acc_id = PJSUA_INVALID_ID;

/* the service thread's: the current call's port, its place in the
   bridge */
CallPort *cur_port;
pjsua_conf_port_id conf_id = PJSUA_INVALID_ID;

std::atomic<pjsua_call_id> cur_call{PJSUA_INVALID_ID};
std::atomic<uintptr_t> dial_tag{0};     /* the outgoing call's user data */
std::atomic<bool> ringing{false};       /* cur_call waits for ANSWER */
std::atomic<bool> hangup_req{false};    /* the call ended (SIP) */
std::atomic<bool> media_ready{false};   /* cur_call's media is up */
std::atomic<bool> cancel_req{false};    /* tell the modem: no more RING */
std::atomic<int> call_law{0};

std::mutex req_lock;
std::deque<Request> requests;
int wake[2] = {-1, -1};                 /* service thread wake up */
int ctl[2] = {-1, -1};                  /* RING / CANCEL to the modem */
std::thread service;
std::atomic<bool> running{false};

FILE *rec_rx, *rec_tx;

/* calls into pjsua from a thread of ours */
void pj_register_thread()
{
	static thread_local pj_thread_desc desc;
	static thread_local pj_thread_t *thread;
	if (!pj_thread_is_registered())
		pj_thread_register("slmodemd", desc, &thread);
}

/* the bridge's audio for the modem (in the media clock's thread): a
   frame of 20 ms onto the port's socket (and $DMODEM_REC.rx); a failed
   write ends the call */
pj_status_t put_frame(pjmedia_port *p, pjmedia_frame *frame)
{
	CallPort *cp = reinterpret_cast<CallPort *>(p);
	if (frame->type != PJMEDIA_FRAME_TYPE_AUDIO)
		return PJ_SUCCESS;
	if (rec_rx)
		fwrite(frame->buf, 1, frame_bytes, rec_rx);
	if (write(cp->sock, frame->buf, frame_bytes) != frame_bytes)
		cp->failed = true;
	return PJ_SUCCESS;
}

/* the modem's audio for the bridge: a frame read from the port's socket
   (blocking: the modem answers each frame with one); silence when it
   fails, which ends the call */
pj_status_t get_frame(pjmedia_port *p, pjmedia_frame *frame)
{
	CallPort *cp = reinterpret_cast<CallPort *>(p);
	if (read(cp->sock, frame->buf, frame_bytes) != frame_bytes) {
		cp->failed = true;
		memset(frame->buf, 0, frame_bytes);
	}
	if (rec_tx)
		fwrite(frame->buf, 1, frame_bytes, rec_tx);
	frame->size = frame_bytes;
	frame->timestamp.u64 = cp->timestamp.u64;
	frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
	cp->timestamp.u64 += PJMEDIA_PIA_SPF(&p->info);
	return PJ_SUCCESS;
}

/* the port's last user is done (the bridge, and port_end()): its socket
   closed, its pool freed */
pj_status_t port_on_destroy(pjmedia_port *p)
{
	CallPort *cp = reinterpret_cast<CallPort *>(p);
	pj_pool_t *pool = cp->pool;
	PJ_LOG(4, (__FILE__, "port %p freed", (void *)cp));
	if (cp->sock >= 0)
		close(cp->sock);
	cp->~CallPort();
	pj_pool_release(pool);
	return PJ_SUCCESS;
}

/* a port for a call's audio socket fd (taken over); NULL (fd closed) if
   none can be made */
CallPort *port_new(int fd)
{
	static const pj_str_t name = { const_cast<char *>("dmodem"), 6 };
	pj_pool_t *pool = pj_pool_create(&cpool.factory, "dmodem%p", 1024,
					 1024, NULL);
	if (!pool) {
		close(fd);
		return NULL;
	}
	CallPort *cp = new (pj_pool_zalloc(pool, sizeof(CallPort))) CallPort;
	cp->pool = pool;
	cp->sock = fd;
	pjmedia_port_info_init(&cp->base.info, &name,
			       PJMEDIA_SIG_CLASS_PORT_AUD('D', 'M'), rate, 1,
			       16, frame_samples);
	cp->base.put_frame = put_frame;
	cp->base.get_frame = get_frame;
	cp->base.on_destroy = port_on_destroy;
	if (pjmedia_port_init_grp_lock(&cp->base, pool, NULL) != PJ_SUCCESS) {
		port_on_destroy(&cp->base);
		return NULL;
	}
	PJ_LOG(4, (__FILE__, "port %p for socket %d", (void *)cp, fd));
	return cp;
}

/* the call's port gone (service thread): its socket shut down now (the
   modem sees the end, a read the media clock waits in returns), out of
   the bridge, our reference dropped; on_destroy closes and frees it when
   the bridge is done with it */
void port_end()
{
	if (!cur_port)
		return;
	shutdown(cur_port->sock, SHUT_RDWR);
	if (conf_id != PJSUA_INVALID_ID) {
		pjsua_conf_remove_port(conf_id);
		conf_id = PJSUA_INVALID_ID;
	}
	pjmedia_port_destroy(&cur_port->base);
	cur_port = NULL;
}

/* a line (RING, CANCEL) to the modem's control socket */
void ctl_write(const char *msg)
{
	if (write(ctl[0], msg, strlen(msg)) < 0)
		PJ_LOG(1, (__FILE__, "control socket: %s", strerror(errno)));
}

/* the service thread out of its poll() */
void wake_service()
{
	char c = 0;
	if (write(wake[1], &c, 1) < 0)
		PJ_LOG(1, (__FILE__, "wake up: %s", strerror(errno)));
}

/* call_id is the current call: cur_call, or the one being dialled (its
   id not known until pjsua_call_make_call() returns) */
bool is_current(pjsua_call_id call_id)
{
	pjsua_call_id cur = cur_call;
	return call_id == cur ||
		(cur == DIALING && dial_tag &&
		 (uintptr_t)pjsua_call_get_user_data(call_id) == dial_tag);
}

/* pjsua: a call's state changed (SIP thread); the current call's end
   goes to the service thread */
void on_call_state(pjsua_call_id call_id, pjsip_event *)
{
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);
	PJ_LOG(3, (__FILE__, "Call %d state=%.*s", call_id,
		   (int)ci.state_text.slen, ci.state_text.ptr));
	/* the service thread cleans up: the port leaves the bridge before
	   its socket closes */
	if (ci.state == PJSIP_INV_STATE_DISCONNECTED && is_current(call_id)) {
		if (ringing)
			cancel_req = true;
		ringing = false;
		hangup_req = true;
		wake_service();
	}
}

/* pjsua: the current call's media is up: its law (call_law), and the
   service thread to put the modem's port in the bridge */
void on_call_media_state(pjsua_call_id call_id)
{
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);
	if (ci.media_status != PJSUA_CALL_MEDIA_ACTIVE || !is_current(call_id))
		return;
	{
		pjsua_stream_info si;
		if (pjsua_call_get_stream_info(call_id, ci.media[0].index, &si) ==
		    PJ_SUCCESS && si.type == PJMEDIA_TYPE_AUDIO) {
			const pj_str_t &e = si.info.aud.fmt.encoding_name;
			call_law = !pj_stricmp2(&e, "PCMA") ? 2 :
				!pj_stricmp2(&e, "PCMU") ? 1 : 0;
			PJ_LOG(3, (__FILE__, "Call %d codec %.*s", call_id,
				   (int)e.slen, e.ptr));
		}
	}
	media_ready = true;
	wake_service();
}

/* the current call's port into the bridge both ways (service thread;
   once a call, when its media is up) */
void port_connect()
{
	pjsua_call_id id = cur_call;
	if (!cur_port || conf_id != PJSUA_INVALID_ID ||
	    id == PJSUA_INVALID_ID || id == DIALING)
		return;
	pjsua_conf_port_id slot = pjsua_call_get_conf_port(id);
	if (slot == PJSUA_INVALID_ID ||
	    pjsua_conf_add_port(cur_port->pool, &cur_port->base, &conf_id) !=
	    PJ_SUCCESS)
		return;
	pjsua_conf_connect(slot, conf_id);
	pjsua_conf_connect(conf_id, slot);
}

/* a call comes in: ring the modem, or busy */
void on_incoming_call(pjsua_acc_id, pjsua_call_id call_id, pjsip_rx_data *)
{
	if (!cfg.listen) {
		pjsua_call_answer(call_id, 480, NULL, NULL);
		return;
	}
	pjsua_call_id none = PJSUA_INVALID_ID;
	if (!cur_call.compare_exchange_strong(none, call_id)) {
		pjsua_call_answer(call_id, 486, NULL, NULL);
		return;
	}
	ringing = true;
	pjsua_call_answer(call_id, 180, NULL, NULL);
	wake_service();
}

/* a new call's audio socket: its port (cur_port), and a frame of
   silence first, for the delay the modem expects (it answers each frame
   with one of its own).  (hangup_req is left as it is: the call may
   have ended already.) */
void audio_attach(int fd)
{
	char buf[frame_bytes];
	call_law = 0;
	cur_port = port_new(fd);
	if (!cur_port) {
		hangup_req = true;
		return;
	}
	memset(buf, 0, sizeof(buf));
	if (write(fd, buf, sizeof(buf)) < 0)
		cur_port->failed = true;
}

/* the end of a call, from either side (service thread) */
void call_cleanup()
{
	pjsua_call_id id = cur_call;
	if (id != PJSUA_INVALID_ID && id != DIALING)
		pjsua_call_hangup(id, 0, NULL, NULL);
	port_end();
	cur_call = PJSUA_INVALID_ID;
	ringing = false;
	hangup_req = false;
	media_ready = false;
}

/* a request of the modem's, in the service thread: ANSWER the call that
   rings, or DIAL sip:<number>@<domain>; if it can't be, the socket is
   closed (NO CARRIER) */
void do_request(Request &r)
{
	if (r.kind == Request::ANSWER) {
		if (!ringing || r.fd < 0) {
			if (r.fd >= 0)
				close(r.fd);    /* no call: NO CARRIER */
			return;
		}
		ringing = false;
		audio_attach(r.fd);
		pjsua_call_answer(cur_call, 200, NULL, NULL);
		return;
	}
	/* the line busy while the call is made: no incoming call takes it,
	   and the call's end is seen before its id is known */
	pjsua_call_id none = PJSUA_INVALID_ID;
	if (r.fd < 0 || !cur_call.compare_exchange_strong(none, DIALING)) {
		if (r.fd >= 0)
			close(r.fd);            /* busy */
		return;
	}
	static uintptr_t dials;
	dial_tag = ++dials;
	audio_attach(r.fd);
	std::string uri = "sip:" + dial_user(r.number.c_str()) + "@" + cfg.domain;
	PJ_LOG(3, (__FILE__, "calling %s", uri.c_str()));
	pj_str_t u = pj_str(&uri[0]);
	pjsua_call_id id;
	if (pjsua_call_make_call(acc_id, &u, 0, (void *)dial_tag.load(), NULL,
				 &id) == PJ_SUCCESS)
		cur_call = id;
	else
		call_cleanup();
	dial_tag = 0;
}

/* the service thread until stop(): the requests, the call's port into
   the bridge, RING every 3 s while a call waits, CANCEL, and the clean
   up when the call or the socket ends (polled every 200 ms) */
void service_loop()
{
	pj_register_thread();
	pj_time_val next_ring = {0, 0};
	while (running) {
		struct pollfd p = {wake[0], POLLIN, 0};
		if (poll(&p, 1, 200) > 0) {
			char buf[64];
			if (read(wake[0], buf, sizeof(buf)) < 0)
				PJ_LOG(1, (__FILE__, "wake: %s", strerror(errno)));
		}
		for (;;) {
			Request r;
			{
				std::lock_guard<std::mutex> l(req_lock);
				if (requests.empty())
					break;
				r = requests.front();
				requests.pop_front();
			}
			do_request(r);
		}
		if (media_ready.exchange(false))
			port_connect();
		if (cancel_req.exchange(false))
			ctl_write("CANCEL\n");
		pj_time_val now;
		pj_gettimeofday(&now);
		if (ringing && PJ_TIME_VAL_GTE(now, next_ring)) {
			ctl_write("RING\n");
			next_ring = now;
			next_ring.sec += 3;
		}
		if (!ringing)
			next_ring.sec = 0;
		if ((hangup_req || (cur_port && cur_port->failed)) && !ringing)
			call_cleanup();
	}
	call_cleanup();
}

/* a request for the service thread */
void post(Request r)
{
	{
		std::lock_guard<std::mutex> l(req_lock);
		requests.push_back(std::move(r));
	}
	wake_service();
}

/* true if status is PJ_SUCCESS; else the error logged (what: the call
   that failed) */
bool check(pj_status_t status, const char *what)
{
	if (status == PJ_SUCCESS)
		return true;
	pjsua_perror(__FILE__, what, status);
	return false;
}

/* G.711 only, mu-law first */
void set_codecs()
{
	pjsua_codec_info codecs[32];
	unsigned count = PJ_ARRAY_SIZE(codecs);
	pjsua_enum_codecs(codecs, &count);
	for (unsigned i = 0; i < count; i++) {
		int pri = 0;
		if (!pj_strcmp2(&codecs[i].codec_id, "PCMU/8000/1"))
			pri = 2;
		else if (!pj_strcmp2(&codecs[i].codec_id, "PCMA/8000/1"))
			pri = 1;
		pjsua_codec_set_priority(&codecs[i].codec_id, pri);
	}
}

} // namespace

/* see sip.h: the strings point into a static copy of SIP_LOGIN */
bool config_from_env(Config &c, bool listen)
{
	/* SIP_LOGIN=user:password@domain */
	static std::string login;
	const char *e = getenv("SIP_LOGIN");
	if (!e) {
		fprintf(stderr, "SIP_LOGIN=user:password@domain is not set\n");
		return false;
	}
	login = e;
	/* (the password may have an '@': the domain is after the last) */
	size_t at = login.rfind('@'), colon = login.find(':');
	if (at == std::string::npos || colon == std::string::npos ||
	    colon > at) {
		fprintf(stderr, "SIP_LOGIN: user:password@domain\n");
		return false;
	}
	login[at] = 0;
	login[colon] = 0;
	c.user = login.c_str();
	c.password = login.c_str() + colon + 1;
	c.domain = login.c_str() + at + 1;
	c.port = getenv("SIP_PORT") ? atoi(getenv("SIP_PORT")) : 5060;
	c.listen = listen;
	c.record = getenv("DMODEM_REC");
	return true;
}

/* see sip.h: pjsua with G.711 only, a fixed jitter buffer and 20 ms
   frames at 8000 Hz, the modem's port, the account, then the service
   thread; -1 on failure */
int start(const Config &config)
{
	cfg = config;
	if (cfg.record) {
		std::string n = cfg.record;
		rec_rx = fopen((n + ".rx").c_str(), "wb");
		rec_tx = fopen((n + ".tx").c_str(), "wb");
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) || pipe(wake)) {
		perror("sip: socketpair");
		return -1;
	}

	if (!check(pjsua_create(), "pjsua_create()"))
		return -1;
	{
		pjsua_config c;
		pjsua_logging_config log_cfg;
		pjsua_media_config med_cfg;

		pjsua_config_default(&c);
		c.cb.on_call_media_state = &on_call_media_state;
		c.cb.on_call_state = &on_call_state;
		c.cb.on_incoming_call = &on_incoming_call;

		pjsua_logging_config_default(&log_cfg);
		log_cfg.console_level = getenv("SIP_LOG_LEVEL") ?
			atoi(getenv("SIP_LOG_LEVEL")) : 4;

		pjsua_media_config_default(&med_cfg);
		med_cfg.no_vad = true;
		med_cfg.ec_tail_len = 0;
		/* a modem can't take samples slipping: a fixed 60 ms in the
		   jitter buffer, never adapted down by discarding audio.  The
		   far end often sends in step with what it receives, so its
		   packets come right when the bridge takes a frame, and a
		   buffer of one frame would run dry and fill in (a slip of a
		   frame) on any jitter */
		med_cfg.jb_max = 2000;
		med_cfg.jb_init = 60;
		med_cfg.jb_min_pre = 60;
		med_cfg.jb_max_pre = 60;
		med_cfg.jb_discard_algo = PJMEDIA_JB_DISCARD_NONE;
		/* the bridge in frames of the modem (20 ms) at the rate of
		   the call: nothing resampled, so that V.90's PCM codes pass
		   unchanged */
		med_cfg.audio_frame_ptime = 20;
		med_cfg.clock_rate = rate;

		if (!check(pjsua_init(&c, &log_cfg, &med_cfg), "pjsua_init()"))
			return -1;
	}
	pjsua_set_ec(0, 0);
	pjsua_set_null_snd_dev();
	set_codecs();
	{
		pjsua_transport_config c;
		pjsua_transport_config_default(&c);
		c.port = cfg.port;
		if (!check(pjsua_transport_create(PJSIP_TRANSPORT_UDP, &c, NULL),
			   "creating the transport"))
			return -1;
	}

	pj_caching_pool_init(&cpool, NULL, 1024 * 1024);
	pool = pj_pool_create(&cpool.factory, "sip", 4000, 4000, NULL);

	if (!check(pjsua_start(), "pjsua_start()"))
		return -1;
	{
		pjsua_acc_config c;
		std::string id = std::string("sip:") + cfg.user + "@" + cfg.domain;
		std::string reg = std::string("sip:") + cfg.domain;
		pjsua_acc_config_default(&c);
		pj_strdup2(pool, &c.id, id.c_str());
		pj_strdup2(pool, &c.reg_uri, reg.c_str());
		/* registered to be reached when listening */
		c.register_on_acc_add = cfg.listen;
		c.cred_count = 1;
		c.cred_info[0].realm = pj_str(const_cast<char *>("*"));
		c.cred_info[0].scheme = pj_str(const_cast<char *>("digest"));
		pj_strdup2(pool, &c.cred_info[0].username, cfg.user);
		c.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
		pj_strdup2(pool, &c.cred_info[0].data, cfg.password);
		if (!check(pjsua_acc_add(&c, PJ_TRUE, &acc_id), "adding the account"))
			return -1;
	}

	running = true;
	service = std::thread(service_loop);
	return ctl[1];
}

/* see sip.h; the recordings closed too */
void stop()
{
	if (running.exchange(false)) {
		wake_service();
		service.join();
	}
	pjsua_destroy();
	if (rec_rx)
		fclose(rec_rx);
	if (rec_tx)
		fclose(rec_tx);
	rec_rx = rec_tx = NULL;
}

/* a DIAL request (number NULL: an empty one) */
void dial(const char *number, int audio_fd)
{
	post(Request{Request::DIAL, number ? number : "", audio_fd});
}

/* an ANSWER request */
void answer(int audio_fd)
{
	post(Request{Request::ANSWER, "", audio_fd});
}

/* see sip.h */
int law()
{
	return call_law;
}

} // namespace sip
