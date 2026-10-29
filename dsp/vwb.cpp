/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vwb.cpp - V.wb, the wideband DMT modem for a bare pair (vwb.h,
 *              docs/dsp/vwb.md)
 *
 */

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bits.h"
#include "vwb.h"

namespace dsp {

namespace {

const float TWO_PI = 6.283185307179586f;

/* the start-up's lengths, in symbols */
const s32 L_SYNC = 160;        /* each side's full-band sync */
const s32 ECHO_TAIL = 32;      /* our own echo heard so long after we
				  sent it, at most (184 ms: the host's
				  buffers both ways, a USB or ALSA line's
				  tens of ms, and the pair's): a far end's
				  sync looked for only after it */
const s32 L_SYNC2 = 48 + ECHO_TAIL;   /* the answerer's on the default
					 band (found after the caller's
					 echo of its own full-band one) */
const s32 L_SYNC3 = 24;        /* each side's on the plan's band */
const s32 SNR_FROM = 48;       /* the SNR measured from this symbol of
				  the far end's full-band sync */
const s32 ECHO_AT = 60;        /* the answerer's echo measured then */
const s16 REPEAT_C = 2;        /* a message bit on that many tones: the
				  caller's (its low band, the pair's
				  best), */
const s16 REPEAT_A = 4;        /* the answerer's (its high band) */
const s32 TX_LEVEL = 5000;     /* the full band's RMS, of 32767 */
const s16 RS_N = 255, RS_K = 239;

/* x modulo m in [0, m) */
s64 mod(s64 x, s64 m)
{
	s64 r = x % m;
	return r < 0 ? r + m : r;
}

/* the CRC-32 (IEEE 802.3) of n octets */
u32 crc32(const u8 *p, size_t n)
{
	u32 c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (s16 k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
	}
	return ~c;
}

/* the modified Bessel function I0 (the Kaiser window's) */
double bessel_i0(double x)
{
	double s = 1, t = 1;
	for (s16 k = 1; k < 40; k++) {
		t *= (x / (2 * k)) * (x / (2 * k));
		s += t;
	}
	return s;
}

/* bits on a PAM axis of 2^m levels: its Gray code to the level index */
s32 gray_index(u32 g)
{
	u32 i = g;
	for (u32 s = 1; s < 16; s <<= 1)
		i ^= i >> s;
	return static_cast<s32>(i);
}

} // namespace

/*
 *    the FFT
 */

/* see vwb.h */
void VwbFft::init(s32 n)
{
	n_ = n;
	tw_.resize(static_cast<size_t>(n / 2));
	for (s32 k = 0; k < n / 2; k++)
		tw_[static_cast<size_t>(k)] = std::polar(1.0f,
			-TWO_PI * static_cast<float>(k) / static_cast<float>(n));
	rev_.resize(static_cast<size_t>(n));
	s32 bits = 0;
	while ((1 << bits) < n)
		bits++;
	for (s32 i = 0; i < n; i++) {
		s32 r = 0;
		for (s32 b = 0; b < bits; b++)
			if (i & (1 << b))
				r |= 1 << (bits - 1 - b);
		rev_[static_cast<size_t>(i)] = r;
	}
}

/* in place, decimation in time; inv: the conjugate twiddles */
void VwbFft::run(cf *x, bool inv) const
{
	for (s32 i = 0; i < n_; i++) {
		s32 r = rev_[static_cast<size_t>(i)];
		if (r > i)
			std::swap(x[i], x[r]);
	}
	for (s32 len = 2; len <= n_; len <<= 1) {
		s32 half = len / 2, step = n_ / len;
		for (s32 i = 0; i < n_; i += len)
			for (s32 j = 0; j < half; j++) {
				cf w = tw_[static_cast<size_t>(j * step)];
				if (inv)
					w = std::conj(w);
				cf a = x[i + j], b = x[i + j + half] * w;
				x[i + j] = a + b;
				x[i + j + half] = a - b;
			}
	}
}

/*
 *    interpolation, histories, the clock
 */

/* a Kaiser (beta 5.65: 60 dB) windowed sinc cut at 0.46 of the rate,
   each phase's taps summing to 1 */
VwbInterp::VwbInterp() : tab_(static_cast<size_t>((PHASES + 1) * 2 * HALF))
{
	const double fc = 0.92, beta = 5.65, i0b = bessel_i0(beta);
	for (s32 p = 0; p <= PHASES; p++) {
		double frac = static_cast<double>(p) / PHASES, sum = 0;
		float *h = &tab_[static_cast<size_t>(p * 2 * HALF)];
		for (s32 j = 0; j < 2 * HALF; j++) {
			/* tap j weighs sample i0 - HALF + 1 + j: at u from
			   the instant */
			double u = (j - HALF + 1) - frac;
			double x = M_PI * fc * u;
			double sinc = std::fabs(u) < 1e-9 ? 1 : std::sin(x) / x;
			double w = u / HALF;
			double win = std::fabs(w) >= 1 ? 0 :
				bessel_i0(beta * std::sqrt(1 - w * w)) / i0b;
			h[j] = static_cast<float>(fc * sinc * win);
			sum += h[j];
		}
		for (s32 j = 0; j < 2 * HALF; j++)
			h[j] = static_cast<float>(h[j] / sum);
	}
}

/* see vwb.h */
void VwbHistory::init(s32 n)
{
	size_t m = 1;
	while (m < static_cast<size_t>(n))
		m <<= 1;
	buf_.assign(m, 0.0f);
	mask_ = static_cast<s64>(m) - 1;
	end_ = 0;
}

/* see vwb.h */
void VwbClock::loop(double v, double e, s32 s, double alpha, double beta)
{
	/* the mapping stays continuous: the phase's correction too is a
	   change of rate, over the next symbol (a jump would put a step
	   into the samples of a symbol) */
	t0 = line(v);
	v0 = v;
	freq += beta * e;
	r = 1 + (freq + alpha * e) / s;
}

/* see vwb.h */
void VwbClock::set_ppm(double v, double ppm, s32 s)
{
	t0 = line(v);
	v0 = v;
	freq = ppm * 1e-6 * s;
	r = 1 + freq / s;
}

/*
 *    QAM
 */

/* see vwb.h */
cf VwbQam::map(s16 b, u32 bits)
{
	s16 mi = static_cast<s16>((b + 1) / 2), mq = static_cast<s16>(b / 2);
	s32 li = 1 << mi, lq = 1 << mq;
	float p = static_cast<float>((li * li - 1) + (lq * lq - 1)) / 3;
	float sc = 1 / std::sqrt(p);
	s32 ii = gray_index(bits & ((1u << mi) - 1));
	s32 iq = gray_index(bits >> mi);
	return cf(static_cast<float>(2 * ii - (li - 1)) * sc,
		  static_cast<float>(2 * iq - (lq - 1)) * sc);
}

/* see vwb.h */
u32 VwbQam::slice(s16 b, cf z)
{
	s16 mi = static_cast<s16>((b + 1) / 2), mq = static_cast<s16>(b / 2);
	s32 li = 1 << mi, lq = 1 << mq;
	float p = static_cast<float>((li * li - 1) + (lq * lq - 1)) / 3;
	float sc = std::sqrt(p);
	auto axis = [](float x, s32 l) {
		s32 i = static_cast<s32>(std::lround((x + (l - 1)) / 2));
		i = std::min(std::max(i, 0), l - 1);
		return static_cast<u32>(i ^ (i >> 1));
	};
	u32 gi = axis(z.real() * sc, li);
	u32 gq = mq ? axis(z.imag() * sc, lq) : 0;
	return gi | gq << mi;
}

/* see vwb.h */
float VwbQam::snr_needed(s16 b, float q)
{
	s16 mi = static_cast<s16>((b + 1) / 2), mq = static_cast<s16>(b / 2);
	s32 li = 1 << mi, lq = 1 << mq;
	float p = static_cast<float>((li * li - 1) + (lq * lq - 1)) / 3;
	return q * q * p / 2;
}

/*
 *    loadings
 */

/* see vwb.h */
void VwbLoading::band(s32 n, s16 lo_, s16 hi_, s16 b, s16 npilots)
{
	code.assign(static_cast<size_t>(n / 2), 0);
	gain.assign(static_cast<size_t>(n / 2), 0.0f);
	lo = lo_;
	hi = hi_;
	for (s16 k = lo; k < hi; k++) {
		code[static_cast<size_t>(k)] = static_cast<u8>(b);
		gain[static_cast<size_t>(k)] = 1;
	}
	for (s16 i = 0; i < npilots && hi > lo; i++) {
		s32 k = lo + ((2 * i + 1) * (hi - lo)) / (2 * npilots);
		code[static_cast<size_t>(k)] = VWB_PILOT;
	}
	count();
}

/* see vwb.h */
void VwbLoading::count()
{
	bits = 0;
	for (u8 c : code)
		if (c && c != VWB_PILOT)
			bits += c;
}

/* see vwb.h */
std::string VwbLoading::str() const
{
	s32 n[16] = {};
	for (s16 k = lo; k < hi; k++)
		n[code[static_cast<size_t>(k)] & 15]++;
	std::string s = "tones " + std::to_string(lo) + "-" +
		std::to_string(hi - 1) + ":";
	for (s16 b = 1; b <= VWB_MAX_BITS; b++)
		if (n[b])
			s += " b" + std::to_string(b) + "x" + std::to_string(n[b]);
	s += " pilots " + std::to_string(n[VWB_PILOT]) + ", off " +
		std::to_string(n[0]) + ", " + std::to_string(bits) +
		" bits a symbol";
	return s;
}

/*
 *    the modem
 */

/* see vwb.h */
s16 VwbModem::init(const Config &c, Source src, Sink sink)
{
	if (c.fs != 48000 && c.fs != 96000 && c.fs != 192000)
		return -1;
	cfg_ = c;
	src_ = std::move(src);
	sink_ = std::move(sink);
	n_ = 512 * c.fs / 96000;
	cp_ = n_ * 40 / 512;
	s_ = n_ + cp_;
	adv_ = cp_ / 2;
	klo_ = c.lo ? c.lo : 21;
	khi_ = c.hi ? c.hi :
		static_cast<s16>(std::min<s32>(193, n_ * 2 / 5 + 1));
	kprobe_ = 24;
	scale_ = static_cast<float>(TX_LEVEL /
		std::sqrt(2.0 * (khi_ - klo_)));
	fft_.init(n_);
	/* the sync symbol: QPSK from a PRBS (x^9 + x^5 + 1) */
	xsync_.assign(static_cast<size_t>(n_ / 2), cf(0, 0));
	u32 reg = 0x1ff;
	for (s32 k = 0; k < n_ / 2; k++) {
		s32 b[2];
		for (s16 j = 0; j < 2; j++) {
			s32 bit = static_cast<s32>(((reg >> 8) ^ (reg >> 4)) & 1);
			reg = ((reg << 1) | static_cast<u32>(bit)) & 0x1ff;
			b[j] = bit;
		}
		xsync_[static_cast<size_t>(k)] =
			cf(b[0] ? -0.70710678f : 0.70710678f,
			   b[1] ? -0.70710678f : 0.70710678f);
	}
	full_.band(n_, klo_, khi_, 2, 0);
	/* the message channel's whitening: x^15 + x^14 + 1 */
	whiten_.resize(1 << 15);
	u32 wr = 1;
	for (u8 &w : whiten_) {
		u32 b = ((wr >> 14) ^ (wr >> 13)) & 1;
		wr = ((wr << 1) | b) & 0x7fff;
		w = static_cast<u8>(b);
	}
	/* the message channel's bands: the lowest and the highest 30% of
	   the tones, so that the plan's split (between them) never puts a
	   side's band to receive on the other's messages, while one side
	   has switched to the plan and the other not yet */
	edge_ = static_cast<s16>(std::max(16, (khi_ - klo_) * 3 / 10));
	VwbLoading lower, upper;
	lower.band(n_, klo_, static_cast<s16>(klo_ + edge_), 2, 4);
	upper.band(n_, static_cast<s16>(khi_ - edge_), khi_, 2, 4);
	msg_tx_ = c.calling ? lower : upper;
	msg_rx_ = c.calling ? upper : lower;
	rep_tx_ = c.calling ? REPEAT_C : REPEAT_A;
	rep_rx_ = c.calling ? REPEAT_A : REPEAT_C;
	auto frame_syms = [](const VwbLoading &l, s32 payload, s16 rep) {
		s32 nb = 0;
		for (s16 k = l.lo; k < l.hi; k++)
			if (l.code[static_cast<size_t>(k)] == 2)
				nb += 2;
		s32 per = nb / rep;
		s32 bits = (3 + payload + 4) * 8;
		return (bits + per - 1) / per;
	};
	s32 nt = khi_ - klo_;
	/* the answerer's frames carry the SNR, the caller's the plan */
	s32 a_pay = nt, c_pay = 4 + 2 * nt;
	frame_syms_tx_ = frame_syms(msg_tx_, c.calling ? c_pay : a_pay, rep_tx_);
	frame_syms_rx_ = frame_syms(msg_rx_, c.calling ? a_pay : c_pay, rep_rx_);
	frame_pay_tx_ = c.calling ? c_pay : a_pay;
	frame_pay_rx_ = c.calling ? a_pay : c_pay;
	rs_.init(RS_N, RS_K);
	line_in_.init(4 * s_ + 4 * VwbInterp::HALF);
	rx_hist_.init(8 * s_);
	tx_hist_.init(8 * s_);
	h_.assign(static_cast<size_t>(n_ / 2), cf(0, 0));
	err_.assign(static_cast<size_t>(n_ / 2), 0.0f);
	snr_rx_.assign(static_cast<size_t>(n_ / 2), 0.0f);
	snr_far_.assign(static_cast<size_t>(n_ / 2), 0.0f);
	cw_hist_.assign(32, 0);
	logf("V.wb %s at %d Hz: FFT %d, prefix %d, tones %d-%d, "
	     "%.1f symbols/s\n", c.calling ? "caller" : "answerer",
	     static_cast<int>(c.fs), static_cast<int>(n_),
	     static_cast<int>(cp_), klo_, khi_ - 1,
	     static_cast<double>(c.fs) / s_);
	restart();
	return 0;
}

/* a line of the log */
void VwbModem::logf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	char t[32];
	snprintf(t, sizeof(t), "%.3f s: ",
		 static_cast<double>(line_t_) / cfg_.fs);
	log_ += t;
	log_ += buf;
}

