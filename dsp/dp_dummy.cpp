
/*
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

#define dprintf(fmt...) DSP_DBG(fmt)


#define DUMMY_SKIP     100
#define DUMMY_TIMEOUT  300

#define DUMMY_A1  0xff


struct dummy_dp {
	struct dp dp;
	unsigned long count;
	unsigned long linked;
	u8 buf[1024];
	int pcm_was;            /* the runtime's "no DC removal" before */
};

/* dp_operations.create: a zeroed pump; turns the core's DC removal off
   (the runtime's pcm flag) for as long as it runs */
static struct dp *dummy_create(struct modem *m, enum DP_ID id,
			       int caller, int srate, int max_frag,
			       struct dp_operations *op)
{
	struct dummy_dp *d;
	dprintf("dummy_create...\n");
	d = static_cast<dummy_dp*>(malloc(sizeof(*d)));
	if (!d)
		return NULL;
	memset(d,0,sizeof(*d));
	d->dp.id   = id;
	d->dp.modem = m;
	d->dp.op = op;
	d->dp.dp_data = d;
	/* its 0/1 bits on the line are not DC free: the core must not
	   take the DC out while it runs */
	{
		dsp::dp_runtime *rt = (dsp::dp_runtime *)
			modem_get_param(m, MDMPRM_DPRUNTIME);
		if (rt) {
			d->pcm_was = rt->pcm;
			rt->pcm = 1;
		}
	}
	return (struct dp *)d;
}

/* dp_operations.destroy: gives the pcm flag back and frees the pump */
static int dummy_delete(struct dp *dp)
{
	struct dummy_dp *d = (struct dummy_dp *)dp;
	dprintf("dummy_delete...\n");
	{
		dsp::dp_runtime *rt = (dsp::dp_runtime *)
			modem_get_param(dp->modem, MDMPRM_DPRUNTIME);
		if (rt)
			rt->pcm = d->pcm_was;
	}
	free(d);
	return 0;
}

/* dp_operations.process, counted in blocks: silent for DUMMY_SKIP
   blocks, then +-'A' samples until the remote's are heard (CONNECT), or
   DPSTAT_ERROR after DUMMY_TIMEOUT; once linked one bit per sample each
   way, in bit 0 of the samples */
static int dummy_process(struct dp *dp, void *in, void *out, int cnt)
{
	struct dummy_dp *d = (struct dummy_dp *)dp;
	int n = cnt * (int)sizeof(s16);    /* the core's samples */
	s16 *s;
	int i;
	d->count++;
	if (!d->linked) {
		if (d->count < DUMMY_SKIP) {
			memset(out,0,n);
			return DPSTAT_OK;
		}
		else if(d->count > DUMMY_TIMEOUT)
			return DPSTAT_ERROR;
		s = static_cast<s16*>(out);
		for(i = 0 ; i < cnt ; i++) {
			*s = (1-2*(i&1))*('A');
			s++;
		}
		s = static_cast<s16*>(in);
		for(i = 0 ; i < cnt ; i++) {
			if(abs(*s) == 'A') {
				d->linked = 1;
				return DPSTAT_CONNECT;
			}
			s++;
		}
	}
	else {
		s = static_cast<s16*>(in);
		for(i = 0 ; i < cnt ; i++) {
			d->buf[i] = *s&0x1;
			s++;
		}
		modem_put_bits(dp->modem,1,d->buf,cnt);
		s = static_cast<s16*>(out);
		modem_get_bits(dp->modem,1,d->buf,cnt);
		for(i = 0 ; i < cnt ; i++) {
			*s = d->buf[i]&1 ;
			s++;
		}
	}
	return DPSTAT_OK;
}


static struct dp_operations dummy_operations = {
	name : "Dummy DataPump driver",
	create: dummy_create,
	destroy: dummy_delete,
	process:dummy_process
};



/* registers the pump as DP_DUMMY: modem_dp_register()'s result */
int dp_dummy_init(void)
{
	return modem_dp_register(DP_DUMMY,&dummy_operations);
}

/* deregisters it */
void dp_dummy_exit(void)
{
	modem_dp_deregister(DP_DUMMY,&dummy_operations);
}


