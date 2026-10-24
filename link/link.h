/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    link.h - the data link layer of the modem: what goes on between the
 *             DTE's characters and the data pump's bits
 *
 *    After the data pump connects, Link runs the V.42 detection phase
 *    (ODP/ADP, or the HDLC flags of a modem that skips it), then either
 *    V.42 LAPM error correction over HDLC frames, with V.44 or V.42bis
 *    compression when both modems agree on it, or MNP 2 to 4 (V.42's
 *    alternative procedure, with MNP 5 compression) with a modem that
 *    has no LAPM, or plain asynchronous characters (start bit, 8 data
 *    bits, stop bit).
 *
 *    The modem core is the Owner: it gives and takes the DTE's
 *    characters, hears of the link's progress and gets its log lines.
 *    The data pump calls get_bits() and put_bits().
 *
 *    The protocols behave on the line as Smart Link's slmodemd link
 *    layer did (modem_pack, modem_ec, modem_comp), which this began as;
 *    none of its code is left.
 *
 */

#ifndef DMODEM_LINK_H
#define DMODEM_LINK_H

#include <cstdint>

/* printf's format checked by GCC and Clang (not MSVC, nor clang-cl) */
#ifndef DMODEM_PRINTF
#ifdef __GNUC__
#define DMODEM_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define DMODEM_PRINTF(f, a)
#endif
#endif

#include "hdlc.h"
#include "lapm.h"
#include "mnp.h"
#include "mnp5.h"
#include "v42bis.h"
#include "v44.h"

namespace dlink {

/* what the link negotiates and what it is asked for */
struct Config {
	bool ec = false;                /* V.42 LAPM */
	bool detect = false;            /* the V.42 detection phase */
	/* MNP 2 to 4: the originator's when the answerer has no V.42 (no
	   ADP in T400, or LAPM unanswered), first (mnp_first: no detection
	   phase) or alone (without ec); the answerer's on an LR */
	bool mnp = false;
	bool mnp_first = false;
	/* no error correction (LAPM or MNP) agreed: the link fails rather
	   than going async */
	bool required = false;
	/* MNP's offer: framing mode (2 octets, 3 HDLC frames), k, N401,
	   the data phase optimizations (bit 0: 256-octet information
	   fields, with N401 256; bit 1: fixed-field LT and LA), MNP 5
	   (after the LRs: whether the call has it) */
	int mnp_mode = 3;
	int mnp_k = MNP_MAX_K;
	int mnp_info = MNP_MAX_INFO;
	int mnp_opt = 3;
	bool mnp5 = false;
	int tx_win = LAPM_MAX_WIN, rx_win = LAPM_MAX_WIN;
	int tx_info = LAPM_MAX_INFO, rx_info = LAPM_MAX_INFO;
	int comp = 0;                   /* V.42bis: bit 0 decoder (what we
					   receive), bit 1 encoder */
	int dict_size = V42BIS_MAX_CODEWORDS;
	int max_string = V42BIS_MAX_STRING;
	/* V.44 offered with compression; after XID: it, rather than
	   V.42bis, is what the call has */
	bool v44 = true;
	int v44_codewords = V44_MAX_CODEWORDS;
	int v44_max_string = V44_MAX_STRING;
	int v44_history = V44_MAX_HISTORY;
};

/* the error correction a call has */
enum class Protocol { None, Lapm, Mnp };

/* a call's counts (ATI11): the DTE's characters each way; the frames
   of the error correction (LAPM's, MNP's) sent, the information frames
   among them sent again, the frames received intact and those received
   with a bad FCS */
struct Stats {
	unsigned long tx_chars = 0, rx_chars = 0;
	unsigned long tx_frames = 0, tx_again = 0;
	unsigned long rx_frames = 0, rx_errors = 0;
};

enum class Status {
	Detected,       /* the detection phase is over: LAPM, MNP or none
			   (protocol()) */
	Up,             /* data can flow (LAPM connected, or async) */
	Released,       /* LAPM disconnected */
	Error,          /* LAPM or V.42bis failed */
};

class Owner {
public:
	/* the DTE's characters to send; 0 if none */
	virtual int read_chars(uint8_t *buf, int n) = 0;
	/* received characters for the DTE; returns how many it took */
	virtual int write_chars(const uint8_t *buf, int n) = 0;
	/* the link's progress */
	virtual void status(Status s) = 0;
	/* the data pump's transmit rate (bit timers), 0 if not known */
	virtual int tx_rate() { return 0; }
	/* parameters agreed in an XID exchange */
	virtual void negotiated(const Config &cfg) { (void)cfg; }
	/* one line of debug output (without the modem's name) */
	virtual void log(const char *line) { (void)line; }
protected:
	~Owner() = default;
};

class Link {
public:
	Link() = default;
	~Link();
	Link(const Link &) = delete;
	Link &operator=(const Link &) = delete;

	/* a call: allocate what cfg needs (V.42bis dictionaries); -1 on
	   failure */
	int init(Owner *owner, bool caller, const Config &cfg);
	/* the end of the call */
	void exit();

