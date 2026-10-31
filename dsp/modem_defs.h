/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    modem_defs.h - the types and numbers the modem core and a data pump
 *                   library share: fixed width integers, the service
 *                   classes, the hook states, the S-registers, the data
 *                   pump ids and results, the call's statistics, what is
 *                   learnt about the line for V.92 quick connect, and the
 *                   fax and voice engines' commands and results
 *
 *    A data pump library sees the modem core only as a struct modem
 *    pointer; each core defines the structure itself.  Most numbers here
 *    are seen from outside: by the DTE (S-registers, AT+MS, AT+FCLASS),
 *    by the core's test transcripts, and by Smart Link's i386 library
 *    (dsp/test/smartlink/dsplibs.o), which was compiled against them.
 *    They are therefore all written out, and struct dsp_info and struct
 *    voice_info keep the layout the library reads and writes.
 *
 *    C linkage; the header is valid C as well as C++.
 *
 */

#ifndef DSP_MODEM_DEFS_H
#define DSP_MODEM_DEFS_H

#include <stdint.h>
#include <sys/types.h>

#include "dm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the modem core, opaque to the data pumps */
struct modem;

/* the service class, numbered as AT+FCLASS sets it (V.250; T.31 8.2;
   V.253) */
enum MODEM_MODE {
	MODEM_MODE_DATA  = 0,
	MODEM_MODE_FAX   = 1,   /* service class 1 */
	MODEM_MODE_VOICE = 8,
};

/* what the core asks of the host's hook */
#define MODEM_HOOK_ON           0
#define MODEM_HOOK_OFF          1
#define MODEM_HOOK_SNOOPING     2   /* off hook to listen only: caller ID */

/* the host's codec, as MDMPRM_CODECTYPE reports it.  Smart Link's
   library knows some codec chips by other numbers; no host here has
   one */
enum codec_types {
	CODEC_UNKNOWN = 0,
};

/*
 *    S-registers: the numbers of the bytes of the core's 256 byte
 *    register file, as ATSn, AT&V and modem_get_sreg() know them.  The
 *    library reads S7, S16, S56 and S221 (the last one unnamed) by number.
 */
enum S_REGISTER {
	SREG_RINGS_TO_AUTO_ANSWER         = 0,   /* V.250 6.3.8 */
	SREG_RING_COUNTER                 = 1,   /* rings of this call */
	SREG_ESCAPE_CHAR                  = 2,   /* of the +++ escape */
	SREG_CR_CHAR                      = 3,   /* V.250 6.2.1 */
	SREG_LF_CHAR                      = 4,   /* V.250 6.2.2 */
	SREG_BS_CHAR                      = 5,   /* V.250 6.2.3 */
	SREG_DIAL_TONE_WAIT_TIME          = 6,   /* s, V.250 6.3.9 */
	SREG_WAIT_CARRIER_AFTER_DIAL      = 7,   /* s, V.250 6.3.10 */
	SREG_DIAL_PAUSE_TIME              = 8,   /* s for a ',', V.250 6.3.11 */
	SREG_CARRIER_DETECT_RESPONSE_TIME = 9,   /* 0.1 s */
	SREG_CARRIER_LOSS_DISCONNECT_TIME = 10,  /* 0.1 s, V.250 6.3.12 */
	SREG_DTMF_DURATION                = 11,  /* ms */
	SREG_ESCAPE_PROMPT_DELAY          = 12,  /* guard time of +++ */
	SREG_ECHO                         = 13,  /* ATE */
	SREG_QUIET                        = 14,  /* ATQ */
	SREG_VERBOSE                      = 15,  /* ATV */
	SREG_TONE_OR_PULSE                = 16,  /* 1 tone (ATT), 0 pulse (ATP) */
	SREG_TEST_TIMER                   = 18,  /* s of an AT&T test, 0 none */
	SREG_FLASH_TIMER                  = 24,  /* hook flash, 10 ms */
	SREG_PULSE_RATIO                  = 28,  /* AT&P */
	SREG_SPEAKER_CONTROL              = 29,  /* ATM */
	SREG_SPEAKER_VOLUME               = 30,  /* ATL */
	SREG_AUTOMODE                     = 31,  /* ATN, AT+MS's automode */
	SREG_DP                           = 32,  /* AT+MS's modulation, a DP_ID */
	SREG_LINE_QUALITY_CONTROL         = 39,  /* AT%E */
	SREG_X_CODE                       = 56,  /* ATX: the result code set */
	SREG_CD                           = 60,  /* AT&C */
	SREG_FLOW_CONTROL                 = 62,  /* AT&H */
	SREG_CONNNECT_MSG_FORMAT          = 70,  /* AT&A */
	SREG_CONNNECT_MSG_SPEED_SRC       = 71,  /* AT&E */
	SREG_ANS_DELAY                    = 92,  /* s from ATA to the answering
						    pump */
	SREG_EC                           = 103, /* AT\N: error correction */
	SREG_COMP                         = 104, /* AT%C: compression */
	SREG_MNP_BLOCK                    = 105, /* AT\A: MNP block size */
	SREG_VCVM_TONES                   = 44,  /* V.cvm: the last call's
						    tones received, read
						    only (12.3/V.cvm) */
	SREG_VCVM_RENEGS                  = 45,  /* and its
						    renegotiations */
	SREG_CONNECT_PROTOCOL             = 106, /* AT\V */
	SREG_LEASED                       = 47,  /* AT&L: 1 a 2-wire leased
						    line, 2 a 4-wire one (no
						    dial tone or ringing: ATD
						    and ATA start the call at
						    once, again after NO
						    CARRIER) */
	SREG_SIM_CARRIER                  = 48,  /* 1: V.13's simulated
						    carrier control */
	SREG_DEFAULT_SETTING              = 161, /* ATY */
};

