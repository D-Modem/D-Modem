/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hw/cadmus2.hpp - Conexant's USB HSF bridge ("Cadmus 2", the
 *    hsfusbcd2 module's hardware: USB 0572:1300 and the others), as far as
 *    the module's own use of it shows, for the interop harness
 *    (half_hsf.cpp with HSF_USB)
 *
 *    After its firmware is loaded the bridge runs scripts, byte code the
 *    host sends with vendor request 2 (CONTROL_SCRIPT, wValue 0xff01 or
 *    0xff02, wIndex the slot it runs in).  Each script's labels are byte
 *    offsets in it (the host resolves them before sending).  What a
 *    script reports comes back on the interrupt pipe:
 *
 *        c1 02 <slot> ff <slot> 00 <n> 00 <n bytes>
 *
 *    the bytes those the script pushed (the first its reply code, the
 *    last a status, 1).  The operations (all 34 of hsfusbcd2's scripts
 *    are made of these, their lengths checked against the scripts):
 *
 *      07 r m    a = B[r] & m          0b r v    B[r] = v
 *      0f r      B[r] = a              13 r m    B[r] &= ~m
 *      17 r m    B[r] |= m             29 r      a = B[r]
 *      52 r      a = X[r]              5a r v    X[r] = v
 *      56 r m    a = X[r] & m          5e r      X[r] = a
 *      62 r m    X[r] |= m             66 r m    X[r] &= ~m
 *      53 r      a = Y[r]              5b r v    Y[r] = v
 *      57 r m    a = Y[r] & m          5f r      Y[r] = a
 *      63 r m    Y[r] |= m             67 r m    Y[r] &= ~m
 *      51 v      a = var[v]            5d v      var[v] = a
 *      55 v m    a = var[v] & m        65 v m    var[v] |= m
 *      2a n      a = arg[n]            3a n      a |= arg[n]
 *      37 m      a &= m                39 v      a |= v
 *      2d n      a += n
 *      1b L      goto L                1c L v    if a == v goto L
 *      1d L v    if a != v goto L      20 L v    if a < v goto L
 *      22 L n    n times round: goto L (then on)
 *      19 L n .. call L with n arguments; 36 returns (or ends the script)
 *      3d t n    wait n ms (t 0x1e; 0xf0 and 0xfe: n set by the host)
 *      26        push a                27 v      push v
 *      28        send what was pushed (the notification)
 *      2e 34 35 44 45 46 47, 2f/30/31/32/33 n: the streams and the
 *                interrupts (33 n: a = 0, nothing pending)
 *
 *    B is the bridge's own registers (its GPIOs and its serial port to
 *    the codec: B[0x1a], B[0x1b] the word out, B[0x04] bit 5 ready), X
 *    and Y two register files behind it (the codec's and the line
 *    side's, it seems).  The meanings above are what the scripts' use of
 *    them suggests, not a datasheet's.
 */

#ifndef DMODEM_HW_CADMUS2_HPP
#define DMODEM_HW_CADMUS2_HPP

#include <stdio.h>
#include <string.h>

#include <deque>
#include <functional>
#include <vector>

namespace half {

class Cadmus2 {
public:
	typedef unsigned char u8;

	u8 B[256] = {}, X[256] = {}, Y[256] = {};
	int dbg = 0;
	/* the codec words written through the serial port (B[0x1a], B[0x1b]) */
	std::vector<unsigned> codec_words;
	/* the notifications to send */
	std::deque<std::vector<u8>> notes;

	Cadmus2()
	{
		B[0x04] = 0x20;         /* the codec port ready */
	}

	/* a CONTROL_SCRIPT request: the script, to run in slot idx */
	void script(unsigned val, unsigned idx, const std::vector<u8> &code)
	{
		(void)val;
		Ctx &c = slot(idx & 0xff);
		c = Ctx();
		c.code = code;
		c.slot = idx & 0xff;
		c.live = true;
		run(c);
	}

	/* each ms: the scripts waiting, on */
	void tick()
	{
		now++;
		for (auto &c : ctx)
			if (c.live && c.wait_until <= now)
				run(c);
	}

private:
	struct Ctx {
		std::vector<u8> code;
		unsigned pc = 0, slot = 0;
		bool live = false;
		u8 a = 0;
		u8 var[256] = {};
		std::vector<u8> out;
		std::vector<std::pair<unsigned, std::vector<u8>>> calls;
		std::vector<u8> args;
		unsigned loops[256] = {};
		unsigned long long wait_until = 0;
	};
	std::vector<Ctx> ctx;
	unsigned long long now = 0;

	Ctx &slot(unsigned s)
	{
		for (auto &c : ctx)
			if (c.slot == s)
				return c;
		ctx.emplace_back();
		ctx.back().slot = s;
		return ctx.back();
	}

	u8 at(Ctx &c, unsigned i) { return i < c.code.size() ? c.code[i] : 0x36; }

	void send(Ctx &c)
	{
		std::vector<u8> n = {0xc1, 0x02, (u8)c.slot, 0xff, (u8)c.slot,
				     0x00, (u8)c.out.size(), 0x00};
		n.insert(n.end(), c.out.begin(), c.out.end());
		notes.push_back(n);
		c.out.clear();
	}

	/* the script on until it waits or ends (a bound on the steps, for
	   one that only loops) */
	void run(Ctx &c)
	{
		for (int steps = 0; c.live && steps < 4096; steps++) {
			unsigned p = c.pc;
			u8 op = at(c, p), r = at(c, p + 1), v = at(c, p + 2);
			unsigned len = 1;
			switch (op) {
			case 0x07: c.a = B[r] & v; len = 3; break;
			case 0x0b: B[r] = v; len = 3; break;
			case 0x0f: B[r] = c.a; len = 2; break;
			case 0x13: B[r] &= ~v; len = 3; break;
			case 0x17: B[r] |= v; len = 3; break;
			case 0x29: c.a = B[r]; len = 2; break;
			case 0x52: c.a = X[r]; len = 2; break;
			case 0x5a: X[r] = v; len = 3; break;
			case 0x56: c.a = X[r] & v; len = 3; break;
			case 0x5e: X[r] = c.a; len = 2; break;
			case 0x62: X[r] |= v; len = 3; break;
			case 0x66: X[r] &= ~v; len = 3; break;
			case 0x53: c.a = Y[r]; len = 2; break;
			case 0x5b: Y[r] = v; len = 3; break;
			case 0x57: c.a = Y[r] & v; len = 3; break;
			case 0x5f: Y[r] = c.a; len = 2; break;
			case 0x63: Y[r] |= v; len = 3; break;
			case 0x67: Y[r] &= ~v; len = 3; break;
			case 0x51: c.a = c.var[r]; len = 2; break;
			case 0x5d: c.var[r] = c.a; len = 2; break;
			case 0x55: c.a = c.var[r] & v; len = 3; break;
			case 0x65: c.var[r] |= v; len = 3; break;
			case 0x2a: c.a = r && r <= c.args.size() ? c.args[r - 1] : 0; len = 2; break;
			case 0x3a: c.a |= r && r <= c.args.size() ? c.args[r - 1] : 0; len = 2; break;
			case 0x37: c.a &= r; len = 2; break;
			case 0x39: c.a |= r; len = 2; break;
			case 0x2d: c.a += r; len = 2; break;
			case 0x1b: c.pc = r; continue;
			case 0x1c: if (c.a == v) { c.pc = r; continue; } len = 3; break;
			case 0x1d: if (c.a != v) { c.pc = r; continue; } len = 3; break;
			case 0x20: if (c.a < v) { c.pc = r; continue; } len = 3; break;
			case 0x22:
				if (++c.loops[p & 0xff] < (v ? v : 256u)) {
					c.pc = r;
					continue;
				}
				c.loops[p & 0xff] = 0;
				len = 3;
				break;
			case 0x19: {
				unsigned n = v;
				std::vector<u8> args(c.code.begin() + std::min<size_t>(p + 3, c.code.size()),
						     c.code.begin() + std::min<size_t>(p + 3 + n, c.code.size()));
				c.calls.push_back({p + 3 + n, c.args});
				c.args = args;
				c.pc = r;
				continue;
			}
			case 0x36:
				if (!c.calls.empty()) {
					c.pc = c.calls.back().first;
					c.args = c.calls.back().second;
					c.calls.pop_back();
					continue;
				}
				c.live = false;
				return;
			case 0x3d:
				c.pc = p + 3;
				c.wait_until = now + (v ? v : 1);
				return;
			case 0x26: c.out.push_back(c.a); break;
			case 0x27: c.out.push_back(r); len = 2; break;
			case 0x28: send(c); break;
			case 0x2f: case 0x30: case 0x31: case 0x32:
				len = 2;
				break;
			case 0x33: c.a = 0; len = 2; break;
			case 0x2e: case 0x34: case 0x35:
			case 0x44: case 0x45: case 0x46: case 0x47:
				break;
			default:
				if (dbg)
					fprintf(stderr, "cadmus2: slot %u: op %02x at %u?\n",
						c.slot, op, p);
				c.live = false;
				return;
			}
			/* a word out to the codec when its low byte goes */
			if ((op == 0x0f || op == 0x0b) && r == 0x1b)
				codec_words.push_back((unsigned)B[0x1a] << 8 | B[0x1b]);
			c.pc = p + len;
		}
		/* a script still looping: on at the next ms */
		c.wait_until = now + 1;
	}
};

}

#endif
