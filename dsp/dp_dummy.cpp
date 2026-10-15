/*
 * SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause
 * Copyright (C) 2026 Neo-Desktop
 * (Smart Link's code in it: Smart Link's notice and licence below)
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 *    dp_dummy.cpp  --  dummy data pump (very stupid simulator).
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "pump.h"

namespace dsp {

#define dprintf(fmt...) DSP_DBG(fmt)


#define DUMMY_SKIP     100
#define DUMMY_TIMEOUT  300

#define DUMMY_A1  0xff


struct DummyPump : Pump {
	static constexpr const char *name = "Dummy DataPump driver";

	/* gives the runtime's pcm flag back */
	~DummyPump();

	struct dp_runtime *rt;
	unsigned long count;
	unsigned long linked;
	u8 buf[1024];
	int pcm_was;            /* the runtime's "no DC removal" before */

	int start(struct modem *m, enum DP_ID id, int caller, int srate,
		  int max_frag);
	int process(void *in, void *out, int cnt) override;
};

/* turns the core's DC removal off (the runtime's pcm flag) for as long
   as it runs; 0 */
int DummyPump::start(struct modem *m, enum DP_ID, int, int, int)
{
	dprintf("dummy_create...\n");
	/* its 0/1 bits on the line are not DC free: the core must not
	   take the DC out while it runs */
	rt = (struct dp_runtime *)modem_get_param(m, MDMPRM_DPRUNTIME);
	if (rt) {
		pcm_was = rt->pcm;
		rt->pcm = 1;
	}
	return 0;
}

DummyPump::~DummyPump()
{
	dprintf("dummy_delete...\n");
	if (rt)
		rt->pcm = pcm_was;
}

/* counted in blocks: silent for DUMMY_SKIP blocks, then +-'A' samples
   until the remote's are heard (CONNECT), or DPSTAT_ERROR after
   DUMMY_TIMEOUT; once linked one bit per sample each way, in bit 0 of
   the samples */
int DummyPump::process(void *in, void *out, int cnt)
{
	int n = cnt * (int)sizeof(s16);    /* the core's samples */
	s16 *s;
	int i;
	count++;
	if (!linked) {
		if (count < DUMMY_SKIP) {
			memset(out,0,n);
			return DPSTAT_OK;
		}
		else if(count > DUMMY_TIMEOUT)
			return DPSTAT_ERROR;
		s = static_cast<s16*>(out);
		for(i = 0 ; i < cnt ; i++) {
			*s = (1-2*(i&1))*('A');
			s++;
		}
		s = static_cast<s16*>(in);
		for(i = 0 ; i < cnt ; i++) {
			if(abs(*s) == 'A') {
				linked = 1;
				return DPSTAT_CONNECT;
			}
			s++;
		}
	}
	else {
		s = static_cast<s16*>(in);
		for(i = 0 ; i < cnt ; i++) {
			buf[i] = *s&0x1;
			s++;
		}
		modem_put_bits(dp.modem,1,buf,cnt);
		s = static_cast<s16*>(out);
		modem_get_bits(dp.modem,1,buf,cnt);
		for(i = 0 ; i < cnt ; i++) {
			*s = buf[i]&1 ;
			s++;
		}
	}
	return DPSTAT_OK;
}

} // namespace dsp

using namespace dsp;

/* registers the pump as DP_DUMMY: modem_dp_register()'s result */
int dp_dummy_init(void)
{
	return modem_dp_register(DP_DUMMY, &PumpOps<DummyPump>::ops);
}

/* deregisters it */
void dp_dummy_exit(void)
{
	modem_dp_deregister(DP_DUMMY, &PumpOps<DummyPump>::ops);
}
