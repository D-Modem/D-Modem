/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    interface.cpp - the modem core as the data pumps see it: the C
 *                    functions of modem_dp.h, modem_param.h, modem_defs.h
 *                    and dsplibs.h that the core provides, the table of
 *                    data pumps, and the meaning of each parameter number
 *
 *    The pumps (the open ones in dsp/, Smart Link's i386 library in
 *    half_smartlink) hold a struct modem pointer and call the functions
 *    below on it; each finds the Modem behind it (Modem::of()) and asks
 *    that Modem.  They are called from inside the Modem's own work (a
 *    pump's create() or process()), so none of them blocks or locks, and
 *    none writes to the log: the bits are logged by the Modem, and the
 *    callers would flood the text log.
 *
 *    The binary library also links against the sysdep_ C library shims
 *    here and passes the parameter numbers as plain integers, so those
 *    numbers (modem_param.h) cannot move.
 *
 */

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dsplibs.h>
#include <modem_dp.h>
#include <modem_param.h>

#include "dcore.h"

namespace dcore {

/*
 *    the data pumps
 */

/* Every pump the core can name, in the order AT+MS=? lists those with a
   code; a library fills in op as it registers.  Plain data with constant
   initializers, so it is ready before any static constructor runs (a
   test registers the pumps from one). */
static DpDriver pump_table[] = {
	{ DP_CALLPROG, "CallProg", nullptr, nullptr },
	{ DP_DUMMY, "Dummy", nullptr, nullptr },
	{ DP_AUTOMODE, "Automode", nullptr, nullptr },
	{ DP_V8, "V8", nullptr, nullptr },
	{ DP_V17, "V17", nullptr, nullptr },
	{ DP_V21, "V21", "21", nullptr },
	{ DP_V22, "V22", "22", nullptr },
	{ DP_V23, "V23", "23", nullptr },
	{ DP_V26, "V26", "26", nullptr },
	{ DP_V27TER, "V27ter", "27", nullptr },
	{ DP_V29, "V29", "29", nullptr },
	{ DP_V22BIS, "V22bis", "122", nullptr },
	{ DP_V32, "V32", "32", nullptr },
	{ DP_V32BIS, "V32bis", "132", nullptr },
	{ DP_V32TERBO, "V32terbo", "232", nullptr },
	{ DP_V33, "V33", "33", nullptr },
	{ DP_V34, "V34", "34", nullptr },
	{ DP_B103, "Bell103", "103", nullptr },
	{ DP_V18, "V18", "18", nullptr },
	{ DP_BAUDOT, "Baudot", "45", nullptr },
	{ DP_EDT, "EDT", "110", nullptr },
	{ DP_CONTACTID, "ContactID", "5", nullptr },
	{ DP_SMS, "SMS", "12", nullptr },
	{ DP_B212, "Bell212", "212", nullptr },
	{ DP_B201, "Bell201", "201", nullptr },
	{ DP_B202, "Bell202", "202", nullptr },
	{ DP_B208, "Bell208", "208", nullptr },
	{ DP_FAX, "VFax", nullptr, nullptr },
	{ DP_K56, "K56Flex", "56", nullptr },
	{ DP_V8BIS, "V8bis", nullptr, nullptr },
	{ DP_V90, "V90", "90", nullptr },
	{ DP_V91, "V91", "91", nullptr },
	{ DP_V92, "V92", "92", nullptr },
	{ DP_VCVM, "VCVM", "150", nullptr },
	{ DP_VWB, "VWB", "200", nullptr },
	{ DP_CLEAR, "CLEAR", "64", nullptr },
	{ DP_SINUS, "Sinus", nullptr, nullptr },
	{},	/* the end: id 0 */
};

/* the table of the pumps (dcore.h), up to the entry whose id is 0 */
DpDriver *dp_drivers()
{
	return pump_table;
}

/*
 *    the parameters
 */

/* a pointer as modem_get_param() hands it out */
static intptr_t param_address(const void *p)
{
	return reinterpret_cast<intptr_t>(p);
}

/* parameter name (enum MODEM_PARAMETER_NAMES) as it is now: a rate, the
   host's answer, the address of one of the Modem's objects, an
   S-register, a field of the current country, or a constant; -1 for a
   number that can only be set and for an unknown one */
intptr_t Modem::get_param(u32 name)
{
	const struct homolog_params &c = *homolog_->params;

	switch (name) {
	/* AT+MS's limits */
	case MDMPRM_MIN_RATE:
		return min_rate_;
	case MDMPRM_MAX_RATE:
		return max_rate_;

	/* what the host knows of its line */
	case MDMPRM_IODELAY:
		return host_.io_delay(*this);
	case MDMPRM_CODECTYPE: {
		s16 codec = host_.codec_type(*this);
		return codec < 0 ? static_cast<s16>(CODEC_UNKNOWN) : codec;
	}

	/* the Modem's own objects, which the pumps read (and write) in
	   place; the dial buffer is the live one, which the core changes
	   under the call progress pump */
	case MDMPRM_DIALSTR:
		return param_address(dialing_);
	case MDMPRM_DPRUNTIME:
		return param_address(dp_runtime_);
	case MDMPRM_DSPINFO:
		return param_address(&dsp_info_);
	case MDMPRM_VOICEINFO:
		return param_address(&voice_info_);
	case MDMPRM_DP_ADDR:
		return param_address(dp_);
	case MDMPRM_STATS:
		return param_address(&dp_stats_);
	case MDMPRM_VCVM:
		vcvm_refresh();
		return param_address(&vcvm_);
	case MDMPRM_TONES:
		return tones_ ? param_address(tones_) : 0;

	/* the service class, V.92 modem-on-hold */
	case MDMPRM_MODE:
		return mode_;
	case MDMPRM_ADAPTIVE:
		return adaptive_param();
	case MDMPRM_MOH_T1:
		return moh_off_ ? 0 : moh_timer_;
	case MDMPRM_MOH_REQ:
		return moh_req_;
	/* the DTE's retrain or renegotiation, the highest rate to receive
	   at */
	case MDMPRM_LINK_REQ:
		return link_req_;
	case MDMPRM_TEXT:
		return text_pump() ? param_address(&text_) : 0;
	case MDMPRM_MAX_RX_RATE:
		return max_rx_rate_ ? max_rx_rate_ : max_rate_;

	/* the settings the S-registers hold */
	case GetDTMFDialSpeed:
		return sregs_[SREG_DTMF_DURATION];
	case GetBlindDialPause:
	case GetDialToneWaitTime:
		return sregs_[SREG_DIAL_TONE_WAIT_TIME];
	case GetNoAnswerTimeOut:
		return sregs_[SREG_WAIT_CARRIER_AFTER_DIAL];
	case GetDialPauseTime:
		return sregs_[SREG_DIAL_PAUSE_TIME];
	case GetPulseDialingFlag:
		return sregs_[SREG_TONE_OR_PULSE] == 0;

	/* the current country's, as the table has them */
	case GetPulseDialMakeTime:
		return c.PulseDialMakeTime;
	case GetPulseDialBreakTime:
		return c.PulseDialBreakTime;
	case GetPulseDialDigitPattern:
		return c.PulseDialDigitPattern;
	case GetPulseBetweenDigitsInterval:
		return c.PulseBetweenDigitsInterval;
	case GetDTMFHighToneLevel:
		return c.DTMFHighToneLevel;
	case GetDTMFHighAndLowToneLevelDifference:
		return c.DTMFHighAndLowToneLevelDifference;
	case GetDialModifierValidation:
		return c.DialModifierValidation;
	case GetABCDDialingPermittedFlag:
		return c.ABCDDialingPermittedFlag;
	case GetComaPauseDurationLimit:
		return c.ComaPauseDurationLimit;
	case GetCallingToneFlag:
		return c.CallingToneFlag;
	case GetHookFlashTime:
		return c.HookFlashTime;
	case GetTransmitLevel:
		return c.TransmitLevel;
	case GetDialToneValidationTime:
		return c.DialToneValidationTime;
	case GetDialToneCallProgressFilterIndex:
		return c.DialToneCallProgressFilterIndex;
	case GetDialToneDetectionThreshold:
		return c.DialToneDetectionThreshold;
	case GetMinBusyCadenceOnTime:
		return c.MinBusyCadenceOnTime;
	case GetMaxBusyCadenceOnTime:
		return c.MaxBusyCadenceOnTime;
	case GetMinBusyCadenceOffTime:
		return c.MinBusyCadenceOffTime;
	case GetMaxBusyCadenceOffTime:
		return c.MaxBusyCadenceOffTime;
	case GetBusyDetectionCyclesNumber:
		return c.BusyDetectionCyclesNumber;
	case GetBusyToneCallProgressFilterIndex:
		return c.BusyToneCallProgressFilterIndex;
	case GetMinRingbackCadenceOnTime:
		return c.MinRingbackCadenceOnTime;
	case GetMaxRingbackCadenceOnTime:
		return c.MaxRingbackCadenceOnTime;
	case GetRingbackDetectionCyclesNumber:
		return c.RingbackDetectionCyclesNumber;
	case GetCallProgressSamplesBufferLength:
		return c.CallProgressSamplesBufferLength;
	case MustNoiseFilterBeApplied:
		return c.MustNoiseFilterBeApplied;

	/* no automode of the pumps' own; and what the country table has
	   no data for (Smart Link's call progress asks, and wants 0) */
	case MDMPRM_AUTOMODE:
	case GetBusyToneDiffTime:
	case GetPulseAndToneDialInSameDialStringPermittedFlag:
	case GetMinRingbackCadenceOffTime:
	case GetMaxRingbackCadenceOffTime:
	case GetRingbackToneCallProgressFilterIndex:
	case GetMinCongestionCadenceOnTime:
	case GetMaxCongestionCadenceOnTime:
	case GetCongestionDetectionCyclesNumber:
	case GetMinCongestionCadenceOffTime:
	case GetMaxCongestionCadenceOffTime:
	case GetCongestionToneCallProgressFilterIndex:
	case GetAdditAttenToBeepgenVoice:
	case GetDialToneFilterSubindex:
	case GetBusyToneLooseDetectionEnabled:
		return 0;

	default:
		return -1;
	}
}

/* parameter name set to val by a pump: the rates it runs at, the pump
   to change to, a modem-on-hold request taken, or something for the
   host (the line's delay, the hook, a pulse); 0 (the host's result for
   the hook and the pulse), -1 for a number that cannot be set */
s32 Modem::set_param(u32 name, s32 val)
{
	switch (name) {
	case MDMPRM_NONE:
		return 0;
	case MDMPRM_RX_RATE:
		rx_rate_ = static_cast<u32>(val);
		rate_sets_++;
		return 0;
	case MDMPRM_TX_RATE:
		tx_rate_ = static_cast<u32>(val);
		rate_sets_++;
		return 0;
	case MDMPRM_DP_REQUESTED:
		/* change_dp() takes it when the pump says DPSTAT_CHANGEDP */
		dp_requested_ = static_cast<u32>(val);
		return 0;
	case MDMPRM_UPDATE_DELAY:
		host_.update_delay(*this, val);
		return 0;
	case MDMPRM_MOH_REQ:
		moh_req_ = val;
		return 0;
	case MDMPRM_LINK_REQ:
		/* taken: counted, not to be told the DTE (V.80) */
		if (!val && link_req_ == 1)
			dte_rt_++;
		else if (!val && link_req_ == 2)
			dte_rn_++;
		link_req_ = val;
		return 0;
	case MDMPRM_HOOK_ON:
		/* straight to the host: the Modem's hook state stays */
		return host_.set_hook(*this,
				      val ? MODEM_HOOK_ON : MODEM_HOOK_OFF);
	case MDMPRM_PULSE_DIAL:
		return host_.pulse_dial(*this, val);
	case MDMPRM_PULSE_HOOK:
		return host_.pulse_hook(*this, val != 0);
	case MDMPRM_PULSE_DIALLING:
		host_.pulse_dialling(*this, val != 0);
		return 0;
	default:
		return -1;
	}
}

} // namespace dcore

