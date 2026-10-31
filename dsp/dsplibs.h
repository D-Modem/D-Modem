/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dsplibs.h - the rest of a data pump library's interface: the entry
 *                points the modem core calls (start up, the call's
 *                runtime, DC removal, caller ID, voice, fax), and what
 *                the core gives the library (the DTE's characters, the
 *                debug log, the C library)
 *
 *    The open library in dsp/ and Smart Link's i386 library (with
 *    dsp/test/smartlink_glue.cpp for what it lacks) both implement the
 *    first part; core/interface.cpp and core/log.cpp, or a test playing
 *    the core, the second.  Everything has C linkage: the definitions take
 *    it from here, and the binary library knows the plain names.
 *
 */

#ifndef DSP_DSPLIBS_H
#define DSP_DSPLIBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "modem_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct modem;

/*
 *    The library, for the core
 */

/* registers each of the library's pumps (modem_dp_register()); 0.  Once
   per process, before the first modem runs */
int prop_dp_init(void);
/* deregisters them */
void prop_dp_exit(void);

/* the state the pumps of one call share (MDMPRM_DPRUNTIME), created as
   the call starts; NULL when out of memory */
void *dp_runtime_create(struct modem *m);
/* frees it at the end of the call */
void dp_runtime_delete(void *runtime);
/* the network's G.711 law for the call's pumps: 1 mu-law, 2 A-law */
void dp_runtime_set_law(void *runtime, int law);
/* 1: the line is an analogue one (a codec on a telephone line, at 9600
   or 8000 Hz), where the V.34 pump is V.90's analogue modem; 0 (the
   default): an 8000 Hz line is the G.711 network, the digital modem's */
void dp_runtime_set_analog(void *runtime, int analog);
/* a clear channel's V.110 user rate (300 to 56000, AT+CBST) for the
   call's pump, 0 for none: then the answerer runs V.110 at the rate of
   the caller's frames if they come, else D-Modem's clear channel */
void dp_runtime_set_v110(void *runtime, long rate);
/* the V.110 user rate the clear pump runs at, 0 for none */
long dp_runtime_v110(void *runtime);
/* 1 while a pump takes the line's exact PCM codes (V.91), when the core
   must not remove DC from them; else 0 */
int dp_runtime_pcm(void *runtime);

/* a DC remover for the received samples */
void *dcr_create(void);
void dcr_delete(void *dcr);
/* takes the DC out of len s16 samples of buf in place; 1 if the DC is
   suspiciously large, else 0 */
int dcr_process(void *dcr, void *buf, int len);

/* a caller ID receiver at rate samples/s (Bell 202 or V.23 FSK, or
   DTMF: dsp_glue.cpp), reporting to modem m's DTE the fields formatted
   (cid_val 1) or the message in hex; packet: as voice mode's complex
   event report, <DLE>X ... <DLE>. (V.253 7.1.2) */
void *CID_create(struct modem *m, u32 rate, unsigned cid_val,
		 int packet);
void CID_delete(void *cid);
/* count received samples: 0 to go on, 1 once a message is reported */
int CID_process(void *cid, void *in, int count);
/* a caller ID message (SDMF or MDMF, from its type octet to its
   checksum: len octets at msg) that came another way than on the line,
   such as a SIP call's caller, reported to modem m's DTE as a received
   one would be (cid_val, packet: as CID_create()); 0, or -1 if it is
   not a whole message */
int CID_report(struct modem *m, unsigned cid_val, int packet,
	       const void *msg, int len);

/* the voice engine (V.253) of modem m at srate samples/s */
void *VOICE_create(struct modem *m, u32 srate);
void VOICE_delete(void *obj);
/* count samples in and out: a VOICE_STATUS_* to report, 0, or < 0 to
   stop voice */
int VOICE_process(void *obj, void *in, void *out, int count);
/* a command: 0, or -1 for one it does not know */
int VOICE_command(void *obj, enum VOICE_CMD cmd);
/* the call progress tones heard so far into *t */
void VOICE_tones(void *obj, struct voice_tones *t);

/* the service class 1 fax engine (T.31) of modem m, on the calling side
   if caller is not 0, at srate samples/s */
void *FAX_create(struct modem *m, unsigned caller, u32 srate);
/* an answerer's answer tone already on the line for ms (adaptive
   answer): its CED cut short, or none */
void FAX_answer_tone(void *obj, u32 ms);
/* class 1.0 (T.31): V.8 and V.34 half-duplex (T.30 Annex F) before the
   first FAX_process, the primary channel up to maxp * 2400 bit/s (minp,
   prefc: +F34's, taken); with a remote without V.8 the engine goes on as
   class 1.  0, or -1 when the modem can't start */
int FAX_v34(void *obj, unsigned maxp, unsigned minp, unsigned prefc);
/* frees it */
void FAX_delete(void *obj);
/* count samples in and out: a FAX_STATUS_* to report, 0, or < 0 to end
   the call */
