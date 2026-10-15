/*
 *
 *    modem.cpp - the modem core (dcore.h): the call's state machine, the
 *                data pumps' life cycle, rings, caller ID, voice and fax,
 *                the DTE's characters and the timers
 *
 *    Smart Link's slmodemd modem.c as a class, with the same states and
 *    the same order of things.  The timers are deadlines on the Host's
 *    clock (no signals, no global timer list).
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dsplibs.h>
#include <link.h>

#include "log.h"
#include "dcore.h"

/* not in the original dsplibs.o */
extern "C" int dp_runtime_pcm(void *runtime) __attribute__((weak));


namespace dcore {

/* who the modem says it is (ATI, +GMI, +GMM, +GMR, +FMI?..., slmodemd's
   banner); the version is the build's (CMakeLists.txt's project) */
#ifndef DMODEM_VERSION
#error "DMODEM_VERSION: the build gives it (CMakeLists.txt)"
#endif
const char *const Modem::author = "D-Modem project";
const char *const Modem::product = "D-Modem";
const char *const Modem::version = DMODEM_VERSION;

/* debug lines (with the modem's name), error lines */
#define DBG(fmt, ...) logf(fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) log_printf(1, "err: %s: " fmt, name_, ##__VA_ARGS__)

/* the timing of the old core, in its ticks of 10 ms */
static const unsigned long RING_ON_MIN = 150;   /* ms */
static const unsigned long RING_ON_MAX = 2500;
static const unsigned long RING_OFF_MIN = 1500;
static const unsigned long RING_OFF_MAX = 6500;
static const unsigned long TICK = 10;
static const unsigned RING_COUNT_MIN = 10;
static const unsigned long ESCAPE_TIMEOUT = 500;
static const int XMIT_SIZE = 4096;

/* t1 is before t2 (ms, the clock may wrap) */
static bool time_before(unsigned long t1, unsigned long t2)
{
	return (long)(t2 - t1) > 0;
}

/* t1 is t2 or after */
static bool time_after(unsigned long t1, unsigned long t2)
{
	return !time_before(t1, t2);
}

/* S-registers by their use; CRLF: S3 and S4 */
#define TOTAL_RINGS        sregs_[SREG_RING_COUNTER]
#define ANSWER_AFTER_RINGS sregs_[SREG_RINGS_TO_AUTO_ANSWER]
#define CRLF               ((const char *)sregs_ + SREG_CR_CHAR)

/* the modulations without V.42 */
static bool nonec_dp(int id)
{
	return id == DP_V23 || id == DP_B103 || id == DP_V21;
}

/* V.8 starts them */
static bool fast_dp(int id)
{
	return id == DP_V34 || id == DP_V90 || id == DP_V91 || id == DP_V92;
}

/*
 *    the link layer's owner
 */

struct LinkOwner final : public dlink::Owner {
	Modem *m;
	/* the DTE's characters for the line */
	int read_chars(uint8_t *buf, int n) override {
		return m->get_chars((char *)buf, n);
	}
	/* received characters to the DTE */
	int write_chars(const uint8_t *buf, int n) override {
		return m->put_chars((const char *)buf, n);
	}
	/* the link's progress as the core's statuses */
	void status(dlink::Status s) override {
		switch (s) {
		case dlink::Status::Detected:
			m->update_status(Modem::ST_PACK_LINK);
			break;
		case dlink::Status::Up:
			m->update_status(Modem::ST_EC_LINK);
			break;
		case dlink::Status::Released:
			m->update_status(Modem::ST_EC_RELEASE);
			break;
		case dlink::Status::Error:
			m->update_status(Modem::ST_EC_ERROR);
			break;
		}
	}
	/* the pump's rate, for the link's bit timers */
	int tx_rate() override { return (int)m->tx_rate(); }
	/* what XID agreed, for the CONNECT messages */
	void negotiated(const dlink::Config &c) override {
		m->negotiated(c.comp, c.v44, c.dict_size, c.max_string);
	}
	/* the link's lines in the modem's log */
	void log(const char *line) override {
		m->logf("%s\n", line);
	}
};

/*
 *    creation
 */

/* a modem on host: the S-registers, the country (opt.country, else
   USA), the link layer, the DTE's buffer; ok() if all went well */
Modem::Modem(Host &host, const Options &opt)
	: host_(host), name_(opt.name ? opt.name : "modem"),
	  dev_name_(opt.dev_name)
{
	handle_ = new struct modem;
	handle_->self = this;
	srate_ = opt.rate;
	frag_ = srate_ / 200;
	min_rate_ = MODEM_MIN_RATE;
	max_rate_ = MODEM_MAX_RATE;
	sregs_init();
	kept_init();
	if (homolog_init(MODEM_DEFAULT_COUNTRY_CODE, nullptr)) {
		ERR("bad default country code `%x'!\n",
		    MODEM_DEFAULT_COUNTRY_CODE);
		return;
	}
	if (opt.country && homolog_init(-1, opt.country))
		log_printf(0, "bad country name `%s', using default by "
			   "code!\n", opt.country);
	ring_last_ = host_.now_ms();
	setup_config();
	DBG("startup modem...\n");
	link_ = new dlink::Link();
	link_owner_ = new LinkOwner();
	link_owner_->m = this;
	xmit_ = new char[XMIT_SIZE];
	xmit_size_ = XMIT_SIZE;
	voice_init();
	ok_ = true;
}

/* a call still going ends (NO CARRIER); everything freed */
Modem::~Modem()
{
	DBG("modem_delete...\n");
	if (started_) {
		DBG("shutdown modem...\n");
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
	delete handle_;
}

/* the Modem behind the pumps' struct modem */
Modem *Modem::of(struct modem *m)
{
	return m->self;
}

/* a debug line (level 1) with the modem's name, printf style */
void Modem::logf(const char *fmt, ...)
{
	char line[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	log_printf(1, "%s: %s", name_, line);
}

/* the name of pump id, NULL if there is none */
const char *Modem::dp_name(int id) const
{
	for (DpDriver *p = dp_drivers(); p->id > 0; p++)
		if (p->id == id)
			return p->name;
	return nullptr;
}

/*
 *    the result codes
 */

/* a result code to the DTE (not with ATQ1): its text (ATV1; CONNECT
   with the AT&A lines and the rate) or its number */
void Modem::report(Result code)
{
	static const struct {
		const char *code, *text;
	} responses[] = {
		{ "0", "OK" }, { "1", "CONNECT" }, { "2", "RING" },
		{ "3", "NO CARRIER" }, { "4", "ERROR" }, { "", "" },
		{ "6", "NO DIALTONE" }, { "7", "BUSY" }, { "8", "NO ANSWER" },
	};
	static const char none[] = "NONE";
	int id = (int)code;
	const char *msg;

	DBG("modem report result: %d (%s)\n", id, responses[id].text);
	if (sregs_[SREG_QUIET])
		return;
	if (sregs_[SREG_VERBOSE]) {
		if (code == Result::Connect) {
			int mask = sregs_[SREG_CONNNECT_MSG_FORMAT];
			if (mask & 1) {
				put_chars("Modulation: ", 12);
				msg = dp_ ? dp_name(dp_->id) : nullptr;
				puts(msg ? msg : none);
				crlf();
			}
			if (mask & 2) {
				put_chars("Protocol: ", 10);
				puts(cfg_.ec ? "LAPM" : none);
				crlf();
			}
			if ((mask & 3) == 3) {
				put_chars("Compression: ", 13);
				puts(!(cfg_.ec && cfg_.comp) ? none :
				     cfg_.v44 ? "V44" : "V42bis");
				crlf();
			}
			if (mask & 4 && tx_rate_) {
				char s[16];
				put_chars("TxRate: ", 8);
				snprintf(s, sizeof(s), "%u", tx_rate_);
				puts(s);
				crlf();
			}
		}
		puts(responses[id].text);
		if (code == Result::Connect && mode_ == MODEM_MODE_DATA &&
		    rx_rate_ && sregs_[SREG_X_CODE] != 0) {
			char s[16];
			snprintf(s, sizeof(s), " %u", rx_rate_);
			puts(s);
		}
	} else
		puts(responses[id].code);
	crlf();
}

/* a string to the DTE */
void Modem::puts(const char *s)
{
	put_chars(s, strlen(s));
}

/* the line's end (S3, S4) to the DTE */
void Modem::crlf()
{
	put_chars(CRLF, 2);
}

/*
 *    the state machine
 */

/* the call's state, and what the link does in it: characters flow
   only ONLINE, the link stops with the pump (IDLE, DP_ESTAB, DP_DISC) */
int Modem::set_state(State s)
{
	DBG("modem set state: %x --> %x...\n", state_, (unsigned)s);
	if (state_ == (unsigned)s)
		return 0;
	switch (s) {
	case IDLE:
		DBG("new state: MODEM_IDLE\n");
		command_ = true;
		link_->set_online(false);
		link_->stop();
		break;
	case DP_ESTAB:
		DBG("new state: DP_ESTAB\n");
		command_ = false;
		link_->set_online(false);
		link_->stop();
		break;
	case EC_ESTAB:
		DBG("new state: EC_ESTAB\n");
		command_ = false;
		link_->set_online(false);
		break;
	case ONLINE:
		DBG("new state: MODEM_ONLINE\n");
		command_ = false;
		link_->set_online(true);
		carrier_ = true;
		break;
	case COMMAND_ONLINE:
		DBG("new state: COMMAND_ONLINE\n");
		command_ = true;
		link_->set_online(false);
		break;
	case EC_DISC:
		DBG("new state: EC_DISC\n");
		link_->set_online(false);
		break;
	case DP_DISC:
		DBG("new state: DP_DISC\n");
		link_->set_online(false);
		link_->stop();
		break;
	}
	state_ = s;
	return 0;
}

/* runs what after samples more line samples (one at a time) */
void Modem::later(Later what, unsigned long samples)
{
	later_ = what;
	later_at_ = count_ + samples;
}

/* hangs up, local (we do: LAPM's DISC first, else the pump hangs up)
   or not (the remote did); the call stops 48 samples later and its
   result (NO CARRIER unless set) goes to the DTE */
void Modem::hup(bool local)
{
	DBG("modem_hup...%d\n", state_);
	switch (state_) {
	case IDLE:
		return;
	case DP_ESTAB:
		if (local && dp_ && dp_->op->hangup)
			dp_->op->hangup(dp_);
		set_state(DP_DISC);
		if (!have_result_) {
			result_ = Result::NoCarrier;
			have_result_ = true;
		}
		later(Later::Stop, 48);
		break;
	case EC_ESTAB:
	case ONLINE:
	case COMMAND_ONLINE:
		if (local && cfg_.ec) {
			link_->disconnect();
			set_state(EC_DISC);
		} else {
			if (local && dp_ && dp_->op->hangup)
				dp_->op->hangup(dp_);
			set_state(DP_DISC);
		}
		if (!have_result_) {
			result_ = Result::NoCarrier;
			have_result_ = true;
		}
		later(Later::Stop, 48);
		carrier_ = false;
		break;
	case EC_DISC:
		if (local && dp_ && dp_->op->hangup)
			dp_->op->hangup(dp_);
		set_state(DP_DISC);
		break;
	case DP_DISC:
	default:
		return;
	}
}

/* the progress of the pump (ST_DP_LINK: the link starts) and of the
   link (ST_EC_LINK: ONLINE and CONNECT; released, errors: hang up), or
   the end of the call with its result */
void Modem::update_status(Status status)
{
	DBG("modem_update_status: %d\n", (int)status);
	switch (status) {
	case ST_OK:
		break;
	case ST_DP_LINK:
		DBG("--> DP LINK\n");
		if (nonec_dp(dp_->id))
			cfg_.ec = false;
		set_state(EC_ESTAB);
		/* the V.42 detection phase, then LAPM or async */
		link_->start(cfg_.ec, cfg_.ec_detector &&
			     !(caller_ && dsp_info_.qc_lapm));
		break;
	case ST_PACK_LINK:
		/* LAPM or not: the link goes on by itself */
		cfg_.ec = link_->ec();
		DBG("--> PACK LINK\n");
		break;
	case ST_EC_LINK:
		DBG("--> EC LINK\n");
		/* the remote set the link up again (its SABME once more):
		   the call goes on, CONNECT once */
		if (state_ == ONLINE || state_ == COMMAND_ONLINE)
			break;
		set_state(ONLINE);
		report(Result::Connect);
		if (sregs_[SREG_SPEAKER_CONTROL] & 1)
			host_.speaker(*this, 0);
		break;
	case ST_EC_RELEASE:
	case ST_EC_ERROR:
		DBG("--> EC UNLINK\n");
		result_ = Result::NoCarrier;
		have_result_ = true;
		hup(state_ == EC_DISC);
		break;
	default:
		DBG("--> FINISH.\n");
		if (status == ST_NODIALTONE)
			result_ = Result::NoDialtone;
		else if (status == ST_BUSY)
			result_ = Result::Busy;
		else if (status == ST_NOANSWER)
			result_ = Result::NoAnswer;
		else
			result_ = Result::NoCarrier;
		have_result_ = true;
		hup(state_ == EC_DISC || state_ == DP_DISC);
		break;
	}
}

/* what XID agreed, for the CONNECT messages */
void Modem::negotiated(int comp, bool v44, int dict_size, int max_string)
{
	cfg_.comp = comp;
	cfg_.v44 = v44;
	cfg_.dict_size = dict_size;
	cfg_.max_string = max_string;
}

/* the call's state, for programs that embed the modem */
LinkState Modem::link_state() const
{
	if (state_ == IDLE)
		return LinkState::Idle;
	if (state_ == ONLINE || state_ == COMMAND_ONLINE)
		return LinkState::Online;
	if (state_ > IDLE && state_ < ONLINE)
		return LinkState::Connecting;
	return LinkState::HangingUp;
}

/* DTE characters in the buffer or in LAPM frames not acknowledged */
bool Modem::tx_pending() const
{
	return xmit_count_ > 0 || link_->tx_pending();
}

/* LAPM frames not sent or not acknowledged */
bool Modem::link_tx_pending() const
{
	return link_->tx_pending();
}

/* the network's G.711 law: to this call's pumps now, to the next ones'
   as their runtime is made */
void Modem::set_line_law(int law)
{
	if (law == line_law_)
		return;
	line_law_ = law;
	if (dp_runtime_ && law)
		dp_runtime_set_law(dp_runtime_, law);
}

/*
 *    the data pumps
 */

/* the pump's transmitter: bits from the link (logged); returns n */
int Modem::get_bits(int nbits, uint8_t *buf, int n)
{
	n = link_->get_bits(nbits, buf, n);
	log_data(this, LOG_TX_BITS, buf, n);
	return n;
}

/* the pump's receiver: bits to the link (logged); returns n */
int Modem::put_bits(int nbits, const uint8_t *buf, int n)
{
	n = link_->put_bits(nbits, buf, n);
	log_data(this, LOG_RX_BITS, buf, n);
	return n;
}

/* a pump's DPSTAT_ as the core's status */
static Modem::Status dpstat2status(int dpstat)
{
	switch (dpstat) {
	case DPSTAT_OK:         return Modem::ST_OK;
	case DPSTAT_CONNECT:    return Modem::ST_DP_LINK;
	case DPSTAT_ERROR:      return Modem::ST_DP_ERROR;
	case DPSTAT_NODIALTONE: return Modem::ST_NODIALTONE;
	case DPSTAT_BUSY:       return Modem::ST_BUSY;
	case DPSTAT_NOANSWER:   return Modem::ST_NOANSWER;
	case DPSTAT_CHANGEDP:   return Modem::ST_OK;
	default:                return Modem::ST_ERROR;
	}
}

/* count samples through the pump, frag_ at a time, acting on what it
   reports (connect, a change of pump, errors); silence once it is
   gone */
void Modem::dp_process(int16_t *in, int16_t *out, int count)
{
	while (count && dp_) {
		int cnt = count > frag_ ? frag_ : count;
		int ret = dp_->op->process(dp_, in, out, cnt);
		switch (ret) {
		case DPSTAT_OK:
			break;
		case DPSTAT_CONNECT:
			if (state_ <= DP_ESTAB && state_ != EC_DISC &&
			    state_ != DP_DISC)
				update_status(ST_DP_LINK);
			break;
		case DPSTAT_CHANGEDP:
			change_dp();
			break;
		default:        /* errors */
			update_status(dpstat2status(ret));
			run_ = Run::None;
			break;
		}
		count -= cnt;
		in += cnt;
		out += cnt;
	}
	if (count)
		memset(out, 0, count * sizeof(*out));
}

/* the next pump: the one a pump asked for (dp_requested_), else
   AT+MS's (V.8 first for V.34 and up; V.90 for K56flex), the fax
   engine in fax mode; the old pump destroyed; hangs up if it cannot
   be created */
void Modem::change_dp()
{
	struct dp *old = dp_;

	if (mode_ == MODEM_MODE_FAX) {
		fax_start();
		dp_ = nullptr;
	} else {
		struct dp_operations *op = nullptr;
		struct dp *dp = nullptr;
		int dp_id = dp_requested_, id;
		dp_requested_ = 0;
		id = dp_id;
		if (dp_id == 0) {
			dp_id = sregs_[SREG_DP];
			/* K56flex is not there (Rockwell's pre-V.90 PCM
			   modem): AT+MS=56 runs V.90, which K56flex modems
			   mostly got as an upgrade, else V.34 */
			if (dp_id == DP_K56)
				dp_id = DP_V90;
			id = fast_dp(dp_id) ? DP_V8 : dp_id;
		}
		DBG("%lu: change dp: --> %d...\n", count_, id);
		for (DpDriver *p = dp_drivers(); p->id > 0; p++)
			if (p->id == id) {
				op = p->op;
				break;
			}
		if (op && op->create)
			dp = op->create(handle_, (enum DP_ID)dp_id, caller_,
					srate_, frag_, op);
		if (!dp) {
			ERR("change dp -> %d error.\n", id);
			hup(true);
			return;
		}
		dp_ = dp;
		run_ = Run::Dp;
	}
	if (old)
		old->op->destroy(old);
}

/*
 *    the line's samples
 */

/* n samples through what runs now (silence if nothing) */
void Modem::run(int16_t *in, int16_t *out, int n)
{
	switch (run_) {
	case Run::Dp:
		dp_process(in, out, n);
		break;
	case Run::CallerId:
		cid_process(in, out, n);
		break;
	case Run::RingDetector:
		rd_process(in, out, n);
		break;
	case Run::Voice:
		voice_process(in, out, n);
		break;
	case Run::Fax:
		fax_process(in, out, n);
		break;
	case Run::None:
		memset(out, 0, n * sizeof(*out));
		break;
	}
}

/* the line's samples: DC removed (not from PCM codes), logged, through
   what runs; then what was due after them (later()) */
void Modem::process(const int16_t *in_, int16_t *out, int n)
{
	if (n > scratch_len_) {
		delete[] scratch_;
		scratch_ = new int16_t[n];
		scratch_len_ = n;
	}
	int16_t *in = scratch_;
	memcpy(in, in_, n * sizeof(*in));
	/* clean DC (not from PCM codes) */
	if (dcr_ && !(dp_runtime_pcm && dp_runtime_pcm(dp_runtime_)))
		dcr_process(dcr_, in, n);
	log_data(this, LOG_RX_SAMPLES, in, n * 2);
	run(in, out, n);
	log_data(this, LOG_TX_SAMPLES, out, n * 2);
	count_ += n;
	if (later_ != Later::None && count_ >= later_at_) {
		Later what = later_;
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
	/* what the DTE sent after a fax or voice command's data (they stop
	   at its <DLE><ETX>) is commands: at once, not only when more
	   characters come */
	if (commands_due_) {
		commands_due_ = false;
		if (command_ && xmit_count_)
			at_process();
	}
}

/*
 *    events and timers
 */

/* events mask after ms (one timer: the last one asked wins) */
void Modem::schedule_event(unsigned mask, unsigned long ms)
{
	new_event_ |= mask;
	timer_at_ = host_.now_ms() + ms;
	timer_on_ = true;
}

/* ms until the next event: 0 if one is due, -1 if none */
long Modem::next_timer_ms()
{
	if (event_)
		return 0;
	if (!timer_on_)
		return -1;
	long t = (long)(timer_at_ - host_.now_ms());
	return t > 0 ? t : 0;
}

/* the timer's events once it is due, and any others pending */
void Modem::poll()
{
	if (timer_on_ && (long)(timer_at_ - host_.now_ms()) <= 0) {
		timer_on_ = false;
		event_ |= new_event_;
		new_event_ = 0;
	}
	if (event_) {
		unsigned ev = event_;
		event_ = 0;
		events(ev);
	}
}

/* EV_RING_CHECK: a ring burst checked by its length and its pulses
   (RING, the answer after S0 rings); EV_ESCAPE: +++ and the guard
   time after it, online command mode and OK */
void Modem::events(unsigned event)
{
	DBG("modem_event: %x...\n", event);
	if (event & EV_RING_CHECK) {
		if (ring_count_ == 0) {
			DBG("ring cancel...\n");
			TOTAL_RINGS = 0;
		} else if (time_before(ring_last_, ring_first_ + RING_ON_MIN) ||
			   time_after(ring_last_, ring_first_ + RING_ON_MAX) ||
			   ring_count_ < RING_COUNT_MIN) {
			DBG("ring reject: count %d (%lu-%lu)\n", ring_count_,
			    ring_first_, ring_last_);
			TOTAL_RINGS = 0;
			ring_count_ = 0;
		} else {
			DBG("ring valid.\n");
			ring_count_ = 0;
			TOTAL_RINGS++;
			if (TOTAL_RINGS == 1)
				crlf();
			report(Result::Ring);
			if (ANSWER_AFTER_RINGS && (!started_ || rd_) &&
			    TOTAL_RINGS >= ANSWER_AFTER_RINGS) {
				TOTAL_RINGS = 0;
				answer();
			} else
				schedule_event(EV_RING_CHECK,
					       RING_OFF_MAX + TICK);
		}
	}
	if (event & EV_ESCAPE) {
		DBG("modem_escape (count %d)...\n", escape_count_);
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

/* a pulse of ring signal (only while idle): the pulses of one burst
   counted, the first starts the check (and caller ID) */
void Modem::ring()
{
	unsigned long now = host_.now_ms();
	if (state_ != IDLE)
		return;
	ring_count_++;
	if (ring_count_ == 1) {
		DBG("modem_ring...\n");
		if (time_before(now, ring_last_ + RING_OFF_MIN)) {
			DBG("bad ring: now %lu, last %lu.\n", now, ring_last_);
			ring_count_ = 0;
		} else {
			ring_first_ = now;
			schedule_event(EV_RING_CHECK, RING_ON_MAX + TICK);
			if (TOTAL_RINGS == 0 && cid_requested_)
				cid_start(RING_ON_MAX + RING_OFF_MIN);
		}
	}
	ring_last_ = now;
}

/* a call that rings by itself (SIP): RING, the answer after S0 rings;
   on = false: it gave up */
void Modem::ring_signal(bool on)
{
	if (!on) {
		DBG("ring cancel...\n");
		TOTAL_RINGS = 0;
		ring_count_ = 0;
		return;
	}
	if (state_ != IDLE || started_)
		return;
	TOTAL_RINGS++;
	if (TOTAL_RINGS == 1)
		crlf();
	report(Result::Ring);
	if (ANSWER_AFTER_RINGS && TOTAL_RINGS >= ANSWER_AFTER_RINGS) {
		TOTAL_RINGS = 0;
		answer();
	}
}

/* the device reported an error: logged only */
void Modem::error()
{
	DBG("modem error...\n");
}

/*
 *    the DTE
 */

/* up to n of the DTE's characters waiting in the buffer; returns how
   many */
int Modem::get_chars(char *buf, int n)
{
	int ret = 0;
	while (n) {
		int cnt = n;
		if (cnt > xmit_count_)
			cnt = xmit_count_;
		if (cnt > xmit_size_ - xmit_tail_)
			cnt = xmit_size_ - xmit_tail_;
		if (cnt <= 0)
			break;
		memcpy(buf, xmit_ + xmit_tail_, cnt);
		ret += cnt;
		n -= cnt;
		buf += cnt;
		xmit_count_ -= cnt;
		xmit_tail_ = (xmit_tail_ + cnt) % xmit_size_;
	}
	return ret;
}

/* characters to the DTE; returns how many it took */
int Modem::put_chars(const char *buf, int n)
{
	int ret = host_.dte_write(*this, buf, n);
	return ret < 0 ? 0 : ret;
}

/* the escape: three escape characters (S2) alone, the guard time
   before them (the timer checks the one after); returns how many came
   so far */
int Modem::check_escape(const char *buf, int count)
{
	unsigned long now = host_.now_ms();
	int i;
	if (count + escape_count_ > 3)
		goto noescape;
	for (i = 0; i < count; i++)
		if (buf[i] != (char)sregs_[SREG_ESCAPE_CHAR])
			goto noescape;
	if (escape_count_ == 0) {
		if (time_before(now, last_esc_check_ + ESCAPE_TIMEOUT))
			goto noescape;
	} else if (time_after(now, last_esc_check_ + ESCAPE_TIMEOUT))
		goto noescape;
	escape_count_ += count;
	if (escape_count_ == 3)
		schedule_event(EV_ESCAPE, ESCAPE_TIMEOUT);
	last_esc_check_ = now;
	return escape_count_;
noescape:
	escape_count_ = 0;
	last_esc_check_ = now;
	return 0;
}

/* the DTE's characters: any while connecting hangs up; online, the
   escape check; into the buffer, and to the AT interpreter in command
   mode; returns how many the buffer took */
int Modem::dte_input(const char *buf, int count)
{
	int ret = 0;
	if (state_ > IDLE && state_ < ONLINE) {
		DBG("modem_tty_write: hangup...\n");
		hup(true);
		return count;
	}
	if (state_ == ONLINE)
		check_escape(buf, count);
	while (count) {
		int cnt = count;
		if (cnt > xmit_size_ - xmit_count_)
			cnt = xmit_size_ - xmit_count_;
		if (cnt > xmit_size_ - xmit_head_)
			cnt = xmit_size_ - xmit_head_;
		if (cnt <= 0) {
			DBG("modem_write: overflow!\n");
			break;
		}
		memcpy(xmit_ + xmit_head_, buf, cnt);
		xmit_count_ += cnt;
		xmit_head_ = (xmit_head_ + cnt) % xmit_size_;
		ret += cnt;
		buf += cnt;
		count -= cnt;
	}
	if (command_)
		at_process();
	return ret;
}

/*
 *    starting and stopping a call
 */

/* the hook through the host; 0 on success */
int Modem::set_hook(int state)
{
	int ret;
	DBG("modem set hook: %d --> %d...\n", hook_, state);
	if (hook_ == state)
		return 0;
	ret = host_.set_hook(*this, state);
	if (!ret)
		hook_ = state;
	return ret;
}

/* the link's configuration from the S-registers (AT\N, AT%C) */
void Modem::setup_config()
{
	cfg_.ec = sregs_[SREG_EC];
	cfg_.ec_detector = sregs_[SREG_EC];
	cfg_.tx_win = dlink::LAPM_MAX_WIN;
	cfg_.rx_win = dlink::LAPM_MAX_WIN;
	cfg_.tx_info = dlink::LAPM_MAX_INFO;
	cfg_.rx_info = dlink::LAPM_MAX_INFO;
	if (cfg_.ec && sregs_[SREG_COMP] && sregs_[SREG_EC])
		cfg_.comp = 0x3;        /* V.42bis both ways */
	else
		cfg_.comp = 0;
	cfg_.dict_size = dlink::V42BIS_MAX_CODEWORDS;
	cfg_.max_string = dlink::V42BIS_MAX_STRING;
	cfg_.v44 = false;
	dsp_info_.qc_lapm = cfg_.ec && cfg_.ec_detector;
}

/* the host starts the line's audio; 0 on success */
int Modem::do_start()
{
	int ret;
	count_ = 0;
	ret = host_.start(*this);
	started_ = !ret;
	return ret;
}

/* a call (dial or answer): off hook, the link set up, the audio
   started; on failure NO CARRIER and the call stopped */
int Modem::start_call()
{
	int ret;
	DBG("modem_start..\n");
	if (started_ && stop_call())
		return -1;
	have_result_ = false;
	setup_config();
	set_state(DP_ESTAB);
	if (sregs_[SREG_SPEAKER_CONTROL] & 1)      /* ATM1, ATM3 */
		host_.speaker(*this, sregs_[SREG_SPEAKER_VOLUME]);
	ret = set_hook(MODEM_HOOK_OFF);
	if (ret)
		goto error;
	xmit_head_ = xmit_tail_ = xmit_count_ = 0;
	command_ = false;
	if (!(dcr_ = dcr_create()) ||
	    !(dp_runtime_ = dp_runtime_create(handle_))) {
		ret = -1;
		goto error;
	}
	if (line_law_)
		dp_runtime_set_law(dp_runtime_, line_law_);
	{
		dlink::Config c;
		c.ec = cfg_.ec;
		c.detect = cfg_.ec_detector;
		c.tx_win = cfg_.tx_win;
		c.rx_win = cfg_.rx_win;
		c.tx_info = cfg_.tx_info;
		c.rx_info = cfg_.rx_info;
		c.comp = cfg_.comp;
		c.dict_size = cfg_.dict_size;
		c.max_string = cfg_.max_string;
		if ((ret = link_->init(link_owner_, caller_, c)))
			goto error;
	}
	/* clear rings and all events */
	TOTAL_RINGS = 0;
	ring_count_ = 0;
	event_ = new_event_ = 0;
	timer_on_ = false;
	ret = do_start();
	if (!ret)
		return 0;
error:
	ERR("modem start = %d: cannot start device.\n", ret);
	result_ = Result::NoCarrier;
	have_result_ = true;
	stop_call();
	return ret;
}

/* the end of a call: the audio stopped, on hook, all freed, IDLE, the
   result to the DTE; returns started_ (true if the host could not
   stop) */
int Modem::stop_call()
{
	DBG("modem_stop..\n");
	run_ = Run::None;
	/* nothing due for this call may reach the next one (a caller ID
	   stop armed between rings would end a dial 4 s in) */
	later_ = Later::None;
	if (started_) {
		int ret = host_.stop(*this);
		started_ = ret != 0;
		if (ret)
			ERR("modem stop = %d: cannot stop device.\n", ret);
	}
	set_hook(MODEM_HOOK_ON);
	if (sregs_[SREG_SPEAKER_CONTROL] & 1)
		host_.speaker(*this, 0);
	caller_ = false;
	/* the call ended online: what the DTE sent is data that did not go
	   out, not commands (in online command state it is commands) */
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
	if (rd_) {
		RD_delete(rd_);
		rd_ = nullptr;
	}
	if (cid_) {
		CID_delete(cid_);
		cid_ = nullptr;
	}
	if (voice_) {
		VOICE_delete(voice_);
		voice_ = nullptr;
	}
	if (fax_) {
		FAX_delete(fax_);
		fax_ = nullptr;
	}
	set_state(IDLE);
	if (have_result_) {
		have_result_ = false;
		report(result_);
	}
	count_ = 0;
	return started_;
}

/* ATA: a call whose answering pump starts after S-register
   SREG_ANS_DELAY (s; 0: 10 ms); -1 if a pump runs */
int Modem::answer()
{
	DBG("modem answer...\n");
	if (dp_) {
		ERR("dp %d is already exists.\n", dp_->id);
		return -1;
	}
	if (started_)
		stop_call();
	dp_requested_ = 0;
	later(Later::ChangeDp, sregs_[SREG_ANS_DELAY] ?
	      sregs_[SREG_ANS_DELAY] * (unsigned long)srate_ : srate_ / 100);
	return start_call();
}

/* the call progress pump for a dial (created with the dial string
   hidden); -1 if a pump runs or it cannot be created */
int Modem::dial_start()
{
	struct dp_operations *op = nullptr;
	struct dp *dp = nullptr;
	char save;
	DBG("modem_dial_start...\n");
	if (dp_)
		return -1;
	if (started_)
		stop_call();
	caller_ = true;
	for (DpDriver *p = dp_drivers(); p->id > 0; p++)
		if (p->id == DP_CALLPROG) {
			op = p->op;
			break;
		}
	/* hide the dial string, so that no DTMF is generated */
	save = dial_string_[0];
	dial_string_[0] = 0;
	if (op && op->create)
		dp = op->create(handle_, DP_CALLPROG, caller_, srate_, frag_,
				op);
	dial_string_[0] = save;
	if (!dp) {
		ERR("cannot create CALLPROG.\n");
		return -1;
	}
	dp_ = dp;
	run_ = Run::Dp;
	return 0;
}

/* ATD: dials number (NULL: the last one again); -1 on failure */
int Modem::dial(const char *number)
{
	if (number) {
		strncpy(dial_string_, number, sizeof(dial_string_) - 1);
		dial_string_[sizeof(dial_string_) - 1] = 0;
	}
	DBG("modem dial: %s...\n", dial_string_);
	dp_requested_ = 0;
	if (dial_start())
		return -1;
	return start_call();
}

/* ATH: the hook, after hanging up a call */
int Modem::hook(int state)
{
	DBG("modem hook...\n");
	if (hook_ == state)
		return 0;
	if (state_ != IDLE)
		hup(true);
	return set_hook(state);
}

/* ATO: back online from online command mode, CONNECT; -1 if not there */
int Modem::online()
{
	DBG("modem online...\n");
	if (state_ != COMMAND_ONLINE)
		return -1;
	command_ = false;
	set_state(ONLINE);
	report(Result::Connect);
	return 0;
}

/* the speaker as ATM and ATL say outside a call (on only with M2) */
void Modem::update_speaker()
{
	host_.speaker(*this, sregs_[SREG_SPEAKER_CONTROL] == 2 ?
		      sregs_[SREG_SPEAKER_VOLUME] : 0);
}

/* hangs up (ATH, DTR off) */
void Modem::hangup()
{
	hup(true);
}

/* hangs up, and runs at once what was due later (the stop) */
void Modem::hangup_now()
{
	hup(true);
	if (later_ != Later::None) {
		later_at_ = count_;
		Later what = later_;
		later_ = Later::None;
		if (what == Later::Stop)
			stop_call();
		else if (what == Later::ChangeDp)
			change_dp();
		else if (what == Later::CallerIdStop)
			cid_stop();
	}
}

/* the country by T.35 code id or by name, with its dial pause and hook
   flash times; -1 if there is none */
int Modem::homolog_init(int id, const char *name)
{
	for (const struct homolog_set *set = homolog_set; set->name; set++)
		if (set->id == id || (name && *name && !strcmp(name, set->name))) {
			homolog_ = set;
			sregs_[SREG_DIAL_PAUSE_TIME] = set->params->DialPauseTime;
			sregs_[SREG_FLASH_TIMER] = set->params->HookFlashTime;
			return 0;
		}
	return -1;
}

/* data, fax or voice (AT+FCLASS); voice stops when it is left */
int Modem::set_mode(enum MODEM_MODE mode)
{
	DBG("modem set mode: -> %d...\n", mode);
	if (mode_ == mode)
		return 0;
	if (mode_ == MODEM_MODE_VOICE && voice_)
		voice_stop();
	mode_ = mode;
	return 0;
}

/* ATZ: a call hung up; the S-registers, the rates, modem-on-hold and
   the mode from the start (the country kept) */
int Modem::reset()
{
	DBG("modem reset...\n");
	if (state_ != IDLE)
		hup(true);
	else if (started_)
		stop_call();
	else if (hook_)
		set_hook(MODEM_HOOK_ON);
	set_state(IDLE);
	command_ = true;
	min_rate_ = MODEM_MIN_RATE;
	max_rate_ = MODEM_MAX_RATE;
	moh_off_ = moh_req_ = 0;
	moh_timer_ = 5;
	sregs_init();
	kept_init();
	homolog_init(homolog_->id, nullptr);
	set_mode(MODEM_MODE_DATA);
	return 0;
}

/* the S-registers' defaults */
void Modem::sregs_init()
{
	sregs_[SREG_ESCAPE_CHAR] = '+';
	sregs_[SREG_CR_CHAR] = '\r';
	sregs_[SREG_LF_CHAR] = '\n';
	sregs_[SREG_BS_CHAR] = '\b';
	sregs_[SREG_DIAL_TONE_WAIT_TIME] = 2;          /* s */
	sregs_[SREG_WAIT_CARRIER_AFTER_DIAL] = 60;     /* s */
	sregs_[SREG_DIAL_PAUSE_TIME] = 2;              /* s */
	sregs_[SREG_CARRIER_DETECT_RESPONSE_TIME] = 6; /* 0.1 s */
	sregs_[SREG_CARRIER_LOSS_DISCONNECT_TIME] = 7; /* 0.1 s */
	sregs_[SREG_DTMF_DURATION] = 100;              /* ms */
	sregs_[SREG_ESCAPE_PROMPT_DELAY] = 50;         /* ms */
	sregs_[SREG_FLASH_TIMER] = 20;                 /* 10 ms */
	sregs_[SREG_ECHO] = 1;
	sregs_[SREG_QUIET] = 0;
	sregs_[SREG_VERBOSE] = 1;
	sregs_[SREG_TONE_OR_PULSE] = 1;                /* tone */
	sregs_[SREG_X_CODE] = 4;
	sregs_[SREG_SPEAKER_CONTROL] = 1;
	sregs_[SREG_SPEAKER_VOLUME] = 3;
	sregs_[SREG_AUTOMODE] = 1;
	sregs_[SREG_DP] = DP_V92;
	sregs_[SREG_ANS_DELAY] = 2;                    /* s */
	sregs_[SREG_LINE_QUALITY_CONTROL] = 0;
	sregs_[SREG_CD] = 0;
	sregs_[SREG_FLOW_CONTROL] = 0;
	sregs_[SREG_CONNNECT_MSG_FORMAT] = 0;
	sregs_[SREG_CONNNECT_MSG_SPEED_SRC] = 0;
	sregs_[SREG_EC] = 1;
	sregs_[SREG_COMP] = 0x3;
}

/*
 *    caller ID
 */

/* caller ID is over (done, failed or timed out): the call stops unless
   the ring detector runs; then the answer if it is due */
void Modem::cid_stop()
{
	DBG("modem_cid_stop...\n");
	later_ = Later::None;
	if (!rd_) {
		run_ = Run::None;
		stop_call();
	} else {
		CID_delete(cid_);
		cid_ = nullptr;
	}
	/* go on and answer, if that is due */
	if (ANSWER_AFTER_RINGS && TOTAL_RINGS >= ANSWER_AFTER_RINGS) {
		TOTAL_RINGS = 0;
		answer();
	}
}

/* caller ID on the line's samples, silence out */
void Modem::cid_process(int16_t *in, int16_t *out, int count)
{
	int status;
	memset(out, 0, count * sizeof(*out));
	status = CID_process(cid_, in, count);
	if (status) {
		if (status < 0)
			DBG("CID failed.\n");
		cid_stop();
	}
}

/* caller ID for ms, snooping the line (or along with the ring
   detector); -1 if a call runs or it cannot be created */
int Modem::cid_start(unsigned long ms)
{
	DBG("modem_cid_start: timeout = %lu...\n", ms / TICK);
	if (started_ && !rd_)
		return -1;
	cid_ = CID_create(handle_, srate_, cid_requested_);
	if (!cid_)
		return -1;
	later(Later::CallerIdStop, (unsigned long)srate_ * ms / 1000);
	if (started_ && rd_)
		return 0;
	run_ = Run::CallerId;
	set_hook(MODEM_HOOK_SNOOPING);
	return do_start();
}

/*
 *    the ring detector
 */

/* the ring detector on the samples: a ring's start (ring()), its end
   with its frequency and length (the ring check); caller ID with it */
void Modem::rd_process(int16_t *in, int16_t *out, int count)
{
	int ret;
	memset(out, 0, count * sizeof(*out));
	ret = RD_process(rd_, in, count);
	if (ret) {
		long freq, duration;
		RD_ring_details(rd_, &freq, &duration);
		DBG("ring details: freq = %ld, duration = %ld\n", freq,
		    duration);
		if (freq == 0) {
			DBG("report ring start...\n");
			ring();
		} else if (freq > 0) {
			DBG("report ring end...\n");
			event_ |= EV_RING_CHECK;
			if (ring_count_ <= 1)
				ring_count_ = duration * freq / 1000;
			ring_last_ = host_.now_ms();
		} else
			ERR("RD returns %ld freq. (duration %ld)\n", freq,
			    duration);
	}
	if (cid_)
		cid_process(in, out, count);
}

/* the line's ring detector, snooping (a hardware modem on hook) */
int Modem::ring_detector_start()
{
	DBG("modem_ring_detector_start...\n");
	if (rd_) {
		ERR("modem_ring_detector_start: rd_obj already exists!\n");
		return -1;
	}
	rd_ = RD_create(handle_, srate_);
	run_ = Run::RingDetector;
	set_hook(MODEM_HOOK_SNOOPING);
	return do_start();
}

/*
 *    voice
 */

/* the voice engine on the samples; its OK, CONNECT, ERROR to the DTE */
void Modem::voice_process(int16_t *in, int16_t *out, int count)
{
	int status = VOICE_process(voice_, in, out, count);
	if (status) {
		DBG("modem_voice_process: status %d\n", status);
		switch (status) {
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
		if (status < 0) {
			DBG("VOICE failed.\n");
			voice_stop();
		}
	}
}

/* AT+VTX, +VRX, +VTS: a command to the voice engine, 1 (no result
   now); -1 without voice or for another command */
int Modem::voice_command(enum VOICE_CMD cmd)
{
	DBG("modem_voice_command: %u...\n", cmd);
	switch (cmd) {
	case VOICE_CMD_BEEP:
	case VOICE_CMD_DTMF:
	case VOICE_CMD_STATE_RX:
	case VOICE_CMD_STATE_TX:
		if (!voice_)
			return -1;
		command_ = false;
		VOICE_command(voice_, cmd);
		return 1;
	default:
		return -1;
	}
}

/* the voice engine, off hook (the ring detector stops); -1 if a call
   runs */
int Modem::voice_start()
{
	DBG("modem_voice_start...\n");
	if (voice_)
		return 0;
	if (started_) {
		if (rd_)
			stop_call();
		else
			return -1;
	}
	voice_ = VOICE_create(handle_, srate_);
	if (!voice_)
		return -1;
	later_ = Later::None;
	run_ = Run::Voice;
	set_hook(MODEM_HOOK_OFF);
	return do_start();
}

/* the end of voice: the call stopped, on hook */
void Modem::voice_stop()
{
	DBG("modem_voice_stop...\n");
	run_ = Run::None;
	later_ = Later::None;
	if (voice_) {
		VOICE_delete(voice_);
		voice_ = nullptr;
	}
	stop_call();
	set_hook(MODEM_HOOK_ON);
}

/* AT+VLS: none (voice off) or the line (voice on: 1, no result now) */
int Modem::voice_set_device(unsigned device)
{
	DBG("modem_voice_set_device: dev = %u...\n", device);
	if (device == VOICE_DEVICE_NONE) {
		if (voice_)
			voice_stop();
		return 0;
	} else if (device == VOICE_DEVICE_LINE) {
		if (voice_)
			return 0;
		if (voice_start())
			return -1;
		return 1;
	}
	return -1;
}

/* the voice parameters' defaults (AT+VIP) */
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
}

/*
 *    fax (class 1)
 */

/* the fax engine on the samples; its results to the DTE */
void Modem::fax_process(int16_t *in, int16_t *out, int count)
{
	int ret = FAX_process(fax_, in, out, count);
	if (ret) {
		DBG("fax_process: %d\n", ret);
		switch (ret) {
		case FAX_STATUS_OK:
			set_state(COMMAND_ONLINE);
			report(Result::Ok);
			command_ = true;
			commands_due_ = true;
			break;
		case FAX_STATUS_CONNECT:
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
		if (ret < 0) {
			DBG("FAX failed.\n");
			stop_call();
		}
	}
}

/* the fax engine (class 1) instead of a pump; -1 if it cannot be
   created */
int Modem::fax_start()
{
	DBG("modem_fax_start...\n");
	fax_ = FAX_create(handle_, caller_, srate_);
	if (!fax_)
		return -1;
	later_ = Later::None;
	run_ = Run::Fax;
	return 0;
}

} // namespace dcore
