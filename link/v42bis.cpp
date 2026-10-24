/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v42bis.cpp - V.42bis data compression (see v42bis.h)
 *
 *    The dictionary is an array indexed by codeword.  Each entry holds
 *    the last character of its string, its parent (the string one
 *    character shorter), its first dependant and its next sibling, so
 *    that strings are matched from the root down and spelled from the
 *    leaf up.  The roots are the codewords N6 + ordinal (6.2); an entry
 *    from N5 up without a parent is empty.
 *
 *    The encoder matches strings the same way in both modes.  In
 *    transparent mode each character goes out as soon as it joins a
 *    string (7.5 allows it); in compressed mode a string goes out as its
 *    codeword when it ends.  It changes modes on an estimate of what the
 *    last 256 characters would cost as codewords (7.8 leaves the test to
 *    the implementation), and never sends RESET.
 *
 *    The decoder repeats the encoder's string matching in transparent
 *    mode, so that both dictionaries stay alike (8), and outputs each
 *    character as soon as it knows it.
 *
 */

#include <new>

#include "v42bis.h"

namespace dlink {

/* control codewords (compressed mode) and command codes (after the
   escape character, transparent mode): Table 2 */
enum { ETM = 0, FLUSH = 1, STEPUP = 2 };
enum { ECM = 0, EID = 1, RESET = 2 };

enum {
	N6 = 3,                 /* control codewords: a root is N6 + ordinal */
	N5 = N6 + 256,          /* the first codeword of a longer string */
	MIN_CODEWORDS = 512,    /* N2, P1's default (5.1) */
	MIN_STRING = 6,         /* N7, P2's default (5.1) */
	ESCAPE_STEP = 51,       /* the escape character's step (9.2 b) */
	/* the compressibility estimate: bits for the last WINDOW characters
	   as codewords, starting at what they cost as characters; compressed
	   mode below TO_COMPRESSED, transparent mode above TO_TRANSPARENT */
	WINDOW = 256,
	EST_START = WINDOW * 8,
	TO_COMPRESSED = EST_START - 11,
	TO_TRANSPARENT = EST_START,
};

/* the estimate after one codeword of size bits stood for len more
   characters: it loses len / WINDOW of itself and gains the codeword */
static uint16_t estimate(uint16_t est, int len, int size)
{
	return static_cast<uint16_t>(est + size - est * len / WINDOW);
}

/* allocates the dictionary for N2 codewords and strings of up to N7
   characters, and starts the coder in the initial state with no
   statistics and no output; -1 if out of range (codewords are 16 bits,
   the string buffer holds V42BIS_MAX_STRING) or out of memory */
int V42bis::init(int dict_size, int max_string)
{
	exit();
	if (dict_size < MIN_CODEWORDS || dict_size > 65535 ||
	    max_string < MIN_STRING || max_string > V42BIS_MAX_STRING)
		return -1;
	dict_ = new (std::nothrow) Node[dict_size];
	if (!dict_)
		return -1;
	dict_size_ = dict_size;
	max_string_ = max_string;
	dict_init();
	raw_bits_ = cmp_bits_ = 0;
	olen_ = ohead_ = 0;
	return 0;
}

/* the parameters XID agreed (5.1), before any data: no more codewords
   than now, strings of MIN_STRING to V42BIS_MAX_STRING characters.  The
   dictionary stays as it is; -1, and nothing changed, if out of range
   (always when not ready()) */
int V42bis::config(int dict_size, int max_string)
{
	if (dict_size < MIN_CODEWORDS || dict_size > dict_size_ ||
	    max_string < MIN_STRING || max_string > V42BIS_MAX_STRING)
		return -1;
	dict_size_ = dict_size;
	max_string_ = max_string;
	return 0;
}

/* frees the dictionary and drops any output: not ready(), no codewords
   (so config() fails); harmless when repeated */
void V42bis::exit()
{
	delete[] dict_;
	dict_ = nullptr;
	dict_size_ = 0;
	olen_ = ohead_ = 0;
}

/* the initial state of 6.2 and 7.2, the same for the encoder and the
   decoder (C-INIT, or RESET from the remote): each tree its root only,
   C1 = N5, C2 = 9 and C3 = 512, transparent mode, escape character 0,
   no string, no pending update, nothing excluded, no bits waiting, the
   estimate at its start.  The statistics and the output stay */
void V42bis::dict_init()
{
	for (int i = 0; i < dict_size_; i++) {
		Node &n = dict_[i];
		n.ch = i >= N6 && i < N5 ? static_cast<uint8_t>(i - N6) : 0;
		n.parent = n.child = n.next = 0;
	}
	next_free_ = N5;
	cw_size_ = 9;
	/* C3 - 1, the largest codeword of C2 bits: C3 itself would not fit
	   16 bits once C2 reaches 16 */
	threshold_ = (1 << 9) - 1;
	mode_ = TRANSPARENT;
	escape_ = false;
	escape_char_ = 0;
	update_at_ = last_matched_ = last_added_ = 0;
	str_len_ = flushed_len_ = 0;
	bit_len_ = 0;
	bit_data_ = 0;
	cmp_last_ = EST_START;
}

/* the codeword of the string at + ch, 0 if the dictionary has no such
   string; at 0 is the empty string, whose extensions are the roots */
uint16_t V42bis::match_char(uint16_t at, uint8_t ch) const
{
	if (!at)
		return N6 + ch;
	for (uint16_t c = dict_[at].child; c; c = dict_[c].next)
		if (dict_[c].ch == ch)
			return c;
	return 0;
}

/* adds the string at + ch to the dictionary (6.4) unless it is there
   already, at codeword C1; it becomes the excluded entry.  Then node
   recovery (6.5): C1 moves on, wrapping from N2 to N5, to the next entry
   that is empty or a leaf, and a leaf there is detached from its parent.
   Callers never pass a string of N7 characters (6.4 a), so no string is
   longer than N7.  Returns the new entry's codeword, 0 if none */
uint16_t V42bis::add_char(uint16_t at, uint8_t ch)
{
	if (match_char(at, ch))
		return 0;
	uint16_t cw = next_free_;
	Node &n = dict_[cw];
	n.ch = ch;
	n.parent = at;
	n.child = 0;
	n.next = dict_[at].child;
	dict_[at].child = cw;
	last_added_ = cw;

	do {
		if (++next_free_ >= dict_size_)
			next_free_ = N5;
	} while (dict_[next_free_].child);
	Node &r = dict_[next_free_];
	if (r.parent) {
		uint16_t *link = &dict_[r.parent].child;
		while (*link != next_free_)
			link = &dict_[*link].next;
		*link = r.next;
		r.parent = r.next = 0;
	}
	return cw;
}

/* string matching (6.3) of the data character ch, the same for the
   encoder's characters and for the decoder's in transparent mode (8).
   First the dictionary update pending from the last string, with ch as
   its unmatched character (6.4, 7.6); then ch extends the current string
   if the dictionary has that string and it is not the excluded entry.
   Returns 1 if ch joined the string; 2 if it joined and the string has
   N7 characters now (it ends, and no update will follow it: 6.4 a); 0 if
   the string ends before ch: with no such string its update with ch is
   now pending, at the excluded entry nothing is and the entry is not
   excluded any more.  After 0 the caller ends the string and matches ch
   again, which then starts the next string */
int V42bis::recv_data(uint16_t ch)
{
	if (update_at_) {
		add_char(update_at_, static_cast<uint8_t>(ch));
		update_at_ = 0;
	}
	uint16_t at = match_char(last_matched_, static_cast<uint8_t>(ch));
	if (!at) {
		update_at_ = last_matched_;
		return 0;
	}
	if (at == last_added_) {
		last_added_ = 0;
		return 0;
	}
	last_matched_ = at;
	return ++str_len_ >= max_string_ ? 2 : 1;
}


/*
 *    the encoder
 *
 */

/* one character from the DTE (7.3): string matching, each string it
   ends passed on (7.5), the compressibility test after each of these
   and after the character joins a string (7.8).  Returns the octets it
   queued */
int V42bis::encode(uint8_t ch)
{
	int before = olen_;
	int r;
	while (!(r = recv_data(ch))) {
		send_string();
		test_mode();
	}
	/* the escape character procedure (9.2) as ch joins its string; in
	   transparent mode ch goes out now */
	if (mode_ == TRANSPARENT) {
		send_data(ch);
		if (ch == escape_char_)
			send_data(EID);
	}
	if (ch == escape_char_)
		escape_char_ += ESCAPE_STEP;
	if (r == 2)
		send_string();
	test_mode();
	return olen_ - before;
}

/* passes on the current string (7.5) and ends it: in compressed mode as
   its codeword, in transparent mode its characters are out already.  Its
   characters not yet counted (since its start, or since the last flush
   in transparent mode) count as one codeword of the size in effect
   before any STEPUP, in the estimate and the statistics.  Returns how
   many characters that was */
int V42bis::send_string()
{
	int len = str_len_ - flushed_len_;
	if (len > 0) {
		int size = cw_size_;
		if (mode_ == COMPRESSED)
			send_cw(last_matched_);
		cmp_last_ = estimate(cmp_last_, len, size);
		raw_bits_ += 8 * len;
		cmp_bits_ += size;
	}
	last_matched_ = 0;
	str_len_ = flushed_len_ = 0;
	return len;
}

/* the compressibility test (7.8): to compressed mode when the estimate
   is below TO_COMPRESSED, to transparent mode when it is above
   TO_TRANSPARENT.  A change cuts string matching short (6.3 ii): the
   partial string, if any, is passed on in the mode being left and
   becomes the pending update with the next character (7.8.1 a, 7.8.2 b);
   without one, an update already pending stays.  Returns 1 if the mode
   changed */
int V42bis::test_mode()
{
	if (mode_ == TRANSPARENT ? cmp_last_ >= TO_COMPRESSED :
	    cmp_last_ <= TO_TRANSPARENT)
		return 0;
	if (last_matched_)
		update_at_ = last_matched_;
	send_string();
	if (mode_ == TRANSPARENT) {
		/* the escape character as it is, then ECM; the first codeword
		   starts at bit 1 of the next octet (7.8.1, 7.5) */
		send_data(escape_char_);
		send_data(ECM);
		mode_ = COMPRESSED;
	} else {
		/* ETM and zeros to the octet boundary (7.8.2, 7.5); nothing is
		   excluded any more, as in the decoder on ETM */
		send_cw(ETM);
		align_bits();
		last_added_ = 0;
		mode_ = TRANSPARENT;
	}
	return 1;
}

/* the codeword cw in C2 bits (7.4), after as many STEPUPs as it needs
   (each in the size it leaves, C3 doubling); its whole octets go out */
void V42bis::send_cw(uint16_t cw)
{
	while (cw > threshold_) {
		bit_data_ |= static_cast<uint32_t>(STEPUP) << bit_len_;
		bit_len_ += cw_size_++;
		threshold_ = static_cast<uint16_t>(threshold_ << 1 | 1);
		send_bits();
	}
	bit_data_ |= static_cast<uint32_t>(cw) << bit_len_;
	bit_len_ += cw_size_;
	send_bits();
}

/* zeros up to the octet boundary (after FLUSH and ETM, 7.5), and the
   last octet out */
void V42bis::align_bits()
{
	bit_len_ = (bit_len_ + 7) & ~7;
	send_bits();
}

/* the whole octets of the bit reservoir to the output, bit 1 first
   (Figure 3); returns how many */
int V42bis::send_bits()
{
	int n = 0;
	for (; bit_len_ >= 8; n++) {
		send_data(bit_data_ & 0xff);
		bit_data_ >>= 8;
		bit_len_ -= 8;
	}
	return n;
}

/* C-FLUSH (7.9, 5.7), each time the DTE has nothing more for now.  In
   transparent mode the characters are out already: those since the last
   flush count as a string passed on, but string matching goes on and no
   update is made.  In compressed mode the partial string, if any, goes
   as its codeword and becomes the pending update; then FLUSH and zeros
   to the octet boundary, unless no codeword went and no bits wait.  With
   nothing new since the last flush nothing happens.  Returns the octets
   it queued */
int V42bis::flush_encoder()
{
	int before = olen_;
	if (mode_ == TRANSPARENT) {
		int len = str_len_ - flushed_len_;
		if (len > 0) {
			cmp_last_ = estimate(cmp_last_, len, cw_size_);
			raw_bits_ += 8 * len;
			cmp_bits_ += cw_size_;
			flushed_len_ = str_len_;
		}
		return 0;
	}
	bool sent = last_matched_ != 0;
	if (sent) {
		update_at_ = last_matched_;
		send_string();
	}
	if (sent || bit_len_) {
		send_cw(FLUSH);
		align_bits();
	}
	return olen_ - before;
}


/*
 *    the decoder
 *
 */

/* one octet of a received I frame (8); the characters it gives join the
   output.  Returns how many, or -1 on C-ERROR (5.8) */
int V42bis::decode(uint8_t ch)
{
	if (mode_ == COMPRESSED) {
		/* the octet's bits join the reservoir, the least significant
		   first (Figure 3); a codeword has 9 bits or more, so no more
		   than one ends in this octet */
		bit_data_ |= static_cast<uint32_t>(ch) << bit_len_;
		bit_len_ += 8;
		if (bit_len_ < cw_size_)
			return 0;
		uint16_t cw = static_cast<uint16_t>(bit_data_ & threshold_);
		bit_data_ >>= cw_size_;
		bit_len_ -= cw_size_;
		switch (cw) {
		case ETM:
			/* transparent mode from the next octet, no string and
			   nothing excluded; an update pending from the last
			   codeword waits for the first character (7.8.2 b) */
			mode_ = TRANSPARENT;
			last_matched_ = 0;
			str_len_ = 0;
			last_added_ = 0;
			[[fallthrough]];
		case FLUSH:
			/* the rest of the octet is padding (7.5, 7.9 c) */
			bit_data_ = 0;
			bit_len_ = 0;
			return 0;
		case STEPUP:
			/* no further than N1 (5.8 a): C2 is N1 once its largest
			   codeword reaches N2 - 1 */
			if (threshold_ >= dict_size_ - 1)
				return -1;
			cw_size_++;
			threshold_ = static_cast<uint16_t>(threshold_ << 1 | 1);
			return 0;
		}
		/* a string: not beyond the dictionary nor an empty entry, such
		   as C1 (5.8 b, c) */
		if (cw >= dict_size_ || (cw >= N5 && !dict_[cw].parent))
			return -1;
		/* spelled from the leaf up into the end of str_data_ (no string
		   is longer than N7) */
		int pos = V42BIS_MAX_STRING;
		for (uint16_t at = cw;; at = dict_[at].parent) {
			str_data_[--pos] = dict_[at].ch;
			if (at < N5)
				break;
		}
		/* the last string plus this one's first character (8, 6.4);
		   C-ERROR if node recovery then took this very entry, which the
		   encoder could not have had */
		if (update_at_) {
			add_char(update_at_, str_data_[pos]);
			update_at_ = 0;
			if (cw >= N5 && !dict_[cw].parent)
				return -1;
		}
		int len = V42BIS_MAX_STRING - pos;
		if (len < max_string_)
			update_at_ = cw;
		/* out, the escape character cycling on its own (9.2 b) */
		for (; pos < V42BIS_MAX_STRING; pos++) {
			uint8_t c = str_data_[pos];
			send_data(c);
			if (c == escape_char_)
				escape_char_ += ESCAPE_STEP;
		}
		return len;
	}

	/* transparent mode: a command code after the escape character */
	if (escape_) {
		escape_ = false;
		switch (ch) {
		case ECM:
			/* compressed mode from the next octet, its first
			   codeword from bit 1; the current string, if any, is
			   the update pending for that codeword (7.8.1 a) */
			if (last_matched_)
				update_at_ = last_matched_;
			last_matched_ = 0;
			str_len_ = 0;
			mode_ = COMPRESSED;
			return 0;
		case EID:
			ch = escape_char_;
			escape_char_ += ESCAPE_STEP;
			break;
		case RESET:
			dict_init();
			return 0;
		default:
			return -1;      /* reserved (5.8 d) */
		}
	} else if (ch == escape_char_) {
		escape_ = true;
		return 0;
	}
	/* a data character: matched as the encoder did, then out */
	int r;
	do {
		r = recv_data(ch);
		if (r != 1) {
			last_matched_ = 0;
			str_len_ = 0;
		}
	} while (!r);
	send_data(ch);
	return 1;
}

/* the end of a received frame.  The decoder holds no characters back
   (each goes out as soon as it is known), and string matching and an
   escape character just received carry on into the next frame, so
   there is nothing to do; returns 0 characters */
int V42bis::flush_decoder()
{
	return 0;
}


/*
 *    output
 *
 */

/* one octet (the encoder's) or character (the decoder's) onto the end
   of the output queue; dropped if it is full (callers take the output
   before each encode() and decode(), which queue far less).  Returns 1
   if queued */
int V42bis::send_data(uint16_t ch)
{
	if (olen_ >= static_cast<int>(sizeof(obuf_)))
		return 0;
	obuf_[(ohead_ + olen_++) % static_cast<int>(sizeof(obuf_))] =
		static_cast<uint8_t>(ch);
	return 1;
}

/* takes up to n octets from the front of the output queue into buf;
   returns how many */
int V42bis::out(uint8_t *buf, int n)
{
	if (n > olen_)
		n = olen_;
	if (n <= 0)
		return 0;
	for (int i = 0; i < n; i++) {
		buf[i] = obuf_[ohead_];
		ohead_ = (ohead_ + 1) % static_cast<int>(sizeof(obuf_));
	}
	olen_ -= n;
	return n;
}

} // namespace dlink
