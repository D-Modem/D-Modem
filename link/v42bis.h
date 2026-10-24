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


#include "dm_types.h"

namespace dlink {

enum {
	V42BIS_MAX_CODEWORDS = 2048,    /* N2 we offer */
	V42BIS_MAX_STRING = 34,         /* N7 we offer */
};

class V42bis {
public:
	V42bis() = default;
	/* the dictionary freed */
	~V42bis() { exit(); }
	V42bis(const V42bis &) = delete;
	V42bis &operator=(const V42bis &) = delete;

	/* N2 codewords and strings of up to N7 characters allocated; -1 if
	   out of range or no memory */
	int init(s32 dict_size, s32 max_string);
	/* smaller values from the remote (within what init allocated) */
	int config(s32 dict_size, s32 max_string);
	/* frees the dictionary (not ready() any more) */
	void exit();
	/* init() was done */
	bool ready() const { return dict_ != nullptr; }

	/* one character in; then out() gives what it produced */
	int encode(u8 ch);
	int decode(u8 ch);
	/* the encoder: what it holds out (FLUSH); the decoder: the string so
	   far out, at the end of a frame */
	int flush_encoder();
	int flush_decoder();
	/* the output so far, up to n bytes */
	int out(u8 *buf, int n);
	/* output waiting */
	bool has_out() const { return olen_ > 0; }

	/* statistics: bits before and after (the encoder) */
	u32 raw_bits() const { return raw_bits_; }
	/* (see raw_bits()) */
	u32 cmp_bits() const { return cmp_bits_; }

private:
	enum Mode { TRANSPARENT, COMPRESSED };
	struct Node {
		u8 ch;
		u16 parent, child, next;
	};

	void dict_init();
	u16 match_char(u16 at, u8 ch) const;
	u16 add_char(u16 at, u8 ch);
	int send_string();
	int recv_data(u16 cw);
	int send_data(u16 cw);
	int test_mode();
	void send_cw(u16 cw);
	void align_bits();
	int send_bits();

	Mode mode_ = TRANSPARENT;
	bool escape_ = false;           /* decoder: after the escape char */
	u16 update_at_ = 0;
	u16 next_free_ = 0;
	u16 last_matched_ = 0;
	u16 last_added_ = 0;
	u16 cw_size_ = 0;
	u16 threshold_ = 0;
	u8 escape_char_ = 0;
	int max_string_ = 0;
	s32 dict_size_ = 0;
	Node *dict_ = nullptr;
	int bit_len_ = 0;
	u32 bit_data_ = 0;
	int flushed_len_ = 0;
	int str_len_ = 0;
	u8 str_data_[V42BIS_MAX_STRING];
	/* the encoder's test of the compression ratio */
	u16 cmp_last_ = 0;
	u32 cmp_bits_ = 0, raw_bits_ = 0;
	/* output */
	int olen_ = 0, ohead_ = 0;
	u8 obuf_[1024];
};

} // namespace dlink

#endif
