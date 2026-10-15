/*
 *
 *    interface.cpp - what the data pumps (dsp/) call in the modem core:
 *                    modem_dp.h, modem_param.h and dsplibs.h, in C
 *
 *    The pumps get a struct modem from the core; here it leads back to
 *    the Modem.  The parameters are those of slmodemd's modem_param.c.
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsplibs.h>
#include <modem_dp.h>
#include <modem_param.h>

#include "dcore.h"


namespace dcore {

/* the pumps register at the start of the program (prop_dp_init()) */
static DpDriver drivers[] = {
	{ DP_CALLPROG, "CallProg", nullptr, nullptr },
	{ DP_DUMMY, "Dummy", nullptr, nullptr },
	{ DP_AUTOMODE, "Automode", nullptr, nullptr },
	{ DP_V8, "V8", nullptr, nullptr },
	{ DP_V17, "V17", nullptr, nullptr },
	{ DP_V21, "V21", "21", nullptr },
	{ DP_V22, "V22", "22", nullptr },
	{ DP_V23, "V23", "23", nullptr },
	{ DP_V22BIS, "V22bis", "122", nullptr },
	{ DP_V32, "V32", "32", nullptr },
	{ DP_V32BIS, "V32bis", "132", nullptr },
	{ DP_V34, "V34", "34", nullptr },
	{ DP_B103, "Bell103", "103", nullptr },
	{ DP_B212, "Bell212", "212", nullptr },
	{ DP_FAX, "VFax", nullptr, nullptr },
	{ DP_K56, "K56Flex", "56", nullptr },
	{ DP_V8BIS, "V8bis", nullptr, nullptr },
	{ DP_V90, "V90", "90", nullptr },
	{ DP_V91, "V91", "91", nullptr },
	{ DP_V92, "V92", "92", nullptr },
	{ DP_SINUS, "Sinus", nullptr, nullptr },
	{ (enum DP_ID)0, nullptr, nullptr, nullptr },
};

/* the pumps' table (AT+MS, change_dp()) */
DpDriver *dp_drivers()
{
	return drivers;
}

/* modem_get_param(): what the pumps ask for: the rates, the line's
   delay and codec, the dial string, the runtime, the mode, modem-on-
   hold, the country's dial and call progress parameters; -1 if
   unknown */