int FAX_process(void *obj, void *in, void *out, int count);
/* an enum FAX_CMD with its parameter (the modulation, or the time in
   10 ms for +FTS and +FRS): 1 once started, its results then coming
   from FAX_process(); -1 to refuse it, or without an engine */
int FAX_class1_command(void *obj, unsigned cmd, unsigned param);
/* from now on the engine works over T.38: it sends each IFP packet
   (struct t38_ifp, t38.h) with tx(ctx, ifp), with up to max_data octets
   of data in it (0: 300), and FAX_process() keeps the time, sending
   silence.  0, or -1 if it cannot */
int FAX_t38_start(void *obj, void (*tx)(void *ctx, const void *ifp),
		  void *ctx, int max_data);
/* back to the audio */
void FAX_t38_stop(void *obj);
/* a T.38 IFP packet received */
void FAX_t38_rx(void *obj, const void *ifp);

/* service class 2 and 2.0 (T.32): the modem runs T.30 itself, its DTE
   gives and takes the pages in T.4 (MH) and steers the call with +FDT,
   +FDR, +FET and +FKS (and polls: +FDR after the DIS).  What the core
   keeps of +FLI, +FIS and +FBO for
   the engine */
struct fax2_settings {
	unsigned cls;           /* 2, or 20 for 2.0 */
	char ident[21];         /* +FLI */
	unsigned vr, br, ec;    /* +FIS: fine resolution, the fastest rate
				   (0 2400 to 5 14400 bit/s; 6 16800 to 13
				   33600: V.34 half-duplex, with error
				   correction), error correction mode */
	unsigned bo;            /* +FBO: bit 0, the page data with its first
				   bit in the top of each octet */
	unsigned lp;            /* +FLP: a document to be polled (answering:
				   the DIS says so, a DTC has it sent) */
	char pi[21];            /* +FPI: our CIG when we poll */
	unsigned sp;            /* +FSP: calling, we poll (V.8 says so) */
};
/* the class 2 commands for FAX2_command() */
enum FAX2_CMD { FAX2_CMD_FDT, FAX2_CMD_FDR, FAX2_CMD_FET, FAX2_CMD_FKS };
/* what FAX2_info() tells */
struct fax2_info {
	unsigned session[8];    /* +FCS: vr, br, wd, ln, df, ec, bf, st */
	int session_set;        /* there is one */
	int hangup;             /* the hangup status (T.32 Table 23, as
				   decimal: 100 is A0 in 2.0), -1: none yet */
};
/* the class 2 engine of modem m, on the calling side if caller is not
   0, at srate samples/s; its results to ATD or ATA from
   FAX2_process().  NULL when out of memory, or in a library without
   it */
void *FAX2_create(struct modem *m, unsigned caller, unsigned srate,
		  const struct fax2_settings *s);
void FAX2_delete(void *obj);
/* count samples in and out: a FAX_STATUS_* to report (FRAME_ERROR is
   ERROR), 0, or < 0 to end the call */
int FAX2_process(void *obj, void *in, void *out, int count);
/* a command (enum FAX2_CMD; param +FET's), with the settings as they
   are now: 1 once started (the DTE's characters then the engine's until
   its result), -1 to refuse it */
int FAX2_command(void *obj, unsigned cmd, unsigned param,
		 const struct fax2_settings *s);
/* the session and the hangup status, into *i */
void FAX2_info(void *obj, struct fax2_info *i);
/* T.38 for the class 2 engine, as FAX_t38_start(), FAX_t38_stop() and
   FAX_t38_rx() for class 1's; and a packet lost */
int FAX2_t38_start(void *obj, void (*tx)(void *ctx, const void *ifp),
		   void *ctx, int max_data);
void FAX2_t38_stop(void *obj);
void FAX2_t38_rx(void *obj, const void *ifp);
void FAX2_t38_lost(void *obj);

/*
 *    The core, for the library
 */

/* the level of the library's debug lines (defined by the core) */
extern unsigned int dsplibs_debug_level;
/* a debug line to the core's log, as printf(); its length */
int dsplibs_debug_printf(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

/* up to n characters of buf to modem m's DTE: how many it took */
int modem_send_to_tty(struct modem *m, const char *buf, int n);
/* up to n characters from modem m's DTE into buf: how many (0: none) */
int modem_recv_from_tty(struct modem *m, char *buf, int n);

/* the C library's functions of the same names without sysdep_ (strstr()
   and memchr() with results that are not const) */
void *sysdep_malloc(unsigned int size);
void sysdep_free(void *mem);
size_t sysdep_strlen(const char *s);
char *sysdep_strcpy(char *d, const char *s);
char *sysdep_strcat(char *d, const char *s);
int sysdep_strcmp(const char *s1, const char *s2);
char *sysdep_strstr(const char *s1, const char *s2);
void *sysdep_memset(void *d, int c, size_t l);
void *sysdep_memcpy(void *d, const void *s, size_t l);
void *sysdep_memchr(const void *s, int c, size_t l);
int sysdep_vsnprintf(char *str, unsigned size, const char *format,
		     va_list ap);
/* sprintf(): no bound on buf */
int sysdep_sprintf(char *buf, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DSPLIBS_H */
