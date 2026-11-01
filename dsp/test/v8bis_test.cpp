/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v8bis_test.cpp - V.8 bis's pieces (v8bis.cpp): the parameter trees
 *                     and messages against known encodings, the signals
 *                     through their detector, the messages through V.21
 *                     and HDLC
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

#include "v8bis.h"

using namespace dsp;
using namespace dsp::v8bis;

/* an MS for data with V.34 and V.42, ACK(1) asked for, the V.8 start-up:
   its octets as clause 8 has them */
TEST(V8bis, MsEncoding)
{
	Caps c;
	c.id = ID_V8 | ID_ACK1;
	c.modes = MODE_DATA;
	c.data = DATA_V34 | DATA_V42;
	std::vector<u8> m = msg_encode(msg_of(MS, c));
	/* type and revision; I: NPar(1), SPar(1); S: NPar(1), SPar(1),
	   the data mode's NPar(2) in two octets, bits 7 and 8 in the last */
	const std::vector<u8> want = { 0x21, 0x89, 0x80, 0x80, 0x81, 0x02,
				       0xD0 };
	EXPECT_EQ(m, want);
	Msg back;
	ASSERT_TRUE(msg_decode(m.data(), m.size(), &back));
	EXPECT_EQ(back.type, MS);
	EXPECT_EQ(back.rev, REVISION);
	Caps d = caps_of(back);
	EXPECT_EQ(d.id, c.id);
	EXPECT_EQ(d.modes, c.modes);
	EXPECT_EQ(d.data, c.data);
}

/* ACK and NAK: the one octet */
TEST(V8bis, AckNak)
{
	EXPECT_EQ(msg_encode(msg_of(ACK1, Caps())), std::vector<u8>{ 0x24 });
	EXPECT_EQ(msg_encode(msg_of(NAK3, Caps())), std::vector<u8>{ 0x2A });
	Msg m;
	const u8 nak1 = 0x18;
	ASSERT_TRUE(msg_decode(&nak1, 1, &m));
	EXPECT_EQ(m.type, NAK1);
	EXPECT_EQ(m.rev, 1);
	/* a type Table 3 has not got */
	const u8 bad = 0x2F;
	EXPECT_FALSE(msg_decode(&bad, 1, &m));
}

/* a CL with an H.324 mode ahead of what we read (its SPar(2) and
   NPar(3)), SVD and data: the trees parsed past what is not ours */
TEST(V8bis, TreeWithLevel3)
{
	Msg m;
	m.type = CL;
	m.id.npar1 = { ID_V8 };
	m.id.spar1 = { 0x01 };          /* the network type */
	m.id.par2.push_back({ { 0x02 }, {}, {} });      /* ISDN */
	m.s.npar1 = { 0 };
	m.s.spar1 = { MODE_DATA | MODE_SVD | MODE_H324 };
	m.s.par2.push_back({ { 0x06, 0x30, 0x0F }, {}, {} });
	m.s.par2.push_back({ { 0x19, 0x02 }, {}, {} });
	/* H.324: video and audio, the data SPar(2) and its NPar(3) */
	m.s.par2.push_back({ { 0x03 }, { 0x01 }, { { 0x05, 0x01 } } });
	std::vector<u8> e = msg_encode(m);
	Msg back;
	ASSERT_TRUE(msg_decode(e.data(), e.size(), &back));
	ASSERT_EQ(back.s.par2.size(), 3u);
	EXPECT_EQ(back.s.par2[2].npar2, std::vector<u8>{ 0x03 });
	EXPECT_EQ(back.s.par2[2].spar2, std::vector<u8>{ 0x01 });
	ASSERT_EQ(back.s.par2[2].npar3.size(), 1u);
	EXPECT_EQ(back.s.par2[2].npar3[0], (std::vector<u8>{ 0x05, 0x01 }));
	EXPECT_EQ(back.id.par2[0].npar2, std::vector<u8>{ 0x02 });
	Caps c = caps_of(back);
	EXPECT_EQ(c.data, DATA_V42 | DATA_V42BIS | DATA_V34 | DATA_V32BIS |
		  DATA_V32 | DATA_V22BIS | DATA_V22 | DATA_V21);
	EXPECT_EQ(c.svd, SVD_V70 | SVD_V34 | SVD_V32BIS | SVD_V42);
	/* cut short anywhere: not a message */
	for (size_t n = 1; n < e.size(); n++)
		EXPECT_FALSE(msg_decode(e.data(), n, &back)) << n;
}

/* each signal from its station, through the detector of the other: told
   apart, at MRe's low level too; the other pair is not heard */
