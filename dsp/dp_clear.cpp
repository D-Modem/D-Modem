/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_clear.cpp - the clear channel pump (DP_CLEAR, AT+MS=CLEAR or 64):
 *                   a 64 kbit/s digital channel with no modulation, such
 *                   as an ISDN B-channel's unrestricted digital bearer
 *                   (a SIP call in CLEARMODE, RFC 4040, or a PBX's data
 *                   call): each of the line's octets carries 8 of the
 *                   link layer's bits each way, the first the least
 *                   significant, so the rate is 64000 bit/s both ways
 *
 *    The line's samples are its octets through the G.711 law of the
 *    line (dp_runtime law): A-law gives every octet back as it was
 *    (a host with a clear channel takes A-law for it); mu-law has one
 *    sample for both its zeros, so that 0x7F arrives as 0xFF.  The
 *    core's DC removal is off while the pump runs.
 *
 *    The start, D-Modem's own: each end sends HDLC flags (0x7E) until
 *    it has heard HEAR of the other end's in a row, then TAIL more for
 *    the other end to hear, then MARK octets of ones, and then the link
 *    layer's bits, with CONNECT.  What the far end sends from its own
 *    ones on goes to the link layer (whatever came before them, its
 *    flags, is dropped).  No flags within S7 of the start is an ERROR
 *    (NO CARRIER).  On a clear channel nothing tells a carrier from
 *    none: the call ends when the line ends it.
 *
 */

#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "g711.h"
#include "pump.h"

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

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	~ClearPump() override;
	s16 process(void *in, void *out, s32 cnt) override;

private:
	u8 source();
	void sink(u8 v);

	s16 alaw_ = 1;
	s16 saved_pcm_ = 0;
	s32 run_ = 0;                   /* the far end's flags in a row */
	bool heard_ = false;            /* HEAR of them came */
	bool open_ = false;             /* its ones came: data from it */
	bool reported_ = false;         /* CONNECT returned to the core */
	s32 tail_ = 0, mark_ = 0;       /* ours sent */
	std::vector<u8> held_;          /* the far end's data before our
					   CONNECT reached the core */
};

/* the pump at 8000 Hz (a clear channel has no other rate): the core's
   DC removal off; 0, or -1 */
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
	}
	set_state(ClearSt::SYNC);
	stat_dp(DP_CLEAR);
	stat_line(8000, 0, 8000, 0);
	DSP_DBG("clear: start, %s, the octets as %s\n",
		caller_ ? "caller" : "answerer",
		alaw_ ? "A-law" : "mu-law (0x7F arrives as 0xFF)");
	return 0;
}

/* gives the core its DC removal back as it was */
ClearPump::~ClearPump()
{
	if (rt)
		rt->pcm = saved_pcm_;
}

/* an octet of the link layer's bits, the first the least significant
   (ones when it has none) */
u8 ClearPump::source()
{
	u8 b[8];
	std::fill_n(b, 8, 1);
	modem_get_bits(modem, 1, b, 8);
	u8 v = 0;
	for (s16 i = 0; i < 8; i++)
		v = static_cast<u8>(v | (b[i] & 1) << i);
	return v;
}

/* an octet received to the link layer, the first bit the least
   significant */
void ClearPump::sink(u8 v)
{
	u8 b[8];
	for (s16 k = 0; k < 8; k++)
		b[k] = (v >> k) & 1;
	modem_put_bits(modem, 1, b, 8);
}

/* cnt octets each way: the start (flags, then ones), CONNECT as our
   data starts, the far end's data to the link once the core has the
   CONNECT; ERROR when no flags come within S7 */
s16 ClearPump::process(void *in, void *out, s32 cnt)
{
	const s16 *rx = static_cast<const s16 *>(in);
	s16 *tx = static_cast<s16 *>(out);
	s16 ret = DPSTAT_OK;

	if (reported_ && !held_.empty()) {
		for (u8 v : held_)
			sink(v);
		held_.clear();
	}
	for (s32 i = 0; i < cnt; i++) {
		u8 r = g711::encode(alaw_, rx[i]), t;

		if (!heard_) {
			run_ = r == FLAG ? run_ + 1 : 0;
			if (run_ >= HEAR) {
				heard_ = true;
				DSP_DBG("clear: the far end's flags at %"
					PRId64 " ms\n",
					s64{total + i} * 1000 / srate);
			}
		} else if (!open_ && r != FLAG) {
			/* (its ones; anything else between is dropped) */
			open_ = r == ONES;
		}
		if (open_) {
			if (reported_)
				sink(r);
			else
				held_.push_back(r);
		}
		if (!heard_ || tail_ < TAIL) {
			if (heard_)
				tail_++;
			t = FLAG;
		} else if (mark_ < MARK) {
			mark_++;
			t = ONES;
		} else {
			if (state == ClearSt::SYNC) {
				set_state(ClearSt::DATA);
				ret = connect_at(RATE, RATE);
			}
			t = source();
		}
		tx[i] = g711::decode(alaw_, t);
	}
	total += cnt;
	count += cnt;
	if (ret == DPSTAT_CONNECT)
		reported_ = true;
	if (state == ClearSt::SYNC && total >= timeout) {
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
