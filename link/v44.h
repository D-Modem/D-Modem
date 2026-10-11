/*
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
 */
#ifndef DMODEM_LINK_V44_H
#define DMODEM_LINK_V44_H

#include <stdint.h>

#include <vector>

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
	int init(int codewords, int max_string, int history);
	/* what was agreed (no more than init's); -1 if out of range */
	int config(int codewords, int max_string, int history);
	void exit();
	bool ready() const { return !hist_.empty(); }

	/* the encoder: one character in; the tokens come once there are
	   enough characters to look ahead, or at the flush */
	int encode(uint8_t ch);
	int flush_encoder();
	/* the decoder: one octet in; -1 on an error */
	int decode(uint8_t octet);
	int flush_decoder() { return 0; }
	/* the output so far, up to n bytes */
	int out(uint8_t *buf, int n);
	bool has_out() const { return ohead_ < obuf_.size(); }

	/* statistics: characters and compressed bits (the encoder) */
	uint32_t raw_bits() const { return raw_bits_; }
	uint32_t cmp_bits() const { return cmp_bits_; }

private:
	enum { NONE = 1, ORD = 0, CW = 2, EXT = 3 };    /* token types */
	enum { ETM = 0, FLUSH = 1, STEPUP = 2, REINIT = 3 };
	struct Entry {
		uint16_t len;
		uint32_t last;          /* the history position of its last
					   character */
		int32_t next;           /* the encoder's hash chain */
	};

	void reset();
	void add(int len, uint32_t last);
	void after_token(int type, uint32_t value);
	/* encoder */
	void step();
	void put_bits(uint32_t v, int n);
	void put_control(int code);
	void put_ordinal(uint8_t ch);
	void put_codeword(uint32_t cw);
	void put_extension(int n);
	void reinit();
	int find(uint32_t pos, uint32_t avail, int *len) const;
	/* decoder */
	bool get_bits(int n, uint32_t *v);
	bool read_token(int *type, uint32_t *value);
	int run_decoder();
	int token(int type, uint32_t value);
	int put_hist(uint8_t ch);

	int n2_ = 0, n7_ = 0, n8_ = 0;          /* agreed */
	int max_n2_ = 0, max_n7_ = 0, max_n8_ = 0;
	std::vector<Entry> dict_;
	std::vector<uint8_t> hist_;
	uint32_t hp_ = 0;                       /* history: written */
	uint32_t c1_ = 0;                       /* the next codeword */
	int c2_ = 0, c5_ = 0;                   /* codeword, ordinal size */
	uint32_t c3_ = 0;                       /* 1 << c2_ */
	int last_type_ = NONE;                  /* for the dictionary */
	uint32_t last_value_ = 0;
	int prev_type_ = NONE;                  /* any token, controls too */
	/* encoder */
	std::vector<int32_t> head_;             /* the first two characters
						   of a string -> its entry */
	uint32_t end_ = 0;                      /* characters in hist_ */
	uint32_t bits_ = 0;
	int nbits_ = 0;
	/* decoder */
	bool transparent_ = false;
	bool escape_ = false;
	uint8_t escape_char_ = 0;
	bool stepup_ = false;
	std::vector<uint8_t> in_;               /* octets not decoded yet */
	size_t in_pos_ = 0;
	int in_bit_ = 0;                        /* bits used of in_[in_pos_] */
	/* output */
	std::vector<uint8_t> obuf_;
	size_t ohead_ = 0;
	uint32_t raw_bits_ = 0, cmp_bits_ = 0;
};

} // namespace dlink

#endif