/* see vwb.h */
std::string VwbModem::take_log()
{
	std::string s;
	s.swap(log_);
	return s;
}

/* the start-up from the beginning: the caller probing, the answerer
   listening for it */
void VwbModem::restart()
{
	rx_state_ = cfg_.calling ? RX_DETECT : RX_PROBE_WAIT;
	rx_count_ = 0;
	detect_hits_ = 0;
	segs_ = 0;
	lost_ = 0;
	plan_ok_ = false;
	far_go_left_ = 0;
	go_left_ = 0;
	echo_ = -1;
	echo_tx0_ = -1;
	shift_ = 0;
	pad_ = 0;
	tx_data_ = rx_data_ = false;
	frame_rx_sym_ = 0;
	frame_soft_.clear();
	tx_set(cfg_.calling ? TX_PROBE : TX_SILENT);
	if (state_ == DATA)
		state_ = STARTUP;
}

/* lost: the start-up again (a retrain), unless the modem failed */
void VwbModem::retrain(const char *why)
{
	if (state_ == FAILED)
		return;
	logf("retrain: %s\n", why);
	retrains_++;
	restart();
}

/* the transmitter to state t, n symbols of it (for those that count) */
void VwbModem::tx_set(Tx t, s32 n)
{
	tx_state_ = t;
	tx_left_ = n;
	tx_count_ = 0;
}

