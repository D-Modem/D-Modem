/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    ppp_sync.h - PPP on an ISDN B-channel (RFC 1618, CAPI's B2 protocol
 *                 5): the frames in synchronous HDLC on the line, in
 *                 the asynchronous HDLC-like framing of RFC 1662 to the
 *                 DTE, where an ordinary pppd runs (the "async to sync
 *                 conversion" of ISDN terminal adapters)
 *
 *    From the DTE: frames between flags (7Eh), with control escapes
 *    (7Dh, the next octet XOR 20h) undone, their FCS-16 checked and
 *    taken off; the line's HDLC framer adds its own.  To the DTE: each
 *    good frame from the line with its FCS-16, flags, and escapes for
 *    7Eh, 7Dh and every control character (whatever ACCM pppd agreed,
 *    an escape too many is always undone).  Bad frames are dropped,
 *    as the other end's PPP retries.
 *
 */

#ifndef DMODEM_LINK_PPP_SYNC_H
#define DMODEM_LINK_PPP_SYNC_H

#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

enum {
	PPP_MAX_FRAME = 2048,   /* the longest frame either way (without
				   FCS): an MRU of 1500 and more */
};

class PppSync : public Framer {
public:
	/* a call over link: nothing held */
	void start(Link *link);
	/* the frame for the DTE that it has not all taken: on with it;
	   true when it has it all */
	bool push_rest();

	/* the HDLC framer's side */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;

private:
	void dte_octet(u8 c);

	Link *link_ = nullptr;
	/* from the DTE: the frame being collected (with its FCS), an
	   escape seen, a frame ready to go */
	Frame tx_frame_ = {};
	u8 tx_buf_[PPP_MAX_FRAME + 2];
	s16 tx_count_ = 0;
	bool escape_ = false, discard_ = false, ready_ = false;
	/* the DTE's characters read but not yet looked at */
	u8 in_[256];
	s16 in_head_ = 0, in_count_ = 0;
	/* to the DTE: a frame in async framing, what is left of it */
	u8 out_[2 * (PPP_MAX_FRAME + 2) + 2];
	s16 out_head_ = 0, out_count_ = 0;
};

} // namespace dlink

#endif
