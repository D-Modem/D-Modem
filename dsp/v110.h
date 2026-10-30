/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v110.h - V.110 rate adaption on a 64 kbit/s channel (an ISDN
 *             B-channel): a user rate of 600 to 38400 bit/s in 80 bit
 *             frames at an intermediate rate of 8, 16, 32 or 64 kbit/s
 *             (RA1), that rate in the first 1, 2, 4 or 8 bits of each
 *             of the channel's octets (RA2); 56000 bit/s in 7 bits of
 *             each octet, with no frames
 *
 *    A frame (V.110 5.1.2.1): octet 0 all zeros, then nine octets that
 *    each start with a one (the alignment pattern), carrying 48 D bits
 *    (the user's), E1 to E7 (E1-E3 the frame's fill, which tells the
 *    user rate within its intermediate rate) and the status bits S and X
 *    (0 is ON).  The first bit of a channel octet is its most
 *    significant (I.460, RFC 4040).  User rates below 4800 repeat each
 *    bit (Table 6a-c), 7200, 14400 and 28800 (N x 3600) and 12000 and
 *    24000 (N x 12000) leave some D bits as fill (Table 6d, 6f).  300
 *    bit/s (asynchronous) rides on 600 with each bit twice.
 *
 */

#ifndef DMODEM_DSP_V110_H
#define DMODEM_DSP_V110_H


#include "dm_types.h"

namespace dsp {

/* how a user rate's bits fill a frame's 48 D bits */
enum class V110Fill : u8 {
	Repeat,         /* each bit `repeat` times (600 to 4800) */
	Nx3600,         /* 36 of them, Table 6d */
	Nx12000,        /* 30 of them, Table 6f */
	Octet56,        /* 56000: no frames, 7 bits an octet */
};

struct V110Rate {
	s32 user;               /* the rate the link layer sees */
	s32 ir;                 /* intermediate rate (8000 to 64000; 64000
				   for 56000, which has no frames) */
	u8 e123;                /* E1, E2, E3 in bits 0, 1, 2 */
	V110Fill fill;
	s16 repeat;             /* (Repeat) each D bit's copies; 300 bit/s
				   is 600's 8 with each user bit twice */
	s16 user_bits;          /* the user's bits in a frame */
};

/* the rate for a user rate, NULL when V.110 has none here */
const V110Rate *v110_rate(s32 user);
/* the rate whose frames come at intermediate rate ir with E1-E3 e123
   (300 bit/s is never found: 600's frames are 600); NULL for none */
const V110Rate *v110_rate_found(s32 ir, u8 e123);

/* one frame's fields */
struct V110Frame {
	u8 d[48];
	u8 e[7];                /* E1 .. E7 */
	u8 s;                   /* S1, S3, S4, S6, S8, S9 all ON (0): 0 */
	u8 x;                   /* X both ON: 0 */
};

/* the frame's 80 bits, the first transmitted first */
void v110_frame_bits(u8 bits[80], const V110Frame &f);

/* the user's bits in, a frame out, and back (Tables 6a-6f) */
class V110Adapt {
public:
	/* the user rate's adaption */
	void init(const V110Rate *r) { rate_ = r; }
	/* (see init()) */
	const V110Rate *rate() const { return rate_; }
	/* f's D and E1-E3 from rate()->user_bits bits of user (the rest of
	   E ones) */
	void to_frame(V110Frame &f, const u8 *user) const;
	/* the user's bits in f (rate()->user_bits of them, the first copy
	   of each repeated one) */
	void from_frame(u8 *user, const V110Frame &f) const;

private:
	const V110Rate *rate_ = nullptr;
};

/* frame synchronisation on an intermediate rate's bits (V.110 5.1.2.3,
   5.1.2.4): frames found after three good alignment patterns in a row,
   lost after three bad ones in a row */
class V110Sync {
public:
	void init();
	/* one bit: true when it ends a frame while in sync (frame()) */
	bool put(u8 bit);
	/* frames are found (three good alignment patterns, not yet three
	   bad) */
	bool synced() const { return synced_; }
	/* the last frame put() ended */
	const V110Frame &frame() const { return frame_; }

private:
	bool aligned() const;
	void decode();

	u8 win_[80] = {};       /* the last 80 bits, oldest at head_ */
	s16 head_ = 0;
	s16 filled_ = 0;
	s16 phase_ = -1;        /* bits since the last frame's end; -1:
				   hunting */
	s16 good_ = 0, bad_ = 0;
	bool synced_ = false;
	V110Frame frame_ = {};
};

/* the first k bits of an octet (k 1, 2, 4 or 8), the first transmitted
   in the most significant, the rest ones (RA2) */
inline u8 v110_ra2_octet(const u8 *bits, s16 k)
{
	u8 o = 0xff;
	for (s16 i = 0; i < k; i++)
		if (!bits[i])
			o = static_cast<u8>(o & ~(0x80 >> i));
	return o;
}

} // namespace dsp

#endif
