/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pump.h - a data pump as a class
 *
 *    The modem core knows a data pump by its struct dp and the table of
 *    functions it registers (modem_dp.h).  A pump class derives from
 *    Pump and has
 *
 *        static constexpr const char *name    for the table
 *        s16 start(struct modem *m, enum DP_ID id, s16 caller, s32 srate,
 *                  s32 max_frag)               0, or -1 if it cannot run
 *        s16 process(void *in, void *out, s32 cnt)   DPSTAT_*
 *        s16 hangup()                                (optional)
 *
 *    and PumpOps<T>::ops is the table to register.  A pump is created
 *    zeroed (value initialized), as the C pumps were with calloc, so its
 *    plain members need no constructor; its destructor frees what start
 *    took.
 *
 *    PumpBase<P, S> is what the modulation pumps share on top of Pump:
 *    the core, the sample rate and side, a state of S with the samples
 *    counted in it and in all, the handshake's time limit (S7) and the
 *    carrier's (S10), the report of a connection and the hand over to
 *    another pump, the tests' settings from the environment, and the
 *    sample loop of process().
 *
 */

#ifndef __PUMP_H__
#define __PUMP_H__

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <new>

#include "dm_types.h"
#include "dsp.h"
#include "env.h"

namespace dsp {

class Pump {
public:
	/* a derived pump's destructor frees what its start() took */
	virtual ~Pump() {}
	/* cnt s16 samples from the line in, cnt to it out: a DPSTAT_* */
	virtual s16 process(void *in, void *out, s32 cnt) = 0;
	/* the call is being hung up: nothing to do by default */
	virtual s16 hangup() { return 0; }

	struct dp dp;           /* what the modem core holds */
};

template <class T>
struct PumpOps {
	/* dp_operations.create: a zeroed T, its struct dp filled in, then
	   T::start(); NULL when out of memory or start() fails */
	static struct dp *create(struct modem *m, enum DP_ID id, int caller,
				 s32 srate, int max_frag,
				 struct dp_operations *op)
	{
		T *p = new (std::nothrow) T();
		if (!p)
			return nullptr;
		p->dp.id = id;
		p->dp.modem = m;
		p->dp.op = op;
		p->dp.dp_data = static_cast<Pump *>(p);
		if (p->start(m, id, caller, srate, max_frag)) {
			delete p;
			return nullptr;
		}
		return &p->dp;
	}
	/* dp_operations.destroy: deletes the pump (its destructor runs) */
	static int destroy(struct dp *d)
	{
		delete static_cast<Pump *>(d->dp_data);
		return 0;
	}
	/* dp_operations.process: the pump's process() */
	static int process(struct dp *d, void *in, void *out, int cnt)
	{
		return static_cast<Pump *>(d->dp_data)->process(in, out, cnt);
	}
	/* dp_operations.hangup: the pump's hangup() */
	static int hangup(struct dp *d)
	{
		return static_cast<Pump *>(d->dp_data)->hangup();
	}
	static struct dp_operations ops;
};

template <class T>
struct dp_operations PumpOps<T>::ops = {
	T::name, 0, PumpOps<T>::create, PumpOps<T>::destroy,
	PumpOps<T>::process, PumpOps<T>::hangup,
};

/* a test's setting var=<n>...[a|c] (the first a or c in it, if any,
   names the side it is for: a the answerer, c the caller): the setting
   when it is for this side, else NULL */
static inline const char *pump_test_env(const char *var, s16 caller)
{
	const char *e = env(var), *side;
	if (!e)
		return nullptr;
	side = e + strcspn(e, "ac");
	return !*side || *side == (caller ? 'c' : 'a') ? e : nullptr;
}

/* the pump P with its states S.  P has
 *
 *     const char *tag()                its name in the log
 *
 * and, when its process() is pump_loop(),
 *
 *     int sample(float x, float *y)    one sample in, one out (*y, 0 to
 *                                      start with): a DPSTAT_*, the
 *                                      block ending at a CHANGEDP or
 *                                      an ERROR
 *     bool in_data()                   past the handshake: carrier_run
 *                                      is watched, no longer timeout
 *     int block_start()                (optional) a DPSTAT_*, the block
 *                                      silent but for DPSTAT_OK
 */
template <class P, class S>
class PumpBase : public Pump {
public:
	struct modem *modem;
	struct dp_runtime *rt;          /* the core's (NULL: none) */
	struct dp_stats *stats;         /* the call's statistics, the core's
					   (NULL: none) */
	s32 srate;
	s16 caller;
	S state;
	s32 count;                         /* samples in the state */
	s32 total;                         /* samples since the start */
	s32 timeout;                       /* the handshake's end, in total */
	s32 carrier_loss;                  /* samples without carrier that end
					   the call */
	s32 carrier_run;                   /* >0 samples with, <0 without
					   carrier */

	/* nothing before the samples */
	s16 block_start() { return DPSTAT_OK; }

	/* for modem m at srate_ samples/s, calling or not: the runtime,
	   the handshake's time from S7 (50 s if 0) and the carrier's from
	   S10 (1.4 s if 0) */
	void pump_setup(struct modem *m, s32 srate_, s16 caller_)
	{
		s16 s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
		s16 s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);

