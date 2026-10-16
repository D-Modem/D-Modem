/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hdlc.cpp - the bit framers: the loops between the data pump's bits
 *               and a framer, HDLC frames (V.42 8.1: flags, zero
 *               insertion, FCS-16), start-stop characters, and the V.42
 *               detection phase (7.2: ODP, ADP)
 *
 *    The BitQueue of a framer holds its bits in line order: the oldest
 *    at position bit - 1 of data, the newest at position 0.
 *
 */

#include "link.h"

namespace dlink {

/* the HDLC flag, 01111110 */
static const uint8_t FLAG = 0x7e;

/* the FCS-16 register before a frame, and after a whole good frame,
   its FCS included (8.1.1.6.1/V.42) */
static const uint16_t FCS_INIT = 0xffff, FCS_GOOD = 0xf0b8;

enum {
	RX_LOOKAHEAD = 18,      /* bits the HDLC receiver needs for a step:
				   an octet, two inserted zeros, a flag */
	MIN_FRAME = 4,          /* address, control, FCS */
};

/* the detection phase's characters: ODP (DC1 with even, then odd
   parity), ADP ('E', then 'C' for V.42 or NUL for no error correction),
   and the start of an MNP frame (SYN DLE STX, A.3/V.42 (10/96)) */
static const uint8_t DC1_EVEN = 0x11, DC1_ODD = 0x91;
static const uint8_t ADP_E = 'E', ADP_C = 'C', ADP_NUL = 0x00;
static const uint32_t MNP_START = 0x16 << 16 | 0x10 << 8 | 0x02;

enum {
	DETECT_MARKS = 512,     /* the originator's marks before its ODPs */
	PATTERN_BITS = 36,      /* one ODP or ADP: two characters, each
				   with 8 marks after its stop bit */
	ADP_BITS = 16 * PATTERN_BITS,   /* the ADPs answering an ODP */
	NOMINAL_RATE = 28800,   /* bit/s when the pump's rate is not known */
};

/* the detection phase's receiver: no pattern character yet, the first
   of a pattern, a pattern decided */
enum { RX_NONE, RX_FIRST, RX_DECIDED };


/*
 *    bit queues
 *
 */

/* the k low bits of bits (the first on the line the most significant
   of them) queued in q after those already there */
static inline void queue_bits(BitQueue &q, uint32_t bits, int k)
{
	q.data = q.data << k | bits;
	q.bit += k;
}

/* the oldest bit queued in q, taken */
static inline int take_bit(BitQueue &q)
{
	return (int)(q.data >> --q.bit & 1);
}

/* the 8 oldest bits queued in q (at least 8), left in it: the oldest
   in the most significant */
static inline uint8_t peek_octet(const BitQueue &q)
{
	return (uint8_t)(q.data >> (q.bit - 8));
}

/* a start-stop character into q: start bit, the 8 data bits least
   significant first, stop bit */
static void queue_char(BitQueue &q, uint8_t ch)
{
	queue_bits(q, (uint32_t)reverse_bits(ch) << 1 | 1, 10);
}

/* a start-stop character from q into ch: the marks before its start
   bit dropped, then its 10 bits taken (the stop bit is not checked);
   false while q holds no whole character */
static bool take_char(BitQueue &q, uint8_t &ch)
{
	while (q.bit && q.data >> (q.bit - 1) & 1)
		q.bit--;
	if (q.bit < 10)
		return false;
	ch = reverse_bits((uint8_t)(q.data >> (q.bit - 9)));
	q.bit -= 10;
	return true;
}


/*
 *    the loops of the framers
 *
 */

/* n items of nbits bits for the data pump: the queued bits, F's
   tx_fill() queueing one more unit each time fewer than nbits are left,
   F's tx_idle() for the rest once it has none; how many items */
template <class F>
int BitFramer<F>::get_bits(int nbits, uint8_t *buf, int n)
{
	F &f = static_cast<F &>(*this);
	const uint8_t mask = (uint8_t)((1 << nbits) - 1);
	int done = 0;

	while (n > 0) {
		if (tx_.bit < nbits) {
			if (f.tx_fill())
				continue;
			done += f.tx_idle(nbits, mask, buf, n);
			break;
		}
		tx_.bit -= nbits;
		*buf++ = (uint8_t)(tx_.data >> tx_.bit & mask);
		n--;
		done++;
	}
	return done;
}

/* n items of nbits bits from the data pump, each queued and then
   scanned by F's rx_scan(); returns n */
template <class F>
int BitFramer<F>::put_bits(int nbits, const uint8_t *buf, int n)
{
	F &f = static_cast<F &>(*this);
	const uint8_t mask = (uint8_t)((1 << nbits) - 1);

	for (int i = 0; i < n; i++) {
		queue_bits(rx_, buf[i] & mask, nbits);
		f.rx_scan();
	}
	return n;
}


/*
 *    start-stop characters
 *
 */

/* a call without error correction: empty queues, no bit timer */
void Async::start(Link *link)
{
	framer_start(link);
	link->set_timer(0, Link::Timer::None);
}

/* the DTE's next character, queued; false if it has none */
bool Async::tx_fill()
{
	uint8_t ch;

	if (link_->read_chars(&ch, 1) != 1)
		return false;
	queue_char(tx_, ch);
	return true;
}

/* the rest of the n items while the DTE has nothing: what is left of
   its last character, completed with marks, then marks; how many */
int Async::tx_idle(int nbits, uint8_t mask, uint8_t *&buf, int &n)
{
	int done = 0;

	if (tx_.bit && n > 0) {
		int marks = nbits - tx_.bit;
		*buf++ = (uint8_t)((tx_.data << marks | ((1u << marks) - 1)) &
				   mask);
		tx_.bit = 0;
		n--;
		done++;
	}
	for (; n > 0; n--, done++)
		*buf++ = mask;
	return done;
}

/* received characters to the DTE, each as its stop bit comes (lost if
   the DTE does not take it) */
void Async::rx_scan()
{
	uint8_t ch;

	while (take_char(rx_, ch))
		link_->write_chars(&ch, 1);
}


/*
 *    the detection phase (7.2/V.42)
 *
 */

/* T400 (750 ms, 9.1.1/V.42) in bits at the pump's rate */
static long t400(const Link *link)
{
	long rate = link->tx_rate();
	if (!rate)
		rate = NOMINAL_RATE;
	return rate * 3 / 4;
}

/* the phase begins: marks out, nothing seen; the originator sends its
   ODPs after 64 octets of marks and waits for an ADP, the answerer
   waits for ODPs (and answers them with "EC") until T400, three times
   T400 when an MNP LR may come */
void Detector::start(Link *link)
{
	framer_start(link);
	tx_count_ = 0;
	rx_count_ = RX_NONE;
	ec_enable_ = false;
	refused_ = false;
	rx_chars_ = 0;
	if (link->caller()) {
		tx_pattern_[0] = DC1_EVEN;
		tx_pattern_[1] = DC1_ODD;
		rx_pattern_[0] = ADP_E;
		link->set_timer(DETECT_MARKS, Link::Timer::DetectBegin);
	} else {
		tx_pattern_[0] = ADP_E;
		tx_pattern_[1] = ADP_C;
		rx_pattern_[0] = DC1_EVEN;
		long t = t400(link);
		link->set_timer(link->asked().mnp ? 3 * t : t,
				Link::Timer::DetectEnd);
	}
}

/* the originator's marks are out: its ODPs from now on, until an ADP
   or T400 */
void Detector::begin()
{
	long t = t400(link_);

	link_->logf("detect: ODPs, T400 %ld bits.", t);
	tx_count_ = 1;
	link_->set_timer(t, Link::Timer::DetectEnd);
}

/* the phase is over: LAPM if it saw V.42 at the other end, else MNP
   unless the answerer refused error correction */
void Detector::finish()
{
	link_->logf("detect: done, %s.", ec_enable_ ? "V.42" :
		    refused_ ? "no error correction" : "no V.42");
	link_->detected(ec_enable_, !refused_);
}

/* a unit of 9 marks; with the patterns on, one character of the pair
   after them (the second first, then each in turn), whose stop bit is
   the first mark of the next unit */
bool Detector::tx_fill()
{
	if (!tx_count_) {
		queue_bits(tx_, 0x1ff, 9);
		return true;
	}
	uint8_t ch = tx_pattern_[tx_count_++ & 1];
	queue_bits(tx_, 0x1ffu << 9 | reverse_bits(ch), 18);
	return true;
}

/* the characters received: SYN DLE STX at an answerer that may take
   MNP hands the rest over at once; an HDLC flag means V.42 and ends the
   phase 36 bits on (armed once); else the patterns: an ADP decides at
   the originator ('E' then 'C': V.42, 'E' then NUL: refused), an ODP at
   the answerer (answered with ADPs for 16 patterns when it has V.42,
   passed over without), anything else starts the search again */
void Detector::rx_scan()
{
	uint8_t ch;

	while (take_char(rx_, ch)) {
		link_->logf("detect: rx 0x%02x.", ch);
		rx_chars_ = (rx_chars_ << 8 | ch) & 0xffffff;
		if (rx_chars_ == MNP_START && !link_->caller() &&
		    link_->asked().mnp) {
			link_->mnp_lr_seen(rx_);
			return;
		}
		if (ch == FLAG) {
			link_->logf("detect: HDLC flag.");
			ec_enable_ = true;
			if (link_->timer_what() != Link::Timer::DetectEnd ||
			    link_->timer() > PATTERN_BITS)
				link_->set_timer(PATTERN_BITS,
						 Link::Timer::DetectEnd);
			continue;
		}
		switch (rx_count_) {
		case RX_NONE:
			if (ch == rx_pattern_[0])
				rx_count_ = RX_FIRST;
			break;
		case RX_FIRST:
			rx_count_ = RX_NONE;
			if (ch == DC1_ODD) {
				if (link_->caller() || !link_->asked().ec)
					break;
				tx_count_ = 1;
				rx_count_ = RX_DECIDED;
				ec_enable_ = true;
				link_->set_timer(ADP_BITS,
						 Link::Timer::DetectEnd);
			} else if (ch == ADP_NUL) {
				rx_count_ = RX_DECIDED;
				ec_enable_ = false;
				refused_ = true;
				link_->set_timer(PATTERN_BITS,
						 Link::Timer::DetectEnd);
			} else if (ch == ADP_C) {
				rx_count_ = RX_DECIDED;
				ec_enable_ = true;
				link_->set_timer(PATTERN_BITS,
						 Link::Timer::DetectEnd);
			}
			break;
		default:
			break;
		}
	}
}


/*
 *    HDLC frames (8.1/V.42)
 *
 */

/* the FCS-16 (x^16 + x^12 + x^5 + 1, the register reflected) after one
   more octet */
static uint16_t fcs_add(uint16_t fcs, uint8_t c)
{
	fcs ^= c;
	for (int i = 0; i < 8; i++)
		fcs = fcs & 1 ? (uint16_t)(fcs >> 1 ^ 0x8408) :
			(uint16_t)(fcs >> 1);
	return fcs;
}

/* f empty, for the next frame */
static void frame_reset(Frame &f)
{
	f.count = 0;
	f.fcs = FCS_INIT;
}

/* a flag has ended frame f (bits: the data bits after its last whole
   octet): a good one to framer (its octets without the FCS, counted),
   one with a bad FCS reported to it (counted), the rest dropped */
static void frame_end(Link *link, Framer *framer, Frame &f, int bits)
{
	if (!f.count)
		return;
	if (bits) {
		link->logf("hdlc: rx frame of %d octets and %d bits.",
			   f.count, bits);
		return;
	}
	if (f.count < MIN_FRAME) {
		link->logf("hdlc: rx frame too short (%d octets).", f.count);
		return;
	}
	if (f.fcs != FCS_GOOD) {
		link->logf("hdlc: rx frame fcs error (%d octets).", f.count);
		if (framer) {
			link->stats().rx_errors++;
			framer->rx_error();
		}
		return;
	}
	if (framer) {
		link->stats().rx_frames++;
		f.count -= 2;
		framer->rx_complete(&f);
	}
}

/* a call: no framer (flags out, frames in dropped), the receiver
   hunting for a flag, no bit timer */
void Hdlc::start(Link *link)
{
	framer_start(link);
	framer_ = nullptr;
	tx_on_ = false;
	tx_frame_ = nullptr;
	tx_ones_ = 0;
	rx_frame_ = {};
	rx_frame_.buf = rx_buf_;
	rx_frame_.size = sizeof(rx_buf_);
	frame_reset(rx_frame_);
	rx_error_ = 1;
	rx_ones_ = 0;
	link->set_timer(0, Link::Timer::None);
}

/* octet in, least significant bit first, with a 0 after each fifth 1
   in a row (the run going on from the octet before) */
void Hdlc::put_byte(uint8_t in)
{
	for (int i = 0; i < 8; i++, in >>= 1) {
		int b = in & 1;
		queue_bits(tx_, (uint32_t)b, 1);
		if (!b) {
			tx_ones_ = 0;
		} else if (++tx_ones_ == 5) {
			queue_bits(tx_, 0, 1);
			tx_ones_ = 0;
		}
	}
}

/* the next unit: between frames a flag, after which the framer's next
   frame starts (when it has one and may send); in a frame its next
   octet, then the FCS, at which the framer hears that the frame is out
   (the flag after it closes it) */
bool Hdlc::tx_fill()
{
	if (!tx_frame_) {
		queue_bits(tx_, FLAG, 8);
		if (framer_ && tx_on_ &&
		    (tx_frame_ = framer_->next_tx_frame())) {
			tx_frame_->count = 0;
			tx_frame_->fcs = FCS_INIT;
			link_->stats().tx_frames++;
		}
		return true;
	}
	if (tx_frame_->count < tx_frame_->size) {
		uint8_t c = tx_frame_->buf[tx_frame_->count++];
		tx_frame_->fcs = fcs_add(tx_frame_->fcs, c);
		put_byte(c);
		return true;
	}
	uint16_t fcs = (uint16_t)~tx_frame_->fcs;
	put_byte((uint8_t)fcs);
	put_byte((uint8_t)(fcs >> 8));
	tx_ones_ = 0;
	Frame *f = tx_frame_;
	tx_frame_ = nullptr;
	framer_->tx_complete(f);
	return true;
}

/* one step of the receiver once 18 bits are in: 8 bit positions while
   hunting, else the bits of one octet (8 data bits and the zeros
   removed among them); a flag ends the step, and the frame before it;
   six 1s abort the frame (hunting for the rest of the step); an octet
   past the buffer drops the frame */
void Hdlc::rx_scan()
{
	uint8_t octet = 0;
	int n = 0;              /* data bits, or positions while hunting */

	if (rx_.bit < RX_LOOKAHEAD)
		return;
	while (n < 8) {
		if (!rx_error_ && rx_ones_ == 5) {
			/* the bit after five 1s: an inserted 0, else an
			   abort */
			rx_ones_ = 0;
			if (take_bit(rx_))
				rx_error_ = 1;
			continue;
		}
		if (peek_octet(rx_) == FLAG) {
			rx_.bit -= 8;
			if (!rx_error_)
				frame_end(link_, framer_, rx_frame_, n);
			frame_reset(rx_frame_);
			rx_error_ = 0;
			rx_ones_ = 0;
			return;
		}
		int b = take_bit(rx_);
		if (!rx_error_) {
			rx_ones_ = b ? rx_ones_ + 1 : 0;
			octet |= (uint8_t)(b << n);
		}
		n++;
	}
	if (rx_error_)
		return;
	if (rx_frame_.count == rx_frame_.size) {
		link_->logf("hdlc: rx frame overflow.");
		rx_error_ = 1;
		return;
	}
	rx_frame_.buf[rx_frame_.count++] = octet;
	rx_frame_.fcs = fcs_add(rx_frame_.fcs, octet);
}

/* the loops of the four framers (MnpAsync's hooks are in mnp.cpp) */
template class BitFramer<Async>;
template class BitFramer<Detector>;
template class BitFramer<Hdlc>;
template class BitFramer<MnpAsync>;

} // namespace dlink