using dcore::DpDriver;
using dcore::Modem;

extern "C" {

/*
 *    modem_dp.h
 */

/* n units of nbits bits for the pump's transmitter, from the link */
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	return Modem::of(m)->get_bits(nbits, buf, n);
}

/* n units of nbits bits from the pump's receiver, to the link */
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	return Modem::of(m)->put_bits(nbits, buf, n);
}

/* op serves pump id from now on, if the table has id and nobody
   serves it yet: 0, else -1 and nothing changes */
int modem_dp_register(enum DP_ID id, struct dp_operations *op)
{
	for (DpDriver *d = dcore::dp_drivers(); d->id; d++)
		if (d->id == id && !d->op) {
			d->op = op;
			return 0;
		}
	return -1;
}

/* nobody serves pump id any more, whoever registered it */
void modem_dp_deregister(enum DP_ID id, struct dp_operations *)
{
	for (DpDriver *d = dcore::dp_drivers(); d->id; d++)
		if (d->id == id)
			d->op = nullptr;
}

/*
 *    modem_param.h, and the S-registers of modem_defs.h
 */

/* a parameter (Modem::get_param()) */
intptr_t modem_get_param(struct modem *m, unsigned param)
{
	return Modem::of(m)->get_param(param);
}

/* sets a parameter (Modem::set_param()) */
long modem_set_param(struct modem *m, unsigned name, int val)
{
	return Modem::of(m)->set_param(name, val);
}