/* see vwb.h */
void VwbModem::process(const s16 *in, s16 *out, s32 n)
{
	for (s32 i = 0; i < n; i++)
		line_in_.push(in[i]);
	/* the receiver: the far clock's samples, a window at a time */
	auto lin = [this](s64 i) { return line_in_(i); };
	for (;;) {
		double t = clk_.line(static_cast<double>(rx_next_));
		if (std::floor(t) + VwbInterp::HALF >=
		    static_cast<double>(line_in_.end()))
			break;
		rx_hist_.push(interp_.at(lin, t));
		rx_next_++;
		if (rx_next_ >= win_ + n_) {
			std::vector<cf> y(static_cast<size_t>(n_));
			window(win_, y.data());
			win_ += s_;
			rx_symbol(y.data());
			rx_total_++;
		}
	}
	/* the transmitter: the answerer's on the far clock (the
	   receiver's), the caller's on its own */
	auto txh = [this](s64 i) { return tx_hist_(i); };
	for (s32 i = 0; i < n; i++, line_t_++) {
		float y;
		if (cfg_.calling) {
			while (tx_next_ <= line_t_)
				tx_symbol();
			y = tx_hist_(line_t_);
		} else {
			double u = clk_.virt(static_cast<double>(line_t_));
			while (static_cast<double>(tx_next_) <=
			       std::floor(u) + VwbInterp::HALF)
				tx_symbol();
			y = interp_.at(txh, u);
		}
		y = std::min(std::max(y, -32767.0f), 32767.0f);
		out[i] = static_cast<s16>(std::lround(y));
	}
	if (state_ != DATA && state_ != FAILED) {
		since_data_ += n;
		if (since_data_ > static_cast<s64>(cfg_.timeout_s) * cfg_.fs) {
			state_ = FAILED;
			why_ = "no data connection in time";
			logf("failed: %s\n", why_);
		}
	} else {
		since_data_ = 0;
	}
}

/* the FFT of the receiver's window at virtual sample at, into y */
void VwbModem::window(s64 at, cf *y) const
{
	for (s32 i = 0; i < n_; i++)
		y[i] = cf(rx_hist_(at + i), 0);
	fft_.forward(y);
}

/*
 *    the receiver
 */

/* one window of the receiver (y its FFT), by the receiver's state */
void VwbModem::rx_symbol(cf *y)
{
	rx_count_++;
	switch (rx_state_) {
	case RX_PROBE_WAIT: {
		/* the caller's probe: its tone 20 dB over the others */
		float p = std::norm(y[kprobe_]), o = 0;
		s32 m = 0;
		for (s16 k = klo_; k < khi_; k++)
			if (std::abs(k - kprobe_) > 2) {
				o += std::norm(y[k]);
				m++;
			}
		o = o / static_cast<float>(std::max(m, 1)) + 1e-3f;
		if (p > 100 * o && p > 1e6f)
			detect_hits_++;
		else
			detect_hits_ = 0;
		if (detect_hits_ >= 6) {
			logf("probe heard: sync on the whole band\n");
			tx_set(TX_SYNC, L_SYNC);
			tx_band_ = &full_;
			tx_after_seg_ = TX_SILENT;
			echo_tx0_ = -1;
			rx_state_ = RX_ECHO;
			rx_count_ = 0;
		}
		break;
	}
	case RX_ECHO:
		if (rx_count_ == ECHO_AT)
			measure_echo();
		/* (the caller's sync looked for once our own has gone by:
		   its echo would be found) */
		if (tx_state_ == TX_SILENT && tx_count_ > ECHO_TAIL &&
		    rx_count_ > ECHO_AT) {
			rx_state_ = RX_DETECT;
			rx_count_ = 0;
			detect_hits_ = 0;
		}
		break;
	case RX_DETECT:
		/* the far end's full-band sync: two finds that agree */
		if (rx_count_ >= 4 && rx_count_ % 2 == 0) {
			s64 before = win_;
			if (acquire(full_, static_cast<s16>(kprobe_ - 2),
				    static_cast<s16>(kprobe_ + 2))) {
				if (detect_hits_ &&
				    std::llabs(mod(win_ - detect_at_ + s_ / 2, s_) -
					       s_ / 2) <= 3)
					detect_hits_++;
				else
					detect_hits_ = 1;
				detect_at_ = win_;
				detect_last_ = rx_count_;
				if (cfg_.calling && detect_hits_ == 1)
					tx_set(TX_SILENT);
			} else {
				win_ = before;
				detect_hits_ = 0;
			}
			if (detect_hits_ >= 2) {
				logf("sync found\n");
				rx_state_ = RX_TRAIN;
				rx_count_ = 0;
				segs_ = 0;
				acc_n_ = 0;
				sig_acc_.assign(static_cast<size_t>(n_ / 2), 0);
				noise_acc_.assign(static_cast<size_t>(n_ / 2), 0);
			}
		}
		if (!cfg_.calling && rx_count_ > 600)
			retrain("no sync from the caller");
		/* the caller's probe again if what it found went away */
		if (cfg_.calling && tx_state_ == TX_SILENT &&
		    rx_count_ - detect_last_ > 60)
			tx_set(TX_PROBE);
		break;
	case RX_TRAIN:
		rx_known(y, full_, true);
		if (rx_count_ > L_SYNC + 120)
			retrain("no segue");
		break;
	case RX_DETECT2:
		/* (the caller's own full-band sync meanwhile: its echo,
		   which the answerer's sync is looked for after) */
		if (rx_count_ == ECHO_AT - (L_SYNC + 2))
			measure_echo();
		if (rx_count_ >= ECHO_TAIL && rx_count_ % 2 == 0) {
			s64 before = win_;
			if (acquire(msg_rx_, 0, 0)) {
				logf("the answerer's sync found\n");
				rx_state_ = RX_TRAIN2;
				rx_count_ = 0;
				segs_ = 0;
			} else {
				win_ = before;
			}
		}
		if (rx_count_ > 600)
			retrain("no sync from the answerer");
		break;
	case RX_TRAIN2:
		rx_known(y, msg_rx_, false);
		if (rx_count_ > 2000)
			retrain("no segue to the messages");
		break;
	case RX_MSG:
		rx_message(y);
		if (rx_count_ > 3000)
			retrain("no plan");
		break;
	case RX_SYNC3:
		rx_known(y, rx_, false);
		if (rx_count_ >= L_SYNC3)
			start_data_rx();
		break;
	case RX_DATA:
		rx_data(y);
		break;
	}
}

/* our own full-band sync (sent from echo_tx0_ on) where we hear it:
   its delay, modulo a symbol */
