/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/i80186.hpp - an Intel 80186 (80C186EB) core: the 8086's
 *                    instructions and the 80186's (PUSH immediate,
 *                    PUSHA/POPA, IMUL immediate, shifts by a count,
 *                    ENTER/LEAVE, BOUND, INS/OUTS), real mode, 1 MiB
 *
 *    Memory is 256 pages of 4 KiB: a page is host memory (read-only or
 *    not) or goes to the bus's callbacks.  I/O goes to the bus.  The
 *    integrated peripherals (the 80C186EB's timers, interrupt
 *    controller, serial units, chip selects) are the harness's, on the
 *    bus at the peripheral control block's addresses; the core takes an
 *    interrupt by its type (irq()), when IF allows.
 *
 *    Not modelled: the prefetch queue, wait states and cycle counts (an
 *    instruction counts one), the 8087's escapes (ESC is a no-op with
 *    its operand decoded), LOCK.
 *
 */

#ifndef HW_I80186_HPP
#define HW_I80186_HPP

#include <stdint.h>
#include <string.h>

#include <functional>

namespace half {

class I80186 {
public:
	typedef uint8_t u8;
	typedef uint16_t u16;
	typedef uint32_t u32;

	/* the bus: memory outside host pages, and I/O */
	std::function<u8(u32 a)> mem_rd;
	std::function<void(u32 a, u8 v)> mem_wr;
	std::function<u16(u16 port, bool word)> io_rd;
	std::function<void(u16 port, u16 v, bool word)> io_wr;

	/* host memory for physical a .. a + n (4 KiB aligned) */
	void map(u32 a, u8 *p, u32 n, bool ro)
	{
		for (u32 o = 0; o < n; o += 0x1000) {
			pages_[((a + o) >> 12) & 0xff] = p + o;
			ro_[((a + o) >> 12) & 0xff] = ro;
		}
	}
	void unmap(u32 a, u32 n)
	{
		for (u32 o = 0; o < n; o += 0x1000)
			pages_[((a + o) >> 12) & 0xff] = nullptr;
	}

	/* the registers */
	u16 r[8] = {};                  /* AX CX DX BX SP BP SI DI */
	u16 sr[4] = {};                 /* ES CS SS DS */
	u16 ip = 0;
	u16 flags = 0xf002;
	enum { AX, CX, DX, BX, SP, BP, SI, DI };
	enum { ES, CS, SS, DS };
	enum { CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, TF = 0x100,
	       IF = 0x200, DF = 0x400, OF = 0x800 };

	unsigned long long icount = 0;
	/* debugging: before each instruction */
	std::function<void(I80186 &, u32 at)> on_step;
	/* the last 4096 instructions' addresses, for a debugger */
	u32 hist[4096] = {};
	unsigned hn = 0;
	bool halted = false;
	/* an opcode not known: stopped there */
	bool stopped = false;
	u32 stop_at = 0;
	u8 stop_op = 0;

	/* a hook on a physical address: before the instruction there */
	std::function<void(I80186 &)> *hooks[256] = {};
	void hook(u32 phys, std::function<void(I80186 &)> fn)
	{
		u32 pg = (phys >> 12) & 0xff;
		if (!hooks[pg]) {
			hooks[pg] = new std::function<void(I80186 &)>[0x1000];
		}
		hooks[pg][phys & 0xfff] = std::move(fn);
		hooked_[pg] = true;
	}

	I80186() { memset(pages_, 0, sizeof(pages_)); memset(ro_, 0, sizeof(ro_)); memset(hooked_, 0, sizeof(hooked_)); }
	~I80186()
	{
		for (auto &h : hooks)
			delete[] h;
	}

	void reset()
	{
		memset(r, 0, sizeof(r));
		sr[ES] = sr[SS] = sr[DS] = 0;
		sr[CS] = 0xffff;
		ip = 0;
		flags = 0xf002;
		halted = false;
	}

	u32 cs_ip() const { return ((u32)sr[CS] << 4) + ip; }

