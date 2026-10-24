/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    pipe_modem.h - a modem whose line's audio comes from one file
 *                   descriptor and goes to another (16 bit linear), with
 *                   a pty for its DTE: modem_test puts two of them back
 *                   to back, the interop tests' half_open one against a
 *                   modem in another process
 *
 *    What slmodemd's modem_test.c and modem_half.c did, on the new core.
 *
 */
#ifndef DMODEMD_PIPE_MODEM_H
#define DMODEMD_PIPE_MODEM_H

#include <dcore.h>
#include <dsp.h>

#include "line_frames.h"
#include "line_recorder.h"
#include "pty.h"
#include "run_loop.h"

namespace dmd {

class PipeModem final : public dcore::Host {
public:
	/* name: the modem's; in, out: its line */
	PipeModem(const char *name, int rate, const char *country, int in,
		  int out);
	/* the core and the recordings (in and out stay the caller's) */
	~PipeModem();
	/* the core */
	dcore::Modem &modem() { return *m_; }
	/* the DTE's pty */
	Pty &pty() { return pty_; }

	/* MODEM_REALTIME: no faster than the sample clock (with PIPE_SYNC
	   it is so on hook anyway); MODEM_SPEED=<x>: else no faster than x
	   times the sample clock (0: as fast as it goes); MODEM_REC: the
	   line both ways into <prefix>.in and <prefix>.out */
	void set_realtime(bool on) { realtime_ = on; }
	/* (MODEM_SPEED) */
	void set_speed(double x) { speed_ = x; }
	/* (MODEM_REC) */
	void set_record(const char *prefix) { record_ = prefix; }

	/* the main loop of any number of them, until stop() */
	static int run(PipeModem **modems, int n);
	/* run() returns at its next turn (safe in a signal handler) */
	static void stop();

	/* Host */
	int start(dcore::Modem &m) override;
	int stop(dcore::Modem &m) override;
	/* the line's delay, which Smart Link's original library asks for:
	   the lead silence and the changes it asked for in this call; a
	   change, made in line() */
	int io_delay(dcore::Modem &) override { return delay_; }
	/* a change of samples in the delay, made in line() */
	void update_delay(dcore::Modem &, int samples) override {
		pending_delay_ += samples;
	}
	/* MODEM_ANALOG=1 (or =<its name>): an analogue line (the tests'
	   8000 Hz analogue modem: netsim's NET_ANALOG); it dials there
	   itself */
	bool analog_line(dcore::Modem &) override { return analog_; }
	/* MODEM_HOOK_FD=<fd>: each change of the hook written to it, '1'
	   off hook and '0' on (netsim's NET_HOOK_A and NET_HOOK_B, for its
	   NET_RING) */
	int set_hook(dcore::Modem &m, int state) override;
	/* for ATI3 */
	const char *driver_name() override { return "modem_test driver"; }
	/* to the pty */
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		return pty_.write(buf, n);
	}
	/* ms: the line's clock with PIPE_SYNC, else the monotonic one */
	unsigned long now_ms() override;

private:
	int line(bool rd = true);       /* the line's audio is readable (rd:
					   read it; else what is left) */
	/* with realtime_, sleep until the clock has these samples' time */
	void pace(int samples);
	int lead();                     /* the line's lead silence, once */

	dcore::Modem *m_ = nullptr;
	Pty pty_;
	int in_, out_;
	int rate_;
	bool lead_done_ = false;        /* the silence before the first call */
	int lead_ = 0;                  /* its samples */
	int delay_ = 0;                 /* io_delay() */
	int pending_delay_ = 0;         /* the change not made yet */
	LineFrames line_;               /* in_'s samples */
	bool realtime_ = false;
	bool analog_ = false;
	int hook_fd_ = -1;              /* MODEM_HOOK_FD */
	char hook_ = 0;                 /* the last written there */
	/* on hook: the line's ring signal (netsim's NET_RING), a pulse
	   each cycle to the core's ring() */
	dsp::RingDetect ring_;
	double speed_ = 0;
	/* PIPE_SYNC=<samples>: the DTE's input only every that many samples
	   of the line, the timers on the line's clock and the line no
	   faster than real time on hook, so that a test's commands meet
	   the line at the same samples every time */
	long sync_ = 0;
	/* of the line, so far */
	long samples() const { return static_cast<long>(line_.taken()); }
	long next_sync_ = 0;
	double line_at_ = 0;    /* when the line last had samples (s) */
	long clock_off_ = 0;    /* now_ms() - the line's ms while idle */
	unsigned long last_ms_ = 0;
	/* PIPE_SYNC: nothing on the line for 0.5 s */
	bool line_idle() const;
	/* PIPE_SYNC: the DTE's input at a sync point */
	void dte_input();
	const char *record_ = nullptr;
	LineRecorder rec_;
	bool rec_tried_ = false;        /* rec_ opened (or not) */
	double clock_start_ = 0, clock_done_ = 0;
	double clock_speed_ = 0;        /* what pace() last went at */
};

} // namespace dmd

#endif
