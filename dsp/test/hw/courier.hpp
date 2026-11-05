/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/courier.hpp - a whole USRobotics Courier V.Everything (external,
 *                     supervisor 7.3.14, DSP 3.0.13: IDSDL302.ROM): its
 *                     80C186EB with its integrated peripherals, the
 *                     ASIC between it and the DSP, the DSP and its
 *                     codec, the NVRAM; for half_courier
 *
 *    The board, as far as the firmware shows it (the facts mostly from
 *    the courier-emu project's notes, checked against the code):
 *
 *    - 80C186EB at 20.16 MHz (timers at 5.04 MHz).  128 KiB of RAM at 0,
 *      the 512 KiB flash at 0x80000.  The application moves the
 *      peripheral control block into memory at 0xff00 (RELREG 10ff).
 *    - The DTE is serial channel 0.  The supervisor finds the DTE's
 *      rate itself: INT1 is RXD's start bit, timer 1 times it, timer 2
 *      samples RXD on P2.5.  RTS is CTS0; DTR is ASIC port 0x12 bit 6.
 *    - The 5 ms tick is INT3, from timer 0's output (25200 counts).
 *    - The ASIC (GCS0, I/O 0x00-0x7f): watchdog (0), hook relay (0x10
 *      bit 0), lamps and the modem's RS-232 outputs, active low (0x14:
 *      CD bit 0, CTS bit 1), ring (0x14 bit 1 in, low), the board's ID
 *      straps (0x14 bit 3 in), the DIP switches (in, on low: 0x12 bits
 *      0-5 and 7 switches 3, 2, 5, 4, 10, 9 and 7, 0x14 bits 0 and 2
 *      switches 8 and 6; switch 1 is P2.6), DTR (0x12 bit 6 in, low),
 *      and the DSP's host interface: the mailbox (tag 0x58/0x5a, word
 *      0x5c/0x5e, status 0x1c, INT0 about every third sample), six data
 *      lanes (0x40 + 4i, high byte at + 2) with their flags (0x18 to
 *      the DSP, 0x1a from it), a stream word (0x60).  The DSP sees the
 *      same as its ports 0x50-0x6f (key = address & 0x2f): 0x07
 *      status, 0x06 lane flags, 0x08+i lanes, 0x0e tag, 0x0f word, 0x20
 *      stream.
 *    - The NVRAM is a 93C66 (256 words) on P1.5 (CS), P1.2 (SK), P2.7
 *      (data).
 *    - The DSP is reset by P1.1.
 *
 *    The DSP's downloads are short-cut: where the supervisor sends the
 *    resident image (8000:e47b) or an overlay (8000:e5da) through the
 *    lanes to the C51's loader, the image goes straight into the DSP's
 *    memory from the flash and the routine returns as if it had sent
 *    it.  (The C51's loader lives in its mask ROM, of which only part is
 *    known.)  The DSP then runs from 0x8000 to its main loop (0x80c8),
 *    past its wait for its serial port (0x8097) and its codec's set-up
 *    (0x814c).
 *
 *    Time: the line's codec samples drive it.  Each sample the CPU runs
 *    its share of instructions (3.4 M a second, the board's measured
 *    rate) and the timers their share of 5.04 MHz; the DSP runs up to
 *    20.16 M instructions a second, until its main loop has been round
 *    once since the sample's interrupt (it then waits for the next).
 *    The codec's rate is the DSP's choice (its selector at 0x8151: 7200,
 *    8210.53, 8400, 8968.42, 9600 or 10266.67 Hz).
 *
 */

#ifndef HW_COURIER_HPP
#define HW_COURIER_HPP

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "i80186.hpp"
#include "tms320c5x.hpp"

namespace half {

class Courier {
public:
	typedef uint8_t u8;
	typedef uint16_t u16;
	typedef uint32_t u32;

	I80186 cpu;
	C5x dsp;

	/* what to log: 1 the DSP's downloads and boots, 2 the DTE's UART,
	   4 interrupts, 8 the mailbox's messages, 16 the DSP's ports, 32
	   the DTE's start bits, 64 the interrupt controller's choices, 128
	   the ASIC's ports */
	int dbg = 0;
	/* to the DTE, a byte at a time */
	std::function<void(u8)> to_dte;
	/* the hook relay (true: off hook) */
	std::function<void(bool)> on_hook;
	/* a codec rate chosen (Hz) */
	std::function<void(double)> on_rate;
	std::function<void(const char *)> log;

	/* the DTE's lines */
	bool dtr = true, rts = true;
	/* ringing on the line */
	bool ring = false;
	bool off_hook = false;
	/* the DIP switches that are on, switch n in bit n - 1: by default
	   3 (result codes), 5 (no auto answer) and 8 (AT commands); off,
	   2 (verbal codes), 4 (echo), 6 (DCD the carrier's), 7 (codes when
	   answering as well), 10 (ATZ loads the NVRAM's profile) */
	unsigned dips = 0x094;

	static constexpr double CPU_IPS = 3.4e6;
	/* the DSP's instructions a second (its overruns counted) */
	void set_dsp_ips(double v) { dsp_ips_ = v; }
	unsigned long overruns = 0;
	static constexpr double TIMER_HZ = 5.04e6;
	static constexpr double DTE_BAUD = 115200;

