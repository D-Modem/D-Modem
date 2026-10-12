/*
 *    fax_stubs.cpp - the DSP's log, and the modem core the FSK pump in
 *    dp_fsk.cpp links against, for the fax tests without a core
 */

#include <stdarg.h>
#include <stdio.h>

#include "../fax.h"

using namespace dsp;

unsigned int dsplibs_debug_level = 0;

int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

int modem_dp_register(enum DP_ID, struct dp_operations *) { return 0; }
void modem_dp_deregister(enum DP_ID, struct dp_operations *) {}
int modem_get_sreg(struct modem *, unsigned int) { return 0; }
long modem_set_param(struct modem *, unsigned, int) { return 0; }
long modem_get_param(struct modem *, unsigned) { return 0; }
int modem_get_bits(struct modem *, int, u8 *, int n) { return n; }
int modem_put_bits(struct modem *, int, u8 *, int n) { return n; }
/* no V.8 for the FSK pump's low speed caller without a core */
int dsp::dp_v8_first(struct modem *, enum DP_ID) { return 0; }
