/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/tms320c5x.hpp - a TMS320C5x (C50/C51) DSP core, enough of it to
 *                       run the Courier's DSP firmware (half_courier)
 *
 *    The instruction set from TI's C5x User's Guide (SPRU056D).  Memory
 *    is the Courier's: data 64 K words, program below 0x0800 the mask
 *    ROM, program 0x2000 to 0x23ff the C51's single-access RAM (data
 *    0x0800 to 0x0bff: the firmware sets RAM and OVLY in PMST),
 *    program 0x2c00 to 0x7fff the same RAM as data 0xac00 to 0xffff,
 *    the rest of program the same words as data (the board's external
 *    RAM is one array, the overlays loaded into it as data).
 *    With CNF set program 0xfe00 to 0xffff is block B0 (data 0x100).
 *
 *    What is not modelled: the serial ports, the timer, wait states,
 *    pipeline effects other than the delayed branches' two words, BIO
 *    (never low).  The harness gives the codec's samples by an
 *    interrupt of its own (intr()), and the host interface's registers
 *    through io_rd and io_wr (data 0x50-0x5f and 0xff50-0xff6f, I/O
 *    ports 0x50-0x6f), or by hooks on program addresses (hook()).
 *
 *    TRM = 0 (PMST bit 0): an instruction's load of TREG0 sets TREG1
 *    and TREG2 too, a store to it as memory (SAMM) does not; the
 *    firmware's AGC (resident 0x8847) depends on that.
 *
 */

#ifndef HW_TMS320C5X_HPP
#define HW_TMS320C5X_HPP

#include <stdint.h>
#include <string.h>

#include <functional>
#include <vector>

namespace half {

class C5x {
public:
	typedef uint16_t w16;
	typedef uint32_t u32;

	w16 P[0x800];                   /* the mask ROM */
	std::vector<w16> D = std::vector<w16>(0x10000);
	u32 acc = 0, accb = 0, p = 0;
	int c = 0, ov = 0, tc = 0, sxm = 1, ovm = 0, pm = 1, cnf = 0;
	int dp = 0, arp = 1, arb = 0;
	int intm = 1;                   /* interrupts disabled */
	bool idle = false;              /* IDLE: until an interrupt */
	w16 pc = 0;
	unsigned long long steps = 0;
	/* an unknown instruction: its address, the core stopped */
	bool stopped = false;
	unsigned stop_op = 0, stop_pc = 0;

	/* memory cleared, no hooks */
	C5x() { memset(P, 0, sizeof(P)); hooks_.resize(0x10000); }

	/* fn before the instruction at a runs (it can move pc) */
	void hook(unsigned a, std::function<void(C5x &)> fn)
	{
		hooks_[a & 0xffff] = std::move(fn);
	}
	/* debugging: fn before every instruction */
	std::function<void(C5x &)> on_step;
	/* the hook at a removed */
	void unhook(unsigned a) { hooks_[a & 0xffff] = nullptr; }

	/* an RPT's count is set and its instruction not yet run */
	bool rpt_pending() const { return rptc_ >= 0; }
	/* the hardware stack, 8 deep */
	w16 top() const { return sp_ ? stack_[sp_ - 1] : 0; }
	/* the number of words on the stack */
	int depth() const { return sp_; }
	/* a word pushed (the bottom one lost when full) */
	void push(u32 v)
	{
		if (sp_ == 8) {
			memmove(stack_, stack_ + 1, 7 * sizeof(w16));
			sp_ = 7;
		}
		stack_[sp_++] = (w16)v;
	}
	/* a word popped (0 if empty) */
	w16 pop() { return sp_ ? stack_[--sp_] : 0; }

