/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_4wire.cpp - the 4-wire leased line's data pumps: ITU V.29 (9600,
 *                   7200 and 4800 bit/s), V.33 (14400 and 12000 bit/s)
 *                   and V.27ter (4800 and 2400 bit/s), V.26 and Bell
 *                   201 (2400 bit/s, unscrambled) and Bell 208 (V.27ter's
 *                   4800 bit/s signal), each duplex with a continuous
 *                   carrier
 *
 *    On a 4-wire circuit each way has its pair, so both modems send in
 *    the same band at once and there is no echo to cancel.  The
 *    modulations are the fax modems' (fax_modems.cpp): the transmitter
 *    sends its synchronizing signal (the training) and then the data
 *    for as long as the call lasts; the receiver finds the far end's
 *    synchronizing signal, trains on it and takes the data.
 *
 *    The transmitter's rate is +MS's highest (AT+MS=29,0,4800,7200: V.29
 *    at 7200).  V.29 and V.27ter have no rate signal (their rate is set
 *    by hand at both ends, 2.2/V.29); the receiver here tries each rate
 *    of the family at once and keeps the one that fits the training.
 *    V.33 sends its rate in segment 3 (8.3/V.33): the receiver tells
 *    the rate from it, and a modem sending faster than the far end asks
 *    for comes down to it (9/V.33).
 *
 *    Synchronizing (10/V.29, 9/V.33): each modem starts with a
 *    synchronizing signal of its own; one that has not had the far
 *    end's 1.2 s after its own ended sends another.  In data a receiver
 *    that loses its equalization (its error too large for a while)
 *    sends a synchronizing signal and expects one back; one that finds
 *    the far end's (its alternations or unmodulated carrier in the
 *    data, or the carrier gone and back) when it had not sent one
 *    returns one.  The DTE's data waits while the transmitter
 *    synchronizes, and the received data is held at ones (circuit 104
 *    clamped) while the receiver does.  A receiver that loses its
 *    equalization twice in 20 s at its own rate sends the next rate down
 *    (not below +MS's lowest), and one whose far end sends slower than
 *    it does comes down to that rate, which for V.33 is its rule (9/V.33)
 *    and for V.29 and V.27ter D-Modem's own.
 *
 */

#include <cinttypes>
#include <cmath>
#include <algorithm>
#include <cstring>

#include "dm_types.h"
#include "dsp.h"
#include "fax.h"
#include "pump.h"

namespace dsp {

/* V.33's rate signal (Table 4A/V.33), B0 first: B7, B11 and B15 for
   the synchronization, B8 and B9 the rates the modem can take */
#define V33_RATE_14400 0x8B80
#define V33_RATE_12000 0x8980

/* a synchronizing signal again this long after ours, the far end's not
   had (10/V.29, 9/V.33) */
#define RESYNC_MS 1200
/* the far end's synchronizing signal in the data: its alternations or
   unmodulated carrier, symbols in a row */
#define SYNC_RUN 32
/* the receivers of each family, at most */
#define MAX_RX 3

enum fw_state {
	FW_START,               /* the first synchronizing signals */
	FW_DATA,                /* CONNECT reported */
};

struct FourWirePump;

/* a receiver's data bits to the pump, with which of them it is */
struct FwRxCtx {
	FourWirePump *pump;
	s16 index;
};

struct FourWirePump : PumpBase<FourWirePump, fw_state> {
	static constexpr const char *name =
		"4-wire leased (V.29/V.33/V.27ter/V.26/Bell) data pump";

	const char *family;             /* in the log */
	enum fax_mod rx_mods[MAX_RX];   /* the family's rates, fastest first */
	s16 nrx;
	enum fax_mod tx_mod;
	FaxTx tx;
	FaxRx rx[MAX_RX];
	FwRxCtx rx_ctx[MAX_RX];
	s16 rx_ready[MAX_RX];           /* its init() worked */
	s16 active;                     /* the receiver chosen, -1: none yet */
	s32 decide_at;                  /* when to choose (total), 0: no
					   receiver has trained */
	s32 rx_sym0;                    /* the active one's symbols when it
					   was chosen */
	s32 bad_syms;                   /* data samples in a row with too
					   large an error */
	s32 sync_end;                   /* when ours ended (total), -1 while
					   it goes out */
	s16 initiated;                  /* we sent ours unasked, and the
					   far end's answer has not come */
	s16 searching;                  /* the receivers look for the far
					   end's (none trained since) ... */
	s32 search_since;               /* ... since (total) */
	s16 respond;                    /* the far end's found: send ours
					   back once it is through */
	s32 min_rate;                   /* +MS's lowest: no fallback below */
	s16 drops;                      /* losses of equalization at our
					   own rate ... */
	s32 drop_at;                    /* ... the first of them (total) */
	s16 connected;
	s32 syncs_sent;
	float mse_limit;                /* the active rate's loss of
					   equalization */
	s32 report_at;                  /* the next statistics (total) */

	/* its name in the log */
	const char *tag() const { return family; }
	/* in data mode (the carrier watched, no handshake timeout) */
	bool in_data() const { return state == FW_DATA; }
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 sample(float x, float *y);
	/* the line's samples (PumpBase) */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}
	/* (the receivers freed) */
	~FourWirePump() override
	{
		for (s16 k = 0; k < nrx; k++)
			if (rx_ready[k])
				rx[k].release();
	}

	void send_sync(bool ours);
	s16 rx_reset();
	bool choose();
	void put_bit(s16 k, s16 bit);
	s16 get_bit();
	float limit_of(enum fax_mod mod) const;
	bool slower(enum fax_mod to);
	/* a modulation's rate, bit/s */
	s32 rate_of(enum fax_mod mod) const { return fax_mod_bps(mod); }
	/* the receiver chosen, or none yet */
	const FaxRx *rx_active() const
	{
		return active >= 0 ? &rx[active] : nullptr;
	}
};

/* the fax modems' callbacks */
static s16 fw_get_bit(void *ctx)
{
	return static_cast<FourWirePump *>(ctx)->get_bit();
}
/* (see fw_get_bit()) */
static void fw_put_bit(void *ctx, s16 bit)
{
	FwRxCtx *c = static_cast<FwRxCtx *>(ctx);
	c->pump->put_bit(c->index, bit);
}

/* the next bit to send: the DTE's once connected, else a one */
s16 FourWirePump::get_bit()
{
	u8 b = 1;
	if (connected)
		modem_get_bits(modem, 1, &b, 1);
	return b & 1;
}

/* a data bit from receiver k: to the DTE from the chosen one once
   connected (the others' go nowhere) */
void FourWirePump::put_bit(s16 k, s16 bit)
{
	if (k != active || !connected)
		return;
	u8 b = static_cast<u8>(bit & 1);
	modem_put_bits(modem, 1, &b, 1);
}

/* the data mse (of the constellation's power) past which the receiver
   has lost its equalization: where the simulated line of the tests
   (leased_call_test) begins to give bit errors, white noise alone */
float FourWirePump::limit_of(enum fax_mod mod) const
{
	switch (mod) {
	case FM_V17_14400: return 0.0045f;
	case FM_V17_12000: return 0.010f;
	case FM_V29_9600:  return 0.008f;
	case FM_V29_7200:  return 0.03f;
	case FM_V29_4800:  return 0.06f;
	case FM_V27_4800:  return 0.024f;
	case FM_V26_A:
	case FM_V26_B:     return 0.018f;
	default:           return 0.018f;  /* V.27ter 2400 */
	}
}

/* our transmitter down to the rate to (the family's, below ours, not
   below +MS's lowest): whether it was */
bool FourWirePump::slower(enum fax_mod to)
{
	if (rate_of(to) >= rate_of(tx_mod) || rate_of(to) < min_rate)
		return false;
	DSP_DBG("%s: down to %" PRId32 " bit/s\n", family, rate_of(to));
	tx_mod = to;
	drops = 0;
	return true;
}

/* our synchronizing signal (the training, then the data path); ours
   (unasked: the far end's is the answer) or an answer to the far
   end's */
void FourWirePump::send_sync(bool ours)
{
	tx.init(tx_mod, 0, srate, fw_get_bit, this);
	if (dp.id == DP_V33)
		tx.bridge = tx_mod == FM_V17_14400 ? V33_RATE_14400 :
						     V33_RATE_12000;
	sync_end = -1;
	initiated = ours;
	syncs_sent++;
	DSP_DBG("%s: synchronizing signal %" PRId32 " at %" PRId32
		" bit/s (%s) at %" PRId64 " ms\n", family, syncs_sent,
		rate_of(tx_mod), ours ? "ours" : "the answer",
		s64{total} * 1000 / srate);
}

/* the receivers back to the search for the far end's synchronizing
   signal (afresh: a signal that goes on is a carrier to them after
   their first 40 ms, a new onset).  0, or -1 when out of memory */
s16 FourWirePump::rx_reset()
{
	for (s16 k = 0; k < nrx; k++) {
		if (rx_ready[k])
			rx[k].release();
		rx_ready[k] = 0;
		rx_ctx[k] = { this, k };
		if (rx[k].init(rx_mods[k], 0, srate, fw_put_bit, &rx_ctx[k],
			       nullptr))
			return -1;
		rx_ready[k] = 1;
		if (dp.id == DP_V33 &&
		    rx[k].set_bridge(rx_mods[k] == FM_V17_14400 ?
					     V33_RATE_14400 : V33_RATE_12000,
				     1))
			return -1;
	}
	active = -1;
	decide_at = 0;
	bad_syms = 0;
	searching = 1;
	search_since = total;
	return 0;
}

/* the receiver that fits the far end's training best, of those through
   it: the least error over V.33's rate signal, else over the training.
   Whether the far end's signal came unasked (not an answer to ours) */
bool FourWirePump::choose()
{
	bool unasked = !initiated;
	float best = 0;
	s16 k, pick = -1;
	for (k = 0; k < nrx; k++) {
		const FaxRx &r = rx[k];
		if (r.stage != FaxRx::RXS_DATA)
			continue;
		float e = dp.id == DP_V33 ?
			r.bridge_err / (r.bridge_n ? r.bridge_n : 1) :
			r.trn_err / (r.trn_n ? r.trn_n : 1);
		DSP_DBG("%s: receiver at %" PRId32 ": training error %.4f\n",
			family, rate_of(rx_mods[k]), e);
		if (pick < 0 || e < best) {
			best = e;
			pick = k;
		}
	}
	active = pick;
	decide_at = 0;
	bad_syms = 0;
	searching = 0;
	rx_sym0 = rx[pick].symbols;
	mse_limit = limit_of(rx_mods[pick]);
	initiated = 0;
	DSP_DBG("%s: the far end's at %" PRId32 " bit/s%s\n", family,
		rate_of(rx_mods[pick]), unasked ? "" : ", the answer");
	return unasked;
}

/* V.29, V.33 or V.27ter by id: the transmitter at +MS's highest rate
   of the family (MDMPRM_MAX_RATE), the family's receivers; caller or
   answerer alike (a leased line's ends are the same).  0, or -1 */
s16 FourWirePump::start(struct modem *m, enum DP_ID id, s16 caller_,
			s32 srate_, s32 max_frag)
{
	s32 max = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));

	pump_setup(m, srate_, caller_);
	if (max <= 0)
		max = 33600;
	min_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MIN_RATE));
	switch (id) {
	case DP_V29:
		family = "V29";
		rx_mods[0] = FM_V29_9600;
		rx_mods[1] = FM_V29_7200;
		rx_mods[2] = FM_V29_4800;
		nrx = 3;
		tx_mod = max >= 9600 ? FM_V29_9600 :
			 max >= 7200 ? FM_V29_7200 : FM_V29_4800;
		break;
	case DP_V33:
		family = "V33";
		rx_mods[0] = FM_V17_14400;
		rx_mods[1] = FM_V17_12000;
		nrx = 2;
		tx_mod = max >= 14400 ? FM_V17_14400 : FM_V17_12000;
		break;
	case DP_V27TER:
		family = "V27ter";
		rx_mods[0] = FM_V27_4800;
		rx_mods[1] = FM_V27_2400;
		nrx = 2;
		tx_mod = max >= 4800 ? FM_V27_4800 : FM_V27_2400;
		break;
	case DP_B208:
		/* 8-phase at 1600 baud, scrambled: V.27ter's 4800 (V.27
		   came from it), at that rate alone */
		family = "Bell208";
		rx_mods[0] = tx_mod = FM_V27_4800;
		nrx = 1;
		break;
	case DP_V26:
	case DP_B201:
		/* 2400 bit/s alone, the data unscrambled */
		family = id == DP_V26 ? "V26" : "Bell201";
		rx_mods[0] = tx_mod = id == DP_V26 ? FM_V26_A : FM_V26_B;
		nrx = 1;
		break;
	default:
		return -1;
	}
	if (srate_ < 7200)
		return -1;
	if (rx_reset())
		return -1;
	state = FW_START;
	send_sync(true);
	DSP_DBG("%s: create %s, %" PRId32 " bit/s, srate %" PRId32 "\n",
		family, caller_ ? "caller" : "answerer", rate_of(tx_mod),
		srate_);
	return 0;
}

