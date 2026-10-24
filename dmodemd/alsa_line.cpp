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

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <log.h>

#include "dm_types.h"
#include "alsa_line.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "alsa: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/* the periods the playback leads by, and the buffer's */
const s16 LEAD_PERIODS = 2;
const s16 BUFFER_PERIODS = 8;

} // namespace

/* the codec closed (close()) */
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
bool AlsaLine::open(const char *pcm, const char *ctl, u32 rate)
{
	std::string ctl_name = ctl ? ctl : ctl_of(pcm);
	int err;

	rate_ = rate;
	frame_ = static_cast<s16>(rate / 50);
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
	ring_.init(static_cast<s32>(rate));
	set(caller_id_, true);
	daa_open();
	snd_ctl_card_info_t *info;
	snd_ctl_card_info_alloca(&info);
	if (snd_ctl_card_info(ctl_, info) >= 0)
		cx_open(snd_ctl_card_info_get_card(info));
	hook_switch(false);
	if (!restart(nullptr)) {
		close();
		return false;
	}
	DBG("%s (control %s): %" PRIu32 " Hz, %d samples ahead\n", pcm,
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
		ERR("ALSA %s: no rate of %" PRIu32 " Hz (%u nearest)\n", way,
		    rate_, rate);
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
		delay_ = static_cast<s32>(period) * LEAD_PERIODS;
	if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
	    (err = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(
		     pcm, sw,
		     capture ? buffer * 2 :
			       static_cast<snd_pcm_uframes_t>(delay_))) < 0 ||
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
s16 AlsaLine::set_hook(dcore::Modem &, s16 state)
{
	bool off = state == MODEM_HOOK_OFF;

	DBG("%s\n", off ? "off hook" : state == MODEM_HOOK_ON ? "on hook" :
	    "on hook, snooping");
	hook_ = state;
	flash_end_ = 0;
	/* (off hook: the settings again, a resume may have reset them;
	   on hook: RZ the country's again) */
	if (daa_ && off)
		daa_write(true);
	set(caller_id_, !off);
	hook_switch(off);
	if (daa_ && !off)
		daa_write(false);
	return 0;
}

/* see alsa_line.h: a country the datasheet has no settings for keeps
   the USA's */
void AlsaLine::country(dcore::Modem &, const char *name)
{
	daa_name_ = name ? name : "USA";
	daa_country_ = si3054_country(name);
	if (!daa_country_) {
		if (daa_)
			DBG("no Si3054 line settings for %s: the USA's\n",
			    daa_name_.c_str());
		daa_country_ = &si3054_usa();
	}
	daa_due_ = true;
	if (daa_ && hook_ != MODEM_HOOK_OFF)
		daa_write(false);
	cx_country_ = cx_country(name);
	if (!cx_country_) {
		if (cx_)
			DBG("no Conexant line setting for %s: the USA's\n",
			    daa_name_.c_str());
		cx_country_ = &cx_usa();
	}
	if (cx_ && !cx_->set_country(*cx_country_))
		ERR("SmartDAA country: %s\n", cx_->error().c_str());
}

/* see alsa_line.h: a Si3054 with a Si3018 line side (its chip ID) */
void AlsaLine::daa_open()
{
	std::string why;
	u16 id = 0;

	daa_ = false;
	if (!hda_.open(ctl_, &why)) {
		DBG("line settings: the kernel driver's (%s)\n", why.c_str());
		return;
	}
	if (!hda_.read(SI3054_CHIPID, &id) || !si3054_chip_ok(id)) {
		DBG("line settings: the kernel driver's (%s: chip ID %04x, "
		    "not a Si3054 with a Si3018)\n", hda_.name().c_str(), id);
		hda_.close();
		return;
	}
	DBG("%s: a Si3054 (chip ID %04x): the country's line settings\n",
	    hda_.name().c_str(), id);
	daa_ = true;
	if (!daa_country_)
		daa_country_ = &si3054_usa();
	daa_write(false);
}

/* see alsa_line.h: nodes 13, 15, 16 and 18, each read, written if the
   country's bits change it */
bool AlsaLine::daa_write(bool off_hook)
{
	Si3054Regs r;
	struct {
		s16 nid;
		u16 *val;
	} regs[] = { { SI3054_LINE_CFG1, &r.n13 }, { SI3054_DC_TERM, &r.n15 },
		     { SI3054_LINE_CFG, &r.n16 },
		     { SI3054_SQ_CONTROL, &r.n18 } };
	u16 old[4];
	bool changed = false;
	s16 i = 0;

	for (auto &x : regs) {
		if (!hda_.read(x.nid, x.val)) {
			ERR("%s: register %d: cannot read it\n",
			    hda_.name().c_str(), x.nid);
			return false;
		}
		old[i++] = *x.val;
	}
	si3054_apply(*daa_country_, off_hook, r);
	i = 0;
	for (auto &x : regs) {
		if (*x.val != old[i++]) {
			changed = true;
			if (!hda_.write(x.nid, *x.val)) {
				ERR("%s: register %d: cannot write it\n",
				    hda_.name().c_str(), x.nid);
				return false;
			}
		}
	}
	if (changed || daa_due_)
		DBG("%s's line%s: registers 13 %04x, 15 %04x, 16 %04x, 18 "
		    "%04x\n", daa_name_.c_str(), off_hook ? " (off hook)" :
		    "", r.n13, r.n15, r.n16, r.n18);
	daa_due_ = false;
	return true;
}

/* see alsa_line.h: the codec's vendor ID one of cx_codec()'s, its
   line side's version read through the SmartDAA */
void AlsaLine::cx_open(int card)
{
	std::string why;
	u16 id1 = 0, id2 = 0;
	u8 version = 0;

	cx_.reset();
	if (!ac97_.is_open() && !ac97_.open(card, &why)) {
		DBG("no AC'97 codec registers (%s): the hook the kernel's\n",
		    why.c_str());
		return;
	}
	if (!ac97_.read(0x7c, &id1) || !ac97_.read(0x7e, &id2) ||
	    !cx_codec(id1, id2)) {
		DBG("AC'97 codec at %s: vendor %04x %04x, its hook the "
		    "kernel's\n", ac97_.name().c_str(), id1, id2);
		return;
	}
	s16 kind = cx_kind(id2);
	if (kind < 0) {
		ERR("Conexant codec at %s (%04x %04x): its SmartDAA is not "
		    "behind register 0x7a; the hook the kernel's\n",
		    ac97_.name().c_str(), id1, id2);
		return;
	}
	cx_.reset(new CxDaa(ac97_, cx_variant(kind), kind));
	cx_->set_board(cx_board(static_cast<u8>(id2)));
	cx_->set_country(*cx_country_);
	if (!cx_->power_up(&version)) {
		ERR("Conexant codec at %s (%04x %04x, SSD kind %#x): its line "
		    "side does not answer (%s)\n", ac97_.name().c_str(), id1,
		    id2, kind, cx_->error().c_str());
		cx_.reset();
		return;
	}
	DBG("Conexant codec at %s (%04x %04x, SSD kind %#x), line side "
	    "version %02x: the hook in its SmartDAA%s%s\n",
	    ac97_.name().c_str(), id1, id2, kind, version,
	    cx_->setup() ? ", its line set up as Conexant's driver does, "
	    "board " : " (its line setup not known)",
	    cx_->setup() ? cx_board((u8)id2).name : "");
}

/* see alsa_line.h */
void AlsaLine::hook_switch(bool off)
{
	if (cx_ && !(off ? cx_->go_off_hook() : cx_->go_on_hook()))
		ERR("SmartDAA hook: %s\n", cx_->error().c_str());
	set(off_hook_, off);
}

/* see dcore.h: on hook for ms, then off again (if it still is) */
void AlsaLine::hook_flash(dcore::Modem &, u16 ms)
{
	if (hook_ != MODEM_HOOK_OFF)
		return;
	DBG("hook flash, %u ms\n", ms);
	hook_switch(false);
	flash_end_ =
		samples_ + static_cast<u64>(ms) * rate_ / 1000;
}

/* see alsa_line.h */
s16 AlsaLine::pulse_hook(dcore::Modem &, bool broken)
{
	if (hook_ != MODEM_HOOK_OFF)
		return -1;
	flash_end_ = 0;
	if (cx_) {
		if (!cx_->set_hook(broken ? CxDaa::PULSE_BREAK :
				   CxDaa::HOOK_OFF))
			ERR("SmartDAA: %s\n", cx_->error().c_str());
	}
	set(off_hook_, !broken);
	return 0;
}

/* see alsa_line.h */
void AlsaLine::watch(RunLoop &loop)
{
	struct pollfd fds[8];
	int n;

	if (!cap_)
		return;
	n = snd_pcm_poll_descriptors(cap_, fds, 8);
	for (s16 i = 0; i < n; i++)
		loop.watch(fds[i].fd);
}

/* both ways stopped, prepared, the playback given its lead of silence
   (which starts it) and the capture started; why (nullptr: the first
   start) in the log */
bool AlsaLine::restart(const char *why)
{
	std::vector<s16> lead(delay_, 0);
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
	err = static_cast<int>(snd_pcm_writei(play_, lead.data(), lead.size()));
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
void AlsaLine::frame(dcore::Modem &m, const s16 *in, s16 *out)
{
	if (flash_end_ && samples_ >= flash_end_) {
		flash_end_ = 0;
		if (hook_ == MODEM_HOOK_OFF)
			hook_switch(true);
	}
	if (hook_ != MODEM_HOOK_OFF) {
		for (s16 i = 0; i < frame_; i++)
			if (ring_.sample(in[i]))
				m.ring();
	}
	samples_ += frame_;
	if (running_ && m.started())
		m.process(in, out, frame_);
	else
		std::fill_n(out, frame_, 0);
}

/* see alsa_line.h */
s16 AlsaLine::pump(dcore::Modem &m)
{
	for (;;) {
		snd_pcm_sframes_t n = snd_pcm_readi(cap_, in_.data() + have_,
						    frame_ - have_);
		if (n == -EAGAIN)
			return 0;
		if (n < 0) {
			if (!restart(n == -EPIPE ? "capture overrun" :
				     snd_strerror(static_cast<int>(n))))
				return -1;
			continue;
		}
		have_ += static_cast<s16>(n);
		if (have_ < frame_)
			return 0;
		have_ = 0;
		frame(m, in_.data(), out_.data());
		s16 done = 0;
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
					     snd_strerror(static_cast<int>(n))))
					return -1;
				break;
			}
			done += static_cast<s16>(n);
		}
	}
}

/* the codec's PCM and control closed, the switches off first */
void AlsaLine::close()
{
	hda_.close();
	daa_ = false;
	if (ctl_) {
		hook_switch(false);
		set(caller_id_, false);
		snd_ctl_close(ctl_);
		ctl_ = nullptr;
	}
	snd_ctl_elem_id_free(off_hook_.id);
	snd_ctl_elem_id_free(caller_id_.id);
	off_hook_ = caller_id_ = Switch();
	cx_.reset();
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