	/* program memory */
	w16 pr(u32 a) const
	{
		a &= 0xffff;
		if (cnf && a >= 0xfe00)
			return D[a - 0xfe00 + 0x100];
		if (a < 0x800)
			return P[a];
		if (a >= 0x2000 && a < 0x2400)
			return D[a - 0x2000 + 0x800];
		if (a >= 0x2c00 && a < 0x8000)
			a += 0x8000;
		return D[a];
	}
	/* a program memory word written (the ROM not) */
	void pw(u32 a, u32 v)
	{
		a &= 0xffff;
		if (a < 0x800)
			return;
		if (a >= 0x2000 && a < 0x2400)
			a = a - 0x2000 + 0x800;
		else if (a >= 0x2c00 && a < 0x8000)
			a += 0x8000;
		D[a] = (w16)v;
	}
	/* the host interface's registers, if the harness models them: data
	   0x50-0x5f and 0xff50-0xff6f, and I/O ports 0x50-0x6f (the ASIC
	   decodes A0-A3 and A5: key = address & 0x2f, data 0x5n key n) */
	std::function<w16(unsigned key)> io_rd;
	std::function<void(unsigned key, w16 v)> io_wr;
	/* a data address's port key for io_rd and io_wr, or -1 */
	static int io_key(u32 a)
	{
		if (a >= 0x50 && a < 0x60)
			return a & 0x0f;
		if (a >= 0xff50 && a < 0xff70)
			return a & 0x2f;
		return -1;
	}
	/* a data memory word read (the ports through io_rd) */
	w16 rdm(u32 a)
	{
		a &= 0xffff;
		if (io_rd) {
			int k = io_key(a);
			if (k >= 0)
				return io_rd(k);
		}
		return D[a];
	}

	/* debugging: watch_fn(pc) on each store to data address watch */
	long watch = -1;
	std::function<void(C5x &, w16 v)> watch_fn;

	/* a data memory word written (the ports through io_wr, watch noted)
	   */
	void wr(u32 a, u32 v)
	{
		a &= 0xffff;
		if (io_wr) {
			int k = io_key(a);
			if (k >= 0) {
				io_wr(k, (w16)v);
				return;
			}
		}
		D[a] = (w16)v;
		if ((long)a == watch && watch_fn)
			watch_fn(*this, (w16)v);
		if (a == 0x10)          /* NDX = 0: AR0 is INDX and ARCR too */
			D[0x18] = D[0x19] = (w16)v;
	}
	/* auxiliary register n */
	w16 ar(int n) const { return D[0x10 + n]; }
	/* auxiliary register n written */
	void setar(int n, u32 v) { wr(0x10 + n, v); }
	/* TREG0 */
	w16 T() const { return D[0x0c]; }

	/* an interrupt (the codec's): the registers saved for RETE, the
	   return address pushed, pc at the vector */
	void intr(unsigned vector, unsigned ret)
	{
		shadow_save();
		push(ret);
		pc = (w16)vector;
		intm = 1;
		idle = false;
	}

	/* one instruction (with its repeats) */
	void step()
	{
		w16 at = pc;
		if (on_step)
			on_step(*this);
		if (hooks_[at]) {
			hooks_[at](*this);
			if (pc != at)
				return;
		}
		w16 op = pr(at);
		w16 nxt = (w16)(at + size(op));
		int j = exe(op, at);
		if (j >= 0 && (j & DELAYED)) {
			pc = nxt;
			w16 end = (w16)(nxt + 2);
			while (pc < end) {
				w16 p2 = pc, o2 = pr(p2);
				exe(o2, p2);
				pc = (w16)(p2 + size(o2));
			}
			pc = (w16)j;
		} else if (j >= 0) {
			pc = (w16)j;
		} else {
			pc = nxt;
		}
		if (rptb_on_ && j < 0 && at <= rptb_end_ && rptb_end_ < pc) {
			if (D[0x09] > 0) {
				D[0x09]--;
				pc = rptb_start_;
			} else {
				rptb_on_ = false;
			}
		}
		steps++;
	}

	/* ST0, ST1 */
	w16 st0() const
	{
		return (w16)((arp << 13) | (ov << 12) | (ovm << 11) | (intm << 9) |
			     (dp & 0x1ff));
	}
	/* ST1 as stored */
	w16 st1() const
	{
		return (w16)((arb << 13) | (cnf << 12) | (tc << 11) |
			     (sxm << 10) | (c << 9) | 0x1f0 | pm);
	}
	/* ST0 loaded */
	void lst0(w16 v)
	{
		arp = v >> 13; ov = (v >> 12) & 1; ovm = (v >> 11) & 1;
		dp = v & 0x1ff;
	}
	/* ST1 loaded (ARB into ARP too) */
	void lst1(w16 v)
	{
		arb = v >> 13; arp = arb; cnf = (v >> 12) & 1;
		tc = (v >> 11) & 1; sxm = (v >> 10) & 1; c = (v >> 9) & 1;
		pm = v & 3;
	}

private:
	enum { DELAYED = 0x10000 };
	static const u32 M32 = 0xffffffffu;