/*
 *    Data pump ids.  The DTE sees them (AT+MS=<id>, AT+MS?, the &A
 *    modulation report as 1000 + id); SREG_DP keeps one in a byte, so all
 *    are below 256; the core's driver table ends with id 0; and Smart
 *    Link's library registers its pumps under these numbers.
 */
enum DP_ID {
	DP_CALLPROG = 2,        /* call progress: dialling, tones */
	DP_DUMMY    = 3,        /* a test pump */
	DP_AUTOMODE = 4,
	DP_V8       = 8,        /* V.8, the start of V.34 and above */
	DP_V17      = 17,
	DP_V21      = 21,
	DP_V27TER   = 27,       /* V.27ter on a 4-wire leased line, 4800 and
				   2400 bit/s duplex */
	DP_V29      = 29,       /* V.29 on a 4-wire leased line, 9600 to
				   4800 bit/s duplex */
	DP_V33      = 33,       /* V.33 on a 4-wire leased line, 14400 and
				   12000 bit/s duplex */
	DP_V22      = 22,
	DP_V23      = 23,
	DP_V32      = 32,
	DP_V34      = 34,
	DP_K56      = 56,       /* K56flex: the core runs V.90 for it */
	DP_CLEAR    = 64,       /* a clear 64 kbit/s digital channel (an
				   ISDN B-channel's data call): no
				   modulation, 8 bits an octet */
	DP_V90      = 90,
	DP_V91      = 91,
	DP_V92      = 92,
	DP_FAX      = 100,
	DP_B103     = 103,      /* Bell 103 */
	DP_V18      = 18,       /* V.18 text telephone automoding: the
				   remote's mode found (or probed for),
				   then 5-bit (Baudot) or EDT here, or
				   V.21, V.23 or Bell 103 by their pumps */
	DP_BAUDOT   = 45,       /* 5-bit (Baudot) text telephone: TTY,
				   telex, RTTY (dp_text.cpp, its tones
				   and rate from struct dp_text) */
	DP_EDT      = 110,      /* EDT: V.21's channel 1 at 110 bit/s,
				   half duplex, 7 bits even parity */
	DP_V8BIS    = 108,
	DP_V22BIS   = 122,
	DP_SINUS    = 127,      /* a test pump */
	DP_VCVM     = 150,      /* V.cvm, data over a mobile speech path
				   (docs/v_cvm.pdf), after V.8 */
	DP_V32BIS   = 132,
	DP_VWB      = 200,      /* V.wb: wideband DMT on a bare pair, at
				   48000 Hz or more (dsp/vwb.h) */
	DP_B212     = 212,      /* Bell 212A */
};

