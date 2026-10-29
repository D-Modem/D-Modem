/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    modem_homolog.h - the countries the modem can be set up for: their
 *                      T.35 codes (AT+GCI), names (dmodemd's -c) and
 *                      dialling and call progress parameters
 *
 *    The table is in homolog_data.cpp.  The core (dcore.h) keeps one of
 *    its entries as the current country: Modem::homolog_init() selects
 *    it by code or by name, USA first (MODEM_DEFAULT_COUNTRY_CODE).
 *
 */
#ifndef DMODEM_CORE_MODEM_HOMOLOG_H
#define DMODEM_CORE_MODEM_HOMOLOG_H

#include <modem_defs.h>

/* A country's parameters.  Selecting the country copies DialPauseTime
   to S8 and HookFlashTime to S24; the data pumps ask for the members by
   their Get... names (modem_param.h, Modem::get_param(), which answers
   the dial pause from S8).  Of the open pumps only call progress reads
   one (CallingToneFlag): the rest are for Smart Link's DSP library,
   which has its own units for them. */
struct homolog_params {
	/* the S-registers' values for the country */
	u8 DialPauseTime;	/* S8: a ',' in the dial string, s */
	u8 HookFlashTime;	/* S24: 10 ms */
	/* pulse dialling */
	u8 PulseDialMakeTime;
	u8 PulseDialBreakTime;
	u8 PulseDialDigitPattern;
	u8 PulseBetweenDigitsInterval;
	/* DTMF dialling: the high tone's level, and how much lower the low
	   tone's is */
	u8 DTMFHighToneLevel;
	u8 DTMFHighAndLowToneLevelDifference;
	/* the dial string: its modifiers, whether A to D may be dialled, the
	   limit of a ',' */
	u8 DialModifierValidation;
	u8 ABCDDialingPermittedFlag;
	u8 ComaPauseDurationLimit;
	/* > 0: the calling tone while a data call waits for the answer */
	u8 CallingToneFlag;
	/* no pump asks for it */
	u8 TransmitLevel;
	/* dial tone detection */
	u8 DialToneValidationTime;
	u8 DialToneCallProgressFilterIndex;
	u8 DialToneDetectionThreshold;
	/* busy tone detection: the bounds of its cadence, the cycles it
	   takes, its filter */
	u8 MinBusyCadenceOnTime;
	u8 MaxBusyCadenceOnTime;
	u8 MinBusyCadenceOffTime;
	u8 MaxBusyCadenceOffTime;
	u8 BusyDetectionCyclesNumber;
	u8 BusyToneCallProgressFilterIndex;
	/* ringback detection */
	u8 MinRingbackCadenceOnTime;
	u8 MaxRingbackCadenceOnTime;
	u8 RingbackDetectionCyclesNumber;
	/* call progress's analysis of the line's samples */
	u16 CallProgressSamplesBufferLength;
	u8 MustNoiseFilterBeApplied;
};

/* a country of the table */
struct homolog_set {
	const u16 id;		/* its code: T.35's, or above 0xff our own */
	const char *name;	/* in upper case; null ends the table */
	const struct homolog_params *params;	/* never null in a country */
};

/* the countries in the order of their names (AT+GCI=?, --countrylist),
   up to an entry whose name is null */
extern const struct homolog_set homolog_set[];

#endif
