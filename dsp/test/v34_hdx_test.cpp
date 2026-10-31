/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v34_hdx_test.cpp - V.34 half-duplex (clause 12/V.34) back to back:
 *    two of v34.h's half-duplex modems through a line (gain, delay,
 *    noise), V.8 to the control channel, messages on it both ways, the
 *    source's primary channel twice with the control channel between
 *    (no modem core: the bits and the events through V34HdxHost)
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <deque>
#include <vector>

#include "dm_types.h"
#include "dsplibs.h"
#include "modem_dp.h"
#include "v34.h"
#include "test_util.h"

using namespace dsp;

/* the modem core the pumps link against: nothing registered, S
   registers and parameters 0, the bits all taken and given */
int modem_dp_register(enum DP_ID, struct dp_operations *) { return 0; }
/* (nothing to deregister) */
void modem_dp_deregister(enum DP_ID, struct dp_operations *) {}
/* an S register: 0 */
int modem_get_sreg(struct modem *, unsigned int) { return 0; }
/* a parameter set: 0 */
long modem_set_param(struct modem *, unsigned, int) { return 0; }
/* a parameter: 0 (none) */
intptr_t modem_get_param(struct modem *, unsigned) { return 0; }
/* the bits asked for: n */
int modem_get_bits(struct modem *, int, u8 *, int n) { return n; }
/* the bits given: n */
int modem_put_bits(struct modem *, int, u8 *, int n) { return n; }
/* no DTE */
int modem_send_to_tty(struct modem *, const char *, int n) { return n; }
/* (nothing from it) */
int modem_recv_from_tty(struct modem *, char *, int) { return 0; }

namespace {

/* DSP_DEBUG=<level>: the DSP's log to stderr */
const int debug_level = [] {
	const char *e = getenv("DSP_DEBUG");
	dsplibs_debug_level = e ? static_cast<unsigned>(atoi(e)) : 0;
	return 0;
}();

/* one side's host: what it sends on each channel (ones when it has
   nothing; the primary's end after its bits), what it received, the
   events */
struct Host {
	std::deque<s16> tx[2];
	std::vector<u8> rx[2];
	std::vector<s16> events;
	bool primary_end = false;       /* -1 after the primary's bits */

	/* (V34HdxHost) */
	static s16 get_bit(void *ctx, s16 primary)
	{
		Host *h = static_cast<Host *>(ctx);
		if (h->tx[primary].empty())
			return primary && h->primary_end ? -1 : 1;
		s16 b = h->tx[primary].front();
		h->tx[primary].pop_front();
		return b;
	}
	/* (V34HdxHost) */
	static void put_bit(void *ctx, s16 primary, s16 bit)
	{
		static_cast<Host *>(ctx)->rx[primary].push_back(
			static_cast<u8>(bit));
	}
	/* (V34HdxHost) */
	static void event(void *ctx, s16 ev)
	{
		static_cast<Host *>(ctx)->events.push_back(ev);
	}
	/* the events of kind ev so far */
	int count(s16 ev) const
	{
		int n = 0;
		for (s16 e : events)
			n += e == ev;
		return n;
	}
	/* a message on channel k: a 0 to find it by (after ones), then its
	   bits */
	void send(int k, const std::vector<u8> &bits)
	{
		tx[k].push_back(0);
		for (u8 b : bits)
			tx[k].push_back(b);
	}
};

/* n bits of a pattern from seed */
std::vector<u8> pattern(int n, unsigned seed)
{
	std::vector<u8> v;
	unsigned r = seed | 1;
	for (int i = 0; i < n; i++) {
		r = r * 1103515245u + 12345u;
		v.push_back((r >> 16) & 1);
	}
	return v;
}

/* the bits of the message received from bit from on: those after the
   first 0 there, as many as want has; the bits that differ (all when
   there is no 0, or too few) */
int errors(const std::vector<u8> &rx, size_t from, const std::vector<u8> &want)
{
	size_t i = from;
	while (i < rx.size() && rx[i])
		i++;
	if (i >= rx.size() || rx.size() - i - 1 < want.size())
		return static_cast<int>(want.size());
	int e = 0;
	for (size_t k = 0; k < want.size(); k++)
		e += rx[i + 1 + k] != want[k];
	return e;
}

/* two modems, the caller and the answerer, on a line of 0.5 gain, 3 ms
   each way, white noise snr dB under the signal */
struct Call {
	Host host[2];
	V34Hdx *m[2] = { nullptr, nullptr };
	std::vector<float> line[2];     /* in flight to side k */
	size_t pos = 0;
	float snr;
	long t = 0;
	bool failed = false;

