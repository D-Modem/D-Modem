/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v34_k56.h - K56flex in the V.34 pump (dp_v34.cpp): the digital
 *                modem (the answerer).  Phase 1: V.8bis (its MRe, CRd,
 *                CL, MS and ACK(1)) before V.8; phase 2 V.34's, our
 *                INFO1a marked as K56flex's.  Phases 3 and 4: our PCM
 *                training (K56Train) down, the caller's V.34 signals up
 *                (S, S-bar, PP, TRN, J; J', TRN, MP, MP', E), then the
 *                data: K56Enc's frames down, V.34 up.  Included by
 *                dp_v34.cpp only, after its states and BitQ.  See
 *                docs/dsp/k56flex-details.md.
 *
 */

#ifndef V34_K56_H
#define V34_K56_H


/* V.8bis messages (7/V.8bis): the message type in octet 0's low 4 bits */
enum { K8_MSG_MS = 1, K8_MSG_CL = 2, K8_MSG_ACK1 = 4 };

/* the K56flex part of the pump */
template <class P>
struct V34K56 {
	s16 k56;                /* offer K56flex (V.8bis before V.8) */
	s16 k56_on;             /* K56flex agreed (our ACK(1) to its MS) */
	s16 k8_tried;           /* V.8bis done (or given up) this call */
	/* the V.8bis tones we send (MRe), and the remote's (CRd) */
	Osc k8_o1, k8_o2;
	float k8_a1, k8_a2;
	Goertzel k8_g[3];       /* 1529, 2225, 1900 Hz */
	s16 k8_dual;            /* CRd's dual tone blocks in a row */
	s16 k8_crd;             /* its dual tone heard */
	s32 k8_crd_end;         /* when it ended (count) */
	/* V.8bis messages in (the responder's, on V.21's channel 2):
	   HDLC frames */
	FskDemod k8_rx;
	u32 k8_sr;              /* the last 8 bits, newest in bit 7 */
	s16 k8_ones, k8_nbit, k8_in;
	unsigned char k8_buf[64];
	s16 k8_n;
	unsigned char k8_msg[64];       /* the last good frame (no FCS) */
	s16 k8_msg_n;
	s16 k8_ev;              /* a good frame came */

	/* phases 3 and 4, and the data */
	K56Train k56t;
	K56Enc k56e;
	unsigned char k56_codes[8];
	s16 k56_fpos;           /* the next code of k56_codes */
	s32 k56_frames;         /* data frames sent (the first B1's ones) */
	s16 k56_down;           /* the downstream rate (1: 32000 bit/s ..
				   13: 56000) */
	s16 k56_up;             /* the upstream's, N * 2400 */
	s16 k56_mp_in;          /* the caller's MP is in */
	s32 k56_t0;             /* when the state began (total) */

	void k8_init();
	s16 k56_start();
	s16 k56_step(s32 c);
	s16 k56_sample();
	void k56_make_mp();
	s16 k56_data_setup();
	void k8_frame(BitQ *q, const unsigned char *o, s16 n);
	float k8_tx_sample();
	void k8_rx_bit(s16 bit);
	void k8_rx_sample(float x);
	bool k8_ms_k56() const;
	s16 k8_step(s32 c);
};

/* our CL (the capabilities list): K56flex's non-standard block, as the
   SM56 client's own with 0x40 in its last octet (the digital modem) */
static const unsigned char k8_cl[] = {
	0x12, 0xc9, 0x80, 0x80, 0x80,
	0x07, 0xb5, 0x02, 0x00, 0x94, 0x81, 0x83, 0x61 };

/* for a new call */
template <class P>
void V34K56<P>::k8_init()
{
	P &self = static_cast<P &>(*this);
	k56_on = 0;
	k8_dual = k8_crd = 0;
	k8_crd_end = 0;
	k8_g[0].init(1529, 96, self.srate);
	k8_g[1].init(2225, 96, self.srate);
	k8_g[2].init(1900, 96, self.srate);
	k8_rx.init(1650, 1850, 300, self.srate);
	k8_sr = 0;
	k8_ones = k8_nbit = k8_in = k8_n = 0;
	k8_msg_n = 0;
	k8_ev = 0;
}

