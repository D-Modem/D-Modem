/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    rtp_events.h - the telephone-events a call's far end sends (RFC 4733,
 *                   RFC 4734), played into the modem's audio as the tones
 *                   they stand for, as an analogue line would carry them
 *
 *    The DTMF digits (events 0 to 15) and RFC 4734's tones: the answer
 *    tones ANS and ANSam (32, 34) and their forms after a phase reversal
 *    (/ANS 33, /ANSam 35: the phase reversed at their start and every
 *    450 ms, ANSam amplitude-modulated at 15 Hz between 0.8 and 1.2),
 *    CNG (36), the V.21 tones (37 to 40, the V.8bis segments that share
 *    them), V.8bis's other segments (23 to 26, 28, 29), CT (49) and the
 *    2225 Hz answer tone (52).  Each at the power its volume field gives
 *    (dBm0; a nominal one if 0), from its first packet until its end, or
 *    until no packet of it has come for a while (a tone goes on through
 *    late packets, a digit not for long).  A tone that follows one of the
 *    same frequency goes on in its phase, so that /ANS after ANS is the
 *    one tone with its phase reversed.
 *
 *    The others are not played: the hook flash (16: no line signal of
 *    an analogue line stands for the far end's), and RFC 4734's events
 *    that tell of modulated signals (CI 53, the V.21 preamble 54, V.32's
 *    AA and AC 63 and 27, the modulations in use 30, 31, 55 to 61, V.18's
 *    XCIMark 62), whose signals the audio carries.  Nor any other event.
 */
#ifndef DMODEMD_RTP_EVENTS_H
#define DMODEMD_RTP_EVENTS_H

#include <cstddef>
#include <cstdint>

namespace dmd {

class EventPlayer {
public:
	enum Kind {
		NONE,           /* nothing new */
		PLAYED,         /* a digit or a tone, played */
		FLASH,          /* the far end's hook flash, not played */
		NOT_PLAYED,     /* an event of a signal the audio carries */
		UNKNOWN,        /* an event of no definition we have */
	};
	/* what a packet began: its kind, the event, a name for the log */
	struct Began {
		Kind kind;
		int event;
		const char *name;
	};

	explicit EventPlayer(uint32_t rate = 8000);

	/* a telephone-event packet's payload p (n octets) at timestamp
	   ts: an event's start (its kind), or NONE for its going on, its
	   end, a copy of its end packet, or a payload too short */
	Began packet(const uint8_t *p, size_t n, uint32_t ts);
	/* n samples: the tone over them while one plays (true), else x
	   untouched (false) */
	bool fill(int16_t *x, uint32_t n);
	/* a tone plays */
	bool playing() const { return on_; }
	/* the call ended: nothing plays, nothing is known */
	void reset();

private:
	struct Osc {
		double phase = 0, inc = 0;      /* cycles */
		double next();
	};

	uint32_t rate_;
	bool on_ = false, known_ = false;
	bool ended_ = false;            /* its end packet came */
	uint32_t ts_ = 0;               /* the played event's timestamp */
	/* the last event not played (its start told once), -1 if none */
	int other_ = -1;
	uint32_t other_ts_ = 0;
	int32_t left_ = 0;              /* samples it plays on without a
					   packet */
	int32_t hold_ = 0;              /* left_ again at each packet */
	Osc o1_, o2_, am_;
	double a1_ = 0, a2_ = 0;
	double f1_ = 0;
	bool mod_ = false;              /* ANSam's 15 Hz */
	bool rev_ = false;              /* phase reversals every 450 ms */
	int32_t rev_count_ = 0;
};

} // namespace dmd

#endif
