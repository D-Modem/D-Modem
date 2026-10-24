/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_msg.cpp - V.cvm's capabilities (Annex A), its INFO and MP
 *                   messages (Annex B) and the message mode they go in
 *                   (9.2.3): P20, or V.21's FSK, an HDLC frame sent twice
 *
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp.h"
#include "fax_hdlc.h"
#include "vcvm_startup.h"

namespace dsp {

/*
 *    Annex A
 */

/* octet 2 of the field: the modes' bits, its two top bits set */
uint8_t VcvmCaps::modes() const
{
	return static_cast<uint8_t>(0xC0 | (wide ? 0x20 : 0) |
				    (wide400 ? 0x10 : 0) | (dpsk8 ? 0x08 : 0) |
				    (sym22 ? 0x04 : 0) | (quick ? 0x02 : 0) |
				    (param ? 0x01 : 0));
}

/* the field's two octets to o: the version, 16000 Hz, fax and rings,
   then the modes */
void VcvmCaps::encode(uint8_t o[2]) const
{
	o[0] = static_cast<uint8_t>(((version & 7) << 5) | (fs16 ? 0x10 : 0) |
			 (fax ? 0x08 : 0) | (rings ? 0x04 : 0));
	o[1] = modes();
}

/* see vcvm_startup.h */
VcvmCaps VcvmCaps::decode(const uint8_t o[2])
{
	VcvmCaps c;
	c.version = 1;
	c.fs16 = o[0] & 0x10;
	c.fax = o[0] & 0x08;
	c.rings = o[0] & 0x04;
	c.wide = o[1] & 0x20;
	c.wide400 = o[1] & 0x10;
	c.dpsk8 = o[1] & 0x08;
	c.sym22 = o[1] & 0x04;
	c.quick = o[1] & 0x02;
	c.param = o[1] & 0x01;
	return c;
}

/* see vcvm_startup.h: 6 octets */
int vcvm_nsf_build(const VcvmCaps &c, uint8_t *out)
{
	out[0] = VCVM_T35_COUNTRY;
	out[1] = VCVM_T35_PROVIDER0;
	out[2] = VCVM_T35_PROVIDER1;
	out[3] = VCVM_NSF_ID;
	c.encode(out + 4);
	return 6;
}

/* the identifier and a field that reads as one: a version, the
   reserved bits clear, the robust and narrowband modes there */
static bool nsf_at(const uint8_t *p, int n, int i)
{
	return i + 2 < n && p[i] == VCVM_NSF_ID && (p[i + 1] >> 5) &&
	       !(p[i + 1] & 3) && (p[i + 2] & 0xC0) == 0xC0;
}

/* see vcvm_startup.h */
bool vcvm_nsf_find(const uint8_t *p, int n, VcvmCaps *c)
{
	/* after a country code and a two octet provider code, or
	   anywhere after the country's */
	int at = nsf_at(p, n, 3) ? 3 : -1;
	for (int i = 1; at < 0 && i + 2 < n; i++)
		if (nsf_at(p, n, i))
			at = i;
	if (at < 0)
		return false;
	*c = VcvmCaps::decode(p + at + 1);
	return true;
}

/* see vcvm_startup.h: the category octet and 13 extension octets */
int vcvm_v8_ns(const VcvmCaps &c, uint8_t *out)
{
	/* the NS field: K + L + M + 1, the country (K = 1), L = 2, the
	   provider, the identifier and the field (M = 3) */
	uint8_t f[8], nsf[6];
	vcvm_nsf_build(c, nsf);
	f[0] = 7;
	f[1] = nsf[0];
	f[2] = 2;
	f[3] = nsf[1];
	f[4] = nsf[2];
	f[5] = nsf[3];
	f[6] = nsf[4];
	f[7] = nsf[5];
	int n = 0;
	out[n++] = 0x0F;
	for (int b = 0; b < 8 * 8; b += 5) {
		unsigned v = 0;
		for (int i = 0; i < 5 && b + i < 64; i++)
			v |= static_cast<unsigned>(
				     (f[(b + i) / 8] >> ((b + i) % 8)) & 1)
			     << i;
		out[n++] = static_cast<uint8_t>(0x10 | (v & 7) |
						((v >> 3) & 3) << 6);
	}
	return n;
}

/* see vcvm_startup.h */
bool vcvm_v8_find(const uint8_t *msg, int n, VcvmCaps *c)
{
	for (int i = 0; i < n; i++) {
		if ((msg[i] & 0x1F) != 0x0F)
			continue;
		/* the extension octets' five bits each, into octets */
		uint8_t f[64] = {};
		int nb = 0;
		for (int j = i + 1; j < n && (msg[j] & 0x38) == 0x10 &&
				    nb + 5 <= 8 * static_cast<int>(sizeof(f));
		     j++) {
			unsigned v = (msg[j] & 7) | ((msg[j] >> 6) & 3) << 3;
			for (int k = 0; k < 5; k++, nb++)
				f[nb / 8] |= static_cast<uint8_t>(((v >> k) & 1)
								  << (nb % 8));
		}
		/* the NS blocks: length, country (two octets after 0xFF),
		   provider code's length, provider code, information */
		int nf = nb / 8;
		for (int p = 0; p < nf && f[p]; p += f[p] + 1) {
			int len = f[p], e = p + 1 + len;
			if (e > nf)
				break;
			int q = p + 1 + (f[p + 1] == 0xFF ? 2 : 1);
			if (q >= e)
				continue;
			q += 1 + f[q];
			if (q + 3 <= e && f[q] == VCVM_NSF_ID) {
				const uint8_t inf[3] = { f[q], f[q + 1], f[q + 2] };
				/* nsf_at's checks, on the field alone */
				if ((inf[1] >> 5) && !(inf[1] & 3) &&
				    (inf[2] & 0xC0) == 0xC0) {
					*c = VcvmCaps::decode(inf + 1);
					return true;
				}
			}
		}
	}
	return false;
}

/* see vcvm_startup.h */
VcvmCaps vcvm_caps_for(int modes, int fs)
{
	VcvmCaps c;
	c.fs16 = fs == 16000;
	c.wide = c.wide400 = c.fs16 && (modes & VCVM_M_WIDE);
	c.dpsk8 = modes & VCVM_M_8DPSK;
	c.rings = modes & VCVM_M_RINGS;
	c.param = modes & VCVM_M_PARAM;
	return c;
}

/* see vcvm_startup.h */
int vcvm_allowed(const VcvmCaps &own, const VcvmCaps &peer, int modes)
{
	int a = modes & (VCVM_M_ROBUST | VCVM_M_NARROW);
	bool w = (modes & VCVM_M_WIDE) && own.fs16 && peer.fs16;
	if (w && own.wide && peer.wide)
		a |= VCVM_M_WIDE;
	if (w && own.wide400 && peer.wide400)
		a |= VCVM_M_WIDE400;
	if (own.dpsk8 && peer.dpsk8)
		a |= modes & VCVM_M_8DPSK;
	if (own.rings && peer.rings)
		a |= modes & VCVM_M_RINGS;
	if (own.param && peer.param)
		a |= modes & VCVM_M_PARAM;
	if (own.sym22 && peer.sym22)
		a |= VCVM_M_SYM22;
	return a;
}

/*
 *    Annex B: the messages
 */

/* INFO0's 4 octets to o: the modes, the combs probed, the quick start
   and rings bits, the level; 4 */
int VcvmInfo0::encode(uint8_t *o) const
{
	o[0] = modes;
	o[1] = 0;
	for (int c = 0; c < 8; c++)
		if (combs & (1u << c))
			o[1] |= static_cast<uint8_t>(0x80 >> c);
	o[2] = static_cast<uint8_t>((combs & 0x100 ? 0x80 : 0) |
			 (quick_offer ? 0x40 : 0) | (quick_accept ? 0x20 : 0) |
			 (rings ? 0x10 : 0));
	o[3] = static_cast<uint8_t>(std::min(std::max(level, 0), 255));
	return 4;
}

/* INFO0 from n octets of o: 0, or -1 if too short or not one */
int VcvmInfo0::decode(const uint8_t *o, int n)
{
	if (n < 4 || (o[0] & 0xC0) != 0xC0)
		return -1;
	modes = o[0];
	combs = 0;
	for (int c = 0; c < 8; c++)
		if (o[1] & (0x80 >> c))
			combs |= 1u << c;
	if (o[2] & 0x80)
		combs |= 0x100;
	quick_offer = o[2] & 0x40;
	quick_accept = o[2] & 0x20;
	rings = o[2] & 0x10;
	level = o[3];
	return 0;
}

/* the symbol period in ms */
double VcvmLoading::sym_ms() const
{
	if (kind == PARAM)
		return p10 ? 10 : 20;
	return sym22 ? 22.5 : 20;
}

/* the bits a symbol carries (4 parametric, 0 none) */
int VcvmLoading::bits() const
{
	return kind == COMB ? load.bits(tones) : kind == PARAM ? 4 : 0;
}

/* the line rate before the code, bit/s */
int VcvmLoading::raw_bps() const
{
	return static_cast<int>(lrint(bits() * 1000.0 / sym_ms()));
}

/* the raw rate times k / 32, bit/s */
int VcvmLoading::coded_bps() const
{
	return raw_bps() * k() / 32;
}

/* the tones loaded (0 parametric) */
int VcvmLoading::loaded() const
{
	return kind == COMB ? load.loaded(tones) : 0;
}

/* the loading as the logs show it: the comb or symbol, the tones, the
   rate before the code and the code rate */
std::string VcvmLoading::str() const
{
	static const char *names[9] = { "N200", "N300", "N400", "W200", "W300",
					"W400", "S200", "S300", "S400" };
	static const char *rates[3] = { "1/2", "3/4", "7/8" };
	char b[160];
	if (kind == NONE)
		return "no data mode";
	if (kind == PARAM)
		snprintf(b, sizeof(b), "%s, %d bit/s, rate %s",
			 p10 ? "P10" : "P20", raw_bps(), rates[rate]);
	else {
		int d8 = 0, r = 0;
		for (int k = 0; k < tones; k++) {
			d8 += load.phase[k] == 3;
			r += load.ring[k] != 0;
		}
		snprintf(b, sizeof(b), "%s%s, %d of %d tones (%d 8-DPSK, %d "
			 "with rings), %d bit/s, rate %s", names[code % 9],
			 sym22 ? " 22.5 ms" : "", loaded(), tones, d8, r,
			 raw_bps(), rates[rate]);
	}
	return b;
}

/* the 2 bit fields of n tones from octet o on, from bit 8 */
static void put_fields(uint8_t *o, const uint8_t *v, int n)
{
	for (int i = 0; i < n; i++)
		o[i / 4] |=
			static_cast<uint8_t>((v[i] & 3) << (6 - 2 * (i % 4)));
}

/* the 2 bit field of tone i from octet o on */
static int get_field(const uint8_t *o, int i)
{
	return (o[i / 4] >> (6 - 2 * (i % 4))) & 3;
}

/* INFO1 or MP to o: the octets' number, 5 and one or two octets of
   2 bit fields per 4 tones */
int VcvmMp::encode(uint8_t *o) const
{
	int per = l.kind == VcvmLoading::PARAM ? (l.p10 ? 2 : 0) :
		l.sym22 ? 1 : 0;
	int N = l.kind == VcvmLoading::COMB ? l.tones : 0;
	bool rg = false;
	uint8_t ph[VCVM_TONES], rs[VCVM_TONES];
	for (int k = 0; k < N; k++) {
		ph[k] = static_cast<uint8_t>(l.load.phase[k] == 2 ? 1 :
					     l.load.phase[k] == 3 ? 2 :
								    0);
		rs[k] = static_cast<uint8_t>(ph[k] ? l.load.ring[k] : 0);
		rg |= rs[k] != 0;
	}
	int nb = (N + 3) / 4;
	memset(o, 0, 5 + 2 * nb);
	o[0] = static_cast<uint8_t>((ack ? 0x80 : 0) | per << 5 |
				    (l.rate & 3) << 3);
	o[1] = retrain ? 0x80 : 0;
	o[2] = static_cast<uint8_t>(std::min(std::max(rtt, 0), 255));
	o[3] = static_cast<uint8_t>(
		(l.kind == VcvmLoading::COMB ? (l.code & 15) << 4 : 0) |
		(l.kind == VcvmLoading::PARAM ? 0x08 : 0) | (rg ? 0x04 : 0));
	o[4] = static_cast<uint8_t>(N);
	put_fields(o + 5, ph, N);
	if (rg)
		put_fields(o + 5 + nb, rs, N);
	return 5 + nb * (rg ? 2 : 1);
}

/* INFO1 or MP from n octets of o: 0, or -1 if malformed or the comb
   and its tones don't agree */
int VcvmMp::decode(const uint8_t *o, int n)
{
	if (n < 5)
		return -1;
	*this = VcvmMp();
	ack = o[0] & 0x80;
	retrain = o[1] & 0x80;
	int per = (o[0] >> 5) & 3, rate = (o[0] >> 3) & 3;
	if (rate == 3 || per == 3)
		return -1;
	l.rate = rate;
	rtt = o[2];
	int N = o[4];
	if (o[3] & 0x08) {
		if (per == 1)
			return -1;
		l.kind = VcvmLoading::PARAM;
		l.p10 = per == 2;
		return 0;
	}
	if (!N)
		return 0;               /* no data mode */
	if (per == 2)
		return -1;
	l.kind = VcvmLoading::COMB;
	l.code = o[3] >> 4;
	l.sym22 = per == 1;
	static const int band_fs[3] = { 8000, 16000, 32000 };
	VcvmComb c;
	if (l.code > 8 || c.init(l.code, band_fs[l.code / 3], l.sym22) ||
	    c.tones != N)
		return -1;
	l.tones = N;
	int nb = (N + 3) / 4;
	bool rg = o[3] & 0x04;
	if (n < 5 + nb * (rg ? 2 : 1))
		return -1;
	for (int k = 0; k < N; k++) {
		int f = get_field(o + 5, k);
		if (f == 3)
			return -1;
		l.load.phase[k] = static_cast<uint8_t>(f ? f + 1 : 0);
		if (rg) {
			int r = get_field(o + 5 + nb, k);
			if (r == 3)
				return -1;
			l.load.ring[k] = static_cast<uint8_t>(f ? r : 0);
		}
	}
	return 0;
}

/*
 *    the message mode
 */

namespace {

struct HdlcMsg {};

/* a message's bits: flags, its frame twice with a flag between, the
   closing flags */
struct MsgBits : HdlcTx<MsgBits, 64, HdlcMsg> {
	const uint8_t *m = nullptr;
	int n = 0, sent = 0;

