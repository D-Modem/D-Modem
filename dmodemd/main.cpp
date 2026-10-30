/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    main.cpp - dmodemd: the modem core on a SIP line (sip_modem.h),
 *               its DTE a pty
 *
 *    With -a the line is a softmodem's codec through ALSA instead
 *    (alsa_line.cpp), with --usb a Smart Link SmartUSB56 or a
 *    pico-serial Line stick through libusb (usb_line.cpp): a telephone
 *    line, its samples pacing the modem.  With --misdn it is an ISDN
 *    basic rate interface through Linux's mISDN (misdn_line.cpp), its
 *    B-channel's octets pacing the modem.
 *
 *    Started as root, it makes its pty and the link /dev/ttyDM<n> to it,
 *    opens its data file and leaves the link to a keeper process
 *    (pty.cpp) before it goes on as nobody.
 *
 */

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __HAIKU__
#include <OS.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif
#include <dcore.h>
#include <dsplibs.h>
#include <log.h>

#ifdef DMODEM_ALSA
#include "dm_types.h"
#include "alsa_line.h"
#endif
#ifdef DMODEM_USB
#include "usb_line.h"
#endif
#ifdef DMODEM_MISDN
#include "misdn_line.h"
#endif
#include "datafile.h"
#include "options.h"
#include "pty.h"
#include "run_loop.h"
#include "sip.h"
#include "sip_modem.h"
#include "reinit.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "dmodemd: " fmt, ##__VA_ARGS__)

