/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    zstd_codec.h - Zstandard compression (RFC 8878), one direction (an
 *                   encoder or a decoder), small: written from the RFC
 *                   for the link layer (Annex F of
 *                   docs/cellular/v_cvm.tex); named so that it does
 *                   not hide the system's <zstd.h>
 *
 *    The decoder takes any Zstandard frame: raw, RLE and compressed
 *    blocks; literals raw, RLE or Huffman coded (the weights direct or
 *    FSE coded, one or four streams, the last table again); sequences
 *    with predefined, RLE, FSE coded or repeated tables; the repeat
 *    offsets; a raw content or a formatted dictionary; skippable
 *    frames; the checksum skipped (the link has its FCS).  It takes
 *    octets as they come and decodes a block once it is all in.
 *
 *    The encoder is small: a hash chain over the window with a lazy
 *    match, the repeat offsets, literals raw, RLE or Huffman coded (a
 *    new table in the direct form, or the last one again, whichever
 *    costs least), each sequence table the predefined, the last again
 *    or a new one of the block's codes, whichever costs least.  A
 *    formatted dictionary (RFC 8878 5) gives both ends its content and
 *    entropy tables from the start (make_dict() builds one from a
 *    text: zstd_dict.h's from Annex E's).  One frame from the start of
 *    the call, without a content size or a checksum, that does not
 *    end: a block at each flush (and every block_size characters), each
 *    one raw when coding gains nothing.
 *
 */
#ifndef DMODEM_LINK_ZSTD_CODEC_H
#define DMODEM_LINK_ZSTD_CODEC_H

#include <vector>

#include "dm_types.h"

namespace dlink {

enum {
	ZSTD_WINDOW_LOG = 15,           /* V.cvm's window: 32 kB */
	ZSTD_BLOCK = 1024,              /* the encoder's block at the most */
	ZSTD_MAX_WINDOW_LOG = 23,       /* the decoder's limit (8 MB) */
};

class Zstd {
public:
	Zstd() = default;
	Zstd(const Zstd &) = delete;
	Zstd &operator=(const Zstd &) = delete;

	/* an encoder or a decoder anew: the window's log (the encoder's,
	   the decoder's largest), a raw content dictionary (or none), the
	   encoder's block size; -1 if out of range */
	s16 init(s16 window_log = ZSTD_WINDOW_LOG, const u8 *dict = nullptr,
		 s32 dict_n = 0, s32 block = ZSTD_BLOCK);
	/* frees it all (not ready() any more) */
	void exit();
	/* init() was done */
	bool ready() const { return window_ > 0; }

	/* the encoder: one character in; a block once there are
	   block_size of them */
	s16 encode(u8 ch);
	/* the characters so far as a block */
	s16 flush_encoder();
	/* the frame's end: the characters so far, then the last block
	   (for a whole frame, a test's) */
	s16 finish();
	/* the decoder: one octet in; -1 on an error */
	s16 decode(u8 octet);
	/* the decoder: nothing to flush */
	s16 flush_decoder() { return 0; }
	/* the decoder: a whole frame ended (its last block) */
	bool frame_done() const { return st_ == DONE; }
	/* the output so far, up to n bytes */
	s16 out(u8 *buf, s16 n);
	/* output waiting */
	bool has_out() const { return ohead_ < obuf_.size(); }

	/* statistics: characters and compressed bits (the encoder) */
	u32 raw_bits() const { return raw_bits_; }
	/* (see raw_bits()) */
	u32 cmp_bits() const { return cmp_bits_; }

	/* what the encoder's blocks code: the literals, the sequences'
	   codes */
	struct Stats {
		u32 lit[256], ll[36], ml[53], of[32];
	};
	/* the encoder's statistics counted into s (nullptr: not) */
	void set_stats(Stats *s) { stats_ = s; }
	/* a formatted dictionary of the content, its ID id: the entropy
	   tables of the content's statistics as this encoder codes it
	   (every 7-bit literal, every length code and the offset codes of
	   a 64 kB window possible), the repeat offsets 1, 4 and 8 */
	static std::vector<u8> make_dict(const u8 *content, s32 n, u32 id);

	/* an FSE table: the state's symbol, bits and baseline */
	struct Fse {
		s16 log = 0;
		std::vector<u8> sym, nb;
		std::vector<u16> base;
	};
	/* a Huffman table (decoding): by max_bits bits, the symbol and
	   its length; empty if none yet */
	struct Huf {
		s16 max_bits = 0;
		std::vector<u8> sym, nb;
	};

private:
	enum St { MAGIC, HEADER, BLOCK_HEADER, BLOCK, SKIP, DONE, ERROR };

	/* both */
	Stats *stats_ = nullptr;
	/* a formatted dictionary's ID (0: none or raw content), tables
	   and repeat offsets: the frame's start */
	u32 dict_id_ = 0;
	bool dict_tables_ = false;
	Huf dict_huf_;
	Fse dict_ll_, dict_of_, dict_ml_;
	u32 dict_rep_[3] = { 1, 4, 8 };
	s32 parse_dict(const u8 *d, s32 n);
	void frame_start();
	s32 window_ = 0;
	u32 rep_[3] = { 1, 4, 8 };
	std::vector<u8> hist_;          /* the window's characters and those
					   after it (the encoder's: not yet
					   coded) */
	s32 dict_n_ = 0;
	std::vector<u8> obuf_;
	size_t ohead_ = 0;
	u32 raw_bits_ = 0, cmp_bits_ = 0;

	/* encoder */
	s32 block_ = 0;
	s32 coded_ = 0;                 /* hist_'s characters coded */
	bool started_ = false;          /* the frame header out */
	std::vector<s32> head_, chain_; /* the hash chains: positions + 1 */
	s32 chain_base_ = 0;            /* hist_[0]'s position */
	s32 ins_ = 0;                   /* the next position for the hash
					   chains */
	std::vector<u8> enc_bits_;      /* the last Huffman table's lengths
					   (by symbol; empty: none) */
	/* the sequences' tables the decoder has (counts and accuracy
	   log; none: empty), and the dictionary's */
	struct Cur {
		std::vector<s16> cnt;
		s16 al = 0;
	};
	Cur cur_[3], dict_cur_[3];      /* LL, OF, ML */
	std::vector<u8> dict_bits_;
	void enc_block(bool last);
	void put_block(const std::vector<u8> &body, s16 type, s32 size,
		       bool last);
	void insert(s32 i);
	s32 best_match(s32 i, s32 end, s32 *off);
	void slide();

	/* decoder */
	St st_ = MAGIC;
	std::vector<u8> in_;
	s32 need_ = 0;                  /* octets of in_ for the next step */
	bool last_block_ = false;
	s16 block_type_ = 0;
	s32 block_size_ = 0;
	s32 skip_ = 0;
	bool checksum_ = false;
	Huf huf_;
	Fse ll_, of_, ml_;
	bool have_ll_ = false, have_of_ = false, have_ml_ = false;
	s16 header(const u8 *p, s32 n);
	s16 block(const u8 *p, s32 n);
	s32 literals(const u8 *p, s32 n, std::vector<u8> *lit);
	s16 sequences(const u8 *p, s32 n, const std::vector<u8> &lit);
	void emit(u8 c);
	void trim();
};

} // namespace dlink

#endif
