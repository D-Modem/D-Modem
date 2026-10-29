/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v44.h - V.44 data compression (LZJH), one direction (an encoder or a
 *            decoder: each has its own dictionary and history)
 *
 *    The modem offers 4096 codewords, strings of up to 255 characters and
 *    a history of 12288 (as the Agere modem does); XID can lower them.
 *
 *    The compressed data: tokens of bits, the first bit in the lowest bit
 *    of an octet, each value its lowest bit first.  A token starts with a
 *    prefix bit:
 *
 *        1 <C2 bits>       a codeword: 4 and up a string of the
 *                          dictionary, 0-3 a control code (ETM, FLUSH,
 *                          STEPUP, REINIT)
 *        0 <C5 bits>       an ordinal: a character (7 bits, 8 after a
 *                          STEPUP); right after a codeword 0 0 <C5 bits>
 *        0 1 <length>      right after a codeword: the string extended
 *                          by that many characters of the history (those
 *                          that followed the string where it was)
 *
 *    The dictionary's strings are in the history; each ordinal or
 *    codeword adds the last string plus its first character, each
 *    extension the extended string.  The codeword size (C2, from 6) and
 *    the ordinal size (C5, from 7) grow by STEPUP; REINIT starts the
 *    dictionary and history again, which the encoder sends when either
 *    is full.  FLUSH ends the data so far on an octet.  The decoder also
 *    takes transparent mode (ETM: octets with an escape character).
 *
 *    A preset dictionary (prime()): before the data, both ends take the
 *    same text into the dictionary and the history as if it had gone
 *    through, encoded then decoded, and come back to that state, rather
 *    than to empty ones, at each REINIT.  Both ends have to have agreed
 *    on it (V.cvm's capabilities do it, link/v44_preset.h).
 *
 */
#ifndef DMODEM_LINK_V44_H
#define DMODEM_LINK_V44_H

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dlink {

enum {
	V44_MAX_CODEWORDS = 4096,       /* N2 we offer */
	V44_MAX_STRING = 255,           /* N7 */
	V44_MAX_HISTORY = 12288,        /* N8 */
	V44_MIN_CODEWORDS = 256,
	V44_MIN_STRING = 32,
	V44_MIN_HISTORY = 512,
};

class V44 {
public:
	V44() = default;
	V44(const V44 &) = delete;
	V44 &operator=(const V44 &) = delete;

	/* the largest dictionary and history (what XID can agree on) */
	s16 init(s32 codewords, s32 max_string, s32 history);
	/* what was agreed (no more than init's); -1 if out of range */
	s16 config(s32 codewords, s32 max_string, s32 history);
	/* frees it all (not ready() any more) */
	void exit();
	/* after config(): the preset dictionary, the first of text's n
	   characters that the dictionary and half the history have room
	   for (the same in the encoder and the decoder of the same
	   parameters); how many were taken, 0 if none (then as without) */
	s32 prime(const u8 *text, s32 n);
	/* prime() took some */
	bool primed() const { return primed_; }
	/* init() was done */
	bool ready() const { return !hist_.empty(); }

	/* the encoder: one character in; the tokens come once there are
	   enough characters to look ahead, or at the flush */
	s16 encode(u8 ch);
	s16 flush_encoder();
	/* the decoder: one octet in; -1 on an error */
	s16 decode(u8 octet);
	/* the decoder: nothing to flush */
	s16 flush_decoder() { return 0; }
	/* the output so far, up to n bytes */
	s16 out(u8 *buf, s16 n);
	/* output waiting */
	bool has_out() const { return ohead_ < obuf_.size(); }

	/* statistics: characters and compressed bits (the encoder) */
	u32 raw_bits() const { return raw_bits_; }
	/* (see raw_bits()) */
	u32 cmp_bits() const { return cmp_bits_; }

private:
	enum { NONE = 1, ORD = 0, CW = 2, EXT = 3 };    /* token types */
	enum { ETM = 0, FLUSH = 1, STEPUP = 2, REINIT = 3 };
	struct Entry {
		u16 len;
		u32 last;               /* the history position of its last
					   character */
		s32 next;               /* the encoder's hash chain */
	};

	void reset();
	/* back to the start: the preset's state, or reset() */
	void restart();
	void add(s16 len, u32 last);
	void after_token(s16 type, u32 value);
	/* encoder */
	void step();
	void put_bits(u32 v, s16 n);
	void put_control(s16 code);
	void put_ordinal(u8 ch);
	void put_codeword(u32 cw);
	void put_extension(s16 n);
	void reinit();
	s32 find(u32 pos, u32 avail, s16 *len) const;
	/* decoder */
	bool get_bits(s16 n, u32 *v);
	bool read_token(s16 *type, u32 *value);
	s16 run_decoder();
	s16 token(s16 type, u32 value);
	s16 put_hist(u8 ch);

	s32 n2_ = 0, n7_ = 0, n8_ = 0;          /* agreed */
	s32 max_n2_ = 0, max_n7_ = 0, max_n8_ = 0;
	std::vector<Entry> dict_;
	std::vector<u8> hist_;
	u32 hp_ = 0;                            /* history: written */
	u32 c1_ = 0;                            /* the next codeword */
	s16 c2_ = 0, c5_ = 0;                   /* codeword, ordinal size */
	u32 c3_ = 0;                            /* 1 << c2_ */
	s16 last_type_ = NONE;                  /* for the dictionary */
	u32 last_value_ = 0;
	s16 prev_type_ = NONE;                  /* any token, controls too */
	/* encoder */
	std::vector<s32> head_;                 /* the first two characters
						   of a string -> its entry */
	u32 end_ = 0;                           /* characters in hist_ */
	u32 bits_ = 0;
	s16 nbits_ = 0;
	/* decoder */
	bool transparent_ = false;
	bool escape_ = false;
	u8 escape_char_ = 0;
	bool stepup_ = false;
	std::vector<u8> in_;                    /* octets not decoded yet */
	size_t in_pos_ = 0;
	s16 in_bit_ = 0;                        /* bits used of in_[in_pos_] */
	/* output */
	std::vector<u8> obuf_;
	size_t ohead_ = 0;
	u32 raw_bits_ = 0, cmp_bits_ = 0;
	/* the preset's state (prime()) */
	bool primed_ = false;
	std::vector<Entry> p_dict_;
	std::vector<s32> p_head_;
	std::vector<u8> p_hist_;
	u32 p_c1_ = 0;
	s16 p_c2_ = 0, p_c5_ = 0;
	s16 p_last_type_ = NONE;
	u32 p_last_value_ = 0;
};

} // namespace dlink

#endif
