/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    rtp_events.cpp - the far end's telephone-events as the tones they
 *                     stand for (rtp_events.h)
 *
 */
#include "rtp_events.h"

#include <cmath>

namespace dmd {

namespace {

/* what an event is: its tones (Hz, 0: none), ANSam's modulation, the
   phase reversals, its nominal power (dBm0, for a volume of 0) */
struct Def {
	int event;
	const char *name;
	EventPlayer::Kind kind;
	double f1, f2;
	bool mod, rev;
	int dbm0;
};

const Def DEFS[] = {
	/* RFC 4733: 0 to 15 are the DTMF digits (below) */
	{ 16, "hook flash", EventPlayer::FLASH, 0, 0, false, false, 0 },
	/* RFC 4734: V.8bis segments (Table 1) */
	{ 23, "CRdSeg", EventPlayer::PLAYED, 1900, 0, false, false, 13 },
	{ 24, "CReSeg", EventPlayer::PLAYED, 400, 0, false, false, 13 },
	{ 25, "MRdSeg", EventPlayer::PLAYED, 1150, 0, false, false, 13 },
	{ 26, "MReSeg", EventPlayer::PLAYED, 650, 0, false, false, 13 },
	{ 28, "V8bISeg", EventPlayer::PLAYED, 1375, 2002, false, false, 13 },
	{ 29, "V8bRSeg", EventPlayer::PLAYED, 1529, 2225, false, false, 13 },
	/* V.25 and V.8 (Tables 2 to 4) */
	{ 32, "ANS", EventPlayer::PLAYED, 2100, 0, false, false, 12 },
	{ 33, "/ANS", EventPlayer::PLAYED, 2100, 0, false, true, 12 },
	{ 34, "ANSam", EventPlayer::PLAYED, 2100, 0, true, false, 12 },
	{ 35, "/ANSam", EventPlayer::PLAYED, 2100, 0, true, true, 12 },
	{ 36, "CNG", EventPlayer::PLAYED, 1100, 0, false, false, 13 },
	{ 37, "V.21 channel 1 '0'", EventPlayer::PLAYED, 1180, 0, false,
	  false, 13 },
	{ 38, "V.21 channel 1 '1'", EventPlayer::PLAYED, 980, 0, false,
	  false, 13 },
	{ 39, "V.21 channel 2 '0'", EventPlayer::PLAYED, 1850, 0, false,
	  false, 13 },
	{ 40, "V.21 channel 2 '1'", EventPlayer::PLAYED, 1650, 0, false,
	  false, 13 },
	{ 49, "CT", EventPlayer::PLAYED, 1300, 0, false, false, 13 },
	{ 52, "ANS2225", EventPlayer::PLAYED, 2225, 0, false, false, 12 },
	/* modulated signals: the audio carries them */
	{ 27, "V.32 AC", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 30, "V21L300", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 31, "V21H300", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 53, "CI", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 54, "V.21 preamble flag", EventPlayer::NOT_PLAYED, 0, 0, false,
	  false, 0 },
	{ 55, "V21L110", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 56, "B103L300", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 57, "V23Main", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 58, "V23Back", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 59, "Baud4545", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 60, "Baud50", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 61, "VBDGen", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 62, "XCIMark", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
	{ 63, "V.32 AA", EventPlayer::NOT_PLAYED, 0, 0, false, false, 0 },
};

const double DTMF_LO[] = { 697, 770, 852, 941 };
const double DTMF_HI[] = { 1209, 1336, 1477, 1633 };
/* the key's row and column in RFC 4733's order 0-9 * # A-D */
const int ROW[] = { 3, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 0, 1, 2, 3 };
const int COL[] = { 1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 2, 3, 3, 3, 3 };
const char *const DIGIT_NAMES[] = {
	"DTMF 0", "DTMF 1", "DTMF 2", "DTMF 3", "DTMF 4", "DTMF 5",
	"DTMF 6", "DTMF 7", "DTMF 8", "DTMF 9", "DTMF *", "DTMF #",
	"DTMF A", "DTMF B", "DTMF C", "DTMF D",
};

/* a sine's peak (16 bit linear) at dbm0: 0 dBm0 is 3.14 dB below the
   G.711 laws' full scale */
double peak(double dbm0)
{
	return 22764.0 * pow(10.0, dbm0 / 20.0);
}

/* the time a tone plays on without a packet of it (ms): a digit not
   long (two like digits stay two), a tone through late packets */
const long HOLD_DIGIT_MS = 100;
const long HOLD_TONE_MS = 300;

} // namespace

/* the oscillator's sample, its phase on by inc */
double EventPlayer::Osc::next()
{
	double s = sin(2 * M_PI * phase);
	phase += inc;
	phase -= floor(phase);
	return s;
}

/* a player at rate, nothing playing (its AM at 15 Hz) */
EventPlayer::EventPlayer(unsigned rate) : rate_(rate)
{
	am_.inc = 15.0 / rate;
}

/* see rtp_events.h */
void EventPlayer::reset()
{
	*this = EventPlayer(rate_);
}

/* see rtp_events.h */
EventPlayer::Began EventPlayer::packet(const uint8_t *p, size_t n,
				       uint32_t ts)
{
	Began b = { NONE, -1, nullptr };
	if (n < 4)
		return b;
	int event = p[0];
	bool end = p[1] & 0x80;
	int volume = p[1] & 0x3f;

	const Def *d = nullptr;
	if (event >= 16) {
		for (const Def &x : DEFS)
			if (x.event == event)
				d = &x;
	}
	/* an event not played: its start told once, and a tone that plays
	   goes on as it was */
	if (event >= 16 && (!d || d->kind != PLAYED)) {
		if (other_ == event && other_ts_ == ts)
			return b;
		other_ = event;
		other_ts_ = ts;
		b.kind = d ? d->kind : UNKNOWN;
		b.event = event;
		b.name = d ? d->name : "event";
		return b;
	}
	if (known_ && ts == ts_) {
		/* the event going on (again, if its packets were late), or
		   its end (again) */
		if (ended_)
			return b;
		left_ = hold_;
		on_ = !end;
		ended_ = end;
		return b;
	}
	/* a new digit or tone */
	known_ = true;
	ts_ = ts;
	ended_ = end;
	b.event = event;

	double f1 = 0, f2 = 0;
	bool mod = false, rev = false;
	int dbm0 = 10;
	if (event < 16) {
		b.kind = PLAYED;
		b.name = DIGIT_NAMES[event];
		f1 = DTMF_LO[ROW[event]];
		f2 = DTMF_HI[COL[event]];
		hold_ = HOLD_DIGIT_MS * static_cast<long>(rate_) / 1000;
	} else {
		b.kind = PLAYED;
		b.name = d->name;
		f1 = d->f1;
		f2 = d->f2;
		mod = d->mod;
		rev = d->rev;
		dbm0 = d->dbm0;
		hold_ = HOLD_TONE_MS * static_cast<long>(rate_) / 1000;
	}
	if (volume)
		dbm0 = volume;
	/* two tones share the power; a tone of the same frequency as the
	   one before goes on in its phase (reversed now if this is the
	   form after a reversal) */
	double a = peak(-static_cast<double>(dbm0) - (f2 > 0 ? 3.0103 : 0.0));
	if (f1 != f1_) {
		o1_ = Osc();
		am_.phase = 0;
	}
	o1_.inc = f1 / rate_;
	o2_ = Osc();
	o2_.inc = f2 / rate_;
	f1_ = f1;
	a1_ = a;
	a2_ = f2 > 0 ? a : 0;
	mod_ = mod;
	if (rev && !rev_) {
		o1_.phase += 0.5;
		o1_.phase -= floor(o1_.phase);
	}
	rev_ = rev;
	rev_count_ = 0;
	left_ = hold_;
	on_ = !end;
	return b;
}

/* see rtp_events.h */
bool EventPlayer::fill(int16_t *x, unsigned n)
{
	if (!on_)
		return false;
	const long rev_every = 450L * rate_ / 1000;
	for (unsigned i = 0; i < n; i++) {
		double y = a1_ * o1_.next() + a2_ * o2_.next();
		if (mod_)
			y *= 1.0 + 0.2 * am_.next();
		if (rev_ && ++rev_count_ >= rev_every) {
			o1_.phase += 0.5;
			o1_.phase -= floor(o1_.phase);
			rev_count_ = 0;
		}
		x[i] = static_cast<int16_t>(y > 32767  ? 32767 :
					    y < -32768 ? -32768 :
							 y);
	}
	left_ -= n;
	if (left_ <= 0)
		on_ = false;
	return true;
}

} // namespace dmd