/* an HDLC frame of n octets into q (7.1/V.8bis): ones, flags, the
   octets least significant bit first with zeros stuffed after five
   ones, the FCS, flags */
template <class P>
void V34K56<P>::k8_frame(BitQ *q, const unsigned char *o, s16 n)
{
	unsigned char f[66];
	s16 i, k, ones = 0;
	memcpy(f, o, n);
	u16 crc = static_cast<u16>(hdlc_fcs(f, n) ^ 0xffff);
	f[n] = crc & 0xff;
	f[n + 1] = crc >> 8;
	q->reset();
	q->put(0xFFFFFFFF, 32);
	for (i = 0; i < 3; i++)
		q->put(0x7e, 8);
	for (i = 0; i < n + 2; i++)
		for (k = 0; k < 8; k++) {
			s16 b = (f[i] >> k) & 1;
			q->put(b, 1);
			ones = b ? ones + 1 : 0;
			if (ones == 5) {
				q->put(0, 1);
				ones = 0;
			}
		}
	q->put(0x7e, 8);
	q->put(0x7e, 8);
}

/* MRe's tones (and silence) */
template <class P>
float V34K56<P>::k8_tx_sample()
{
	return k8_a1 * k8_o1.next() + k8_a2 * k8_o2.next();
}

/* a bit of V.21's channel 2: HDLC flags, stuffing and frames */
template <class P>
void V34K56<P>::k8_rx_bit(s16 bit)
{
	k8_sr = (k8_sr >> 1) | static_cast<u32>(bit << 7);
	if ((k8_sr & 0xff) == 0x7e) {
		/* a flag: the frame before it (its 7 bits of the flag
		   gone) */
		if (k8_in && k8_n >= 3 && k8_nbit == 7 &&
		    hdlc_fcs(k8_buf, k8_n) == HDLC_FCS_GOOD) {
			k8_msg_n = static_cast<s16>(k8_n - 2);
			memcpy(k8_msg, k8_buf, k8_msg_n);
			k8_ev = 1;
		}
		k8_in = 1;
		k8_n = k8_nbit = k8_ones = 0;
		return;
	}
	if (!k8_in)
		return;
	if (bit) {
		if (++k8_ones > 6) {    /* an abort, or ones */
			k8_in = 0;
			return;
		}
	} else {
		if (k8_ones == 5) {     /* a stuffed zero */
			k8_ones = 0;
			return;
		}
		k8_ones = 0;
	}
	if (k8_n >= static_cast<s16>(sizeof(k8_buf))) {
		k8_in = 0;
		return;
	}
	if (!k8_nbit)
		k8_buf[k8_n] = 0;
	k8_buf[k8_n] = static_cast<unsigned char>(k8_buf[k8_n] | bit << k8_nbit);
	if (++k8_nbit == 8) {
		k8_nbit = 0;
		k8_n++;
	}
}

/* a line sample in phase 1's V.8bis */
template <class P>
void V34K56<P>::k8_rx_sample(float x)
{
	P &self = static_cast<P &>(*this);
	s16 bit = k8_rx.run(x);
	if (bit >= 0 && k8_rx.carrier)
		k8_rx_bit(bit);
	k8_g[2].run(x);
	k8_g[1].run(x);
	if (k8_g[0].run(x)) {
		float t = tone_power(300);
		bool dual = k8_g[0].power > t && k8_g[1].power > t;
		k8_dual = dual ? static_cast<s16>(k8_dual + 1) : 0;
		if (k8_dual >= 15)
			k8_crd = 1;
		if (k8_crd == 1 && !dual) {
			k8_crd = 2;
			k8_crd_end = self.count;
		}
	}
}

/* the responder's MS selects K56flex: its non-standard block (L > 6,
   T.35 code 0xb5 0x02, bit 0 of its 6th octet) */
