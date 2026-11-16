/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v34_x2.h - USRobotics' x2 in the V.34 pump (dp_v34.cpp), the server
 *               (the digital modem, answering).  Phase 1 V.8, our JM's
 *               carrier turned over between its messages; phase 2
 *               V.34's, our INFO0a marked as an x2 server's (bits 23
 *               and 24), L1 alone, then the client's 7 bit marker
 *               instead of its probing and INFO1c.  Phases 3 and 4: the
 *               client's V.34 signals up (S, S-bar, PP, TRN, J; after a
 *               silence S, S-bar, TRN, its MP and E at 16 points), our
 *               PCM training (X2Train) down from its J: the preamble,
 *               TRN until its phase 4, 09B0, ones, the alignment, frames
 *               of ones until its MP, our record until its E.  Then the
 *               data: X2Map's frames down, V.34 (expanded shaping, 64
 *               states) up.  Included by dp_v34.cpp only, after its
 *               states.  See docs/dsp/x2-details.md.
 *
 */

#ifndef V34_X2_H
#define V34_X2_H


/* the I-modem's marker 12.7 s into the data (frames from the first in
   the data levels), its records after it */
#define X2_MARK_AT      16882
#define X2_MARK_N       9
/* the I-modem's next rate: one frame after its 43 records */
#define X2_SWITCH_AFTER (X2_MARK_N + 43 * 4 + 1)
/* ones in place of the data from that frame to 681 into the next rate
   (both its calls; the Courier takes no data at the next rate
   without them) */
#define X2_ONES_AFTER   681
/* our marker as the answer to the client's MP when it renegotiates
   (its S about 50 ms before the I-modem's marker); without one that
   many frames (12 s) after the I-modem's. A marker before its S makes
   it renegotiate on 16 points without an MP, and it hangs up; in one
   call behind a 6 dB pad its S came 3 s late */
#define X2_MARK_WAIT    16000
/* (a marker's or switch's frame that does not come) */
#define X2_NEVER        INT32_C(0x3fffffff)
/* the data's first frame by that count: after the start's 680 in the
   data levels */
#define X2_START_FR     680

/* the x2 part of the pump */
template <class P>
struct V34X2 {
	s16 x2;                 /* offer x2 as the server */
	s16 x2_on;              /* the client's marker came */
	s16 x2_mark;            /* its 7 bits: the carrier (bit 0), its
				   symbol rate (bits 4 to 6) */
	s16 x2_jm_sign;         /* V.8: our JM's carrier, + or - */
	X2Train x2t;
	s32 x2_t0;              /* when the state began (total) */
	s16 x2_quiet;           /* the client went quiet after its J */
	s16 x2_mp_in;           /* its MP is in */
	s16 x2_up;              /* the upstream's rate, N * 2400 */
	s16 x2_rec_e;           /* our records sent at its E (-1 before) */
	s16 x2_reneg;           /* the client renegotiates its rate */
	s16 x2_rec_out;         /* our record in the data, 4 frames of it
				   (its bit next), -1 none */
	unsigned char x2_rec[96];
	s32 x2_fr;              /* frames since the first in the data
				   levels */
	s16 x2_rec_left;        /* records still to send (at the marker) */
	s32 x2_mark_fr;         /* our marker's frame (sooner as the
				   answer to the client's MP) */
	s32 x2_switch_fr;       /* our next rate's frame */
	const X2Rate *x2_cur;   /* the data's rate */
	const X2Rate *x2_next;  /* the next rate's (at x2_switch_fr) */
	s16 x2_down;            /* the client's MP: the most it takes */
	s16 x2_mode;            /* and the digital pad it measured (0 to
				   2: none, 3 dB, 6 dB) */
	s32 x2_ones;            /* frames of ones still to send */
	unsigned char x2_raw[X2_FRAME];
	unsigned char x2_bits[X2_MAXBITS];

	/* the client (the analogue modem, calling): x2 offered (+MS=X2 on
	   an analogue line at 9600 Hz; DSP_X2C=0 leaves it out), the
	   server's INFO0 marked so, the line into x2rx */
	s16 x2c;
	s16 x2c_on;
	s16 x2_rx;
	X2Rx x2rx;
	s32 x2c_t0;             /* when the state began (total) */
	s32 x2c_quiet;          /* data mode samples without the signal */
	s32 x2c_rn_at;          /* our renegotiation, samples into the
				   data (-1 none) */
	s16 x2c_rn_n;           /* the rate it asks for */

