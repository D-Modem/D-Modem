/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    speaker_out.cpp - the modem's speaker on a sound device
 *                      (speaker_out.h): ALSA's PCM, waveOut or the
 *                      Media Kit's BSoundPlayer
 *
 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(DMODEM_SPEAKER_ALSA)
#include <alsa/asoundlib.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#elif defined(__HAIKU__)
#include <MediaDefs.h>
#include <SoundPlayer.h>
#endif

#include <log.h>

#include "speaker_out.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "speaker: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/* ATL's level as a gain, 256 for 0 dB: L1 -12 dB, L2 -6 dB, L3 0 dB */
s32 level_gain(s16 level)
{
	return level >= 3 ? 256 : level == 2 ? 128 : 64;
}

/* a sample of the line: what came (rx) and what the modem sent (tx)
   added, at gain (256 for 0 dB), clipped to 16 bits */
s16 mix_sample(s16 rx, s16 tx, s32 gain)
{
	s32 v = (static_cast<s32>(rx) + tx) * gain / 256;
	return static_cast<s16>(std::max<s32>(-32768, std::min<s32>(32767, v)));
}

#if defined(DMODEM_SPEAKER_ALSA)
/* the PCM's latency: what it can hold (us), and what it has before it
   starts (ms) */
const unsigned ALSA_LATENCY_US = 120000;
const s32 ALSA_START_MS = 40;

/* the snd_pcm_t of the void * that the header keeps */
snd_pcm_t *alsa_pcm(void *p)
{
	return static_cast<snd_pcm_t *>(p);
}
#elif defined(_WIN32)
/* waveOut's queue: BUFFERS buffers of BUFFER_MS each */
const size_t BUFFERS = 6;
const s32 BUFFER_MS = 20;

/* the HWAVEOUT of the void * that the header keeps */
HWAVEOUT wave_out(void *p)
{
	return static_cast<HWAVEOUT>(p);
}

/* a device's name has text in it, letters of either case */
bool name_has(const char *name, const char *text)
{
	size_t n = strlen(text);
	for (const char *p = name; *p; p++) {
		size_t i = 0;
		while (i < n && p[i] &&
		       tolower(static_cast<unsigned char>(p[i])) ==
		       tolower(static_cast<unsigned char>(text[i])))
			i++;
		if (i == n)
			return true;
	}
	return n == 0;
}
#elif defined(__HAIKU__)
/* the ring between the threads (a power of two); the most it keeps
   before the Media Kit's thread skips to its newest TRIM_MS (the two
   clocks drift apart), in ms */
const u32 RING = 8192;
const s32 BACKLOG_MS = 160;
const s32 TRIM_MS = 60;

/* the BSoundPlayer of the void * that the header keeps */
BSoundPlayer *sound_player(void *p)
{
	return static_cast<BSoundPlayer *>(p);
}

/* BSoundPlayer's callback: the buffer filled in its format (16 bit or
   float samples, one channel or more) from the speaker's ring */
void play_buffer(void *cookie, void *buffer, size_t size,
		 const media_raw_audio_format &f)
{
	SpeakerOut *s = static_cast<SpeakerOut *>(cookie);
	const u32 ch = f.channel_count ? f.channel_count : 1;
	const bool fl = f.format == media_raw_audio_format::B_AUDIO_FLOAT;
	const size_t one = fl ? sizeof(float) : sizeof(s16);
	size_t frames = size / (one * ch), done = 0;
	s16 chunk[256];

	if (!fl && f.format != media_raw_audio_format::B_AUDIO_SHORT) {
		memset(buffer, 0, size);
		return;
	}
	while (done < frames) {
		s32 k = static_cast<s32>(std::min<size_t>(256, frames - done));
		s->pull(chunk, k);
		for (s32 i = 0; i < k; i++)
			for (u32 c = 0; c < ch; c++) {
				size_t at = (done + i) * ch + c;
				if (fl)
					static_cast<float *>(buffer)[at] =
						chunk[i] / 32768.0f;
				else
					static_cast<s16 *>(buffer)[at] =
						chunk[i];
			}
		done += k;
	}
}
#endif

} // namespace

/* see speaker_out.h */
bool speaker_supported()
{
#if defined(DMODEM_SPEAKER_ALSA) || defined(_WIN32) || defined(__HAIKU__)
	return true;
#else
	return false;
#endif
}