template <class P>
bool V34K56<P>::k8_ms_k56() const
{
	const unsigned char *o = k8_msg;
	s16 n = k8_msg_n, i = 1, k;
	if (n < 2 || (o[0] & 0x0f) != K8_MSG_MS || !(o[1] & 0x40))
		return false;
	/* the identification field's NPar(1) and SPar(1), the standard
	   field's: each up to the octet with bit 7 set */
	for (k = 0; k < 4; k++) {
		while (i < n && !(o[i] & 0x80))
			i++;
		i++;
	}
	while (i < n) {
		s16 L = o[i];
		if (i + L >= n + 1)
			return false;
		if (L > 6 && o[i + 1] == 0xb5 && o[i + 2] == 0x02 &&
		    (o[i + 6] & 1))
			return true;
		i = static_cast<s16>(i + L + 1);
	}
	return false;
}

/* phase 1 before V.8: MRe, the responder's CRd, our CL, its MS, our
   ACK(1); then V.8's ANSam */
template <class P>
s16 V34K56<P>::k8_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	switch (self.state) {
	case K8_MRE:
		/* MRe (8.2/V.8bis): 1375 + 2002 Hz 400 ms, 650 Hz 100 ms */
		if (c == ms2samples(400, self.srate)) {
			k8_o1.set_freq(650, self.srate);
			k8_a1 = V34_TONE_AMP;
			k8_a2 = 0;
		}
		if (c >= ms2samples(500, self.srate)) {
			k8_a1 = 0;
			self.set_state(K8_CRD);
			self.txm = TXM_SILENCE;
		}
		break;
	case K8_CRD:
		/* its CRd (1529 + 2225 Hz, then 1900 Hz 100 ms); our CL 120
		   ms after it */
		if (k8_crd == 2 &&
		    self.count - k8_crd_end >= ms2samples(220, self.srate)) {
			DSP_DBG("v34: V.8bis CRd; CL\n");
			k8_frame(&self.v8tx, k8_cl,
				 static_cast<s16>(sizeof(k8_cl)));
			self.v8tx_repeat = 0;
			self.fsk.init(980, 1180, V34_TONE_AMP, 300,
				      self.srate, 1);
			self.txm = TXM_FSK;
			k8_ev = 0;
			self.set_state(K8_CL);
		} else if (c >= ms2samples(1500, self.srate) && !k8_crd) {
			DSP_DBG("v34: V.8bis: no CRd, V.8\n");
			self.txm = TXM_SILENCE;
			self.set_state(V8A_SILENCE);
		}
		break;
	case K8_CL:
		if (self.v8tx.pos >= self.v8tx.n && self.fsk.edge())
			self.txm = TXM_SILENCE;
		if (k8_ev) {
			k8_ev = 0;
			char buf[200];
			s16 i, n = 0;
			for (i = 0; i < k8_msg_n && n < 190; i++)
				n += snprintf(buf + n, sizeof(buf) - n, " %02x",
					      k8_msg[i]);
			DSP_DBG("v34: V.8bis message:%s\n", buf);
			if ((k8_msg[0] & 0x0f) != K8_MSG_MS)
				break;
			k56_on = k8_ms_k56();
			DSP_DBG("v34: V.8bis MS: %s\n", k56_on ?
				"K56flex" : "no K56flex");
			self.txm = TXM_SILENCE;
			self.set_state(K8_ACK_GAP);
		} else if (c >= ms2samples(2500, self.srate)) {
			DSP_DBG("v34: V.8bis: no MS, V.8\n");
			self.txm = TXM_SILENCE;
			self.set_state(V8A_SILENCE);
		}
		break;
	case K8_ACK_GAP:
		if (c >= ms2samples(180, self.srate)) {
			static const unsigned char ack[] = { 0x14 };
			k8_frame(&self.v8tx, ack, 1);
			self.v8tx_repeat = 0;
			self.fsk.init(980, 1180, V34_TONE_AMP, 300,
				      self.srate, 1);
			self.txm = TXM_FSK;
			self.set_state(K8_ACK);
		}
		break;
	case K8_ACK:
		if (self.v8tx.pos >= self.v8tx.n && self.fsk.edge()) {
			self.txm = TXM_SILENCE;
			if (k56_on) {
				/* V.8 for V.34 alone: K56flex goes on from
				   phase 2 */
				self.v90 = self.v91 = self.v92 = 0;
			}
			self.set_state(V8A_SILENCE);
		}
		break;
	default:
		break;
	}
	return 0;
}

