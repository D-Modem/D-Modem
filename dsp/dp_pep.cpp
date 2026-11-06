/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_pep.cpp - the PEP pump (DP_PEP, AT+MS=PEP or 255, Telebit's
 *                 S50=255): one end of pep.h's modem, at 8000 Hz (9600
 *                 resampled to it each way); the core's bits its octets
 *                 (the first bit the least significant); CONNECT at the
 *                 rate each way when both send all they can
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "pep.h"
#include "pump.h"

namespace dsp {

enum class PepSt { RUN };

class PepPump : public PumpBase<PepPump, PepSt> {
public:
	static constexpr const char *name = "PEP";
	/* its name in the log */
	const char *tag() const { return "pep"; }

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 process(void *in, void *out, s32 cnt) override;

private:
	s32 source(u8 *buf, s32 n);
	bool pull(u8 *v);
	void pace();
	void deliver();
	void deliver_all();
	bool idle(u8 v);
	void sink(const u8 *d, s32 n);
	void log_lines();
	void run8k(float x);
	void stats_pep(bool tables);
	s16 watch(float x);
	s16 not_pep(const char *why, enum DP_ID id);

	PepModem mdm_;
	bool up_ = false;               /* CONNECT reported */
	bool resample_ = false;         /* 9600 Hz */
	Resampler down_, up_rs_;
	std::deque<float> tx_;          /* at srate, to send */
	std::string pending_;
	/* the link's bits kept for PEP: the ones they end with, the
	   last octet and how many of it in a row */
	static constexpr s16 FLAGS_KEPT = 16;
	static constexpr s32 TXQ_TURNS = 2;
	std::deque<u8> txq_;
	double credit_ = 0;             /* pace()'s bits due */
	std::deque<u8> rxq_;            /* received, for the link */
	double rx_credit_ = 0;          /* deliver()'s bits due */
	s16 ones_ = 0;
	u8 last_ = 0;
	s16 same_ = 0;

	/* automode (+MS=PEP,1): a caller that is not PEP's handed on */
	bool automode_ = false;
	/* the caller's watch of the line before PEP's tones: 10 ms blocks,
	   the power in 2100 Hz (an answer tone), in PEP's two tones */
	Goertzel g2100_, g1437_, g1687_;
	s32 blk_n_ = 0;
	double blk_e_ = 0;
	s32 ans_ = 0;                   /* blocks of the answer tone */
	bool ans_over_ = false;         /* it ended */
	s32 after_ = 0;                 /* blocks since */
	s32 other_ = 0;                 /* blocks of neither, in a row */
	s32 gap_ = 0;                   /* of the answer tone's, in a row */
	std::vector<float> env_;        /* the answer tone's amplitude */
	/* the answerer's V.25 answer tone before PEP, as the T2000's
	   (DSP_PEP_ANS=1): samples of it, then of silence, still to send */
	s32 ans_tone_ = 0, ans_gap_ = 0;
	double ans_ph_ = 0;
};

/* the pump at srate_ (8000 or 9600 Hz), calling or answering; S7 the
   time to CONNECT: 0, or -1 */
s16 PepPump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_, s32)
{
	if (srate_ != 8000 && srate_ != 9600) {
		DSP_DBG("pep: %" PRId32 " Hz: PEP runs at 8000 or 9600 Hz\n",
			srate_);
		return -1;
	}
	pump_setup(m, srate_, caller_);
	resample_ = srate_ == 9600;
	if (resample_ && (down_.init(5, 6) || up_rs_.init(6, 5)))
		return -1;
	PepModem::Config c;
	c.calling = caller_;
	c.timeout_s = static_cast<s32>(carrier_loss / srate_) + 4;
	if (const char *e = pump_test_env("DSP_PEP_MAXBITS", caller_))
		c.max_bits = static_cast<s16>(std::strtol(e, nullptr, 10));
	if (const char *e = pump_test_env("DSP_PEP_TURN", caller_))
		c.turn_max = static_cast<s16>(std::strtol(e, nullptr, 10));
	mdm_.init(c, [this](u8 *b, s32 n) { return source(b, n); },
		  [this](const u8 *d, s32 n) { sink(d, n); });
	automode_ = modem_get_sreg(m, SREG_AUTOMODE) != 0;
	if (!caller_ && pump_test_env("DSP_PEP_ANS", 0)) {
		ans_tone_ = srate_ * 31 / 10;
		ans_gap_ = srate_ * 7 / 100;
	}
	s16 blk = static_cast<s16>(srate_ / 100);
	g2100_.init(2100, blk, srate_);
	g1437_.init(1437.5f, blk, srate_);
	g1687_.init(1687.5f, blk, srate_);
	stat_dp(DP_PEP);
	log_lines();
	DSP_DBG("pep: create %s, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", srate_);
	return 0;
}

