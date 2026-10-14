/*
 *
 *    half_dte.h - the harnesses' output to their DTE (half_agere,
 *                 half_sm56, half_pctel, half_hsf, half_spandsp)
 *
 *    A harness's pty is non-blocking (the line goes on whatever the DTE
 *    does) and takes some 20 KB ahead of the DTE's reading: what a
 *    write() refused when the driver was late to read was lost, a block
 *    out of the middle of the data.  dte_write() keeps it, in order, and
 *    dte_flush(), each time round the harness's loop, writes it on as
 *    the pty takes it.  The modems' output is taken from them as before,
 *    so their own flow control has no part in it.
 *
 *    The other way, dte_read() gives the modem what it takes of the
 *    DTE's input and keeps the rest for the next time, before it reads
 *    any more: a modem can take less than the room it gave (the SM56
 *    modem does, with a driver that writes ahead of it).
 *
 */

#ifndef HALF_DTE_H
#define HALF_DTE_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
	unsigned char *buf;
	size_t head, n, size;   /* buf[head .. head + n) waits */
} dte_q;

/* what waits to the pty, as much as it takes */
static inline void dte_flush(int pty)
{
	while (dte_q.n) {
		ssize_t w = write(pty, dte_q.buf + dte_q.head, dte_q.n);
		if (w <= 0) {
			if (w < 0 && errno != EAGAIN && errno != EINTR)
				dte_q.n = 0;    /* (no DTE: the pty gone) */
			break;
		}
		dte_q.head += (size_t)w;
		dte_q.n -= (size_t)w;
	}
	if (!dte_q.n)
		dte_q.head = 0;
}

/* n octets to the pty, after what waits; what it does not take now
   waits */
static inline void dte_write(int pty, const void *p, size_t n)
{
	if (!n)
		return;
	dte_flush(pty);
	if (!dte_q.n) {
		ssize_t w = write(pty, p, n);
		if (w > 0) {
			p = (const unsigned char *)p + w;
			n -= (size_t)w;
		}
		if (!n)
			return;
	}
	if (dte_q.head + dte_q.n + n > dte_q.size) {
		memmove(dte_q.buf, dte_q.buf + dte_q.head, dte_q.n);
		dte_q.head = 0;
		if (dte_q.n + n > dte_q.size) {
			size_t size = dte_q.size ? dte_q.size : 65536;
			while (size < dte_q.n + n)
				size *= 2;
			dte_q.buf = (unsigned char *)realloc(dte_q.buf, size);
			if (!dte_q.buf) {
				perror("dte_write");
				exit(1);
			}
			dte_q.size = size;
		}
	}
	memcpy(dte_q.buf + dte_q.head + dte_q.n, p, n);
	dte_q.n += n;
}

/* the DTE's input to the modem, up to room octets: put() takes what it
   can of n at p and says how many; what it left waits for the next
   call */
static inline void dte_read(int pty, int room,
			    int (*put)(const void *p, int n))
{
	static unsigned char buf[256];
	static int head, n;
	int k;

	while (room > 0) {
		if (!n) {
			ssize_t r = read(pty, buf, room < (int)sizeof(buf) ?
					 (size_t)room : sizeof(buf));
			if (r <= 0)
				return;
			head = 0;
			n = (int)r;
		}
		k = put(buf + head, n < room ? n : room);
		if (k <= 0)
			return;
		head += k;
		n -= k;
		room -= k;
	}
}

#endif