	s16 x2_start();
	s16 x2_step(s32 c);
	s16 x2_sample();
	s16 x2_data_setup();
	void x2c_marker();
	s16 x2c_start();
	s16 x2c_data_setup();
	void x2c_mp(s16 down, u16 mask);
	s16 x2c_reneg(s32 c);
	s16 x2c_step(s32 c);
};

/* the client's marker: its V.34 transmitter at the symbol rate and on
   the carrier it says (GPA, as an answerer's), our PCM silent until its
   J */
template <class P>
s16 V34X2<P>::x2_start()
{
	P &self = static_cast<P &>(*this);
	s16 b = (x2_mark >> 4) & 7;
	if (b >= V34_NBAUDS)
		b = V34_BAUD_3200;
	self.baud_rx = b;
	self.high_rx = x2_mark & 1;
	if (self.prx.init(self.baud_rx, self.high_rx, 1))
		return -1;
	self.prx_on = 1;
	self.prx.x2 = 1;
	self.txm = TXM_SILENCE;
	x2t.init();
	x2t.data_rate = &x2_rate_b;
	x2_on = 1;
	x2_quiet = 0;
	x2_mp_in = 0;
	x2_up = 0;
	x2_rec_e = -1;
	x2_reneg = 0;
	x2_rec_out = -1;
	x2_rec_left = 0;
	x2_fr = X2_START_FR - 1;
	/* (test: DSP_X2_MARK_AT=<frame> for another) */
	x2_mark_fr = env_int("DSP_X2_MARK_AT", X2_MARK_AT + X2_MARK_WAIT);
	x2_switch_fr = x2_mark_fr + X2_SWITCH_AFTER;
	x2_cur = &x2_rate_b;
	/* (without an MP that says, the rate of the tests' Courier) */
	x2_next = x2_rate_for(14);
	x2_down = 0;
	x2_mode = 0;
	x2_ones = 0;
	x2_t0 = self.total;
	/* PCM once our tone is out of the converters */
	self.pcm_tx = 0;
	self.set_state(X3_WAIT);
	DSP_DBG("v34: x2 marker %#04x: phase 3, the client at %.0f baud, "
		"%s carrier\n", x2_mark, v34_symbol_rate(self.baud_rx),
		self.high_rx ? "high" : "low");
	return 0;
}

/* the client's MP (x2.h's four words): the rates both ways.  Down: its
   word 0's bits 2 to 5 the most it takes (1 to 15: the start-up's MP
   has 1, its renegotiations' 8 to 14 in the I-modem's calls; the next
   rate by x2_rate_for(), which knows all 15; test: DSP_X2_DOWN_MAX=<n>
   takes no more than n); up: the highest rate of its word 1's mask
   that our record allows (12), at expanded shaping, 64 states and
   non-linear encoding */
template <class P>
s16 V34X2<P>::x2_data_setup()
{
	P &self = static_cast<P &>(*this);
	const u16 *w = self.prx.x2_w;
	V34Params p;
	s16 down = (w[0] >> 2) & 15, mode = (w[0] >> 11) & 15,
		md = w[3] >> 8, n;
	u16 mask = static_cast<u16>(w[1] & 0x0fff);
	/* (no line probing to go by: our record's 12 at most; test:
	   DSP_V34_RX_MAX=<n> as for V.34) */
	s16 max = 12;
	if (s32 m = env_int("DSP_V34_RX_MAX"); m > 0 && max > m)
		max = static_cast<s16>(m);
	/* (the Quad's firmware builds no levels for another pad) */
	if (down < 1 || md != x2_rate_b.MD || mode >= X2_MODES)
		return -1;
	if (s32 m = env_int("DSP_X2_DOWN_MAX"); m > 0 && down > m)
		down = static_cast<s16>(m);
	x2_down = down;
	x2_mode = mode;
	for (n = max; n >= 1; n--)
		if ((mask >> (n - 1)) & 1 &&
		    v34_rates[self.prx.baud][n - 1].b)
			break;
	if (n < 1 || p.init(self.prx.baud, n, 1))
		return -1;
	p.states = 64;
	/* the client's encoder is non-linear (9.7/V.34) */
	p.set_nonlinear(0.3125);
	self.prx.start_data(&p, CoreBits{ self.modem });
	x2_up = n;
	self.rate_rx = p.rate;
	self.rate_tx = (x2_cur->B + x2_cur->MD) * 8000 / X2_FRAME;
	DSP_DBG("v34: x2 rates: down %" PRId32 ", up %" PRId32 " (the "
		"client's MP: down %d, mask %04x, %d signs, pad %d)\n",
		self.rate_tx, self.rate_rx, down, w[1], md, mode);
	return 0;
}

