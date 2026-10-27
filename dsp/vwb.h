/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vwb.h - V.wb, D-Modem's own wideband mode for a bare copper pair
 *            (pico-serial's WB stick, SPEC-WB): discrete multitone
 *            (DMT, OFDM) in frequency-division duplex, sample by sample
 *            at 48000, 96000 or 192000 Hz (docs/dsp/vwb.md)
 *
 *    Tones 187.5 Hz apart (an FFT of 512 at 96000 Hz, 256 at 48000,
 *    1024 at 192000) with a cyclic prefix of 40/512 of the FFT, about
 *    174 symbols a second; the pair's band from tone 21 (3.9 kHz, the
 *    WB stick's transformer) to tone 192 (36 kHz, its ADC's passband at
 *    96 kHz), less at 48000 Hz.  The caller sends on the lower part,
 *    the answerer on the upper, a guard of a few tones between; the
 *    split is chosen at training so both ways get about the same rate.
 *
 *    The start-up: the caller's probe tone; the answerer's sync symbols
 *    on the whole band, then the caller's (one at a time: each measures
 *    the other's SNR on every tone, the answerer its own echo's delay);
 *    then both at once on the default split, the answerer's tables of
 *    SNR to the caller, the caller's plan (split, bits and gains of
 *    each tone, pilots) to the answerer, each side's GO, its sync
 *    symbols on the plan's band and data.  The answerer is loop timed:
 *    it takes the caller's sample clock for its own transmitter too
 *    (both resampled by the clock offset its receiver tracks) and lines
 *    its symbols up with the caller's at its own receiver, so its echo
 *    stays orthogonal to what it receives; at the caller the two are
 *    apart by twice the pair's delay, inside the cyclic prefix.
 *
 *    Data: the octets through the self-synchronising scrambler of
 *    V.34 (bits.h), RS(255,239) (rs.h), a block interleaver of up to 8
 *    codewords, bits on the tones by their loading (0 to 12, QAM, Gray
 *    coded on each axis).  Pilot tones keep the receiver's timing (and
 *    the clock offset, ±100 ppm and more); a loss of sync (the pilots,
 *    or the codewords failing) starts the start-up again.
 *
 */

#ifndef DSP_VWB_H
#define DSP_VWB_H

#include <cmath>
#include <complex>
#include <functional>
#include <string>
#include <vector>

#include "dm_types.h"
#include "rs.h"

namespace dsp {

using cf = std::complex<float>;

/* a complex FFT of a power of two, radix 2 */
class VwbFft {
public:
	/* for n points */
	void init(s32 n);
	/* x in place: X[k] = sum x[n] e^{-2 pi i k n / N} */
	void forward(cf *x) const { run(x, false); }
	/* x in place: x[n] = sum X[k] e^{+2 pi i k n / N} (not divided by
	   N) */
	void inverse(cf *x) const { run(x, true); }
	/* (see init()) */
	s32 size() const { return n_; }

private:
	void run(cf *x, bool inv) const;

	s32 n_ = 0;
	std::vector<cf> tw_;
	std::vector<s32> rev_;
};

/* band-limited interpolation of a sampled signal at any instant: a
   Kaiser windowed sinc of 32 taps, 256 phases (linear between them),
   passing up to 0.42 of the rate */
class VwbInterp {
public:
	enum { HALF = 16, PHASES = 256 };
	/* the table */
	VwbInterp();
	/* the signal at t, x(i) its sample i (from floor(t) - HALF + 1 to
	   floor(t) + HALF) */
	template <class X>
	float at(const X &x, double t) const
	{
		double fl = std::floor(t);
		s64 i0 = static_cast<s64>(fl);
		double ph = (t - fl) * PHASES;
		s32 p = static_cast<s32>(ph);
		float a = static_cast<float>(ph - p);
		const float *h0 = &tab_[p * 2 * HALF];
		const float *h1 = h0 + 2 * HALF;
		float y = 0;
		for (s32 j = 0; j < 2 * HALF; j++) {
			float h = h0[j] + a * (h1[j] - h0[j]);
			y += h * x(i0 - HALF + 1 + j);
		}
		return y;
	}

private:
	std::vector<float> tab_;   /* PHASES + 1 rows of 2 HALF taps */
};

/* the last of a stream of samples by their index from the start */
class VwbHistory {
public:
	/* room for n (a power of two is used) */
	void init(s32 n);
	/* the next sample */
	void push(float x) { buf_[static_cast<size_t>(end_++ & mask_)] = x; }
	/* sample i (end() - size() <= i < end(); 0 before the first) */
	float operator()(s64 i) const
	{
		return i < 0 || i < end_ - static_cast<s64>(buf_.size()) ||
				       i >= end_ ? 0.0f :
			buf_[static_cast<size_t>(i & mask_)];
	}
	/* the index of the next sample */
	s64 end() const { return end_; }

private:
	std::vector<float> buf_;
	s64 end_ = 0;
	s64 mask_ = 0;
};

/* the time of the far end's clock in our samples: line time t of
   virtual (far clock) sample v is t0 + (v - v0) r */
struct VwbClock {
	double t0 = 0, v0 = 0, r = 1;
	double freq = 0;           /* the loop's estimate: samples a symbol */

