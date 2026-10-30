/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    ppp_sync.cpp - PPP's frames between the DTE's asynchronous HDLC-like
 *                   framing (RFC 1662 4) and the line's synchronous HDLC
 *                   (RFC 1662 5, RFC 1618)
 *
 */

#include <cstring>

#include "dm_types.h"
#include "link.h"

namespace dlink {

static const u8 FLAG = 0x7e, ESC = 0x7d;

/* a call: nothing collected, nothing held for the DTE; frames go as
   soon as the DTE sends them */
void PppSync::start(Link *link)
{
	link_ = link;
	tx_frame_ = {};
	tx_frame_.buf = tx_buf_;
	tx_count_ = 0;
	escape_ = discard_ = ready_ = false;
	in_head_ = in_count_ = 0;
	out_head_ = out_count_ = 0;
	link->hdlc().set_framer(this, true);
	link->push_pending = false;
}

/* one octet of the DTE's: a flag ends the frame before it (a good one
   is ready, without its FCS; one too short or with a bad FCS is
   dropped), an escape changes the next octet, the rest are the frame's
   (one too long is dropped at its end) */
void PppSync::dte_octet(u8 c)
{
	if (c == FLAG) {
		if (tx_count_ && !discard_ && !escape_) {
			u16 fcs = FCS16_INIT;
			for (s16 i = 0; i < tx_count_; i++)
				fcs = fcs16_add(fcs, tx_buf_[i]);
			if (tx_count_ >= 4 && fcs == FCS16_GOOD) {
				tx_frame_.size = static_cast<s16>(tx_count_ - 2);
				ready_ = true;
			} else
				link_->logf("err: ppp: a frame of %d octets "
					    "from the DTE with a bad FCS.",
					    tx_count_);
		} else if (discard_)
			link_->logf("err: ppp: a frame from the DTE over %d "
				    "octets dropped.", PPP_MAX_FRAME);
		tx_count_ = 0;
		escape_ = discard_ = false;
		return;
	}
	if (c == ESC) {
		escape_ = true;
		return;
	}
	if (escape_) {
		c ^= 0x20;
		escape_ = false;
	}
	if (discard_)
		return;
	if (tx_count_ == static_cast<s16>(sizeof(tx_buf_))) {
		discard_ = true;
		return;
	}
	tx_buf_[tx_count_++] = c;
}

/* the framer's next frame: the DTE's characters looked at until a
   frame is ready (those after it kept); NULL while none is */
Frame *PppSync::next_tx_frame()
{
	while (!ready_) {
		if (!in_count_) {
			in_head_ = 0;
			in_count_ = link_->read_chars(in_, sizeof(in_));
			if (in_count_ <= 0) {
				in_count_ = 0;
				return nullptr;
			}
		}
		while (in_count_ && !ready_) {
			in_count_--;
			dte_octet(in_[in_head_++]);
		}
	}
	return &tx_frame_;
}

/* the frame is out: the next one may be collected (into the same
   buffer) */
void PppSync::tx_complete(Frame *f)
{
	(void)f;
	ready_ = false;
}

/* a good frame from the line: to the DTE in async framing, with its
   FCS; dropped while the last one is not all taken */
void PppSync::rx_complete(Frame *f)
{
	if (out_count_ && !push_rest()) {
		link_->logf("err: ppp: the DTE is busy: a frame of %d octets "
			    "dropped.", f->count);
		return;
	}
	if (f->count > PPP_MAX_FRAME) {
		link_->logf("err: ppp: a frame of %d octets dropped.",
			    f->count);
		return;
	}
	u16 fcs = FCS16_INIT;
	s16 n = 0;
	auto put = [&](u8 c) {
		if (c == FLAG || c == ESC || c < 0x20) {
			out_[n++] = ESC;
			c ^= 0x20;
		}
		out_[n++] = c;
	};
	out_[n++] = FLAG;
	for (s16 i = 0; i < f->count; i++) {
		fcs = fcs16_add(fcs, f->buf[i]);
		put(f->buf[i]);
	}
	fcs = static_cast<u16>(~fcs);
	put(static_cast<u8>(fcs));
	put(static_cast<u8>(fcs >> 8));
	out_[n++] = FLAG;
	out_head_ = 0;
	out_count_ = n;
	if (!push_rest())
		link_->push_pending = true;
}

/* the frame for the DTE: as much as it takes */
bool PppSync::push_rest()
{
	while (out_count_) {
		s16 w = link_->write_chars(out_ + out_head_, out_count_);
		if (w <= 0)
			return false;
		out_head_ += w;
		out_count_ -= w;
	}
	link_->push_pending = false;
	return true;
}

} // namespace dlink