void VwbModem::measure_echo()
{
	s64 keep = win_;
	if (echo_tx0_ >= 0 && acquire(full_, static_cast<s16>(kprobe_ - 2),
				      static_cast<s16>(kprobe_ + 2))) {
		s64 start = win_ - cp_ + adv_;
		echo_ = mod(start - echo_tx0_, s_);
		logf("echo: %lld samples (modulo a symbol)\n",
		     static_cast<long long>(echo_));
	} else {
		echo_ = cfg_.calling ? -1 : 0;
		logf("echo: not found\n");
	}
	win_ = keep;
}

/* the window that best holds a symbol of the known kind on l's tones
   (but those from mask_lo to mask_hi) in what the receiver has: win_
   set to the next such window, the channel's main path adv_ into it;
   whether a sync symbol is there at all */
bool VwbModem::acquire(const VwbLoading &l, s16 mask_lo, s16 mask_hi)
{
	s64 first = rx_next_ - n_ - s_;
	if (first < 0)
		return false;
	std::vector<cf> y(static_cast<size_t>(n_));
	/* the main path's peak as wide as the band is narrow */
	s32 tones = 0;
	for (s16 k = l.lo; k < l.hi; k++)
		if (l.code[static_cast<size_t>(k)])
			tones++;
	s32 wide = std::min(std::max(n_ / (2 * std::max(tones, 1)), 2), 32);
	float best_q = 0;
	s64 best_p = 0;
	s32 best_m = 0;
	for (s64 p = first; p <= rx_next_ - n_; p += 16) {
		window(p, y.data());
		for (s32 k = 0; k < n_; k++) {
			bool on = k < n_ / 2 && k >= l.lo && k < l.hi &&
				l.code[static_cast<size_t>(k)] &&
				!(k >= mask_lo && k <= mask_hi);
			y[static_cast<size_t>(k)] = on ?
				y[static_cast<size_t>(k)] *
				std::conj(xsync_[static_cast<size_t>(k)]) :
				cf(0, 0);
		}
		fft_.inverse(y.data());
		float tot = 0, mx = 0;
		s32 m = 0;
		for (s32 i = 0; i < n_; i++) {
			float e = std::norm(y[static_cast<size_t>(i)]);
			tot += e;
			if (e > mx) {
				mx = e;
				m = i;
			}
		}
		float pk = 0;
		for (s32 d = -wide; d <= wide; d++)
			pk += std::norm(y[static_cast<size_t>(mod(m + d, n_))]);
		float q = tot > 0 ? pk / tot : 0;
		if (q > best_q) {
			best_q = q;
			best_p = p;
			best_m = m;
		}
	}
	if (best_q < 0.3f)
		return false;
	s64 d = mod(best_m - adv_ + n_ / 2, n_) - n_ / 2;
	s64 w = best_p + d;
	while (w + n_ <= rx_next_)
		w += s_;
	while (w - s_ + n_ > rx_next_)
		w -= s_;
	win_ = w;
	return true;
}

/* the timing loop on the tones of l (its pilots alone, or every tone
   it uses, all with the sync symbol's points times sign): the slope of
   their phases against H X; y's phases then less that slope (and their
   mean) */
void VwbModem::track(cf *y, const VwbLoading &l, bool pilots_only,
		     float sign)
{
	double sw = 0, sk = 0, sp = 0, skk = 0, skp = 0;
	for (s16 k = l.lo; k < l.hi; k++) {
		u8 c = l.code[static_cast<size_t>(k)];
		if (!c || (pilots_only && c != VWB_PILOT))
			continue;
		cf hx = h_[static_cast<size_t>(k)] *
			xsync_[static_cast<size_t>(k)] *
			l.gain[static_cast<size_t>(k)] * sign;
		cf e = y[k] * std::conj(hx);
		double w = std::norm(hx);
		double ph = std::arg(e);
		sw += w;
		sk += w * k;
		sp += w * ph;
		skk += w * k * k;
		skp += w * k * ph;
	}
	if (sw <= 0)
		return;
	double den = sw * skk - sk * sk;
	if (den <= 0)
		return;
	double slope = (sw * skp - sk * sp) / den;
	double theta = (sp - slope * sk) / sw;
	double e = -slope * n_ / TWO_PI;
	e = std::min(std::max(e, -2.0), 2.0);
	/* known symbols on every tone: a fast loop; pilots alone (data):
	   a slow one, the clock found already (and its noise the less in
	   the window's rate, which our own echo would feel) */
	if (pilots_only)
		clk_.loop(static_cast<double>(rx_next_), e, s_, 0.05, 0.002);
	else
		clk_.loop(static_cast<double>(rx_next_), e, s_, 0.3, 0.05);
	/* this window's own lateness taken out of it: the decisions, the
	   channel's estimate and the SNR without the loop's lag */
	for (s32 k = 0; k < n_ / 2; k++)
		y[k] *= std::polar(1.0f, static_cast<float>(-(theta + slope * k)));
}

/* a window of known symbols on l's tones (the sync symbol, or its
   negative, the segue): the channel (the first 4 averaged, then
   followed), the timing, the SNR from SNR_FROM on when train, and two
   segues in a row: the next phase */
void VwbModem::rx_known(cf *y, const VwbLoading &l, bool train)
{
	auto x_at = [this, &l](s16 k) {
		return xsync_[static_cast<size_t>(k)] *
			l.gain[static_cast<size_t>(k)];
	};
	if (rx_count_ <= 4) {
		for (s16 k = l.lo; k < l.hi; k++)
			if (l.code[static_cast<size_t>(k)]) {
				cf v = y[k] / x_at(k);
				h_[static_cast<size_t>(k)] = rx_count_ == 1 ? v :
					h_[static_cast<size_t>(k)] +
					(v - h_[static_cast<size_t>(k)]) /
					static_cast<float>(rx_count_);
			}
		return;
	}
	float rho = known_corr(y, l);
	float sign = rho < 0 ? -1.0f : 1.0f;
	track(y, l, false, sign);
	for (s16 k = l.lo; k < l.hi; k++) {
		if (!l.code[static_cast<size_t>(k)])
			continue;
		cf v = sign * y[k] / x_at(k);
		cf &h = h_[static_cast<size_t>(k)];
		if (train && rx_count_ >= SNR_FROM && sign > 0) {
			sig_acc_[static_cast<size_t>(k)] += std::norm(h);
			noise_acc_[static_cast<size_t>(k)] +=
				std::norm(v - h);
		}
		h += (v - h) * 0.0625f;
	}
	if (train && rx_count_ >= SNR_FROM && sign > 0)
		acc_n_++;
	if (rho < -0.5f) {
		segs_++;
	} else {
		segs_ = 0;
		lost_ = rho < 0.3f ? lost_ + 1 : 0;
		if (lost_ > 16) {
			retrain("sync lost in the start-up");
			return;
		}
	}
	if (segs_ < 2)
		return;
	segs_ = 0;
	lost_ = 0;
	switch (rx_state_) {
	case RX_TRAIN: {
		double mean = 0;
		s32 m = 0;
		for (s16 k = klo_; k < khi_; k++) {
			double nz = noise_acc_[static_cast<size_t>(k)] + 1e-9;
			double s = sig_acc_[static_cast<size_t>(k)];
			float db = acc_n_ ?
				static_cast<float>(10 * std::log10(s / nz)) : 0;
			snr_rx_[static_cast<size_t>(k)] = db;
			mean += db;
			m++;
		}
		logf("segue: SNR measured over %d symbols, mean %.1f dB, "
		     "clock %+.1f ppm\n", static_cast<int>(acc_n_),
		     mean / std::max(m, 1), clock_ppm());
		if (cfg_.calling) {
			/* our turn: the full-band sync, then the default
			   band's idle symbols; the answerer's sync to find
			   on its band, on our clock (it follows ours) */
			tx_set(TX_SYNC, L_SYNC);
			tx_band_ = &full_;
			tx_after_seg_ = TX_IDLE;
			echo_tx0_ = -1;
			clk_.set_ppm(static_cast<double>(rx_next_), 0, s_);
			rx_state_ = RX_DETECT2;
			rx_count_ = -(L_SYNC + 2);
		} else {
			/* the caller's sync is over: on its clock, our
			   symbols lined up with its at our receiver */
			s64 far_start = win_ - cp_ + adv_;
			tx_grid_ = mod(far_start - std::max<s64>(echo_, 0), s_);
			align_ = true;
			tx_set(TX_SYNC, L_SYNC2);
			tx_band_ = &msg_tx_;
			tx_after_seg_ = TX_MSG;
			tx_msg_ = MSG_SNR;
			rx_state_ = RX_TRAIN2;
			rx_count_ = 100;
		}
		break;
	}
	case RX_TRAIN2:
		logf("segue: messages\n");
		rx_state_ = RX_MSG;
		rx_count_ = 0;
		frame_rx_sym_ = 0;
		frame_soft_.clear();
		break;
	default:
		break;
	}
}