/* the next sample down */
template <class P>
s16 V34X2<P>::x2_sample()
{
	P &self = static_cast<P &>(*this);
	unsigned char code;
	s16 s0 = x2t.seg;
	if (x2t.seg == X2_DATA) {
		if (!x2t.pos)
			x2_fr++;
		if (!x2t.pos && x2_fr == x2_switch_fr - 1)
			x2_ones = 1 + X2_ONES_AFTER;
		if (!x2t.pos && x2_fr == x2_switch_fr) {
			x2_cur = x2_next;
			x2t.map.init(*x2_cur);
			self.rate_tx = static_cast<s32>(x2t.map.frame_bits()) *
				8000 / X2_FRAME;
			self.set_rates(self.rate_tx, self.rate_rx);
			DSP_DBG("v34: x2: down %" PRId32 " from here (%" PRId32
				" ms)\n", self.rate_tx,
				self.total * 1000 / self.srate);
		}
		if (!x2t.pos && x2_fr >= x2_mark_fr &&
		    x2_fr < x2_mark_fr + X2_MARK_N) {
			/* the I-modem's: 8 frames +++---, one ---+++, at
			   the rate's largest level in each position */
			s16 k = x2_fr == x2_mark_fr + X2_MARK_N - 1;
			for (s16 i = 0; i < X2_FRAME; i++)
				x2_raw[i] = x2_code(*x2_cur, i, 0) &
					((i < 3) != k ? 0xff : 0x7f);
			x2t.raw = x2_raw;
			if (x2_fr == x2_mark_fr)
				DSP_DBG("v34: x2: our marker (%" PRId32
					" ms)\n", self.total * 1000 /
					self.srate);
			if (x2_fr == x2_mark_fr + X2_MARK_N - 1) {
				/* then our record: 10 without the
				   acknowledge bit, 33 with it */
				u16 w[3] = { static_cast<u16>(x2t.rec_w[0] &
							      0x7fff),
					     x2t.rec_w[1], x2t.rec_w[2] };
				memset(x2_rec, 0, sizeof x2_rec);
				x2_record(w, 3, x2_rec);
				x2_rec_out = 0;
				x2_rec_left = 43;
			}
		} else if (!x2t.pos && x2_rec_out >= 0) {
			/* our record in place of the data (a renegotiation) */
			s16 nb = x2t.map.frame_bits();
			memcpy(x2_bits, x2_rec + x2_rec_out,
			       static_cast<size_t>(nb));
			x2_rec_out = static_cast<s16>((x2_rec_out + nb) %
						      sizeof x2_rec);
			if (!x2_rec_out && x2_rec_left > 0) {
				if (--x2_rec_left == 33) {
					memset(x2_rec, 0, sizeof x2_rec);
					x2_record(x2t.rec_w, 3, x2_rec);
				}
				if (!x2_rec_left) {
					DSP_DBG("v34: x2: our records sent (%"
						PRId32 " ms)\n", self.total *
						1000 / self.srate);
					x2_rec_out = -1;
				}
			} else if (!x2_reneg && !x2_rec_out && !x2_rec_left)
				/* the client's E: the data again after the
				   record */
				x2_rec_out = -1;
		} else if (!x2t.pos && x2_ones > 0) {
			/* ones about the next rate */
			x2_ones--;
			memset(x2_bits, 1,
			       static_cast<size_t>(x2t.map.frame_bits()));
		} else if (!x2t.pos)
			CoreBits{ self.modem }.get(x2_bits,
						   x2t.map.frame_bits());
		code = x2t.next(x2_bits);
		if (x2t.pos == 1 && x2_fr == x2_mark_fr + X2_MARK_N - 1) {
			/* the scrambler from zero after the marker, and
			   the records and the frame after them in the
			   start-up's data levels (B) at any rate: so the
			   I-modem sends them after a second marker too */
			x2t.map.scr = 0;
			x2t.map.last = 1;
			x2t.map.init(*x2_rate_for(1, x2_mode));
		}
	} else
		code = x2t.next();
	if (x2t.seg != s0)
		DSP_DBG("v34: x2: segment %d\n", x2t.seg);
	/* the code's mu-law value, whatever the network's law: on an
	   A-law line the codec sends the A-law octet nearest it.  (The
	   Quad's A-law mode sends the A-law octet of the same code number
	   instead; with that the emulated Courier sent no MP, with this it
	   reached LAPM and then cleared down: x2-details.md) */
	s16 v = v90_ucode_linear(0, code & 0x7f);
	return code & 0x80 ? v : static_cast<s16>(-v);
}

