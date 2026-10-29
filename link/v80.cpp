/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v80.cpp - V.80 Synchronous Access Mode (8.8/V.80): the DTE's
 *              EM-coded octets to the line's bits and back, in the
 *              transparent and framed sub-modes
 *
 *    Transmit: the DTE's characters (Link::read_chars()) are decoded
 *    one at a time; their octets, flags and aborts wait in a short
 *    list and go out one at a time, the octets least significant bit
 *    first (with zero insertion in a frame).  When the DTE has nothing
 *    the line gets SYN octets (transparent) or flags or marks (framed),
 *    after an abort or a flag for a frame cut short (+ESA's
 *    <framed_un_ov>).
 *
 *    Receive: the line's bits as octets (transparent; with +ESA's
 *    <trans_idle> 1 or 2 only from the first SYN sequence on, the
 *    octets aligned on it), or HDLC frames
 *    (framed): the receiver hunts for a flag, reports it with
 *    <EM><err>, then gives each frame's octets (the FCS held back and
 *    dropped when the DCE checks it) and <EM><flag> or <EM><err> at its
 *    end; seven 1s are <EM><mark> and a new hunt.  What goes to the DTE
 *    waits in a ring until it takes it; when the ring is full the
 *    octets are lost and <EM><rover> marks the place.
 *
 */

#include <cinttypes>

#include "dm_types.h"
#include "link.h"

