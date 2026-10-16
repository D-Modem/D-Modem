/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    alsa_line.cpp - the modem's line a softmodem's codec through ALSA
 *                    (alsa_line.h)
 *
 *    Both ways S16 mono at the line's rate, non-blocking, in periods of
 *    20 ms, the playback started two periods ahead of the capture (the
 *    line's delay, io_delay()).  Each turn the capture's whole frames
 *    are read; each one's answer is written at once, so the two keep
 *    their distance.  An overrun or underrun starts both again.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <log.h>

#include "alsa_line.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "alsa: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/* the periods the playback leads by, and the buffer's */
const int LEAD_PERIODS = 2;
const int BUFFER_PERIODS = 8;

} // namespace

AlsaLine::~AlsaLine()
{
	close();
}

/* see alsa_line.h */
std::string AlsaLine::ctl_of(const char *pcm)
{
	std::string s(pcm);
	size_t colon = s.find(':');

	if (colon == std::string::npos)
		return s;
	std::string kind = s.substr(0, colon);
	if (kind != "hw" && kind != "plughw")
		return s;
	std::string card = s.substr(colon + 1);
	size_t comma = card.find(',');
	if (comma != std::string::npos)
		card.erase(comma);
	return "hw:" + card;
}

/* see alsa_line.h: the hook switch is a must, the caller ID one is not
   (no rings then while on hook, maybe) */
bool AlsaLine::open(const char *pcm, const char *ctl, unsigned rate)
{
	std::string ctl_name = ctl ? ctl : ctl_of(pcm);
	int err;

	rate_ = rate;
	frame_ = (int)rate / 50;
	in_.assign(frame_, 0);
	out_.assign(frame_, 0);
	err = snd_pcm_open(&cap_, pcm, SND_PCM_STREAM_CAPTURE,
			   SND_PCM_NONBLOCK);
	if (err >= 0)
		err = snd_pcm_open(&play_, pcm, SND_PCM_STREAM_PLAYBACK,
				   SND_PCM_NONBLOCK);
	if (err < 0) {
		ERR("ALSA %s: %s\n", pcm, snd_strerror(err));
		close();
		return false;
	}
	if (!setup(cap_, true) || !setup(play_, false)) {
		close();
		return false;
	}
	err = snd_ctl_open(&ctl_, ctl_name.c_str(), 0);
	if (err < 0) {
		ERR("ALSA control %s: %s\n", ctl_name.c_str(),
		    snd_strerror(err));
		close();
		return false;
	}
	if (!find(off_hook_, "Off-hook Switch")) {
		ERR("ALSA control %s has no \"Off-hook Switch\": not a "
		    "modem codec?\n", ctl_name.c_str());
		close();
		return false;
	}
	if (!find(caller_id_, "Caller ID Switch"))
		ERR("ALSA control %s has no \"Caller ID Switch\": rings and "
		    "caller ID may not be heard\n", ctl_name.c_str());
	ring_.init((int)rate);
	set(off_hook_, false);
	set(caller_id_, true);
	if (!restart(nullptr)) {
		close();
		return false;
	}
	DBG("%s (control %s): %u Hz, %d samples ahead\n", pcm,
	    ctl_name.c_str(), rate, delay_);
	return true;
}

/* one way's hardware and software parameters: S16 mono at rate_,
   periods of frame_ near enough, BUFFER_PERIODS of them; a capture
   started by restart(), a playback by its lead */
bool AlsaLine::setup(snd_pcm_t *pcm, bool capture)
{
	const char *way = capture ? "capture" : "playback";
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t period = frame_, buffer;
	unsigned rate = rate_;
	int dir = 0, err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw,
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw,
			SND_PCM_FORMAT_S16)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, 1)) < 0) {
		ERR("ALSA %s: S16 mono: %s\n", way, snd_strerror(err));
		return false;
	}
	err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir);
	if (err < 0 || rate != rate_) {
		ERR("ALSA %s: no rate of %u Hz (%u nearest)\n", way, rate_,
		    rate);
		return false;
	}
	if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period,
							  &dir)) < 0) {
		ERR("ALSA %s: period: %s\n", way, snd_strerror(err));
		return false;
	}
	buffer = period * BUFFER_PERIODS;
	if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw,
							  &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		ERR("ALSA %s: buffer: %s\n", way, snd_strerror(err));
		return false;
	}
	if (!capture)
		delay_ = (int)period * LEAD_PERIODS;
	if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
	    (err = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw,
			capture ? buffer * 2 : (snd_pcm_uframes_t)delay_)) < 0 ||
	    (err = snd_pcm_sw_params(pcm, sw)) < 0) {
		ERR("ALSA %s: software parameters: %s\n", way,
		    snd_strerror(err));
		return false;
	}
	DBG("%s: period %lu, buffer %lu\n", way, (unsigned long)period,
	    (unsigned long)buffer);
	return true;
}

