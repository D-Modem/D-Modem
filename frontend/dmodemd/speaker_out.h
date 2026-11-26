/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    speaker_out.h - the modem's speaker on a sound device (dmodemd's
 *                    --speaker): the call's audio, as ATM and ATL set
 *                    it, on the computer's own speakers
 *
 *    The core (dcore::SpeakerPath) gives the volume as it changes and,
 *    while the speaker is on, each block of the line's samples: what
 *    came and what the modem sent.  The two are mixed, as on a modem
 *    whose speaker is on the line, at ATL's volume (L1 -12 dB, L2
 *    -6 dB, L3 0 dB).  Nothing here blocks the line's thread: samples
 *    that the device cannot take now are dropped.
 *
 *    The device, by system:
 *      Linux    an ALSA PCM (default, plughw:0, ...), opened at once
 *               (as root, before dmodemd goes on as nobody); its rate
 *               set when the speaker first comes on
 *      Windows  a waveOut device: default (the wave mapper), its
 *               number (0, 1, ...), or a part of its name
 *      Haiku    default: the Media Kit's system mixer (BSoundPlayer)
 *
 */
#ifndef DMODEMD_SPEAKER_OUT_H
#define DMODEMD_SPEAKER_OUT_H

#include <atomic>
#include <vector>

#include <dcore.h>

#include "dm_types.h"

namespace dmd {

/* this build has a speaker back end (ALSA, waveOut or the Media Kit) */
bool speaker_supported();

class SpeakerOut final : public dcore::SpeakerPath {
public:
	/* no device yet (open()) */
	SpeakerOut();
	/* the device closed */
	~SpeakerOut();
	SpeakerOut(const SpeakerOut &) = delete;
	SpeakerOut &operator=(const SpeakerOut &) = delete;

	/* the device named (see above) found or opened; false, with an
	   error on stderr, if it cannot be */
	bool open(const char *device);

	/* see dcore::SpeakerPath: on (level 1 to 3) or off (0) */
	void volume(s16 level, s32 rate) override;
	/* see dcore::SpeakerPath: rx and tx mixed to the device */
	void audio(const s16 *rx, const s16 *tx, s32 n) override;

	/* (the Media Kit's thread: the next samples it plays) */
	void pull(s16 *pcm, s32 n);

private:
	/* the back end at rate, started (or started again); false if it
	   cannot be */
	bool start(s32 rate);
	/* the back end stopped: what it still has dropped */
	void stop();
	/* the mixed samples to the back end */
	void write(const s16 *pcm, s32 n);

	s16 level_ = 0;                 /* ATL's, 0 off */
	s32 rate_ = 0;                  /* the back end's, 0 not set */
	bool failed_ = false;           /* the back end refused: no more */
	std::vector<s16> mix_;          /* audio()'s mixed samples */

	/* ALSA's snd_pcm_t (here in every build: only speaker_out.cpp
	   has DMODEM_SPEAKER_ALSA, and the class is the same for all) */
	void *pcm_ = nullptr;
#if defined(_WIN32)
	/* the device's number (WAVE_MAPPER for default), its handle; the
	   buffers queued in turn, the one being filled and how full */
	unsigned id_ = 0;
	void *wave_ = nullptr;          /* the HWAVEOUT */
	struct Buffer;
	std::vector<Buffer> bufs_;
	size_t fill_ = 0;
	s32 filled_ = 0;
#elif defined(__HAIKU__)
	/* the BSoundPlayer; a ring of samples from the line's thread
	   (head_, the writer's) to the Media Kit's (tail_, the reader's) */
	void *player_ = nullptr;
	std::vector<s16> ring_;
	std::atomic<u32> head_{0}, tail_{0};
	/* (the reader empties the ring first: the speaker came on again) */
	std::atomic<bool> flush_{false};
#endif
};

} // namespace dmd

#endif
