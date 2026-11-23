/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_embed.cpp - dmodemd as a library (dmodem_embed.h): the modem
 *                       core in a thread of the program, its DTE one end
 *                       of a socket pair, the program's the other
 *
 *    The SIP line is SipModem's (sip_modem.h), as in dmodemd and the
 *    Android app.  The USB, ALSA and mISDN lines are run as dmodemd's
 *    main.cpp runs them (run_line()), on the socket in place of the pty,
 *    and with a wake socket that dmodem_embed_stop() writes to.
 *
 */

#include "dmodem_embed.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <dcore.h>
#include <dsplibs.h>
#include <log.h>

#include "dm_types.h"
#include "datafile.h"
#include "options.h"
#include "portable.h"
#include "run_loop.h"
#include "sip.h"
#include "sip_modem.h"
#ifdef DMODEM_ALSA
#include "alsa_line.h"
#endif
#ifdef DMODEM_USB
#include "usb_line.h"
#endif
#ifdef DMODEM_MISDN
#include "misdn_line.h"
#endif

#ifndef DMODEM_VERSION
#define DMODEM_VERSION "unknown"
#endif

namespace {

/* the SIP line: 8000 samples a second */
const s32 RATE = 8000;
/* the modem's name, as dmodemd's with no -n */
const char NAME[] = "dmodem0";

/* a string of the config: "" for NULL */
std::string str(const char *s)
{
	return s ? s : "";
}

/* the modem whose log the core's hook gives (one for each process) */
std::mutex hook_mu;
dmodem_embed *hook_owner;

} // namespace

/* the settings, copied: the thread's own */
struct EmbedConfig {
	int line = DMODEM_EMBED_SIP;
	std::string country, data_file;
	int log_level = 0;
	std::string sip_user, sip_password, sip_domain, sip_dns;
	int sip_port = 5060;
	bool sip_listen = false, sip_t38 = true, sip_v152 = true,
		sip_v150 = false, sip_clearmode = true, sip_tcp = true;
	int sip_srtp = 1, sip_red = 2, sip_jitter_ms = 60, sip_log_level = -1;
	std::string usb, alsa, alsa_ctl, misdn, misdn_msn;
	unsigned rate = 0;
	bool misdn_flip = true;
};

struct dmodem_embed {
	std::mutex mu;
	std::thread thread;
	/* the thread runs (until its end) */
	bool active = false;
	/* dmodem_embed_stop() was called */
	std::atomic<bool> stopping{false};
	/* the SIP modem that runs, for its stop() */
	dmd::SipModem *sip = nullptr;
	/* the line loop's wake socket: a byte into [1] stops it */
	int wake[2] = { -1, -1 };
	std::string error;
	dmodem_embed_log_fn log_fn = nullptr;
	void *log_ctx = nullptr;
	dmodem_embed_exit_fn exit_fn = nullptr;
	void *exit_ctx = nullptr;
};

namespace {

/* a line of the core's log to the owner's callback (the core's hook) */
void log_hook(const char *line)
{
	std::lock_guard<std::mutex> lk(hook_mu);
	if (!hook_owner || !hook_owner->log_fn)
		return;
	/* (the line with no newline) */
	std::string s(line);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	hook_owner->log_fn(hook_owner->log_ctx, s.c_str());
}

/* a line of this file's to the log */
void say(const char *fmt, const char *arg)
{
	char buf[512];
	snprintf(buf, sizeof(buf), fmt, arg ? arg : "");
	dcore::log_line(buf);
}

#if defined(DMODEM_ALSA) || defined(DMODEM_USB) || defined(DMODEM_MISDN)
/* the modem m on line (a USB, ALSA or mISDN one) with dte, as dmodemd's
   run_line(), until a byte comes on wake, the DTE has gone (0), or an
   error (-1) */
template <class Line>
s16 run_line(dcore::Modem &m, Line &line, dmd::Dte &dte, int wake,
	     const std::atomic<bool> &stopping)
{
	dcore::Modem *const modems[] = { &m };
	dmd::RunLoop loop;

	while (!stopping) {
		s32 wait = dmd::RunLoop::poll(modems, 1);
		int ret;

		loop.clear();
		line.watch(loop);
		loop.watch(wake);
		if (dte.want_read(m))
			loop.watch(dte.fd());
		if (dte.want_write())
			loop.watch_write(dte.fd());
		ret = loop.wait(wait);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			say("error: select: %s\n", strerror(errno));
			return -1;
		}
		if (line.pump(m) < 0)
			return -1;
		if (ret <= 0)
			continue;
		if (loop.ready(wake))
			return 0;
		if (loop.ready_write(dte.fd()))
			dte.flush();
		if (loop.ready(dte.fd())) {
			s16 r = dte.serve(m);
			if (r < 0)
				return -1;
			if (r > 0)
				return 0;
		}
	}
	return 0;
}
#endif

