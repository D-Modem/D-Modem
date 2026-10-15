/*
 *
 *    run_loop.h - the main loop of a modem program (dmodemd, the
 *                 modem_test pairs): until SIGINT, SIGTERM or stop(),
 *                 each turn the modems' work due, then select() on the
 *                 descriptors the turn watches until their next timer
 *
 */
#ifndef DMODEMD_RUN_LOOP_H
#define DMODEMD_RUN_LOOP_H

#include <signal.h>
#include <sys/select.h>

namespace dcore { class Modem; }

namespace dmd {

class RunLoop {
public:
	/* SIGINT and SIGTERM stop() it */
	static void catch_signals();
	/* the loop ends at its next turn (safe in a signal handler) */
	static void stop() { running_ = 0; }
	static bool running() { return running_; }

	/* the n modems' work due (poll()): the ms to the first of their
	   next timers, 1000 at most */
	static long poll(dcore::Modem *const *m, int n);

	/* this turn's descriptors: none, then each to watch */
	void clear();
	void watch(int fd);
	/* fd is readable (after wait()) */
	bool ready(int fd) const { return fd >= 0 && FD_ISSET(fd, &set_); }
	/* select() on them for ms: > 0 some are ready, 0 none (or a
	   signal), < 0 an error (errno) */
	int wait(long ms);

private:
	static volatile sig_atomic_t running_;
	fd_set set_;
	int max_ = -1;
};

} // namespace dmd

#endif
