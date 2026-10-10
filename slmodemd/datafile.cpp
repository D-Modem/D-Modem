/*
 *
 *    datafile.cpp - the dsp_info file (datafile.h); slmodemd's
 *                   modem_datafile.c, the same format
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <log.h>

#include "datafile.h"

namespace slm {

struct datafile_record {
	unsigned minor;
	struct dsp_info dsp_info;
	unsigned size;
};

int datafile_load(const char *path, struct dsp_info *info)
{
	struct datafile_record dr;
	int fd, ret;

	dcore::log_printf(1, "open file: %s...\n", path);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		dcore::log_printf(1, "cannot open '%s': %s\n", path,
				  strerror(errno));
		return -errno;
	}
	ret = read(fd, &dr, sizeof(dr));
	close(fd);
	if (ret < 0)
		return -errno;
	/* a simple check */
	if (ret != (int)sizeof(dr) || dr.size != sizeof(dr))
		return -1;
	memcpy(info, &dr.dsp_info, sizeof(*info));
	return ret;
}

int datafile_save(const char *path, const struct dsp_info *info)
{
	struct datafile_record dr = {};
	int fd, ret;

	fd = open(path, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		return -errno;
	memcpy(&dr.dsp_info, info, sizeof(*info));
	dr.size = sizeof(dr);
	ret = write(fd, &dr, sizeof(dr));
	close(fd);
	return ret;
}

} // namespace slm
