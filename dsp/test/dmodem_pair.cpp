/*
 *    dmodem_pair.cpp - two modems of libdmodem back to back in one process
 *    (the caller dials, the answerer answers), data both ways through
 *    their DTE socketpairs; several pairs in threads at once.
 *
 *    dmodem_pair [AT+MS=34] [pairs] [seconds]
 *    DMODEM_LAW=alaw: the modems on an A-law line
 *    DMODEM_NET: the digital network between them, as G.711 octets
 *      g711    as it is
 *      rbs     robbed bit signalling: the LSB of every 6th octet is 1
 *              (a T1 trunk; each way at another place in the frame)
 *      pad     a 3 dB digital loss (decoded, scaled, coded again)
 *      xlaw    the caller on mu-law, the answerer on A-law, converted
 *    (else the 16 bit samples go across unchanged)
 *    DMODEM_RATE=<bit/s>: fail unless online at least this fast
 *    DMODEM_BULK=<bytes>: then that much from the caller to the answerer,
 *    the throughput (in the modems' time) printed
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

extern int dmodem_g711_decode(int alaw, unsigned char c);
extern unsigned char dmodem_g711_encode(int alaw, int x);

enum { NET_LINEAR, NET_G711, NET_RBS, NET_PAD, NET_XLAW };
static int net;

/* an octet across the network: from a modem on law from to one on law
   to; k counts the octets of this direction */
/* bulk data that does not compress */
static char bulk_byte(long k)
{
	unsigned h = (unsigned)k;
	h = ((h >> 16) ^ h) * 0x45d9f3b;
	h = ((h >> 16) ^ h) * 0x45d9f3b;
	return (char)((h >> 16) ^ h);
}

