/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    main.cpp - dmodemd: the modem core on a SIP line (sip_modem.h),
 *               its DTE a pty
 *
 *    Started as root, it makes its pty and the link /dev/ttyDM<n> to it,
 *    opens its data file and leaves the link to a keeper process
 *    (pty.cpp) before it goes on as nobody.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <dcore.h>
#include <dsplibs.h>
#include <log.h>

#include "datafile.h"
#include "options.h"
#include "pty.h"
#include "run_loop.h"
#include "sip.h"
#include "sip_modem.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "dmodemd: " fmt, ##__VA_ARGS__)

namespace {

/* the line: 8000 samples a second */
const int RATE = 8000;

/* the user dmodemd goes on as, started as root, and the locked memory
   it then needs with realtime scheduling */
const char RUN_AS[] = "nobody";
const rlim_t MEMLOCK_MIN = 8UL << 20;

/* where the modems' data files are */
const char DATA_PREFIX[] = "/var/lib/dmodem/data.";

/* the pty's link: /dev/ttyDM<n>, n the number name ends with (0 if it
   ends with no digit) */
std::string link_path(const char *name)
{
	const char *end = name + strlen(name), *p = end;
	unsigned long n = 0;

	while (p > name && isdigit((unsigned char)p[-1]))
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
	struct sched_param sp;

	if (mlockall(MCL_CURRENT) < 0)
		ERR("cannot lock the memory: %s\n", strerror(errno));
	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		ERR("cannot have realtime scheduling: %s\n", strerror(errno));
	else
		DBG("realtime scheduling: SCHED_FIFO, priority %d\n",
		    sp.sched_priority);
}

/* root's rights given up for RUN_AS's, for good (exits if they can't
   be); with realtime scheduling the locked memory it needs must be
   allowed first */
void drop_privileges(bool realtime)
{
	struct passwd *pw;
	gid_t gid;
	uid_t uid;

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

} // namespace

/* the modem on a pty and a SIP line until SIGINT or SIGTERM (0), or an
   error (255) */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	dmd::Pty pty;
	dmd::PtyDte dte(pty);
	dcore::Options mo;
	sip::Config cfg;
	const char *name, *slash;
	int ret;

	dmd::parse_options(argc, argv, opt);
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
	std::unique_ptr<dmd::SipModem> sm(new dmd::SipModem(dte, mo));
	if (!sm->ok())
		exit(-1);

	if (pty.open(opt.group, opt.perm, link.c_str()) < 0) {
		ERR("cannot create PTY.\n");
		exit(-1);
	}
	fprintf(stderr, "modem `%s' created. TTY is `%s'\n", name, pty.name());

	/* opened as root: saved at the end as nobody */
	dmd::DataFile data((DATA_PREFIX + std::string(name)).c_str());
	data.load(&sm->modem().dsp_info());

	/* the keeper an ordinary process, forked before any thread */
	if (geteuid() == 0)
		pty.fork_keeper();
	if (opt.realtime)
		go_realtime();
	dmd::RunLoop::catch_signals();
	signal(SIGPIPE, SIG_IGN);
	if (geteuid() == 0)
		drop_privileges(opt.realtime);
	fprintf(stderr, "Use `%s' as modem device, Ctrl+C for termination.\n",
		*pty.link() ? pty.link() : pty.name());

	if (!sip::config_from_env(cfg, opt.listen))
		exit(-1);
	ret = sm->run(cfg);

	data.save(&sm->modem().dsp_info());
	sm.reset();
	/* what the modem said last still there for the DTE to read */
	usleep(100000);
	pty.close();
	prop_dp_exit();
	dcore::log_close();
	return ret < 0 ? 255 : 0;
}
