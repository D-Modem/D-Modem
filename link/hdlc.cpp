/*
 *
 *    hdlc.cpp - HDLC framing, asynchronous characters and the V.42
 *               detection phase
 *
 *    From slmodemd's modem_pack.c:
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "link.h"

namespace dlink {

static const uint8_t HDLC_FLAG = 0x7e;

/* the line sends the least significant bit of a character first */
static uint8_t reverse_bits(uint8_t c)
{
	static const uint8_t nibble[16] = {
		0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
		0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf };
	return (uint8_t)(nibble[c & 0xf] << 4 | nibble[c >> 4]);
}

/* bits from the queue, nbits at a time; the number of items filled */
static int take_bits(BitQueue *s, int nbits, uint8_t mask, uint8_t *&buf,
		     int &n)
{
	int ret = 0;
	while (n > 0 && s->bit >= nbits) {
		*buf++ = (uint8_t)(s->data >> (s->bit - nbits)) & mask;
		s->bit -= nbits;
		ret++;
		n--;
	}
	return ret;
}


/*
 *    asynchronous characters
 *
 */

static const int CHAR_SIZE = 10;        /* start, 8 data, stop */
static const int STOP_BITS = 1;

/* a call in async mode: empty queues, no bit timer */
void Async::start(Link *link)
{
	link_ = link;
	tx_.clear();
	rx_.clear();
	link->set_timer(0, nullptr);
}

/* n items of nbits bits for the data pump: the DTE's characters as
   start bit, 8 data bits (least significant first) and stop bit, marks
   when there are none; returns n */
int Async::get_bits(int nbits, uint8_t *buf, int n)
{
	int ret = 0;
	uint8_t ch;
	for (;;) {
		ret += take_bits(&tx_, nbits, 0xff, buf, n);
		if (n <= 0)
			break;
		if (link_->read_chars(&ch, 1) != 1)
			break;
		ch = reverse_bits(ch);
		tx_.data <<= CHAR_SIZE;
		tx_.data |= (uint32_t)ch << 1 | 1;
		tx_.bit += CHAR_SIZE;
	}
	if (tx_.bit && n > 0) {
		/* what is left, then stop bits */
		*buf++ = (uint8_t)(tx_.data << (nbits - tx_.bit) |
				   ((1 << (nbits - tx_.bit)) - 1));
		ret++;
		n--;
		tx_.bit = 0;
	}
	for (; n > 0; n--, ret++)
		*buf++ = (uint8_t)((1 << nbits) - 1);
	return ret;
}

/* n items of nbits bits from the data pump: marks skipped up to a start
   bit, then a character to the DTE every 10 bits (the stop bit is not
   checked; a character the DTE does not take is lost); returns n */
int Async::put_bits(int nbits, const uint8_t *buf, int n)
{
	int ret = 0;
	uint8_t ch;
	while (n > 0) {
		rx_.data <<= nbits;
		rx_.data |= *buf++ & ((1 << nbits) - 1);
		rx_.bit += nbits;
		n--;
		ret++;
		/* marks before a start bit */
		while (rx_.bit && rx_.data & (1u << (rx_.bit - 1)))
			rx_.bit--;
		while (rx_.bit >= CHAR_SIZE) {
			ch = (uint8_t)(rx_.data >>
				       (rx_.bit - (CHAR_SIZE - STOP_BITS)));
			rx_.bit -= CHAR_SIZE;
			ch = reverse_bits(ch);
			link_->write_chars(&ch, 1);
		}
	}
	return ret;
}


/*
 *    the detection phase
 *
 *    originator: transmits ODPs until an ADP or flags come (it has no
 *    T400 here: the modem core skips the phase at the originator);
 *    answerer: transmits marks until the phase ends, at least 10 ADPs
 *    after an ODP, or until continuous flags come.
 *
 */

/* ODP: 0100010001 11111111 0100010011 11111111 */
static const uint8_t ODP0 = 0x88, ODP1 = 0x89;
/* ADP V.42: 0101000101 11111111 0110000101 11111111,
   ADP V.14: 0101000101 11111111 0000000001 11111111 */
static const uint8_t ADP0 = 0xa2, ADP1_V14 = 0x00, ADP1_V42 = 0xc2;

static const int MAX_TX_PATTERNS = 16;
static const int PATTERN_SIZE = 36;

/* the originator's bit timer: its ODPs start after 64 octets of marks */
void Detector::begin(Link *link)
{
	link->logf("detector start...");
	link->det_.tx_count_ = 1;
}

/* the bit timer at the end of the phase: LAPM if an ADP (V.42), ODPs or
   flags came, else async */
void Detector::finish(Link *link)
{
	link->logf("detector finished.");
	link->detected(link->det_.ec_enable_);
}

/* the phase at the start of the data mode: the originator waits 64
   octets of marks for its ODPs, the answerer sends marks until ODPs
   come, at most T400 */