		modem = m;
		srate = srate_;
		caller = caller_;
		rt = reinterpret_cast<struct dp_runtime *>(
			modem_get_param(m, MDMPRM_DPRUNTIME));
		intptr_t st = modem_get_param(m, MDMPRM_STATS);
		stats = st == -1 ? nullptr :
				   reinterpret_cast<struct dp_stats *>(st);
		timeout = static_cast<long>(s7 > 0 ? s7 : 50) * srate_;
		carrier_loss =
			static_cast<long>(s10 > 0 ? s10 : 14) * srate_ / 10;
	}

	/* the call's statistics (ATI11): a retrain or a rate renegotiation,
	   ours or the remote's; the modulation that came up (when it is not
	   the pump's own: V.8's); the receiver's SNR (dB), the round trip
	   delay (samples) */
	void stat_retrain(bool remote)
	{
		if (stats)
			++*(remote ? &stats->retrains_remote :
			    &stats->retrains_local);
	}
	/* (see stat_retrain()) */
	void stat_reneg(bool remote)
	{
		if (stats)
			++*(remote ? &stats->renegs_remote :
			    &stats->renegs_local);
	}
	/* (see stat_retrain()) */
	void stat_dp(s16 id)
	{
		if (stats)
			stats->dp = id;
	}
	/* (see stat_retrain()) */
	void stat_snr(float db)
	{
		if (!stats)
			return;
		stats->snr = db;
		stats->have |= DP_STATS_SNR;
	}
	/* (see stat_retrain()) */
	void stat_rtd(s32 samples)
	{
		if (!stats || samples <= 0)
			return;
		stats->rtd = static_cast<int>(samples * 1000 / srate);
		stats->have |= DP_STATS_RTD;
	}

	/* to state s, count from 0 */
	void set_state(S s)
	{
		DSP_DBG("%s: state %d -> %d at %" PRId64 " ms\n",
			host().tag(), (s16)state, (s16)s,
			s64{total} * 1000 / srate);
		state = s;
		count = 0;
	}

	/* a sample with or without carrier into carrier_run */
	void carrier_tick(s16 on)
	{
		if (on)
			carrier_run = carrier_run > 0 ? carrier_run + 1 : 1;
		else
			carrier_run = carrier_run < 0 ? carrier_run - 1 : -1;
	}

	/* the data rates to the core */
	void set_rates(s32 tx, s32 rx)
	{
		modem_set_param(modem, MDMPRM_TX_RATE, tx);
		modem_set_param(modem, MDMPRM_RX_RATE, rx);
	}

	/* the data rates to the core: DPSTAT_CONNECT */
	s16 connect_at(s32 tx, s32 rx)
	{
		set_rates(tx, rx);
		DSP_DBG("%s: connect %" PRId32 "/%" PRId32 "\n", host().tag(),
			tx, rx);
		return DPSTAT_CONNECT;
	}

	/* the call to the pump id, the answer tone marked as sent if
	   ans_sent: DPSTAT_CHANGEDP */
	s16 hand_over(enum DP_ID id, s16 ans_sent)
	{
		if (ans_sent && rt)
			rt->ans_sent = 1;
		modem_set_param(modem, MDMPRM_DP_REQUESTED, id);
		return DPSTAT_CHANGEDP;
	}

	/* a test's time var=<ms>[a|c] (pump_test_env) in samples, 0 when
	   not set for this side */
	s32 test_samples(const char *var) const
	{
		const char *e = pump_test_env(var, caller);
		return e ? ms2samples(static_cast<s32>(
				   std::strtol(e, nullptr, 10)), srate) : 0;
	}

	/* process() by the sample: P's block_start(), then sample() for
	   each, the handshake's time limit and the carrier's checked after
	   it (DPSTAT_ERROR); the first other status it gives is returned
	   at the end of the block.  What is not sent of the block is
	   silent */
	s16 pump_loop(void *in, void *out, s32 cnt)
	{
		s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
		s16 ret, r;
		s32 i;

		std::fill_n(dst, cnt, 0);
		ret = host().block_start();
		if (ret != DPSTAT_OK)
			return ret;
		for (i = 0; i < cnt; i++) {
			float y = 0;

			r = host().sample(src[i], &y);
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
			if (r != DPSTAT_OK)
				ret = r;
			if (host().in_data()) {
				if (-carrier_run >= carrier_loss) {
					DSP_DBG("%s: carrier lost\n",
						host().tag());
					return DPSTAT_ERROR;
				}
			} else if (total >= timeout) {
				DSP_DBG("%s: handshake timeout in state %d\n",
					host().tag(), (s16)state);
				return DPSTAT_ERROR;
			}
			dst[i] = dsp_clip16(y);
			count++;
			total++;
		}
		return ret;
	}

private:
	/* the pump this is part of */
	P &host() { return static_cast<P&>(*this); }
	/* (see host()) */
	const P &host() const { return static_cast<const P&>(*this); }
};

} // namespace dsp

#endif /* __PUMP_H__ */
