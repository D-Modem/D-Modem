/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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

#include <stdint.h>
#include <time.h>
#include <vector>

#include <modem_defs.h>
#include <modem_dp.h>

#include "modem_homolog.h"

namespace dlink { class Link; }

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
inline unsigned long monotonic_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
}

/* the program around the modem */
class Host {
public:
	/* the line: the start and the end of a call's audio (the modem
	   tells which call: caller(), dial_string()); 0 on success */
	virtual int start(Modem &m) = 0;
	virtual int stop(Modem &m) = 0;
	/* MODEM_HOOK_ON, _OFF, _SNOOPING; 0 on success */
	virtual int set_hook(Modem &m, int state) { (void)m; (void)state; return 0; }
	virtual void speaker(Modem &m, int volume) { (void)m; (void)volume; }
	/* the line's delay, which Smart Link's original library asks for
	   (the open data pumps do not): the samples between the modem's
	   output and its input, and a change of it (> 0: silence out, < 0:
	   input dropped) */
	virtual int io_delay(Modem &m) { (void)m; return 0; }
	virtual void update_delay(Modem &m, int samples) { (void)m; (void)samples; }
	/* codec type, pulse dial */
	virtual int codec_type(Modem &m) { (void)m; return CODEC_UNKNOWN; }
	virtual int pulse_dial(Modem &m, int val) { (void)m; (void)val; return -1; }
	/* for ATI3 */
	virtual const char *driver_name() { return nullptr; }

	/* the DTE: responses and received data; returns how many it took */
	virtual int dte_write(Modem &m, const char *buf, int n) = 0;

	/* fax over T.38 (Modem::t38_start()): an IFP packet of the fax
	   engine's to send (a struct t38_ifp, dsp/t38.h) */
	virtual void t38_send(Modem &m, const void *ifp) { (void)m; (void)ifp; }

	/* the time in ms, from any origin */
	virtual unsigned long now_ms() = 0;
protected:
	~Host() = default;
};

struct Options {
	const char *name = "modem";     /* for the log */
	const char *dev_name = nullptr; /* for ATI3 */
	int rate = 8000;                /* the line's sample rate */
	const char *country = nullptr;  /* by name; NULL: USA */
};

enum class Result {
	Ok = 0, Connect = 1, Ring = 2, NoCarrier = 3, Error = 4,
	NoDialtone = 6, Busy = 7, NoAnswer = 8,
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
	int dte_input(const char *buf, int n);
	int dte_space() const { return xmit_size_ - xmit_count_; }
	/* the line's samples, while started(): n in, n out */
	void process(const int16_t *in, int16_t *out, int n);
	/* the timers: what is due now; the ms until the next one, or -1 */
	void poll();
	long next_timer_ms();

	/* a ring signal on the line (its cadence is checked) */
	void ring();
	/* a call that rings by itself (SIP): on = false, the caller gave up */
	void ring_signal(bool on);
	/* hang up (ATH, DTR off); hangup_now() also stops at once;
	   line_dropped(): the line ended the call (a SIP call that is over),
	   at once */
	void hangup();
	void hangup_now();
	void line_dropped();
	/* ATA, ATD<number> */
	int answer();
	int dial(const char *number);
	/* the call's fax over T.38 instead of the audio, for the rest of
	   the call (the line went over to it): the fax engine's packets to
	   Host::t38_send, max_data data octets in one at most (0: 300),
	   process() still its time (its audio silence); t38_rx(): a packet
	   that came.  0, or -1 when the modem is not in fax mode (or its fax
	   has no T.38).  t38_stop(): back to the audio */
	int t38_start(int max_data);
	void t38_stop();
	void t38_rx(const void *ifp);
	bool t38() const { return t38_; }
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
	int rate() const { return srate_; }
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
	unsigned tx_rate() const { return tx_rate_; }
	unsigned rx_rate() const { return rx_rate_; }
	/* DTE data the remote does not have yet (with V.42: not
	   acknowledged) */
	bool tx_pending() const;
	/* DTE characters in the buffer */
	int xmit_count() const { return xmit_count_; }
	/* LAPM frames not sent or not acknowledged */
	bool link_tx_pending() const;
	/* the pumps' state shared over a call (dsplibs.h), and what they
	   learn of the line over calls (the program keeps it) */
	void *dp_runtime() const { return dp_runtime_; }
	struct dsp_info &dsp_info() { return dsp_info_; }
	/* the network's G.711 law for the pumps of this call and the next
	   ones (dp_runtime_set_law: 1 mu-law, 2 A-law, 0 not known) */
	void set_line_law(int law);

	/* S-registers */
	int sreg(unsigned num) const {
		return num < sizeof(sregs_) ? sregs_[num] : -1;
	}
	/* sets S-register num (8 bits); -1 if there is none */
	int set_sreg(unsigned num, int val) {
		if (num >= sizeof(sregs_))
			return -1;
		sregs_[num] = (unsigned char)val;
		return 0;
	}

