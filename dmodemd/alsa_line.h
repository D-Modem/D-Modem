/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    alsa_line.h - the modem's line a softmodem's codec through ALSA: an
 *                  AC'97 modem (snd-intel8x0m, snd-atiixp-modem,
 *                  snd-via82xx-modem) or an HD-audio Si3054 and its kind
 *                  (snd-hda-codec-si3054), whose kernel drivers give
 *                  their samples and two switches, "Off-hook Switch" and
 *                  "Caller ID Switch" (the line monitor on hook)
 *
 *    The codec's PCM runs both ways from open() on, on hook too: its
 *    samples then go through the ring detector (dsp::RingDetect), which
 *    has the modem's ring() at each cycle of a ring signal, and through
 *    the modem while it has the line started (caller ID; then the
 *    call).  On hook the line monitor is on.  The modem dials itself, in
 *    DTMF.  Time is the codec's: the samples it has given.
 *
 *    A Conexant (Rockwell) AC'97 modem codec on an ICH modem controller
 *    has its hook in its SmartDAA, not the GPIO the kernel's switch
 *    drives: as root, the hook goes there too (cx_daa.h, through the
 *    controller's I/O ports, ac97_ports.h).
 *
 *    An HD-audio Si3054 with a Si3018 line side also gets the country's
 *    line settings (si3054.h) through the kernel's hwdep device
 *    (hda_codec.h): at open(), on each change of the country, and before
 *    each going off hook (a resume's codec reset undone).  Other codecs
 *    keep their kernel driver's.
 *
 */
#ifndef DMODEMD_ALSA_LINE_H
#define DMODEMD_ALSA_LINE_H

#include <memory>
#include <string>
#include <vector>

#include <alsa/asoundlib.h>
#include <dcore.h>
#include <dsp.h>

#include "dm_types.h"
#include "ac97_ports.h"
#include "cx_country.h"
#include "cx_daa.h"
#include "hda_codec.h"
#include "pty.h"
#include "run_loop.h"
#include "si3054.h"

namespace dmd {

class AlsaLine final : public dcore::Host {
public:
	/* the line of pty's modem, not yet open */
	explicit AlsaLine(Pty &pty) : pty_(pty) {}
	~AlsaLine();
	AlsaLine(const AlsaLine &) = delete;
	AlsaLine &operator=(const AlsaLine &) = delete;

	/* the codec's PCM pcm (both ways) and its controls' ctl (nullptr:
	   the card of pcm's, or pcm's name) opened at rate, running, on
	   hook; false with the reason on stderr */
	bool open(const char *pcm, const char *ctl, u32 rate);
	/* the line's samples: its descriptors watched this turn */
	void watch(RunLoop &loop);
	/* the samples that have come, a frame (20 ms) at a time, through
	   the ring detector and the modem, its answer out; 0, or -1 if the
	   codec has failed for good */
	s16 pump(dcore::Modem &m);

	/* Host */
	s16 start(dcore::Modem &) override { running_ = true; return 0; }
	/* the samples no longer through the modem */
	s16 stop(dcore::Modem &) override { running_ = false; return 0; }
	s16 set_hook(dcore::Modem &m, s16 state) override;
	/* an analogue line, the modem dialling it itself */
	bool analog_line(dcore::Modem &) override { return true; }
	/* a hook flash it can make (hook_flash()) */
	bool can_flash(dcore::Modem &) override { return true; }
	void hook_flash(dcore::Modem &m, u16 ms) override;
	/* pulse dialling: the hook switch off (broken) or on, while off
	   hook */
	s16 pulse_hook(dcore::Modem &m, bool broken) override;
	/* the playback's lead, in samples */
	s32 io_delay(dcore::Modem &) override { return delay_; }
	/* the country: an Si3054's line settings, written now on hook,
	   else once on hook again */
	void country(dcore::Modem &m, const char *name) override;
	/* for ATI3 */
	const char *driver_name() override { return "ALSA"; }
	/* the modem's characters to the pty: how many it took */
	s32 dte_write(dcore::Modem &, const char *buf, s32 n) override {
		return pty_.write(buf, n);
	}
	/* the clock: the codec's samples so far, in ms */
	u64 now_ms() override {
		return samples_ * 1000 / rate_;
	}

	/* the control's name for PCM pcm: hw:<card> for hw:<card>,<dev> or
	   plughw:..., else pcm's own */
	static std::string ctl_of(const char *pcm);

private:
	/* a switch of the codec's, if it has it */
	struct Switch {
		snd_ctl_elem_id_t *id = nullptr;
		bool have = false;
	};
	bool setup(snd_pcm_t *pcm, bool capture);
	bool find(Switch &s, const char *name);
	void set(Switch &s, bool on);
	/* both ways started again, the playback ahead by delay_ */
	bool restart(const char *why);
	/* frame_ samples of the line's through the ring detector and the
	   modem; the answer in out */
	void frame(dcore::Modem &m, const s16 *in, s16 *out);
	void close();
	/* the HD-audio modem codec, if it is a Si3054 the country's line
	   settings can go into (open()'s) */
	void daa_open();
	/* the country's settings into its registers, RZ off (off_hook) or
	   the country's; false (logged) on an error */
	bool daa_write(bool off_hook);
	/* a Conexant SmartDAA on card card's codec, if there is one and
	   its line side answers (open()'s) */
	void cx_open(int card);
	/* the hook switch: the kernel's, and the SmartDAA's if there is
	   one (an error logged) */
	void hook_switch(bool off);

	Pty &pty_;
	snd_pcm_t *cap_ = nullptr, *play_ = nullptr;
	snd_ctl_t *ctl_ = nullptr;
	Switch off_hook_, caller_id_;
	u32 rate_ = 9600;
	s16 frame_ = 192;               /* 20 ms */
	s32 delay_ = 0;                 /* the playback's lead (samples) */
	std::vector<s16> in_, out_;
	s16 have_ = 0;                  /* of in_ */
	u64 samples_ = 0;
	bool running_ = false;
	s16 hook_ = MODEM_HOOK_ON;
	u64 flash_end_ = 0;
	dsp::RingDetect ring_;
	/* the codec's registers, if they take the country's settings; the
	   country, its name, still to be written */
	HdaCodec hda_;
	bool daa_ = false;
	const Si3054Country *daa_country_ = &si3054_usa();
	std::string daa_name_ = "USA";
	/* the country's Conexant SmartDAA setting (cx_country.h) */
	const CxCountry *cx_country_ = &cx_usa();
	bool daa_due_ = true;
	/* a Conexant codec's SmartDAA, if its hook is there */
	Ac97Ports ac97_;
	std::unique_ptr<CxDaa> cx_;
};

} // namespace dmd

#endif