#if defined(_WIN32)
/* a waveOut buffer: its header and its samples */
struct SpeakerOut::Buffer {
	WAVEHDR hdr;
	std::vector<s16> pcm;
};
#endif

/* see speaker_out.h */
SpeakerOut::SpeakerOut() = default;

/* see speaker_out.h */
SpeakerOut::~SpeakerOut()
{
	stop();
#if !defined(DMODEM_SPEAKER_ALSA)
	(void)pcm_;                     /* (ALSA's alone) */
#endif
#if defined(DMODEM_SPEAKER_ALSA)
	if (pcm_)
		snd_pcm_close(alsa_pcm(pcm_));
#elif defined(_WIN32)
	if (wave_) {
		for (Buffer &b : bufs_)
			waveOutUnprepareHeader(wave_out(wave_), &b.hdr,
					       sizeof(b.hdr));
		waveOutClose(wave_out(wave_));
	}
#elif defined(__HAIKU__)
	delete sound_player(player_);
#endif
}

/* see speaker_out.h */
bool SpeakerOut::open(const char *device)
{
#if defined(DMODEM_SPEAKER_ALSA)
	snd_pcm_t *pcm;
	int r = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK,
			     SND_PCM_NONBLOCK);
	if (r < 0) {
		ERR("--speaker: cannot open ALSA's %s: %s\n", device,
		    snd_strerror(r));
		return false;
	}
	pcm_ = pcm;
	DBG("ALSA's %s\n", device);
	return true;
#elif defined(_WIN32)
	UINT n = waveOutGetNumDevs();
	char *end;

	if (!strcmp(device, "default")) {
		id_ = WAVE_MAPPER;
		DBG("waveOut's wave mapper\n");
		return true;
	}
	unsigned long id = strtoul(device, &end, 10);
	if (*device && !*end) {
		if (id < n) {
			id_ = static_cast<unsigned>(id);
			DBG("waveOut device %u\n", id_);
			return true;
		}
	} else {
		for (UINT i = 0; i < n; i++) {
			WAVEOUTCAPSA caps;
			if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) ==
			    MMSYSERR_NOERROR && name_has(caps.szPname, device)) {
				id_ = i;
				DBG("waveOut device %u, %s\n", id_,
				    caps.szPname);
				return true;
			}
		}
	}
	ERR("--speaker: no waveOut device %s; there are:\n", device);
	for (UINT i = 0; i < n; i++) {
		WAVEOUTCAPSA caps;
		if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) ==
		    MMSYSERR_NOERROR)
			fprintf(stderr, "  %u  %s\n", i, caps.szPname);
	}
	return false;
#elif defined(__HAIKU__)
	if (strcmp(device, "default")) {
		ERR("--speaker: Haiku has only default (the system mixer), "
		    "not %s\n", device);
		return false;
	}
	ring_.assign(RING, 0);
	DBG("the Media Kit's system mixer\n");
	return true;
#else
	ERR("--speaker %s: dmodemd was built without a sound device "
	    "(ALSA's libasound2-dev on Linux)\n", device);
	return false;
#endif
}

/* see speaker_out.h */
void SpeakerOut::volume(s16 level, s32 rate)
{
	const bool was_on = level_ > 0;

	if (level <= 0) {
		level_ = 0;
		if (was_on) {
			DBG("off\n");
			stop();
		}
		return;
	}
	if (failed_)
		return;
	if (!was_on || rate != rate_) {
		if (!start(rate)) {
			failed_ = true;
			level_ = 0;
			dcore::log_printf(1, "err: speaker: the device does "
					  "not start: no speaker from now "
					  "on\n");
			return;
		}
	}
	if (level != level_)
		DBG("on, ATL%d, %d Hz\n", level, rate);
	level_ = level;
}

/* see speaker_out.h */
void SpeakerOut::audio(const s16 *rx, const s16 *tx, s32 n)
{
	if (level_ <= 0 || failed_ || n <= 0)
		return;
	if (mix_.size() < static_cast<size_t>(n))
		mix_.resize(n);
	const s32 gain = level_gain(level_);
	for (s32 i = 0; i < n; i++)
		mix_[i] = mix_sample(rx[i], tx[i], gain);
	write(mix_.data(), n);
}

#if defined(DMODEM_SPEAKER_ALSA)

/* see speaker_out.h: the PCM set to rate (when it changes), then
   prepared to play */