/* the server's phases 3 and 4 and data, c samples into the state */
template <class P>
s16 V34X2<P>::x2_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	V34Rx &r = self.prx;
	switch (self.state) {
	case P2A_X2MARK:
		/* the client's marker after our L1 (it has neither L1 nor
		   INFO1c); a V.34 caller waits for L2 instead */
		if (self.ev_info1) {
			if (x2_start())
				return DPSTAT_ERROR;
		} else if (c >= ms2samples(800, self.srate)) {
			DSP_DBG("v34: x2: no marker, V.34 (L2)\n");
			x2 = 0;
			self.txm = TXM_PROBE;
			self.set_state(P2A_L2);
		}
		break;
	case X3_WAIT:
		if (!self.pcm_tx && c >= ms2samples(40, self.srate))
			self.pcm_tx = 1;
		/* the client's phase 3 to its J (or its end) */
		if (r.ev_j || (r.phase4 && r.stage == V34_RX_IDLE)) {
			DSP_DBG("v34: x2: the client's %s; our training\n",
				r.ev_j ? "J" : "phase 3 over");
			x2t.segment(X2_PRE);
			x2_t0 = self.total;
			self.set_state(X3_TRAIN);
		} else if (self.total - x2_t0 >= ms2samples(6000,
							      self.srate)) {
			DSP_DBG("v34: x2: no phase 3 from the client\n");
			return self.fallback("no x2 phase 3");
		}
		break;
	case X3_TRAIN:
		/* the client quiet after its J: its phase 4 at 16 points */
		if (!x2_quiet && r.stage == V34_RX_IDLE) {
			x2_quiet = 1;
			r.trn16 = 1;
			r.ev_jp = 1;
			r.ls_n = 0;
		}
		/* TRN, then 09B0: the client's phase 4 */
		if (x2t.seg > X2_TRN) {
			self.set_state(X3_SEQ);
		} else if (self.total - x2_t0 >= ms2samples(8000,
							      self.srate)) {
			DSP_DBG("v34: x2: no phase 4 from the client\n");
			return self.fallback("no x2 phase 4");
		}
		break;
	case X3_SEQ:
		if (r.ev_mp && !x2_mp_in) {
			x2_mp_in = 1;
			if (x2_data_setup()) {
				DSP_DBG("v34: x2: no common rate\n");
				return self.fallback("no common data rate");
			}
			/* the start-up's data levels (B) across its pad */
			x2t.data_rate = x2_cur = x2_rate_for(1, x2_mode);
			x2_next = x2_rate_for(14, x2_mode);
		}
		/* frames of ones (X2_SYNC_MIN codes at least) until its MP; then our
		   record */
		if (x2t.seg == X2_SYNC && x2_mp_in &&
		    !x2t.go) {
			x2t.go = 1;
			x2_t0 = self.total;
			self.set_state(X4_REC);
		} else if (self.total - x2_t0 >= ms2samples(12000,
							      self.srate)) {
			DSP_DBG("v34: x2: no MP from the client\n");
			return self.fallback("no MP");
		}
		break;
	case X4_REC:
		/* our record until its E */
		if (x2t.seg == X2_REC && r.ev_e && x2_rec_e < 0) {
			DSP_DBG("v34: x2: the client's E after %d records\n",
				x2t.rec_n);
			x2_rec_e = x2t.rec_n;
		}
		if (x2_rec_e >= 0 && !x2t.go)
			x2t.go = 1;
		if (x2t.seg == X2_DATA) {
			self.set_state(X4_DATA);
			self.set_rates(self.rate_tx, self.rate_rx);
			self.connected = 1;
			DSP_DBG("v34: x2 connect " RATE_FMT "\n",
				self.rate_tx, self.rate_rx);
			self.stat_dp(DP_X2);
			return DPSTAT_CONNECT;
		}
		if (!x2t.go && self.total - x2_t0 >=
		    ms2samples(8000, self.srate)) {
			DSP_DBG("v34: x2: no E from the client\n");
			return self.fallback("no E");
		}
		break;
	case X4_DATA:
		/* the client's renegotiation: its S, S-bar, TRN, MP and E on
		   4 points (its upstream rate); our downstream goes on */
		if (r.ev_reneg) {
			DSP_DBG("v34: x2: the client renegotiates (%" PRId32
				" ms)\n", self.total * 1000 / self.srate);
			r.ev_reneg = 0;
			x2_reneg = 1;
			x2_mp_in = 0;
			x2_t0 = self.total;
		}
		if (!x2_reneg)
			break;
		if (r.ev_mp && !x2_mp_in) {
			x2_mp_in = 1;
			if (x2_data_setup()) {
				DSP_DBG("v34: x2: no common rate\n");
				return DPSTAT_ERROR;
			}
			/* the rate it takes, when we know its levels, is the
			   answer (our marker, records and that rate): the
			   I-modem does so at each renegotiation, to a higher
			   rate (12, then 14) or the same one (8 three times
			   on a 18 dB line); one we do not know, our record
			   until its E */
			const X2Rate *nr = x2_rate_for(x2_down, x2_mode);
			if (x2_fr + 1 < x2_mark_fr && !nr) {
				/* none: no marker, and no next rate */
				x2_mark_fr = x2_switch_fr = X2_NEVER;
			}
			if (nr && (x2_fr + 1 < x2_mark_fr ||
				   x2_fr >= x2_switch_fr + X2_ONES_AFTER)) {
				x2_next = nr;
				x2_mark_fr = x2_fr + 1;
				x2_switch_fr = x2_mark_fr + X2_SWITCH_AFTER;
			} else if ((x2_mark_fr == X2_NEVER ||
				    x2_fr >= x2_mark_fr + X2_MARK_N) &&
				   !x2_rec_left) {
				/* our record (its acknowledge bit set) in
				   the data until its E */
				memset(x2_rec, 0, sizeof x2_rec);
				x2_record(x2t.rec_w, 3, x2_rec);
				x2_rec_out = 0;
			}
		}
		if (x2_mp_in && r.ev_e) {
			x2_reneg = 0;
			self.set_rates(self.rate_tx, self.rate_rx);
			DSP_DBG("v34: x2 renegotiated " RATE_FMT "\n",
				self.rate_tx, self.rate_rx);
		} else if (self.total - x2_t0 >= ms2samples(5000,
							      self.srate)) {
			DSP_DBG("v34: x2: the client's renegotiation timed "
				"out\n");
			return DPSTAT_ERROR;
		}
		break;
	default:
		break;
	}
	return 0;
}

