/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    main_windows.cpp - dmodemd on Windows: the modem core on a SIP line
 *                       (sip_modem.h), its DTE a COM port (--com) or TCP
 *                       connections (--tcp, the default 127.0.0.1:2323);
 *                       windows_dte.h has both
 *
 *    What main.cpp does for POSIX's ptys, root and realtime scheduling
 *    has no place here: the process runs as the user who starts it, at
 *    high priority with the system's timer at 1 ms (the call's 20 ms
 *    frames are paced by sleeps), and keeps the line's data in
 *    %LOCALAPPDATA%\dmodem.
 *
 */

#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>

#include <dcore.h>
#include <dsplibs.h>
#include <log.h>

#include "datafile.h"
#include "options.h"
#include "portable.h"
#include "run_loop.h"
#include "sip.h"
#include "sip_modem.h"
#include "windows_dte.h"

#include <mmsystem.h>

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)

namespace {

/* the line: 8000 samples a second */
const int RATE = 8000;

/* where the modems' data files are: %LOCALAPPDATA%\dmodem (made if it
   isn't there), else the current directory */
std::string data_path(const char *name)
{
	const char *base = getenv("LOCALAPPDATA");
	if (!base || !*base)
		return std::string("dmodem-data.") + name;
	std::string dir = std::string(base) + "\\dmodem";
	_mkdir(dir.c_str());
	return dir + "\\data." + name;
}

} // namespace

/* the modem on a COM port or TCP and a SIP line until Ctrl+C or the
   console closing (0), or an error (255) */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	dcore::Options mo;
	sip::Config cfg;
	const char *name, *slash;
	std::unique_ptr<dmd::Dte> dte;
	std::string where;
	int ret;

	/* (Windows' C library buffers a pipe whole, line buffering or
	   not: the log as it comes, for whatever reads it) */
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	dmd::parse_options(argc, argv, opt);
	slash = strrchr(opt.name, '\\');
	if (!slash)
		slash = strrchr(opt.name, '/');
	name = slash ? slash + 1 : opt.name;
	if (opt.alsa) {
		ERR("-a: ALSA is Linux's\n");
		return 255;
	}
	if (opt.com && opt.tcp) {
		ERR("--com or --tcp, not both\n");
		return 255;
	}
	dcore::log_open(name);
	dmd::print_version();
	prop_dp_init();
	if (!dmd::net_init()) {
		ERR("Winsock can't be started\n");
		return 255;
	}

	/* the DTE */
	if (opt.com) {
		std::unique_ptr<dmd::ComDte> c(new dmd::ComDte);
		if (!c->open(opt.com))
			return 255;
		where = c->where();
		dte = std::move(c);
	} else {
		std::unique_ptr<dmd::TcpDte> t(new dmd::TcpDte);
		if (!t->open(opt.tcp ? opt.tcp : "127.0.0.1:2323"))
			return 255;
		where = "TCP " + t->where();
		dte = std::move(t);
	}

	mo.name = name;
	mo.dev_name = opt.name;
	mo.rate = RATE;
	mo.country = opt.country;
	dmd::SipModem sm(*dte, mo);
	if (!sm.ok())
		return 255;
	fprintf(stderr, "modem `%s' created. DTE is %s\n", name,
		where.c_str());

	dmd::DataFile data(data_path(name).c_str());
	data.load(&sm.modem().dsp_info());

	/* the 20 ms frames on time: the system's timer at 1 ms, and the
	   process ahead of the others (-n: not) */
	timeBeginPeriod(1);
	if (opt.realtime && !SetPriorityClass(GetCurrentProcess(),
					      HIGH_PRIORITY_CLASS))
		ERR("cannot raise the priority (error %lu)\n",
		    (unsigned long)GetLastError());
	dmd::RunLoop::catch_signals();
	fprintf(stderr, "Use %s as the modem, Ctrl+C for termination.\n",
		where.c_str());

	if (!sip::config_from_env(cfg, opt.listen))
		return 255;
	ret = sm.run(cfg);

	data.save(&sm.modem().dsp_info());
	/* what the modem said last still there for the DTE to read */
	Sleep(100);
	timeEndPeriod(1);
	prop_dp_exit();
	dcore::log_close();
	return ret < 0 ? 255 : 0;
}