bool SpeakerOut::start(s32 rate)
{
	snd_pcm_t *pcm = alsa_pcm(pcm_);
	int r;

	if (!pcm)
		return false;
	snd_pcm_drop(pcm);
	if (rate != rate_) {
		snd_pcm_sw_params_t *sw;
		r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16,
				       SND_PCM_ACCESS_RW_INTERLEAVED, 1,
				       static_cast<unsigned>(rate), 1,
				       ALSA_LATENCY_US);
		if (r < 0) {
			ERR("speaker: ALSA at %d Hz: %s\n", rate,
			    snd_strerror(r));
			return false;
		}
		/* (it plays when it has ALSA_START_MS, not when full:
		   less delay) */
		snd_pcm_sw_params_alloca(&sw);
		if (snd_pcm_sw_params_current(pcm, sw) == 0 &&
		    snd_pcm_sw_params_set_start_threshold(pcm, sw,
			static_cast<snd_pcm_uframes_t>(rate) *
			ALSA_START_MS / 1000) == 0)
			snd_pcm_sw_params(pcm, sw);
		rate_ = rate;
	}
	r = snd_pcm_prepare(pcm);
	if (r < 0) {
		ERR("speaker: ALSA: %s\n", snd_strerror(r));
		return false;
	}
	return true;
}

/* see speaker_out.h */
void SpeakerOut::stop()
{
	if (pcm_)
		snd_pcm_drop(alsa_pcm(pcm_));
}

/* see speaker_out.h: what the PCM takes now; after an underrun it is
   prepared again; after another error, no speaker from then on */
void SpeakerOut::write(const s16 *pcm, s32 n)
{
	snd_pcm_t *p = alsa_pcm(pcm_);
	snd_pcm_sframes_t r = snd_pcm_writei(p, pcm, n);

	if (r == -EPIPE || r == -ESTRPIPE) {
		if (snd_pcm_prepare(p) == 0)
			r = snd_pcm_writei(p, pcm, n);
	}
	/* (-EAGAIN: it is full, the samples dropped) */
	if (r < 0 && r != -EAGAIN && r != -EPIPE) {
		dcore::log_printf(1, "err: speaker: ALSA: %s: no speaker "
				  "from now on\n",
				  snd_strerror(static_cast<int>(r)));
		snd_pcm_drop(p);
		failed_ = true;
	}
}

#elif defined(_WIN32)

/* see speaker_out.h: the device opened at rate (again when the rate
   changes), its buffers prepared */
bool SpeakerOut::start(s32 rate)
{
	if (wave_ && rate != rate_) {
		waveOutReset(wave_out(wave_));
		for (Buffer &b : bufs_)
			waveOutUnprepareHeader(wave_out(wave_), &b.hdr,
					       sizeof(b.hdr));
		waveOutClose(wave_out(wave_));
		wave_ = nullptr;
	}
	if (!wave_) {
		WAVEFORMATEX wf;
		HWAVEOUT h;
		memset(&wf, 0, sizeof(wf));
		wf.wFormatTag = WAVE_FORMAT_PCM;
		wf.nChannels = 1;
		wf.nSamplesPerSec = static_cast<DWORD>(rate);
		wf.wBitsPerSample = 16;
		wf.nBlockAlign = 2;
		wf.nAvgBytesPerSec = static_cast<DWORD>(rate) * 2;
		MMRESULT r = waveOutOpen(&h, id_, &wf, 0, 0, CALLBACK_NULL);
		if (r != MMSYSERR_NOERROR) {
			ERR("speaker: waveOut at %d Hz: error %u\n", rate,
			    static_cast<unsigned>(r));
			return false;
		}
		wave_ = h;
		bufs_.assign(BUFFERS, Buffer());
		for (Buffer &b : bufs_) {
			b.pcm.assign(static_cast<size_t>(rate) * BUFFER_MS /
				     1000, 0);
			memset(&b.hdr, 0, sizeof(b.hdr));
			b.hdr.lpData = reinterpret_cast<LPSTR>(b.pcm.data());
			b.hdr.dwBufferLength =
				static_cast<DWORD>(b.pcm.size() * 2);
			waveOutPrepareHeader(h, &b.hdr, sizeof(b.hdr));
		}
		rate_ = rate;
	}
	fill_ = 0;
	filled_ = 0;
	return true;
}

/* see speaker_out.h: the queued buffers returned */
void SpeakerOut::stop()
{
	if (wave_)
		waveOutReset(wave_out(wave_));
	fill_ = 0;
	filled_ = 0;
}

