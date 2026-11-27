/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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
 *    connected: frontend/pbx/dmodem_bridge.c, as in the PBX modules.  At
 *    the end DMODEMSTATUS and DMODEMRATE are set and the call goes back
 *    to the dialplan, after the Stasis().
 *
 *    With t38 = yes the line is a SIP leg instead (sip.cpp): Asterisk's
 *    PJSIP channel to our own SIP side, the audio in RTP as before, and
 *    T.38 (UDPTL) when the leg goes over to it by a re-INVITE; the fax
 *    machine is then a T.38 terminal, its time by the clock.
 *
 *    Faxes as app_dmodem has them: a fax machine that calls a modem
 *    profile (fax on) is received into fax_dir; Stasis(<app>,sendfax,
 *    <file>[,<ident>[,<options>]]) sends TIFF files (DModemSendFax), and
 *    Stasis(<app>,receivefax,<file>[,<ident>[,<options>]]) answers as a fax machine
 *    (DModemReceiveFax); DMODEMFAX* set as they do, but as soon as the
 *    result is final (the MCF to the last page): the channel is gone
 *    with the other machine's hangup, which may come right after it.
 *    Stasis(<app>,faxdetect[,<seconds>[,<options>]]) listens for a fax
 *    machine (DModemFaxDetect): its CNG, or its T.38 offer on the SIP
 *    side, and continues the call at the fax extension.
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
#include <cinttypes>
#include <csignal>
#include <cstdarg>
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
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dm_types.h"
#include "client.h"
#include "dmodem.h"
#include "dmodem_bridge.h"
#include "g711.h"
#include "json.h"
#include "sip.h"

namespace {

/* a UDP socket closed on exec (fcntl(): Haiku has no SOCK_CLOEXEC) */
int udp_socket()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd >= 0)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

const s16 IDLE_MS = 200;        /* no packet this long: go on alone */
const s16 MAX_SAMPLES = 1600;   /* in a packet, 200 ms */

/*
 *    configuration and logging
 */

/* dmodem.conf: its sections' name = value lines, in order */
using Section = std::vector<std::pair<std::string, std::string>>;
std::map<std::string, Section> conf;