	static constexpr double RATES[6] = { 7200.0, 8210.53, 8400.0,
		8968.42, 9600.0, 10266.67 };
	double rate() const { return RATES[rate_]; }

	bool load(const char *rom, const char *mask, const char *nvram,
		  std::string &err)
	{
		if (!slurp(rom, rom_) || rom_.size() != 0x80000) {
			err = std::string(rom) + ": not the Courier's 512 KiB image";
			return false;
		}
		std::vector<u8> m;
		if (!slurp(mask, m) || m.size() < 0x1000) {
			err = std::string(mask) + ": not the C51's mask ROM";
			return false;
		}
		for (unsigned i = 0; i < 0x800; i++)
			dsp.P[i] = (u16)(m[2 * i] | m[2 * i + 1] << 8);
		memset(eeprom_, 0xff, sizeof(eeprom_));
		if (nvram) {
			nvram_path_ = nvram;
			std::vector<u8> e;
			if (slurp(nvram, e) && e.size() == sizeof(eeprom_))
				memcpy(eeprom_, e.data(), sizeof(eeprom_));
		}
		ram_.assign(0x20000, 0);
		cpu.map(0, ram_.data(), 0xf000, false);
		cpu.map(0x10000, ram_.data() + 0x10000, 0x10000, false);
		cpu.map(0x80000, rom_.data(), 0x80000, true);
		cpu.mem_rd = [this](u32 a) { return mem_rd(a); };
		cpu.mem_wr = [this](u32 a, u8 v) { mem_wr(a, v); };
		cpu.io_rd = [this](u16 p, bool w) { return io_rd(p, w); };
		cpu.io_wr = [this](u16 p, u16 v, bool w) { io_wr(p, v, w); };
		dsp.io_rd = [this](unsigned k) { return dsp_rd(k); };
		dsp.io_wr = [this](unsigned k, u16 v) { dsp_wr(k, v); };
		shortcuts();
		reset();
		return true;
	}

	void reset()
	{
		cpu.reset();
		memset(pcb_, 0, sizeof(pcb_));
		pcb_[0xa8 / 2] = 0x00ff;
		pcb_[0x08 / 2] = 0x00fd;        /* IMASK: all masked */
		pcb_[0x0a / 2] = 7;
		for (int i = 0x12; i <= 0x1e; i += 2)
			pcb_[i / 2] = 0x000f;
		pcb_[0x66 / 2] = S_TXE;
		pcb_[0x76 / 2] = S_TXE;
		pcb_[0x56 / 2] = pcb_[0x5e / 2] = 0xff;
		pcb_[0x50 / 2] = pcb_[0x58 / 2] = 0xff;
		dsp_on_ = false;
	}

	/* the DTE's byte to the modem */
	void from_dte(u8 b) { rxq_.push_back(b); }
	/* the DTE's bytes not yet on RXD */
	size_t dte_queued() const { return rxq_.size(); }
	/* the modem's CTS to the DTE (the panel's lamp, low on) */
	bool cts() const { return !(asic_[0x14] & 0x02); }
	/* its DCD (the same) */
	bool dcd() const { return !(asic_[0x14] & 0x01); }

	/* one codec sample each way; the CPU and timers run up to it */
	short sample(short adc)
	{
		double per = 1.0 / rate();
		cpu_due_ += CPU_IPS * per;
		tim_due_ += TIMER_HZ * per;
		/* in slices, so the timers, the DTE's bits and the CPU keep
		   step */
		const int SL = 8;
		for (int s = 0; s < SL; s++) {
			double c = cpu_due_ / (SL - s), t = tim_due_ / (SL - s);
			run_cpu((long)c);
			cpu_due_ -= (long)c;
			timers((long)t);
			tim_due_ -= (long)t;
		}
		short dac = 0;
		if (dsp_on_) {
			if (!dsp_sample(adc, dac)) {
				dsp_dead_ = true;
				dsp_on_ = false;
				if (dsp.stopped)
					msg("the DSP stopped at %04x (op %04x)",
					    dsp.stop_pc, dsp.stop_op);
				else
					msg("the DSP is stuck at %04x", dsp.pc);
			}
		}
		/* the ASIC's doorbell: about Fs / 3 (2401 Hz measured) */
		if (++bell_ >= 3) {
			bell_ = 0;
			pin(4, true);   /* INT0, edge */
			pin(4, false);
		}
		return dac;
	}

	void save_nvram()
	{
		if (nvram_path_.empty() || !nvram_dirty_)
			return;
		FILE *f = fopen(nvram_path_.c_str(), "wb");
		if (f) {
			fwrite(eeprom_, 1, sizeof(eeprom_), f);
			fclose(f);
		}
		nvram_dirty_ = false;
	}

