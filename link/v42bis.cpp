/*
 *
 *    v42bis.cpp - V.42bis data compression
 *
 *    From slmodemd's modem_comp.c:
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

#include <string.h>

#include <new>

#include "v42bis.h"

namespace dlink {

/* limits of the protocol */
static const int MIN_DICT_SIZE = 512;
static const int MIN_STR_SIZE = 6;
static const int MAX_STR_SIZE = 250;

static const int CHAR_SIZE = 8;                         /* N3 */
static const int TOTAL_CHARS = 256;                     /* N4 */
static const int TOTAL_CTRLS = 3;                       /* N6 */
static const int FIRST_ENTRY = TOTAL_CTRLS;
static const int FIRST_FREE = TOTAL_CHARS + FIRST_ENTRY; /* N5 */

/* control codewords */
enum { CCW_ETM = 0, CCW_FLUSH = 1, CCW_STEPUP = 2 };
/* command codes after the escape character */
enum { CC_ECM = 0, CC_EID = 1, CC_RESET = 2 };

static const int ESCAPE_STEP = 51;
static const int TEST_SLICE = 256;      /* compression test (characters) */


/*
 *    the dictionary
 *
 */

void V42bis::dict_init()
{
	for (int i = 0; i < dict_size_; i++) {
		dict_[i].parent = 0;
		dict_[i].child = 0;
		dict_[i].next = 0;
	}
	next_free_ = FIRST_FREE;
	last_matched_ = 0;
	update_at_ = 0;
	last_added_ = 0;
	cw_size_ = CHAR_SIZE + 1;
	threshold_ = TOTAL_CHARS << 1;
	bit_len_ = 0;
	flushed_len_ = 0;
	str_len_ = 0;
	escape_char_ = 0;
	cmp_last_ = TEST_SLICE << 3;
	mode_ = TRANSPARENT;
	escape_ = false;
}

uint16_t V42bis::match_char(uint16_t at, uint8_t ch) const
{
	if (!at)
		return (uint16_t)(ch + FIRST_ENTRY);
	for (uint16_t e = dict_[at].child; e; e = dict_[e].next)
		if (dict_[e].ch == ch)
			return e;
	return 0;
}

uint16_t V42bis::add_char(uint16_t at, uint8_t ch)
{
	uint16_t added = next_free_, next;
	dict_[added].ch = ch;
	dict_[added].parent = at;
	dict_[added].child = 0;
	dict_[added].next = dict_[at].child;
	dict_[at].child = added;
	/* the next free entry: a leaf, taken out of its parent's list */
	next = added;
	do {
		if (++next == dict_size_)
			next = FIRST_FREE;
	} while (dict_[next].child);
	if (dict_[next].parent) {
		uint16_t e = dict_[next].parent;
		if (dict_[e].child == next)
			dict_[e].child = dict_[next].next;
		else {
			e = dict_[e].child;
			while (dict_[e].next != next)
				e = dict_[e].next;
			dict_[e].next = dict_[next].next;
		}
	}
	next_free_ = next;
	return added;
}


/*
 *    decoder
 *
 */

int V42bis::send_string()
{
	int ret = str_len_;
	memcpy(obuf_ + olen_, str_data_, str_len_);
	olen_ += ret;
	str_len_ = 0;
	flushed_len_ = 0;
	return ret;
}

int V42bis::recv_data(uint16_t cw)
{
	int ret = 0, i;
	uint16_t p;
	for (p = cw; p; ret++)
		p = dict_[p].parent;
	i = str_len_ + ret - 1;
	for (p = cw; p; p = dict_[p].parent)
		str_data_[i--] = dict_[p].ch;
	str_len_ += ret;
	return ret;
}

int V42bis::flush_decoder()
{
	int total = flushed_len_ + str_len_;
	int ret = send_string();
	flushed_len_ = total;
	return ret;
}

