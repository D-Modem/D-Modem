/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dp_dummy.cpp - the dummy data pump (DP_DUMMY), a link simulator
 *               without a modulation
 *
 *    Selected with ATS32=3, on either side of a call.  The pump is
 *    silent for its first 99 blocks (process() calls), then sends a tone
 *    of samples alternately +65 and -65 and listens for a sample of
 *    exactly that size from the other end.  The first one it hears
 *    links it (DPSTAT_CONNECT); none by block 300 is an error.  Once
 *    linked, every sample carries one data bit each way, its least
 *    significant one, so the bit rate is the sample rate.  Two dummies
 *    find each other only on a path that keeps every sample exact: no
 *    G.711, no gain, filtering or noise.  While the pump runs, the core's
 *    DC removal is off, for the received samples to stay as they were
 *    sent.
 *
 */

#include <cstring>

#include "dsp.h"
#include "pump.h"

namespace dsp {

struct DummyPump : Pump {
	static constexpr const char *name = "Dummy";

	/* blocks 1 to SILENT_BLOCKS are silent, the tone runs up to
	   LAST_TONE_BLOCK, and the next block without a link fails */
	static constexpr long SILENT_BLOCKS = 99;
	static constexpr long LAST_TONE_BLOCK = 300;
	/* the tone's level (the code of 'A'), +TONE first in each block */
	static constexpr s16 TONE = 65;
	/* the bits handed to or taken from the core at a time */
	static constexpr int BIT_CHUNK = 64;

	CoreBits core;
	struct dp_runtime *rt;  /* the core's (NULL: none) */
	int saved_pcm;          /* rt->pcm as it was before the pump */
	long blocks;            /* process() calls before the link */
	bool linked;            /* the other end was heard: data */

	int start(struct modem *m, enum DP_ID, int, int, int);
	~DummyPump();
	int process(void *in, void *out, int cnt) override;

private:
	int handshake(const s16 *rx, s16 *tx, int cnt);
	void exchange(const s16 *rx, s16 *tx, int cnt);
};

/* the pump for modem m (the id, the side, the sample rate and the
   block size make no difference): the core's DC removal turned off
   while it runs; always 0 */
int DummyPump::start(struct modem *m, enum DP_ID, int, int, int)
{
	core.modem = m;
	rt = reinterpret_cast<struct dp_runtime *>(
		modem_get_param(m, MDMPRM_DPRUNTIME));
	if (rt) {
		saved_pcm = rt->pcm;
		rt->pcm = 1;
	}
	DSP_DBG("dummy: start\n");
	return 0;
}

/* gives the core its DC removal back as it was */
DummyPump::~DummyPump()
{
	if (rt)
		rt->pcm = saved_pcm;
	DSP_DBG("dummy: end\n");
}

/* one block: the start until the other end is heard, data after it */
int DummyPump::process(void *in, void *out, int cnt)
{
	const s16 *rx = static_cast<const s16 *>(in);
	s16 *tx = static_cast<s16 *>(out);

	if (!linked)
		return handshake(rx, tx, cnt);
	exchange(rx, tx, cnt);
	return DPSTAT_OK;
}

/* a block before the link: silence up to block SILENT_BLOCKS; then
   the tone, and DPSTAT_CONNECT as soon as a received sample is +TONE or
   -TONE (the rest of the block's input unread); after LAST_TONE_BLOCK,
   silence and DPSTAT_ERROR */
int DummyPump::handshake(const s16 *rx, s16 *tx, int cnt)
{
	int i;

	if (++blocks <= SILENT_BLOCKS) {
		std::fill_n(tx, cnt, 0);
		return DPSTAT_OK;
	}
	if (blocks > LAST_TONE_BLOCK) {
		std::fill_n(tx, cnt, 0);
		DSP_DBG("dummy: no tone from the other end\n");
		return DPSTAT_ERROR;
	}
	for (i = 0; i < cnt; i++)
		tx[i] = i & 1 ? -TONE : TONE;
	for (i = 0; i < cnt; i++)
		if (rx[i] == TONE || rx[i] == -TONE) {
			linked = true;
			DSP_DBG("dummy: linked\n");
			return DPSTAT_CONNECT;
		}
	return DPSTAT_OK;
}

/* a block of data: the low bit of each received sample to the core,
   all of them first, then as many bits from the core, each one sent as
   a sample of 0 or 1 */
void DummyPump::exchange(const s16 *rx, s16 *tx, int cnt)
{
	u8 bits[BIT_CHUNK];
	int i, j, n;

	for (i = 0; i < cnt; i += n) {
		n = cnt - i < BIT_CHUNK ? cnt - i : BIT_CHUNK;
		for (j = 0; j < n; j++)
			bits[j] = static_cast<u16>(rx[i + j]) & 1;
		core.put(bits, n);
	}
	for (i = 0; i < cnt; i += n) {
		n = cnt - i < BIT_CHUNK ? cnt - i : BIT_CHUNK;
		core.get(bits, n);
		for (j = 0; j < n; j++)
			tx[i + j] = bits[j] & 1;
	}
}

} // namespace dsp

/* registers the pump as DP_DUMMY: modem_dp_register()'s result */
int dp_dummy_init(void)
{
	return modem_dp_register(DP_DUMMY, &dsp::PumpOps<dsp::DummyPump>::ops);
}

/* deregisters it */
void dp_dummy_exit(void)
{
	modem_dp_deregister(DP_DUMMY, &dsp::PumpOps<dsp::DummyPump>::ops);
}