	void msg(const char *fmt, ...) const __attribute__((format(printf, 2, 3)))
	{
		char b[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(b, sizeof(b), fmt, ap);
		va_end(ap);
		if (log)
			log(b);
		else
			fprintf(stderr, "courier: %s\n", b);
	}

	/* state for a debugger's eye */
	unsigned long long samples = 0;

private:
	std::vector<u8> rom_, ram_;
	u16 pcb_[128];
	u8 pcb_lo_ = 0;
	u16 pcb_rd_ = 0;
	double cpu_due_ = 0, tim_due_ = 0;
	int bell_ = 0;
	int rate_ = 0;
	bool dsp_on_ = false, dsp_dead_ = false;
	std::string nvram_path_;
	bool nvram_dirty_ = false;

	/* ---- the interrupt control unit ---- */
	/* sources, by their bit in REQST/IMASK/INSERV: 0 timers, 2 serial,
	   3 INT4, 4 INT0, 5 INT1, 6 INT2, 7 INT3 */
	u16 req_ = 0, insrv_ = 0;
	u8 tmr_pending_ = 0;    /* which timers: bit n timer n */
	u8 ser_pending_ = 0;    /* bit 0 RX, bit 1 TX */
	bool pins_[8] = {};

	int con_of(int src) const
	{
		static const int off[8] = { 0x12, -1, 0x14, 0x16, 0x18, 0x1a,
			0x1c, 0x1e };
		return off[src] < 0 ? 0xf : pcb_[off[src] / 2];
	}
	bool level(int src) const { return src >= 3 && (con_of(src) & 0x10); }
	/* an external pin's level (INT0 4, INT1 5, INT2 6, INT3 7, INT4 3) */
	void pin(int src, bool v)
	{
		if (v && !pins_[src])
			req_ |= 1 << src;
		pins_[src] = v;
		if (level(src)) {
			if (v) req_ |= 1 << src; else req_ &= ~(1 << src);
		}
	}
	u16 imask() const
	{
		u16 m = 0;
		for (int s = 0; s < 8; s++)
			if (s != 1 && (con_of(s) & 8))
				m |= 1 << s;
		return m;
	}
	void set_imask(u16 v)
	{
		static const int off[8] = { 0x12, -1, 0x14, 0x16, 0x18, 0x1a,
			0x1c, 0x1e };
		for (int s = 0; s < 8; s++)
			if (off[s] >= 0) {
				u16 &c = pcb_[off[s] / 2];
				c = (u16)((c & ~8) | ((v >> s) & 1 ? 8 : 0));
			}
	}
	void icu()
	{
		if ((dbg & 64) && (req_ & 0x20)) {
			static int n;
			if (n++ < 20)
				msg("icu: if %d req %04x insrv %04x imask %04x primsk %04x at %04x:%04x halted %d",
				    !!(cpu.flags & I80186::IF), req_, insrv_, imask(), pcb_[0x0a / 2], cpu.sr[1], cpu.ip, cpu.halted);
		}
		if (!(cpu.flags & I80186::IF))
			return;
		u16 pend = req_ & ~imask();
		if (!pend)
			return;
		int best = -1, bp = 8;
		for (int s = 0; s < 8; s++)
			if (pend & (1 << s)) {
				int p = con_of(s) & 7;
				if (p < bp) { bp = p; best = s; }
			}
		if (best < 0 || bp > (pcb_[0x0a / 2] & 7))
			return;
		for (int s = 0; s < 8; s++)
			if ((insrv_ & (1 << s)) && (con_of(s) & 7) <= bp)
				return;
		/* the source's type; its request taken only once the CPU
		   takes the interrupt (it may not, between instructions
		   that inhibit it) */
		int type, bit = 0;
		switch (best) {
		case 0:
			bit = tmr_pending_ & 1 ? 1 : tmr_pending_ & 2 ? 2 : 4;
			type = bit == 1 ? 8 : bit == 2 ? 0x12 : 0x13;
			break;
		case 2:
			bit = ser_pending_ & 1 ? 1 : 2;
			type = bit == 1 ? 0x14 : 0x15;
			break;
		case 3: type = 0x11; break;
		default: type = 0x0c + (best - 4); break;
		}
		if (!cpu.irq((u8)type))
			return;
		if (best == 0) {
			tmr_pending_ &= ~bit;
			if (!tmr_pending_) req_ &= ~1;
		} else if (best == 2) {
			ser_pending_ &= ~bit;
			if (!ser_pending_) req_ &= ~4;
		} else if (!level(best)) {
			req_ &= ~(1 << best);
		}
		if ((dbg & 4) && type != 0x0c && type != 0x0f)
			msg("interrupt %02x to %04x:%04x", type, cpu.sr[I80186::CS], cpu.ip);
		insrv_ |= 1 << best;
	}
	void eoi(u16 v)
	{
		if (v & 0x8000) {
			int best = -1, bp = 8;
			for (int s = 0; s < 8; s++)
				if ((insrv_ & (1 << s)) && (con_of(s) & 7) < bp) {
					bp = con_of(s) & 7;
					best = s;
				}
			if (best >= 0)
				insrv_ &= ~(1 << best);
			return;
		}
		int t = v & 0x1f, s = -1;
		if (t == 8 || t == 0x12 || t == 0x13) s = 0;
		else if (t == 0x14 || t == 0x15) s = 2;
		else if (t == 0x11) s = 3;
		else if (t >= 0x0c && t <= 0x0f) s = 4 + (t - 0x0c);
		if (s >= 0)
			insrv_ &= ~(1 << s);
	}

	void run_cpu(long n)
	{
		for (long i = 0; i < n; i++) {
			icu();
			if (cpu.halted)
				return;
			cpu.step();
			if (cpu.stopped) {
				static bool said;
				if (!said) {
					said = true;
					msg("cpu stopped at %05x: opcode %02x",
					    cpu.stop_at, cpu.stop_op);
				}
				return;
			}
		}
	}

	/* ---- the timers ---- */
	double t_frac_[3] = {};
	void timers(long n)
	{
		/* timer 2 first: it may prescale the others */
		long t2_ends = tick_timer(2, n, 0);
		tick_timer(0, n, t2_ends);
		tick_timer(1, n, t2_ends);
		serial(n);
	}
	/* n counts of CLKOUT/4; how many times it reached its maximum */
	long tick_timer(int t, long n, long t2_ends)
	{
		static const int base[3] = { 0x30, 0x38, 0x40 };
		u16 &cnt = pcb_[base[t] / 2], &con = pcb_[(base[t] + 6) / 2];
		if (!(con & 0x8000))
			return 0;
		long steps = n;
		if (t < 2 && (con & 8))
			steps = t2_ends;
		/* timer 1's input gates it (EXT and RTG clear: it counts while
		   T1IN is high): on this board T1IN is RXD inverted, so timer 1
		   times the DTE's start bits (and a break), and never runs out
		   on an idle line */
		if (t == 1 && !(con & 0x14) && rxd_)
			steps = 0;
		if (t < 2 && (con & 4))
			steps = 0;      /* external clock: none modelled */
		long ends = 0;
		while (steps > 0) {
			bool useb = t < 2 && (con & 2) && (con & 0x1000);
			u16 max = useb ? pcb_[(base[t] + 4) / 2] : pcb_[(base[t] + 2) / 2];
			long room = (max ? max : 0x10000) - cnt;
			if (room <= 0) room = 1;
			if (steps < room) { cnt = (u16)(cnt + steps); break; }
			steps -= room;
			cnt = 0;
			ends++;
			con |= 0x20;    /* MC */
			if (t < 2 && (con & 2))
				con ^= 0x1000;  /* RIU */
			if (con & 0x2000) {
				tmr_pending_ |= 1 << t;
				req_ |= 1;
			}
			if (t == 0) {   /* T0OUT into INT3 */
				pin(7, true);
				pin(7, false);
			}
			if (!(con & 1)) {
				con &= ~0x8000;
				break;
			}
		}
		return ends;
	}

	/* ---- serial channel 0, the DTE ---- */
	/* S0STS's bits as the supervisor uses them (TXE 0x08 before each
	   byte, the two break bits together as 0x300) */
	enum { S_CTS = 0x02, S_OE = 0x04, S_TXE = 0x08, S_FE = 0x10,
	       S_TI = 0x20, S_RI = 0x40 };
	std::deque<u8> rxq_;
	/* the DTE's byte on RXD: its bits, start bit first, at DTE_BAUD */
	int rx_bit_ = -1;       /* -1 idle; 0 start, 1-8 data, 9 stop */
	double rx_t_ = 0;       /* counts into the bit */
	u8 rx_byte_ = 0;
	bool rxd_ = true;
	long tx_busy_ = 0;      /* counts until the byte has gone */
	long rx_gap_ = 0;

	void serial(long n)
	{
		double bit = TIMER_HZ / DTE_BAUD;
		/* the DTE's next byte onto RXD */
		long left = n;
		while (left > 0) {
			if (rx_bit_ < 0) {
				if (rxq_.empty() || rx_gap_ > 0) {
					if (rx_gap_ > 0) rx_gap_ -= left;
					break;
				}
				rx_byte_ = rxq_.front();
				rxq_.pop_front();
				rx_bit_ = 0;
				rx_t_ = 0;
				rxd_ = false;
				pin(5, true);   /* INT1: RXD's falling edge */
				pin(5, false);
				if (dbg & 32)
					msg("start bit: req %04x insrv %04x imask %04x if %d i1con %04x i3con %04x scu %04x tcu %04x i0 %04x cs:ip %04x:%04x",
					    req_, insrv_, imask(), !!(cpu.flags & 0x200), con_of(5), con_of(7), con_of(2), con_of(0), con_of(4), cpu.sr[1], cpu.ip);
			}
			double step = bit - rx_t_;
			if (step > left) { rx_t_ += left; break; }
			left -= (long)step;
			rx_t_ = 0;
			rx_bit_++;
			if (rx_bit_ <= 8)
				rxd_ = (rx_byte_ >> (rx_bit_ - 1)) & 1;
			else if (rx_bit_ == 9)
				rxd_ = true;
			else {
				rx_bit_ = -1;
				received(rx_byte_);
				rx_gap_ = (long)(bit * 2);
			}
		}
		if (tx_busy_ > 0) {
			tx_busy_ -= n;
			if (tx_busy_ <= 0) {
				u16 &sts = pcb_[0x66 / 2];
				sts |= S_TI | S_TXE;
				ser_pending_ |= 2;
				req_ |= 4;
			}
		}
	}
	void received(u8 b)
	{
		u16 con = pcb_[0x64 / 2];
		if ((dbg & 2))
			msg("DTE byte %02x, S0CON %04x B0CMP %04x I1CON %04x", b, con,
			    pcb_[0x60 / 2], pcb_[0x1a / 2]);
		if (!(con & 0x20))
			return;
		u16 &sts = pcb_[0x66 / 2];
		if (sts & S_RI)
			sts |= S_OE;
		u16 v = b;
		if ((con & 7) == 1 && (con & 8))
			v &= 0x7f;      /* 7 bits and parity */
		pcb_[0x68 / 2] = v;
		sts |= S_RI;
		ser_pending_ |= 1;
		req_ |= 4;
	}
	void transmit(u8 b)
	{
		u16 con = pcb_[0x64 / 2];
		if ((con & 7) == 1 && (con & 8))
			b &= 0x7f;
		if (to_dte)
			to_dte(b);
		pcb_[0x66 / 2] &= ~(S_TI | S_TXE);
		u16 br = pcb_[0x60 / 2] & 0x7fff;
		double baud = 20.16e6 / (8.0 * (br + 1));
		tx_busy_ = (long)(TIMER_HZ / baud * 10) + 1;
	}

	/* ---- the peripheral control block ---- */
	u16 pcb_read(unsigned off)
	{
		off &= 0xfe;
		u16 v = pcb_[off / 2];
		switch (off) {
		case 0x08: return imask();
		case 0x0c: return insrv_;
		case 0x0e: return req_;
		case 0x36: case 0x3e: case 0x46: return v & ~0x4000;
		case 0x52: return p1pin();
		case 0x5a: return p2pin();
		case 0x66: {
			u16 r = (u16)((v & ~S_CTS) | (rts ? S_CTS : 0));
			pcb_[0x66 / 2] &= S_TXE;        /* a read clears the rest */
			return r;
		}
		case 0x68: pcb_[0x66 / 2] &= ~S_RI; return v;
		}
		return v;
	}
	void pcb_write(unsigned off, u16 v)
	{
		off &= 0xfe;
		switch (off) {
		case 0x02: eoi(v); return;
		case 0x08: set_imask(v); return;
		case 0x0c: insrv_ = v; return;
		case 0x0e: return;
		case 0x36: case 0x3e: case 0x46: {
			u16 &c = pcb_[off / 2];
			u16 keep = c & 0x8000;
			c = (u16)(v & ~0x4000);
			if (!(v & 0x4000))
				c = (u16)((c & ~0x8000) | keep);
			return;
		}
		case 0x56: pcb_[off / 2] = v; p1_changed(); return;
		case 0x5e: pcb_[off / 2] = v; p2_changed(); return;
		case 0x58: pcb_[off / 2] = v; p2_changed(); return;
		case 0x6a: pcb_[off / 2] = v; transmit((u8)v); return;
		case 0x60: case 0x64:
			if ((dbg & 2))
				msg("%s = %04x", off == 0x60 ? "B0CMP" : "S0CON", v);
			break;
		case 0xa8:
			pcb_[off / 2] = v;
			return;
		}
		pcb_[off / 2] = v;
	}
	bool pcb_mem() const { return pcb_[0xa8 / 2] & 0x1000; }
	u32 pcb_base() const { return (u32)(pcb_[0xa8 / 2] & 0xff) << 8; }

	u8 mem_rd(u32 a)
	{
		if (pcb_mem() && a >= pcb_base() && a < pcb_base() + 0x100) {
			unsigned o = a - pcb_base();
			if (!(o & 1)) {
				pcb_rd_ = pcb_read(o);
				return (u8)pcb_rd_;
			}
			return (u8)(pcb_rd_ >> 8);
		}
		if (a < 0x20000)
			return ram_[a];
		return 0xff;
	}
	void mem_wr(u32 a, u8 v)
	{
		if (pcb_mem() && a >= pcb_base() && a < pcb_base() + 0x100) {
			unsigned o = a - pcb_base();
			if (!(o & 1))
				pcb_lo_ = v;
			else
				pcb_write(o & ~1u, (u16)(pcb_lo_ | v << 8));
			return;
		}
		if (a < 0x20000)
			ram_[a] = v;
	}

	/* ---- ports 1 and 2: the DSP's reset, the NVRAM ---- */
	u16 p1pin()
	{
		return pcb_[0x56 / 2] & 0xff;
	}
	u16 p2pin()
	{
		u16 v = pcb_[0x5e / 2] & 0xff;
		v = (u16)((v & ~0x20) | (rxd_ ? 0x20 : 0));
		/* DIP switch 1 (DTR forced on: low) */
		v = (u16)((v & ~0x40) | (dips & 1 ? 0 : 0x40));
		if (pcb_[0x58 / 2] & 0x80)      /* P2.7 an input: the NVRAM */
			v = (u16)((v & ~0x80) | (ee_do_ ? 0x80 : 0));
		return v;
	}
	bool p1_dsp_ = false;
	void p1_changed()
	{
		u8 l = pcb_[0x56 / 2] & 0xff;
		bool run = l & 2;
		if (run != p1_dsp_) {
			p1_dsp_ = run;
			if (!run)
				dsp_on_ = false;
		}
		ee_clock(l & 0x20, l & 4, (pcb_[0x5e / 2] & 0x80) != 0);
	}
	void p2_changed()
	{
		u8 l = pcb_[0x56 / 2] & 0xff;
		ee_clock(l & 0x20, l & 4, (pcb_[0x5e / 2] & 0x80) != 0);
	}

	/* the 93C66 (x16: 256 words, 8 address bits) */
	u8 eeprom_[512];        /* 256 words, each low byte first */
	bool ee_cs_ = false, ee_sk_ = false, ee_do_ = true, ee_ewen_ = false;
	unsigned ee_sr_ = 0;
	int ee_n_ = 0;          /* bits in since start */
	int ee_out_ = -1;       /* bits left to shift out */
	u16 ee_word_ = 0;
	int ee_wr_ = -1;        /* the address of a write in progress */
	int ee_op_ = 0;
	void ee_clock(bool cs, bool sk, bool di)
	{
		if (!cs) {
			ee_cs_ = false;
			ee_n_ = 0;
			ee_sr_ = 0;
			ee_out_ = -1;
			ee_do_ = true;          /* ready */
			ee_sk_ = sk;
			return;
		}
		if (!ee_cs_) {
			ee_cs_ = true;
			ee_n_ = 0;
			ee_sr_ = 0;
			ee_do_ = true;
		}
		bool rise = sk && !ee_sk_;
		ee_sk_ = sk;
		if (!rise)
			return;
		if (ee_out_ > 0) {
			ee_out_--;
			ee_do_ = (ee_word_ >> ee_out_) & 1;
			return;
		}
		if (ee_n_ == 0 && !di)
			return;         /* waiting for the start bit */
		ee_sr_ = (ee_sr_ << 1) | (di ? 1 : 0);
		ee_n_++;
		if (ee_wr_ >= 0) {
			if (ee_n_ == 11 + 16) {
				u16 w = ee_sr_ & 0xffff;
				eeprom_[ee_wr_ * 2] = (u8)w;
				eeprom_[ee_wr_ * 2 + 1] = (u8)(w >> 8);
				nvram_dirty_ = true;
				ee_wr_ = -1;
				ee_n_ = 0;
				ee_do_ = true;
			}
			return;
		}
		if (ee_n_ == 11) {      /* start, 2 op bits, 8 address bits */
			int op = (ee_sr_ >> 8) & 3, ad = ee_sr_ & 0xff;
			switch (op) {
			case 2:         /* READ: a dummy 0, then 16 bits */
				ee_word_ = (u16)(eeprom_[ad * 2] | eeprom_[ad * 2 + 1] << 8);
				ee_do_ = false;
				ee_out_ = 16;
				break;
			case 1:         /* WRITE */
				if (ee_ewen_) ee_wr_ = ad; else ee_n_ = 0;
				break;
			case 3:         /* ERASE */
				if (ee_ewen_) {
					eeprom_[ad * 2] = eeprom_[ad * 2 + 1] = 0xff;
					nvram_dirty_ = true;
				}
				ee_n_ = 0;
				break;
			case 0:
				switch (ad >> 6) {
				case 3: ee_ewen_ = true; break;
				case 0: ee_ewen_ = false; break;
				case 2:
					if (ee_ewen_) {
						memset(eeprom_, 0xff, sizeof(eeprom_));
						nvram_dirty_ = true;
					}
					break;
				}
				ee_n_ = 0;
				break;
			}
		}
	}

	/* ---- the ASIC: the CPU's side ---- */
	u8 asic_[0x80] = {};
	/* the DSP's host interface: latches each way, by key */
	u16 to_dsp_[0x30] = {}, from_dsp_[0x30] = {};
	bool msg_in_ = false;           /* a message waits for the DSP */
	bool msg_out_ = false;          /* the DSP's waits for the CPU */
	bool stream_ = false;
	u8 lane_in_ = 0;                /* lanes holding a word for the DSP */
	u8 lane_out_ = 0;               /* lanes holding the DSP's word */

	static int lane_key(unsigned p) { return (int)((0xff58 + ((p - 0x40) >> 2)) & 0x2f); }

	u16 io_rd(u16 port, bool word)
	{
		if (word)
			return (u16)(io_rd8(port) | io_rd8((u16)(port + 1)) << 8);
		return io_rd8(port);
	}
	void io_wr(u16 port, u16 v, bool word)
	{
		io_wr8(port, (u8)v);
		if (word)
			io_wr8((u16)(port + 1), (u8)(v >> 8));
	}
	u8 io_rd8(u16 p)
	{
		u8 v = io_rd8_(p);
		if ((dbg & 128) && p < 0x40)
			msg("in %02x = %02x at %04x:%04x", p, v, cpu.sr[1], cpu.ip);
		return v;
	}
	u8 io_rd8_(u16 p)
	{
		if (!pcb_mem() && p >= pcb_base() && p < pcb_base() + 0x100) {
			unsigned o = p - pcb_base();
			if (!(o & 1)) {
				pcb_rd_ = pcb_read(o);
				return (u8)pcb_rd_;
			}
			return (u8)(pcb_rd_ >> 8);
		}
		if (p >= 0x80)
			return (u8)p;
		if (p & 1)
			return 0;
		switch (p) {
		case 0x00: return 0;
		case 0x0e: return (u8)((asic_[0x0e] & ~0x18) | 0x08);
		case 0x12: {
			/* DIP switches 2-5, 7, 9 and 10 (on: low), DTR */
			static const signed char sw[8] = { 3, 2, 5, 4, 10, 9,
				-1, 7 };
			u8 v = 0;
			for (int b = 0; b < 8; b++)
				if (sw[b] > 0 && !(dips >> (sw[b] - 1) & 1))
					v |= (u8)(1 << b);
			return (u8)(v | (dtr ? 0 : 0x40));
		}
		case 0x14: {
			/* DIP switches 8 (bit 0) and 6 (bit 2), on: low; ring
			   (bit 1, low); bit 3 senses the strap matrix: the
			   lines the CPU drives low (0x14 bits 4-6, 0x12 bit 1)
			   pull it low through a strap.  The board's ID, 7 (the
			   external model), is the strap on 0x14 bit 6 */
			u8 v = 0x7a | (dips >> 7 & 1 ? 0 : 0x01) |
				(dips >> 5 & 1 ? 0 : 0x04);
			if (ring) v &= ~0x02;
			if (!(asic_[0x14] & 0x40))
				v &= ~0x08;
			return v;
		}
		case 0x18: return (u8)(~lane_in_ & 0x3f) | 0xc0;
		case 0x1a: return lane_out_;
		case 0x1c: return (u8)((msg_in_ ? 0 : 1) | (msg_out_ ? 2 : 0) |
				       (stream_ ? 4 : 0) | 0xf8);
		case 0x1e: return 0xff;
		}
		if (p >= 0x40 && p < 0x64) {
			int k = lane_key(p);
			u16 w = from_dsp_[k];
			if (k == 0x20 && (p & 2))
				stream_ = false;
			return (u8)((p & 2) ? w >> 8 : w);
		}
		return asic_[p];
	}
	void io_wr8(u16 p, u8 v)
	{
		if (!pcb_mem() && p >= pcb_base() && p < pcb_base() + 0x100) {
			unsigned o = p - pcb_base();
			if (!(o & 1))
				pcb_lo_ = v;
			else
				pcb_write(o & ~1u, (u16)(pcb_lo_ | v << 8));
			return;
		}
		/* the boot block's PCB writes are byte-wide outs to even
		   ports with AX: take a word out to an even PCB port too */
		if (p >= 0x80 || (p & 1))
			return;
		switch (p) {
		case 0x10: {
			bool oh = v & 1;
			if (oh != off_hook) {
				off_hook = oh;
				if (on_hook) on_hook(oh);
			}
			break;
		}
		case 0x18: lane_in_ |= v & 0x3f; break;
		case 0x1a: lane_out_ &= ~(v & 0x3f); break;
		case 0x1c:
			if (v & 1) {
				msg_in_ = true;
				if (dbg & 8)
					msg("to dsp %02x %04x", to_dsp_[0x0e],
					    to_dsp_[0x0f]);
			}
			if (v & 2) msg_out_ = false;
			if (v & 4) stream_ = false;
			break;
		}
		if (p >= 0x40 && p < 0x64) {
			int k = lane_key(p);
			u16 &w = to_dsp_[k];
			w = (p & 2) ? (u16)((w & 0xff) | v << 8) : (u16)((w & 0xff00) | v);
		}
		asic_[p] = v;
	}

	/* ---- the ASIC: the DSP's side ---- */
	u16 dsp_rd(unsigned k)
	{
		if ((dbg & 16) && k != 6 && k != 7)
			msg("dsp rd %02x at %04x", k, dsp.pc);
		switch (k) {
		case 0x07:
			return (u16)((msg_in_ ? 1 : 0) | (msg_out_ ? 0 : 2));
		case 0x06:
			return (u16)(lane_in_ | ((~lane_out_ & 0x3f) << 8));
		}
		return to_dsp_[k];
	}
	void dsp_wr(unsigned k, u16 v)
	{
		if (dbg & 16)
			msg("dsp wr %02x %04x at %04x", k, v, dsp.pc);
		switch (k) {
		case 0x07:
			if (v == 0xffff) {      /* the resident's start */
				msg_in_ = msg_out_ = false;
				return;
			}
			if (v & 1) msg_in_ = false;
			if (v & 2) {
				msg_out_ = true;
				if (dbg & 8)
					msg("from dsp %02x %04x", from_dsp_[0x0e],
					    from_dsp_[0x0f]);
			}
			if (v & 4) stream_ = true;
			return;
		case 0x06:
			if (v == 0xffff) {
				lane_in_ = lane_out_ = 0;
				return;
			}
			lane_in_ &= ~(v & 0x3f);
			lane_out_ |= (v >> 8) & 0x3f;
			return;
		}
		from_dsp_[k] = v;
	}

	/* ---- the DSP ---- */
	void dsp_image(int id)
	{
		static const u32 seg[9] = { 0, 0, 0, 0, 0, 0xa908, 0xb690,
			0xbc2f, 0xbfd9 };
		u32 row = 0xe711 + 6 * id;
		u16 s = rd16rom(row), e = rd16rom(row + 2), dst = rd16rom(row + 4);
		u32 src = seg[id] * 16 - 0x80000 + s;
		unsigned n = (u16)(e - s) / 2;
		for (unsigned i = 0; i < n; i++)
			dsp.D[(u16)(dst + i)] = (u16)(rom_[src + 2 * i] |
						     rom_[src + 2 * i + 1] << 8);
		if (dbg & 1)
			msg("dsp image %d: %u words at %04x", id, n, dst);
	}
	u16 rd16rom(u32 o) { return (u16)(rom_[o] | rom_[o + 1] << 8); }

	void dsp_boot()
	{
		C5x &c = dsp;
		c.acc = c.accb = c.p = 0;
		c.c = c.ov = c.tc = 0; c.sxm = 1; c.ovm = 0; c.pm = 0; c.cnf = 0;
		c.dp = 0; c.arp = 0; c.arb = 0;
		while (c.depth()) c.pop();
		c.stopped = false;
		c.pc = 0x8000;
		c.hook(0x8097, [](C5x &x) { x.pc = 0x809a; });
		c.hook(0x814c, [](C5x &x) { x.D[0x6b] = 0; });
		for (long n = 0; c.pc != 0x80c8 && n < 500000 && !c.stopped; n++)
			c.step();
		c.unhook(0x814c);
		c.hook(0x8151, [this](C5x &x) {
			int i = x.acc & 0xffff;
			if (i < 6 && i != rate_) {
				rate_ = i;
				if (on_rate) on_rate(RATES[i]);
			}
		});
		dsp_on_ = c.pc == 0x80c8 && !c.stopped;
		dsp_dead_ = false;
		msg_in_ = msg_out_ = stream_ = false;
		lane_in_ = lane_out_ = 0;
		if (!dsp_on_)
			msg("the DSP did not boot (at %04x)", c.pc);
		else if (dbg & 1)
			msg("the DSP is up");
	}
	/* one codec frame: the DSP runs its share of instructions (it
	   waits at 0x80d2, or in IDLE, for the frame's interrupt), then the
	   frame's interrupt (XINT, its vector in @65) takes the ADC's
	   sample from DRR (0x20) and leaves the DAC's in DXR (0x21) */
	double dsp_ips_ = 20.16e6;
	double dsp_due_ = 0;
	bool frame_ = false;
	bool passed_ = true;     /* the main loop round since the frame */
	bool dsp_sample(short adc, short &dac)
	{
		C5x &c = dsp;
		dac = (short)c.D[0x21];
		c.D[0x20] = (u16)adc;
		if (frame_ && ++overruns <= 10 && (dbg & 1))
			msg("the DSP missed a frame at %04x", c.pc);
		frame_ = true;
		dsp_due_ += dsp_ips_ / rate();
		while (dsp_due_ > 0) {
			if (frame_ && !c.intm) {
				frame_ = false;
				passed_ = false;
				c.intr(c.D[0x65], c.pc);
			}
			/* the main loop's end (0x80d2, TC: nothing more to
			   do) once it has been round since the frame came */
			if (c.idle || (c.pc == 0x80d2 && c.tc && passed_)) {
				if (frame_)
					return !c.stopped;      /* still masked */
				break;                          /* waits */
			}
			if (c.pc == 0x80d2)
				passed_ = true;
			c.step();
			dsp_due_ -= 1;
			if (c.stopped)
				return false;
		}
		if (dsp_due_ > 0)
			dsp_due_ = 0;   /* waited: no credit */
		return true;
	}

	/* the supervisor's DSP downloads, short-cut */
	void ret_near(bool cf)
	{
		cpu.ip = cpu.rd16(((u32)cpu.sr[I80186::SS] << 4) + cpu.r[I80186::SP]);
		cpu.r[I80186::SP] += 2;
		if (cf) cpu.flags |= I80186::CF; else cpu.flags &= ~I80186::CF;
	}
	void push16(u16 v)
	{
		cpu.r[I80186::SP] -= 2;
		cpu.wr16(((u32)cpu.sr[I80186::SS] << 4) + cpu.r[I80186::SP], v);
	}
	void shortcuts()
	{
		/* timer 1's handler while the supervisor waits for the DTE's
		   first start bit (9ea4:075d) re-arms the wait with a near
		   call of 9ea4:0133, which returns far: called so, it would
		   return to the CS its PUSHA left (into RAM at 0x3591).  The
		   call made far, as the routine expects */
		cpu.hook(0x9f19e, [this](I80186 &c) {
			push16(c.sr[I80186::CS]);
			push16(0x0761);
			c.ip = 0x0133;
		});
		/* e370: the boot word; e3aa: reset and the loader's start */
		cpu.hook(0x8e370, [this](I80186 &) { ret_near(false); });
		cpu.hook(0x8e3aa, [this](I80186 &) { dsp_on_ = false; ret_near(false); });
		/* e47b: the resident image; then the DSP runs it */
		cpu.hook(0x8e47b, [this](I80186 &) {
			dsp_image(5);
			dsp_boot();
			ret_near(false);
		});
		/* e598: the checksum again, the resident restarted */
		cpu.hook(0x8e598, [this](I80186 &) {
			dsp_boot();
			ret_near(false);
		});
		/* e5da: the overlay [0xe3c] (6 brings 8 with it) */
		cpu.hook(0x8e5da, [this](I80186 &c) {
			u32 a = ((u32)c.sr[I80186::DS] << 4) + 0xe3c;
			int id = c.rd8(a) & 0x7f;
			if (id == 6) { dsp_image(6); dsp_image(8); }
			else if (id >= 5 && id <= 8) dsp_image(id);
			c.wr8(a, 0);
			ret_near(false);
		});
	}

	static bool slurp(const char *path, std::vector<u8> &v)
	{
		FILE *f = fopen(path, "rb");
		if (!f)
			return false;
		u8 buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			v.insert(v.end(), buf, buf + n);
		fclose(f);
		return true;
	}
};

}

#endif
