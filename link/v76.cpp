/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v76.cpp - V.76's multiplexer and V.75's control of its DLCs, for
 *              DSVD's data and voice channels (v76.h)
 *
 */

#include <cstdio>

#include "link.h"
#include "v76.h"

namespace dlink {

/* the U frames' control octets without P/F (6.2.3/V.76, as LAPM's) */
static const u8 U_SABME = 0x6f, U_DM = 0x0f, U_DISC = 0x43, U_UA = 0x63,
	U_XID = 0xaf, U_UI = 0x03;
static const u8 PF = 0x10;

enum {
	FLAG_BITS = 384,        /* the initiator's flags before its first
				   frame (at least 16, clause 7/V.76) */
	DLCI_FIRST = 0,         /* the initiator's first DLCI (Annex C) */
	VOICE_TRIES = 4,        /* our voice SABME's tries */
	VOICE_PORT = 1,         /* the voice channel's port (data: 0) */
	VOICE_CREDIT = 4,       /* voice frames' bits saved up at most */
	HDR_SILENCE = 0x01,     /* the audio header's bits (Table 7/V.75) */
	HDR_SID = 0x02,
};

/* the line's bits in 10 ms (a voice frame's time), rate in bit/s */
static s32 frame_bits(s32 rate)
{
	return rate > 0 ? rate / 100 : 288;
}

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
	vframe_ = Frame();
	vframe_.buf = vbuf_;
	vbusy_ = false;
	vstate_ = VState::Closed;
	vdlci_ = -1;
}

/* (see v76.h) */
void Mux::start(bool initiator)
{
	initiator_ = initiator;
	dlci_ = -1;
	head_ = tail_ = 0;
	vstate_ = VState::Closed;
	vdlci_ = -1;
	vbusy_ = false;
	vtimer_ = 0;
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
	if (link_->voice()) {
		vtries_ = 0;
		vdlci_ = dlci_ == DLCI_FIRST + 1 ? DLCI_FIRST + 2 : DLCI_FIRST + 1;
		vlcn_ = static_cast<u16>(lcn_ + 1);
		open_voice();
	}
}

/* our voice channel's SABME on vdlci_ (again after no answer): G.729
   Annex A, a frame a packet, UNERM both ways with a 16 bit FCS and
   V.75's audio header (for silence: SID frames, and none sent) */
void Mux::open_voice()
{
	h245::Msg m;
	m.type = h245::Type::Olc;
	m.ch.lcn = vlcn_;
	m.ch.port = VOICE_PORT;
	m.ch.kind = h245::Kind::Audio;
	m.ch.frames = 1;
	m.ch.fwd.erm = m.ch.rev.erm = false;
	m.ch.fwd.n401 = m.ch.rev.n401 = g729::FRAME_OCTETS + 1;
	m.ch.fwd.audio_header = m.ch.rev.audio_header = true;
	log_msg("out", static_cast<u8>(vdlci_), m);
	tx_unnum(static_cast<u8>(vdlci_), true, U_SABME | PF, h245::user_data(m));
	vstate_ = VState::Opening;
	vhdr_tx_ = vhdr_rx_ = false;
	vtimer_ = link_->tx_rate() > 0 ? link_->tx_rate() : 28800;
	vtries_++;
}

/* (see v76.h) */
void Mux::clock(s32 bits)
{
	if (vstate_ == VState::Open) {
		const s32 per = frame_bits(link_->tx_rate());
		vcredit_ += bits;
		if (vcredit_ > VOICE_CREDIT * per)
			vcredit_ = VOICE_CREDIT * per;
		/* silence at the far end: comfort noise every 10 ms that
		   brings no frame */
		if (vsil_) {
			vrx_credit_ += bits;
			while (vrx_credit_ >= per) {
				vrx_credit_ -= per;
				s16 pcm[g729::FRAME];
				dec_.decode(nullptr, 0, pcm);
				link_->voice_out(pcm, g729::FRAME);
			}
		}
	}
	if (vtimer_ <= 0 || (vtimer_ -= bits) > 0)
		return;
	vtimer_ = 0;
	if (vstate_ != VState::Opening)
		return;
	if (vtries_ < VOICE_TRIES) {
		open_voice();
		return;
	}
	link_->logf("v76: DLCI %d: no answer: no voice channel.", vdlci_);
	vstate_ = VState::Closed;
	vdlci_ = -1;
}

