/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    smartlink_glue.cpp - Smart Link's original library
 *                         (smartlink/dsplibs.o) on the modem core, for
 *                         half_smartlink: half_open.cpp with the library
 *                         in place of the open data pumps
 *
 *    The core has the library's interface (../../core/interface.cpp,
 *    the one the open data pumps keep); here is the rest of what the
 *    library needs, and the tests' handles on it.  The program is
 *    linked with --wrap=dp_runtime_create and --wrap=modem_dp_register,
 *    so that those calls come here first:
 *
 *    SMARTLINK_V92FULL=1   the library is a "V.92 Lite" build: its
 *                          runtime has the full V.92 bit (byte 2, bit 7)
 *                          clear, and the library then retrains to V.34
 *                          upstream when PCM upstream is selected.  This
 *                          sets the bit in each call's runtime.
 *    SMARTLINK_FPE_AT=<s>  its own fast parameter exchange (9.9/V.92) as
 *                          the analogue modem, that many seconds into
 *                          data mode: nothing in it starts one
 *                          (V92Modulator::initiateFPE is only called on
 *                          the digital modem's Rf), so its V.92 pump
 *                          calls it then, through its VPcmFloModem
 *                          (vpcm_create keeps it at 0x3574, it its
 *                          V92Modulator at 0x6124).
 *
 */

#include <stdlib.h>

#include <dcore.h>
#include <dsplibs.h>
#include <log.h>
#include <modem_dp.h>

extern "C" {

/* the library's records for the binary log (dmodem.log): the core's */
int modem_debug_log_data(struct modem *m, unsigned id, const void *data,
			 int count)
{
	dcore::log_data(m, id, data, count);
	return count;
}

/* the network's G.711 law: the library has no digital modem to tell */
void dp_runtime_set_law(void *runtime, int law)
{
	(void)runtime;
	(void)law;
}

/* its fax has no T.38: it stays on the audio */
int FAX_t38_start(void *obj, void (*tx)(void *ctx, const void *ifp),
		  void *ctx, int max_data)
{
	(void)obj;
	(void)tx;
	(void)ctx;
	(void)max_data;
	return -1;
}

/* no T.38 to stop */
void FAX_t38_stop(void *obj)
{
	(void)obj;
}

/* T.38 received: ignored */
void FAX_t38_rx(void *obj, const void *ifp)
{
	(void)obj;
	(void)ifp;
}

/* its caller ID has only the line's receiver: a caller ID message from
   elsewhere (a SIP call's caller) is not reported */
int CID_report(struct modem *m, unsigned cid_val, int packet,
	       const void *msg, int len)
{
	(void)m;
	(void)cid_val;
	(void)packet;
	(void)msg;
	(void)len;
	return -1;
}

/* its voice engine tells of no call progress tones: none heard */
void VOICE_tones(void *obj, struct voice_tones *t)
{
	(void)obj;
	*t = voice_tones();
}

/* the library's own, and the member function that starts an FPE (it
   takes its object as its first argument) */
void *__real_dp_runtime_create(struct modem *m);
int __real_modem_dp_register(enum DP_ID id, struct dp_operations *op);
void _ZN12V92Modulator11initiateFPEEv(void *mod);

/* a call's runtime, with SMARTLINK_V92FULL the full V.92 bit set */
void *__wrap_dp_runtime_create(struct modem *m)
{
	void *rt = __real_dp_runtime_create(m);
	const char *v = getenv("SMARTLINK_V92FULL");
	if (rt && v && atoi(v))
		((unsigned char *)rt)[2] |= 0x80;
	return rt;
}

}

/* the library's V.92 pump, and its process() */
static struct dp_operations v92_ops;
static int (*v92_process)(struct dp *, void *, void *, int);

/* SMARTLINK_FPE_AT: the samples of data mode so far, and when to start
   the FPE (-1: not, or done) */
static long fpe_at = -2, fpe_n;

/* the V.92 pump on cnt samples, after an FPE if it is time for one */
static int v92_fpe_process(struct dp *dp, void *in, void *out, int cnt)
{
	dcore::Modem *m = dp->modem->self;

	if (fpe_at == -2)
		fpe_at = getenv("SMARTLINK_FPE_AT") ?
			atol(getenv("SMARTLINK_FPE_AT")) * (long)m->rate() :
			-1;
	if (fpe_at >= 0 && m->link_state() == dcore::LinkState::Online &&
	    (fpe_n += cnt) >= fpe_at) {
		void *flo = *(void **)((char *)dp + 0x3574);
		void *mod = flo ? *(void **)((char *)flo + 0x6124) : NULL;
		fpe_at = -1;
		dcore::log_printf(1, "%s: SMARTLINK_FPE_AT: initiateFPE (%p)\n",
				  m->name(), mod);
		if (mod)
			_ZN12V92Modulator11initiateFPEEv(mod);
	}
	return v92_process(dp, in, out, cnt);
}

/* the library's pumps registered, its V.92 one through
   v92_fpe_process() */
extern "C" int __wrap_modem_dp_register(enum DP_ID id,
					struct dp_operations *op)
{
	if (id == DP_V92 && op) {
		v92_ops = *op;
		v92_process = op->process;
		v92_ops.process = v92_fpe_process;
		op = &v92_ops;
	}
	return __real_modem_dp_register(id, op);
}
