/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    modem_half.cpp - what the test peers' harnesses share (see
 *                     modem_half.hpp)
 *
 */

#include "modem_half.hpp"

#include "line_frames.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

namespace half {

/*
 *    the stack before a call into a closed module
 */

/* see modem_half.hpp */
void __attribute__((noinline)) stack_clear()
{
	static int fill = -1;
	char z[8192];

	if (fill < 0)
		fill = getenv("HALF_STACK_FILL") ?
			atoi(getenv("HALF_STACK_FILL")) & 0xff : 0;
	memset(z, fill, sizeof(z));
	__asm__ volatile("" : : "r"(z) : "memory");
}

/*
 *    the pty
 */

/* the queue freed */
Pty::~Pty()
{
	free(q_);
}

/* see modem_half.hpp */
int Pty::open()
{
	struct termios t;

	fd_ = posix_openpt(O_RDWR | O_NOCTTY);
	if (fd_ < 0 || grantpt(fd_) || unlockpt(fd_)) {
		perror("pty");
		return -1;
	}
	tcgetattr(fd_, &t);
	cfmakeraw(&t);
	tcsetattr(fd_, TCSANOW, &t);
	fcntl(fd_, F_SETFL, O_NONBLOCK);
	name_ = ptsname(fd_);
	return 0;
}

/* see modem_half.hpp */
void Pty::flush()
{
	while (n_) {
		ssize_t w = ::write(fd_, q_ + head_, n_);
		if (w <= 0) {
			if (w < 0 && errno != EAGAIN && errno != EINTR)
				n_ = 0;         /* (no DTE: the pty gone) */
			break;
		}
		head_ += (size_t)w;
		n_ -= (size_t)w;
	}
	if (!n_)
		head_ = 0;
}

/* see modem_half.hpp */
void Pty::write(const void *p, size_t n)
{
	if (!n)
		return;
	flush();
	if (!n_) {
		ssize_t w = ::write(fd_, p, n);
		if (w > 0) {
			p = (const byte *)p + w;
			n -= (size_t)w;
		}
		if (!n)
			return;
	}
	if (head_ + n_ + n > size_) {
		memmove(q_, q_ + head_, n_);
		head_ = 0;
		if (n_ + n > size_) {
			size_t size = size_ ? size_ : 65536;
			while (size < n_ + n)
				size *= 2;
			q_ = (byte *)realloc(q_, size);
			if (!q_) {
				perror("pty");
				exit(1);
			}
			size_ = size;
		}
	}
	memcpy(q_ + head_ + n_, p, n);
	n_ += n;
}

/* see modem_half.hpp */
void Pty::read(int room, const std::function<int(const void *p, int n)> &put)
{
	int k;

	while (room > 0) {
		if (!in_n_) {
			ssize_t r = ::read(fd_, in_, room < (int)sizeof(in_) ?
					   (size_t)room : sizeof(in_));
			if (r <= 0)
				return;
			in_head_ = 0;
			in_n_ = (int)r;
		}
		k = put(in_ + in_head_, in_n_ < room ? in_n_ : room);
		if (k <= 0)
			return;
		in_head_ += k;
		in_n_ -= k;
		room -= k;
	}
}

/*
 *    the result codes
 */

/* octet c of the output: online on CONNECT, offline on NO
   CARRIER or OK */
void Results::scan(byte c)
{
	static const char *const pat[] = { "CONNECT", "NO CARRIER",
					   "\r\nOK\r\n" };
	int i;

	for (i = 0; i < 3; i++) {
		if (c != (byte)pat[i][at_[i]])
			at_[i] = c == (byte)pat[i][0];
		else
			at_[i]++;
		if (!pat[i][at_[i]]) {
			at_[i] = 0;
			online_ = i == 0;
		}
	}
}

/* n octets of it at p */
void Results::scan(const void *p, size_t n)
{
	for (size_t k = 0; k < n; k++)
		scan(((const byte *)p)[k]);
}

/*
 *    the program
 */

/* see modem_half.hpp */
HalfModem::HalfModem(const char *name, const char *prefix) :
	name_(name), prefix_(prefix)
{
}

/* see modem_half.hpp */
const char *HalfModem::env(const char *what) const
{
	char v[64];
	snprintf(v, sizeof(v), "%s_%s", prefix_, what);
	return getenv(v);
}

/* see modem_half.hpp */
void HalfModem::log(const char *fmt, ...) const
{
	va_list ap;
	fprintf(stderr, "%s: ", name_);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* see modem_half.hpp */
short HalfModem::line_sample(short in)
{
	(void)in;
	line_n++;
	return 0;
}

/* see modem_half.hpp */
int HalfModem::line_block(const short *in, short *out, int n)
{
	int i = 0;
	while (i < n) {
		out[i] = line_sample(in[i]);
		i++;
		if (line_n % 48 == 0)
			break;
	}
	return i;
}

/* see modem_half.hpp */
void HalfModem::after_read()
{
	if (!sync_n)
		dte_poll();
}

/* see modem_half.hpp */
int HalfModem::run()
{
	short out[480];
	dmd::LineFrames line(3, 48);
	const char *s;

	dbg = (s = env("DEBUG")) ? atoi(s) : 0;
	if ((s = env("RATE")))
		line_rate = atoi(s);
	sync_n = (s = env("SYNC")) ? (atol(s) + 47) / 48 * 48 : 0;
	sync_phase = (s = env("SYNC_PHASE")) ?
		atol(s) / 48 * 48 % (sync_n ? sync_n : 1) : 0;
	setvbuf(stdout, NULL, _IONBF, 0);

	if (pty.open() || start())
		return 1;
	printf("created %s: %s\n", name_, pty.name());

	/* the other side waits for us first */
	memset(out, 0, sizeof(out));
	if (::write(4, out, 48 * 2 * 4) < 0) {
		perror("the line (fd 4)");
		return 1;
	}
	/* what comes (an odd octet waits for its other half), in blocks
	   to each 48th sample of line_n, the DTE between them; a modem's
	   delay runs line_n on as well, so a block can end early (what it
	   left goes in the next) */
	while (line.read(sizeof(out)) > 0) {
		const short *in;
		int n = 0, k, done;
		while ((k = line.block(&in, 48, line_n)) > 0) {
			done = line_block(in, out + n, k);
			line.unget(k - done);
			n += done;
			if (line_n % 48 == 0)
				dte_poll();
		}
		if (::write(4, out, n * 2) != n * 2)
			break;
		after_read();
	}
	return 0;
}

}