/* a sample x in, *y out: the transmitter, the receivers, then the
   synchronizing (see the top).  DPSTAT_CONNECT once both ways are up
   (the time limit, S7, and the loss of carrier in data, S10, are
   pump_loop()'s) */
s16 FourWirePump::sample(float x, float *y)
{
	s16 ret = DPSTAT_OK, k, carrier = 0;

	*y = tx.sample();
	if (sync_end < 0 && tx.in_data()) {
		sync_end = total;
		DSP_DBG("%s: our synchronizing signal sent at %" PRId64
			" ms\n", family, s64{total} * 1000 / srate);
	}

	/* the receivers: all of them while they search, the chosen one
	   after */
	for (k = 0; k < nrx; k++) {
		if (active >= 0 && k != active)
			continue;
		rx[k].sample(x);
		carrier |= rx[k].carrier;
		if (active < 0 && !decide_at &&
		    rx[k].stage == FaxRx::RXS_DATA)
			/* the others through the same training a little
			   after (10 ms) */
			decide_at = total + srate / 100;
	}
	carrier_tick(carrier);
	if (active < 0 && decide_at && total >= decide_at) {
		bool unasked = choose();
		if (slower(rx_mods[active])) {
			/* the far end asks for less than we send (V.33's rate
			   signal, 9/V.33; V.29 and V.27ter: it sends less) */
			respond = 0;
			send_sync(true);
		} else if (unasked && sync_end >= 0) {
			/* the far end's signal, unasked: one of ours back
			   (when ours is not just going out) */
			respond = 1;
		}
		if (connected)
			set_rates(rate_of(tx_mod), rate_of(rx_mods[active]));
	}
	if (respond && sync_end >= 0) {
		respond = 0;
		send_sync(false);
	}

	/* the far end's synchronizing signal in our data, or its carrier
	   gone (and maybe back): the receivers search again */
	const FaxRx *r = rx_active();
	if (r && r->stage == FaxRx::RXS_DATA) {
		/* (V.26's data is not scrambled: its marks are
		   alternations, its zeros an unmodulated carrier) */
		bool plain = dp.id == DP_V26 || dp.id == DP_B201;
		bool again = !r->carrier || (!plain &&
			(r->alt_run >= SYNC_RUN || r->const_run >= SYNC_RUN));
		if (again) {
			DSP_DBG("%s: the far end synchronizes (%s) at %" PRId64
				" ms\n", family, !r->carrier ? "carrier off" :
				r->alt_run >= SYNC_RUN ? "alternations" :
				"unmodulated carrier",
				s64{total} * 1000 / srate);
			if (connected)
				stat_retrain(true);
			/* (answered once it is found, unless it is the
			   answer to ours) */
			if (rx_reset())
				return DPSTAT_ERROR;
		}
	}

	/* our receiver's loss of equalization: ours, and the far end's
	   expected back */
	r = rx_active();
	if (r && r->stage == FaxRx::RXS_DATA && r->trained) {
		if (r->mse > mse_limit)
			bad_syms++;
		else
			bad_syms = 0;
		/* (for 0.3 s) */
		if (bad_syms > srate * 3 / 10 && sync_end >= 0) {
			DSP_DBG("%s: equalization lost (mse %.4f) at %" PRId64
				" ms\n", family, r->mse,
				s64{total} * 1000 / srate);
			if (connected)
				stat_retrain(false);
			if (rx_mods[active] == tx_mod) {
				/* twice in 20 s at our own rate: the next one
				   down (the far end comes down to it) */
				if (!drops || total - drop_at > 20 * srate) {
					drops = 1;
					drop_at = total;
				} else if (++drops >= 2 && active + 1 < nrx) {
					slower(rx_mods[active + 1]);
				}
			}
			if (rx_reset())
				return DPSTAT_ERROR;
			send_sync(true);
		}
	}

	/* ours again when the far end's has not come 1.2 s after it, or
	   1.2 s into a search that began without ours (the carrier gone
	   and back: whichever end has the search) */
	if (sync_end >= 0 && searching && !decide_at &&
	    total - std::max(sync_end, search_since) >=
		    ms2samples(RESYNC_MS, srate)) {
		bool training = false;
		for (k = 0; k < nrx; k++)
			training |= rx[k].stage == FaxRx::RXS_TRAIN;
		if (!training)
			send_sync(true);
	}

	/* both ways up: CONNECT (once; a resynchronization later keeps the
	   call) */
	r = rx_active();
	if (!connected && r && r->trained && sync_end >= 0) {
		connected = 1;
		set_state(FW_DATA);
		report_at = total + srate;
		ret = connect_at(rate_of(tx_mod), rate_of(rx_mods[active]));
	}
	if (connected && r && total >= report_at) {
		report_at = total + srate;
		if (r->trained && r->carrier)
			stat_snr(-10 * log10f(r->mse + 1e-6f));
	}
	return ret;
}

