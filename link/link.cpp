/*
 *
 *    link.cpp - the data link layer: detection, LAPM or async, V.42bis
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "link.h"

namespace dlink {

Link::~Link()
{
	exit();
}

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
	}
	if (cfg.ec)
		lapm_.init(this, cfg.tx_win, cfg.rx_win, cfg.tx_info,
			   cfg.rx_info);
	return 0;
}

void Link::exit()
{
	lapm_.exit();
	enc_.exit();
	dec_.exit();
	mode_ = Mode::Idle;
	timer_ = 0;
	push_pending = false;
}

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

void Link::status(Status s)
{
	owner_->status(s);
}

void Link::start(bool ec, bool detect)
{
	cfg_.ec = ec;
	if (ec && detect) {
		mode_ = Mode::Detect;
		det_.start(this);
	} else
		detected(ec);
}

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

void Link::stop()
{
	mode_ = Mode::Idle;
}

void Link::disconnect()
{
	if (mode_ != Mode::Lapm)
		return;
	timer_ = 0;
	push_pending = false;
	lapm_.disconnect();
}

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

/* XID: the remote's V.42bis parameters (6.4/V.42bis), ours if it asks
   for none */
void Link::update_config(const Config &remote)
{
	if (remote.comp && cfg_.comp) {
		cfg_.comp = remote.comp;
		if (!enc_.config(remote.dict_size, remote.max_string) &&
		    !dec_.config(remote.dict_size, remote.max_string)) {
			cfg_.dict_size = remote.dict_size;
			cfg_.max_string = remote.max_string;
		} else
			logf("err: v42bis: setup failed (%d/%d).",
			     remote.dict_size, remote.max_string);
	} else
		cfg_.comp = 0;
	owner_->negotiated(cfg_);
}


/*
 *    characters
 *
 */

int Link::read_chars(uint8_t *buf, int n)
{
	if (!online_)
		return 0;
	if (cfg_.ec && cfg_.comp & 2 && enc_.ready())
		return comp_read(buf, n);
	return owner_->read_chars(buf, n);
}

int Link::write_chars(const uint8_t *buf, int n)
{
	if (!online_)
		return 0;
	if (cfg_.ec && cfg_.comp & 1 && dec_.ready())
		return comp_write(buf, n);
	return owner_->write_chars(buf, n);
}

/* the DTE's characters through the encoder: as many as fit, then a
   flush when the DTE has no more */
int Link::comp_read(uint8_t *buf, int n)
{
	int ret = 0;
	uint8_t ch;
	while (ret < n) {
		ret += enc_.out(buf + ret, n - ret);
		if (ret == n)
			break;
		if (owner_->read_chars(&ch, 1) != 1)
			break;
		enc_.encode(ch);
	}
	if (ret < n) {
		enc_.flush_encoder();
		ret += enc_.out(buf + ret, n - ret);
	}
	return ret;
}

/* decoded characters to the DTE; false while it has not taken them
   all */
bool Link::comp_flush_output()
{
	for (;;) {
		if (!dec_count_) {
			dec_head_ = 0;
			dec_count_ = dec_.out(dec_out_, sizeof(dec_out_));
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
int Link::comp_write(const uint8_t *buf, int n)
{
	int i = 0;
	if (!comp_flush_output())
		return 0;
	while (i < n) {
		if (dec_.decode(buf[i++]) < 0) {
			logf("decoder error. %d(%d)", i, n);
			status(Status::Error);
			break;
		}
		if (!comp_flush_output())
			break;
	}
	if (i == n) {
		dec_.flush_decoder();
		comp_flush_output();
	}
	return i;
}

} // namespace dlink