/* the switch called name, if the control has it (a mixer element) */
bool AlsaLine::find(Switch &s, const char *name)
{
	snd_ctl_elem_info_t *info;

	snd_ctl_elem_info_alloca(&info);
	if (snd_ctl_elem_id_malloc(&s.id) < 0)
		return false;
	snd_ctl_elem_id_set_interface(s.id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(s.id, name);
	snd_ctl_elem_info_set_id(info, s.id);
	s.have = snd_ctl_elem_info(ctl_, info) >= 0;
	return s.have;
}

/* a switch on or off (an error only logged) */
void AlsaLine::set(Switch &s, bool on)
{
	snd_ctl_elem_value_t *v;
	int err;

	if (!s.have)
		return;
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_id(v, s.id);
	snd_ctl_elem_value_set_boolean(v, 0, on);
	err = snd_ctl_elem_write(ctl_, v);
	if (err < 0)
		ERR("ALSA %s: %s\n", snd_ctl_elem_id_get_name(s.id),
		    snd_strerror(err));
}

/* see alsa_line.h: off hook the line monitor off; on hook, or snooping
   for caller ID, on */
int AlsaLine::set_hook(dcore::Modem &, int state)
{
	bool off = state == MODEM_HOOK_OFF;

	DBG("%s\n", off ? "off hook" : state == MODEM_HOOK_ON ? "on hook" :
	    "on hook, snooping");
	hook_ = state;
	flash_end_ = 0;
	set(caller_id_, !off);
	set(off_hook_, off);
	return 0;
}

/* see dcore.h: on hook for ms, then off again (if it still is) */
void AlsaLine::hook_flash(dcore::Modem &, unsigned ms)
{
	if (hook_ != MODEM_HOOK_OFF)
		return;
	DBG("hook flash, %u ms\n", ms);
	set(off_hook_, false);
	flash_end_ = samples_ + (unsigned long long)ms * rate_ / 1000;
}

/* see alsa_line.h */
void AlsaLine::watch(RunLoop &loop)
{
	struct pollfd fds[8];
	int n;

	if (!cap_)
		return;
	n = snd_pcm_poll_descriptors(cap_, fds, 8);
	for (int i = 0; i < n; i++)
		loop.watch(fds[i].fd);
}

/* both ways stopped, prepared, the playback given its lead of silence
   (which starts it) and the capture started; why (nullptr: the first
   start) in the log */
bool AlsaLine::restart(const char *why)
{
	std::vector<int16_t> lead(delay_, 0);
	int err;

	if (why)
		DBG("%s: both ways started again\n", why);
	snd_pcm_drop(cap_);
	snd_pcm_drop(play_);
	have_ = 0;
	if ((err = snd_pcm_prepare(cap_)) < 0 ||
	    (err = snd_pcm_prepare(play_)) < 0) {
		ERR("ALSA prepare: %s\n", snd_strerror(err));
		return false;
	}
	err = (int)snd_pcm_writei(play_, lead.data(), lead.size());
	if (err < 0) {
		ERR("ALSA playback: %s\n", snd_strerror(err));
		return false;
	}
	if (snd_pcm_state(play_) != SND_PCM_STATE_RUNNING &&
	    (err = snd_pcm_start(play_)) < 0) {
		ERR("ALSA playback start: %s\n", snd_strerror(err));
		return false;
	}
	if ((err = snd_pcm_start(cap_)) < 0) {
		ERR("ALSA capture start: %s\n", snd_strerror(err));
		return false;
	}
	return true;
}

/* see alsa_line.h */
void AlsaLine::frame(dcore::Modem &m, const int16_t *in, int16_t *out)
{
	if (flash_end_ && samples_ >= flash_end_) {
		flash_end_ = 0;
		if (hook_ == MODEM_HOOK_OFF)
			set(off_hook_, true);
	}
	if (hook_ != MODEM_HOOK_OFF) {
		for (int i = 0; i < frame_; i++)
			if (ring_.sample(in[i]))
				m.ring();
	}
	samples_ += frame_;
	if (running_ && m.started())
		m.process(in, out, frame_);
	else
		memset(out, 0, frame_ * sizeof(*out));
}

/* see alsa_line.h */
int AlsaLine::pump(dcore::Modem &m)
{
	for (;;) {
		snd_pcm_sframes_t n = snd_pcm_readi(cap_, in_.data() + have_,
						    frame_ - have_);
		if (n == -EAGAIN)
			return 0;
		if (n < 0) {
			if (!restart(n == -EPIPE ? "capture overrun" :
				     snd_strerror((int)n)))
				return -1;
			continue;
		}
		have_ += (int)n;
		if (have_ < frame_)
			return 0;
		have_ = 0;
		frame(m, in_.data(), out_.data());
		int done = 0;
		while (done < frame_) {
			n = snd_pcm_writei(play_, out_.data() + done,
					   frame_ - done);
			if (n == -EAGAIN) {
				/* the playback has drifted ahead: this
				   frame's rest dropped */
				DBG("playback full: %d samples dropped\n",
				    frame_ - done);
				break;
			}
			if (n < 0) {
				if (!restart(n == -EPIPE ? "playback underrun" :
					     snd_strerror((int)n)))
					return -1;
				break;
			}
			done += (int)n;
		}
	}
}

/* the codec's PCM and control closed, the switches off first */
void AlsaLine::close()
{
	if (ctl_) {
		set(off_hook_, false);
		set(caller_id_, false);
		snd_ctl_close(ctl_);
		ctl_ = nullptr;
	}
	snd_ctl_elem_id_free(off_hook_.id);
	snd_ctl_elem_id_free(caller_id_.id);
	off_hook_ = caller_id_ = Switch();
	if (cap_) {
		snd_pcm_close(cap_);
		cap_ = nullptr;
	}
	if (play_) {
		snd_pcm_close(play_);
		play_ = nullptr;
	}
}

} // namespace dmd