/*
 *    the client's phases 2 to 4 (the analogue modem, calling)
 */

/* the Courier's MP at the start: the start-up's rate (1) down, 13 up
   (word 0), the upstream's mask, 5 signs (word 3's high octet) */
#define X2C_MP_W0       0x0344
#define X2C_MP_MASK     0x03fe
#define X2C_MP_W3       0x0500
/* the upstream rate the server's record allows at most */
#define X2C_UP_MAX      12

/* the Courier's renegotiation: about 12.7 s into the data, its MP with
   the rate it measured and this mask */
#define X2C_RENEG_MS    12700
#define X2C_RN_MASK     0x1ffe
#define X2C_RN_TRN      512

/* our MP's words: the rate down (1 to 15), 13 up, the pad we measured,
   the mask up, 5 signs */
template <class P>
void V34X2<P>::x2c_mp(s16 down, u16 mask)
{
	P &self = static_cast<P &>(*this);
	V34Tx &t = self.ptx;
	t.x2_w[0] = static_cast<u16>((X2C_MP_W0 & ~0x783c) | down << 2 |
				     x2rx.mode << 11);
	t.x2_w[1] = mask;
	/* (the Courier's behind a pad) */
	t.x2_w[2] = x2rx.mode ? 0x8888 : 0;
	t.x2_w[3] = static_cast<u16>(X2C_MP_W3 | (x2rx.mode ? 0x88 : 0));
	t.ack_mp = 0;
	t.mp_sent = 0;
}