	/* line time of virtual time v */
	double line(double v) const { return t0 + (v - v0) * r; }
	/* virtual time of line time t */
	double virt(double t) const { return v0 + (t - t0) / r; }
	/* a step of the timing loop at virtual time v: e the far symbols'
	   lateness (virtual samples), s the symbol's length; alpha of e
	   corrected over the next symbol, beta of it into the frequency */
	void loop(double v, double e, s32 s, double alpha, double beta);
	/* r set to 1 + ppm / 1e6 from virtual time v on, the loop's
	   frequency with it */
	void set_ppm(double v, double ppm, s32 s);
};

/* a tone's QAM of b bits (1 to 12): ceil(b/2) on I, floor(b/2) on Q,
   each axis Gray coded, unit power */
struct VwbQam {
	/* the point of bits (the first in bit 0: I's, then Q's) */
	static cf map(s16 b, u32 bits);
	/* the bits of the point nearest z */
	static u32 slice(s16 b, cf z);
	/* the point bits slice() gives back for z */
	static cf decided(s16 b, cf z) { return map(b, slice(b, z)); }
	/* the SNR (ratio) the constellation needs: (d/2) q sigma apart */
	static float snr_needed(s16 b, float q = 4.75f);
};

/* how the tones of one way are used: per tone 0 unused, 1 to 12 its
   bits, VWB_PILOT a pilot; per tone its gain */
enum { VWB_PILOT = 15, VWB_MAX_BITS = 12 };
struct VwbLoading {
	std::vector<u8> code;       /* by tone, 0 .. N/2 */
	std::vector<float> gain;    /* linear, by tone */
	s32 bits = 0;               /* a symbol's data bits */
	s16 lo = 0, hi = 0;         /* the band's tones [lo, hi) */

	/* a band [lo, hi) of n tones with npilots pilots evenly through it
	   and every other tone at b bits (2: the message channel), gain 1 */
	void band(s32 n, s16 lo_, s16 hi_, s16 b, s16 npilots = 8);
	/* the bits counted */
	void count();
	/* "b2 x 80, b12 x 10" and the like */
	std::string str() const;
};

class VwbModem {
public:
	/* the octet to send next (0xff when there is no data) */
	using Source = std::function<u8()>;
	/* n octets received */
	using Sink = std::function<void(const u8 *, s32)>;

	struct Config {
		s32 fs = 96000;
		bool calling = true;
		s16 max_bits = VWB_MAX_BITS;    /* a tone's at most */
		float margin_db = 4;            /* over what a tone's bits need */
		s16 guard = 4;                  /* tones between the bands */
		float max_gain_db = 2.5f;       /* the fine gains' range */
		s16 lo = 0, hi = 0;             /* the band, 0: by fs */
		s32 timeout_s = 60;             /* no data so long: FAILED */
	};
	enum State { STARTUP, DATA, FAILED };

	/* for c, its data from src, what it receives to sink: 0, or -1 for
	   a rate it cannot run at (under 48000 Hz) */
	s16 init(const Config &c, Source src, Sink sink);
	/* n line samples in, n out */
	void process(const s16 *in, s16 *out, s32 n);

