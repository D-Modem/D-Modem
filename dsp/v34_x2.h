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
   word 0's bits 2 to 5 the most it takes (1 to 15, of which we have the
   first); up: the highest rate of its word 1's mask that our record
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
	self.rate_tx = (x2_rate_b.B + x2_rate_b.MD) * 8000 / X2_FRAME;
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
			CoreBits{ self.modem }.get(x2_bits,
						   x2t.map.frame_bits());
		code = x2t.next(x2_bits);
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
	default:
		break;
	}
	return 0;
}

#endif
