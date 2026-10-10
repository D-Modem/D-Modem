/*
 *    dmodem_pair.c - two modems of libdmodem back to back in one process
 *    (the caller dials, the answerer answers), data both ways through
 *    their DTE socketpairs; several pairs in threads at once.
 *
 *    dmodem_pair [AT+MS=34] [pairs] [seconds]
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dmodem.h"

#define FRAME 160               /* 20 ms, as Asterisk */

struct pair {
	int id;
	const char *ms;
	int seconds;
	int ok;
	int fax_seen;
};

static int nonblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void drain(int fd)
{
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

static void *run(void *arg)
{
	struct pair *p = arg;
	struct dmodem_config c = { 0 };
	struct dmodem *a, *b;
	int sa[2], sb[2], i, frames = p->seconds * 50, online = 0;
	short ab[FRAME] = { 0 }, ba[FRAME] = { 0 }, oa[FRAME], ob[FRAME];
	char init[64], msg[] = "The quick brown fox jumps over the lazy dog.\r\n";
	char got_a[4096], got_b[4096];
	int na = 0, nb = 0, sent = 0;

	socketpair(AF_UNIX, SOCK_STREAM, 0, sa);
	socketpair(AF_UNIX, SOCK_STREAM, 0, sb);
	nonblock(sa[0]); nonblock(sa[1]); nonblock(sb[0]); nonblock(sb[1]);
	snprintf(init, sizeof(init), "ATE0Q1X3%s\r", p->ms);
	c.init = init;
	c.law = DMODEM_LAW_ULAW;
	c.name = "caller";
	c.dte_fd = sa[0];
	a = dmodem_create(&c);
	c.name = "answerer";
	c.dte_fd = sb[0];
	b = dmodem_create(&c);
	drain(sa[1]);
	drain(sb[1]);
	dmodem_answer(b);
	dmodem_dial(a, "");

	for (i = 0; i < frames; i++) {
		struct dmodem_status s1, s2;
		int n;
		dmodem_process(a, ba, oa, FRAME);
		dmodem_process(b, ab, ob, FRAME);
		memcpy(ab, oa, sizeof(ab));
		memcpy(ba, ob, sizeof(ba));
		dmodem_poll_dte(a);
		dmodem_poll_dte(b);
		dmodem_status(a, &s1);
		dmodem_status(b, &s2);
		/* a data call must never look like a fax machine calling */
		if (s1.fax_caller || s2.fax_caller) {
			if (!p->fax_seen)
				printf("pair %d: fax CNG seen in a data call\n",
				       p->id);
			p->fax_seen = 1;
		}
		if (!online && s1.link == DMODEM_ONLINE &&
		    s2.link == DMODEM_ONLINE) {
			online = i;
			printf("pair %d: online after %.1f s, %u/%u bit/s\n",
			       p->id, i / 50.0, s1.tx_rate, s1.rx_rate);
			drain(sa[1]);
			drain(sb[1]);
		}
		/* a line of text each way every second once online */
		if (online && i > online + 50 && (i - online) % 50 == 0 &&
		    sent < 5) {
			write(sa[1], msg, strlen(msg));
			write(sb[1], msg, strlen(msg));
			sent++;
		}
		while ((n = read(sa[1], got_a + na, sizeof(got_a) - na)) > 0)
			na += n;
		while ((n = read(sb[1], got_b + nb, sizeof(got_b) - nb)) > 0)
			nb += n;
	}
	p->ok = !p->fax_seen && sent == 5 && na == 5 * (int)strlen(msg) &&
		nb == 5 * (int)strlen(msg) &&
		!memcmp(got_a, msg, strlen(msg)) && !memcmp(got_b, msg, strlen(msg));
	if (!p->ok) printf("a: [%.*s]\nb: [%.*s]\n", na, got_a, nb, got_b);
	printf("pair %d: received %d and %d of %d: %s\n", p->id, na, nb,
	       5 * (int)strlen(msg), p->ok ? "PASS" : "FAIL");
	dmodem_destroy(a);
	dmodem_destroy(b);
	return NULL;
}

int main(int argc, char **argv)
{
	int n = argc > 2 ? atoi(argv[2]) : 1, i, fails = 0;
	pthread_t th[16];
	struct pair p[16];

	dmodem_init();
	if (getenv("DMODEM_DEBUG"))
		dmodem_set_debug(atoi(getenv("DMODEM_DEBUG")));
	memset(p, 0, sizeof(p));
	for (i = 0; i < n && i < 16; i++) {
		p[i].id = i;
		p[i].ms = argc > 1 ? argv[1] : "";
		p[i].seconds = argc > 3 ? atoi(argv[3]) : 25;
		pthread_create(&th[i], NULL, run, &p[i]);
	}
	for (i = 0; i < n && i < 16; i++) {
		pthread_join(th[i], NULL);
		fails += !p[i].ok;
	}
	return fails != 0;
}