	/* the start-up, data both ways, or given up */
	State state() const { return state_; }
	/* data both ways (CONNECT) */
	bool connected() const { return state_ == DATA; }
	/* the data rates, bit/s of octets: ours and the far end's */
	s32 tx_rate() const { return rate_of(tx_); }
	/* (see tx_rate()) */
	s32 rx_rate() const { return rate_of(rx_); }
	/* the plan's tones: ours to send on, the far end's */
	const VwbLoading &tx_loading() const { return tx_; }
	/* (see tx_loading()) */
	const VwbLoading &rx_loading() const { return rx_; }
	/* the start-ups after the first */
	s32 retrains() const { return retrains_; }
	/* the codewords received: all, corrected, failed */
	s64 cw_total() const { return cw_total_; }
	/* (see cw_total()) */
	s64 cw_corrected() const { return cw_fixed_; }
	/* (see cw_total()) */
	s64 cw_failed() const { return cw_failed_; }
	/* the receiver's clock: far samples per ours, less 1, in ppm */
	double clock_ppm() const { return (clk_.r - 1) * 1e6; }
	/* the mean SNR of the tones received in data (dB) */
	float snr_db() const { return snr_db_; }
	/* why FAILED */
	const char *why() const { return why_; }
	/* the log lines since the last call */
	std::string take_log();
	/* the FFT's size */
	s32 fft_size() const { return n_; }
	/* a symbol's samples, its cyclic prefix with them */
	s32 symbol_len() const { return s_; }

private:
	enum Tx {
		TX_SILENT, TX_PROBE, TX_SYNC, TX_SEG, TX_IDLE, TX_MSG,
		TX_SYNC3, TX_DATA,
	};
	enum Rx {
		RX_PROBE_WAIT,    /* answerer: the caller's probe */
		RX_ECHO,          /* answerer: its own sync's echo */
		RX_DETECT,        /* the far end's full-band sync */
		RX_TRAIN,         /* its sync: timing, H, SNR, the segue */
		RX_DETECT2,       /* caller: the answerer's sync on its band */
		RX_TRAIN2,        /* its sync on the default band */
		RX_MSG,           /* frames */
		RX_SYNC3,         /* sync on the plan's band */
		RX_DATA,
	};
	enum MsgType { MSG_IDLE = 0, MSG_SNR = 1, MSG_PLAN = 2, MSG_GO = 3 };

	/* the receiver */
	void rx_symbol(cf *y);
	void rx_known(cf *y, const VwbLoading &l, bool train);
	void rx_message(cf *y);
	void rx_frame(const std::vector<u8> &f);
	void rx_data(cf *y);
	bool acquire(const VwbLoading &l, s16 mask_lo, s16 mask_hi);
	void window(s64 at, cf *y) const;
	void track(cf *y, const VwbLoading &l, bool pilots_only,
		   float sign);
	float known_corr(const cf *y, const VwbLoading &l) const;
	void measure_echo();

	/* the transmitter */
	void tx_symbol();
	void tx_tones(const cf *x, const VwbLoading &l);
	void tx_frame_bits();
	void tx_data_symbol();
	void tx_set(Tx t, s32 n = 0);

	/* the start-up's decisions */
	void make_plan();
	void plan_bytes(std::vector<u8> &b) const;
	bool plan_from(const std::vector<u8> &b);
	void start_data_rx();
	void start_data_tx();
	void retrain(const char *why);
	void restart();
	s32 rate_of(const VwbLoading &l) const;
	void logf(const char *fmt, ...)
#ifdef __GNUC__
		__attribute__((format(printf, 2, 3)))
#endif
		;

	Config cfg_;
	Source src_;
	Sink sink_;
	State state_ = STARTUP;
	const char *why_ = "";
	std::string log_;

	s32 n_ = 512, cp_ = 40, s_ = 552;
	s16 klo_ = 21, khi_ = 193;     /* the pair's band [klo_, khi_) */
	s16 kprobe_ = 24;
	s16 split_ = 0;                /* the upper band's first tone */
	float scale_ = 0;              /* a unit tone's amplitude */
	VwbFft fft_;
	VwbInterp interp_;
	std::vector<cf> xsync_;        /* the sync symbol, QPSK by tone */
	VwbLoading full_;              /* the whole band, for the sync */

