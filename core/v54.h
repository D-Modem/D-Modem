/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v54.h - V.54's loop tests and V.52's test pattern, at the bit level
 *            between the link and the data pump
 *
 *    The loops (V.54 2): loop 3, the local analogue loop (the pump's
 *    signal back into its own receiver: Modem::run()), and loop 2, the
 *    remote digital loop (the far end's DCE sends what it receives back
 *    to us).  Loop 2 is set up in band, with the simplified signalling of
 *    V.54 6 for point-to-point synchronous DCEs (its patterns scrambled
 *    with 1 + x^-4 + x^-7, sent at the data rate as if they came from the
 *    DTE):
 *
 *      PREP   DCE A: 2048 bits of scrambled 0s; asks for the loop
 *      ACK    DCE B: 1948 bits of scrambled 1s; B loops after it, and A
 *             starts its test 2148 bits after it recognises it
 *      TERM   DCE A: 8192 bits of scrambled 1s, then 64 1s; B leaves the
 *             loop during the 1s (or when the carrier goes for 1 s: the
 *             call ends here)
 *
 *    The recognition criteria are left to the DCE (V.54 5.1): here a
 *    pattern is there in a 256-bit block when at least 240 of its bits
 *    descramble to the pattern's bit and the raw bits have a run of seven
 *    of the other value (the scrambled patterns have one in each 127
 *    bits; seven 1s also keep HDLC flags from passing for PREP, as 5.1
 *    asks), and recognised after enough blocks in a row: PREP 4 (1024
 *    bits), ACK 3 (768), TERM 16 (4096).
 *
 *    The self-test (V.52): the 511-bit pseudo-random pattern of
 *    x^9 + x^5 + 1 in place of the DTE's data, and a checker that
 *    synchronises on what comes back and counts the bits in error.
 */
#ifndef DMODEM_CORE_V54_H
#define DMODEM_CORE_V54_H

#include <deque>

#include <modem_defs.h>

namespace dcore {
namespace v54 {

/* the scrambler of V.54's patterns (Figure 3/V.54): 1 + x^-4 + x^-7 */
struct Scrambler {
	/* (not all 0s, where 0s stay 0s, nor all 1s, where 1s stay
	   1s) */
	u8 s = 0x55;
	/* the bit scrambled */
	u8 bit(u8 in)
	{
		u8 o = (in ^ (s >> 3) ^ (s >> 6)) & 1;
		s = static_cast<u8>(((s << 1) | o) & 0x7f);
		return o;
	}
};

/* its self-synchronising descrambler */
struct Descrambler {
	u8 s = 0;
	/* the bit descrambled */
	u8 bit(u8 in)
	{
		u8 o = (in ^ (s >> 3) ^ (s >> 6)) & 1;
		s = static_cast<u8>(((s << 1) | in) & 0x7f);
		return o;
	}
};

/* one of the patterns, recognised in the line's bits: scrambled v,
   blocks blocks of 256 bits in a row */
class Recogniser {
public:
	/* the pattern of scrambled v, after blocks blocks */
	Recogniser(u8 v, s16 blocks) : v_(v), need_(blocks) {}
	/* a bit from the line; whether the pattern is recognised (once
	   true, true until reset()) */
	bool bit(u8 b);
	void reset();
	/* (see bit()) */
	bool seen() const { return seen_; }

private:
	Descrambler d_;
	u8 v_;
	s16 need_;
	s16 n_ = 0, ok_ = 0, run_ = 0, blocks_ = 0;
	u8 last_ = 2;
	bool run_seen_ = false, seen_ = false;
};

/* V.52's 511-bit pattern, x^9 + x^5 + 1 */
struct Prbs9 {
	u16 s = 0x1ff;
	/* the pattern's next bit */
	u8 bit()
	{
		u8 o = ((s >> 8) ^ (s >> 4)) & 1;
		s = static_cast<u16>(((s << 1) | o) & 0x1ff);
		return o;
	}
};

/* what comes back of it: synchronised once 32 bits in a row follow
   from the 9 before them, then the bits that differ from the pattern
   counted (synchronised again after 64 errors in 256 bits) */
class Checker {
public:
	void bit(u8 b);
	/* to synchronise anew */
	void reset() { *this = Checker(); }
	/* synchronised on the pattern */
	bool locked() const { return locked_; }
	/* the bits in error since */
	u32 errors() const { return errors_; }
	/* the bits checked */
	u32 bits() const { return bits_; }

private:
	Prbs9 gen_;
	u16 hist_ = 0;
	s16 good_ = 0, win_ = 0, win_err_ = 0;
	bool locked_ = false;
	u32 errors_ = 0, bits_ = 0;
};

/* the loop tests of a call, between the link's bits and the pump's */
class LoopTest {
public:
	enum class Phase {
		Idle,
		/* DCE A, loop 2 (AT&T6, AT&T7) */
		Prep, WaitAck, Settle, Test, Term, TermOnes,
		/* DCE B, loop 2 (asked by A, or AT&T3) */
		Ack, Loop,
		/* loop 3 (AT&T1, AT&T8): the pump loops; with self_test,
		   the pattern in place of the link's bits */
		Local3,
	};
	/* what a test did, for the modem to act on (event()) */
	enum class Event {
		None,
		Started,        /* A: the test phase (CONNECT, or OK) */
		NoAck,          /* A: no ACK (ERROR) */
		Ended,          /* A: TERM sent (the result of AT&T0) */
		Looping,        /* B: the far end's loop 2 */
		Unlooped,       /* B: the loop is over */
	};

