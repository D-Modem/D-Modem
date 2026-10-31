/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_relay.cpp - the relayed call's pump (DP_RELAY): V.150.1's modem
 *                   relay, where the far end's gateway terminates the
 *                   modulation and the host carries the call's data
 *                   octets over IP itself (dmodemd's SIP line, SPRT);
 *                   the pump moves them between the host (struct
 *                   dp_relay, MDMPRM_RELAY) and the core's link as
 *                   start-stop characters (8N1), and sends silence
 *
 *    It waits for the relay to come up (the far end's modem trained
 *    with its gateway), CONNECT at the rates the relay gives, then each
 *    block moves as many characters each way as those rates allow in
 *    the block's time: the DTE's to the host (held while the host has
 *    no room, which is the flow control the far end's window gives),
 *    the host's to the DTE.  No relay within S7 of the start, or the
 *    relay over, is an ERROR (NO CARRIER).
 *
 */

#include <algorithm>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"

namespace dsp {

enum class RelaySt { WAIT, DATA };

class RelayPump : public PumpBase<RelayPump, RelaySt> {
public:
	static constexpr const char *name = "RELAY";
	/* its name in the log */
	const char *tag() const { return "relay"; }

	/* a character's bits: start, 8 data, stop */
	static constexpr s32 CHAR_BITS = 10;
	/* characters a block may carry beyond its rate (the host's late
	   octets catch up) */
	static constexpr s32 SLACK = 64;

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 process(void *in, void *out, s32 cnt) override;

private:
	s32 dte_char();
	void to_link(const u8 *b, s32 n);
	void move(s32 cnt);

	const struct dp_relay *relay_ = nullptr;
	s32 tx_rate_ = 0, rx_rate_ = 0;
	s64 tx_credit_ = 0, rx_credit_ = 0;  /* in bits x srate */
	std::vector<u8> held_;          /* the DTE's, the host had no room */
	bool over_ = false;             /* (told once) */
};

/* the host's relay (MDMPRM_RELAY): 0, or -1 when it has none */
s16 RelayPump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_,
		     s32)
{
	pump_setup(m, srate_, caller_);
	intptr_t p = modem_get_param(m, MDMPRM_RELAY);
	if (p == 0 || p == -1) {
		DSP_DBG("relay: the line relays nothing\n");
		return -1;
	}
	relay_ = reinterpret_cast<const struct dp_relay *>(p);
	set_state(RelaySt::WAIT);
	stat_dp(DP_RELAY);
	DSP_DBG("relay: start, %s\n", caller_ ? "caller" : "answerer");
	return 0;
}

/* the DTE's next character off the link's 8N1 bit stream (its idle
   marks one at a time), -1 for none */
s32 RelayPump::dte_char()
{
	u8 b = 1, bits[9];
	s32 c = 0;

	modem_get_bits(modem, 1, &b, 1);
	if (b & 1)
		return -1;
	modem_get_bits(modem, 1, bits, 9);
	for (s16 i = 0; i < 8; i++)
		c |= (bits[i] & 1) << i;
	return c;
}

/* n octets to the DTE, each in 8N1 */
void RelayPump::to_link(const u8 *b, s32 n)
{
	for (s32 k = 0; k < n; k++) {
		u8 bits[CHAR_BITS];
		bits[0] = 0;
		for (s16 i = 0; i < 8; i++)
			bits[1 + i] = (b[k] >> i) & 1;
		bits[9] = 1;
		modem_put_bits(modem, 1, bits, CHAR_BITS);
	}
}

/* a block of cnt samples' worth of characters each way */
void RelayPump::move(s32 cnt)
{
	const s64 unit = static_cast<s64>(CHAR_BITS) * srate;
	const s64 cap = unit * SLACK;

	/* the DTE's: what is held first, then new characters */
	tx_credit_ = std::min(tx_credit_ + static_cast<s64>(tx_rate_) * cnt,
			      cap);
	while (tx_credit_ >= unit) {
		if (held_.empty()) {
			s32 c = dte_char();
			if (c < 0) {
				/* (an idle line saves no time up) */
				tx_credit_ = std::min(tx_credit_, unit);
				break;
			}
			held_.push_back(static_cast<u8>(c));
		}
		int took = relay_->write(relay_->ctx, held_.data(),
					 static_cast<int>(held_.size()));
		if (took <= 0)
			break;
		held_.erase(held_.begin(), held_.begin() + took);
		tx_credit_ -= unit * took;
	}

	/* the far end's */
	rx_credit_ = std::min(rx_credit_ + static_cast<s64>(rx_rate_) * cnt,
			      cap);
	s32 n = static_cast<s32>(rx_credit_ / unit);
	if (n > 0) {
		u8 buf[SLACK];
		int got = relay_->read(relay_->ctx, buf,
				       static_cast<int>(std::min(n, SLACK)));
		if (got > 0) {
			to_link(buf, got);
			rx_credit_ -= unit * got;
		} else {
			rx_credit_ = std::min(rx_credit_, unit);
		}
	}
}

/* cnt samples of silence out (the line's samples are not the call's):
   the relay's start, CONNECT, the data; ERROR when it does not come up
   within S7 or is over */
s16 RelayPump::process(void *, void *out, s32 cnt)
{
	s16 *tx = static_cast<s16 *>(out);
	s16 ret = DPSTAT_OK;
	s32 t = 0, r = 0;

	for (s32 i = 0; i < cnt; i++)
		tx[i] = 0;
	total += cnt;
	count += cnt;
	int st = relay_->state(relay_->ctx, &t, &r);
	if (st < 0) {
		if (!over_)
			DSP_DBG("relay: over\n");
		over_ = true;
		return DPSTAT_ERROR;
	}
	if (state == RelaySt::WAIT) {
		if (st > 0) {
			tx_rate_ = t > 0 ? t : 0;
			rx_rate_ = r > 0 ? r : 0;
			set_state(RelaySt::DATA);
			return connect_at(tx_rate_, rx_rate_);
		}
		if (total >= timeout) {
			DSP_DBG("relay: not up within S7\n");
			return DPSTAT_ERROR;
		}
		return ret;
	}
	/* (the core had our CONNECT a block ago: the link is there) */
	move(cnt);
	return ret;
}

/* registers the pump as DP_RELAY */
s16 dp_relay_init(void)
{
	return modem_dp_register(DP_RELAY, &PumpOps<RelayPump>::ops);
}

/* and no more */
void dp_relay_exit(void)
{
	modem_dp_deregister(DP_RELAY, &PumpOps<RelayPump>::ops);
}

} // namespace dsp
