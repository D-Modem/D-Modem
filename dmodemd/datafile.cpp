/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    datafile.cpp - the dsp_info file (datafile.h), in the format of
 *                   slmodemd's modem_datafile.c
 *
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#endif

#include <log.h>

#include "dm_types.h"
#include "datafile.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, fmt, ##__VA_ARGS__)

#ifdef _WIN32
/* (Windows: the file binary, not inherited; positioned reads and writes
   by a seek first, the descriptor being this object's alone) */
#define O_CLOEXEC (O_BINARY | O_NOINHERIT)
/* pread() by a seek and a read */
static ssize_t pread(int fd, void *buf, size_t n, off_t at)
{
	if (lseek(fd, at, SEEK_SET) < 0)
		return -1;
	return read(fd, buf, (unsigned)n);
}
/* pwrite() by a seek and a write */
static ssize_t pwrite(int fd, const void *buf, size_t n, off_t at)
{
	if (lseek(fd, at, SEEK_SET) < 0)
		return -1;
	return write(fd, buf, (unsigned)n);
}
#endif

namespace dmd {

/* the file: one record, its own size last as the check */
struct datafile_record {
	u32 minor;
	struct dsp_info dsp_info;
	u32 size;
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

/* the file closed, if open */
DataFile::~DataFile()
{
	if (fd_ >= 0)
		close(fd_);
}

/* see datafile.h */
s32 DataFile::load(struct dsp_info *info) const
{
	struct datafile_record dr;
	ssize_t ret;

	if (fd_ < 0)
		return -EBADF;
	ret = pread(fd_, &dr, sizeof(dr), 0);
	if (ret < 0)
		return -errno;
	/* a simple check */
	if (ret != static_cast<ssize_t>(sizeof(dr)) || dr.size != sizeof(dr))
		return -1;
	*info = dr.dsp_info;
	return ret;
}

/* see datafile.h */
s32 DataFile::save(const struct dsp_info *info)
{
	struct datafile_record dr = {};
	ssize_t ret;

	if (!writable_)
		return -EBADF;
	dr.dsp_info = *info;
	dr.size = sizeof(dr);
	ret = pwrite(fd_, &dr, sizeof(dr), 0);
	if (ret == static_cast<ssize_t>(sizeof(dr)) &&
	    !ftruncate(fd_, sizeof(dr)))
		return ret;
	int err = ret < 0 || ret == static_cast<ssize_t>(sizeof(dr)) ? errno :
								       EIO;
	ERR("cannot save '%s': %s\n", path_.c_str(), strerror(err));
	return -err;
}

} // namespace dmd