	/* the frame (at most max octets) to buf, twice: its length, then -1 */
	int hdlc_tx_next(HdlcMsg, unsigned char *buf, int max)
	{
		if (sent == 2)
			return -1;
		int k = std::min(n, max);
		memcpy(buf, m, static_cast<size_t>(k));
		sent++;
		return k;
	}
	/* the message's bits to out, lead octets of flags before */
	void bits(int lead, std::vector<uint8_t> &out)
	{
		hdlc_tx_start(8L * lead, 16);
		for (int b; (b = hdlc_tx_bit()) != FAX_BIT_END;)
			out.push_back(static_cast<uint8_t>(b));
	}
};

/* V.21's channels: mark and space, Hz */
const float v21_ch[2][2] = { { 980, 1180 }, { 1650, 1850 } };

} // namespace

/* see vcvm_startup.h: 0, or -1 for a rate not 8000 or 16000 */
int VcvmMsgTx::init(int fs, bool param, bool calling)
{
	if (fs != 8000 && fs != 16000)
		return -1;
	fs_ = fs;
	param_ = param;
	calling_ = calling;
	ph_ = clk_ = 0;
	return param ? ptx_.init(fs, false) : 0;
}

/* see vcvm_startup.h: P20's symbols, or V.21's FSK at -12 dBFS,
   phase continuous */
void VcvmMsgTx::send(const uint8_t *m, int n, std::vector<float> &out)
{
	MsgBits mb;
	mb.m = m;
	mb.n = n;
	std::vector<uint8_t> b;
	mb.bits(param_ ? 2 : 4, b);
	if (param_) {
		/* to whole symbols with a flag's bits */
		for (int i = 0; b.size() % 4; i++)
			b.push_back(
				static_cast<uint8_t>((0x7e >> (i % 8)) & 1));
		size_t at = out.size();
		const int T = ptx_.sps();
		out.resize(at + 8 * T + b.size() / 4 * T);
		ptx_.preamble(&out[at]);
		at += 8 * T;
		for (size_t i = 0; i < b.size(); i += 4, at += T)
			ptx_.data(&b[i], &out[at]);
		return;
	}
	/* V.21: -12 dBFS, phase continuous */
	const float *ch = v21_ch[calling_ ? 0 : 1];
	const double A = 32767 * pow(10, -12 / 20.0) * sqrt(2.0);
	const double inc = 300.0 / fs_;
	for (uint8_t bit : b) {
		double w = 2 * M_PI * ch[bit ? 0 : 1] / fs_;
		while (clk_ < 1) {
			ph_ = fmod(ph_ + w, 2 * M_PI);
			out.push_back(static_cast<float>(A * cos(ph_)));
			clk_ += inc;
		}
		clk_ -= 1;
	}
}

struct VcvmMsgRx::Hdlc : HdlcRx<Hdlc, 64, HdlcMsg> {
	VcvmMsgRx *o = nullptr;
	/* a frame between flags, to the receiver if whole */
	void hdlc_rx_frame(HdlcMsg, const unsigned char *d, int len, bool whole)
	{
		if (whole)
			o->frame(d, len);
	}
	/* the deframer back to hunting for a flag */
	void reset() { hdlc_rx_reset(); }
};

/* the two filters, mark and space Hz at fs, over 80 per cent of a bit;
   the bit clock at 0 */
void VcvmFskRx::init(float mark, float space, int fs)
{
	fs_ = fs;
	wm_ = 2 * M_PI * mark / fs;
	ws_ = 2 * M_PI * space / fs;
	pm_ = ps_ = 0;
	w_ = static_cast<int>(lrint(0.8 * fs / 300));
	buf_.assign(static_cast<size_t>(w_) * 4, 0);
	pos_ = 0;
	for (double &v : sum_)
		v = 0;
	inc_ = 300.0 / fs;
	phase_ = 0;
	last_ = 0;
	n_ = 0;
}

/* see vcvm_startup.h: the bit 1 if the mark's magnitude is the greater */
int VcvmFskRx::run(float x)
{
	/* the window's sums of x against each tone */
	double v[4] = { x * cos(pm_), -x * sin(pm_), x * cos(ps_), -x * sin(ps_) };
	pm_ = fmod(pm_ + wm_, 2 * M_PI);
	ps_ = fmod(ps_ + ws_, 2 * M_PI);
	for (int i = 0; i < 4; i++) {
		sum_[i] += v[i] - buf_[pos_ * 4 + static_cast<size_t>(i)];
		buf_[pos_ * 4 + static_cast<size_t>(i)] = v[i];
	}
	pos_ = (pos_ + 1) % static_cast<size_t>(w_);
	if ((++n_ & 1023) == 0) {       /* the rounding the sums gather */
		for (int i = 0; i < 4; i++) {
			sum_[i] = 0;
			for (int k = 0; k < w_; k++)
				sum_[i] += buf_[static_cast<size_t>(k) * 4 +
						static_cast<size_t>(i)];
		}
	}
	double d = hypot(sum_[0], sum_[1]) - hypot(sum_[2], sum_[3]);
	/* the window straddles a bit boundary equally at a crossing: the
	   window that ends half a bit later is the next bit's */
	if ((d > 0) != (last_ > 0)) {
		/* the first crossings pull the clock in; after them a
		   crossing far from a boundary is the codec's noise */
		double e = phase_ < 0.5 ? -phase_ : 1 - phase_;
		if (acq_ < 12) {
			phase_ += 0.5 * e;
			acq_++;
		} else if (fabs(e) < 0.3)
			phase_ += 0.1 * e;
	}
	last_ = d;
	double p = phase_;
	phase_ += inc_;
	int bit = -1;
	if (p < 0.5 && phase_ >= 0.5)
		bit = d > 0;
	if (phase_ >= 1)
		phase_ -= 1;
	return bit;
}

struct VcvmMsgRx::Fsk {
	VcvmFskRx d;
};

/* the receiver, its FSK detector and deframer */
VcvmMsgRx::VcvmMsgRx() : fsk_(new Fsk), hdlc_(new Hdlc)
{
	hdlc_->o = this;
}

VcvmMsgRx::~VcvmMsgRx() = default;

/* see vcvm_startup.h: 0, or -1 for a rate not 8000 or 16000 */
int VcvmMsgRx::init(int fs, bool param, bool peer_calling)
{
	if (fs != 8000 && fs != 16000)
		return -1;
	fs_ = fs;
	param_ = param;
	if (param && prx_.init(fs, false))
		return -1;
	const float *ch = v21_ch[peer_calling ? 0 : 1];
	fsk_->d.init(ch[0], ch[1], fs);
	win_.assign(static_cast<size_t>(fs) / 100, 0);
	out_.clear();
	restart(0);
	return 0;
}

/* see vcvm_startup.h */
void VcvmMsgRx::restart(long at)
{
	out_.clear();
	n_ = at;
	active_ = got_ = false;
	quiet_ = loud_ = 0;
	std::fill(win_.begin(), win_.end(), 0);
	wsum_ = 0;
	wpos_ = 0;
	if (param_)
		prx_.restart(at);
	hdlc_->reset();
}

/* see vcvm_startup.h: the first whose FCS checks kept, its FCS off,
   while a transmission is coming in */
void VcvmMsgRx::frame(const uint8_t *d, int len)
{
	if (got_ || !active_ || len < 3 || hdlc_fcs(d, len) != HDLC_FCS_GOOD)
		return;
	first_.assign(d, d + len - 2);
	got_ = true;
}

/* no copy checked: the second best lag on the least sure symbols (each
   of the five, then each pair of them), until a frame's FCS checks */
bool VcvmMsgRx::chase()
{
	const int K = 5;
	std::vector<size_t> low(syms_.size());
	for (size_t i = 0; i < low.size(); i++)
		low[i] = i;
	size_t k = std::min<size_t>(K, low.size());
	std::partial_sort(low.begin(), low.begin() + k, low.end(),
			  [&](size_t a, size_t b) {
				  return syms_[a].margin < syms_[b].margin;
			  });
	auto trial = [&](long x, long y) {
		hdlc_->reset();
		for (size_t i = 0; i < syms_.size() && !got_; i++) {
			bool flip = static_cast<long>(i) == x ||
				    static_cast<long>(i) == y;
			uint8_t b[4];
			VcvmParamRx::bits(flip ? syms_[i].second : syms_[i].idx, b);
			for (int j = 0; j < 4; j++)
				hdlc_->hdlc_rx_bit(b[j]);
		}
		return got_;
	};
	for (size_t i = 0; i < k; i++)
		if (trial(static_cast<long>(low[i]), -1))
			return true;
	for (size_t i = 0; i < k; i++)
		for (size_t j = i + 1; j < k; j++)
			if (trial(static_cast<long>(low[i]),
				  static_cast<long>(low[j])))
				return true;
	return false;
}

/* a transmission's signal ended at sample at: P20's chased if no copy
   checked, the message queued for get() */
void VcvmMsgRx::ended(long at)
{
	if (param_ && !got_ && !syms_.empty())
		chase();
	syms_.clear();
	VcvmMsg m;
	m.ok = got_;
	if (got_)
		m.octets = first_;
	m.start = start_;
	m.end = at;
	out_.push_back(m);
	active_ = got_ = false;
	hdlc_->reset();
}

/* samples from the line: P20's symbols or the FSK's bits to the
   deframer; a transmission's start and end from its level (quiet: 10 ms
   under -40 dBFS for 30 ms), or P20's preamble */
void VcvmMsgRx::push(const float *x, int n)
{
	/* quiet: 10 ms under -40 dBFS */
	const double thr =
		pow(32767 * 0.01, 2) * static_cast<double>(win_.size());
	const long hold = fs_ * 30 / 1000, longest = 6L * fs_;
	if (param_) {
		prx_.push(x, n);
		if (!prx_.locked() && prx_.search()) {
			active_ = true;
			got_ = false;
			hdlc_->reset();
			syms_.clear();
			VcvmParam p;
			p.init(fs_, false);
			start_ = prx_.next_start() - 8L * p.sps;
		}
		int idx, second;
		float mg;
		while (prx_.next(&idx, nullptr, &mg, nullptr, &second)) {
			if (active_ && syms_.size() < 4096)
				syms_.push_back({ static_cast<uint8_t>(idx),
						  static_cast<uint8_t>(second),
						  mg });
			uint8_t b[4];
			VcvmParamRx::bits(idx, b);
			for (int i = 0; i < 4; i++)
				hdlc_->hdlc_rx_bit(b[i]);
		}
	}
	for (int i = 0; i < n; i++, n_++) {
		double e = static_cast<double>(x[i]) * x[i];
		wsum_ += e - win_[wpos_];
		win_[wpos_] = e;
		wpos_ = (wpos_ + 1) % win_.size();
		bool quiet = wsum_ < thr;
		quiet_ = quiet ? quiet_ + 1 : 0;
		loud_ = quiet ? 0 : loud_ + 1;
		if (!param_) {
			int b = fsk_->d.run(x[i]);
			if (!quiet && loud_ == 1)
				fsk_->d.resync();
			if (!active_ && loud_ > hold) {
				active_ = true;
				got_ = false;
				hdlc_->reset();
				start_ = n_ - hold;
			}
			if (active_ && b >= 0)
				hdlc_->hdlc_rx_bit(b);
		}
		if (active_ && (quiet_ >= hold || n_ - start_ > longest))
			ended(n_ - quiet_ + 1);
	}
	if (param_ && !active_ && prx_.locked())
		prx_.restart(n_);
}

/* see vcvm_startup.h: the oldest, true if there was one */
bool VcvmMsgRx::get(VcvmMsg *m)
{
	if (out_.empty())
		return false;
	*m = out_.front();
	out_.erase(out_.begin());
	return true;
}

} // namespace dsp