	/* memory, physical */
	u8 rd8(u32 a)
	{
		a &= 0xfffff;
		u8 *p = pages_[a >> 12];
		if (p)
			return p[a & 0xfff];
		return mem_rd ? mem_rd(a) : 0xff;
	}
	void wr8(u32 a, u8 v)
	{
		a &= 0xfffff;
		u8 *p = pages_[a >> 12];
		if (p) {
			if (!ro_[a >> 12])
				p[a & 0xfff] = v;
			return;
		}
		if (mem_wr)
			mem_wr(a, v);
	}
	u16 rd16(u32 a) { return (u16)(rd8(a) | rd8(a + 1) << 8); }
	void wr16(u32 a, u16 v) { wr8(a, (u8)v); wr8(a + 1, (u8)(v >> 8)); }

	/* a hardware or software interrupt of this type */
	void interrupt(u8 type)
	{
		push(flags);
		flags &= ~(IF | TF);
		push(sr[CS]);
		push(ip);
		ip = rd16(type * 4u);
		sr[CS] = rd16(type * 4u + 2);
		halted = false;
	}
	/* a maskable one: taken if IF is set (and not between a segment
	   register's load and the next instruction); true if taken */
	bool irq(u8 type)
	{
		if (!(flags & IF) || inhibit_)
			return false;
		interrupt(type);
		return true;
	}
	void nmi() { interrupt(2); }

	/* one instruction (its prefixes and repeats with it) */
	void step()
	{
		if (halted || stopped)
			return;
		inhibit_ = false;
		u32 at = cs_ip();
		if (hooked_[at >> 12] && hooks[at >> 12][at & 0xfff]) {
			u16 cs0 = sr[CS], ip0 = ip;
			hooks[at >> 12][at & 0xfff](*this);
			if (sr[CS] != cs0 || ip != ip0 || halted)
				return;
		}
		seg_ = -1;
		rep_ = 0;
		icount++;
		hist[hn++ & 4095] = at;
		if (on_step)
			on_step(*this, at);
		for (;;) {
			u8 op = fetch8();
			switch (op) {
			case 0x26: seg_ = ES; continue;
			case 0x2e: seg_ = CS; continue;
			case 0x36: seg_ = SS; continue;
			case 0x3e: seg_ = DS; continue;
			case 0xf0: continue;            /* LOCK */
			case 0xf2: rep_ = 2; continue;  /* REPNE */
			case 0xf3: rep_ = 1; continue;  /* REP(E) */
			}
			exec(op, at);
			return;
		}
	}

private:
	u8 *pages_[256];
	bool ro_[256];
	bool hooked_[256];
	int seg_ = -1, rep_ = 0;
	bool inhibit_ = false;

	/* the decoded ModRM */
	int mod_, reg_, rm_;
	u32 ea_;                        /* physical, for memory operands */
	u16 eoff_;                      /* its offset */

	u8 fetch8() { u8 v = rd8(cs_ip()); ip++; return v; }
	u16 fetch16() { u16 v = rd16(cs_ip()); ip += 2; return v; }

	void push(u16 v) { r[SP] -= 2; wr16(((u32)sr[SS] << 4) + r[SP], v); }
	u16 pop() { u16 v = rd16(((u32)sr[SS] << 4) + r[SP]); r[SP] += 2; return v; }

	u32 lin(int s, u16 off) { return ((u32)sr[s] << 4) + off; }
	u32 dseg(u16 off) { return lin(seg_ >= 0 ? seg_ : DS, off); }

	u8 &reg8(int n) { return ((u8 *)&r[n & 3])[n >> 2]; }

