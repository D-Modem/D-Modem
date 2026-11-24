/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    aft.cpp - Hayes AFT framing (see aft.h)
 *
 */

#include "aft.h"
#include "link.h"

namespace dlink {

/* the flag and the control escape (RFC 1662, 4.2) */
static const u8 AFT_FLAG = 0x7e, AFT_ESC = 0x7d, AFT_XOR = 0x20;

/* a call: no frame going out, the receiver between frames, the escape
   map with 7Dh and 7Eh only */
void Aft::start(Link *link)
{
	framer_start(link);
	tx_frame_ = nullptr;
	tx_state_ = Tx::Idle;
	tx_esc_ = false;
	rx_frame_ = {};
	rx_frame_.buf = rx_buf_;
	rx_frame_.size = sizeof(rx_buf_);
	rx_fcs_ = FCS16_INIT;
	rx_esc_ = rx_drop_ = false;
	set_options(0);
}

/* the escape map from the options (bank 4 6D5Fh): 7Dh and 7Eh always;
   with AFT_OPT_CTRL also 00h-1Fh, 7Fh, 80h-9Fh and FFh */
void Aft::set_options(u8 opts)
{
	ctrl_ = (opts & AFT_OPT_CTRL) != 0;
	for (auto &m : map_)
		m = 0;
	for (s16 c = 0; c < 256; c++) {
		const bool esc = c == AFT_FLAG || c == AFT_ESC ||
			(ctrl_ && (c < 0x20 || c == 0x7f ||
				   (c >= 0x80 && c < 0xa0) || c == 0xff));
		if (esc)
			map_[c >> 3] = static_cast<u8>(map_[c >> 3] |
						       1 << (c & 7));
	}
}

/* a character to send: start bit, 8 data bits (the least significant
   first), stop bit */
void Aft::tx_char(u8 ch)
{
	tx_.data <<= 10;
	tx_.data |= static_cast<u32>(reverse_bits(ch)) << 1 | 1;
	tx_.bit += 10;
}

/* an octet of the frame: as it is, or 7Dh now and the octet XOR 20h
   as the next character */
void Aft::tx_octet(u8 c)
{
	if (!escaped(c)) {
		tx_char(c);
		return;
	}
	tx_char(AFT_ESC);
	tx_esc_ = true;
	tx_esc_char_ = static_cast<u8>(c ^ AFT_XOR);
}

/* the transmitter, a character at a time: the framer's next frame
   after a flag, its octets, the complement of the FCS (low octet
   first), the closing flag; tx_complete() once the closing flag is
   queued; false when there is no frame (marks) */
bool Aft::tx_fill()
{
	if (tx_esc_) {
		tx_esc_ = false;
		tx_char(tx_esc_char_);
		return true;
	}
	switch (tx_state_) {
	case Tx::Idle:
		if (!framer_ || !(tx_frame_ = framer_->next_tx_frame()))
			return false;
		link_->stats().tx_frames++;
		tx_frame_->count = 0;
		tx_fcs_ = FCS16_INIT;
		tx_state_ = Tx::Body;
		tx_char(AFT_FLAG);
		return true;
	case Tx::Body:
		if (tx_frame_->count < tx_frame_->size) {
			const u8 c = tx_frame_->buf[tx_frame_->count++];
			tx_fcs_ = fcs16_add(tx_fcs_, c);
			tx_octet(c);
			return true;
		}
		tx_fcs_ = static_cast<u16>(~tx_fcs_);
		tx_fcs_left_ = 2;
		tx_state_ = Tx::Fcs;
		/* fall through */
	case Tx::Fcs:
		if (tx_fcs_left_) {
			tx_octet(static_cast<u8>(tx_fcs_left_ == 2 ? tx_fcs_ :
						 tx_fcs_ >> 8));
			if (--tx_fcs_left_ == 0)
				tx_state_ = Tx::End;
			return true;
		}
		tx_state_ = Tx::End;
		/* fall through */
	case Tx::End: {
		tx_char(AFT_FLAG);
		Frame *f = tx_frame_;
		tx_frame_ = nullptr;
		tx_state_ = Tx::Idle;
		framer_->tx_complete(f);
		return true;
	}
	}
	return false;
}

/* the rest of the n items with no frame to send: what is left of the
   last character, then marks; how many */
s32 Aft::tx_idle(s16 nbits, u8 mask, u8 *&buf, s32 &n)
{
	s32 ret = 0;

	if (tx_.bit && n > 0) {
		*buf++ = static_cast<u8>(tx_.data << (nbits - tx_.bit) |
					 ((1 << (nbits - tx_.bit)) - 1)) & mask;
		ret++;
		n--;
		tx_.bit = 0;
	}
	for (; n > 0; n--, ret++)
		*buf++ = mask;
	return ret;
}

/* the receiver: characters after marks (a start bit, 8 data bits, the
   stop bit not checked) */
void Aft::rx_scan()
{
	for (;;) {
		while (rx_.bit && rx_.data & (UINT32_C(1) << (rx_.bit - 1)))
			rx_.bit--;
		if (rx_.bit < 10)
			break;
		const u8 ch = static_cast<u8>(rx_.data >> (rx_.bit - 9));
		rx_.bit -= 10;
		rx_char(reverse_bits(ch));
	}
}

/* the end of a frame at a flag: a good one (4 octets or more, the FCS
   good) to the framer without its FCS, a bad one counted; 7Dh before
   the flag aborts the frame; then a new frame */
void Aft::rx_end()
{
	const s16 n = rx_frame_.count;
	if (rx_esc_) {
		link_->logf("aft: frame aborted.");
	} else if (!rx_drop_ && n >= 4 && rx_fcs_ == FCS16_GOOD) {
		rx_frame_.count = static_cast<s16>(n - 2);
		link_->stats().rx_frames++;
		if (framer_)
			framer_->rx_complete(&rx_frame_);
	} else if (!rx_drop_ && n > 0) {
		link_->stats().rx_errors++;
		if (framer_)
			framer_->rx_error();
	}
	rx_frame_.count = 0;
	rx_fcs_ = FCS16_INIT;
	rx_esc_ = rx_drop_ = false;
}

/* a received character: a flag ends the frame, 7Dh escapes the next
   one; with AFT_OPT_CTRL a character of the escape map that comes
   without 7Dh is ignored; a frame past the buffer is dropped */
void Aft::rx_char(u8 ch)
{
	if (ch == AFT_FLAG) {
		rx_end();
		return;
	}
	if (rx_drop_)
		return;
	if (ch == AFT_ESC) {
		rx_esc_ = true;
		return;
	}
	if (rx_esc_) {
		ch = static_cast<u8>(ch ^ AFT_XOR);
		rx_esc_ = false;
	} else if (ctrl_ && escaped(ch)) {
		return;
	}
	if (rx_frame_.count == rx_frame_.size) {
		link_->logf("aft: rx frame overflow.");
		rx_drop_ = true;
		return;
	}
	rx_frame_.buf[rx_frame_.count++] = ch;
	rx_fcs_ = fcs16_add(rx_fcs_, ch);
}

} // namespace dlink
