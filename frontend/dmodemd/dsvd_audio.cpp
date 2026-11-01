/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dsvd_audio.cpp - DSVD's voice through another program
 *                     (dsvd_audio.h)
 *
 */

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <log.h>

#include "dsvd_audio.h"

extern char **environ;

#define DBG(fmt, ...) dcore::log_printf(1, "dsvd: " fmt, ##__VA_ARGS__)

namespace dmd {

/* (see dsvd_audio.h) */
void DsvdAudio::state(bool on)
{
	if (on)
		start();
	else
		stop();
}

/* the command started on two pipes, ours not blocking; false (and
   nothing started) if it cannot be */
bool DsvdAudio::start()
{
	stop();
	int in[2], out[2];
	if (pipe(in) < 0)
		return false;
	if (pipe(out) < 0) {
		close(in[0]);
		close(in[1]);
		return false;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, in[0], 0);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	posix_spawn_file_actions_addclose(&fa, in[1]);
	posix_spawn_file_actions_addclose(&fa, out[0]);
	char sh[] = "/bin/sh", c[] = "-c";
	char *argv[] = { sh, c, const_cast<char *>(cmd_), nullptr };
	pid_t pid;
	const int err = posix_spawn(&pid, "/bin/sh", &fa, nullptr, argv,
				    environ);
	posix_spawn_file_actions_destroy(&fa);
	close(in[0]);
	close(out[1]);
	if (err) {
		dcore::log_printf(0, "dsvd: %s: %s\n", cmd_, strerror(err));
		close(in[1]);
		close(out[0]);
		return false;
	}
	pid_ = pid;
	to_ = in[1];
	from_ = out[0];
	odd_ = -1;
	for (int fd : { to_, from_ }) {
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	}
	DBG("voice through \"%s\" (%d)\n", cmd_, static_cast<int>(pid_));
	return true;
}

/* the pipes closed, the command ended (asked, then made to) */
void DsvdAudio::stop()
{
	if (to_ >= 0)
		close(to_);
	if (from_ >= 0)
		close(from_);
	to_ = from_ = -1;
	if (pid_ <= 0)
		return;
	kill(pid_, SIGTERM);
	int status;
	for (int i = 0; i < 50; i++) {
		if (waitpid(pid_, &status, WNOHANG) != 0) {
			pid_ = -1;
			return;
		}
		usleep(2000);
	}
	kill(pid_, SIGKILL);
	waitpid(pid_, &status, 0);
	pid_ = -1;
}

/* (see dsvd_audio.h) */
s16 DsvdAudio::mic(s16 *pcm, s16 n)
{
	if (from_ < 0 || n <= 0)
		return 0;
	unsigned char b[2 * 160 + 1];
	size_t want = 2 * static_cast<size_t>(n < 160 ? n : 160);
	size_t have = 0;
	if (odd_ >= 0) {
		b[have++] = static_cast<unsigned char>(odd_);
		want--;
	}
	const ssize_t r = read(from_, b + have, want);
	if (r > 0)
		have += static_cast<size_t>(r);
	s16 k = 0;
	for (size_t i = 0; i + 1 < have; i += 2)
		pcm[k++] = static_cast<s16>(b[i] | b[i + 1] << 8);
	odd_ = have & 1 ? b[have - 1] : -1;
	return k;
}

/* (see dsvd_audio.h) */
void DsvdAudio::ear(const s16 *pcm, s16 n)
{
	if (to_ < 0 || n <= 0)
		return;
	unsigned char b[2 * 160];
	if (n > 160)
		n = 160;
	for (s16 i = 0; i < n; i++) {
		b[2 * i] = static_cast<unsigned char>(pcm[i]);
		b[2 * i + 1] = static_cast<unsigned char>(pcm[i] >> 8);
	}
	/* (a pipe takes the whole of a write this small, or none of it) */
	if (write(to_, b, 2 * static_cast<size_t>(n)) < 0 && errno == EPIPE)
		stop();
}

} // namespace dmd