void Detector::start(Link *link)
{
	/* T400 (750 ms) at the data pump's rate (a nominal 28800 bit/s
	   if not known: 18 s at 1200 bit/s, longer than a remote without
	   V.42 waits for our CONNECT) */
	const int rate = link->tx_rate() ? link->tx_rate() : 28800;
	link_ = link;
	tx_count_ = 0;          /* mute */
	rx_count_ = 0;
	ec_enable_ = false;
	if (link->caller()) {
		tx_pattern_[0] = ODP0;
		tx_pattern_[1] = ODP1;
		rx_pattern_[0] = ADP0;
		rx_pattern_[1] = ADP1_V42;
		link->set_timer(64 * 8, begin);
	} else {
		tx_pattern_[0] = ADP0;
		tx_pattern_[1] = ADP1_V42;
		rx_pattern_[0] = ODP0;
		rx_pattern_[1] = ODP1;
		link->set_timer(rate * 3 / 4, finish);
	}
	tx_.clear();
	rx_.clear();
}

/* the phase's transmitter: 9 marks, then (once sending) the next
   pattern character as 9 bits, a start bit and its 8 bits (its stop
   bit the first of the next marks); returns n */
int Detector::get_bits(int nbits, uint8_t *buf, int n)
{
	int ret = 0;
	for (;;) {
		ret += take_bits(&tx_, nbits, 0xff, buf, n);
		if (n <= 0)
			break;
		/* 9 marks, and a pattern character when sending */
		tx_.data <<= 9;
		tx_.data |= 0x1ff;
		tx_.bit += 9;
		if (tx_count_ > 0) {
			tx_.data <<= 9;
			tx_.data |= tx_pattern_[tx_count_++ % 2];
			tx_.bit += 9;
		}
	}
	return ret;
}

/* the phase's receiver: characters after marks, as in async; a flag
   ends the phase one pattern later (LAPM); at the answerer the second
   ODP starts 16 ADPs and LAPM, at the originator the second ADP ends
   the phase (V.42: LAPM, V.14: async); returns n */
int Detector::put_bits(int nbits, const uint8_t *buf, int n)
{
	int ret = 0;
	uint8_t ch;
	while (n > 0) {
		rx_.data <<= nbits;
		rx_.data |= *buf++ & ((1 << nbits) - 1);
		rx_.bit += nbits;
		n--;
		ret++;
		/* marks */
		while (rx_.bit && rx_.data & (1u << (rx_.bit - 1)))
			rx_.bit--;
		while (rx_.bit >= 10) {
			ch = (uint8_t)(rx_.data >> (rx_.bit - 9));
			rx_.bit -= 10;
			link_->logf("rx pattern: 0x%02x.", ch);
			if (ch == HDLC_FLAG) {
				link_->logf("hdlc flag detected.");
				ec_enable_ = true;
				/* once: re-arming on every flag would hold
				   the detector until the flags end, and the
				   frame after them would be lost */
				if (link_->timer_fn() != finish ||
				    link_->timer() > PATTERN_SIZE)
					link_->set_timer(PATTERN_SIZE, finish);
				continue;
			}
			if (!rx_count_) {
				rx_count_ = ch == rx_pattern_[0];
				continue;
			}
			if (rx_count_ >= 2)
				continue;
			switch (ch) {
			case ODP1:
				if (!link_->caller()) {
					/* answer with ADPs, then go on */
					tx_count_ = 1;
					rx_count_ = 2;
					ec_enable_ = true;
					link_->set_timer(MAX_TX_PATTERNS *
							 PATTERN_SIZE, finish);
				} else
					rx_count_ = 0;
				break;
			case ADP1_V14:
				rx_count_ = 2;
				ec_enable_ = false;
				link_->set_timer(PATTERN_SIZE, finish);
				break;
			case ADP1_V42:
				rx_count_ = 2;
				ec_enable_ = true;
				link_->set_timer(PATTERN_SIZE, finish);
				break;
			default:
				rx_count_ = 0;
				break;
			}
		}
	}
	return ret;
}


/*
 *    HDLC
 *
 */

