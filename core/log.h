/*
 *
 *    log.h - the modem's debug output: lines of text (to stderr with a
 *            time stamp, or to a hook), and a binary log of the samples,
 *            bits and characters (slmodem.log, for debugging)
 *
 *    The data pumps print through dsplibs_debug_printf() (dsplibs.h),
 *    which ends up here too.
 *
 */
#ifndef DMODEM_CORE_LOG_H
#define DMODEM_CORE_LOG_H

#include <stdarg.h>

namespace dcore {

/* the lines up to this level are printed (0: none of the modem's) */
extern unsigned log_level;
/* the categories of the binary log up to this one are written (0: only
   the text, when the log is open) */
extern unsigned log_data_level;

/* NULL: stderr, with a time stamp; else each line goes to fn */
void log_set_hook(void (*fn)(const char *line));
bool log_hooked();

/* a line, printed if level is up to log_level; its length */
int log_printf(unsigned level, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));
/* the same with a va_list */
int log_vprintf(unsigned level, const char *fmt, va_list args);

/* the binary log: slmodem.log[.<suffix>], if log_data_level > 0 */
enum LogData {
	LOG_TEXT = 0, LOG_RX_SAMPLES, LOG_TX_SAMPLES, LOG_ECHO_SAMPLES,
	LOG_MISC, LOG_MISC1, LOG_RX_BITS, LOG_TX_BITS, LOG_RX_DATA,
	LOG_TX_DATA, LOG_RX_CHARS, LOG_TX_CHARS,
};
/* opens it (and the pumps' level follows log_level); -1 on failure */
int log_open(const char *suffix);
/* closes it */
void log_close();
/* a record of count octets of category id from modem who */
void log_data(const void *who, unsigned id, const void *data, int count);

} // namespace dcore

#endif
