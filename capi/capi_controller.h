/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    capi_controller.h - a CAPI 2.0 controller on a D-Modem: one
 *                        controller (1) with one B-channel, its calls
 *                        made with the modem's AT commands on its DTE
 *
 *    The applications' messages come in by message(); the controller's
 *    go out through Out::to_app().  What it writes to the modem goes
 *    through Out::to_modem(), and the modem's output comes in by
 *    modem_input().  No clock of its own: each call gives the time in
 *    ms (now), and poll() runs what is due.
 *
 *    The modem is set up once (init): ATE0V1Q0X3 (BUSY, and no dial
 *    tone waited for), +CRC=1 for +CRING: SYNC (a clear channel's call,
 *    an ISDN data call) or VOICE, +VCID=1 for the caller's number, S0=0
 *    (the controller answers).  A call ends with +++ between guard
 *    times, then ATH.
 *
 *    A call's B protocol (CAPI's B1, B2, B3) in AT commands:
 *
 *      B1 0 (HDLC), B2 0 (X.75)          +MS=CLEAR +B2=0
 *      B1 0, B2 1 (transparent: frames)  +MS=CLEAR +B2=5, the frames
 *                                        in the DTE's RFC 1662 framing
 *                                        (sync PPP's), one a DATA_B3
 *      B1 0, B2 9 (V.120 async)          +MS=CLEAR +B2=9
 *      B1 2 (V.110 async), B2 1          +MS=CLEAR +CBST=<rate>,0 \N0
 *      B1 3 (V.110 sync), B2 0, 1, 9     +CBST=<rate>,1, B2 as above
 *      B1 7 (modem, negotiated), B2 7    the modem's own +MS, \N3
 *      B1 7 or 8 (modem), B2 1           the modem's own +MS, \N0
 *
 *    B3 is 0 (transparent), or 7 with B1 7.  The CIP value of an
 *    incoming call: 2 (unrestricted digital) for +CRING: SYNC, else
 *    the first of 4 (3.1 kHz audio), 16 (telephony) and 1 (speech) that
 *    the application listens for.
 *
 */

#ifndef DMODEM_CAPI_CONTROLLER_H
#define DMODEM_CAPI_CONTROLLER_H

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "capi_msg.h"

namespace capi {

/* the one PLCI and NCCI */
enum : u32 { CONTROLLER = 1, PLCI = 0x0101, NCCI = 0x00010101 };

/* the profile's B protocols (GET_PROFILE's bit masks) */
enum : u32 {
	PROFILE_B1 = 1u << 0 | 1u << 2 | 1u << 3 | 1u << 7 | 1u << 8,
	PROFILE_B2 = 1u << 0 | 1u << 1 | 1u << 7 | 1u << 9,
	PROFILE_B3 = 1u << 0 | 1u << 7,
};

/* the controller's profile (GET_PROFILE's 64 octets) */
std::vector<u8> profile();

/* what the B protocol is in AT commands (see the top) */
struct Mode {
	u16 info = NO_ERROR;    /* else B1_NOT_SUPPORTED and the like */
	bool clear = false;     /* a clear channel's call */
	std::string at;         /* the commands, without AT */
	bool frames = false;    /* the DTE's data in RFC 1662 frames */
};
/* the B protocol struct's octets (empty: CAPI's default, X.75) */
Mode mode_of(const std::vector<u8> &bprot);

class Controller {
public:
	/* where the controller's output goes */
	class Out {
	public:
		/* a message to its application (m.appl) */
		virtual void to_app(const Msg &m) = 0;
		/* octets to the modem: how many it took */
		virtual size_t to_modem(const u8 *p, size_t n) = 0;
	protected:
		~Out() = default;
	};

	/* init: the modem's setup commands after AT (see the top), more
	   after them */
	Controller(Out &out, const std::string &init_extra = "");

	/* an application registered (RCAPI's REGISTER): its data blocks
	   at most (maxBDataBlocks) and their length (maxBDataLen) */
	void add_app(u16 appl, u16 blocks, u16 len);
	/* the application gone: its call hung up */
	void remove_app(u16 appl, u64 now);
	/* a message of an application's */
	void message(const Msg &m, u64 now);
	/* the modem's output */
	void modem_input(const u8 *p, size_t n, u64 now);
	/* the controller takes more of the modem's output now (its
	   applications have taken what it had) */
	bool wants_modem_input() const;
	/* what is due: timers, octets the modem has not taken */
	void poll(u64 now);
	/* the ms until poll() has something to do, -1 none */
	s64 next_ms(u64 now) const;
	/* the modem is set up and answers commands */
	bool ready() const { return state_ != State::Init; }
	/* a call is on (dialled, ringing or connected) */
	bool busy() const { return call_ != Call::None; }

private:
	enum class State {
		Init,           /* the setup command sent */
		Query,          /* AT+MS? sent */
		Idle,
		Setup,          /* the call's mode commands sent */
		Dialling,       /* ATD sent */
		Answering,      /* ATA sent */
		Online,
		Escape,         /* +++ after its guard, then OK */
		Hangup,         /* ATH sent */
	};
	enum class Call {
		None,
		Out,            /* CONNECT_REQ's, dialling */
		Offered,        /* CONNECT_IND sent, no answer yet */
		Accepted,       /* CONNECT_RESP's accept, answering */
		Active,         /* connected */
		Clearing,       /* DISCONNECT_REQ's, hanging up */
	};
	struct App {
		u16 blocks = 7, len = 2048;
		bool listening = false;
		u32 cip_mask = 0;
	};
	struct Block {
		std::vector<u8> bytes;
		size_t done = 0;
		u16 appl = 0, num = 0, handle = 0;
		bool conf = false;      /* a DATA_B3_CONF when it is written */
	};

	void send(u16 appl, u8 cmd, u8 sub, u16 num,
		  const std::vector<u8> &params,
		  const std::vector<u8> &data = {});
	void conf(const Msg &m, u32 ident, u16 info);
	/* the number of a message of the controller's own */
	u16 next_num() { return num_++; }

	void connect_req(const Msg &m, u64 now);
	void connect_resp(const Msg &m, u64 now);
	void listen_req(const Msg &m);
	void data_b3_req(const Msg &m, u64 now);
	void facility_req(const Msg &m);

	void command(const std::string &line, State s, u64 now,
		     u32 timeout_ms);
	void result(const std::string &l, u64 now);
	void online_input(const u8 *p, size_t n, u64 now);
	void start_call(u64 now);
	void hangup(u64 now);
	void call_ended(u16 reason);
	void finish(u16 reason, u64 now);
	void connected();
	void ring(bool clear, u64 now);
	void offer(u64 now);
	void b3_active();
	void deliver(const u8 *p, size_t n);
	void push_rx();
	void push_tx();
	void drop_tx();

	Out &out_;
	std::string init_;
	std::map<u16, App> apps_;
	u16 num_ = 1;

	State state_ = State::Init;
	u64 deadline_ = 0;              /* the state's timeout, 0 none */
	std::string line_;              /* the modem's line so far */
	bool skip_lf_ = false;
	std::string ms_;                /* the modem's own +MS (Query) */
	int init_tries_ = 0;

	Call call_ = Call::None;
	u16 owner_ = 0;                 /* the call's application */
	Mode mode_;
	std::string number_;            /* to dial */
	u16 clear_reason_ = 0;          /* DISCONNECT_IND's when Clearing */
	/* the far end hung up: the data left to deliver until end_by_ */
	bool ending_ = false;
	u16 end_reason_ = 0;
	u64 end_by_ = 0;
	/* an incoming call: ringing, its bearer and caller, the
	   applications it went to, its last +CRING, when to offer it */
	bool ringing_ = false, ring_clear_ = false, ignored_ = false;
	std::string caller_;
	std::set<u16> offered_;
	u64 ring_last_ = 0, offer_at_ = 0;
	/* B3: on (CONNECT_B3_ACTIVE_IND sent) or coming */
	bool ncci_ = false, b3_up_ = false, b3_wait_ = false;
	/* to the application: the modem's data not yet sent, DATA_B3_INDs
	   it has not answered, the next handle; the tail that may be NO
	   CARRIER's start, and since when */
	std::deque<u8> rx_;
	std::deque<std::vector<u8>> rx_frames_;
	std::vector<u8> rx_frame_;      /* (frames: the one coming in) */
	bool rx_esc_ = false;
	std::set<u16> rx_out_;
	u16 rx_handle_ = 0;
	std::string held_;
	u64 held_at_ = 0;
	/* to the modem: the blocks of DATA_B3_REQs, each confirmed once it
	   is written; commands */
	std::deque<Block> tx_;
	u64 escape_at_ = 0;             /* +++ due (Escape), 0 sent */
};

} // namespace capi

#endif