/* Re sum y conj(H X) / sum |H X|^2 on l's tones (X at their gains):
   about 1 for the sync symbol, -1 for the segue */
float VwbModem::known_corr(const cf *y, const VwbLoading &l) const
{
	float c = 0, p = 0;
	for (s16 k = l.lo; k < l.hi; k++) {
		if (!l.code[static_cast<size_t>(k)])
			continue;
		cf hx = h_[static_cast<size_t>(k)] *
			xsync_[static_cast<size_t>(k)] *
			l.gain[static_cast<size_t>(k)];
		c += (y[k] * std::conj(hx)).real();
		p += std::norm(hx);
	}
	return p > 0 ? c / p : 0;
}

/* a window of the far end's frames: timing on the pilots, each bit's
   copies weighed by their tones' SNR, a frame's bits once its last
   symbol is in */
void VwbModem::rx_message(cf *y)
{
	const VwbLoading &l = msg_rx_;
	track(y, l, true, 1);
	float pc = 0, pp = 0;
	for (s16 k = l.lo; k < l.hi; k++)
		if (l.code[static_cast<size_t>(k)] == VWB_PILOT) {
			cf hx = h_[static_cast<size_t>(k)] *
				xsync_[static_cast<size_t>(k)];
			pc += (y[k] * std::conj(hx)).real();
			pp += std::norm(hx);
		}
	lost_ = pp > 0 && pc / pp < 0.3f ? lost_ + 1 : 0;
	if (lost_ >= 16) {
		retrain("pilots lost in the messages");
		return;
	}
	std::vector<float> slot;
	for (s16 k = l.lo; k < l.hi; k++) {
		u8 c = l.code[static_cast<size_t>(k)];
		cf &h = h_[static_cast<size_t>(k)];
		if (c == VWB_PILOT) {
			cf v = y[k] / xsync_[static_cast<size_t>(k)];
			h += (v - h) * 0.0625f;
			continue;
		}
		if (c != 2)
			continue;
		cf z = y[k] / h;
		float w = std::pow(10.0f, std::min(snr_rx_[static_cast<size_t>(k)],
						   40.0f) / 10);
		slot.push_back(z.real() * w);
		slot.push_back(z.imag() * w);
		/* the channel followed on the decisions */
		cf d(z.real() < 0 ? -0.70710678f : 0.70710678f,
		     z.imag() < 0 ? -0.70710678f : 0.70710678f);
		h += (y[k] / d - h) * 0.03f;
	}
	s32 per = static_cast<s32>(slot.size()) / rep_rx_;
	for (s32 i = 0; i < per; i++) {
		float s = 0;
		for (s16 r = 0; r < rep_rx_; r++)
			s += slot[static_cast<size_t>(i + r * per)];
		frame_soft_.push_back(s);
	}
	if (++frame_rx_sym_ < frame_syms_rx_)
		return;
	std::vector<u8> f((frame_soft_.size()) / 8, 0);
	for (size_t i = 0; i < f.size() * 8; i++)
		if ((frame_soft_[i] < 0) != (whiten_[i % whiten_.size()] != 0))
			f[i / 8] |= static_cast<u8>(1 << (i % 8));
	frame_soft_.clear();
	frame_rx_sym_ = 0;
	rx_frame(f);
	if (far_go_left_ > 0 && --far_go_left_ == 0) {
		logf("the far end switches to the plan\n");
		rx_state_ = RX_SYNC3;
		rx_count_ = 0;
		/* the answerer's symbols that much later from now on */
		if (cfg_.calling)
			win_ += shift_;
	}
}

/* a frame of the far end's, its CRC checked */
void VwbModem::rx_frame(const std::vector<u8> &f)
{
	s32 pay = frame_pay_rx_;
	if (static_cast<s32>(f.size()) < 3 + pay + 4)
		return;
	u32 crc = static_cast<u32>(f[3 + pay]) |
		static_cast<u32>(f[4 + pay]) << 8 |
		static_cast<u32>(f[5 + pay]) << 16 |
		static_cast<u32>(f[6 + pay]) << 24;
	if (crc32(f.data(), static_cast<size_t>(3 + pay)) != crc) {
		logf("frame: bad CRC\n");
		return;
	}
	u8 type = f[0];
	s32 len = f[1] | f[2] << 8;
	if (len > pay)
		return;
	std::vector<u8> p(f.begin() + 3, f.begin() + 3 + len);
	switch (type) {
	case MSG_SNR:
		if (!cfg_.calling || plan_ok_ || len != khi_ - klo_)
			break;
		for (s16 k = klo_; k < khi_; k++)
			snr_far_[static_cast<size_t>(k)] =
				p[static_cast<size_t>(k - klo_)] / 2.0f;
		make_plan();
		plan_ok_ = true;
		break;
	case MSG_PLAN:
		if (cfg_.calling || plan_ok_)
			break;
		if (plan_from(p)) {
			plan_ok_ = true;
			go_left_ = 2;
		}
		break;
	case MSG_GO:
		if (len < 1 || !plan_ok_ || far_go_left_)
			break;
		logf("GO %d from the far end\n", p[0]);
		far_go_left_ = p[0];
		if (cfg_.calling && !go_left_)
			go_left_ = 2;
		break;
	default:
		break;
	}
}

/* to data on the plan's band: the receiver's side */
void VwbModem::start_data_rx()
{
	rx_state_ = RX_DATA;
	rx_count_ = 0;
	rx_data_ = true;
	depth_rx_ = depth_for(rx_);
	rx_octets_.clear();
	rx_acc_ = 0;
	rx_acc_n_ = 0;
	dscr_ = 0;
	std::fill(cw_hist_.begin(), cw_hist_.end(), 0);
	double s = 0;
	s32 m = 0;
	for (s16 k = rx_.lo; k < rx_.hi; k++)
		if (rx_.code[static_cast<size_t>(k)] &&
		    rx_.code[static_cast<size_t>(k)] != VWB_PILOT) {
			s += snr_rx_[static_cast<size_t>(k)];
			m++;
		}
	snr_db_ = m ? static_cast<float>(s / m) : 0;
	logf("receiving data: %d bit/s, interleaver %d; clock %+.1f ppm\n",
	     static_cast<int>(rx_rate()), depth_rx_, clock_ppm());
	if (tx_data_)
		state_ = DATA;
}