	/* the receiver: the line's samples, then the far clock's */
	VwbHistory line_in_, rx_hist_;
	VwbClock clk_;
	s64 line_t_ = 0;               /* the next line sample's index */
	s64 rx_next_ = 0;              /* the next far clock sample made */
	s64 win_ = 0;                  /* the next window's start */
	s32 adv_ = 10;                 /* the window that much before
					  the end of the cyclic prefix */
	Rx rx_state_ = RX_PROBE_WAIT;
	s32 rx_count_ = 0;             /* windows in the state */
	s32 rx_total_ = 0;
	s32 detect_hits_ = 0, detect_last_ = 0;
	s64 detect_at_ = 0;
	std::vector<cf> h_;            /* the channel, by tone */
	std::vector<float> err_;       /* data: the decisions' error power */
	std::vector<double> sig_acc_, noise_acc_;
	s32 acc_n_ = 0;
	std::vector<float> snr_rx_;    /* measured, dB, by tone */
	std::vector<float> snr_far_;   /* the far end's of our signal */
	s16 segs_ = 0;
	s32 lost_ = 0;

	/* the transmitter's samples: the answerer's on the far clock
	   (its receiver's), the caller's on its own */
	VwbHistory tx_hist_;
	s64 tx_next_ = 0;              /* the next sample made */
	s64 tx_grid_ = 0;              /* where the answerer's symbols start */
	bool align_ = false;           /* the next symbol on tx_grid_ */
	Tx tx_state_ = TX_SILENT;
	s32 tx_count_ = 0, tx_left_ = 0;
	const VwbLoading *tx_band_ = nullptr;  /* the sync's and segue's */
	Tx tx_after_seg_ = TX_SILENT;
	double probe_phase_ = 0;
	s64 echo_ = -1;                /* our echo's delay (modulo a
					  symbol): the answerer's lines up
					  its symbols, the caller's the
					  plan's shift */
	s32 shift_ = 0;                /* the answerer's symbols later by
					  this from the plan's sync on */
	s32 pad_ = 0;                  /* zeros to send first */
	s64 echo_tx0_ = -1;            /* where its full-band sync began */

	/* the message channel: frames of a fixed number of symbols */
	VwbLoading msg_tx_, msg_rx_;   /* their bands */
	s16 edge_ = 0;                 /* each one's tones */
	s16 rep_tx_ = 2, rep_rx_ = 4;  /* each bit on that many tones */
	s32 frame_syms_tx_ = 0, frame_syms_rx_ = 0;
	s32 frame_pay_tx_ = 0, frame_pay_rx_ = 0;   /* payload octets */
	std::vector<u8> frame_out_;    /* the frame being sent, its bits */
	std::vector<u8> whiten_;       /* XORed on a frame's bits */
	s32 frame_pos_ = 0;
	std::vector<float> frame_soft_;
	s32 frame_rx_sym_ = 0;
	u8 tx_msg_ = MSG_IDLE;
	s16 go_left_ = 0;              /* our GOs still to send */
	bool sent_go_ = false;
	bool plan_ok_ = false;         /* the plan agreed (we have it) */
	s16 far_go_left_ = 0;          /* the far end's frames to its
					  switch */
	std::vector<u8> plan_;         /* the plan's octets (as sent) */

	/* data */
	VwbLoading tx_, rx_;
	ReedSolomon rs_;
	s16 depth_tx_ = 1, depth_rx_ = 1;
	u32 scr_ = 0, dscr_ = 0;
	std::vector<u8> tx_bits_;      /* bits waiting for symbols */
	size_t tx_bit_pos_ = 0;
	std::vector<u8> rx_octets_;
	u32 rx_acc_ = 0;
	s16 rx_acc_n_ = 0;
	s64 cw_total_ = 0, cw_fixed_ = 0, cw_failed_ = 0;
	std::vector<s8> cw_hist_;      /* the last codewords: 1 failed */
	size_t cw_hist_pos_ = 0;
	float snr_db_ = 0;
	bool tx_data_ = false, rx_data_ = false;
	s32 retrains_ = 0;
	s64 since_data_ = 0;           /* line samples without data */
};

} // namespace dsp

#endif /* DSP_VWB_H */
