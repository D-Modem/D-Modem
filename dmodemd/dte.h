/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dte.h - the modem's DTE as dmodemd's lines see it: a pty
 *            (sip_modem.h's PtyDte) on POSIX, a TCP connection, a COM
 *            port, a named pipe or the virtual modem driver's port on
 *            Windows (windows_dte.h)
 *
 */
#ifndef DMODEMD_DTE_H
#define DMODEMD_DTE_H

#include <dcore.h>

namespace dmd {

/* the modem's DTE: a descriptor its characters come on, the modem's
   go to */
class Dte {
public:
	virtual ~Dte() = default;
	/* the descriptor, -1 if none */
	virtual int fd() const = 0;
	/* select() on it for reading now? */
	virtual bool want_read(const dcore::Modem &m) = 0;
	/* select() on it for writing now (output waits)? */
	virtual bool want_write() const { return false; }
	/* it is readable: its characters into m.  0; 1 when the DTE has
	   gone for good (the modem stops); -1 on an error */
	virtual int serve(dcore::Modem &m) = 0;
	/* it is writable: what waits */
	virtual void flush() {}
	/* the modem's characters: how many it took */
	virtual int write(const char *buf, int n) = 0;
};

} // namespace dmd

#endif
