/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_v29fp.cpp - V.29 FastPOS: V.29 (9600 bit/s, or 7200 and 4800)
 *                   half duplex on the switched network, for point of
 *                   sale terminals and their hosts
 *
 *    FastPOS is Hypercom's; no Recommendation describes it, and D-Modem
 *    follows what is published of it (Silicon Labs' AN93, Appendix D):
 *    the calling terminal sends a 980 Hz calling tone (the call
 *    progress pump's, for this +MS), the host answers with a 2225 Hz
 *    answer tone, and from then on each side sends in turn, a burst of
 *    V.29 at a time, each burst its training (as fax's V.29, 10/V.29:
 *    some 250 ms) and then the data.  The DTEs frame their data as HDLC
 *    (SDLC) frames, through V.80's synchronous access mode (AT+ES=6,,8)
 *    or a DTE of their own; plain characters go as well.
 *
 *    The answerer: silence, the answer tone (S50, 400 ms by default),
 *    75 ms of silence, CONNECT.  The caller: CONNECT once its call
 *    progress has heard 300 ms of the answer tone.  Neither sends until
 *    its DTE has data.
 *
 *    The turns: a side whose line is quiet (no carrier for 25 ms) takes
 *    its link's bits at the line's rate and looks at them.  Idle (marks,
 *    or HDLC flags) they are dropped; at the first data (a 0 that is no
 *    part of a flag) the burst starts: the training, then the last 32
 *    bits taken (the data's start and the flag or marks before it), then
 *    the link's bits as they come, until 32 of them in a row are idle.
 *    The receiver is deaf while its own side sends, and starts afresh
 *    after each burst, ours or the far end's.  The far end's data goes to
 *    the link while its carrier lasts, until its shutdown (64 ones in a
 *    row: what is idle in the data is far shorter, the burst ends first).
 *
 *    In data the call stays up with no carrier (half duplex: a quiet line
 *    is no loss of carrier); the DTE ends it.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "dm_types.h"
#include "dsp.h"
#include "fax.h"
#include "pump.h"

namespace dsp {

/* the answer tone by default (S50 0), its frequency; the silence after
   it */
#define FP_ANS_MS       400
#define FP_ANS_HZ       2225
#define FP_GAP_MS       75
/* the bits kept from before a burst's data; idle bits that end a burst;
   received ones in a row that end the far end's (its shutdown) */
#define FP_LEAD_BITS    32
#define FP_END_IDLE     32
#define FP_RX_END_ONES  64
/* the line quiet this long before we send; after our own burst the
   receiver restarts this much later (the echo's tail) */
#define FP_QUIET_MS     25
#define FP_ECHO_MS      30

enum fp_state {
	FP_C_START,             /* caller: its call progress heard the answer
				   tone, CONNECT */
	FP_A_SILENCE,           /* answerer: before the answer tone */
	FP_A_TONE,
	FP_A_GAP,
	FP_DATA,                /* CONNECT reported: turns */
};

/* what a side does in the data */
enum fp_turn {
	FP_LISTEN,              /* the receiver on, the link watched */
	FP_SEND,                /* our burst */
	FP_ECHO,                /* after it, before the receiver restarts */
};

/* the link's bits as they go by: idle (marks or HDLC flags) or data */
struct IdleWatch {
	u32 bits;               /* the last 32, the newest in bit 0 */
	u32 flag_end;           /* bit k: a flag ended k bits ago */
	s32 seen;               /* bits in all, up to 8 */
	s32 idle_run;           /* bits since the last data */

	/* bit in; true when the bit 7 back turns out to be data: a 0
	   that is neither a flag's last bit nor the first of one ending
	   now */
	bool push(s16 bit)
	{
		bits = bits << 1 | static_cast<u32>(bit & 1);
		flag_end <<= 1;
		if ((bits & 0xFF) == 0x7E)
			flag_end |= 1;
		if (seen < 8)
			seen++;
		idle_run++;
		if (seen < 8 || (bits >> 7 & 1) || (flag_end >> 7 & 1) ||
		    (flag_end & 1))
			return false;
		idle_run = 0;
		return true;
	}
};

struct V29fpPump : PumpBase<V29fpPump, fp_state> {
	static constexpr const char *name = "V.29 FastPOS data pump";

	enum fax_mod mod;
	s32 rate;
	s32 ans_ms;
	Osc tone;

	enum fp_turn turn;
	s32 turn_at;            /* total when the turn began */
	FaxTx tx;
	FaxRx rx;
	s16 rx_ready;           /* rx's init() worked */
	PowerMeter level;       /* the line's power (10 ms) ... */
	float floor_pwr;        /* ... its noise floor ... */
	s16 busy;               /* ... and a signal on it */
	s32 quiet;              /* samples without one */
	s16 rx_mute;            /* the far end's burst is over */
	s32 rx_ones;            /* received ones in a row */

	/* the link's bits */
	IdleWatch watch;
	u8 lead[FP_LEAD_BITS];  /* the last bits taken, a ring */
	s16 lead_pos, lead_n;
	s16 lead_out;           /* of them still to send */
	float bit_clock;        /* bits due while we listen */
	s32 bursts_sent, bursts_heard;

	/* its name in the log */
	const char *tag() const { return "V29FP"; }
	/* in data mode (no handshake timeout) */
	bool in_data() const { return state == FP_DATA; }
	~V29fpPump() override
	{
		if (rx_ready)
			rx.release();
	}
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 sample(float x, float *y);
	s16 connect();
	void rx_restart();
	void listen(float x);
	s16 take_bit();
	s16 tx_bit();
	void rx_bit(s16 bit);
};

/* the fax modems' callbacks */
static s16 fp_get_bit(void *ctx)
{
	return static_cast<V29fpPump *>(ctx)->tx_bit();
}
/* (see fp_get_bit()) */
static void fp_put_bit(void *ctx, s16 bit)
{
	static_cast<V29fpPump *>(ctx)->rx_bit(bit);
}

/* a bit from the link, watched and kept with the last FP_LEAD_BITS:
   1 when it shows data (the burst to start), else 0 */
s16 V29fpPump::take_bit()
{
	u8 b = 1;
	modem_get_bits(modem, 1, &b, 1);
	b &= 1;
	lead[lead_pos] = b;
	lead_pos = (lead_pos + 1) % FP_LEAD_BITS;
	if (lead_n < FP_LEAD_BITS)
		lead_n++;
	return watch.push(b) ? 1 : 0;
}

/* the burst's next data bit: those kept from before it, then the
   link's, FAX_BIT_END once FP_END_IDLE of them in a row were idle */
s16 V29fpPump::tx_bit()
{
	if (lead_out > 0) {
		s16 k = (lead_pos - lead_out + FP_LEAD_BITS) % FP_LEAD_BITS;
		lead_out--;
		return lead[k];
	}
	take_bit();
	if (watch.idle_run >= FP_END_IDLE)
		return FAX_BIT_END;
	return lead[(lead_pos + FP_LEAD_BITS - 1) % FP_LEAD_BITS];
}

/* a bit of the far end's burst: to the link, until its shutdown */
void V29fpPump::rx_bit(s16 bit)
{
	u8 b = static_cast<u8>(bit & 1);
	if (rx_mute)
		return;
	rx_ones = b ? rx_ones + 1 : 0;
	if (rx_ones >= FP_RX_END_ONES) {
		rx_mute = 1;
		return;
	}
	modem_put_bits(modem, 1, &b, 1);
}

/* the receiver from scratch, for the next burst */
void V29fpPump::rx_restart()
{
	if (rx_ready)
		rx.release();
	rx_ready = !rx.init(mod, 0, srate, fp_put_bit, this, nullptr);
	rx_mute = 0;
	rx_ones = 0;
	quiet = 0;
}

/* to data: the rates to the core, DPSTAT_CONNECT */
s16 V29fpPump::connect()
{
	set_state(FP_DATA);
	turn = FP_LISTEN;
	turn_at = total;
	rx_restart();
	level.init(0.01f, srate);
	floor_pwr = 1e12f;
	busy = 0;
	if (stats)
		stat_line(2400, 1700, 2400, 1700);
	return connect_at(rate, rate);
}

/* a sample in while we listen: the receiver, and the link's bits while
   the line is quiet (a burst of ours when they show data) */
void V29fpPump::listen(float x)
{
	float p = level.run(x);

	if (rx_ready)
		rx.sample(x);
	/* a signal: well above the noise floor, which follows the quiet
	   line (down at once, up slowly: it doubles in some 0.7 s); from
	   the line's level after 20 ms of the data, whatever it is then */
	if (count < ms2samples(20, srate))
		return;
	if (p < floor_pwr)
		floor_pwr = p;
	else
		floor_pwr += floor_pwr / srate;
	if (!busy && p > tone_power(80) && p > 6 * floor_pwr) {
		busy = 1;
		DSP_DBG("V29FP: signal on at %" PRId64 " ms\n",
			s64{total} * 1000 / srate);
	} else if (busy && (p < tone_power(60) || p < 3 * floor_pwr)) {
		busy = 0;
		DSP_DBG("V29FP: signal off at %" PRId64 " ms\n",
			s64{total} * 1000 / srate);
		if (rx_ready && rx.trained)
			bursts_heard++;
		/* the receiver afresh for the next burst */
		rx_restart();
	}
	quiet = busy ? 0 : quiet + 1;
	if (quiet < ms2samples(FP_QUIET_MS, srate))
		return;
	bit_clock += static_cast<float>(rate) / srate;
	while (bit_clock >= 1) {
		bit_clock -= 1;
		if (!take_bit())
			continue;
		/* data: our burst, the bits before it first */
		lead_out = lead_n;
		watch.idle_run = 0;
		tx.init(mod, 0, srate, fp_get_bit, this);
		turn = FP_SEND;
		turn_at = total;
		DSP_DBG("V29FP: burst %" PRId32 " at %" PRId64 " ms\n",
			bursts_sent + 1, s64{total} * 1000 / srate);
		break;
	}
}

/* a sample x in, *y out: the answerer's tones, then the turns.  In data
   the call has carrier whatever the line does (half duplex) */
s16 V29fpPump::sample(float x, float *y)
{
	s32 ms = static_cast<s32>(s64{count} * 1000 / srate);
	s16 r = DPSTAT_OK;

	*y = 0;
	switch (state) {
	case FP_C_START:
		r = connect();
		break;
	case FP_A_SILENCE:
		if (ms >= ANS_SILENCE_MS)
			set_state(FP_A_TONE);
		break;
	case FP_A_TONE:
		*y = DSP_TX_AMPLITUDE * tone.next();
		if (ms >= ans_ms)
			set_state(FP_A_GAP);
		break;
	case FP_A_GAP:
		if (ms >= FP_GAP_MS)
			r = connect();
		break;
	case FP_DATA:
		carrier_tick(1);
		switch (turn) {
		case FP_LISTEN:
			listen(x);
			break;
		case FP_SEND:
			*y = tx.sample();
			if (tx.done) {
				bursts_sent++;
				DSP_DBG("V29FP: burst %" PRId32 " sent, %" PRId64
					" ms\n", bursts_sent,
					s64{total - turn_at} * 1000 / srate);
				turn = FP_ECHO;
				turn_at = total;
			}
			break;
		case FP_ECHO:
			if (total - turn_at >= ms2samples(FP_ECHO_MS, srate)) {
				rx_restart();
				lead_n = 0;
				bit_clock = 0;
				turn = FP_LISTEN;
				turn_at = total;
			}
			break;
		}
		break;
	}
	return r;
}

/* V.29 FastPOS at +MS's highest rate (9600, 7200 or 4800 bit/s: V.29 has
   no rate signal, both ends are set alike); the answer tone from S50.
   The caller connects at once (its call progress heard the answer
   tone).  0 */
s16 V29fpPump::start(struct modem *m, enum DP_ID id, s16 caller_,
		     s32 srate_, s32 max_frag)
{
	s32 max_rate, v;

	(void)id;
	(void)max_frag;
	pump_setup(m, srate_, caller_);
	max_rate = static_cast<s32>(modem_get_param(m, MDMPRM_MAX_RATE));
	if (max_rate > 0 && max_rate < 7200)
		mod = FM_V29_4800;
	else if (max_rate > 0 && max_rate < 9600)
		mod = FM_V29_7200;
	else
		mod = FM_V29_9600;
	rate = fax_mod_bps(mod);
	v = static_cast<s32>(modem_get_sreg(m, SREG_FAST_ANS_TONE));
	ans_ms = v > 0 ? 10 * v : FP_ANS_MS;
	tone.set_freq(FP_ANS_HZ, srate_);
	state = caller_ ? FP_C_START : FP_A_SILENCE;
	DSP_DBG("V29FP: create %s, %" PRId32 " bit/s, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", rate, srate_);
	return 0;
}

/* registers the pump as DP_V29FP; 0 */
s16 dp_v29fp_init(void)
{
	modem_dp_register(DP_V29FP, &PumpOps<V29fpPump>::ops);
	return 0;
}

/* deregisters it */
void dp_v29fp_exit(void)
{
	modem_dp_deregister(DP_V29FP, &PumpOps<V29fpPump>::ops);
}

} // namespace dsp