/* the voice channel is closed (it was open: the owner hears) */
void Mux::voice_closed()
{
	const bool was = vstate_ == VState::Open;
	vstate_ = VState::Closed;
	vdlci_ = -1;
	vtimer_ = 0;
	if (was)
		link_->voice_state(false);
}

/* (see v76.h): nothing while no channel is open or opening */
void Mux::disconnect()
{
	if (vstate_ != VState::Closed) {
		h245::Msg m;
		m.type = h245::Type::Clc;
		m.lcn = vlcn_;
		log_msg("out", static_cast<u8>(vdlci_), m);
		tx_unnum(static_cast<u8>(vdlci_), true, U_DISC | PF,
			 h245::user_data(m));
		voice_closed();
	}
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

/* the framer's next frame: ours first, then the voice channel's,
   then the data channel's */
Frame *Mux::next_tx_frame()
{
	if (head_ != tail_) {
		Frame *f = &slot_[head_].frame;
		head_ = static_cast<s16>((head_ + 1) % CTRL_FRAMES);
		return f;
	}
	if (Frame *f = next_voice())
		return f;
	return dlci_ >= 0 ? lapm_->next_tx_frame() : nullptr;
}

/* the voice channel's next UI frame, once its 10 ms are due and the
   owner has given its 80 samples; else none (and none while the
   encoder finds silence that has not changed: nothing is sent) */
Frame *Mux::next_voice()
{
	if (vstate_ != VState::Open || vbusy_)
		return nullptr;
	const s32 per = frame_bits(link_->tx_rate());
	while (vcredit_ >= per) {
		if (mic_n_ < g729::FRAME)
			mic_n_ = static_cast<s16>(mic_n_ +
				link_->voice_in(mic_ + mic_n_,
						static_cast<s16>(g729::FRAME - mic_n_)));
		if (mic_n_ < g729::FRAME)
			return nullptr;
		vcredit_ -= per;
		mic_n_ = 0;
		s16 n = 0;
		vbuf_[n++] = addr(static_cast<u8>(vdlci_), true);
		vbuf_[n++] = U_UI;
		const s16 hdr = n;
		if (vhdr_tx_)
			n++;
		const int len = enc_.encode(mic_, vbuf_ + n);
		if (len == 0)
			continue;
		if (vhdr_tx_) {
			/* a SID frame (with the silence bit) or speech; the
			   sequence number in bits 2 to 6 (clause 9/V.75) */
			vbuf_[hdr] = static_cast<u8>((vseq_tx_ & 0x1f) << 2 |
				(len == g729::SID_OCTETS ? HDR_SILENCE | HDR_SID : 0));
			vseq_tx_ = static_cast<u8>((vseq_tx_ + 1) & 0x1f);
		}
		vframe_.size = static_cast<s16>(n + len);
		vbusy_ = true;
		return &vframe_;
	}
	return nullptr;
}

/* a frame is out: the data channel's to it */
void Mux::tx_complete(Frame *f)
{
	if (f == &vframe_) {
		vbusy_ = false;
		return;
	}
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
	if (vdlci_ >= 0 && dlci == vdlci_) {
		voice_frame(f, ctrl, cmd);
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
	else if (c.kind == h245::Kind::Audio && rx_voice(f, dlci, m, &cause))
		return;
	else if (c.kind != h245::Kind::Data)
		cause = c.kind == h245::Kind::Audio ? cause :
			h245::TYPE_NOT_SUPPORTED;
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

/* the remote's SABME on dlci opening an audio channel m: the first one
   (G.729 Annex A, UNERM both ways with a 16 bit FCS), with a voice
   path, becomes the voice channel, the UA with OpenLogicalChannelAck
   (true); else false and why */
bool Mux::rx_voice(Frame *f, u8 dlci, const h245::Msg &m, u8 *cause)
{
	const h245::Channel &c = m.ch;
	if (!link_->voice())
		*cause = h245::TYPE_NOT_SUPPORTED;
	else if (vstate_ != VState::Closed)
		*cause = h245::TYPE_NOT_AVAILABLE;
	else if (c.fwd.erm || c.fwd.crc != 16 ||
		 (c.has_rev && (c.rev.erm || c.rev.crc != 16)))
		*cause = h245::UNSUITABLE_REVERSE;
	else
		*cause = h245::UNSPECIFIED;
	if (*cause != h245::UNSPECIFIED)
		return false;
	/* the opener's forward direction is what we receive */
	vdlci_ = dlci;
	vlcn_ = m.lcn;
	vhdr_rx_ = c.fwd.audio_header;
	vhdr_tx_ = c.has_rev && c.rev.audio_header;
	h245::Msg r;
	r.type = h245::Type::OlcAck;
	r.lcn = vlcn_;
	log_msg("out", dlci, r);
	tx_unnum(dlci, false, static_cast<u8>(U_UA | (f->buf[1] & PF)),
		 h245::user_data(r));
	voice_open();
	return true;
}

/* the voice channel is open: the coder from its start, with Annex B's
   DTX if the audio header can say so; the owner hears */
void Mux::voice_open()
{
	vstate_ = VState::Open;
	vtimer_ = 0;
	vseq_tx_ = 0;
	vseq_rx_ = -1;
	vcredit_ = 0;
	vrx_credit_ = 0;
	vsil_ = false;
	mic_n_ = 0;
	enc_.set_dtx(vhdr_tx_);
	enc_.reset();
	dec_.reset();
	link_->voice_state(true);
}

/* a frame on the voice channel's DLC: speech (a UI command) to the
   owner; our SABME's UA (open) or DM (refused); the remote's DISC
   (closed: UA with CloseLogicalChannelAck); its SABME again (our UA
   lost: again) */
void Mux::voice_frame(Frame *f, u8 ctrl, bool cmd)
{
	const u8 u = static_cast<u8>(ctrl & ~PF);
	const u8 dlci = static_cast<u8>(vdlci_);
	const u8 *p = f->buf + 2;
	s16 n = static_cast<s16>(f->count - 2);
	h245::Msg m;
	const bool msg = (ctrl & 3) == 3 && u != U_UI && n > 0 &&
		h245::from_user_data(p, static_cast<size_t>(n), &m);
	if (msg)
		log_msg("in", dlci, m);

	if (u == U_UI && cmd) {
		if (vstate_ != VState::Open)
			return;
		bool silence = false;
		if (vhdr_rx_) {
			if (n < 1)
				return;
			silence = (p[0] & (HDR_SILENCE | HDR_SID)) != 0;
			const s16 seq = static_cast<s16>(p[0] >> 2 & 0x1f);
			/* frames lost since the last: concealed (a few) */
			if (vseq_rx_ >= 0) {
				const s16 gap = static_cast<s16>((seq - vseq_rx_) & 0x1f);
				for (s16 i = 0; i < gap && i < 3; i++) {
					s16 pcm[g729::FRAME];
					dec_.erase(pcm);
					link_->voice_out(pcm, g729::FRAME);
				}
			}
			vseq_rx_ = static_cast<s16>((seq + 1) & 0x1f);
			p++;
			n--;
		}
		/* silence: a SID frame (or none), then comfort noise until
		   speech comes again (clock()) */
		vrx_credit_ = -frame_bits(link_->tx_rate()) / 2;
		vsil_ = silence;
		if (silence) {
			s16 pcm[g729::FRAME];
			dec_.decode(p, n >= g729::SID_OCTETS ? g729::SID_OCTETS : 0,
				    pcm);
			link_->voice_out(pcm, g729::FRAME);
			return;
		}
		for (; n >= g729::FRAME_OCTETS; n -= g729::FRAME_OCTETS,
		     p += g729::FRAME_OCTETS) {
			s16 pcm[g729::FRAME];
			dec_.decode(p, pcm);
			link_->voice_out(pcm, g729::FRAME);
		}
		return;
	}
	if (!cmd && u == U_UA && vstate_ == VState::Opening) {
		/* as asked: the audio header both ways */
		vhdr_tx_ = vhdr_rx_ = true;
		voice_open();
		return;
	}
	if (!cmd && u == U_DM && vstate_ == VState::Opening) {
		link_->logf("v76: DLCI %u: voice channel refused.", dlci);
		voice_closed();
		return;
	}
	if (cmd && u == U_DISC) {
		h245::Msg r;
		r.type = h245::Type::ClcAck;
		r.lcn = vlcn_;
		log_msg("out", dlci, r);
		tx_unnum(dlci, false, static_cast<u8>(U_UA | (ctrl & PF)),
			 h245::user_data(r));
		voice_closed();
		return;
	}
	if (cmd && u == U_SABME && vstate_ == VState::Open && !initiator_ &&
	    msg && m.type == h245::Type::Olc) {
		h245::Msg r;
		r.type = h245::Type::OlcAck;
		r.lcn = vlcn_;
		tx_unnum(dlci, false, static_cast<u8>(U_UA | (ctrl & PF)),
			 h245::user_data(r));
	}
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