	/* the call starts or ends: no test */
	void reset();
	/* the far end may ask for loop 2 (AT&T4; AT&T5 refuses) */
	void set_grant(bool g) { grant_ = g; }
	/* (see set_grant()) */
	bool grant() const { return grant_; }
	/* the link has no error correction: tests run only then (loop 2's
	   signalling is in the data) */
	void set_plain(bool p) { plain_ = p; }

	/* AT&T6 (self_test false) and AT&T7: loop 2 asked for */
	void remote(bool self_test);
	/* AT&T3: loop 2 here, on the far end's data */
	void local2();
	/* AT&T1 and AT&T8: loop 3, the pump's own signal */
	void local3(bool self_test);
	/* AT&T0: A sends TERM; the other tests are over at once */
	void end();

	/* where the test is */
	Phase phase() const { return phase_; }
	/* a test runs */
	bool active() const { return phase_ != Phase::Idle; }
	/* V.52's pattern in place of the DTE's data */
	bool self_test() const { return self_; }
	/* the link has the bits (it sends and receives them): no test,
	   or a test that carries the DTE's data */
	bool link_tx() const;
	bool link_rx() const;
	/* the DTE's data goes nowhere (B loops) */
	bool dte_cut() const
	{
		return phase_ == Phase::Ack || phase_ == Phase::Loop;
	}

	/* a bit for the line: the link's (link_bit) or the test's */
	u8 tx(u8 link_bit);
	/* a bit from the line, seen by the test */
	void rx(u8 b);

	/* what happened since the last call */
	Event event()
	{
		Event e = event_;
		event_ = Event::None;
		return e;
	}
	/* the self-test's bit errors so far */
	u32 errors() const { return check_.errors(); }
	/* the checker */
	const Checker &checker() const { return check_; }

	/* the patterns' lengths (V.54 6), A's wait for ACK (bits after
	   its PREP) */
	enum {
		PREP_BITS = 2048, ACK_BITS = 1948, SETTLE_BITS = 2148,
		TERM_BITS = 8192, TERM_ONES = 64, ACK_WAIT_BITS = 6144,
		LOOP_MAX = 65536,
	};

private:
	/* the phase, for bits bits */
	void set(Phase p, s32 bits = 0)
	{
		phase_ = p;
		left_ = bits;
	}

	Phase phase_ = Phase::Idle;
	s32 left_ = 0;
	bool self_ = false, grant_ = false, plain_ = false;
	bool asked_ = false;    /* B: A asked (TERM ends it) */
	s16 ones_ = 0;
	Event event_ = Event::None;
	Scrambler scr_;
	Recogniser prep_{ 0, 4 }, ack_{ 1, 3 }, term_{ 1, 16 };
	Prbs9 gen_;
	Checker check_;
	std::deque<u8> loop_;
};

}  // namespace v54
}  // namespace dcore

#endif
