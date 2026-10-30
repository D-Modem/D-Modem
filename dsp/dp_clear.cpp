/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_clear.cpp - the clear channel pump (DP_CLEAR, AT+MS=CLEAR or 64):
 *                   a 64 kbit/s digital channel with no modulation, such
 *                   as an ISDN B-channel's unrestricted digital bearer
 *                   (a SIP call in CLEARMODE, RFC 4040, or a PBX's data
 *                   call): each of the line's octets carries 8 of the
 *                   link layer's bits each way, the first the most
 *                   significant (as I.460 and RFC 4040 have it), so the
 *                   rate is 64000 bit/s both ways; or V.110's rate
 *                   adaption on it (v110.h), at a user rate of 300 to
 *                   56000 bit/s
 *
 *    The line's samples are its octets through the G.711 law of the
 *    line (dp_runtime law): A-law gives every octet back as it was
 *    (a host with a clear channel takes A-law for it); mu-law has one
 *    sample for both its zeros, so that 0xFF arrives for 0x7F.  The
 *    core's DC removal is off while the pump runs.
 *
 *    The start at 64 kbit/s, D-Modem's own: each end sends HDLC flags
 *    (0x7E) until it has heard HEAR of the other end's in a row, then
 *    TAIL more for the other end to hear, then MARK octets of ones, and
 *    then the link layer's bits, with CONNECT.  What the far end sends
 *    from its own ones on goes to the link layer (whatever came before
 *    them, its flags, is dropped); a far end that sends something else
 *    after its flags (other ISDN equipment, whose link layer starts with
 *    them: X.75's SABM) is taken from there, a flag first.  No flags
 *    within S7 of the start is an ERROR (NO CARRIER).  On a clear
 *    channel nothing tells a carrier from none: the call ends when the
 *    line ends it.
 *
 *    V.110 (dp_runtime v110, AT+CBST): each end sends frames, S and X
 *    OFF, its D bits ones, until it is in frame sync with the other's,
 *    then S and X ON; CONNECT at the user rate once in sync and the
 *    other's X is ON.  The user's bits go to the link layer while its S
 *    bits are ON (ones otherwise, and while out of sync).  56000 bit/s
 *    has no frames, and connects at once.  An answerer with no V.110
 *    rate of its own looks for frames at each intermediate rate while it
 *    waits for flags: the first in sync, by its E bits, gives the rate.
 *
 */

#include <algorithm>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "g711.h"
#include "pump.h"
#include "v110.h"

namespace dsp {

enum class ClearSt { SYNC, DATA };

class ClearPump : public PumpBase<ClearPump, ClearSt> {
public:
	static constexpr const char *name = "CLEAR";
	/* its name in the log */
	const char *tag() const { return "clear"; }

	/* the line's rate, each way */
	static constexpr s32 RATE = 64000;
	/* the far end's flags in a row that start the call (4 ms) */
	static constexpr s32 HEAR = 32;
	/* our flags after we heard the far end's (8 ms), our ones after
	   them (8 ms) */
	static constexpr s32 TAIL = 64;
	static constexpr s32 MARK = 64;
	static constexpr u8 FLAG = 0x7E;
	static constexpr u8 ONES = 0xFF;
	/* the intermediate rates an answerer looks for frames at */
	static constexpr s16 HUNTS = 4;

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	~ClearPump() override;
	s16 process(void *in, void *out, s32 cnt) override;

private:
	u8 source();
	void sink(u8 v);
	void put_bits(const u8 *b, s32 n);
	void v110_start(const V110Rate *r, const V110Sync *synced);
	s16 v110_rx(u8 r);
	u8 v110_tx();
	s16 clear_rx(u8 r);
	u8 clear_tx(s16 &ret);
	bool hunt(u8 r);

	s16 alaw_ = 1;
	s16 saved_pcm_ = 0;
	s32 run_ = 0;                   /* the far end's flags in a row */
	bool heard_ = false;            /* HEAR of them came */
	bool open_ = false;             /* its ones came: data from it */
	bool reported_ = false;         /* CONNECT returned to the core */
	s32 tail_ = 0, mark_ = 0;       /* ours sent */
	std::vector<u8> held_;          /* the far end's bits before our
					   CONNECT reached the core */

	/* V.110 */
	const V110Rate *v110_ = nullptr;
	bool hunting_ = false;          /* the answerer looks for frames */
	V110Sync hunt_[HUNTS];
	V110Adapt adapt_;
	V110Sync rx_sync_;
	s16 ir_bits_ = 8;               /* intermediate rate's bits an octet */
	u8 tx_bits_[80] = {};
	s16 tx_pos_ = 80;
};

/* the pump at 8000 Hz (a clear channel has no other rate): the core's
   DC removal off; V.110 at the rate asked for, or looked for by the
   answerer; 0, or -1 */
s16 ClearPump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_,
		     s32)
{
	if (srate_ != 8000) {
		DSP_DBG("clear: %" PRId32 " Hz: a clear channel is 8000 "
			"octets a second\n", srate_);
		return -1;
	}
	pump_setup(m, srate_, caller_);
	alaw_ = rt && rt->law == 1 ? 0 : 1;
	if (rt) {
		saved_pcm_ = rt->pcm;
		rt->pcm = 1;
		rt->v110_rate = 0;
	}
	set_state(ClearSt::SYNC);
	stat_dp(DP_CLEAR);
	stat_line(8000, 0, 8000, 0);
	DSP_DBG("clear: start, %s, the octets as %s\n",
		caller_ ? "caller" : "answerer",
		alaw_ ? "A-law" : "mu-law (0xFF arrives for 0x7F)");
	s32 asked = rt ? rt->v110 : 0;
	if (asked) {
		const V110Rate *r = v110_rate(asked);
		if (!r) {
			DSP_DBG("clear: V.110 has no %" PRId32 " bit/s\n",
				asked);
			return -1;
		}
		v110_start(r, nullptr);
	} else if (!caller_) {
		hunting_ = true;
		for (V110Sync &h : hunt_)
			h.init();
	}
	return 0;
}

/* gives the core its DC removal back as it was */
ClearPump::~ClearPump()
{
	if (rt)
		rt->pcm = saved_pcm_;
}

/* an octet of the link layer's bits, the first the most significant
   (ones when it has none) */
u8 ClearPump::source()
{
	u8 b[8];
	std::fill_n(b, 8, 1);
	modem_get_bits(modem, 1, b, 8);
	u8 v = 0;
	for (s16 i = 0; i < 8; i++)
		v = static_cast<u8>(v | (b[i] & 1) << (7 - i));
	return v;
}

/* n bits received to the link layer, or held until the core has our
   CONNECT */
void ClearPump::put_bits(const u8 *b, s32 n)
{
	if (reported_) {
		modem_put_bits(modem, 1, const_cast<u8 *>(b), n);
		return;
	}
	held_.insert(held_.end(), b, b + n);
}

/* an octet received to the link layer, the first bit the most
   significant */
void ClearPump::sink(u8 v)
{
	u8 b[8];
	for (s16 k = 0; k < 8; k++)
		b[k] = (v >> (7 - k)) & 1;
	put_bits(b, 8);
}

/* V.110 at r from now on: frames out at once (or 56000's octets);
   synced: the frame sync an answerer's look found, kept */
void ClearPump::v110_start(const V110Rate *r, const V110Sync *synced)
{
	v110_ = r;
	hunting_ = false;
	adapt_.init(r);
	ir_bits_ = static_cast<s16>(r->ir / 8000);
	if (synced)
		rx_sync_ = *synced;
	else
		rx_sync_.init();
	tx_pos_ = 80;
	DSP_DBG("clear: V.110 at %" PRId32 " bit/s (intermediate rate %"
		PRId32 ")%s\n", r->user, r->ir,
		synced ? ", the caller's frames" : "");
}

/* the answerer's look for V.110 frames: the octet's first 1, 2, 4 and 8
   bits at each intermediate rate; the first rate in sync whose E bits
   name a user rate is the call's: true when it is found, the octet's
   bits after the frame given to V.110's frame sync */
bool ClearPump::hunt(u8 r)
{
	for (s16 h = 0; h < HUNTS; h++) {
		s16 k = static_cast<s16>(1 << h);
		for (s16 i = 0; i < k; i++) {
			if (!hunt_[h].put((r >> (7 - i)) & 1))
				continue;
			const V110Frame &f = hunt_[h].frame();
			u8 e = static_cast<u8>(f.e[0] | f.e[1] << 1 |
					       f.e[2] << 2);
			const V110Rate *rate = v110_rate_found(8000 * k, e);
			if (!rate)
				continue;
			V110Sync s = hunt_[h];
			/* (the bits after this frame's end in the octet are
			   the next frame's: given to it below) */
			v110_start(rate, &s);
			for (s16 j = static_cast<s16>(i + 1); j < k; j++)
				rx_sync_.put((r >> (7 - j)) & 1);
			return true;
		}
	}
	return false;
}

/* an octet of V.110: its intermediate rate's bits to frame sync, a
   frame's user bits to the link while its S bits are ON; DPSTAT_CONNECT
   when in sync and the other end's X is ON */
s16 ClearPump::v110_rx(u8 r)
{
	s16 ret = DPSTAT_OK;
	if (v110_->fill == V110Fill::Octet56) {
		u8 b[7];
		for (s16 i = 0; i < 7; i++)
			b[i] = (r >> (7 - i)) & 1;
		if (state == ClearSt::DATA)
			put_bits(b, 7);
		return ret;
	}
	for (s16 i = 0; i < ir_bits_; i++) {
		if (!rx_sync_.put((r >> (7 - i)) & 1))
			continue;
		const V110Frame &f = rx_sync_.frame();
		if (state == ClearSt::SYNC && !f.x) {
			set_state(ClearSt::DATA);
			if (rt)
				rt->v110_rate = v110_->user;
			DSP_DBG("clear: V.110 in sync, the other end's X ON\n");
			ret = connect_at(v110_->user, v110_->user);
		}
		if (state != ClearSt::DATA)
			continue;
		u8 user[48];
		adapt_.from_frame(user, f);
		if (f.s)
			std::fill_n(user, v110_->user_bits, 1);
		put_bits(user, v110_->user_bits);
	}
	return ret;
}

/* an octet of V.110 to send: the next bits of our frames (S and X ON
   once in sync, the link's bits once connected, else ones), or 56000's
   7 bits and a one */
u8 ClearPump::v110_tx()
{
	u8 b[8];
	if (v110_->fill == V110Fill::Octet56) {
		std::fill_n(b, 8, 1);
		if (state == ClearSt::DATA)
			modem_get_bits(modem, 1, b, 7);
		return v110_ra2_octet(b, 8);
	}
	if (tx_pos_ >= 80) {
		V110Frame f;
		u8 user[48];
		std::fill_n(user, 48, 1);
		if (state == ClearSt::DATA && rx_sync_.synced())
			modem_get_bits(modem, 1, user, v110_->user_bits);
		adapt_.to_frame(f, user);
		f.s = f.x = rx_sync_.synced() ? 0 : 1;
		v110_frame_bits(tx_bits_, f);
		tx_pos_ = 0;
	}
	u8 o = v110_ra2_octet(tx_bits_ + tx_pos_, ir_bits_);
	tx_pos_ = static_cast<s16>(tx_pos_ + ir_bits_);
	return o;
}

/* an octet at 64 kbit/s: the far end's flags counted, its data (from
   its ones, or what follows its flags) to the link */
s16 ClearPump::clear_rx(u8 r)
{
	if (!heard_) {
		run_ = r == FLAG ? run_ + 1 : 0;
		if (run_ >= HEAR) {
			heard_ = true;
			hunting_ = false;
			DSP_DBG("clear: the far end's flags at %" PRId64
				" ms\n", s64{total} * 1000 / srate);
		}
	} else if (!open_ && r != FLAG) {
		/* its ones, or another device's frames after its flags
		   (with the flag before them for the link's framer) */
		open_ = true;
		if (r != ONES) {
			DSP_DBG("clear: the far end's data after its "
				"flags, no ones\n");
			sink(FLAG);
		}
	}
	if (open_)
		sink(r);
	return DPSTAT_OK;
}

/* an octet at 64 kbit/s to send: flags until we heard the far end's and
   TAIL more, MARK octets of ones, then the link's bits (CONNECT in
   ret) */
u8 ClearPump::clear_tx(s16 &ret)
{
	if (!heard_ || tail_ < TAIL) {
		if (heard_)
			tail_++;
		return FLAG;
	}
	if (mark_ < MARK) {
		mark_++;
		return ONES;
	}
	if (state == ClearSt::SYNC) {
		set_state(ClearSt::DATA);
		ret = connect_at(RATE, RATE);
	}
	return source();
}

/* cnt octets each way: the start, CONNECT, the data; ERROR when the
   far end is not there within S7 */
s16 ClearPump::process(void *in, void *out, s32 cnt)
{
	const s16 *rx = static_cast<const s16 *>(in);
	s16 *tx = static_cast<s16 *>(out);
	s16 ret = DPSTAT_OK;

	if (reported_ && !held_.empty()) {
		modem_put_bits(modem, 1, held_.data(),
			       static_cast<s32>(held_.size()));
		held_.clear();
	}
	if (v110_ && v110_->fill == V110Fill::Octet56 &&
	    state == ClearSt::SYNC) {
		set_state(ClearSt::DATA);
		if (rt)
			rt->v110_rate = v110_->user;
		ret = connect_at(v110_->user, v110_->user);
	}
	for (s32 i = 0; i < cnt; i++) {
		u8 r = g711::encode(alaw_, rx[i]), t;
		s16 st;

		bool found = hunting_ && hunt(r);
		if (v110_) {
			st = found ? DPSTAT_OK : v110_rx(r);
			t = v110_tx();
		} else {
			st = clear_rx(r);
			t = clear_tx(st);
		}
		if (st != DPSTAT_OK)
			ret = st;
		tx[i] = g711::decode(alaw_, t);
	}
	total += cnt;
	count += cnt;
	if (ret == DPSTAT_CONNECT)
		reported_ = true;
	if (state == ClearSt::SYNC && total >= timeout) {
		if (v110_)
			DSP_DBG("clear: no V.110 frame sync\n");
		else
			DSP_DBG("clear: no flags from the far end\n");
		return DPSTAT_ERROR;
	}
	return ret;
}

/* registers the pump as DP_CLEAR */
s16 dp_clear_init(void)
{
	return modem_dp_register(DP_CLEAR, &PumpOps<ClearPump>::ops);
}

/* and no more */
void dp_clear_exit(void)
{
	modem_dp_deregister(DP_CLEAR, &PumpOps<ClearPump>::ops);
}

} // namespace dsp
