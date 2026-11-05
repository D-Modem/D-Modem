/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hdlc.h - bit level framing: HDLC frames (flags, zero insertion,
 *             FCS-16 or FCS-32), asynchronous characters, and the V.42
 *             detection phase (ODP/ADP)
 *
 *    Bits come and go nbits at a time in the low bits of each byte, the
 *    first one on the line in the most significant of them (the data
 *    pumps use nbits = 1).
 *
 */

#ifndef DMODEM_LINK_HDLC_H
#define DMODEM_LINK_HDLC_H


#include "dm_types.h"

namespace dlink {

class Link;

/* the line sends the least significant bit of a character first */
inline u8 reverse_bits(u8 c)
{
	static const u8 nibble[16] = {
		0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
		0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf };
	return static_cast<u8>(nibble[c & 0xf] << 4 | nibble[c >> 4]);
}

/* a frame: address, control, information (no FCS) */
struct Frame {
	Frame *next;            /* the frame lists of LAPM are rings */
	s16 count;              /* bytes sent or received so far */
	s16 size;               /* to send / room */
	u16 fcs;
	u8 *buf;
	/* a received frame: it came with an FCS-32 (else an FCS-16) */
	bool fcs32 = false;
};

/* the bits not yet given out or taken in */
struct BitQueue {
	s16 bit = 0;
	u32 data = 0;
	/* empty */
	void clear() { bit = 0; data = 0; }
};

/* the k low bits of bits (the first on the line the most significant
   of them) queued in q after those already there */
inline void queue_bits(BitQueue &q, u32 bits, s16 k)
{
	q.data = q.data << k | bits;
	q.bit += k;
}

/* the oldest bit queued in q, taken */
inline s16 take_bit(BitQueue &q)
{
	return static_cast<s16>(q.data >> --q.bit & 1);
}

/* the 8 oldest bits queued in q (at least 8), left in it: the oldest
   in the most significant */
inline u8 peek_octet(const BitQueue &q)
{
	return static_cast<u8>(q.data >> (q.bit - 8));
}

/* the FCS-16 register before a frame, and after a whole good frame,
   its FCS included (8.1.1.6.1/V.42) */
const u16 FCS16_INIT = 0xffff, FCS16_GOOD = 0xf0b8;

/* the FCS-16 (x^16 + x^12 + x^5 + 1, the register reflected) after one
   more octet */
inline u16 fcs16_add(u16 fcs, u8 c)
{
	fcs ^= c;
	for (s16 i = 0; i < 8; i++)
		fcs = fcs & 1 ? static_cast<u16>(fcs >> 1 ^ 0x8408) :
			static_cast<u16>(fcs >> 1);
	return fcs;
}

/* the FCS-32 register before a frame, and after a whole good frame,
   its FCS included (8.1.1.6.2/V.42) */
const u32 FCS32_INIT = 0xffffffff, FCS32_GOOD = 0xdebb20e3;

/* the FCS-32 (x^32 + x^26 + ... + x + 1, the register reflected) after
   one more octet */
inline u32 fcs32_add(u32 fcs, u8 c)
{
	fcs ^= c;
	for (s16 i = 0; i < 8; i++)
		fcs = fcs & 1 ? fcs >> 1 ^ 0xedb88320 : fcs >> 1;
	return fcs;
}

/* the FCSs an Hdlc receiver accepts (a mask) */
enum : u8 { RX_FCS16 = 1, RX_FCS32 = 2 };

/* who sends and receives the frames */
class Framer {
public:
	/* the next frame to send (its buf and size), NULL for none (flags) */
	virtual Frame *next_tx_frame() = 0;
	/* f is out on the line */
	virtual void tx_complete(Frame *f) = 0;
	/* a good frame: f->count octets without the FCS, in a buffer the next
	   frame reuses */
	virtual void rx_complete(Frame *f) = 0;
	/* a frame with a bad FCS (MNP answers it; LAPM does not) */
	virtual void rx_error() {}
protected:
	~Framer() = default;
};

/* the Link's protocol in the data phase (LAPM, or V.76's multiplexer
   over it; X.75 / V.120; MNP; PPP): what the Link asks of whichever is
   on (Link::active()) */
class LinkProto {
public:
	/* we hang up: DISC, MNP's LD (PPP: nothing) */
	virtual void disconnect() {}
	/* the characters the DTE did not take: to it again */
	virtual void push_rest() {}
	/* frames not sent yet or not acknowledged */
	virtual bool tx_pending() const { return false; }
protected:
	~LinkProto() = default;
};

/* what the three framers share, a mixin on the framer F (CRTP): the
   link, the bit queues each way, and the loops of get_bits() and
   put_bits().  Out: the queue's bits nbits at a time, F's tx_fill()
   queueing more until n items are out or it has nothing more, then F's
   tx_idle() for the rest (none by default).  In: nbits at a time into
   rx_, F's rx_scan() after each item. */
template <class F>
class BitFramer {
public:
	/* n items of nbits bits for the data pump, and from it; how many */
	s32 get_bits(s16 nbits, u8 *buf, s32 n);
	s32 put_bits(s16 nbits, const u8 *buf, s32 n);
	/* the received bits not taken yet (for the framer after this one) */
	const BitQueue &rx_queue() const { return rx_; }

protected:
	/* a call: the link, empty queues */
	void framer_start(Link *link)
	{
		link_ = link;
		tx_.clear();
		rx_.clear();
	}
	/* (F's) the rest of the n items when tx_fill() has nothing: none
	   here; how many it filled */
	s32 tx_idle(s16 nbits, u8 mask, u8 *&buf, s32 &n)
	{
		(void)nbits;
		(void)mask;
		(void)buf;
		(void)n;
		return 0;
	}