/* to data: the transmitter's side */
void VwbModem::start_data_tx()
{
	tx_data_ = true;
	depth_tx_ = depth_for(tx_);
	tx_bits_.clear();
	tx_bit_pos_ = 0;
	scr_ = 0;
	logf("sending data: %d bit/s, interleaver %d\n",
	     static_cast<int>(tx_rate()), depth_tx_);
	if (rx_data_)
		state_ = DATA;
}

/* a data window: timing on the pilots, each tone sliced (the channel
   followed on the decisions), the bits to octets, codewords, the
   core's octets; a loss of sync (pilots, codewords) retrains */
void VwbModem::rx_data(cf *y)
{
	const VwbLoading &l = rx_;
	track(y, l, true, 1);
	float pc = 0, pp = 0;
	for (s16 k = l.lo; k < l.hi; k++) {
		u8 c = l.code[static_cast<size_t>(k)];
		if (!c)
			continue;
		cf &h = h_[static_cast<size_t>(k)];
		float g = l.gain[static_cast<size_t>(k)];
		if (c == VWB_PILOT) {
			cf hx = h * xsync_[static_cast<size_t>(k)] * g;
			pc += (y[k] * std::conj(hx)).real();
			pp += std::norm(hx);
			cf v = y[k] / (xsync_[static_cast<size_t>(k)] * g);
			h += (v - h) * 0.03f;
			continue;
		}
		cf z = y[k] / (h * g);
		u32 bits = VwbQam::slice(c, z);
		cf d = VwbQam::map(c, bits);
		if (std::norm(d) > 0.04f)
			h += (y[k] / (d * g) - h) * 0.01f;
		float &ev = err_[static_cast<size_t>(k)];
		ev = rx_count_ == 1 ? std::norm(z - d) :
			ev + (std::norm(z - d) - ev) * 0.01f;
		for (s16 i = 0; i < c; i++) {
			rx_acc_ |= ((bits >> i) & 1u) << rx_acc_n_;
			if (++rx_acc_n_ == 8) {
				rx_octets_.push_back(static_cast<u8>(rx_acc_));
				rx_acc_ = 0;
				rx_acc_n_ = 0;
			}
		}
	}
	if (rx_count_ % 1000 == 200) {
		/* the tones' SNR in data, from the decisions */
		double sum = 0;
		s32 m = 0, worst = 0;
		float wdb = 100;
		for (s16 k = l.lo; k < l.hi; k++) {
			u8 c = l.code[static_cast<size_t>(k)];
			if (!c || c == VWB_PILOT)
				continue;
			float db = -10 * std::log10(err_[static_cast<size_t>(k)] +
						    1e-9f);
			sum += db;
			m++;
			if (db < wdb) {
				wdb = db;
				worst = k;
			}
		}
		snr_db_ = m ? static_cast<float>(sum / m) : 0;
		logf("data: SNR %.1f dB, the worst tone %d at %.1f dB "
		     "(needs %.1f); codewords %lld, %lld corrected, %lld "
		     "failed\n", snr_db_, static_cast<int>(worst), wdb,
		     10 * std::log10(VwbQam::snr_needed(
			     l.code[static_cast<size_t>(worst)])),
		     static_cast<long long>(cw_total_),
		     static_cast<long long>(cw_fixed_),
		     static_cast<long long>(cw_failed_));
	}
	float rho = pp > 0 ? pc / pp : 0;
	lost_ = rho < 0.3f ? lost_ + 1 : 0;
	if (lost_ >= 8) {
		retrain("pilots lost");
		return;
	}
	size_t block = static_cast<size_t>(depth_rx_) * RS_N;
	while (rx_octets_.size() >= block) {
		s32 fails = 0;
		for (s16 j = 0; j < depth_rx_; j++) {
			u8 cw[RS_N];
			for (s16 i = 0; i < RS_N; i++)
				cw[i] = rx_octets_[static_cast<size_t>(
					i * depth_rx_ + j)];
			s16 r = rs_.decode(cw);
			cw_total_++;
			if (r < 0) {
				cw_failed_++;
				fails++;
			} else if (r > 0) {
				cw_fixed_++;
			}
			cw_hist_[cw_hist_pos_++ % cw_hist_.size()] = r < 0;
			u8 out[RS_K];
			for (s16 i = 0; i < RS_K; i++) {
				u8 o = 0;
				for (s16 b = 0; b < 8; b++)
					o |= static_cast<u8>(descramble(&dscr_,
						SCR_GPC, (cw[i] >> b) & 1) << b);
				out[i] = o;
			}
			if (sink_)
				sink_(out, RS_K);
		}
		rx_octets_.erase(rx_octets_.begin(),
				 rx_octets_.begin() + static_cast<long>(block));
		s32 bad = 0;
		for (s8 f : cw_hist_)
			bad += f;
		if (fails && bad >= 16) {
			retrain("codewords failing");
			return;
		}
	}
}

/*
 *    the plan
 */

/* the caller's plan from both ways' SNR: the split for the same rate
   both ways, each tone's bits and gain, 8 pilots a band */
void VwbModem::make_plan()
{
	const float margin = cfg_.margin_db;
	auto bits_for = [this, margin](float db) {
		s16 b = 0;
		for (s16 c = 1; c <= cfg_.max_bits; c++)
			if (db >= 10 * std::log10(VwbQam::snr_needed(c)) + margin)
				b = c;
		return b;
	};
	/* the lower band: ours to the answerer (its SNR, snr_far_); the
	   upper: its to us (ours, snr_rx_) */
	std::vector<s32> lo_sum(static_cast<size_t>(khi_ + 1), 0),
		up_sum(static_cast<size_t>(khi_ + 1), 0);
	for (s16 k = klo_; k < khi_; k++) {
		lo_sum[static_cast<size_t>(k + 1)] = lo_sum[static_cast<size_t>(k)] +
			bits_for(snr_far_[static_cast<size_t>(k)]);
		up_sum[static_cast<size_t>(k + 1)] = up_sum[static_cast<size_t>(k)] +
			bits_for(snr_rx_[static_cast<size_t>(k)]);
	}
	s16 best = static_cast<s16>((klo_ + khi_) / 2);
	s32 best_min = -1;
	for (s16 s = static_cast<s16>(klo_ + edge_ + cfg_.guard);
	     s <= khi_ - edge_; s++) {
		s32 lo = lo_sum[static_cast<size_t>(s - cfg_.guard)] -
			lo_sum[static_cast<size_t>(klo_)];
		s32 up = up_sum[static_cast<size_t>(khi_)] -
			up_sum[static_cast<size_t>(s)];
		if (std::min(lo, up) > best_min) {
			best_min = std::min(lo, up);
			best = s;
		}
	}
	split_ = best;
	/* our echo and the answerer's symbols apart by twice the pair's
	   delay (it lined its own up at its receiver): its symbols moved by
	   half of that, so that each side's echo comes as much before the
	   far end's symbols */
	shift_ = 0;
	if (echo_ >= 0) {
		s64 far = mod(win_ - cp_ + adv_, s_);
		s64 o = mod(far - echo_ + s_ / 2, s_) - s_ / 2;
		shift_ = static_cast<s32>(mod(-(o / 2), s_));
		logf("our echo %lld before the answerer's symbols: they move by "
		     "%d\n", static_cast<long long>(o), static_cast<int>(shift_));
	}
	auto fill = [&](VwbLoading &l, s16 lo, s16 hi,
			const std::vector<float> &snr) {
		l.band(n_, lo, hi, 0, 0);
		for (s16 k = lo; k < hi; k++)
			l.code[static_cast<size_t>(k)] = static_cast<u8>(
				bits_for(snr[static_cast<size_t>(k)]));
		/* 8 pilots through the band, each on a good tone near
		   its place */
		for (s16 i = 0; i < 8; i++) {
			s32 at = lo + ((2 * i + 1) * (hi - lo)) / 16;
			s32 pick = at;
			float bs = -100;
			for (s32 k = std::max<s32>(lo, at - 4);
			     k <= std::min<s32>(hi - 1, at + 4); k++)
				if (snr[static_cast<size_t>(k)] > bs &&
				    l.code[static_cast<size_t>(k)] != VWB_PILOT) {
					bs = snr[static_cast<size_t>(k)];
					pick = k;
				}
			l.code[static_cast<size_t>(pick)] = VWB_PILOT;
		}
		/* fine gains: each tone at the same margin, within
		   max_gain_db, the power of the tones in use kept */
		double sum = 0;
		s32 used = 0;
		for (s16 k = lo; k < hi; k++) {
			u8 c = l.code[static_cast<size_t>(k)];
			float g = 0;
			if (c == VWB_PILOT) {
				g = 1;
			} else if (c) {
				float need = VwbQam::snr_needed(c) *
					std::pow(10.0f, margin / 10);
				float have = std::pow(10.0f,
					snr[static_cast<size_t>(k)] / 10);
				float gdb = 10 * std::log10(need / have);
				gdb = std::min(std::max(gdb, -cfg_.max_gain_db),
					       cfg_.max_gain_db);
				g = std::pow(10.0f, gdb / 20);
			}
			l.gain[static_cast<size_t>(k)] = g;
			if (c) {
				sum += g * g;
				used++;
			}
		}
		float norm = used ? static_cast<float>(std::sqrt(used / sum)) : 1;
		for (s16 k = lo; k < hi; k++) {
			float &g = l.gain[static_cast<size_t>(k)];
			if (!g)
				continue;
			float db = 20 * std::log10(g * norm);
			db = std::min(std::max(db, -cfg_.max_gain_db),
				      cfg_.max_gain_db);
			/* (as the plan's octets carry it: 0.1 dB) */
			g = std::pow(10.0f, std::round(db * 10) / 200);
		}
		l.count();
	};
	fill(tx_, klo_, static_cast<s16>(split_ - cfg_.guard), snr_far_);
	fill(rx_, split_, khi_, snr_rx_);
	logf("plan: split at tone %d; sending %s; receiving %s\n", split_,
	     tx_.str().c_str(), rx_.str().c_str());
	plan_.clear();
	plan_bytes(plan_);
}