/* our marker in place of L1, L2 and INFO1c, once the server's L1 is
   over: the high carrier (bit 0), 6 (bits 1 to 3), our symbol rate
   (bits 4 to 6), 3200 baud as the Courier's: 4d */
template <class P>
void V34X2<P>::x2c_marker()
{
	P &self = static_cast<P &>(*this);
	s16 m = static_cast<s16>(1 | 6 << 1 | V34_BAUD_3200 << 4);
	self.cctx.info_start();
	self.cctx.put(static_cast<u32>(m), 7);
	self.cctx.info_end();
	self.cc_send();
	DSP_DBG("v34: x2: our marker %#04x\n", m);
}

/* after our marker: our V.34 transmitter at 3200 baud on the high
   carrier with the answerer's scrambler (GPA), J asking for 16 points;
   the PCM receiver on the line */
template <class P>
s16 V34X2<P>::x2c_start()
{
	P &self = static_cast<P &>(*this);
	self.baud_tx = V34_BAUD_3200;
	self.high_tx = 1;
	self.ptx.init(self.baud_tx, self.high_tx, 0);
	self.ptx.jword = 0x89b0;
	self.ptx.x2 = 1;
	self.txm = TXM_PRIMARY;
	x2rx.init(self.alaw, self.line_rate);
	x2_rx = 1;
	x2c_quiet = 0;
	x2c_t0 = self.total;
	if (self.rt)
		self.rt->pcm = 1;
	self.set_state(XC3_WAIT);
	DSP_DBG("v34: x2 phase 3, the client at %.0f baud, high carrier\n",
		v34_symbol_rate(self.baud_tx));
	return 0;
}

/* the server's record: the rates both ways.  Up: the highest of our
   mask that the record allows (as x2_data_setup()), at expanded
   shaping, 64 states and non-linear encoding; down: the start-up's */
template <class P>
s16 V34X2<P>::x2c_data_setup()
{
	P &self = static_cast<P &>(*this);
	V34Params p;
	s16 n;
	u16 mask = static_cast<u16>(self.ptx.x2_w[1] & 0x0fff);
	for (n = X2C_UP_MAX; n >= 1; n--)
		if ((mask >> (n - 1)) & 1 &&
		    v34_rates[self.ptx.baud][n - 1].b)
			break;
	if (n < 1 || p.init(self.ptx.baud, n, 1))
		return -1;
	p.states = 64;
	p.set_nonlinear(0.3125);
	self.ptx.start_data(&p, CoreBits{ self.modem });
	self.rate_tx = p.rate;
	if (!self.connected)
		self.rate_rx = static_cast<s32>(x2rx.lv_d.r->B +
						x2rx.lv_d.r->MD) * 8000 /
			X2_FRAME;
	DSP_DBG("v34: x2 rates: up %" PRId32 ", down %" PRId32 " (the "
		"server's record %04x %04x %04x)\n", self.rate_tx,
		self.rate_rx, x2rx.rec_w[0], x2rx.rec_w[1], x2rx.rec_w[2]);
	return 0;
}

/* our renegotiation in the data (XC5_*), c samples into the state:
   S, S-bar, TRN, our MP until a record of the server's after its
   marker, MP', E; then the data again */
