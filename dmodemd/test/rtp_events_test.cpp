/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    rtp_events_test.cpp - the far end's telephone-events as tones
 *                          (dmd::EventPlayer, rtp_events.h)
 *
 */

#include <gtest/gtest.h>

#include <math.h>

#include <complex>
#include <vector>

#include "rtp_events.h"

using dmd::EventPlayer;

namespace {

const unsigned RATE = 8000, FRAME = 160;

/* an event packet's payload */
std::vector<uint8_t> pkt(int event, bool end, int volume, unsigned dur)
{
	return { (uint8_t)event, (uint8_t)((end ? 0x80 : 0) | volume),
		 (uint8_t)(dur >> 8), (uint8_t)dur };
}

/* an event packet to p at ts: the kind it began (NONE if
   none) */
EventPlayer::Kind send(EventPlayer &p, int event, bool end, int volume,
		       unsigned dur, uint32_t ts)
{
	std::vector<uint8_t> b = pkt(event, end, volume, dur);
	return p.packet(b.data(), b.size(), ts).kind;
}

/* n frames of what plays (silence where nothing does) */
std::vector<int16_t> frames(EventPlayer &p, int n)
{
	std::vector<int16_t> v(n * FRAME, 0);
	for (int i = 0; i < n; i++)
		p.fill(&v[i * FRAME], FRAME);
	return v;
}

/* x's component at f (complex amplitude, over [a, b)) */
std::complex<double> at(const std::vector<int16_t> &x, double f, size_t a,
			size_t b)
{
	std::complex<double> s = 0;
	for (size_t i = a; i < b; i++)
		s += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / RATE);
	return s * 2.0 / (double)(b - a);
}

/* x's rms over [a, b) */
double rms(const std::vector<int16_t> &x, size_t a, size_t b)
{
	double e = 0;
	for (size_t i = a; i < b; i++)
		e += (double)x[i] * x[i];
	return sqrt(e / (b - a));
}

/* a sine's rms at dbm0 */
double rms_dbm0(double dbm0)
{
	return 22764.0 / sqrt(2.0) * pow(10.0, dbm0 / 20.0);
}

} // namespace

/* a digit: its two tones at the volume's power (split between them),
   until its end packet; a copy of the end starts nothing */
TEST(RtpEvents, DtmfDigitLevelAndEnd)
{
	EventPlayer p;
	EXPECT_EQ(send(p, 5, false, 10, 160, 1000), EventPlayer::PLAYED);
	std::vector<int16_t> x = frames(p, 5);
	EXPECT_NEAR(abs(at(x, 770, 0, x.size())), 22764 * pow(10, -13.01 / 20),
		    100);
	EXPECT_NEAR(abs(at(x, 1336, 0, x.size())), 22764 * pow(10, -13.01 / 20),
		    100);
	EXPECT_NEAR(rms(x, 0, x.size()), rms_dbm0(-10), 60);
	EXPECT_LT(abs(at(x, 697, 0, x.size())), 400);
	EXPECT_LT(abs(at(x, 1209, 0, x.size())), 400);
	/* 100 ms without a packet: it stopped; its next packet resumes it */
	EXPECT_FALSE(p.playing());
	EXPECT_EQ(send(p, 5, false, 10, 800, 1000), EventPlayer::NONE);
	EXPECT_TRUE(p.playing());
	EXPECT_EQ(send(p, 5, true, 10, 960, 1000), EventPlayer::NONE);
	EXPECT_FALSE(p.playing());
	EXPECT_EQ(send(p, 5, true, 10, 960, 1000), EventPlayer::NONE);
	EXPECT_FALSE(p.playing());
	int16_t f[FRAME] = {};
	EXPECT_FALSE(p.fill(f, FRAME));
	/* the same digit again, a new event: played */
	EXPECT_EQ(send(p, 5, false, 10, 160, 3000), EventPlayer::PLAYED);
	EXPECT_TRUE(p.playing());
}