	void modrm()
	{
		u8 m = fetch8();
		mod_ = m >> 6;
		reg_ = (m >> 3) & 7;
		rm_ = m & 7;
		if (mod_ == 3)
			return;
		u16 off = 0;
		int ds = DS;
		switch (rm_) {
		case 0: off = r[BX] + r[SI]; break;
		case 1: off = r[BX] + r[DI]; break;
		case 2: off = r[BP] + r[SI]; ds = SS; break;
		case 3: off = r[BP] + r[DI]; ds = SS; break;
		case 4: off = r[SI]; break;
		case 5: off = r[DI]; break;
		case 6:
			if (mod_ == 0)
				off = fetch16();
			else {
				off = r[BP];
				ds = SS;
			}
			break;
		case 7: off = r[BX]; break;
		}
		if (mod_ == 1)
			off += (u16)(int8_t)fetch8();
		else if (mod_ == 2)
			off += fetch16();
		eoff_ = off;
		ea_ = lin(seg_ >= 0 ? seg_ : ds, off);
	}
	u8 rm8() { return mod_ == 3 ? reg8(rm_) : rd8(ea_); }
	u16 rm16() { return mod_ == 3 ? r[rm_] : rd16(ea_); }
	void setrm8(u8 v) { if (mod_ == 3) reg8(rm_) = v; else wr8(ea_, v); }
	void setrm16(u16 v) { if (mod_ == 3) r[rm_] = v; else wr16(ea_, v); }
	/* the word after the ModRM's word: for LES, LDS, BOUND, CALL far */
	u16 rm16b() { return rd16(ea_ + 2); }

	static bool parity(u8 v)
	{
		v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
		return !(v & 1);
	}
	void szp8(u8 v)
	{
		flags &= ~(SF | ZF | PF);
		if (v & 0x80) flags |= SF;
		if (!v) flags |= ZF;
		if (parity(v)) flags |= PF;
	}
	void szp16(u16 v)
	{
		flags &= ~(SF | ZF | PF);
		if (v & 0x8000) flags |= SF;
		if (!v) flags |= ZF;
		if (parity((u8)v)) flags |= PF;
	}
	void setf(u16 f, bool on) { if (on) flags |= f; else flags &= ~f; }

	/* the ALU: op 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
	u32 alu(int op, u32 a, u32 b, bool w)
	{
		u32 m = w ? 0xffff : 0xff, sb = w ? 0x8000 : 0x80;
		u32 res = 0, cin;
		switch (op) {
		case 0: case 2:
			cin = op == 2 ? (flags & CF) : 0;
			res = a + b + cin;
			setf(CF, res > m);
			setf(AF, ((a ^ b ^ res) & 0x10) != 0);
			setf(OF, ((~(a ^ b) & (a ^ res)) & sb) != 0);
			break;
		case 3: case 5: case 7:
			cin = op == 3 ? (flags & CF) : 0;
			res = a - b - cin;
			setf(CF, (uint64_t)b + cin > a);
			setf(AF, ((a ^ b ^ res) & 0x10) != 0);
			setf(OF, (((a ^ b) & (a ^ res)) & sb) != 0);
			break;
		case 1: res = a | b; flags &= ~(CF | OF | AF); break;
		case 4: res = a & b; flags &= ~(CF | OF | AF); break;
		case 6: res = a ^ b; flags &= ~(CF | OF | AF); break;
		}
		res &= m;
		if (w) szp16((u16)res); else szp8((u8)res);
		return res;
	}
	u32 incdec(u32 a, bool dec, bool w)
	{
		u16 cf = flags & CF;
		u32 res = alu(dec ? 5 : 0, a, 1, w);
		flags = (u16)((flags & ~CF) | cf);
		return res;
	}

	/* shifts and rotates: op 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 6 SAL
	   7 SAR; the 80186 masks the count to 5 bits */
	u32 shift(int op, u32 v, int n, bool w)
	{
		int bits = w ? 16 : 8;
		u32 m = w ? 0xffff : 0xff, sb = w ? 0x8000 : 0x80;
		n &= 31;
		if (!n)
			return v;
		u32 res = v;
		for (int i = 0; i < n; i++) {
			u32 cf;
			switch (op) {
			case 0: cf = (res & sb) != 0; res = ((res << 1) | cf) & m; setf(CF, cf); break;
			case 1: cf = res & 1; res = (res >> 1) | (cf ? sb : 0); setf(CF, cf); break;
			case 2: cf = (res & sb) != 0; res = ((res << 1) | (flags & CF)) & m; setf(CF, cf); break;
			case 3: cf = res & 1; res = (res >> 1) | ((flags & CF) ? sb : 0); setf(CF, cf); break;
			case 4: case 6: cf = (res & sb) != 0; res = (res << 1) & m; setf(CF, cf); break;
			case 5: cf = res & 1; res >>= 1; setf(CF, cf); break;
			case 7: cf = res & 1; res = (res >> 1) | (res & sb); setf(CF, cf); break;
			}
		}
		switch (op) {
		case 0: case 2: setf(OF, (((res & sb) != 0) ^ ((flags & CF) != 0))); break;
		case 1: case 3: setf(OF, ((res ^ (res << 1)) & sb) != 0); break;
		case 4: case 6: setf(OF, (((res & sb) != 0) ^ ((flags & CF) != 0))); szpw(res, w); break;
		case 5: setf(OF, n == 1 && (v & sb)); szpw(res, w); break;
		case 7: flags &= ~OF; szpw(res, w); break;
		}
		(void)bits;
		return res;
	}
	void szpw(u32 v, bool w) { if (w) szp16((u16)v); else szp8((u8)v); }

