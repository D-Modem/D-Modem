
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
 *    modem_param.h  --  modem parameters access definitions.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#ifndef __MODEM_PARAM_H__
#define __MODEM_PARAM_H__

#ifdef __cplusplus
extern "C" {
#endif

/* param names */
enum MODEM_PARAMETER_NAMES {
	MDMPRM_NONE,
	MDMPRM_RX_RATE,
	MDMPRM_RATE = MDMPRM_RX_RATE,
	MDMPRM_TX_RATE,
	MDMPRM_MIN_RATE,
	MDMPRM_MAX_RATE,
	MDMPRM_IODELAY,
	MDMPRM_CODECTYPE,
	MDMPRM_DIALSTR,
	MDMPRM_AUTOMODE,
	MDMPRM_DP_REQUESTED,
	MDMPRM_DPRUNTIME,
	MDMPRM_DSPINFO,
	MDMPRM_VOICEINFO,
	MDMPRM_UPDATE_DELAY,
	MDMPRM_DP_ADDR,
	MDMPRM_HOOK_ON,
	MDMPRM_PULSE_DIAL,
	//GetDTMFBetweenDigitsInterval,
	GetPulseDialMakeTime,
	GetPulseDialBreakTime,
	GetPulseDialDigitPattern,
	GetDTMFHighToneLevel,
	GetDTMFDialSpeed,
	//GetCallProgressDetectionThreshold,
	//GetDialToneDelay,
	//GetDialToneHoleTime,
	//GetDelayBeforeRingCountResettingInMcs,
	//GetRingCountForAutoanswer,
	//GetMinRingSignalPeriodInMcs,
	//GetMaxRingSignalPeriodInMcs,
	//GetMinRingCadenceOnTimeInMcs,
	//GetRingCadenceOffTimeInMcs,
	GetMinBusyCadenceOnTime,
	GetMaxBusyCadenceOnTime,
	GetBusyDetectionCyclesNumber,
	GetMinBusyCadenceOffTime,
	GetMaxBusyCadenceOffTime,
	//GetContinuousUnavailableDetectionFlag,
	//GetAnswerToneValidationTime,
	//GetBlacklistingDelayedFlag,
	//GetFailedAttemptsAllowedInFirstStage,
	//GetFailedAttemptsNumberBeforeBlacklisting,
	//GetTroubledCallDelay,
	//GetIneffectiveCallDelay,
	//GetErroneousCallDelay,
	//GetBlackListingDuration,
	//GetLoopCurrentSenseWaitingTime,
	GetCallingToneFlag,
	//GetBlindDialFlag,
	//GetBusyDetectDisable,
	//GetGuardToneType,
	//GetBellModesPermittedFlag,
	//GetOffHookRestrictions,
	//GetShuntRelayOnTimeDuringOffHook,
	GetHookFlashTime,
	GetBlindDialPause,
	GetNoAnswerTimeOut,
	GetDialPauseTime,
	//GetNoCarrierDisconnectTime,
	GetTransmitLevel,
	GetDialModifierValidation,
	//GetTADReceiveGain,
	//GetHalfOrFullWaveRingDetection,
	//GetFailedAttemptsNumberBeforeBlockingFurtherAttempts,
	//GetErroneousCallIncrementCount,
	GetDialToneValidationTime,
	//GetDialToneMinOnTimePeriod,
	GetBusyToneDiffTime,
	//GetBusyToneDetectionDuringDialToneDetectionFlag,
	GetDTMFHighAndLowToneLevelDifference,
	//GetLocalPhoneDetectionInSpeakerphone,
	GetPulseDialingFlag,
	GetDialToneCallProgressFilterIndex,
	GetDialToneDetectionThreshold,
	GetABCDDialingPermittedFlag,
	GetComaPauseDurationLimit,
	//GetTADTransmitLevel,
	//GetDataFAXCarrierReceiveThreshold,
	//GetNoDialToneTimeOutDuration,
	//GetFAXDataAnswerToneDetectionThreshold,
	//GetDialToneLevel,
	//GetFAXCallingToneLevel,
	GetPulseAndToneDialInSameDialStringPermittedFlag,
	//GetRingerImpedanceRelayFlag,
	//GetDCLoopVICharacteristicsRelayFlag,
	//GetDCLoopLimitingRelayFlag,
	//GetRealOrComplexImpedance,
	//GetMinPauseBetweenRingSequencesInMcs,
	//GetMaxPauseBetweenRingSequencesInMcs,
	//GetMinRingCadenceOnTimeFirstInMcs,
	//GetMaxRingCadenceOnTimeFirstInMcs,
	//GetMinRingCadenceOnTimeForFirstSequenceInMcs,
	//GetMaxRingCadenceOnTimeForFirstSequenceInMcs,
	//GetMaxRingCadenceOnTimeInMcs,
	//GetNumberOfSequencesInFirstRing,
	//GetNumberOfSequencesInRegularRing,
	//IsALaw,
	GetBusyToneCallProgressFilterIndex,
	//GetCoefficientTableIndex,
	GetPulseBetweenDigitsInterval,
	GetDialToneWaitTime,
	//GetAnswerDelay,
	//GetPulseRefresh,
	//GetHandsetGain,
	//GetSpecificHardwareBitmask,
	//GetDefaultDTMFDialSpeed,
	//GetDefaultNoAnswerTimeout,
	//GetBlacklistingTimeout,
	//IsATPCommandDisabled,
	//GetDelayAfterCounterpartDisconnection,
	//IsFloatModeForced,
	//GetDataFaxCarrierDisconnectThreshold,
	//GetFaxTransmitLevel,
	//GetPulseDialingRefreshScheme,
	GetMinRingbackCadenceOnTime,
	GetMaxRingbackCadenceOnTime,
	GetRingbackDetectionCyclesNumber,
	GetMinRingbackCadenceOffTime,
	GetMaxRingbackCadenceOffTime,
	GetRingbackToneCallProgressFilterIndex,
	GetMinCongestionCadenceOnTime,
	GetMaxCongestionCadenceOnTime,
	GetCongestionDetectionCyclesNumber,
	GetMinCongestionCadenceOffTime,
	GetMaxCongestionCadenceOffTime,
	GetCongestionToneCallProgressFilterIndex,
	GetCallProgressSamplesBufferLength,
	MustNoiseFilterBeApplied,
	//GetDigitalImpairmentsMask,
	GetAdditAttenToBeepgenVoice,
	GetDialToneFilterSubindex,
	GetBusyToneLooseDetectionEnabled,
	//GetRingFreqValueInHz(int);
	//GetEndUniversalRingTimeoutInMcs,
	//GetRingIntsForFreq,
	//GetDelayBeforeUniversalRingCountResettingInMcs,
	//GetUniversalRingDetectorEnabled, 
	//GetCallWaitingIntegrationTimeMiliSec,
	//GetCallWaitingThresholdPercentage,
	//GetDialStringAfterFlash(char *dialString);
	/* not in the original library's interface */
	MDMPRM_MODE,            /* enum MODEM_MODE: data, fax, voice */
	/* V.92 modem-on-hold (9.10/V.92): the T1 code (Table 33/V.92) we
	   grant the remote, 0 none (+PMH, +PMHT); a request of ours
	   (+PMHR), which the pump takes (sets to 0) */
	MDMPRM_MOH_T1,
	MDMPRM_MOH_REQ,
	/* the call's struct dp_stats (modem_defs.h), which the pumps fill
	   in; -1 (or 0) where there is none */
	MDMPRM_STATS,
	MDMPRM_LAST
};


struct modem;

/* the value of a parameter (enum MODEM_PARAMETER_NAMES), -1 for an
   unknown one; pointers (the dial string, the dp runtime) come cast to
   long */
extern long modem_get_param(struct modem *m, unsigned param);
/* sets a parameter: 0 (the host's result for MDMPRM_HOOK_ON and
   MDMPRM_PULSE_DIAL), or -1 for one that cannot be set */
extern long modem_set_param(struct modem *m, unsigned name, int val);

/* S-register sreg_param, like modem_get_sreg() */
extern long modem_get_sreg_param(struct modem *m, unsigned sreg_param);

#ifdef __cplusplus
}
#endif

#endif /* __MODEM_PARAM_H__ */

