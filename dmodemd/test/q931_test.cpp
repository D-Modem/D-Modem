/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    q931_test.cpp - the user side of ISDN's call control (q931.h)
 *                    against the network's messages, written out here
 *                    octet by octet
 *
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "q931.h"

using dmd::Q931;

namespace {

using Msg = std::vector<u8>;

/* what the call control did */
struct User : Q931::User {
	std::vector<Msg> sent;
	std::vector<s16> channels;
	std::vector<Q931::Call> offered;
	int connected = 0;
	std::vector<s16> ended;

	void q931_send(const u8 *m, s16 n) override {
		sent.emplace_back(m, m + n);
	}
	void q931_channel(s16 b) override { channels.push_back(b); }
	void q931_offered(const Q931::Call &c) override {
		offered.push_back(c);
	}
	void q931_connected() override { connected++; }
	void q931_ended(s16 cause) override { ended.push_back(cause); }
};

/* a message: call reference cr with flag (0x80: from the side that
   did not make the call), type t, elements */
Msg msg(u8 cr, u8 t, Msg ies = {})
{
	Msg m = { 0x08, 1, cr, t };
	m.insert(m.end(), ies.begin(), ies.end());
	return m;
}

void rx(Q931 &q, const Msg &m, u64 now = 0)
{
	q.receive(m.data(), static_cast<s16>(m.size()), now);
}

/* a cause element */
Msg cause(u8 c)
{
	return { 0x08, 2, 0x80, static_cast<u8>(0x80 | c) };
}

/* the network's SETUP for MSN called on B-channel b, bearer bc */
Msg setup(u8 cr, const std::string &called, u8 b, Msg bc)
{
	Msg ies = bc;
	ies.insert(ies.end(), { 0x18, 1, static_cast<u8>(0x88 | b) });
	ies.insert(ies.end(), { 0x6C, 5, 0x21, 0x80, '1', '2', '3' });
	ies.push_back(0x70);
	ies.push_back(static_cast<u8>(called.size() + 1));
	ies.push_back(0x81);
	ies.insert(ies.end(), called.begin(), called.end());
	return msg(cr, Q931::SETUP, ies);
}

const Msg AUDIO = { 0x04, 3, 0x90, 0x90, 0xA3 };
const Msg UDI = { 0x04, 2, 0x88, 0x90 };

} // namespace

/* our call, answered: SETUP with the bearer, any channel, the numbers
   and sending complete; the network's channel; CONNECT ACKNOWLEDGE */
TEST(Q931, Dial)
{
	User u;
	Q931 q(u);
	q.set_msn("4711");
	ASSERT_EQ(q.dial("T0,30-1234", Q931::Bearer::Udi, 0), 0);
	ASSERT_EQ(u.sent.size(), 1u);
	EXPECT_EQ(u.sent[0], (Msg{ 0x08, 1, 0x01, 0x05,
				   0x04, 2, 0x88, 0x90,
				   0x18, 1, 0x83,
				   0x6C, 5, 0x81, '4', '7', '1', '1',
				   0x70, 8, 0x81, '0', '3', '0', '1', '2',
				   '3', '4',
				   0xA1 }));
	EXPECT_EQ(q.state(), Q931::U1);
	EXPECT_EQ(q.dial("123", Q931::Bearer::Audio, 0), -1);

	rx(q, msg(0x81, Q931::CALL_PROCEEDING, { 0x18, 1, 0x89 }));
	EXPECT_EQ(q.state(), Q931::U3);
	EXPECT_EQ(u.channels, std::vector<s16>{ 1 });
	rx(q, msg(0x81, Q931::ALERTING));
	EXPECT_EQ(q.state(), Q931::U4);
	rx(q, msg(0x81, Q931::CONNECT));
	EXPECT_EQ(q.state(), Q931::U10);
	EXPECT_EQ(u.connected, 1);
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::CONNECT_ACK));
	EXPECT_EQ(u.channels.size(), 1u);
	EXPECT_TRUE(u.ended.empty());

	/* we hang up: DISCONNECT, the network's RELEASE, our RELEASE
	   COMPLETE; nothing said back */
	q.hangup(100);
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::DISCONNECT, cause(16)));
	EXPECT_EQ(q.state(), Q931::U11);
	rx(q, msg(0x81, Q931::RELEASE));
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::RELEASE_COMPLETE));
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_TRUE(u.ended.empty());

	/* the next call's reference is another */
	ASSERT_EQ(q.dial("5", Q931::Bearer::Audio, 0), 0);
	EXPECT_EQ(u.sent.back()[2], 0x02);
	EXPECT_EQ(u.sent.back()[4], 0x04);
	EXPECT_EQ(u.sent.back()[5], 3);
}

/* busy: the network's DISCONNECT with cause 17, our RELEASE, its
   RELEASE COMPLETE */
