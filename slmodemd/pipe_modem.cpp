/*
 *
 *    pipe_modem.cpp - a modem on a line of two file descriptors
 *                     (pipe_modem.h)
 *
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <log.h>

#include "pipe_modem.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "main: " fmt, ##__VA_ARGS__)

namespace slm {

/* the core gets whole periods: the line's samples in frames of these */
static const int HALF_FRAME = 48;

static volatile sig_atomic_t keep_running = 1;

PipeModem::PipeModem(const char *name, int rate, const char *country,
		     int in, int out)
	: in_(in), out_(out), rate_(rate)
{
	dcore::Options o;
	o.name = name;
	o.dev_name = name;
	o.rate = rate;
	o.country = country;
	if (pty_.open() < 0)
		exit(-1);
	m_ = new dcore::Modem(*this, o);
	if (!m_->ok())
		exit(-1);
	DBG("created %s: %s\n", name, pty_.name());
}

PipeModem::~PipeModem()
{
	delete m_;
	if (rec_in_) {
		fclose((FILE *)rec_in_);
		fclose((FILE *)rec_out_);
	}
}

unsigned long PipeModem::now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
}

int PipeModem::start(dcore::Modem &)
{
	char zeros[256] = {};
	DBG("modem_test_start...\n");
	delay_ = sizeof(zeros);
	if (write(out_, zeros, delay_) != delay_)
		return -1;
	return 0;
}

int PipeModem::stop(dcore::Modem &)
{
	DBG("modem_test_stop...\n");
	delay_ = 0;
	return 0;
}

/* no faster than the sample clock, so that a DTE program (fax) keeps up
   with the protocol's timers */
void PipeModem::pace(int samples)
{
	struct timeval tv;
	double now, ahead;
	if (!realtime_)
		return;
	gettimeofday(&tv, NULL);
	now = tv.tv_sec + tv.tv_usec * 1e-6;
	if (clock_done_ == 0 ||
	    now - clock_start_ - clock_done_ / rate_ > 0.5) {
		/* (re)start the clock after a pause */
		clock_start_ = now;
		clock_done_ = 0;
	}
	clock_done_ += samples;
	ahead = clock_start_ + clock_done_ / rate_ - now;
	if (ahead > 0.002)
		usleep(ahead * 1e6);
}

int PipeModem::line()
{
	char inbuf[4096], outbuf[4096];
	const int frame = HALF_FRAME * 2;
	char *in = inbuf;
	int count = read(in_, pend_ + npend_, sizeof(pend_) - npend_);

	if (count < 0) {
		ERR("dev read: %s\n", strerror(errno));
		return -1;
	}
	if (count == 0) {
		DBG("dev read = 0\n");
		return 0;
	}
	npend_ += count;
	count = npend_ / frame * frame;
	if (count > (int)sizeof(inbuf) / frame * frame)
		count = sizeof(inbuf) / frame * frame;
	if (count == 0)
		return 0;
	memcpy(inbuf, pend_, count);
	memmove(pend_, pend_ + count, npend_ - count);
	npend_ -= count;

	pending_delay_ += m_->take_update_delay();
	if (pending_delay_ < 0) {
		if (-pending_delay_ >= count / 2) {
			DBG("change delay -%d...\n", count / 2);
			delay_ -= count;
			pending_delay_ += count / 2;
			return 0;
		}
		DBG("change delay %d...\n", pending_delay_);
		in -= pending_delay_ * 2;
		count += pending_delay_ * 2;
		delay_ += pending_delay_ * 2;
		pending_delay_ = 0;
	}
	if (m_->started()) {
		m_->process((int16_t *)in, (int16_t *)outbuf, count / 2);
		pace(count / 2);
		if (record_ && !rec_in_) {
			char name[256];
			snprintf(name, sizeof(name), "%s.in", record_);
			rec_in_ = fopen(name, "wb");
			snprintf(name, sizeof(name), "%s.out", record_);
			rec_out_ = fopen(name, "wb");
		}
		if (rec_in_ && rec_out_) {
			fwrite(in, 1, count, (FILE *)rec_in_);
			fwrite(outbuf, 1, count, (FILE *)rec_out_);
		}
	} else
		memset(outbuf, 0, count);
	count = write(out_, outbuf, count);
	if (count < 0) {
		ERR("dev write: %s\n", strerror(errno));
		return -1;
	}
	if (count == 0)
		DBG("dev write = 0\n");
	pending_delay_ += m_->take_update_delay();
	if (pending_delay_ > 0) {
		int n = pending_delay_ * 2 > (int)sizeof(outbuf) ?
			sizeof(outbuf) : pending_delay_ * 2;
		DBG("change delay %d...\n", pending_delay_);
		memset(outbuf, 0, n);
		count = write(out_, outbuf, n);
		if (count < 0) {
			ERR("dev write: %s\n", strerror(errno));
			return -1;
		}
		delay_ += n;
		pending_delay_ -= n / 2;
	}
	return 0;
}

static void mark_termination(int signum)
{
	(void)signum;
	keep_running = 0;
}

void PipeModem::stop()
{
	keep_running = 0;
}

int PipeModem::run(PipeModem **modems, int n)
{
	signal(SIGINT, mark_termination);
	signal(SIGTERM, mark_termination);
	while (keep_running) {
		struct timeval tmo;
		fd_set rset;
		int max_fd = 0, ret;
		long next = 1000;

		for (int i = 0; i < n; i++) {
			long t;
			modems[i]->m_->poll();
			t = modems[i]->m_->next_timer_ms();
			if (t >= 0 && t < next)
				next = t;
		}
		tmo.tv_sec = next / 1000;
		tmo.tv_usec = next % 1000 * 1000;
		FD_ZERO(&rset);
		for (int i = 0; i < n; i++) {
			PipeModem *p = modems[i];
			FD_SET(p->in_, &rset);
			if (p->in_ > max_fd)
				max_fd = p->in_;
			if (p->pty_.want_read(*p->m_)) {
				FD_SET(p->pty_.fd(), &rset);
				if (p->pty_.fd() > max_fd)
					max_fd = p->pty_.fd();
			}
		}
		ret = select(max_fd + 1, &rset, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ERR("select: %s\n", strerror(errno));
			return ret;
		}
		if (ret == 0)
			continue;
		for (int i = 0; i < n; i++)
			if (FD_ISSET(modems[i]->in_, &rset) &&
			    modems[i]->line() < 0)
				return -1;
		for (int i = 0; i < n; i++) {
			PipeModem *p = modems[i];
			if (!FD_ISSET(p->pty_.fd(), &rset))
				continue;
			switch (p->pty_.read(*p->m_)) {
			case Pty::CLOSED:
				if (p->pty_.hupcl())
					p->m_->hangup();
				break;
			case Pty::ERROR:
				return -1;
			default:
				break;
			}
		}
	}
	return 0;
}

} // namespace slm
