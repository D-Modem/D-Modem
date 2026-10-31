/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v34_k56.h - K56flex in the V.34 pump (dp_v34.cpp).  The digital
 *                modem (the answerer): phase 1 V.8bis (its MRe, CRd,
 *                CL, MS and ACK(1)) before V.8; phase 2 V.34's, our
 *                INFO1a marked as K56flex's.  Phases 3 and 4: our PCM
 *                training (K56Train) down, the caller's V.34 signals up
 *                (S, S-bar, PP, TRN, J; J', TRN, MP, MP', E), then the
 *                data: K56Enc's frames down, V.34 up.  The analogue
 *                modem (the caller), the other way round: CRd and MS
 *                to the digital modem's MRe and CL, INFO1c marked, the
 *                PCM receiver (K56Rx) on its training and data.
 *                Included by dp_v34.cpp only, after its states and
 *                BitQ.  See docs/dsp/k56flex-details.md.
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

	/* the analogue modem (calling): K56flex offered (+MS=K56 on an
	   analogue line at 9600 Hz; DSP_K56A=0 leaves it out), agreed
	   (the digital modem's ACK(1) to our MS), the line into k56rx */
	s16 k56c;
	s16 k56c_on;
	s16 k56_rx;
	K56Rx k56rx;
	s32 k56_quiet;          /* data mode samples without the signal */
	s32 k8_ans;             /* ANSam's samples after V.8bis */

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
	bool k8_msg_k56(s16 type) const;
	s16 k8_step(s32 c);
	s16 k56c_start();
	s16 k56c_step(s32 c);
	s16 k56c_data_setup();
};

/* our CL (the capabilities list): K56flex's non-standard block, as the
   SM56 client's own with 0x40 in its last octet (the digital modem) */
static const unsigned char k8_cl[] = {
	0x12, 0xc9, 0x80, 0x80, 0x80,
	0x07, 0xb5, 0x02, 0x00, 0x94, 0x81, 0x83, 0x61 };
/* the analogue modem's MS: the SM56 client's, the block without 0x40 */
static const unsigned char k8_ms[] = {
	0x11, 0xc9, 0x80, 0x80, 0x80,
	0x07, 0xb5, 0x02, 0x00, 0x94, 0x81, 0x83, 0x21 };

/* for a new call */
template <class P>
void V34K56<P>::k8_init()
{
	P &self = static_cast<P &>(*this);
	k56_on = 0;
	k8_dual = k8_crd = 0;
	k8_crd_end = 0;
	if (self.caller) {
		/* MRe's tones; the CL and ACK on V.21's channel 1 */
		k8_g[0].init(1375, 96, self.srate);
		k8_g[1].init(2002, 96, self.srate);
		k8_g[2].init(650, 96, self.srate);
		k8_rx.init(980, 1180, 300, self.srate);
	} else {
		k8_g[0].init(1529, 96, self.srate);
		k8_g[1].init(2225, 96, self.srate);
		k8_g[2].init(1900, 96, self.srate);
		k8_rx.init(1650, 1850, 300, self.srate);
	}
	k56c_on = k56_rx = 0;
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
	if (!k8_g[0].run(x))
		return;
	float t = tone_power(300);
	if (self.caller) {
		/* MRe: its dual tone, then 650 Hz; ended when neither */
		bool on = (k8_g[0].power > t && k8_g[1].power > t) ||
			k8_g[2].power > t;
		k8_dual = on ? static_cast<s16>(k8_dual + 1) : 0;
		if (k8_dual >= 3 && !k8_crd)
			k8_crd = 1;
		if (k8_crd == 1 && !on) {
			k8_crd = 2;
			k8_crd_end = self.count;
		}
		return;
	}
	bool dual = k8_g[0].power > t && k8_g[1].power > t;
	k8_dual = dual ? static_cast<s16>(k8_dual + 1) : 0;
	if (k8_dual >= 15)
		k8_crd = 1;
	if (k8_crd == 1 && !dual) {
		k8_crd = 2;
		k8_crd_end = self.count;
	}
}

