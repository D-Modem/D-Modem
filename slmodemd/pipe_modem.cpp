/*
 *
 *    pipe_modem.cpp - a modem on a line of two file descriptors
 *                     (pipe_modem.h)
 *
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include <log.h>

#include "pipe_modem.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "main: " fmt, ##__VA_ARGS__)

namespace slm {

/* the core gets whole periods: the line's samples in frames of these */
static const int HALF_FRAME = 48;

/* the core at rate on the line in -> out, with a new pty for its DTE;
   PIPE_SYNC read here.  Exits if the pty or the core can't be made */
PipeModem::PipeModem(const char *name, int rate, const char *country,
		     int in, int out)
	: in_(in), out_(out), rate_(rate)
{
	dcore::Options o;
	o.name = name;
	o.dev_name = name;
	o.rate = rate;
	o.country = country;
	if (getenv("PIPE_SYNC")) {
		/* whole half frames */
		sync_ = (atol(getenv("PIPE_SYNC")) + HALF_FRAME - 1) /
			HALF_FRAME * HALF_FRAME;
		next_sync_ = sync_;
	}
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

/* the time in ms: the monotonic clock, or with PIPE_SYNC the line's */
unsigned long PipeModem::now_ms()
{
	if (sync_) {
		/* the line's clock while it runs (the wall clock's while it
		   does not), so that the timers meet it at the same samples */
		long line_ms = (long)(samples_ * 1000 / rate_);
		unsigned long t;
		if (line_idle())
			clock_off_ = (long)dcore::monotonic_ms() - line_ms;
		t = (unsigned long)(line_ms + clock_off_);
		if (t < last_ms_)
			t = last_ms_;
		return last_ms_ = t;
	}
	return dcore::monotonic_ms();
}

/* the silence ahead of everything on the line (PIPE_LEAD=<samples>,
   128): once, as the line keeps it, so that every call has the same
   delay; 0, or -1 if it can't be written */
int PipeModem::lead()
{
	char zeros[2048] = {};
	int n;
	if (lead_done_)
		return 0;
	lead_done_ = true;
	n = getenv("PIPE_LEAD") ? 2 * atoi(getenv("PIPE_LEAD")) : 256;
	if (n > (int)sizeof(zeros))
		n = sizeof(zeros);
	if (write(out_, zeros, n) != n)
		return -1;
	return 0;
}

/* off hook (the line is always there, with PIPE_SYNC from the start:
   run()); 0, or -1 if the lead silence can't be written */
int PipeModem::start(dcore::Modem &)
{
	DBG("modem_test_start...\n");
	return lead();
}

/* on hook: nothing to do, the line stays */
int PipeModem::stop(dcore::Modem &)
{
	DBG("modem_test_stop...\n");
	return 0;
}

/* no faster than the sample clock, so that a DTE program (fax) keeps up
   with the protocol's timers; with PIPE_SYNC on hook as well (the
   DTE's commands then meet the samples they would in real time,
   however late the DTE answers what the modem said), and off hook as
   fast as it goes (speed_ times the sample clock at most), the timers
   on the line's clock */
void PipeModem::pace(int samples)
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
	gettimeofday(&tv, NULL);
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

/* the line is readable: what is there into pend_ (rd false: only what
   pend_ holds), whole half frames of it through the core (4032 bytes at
   most, with PIPE_SYNC up to the next sync point), the answer written
   to out_; 0, or -1 on an error */
int PipeModem::line(bool rd)
{
	char inbuf[4096], outbuf[4096];
	const int frame = HALF_FRAME * 2;
	char *in = inbuf;
	int count;
	bool off_hook;

	if (sync_ && samples_ >= next_sync_) {
		next_sync_ += sync_;
		dte_input();
	}
	if (!rd)
		goto pending;
	count = read(in_, pend_ + npend_, sizeof(pend_) - npend_);
	if (sync_ && count > 0) {
		struct timeval tv;
		/* the line runs again (a call): its periods from here */
		if (line_idle())
			next_sync_ = samples_ + sync_;
		gettimeofday(&tv, NULL);
		line_at_ = tv.tv_sec + tv.tv_usec * 1e-6;
	}

	if (count < 0) {
		ERR("dev read: %s\n", strerror(errno));
		return -1;
	}
	if (count == 0) {
		/* the other end is gone: select() would find it readable
		   for ever */
		DBG("dev read = 0: the line's other end closed\n");
		return -1;
	}
	npend_ += count;
pending:
	count = npend_ / frame * frame;
	if (count > (int)sizeof(inbuf) / frame * frame)
		count = sizeof(inbuf) / frame * frame;
	/* up to the next sample the DTE's input is taken at */
	if (sync_ && count > (next_sync_ - samples_) * 2)
		count = (int)(next_sync_ - samples_) * 2;
	if (count == 0)
		return 0;
	memcpy(inbuf, pend_, count);
	memmove(pend_, pend_ + count, npend_ - count);
	npend_ -= count;
	samples_ += count / 2;

	/* no faster than the clock between calls as well: the line's
	   silence goes round and round */
	pace(count / 2);
	off_hook = m_->started();
	if (off_hook && sync_) {
		/* a half frame at a time, with the timers in between: they
		   meet the line at the same samples whatever the reads
		   bring */
		for (int k = 0; k < count / 2; k += HALF_FRAME) {
			int len = count / 2 - k < HALF_FRAME ?
				count / 2 - k : HALF_FRAME;
			if (m_->started())
				m_->process((int16_t *)in + k,
					    (int16_t *)outbuf + k, len);
			else
				memset(outbuf + 2 * k, 0, 2 * len);
			m_->poll();
		}
	} else if (off_hook)
		m_->process((int16_t *)in, (int16_t *)outbuf, count / 2);
	else
		memset(outbuf, 0, count);
	/* MODEM_REC: the line while off hook */
	if (off_hook && record_) {
		if (!rec_tried_) {
			/* once: both files, or neither */
			rec_tried_ = true;
			rec_.open(record_, ".in", ".out");
		}
		rec_.rx(in, count);
		rec_.tx(outbuf, count);
	}
	count = write(out_, outbuf, count);
	if (count < 0) {
		ERR("dev write: %s\n", strerror(errno));
		return -1;
	}
	if (count == 0)
		DBG("dev write = 0\n");
	/* PIPE_SYNC stops at its samples: on with what is left, else it
	   may hold the line's last samples with nothing more to read */
	if (sync_ && npend_ >= frame)
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
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec * 1e-6 - line_at_ > 0.5;
}

/* PIPE_SYNC: at the sample, whatever the DTE has sent */
void PipeModem::dte_input()
{
	struct pollfd p = { pty_.fd(), POLLIN, 0 };
	if (!pty_.want_read(*m_) || poll(&p, 1, 0) <= 0)
		return;
	DBG("DTE input at line sample %ld\n", samples_);
	pty_.serve(*m_);
}

/* select() on the lines and the ptys of the n modems, their timers in
   between; 0 once stopped, -1 on an error */
int PipeModem::run(PipeModem **modems, int n)
{
	RunLoop loop;
	std::vector<dcore::Modem *> m(n);

	RunLoop::catch_signals();
	/* with PIPE_SYNC the line runs from the start (on hook in real
	   time): the DTE's commands then meet it at its periods from the
	   first, and a call starts at the same sample every time, not
	   wherever the scheduler had it when a line that stood still came
	   to life */
	for (int i = 0; i < n; i++)
		if (modems[i]->sync_ && modems[i]->lead() < 0)
			return -1;
	for (int i = 0; i < n; i++)
		m[i] = modems[i]->m_;
	while (RunLoop::running()) {
		long next = RunLoop::poll(m.data(), n);
		int ret;

		loop.clear();
		for (int i = 0; i < n; i++) {
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
		for (int i = 0; i < n; i++)
			if (loop.ready(modems[i]->in_) &&
			    modems[i]->line() < 0)
				return -1;
		for (int i = 0; i < n; i++) {
			PipeModem *p = modems[i];
			if (loop.ready(p->pty_.fd()) &&
			    p->pty_.serve(*p->m_) < 0)
				return -1;
		}
	}
	return 0;
}

} // namespace slm