	bool cc(int c)
	{
		bool cf = flags & CF, zf = flags & ZF, sf = (flags & SF) != 0,
		     of = (flags & OF) != 0, pf = flags & PF;
		bool v;
		switch (c >> 1) {
		case 0: v = of; break;
		case 1: v = cf; break;
		case 2: v = zf; break;
		case 3: v = cf || zf; break;
		case 4: v = sf; break;
		case 5: v = pf; break;
		case 6: v = sf != of; break;
		default: v = zf || (sf != of); break;
		}
		return (c & 1) ? !v : v;
	}

	void stop(u8 op, u32 at)
	{
		stopped = true;
		stop_at = at;
		stop_op = op;
	}

	void div8(bool sgn, u32 at)
	{
		u8 d = rm8();
		if (!d) { ip = (u16)(at - ((u32)sr[CS] << 4)); interrupt(0); return; }
		if (sgn) {
			int a = (int16_t)r[AX], q = a / (int8_t)d, rem = a % (int8_t)d;
			if (q > 127 || q < -128) { interrupt(0); return; }
			r[AX] = (u16)((u8)q | ((u8)rem << 8));
		} else {
			u32 a = r[AX], q = a / d, rem = a % d;
			if (q > 0xff) { interrupt(0); return; }
			r[AX] = (u16)(q | rem << 8);
		}
	}
	void div16(bool sgn)
	{
		u16 d = rm16();
		if (!d) { interrupt(0); return; }
		if (sgn) {
			int32_t a = (int32_t)((u32)r[DX] << 16 | r[AX]);
			int32_t q = a / (int16_t)d, rem = a % (int16_t)d;
			if (q > 32767 || q < -32768) { interrupt(0); return; }
			r[AX] = (u16)q; r[DX] = (u16)rem;
		} else {
			u32 a = (u32)r[DX] << 16 | r[AX], q = a / d, rem = a % d;
			if (q > 0xffff) { interrupt(0); return; }
			r[AX] = (u16)q; r[DX] = (u16)rem;
		}
	}

	/* string ops with REP: one iteration each; true to go on */
	template <class F> void rep(F &&body, bool cmp)
	{
		if (!rep_) { body(); return; }
		while (r[CX]) {
			body();
			r[CX]--;
			if (cmp) {
				bool z = flags & ZF;
				if ((rep_ == 1 && !z) || (rep_ == 2 && z))
					break;
			}
		}
	}
	int dstep(int n) { return (flags & DF) ? -n : n; }