/* the thread of the modem: cfg's line, on the DTE dte_fd (closed at the
   end), until dmodem_embed_stop() or the program's end of the socket
   closed */
void run(dmodem_embed *e, EmbedConfig cfg, int dte_fd)
{
	dcore::Options mo;
	s16 ret = -1;

	dcore::log_level = cfg.log_level > 0 ?
		static_cast<u32>(cfg.log_level) : 0;
	{
		std::lock_guard<std::mutex> lk(hook_mu);
		hook_owner = e;
	}
	dcore::log_set_hook(log_hook);
	/* (the pumps' level follows log_level; no binary log) */
	dcore::log_open(NAME);
	prop_dp_init();

	mo.name = NAME;
	mo.dev_name = NAME;
	mo.rate = RATE;
	mo.country = cfg.country.empty() ? nullptr : cfg.country.c_str();

	/* the modem's data file loaded, run_it() run, the file saved */
	auto serve = [&](dcore::Modem &m, auto &&run_it) {
		std::unique_ptr<dmd::DataFile> data;
		if (!cfg.data_file.empty()) {
			data.reset(new dmd::DataFile(cfg.data_file.c_str()));
			data->load(&m.dsp_info());
		}
		ret = run_it();
		if (data)
			data->save(&m.dsp_info());
	};
	/* (dmodemd's -r: 9600 when not given) */
	u32 rate = cfg.rate ? cfg.rate : 9600;
	bool rate_given = cfg.rate != 0;

	{
		dmd::FdDte dte(dte_fd);

		switch (cfg.line) {
		case DMODEM_EMBED_SIP: {
			sip::Config sc;
			sc.user = cfg.sip_user.c_str();
			sc.password = cfg.sip_password.c_str();
			sc.domain = cfg.sip_domain.c_str();
			sc.port = cfg.sip_port > 0 ? cfg.sip_port : 5060;
			sc.listen = cfg.sip_listen;
			sc.record = nullptr;
			sc.t38 = cfg.sip_t38;
			sc.clearmode = cfg.sip_clearmode;
			sc.srtp = static_cast<s16>(cfg.sip_srtp);
			sc.v152 = cfg.sip_v152;
			sc.red = static_cast<s16>(cfg.sip_red);
			sc.jitter_ms = static_cast<s16>(cfg.sip_jitter_ms);
			sc.v150 = cfg.sip_v150;
			sc.tcp = cfg.sip_tcp;
			sc.dns = cfg.sip_dns.empty() ? nullptr :
				cfg.sip_dns.c_str();
			sc.log_level = static_cast<s16>(cfg.sip_log_level);
			dmd::SipModem sm(dte, mo);
			if (!sm.ok())
				break;
			{
				std::lock_guard<std::mutex> lk(e->mu);
				e->sip = &sm;
				if (e->stopping)
					sm.stop();
			}
			say("dmodem: the modem is ready on the SIP line%s\n", "");
			serve(sm.modem(), [&sm, &sc] { return sm.run(sc); });
			std::lock_guard<std::mutex> lk(e->mu);
			e->sip = nullptr;
			break;
		}
		case DMODEM_EMBED_USB: {
#ifdef DMODEM_USB
			const char *dev = cfg.usb.empty() ? "auto" :
				cfg.usb.c_str();
			dmd::UsbLine usb(dte);
			rate = dmd::UsbLine::line_rate(dev, rate, rate_given);
			mo.rate = static_cast<s32>(rate);
			dcore::Modem m(usb, mo);
			if (!m.ok() || !usb.open(dev, rate))
				break;
			serve(m, [&] {
				return run_line(m, usb, dte, e->wake[0],
						e->stopping);
			});
#else
			say("error: a USB line: not in this build (libusb)%s\n",
			    "");
#endif
			break;
		}
		case DMODEM_EMBED_ALSA: {
#ifdef DMODEM_ALSA
			dmd::AlsaLine alsa(dte);
			mo.rate = static_cast<s32>(rate);
			dcore::Modem m(alsa, mo);
			if (!m.ok() || !alsa.open(cfg.alsa.c_str(),
					cfg.alsa_ctl.empty() ? nullptr :
					cfg.alsa_ctl.c_str(), rate))
				break;
			serve(m, [&] {
				return run_line(m, alsa, dte, e->wake[0],
						e->stopping);
			});
#else
			say("error: an ALSA line: not in this build (alsa)%s\n",
			    "");
#endif
			break;
		}
		case DMODEM_EMBED_MISDN: {
#ifdef DMODEM_MISDN
			dmd::MisdnLine misdn(dte);
			dcore::Modem m(misdn, mo);
			if (!m.ok() || !misdn.open(cfg.misdn.c_str(),
					cfg.misdn_msn.empty() ? nullptr :
					cfg.misdn_msn.c_str(), cfg.misdn_flip))
				break;
			serve(m, [&] {
				return run_line(m, misdn, dte, e->wake[0],
						e->stopping);
			});
#else
			say("error: an ISDN line: not in this build (mISDN)%s\n",
			    "");
#endif
			break;
		}
		default:
			say("error: no such line%s\n", "");
			break;
		}
		/* (what the modem said last, to the program) */
		dte.flush();
	}
	prop_dp_exit();
	dcore::log_close();
	dmd::fd_close(dte_fd);
	{
		std::lock_guard<std::mutex> lk(hook_mu);
		if (hook_owner == e)
			hook_owner = nullptr;
	}
	dcore::log_set_hook(nullptr);
	{
		std::lock_guard<std::mutex> lk(e->mu);
		e->active = false;
	}
	if (e->exit_fn)
		e->exit_fn(e->exit_ctx, ret < 0 ? -1 : 0);
}

