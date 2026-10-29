/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_sinus.cpp - the sinus data pump (DP_SINUS), a test tone
 *
 *    Selected with ATS32=127.  The pump sends a sine of 32 samples a
 *    period, srate/32 Hz (300 Hz at 9600 samples/s, 250 Hz at 8000),
 *    with a peak of 16384 (about -6 dBFS), from a table, the phase going
 *    on from block to block.  It ignores what it receives, never
 *    connects and never fails: the call stays in the handshake until the
 *    DTE or the line ends it.
 *
 */

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"

namespace dsp {

struct SinusPump : Pump {
	static constexpr const char *name = "Sinus";

	u16 phase;              /* the next sample's entry in the table */

	s16 start(struct modem *, enum DP_ID, s16, s32, s32);
	~SinusPump();
	s16 process(void *in, void *out, s32 cnt) override;
};

/* trunc(16384 sin(2 pi k / 32)) for k = 0..8: the first quarter of the
   period */
static const s16 quarter_wave[9] = {
	0, 3196, 6269, 9102, 11585, 13622, 15136, 16069, 16384,
};

/* entry k (0..31) of the table trunc(16384 sin(2 pi k / 32)): the
   quarter wave, mirrored in the second quarter, negated in the second
   half */
static s16 sine32(u16 k)
{
	u16 q = k % 16;
	s16 v = quarter_wave[q <= 8 ? q : 16 - q];

	return k < 16 ? v : static_cast<s16>(-v);
}

/* the tone from phase 0, whatever the arguments; always 0 */
s16 SinusPump::start(struct modem *, enum DP_ID, s16, s32, s32)
{
	DSP_DBG("sinus: start\n");
	return 0;
}

/* nothing to free */
SinusPump::~SinusPump()
{
	DSP_DBG("sinus: end\n");
}

/* cnt samples of the tone, the input unread: DPSTAT_OK */
s16 SinusPump::process(void *, void *out, s32 cnt)
{
	s16 *tx = static_cast<s16 *>(out);

	for (s32 i = 0; i < cnt; i++) {
		tx[i] = sine32(phase);
		phase = (phase + 1) % 32;
	}
	return DPSTAT_OK;
}

} // namespace dsp

/* registers the pump as DP_SINUS: modem_dp_register()'s result */
s16 dp_sinus_init(void)
{
	return modem_dp_register(DP_SINUS, &dsp::PumpOps<dsp::SinusPump>::ops);
}

/* deregisters it */
void dp_sinus_exit(void)
{
	modem_dp_deregister(DP_SINUS, &dsp::PumpOps<dsp::SinusPump>::ops);
}
