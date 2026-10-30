/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pipe_modem.cpp - a modem on a line of two file descriptors
 *                     (pipe_modem.h)
 *
 */

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <log.h>

#include "dm_types.h"
#include "env.h"
#include "pipe_modem.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "main: " fmt, ##__VA_ARGS__)

namespace dmd {

/* the core gets whole periods: the line's samples in frames of these,
   MAX_SAMPLES at a time at most */
static const s16 HALF_FRAME = 48;
static const s16 MAX_SAMPLES = 42 * HALF_FRAME;

/* the core at rate on the line in -> out, with a new pty for its DTE;
   PIPE_SYNC read here.  Exits if the pty or the core can't be made */
PipeModem::PipeModem(const char *name, s32 rate, const char *country,
		     int in, int out)
	: in_(in), out_(out), rate_(rate), line_(in, HALF_FRAME)
{
	dcore::Options o;
	o.name = name;
	o.dev_name = name;
	o.rate = rate;
	o.country = country;
	/* MODEM_ANALOG=1, or =<name> for this one only */
	if (const char *a = dsp::env("MODEM_ANALOG"))
		analog_ = atoi(a) == 1 || !strcmp(a, name);
	if (dsp::env("PIPE_SYNC")) {
		/* whole half frames */
		sync_ = (dsp::env_long("PIPE_SYNC") + HALF_FRAME - 1) /
			HALF_FRAME * HALF_FRAME;
		next_sync_ = sync_;
	}
	hook_fd_ = dsp::env_int("MODEM_HOOK_FD", hook_fd_);
	ring_.init(rate);
	if (pty_.open() < 0)
		exit(-1);
	m_ = new dcore::Modem(*this, o);
	if (!m_->ok())
		exit(-1);
	DBG("created %s: %s\n", name, pty_.name());
}

/* the core (the recordings close after it) */
PipeModem::~PipeModem()
{
	delete m_;
}

/* see pipe_modem.h */
s16 PipeModem::set_hook(dcore::Modem &, s16 state)
{
	char c = state == MODEM_HOOK_OFF ? '1' : '0';
	if (hook_fd_ >= 0 && c != hook_ && write(hook_fd_, &c, 1) == 1)
		hook_ = c;
	return 0;
}

/* the time in ms: the monotonic clock, or with PIPE_SYNC the line's */
u64 PipeModem::now_ms()
{
	if (sync_) {
		/* the line's clock while it runs (the wall clock's while it
		   does not), so that the timers meet it at the same samples */
		s64 line_ms = samples() * 1000 / rate_;
		u64 t;
		if (line_idle())
			clock_off_ =
				static_cast<s64>(dcore::monotonic_ms()) -
				line_ms;
		t = static_cast<u64>(line_ms + clock_off_);
		if (t < last_ms_)
			t = last_ms_;
		return last_ms_ = t;
	}
	return dcore::monotonic_ms();
}

/* the silence ahead of everything on the line (PIPE_LEAD=<samples>,
   128): once, as the line keeps it, so that every call has the same
   delay; 0, or -1 if it can't be written */
s16 PipeModem::lead()
{
	char zeros[2048] = {};
	s32 n;
	if (lead_done_)
		return 0;
	lead_done_ = true;
	n = dsp::env("PIPE_LEAD") ? 2 * dsp::env_int("PIPE_LEAD") : 256;
	if (n > static_cast<s32>(sizeof(zeros)))
		n = sizeof(zeros);
	lead_ = n / 2;
	if (write(out_, zeros, n) != n)
		return -1;
	return 0;
}

/* off hook (the line is always there, with PIPE_SYNC from the start:
   run()), its delay the lead silence's; 0, or -1 if the lead silence
   can't be written */
s16 PipeModem::start(dcore::Modem &)
{
	s16 ret;
	DBG("modem_test_start...\n");
	ret = lead();
	delay_ = lead_;
	pending_delay_ = 0;
	return ret;
}

/* on hook: nothing to do, the line stays */
s16 PipeModem::stop(dcore::Modem &)
{
	DBG("modem_test_stop...\n");
	delay_ = 0;
	pending_delay_ = 0;
	return 0;
}

/* no faster than the sample clock, so that a DTE program (fax) keeps up
   with the protocol's timers; with PIPE_SYNC on hook as well (the
   DTE's commands then meet the samples they would in real time,
   however late the DTE answers what the modem said), and off hook as
   fast as it goes (speed_ times the sample clock at most), the timers
   on the line's clock */
void PipeModem::pace(s16 samples)
{
	struct timeval tv;
	double now, ahead, speed;
	speed = realtime_ || (sync_ && !m_->started()) ? 1 : speed_;
	if (speed != clock_speed_) {
		clock_speed_ = speed;
		clock_done_ = 0;        /* (it starts anew) */
	}
	if (speed <= 0)
		return;
	gettimeofday(&tv, nullptr);
	now = tv.tv_sec + tv.tv_usec * 1e-6;
	if (clock_done_ == 0 ||
	    now - clock_start_ - clock_done_ / rate_ / speed > 0.5) {
		/* (re)start the clock after a pause */
		clock_start_ = now;
		clock_done_ = 0;
	}
	clock_done_ += samples;
	ahead = clock_start_ + clock_done_ / rate_ / speed - now;
	if (ahead > 0.002)
		usleep(ahead * 1e6);
}

/* the line is readable: what is there into line_ (rd false: only what
   it holds), whole half frames of it through the core (2016 samples at
   most, with PIPE_SYNC up to the next sync point), the answer written
   to out_; 0, or -1 on an error */
s16 PipeModem::line(bool rd)
{
	s16 outbuf[MAX_SAMPLES];
	const s16 *in;
	s16 count, max = MAX_SAMPLES;
	bool off_hook;

	if (sync_ && samples() >= next_sync_) {
		next_sync_ += sync_;
		dte_input();
	}
	if (rd) {
		count = line_.read();
		if (sync_ && count > 0) {
			struct timeval tv;
			/* the line runs again (a call): its periods from
			   here */
			if (line_idle())
				next_sync_ = samples() + sync_;
			gettimeofday(&tv, nullptr);
			line_at_ = tv.tv_sec + tv.tv_usec * 1e-6;
		}
		if (count < 0) {
			ERR("dev read: %s\n", strerror(errno));
			return -1;
		}
		if (count == 0) {
			/* the other end is gone: select() would find it
			   readable for ever */
			DBG("dev read = 0: the line's other end closed\n");
			return -1;
		}
	}
	/* up to the next sample the DTE's input is taken at */
	if (sync_ && max > next_sync_ - samples())
		max = static_cast<s16>(next_sync_ - samples());
	count = line_.frames(&in, max);
	if (count == 0)
		return 0;
	/* a shorter line (update_delay()): input dropped */
	if (pending_delay_ < 0) {
		s16 n = -pending_delay_ < count ? -pending_delay_ : count;
		DBG("change delay -%d...\n", n);
		in += n;
		count -= n;
		delay_ -= n;
		pending_delay_ += n;
		if (count == 0)
			return 0;
	}

	/* no faster than the clock between calls as well: the line's
	   silence goes round and round */
	pace(count);
	off_hook = m_->started();
	if (off_hook && sync_) {
		/* a half frame at a time, with the timers in between: they
		   meet the line at the same samples whatever the reads
		   bring */
		for (s16 k = 0; k < count; k += HALF_FRAME) {
			s16 len = count - k < HALF_FRAME ? count - k :
				HALF_FRAME;
			if (m_->started())
				m_->process(in + k, outbuf + k, len);
			else
				std::fill_n(outbuf + k, len, 0);
			m_->poll();
		}
	} else if (off_hook)
		m_->process(in, outbuf, count);
	else {
		/* on hook: the ring signal's pulses, the timers between
		   (the core checks a burst's cadence by its now_ms()) */
		for (s16 k = 0; k < count; k++)
			if (ring_.sample(in[k]))
				m_->ring();
		/* and caller ID sent before the ring */
		m_->idle_audio(in, count);
		std::fill_n(outbuf, count, 0);
	}
	/* MODEM_REC: the line while off hook */
	if (off_hook && record_) {
		if (!rec_tried_) {
			/* once: both files, or neither */
			rec_tried_ = true;
			rec_.open(record_, ".in", ".out");
		}
		rec_.rx(in, 2 * count);
		rec_.tx(outbuf, 2 * count);
	}
	count = write(out_, outbuf, 2 * count);
	if (count < 0) {
		ERR("dev write: %s\n", strerror(errno));
		return -1;
	}
	if (count == 0)
		DBG("dev write = 0\n");
	/* a longer line (update_delay(), as the modem ran): silence out */
	while (pending_delay_ > 0) {
		static const s16 zeros[MAX_SAMPLES] = {};
		s16 n = pending_delay_ < MAX_SAMPLES ? pending_delay_ :
			MAX_SAMPLES;
		DBG("change delay %d...\n", n);
		if (write(out_, zeros, 2 * n) != 2 * n) {
			ERR("dev write: %s\n", strerror(errno));
			return -1;
		}
		delay_ += n;
		pending_delay_ -= n;
	}
	/* PIPE_SYNC stops at its samples: on with what is left, else it
	   may hold the line's last samples with nothing more to read */
	if (sync_ && line_.waiting() >= HALF_FRAME)
		return line(false);
	return 0;
}

/* run() returns at its next turn */
void PipeModem::stop()
{
	RunLoop::stop();
}

/* PIPE_SYNC: no line samples for 0.5 s (no call yet, nothing flows): the
   DTE's input as it comes */
bool PipeModem::line_idle() const
{
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return tv.tv_sec + tv.tv_usec * 1e-6 - line_at_ > 0.5;
}

/* PIPE_SYNC: at the sample, whatever the DTE has sent */
void PipeModem::dte_input()
{
	struct pollfd p = { pty_.fd(), POLLIN, 0 };
	if (!pty_.want_read(*m_) || poll(&p, 1, 0) <= 0)
		return;
	DBG("DTE input at line sample %" PRId64 "\n", samples());
	pty_.serve(*m_);
}

/* select() on the lines and the ptys of the n modems, their timers in
   between; 0 once stopped, -1 on an error */
s16 PipeModem::run(PipeModem **modems, s16 n)
{
	RunLoop loop;
	std::vector<dcore::Modem *> m(n);

	RunLoop::catch_signals();
	/* with PIPE_SYNC the line runs from the start (on hook in real
	   time): the DTE's commands then meet it at its periods from the
	   first, and a call starts at the same sample every time, not
	   wherever the scheduler had it when a line that stood still came
	   to life */
	for (s16 i = 0; i < n; i++)
		if (modems[i]->sync_ && modems[i]->lead() < 0)
			return -1;
	for (s16 i = 0; i < n; i++)
		m[i] = modems[i]->m_;
	while (RunLoop::running()) {
		s32 next = RunLoop::poll(m.data(), n);
		int ret;

		loop.clear();
		for (s16 i = 0; i < n; i++) {
			PipeModem *p = modems[i];
			loop.watch(p->in_);
			/* with PIPE_SYNC, commands only at the line's
			   periods; data (carrier up) at once */
			if ((!p->sync_ || p->line_idle() ||
			     p->m_->carrier()) &&
			    p->pty_.want_read(*p->m_))
				loop.watch(p->pty_.fd());
		}
		ret = loop.wait(next);
		if (ret < 0) {
			ERR("select: %s\n", strerror(errno));
			return ret;
		}
		if (ret == 0)
			continue;
		for (s16 i = 0; i < n; i++)
			if (loop.ready(modems[i]->in_) &&
			    modems[i]->line() < 0)
				return -1;
		for (s16 i = 0; i < n; i++) {
			PipeModem *p = modems[i];
			if (loop.ready(p->pty_.fd()) &&
			    p->pty_.serve(*p->m_) < 0)
				return -1;
		}
	}
	return 0;
}

} // namespace dmd
