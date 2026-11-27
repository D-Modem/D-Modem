/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    e96_burst.cpp - the Express 96 burst manager and the bits of its
 *                    bursts (see e96_burst.h)
 *
 *    The handlers of the firmware (bank 4): the jump table 646Eh, the
 *    states 1 (64B5h), 3 (64E9h), 4 (651Fh), 5 (6545h), 6 (655Fh),
 *    7 (6575h), 8 (6597h), 9 (65EDh), 10 (6613h), 11 (6636h) and
 *    12 (6666h).  The retrain states 13 to 16 are not done.
 *
 */

#include <algorithm>

#include "e96_burst.h"
#include "link.h"

namespace dlink {

/* the flag of the frames */
static const u8 E96_FLAG = 0x7e;

/* (6362h: the four times from the rate in 8E2Bh) */
E96Times E96Times::for_rate(s32 bps)
{
	E96Times t;
	if (bps <= 4800) {
		t.silence = 20;
		t.rx_delay = 15;
		t.null_tx = 1;
		t.tx_off = 12;
	}
	return t;
}

/* a connection: the originator starts as state 1 does at the end of
   its long timer (to transmit); the answerer as state 7 does (it tells
   the pump to receive, and waits for the line) */
void E96Burst::init(E96Port *port, const E96Times &t, bool first_tx)
{
	port_ = port;
	t_ = t;
	turn_left_ = long_left_ = 0;
	now_ = last_good_ = 0;
	budget_ = 0;
	errors_ = 0;
	first_left_ = t.first_bursts;
	failures_ = 0;
	bursts_ = empty_ = 0;
	rx_frames_ = rx_burst_frames_ = 0;
	if (first_tx) {
		state_ = State::Start;
		event(Event::LongTimer);
		return;
	}
	port_->e96_pump_receive();
	long_timer(t_.wait_line);
	state_ = State::WaitLine;
}

/* the turnaround timer: its event after ms milliseconds (at least 1) */
void E96Burst::turn_timer(s32 ms)
{
	turn_left_ = std::max<s32>(ms, 1);
}

/* the long timer: its event after ticks of the long timer */
void E96Burst::long_timer(s32 ticks)
{
	long_left_ = std::max<s32>(ticks * t_.tick_ms, 1);
}

/* the time goes on, a millisecond at a time: the timers that end give
   their events */
void E96Burst::tick(s32 ms)
{
	for (s32 i = 0; i < ms; i++) {
		now_++;
		if (turn_left_ && --turn_left_ == 0)
			event(Event::TurnTimer);
		if (long_left_ && --long_left_ == 0)
			event(Event::LongTimer);
	}
}

/* the start of a burst (state 3, 64E9h; the budget 64FCh): the
   transmitter on, the budget 7 (4 or 2 with errors); the first bursts
   and a burst with no frame are empty bursts of flags (state 6) */
void E96Burst::transmit()
{
	bursts_++;
	port_->e96_tx(true);
	budget_ = errors_ & 2 ? 2 : errors_ & 1 ? 4 : 7;
	if (first_left_ > 0) {
		first_left_--;
		empty_++;
		turn_timer(t_.first_null);
		state_ = State::TxEmpty;
		return;
	}
	if (port_->e96_frame_ready()) {
		budget_--;
		port_->e96_send_frame();
		state_ = State::TxFrames;
		return;
	}
	empty_++;
	turn_timer(t_.null_tx);
	state_ = State::TxEmpty;
}

/* no good receive in time (6597h, 678Ch): a failure counted, the
   receiver off; too many in a row end the link, else a wait, then a
   new burst from state 1 */
void E96Burst::failure()
{
	failures_++;
	port_->e96_rx(false);
	turn_left_ = 0;
	if (failures_ > t_.max_failures) {
		state_ = State::Failed;
		long_left_ = 0;
		port_->e96_failed();
		return;
	}
	state_ = State::Start;
	long_timer(t_.retry);
}

/* one event in the state now (the jump table 646Eh); events that a
   state does not have are ignored */
void E96Burst::event(Event e)
{
	if (e == Event::FrameReceived) {
		rx_burst_frames_++;
		return;
	}
	switch (state_) {
	case State::Start:
		if (e == Event::LongTimer) {
			port_->e96_tx(false);
			port_->e96_rx(false);
			turn_timer(t_.silence);
			state_ = State::WaitTx;
		}
		break;
	case State::WaitTx:
		if (e == Event::TurnTimer)
			transmit();
		break;
	case State::TxFrames:
		if (e != Event::FrameSent)
			break;
		if (budget_ > 0 && port_->e96_frame_ready()) {
			budget_--;
			port_->e96_send_frame();
			break;
		}
		port_->e96_tx(false);
		state_ = State::TxDrain;
		break;
	case State::TxDrain:
		if (e == Event::TxIdle) {
			turn_timer(t_.tx_off);
			state_ = State::Turn;
		}
		break;
	case State::TxEmpty:
		if (e == Event::TurnTimer) {
			port_->e96_tx(false);
			turn_timer(t_.tx_off);
			state_ = State::Turn;
		}
		break;
	case State::Turn:
		if (e == Event::TurnTimer) {
			port_->e96_pump_receive();
			long_timer(t_.wait_line);
			state_ = State::WaitLine;
		}
		break;
	case State::WaitLine:
		if (e == Event::RxLine) {
			turn_timer(t_.rx_delay);
			state_ = State::RxArm;
		} else if (e == Event::LongTimer) {
			failure();
		}
		break;
	case State::RxArm:
		if (e == Event::TurnTimer) {
			port_->e96_rx(true);
			rx_burst_frames_ = 0;
			state_ = State::RxHunt;
		} else if (e == Event::LongTimer) {
			failure();
		}
		break;
	case State::RxHunt:
		if (e == Event::RxMarks) {
			last_good_ = now_;
			state_ = State::RxFrames;
		} else if (e == Event::LongTimer) {
			failure();
		}
		break;
	case State::RxFrames:
		if (e == Event::RxFrames) {
			long_timer(t_.rx_frames);
			state_ = State::RxEnd;
		} else if (e == Event::LongTimer) {
			failure();
		}
		break;
	case State::RxEnd:
		if (e == Event::RxEnd) {
			port_->e96_rx(false);
			long_left_ = 0;
			failures_ = 0;
			rx_frames_ += rx_burst_frames_;
			turn_timer(t_.silence);
			state_ = State::WaitTx;
		} else if (e == Event::LongTimer) {
			failure();
		}
		break;
	case State::Failed:
		break;
	}
}


/*
 *    the bits of the bursts
 *
 */

/* a connection: the Hdlc for the frames of upper (flags when it has
   none), the transmitter and the receiver off; then the burst
   manager */
void E96Line::start(Link *link, Framer *upper, const E96Times &t,
		    bool caller, s32 bps, s16 lead_ms)
{
	link_ = link;
	upper_ = upper;
	bps_ = bps;
	lead_ms_ = lead_ms;
	tx_ = Tx::Off;
	tx_left_ = 0;
	tx_shift_ = 0;
	drain_ = false;
	held_ = nullptr;
	send_ = false;
	burst_frames_ = max_frames_ = 0;
	ended_ = remote_ends_ = 0;
	rx_ = Rx::Off;
	ones_ = 0;
	rx_shift_ = 0;
	line_event_ = false;
	failed_ = false;
	hdlc_.start(link);
	hdlc_.set_framer(this, true);
	burst_.init(this, t, caller);
}

/* the milliseconds go on: the pump's report that the line turned (after
   e96_pump_receive(), once our own burst is all out), then the
   timers */
void E96Line::tick(s32 ms)
{
	for (s32 i = 0; i < ms; i++) {
		if (line_event_ && tx_ == Tx::Off) {
			line_event_ = false;
			burst_.event(E96Burst::Event::RxLine);
		}
		burst_.tick(1);
	}
}

/* the transmitter on: the leading marks (16 at least), then the Hdlc's
   flags and frames; off: the rest of the Hdlc's frame and its flag,
   then the trailing marks.  The leading marks stand in for the data
   pump's training, so they always go out whole: an empty burst is the
   leading marks, a flag and the trailing marks */
void E96Line::e96_tx(bool on)
{
	if (on) {
		tx_ = Tx::Lead;
		drain_ = false;
		tx_left_ = std::max<s32>(MARKS_START,
					 static_cast<s32>(lead_ms_) * bps_ /
					 1000);
		burst_frames_ = 0;
		return;
	}
	if (tx_ == Tx::Lead)
		drain_ = true;
	else if (tx_ == Tx::Burst)
		tx_ = Tx::Drain;
}

/* the receiver on, hunting for 16 marks; or off */
void E96Line::e96_rx(bool on)
{
	rx_ = on ? Rx::Hunt : Rx::Off;
	ones_ = 0;
}

/* the upper framer's next frame, held until it may go */
bool E96Line::e96_frame_ready()
{
	if (!held_)
		held_ = upper_->next_tx_frame();
	return held_ != nullptr;
}

/* the held frame goes at the Hdlc's next flag */
void E96Line::e96_send_frame()
{
	send_ = true;
}

/* the line turns to us: the event at the next millisecond (the data
   pump's CTS goes off) */
void E96Line::e96_pump_receive()
{
	line_event_ = true;
}

/* (the Hdlc) the held frame, once the burst manager sent it */
Frame *E96Line::next_tx_frame()
{
	if (!send_ || !held_)
		return nullptr;
	send_ = false;
	Frame *f = held_;
	held_ = nullptr;
	burst_frames_++;
	max_frames_ = std::max(max_frames_, burst_frames_);
	return f;
}

/* (the Hdlc) a frame is out: to the upper framer, then the event */
void E96Line::tx_complete(Frame *f)
{
	upper_->tx_complete(f);
	burst_.event(E96Burst::Event::FrameSent);
}

/* (the Hdlc) a good frame: to the upper framer, then the event */
void E96Line::rx_complete(Frame *f)
{
	upper_->rx_complete(f);
	burst_.event(E96Burst::Event::FrameReceived);
}

/* the next n bits on the line: 0 while off, 1 for the marks, the Hdlc's
   bits in the burst; the trailing marks end with the transmitter idle */
s32 E96Line::get_bits(u8 *buf, s32 n)
{
	for (s32 i = 0; i < n; i++) {
		u8 b = 0;
		switch (tx_) {
		case Tx::Off:
			break;
		case Tx::Lead:
			b = 1;
			if (--tx_left_ == 0)
				tx_ = drain_ ? Tx::Drain : Tx::Burst;
			break;
		case Tx::Burst:
		case Tx::Drain:
			hdlc_.get_bits(1, &b, 1);
			tx_shift_ = static_cast<u8>(tx_shift_ << 1 | b);
			if (tx_ == Tx::Drain && tx_shift_ == E96_FLAG) {
				tx_ = Tx::Trail;
				tx_left_ = TRAIL_BITS;
			}
			break;
		case Tx::Trail:
			b = 1;
			if (--tx_left_ == 0) {
				tx_ = Tx::Off;
				ended_++;
				burst_.event(E96Burst::Event::TxIdle);
			}
			break;
		}
		buf[i] = b;
	}
	return n;
}

/* n bits of the line while the receiver is on (a half-duplex pump
   does not hear the line while it transmits): 16 marks start the
   remote's burst, a flag its frames (to the Hdlc), 7 marks after them
   end it (the Hdlc gets more marks, so that it ends the last frame) */
void E96Line::put_bits(const u8 *buf, s32 n)
{
	static const u8 marks[24] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
				      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
	for (s32 i = 0; i < n; i++) {
		const u8 b = buf[i] & 1;
		if (rx_ == Rx::Off || tx_ != Tx::Off)
			continue;
		ones_ = b ? static_cast<s16>(ones_ + 1) : 0;
		rx_shift_ = static_cast<u8>(rx_shift_ << 1 | b);
		switch (rx_) {
		case Rx::Off:
			break;
		case Rx::Hunt:
			if (ones_ >= MARKS_START) {
				rx_ = Rx::Flag;
				burst_.event(E96Burst::Event::RxMarks);
			}
			break;
		case Rx::Flag:
			hdlc_.put_bits(1, &b, 1);
			if (rx_shift_ == E96_FLAG) {
				rx_ = Rx::Frames;
				burst_.event(E96Burst::Event::RxFrames);
			}
			break;
		case Rx::Frames:
			hdlc_.put_bits(1, &b, 1);
			if (ones_ >= MARKS_END) {
				hdlc_.put_bits(1, marks, sizeof(marks));
				remote_ends_++;
				rx_ = Rx::Off;
				burst_.event(E96Burst::Event::RxEnd);
			}
			break;
		}
	}
}

} // namespace dlink
