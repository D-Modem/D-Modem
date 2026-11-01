/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dsvd_audio_test.cpp - dmodemd's --dsvd-audio (dsvd_audio.h): cat
 *                          as the command, what goes to it comes back;
 *                          started and stopped with the voice channel,
 *                          a command that ends is not waited for
 *
 *    dsvd_audio_test
 *
 */

#include <signal.h>
#include <time.h>

#include <vector>

#include <gtest/gtest.h>

#include "dsvd_audio.h"

/* a millisecond's pause */
static void pause_ms()
{
	const struct timespec ts = { 0, 1000000 };
	nanosleep(&ts, nullptr);
}

/* up to n samples back from audio, waiting up to about a second */
static std::vector<s16> collect(dmd::DsvdAudio &audio, size_t n)
{
	std::vector<s16> got;
	for (int i = 0; i < 1000 && got.size() < n; i++) {
		s16 buf[80];
		const s16 k = audio.mic(buf, 80);
		got.insert(got.end(), buf, buf + k);
		if (!k)
			pause_ms();
	}
	return got;
}

/* cat: the ear's samples are the microphone's, in order */
TEST(DsvdAudio, Loopback)
{
	dmd::DsvdAudio audio("exec cat");
	s16 none[80];
	EXPECT_EQ(audio.mic(none, 80), 0);  /* closed: nothing */
	audio.state(true);
	std::vector<s16> sent;
	for (int f = 0; f < 10; f++) {
		s16 pcm[80];
		for (int i = 0; i < 80; i++)
			pcm[i] = static_cast<s16>((f * 80 + i) * 37 - 15000);
		audio.ear(pcm, 80);
		sent.insert(sent.end(), pcm, pcm + 80);
	}
	EXPECT_EQ(collect(audio, sent.size()), sent);
	audio.state(false);
	EXPECT_EQ(audio.mic(none, 80), 0);

	/* again, a second channel */
	audio.state(true);
	const s16 one[3] = { 1, -2, 3 };
	audio.ear(one, 3);
	EXPECT_EQ(collect(audio, 3), std::vector<s16>(one, one + 3));
	audio.state(false);
}

/* a command that ends at once: the ear's samples are dropped (with
   SIGPIPE ignored, as dmodemd has it), the microphone silent */
TEST(DsvdAudio, CommandEnds)
{
	signal(SIGPIPE, SIG_IGN);
	dmd::DsvdAudio audio("exit 0");
	audio.state(true);
	s16 pcm[80] = {};
	for (int i = 0; i < 50; i++) {
		audio.ear(pcm, 80);
		pause_ms();
	}
	EXPECT_EQ(collect(audio, 1).size(), 0u);
	audio.state(false);
}