/*
 *    What a pump's process() returns, numbered as V.250's numeric result
 *    codes (Table 1/V.250)
 */
#define DPSTAT_OK               0   /* nothing to report */
#define DPSTAT_CONNECT          1   /* the data connection is up, at the
				       rates set with MDMPRM_TX_RATE and
				       MDMPRM_RX_RATE */
#define DPSTAT_ERROR            4   /* the call failed */
#define DPSTAT_NODIALTONE       6   /* call progress */
#define DPSTAT_BUSY             7   /* call progress */
#define DPSTAT_NOANSWER         8   /* call progress */
#define DPSTAT_CHANGEDP         10  /* another pump takes over: the one in
				       MDMPRM_DP_REQUESTED, or the core's
				       choice if that is 0 */
#define DPSTAT_DIALED           11  /* a dial string ending in ';' is
				       dialled: back to command state, the
				       call kept (V.250 6.3.1) */
#define DPSTAT_FAX              12  /* answering adaptively
				       (MDMPRM_ADAPTIVE): the caller is a
				       fax machine; the pump is done */

/*
 *    The call's statistics (AT I11), which the pumps fill in through
 *    MDMPRM_STATS.  The core clears them at the start of each call.
 */
struct dp_coding {
	int states;                     /* of the trellis code */
	int nonlinear;                  /* the non-linear encoder */
	int precoding;                  /* precoding coefficients not 0 */
	int shaping;                    /* constellation expansion */
};

#define DP_STATS_V8_MAX 32

struct dp_stats {
	int dp;                         /* the DP_ID the call came up with;
					   0: the pump's own id */
	unsigned retrains_local;        /* retrains we started */
	unsigned retrains_remote;       /* retrains the remote started */
	unsigned renegs_local;          /* rate renegotiations we started */
	unsigned renegs_remote;         /* those the remote started */
	unsigned carrier_losses;        /* in data, the carrier gone for
					   CARRIER_DROP_MS or more and back
					   again (not the call's end) */
	unsigned have;                  /* DP_STATS_*: which of the
					   measures below are valid */
	float snr;                      /* the receiver's SNR, dB */
	float nmse;                     /* DP_STATS_NMSE: its mean squared
					   error over the square of the
					   points' minimum distance */
	int rtd;                        /* round trip delay, ms */
	float near_echo;                /* echo levels the canceller */
	float far_echo;                 /* removed, dBm0 */
	int far_echo_delay;             /* DP_STATS_FAR_DELAY: the far
					   echo's delay, ms */
	unsigned info_bits;             /* DP_STATS_INFO: V.34's INFO bits
					   as AT#UD's key 18 has them */
	/* V.8's CM and JM, from the call function octet on (0 octets:
	   not known) */
	unsigned char v8_cm[DP_STATS_V8_MAX], v8_jm[DP_STATS_V8_MAX];
	int v8_cm_n, v8_jm_n;
	/* DP_STATS_LINE: each direction's symbol rate (symbols/s) and
	   carrier (Hz); a PCM direction (V.90, V.91, V.92) 8000 and 0 */
	float sym_tx, sym_rx;
	float carrier_tx, carrier_rx;
	/* DP_STATS_CODING_TX / _RX: V.34's coding of each direction */
	struct dp_coding coding_tx, coding_rx;
	/* DP_STATS_FREQ_OFFSET: the receiver's carrier off its nominal,
	   Hz; DP_STATS_TIMING_OFFSET: the remote's symbol clock off ours,
	   ppm */
	float freq_offset;
	float timing_offset;
	/* DP_STATS_PCM: the network's G.711 law (1 A-law); DP_STATS_DIL:
	   what the V.90 / V.92 analogue modem's DIL showed of it, its
	   digital loss (dB) and the frame intervals (bit 0: the first of
	   6) whose least significant bit is robbed */
	int pcm_alaw;
	float digital_loss;
	int robbed_bits;
};

