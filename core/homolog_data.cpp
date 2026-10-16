/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    homolog_data.cpp - the countries' table (modem_homolog.h)
 *
 *    Several countries have the same parameters; they share one record,
 *    named after one of them.
 *
 */

#include "modem_homolog.h"

/* The records give their members in the order of struct homolog_params,
   on two lines:

	 S8  S24   Mk  Brk  Pat  Gap  DTh  Dif  DMV  A-D  CPL  CTF  TxL
	DTV  DTF  DTT  BN-  BN+  BF-  BF+  BCy  BFi  RN-  RN+  RCy  Buf   NF

   S8 DialPauseTime and S24 HookFlashTime; pulse dialling's make and
   break times (Mk, Brk), digit pattern (Pat) and interval between
   digits (Gap); DTMF's high tone level (DTh) and the low one's
   difference (Dif); DialModifierValidation (DMV),
   ABCDDialingPermittedFlag (A-D), ComaPauseDurationLimit (CPL),
   CallingToneFlag (CTF), TransmitLevel (TxL); dial tone's validation
   time (DTV), filter (DTF) and threshold (DTT); busy tone's least and
   most on (BN-, BN+) and off (BF-, BF+) times, cycles (BCy) and filter
   (BFi); ringback's least and most on times (RN-, RN+) and cycles
   (RCy); CallProgressSamplesBufferLength (Buf) and
   MustNoiseFilterBeApplied (NF). */

