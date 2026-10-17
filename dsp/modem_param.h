/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    modem_param.h - the parameters a data pump reads from or gives to
 *                    the modem core, by number: rates, the line, the dial
 *                    string, the call's shared objects, the country's
 *                    dialling and call progress settings, modem-on-hold
 *
 *    Smart Link's library passes the numbers 1 to 62 as they are, so the
 *    whole list keeps its order and each value is written out.  The names
 *    from 17 to 62 are the library's own, mixed case and all.  From 63
 *    on the numbers are D-Modem's.
 *
 *    A pointer comes back from modem_get_param() as an intptr_t: the
 *    library's long on i386 and x86_64 Linux, where the two are the same
 *    size, and wide enough on 64 bit Windows, where a long is 32 bits.
 *
 */

#ifndef DSP_MODEM_PARAM_H
#define DSP_MODEM_PARAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modem;

enum MODEM_PARAMETER_NAMES {
	MDMPRM_NONE         = 0,   /* set: nothing */
	MDMPRM_RX_RATE      = 1,   /* set: the receive rate, bit/s */
	MDMPRM_TX_RATE      = 2,   /* set: the transmit rate, bit/s */
	MDMPRM_MIN_RATE     = 3,   /* AT+MS's lowest rate, bit/s */
	MDMPRM_MAX_RATE     = 4,   /* AT+MS's highest rate, bit/s */
	MDMPRM_IODELAY      = 5,   /* the host's delay from its output back
				      to its input, samples */
	MDMPRM_CODECTYPE    = 6,   /* enum codec_types */
	MDMPRM_DIALSTR      = 7,   /* the string to dial (const char *) */
	MDMPRM_AUTOMODE     = 8,   /* 0 */
	MDMPRM_DP_REQUESTED = 9,   /* set: the DP_ID that DPSTAT_CHANGEDP
				      asks for, 0 for the core's choice */
	MDMPRM_DPRUNTIME    = 10,  /* the call's dp_runtime_create() */
	MDMPRM_DSPINFO      = 11,  /* struct dsp_info *, never NULL */
	MDMPRM_VOICEINFO    = 12,  /* struct voice_info * */
	MDMPRM_UPDATE_DELAY = 13,  /* set: a change of the line's delay,
				      samples */
	MDMPRM_DP_ADDR      = 14,  /* the current struct dp * */
	MDMPRM_HOOK_ON      = 15,  /* set: on hook (not 0) or off (0) */
	MDMPRM_PULSE_DIAL   = 16,  /* set: a digit to pulse dial */

	/* the country's dialling and call progress settings, or the
	   S-register that holds one */
	GetPulseDialMakeTime                            = 17,
	GetPulseDialBreakTime                           = 18,
	GetPulseDialDigitPattern                        = 19,
	GetDTMFHighToneLevel                            = 20,
	GetDTMFDialSpeed                                = 21,
	GetMinBusyCadenceOnTime                         = 22,
	GetMaxBusyCadenceOnTime                         = 23,
	GetBusyDetectionCyclesNumber                    = 24,
	GetMinBusyCadenceOffTime                        = 25,
	GetMaxBusyCadenceOffTime                        = 26,
	GetCallingToneFlag                              = 27,
	GetHookFlashTime                                = 28,
	GetBlindDialPause                               = 29,
	GetNoAnswerTimeOut                              = 30,
	GetDialPauseTime                                = 31,
	GetTransmitLevel                                = 32,
	GetDialModifierValidation                       = 33,
	GetDialToneValidationTime                       = 34,
	GetBusyToneDiffTime                             = 35,
	GetDTMFHighAndLowToneLevelDifference            = 36,
	GetPulseDialingFlag                             = 37,
	GetDialToneCallProgressFilterIndex              = 38,
	GetDialToneDetectionThreshold                   = 39,
	GetABCDDialingPermittedFlag                     = 40,
	GetComaPauseDurationLimit                       = 41,
	GetPulseAndToneDialInSameDialStringPermittedFlag = 42,
	GetBusyToneCallProgressFilterIndex              = 43,
	GetPulseBetweenDigitsInterval                   = 44,
	GetDialToneWaitTime                             = 45,
	GetMinRingbackCadenceOnTime                     = 46,
	GetMaxRingbackCadenceOnTime                     = 47,
	GetRingbackDetectionCyclesNumber                = 48,
	GetMinRingbackCadenceOffTime                    = 49,
	GetMaxRingbackCadenceOffTime                    = 50,
	GetRingbackToneCallProgressFilterIndex          = 51,
	GetMinCongestionCadenceOnTime                   = 52,
	GetMaxCongestionCadenceOnTime                   = 53,
	GetCongestionDetectionCyclesNumber              = 54,
	GetMinCongestionCadenceOffTime                  = 55,
	GetMaxCongestionCadenceOffTime                  = 56,
	GetCongestionToneCallProgressFilterIndex        = 57,
	GetCallProgressSamplesBufferLength              = 58,
	MustNoiseFilterBeApplied                        = 59,
	GetAdditAttenToBeepgenVoice                     = 60,
	GetDialToneFilterSubindex                       = 61,
	GetBusyToneLooseDetectionEnabled                = 62,

	/* D-Modem's own */
	MDMPRM_MODE         = 63,  /* the enum MODEM_MODE */
	MDMPRM_MOH_T1       = 64,  /* the modem-on-hold time the DTE allows,
				      as V.92's Table 33 codes it; 0: none */
	MDMPRM_MOH_REQ      = 65,  /* the DTE asks for modem-on-hold (> 0);
				      the pump sets it back to 0 as it
				      takes the request (V.92 9.10) */
	MDMPRM_STATS        = 66,  /* the call's struct dp_stats *; -1 or 0
				      from a core that has none */
	MDMPRM_VCVM         = 67,  /* the struct dp_vcvm *; -1 or 0 from a
				      core that has none */
};

/*
 *    From the core
 */

/* parameter param (a pointer cast to intptr_t for those that are one);
   -1 for one the core does not give */
intptr_t modem_get_param(struct modem *m, unsigned param);
/* sets parameter name to val: 0, or -1 for one that cannot be set
   (MDMPRM_HOOK_ON and MDMPRM_PULSE_DIAL: the host's result) */
long modem_set_param(struct modem *m, unsigned name, int val);
/* S-register sreg_param as modem_get_sreg() gives it */
long modem_get_sreg_param(struct modem *m, unsigned sreg_param);

#ifdef __cplusplus
}
#endif

#endif /* DSP_MODEM_PARAM_H */
