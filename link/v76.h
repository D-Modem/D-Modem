/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v76.h - V.70's multiplexer (V.76) and its control entity (V.75),
 *            for DSVD's data channel
 *
 *    V.76 shares one HDLC link among data link connections (DLCs), each
 *    known by the DLCI in its frames' address octet.  A DLC in error
 *    recovery mode (ERM) is V.42's LAPM with that DLCI; one in
 *    unacknowledged mode (UNERM) carries UI frames.  V.75 opens a DLC
 *    with the SABME that starts it, H.245's OpenLogicalChannel in its
 *    information field, answered by a UA with OpenLogicalChannelAck or
 *    a DM with OpenLogicalChannelReject; the DISC that ends it carries
 *    CloseLogicalChannel (h245.h).
 *
 *    Here the DLCs are the one data channel (ERM, the DTE's
 *    characters, a Lapm of the Link's): the initiator (the station
 *    V.8 bis made it) opens it on DLCI 0 after V.76's flags; the
 *    responder takes the first data channel the remote opens.  A
 *    channel of another kind is refused, a capability set (XID) is
 *    answered with TerminalCapabilitySetReject (the defaults hold), a
 *    frame for a DLC that is not open is dropped (DISC: DM).
 *
 */

#ifndef DMODEM_LINK_V76_H
#define DMODEM_LINK_V76_H


#include <vector>

#include "dm_types.h"
#include "h245.h"
#include "hdlc.h"

namespace dlink {

class Lapm;

class Mux : public Framer {
public:
	/* a call on link, its data DLC lapm (initialised by the Link) */
	void init(Link *link, Lapm *lapm);
	/* the data pump has connected: our frames through Hdlc; the
	   initiator opens the data channel after its flags (the Link's
	   bit timer: Link::Timer::Mux, opening()) */
	void start(bool initiator);
	/* the initiator's flags are out: the data channel's SABME */
	void opening();
	/* we hang up: the data channel's DISC (CloseLogicalChannel) */
	void disconnect();
	/* the data channel's DLCI, -1 while it has none */
	s16 data_dlci() const { return dlci_; }

	/* the HDLC framer's side */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;

private:
	enum { CTRL_FRAMES = 4, CTRL_SIZE = 2 + 64 };
	struct Slot {
		Frame frame;
		u8 buf[CTRL_SIZE];
	};
	bool command(u8 addr) const;
	u8 addr(u8 dlci, bool cmd) const;
	void tx_unnum(u8 dlci, bool cmd, u8 ctrl,
		      const std::vector<u8> &ud = {});
	void rx_open(Frame *f, u8 dlci);
	void rx_xid(Frame *f, u8 dlci);
	void log_msg(const char *dir, u8 dlci, const h245::Msg &m);

	Link *link_ = nullptr;
	Lapm *lapm_ = nullptr;
	bool initiator_ = false;
	s16 dlci_ = -1;                 /* the data channel's DLC */
	u16 lcn_ = 1;                   /* and its logical channel number */
	/* our own U frames (DLCs that are not open): a ring */
	Slot slot_[CTRL_FRAMES];
	s16 head_ = 0, tail_ = 0;
};

} // namespace dlink

#endif
