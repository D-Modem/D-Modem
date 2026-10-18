/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_data.h - V.cvm's data mode (docs/v_cvm.pdf clauses 10.8, 10.9,
 *                  11, Annex C): the octets through the scrambler
 *                  (GPC from the calling DCE, GPA from the answering
 *                  one), the Reed-Solomon code (RS(32,k)), its
 *                  interleaver (4 codewords deep, 1 in the parametric
 *                  mode) and the mapping onto the loading's tones or lag symbols; the
 *                  receiver the reverse, the decisions' reliabilities
 *                  marking the decoder's erasures; and the control
 *                  sequence (eight symbols of 180 degrees on every loaded
 *                  tone, or sixteen of the lowest lag).
 *
 *    Phase 4 begins, in each direction, with the comb's phase reference
 *    symbol, sent three times (the codec's first symbols out of silence
 *    are its own), or the parametric mode's preamble; the first data
 *    symbol begins the first interleaver block, and the scrambler starts
 *    there.
 *
 *    The scrambler is the code's outer layer, on the information octets
 *    (10.9 puts it after the interleaver): the descrambler's tripling of
 *    each error then falls on what the code left, not on what it is to
 *    correct (a lag symbol in error would cost a parametric codeword
 *    three or four octets, beyond rate 7/8's two).
 *
 */

#ifndef DSP_VCVM_DATA_H
#define DSP_VCVM_DATA_H

#include <stdint.h>

#include <functional>
#include <vector>

#include "rs.h"
#include "vcvm.h"
#include "vcvm_startup.h"

namespace dsp {

/* the octets of an interleaver block: D codewords of 32 */
#define VCVM_RS_N       32

/* the data mode's transmitter */
class VcvmDataTx {
public:
	/* the octets to send: the next, or -1 for none now (then an HDLC
	   flag, 0x7e, which is what the link layer sends when idle) */
	using Source = std::function<int()>;

	/* the loading the peer asked for, at fs; tap the scrambler's
	   (SCR_GPC calling, SCR_GPA answering) */
	int init(const VcvmLoading &l, int fs, int tap, Source src);
	/* the loading sent */
	const VcvmLoading &loading() const { return l_; }
	int sps() const;

	/* the start of phase 4 (the phase reference, or the preamble) to
	   out, its samples */
	void start(std::vector<float> &out);
	/* one data symbol to out */
	void symbol(std::vector<float> &out);
	/* the control sequence (10.8) to out */
	void control(std::vector<float> &out);
	/* the information octets sent so far */
	long octets() const { return sent_; }

private:
	int bit();
	void block();

	VcvmLoading l_{};
	int fs_ = 8000, tap_ = 18, D_ = 4;
	Source src_;
	ReedSolomon rs_;
	VcvmCombTx ctx_;
	VcvmParamTx ptx_;
	unsigned scr_ = 0;
	std::vector<uint8_t> line_;     /* the block's octets, interleaved */
	size_t pos_ = 0;                /* its next bit */
	long sent_ = 0;
};

/* what the receiver has measured of the data (for 9.5's renegotiation
   and retrain) */
struct VcvmDataStats {
	long symbols = 0;
	long blocks = 0;
	long words = 0, failed = 0;     /* codewords, those not decoded */
	long corrected = 0;             /* octets the code corrected */
	long erasures = 0;              /* octets marked */
	float sigma[VCVM_TONES] = {};   /* each tone's phase error, degrees
					   rms (decision directed, the last
					   few seconds) */
	float cbar = 0;                 /* the parametric mode's c-bar */
};

/* the data mode's receiver */
class VcvmDataRx {
public:
	/* the information octets decoded, a block at a time */
	using Sink = std::function<void(const uint8_t *d, int n)>;

	/* the loading this DCE asked for, at fs; tap the peer's
	   scrambler's */
	int init(const VcvmLoading &l, int fs, int tap, Sink sink);
	/* the loading received */
	const VcvmLoading &loading() const { return l_; }

	/* look for phase 4's start from sample from (the samples pushed
	   next are from there) */
	void start(long from);
	void push(const float *x, int n);
	long received() const;

	/* phase 4's start found */
	bool locked() const { return locked_; }
	/* the first block decoded: the signal is the peer's data */
	bool confirmed() const { return confirmed_; }
	/* the control sequence received (once confirmed: before it, the
	   signal may be an MP): no more data (the block under way is
	   dropped) */
	bool control() const { return control_; }
	/* no codeword decoded for 2 s, or the timing lost: a retrain */
	bool lost() const { return lost_; }
	/* what has been measured of the data */
	const VcvmDataStats &stats() const { return st_; }
	/* the end of the last symbol taken */
	long position() const { return pos_; }

private:
	void symbol(const uint8_t *bits, const float *rel, int nb);
	void line_bit(int b, float rel);
	void level(double v);
	void decode_block();
	void run();

	VcvmLoading l_{};
	int fs_ = 8000, tap_ = 18, D_ = 4;
	Sink sink_;
	ReedSolomon rs_;
	VcvmCombRx crx_;
	VcvmParamRx prx_;
	bool locked_ = false, confirmed_ = false, control_ = false,
		lost_ = false;
	long from_ = 0, pos_ = 0;
	bool onset_ = false;
	int lead_ = 0;                  /* the reference symbols taken */
	/* the symbols held back three, for the control sequence */
	struct Held {
		std::vector<uint8_t> bits;
		std::vector<float> rel;
	};
	std::vector<Held> held_;
	int ctl_run_ = 0;
	/* the descrambler, on the decoded information octets */
	unsigned scr_ = 0;
	/* the octets coming */
	int nbit_ = 0;
	unsigned oct_ = 0;
	float orel_ = 1;
	std::vector<uint8_t> blk_;
	std::vector<float> rel_;        /* each octet's reliability */
	std::vector<uint8_t> early_;    /* decoded before the confirmation */
	long bad_since_ = -1;           /* symbols since a codeword was last
					   decoded */
	long quiet_ = 0;                /* symbols in a row far under the
					   level */
	double ref_ = 0;                /* the symbols' level */
	long lost_after_ = 0;           /* symbols: 2 s, or 2.5 blocks */
	long sym_ = 0;
	VcvmDataStats st_;
	double se2_[VCVM_TONES] = {};
};

} // namespace dsp

#endif /* DSP_VCVM_DATA_H */
