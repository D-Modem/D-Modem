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

/* the data pumps (prop_dp_init registers them) */
extern int  prop_dp_init(void);
extern void prop_dp_exit(void);
/* state shared by the data pumps of a modem */
extern void *dp_runtime_create(struct modem *m);
extern void  dp_runtime_delete(void *runtime);
/* the G.711 law of the network (1 mu-law, 2 A-law): the open pumps only */
extern void  dp_runtime_set_law(void *runtime, int law);
/* 1 while a pump takes the line's exact PCM codes (V.91): no DC removal */
extern int   dp_runtime_pcm(void *runtime);
/* the DC remover */
extern void *dcr_create(void);
extern void  dcr_delete(void *dcr);
extern int   dcr_process(void *dcr, void *buf, int len);
/* the ring detector */
extern void *RD_create(struct modem *m, unsigned rate);
extern void  RD_delete(void *obj);
extern int   RD_process(void *obj, void *in, int count);
extern void  RD_ring_details(void *obj, long *freq, long *duration);
/* caller ID */
extern void *CID_create(struct modem *m, unsigned rate, unsigned cid_val);
extern void  CID_delete(void *cid);
extern int   CID_process(void *cid, void *in, int count);
/* voice */
extern void *VOICE_create(struct modem *m, unsigned srate);
extern void  VOICE_delete(void *obj);
extern int   VOICE_process(void *obj, void *in, void *out, int count);
extern int   VOICE_command(void *obj, enum VOICE_CMD cmd);
/* fax class 1 */
extern void *FAX_create(struct modem *m, unsigned caller, unsigned srate);
extern void  FAX_delete(void *obj);
extern int   FAX_process(void *obj, void *in, void *out, int count);
extern int   FAX_class1_command(void *obj, unsigned cmd, unsigned param);

/*
 *    the core -> the library (besides modem_dp.h and modem_param.h)
 */

extern unsigned int dsplibs_debug_level;
extern int dsplibs_debug_printf(const char *fmt, ...);
extern int modem_send_to_tty(struct modem *m, const char *buf, int n);
extern int modem_recv_from_tty(struct modem *m, char *buf, int n);

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