/* the link's next octet, the first bit the least significant; whether
   it was kept (not idle()) */
bool PepPump::pull(u8 *v)
{
	u8 b[8];
	std::fill_n(b, 8, 1);
	modem_get_bits(modem, 1, b, 8);
	u8 x = 0;
	for (s16 k = 0; k < 8; k++)
		x = static_cast<u8>(x | (b[k] & 1) << k);
	*v = x;
	return !idle(x);
}

/* the link's bits at its rate (connect_at()'s, its bit timers'), one
   sample's worth, when it frames them: into txq_ while it holds less
   than a few turns of them; its idle left out */
void PepPump::pace()
{
	if (!up_ || !rt || !rt->framed)
		return;
	credit_ += static_cast<double>(mdm_.duplex_rate(false)) / srate;
	size_t cap = static_cast<size_t>(TXQ_TURNS) *
		static_cast<size_t>(mdm_.turn_octets());
	while (credit_ >= 8) {
		credit_ -= 8;
		if (txq_.size() >= cap)
			continue;
		u8 v;
		if (pull(&v))
			txq_.push_back(v);
	}
}

/* up to n octets of the link's for PEP: what pace() took, then more at
   once while it has them (until its idle) */
s32 PepPump::source(u8 *buf, s32 n)
{
	s32 got = 0;
	while (got < n && !txq_.empty()) {
		buf[got++] = txq_.front();
		txq_.pop_front();
	}
	u8 v;
	while (got < n && pull(&v))
		buf[got++] = v;
	return got;
}

/* whether the link's octet v is idle that can go (when the link frames
   its bits, dp_runtime_set_framed()): ones after at least
   9 kept (marks: no character or frame has so many), or the flag (at
   any of its 8 phases) after FLAGS_KEPT of it kept (HDLC's flags
   between frames, as many as the answerer's detection phase needs to
   find one as a character; never three in a row in asynchronous
   characters, whose start bits come every 10); v kept, if not */
bool PepPump::idle(u8 v)
{
	if (!rt || !rt->framed)
		return false;
	auto flag = [](u8 x) {
		for (s16 r = 0; r < 8; r++)
			if (static_cast<u8>(0x7e << r | 0x7e >> (8 - r)) == x)
				return true;
		return false;
	};
	if ((v == 0xff && ones_ >= 9) ||
	    (same_ >= FLAGS_KEPT && v == last_ && flag(v)))
		return true;
	if (v == 0xff) {
		ones_ += 8;
	} else {
		/* the ones it ends with, sent last (its top bits) */
		s16 t = 0;
		while (t < 8 && (v >> (7 - t) & 1))
			t++;
		ones_ = t;
	}
	same_ = v == last_ ? static_cast<s16>(std::min<s16>(same_ + 1,
							     FLAGS_KEPT)) : 1;
	last_ = v;
	return false;
}

/* octets received, for the link layer (deliver()), the first bit the
   least significant */
void PepPump::sink(const u8 *d, s32 n)
{
	rxq_.insert(rxq_.end(), d, d + n);
	if (!rt || !rt->framed)
		deliver_all();
}

/* (see deliver()) all of them at once: a raw stream's, which has no
   timers to keep */
void PepPump::deliver_all()
{
	while (!rxq_.empty()) {
		u8 b[8];
		for (s16 k = 0; k < 8; k++)
			b[k] = (rxq_.front() >> k) & 1;
		rxq_.pop_front();
		modem_put_bits(modem, 1, b, 8);
	}
}

/* received octets to the link at the far side's line rate, one
   sample's worth (as a modem's line gives them, not a turn's at once:
   its bit timers, such as V.42's detection phase's, see them come) */
void PepPump::deliver()
{
	if (rxq_.empty()) {
		rx_credit_ = 0;
		return;
	}
	rx_credit_ += static_cast<double>(mdm_.rx_rate()) / srate;
	while (rx_credit_ >= 8 && !rxq_.empty()) {
		rx_credit_ -= 8;
		u8 b[8];
		for (s16 k = 0; k < 8; k++)
			b[k] = (rxq_.front() >> k) & 1;
		rxq_.pop_front();
		modem_put_bits(modem, 1, b, 8);
	}
}

