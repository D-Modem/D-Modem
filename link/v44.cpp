/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v44.cpp - V.44 data compression (see v44.h)
 *
 *    As the Agere modem does it: the encoder and decoder start in
 *    compressed mode (codewords from 4, 6 bits, ordinals of 7 bits); the
 *    encoder sends REINIT when the dictionary or the history is full and
 *    FLUSH when the DTE has no more for now, and stays in compressed
 *    mode.
 *
 */

#include <algorithm>
#include <cstring>

#include "dm_types.h"
#include "v44.h"

namespace dlink {

enum { HASH = 4096 };

/* the encoder's hash of a string's first two characters */
static u16 hash2(u8 a, u8 b)
{
	return (static_cast<u32>(a) * 263 + b) & (HASH - 1);
}

/* allocates for the largest parameters (N2 codewords, N7 characters,
   N8 octets of history) and starts both coders' state; -1 if out of
   range */
s16 V44::init(s32 codewords, s32 max_string, s32 history)
{
	exit();
	if (codewords < V44_MIN_CODEWORDS || codewords > 65535 ||
	    max_string < V44_MIN_STRING || max_string > 255 ||
	    history < V44_MIN_HISTORY || history < max_string)
		return -1;
	max_n2_ = codewords;
	max_n7_ = max_string;
	max_n8_ = history;
	dict_.assign(codewords, Entry());
	hist_.assign(history, 0);
	head_.assign(HASH, -1);
	n2_ = max_n2_;
	n7_ = max_n7_;
	n8_ = max_n8_;
	end_ = 0;
	bits_ = 0;
	nbits_ = 0;
	transparent_ = escape_ = stepup_ = false;
	escape_char_ = 0;
	in_.clear();
	in_pos_ = 0;
	in_bit_ = 0;
	obuf_.clear();
	ohead_ = 0;
	raw_bits_ = cmp_bits_ = 0;
	prev_type_ = NONE;
	primed_ = false;
	reset();
	return 0;
}

/* what XID agreed (no more than init()'s): the dictionary and the
   history from the start; -1 if out of range */
s16 V44::config(s32 codewords, s32 max_string, s32 history)
{
	if (!ready() || codewords < V44_MIN_CODEWORDS ||
	    codewords > max_n2_ || max_string < V44_MIN_STRING ||
	    max_string > max_n7_ || history < V44_MIN_HISTORY ||
	    history > max_n8_ || history < max_string)
		return -1;
	n2_ = codewords;
	n7_ = max_string;
	n8_ = history;
	end_ = 0;
	primed_ = false;
	reset();
	return 0;
}

/* frees the dictionary, the history and the buffers */
void V44::exit()
{
	dict_.clear();
	hist_.clear();
	head_.clear();
	in_.clear();
	obuf_.clear();
	ohead_ = 0;
	primed_ = false;
	p_dict_.clear();
	p_head_.clear();
	p_hist_.clear();
}

/* the dictionary and history from the start */
void V44::reset()
{
	c1_ = 4;
	c2_ = 6;
	c3_ = UINT32_C(1) << c2_;
	c5_ = 7;
	hp_ = 0;
	last_type_ = NONE;
	last_value_ = 0;
	stepup_ = false;
	if (!head_.empty())
		for (auto &h : head_)
			h = -1;
}

/* the dictionary and history from the start: the preset's (prime()),
   if there is one */
void V44::restart()
{
	if (!primed_) {
		reset();
		return;
	}
	std::copy(p_dict_.begin(), p_dict_.end(), dict_.begin());
	head_ = p_head_;
	std::copy(p_hist_.begin(), p_hist_.end(), hist_.begin());
	hp_ = static_cast<u32>(p_hist_.size());
	c1_ = p_c1_;
	c2_ = p_c2_;
	c3_ = UINT32_C(1) << c2_;
	c5_ = p_c5_;
	last_type_ = p_last_type_;
	last_value_ = p_last_value_;
	stepup_ = false;
}

/* see v44.h: the text through an encoder of the same parameters, its
   tokens through this one's decoder (whichever this is), and that state
   kept for restart() */
s32 V44::prime(const u8 *text, s32 n)
{
	if (!ready() || n <= 0)
		return 0;
	V44 enc;
	if (enc.init(n2_, n7_, n8_))
		return 0;
	/* no more than the dictionary has room for, the lookahead's
	   strings (two a character at most) and the flush's included, nor
	   half the history: no REINIT */
	s32 k = 0;
	while (k < n &&
	       enc.c1_ + 2 * (enc.end_ - enc.hp_ + 1) + 4 <
		       static_cast<u32>(n2_) &&
	       enc.end_ + 1 < static_cast<u32>(n8_ / 2))
		enc.encode(text[k++]);
	enc.flush_encoder();
	if (!k || enc.hp_ != enc.end_ || enc.hp_ != static_cast<u32>(k))
		return 0;               /* (it started again: none) */
	end_ = 0;
	hp_ = 0;
	primed_ = false;
	reset();
	in_.clear();
	in_pos_ = 0;
	in_bit_ = 0;
	transparent_ = escape_ = false;
	prev_type_ = NONE;
	bool ok = true;
	for (u8 o : enc.obuf_)
		if (decode(o) < 0 || transparent_) {
			ok = false;
			break;
		}
	if (!ok || hp_ != static_cast<u32>(k) || !in_.empty()) {
		/* (cannot be: as config() left it) */
		s32 a = n2_, b = n7_, c = n8_;
		init(max_n2_, max_n7_, max_n8_);
		config(a, b, c);
		return 0;
	}
	/* the decoder's state is the encoder's after its FLUSH: both
	   ends start from it */
	end_ = hp_;
	c3_ = UINT32_C(1) << c2_;
	bits_ = 0;
	nbits_ = 0;
	prev_type_ = NONE;
	obuf_.clear();
	ohead_ = 0;
	raw_bits_ = cmp_bits_ = 0;
	p_dict_.assign(dict_.begin(), dict_.begin() + c1_);
	p_head_ = head_;
	p_hist_.assign(hist_.begin(), hist_.begin() + hp_);
	p_c1_ = c1_;
	p_c2_ = c2_;
	p_c5_ = c5_;
	p_last_type_ = last_type_;
	p_last_value_ = last_value_;
	primed_ = true;
	return k;
}

/* a new string in the dictionary */
void V44::add(s16 len, u32 last)
{
	Entry &e = dict_[c1_];
	e.len = static_cast<u16>(len);
	e.last = last;
	/* the encoder finds it by its first two characters */
	u32 start = last - len + 1;
	u16 k = hash2(hist_[start], hist_[start + 1]);
	e.next = head_[k];
	head_[k] = static_cast<s32>(c1_);
	c1_++;
}

/* the dictionary after an ordinal, a codeword or an extension, the same
   in the encoder and the decoder: the last string (an ordinal, or a
   codeword's string) plus the first character of this one; an extended
   string */
void V44::after_token(s16 type, u32 value)
{
	if (c1_ < static_cast<u32>(n2_)) {
		if (type == ORD || type == CW) {
			u32 first = type == ORD ? hp_ - 1 :
				hp_ - dict_[value].len;
			if (last_type_ == ORD) {
				add(2, first);
			} else if (last_type_ == CW) {
				s16 l = dict_[last_value_].len;
				if (l < n7_)
					add(l + 1, first);
			}
		} else if (type == EXT && last_type_ == CW) {
			s16 l = dict_[last_value_].len +
				static_cast<s16>(value);
			if (l <= n7_)
				add(l, hp_ - 1);
		}
	}
	last_type_ = type;
	last_value_ = value;            /* after EXT nothing looks at it */
}


/*
 *    the encoder
 */

/* the encoder: n bits of v into the octets, the lowest first */
void V44::put_bits(u32 v, s16 n)
{
	bits_ |= v << nbits_;
	nbits_ += n;
	cmp_bits_ += n;
	while (nbits_ >= 8) {
		obuf_.push_back(static_cast<u8>(bits_));
		bits_ >>= 8;
		nbits_ -= 8;
	}
}

/* the encoder: a control code, prefix 1 and C2 bits */
void V44::put_control(s16 code)
{
	put_bits(1, 1);
	put_bits(code, c2_);
	prev_type_ = NONE;
}

/* the encoder: a character as an ordinal (STEPUP to 8 bits first for
   one of 128 and up) */
void V44::put_ordinal(u8 ch)
{
	if (ch >= 128 && c5_ == 7) {
		put_control(STEPUP);
		c5_ = 8;
	}
	/* right after a codeword: 0 0, else 0 */
	put_bits(0, prev_type_ == CW ? 2 : 1);
	put_bits(ch, c5_);
	prev_type_ = ORD;
}

/* the encoder: a codeword, after the STEPUPs that make C2 bits hold it */
void V44::put_codeword(u32 cw)
{
	while (cw >= c3_) {
		put_control(STEPUP);
		c2_++;
		c3_ <<= 1;
	}
	put_bits(1, 1);
	put_bits(cw, c2_);
	prev_type_ = CW;
}

/* the bits of a length of 13 and up: by the longest string */
static s16 ext_bits(s16 n7)
{
	return n7 <= 46 ? 5 : n7 <= 78 ? 6 : n7 <= 142 ? 7 : 8;
}

/* the encoder: the string extended by n characters: 0 1, then the
   length (1: 1; 2-4: 0, 2 bits; 5-12: 0, 00, 0, 3 bits; 13 and up: 0,
   00, 1, ext_bits() bits) */
void V44::put_extension(s16 n)
{
	put_bits(2, 2);                 /* 0 1 */
	if (n == 1) {
		put_bits(1, 1);
	} else if (n <= 4) {
		put_bits(0, 1);
		put_bits(n - 1, 2);
	} else if (n <= 12) {
		put_bits(0, 4);
		put_bits(n - 5, 3);
	} else {
		put_bits(8, 4);         /* 0 00 1 */
		put_bits(n - 13, ext_bits(n7_));
	}
	prev_type_ = EXT;
}

/* REINIT: the characters not encoded yet to the start of the history */
void V44::reinit()
{
	put_control(REINIT);
	u32 rest = end_ - hp_;
	u32 at = primed_ ? static_cast<u32>(p_hist_.size()) : 0;
	if (rest)
		memmove(&hist_[at], &hist_[hp_], rest);
	restart();
	end_ = at + rest;
}

/* the longest string of the dictionary at pos (avail characters there);
   -1 if none */
s32 V44::find(u32 pos, u32 avail, s16 *len) const
{
	s32 best = -1, best_len = 0;
	if (avail < 2)
		return -1;
	for (s32 e = head_[hash2(hist_[pos], hist_[pos + 1])]; e >= 0;
	     e = dict_[e].next) {
		const Entry &d = dict_[e];
		if (d.len <= best_len || d.len > avail)
			continue;
		if (!memcmp(&hist_[d.last - d.len + 1], &hist_[pos], d.len)) {
			best = e;
			best_len = d.len;
		}
	}
	*len = best_len;
	return best;
}

/* one token (a codeword maybe extended) for the characters at hp_ */
void V44::step()
{
	u32 avail = end_ - hp_;
	s16 len;
	s32 cw = find(hp_, avail, &len);

	if (cw < 0) {
		u8 ch = hist_[hp_];
		put_ordinal(ch);
		hp_++;
		after_token(ORD, ch);
	} else {
		put_codeword(cw);
		hp_ += len;
		after_token(CW, cw);
		/* the string goes on as it did where it was: an extension,
		   to no more than the longest string */
		u32 src = dict_[cw].last + 1;
		s16 n = 0, max = n7_ - len;
		while (n < max && hp_ + n < end_ &&
		       hist_[src + n] == hist_[hp_ + n])
			n++;
		if (n > 0) {
			put_extension(n);
			hp_ += n;
			after_token(EXT, n);
		}
	}
	if (c1_ >= static_cast<u32>(n2_) ||
	    hp_ >= static_cast<u32>(n8_))
		reinit();
}

/* the encoder: one character from the DTE into the history; tokens for
   what is more than a longest string ahead (the history full: all of
   it, then REINIT); -1 if not ready */
s16 V44::encode(u8 ch)
{
	if (!ready())
		return -1;
	if (end_ == static_cast<u32>(n8_)) {
		/* the history is full: what is in it, then REINIT */
		while (end_ == static_cast<u32>(n8_))
			step();
	}
	hist_[end_++] = ch;
	raw_bits_ += 8;
	/* enough to look ahead for the longest string */
	while (end_ - hp_ > static_cast<u32>(n7_))
		step();
	return 0;
}

/* all the characters so far, then FLUSH and the rest of the octet */
s16 V44::flush_encoder()
{
	if (!ready())
		return 0;
	if (hp_ == end_ && prev_type_ == NONE && !nbits_)
		return 0;               /* nothing since the last flush */
	while (hp_ < end_)
		step();
	put_control(FLUSH);
	if (nbits_)
		put_bits(0, 8 - nbits_);
	return 0;
}


/*
 *    the decoder
 */

/* n bits, the lowest first; false if they are not there yet */
bool V44::get_bits(s16 n, u32 *v)
{
	size_t have = (in_.size() - in_pos_) * 8 - in_bit_;
	*v = 0;
	if (static_cast<size_t>(n) > have)
		return false;
	for (s16 k = 0; k < n; k++) {
		*v |= static_cast<u32>((in_[in_pos_] >> in_bit_) & 1) << k;
		if (++in_bit_ == 8) {
			in_bit_ = 0;
			in_pos_++;
		}
	}
	return true;
}

/* the next token; false if its bits are not all there yet (or on an
   error: *type -1) */
bool V44::read_token(s16 *type, u32 *value)
{
	u32 b, v;

	*type = -1;
	if (!get_bits(1, &b))
		return false;
	if (b) {
		if (stepup_) {
			stepup_ = false;
			if ((UINT32_C(1) << c2_) >= static_cast<u32>(n2_))
				return true;            /* an error */
			c2_++;
		}
		if (!get_bits(c2_, &v))
			return false;
		*type = v > 3 ? CW : NONE;
		*value = v;
		return true;
	}
	if (prev_type_ == CW) {
		if (!get_bits(1, &b))
			return false;
		if (b) {
			/* a string extension: its length */
			if (!get_bits(1, &b))
				return false;
			if (b) {
				v = 1;
			} else {
				if (!get_bits(2, &v))
					return false;
				if (v) {
					v++;
				} else {
					if (!get_bits(1, &b))
						return false;
					if (b) {
						if (!get_bits(ext_bits(n7_), &v))
							return false;
						v += 13;
					} else {
						if (!get_bits(3, &v))
							return false;
						v += 5;
					}
				}
			}
			*type = EXT;
			*value = v;
			return true;
		}
	}
	if (stepup_) {
		stepup_ = false;
		if (c5_ == 8)
			return true;                    /* an error */
		c5_++;
	}
	if (!get_bits(c5_, &v))
		return false;
	*type = ORD;
	*value = v;
	return true;
}

/* the decoder: one character into the history and the output; -1 if
   the history is full */
s16 V44::put_hist(u8 ch)
{
	if (hp_ >= static_cast<u32>(n8_))
		return -1;
	hist_[hp_++] = ch;
	obuf_.push_back(ch);
	return 0;
}

/* the decoder: one token (a control code, an ordinal, a codeword, an
   extension) into the history and the dictionary; the codeword being
   defined is the last string and its own first character; -1 on an
   error */
s16 V44::token(s16 type, u32 value)
{
	prev_type_ = type;
	switch (type) {
	case NONE:                      /* a control code */
		switch (value) {
		case ETM:
			transparent_ = true;
			/* on an octet, as FLUSH */
			[[fallthrough]];
		case FLUSH:
			if (in_bit_) {
				in_bit_ = 0;
				in_pos_++;
			}
			break;
		case STEPUP:
			stepup_ = true;
			break;
		case REINIT:
			restart();
			break;
		}
		return 0;
	case ORD:
		if (put_hist(static_cast<u8>(value)))
			return -1;
		after_token(ORD, value);
		return 0;
	case CW:
		if (value < c1_) {
			const Entry &e = dict_[value];
			u32 start = e.last - e.len + 1;
			if (hp_ + e.len > static_cast<u32>(n8_))
				return -1;
			for (s16 k = 0; k < e.len; k++)
				put_hist(hist_[start + k]);
			after_token(CW, value);
			return 0;
		}
		if (value == c1_ && c1_ < static_cast<u32>(n2_)) {
			/* the string being defined: the last one and its
			   first character */
			u32 start, l;
			if (last_type_ == ORD) {
				start = hp_ - 1;
				l = 1;
			} else if (last_type_ == CW &&
				   dict_[last_value_].len < n7_) {
				const Entry &p = dict_[last_value_];
				start = p.last - p.len + 1;
				l = p.len;
			} else {
				return -1;
			}
			u32 at = hp_;
			if (hp_ + l + 1 > static_cast<u32>(n8_))
				return -1;
			for (u32 k = 0; k < l; k++)
				put_hist(hist_[start + k]);
			put_hist(hist_[start]);
			add(l + 1, at);
			last_type_ = CW;
			last_value_ = value;
			return 0;
		}
		return -1;
	case EXT:
		if (last_type_ != CW)
			return -1;
		{
			u32 src = dict_[last_value_].last + 1;
			if (hp_ + value > static_cast<u32>(n8_))
				return -1;
			for (u32 k = 0; k < value; k++)
				put_hist(hist_[src + k]);
		}
		after_token(EXT, value);
		return 0;
	}
	return -1;
}

/* the decoder: the octets received so far, as tokens while whole ones
   are there (one cut short waits for more), or as transparent octets;
   the used octets dropped; -1 on an error */
s16 V44::run_decoder()
{
	for (;;) {
		if (transparent_) {
			if (in_pos_ >= in_.size())
				break;
			u8 b = in_[in_pos_++];
			if (escape_) {
				escape_ = false;
				if (b == 0) {
					/* ECM: compressed again, from the
					   start */
					transparent_ = false;
					prev_type_ = NONE;
					restart();
				} else if (b == 1) {
					obuf_.push_back(escape_char_);
					escape_char_ += 0x33;
				} else if (b != 2) {
					return -1;
				}
			} else if (b == escape_char_) {
				escape_ = true;
			} else {
				obuf_.push_back(b);
			}
			continue;
		}
		/* a token, if its bits are all there */
		size_t pos = in_pos_;
		s16 bit = in_bit_, c2 = c2_, c5 = c5_;
		bool stepup = stepup_;
		s16 type;
		u32 value;
		if (!read_token(&type, &value)) {
			in_pos_ = pos;
			in_bit_ = bit;
			c2_ = c2;
			c5_ = c5;
			stepup_ = stepup;
			break;
		}
		if (type < 0 || token(type, value) < 0)
			return -1;
	}
	if (in_pos_) {
		in_.erase(in_.begin(), in_.begin() + in_pos_);
		in_pos_ = 0;
	}
	return 0;
}

/* the decoder: one octet from the line; -1 on an error */
s16 V44::decode(u8 octet)
{
	if (!ready())
		return -1;
	in_.push_back(octet);
	return run_decoder();
}

/* up to n octets of the output into buf; returns how many */
s16 V44::out(u8 *buf, s16 n)
{
	size_t k = obuf_.size() - ohead_;
	if (k > static_cast<size_t>(n))
		k = n;
	if (k)
		memcpy(buf, &obuf_[ohead_], k);
	ohead_ += k;
	if (ohead_ == obuf_.size()) {
		obuf_.clear();
		ohead_ = 0;
	}
	return static_cast<s16>(k);
}

} // namespace dlink