TEST(V8bis, Signals)
{
	struct Case {
		u8 sig;
		bool initiating;
	};
	static const Case cases[] = {
		{ SIG_MRE, true }, { SIG_MRD, true }, { SIG_CRE, true },
		{ SIG_CRD, true }, { SIG_MRD, false }, { SIG_CRD, false },
	};
	const s32 srate = 8000;
	for (const Case &c : cases) {
		for (float amp : { 4225.0f, 1060.0f }) {
			SigTx tx;
			SigRx rx, other;
			tx.init(c.sig, c.initiating, amp, srate);
			rx.init(srate, c.initiating ? SigRx::PAIR_INIT :
				SigRx::PAIR_RESP);
			other.init(srate, c.initiating ? SigRx::PAIR_RESP :
				   SigRx::PAIR_INIT);
			u8 got = SIG_NONE, other_got = SIG_NONE;
			unsigned seed = 1;
			for (s32 i = 0; i < srate; i++) {
				seed = seed * 1103515245 + 12345;
				float noise = static_cast<float>(
					(seed >> 16) % 61) - 30;
				float x = tx.next() + noise;
				u8 s = rx.run(x);
				if (s != SIG_NONE)
					got = s;
				s = other.run(x);
				if (s != SIG_NONE)
					other_got = s;
			}
			EXPECT_EQ(got, c.sig) << sig_name(c.sig) << " at " <<
				amp;
			EXPECT_EQ(other_got, SIG_NONE) << sig_name(c.sig);
		}
	}
}

/* ESi: segment 1, then its message's preamble is segment 2 */
TEST(V8bis, EscapeAndMessage)
{
	const s32 srate = 8000;
	SigTx sig;
	MsgTx tx;
	SigRx srx;
	MsgRx mrx;
	sig.init(SIG_ESI, true, 4225, srate);
	tx.init(false, 4225, srate);
	srx.init(srate, SigRx::PAIR_INIT);
	mrx.init(false, srate);
	std::vector<u8> cl = msg_encode(msg_of(CL, Caps()));
	tx.send(cl);
	u8 got = SIG_NONE;
	for (s32 i = 0; i < 2 * srate; i++) {
		float x = !sig.done() ? sig.next() : tx.next();
		u8 s = srx.run(x);
		if (s != SIG_NONE)
			got = s;
		mrx.run(x);
	}
	EXPECT_EQ(got, SIG_ESI);
	ASSERT_EQ(mrx.msgs.size(), 1u);
	EXPECT_EQ(mrx.msgs[0], cl);
	EXPECT_EQ(mrx.bad, 0);
}

/* CL then MS straight after it (CL-MS), on each channel; a bit flipped
   in a frame is an invalid one */
TEST(V8bis, Messages)
{
	const s32 srate = 8000;
	Caps c;
	c.id = ID_V8;
	c.modes = MODE_DATA | MODE_SVD;
	c.data = DATA_V42 | DATA_V34;
	c.svd = SVD_V70 | SVD_V34 | SVD_V76;
	std::vector<u8> cl = msg_encode(msg_of(CL, c));
	c.modes = MODE_SVD;
	std::vector<u8> ms = msg_encode(msg_of(MS, c));
	for (bool high : { false, true }) {
		MsgTx tx;
		MsgRx rx, other;
		tx.init(high, 4225, srate);
		rx.init(high, srate);
		other.init(!high, srate);
		tx.send(cl);
		tx.send(ms);
		s32 n = 0;
		for (; n < 3 * srate && !tx.idle(); n++) {
			float x = tx.next();
			rx.run(x);
			other.run(x);
		}
		EXPECT_TRUE(tx.idle());
		/* 100 ms, 3 flags, the frame, 2 flags each: about 170
		   bits */
		EXPECT_LT(n, 360 * srate / 300);
		for (s32 i = 0; i < srate / 10; i++)
			rx.run(0), other.run(0);
		ASSERT_EQ(rx.msgs.size(), 2u) << high;
		EXPECT_EQ(rx.msgs[0], cl);
		EXPECT_EQ(rx.msgs[1], ms);
		EXPECT_EQ(rx.bad, 0);
		EXPECT_TRUE(other.msgs.empty());
		Msg m;
		ASSERT_TRUE(msg_decode(rx.msgs[1].data(), rx.msgs[1].size(),
				       &m));
		EXPECT_EQ(caps_of(m).svd, SVD_V70 | SVD_V34 | SVD_V76);
	}

	/* the FCS catches a bit flipped on the way: the frame's second
	   bit (a one: CL's type) sent as space */
	MsgTx tx;
	MsgRx rx;
	tx.init(false, 4225, srate);
	rx.init(false, srate);
	tx.send(cl);
	s32 from = (30 + 24 + 1) * srate / 300 + 4;
	for (s32 n = 0; n < srate && !tx.idle(); n++) {
		float x = tx.next();
		/* a tone at the other frequency for a moment */
		if (n >= from && n < from + 20)
			x = 4225 * std::sin(2 * 3.14159265f * 1180 * n /
					    srate);
		rx.run(x);
	}
	for (s32 i = 0; i < srate / 10; i++)
		rx.run(0);
	EXPECT_TRUE(rx.msgs.empty());
	EXPECT_EQ(rx.bad, 1);
}
