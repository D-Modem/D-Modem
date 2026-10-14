/*
 *
 *    datafile.cpp - the dsp_info file (datafile.h); slmodemd's
 *                   modem_datafile.c, the same format
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <log.h>

#include "datafile.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, fmt, ##__VA_ARGS__)

namespace slm {

/* the file: one record, its own size last as the check */
struct datafile_record {
	unsigned minor;
	struct dsp_info dsp_info;
	unsigned size;
};

/* see datafile.h.  No directory (ENOENT) is the usual case of no data
   kept, logged as debug; anything else is an error */
DataFile::DataFile(const char *path) : path_(path)
{
	DBG("open file: %s...\n", path);
	fd_ = open(path, O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (fd_ >= 0) {
		writable_ = true;
		return;
	}
	int err = errno;
	fd_ = open(path, O_RDONLY | O_CLOEXEC);
	if (fd_ >= 0)
		ERR("cannot write '%s': %s; the line's data is not saved\n",
		    path, strerror(err));
	else if (err == ENOENT)
		DBG("cannot open '%s': %s\n", path, strerror(err));
	else
		ERR("cannot open '%s': %s; the line's data is not kept\n",
		    path, strerror(err));
}

DataFile::~DataFile()
{
	if (fd_ >= 0)
		close(fd_);
}

/* see datafile.h */
int DataFile::load(struct dsp_info *info) const
{
	struct datafile_record dr;
	ssize_t ret;

	if (fd_ < 0)
		return -EBADF;
	ret = pread(fd_, &dr, sizeof(dr), 0);
	if (ret < 0)
		return -errno;
	/* a simple check */
	if (ret != (ssize_t)sizeof(dr) || dr.size != sizeof(dr))
		return -1;
	memcpy(info, &dr.dsp_info, sizeof(*info));
	return ret;
}

/* see datafile.h */
int DataFile::save(const struct dsp_info *info)
{
	struct datafile_record dr = {};
	ssize_t ret;

	if (!writable_)
		return -EBADF;
	memcpy(&dr.dsp_info, info, sizeof(*info));
	dr.size = sizeof(dr);
	ret = pwrite(fd_, &dr, sizeof(dr), 0);
	if (ret == (ssize_t)sizeof(dr) && !ftruncate(fd_, sizeof(dr)))
		return ret;
	int err = ret < 0 || ret == (ssize_t)sizeof(dr) ? errno : EIO;
	ERR("cannot save '%s': %s\n", path_.c_str(), strerror(err));
	return -err;
}

} // namespace slm