TEST(Q931, Busy)
{
	User u;
	Q931 q(u);
	ASSERT_EQ(q.dial("5551000", Q931::Bearer::Audio, 0), 0);
	rx(q, msg(0x81, Q931::DISCONNECT, cause(17)));
	EXPECT_EQ(u.ended, std::vector<s16>{ 17 });
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::RELEASE, cause(16)));
	EXPECT_EQ(q.state(), Q931::U19);
	EXPECT_TRUE(q.idle());
	rx(q, msg(0x81, Q931::RELEASE_COMPLETE));
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_EQ(u.ended.size(), 1u);
}

/* no answer to SETUP: sent again after T303, then given up */
TEST(Q931, T303)
{
	User u;
	Q931 q(u);
	ASSERT_EQ(q.dial("5551000", Q931::Bearer::Audio, 1000), 0);
	EXPECT_EQ(q.next_timer_ms(1000), Q931::T303);
	q.poll(1000 + Q931::T303 - 1);
	EXPECT_EQ(u.sent.size(), 1u);
	q.poll(1000 + Q931::T303);
	ASSERT_EQ(u.sent.size(), 2u);
	EXPECT_EQ(u.sent[0], u.sent[1]);
	q.poll(1000 + 2 * Q931::T303);
	EXPECT_EQ(u.ended, std::vector<s16>{ Q931::TIMER_EXPIRY });
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_EQ(q.next_timer_ms(9000), -1);
}

/* our DISCONNECT unanswered: RELEASE after T305, twice after T308,
   then the null state */
TEST(Q931, ClearingTimers)
{
	User u;
	Q931 q(u);
	ASSERT_EQ(q.dial("1", Q931::Bearer::Audio, 0), 0);
	rx(q, msg(0x81, Q931::CONNECT, { 0x18, 1, 0x8A }));
	EXPECT_EQ(u.channels, std::vector<s16>{ 2 });
	q.hangup(0);
	q.poll(Q931::T305);
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::RELEASE, cause(16)));
	EXPECT_EQ(q.state(), Q931::U19);
	q.poll(Q931::T305 + Q931::T308);
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::RELEASE, cause(16)));
	q.poll(Q931::T305 + 2 * Q931::T308);
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_TRUE(u.ended.empty());
}

/* a call for us: ALERTING with its channel, ringing; we answer:
   CONNECT, the B-channel, CONNECT ACKNOWLEDGE; the far end hangs up */
TEST(Q931, Incoming)
{
	User u;
	Q931 q(u);
	q.set_msn("4711");
	rx(q, setup(0x05, "4711", 2, UDI));
	ASSERT_EQ(u.offered.size(), 1u);
	EXPECT_EQ(u.offered[0].bearer, Q931::Bearer::Udi);
	EXPECT_EQ(u.offered[0].calling, "123");
	EXPECT_FALSE(u.offered[0].withheld);
	EXPECT_EQ(u.offered[0].called, "4711");
	EXPECT_EQ(u.offered[0].channel, 2);
	EXPECT_EQ(u.sent.back(),
		  msg(0x85, Q931::ALERTING, { 0x18, 1, 0x8A }));
	EXPECT_EQ(q.state(), Q931::U7);
	EXPECT_TRUE(u.channels.empty());

	q.answer(0);
	EXPECT_EQ(u.sent.back(), msg(0x85, Q931::CONNECT));
	EXPECT_EQ(u.channels, std::vector<s16>{ 2 });
	EXPECT_EQ(q.state(), Q931::U8);
	rx(q, msg(0x05, Q931::CONNECT_ACK));
	EXPECT_EQ(q.state(), Q931::U10);
	EXPECT_EQ(u.connected, 1);

	rx(q, msg(0x05, Q931::DISCONNECT, cause(16)));
	EXPECT_EQ(u.ended, std::vector<s16>{ 16 });
	EXPECT_EQ(u.sent.back(), msg(0x85, Q931::RELEASE, cause(16)));
	rx(q, msg(0x05, Q931::RELEASE_COMPLETE));
	EXPECT_EQ(q.state(), Q931::U0);
}

/* calls not for us are left alone: another MSN, no B-channel, a bearer
   we don't take; with a call up, a busy RELEASE COMPLETE */
TEST(Q931, IncomingNotOurs)
{
	User u;
	Q931 q(u);
	q.set_msn("4711");
	rx(q, setup(0x05, "4712", 1, AUDIO));
	rx(q, setup(0x06, "4711", 0, AUDIO));
	rx(q, setup(0x07, "4711", 1, { 0x04, 2, 0x98, 0x90 }));
	EXPECT_TRUE(u.offered.empty());
	EXPECT_TRUE(u.sent.empty());

	/* a speech call to a longer number ending with the MSN */
	rx(q, setup(0x08, "0301234711", 1, { 0x04, 3, 0x80, 0x90, 0xA3 }));
	ASSERT_EQ(u.offered.size(), 1u);
	EXPECT_EQ(u.offered[0].bearer, Q931::Bearer::Speech);
	rx(q, setup(0x09, "4711", 2, AUDIO));
	EXPECT_EQ(u.offered.size(), 1u);
	EXPECT_EQ(u.sent.back(),
		  msg(0x89, Q931::RELEASE_COMPLETE, cause(17)));
}

