/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    q931.h - the user side of ISDN's call control (Q.931, ETSI's
 *             EN 300 403 for Euro-ISDN) on a basic rate interface's
 *             D-channel, for one call at a time: the modem's
 *
 *    No I/O of its own: messages in from layer 2 (receive()), out to
 *    the User's q931_send(), time given in ms.  Outgoing calls with
 *    the whole number in their SETUP (en bloc, sending complete), the
 *    B-channel the network's choice; incoming calls for our MSN (or
 *    any, with none), with a B-channel, of speech, 3.1 kHz audio or
 *    unrestricted digital information.  A-law in the bearer we send.
 *
 *    The states are Q.931's (U0 .. U19).  Timers: T303 (SETUP
 *    unanswered, sent twice), T305 (our DISCONNECT unanswered), T308
 *    (RELEASE, sent twice) and T313 (our CONNECT unacknowledged).  A
 *    message for a call we don't have gets RELEASE COMPLETE (cause 81),
 *    a STATUS ENQUIRY its STATUS, a RESTART its RESTART ACKNOWLEDGE.
 *
 */

#ifndef DMODEMD_Q931_H
#define DMODEMD_Q931_H

#include <string>

#include "dm_types.h"

namespace dmd {

class Q931 {
public:
	/* a call's bearer: what its SETUP's bearer capability says */
	enum class Bearer : u8 { Speech, Audio, Udi, Other };

	/* an incoming call */
	struct Call {
		Bearer bearer = Bearer::Other;
		std::string calling;    /* the caller's number ("" if none) */
		bool withheld = false;  /* its presentation restricted */
		std::string called;     /* the number called (our MSN) */
		s16 channel = 0;        /* its B-channel, 1 or 2 */
	};

	/* the call's world */
	class User {
	public:
		/* a message to send on the D-channel (an I frame) */
		virtual void q931_send(const u8 *msg, s16 n) = 0;
		/* B-channel b (1 or 2) is the call's */
		virtual void q931_channel(s16 b) = 0;
		/* a call for us is ringing (we sent ALERTING) */
		virtual void q931_offered(const Call &c) = 0;
		/* the call is up: our call answered (CONNECT), theirs taken
		   (CONNECT ACKNOWLEDGE) */
		virtual void q931_connected() = 0;
		/* the call has ended for a reason of the network's or the
		   far end's (not our hangup()): Q.931's cause value */
		virtual void q931_ended(s16 cause) = 0;
	protected:
		~User() = default;
	};

	/* Q.931's call states (the user side's) */
	enum State : u8 {
		U0 = 0,         /* null */
		U1 = 1,         /* call initiated (SETUP sent) */
		U2 = 2,         /* overlap sending (SETUP ACKNOWLEDGE) */
		U3 = 3,         /* outgoing call proceeding */
		U4 = 4,         /* call delivered (ALERTING) */
		U7 = 7,         /* call received (we sent ALERTING) */
		U8 = 8,         /* connect request (we sent CONNECT) */
		U10 = 10,       /* active */
		U11 = 11,       /* disconnect request (we sent DISCONNECT) */
		U19 = 19,       /* release request (we sent RELEASE) */
	};

	/* message types */
	enum : u8 {
		ALERTING = 0x01, CALL_PROCEEDING = 0x02, PROGRESS = 0x03,
		SETUP = 0x05, CONNECT = 0x07, SETUP_ACK = 0x0D,
		CONNECT_ACK = 0x0F, DISCONNECT = 0x45, RESTART = 0x46,
		RESTART_ACK = 0x4E, RELEASE = 0x4D, RELEASE_COMPLETE = 0x5A,
		STATUS_ENQUIRY = 0x75, INFORMATION = 0x7B, STATUS = 0x7D,
	};
	/* cause values */
	enum : u8 {
		NORMAL_CLEARING = 16, USER_BUSY = 17, NO_USER_RESPONDING = 18,
		NO_ANSWER = 19, CALL_REJECTED = 21, TEMPORARY_FAILURE = 41,
		INVALID_CALL_REF = 81, STATUS_ENQUIRY_RESPONSE = 30,
		TIMER_EXPIRY = 102,
	};
	/* the timers, ms */
	enum : s32 {
		T303 = 4000, T305 = 30000, T308 = 4000, T313 = 4000,
	};

	/* user's call control, idle */
	explicit Q931(User &user) : user_(user) {}

	/* incoming calls for msn alone ("": any; the called number must
	   end with it); our number in our SETUPs (calling party), if
	   given */
	void set_msn(const std::string &msn) { msn_ = msn; }

	/* the state */
	State state() const { return state_; }
	/* no call: none, or one being cleared */
	bool idle() const {
		return state_ == U0 || state_ == U11 || state_ == U19;
	}

	/* a call to number (its digits, * and #) with bearer b (Audio or
	   Udi; Speech goes as Audio): SETUP.  A call being cleared is
	   forgotten.  0, or -1 with a call up or ringing (or no digits) */
	s16 dial(const char *number, Bearer b, u64 now);
	/* the ringing call answered: CONNECT */
	void answer(u64 now);
	/* we hang up: DISCONNECT (nothing once the call is being
	   cleared) */
	void hangup(u64 now);

	/* a message from layer 2 */
	void receive(const u8 *msg, s16 n, u64 now);
	/* the timers due by now */
	void poll(u64 now);
	/* ms until the next timer, -1 for none */
	s32 next_timer_ms(u64 now) const;

	/* what a message's information elements say (receive()'s) */
	struct Info {
		bool bearer = false;
		Bearer bc = Bearer::Other;
		s16 cause = -1;
		s16 channel = -1;       /* 1, 2; 0 none, 3 any; -1 absent */
		bool calling = false;
		std::string calling_number;
		bool withheld = false;
		std::string called_number;
		s16 call_state = -1;    /* a STATUS's */
	};
	/* msg's elements (after its header at off); false if malformed */
	static bool parse(const u8 *msg, s16 n, s16 off, Info &info);

private:
	enum class Timer : u8 { None, T303, T305, T308, T313 };

	/* a message for our call: type t, then n octets of elements */
	void send(u8 t, const u8 *ies = nullptr, s16 n = 0);
	/* a message for another call reference (cr, flag) */
	void send_cr(u8 cr, bool flag, u8 t, const u8 *ies, s16 n);
	/* the cause element of c */
	static s16 cause_ie(u8 *p, u8 c);
	void timer(Timer t, s32 ms, u64 now);
	/* the network's word on a call of ours: the B-channel it gave */
	void channel(const Info &info);
	/* the call is over for the network's reason: told once */
	void ended(s16 cause);
	/* our RELEASE (U19, T308) */
	void release(u8 cause, u64 now);
	/* to U0, no timer */
	void null();
	/* a SETUP from the network */
	void setup(u8 cr, const Info &info);
	/* a message for a call reference that is not our call's */
	void stray(u8 cr, bool flag, u8 t);

	User &user_;
	std::string msn_;
	State state_ = U0;
	u8 cr_ = 0;             /* the call's reference */
	bool ours_ = false;     /* we made the call (the flag's 0 on ours) */
	u8 next_cr_ = 0;
	s16 channel_ = 0;       /* the call's B-channel, once known */
	bool told_ = false;     /* q931_ended() said (or our hangup()) */
	Timer timer_ = Timer::None;
	u64 due_ = 0;
	s16 tries_ = 0;
	u8 setup_[64];          /* our SETUP, for T303's second */
	s16 setup_n_ = 0;
};

} // namespace dmd

#endif
