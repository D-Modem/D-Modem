/*
 *
 *    hdlc.h - bit level framing: HDLC frames (flags, zero insertion,
 *             FCS-16), asynchronous characters, and the V.42 detection
 *             phase (ODP/ADP)
 *
 *    Bits come and go nbits at a time in the low bits of each byte, the
 *    first one on the line in the most significant of them (the data
 *    pumps use nbits = 1).
 *
 */

#ifndef DMODEM_LINK_HDLC_H
#define DMODEM_LINK_HDLC_H

#include <stdint.h>

namespace dlink {

class Link;

/* a frame: address, control, information (no FCS) */
struct Frame {
	Frame *next;            /* the frame lists of LAPM are rings */
	int count;              /* bytes sent or received so far */
	int size;               /* to send / room */
	uint16_t fcs;
	uint8_t *buf;
};

/* the bits not yet given out or taken in */
struct BitQueue {
	int bit = 0;
	uint32_t data = 0;
	void clear() { bit = 0; data = 0; }
};

/* who sends and receives the frames */
class Framer {
public:
	virtual Frame *next_tx_frame() = 0;
	virtual void tx_complete(Frame *f) = 0;
	virtual void rx_complete(Frame *f) = 0;
protected:
	~Framer() = default;
};

class Hdlc {
public:
	void start(Link *link);
	/* the framer: nothing to send without one (flags), received frames
	   go nowhere */
	void set_framer(Framer *f, bool tx) { framer_ = f; tx_on_ = tx; }
	void enable_tx(bool tx) { tx_on_ = tx; }
	int get_bits(int nbits, uint8_t *buf, int n);
	int put_bits(int nbits, const uint8_t *buf, int n);

private:
	void put_byte(uint8_t in);

	Link *link_ = nullptr;
	Framer *framer_ = nullptr;
	bool tx_on_ = false;
	BitQueue tx_, rx_;
	Frame *tx_frame_ = nullptr;
	Frame rx_frame_ = {};
	uint8_t rx_buf_[512];
	int rx_error_ = 0;
	int rx_ones_ = 0, tx_ones_ = 0;
};

/* start bit, 8 data bits (least significant first), stop bit */
class Async {
public:
	void start(Link *link);
	int get_bits(int nbits, uint8_t *buf, int n);
	int put_bits(int nbits, const uint8_t *buf, int n);

private:
	Link *link_ = nullptr;
	BitQueue tx_, rx_;
};

/* 8.10/V.42: the originator sends ODPs, the answerer ADPs once it has
   seen two; HDLC flags from the remote end it too */
class Detector {
public:
	void start(Link *link);
	int get_bits(int nbits, uint8_t *buf, int n);
	int put_bits(int nbits, const uint8_t *buf, int n);
	bool ec_enable() const { return ec_enable_; }
	/* the bit timers */
	static void begin(Link *link);
	static void finish(Link *link);

private:
	Link *link_ = nullptr;
	BitQueue tx_, rx_;
	int tx_count_ = 0;
	uint8_t tx_pattern_[2] = {};
	int rx_count_ = 0;
	uint8_t rx_pattern_[2] = {};
	bool ec_enable_ = false;
};

} // namespace dlink

#endif
