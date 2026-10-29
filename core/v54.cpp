/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v54.cpp - V.54's loop tests and V.52's test pattern (v54.h)
 */
#include "v54.h"

namespace dcore {
namespace v54 {

bool Recogniser::bit(u8 b)
{
	u8 d = d_.bit(b);
	if (seen_)
		return true;
	ok_ += d == v_;
	run_ = b == last_ ? run_ + 1 : 1;
	last_ = b;
	if (b != v_ && run_ >= 7)
		run_seen_ = true;
	if (++n_ < 256)
		return false;
	if (ok_ >= 240 && run_seen_) {
		if (++blocks_ >= need_)
			seen_ = true;
	} else {
		blocks_ = 0;
	}
	n_ = ok_ = 0;
	run_seen_ = false;
	return seen_;
}

void Recogniser::reset()
{
	d_ = Descrambler();
	n_ = ok_ = run_ = blocks_ = 0;
	last_ = 2;
	run_seen_ = seen_ = false;
}

void Checker::bit(u8 b)
{
	if (!locked_) {
		u8 p = ((hist_ >> 8) ^ (hist_ >> 4)) & 1;
		hist_ = static_cast<u16>(((hist_ << 1) | b) & 0x1ff);
		good_ = p == b ? good_ + 1 : 0;
		if (good_ >= 32) {
			locked_ = true;
			gen_.s = hist_;
			win_ = win_err_ = 0;
		}
		return;
	}
	u8 e = gen_.bit() != b;
	bits_++;
	errors_ += e;
	win_err_ += e;
	if (++win_ < 256)
		return;
	if (win_err_ >= 64) {
		/* a slip: the pattern again */
		locked_ = false;
		good_ = 0;
	}
	win_ = win_err_ = 0;
}

void LoopTest::reset()
{
	bool g = grant_;
	*this = LoopTest();
	grant_ = g;
}

void LoopTest::remote(bool self_test)
{
	self_ = self_test;
	scr_ = Scrambler();
	gen_ = Prbs9();
	check_.reset();
	ack_.reset();
	set(Phase::Prep, PREP_BITS);
}

void LoopTest::local2()
{
	asked_ = false;
	loop_.clear();
	set(Phase::Loop);
}

void LoopTest::local3(bool self_test)
{
	self_ = self_test;
	gen_ = Prbs9();
	check_.reset();
	set(Phase::Local3);
}

void LoopTest::end()
{
	switch (phase_) {
	case Phase::Prep:
	case Phase::WaitAck:
	case Phase::Settle:
	case Phase::Test:
		scr_ = Scrambler();
		set(Phase::Term, TERM_BITS);
		break;
	case Phase::Loop:
	case Phase::Local3:
		if (phase_ == Phase::Loop && asked_)
			break;  /* (the far end's: its TERM ends it) */
		loop_.clear();
		set(Phase::Idle);
		break;
	default:
		break;
	}
}

bool LoopTest::link_tx() const
{
	switch (phase_) {
	case Phase::Idle:
		return true;
	case Phase::Test:
	case Phase::Local3:
		return !self_;
	default:
		return false;
	}
}

bool LoopTest::link_rx() const
{
	return link_tx();
}

u8 LoopTest::tx(u8 link_bit)
{
	u8 o;

	switch (phase_) {
	case Phase::Idle:
		return link_bit;
	case Phase::Prep:
		o = scr_.bit(0);
		if (--left_ <= 0)
			set(Phase::WaitAck, ACK_WAIT_BITS);
		return o;
	case Phase::WaitAck:
		if (--left_ <= 0) {
			set(Phase::Idle);
			event_ = Event::NoAck;
		}
		return 1;
	case Phase::Settle:
		if (--left_ <= 0) {
			set(Phase::Test);
			check_.reset();
			event_ = Event::Started;
		}
		return 1;
	case Phase::Test:
	case Phase::Local3:
		return self_ ? gen_.bit() : link_bit;
	case Phase::Term:
		o = scr_.bit(1);
		if (--left_ <= 0)
			set(Phase::TermOnes, TERM_ONES);
		return o;
	case Phase::TermOnes:
		if (--left_ <= 0) {
			set(Phase::Idle);
			event_ = Event::Ended;
		}
		return 1;
	case Phase::Ack:
		o = scr_.bit(1);
		if (--left_ <= 0) {
			loop_.clear();
			term_.reset();
			ones_ = 0;
			set(Phase::Loop);
		}
		return o;
	case Phase::Loop:
		if (loop_.empty())
			return 1;
		o = loop_.front();
		loop_.pop_front();
		return o;
	}
	return 1;
}

void LoopTest::rx(u8 b)
{
	b &= 1;
	switch (phase_) {
	case Phase::Idle:
		if (grant_ && plain_ && prep_.bit(b)) {
			prep_.reset();
			asked_ = true;
			scr_ = Scrambler();
			set(Phase::Ack, ACK_BITS);
			event_ = Event::Looping;
		}
		break;
	case Phase::WaitAck:
		if (ack_.bit(b))
			set(Phase::Settle, SETTLE_BITS);
		break;
	case Phase::Test:
	case Phase::Local3:
		if (self_)
			check_.bit(b);
		break;
	case Phase::Loop:
		if (loop_.size() < LOOP_MAX)
			loop_.push_back(b);
		if (!asked_)
			break;
		if (!term_.seen()) {
			term_.bit(b);
			break;
		}
		ones_ = b ? ones_ + 1 : 0;
		if (ones_ >= 32) {
			loop_.clear();
			prep_.reset();
			set(Phase::Idle);
			event_ = Event::Unlooped;
		}
		break;
	default:
		break;
	}
}

}  // namespace v54
}  // namespace dcore