/*
 *    phases 3 and 4
 */

/* after our INFO1a: the caller's V.34 transmitter (at the symbol rate
   our INFO1a asked for, on its carrier), our PCM training */
template <class P>
s16 V34K56<P>::k56_start()
{
	P &self = static_cast<P &>(*this);
	if (self.prx.init(self.baud_rx, self.high_rx, 0))
		return -1;
	self.prx_on = 1;
	/* its J: J's 16 bits and an octet */
	self.prx.j24 = 1;
	self.txm = TXM_SILENCE;
	k56t.init(self.alaw);
	k56t.js = static_cast<u16>(env_int("DSP_K56_JS", 0x89b0));
	/* its phase 4 of 16 points when our Js asks (bit 5) */
	self.prx.trn16 = (k56t.js >> 5) & 1;
	k56_fpos = 0;
	k56_frames = 0;
	k56_down = k56_up = 0;
	k56_mp_in = 0;
	k56_t0 = self.total;
	/* PCM once INFO1a is out of the converters (their delay) */
	self.pcm_tx = 0;
	self.set_state(K3_TRAIN);
	DSP_DBG("v34: K56flex phase 3, the caller at %.0f baud\n",
		v34_symbol_rate(self.baud_rx));
	return 0;
}

/* our MP and MP' (as V.34's, type 0): the upstream rate (N), the
   downstream's (1 to 13 in the answerer to caller field), 11 octets
   each, least significant bit first */
template <class P>
void V34K56<P>::k56_make_mp()
{
	V34Mp mp;
	unsigned char b[88];
	zero(mp);
	mp.rate_ca = k56_up;
	mp.rate_ac = k56_down;
	mp.rate_mask = 0x3FFF;
	mp.asym = 1;
	for (s16 a = 0; a < 2; a++) {
		unsigned char *o = a ? k56t.mpp : k56t.mp;
		mp.ack = a;
		mp.build(b);
		memset(o, 0, 11);
		for (s16 i = 0; i < 88; i++)
			o[i / 8] = static_cast<unsigned char>(
				o[i / 8] | (b[i] & 1) << (i % 8));
	}
}

/* the caller's MP: the rates both ways, the upstream receiver's data
   mode */
template <class P>
s16 V34K56<P>::k56_data_setup()
{
	P &self = static_cast<P &>(*this);
	const V34Mp *r = &self.prx.mp;
	V34Params p;
	s16 n, max = self.max_rate_rx;
	s16 mask = r->rate_mask & 0x3FFF;
	/* (test: DSP_V34_RX_MAX=<n> as for V.34) */
	if (s32 m = env_int("DSP_V34_RX_MAX"); m > 0 && max > m)
		max = static_cast<s16>(m);
	if (r->rate_ca < max)
		max = r->rate_ca;
	for (n = max; n >= 1; n--)
		if ((mask >> (n - 1)) & 1 &&
		    v34_rates[self.prx.baud][n - 1].b)
			break;
	if (n < 1 || p.init(self.prx.baud, n, 0))
		return -1;
	p.states = 16;
	self.prx.start_data(&p, CoreBits{ self.modem });
	k56_up = n;
	self.rate_rx = p.rate;
	/* downstream: what the caller asks for, at most DSP_K56_MAX */
	s32 dmax = env_int("DSP_K56_MAX", 13);
	k56_down = r->rate_ac;
	if (k56_down > dmax)
		k56_down = static_cast<s16>(dmax);
	if (k56_down < 1 || k56_down > 13)
		return -1;
	if (k56e.init({ 30000 + 2000 * k56_down, 0, self.alaw, 0 }))
		return -1;
	self.rate_tx = 30000 + 2000 * k56_down;
	k56_make_mp();
	DSP_DBG("v34: K56flex rates: down %" PRId32 ", up %" PRId32
		" (the caller's MP: %d, %d)\n", self.rate_tx, self.rate_rx,
		r->rate_ac, r->rate_ca);
	return 0;
}

