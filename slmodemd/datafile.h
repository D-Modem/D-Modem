/*
 *
 *    datafile.h - what the data pumps learn about the line over calls
 *                 (struct dsp_info), kept in a file between runs
 *                 (/var/lib/slmodem/data.<modem>)
 *
 */
#ifndef SLMODEMD_DATAFILE_H
#define SLMODEMD_DATAFILE_H

#include <modem_defs.h>

namespace slm {

/* < 0 on failure (no file yet, a file of another size) */
int datafile_load(const char *path, struct dsp_info *info);
int datafile_save(const char *path, const struct dsp_info *info);

} // namespace slm

#endif
