/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    sip_modem.h - the modem core on a SIP line (sip.h), its DTE a
 *                  descriptor: dmodemd's, on its pty, and a program's
 *                  that has the modem in a thread of its own, on a
 *                  socket (an Android app)
 *
 *    Each call's audio comes from the SIP user agent on a socket pair,
 *    20 ms frames that pace the modem; a fax call that goes over to
 *    T.38 has the modem's fax engine on the call's UDPTL instead, on our
 *    own clock.
 *
 */

#ifndef DMODEMD_SIP_MODEM_H
#define DMODEMD_SIP_MODEM_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <dcore.h>

#include "dm_types.h"
#include "dte.h"
#include "sip.h"

namespace dmd {

class Pty;

#ifndef _WIN32
/* a pty (pty.h) as the DTE: its DTE closing it with HUPCL hangs up,
   and a new pty stands in for it */
class PtyDte final : public Dte {
public:
	/* pty, which stays the caller's */
	explicit PtyDte(Pty &pty) : pty_(pty) {}
	int fd() const override;
	bool want_read(const dcore::Modem &m) override;
	s16 serve(dcore::Modem &m) override;
	s32 write(const char *buf, s32 n) override;
private:
	Pty &pty_;
};
#endif

/* any stream descriptor as the DTE (one end of a socket pair, say),
   made non-blocking; the other end closed ends the call and the modem.
   What the descriptor can't take now waits (up to BUFFER octets) */
class FdDte final : public Dte {
public:
	enum : s32 { BUFFER = INT32_C(64) * 1024 };
	/* fd stays the caller's */
	explicit FdDte(int fd);
	/* see Dte */
	int fd() const override { return fd_; }
	bool want_read(const dcore::Modem &m) override;
	/* output waits */
	bool want_write() const override { return !out_.empty(); }
	s16 serve(dcore::Modem &m) override;
	void flush() override;
	s32 write(const char *buf, s32 n) override;
private:
	int fd_;
	std::string out_;
};

class SipLine;

/* the modem and its line */
class SipModem {
public:
	/* what a turn of run() saw of the call (its state, off hook, the
	   rates), each time it changes, on run()'s thread */
	struct Status {
		dcore::LinkState link = dcore::LinkState::Idle;
		bool offhook = false;
		u32 tx_rate = 0, rx_rate = 0;
		/* the same state, hook and rates */
		bool operator==(const Status &o) const {
			return link == o.link && offhook == o.offhook &&
				tx_rate == o.tx_rate && rx_rate == o.rx_rate;
		}
		/* something has changed */
		bool operator!=(const Status &o) const { return !(*this == o); }
	};

	/* the core as opt has it (opt.rate is the line's, 8000), on dte
	   (the caller's, used until run() returns) */
	SipModem(Dte &dte, const dcore::Options &opt);
	~SipModem();
	SipModem(const SipModem &) = delete;
	SipModem &operator=(const SipModem &) = delete;

	/* the core could be set up */
	bool ok() const;
	/* the core (its data file, before run()) */
	dcore::Modem &modem() { return *m_; }

	/* the status changes to fn, from the next turn of run() */
	void on_status(std::function<void(const Status &)> fn) {
		status_fn_ = std::move(fn);
	}

	/* the modem's speaker, as ATM and ATL have it (on while it dials
	   and trains with ATM1): volume(level) as it changes (0 off, 1..3
	   ATL's), and while it is on audio(rx, tx, n), each block of the
	   call's samples as it passes: what came, and the modem's answer.
	   On run()'s thread, before run() */
	struct Speaker {
		std::function<void(s16 level)> volume;
		std::function<void(const s16 *rx, const s16 *tx,
				   s16 n)> audio;
	};
	void on_speaker(Speaker s);

	/* the SIP user agent started with cfg, then the modem served until
	   stop() or SIGINT/SIGTERM (RunLoop::catch_signals()), or the DTE
	   has gone: 0; -1 on an error (the user agent failing to start, or
	   going).  The user agent stopped again when it returns */
	s16 run(const sip::Config &cfg);
	/* run() returns at its next turn: from any thread, or a signal
	   handler; before run() it returns at once */
	void stop();

private:
	s16 serve(int ctl);
	void report();

	Dte &dte_;
	std::unique_ptr<SipLine> line_;
	std::unique_ptr<dcore::Modem> m_;
	/* stop(): a byte into [1], run() watches [0] */
	int wake_[2] = { -1, -1 };
	std::atomic<bool> stopped_{false};
	std::function<void(const Status &)> status_fn_;
	Status status_;
};

} // namespace dmd

#endif