	void exec(u8 op, u32 at);
};

inline void I80186::exec(u8 op, u32 at)
{
	u32 a, b;
	switch (op) {
	/* the ALU's eight, in its six forms each */
	case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
		modrm(); a = alu(op >> 3, rm8(), reg8(reg_), false); if ((op >> 3) != 7) setrm8((u8)a); return;
	case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39:
		modrm(); a = alu(op >> 3, rm16(), r[reg_], true); if ((op >> 3) != 7) setrm16((u16)a); return;
	case 0x02: case 0x0a: case 0x12: case 0x1a: case 0x22: case 0x2a: case 0x32: case 0x3a:
		modrm(); a = alu(op >> 3, reg8(reg_), rm8(), false); if ((op >> 3) != 7) reg8(reg_) = (u8)a; return;
	case 0x03: case 0x0b: case 0x13: case 0x1b: case 0x23: case 0x2b: case 0x33: case 0x3b:
		modrm(); a = alu(op >> 3, r[reg_], rm16(), true); if ((op >> 3) != 7) r[reg_] = (u16)a; return;
	case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c:
		a = alu(op >> 3, r[AX] & 0xff, fetch8(), false); if ((op >> 3) != 7) reg8(0) = (u8)a; return;
	case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d:
		a = alu(op >> 3, r[AX], fetch16(), true); if ((op >> 3) != 7) r[AX] = (u16)a; return;
	case 0x06: push(sr[ES]); return;
	case 0x07: sr[ES] = pop(); inhibit_ = true; return;
	case 0x0e: push(sr[CS]); return;
	case 0x16: push(sr[SS]); return;
	case 0x17: sr[SS] = pop(); inhibit_ = true; return;
	case 0x1e: push(sr[DS]); return;
	case 0x1f: sr[DS] = pop(); inhibit_ = true; return;
	case 0x27: {    /* DAA */
		u8 al = (u8)r[AX]; bool cf = flags & CF;
		if ((al & 0xf) > 9 || (flags & AF)) { al += 6; flags |= AF; } else flags &= ~AF;
		if ((u8)r[AX] > 0x99 || cf) { al += 0x60; flags |= CF; } else flags &= ~CF;
		reg8(0) = al; szp8(al); return;
	}
	case 0x2f: {    /* DAS */
		u8 al0 = (u8)r[AX], al = al0; bool cf = flags & CF;
		if ((al & 0xf) > 9 || (flags & AF)) { al -= 6; flags |= AF; } else flags &= ~AF;
		if (al0 > 0x99 || cf) { al -= 0x60; flags |= CF; } else flags &= ~CF;
		reg8(0) = al; szp8(al); return;
	}
	case 0x37:      /* AAA */
		if ((r[AX] & 0xf) > 9 || (flags & AF)) { r[AX] += 0x106; flags |= AF | CF; }
		else flags &= ~(AF | CF);
		r[AX] &= 0xff0f; return;
	case 0x3f:      /* AAS */
		if ((r[AX] & 0xf) > 9 || (flags & AF)) { r[AX] -= 6; reg8(4)--; flags |= AF | CF; }
		else flags &= ~(AF | CF);
		r[AX] &= 0xff0f; return;
	}
	if (op >= 0x40 && op <= 0x47) { r[op & 7] = (u16)incdec(r[op & 7], false, true); return; }
	if (op >= 0x48 && op <= 0x4f) { r[op & 7] = (u16)incdec(r[op & 7], true, true); return; }
	if (op >= 0x50 && op <= 0x57) { u16 v = r[op & 7]; push(op == 0x54 ? (u16)(v - 2) : v); return; }
	if (op >= 0x58 && op <= 0x5f) { u16 v = pop(); r[op & 7] = v; return; }
	if (op >= 0x70 && op <= 0x7f) { int8_t d = (int8_t)fetch8(); if (cc(op & 15)) ip += d; return; }
	if (op >= 0x91 && op <= 0x97) { u16 t = r[AX]; r[AX] = r[op & 7]; r[op & 7] = t; return; }
	if (op >= 0xb0 && op <= 0xb7) { reg8(op & 7) = fetch8(); return; }
	if (op >= 0xb8 && op <= 0xbf) { r[op & 7] = fetch16(); return; }
	switch (op) {
	case 0x60: {    /* PUSHA */
		u16 sp = r[SP];
		push(r[AX]); push(r[CX]); push(r[DX]); push(r[BX]);
		push(sp); push(r[BP]); push(r[SI]); push(r[DI]);
		return;
	}
	case 0x61:      /* POPA */
		r[DI] = pop(); r[SI] = pop(); r[BP] = pop(); pop();
		r[BX] = pop(); r[DX] = pop(); r[CX] = pop(); r[AX] = pop();
		return;
	case 0x62: {    /* BOUND */
		modrm();
		int16_t v = (int16_t)r[reg_], lo = (int16_t)rd16(ea_), hi = (int16_t)rm16b();
		if (v < lo || v > hi) { ip = (u16)(at - ((u32)sr[CS] << 4)); interrupt(5); }
		return;
	}
	case 0x68: push(fetch16()); return;
	case 0x6a: push((u16)(int8_t)fetch8()); return;
	case 0x69: case 0x6b: {
		modrm();
		int32_t s = (int16_t)rm16();
		int32_t k = op == 0x69 ? (int16_t)fetch16() : (int8_t)fetch8();
		int32_t p = s * k;
		r[reg_] = (u16)p;
		setf(CF | OF, p != (int16_t)p);
		return;
	}
	case 0x6c: rep([&] { wr8(lin(ES, r[DI]), (u8)io_rd(r[DX], false)); r[DI] += dstep(1); }, false); return;
	case 0x6d: rep([&] { wr16(lin(ES, r[DI]), io_rd(r[DX], true)); r[DI] += dstep(2); }, false); return;
	case 0x6e: rep([&] { io_wr(r[DX], rd8(dseg(r[SI])), false); r[SI] += dstep(1); }, false); return;
	case 0x6f: rep([&] { io_wr(r[DX], rd16(dseg(r[SI])), true); r[SI] += dstep(2); }, false); return;
	case 0x80: case 0x82:
		modrm(); b = fetch8(); a = alu(reg_, rm8(), b, false); if (reg_ != 7) setrm8((u8)a); return;
	case 0x81:
		modrm(); b = fetch16(); a = alu(reg_, rm16(), b, true); if (reg_ != 7) setrm16((u16)a); return;
	case 0x83:
		modrm(); b = (u16)(int8_t)fetch8(); a = alu(reg_, rm16(), b, true); if (reg_ != 7) setrm16((u16)a); return;
	case 0x84: modrm(); alu(4, rm8(), reg8(reg_), false); return;
	case 0x85: modrm(); alu(4, rm16(), r[reg_], true); return;
	case 0x86: { modrm(); u8 t = rm8(); setrm8(reg8(reg_)); reg8(reg_) = t; return; }
	case 0x87: { modrm(); u16 t = rm16(); setrm16(r[reg_]); r[reg_] = t; return; }
	case 0x88: modrm(); setrm8(reg8(reg_)); return;
	case 0x89: modrm(); setrm16(r[reg_]); return;
	case 0x8a: modrm(); reg8(reg_) = rm8(); return;
	case 0x8b: modrm(); r[reg_] = rm16(); return;
	case 0x8c: modrm(); setrm16(sr[reg_ & 3]); return;
	case 0x8d: modrm(); r[reg_] = eoff_; return;
	case 0x8e: modrm(); sr[reg_ & 3] = rm16(); inhibit_ = true; return;
	case 0x8f: modrm(); { u16 v = pop(); setrm16(v); } return;
	case 0x90: return;
	case 0x98: r[AX] = (u16)(int8_t)r[AX]; return;
	case 0x99: r[DX] = (r[AX] & 0x8000) ? 0xffff : 0; return;
	case 0x9a: { u16 o = fetch16(), s = fetch16(); push(sr[CS]); push(ip); sr[CS] = s; ip = o; return; }
	case 0x9b: return;      /* WAIT */
	case 0x9c: push(flags); return;
	case 0x9d: flags = (u16)((pop() & 0x0fd5) | 0xf002); return;
	case 0x9e: flags = (u16)((flags & 0xff00) | ((r[AX] >> 8) & 0xd5) | 2); return;
	case 0x9f: reg8(4) = (u8)flags; return;
	case 0xa0: reg8(0) = rd8(dseg(fetch16())); return;
	case 0xa1: r[AX] = rd16(dseg(fetch16())); return;
	case 0xa2: wr8(dseg(fetch16()), (u8)r[AX]); return;
	case 0xa3: wr16(dseg(fetch16()), r[AX]); return;
	case 0xa4: rep([&] { wr8(lin(ES, r[DI]), rd8(dseg(r[SI]))); r[SI] += dstep(1); r[DI] += dstep(1); }, false); return;
	case 0xa5: rep([&] { wr16(lin(ES, r[DI]), rd16(dseg(r[SI]))); r[SI] += dstep(2); r[DI] += dstep(2); }, false); return;
	case 0xa6: rep([&] { alu(7, rd8(dseg(r[SI])), rd8(lin(ES, r[DI])), false); r[SI] += dstep(1); r[DI] += dstep(1); }, true); return;
	case 0xa7: rep([&] { alu(7, rd16(dseg(r[SI])), rd16(lin(ES, r[DI])), true); r[SI] += dstep(2); r[DI] += dstep(2); }, true); return;
	case 0xa8: alu(4, r[AX] & 0xff, fetch8(), false); return;
	case 0xa9: alu(4, r[AX], fetch16(), true); return;
	case 0xaa: rep([&] { wr8(lin(ES, r[DI]), (u8)r[AX]); r[DI] += dstep(1); }, false); return;
	case 0xab: rep([&] { wr16(lin(ES, r[DI]), r[AX]); r[DI] += dstep(2); }, false); return;
	case 0xac: rep([&] { reg8(0) = rd8(dseg(r[SI])); r[SI] += dstep(1); }, false); return;
	case 0xad: rep([&] { r[AX] = rd16(dseg(r[SI])); r[SI] += dstep(2); }, false); return;
	case 0xae: rep([&] { alu(7, r[AX] & 0xff, rd8(lin(ES, r[DI])), false); r[DI] += dstep(1); }, true); return;
	case 0xaf: rep([&] { alu(7, r[AX], rd16(lin(ES, r[DI])), true); r[DI] += dstep(2); }, true); return;
	case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
		bool w = op & 1;
		modrm();
		int n = (op <= 0xc1) ? fetch8() : (op <= 0xd1 ? 1 : (r[CX] & 0xff));
		if (w) setrm16((u16)shift(reg_, rm16(), n, true));
		else setrm8((u8)shift(reg_, rm8(), n, false));
		return;
	}
	case 0xc2: { u16 n = fetch16(); ip = pop(); r[SP] += n; return; }
	case 0xc3: ip = pop(); return;
	case 0xc4: modrm(); r[reg_] = rd16(ea_); sr[ES] = rm16b(); return;
	case 0xc5: modrm(); r[reg_] = rd16(ea_); sr[DS] = rm16b(); return;
	case 0xc6: modrm(); setrm8(fetch8()); return;
	case 0xc7: modrm(); setrm16(fetch16()); return;
	case 0xc8: {    /* ENTER */
		u16 n = fetch16(); u8 lv = fetch8() & 31;
		push(r[BP]);
		u16 fp = r[SP];
		for (int i = 1; i < lv; i++) { r[BP] -= 2; push(rd16(lin(SS, r[BP]))); }
		if (lv) push(fp);
		r[BP] = fp; r[SP] -= n;
		return;
	}
	case 0xc9: r[SP] = r[BP]; r[BP] = pop(); return;
	case 0xca: { u16 n = fetch16(); ip = pop(); sr[CS] = pop(); r[SP] += n; return; }
	case 0xcb: ip = pop(); sr[CS] = pop(); return;
	case 0xcc: interrupt(3); return;
	case 0xcd: { u8 t = fetch8(); interrupt(t); return; }
	case 0xce: if (flags & OF) interrupt(4); return;
	case 0xcf: ip = pop(); sr[CS] = pop(); flags = (u16)((pop() & 0x0fd5) | 0xf002); return;
	case 0xd4: { u8 k = fetch8(); u8 al = (u8)r[AX]; if (!k) { interrupt(0); return; }
		r[AX] = (u16)((al / k) << 8 | (al % k)); szp8((u8)r[AX]); return; }
	case 0xd5: { u8 k = fetch8(); u8 al = (u8)(r[AX] + (r[AX] >> 8) * k); r[AX] = al; szp8(al); return; }
	case 0xd6: reg8(0) = (flags & CF) ? 0xff : 0; return;   /* SALC */
	case 0xd7: reg8(0) = rd8(dseg((u16)(r[BX] + (r[AX] & 0xff)))); return;
	case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
		modrm(); return;        /* ESC */
	case 0xe0: { int8_t d = (int8_t)fetch8(); r[CX]--; if (r[CX] && !(flags & ZF)) ip += d; return; }
	case 0xe1: { int8_t d = (int8_t)fetch8(); r[CX]--; if (r[CX] && (flags & ZF)) ip += d; return; }
	case 0xe2: { int8_t d = (int8_t)fetch8(); r[CX]--; if (r[CX]) ip += d; return; }
	case 0xe3: { int8_t d = (int8_t)fetch8(); if (!r[CX]) ip += d; return; }
	case 0xe4: reg8(0) = (u8)io_rd(fetch8(), false); return;
	case 0xe5: r[AX] = io_rd(fetch8(), true); return;
	case 0xe6: io_wr(fetch8(), r[AX] & 0xff, false); return;
	case 0xe7: io_wr(fetch8(), r[AX], true); return;
	case 0xe8: { u16 d = fetch16(); push(ip); ip += d; return; }
	case 0xe9: { u16 d = fetch16(); ip += d; return; }
	case 0xea: { u16 o = fetch16(), s = fetch16(); ip = o; sr[CS] = s; return; }
	case 0xeb: { int8_t d = (int8_t)fetch8(); ip += d; return; }
	case 0xec: reg8(0) = (u8)io_rd(r[DX], false); return;
	case 0xed: r[AX] = io_rd(r[DX], true); return;
	case 0xee: io_wr(r[DX], r[AX] & 0xff, false); return;
	case 0xef: io_wr(r[DX], r[AX], true); return;
	case 0xf4: halted = true; return;
	case 0xf5: flags ^= CF; return;
	case 0xf6: case 0xf7: {
		bool w = op & 1;
		modrm();
		switch (reg_) {
		case 0: case 1:
			if (w) alu(4, rm16(), fetch16(), true); else alu(4, rm8(), fetch8(), false);
			return;
		case 2: if (w) setrm16((u16)~rm16()); else setrm8((u8)~rm8()); return;
		case 3:
			if (w) setrm16((u16)alu(5, 0, rm16(), true)); else setrm8((u8)alu(5, 0, rm8(), false));
			return;
		case 4:
			if (w) { u32 p = (u32)r[AX] * rm16(); r[AX] = (u16)p; r[DX] = (u16)(p >> 16); setf(CF | OF, r[DX] != 0); }
			else { u16 p = (u16)((r[AX] & 0xff) * rm8()); r[AX] = p; setf(CF | OF, (p >> 8) != 0); }
			return;
		case 5:
			if (w) { int32_t p = (int32_t)(int16_t)r[AX] * (int16_t)rm16(); r[AX] = (u16)p; r[DX] = (u16)((u32)p >> 16); setf(CF | OF, p != (int16_t)p); }
			else { int16_t p = (int16_t)((int8_t)r[AX] * (int8_t)rm8()); r[AX] = (u16)p; setf(CF | OF, p != (int8_t)p); }
			return;
		case 6: if (w) div16(false); else div8(false, at); return;
		case 7: if (w) div16(true); else div8(true, at); return;
		}
		return;
	}
	case 0xf8: flags &= ~CF; return;
	case 0xf9: flags |= CF; return;
	case 0xfa: flags &= ~IF; return;
	case 0xfb: flags |= IF; inhibit_ = true; return;
	case 0xfc: flags &= ~DF; return;
	case 0xfd: flags |= DF; return;
	case 0xfe:
		modrm();
		if (reg_ < 2) { setrm8((u8)incdec(rm8(), reg_ == 1, false)); return; }
		stop(op, at); return;
	case 0xff:
		modrm();
		switch (reg_) {
		case 0: setrm16((u16)incdec(rm16(), false, true)); return;
		case 1: setrm16((u16)incdec(rm16(), true, true)); return;
		case 2: { u16 t = rm16(); push(ip); ip = t; return; }
		case 3: { u16 o = rd16(ea_), s = rm16b(); push(sr[CS]); push(ip); sr[CS] = s; ip = o; return; }
		case 4: ip = rm16(); return;
		case 5: ip = rd16(ea_); sr[CS] = rm16b(); return;
		case 6: push(rm16()); return;
		}
		stop(op, at); return;
	}
	stop(op, at);
}

}

#endif
