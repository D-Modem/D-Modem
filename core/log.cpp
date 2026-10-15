/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    log.cpp - the modem's debug output (log.h); slmodemd's
 *              modem_debug.c
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <dsplibs.h>

#include "log.h"

/* the data pumps' level (dsplibs.h) */
unsigned int dsplibs_debug_level = 0;

namespace dcore {

unsigned log_level = 0;
unsigned log_data_level = 0;

static void (*hook)(const char *line);
static int log_fd = -1;

/* a record of the binary log: the modem (its address), the category
   (LogData), the octets that follow */
struct LogHeader {
	unsigned modem_id;
	unsigned id;
	unsigned length;
};

/* the lines to fn (without their time stamp), or to stderr (NULL) */
void log_set_hook(void (*fn)(const char *line))
{
	hook = fn;
}

/* the lines go to a hook */
bool log_hooked()
{
	return hook != nullptr;
}

/* a record of count octets to the binary log, if it is open and id is
   up to log_data_level */
void log_data(const void *who, unsigned id, const void *data, int count)
{
	LogHeader hdr;
	if (id > log_data_level || log_fd < 0)
		return;
	hdr.modem_id = (unsigned)(uintptr_t)who;
	hdr.id = id;
	hdr.length = count;
	if (write(log_fd, &hdr, sizeof(hdr)) < 0 ||
	    write(log_fd, data, count) < 0)
		fprintf(stderr, "log_data: cannot write: %s\n",
			strerror(errno));
}

/* a line with a time stamp (s modulo 1000, us): to the binary log when
   it is on, to the hook (without the stamp) or stderr when level is up
   to log_level; returns the length without the stamp (511 octets at
   most with it) */
int log_vprintf(unsigned level, const char *fmt, va_list args)
{
	char line[512];
	struct timeval tv;
	int i, len;
	gettimeofday(&tv, NULL);
	i = snprintf(line, sizeof(line), "<%03ld.%06ld> ",
		     (long)(tv.tv_sec % 1000), (long)tv.tv_usec);
	len = vsnprintf(line + i, sizeof(line) - i, fmt, args);
	if (len < 0)
		return len;
	if (len > (int)sizeof(line) - i - 1)
		len = sizeof(line) - i - 1;
	if (log_data_level)
		log_data(nullptr, LOG_TEXT, line, len + i);
	if (level <= log_level) {
		if (hook)
			hook(line + i);
		else
			fputs(line, stderr);
	}
	return len;
}

/* log_vprintf() with the arguments */
int log_printf(unsigned level, const char *fmt, ...)
{
	va_list args;
	int len;
	va_start(args, fmt);
	len = log_vprintf(level, fmt, args);
	va_end(args);
	return len;
}

/* the pumps' level follows the modem's; with log_data_level, the
   binary log dmodem.log[.suffix] in the current directory (and the
   pumps at level 3 at least); -1 if it cannot be created */
int log_open(const char *suffix)
{
	char name[PATH_MAX];
	dsplibs_debug_level = log_level;
	if (!log_data_level)
		return 0;
	if (suffix)
		snprintf(name, sizeof(name), "dmodem.log.%s", suffix);
	else
		snprintf(name, sizeof(name), "dmodem.log");
	log_fd = creat(name, S_IRUSR | S_IWUSR);
	if (log_fd < 0) {
		fprintf(stderr, "cannot create `%s': %s\n", name,
			strerror(errno));
		return -1;
	}
	if (dsplibs_debug_level < 3)
		dsplibs_debug_level = 3;
	return 0;
}

/* the binary log closed */
void log_close()
{
	if (log_fd >= 0) {
		close(log_fd);
		log_fd = -1;
	}
}

} // namespace dcore

/* the data pumps' lines: printed when their level is up to the modem's */
int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list args;
	int len;
	va_start(args, fmt);
	len = dcore::log_vprintf(dsplibs_debug_level, fmt, args);
	va_end(args);
	return len;
}