int V42bis::decode(uint8_t in)
{
	int ret = 0;
	uint16_t cw, p;
	bool consumed = false;

	while (!consumed) {
		switch (mode_) {
		case COMPRESSED:
			bit_data_ |= (uint32_t)in << bit_len_;
			bit_len_ += 8;
			consumed = true;
			while (bit_len_ >= cw_size_ && mode_ == COMPRESSED) {
				cw = (uint16_t)(bit_data_ & ((1u << cw_size_) - 1));
				bit_data_ >>= cw_size_;
				bit_len_ -= cw_size_;
				switch (cw) {
				case CCW_ETM:
					bit_len_ = 0;
					mode_ = TRANSPARENT;
					last_matched_ = 0;
					last_added_ = 0;
					break;
				case CCW_FLUSH:
					bit_len_ = 0;
					break;
				case CCW_STEPUP:
					cw_size_++;
					threshold_ <<= 1;
					if (cw_size_ > (dict_size_ >> 3))
						return -1;      /* C-ERROR */
					break;
				default:
					if (cw == next_free_)
						return -1;
					ret += recv_data(cw);
					if (update_at_) {
						uint8_t ch = str_data_[0];
						p = match_char(update_at_, ch);
						if (!p) {
							last_added_ = add_char(
								update_at_, ch);
							if (cw == next_free_)
								return -1;
						} else if (p == last_added_)
							last_added_ = 0;
					}
					update_at_ = str_len_ + flushed_len_ ==
						max_string_ ? 0 : cw;
					for (int j = 0; j < str_len_; j++)
						if (str_data_[j] == escape_char_)
							escape_char_ +=
								ESCAPE_STEP;
					send_string();
					break;
				}
			}
			break;
		case TRANSPARENT:
			if (escape_) {
				escape_ = false;
				switch (in) {
				case CC_ECM:
					ret += send_string();
					mode_ = COMPRESSED;
					update_at_ = last_matched_;
					last_matched_ = 0;
					return ret;
				case CC_EID:
					in = escape_char_;
					escape_char_ += ESCAPE_STEP;
					break;
				case CC_RESET:
					ret += send_string();
					dict_init();
					return ret;
				default:
					return -1;      /* C-ERROR */
				}
			} else if (in == escape_char_) {
				escape_ = true;
				return ret;
			}
			if (update_at_) {
				if (!match_char(update_at_, in))
					last_added_ = add_char(update_at_, in);
				update_at_ = 0;
			}
			cw = match_char(last_matched_, in);
			if (!cw) {
				update_at_ = last_matched_;
				ret += send_string();
				last_matched_ = 0;
				/* the character starts the next string */
			} else if (cw == last_added_) {
				last_added_ = 0;
				ret += send_string();
				last_matched_ = 0;
			} else {
				last_matched_ = cw;
				str_data_[str_len_++] = in;
				if (str_len_ + flushed_len_ == max_string_) {
					ret += send_string();
					last_matched_ = 0;
				}
				consumed = true;
			}
			break;
		}
	}
	return ret;
}


/*
 *    encoder
 *
 */

void V42bis::send_cw(uint16_t cw)
{
	bit_data_ |= (uint32_t)cw << bit_len_;
	bit_len_ += cw_size_;
}

void V42bis::align_bits()
{
	if (bit_len_ % 8)
		bit_len_ += 8 - bit_len_ % 8;
}

int V42bis::send_bits()
{
	int ret = 0;
	while (bit_len_ >= 8) {
		obuf_[olen_++] = (uint8_t)bit_data_;
		bit_data_ >>= 8;
		bit_len_ -= 8;
		ret++;
	}
	return ret;
}

int V42bis::send_data(uint16_t cw)
{
	int ret = 0;

	raw_bits_ += str_len_ << 3;
	cmp_bits_ += cw_size_;
	cmp_last_ += cw_size_ - cmp_last_ * str_len_ / TEST_SLICE;

	switch (mode_) {
	case TRANSPARENT:
		for (int i = 0; i < str_len_; i++) {
			obuf_[olen_++] = str_data_[i];
			ret++;
			if (str_data_[i] == escape_char_) {
				obuf_[olen_++] = CC_EID;
				ret++;
				escape_char_ += ESCAPE_STEP;
			}
		}
		break;
	case COMPRESSED:
		for (int i = 0; i < str_len_; i++)
			if (str_data_[i] == escape_char_)
				escape_char_ += ESCAPE_STEP;
		while (cw >= threshold_) {
			send_cw(CCW_STEPUP);
			ret += send_bits();
			threshold_ <<= 1;
			cw_size_++;
		}
		send_cw(cw);
		ret += send_bits();
		break;
	}
	str_len_ = 0;
	flushed_len_ = 0;
	return ret;
}