/*
 *    Text telephones (+MV18S, +MV18AM, +MBAUDOT): what the pumps of
 *    DP_V18, DP_BAUDOT and DP_EDT read through MDMPRM_TEXT
 */
struct dp_text {
	int baud100;            /* the 5-bit mode's rate, 1/100 bit/s
				   (4545: 45.45) */
	int mark, space;        /* its tones, Hz (1400, 1800) */
	int ita2;               /* its figures: 1 ITA2's (telex), 0 the
				   US TTY's */
	int probe;              /* DP_V18: probe a silent remote */
	int answer_msg;         /* answering, send msg once connected */
	char msg[64];           /* +MV18AM's answer message */
};

/*
 *    V.cvm (+CVM, S44, S45): what the DTE allows, and what the pump
 *    reports of the connection, through MDMPRM_VCVM.  The core keeps the
 *    settings (+CVM's) over calls and clears the rest at the start of
 *    each.
 */
struct dp_vcvm {
	int modes;              /* +CVM's <modes>: 1 robust, 2 narrowband,
				   4 wideband, 8 8-DPSK, 16 the amplitude
				   rings, 32 parametric (63 all) */
	int rate;               /* +CVM's <rate>: 8000 or 16000 */
	int report;             /* +CVM's <report>: +CVMR on each
				   renegotiation */
	int on;                 /* the pump has a connection: what
				   follows is valid */
	s32 raw_tx, raw_rx; /* bit/s */
	s32 coded_tx, coded_rx;
	int sym_tenths;         /* the receive direction's symbol, 0.1 ms */
	int tones_tx, tones_rx; /* 0 in the parametric mode */
	int code_rate;          /* the receive direction's: 0 1/2, 1 3/4,
				   2 7/8 */
	int renegs;             /* this call's renegotiations */
	long erasures;          /* octets the receiver marked */
	int v44_preset;         /* both DCEs have V.44's preset dictionary
				   (their INFO0s, Annex E): the link primes
				   V.44 with it */
	int zstd;               /* both DCEs indicated zstd (their INFO0s,
				   Annex F): the link's compression, if XID
				   agrees on any */
};

#define DP_STATS_SNR            0x1
#define DP_STATS_RTD            0x2
#define DP_STATS_NEAR_ECHO      0x4
#define DP_STATS_FAR_ECHO       0x8
#define DP_STATS_LINE           0x10
#define DP_STATS_CODING_TX      0x20
#define DP_STATS_CODING_RX      0x40
#define DP_STATS_FREQ_OFFSET    0x80
#define DP_STATS_TIMING_OFFSET  0x100
#define DP_STATS_PCM            0x200
#define DP_STATS_DIL            0x400
#define DP_STATS_FAR_DELAY      0x800
#define DP_STATS_NMSE           0x1000
#define DP_STATS_INFO           0x2000

/* the mean square of 16 bit linear samples at 0 dBm0: a sine of peak
   22300, G.711's digital milliwatt */
#define DP_DBM0_POWER           2.49e8f

/*
 *    What the pumps learn about the line over calls, for V.92 quick
 *    connect (V.92 clause 8).  The core keeps it per modem and hands it
 *    out with MDMPRM_DSPINFO; dmodemd saves it in its data file as it
 *    is.  Smart Link's library reads and writes all of it.
 */
struct dsp_info {
	unsigned connection_type;
	long clock_deviation;
	unsigned qc_lapm;
	unsigned qc_index;
};

/*
 *    Fax, service class 1 (T.31)
 */

/* what FAX_process() reports */
#define FAX_STATUS_OK           1
#define FAX_STATUS_ERROR        2
#define FAX_STATUS_CONNECT      3
#define FAX_STATUS_NOCARRIER    4
#define FAX_STATUS_FRAME_ERROR  5   /* an ERROR result, the call kept */

/* the class 1 commands of FAX_class1_command(), T.31 8.3.1 to 8.3.6 */
enum FAX_CMD {
	FAX_CMD_FTS = 0,        /* +FTS: silence */
	FAX_CMD_FRS = 1,        /* +FRS: wait for silence */
	FAX_CMD_FTM = 2,        /* +FTM: send data */
	FAX_CMD_FRM = 3,        /* +FRM: receive data */
	FAX_CMD_FTH = 4,        /* +FTH: send HDLC frames */
	FAX_CMD_FRH = 5,        /* +FRH: receive HDLC frames */
};