	/* the two modems, the caller sending (else the answerer) */
	Call(bool caller_sends, float snr_) : snr(snr_)
	{
		for (int k = 0; k < 2; k++) {
			V34HdxHost h = { &host[k], Host::get_bit,
					 Host::put_bit, Host::event };
			m[k] = v34_hdx_create(nullptr, k == 0, caller_sends,
					      9600, &h);
			line[k].assign(29 + 96, 0.0f);
		}
	}
	/* (the modems freed) */
	~Call()
	{
		for (V34Hdx *x : m)
			if (x)
				v34_hdx_free(x);
	}
	/* secs more of the call, or until done() */
	template <class Done> bool run(double secs, Done done)
	{
		const int B = 96;
		long end = t + static_cast<long>(secs * 9600);
		float noise = 0.5f * 2988 * powf(10, -snr / 20);
		while (t < end && !failed) {
			s16 in[2][B], out[2][B];
			for (int k = 0; k < 2; k++)
				for (int i = 0; i < B; i++) {
					float x = line[k][i] +
						noise * static_cast<float>(gauss());
					in[k][i] = static_cast<s16>(
						lrintf(std::max(-32767.0f,
						       std::min(32767.0f, x))));
				}
			for (int k = 0; k < 2; k++) {
				s16 r = v34_hdx_process(m[k], in[k], out[k], B);
				if (r == DPSTAT_ERROR || r == DPSTAT_CHANGEDP)
					failed = true;
			}
			for (int k = 0; k < 2; k++) {
				std::vector<float> &l = line[1 - k];
				l.erase(l.begin(), l.begin() + B);
				for (int i = 0; i < B; i++)
					l.push_back(0.5f * out[k][i]);
			}
			t += B;
			if (done())
				return true;
		}
		return false;
	}
};

/* a fax call: V.8, phase 2, the training, the control channel; a
   message each way on it; a page on the primary channel, then the
   control channel again with another message each way, and a second
   page */
void fax_call(bool caller_sends, float snr)
{
	Call c(caller_sends, snr);
	int src = caller_sends ? 0 : 1, rec = 1 - src;

	ASSERT_TRUE(c.run(20, [&] {
		return c.host[0].count(V34HDX_CC_UP) &&
			c.host[1].count(V34HDX_CC_UP);
	})) << "no control channel";
	EXPECT_TRUE(v34_hdx_source(c.m[src]));
	EXPECT_FALSE(v34_hdx_source(c.m[rec]));
	s32 rate = v34_hdx_rate(c.m[src]);
	EXPECT_GE(rate, 21600);
	EXPECT_EQ(rate, v34_hdx_rate(c.m[rec]));

	for (int round = 0; round < 2; round++) {
		/* the control channel both ways */
		std::vector<u8> a = pattern(400, 1 + round),
				b = pattern(400, 7 + round);
		size_t at[2] = { c.host[0].rx[0].size(),
				 c.host[1].rx[0].size() };
		c.host[src].send(0, a);
		c.host[rec].send(0, b);
		c.run(1.5, [] { return false; });
		ASSERT_FALSE(c.failed);
		EXPECT_EQ(errors(c.host[rec].rx[0], at[rec], a), 0)
			<< "round " << round;
		EXPECT_EQ(errors(c.host[src].rx[0], at[src], b), 0)
			<< "round " << round;

		/* a page on the primary channel */
		std::vector<u8> page = pattern(rate * 3, 11 + round);
		size_t pat = c.host[rec].rx[1].size();
		for (int i = 0; i < 64; i++)
			c.host[src].tx[1].push_back(1);
		c.host[src].send(1, page);
		c.host[src].primary_end = true;
		v34_hdx_primary(c.m[src]);
		int ups = c.host[rec].count(V34HDX_CC_UP);
		ASSERT_TRUE(c.run(10, [&] {
			return c.host[rec].count(V34HDX_CC_UP) > ups &&
				c.host[src].count(V34HDX_CC_UP) > ups;
		})) << "no control channel after page " << round;
		EXPECT_EQ(c.host[rec].count(V34HDX_PRIMARY_UP), round + 1);
		EXPECT_EQ(c.host[rec].count(V34HDX_PRIMARY_DOWN), round + 1);
		EXPECT_EQ(errors(c.host[rec].rx[1], pat, page), 0)
			<< "page " << round;
		c.host[src].primary_end = false;
	}
}

/* the caller sends a fax */
TEST(V34Hdx, CallerSends)
{
	fax_call(true, 35);
}

/* the answerer sends: the caller polled */
TEST(V34Hdx, AnswererSendsToAPoll)
{
	fax_call(false, 35);
}

} // namespace