template <class P>
s16 V34X2<P>::x2c_reneg(s32 c)
{
	P &self = static_cast<P &>(*this);
	V34Tx &t = self.ptx;
	(void)c;
	switch (self.state) {
	case XC5_S:
		if (t.n >= 128)
			self.tx_next(XC5_SBAR, V34_TX_SBAR);
		break;
	case XC5_SBAR:
		if (t.n >= 16)
			self.tx_next(XC5_TRN, V34_TX_TRN);
		break;
	case XC5_TRN:
		if (t.n >= X2C_RN_TRN)
			self.tx_next(XC5_MP, V34_TX_MP);
		break;
	case XC5_MP:
		/* our MP until the server's marker and a record after it;
		   then MP' and E */
		if (x2rx.ev_rn_rec && !t.ack_mp) {
			if (x2c_data_setup()) {
				DSP_DBG("v34: x2: no common rate\n");
				return DPSTAT_ERROR;
			}
			t.ack_mp = 1;
		}
		if (t.ack_mp && t.mp.ack && t.pos >= t.nbits)
			self.tx_next(XC5_E, V34_TX_E);
		break;
	case XC5_E:
		if (t.seg == V34_TX_DATA) {
			x2rx.ev_rn_rec = 0;
			self.set_state(XC4_DATA);
			self.set_rates(self.rate_tx, self.rate_rx);
			DSP_DBG("v34: x2: renegotiated, up %" PRId32 "\n",
				self.rate_tx);
		}
		break;
	default:
		break;
	}
	if (self.state != XC4_DATA && self.total - x2c_t0 >=
	    ms2samples(5000, self.srate)) {
		DSP_DBG("v34: x2: our renegotiation timed out\n");
		return DPSTAT_ERROR;
	}
	return 0;
}

