/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dcore.h - the modem core: the AT command interpreter, the
 *              S-registers, the result codes, the call's state machine
 *              and its timers, between a DTE, the data pumps (dsp/) and
 *              the data link layer (link/)
 *
 *    A program that runs a modem (dmodemd, libdmodem, a test) is its
 *    Host: it has the line (the call's audio) and the DTE, and it tells
 *    the time.  It hands the modem the DTE's characters (dte_input()), the
 *    line's samples while the modem is started (process()), and calls
 *    poll() for the timers.
 *
 *    The behaviour began as Smart Link's slmodemd core's (modem.c,
 *    modem_at.c, modem_param.c): the same commands, responses and
 *    states; none of its code is left.
 *
 */
#ifndef DMODEM_CORE_DCORE_H
#define DMODEM_CORE_DCORE_H

#include <ctime>
#include <string>
#include <vector>

#include <modem_defs.h>
#include <modem_dp.h>

#include "modem_homolog.h"
#include "v13.h"
#include "v54.h"

/* printf's format checked by GCC and Clang (not MSVC, nor clang-cl) */
#ifndef DMODEM_PRINTF
#ifdef __GNUC__
#define DMODEM_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define DMODEM_PRINTF(f, a)
#endif
#endif

namespace dlink { class Link; enum class SyncCommand; }

namespace tones {
struct Country;
}

namespace dsp {
struct CngDetect;
}

namespace dcore { class Modem; }

/* what the data pumps get as their struct modem (modem_dp.h): the way
   back to the Modem */
struct modem {
	dcore::Modem *self;
};

#define MODEM_MIN_RATE 300
#define MODEM_MAX_RATE 56000
#define MODEM_DEFAULT_COUNTRY_CODE 0xb5 /* USA */

namespace dcore {

class Modem;
struct LinkOwner;

/* CLOCK_MONOTONIC in ms (a Host's now_ms() on a real line) */
inline u64 monotonic_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<u64>(ts.tv_sec) * 1000 +
		static_cast<u64>(ts.tv_nsec) / 1000000;
}

/* the program around the modem */
class Host {
public:
	/* the line: the start and the end of a call's audio (the modem
	   tells which call: caller(), dial_string()); 0 on success */
	virtual s16 start(Modem &m) = 0;
	virtual s16 stop(Modem &m) = 0;
	/* MODEM_HOOK_ON, _OFF, _SNOOPING; 0 on success */
	virtual s16 set_hook(Modem &m, s16 state) { (void)m; (void)state; return 0; }
	/* a hook flash of ms on the call's line (+VTS's !): whether the
	   line can have one (no: the command is an ERROR), and the flash
	   itself, which the line signals its own way (the modem's audio is
	   silent for as long) */
	virtual bool can_flash(Modem &m) { (void)m; return false; }
	/* (see can_flash()) */
	virtual void hook_flash(Modem &m, u16 ms) { (void)m; (void)ms; }
	/* the line tells when a call it dials is answered and why one
	   fails (Modem::line_answered(), line_failed()), as SIP does; else
	   a voice dial takes the call for answered after +VRN */
	virtual bool signals_answer(Modem &m) { (void)m; return false; }
	/* the line is an analogue telephone line (a softmodem's codec):
	   the modem dials the number itself (its call progress pump in
	   DTMF, after a dial tone with X2 or X4), and offers no mode of a
	   digital modem (V.90's and V.92's, V.91: +MS's V.90 and up are
	   V.34).  Else the line is a digital network's (SIP, a PBX's
	   channel), which dials the number */
	virtual bool analog_line(Modem &m) { (void)m; return false; }
	/* the call is a clear 64 kbit/s digital channel (an ISDN data
	   call: a SIP call in CLEARMODE, a PBX's call with a digital
	   bearer): the modem runs its clear channel pump (DP_CLEAR) on
	   it, whatever +MS says, with no call progress */
	virtual bool clear_line(Modem &m) { (void)m; return false; }
	/* the line tells when a phone on the modem's own jack (the local
	   phone, V.253's) goes off or on hook (Modem::local_phone()) */
	virtual bool senses_local_phone(Modem &m) { (void)m; return false; }
	/* a handset or headset on the modem itself, on a codec of its own
	   (V.253's H0): +VLS=14, the handset alone with the line on hook,
	   and 15, the handset on the call with the line off hook */
	virtual bool has_handset(Modem &m) { (void)m; return false; }
	/* where the voice engine's audio is (+VLS, has_handset()):
	   VOICE_DEVICE_LINE the line's, as always (and in every other
	   mode); _HANDSET the handset's instead (process() gets its
	   microphone, its output goes to the handset's earpiece);
	   _HANDSET_LINE the line's, with the handset on the call too (the
	   Host mixes it into the line both ways).  0, or -1 if it cannot */
	virtual s16 voice_route(Modem &m, u16 device) {
		(void)m;
		return device == VOICE_DEVICE_LINE ? 0 : -1;
	}
	/* the speaker's volume (ATL, S30 with ATM2), 0: off */
	virtual void speaker(Modem &m, s16 volume) { (void)m; (void)volume; }
	/* the line's delay, which Smart Link's original library asks for
	   (the open data pumps do not): the samples between the modem's
	   output and its input, and a change of it (> 0: silence out, < 0:
	   input dropped) */
	virtual s32 io_delay(Modem &m) { (void)m; return 0; }
	/* (see io_delay()) */
	virtual void update_delay(Modem &m, s32 samples) { (void)m; (void)samples; }
	/* codec type, pulse dial */
	virtual s16 codec_type(Modem &m) { (void)m; return CODEC_UNKNOWN; }
	/* a digit dialled in pulses (the pump's MDMPRM_PULSE_DIAL); 0, or -1
	   for a line that cannot */
	virtual s16 pulse_dial(Modem &m, s16 val) { (void)m; (void)val; return -1; }
	/* pulse dialling (ATP): the line broken (true: on hook) or made
	   again, at once; the pump times the pulses (the country's make,
	   break and gap).  0, or -1 for a line that cannot (a digital
	   network's: the pump dials in DTMF instead) */
	virtual s16 pulse_hook(Modem &m, bool broken) { (void)m; (void)broken; return -1; }
	/* pulse dialling begins (on: before its first break) or is over
	   (the dial string's end, a ';' or a 'T'), for a line that does
	   something of its own meanwhile */
	virtual void pulse_dialling(Modem &m, bool on) { (void)m; (void)on; }
	/* for ATI3 */
	virtual const char *driver_name() { return nullptr; }
	/* the country (-c, +GCI) by its name, at the start and on each
	   change: a line that sets its own termination and ring detection
	   (a DAA's registers) sets the country's */
	virtual void country(Modem &m, const char *name) { (void)m; (void)name; }

	/* the DTE: responses and received data; returns how many it took */
	virtual s32 dte_write(Modem &m, const char *buf, s32 n) = 0;

	/* fax over T.38 (Modem::t38_start()): an IFP packet of the fax
	   engine's to send (a struct t38_ifp, dsp/t38.h) */
	virtual void t38_send(Modem &m, const void *ifp) { (void)m; (void)ifp; }

	/* the time in ms, from any origin */
	virtual u64 now_ms() = 0;
protected:
	~Host() = default;
};

struct Options {
	const char *name = "modem";     /* for the log */
	const char *dev_name = nullptr; /* for ATI3 */
	s32 rate = 8000;                /* the line's sample rate */
	const char *country = nullptr;  /* by name; NULL: USA */
	/* the program takes a fax machine that calls itself (libdmodem's
	   fax receiver): answering in data mode, the modem finds one out
	   (fax_caller()) and, for a V.8 fax call, stops its data answer;
	   with +FAA=1 it takes the call on in class 1 instead */
	bool fax_detect = false;
};

enum class Result {
	Ok = 0, Connect = 1, Ring = 2, NoCarrier = 3, Error = 4,
	NoDialtone = 6, Busy = 7, NoAnswer = 8,
	/* adaptive answer (+FAA=1): the call is a fax call, or a data
	   call (with ATV0 88 and 89, D-Modem's own numbers) */
	Fax = 9, Data = 10,
};

/* the call (for programs that embed the modem) */
enum class LinkState { Idle, Connecting, Online, HangingUp };

class Modem {
public:
	/* a modem on host; ok() if it could be set up */
	Modem(Host &host, const Options &opt);
	~Modem();
	Modem(const Modem &) = delete;
	Modem &operator=(const Modem &) = delete;
	/* the constructor succeeded */
	bool ok() const { return ok_; }

	/*
	 *    the program -> the modem
	 */

	/* characters from the DTE: commands, or data when online; returns
	   how many it took (see dte_space()) */
	s32 dte_input(const char *buf, s32 n);
	/* the characters dte_input() takes now (in V.80's Synchronous
	   Access Mode, up to +ITF's <off> in the buffer) */
	s16 dte_space() const;
	/* the line's samples, while started(): n in, n out */
	void process(const s16 *in, s16 *out, s32 n);
	/* the timers: what is due now; the ms until the next one, or -1 */
	void poll();
	s32 next_timer_ms();

	/* a ring signal on the line (its cadence is checked) */
	void ring();
	/* the local phone (a phone on the modem's own jack: Host's
	   senses_local_phone()) off or on hook: in voice mode V.253's
	   event 8, <DLE>H or <DLE>h, to the DTE; logged */
	void local_phone(bool off_hook);
	/* (see local_phone()) */
	bool local_phone_off_hook() const { return local_phone_off_; }
	/* a call that rings by itself (SIP): on = false, the caller gave up */
	void ring_signal(bool on);
	/* the caller of such a call, ahead of its first ring_signal(): date
	   the local "MMDDHHMM" (or ""), number and name ("" when not known,
	   nullptr when withheld); reported after the first RING as caller
	   ID on the line is, when +VCID asks for it */
	void caller_id(const char *date, const char *number,
		       const char *name);
	/* hang up (ATH, DTR off); hangup_now() also stops at once;
	   line_dropped(): the line ended the call (a SIP call that is over),
	   at once */
	void hangup();
	void hangup_now();
	void line_dropped();
	/* the line's word on the call it dials: answered (a voice dial's
	   OK), or failed with r (BUSY, NO ANSWER, NO CARRIER: the dial's
	   result, the line's line_dropped() to follow) */
	void line_answered();
	void line_failed(Result r);
	/* ATA, ATD<number> (adaptive false: no +FAA adaptive answer) */
	s16 answer(bool adaptive = true);
	s16 dial(const char *number);
	/* the call's fax over T.38 instead of the audio, for the rest of
	   the call (the line went over to it): the fax engine's packets to
	   Host::t38_send, max_data data octets in one at most (0: 300),
	   process() still its time (its audio silence); t38_rx(): a packet
	   that came.  0, or -1 when the modem is not in fax mode (or its fax
	   has no T.38).  t38_stop(): back to the audio */
	s16 t38_start(s32 max_data);
	void t38_stop();
	void t38_rx(const void *ifp);
	/* the fax is over T.38 (t38_start() until t38_stop()) */
	bool t38() const { return t38_; }
	/* answering with +FAA=1, the call not yet found to be data or fax
	   (a T.38 offer now makes it a fax call: t38_start()) */
	bool adaptive_answering() const {
		return adapt_ == Adapt::Data || adapt_ == Adapt::Fax;
	}
	/* the call we answered is a fax machine's (its CNG, a V.8 CM of a
	   fax call, T.38): with +FAA=1 the modem has gone over to class 1,
	   with Options::fax_detect it waits for the program to take it */
	bool fax_caller() const { return fax_caller_; }
	/* the device reports an error */
	void error();

	/*
	 *    the modem -> the program
	 */

	/* its name in the log */
	const char *name() const { return name_; }
	/* the device's name (ATI3) */
	const char *dev_name() const { return dev_name_; }
	/* the line's sample rate (Hz) */
	s32 rate() const { return srate_; }
	/* the line runs above the voice band's rates (48000 Hz and up):
	   only the wideband pump (DP_VWB) runs there, and with no call
	   progress (a bare pair's, no exchange) */
	bool wideband() const { return srate_ > 9600; }
	/* a line without an exchange: AT&L1 or AT&L2, or wideband (no
	   dial tone, dialling or ringing) */
	bool leased() const { return wideband() || sregs_[SREG_LEASED]; }
	/* a 4-wire leased line (AT&L2): each way on a pair of its own */
	bool four_wire() const { return sregs_[SREG_LEASED] == 2; }
	/* the calling pump starts at once, with no call progress: the
	   wideband one, and on a leased line V.wb's and the 4-wire pumps
	   (V.29, V.33, V.27ter) and V.91 on a 4-wire digital circuit,
	   which have no answer tone to wait for;
	   the voice band's others listen for the answerer's tone first */
	bool leased_direct() const;
	/* the call is on a clear 64 kbit/s channel: +MS=CLEAR, or the
	   line says so (Host::clear_line()), on a digital line at 8000 Hz:
	   the clear channel pump at once, as on a leased line */
	bool clear_channel() const;
	/* +CBST's V.110 user rate, 0 for none */
	s32 cbst_rate() const;
	/* the line's audio runs (process()) */
	bool started() const { return started_; }
	/* we dialled */
	bool caller() const { return caller_; }
	/* the number dialled */
	const char *dial_string() const { return dial_string_; }
	/* data, fax or voice */
	enum MODEM_MODE mode() const { return mode_; }
	/* the call's state */
	LinkState link_state() const;
	/* a data connection is (or was, in this call) up */
	bool carrier() const { return carrier_; }
	/* the pump's rates (bit/s) */
	u32 tx_rate() const { return tx_rate_; }
	/* (see tx_rate()) */
	u32 rx_rate() const { return rx_rate_; }
	/* DTE data the remote does not have yet (with V.42: not
	   acknowledged) */
	bool tx_pending() const;
	/* DTE characters in the buffer */
	s16 xmit_count() const { return xmit_count_; }
	/* LAPM frames not sent or not acknowledged */
	bool link_tx_pending() const;
	/* the pumps' state shared over a call (dsplibs.h), and what they
	   learn of the line over calls (the program keeps it) */
	void *dp_runtime() const { return dp_runtime_; }
	/* (see dp_runtime()) */
	struct dsp_info &dsp_info() { return dsp_info_; }
	/* the network's G.711 law for the pumps of this call and the next
	   ones (dp_runtime_set_law: 1 mu-law, 2 A-law, 0 not known) */
	void set_line_law(s16 law);

	/* S-registers */
	s16 sreg(u32 num) const {
		return num < sizeof(sregs_) ? sregs_[num] : -1;
	}
	/* sets S-register num (8 bits); -1 if there is none */
	s16 set_sreg(u32 num, s32 val) {
		if (num >= sizeof(sregs_))
			return -1;
		sregs_[num] = static_cast<unsigned char>(val);
		return 0;
	}

	/* the C interface of the data pumps (dsp/), see interface.cpp */
	struct modem *handle() { return handle_; }
	/* and the Modem behind it */
	static Modem *of(struct modem *m);
	/* modem_get_param(), modem_set_param(): MDMPRM_ and the country's
	   parameters; -1 if unknown */
	intptr_t get_param(u32 name);
	s32 set_param(u32 name, s32 val);
	/* the pumps' bits to and from the link */
	s32 get_bits(s16 nbits, u8 *buf, s32 n);
	s32 put_bits(s16 nbits, const u8 *buf, s32 n);
	/* characters to and from the DTE, for the pumps (fax, voice) */
	s32 send_to_dte(const char *buf, s32 n) { return put_chars(buf, n); }
	/* (see send_to_dte()) */
	s32 recv_from_dte(char *buf, s32 n) { return get_chars(buf, n); }

	/* the link layer's side (LinkOwner) */
	s32 get_chars(char *buf, s32 n);
	s32 link_chars(char *buf, s32 n);
	s32 put_chars(const char *buf, s32 n);
	/* the progress of the pump and of the link */
	enum Status {
		ST_OK, ST_DP_LINK, ST_PACK_LINK, ST_EC_LINK, ST_EC_RELEASE,
		ST_EC_ERROR, ST_ERROR, ST_DP_ERROR, ST_NOCARRIER,
		ST_NODIALTONE, ST_BUSY, ST_NOANSWER,
	};
	/* what the core does with it */
	void update_status(Status s);
	/* what XID (or MNP's LRs) agreed */
	void negotiated(s16 comp, bool v44, bool zstd, s32 dict_size,
			s32 max_string, bool mnp5);
	/* the DTE's in-band command in V.80's Synchronous Access Mode */
	void sync_command(dlink::SyncCommand c, u8 tx, u8 rx);
	/* a debug line with the modem's name */
	void logf(const char *fmt, ...) DMODEM_PRINTF(2, 3);

	/* the version and the name ATI shows */
	static const char *const author, *const product, *const version;
	/* the extended parameters of other modems' setup strings it keeps
	   (at most), their values (at most) */
	enum { KEPT_PARAMS = 25, KEPT_VALUES = 9 };

private:
	enum State {
		IDLE = 0x1, DP_ESTAB = 0x2, EC_ESTAB = 0x4, ONLINE = 0x5,
		COMMAND_ONLINE = 0x6, EC_DISC = 0x7, DP_DISC = 0x9,
	};
	/* what process() runs */
	enum class Run { None, Dp, CallerId, Voice, Fax };
	/* what runs after a number of samples */
	enum class Later { None, Stop, ChangeDp, CallerIdStop };
	/* why a call ended (ATI11) */
	enum class Cause {
		None, LocalHangup, RemoteCleardown, LineDropped, CarrierLost,
		NoDialtone, Busy, NoAnswer, TrainingFailed, ProtocolFailure,
		NoCarrier,
	};
	enum { EV_RING_CHECK = 0x01, EV_ESCAPE = 0x02 };

	/* the state machine (modem.c) */
	s16 set_state(State s);
	void hup(bool local);
	s16 start_call(bool keep_line = false);
	s16 do_start();
	s16 stop_call();
	s16 set_hook(s16 state);
	void setup_config();
	void change_dp();
	s16 dial_start();
	void dialed();
	s16 link_init();
	static s16 dte_dirs(u32 direction);
	bool link_compression_ok();
	void report(Result r);
	void events(u16 ev);
	void leased_poll();
	void leased_stop();
	s16 at_ampT(char *p, s16 *len);
	bool link_plain() const;
	s16 loop3_start(bool self_test);
	s16 test_end(bool timer);
	void test_events();
	void test_started();
	void print_test_errors();
	void schedule_event(u16 mask, u32 ms);
	void sregs_init();
	s16 homolog_init(s32 id, const char *name);
	std::string country_name() const;
	s16 set_mode(enum MODEM_MODE mode);
	s16 reset();
	s16 online();
	s16 hook(s16 state);
	void update_speaker();
	s16 check_escape(const char *buf, s32 n);
	void run(s16 *in, s16 *out, s32 n);
	void dp_process(s16 *in, s16 *out, s32 n);
	void cid_process(s16 *in, s16 *out, s32 n);
	void cid_stop();
	s16 cid_start(u32 ms);
	void voice_process(s16 *in, s16 *out, s32 n);
	s16 voice_start(bool off_hook = true);
	void voice_stop();
	void voice_hold();
	void voice_calling_tones();
	s16 voice_command(enum VOICE_CMD cmd);
	s16 voice_tone_next();
	s16 voice_dial(const char *number);
	void keep_dial_string(const char *number);
	void voice_dial_check();
	s32 voice_dial_left_ms();
	void voice_dial_end(Result r);
	void voice_line_gone();
	void voice_drain();
	s16 voice_set_device(u16 device);
	void voice_unroute();
	void voice_init();
	void fax_process(s16 *in, s16 *out, s32 n);
	s16 fax_start();
	void fax_found(const char *why);
	s16 adaptive_param() const;
	static void fax_t38_send(void *ctx, const void *ifp);
	void later(Later what, u32 samples);
	const char *dp_name(s16 id) const;
	const char *protocol_name() const;
	const char *compression_name() const;
	void set_cause(Cause c);
	void call_stats();
	void unimodem_diag();

	/* the AT interpreter (at.cpp) */
	void at_process();
	s16 at_command(char *cmd);
	s16 at_set_sreg(s16 num, char c, s16 lo, s16 hi, s16 *len);
	s16 at_A(char *p, s16 *len);
	s16 at_B(char *p, s16 *len);
	s16 at_D(char *p, s16 *len);
	s16 at_H(char *p, s16 *len);
	s16 at_I(char *p, s16 *len);
	s16 at_O(char *p, s16 *len);
	s16 at_ampV(char *p, s16 *len);
	s16 at_S(char *p, s16 *len);
	s16 at_Z(char *p, s16 *len);
	s16 at_GCI(char *p, s16 *len);
	s16 at_MS(char *p, s16 *len);
	s16 at_PMH(char *p, s16 *len);
	s16 at_FCLASS(char *p, s16 *len);
	s16 at_CID(char *p, s16 *len);
	s16 at_voice(char *p, s16 *len);
	s16 at_fax(char *p, s16 *len);
	s16 fax_command(enum FAX_CMD cmd, u32 val);
	s16 at_kept(const char *p, s16 *len);
	void kept_init();
	/* a kept parameter's values (its name as kept_params has it) */
	const u32 *kept(const char *name) const;
	/* \N<n>: S103 and +ES; and +ES's values as a \N in S103 */
	void set_ec_mode(s16 n);
	void ec_mode_from_es();
	struct VoiceParam;
	s16 voice_params(char *p, const char *cmd, VoiceParam *param,
			 s16 *len, const char *prefix, const char *suffix);
	void puts(const char *s);
	/* (see puts(const char *)) */
	void puts(const std::string &s) { puts(s.c_str()); }
	void crlf();

	Host &host_;
	struct modem *handle_;
	bool ok_ = false;
	const char *name_, *dev_name_;
	s32 srate_;
	s32 frag_;

	/* the call */
	u16 state_ = IDLE;
	bool started_ = false;
	bool command_ = true;
	/* a fax or voice command ended: the DTE's characters after its
	   data are its next commands (process() runs them) */
	bool commands_due_ = false;
	Result result_ = Result::Ok;
	bool have_result_ = false;
	s16 hook_ = MODEM_HOOK_ON;
	bool caller_ = false;
	bool carrier_ = false;
	u32 rx_rate_ = 0, tx_rate_ = 0;
	enum MODEM_MODE mode_ = MODEM_MODE_DATA;
	u32 min_rate_, max_rate_;
	/* V.92 modem-on-hold: +PMH (0 on), +PMHT (the T1 code we grant),
	   +PMHR (ours, until the pump takes it) */
	s16 moh_off_ = 0, moh_timer_ = 5;
	s32 moh_req_ = 0;
	unsigned char sregs_[256] = {};
	/* the extended parameters of other modems' setup strings, kept
	   with no effect (at.cpp): their values, how many */
	u32 kept_[KEPT_PARAMS][KEPT_VALUES] = {};
	u16 kept_n_[KEPT_PARAMS] = {};
	const struct homolog_set *homolog_ = nullptr;
	/* the country's tones (dsp/tones.h: busy and congestion for the
	   call progress pump), and whether homolog_ is only the default's
	   line settings for a country the table has not (tones_only_) */
	const tones::Country *tones_ = nullptr;
	bool tones_only_ = false;

	/* events and timers */
	u16 event_ = 0, new_event_ = 0;
	bool timer_on_ = false;
	u32 timer_at_ = 0;
	u64 count_ = 0;
	u64 later_at_ = 0;
	Later later_ = Later::None;
	/* AT&L1: the call to start again by itself (ATD's or ATA's, or
	   S0's answer) after a NO CARRIER, at leased_at_ms_ (0: none due);
	   S0's answer only once a command line has come since a local
	   hang up (so that one can be typed to stop it) */
	enum class Leased { None, Dial, Answer };
	Leased leased_role_ = Leased::None;
	u64 leased_at_ms_ = 0;
	bool leased_auto_ = true;
	/* when the line's samples last went through (now_ms()): a stop due
	   after samples is done on time once they have stopped coming (a
	   SIP call ringing without early media has none) */
	u64 samples_at_ms_ = 0;

	/* AT */
	u16 at_count_ = 0;
	char at_line_[64] = {};
	char at_cmd_[64] = {};

	/* rings */
	u16 ring_count_ = 0;
	u32 ring_first_ = 0, ring_last_ = 0;
	u16 cid_requested_ = 0;
	void *cid_ = nullptr;
	/* caller_id()'s MDMF message, until the first RING */
	std::vector<u8> cid_msg_;

	char dial_string_[128] = {};
	/* what call progress dials: the dial string, and after a ';'
	   (dialed_: in command state, the call kept) what ATD adds; the
	   rest of the ATD line, to run then */
	char dialing_[128] = {};
	bool dialed_ = false;
	char dial_rest_[64] = {};
	u16 escape_count_ = 0;
	u32 last_esc_check_ = 0;

	/* the DTE's characters on their way to the line or the AT
	   interpreter */
	char *xmit_ = nullptr;
	s16 xmit_size_ = 0, xmit_head_ = 0, xmit_tail_ = 0, xmit_count_ = 0;
	/* the DTE's data still in the buffer when the escape took effect
	   (+++ itself among it): for the line when the call is online
	   again, ahead of what comes then */
	std::string esc_held_;

	/* the link layer: what is asked (ec LAPM, mnp, mnp_first,
	   required: see dlink::Config), then what the call has */
	struct Cfg {
		bool ec, ec_detector;
		bool mnp, mnp_first, required;
		s16 mnp_info;           /* N401 */
		s16 tx_win, rx_win, tx_info, rx_info;
		s16 comp;
		s32 dict_size, max_string;
		bool v44;               /* V.44 rather than V.42bis */
		bool zstd;              /* zstd (V.cvm) rather than either */
		bool mnp5;
		bool mnp_on;            /* the call has MNP */
		bool sync;              /* V.80 Synchronous Access Mode */
	} cfg_ = {};
	/* the directions (Config::comp's bits) +DS's and +DS44's
	   <compression_negotiation> 1 require (link_init()) */
	s16 comp_need42_ = 0, comp_need44_ = 0;
	/* Synchronous Access Mode: the rates the DTE last heard of (an
	   <EM><rate> when they change) */
	u32 sync_tx_ = 0, sync_rx_ = 0;
	/* the retrains and renegotiations the DTE has heard of
	   (<EM><rtn>, <EM><rrn>: all but those it asked for, dte_rt_ and
	   dte_rn_ of them), and all of them: after one, an <EM><rate>
	   once the pump has set its rates again (rate_sets_ past
	   sync_rate_mark_) */
	u32 sync_rt_ = 0, sync_rn_ = 0, sync_events_ = 0;
	u32 dte_rt_ = 0, dte_rn_ = 0;
	u32 rate_sets_ = 0, sync_rate_mark_ = 0;
	bool sync_rate_due_ = false;
	/* the DTE's retrain (1) or renegotiation (2) for the pump
	   (MDMPRM_LINK_REQ: V.80's <EM><rtn>, <EM><rrn>; ATO1, ATO3) */
	s32 link_req_ = 0;
	/* the highest rate to receive at (V.80's <EM><rate>), 0: as
	   max_rate_ */
	u32 max_rx_rate_ = 0;
	bool link_req(s32 what);
	dlink::Link *link_ = nullptr;
	LinkOwner *link_owner_ = nullptr;
	/* V.54's loop tests (AT&T): the test, loop 3's samples back into
	   the pump, the test's end by S18 (ms, 0 none), AT&T0's result
	   due when the TERM has gone */
	v54::LoopTest test_;
	bool loop3_ = false;
	std::deque<s16> loop3_buf_;
	u64 test_end_ms_ = 0;
	bool test_reply_ = false;
	/* V.13's simulated carrier (S48=1, a call with no error
	   correction), the bits to the link through it */
	v13::SimCarrier sim_;
	bool sim_on_ = false;
	std::vector<u8> sim_bits_;

	/* the data pumps */
	u32 dp_requested_ = 0;
	s16 line_law_ = 0;
	Run run_ = Run::None;
	struct dp *dp_ = nullptr;
	void *dp_runtime_ = nullptr;
	void *dcr_ = nullptr;
	struct dsp_info dsp_info_ = {};
	/* the last call's statistics, or this one's (ATI11): the pumps'
	   (MDMPRM_STATS), and the core's own: whether there was one, fax
	   or not, when it started, came up (CONNECT) and ended (host ms),
	   whether we called, its modulation (the pump's id), its rates at
	   CONNECT and at its
	   end, why it ended, the line's power each way while it was up
	   (sums of squares over samples) */
	struct dp_stats dp_stats_ = {};
	struct dp_vcvm vcvm_ = {};      /* V.cvm's: +CVM's settings, the
					   pump's figures (MDMPRM_VCVM) */
	void vcvm_refresh();
	struct CallStats {
		bool call, fax, connected, ended;
		bool caller;
		u32 start, up, end;
		s16 dp;
		u32 tx0, rx0, tx1, rx1;
		Cause cause;
		double rx_pow, tx_pow;
		u64 samples;
	} stats_ = {};
	s16 *scratch_ = nullptr;
	s32 scratch_len_ = 0;
	void *voice_ = nullptr;
	/* +VLS's device while voice_ runs (VOICE_DEVICE_*) */
	u16 voice_device_ = VOICE_DEVICE_NONE;
	struct voice_info voice_info_ = {};
	u16 voice_events_ = 0;          /* +VEM */
	bool local_phone_off_ = false;  /* local_phone() */
	/* the elements of a +VTS string, played in turn: a DTMF digit
	   (its character), a hook flash, or a tone of f1 and f2 Hz (0:
	   none); d in 10 ms */
	struct VoiceTone {
		char dtmf;
		u16 f1, f2, d;
		bool flash;
	};
	std::vector<VoiceTone> vts_;
	size_t vts_next_ = 0;
	std::vector<VoiceTone> dtmf_tones(const char *number);
	/* a voice dial under way (ATD in voice mode): since when (ms) */
	bool voice_dialing_ = false;
	u32 voice_dial_since_ = 0;
	/* it dials the number (in vts_) first; its timers from then */
	bool voice_dial_digits_ = false;
	/* the voice engine's command (a VOICE_CMD: +VTS's tones, +VTX,
	   +VRX, +VTR) until its OK or ERROR, -1 for none */
	s16 voice_cmd_ = -1;
	/* the line gone during +VTX or +VTR (duplex): the DTE's samples
	   discarded up to their end (a <DLE> code) */
	bool voice_drain_ = false, voice_drain_dle_ = false;
	bool voice_drain_duplex_ = false;
	/* the voice call's line kept off hook after +FCLASS left voice
	   mode (V.253 5.3.2), for ATA or ATD on it; the calling tones the
	   voice engine heard so far (CNG, V.25's CT) that +FAA=1 acted on */
	bool voice_held_ = false;
	bool v110_ = false;             /* the call runs V.110 (clear
					   channel) */
	u32 voice_cng_seen_ = 0, voice_ct_seen_ = 0;
	void *fax_ = nullptr;
	/* adaptive answer, while the call we answer is not yet known to
	   be data or fax: Off, or the program's (Options::fax_detect),
	   or +FAA=1's from data mode (Data) or class 1 (Fax: the data
	   answer runs, +FCLASS=0 until a fax is found out or the call
	   ends); the caller found to be a fax machine; its CNG detector
	   and the bursts it found cut short so far; where the answer
	   tone starts (count_) */
	enum class Adapt { Off, Detect, Data, Fax };
	Adapt adapt_ = Adapt::Off;
	bool fax_detect_ = false;
	bool fax_caller_ = false;
	dsp::CngDetect *cng_ = nullptr;
	u16 cng_cut_ = 0;
	u64 answer_tone_at_ = 0;
	/* the call's fax over T.38 (its data octets in a packet) */
	bool t38_ = false;
	s32 t38_max_ = 0;

	friend struct LinkOwner;
};

/* the data pumps register themselves (modem_dp_register()); for AT+MS */
struct DpDriver {
	enum DP_ID id;
	const char *name;
	const char *code;       /* AT+MS shows it */
	struct dp_operations *op;
};
/* the table, ended by an id of 0 */
DpDriver *dp_drivers();

} // namespace dcore

#endif