ari::Server server;
std::string app = "dmodem";
s16 verbose;                    /* -v: 1 the calls, 2 the bridge's details */
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
	s32 status = ari::request(server, method, "/ari/" + path, &body);
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
	u8 pt = 0;                      /* 0 PCMU, 8 PCMA */
	u16 seq = 0;
	u32 ts = 0, ssrc = 0;
	bool first = true;

	/* the socket closed */
	~Rtp() { if (fd >= 0) close(fd); }

	/* a UDP socket on host, any port: its port, -1 if none */
	s32 open(const std::string &host)
	{
		struct sockaddr_in a = {};
		socklen_t len = sizeof(a);
		fd = udp_socket();
		a.sin_family = AF_INET;
		if (fd < 0 ||
		    inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
		    bind(fd, reinterpret_cast<struct sockaddr *>(&a),
			 sizeof(a)) ||
		    getsockname(fd, reinterpret_cast<struct sockaddr *>(&a),
				&len))
			return -1;
		ssrc = static_cast<u32>(random());
		seq = static_cast<u16>(random());
		return ntohs(a.sin_port);
	}
	/* where Asterisk takes the packets; false if not an address */
	bool set_peer(const std::string &addr, const std::string &port)
	{
		peer.sin_family = AF_INET;
		peer.sin_port =
			htons(static_cast<u16>(
				strtol(port.c_str(), nullptr, 10)));
		return inet_pton(AF_INET, addr.c_str(), &peer.sin_addr) == 1 &&
			peer.sin_port;
	}
	/* a packet's payload into buf (its octets), within timeout_ms: >0
	   the samples, 0 none came, -1 an error */
	s16 recv(u8 *buf, s16 max, s16 timeout_ms)
	{
		struct pollfd p = { fd, POLLIN, 0 };
		u8 pkt[12 + 60 + MAX_SAMPLES];
		int r = poll(&p, 1, timeout_ms);
		if (r <= 0)
			return r < 0 && errno != EINTR ? -1 : 0;
		ssize_t n = ::recv(fd, pkt, sizeof(pkt), 0);
		if (n < 12 || (pkt[0] >> 6) != 2)
			return 0;
		/* past the CSRCs and an extension; the padding off */
		size_t off = 12 + 4 * (pkt[0] & 0x0f);
		if ((pkt[0] & 0x10) && static_cast<size_t>(n) >= off + 4)
			off += 4 + 4 * (static_cast<size_t>(pkt[off + 2]) << 8 |
					 pkt[off + 3]);
		if (pkt[0] & 0x20)
			n -= pkt[n - 1];
		if (static_cast<size_t>(n) <= off || (pkt[1] & 0x7f) != pt)
			return 0;       /* (not the call's law: DTMF, CN) */
		n -= off;
		if (n > max)
			n = max;
		memcpy(buf, pkt + off, n);
		return static_cast<s16>(n);
	}
	/* n samples (octets) to Asterisk */
	void send(const u8 *buf, s16 n)
	{
		u8 pkt[12 + MAX_SAMPLES];
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
enum Kind { MODEM, SENDFAX, RECEIVEFAX, FAXDETECT };

struct Call {
	std::string id, name;           /* the channel (id: its uniqueid) */
	Kind kind = MODEM;
	std::string profile, options;   /* MODEM: Stasis(app,profile,options) */
	std::string file, ident;        /* the faxes: Stasis(app,sendfax,file,
					   ident,options) */
	std::string fax_options;
	std::vector<std::string> faxopt;        /* DMODEMFAXOPT_* (see
						   fax_read_settings()) */
	std::string state, caller, caller_name, exten, context;
	std::atomic<bool> hungup{false};
};

std::mutex calls_lock;
std::map<std::string, std::shared_ptr<Call>> calls;
/* the external media channels of ours: true once in Stasis (their
   StasisStart starts no call) */
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
s16 next_block(Call &c, Rtp &rtp, enum dmodem_law law, u8 *in)
{
	if (c.hungup)
		return 0;
	s16 n = rtp.recv(in, MAX_SAMPLES, IDLE_MS);
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

/* the variable of a fax setting item: DMODEMFAXOPT_ and the item in
   capitals */
std::string faxopt_var(const char *item)
{
	std::string var = "DMODEMFAXOPT_";
	for (const char *p = item; *p; p++)
		var += static_cast<char>(toupper(static_cast<unsigned char>(*p)));
	return var;
}

/* the channel's variables DMODEMFAXOPT_<ITEM> (as DMODEMFAXOPT() of
   app_dmodem sets them) into c.faxopt, in the order of
   dmbridge_faxopt_items: one request (EVAL() of them all, | between
   them), so that the call does not wait for one for each.  The last
   item (headerinfo, free text) is all that is after the last | before
   it, so its text can have a | too */
void fax_read_settings(Call &c)
{
	std::string expr = "EVAL(";
	size_t n = 0;
	for (; dmbridge_faxopt_items[n]; n++)
		expr += (n ? "|${" : "${") + faxopt_var(dmbridge_faxopt_items[n]) +
			"}";
	std::string v = get_var(c.id, expr + ")");
	c.faxopt.clear();
	size_t at = 0, bar;
	while (c.faxopt.size() + 1 < n &&
	       (bar = v.find('|', at)) != std::string::npos) {
		c.faxopt.push_back(v.substr(at, bar - at));
		at = bar + 1;
	}
	c.faxopt.push_back(v.substr(at));
}

/* the channel's own fax settings (c.faxopt, read at the start of the
   call) over *f */
void fax_channel_settings(const Call &c, struct dmbridge_fax *f)
{
	for (size_t k = 0; dmbridge_faxopt_items[k] && k < c.faxopt.size();
	     k++) {
		const std::string &v = c.faxopt[k];
		if (!v.empty() &&
		    dmbridge_faxopt(&host, f, dmbridge_faxopt_items[k],
				    v.c_str()))
			say("WARNING %s: %s: not valid: '%s'", c.name.c_str(),
			    faxopt_var(dmbridge_faxopt_items[k]).c_str(),
			    v.c_str());
	}
}

/* how a fax runs: Stasis()'s fax options, as DModemSendFax has them */
struct FaxRun {
	bool debug = false;     /* d: each phase in the log */
	bool audio_only = false;        /* F: no T.38 */
	bool ask_t38 = false;   /* z: a sender asks for T.38 */
	bool need_t38 = false;  /* T: T.38 or no fax */
	bool status_events = false;     /* s: DModemFaxStatus user events */
};

/* the options into *r: false if one is unknown, or F and T together */
bool fax_options(const std::string &o, FaxRun *r)
{
	for (char ch : o) {
		switch (ch) {
		case 'd': r->debug = true; break;
		case 'f': break;        /* (audio after T.38: anyway) */
		case 'F': r->audio_only = true; break;
		case 'z': r->ask_t38 = true; break;
		case 'T': r->need_t38 = true; break;
		case 's': r->status_events = true; break;
		default: return false;
		}
	}
	return !(r->audio_only && r->need_t38);
}

std::atomic<int> fax_session_seq{0};    /* the last fax session's number */

s64 now_us();

/* a fax that runs, for the list that SIGUSR1 writes to the log */
struct FaxSession {
	std::string channel, file;
	bool sending = false, t38 = false;
	s64 start_us = 0;
	int pages = 0;
	s32 bps = 0;
	std::string phase;
};

/* the faxes that run and the totals since the start, with fax_lock */
std::mutex fax_lock;
std::map<int, FaxSession> fax_sessions;
struct FaxStats {
	int tx_attempts = 0, rx_attempts = 0, completed = 0, failed = 0;
	std::map<std::string, int> reasons;     /* failures by code */
} fax_stats;

volatile sig_atomic_t fax_report;       /* SIGUSR1: the list to the log */

/* s as a JSON string, in quotes */
std::string json_string(const std::string &s)
{
	std::string o = "\"";
	char buf[8];
	for (unsigned char ch : s) {
		if (ch == '"' || ch == '\\') {
			o += '\\';
			o += static_cast<char>(ch);
		} else if (ch < 0x20) {
			snprintf(buf, sizeof(buf), "\\u%04x", ch);
			o += buf;
		} else {
			o += static_cast<char>(ch);
		}
	}
	return o + "\"";
}

/* an ARI user event (an AMI UserEvent) of the call's channel: name, and
   the headers as its variables */
void user_event(const Call &c, const char *name,
		const std::vector<std::pair<std::string, std::string>> &h)
{
	std::string body = "{\"variables\":{", reply;
	for (size_t k = 0; k < h.size(); k++)
		body += (k ? "," : "") + json_string(h[k].first) + ":" +
			json_string(h[k].second);
	body += "}}";
	s32 st = ari::request_json(server, "POST", "/ari/events/user/" +
				   std::string(name) + "?application=" +
				   ari::url_encode(app) + "&source=channel:" +
				   ari::url_encode(c.id), body, &reply);
	if ((st < 200 || st > 299) && verbose >= 1)
		say("ari: user event %s: %d %s", name, st, reply.c_str());
}

/* the start of a fax: in the list, an attempt counted */
void fax_session_start(const Call &c, int session, bool sending,
		       const std::string &file)
{
	std::lock_guard<std::mutex> lk(fax_lock);
	FaxSession &f = fax_sessions[session];
	f.channel = c.name;
	f.file = file;
	f.sending = sending;
	f.start_us = now_us();
	if (sending)
		fax_stats.tx_attempts++;
	else
		fax_stats.rx_attempts++;
}

/* the fax's status now in the list */
void fax_session_update(int session, const struct dmodem_fax_status &st,
			bool t38)
{
	std::lock_guard<std::mutex> lk(fax_lock);
	auto it = fax_sessions.find(session);
	if (it == fax_sessions.end())
		return;
	it->second.pages = st.pages;
	it->second.bps = st.bps;
	it->second.t38 = t38;
	it->second.phase = st.phase ? st.phase : "";
}

/* the end of a fax: out of the list, its result in the totals */
void fax_session_end(int session, bool ok, const char *error)
{
	std::lock_guard<std::mutex> lk(fax_lock);
	fax_sessions.erase(session);
	if (ok) {
		fax_stats.completed++;
	} else {
		fax_stats.failed++;
		fax_stats.reasons[error ? error : "UNKNOWN"]++;
	}
}

/* the faxes that run and the totals to the log (on SIGUSR1) */
void fax_log_report()
{
	std::lock_guard<std::mutex> lk(fax_lock);
	say("dmodem-ari: fax: %zu now; transmit attempts %d, receive "
	    "attempts %d, completed %d, failed %d", fax_sessions.size(),
	    fax_stats.tx_attempts, fax_stats.rx_attempts,
	    fax_stats.completed, fax_stats.failed);
	for (const auto &r : fax_stats.reasons)
		say("dmodem-ari: fax: failed %s: %d", r.first.c_str(),
		    r.second);
	for (const auto &f : fax_sessions)
		say("dmodem-ari: fax %d: %s %s %s, %d pages at %" PRId32
		    " bit/s%s, %" PRId64 " s, %s", f.first,
		    f.second.channel.c_str(),
		    f.second.sending ? "sending" : "receiving",
		    f.second.file.c_str(), f.second.pages, f.second.bps,
		    f.second.t38 ? " over T.38" : "",
		    (now_us() - f.second.start_us) / 1000000,
		    f.second.phase.c_str());
}

/* the user event DModemSendFax or DModemReceiveFax: the fax's result,
   with the headers of app_dmodem's AMI event */
void fax_result_event(const Call &c, int session, bool sending,
		      const struct dmbridge_fax &fs,
		      const struct dmodem_fax_status &st, bool t38,
		      const std::string &file)
{
	user_event(c, sending ? "DModemSendFax" : "DModemReceiveFax", {
		{ "Channel", c.name },
		{ "SessionNumber", std::to_string(session) },
		{ "Status", st.ok ? "SUCCESS" : "FAILED" },
		{ "Error", st.error ? st.error : "UNKNOWN" },
		{ "StatusString", st.result ? st.result : "" },
		{ "Pages", std::to_string(st.pages) },
		{ "Rate", std::to_string(st.bps) },
		{ "Resolution", st.fine ? "8040x7700" : "8040x3850" },
		{ "ECM", st.ecm ? "yes" : "no" },
		{ "Transport", t38 ? "T.38" : "audio" },
		{ "LocalStationID", fs.ident },
		{ "RemoteStationID", st.remote_ident },
		{ "FileName", file },
	});
}

/* the fax's result in the call's variables, as DModemSendFax and
   DModemReceiveFax set them (t38: it went over T.38; session: its
   number): DMODEMFAX*, and res_fax's with fax_res_vars */
void fax_variables(const Call &c, const struct dmbridge_fax &fs,
		   const struct dmodem_fax_status &st, bool t38, int session)
{
	const char *status = st.ok ? "SUCCESS" : "FAILED";
	const char *res = st.fine ? "8040x7700" : "8040x3850";
	const char *error = st.error ? st.error : "UNKNOWN";
	const char *result = st.result ? st.result : "";
	std::string pages = std::to_string(st.pages);
	std::string rate = std::to_string(st.bps);

	set_var(c.id, "DMODEMFAXSTATUS", status);
	set_var(c.id, "DMODEMFAXRESULT", result);
	set_var(c.id, "DMODEMFAXERROR", error);
	set_var(c.id, "DMODEMFAXPAGES", pages);
	set_var(c.id, "DMODEMFAXRATE", rate);
	set_var(c.id, "DMODEMFAXRESOLUTION", res);
	set_var(c.id, "DMODEMFAXREMOTE", st.remote_ident);
	set_var(c.id, "DMODEMFAXECM", st.ecm ? "yes" : "no");
	set_var(c.id, "DMODEMFAXT38", t38 ? "yes" : "no");
	set_var(c.id, "DMODEMFAXSESSION", std::to_string(session));
	if (!fs.res_vars)
		return;
	set_var(c.id, "FAXSTATUS", status);
	set_var(c.id, "FAXSTATUSSTRING", result);
	set_var(c.id, "FAXERROR", error);
	set_var(c.id, "FAXPAGES", pages);
	set_var(c.id, "FAXBITRATE", rate);
	set_var(c.id, "FAXRESOLUTION", res);
	set_var(c.id, "REMOTESTATIONID", st.remote_ident);
	set_var(c.id, "LOCALSTATIONID", fs.ident);
}

/* a fax that did not start or stopped (error the code, result its text)
   in the variables, the totals and the user event (started: it is in
   the list, else it is an attempt all the same) */
void fax_failed(const Call &c, const struct dmbridge_fax &fs,
		const char *error, const char *result, int session,
		bool sending, const std::string &file, bool started)
{
	struct dmodem_fax_status st = {};
	st.error = error;
	st.result = result;
	fax_variables(c, fs, st, false, session);
	if (!started)
		fax_session_start(c, session, sending, file);
	fax_session_end(session, false, error);
	fax_result_event(c, session, sending, fs, st, false, file);
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
void t38_send(void *ctx, const unsigned char *pkt, s32 len)
{
	const T38Peer *p = static_cast<const T38Peer *>(ctx);
	sendto(p->fd, pkt, len, 0,
	       reinterpret_cast<const struct sockaddr *>(&p->to),
	       sizeof(p->to));
}

/* microseconds, monotonic */
s64 now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<s64>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
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
   for the answer (silence on the line) before it starts on the audio; a
   sender takes it when asked.  The wait ends after 6 s by the clock or
   6 s of the line's audio, whichever comes first: the audio's time is
   the fax machines' (their timers count its samples), and it can run
   ahead of the clock.  Between two of our calls on one Asterisk nothing
   paces the packets (each side sends one for each it gets), so if the
   answer never comes (Asterisk drops the request when the other leg is
   not in its bridge yet) a wait by the clock alone is the sender's T1
   (35 s of audio) in a second or two: "no fax machine" */
void run_fax(Call &c, Line &line, enum dmodem_law law,
	     enum dmodem_fax_dir dir, const std::string &file,
	     const struct dmbridge_fax &fs, const FaxRun &ro)
{
	struct dmodem_fax_config cfg = {};
	struct dmodem_fax_status st = {};
	struct dmodem_fax *f;
	u8 in[MAX_SAMPLES], out[MAX_SAMPLES];
	bool sending = dir == DMODEM_FAX_SEND, told = false, t38 = false;
	bool may_switch = line.leg != nullptr && fs.t38 && !ro.audio_only;
	bool asking = false;
	T38Peer peer = { line.udptl, {} };
	s64 t0 = 0, samples = 0, asked = 0, waited = 0;
	int session = ++fax_session_seq;
	const char *phase = nullptr;
	int pages = 0;

	cfg.law = law;
	cfg.dir = dir;
	cfg.file = file.c_str();
	cfg.ident = fs.ident;
	cfg.header = fs.header[0] ? fs.header : nullptr;
	cfg.max_bps = fs.max_bps;
	cfg.min_bps = fs.min_bps;
	cfg.no_ecm = !fs.ecm;
	cfg.modems = fs.modems;
	set_var(c.id, "DMODEMFAXFILE", file);
	if (ro.need_t38 && !may_switch) {
		say("WARNING %s: dmodem fax: T.38 needed, and the line cannot "
		    "have it", c.name.c_str());
		fax_failed(c, fs, "T38", "no T.38", session, sending, file,
			   false);
		return;
	}
	f = dmodem_fax_create(&cfg);
	if (!f) {
		say("WARNING %s: cannot %s fax file %s", c.name.c_str(),
		    sending ? "read" : "use", file.c_str());
		fax_failed(c, fs, "DOCUMENT", "no document", session, sending,
			   file, false);
		return;
	}
	fax_session_start(c, session, sending, file);
	if (verbose >= 1)
		say("%s: dmodem fax %d: %s %s", c.name.c_str(), session,
		    sending ? "sending" : "receiving", file.c_str());
	if (may_switch) {
		sip.accept_t38(line.leg, true);
		if ((!sending || ro.ask_t38 || ro.need_t38) &&
		    sip.ask_t38(line.leg)) {
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
					static_cast<u16>(l.t38_peer.port));
				inet_pton(AF_INET, l.t38_peer.addr.c_str(),
					  &peer.to.sin_addr);
				f = dmodem_fax_create(&cfg);
				if (!f) {
					say("WARNING %s: cannot %s fax file %s",
					    c.name.c_str(),
					    sending ? "read" : "use", file.c_str());
					fax_failed(c, fs, "DOCUMENT",
						   "no document", session,
						   sending, file, true);
					return;
				}
				if (verbose >= 1)
					say("%s: dmodem fax: T.38 (version %d, "
					    "%s:%" PRId32 "%s)", c.name.c_str(),
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
				now_us() - asked < fs.t38_timeout * 1000LL &&
				waited < fs.t38_timeout * 8LL;
			if (ro.need_t38 && !t38 && !asking &&
			    l.t38 != ari::T38_ON) {
				/* no T.38: no fax (option T) */
				say("WARNING %s: dmodem fax: no T.38",
				    c.name.c_str());
				dmodem_fax_destroy(f);
				fax_failed(c, fs, "T38", "no T.38", session,
					   sending, file, true);
				return;
			}
		}
		if (t38) {
			unsigned char pkt[2048];
			struct pollfd pf = { line.udptl, POLLIN, 0 };
			ssize_t n;
			s64 due;
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
			s16 n = next_block(c, line.rtp, law, in);
			if (n <= 0) {
				dmodem_fax_hangup(f);
				break;
			}
			if (asking) {
				/* (T.38 or not: the fax machine waits) */
				memset(out, law == DMODEM_LAW_ALAW ? 0xd5 : 0xff,
				       n);
				line.rtp.send(out, n);
				waited += n;
				continue;
			}
			dmodem_fax_process_g711(f, in, out, n);
			line.rtp.send(out, n);
		}
		dmodem_fax_status(f, &st);
		fax_session_update(session, st, t38);
		if (st.phase && (!phase || strcmp(phase, st.phase) ||
				 st.pages != pages)) {
			if (ro.debug)
				say("%s: dmodem fax %d: %s", c.name.c_str(),
				    session, st.phase);
			if (ro.status_events)
				user_event(c, "DModemFaxStatus", {
					{ "Channel", c.name },
					{ "SessionNumber",
					  std::to_string(session) },
					{ "Operation",
					  sending ? "send" : "receive" },
					{ "Status", st.phase },
					{ "Pages", std::to_string(st.pages) },
					{ "FileName", file },
				});
			phase = st.phase;
			pages = st.pages;
		}
		if (st.complete && !told) {
			fax_variables(c, fs, st, t38, session);
			told = true;
		}
		if (st.done)
			break;
	}
	dmodem_fax_status(f, &st);
	if (!told && !c.hungup)
		fax_variables(c, fs, st, t38, session);
	fax_session_end(session, st.ok, st.error);
	fax_result_event(c, session, sending, fs, st, t38, file);
	/* (the received file closed with it) */
	dmodem_fax_destroy(f);
	if (verbose >= 1)
		say("%s: dmodem fax %d %s: %s, %d pages at %" PRId32 " bit/s%s%s, "
		    "remote '%s'", c.name.c_str(), session,
		    sending ? "sent" : "received",
		    st.result, st.pages, st.bps, st.ecm ? " (ECM)" : "",
		    t38 ? " over T.38" : "", st.remote_ident);
}

/*
 *    voice: a person's call to an extension, a DSVD or ASVD caller's voice
 */

/* a channel of ours beside a call (the extension's Local channel, a
   snoop): its StasisStart starts no call; started once in Stasis, up
   once answered, gone once hung up */
struct Leg {
	std::atomic<bool> started{false}, up{false}, gone{false};
};
std::map<std::string, std::shared_ptr<Leg>> legs;

bool media_started(const std::string &id, s16 ms);

/* a channel id of ours, what it is for first */
std::string new_id(const char *what)
{
	char b[48];
	snprintf(b, sizeof(b), "dmodem-%s-%08lx%04lx", what,
		 static_cast<unsigned long>(random()),
		 static_cast<unsigned long>(random() & 0xffff));
	return b;
}

/* a Leg for channel id, before it is made */
std::shared_ptr<Leg> add_leg(const std::string &id)
{
	auto l = std::make_shared<Leg>();
	std::lock_guard<std::mutex> lk(calls_lock);
	legs[id] = l;
	return l;
}

/* channel id's Leg forgotten */
void forget_leg(const std::string &id)
{
	std::lock_guard<std::mutex> lk(calls_lock);
	legs.erase(id);
}

/* true once leg l is in Stasis, within ms */
bool leg_started(const Leg &l, s16 ms)
{
	for (s16 k = 0; k < ms / 10 && !l.started; k++)
		usleep(10000);
	return l.started;
}

/* the profile's voice extension (exten@context, exten alone in the
   call's context; a channel to dial, one with a '/', as it is) on a
   channel of ours, in Stasis at once and dialled, the caller's number
   its caller ID: its id, "" if not */
std::string ext_dial(const Call &c, const char *voice)
{
	std::string v = voice, endpoint;
	if (v.find('/') != std::string::npos)
		endpoint = v;
	else
		endpoint = "Local/" + v + (v.find('@') == std::string::npos ?
					   "@" + c.context : "") + "/n";
	std::string id = new_id("ext");
	auto l = add_leg(id);
	if (!ari_call("POST", "channels/create?endpoint=" +
		      ari::url_encode(endpoint) + "&app=" + ari::url_encode(app) +
		      "&appArgs=leg&channelId=" + ari::url_encode(id) +
		      "&originator=" + ari::url_encode(c.id)) ||
	    !leg_started(*l, 5000)) {
		say("WARNING %s: no channel %s", c.name.c_str(),
		    endpoint.c_str());
		ari_call("DELETE", "channels/" + id);
		forget_leg(id);
		return std::string();
	}
	/* (a Local channel's other half has its connected line as its
	   caller ID) */
	set_var(id, "CONNECTEDLINE(num)", c.caller);
	if (!c.caller_name.empty())
		set_var(id, "CONNECTEDLINE(name)", c.caller_name);
	if (!ari_call("POST", "channels/" + id + "/dial?timeout=60")) {
		ari_call("DELETE", "channels/" + id);
		forget_leg(id);
		return std::string();
	}
	if (verbose >= 1)
		say("%s: dmodem: voice to %s", c.name.c_str(), endpoint.c_str());
	return id;
}

/* the extension's channel hung up (if it is not gone) and forgotten */
void ext_end(const std::string &id)
{
	if (id.empty())
		return;
	ari_call("DELETE", "channels/" + id);
	forget_leg(id);
}

/* the voice leg: the caller through to the extension in the call's
   bridge, our line taken out of it into a bridge of its own with a
   snoop of the caller's own audio (for the modem's ear, its calling
   tones: none of the extension's); 1 when the leg is over (the
   extension gone, or the caller's modem asking for data: the leg hung
   up, our line back in the call's bridge), -1 if the call hung up */
int voice_leg(Call &c, const struct dmbridge_profile &p,
	      enum dmodem_law law, Line &line, struct dmbridge_call &bc)
{
	u8 in[MAX_SAMPLES], out[MAX_SAMPLES];
	std::string ext = ext_dial(c, p.voice), snoop, bridge;
	std::shared_ptr<Leg> el, sl;
	ari::Json r;
	bool ringing = false;
	int res = 0;

	if (ext.empty())
		return c.hungup ? -1 : 1;
	{
		std::lock_guard<std::mutex> lk(calls_lock);
		el = legs[ext];
	}
	snoop = new_id("snoop");
	sl = add_leg(snoop);
	if (!ari_call("POST", "channels/" + c.id + "/snoop?spy=in&app=" +
		      ari::url_encode(app) + "&appArgs=snoop&snoopId=" +
		      ari::url_encode(snoop)) || !leg_started(*sl, 5000) ||
	    !ari_call("POST", "bridges?type=mixing", &r)) {
		say("WARNING %s: no snoop on the call", c.name.c_str());
		ari_call("DELETE", "channels/" + snoop);
		forget_leg(snoop);
		ext_end(ext);
		return c.hungup ? -1 : 1;
	}
	bridge = r["id"].str();
	ari_call("POST", "bridges/" + line.bridge + "/addChannel?channel=" +
		 ari::url_encode(ext));
	ari_call("POST", "bridges/" + line.bridge + "/removeChannel?channel=" +
		 ari::url_encode(line.media));
	ari_call("POST", "bridges/" + bridge + "/addChannel?channel=" +
		 ari::url_encode(snoop + "," + line.media));
	/* the caller hears it ring until it answers */
	if (!el->up) {
		ringing = ari_call("POST", "channels/" + c.id + "/ring");
	}
	while (!res) {
		s16 n = next_block(c, line.rtp, law, in);
		if (n <= 0) {
			res = -1;
			break;
		}
		dmodem_process_g711(bc.d, in, out, n);
		line.rtp.send(out, n);
		if (ringing && el->up) {
			ari_call("DELETE", "channels/" + c.id + "/ring");
			ringing = false;
		}
		if (el->gone) {
			if (verbose >= 1)
				say("%s: dmodem: the extension's leg ended: "
				    "the modem answers", c.name.c_str());
			res = 1;
		} else if (dmbridge_voice_step(&bc, n) == DMBRIDGE_DATA) {
			/* (the bridge's log says why) */
			res = 1;
		}
	}
	if (ringing)
		ari_call("DELETE", "channels/" + c.id + "/ring");
	ext_end(ext);
	ari_call("DELETE", "channels/" + snoop);
	forget_leg(snoop);
	ari_call("DELETE", "bridges/" + bridge);
	if (res > 0)
		ari_call("POST", "bridges/" + line.bridge +
			 "/addChannel?channel=" + ari::url_encode(line.media));
	return res;
}

/* a DSVD or ASVD voice channel's leg: the extension on a channel of
   ours, in a bridge with an external media channel whose RTP (mu-law)
   is ours; made and taken down in threads of their own (ARI's requests
   take time the line hasn't got) */
struct DsvdLeg {
	std::string ext, media, bridge;
	std::shared_ptr<Leg> el;
	Rtp rtp;
	std::atomic<bool> ready{false}, failed{false};
	std::thread maker;
};

/* (see DsvdLeg) the leg made: ready, or failed */
void dsvd_make(DsvdLeg *d, const Call *c, std::string voice)
{
	ari::Json r;
	s32 port;

	d->ext = ext_dial(*c, voice.c_str());
	if (d->ext.empty()) {
		d->failed = true;
		return;
	}
	{
		std::lock_guard<std::mutex> lk(calls_lock);
		d->el = legs[d->ext];
	}
	d->rtp.pt = 0;
	port = d->rtp.open("127.0.0.1");
	d->media = new_id("dsvd");
	{
		std::lock_guard<std::mutex> lk(calls_lock);
		media_channels.emplace(d->media, false);
	}
	if (port < 0 ||
	    !ari_call("POST", "channels/externalMedia?app=" +
		      ari::url_encode(app) + "&channelId=" +
		      ari::url_encode(d->media) + "&external_host=" +
		      ari::url_encode("127.0.0.1:" + std::to_string(port)) +
		      "&format=ulaw", &r) ||
	    (!d->rtp.set_peer(r["channelvars"]["UNICASTRTP_LOCAL_ADDRESS"].str(),
			      r["channelvars"]["UNICASTRTP_LOCAL_PORT"].str()) &&
	     !d->rtp.set_peer(get_var(d->media, "UNICASTRTP_LOCAL_ADDRESS"),
			      get_var(d->media, "UNICASTRTP_LOCAL_PORT"))) ||
	    !media_started(d->media, 5000) ||
	    !ari_call("POST", "bridges?type=mixing", &r)) {
		d->failed = true;
		return;
	}
	d->bridge = r["id"].str();
	if (!ari_call("POST", "bridges/" + d->bridge + "/addChannel?channel=" +
		      ari::url_encode(d->ext + "," + d->media))) {
		d->failed = true;
		return;
	}
	d->ready = true;
}

/* (see DsvdLeg) the leg taken down, d deleted */
void dsvd_unmake(DsvdLeg *d)
{
	if (d->maker.joinable())
		d->maker.join();
	ext_end(d->ext);
	if (!d->media.empty()) {
		ari_call("DELETE", "channels/" + d->media);
		std::lock_guard<std::mutex> lk(calls_lock);
		media_channels.erase(d->media);
	}
	if (!d->bridge.empty())
		ari_call("DELETE", "bridges/" + d->bridge);
	delete d;
}

/* the leg's frames after n samples of the line: the extension's
   speech to the voice channel, the far end's to it; false once the leg
   has failed or gone */
bool dsvd_move(DsvdLeg *d, struct dmbridge_call &bc, s16 n)
{
	u8 buf[MAX_SAMPLES];
	int16_t pcm[MAX_SAMPLES];
	s16 k;

	if (d->failed || (d->ready && d->el->gone))
		return false;
	if (!d->ready)
		return true;
	while ((k = d->rtp.recv(buf, MAX_SAMPLES, 0)) > 0) {
		for (s16 i = 0; i < k; i++)
			pcm[i] = dsp::g711::ulaw2lin(buf[i]);
		dmbridge_dsvd_mic(&bc, pcm, k);
	}
	k = static_cast<s16>(dmbridge_dsvd_ear(&bc, pcm, n));
	if (k > 0) {
		for (s16 i = 0; i < k; i++)
			buf[i] = dsp::g711::lin2ulaw(pcm[i]);
		d->rtp.send(buf, k);
	}
	return true;
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
	u8 in[MAX_SAMPLES], out[MAX_SAMPLES];
	std::string status;
	bool orig = c.options.find('o') != std::string::npos, fax = false;
	DsvdLeg *dsvd = nullptr;        /* a DSVD voice channel's leg */

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
		s16 n;

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
			if (dsvd && !dsvd_move(dsvd, bc, n)) {
				/* the extension gone: the voice channel
				   closed, the data going on */
				dsvd_unmake(dsvd);
				dsvd = nullptr;
				dmbridge_dsvd_close(&bc);
			}
			ev = dmbridge_call_step(&bc, n);
		}
		if (ev == DMBRIDGE_VOICE) {
			/* a person: to the extension until its leg is over,
			   then the modem answers */
			int r = voice_leg(c, p, law, line, bc);
			if (r < 0) {
				bc.status = "HANGUP";
				break;
			}
			if (dmbridge_call_resume(&bc))
				break;
			continue;
		}
		if (ev == DMBRIDGE_DSVD_OPEN && !dsvd) {
			/* a DSVD or ASVD caller's voice channel: its leg to
			   the extension, made meanwhile */
			dsvd = new DsvdLeg;
			dsvd->maker = std::thread(dsvd_make, dsvd, &c,
						  std::string(p.voice));
			continue;
		}
		if (ev == DMBRIDGE_DSVD_CLOSED) {
			if (dsvd) {
				std::thread(dsvd_unmake, dsvd).detach();
				dsvd = nullptr;
			}
			continue;
		}
		if (ev == DMBRIDGE_CONNECTED) {
			char connected[24];
			const char *env[] = {
				"DMODEM_CHANNEL", c.name.c_str(),
				"DMODEM_PROFILE", c.profile.c_str(),
				"DMODEM_RATE", bc.rate,
				"DMODEM_MODULATION", bc.modulation,
				"DMODEM_PROTOCOL", bc.protocol,
				"DMODEM_COMPRESSION", bc.compression,
				"DMODEM_CALLERID", c.caller.c_str(),
				"DMODEM_DIALED", c.exten.c_str(),
				"DMODEM_CONNECTED", connected,
				nullptr
			};
			snprintf(connected, sizeof(connected), "%" PRId64,
				 static_cast<s64>(time(nullptr)));
			set_var(c.id, "DMODEMRATE", bc.rate);
			set_var(c.id, "DMODEMMODULATION", bc.modulation);
			set_var(c.id, "DMODEMPROTOCOL", bc.protocol);
			set_var(c.id, "DMODEMCOMPRESSION", bc.compression);
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
			struct dmbridge_fax pf = p.fax;
			fax_channel_settings(c, &pf);
			run_fax(c, line, law, DMODEM_FAX_RECEIVE, path, pf,
				FaxRun());
			fax = true;
			break;
		}
		if (ev == DMBRIDGE_END)
			break;
	}
	if (dsvd)
		dsvd_unmake(dsvd);
	dmbridge_call_hangup(&bc);
	status = fax ? "FAX" : bc.status;
	dmbridge_call_end(&bc);
	return status;
}

/* a UDP socket of ours on host, any port, into *fd: its port, -1 if
   none */
s32 udp_open(const std::string &host, int *fd)
{
	struct sockaddr_in a = {};
	socklen_t len = sizeof(a);
	*fd = udp_socket();
	a.sin_family = AF_INET;
	if (*fd < 0 || inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
	    bind(*fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) ||
	    getsockname(*fd, reinterpret_cast<struct sockaddr *>(&a), &len))
		return -1;
	return ntohs(a.sin_port);
}

/* true once channel id has had its StasisStart, within ms */
bool media_started(const std::string &id, s16 ms)
{
	for (s16 k = 0; k < ms / 10; k++) {
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
	s32 rtp_port, udptl_port;

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
	line.rtp.pt = static_cast<u8>(l.pt);
	law = l.pt == 8 ? DMODEM_LAW_ALAW : DMODEM_LAW_ULAW;
	if (!line.rtp.set_peer(l.rtp_addr, std::to_string(l.rtp_peer_port))) {
		say("ERROR %s: no RTP address in Asterisk's SDP",
		    c.name.c_str());
		return false;
	}
	return true;
}

/* the call's line: the channel answered (unless it is up, or answer is
   false), our UDP port,
   the external media channel that sends the call's audio there in law
   (T.38: the SIP leg, taking T.38 offers if t38), and the two in a
   bridge; false (logged) if any of it fails */
bool open_line(Call &c, enum dmodem_law &law, Line &line, bool t38,
	       bool answer = true)
{
	ari::Json r;
	s32 port;

	line.rtp.pt = law == DMODEM_LAW_ALAW ? 8 : 0;
	if (answer && c.state != "Up" &&
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
			/* (its StasisStart may have come already) */
			std::lock_guard<std::mutex> lk(calls_lock);
			media_channels.emplace(line.media, false);
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
		/* a channel not yet in Stasis can't go into the bridge */
		if (!media_started(line.media, 5000)) {
			say("ERROR %s: the external media channel never "
			    "reached Stasis", c.name.c_str());
			return false;
		}
	}
	if (!ari_call("POST", "bridges?type=mixing", &r))
		return false;
	line.bridge = r["id"].str();
	return ari_call("POST", "bridges/" + line.bridge + "/addChannel?channel=" +
			ari::url_encode(c.id + "," + line.media));
}

/* the line taken down: its media channel (or SIP leg's channel) and
   bridge deleted, the leg forgotten; line empty after */
void close_line(Line &line)
{
	if (!line.media.empty())
		ari_call("DELETE", "channels/" + line.media);
	if (!line.bridge.empty())
		ari_call("DELETE", "bridges/" + line.bridge);
	if (line.leg)
		sip.forget(line.leg);
	{
		std::lock_guard<std::mutex> lk(calls_lock);
		media_channels.erase(line.media);
	}
	line.media.clear();
	line.bridge.clear();
	line.leg = nullptr;
}

/* the options of Stasis(app,faxdetect,seconds,options), as
   DModemFaxDetect has them: c the CNG only, t the T.38 request only, n
   no answer, r ringing, x no jump to the fax extension; false if one is
   unknown, or c and t together */
bool detect_options(const std::string &s, bool *cng_only, bool *t38_only,
		    bool *no_answer, bool *ringing, bool *no_jump)
{
	*cng_only = *t38_only = *no_answer = *ringing = *no_jump = false;
	for (char ch : s) {
		if (ch == 'c')
			*cng_only = true;
		else if (ch == 't')
			*t38_only = true;
		else if (ch == 'n')
			*no_answer = true;
		else if (ch == 'r')
			*ringing = true;
		else if (ch == 'x')
			*no_jump = true;
		else
			return false;
	}
	return !(*cng_only && *t38_only);
}

/* Stasis(app,faxdetect[,seconds[,options]]): the line opened (a T.38
   offer on the SIP side taken, unless option c: the caller is then on
   T.38 with Asterisk, and the fax application's own leg asks for it
   again), then up to seconds (default 4) of listening for a fax machine: its
   CNG, or a T.38 offer on the SIP side.  DMODEMFAXDETECTED is CNG, T38
   or NONE; on a fax, DMODEMFAXEXTEN the extension, and the call
   continues at the context's fax extension if it has one (not with
   option x: the dialplan reads DMODEMFAXDETECTED).  True if the
   call continues there (run_call() then does not continue it) */
bool run_detect(Call &c)
{
	bool cng_only, t38_only, no_answer, ringing, no_jump;
	enum dmodem_law law = call_law(c, DMODEM_LAW_UNKNOWN);
	double secs = c.file.empty() ? 4 : atof(c.file.c_str());
	std::string what = "NONE", playback;
	struct dmodem_faxdetect *d;
	u8 in[MAX_SAMPLES];
	Line line;
	bool jumped = false;
	ari::Json r;

	if (secs <= 0) {
		say("WARNING %s: faxdetect: bad time '%s'", c.name.c_str(),
		    c.file.c_str());
		secs = 4;
	}
	if (!detect_options(c.fax_options, &cng_only, &t38_only, &no_answer,
			    &ringing, &no_jump)) {
		say("WARNING %s: faxdetect: bad options '%s'", c.name.c_str(),
		    c.fax_options.c_str());
		return false;
	}
	if (!open_line(c, law, line, !cng_only, !no_answer)) {
		close_line(line);
		return false;
	}
	/* (on the channel: a playback on the bridge would put a third
	   channel in it, and the bridge's change to mixing would change
	   the channel's formats) */
	if (ringing && ari_call("POST", "channels/" + c.id +
				"/play?media=tone:ring", &r))
		playback = r["id"].str();
	d = dmodem_faxdetect_create(law);
	s64 end = now_us() + static_cast<s64>(secs * 1e6);
	while (d && what == "NONE" && now_us() < end) {
		s16 n = next_block(c, line.rtp, law, in);
		if (n <= 0)
			break;
		if (!t38_only && dmodem_faxdetect_process_g711(d, in, n))
			what = "CNG";
		else if (!cng_only && line.leg && sip.get(line.leg).t38_offered)
			what = "T38";
	}
	dmodem_faxdetect_destroy(d);
	if (!playback.empty())
		ari_call("DELETE", "playbacks/" + playback);
	close_line(line);
	if (c.hungup)
		return false;
	set_var(c.id, "DMODEMFAXDETECTED", what);
	if (what == "NONE") {
		if (verbose >= 1)
			say("%s: dmodem fax detect: no fax", c.name.c_str());
		return false;
	}
	set_var(c.id, "DMODEMFAXEXTEN", c.exten);
	if (no_jump) {
		if (verbose >= 1)
			say("%s: dmodem fax detect: %s", c.name.c_str(),
			    what.c_str());
	} else if (c.exten != "fax" &&
		   get_var(c.id, "DIALPLAN_EXISTS(" + c.context + ",fax,1)") == "1") {
		if (verbose >= 1)
			say("%s: dmodem fax detect: %s, to the fax extension",
			    c.name.c_str(), what.c_str());
		jumped = ari_call("POST", "channels/" + c.id +
				  "/continue?context=" +
				  ari::url_encode(c.context) +
				  "&extension=fax&priority=1");
	} else if (verbose >= 1) {
		say("%s: dmodem fax detect: %s (no fax extension in %s)",
		    c.name.c_str(), what.c_str(), c.context.c_str());
	}
	return jumped;
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

	if (c->kind == FAXDETECT) {
		bool jumped = run_detect(*c);
		if (!c->hungup && !jumped)
			ari_call("POST", "channels/" + c->id + "/continue");
		std::lock_guard<std::mutex> lk(calls_lock);
		calls.erase(c->id);
		return;
	}
	fax_read_settings(*c);
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
		/* the fax settings of [general], the channel's own
		   (DMODEMFAXOPT_*), the ident of Stasis() */
		FaxRun ro;
		general_fax(&fs);
		fax_channel_settings(*c, &fs);
		if (!c->ident.empty())
			snprintf(fs.ident, sizeof(fs.ident), "%s",
				 c->ident.c_str());
		if (!fax_options(c->fax_options, &ro)) {
			say("WARNING %s: bad fax options '%s'",
			    c->name.c_str(), c->fax_options.c_str());
			set_var(c->id, "DMODEMFAXSTATUS", "FAILED");
		} else if (c->file.empty()) {
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
					c->file, fs, ro);
			else if (!c->hungup)
				set_var(c->id, "DMODEMFAXSTATUS", "FAILED");
		}
	}
	close_line(line);
	if (!c->hungup) {
		if (c->kind == MODEM)
			set_var(c->id, "DMODEMSTATUS", status);
		ari_call("POST", "channels/" + c->id + "/continue");
	}
	std::lock_guard<std::mutex> lk(calls_lock);
	calls.erase(c->id);
}