long Modem::get_param(unsigned name)
{
	const struct homolog_params *h = homolog_->params;
	int val;
	switch (name) {
	case MDMPRM_NONE:
		break;
	case MDMPRM_MIN_RATE:
		return min_rate_;
	case MDMPRM_MAX_RATE:
		return max_rate_;
	case MDMPRM_IODELAY:
		/* (Smart Link's original library asks, not the open data
		   pumps) */
		return host_.io_delay(*this);
	case MDMPRM_CODECTYPE:
		val = host_.codec_type(*this);
		return val < 0 ? CODEC_UNKNOWN : val;
	case MDMPRM_DIALSTR:
		return (long)dial_string_;
	case MDMPRM_AUTOMODE:
		return 0;
	case MDMPRM_DPRUNTIME:
		return (long)dp_runtime_;
	case MDMPRM_MODE:
		return mode_;
	case MDMPRM_MOH_T1:
		return moh_off_ ? 0 : moh_timer_;
	case MDMPRM_MOH_REQ:
		return moh_req_;
	case MDMPRM_DSPINFO:
		return (long)&dsp_info_;
	case MDMPRM_VOICEINFO:
		return (long)&voice_info_;
	case MDMPRM_DP_ADDR:
		return (long)dp_;
	case GetPulseDialMakeTime:
		return h->PulseDialMakeTime;
	case GetPulseDialBreakTime:
		return h->PulseDialBreakTime;
	case GetPulseDialDigitPattern:
		return h->PulseDialDigitPattern;
	case GetDTMFHighToneLevel:
		return h->DTMFHighToneLevel;
	case GetDTMFDialSpeed:
		return sregs_[SREG_DTMF_DURATION];
	case GetMinBusyCadenceOnTime:
		return h->MinBusyCadenceOnTime;
	case GetMaxBusyCadenceOnTime:
		return h->MaxBusyCadenceOnTime;
	case GetBusyDetectionCyclesNumber:
		return h->BusyDetectionCyclesNumber;
	case GetMinBusyCadenceOffTime:
		return h->MinBusyCadenceOffTime;
	case GetMaxBusyCadenceOffTime:
		return h->MaxBusyCadenceOffTime;
	case GetCallingToneFlag:
		return h->CallingToneFlag;
	case GetHookFlashTime:
		return h->HookFlashTime;
	case GetBlindDialPause:
		return sregs_[SREG_DIAL_TONE_WAIT_TIME];
	case GetNoAnswerTimeOut:
		return sregs_[SREG_WAIT_CARRIER_AFTER_DIAL];
	case GetDialPauseTime:
		return sregs_[SREG_DIAL_PAUSE_TIME];
	case GetTransmitLevel:
		return h->TransmitLevel;
	case GetDialModifierValidation:
		return h->DialModifierValidation;
	case GetDialToneValidationTime:
		return h->DialToneValidationTime;
	case GetBusyToneDiffTime:
		return 0;
	case GetDTMFHighAndLowToneLevelDifference:
		return h->DTMFHighAndLowToneLevelDifference;
	case GetPulseDialingFlag:
		return !sregs_[SREG_TONE_OR_PULSE];
	case GetDialToneCallProgressFilterIndex:
		return h->DialToneCallProgressFilterIndex;
	case GetDialToneDetectionThreshold:
		return h->DialToneDetectionThreshold;
	case GetABCDDialingPermittedFlag:
		return h->ABCDDialingPermittedFlag;
	case GetComaPauseDurationLimit:
		return h->ComaPauseDurationLimit;
	case GetPulseAndToneDialInSameDialStringPermittedFlag:
		return 0;
	case GetBusyToneCallProgressFilterIndex:
		return h->BusyToneCallProgressFilterIndex;
	case GetPulseBetweenDigitsInterval:
		return h->PulseBetweenDigitsInterval;
	case GetDialToneWaitTime:
		return sregs_[SREG_DIAL_TONE_WAIT_TIME];
	case GetMinRingbackCadenceOnTime:
		return h->MinRingbackCadenceOnTime;
	case GetMaxRingbackCadenceOnTime:
		return h->MaxRingbackCadenceOnTime;
	case GetRingbackDetectionCyclesNumber:
		return h->RingbackDetectionCyclesNumber;
	case GetMinRingbackCadenceOffTime:
	case GetMaxRingbackCadenceOffTime:
	case GetRingbackToneCallProgressFilterIndex:
	case GetMinCongestionCadenceOnTime:
	case GetMaxCongestionCadenceOnTime:
	case GetCongestionDetectionCyclesNumber:
	case GetMinCongestionCadenceOffTime:
	case GetMaxCongestionCadenceOffTime:
	case GetCongestionToneCallProgressFilterIndex:
		return 0;
	case GetCallProgressSamplesBufferLength:
		return h->CallProgressSamplesBufferLength;
	case MustNoiseFilterBeApplied:
		return h->MustNoiseFilterBeApplied;
	case GetAdditAttenToBeepgenVoice:
	case GetDialToneFilterSubindex:
	case GetBusyToneLooseDetectionEnabled:
		return 0;
	}
	return -1;
}

/* modem_set_param(): what the pumps tell (the rates, the pump to change
   to, the line's delay, modem-on-hold) or ask for (the hook, a pulse
   dial digit); -1 if unknown */
long Modem::set_param(unsigned name, int val)
{
	switch (name) {
	case MDMPRM_NONE:
		break;
	case MDMPRM_RX_RATE:
		rx_rate_ = val;
		break;
	case MDMPRM_TX_RATE:
		tx_rate_ = val;
		break;
	case MDMPRM_DP_REQUESTED:
		dp_requested_ = val;
		break;
	case MDMPRM_UPDATE_DELAY:
		/* (as MDMPRM_IODELAY) */
		host_.update_delay(*this, val);
		break;
	case MDMPRM_MOH_REQ:
		moh_req_ = val;
		break;
	case MDMPRM_HOOK_ON:
		return host_.set_hook(*this, val ? MODEM_HOOK_ON :
				      MODEM_HOOK_OFF);
	case MDMPRM_PULSE_DIAL:
		return host_.pulse_dial(*this, val);
	default:
		return -1;
	}
	return 0;
}

} // namespace dcore

