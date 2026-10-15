/*
 *
 *    line_recorder.cpp - a call's line audio in two files
 *                        (line_recorder.h)
 *
 */

#include <string>

#include "line_recorder.h"

namespace dmd {

/* see line_recorder.h */
bool LineRecorder::open(const char *base, const char *rx_ext,
			const char *tx_ext)
{
	std::string b = base;

	close();
	rx_ = fopen((b + rx_ext).c_str(), "wb");
	tx_ = fopen((b + tx_ext).c_str(), "wb");
	if (rx_ && tx_)
		return true;
	fprintf(stderr, "error: can't record to %s%s/%s\n", base, rx_ext,
		tx_ext);
	close();
	return false;
}

/* see line_recorder.h */
void LineRecorder::close()
{
	if (rx_)
		fclose(rx_);
	if (tx_)
		fclose(tx_);
	rx_ = tx_ = nullptr;
}

} // namespace dmd
