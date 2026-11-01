/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dsvd_audio.h - DSVD's voice through another program (dmodemd's
 *                   --dsvd-audio): a command started when a voice
 *                   channel opens, stopped when it closes
 *
 *    The command (run by /bin/sh -c) gets the far end's speech on its
 *    standard input and gives ours on its standard output, both raw
 *    16 bit signed little-endian samples, 8000 a second, one channel:
 *    for instance, with ALSA's tools,
 *
 *        aplay -q -t raw -f S16_LE -r 8000 & exec arecord -q -t raw -f S16_LE -r 8000
 *
 *    Neither side waits for the other: what the command does not take
 *    at once is dropped, what it has not given yet is not waited for.
 *
 */
#ifndef DMODEMD_DSVD_AUDIO_H
#define DMODEMD_DSVD_AUDIO_H

#include <sys/types.h>

#include "dcore.h"
#include "dm_types.h"

namespace dmd {

class DsvdAudio final : public dcore::VoicePath {
public:
	/* the command (kept: the caller's string) */
	explicit DsvdAudio(const char *command) : cmd_(command) {}
	/* the command stopped */
	~DsvdAudio() { stop(); }
	DsvdAudio(const DsvdAudio &) = delete;
	DsvdAudio &operator=(const DsvdAudio &) = delete;

	/* the voice channel open: the command started; closed: stopped */
	void state(bool on) override;
	/* the command's samples so far, up to n */
	s16 mic(s16 *pcm, s16 n) override;
	/* to the command */
	void ear(const s16 *pcm, s16 n) override;

private:
	bool start();
	void stop();

	const char *cmd_;
	pid_t pid_ = -1;
	int to_ = -1, from_ = -1;       /* its stdin, its stdout */
	int odd_ = -1;                  /* a sample's first octet read */
};

} // namespace dmd

#endif