static const uint16_t fcs16_tab[256] = {
	0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
	0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
	0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
	0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
	0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
	0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
	0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
	0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
	0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
	0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
	0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
	0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
	0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
	0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
	0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
	0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
	0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
	0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
	0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
	0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
	0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
	0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
	0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
	0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
	0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
	0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
	0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
	0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
	0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
	0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
	0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
	0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

static const uint16_t INIT_FCS = 0xffff, GOOD_FCS = 0xf0b8;

/* the FCS-16 (x^16 + x^12 + x^5 + 1, bits reflected) after one more
   octet */
static uint16_t fcs_add(uint16_t fcs, uint8_t c)
{
	return (uint16_t)(fcs >> 8 ^ fcs16_tab[(fcs ^ c) & 0xff]);
}

/* LAPM's framing: no framer and no frame yet (flags go out), the
   receiver hunting for a flag */
void Hdlc::start(Link *link)
{
	link_ = link;
	framer_ = nullptr;
	tx_on_ = false;
	tx_frame_ = nullptr;
	rx_ones_ = tx_ones_ = 0;
	/* wait for a flag before the first frame */
	rx_error_ = 1;
	rx_frame_ = {};
	rx_frame_.buf = rx_buf_;
	rx_frame_.size = sizeof(rx_buf_);
	rx_frame_.fcs = INIT_FCS;
	tx_.clear();
	rx_.clear();
	link->set_timer(0, nullptr);
}

/* a byte, least significant bit first, a 0 after five 1s */
void Hdlc::put_byte(uint8_t in)
{
	for (int i = 0; i < 8; i++) {
		tx_.data <<= 1;
		tx_.bit++;
		if (in & 1) {
			tx_ones_++;
			tx_.data |= 1;
			if (tx_ones_ == 5) {
				tx_.data <<= 1;
				tx_.bit++;
				tx_ones_ = 0;
			}
		} else
			tx_ones_ = 0;
		in >>= 1;
	}
}

/* n items of nbits bits for the data pump: flags between frames, the
   framer's next frame (while tx is on) with zeros inserted and its
   FCS, tx_complete() once it is out; returns n */
int Hdlc::get_bits(int nbits, uint8_t *buf, int n)
{
	int ret = 0;
	const uint8_t mask = (uint8_t)((1 << nbits) - 1);

	for (;;) {
		ret += take_bits(&tx_, nbits, mask, buf, n);
		if (n <= 0)
			break;
		if (!tx_frame_) {
			tx_.data <<= 8;
			tx_.data |= HDLC_FLAG;
			tx_.bit += 8;
			if (framer_ && tx_on_ &&
			    (tx_frame_ = framer_->next_tx_frame())) {
				tx_frame_->fcs = INIT_FCS;
				tx_frame_->count = 0;
			}
		} else if (tx_frame_->count == tx_frame_->size) {
			/* the FCS, then the frame is out */
			tx_frame_->fcs ^= 0xffff;
			put_byte((uint8_t)(tx_frame_->fcs & 0xff));
			put_byte((uint8_t)(tx_frame_->fcs >> 8));
			tx_ones_ = 0;
			framer_->tx_complete(tx_frame_);
			tx_frame_ = nullptr;
		} else {
			uint8_t in = tx_frame_->buf[tx_frame_->count++];
			tx_frame_->fcs = fcs_add(tx_frame_->fcs, in);
			put_byte(in);
		}
	}
	return ret;
}

/* n items of nbits bits from the data pump: flags delimit the frames,
   inserted zeros are removed, six 1s outside a flag abort the frame;
   a frame of whole octets, of at least 3 with the FCS and a good FCS,
   goes to the framer's rx_complete() without it; returns n */
int Hdlc::put_bits(int nbits, const uint8_t *buf, int n)
{
	int ret = 0;
	const uint8_t mask = (uint8_t)((1 << nbits) - 1);
	Frame *f = &rx_frame_;

	while (n > 0) {
		rx_.data <<= nbits;
		rx_.data |= *buf++ & mask;
		rx_.bit += nbits;
		n--;
		ret++;

		/* a character, 2 possible zeros inserted, 8 for a flag */
		if (rx_.bit < 8 + 8 + 2)
			continue;

		uint8_t out = 0;
		int i;
		for (i = 0; i < 8; i++) {
			uint8_t in = (uint8_t)(rx_.data >> (rx_.bit - 8));
			if (in == HDLC_FLAG) {
				/* the end of a frame */
				if (!f->count)
					;
				else if (rx_error_)
					link_->logf("hdlc frame error.");
				else if (i)
					link_->logf("hdlc frame integrity "
						    "error.");
				else if (f->count < 3)
					link_->logf("hdlc frame size error.");
				else if (f->fcs != GOOD_FCS)
					link_->logf("hdlc frame fcs error.");
				else if (framer_) {
					f->count -= 2;  /* the FCS */
					framer_->rx_complete(f);
				}
				rx_.bit -= 8;
				rx_ones_ = 0;
				rx_error_ = 0;
				f->fcs = INIT_FCS;
				f->count = 0;
				break;
			} else if (rx_error_) {
				rx_.bit--;
				continue;
			}
			out >>= 1;
			if (in & 0x80) {
				out |= 0x80;
				rx_ones_++;
				/* an inserted zero, or an abort / idle */
				if (rx_ones_ == 5) {
					if (in & 0x40) {
						rx_error_++;
						f->count = 0;
					}
					rx_.bit--;
					rx_ones_ = 0;
				}
			} else
				rx_ones_ = 0;
			rx_.bit--;
		}

		if (i == 8 && !rx_error_) {
			if (f->count < f->size) {
				f->buf[f->count++] = out;
				f->fcs = fcs_add(f->fcs, out);
			} else {
				link_->logf("hdlc rx frame overflow.");
				rx_error_++;
				f->count = 0;
			}
		}
	}
	return ret;
}

} // namespace dlink