/* the plan's octets (the caller's): the split, the guard, the
   answerer's shift (2 octets), then for each tone of the band the lower band's (caller to answerer) or the
   upper's code, then their gains (128 + 0.1 dB steps) */
void VwbModem::plan_bytes(std::vector<u8> &b) const
{
	b.push_back(static_cast<u8>(split_ - klo_));
	b.push_back(static_cast<u8>(cfg_.guard));
	b.push_back(static_cast<u8>(shift_));
	b.push_back(static_cast<u8>(shift_ >> 8));
	const VwbLoading &lo = cfg_.calling ? tx_ : rx_;
	const VwbLoading &up = cfg_.calling ? rx_ : tx_;
	for (s16 k = klo_; k < khi_; k++)
		b.push_back(k < split_ ? lo.code[static_cast<size_t>(k)] :
			    up.code[static_cast<size_t>(k)]);
	for (s16 k = klo_; k < khi_; k++) {
		float g = k < split_ ? lo.gain[static_cast<size_t>(k)] :
			up.gain[static_cast<size_t>(k)];
		s32 v = g > 0 ? static_cast<s32>(std::lround(
			200 * std::log10(g))) + 128 : 0;
		b.push_back(static_cast<u8>(std::min(std::max(v, 1), 255) *
					    (g > 0)));
	}
}

/* the answerer's side of the plan from its octets: false if they make
   no sense */
bool VwbModem::plan_from(const std::vector<u8> &b)
{
	s32 nt = khi_ - klo_;
	if (static_cast<s32>(b.size()) != 4 + 2 * nt)
		return false;
	s32 shift = b[2] | b[3] << 8;
	if (shift >= s_)
		return false;
	s16 split = static_cast<s16>(klo_ + b[0]), guard = b[1];
	if (split < klo_ + edge_ + guard || split > khi_ - edge_)
		return false;
	VwbLoading lo, up;
	lo.band(n_, klo_, static_cast<s16>(split - guard), 0, 0);
	up.band(n_, split, khi_, 0, 0);
	for (s16 k = klo_; k < khi_; k++) {
		u8 c = b[static_cast<size_t>(4 + k - klo_)];
		u8 gv = b[static_cast<size_t>(4 + nt + k - klo_)];
		if ((c > VWB_MAX_BITS && c != VWB_PILOT))
			return false;
		VwbLoading &l = k < split ? lo : up;
		if (k >= l.hi)
			continue;
		l.code[static_cast<size_t>(k)] = c;
		l.gain[static_cast<size_t>(k)] = gv ?
			std::pow(10.0f, (gv - 128) / 200.0f) : 0;
	}
	lo.count();
	up.count();
	split_ = split;
	shift_ = shift;
	rx_ = lo;
	tx_ = up;
	logf("plan received: split at tone %d; sending %s; receiving %s\n",
	     split_, tx_.str().c_str(), rx_.str().c_str());
	return true;
}

/* the interleaver's codewords for loading l: as many as 30 ms of the
   line hold (1 to 8).  Deeper would spread a burst over more of them,
   but the delay each way it adds slows V.42 (its window of 15 frames
   of 128 octets waits a round trip for its acknowledgement) */
s16 VwbModem::depth_for(const VwbLoading &l) const
{
	s32 d = static_cast<s32>(0.03 * rate_of(l) * RS_N / RS_K /
				 (8.0 * RS_N));
	return static_cast<s16>(std::min(std::max(d, 1), 8));
}

/* bit/s of octets on loading l: its bits a symbol less RS's checks */
s32 VwbModem::rate_of(const VwbLoading &l) const
{
	return static_cast<s32>(static_cast<double>(l.bits) * RS_K / RS_N *
				cfg_.fs / s_);
}

/*
 *    the transmitter
 */

/* a symbol of x on l's tones (at their gains), with its prefix, to the
   transmitter's samples */
void VwbModem::tx_tones(const cf *x, const VwbLoading &l)
{
	std::vector<cf> v(static_cast<size_t>(n_), cf(0, 0));
	for (s16 k = l.lo; k < l.hi; k++) {
		if (!l.code[static_cast<size_t>(k)])
			continue;
		cf a = x[k] * (scale_ * l.gain[static_cast<size_t>(k)]);
		v[static_cast<size_t>(k)] = a;
		v[static_cast<size_t>(n_ - k)] = std::conj(a);
	}
	fft_.inverse(v.data());
	for (s32 i = n_ - cp_; i < n_; i++)
		tx_hist_.push(v[static_cast<size_t>(i)].real());
	for (s32 i = 0; i < n_; i++)
		tx_hist_.push(v[static_cast<size_t>(i)].real());
	tx_next_ += s_;
}

