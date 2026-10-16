/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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

#ifdef __cplusplus
extern "C" {
#endif

/* the modem core, opaque to the data pumps */
struct modem;

/* integers of a fixed width; s16 is a 16 bit linear sample */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

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
	SREG_CONNECT_PROTOCOL             = 106, /* AT\V */
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
	DP_V22      = 22,
	DP_V23      = 23,
	DP_V32      = 32,
	DP_V34      = 34,
	DP_K56      = 56,       /* K56flex: the core runs V.90 for it */
	DP_V90      = 90,
	DP_V91      = 91,
	DP_V92      = 92,
	DP_FAX      = 100,
	DP_B103     = 103,      /* Bell 103 */
	DP_V8BIS    = 108,
	DP_V22BIS   = 122,
	DP_SINUS    = 127,      /* a test pump */
	DP_V32BIS   = 132,
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

/*
 *    The call's statistics (AT I11), which the pumps fill in through
 *    MDMPRM_STATS.  The core clears them at the start of each call.
 */
struct dp_stats {
	int dp;                         /* the DP_ID the call came up with;
					   0: the pump's own id */
	unsigned retrains_local;        /* retrains we started */
	unsigned retrains_remote;       /* retrains the remote started */
	unsigned renegs_local;          /* rate renegotiations we started */
	unsigned renegs_remote;         /* those the remote started */
	unsigned have;                  /* DP_STATS_*: which of the
					   measures below are valid */
	float snr;                      /* the receiver's SNR, dB */
	int rtd;                        /* round trip delay, ms */
	float near_echo;                /* echo levels the canceller */
	float far_echo;                 /* removed, dBm0 */
};

#define DP_STATS_SNR            0x1
#define DP_STATS_RTD            0x2
#define DP_STATS_NEAR_ECHO      0x4
#define DP_STATS_FAR_ECHO       0x8

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
   sounded last (since the engine started if none did), and the busy
   cadences (V.253's <DLE>r and <DLE>b, which the engine reports) */
struct voice_tones {
	unsigned rings;
	int ringing;
	long quiet_ms;
	unsigned busy;
};

/* the devices of AT+VLS (V.253 10.2.4.1) */
#define VOICE_DEVICE_NONE       0   /* on hook */
#define VOICE_DEVICE_LINE       1   /* the telephone line */

/* the core's +V settings, handed out with MDMPRM_VOICEINFO; Smart Link's
   library reads them at these offsets */
struct voice_info {
	unsigned comp_method;
	unsigned sample_rate;
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