/* every digit is its key's pair */
TEST(RtpEvents, DtmfDigitsPairs)
{
	const double lo[] = { 941, 697, 697, 697, 770, 770, 770, 852, 852,
			      852, 941, 941, 697, 770, 852, 941 };
	const double hi[] = { 1336, 1209, 1336, 1477, 1209, 1336, 1477, 1209,
			      1336, 1477, 1209, 1477, 1633, 1633, 1633, 1633 };
	for (int d = 0; d < 16; d++) {
		EventPlayer p;
		ASSERT_EQ(send(p, d, false, 10, 160, 7), EventPlayer::PLAYED);
		std::vector<int16_t> x = frames(p, 3);
		EXPECT_GT(abs(at(x, lo[d], 0, x.size())), 4000) << d;
		EXPECT_GT(abs(at(x, hi[d], 0, x.size())), 4000) << d;
	}
}

/* without packets a digit plays on for 100 ms, a tone for 300 ms */
TEST(RtpEvents, HoldWithoutPackets)
{
	EventPlayer p;
	send(p, 1, false, 10, 160, 1);
	int frames_on = 0;
	int16_t f[FRAME];
	while (p.fill(f, FRAME) && frames_on < 100)
		frames_on++;
	EXPECT_EQ(frames_on, 5);
	send(p, 32, false, 12, 160, 2);
	frames_on = 0;
	while (p.fill(f, FRAME) && frames_on < 100)
		frames_on++;
	EXPECT_EQ(frames_on, 15);
}

/* ANS at its volume, a volume of 0 its nominal -12 dBm0; then /ANS:
   the same tone, its phase reversed at its start and every 450 ms */
TEST(RtpEvents, AnsThenSlashAnsPhaseReversals)
{
	EventPlayer p;
	ASSERT_EQ(send(p, 32, false, 0, 160, 100), EventPlayer::PLAYED);
	std::vector<int16_t> x;
	for (int i = 0; i < 10; i++) {          /* 200 ms of ANS */
		send(p, 32, false, 0, 160 * (i + 1), 100);
		std::vector<int16_t> f = frames(p, 1);
		x.insert(x.end(), f.begin(), f.end());
	}
	EXPECT_NEAR(rms(x, 0, x.size()), rms_dbm0(-12), 50);
	ASSERT_EQ(send(p, 32, true, 0, 1600, 100), EventPlayer::NONE);
	ASSERT_EQ(send(p, 33, false, 0, 160, 1700), EventPlayer::PLAYED);
	for (int i = 0; i < 60; i++) {          /* 1.2 s of /ANS */
		send(p, 33, false, 0, 160 * (i + 1), 1700);
		std::vector<int16_t> f = frames(p, 1);
		x.insert(x.end(), f.begin(), f.end());
	}
	/* the phase of 2100 Hz over 10 ms blocks; its jumps */
	std::vector<double> ph;
	for (size_t a = 0; a + 80 <= x.size(); a += 80)
		ph.push_back(arg(at(x, 2100, a, a + 80)));
	std::vector<size_t> jumps;
	for (size_t i = 1; i < ph.size(); i++) {
		double d = remainder(ph[i] - ph[i - 1], 2 * M_PI);
		if (fabs(d) > M_PI / 2)
			jumps.push_back(i);
	}
	/* at 200 ms (/ANS's start: block 20, its first sample in block
	   20 or 21), then 450 ms and 900 ms on */
	ASSERT_EQ(jumps.size(), 3u);
	EXPECT_NEAR((double)jumps[0], 20.5, 0.6);
	EXPECT_NEAR((double)(jumps[1] - jumps[0]), 45, 1);
	EXPECT_NEAR((double)(jumps[2] - jumps[1]), 45, 1);
}

/* ANSam: 2100 Hz between 0.8 and 1.2 of its amplitude at 15 Hz */
TEST(RtpEvents, AnsamModulation)
{
	EventPlayer p;
	send(p, 34, false, 12, 160, 5);
	std::vector<int16_t> x;
	for (int i = 0; i < 25; i++) {
		send(p, 34, false, 12, 160 * (i + 1), 5);
		std::vector<int16_t> f = frames(p, 1);
		x.insert(x.end(), f.begin(), f.end());
	}
	/* the envelope over 5 ms blocks (a 15 Hz period is 66.7 ms) */
	double lo = 1e9, hi = 0;
	for (size_t a = 0; a + 40 <= x.size(); a += 40) {
		double r = rms(x, a, a + 40);
		lo = std::min(lo, r);
		hi = std::max(hi, r);
	}
	EXPECT_NEAR(hi / lo, 1.5, 0.08);
	/* 15 Hz: the envelope's minima 66.7 ms apart */
	EXPECT_NEAR(rms(x, 0, x.size()), rms_dbm0(-12) * sqrt(1 + 0.02),
		    60);
}

