/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v54.cpp - V.54's loop tests and V.52's test pattern (v54.h)
 */
#include "v54.h"

namespace dcore {
namespace v54 {

/* see v54.h */
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

/* (see v54.h) */
void Recogniser::reset()
{
	d_ = Descrambler();
	n_ = ok_ = run_ = blocks_ = 0;
	last_ = 2;
	run_seen_ = seen_ = false;
}

/* see v54.h */
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

/* see v54.h */
void LoopTest::reset()
{
	bool g = grant_;
	Ber b = ber_;
	*this = LoopTest();
	grant_ = g;
	ber_ = b;
	ber_.on = ber_.own_self = false;
}

/* see v54.h */
bool LoopTest::ber_start(u8 type, u32 block_len, u32 blocks)
{
	if (!loop_up())
		return false;
	ber_ = Ber();
	ber_.on = true;
	ber_.type = type;
	ber_.block_len = block_len;
	ber_.left = blocks;
	if (!self_) {
		self_ = true;
		ber_.own_self = true;
	}
	gen_ = Prbs9();
	check_.reset();
	return true;
}

/* see v54.h */
void LoopTest::ber_stop()
{
	if (ber_.own_self)
		self_ = false;
	ber_.on = ber_.own_self = false;
}

/* see v54.h */
void LoopTest::ber_count()
{
	u32 bits = check_.bits(), errors = check_.errors();
	for (; ber_.seen_bits < bits; ber_.seen_bits++) {
		if (errors > ber_.seen_errors) {
			/* (the errors of this bit, when one came) */
			ber_.bit_errors++;
			ber_.seen_errors++;
			ber_.block_bad = true;
		}
		if (++ber_.in_block < ber_.block_len)
			continue;
		ber_.block_errors += ber_.block_bad;
		ber_.in_block = 0;
		ber_.block_bad = false;
		if (--ber_.left == 0) {
			ber_stop();
			return;
		}
	}
}

/* see v54.h */
void LoopTest::remote(bool self_test)
{
	self_ = self_test;
	scr_ = Scrambler();
	gen_ = Prbs9();
	check_.reset();
	ack_.reset();
	set(Phase::Prep, PREP_BITS);
}

/* see v54.h */
void LoopTest::local2()
{
	asked_ = false;
	loop_.clear();
	set(Phase::Loop);
}

/* see v54.h */
void LoopTest::local3(bool self_test)
{
	self_ = self_test;
	gen_ = Prbs9();
	check_.reset();
	set(Phase::Local3);
}

/* see v54.h */
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

/* see v54.h */
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

/* see v54.h */
bool LoopTest::link_rx() const
{
	return link_tx();
}

/* see v54.h */
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

/* see v54.h */
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
		if (ber_.on)
			ber_count();
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

/* see v54.h */
bool self_check()
{
	/* the pattern back as it went: found, no errors */
	Prbs9 g;
	Checker c;
	for (int i = 0; i < 4096; i++)
		c.bit(g.bit());
	if (!c.locked() || c.errors() || c.bits() < 4000)
		return false;
	/* one bit in 256 wrong: still synchronised, each one counted */
	Checker e;
	Prbs9 h;
	u32 put = 0;
	for (int i = 0; i < 8192; i++) {
		u8 b = h.bit();
		if (e.locked() && i % 256 == 100) {
			b ^= 1;
			put++;
		}
		e.bit(b);
	}
	if (!e.locked() || e.errors() != put || !put)
		return false;
	/* scrambled 0s (PREP) recognised as such, not as 1s (ACK) */
	Scrambler s;
	Recogniser prep(0, 4), ack(1, 3);
	bool p = false, a = false;
	for (int i = 0; i < 2048; i++) {
		u8 b = s.bit(0);
		p = prep.bit(b);
		a = ack.bit(b);
	}
	return p && !a;
}

}  // namespace v54
}  // namespace dcore