/* one of ARI's events */
void event(const ari::Json &e)
{
	const std::string &type = e["type"].str();
	const ari::Json &ch = e["channel"];
	const std::string &id = ch["id"].str();

	if (type == "StasisStart") {
		std::lock_guard<std::mutex> lk(calls_lock);
		auto l = legs.find(id);
		if (l != legs.end()) {
			l->second->started = true;
			if (ch["state"].str() == "Up")
				l->second->up = true;
			return;
		}
		auto m = media_channels.find(id);
		if (m != media_channels.end()) {
			m->second = true;       /* (a SIP leg: up) */
			return;
		}
		if (!ch["name"].str().compare(0, 11, "UnicastRTP/")) {
			/* ours, in Stasis before externalMedia's answer
			   reached open_line() */
			media_channels[id] = true;
			return;
		}
		auto c = std::make_shared<Call>();
		const std::string &what = e["args"][0].str();
		c->id = id;
		c->name = ch["name"].str();
		c->state = ch["state"].str();
		c->caller = ch["caller"]["number"].str();
		c->exten = ch["dialplan"]["exten"].str();
		c->context = ch["dialplan"]["context"].str();
		c->caller_name = ch["caller"]["name"].str();
		if (what == "sendfax" || what == "receivefax") {
			c->kind = what == "sendfax" ? SENDFAX : RECEIVEFAX;
			c->file = e["args"][1].str();
			c->ident = e["args"][2].str();
			c->fax_options = e["args"][3].str();
		} else if (what == "faxdetect") {
			/* (seconds in file, options in fax_options) */
			c->kind = FAXDETECT;
			c->file = e["args"][1].str();
			c->fax_options = e["args"][2].str();
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
		auto l = legs.find(id);
		if (l != legs.end())
			l->second->gone = true;
	}
	/* a leg of ours answered */
	if (type == "ChannelStateChange" && ch["state"].str() == "Up") {
		std::lock_guard<std::mutex> lk(calls_lock);
		auto l = legs.find(id);
		if (l != legs.end())
			l->second->up = true;
	}
}

volatile sig_atomic_t stop;

/* SIGINT, SIGTERM: stop set, the main loop ends */
void on_signal(int)
{
	stop = 1;
}

/* SIGUSR1: the faxes and their totals to the log (by the main loop) */
void on_usr1(int)
{
	fax_report = 1;
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
			       strtol(bind.c_str() + colon + 1, nullptr, 10))) {
			fprintf(stderr, "dmodem-ari: no SIP socket on %s: %s\n",
				bind.c_str(), strerror(errno));
			return 1;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_usr1);
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
			say("dmodem-ari: app %s on %s:%" PRId32, app.c_str(),
			    server.host.c_str(), server.port);
			if (use_sip)
				say("dmodem-ari: T.38: the calls' legs on "
				    "PJSIP/%s to %s:%" PRId32,
				    sip_endpoint.c_str(),
				    sip.host().c_str(), sip.port());
		}
		if (fax_report) {
			fax_report = 0;
			fax_log_report();
		}
		s16 r = ws.read(msg, 500);
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