/* the wake sockets closed */
void close_wake(dmodem_embed *e)
{
	for (int &fd : e->wake) {
		if (fd >= 0)
			dmd::fd_close(fd);
		fd = -1;
	}
}

} // namespace

/* see dmodem_embed.h */
const char *dmodem_embed_version(void)
{
	return DMODEM_VERSION;
}

/* see dmodem_embed.h */
int dmodem_embed_lines(void)
{
	int lines = 1 << DMODEM_EMBED_SIP;
#ifdef DMODEM_USB
	lines |= 1 << DMODEM_EMBED_USB;
#endif
#ifdef DMODEM_ALSA
	lines |= 1 << DMODEM_EMBED_ALSA;
#endif
#ifdef DMODEM_MISDN
	lines |= 1 << DMODEM_EMBED_MISDN;
#endif
	return lines;
}

/* see dmodem_embed.h */
void dmodem_embed_config_init(dmodem_embed_config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->line = DMODEM_EMBED_SIP;
	cfg->sip_port = 5060;
	cfg->sip_t38 = 1;
	cfg->sip_v152 = 1;
	cfg->sip_clearmode = 1;
	cfg->sip_tcp = 1;
	cfg->sip_srtp = 1;
	cfg->sip_red = 2;
	cfg->sip_jitter_ms = 60;
	cfg->sip_log_level = -1;
	cfg->misdn_flip = 1;
}

/* see dmodem_embed.h */
dmodem_embed *dmodem_embed_new(void)
{
	return new (std::nothrow) dmodem_embed;
}

/* see dmodem_embed.h */
void dmodem_embed_free(dmodem_embed *e)
{
	if (!e)
		return;
	dmodem_embed_stop(e);
	delete e;
}

/* see dmodem_embed.h */
void dmodem_embed_on_log(dmodem_embed *e, dmodem_embed_log_fn fn, void *ctx)
{
	std::lock_guard<std::mutex> lk(hook_mu);
	e->log_fn = fn;
	e->log_ctx = ctx;
}

/* see dmodem_embed.h */
void dmodem_embed_on_exit(dmodem_embed *e, dmodem_embed_exit_fn fn,
			  void *ctx)
{
	std::lock_guard<std::mutex> lk(e->mu);
	e->exit_fn = fn;
	e->exit_ctx = ctx;
}

