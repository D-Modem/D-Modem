/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    mnp.cpp - MNP 2 to 4 (V.42 (10/96) Annex A): the octet framing and
 *              the procedure
 *
 */

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "dm_types.h"
#include "link.h"

namespace dlink {

static const u8 SYN = 0x16, DLE = 0x10, STX = 0x02, ETX = 0x03;

/* the FCS of A.3.6 (x^16 + x^15 + x^2 + 1, bits reflected, from 0) after
   one more octet */
static u16 crc_add(u16 crc, u8 c)
{
	crc ^= c;
	for (s16 i = 0; i < 8; i++)
		crc = crc & 1 ? static_cast<u16>(crc >> 1 ^ 0xa001) :
			static_cast<u16>(crc >> 1);
	return crc;
}

/* the first octet of variable parameter type in a header of len octets
   (with its length indication), its parameters from octet start; -1 if
   it has none */
static s16 param(const u8 *h, s16 len, s16 start, s16 type)
{
	for (s16 i = start; i + 2 <= len; i += 2 + h[i + 1]) {
		if (i + 2 + h[i + 1] > len)
			break;
		if (h[i] == type && h[i + 1] >= 1)
			return h[i + 2];
	}
	return -1;
}


/*
 *    start-stop octets (A.3)
 *
 */

/* MNP's frames on start-stop octets: no frame going out, the receiver
   hunting for SYN DLE STX */
void MnpAsync::start(Link *link, Mnp *mnp)
{
	framer_start(link);
	mnp_ = mnp;
	tx_frame_ = nullptr;
	tx_state_ = Tx::Idle;
	rx_frame_ = {};
	rx_frame_.buf = rx_buf_;
	rx_frame_.size = sizeof(rx_buf_);
	rx_restart(Rx::Hunt);
}

/* the detection phase has taken SYN DLE STX: the body follows, the
   first bits of it in rx */
void MnpAsync::start_in_frame(const BitQueue &rx)
{
	rx_ = rx;
	rx_restart(Rx::Body);
	rx_scan();
}

/* a character to send: start bit, 8 data bits (least significant
   first), stop bit */
void MnpAsync::tx_char(u8 ch)
{
	tx_.data <<= 10;
	tx_.data |= static_cast<u32>(reverse_bits(ch)) << 1 | 1;
	tx_.bit += 10;
}

/* the transmitter: Mnp's next frame (while the octets are the framing)
   a character at a time, SYN DLE STX, the body with DLE doubled, DLE
   ETX, the FCS; tx_complete() once the last character is out; false
   when there is no frame (marks) */
bool MnpAsync::tx_fill()
{
	switch (tx_state_) {
	case Tx::Idle:
		if (!(tx_frame_ = mnp_->next_octet_frame()))
			return false;
		link_->stats().tx_frames++;
		tx_frame_->count = 0;
		tx_fcs_ = 0;
		tx_dle_ = false;
		tx_step_ = 0;
		tx_state_ = Tx::Start;
		/* fall through */
	case Tx::Start:
		tx_char(tx_step_ == 0 ? SYN : tx_step_ == 1 ? DLE : STX);
		if (++tx_step_ == 3)
			tx_state_ = Tx::Body;
		return true;
	case Tx::Body:
		if (tx_dle_) {
			tx_dle_ = false;
			tx_char(DLE);
			return true;
		}
		if (tx_frame_->count < tx_frame_->size) {
			u8 c = tx_frame_->buf[tx_frame_->count++];
			tx_fcs_ = crc_add(tx_fcs_, c);
			tx_dle_ = c == DLE;
			tx_char(c);
			return true;
		}
		tx_step_ = 0;
		tx_state_ = Tx::End;
		/* fall through */
	case Tx::End:
		switch (tx_step_++) {
		case 0:
			tx_char(DLE);
			break;
		case 1:
			tx_fcs_ = crc_add(tx_fcs_, ETX);
			tx_char(ETX);
			break;
		case 2:
			tx_char(static_cast<u8>(tx_fcs_));
			break;
		default:
			tx_char(static_cast<u8>(tx_fcs_ >> 8));
			tx_state_ = Tx::Done;
			break;
		}
		return true;
	case Tx::Done: {
		/* the frame is out: the next one (Mnp may have gone to HDLC
		   frames: none then) */
		Frame *f = tx_frame_;
		tx_frame_ = nullptr;
		tx_state_ = Tx::Idle;
		mnp_->tx_complete(f);
		return tx_fill();
	}
	}
	return false;
}

/* the rest of the n items with no frame to send: what is left of the
   last character, then marks; how many */
s32 MnpAsync::tx_idle(s16 nbits, u8 mask, u8 *&buf, s32 &n)
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

/* a new frame from state (Hunt, or Body once its start is in) */
void MnpAsync::rx_restart(Rx state)
{
	rx_state_ = state;
	rx_frame_.count = 0;
	rx_fcs_ = 0;
}

/* the receiver: characters after marks, as Async's */
void MnpAsync::rx_scan()
{
	for (;;) {
		/* marks before a start bit */
		while (rx_.bit && rx_.data & (UINT32_C(1) << (rx_.bit - 1)))
			rx_.bit--;
		if (rx_.bit < 10)
			break;
		u8 ch = static_cast<u8>(rx_.data >> (rx_.bit - 9));
		rx_.bit -= 10;
		rx_char(reverse_bits(ch));
	}
}

/* a received character: SYN DLE STX starts a frame, DLE DLE in it is a
   DLE, DLE ETX ends it, the FCS follows; a good frame of two octets or
   more to Mnp, a bad one counted */
void MnpAsync::rx_char(u8 ch)
{
	switch (rx_state_) {
	case Rx::Hunt:
		if (ch == SYN)
			rx_state_ = Rx::Syn;
		break;
	case Rx::Syn:
		rx_state_ = ch == DLE ? Rx::Dle : ch == SYN ? Rx::Syn :
			Rx::Hunt;
		break;
	case Rx::Dle:
		if (ch == STX)
			rx_restart(Rx::Body);
		else
			rx_state_ = ch == SYN ? Rx::Syn : Rx::Hunt;
		break;
	case Rx::Body:
		if (ch == DLE) {
			rx_state_ = Rx::BodyDle;
			break;
		}
		/* fall through */
	body:
		if (rx_frame_.count == rx_frame_.size) {
			link_->logf("mnp: rx frame overflow.");
			mnp_->rx_bad_frame(false);
			rx_restart(Rx::Hunt);
			break;
		}
		rx_frame_.buf[rx_frame_.count++] = ch;
		rx_fcs_ = crc_add(rx_fcs_, ch);
		break;
	case Rx::BodyDle:
		if (ch == DLE) {
			rx_state_ = Rx::Body;
			goto body;
		}
		if (ch == ETX) {
			rx_fcs_ = crc_add(rx_fcs_, ETX);
			rx_state_ = Rx::Fcs1;
			break;
		}
		link_->logf("mnp: DLE 0x%02x in a frame.", ch);
		mnp_->rx_bad_frame(false);
		if (ch == STX)          /* a new frame */
			rx_restart(Rx::Body);
		else
			rx_restart(Rx::Hunt);
		break;
	case Rx::Fcs1:
		rx_fcs_lo_ = ch;
		rx_state_ = Rx::Fcs2;
		break;
	case Rx::Fcs2:
		if ((rx_fcs_lo_ | static_cast<u32>(ch) << 8) != rx_fcs_) {
			link_->logf("mnp: rx frame fcs error.");
			link_->stats().rx_errors++;
			mnp_->rx_bad_frame(false);
		} else if (rx_frame_.count < 2) {
			link_->logf("mnp: rx frame too short.");
			mnp_->rx_bad_frame(false);
		} else {
			link_->stats().rx_frames++;
			mnp_->rx_octet_frame(&rx_frame_);
		}
		rx_restart(Rx::Hunt);
		break;
	}
}


/*
 *    the procedure: timers
 *
 */

/* the data pump's rate (a nominal 28800 bit/s if not known) */
s32 Mnp::rate() const
{
	s32 r = link_->tx_rate();
	return r ? r : 28800;
}

/* T401 in the establishment (0.5 to 9 s): 2 s, and the time of three
   LRs (at 300 bit/s an LR takes a second) */
s32 Mnp::t401_est() const
{
	return rate() * 2 + 3 * 40 * 10;
}

/* T401 in the data phase (A.7.5.1): k frames of N401 octets and an LA
   at the line's rate, and a round trip of 1 s */
s32 Mnp::t401_data() const
{
	const s16 lb = mode_ == 3 ? 8 : 10, lf = mode_ == 3 ? 4 : 7;
	const s16 llt = opt_ & 2 ? 3 : 5, lla = opt_ & 2 ? 4 : 8;
	return static_cast<s32>(k_) * lb * (lf + llt + n401_) +
	       lb * (lf + lla) + rate();
}

/* T404 (A.7.5.4): 3 s at 2400 bit/s and above, else 7 s */
static s32 t404(s32 rate)
{
	return rate * (rate >= 2400 ? 3 : 7);
}

/* bits more have gone out: the timers that run out */
void Mnp::clock(s32 bits)
{
	if (t_start_ > 0 && (t_start_ -= bits) <= 0) {
		/* the initiator's LR after the marks */
		t_start_ = 0;
		if (phase_ == Phase::Init) {
			phase_ = Phase::Request;
			tries_ = 1;
			lr_due_ = true;
			t401_ = t401_est();
		}
	}
	if (t402_ > 0 && (t402_ -= bits) <= 0) {
		t402_ = 0;
		if (unacked_)
			la_due_ = true;
	}
	if (t404_ > 0 && (t404_ -= bits) <= 0) {
		t404_ = 0;
		if (phase_ == Phase::Data)
			la_due_ = true;
	}
	if (t401_ > 0 && (t401_ -= bits) <= 0) {
		t401_ = 0;
		t401_expired();
	}
}

/* T401 ran out: the establishment's LR again (after EST_TRIES of them,
   async if no frame came at all, else LD; after as long a wait for the
   responder's LR, async: it has sent nothing, so its start in the
   detection phase was not one); in the data phase the LT frames again
   from the first not acknowledged (not without credit; after N400 of
   them LD); in the disconnect phase released */
void Mnp::t401_expired()
{
	switch (phase_) {
	case Phase::Listen:
	case Phase::Request:
	case Phase::Respond:
		if (tries_ >= EST_TRIES) {
			link_->logf("mnp: no answer to the establishment.");
			if (!heard_ || phase_ == Phase::Listen)
				fallback();
			else
				fail(1);
			return;
		}
		tries_++;
		if (phase_ != Phase::Listen)
			lr_due_ = true;
		t401_ = t401_est();
		break;
	case Phase::Data:
		if (vs_ == una_)
			break;
		if (!can_send(una_)) {
			/* no credit: the remote's LA will say when */
			t401_ = t401_data();
			break;
		}
		if (++rtx_count_ > N400) {
			link_->logf("mnp: LT %d sent %d times: disconnect.", una_,
				    N400);
			fail(4);
			return;
		}
		link_->logf("mnp: T401: LT %d again.", una_);
		nxt_ = una_;
		dup_ignore_ = 0;
		t401_ = t401_data();
		break;
	case Phase::Disc:
		/* the LD did not go out */
		phase_ = Phase::Idle;
		link_->status(Status::Released);
		break;
	default:
		break;
	}
}


/*
 *    frames to send
 *
 */

/* the control frame's buffer for a header of len octets (with the
   length indication) */
Frame *Mnp::ctrl(s16 len)
{
	ctrl_.buf = ctrl_buf_;
	ctrl_.size = len;
	ctrl_buf_[0] = static_cast<u8>(len - 1);
	return &ctrl_;
}

/* our LR (A.6.4.1): the initiator's offer (N401 64 with the 256-octet
   optimization, as the peers have it: the PCTel modem takes no LR with
   N401 256), or the responder's answer with what was agreed and only
   the parameters the initiator's had (and we know) */
Frame *Mnp::build_lr()
{
	static const u8 constant2[] = { 1, 6, 1, 0, 0, 0, 0, 255 };
	u8 *b = ctrl_buf_;
	s16 n = 1;
	const s16 mode = initiator_ ? our_mode_ : mode_;
	const s16 k = initiator_ ? our_k_ : k_;
	const s16 n401 = !initiator_ ? lr_n401_ :
		our_opt_ & 1 ? static_cast<s16>(MNP_DFLT_INFO) : our_n401_;

	b[n++] = LR;
	b[n++] = 2;             /* constant parameter 1 */
	memcpy(b + n, constant2, sizeof(constant2));
	n += sizeof(constant2);
	b[n++] = 2;
	b[n++] = 1;
	b[n++] = static_cast<u8>(mode);
	b[n++] = 3;
	b[n++] = 1;
	b[n++] = static_cast<u8>(k);
	b[n++] = 4;
	b[n++] = 2;
	b[n++] = static_cast<u8>(n401);
	b[n++] = static_cast<u8>(n401 >> 8);
	if (initiator_ || lr_opt_) {
		b[n++] = 8;
		b[n++] = 1;
		b[n++] = static_cast<u8>(initiator_ ? our_opt_ : opt_);
	}
	if (initiator_ ? our_mnp5_ : mnp5_) {
		b[n++] = 9;
		b[n++] = 1;
		b[n++] = 1;
	}
	return ctrl(n);
}

/* an LA (A.6.4.2): N(R) = V(R) - 1, our credit (none while the DTE has
   not taken the last LT's data), fixed or variable fields as agreed;
   the LTs received are acknowledged, T404 again */
Frame *Mnp::build_la()
{
	const u8 nr = static_cast<u8>(vr_ - 1);
	const u8 nk = static_cast<u8>(busy_ ? 0 : k_);
	Frame *f;
	if (opt_ & 2) {
		f = ctrl(4);
		ctrl_buf_[1] = LA;
		ctrl_buf_[2] = nr;
		ctrl_buf_[3] = nk;
	} else {
		f = ctrl(8);
		ctrl_buf_[1] = LA;
		ctrl_buf_[2] = 1;
		ctrl_buf_[3] = 1;
		ctrl_buf_[4] = nr;
		ctrl_buf_[5] = 2;
		ctrl_buf_[6] = 1;
		ctrl_buf_[7] = nk;
	}
	unacked_ = 0;
	la_due_ = false;
	t402_ = 0;
	if (phase_ == Phase::Data)
		t404_ = t404(rate());
	return f;
}

/* an LD (A.6.5.1) with ld_reason_ */
Frame *Mnp::build_ld()
{
	Frame *f = ctrl(5);
	ctrl_buf_[1] = LD;
	ctrl_buf_[2] = 1;
	ctrl_buf_[3] = 1;
	ctrl_buf_[4] = static_cast<u8>(ld_reason_);
	return f;
}

/* an LNA (A.6.7.2) for the LN numbered lna_nr_ */
Frame *Mnp::build_lna()
{
	Frame *f = ctrl(5);
	ctrl_buf_[1] = LNA;
	ctrl_buf_[2] = 1;
	ctrl_buf_[3] = 1;
	ctrl_buf_[4] = lna_nr_;
	return f;
}

/* may the LT numbered ns go: after N(R), within the credit N(k) */
bool Mnp::can_send(u8 ns) const
{
	const u8 d = static_cast<u8>(ns - nr_);
	return d >= 1 && d <= nk_;
}

/* the next LT: one again from nxt_ (retransmission), else a new one
   with the DTE's characters (V(S) its N(S)) while fewer than k are
   outstanding; NULL if none can go */
Frame *Mnp::next_lt()
{
	Slot *s;
	if (nxt_ != vs_) {
		if (!can_send(nxt_))
			return nullptr;
		s = &slot_[nxt_ % MNP_MAX_K];
		nxt_++;
		lt_again_++;
		link_->stats().tx_again++;
	} else {
		if (static_cast<u8>(vs_ - una_) >= k_ || !can_send(vs_))
			return nullptr;
		s = &slot_[vs_ % MNP_MAX_K];
		const s16 hdr = opt_ & 2 ? 3 : 5;
		s16 n = link_->read_chars(s->buf + hdr, n401_);
		if (n <= 0)
			return nullptr;
		u8 *b = s->buf;
		if (opt_ & 2) {
			b[0] = 2;
			b[1] = LT;
			b[2] = vs_;
		} else {
			b[0] = 4;
			b[1] = LT;
			b[2] = 1;
			b[3] = 1;
			b[4] = vs_;
		}
		s->frame.buf = b;
		s->frame.size = hdr + n;
		vs_++;
		nxt_ = vs_;
		lt_sent_++;
	}
	/* T401 from the start of the LT (k > 1) */
	if (!t401_)
		t401_ = t401_data();
	return &s->frame;
}

/* the next frame for the framing in use: the establishment's LR or LA,
   the LD; in the data phase an LNA, an LA when one is due (or k / 2
   LTs unacknowledged), an LT, else an LA for any LT unacknowledged
   (A.7.3.3: no data to send); NULL for none */
Frame *Mnp::next_frame()
{
	switch (phase_) {
	case Phase::Request:
	case Phase::Respond:
		if (!lr_due_)
			return nullptr;
		lr_due_ = false;
		return build_lr();
	case Phase::Confirm:
		/* N(R) 0, the credit the responder's LR gave (k) */
		return build_la();
	case Phase::Disc:
		if (!ld_due_)
			return nullptr;
		ld_due_ = false;
		return build_ld();
	case Phase::Data:
		break;
	default:
		return nullptr;
	}
	if (lna_due_) {
		lna_due_ = false;
		return build_lna();
	}
	if (la_due_ || (unacked_ && unacked_ >= std::max(1, k_ / 2)))
		return build_la();
	if (Frame *f = next_lt())
		return f;
	if (unacked_)
		return build_la();
	return nullptr;
}

/* MnpAsync's next frame (none once HDLC frames are the framing) */
Frame *Mnp::next_octet_frame()
{
	return bit_tx_ ? nullptr : next_frame();
}

/* Hdlc's next frame (in the data phase of framing mode 3) */
Frame *Mnp::next_tx_frame()
{
	return bit_tx_ ? next_frame() : nullptr;
}

/* a frame is out: the confirming LA starts the initiator's data phase,
   the LD releases the link */
void Mnp::tx_complete(Frame *f)
{
	if (f != &ctrl_)
		return;
	switch (ctrl_buf_[1]) {
	case LA:
		if (phase_ == Phase::Confirm)
			data_phase();
		break;
	case LD:
		if (phase_ == Phase::Disc) {
			phase_ = Phase::Idle;
			t401_ = 0;
			link_->status(Status::Released);
		}
		break;
	default:
		break;
	}
}


/*
 *    frames received
 *
 */

/* an LR's parameters against ours (A.7.1.5): constant parameter 1 must
   be 2; the lower framing mode, k and N401, the optimizations both
   have (256-octet information fields with bit 1), MNP 5 (parameter 9)
   if both want it; unknown parameters are ignored; false (ld_reason_
   set) if they cannot be agreed */
bool Mnp::rx_lr(const u8 *h, s16 len)
{
	if (len < 3 || h[2] != 2) {
		ld_reason_ = 2;
		return false;
	}
	s16 mode = param(h, len, 3, 2);
	s16 k = param(h, len, 3, 3);
	s16 opt = param(h, len, 3, 8);
	s16 mnp5 = param(h, len, 3, 9);
	s32 n401 = MNP_DFLT_INFO;
	for (s16 i = 3; i + 2 <= len && i + 2 + h[i + 1] <= len;
	     i += 2 + h[i + 1])
		if (h[i] == 4)
			n401 = h[i + 1] >= 2 ?
				h[i + 2] | static_cast<s32>(h[i + 3]) << 8 :
				h[i + 1] == 1 ? h[i + 2] : n401;
	mode = std::min<s16>(mode < 0 ? 2 : mode, our_mode_);
	k = std::min<s16>(k < 0 ? 1 : k, our_k_);
	n401 = std::min(n401, static_cast<s32>(our_n401_));
	if (mode < 2 || k < 1 || n401 < 1) {
		link_->logf("mnp: LR: mode %d, k %d, N401 %" PRId32
			    " not supported.", mode, k, n401);
		ld_reason_ = 3;
		return false;
	}
	lr_opt_ = opt >= 0;
	opt_ = opt < 0 ? 0 : opt & our_opt_ & 3;
	lr_n401_ = n401;
	if (opt_ & 1)
		n401 = MNP_MAX_INFO;
	mode_ = mode;
	k_ = k;
	n401_ = n401;
	mnp5_ = mnp5 > 0 && (mnp5 & 1) && our_mnp5_;
	link_->logf("mnp: LR: framing mode %d, k %d, N401 %d, "
		    "optimization %d%s.", mode_, k_, n401_, opt_,
		    mnp5_ ? ", MNP 5" : "");
	return true;
}

/* an LT in the data phase (A.7.3.2): in sequence and with credit, its
   data to the DTE (what it does not take kept: no credit until it
   has); V(R) - 1 once ignored, others out of sequence or without credit
   discarded with an LA.  One with no data (A.6.6.1 has none, but the
   HSF modem sends them with MNP 5) is in sequence all the same */
void Mnp::rx_lt(const u8 *h, s16 len, const u8 *info, s16 n)
{
	const s16 ns = len == 3 ? h[2] : param(h, len, 2, 1);
	if (ns < 0 || n > MNP_MAX_INFO) {
		link_->logf("mnp: bad LT (header %d, N(S) %d, %d octets).",
			    len, ns, n);
		la_due_ = true;
		return;
	}
	if (!t402_)
		t402_ = t401_data() / 2;
	if (ns != vr_) {
		if (ns == static_cast<u8>(vr_ - 1) && !dup_seen_) {
			dup_seen_ = true;
			return;
		}
		la_due_ = true;
		return;
	}
	if (busy_) {
		la_due_ = true;
		return;
	}
	dup_seen_ = false;
	vr_++;
	unacked_++;
	lt_rcvd_++;
	if (!n)
		return;
	s16 w = link_->write_chars(info, n);
	if (w < 0)
		w = 0;
	if (w < n) {
		/* keep the rest; no credit until the DTE takes it */
		rx_head_ = 0;
		rx_count_ = n - w;
		memcpy(rx_buf_, info + w, rx_count_);
		busy_ = true;
		link_->push_pending = true;
		la_due_ = true;
	}
}

/* an LA in the data phase (A.7.3.4): our LTs up to N(R) acknowledged
   (T401 again for any still out), its credit; the same N(R) again sends
   from the first not acknowledged (A.7.3.5), but for the LAs the LTs
   sent before that may still bring */
void Mnp::rx_la(const u8 *h, s16 len)
{
	s16 nr = len == 4 ? h[2] : param(h, len, 2, 1);
	s16 nk = len == 4 ? h[3] : param(h, len, 2, 2);
	if (nr < 0 || nk < 0) {
		link_->logf("mnp: bad LA.");
		return;
	}
	const u8 out = static_cast<u8>(vs_ - una_);
	const u8 acked =
		static_cast<u8>(nr - static_cast<u8>(una_ - 1));
	if (acked > out) {
		link_->logf("mnp: LA N(R) %d out of %d..%d.", nr,
			    static_cast<u8>(una_ - 1),
			    static_cast<u8>(vs_ - 1));
		return;
	}
	const bool dup = have_la_ && nr == nr_;
	have_la_ = true;
	nr_ = static_cast<u8>(nr);
	nk_ = static_cast<u8>(nk);
	if (acked) {
		/* those still to be sent again from below N(R) are not */
		if (static_cast<u8>(nxt_ - una_) < acked)
			nxt_ = static_cast<u8>(nr + 1);
		una_ = static_cast<u8>(nr + 1);
		rtx_count_ = 0;
		dup_ignore_ = 0;
		t401_ = una_ == vs_ ? 0 : t401_data();
	} else if (dup && una_ != vs_) {
		if (dup_ignore_ > 0) {
			dup_ignore_--;
			return;
		}
		if (++rtx_count_ > N400) {
			link_->logf("mnp: LT %d sent %d times: disconnect.", una_,
				    N400);
			fail(4);
			return;
		}
		/* the LTs that went out after the one lost each bring
		   their LA */
		dup_ignore_ = static_cast<u8>(nxt_ - una_) - 1;
		nxt_ = una_;
		t401_ = t401_data();
	}
}

/* an LN (A.7.4.3): an LNA, and the break (in sequence) noted; we pass
   no break to the DTE */
void Mnp::rx_ln(const u8 *h, s16 len)
{
	const s16 nsa = param(h, len, 2, 1);
	const s16 type = param(h, len, 2, 2);
	if (nsa < 0) {
		link_->logf("mnp: bad LN.");
		return;
	}
	if (nsa == vra_) {
		vra_++;
		link_->logf("mnp: break (type %d).", type);
	}
	lna_nr_ = static_cast<u8>(nsa);
	lna_due_ = true;
}

/* an LD: the connection is over */
void Mnp::rx_ld(const u8 *h, s16 len)
{
	link_->logf("mnp: LD, reason %d.", param(h, len, 2, 1));
	phase_ = Phase::Idle;
	t401_ = t402_ = t404_ = 0;
	link_->status(Status::Released);
}

/* a frame from either framing (bit: an HDLC frame): by the phase */
void Mnp::rx_frame(Frame *f, bool bit)
{
	const u8 *b = f->buf;
	const s16 len = b[0] + 1;
	if (b[0] == 255 || len < 2 || len > f->count) {
		link_->logf("mnp: bad header (%d of %d).", b[0], f->count);
		rx_bad_frame(bit);
		return;
	}
	const s16 type = b[1];
	/* the data phase's framing */
	const bool data = bit == (mode_ == 3);
	heard_ = true;
	if (type == LD && phase_ != Phase::Idle && phase_ != Phase::Disc) {
		rx_ld(b, len);
		return;
	}
	switch (phase_) {
	case Phase::Listen:
		if (type != LR || bit)
			break;
		if (!rx_lr(b, len)) {
			fail(ld_reason_);
			break;
		}
		phase_ = Phase::Respond;
		tries_ = 1;
		lr_due_ = true;
		t401_ = t401_est();
		break;
	case Phase::Request:
		if (type != LR || bit)
			break;
		if (!rx_lr(b, len)) {
			fail(3);
			break;
		}
		phase_ = Phase::Confirm;
		t401_ = 0;
		break;
	case Phase::Respond:
		if (type == LR && !bit) {
			/* ours did not arrive */
			lr_due_ = true;
			t401_ = t401_est();
		} else if (type == LA && !bit) {
			data_phase();
			const s16 nk = len == 4 ? b[3] : param(b, len, 2, 2);
			if (nk >= 0)
				nk_ = static_cast<u8>(nk);
		} else if ((type == LT || type == LA) && data) {
			/* the LA was lost: the initiator is in the data
			   phase */
			link_->logf("mnp: data phase without the LA.");
			data_phase();
			rx_frame(f, bit);
		}
		break;
	case Phase::Data:
		if (!data) {
			if (type == LR)
				link_->logf("mnp: LR in the data phase.");
			break;
		}
		switch (type) {
		case LT:
			rx_lt(b, len, b + len, f->count - len);
			break;
		case LA:
			rx_la(b, len);
			break;
		case LN:
			rx_ln(b, len);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

/* MnpAsync's good frames */
void Mnp::rx_octet_frame(Frame *f)
{
	rx_frame(f, false);
}

/* Hdlc's (only while they are the data phase's framing: Link) */
void Mnp::rx_complete(Frame *f)
{
	rx_frame(f, true);
}

/* a frame with a bad FCS (bit: HDLC), of the framing in use: our LR
   again in the establishment (A.7.1.2, A.7.1.3), an LA in the data
   phase (A.7.3.3) */
void Mnp::rx_bad_frame(bool bit)
{
	switch (phase_) {
	case Phase::Request:
	case Phase::Respond:
		if (bit)
			break;
		heard_ = true;
		if (tries_ < EST_TRIES) {
			tries_++;
			lr_due_ = true;
			t401_ = t401_est();
		}
		break;
	case Phase::Listen:
		if (!bit)
			heard_ = true;
		break;
	case Phase::Data:
		if (bit == (mode_ == 3))
			la_due_ = true;
		break;
	default:
		break;
	}
}


/*
 *    the procedure
 *
 */

/* the data phase (A.6.3): V(S), V(R), V(SA), V(RA) 1, the remote's
   credit k, T404; HDLC frames in mode 3; the link is up */
void Mnp::data_phase()
{
	phase_ = Phase::Data;
	vs_ = una_ = nxt_ = 1;
	nr_ = 0;
	nk_ = static_cast<u8>(k_);
	have_la_ = false;
	rtx_count_ = dup_ignore_ = 0;
	vr_ = vra_ = 1;
	unacked_ = 0;
	la_due_ = dup_seen_ = busy_ = lna_due_ = false;
	rx_count_ = rx_head_ = 0;
	t401_ = t402_ = 0;
	t404_ = t404(rate());
	lt_sent_ = lt_again_ = lt_rcvd_ = 0;
	bit_tx_ = mode_ == 3;
	link_->logf("mnp: data phase, %s frames.", bit_tx_ ? "HDLC" :
		    "octet");
	link_->cfg().mnp5 = mnp5_;
	link_->negotiated();
	link_->status(Status::Up);
}

/* the establishment failed or the connection is lost: LD with reason,
   then released */
void Mnp::fail(s16 reason)
{
	link_->logf("mnp: disconnect, reason %d.", reason);
	ld_reason_ = reason;
	ld_due_ = true;
	phase_ = Phase::Disc;
	t402_ = t404_ = 0;
	t401_ = t401_est();
}

/* no frame came at all (A.7.2.2): no LD, the call goes on without
   error correction */
void Mnp::fallback()
{
	phase_ = Phase::Idle;
	t401_ = t402_ = t404_ = t_start_ = 0;
	link_->mnp_fallback();
}

/* we hang up: LD (user-initiated, 255), then released */
void Mnp::disconnect()
{
	if (phase_ == Phase::Idle || phase_ == Phase::Disc)
		return;
	fail(255);
}

/* the characters the DTE did not take: to it again; once all are taken
   our credit again (LA) */
void Mnp::push_rest()
{
	if (phase_ != Phase::Data || !rx_count_) {
		link_->push_pending = false;
		return;
	}
	s16 w = link_->write_chars(rx_buf_ + rx_head_, rx_count_);
	if (w > 0) {
		rx_head_ += w;
		rx_count_ -= w;
	}
	if (!rx_count_) {
		rx_head_ = 0;
		busy_ = false;
		la_due_ = true;
		link_->push_pending = false;
	}
}

/* LT frames not acknowledged */
bool Mnp::tx_pending() const
{
	return phase_ == Phase::Data && vs_ != una_;
}

/* the establishment: the initiator's marks before its LR, the
   responder's wait for one; the defaults agreed until LRs say more */
void Mnp::start(bool initiator)
{
	initiator_ = initiator;
	tries_ = 0;
	heard_ = false;
	bit_tx_ = false;
	ld_reason_ = 0;
	lr_due_ = la_due_ = ld_due_ = lna_due_ = false;
	mode_ = 2;
	k_ = 1;
	n401_ = MNP_DFLT_INFO;
	opt_ = 0;
	mnp5_ = lr_opt_ = false;
	vr_ = 1;
	busy_ = false;
	t401_ = t402_ = t404_ = t_start_ = 0;
	if (initiator) {
		phase_ = Phase::Init;
		/* 64 characters of marks first, as before the ODPs */
		t_start_ = 64 * 10;
	} else {
		phase_ = Phase::Listen;
		tries_ = 1;
		t401_ = t401_est();
	}
}

/* a call: our offer from cfg (framing mode, k, N401, the
   optimizations, the 256-octet one only with N401 256, MNP 5), the LT
   frames' buffers */
void Mnp::init(Link *link, const Config &cfg)
{
	link_ = link;
	phase_ = Phase::Idle;
	our_mode_ = cfg.mnp_mode == 2 ? 2 : 3;
	our_k_ = std::clamp<s16>(cfg.mnp_k, 1, MNP_MAX_K);
	our_n401_ = std::clamp<s16>(cfg.mnp_info, 1, MNP_MAX_INFO);
	our_opt_ = cfg.mnp_opt & (our_n401_ == MNP_MAX_INFO ? 3 : 2);
	our_mnp5_ = cfg.mnp5;
	for (auto &s : slot_) {
		s.frame = {};
		s.frame.buf = s.buf;
	}
	ctrl_ = {};
	ctrl_.buf = ctrl_buf_;
	t401_ = t402_ = t404_ = t_start_ = 0;
}

/* the end of the call: what the data phase did, in the log */
void Mnp::exit()
{
	if (link_ && (lt_sent_ || lt_rcvd_))
		link_->logf("mnp: %" PRId32 " LTs sent (%" PRId32 " again), %"
			    PRId32 " received.",
			    lt_sent_, lt_again_, lt_rcvd_);
	lt_sent_ = lt_again_ = lt_rcvd_ = 0;
	phase_ = Phase::Idle;
	t401_ = t402_ = t404_ = t_start_ = 0;
	rx_count_ = 0;
}

} // namespace dlink