	Link *link_ = nullptr;
	BitQueue tx_, rx_;
};

class Hdlc : public BitFramer<Hdlc> {
public:
	void start(Link *link);
	/* the framer: nothing to send without one (flags), received frames
	   go nowhere */
	void set_framer(Framer *f, bool tx) { framer_ = f; tx_on_ = tx; }
	/* the framer's frames go out, or only flags */
	void enable_tx(bool tx) { tx_on_ = tx; }
	/* the FCS of the frames that start from now on: FCS-32, or FCS-16
	   (as after start()) */
	void set_tx_fcs32(bool on) { tx_fcs32_ = on; }
	/* the frames that start from now on have an FCS-32 */
	bool tx_fcs32() const { return tx_fcs32_; }
	/* the FCSs a received frame may have (RX_FCS16, RX_FCS32; FCS-16
	   only after start()): with both, a frame is good if either is
	   (8.10.2/V.42), and Frame::fcs32 says which */
	void set_rx_fcs(u8 mask) { rx_fcs_ = mask; }
	/* the FCSs a received frame may have */
	u8 rx_fcs() const { return rx_fcs_; }

private:
	friend class BitFramer<Hdlc>;
	bool tx_fill();
	void rx_scan();
	void put_byte(u8 in);

	Framer *framer_ = nullptr;
	bool tx_on_ = false;
	Frame *tx_frame_ = nullptr;
	Frame rx_frame_ = {};
	/* the longest frame received: X.75's or V.120's address, control
	   and header, 2048 octets of information, and the FCS (32 bits at
	   most) */
	u8 rx_buf_[5 + 2048 + 4];
	bool tx_fcs32_ = false, tx_frame32_ = false;
	u32 tx_fcs32_reg_ = 0;
	s16 tx_fcs32_left_ = 0;         /* FCS-32 octets still to go */
	u8 rx_fcs_ = RX_FCS16;
	u32 rx_fcs32_reg_ = 0;
	s16 rx_error_ = 0;
	s16 rx_ones_ = 0, tx_ones_ = 0;
};

/* start bit, 8 data bits (least significant first), stop bit */
class Async : public BitFramer<Async> {
public:
	void start(Link *link);

private:
	friend class BitFramer<Async>;
	bool tx_fill();
	s32 tx_idle(s16 nbits, u8 mask, u8 *&buf, s32 &n);
	void rx_scan();
};

/* 7.2/V.42: the originator sends ODPs, the answerer ADPs once it has
   seen two; HDLC flags from the remote end it too, and at the answerer
   an MNP LR's start (SYN DLE STX) */
class Detector : public BitFramer<Detector> {
public:
	void start(Link *link);
	/* LAPM, as far as the phase has seen */
	bool ec_enable() const { return ec_enable_; }
	/* the bit timers (Link::Timer::DetectBegin, DetectEnd) */
	void begin();
	void finish();

private:
	friend class BitFramer<Detector>;
	bool tx_fill();
	void rx_scan();

	s16 tx_count_ = 0;
	u8 tx_pattern_[2] = {};
	s16 rx_count_ = 0;
	u8 rx_pattern_[2] = {};
	bool ec_enable_ = false;
	bool refused_ = false;          /* ADP "E" NUL: no error correction */
	u32 rx_chars_ = 0;              /* the last three characters */
};

} // namespace dlink

#endif
