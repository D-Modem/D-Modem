/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    modem.cpp - the modem core's call (dcore.h): its states from the
 *                dial or the answer to the hang-up, the DTE's characters,
 *                the line's samples through the data pump that runs, the
 *                data link layer it owns, the result codes, rings and
 *                caller ID, voice and class 1 fax (T.38 too), the timers
 *                and the call's statistics (ATI11)
 *
 *    The AT interpreter (at.cpp) is the other half of the class: it gets
 *    the DTE's characters while the modem is in command state and calls
 *    back here for what a command does to the call.  The data pumps see
 *    the modem through interface.cpp.
 *
 *    Time comes from the Host in two forms: its clock (now_ms()), for the
 *    escape's guard time and the rings' cadence, and the count of the
 *    line's samples, for everything that happens inside a call (the hang-
 *    up's stop, the answer delay, the end of caller ID).
 *
 */

#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <dsplibs.h>
#include <link.h>
#include <modem_dp.h>

#include "dm_types.h"
#include "dcore.h"
#include "dsp.h"
#include "tones.h"
#include "log.h"
#include "str.h"
#include "reinit.h"

/* (weak: Smart Link's library, half_smartlink's, has none of these;
   MSVC has no weak symbols, and links the open DSP, which has them) */
#ifdef __GNUC__
#define DCORE_WEAK __attribute__((weak))
#define DCORE_HAS(f) (f != nullptr)
#else
#define DCORE_WEAK
#define DCORE_HAS(f) true
#endif
/* V.91's exact PCM codes, which must keep their DC (dsplibs.h): weak, as
   Smart Link's library (half_smartlink) has no such function, and then
   the samples are never PCM codes */
extern "C" int dp_runtime_pcm(void *runtime) DCORE_WEAK;
/* an analogue line for the pumps (dsplibs.h): weak as well */
extern "C" void dp_runtime_set_analog(void *runtime, int analog)
	DCORE_WEAK;
/* the fax engine's CED cut short after an answer tone (dsplibs.h):
   Smart Link's library's fax engine has none */
extern "C" void FAX_answer_tone(void *obj, unsigned ms) DCORE_WEAK;

namespace dcore {

const char *const Modem::author = "D-Modem project";
const char *const Modem::product = "D-Modem";
const char *const Modem::version = DMODEM_VERSION;

/* the DTE's characters waiting for the line or the interpreter */
static const s16 XMIT_SIZE = 4096;
/* a hang-up's stop: the samples after it */
static const u32 STOP_SAMPLES = 48;
/* the stop due after STOP_SAMPLES done anyway once the line has given no
   samples for this long: a line with no audio yet (a SIP call that
   rings without early media) would otherwise hold it until the call
   ended some other way.  Well past a frame's gap (20 ms) with jitter */
static const u32 STOP_IDLE_MS = 100;
/* the escape's guard time (before, between and after the S2s) */
static const u32 ESCAPE_MS = 500;
/* caller ID listens this long after the first ring */
static const u32 CID_MS = 4000;
/* a ring burst: the least gap before its first pulse, when it is
   checked, its least length (first to last pulse) and pulses (four
   cycles: a 16 Hz signal's in 0.3 s); how long a ring counts in S1
   without another one (8 s: a cadence of up to 10 s, Serbia's 1 s on and
   9 off, rings in time); and, for ringing with no such gap (Egypt's 2 s
   on and 1 off, Syria's 1 and 0.5), how long after a burst's first pulse
   the next pulse starts another */
static const u32 RING_GAP_MS = 1500, RING_CHECK_MS = 2510;
static const u32 RING_MIN_MS = 150;
static const u16 RING_PULSES = 4;
static const u32 RING_GONE_MS = 8010, RING_AGAIN_MS = 6000;

/* why a call ended (ATI11), by Modem::Cause */
static const char *const cause_text[] = {
	"None", "Local hangup", "Remote cleardown", "Line dropped",
	"Carrier lost", "No dial tone", "Busy", "No answer",
	"Training failed", "Protocol failure", "No carrier",
};

/* the result codes in words, by Result */
static const char *const result_text[] = {
	"OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE",
	"BUSY", "NO ANSWER", "FAX", "DATA",
};

/* CONNECT's number with ATV0 for a rate (Smart Link's ST56 numbers,
   D-Modem's own from 80) */
static const struct {
	u32 rate;
	u16 code;
} connect_codes[] = {
	{ 300, 9 }, { 600, 10 }, { 1200, 5 }, { 2400, 11 }, { 4800, 12 },
	{ 7200, 13 }, { 9600, 27 }, { 12000, 14 }, { 14400, 15 },
	{ 16800, 16 }, { 19200, 17 }, { 21600, 18 }, { 24000, 19 },
	{ 26400, 20 }, { 28800, 21 }, { 31200, 22 }, { 33600, 23 },
	{ 34800, 24 }, { 40000, 25 }, { 42000, 26 }, { 44000, 28 },
	{ 46000, 29 }, { 48000, 30 }, { 50000, 31 }, { 52000, 32 },
	{ 54000, 33 }, { 56000, 34 }, { 57600, 35 }, { 115200, 36 },
	{ 230400, 37 }, { 460800, 38 }, { 921600, 39 }, { 32000, 40 },
	{ 34000, 41 }, { 36000, 42 }, { 38000, 43 }, { 58000, 44 },
	{ 60000, 45 }, { 28000, 46 }, { 29333, 47 }, { 30666, 48 },
	{ 33333, 49 }, { 34666, 50 }, { 37333, 51 }, { 38666, 52 },
	{ 41333, 53 }, { 42666, 54 }, { 45333, 55 }, { 46666, 56 },
	{ 49333, 57 }, { 50666, 58 }, { 53333, 59 }, { 54666, 60 },
	{ 75, 80 }, { 25333, 81 }, { 26666, 82 }, { 57333, 83 },
	{ 58666, 84 }, { 61333, 85 }, { 62666, 86 }, { 64000, 87 },
};

/* the number CONNECT has for rate with ATV0; 1 for a rate without one */
static u16 connect_code(u32 rate)
{
	for (const auto &c : connect_codes)
		if (c.rate == rate)
			return c.code;
	return 1;
}

/* the driver table's entry for pump id, NULL if there is none */
static const DpDriver *find_driver(u32 id)
{
	for (const DpDriver *d = dp_drivers(); d->id; d++)
		if (static_cast<u32>(d->id) == id)
			return d;
	return nullptr;
}

/* the modulation a call has: the one the pumps reported in its
   statistics, else the pump's own id, else 0 */
static s16 modulation(const struct dp_stats &st, const struct dp *dp)
{
	return st.dp ? st.dp : dp ? static_cast<s16>(dp->id) : 0;
}

/* src into dst (size octets), cut after its first ';' (which stays) */
static void copy_dial(char *dst, size_t size, const char *src)
{
	size_t i;
	for (i = 0; i + 1 < size && src[i]; i++) {
		dst[i] = src[i];
		if (src[i] == ';') {
			i++;
			break;
		}
	}
	dst[i] = '\0';
}

/*
 *    the data link layer's owner
 */

/* what the link (link.h) asks of the modem that owns it */
struct LinkOwner final : public dlink::Owner {
	/* the link's owner for m */
	explicit LinkOwner(Modem &m) : m_(m) {}

	/* the DTE's characters for the line */
	s16 read_chars(u8 *buf, s16 n) override {
		return m_.get_chars(reinterpret_cast<char *>(buf), n);
	}
	/* the remote's characters for the DTE: what it took */
	s16 write_chars(const u8 *buf, s16 n) override {
		return m_.put_chars(reinterpret_cast<const char *>(buf), n);
	}
	/* the link's progress as the modem's */
	void status(dlink::Status s) override {
		switch (s) {
		case dlink::Status::Detected:
			m_.update_status(Modem::ST_PACK_LINK);
			break;
		case dlink::Status::Up:
			m_.update_status(Modem::ST_EC_LINK);
			break;
		case dlink::Status::Released:
			m_.update_status(Modem::ST_EC_RELEASE);
			break;
		case dlink::Status::Error:
			m_.update_status(Modem::ST_EC_ERROR);
			break;
		}
	}
	/* the pump's transmit rate, for the link's bit timers */
	s32 tx_rate() override {
		return static_cast<s32>(m_.tx_rate());
	}
	/* what XID or MNP's LRs agreed */
	void negotiated(const dlink::Config &c) override {
		m_.negotiated(c.comp, c.v44, c.dict_size, c.max_string,
			      c.mnp5);
	}
	/* a line of the link's in the modem's log */
	void log(const char *line) override { m_.logf("%s\n", line); }

