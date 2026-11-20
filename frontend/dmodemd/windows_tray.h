/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    windows_tray.h - dmodemd's icon in the Windows notification area
 *                     (the system tray), and its log window
 *
 *    Tray::start() puts the icon in the tray, with the tip
 *    "D-Modem <version>".  A double-click on the icon opens the log
 *    window: what dmodemd writes to stdout and stderr (its log among
 *    it), the newest at the bottom.  A right-click opens a menu:
 *
 *        View Logs   the log window
 *        About       the version, the modem and its DTE
 *        Exit        dmodemd stops (RunLoop::stop(), as Ctrl+C)
 *
 *    The output still goes where it went before (the console, a file
 *    or a pipe): the tray reads it through a pipe of its own and
 *    writes it on.  The icon and its windows have a thread of their
 *    own.  The calls are Windows 95's and NT 4.0's (the shell of
 *    Internet Explorer 4 for the taskbar's restart), so the same
 *    dmodemd.exe runs on Windows 98 SE and on Windows 11.
 *
 */
#ifndef DMODEMD_WINDOWS_TRAY_H
#define DMODEMD_WINDOWS_TRAY_H

namespace dmd {

class Tray {
public:
	/* stdout and stderr through the log, and the icon in the tray;
	   name is the modem's.  false: no icon (no shell, no window), and
	   the output as it was */
	static bool start(const char *name);
	/* what the About box says of the DTE ("TCP 127.0.0.1:2323") */
	static void set_dte(const char *where);
	/* the icon and its windows gone, stdout and stderr as they were
	   (what is in the pipe written on first) */
	static void stop();
};

} // namespace dmd

#endif
