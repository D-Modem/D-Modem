/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem_ari.cpp - dmodem-ari: D-Modem's modem for Asterisk calls, as
 *                     an ARI application (a program of its own, not a
 *                     module in Asterisk)
 *
 *    The dialplan hands a call over with Stasis(<app>,<profile>[,o]).
 *    For each call: the channel answered, a UDP port of ours, an
 *    external media channel that sends the call's audio there as RTP in
 *    the call's G.711 law (nothing transcoded), the two in a bridge; each
 *    packet that comes through libdmodem's modem, its answer sent back
 *    (the line's clock is the call's).  The profile (dmodem.conf, as
 *    app_dmodem has it) says where the data goes once the modems are
 *    connected: pbx/dmodem_bridge.c, as in the PBX modules.  At the end
 *    DMODEMSTATUS and DMODEMRATE are set and the call goes back to the
 *    dialplan, after the Stasis().
 *
 *    With t38 = yes the line is a SIP leg instead (sip.cpp): Asterisk's
 *    PJSIP channel to our own SIP side, the audio in RTP as before, and
 *    T.38 (UDPTL) when the leg goes over to it by a re-INVITE; the fax
 *    machine is then a T.38 terminal, its time by the clock.
 *
 *    Faxes as app_dmodem has them: a fax machine that calls a modem
 *    profile (fax on) is received into fax_dir; Stasis(<app>,sendfax,
 *    <file>[,<ident>]) sends a TIFF file (DModemSendFax), and
 *    Stasis(<app>,receivefax,<file>[,<ident>]) answers as a fax machine
 *    (DModemReceiveFax); DMODEMFAX* set as they do, but as soon as the
 *    result is final (the MCF to the last page): the channel is gone
 *    with the other machine's hangup, which may come right after it.
 *
 *    One thread reads ARI's events (the WebSocket); each call has a
 *    thread of its own.
 *
 *    dmodem-ari [-c dmodem.conf] [-u http://host:port] [-U user:password]
 *               [-a app] [-v]...
 *
 */
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client.h"
#include "dmodem.h"
#include "dmodem_bridge.h"
#include "json.h"
#include "sip.h"

namespace {

const int IDLE_MS = 200;        /* no packet this long: go on alone */
const int MAX_SAMPLES = 1600;   /* in a packet, 200 ms */

/*
 *    configuration and logging
 */

/* dmodem.conf: its sections' name = value lines, in order */
using Section = std::vector<std::pair<std::string, std::string>>;
std::map<std::string, Section> conf;

ari::Server server;
std::string app = "dmodem";
int verbose;                    /* -v: 1 the calls, 2 the bridge's details */
/* T.38: the calls' lines are legs to our SIP side, on Asterisk's PJSIP
   endpoint sip_endpoint, instead of external media */
bool use_sip;
std::string sip_endpoint = "dmodem";
ari::SipAgent sip;

std::mutex log_lock;

/* a line to stderr, the time first */
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* (see the declaration above) */
void say(const char *fmt, ...)
{
	char t[32];
	time_t now = time(nullptr);
	va_list ap;

	strftime(t, sizeof(t), "%H:%M:%S", localtime(&now));
	std::lock_guard<std::mutex> lk(log_lock);
	fprintf(stderr, "%s ", t);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* s without the blanks around it */
std::string trim(const std::string &s)
{
	size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
	return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

/* path into conf, as Asterisk reads its files: [section], name = value
   (or =>), ';' to the end of the line a comment and "\;" a ';'; a
   section seen again goes on; false if the file can't be read */
bool read_conf(const char *path)
{
	std::ifstream in(path);
	std::string line, section = "general";

	if (!in)
		return false;
	while (std::getline(in, line)) {
		std::string l;
		for (size_t i = 0; i < line.size(); i++) {
			if (line[i] == '\\' && i + 1 < line.size() &&
			    line[i + 1] == ';') {
				l += ';';
				i++;
			} else if (line[i] == ';') {
				break;
			} else {
				l += line[i];
			}
		}
		l = trim(l);
		if (l.empty())
			continue;
		if (l[0] == '[') {
			size_t e = l.find(']');
			section = trim(l.substr(1, e == std::string::npos ?
						e : e - 1));
			conf[section];
			continue;
		}
		size_t eq = l.find('=');
		if (eq == std::string::npos)
			continue;
		std::string value = l.substr(eq + 1);
		if (!value.empty() && value[0] == '>')
			value.erase(0, 1);
		conf[section].emplace_back(trim(l.substr(0, eq)), trim(value));
	}
	return true;
}

/* the value of name in [general], def if none */
std::string general(const char *name, const std::string &def = "")
{
	for (const auto &v : conf["general"])
		if (dmbridge_option_is(v.first.c_str(), name))
			return v.second;
	return def;
}

/* the bridge's lines: a call's under its channel's name */
void bridge_log(void *call, enum dmbridge_level level, const char *line)
{
	static const char *const tag[] = { "ERROR", "WARNING", "", "" };
	if (level == DMBRIDGE_DEBUG && verbose < 2)
		return;
	if (level == DMBRIDGE_INFO && verbose < 1)
		return;
	say("%s%s%s: %s", tag[level], *tag[level] ? " " : "",
	    call ? static_cast<const char *>(call) : "dmodem", line);
}

/* yes, true, on, 1 */
int bridge_true(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
		!strcasecmp(v, "on") || !strcmp(v, "1");
}

/* libdmodem's log lines */
void modem_log(const char *line)
{
	std::string l = line;
	while (!l.empty() && l.back() == '\n')
		l.pop_back();
	say("dmodem: %s", l.c_str());
}

const struct dmbridge_host host = {
	bridge_log, bridge_true, nullptr, nullptr, nullptr,
};

/* [name] into p, the fax settings of [general] first; false if there is
   no such profile or it is incomplete */
bool load_profile(const std::string &name, struct dmbridge_profile *p)
{
	std::string where = "[" + name + "]";
	auto s = conf.find(name);

	dmbridge_profile_init(p);
	if (s == conf.end() || name == "general") {
		say("ERROR dmodem.conf: no profile %s", where.c_str());
		return false;
	}
	dmbridge_fax_defaults(&p->fax, "/var/spool/asterisk/fax");
	for (const auto &v : conf["general"])
		dmbridge_fax_option(&host, &p->fax, v.first.c_str(),
				    v.second.c_str());
	for (const auto &v : s->second)
		dmbridge_profile_option(&host, p, where.c_str(),
					v.first.c_str(), v.second.c_str());
	return !dmbridge_profile_check(&host, p, where.c_str());
}

/*
 *    ARI's requests
 */

/* method /ari/<path>; the reply's JSON into *reply (if given); false
   (logged) unless it is a 2xx */
bool ari_call(const char *method, const std::string &path,
	      ari::Json *reply = nullptr)
{
	std::string body;
	int status = ari::request(server, method, "/ari/" + path, &body);
	/* (a DELETE of what has gone already is no news) */
	if (status == 404 && !strcmp(method, "DELETE"))
		return true;
	if (status < 200 || status > 299) {
		if (verbose >= 1 || status < 0)
			say("ari: %s /ari/%s: %d %s", method, path.c_str(),
			    status, body.c_str());
		return false;
	}
	if (reply && (body.empty() || !ari::Json::parse(body, *reply)))
		*reply = ari::Json();
	return true;
}

/* a channel variable of channel id ("" if none) */
std::string get_var(const std::string &id, const std::string &name)
{
	ari::Json r;
	if (!ari_call("GET", "channels/" + id + "/variable?variable=" +
		      ari::url_encode(name), &r))
		return std::string();
	return r["value"].str();
}

/* a channel variable of channel id set to value */
void set_var(const std::string &id, const std::string &name,
	     const std::string &value)
{
	ari_call("POST", "channels/" + id + "/variable?variable=" +
		 ari::url_encode(name) + "&value=" + ari::url_encode(value));
}

/*
 *    the call's line: RTP to and from the external media channel
 */

struct Rtp {
	int fd = -1;
	struct sockaddr_in peer = {};   /* Asterisk's end */
	uint8_t pt = 0;                 /* 0 PCMU, 8 PCMA */
	uint16_t seq = 0;
	uint32_t ts = 0, ssrc = 0;
	bool first = true;

	/* the socket closed */
	~Rtp() { if (fd >= 0) close(fd); }

	/* a UDP socket on host, any port: its port, -1 if none */
	int open(const std::string &host)
	{
		struct sockaddr_in a = {};
		socklen_t len = sizeof(a);
		fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		a.sin_family = AF_INET;
		if (fd < 0 ||
		    inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
		    bind(fd, reinterpret_cast<struct sockaddr *>(&a),
			 sizeof(a)) ||
		    getsockname(fd, reinterpret_cast<struct sockaddr *>(&a),
				&len))
			return -1;
		ssrc = static_cast<uint32_t>(random());
		seq = static_cast<uint16_t>(random());
		return ntohs(a.sin_port);
	}
	/* where Asterisk takes the packets; false if not an address */
	bool set_peer(const std::string &addr, const std::string &port)
	{
		peer.sin_family = AF_INET;
		peer.sin_port =
			htons(static_cast<uint16_t>(atoi(port.c_str())));
		return inet_pton(AF_INET, addr.c_str(), &peer.sin_addr) == 1 &&
			peer.sin_port;
	}
	/* a packet's payload into buf (its octets), within timeout_ms: >0
	   the samples, 0 none came, -1 an error */
	int recv(uint8_t *buf, int max, int timeout_ms)
	{
		struct pollfd p = { fd, POLLIN, 0 };
		uint8_t pkt[12 + 60 + MAX_SAMPLES];
		int r = poll(&p, 1, timeout_ms);
		if (r <= 0)
			return r < 0 && errno != EINTR ? -1 : 0;
		ssize_t n = ::recv(fd, pkt, sizeof(pkt), 0);
		if (n < 12 || (pkt[0] >> 6) != 2)
			return 0;
		/* past the CSRCs and an extension; the padding off */
		size_t off = 12 + 4 * (pkt[0] & 0x0f);
		if ((pkt[0] & 0x10) && static_cast<size_t>(n) >= off + 4)
			off += 4 + 4 * ((pkt[off + 2] << 8) | pkt[off + 3]);
		if (pkt[0] & 0x20)
			n -= pkt[n - 1];
		if (static_cast<size_t>(n) <= off || (pkt[1] & 0x7f) != pt)
			return 0;       /* (not the call's law: DTMF, CN) */
		n -= off;
		if (n > max)
			n = max;
		memcpy(buf, pkt + off, n);
		return static_cast<int>(n);
	}
	/* n samples (octets) to Asterisk */
	void send(const uint8_t *buf, int n)
	{
		uint8_t pkt[12 + MAX_SAMPLES];
		pkt[0] = 0x80;
		pkt[1] = pt | (first ? 0x80 : 0);
		pkt[2] = seq >> 8;
		pkt[3] = seq & 0xff;
		pkt[4] = ts >> 24;
		pkt[5] = ts >> 16;
		pkt[6] = ts >> 8;
		pkt[7] = ts;
		pkt[8] = ssrc >> 24;
		pkt[9] = ssrc >> 16;
		pkt[10] = ssrc >> 8;
		pkt[11] = ssrc;
		memcpy(pkt + 12, buf, n);
		sendto(fd, pkt, 12 + n, 0,
		       reinterpret_cast<struct sockaddr *>(&peer),
		       sizeof(peer));
		first = false;
		seq++;
		ts += n;
	}
};

/*
 *    calls
 */

/* what a call is for: the first of Stasis()'s arguments */
enum Kind { MODEM, SENDFAX, RECEIVEFAX };

struct Call {
	std::string id, name;           /* the channel (id: its uniqueid) */
	Kind kind = MODEM;
	std::string profile, options;   /* MODEM: Stasis(app,profile,options) */
	std::string file, ident;        /* the faxes: Stasis(app,sendfax,file,
					   ident) */
	std::string state, caller, exten;
	std::atomic<bool> hungup{false};
};

std::mutex calls_lock;
std::map<std::string, std::shared_ptr<Call>> calls;
/* the external media channels of ours (their StasisStart ignored) */
std::map<std::string, bool> media_channels;

/* the call's law: law if it is known, else the channel's (A-law if it has
   alaw and not ulaw) */
enum dmodem_law call_law(const Call &c, enum dmodem_law law)
{
	if (law != DMODEM_LAW_UNKNOWN)
		return law;
	std::string f = get_var(c.id, "CHANNEL(audionativeformat)");
	return f.find("alaw") != std::string::npos &&
		f.find("ulaw") == std::string::npos ?
		DMODEM_LAW_ALAW : DMODEM_LAW_ULAW;
}

/* the line's next block into in: a packet's samples, or silence once
   IDLE_MS went by without one; 0 if the call has gone, -1 on an error */
int next_block(Call &c, Rtp &rtp, enum dmodem_law law, uint8_t *in)
{
	if (c.hungup)
		return 0;
	int n = rtp.recv(in, MAX_SAMPLES, IDLE_MS);
	if (n == 0) {
		n = 160;
		memset(in, law == DMODEM_LAW_ALAW ? 0xd5 : 0xff, n);
	}
	return n;
}

/* the fax settings of [general]: the built-in ones, then its own */
void general_fax(struct dmbridge_fax *f)
{
	dmbridge_fax_defaults(f, "/var/spool/asterisk/fax");
	for (const auto &v : conf["general"])
		dmbridge_fax_option(&host, f, v.first.c_str(), v.second.c_str());
}

/* dir and the directories above it made (as far as they can be) */
void make_dirs(const std::string &dir)
{
	for (size_t i = 1; i <= dir.size(); i++)
		if (i == dir.size() || dir[i] == '/')
			mkdir(dir.substr(0, i).c_str(), 0755);
}

/* where a fax a modem profile takes goes:
   <dir>/<date>-<time>-<caller>-<uniqueid>.tif, as app_dmodem names it */
std::string fax_file_name(const Call &c, const char *dir)
{
	char when[32];
	time_t now = time(nullptr);
	std::string caller, id = c.id;

	strftime(when, sizeof(when), "%Y%m%d-%H%M%S", localtime(&now));
	for (char ch : c.caller)
		if ((isalnum(static_cast<unsigned char>(ch)) || ch == '+') &&
		    caller.size() < 31)
			caller += ch;
	for (char &ch : id)
		if (!isalnum(static_cast<unsigned char>(ch)) && ch != '.')
			ch = '_';
	return std::string(dir) + "/" + when + "-" +
		(caller.empty() ? "unknown" : caller) + "-" + id + ".tif";
}

/* the fax's result in the call's variables, as DModemSendFax and
   DModemReceiveFax set them (t38: it went over T.38) */
void fax_variables(const Call &c, const struct dmodem_fax_status &st,
		   bool t38)
{
	set_var(c.id, "DMODEMFAXSTATUS", st.ok ? "SUCCESS" : "FAILED");
	set_var(c.id, "DMODEMFAXRESULT", st.result);
	set_var(c.id, "DMODEMFAXPAGES", std::to_string(st.pages));
	set_var(c.id, "DMODEMFAXRATE", std::to_string(st.bps));
	set_var(c.id, "DMODEMFAXREMOTE", st.remote_ident);
	set_var(c.id, "DMODEMFAXECM", st.ecm ? "yes" : "no");
	set_var(c.id, "DMODEMFAXT38", t38 ? "yes" : "no");
}

/* the call's line: our RTP, and what Asterisk has at its end (an
   external media channel, or our SIP leg), in a bridge with the call */
struct Line {
	Rtp rtp;
	std::string media, bridge;      /* their ids, for the end */
	std::shared_ptr<ari::SipLeg> leg;       /* (T.38) */
	int udptl = -1;                 /* our UDPTL socket */

	/* our UDPTL socket closed */
	~Line() { if (udptl >= 0) close(udptl); }
};

/* where our T.38 packets go */
struct T38Peer {
	int fd;
	struct sockaddr_in to;
};

/* the fax engine's T.38 packet of len octets to ctx's peer (a T38Peer) */
void t38_send(void *ctx, const unsigned char *pkt, int len)
{
	const T38Peer *p = static_cast<const T38Peer *>(ctx);
	sendto(p->fd, pkt, len, 0,
	       reinterpret_cast<const struct sockaddr *>(&p->to),
	       sizeof(p->to));
}

/* microseconds, monotonic */
long long now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<long long>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

/* the fax machine on the call's line (dir: send file, or receive into
   it) until the fax call ends; DMODEMFAXFILE first, and the result's
   variables as soon as it is final (complete: the MCF to the last page,
   the received pages in the file).  From then on the other machine may
   hang up, and the dialplan's h extension runs at once: an ARI app
   cannot hold the channel as DModemReceiveFax does.

   On a SIP leg (fax_t38 on) the fax goes over to T.38 when the leg
   does, before a page has gone (the fax machine anew on UDPTL, its time
   by the clock): a receiver asks for it, as ReceiveFAX does, and waits
   for the answer (6 s at most, silence on the line) before it starts on
   the audio; a sender takes it when asked */
void run_fax(Call &c, Line &line, enum dmodem_law law,
	     enum dmodem_fax_dir dir, const std::string &file,
	     const struct dmbridge_fax &fs)
{
	struct dmodem_fax_config cfg = {};
	struct dmodem_fax_status st = {};
	struct dmodem_fax *f;
	uint8_t in[MAX_SAMPLES], out[MAX_SAMPLES];
	bool sending = dir == DMODEM_FAX_SEND, told = false, t38 = false;
	bool may_switch = line.leg != nullptr && fs.t38, asking = false;
	T38Peer peer = { line.udptl, {} };
	long long t0 = 0, samples = 0, asked = 0;

	cfg.law = law;
	cfg.dir = dir;
	cfg.file = file.c_str();
	cfg.ident = fs.ident;
	cfg.max_bps = fs.max_bps;
	cfg.no_ecm = !fs.ecm;
	set_var(c.id, "DMODEMFAXFILE", file);
	f = dmodem_fax_create(&cfg);
	if (!f) {
		say("WARNING %s: cannot %s fax file %s", c.name.c_str(),
		    sending ? "read" : "use", file.c_str());
		set_var(c.id, "DMODEMFAXSTATUS", "FAILED");
		set_var(c.id, "DMODEMFAXRESULT", "no document");
		return;
	}
	if (verbose >= 1)
		say("%s: dmodem fax: %s %s", c.name.c_str(),
		    sending ? "sending" : "receiving", file.c_str());
	if (may_switch) {
		sip.accept_t38(line.leg, true);
		if (!sending && sip.ask_t38(line.leg)) {
			asked = now_us();
			if (verbose >= 1)
				say("%s: dmodem fax: T.38 asked for",
				    c.name.c_str());
		}
	}
	for (;;) {
		if (line.leg) {
			ari::SipLeg l = sip.get(line.leg);
			if (l.gone) {
				dmodem_fax_hangup(f);
				break;
			}
			if (may_switch && l.t38 == ari::T38_ON) {
				/* over to T.38: the fax machine anew */
				dmodem_fax_destroy(f);
				cfg.t38 = 1;
				cfg.t38_version = l.t38_peer.version;
				cfg.t38_ec = l.t38_peer.redundancy ?
					DMODEM_T38_EC_REDUNDANCY :
					DMODEM_T38_EC_NONE;
				cfg.t38_max_datagram = l.t38_peer.max_datagram;
				cfg.t38_send = t38_send;
				cfg.t38_ctx = &peer;
				peer.to.sin_family = AF_INET;
				peer.to.sin_port = htons(
					static_cast<uint16_t>(l.t38_peer.port));
				inet_pton(AF_INET, l.t38_peer.addr.c_str(),
					  &peer.to.sin_addr);
				f = dmodem_fax_create(&cfg);
				if (!f) {
					say("WARNING %s: cannot %s fax file %s",
					    c.name.c_str(),
					    sending ? "read" : "use", file.c_str());
					set_var(c.id, "DMODEMFAXSTATUS", "FAILED");
					return;
				}
				if (verbose >= 1)
					say("%s: dmodem fax: T.38 (version %d, "
					    "%s:%d%s)", c.name.c_str(),
					    l.t38_peer.version,
					    l.t38_peer.addr.c_str(), l.t38_peer.port,
					    l.t38_peer.redundancy ?
					    ", redundancy" : "");
				t38 = true;
				may_switch = false;
				t0 = now_us();
				samples = 0;
			} else if (may_switch && (st.pages || st.complete)) {
				/* a page has gone on audio: it stays */
				sip.accept_t38(line.leg, false);
				may_switch = false;
			}
			asking = asked && l.t38 == ari::T38_ASKED &&
				now_us() - asked < 6000000;
		}
		if (t38) {
			unsigned char pkt[2048];
			struct pollfd pf = { line.udptl, POLLIN, 0 };
			ssize_t n;
			long long due;
			if (c.hungup) {
				dmodem_fax_hangup(f);
				break;
			}
			poll(&pf, 1, 20);
			while ((n = recv(line.udptl, pkt, sizeof(pkt),
					 MSG_DONTWAIT)) > 0)
				dmodem_fax_t38_rx(f, pkt, static_cast<int>(n));
			/* the time since, by the clock (a second at most) */
			due = (now_us() - t0) * 8 / 1000;
			if (due > samples) {
				dmodem_fax_t38_time(
					f, due - samples > 8000 ?
						   8000 :
						   static_cast<int>(due -
								    samples));
				samples = due;
			}
		} else {
			int n = next_block(c, line.rtp, law, in);
			if (n <= 0) {
				dmodem_fax_hangup(f);
				break;
			}
			if (asking) {
				/* (T.38 or not: the fax machine waits) */
				memset(out, law == DMODEM_LAW_ALAW ? 0xd5 : 0xff,
				       n);
				line.rtp.send(out, n);
				continue;
			}
			dmodem_fax_process_g711(f, in, out, n);
			line.rtp.send(out, n);
		}
		dmodem_fax_status(f, &st);
		if (st.complete && !told) {
			fax_variables(c, st, t38);
			told = true;
		}
		if (st.done)
			break;
	}
	dmodem_fax_status(f, &st);
	if (!told && !c.hungup)
		fax_variables(c, st, t38);
	/* (the received file closed with it) */
	dmodem_fax_destroy(f);
	if (verbose >= 1)
		say("%s: dmodem fax %s: %s, %d pages at %d bit/s%s%s, remote "
		    "'%s'", c.name.c_str(), sending ? "sent" : "received",
		    st.result, st.pages, st.bps, st.ecm ? " (ECM)" : "",
		    t38 ? " over T.38" : "", st.remote_ident);
}

/* the modem on the call's line until it ends (a fax machine that calls a
   profile with fax on: the fax receiver in its place; also when the
   other end goes over to T.38 before the modems are connected); its
   status */
std::string run_modem(Call &c, const struct dmbridge_profile &p,
		      enum dmodem_law law, Line &line)
{
	struct dmbridge_call bc;
	char *who = const_cast<char *>(c.name.c_str());
	uint8_t in[MAX_SAMPLES], out[MAX_SAMPLES];
	std::string status;
	bool orig = c.options.find('o') != std::string::npos, fax = false;

	dmbridge_call_init(&bc);
	if (dmbridge_call_start(&bc, &host, who, &p, c.name.c_str(), law,
				orig)) {
		dmbridge_call_end(&bc);
		return "ERROR";
	}
	if (verbose >= 1)
		say("%s: dmodem %s (profile %s, %s)", c.name.c_str(),
		    orig ? "calling" : "answering", c.profile.c_str(),
		    law == DMODEM_LAW_ALAW ? "alaw" : "ulaw");
	if (line.leg && p.fax.on && p.fax.t38)
		sip.accept_t38(line.leg, true);
	for (;;) {
		enum dmbridge_event ev;
		int n;

		if (line.leg) {
			ari::SipLeg l = sip.get(line.leg);
			if (l.gone) {
				bc.status = "HANGUP";
				break;
			}
			fax = l.t38 == ari::T38_ON;
		}
		n = fax ? 0 : next_block(c, line.rtp, law, in);
		if (!fax && n <= 0) {
			bc.status = n ? "ERROR" : "HANGUP";
			break;
		}
		ev = DMBRIDGE_RUN;
		if (!fax) {
			/* a packet out for each packet in */
			dmodem_process_g711(bc.d, in, out, n);
			line.rtp.send(out, n);
			ev = dmbridge_call_step(&bc, n);
		}
		if (ev == DMBRIDGE_CONNECTED) {
			char connected[24];
			const char *env[] = {
				"DMODEM_CHANNEL", c.name.c_str(),
				"DMODEM_PROFILE", c.profile.c_str(),
				"DMODEM_RATE", bc.rate,
				"DMODEM_CALLERID", c.caller.c_str(),
				"DMODEM_DIALED", c.exten.c_str(),
				"DMODEM_CONNECTED", connected,
				nullptr
			};
			snprintf(connected, sizeof(connected), "%lld",
				 static_cast<long long>(time(nullptr)));
			set_var(c.id, "DMODEMRATE", bc.rate);
			if (verbose >= 1)
				say("%s: dmodem connected at %s bit/s",
				    c.name.c_str(), bc.rate);
			if (line.leg)
				sip.accept_t38(line.leg, false);
			ev = dmbridge_call_serve(&bc, env);
		}
		if (ev == DMBRIDGE_FAX || fax) {
			/* the modem is gone: the line is the fax receiver's */
			std::string path = fax_file_name(c, p.fax.dir);
			if (verbose >= 1)
				say("%s: dmodem: a fax machine is calling%s",
				    c.name.c_str(), fax ? " (T.38)" : "");
			/* (now: it may have hung up when the fax ends) */
			set_var(c.id, "DMODEMSTATUS", "FAX");
			make_dirs(p.fax.dir);
			run_fax(c, line, law, DMODEM_FAX_RECEIVE, path, p.fax);
			fax = true;
			break;
		}
		if (ev == DMBRIDGE_END)
			break;
	}
	dmbridge_call_hangup(&bc);
	status = fax ? "FAX" : bc.status;
	dmbridge_call_end(&bc);
	return status;
}

/* a UDP socket of ours on host, any port, into *fd: its port, -1 if
   none */
int udp_open(const std::string &host, int *fd)
{
	struct sockaddr_in a = {};
	socklen_t len = sizeof(a);
	*fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	a.sin_family = AF_INET;
	if (*fd < 0 || inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
	    bind(*fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) ||
	    getsockname(*fd, reinterpret_cast<struct sockaddr *>(&a), &len))
		return -1;
	return ntohs(a.sin_port);
}

/* true once channel id has had its StasisStart, within ms */
bool media_started(const std::string &id, int ms)
{
	for (int k = 0; k < ms / 10; k++) {
		{
			std::lock_guard<std::mutex> lk(calls_lock);
			auto it = media_channels.find(id);
			if (it != media_channels.end() && it->second)
				return true;
		}
		usleep(10000);
	}
	return false;
}

/* the SIP leg for the call (T.38): our RTP and UDPTL on the SIP side's
   address, Asterisk's channel to our SIP side for a token of the call's
   (its INVITE in law: what we answer it with goes into law), up and in
   Stasis; a T.38 offer taken from the start if t38 (a fax, or a modem
   that may get one: the other end may ask before our side is ready);
   false (logged) if it isn't */
bool open_leg(Call &c, enum dmodem_law &law, Line &line, bool t38)
{
	std::string token = "dmodem-" + std::to_string(random());
	int rtp_port, udptl_port;

	line.rtp.pt = law == DMODEM_LAW_ALAW ? 8 : 0;
	rtp_port = line.rtp.open(sip.host());
	udptl_port = udp_open(sip.host(), &line.udptl);
	if (rtp_port < 0 || udptl_port < 0) {
		say("ERROR %s: no UDP port: %s", c.name.c_str(),
		    strerror(errno));
		return false;
	}
	line.leg = sip.expect(token, line.rtp.pt, rtp_port, udptl_port);
	sip.accept_t38(line.leg, t38);
	line.media = "leg-" + token;
	{
		std::lock_guard<std::mutex> lk(calls_lock);
		media_channels[line.media] = false;
	}
	if (!ari_call("POST", "channels?endpoint=" +
		      ari::url_encode("PJSIP/" + sip_endpoint + "/sip:" + token +
				      "@" + sip.host() + ":" +
				      std::to_string(sip.port())) +
		      "&app=" + ari::url_encode(app) + "&appArgs=leg"
		      "&channelId=" + ari::url_encode(line.media) +
		      "&formats=" + (law == DMODEM_LAW_ALAW ? "alaw" : "ulaw")))
		return false;
	if (!sip.wait_up(line.leg, 10000) || !media_started(line.media, 5000)) {
		say("ERROR %s: Asterisk's channel to our SIP side (PJSIP/%s) "
		    "did not come", c.name.c_str(), sip_endpoint.c_str());
		return false;
	}
	ari::SipLeg l = sip.get(line.leg);
	line.rtp.pt = static_cast<uint8_t>(l.pt);
	law = l.pt == 8 ? DMODEM_LAW_ALAW : DMODEM_LAW_ULAW;
	if (!line.rtp.set_peer(l.rtp_addr, std::to_string(l.rtp_peer_port))) {
		say("ERROR %s: no RTP address in Asterisk's SDP",
		    c.name.c_str());
		return false;
	}
	return true;
}

/* the call's line: the channel answered (unless it is up), our UDP port,
   the external media channel that sends the call's audio there in law
   (T.38: the SIP leg, taking T.38 offers if t38), and the two in a
   bridge; false (logged) if any of it fails */
bool open_line(Call &c, enum dmodem_law &law, Line &line, bool t38)
{
	ari::Json r;
	int port;

	line.rtp.pt = law == DMODEM_LAW_ALAW ? 8 : 0;
	if (c.state != "Up" &&
	    !ari_call("POST", "channels/" + c.id + "/answer"))
		return false;
	if (use_sip) {
		if (!open_leg(c, law, line, t38))
			return false;
	} else {
		port = line.rtp.open("127.0.0.1");
		if (port < 0) {
			say("ERROR %s: no UDP port: %s", c.name.c_str(),
			    strerror(errno));
			return false;
		}
		/* the call's audio to our port, in its law */
		if (!ari_call("POST", "channels/externalMedia?app=" +
			      ari::url_encode(app) + "&external_host=" +
			      ari::url_encode("127.0.0.1:" +
					      std::to_string(port)) +
			      "&format=" +
			      (law == DMODEM_LAW_ALAW ? "alaw" : "ulaw"), &r))
			return false;
		line.media = r["id"].str();
		{
			std::lock_guard<std::mutex> lk(calls_lock);
			media_channels[line.media] = true;
		}
		if (!line.rtp.set_peer(
			    r["channelvars"]["UNICASTRTP_LOCAL_ADDRESS"].str(),
			    r["channelvars"]["UNICASTRTP_LOCAL_PORT"].str()) &&
		    !line.rtp.set_peer(get_var(line.media,
					       "UNICASTRTP_LOCAL_ADDRESS"),
				       get_var(line.media,
					       "UNICASTRTP_LOCAL_PORT"))) {
			say("ERROR %s: no address of the external media channel",
			    c.name.c_str());
			return false;
		}
	}
	if (!ari_call("POST", "bridges?type=mixing", &r))
		return false;
	line.bridge = r["id"].str();
	return ari_call("POST", "bridges/" + line.bridge + "/addChannel?channel=" +
			ari::url_encode(c.id + "," + line.media));
}

/* a call's thread: what it is for on its line (the modem, or a fax to
   send or receive), the line taken down, and the call back to the
   dialplan (if it is still there) */
void run_call(std::shared_ptr<Call> c)
{
	struct dmbridge_profile p;
	struct dmbridge_fax fs;
	std::string status = "ERROR";
	enum dmodem_law law = DMODEM_LAW_UNKNOWN;
	Line line;
	bool up = false;

	if (c->kind == MODEM) {
		if (load_profile(c->profile, &p)) {
			law = call_law(*c, p.law);
			up = open_line(*c, law, line,
				       p.fax.on && p.fax.t38);
		}
		if (up)
			status = run_modem(*c, p, law, line);
		else if (c->hungup)
			status = "HANGUP";
		if (verbose >= 1)
			say("%s: dmodem done: %s", c->name.c_str(),
			    status.c_str());
	} else {
		/* the fax settings of [general], the ident of Stasis() */
		general_fax(&fs);
		if (!c->ident.empty())
			snprintf(fs.ident, sizeof(fs.ident), "%s",
				 c->ident.c_str());
		if (c->file.empty()) {
			say("WARNING %s: %s needs a file name",
			    c->name.c_str(), c->kind == SENDFAX ?
			    "sendfax" : "receivefax");
			set_var(c->id, "DMODEMFAXSTATUS", "FAILED");
		} else {
			law = call_law(*c, DMODEM_LAW_UNKNOWN);
			up = open_line(*c, law, line, fs.t38);
			if (up)
				run_fax(*c, line, law, c->kind == SENDFAX ?
					DMODEM_FAX_SEND : DMODEM_FAX_RECEIVE,
					c->file, fs);
			else if (!c->hungup)
				set_var(c->id, "DMODEMFAXSTATUS", "FAILED");
		}
	}
	if (!line.media.empty())
		ari_call("DELETE", "channels/" + line.media);
	if (!line.bridge.empty())
		ari_call("DELETE", "bridges/" + line.bridge);
	if (line.leg)
		sip.forget(line.leg);
	if (!c->hungup) {
		if (c->kind == MODEM)
			set_var(c->id, "DMODEMSTATUS", status);
		ari_call("POST", "channels/" + c->id + "/continue");
	}
	std::lock_guard<std::mutex> lk(calls_lock);
	calls.erase(c->id);
	media_channels.erase(line.media);
}

/* one of ARI's events */
void event(const ari::Json &e)
{
	const std::string &type = e["type"].str();
	const ari::Json &ch = e["channel"];
	const std::string &id = ch["id"].str();

	if (type == "StasisStart") {
		std::lock_guard<std::mutex> lk(calls_lock);
		auto m = media_channels.find(id);
		if (m != media_channels.end()) {
			m->second = true;       /* (a SIP leg: up) */
			return;
		}
		if (!ch["name"].str().compare(0, 11, "UnicastRTP/"))
			return;
		auto c = std::make_shared<Call>();
		const std::string &what = e["args"][0].str();
		c->id = id;
		c->name = ch["name"].str();
		c->state = ch["state"].str();
		c->caller = ch["caller"]["number"].str();
		c->exten = ch["dialplan"]["exten"].str();
		if (what == "sendfax" || what == "receivefax") {
			c->kind = what == "sendfax" ? SENDFAX : RECEIVEFAX;
			c->file = e["args"][1].str();
			c->ident = e["args"][2].str();
		} else {
			c->profile = what.empty() ? "default" : what;
			c->options = e["args"][1].str();
		}
		calls[id] = c;
		std::thread(run_call, c).detach();
		return;
	}
	/* the call gone, or out of our hands (a StasisEnd after our own
	   continue finds no call: its thread is done) */
	if (type == "ChannelHangupRequest" || type == "ChannelDestroyed" ||
	    type == "StasisEnd") {
		std::lock_guard<std::mutex> lk(calls_lock);
		auto it = calls.find(id);
		if (it != calls.end())
			it->second->hungup = true;
	}
}

volatile sig_atomic_t stop;

/* SIGINT, SIGTERM: stop set, the main loop ends */
void on_signal(int)
{
	stop = 1;
}

/* the options to stderr */
void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-c dmodem.conf] [-u http://host:port] "
		"[-U user:password] [-a app] [-v]...\n"
		"  ARI's address, user and app also as ari_url, ari_user, "
		"ari_password and\n  ari_app in [general]\n", prog);
}

} // namespace

/* the program: the options and dmodem.conf, then ARI's events until
   SIGINT or SIGTERM; 1 if it cannot start, 2 for bad options */
int main(int argc, char *argv[])
{
	const char *conf_path = "/etc/asterisk/dmodem.conf";
	std::string url, login;
	int opt;

	while ((opt = getopt(argc, argv, "c:u:U:a:vh")) != -1) {
		switch (opt) {
		case 'c': conf_path = optarg; break;
		case 'u': url = optarg; break;
		case 'U': login = optarg; break;
		case 'a': app = optarg; break;
		case 'v': verbose++; break;
		default: usage(argv[0]); return 2;
		}
	}
	if (!read_conf(conf_path)) {
		fprintf(stderr, "dmodem-ari: cannot read %s\n", conf_path);
		return 1;
	}
	if (url.empty())
		url = general("ari_url", "http://127.0.0.1:8088");
	if (!server.set_url(url)) {
		fprintf(stderr, "dmodem-ari: not an http:// URL: %s\n",
			url.c_str());
		return 1;
	}
	if (login.empty())
		login = general("ari_user") + ":" + general("ari_password");
	server.user = login.substr(0, login.find(':'));
	server.password = login.find(':') == std::string::npos ? "" :
		login.substr(login.find(':') + 1);
	if (app == "dmodem")
		app = general("ari_app", app);
	/* T.38: the calls on legs to our SIP side */
	use_sip = bridge_true(general("t38", "no").c_str());
	if (use_sip) {
		std::string bind = general("sip_bind", "127.0.0.1:5090");
		size_t colon = bind.rfind(':');
		sip_endpoint = general("sip_endpoint", sip_endpoint);
		if (!sip.start(bind.substr(0, colon),
			       colon == std::string::npos ? 5090 :
			       atoi(bind.c_str() + colon + 1))) {
			fprintf(stderr, "dmodem-ari: no SIP socket on %s: %s\n",
				bind.c_str(), strerror(errno));
			return 1;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	srandom(static_cast<unsigned>(time(nullptr)) ^
		static_cast<unsigned>(getpid()));
	dmodem_set_log(modem_log);
	dmodem_init();
	dmodem_set_debug(atoi(general("debug", "0").c_str()));

	/* the events, the WebSocket again a second after it goes */
	ari::WebSocket ws;
	bool up = false;
	while (!stop) {
		std::string msg;
		if (!up) {
			up = ws.open(server, "/ari/events?app=" +
				     ari::url_encode(app));
			if (!up) {
				sleep(1);
				continue;
			}
			say("dmodem-ari: app %s on %s:%d", app.c_str(),
			    server.host.c_str(), server.port);
			if (use_sip)
				say("dmodem-ari: T.38: the calls' legs on "
				    "PJSIP/%s to %s:%d", sip_endpoint.c_str(),
				    sip.host().c_str(), sip.port());
		}
		int r = ws.read(msg, 500);
		if (r < 0) {
			say("dmodem-ari: the event stream has gone");
			up = false;
			continue;
		}
		ari::Json e;
		if (r > 0 && ari::Json::parse(msg, e))
			event(e);
	}
	say("dmodem-ari: stopped");
	/* the calls' threads end with the process */
	fflush(stderr);
	_exit(0);
}
