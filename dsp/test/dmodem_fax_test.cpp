/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem_fax_test.cpp - libdmodem's fax: a fax machine calls a data
 *    modem answering (as DModem() does), which sees the CNG and hands the
 *    call to a fax receiver; the pages must come through, mu-law and A-law
 *
 *    DMODEM_DEBUG=<level>   libdmodem's log
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dmodem.h"
#include "fax_util.h"

class DmodemFax : public testing::TestWithParam<enum dmodem_law> {};

/* the fax sender calls a data modem answering, 20 ms frames of G.711
   each way (15 minutes of line at most); once the modem says a fax
   machine calls (fax_caller) a fax receiver takes its place on the
   line; both must end well, the two pages sent the same as received.
   Each side is complete (the receiver first, at its MCF) before it is
   done, the sender's DCN still to go, and the received file has the
   pages already then */
TEST_P(DmodemFax, DataModemHandsCallToFax)
{
	enum dmodem_law law = GetParam();
	std::string in = fax_file("in.tif"), out = fax_file("out.tif");
	struct dmodem_config mc = {};
	struct dmodem_fax_config fc = {};
	struct dmodem *modem;
	struct dmodem_fax *sender, *receiver = NULL;
	struct dmodem_status ms;
	struct dmodem_fax_status ss = {}, rs = {};
	unsigned char ab[160], ba[160], oa[160], ob[160];
	int dte[2], n;
	int s_complete = -1, s_done = -1, r_complete = -1;
	bool pages_at_complete = false;
	unsigned char idle = law == DMODEM_LAW_ALAW ? 0xd5 : 0xff;

	ASSERT_TRUE(write_test_doc(in, true));
	memset(ab, idle, sizeof(ab));
	memcpy(ba, ab, sizeof(ba));
	dmodem_init();
	if (getenv("DMODEM_DEBUG"))
		dmodem_set_debug(atoi(getenv("DMODEM_DEBUG")));

	/* the answering side: a data modem first */
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, dte), 0);
	fcntl(dte[0], F_SETFL, O_NONBLOCK);
	fcntl(dte[1], F_SETFL, O_NONBLOCK);
	mc.name = "answer";
	mc.dte_fd = dte[0];
	mc.law = law;
	mc.init = "ATE0Q1S0=0\r";
	modem = dmodem_create(&mc);
	ASSERT_NE(modem, nullptr);
	dmodem_answer(modem);

	fc.law = law;
	fc.dir = DMODEM_FAX_SEND;
	fc.file = in.c_str();
	fc.ident = "+1 555 0100";
	sender = dmodem_fax_create(&fc);
	ASSERT_NE(sender, nullptr) << "cannot read " << in;

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
				fc.file = out.c_str();
				fc.ident = "+1 555 0199";
				receiver = dmodem_fax_create(&fc);
				ASSERT_NE(receiver, nullptr);
				memset(ob, idle, 160);
			}
		} else {
			dmodem_fax_process_g711(receiver, ab, ob, 160);
		}
		memcpy(ab, oa, sizeof(ab));
		memcpy(ba, ob, sizeof(ba));
		dmodem_fax_status(sender, &ss);
		if (ss.complete && s_complete < 0)
			s_complete = n;
		if (ss.done && s_done < 0)
			s_done = n;
		if (receiver) {
			dmodem_fax_status(receiver, &rs);
			if (rs.complete && r_complete < 0) {
				r_complete = n;
				EXPECT_TRUE(rs.ok) << "receiver: " << rs.result;
				pages_at_complete = same_pages(in, out);
			}
			if (ss.done && rs.done)
				break;
		} else if (ss.done) {
			break;
		}
	}
	EXPECT_NE(receiver, nullptr) << "the modem never saw the fax caller";
	EXPECT_TRUE(ss.ok) << "sender: " << ss.result;
	EXPECT_TRUE(rs.ok) << "receiver: " << (receiver ? rs.result : "none");
	EXPECT_EQ(ss.pages, 2);
	EXPECT_TRUE(rs.done) << "receiver: no DCN";
	EXPECT_GE(r_complete, 0) << "receiver never complete";
	EXPECT_GT(s_complete, r_complete) << "sender complete before its MCF";
	EXPECT_GT(s_done, s_complete) << "sender done before its DCN";
	EXPECT_TRUE(pages_at_complete) << "the received file at complete";
	dmodem_fax_destroy(sender);
	dmodem_fax_destroy(receiver);
	if (modem)
		dmodem_destroy(modem);
	close(dte[0]);
	close(dte[1]);
	EXPECT_TRUE(same_pages(in, out));
}

INSTANTIATE_TEST_SUITE_P(Law, DmodemFax,
	testing::Values(DMODEM_LAW_ULAW, DMODEM_LAW_ALAW),
	/* the cases' names: ulaw, alaw */
	[](const testing::TestParamInfo<enum dmodem_law> &i) {
		return std::string(i.param == DMODEM_LAW_ALAW ? "alaw" : "ulaw");
	});