/* the next symbol of the transmitter's state */
void VwbModem::tx_symbol()
{
	if (align_) {
		/* the answerer's first symbol on the far clock: lined up */
		align_ = false;
		while (mod(tx_next_ - tx_grid_, s_)) {
			tx_hist_.push(0);
			tx_next_++;
		}
	}
	for (; pad_ > 0; pad_--) {
		/* the answerer's symbols moved by the plan's shift */
		tx_hist_.push(0);
		tx_next_++;
	}
	std::vector<cf> x(static_cast<size_t>(n_ / 2), cf(0, 0));
	tx_count_++;
	switch (tx_state_) {
	case TX_SILENT:
		for (s32 i = 0; i < s_; i++)
			tx_hist_.push(0);
		tx_next_ += s_;
		break;
	case TX_PROBE: {
		double w = TWO_PI * kprobe_ / static_cast<double>(n_);
		for (s32 i = 0; i < s_; i++) {
			tx_hist_.push(static_cast<float>(8000 *
							 std::cos(probe_phase_)));
			probe_phase_ = std::fmod(probe_phase_ + w, TWO_PI);
		}
		tx_next_ += s_;
		break;
	}
	case TX_SYNC:
		if (tx_band_ == &full_ && echo_tx0_ < 0)
			echo_tx0_ = tx_next_;
		tx_tones(xsync_.data(), *tx_band_);
		if (--tx_left_ <= 0)
			tx_set(TX_SEG, 2);
		break;
	case TX_SEG:
		for (s32 k = 0; k < n_ / 2; k++)
			x[static_cast<size_t>(k)] = -xsync_[static_cast<size_t>(k)];
		tx_tones(x.data(), *tx_band_);
		if (--tx_left_ <= 0) {
			tx_set(tx_after_seg_);
			frame_pos_ = 0;
			frame_out_.clear();
		}
		break;
	case TX_IDLE:
		tx_tones(xsync_.data(), msg_tx_);
		if (plan_ok_ && cfg_.calling) {
			tx_band_ = &msg_tx_;
			tx_after_seg_ = TX_MSG;
			tx_msg_ = MSG_PLAN;
			tx_set(TX_SEG, 2);
		}
		break;
	case TX_MSG:
		tx_frame_bits();
		break;
	case TX_SYNC3:
		tx_tones(xsync_.data(), tx_);
		if (--tx_left_ <= 0) {
			tx_set(TX_DATA);
			start_data_tx();
		}
		break;
	case TX_DATA:
		tx_data_symbol();
		break;
	}
}

/* a symbol of the message channel: the frame's bits (a new frame at
   its start: our GOs first, else the SNR or the plan), each on rep_tx_
   tones, QPSK; the pilots the sync symbol's */
void VwbModem::tx_frame_bits()
{
	const VwbLoading &l = msg_tx_;
	if (frame_out_.empty() || frame_pos_ >= frame_syms_tx_) {
		if (!frame_out_.empty() && go_left_ == 0 && sent_go_) {
			/* our last GO is out: the plan's band */
			sent_go_ = false;
			tx_set(TX_SYNC3, L_SYNC3);
			if (!cfg_.calling)
				pad_ = shift_;
			return;
		}
		std::vector<u8> pay;
		u8 type = tx_msg_;
		if (go_left_ > 0) {
			type = MSG_GO;
			pay.push_back(static_cast<u8>(go_left_));
			go_left_--;
			sent_go_ = true;
		} else if (type == MSG_SNR) {
			for (s16 k = klo_; k < khi_; k++)
				pay.push_back(static_cast<u8>(std::min(255.0f,
					std::max(0.0f, 2 * snr_rx_[static_cast<size_t>(k)]))));
		} else if (type == MSG_PLAN) {
			pay = plan_;
		}
		std::vector<u8> f(static_cast<size_t>(3 + frame_pay_tx_ + 4), 0);
		f[0] = type;
		f[1] = static_cast<u8>(pay.size());
		f[2] = static_cast<u8>(pay.size() >> 8);
		std::copy(pay.begin(), pay.end(), f.begin() + 3);
		u32 crc = crc32(f.data(), static_cast<size_t>(3 + frame_pay_tx_));
		for (s16 i = 0; i < 4; i++)
			f[static_cast<size_t>(3 + frame_pay_tx_ + i)] =
				static_cast<u8>(crc >> (8 * i));
		frame_out_.clear();
		for (size_t i = 0; i < f.size() * 8; i++)
			frame_out_.push_back((f[i / 8] >> (i % 8)) & 1);
		frame_pos_ = 0;
	}
	s32 nslots = 0;
	for (s16 k = l.lo; k < l.hi; k++)
		if (l.code[static_cast<size_t>(k)] == 2)
			nslots += 2;
	s32 per = nslots / rep_tx_;
	std::vector<s8> slot(static_cast<size_t>(nslots), 0);
	for (s32 i = 0; i < per; i++) {
		size_t at = static_cast<size_t>(frame_pos_ * per + i);
		/* (whitened: a frame of zeros is no train of equal
		   symbols, whose peaks would clip) */
		u8 bit = (at < frame_out_.size() ? frame_out_[at] : 0) ^
			whiten_[at % whiten_.size()];
		for (s16 r = 0; r < rep_tx_; r++)
			slot[static_cast<size_t>(i + r * per)] =
				static_cast<s8>(bit);
	}
	std::vector<cf> x(static_cast<size_t>(n_ / 2), cf(0, 0));
	s32 j = 0;
	for (s16 k = l.lo; k < l.hi; k++) {
		u8 c = l.code[static_cast<size_t>(k)];
		if (c == VWB_PILOT) {
			x[static_cast<size_t>(k)] = xsync_[static_cast<size_t>(k)];
		} else if (c == 2) {
			x[static_cast<size_t>(k)] = cf(
				slot[static_cast<size_t>(j)] ? -0.70710678f : 0.70710678f,
				slot[static_cast<size_t>(j + 1)] ? -0.70710678f : 0.70710678f);
			j += 2;
		}
	}
	tx_tones(x.data(), l);
	frame_pos_++;
}

/* a data symbol: the bits of the interleaved codewords (the core's
   octets scrambled, RS coded) on the tones by their loading */
void VwbModem::tx_data_symbol()
{
	const VwbLoading &l = tx_;
	while (tx_bits_.size() - tx_bit_pos_ < static_cast<size_t>(l.bits)) {
		if (tx_bit_pos_ > 65536) {
			tx_bits_.erase(tx_bits_.begin(),
				       tx_bits_.begin() +
				       static_cast<long>(tx_bit_pos_));
			tx_bit_pos_ = 0;
		}
		std::vector<u8> blk(static_cast<size_t>(depth_tx_ * RS_N));
		for (s16 j = 0; j < depth_tx_; j++) {
			u8 cw[RS_N];
			for (s16 i = 0; i < RS_K; i++) {
				u8 o = src_ ? src_() : 0xff, s = 0;
				for (s16 b = 0; b < 8; b++)
					s |= static_cast<u8>(scramble(&scr_,
						SCR_GPC, (o >> b) & 1) << b);
				cw[i] = s;
			}
			rs_.encode(cw, cw);
			for (s16 i = 0; i < RS_N; i++)
				blk[static_cast<size_t>(i * depth_tx_ + j)] = cw[i];
		}
		for (u8 o : blk)
			for (s16 b = 0; b < 8; b++)
				tx_bits_.push_back((o >> b) & 1);
	}
	std::vector<cf> x(static_cast<size_t>(n_ / 2), cf(0, 0));
	for (s16 k = l.lo; k < l.hi; k++) {
		u8 c = l.code[static_cast<size_t>(k)];
		if (!c)
			continue;
		if (c == VWB_PILOT) {
			x[static_cast<size_t>(k)] = xsync_[static_cast<size_t>(k)];
			continue;
		}
		u32 bits = 0;
		for (s16 i = 0; i < c; i++)
			bits |= static_cast<u32>(tx_bits_[tx_bit_pos_++]) << i;
		x[static_cast<size_t>(k)] = VwbQam::map(c, bits);
	}
	tx_tones(x.data(), l);
}

} // namespace dsp
