/*
 *
 *    script.h - the modem core as core_script drives it
 *               (script_core.cpp)
 *
 */
#ifndef CORE_SCRIPT_H
#define CORE_SCRIPT_H

#include <stdint.h>
#include <string>

struct Core {
	virtual ~Core() = default;
	/* the DTE's characters; returns how many the modem took */
	virtual int dte_in(const char *buf, int n) = 0;
	/* the room for them */
	virtual int dte_space() = 0;
	/* what the modem wrote to its DTE since the last call */
	virtual std::string dte_out() = 0;
	/* the line's audio runs */
	virtual bool started() = 0;
	/* n samples in and out, while started() */
	virtual void process(const int16_t *in, int16_t *out, int n) = 0;
	/* the timers and events */
	virtual void poll() = 0;
	/* a ring pulse, a ringing call (SIP), the program hangs up */
	virtual void ring() = 0;
	virtual void ring_signal(bool on) = 0;
	virtual void hangup() = 0;
	/* 0 idle, 1 connecting, 2 online, 3 hanging up */
	virtual int link_state() = 0;
	virtual unsigned rates() = 0;   /* tx_rate / 100 << 16 | rx_rate / 100 */
};

/* the pumps register (once); a modem at 8000 Hz on the clock *ms */
void core_init();
Core *core_create(const char *name, const unsigned long *ms);

#endif
