/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    aft.h - Hayes AFT ("asynchronous framing technique"): the frames of
 *            LAP-B or LAP-M on start-stop characters, as the Hayes
 *            V-series Smartmodem 9600 sends them for the "/AFT" result
 *            codes 73, 76 and 79 (firmware 1.4, bank 4 6C17h to 6D5Dh,
 *            fixed ROM 20BDh to 21C4h)
 *
 *    This is the method of RFC 1662 (PPP in HDLC-like framing):
 *
 *    - Each octet goes out as a start-stop character: a start bit, 8
 *      data bits (the least significant first), a stop bit.
 *    - A frame is between flags 7Eh.  The control escape is 7Dh: an
 *      escaped octet goes out as 7Dh, then the octet XOR 20h.
 *    - 7Dh and 7Eh are always escaped.  With the option AFT_OPT_CTRL
 *      (AFT P2 bit 1), 00h-1Fh, 7Fh, 80h-9Fh and FFh are escaped too
 *      (the map at bank 4 6D5Fh, which includes XON and XOFF with and
 *      without bit 7).  The receiver then ignores these characters when
 *      they come without the escape (flow control from the DCE).
 *    - The FCS is the FCS-16 of V.42 (CRC-CCITT, the register reflected,
 *      start FFFFh).  Its complement goes out low octet first, escaped
 *      as the data is.
 *    - 7Dh 7Eh aborts the frame.
 *
 *    The 7-bit mode (AFT P2 bit 2: a parity bit from the table at A032h
 *    in bit 7 of each character) is not done: the firmware analysis does
 *    not show how 8-bit data goes through it (docs/hayes-details.md).
 *
 */

#ifndef DMODEM_LINK_AFT_H
#define DMODEM_LINK_AFT_H


#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

class Aft : public BitFramer<Aft> {
public:
	/* a call: no frame going out, the receiver between frames, only
	   7Dh and 7Eh escaped */
	void start(Link *link);
	/* the framer: its frames go out, and received frames go to it */
	void set_framer(Framer *f) { framer_ = f; }
	/* the escape options, the OR of the two AFT P2 values
	   (AFT_OPT_CTRL; hayes_xid.h) */
	void set_options(u8 opts);
	/* the octet c goes out escaped */
	bool escaped(u8 c) const { return (map_[c >> 3] >> (c & 7)) & 1; }

private:
	friend class BitFramer<Aft>;
	enum class Tx { Idle, Body, Fcs, End };
	bool tx_fill();
	s32 tx_idle(s16 nbits, u8 mask, u8 *&buf, s32 &n);
	void rx_scan();
	void tx_char(u8 ch);
	void tx_octet(u8 c);
	void rx_char(u8 ch);
	void rx_end();

	Framer *framer_ = nullptr;
	/* the escape map: bit (c & 7) of map_[c >> 3] */
	u8 map_[32] = {};
	bool ctrl_ = false;
	Frame *tx_frame_ = nullptr;
	Tx tx_state_ = Tx::Idle;
	u16 tx_fcs_ = 0;
	s16 tx_fcs_left_ = 0;
	/* an escaped octet's second character, to go after the 7Dh */
	bool tx_esc_ = false;
	u8 tx_esc_char_ = 0;
	Frame rx_frame_ = {};
	/* address, control, up to 2048 octets of information, the FCS */
	u8 rx_buf_[2 + 2048 + 2];
	u16 rx_fcs_ = FCS16_INIT;
	bool rx_esc_ = false;
	/* the frame is too long or aborted: dropped up to the next flag */
	bool rx_drop_ = false;
};

} // namespace dlink

#endif