/* S-register sreg_param, 0 to 255; -1 if there is none */
long modem_get_sreg_param(struct modem *m, unsigned sreg_param)
{
	return Modem::of(m)->sreg(sreg_param);
}

/* S-register num, 0 to 255; -1 if there is none */
int modem_get_sreg(struct modem *m, unsigned int num)
{
	return Modem::of(m)->sreg(num);
}

/* the low 8 bits of val into S-register num: 0, or -1 if there is
   none */
int modem_set_sreg(struct modem *m, unsigned int num, int val)
{
	return Modem::of(m)->set_sreg(num, val);
}

/*
 *    dsplibs.h: the DTE's characters
 */

/* up to n characters to the DTE: how many it took */
int modem_send_to_tty(struct modem *m, const char *buf, int n)
{
	return Modem::of(m)->send_to_dte(buf, n);
}

/* up to n of the DTE's waiting characters: how many (0: none) */
int modem_recv_from_tty(struct modem *m, char *buf, int n)
{
	return Modem::of(m)->recv_from_dte(buf, n);
}

/*
 *    dsplibs.h: the C library under the names the binary library knows;
 *    each does what the function without sysdep_ does
 */

/* malloc() */
void *sysdep_malloc(unsigned int size)
{
	return malloc(size);
}

/* free() */
void sysdep_free(void *mem)
{
	free(mem);
}