namespace dlink {

using namespace v80;

/* the HDLC flag, and an abort's (and marks') octet */
static const u8 HDLC_FLAG = 0x7e, ONES = 0xff;

/* the FCS-32 register before a frame, and after a whole good frame,
   its FCS included (8.1.1.6.2/V.42) */
static const u32 FCS32_INIT = 0xffffffff, FCS32_GOOD = 0xdebb20e3;

/* the FCS-32 (the register reflected) after one more octet */
static u32 fcs32_add(u32 fcs, u8 c)
{
	fcs ^= c;
	for (s16 i = 0; i < 8; i++)
		fcs = fcs & 1 ? fcs >> 1 ^ 0xedb88320 : fcs >> 1;
	return fcs;
}

/* the octets of the shielding codes T1 to T20 (Table 9/V.80), from the
   set 19h, 99h, DC1, DC3: T1 to T4 one, T5 to T8 the same twice, T9 to
   T20 each ordered pair of two different ones */
static const u8 shielded[4] = { EM, EM8, DC1, DC3 };

/* the octets a shielding code stands for (n of them); false for one
   that is not */
static bool unshield(u8 code, u8 out[2], s16 &n)
{
	static const u8 single[4] = { T1, T2, T3, T4 };
	static const u8 twice[4] = { T5, T6, T7, T8 };
	for (s16 i = 0; i < 4; i++) {
		if (code == single[i]) {
			out[0] = shielded[i];
			n = 1;
			return true;
		}
		if (code == twice[i]) {
			out[0] = out[1] = shielded[i];
			n = 2;
			return true;
		}
	}
	if (code < T9 || code > T20)
		return false;
	s16 k = code - T9;
	s16 a = k / 3, b = k % 3;
	if (b >= a)
		b++;
	out[0] = shielded[a];
	out[1] = shielded[b];
	n = 2;
	return true;
}

/* a character an in-band command may have (6.1.1/V.80): 20h to 7Eh,
   A0h to FEh */
static bool command_char(u8 c)
{
	return (c >= 0x20 && c <= 0x7e) || (c >= 0xa0 && c <= 0xfe);
}

/* the <tx> <rx> code of rate (Table 10/V.80) */
u8 v80::rate_code(s32 rate)
{
	if (rate < 2400)
		return 0x20;
	if (rate < 4800)
		return 0x21;
	s32 k = (rate - 4800) / 2400;
	return static_cast<u8>(0x22 + (k > 12 ? 12 : k));
}

/* the rate of a <tx> <rx> code; 0 for none */
s32 v80::code_rate(u8 code)
{
	if (code == 0x20)
		return 1200;
	if (code == 0x21)
		return 2400;
	if (code >= 0x22 && code <= 0x2e)
		return 4800 + (code - 0x22) * 2400;
	return 0;
}


/*
 *    the mode
 *
 */

/* Synchronous Access Mode begins (after CONNECT): transparent both ways,
   nothing decoded, the first idle octet an underrun to report */
void SyncAccess::start(Link *link, const SyncConfig &cfg)
{
	framer_start(link);
	cfg_ = cfg;
	framed_ = false;
	esc_ = 0;
	pair_n_ = 0;
	pend_n_ = 0;
	underrun_ = false;
	in_frame_ = false;
	need_flag_ = false;
	tx_ones_ = 0;
	discard_ = false;
	discarded_ = 0;
	report_left_ = 0;
	syn_second_ = false;
	rx_reset(true);
	syn_hunt();
	out_head_ = out_count_ = 0;
	rover_ = false;
	link->set_timer(0, Link::Timer::None);
	link->logf("v80: synchronous access, transparent; idle %s, %s, "
		   "crc %d, syn 0x%02x 0x%02x (trans_idle %d).",
		   cfg.framed_idle ? "marks" : "flags",
		   cfg.framed_un_ov ? "flag on underrun" : "abort on underrun",
		   cfg.crc, cfg.syn1, cfg.syn2, cfg.trans_idle);
}

/* the transparent receiver hunts for the SYN sequence (8.8.1.2/V.80)
   when <trans_idle> asks for it: nothing to the DTE until it comes */
void SyncAccess::syn_hunt()
{
	syn_hunting_ = cfg_.trans_idle == 1 || cfg_.trans_idle == 2;
	syn_reg_ = 0;
	syn_bits_ = 0;
}

/* the framed sub-mode, from the transparent one (<EM><flag>): no frame
   begun, the receiver hunting, its buffer flushed */
void SyncAccess::enter_framed()
{
	link_->logf("v80: framed.");
	framed_ = true;
	in_frame_ = false;
	need_flag_ = false;
	tx_ones_ = 0;
	rx_reset(true);
	out_count_ = 0;
	rover_ = false;
}

/* the transparent sub-mode (<EM><mark>): a frame being sent is left as
   it is, the receiver's octets from here, its buffer flushed */
void SyncAccess::enter_transparent()
{
	link_->logf("v80: transparent.");
	framed_ = false;
	in_frame_ = false;
	discard_ = false;
	underrun_ = false;
	syn_second_ = false;
	rx_.clear();
	syn_hunt();
	out_count_ = 0;
	rover_ = false;
}

/* bits went out: <EM><bnum> with the octets the DTE's buffer holds,
   every report_period 10 ms at the pump's rate */
void SyncAccess::clock(s32 bits)
{
	if (!cfg_.report_period)
		return;
	report_left_ -= bits;
	if (report_left_ > 0)
		return;
	s32 rate = link_->tx_rate();
	s32 period = (rate > 0 ? rate : 28800) * cfg_.report_period / 100;
	report_left_ += period > 0 ? period : 1;
	if (report_left_ <= 0)
		report_left_ = period > 0 ? period : 1;
	u8 p[2];
	encode14(p, link_->tx_buffered());
	indicate(BNUM, p, 2);
}

/* v (0 to 16383) as <octnum0> <octnum1> (8.4/V.80): seven bits in
   each, above a 0 */
void SyncAccess::encode14(u8 *par, s32 v)
{
	if (v < 0)
		v = 0;
	if (v > 16383)
		v = 16383;
	par[0] = static_cast<u8>((v & 0x7f) << 1);
	par[1] = static_cast<u8>((v >> 7 & 0x7f) << 1);
}


/*
 *    transmit
 *
 */

/* an item for the line, after those waiting: an octet with zero
   insertion, or as it is (raw: flags, aborts, transparent octets) */
void SyncAccess::push(u8 octet, bool raw)
{
	s16 i = (pend_head_ + pend_n_) % PEND;
	pend_[i] = octet;
	pend_raw_[i] = raw;
	pend_n_++;
}

/* the oldest item waiting, into the bit queue: least significant bit
   first, a 0 after each fifth 1 in a row unless raw */
void SyncAccess::emit()
{
	u8 c = pend_[pend_head_];
	bool raw = pend_raw_[pend_head_];
	pend_head_ = (pend_head_ + 1) % PEND;
	pend_n_--;
	if (raw) {
		queue_bits(tx_, reverse_bits(c), 8);
		tx_ones_ = 0;
		return;
	}
	for (s16 i = 0; i < 8; i++, c >>= 1) {
		s16 b = c & 1;
		queue_bits(tx_, static_cast<u32>(b), 1);
		if (!b) {
			tx_ones_ = 0;
		} else if (++tx_ones_ == 5) {
			queue_bits(tx_, 0, 1);
			tx_ones_ = 0;
		}
	}
}

/* the next unit for the line: what waits, else the DTE's next octet or
   command, else the idle pattern; always something */
bool SyncAccess::tx_fill()
{
	u8 c;

	while (!pend_n_) {
		switch (next_input(c)) {
		case In::Data:
			data(c);
			break;
		case In::Done:
			break;
		case In::None:
			idle();
			break;
		}
	}
	emit();
	return true;
}

/* the DTE's next octet (Data) or an in-band command done (Done: there
   may be bits to send); None when it has nothing more now.  An EM and
   what follows it may come in separate reads */
SyncAccess::In SyncAccess::next_input(u8 &octet)
{
	u8 c;

	if (pair_n_) {
		pair_n_ = 0;
		octet = pair_;
		return In::Data;
	}
	for (;;) {
		if (link_->read_chars(&c, 1) != 1)
			return In::None;
		if (esc_ >= 2) {
			/* <EM><rate><tx><rx> */
			rate_[esc_ - 2] = c;
			if (esc_++ == 2)
				continue;
			esc_ = 0;
			link_->sync_command(SyncCommand::Rate, rate_[0],
					    rate_[1]);
			return In::Done;
		}
		if (esc_ == 1) {
			u8 o[2];
			s16 n;
			esc_ = 0;
			if (unshield(c, o, n)) {
				if (n == 2) {
					pair_ = o[1];
					pair_n_ = 1;
				}
				octet = o[0];
				return In::Data;
			}
			if (!command_char(c)) {
				/* 6.1.6/V.80: not a command, the octet is
				   data */
				link_->logf("v80: EM 0x%02x: no command.", c);
				octet = c;
				return In::Data;
			}
			if (c == RATE) {
				esc_ = 2;
				continue;
			}
			command(c);
			return In::Done;
		}
		if (c == EM || c == EM8) {
			esc_ = 1;
			continue;
		}
		if (cfg_.dc_flow && (c == DC1 || c == DC3))
			continue;
		octet = c;
		return In::Data;
	}
}

/* the DTE's in-band command <EM><code> (Table 9/V.80) */
void SyncAccess::command(u8 code)
{
	switch (code) {
	case MARK:
		if (framed_)
			enter_transparent();
		break;
	case FLAG:
		if (!framed_)
			enter_framed();
		if (in_frame_ && cfg_.crc == 1) {
			u16 f = static_cast<u16>(~tx_fcs_);
			push(static_cast<u8>(f), false);
			push(static_cast<u8>(f >> 8), false);
		} else if (in_frame_ && cfg_.crc == 2) {
			u32 f = ~tx_fcs_;
			for (s16 i = 0; i < 4; i++)
				push(static_cast<u8>(f >> 8 * i), false);
		}
		push(HDLC_FLAG, true);
		in_frame_ = false;
		need_flag_ = false;
		break;
	case ERR:
		if (!framed_)
			break;
		push(ONES, true);
		in_frame_ = false;
		need_flag_ = true;
		break;
	case HUNT:
		/* framed, or transparent with a SYN to hunt for (else
		   nothing, 8.8.1.2/V.80): the buffer flushed */
		if (framed_) {
			rx_reset(true);
			out_count_ = 0;
		} else if (cfg_.trans_idle == 1 || cfg_.trans_idle == 2) {
			syn_hunt();
			out_count_ = 0;
		}
		break;
	case RESUME:
		if (discard_) {
			u8 p[2];
			discard_ = false;
			encode14(p, discarded_);
			link_->logf("v80: resume, %" PRId32 " octets dropped.",
				    discarded_);
			indicate(UNUM, p, 2);
		}
		break;
	case ECS:
		link_->sync_command(SyncCommand::Ecs, 0, 0);
		break;
	case EOT:
		link_->sync_command(SyncCommand::Eot, 0, 0);
		break;
	case RTN:
		link_->sync_command(SyncCommand::Retrain, 0, 0);
		break;
	case RRN:
		link_->sync_command(SyncCommand::Reneg, 0, 0);
		break;
	default:
		link_->logf("v80: EM 0x%02x ignored.", code);
		break;
	}
}

/* one of the DTE's octets for the line: as it is (transparent), in a
   frame (framed: a flag before it after marks or an abort, the FCS
   begun with it), or dropped while waiting for <EM><resume> */
void SyncAccess::data(u8 octet)
{
	if (discard_) {
		discarded_++;
		return;
	}
	underrun_ = false;
	syn_second_ = false;
	if (!framed_) {
		push(octet, true);
		return;
	}
	if (!in_frame_) {
		if (need_flag_)
			push(HDLC_FLAG, true);
		need_flag_ = false;
		in_frame_ = true;
		tx_fcs_ = cfg_.crc == 2 ? FCS32_INIT : FCS16_INIT;
	}
	tx_fcs_ = cfg_.crc == 2 ? fcs32_add(tx_fcs_, octet) :
		fcs16_add(static_cast<u16>(tx_fcs_), octet);
	push(octet, false);
}

/* the DTE has nothing (8.8.1.1, 8.8.2.1/V.80): transparent, <syn1>
   (<syn1> <syn2> in turn with <trans_idle> 2), and <EM><under> the
   first time; framed, a frame cut short ends in
   an abort or a flag (no FCS) with <EM><under> (after the flag the
   DTE's octets are dropped until <EM><resume>), then flags or marks */
void SyncAccess::idle()
{
	if (!framed_) {
		if (!underrun_) {
			underrun_ = true;
			indicate(UNDER);
		}
		push(syn_second_ ? cfg_.syn2 : cfg_.syn1, true);
		syn_second_ = cfg_.trans_idle == 2 && !syn_second_;
		return;
	}
	if (in_frame_) {
		in_frame_ = false;
		if (cfg_.framed_un_ov) {
			push(HDLC_FLAG, true);
			discard_ = true;
			discarded_ = 0;
		} else {
			push(ONES, true);
			need_flag_ = true;
		}
		link_->logf("v80: tx underrun in a frame.");
		indicate(UNDER);
	}
	if (cfg_.framed_idle) {
		push(ONES, true);
		need_flag_ = true;
	} else {
		push(HDLC_FLAG, true);
		need_flag_ = false;
	}
}


/*
 *    receive
 *
 */

/* the line's bits: octets as they come (transparent, least significant
   bit first, after the SYN when hunting for it), or bit by bit to the
   HDLC receiver */
void SyncAccess::rx_scan()
{
	if (!framed_) {
		syn_scan();
		while (!syn_hunting_ && rx_.bit >= 8) {
			u8 c = reverse_bits(peek_octet(rx_));
			rx_.bit -= 8;
			put(c);
		}
		return;
	}
	while (rx_.bit)
		rx_bit(take_bit(rx_));
}

/* the transparent receiver's hunt: the bits one at a time until the
   last 8 are <syn1> (<trans_idle> 1) or the last 16 <syn1> then <syn2>
   (2, <syn1> first on the line); the SYN to the DTE, and the octets
   from there on aligned on it */
void SyncAccess::syn_scan()
{
	const s16 len = cfg_.trans_idle == 2 ? 16 : 8;
	const u16 want = len == 16 ?
		static_cast<u16>(cfg_.syn1 | cfg_.syn2 << 8) : cfg_.syn1;
	while (syn_hunting_ && rx_.bit) {
		u16 b = static_cast<u16>(take_bit(rx_));
		syn_reg_ = static_cast<u16>(syn_reg_ >> 1 | b << 15);
		if (syn_bits_ < 16)
			syn_bits_++;
		if (syn_bits_ < len ||
		    (len == 16 ? syn_reg_ : syn_reg_ >> 8) != want)
			continue;
		syn_hunting_ = false;
		link_->logf("v80: rx SYN found.");
		put(cfg_.syn1);
		if (len == 16)
			put(cfg_.syn2);
	}
}

/* the receiver hunts for a flag (hunt) or waits for a frame: nothing
   received yet */
void SyncAccess::rx_reset(bool hunt)
{
	hunt_ = hunt;
	rx_ones_ = 0;
	rx_octet_ = 0;
	rx_n_ = 0;
	rx_bits_ = 0;
	rx_count_ = 0;
	rx_fcs_ = cfg_.crc == 2 ? FCS32_INIT : FCS16_INIT;
	held_n_ = 0;
}

/* one bit of a frame: the 0 after five 1s dropped, 0 111111 0 a flag,
   seven 1s an abort; the sixth 1 waits to see which; the others are
   data, least significant first (a flag's first six bits come in as
   data too, and rx_flag() takes them out) */
void SyncAccess::rx_bit(s16 b)
{
	if (b) {
		if (++rx_ones_ == 7)
			rx_abort();
		if (rx_ones_ >= 6)
			return;
	} else {
		s16 ones = rx_ones_;
		rx_ones_ = 0;
		if (ones == 6) {
			rx_flag();
			return;
		}
		if (ones >= 5)
			return;
	}
	if (hunt_)
		return;
	rx_bits_++;
	rx_octet_ |= static_cast<u8>(b << rx_n_);
	if (++rx_n_ == 8) {
		rx_octet(rx_octet_);
		rx_octet_ = 0;
		rx_n_ = 0;
	}
}

/* an octet of a frame: to the DTE, but for the last two or four while
   the DCE checks the FCS (which they may be) */
void SyncAccess::rx_octet(u8 octet)
{
	rx_count_++;
	if (!cfg_.crc) {
		put(octet);
		return;
	}
	rx_fcs_ = cfg_.crc == 2 ? fcs32_add(rx_fcs_, octet) :
		fcs16_add(static_cast<u16>(rx_fcs_), octet);
	s16 len = cfg_.crc == 2 ? 4 : 2;
	if (held_n_ == len) {
		put(held_[0]);
		for (s16 i = 1; i < len; i++)
			held_[i - 1] = held_[i];
		held_n_--;
	}
	held_[held_n_++] = octet;
}

/* a flag: after the hunt, <EM><err> (8.8.2.2/V.80); else the end of the
   frame before it, if it had data: <EM><flag> for one of whole octets
   with a good FCS (if checked), <EM><err> for the rest.  Then the next
   frame */
void SyncAccess::rx_flag()
{
	if (hunt_) {
		indicate(ERR);
		rx_reset(false);
		return;
	}
	/* the flag's 0 and five 1s came in as data (only the 1s when its
	   0 closed the flag before) */
	s32 extra = rx_bits_ < 6 ? rx_bits_ : 6;
	if (rx_bits_ > extra) {
		bool good = rx_n_ == extra;
		if (cfg_.crc == 1)
			good = good && rx_count_ > 2 &&
				static_cast<u16>(rx_fcs_) == FCS16_GOOD;
		else if (cfg_.crc == 2)
			good = good && rx_count_ > 4 && rx_fcs_ == FCS32_GOOD;
		if (!good)
			link_->logf("v80: rx bad frame (%" PRId32 " octets, %d"
				    " bits).", rx_count_, rx_n_ - extra);
		indicate(good ? FLAG : ERR);
		link_->stats().rx_frames += good;
		link_->stats().rx_errors += !good;
	}
	rx_reset(false);
}

/* seven 1s: an abort, or the remote's marks after a frame:
   <EM><mark>, and the hunt (not reported while hunting) */
void SyncAccess::rx_abort()
{
	if (!hunt_)
		indicate(MARK);
	rx_reset(true);
	rx_ones_ = 7;
}


/*
 *    to the DTE
 *
 */

/* a received octet to the DTE, shielded if it is EM (either form) or,
   with the DTE's DC1/DC3 flow control, DC1 or DC3 */
void SyncAccess::put(u8 octet)
{
	static const u8 single[4] = { T1, T2, T3, T4 };
	for (s16 i = 0; i < 4; i++)
		if (octet == shielded[i] && (i < 2 || cfg_.shield_dc)) {
			u8 b[2] = { EM, single[i] };
			put_raw(b, 2);
			return;
		}
	put_raw(&octet, 1);
}

/* the indication <EM><code> with its n parameter octets to the DTE */
void SyncAccess::indicate(u8 code, const u8 *par, s16 n)
{
	u8 b[4] = { EM, code, 0, 0 };
	for (s16 i = 0; i < n && i < 2; i++)
		b[2 + i] = par[i];
	put_raw(b, static_cast<s16>(2 + (n < 2 ? n : 2)));
}

/* n octets into the ring for the DTE, all or none; dropped while not
   online (online command state discards what comes, 8.8.3/V.80).  The
   first ones to fit after some were lost: <EM><rover> before them */
void SyncAccess::put_raw(const u8 *p, s16 n)
{
	const s16 size = static_cast<s16>(sizeof(out_));
	if (!link_->online())
		return;
	if (rover_ && out_count_ + 2 + n <= size) {
		rover_ = false;
		out_[(out_head_ + out_count_++) % size] = EM;
		out_[(out_head_ + out_count_++) % size] = ROVER;
	}
	if (rover_ || out_count_ + n > size) {
		if (!rover_)
			link_->logf("v80: rx overrun.");
		rover_ = true;
		return;
	}
	for (s16 i = 0; i < n; i++)
		out_[(out_head_ + out_count_++) % size] = p[i];
}

/* the ring to the DTE, as much as it takes now; emptied when not online */
void SyncAccess::flush()
{
	const s16 size = static_cast<s16>(sizeof(out_));
	if (!link_->online()) {
		out_count_ = 0;
		return;
	}
	while (out_count_) {
		s16 run = size - out_head_;
		if (run > out_count_)
			run = out_count_;
		s16 w = link_->write_chars(out_ + out_head_, run);
		if (w <= 0)
			break;
		out_head_ = static_cast<s16>((out_head_ + w) % size);
		out_count_ = static_cast<s16>(out_count_ - w);
	}
}

} // namespace dlink
