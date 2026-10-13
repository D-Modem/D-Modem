/*
 *
 *    link.cpp - the data link layer: detection, LAPM or async, V.42bis
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "link.h"

namespace dlink {

/* frees what init() allocated */
Link::~Link()
{
	exit();
}

/* a call: the V.42bis and V.44 coders at the largest sizes XID can
   agree on (with ec and comp), LAPM's frames; -1 if they cannot be
   allocated */
int Link::init(Owner *owner, bool caller, const Config &cfg)
{
	owner_ = owner;
	caller_ = caller;
	cfg_ = cfg;
	online_ = false;
	mode_ = Mode::Idle;
	timer_ = 0;
	timer_fn_ = nullptr;
	push_pending = false;
	dec_head_ = dec_count_ = 0;
	if (cfg.ec && cfg.comp) {
		if (enc_.init(cfg.dict_size, cfg.max_string) ||
		    dec_.init(cfg.dict_size, cfg.max_string)) {
			enc_.exit();
			return -1;
		}
		if (cfg.v44 &&
		    (enc44_.init(cfg.v44_codewords, cfg.v44_max_string,
				 cfg.v44_history) ||
		     dec44_.init(cfg.v44_codewords, cfg.v44_max_string,
				 cfg.v44_history))) {
			enc_.exit();
			dec_.exit();
			enc44_.exit();
			return -1;
		}
	} else {
		cfg_.v44 = false;
	}
	if (cfg.ec)
		lapm_.init(this, cfg.tx_win, cfg.rx_win, cfg.tx_info,
			   cfg.rx_info);
	return 0;
}

/* the end of the call: coders freed, no mode, no timer */
void Link::exit()
{
	lapm_.exit();
	enc_.exit();
	dec_.exit();
	enc44_.exit();
	dec44_.exit();
	mode_ = Mode::Idle;
	timer_ = 0;
	push_pending = false;
}

/* a line of debug output to the owner (up to 255 characters) */
void Link::logf(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	if (!owner_)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	owner_->log(line);
}

/* the link's progress to the owner */
void Link::status(Status s)
{
	owner_->status(s);
}

/* the data pump has connected: the detection phase, or at once LAPM or
   async */
void Link::start(bool ec, bool detect)
{
	cfg_.ec = ec;
	if (ec && detect) {
		mode_ = Mode::Detect;
		det_.start(this);
	} else
		detected(ec);
}

/* the detection phase is over (or skipped): Detected, then LAPM, or
   async and Up at once */
void Link::detected(bool ec)
{
	cfg_.ec = ec;
	owner_->status(Status::Detected);
	if (ec) {
		mode_ = Mode::Lapm;
		hdlc_.start(this);
		lapm_.start();
	} else {
		mode_ = Mode::Async;
		async_.start(this);
		owner_->status(Status::Up);
	}
}

/* the detection phase ends without a result: its timer (T400, the
   ADPs') must not report the link up after the hangup */
void Link::end_detect()
{
	if (mode_ != Mode::Detect)
		return;
	timer_ = 0;
	timer_fn_ = nullptr;
	mode_ = Mode::Idle;
}

/* the data pump is gone: marks out, bits in dropped (LAPM's bit timer
   is left as it is; the detection phase's ends) */
void Link::stop()
{
	end_detect();
	mode_ = Mode::Idle;
}

/* we hang up: LAPM sends DISC; the detection phase ends; nothing in the
   other modes */
void Link::disconnect()
{
	end_detect();
	if (mode_ != Mode::Lapm)
		return;
	timer_ = 0;
	push_pending = false;
	lapm_.disconnect();
}

/* the data pump's transmitter: n items of nbits bits from the mode's
   framer (marks when idle); the bit timer counts them, and characters
   LAPM still holds are pushed on; returns n */
int Link::get_bits(int nbits, uint8_t *buf, int n)
{
	switch (mode_) {
	case Mode::Detect:
		n = det_.get_bits(nbits, buf, n);
		break;
	case Mode::Lapm:
		n = hdlc_.get_bits(nbits, buf, n);
		break;
	case Mode::Async:
		n = async_.get_bits(nbits, buf, n);
		break;
	case Mode::Idle:
		memset(buf, 0xff, n);
		break;
	}
	/* the bit timer counts what goes out */
	if (timer_ && (timer_ -= (long)nbits * n) <= 0) {
		timer_ = 0;
		if (timer_fn_)
			timer_fn_(this);
	}
	if (push_pending)
		lapm_.push_rest();
	return n;
}

/* the data pump's receiver: n items of nbits bits to the mode's
   framer (dropped when idle); returns n */
int Link::put_bits(int nbits, const uint8_t *buf, int n)
{
	switch (mode_) {
	case Mode::Detect:
		return det_.put_bits(nbits, buf, n);
	case Mode::Lapm:
		return hdlc_.put_bits(nbits, buf, n);
	case Mode::Async:
		return async_.put_bits(nbits, buf, n);
	case Mode::Idle:
		break;
	}
	return n;
}