/* PEP's statistics into the call's (ATI11, ATI12): the packets' counts,
   and if tables the rates, the loadings and the noise */
void PepPump::stats_pep(bool tables)
{
	if (!stats)
		return;
	stats->have |= DP_STATS_PEP;
	stats->pep_bad = static_cast<unsigned long>(mdm_.packets_bad());
	stats->pep_good = static_cast<unsigned long>(mdm_.packets_good());
	stats->pep_micro_tx = static_cast<unsigned long>(mdm_.micro_tx());
	stats->pep_micro_rx = static_cast<unsigned long>(mdm_.micro_rx());
	if (!tables)
		return;
	stats->pep_tx_rate = mdm_.tx_rate();
	stats->pep_rx_rate = mdm_.rx_rate();
	const PepLoading &tl = mdm_.tx_loading(), &rl = mdm_.rx_loading();
	for (s16 k = 1; k <= pep::CARRIERS && k < DP_STATS_PEP_BINS; k++) {
		size_t i = static_cast<size_t>(k);
		auto bits = [i](const PepLoading &l) {
			u8 b = i < l.bits.size() ? l.bits[i] : 0;
			return static_cast<unsigned char>(b == pep::PILOT ? 0 : b);
		};
		stats->pep_tx_bits[i] = bits(tl);
		stats->pep_rx_bits[i] = bits(rl);
		stats->pep_noise[i] = mdm_.noise_dbm0(k);
	}
	/* its samples 1 / r long in ours: r - 1 shorter */
	stat_timing_offset(static_cast<float>(-mdm_.clock_ppm() * 1e-6), 1);
}

/* the modem's log lines to the DSP's log */
void PepPump::log_lines()
{
	pending_ += mdm_.take_log();
	size_t at = 0;
	for (;;) {
		size_t e = pending_.find('\n', at);
		if (e == std::string::npos)
			break;
		DSP_DBG("pep: %s\n", pending_.substr(at, e - at).c_str());
		at = e + 1;
	}
	pending_.erase(0, at);
}

/* one 8000 Hz sample through the modem, what it sends to tx_ (at
   srate) */
void PepPump::run8k(float x)
{
	s16 i = dsp_clip16(x), o = 0;
	mdm_.process(&i, &o, 1);
	if (!resample_) {
		tx_.push_back(o);
		return;
	}
	float y[2];
	s16 n = up_rs_.run(o, y);
	for (s16 k = 0; k < n; k++)
		tx_.push_back(y[k]);
}

/* the call to another pump (automode): DPSTAT_CHANGEDP */
s16 PepPump::not_pep(const char *why, enum DP_ID id)
{
	DSP_DBG("pep: %s: not PEP, pump %d\n", why, static_cast<int>(id));
	/* the call's modulation is the next pump's */
	if (stats) {
		stats->dp = 0;
		stats->have &= ~DP_STATS_PEP;
	}
	if (id == DP_V32BIS && rt)
		rt->v32_automode = 1;
	return hand_over(id, 0);
}

/* the automode caller, one sample of the line until PEP's tones are
   heard: an answer tone with V.8's 15 Hz modulation (ANSam) is a V.8
   answerer, V.34's pump's (as it tells, 0.1 deep or more); a plain one (V.25's ANS) may be a Telebit's
   before its PEP (S92=2, the T2000's), so after it ends: 150 ms of a
   signal that is neither, or 1.5 s of nothing, is an older modem's,
   V.32bis's automode chain's.  DPSTAT_CHANGEDP then, else DPSTAT_OK */