using dcore::Modem;

/*
 *    modem_dp.h
 */

/* the pump's transmitter: bits from the link */
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	return Modem::of(m)->get_bits(nbits, buf, n);
}

/* the pump's receiver: bits to the link */
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	return Modem::of(m)->put_bits(nbits, buf, n);
}

/* a pump registers its operations for id; -1 if id is unknown or taken */
int modem_dp_register(enum DP_ID id, struct dp_operations *op)
{
	for (dcore::DpDriver *p = dcore::drivers; p->id > 0; p++)
		if (p->id == id && !p->op) {
			p->op = op;
			return 0;
		}
	return -1;
}

/* a pump is gone */
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op)
{
	(void)op;
	for (dcore::DpDriver *p = dcore::drivers; p->id > 0; p++)
		if (p->id == id)
			p->op = nullptr;
}

/*
 *    modem_param.h, modem_defs.h
 */

/* Modem::get_param() */
long modem_get_param(struct modem *m, unsigned name)
{
	return Modem::of(m)->get_param(name);
}

/* Modem::set_param() */
long modem_set_param(struct modem *m, unsigned name, int val)
{
	return Modem::of(m)->set_param(name, val);
}

/* an S-register, -1 if there is none */
long modem_get_sreg_param(struct modem *m, unsigned num)
{
	return Modem::of(m)->sreg(num);
}

/* an S-register, -1 if there is none */
int modem_get_sreg(struct modem *m, unsigned int num)
{
	return Modem::of(m)->sreg(num);
}

/* sets an S-register; -1 if there is none */
int modem_set_sreg(struct modem *m, unsigned int num, int val)
{
	return Modem::of(m)->set_sreg(num, val);
}

/*
 *    dsplibs.h
 */

/* characters to the DTE; how many it took */
int modem_send_to_tty(struct modem *m, const char *buf, int n)
{
	return Modem::of(m)->send_to_dte(buf, n);
}

/* the DTE's characters; how many */
int modem_recv_from_tty(struct modem *m, char *buf, int n)
{
	return Modem::of(m)->recv_from_dte(buf, n);
}

/* the C library, for the pumps */
void *sysdep_malloc(unsigned int size) { return malloc(size); }
void sysdep_free(void *mem) { free(mem); }
size_t sysdep_strlen(const char *s) { return strlen(s); }
char *sysdep_strcpy(char *d, const char *s) { return strcpy(d, s); }
char *sysdep_strcat(char *d, const char *s) { return strcat(d, s); }
int sysdep_strcmp(const char *a, const char *b) { return strcmp(a, b); }
/* strstr(), its result not const as in C */
char *sysdep_strstr(const char *a, const char *b)
{
	return const_cast<char *>(strstr(a, b));
}
void *sysdep_memset(void *d, int c, size_t l) { return memset(d, c, l); }
/* memcpy() */
void *sysdep_memcpy(void *d, const void *s, size_t l)
{
	return memcpy(d, s, l);
}
/* memchr(), its result not const as in C */
void *sysdep_memchr(const void *s, int c, size_t l)
{
	return const_cast<void *>(memchr(s, c, l));
}
/* vsnprintf() */
int sysdep_vsnprintf(char *str, unsigned size, const char *fmt, va_list ap)
{
	return vsnprintf(str, size, fmt, ap);
}
/* sprintf() (unbounded: the callers size buf) */
int sysdep_sprintf(char *buf, const char *fmt, ...)
{
	va_list args;
	int i;
	va_start(args, fmt);
	i = vsprintf(buf, fmt, args);
	va_end(args);
	return i;
}
