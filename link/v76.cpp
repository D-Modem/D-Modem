/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v76.cpp - V.76's multiplexer and V.75's control of its DLCs, for
 *              DSVD's data channel (v76.h)
 *
 */

#include <cstdio>

#include "link.h"
#include "v76.h"

namespace dlink {

/* the U frames' control octets without P/F (6.2.3/V.76, as LAPM's) */
static const u8 U_SABME = 0x6f, U_DM = 0x0f, U_DISC = 0x43, U_UA = 0x63,
	U_XID = 0xaf;
static const u8 PF = 0x10;

enum {
	FLAG_BITS = 384,        /* the initiator's flags before its first
				   frame (at least 16, clause 7/V.76) */
	DLCI_FIRST = 0,         /* the initiator's first DLCI (Annex C) */
};

/* a call on link: no data channel, our ring empty */
void Mux::init(Link *link, Lapm *lapm)
{
	link_ = link;
	lapm_ = lapm;
	dlci_ = -1;
	lcn_ = 1;
	head_ = tail_ = 0;
	for (Slot &s : slot_) {
		s.frame = Frame();
		s.frame.buf = s.buf;
	}
}

/* (see v76.h) */
void Mux::start(bool initiator)
{
	initiator_ = initiator;
	dlci_ = -1;
	head_ = tail_ = 0;
	link_->logf("v76: %s.", initiator ? "initiator" : "responder");
	link_->hdlc().set_framer(this, true);
	if (initiator)
		link_->set_timer(FLAG_BITS, Link::Timer::Mux);
}

/* the data channel opened by us (unless the remote opened one first):
   DLCI 0, ERM both ways with V.76's defaults (REJ, k 15, N401 128,
   16 bit FCS), V.14's buffered characters, the line's rate */
void Mux::opening()
{
	if (dlci_ >= 0)
		return;
	dlci_ = DLCI_FIRST;
	lcn_ = 1;
	h245::Msg m;
	m.type = h245::Type::Olc;
	m.ch.lcn = lcn_;
	m.ch.port = 0;
	m.ch.kind = h245::Kind::Data;
	m.ch.proto = h245::Proto::V14Buffered;
	m.ch.max_rate = static_cast<u32>(link_->tx_rate() / 100);
	lapm_->set_dlc(static_cast<u8>(dlci_), initiator_, LAPM_DFLT_WIN,
		       LAPM_DFLT_WIN, LAPM_DFLT_INFO, LAPM_DFLT_INFO);
	lapm_->set_user_data(true, h245::user_data(m));
	log_msg("out", static_cast<u8>(dlci_), m);
	lapm_->open();
}

/* (see v76.h): nothing while no channel is open or opening */
void Mux::disconnect()
{
	if (dlci_ < 0)
		return;
	h245::Msg m;
	m.type = h245::Type::Clc;
	m.lcn = lcn_;
	lapm_->set_user_data(true, h245::user_data(m));
	log_msg("out", static_cast<u8>(dlci_), m);
	lapm_->disconnect();
}

/* whether a frame with address octet a is a command of the remote's:
   the initiator's commands have C/R 1, the responder's 0 (6.1.2/V.76) */
bool Mux::command(u8 a) const
{
	return ((a & 2) != 0) == !initiator_;
}

/* the address octet of our command (cmd) or response on dlci */
u8 Mux::addr(u8 dlci, bool cmd) const
{
	return static_cast<u8>((dlci & 0x3f) << 2 | (cmd == initiator_ ? 2 : 0) |
			       1);
}

/* a U frame of ours on a DLC that is not open (ctrl with its P/F),
   with user data ud: on the ring, or dropped when it is full */
void Mux::tx_unnum(u8 dlci, bool cmd, u8 ctrl, const std::vector<u8> &ud)
{
	const s16 next = static_cast<s16>((tail_ + 1) % CTRL_FRAMES);
	if (next == head_ || ud.size() > CTRL_SIZE - 2) {
		link_->logf("err: v76: control queue full: frame dropped.");
		return;
	}
	Frame &f = slot_[tail_].frame;
	f.buf[0] = addr(dlci, cmd);
	f.buf[1] = ctrl;
	for (size_t i = 0; i < ud.size(); i++)
		f.buf[2 + i] = ud[i];
	f.size = static_cast<s16>(2 + ud.size());
	tail_ = next;
}

/* the framer's next frame: ours first, then the data channel's */
Frame *Mux::next_tx_frame()
{
	if (head_ != tail_) {
		Frame *f = &slot_[head_].frame;
		head_ = static_cast<s16>((head_ + 1) % CTRL_FRAMES);
		return f;
	}
	return dlci_ >= 0 ? lapm_->next_tx_frame() : nullptr;
}

/* a frame is out: the data channel's to it */
void Mux::tx_complete(Frame *f)
{
	for (const Slot &s : slot_)
		if (f == &s.frame)
			return;
	lapm_->tx_complete(f);
}

/* a good frame from Hdlc: an XID command answered here on any DLC; the
   data channel's frames to it (the UA or DM of its SABME, the remote's
   DISC, with V.75's messages read for the log and answered); a SABME
   opening another DLC to rx_open(); a DISC on a DLC that is not open
   answered with DM (7.4/V.76); anything else on one dropped.  Two-
   octet addresses (DLCIs over 63) are not ours */
void Mux::rx_complete(Frame *f)
{
	if (f->count < 2)
		return;
	const u8 a = f->buf[0];
	if (!(a & 1)) {
		link_->logf("err: v76: two-octet address: frame dropped.");
		return;
	}
	const u8 dlci = static_cast<u8>(a >> 2);
	const bool cmd = command(a);
	const u8 ctrl = f->buf[1];
	const bool unnum = (ctrl & 3) == 3;
	const u8 u = static_cast<u8>(ctrl & ~PF);
	if (unnum && u == U_XID) {
		if (cmd)
			rx_xid(f, dlci);
		return;
	}
	if (dlci_ >= 0 && dlci == dlci_) {
		if (unnum && f->count > 2 &&
		    (u == U_SABME || u == U_DISC || u == U_UA || u == U_DM)) {
			h245::Msg m;
			if (h245::from_user_data(f->buf + 2,
						 static_cast<size_t>(f->count - 2),
						 &m))
				log_msg("in", dlci, m);
		}
		if (unnum && cmd && (u == U_SABME || u == U_DISC)) {
			h245::Msg m;
			m.type = u == U_SABME ? h245::Type::OlcAck :
				h245::Type::ClcAck;
			m.lcn = lcn_;
			if (u == U_DISC)
				log_msg("out", dlci, m);
			lapm_->set_user_data(false, h245::user_data(m));
		}
		lapm_->rx_complete(f);
		return;
	}
	if (!unnum || !cmd)
		return;
	if (u == U_SABME)
		rx_open(f, dlci);
	else if (u == U_DISC)
		tx_unnum(dlci, false, static_cast<u8>(U_DM | (ctrl & PF)));
}

/* the remote's SABME on dlci, opening a DLC (8.1/V.75): the first data
   channel (ERM both ways, a 16 bit FCS) becomes ours, the UA with
   OpenLogicalChannelAck; anything else gets a DM with
   OpenLogicalChannelReject */
void Mux::rx_open(Frame *f, u8 dlci)
{
	h245::Msg m;
	const bool ok = h245::from_user_data(f->buf + 2,
					     static_cast<size_t>(f->count - 2),
					     &m);
	if (m.type == h245::Type::Olc)
		log_msg("in", dlci, m);
	const h245::Channel &c = m.ch;
	u8 cause = h245::UNSPECIFIED;
	bool accept = false;
	if (!ok || m.type != h245::Type::Olc)
		cause = h245::UNKNOWN_TYPE;
	else if (c.kind != h245::Kind::Data)
		cause = h245::TYPE_NOT_SUPPORTED;
	else if (dlci_ >= 0)
		cause = h245::TYPE_NOT_AVAILABLE;
	else if (!c.fwd.erm || c.fwd.crc != 16 ||
		 (c.has_rev && (!c.rev.erm || c.rev.crc != 16)))
		cause = h245::UNSUITABLE_REVERSE;
	else
		accept = true;
	if (!accept) {
		h245::Msg r;
		r.type = h245::Type::OlcReject;
		r.lcn = m.type == h245::Type::Olc ? m.lcn : 1;
		r.cause = cause;
		log_msg("out", dlci, r);
		tx_unnum(dlci, false, static_cast<u8>(U_DM | (f->buf[1] & PF)),
			 h245::user_data(r));
		return;
	}
	/* the opener's forward direction is what we receive, its reverse
	   what we send */
	dlci_ = dlci;
	lcn_ = m.lcn;
	const h245::V76Params dflt;
	const h245::V76Params &tx = c.has_rev ? c.rev : dflt;
	lapm_->set_dlc(dlci, initiator_, tx.window, c.fwd.window,
		       static_cast<s16>(tx.n401), static_cast<s16>(c.fwd.n401));
	h245::Msg r;
	r.type = h245::Type::OlcAck;
	r.lcn = lcn_;
	log_msg("out", dlci, r);
	lapm_->set_user_data(false, h245::user_data(r));
	lapm_->rx_complete(f);
}

/* the remote's XID command on dlci (L-SETPARM, 8.3/V.75): a
   TerminalCapabilitySet is answered with TerminalCapabilitySetReject,
   the defaults staying (6.2.1/V.70); anything else is not answered */
void Mux::rx_xid(Frame *f, u8 dlci)
{
	h245::Msg m;
	if (!h245::from_user_data(f->buf + 2, static_cast<size_t>(f->count - 2),
				  &m) || m.type != h245::Type::Tcs)
		return;
	log_msg("in", dlci, m);
	h245::Msg r;
	r.type = h245::Type::TcsReject;
	r.seq = m.seq;
	log_msg("out", dlci, r);
	tx_unnum(dlci, false, static_cast<u8>(U_XID | (f->buf[1] & PF)),
		 h245::user_data(r));
}

/* V.75's message m on dlci, in or out, to the log */
void Mux::log_msg(const char *dir, u8 dlci, const h245::Msg &m)
{
	if (m.type == h245::Type::Olc)
		link_->logf("v76: DLCI %u %s: %s, channel %u, %s, %s.", dlci,
			    dir, h245::type_name(m.type), m.ch.lcn,
			    m.ch.kind == h245::Kind::Data ? "data" :
			    m.ch.kind == h245::Kind::Audio ? "audio" :
			    m.ch.kind == h245::Kind::Control ? "control" :
			    "other", m.ch.fwd.erm ? "ERM" : "UNERM");
	else
		link_->logf("v76: DLCI %u %s: %s, channel %u.", dlci, dir,
			    h245::type_name(m.type), m.lcn);
}

} // namespace dlink