	std::vector<std::function<void(C5x &)>> hooks_;
	w16 stack_[8] = {};
	int sp_ = 0;
	int rptc_ = -1;
	bool rptb_on_ = false;
	w16 rptb_end_ = 0, rptb_start_ = 0;

	struct Shadow {
		u32 acc, accb, p;
		w16 st0, st1, pmst, t0, t1, t2, indx, arcr, brcr;
	};
	Shadow shadow_[8];
	int nshadow_ = 0;

	/* a 32 bit value as signed */
	static int32_t s32(u32 v) { return (int32_t)v; }
	/* a 16 bit value as signed */
	static int s16(u32 v) { return (int16_t)(w16)v; }

	/* TREG0 written, and TREG1 and TREG2 with it unless TRM */
	void setT(u32 v)
	{
		D[0x0c] = (w16)v;
		if (!(D[0x07] & 1)) {
			D[0x0d] = v & 0x1f;
			D[0x0e] = v & 0xf;
		}
	}

	/* the registers an interrupt saves, onto the shadow stack */
	void shadow_save()
	{
		if (nshadow_ == 8) {
			memmove(shadow_, shadow_ + 1, 7 * sizeof(Shadow));
			nshadow_ = 7;
		}
		Shadow &s = shadow_[nshadow_++];
		s = { acc, accb, p, st0(), st1(), D[0x07], D[0x0c], D[0x0d],
		      D[0x0e], D[0x18], D[0x19], D[0x09] };
	}
	/* RETE's restore of the shadow registers */
	void shadow_restore()
	{
		Shadow &s = shadow_[--nshadow_];
		acc = s.acc; accb = s.accb; p = s.p;
		D[0x07] = s.pmst; D[0x0c] = s.t0; D[0x0d] = s.t1;
		D[0x0e] = s.t2; D[0x18] = s.indx; D[0x19] = s.arcr;
		D[0x09] = s.brcr;
		lst0(s.st0); lst1(s.st1); arp = s.st0 >> 13;
	}

	/* a word's bits reversed */
	static w16 rev16(w16 x)
	{
		w16 r = 0;
		for (int i = 0; i < 16; i++)
			if (x & (1 << i))
				r |= (w16)(0x8000 >> i);
		return r;
	}
	/* a bit-reversed (*BR0+ or *BR0-) step of a by INDX */
	w16 brev(w16 a, int d) const
	{
		w16 i = D[0x18];
		return rev16((w16)(rev16(a) + d * rev16(i)));
	}
	/* the end of circular buffer n's AR, or -1 if it is in none */
	int circ_end(int n) const
	{
		w16 r = D[0x1e];
		if ((r & 8) && (r & 7) == n)
			return D[0x1b];
		if ((r & 0x80) && ((r >> 4) & 7) == n)
			return D[0x1d];
		return -1;
	}

	/* the data address of an operand (indirect: AR modified) */
	u32 ea(w16 op)
	{
		if (!(op & 0x80))
			return (u32)((dp << 7) | (op & 0x7f));
		int n = arp;
		w16 a = ar(n);
		int m = (op >> 4) & 7;
		switch (m) {
		case 1: setar(n, a - 1); break;
		case 2: setar(n, a + 1); break;
		case 4: setar(n, brev(a, -1)); break;
		case 5: setar(n, a - D[0x18]); break;
		case 6: setar(n, a + D[0x18]); break;
		case 7: setar(n, brev(a, 1)); break;
		case 3: stop(op); break;
		}
		if ((m == 1 || m == 2 || m == 5 || m == 6) && a == circ_end(n))
			setar(n, (D[0x1e] & 7) == n ? D[0x1a] : D[0x1c]);
		if (op & 8) {
			arb = arp;
			arp = op & 7;
		}
		return a;
	}
	/* an indirect operand's side effect on the ARs (none for direct) */
	void mod(w16 op)
	{
		if (op & 0x80)
			ea(op);
	}
	/* a memory-mapped register's address: direct, page 0 */
	u32 mmr(w16 op) { return (op & 0x80) ? (ea(op) & 0x7f) : (op & 0x7f); }

