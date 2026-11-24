/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    e96_burst.h - the link rules of Hayes Express 96 above the
 *                  modulation: the half-duplex burst manager of the
 *                  V-series Smartmodem 9600 (firmware 1.4, bank 4 6260h)
 *                  and the bits of a burst on the line
 *
 *    Express 96 is half-duplex: one modem at a time sends a burst
 *    (docs/hayes-details.md):
 *
 *        marks (16 or more 1s)  flags  up to 7 frames  marks (7 or more)
 *
 *    The frames are the HDLC frames of LAP-B or LAP-M (flag 7Eh, zero
 *    insertion, FCS-16) with N401 128.  A burst with no frame (the
 *    "empty burst") has only flags.  The receiver hunts for 16
 *    consecutive 1s, then for a flag; 7 or more 1s after the frames end
 *    the remote's burst, and the line turns.  The budget of frames is 7,
 *    or 4 or 2 when the link reports errors.
 *
 *    E96Burst is the state machine of the firmware (the states 1 to 12
 *    of FA 6.6.5), driven by events and by time in milliseconds.  It
 *    tells an E96Port what to do.  E96Line is an E96Port that makes and
 *    receives the bits of the bursts with an Hdlc, for a loop-back test
 *    of the link layer.  The modulation of Express 96 is not known yet:
 *    E96Line's leading marks stand in for the training of its data pump.
 *
 */

#ifndef DMODEM_LINK_E96_BURST_H
#define DMODEM_LINK_E96_BURST_H


#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

/* the times of the burst manager: milliseconds, and the ticks of the
   long timer (probable: the unit of 1 ms is from the CPU clock, the tick
   of the long timer is not known) */
struct E96Times {
	s16 silence = 25;       /* 9FCEh: before this modem transmits */
	s16 rx_delay = 30;      /* 9FCFh: the line turned, before the
				   receiver is on */
	s16 null_tx = 4;        /* 9FD0h: the transmitter on in an empty
				   burst */
	s16 tx_off = 15;        /* 9FD1h: the transmitter off, before the
				   data pump is told to receive */
	s16 first_null = 35;    /* the empty bursts at the start (6555h) */
	s16 first_bursts = 5;   /* their number (9FD4h) */
	s16 tick_ms = 10;       /* the long timer's tick */
	s16 wait_line = 100;    /* ticks: for the remote's burst (6575h) */
	s16 retry = 260;        /* ticks: after a failure, then transmit */
	s16 rx_frames = 500;    /* ticks: for the end of the remote's
				   burst (6636h) */
	s16 max_failures = 8;   /* failures in a row: the link fails */

	/* the turnaround times for the connected rate (6362h): 4800 bit/s
	   and below 20, 15, 1, 12 ms; 9600 bit/s 25, 30, 4, 15 ms */
	static E96Times for_rate(s32 bps);
};

/* what the burst manager drives: the line functions of the firmware */
class E96Port {
public:
	/* the transmitter on or off (fn 26h, D bit 3) */
	virtual void e96_tx(bool on) = 0;
	/* the receiver on or off (D bit 0) */
	virtual void e96_rx(bool on) = 0;
	/* a frame waits to go (9FC9h) */
	virtual bool e96_frame_ready() = 0;
	/* send that frame */
	virtual void e96_send_frame() = 0;
	/* tell the data pump to receive (port 81h, mailbox 10h and 18h) */
	virtual void e96_pump_receive() = 0;
	/* the link failed (67DEh, message 91h) */
	virtual void e96_failed() = 0;
protected:
	~E96Port() = default;
};

class E96Burst {
public:
	/* the input events (FA 6.6.5); the line's are those of the receive
	   state machine (FA 6.5) */
	enum class Event {
		TurnTimer,      /* 0: the turnaround timer ended (78h) */
		LongTimer,      /* 1: the long timer ended (03h) */
		FrameSent,      /* 2: a frame is out (7Dh) */
		RxLine,         /* 4: the line turned to us, mark hunt (B 1) */
		RxMarks,        /* 5: 16 marks found (B 2) */
		RxFrames,       /* 6: flags and frames come (B 3) */
		RxEnd,          /* 7: 7 marks, the remote's burst ended (B 4) */
		FrameReceived,  /* 8: a good frame (7Ch) */
		TxIdle,         /* 10: the transmitter is idle (80h) */
	};
	/* the states, with the firmware's numbers */
	enum class State {
		Start = 1, WaitTx = 3, TxFrames = 4, TxDrain = 5,
		TxEmpty = 6, Turn = 7, WaitLine = 8, RxArm = 9, RxHunt = 10,
		RxFrames = 11, RxEnd = 12, Failed = 0,
	};

