
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
 *    sinus.c  --  sinus data pump.
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

#define SIN_AMPLITUDE 16384
#define SIN_PERIOD    (sizeof(sintab)/sizeof(sintab[0]))
static
const s16 sintab[] = {
 0, 3196, 6269, 9102, 11585, 13622, 15136, 16069,
 16384, 16069, 15136, 13622, 11585, 9102, 6269, 3196,
 0, -3196, -6269, -9102, -11585, -13622, -15136, -16069,
 -16384, -16069, -15136, -13622, -11585, -9102, -6269, -3196};

#define SIN_DBG(fmt,arg...) dprintf("sin: " fmt , ##arg)

struct SinusPump : Pump {
	static constexpr const char *name = "Sinus DP driver";

	~SinusPump() { SIN_DBG("delete...\n"); }

	int      amp;
	unsigned period;
	unsigned phase;

	int start(struct modem *m, enum DP_ID id, int caller, int srate,
		  int max_frag);
	int process(void *in, void *out, int cnt) override;
};

/* the table's sine (srate / 32 Hz, amplitude SIN_AMPLITUDE); 0 */
int SinusPump::start(struct modem *, enum DP_ID, int, int, int)
{
	SIN_DBG("create...\n");
	phase = 0;
	amp   = SIN_AMPLITUDE;
	period= SIN_PERIOD;
	return 0;
}

/* cnt samples of the sine out, the input ignored; always DPSTAT_OK */
int SinusPump::process(void *, void *out, int cnt)
{
	s16 *smpl = static_cast<s16*>(out);
	int i;
	for (i=0;i<cnt;i++) {
		smpl[i]  = sintab[phase%SIN_PERIOD];
		phase = phase+1;
	}
	return DPSTAT_OK;
}

} // namespace dsp

using namespace dsp;

/* registers the pump as DP_SINUS: modem_dp_register()'s result */
int dp_sinus_init(void)
{
	return modem_dp_register(DP_SINUS, &PumpOps<SinusPump>::ops);
}

/* deregisters it */
void dp_sinus_exit(void)
{
	modem_dp_deregister(DP_SINUS, &PumpOps<SinusPump>::ops);
}