s16 PepPump::watch(float x)
{
	blk_e_ += static_cast<double>(x) * x;
	s16 done = g2100_.run(x);
	g1437_.run(x);
	g1687_.run(x);
	if (!done)
		return DPSTAT_OK;
	float e = static_cast<float>(blk_e_ / (srate / 100));
	blk_e_ = 0;
	blk_n_++;
	const float min = DP_DBM0_POWER * 5e-5f;        /* -43 dBm0 */
	bool on = e > min;
	bool ans = on && g2100_.power > 0.5f * e;
	bool tones = on && g1437_.power + g1687_.power > 0.5f * e;
	/* (ANSam's phase reversals, every 450 ms, take a block or two
	   out of it: its amplitude the last one's there) */
	gap_ = ans || !ans_ ? 0 : gap_ + 1;
	if (!ans_over_ && (ans || (ans_ && gap_ < 5))) {
		ans_++;
		env_.push_back(ans || env_.empty() ? std::sqrt(g2100_.power) :
			       env_.back());
		/* 400 ms of it (6 periods of 15 Hz): its modulation at
		   15 Hz against its mean */
		if (env_.size() == 40) {
			double m = 0;
			for (float v : env_)
				m += v;
			m /= static_cast<double>(env_.size());
			std::complex<double> c(0, 0);
			for (size_t i = 0; i < env_.size(); i++)
				c += (env_[i] - m) * std::polar(1.0,
					-2 * M_PI * 15 * 0.01 *
					static_cast<double>(i));
			double depth = 2 * std::abs(c) /
				(static_cast<double>(env_.size()) *
				 std::max(m, 1e-9));
			DSP_DBG("pep: answer tone, its AM depth %.2f\n",
				depth);
			if (depth > 0.1)
				return not_pep("ANSam", DP_V34);
		}
		other_ = 0;
		return DPSTAT_OK;
	}
	if (ans_ && !ans_over_)
		ans_over_ = true;
	if (ans_over_)
		after_++;
	other_ = on && !tones && !ans ? other_ + 1 : 0;
	if (other_ >= 15)
		return not_pep("a signal of another modulation", DP_V32BIS);
	if (after_ >= 150 || (!ans_ && blk_n_ >= 300))
		return not_pep("no PEP tones", DP_V32BIS);
	return DPSTAT_OK;
}

/* cnt samples each way through the modem: CONNECT once both ways are
   on their own loadings; ERROR when it fails, or no CONNECT within S7 */
s16 PepPump::process(void *in, void *out, s32 cnt)
{
	const s16 *src = static_cast<const s16 *>(in);
	s16 *dst = static_cast<s16 *>(out);
	s16 ret = DPSTAT_OK;
	bool watching = automode_ && caller && !mdm_.far_heard();
	if (watching && rt && rt->answer_tone == ANSTONE_2225)
		return not_pep("Bell answer tone", DP_B212);
	for (s32 i = 0; i < cnt; i++) {
		/* the T2000's 3.1 s of 2100 Hz at -12 dBm0, 70 ms of
		   silence, then PEP */
		if (ans_tone_ > 0 || ans_gap_ > 0) {
			float y = 0;
			if (ans_tone_ > 0) {
				ans_tone_--;
				y = std::sqrt(2 * DP_DBM0_POWER * 0.063f) *
					static_cast<float>(std::sin(ans_ph_));
				ans_ph_ += 2 * M_PI * 2100 / srate;
			} else
				ans_gap_--;
			dst[i] = dsp_clip16(y);
			continue;
		}
		if (watching && !mdm_.far_heard()) {
			s16 r = watch(src[i]);
			if (r != DPSTAT_OK)
				return r;
		}
		if (resample_) {
			float y[2];
			s16 n = down_.run(src[i], y);
			for (s16 k = 0; k < n; k++)
				run8k(y[k]);
		} else
			run8k(src[i]);
		pace();
		deliver();
		float y = 0;
		if (!tx_.empty()) {
			y = tx_.front();
			tx_.pop_front();
		}
		dst[i] = dsp_clip16(y);
	}
	total += cnt;
	log_lines();
	if (mdm_.state() == PepModem::FAILED) {
		DSP_DBG("pep: %s\n", mdm_.why());
		return DPSTAT_ERROR;
	}
	if (!up_ && mdm_.connected()) {
		up_ = true;
		stat_snr(mdm_.snr_db());
		stat_rtd(static_cast<s32>(mdm_.rtd() * srate / pep::FS));
		ret = connect_at(mdm_.duplex_rate(false),
				 mdm_.duplex_rate(true));
		stats_pep(true);
	} else if (up_)
		stats_pep(false);
	/* the automode answerer: no PEP caller 2 s after our tones and
	   comb: V.8's answer (ANSam), then its automode chain */
	if (automode_ && !caller && !mdm_.far_heard() &&
	    mdm_.state() == PepModem::STARTUP &&
	    mdm_.now() > mdm_.first_end() + 2 * pep::FS)
		return not_pep("no PEP caller", DP_V34);
	if (!up_ && total >= timeout) {
		DSP_DBG("pep: no connection in time\n");
		return DPSTAT_ERROR;
	}
	return ret;
}

/* registers the pump as DP_PEP */
s16 dp_pep_init(void)
{
	return modem_dp_register(DP_PEP, &PumpOps<PepPump>::ops);
}

/* and no more */
void dp_pep_exit(void)
{
	modem_dp_deregister(DP_PEP, &PumpOps<PepPump>::ops);
}

} // namespace dsp