	/* PREG shifted by PM */
	u32 pshift() const
	{
		int32_t v = s32(p);
		switch (pm) {
		case 1: return (u32)v << 1;
		case 2: return (u32)v << 4;
		case 3: return (u32)(v >> 6);
		}
		return (u32)v;
	}
	/* a data word to the accumulator's width, sign-extended by SXM,
	   shifted */
	u32 ld(u32 v, int sh) const
	{
		u32 x = sxm ? (u32)s16(v) : (v & 0xffff);
		return x << sh;
	}
	/* x (and a carry) added to ACC: C, OV, saturated by OVM */
	void addacc(u32 x, u32 cin = 0)
	{
		u32 a = acc;
		uint64_t r = (uint64_t)a + x + cin;
		c = r > M32;
		u32 res = (u32)r;
		if ((s32(a) >= 0) == (s32(x) >= 0) &&
		    (s32(res) >= 0) != (s32(a) >= 0)) {
			ov = 1;
			if (ovm)
				res = s32(a) >= 0 ? 0x7fffffff : 0x80000000;
		}
		acc = res;
	}
	/* x (and a borrow) subtracted from ACC: C, OV, saturated by OVM */
	void subacc(u32 x, u32 bin = 0)
	{
		u32 a = acc;
		int64_t r = (int64_t)a - x - bin;
		c = r < 0 ? 0 : 1;
		u32 res = (u32)r;
		if ((s32(a) >= 0) != (s32(x) >= 0) &&
		    (s32(res) >= 0) != (s32(a) >= 0)) {
			ov = 1;
			if (ovm)
				res = s32(a) >= 0 ? 0x7fffffff : 0x80000000;
		}
		acc = res;
	}

	/* a conditional instruction's conditions (its low byte) all true */
	bool cond(w16 op)
	{
		int m = op & 15, v = (op >> 4) & 15;
		int32_t a = s32(acc);
		int zl = (v & 0xc) | ((m >> 2) & 3);
		bool ok;
		switch (zl) {
		case 1: case 3: case 9: ok = a > 0; break;
		case 2: case 6: ok = a != 0; break;
		case 5: case 7: case 13: ok = a < 0; break;
		case 10: case 14: ok = a == 0; break;
		case 11: ok = a >= 0; break;
		case 15: ok = a <= 0; break;
		default: ok = true; break;
		}
		if (m & 2) {
			ok = ok && ov == ((v >> 1) & 1);
			ov = 0;
		}
		if (m & 1)
			ok = ok && c == (v & 1);
		int tp = (op >> 8) & 3;
		if (tp == 0)
			ok = false;     /* BIO: never low */
		else if (tp == 1)
			ok = ok && tc == 1;
		else if (tp == 2)
			ok = ok && tc == 0;
		return ok;
	}

	/* an instruction's length in words */
	static int size(w16 op)
	{
		int b = op >> 8, lo = op & 0xff;
		switch (b) {
		case 0x09: case 0x0c: case 0x89: case 0xa2: case 0xa3:
		case 0xa5: case 0xa8: case 0xa9: case 0xae: case 0xaf:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f:
		case 0x79: case 0x7a: case 0x7b: case 0x7d: case 0x7e:
		case 0x7f:
			return 2;
		}
		if ((b >= 0xe0 && b <= 0xe3) || (b >= 0xe8 && b <= 0xeb) ||
		    (b >= 0xf0 && b <= 0xf3) || (b >= 0xf8 && b <= 0xfb))
			return 2;
		if (b == 0xbe && (lo == 0x80 || lo == 0x81 || lo == 0x82 ||
				  lo == 0x83 || lo == 0xc4 || lo == 0xc5 ||
				  lo == 0xc6))
			return 2;
		if (b == 0xbf) {
			int s = (op >> 4) & 15;
			if ((s == 0 && (op & 8)) || (s >= 8 && s <= 13))
				return 2;
		}
		return 1;
	}

	/* the DSP stopped at an instruction it does not know */
	void stop(w16 op)
	{
		if (!stopped) {
			stopped = true;
			stop_op = op;
			stop_pc = pc;
		}
	}

	/* one instruction at at, repeated by RPT: the next pc, or -1 for the
	   one after */
	int exe(w16 op, w16 at)
	{
		int b = op >> 8, lo = op & 0xff;
		w16 imm = pr(at + 1u);
		int times = 1;
		if (rptc_ >= 0 && !(b == 0xbb || b == 0x0b ||
				    (b == 0xbe && (lo == 0xc4 || lo == 0xc5)))) {
			times = rptc_ + 1;
			rptc_ = -1;
		}
		for (int it = 0; it < times; it++) {
			int r = exe1(op, at, b, lo, imm, it);
			if (r >= 0)
				return r;
		}
		return -1;
	}