static const struct homolog_params australia = {
	  2,   9,  33,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,  35,  39,  35,  39,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params austria = {
	  4,   9,  40,  60,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	  8,   6,  45,   0,   0,   0,   0,   2,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params belgium = {
	  4,  12,  33,  67,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	  7,   4,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params brazil = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0, 255,   0,   0,
	 25,   5,  43,  18,  80,  18,  80,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params brunei = {
	  2,  60,  33,  67,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,  42,  80,  42,  80,   4,   0,   0,   0,   0, 666,   0,
};

/* BULGARIA, LATVIA */
static const struct homolog_params bulgaria = {
	  4,   7,  40,  60,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

/* CHINA, INDONESIA */
static const struct homolog_params china = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

/* CTR21EUROPE (Europe's common CTR 21), ESTONIA, GREECE, ICELAND,
   LUXEMBOURG, TURKEY */
static const struct homolog_params ctr21 = {
	  4,   7,  33,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

/* CYPRUS, JORDAN */
static const struct homolog_params cyprus = {
	  4,   7,  33,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,  42,  80,  42,  80,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params czech_republic = {
	  4,  10,  40,  60,   1,  80,   6,   2,   1,   0,  30,   1,   0,
	 14,   0,  34,   6,  42,  13,  60,   8,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params denmark = {
	  2,   9,  36,  64,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 25,   0,  38,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params egypt = {
	  4,   7,  33,  67,   1,  80,   9,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params finland = {
	  4,   9,  40,  60,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 25,   1,  43,  10,  66,  10,  65,   4,   0,   0,   0,   0, 666,   0,
};

/* ALGERIA, FRANCE, TUNISIA */
static const struct homolog_params france = {
	  2,  27,  34,  67,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 16,   1,  43,  40,  63,  37,  63,   2,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params germany = {
	  4,  18,  41,  60,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 25,   5,  43,  15,  60,  35,  60,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params hong_kong = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   6,   0,   0,   0, 666,   0,
};

static const struct homolog_params hungary = {
	  4,   9,  33,  67,   1,  80,   6,   2,   1,   0,  30,   0,   0,
	  8,   6,  45,  11,  60,  11,  60,   2,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params india = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0,  46,   0,   0,
	 25,   1,  43,  65,  85,  65,  85,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params ireland = {
	  4,   7,  34,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	 25,   2,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params israel = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0, 255,   0,   0,
	 25,   5,  43,  18,  60,  18,  60,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params italy = {
	  2,  11,  41,  65,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 15,   4,  40,   0,   0,   0,   0,   2,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params japan = {
	  2,  50,  34,  66,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params korea = {
	  2,  50,  34,  66,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   6,   0,   0,   0, 666,   0,
};

static const struct homolog_params kuwait = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0, 255,   0,   0,
	 25,   0,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params lebanon = {
	  4,   7,  33,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,  33,  48,  14,  25,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params lithuania = {
	  4,  18,  40,  60,   1,  80,   6,   2,   1,   0,  30,   0,   0,
	 25,   5,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params malaysia = {
	  2,  50,  33,  67,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params malta = {
	  4,   7,  33,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,  33,  60,  33,  60,   4,   0,   0,   0,   0, 666,   0,
};

/* MEXICO, PAKISTAN, PHILIPPINES, VIETNAM */
static const struct homolog_params mexico = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0,  46,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params morocco = {
	  4,   7,  33,  67,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params netherlands = {
	  2,  11,  40,  60,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 15,   7,  32,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params new_zealand = {
	  4,  70,  33,  67,   3,  80,   7,   1,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params norway = {
	  4,  10,  36,  64,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 15,   0,  39,  10,  66,  10,  65,   2,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params oman = {
	  2,   9,  33,  67,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params poland = {
	  4,   9,  33,  67,   1,  80,   6,   2,   1,   0,  30,   0,   0,
	  8,   5,  45,  11,  60,  11,  60,   2,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params portugal = {
	  2,  20,  33,  67,   1,  80,   8,   2,   1,   1,  30,   0,   0,
	 25,   3,  35,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params romania = {
	  4,  18,  40,  60,   1,  80,   7,   2,   1,   0,  30,   0,   0,
	 25,   5,  43,  12,  22,  12,  22,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params russia = {
	  2,   9,  38,  62,   1,  50,   4,   3,   0,   0,  30,   0,   0,
	 25,   1,  43,  34,  46,  34,  46,   4,   0,   0,   0,   0, 666,   0,
};

/* SINGAPORE, SRILANKA */
static const struct homolog_params singapore = {
	  2,   9,  33,  67,   1,  80,   7,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,  72,  78,  72,  78,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params slovakia = {
	  4,  10,  40,  60,   1,  85,   7,   2,   1,   0,  30,   0,   0,
	 25,   5,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params slovenia = {
	  4,  18,  40,  60,   1,  80,   7,   2,   1,   0,  30,   0,   0,
	 25,   5,  43,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params southafrica = {
	  4,   7,  40,  60,   1,  80,   9,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,  42,  80,  42,  80,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params spain = {
	  5,  10,  33,  67,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 12,   5,  41,   5,  60,   5,  60,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params sweden = {
	  4,   9,  40,  60,   2,  80,   8,   2,   1,   0,  30,   0,   0,
	 15,   0,  31,  20,  30,  20,  85,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params switzerland = {
	  4,  12,  40,  60,   1,  80,   8,   2,   1,   0,  30,   0,   0,
	 15,   0,  37,   8,  80,   8,  80,   2,   0,   0,   0,   0, 666,   1,
};

static const struct homolog_params taiwan = {
	  2,  50,  33,  67,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 10,   1,  43,  20,  60,  20,  60,   4,   6,   0,   0,   0, 300,   0,
};

static const struct homolog_params thailand = {
	  2,   9,  40,  60,   1,  80,   6,   2,   0,   0,  30,   0,   0,
	 25,   1,  35,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params uae = {
	  4,   7,  33,  67,   1,  21,   7,   2,   0,   0,  30,   0,   0,
	  2,   3,  45,   0,   0,   0,   0,   4,   0,   0,   0,   0, 666,   0,
};

static const struct homolog_params uk = {
	  4,   7,  34,  72,   1,  80,   8,   2,   0,   0,  30,   0,   0,
	 25,   1,  43,  35,  39,  35,  39,   4,   0,   0,   0,   0, 666,   0,
};

/* USA (the default country), ARGENTINA, BAHREIN, CANADA, CHILE, PERU,
   SAUDIARABIA, SOUTHKOREA, URUGUAY */
static const struct homolog_params usa = {
	  2,  50,  36,  64,   1,  80,   6,   2,   0,   0, 255,   0,   0,
	 25,   5,  43,  18,  60,  18,  60,   4,   6,   0,   0,   0, 666,   0,
};

/* the countries, in the order of their names; the codes up to 0xff are
   T.35's (V.250's +GCI), the others the table's own */
const struct homolog_set homolog_set[] = {
	{ 0x02,  "ALGERIA",         &france },
	{ 0x07,  "ARGENTINA",       &usa },
	{ 0x09,  "AUSTRALIA",       &australia },
	{ 0x0a,  "AUSTRIA",         &austria },
	{ 0x0c,  "BAHREIN",         &usa },
	{ 0x0f,  "BELGIUM",         &belgium },
	{ 0x16,  "BRAZIL",          &brazil },
	{ 0x1a,  "BRUNEI",          &brunei },
	{ 0x1b,  "BULGARIA",        &bulgaria },
	{ 0x20,  "CANADA",          &usa },
	{ 0x25,  "CHILE",           &usa },
	{ 0x26,  "CHINA",           &china },
	{ 0x100, "CTR21EUROPE",     &ctr21 },
	{ 0x2d,  "CYPRUS",          &cyprus },
	{ 0x2e,  "CZECH_REPUBLIC",  &czech_republic },
	{ 0x31,  "DENMARK",         &denmark },
	{ 0x36,  "EGYPT",           &egypt },
	{ 0x101, "ESTONIA",         &ctr21 },
	{ 0x3c,  "FINLAND",         &finland },
	{ 0x3d,  "FRANCE",          &france },
	{ 0x42,  "GERMANY",         &germany },
	{ 0x46,  "GREECE",          &ctr21 },
	{ 0x50,  "HONG_KONG",       &hong_kong },
	{ 0x51,  "HUNGARY",         &hungary },
	{ 0x52,  "ICELAND",         &ctr21 },
	{ 0x53,  "INDIA",           &india },
	{ 0x54,  "INDONESIA",       &china },
	{ 0x57,  "IRELAND",         &ireland },
	{ 0x58,  "ISRAEL",          &israel },
	{ 0x59,  "ITALY",           &italy },
	{ 0x00,  "JAPAN",           &japan },
	{ 0x5e,  "JORDAN",          &cyprus },
	{ 0x61,  "KOREA",           &korea },
	{ 0x62,  "KUWAIT",          &kuwait },
	{ 0x103, "LATVIA",          &bulgaria },
	{ 0x64,  "LEBANON",         &lebanon },
	{ 0x104, "LITHUANIA",       &lithuania },
	{ 0x69,  "LUXEMBOURG",      &ctr21 },
	{ 0x6c,  "MALAYSIA",        &malaysia },
	{ 0x70,  "MALTA",           &malta },
	{ 0x73,  "MEXICO",          &mexico },
	{ 0x77,  "MOROCCO",         &morocco },
	{ 0x7b,  "NETHERLANDS",     &netherlands },
	{ 0x7e,  "NEW_ZEALAND",     &new_zealand },
	{ 0x82,  "NORWAY",          &norway },
	{ 0x83,  "OMAN",            &oman },
	{ 0x84,  "PAKISTAN",        &mexico },
	{ 0x88,  "PERU",            &usa },
	{ 0x89,  "PHILIPPINES",     &mexico },
	{ 0x8a,  "POLAND",          &poland },
	{ 0x8b,  "PORTUGAL",        &portugal },
	{ 0x8e,  "ROMANIA",         &romania },
	{ 0xb8,  "RUSSIA",          &russia },
	{ 0x98,  "SAUDIARABIA",     &usa },
	{ 0x9c,  "SINGAPORE",       &singapore },
	{ 0xc5,  "SLOVAKIA",        &slovakia },
	{ 0xc6,  "SLOVENIA",        &slovenia },
	{ 0x9f,  "SOUTHAFRICA",     &southafrica },
	{ 0x107, "SOUTHKOREA",      &usa },
	{ 0xa0,  "SPAIN",           &spain },
	{ 0xa1,  "SRILANKA",        &singapore },
	{ 0xa5,  "SWEDEN",          &sweden },
	{ 0xa6,  "SWITZERLAND",     &switzerland },
	{ 0x108, "TAIWAN",          &taiwan },
	{ 0xa9,  "THAILAND",        &thailand },
	{ 0xad,  "TUNISIA",         &france },
	{ 0xae,  "TURKEY",          &ctr21 },
	{ 0xb3,  "UAE",             &uae },
	{ 0xb4,  "UK",              &uk },
	{ 0xb7,  "URUGUAY",         &usa },
	{ 0xb5,  "USA",             &usa },
	{ 0xbc,  "VIETNAM",         &mexico },
	{ 0, nullptr, nullptr },
};