/* registers the pump as DP_V29, DP_V33, DP_V27TER, DP_V26, DP_B201 and
   DP_B208; 0 */
s16 dp_4wire_init(void)
{
	modem_dp_register(DP_V29, &PumpOps<FourWirePump>::ops);
	modem_dp_register(DP_V33, &PumpOps<FourWirePump>::ops);
	modem_dp_register(DP_V27TER, &PumpOps<FourWirePump>::ops);
	modem_dp_register(DP_V26, &PumpOps<FourWirePump>::ops);
	modem_dp_register(DP_B201, &PumpOps<FourWirePump>::ops);
	modem_dp_register(DP_B208, &PumpOps<FourWirePump>::ops);
	return 0;
}

/* deregisters them */
void dp_4wire_exit(void)
{
	modem_dp_deregister(DP_V29, &PumpOps<FourWirePump>::ops);
	modem_dp_deregister(DP_V33, &PumpOps<FourWirePump>::ops);
	modem_dp_deregister(DP_V27TER, &PumpOps<FourWirePump>::ops);
	modem_dp_deregister(DP_V26, &PumpOps<FourWirePump>::ops);
	modem_dp_deregister(DP_B201, &PumpOps<FourWirePump>::ops);
	modem_dp_deregister(DP_B208, &PumpOps<FourWirePump>::ops);
}

} // namespace dsp
