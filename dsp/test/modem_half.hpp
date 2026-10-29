/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    modem_half.hpp - what the test peers' harnesses share (half_agere,
 *                     half_sm56, half_pctel, half_hsf, half_broadcom,
 *                     half_ess, half_intel, half_spandsp, half_hda): a
 *                     modem whose line's audio comes in on fd 3 and
 *                     goes out on fd 4 (16 bit samples), its DTE a pty
 *                     whose name it prints ("created <name>: <pty>"),
 *                     for interop_call
 *
 *    HalfModem is the program: its settings from the environment
 *    (<PREFIX>_DEBUG, _RATE, _SYNC, _SYNC_PHASE), the pty, the modem's
 *    start, then the line a sample at a time, the DTE served every 48
 *    samples.  A harness derives from it: start(), line_sample() (or
 *    line_block()) and dte_poll().  And the pty's buffering (Pty), the
 *    result codes' online state (Results), and the clear of the stack
 *    before a call into a closed module (stack_clear).
 *
 *    The hardware a harness emulates is in hw/, each a mixin the
 *    harness derives from as well (class Sm56 : public HalfModem, public
 *    Ac97Codec<Sm56>, public Ich<Sm56>, ...): its members named for it
 *    (ich_*, pci_*, ...), what it needs of the harness called on the
 *    harness (Host): dbg, log() and the line of a HalfModem, and what
 *    each says (a controller's codec, the port I/O of the traps).
 *
 *    hw/ac97_codec.hpp   an AC'97 modem codec's registers
 *    hw/ich.hpp          the Intel ICH AC'97 modem controller
 *    hw/pci.hpp          PCI's configuration space of one device
 *    hw/x86_traps.hpp    in, out, cli, sti, CRn of a driver's own code
 *    hw/tsc.hpp          rdtsc on the line's clock
 *    hw/codec_line.hpp   the line and a codec at a rate of its own
 *    hw/smartdaa.hpp     Conexant's SmartDAA (HSF)
 *    hw/si3034.hpp       Silicon Labs' Si3034 DAA (Broadcom)
 *    hw/bcm4212.hpp      Broadcom's BCM4212 modem chip
 *    hw/i537ep.hpp       Intel's 537EP modem chip
 *    hw/es2838.hpp       ESS's ES2838 modem chip
 *    hw/hda.hpp          an Intel HD Audio controller
 *    hw/si3054_hda.hpp   Silicon Labs' Si3054 HD-audio modem codec (HDA)
 *
 *    interop_windows.h is a Windows kernel for a WDM driver on such a
 *    harness.
 *
 */

#ifndef MODEM_HALF_HPP
#define MODEM_HALF_HPP

#include <stddef.h>
#include <stdint.h>

#include <functional>

/* printf's format checked by GCC and Clang (MSVC builds this header for
   hw_test, and has no such attribute) */
#ifdef __GNUC__
#define MODEM_HALF_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define MODEM_HALF_PRINTF(f, a)
#endif

namespace half {

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned int u32;

/*
 *    the stack before a call into a closed module
 *
 *    The modules read some of their stack before writing it: a status
 *    built there with values left unset (Agere's pumps' reports, SM56's
 *    dial response), locals of HSF's engine, a name copied there without
 *    its 0 (PCTel's +MS).  What they find is what the calls before left
 *    there, the harness's own among them (a read or write of the pty
 *    that the DTE's timing decides), so the same samples could give
 *    another call.  stack_clear() before each call into a module zeroes
 *    the 8 KB of stack below the caller's frame: the module finds the
 *    same there every time.  The modules ran on kernel stacks of 4 or 8
 *    KB, so their own frames lie in it (PCTel's calls go 988 octets deep
 *    at most); what goes deeper is the harness's, under them (SM56's port
 *    I/O signal frames, HSF's NVM file read).
 *
 *    HALF_STACK_FILL=<byte>: that instead of 0, to see whether a module's
 *    results depend on what it has not written.
 */
void stack_clear();

/*
 *    the DTE's side: a pty, raw and non-blocking (the line goes on
 *    whatever the DTE does)
 *
 *    It takes some 20 KB ahead of the DTE's reading: what a write()
 *    refused when the driver was late to read was lost, a block out of
 *    the middle of the data.  write() keeps it, in order, and flush(),
 *    each time round, writes it on as the pty takes it.  The modems'
 *    output is taken from them as before, so their own flow control has
 *    no part in it.  The other way, read() gives the modem what it takes
 *    of the DTE's input and keeps the rest for the next time, before it
 *    reads any more: a modem can take less than the room it gave (the
 *    SM56 modem does, with a driver that writes ahead of it).
 */
class Pty {
public:
	~Pty();
	/* a pty opened, raw and non-blocking; 0, or -1 with a message */
	int open();
	/* its fd */
	int fd() const { return fd_; }
	/* its name (the DTE's side) */
	const char *name() const { return name_; }

	/* n octets to the DTE, after what waits; what it does not take now
	   waits */
	void write(const void *p, size_t n);
	/* what waits to the DTE, as much as it takes */
	void flush();
	/* the DTE's input to the modem, up to room octets: put() takes what
	   it can of n at p and says how many; what it left waits for the
	   next call */
	void read(int room, const std::function<int(const void *p, int n)> &put);

private:
	int fd_ = -1;
	const char *name_ = "";
	byte *q_ = nullptr;             /* q_[head_ .. head_ + n_) waits */
	size_t head_ = 0, n_ = 0, size_ = 0;
	byte in_[256];                  /* the DTE's input the modem left */
	int in_head_ = 0, in_n_ = 0;
};

/* the modem's result codes as they go to the DTE: online from CONNECT,
   offline from NO CARRIER or OK (a UART without DCD does not tell) */
class Results {
public:
	void scan(byte c);
	void scan(const void *p, size_t n);
	/* online: CONNECT the last of them */
	bool online() const { return online_; }

private:
	unsigned at_[3] = {};
	bool online_ = false;
};

/*
 *    the program
 */
class HalfModem {
public:
	/* name for "created <name>:" and the log; prefix its settings'
	   (AGERE: AGERE_DEBUG, ...) */
	HalfModem(const char *name, const char *prefix);
	virtual ~HalfModem() = default;

	/* the settings, the pty, start(), "created ...", then the line from
	   fd 3 to fd 4 until it ends; main()'s result */
	int run();

	/* <PREFIX>_DEBUG: what to print; <PREFIX>_RATE: the line's sample
	   rate (8000); <PREFIX>_SYNC=<samples>: the DTE's input in command
	   state only every that many line samples (rounded up to 48), so
	   that a test's commands meet the line at the same samples every
	   time, at sample <PREFIX>_SYNC_PHASE of each period (0) */
	int dbg = 0;
	int line_rate = 8000;
	long sync_n = 0, sync_phase = 0;
	unsigned long long line_n = 0;  /* the line's samples so far */
	Pty pty;
	Results results;

	/* the program's name */
	const char *name() const { return name_; }
	/* getenv("<PREFIX>_<what>") */
	const char *env(const char *what) const;
	/* "<name>: ..." to stderr */
	void log(const char *fmt, ...) const MODEM_HALF_PRINTF(2, 3);
	/* ms of line time */
	unsigned long long ms() const { return line_n * 1000 / line_rate; }
	/* the DTE's input waits: with <PREFIX>_SYNC, in command state, but
	   at the period's sample (online: data goes in at once) */
	bool dte_waits(bool online) const
	{
		return sync_n && (long)(line_n % sync_n) != sync_phase && !online;
	}
	/* the same, online as the result codes say */
	bool dte_waits() const { return dte_waits(results.online()); }

protected:
	/* the modem up and its port open (the pty is there); 0, or -1 */
	virtual int start() = 0;
	/* one line sample each way; line_n counted */
	virtual short line_sample(short in);
	/* up to n line samples each way (to the next 48th at most): those
	   done; line_sample() each, until line_n comes to a 48th (a sleep of
	   the modem's can take it past the block's end), unless a harness
	   has its own */
	virtual int line_block(const short *in, short *out, int n);
	/* the DTE served: the modem's output to it, its input to the modem;
	   every 48 line samples */
	virtual void dte_poll() = 0;
	/* after each read of the line: the DTE too, without the sync */
	virtual void after_read();

private:
	const char *name_, *prefix_;
};

}

#endif
