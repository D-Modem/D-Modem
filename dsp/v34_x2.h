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
   many frames (3 s) after the I-modem's. A marker before its S makes
   it renegotiate on 16 points without an MP, and it hangs up */
#define X2_MARK_WAIT    4000
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
	s32 x2_ones;            /* frames of ones still to send */
	unsigned char x2_raw[X2_FRAME];
	unsigned char x2_bits[X2_MAXBITS];

	s16 x2_start();
	s16 x2_step(s32 c);
	s16 x2_sample();
	s16 x2_data_setup();
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
	x2_next = &x2_rate_c;
	x2_down = 0;
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
   has 1, its renegotiations' 12 or 14 in the I-modem's calls; the next
   rate by x2_rate_for(); test: DSP_X2_DOWN_MAX=<n> takes no more than
   n); up: the highest rate of its word 1's mask that our record
   allows (12), at expanded shaping, 64 states and non-linear encoding */
template <class P>
s16 V34X2<P>::x2_data_setup()
{
	P &self = static_cast<P &>(*this);
	const u16 *w = self.prx.x2_w;
	V34Params p;
	s16 down = (w[0] >> 2) & 15, md = w[3] >> 8, n;
	u16 mask = static_cast<u16>(w[1] & 0x0fff);
	/* (no line probing to go by: our record's 12 at most; test:
	   DSP_V34_RX_MAX=<n> as for V.34) */
	s16 max = 12;
	if (s32 m = env_int("DSP_V34_RX_MAX"); m > 0 && max > m)
		max = static_cast<s16>(m);
	if (down < 1 || md != x2_rate_b.MD)
		return -1;
	if (s32 m = env_int("DSP_X2_DOWN_MAX"); m > 0 && down > m)
		down = static_cast<s16>(m);
	x2_down = down;
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
		"client's MP: down %d, mask %04x, %d signs)\n", self.rate_tx,
		self.rate_rx, down, w[1], md);
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
				x2_raw[i] = k56_code(x2_cur->lv[i][0], 0) &
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
			/* the scrambler from zero after the marker */
			x2t.map.scr = 0;
			x2t.map.last = 1;
		}
	} else
		code = x2t.next();
	if (x2t.seg != s0)
		DSP_DBG("v34: x2: segment %d\n", x2t.seg);
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
			/* the rate it takes: one above ours is the answer
			   (our marker, records and that rate, as the
			   I-modem does again in a call whose first
			   renegotiation took 12 and its next 14); the
			   same or lower, our record until its E */
			const X2Rate *nr = x2_rate_for(x2_down);
			s16 up = nr && nr->B + nr->MD > x2_cur->B +
				x2_cur->MD;
			if (x2_fr + 1 < x2_mark_fr && !up) {
				/* none: no marker, and no next rate */
				x2_mark_fr = x2_switch_fr = X2_NEVER;
			}
			if (up && (x2_fr + 1 < x2_mark_fr ||
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

#endif
