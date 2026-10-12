/*
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
	bool flat = false;      /* no band limitation */
	const char *dump = nullptr;     /* what each side hears: <dump>.a
					   (answerer), <dump>.c (caller) */
};

struct LoopbackSide {
	bool connected = false;
	double connect_s = 0;   /* when */
	int tx_rate = 0, rx_rate = 0;
	long received = 0;      /* bits */
	long checked = 0, errors = 0, resyncs = 0;
	/* connected, and a bit error rate of 1e-3 at most */
	bool ok() const;
	std::string text(const char *name) const;
};

struct LoopbackResult {
	bool dp_failed = false; /* a pump ended with an error status */
	LoopbackSide side[2];   /* the caller, the answerer */
};

LoopbackResult loopback_run(const LoopbackConfig &cfg);

#endif
