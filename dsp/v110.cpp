/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v110.cpp - V.110's frames, its user rates' fill (RA1), and frame
 *               synchronisation; see v110.h
 *
 */

#include <cstring>

#include "dm_types.h"
#include "v110.h"

namespace dsp {

/* E1-E3 (Table 5/V.110), E1 in bit 0 */
static const u8 E_600 = 0x1, E_1200 = 0x2, E_2400 = 0x3, E_NX4800 = 0x6,
	E_NX3600 = 0x5, E_NX12000 = 0x4;

static const V110Rate RATES[] = {
	{ 300, 8000, E_600, V110Fill::Repeat, 16, 3 },
	{ 600, 8000, E_600, V110Fill::Repeat, 8, 6 },
	{ 1200, 8000, E_1200, V110Fill::Repeat, 4, 12 },
	{ 2400, 8000, E_2400, V110Fill::Repeat, 2, 24 },
	{ 4800, 8000, E_NX4800, V110Fill::Repeat, 1, 48 },
	{ 7200, 16000, E_NX3600, V110Fill::Nx3600, 1, 36 },
	{ 9600, 16000, E_NX4800, V110Fill::Repeat, 1, 48 },
	{ 12000, 32000, E_NX12000, V110Fill::Nx12000, 1, 30 },
	{ 14400, 32000, E_NX3600, V110Fill::Nx3600, 1, 36 },
	{ 19200, 32000, E_NX4800, V110Fill::Repeat, 1, 48 },
	{ 24000, 64000, E_NX12000, V110Fill::Nx12000, 1, 30 },
	{ 28800, 64000, E_NX3600, V110Fill::Nx3600, 1, 36 },
	{ 38400, 64000, E_NX4800, V110Fill::Repeat, 1, 48 },
	{ 56000, 64000, 0, V110Fill::Octet56, 1, 0 },
};

/* which of the 48 D bits carry the user's (1) and which are fill (0):
   runs of user bits and fill alternately, starting with the user's */
static const s16 NX3600_RUNS[] = { 10, 2, 2, 2, 2, 2, 14, 2, 2, 2, 2, 2, 4 };
static const s16 NX12000_RUNS[] = { 10, 2, 2, 2, 2, 2, 1, 3,
				    10, 2, 2, 2, 2, 2, 1, 3 };

/* the 48 D bits' use from runs (n of them) */
static void runs_mask(u8 mask[48], const s16 *runs, s16 n)
{
	s16 at = 0;
	for (s16 i = 0; i < n; i++)
		for (s16 k = 0; k < runs[i] && at < 48; k++)
			mask[at++] = (i & 1) ? 0 : 1;
}

/* the D bits' use for a fill (all of them for Repeat) */
static const u8 *fill_mask(V110Fill fill)
{
	static u8 all[48], nx3600[48], nx12000[48];
	static bool made;
	if (!made) {
		memset(all, 1, sizeof(all));
		runs_mask(nx3600, NX3600_RUNS,
			  sizeof(NX3600_RUNS) / sizeof(NX3600_RUNS[0]));
		runs_mask(nx12000, NX12000_RUNS,
			  sizeof(NX12000_RUNS) / sizeof(NX12000_RUNS[0]));
		made = true;
	}
	return fill == V110Fill::Nx3600 ? nx3600 :
		fill == V110Fill::Nx12000 ? nx12000 : all;
}

/* see v110.h */
const V110Rate *v110_rate(s32 user)
{
	for (const V110Rate &r : RATES)
		if (r.user == user)
			return &r;
	return nullptr;
}

/* see v110.h */
const V110Rate *v110_rate_found(s32 ir, u8 e123)
{
	for (const V110Rate &r : RATES)
		if (r.ir == ir && r.e123 == e123 && r.user != 300 &&
		    r.fill != V110Fill::Octet56)
			return &r;
	return nullptr;
}

/* the frame's octets 1 to 9, bits 1 to 7: D bits 6 to an octet but in
   octet 5 (E1-E7), the octet's last bit S or X */
void v110_frame_bits(u8 bits[80], const V110Frame &f)
{
	static const s16 SX[10] = { 0, 1, 2, 1, 1, 0, 1, 2, 1, 1 };
	memset(bits, 0, 8);
	s16 d = 0;
	for (s16 o = 1; o < 10; o++) {
		u8 *b = bits + o * 8;
		b[0] = 1;
		if (o == 5) {
			memcpy(b + 1, f.e, 7);
			continue;
		}
		memcpy(b + 1, f.d + d, 6);
		d = static_cast<s16>(d + 6);
		b[7] = SX[o] == 2 ? f.x : f.s;
	}
}

/* see v110.h */
void V110Adapt::to_frame(V110Frame &f, const u8 *user) const
{
	const u8 *mask = fill_mask(rate_->fill);
	s16 u = 0;
	for (s16 i = 0; i < 48; i++) {
		if (rate_->fill == V110Fill::Repeat)
			f.d[i] = user[i / rate_->repeat];
		else
			f.d[i] = mask[i] ? user[u++] : 1;
	}
	for (s16 i = 0; i < 3; i++)
		f.e[i] = (rate_->e123 >> i) & 1;
	for (s16 i = 3; i < 7; i++)
		f.e[i] = 1;
}

/* see v110.h */
void V110Adapt::from_frame(u8 *user, const V110Frame &f) const
{
	const u8 *mask = fill_mask(rate_->fill);
	if (rate_->fill == V110Fill::Repeat) {
		for (s16 i = 0; i < rate_->user_bits; i++)
			user[i] = f.d[i * rate_->repeat];
		return;
	}
	s16 u = 0;
	for (s16 i = 0; i < 48; i++)
		if (mask[i])
			user[u++] = f.d[i];
}

/* hunting, nothing seen */
void V110Sync::init()
{
	memset(win_, 0, sizeof(win_));
	head_ = filled_ = 0;
	phase_ = -1;
	good_ = bad_ = 0;
	synced_ = false;
	frame_ = V110Frame();
}

/* the last 80 bits: eight zeros, then a one every eighth bit */
bool V110Sync::aligned() const
{
	for (s16 i = 0; i < 80; i++) {
		u8 b = win_[(head_ + i) % 80];
		if (i < 8 ? b != 0 : (i % 8 == 0 && b != 1))
			return false;
	}
	return true;
}

/* the last 80 bits' fields into frame_ */
void V110Sync::decode()
{
	u8 b[80];
	for (s16 i = 0; i < 80; i++)
		b[i] = win_[(head_ + i) % 80];
	s16 d = 0;
	u8 s = 0, x = 0;
	for (s16 o = 1; o < 10; o++) {
		const u8 *p = b + o * 8;
		if (o == 5) {
			memcpy(frame_.e, p + 1, 7);
			continue;
		}
		memcpy(frame_.d + d, p + 1, 6);
		d = static_cast<s16>(d + 6);
		if (o == 2 || o == 7)
			x |= p[7];
		else
			s |= p[7];
	}
	frame_.s = s;
	frame_.x = x;
}

/* see v110.h */
bool V110Sync::put(u8 bit)
{
	win_[head_] = bit & 1;
	head_ = static_cast<s16>((head_ + 1) % 80);
	if (filled_ < 80) {
		filled_++;
		if (filled_ < 80)
			return false;
	}
	if (phase_ < 0) {
		/* hunting: a pattern starts the count of good ones */
		if (aligned()) {
			phase_ = 0;
			good_ = 1;
			bad_ = 0;
		}
		return false;
	}
	if (++phase_ < 80)
		return false;
	phase_ = 0;
	if (aligned()) {
		bad_ = 0;
		if (!synced_ && ++good_ >= 3)
			synced_ = true;
	} else if (!synced_) {
		/* not three in a row: hunt again from here */
		phase_ = -1;
		good_ = 0;
		return false;
	} else if (++bad_ >= 3) {
		synced_ = false;
		phase_ = -1;
		good_ = bad_ = 0;
		return false;
	}
	if (!synced_)
		return false;
	decode();
	return true;
}

} // namespace dsp