namespace {

/* the line: 8000 samples a second */
const s32 RATE = 8000;

/* the user dmodemd goes on as, started as root, and the locked memory
   it then needs with realtime scheduling */
const char RUN_AS[] = "nobody";
/* (where realtime scheduling locks the memory too: neither Haiku nor
   macOS has mlockall()) */
#if !defined(__HAIKU__) && !defined(__APPLE__)
#define DMODEM_LOCKS_MEMORY 1
const rlim_t MEMLOCK_MIN = 8UL << 20;
#endif

/* where the modems' data files are */
const char DATA_PREFIX[] = "/var/lib/dmodem/data.";

/* the pty's link: /dev/ttyDM<n>, n the number name ends with (0 if it
   ends with no digit) */
std::string link_path(const char *name)
{
	const char *end = name + strlen(name), *p = end;
	unsigned long n = 0;

	while (p > name && isdigit(static_cast<unsigned char>(p[-1])))
		p--;
	if (p < end)
		n = strtoul(p, nullptr, 10);
	return "/dev/ttyDM" + std::to_string(n);
}

/* all the memory we have now locked, and SCHED_FIFO at its highest
   priority (the threads made later have it too); what fails is only
   reported */
void go_realtime()
{
#ifdef __HAIKU__
	/* (Haiku: no mlockall() or SCHED_FIFO, but real-time priorities,
	   which run until they block, as SCHED_FIFO does: this thread's,
	   the line's loop; the threads made later have their own) */
	status_t r = set_thread_priority(find_thread(nullptr),
					 B_REAL_TIME_DISPLAY_PRIORITY);
	if (r < B_OK)
		ERR("cannot have a real-time priority: %s\n", strerror(r));
	else
		DBG("real-time priority %d\n", B_REAL_TIME_DISPLAY_PRIORITY);
#elif defined(__APPLE__)
	/* (macOS: no SCHED_FIFO for a process, nor mlockall(); Mach's
	   time constraint policy, as audio threads have it: this thread's,
	   the line's loop, every 10 ms up to 5 ms of it; the threads made
	   later have their own) */
	mach_timebase_info_data_t tb;
	thread_time_constraint_policy_data_t p;

	mach_timebase_info(&tb);
	const double per_ms = 1e6 * tb.denom / tb.numer;
	p.period = static_cast<uint32_t>(10 * per_ms);
	p.computation = static_cast<uint32_t>(5 * per_ms);
	p.constraint = static_cast<uint32_t>(10 * per_ms);
	p.preemptible = TRUE;
	kern_return_t r = thread_policy_set(pthread_mach_thread_np(pthread_self()),
		THREAD_TIME_CONSTRAINT_POLICY,
		reinterpret_cast<thread_policy_t>(&p),
		THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (r != KERN_SUCCESS)
		ERR("cannot have realtime scheduling: %s\n",
		    mach_error_string(r));
	else
		DBG("realtime scheduling: Mach's time constraint policy\n");
#else
	struct sched_param sp;

	if (mlockall(MCL_CURRENT) < 0)
		ERR("cannot lock the memory: %s\n", strerror(errno));
	dsp::zero(sp);
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		ERR("cannot have realtime scheduling: %s\n", strerror(errno));
	else
		DBG("realtime scheduling: SCHED_FIFO, priority %d\n",
		    sp.sched_priority);
#endif
}

/* started with root's rights, to be given up after start-up (Haiku:
   every process has uid 0, and there are no other users to run as) */
bool started_as_root()
{
#ifdef __HAIKU__
	return false;
#else
	return geteuid() == 0;
#endif
}

/* root's rights given up for RUN_AS's, for good (exits if they can't
   be); with realtime scheduling the locked memory it needs must be
   allowed first */
void drop_privileges(bool realtime)
{
	struct passwd *pw;
	gid_t gid;
	uid_t uid;

#ifdef DMODEM_LOCKS_MEMORY
	if (realtime) {
		struct rlimit rl;
		if (getrlimit(RLIMIT_MEMLOCK, &rl) < 0) {
			ERR("getrlimit: %s\n", strerror(errno));
			exit(-1);
		}
		if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < MEMLOCK_MIN) {
			ERR("the locked memory limit is too low for "
			    "realtime scheduling\n");
			ERR("%lu octets needed, %lu allowed: ulimit -l %lu "
			    "(or -n)\n", (unsigned long)MEMLOCK_MIN,
			    (unsigned long)rl.rlim_cur,
			    (unsigned long)(MEMLOCK_MIN >> 10));
			exit(-1);
		}
	}
#else
	(void)realtime;
#endif
	pw = getpwnam(RUN_AS);
	if (!pw) {
		ERR("cannot find the user '%s'\n", RUN_AS);
		exit(-1);
	}
	gid = pw->pw_gid;
	uid = pw->pw_uid;
	if (setgroups(1, &gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0) {
		ERR("cannot go on as gid %u, uid %u: %s\n", (unsigned)gid,
		    (unsigned)uid, strerror(errno));
		exit(-1);
	}
	if (setuid(0) == 0) {
		ERR("root's rights are still there after setuid(%u)\n",
		    (unsigned)uid);
		exit(-1);
	}
	DBG("running as gid %u, uid %u\n", (unsigned)gid, (unsigned)uid);
}

#if defined(DMODEM_ALSA) || defined(DMODEM_USB) || defined(DMODEM_MISDN)
/* the loop on a line (ALSA's, the SmartUSB56's, mISDN's), until
   SIGINT or SIGTERM (0) or an error (-1), each turn: the modem's timers,
   then select() on the line's descriptors and the pty (if the modem
   takes its characters now), then the samples that came */
template <class Line>
s16 run_line(dcore::Modem &m, Line &line, dmd::Pty &pty)
{
	dcore::Modem *const modems[] = { &m };
	dmd::RunLoop loop;

	while (dmd::RunLoop::running()) {
		s32 wait = dmd::RunLoop::poll(modems, 1);
		int ret;

		loop.clear();
		line.watch(loop);
		if (pty.want_read(m))
			loop.watch(pty.fd());
		ret = loop.wait(wait);
		if (ret < 0) {
			ERR("select: %s\n", strerror(errno));
			return -1;
		}
		if (line.pump(m) < 0)
			return -1;
		if (ret > 0 && loop.ready(pty.fd()) && pty.serve(m, true) < 0)
			return -1;
	}
	return 0;
}
#endif

} // namespace

/* the modem on a pty and a SIP line (or with -a an ALSA one) until
   SIGINT or SIGTERM (0), or an error (255) */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	dmd::Pty pty;
	dmd::PtyDte dte(pty);
	dcore::Options mo;
	sip::Config cfg;
	const char *name, *slash;
	s16 ret;
#ifdef DMODEM_ALSA
	dmd::AlsaLine alsa(pty);
	std::unique_ptr<dcore::Modem> am;
#endif
#ifdef DMODEM_USB
	dmd::UsbLine usb(dte);
	std::unique_ptr<dcore::Modem> um;
#endif
#ifdef DMODEM_MISDN
	dmd::MisdnLine misdn(pty);
	std::unique_ptr<dcore::Modem> mm;
#endif

	dmd::parse_options(argc, argv, opt);
#ifdef DMODEM_USB
	/* --usb list: the devices, and no modem */
	if (opt.usb && !strcmp(opt.usb, "list"))
		return dmd::UsbLine::list() < 0 ? 255 : 0;
#endif
	slash = strrchr(opt.name, '/');
	name = slash ? slash + 1 : opt.name;
	dcore::log_open(name);
	dmd::print_version();
	prop_dp_init();
	std::string link = link_path(opt.name);

