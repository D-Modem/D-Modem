/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v76.h - V.70's multiplexer (V.76) and its control entity (V.75),
 *            for DSVD's data and voice channels
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
 *    With a voice path (Owner::voice()) there is a voice channel too:
 *    G.729 Annex A in UNERM, a 10 ms frame (10 octets) in each UI
 *    frame (V.70's audio blocking factor 1), V.75's audio header
 *    (clause 9) both ways (the initiator asks for it; the responder
 *    takes what the remote asks).  The initiator opens it on DLCI 1
 *    with the data channel (again if its SABME is not answered); the
 *    responder takes the first one opened.  A frame goes out every
 *    10 ms of the line's bits once the owner has its 80 samples
 *    (Owner::voice_in()), before the data channel's; what comes in is
 *    decoded to the owner (voice_out()), a lost frame (the audio
 *    header's sequence number) concealed.  With the audio header we
 *    send, silence is G.729 Annex B's: a SID frame (2 octets, the
 *    header's silence and SID bits set) when the noise changes, and
 *    nothing between, so the data channel has the line; after a
 *    silence frame the far end's comfort noise is made every 10 ms
 *    until speech comes.
 *
 */

#ifndef DMODEM_LINK_V76_H
#define DMODEM_LINK_V76_H


#include <vector>

#include "dm_types.h"
#include "g729a.h"
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
	/* we hang up: the voice channel's DISC, then the data channel's
	   (CloseLogicalChannel) */
	void disconnect();
	/* the call is over: an open voice channel closed (the owner
	   hears) */
	void end() { voice_closed(); }
	/* the data channel's DLCI, -1 while it has none */
	s16 data_dlci() const { return dlci_; }
	/* the voice channel's DLCI, -1 while it has none (or it is not
	   open yet) */
	s16 voice_dlci() const { return vstate_ == VState::Open ? vdlci_ : -1; }
	/* bits have gone out (the voice channel's pace and its SABME's
	   timer) */
	void clock(s32 bits);

	/* the HDLC framer's side */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;

private:
	enum { CTRL_FRAMES = 4, CTRL_SIZE = 2 + 64,
	       VOICE_SIZE = 2 + 1 + g729::FRAME_OCTETS };
	enum class VState : u8 { Closed, Opening, Open };
	struct Slot {
		Frame frame;
		u8 buf[CTRL_SIZE];
	};
	bool command(u8 addr) const;
	u8 addr(u8 dlci, bool cmd) const;
	void tx_unnum(u8 dlci, bool cmd, u8 ctrl,
		      const std::vector<u8> &ud = {});
	void rx_open(Frame *f, u8 dlci);
	bool rx_voice(Frame *f, u8 dlci, const h245::Msg &m, u8 *cause);
	void rx_xid(Frame *f, u8 dlci);
	void open_voice();
	void voice_frame(Frame *f, u8 ctrl, bool cmd);
	void voice_open();
	void voice_closed();
	Frame *next_voice();
	void log_msg(const char *dir, u8 dlci, const h245::Msg &m);

	Link *link_ = nullptr;
	Lapm *lapm_ = nullptr;
	bool initiator_ = false;
	s16 dlci_ = -1;                 /* the data channel's DLC */
	u16 lcn_ = 1;                   /* and its logical channel number */
	/* our own U frames (DLCs that are not open): a ring */
	Slot slot_[CTRL_FRAMES];
	s16 head_ = 0, tail_ = 0;

	/* the voice channel */
	VState vstate_ = VState::Closed;
	s16 vdlci_ = -1;
	u16 vlcn_ = 2;
	bool vhdr_tx_ = false, vhdr_rx_ = false;  /* V.75's audio header */
	u8 vseq_tx_ = 0;
	s16 vseq_rx_ = -1;              /* the next expected, -1 unknown */
	s32 vcredit_ = 0;               /* bits towards the next frame */
	s32 vrx_credit_ = 0;            /* and the far end's, in silence */
	bool vsil_ = false;             /* the far end is silent */
	s32 vtimer_ = 0;                /* our SABME's answer, in bits */
	s16 vtries_ = 0;
	g729::Encoder enc_;
	g729::Decoder dec_;
	s16 mic_[g729::FRAME];
	s16 mic_n_ = 0;
	Frame vframe_;
	u8 vbuf_[VOICE_SIZE];
	bool vbusy_ = false;            /* vframe_ is with Hdlc */
};

} // namespace dlink

#endif
