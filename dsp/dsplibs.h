/*
 *
 *    dsplibs.h - the interface between the modem core and its data pump
 *                library: the original binary-only dsplibs.o (i386
 *                object code, C linkage) or the open data pumps in dsp/
 *
 *    The data pumps themselves register through modem_dp.h; this is the
 *    rest, both ways.  Everything here keeps C linkage so that the core,
 *    built as C++, still links with the original library.
 *
 */

#ifndef __DSPLIBS_H__
#define __DSPLIBS_H__

#include <stdarg.h>
#include <stddef.h>

#include <modem_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modem;

/*
 *    the library -> the core
 */

/* the data pumps (prop_dp_init registers them, 0; prop_dp_exit takes
   them back) */
extern int  prop_dp_init(void);
extern void prop_dp_exit(void);
/* state shared by the data pumps of a modem (struct dp_runtime): one per
   modem, NULL when out of memory; the pumps find it via MDMPRM_DPRUNTIME */
extern void *dp_runtime_create(struct modem *m);
extern void  dp_runtime_delete(void *runtime);
/* the G.711 law of the network (1 mu-law, 2 A-law): the open pumps only */
extern void  dp_runtime_set_law(void *runtime, int law);
/* 1 while a pump takes the line's exact PCM codes (V.91): no DC removal */
extern int   dp_runtime_pcm(void *runtime);
/* the DC remover: dcr_process takes the DC out of len s16 samples in
   place, and returns 1 when the DC is suspiciously large (>= 3000) */
extern void *dcr_create(void);
extern void  dcr_delete(void *dcr);
extern int   dcr_process(void *dcr, void *buf, int len);
/* caller ID (Bell 202 FSK) at rate Hz; cid_val 1: formatted fields, else
   the raw message in hex, sent to the DTE.  CID_process returns 0 to go
   on, 1 when the message was reported, -1 on error */
extern void *CID_create(struct modem *m, unsigned rate, unsigned cid_val);
extern void  CID_delete(void *cid);
extern int   CID_process(void *cid, void *in, int count);
/* voice (IS-101 8 bit linear with <DLE> shielding) at srate Hz:
   VOICE_process returns a VOICE_STATUS_* to report, or 0;
   VOICE_command 0, or -1 for a command it does not know */
extern void *VOICE_create(struct modem *m, unsigned srate);
extern void  VOICE_delete(void *obj);
extern int   VOICE_process(void *obj, void *in, void *out, int count);
extern int   VOICE_command(void *obj, enum VOICE_CMD cmd);
/* fax class 1 (caller: the calling fax): FAX_process returns a
   FAX_STATUS_* to report, or 0; FAX_class1_command (enum FAX_CMD and
   its +F parameter) 1, or -1 when refused */
extern void *FAX_create(struct modem *m, unsigned caller, unsigned srate);
extern void  FAX_delete(void *obj);
extern int   FAX_process(void *obj, void *in, void *out, int count);
extern int   FAX_class1_command(void *obj, unsigned cmd, unsigned param);

/*
 *    the core -> the library (besides modem_dp.h and modem_param.h)
 */

/* the library's debug output, printed when dsplibs_debug_level allows */
extern unsigned int dsplibs_debug_level;
extern int dsplibs_debug_printf(const char *fmt, ...);
/* characters to and from the DTE: n at most, returns how many moved */
extern int modem_send_to_tty(struct modem *m, const char *buf, int n);
extern int modem_recv_from_tty(struct modem *m, char *buf, int n);

/* the C library, for the original object code */
extern void  *sysdep_malloc(unsigned int size);
extern void   sysdep_free(void *mem);
extern size_t sysdep_strlen(const char *s);
extern char  *sysdep_strcpy(char *d, const char *s);
extern char  *sysdep_strcat(char *d, const char *s);
extern int    sysdep_strcmp(const char *s1, const char *s2);
extern char  *sysdep_strstr(const char *s1, const char *s2);
extern void  *sysdep_memset(void *d, int c, size_t l);
extern void  *sysdep_memcpy(void *d, const void *s, size_t l);
extern void  *sysdep_memchr(const void *s, int c, size_t l);
extern int    sysdep_vsnprintf(char *str, unsigned size, const char *format,
			       va_list ap);
extern int    sysdep_sprintf(char *buf, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __DSPLIBS_H__ */