/* see dmodem_embed.h */
int dmodem_embed_start(dmodem_embed *e, const dmodem_embed_config *c)
{
	int sv[2];

	if (!e || !c)
		return -1;
	std::unique_lock<std::mutex> lk(e->mu);
	if (e->active) {
		e->error = "the modem runs already";
		return -1;
	}
	if (!(dmodem_embed_lines() & (1 << c->line))) {
		e->error = "this line is not in this build";
		return -1;
	}
	/* (the thread ended by itself) */
	if (e->thread.joinable()) {
		std::thread t = std::move(e->thread);
		lk.unlock();
		t.join();
		lk.lock();
	}
	{
		std::lock_guard<std::mutex> hk(hook_mu);
		if (hook_owner && hook_owner != e) {
			e->error = "another modem runs in this program";
			return -1;
		}
	}
	close_wake(e);

	EmbedConfig cfg;
	cfg.line = c->line;
	cfg.country = str(c->country);
	cfg.log_level = c->log_level;
	cfg.data_file = str(c->data_file);
	cfg.sip_user = str(c->sip_user);
	cfg.sip_password = str(c->sip_password);
	cfg.sip_domain = str(c->sip_domain);
	cfg.sip_port = c->sip_port;
	cfg.sip_listen = c->sip_listen != 0;
	cfg.sip_t38 = c->sip_t38 != 0;
	cfg.sip_v152 = c->sip_v152 != 0;
	cfg.sip_v150 = c->sip_v150 != 0;
	cfg.sip_clearmode = c->sip_clearmode != 0;
	cfg.sip_tcp = c->sip_tcp != 0;
	cfg.sip_srtp = c->sip_srtp < 0 ? 0 : c->sip_srtp > 2 ? 2 : c->sip_srtp;
	cfg.sip_red = c->sip_red < 0 ? 0 : c->sip_red > 3 ? 3 : c->sip_red;
	cfg.sip_jitter_ms = c->sip_jitter_ms < 20 ? 20 :
		c->sip_jitter_ms > 1000 ? 1000 : c->sip_jitter_ms;
	cfg.sip_log_level = c->sip_log_level;
	cfg.sip_dns = str(c->sip_dns);
	cfg.usb = str(c->usb);
	cfg.alsa = str(c->alsa);
	cfg.alsa_ctl = str(c->alsa_ctl);
	cfg.rate = c->rate;
	cfg.misdn = str(c->misdn);
	cfg.misdn_msn = str(c->misdn_msn);
	cfg.misdn_flip = c->misdn_flip != 0;
	if (cfg.rate && !dmd::line_rate_ok(static_cast<long>(cfg.rate))) {
		e->error = "the rate is not 8000, 9600, 48000, 96000 or 192000";
		return -1;
	}
	if (cfg.line == DMODEM_EMBED_ALSA && cfg.alsa.empty()) {
		e->error = "an ALSA line needs its PCM device";
		return -1;
	}
	if (cfg.line == DMODEM_EMBED_MISDN && cfg.misdn.empty()) {
		e->error = "an ISDN line needs its card";
		return -1;
	}

	if (dmd::socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		e->error = std::string("socketpair: ") + strerror(errno);
		return -1;
	}
	if (dmd::socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, e->wake) < 0) {
		e->error = std::string("socketpair: ") + strerror(errno);
		dmd::fd_close(sv[0]);
		dmd::fd_close(sv[1]);
		e->wake[0] = e->wake[1] = -1;
		return -1;
	}
	dmd::set_nonblocking(sv[1]);
	e->active = true;
	e->stopping = false;
	try {
		e->thread = std::thread(run, e, std::move(cfg), sv[0]);
	} catch (...) {
		e->active = false;
		e->error = "the thread could not start";
		dmd::fd_close(sv[0]);
		dmd::fd_close(sv[1]);
		close_wake(e);
		return -1;
	}
	e->error.clear();
	return sv[1];
}

/* see dmodem_embed.h */
void dmodem_embed_stop(dmodem_embed *e)
{
	if (!e)
		return;
	std::unique_lock<std::mutex> lk(e->mu);
	if (!e->thread.joinable())
		return;
	e->stopping = true;
	if (e->sip)
		e->sip->stop();
	if (e->wake[1] >= 0) {
		char b = 0;
		(void)dmd::fd_write(e->wake[1], &b, 1);
	}
	std::thread t = std::move(e->thread);
	lk.unlock();
	t.join();
	lk.lock();
	close_wake(e);
}

/* see dmodem_embed.h */
int dmodem_embed_running(dmodem_embed *e)
{
	if (!e)
		return 0;
	std::lock_guard<std::mutex> lk(e->mu);
	return e->active ? 1 : 0;
}

/* see dmodem_embed.h */
const char *dmodem_embed_error(dmodem_embed *e)
{
	if (!e)
		return "";
	std::lock_guard<std::mutex> lk(e->mu);
	return e->error.c_str();
}
