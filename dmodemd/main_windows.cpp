/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    main_windows.cpp - dmodemd on Windows: the modem core on a SIP line
 *                       (sip_modem.h), its DTE a COM port (--com), a
 *                       named pipe (--pipe, --pipe-connect), D-Modem's
 *                       virtual modem driver's COM port (--driver) or TCP
 *                       connections (--tcp, the default 127.0.0.1:2323);
 *                       windows_dte.h has them
 *
 *    With --usb the line is a Smart Link SmartUSB56 or a pico-serial Line
 *    stick through libusb (usb_line.cpp, on WinUSB), not SIP; the DTE is
 *    any of the above.
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
#ifdef DMODEM_USB
#include "usb_line.h"
#endif
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

#ifdef DMODEM_USB
/* the modem on a USB line until Ctrl+C (0), the DTE gone for good (0)
   or an error (-1), each turn: the modem's timers, select() on the DTE
   (for its characters if the modem takes them now, for its output that
   waits) for UsbLine::POLL_MS at most (Windows' libusb has no
   descriptors to watch), then the line's samples */
int run_usb(dcore::Modem &m, dmd::UsbLine &line, dmd::Dte &dte)
{
	dcore::Modem *const modems[] = { &m };
	dmd::RunLoop loop;

	while (dmd::RunLoop::running()) {
		long wait = dmd::RunLoop::poll(modems, 1);
		int fd = dte.fd(), ret;

		if (wait < 0 || wait > dmd::UsbLine::POLL_MS)
			wait = dmd::UsbLine::POLL_MS;
		loop.clear();
		if (fd >= 0 && dte.want_read(m))
			loop.watch(fd);
		if (fd >= 0 && dte.want_write())
			loop.watch_write(fd);
		ret = loop.wait(wait);
		if (ret < 0) {
			ERR("select: error %d\n", WSAGetLastError());
			return -1;
		}
		if (line.pump(m) < 0)
			return -1;
		if (ret == 0)
			continue;
		if (loop.ready_write(fd))
			dte.flush();
		if (loop.ready(fd)) {
			ret = dte.serve(m);
			if (ret < 0)
				return -1;
			if (ret > 0)
				return 0;
		}
	}
	return 0;
}
#endif

} // namespace

/* the modem on a COM port or TCP and a SIP line (or with --usb a USB
   modem's) until Ctrl+C or the console closing (0), or an error (255) */
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
#ifdef DMODEM_USB
	/* --usb list: the devices, and no modem */
	if (opt.usb && !strcmp(opt.usb, "list"))
		return dmd::UsbLine::list() < 0 ? 255 : 0;
#endif
	slash = strrchr(opt.name, '\\');
	if (!slash)
		slash = strrchr(opt.name, '/');
	name = slash ? slash + 1 : opt.name;
	if (opt.alsa) {
		ERR("-a: ALSA is Linux's\n");
		return 255;
	}
	if (!!opt.com + !!opt.pipe + !!opt.pipe_connect + !!opt.tcp +
	    !!opt.driver > 1) {
		ERR("one DTE: --com, --pipe, --pipe-connect, --driver or "
		    "--tcp\n");
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
	} else if (opt.driver) {
		std::unique_ptr<dmd::DriverDte> d(new dmd::DriverDte);
		if (!d->open(opt.driver))
			return 255;
		where = "the virtual modem driver's port " + d->where();
		dte = std::move(d);
	} else if (opt.pipe || opt.pipe_connect) {
		std::unique_ptr<dmd::PipeDte> pp(new dmd::PipeDte);
		if (!pp->open(opt.pipe ? opt.pipe : opt.pipe_connect,
			      !opt.pipe))
			return 255;
		where = "pipe " + pp->where();
		dte = std::move(pp);
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
	/* the modem: on the USB line (the core on it alone), or on SIP */
	std::unique_ptr<dmd::SipModem> sm;
	dcore::Modem *m;
#ifdef DMODEM_USB
	dmd::UsbLine usb(*dte);
	std::unique_ptr<dcore::Modem> um;
#endif
	if (opt.usb) {
#ifdef DMODEM_USB
		mo.rate = (int)opt.rate;
		um.reset(new dcore::Modem(usb, mo));
		if (!um->ok())
			return 255;
		m = um.get();
#else
		ERR("--usb: dmodemd.exe was built without libusb "
		    "(DMODEM_USB)\n");
		return 255;
#endif
	} else {
		sm.reset(new dmd::SipModem(*dte, mo));
		if (!sm->ok())
			return 255;
		m = &sm->modem();
	}
	fprintf(stderr, "modem `%s' created. DTE is %s\n", name,
		where.c_str());

	dmd::DataFile data(data_path(name).c_str());
	data.load(&m->dsp_info());
#ifdef DMODEM_USB
	usb.set_leds(opt.usb_leds);
	usb.set_wake(opt.usb_wake);
	usb.set_handset(opt.usb_handset);
	usb.set_international(opt.usb_international);
	if (opt.usb_voice_codec)
		usb.set_voice_codec(!strcmp(opt.usb_voice_codec, "alaw") ?
				    dmd::UsbLine::VoiceLaw::Alaw :
				    !strcmp(opt.usb_voice_codec, "linear") ?
				    dmd::UsbLine::VoiceLaw::Linear :
				    dmd::UsbLine::VoiceLaw::Ulaw);
	if (opt.usb && !usb.open(opt.usb, opt.rate))
		return 255;
#endif

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

#ifdef DMODEM_USB
	if (um) {
		ret = run_usb(*um, usb, *dte);
	} else
#endif
	{
		if (!sip::config_from_env(cfg, opt.listen))
			return 255;
		ret = sm->run(cfg);
	}

	data.save(&m->dsp_info());
	sm.reset();
#ifdef DMODEM_USB
	um.reset();
#endif
	/* what the modem said last still there for the DTE to read */
	Sleep(100);
	timeEndPeriod(1);
	prop_dp_exit();
	dcore::log_close();
	return ret < 0 ? 255 : 0;
}
