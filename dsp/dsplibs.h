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
