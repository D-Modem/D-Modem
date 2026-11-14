/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    mnp10.cpp - MNP 10 and MNP 10EC (Microcom's, as the DeskPorte FAST
 *                firmware has it; docs/link.md): the LR's parameters,
 *                the LMI probes and the LTs' size, the rate shifts and
 *                the cellular mode's line reports and transmit level
 *
 *    Time goes in ticks of 12.5 ms (as the DeskPorte's 80 Hz), counted
 *    from the bits that go out at the pump's rate: none go while the
 *    pumps change, and the owner keeps that time itself.
 *
 */

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "dm_types.h"
#include "link.h"

namespace dlink {

namespace {

enum : s32 {
	TICKS_S = 80,                   /* ticks a second */
	PROBE_TICKS = 80,               /* the LMI probe's interval */
	FF_WAIT = 4 * TICKS_S,          /* the fall forward's answer */
	FB_WAIT = 1 * TICKS_S,          /* the fallback's, after its signals */
	RESYNC_WAIT = 25 * TICKS_S,     /* the responder's first frame after */
	RESYNC_STEP = TICKS_S / 2,
	T19_TICKS = 120 * TICKS_S,      /* :T19 */
	HOLDOFF_START = 30,             /* the fall forward's hold-off, s */
	HOLDOFF_MAX = 1920,
	HOLDOFF_REFUSE = 420,           /* a fall forward refused from here */
	POST_HOLD = 10,                 /* no fallback so soon after a shift */
	HALVE_AFTER = 3 * TICKS_S,      /* P halved on a resync after this */
	FB_COPIES = 6,                  /* the fallback request's */
	X_HIGH = 0x55,                  /* the fallback's error average */
	X_GOOD = 1,                     /* the fall forward's */
	EQM_GOOD = 0x12,                /* its smoothed EQM, at most */
	P_FULL = 0xff,                  /* the fall forward's packet size */
	MAX_FAILS = 4,                  /* changes failed in a row: the end */
};

/* LR parameters MNP 10 adds (and D-Modem's own) */
enum : u8 {
	PI_OPT = 8, PI_COMP = 9, PI_M10_OLD = 0x0a, PI_SECURITY = 0x0b,
	PI_M10 = 0x0c, PI_QUALITY = 0x0d, PI_V42BIS = 0x0e,
	PI_ORIG = 0x11, PI_ASYNC = 0x80, PI_RESUME = 0x82, PI_EXT = 0xc0,
};

/* parameter 8's bits */
enum : u8 {
	OPT_256 = 0x01, OPT_FIXED = 0x02, OPT_M10 = 0x10, OPT_EC = 0x20,
	OPT_V42 = 0x80, OPT_KNOWN = 0xb3,
};

/* 0x0C's feature byte */
enum : u8 {
	F_FALLBACK = 0x01, F_FALLFWD = 0x02, F_1200 = 0x04, F_2400 = 0x08,
	F_RSVD = 0x40, F_CELL = 0x80,
	F_ACTIVE = F_1200 | F_2400 | 0x10 | 0x20 | F_CELL,
};

/* 0xC0's flags (D-Modem's): the extension, fallback, fall forward */
enum : u8 { X_ON = 0x01, X_FALLBACK = 0x02, X_FALLFWD = 0x04 };

/* type 8's subtypes */
enum : u8 {
	S_FB_REQ = 0x02, S_FB_ACK = 0x03, S_FF_REQ = 0x04, S_FF_ACK = 0x05,
	S_FF_NAK = 0x09, S_LMI = 0x0a,
};

enum { T8 = 8, T9 = 9 };

/* inc(x): bits 0-6 one more, bit 7 kept */
u8 inc7(u8 x)
{
	return static_cast<u8>((x & 0x80) | ((x + 1) & 0x7f));
}

/* the modulations' rates; at a rate the ladder has in several, a change
   of modulation goes to the first here (the call's own first) */
const Rung k_rates[] = {
	{ Mod::V92, 56000 }, { Mod::V90, 56000 }, { Mod::K56, 56000 },
	{ Mod::X2, 56000 },
	{ Mod::V32T, 19200 }, { Mod::V32T, 16800 },
	{ Mod::V32B, 14400 }, { Mod::V32B, 12000 }, { Mod::V32B, 9600 },
	{ Mod::V32B, 7200 }, { Mod::V32B, 4800 },
	{ Mod::V22B, 2400 }, { Mod::V22, 1200 }, { Mod::B212, 1200 },
	{ Mod::V34, 33600 }, { Mod::V34, 31200 }, { Mod::V34, 28800 },
	{ Mod::V34, 26400 }, { Mod::V34, 24000 }, { Mod::V34, 21600 },
	{ Mod::V34, 19200 }, { Mod::V34, 16800 }, { Mod::V34, 14400 },
	{ Mod::V34, 12000 }, { Mod::V34, 9600 }, { Mod::V34, 7200 },
	{ Mod::V34, 4800 }, { Mod::V34, 2400 },
	{ Mod::VFC, 28800 }, { Mod::VFC, 26400 }, { Mod::VFC, 24000 },
	{ Mod::VFC, 21600 }, { Mod::VFC, 19200 }, { Mod::VFC, 16800 },
	{ Mod::VFC, 14400 },
	{ Mod::HST, 16800 }, { Mod::HST, 14400 }, { Mod::HST, 12000 },
	{ Mod::HST, 9600 }, { Mod::HST, 7200 }, { Mod::HST, 4800 },
	{ Mod::V33, 14400 }, { Mod::V33, 12000 },
	{ Mod::V29, 9600 }, { Mod::V29, 7200 }, { Mod::V29, 4800 },
	{ Mod::V27T, 4800 }, { Mod::V27T, 2400 },
	{ Mod::V26T, 2400 }, { Mod::V26T, 1200 }, { Mod::V26, 2400 },
	{ Mod::B208, 4800 }, { Mod::B201, 2400 }, { Mod::B202, 1200 },
	{ Mod::V23, 1200 }, { Mod::V21, 300 }, { Mod::B103, 300 },
};

/* V.32bis's mask (0x0C's byte 1): bit, rate */
const struct { u8 bit; s32 rate; } k_v32_bits[] = {
	{ 0x20, 14400 }, { 0x08, 12000 }, { 0x01, 9600 }, { 0x10, 7200 },
	{ 0x04, 4800 },
};

/* the V.34 (or V.FC) rate's bit in 0x0C's bytes 2 and 3 (byte << 8 |
   bit), 0 for none */
u16 v34_bit(s32 rate)
{
	switch (rate) {
	case 33600: return 0x0204;
	case 31200: return 0x0202;
	case 9600: return 0x0280;
	case 7200: return 0x0240;
	case 4800: return 0x0220;
	case 2400: return 0x0210;
	default:
		if (rate >= 12000 && rate <= 28800 && rate % 2400 == 0)
			return static_cast<u16>(0x0300 |
						1 << ((rate - 12000) / 2400));
		return 0;
	}
}

/* modulations the pumps change between by a retrain (or V.34's rate
   renegotiation) rather than a start-up */
bool in_pump(Mod m)
{
	return m == Mod::V34 || m == Mod::V32B || m == Mod::V32T;
}

/* those whose rate the pump picks (V.34's auto-rate, PCM) */
bool auto_rate(Mod m)
{
	return m == Mod::V34 || m == Mod::V92 || m == Mod::V90 ||
	       m == Mod::K56 || m == Mod::X2;
}

/* the modulation's bit in a set of them (Owner::mnp10_mods()) */
u32 mod_bit(Mod m)
{
	return UINT32_C(1) << static_cast<u8>(m);
}

/* the modulation's name, for the log */
const char *mod_name(Mod m)
{
	static const char *const names[] = {
		"V.92", "V.90", "K56flex", "x2", "V.34", "V.FC",
		"V.32terbo", "V.32bis", "HST", "V.26ter", "V.22bis", "V.22",
		"Bell 212A", "V.23", "V.21", "Bell 103", "V.33", "V.29",
		"V.27ter", "V.26", "Bell 208", "Bell 201", "Bell 202",
	};
	return m < Mod::Count ? names[static_cast<u8>(m)] : "none";
}

} // namespace


/*
 *    the line's codes
 *
 */

/* G: the receive level (dBm0, as a negative number) through the table;
   0xFF when not known */
u8 Mnp::g_code(s16 dbm)
{
	static const s16 at_most[] = { 14, 15, 16, 18, 20, 22, 24, 26, 28,
				       30, 32, 34, 36, 38, 40, 42 };
	static const u8 code[] = { 0x00, 0x01, 0x03, 0x05, 0x07, 0x0a, 0x0d,
				   0x0f, 0x12, 0x15, 0x17, 0x1a, 0x1c, 0x1f,
				   0x22, 0x25 };
	if (dbm >= 0)
		return 0xff;
	const s16 level = static_cast<s16>(-dbm);
	for (size_t i = 0; i < sizeof(at_most) / sizeof(at_most[0]); i++)
		if (level <= at_most[i])
			return code[i];
	return 0x26;
}

/* E: the EQM byte through the table; 0xFF when not known (or past
   127) */
u8 Mnp::e_code(s16 eqm)
{
	static const s16 at_most[] = { 0, 1, 4, 6, 8, 10, 12, 14, 16, 18, 20,
				       25, 30, 35, 40, 48, 64, 96, 127 };
	static const u8 code[] = { 0x00, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0a,
				   0x0b, 0x0c, 0x0d, 0x0e, 0x10, 0x11, 0x12,
				   0x14, 0x16, 0x1a, 0x20, 0x28 };
	if (eqm < 0)
		return 0xff;
	for (size_t i = 0; i < sizeof(at_most) / sizeof(at_most[0]); i++)
		if (eqm <= at_most[i])
			return code[i];
	return 0xff;
}


/*
 *    the LR
 *
 */

/* our 0x0C: the feature byte (shifts only on the modulations the
   DeskPorte shifts on; 2400 and 1200 by -Q and the top speed; the
   cellular mode), V.32bis's mask, V.34's (or V.FC's) when the call is
   on it */
void Mnp::m10_local_0c()
{
	const u32 mods = link_->mnp10_mods();
	const Mod cur = link_->mnp10_mod();
	const bool v32 = mods & mod_bit(Mod::V32B);
	u8 *b = m10_ours_;
	memset(b, 0, 4);
	b[0] = F_RSVD;
	if ((mods & mod_bit(cur)) &&
	    (cur == Mod::V34 || cur == Mod::V32B || cur == Mod::V22B ||
	     cur == Mod::V22))
		b[0] |= F_FALLBACK | F_FALLFWD;
	if (cfg_lowest_ >= 2 && cfg_top_ >= 1200)
		b[0] |= F_1200;
	if (cfg_lowest_ >= 1 && cfg_top_ > 1200)
		b[0] |= F_2400;
	if (cfg_cell_ && cfg_top_ > 600)
		b[0] |= F_CELL;
	if (v32)
		for (const auto &v : k_v32_bits)
			if (v.rate <= cfg_top_)
				b[1] |= v.bit;
	if (cur == Mod::V34 || cur == Mod::VFC) {
		for (s32 r = 2400; r <= 33600; r += 2400) {
			const u16 bit = v34_bit(r);
			if (!bit || r > cfg_top_)
				continue;
			/* 2400 only as the top speed (as the DeskPorte) */
			if (r == 2400 && cfg_top_ > 2400)
				continue;
			if (cur == Mod::VFC && (r < 14400 || r > 28800))
				continue;
			b[bit >> 8] |= static_cast<u8>(bit);
		}
		if (cur == Mod::V34)
			b[2] |= 1;
		else
			b[2] = 0;
	}
	m10_ours_n_ = b[2] || b[3] ? 4 : 2;
	/* 0xC0: the modulations, shifts if the call is on one of them */
	ext_ours_ = mods & ~failed_mods_;
	ext_flags_ours_ = cfg_ext_ ? X_ON : 0;
	if (cfg_ext_ && (ext_ours_ & mod_bit(cur)))
		ext_flags_ours_ |= X_FALLBACK | X_FALLFWD;
}

/* MNP 10's parameters of our LR from b[n] on: 0x0C, 0x0D, D-Modem's
   0xC0, the originator's 0x11 (the responder: those the initiator's
   had); the new length */
s16 Mnp::m10_lr(u8 *b, s16 n)
{
	if (!m10_offer_ || (!initiator_ && !m10_seen_))
		return n;
	m10_local_0c();
	b[n++] = PI_M10;
	b[n++] = static_cast<u8>(m10_ours_n_);
	memcpy(b + n, m10_ours_, static_cast<size_t>(m10_ours_n_));
	n = static_cast<s16>(n + m10_ours_n_);
	s16 dbm, eqm;
	link_->mnp10_line(dbm, eqm);
	b[n++] = PI_QUALITY;
	b[n++] = 2;
	b[n++] = g_code(dbm);
	b[n++] = e_code(eqm);
	if (cfg_ext_ && (initiator_ || ext_theirs_)) {
		const u32 mods = ext_ours_;
		const u16 top = static_cast<u16>(cfg_top_ / 100);
		b[n++] = PI_EXT;
		b[n++] = 7;
		b[n++] = ext_flags_ours_;
		b[n++] = static_cast<u8>(mods);
		b[n++] = static_cast<u8>(mods >> 8);
		b[n++] = static_cast<u8>(mods >> 16);
		b[n++] = static_cast<u8>(top);
		b[n++] = static_cast<u8>(top >> 8);
		b[n++] = static_cast<u8>(cfg_lowest_);
	}
	if (initiator_) {
		b[n++] = PI_ORIG;
		b[n++] = 1;
		b[n++] = 0;
	}
	return n;
}

/* the remote's LR, MNP 10's parameters in it (len octets with the
   length indication) */
void Mnp::m10_rx_lr(const u8 *h, s16 len)
{
	m10_seen_ = false;
	memset(m10_theirs_, 0, 4);
	ext_theirs_ = 0;
	ext_flags_theirs_ = 0;
	p11_ = false;
	first_g_ = first_e_ = -1;
	m10_opt_ = 0;
	for (s16 i = 3; i + 2 <= len && i + 2 + h[i + 1] <= len;
	     i += 2 + h[i + 1]) {
		const u8 pi = h[i], pl = h[i + 1];
		const u8 *pv = h + i + 2;
		switch (pi) {
		case PI_OPT:
			if (pl >= 1)
				m10_opt_ = pv[0];
			break;
		case PI_M10:
			m10_seen_ = true;
			memset(m10_theirs_, 0, 4);
			memcpy(m10_theirs_, pv, std::min<size_t>(pl, 4));
			break;
		case PI_M10_OLD:
			/* the older form: two octets before 0x0C's */
			if (pl >= 3 && !m10_seen_) {
				m10_seen_ = true;
				memset(m10_theirs_, 0, 4);
				memcpy(m10_theirs_, pv + 2,
				       std::min<size_t>(pl - 2, 4));
			}
			break;
		case PI_QUALITY:
			if (pl >= 2) {
				first_g_ = pv[0];
				first_e_ = pv[1];
			}
			break;
		case PI_EXT:
			if (pl >= 4 && (pv[0] & X_ON)) {
				ext_flags_theirs_ = pv[0];
				ext_theirs_ = pv[1] | static_cast<u32>(pv[2]) << 8 |
					static_cast<u32>(pv[3]) << 16;
				if (pl >= 6) {
					const s32 top = (pv[4] | pv[5] << 8) * 100;
					if (top > 0 && top < cfg_top_)
						cfg_top_ = top;
				}
				if (pl >= 7 && pv[6] < cfg_lowest_)
					cfg_lowest_ = pv[6];
			}
			break;
		case PI_ORIG:
			p11_ = true;
			break;
		default:
			/* 0x0B, 0x0E, 0x80, 0x82 and the rest: ignored */
			break;
		}
	}
}

/* what the two LRs agreed for MNP 10 (each side alike): parameter 8's
   bit 4 at service class 3, 0x0C ANDed (its cellular bit ORed), the
   V.32bis mask cleared where V.34's is left (but between two
   D-Modems); false if the remote's LR cannot be taken (an originator
   given 0x11) */
bool Mnp::m10_agree(bool initiator)
{
	m10_ = cell_ = ext_ = false;
	fb_ok_ = ff_ok_ = false;
	if (!m10_offer_)
		return true;
	if (initiator && p11_ && !upgraded_) {
		link_->logf("mnp10: parameter 0x11 from the answerer.");
		return false;
	}
	s16 opt = static_cast<s16>(m10_opt_ & OPT_KNOWN);
	if (opt & OPT_V42)
		opt = static_cast<s16>(opt & 0x7f);
	if (!(opt & OPT_M10) || mode_ != 3 || !m10_seen_)
		return true;
	m10_local_0c();
	for (s16 i = 0; i < 4; i++)
		m10_agreed_[i] = static_cast<u8>(m10_ours_[i] & m10_theirs_[i]);
	if ((m10_ours_[0] | m10_theirs_[0]) & F_CELL)
		m10_agreed_[0] |= F_CELL;
	ext_ = cfg_ext_ && (ext_flags_theirs_ & X_ON) && (ext_flags_ours_ & X_ON);
	if (!ext_ && ((m10_agreed_[2] & 0xfe) || m10_agreed_[3]))
		m10_agreed_[1] = 0;
	const u8 *a = m10_agreed_;
	if (!(a[0] & F_ACTIVE) && !a[1] && !(a[2] & 0xfe) && !a[3] && !ext_)
		return true;
	m10_ = true;
	cell_ = a[0] & F_CELL;
	fb_ok_ = a[0] & F_FALLBACK;
	ff_ok_ = a[0] & F_FALLFWD;
	if (ext_) {
		fb_ok_ = ext_flags_ours_ & ext_flags_theirs_ & X_FALLBACK;
		ff_ok_ = ext_flags_ours_ & ext_flags_theirs_ & X_FALLFWD;
	}
	opt_ = static_cast<s16>(opt_ | OPT_M10);
	link_->logf("mnp10: agreed: %02x %02x %02x %02x%s%s%s%s.", a[0], a[1],
		    a[2], a[3], cell_ ? ", cellular" : "",
		    fb_ok_ ? ", fallback" : "", ff_ok_ ? ", fall forward" : "",
		    ext_ ? ", D-Modem's modulations" : "");
	return true;
}

/* MNP's parameters for V.42's XID (-K1): k, then 8 on as in our LR */
s16 Mnp::upgrade_params(u8 *b, s16 n, bool initiator)
{
	u8 tmp[64];
	s16 k = 0;
	initiator_ = initiator;
	tmp[k++] = 3;
	tmp[k++] = 1;
	tmp[k++] = static_cast<u8>(initiator ? our_k_ : k_);
	tmp[k++] = PI_OPT;
	tmp[k++] = 1;
	tmp[k++] = static_cast<u8>(initiator ? our_opt_ | OPT_M10 : opt_);
	if (our_mnp5_) {
		tmp[k++] = PI_COMP;
		tmp[k++] = 1;
		tmp[k++] = 1;
	}
	const bool was = initiator_;
	initiator_ = true;      /* (0x0C and 0x0D always; 0x11 not) */
	k = m10_lr(tmp, k);
	initiator_ = was;
	if (k >= 3 && tmp[k - 3] == PI_ORIG)
		k = static_cast<s16>(k - 3);
	k = std::min(k, n);
	memcpy(b, tmp, static_cast<size_t>(k));
	return k;
}

/* the remote's MNP parameters in its XID (len octets from p): agreed
   as an LR's (constant parameter 2, service class 3); true if MNP 10
   is agreed */
bool Mnp::upgrade_rx(const u8 *p, s16 len, bool initiator)
{
	u8 h[3 + 128];
	if (len > 120)
		return false;
	h[1] = LR;
	h[2] = 2;
	h[3] = 2;
	h[4] = 1;
	h[5] = 3;
	memcpy(h + 6, p, static_cast<size_t>(len));
	h[0] = static_cast<u8>(5 + len);
	initiator_ = initiator;
	upgraded_ = true;
	if (!rx_lr(h, static_cast<s16>(6 + len)) || !m10_) {
		upgraded_ = false;
		return false;
	}
	return true;
}

/* the XIDs agreed (upgrade_rx()): the data phase in HDLC frames, the
   initiator's LA (the LRs' confirmation) first, the responder's wait
   for it */
void Mnp::upgrade_start(bool initiator)
{
	initiator_ = initiator;
	upgraded_ = true;
	bit_tx_ = true;
	tries_ = 1;
	heard_ = true;
	t_start_ = 0;
	if (initiator) {
		phase_ = Phase::Confirm;
		t401_ = 0;
	} else {
		phase_ = Phase::Respond;
		t401_ = t401_est();
	}
}


/*
 *    the ladder
 *
 */

/* the rungs the call may go to: between two D-Modems every modulation
   both can run (that has not failed in this call) within the top speed
   and -Q; else those of the call's modulation class, from 0x0C */
void Mnp::m10_ladder()
{
	const u8 *a = m10_agreed_;
	const Mod cur = link_->mnp10_mod();
	const u32 common = ext_ ? ext_ours_ & ext_theirs_ & ~failed_mods_ : 0;
	const s32 lowest = cfg_lowest_ == 0 ? 4800 : cfg_lowest_ == 1 ? 2400 :
		0;
	nrungs_ = 0;
	for (const Rung &r : k_rates) {
		bool ok;
		if (ext_) {
			ok = (common & mod_bit(r.mod)) &&
				(r.rate <= cfg_top_ || r.rate == 56000) &&
				r.rate >= lowest;
			/* the 0x0C bits the DeskPorte has, as it has them */
			if (r.mod == Mod::V22B)
				ok = ok && (a[0] & F_2400);
			else if (r.mod == Mod::V22)
				ok = ok && (a[0] & F_1200);
		} else {
			switch (r.mod) {
			case Mod::V34:
			case Mod::VFC: {
				const u16 bit = v34_bit(r.rate);
				ok = cur == r.mod && bit &&
					(a[bit >> 8] & static_cast<u8>(bit)) &&
					((a[2] & 1) == (r.mod == Mod::V34)) &&
					(r.mod == Mod::V34 ||
					 (r.rate >= 14400 && r.rate <= 28800));
				break;
			}
			case Mod::V32B:
			case Mod::V22B:
			case Mod::V22:
				ok = cur == Mod::V32B || cur == Mod::V22B ||
					cur == Mod::V22;
				if (r.mod == Mod::V22B)
					ok = ok && (a[0] & F_2400);
				else if (r.mod == Mod::V22)
					ok = ok && (a[0] & F_1200);
				else {
					u8 bit = 0;
					for (const auto &v : k_v32_bits)
						if (v.rate == r.rate)
							bit = v.bit;
					ok = ok && (a[1] & bit);
				}
				break;
			default:
				ok = false;
				break;
			}
		}
		if (ok && nrungs_ < MAX_RUNGS)
			rungs_[nrungs_++] = r;
	}
	std::stable_sort(rungs_, rungs_ + nrungs_,
			 [](const Rung &x, const Rung &y) {
				 return x.rate > y.rate;
			 });
}

/* the rung of mod at rate: that one, else mod's highest below rate,
   else the ladder's highest at or below rate (its own modulation's
   first); -1 for none */
s16 Mnp::m10_find(Mod mod, s32 rate) const
{
	for (s16 i = 0; i < nrungs_; i++)
		if (rungs_[i].mod == mod && rungs_[i].rate <= rate)
			return i;
	/* PCM's one rung, whatever its rates now */
	for (s16 i = 0; i < nrungs_; i++)
		if (rungs_[i].mod == mod && rungs_[i].rate == 56000)
			return i;
	for (s16 i = 0; i < nrungs_; i++)
		if (rungs_[i].rate <= rate)
			return i;
	return -1;
}

/* the next rung up (or down): the nearest rate above (below) the
   current rung's, in its modulation if that has the rate, else the
   first that does; -1 for none */
s16 Mnp::m10_next(bool up) const
{
	if (cur_ < 0)
		return -1;
	const Rung &c = rungs_[cur_];
	s32 target = 0;
	for (s16 i = 0; i < nrungs_; i++) {
		const s32 r = rungs_[i].rate;
		if (up ? r > c.rate && (!target || r < target) :
			 r < c.rate && r > target)
			target = r;
	}
	if (!target)
		return -1;
	s16 first = -1;
	for (s16 i = 0; i < nrungs_; i++) {
		if (rungs_[i].rate != target)
			continue;
		if (rungs_[i].mod == c.mod)
			return i;
		if (first < 0)
			first = i;
	}
	return first;
}


/*
 *    the data phase
 *
 */

/* the shift state's reset (after the link's start and each shift):
   P, X, H, the probes, the reports, the hold-offs */
void Mnp::m10_reset()
{
	pkt_p_ = 0x20;
	err_x_ = 0;
	health_ = 9;
	lt_again_since_ = false;
	x_high_secs_ = 0;
	lmi_s_ = lmi_a_ = lmi_r_ = 0;
	t9_due_ = probe_due_ = false;
	probe_ticks_ = 0;
	peer_g_ = peer_e_ = 0xff;
	holdoff_ = holdoff_len_ = HOLDOFF_START;
	post_hold_ = POST_HOLD;
	sig_ = 0;
	sig_copies_ = 0;
	ticks_ = 0;
	sec_ticks_ = 0;
	t19_ticks_ = 0;
	eqm_sum_ = eqm_n_ = eqm_wins_ = eqm_none_ = 0;
	sh_ = Sh::Idle;
	sh_ticks_ = 0;
	sh_restart_ = sh_responder_ = false;
}

/* MNP 10's start with the data phase: the ladder and the call's rung,
   P from the rate, the cellular transmit level's start */
void Mnp::m10_start()
{
	cur_ = -1;
	nrungs_ = 0;
	failed_mods_ = 0;
	sh_fails_ = 0;
	cell_retries_ = 2;
	shifts_ = 0;
	ff_failed_ = false;
	if (!m10_)
		return;
	m10_ladder();
	cur_ = m10_find(link_->mnp10_mod(), rate());
	m10_reset();
	pkt_p_ = rate() <= 2400 ? 0x20 : 0xc0;
	link_->logf("mnp10: %d rungs, on %d (%s %" PRId32 ").", nrungs_, cur_,
		    cur_ >= 0 ? mod_name(rungs_[cur_].mod) : "none",
		    cur_ >= 0 ? rungs_[cur_].rate : 0);
	if (cell_) {
		/* @M: 8 for @M0, 20 for @M1, @M - 10 from @M10 */
		lvl_ = cfg_level_ == 0 ? 8 : cfg_level_ == 1 ? 20 :
			cfg_level_ >= 10 ? static_cast<s16>(cfg_level_ - 10) : 0;
		lvl_ = std::min<s16>(lvl_, 25);
		prev_e_ = prev_lvl_ = -1;
		lvl_first_ = true;
		link_->mnp10_tx_level(lvl_);
		if (first_g_ >= 0 && first_g_ != 0xff && first_e_ != 0xff)
			tx_level_report(first_g_, first_e_);
	}
}

/* our LT's information field now: P + 1 with the 256-octet option
   (8 at least), else N401 */
s16 Mnp::lt_size() const
{
	if (!m10_ || !(opt_ & 1))
		return n401_;
	return static_cast<s16>(std::min<s32>(n401_,
					      std::max<s16>(pkt_p_, 7) + 1));
}

/* a tick: the local line sampled, the probe's timer, the shift's
   timers, :T19, once a second the shift policy */
void Mnp::m10_tick()
{
	ticks_++;
	s16 dbm, eqm;
	link_->mnp10_line(dbm, eqm);
	g_ = g_code(dbm);
	e_ = e_code(eqm);
	if (eqm < 0)
		eqm_none_ = static_cast<s16>(std::min(eqm_none_ + 1, 1000));
	else
		eqm_none_ = 0;
	if (eqm >= 0) {
		eqm_sum_ += std::min<s16>(eqm, 255);
		if (++eqm_n_ >= 16) {
			for (s16 i = 3; i > 0; i--)
				eqm_win_[i] = eqm_win_[i - 1];
			eqm_win_[0] = static_cast<s16>(eqm_sum_ / 16);
			eqm_wins_ = std::min<s16>(eqm_wins_ + 1, 4);
			eqm_sum_ = eqm_n_ = 0;
		}
	}
	if ((opt_ & 1) && ++probe_ticks_ >= PROBE_TICKS) {
		probe_ticks_ = 0;
		probe_due_ = true;
	}
	if (cfg_t19_ && rate() <= 1200 && ++t19_ticks_ >= T19_TICKS) {
		link_->logf("mnp10: :T19: nothing for 120 s at 1200 bit/s.");
		t19_ticks_ = 0;
		fail(4);
		return;
	}
	switch (sh_) {
	case Sh::FfReq:
		if (--sh_ticks_ <= 0) {
			link_->logf("mnp10: fall forward unanswered.");
			holdoff_len_ = std::min(holdoff_len_ * 2,
						static_cast<s32>(HOLDOFF_MAX));
			holdoff_ = holdoff_len_;
			sh_ = Sh::Idle;
		}
		break;
	case Sh::FbReq:
		if (!sig_copies_ && --sh_ticks_ <= 0)
			go(m10_next(false), false);
		break;
	case Sh::Resync:
		if (--sh_ticks_ <= 0) {
			link_->logf("mnp10: no frame after the shift.");
			fail(4);
		}
		break;
	default:
		break;
	}
	if (++sec_ticks_ >= TICKS_S) {
		sec_ticks_ = 0;
		m10_second();
	}
}

/* the fall forward's tests: P full (not in auto-rate), the error
   average and the smoothed EQM good, a rung above; the responder's
   P alone in auto-rate */
bool Mnp::ff_test(bool responder) const
{
	if (cur_ < 0 || m10_next(true) < 0)
		return false;
	const bool autor = auto_rate(rungs_[cur_].mod);
	if (responder)
		return autor || pkt_p_ >= P_FULL;
	if (!autor && pkt_p_ < P_FULL)
		return false;
	if (err_x_ > X_GOOD)
		return false;
	/* no EQM from this pump: P full and X low are the line's word */
	if (eqm_wins_ < 4)
		return eqm_none_ >= 4 * 16;
	s32 sum = 0;
	for (s16 i = 0; i < 4; i++)
		sum += eqm_win_[i];
	return sum / 4 <= EQM_GOOD;
}

/* once a second: the hold-offs; with no shift going on, a fall forward
   or a fallback asked for when the line says so */
void Mnp::m10_second()
{
	if (holdoff_ > 0)
		holdoff_--;
	if (post_hold_ > 0)
		post_hold_--;
	if (cell_ && err_x_ >= X_HIGH)
		x_high_secs_++;
	else
		x_high_secs_ = 0;
	if (sh_ != Sh::Idle || phase_ != Phase::Data)
		return;
	if (ff_ok_ && !holdoff_ && ff_test(false)) {
		ff_start();
		return;
	}
	if (fb_ok_ && !post_hold_ && m10_next(false) >= 0 &&
	    ((cell_ && x_high_secs_ >= 5) || (!cell_ && err_x_ >= X_HIGH) ||
	     health_ <= 0))
		fb_start();
}

/* our fall forward: 04, its answer awaited 4 s */
void Mnp::ff_start()
{
	link_->logf("mnp10: fall forward asked.");
	sh_ = Sh::FfReq;
	sh_ticks_ = FF_WAIT;
	sh_responder_ = false;
	sig_ = S_FF_REQ;
	sig_copies_ = 1;
}

/* our fallback: 02 up to 6 times, then 1 s for the answer */
void Mnp::fb_start()
{
	link_->logf("mnp10: fallback asked (X %d, H %d).", err_x_, health_);
	sh_ = Sh::FbReq;
	sh_ticks_ = FB_WAIT;
	sh_responder_ = false;
	sig_ = S_FB_REQ;
	sig_copies_ = FB_COPIES;
}

/* the pumps' change to rung target (up or down) once our last signal
   is out (the initiator's at once) */
void Mnp::go(s16 target, bool up)
{
	if (target < 0) {
		sh_ = Sh::Idle;
		return;
	}
	if (sig_copies_) {
		sh_ = Sh::Go;
		sh_target_ = target;
		sh_up_ = up;
		return;
	}
	pump(target, up, sh_restart_);
}

/* the pumps' change to rung target: a retrain or V.34's rate
   renegotiation in the same modulation, else its start-up (or always,
   restart: no pump runs); the link waits (shift_done()) */
void Mnp::pump(s16 target, bool up, bool restart)
{
	const Rung &t = rungs_[target];
	const Mod from = cur_ >= 0 ? rungs_[cur_].mod : Mod::Count;
	Shift s;
	s.mod = t.mod;
	s.rate = t.rate;
	s.up = up;
	s.lead = !sh_responder_;
	if (!restart && from == t.mod && in_pump(t.mod))
		s.kind = up && t.mod == Mod::V34 ? ShiftKind::Reneg :
			ShiftKind::Retrain;
	else
		s.kind = ShiftKind::Restart;
	sh_from_ = cur_;
	sh_target_ = target;
	sh_up_ = up;
	sh_ = Sh::Pump;
	sig_ = 0;
	sig_copies_ = 0;
	link_->logf("mnp10: %s to %s %" PRId32 " (%s).", up ? "up" : "down",
		    mod_name(t.mod), t.rate,
		    s.kind == ShiftKind::Reneg ? "rate renegotiation" :
		    s.kind == ShiftKind::Retrain ? "retrain" : "start-up");
	sh_restart_ = s.kind == ShiftKind::Restart;
	if (!link_->mnp10_shift(s))
		shift_failed();
}

/* the pumps could not change: back to where the call came from (a
   fall forward), or further down (another modulation between two
   D-Modems; the cellular mode's 1200 bit/s); else the end */
void Mnp::shift_failed()
{
	const s16 target = sh_target_, from = sh_from_;
	const bool up = sh_up_;
	link_->logf("mnp10: the change failed.");
	if (++sh_fails_ > MAX_FAILS) {
		fail(4);
		return;
	}
	if (up) {
		ff_failed_ = true;
		holdoff_len_ = std::min(holdoff_len_ * 2,
					static_cast<s32>(HOLDOFF_MAX));
		/* a modulation that would not start is not tried again */
		if (ext_ && target >= 0 && from >= 0 &&
		    rungs_[target].mod != rungs_[from].mod) {
			const Rung back = rungs_[from];
			failed_mods_ |= mod_bit(rungs_[target].mod);
			m10_ladder();
			cur_ = m10_find(back.mod, back.rate);
		} else
			cur_ = from;
		if (cur_ >= 0) {
			pump(cur_, false, true);
			return;
		}
		fail(4);
		return;
	}
	s16 next = -1;
	if (ext_ && target >= 0) {
		const Rung t = rungs_[target];
		failed_mods_ |= mod_bit(t.mod);
		m10_ladder();
		next = m10_find(Mod::Count, t.rate);
	}
	if (next < 0 && cell_ && cell_retries_ > 0 && nrungs_) {
		cell_retries_--;
		next = static_cast<s16>(nrungs_ - 1);
	}
	if (next < 0) {
		fail(4);
		return;
	}
	cur_ = -1;
	pump(next, false, true);
}

/* the pumps changed (ok), or did not */
void Mnp::shift_done(bool ok)
{
	if (sh_ != Sh::Pump)
		return;
	if (!ok) {
		shift_failed();
		return;
	}
	sh_fails_ = 0;
	shifts_++;
	const Rung t = rungs_[sh_target_];
	/* the rung the pump came up on (it may pick another rate) */
	cur_ = m10_find(link_->mnp10_mod(), rate());
	if (cur_ < 0)
		cur_ = sh_target_;
	link_->logf("mnp10: shifted to %s %" PRId32 " (asked %s %" PRId32 ").",
		    mod_name(rungs_[cur_].mod), rate(), mod_name(t.mod), t.rate);
	const bool responder = sh_responder_;
	const bool failed = ff_failed_;
	const s32 hold = holdoff_len_;
	m10_reset();
	if (failed) {
		holdoff_len_ = hold;
		holdoff_ = hold;
		ff_failed_ = false;
	}
	/* the LTs outstanding go again at the new rate */
	if (vs_ != una_) {
		nxt_ = una_;
		t401_ = t401_data();
	}
	t404_ = 0;
	if (responder) {
		sh_ = Sh::Resync;
		sh_ticks_ = RESYNC_WAIT;
	} else {
		probe_due_ = true;
		sh_ = Sh::Idle;
	}
	if (cell_)
		link_->mnp10_tx_level(lvl_);
}


/*
 *    frames
 *
 */

/* a type 8 signal */
Frame *Mnp::build_t8(u8 sub)
{
	Frame *f = ctrl(3);
	ctrl_buf_[1] = T8;
	ctrl_buf_[2] = sub;
	return f;
}

/* an LMI probe, S then R (and G, E in the cellular mode); NULL with
   127 outstanding */
Frame *Mnp::build_probe()
{
	const u8 s = lmi_s_;
	const u8 next = inc7(s);
	if ((next & 0x7f) == (lmi_a_ & 0x7f))
		return nullptr;
	lmi_s_ = next;
	Frame *f = ctrl(cell_ ? 7 : 5);
	ctrl_buf_[1] = T8;
	ctrl_buf_[2] = S_LMI;
	ctrl_buf_[3] = s;
	ctrl_buf_[4] = lmi_r_;
	if (cell_) {
		ctrl_buf_[5] = static_cast<u8>(g_);
		ctrl_buf_[6] = static_cast<u8>(e_);
	}
	t9_due_ = false;
	return f;
}

/* a type 9: an LA with R */
Frame *Mnp::build_t9()
{
	Frame *f = build_la();
	if (f->size == 4) {
		/* 03 05 NR NK -> 04 09 R NR NK */
		ctrl_buf_[4] = ctrl_buf_[3];
		ctrl_buf_[3] = ctrl_buf_[2];
		ctrl_buf_[2] = lmi_r_;
		ctrl_buf_[1] = T9;
		f = ctrl(5);
	} else {
		/* 07 05 01 01 NR 02 01 NK -> 08 09 R 01 01 NR 02 01 NK */
		memmove(ctrl_buf_ + 3, ctrl_buf_ + 2, 6);
		ctrl_buf_[2] = lmi_r_;
		ctrl_buf_[1] = T9;
		f = ctrl(9);
	}
	t9_due_ = false;
	return f;
}

/* MNP 10's frames before the LTs: a signal, then (with an LA due) a
   type 9 in the LA's place, a probe; NULL for none (and none at all
   while the pumps change) */
Frame *Mnp::m10_frame()
{
	if (sh_ == Sh::Pump)
		return nullptr;
	if (sig_copies_ > 0) {
		sig_copies_--;
		return build_t8(sig_);
	}
	if (probe_due_ && (opt_ & 1) && sh_ != Sh::FfReq) {
		probe_due_ = false;
		probe_ticks_ = 0;
		if (Frame *f = build_probe())
			return f;
	}
	return nullptr;
}

/* a frame is out: X, and the responder's change once its answer went */
void Mnp::m10_sent(const Frame *f)
{
	if (!m10_ || phase_ != Phase::Data)
		return;
	if (cur_ < 0 || !auto_rate(rungs_[cur_].mod)) {
		s16 x = static_cast<s16>(err_x_ - err_x_ / 16);
		x = static_cast<s16>(std::max(0, x - 2));
		if (lt_again_since_)
			x = static_cast<s16>(x + 16);
		err_x_ = std::min<s16>(x, 255);
	}
	lt_again_since_ = false;
	if (f == &ctrl_ && ctrl_buf_[1] == T8 && sh_ == Sh::Go &&
	    !sig_copies_)
		pump(sh_target_, sh_up_, sh_restart_);
}

/* an LT goes again: P shrinks, X counts it */
void Mnp::m10_lt_again()
{
	if (!m10_)
		return;
	lt_again_since_ = true;
	if (opt_ & 1)
		p_shrink();
}

/* P grows by d acknowledged probes */
void Mnp::p_grow(s16 d)
{
	for (s16 i = 0; i < d; i++) {
		s16 add = pkt_p_ < 0x40 ? 16 : pkt_p_ < 0x80 ? 12 :
			pkt_p_ < 0xc0 ? 8 : 4;
		if (rate() <= 1200)
			add = static_cast<s16>(add / 2 + 1);
		pkt_p_ = std::min<s16>(pkt_p_ + add, 255);
	}
	if (pkt_p_ > 0x10)
		health_ = 8;
}

/* P shrinks for an LT sent again; H falls when it is small */
void Mnp::p_shrink()
{
	pkt_p_ = static_cast<s16>(std::max(0, pkt_p_ - (pkt_p_ * 45 / 256 + 1)));
	if (pkt_p_ < 0x11 && health_ > 0)
		health_--;
}

/* an acknowledgement R' of our probes (in a probe or a type 9) */
void Mnp::rx_ack(u8 r)
{
	if ((r ^ lmi_s_) & 0x80) {
		lmi_s_ = lmi_a_ = r;
		if (ticks_ >= HALVE_AFTER)
			pkt_p_ = std::max<s16>(pkt_p_ / 2, 7);
		return;
	}
	const s16 d = static_cast<s16>((r - lmi_a_) & 0x7f);
	if (!d)
		return;
	lmi_a_ = r;
	if (opt_ & 1)
		p_grow(d);
}

/* a probe 08 0A Q R' (G E) */
void Mnp::rx_probe(const u8 *h, s16 len)
{
	if (len < 5)
		return;
	const u8 q = h[3], r = h[4];
	t9_due_ = true;
	if (lmi_r_ == q)
		lmi_r_ = inc7(lmi_r_);
	else if (!((lmi_r_ ^ q) & 0x80))
		lmi_r_ ^= 0x80;
	if (h[0] >= 6 && len >= 7)
		rx_report(h[5], h[6]);
	rx_ack(r);
}

/* the remote's line report (cellular) */
void Mnp::rx_report(s16 g, s16 e)
{
	peer_g_ = g;
	peer_e_ = e;
	if (cell_ && g != 0xff && e != 0xff)
		tx_level_report(g, e);
}

/* the transmit level's hill climb on the remote's eye quality, bounded
   by its receive level (the 10EC section's rules); the level found
   goes out with the next report, not at 1200 bit/s */
void Mnp::tx_level_report(s16 g, s16 e)
{
	static const u8 t_table[] = {
		0x2b, 0x2a, 0x28, 0x27, 0x26, 0x25, 0x23, 0x22, 0x21, 0x1f,
		0x1e, 0x1d, 0x1b, 0x1a, 0x18, 0x17, 0x16, 0x14, 0x13, 0x12,
		0x10, 0x0f, 0x0e, 0x0c, 0x0b, 0x0a, 0x08, 0x07, 0x06, 0x04,
		0x03, 0x02, 0x01, 0x00 };
	const s16 l = lvl_;
	s16 t;
	if (g >= 0x29) {
		t = l;
	} else {
		s16 count = 0;
		for (u8 v : t_table)
			if (v > g)
				count++;
		t = static_cast<s16>(0x2d - count - 10);
	}
	const s16 h = static_cast<s16>(std::max(t, l) - l);
	s16 next;
	if (prev_e_ < 0 || lvl_first_ || l == prev_lvl_) {
		if (l < 8)
			next = static_cast<s16>(l + 2);
		else if (l >= 0x11)
			next = static_cast<s16>(l - 2);
		else
			next = static_cast<s16>(t >= 0x16 ? l - 2 : l + 2);
	} else {
		const bool went_up = l > prev_lvl_;
		if (e <= prev_e_)
			next = static_cast<s16>(went_up ? l + 2 : l - 2);
		else
			next = static_cast<s16>(went_up ? l - 4 : l + 4);
	}
	next = std::max<s16>(next, 0);
	next = std::min<s16>(next, 25);
	next = std::min<s16>(next, static_cast<s16>(30 - h));
	next = std::max<s16>(next, 0);
	prev_e_ = e;
	prev_lvl_ = l;
	lvl_first_ = false;
	if (next != lvl_ && rate() > 1200) {
		lvl_ = next;
		link_->mnp10_tx_level(lvl_);
	}
}

/* a type 8 frame (len octets with the length indication) */
void Mnp::rx_t8(const u8 *h, s16 len)
{
	if (len < 3)
		return;
	const u8 sub = h[2];
	if (sub == S_LMI) {
		rx_probe(h, len);
		return;
	}
	if (sh_ == Sh::Pump)
		return;
	switch (sub) {
	case S_FB_REQ:
		if (!fb_ok_)
			break;
		if (sh_ == Sh::FbReq) {
			/* crossing requests: both go down */
			sig_copies_ = 0;
			go(m10_next(false), false);
			break;
		}
		if (sh_ == Sh::Go)
			break;
		link_->logf("mnp10: fallback asked by the remote.");
		sh_responder_ = true;
		sig_ = S_FB_ACK;
		sig_copies_ = 1;
		sh_restart_ = false;
		go(m10_next(false), false);
		if (sh_ == Sh::Idle) {
			/* nowhere to go: no answer after all */
			sig_copies_ = 0;
		}
		break;
	case S_FB_ACK:
		if (sh_ == Sh::FbReq) {
			sig_copies_ = 0;
			go(m10_next(false), false);
		}
		break;
	case S_FF_REQ:
		if (!ff_ok_)
			break;
		if (sh_ == Sh::FfReq) {
			/* crossing: an accept */
			sh_responder_ = false;
			sig_ = S_FF_ACK;
			sig_copies_ = 1;
			go(m10_next(true), true);
			break;
		}
		if (sh_ != Sh::Idle)
			break;
		if ((holdoff_ >= HOLDOFF_REFUSE &&
		     !(cur_ >= 0 && auto_rate(rungs_[cur_].mod))) ||
		    !ff_test(true)) {
			link_->logf("mnp10: fall forward refused.");
			sig_ = S_FF_NAK;
			sig_copies_ = 1;
			break;
		}
		link_->logf("mnp10: fall forward asked by the remote.");
		sh_responder_ = true;
		sig_ = S_FF_ACK;
		sig_copies_ = 1;
		sh_restart_ = false;
		go(m10_next(true), true);
		break;
	case S_FF_ACK:
		if (sh_ == Sh::FfReq) {
			sh_restart_ = false;
			go(m10_next(true), true);
		}
		break;
	case S_FF_NAK:
		if (sh_ == Sh::FfReq) {
			link_->logf("mnp10: fall forward refused by the remote.");
			holdoff_ = holdoff_len_;
			sh_ = Sh::Idle;
		}
		break;
	default:
		break;
	}
}

/* a frame came in the data phase (type): :T19, the responder's wait
   after a shift */
void Mnp::m10_heard(s16 type)
{
	if (type == LA || type == T9 || type == T8)
		t19_ticks_ = 0;
	if (sh_ == Sh::Resync)
		sh_ = Sh::Idle;
}

} // namespace dlink