	mo.name = name;
	mo.dev_name = opt.name;
	mo.rate = RATE;
	mo.country = opt.country;
	/* the modem: on the ALSA line (the core on it alone), or on SIP */
	std::unique_ptr<dmd::SipModem> sm;
	dcore::Modem *m;
	if (opt.usb) {
#ifdef DMODEM_USB
		/* (an Agere modem's rate is its own, and a WB stick's unless
		   -r says) */
		opt.rate = dmd::UsbLine::line_rate(opt.usb, opt.rate,
						      opt.rate_given);
		mo.rate = static_cast<s32>(opt.rate);
		um.reset(new dcore::Modem(usb, mo));
		if (!um->ok())
			exit(-1);
		m = um.get();
#else
		ERR("--usb: dmodemd was built without libusb (libusb-1.0-0-dev)\n");
		exit(-1);
#endif
	} else if (opt.misdn) {
#ifdef DMODEM_MISDN
		mo.rate = RATE;
		mm.reset(new dcore::Modem(misdn, mo));
		if (!mm->ok())
			exit(-1);
		m = mm.get();
#else
		ERR("--misdn: dmodemd was built without mISDN (Linux's)\n");
		exit(-1);
#endif
	} else if (opt.alsa) {
#ifdef DMODEM_ALSA
		mo.rate = static_cast<s32>(opt.rate);
		am.reset(new dcore::Modem(alsa, mo));
		if (!am->ok())
			exit(-1);
		m = am.get();
#else
		ERR("-a: dmodemd was built without ALSA (libasound2-dev)\n");
		exit(-1);
#endif
	} else {
		sm.reset(new dmd::SipModem(dte, mo));
		if (!sm->ok())
			exit(-1);
		m = &sm->modem();
	}

	if (pty.open(opt.group, opt.perm, link.c_str()) < 0) {
		ERR("cannot create PTY.\n");
		exit(-1);
	}
	fprintf(stderr, "modem `%s' created. TTY is `%s'\n", name, pty.name());

	/* opened as root: saved at the end as nobody */
	dmd::DataFile data((DATA_PREFIX + std::string(name)).c_str());
	data.load(&m->dsp_info());

#ifdef DMODEM_ALSA
	/* the codec opened as root (its device's group may be audio) */
	if (opt.alsa && !alsa.open(opt.alsa, opt.alsa_ctl, opt.rate))
		exit(-1);
#endif
#ifdef DMODEM_USB
	/* the device opened as root (its node's is root's alone) */
	usb.set_leds(opt.usb_leds);
	usb.set_wake(opt.usb_wake);
	usb.set_handset(opt.usb_handset);
	usb.set_international(opt.usb_international);
	usb.set_rhom(opt.usb_rhom);
	usb.set_buzzer(opt.usb_buzzer);
	usb.set_wb_gains(opt.usb_wb_tx_gain, opt.usb_wb_rx_gain);
	if (opt.usb_voice_codec)
		usb.set_voice_codec(!strcmp(opt.usb_voice_codec, "alaw") ?
				    dmd::UsbLine::VoiceLaw::Alaw :
				    !strcmp(opt.usb_voice_codec, "linear") ?
				    dmd::UsbLine::VoiceLaw::Linear :
				    dmd::UsbLine::VoiceLaw::Ulaw);
	if (opt.usb && !usb.open(opt.usb, opt.rate))
		exit(-1);
#endif
#ifdef DMODEM_MISDN
	/* the card's D-channel opened as root (the device list's socket
	   needs CAP_NET_RAW) */
	if (opt.misdn && !misdn.open(opt.misdn, opt.misdn_msn,
				     opt.misdn_flip))
		exit(-1);
#endif
	/* the keeper an ordinary process, forked before any thread */
	if (started_as_root())
		pty.fork_keeper();
	if (opt.realtime)
		go_realtime();
	dmd::RunLoop::catch_signals();
	signal(SIGPIPE, SIG_IGN);
	if (started_as_root())
		drop_privileges(opt.realtime);
	fprintf(stderr, "Use `%s' as modem device, Ctrl+C for termination.\n",
		*pty.link() ? pty.link() : pty.name());

#ifdef DMODEM_ALSA
	if (am) {
		ret = run_line(*am, alsa, pty);
	} else
#endif
#ifdef DMODEM_USB
	if (um) {
		ret = run_line(*um, usb, pty);
	} else
#endif
#ifdef DMODEM_MISDN
	if (mm) {
		ret = run_line(*mm, misdn, pty);
	} else
#endif
	{
		if (!sip::config_from_env(cfg, opt.listen))
			exit(-1);
		ret = sm->run(cfg);
	}

	data.save(&m->dsp_info());
	sm.reset();
#ifdef DMODEM_ALSA
	am.reset();
#endif
#ifdef DMODEM_USB
	um.reset();
#endif
#ifdef DMODEM_MISDN
	mm.reset();
#endif
	/* what the modem said last still there for the DTE to read */
	usleep(100000);
	pty.close();
	prop_dp_exit();
	dcore::log_close();
	return ret < 0 ? 255 : 0;
}
