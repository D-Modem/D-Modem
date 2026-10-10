/*
 *    dmodem_fax_test.c - libdmodem's fax: a fax machine calls a data
 *    modem answering (as DModem() does), which sees the CNG and hands the
 *    call to a fax receiver
 *
 *    dmodem_fax_test <in.tif> <out.tif> [ulaw|alaw]
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dmodem.h"

int main(int argc, char **argv)
{
	struct dmodem_config mc = { 0 };
	struct dmodem_fax_config fc = { 0 };
	struct dmodem *modem;
	struct dmodem_fax *sender, *receiver = NULL;
	struct dmodem_status ms;
	struct dmodem_fax_status ss, rs;
	unsigned char ab[160], ba[160], oa[160], ob[160];
	enum dmodem_law law = DMODEM_LAW_ULAW;
	int dte[2], n, handed = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: dmodem_fax_test <in.tif> <out.tif> [alaw]\n");
		return 2;
	}
	if (argc > 3 && !strcmp(argv[3], "alaw"))
		law = DMODEM_LAW_ALAW;
	memset(ab, law == DMODEM_LAW_ALAW ? 0xd5 : 0xff, sizeof(ab));
	memcpy(ba, ab, sizeof(ba));
	dmodem_init();
	if (getenv("DMODEM_DEBUG"))
		dmodem_set_debug(atoi(getenv("DMODEM_DEBUG")));

	/* the answering side: a data modem first */
	socketpair(AF_UNIX, SOCK_STREAM, 0, dte);
	fcntl(dte[0], F_SETFL, O_NONBLOCK);
	fcntl(dte[1], F_SETFL, O_NONBLOCK);
	mc.name = "answer";
	mc.dte_fd = dte[0];
	mc.law = law;
	mc.init = "ATE0Q1S0=0\r";
	modem = dmodem_create(&mc);
	dmodem_answer(modem);

	fc.law = law;
	fc.dir = DMODEM_FAX_SEND;
	fc.file = argv[1];
	fc.ident = "+1 555 0100";
	sender = dmodem_fax_create(&fc);
	if (!sender) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}

	for (n = 0; n < 50 * 900; n++) {
		dmodem_fax_process_g711(sender, ba, oa, 160);
		if (!receiver) {
			dmodem_process_g711(modem, ab, ob, 160);
			dmodem_status(modem, &ms);
			if (ms.fax_caller) {
				printf("fax caller seen after %.1f s\n", n / 50.0);
				dmodem_destroy(modem);
				modem = NULL;
				fc.dir = DMODEM_FAX_RECEIVE;
				fc.file = argv[2];
				fc.ident = "+1 555 0199";
				receiver = dmodem_fax_create(&fc);
				handed = 1;
				memset(ob, law == DMODEM_LAW_ALAW ? 0xd5 : 0xff, 160);
			}
		} else {
			dmodem_fax_process_g711(receiver, ab, ob, 160);
		}
		memcpy(ab, oa, sizeof(ab));
		memcpy(ba, ob, sizeof(ba));
		dmodem_fax_status(sender, &ss);
		if (receiver) {
			dmodem_fax_status(receiver, &rs);
			if (ss.done && rs.done)
				break;
		} else if (ss.done) {
			break;
		}
	}
	printf("sender: %s, %d pages at %d bit/s to '%s'\n", ss.result,
	       ss.pages, ss.bps, ss.remote_ident);
	if (receiver)
		printf("receiver: %s, %d pages from '%s'\n", rs.result,
		       rs.pages, rs.remote_ident);
	dmodem_fax_destroy(sender);
	dmodem_fax_destroy(receiver);
	if (modem)
		dmodem_destroy(modem);
	return handed && ss.ok && rs.ok ? 0 : 1;
}
