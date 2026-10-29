/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    loopback.h - two of the open data pumps back to back through a
 *    simulated line (gain, delay, white noise, a tilt, a clock offset,
 *    near and far echoes), with a fake modem core: PRBS bits both ways,
 *    the bit errors of the data phase counted (loopback.cpp)
 */
#ifndef DMODEM_LOOPBACK_H
#define DMODEM_LOOPBACK_H

#include <string>

struct LoopbackConfig {
	int dp;                 /* the answerer's data pump (DP_ID) */
	int caller_dp = 0;      /* the caller's after the call progress, if
				   another (automode) */
	float snr = 40;         /* dB below the received signal */
	float secs = 20;
	int srate = 9600;
	float gain = 0.5f;      /* of the line */
	float ppm = 0;          /* sample clock offset, each way */
	float echo_db = 0;      /* < 0: each modem's own signal at its input
				   (hybrid echo, about 2 ms) */
	float far_db = 0;       /* < 0: the echo of the far end's hybrid */
	int far_delay = 0;      /* samples more for the far echo (VoIP) */
	int line_delay = 0;     /* samples more to the answerer (VoIP) */
	int line_delay_c = -1;  /* to the caller (< 0: line_delay) */
	bool flat = false;      /* no band limitation */
	float cut_s = 0;        /* > 0: what the pumps receive from then
				   on dropped (a call that stalls) */
	const char *codec = nullptr;    /* a speech codec each way on the
					   line (codec_sim.h's spec, at 8000
					   Hz alone), e.g. "amr:12.2" */
	const char *dump = nullptr;     /* what each side hears: <dump>.a
					   (answerer), <dump>.c (caller) */
};

struct LoopbackSide {
	bool connected = false;
	double connect_s = 0;   /* when */
	int tx_rate = 0, rx_rate = 0;
	long received = 0;      /* bits */
	/* of them, since the CONNECT; and what rx_rate carries from the
	   CONNECT to the end (the bits kept at most) */
	long carried = 0, expected = 0;
	long checked = 0, errors = 0, resyncs = 0;
	/* connected, CARRIED_MIN of the bits expected carried (a call
	   that stalls is no good however few errors its first bits have),
	   and a bit error rate of 1e-3 at most */
	bool ok() const;
	/* the side's result on one line, name first */
	std::string text(const char *name) const;
};

struct LoopbackResult {
	bool dp_failed = false; /* a pump ended with an error status */
	LoopbackSide side[2];   /* the caller, the answerer */
};

/* the caller from call progress on and the answerer at cfg.dp, back to
   back for cfg.secs of line time; their results */
LoopbackResult loopback_run(const LoopbackConfig &cfg);

#endif
