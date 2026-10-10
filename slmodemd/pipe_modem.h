/*
 *
 *    pipe_modem.h - a modem whose line's audio comes from one file
 *                   descriptor and goes to another (16 bit linear), with
 *                   a pty for its DTE: modem_test puts two of them back
 *                   to back, the interop tests' modem_half one against a
 *                   modem in another process
 *
 *    slmodemd's modem_test.c and modem_half.c on the new core.
 *
 */
#ifndef SLMODEMD_PIPE_MODEM_H
#define SLMODEMD_PIPE_MODEM_H

#include <dcore.h>

#include "pty.h"

namespace slm {

class PipeModem final : public dcore::Host {
public:
	/* name: the modem's; in, out: its line */
	PipeModem(const char *name, int rate, const char *country, int in,
		  int out);
	~PipeModem();
	dcore::Modem &modem() { return *m_; }
	Pty &pty() { return pty_; }

	/* MODEM_REALTIME: no faster than the sample clock; MODEM_REC: the
	   line both ways into <prefix>.in and <prefix>.out */
	void set_realtime(bool on) { realtime_ = on; }
	void set_record(const char *prefix) { record_ = prefix; }

	/* the main loop of any number of them, until stop() */
	static int run(PipeModem **modems, int n);
	static void stop();

	/* Host */
	int start(dcore::Modem &m) override;
	int stop(dcore::Modem &m) override;
	int io_delay(dcore::Modem &) override { return delay_ / 2; }
	const char *driver_name() override { return "modem_test driver"; }
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		return pty_.write(buf, n);
	}
	unsigned long now_ms() override;

private:
	int line();             /* the line's audio is readable */
	void pace(int samples);

	dcore::Modem *m_ = nullptr;
	Pty pty_;
	int in_, out_;
	int rate_;
	int delay_ = 0;         /* bytes on their way */
	int pending_delay_ = 0; /* samples the pumps asked for, left */
	char pend_[8192];
	int npend_ = 0;
	bool realtime_ = false;
	const char *record_ = nullptr;
	void *rec_in_ = nullptr, *rec_out_ = nullptr;
	double clock_start_ = 0, clock_done_ = 0;
};

} // namespace slm

#endif