/* see speaker_out.h: into the buffer being filled, which goes to the
   device when full; while the next one is still queued, the samples
   are dropped */
void SpeakerOut::write(const s16 *pcm, s32 n)
{
	while (wave_ && n > 0) {
		Buffer &b = bufs_[fill_];
		const s32 cap = static_cast<s32>(b.pcm.size());
		if (b.hdr.dwFlags & WHDR_INQUEUE)
			return;
		s32 k = std::min(n, cap - filled_);
		std::copy_n(pcm, k, b.pcm.data() + filled_);
		filled_ += k;
		pcm += k;
		n -= k;
		if (filled_ == cap) {
			waveOutWrite(wave_out(wave_), &b.hdr, sizeof(b.hdr));
			fill_ = (fill_ + 1) % bufs_.size();
			filled_ = 0;
		}
	}
}

#elif defined(__HAIKU__)

/* see speaker_out.h: a BSoundPlayer at rate (again when the rate
   changes), the ring empty, started */
bool SpeakerOut::start(s32 rate)
{
	BSoundPlayer *p = sound_player(player_);

	if (p && rate != rate_) {
		p->Stop(true, true);
		delete p;
		player_ = p = nullptr;
	}
	flush_.store(true);
	if (!p) {
		media_raw_audio_format f = media_raw_audio_format::wildcard;
		f.frame_rate = static_cast<float>(rate);
		f.channel_count = 1;
		f.format = media_raw_audio_format::B_AUDIO_SHORT;
		f.byte_order = B_MEDIA_HOST_ENDIAN;
		f.buffer_size = static_cast<size_t>(rate) / 50 * sizeof(s16);
		rate_ = rate;
		p = new BSoundPlayer(&f, "dmodemd speaker", play_buffer,
				     nullptr, this);
		if (p->InitCheck() != B_OK) {
			ERR("speaker: the Media Kit's sound player does not "
			    "start (is the media server running?)\n");
			delete p;
			return false;
		}
		player_ = p;
	}
	p->SetHasData(true);
	return p->Start() == B_OK;
}

/* see speaker_out.h: the player stopped, not waited for */
void SpeakerOut::stop()
{
	if (player_)
		sound_player(player_)->Stop(false, true);
}

/* see speaker_out.h: into the ring, what has room */
void SpeakerOut::write(const s16 *pcm, s32 n)
{
	if (ring_.empty())
		return;
	u32 h = head_.load(std::memory_order_relaxed);
	u32 t = tail_.load(std::memory_order_acquire);
	u32 k = std::min<u32>(static_cast<u32>(n), RING - (h - t));
	for (u32 i = 0; i < k; i++)
		ring_[(h + i) & (RING - 1)] = pcm[i];
	head_.store(h + k, std::memory_order_release);
}

/* see speaker_out.h: from the ring (its oldest skipped when it holds
   too much), silence for what it does not have */
void SpeakerOut::pull(s16 *pcm, s32 n)
{
	u32 t = tail_.load(std::memory_order_relaxed);
	u32 h = head_.load(std::memory_order_acquire);
	const u32 most = static_cast<u32>(rate_) * BACKLOG_MS / 1000;
	const u32 keep = static_cast<u32>(rate_) * TRIM_MS / 1000;

	if (ring_.empty()) {
		std::fill_n(pcm, n, 0);
		return;
	}
	if (flush_.exchange(false))
		t = h;
	else if (h - t > most)
		t = h - keep;
	u32 k = std::min<u32>(static_cast<u32>(n), h - t);
	for (u32 i = 0; i < k; i++)
		pcm[i] = ring_[(t + i) & (RING - 1)];
	std::fill_n(pcm + k, n - static_cast<s32>(k), 0);
	tail_.store(t + k, std::memory_order_release);
}

#else

/* (no back end: open() refuses, so none of these runs) */
bool SpeakerOut::start(s32 rate)
{
	(void)rate;
	return false;
}

/* (see above) */
void SpeakerOut::stop() {}

/* (see above) */
void SpeakerOut::write(const s16 *pcm, s32 n)
{
	(void)pcm;
	(void)n;
}

#endif

#if !defined(__HAIKU__)
/* see speaker_out.h (the Media Kit's alone) */
void SpeakerOut::pull(s16 *pcm, s32 n)
{
	std::fill_n(pcm, n, 0);
}
#endif

} // namespace dmd
