/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    line_recorder.h - a call's line audio in two raw files, as the
 *                      front end passes it: what the modem receives and
 *                      what it sends (PipeModem's MODEM_REC, the SIP
 *                      user agent's DMODEM_REC)
 *
 */
#ifndef DMODEMD_LINE_RECORDER_H
#define DMODEMD_LINE_RECORDER_H

#include <cstddef>
#include <cstdio>

namespace dmd {

class LineRecorder {
public:
	LineRecorder() = default;
	/* close() */
	~LineRecorder() { close(); }
	LineRecorder(const LineRecorder &) = delete;
	LineRecorder &operator=(const LineRecorder &) = delete;

	/* <base><rx_ext> and <base><tx_ext>: both, or neither (false, with
	   a message) */
	bool open(const char *base, const char *rx_ext, const char *tx_ext);
	/* the files closed, if open */
	void close();
	/* n bytes the modem received, and n it sent (if open) */
	void rx(const void *buf, size_t n)
	{
		if (rx_)
			fwrite(buf, 1, n, rx_);
	}
	/* (n bytes it sent) */
	void tx(const void *buf, size_t n)
	{
		if (tx_)
			fwrite(buf, 1, n, tx_);
	}

private:
	FILE *rx_ = nullptr, *tx_ = nullptr;
};

} // namespace dmd

#endif