/* strlen() */
size_t sysdep_strlen(const char *s)
{
	return strlen(s);
}

/* strcpy() */
char *sysdep_strcpy(char *d, const char *s)
{
	return strcpy(d, s);
}

/* strcat() */
char *sysdep_strcat(char *d, const char *s)
{
	return strcat(d, s);
}

/* strcmp() */
int sysdep_strcmp(const char *s1, const char *s2)
{
	return strcmp(s1, s2);
}

/* strstr(), with C's result that is not const */
char *sysdep_strstr(const char *s1, const char *s2)
{
	return const_cast<char *>(strstr(s1, s2));
}

/* memset() */
void *sysdep_memset(void *d, int c, size_t l)
{
	return memset(d, c, l);
}

/* memcpy() */
void *sysdep_memcpy(void *d, const void *s, size_t l)
{
	return memcpy(d, s, l);
}

/* memchr(), with C's result that is not const */
void *sysdep_memchr(const void *s, int c, size_t l)
{
	return const_cast<void *>(memchr(s, c, l));
}

/* vsnprintf() */
int sysdep_vsnprintf(char *str, unsigned size, const char *format,
		     va_list ap)
{
	return vsnprintf(str, size, format, ap);
}

/* sprintf() for Smart Link's original library (its T.30 frame names):
   the caller sized buf, and gives no size (vsnprintf() without a bound,
   as Apple's SDK has vsprintf() deprecated) */
int sysdep_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int len;
	va_start(ap, fmt);
	len = vsnprintf(buf, INT_MAX, fmt, ap);
	va_end(ap);
	return len;
}

} // extern "C"