	/* the data pump has connected: on, error correction as asked
	   (Config), the detection phase (detect) and LAPM or MNP, else
	   async */
	void start(bool on, bool detect);
	/* we hang up: LAPM sends DISC, MNP LD */
	void disconnect();
	/* the data pump is not there (any more): marks out, nothing in */
	void stop();
	/* characters flow only while online (the DTE is in data mode) */
	void set_online(bool on) { online_ = on; }

	/* the data pump's bits: nbits bits in each byte of buf */
	int get_bits(int nbits, uint8_t *buf, int n);
	int put_bits(int nbits, const uint8_t *buf, int n);

	/* what was asked for, then what XID (or MNP's LRs) agreed */
	const Config &config() const { return cfg_; }
	/* what was asked for */
	const Config &asked() const { return asked_; }
	/* LAPM rather than async or MNP (once detected) */
	bool ec() const { return cfg_.ec; }
	/* the error correction (once detected) */
	Protocol protocol() const { return proto_; }
	/* LAPM's or MNP's frames not sent yet or not acknowledged */
	bool tx_pending() const {
		return (mode_ == Mode::Lapm && lapm_.tx_pending()) ||
			(mode_ == Mode::Mnp && mnp_.tx_pending());
	}
	/* the data pump's transmit rate (bit/s), 0 if not known */
	int tx_rate() const { return owner_->tx_rate(); }
	/* the call's counts so far (kept after it, until the next init()),
	   which the framers add to */
	const Stats &stats() const { return stats_; }
	/* (see the const one) */
	Stats &stats() { return stats_; }

	/* for the parts */
	void logf(const char *fmt, ...) DMODEM_PRINTF(2, 3);
	/* the link's progress to the owner */
	void status(Status s);
	/* what the bit timer runs when it fires */
	enum class Timer {
		None,
		DetectBegin,    /* the originator's ODPs (Detector::begin) */
		DetectEnd,      /* the detection phase's end (Detector::finish) */
		T401,           /* LAPM's T401 (Lapm::t401_timeout) */
		Negotiate,      /* the caller's XID (Lapm::negotiate) */
	};
	/* the bit timer: what once bits more bits went out (0: none) */
	void set_timer(long bits, Timer what) {
		timer_ = bits;
		timer_what_ = what;
	}
	/* the bits left before it fires, 0 if it does not run */
	long timer() const { return timer_; }
	/* what it runs (or last ran) */
	Timer timer_what() const { return timer_what_; }
	/* LAPM's frames on the line */
	Hdlc &hdlc() { return hdlc_; }
	/* the detection phase is over: LAPM (ec), else MNP (the originator,
	   if asked and mnp) or async */
	void detected(bool ec, bool mnp = true);
	/* the detection phase (the answerer) saw an MNP LR start: MNP's
	   framer takes the rest of it, rx */
	void mnp_lr_seen(const BitQueue &rx);
	/* MNP's establishment heard nothing: async, as without */
	void mnp_fallback() { no_ec(); }
	/* we are the originator */
	bool caller() const { return caller_; }
	/* the parameters, for XID */
	Config &cfg() { return cfg_; }
	/* characters between the DTE and LAPM or the async framer,
	   through V.42bis when it is on in that direction */
	int read_chars(uint8_t *buf, int n);
	int write_chars(const uint8_t *buf, int n);
	/* LAPM's XID: what the remote wants (Config's V.42bis part) */
	void update_config(const Config &remote);
	/* what MNP's LRs agreed (cfg()) to the owner */
	void negotiated() { owner_->negotiated(cfg_); }
	/* LAPM keeps characters the DTE did not take; push them on */
	bool push_pending = false;

private:
	void end_detect();
	void start_mnp(bool initiator, const BitQueue *rx);
	void no_ec();
	enum class Mode { Idle, Detect, Lapm, Mnp, Async };
	void run_timer();
	int comp_read(uint8_t *buf, int n);
	int comp_write(const uint8_t *buf, int n);
	bool comp_flush_output();
	template <class E> int comp_read_with(E &enc, uint8_t *buf, int n);
	template <class D> int comp_write_with(D &dec, const uint8_t *buf,
					       int n);
	template <class D> bool comp_flush_with(D &dec);
	int dte_read(uint8_t *buf, int n);
	int dte_write(const uint8_t *buf, int n);

	Owner *owner_ = nullptr;
	bool caller_ = false;
	bool online_ = false;
	Stats stats_;
	Config cfg_, asked_;
	Mode mode_ = Mode::Idle;
	Protocol proto_ = Protocol::None;
	bool mnp_tried_ = false;
	long timer_ = 0;
	Timer timer_what_ = Timer::None;

	Detector det_;
	Hdlc hdlc_;
	Async async_;
	Lapm lapm_;
	MnpAsync mnp_async_;
	Mnp mnp_;
	V42bis enc_, dec_;
	V44 enc44_, dec44_;
	Mnp5 enc5_, dec5_;
	/* decoded characters the DTE has not taken yet */
	uint8_t dec_out_[256];
	int dec_head_ = 0, dec_count_ = 0;
};

} // namespace dlink

#endif
