/*
 *
 *    modem_half.cpp - what the test peers' harnesses share (see
 *                     modem_half.hpp)
 *
 */

#include "modem_half.hpp"

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

Pty::~Pty()
{
	free(q_);
}

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

void Results::scan(const void *p, size_t n)
{
	for (size_t k = 0; k < n; k++)
		scan(((const byte *)p)[k]);
}

/*
 *    the program
 */

HalfModem::HalfModem(const char *name, const char *prefix) :
	name_(name), prefix_(prefix)
{
}

const char *HalfModem::env(const char *what) const
{
	char v[64];
	snprintf(v, sizeof(v), "%s_%s", prefix_, what);
	return getenv(v);
}

void HalfModem::log(const char *fmt, ...) const
{
	va_list ap;
	fprintf(stderr, "%s: ", name_);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

short HalfModem::line_sample(short in)
{
	(void)in;
	line_n++;
	return 0;
}

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

void HalfModem::after_read()
{
	if (!sync_n)
		dte_poll();
}

int HalfModem::run()
{
	short in[480], out[480];
	int odd = 0;
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
	for (;;) {
		int n = (int)::read(3, (char *)in + odd, sizeof(in) - odd);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		n += odd;
		odd = n & 1;
		n /= 2;
		/* in blocks to each 48th sample of the line, the DTE
		   between them */
		for (int i = 0; i < n; ) {
			int k = 48 - (int)(line_n % 48);
			if (k > n - i)
				k = n - i;
			i += line_block(in + i, out + i, k);
			if (line_n % 48 == 0)
				dte_poll();
		}
		if (::write(4, out, n * 2) != n * 2)
			break;
		if (odd)
			((char *)in)[0] = ((char *)in)[n * 2];
		after_read();
	}
	return 0;
}

}
