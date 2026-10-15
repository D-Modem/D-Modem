/*
 *
 *    line_frames.h - a modem's line of 16 bit samples coming in on a
 *                    descriptor, cut at every frame-th sample of the
 *                    line: between the pieces go the modem's timers and
 *                    its DTE, so that they meet the line at the same
 *                    samples whatever the reads bring.  PipeModem's line
 *                    (whole frames, up to the DTE's next sync point) and
 *                    the test peers' HalfModem (what came, to each
 *                    frame's end).
 *
 *    Header only: the test peers' 32 bit harnesses have it too.
 *
 */
#ifndef SLMODEMD_LINE_FRAMES_H
#define SLMODEMD_LINE_FRAMES_H

#include <errno.h>
#include <string.h>
#include <unistd.h>

namespace slm {

class LineFrames {
public:
	/* the line in on fd, cut at every frame-th sample */
	LineFrames(int fd, int frame) : fd_(fd), frame_(frame) {}

	/* what the descriptor has, behind what waits, until fill bytes
	   wait at most: the bytes read, 0 at its end, -1 on an error
	   (errno; EINTR is tried again; ENOBUFS: fill bytes wait already) */
	int read(int fill = SIZE)
	{
		int r;

		if (head_) {
			memmove(bytes(), bytes() + head_, n_);
			head_ = 0;
		}
		if (fill > SIZE)
			fill = SIZE;
		if (fill <= n_) {
			errno = ENOBUFS;
			return -1;
		}
		do
			r = (int)::read(fd_, bytes() + n_, fill - n_);
		while (r < 0 && errno == EINTR);
		if (r > 0)
			n_ += r;
		return r;
	}
	/* the whole samples waiting */
	int waiting() const { return n_ / 2; }
	/* the line's samples taken so far */
	unsigned long long taken() const { return taken_; }

	/* the samples waiting up to the next frame end of the line's clock,
	   max at most: at *p (until read()), how many; 0 if none wait.  The
	   clock is the samples taken, or the host's own count of the line
	   (HalfModem's line_n, which a modem's delay moves on as well) */
	int block(const short **p, int max) { return block(p, max, taken_); }
	int block(const short **p, int max, unsigned long long clock)
	{
		int k = frame_ - (int)(clock % frame_);
		return take(p, k < max ? k : max);
	}
	/* the last n samples taken wait again (a host that did fewer of
	   them than it took; no read() since) */
	void unget(int n)
	{
		head_ -= 2 * n;
		n_ += 2 * n;
		taken_ -= n;
	}
	/* the samples waiting up to the last frame end there is among
	   them, max at most: at *p (until read()), how many; 0 if no frame
	   end is that near */
	int frames(const short **p, int max)
	{
		unsigned long long end;
		int n = waiting() < max ? waiting() : max;

		end = (taken_ + n) / frame_ * frame_;
		return end > taken_ ? take(p, (int)(end - taken_)) : 0;
	}

private:
	enum { SIZE = 8192 };

	/* n of the samples waiting, fewer if fewer wait */
	int take(const short **p, int n)
	{
		if (n > waiting())
			n = waiting();
		if (n <= 0)
			return 0;
		*p = buf_ + head_ / 2;
		head_ += 2 * n;
		n_ -= 2 * n;
		taken_ += n;
		return n;
	}

	/* the buffer's octets */
	char *bytes() { return reinterpret_cast<char *>(buf_); }

	int fd_, frame_;
	short buf_[SIZE / 2];
	int head_ = 0, n_ = 0;          /* bytes()[head_ .. head_ + n_)
					   waits, head_ even */
	unsigned long long taken_ = 0;
};

} // namespace slm

#endif
