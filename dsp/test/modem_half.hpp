/*
 *
 *    modem_half.hpp - what the test peers' harnesses share (half_agere,
 *                     half_sm56, half_pctel, half_hsf, half_broadcom,
 *                     half_spandsp): a modem whose line's audio comes in
 *                     on fd 3 and goes out on fd 4 (16 bit samples), its
 *                     DTE a pty whose name it prints ("created <name>:
 *                     <pty>"), for interop_call
 *
 *    HalfModem is the program: its settings from the environment
 *    (<PREFIX>_DEBUG, _RATE, _NOISE, _SYNC, _SYNC_PHASE), the pty, the
 *    modem's start, then the line a sample at a time, the DTE served
 *    every 48 samples.  A harness derives from it: start(),
 *    line_sample() (or line_block()) and dte_poll().
 *
 *    And what more than one of them emulates or needs: the pty's
 *    buffering (Pty), the result codes' online state (Results), a
 *    resampler between the line and a codec at a rate of its own
 *    (CodecLine), the Intel ICH AC'97 modem controller (Ich), PCI's
 *    configuration space of one device (PciConfig), the time stamp
 *    counter on the line's clock (Tsc), the x86 instructions a driver
 *    traps on (X86Traps), and the clear of the stack before a call into
 *    a closed module (stack_clear).  interop_windows.h is a Windows
 *    kernel on these, for a WDM driver.
 *
 */

#ifndef MODEM_HALF_HPP
#define MODEM_HALF_HPP

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include <functional>

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
	int fd() const { return fd_; }
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
	bool online() const { return online_; }

private:
	unsigned at_[3] = {};
	bool online_ = false;
};

/*
 *    the line and a codec at a rate of its own: a windowed sinc
 *    resampler each way, and the codec's input noise (rms noise_rms; 5 by
 *    default, about -76 dB of full scale): a line is never digitally
 *    silent, and the SM56 modem's answering automode needs it not to be
 */
class CodecLine {
public:
	float noise_rms = 5;

	/* one line sample: in (and the noise) resampled to the codec's
	   samples due within it, each one's ADC sample through codec(adc),
	   which gives the DAC's back; the line's sample out.  r is the
	   line's rate over the codec's */
	template <class F> short sample(short in, double r, F &&codec)
	{
		push(up_, in + noise());
		while (up_ph_ < 1) {
			short dac = codec(sat(at(up_, up_ph_, r > 1 ? 1 / r : 1)));
			push(down_, dac);
			up_ph_ += r;
		}
		up_ph_ -= 1;
		/* (the newest codec sample was at up_ph_ - r) */
		return sat(at(down_, 1 - up_ph_ / r, r < 1 ? r : 1));
	}

	/* x rounded and clipped to 16 bits */
	static short sat(double x);

private:
	enum { TAPS = 32 };
	float up_[TAPS] = {}, down_[TAPS] = {};  /* the newest last */
	double up_ph_ = 0;      /* the next codec sample's time after the
				   line's sample before the newest, in line
				   samples */
	u32 seed_ = 1;

	float noise();
	/* the signal of h at f (0..1 or a little over) after its sample
	   TAPS / 2 - 1, lowpass at cut (<= 1) of its Nyquist */
	static float at(const float *h, double f, double cut);
	/* x in as the newest */
	static void push(float *h, float x);
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
	CodecLine codec;                /* (its noise: <PREFIX>_NOISE) */