/* the caller gives up while it rings; another terminal takes a call
   (RELEASE, non-selected user clearing) */
TEST(Q931, IncomingCleared)
{
	User u;
	Q931 q(u);
	rx(q, setup(0x05, "1", 1, AUDIO));
	rx(q, msg(0x05, Q931::DISCONNECT, cause(16)));
	EXPECT_EQ(u.ended, std::vector<s16>{ 16 });
	rx(q, msg(0x05, Q931::RELEASE));
	EXPECT_EQ(u.sent.back(), msg(0x85, Q931::RELEASE_COMPLETE));
	EXPECT_EQ(q.state(), Q931::U0);

	rx(q, setup(0x06, "1", 1, AUDIO));
	q.answer(0);
	rx(q, msg(0x06, Q931::RELEASE, cause(26)));
	EXPECT_EQ(u.ended, (std::vector<s16>{ 16, 26 }));
	EXPECT_EQ(u.sent.back(), msg(0x86, Q931::RELEASE_COMPLETE));
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_EQ(u.connected, 0);
}

/* CONNECT unacknowledged: DISCONNECT after T313 */
TEST(Q931, T313)
{
	User u;
	Q931 q(u);
	rx(q, setup(0x05, "1", 1, AUDIO));
	q.answer(0);
	q.poll(Q931::T313);
	EXPECT_EQ(u.ended, std::vector<s16>{ Q931::TIMER_EXPIRY });
	EXPECT_EQ(u.sent.back(),
		  msg(0x85, Q931::DISCONNECT, cause(Q931::TIMER_EXPIRY)));
	EXPECT_EQ(q.state(), Q931::U11);
}

/* the others: STATUS ENQUIRY, RESTART, a message for no call */
TEST(Q931, Housekeeping)
{
	User u;
	Q931 q(u);
	ASSERT_EQ(q.dial("1", Q931::Bearer::Audio, 0), 0);
	rx(q, msg(0x81, Q931::STATUS_ENQUIRY));
	EXPECT_EQ(u.sent.back(), msg(0x01, Q931::STATUS,
				     { 0x08, 2, 0x80, 0x9E, 0x14, 1, 1 }));
	rx(q, msg(0x82, Q931::CONNECT));
	EXPECT_EQ(u.sent.back(), msg(0x02, Q931::RELEASE_COMPLETE,
				     cause(Q931::INVALID_CALL_REF)));
	rx(q, msg(0x83, Q931::RELEASE));
	EXPECT_EQ(u.sent.back(), msg(0x03, Q931::RELEASE_COMPLETE));
	size_t before = u.sent.size();
	rx(q, msg(0x84, Q931::RELEASE_COMPLETE));
	EXPECT_EQ(u.sent.size(), before);
	EXPECT_EQ(q.state(), Q931::U1);

	/* RESTART on the global call reference */
	const Msg restart = { 0x08, 1, 0x00, Q931::RESTART,
			      0x79, 1, 0x87 };
	rx(q, restart);
	EXPECT_EQ(u.sent.back(), (Msg{ 0x08, 0, Q931::RESTART_ACK,
				       0x79, 1, 0x87 }));
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_EQ(u.ended, std::vector<s16>{ Q931::TEMPORARY_FAILURE });

	/* a STATUS saying the network has no call */
	ASSERT_EQ(q.dial("1", Q931::Bearer::Audio, 0), 0);
	rx(q, msg(0x82, Q931::STATUS, { 0x08, 2, 0x80, 0xE5, 0x14, 1, 0 }));
	EXPECT_EQ(q.state(), Q931::U0);
	EXPECT_EQ(u.ended.back(), 101);
}

/* the elements: codeset shifts, octet 3a, truncation */
TEST(Q931, Parse)
{
	Q931::Info info;
	const Msg m = { 0x08, 1, 0x01, Q931::SETUP,
			0x96, 0x6C, 2, 0x81, '9',         /* locking shift 6 */
			0x90, 0x08, 3, 0x00, 0x80, 0x91, /* back to 0 */
			0x6C, 4, 0x01, 0xA3, '7', '7' };
	ASSERT_TRUE(Q931::parse(m.data(), static_cast<s16>(m.size()), 4,
				info));
	EXPECT_EQ(info.cause, 17);
	EXPECT_EQ(info.calling_number, "77");
	EXPECT_TRUE(info.withheld);
	const Msg bad = { 0x08, 1, 0x01, Q931::SETUP, 0x70, 5, 0x81, '1' };
	EXPECT_FALSE(Q931::parse(bad.data(), static_cast<s16>(bad.size()), 4,
				 info));
}