/* the client's phases 3 and 4 and data, c samples into the state */
template <class P>
s16 V34X2<P>::x2c_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	V34Tx &t = self.ptx;
	switch (self.state) {
	case XC2_MARK:
		/* our tone for the marker's phase reference, then it */
		if (!self.cc_busy() && self.cctx.n == 0 &&
		    c >= 2 * self.srate / CC_BAUD)
			x2c_marker();
		else if (self.cctx.n && !self.cc_busy()) {
			self.cc_silence();
			self.cctx.reset();
			if (x2c_start())
				return DPSTAT_ERROR;
		}
		break;
	case XC3_WAIT:
		if (c >= ms2samples(70, self.srate))
			self.tx_next(XC3_S, V34_TX_S);
		break;
	case XC3_S:
		if (t.n >= 128)
			self.tx_next(XC3_SBAR, V34_TX_SBAR);
		break;
	case XC3_SBAR:
		if (t.n >= 16)
			self.tx_next(XC3_PP, V34_TX_PP);
		break;
	case XC3_PP:
		if (t.n >= 288)
			self.tx_next(XC3_TRN, V34_TX_TRN);
		break;
	case XC3_TRN:
		if (t.n >= TRN3_LEN) {
			self.tx_next(XC3_J, V34_TX_J);
			x2c_t0 = self.total;
		}
		break;
	case XC3_J:
		/* J until the server's preamble has begun (the Courier's
		   about 150 ms into it), then silence */
		if (x2rx.ev_pre && t.n % 8 == 0 &&
		    self.total - x2c_t0 >= ms2samples(150, self.srate)) {
			x2c_t0 = self.total;
			self.tx_next(XC3_QUIET, V34_TX_SILENCE);
			DSP_DBG("v34: x2: the server's preamble; quiet\n");
		} else if (!x2rx.ev_pre) {
			x2c_t0 = self.total;
			if (c >= ms2samples(3000, self.srate)) {
				DSP_DBG("v34: x2: no preamble after our J\n");
				return self.fallback("no x2 training");
			}
		}
		break;
	case XC3_QUIET:
		/* the server's TRN until its J words: our phase 4 (16
		   points) */
		if (x2rx.k >= X2RX_K_JS) {
			t.trn16 = 1;
			x2c_mp(1, X2C_MP_MASK);
			self.tx_next(XC4_S, V34_TX_S);
		} else if (c >= ms2samples(5000, self.srate)) {
			DSP_DBG("v34: x2: no TRN from the server\n");
			return self.fallback("no x2 TRN");
		}
		break;
	case XC4_S:
		if (x2rx.ev_js < 0) {
			DSP_DBG("v34: x2: the J words do not decode\n");
			return self.fallback("no x2 J words");
		}
		if (t.n >= 128)
			self.tx_next(XC4_SBAR, V34_TX_SBAR);
		break;
	case XC4_SBAR:
		if (t.n >= 16)
			self.tx_next(XC4_TRN, V34_TX_TRN);
		break;
	case XC4_TRN:
		if (t.n >= TRN4_LEN) {
			/* the pad the sync frames show, in our MP */
			x2rx.measure_pad();
			x2c_mp(1, X2C_MP_MASK);
			self.tx_next(XC4_MP, V34_TX_MP);
			x2c_t0 = self.total;
		}
		break;
	case XC4_MP:
		/* our MP until the server's record, then MP' and E after
		   a whole one */
		if (x2rx.ev_rec && !t.ack_mp) {
			if (x2c_data_setup()) {
				DSP_DBG("v34: x2: no common rate\n");
				return self.fallback("no common data rate");
			}
			t.ack_mp = 1;
		}
		if (t.ack_mp && t.mp.ack && t.pos >= t.nbits) {
			self.tx_next(XC4_E, V34_TX_E);
			x2rx.e_sent = 1;
			x2c_t0 = self.total;
		} else if (self.total - x2c_t0 >= ms2samples(10000,
							      self.srate)) {
			DSP_DBG("v34: x2: no record from the server\n");
			return self.fallback("no x2 record");
		}
		break;
	case XC4_E:
		if (t.seg == V34_TX_DATA && x2rx.ev_data) {
			self.set_state(XC4_DATA);
			self.set_rates(self.rate_tx, self.rate_rx);
			self.connected = 1;
			DSP_DBG("v34: x2 connect " RATE_FMT " (client)\n",
				self.rate_tx, self.rate_rx);
			self.stat_dp(DP_X2);
			/* our renegotiation (test: DSP_X2C_RENEG=<ms>, 0
			   none) */
			{
				s32 ms = env_int("DSP_X2C_RENEG", X2C_RENEG_MS);
				x2c_rn_at = ms > 0 ? ms2samples(ms, self.srate) :
					-1;
			}
			return DPSTAT_CONNECT;
		} else if (self.total - x2c_t0 >= ms2samples(4000,
							      self.srate)) {
			DSP_DBG("v34: x2: no start from the server\n");
			return self.fallback("no x2 start");
		}
		break;
	case XC4_DATA:
	case XC5_S:
	case XC5_SBAR:
	case XC5_TRN:
	case XC5_MP:
	case XC5_E:
		/* the PCM signal gone: carrier lost */
		x2c_quiet = x2rx.dpow < 1e-3f * x2rx.level * x2rx.level ?
			x2c_quiet + 1 : 0;
		self.stat_carrier(!x2c_quiet);
		if (x2c_quiet >= self.carrier_loss) {
			DSP_DBG("v34: x2: carrier lost\n");
			return DPSTAT_ERROR;
		}
		/* the next rate's data (ours, or the server's own) */
		if (x2rx.ev_rn_data) {
			x2rx.ev_rn_data = 0;
			self.rate_rx = static_cast<s32>(x2rx.lv_d.r->B +
							x2rx.lv_d.r->MD) *
				8000 / X2_FRAME;
			self.set_rates(self.rate_tx, self.rate_rx);
			DSP_DBG("v34: x2: down %" PRId32 " from here\n",
				self.rate_rx);
		}
		if (self.state != XC4_DATA)
			return x2c_reneg(c);
		if (c % ms2samples(1000, self.srate) == 0)
			DSP_DBG("v34: x2 data: error %.2f rms (%.1f dB), "
				"timing %+.2f ppm\n", sqrtf(x2rx.de_mse),
				10 * log10f(x2rx.dpow / (x2rx.de_mse + 1e-6f)),
				x2rx.ppm());
		if (x2c_rn_at >= 0 && c >= x2c_rn_at &&
		    x2rx.rn == X2Rx::RN_NONE) {
			/* the rate the sync frames' noise allows (test:
			   DSP_X2C_DOWN=<n> for another) */
			s16 n = static_cast<s16>(env_int("DSP_X2C_DOWN",
							 x2rx.best_rate()));
			x2c_rn_at = -1;
			if (n < 1 || n > 15 || !x2_rate_for(n, x2rx.mode))
				break;
			x2c_rn_n = n;
			x2rx.rn_want = n;
			x2c_mp(n, X2C_RN_MASK);
			t.trn16 = 0;
			x2c_t0 = self.total;
			DSP_DBG("v34: x2: our renegotiation: down %d (%d "
				"bit/s; the decisions' error %.2f rms)\n", n,
				(x2_rate_for(n, x2rx.mode)->B + 5) * 8000 /
				X2_FRAME, sqrtf(x2rx.de_mse));
			self.tx_next(XC5_S, V34_TX_S);
		}
		break;
	default:
		break;
	}
	return 0;
}

#endif