/* switch between transparent and compressed by the last TEST_SLICE
   characters */
int V42bis::test_mode()
{
	int ret = 0;
	switch (mode_) {
	case TRANSPARENT:
		if (cmp_last_ < (TEST_SLICE << 3) - 11) {
			if (last_matched_) {
				update_at_ = last_matched_;
				ret += send_data(last_matched_);
				last_matched_ = 0;
			}
			obuf_[olen_++] = escape_char_;
			obuf_[olen_++] = CC_ECM;
			ret += 2;
			bit_data_ = 0;
			mode_ = COMPRESSED;
		}
		break;
	case COMPRESSED:
		if (cmp_last_ > (TEST_SLICE << 3)) {
			if (last_matched_) {
				update_at_ = last_matched_;
				ret += send_data(last_matched_);
				last_matched_ = 0;
			}
			last_added_ = 0;
			send_cw(CCW_ETM);
			align_bits();
			ret += send_bits();
			mode_ = TRANSPARENT;
		}
		break;
	}
	return ret;
}

int V42bis::flush_encoder()
{
	int ret = 0;
	if (update_at_)
		return 0;
	if (last_matched_) {
		int total = flushed_len_ + str_len_;
		ret = send_data(last_matched_);
		flushed_len_ = total;
	}
	if (mode_ == COMPRESSED) {
		update_at_ = last_matched_;
		last_matched_ = 0;
		flushed_len_ = 0;
		send_cw(CCW_FLUSH);
		align_bits();
		send_bits();
	}
	return ret;
}

int V42bis::encode(uint8_t in)
{
	int ret = 0;
	bool consumed = false;

	while (!consumed) {
		if (update_at_) {
			if (!match_char(update_at_, in))
				last_added_ = add_char(update_at_, in);
			update_at_ = 0;
		}
		for (;;) {
			uint16_t cw = match_char(last_matched_, in);
			if (!cw) {
				update_at_ = last_matched_;
				ret += send_data(last_matched_);
				last_matched_ = 0;
				break;
			}
			if (cw == last_added_) {
				last_added_ = 0;
				ret += send_data(last_matched_);
				last_matched_ = 0;
				break;
			}
			last_matched_ = cw;
			str_data_[str_len_++] = in;
			consumed = true;
			if (str_len_ + flushed_len_ == max_string_) {
				ret += send_data(last_matched_);
				last_matched_ = 0;
			}
			break;
		}
		ret += test_mode();
	}
	return ret;
}


/*
 *    set up
 *
 */

int V42bis::init(int dict_size, int max_string)
{
	exit();
	if (dict_size < MIN_DICT_SIZE || max_string < MIN_STR_SIZE ||
	    max_string > MAX_STR_SIZE || max_string > V42BIS_MAX_STRING)
		return -1;
	dict_ = new (std::nothrow) Node[dict_size]();
	if (!dict_)
		return -1;
	max_string_ = max_string;
	dict_size_ = dict_size;
	for (int i = 0; i < TOTAL_CHARS; i++)
		dict_[i + FIRST_ENTRY].ch = (uint8_t)i;
	dict_init();
	cmp_bits_ = raw_bits_ = 0;
	olen_ = ohead_ = 0;
	return 0;
}

int V42bis::config(int dict_size, int max_string)
{
	if (dict_size < MIN_DICT_SIZE || max_string < MIN_STR_SIZE ||
	    max_string > MAX_STR_SIZE)
		return -1;
	if (dict_size > dict_size_ || max_string > (int)sizeof(str_data_))
		return -1;
	max_string_ = max_string;
	dict_size_ = dict_size;
	return 0;
}

void V42bis::exit()
{
	delete[] dict_;
	dict_ = nullptr;
	dict_size_ = 0;
}

int V42bis::out(uint8_t *buf, int n)
{
	int ret = 0;
	if (olen_ > 0) {
		ret = olen_ - ohead_ < n ? olen_ - ohead_ : n;
		memcpy(buf, obuf_ + ohead_, ret);
		ohead_ += ret;
		if (ohead_ == olen_)
			ohead_ = olen_ = 0;
	}
	return ret;
}

} // namespace dlink