static unsigned char net_octet(unsigned char c, int from, int to, long k,
			       int phase)
{
	switch (net) {
	case NET_RBS:
		if ((k + phase) % 6 == 0)
			c |= 1;
		break;
	case NET_PAD:
		c = dmodem_g711_encode(to,
			(int)(dmodem_g711_decode(from, c) * 0.708f));
		break;
	case NET_XLAW:
		c = dmodem_g711_encode(to, dmodem_g711_decode(from, c));
		break;
	}
	return c;
}

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
	struct pair *p = static_cast<pair*>(arg);
	struct dmodem_config c = { 0 };
	struct dmodem *a, *b;
	int sa[2], sb[2], i, frames = p->seconds * 50, online = 0;
	short ab[FRAME] = { 0 }, ba[FRAME] = { 0 }, oa[FRAME], ob[FRAME];
	char init[64], msg[] = "The quick brown fox jumps over the lazy dog.\r\n";
	char got_a[4096], got_b[4096];
	int na = 0, nb = 0, sent = 0;
	unsigned char gab[FRAME], gba[FRAME], goa[FRAME], gob[FRAME];
	int law_a, law_b;
	long ka = 0, kb = 0;
	long bulk = getenv("DMODEM_BULK") ? atol(getenv("DMODEM_BULK")) : 0;
	long bulk_out = 0, bulk_in = 0, bulk_start = 0, bulk_bad = 0;

	socketpair(AF_UNIX, SOCK_STREAM, 0, sa);
	socketpair(AF_UNIX, SOCK_STREAM, 0, sb);
	nonblock(sa[0]); nonblock(sa[1]); nonblock(sb[0]); nonblock(sb[1]);
	snprintf(init, sizeof(init), "ATE0Q1X3%s\r", p->ms);
	c.init = init;
	c.law = getenv("DMODEM_LAW") && !strcmp(getenv("DMODEM_LAW"), "alaw") ?
		DMODEM_LAW_ALAW : DMODEM_LAW_ULAW;
	c.name = "caller";
	c.dte_fd = sa[0];
	if (net == NET_XLAW)
		c.law = DMODEM_LAW_ULAW;
	law_a = c.law == DMODEM_LAW_ALAW;
	a = dmodem_create(&c);
	c.name = "answerer";
	c.dte_fd = sb[0];
	if (net == NET_XLAW)
		c.law = DMODEM_LAW_ALAW;
	law_b = c.law == DMODEM_LAW_ALAW;
	b = dmodem_create(&c);
	memset(gab, law_b ? 0xD5 : 0xFF, sizeof(gab));
	memset(gba, law_a ? 0xD5 : 0xFF, sizeof(gba));
	drain(sa[1]);
	drain(sb[1]);
	dmodem_answer(b);
	dmodem_dial(a, "");

	for (i = 0; i < frames; i++) {
		struct dmodem_status s1, s2;
		int n;
		if (net == NET_LINEAR) {
			dmodem_process(a, ba, oa, FRAME);
			dmodem_process(b, ab, ob, FRAME);
			memcpy(ab, oa, sizeof(ab));
			memcpy(ba, ob, sizeof(ba));
		} else {
			int k;
			dmodem_process_g711(a, gba, goa, FRAME);
			dmodem_process_g711(b, gab, gob, FRAME);
			for (k = 0; k < FRAME; k++) {
				gab[k] = net_octet(goa[k], law_a, law_b, ka++, 0);
				gba[k] = net_octet(gob[k], law_b, law_a, kb++, 3);
			}
		}
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
			if (getenv("DMODEM_RATE") &&
			    (s1.tx_rate < (unsigned)atoi(getenv("DMODEM_RATE")) ||
			     s1.rx_rate < (unsigned)atoi(getenv("DMODEM_RATE")))) {
				printf("pair %d: slower than %s\n", p->id,
				       getenv("DMODEM_RATE"));
				p->fax_seen = 1;        /* a failure */
			}
			drain(sa[1]);
			drain(sb[1]);
		}
		/* bulk data, once the text is through */
		if (bulk && online && sent == 5 && na >= 5 * (int)strlen(msg) &&
		    nb >= 5 * (int)strlen(msg) && bulk_in < bulk) {
			char buf[4096];
			long k;
			if (!bulk_start)
				bulk_start = i;
			while (bulk_out < bulk) {
				int m = (int)(bulk - bulk_out < (long)sizeof(buf) ?
					      bulk - bulk_out : (long)sizeof(buf));
				for (k = 0; k < m; k++)
					buf[k] = bulk_byte(bulk_out + k);
				m = (int)write(sa[1], buf, m);
				if (m <= 0)
					break;
				bulk_out += m;
			}
			while ((n = read(sb[1], buf, sizeof(buf))) > 0) {
				for (k = 0; k < n; k++)
					if (buf[k] != bulk_byte(bulk_in + k))
						bulk_bad++;
				bulk_in += n;
			}
			if (bulk_in >= bulk) {
				double t = (i - bulk_start) / 50.0;
				printf("pair %d: %ld bytes in %.1f s: %.0f bit/s, %ld wrong\n",
				       p->id, bulk_in, t, bulk_in * 8 / t, bulk_bad);
				if (bulk_bad)
					p->fax_seen = 1;
			}
			continue;
		}
		/* a line of text each way every second once online */
		if (online && i > online + 50 && (i - online) % 50 == 0 &&
		    sent < 5) {
			if (write(sa[1], msg, strlen(msg)) < 0 ||
			    write(sb[1], msg, strlen(msg)) < 0)
				perror("write");
			sent++;
		}
		while ((n = read(sa[1], got_a + na, sizeof(got_a) - na)) > 0)
			na += n;
		while ((n = read(sb[1], got_b + nb, sizeof(got_b) - nb)) > 0)
			nb += n;
	}
	if (bulk && bulk_in < bulk) {
		printf("pair %d: only %ld of %ld bytes\n", p->id, bulk_in, bulk);
		p->fax_seen = 1;
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
	if (getenv("DMODEM_NET")) {
		const char *e = getenv("DMODEM_NET");
		net = !strcmp(e, "g711") ? NET_G711 : !strcmp(e, "rbs") ? NET_RBS :
			!strcmp(e, "pad") ? NET_PAD : !strcmp(e, "xlaw") ? NET_XLAW :
			NET_LINEAR;
	}
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