	Modem &m_;
};

/*
 *    the modem
 */

/* a modem on host with opt: the S-registers and the kept parameters at
   their defaults, the country (USA, or opt.country), the link layer and
   the DTE's buffer; ok() if the country could be set */
Modem::Modem(Host &host, const Options &opt)
	: host_(host), handle_(new struct modem()),
	  name_(opt.name ? opt.name : "modem"), dev_name_(opt.dev_name),
	  srate_(opt.rate), frag_(opt.rate / 200 > 0 ? opt.rate / 200 : 1),
	  min_rate_(MODEM_MIN_RATE), max_rate_(MODEM_MAX_RATE)
{
	handle_->self = this;
	fax_detect_ = opt.fax_detect;
	cng_ = new dsp::CngDetect();
	cng_->init(srate_);
	link_owner_ = new LinkOwner(*this);
	link_ = new dlink::Link;
	xmit_ = new char[XMIT_SIZE];
	xmit_size_ = XMIT_SIZE;
	sregs_init();
	kept_init();
	if (homolog_init(MODEM_DEFAULT_COUNTRY_CODE, nullptr)) {
		log_printf(1, "err: %s: no default country\n", name_);
		return;
	}
	if (opt.country && homolog_init(-1, opt.country))
		log_printf(0, "%s: bad country name %s, using default by "
			   "code!\n", name_, opt.country);
	ring_last_ = ring_first_ = static_cast<u32>(host_.now_ms());
	setup_config();
	logf("created: %" PRId32 " Hz, %s\n", srate_, country_name().c_str());
	voice_init();
	ok_ = true;
}

/* a call that is on ends (NO CARRIER to the DTE: the Host still
   serves), then everything goes */
Modem::~Modem()
{
	if (started_) {
		if (state_ != IDLE) {
			result_ = Result::NoCarrier;
			have_result_ = true;
		}
		stop_call();
	}
	delete[] xmit_;
	delete link_;
	delete link_owner_;
	delete[] scratch_;
	delete cng_;
	delete handle_;
}

/* the Modem behind a pump's struct modem */
Modem *Modem::of(struct modem *m)
{
	return m->self;
}

/* a debug line, after the modem's name (the caller ends it) */
void Modem::logf(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	log_printf(1, "%s: %s", name_, line);
}

/* the device reports an error: only a line in the log */
void Modem::error()
{
	logf("device error\n");
}

/* the S-registers that have defaults (ATZ sets them too; the others
   keep what they have) */
void Modem::sregs_init()
{
	static const struct {
		unsigned char reg, val;
	} defaults[] = {
		{ SREG_ESCAPE_CHAR, '+' }, { SREG_CR_CHAR, 13 },
		{ SREG_LF_CHAR, 10 }, { SREG_BS_CHAR, 8 },
		{ SREG_DIAL_TONE_WAIT_TIME, 2 },
		{ SREG_WAIT_CARRIER_AFTER_DIAL, 60 },
		{ SREG_DIAL_PAUSE_TIME, 2 },
		{ SREG_CARRIER_DETECT_RESPONSE_TIME, 6 },
		{ SREG_CARRIER_LOSS_DISCONNECT_TIME, 7 },
		{ SREG_DTMF_DURATION, 100 }, { SREG_ESCAPE_PROMPT_DELAY, 50 },
		{ SREG_ECHO, 1 }, { SREG_QUIET, 0 }, { SREG_VERBOSE, 1 },
		{ SREG_TONE_OR_PULSE, 1 }, { SREG_FLASH_TIMER, 20 },
		{ SREG_SPEAKER_CONTROL, 1 }, { SREG_SPEAKER_VOLUME, 3 },
		{ SREG_AUTOMODE, 1 }, { SREG_DP, DP_V92 },
		{ SREG_LINE_QUALITY_CONTROL, 0 }, { SREG_X_CODE, 4 },
		{ SREG_CD, 0 }, { SREG_FLOW_CONTROL, 0 },
		{ SREG_CONNNECT_MSG_FORMAT, 0 },
		{ SREG_CONNNECT_MSG_SPEED_SRC, 0 }, { SREG_ANS_DELAY, 2 },
		{ SREG_EC, 3 }, { SREG_COMP, 3 }, { SREG_MNP_BLOCK, 3 },
		{ SREG_CONNECT_PROTOCOL, 0 },
	};
	for (const auto &d : defaults)
		sregs_[d.reg] = d.val;
}

/* the country: the first of the table whose T.35 code is id, or whose
   name is name (when there is one), or is the same country of the ITU's
   list of tones as name (dsp/tones.h: UNITED_KINGDOM is UK), with its S8
   and S24 and its tones; else a country of that list alone, with the
   default's line settings; the Host told (Host::country()); -1 if there
   is none */
s16 Modem::homolog_init(s32 id, const char *name)
{
	const tones::Country *t = name && *name ? tones::find(name) : nullptr;
	const struct homolog_set *dflt = nullptr;
	for (const struct homolog_set *h = homolog_set; h->name; h++) {
		if (h->id == MODEM_DEFAULT_COUNTRY_CODE)
			dflt = h;
		if (h->id != id && !(name && *name &&
				     std::string_view(h->name) == name) &&
		    !(t && tones::find(h->name) == t))
			continue;
		homolog_ = h;
		tones_ = tones::find(h->name);
		tones_only_ = false;
		sregs_[SREG_DIAL_PAUSE_TIME] = h->params->DialPauseTime;
		sregs_[SREG_FLASH_TIMER] = h->params->HookFlashTime;
		host_.country(*this, h->name);
		return 0;
	}
	if (!t || !dflt)
		return -1;
	homolog_ = dflt;
	tones_ = t;
	tones_only_ = true;
	sregs_[SREG_DIAL_PAUSE_TIME] = dflt->params->DialPauseTime;
	sregs_[SREG_FLASH_TIMER] = dflt->params->HookFlashTime;
	host_.country(*this, t->name);
	return 0;
}

/* the country's name: the table's, or the ITU list's with the line
   settings' ("FIJI (USA line)") */
std::string Modem::country_name() const
{
	if (!tones_only_)
		return homolog_->name;
	return std::string(tones_->name) + " (" + homolog_->name +
		" line)";
}

/* the voice parameters' defaults (+VIP) */
void Modem::voice_init()
{
	voice_info_.comp_method = 1;
	voice_info_.sample_rate = 8000;
	voice_info_.rx_gain = 128;
	voice_info_.tx_gain = 128;
	voice_info_.dtmf_symbol = 0;
	voice_info_.tone1_freq = 933;
	voice_info_.tone2_freq = 0;
	voice_info_.tone_duration = 150;
	voice_info_.inactivity_timer = 0;
	voice_info_.silence_detect_sensitivity = 128;
	voice_info_.silence_detect_period = 50;
	voice_info_.tone_default = 100;
	voice_info_.ring_after = 50;
	voice_info_.ring_never = 10;
	voice_events_ = 0;
}

/*
 *    the call's states
 */

/* the call's state, to the Host */
LinkState Modem::link_state() const
{
	switch (state_) {
	case DP_ESTAB:
	case EC_ESTAB:
		return LinkState::Connecting;
	case ONLINE:
	case COMMAND_ONLINE:
		return LinkState::Online;
	case EC_DISC:
	case DP_DISC:
		return LinkState::HangingUp;
	default:
		return LinkState::Idle;
	}
}

/* the call goes to state s: whether the DTE's characters are commands,
   whether they (and the remote's) flow through the link, whether the
   link still runs, and the carrier once online; nothing if it is there
   already; 0 */
s16 Modem::set_state(State s)
{
	if (state_ == static_cast<u16>(s))
		return 0;
	logf("state %u -> %u\n", state_, static_cast<u16>(s));
	state_ = s;
	switch (s) {
	case IDLE:
		command_ = true;
		link_->set_online(false);
		link_->stop();
		break;
	case DP_ESTAB:
		command_ = false;
		link_->set_online(false);
		link_->stop();
		break;
	case EC_ESTAB:
		command_ = false;
		link_->set_online(false);
		break;
	case ONLINE:
		command_ = false;
		link_->set_online(true);
		carrier_ = true;
		break;
	case COMMAND_ONLINE:
		command_ = true;
		link_->set_online(false);
		break;
	case EC_DISC:
		link_->set_online(false);
		break;
	case DP_DISC:
		link_->set_online(false);
		link_->stop();
		break;
	}
	return 0;
}

/* the Host's hook to state, unless it is there already: the Host's
   result, the state kept only when it is 0 */
s16 Modem::set_hook(s16 state)
{
	s16 ret;
	if (hook_ == state)
		return 0;
	ret = host_.set_hook(*this, state);
	if (!ret)
		hook_ = state;
	return ret;
}

/* why the call ends, unless that is known already (only while a call
   is on) */
void Modem::set_cause(Cause c)
{
	if (stats_.call && !stats_.ended && stats_.cause == Cause::None)
		stats_.cause = c;
}

/* the call hangs up, by us (local: the pump and the link say goodbye)
   or not; the stop follows STOP_SAMPLES later, with NO CARRIER unless
   another result is due */
void Modem::hup(bool local)
{
	struct dp *dp = dp_;

	switch (state_) {
	case DP_ESTAB:
		dialed_ = false;
		if (local && dp && dp->op->hangup)
			dp->op->hangup(dp);
		set_state(DP_DISC);
		break;
	case EC_ESTAB:
	case ONLINE:
	case COMMAND_ONLINE:
		if (local && (cfg_.ec || cfg_.mnp_on)) {
			link_->disconnect();
			set_state(EC_DISC);
		} else {
			if (local && dp && dp->op->hangup)
				dp->op->hangup(dp);
			set_state(DP_DISC);
		}
		carrier_ = false;
		break;
	case EC_DISC:
		if (local && dp && dp->op->hangup)
			dp->op->hangup(dp);
		set_state(DP_DISC);
		return;
	default:
		return;
	}
	if (!have_result_) {
		result_ = Result::NoCarrier;
		have_result_ = true;
	}
	later(Later::Stop, STOP_SAMPLES);
}

/* the line starts: its samples counted from 0, the Host's start();
   0, or the Host's error */
s16 Modem::do_start()
{
	s16 ret;
	count_ = 0;
	ret = host_.start(*this);
	if (!ret)
		started_ = true;
	return ret;
}

/* a call starts (dialled or answered, its first pump there or due): its
   statistics anew, the link set up, off hook, the line started (with
   keep_line, the voice call's line held off hook goes on as it is); -1
   if any of it fails, the call then stopped with NO CARRIER */
s16 Modem::start_call(bool keep_line)
{
	auto fail = [this](const char *what) {
		log_printf(1, "err: %s: %s\n", name_, what);
		result_ = Result::NoCarrier;
		have_result_ = true;
		stop_call();
		return -1;
	};

	if (started_ && !keep_line && stop_call())
		return -1;
	have_result_ = false;
	stats_ = CallStats();
	stats_.call = true;
	/* V.cvm: +CVM's settings kept, this call's figures cleared */
	{
		struct dp_vcvm v = {};
		v.modes = vcvm_.modes;
		v.rate = vcvm_.rate;
		v.report = vcvm_.report;
		vcvm_ = v;
		vcvm_refresh();
	}
	stats_.fax = mode_ == MODEM_MODE_FAX;
	stats_.start = static_cast<u32>(host_.now_ms());
	dsp::zero(dp_stats_);
	setup_config();
	set_state(DP_ESTAB);
	if (sregs_[SREG_SPEAKER_CONTROL] & 1)
		host_.speaker(*this, sregs_[SREG_SPEAKER_VOLUME]);
	if (set_hook(MODEM_HOOK_OFF))
		return fail("cannot go off hook");
	xmit_head_ = xmit_tail_ = xmit_count_ = 0;
	command_ = false;
	dcr_ = dcr_create();
	dp_runtime_ = dp_runtime_create(handle());
	if (!dcr_ || !dp_runtime_)
		return fail("no memory for the call");
	if (line_law_)
		dp_runtime_set_law(dp_runtime_, line_law_);
	if (DCORE_HAS(dp_runtime_set_analog))
		dp_runtime_set_analog(dp_runtime_, host_.analog_line(*this));
	if (link_init())
		return fail("cannot set up the link");
	sregs_[SREG_RING_COUNTER] = 0;
	ring_count_ = 0;
	event_ = new_event_ = 0;
	timer_on_ = false;
	escape_count_ = 0;
	if (!(keep_line && started_) && do_start())
		return fail("the line does not start");
	return 0;
}

/* the call (or caller ID, or voice) ends: whatever runs goes, on hook,
   the statistics closed, command state, idle, and the result due
   reported; whether the line still runs (the Host could not stop it) */
s16 Modem::stop_call()
{
	run_ = Run::None;
	later_ = Later::None;
	if (started_) {
		if (host_.stop(*this))
			log_printf(1, "err: %s: the line does not stop\n",
				   name_);
		else
			started_ = false;
	}
	set_hook(MODEM_HOOK_ON);
	if (sregs_[SREG_SPEAKER_CONTROL] & 1)
		host_.speaker(*this, 0);
	caller_ = false;
	dialed_ = false;
	voice_held_ = false;
	dial_rest_[0] = '\0';
	/* an adaptive answer from fax mode that found out nothing: back in
	   fax mode */
	if (adapt_ == Adapt::Fax)
		set_mode(MODEM_MODE_FAX);
	adapt_ = Adapt::Off;
	if (stats_.call && !stats_.ended) {
		const char *dp = stats_.connected ? dp_name(stats_.dp) :
			nullptr;
		if (stats_.cause == Cause::None)
			stats_.cause = Cause::NoCarrier;
		stats_.ended = true;
		stats_.end = static_cast<u32>(host_.now_ms());
		stats_.tx1 = tx_rate_;
		stats_.rx1 = rx_rate_;
		logf("call over: %s, %" PRIu32 "/%" PRIu32 " bit/s, %" PRIu32
		     " ms, %s\n",
		     stats_.fax ? "fax" : dp ? dp : "no connection",
		     stats_.tx1, stats_.rx1, stats_.end - stats_.start,
		     cause_text[static_cast<s16>(stats_.cause)]);
	}
	/* data that did not go out is no command */
	if (!command_)
		xmit_head_ = xmit_tail_ = xmit_count_ = 0;
	command_ = true;
	commands_due_ = false;
	link_->exit();
	if (dp_) {
		struct dp *dp = dp_;
		dp_ = nullptr;
		dp->op->destroy(dp);
	}
	if (dp_runtime_) {
		dp_runtime_delete(dp_runtime_);
		dp_runtime_ = nullptr;
	}
	if (dcr_) {
		dcr_delete(dcr_);
		dcr_ = nullptr;
	}
	if (cid_) {
		CID_delete(cid_);
		cid_ = nullptr;
	}
	if (voice_) {
		VOICE_delete(voice_);
		voice_ = nullptr;
	}
	voice_unroute();
	if (fax_) {
		FAX_delete(fax_);
		fax_ = nullptr;
	}
	t38_ = false;
	set_state(IDLE);
	if (have_result_) {
		have_result_ = false;
		report(result_);
	}
	count_ = 0;
	return started_;
}

/* what to do once samples more of the line's samples have gone through,
   in place of what was due; due after 0 samples, it is done at once */
void Modem::later(Later what, u32 samples)
{
	later_ = what;
	later_at_ = count_ + samples;
	if (samples || what == Later::None)
		return;
	later_ = Later::None;
	switch (what) {
	case Later::Stop:
		stop_call();
		break;
	case Later::ChangeDp:
		change_dp();
		break;
	case Later::CallerIdStop:
		cid_stop();
		break;
	case Later::None:
		break;
	}
}

/*
 *    the link layer
 */

/* the link's configuration for this call (our side's: caller_) from
   +ES, S104 and S105, nothing in use yet; and whether the pumps may
   skip V.42's detection phase (qc_lapm) */
void Modem::setup_config()
{
	const u32 *es = kept("+ES");
	u16 comp = sregs_[SREG_COMP];
	Cfg c = {};

	if (caller_) {
		u32 rqst = es[0], fbk = es[1];
		c.ec_detector = true;
		switch (rqst) {
		case 2:
			c.ec = true;
			c.ec_detector = false;
			c.mnp = fbk != 3;
			break;
		case 3:
			c.ec = true;
			c.mnp = fbk != 3;
			break;
		case 4:
			c.mnp = true;
			c.mnp_first = true;
			break;
		default:
			break;
		}
		if (c.ec && fbk == 4) {
			c.ec = false;
			c.mnp = true;
			c.mnp_first = true;
		}
		c.required = (c.ec || c.mnp) && fbk >= 2;
	} else {
		u32 fbk = es[2];
		c.ec = fbk >= 2 && fbk != 6;
		c.mnp = fbk >= 2 && fbk != 5;
		c.required = fbk >= 4 && fbk <= 6;
		c.ec_detector = true;
	}
	c.mnp_info = 64 * (1 + (sregs_[SREG_MNP_BLOCK] & 3));
	c.tx_win = c.rx_win = dlink::LAPM_MAX_WIN;
	c.tx_info = c.rx_info = dlink::LAPM_MAX_INFO;
	c.comp = c.ec && (comp & 2) ? 3 : 0;
	c.mnp5 = c.mnp && (comp & 1);
	c.dict_size = dlink::V42BIS_MAX_CODEWORDS;
	c.max_string = dlink::V42BIS_MAX_STRING;
	c.v44 = false;
	c.mnp_on = false;
	cfg_ = c;
	dsp_info_.qc_lapm = c.ec && c.ec_detector;
}

/* the link set up for the call as configured; -1 if it cannot be */
s16 Modem::link_init()
{
	dlink::Config c;
	c.ec = cfg_.ec;
	c.detect = cfg_.ec_detector;
	c.mnp = cfg_.mnp;
	c.mnp_first = cfg_.mnp_first;
	c.required = cfg_.required;
	c.mnp_info = cfg_.mnp_info;
	c.mnp5 = cfg_.mnp5;
	c.tx_win = cfg_.tx_win;
	c.rx_win = cfg_.rx_win;
	c.tx_info = cfg_.tx_info;
	c.rx_info = cfg_.rx_info;
	c.comp = cfg_.comp;
	c.dict_size = cfg_.dict_size;
	c.max_string = cfg_.max_string;
	return link_->init(link_owner_, caller_, c);
}

/* what XID or MNP's LRs agreed: the call's compression */
void Modem::negotiated(s16 comp, bool v44, s32 dict_size,
		       s32 max_string, bool mnp5)
{
	cfg_.comp = comp;
	cfg_.v44 = v44;
	cfg_.dict_size = dict_size;
	cfg_.max_string = max_string;
	cfg_.mnp5 = mnp5;
	logf("negotiated: compression %d%s, %" PRId32 " codewords, strings of %"
	     PRId32 "%s\n",
	     comp, v44 ? " (V.44)" : "", dict_size, max_string,
	     mnp5 ? ", MNP 5" : "");
}

/* the call's error correction, as CONNECT and ATI11 name it */
const char *Modem::protocol_name() const
{
	return cfg_.ec ? "LAPM" : cfg_.mnp_on ? "MNP" : "NONE";
}

/* the call's compression, as CONNECT and ATI11 name it */
const char *Modem::compression_name() const
{
	if (cfg_.mnp_on)
		return cfg_.mnp5 ? "MNP5" : "NONE";
	if (cfg_.ec && cfg_.comp)
		return cfg_.v44 ? "V44" : "V42bis";
	return "NONE";
}

/* the pump's or the link's progress s: the link started when the pump
   connects, CONNECT when the link is up, the hang-up when either
   fails or the remote leaves */
void Modem::update_status(Status s)
{
	switch (s) {
	case ST_OK:
		break;
	case ST_DP_LINK:
		/* adaptive answer: a data call (DATA, then its CONNECT) */
		if (adaptive_answering())
			report(Result::Data);
		adapt_ = Adapt::Off;
		if (dp_ && (dp_->id == DP_V23 || dp_->id == DP_B103 ||
			    dp_->id == DP_V21)) {
			cfg_.ec = false;
			cfg_.mnp = false;
		}
		/* V.cvm: LAPM, and no detection phase (11.1/V.cvm: both
		   indicated it in V.8) */
		if (dp_ && dp_->id == DP_VCVM) {
			cfg_.ec = true;
			cfg_.mnp = false;
		}
		set_state(EC_ESTAB);
		link_->start(cfg_.ec || cfg_.mnp, cfg_.ec_detector &&
			     !(caller_ && dsp_info_.qc_lapm) &&
			     !(dp_ && dp_->id == DP_VCVM));
		break;
	case ST_PACK_LINK:
		cfg_.ec = link_->ec();
		cfg_.mnp_on = link_->protocol() == dlink::Protocol::Mnp;
		break;
	case ST_EC_LINK:
		if (state_ == ONLINE || state_ == COMMAND_ONLINE)
			break;
		set_state(ONLINE);
		if (!stats_.connected) {
			stats_.connected = true;
			stats_.up = static_cast<u32>(host_.now_ms());
			stats_.dp = modulation(dp_stats_, dp_);
			stats_.tx0 = tx_rate_;
			stats_.rx0 = rx_rate_;
		}
		report(Result::Connect);
		if (sregs_[SREG_SPEAKER_CONTROL] & 1)
			host_.speaker(*this, 0);
		break;
	case ST_EC_RELEASE:
	case ST_EC_ERROR:
		set_cause(s == ST_EC_RELEASE ? Cause::RemoteCleardown :
			  Cause::ProtocolFailure);
		result_ = Result::NoCarrier;
		have_result_ = true;
		hup(state_ == EC_DISC);
		break;
	case ST_ERROR:
	case ST_DP_ERROR:
	case ST_NOCARRIER:
	case ST_NODIALTONE:
	case ST_BUSY:
	case ST_NOANSWER:
		switch (s) {
		case ST_NODIALTONE:
			set_cause(Cause::NoDialtone);
			result_ = Result::NoDialtone;
			break;
		case ST_BUSY:
			set_cause(Cause::Busy);
			result_ = Result::Busy;
			break;
		case ST_NOANSWER:
			set_cause(Cause::NoAnswer);
			result_ = Result::NoAnswer;
			break;
		default:
			set_cause(stats_.connected ? Cause::CarrierLost :
				  Cause::TrainingFailed);
			result_ = Result::NoCarrier;
			break;
		}
		have_result_ = true;
		hup(state_ == EC_DISC || state_ == DP_DISC);
		break;
	}
}

/* the pumps' bits for the line, from the link */
s32 Modem::get_bits(s16 nbits, u8 *buf, s32 n)
{
	s32 ret = link_->get_bits(nbits, buf, n);
	log_data(this, LOG_TX_BITS, buf, n);
	return ret;
}

/* the pumps' bits from the line, to the link */
s32 Modem::put_bits(s16 nbits, const u8 *buf, s32 n)
{
	s32 ret = link_->put_bits(nbits, buf, n);
	log_data(this, LOG_RX_BITS, buf, n);
	return ret;
}

/* DTE data the remote does not have yet */
bool Modem::tx_pending() const
{
	return xmit_count_ > 0 || link_->tx_pending();
}

/* LAPM's or MNP's frames not sent or not acknowledged */
bool Modem::link_tx_pending() const
{
	return link_->tx_pending();
}

/*
 *    the DTE
 */

/* characters from the DTE: one aborts a call being set up; online they
   are checked for the escape; they wait in the buffer (as many as fit),
   and in command state the interpreter takes them; how many were
   taken */
s32 Modem::dte_input(const char *buf, s32 n)
{
	s16 k = 0;

	if (n <= 0)
		return 0;
	if ((state_ == DP_ESTAB || state_ == EC_ESTAB) && !command_) {
		logf("a character from the DTE: hanging up\n");
		set_cause(Cause::LocalHangup);
		hup(true);
		return n;
	}
	if (state_ == ONLINE)
		check_escape(buf, n);
	while (k < n && xmit_count_ < xmit_size_) {
		s16 run = xmit_size_ - xmit_head_;
		if (run > xmit_size_ - xmit_count_)
			run = xmit_size_ - xmit_count_;
		if (run > n - k)
			run = n - k;
		memcpy(xmit_ + xmit_head_, buf + k, run);
		xmit_head_ = (xmit_head_ + run) % xmit_size_;
		xmit_count_ += run;
		k += run;
	}
	if (k < n)
		logf("DTE buffer full: %d of %d characters taken\n", k, n);
	if (voice_drain_)
		voice_drain();
	if (command_)
		at_process();
	return k;
}

/* up to n of the DTE's characters, oldest first: how many */
s32 Modem::get_chars(char *buf, s32 n)
{
	s16 k = 0;
	while (k < n && xmit_count_ > 0) {
		s16 run = xmit_size_ - xmit_tail_;
		if (run > xmit_count_)
			run = xmit_count_;
		if (run > n - k)
			run = n - k;
		memcpy(buf + k, xmit_ + xmit_tail_, run);
		xmit_tail_ = (xmit_tail_ + run) % xmit_size_;
		xmit_count_ -= run;
		k += run;
	}
	return k;
}

/* n characters to the DTE: how many the Host took */
s32 Modem::put_chars(const char *buf, s32 n)
{
	int ret;
	if (n <= 0)
		return 0;
	ret = host_.dte_write(*this, buf, n);
	return ret < 0 ? 0 : ret;
}

/* a string to the DTE */
void Modem::puts(const char *s)
{
	put_chars(s, static_cast<s32>(strlen(s)));
}

/* the end of a line to the DTE: S3, S4 */
void Modem::crlf()
{
	put_chars(reinterpret_cast<const char *>(&sregs_[SREG_CR_CHAR]), 2);
}

/* the escape (+++): this input (online) after the last one's, as the
   rules of the guard time have it: all of it S2 and the S2s at most
   three, the first a guard time after the input before, the others
   within one; the third arms the escape a guard time later.  The S2s
   so far */
s16 Modem::check_escape(const char *buf, s32 n)
{
	u32 now = static_cast<u32>(host_.now_ms());
	s32 since = static_cast<s32>(now - last_esc_check_);
	bool escape = escape_count_ + n <= 3;

	for (s16 i = 0; escape && i < n; i++)
		escape = static_cast<unsigned char>(buf[i]) ==
			 sregs_[SREG_ESCAPE_CHAR];
	if (escape)
		escape = escape_count_ ?
			since < static_cast<s32>(ESCAPE_MS) :
			since >= static_cast<s32>(ESCAPE_MS);
	last_esc_check_ = now;
	if (!escape) {
		escape_count_ = 0;
		return 0;
	}
	escape_count_ += n;
	if (escape_count_ == 3)
		schedule_event(EV_ESCAPE, ESCAPE_MS);
	return escape_count_;
}

/*
 *    the line's samples
 */

/* n samples of the line in, n out: the input's DC taken out, whatever
   runs on them, the line's power while the data call is up, then what
   falls due after them and the commands waiting after a fax or voice
   command */
void Modem::process(const s16 *in, s16 *out, s32 n)
{
	if (n <= 0)
		return;
	if (n > scratch_len_) {
		delete[] scratch_;
		scratch_ = new s16[n];
		scratch_len_ = n;
	}
	std::copy_n(in, n, scratch_);
	if (dcr_ && !(DCORE_HAS(dp_runtime_pcm) && dp_runtime_ &&
		      dp_runtime_pcm(dp_runtime_)))
		dcr_process(dcr_, scratch_, n);
	/* adaptive answer: a calling fax machine's CNG */
	if (adapt_ != Adapt::Off && state_ == DP_ESTAB) {
		for (s32 i = 0; i < n; i++)
			cng_->sample(scratch_[i]);
		if (cng_->bursts)
			fax_found("CNG");
		/* a burst cut short as our answer tone starts: a fax
		   machine that stops its CNG on hearing it, silent until
		   the DIS (the tone reaches it, its burst ends and its end
		   reaches us within a round trip) */
		if (cng_->cut != cng_cut_) {
			cng_cut_ = cng_->cut;
			if (answer_tone_at_ && count_ >= answer_tone_at_ &&
			    count_ - answer_tone_at_ <=
			    static_cast<u32>(srate_) * 3 / 2)
				fax_found("CNG cut short by the answer tone");
		}
	}
	log_data(this, LOG_RX_SAMPLES, scratch_, 2 * n);
	run(scratch_, out, n);
	log_data(this, LOG_TX_SAMPLES, out, 2 * n);
	if (stats_.connected && !stats_.ended && !stats_.fax &&
	    (state_ == ONLINE || state_ == COMMAND_ONLINE)) {
		for (s32 i = 0; i < n; i++) {
			stats_.rx_pow +=
				static_cast<double>(scratch_[i]) * scratch_[i];
			stats_.tx_pow += static_cast<double>(out[i]) * out[i];
		}
		stats_.samples += n;
	}
	count_ += n;
	samples_at_ms_ = host_.now_ms();
	if (later_ != Later::None &&
	    static_cast<s64>(count_ - later_at_) >= 0)
		later(later_, 0);
	if (commands_due_) {
		commands_due_ = false;
		if (command_ && xmit_count_ > 0)
			at_process();
	}
}

/* the samples through what runs: a pump, caller ID, voice, fax, or
   nothing (silence) */
void Modem::run(s16 *in, s16 *out, s32 n)
{
	switch (run_) {
	case Run::Dp:
		dp_process(in, out, n);
		break;
	case Run::CallerId:
		cid_process(in, out, n);
		break;
	case Run::Voice:
		voice_process(in, out, n);
		break;
	case Run::Fax:
		fax_process(in, out, n);
		break;
	case Run::None:
		std::fill_n(out, n, 0);
		break;
	}
}

/* the samples through the current pump, frag_ at most at a time, each
   piece's status acted on; silence once there is no pump; after a
   failure the rest goes to the pump, and from the next samples on
   nothing runs */
void Modem::dp_process(s16 *in, s16 *out, s32 n)
{
	bool failed = false;

	for (s32 done = 0; done < n;) {
		s32 len = n - done < frag_ ? n - done : frag_;
		int st;
		if (!dp_ || run_ != Run::Dp) {
			std::fill_n(out + done, n - done, 0);
			break;
		}
		st = dp_->op->process(dp_, in + done, out + done, len);
		done += len;
		switch (st) {
		case DPSTAT_OK:
			break;
		case DPSTAT_CONNECT:
			if (state_ == IDLE || state_ == DP_ESTAB)
				update_status(ST_DP_LINK);
			break;
		case DPSTAT_CHANGEDP:
			change_dp();
			break;
		case DPSTAT_DIALED:
			if (state_ == DP_ESTAB)
				dialed();
			break;
		case DPSTAT_FAX:
			fax_found(dp_->id == DP_B103 ?
				  "silent through the automode chain" :
				  "V.8");
			break;
		case DPSTAT_ERROR:
			failed = true;
			update_status(ST_DP_ERROR);
			break;
		case DPSTAT_NODIALTONE:
			failed = true;
			update_status(ST_NODIALTONE);
			break;
		case DPSTAT_BUSY:
			failed = true;
			update_status(ST_BUSY);
			break;
		case DPSTAT_NOANSWER:
			failed = true;
			update_status(ST_NOANSWER);
			break;
		default:
			failed = true;
			update_status(ST_ERROR);
			break;
		}
	}
	if (failed && run_ == Run::Dp)
		run_ = Run::None;
}

/* the pump's name in the driver table, NULL for an id it does not
   have */
const char *Modem::dp_name(s16 id) const
{
	const DpDriver *d =
		id > 0 ? find_driver(static_cast<u32>(id)) : nullptr;
	return d ? d->name : nullptr;
}

/* the next pump: in fax mode the fax engine; else the one a pump asked
   for, or +MS's (V.8 starting V.34 and up; V.34 at most on an analogue
   line), created before the old one goes; if it cannot be, the call
   hangs up on the old one */
void Modem::change_dp()
{
	const DpDriver *d;
	struct dp *dp = nullptr, *old;
	u32 id, drv;

	if (mode_ == MODEM_MODE_FAX) {
		fax_start();
		return;
	}
	if (dp_requested_) {
		id = drv = dp_requested_;
		dp_requested_ = 0;
	} else {
		id = sregs_[SREG_DP];
		if (id == DP_K56)
			id = DP_V90;
		/* an analogue line has no V.91 (that is between two
		   digital modems); V.90 and V.92 run as the analogue modem
		   (dp_runtime_set_analog()) */
		if (host_.analog_line(*this) && id == DP_V91)
			id = DP_V34;
		drv = id == DP_V34 || id == DP_V90 || id == DP_V91 ||
				      id == DP_V92 || id == DP_VCVM ?
			      static_cast<u32>(DP_V8) :
			      id;
	}
	logf("pump %u (driver %u), %s\n", id, drv,
	     caller_ ? "caller" : "answerer");
	/* the answering pump's answer tone after its 0.2 s of silence */
	if (!caller_ && !dp_)
		answer_tone_at_ =
			count_ + static_cast<u32>(srate_) / 5;
	d = find_driver(drv);
	if (d && d->op)
		dp = d->op->create(handle(), static_cast<enum DP_ID>(id),
				   caller_, srate_, frag_, d->op);
	if (!dp) {
		log_printf(1, "err: %s: cannot create pump %u\n", name_, id);
		hup(true);
		return;
	}
	old = dp_;
	dp_ = dp;
	run_ = Run::Dp;
	if (old)
		old->op->destroy(old);
}

/*
 *    dialling and answering
 */

/* call progress for a dial: the line stopped if caller ID or voice has
   it, the caller, the pump created with the dial string hidden when the
   network dials it (unless the pump has to wait for a dial tone; on an
   analogue line, Host::analog_line(), the modem dials it); -1 if there
   is a pump already or none can be made */
s16 Modem::dial_start()
{
	const DpDriver *d;
	char first;

	if (dp_)
		return -1;
	if (started_ && !voice_held_ && stop_call())
		return -1;
	caller_ = true;
	d = find_driver(DP_CALLPROG);
	first = dialing_[0];
	if (!host_.analog_line(*this))
		dialing_[0] = '\0';
	if (d && d->op)
		dp_ = d->op->create(handle(), DP_CALLPROG, 1, srate_, frag_,
				    d->op);
	dialing_[0] = first;
	if (!dp_) {
		log_printf(1, "err: %s: cannot create CALLPROG.\n", name_);
		return -1;
	}
	run_ = Run::Dp;
	return 0;
}

/* ATD<number>, ATDL (number NULL: the last one again): call progress and
   the call; on a call held after a ';', number is dialled on it (a
   blank for none: the pump goes on to the handshake); 0, or -1 */
s16 Modem::dial(const char *number)
{
	if (dialed_) {
		size_t len = strlen(dialing_);
		if (!number || len >= sizeof(dialing_) - 1)
			return -1;
		copy_dial(dialing_ + len, sizeof(dialing_) - len,
			  *number ? number : " ");
		logf("dial more: %s\n", dialing_);
		dialed_ = false;
		command_ = false;
		return 0;
	}
	if (number)
		copy_dial(dial_string_, sizeof(dial_string_), number);
	strcpy(dialing_, dial_string_);
	logf("dial: %s\n", dialing_);
	dp_requested_ = 0;
	/* (on a voice call's held line: call progress there, as after the
	   number is dialled, its answer tone to come) */
	bool held = voice_held_;
	if (dial_start())
		return -1;
	voice_held_ = false;
	return start_call(held);
}

/* call progress has dialled up to the ';': the call is held in command
   state, and the rest of the ATD line runs (OK when there is none) */
void Modem::dialed()
{
	char cmd[sizeof(dial_rest_)];
	s16 ret;

	dialed_ = true;
	command_ = true;
	if (!dial_rest_[0]) {
		report(Result::Ok);
		return;
	}
	dsp::copy_array(cmd, dial_rest_);
	dial_rest_[0] = '\0';
	logf("run cmd: %s\n", cmd);
	ret = at_command(cmd);
	if (ret < 0)
		report(Result::Error);
	else if (!ret)
		report(Result::Ok);
}

/* ATO: on a held call the handshake (as ATD); in online command state
   back online with CONNECT; -1 otherwise */
s16 Modem::online()
{
	if (dialed_)
		return dial("");
	if (state_ != COMMAND_ONLINE)
		return -1;
	command_ = false;
	set_state(ONLINE);
	report(Result::Connect);
	return 0;
}

/* ATA: the answering pump after the answer delay (S92 s, or 10 ms), on
   the held call (call-back: we answer the call we dialled) or a new
   one; -1 if a pump runs already or the call cannot start */
s16 Modem::answer(bool adaptive)
{
	u32 delay =
		sregs_[SREG_ANS_DELAY] ?
			static_cast<u32>(sregs_[SREG_ANS_DELAY]) *
				srate_ :
			srate_ / 100;
	/* a voice call's held line: off hook a while already */
	bool held = voice_held_;

	if (held)
		delay = srate_ / 100;
	voice_held_ = false;

	if (dialed_) {
		dialed_ = false;
		command_ = false;
		caller_ = false;
		setup_config();
		link_->exit();
		if (link_init()) {
			log_printf(1, "err: %s: cannot set up the link\n",
				   name_);
			hup(true);
			return -1;
		}
		dp_requested_ = 0;
		later(Later::ChangeDp, delay);
		return 0;
	}
	if (dp_) {
		log_printf(1, "err: %s: answer: a pump runs\n", name_);
		return -1;
	}
	if (started_ && !held && stop_call())
		return -1;
	caller_ = false;
	dp_requested_ = 0;
	/* adaptive answer: +FAA=1 in data or fax mode (from fax mode the
	   data answer runs, in data mode, until a fax is found), or the
	   program's own fax receiver in data mode */
	fax_caller_ = false;
	cng_->init(srate_);
	cng_cut_ = 0;
	answer_tone_at_ = 0;
	if (adaptive && kept("+FAA")[0] && (mode_ == MODEM_MODE_DATA ||
					    mode_ == MODEM_MODE_FAX)) {
		adapt_ = mode_ == MODEM_MODE_FAX ? Adapt::Fax : Adapt::Data;
		set_mode(MODEM_MODE_DATA);
		logf("adaptive answer (+FAA=1, from %s mode)\n",
		     adapt_ == Adapt::Fax ? "fax" : "data");
	} else if (fax_detect_ && mode_ == MODEM_MODE_DATA) {
		adapt_ = Adapt::Detect;
	}
	later(Later::ChangeDp, delay);
	return start_call(held);
}

/* adaptive answer: the call we answer is a fax machine's (why: its
   CNG, its V.8 CM, T.38).  With +FAA=1: FAX to the DTE, class 1
   (+FCLASS=1), the fax engine as the answerer after the answer tone
   sent already (its CED cut short), its CONNECT for +FTH=3 to follow.
   For the program's fax receiver (Options::fax_detect): fax_caller(),
   and after a V.8 CM no more data answer (the line silent) */
void Modem::fax_found(const char *why)
{
	if (fax_caller_ || adapt_ == Adapt::Off)
		return;
	fax_caller_ = true;
	logf("adaptive answer: a fax call (%s)\n", why);
	if (adapt_ == Adapt::Detect) {
		adapt_ = Adapt::Off;
		if (!starts_with(why, "CNG"))
			run_ = Run::None;
		return;
	}
	u64 sent = count_ > answer_tone_at_ && answer_tone_at_ ?
		(count_ - answer_tone_at_) * 1000 / srate_ : 0;
	adapt_ = Adapt::Off;
	set_mode(MODEM_MODE_FAX);
	stats_.fax = true;
	report(Result::Fax);
	if (fax_start() == 0 && DCORE_HAS(FAX_answer_tone))
		FAX_answer_tone(fax_, static_cast<unsigned>(sent));
}

/* MDMPRM_ADAPTIVE: 1 while answering adaptively (a V.8 fax call, or
   a caller silent through the whole automode chain, told with
   DPSTAT_FAX), else 0 */
s16 Modem::adaptive_param() const
{
	if (caller_)
		return 0;
	switch (adapt_) {
	case Adapt::Detect:
	case Adapt::Data:
	case Adapt::Fax:
		return 1;
	default:
		return 0;
	}
}

/* the Host hangs up (DTR, HUPCL) */
void Modem::hangup()
{
	if (state_ != IDLE)
		set_cause(Cause::LocalHangup);
	else if (voice_held_)
		stop_call();
	hup(true);
}

/* the Host hangs up, and whatever is due happens now */
void Modem::hangup_now()
{
	hangup();
	if (later_ != Later::None)
		later(later_, 0);
}

/* the line ended the call (the SIP call is over): at once; a voice
   dial's NO CARRIER, a voice call's end */
void Modem::line_dropped()
{
	if (voice_dialing_) {
		voice_dial_end(Result::NoCarrier);
		return;
	}
	if (voice_) {
		voice_line_gone();
		return;
	}
	if (voice_held_) {
		logf("voice: the line ended the held call\n");
		stop_call();
		return;
	}
	if (state_ != IDLE)
		set_cause(Cause::LineDropped);
	hangup_now();
}

/* see dcore.h */
void Modem::line_answered()
{
	if (!voice_dialing_)
		return;
	logf("voice dial: answered\n");
	voice_dialing_ = false;
	report(Result::Ok);
	command_ = true;
	commands_due_ = true;
}

/* see dcore.h: a data call not yet up keeps r for its end (a voice call
   taken as answered on its early media's tones ends with the line's
   line_dropped()) */
void Modem::line_failed(Result r)
{
	if (voice_dialing_) {
		voice_dial_end(r);
		return;
	}
	if (voice_ || state_ == IDLE || stats_.connected || have_result_)
		return;
	set_cause(r == Result::Busy ? Cause::Busy :
		  r == Result::NoAnswer ? Cause::NoAnswer : Cause::NoCarrier);
	result_ = r;
	have_result_ = true;
}

/* ATH (MODEM_HOOK_ON), ATH1: a call hangs up when the hook goes on;
   nothing if the hook is there already; the Host's result */
s16 Modem::hook(s16 state)
{
	if (state == MODEM_HOOK_ON && voice_held_) {
		stop_call();
		return 0;
	}
	if (hook_ == state)
		return 0;
	if (state_ != IDLE) {
		set_cause(Cause::LocalHangup);
		hup(true);
	}
	return set_hook(state);
}

/* ATZ: the call hung up (its stop still to come), the line stopped or on
   hook; the S-registers, +MS, modem-on-hold, the kept parameters and
   data mode as at the start, the country's S8 and S24 again; 0 */
s16 Modem::reset()
{
	if (state_ != IDLE) {
		set_cause(Cause::LocalHangup);
		hup(true);
	} else if (started_) {
		stop_call();
	} else {
		set_hook(MODEM_HOOK_ON);
	}
	set_state(IDLE);
	command_ = true;
	min_rate_ = MODEM_MIN_RATE;
	max_rate_ = MODEM_MAX_RATE;
	moh_off_ = 0;
	moh_timer_ = 5;
	moh_req_ = 0;
	sregs_init();
	kept_init();
	sregs_[SREG_DIAL_PAUSE_TIME] = homolog_->params->DialPauseTime;
	sregs_[SREG_FLASH_TIMER] = homolog_->params->HookFlashTime;
	set_mode(MODEM_MODE_DATA);
	return 0;
}

/* +FCLASS: data, fax or voice; the voice engine stops when voice is
   left, its line held off hook (V.253 5.3.2: ATA or ATD then starts
   the fax or data handshake on the call) unless it was dialling; 0 */
s16 Modem::set_mode(enum MODEM_MODE mode)
{
	if (mode == mode_)
		return 0;
	if (mode_ == MODEM_MODE_VOICE && voice_) {
		/* (the handset's alone: no call to hold) */
		if (voice_dialing_ || mode == MODEM_MODE_VOICE ||
		    voice_device_ == VOICE_DEVICE_HANDSET)
			voice_stop();
		else
			voice_hold();
	}
	mode_ = mode;
	return 0;
}

/* ATL, ATM: the speaker on at S30 with ATM2, else off */
void Modem::update_speaker()
{
	host_.speaker(*this, sregs_[SREG_SPEAKER_CONTROL] == 2 ?
		      sregs_[SREG_SPEAKER_VOLUME] : 0);
}

/* the network's G.711 law, for this call's pumps and the next ones' */
void Modem::set_line_law(s16 law)
{
	if (law == line_law_)
		return;
	line_law_ = law;
	if (dp_runtime_ && law)
		dp_runtime_set_law(dp_runtime_, law);
}

/*
 *    result codes and the call's statistics
 */

/* V.cvm's block from +CVM's settings; S44 and S45 from the pump's
   figures of the call (the last one's, once it has ended) */
void Modem::vcvm_refresh()
{
	const u32 *c = kept("+CVM");
	if (c) {
		vcvm_.modes = static_cast<int>(c[0]);
		vcvm_.rate = static_cast<int>(c[1]);
		vcvm_.report = static_cast<int>(c[2]);
	}
	if (vcvm_.on) {
		sregs_[SREG_VCVM_TONES] = static_cast<unsigned char>(
			std::min(vcvm_.tones_rx, 255));
		sregs_[SREG_VCVM_RENEGS] =
			static_cast<unsigned char>(std::min(vcvm_.renegs, 255));
	}
}

/* result r to the DTE (unless ATQ1), in words (ATV1) or as a number; a
   CONNECT after the lines AT&A asks for, with the receive rate in data
   mode (unless ATX0) and AT\V's protocol */
void Modem::report(Result r)
{
	bool verbose = sregs_[SREG_VERBOSE] != 0;
	std::string line;

	logf("result: %s\n", result_text[static_cast<s16>(r)]);
	if (sregs_[SREG_QUIET])
		return;
	if (r == Result::Connect) {
		u16 a = sregs_[SREG_CONNNECT_MSG_FORMAT];
		s16 m = modulation(dp_stats_, dp_);
		const char *comp = compression_name();
		if ((a & 1) && (m || verbose)) {
			const char *name = dp_name(m);
			if (verbose)
				line = std::string("Modulation: ") +
					(name ? name : "NONE");
			else
				line = dec(1000 + m);
			puts(line);
			crlf();
		}
		if (a & 2) {
			if (verbose)
				line = std::string("Protocol: ") +
					protocol_name();
			else
				line = dec(cfg_.ec ? 77 : cfg_.mnp_on ? 78 : 76);
			puts(line);
			crlf();
		}
		if ((a & 3) == 3) {
			const std::string_view c = comp;
			if (verbose)
				line = "Compression: " + std::string(c);
			else
				line = dec(c == "MNP5" ? 66 : c == "V42bis" ? 67 :
					   c == "V44" ? 68 : 69);
			puts(line);
			crlf();
		}
		if ((a & 4) && verbose && tx_rate_) {
			line = "TxRate: " + dec(tx_rate_);
			puts(line);
			crlf();
		}
	}
	/* V.cvm's CONNECT: the coded rate and /CVM (12.2 c)/V.cvm) */
	bool cvm = modulation(dp_stats_, dp_) == DP_VCVM;
	if (r == Result::Connect && mode_ == MODEM_MODE_DATA && rx_rate_ &&
	    sregs_[SREG_X_CODE]) {
		if (!verbose) {
			line = dec(connect_code(rx_rate_));
		} else if (sregs_[SREG_CONNECT_PROTOCOL]) {
			const std::string_view comp = compression_name();
			line = "CONNECT " + dec(rx_rate_) + (cvm ? "/CVM" : "") +
				"/" + protocol_name();
			if (comp != "NONE")
				line += "/" + std::string(comp);
		} else {
			line = "CONNECT " + dec(rx_rate_) + (cvm ? "/CVM" : "");
		}
	} else if (verbose) {
		line = result_text[static_cast<s16>(r)];
	} else {
		line = dec(r == Result::Fax ? 88 : r == Result::Data ? 89 :
			   static_cast<s16>(r));
	}
	puts(line);
	crlf();
}

/* ATI11: the last call's statistics (or this one's), a row each: its
   label padded to 36 columns, then the value; "No Previous Call" before
   the first */
void Modem::call_stats()
{
	auto row = [this](const std::string &label, const std::string &value) {
		std::string line = label;
		if (line.size() < 36)
			line.append(36 - line.size(), ' ');
		puts(line + value);
		crlf();
	};
	auto number = [&row](const char *label, u64 n) {
		row(label, dec(n));
	};
	auto decibels = [&row](const char *label, double db) {
		row(label, fixed(db, 1));
	};

	if (!stats_.call) {
		puts("No Previous Call");
		crlf();
		return;
	}
	row("Description", "Status");
	row("-----------", "------");
	if (stats_.fax) {
		row("Last Connection", stats_.connected ? "Fax" : "None");
	} else if (!stats_.connected) {
		row("Last Connection", "None");
	} else {
		const struct dp_stats &d = dp_stats_;
		const dlink::Stats &l = link_->stats();
		const char *name = dp_name(stats_.dp);
		row("Last Connection", name ? name : "Unknown");
		number("Initial Transmit Carrier Rate", stats_.tx0);
		number("Initial Receive Carrier Rate", stats_.rx0);
		number("Final Transmit Carrier Rate",
		       stats_.ended ? stats_.tx1 : tx_rate_);
		number("Final Receive Carrier Rate",
		       stats_.ended ? stats_.rx1 : rx_rate_);
		row("Protocol Negotiation Result", protocol_name());
		row("Data Compression Result", compression_name());
		if (d.have & DP_STATS_SNR)
			decibels("Estimated SNR (dB)", d.snr);
		if (stats_.samples) {
			decibels("Receive Signal Power Level (dBm0)",
				 10 * log10(stats_.rx_pow / stats_.samples /
					    DP_DBM0_POWER + 1e-9));
			decibels("Transmit Signal Power Level (dBm0)",
				 10 * log10(stats_.tx_pow / stats_.samples /
					    DP_DBM0_POWER + 1e-9));
		}
		if (d.have & DP_STATS_RTD) {
			row("Round Trip Delay (ms)", dec(d.rtd));
		}
		if (d.have & DP_STATS_NEAR_ECHO)
			decibels("Near Echo Level (dBm0)", d.near_echo);
		if (d.have & DP_STATS_FAR_ECHO)
			decibels("Far Echo Level (dBm0)", d.far_echo);
		number("Characters Sent", l.tx_chars);
		number("Characters Received", l.rx_chars);
		if (cfg_.ec || cfg_.mnp_on) {
			number("Transmit Frame Count", l.tx_frames);
			number("Transmit Frame Error Count", l.tx_again);
			number("Receive Frame Count", l.rx_frames);
			number("Receive Frame Error Count", l.rx_errors);
		}
		number("Retrains by Local Modem", d.retrains_local);
		number("Retrains Requested by Remote Modem",
		       d.retrains_remote);
		number("Rate Renegotiation by Local Modem", d.renegs_local);
		number("Rate Renegotiation by Remote Modem", d.renegs_remote);
	}
	if (stats_.connected) {
		u32 s = ((stats_.ended ? stats_.end :
			       static_cast<u32>(host_.now_ms())) -
			      stats_.up) / 1000;
		row("Call Duration", dec(s / 3600) + ":" + dec(s / 60 % 60, 2) +
		    ":" + dec(s % 60, 2));
	}
	if (stats_.ended)
		row("Call Termination Cause",
		    cause_text[static_cast<s16>(stats_.cause)]);
}

/*
 *    timers and events
 */

/* events (mask) due ms from now: the timer's deadline is that */
void Modem::schedule_event(u16 mask, u32 ms)
{
	new_event_ |= mask;
	timer_at_ = static_cast<u32>(host_.now_ms()) + ms;
	timer_on_ = true;
}

/* the timer's events, if its deadline has passed; a voice dial's
   timers */
void Modem::poll()
{
	u16 ev;

	/* (a call stopped while its line had no audio: at once) */
	if (later_ == Later::Stop &&
	    host_.now_ms() - samples_at_ms_ >= STOP_IDLE_MS)
		later(Later::Stop, 0);

	if (voice_dialing_ && !voice_dial_digits_)
		voice_dial_check();
	if (timer_on_ &&
	    static_cast<s32>(host_.now_ms() - timer_at_) >= 0) {
		timer_on_ = false;
		event_ |= new_event_;
		new_event_ = 0;
	}
	if (!event_)
		return;
	ev = event_;
	event_ = 0;
	events(ev);
}

/* the ms until the timer's events or a voice dial's timer (0: due), -1
   without any */
s32 Modem::next_timer_ms()
{
	s32 left = -1;
	if (event_)
		return 0;
	if (timer_on_) {
		left = static_cast<s32>(timer_at_ - host_.now_ms());
		if (left < 0)
			left = 0;
	}
	if (voice_dialing_ && !voice_dial_digits_) {
		s32 vd = voice_dial_left_ms();
		if (left < 0 || vd < left)
			left = vd;
	}
	if (later_ == Later::Stop) {
		u64 idle = host_.now_ms() - samples_at_ms_;
		s32 st = idle >= STOP_IDLE_MS ? 0 :
			static_cast<s32>(STOP_IDLE_MS - idle);
		if (left < 0 || st < left)
			left = st;
	}
	return left;
}

/* the events due: the ring check (a burst ends: RING, the auto answer,
   or the ring gone), then the escape (online command state, OK) */
void Modem::events(u16 ev)
{
	if (ev & EV_RING_CHECK) {
		s32 len = static_cast<s32>(ring_last_ - ring_first_);
		if (!ring_count_) {
			sregs_[SREG_RING_COUNTER] = 0;
		} else if (len < static_cast<s32>(RING_MIN_MS) ||
			   ring_count_ < RING_PULSES) {
			logf("bad ring: %u pulses in %" PRId32 " ms\n",
			     ring_count_, len);
			sregs_[SREG_RING_COUNTER] = 0;
			ring_count_ = 0;
		} else {
			ring_count_ = 0;
			sregs_[SREG_RING_COUNTER]++;
			if (sregs_[SREG_RING_COUNTER] == 1)
				crlf();
			report(Result::Ring);
			if (sregs_[SREG_RINGS_TO_AUTO_ANSWER] && !started_ &&
			    sregs_[SREG_RING_COUNTER] >=
			    sregs_[SREG_RINGS_TO_AUTO_ANSWER]) {
				sregs_[SREG_RING_COUNTER] = 0;
				answer();
			} else {
				schedule_event(EV_RING_CHECK, RING_GONE_MS);
			}
		}
	}
	if (ev & EV_ESCAPE) {
		if (state_ == ONLINE && escape_count_ == 3) {
			set_state(COMMAND_ONLINE);
			xmit_head_ = xmit_tail_ = xmit_count_ = 0;
			crlf();
			report(Result::Ok);
			command_ = true;
		}
		escape_count_ = 0;
	}
}

/*
 *    rings and caller ID
 */

/* a pulse of a ring signal (idle only): a burst's first one, if it comes
   late enough after the last pulse (or, the signal on all along, late
   enough after the last burst's first, or the start), has the burst
   checked later (and starts caller ID before the first ring) */
void Modem::ring()
{
	u32 now = static_cast<u32>(host_.now_ms());

	if (state_ != IDLE)
		return;
	if (!ring_count_) {
		if (static_cast<s32>(now - ring_last_) <
			    static_cast<s32>(RING_GAP_MS) &&
		    static_cast<s32>(now - ring_first_) <
			    static_cast<s32>(RING_AGAIN_MS)) {
			ring_last_ = now;
			return;
		}
		ring_first_ = now;
		schedule_event(EV_RING_CHECK, RING_CHECK_MS);
		if (!sregs_[SREG_RING_COUNTER] && cid_requested_)
			cid_start(CID_MS);
	}
	ring_last_ = now;
	ring_count_++;
}

/* see dcore.h: a change only (the line may say it twice) */
void Modem::local_phone(bool off_hook)
{
	static const char ev[2][2] = { { 0x10, 'h' }, { 0x10, 'H' } };

	if (off_hook == local_phone_off_)
		return;
	local_phone_off_ = off_hook;
	logf("the local phone %s hook\n", off_hook ? "off" : "on");
	if (mode_ == MODEM_MODE_VOICE)
		host_.dte_write(*this, ev[off_hook], 2);
}

/* a call that rings by itself (SIP): RING each time, the auto answer
   (idle, line stopped); on false the rings are over */
void Modem::ring_signal(bool on)
{
	if (!on) {
		sregs_[SREG_RING_COUNTER] = 0;
		ring_count_ = 0;
		return;
	}
	if (state_ != IDLE || started_)
		return;
	sregs_[SREG_RING_COUNTER]++;
	if (sregs_[SREG_RING_COUNTER] == 1)
		crlf();
	report(Result::Ring);
	if (sregs_[SREG_RINGS_TO_AUTO_ANSWER] && sregs_[SREG_RING_COUNTER] >=
	    sregs_[SREG_RINGS_TO_AUTO_ANSWER]) {
		sregs_[SREG_RING_COUNTER] = 0;
		answer();
	}
}

/* caller ID for ms: its receiver on the line, the hook snooping; -1 if
   the line runs already or the receiver cannot be had */
s16 Modem::cid_start(u32 ms)
{
	if (started_)
		return -1;
	/* voice mode's report is a packet (V.253 9.2.3.1.3, example 3) */
	cid_ = CID_create(handle(), srate_, cid_requested_,
			  mode_ == MODEM_MODE_VOICE);
	if (!cid_)
		return -1;
	count_ = 0;
	later(Later::CallerIdStop,
	      static_cast<u32>(srate_) * ms / 1000);
	run_ = Run::CallerId;
	set_hook(MODEM_HOOK_SNOOPING);
	if (do_start()) {
		log_printf(1, "err: %s: the line does not start\n", name_);
		stop_call();
		return -1;
	}
	return 0;
}

/* the caller ID receiver on the samples (silence out) until it has
   reported (or failed) */
void Modem::cid_process(s16 *in, s16 *out, s32 n)
{
	int ret;
	std::fill_n(out, n, 0);
	ret = CID_process(cid_, in, n);
	if (ret < 0)
		logf("caller ID failed\n");
	if (ret)
		cid_stop();
}

/* the end of caller ID: the line stops, and the auto answer if the
   rings have come */
void Modem::cid_stop()
{
	later_ = Later::None;
	run_ = Run::None;
	stop_call();
	if (sregs_[SREG_RINGS_TO_AUTO_ANSWER] && sregs_[SREG_RING_COUNTER] >=
	    sregs_[SREG_RINGS_TO_AUTO_ANSWER]) {
		sregs_[SREG_RING_COUNTER] = 0;
		answer();
	}
}

/*
 *    voice
 */

/* +VLS: the voice engine on the line (1), on the handset alone (14,
   the line on hook) or on the call with the handset too (15), the last
   two where the Host has a handset (has_handset()): 1 started, its OK
   to come; 0 it was there (moved from one to another: the line taken or
   left, the engine going on), or off (0); -1 for another device or if
   it cannot.  The line is taken to answer (a ringing call's, on SIP) */
s16 Modem::voice_set_device(u16 device)
{
	bool handset = device == VOICE_DEVICE_HANDSET ||
		device == VOICE_DEVICE_HANDSET_LINE;

	if (handset && !host_.has_handset(*this))
		return -1;
	if (device == VOICE_DEVICE_NONE) {
		if (voice_)
			voice_stop();
		return 0;
	}
	if (device != VOICE_DEVICE_LINE && !handset)
		return -1;
	bool off = device != VOICE_DEVICE_HANDSET;
	if (voice_) {
		if (device == voice_device_)
			return 0;
		if (host_.voice_route(*this, device) ||
		    set_hook(off ? MODEM_HOOK_OFF : MODEM_HOOK_ON))
			return -1;
		logf("voice: +VLS=%u\n", device);
		voice_device_ = device;
		return 0;
	}
	caller_ = false;
	if (host_.voice_route(*this, device))
		return -1;
	voice_device_ = device;
	if (voice_start(off)) {
		voice_unroute();
		return -1;
	}
	return 1;
}

/* the voice engine's audio back on the line (the Host's voice_route()),
   if +VLS had it on the handset */
void Modem::voice_unroute()
{
	if (voice_device_ == VOICE_DEVICE_HANDSET ||
	    voice_device_ == VOICE_DEVICE_HANDSET_LINE)
		host_.voice_route(*this, VOICE_DEVICE_LINE);
	voice_device_ = VOICE_DEVICE_NONE;
}

/* the voice engine started, off hook on the line (else on hook: +VLS=14,
   the handset's alone): 0, or -1 */
s16 Modem::voice_start(bool off_hook)
{
	if (voice_)
		return 0;
	/* (caller ID on the line gives way, as to ATA) */
	if (started_ && (run_ != Run::CallerId || stop_call()))
		return -1;
	voice_ = VOICE_create(handle(), srate_);
	if (!voice_)
		return -1;
	if (voice_device_ == VOICE_DEVICE_NONE)
		voice_device_ = VOICE_DEVICE_LINE;
	later_ = Later::None;
	run_ = Run::Voice;
	voice_cng_seen_ = voice_ct_seen_ = 0;
	if (off_hook)
		set_hook(MODEM_HOOK_OFF);
	if (do_start()) {
		log_printf(1, "err: %s: the line does not start\n", name_);
		voice_stop();
		return -1;
	}
	return 0;
}

/* the voice engine goes, the line stops, on hook */
void Modem::voice_stop()
{
	run_ = Run::None;
	later_ = Later::None;
	VOICE_delete(voice_);
	voice_ = nullptr;
	vts_.clear();
	vts_next_ = 0;
	voice_dialing_ = false;
	voice_dial_digits_ = false;
	voice_cmd_ = -1;
	stop_call();
	set_hook(MODEM_HOOK_ON);
}

static voice_tones heard(void *voice);

/* the voice engine goes, the line held off hook as it runs (silence
   on it) for a fax or data handshake on the call */
void Modem::voice_hold()
{
	logf("voice: the call held for a handshake\n");
	run_ = Run::None;
	later_ = Later::None;
	VOICE_delete(voice_);
	voice_ = nullptr;
	voice_unroute();
	vts_.clear();
	vts_next_ = 0;
	voice_dial_digits_ = false;
	voice_cmd_ = -1;
	voice_held_ = started_;
}

/* +FAA=1 in voice mode, in the voice command state (a +VTX, +VRX,
   +VTR or +VTS under way first ends): a fax machine's CNG on the call
   makes it a fax call (FAX, class 1, the fax answerer's CED and DIS on
   the held line), a data modem's V.25 calling tone a data call (data
   mode, its adaptive answer: DATA, then the CONNECT) */
void Modem::voice_calling_tones()
{
	voice_tones t = heard(voice_);
	bool fax = t.cng != voice_cng_seen_, data = t.ct != voice_ct_seen_;

	if ((!fax && !data) || !kept("+FAA")[0] || voice_dialing_ ||
	    voice_cmd_ != -1 || voice_drain_ || !command_)
		return;
	voice_cng_seen_ = t.cng;
	voice_ct_seen_ = t.ct;
	logf("voice: %s calling tone, +FAA=1: a %s call\n",
	     fax ? "a fax machine's" : "a data modem's", fax ? "fax" : "data");
	set_mode(fax ? MODEM_MODE_FAX : MODEM_MODE_DATA);
	if (fax)
		report(Result::Fax);
	if (answer(!fax))
		report(Result::Error);
}

/* +VTS, +VTX, +VRX, +VTR: the voice engine's command, the DTE's
   characters its own until it reports; 1, or -1 without an engine or
   for another command */
s16 Modem::voice_command(enum VOICE_CMD cmd)
{
	switch (cmd) {
	case VOICE_CMD_BEEP:
	case VOICE_CMD_DTMF:
	case VOICE_CMD_STATE_RX:
	case VOICE_CMD_STATE_TX:
	case VOICE_CMD_STATE_DUPLEX:
		if (!voice_)
			return -1;
		command_ = false;
		voice_cmd_ = cmd;
		VOICE_command(voice_, cmd);
		return 1;
	default:
		return -1;
	}
}

/* the next element of the +VTS string to the voice engine: 1, or -1
   without an engine */
s16 Modem::voice_tone_next()
{
	const VoiceTone &t = vts_[vts_next_++];

	voice_info_.tone_duration = t.d;
	if (t.flash) {
		/* the line signals it; the modem's audio is silence */
		host_.hook_flash(*this, t.d * 10);
		voice_info_.tone1_freq = 0;
		voice_info_.tone2_freq = 0;
		return voice_command(VOICE_CMD_BEEP);
	}
	if (t.dtmf) {
		voice_info_.dtmf_symbol = static_cast<unsigned char>(t.dtmf);
		return voice_command(VOICE_CMD_DTMF);
	}
	voice_info_.tone1_freq = t.f1;
	voice_info_.tone2_freq = t.f2;
	return voice_command(VOICE_CMD_BEEP);
}

/* number as the dial string (ATDL's), as dial() keeps it */
void Modem::keep_dial_string(const char *number)
{
	copy_dial(dial_string_, sizeof(dial_string_), number);
}

/* the call progress tones the voice engine heard (none without one) */
static voice_tones heard(void *voice)
{
	voice_tones t = {};
	if (voice)
		VOICE_tones(voice, &t);
	return t;
}

/* the ms until a voice dial's next timer: on a line that does not tell
   of the answer, +VRN's while no ring was heard (when not beyond S7),
   +VRA's once one ended; S7's */
s32 Modem::voice_dial_left_ms()
{
	u32 s7 = sregs_[SREG_WAIT_CARRIER_AFTER_DIAL] * UINT32_C(1000);
	s32 left =
		static_cast<s32>(voice_dial_since_ + s7 - host_.now_ms());
	voice_tones t = heard(voice_);

	if (!host_.signals_answer(*this)) {
		s32 due = -1;
		if (!t.rings && voice_info_.ring_never * UINT32_C(1000) <= s7)
			due = static_cast<s32>(voice_dial_since_ +
				     voice_info_.ring_never * UINT32_C(1000) -
				     host_.now_ms());
		else if (t.rings && !t.ringing)
			due = static_cast<s32>(
				voice_info_.ring_after * 100L - t.quiet_ms);
		if (due >= 0 && due < left)
			left = due;
	}
	return left > 0 ? left : 0;
}

/* a voice dial's timers and tones (the core's poll(), and each block of
   the line's samples; on SIP its early media).  On any line (V.253
   9.1.1): BUSY on a busy tone's cadence, answered on an answer tone (the
   far end a modem or fax: "high confidence").  On a line that does not
   tell of the answer, also answered when +VRN has gone without a ring
   of a ringing tone (10.2.6), or when +VRA has gone since the last ring
   ended without another (10.2.5).  NO ANSWER after S7. */
void Modem::voice_dial_check()
{
	u32 gone = static_cast<u32>(host_.now_ms()) -
		voice_dial_since_;
	voice_tones t = heard(voice_);

	if (t.busy) {
		logf("voice dial: busy tone\n");
		voice_dial_end(Result::Busy);
		return;
	}
	if (t.ans2100 || t.ans2225) {
		logf("voice dial: answer tone (%s Hz)\n",
		     t.ans2100 ? "2100" : "2225");
		line_answered();
		return;
	}
	if (!host_.signals_answer(*this)) {
		if (!t.rings &&
		    gone >= voice_info_.ring_never * UINT32_C(1000) &&
		    voice_info_.ring_never <=
		    sregs_[SREG_WAIT_CARRIER_AFTER_DIAL]) {
			logf("voice dial: no ringing tone in +VRN\n");
			line_answered();
			return;
		}
		if (t.rings && !t.ringing &&
		    t.quiet_ms >=
			    static_cast<long>(voice_info_.ring_after) * 100) {
			logf("voice dial: ringing tone gone (%u rings)\n",
			     t.rings);
			line_answered();
			return;
		}
	}
	if (gone >= sregs_[SREG_WAIT_CARRIER_AFTER_DIAL] * UINT32_C(1000)) {
		logf("voice dial: no answer in S7\n");
		voice_dial_end(Result::NoAnswer);
	}
}

/* a voice dial over: on hook, result r; NO CARRIER, which V.253 does
   not allow in voice mode (Table 3), is ERROR ("the operation completed
   abnormally") */
void Modem::voice_dial_end(Result r)
{
	if (r == Result::NoCarrier)
		r = Result::Error;
	logf("voice dial: %s\n", r == Result::Busy ? "busy" :
	     r == Result::NoAnswer ? "no answer" : "failed");
	voice_dialing_ = false;
	voice_stop();
	report(r);
	command_ = true;
	commands_due_ = true;
}

/* the voice call's line ended it (the far end hung up): <DLE><l>, V.253's
   loop current interruption, which "usually indicates a remote hangup",
   on hook, and the end of the engine's command (V.253 has no NO CARRIER
   in voice mode): +VRX's samples end (<DLE><ETX>) and OK; +VTX's and
   +VTR's when the DTE ends them (voice_drain()); +VTS's tones ERROR;
   none in the command state */
void Modem::voice_line_gone()
{
	static const char lci[2] = { 0x10, 'l' };
	static const char etx[2] = { 0x10, 0x03 };
	s16 cmd = voice_cmd_;

	logf("voice: the line ended the call\n");
	host_.dte_write(*this, lci, 2);
	voice_stop();
	switch (cmd) {
	case VOICE_CMD_STATE_RX:
		host_.dte_write(*this, etx, 2);
		report(Result::Ok);
		break;
	case VOICE_CMD_STATE_TX:
	case VOICE_CMD_STATE_DUPLEX:
		voice_drain_ = true;
		voice_drain_dle_ = false;
		voice_drain_duplex_ = cmd == VOICE_CMD_STATE_DUPLEX;
		command_ = false;
		voice_drain();
		return;
	case VOICE_CMD_BEEP:
	case VOICE_CMD_DTMF:
		report(Result::Error);
		break;
	default:
		break;
	}
	command_ = true;
	commands_due_ = true;
}

/* the DTE's samples of a +VTX or +VTR whose line has gone, discarded up
   to their end (+VTX's <DLE><ETX>, <DLE>! or <DLE><CAN>, +VTR's <DLE><^>
   only, as the engine has them): then +VTR's <DLE><ETX>, OK, and the
   command state */
void Modem::voice_drain()
{
	static const char etx[2] = { 0x10, 0x03 };
	char c;

	while (voice_drain_ && get_chars(&c, 1) == 1) {
		if (!voice_drain_dle_) {
			voice_drain_dle_ = c == 0x10;
			continue;
		}
		voice_drain_dle_ = false;
		if (voice_drain_duplex_ ? c != '^' :
		    c != 0x03 && c != '!' && c != 0x18)
			continue;
		voice_drain_ = false;
		if (voice_drain_duplex_)
			host_.dte_write(*this, etx, 2);
		report(Result::Ok);
		command_ = true;
		commands_due_ = true;
	}
}

/* the voice engine on the samples, its results to the DTE: after OK or
   ERROR the DTE's characters are commands again (those already there
   run); a +VTS string's OK comes after its last element */
void Modem::voice_process(s16 *in, s16 *out, s32 n)
{
	int st = VOICE_process(voice_, in, out, n);

	if (st < 0) {
		logf("voice engine: %d, stopped\n", st);
		voice_stop();
		return;
	}
	if (voice_dialing_) {
		/* the engine's OKs (the line taken, a digit played) are not
		   the dial's, which comes with the answer; while it dials
		   the number, each plays the next digit, and its timers
		   start from the last */
		if (st == VOICE_STATUS_OK && voice_dial_digits_) {
			if (vts_next_ < vts_.size()) {
				voice_tone_next();
				return;
			}
			voice_dial_digits_ = false;
			voice_cmd_ = -1;
			vts_.clear();
			vts_next_ = 0;
			voice_dial_since_ =
				static_cast<u32>(host_.now_ms());
			logf("voice dial: dialled\n");
		}
		if (st == VOICE_STATUS_OK)
			st = 0;
		if (!voice_dial_digits_)
			voice_dial_check();
		if (!voice_)
			return;
	}
	if (st == VOICE_STATUS_OK && vts_next_ < vts_.size()) {
		voice_tone_next();
		return;
	}
	if (st == VOICE_STATUS_OK || st == VOICE_STATUS_ERROR) {
		voice_cmd_ = -1;
		vts_.clear();
		vts_next_ = 0;
	}
	switch (st) {
	case VOICE_STATUS_OK:
		report(Result::Ok);
		command_ = true;
		commands_due_ = true;
		break;
	case VOICE_STATUS_CONNECT:
		report(Result::Connect);
		command_ = false;
		break;
	case VOICE_STATUS_ERROR:
		report(Result::Error);
		command_ = true;
		commands_due_ = true;
		break;
	default:
		break;
	}
	if (voice_)
		voice_calling_tones();
}

/*
 *    class 1 fax
 */

/* the fax engine in the pump's place (on T.38 if that was asked for);
   the line silent if it cannot be had; 0, or -1 */
s16 Modem::fax_start()
{
	struct dp *old = dp_;

	fax_ = FAX_create(handle(), caller_, srate_);
	if (fax_ && t38_ &&
	    FAX_t38_start(fax_, fax_t38_send, this, t38_max_) < 0)
		t38_ = false;
	later_ = Later::None;
	run_ = fax_ ? Run::Fax : Run::None;
	dp_ = nullptr;
	if (old)
		old->op->destroy(old);
	if (!fax_) {
		log_printf(1, "err: %s: cannot create the fax engine\n", name_);
		return -1;
	}
	logf("fax engine, %s%s\n", caller_ ? "caller" : "answerer",
	     t38_ ? ", T.38" : "");
	return 0;
}

/* the fax engine on the samples, its results to the DTE: after OK, NO
   CARRIER or ERROR the DTE's characters are commands again (those
   already there run) */
void Modem::fax_process(s16 *in, s16 *out, s32 n)
{
	int st = FAX_process(fax_, in, out, n);

	if (st < 0) {
		logf("fax engine: %d, the call stops\n", st);
		stop_call();
		return;
	}
	switch (st) {
	case FAX_STATUS_OK:
		set_state(COMMAND_ONLINE);
		report(Result::Ok);
		command_ = true;
		commands_due_ = true;
		break;
	case FAX_STATUS_CONNECT:
		if (!stats_.connected) {
			stats_.connected = true;
			stats_.up = static_cast<u32>(host_.now_ms());
		}
		set_state(ONLINE);
		report(Result::Connect);
		command_ = false;
		break;
	case FAX_STATUS_NOCARRIER:
		set_state(COMMAND_ONLINE);
		report(Result::NoCarrier);
		command_ = true;
		commands_due_ = true;
		break;
	case FAX_STATUS_FRAME_ERROR:
		set_state(COMMAND_ONLINE);
		report(Result::Error);
		command_ = true;
		commands_due_ = true;
		break;
	case FAX_STATUS_ERROR:
		update_status(ST_ERROR);
		break;
	default:
		break;
	}
}

/* the fax engine's T.38 packet (ctx: the modem) to the Host */
void Modem::fax_t38_send(void *ctx, const void *ifp)
{
	Modem *m = static_cast<Modem *>(ctx);
	m->host_.t38_send(*m, ifp);
}

/* the call's fax goes over T.38, now or as its engine starts; 0, or -1
   outside fax mode or if the engine cannot */
s16 Modem::t38_start(s32 max_data)
{
	/* adaptive answer: a far end that goes over to T.38 is a fax */
	if (mode_ != MODEM_MODE_FAX && adaptive_answering())
		fax_found("T.38");
	if (mode_ != MODEM_MODE_FAX)
		return -1;
	t38_max_ = max_data;
	if (fax_ && FAX_t38_start(fax_, fax_t38_send, this, max_data) < 0)
		return -1;
	logf("T.38 on, %d octets of data a packet\n", max_data);
	t38_ = true;
	/* a call we dialled, its call progress still waiting for the
	   answer tone: the far end, going over to T.38, has answered (it
	   may well do so before its CED, which T.30 has wait 1.8 s and
	   which T.38 then carries as an indicator, not as audio): the fax
	   engine now, on T.38, as the answer tone would have started it */
	if (!fax_ && caller_ && run_ == Run::Dp && started_) {
		logf("T.38 before the answer tone: the call answered\n");
		change_dp();
	}
	return 0;
}

/* back to the audio */
void Modem::t38_stop()
{
	if (!t38_)
		return;
	logf("T.38 off\n");
	t38_ = false;
	if (fax_)
		FAX_t38_stop(fax_);
}

/* a T.38 packet from the remote, for the fax engine */
void Modem::t38_rx(const void *ifp)
{
	if (t38_ && fax_)
		FAX_t38_rx(fax_, ifp);
}

} // namespace dcore