/* the next sample down: the training's code, then the data frames
   (B1: 6 frames of ones first) */
template <class P>
s16 V34K56<P>::k56_sample()
{
	P &self = static_cast<P &>(*this);
	unsigned char code;
	if (k56t.seg != K56_DATA) {
		s16 s0 = k56t.seg;
		code = k56t.next();
		if (k56t.seg != s0)
			DSP_DBG("v34: K56flex: segment %d\n", k56t.seg);
	}
	else {
		if (!k56_fpos) {
			unsigned char bits[K56_MAXBITS];
			s16 nb = k56e.frame_bits();
			if (k56_frames < 6)
				memset(bits, 1, sizeof bits);
			else
				CoreBits{ self.modem }.get(bits, nb);
			k56e.frame(bits, k56_codes);
			k56_frames++;
		}
		code = k56_codes[k56_fpos];
		k56_fpos = (k56_fpos + 1) & 7;
	}
	s16 v = v90_ucode_linear(self.alaw, code & 0x7f);
	return code & 0x80 ? v : static_cast<s16>(-v);
}

template <class P>
s16 V34K56<P>::k56_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	switch (self.state) {
	case K3_TRAIN:
		if (!self.pcm_tx && c >= ms2samples(40, self.srate))
			self.pcm_tx = 1;
		/* the silence after Js until the caller's J (its S, S-bar,
		   PP and TRN came after our Js) */
		if (k56t.seg == K56_SIL2 && k56t.len >= 0) {
			k56t.len = -1;
			k56_t0 = self.total;
		}
		if (k56t.seg == K56_SIL2 && (self.prx.ev_j ||
		    self.total - k56_t0 >= ms2samples(3000, self.srate))) {
			if (self.prx.ev_j)
				DSP_DBG("v34: K56flex: the caller's J (%s, "
					"robbed bits %#04x); R\n",
					self.prx.j16 ? "16 point" : "4 point",
					self.prx.j_rbs);
			else
				DSP_DBG("v34: K56flex: no J from the caller; "
					"R\n");
			/* R on the symbols' grid: the silence 12288 at
			   least, in eights (the caller's receiver keeps
			   its timing and phase through it) */
			k56t.len = (k56t.n + 7) & ~7;
			if (k56t.len < k56_seg_len[K56_SIL2])
				k56t.len = k56_seg_len[K56_SIL2];
			self.set_state(K3_TRN2);
		}
		break;
	case K3_TRN2:
		/* TRN2B until the caller's MP */
		if (k56t.seg == K56_TRN2B && k56t.len >= 0) {
			k56t.len = -1;
			k56_t0 = self.total;
		}
		if (self.prx.ev_mp && !k56_mp_in) {
			k56_mp_in = 1;
			if (k56_data_setup()) {
				DSP_DBG("v34: K56flex: no common rate\n");
				return self.fallback("no common data rate");
			}
		}
		if (k56t.seg == K56_TRN2B && k56_mp_in &&
		    k56t.n >= 2048) {
			k56t.segment(K56_MP);
			self.set_state(K4_MP);
		} else if (k56t.seg == K56_TRN2B &&
			   self.total - k56_t0 >= ms2samples(8000, self.srate)) {
			DSP_DBG("v34: K56flex: no MP from the caller\n");
			return self.fallback("no MP");
		}
		break;
	case K4_MP:
		/* MP, MP', E and B1 down; the caller's MP', E */
		if (k56t.seg == K56_DATA && self.prx.ev_e) {
			self.set_state(K4_DATA);
			self.set_rates(self.rate_tx, self.rate_rx);
			self.connected = 1;
			DSP_DBG("v34: K56flex connect " RATE_FMT "\n",
				self.rate_tx, self.rate_rx);
			self.stat_dp(DP_K56);
			return DPSTAT_CONNECT;
		}
		break;
	default:
		break;
	}
	return 0;
}

#endif