	/* the C interface of the data pumps (dsp/), see interface.cpp */
	struct modem *handle() { return handle_; }
	/* and the Modem behind it */
	static Modem *of(struct modem *m);
	/* modem_get_param(), modem_set_param(): MDMPRM_ and the country's
	   parameters; -1 if unknown */
	long get_param(unsigned name);
	long set_param(unsigned name, int val);
	/* the pumps' bits to and from the link */
	int get_bits(int nbits, uint8_t *buf, int n);
	int put_bits(int nbits, const uint8_t *buf, int n);
	/* characters to and from the DTE, for the pumps (fax, voice) */
	int send_to_dte(const char *buf, int n) { return put_chars(buf, n); }
	int recv_from_dte(char *buf, int n) { return get_chars(buf, n); }

	/* the link layer's side (LinkOwner) */
	int get_chars(char *buf, int n);
	int put_chars(const char *buf, int n);
	/* the progress of the pump and of the link */
	enum Status {
		ST_OK, ST_DP_LINK, ST_PACK_LINK, ST_EC_LINK, ST_EC_RELEASE,
		ST_EC_ERROR, ST_ERROR, ST_DP_ERROR, ST_NOCARRIER,
		ST_NODIALTONE, ST_BUSY, ST_NOANSWER,
	};
	/* what the core does with it */
	void update_status(Status s);
	/* what XID (or MNP's LRs) agreed */
	void negotiated(int comp, bool v44, int dict_size, int max_string,
			bool mnp5);
	/* a debug line with the modem's name */
	void logf(const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));

	/* the version and the name ATI shows */
	static const char *const author, *const product, *const version;
	/* the extended parameters of other modems' setup strings it keeps
	   (at most), their values (at most) */
	enum { KEPT_PARAMS = 24, KEPT_VALUES = 9 };

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
	int set_state(State s);
	void hup(bool local);
	int start_call();
	int do_start();
	int stop_call();
	int set_hook(int state);
	void setup_config();
	void change_dp();
	int dial_start();
	void dialed();
	int link_init();
	void report(Result r);
	void events(unsigned ev);
	void schedule_event(unsigned mask, unsigned long ms);
	void sregs_init();
	int homolog_init(int id, const char *name);
	int set_mode(enum MODEM_MODE mode);
	int reset();
	int online();
	int hook(int state);
	void update_speaker();
	int check_escape(const char *buf, int n);
	void run(int16_t *in, int16_t *out, int n);
	void dp_process(int16_t *in, int16_t *out, int n);
	void cid_process(int16_t *in, int16_t *out, int n);
	void cid_stop();
	int cid_start(unsigned long ms);
	void voice_process(int16_t *in, int16_t *out, int n);
	int voice_start();
	void voice_stop();
	int voice_command(enum VOICE_CMD cmd);
	int voice_tone_next();
	int voice_set_device(unsigned device);
	void voice_init();
	void fax_process(int16_t *in, int16_t *out, int n);
	int fax_start();
	static void fax_t38_send(void *ctx, const void *ifp);
	void later(Later what, unsigned long samples);
	const char *dp_name(int id) const;
	const char *protocol_name() const;
	const char *compression_name() const;
	void set_cause(Cause c);
	void call_stats();

	/* the AT interpreter (at.cpp) */
	void at_process();
	int at_command(char *cmd);
	int at_set_sreg(int num, char c, int lo, int hi, int *len);
	int at_A(char *p, int *len);
	int at_B(char *p, int *len);
	int at_D(char *p, int *len);
	int at_H(char *p, int *len);
	int at_I(char *p, int *len);
	int at_O(char *p, int *len);
	int at_ampV(char *p, int *len);
	int at_S(char *p, int *len);
	int at_Z(char *p, int *len);
	int at_GCI(char *p, int *len);
	int at_MS(char *p, int *len);
	int at_PMH(char *p, int *len);
	int at_FCLASS(char *p, int *len);
	int at_CID(char *p, int *len);
	int at_voice(char *p, int *len);
	int at_fax(char *p, int *len);
	int fax_command(enum FAX_CMD cmd, unsigned val);
	int at_kept(const char *p, int *len);
	void kept_init();
	/* a kept parameter's values (its name as kept_params has it) */
	const unsigned long *kept(const char *name) const;
	/* \N<n>: S103 and +ES; and +ES's values as a \N in S103 */
	void set_ec_mode(int n);
	void ec_mode_from_es();
	struct VoiceParam;
	int voice_params(char *p, const char *cmd, VoiceParam *param,
			 int *len, const char *prefix, const char *suffix);
	void puts(const char *s);
	void crlf();

	Host &host_;
	struct modem *handle_;
	bool ok_ = false;
	const char *name_, *dev_name_;
	int srate_, frag_;

	/* the call */
	unsigned state_ = IDLE;
	bool started_ = false;
	bool command_ = true;
	/* a fax or voice command ended: the DTE's characters after its
	   data are its next commands (process() runs them) */
	bool commands_due_ = false;
	Result result_ = Result::Ok;
	bool have_result_ = false;
	int hook_ = MODEM_HOOK_ON;
	bool caller_ = false;
	bool carrier_ = false;
	unsigned rx_rate_ = 0, tx_rate_ = 0;
	enum MODEM_MODE mode_ = MODEM_MODE_DATA;
	unsigned min_rate_, max_rate_;
	/* V.92 modem-on-hold: +PMH (0 on), +PMHT (the T1 code we grant),
	   +PMHR (ours, until the pump takes it) */
	int moh_off_ = 0, moh_timer_ = 5, moh_req_ = 0;
	unsigned char sregs_[256] = {};
	/* the extended parameters of other modems' setup strings, kept
	   with no effect (at.cpp): their values, how many */
	unsigned long kept_[KEPT_PARAMS][KEPT_VALUES] = {};
	unsigned kept_n_[KEPT_PARAMS] = {};
	const struct homolog_set *homolog_ = nullptr;

	/* events and timers */
	unsigned event_ = 0, new_event_ = 0;
	bool timer_on_ = false;
	unsigned long timer_at_ = 0;
	unsigned long count_ = 0;
	unsigned long later_at_ = 0;
	Later later_ = Later::None;

	/* AT */
	unsigned at_count_ = 0;
	char at_line_[64] = {};
	char at_cmd_[64] = {};

	/* rings */
	unsigned ring_count_ = 0;
	unsigned long ring_first_ = 0, ring_last_ = 0;
	unsigned cid_requested_ = 0;
	void *cid_ = nullptr;

	char dial_string_[128] = {};
	/* what call progress dials: the dial string, and after a ';'
	   (dialed_: in command state, the call kept) what ATD adds; the
	   rest of the ATD line, to run then */
	char dialing_[128] = {};
	bool dialed_ = false;
	char dial_rest_[64] = {};
	unsigned escape_count_ = 0;
	unsigned long last_esc_check_ = 0;

	/* the DTE's characters on their way to the line or the AT
	   interpreter */
	char *xmit_ = nullptr;
	int xmit_size_ = 0, xmit_head_ = 0, xmit_tail_ = 0, xmit_count_ = 0;

	/* the link layer: what is asked (ec LAPM, mnp, mnp_first,
	   required: see dlink::Config), then what the call has */
	struct Cfg {
		bool ec, ec_detector;
		bool mnp, mnp_first, required;
		int mnp_info;           /* N401 */
		int tx_win, rx_win, tx_info, rx_info;
		int comp, dict_size, max_string;
		bool v44;               /* V.44 rather than V.42bis */
		bool mnp5;
		bool mnp_on;            /* the call has MNP */
	} cfg_ = {};
	dlink::Link *link_ = nullptr;
	LinkOwner *link_owner_ = nullptr;

	/* the data pumps */
	unsigned dp_requested_ = 0;
	int line_law_ = 0;
	Run run_ = Run::None;
	struct dp *dp_ = nullptr;
	void *dp_runtime_ = nullptr;
	void *dcr_ = nullptr;
	struct dsp_info dsp_info_ = {};
	/* the last call's statistics, or this one's (ATI11): the pumps'
	   (MDMPRM_STATS), and the core's own: whether there was one, fax
	   or not, when it started, came up (CONNECT) and ended (host ms),
	   its modulation (the pump's id), its rates at CONNECT and at its
	   end, why it ended, the line's power each way while it was up
	   (sums of squares over samples) */
	struct dp_stats dp_stats_ = {};
	struct CallStats {
		bool call, fax, connected, ended;
		unsigned long start, up, end;
		int dp;
		unsigned tx0, rx0, tx1, rx1;
		Cause cause;
		double rx_pow, tx_pow;
		unsigned long samples;
	} stats_ = {};
	int16_t *scratch_ = nullptr;
	int scratch_len_ = 0;
	void *voice_ = nullptr;
	struct voice_info voice_info_ = {};
	/* the elements of a +VTS string, played in turn: a DTMF digit
	   (its character), or a tone of f1 and f2 Hz (0: none); d in 10 ms */
	struct VoiceTone {
		char dtmf;
		unsigned f1, f2, d;
	};
	std::vector<VoiceTone> vts_;
	size_t vts_next_ = 0;
	void *fax_ = nullptr;
	/* the call's fax over T.38 (its data octets in a packet) */
	bool t38_ = false;
	int t38_max_ = 0;

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
