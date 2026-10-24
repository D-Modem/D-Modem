/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_data.cpp - V.cvm's data mode: the inner code, the interleaver,
 *                    the scrambler and the mapping (vcvm_data.h)
 *
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dm_types.h"
#include "bits.h"
#include "reinit.h"
#include "vcvm_data.h"

namespace dsp {

/*
 *    the transmitter
 */

/* see vcvm_data.h; -1 for no loading, or one the code or the signal
   set can't take */
int VcvmDataTx::init(const VcvmLoading &l, s32 fs, int tap,
		     Source src)
{
	if (l.kind == VcvmLoading::NONE)
		return -1;
	l_ = l;
	fs_ = fs;
	tap_ = tap;
	src_ = std::move(src);
	D_ = l.kind == VcvmLoading::PARAM ? 1 : 4;
	if (rs_.init(VCVM_RS_N, l.k()))
		return -1;
	if (l.kind == VcvmLoading::COMB) {
		if (ctx_.init(l.code, fs, l.sym22))
			return -1;
		ctx_.set_load(l.load);
	} else if (ptx_.init(fs, l.p10))
		return -1;
	scr_ = 0;
	line_.clear();
	pos_ = 0;
	sent_ = 0;
	return 0;
}

/* a symbol of the loading, samples */
int VcvmDataTx::sps() const
{
	return l_.kind == VcvmLoading::COMB ? ctx_.sps() : ptx_.sps();
}

/* see vcvm_data.h: the comb's reference three times, or the preamble */
void VcvmDataTx::start(std::vector<float> &out)
{
	size_t at = out.size();
	if (l_.kind == VcvmLoading::COMB) {
		/* the reference three times: the first two are the codec's,
		   which takes a symbol or two to follow a signal out of
		   silence */
		out.resize(at + 3 * static_cast<size_t>(sps()));
		for (int i = 0; i < 3; i++)
			ctx_.reference(
				&out[at + static_cast<size_t>(i) * sps()]);
	} else {
		out.resize(at + 8 * static_cast<size_t>(sps()));
		ptx_.preamble(&out[at]);
	}
}

/* D codewords of k information octets, interleaved octet by octet */
void VcvmDataTx::block()
{
	const int k = rs_.k(), n = VCVM_RS_N;
	line_.assign(static_cast<size_t>(n) * D_, 0);
	u8 cw[VCVM_RS_N];
	for (int c = 0; c < D_; c++) {
		for (int j = 0; j < k; j++) {
			int v = src_ ? src_() : -1;
			unsigned o = static_cast<unsigned>(v < 0 ? 0x7e : v),
				 sc = 0;
			sent_ += v >= 0;
			/* scrambled before the code, least significant bit
			   first: a line error costs the decoder one octet,
			   not the three the descrambler would make of it */
			for (int b = 0; b < 8; b++)
				sc |= static_cast<unsigned>(scramble(
					      &scr_, tap_, (o >> b) & 1))
				      << b;
			cw[j] = static_cast<u8>(sc);
		}
		rs_.encode(cw, cw);
		for (int j = 0; j < n; j++)
			line_[static_cast<size_t>(j) * D_ + c] = cw[j];
	}
	pos_ = 0;
}

/* the next coded bit, least significant first */
int VcvmDataTx::bit()
{
	if (pos_ >= line_.size() * 8)
		block();
	int b = (line_[pos_ / 8] >> (pos_ % 8)) & 1;
	pos_++;
	return b;
}

/* one data symbol of the loading's bits, the next coded bits, to out */
void VcvmDataTx::symbol(std::vector<float> &out)
{
	int nb = l_.bits();
	u8 bits[VCVM_TONES * 5];
	for (int i = 0; i < nb; i++)
		bits[i] = static_cast<u8>(bit());
	size_t at = out.size();
	out.resize(at + static_cast<size_t>(sps()));
	if (l_.kind == VcvmLoading::COMB)
		ctx_.data(bits, &out[at]);
	else
		ptx_.data(bits, &out[at]);
}

/* the control sequence to out: 8 symbols of a half turn on every tone,
   or 16 of index 0 (the lowest lag) */
void VcvmDataTx::control(std::vector<float> &out)
{
	size_t at = out.size();
	if (l_.kind == VcvmLoading::COMB) {
		u8 ph8[VCVM_TONES], rs[VCVM_TONES] = {};
		for (int k = 0; k < VCVM_TONES; k++)
			ph8[k] = 4;
		out.resize(at + 8 * static_cast<size_t>(sps()));
		for (int s = 0; s < 8; s++)
			ctx_.steps(ph8, rs,
				   &out[at + static_cast<size_t>(s) * sps()]);
	} else {
		out.resize(at + 16 * static_cast<size_t>(sps()));
		for (int s = 0; s < 16; s++)
			ptx_.symbol(0,
				    &out[at + static_cast<size_t>(s) * sps()]);
	}
}

/*
 *    the receiver
 */

/* see vcvm_data.h; -1 for no loading, or one the code or the signal
   set can't take; the receiver started at sample 0 */
int VcvmDataRx::init(const VcvmLoading &l, s32 fs, int tap,
		     Sink sink)
{
	if (l.kind == VcvmLoading::NONE)
		return -1;
	l_ = l;
	fs_ = fs;
	tap_ = tap;
	sink_ = std::move(sink);
	D_ = l.kind == VcvmLoading::PARAM ? 1 : 4;
	if (rs_.init(VCVM_RS_N, l.k()))
		return -1;
	if (l.kind == VcvmLoading::COMB) {
		if (crx_.init(l.code, fs, l.sym22))
			return -1;
		crx_.set_load(l.load);
	} else if (prx_.init(fs, l.p10))
		return -1;
	start(0);
	return 0;
}

/* see vcvm_data.h: everything back to unlocked, the statistics too */
void VcvmDataRx::start(s32 from)
{
	from_ = pos_ = from;
	locked_ = confirmed_ = control_ = lost_ = false;
	onset_ = false;
	lead_ = 0;
	if (l_.kind == VcvmLoading::COMB)
		crx_.restart(from);
	else
		prx_.restart(from);
	held_.clear();
	ctl_run_ = 0;
	scr_ = 0;
	nbit_ = 0;
	oct_ = 0;
	orel_ = 1;
	blk_.clear();
	rel_.clear();
	quiet_ = 0;
	ref_ = 0;
	/* 2 s without a codeword decoded, or two and a half blocks where
	   a block takes longer */
	lost_after_ = static_cast<s32>(std::max(
		lrint(2000 / l_.sym_ms()),
		static_cast<long>(2.5 * VCVM_RS_N * 8 * D_ / l_.bits()) + 1));
	early_.clear();
	bad_since_ = 0;
	sym_ = 0;
	st_ = VcvmDataStats();
	for (double &v : se2_)
		v = 0;
}

/* the samples received: the index of the next one pushed */
s32 VcvmDataRx::received() const
{
	return l_.kind == VcvmLoading::COMB ? crx_.received() : prx_.received();
}

/* samples from the line (16 bit scale), the symbols they complete
   taken */
void VcvmDataRx::push(const float *x, int n)
{
	if (l_.kind == VcvmLoading::COMB)
		crx_.push(x, n);
	else
		prx_.push(x, n);
	run();
}

/* a coded line bit in, into the octets: an octet as reliable as its
   least reliable bit */
void VcvmDataRx::line_bit(int b, float rel)
{
	oct_ |= static_cast<unsigned>(b & 1) << nbit_;
	orel_ = std::min(orel_, rel);
	if (++nbit_ == 8) {
		blk_.push_back(static_cast<u8>(oct_));
		rel_.push_back(orel_);
		oct_ = 0;
		orel_ = 1;
		nbit_ = 0;
		if (static_cast<int>(blk_.size()) == VCVM_RS_N * D_)
			decode_block();
	}
}

/* the interleaver block in, decoded: each codeword with errors alone,
   then with ever more of its least reliable octets erased; the
   information octets descrambled to the sink once the data is
   confirmed */
void VcvmDataRx::decode_block()
{
	const int n = VCVM_RS_N, k = rs_.k(), nc = n - k;
	std::vector<u8> out;
	int ok = 0;
	for (int c = 0; c < D_; c++) {
		u8 cw[VCVM_RS_N], er[VCVM_RS_N];
		float rl[VCVM_RS_N];
		int ord[VCVM_RS_N];
		for (int j = 0; j < n; j++) {
			cw[j] = blk_[static_cast<size_t>(j) * D_ + c];
			rl[j] = rel_[static_cast<size_t>(j) * D_ + c];
			ord[j] = j;
		}
		/* the errors alone, then the 2, 4, ... least reliable octets
		   erased (never all the check octets: a word with as many
		   erasures as checks always decodes, rightly or not) */
		std::sort(ord, ord + n, [&](int a, int b) { return rl[a] < rl[b]; });
		int r = rs_.decode(cw), ne = 0;
		for (int e = 2; r < 0 && e <= nc - 2; e += 2) {
			if (rl[ord[e - 1]] >= 0.5f)
				break;          /* no octet so unsure */
			zero(er);
			for (int i = 0; i < e; i++)
				er[ord[i]] = 1;
			r = rs_.decode(cw, er);
			ne = e;
		}
		st_.words++;
		st_.erasures += r >= 0 ? ne : 0;
		if (r < 0) {
			st_.failed++;
		}
		else {
			ok++;
			st_.corrected += r;
		}
		/* the information octets descrambled */
		for (int j = 0; j < k; j++) {
			unsigned o = 0;
			for (int b = 0; b < 8; b++)
				o |= static_cast<unsigned>(descramble(
					     &scr_, tap_, (cw[j] >> b) & 1))
				     << b;
			out.push_back(static_cast<u8>(o));
		}
	}
	st_.blocks++;
	blk_.clear();
	rel_.clear();
	if (ok)
		bad_since_ = 0;
	/* the signal is the peer's data once a block decodes whole (two
	   in a row of the parametric mode's one codeword) */
	if (!confirmed_) {
		if (ok < D_) {
			early_.clear();
			return;
		}
		early_.insert(early_.end(), out.begin(), out.end());
		if (static_cast<int>(early_.size()) <
		    (D_ == 1 ? 2 : 1) * k * D_)
			return;
		confirmed_ = true;
		out.swap(early_);
		early_.clear();
	}
	if (sink_)
		sink_(out.data(), static_cast<int>(out.size()));
}

/* a symbol's bits, held back so that the control sequence's are not
   taken for data */
void VcvmDataRx::symbol(const u8 *bits, const float *rel, int nb)
{
	Held h;
	h.bits.assign(bits, bits + nb);
	h.rel.assign(rel, rel + nb);
	held_.push_back(std::move(h));
	size_t hold = l_.kind == VcvmLoading::COMB ? 3 : 9;
	while (held_.size() > hold) {
		const Held &f = held_.front();
		for (size_t i = 0; i < f.bits.size(); i++)
			line_bit(f.bits[i], f.rel[i]);
		held_.erase(held_.begin());
	}
}

/* a symbol's level: the signal lost when it is 20 dB under the data's
   (silence decodes as the all-zero codeword), or when no codeword has
   decoded, for 2 s */
void VcvmDataRx::level(double v)
{
	if (ref_ == 0 || v > 0.3 * ref_)
		ref_ = ref_ == 0 ? v : 0.98 * ref_ + 0.02 * v;
	quiet_ = v < 0.1 * ref_ ? quiet_ + 1 : 0;
	bad_since_++;
	if (confirmed_ && (quiet_ > lost_after_ || bad_since_ > lost_after_))
		lost_ = true;
}

/* the symbols received so far taken: phase 4's start found, then each
   symbol's bits to the octets, its errors and level measured, until
   the control sequence */
void VcvmDataRx::run()
{
	if (control_)
		return;
	if (l_.kind == VcvmLoading::COMB) {
		if (!locked_) {
			if (!onset_) {
				s32 o = crx_.onset(from_, -35);
				if (o < 0 || crx_.acquire(o, o))
					return;
				onset_ = true;
			}
			/* the three references: the last is the one */
			VcvmCombSym s;
			while (lead_ < 3 && crx_.next(&s))
				lead_++;
			if (lead_ < 3)
				return;
			locked_ = true;
			pos_ = crx_.next_start();
		}
		const int K = l_.tones, nl = l_.loaded();
		VcvmCombSym s;
		while (!control_ && crx_.next(&s)) {
			sym_++;
			st_.symbols++;
			pos_ = crx_.next_start();
			int match = 0;
			for (int k = 0; k < K; k++) {
				if (!l_.load.phase[k])
					continue;
				match += s.ph8[k] == 4;
				double e = s.perr[k] * 180 / M_PI;
				se2_[k] = sym_ < 20 ? (se2_[k] * (sym_ - 1) + e * e) / sym_ :
					0.98 * se2_[k] + 0.02 * e * e;
				st_.sigma[k] =
					static_cast<float>(sqrt(se2_[k]));
			}
			double lv = 0;
			for (int k = 0; k < K; k++)
				if (l_.load.phase[k])
					lv += s.level[k];
			/* the control sequence: in the data, at its level
			   (silence decides as one phase step on every
			   tone) */
			bool ctl = confirmed_ && lv > 0.3 * ref_ &&
				match >= (nl >= 8 ? nl - 1 : nl);
			ctl_run_ = ctl ? ctl_run_ + 1 : 0;
			if (ctl_run_ >= 4) {
				control_ = true;
				held_.clear();
				break;
			}
			symbol(s.bits, s.rel, s.nbits);
			level(lv);
		}
		return;
	}
	if (!locked_) {
		if (!prx_.search())
			return;
		locked_ = true;
	}
	int idx;
	float c, mg;
	float rms;
	while (!control_ && prx_.next(&idx, &c, &mg, &rms)) {
		sym_++;
		st_.symbols++;
		pos_ = prx_.next_start();
		st_.cbar = prx_.cbar();
		/* of the data's level and a lag (silence decides as index 0),
		   in the data (an MP's zero octets are runs of index 0) */
		bool ctl = confirmed_ && idx == 0 && c > 0.5f && rms > 0.3 * ref_;
		ctl_run_ = ctl ? ctl_run_ + 1 : 0;
		if (ctl_run_ >= 10) {
			control_ = true;
			held_.clear();
			break;
		}
		u8 b[4];
		float r[4];
		VcvmParamRx::bits(idx, b);
		float rel = std::min(1.0f, std::max(0.0f, mg / 0.1f));
		for (float &v : r)
			v = rel;
		symbol(b, r, 4);
		level(rms);
	}
}

} // namespace dsp
