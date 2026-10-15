/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v42bis.h - V.42bis data compression, one direction (an encoder or a
 *               decoder: each has its own dictionary)
 *
 *    The modem offers 2048 codewords and strings of up to 34 characters;
 *    XID can lower them (no less than 512 and 6).
 *
 */

#ifndef DMODEM_LINK_V42BIS_H
#define DMODEM_LINK_V42BIS_H

#include <stdint.h>

namespace dlink {

enum {
	V42BIS_MAX_CODEWORDS = 2048,    /* N2 we offer */
	V42BIS_MAX_STRING = 34,         /* N7 we offer */
};

class V42bis {
public:
	V42bis() = default;
	~V42bis() { exit(); }
	V42bis(const V42bis &) = delete;
	V42bis &operator=(const V42bis &) = delete;

	/* N2 codewords and strings of up to N7 characters allocated; -1 if
	   out of range or no memory */
	int init(int dict_size, int max_string);
	/* smaller values from the remote (within what init allocated) */
	int config(int dict_size, int max_string);
	/* frees the dictionary (not ready() any more) */
	void exit();
	/* init() was done */
	bool ready() const { return dict_ != nullptr; }

	/* one character in; then out() gives what it produced */
	int encode(uint8_t ch);
	int decode(uint8_t ch);
	/* the encoder: what it holds out (FLUSH); the decoder: the string so
	   far out, at the end of a frame */
	int flush_encoder();
	int flush_decoder();
	/* the output so far, up to n bytes */
	int out(uint8_t *buf, int n);
	/* output waiting */
	bool has_out() const { return olen_ > 0; }

	/* statistics: bits before and after (the encoder) */
	uint32_t raw_bits() const { return raw_bits_; }
	uint32_t cmp_bits() const { return cmp_bits_; }

private:
	enum Mode { TRANSPARENT, COMPRESSED };
	struct Node {
		uint8_t ch;
		uint16_t parent, child, next;
	};

	void dict_init();
	uint16_t match_char(uint16_t at, uint8_t ch) const;
	uint16_t add_char(uint16_t at, uint8_t ch);
	int send_string();
	int recv_data(uint16_t cw);
	int send_data(uint16_t cw);
	int test_mode();
	void send_cw(uint16_t cw);
	void align_bits();
	int send_bits();

	Mode mode_ = TRANSPARENT;
	bool escape_ = false;           /* decoder: after the escape char */
	uint16_t update_at_ = 0;
	uint16_t next_free_ = 0;
	uint16_t last_matched_ = 0;
	uint16_t last_added_ = 0;
	uint16_t cw_size_ = 0;
	uint16_t threshold_ = 0;
	uint8_t escape_char_ = 0;
	int max_string_ = 0;
	int dict_size_ = 0;
	Node *dict_ = nullptr;
	int bit_len_ = 0;
	uint32_t bit_data_ = 0;
	int flushed_len_ = 0;
	int str_len_ = 0;
	uint8_t str_data_[V42BIS_MAX_STRING];
	/* the encoder's test of the compression ratio */
	uint16_t cmp_last_ = 0;
	uint32_t cmp_bits_ = 0, raw_bits_ = 0;
	/* output */
	int olen_ = 0, ohead_ = 0;
	uint8_t obuf_[1024];
};

} // namespace dlink

#endif