	/* a connection: the port, the times; the originator transmits
	   first, the answerer waits for its burst */
	void init(E96Port *port, const E96Times &t, bool first_tx);
	/* one event */
	void event(Event e);
	/* ms milliseconds more: the timers that end send their events */
	void tick(s32 ms);
	/* the error status of the link (70A9h): bit 0 a budget of 4 frames,
	   bit 1 of 2 */
	void set_errors(u8 bits) { errors_ = bits; }

	/* the state now */
	State state() const { return state_; }
	/* the frames this burst can still send */
	s16 budget() const { return budget_; }
	/* the bursts sent, and the empty ones among them */
	s32 bursts() const { return bursts_; }
	/* (see bursts()) */
	s32 empty_bursts() const { return empty_; }
	/* the frames received in the bursts that ended well */
	s32 frames_received() const { return rx_frames_; }
	/* the failures in a row */
	s16 failures() const { return failures_; }
	/* the time (ms) of the last good receive (9FD2h) */
	s32 last_good() const { return last_good_; }

private:
	void turn_timer(s32 ms);
	void long_timer(s32 ticks);
	void transmit();
	void failure();

	E96Port *port_ = nullptr;
	E96Times t_;
	State state_ = State::Start;
	/* the timers: milliseconds left, 0 when off */
	s32 turn_left_ = 0, long_left_ = 0;
	s32 now_ = 0, last_good_ = 0;
	s16 budget_ = 0;
	u8 errors_ = 0;
	s16 first_left_ = 0;    /* the first empty bursts still to go */
	s16 failures_ = 0;
	s32 bursts_ = 0, empty_ = 0;
	s32 rx_frames_ = 0, rx_burst_frames_ = 0;
};

/* the bits of the bursts for a loop-back test: one bit in each item, 0
   while the transmitter is off (no carrier) */
class E96Line : public E96Port, public Framer {
public:
	/* a connection over link: the frames of upper, the times, the
	   rate (bit/s), the leading marks (ms; the data pump's training) */
	void start(Link *link, Framer *upper, const E96Times &t,
		   bool caller, s32 bps, s16 lead_ms = 40);
	/* the milliseconds go on */
	void tick(s32 ms);
	/* the bits that this modem sends in the next n items */
	s32 get_bits(u8 *buf, s32 n);
	/* the bits of the line, n items */
	void put_bits(const u8 *buf, s32 n);
	/* the burst manager */
	E96Burst &burst() { return burst_; }
	/* the transmitter is on (the line is ours) */
	bool transmitting() const { return tx_ != Tx::Off; }

	/* the most frames in one of our bursts */
	s16 max_frames() const { return max_frames_; }
	/* the bursts whose trailing marks went out (7 or more 1s each) */
	s32 bursts_ended() const { return ended_; }
	/* the remote's bursts that ended with 7 or more 1s */
	s32 remote_ends() const { return remote_ends_; }
	/* the link failed */
	bool failed() const { return failed_; }

	/* (E96Port) the transmitter on or off: leading marks, or the end
	   of the burst and the trailing marks */
	void e96_tx(bool on) override;
	/* (E96Port) the receiver on (hunting for 16 marks) or off */
	void e96_rx(bool on) override;
	/* (E96Port) the upper framer has a frame (now held) */
	bool e96_frame_ready() override;
	/* (E96Port) the held frame goes at the Hdlc's next flag */
	void e96_send_frame() override;
	/* (E96Port) the line turns: RxLine once our burst is out */
	void e96_pump_receive() override;
	/* (E96Port) the link failed */
	void e96_failed() override { failed_ = true; }

	/* (Framer, for the Hdlc) the held frame once it may go */
	Frame *next_tx_frame() override;
	/* (Framer) a frame is out: to the upper framer, FrameSent */
	void tx_complete(Frame *f) override;
	/* (Framer) a good frame: to the upper framer, FrameReceived */
	void rx_complete(Frame *f) override;

private:
	enum class Tx { Off, Lead, Burst, Drain, Trail };
	enum class Rx { Off, Hunt, Flag, Frames };
	enum { TRAIL_BITS = 16, MARKS_START = 16, MARKS_END = 7 };

	Link *link_ = nullptr;
	Framer *upper_ = nullptr;
	Hdlc hdlc_;
	E96Burst burst_;
	s32 bps_ = 9600;
	s16 lead_ms_ = 40;
	Tx tx_ = Tx::Off;
	s32 tx_left_ = 0;
	u8 tx_shift_ = 0;
	bool drain_ = false;            /* off asked for in the lead */
	Frame *held_ = nullptr;         /* the frame e96_frame_ready() got */
	bool send_ = false;             /* the Hdlc may take it */
	s16 burst_frames_ = 0, max_frames_ = 0;
	s32 ended_ = 0;
	Rx rx_ = Rx::Off;
	s16 ones_ = 0;
	u8 rx_shift_ = 0;
	s32 remote_ends_ = 0;
	bool line_event_ = false;       /* RxLine to give at the next tick */
	bool failed_ = false;
};

} // namespace dlink

#endif