/* the last message, of type (K8_MSG_*), offers K56flex: its
   non-standard block (L > 6, T.35 code 0xb5 0x02, bit 0 of its 6th
   octet) */
template <class P>
bool V34K56<P>::k8_msg_k56(s16 type) const
{
	const unsigned char *o = k8_msg;
	s16 n = k8_msg_n, i = 1, k;
	if (n < 2 || (o[0] & 0x0f) != type || !(o[1] & 0x40))
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

/* the responder's MS selects K56flex */
template <class P>
bool V34K56<P>::k8_ms_k56() const
{
	return k8_msg_k56(K8_MSG_MS);
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

	/* the analogue modem, calling: the digital modem's MRe (heard by
	   the call progress), our CRd 150 ms after it, its CL, our MS,
	   its ACK(1); then V.8 from its ANSam */
	case KC8_MRE:
		if (k8_crd == 2 &&
		    self.count - k8_crd_end >= ms2samples(150, self.srate)) {
			DSP_DBG("v34: V.8bis MRe; CRd\n");
			k8_o1.set_freq(1529, self.srate);
			k8_o2.set_freq(2225, self.srate);
			k8_a1 = k8_a2 = V34_TONE_AMP / 2;
			self.txm = TXM_K8;
			self.set_state(KC8_CRD);
		} else if (c >= ms2samples(2000, self.srate)) {
			DSP_DBG("v34: V.8bis: MRe does not end, V.8\n");
			self.set_state(KC8_ANS);
		}
		break;
	case KC8_CRD:
		/* CRd (8.2/V.8bis): 1529 + 2225 Hz 400 ms, 1900 Hz 100 ms */
		if (c == ms2samples(400, self.srate)) {
			k8_o1.set_freq(1900, self.srate);
			k8_a1 = V34_TONE_AMP;
			k8_a2 = 0;
		}
		if (c >= ms2samples(500, self.srate)) {
			k8_a1 = 0;
			self.txm = TXM_SILENCE;
			k8_ev = 0;
			self.set_state(KC8_CL);
		}
		break;
	case KC8_CL:
		if (k8_ev) {
			k8_ev = 0;
			if ((k8_msg[0] & 0x0f) != K8_MSG_CL)
				break;
			if (!k8_msg_k56(K8_MSG_CL)) {
				DSP_DBG("v34: V.8bis CL: no K56flex, V.8\n");
				self.set_state(KC8_ANS);
				break;
			}
			DSP_DBG("v34: V.8bis CL: K56flex; MS\n");
			k8_frame(&self.v8tx, k8_ms,
				 static_cast<s16>(sizeof(k8_ms)));
			self.v8tx_repeat = 0;
			self.fsk.init(1650, 1850, V34_TONE_AMP, 300,
				      self.srate, 1);
			self.txm = TXM_FSK;
			self.set_state(KC8_MS);
		} else if (c >= ms2samples(3000, self.srate)) {
			DSP_DBG("v34: V.8bis: no CL, V.8\n");
			self.set_state(KC8_ANS);
		}
		break;
	case KC8_MS:
		if (self.v8tx.pos >= self.v8tx.n && self.fsk.edge()) {
			self.txm = TXM_SILENCE;
			k8_ev = 0;
			self.set_state(KC8_ACK);
		}
		break;
	case KC8_ACK:
		if (k8_ev) {
			k8_ev = 0;
			if ((k8_msg[0] & 0x0f) != K8_MSG_ACK1)
				break;
			k56c_on = 1;
			/* V.8 for V.34 alone: K56flex goes on from phase 2 */
			self.v90a = self.v92a = 0;
			DSP_DBG("v34: V.8bis ACK(1): K56flex\n");
			self.set_state(KC8_ANS);
		} else if (c >= ms2samples(2000, self.srate)) {
			DSP_DBG("v34: V.8bis: no ACK(1), V.8\n");
			self.set_state(KC8_ANS);
		}
		break;
	case KC8_ANS:
		/* its ANSam there 200 ms: V.8 as the call modem */
		k8_ans = self.ans_peak > 300 && self.ans_gone == 0 ?
			k8_ans + 1 : 0;
		if (k8_ans >= ms2samples(200, self.srate)) {
			self.ans_peak = 0;
			self.env_sum = self.env15_re = self.env15_im = 0;
			self.env_n = 0;
			self.set_state(V8C_LISTEN);
		} else if (c >= ms2samples(5000, self.srate)) {
			DSP_DBG("v34: V.8bis: no ANSam after it\n");
			return DPSTAT_ERROR;
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

/* the server's K56flex phases 3 and 4 and data, c samples into the
   state: the next sample sent */
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

/*
 *    the analogue modem's phases 3 and 4
 */

/* after the digital modem's INFO1a marked K56flex: our V.34
   transmitter at the symbol rate it asked for (the caller's, GPC), the
   PCM receiver on the line; silent until its Js */
template <class P>
s16 V34K56<P>::k56c_start()
{
	P &self = static_cast<P &>(*this);
	self.ptx.init(self.baud_tx, self.high_tx, 1);
	/* our J: 0x89b0 (16 point), then a robbed-bit octet: none */
	self.ptx.j24 = 1;
	self.ptx.jword = 0x89b0;
	self.ptx.j_rbs = 0;
	self.txm = TXM_PRIMARY;
	k56rx.init(self.alaw, self.line_rate);
	k56_rx = 1;
	k56_quiet = 0;
	k56_t0 = self.total;
	/* (the PCM codes' DC is theirs, as V.90's) */
	if (self.rt)
		self.rt->pcm = 1;
	self.set_state(KC3_WAIT);
	DSP_DBG("v34: K56flex phase 3, the analogue modem at %.0f baud, "
		"%s\n", v34_symbol_rate(self.baud_tx),
		self.alaw ? "A-law" : "u-law");
	return 0;
}

/* the digital modem's MP: the rates both ways, our data encoder */
template <class P>
s16 V34K56<P>::k56c_data_setup()
{
	P &self = static_cast<P &>(*this);
	const V34Mp &m = k56rx.mp;
	V34Params p;
	s16 n = m.rate_ca;
	if (k56rx.bad || n < 1 || n > 14 ||
	    !v34_rates[self.ptx.baud][n - 1].b || p.init(self.ptx.baud, n, 0))
		return -1;
	p.states = 16;
	self.ptx.start_data(&p, CoreBits{ self.modem });
	self.rate_tx = p.rate;
	self.rate_rx = 30000 + 2000 * m.rate_ac;
	DSP_DBG("v34: K56flex rates: up %" PRId32 ", down %" PRId32 "\n",
		self.rate_tx, self.rate_rx);
	return 0;
}

/* the analogue modem's phases 3 and 4 and data, c samples into the
   state */
template <class P>
s16 V34K56<P>::k56c_step(s32 c)
{
	P &self = static_cast<P &>(*this);
	V34Tx &t = self.ptx;
	switch (self.state) {
	case KC3_WAIT:
		/* the digital modem's training until its Js, then our S */
		if (k56rx.ev_js) {
			DSP_DBG("v34: K56flex: Js %#06x; S\n", k56rx.js);
			self.tx_next(KC3_S, V34_TX_S);
		} else if (self.total - k56_t0 >= ms2samples(10000,
							      self.srate)) {
			DSP_DBG("v34: K56flex: no training from the digital "
				"modem\n");
			return self.fallback("no K56flex training");
		}
		break;
	case KC3_S:
		if (t.n >= 128)
			self.tx_next(KC3_SBAR, V34_TX_SBAR);
		break;
	case KC3_SBAR:
		if (t.n >= 16)
			self.tx_next(KC3_PP, V34_TX_PP);
		break;
	case KC3_PP:
		if (t.n >= 288)
			self.tx_next(KC3_TRN, V34_TX_TRN);
		break;
	case KC3_TRN:
		if (t.n >= 2048) {
			self.tx_next(KC3_J, V34_TX_J);
			k56_t0 = self.total;
		}
		break;
	case KC3_J:
		/* J until its TRN2A, then J' after a whole J; our phase 4
		   of 16 points if its Js asked */
		if (k56rx.ev_trn2a && t.n % 12 == 0) {
			t.trn16 = (k56rx.js >> 5) & 1;
			DSP_DBG("v34: K56flex: TRN2A; J'\n");
			self.tx_next(KC4_TRN, V34_TX_JP);
		} else if (self.total - k56_t0 >= ms2samples(8000,
							      self.srate)) {
			DSP_DBG("v34: K56flex: no R after our J\n");
			return self.fallback("no K56flex phase 4");
		}
		break;
	case KC4_TRN:
		if (t.seg == V34_TX_TRN && t.n >= 2048) {
			/* our MP: the most we send, the most we take down
			   (DSP_K56_MAX, 13: 56000 bit/s) */
			V34Mp *mp = &t.mp;
			zero(*mp);
			mp->rate_ca = self.rate_cap(MDMPRM_MAX_RATE);
			mp->rate_ac = static_cast<s16>(env_int("DSP_K56_MAX",
							       13));
			mp->rate_mask = 0x3FFF;
			mp->asym = 1;
			self.tx_next(KC4_MP, V34_TX_MP);
			k56_t0 = self.total;
		}
		break;
	case KC4_MP:
		if (k56rx.ev_mp && !t.ack_mp) {
			if (k56c_data_setup()) {
				DSP_DBG("v34: K56flex: no common rate\n");
				return self.fallback("no common data rate");
			}
			t.ack_mp = 1;
		}
		/* E after a whole MP' once its MP' has come */
		if (k56rx.ev_mpack && t.mp_sent >= 1 && t.mp.ack &&
		    t.pos >= t.nbits)
			self.tx_next(KC4_E, V34_TX_E);
		else if (self.total - k56_t0 >= ms2samples(8000, self.srate)) {
			DSP_DBG("v34: K56flex: no MP from the digital "
				"modem\n");
			return self.fallback("no MP");
		}
		break;
	case KC4_E:
		if (t.seg == V34_TX_DATA && k56rx.ev_data) {
			self.set_state(KC4_DATA);
			self.set_rates(self.rate_tx, self.rate_rx);
			self.connected = 1;
			DSP_DBG("v34: K56flex connect " RATE_FMT
				" (analogue modem)\n", self.rate_tx,
				self.rate_rx);
			self.stat_dp(DP_K56);
			return DPSTAT_CONNECT;
		}
		break;
	case KC4_DATA:
		/* the PCM signal gone: carrier lost */
		k56_quiet = k56rx.dpow < 1e-3f * k56rx.level * k56rx.level ?
			k56_quiet + 1 : 0;
		self.stat_carrier(!k56_quiet);
		if (k56_quiet >= self.carrier_loss) {
			DSP_DBG("v34: K56flex: carrier lost\n");
			return DPSTAT_ERROR;
		}
		if (c % ms2samples(1000, self.srate) == 0)
			DSP_DBG("v34: K56flex data: error %.2f rms (%.1f dB), "
				"%" PRId32 " code errors, timing %+.2f ppm\n",
				sqrtf(k56rx.de_mse),
				10 * log10f(k56rx.dpow /
					    (k56rx.de_mse + 1e-6f)),
				k56rx.dec.errors, k56rx.ppm());
		break;
	default:
		break;
	}
	return 0;
}

#endif