	/* one execution of an instruction: the next pc, or -1 for the one
	   after */
	int exe1(w16 op, w16 at, int b, int lo, w16 imm, int it)
	{
		std::vector<w16> &d = D;
		u32 a, x;
		if (b <= 0x07) { setar(b, rdm(ea(op))); return -1; }
		switch (b) {
		case 0x08: acc = rdm(mmr(op)); return -1;
		case 0x09: a = mmr(op); wr(imm, rdm(a)); return -1;
		case 0x0a:
			x = (u32)rdm(ea(op)) << 15;
			if (acc >= x)
				acc = ((acc - x) << 1) + 1;
			else
				acc <<= 1;
			return -1;
		case 0x0b: rptc_ = rdm(ea(op)); return -1;
		case 0x0c:      /* OUT */
			a = ea(op);
			if (io_wr && imm >= 0x50 && imm < 0x70)
				io_wr(imm & 0x2f, rdm(a));
			return -1;
		case 0x0d: dp = rdm(ea(op)) & 0x1ff; return -1;
		case 0x0e: lst0(rdm(ea(op))); return -1;
		case 0x0f: lst1(rdm(ea(op))); return -1;
		}
		if (b >= 0x10 && b <= 0x1f) { acc = ld(rdm(ea(op)), b & 15); return -1; }
		if (b >= 0x20 && b <= 0x2f) { addacc(ld(rdm(ea(op)), b & 15)); return -1; }
		if (b >= 0x30 && b <= 0x3f) { subacc(ld(rdm(ea(op)), b & 15)); return -1; }
		if (b >= 0x40 && b <= 0x4f) { tc = (rdm(ea(op)) >> (15 - (b & 15))) & 1; return -1; }
		switch (b) {
		case 0x50: x = pshift(); p = (u32)(s16(T()) * s16(rdm(ea(op)))); addacc(x); return -1;
		case 0x51: x = pshift(); p = (u32)(s16(T()) * s16(rdm(ea(op)))); subacc(x); return -1;
		case 0x52: case 0x53: {
			x = pshift();
			w16 v = rdm(ea(op));
			setT(v);
			p = (u32)(s16(v) * s16(v));
			if (b == 0x52) addacc(x); else subacc(x);
			return -1;
		}
		case 0x54: p = (u32)(s16(T()) * s16(rdm(ea(op)))); return -1;
		case 0x55: p = (u32)T() * rdm(ea(op)); return -1;
		case 0x57: pw(d[0x1f] + (u32)it, rdm(ea(op))); return -1;
		case 0x58: case 0x59: case 0x5a: case 0x5b:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
			a = ea(op);
			w16 k = b >= 0x5c ? imm : d[0x0f];
			/* XPL, OPL, APL: TC set if the result is 0 (SPRU056D
			   6-284 and the others); CPL: TC if equal */
			w16 r;
			switch ((b - 0x58) & 3) {
			case 0: r = rdm(a) ^ k; wr(a, r); tc = r == 0; break;
			case 1: r = rdm(a) | k; wr(a, r); tc = r == 0; break;
			case 2: r = rdm(a) & k; wr(a, r); tc = r == 0; break;
			default: tc = rdm(a) == k; break;
			}
			return -1;
		}
		case 0x60: addacc(rdm(ea(op)), c); return -1;
		case 0x61: { int c0 = c; addacc(ld(rdm(ea(op)), 16)); c |= c0; return -1; }
		case 0x62: addacc(rdm(ea(op))); return -1;
		case 0x63: addacc(ld(rdm(ea(op)), d[0x0d] & 15)); return -1;
		case 0x64: subacc(rdm(ea(op)), 1 - c); return -1;
		case 0x65: { int c0 = c; subacc(ld(rdm(ea(op)), 16)); c &= c0; return -1; }
		case 0x66: subacc(rdm(ea(op))); return -1;
		case 0x67: subacc(ld(rdm(ea(op)), d[0x0d] & 15)); return -1;
		case 0x68: acc = ((u32)rdm(ea(op)) << 16) | 0x8000; return -1;
		case 0x69: acc = rdm(ea(op)); return -1;
		case 0x6a: acc = ld(rdm(ea(op)), 16); return -1;
		case 0x6b: acc = ld(rdm(ea(op)), d[0x0d] & 15); return -1;
		case 0x6c: acc ^= rdm(ea(op)); return -1;
		case 0x6d: acc |= rdm(ea(op)); return -1;
		case 0x6e: acc &= rdm(ea(op)); return -1;
		case 0x6f: tc = (rdm(ea(op)) >> (15 - (d[0x0e] & 15))) & 1; return -1;
		case 0x70: x = pshift(); setT(rdm(ea(op))); addacc(x); return -1;
		case 0x71: x = pshift(); setT(rdm(ea(op))); acc = x; return -1;
		case 0x72:
			x = pshift(); a = ea(op); setT(rdm(a));
			wr((a + 1) & 0xffff, rdm(a)); addacc(x); return -1;
		case 0x73: setT(rdm(ea(op))); return -1;
		case 0x74: x = pshift(); setT(rdm(ea(op))); subacc(x); return -1;
		case 0x75: p = (p & 0xffff) | ((u32)rdm(ea(op)) << 16); return -1;
		case 0x76: push(rdm(ea(op))); return -1;
		case 0x77: a = ea(op); wr((a + 1) & 0xffff, rdm(a)); return -1;
		case 0x78: setar(arp, ar(arp) + lo); return -1;
		case 0x7c: setar(arp, ar(arp) - lo); return -1;
		case 0x79: mod(op); return imm;
		case 0x7d: mod(op); return imm | DELAYED;
		case 0x7a: mod(op); push(at + 2u); return imm;
		case 0x7e: mod(op); push(at + 4u); return imm | DELAYED;
		case 0x7b: case 0x7f: {
			bool z = ar(arp) != 0;
			mod(op);
			if (z)
				return b == 0x7b ? imm : (imm | DELAYED);
			return b == 0x7b ? (w16)(at + 2) : ((w16)(at + 4) | DELAYED);
		}
		}
		if (b >= 0x80 && b <= 0x87) { wr(ea(op), ar(b & 7)); return -1; }
		switch (b) {
		case 0x88: a = mmr(op); wr(a, acc); return -1;
		case 0x89: a = mmr(op); wr(a, rdm(imm)); return -1;
		case 0x8a: wr(ea(op), pop()); return -1;
		case 0x8b: if (op != 0x8b00) ea(op); return -1;
		case 0x8c: wr(ea(op), pshift()); return -1;
		case 0x8d: wr(ea(op), pshift() >> 16); return -1;
		case 0x8e: a = ea(op); wr((op & 0x80) ? (a & 0x7f) : (op & 0x7f), st0()); return -1;
		case 0x8f: a = ea(op); wr((op & 0x80) ? (a & 0x7f) : (op & 0x7f), st1()); return -1;
		}
		if (b >= 0x90 && b <= 0x97) { wr(ea(op), acc << (b & 7)); return -1; }
		if (b >= 0x98 && b <= 0x9f) { wr(ea(op), (acc << (b & 7)) >> 16); return -1; }
		switch (b) {
		case 0xa0: {    /* NORM: AR modified only with a shift */
			int32_t v = s32(acc);
			if (v != 0 && ((v >> 31) & 1) == ((v >> 30) & 1)) {
				acc <<= 1;
				tc = 0;
				ea(op);
			} else {
				tc = 1;
			}
			return -1;
		}
		case 0xa2: case 0xa3:
			x = pshift(); a = ea(op); setT(rdm(a));
			p = (u32)(s16(rdm(a)) * s16(pr(imm + (u32)it)));
			if (b == 0xa3) wr((a + 1) & 0xffff, rdm(a));
			addacc(x); return -1;
		case 0xaa: case 0xab:
			x = pshift(); a = ea(op); setT(rdm(a));
			p = (u32)(s16(rdm(a)) * s16(pr(d[0x1f] + (u32)it)));
			if (b == 0xab) wr((a + 1) & 0xffff, rdm(a));
			addacc(x); return -1;
		case 0xa4: a = ea(op); wr(a, pr(d[0x1f] + (u32)it)); return -1;
		case 0xac: a = ea(op); wr(a, rdm(d[0x1f] + it)); return -1;
		case 0xad: a = ea(op); wr((d[0x1f] + it) & 0xffff, rdm(a)); return -1;
		case 0xa5: a = ea(op); wr(a, pr(imm + (u32)it)); return -1;
		case 0xa6: a = ea(op); wr(a, pr((acc & 0xffff) + (u32)it)); return -1;
		case 0xa7: a = ea(op); pw((acc & 0xffff) + (u32)it, rdm(a)); return -1;
		case 0xa8: a = ea(op); wr(a, rdm(imm + it)); return -1;
		case 0xa9: a = ea(op); wr((imm + it) & 0xffff, rdm(a)); return -1;
		case 0xae: wr(ea(op), imm); return -1;
		case 0xaf:      /* IN */
			a = ea(op);
			wr(a, io_rd && imm >= 0x50 && imm < 0x70 ? io_rd(imm & 0x2f) : 0);
			return -1;
		}
		if (b >= 0xb0 && b <= 0xb7) { setar(b & 7, lo); return -1; }
		switch (b) {
		case 0xb8: addacc(lo); return -1;
		case 0xb9: acc = lo; return -1;
		case 0xba: subacc(lo); return -1;
		case 0xbb: rptc_ = lo; return -1;
		case 0xbc: case 0xbd: dp = op & 0x1ff; return -1;
		case 0xbe: return be(op, at, lo, imm);
		case 0xbf: {
			int s = (op >> 4) & 15, sh = op & 15;
			if (s == 0 && (op & 8)) { setar(op & 7, imm); return -1; }
			if (s == 0) { pm = op & 3; return -1; }
			if (s == 4) {
				w16 xa = ar(arp), y = D[0x19];
				switch (op & 3) {
				case 0: tc = xa == y; break;
				case 1: tc = xa < y; break;
				case 2: tc = xa > y; break;
				default: tc = xa != y; break;
				}
				return -1;
			}
			if (s >= 8 && s <= 13) {
				u32 k = s <= 10 ? ld(imm, sh) : ((u32)imm << sh);
				switch (s) {
				case 8: acc = k; break;
				case 9: addacc(k); break;
				case 10: subacc(k); break;
				case 11: acc &= k; break;
				case 12: acc |= k; break;
				default: acc ^= k; break;
				}
				return -1;
			}
			if (s == 14) {
				int k = sh + 1;
				acc = sxm ? (u32)(s32(acc) >> k) : acc >> k;
				return -1;
			}
			break;
		}
		}
		if (b >= 0xc0 && b <= 0xdf) {
			int k = op & 0x1fff;
			if (k & 0x1000)
				k -= 0x2000;
			p = (u32)(s16(T()) * k);
			return -1;
		}
		if (b >= 0xe0 && b <= 0xe3)
			return cond(op) ? imm : (w16)(at + 2);
		if ((b >= 0xe4 && b <= 0xe7) || (b >= 0xf4 && b <= 0xf7)) {
			int n = 1 + ((op >> 12) & 1);
			if (!cond(op))
				return (w16)(at + 1 + n);
			return -1;
		}
		if (b >= 0xe8 && b <= 0xeb) {
			if (cond(op)) { push(at + 2u); return imm; }
			return (w16)(at + 2);
		}
		if (b >= 0xec && b <= 0xef) {
			if (op == 0xef00 || cond(op))
				return pop();
			return -1;
		}
		if (b >= 0xf0 && b <= 0xf3)
			return cond(op) ? (imm | DELAYED) : ((w16)(at + 4) | DELAYED);
		if (b >= 0xf8 && b <= 0xfb) {
			if (cond(op)) { push(at + 4u); return imm | DELAYED; }
			return (w16)(at + 4) | DELAYED;
		}
		if (b >= 0xfc) {
			if (op == 0xff00 || cond(op))
				return pop() | DELAYED;
			return -1;
		}
		stop(op);
		return -1;
	}