/* the other tones at their frequencies */
TEST(RtpEvents, Tones)
{
	struct { int event; double f1, f2; } cases[] = {
		{ 36, 1100, 0 }, { 49, 1300, 0 }, { 52, 2225, 0 },
		{ 37, 1180, 0 }, { 38, 980, 0 }, { 39, 1850, 0 },
		{ 40, 1650, 0 }, { 23, 1900, 0 }, { 24, 400, 0 },
		{ 25, 1150, 0 }, { 26, 650, 0 }, { 28, 1375, 2002 },
		{ 29, 1529, 2225 },
	};
	for (const auto &c : cases) {
		EventPlayer p;
		ASSERT_EQ(send(p, c.event, false, 13, 160, 9),
			  EventPlayer::PLAYED) << c.event;
		std::vector<int16_t> x = frames(p, 4);
		double want = c.f2 > 0 ? rms_dbm0(-16.01) : rms_dbm0(-13);
		EXPECT_NEAR(abs(at(x, c.f1, 0, x.size())) / sqrt(2.0), want,
			    want * 0.05) << c.event;
		if (c.f2 > 0) {
			EXPECT_NEAR(abs(at(x, c.f2, 0, x.size())) / sqrt(2.0),
				    want, want * 0.05) << c.event;
		}
		EXPECT_NEAR(rms(x, 0, x.size()), rms_dbm0(-13), 60) << c.event;
	}
}

/* the flash, the events of modulated signals and unknown ones are not
   played, told once each, and leave a tone that plays as it was */
TEST(RtpEvents, NotPlayed)
{
	EventPlayer p;
	EXPECT_EQ(send(p, 16, false, 0, 160, 1), EventPlayer::FLASH);
	EXPECT_FALSE(p.playing());
	EXPECT_EQ(send(p, 16, true, 0, 800, 1), EventPlayer::NONE);
	EXPECT_EQ(send(p, 99, false, 0, 160, 2), EventPlayer::UNKNOWN);
	EXPECT_FALSE(p.playing());
	for (int e : { 27, 30, 31, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
		       63 }) {
		EventPlayer q;
		EXPECT_EQ(send(q, e, false, 0, 160, 3), EventPlayer::NOT_PLAYED)
			<< e;
		EXPECT_FALSE(q.playing()) << e;
	}
	/* CI reported while ANSam plays: ANSam goes on, its phase too */
	send(p, 34, false, 12, 160, 50);
	std::vector<int16_t> a = frames(p, 2);
	EXPECT_EQ(send(p, 53, false, 0, 160, 900), EventPlayer::NOT_PLAYED);
	EXPECT_EQ(send(p, 53, false, 0, 320, 900), EventPlayer::NONE);
	EXPECT_EQ(send(p, 34, false, 12, 480, 50), EventPlayer::NONE);
	EXPECT_TRUE(p.playing());
	std::vector<int16_t> b = frames(p, 1);
	/* no step in the phase of 2100 Hz across the CI */
	double d = remainder(arg(at(b, 2100, 0, 80)) -
			     arg(at(a, 2100, 240, 320)) -
			     2 * M_PI * 2100 * 80.0 / RATE, 2 * M_PI);
	EXPECT_LT(fabs(d), 0.2);
}

/* too short a payload is nothing; reset() forgets */
TEST(RtpEvents, ShortAndReset)
{
	EventPlayer p;
	uint8_t b[3] = { 5, 10, 0 };
	EXPECT_EQ(p.packet(b, 3, 1).kind, EventPlayer::NONE);
	send(p, 5, false, 10, 160, 1);
	EXPECT_TRUE(p.playing());
	p.reset();
	EXPECT_FALSE(p.playing());
	EXPECT_EQ(send(p, 5, false, 10, 320, 1), EventPlayer::PLAYED);
}