/*
 *    Voice (V.253)
 */

/* the commands of VOICE_command(); Smart Link's library takes 0 to 7 */
enum VOICE_CMD {
	VOICE_CMD_STATE_COMMAND = 0,    /* back to command state */
	VOICE_CMD_STATE_RX      = 1,    /* receive samples (+VRX) */
	VOICE_CMD_STATE_TX      = 2,    /* transmit samples (+VTX) */
	VOICE_CMD_STATE_DUPLEX  = 3,    /* both (+VTR) */
	VOICE_CMD_STATE_SPEAKER = 4,    /* the speaker (no one sends it) */
	VOICE_CMD_BEEP          = 5,    /* a +VTS tone */
	VOICE_CMD_DTMF          = 6,    /* a +VTS DTMF digit */
	VOICE_CMD_ABORT         = 7,
};

/* what VOICE_process() reports */
#define VOICE_STATUS_OK         1
#define VOICE_STATUS_ERROR      2
#define VOICE_STATUS_CONNECT    3

/* the voice engine's streaming state, as bits */
#define VOICE_STATE_COMMAND     0x00
#define VOICE_STATE_RX          0x01
#define VOICE_STATE_TX          0x02
#define VOICE_STATE_DUPLEX      (VOICE_STATE_RX | VOICE_STATE_TX)

/* the call progress tones the voice engine heard (VOICE_tones()): the
   rings of a ringing tone, whether one sounds now, the ms since one
   sounded last (since the engine started if none did), the busy
   cadences, the answer tones of 2100 Hz and of 2225 Hz, and the
   calling tones' bursts (V.253's <DLE>r, <DLE>b, <DLE>a, <DLE>f,
   <DLE>c and <DLE>e, which the engine reports) */
struct voice_tones {
	unsigned rings;
	int ringing;
	long quiet_ms;
	unsigned busy;
	unsigned ans2100, ans2225;
	unsigned cng, ct;       /* calling tones' bursts: a fax machine's
				   CNG (1100 Hz), V.25's CT (1300 Hz) */
};

/* the devices of AT+VLS (V.253 10.2.4.1) */
#define VOICE_DEVICE_NONE       0   /* on hook */
#define VOICE_DEVICE_LINE       1   /* the telephone line */
#define VOICE_DEVICE_HANDSET    14  /* V.253's H: a handset on the modem,
                                       the line on hook */
#define VOICE_DEVICE_HANDSET_LINE 15 /* HT: the handset on the call, the
                                       line off hook */

/* the core's +V settings, handed out with MDMPRM_VOICEINFO; Smart Link's
   library reads them at these offsets */
struct voice_info {
	unsigned comp_method;
	u32 sample_rate;
	unsigned rx_gain;
	unsigned tx_gain;
	unsigned dtmf_symbol;
	unsigned tone1_freq;
	unsigned tone2_freq;
	unsigned tone_duration;         /* 10 ms */
	unsigned inactivity_timer;
	unsigned silence_detect_sensitivity;
	unsigned silence_detect_period;
	/* +VTD: the duration of a +VTS element that gives none, 10 ms
	   (0: the DCE's own, 1 s); after the library's fields, which keep
	   their offsets */
	unsigned tone_default;
	/* +VRA, +VRN (V.253 10.2.5, 10.2.6): a voice dial's ringing-tone
	   timers, 100 ms and 1 s */
	unsigned ring_after;
	unsigned ring_never;
	/* #CID: caller ID on the call (Type II, the voice object) when not
	   0, its report as CID_create()'s cid_val */
	unsigned cid_format;
};

/*
 *    S-registers, from the core
 */

/* S-register num, 0 to 255; -1 if there is no such register */
int modem_get_sreg(struct modem *m, unsigned int num);
/* stores the low 8 bits of val in S-register num: 0, or -1 if there is
   no such register */
int modem_set_sreg(struct modem *m, unsigned int num, int val);

#ifdef __cplusplus
}
#endif

#endif /* DSP_MODEM_DEFS_H */
