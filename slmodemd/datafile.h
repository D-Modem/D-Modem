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

#include <string>

namespace slm {

class DataFile {
public:
	/* path opened for reading and writing, made (0600) if need be;
	   read only if it can't be written.  Opened while slmodemd is
	   still root, the descriptor is what lets save() write it after
	   the privileges are gone.  What can't be done is logged. */
	explicit DataFile(const char *path);
	~DataFile();
	DataFile(const DataFile &) = delete;
	DataFile &operator=(const DataFile &) = delete;

	/* the record into *info: the bytes read; -errno if the file can't
	   be read, -1 if it is empty, short or of another size (info
	   untouched then) */
	int load(struct dsp_info *info) const;
	/* *info as the file's record (written over in place, the file cut
	   to its size): the bytes written, -errno on failure (logged) */
	int save(const struct dsp_info *info);

private:
	std::string path_;
	int fd_ = -1;
	bool writable_ = false;
};

} // namespace slm

#endif