	const char *name() const { return name_; }
	/* getenv("<PREFIX>_<what>") */
	const char *env(const char *what) const;
	/* "<name>: ..." to stderr */
	void log(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
	/* ms of line time */
	unsigned long long ms() const { return line_n * 1000 / line_rate; }
	/* the DTE's input waits: with <PREFIX>_SYNC, in command state, but
	   at the period's sample (online: data goes in at once) */
	bool dte_waits(bool online) const
	{
		return sync_n && (long)(line_n % sync_n) != sync_phase && !online;
	}
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

/*
 *    the Intel ICH AC'97 modem controller (8086:2416), as the soft modems
 *    drive it: its codec's registers at NAMBAR, the harness's (the
 *    primary codec's at 0, a secondary's at 0x80); at NABMBAR its two bus
 *    masters, PCM in (to the modem) at 0x00 and PCM out at 0x10 (BDBAR,
 *    CIV, LVI, SR, PICB, CR), GLOB_CNT at 0x3c and GLOB_STA at 0x40.
 *    Each bus master walks its buffer descriptor list a sample at a time;
 *    a buffer done with IOC, or the last valid one, sets its status, and
 *    its interrupt is a status bit whose enable is set.
 */
class Ich {
public:
	struct BusMaster {
		u32 bdbar = 0;
		byte civ = 0, lvi = 0, cr = 0;
		word sr = SR_DCH, picb = 0;
		int pos = 0;            /* samples done in the current buffer */
		bool fresh = true;      /* not run since a reset */
	};
	enum {
		SR_DCH = 0x01, SR_CELV = 0x02, SR_LVBCI = 0x04, SR_BCIS = 0x08,
		SR_FIFOE = 0x10,
		CR_RPBM = 0x01, CR_RR = 0x02, CR_LVBIE = 0x04, CR_FEIE = 0x08,
		CR_IOCE = 0x10,
	};

	BusMaster pi, po;
	u32 glob_cnt = 0, glob_sta;

	/* m the harness (its log); nambar and nabmbar the ports of the two
	   BARs; secondary: the codec's registers at NAMBAR + 0x80 (and
	   GLOB_STA's primary and secondary ready), else at NAMBAR; bus: what
	   a bus address is short of the address (the SM56 module's kernel
	   puts memory at 0x02000000 on); resume: a run after a pause goes on
	   where the bus master stopped, and one halted at the last valid
	   buffer goes on when there is another (the HSF module pauses it to
	   move LVI on) */
	Ich(const class HalfModem &m, unsigned nambar, unsigned nabmbar,
	    bool secondary, uintptr_t bus, bool resume);
	const unsigned nambar, nabmbar;

	/* the codec's registers: the harness's */
	std::function<word(int r)> codec_read;
	std::function<void(int r, word v)> codec_write;

	/* a port read of size bytes at a: the codec's and the controller's
	   registers; false if a is not the controller's */
	bool io_in(unsigned a, int size, u32 *v);
	/* a port write */
	bool io_out(unsigned a, u32 v, int size);

	/* a codec sample through a running bus master: in to memory (PCM
	   in), or out of it (PCM out) */
	short sample(BusMaster &b, short in);
	/* b's interrupt; either's */
	bool irq(const BusMaster &b) const;
	bool irq() const { return irq(pi) || irq(po); }
	/* GLOB_STA, its PCM in and out interrupt bits as the bus masters
	   are */
	u32 status() const;
	/* a running bus master at the start of a buffer: none of it done */
	bool at_start(const BusMaster &b) const;

private:
	const HalfModem &m_;
	unsigned codec_at_;
	uintptr_t bus_;
	bool resume_;

	u32 *bd(const BusMaster &b, int i) const;
	void load(BusMaster &b);
	void control(BusMaster &b, byte v);
	const char *name(const BusMaster &b) const { return &b == &pi ? "pcm in" : "pcm out"; }
};

/*
 *    PCI's configuration space of one device on bus 0, and the ports of
 *    configuration mechanism 1 (0xcf8, 0xcfc)
 */
class PciConfig {
public:
	u32 cfg[64] = {};
	unsigned devfn = 0;
	/* the BARs' sizes (powers of 2): what a write of all ones reads
	   back */
	u32 bar_size[6] = {};
	/* not 0: function 0 of the slot is a multi-function bridge with
	   this ID (an ICH's LPC bridge; the SM56 module looks before it
	   reads function 6) */
	u32 fn0_id = 0;

	PciConfig(const class HalfModem &m) : m_(m) {}

	/* size bytes of register r; a write */
	u32 read(unsigned r, int size) const;
	void write(unsigned r, u32 v, int size);
	/* the configuration ports: false for another port */
	bool io_in(unsigned a, int size, u32 *v);
	bool io_out(unsigned a, u32 v, int size);

private:
	const HalfModem &m_;
	u32 cf8_ = 0;
};

/*
 *    the time stamp counter as a driver reads it (rdtsc, which traps:
 *    PR_TSC_SIGSEGV): the line's time at 1 GHz, monotonic, and on by 0.1
 *    us each read while the line stands still (so that a busy wait ends)
 */
class Tsc {
public:
	unsigned long long last = 0;    /* the last read */

	/* a read at line sample line_n, line_rate a second */
	unsigned long long read(unsigned long long line_n, int line_rate)
	{
		unsigned long long t = line_n * (1000000000ull / line_rate);
		if (t <= last)
			t = last + 100;
		return last = t;
	}
};

#if defined(__i386__)
/*
 *    the instructions a driver's code traps on, carried out and stepped
 *    over (from SIGSEGV's handler): in and out (no I/O permission), cli
 *    and sti, mov to and from control registers (CR0 with TS clear: the
 *    FPU ours to use; CR4 with OSFXSR), rdtsc (PR_TSC_SIGSEGV)
 */
class X86Traps {
public:
	std::function<u32(unsigned port, int size)> in;
	std::function<void(unsigned port, u32 v, int size)> out;
	std::function<unsigned long long()> tsc;

	/* the instruction at the context's EIP, if one of those: done, EIP
	   past it; false if not */
	bool step(ucontext_t *uc);

private:
	u32 cr_[8] = { 0x80050033, 0, 0, 0, 0x000006f0 };
};
#endif

}

#endif
