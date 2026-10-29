/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vcvm_probe.cpp - V.cvm's probing (Annex B.1, B.2): L1, L2 and L3
 *                     sent, received and measured, and the loading of
 *                     clause 9.3 from what was measured
 *
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dm_types.h"
#include "bits.h"
#include "reinit.h"
#include "vcvm_startup.h"

namespace dsp {

/* see vcvm_startup.h: bit c comb code c (N200 to N400 at 8000 Hz, W200,
   W300 and W400 at 16000) */
u16 vcvm_probe_combs(s32 fs, s16 allowed)
{
	if (fs == 8000)
		return allowed & VCVM_M_NARROW ? 0x7 : 0;
	if (fs == 16000)
		return (allowed & VCVM_M_WIDE ? 0x18u : 0) |
		       (allowed & VCVM_M_WIDE400 ? 0x20u : 0);
	return 0;
}

/* the known phase steps of a symbol: two bits a tone, from the lowest,
   from the scrambler fed with ones, Gray coded as DQPSK */
static void known_steps(u32 *scr, s16 tap, s16 tones, u8 *ph8)
{
	for (s16 k = 0; k < tones; k++) {
		s16 b1 = scramble(scr, tap, 1), b0 = scramble(scr, tap, 1);
		ph8[k] = static_cast<u8>(2 * vcvm_ungray(b1 << 1 | b0));
	}
}

/* the known lag symbol's four bits, and its index */
static s16 known_lag(u32 *scr, s16 tap, u8 *b)
{
	s16 v = 0;
	for (s16 i = 0; i < 4; i++) {
		b[i] = static_cast<u8>(scramble(scr, tap, 1));
		v = v << 1 | b[i];
	}
	return vcvm_ungray(v);
}

/* a comb part: its reference, then n - 1 symbols of known steps */
static void comb_part(s16 code, s32 fs, bool sym22, s16 n, u32 *scr,
		      s16 tap, std::vector<float> &out)
{
	VcvmCombTx tx;
	if (tx.init(code, fs, sym22))
		return;
	VcvmLoad l;
	l.all(tx.comb().tones, 2);
	tx.set_load(l);
	const s16 sps = tx.sps();
	size_t at = out.size();
	out.resize(at + static_cast<size_t>(n) * sps);
	tx.reference(&out[at]);
	u8 ph8[VCVM_TONES], rs[VCVM_TONES] = {};
	for (s16 s = 1; s < n; s++) {
		known_steps(scr, tap, tx.comb().tones, ph8);
		tx.steps(ph8, rs, &out[at + static_cast<size_t>(s) * sps]);
	}
}

/* a parametric part: its preamble, then n symbols of known lags */
static void param_part(s32 fs, bool p10, s16 n, u32 *scr, s16 tap,
		       std::vector<float> &out)
{
	VcvmParamTx tx;
	if (tx.init(fs, p10))
		return;
	const s16 T = tx.sps();
	size_t at = out.size();
	out.resize(at + static_cast<size_t>(8 + n) * T);
	tx.preamble(&out[at]);
	u8 b[4];
	for (s16 s = 0; s < n; s++) {
		known_lag(scr, tap, b);
		tx.data(b, &out[at + static_cast<size_t>(8 + s) * T]);
	}
}

/* see vcvm_startup.h: L1's combs, L2, L3's P20 and P10, the known
   steps from one scrambler throughout */
void vcvm_probe_send(const VcvmProbe &p, std::vector<float> &out)
{
	u32 scr = 0;
	for (s16 c = 0; c < 9; c++)
		if (p.combs & (1u << c))
			comb_part(c, p.fs, false, VcvmProbe::L1_SYMS, &scr,
				  p.tap, out);
	if (p.l2)
		comb_part(p.l2_code(), p.fs, true, VcvmProbe::L2_SYMS, &scr,
			  p.tap, out);
	if (p.l3) {
		param_part(p.fs, false, VcvmProbe::L3_P20, &scr, p.tap, out);
		param_part(p.fs, true, VcvmProbe::L3_P10, &scr, p.tap, out);
	}
}

/*
 *    the receiver
 */

/* the receiver for the parts p says (L1's combs, L2, L3), idle until
   start(); 0, or -1 for a rate not 8000 or 16000 */
s16 VcvmProbeRx::init(const VcvmProbe &p)
{
	if (p.fs != 8000 && p.fs != 16000)
		return -1;
	p_ = p;
	m_ = VcvmProbeMeas();
	parts_.clear();
	for (s16 c = 0; c < 9; c++)
		if ((p.combs & (1u << c)) && m_.ncomb < 3) {
			VcvmCombMeas &cm = m_.comb[m_.ncomb++];
			cm.code = c;
			parts_.push_back({ c, false, VcvmProbe::L1_SYMS, &cm });
		}
	if (p.l2) {
		m_.l2.code = p.l2_code();
		m_.l2.sym22 = true;
		parts_.push_back({ p.l2_code(), true, VcvmProbe::L2_SYMS, &m_.l2 });
	}
	st_ = IDLE;
	hist_.clear();
	hbase_ = 0;
	return 0;
}

/* see vcvm_startup.h: L3 at once where there are no combs; 3 s for
   the onset */
void VcvmProbeRx::start(s32 from)
{
	hist_.clear();
	hbase_ = scan_ = from;
	scr_ = 0;
	part_ = 0;
	st_ = parts_.empty() ? (p_.l3 ? L3A : DONE) : ONSET;
	first_ = from;
	end_ = from;
	deadline_ = from + 3L * p_.fs;
	if (st_ == L3A)
		begin_param(false);
	else if (st_ == DONE)
		end_ = from;
}

/* see vcvm_startup.h: the last 3 to 4 s kept, the parts taken as they
   come */
void VcvmProbeRx::push(const float *x, s32 n)
{
	hist_.insert(hist_.end(), x, x + n);
	if (static_cast<s32>(hist_.size()) > 4L * p_.fs) {
		s32 cut = static_cast<s32>(hist_.size()) - 3L * p_.fs;
		hist_.erase(hist_.begin(), hist_.begin() + cut);
		hbase_ += cut;
	}
	run();
}

/* the samples a receiver has not had yet */
template <class R> static void feed(R &r, const std::vector<float> &h,
				    s32 hbase)
{
	s32 a = std::max(r.received(), hbase),
		e = hbase + static_cast<s32>(h.size());
	if (a < e)
		r.push(&h[static_cast<size_t>(a - hbase)],
		       static_cast<s32>(e - a));
}

/* the next comb part: its receiver from a symbol before its expected
   start, its sums cleared, 2 s to receive it */
void VcvmProbeRx::begin_comb()
{
	Part &pt = parts_[static_cast<size_t>(part_)];
	crx_.init(pt.code, p_.fs, pt.sym22);
	const s16 sps = crx_.comb().sps;
	crx_.restart(std::max(hbase_, first_ - 2 * sps));
	acquired_ = pend_ = resolve_ = false;
	sym_ = 0;
	n_ = 0;
	for (s16 k = 0; k < VCVM_TONES; k++)
		se_[k] = see_[k] = sa_[k] = lv_[k] = err_[k] = 0;
	pt.m->tones = crx_.comb().tones;
	deadline_ = first_ + 2L * p_.fs;
	st_ = COMB;
}

/* L3's P20 (or P10) part: locked where the last part ended, or P20's
   preamble looked for where no comb came before; 1.5 s to receive it */
void VcvmProbeRx::begin_param(bool p10)
{
	prx_.init(p_.fs, p10);
	VcvmParam pp;
	pp.init(p_.fs, p10);
	prx_.restart(std::max(hbase_, first_ - pp.sps / 2));
	/* each part where the last ends, after its preamble: the comb's
	   timing (and P20's for P10) is surer than the preambles' search,
	   and P10's preamble lags (20 and 35 samples) are the alphabet's
	   edges, which the lower codec modes code worst.  Without a comb
	   before it, P20's preamble is looked for */
	if (p10 || !parts_.empty())
		prx_.lock_at(first_ + 8L * pp.sps);
	sym_ = 0;
	pend_ = false;
	deadline_ = first_ + 3L * p_.fs / 2;
	csum_ = 0;
	st_ = p10 ? L3B : L3A;
}

/* a comb part's sums into its measurements *cm: per tone the phase
   error, the magnitude's change, the level, the DQPSK errors */
void VcvmProbeRx::finish_comb(VcvmCombMeas *cm)
{
	double mx = 0;
	for (s16 k = 0; k < cm->tones; k++)
		mx = std::max(mx, lv_[k]);
	cm->n = n_;
	for (s16 k = 0; k < cm->tones; k++) {
		double n = std::max<s16>(n_, 1), mu = se_[k] / n;
		cm->sigma[k] = static_cast<float>(
			sqrt(std::max(0.0, see_[k] / n - mu * mu)));
		cm->sigma_a[k] = static_cast<float>(sqrt(sa_[k] / n));
		cm->level_db[k] = static_cast<float>(
			20 * log10((lv_[k] + 1e-9) / (mx + 1e-9)));
		cm->errs[k] = static_cast<s16>(err_[k]);
	}
	cm->ok = n_ > 0;
}

/* the first comb's first symbol, which the prefix's timing finds only
   to a symbol, by the signal's onset: through a codec that opens up
   slowly the onset comes as much as half a symbol late, so that a few
   samples either way are a symbol either way.  The symbol before, the
   one acquired and the one after, each as the start: the one whose
   first symbols' steps are nearest the known ones.  0, or -1 while
   the symbols are not all in */
s16 VcvmProbeRx::resolve_start()
{
	enum { NS = 8 };
	const s16 sps = crx_.comb().sps, K = crx_.comb().tones;
	const s32 s0 = crx_.next_start();
	feed(crx_, hist_, hbase_);
	if (crx_.received() < s0 + static_cast<s32>(NS + 3) * sps)
		return -1;
	double best = 1e300;
	s32 at = s0;
	for (s16 d = -1; d <= 1; d++) {
		s32 c0 = s0 + d * sps;
		if (c0 < crx_.base())
			continue;
		VcvmCombRx c = crx_;
		c.start_at(c0);
		u32 scr = scr_;
		u8 ph8[VCVM_TONES];
		VcvmCombSym s;
		double e = 0;
		if (!c.next(&s))
			continue;
		for (s16 i = 1; i <= NS; i++) {
			known_steps(&scr, p_.tap, K, ph8);
			if (!c.next(&s, ph8))
				break;
			/* (the codec's start aside) */
			if (i >= 2)
				for (s16 k = 0; k < K; k++)
					e += fabs(s.perr[k]);
		}
		if (e < best) {
			best = e;
			at = c0;
		}
	}
	if (at != s0)
		crx_.start_at(at);
	return 0;
}

/* the probing received so far taken: L1's onset, each comb part's
   symbols against the known steps, L3's lags against the known ones;
   FAILED or DONE at the end */
void VcvmProbeRx::run()
{
	for (;;) {
		switch (st_) {
		case IDLE:
		case DONE:
		case FAILED:
			return;
		case ONSET: {
			/* 5 ms of the signal at -35 dBFS: over the level
			   the message mode's receiver takes for quiet (-40),
			   which a codec's tail after a message may keep */
			const s16 w = p_.fs / 200, step = p_.fs / 1000;
			const double thr = pow(32767 * pow(10, -35 / 20.0), 2) * w;
			s32 e = received();
			for (; scan_ + w <= e; scan_ += step) {
				double s = 0;
				for (s16 t = 0; t < w; t++) {
					double v = hist_[static_cast<size_t>(
						scan_ - hbase_ + t)];
					s += v * v;
				}
				if (s >= thr)
					break;
			}
			if (scan_ + w > e) {
				if (e > deadline_)
					st_ = FAILED;
				return;
			}
			first_ = scan_;
			onset_ = true;
			begin_comb();
			break;
		}
		case COMB: {
			Part &pt = parts_[static_cast<size_t>(part_)];
			feed(crx_, hist_, hbase_);
			const s16 sps = crx_.comb().sps;
			if (!acquired_) {
				s32 from =
					onset_ ? first_ : first_ + sps / 4;
				if (crx_.acquire(from, first_)) {
					if (received() > deadline_)
						st_ = FAILED;
					return;
				}
				acquired_ = true;
				resolve_ = onset_;
				onset_ = false;
			}
			if (resolve_) {
				if (resolve_start() < 0) {
					if (received() > deadline_)
						st_ = FAILED;
					return;
				}
				resolve_ = false;
			}
			const s16 K = crx_.comb().tones;
			VcvmCombSym s;
			for (;;) {
				if (sym_ > 0 && !pend_) {
					known_steps(&scr_, p_.tap, K, ph8_);
					pend_ = true;
				}
				if (!crx_.next(&s, sym_ > 0 ? ph8_ : nullptr))
					return;
				pend_ = false;
				/* the first two after the reference aside: the
				   codec's start, the last comb's end */
				if (sym_ >= 3) {
					for (s16 k = 0; k < K; k++) {
						double e = s.perr[k] * 180 / M_PI;
						se_[k] += e;
						see_[k] += e * e;
						sa_[k] +=
							static_cast<double>(
								s.ratio_db[k]) *
							s.ratio_db[k];
						lv_[k] += s.level[k];
						err_[k] += fabs(s.perr[k]) > M_PI / 4;
					}
					n_++;
				}
				if (++sym_ == pt.n)
					break;
			}
			finish_comb(pt.m);
			first_ = crx_.next_start();
			end_ = first_;
			if (++part_ < static_cast<s16>(parts_.size()))
				begin_comb();
			else if (p_.l3)
				begin_param(false);
			else
				st_ = DONE;
			break;
		}
		case L3A:
		case L3B: {
			bool p10 = st_ == L3B;
			feed(prx_, hist_, hbase_);
			if (!prx_.locked() && !prx_.search()) {
				if (received() > deadline_) {
					/* not found: P20's part (if it came)
					   stands, the end where the part's
					   would be */
					VcvmParam p20, p10p;
					p20.init(p_.fs, false);
					p10p.init(p_.fs, true);
					end_ = first_ + (8L + VcvmProbe::L3_P10) * p10p.sps +
						(p10 ? 0 : (8L + VcvmProbe::L3_P20) * p20.sps);
					st_ = DONE;
				}
				return;
			}
			const s16 N = p10 ? VcvmProbe::L3_P10 : VcvmProbe::L3_P20;
			s16 idx;
			float c;
			while (sym_ < N) {
				u8 b[4];
				if (!pend_) {
					want_ = known_lag(&scr_, p_.tap, b);
					pend_ = true;
				}
				if (!prx_.next(&idx, &c))
					return;
				pend_ = false;
				if (p10) {
					m_.n10++;
					m_.e10 += idx != want_;
				} else {
					m_.n20++;
					m_.e20 += idx != want_;
					csum_ += c;
				}
				sym_++;
			}
			first_ = end_ = prx_.next_start();
			if (!p10) {
				m_.cbar = static_cast<float>(csum_ / N);
				m_.l3 = true;
				begin_param(true);
			} else
				st_ = DONE;
			break;
		}
		}
	}
}

/*
 *    the loading (9.3)
 */

/* the tones' loading of a comb, its bits a second; the tones whose
   phase error says no have none */
static double load_comb(const VcvmCombMeas &cm, s16 allowed, VcvmLoad *l)
{
	zero(*l);
	double bits = 0;
	for (s16 k = 0; k < cm.tones; k++) {
		if (cm.level_db[k] < -20)
			continue;               /* a) out of the band */
		double s = cm.sigma[k];
		s16 pb = s <= 9 && (allowed & VCVM_M_8DPSK) ? 3 : s <= 20 ? 2 : 0;
		if (!pb)
			continue;
		s16 rb = 0;
		if (allowed & VCVM_M_RINGS)
			rb = cm.sigma_a[k] <= 0.4f ? 2 : cm.sigma_a[k] <= 0.8f ? 1 : 0;
		l->phase[k] = static_cast<u8>(pb);
		l->ring[k] = static_cast<u8>(rb);
		bits += pb + rb;
	}
	return bits * 1000 / (cm.sym22 ? 22.5 : 20);
}

/* d): the code rate from the tone symbol error ratio, measured (on
   DQPSK) or, where higher, what the phase error gives on the order
   loaded */
static s16 comb_rate(const VcvmCombMeas &cm, const VcvmLoad &l)
{
	double p = 0;
	s16 n = 0;
	for (s16 k = 0; k < cm.tones; k++) {
		if (!l.phase[k])
			continue;
		double M = l.phase[k] == 2 ? 4 : 8;
		double sr = std::max(cm.sigma[k], 0.1f) * M_PI / 180;
		double pred = erfc(M_PI / M / (sr * sqrt(2.0)));
		double meas = cm.n ? static_cast<double>(cm.errs[k]) / cm.n : 0;
		p += std::max(pred, l.phase[k] == 2 ? meas : 0.0);
		n++;
	}
	p = n ? p / n : 1;
	return p < 1e-3 ? 2 : p < 1e-2 ? 1 : 0;
}

/* comb code (sym22: its 22.5 ms symbol) in the modes allowed */
static bool comb_allowed(s16 code, bool sym22, s16 allowed)
{
	if (sym22 && !(allowed & VCVM_M_SYM22))
		return false;
	if (code < 3)
		return allowed & VCVM_M_NARROW;
	if (code < 5)
		return allowed & VCVM_M_WIDE;
	if (code == 5)
		return allowed & VCVM_M_WIDE400;
	return false;
}

/* see vcvm_startup.h: the comb that carries the most (its tones'
   orders and rings, its code rate) where it loads four tones and
   carries more than the parametric mode; else that mode, or none */
VcvmLoading vcvm_choose(const VcvmProbeMeas &m, s16 allowed)
{
	VcvmLoading best;
	double bbps = -1;
	const VcvmCombMeas *cand[4];
	s16 nc = 0;
	for (s16 i = 0; i < m.ncomb; i++)
		cand[nc++] = &m.comb[i];
	cand[nc++] = &m.l2;
	for (s16 i = 0; i < nc; i++) {
		const VcvmCombMeas &cm = *cand[i];
		if (!cm.ok || !comb_allowed(cm.code, cm.sym22, allowed))
			continue;
		VcvmLoad l;
		double bps = load_comb(cm, allowed, &l);
		if (bps > bbps) {
			bbps = bps;
			best.kind = VcvmLoading::COMB;
			best.code = cm.code;
			best.sym22 = cm.sym22;
			best.tones = cm.tones;
			best.load = l;
			best.rate = comb_rate(cm, l);
		}
	}
	/* e) the comb where it loads four tones, unless the parametric
	   mode carries as much after the code */
	VcvmLoading p;
	if ((allowed & VCVM_M_PARAM) && m.l3) {
		/* P10 where none of its 80 erred; else P20 where at most one
		   of its 40 did (one in 40 is the measurement's, not always
		   the codec mode's); the code rate from the errors */
		if (m.n10 && !m.e10) {
			p.kind = VcvmLoading::PARAM;
			p.p10 = true;
			p.rate = 2;
		} else if (m.n20 && m.e20 <= 1) {
			p.kind = VcvmLoading::PARAM;
			p.rate = m.e20 ? 0 : 2;
		}
	}
	if (best.kind == VcvmLoading::COMB && best.loaded() >= 4 &&
	    (p.kind != VcvmLoading::PARAM || best.coded_bps() > p.coded_bps()))
		return best;
	return p;
}

} // namespace dsp