	/* the 0xbe group (accumulator and control instructions) */
	int be(w16 op, w16 at, int lo, w16 imm)
	{
		u32 a = acc;
		switch (lo) {
		case 0x00: {
			int32_t v = s32(a);
			if (v == INT32_MIN) {
				acc = ovm ? 0x7fffffff : a;
				ov = 1;
			} else {
				acc = (u32)(v < 0 ? -v : v);
			}
			return -1;
		}
		case 0x01: acc = ~a; return -1;
		case 0x02: acc = 0; subacc(a); return -1;
		case 0x03: acc = pshift(); return -1;
		case 0x04: addacc(pshift()); return -1;
		case 0x05: subacc(pshift()); return -1;
		case 0x09: c = (a >> 31) & 1; acc = a << 1; return -1;
		case 0x0a: c = a & 1; acc = sxm ? (u32)(s32(a) >> 1) : a >> 1; return -1;
		case 0x0c: { int c0 = c; c = (a >> 31) & 1; acc = (a << 1) | c0; return -1; }
		case 0x0d: { int c0 = c; c = a & 1; acc = (a >> 1) | ((u32)c0 << 31); return -1; }
		case 0x10: addacc(accb); return -1;
		case 0x11: addacc(accb, c); return -1;
		case 0x12: acc &= accb; return -1;
		case 0x13: acc |= accb; return -1;
		case 0x14: {
			int c0 = c; c = (a >> 31) & 1;
			acc = (a << 1) | (accb >> 31);
			accb = (accb << 1) | c0;
			return -1;
		}
		case 0x15: {
			int c0 = c; c = accb & 1;
			accb = (accb >> 1) | ((a & 1) << 31);
			acc = (a >> 1) | ((u32)c0 << 31);
			return -1;
		}
		case 0x16:
			c = (a >> 31) & 1;
			acc = (a << 1) | (accb >> 31);
			accb <<= 1;
			return -1;
		case 0x17: {
			c = accb & 1;
			u32 nb = (accb >> 1) | ((a & 1) << 31);
			acc = sxm ? (u32)(s32(a) >> 1) : a >> 1;
			accb = nb;
			return -1;
		}
		case 0x18: subacc(accb); return -1;
		case 0x19: subacc(accb, 1 - c); return -1;
		case 0x1a: acc ^= accb; return -1;
		case 0x1b:      /* CRGT */
			if (s32(a) >= s32(accb)) { accb = a; c = 1; }
			else { acc = accb; c = 0; }
			return -1;
		case 0x1c:      /* CRLT */
			if (s32(a) < s32(accb)) { accb = a; c = 1; }
			else { acc = accb; c = 0; }
			return -1;
		case 0x1d: acc = accb; accb = a; return -1;
		case 0x1e: accb = a; return -1;
		case 0x1f: acc = accb; return -1;
		case 0x20: return a & 0xffff;
		case 0x21: return (a & 0xffff) | DELAYED;
		case 0x30: push(at + 1u); return a & 0xffff;
		case 0x3d: push(at + 3u); return (a & 0xffff) | DELAYED;
		case 0x32: acc = pop(); return -1;
		case 0x3c: push(a); return -1;
		case 0x3a:      /* RETE */
			if (nshadow_)
				shadow_restore();
			intm = 0;
			return pop();
		case 0x38: return pop();        /* RETI */
		case 0x22: case 0x23: idle = true; return -1;  /* IDLE, IDLE2 */
		case 0x40: intm = 0; return -1;
		case 0x41: intm = 1; return -1;
		case 0x42: ovm = 0; return -1;
		case 0x43: ovm = 1; return -1;
		case 0x44: cnf = 0; return -1;
		case 0x45: cnf = 1; return -1;
		case 0x46: sxm = 0; return -1;
		case 0x47: sxm = 1; return -1;
		case 0x4a: tc = 0; return -1;
		case 0x4b: tc = 1; return -1;
		case 0x4c: case 0x4d: return -1;
		case 0x4e: c = 0; return -1;
		case 0x4f: c = 1; return -1;
		case 0x58: p = 0; return -1;
		case 0x59: p = 0; acc = 0; return -1;
		case 0x5a:
			if (D[0x0d] & 0x10)
				acc = sxm ? (u32)(s32(a) >> 16) : a >> 16;
			return -1;
		case 0x5b: {
			int k = D[0x0d] & 15;
			acc = sxm ? (u32)(s32(a) >> k) : a >> k;
			return -1;
		}
		case 0x80: p = (u32)(s16(T()) * s16(imm)); return -1;
		case 0x81: acc &= (u32)imm << 16; return -1;
		case 0x82: acc |= (u32)imm << 16; return -1;
		case 0x83: acc ^= (u32)imm << 16; return -1;
		case 0xc4: rptc_ = imm; return -1;
		case 0xc5: rptc_ = imm; acc = 0; p = 0; return -1;
		case 0xc6: rptb_start_ = (w16)(at + 2); rptb_end_ = imm; rptb_on_ = true; return -1;
		}
		if (lo >= 0x60 && lo <= 0x7f) {        /* INTR k */
			intm = 1;
			shadow_save();
			push(at + 1u);
			return 2 * (lo & 31);
		}
		stop(op);
		return -1;
	}
};

}

#endif