/* XID: the directions both modems want (remote's P0, already from our
   side, and ours); V.44 if both have it, else V.42bis, either with the
   smaller of the two modems' parameters, the same both ways (6.4/V.42bis);
   no compression if they cannot be had */
void Link::update_config(const Config &remote)
{
	int dir = remote.comp & cfg_.comp;

	if (dir && remote.v44 && cfg_.v44 && enc44_.ready()) {
		int n2 = std::min(cfg_.v44_codewords, remote.v44_codewords);
		int n7 = std::min(cfg_.v44_max_string, remote.v44_max_string);
		int n8 = std::min(cfg_.v44_history, remote.v44_history);
		if (!enc44_.config(n2, n7, n8) && !dec44_.config(n2, n7, n8)) {
			cfg_.comp = dir;
			cfg_.v44_codewords = n2;
			cfg_.v44_max_string = n7;
			cfg_.v44_history = n8;
			logf("v44: %d codewords, strings of %d, history %d",
			     n2, n7, n8);
			owner_->negotiated(cfg_);
			return;
		}
		logf("err: v44: setup failed (%d/%d/%d).", n2, n7, n8);
	}
	cfg_.v44 = false;
	cfg_.comp = 0;
	if (dir) {
		int n2 = std::min(cfg_.dict_size, remote.dict_size);
		int n7 = std::min(cfg_.max_string, remote.max_string);
		if (!enc_.config(n2, n7) && !dec_.config(n2, n7)) {
			cfg_.comp = dir;
			cfg_.dict_size = n2;
			cfg_.max_string = n7;
		} else
			logf("err: v42bis: setup failed (%d/%d).", n2, n7);
	}
	owner_->negotiated(cfg_);
}


/*
 *    characters
 *
 */

/* characters for LAPM's I frames or the async framer: the DTE's,
   through the encoder when we compress; 0 while not online */
int Link::read_chars(uint8_t *buf, int n)
{
	if (!online_)
		return 0;
	if (cfg_.ec && cfg_.comp & 2 &&
	    (cfg_.v44 ? enc44_.ready() : enc_.ready()))
		return comp_read(buf, n);
	return owner_->read_chars(buf, n);
}

/* received characters to the DTE, through the decoder when the remote
   compresses; returns how many were taken (0 while not online) */
int Link::write_chars(const uint8_t *buf, int n)
{
	if (!online_)
		return 0;
	if (cfg_.ec && cfg_.comp & 1 &&
	    (cfg_.v44 ? dec44_.ready() : dec_.ready()))
		return comp_write(buf, n);
	return owner_->write_chars(buf, n);
}

/* read_chars() through V.44's or V.42bis's encoder */
int Link::comp_read(uint8_t *buf, int n)
{
	return cfg_.v44 ? comp_read_with(enc44_, buf, n) :
		comp_read_with(enc_, buf, n);
}

/* write_chars() through V.44's or V.42bis's decoder */
int Link::comp_write(const uint8_t *buf, int n)
{
	return cfg_.v44 ? comp_write_with(dec44_, buf, n) :
		comp_write_with(dec_, buf, n);
}

/* the decoder's output to the DTE; false while some is left */
bool Link::comp_flush_output()
{
	return cfg_.v44 ? comp_flush_with(dec44_) : comp_flush_with(dec_);
}

/* the DTE's characters through the encoder: as many as fit, then a
   flush when the DTE has no more */
template <class E>
int Link::comp_read_with(E &enc, uint8_t *buf, int n)
{
	int ret = 0;
	uint8_t ch;
	while (ret < n) {
		ret += enc.out(buf + ret, n - ret);
		if (ret == n)
			break;
		if (owner_->read_chars(&ch, 1) != 1)
			break;
		enc.encode(ch);
	}
	if (ret < n) {
		enc.flush_encoder();
		ret += enc.out(buf + ret, n - ret);
	}
	return ret;
}

/* decoded characters to the DTE; false while it has not taken them
   all */
template <class D>
bool Link::comp_flush_with(D &dec)
{
	for (;;) {
		if (!dec_count_) {
			dec_head_ = 0;
			dec_count_ = dec.out(dec_out_, sizeof(dec_out_));
			if (!dec_count_)
				return true;
		}
		int w = owner_->write_chars(dec_out_ + dec_head_, dec_count_);
		if (w <= 0)
			return false;
		dec_head_ += w;
		dec_count_ -= w;
	}
}

/* received characters through the decoder; returns how many it took
   (LAPM keeps the rest until the DTE takes the output) */
template <class D>
int Link::comp_write_with(D &dec, const uint8_t *buf, int n)
{
	int i = 0;
	if (!comp_flush_output())
		return 0;
	while (i < n) {
		if (dec.decode(buf[i++]) < 0) {
			logf("decoder error. %d(%d)", i, n);
			status(Status::Error);
			break;
		}
		if (!comp_flush_output())
			break;
	}
	if (i == n) {
		dec.flush_decoder();
		comp_flush_output();
	}
	return i;
}

} // namespace dlink
