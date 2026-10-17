/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm.cpp - the signal set of V.cvm (vcvm.h): the comb's and the
 *               parametric mode's transmitters and receivers
 *
 */

#include <math.h>
#include <string.h>

#include <algorithm>

#include "vcvm.h"

namespace dsp {

/* Table 10-2: spacing, the tones' multiples, the useful part and the
   prefix in ms (the 20 ms symbol) */
static const struct {
	int spacing, kmin, kmax;
	double tu, tg;
} combs[9] = {
	{ 200, 2, 17, 15, 5 },  { 300, 1, 11, 10, 10 }, { 400, 1, 8, 15, 5 },
	{ 200, 2, 34, 15, 5 },  { 300, 1, 23, 10, 10 }, { 400, 1, 17, 15, 5 },
	{ 200, 2, 69, 15, 5 },  { 300, 1, 46, 10, 10 }, { 400, 1, 35, 15, 5 },
};

/* the comb (the band of its code at its rate: N at 8000, W at 16000,
   S at 32000 Hz) */
int VcvmComb::init(int code_, int fs_, bool sym22_)
{
	static const int band_fs[3] = { 8000, 16000, 32000 };
	if (code_ < 0 || code_ > 8 || fs_ != band_fs[code_ / 3] ||
	    (sym22_ && code_ != 0))
		return -1;
	code = code_;
	fs = fs_;
	spacing = combs[code].spacing;
	kmin = combs[code].kmin;
	kmax = combs[code].kmax;
	tones = kmax - kmin + 1;
	sym22 = sym22_;
	/* the 22.5 ms symbol: four cycles of 200 Hz and 2.5 ms */
	double tu_ms = sym22 ? 20 : combs[code].tu;
	double tg_ms = sym22 ? 2.5 : combs[code].tg;
	tu = (int)lrint(fs * tu_ms / 1000);
	tg = (int)lrint(fs * tg_ms / 1000);
	sps = tu + tg;
	return 0;
}

void VcvmLoad::all(int n, int pb)
{
	memset(phase, 0, sizeof(phase));
	memset(ring, 0, sizeof(ring));
	for (int i = 0; i < n && i < VCVM_TONES; i++)
		phase[i] = (uint8_t)pb;
}

int VcvmLoad::loaded(int n) const
{
	int c = 0;
	for (int i = 0; i < n && i < VCVM_TONES; i++)
		c += phase[i] != 0;
	return c;
}

int VcvmLoad::bits(int n) const
{
	int b = 0;
	for (int i = 0; i < n && i < VCVM_TONES; i++)
		if (phase[i])
			b += phase[i] + ring[i];
	return b;
}

/* the useful part's cosine and sine of each tone */
static void tone_tables(const VcvmComb &c, std::vector<float> &co,
			std::vector<float> &si)
{
	co.assign((size_t)c.tones * c.tu, 0);
	si.assign((size_t)c.tones * c.tu, 0);
	for (int k = 0; k < c.tones; k++) {
		double w = 2 * M_PI * c.freq(k) / c.fs;
		for (int t = 0; t < c.tu; t++) {
			co[(size_t)k * c.tu + t] = (float)cos(w * t);
			si[(size_t)k * c.tu + t] = (float)sin(w * t);
		}
	}
}

/* the ring's step (dB) for ring bits rb, the number of rings */
static double ring_db(int rb) { return rb == 1 ? 6.0 : 3.0; }
static int rings(int rb) { return rb == 1 ? 2 : 4; }

/* g_R of 10.11: the rings' mean power that of a tone without */
static double ring_gain(int rb)
{
	if (!rb)
		return 1;
	int R = rings(rb);
	double s = 0;
	for (int r = 0; r < R; r++)
		s += pow(10, r * ring_db(rb) / 10);
	return 1 / sqrt(s / R);
}

int VcvmCombTx::init(int code, int fs, bool sym22, double dbfs)
{
	if (c_.init(code, fs, sym22))
		return -1;
	rms_ = 32767 * pow(10, dbfs / 20);
	tone_tables(c_, cos_, sin_);
	/* the tones start at Schroeder's phases, for a low peak factor */
	for (int k = 0; k < c_.tones; k++)
		ph_[k] = M_PI * k * k / c_.tones;
	VcvmLoad l;
	l.all(c_.tones, 2);
	set_load(l);
	return 0;
}

void VcvmCombTx::set_load(const VcvmLoad &l)
{
	load_ = l;
	int n = load_.loaded(c_.tones);
	double a = n ? rms_ * sqrt(2.0 / n) : 0;
	for (int k = 0; k < c_.tones; k++) {
		ring_[k] = 0;
		amp_[k] = load_.phase[k] ? a * ring_gain(load_.ring[k]) : 0;
	}
}

/* the symbol as the tones are now: the prefix (the useful part's end),
   then the useful part */
void VcvmCombTx::emit(float *out)
{
	const int tu = c_.tu, tg = c_.tg;
	std::vector<float> u(tu, 0.0f);
	for (int k = 0; k < c_.tones; k++) {
		if (amp_[k] == 0)
			continue;
		double a = amp_[k];
		if (load_.ring[k])
			a *= pow(10, ring_[k] * ring_db(load_.ring[k]) / 20);
		float ca = (float)(a * cos(ph_[k])), sa = (float)(a * sin(ph_[k]));
		const float *co = &cos_[(size_t)k * tu], *si = &sin_[(size_t)k * tu];
		for (int t = 0; t < tu; t++)
			u[t] += ca * co[t] - sa * si[t];
	}
	memcpy(out, &u[tu - tg], tg * sizeof(float));
	memcpy(out + tg, u.data(), tu * sizeof(float));
}

void VcvmCombTx::reference(float *out)
{
	for (int k = 0; k < c_.tones; k++)
		ring_[k] = 0;
	emit(out);
}

void VcvmCombTx::steps(const uint8_t *ph8, const uint8_t *rs, float *out)
{
	for (int k = 0; k < c_.tones; k++) {
		if (!load_.phase[k])
			continue;
		ph_[k] = fmod(ph_[k] + ph8[k] * M_PI / 4, 2 * M_PI);
		if (load_.ring[k] && rs)
			ring_[k] = (ring_[k] + rs[k]) % rings(load_.ring[k]);
	}
	emit(out);
}

/* n bits, the first the most significant */
static int take(const uint8_t *&b, int n)
{
	int v = 0;
	while (n--)
		v = (v << 1) | (*b++ & 1);
	return v;
}

void VcvmCombTx::data(const uint8_t *bits, float *out)
{
	uint8_t ph8[VCVM_TONES] = {}, rs[VCVM_TONES] = {};
	const uint8_t *b = bits;
	for (int k = 0; k < c_.tones; k++) {
		int pb = load_.phase[k];
		if (!pb)
			continue;
		int q = vcvm_ungray(take(b, pb));
		ph8[k] = (uint8_t)(pb == 2 ? 2 * q : q);
		if (load_.ring[k])
			rs[k] = (uint8_t)vcvm_ungray(take(b, load_.ring[k]));
	}
	steps(ph8, rs, out);
}

/*
 *    the comb's receiver
 */

int VcvmCombRx::init(int code, int fs, bool sym22)
{
	if (c_.init(code, fs, sym22))
		return -1;
	tone_tables(c_, cos_, sin_);
	VcvmLoad l;
	l.all(c_.tones, 2);
	set_load(l);
	buf_.clear();
	base_ = 0;
	locked_ = have_prev_ = false;
	return 0;
}

void VcvmCombRx::set_load(const VcvmLoad &l)
{
	load_ = l;
}

void VcvmCombRx::push(const float *x, int n)
{
	buf_.insert(buf_.end(), x, x + n);
	trim();
}

float VcvmCombRx::at(long i) const
{
	i -= base_;
	return i >= 0 && i < (long)buf_.size() ? buf_[(size_t)i] : 0.0f;
}

/* what is older than two symbols before the next is not needed again
   (before the lock: 12 s of it, for acquire()) */
void VcvmCombRx::trim()
{
	long keep = locked_ ? next_ - 2 * c_.sps : received() - 12L * c_.fs;
	if (keep - base_ > 4L * c_.fs) {
		buf_.erase(buf_.begin(), buf_.begin() + (keep - base_));
		base_ = keep;
	}
}

/* the useful part from sample u against tone k */
std::complex<double> VcvmCombRx::tone(long u, int k) const
{
	const float *co = &cos_[(size_t)k * c_.tu], *si = &sin_[(size_t)k * c_.tu];
	double re = 0, im = 0;
	long i = u - base_;
	if (i >= 0 && i + c_.tu <= (long)buf_.size()) {
		const float *y = &buf_[(size_t)i];
		for (int t = 0; t < c_.tu; t++) {
			re += y[t] * co[t];
			im -= y[t] * si[t];
		}
	} else {
		for (int t = 0; t < c_.tu; t++) {
			re += at(u + t) * co[t];
			im -= at(u + t) * si[t];
		}
	}
	return std::complex<double>(re, im);
}

long VcvmCombRx::onset(long from, double dbfs) const
{
	int w = c_.fs / 200, step = c_.fs / 1000;
	double thr = pow(32767 * pow(10, dbfs / 20), 2) * w;
	if (from < base_)
		from = base_;
	for (long a = from; a + w <= received(); a += step) {
		double e = 0;
		for (int t = 0; t < w; t++)
			e += (double)at(a + t) * at(a + t);
		if (e >= thr)
			return a;
	}
	return -1;
}

/* the prefix's correlation with the useful part's end, over 8 symbols,
   at each phase of the symbol: the best is the prefix's start */
int VcvmCombRx::acquire(long from, long first)
{
	const int S = 8, sps = c_.sps, tu = c_.tu, tg = c_.tg;
	if (from < base_ || received() < from + (long)(S + 1) * sps + tu)
		return -1;
	double best = -1e300;
	int bt = 0;
	for (int th = 0; th < sps; th++) {
		double m = 0, e1 = 0, e2 = 0;
		for (int s = 0; s < S; s++) {
			long a = from + th + (long)s * sps;
			for (int t = 0; t < tg; t++) {
				double x = at(a + t), y = at(a + t + tu);
				m += x * y;
				e1 += x * x;
				e2 += y * y;
			}
		}
		m /= sqrt(e1 * e2) + 1e-9;
		if (m > best) {
			best = m;
			bt = th;
		}
	}
	long s0 = from + bt;
	if (first >= 0) {
		while (s0 + sps / 2 < first)
			s0 += sps;
		while (s0 - sps / 2 > first && s0 - sps >= base_)
			s0 -= sps;
	}
	next_ = s0;
	locked_ = true;
	have_prev_ = false;
	tau_ = drift_ = rot_ = 0;
	return 0;
}

static double wrap(double a)
{
	a = fmod(a + M_PI, 2 * M_PI);
	if (a < 0)
		a += 2 * M_PI;
	return a - M_PI;
}

bool VcvmCombRx::next(VcvmCombSym *s, const uint8_t *known)
{
	const int K = c_.tones;
	if (!locked_ || received() < next_ + c_.sps + 2)
		return false;
	/* the useful part, an eighth of the prefix early: the timing may
	   be half a sample off either way, and the next symbol must not
	   leak in (the shift is in the cyclic part: a constant phase a
	   tone, which the differences take off) */
	long u = next_ + c_.tg - c_.tg / 8;
	double tp = tau_ + drift_;              /* this symbol's timing */
	std::complex<double> r[VCVM_TONES];
	double lv[VCVM_TONES], w[VCVM_TONES], maxl = 0;
	for (int k = 0; k < K; k++) {
		w[k] = 2 * M_PI * c_.freq(k) / c_.fs;
		r[k] = tone(u, k) * std::polar(1.0, w[k] * tp) *
			(2.0 / c_.tu);
		lv[k] = std::abs(r[k]);
		maxl = std::max(maxl, lv[k]);
	}
	memset(s, 0, sizeof(*s));
	s->start = next_;
	for (int k = 0; k < K; k++)
		s->level[k] = (float)lv[k];
	if (!have_prev_) {
		/* the reference: its tones are the next symbol's */
		have_prev_ = true;
		for (int k = 0; k < K; k++)
			prev_[k] = r[k];
		tau_ = tp;
		next_ += c_.sps;
		trim();
		return true;
	}
	double se = 0, sw = 0, swx = 0, swxx = 0, swy = 0, swxy = 0;
	int nb = 0;
	for (int k = 0; k < K; k++) {
		std::complex<double> z = r[k] * std::conj(prev_[k]);
		double d = wrap(std::arg(z) - rot_);
		int pb = load_.phase[k], rb = load_.ring[k];
		int q8;
		if (known)
			q8 = known[k] & 7;
		else if (pb == 2)
			q8 = 2 * (((int)lrint(d / (M_PI / 2)) % 4 + 4) % 4);
		else
			q8 = ((int)lrint(d / (M_PI / 4)) % 8 + 8) % 8;
		double e = wrap(d - q8 * M_PI / 4);
		double rdb = 20 * log10((lv[k] + 1e-9) /
					(std::abs(prev_[k]) + 1e-9));
		s->dphi[k] = (float)d;
		s->ratio_db[k] = (float)rdb;
		s->ph8[k] = (uint8_t)q8;
		s->perr[k] = (float)e;
		if (pb) {
			int M = pb == 2 ? 4 : 8;
			int q = pb == 2 ? q8 / 2 : q8;
			int g = vcvm_gray(q);
			float rel = (float)std::max(0.0, 1 - fabs(e) / (M_PI / M));
			for (int i = pb - 1; i >= 0; i--) {
				s->bits[nb] = (uint8_t)((g >> i) & 1);
				s->rel[nb++] = rel;
			}
			if (rb) {
				int R = rings(rb);
				double st = ring_db(rb);
				int dd = (int)lrint(rdb / st);
				double re = fabs(rdb - dd * st) / (st / 2);
				dd = (dd % R + R) % R;
				s->rs[k] = (uint8_t)dd;
				int gg = vcvm_gray(dd);
				for (int i = rb - 1; i >= 0; i--) {
					s->bits[nb] = (uint8_t)((gg >> i) & 1);
					s->rel[nb++] = (float)std::max(0.0, 1 - re);
				}
			}
		}
		/* the timing's fit: the tones decided surely (or known),
		   strong enough to count */
		bool use = known ? lv[k] > 0.1 * maxl :
			pb && fabs(e) < 0.6 * M_PI / (pb == 2 ? 4 : 8);
		if (use) {
			double wt = lv[k];
			sw += wt;
			swx += wt * w[k];
			swxx += wt * w[k] * w[k];
			swy += wt * e;
			swxy += wt * w[k] * e;
			se += wt * e * e;
		}
	}
	s->nbits = nb;
	/* e = a + b w: b the timing's change (the window late by -b
	   samples), a a rotation common to the tones */
	double det = sw * swxx - swx * swx;
	double a = 0, b = 0;
	if (sw > 0 && det > 1e-12 * sw * sw) {
		b = (sw * swxy - swx * swy) / det;
		a = (swy - b * swx) / sw;
	} else if (sw > 0)
		a = swy / sw;
	double eps = -b;                /* the window early by eps */
	tau_ = tp + 0.5 * eps;
	drift_ += 0.05 * eps;
	rot_ += 0.2 * a;
	/* the tones as the next symbol's reference, at the timing now */
	for (int k = 0; k < K; k++)
		prev_[k] = r[k] * std::polar(1.0, w[k] * (tau_ - tp));
	s->tau = (float)tau_;
	s->drift = (float)drift_;
	next_ += c_.sps;
	if (tau_ > 0.5) {
		next_ += 1;
		tau_ -= 1;
	} else if (tau_ < -0.5) {
		next_ -= 1;
		tau_ += 1;
	}
	trim();
	(void)se;
	return true;
}

/*
 *    the parametric mode
 */

int VcvmParam::init(int fs_, bool p10_)
{
	if (fs_ != 8000 && fs_ != 16000)
		return -1;
	fs = fs_;
	p10 = p10_;
	int m = fs / 8000;
	sps = (p10 ? 80 : 160) * m;
	for (int i = 0; i < 16; i++)
		lag[i] = (p10 ? 20 + i : 40 + 2 * i) * m;
	return 0;
}

int VcvmParamTx::init(int fs, bool p10, double dbfs)
{
	if (p_.init(fs, p10))
		return -1;
	/* the band-pass pulse: 300 Hz to the band's top under a Hamming
	   window, 33 taps at 8000 Hz */
	int n = 33 * fs / 8000;
	double top = fs == 8000 ? 3400 : 7000;
	h_.assign(n, 0);
	for (int i = 0; i < n; i++) {
		int k = i - n / 2;
		auto sinc = [&](double fc) {
			return k == 0 ? 2 * fc / fs :
				sin(2 * M_PI * fc / fs * k) / (M_PI * k);
		};
		h_[i] = (float)((sinc(top) - sinc(300)) *
				(0.54 - 0.46 * cos(2 * M_PI * i / (n - 1))));
	}
	/* each lag's gain: its symbol at dbfs rms, measured in the steady
	   state of a train of it */
	double want = 32767 * pow(10, dbfs / 20);
	std::vector<float> y(p_.sps);
	for (int i = 0; i < 16; i++) {
		gain_[i] = 1;
		fir_.assign(n, 0);
		pos_ = 0;
		tilt_ = 0;
		for (int s = 0; s < 4; s++)
			symbol(i, y.data());
		double e = 0;
		for (float v : y)
			e += (double)v * v;
		gain_[i] = (float)(want / (sqrt(e / p_.sps) + 1e-9));
	}
	fir_.assign(n, 0);
	pos_ = 0;
	tilt_ = 0;
	return 0;
}

/* an excitation sample through the tilt and the pulse */
float VcvmParamTx::step(float e)
{
	tilt_ = e + 0.7f * tilt_;
	int n = (int)h_.size();
	fir_[pos_] = tilt_;
	double y = 0;
	for (int i = 0; i < n; i++)
		y += h_[i] * fir_[(pos_ - i + n) % n];
	pos_ = (pos_ + 1) % n;
	return (float)y;
}

void VcvmParamTx::symbol(int i, float *out)
{
	int L = p_.lag[i & 15];
	for (int m = 0; m < p_.sps; m++)
		out[m] = step(m % L == 0 ? gain_[i & 15] : 0.0f);
}

void VcvmParamTx::data(const uint8_t *bits, float *out)
{
	const uint8_t *b = bits;
	symbol(vcvm_ungray(take(b, 4)), out);
}

void VcvmParamTx::preamble(float *out)
{
	for (int s = 0; s < 8; s++)
		symbol(s & 1 ? 15 : 0, out + s * p_.sps);
}

int VcvmParamRx::init(int fs, bool p10)
{
	if (p_.init(fs, p10))
		return -1;
	buf_.clear();
	base_ = 0;
	locked_ = false;
	searched_ = 0;
	cs_.assign(2000 / (p10 ? 10 : 20), 0);
	csn_ = csi_ = 0;
	fitn_ = 0;
	return 0;
}

void VcvmParamRx::push(const float *x, int n)
{
	buf_.insert(buf_.end(), x, x + n);
	trim();
}

float VcvmParamRx::at(long i) const
{
	i -= base_;
	return i >= 0 && i < (long)buf_.size() ? buf_[(size_t)i] : 0.0f;
}

void VcvmParamRx::trim()
{
	long keep = locked_ ? next_ - 2 * p_.sps :
		std::min(searched_ - 2L * p_.sps, received() - 12L * p_.fs);
	keep -= 8L * p_.sps;
	if (keep - base_ > 4L * p_.fs) {
		buf_.erase(buf_.begin(), buf_.begin() + (keep - base_));
		base_ = keep;
	}
}

/* the normalized autocorrelation of [a, b) at lag L (10.6.5) */
float VcvmParamRx::corr(long a, long b, int L) const
{
	double xy = 0, xx = 0, yy = 0;
	for (long m = a; m + L < b; m++) {
		double x = at(m), y = at(m + L);
		xy += x * y;
		xx += x * x;
		yy += y * y;
	}
	return (float)(xy / (sqrt(xx * yy) + 1e-9));
}

/* the symbol from a: the lag of the greatest c, the smallest within
   0.02 of it */
int VcvmParamRx::decide(long a, float *c, float *margin) const
{
	float cs[16], best = -2;
	/* the symbol but its first sixteenth (1.25 ms of P20), where the
	   codec's predictor goes over to the new lag */
	long a0 = a + p_.sps / 16;
	for (int i = 0; i < 16; i++) {
		cs[i] = corr(a0, a + p_.sps, p_.lag[i]);
		best = std::max(best, cs[i]);
	}
	int d = 0;
	while (d < 15 && cs[d] < best - 0.02f)
		d++;
	float other = -2;
	for (int i = 0; i < 16; i++)
		if (i != d)
			other = std::max(other, cs[i]);
	if (c)
		*c = cs[d];
	if (margin)
		*margin = cs[d] - other;
	return d;
}

/* the preamble's 8 windows ending at e: 0, 15, 0, 15 ... by c at the
   two lags; the score their c's sum, or -1 */
bool VcvmParamRx::search()
{
	const int T = p_.sps, L0 = p_.lag[0], L15 = p_.lag[15];
	const int step = p_.fs / 4000;
	if (locked_)
		return true;
	if (searched_ < base_ + 8L * T)
		searched_ = base_ + 8L * T;
	auto score = [&](long e) -> float {
		float sc = 0;
		for (int w = 7; w >= 0; w--) {
			long a = e - (long)(8 - w) * T;
			float c0 = corr(a, a + T, L0), c1 = corr(a, a + T, L15);
			bool hi = w & 1;
			float cw = hi ? c1 : c0, co = hi ? c0 : c1;
			if (cw < 0.5f || cw < co + 0.1f)
				return -1;
			sc += cw;
		}
		return sc;
	};
	for (long e = searched_; e <= received(); e += step) {
		searched_ = e;
		if (score(e) < 0)
			continue;
		/* the best end within a quarter symbol of the first */
		if (received() < e + T / 4)
			return false;
		float bs = -1;
		long be = e;
		for (long x = e; x <= e + T / 4; x++) {
			float sc = score(x);
			if (sc > bs) {
				bs = sc;
				be = x;
			}
		}
		next_ = be;
		locked_ = true;
		fitn_ = 0;
		fit_[0] = fit_[1] = fit_[2] = 0;
		return true;
	}
	return false;
}

bool VcvmParamRx::next(int *idx, float *c, float *margin)
{
	const int T = p_.sps;
	if (!locked_ || received() < next_ + T + 1)
		return false;
	float cc, mg;
	int d = decide(next_, &cc, &mg);
	*idx = d;
	if (c)
		*c = cc;
	if (margin)
		*margin = mg;
	cs_[(size_t)csi_] = cc;
	csi_ = (csi_ + 1) % (int)cs_.size();
	csn_ = std::min(csn_ + 1, (int)cs_.size());
	/* the timing: the decided lag's c a sample either side, over 8
	   symbols, against here */
	int L = p_.lag[d];
	fit_[0] += corr(next_ - 1, next_ - 1 + T, L);
	fit_[1] += cc;
	fit_[2] += corr(next_ + 1, next_ + 1 + T, L);
	next_ += T;
	if (++fitn_ == 8) {
		if (fit_[0] > fit_[1] * 1.002f && fit_[0] > fit_[2])
			next_ -= 1;
		else if (fit_[2] > fit_[1] * 1.002f)
			next_ += 1;
		fit_[0] = fit_[1] = fit_[2] = 0;
		fitn_ = 0;
	}
	trim();
	return true;
}

void VcvmParamRx::bits(int idx, uint8_t *b)
{
	int g = vcvm_gray(idx & 15);
	for (int i = 0; i < 4; i++)
		b[i] = (uint8_t)((g >> (3 - i)) & 1);
}

float VcvmParamRx::cbar() const
{
	if (!csn_)
		return 0;
	double s = 0;
	for (int i = 0; i < csn_; i++)
		s += cs_[(size_t)i];
	return (float)(s / csn_);
}

} // namespace dsp
