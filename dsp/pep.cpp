/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pep.cpp - Telebit's PEP: the ensemble, the start-up, the packets and
 *              the turns (pep.h; docs/dsp/pep-details.md)
 *
 */

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <queue>

#include "modem_defs.h"
#include "pep.h"

namespace dsp {

using namespace pep;

namespace {

const float TWO_PI_F = 6.283185307f;
const double TWO_PI = 6.283185307179586;

/* 0 dBR, the patent's -9 dBm (-9 dBm0: a sine of 0 dBm0 has an RMS of
   16055 of 32767): the comb's 511 carriers at -27 dBR each come to
   about that */
const float REF_RMS = 5700;
/* the timing tones' level against a comb carrier's: -3 dBR against -27 */
const float TONE_GAIN = 15.85f;
/* the phase epochs': -28 dBR */
const float PHASE_GAIN = 0.891f;
/* the comb's epochs in the start-up, the noise's windows, the phase
   epochs */
const s16 COMBS = 4;
const s16 NOISES = 3;
const s16 PHASES = 2;
/* a turn: silence first, then the two tones (samples); then one comb
   epoch and the packets */
const s32 TURN_GAP = 160;
const s32 PRE_LEN = 256;
/* the window's place in an epoch: the middle of the guard */
const s32 W_OFF = GUARD / 2;
/* after our own signal, the input is ours again after this (its echo) */
const s32 ECHO_GAP = 80;
/* and the noise measured from this much after it */
const s32 NOISE_GAP = 240;
/* the quiet level's rise a block, and how far above it the line is in
   use */
const double FLOOR_RISE = 1.002;
const double BUSY = 10;
/* no turn of the far side's by this long after ours: ours again (the
   caller's shorter, so that the two do not collide again) */
const s32 WAIT_CALLER = 4800, WAIT_ANSWERER = 7200;
/* the tones' detector: blocks of 128 samples (the tones on its bins 23
   and 27), hops of 32; the share of the block's power in the tones */
const s32 BLK = 128, HOP = 32;
const float TONES_ON = 0.6f, TONES_OFF = 0.35f;
const s16 TONES_HITS = 3;
/* micro turns (D-Modem's for Telebit's micro packets, 88 baud): the
   gap, 24 ms of the tones, then short epochs of 64 samples and 26 of
   guard (90, 88.9 baud): one of the comb (its timing), MS_DATA of one
   packet; their carriers every 16th bin (125 Hz) from 375 to 3375 Hz,
   16 times the power (the long epoch's power on a 16th of the
   carriers), so 2 bits more than the loading's there, 6 at most; the
   window from 13 samples in */
const s32 PRE_SHORT = 192;
const s32 MS_N = 64, MS_G = 26, MS_EP = MS_N + MS_G, MS_W = 13;
const s16 MS_DATA = 3, MS_STEP = 16, MS_LO = 3, MS_HI = 27;
const float MS_BOOST = 4;
const s32 MS_LEN = (1 + MS_DATA) * MS_EP;
/* the receiver's search for the short comb from the tones' end
   (samples), and its coherence at least */
const s32 MS_SCAN_LO = -32, MS_SCAN_HI = 128, MS_SCAN_STEP = 4;
const double MS_COHERENT = 0.8;
/* a packet's header and CRC (octets); its sequence numbers' window */
const s32 HDR = 7, OVERHEAD = HDR + 2;
const u8 WINDOW = 64;
enum { PK_FILL = 0, PK_DATA = 1, PK_CTRL = 2 };
/* the control stream's loading record: 'L' and 10 bits a carrier */
const s32 LOAD_LEN = 1 + (CARRIERS * 10 + 7) / 8;
/* a carrier's power at most against the comb's (+6 dB) */
const float MAX_POWER = 4;

/* the CRC-16 of X.25 (CCITT, initially all ones) */
u16 crc16(const u8 *p, size_t n)
{
	u16 c = 0xffff;
	for (size_t i = 0; i < n; i++) {
		c ^= static_cast<u16>(p[i] << 8);
		for (s16 k = 0; k < 8; k++)
			c = static_cast<u16>(c & 0x8000 ? (c << 1) ^ 0x1021 :
					     c << 1);
	}
	return c;
}

/* the packets' scrambler: a 16 bit LFSR (x^16 + x^14 + x^13 + x^11 + 1)
   from the same seed for every packet, its octets XORed on */
void scramble(u8 *p, size_t n)
{
	u16 s = 0xace1;
	for (size_t i = 0; i < n; i++) {
		u8 v = 0;
		for (s16 k = 0; k < 8; k++) {
			u16 bit = static_cast<u16>(((s >> 0) ^ (s >> 2) ^
						    (s >> 3) ^ (s >> 5)) & 1);
			s = static_cast<u16>(s >> 1 | bit << 15);
			v = static_cast<u8>(v | (s & 1) << k);
		}
		p[i] ^= v;
	}
}

/* sequence numbers: b - a */
inline u8 seq_diff(u8 b, u8 a) { return static_cast<u8>(b - a); }
/* whether b is in [a, a + WINDOW) */
inline bool in_window(u8 b, u8 a) { return seq_diff(b, a) < WINDOW; }

/* the bits a carrier may take, fewest first */
const s16 STEPS[] = { 0, 2, 4, 5, 6 };

} // namespace

/*
 *    loadings
 */

/* see pep.h */
void PepLoading::clear()
{
	bits.assign(CARRIERS + 1, 0);
	gain.assign(CARRIERS + 1, 0);
	bits[TONE1] = bits[TONE2] = PILOT;
	gain[TONE1] = gain[TONE2] = 1;
	count();
}

/* see pep.h */
void PepLoading::uniform(const std::vector<u8> &mask, s16 b)
{
	clear();
	for (s16 k = 1; k <= CARRIERS; k++)
		if (mask[static_cast<size_t>(k)] && bits[k] != PILOT) {
			bits[k] = static_cast<u8>(b);
			gain[k] = 1;
		}
	count();
}

/* see pep.h */
void PepLoading::count()
{
	total = 0;
	for (u8 b : bits)
		if (b != PILOT)
			total += b;
}

/* see pep.h */
std::string PepLoading::str() const
{
	s32 n[16] = {};
	for (s16 k = 1; k <= CARRIERS; k++)
		n[bits[static_cast<size_t>(k)] & 15]++;
	std::string s;
	for (s16 b = 1; b <= MAX_BITS; b++)
		if (n[b])
			s += "b" + std::to_string(b) + "x" +
			     std::to_string(n[b]) + " ";
	return s + std::to_string(total) + " bits an epoch";
}

/*
 *    the modem
 */

/* see pep.h */
void PepModem::init(const Config &c, Source src, Sink sink)
{
	cfg_ = c;
	src_ = std::move(src);
	sink_ = std::move(sink);
	fft_.init(N);
	in_.init(8 * 8192);
	scale_ = REF_RMS * std::sqrt(std::pow(10.0f, -2.7f) / 2);
	/* the comb: every carrier at -27 dBR, Newman's phases (a low peak
	   to mean ratio) */
	comb_.assign(CARRIERS + 1, cf(0, 0));
	for (s16 k = 1; k <= CARRIERS; k++)
		comb_[static_cast<size_t>(k)] = std::polar(1.0,
			M_PI * (k - 1) * (k - 1) / CARRIERS);
	H_.assign(CARRIERS + 1, cf(0, 0));
	link_.clear();
	tx_final_.clear();
	rx_final_.clear();
	if (c.calling) {
		ph_ = PH_C_LISTEN;
		rx_ = RX_TONES;
		rx_scan_ = 0;
	} else {
		ph_ = PH_A_SEND;
		tx_start_tones_comb(COMBS, 0, nullptr);
		first_end_ = tx_end_;
		logf("answerer: tones and comb");
	}
}

/* see pep.h */
void PepModem::logf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	char t[32];
	snprintf(t, sizeof(t), "%s %.3f ", cfg_.calling ? "c" : "a",
		 static_cast<double>(t_) / FS);
	log_ += t;
	log_ += buf;
	log_ += '\n';
}

/* see pep.h */
std::string PepModem::take_log()
{
	std::string s;
	s.swap(log_);
	return s;
}

/* given up, why */
void PepModem::fail(const char *why)
{
	if (state_ == FAILED)
		return;
	state_ = FAILED;
	why_ = why;
	logf("failed: %s", why);
}

/* see pep.h */
void PepModem::process(const s16 *in, s16 *out, s32 n)
{
	for (s32 i = 0; i < n; i++) {
		float y = 0;
		if (!out_.empty()) {
			y = out_.front();
			out_.pop_front();
		}
		out[i] = static_cast<s16>(std::lround(
			std::max(-32767.0f, std::min(32767.0f, y))));
		in_.push(in[i]);
		t_++;
		if (state_ == FAILED)
			continue;
		rx_run();
		if (ph_ == PH_LINK && out_.empty() && rx_ == RX_PRE &&
		    !hits_ && t_ > wait_until_) {
			logf("no turn from the far side: ours again");
			tx_turn();
		}
		if (ph_ == PH_LINK && heard_ &&
		    t_ - heard_ > s64{cfg_.timeout_s} * FS)
			fail("no packets");
	}
}

/*
 *    the transmitter
 */

/* X (by bin, 0 to 511) as len samples: the IFFT's 1024 again and again
   (an epoch's guard is its first 64 after them) */
void PepModem::tx_samples(const std::vector<cf> &X, s32 len)
{
	std::vector<cf> a(N, cf(0, 0));
	for (s16 k = 1; k <= CARRIERS; k++)
		a[static_cast<size_t>(k)] = X[static_cast<size_t>(k)];
	fft_.inverse(a.data());
	for (s32 i = 0; i < len; i++)
		out_.push_back(2 * scale_ * a[static_cast<size_t>(i % N)].real());
	tx_end_ = t_ + static_cast<s64>(out_.size());
	rx_ = RX_OFF;
	rx_from_ = tx_end_ + ECHO_GAP;
}

/* the two timing tones for len samples, in phase at the start */
void PepModem::tx_epoch_tones(s32 len)
{
	std::vector<cf> X(CARRIERS + 1, cf(0, 0));
	X[TONE1] = X[TONE2] = TONE_GAIN;
	tx_samples(X, len);
}

/* a comb epoch: every carrier (or those of mask) times sign, gain */
void PepModem::tx_epoch_comb(float sign, float gain,
			     const std::vector<u8> *mask)
{
	std::vector<cf> X(comb_);
	for (s16 k = 1; k <= CARRIERS; k++) {
		float s = gain;
		if (mask && (*mask)[static_cast<size_t>(k)])
			s = -s;
		X[static_cast<size_t>(k)] *= s * sign;
	}
	tx_samples(X, EPOCH);
}

/* an epoch of a packet's octets on l, the first bit the least
   significant, carrier by carrier up; the pilots at the comb's */
void PepModem::tx_epoch_data(const PepLoading &l, const std::vector<u8> &oct)
{
	std::vector<cf> X(CARRIERS + 1, cf(0, 0));
	s32 pos = 0, nbits = static_cast<s32>(oct.size()) * 8;
	for (s16 k = 1; k <= CARRIERS; k++) {
		u8 b = l.bits[static_cast<size_t>(k)];
		if (b == PILOT) {
			X[static_cast<size_t>(k)] = comb_[static_cast<size_t>(k)];
			continue;
		}
		if (!b)
			continue;
		u32 v = 0;
		for (s16 j = 0; j < b; j++, pos++)
			if (pos < nbits && (oct[static_cast<size_t>(pos / 8)] >>
					    (pos % 8) & 1))
				v |= 1u << j;
		X[static_cast<size_t>(k)] = VwbQam::map(b, v) *
			l.gain[static_cast<size_t>(k)];
	}
	tx_samples(X, EPOCH);
}

/* the bits of a micro turn's carriers (by short bin j, MS_LO to MS_HI)
   on loading l: 2 more than l's at bin 16 j, 6 at most, none where it
   has none */
void PepModem::micro_bits(const PepLoading &l, std::vector<u8> &b) const
{
	b.assign(MS_HI + 1, 0);
	for (s16 j = MS_LO; j <= MS_HI; j++) {
		u8 lb = l.bits[static_cast<size_t>(j * MS_STEP)];
		b[static_cast<size_t>(j)] = lb == 0 || lb == PILOT ? 0 :
			static_cast<u8>(std::min<s16>(MAX_BITS, lb + 2));
	}
}

/* (see micro_bits()) the octets of a micro turn's packet on l */
s32 PepModem::micro_octets(const PepLoading &l) const
{
	std::vector<u8> b;
	micro_bits(l, b);
	s32 n = 0;
	for (u8 v : b)
		n += v;
	return n * MS_DATA / 8;
}

/* a short epoch of X (by bin, only every 16th used): its period of 64
   (the IFFT's first 64), then their first 26 again */
void PepModem::tx_short(const std::vector<cf> &X)
{
	std::vector<cf> a(N, cf(0, 0));
	for (s16 k = MS_STEP; k <= CARRIERS; k += MS_STEP)
		a[static_cast<size_t>(k)] = X[static_cast<size_t>(k)];
	fft_.inverse(a.data());
	for (s32 i = 0; i < MS_EP; i++)
		out_.push_back(2 * scale_ * a[static_cast<size_t>(i % MS_N)].real());
	tx_end_ = t_ + static_cast<s64>(out_.size());
	rx_ = RX_OFF;
	rx_from_ = tx_end_ + ECHO_GAP;
}

/* a micro turn of the packet oct (its micro_octets(l)): the gap, the
   tones, the short comb, its data epochs */
void PepModem::tx_micro(const PepLoading &l, const std::vector<u8> &oct)
{
	std::vector<u8> b;
	micro_bits(l, b);
	out_.insert(out_.end(), static_cast<size_t>(TURN_GAP), 0.0f);
	tx_epoch_tones(PRE_SHORT);
	std::vector<cf> X(CARRIERS + 1, cf(0, 0));
	for (s16 j = MS_LO; j <= MS_HI; j++)
		X[static_cast<size_t>(j * MS_STEP)] = MS_BOOST *
			comb_[static_cast<size_t>(j * MS_STEP)];
	tx_short(X);
	s32 pos = 0, nbits = static_cast<s32>(oct.size()) * 8;
	for (s16 e = 0; e < MS_DATA; e++) {
		std::fill(X.begin(), X.end(), cf(0, 0));
		for (s16 j = MS_LO; j <= MS_HI; j++) {
			size_t k = static_cast<size_t>(j * MS_STEP);
			u8 nb = b[static_cast<size_t>(j)];
			if (!nb)
				continue;
			u32 v = 0;
			for (s16 i = 0; i < nb; i++, pos++)
				if (pos < nbits && (oct[static_cast<size_t>(
						pos / 8)] >> (pos % 8) & 1))
					v |= 1u << i;
			X[k] = VwbQam::map(nb, v) * l.gain[k] * MS_BOOST;
		}
		tx_short(X);
	}
}

/* the start-up's tones, combs comb epochs and phases phase epochs of
   mask (180 degrees on its carriers) */
void PepModem::tx_start_tones_comb(s16 combs, s16 phases,
				   const std::vector<u8> *mask)
{
	tx_epoch_tones(EPOCH);
	for (s16 i = 0; i < combs; i++)
		tx_epoch_comb(i & 1 ? 1 : -1, 1, nullptr);
	for (s16 i = 0; i < phases; i++)
		tx_epoch_comb(1, PHASE_GAIN, mask);
}

/* a packet of type with seq, n octets of pay, left epochs after it in
   the turn, our acknowledgements; in cap octets, scrambled */
std::vector<u8> PepModem::make_packet(u8 type, u8 seq, const u8 *pay, s32 n,
				      u8 left, s32 cap)
{
	std::vector<u8> p(static_cast<size_t>(cap), 0);
	p[0] = static_cast<u8>(type << 6 | (n >> 8 & 0x0f));
	p[1] = static_cast<u8>(n);
	p[2] = seq;
	p[3] = left;
	p[4] = rx_base_;
	u16 map = 0;
	for (s16 i = 0; i < 16; i++)
		if (rxq_.count(static_cast<u8>(rx_base_ + 1 + i)))
			map = static_cast<u16>(map | 1u << i);
	p[5] = static_cast<u8>(map);
	p[6] = static_cast<u8>(map >> 8);
	std::copy(pay, pay + n, p.begin() + HDR);
	u16 c = crc16(p.data(), static_cast<size_t>(HDR + n));
	p[static_cast<size_t>(HDR + n)] = static_cast<u8>(c >> 8);
	p[static_cast<size_t>(HDR + n + 1)] = static_cast<u8>(c);
	scramble(p.data(), p.size());
	return p;
}

/* our turn: what the far side has not acknowledged again, then control
   octets, then data (on our own loading once we have it), up to N
   packets; one packet of nothing at least */
void PepModem::tx_turn()
{
	const PepLoading &l = tx_final_on_ ? tx_final_ : link_;
	s32 cap = l.octets(), room = cap - OVERHEAD;
	if (room < 1) {
		fail("no carriers");
		return;
	}
	struct P {
		u8 type, seq;
		const std::vector<u8> *pay;
		std::vector<u8> fresh;
	};
	std::vector<P> ps;
	/* little to send (nothing again, no control, the source's data
	   short of a micro packet): a micro turn of it */
	s32 mcap = tx_final_on_ && have_tx_final_ && cfg_.micro ?
		micro_octets(tx_final_) : 0;
	std::vector<u8> first;
	bool pulled = false;
	if (mcap > OVERHEAD && outq_.empty() && ctrl_out_.empty() &&
	    seq_diff(tx_seq_, tx_base_) < WINDOW - 1) {
		s32 mroom = mcap - OVERHEAD;
		first.resize(static_cast<size_t>(room));
		s32 n = src_ ? src_(first.data(), mroom) : 0;
		first.resize(static_cast<size_t>(std::max(n, 0)));
		pulled = true;
		if (n < mroom) {
			u8 type = PK_FILL, sq = 0;
			const u8 *d = nullptr;
			if (n > 0) {
				sq = tx_seq_++;
				auto &kept = outq_[sq] = Out{ PK_DATA, first };
				type = PK_DATA;
				d = kept.pay.data();
			}
			tx_micro(tx_final_, make_packet(type, sq, d, n, 0,
							mcap));
			turns_tx_++;
			micro_tx_++;
			wait_until_ = tx_end_ + (cfg_.calling ? WAIT_CALLER :
						 WAIT_ANSWERER);
			hits_ = 0;
			return;
		}
	}
	/* again, oldest first */
	for (u8 i = 0; i < WINDOW && ps.size() < static_cast<size_t>(
				 cfg_.turn_max); i++) {
		auto it = outq_.find(static_cast<u8>(tx_base_ + i));
		if (it == outq_.end())
			continue;
		if (static_cast<s32>(it->second.pay.size()) > room)
			continue;
		ps.push_back(P{ it->second.type, it->first, &it->second.pay,
				{} });
		pk_again_++;
	}
	/* new */
	while (ps.size() < static_cast<size_t>(cfg_.turn_max) &&
	       seq_diff(tx_seq_, tx_base_) < WINDOW - 1) {
		Out o;
		if (!ctrl_out_.empty()) {
			s32 n = std::min(room, static_cast<s32>(
						 ctrl_out_.size()));
			o.type = PK_CTRL;
			o.pay.assign(ctrl_out_.begin(), ctrl_out_.begin() + n);
			ctrl_out_.erase(ctrl_out_.begin(), ctrl_out_.begin() + n);
		} else if (tx_final_on_ && src_) {
			/* (what the micro turn's test took first, topped
			   up) */
			s32 have = 0;
			if (pulled) {
				o.pay = std::move(first);
				have = static_cast<s32>(o.pay.size());
				pulled = false;
			}
			o.pay.resize(static_cast<size_t>(room));
			s32 n = have + std::max<s32>(0, src_(o.pay.data() +
							       have,
							       room - have));
			if (n <= 0)
				break;
			o.pay.resize(static_cast<size_t>(n));
			o.type = PK_DATA;
		} else
			break;
		u8 s = tx_seq_++;
		auto &kept = outq_[s] = std::move(o);
		ps.push_back(P{ kept.type, s, &kept.pay, {} });
	}
	if (ps.empty())
		ps.push_back(P{ PK_FILL, 0, nullptr, {} });

	out_.insert(out_.end(), static_cast<size_t>(TURN_GAP), 0.0f);
	tx_epoch_tones(PRE_LEN);
	tx_epoch_comb(tx_final_on_ ? -1 : 1, 1, nullptr);
	for (size_t i = 0; i < ps.size(); i++) {
		const P &p = ps[i];
		const u8 *d = p.pay ? p.pay->data() : nullptr;
		s32 n = p.pay ? static_cast<s32>(p.pay->size()) : 0;
		tx_epoch_data(l, make_packet(p.type, p.seq, d, n,
					     static_cast<u8>(ps.size() - 1 - i),
					     cap));
	}
	turns_tx_++;
	wait_until_ = tx_end_ + (cfg_.calling ? WAIT_CALLER : WAIT_ANSWERER);
	hits_ = 0;
}

/*
 *    the receiver
 */

/* the next sample's work: the state's, while it has what it needs */
void PepModem::rx_run()
{
	for (;;) {
		Rx before = rx_;
		switch (rx_) {
		case RX_OFF:
			if (t_ < rx_from_ || !out_.empty())
				return;
			rx_scan_ = rx_from_;
			hits_ = 0;
			if (ph_ == PH_A_SEND) {
				nacc_.assign(CARRIERS + 1, 0);
				nacc_n_ = 0;
				at_ = static_cast<double>(tx_end_ + NOISE_GAP);
				rx_ = RX_NOISE;
				break;
			}
			if (ph_ == PH_C_SEND)
				ph_ = PH_C_WAIT6;
			rx_ = RX_PRE;
			break;
		case RX_TONES:
		case RX_PRE: {
			if (t_ < rx_scan_ + BLK)
				return;
			float ratio = 0;
			double e = 0;
			bool on = tones_found(rx_scan_, &ratio, &e);
			/* the line's quiet level, followed down at once and
			   up slowly; well above it, the far side is in a
			   turn whose tones we missed: ours waits for it */
			floor_ = floor_ <= 0 || e < floor_ ? e :
				 std::min(e, floor_ * FLOOR_RISE);
			if (ph_ == PH_LINK && e > BUSY * floor_)
				wait_until_ = std::max(wait_until_, rx_scan_ +
					BLK + (cfg_.calling ? WAIT_CALLER :
					       WAIT_ANSWERER));
			if (hits_ < TONES_HITS) {
				hits_ = on ? hits_ + 1 : 0;
			} else if (ratio < TONES_OFF) {
				/* the tones ended in this block: what
				   follows them starts about at its middle */
				at_ = static_cast<double>(rx_scan_ + BLK / 2 -
							  37);
				hits_ = 0;
				if (rx_ == RX_TONES) {
					rx_ = RX_COMB;
					far_heard_ = true;
				} else {
					/* a micro turn, if the far side
					   may send one, else (or if it
					   is not) a long one's comb */
					rx_ = rx_final_.total > 0 &&
						ph_ == PH_LINK ? RX_MICRO :
								 RX_SYNC;
				}
			}
			rx_scan_ += HOP;
			break;
		}
		case RX_COMB:
			if (t_ < static_cast<s64>(at_) + COMBS * EPOCH + 2 * GUARD)
				return;
			rx_comb_epochs();
			break;
		case RX_NOISE:
			if (t_ < static_cast<s64>(at_) + NOISES * N)
				return;
			rx_noise();
			break;
		case RX_PHASE:
			if (t_ < static_cast<s64>(at_ + PHASES * EPOCH * r_) +
				 2 * GUARD)
				return;
			rx_phase_epochs();
			break;
		case RX_SYNC:
			if (t_ < static_cast<s64>(at_) + EPOCH + 2 * GUARD)
				return;
			rx_turn_sync();
			break;
		case RX_EPOCHS:
			if (t_ < static_cast<s64>(at_) + EPOCH + 2 * GUARD)
				return;
			rx_turn_epoch();
			break;
		case RX_MICRO:
			if (t_ < static_cast<s64>(at_) + MS_SCAN_HI + MS_LEN +
				 2 * GUARD)
				return;
			if (rx_micro())
				rx_turn_end();
			else
				rx_ = RX_SYNC;
			break;
		}
		if (rx_ == before && rx_ != RX_TONES && rx_ != RX_PRE)
			return;
		if (state_ == FAILED)
			return;
	}
}

/* the block of BLK samples at at: whether most of its power is in the
   two tones (the share in *ratio) */
bool PepModem::tones_found(s64 at, float *ratio, double *energy) const
{
	cf a(0, 0), b(0, 0);
	double e = 0;
	for (s32 n = 0; n < BLK; n++) {
		float x = in_(at + n);
		e += static_cast<double>(x) * x;
		a += x * std::polar(1.0f, -TWO_PI_F * 23 * n / BLK);
		b += x * std::polar(1.0f, -TWO_PI_F * 27 * n / BLK);
	}
	*energy = e;
	if (e < 100.0 * BLK) {
		*ratio = 0;
		return false;
	}
	*ratio = static_cast<float>(2 * (std::norm(a) + std::norm(b)) /
				    (BLK * e));
	return *ratio > TONES_ON;
}

/* the epoch starting at line time at: its window's FFT, bins 0 to 511,
   as an epoch on time gives it (the window's place in the guard and its
   fraction of a sample taken out) */
void PepModem::window(double at, cf *Y) const
{
	window(at, Y, r_);
}

/* (see window()) on the far clock of r of our samples to its one: the
   window's samples interpolated where its clock has them */
void PepModem::window(double at, cf *Y, double r) const
{
	std::vector<cf> y(N);
	auto x = [this](s64 i) { return in_(i); };
	for (s32 n = 0; n < N; n++)
		y[static_cast<size_t>(n)] = cf(interp_.at(x, at + (W_OFF + n) * r),
					       0);
	fft_.forward(y.data());
	for (s16 k = 0; k <= CARRIERS; k++)
		Y[k] = y[static_cast<size_t>(k)] * std::polar(1.0f,
			static_cast<float>(-TWO_PI * k * W_OFF / N));
}

/* the short epoch at line time at: its bins every 16th (the rest 0),
   as window() gives a long one's (the same scale, from its start) */
void PepModem::window_short(double at, cf *Y) const
{
	auto x = [this](s64 i) { return in_(i); };
	float y[MS_N];
	for (s32 n = 0; n < MS_N; n++)
		y[n] = interp_.at(x, at + (MS_W + n) * r_);
	for (s16 k = 0; k <= CARRIERS; k++)
		Y[k] = cf(0, 0);
	for (s16 j = MS_LO; j <= MS_HI; j++) {
		cf c(0, 0);
		for (s32 n = 0; n < MS_N; n++)
			c += y[n] * std::polar(1.0f, static_cast<float>(
				-TWO_PI * j * (n + MS_W) / MS_N));
		Y[j * MS_STEP] = c * static_cast<float>(N / MS_N);
	}
}

/* a micro turn's packet at about at_, if it is one: the short comb's
   timing against our channel (its phase's slope over the carriers,
   twice), the data epochs decoded on rx_final_; whether its CRC is
   right (then it is taken, the turn over) */
bool PepModem::rx_micro()
{
	std::vector<cf> Y(CARRIERS + 1);
	/* the short comb's coherence (its adjacent carriers' phase
	   steps, together against their power) with the window from
	   at, and the step's mean as samples late */
	auto fit = [&](double at, double *d) {
		window_short(at, Y.data());
		std::complex<double> sum(0, 0);
		double e = 0;
		cf prev(0, 0);
		for (s16 j = MS_LO; j <= MS_HI; j++) {
			size_t k = static_cast<size_t>(j * MS_STEP);
			cf c = Y[k] * std::conj(comb_[k] * H_[k]);
			if (j > MS_LO)
				sum += std::complex<double>(c * std::conj(prev));
			e += std::norm(c);
			prev = c;
		}
		*d = std::arg(sum) * MS_N / TWO_PI;
		return e > 0 ? std::abs(sum) / e : 0.0;
	};
	/* the tones' end is only about where a micro turn's comb starts:
	   the best of a scan, then its timing refined */
	double at = at_, best = 0, d = 0;
	for (s32 o = MS_SCAN_LO; o <= MS_SCAN_HI; o += MS_SCAN_STEP) {
		double q = fit(at_ + o, &d);
		if (q > best) {
			best = q;
			at = at_ + o;
		}
	}
	if (best < MS_COHERENT)
		return false;
	for (s16 it = 0; it < 3; it++) {
		fit(at, &d);
		at -= d;
		if (std::fabs(d) < 0.05)
			break;
	}
	/* the common phase from the comb */
	window_short(at, Y.data());
	std::complex<double> cp(0, 0);
	for (s16 j = MS_LO; j <= MS_HI; j++) {
		size_t k = static_cast<size_t>(j * MS_STEP);
		cp += std::complex<double>(Y[k] * std::conj(comb_[k] * H_[k] *
							   MS_BOOST));
	}
	cf rot = std::polar(1.0f, static_cast<float>(-std::arg(cp)));
	std::vector<u8> b;
	micro_bits(rx_final_, b);
	s32 n = micro_octets(rx_final_);
	std::vector<u8> oct(static_cast<size_t>(n), 0);
	s32 pos = 0, nbits = n * 8;
	for (s16 e = 0; e < MS_DATA; e++) {
		window_short(at + (1 + e) * MS_EP * r_, Y.data());
		for (s16 j = MS_LO; j <= MS_HI; j++) {
			size_t k = static_cast<size_t>(j * MS_STEP);
			u8 nb = b[static_cast<size_t>(j)];
			if (!nb)
				continue;
			cf z = Y[k] / H_[k] * rot /
				(rx_final_.gain[k] * MS_BOOST);
			u32 v = VwbQam::slice(nb, z);
			for (s16 i = 0; i < nb; i++, pos++)
				if (pos < nbits && (v >> i & 1))
					oct[static_cast<size_t>(pos / 8)] |=
						static_cast<u8>(1 << (pos % 8));
		}
	}
	scramble(oct.data(), oct.size());
	/* not a micro packet: no count of a bad one (the long turn's
	   comb is tried next) */
	s64 bad = pk_bad_;
	bool ok = rx_packet(oct);
	if (!ok) {
		pk_bad_ = bad;
		return false;
	}
	micro_rx_++;
	rx_got_ = 1;
	if (!rx_final_seen_) {
		rx_final_seen_ = true;
		check_up();
	}
	logf("turn: micro at %.2f (%+.2f)", at, at - at_);
	return true;
}

/* a comb epoch's lateness (samples): the slope of its phase over the
   carriers (against H's, if given); C, if given, the comb taken out */
double PepModem::comb_fit(const cf *Y, const std::vector<cf> *H, cf *C) const
{
	std::vector<cf> c(CARRIERS + 1);
	for (s16 k = 1; k <= CARRIERS; k++) {
		c[static_cast<size_t>(k)] = Y[k] *
			std::conj(comb_[static_cast<size_t>(k)]);
		if (C)
			C[k] = c[static_cast<size_t>(k)];
		if (H)
			c[static_cast<size_t>(k)] *=
				std::conj((*H)[static_cast<size_t>(k)]);
	}
	std::complex<double> s(0, 0);
	for (s16 k = 2; k <= CARRIERS; k++)
		s += std::complex<double>(c[static_cast<size_t>(k)] *
					  std::conj(c[static_cast<size_t>(k - 1)]));
	return std::arg(s) * N / TWO_PI;
}

/* the start-up's comb (COMBS epochs from about at_): its timing, the far
   clock's against ours, the channel */
void PepModem::rx_comb_epochs()
{
	std::vector<cf> Y(CARRIERS + 1);
	double a0 = at_, r = 1;
	/* twice: the first from the epochs after the first (the tones
	   before it), the second from all on the first's grid */
	for (s16 pass = 0; pass < 2; pass++) {
		double sj = 0, sd = 0, sjj = 0, sjd = 0;
		s16 m = 0;
		for (s16 j = pass ? 0 : 1; j < COMBS; j++) {
			window(a0 + j * EPOCH * r, Y.data(), r);
			double d = comb_fit(Y.data(), nullptr, nullptr);
			sj += j;
			sd += d;
			sjj += j * j;
			sjd += j * d;
			m++;
		}
		double slope = (m * sjd - sj * sd) / (m * sjj - sj * sj);
		double d0 = (sd - slope * sj) / m;
		a0 -= d0;
		r *= 1 - slope / (EPOCH * r);
	}
	at_ = a0;
	r_ = r;
	/* the channel: the epochs' mean, their signs (- + - +: the last
	   one's echo in the next epoch on its side) taken out;
	   what is left of them, the noise with the echoes of each epoch in
	   the next (more than the guard takes) */
	std::vector<std::vector<cf>> Cs(COMBS, std::vector<cf>(CARRIERS + 1));
	std::fill(H_.begin(), H_.end(), cf(0, 0));
	for (s16 j = 0; j < COMBS; j++) {
		window(at_ + j * EPOCH * r_, Y.data());
		float sg = j & 1 ? 1.0f : -1.0f;
		for (s16 k = 1; k <= CARRIERS; k++) {
			cf c = sg * Y[k] * std::conj(comb_[static_cast<size_t>(k)]);
			Cs[static_cast<size_t>(j)][static_cast<size_t>(k)] = c;
			H_[static_cast<size_t>(k)] += c / static_cast<float>(COMBS);
		}
	}
	std::vector<double> d(CARRIERS + 1, 0);
	for (s16 k = 1; k <= CARRIERS; k++)
		for (s16 j = 0; j < COMBS; j++)
			d[static_cast<size_t>(k)] += std::norm(
				Cs[static_cast<size_t>(j)][static_cast<size_t>(k)] -
				H_[static_cast<size_t>(k)]) / (COMBS - 1);
	/* smoothed over 9 carriers: few epochs make a rough measure */
	dist_.assign(CARRIERS + 1, 0);
	for (s16 k = 1; k <= CARRIERS; k++) {
		double s = 0;
		s16 m = 0;
		for (s16 i = -4; i <= 4; i++)
			if (k + i >= 1 && k + i <= CARRIERS) {
				s += d[static_cast<size_t>(k + i)];
				m++;
			}
		dist_[static_cast<size_t>(k)] = s / m;
	}
	logf("comb at %.2f, far clock %+.1f ppm", at_, (r_ - 1) * 1e6);
	if (ph_ == PH_C_LISTEN) {
		nacc_.assign(CARRIERS + 1, 0);
		nacc_n_ = 0;
		at_ += COMBS * EPOCH * r_ + NOISE_GAP;
		rx_ = RX_NOISE;
	} else {
		phase_acc_.assign(CARRIERS + 1, cf(0, 0));
		at_ += COMBS * EPOCH * r_;
		rx_ = RX_PHASE;
	}
}

/* NOISES windows of silence from at_: the noise by carrier; then the
   caller sends its tones, comb and phase epochs, the answerer listens
   for them */
void PepModem::rx_noise()
{
	std::vector<cf> y(N);
	for (s16 i = 0; i < NOISES; i++) {
		s64 w = static_cast<s64>(at_) + i * N;
		for (s32 n = 0; n < N; n++)
			y[static_cast<size_t>(n)] = cf(in_(w + n), 0);
		fft_.forward(y.data());
		for (s16 k = 1; k <= CARRIERS; k++)
			nacc_[static_cast<size_t>(k)] +=
				std::norm(y[static_cast<size_t>(k)]) / NOISES;
	}
	/* a floor: the receiver's own quantisation */
	for (s16 k = 1; k <= CARRIERS; k++)
		nacc_[static_cast<size_t>(k)] =
			std::max(nacc_[static_cast<size_t>(k)], 1.0 * N);
	if (ph_ == PH_A_SEND) {
		ph_ = PH_A_LISTEN;
		rx_scan_ = static_cast<s64>(at_) + NOISES * N;
		rx_ = RX_TONES;
		hits_ = 0;
		return;
	}
	/* the caller: its SNRs are the answerer's way's */
	noise_db_.assign(CARRIERS + 1, 0);
	for (s16 k = 1; k <= CARRIERS; k++)
		noise_db_[static_cast<size_t>(k)] = static_cast<float>(
			10 * std::log10(noise(k) /
					std::max(1e-9f, std::norm(
						H_[static_cast<size_t>(k)]))));
	mask_far_ = capable(2);
	s32 n = 0;
	for (u8 b : mask_far_)
		n += b;
	logf("caller: %d carriers for 2 bits the answerer's way; tones, "
	     "comb and phases", n);
	ph_ = PH_C_SEND;
	tx_start_tones_comb(COMBS, PHASES, &mask_far_);
}

/* the answerer: the caller's phase epochs, the carriers it can take 2
   bits on our way; those we can take them on its way too are the
   link's: our turn of them */
void PepModem::rx_phase_epochs()
{
	std::vector<cf> Y(CARRIERS + 1);
	for (s16 p = 0; p < PHASES; p++) {
		window(at_ + p * EPOCH * r_, Y.data());
		for (s16 k = 1; k <= CARRIERS; k++)
			phase_acc_[static_cast<size_t>(k)] += Y[k] *
				std::conj(comb_[static_cast<size_t>(k)] *
					  H_[static_cast<size_t>(k)]);
	}
	noise_db_.assign(CARRIERS + 1, 0);
	for (s16 k = 1; k <= CARRIERS; k++)
		noise_db_[static_cast<size_t>(k)] = static_cast<float>(
			10 * std::log10(noise(k) /
					std::max(1e-9f, std::norm(
						H_[static_cast<size_t>(k)]))));
	std::vector<u8> ours = capable(2);
	mask_both_.assign(CARRIERS + 1, 0);
	s32 n = 0;
	for (s16 k = 1; k <= CARRIERS; k++) {
		bool far = phase_bit(k);
		mask_both_[static_cast<size_t>(k)] =
			far && ours[static_cast<size_t>(k)];
		n += mask_both_[static_cast<size_t>(k)];
	}
	logf("answerer: %d carriers for 2 bits both ways", n);
	start_link();
	out_.insert(out_.end(), static_cast<size_t>(TURN_GAP), 0.0f);
	tx_epoch_tones(PRE_LEN);
	tx_epoch_comb(1, 1, nullptr);
	for (s16 p = 0; p < PHASES; p++)
		tx_epoch_comb(1, PHASE_GAIN, &mask_both_);
	wait_until_ = tx_end_ + WAIT_ANSWERER;
}

/* a turn's comb epoch at about at_: its timing against our channel,
   its sign (which loading the turn's packets are on), the channel
   brought up to date */
void PepModem::rx_turn_sync()
{
	std::vector<cf> Y(CARRIERS + 1), C(CARRIERS + 1);
	window(at_, Y.data());
	double d0 = comb_fit(Y.data(), &H_, nullptr), d = d0;
	at_ -= d;
	for (s16 i = 0; i < 4; i++) {
		window(at_, Y.data());
		d = comb_fit(Y.data(), &H_, C.data());
		at_ -= d;
		if (std::fabs(d) < 0.02)
			break;
	}
	std::complex<double> s(0, 0);
	for (s16 k = 1; k <= CARRIERS; k++)
		s += std::complex<double>(C[k] *
					  std::conj(H_[static_cast<size_t>(k)]));
	float sign = s.real() < 0 ? -1.0f : 1.0f;
	/* the round trip: from the end of our last turn to the start of
	   this one, less its silence */
	if (tx_end_ > 0) {
		double rt = at_ - PRE_LEN - TURN_GAP - static_cast<double>(tx_end_);
		if (rt > 0 && rt < 4 * FS)
			rtd_ = rtd_ > 0 ? 0.8 * rtd_ + 0.2 * rt : rt;
	}
	logf("turn: comb at %.2f (%+.2f %+.2f), %s", at_, d0, d, sign < 0 ? "final" :
	     "link");
	rx_final_on_ = sign < 0;
	for (s16 k = 1; k <= CARRIERS; k++)
		H_[static_cast<size_t>(k)] = 0.5f * H_[static_cast<size_t>(k)] +
			0.5f * sign * C[k];
	at_ += EPOCH * r_;
	rx_left_ = ph_ == PH_C_WAIT6 ? PHASES :
		   static_cast<s16>(cfg_.turn_max);
	rx_got_ = 0;
	phase_acc_.assign(CARRIERS + 1, cf(0, 0));
	rx_ = RX_EPOCHS;
}

/* Z's points on l: their octets (the first bit the least significant)
   and, if dec, the points decided (the pilots' the comb's) */
void PepModem::decode(const cf *Z, const PepLoading &l, std::vector<u8> &oct,
		      std::vector<cf> *dec) const
{
	oct.assign(static_cast<size_t>(l.octets()), 0);
	s32 pos = 0, nbits = l.octets() * 8;
	if (dec)
		dec->assign(CARRIERS + 1, cf(0, 0));
	for (s16 k = 1; k <= CARRIERS; k++) {
		u8 b = l.bits[static_cast<size_t>(k)];
		if (b == PILOT) {
			if (dec)
				(*dec)[static_cast<size_t>(k)] =
					comb_[static_cast<size_t>(k)];
			continue;
		}
		if (!b)
			continue;
		float g = l.gain[static_cast<size_t>(k)];
		u32 v = VwbQam::slice(b, Z[k] / g);
		if (dec)
			(*dec)[static_cast<size_t>(k)] = VwbQam::map(b, v) * g;
		for (s16 j = 0; j < b; j++, pos++)
			if (pos < nbits && (v >> j & 1))
				oct[static_cast<size_t>(pos / 8)] |=
					static_cast<u8>(1 << (pos % 8));
	}
}

/* one of a turn's epochs at about at_: a packet (or the answerer's
   phase epoch); its timing and the channel followed by its points */
void PepModem::rx_turn_epoch()
{
	std::vector<cf> Y(CARRIERS + 1), Z(CARRIERS + 1), dec;
	window(at_, Y.data());
	/* nothing there: the turn is over */
	double e = 0, h = 0;
	for (s16 k = 1; k <= CARRIERS; k++) {
		e += std::norm(Y[k]);
		h += std::norm(H_[static_cast<size_t>(k)]);
	}
	if (e < 0.1 * h) {
		logf("turn: nothing at %.2f (%.3f)", at_, e / h);
		rx_turn_end();
		return;
	}
	if (ph_ == PH_C_WAIT6) {
		for (s16 k = 1; k <= CARRIERS; k++)
			phase_acc_[static_cast<size_t>(k)] += Y[k] *
				std::conj(comb_[static_cast<size_t>(k)] *
					  H_[static_cast<size_t>(k)]);
		at_ += EPOCH * r_;
		if (--rx_left_ <= 0) {
			/* the answerer's word is the link's: on every
			   carrier we can tell its phase on (1 bit), not
			   only on those we offered */
			mask_both_.assign(CARRIERS + 1, 0);
			std::vector<u8> bpsk = capable(1);
			s32 n = 0;
			for (s16 k = 1; k <= CARRIERS; k++) {
				mask_both_[static_cast<size_t>(k)] =
					bpsk[static_cast<size_t>(k)] &&
					phase_bit(k);
				n += mask_both_[static_cast<size_t>(k)];
			}
			logf("caller: %d carriers for 2 bits both ways", n);
			start_link();
			tx_turn();
		}
		return;
	}
	const PepLoading &l = rx_final_on_ ? rx_final_ : link_;

	/* the timing and phase from the decisions (twice: first those of
	   the carriers of 4 bits or fewer, then all); the epoch comes where
	   the last one said, so they start at 0 */
	double dt = 0, ph = 0;
	std::vector<u8> oct;
	for (s16 it = 0; it < 3; it++) {
		for (s16 k = 1; k <= CARRIERS; k++)
			Z[k] = Y[k] / H_[static_cast<size_t>(k)] *
				std::polar(1.0f, static_cast<float>(
					-ph - TWO_PI * k * dt / N));
		decode(Z.data(), l, oct, &dec);
		if (it == 2)
			break;
		/* the phase errors' line over the carriers, by least
		   squares weighted by the points' power */
		double sw = 0, sk = 0, skk = 0, se = 0, ske = 0;
		for (s16 k = 1; k <= CARRIERS; k++) {
			cf dk = dec[static_cast<size_t>(k)];
			u8 b = l.bits[static_cast<size_t>(k)];
			if (std::norm(dk) == 0 || (!it && b > 4 && b != PILOT))
				continue;
			double er = std::arg(Z[k] * std::conj(dk));
			if (std::fabs(er) > 0.6)
				continue;
			double w = std::norm(dk);
			sw += w;
			sk += w * k;
			skk += w * k * k;
			se += w * er;
			ske += w * k * er;
		}
		double den = sw * skk - sk * sk;
		if (sw <= 0 || std::fabs(den) < 1e-9)
			break;
		double slope = (sw * ske - sk * se) / den;
		double icpt = (se - slope * sk) / sw;
		dt += slope * N / TWO_PI;
		ph += icpt;
	}
	/* the channel and the clock followed */
	double err = 0, sig = 0;
	for (s16 k = 1; k <= CARRIERS; k++) {
		cf dk = dec[static_cast<size_t>(k)];
		if (std::norm(dk) == 0)
			continue;
		cf q = Z[k] / dk;
		H_[static_cast<size_t>(k)] *= std::polar(1.0f,
			static_cast<float>(ph)) * (1.0f + 0.1f * (q - 1.0f));
		err += std::norm(Z[k] - dk);
		sig += std::norm(dk);
	}
	if (sig > 0 && err > 0)
		snr_db_ = static_cast<float>(10 * std::log10(sig / err));
	if (getenv("DSP_PEP_BANDS")) {
		std::string s;
		for (s16 b0 = 1; b0 <= CARRIERS; b0 += 64) {
			double e2 = 0, s2 = 0;
			for (s16 k = b0; k < b0 + 64 && k <= CARRIERS; k++) {
				cf dk = dec[static_cast<size_t>(k)];
				if (std::norm(dk) == 0)
					continue;
				e2 += std::norm(Z[k] - dk);
				s2 += std::norm(dk);
			}
			char t[16];
			snprintf(t, sizeof(t), " %.0f", s2 > 0 && e2 > 0 ?
				 10 * std::log10(s2 / e2) : 0.0);
			s += t;
		}
		logf("epoch %d: dt %+.3f ph %+.3f SNR by 64 bins:%s", rx_got_,
		     dt, ph, s.c_str());
	}
	/* late by dt: the next window earlier, the far clock faster */
	r_ -= 0.05 * dt / EPOCH;
	at_ -= 0.5 * dt;
	rx_got_++;

	scramble(oct.data(), oct.size());
	bool ok = rx_packet(oct);
	at_ += EPOCH * r_;
	if (ok) {
		rx_left_ = static_cast<s16>(oct[3]);
		if (rx_final_on_ && !rx_final_seen_) {
			rx_final_seen_ = true;
			check_up();
		}
	} else
		rx_left_--;
	if (rx_left_ <= 0)
		rx_turn_end();
}

/* a packet's octets (descrambled): taken if its CRC is right (its
   acknowledgements, its payload in order); whether it was */
bool PepModem::rx_packet(const std::vector<u8> &oct)
{
	if (static_cast<s32>(oct.size()) < OVERHEAD) {
		pk_bad_++;
		return false;
	}
	s32 n = (oct[0] & 0x0f) << 8 | oct[1];
	u8 type = oct[0] >> 6;
	if (HDR + n + 2 > static_cast<s32>(oct.size()) || type > PK_CTRL) {
		pk_bad_++;
		return false;
	}
	u16 c = static_cast<u16>(oct[static_cast<size_t>(HDR + n)] << 8 |
				 oct[static_cast<size_t>(HDR + n + 1)]);
	if (crc16(oct.data(), static_cast<size_t>(HDR + n)) != c) {
		pk_bad_++;
		return false;
	}
	pk_good_++;
	heard_ = t_;

	/* what the far side has */
	u8 ack = oct[4];
	u16 map = static_cast<u16>(oct[5] | oct[6] << 8);
	if (in_window(ack, tx_base_) || ack == static_cast<u8>(tx_base_ +
								WINDOW)) {
		while (tx_base_ != ack) {
			outq_.erase(tx_base_);
			tx_base_++;
		}
	}
	for (s16 i = 0; i < 16; i++)
		if (map >> i & 1)
			outq_.erase(static_cast<u8>(ack + 1 + i));
	while (tx_base_ != tx_seq_ && !outq_.count(tx_base_))
		tx_base_++;

	if (type == PK_FILL)
		return true;
	u8 seq = oct[2];
	if (!in_window(seq, rx_base_) || rxq_.count(seq))
		return true;
	Out o;
	o.type = type;
	o.pay.assign(oct.begin() + HDR, oct.begin() + HDR + n);
	rxq_[seq] = std::move(o);
	for (;;) {
		auto it = rxq_.find(rx_base_);
		if (it == rxq_.end())
			break;
		if (it->second.type == PK_DATA) {
			if (sink_ && !it->second.pay.empty())
				sink_(it->second.pay.data(), static_cast<s32>(
					      it->second.pay.size()));
		} else
			ctrl_in(it->second.pay.data(),
				static_cast<s32>(it->second.pay.size()));
		rxq_.erase(it);
		rx_base_++;
	}
	return true;
}

/* the phase epochs' bit of carrier k (phase_acc_ against our channel):
   180 degrees, clearly */
bool PepModem::phase_bit(s16 k) const
{
	double want = PHASES * PHASE_GAIN *
		std::norm(H_[static_cast<size_t>(k)]);
	return phase_acc_[static_cast<size_t>(k)].real() < -0.5 * want;
}

/* the far side's turn is over: ours */
void PepModem::rx_turn_end()
{
	logf("turn: %d epochs, packets %lld good %lld bad in all, SNR %.1f "
	     "dB, out %zu", rx_got_, static_cast<long long>(pk_good_),
	     static_cast<long long>(pk_bad_), static_cast<double>(snr_db_),
	     outq_.size());
	turns_rx_++;
	rx_ = RX_OFF;
	rx_from_ = t_;
	tx_turn();
}

/*
 *    the start-up's decisions
 */

/* see pep.h: a tone of amplitude a is a bin of a N / 2, its power
   a^2 / 2, so a bin's power p is the line's 2 p / N^2 */
float PepModem::noise_dbm0(s16 k) const
{
	if (nacc_.empty() || k < 1 || k > CARRIERS)
		return 0;
	return static_cast<float>(10 * std::log10(
		2 * noise(k) / (static_cast<double>(N) * N) / DP_DBM0_POWER +
		1e-12));
}

/* carrier k's noise: the silence's, or the comb's own unsteadiness if
   more */
double PepModem::noise(s16 k) const
{
	return std::max(nacc_[static_cast<size_t>(k)],
			dist_.empty() ? 0.0 : dist_[static_cast<size_t>(k)]);
}

/* by bin, whether a carrier takes b bits at the comb's power with the
   margin, from our channel and noise */
std::vector<u8> PepModem::capable(s16 b) const
{
	std::vector<u8> m(CARRIERS + 1, 0);
	float need = VwbQam::snr_needed(b) *
		std::pow(10.0f, cfg_.margin_db / 10);
	for (s16 k = 1; k <= CARRIERS; k++) {
		if (k == TONE1 || k == TONE2)
			continue;
		double snr = std::norm(H_[static_cast<size_t>(k)]) / noise(k);
		m[static_cast<size_t>(k)] = snr >= need;
	}
	return m;
}

/* Hughes-Hartogs: the bits of each carrier our way and its power, the
   next bits always on the carrier where they cost the least power,
   until the comb's power is spent; the powers rounded to the loading
   record's 0.25 dB steps */
PepLoading PepModem::allocate() const
{
	PepLoading l;
	l.clear();
	float margin = std::pow(10.0f, cfg_.margin_db / 10);
	std::vector<float> snr(CARRIERS + 1, 0);
	std::vector<s16> step(CARRIERS + 1, 0);
	std::vector<float> pw(CARRIERS + 1, 0);
	using Q = std::pair<float, s16>;     /* power a bit, carrier */
	std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
	s16 nsteps = 1;
	while (nsteps < 5 && STEPS[nsteps] <= cfg_.max_bits)
		nsteps++;
	auto need = [&](s16 k, s16 s) {
		return VwbQam::snr_needed(STEPS[s]) * margin /
			snr[static_cast<size_t>(k)];
	};
	for (s16 k = 1; k <= CARRIERS; k++) {
		if (k == TONE1 || k == TONE2)
			continue;
		snr[static_cast<size_t>(k)] = static_cast<float>(
			std::norm(H_[static_cast<size_t>(k)]) / noise(k));
		if (nsteps > 1 && need(k, 1) <= MAX_POWER)
			q.push(Q(need(k, 1) / STEPS[1], k));
	}
	float budget = CARRIERS - 2;
	while (!q.empty()) {
		Q top = q.top();
		q.pop();
		s16 k = top.second;
		s16 s = static_cast<s16>(step[static_cast<size_t>(k)] + 1);
		float p = need(k, s), dp = p - pw[static_cast<size_t>(k)];
		if (dp > budget)
			break;
		budget -= dp;
		pw[static_cast<size_t>(k)] = p;
		step[static_cast<size_t>(k)] = s;
		if (s + 1 < nsteps && need(k, s + 1) <= MAX_POWER)
			q.push(Q((need(k, s + 1) - p) /
				 (STEPS[s + 1] - STEPS[s]), k));
	}
	for (s16 k = 1; k <= CARRIERS; k++) {
		s16 s = step[static_cast<size_t>(k)];
		if (!s)
			continue;
		float db = 10 * std::log10(pw[static_cast<size_t>(k)]);
		s32 code = std::min(63, std::max(0, static_cast<s32>(
				std::lround((db + 8) / 0.25f))));
		l.bits[static_cast<size_t>(k)] = static_cast<u8>(STEPS[s]);
		l.gain[static_cast<size_t>(k)] = std::pow(10.0f,
			(-8 + 0.25f * code) / 20);
	}
	l.count();
	return l;
}

/* l as the loading record: 'L', then for each carrier from 1 its bits
   (4) and its power (6: -8 dB and 0.25 dB a step), the first bit the
   least significant */
void PepModem::load_bytes(const PepLoading &l, std::vector<u8> &b) const
{
	b.assign(static_cast<size_t>(LOAD_LEN), 0);
	b[0] = 'L';
	s32 pos = 8;
	auto put = [&](u32 v, s16 n) {
		for (s16 i = 0; i < n; i++, pos++)
			if (v >> i & 1)
				b[static_cast<size_t>(pos / 8)] |=
					static_cast<u8>(1 << (pos % 8));
	};
	for (s16 k = 1; k <= CARRIERS; k++) {
		u8 bits = l.bits[static_cast<size_t>(k)];
		float g = l.gain[static_cast<size_t>(k)];
		u32 code = 0;
		if (g > 0)
			code = static_cast<u32>(std::min(63L, std::max(0L,
				std::lround((20 * std::log10(g) + 8) / 0.25f))));
		put(bits, 4);
		put(code, 6);
	}
}

/* the loading record b (n octets) into l: whether it is one */
bool PepModem::load_from(const u8 *b, s32 n, PepLoading &l) const
{
	if (n < LOAD_LEN || b[0] != 'L')
		return false;
	l.clear();
	s32 pos = 8;
	auto get = [&](s16 c) {
		u32 v = 0;
		for (s16 i = 0; i < c; i++, pos++)
			v |= static_cast<u32>(b[pos / 8] >> (pos % 8) & 1) << i;
		return v;
	};
	for (s16 k = 1; k <= CARRIERS; k++) {
		u32 bits = get(4), code = get(6);
		if (bits == PILOT)
			continue;
		if (bits > MAX_BITS)
			return false;
		l.bits[static_cast<size_t>(k)] = static_cast<u8>(bits);
		l.gain[static_cast<size_t>(k)] = bits ? std::pow(10.0f,
			(-8 + 0.25f * static_cast<float>(code)) / 20) : 0;
	}
	l.count();
	return true;
}

/* the link on the carriers of 2 bits both ways; our loading for the far
   side's way to it first */
void PepModem::start_link()
{
	link_.uniform(mask_both_, 2);
	if (getenv("DSP_PEP_MASKS")) {
		std::string s;
		for (s16 k = 1; k <= CARRIERS; k++)
			if (mask_both_[static_cast<size_t>(k)] &&
			    !mask_both_[static_cast<size_t>(k - 1)])
				s += " " + std::to_string(k);
			else if (!mask_both_[static_cast<size_t>(k)] &&
				 mask_both_[static_cast<size_t>(k - 1)])
				s += "-" + std::to_string(k - 1);
		logf("mask:%s", s.c_str());
	}
	rx_final_ = allocate();
	{
		double sn = 0, sd = 0;
		s16 m = 0;
		for (s16 k = 40; k < 400; k++) {
			sn += 10 * std::log10(std::norm(H_[static_cast<size_t>(k)]) /
					      nacc_[static_cast<size_t>(k)]);
			sd += 10 * std::log10(std::norm(H_[static_cast<size_t>(k)]) /
					      std::max(1e-9, dist_[static_cast<size_t>(k)]));
			m++;
		}
		logf("SNR 40-400: silence %.1f dB, comb %.1f dB", sn / m, sd / m);
	}
	logf("link: %s; the far side's to us: %s", link_.str().c_str(),
	     rx_final_.str().c_str());
	std::vector<u8> b;
	load_bytes(rx_final_, b);
	ctrl_out_.insert(ctrl_out_.end(), b.begin(), b.end());
	ph_ = PH_LINK;
	heard_ = t_;
}

/* control octets received in order: a loading record is ours to send
   with from our next turn on */
void PepModem::ctrl_in(const u8 *p, s32 n)
{
	ctrl_in_.insert(ctrl_in_.end(), p, p + n);
	if (static_cast<s32>(ctrl_in_.size()) < LOAD_LEN)
		return;
	if (load_from(ctrl_in_.data(), LOAD_LEN, tx_final_)) {
		have_tx_final_ = tx_final_on_ = true;
		logf("our loading: %s", tx_final_.str().c_str());
		check_up();
	} else
		logf("a bad loading record");
	ctrl_in_.erase(ctrl_in_.begin(), ctrl_in_.begin() + LOAD_LEN);
}

/* both ways on their own loadings: data */
void PepModem::check_up()
{
	if (state_ == STARTUP && tx_final_on_ && rx_final_seen_) {
		state_ = DATA;
		logf("data: %d bit/s out, %d in (lines' rates)", tx_rate(),
		     rx_rate());
	}
}

/* see pep.h */
s32 PepModem::tx_rate() const
{
	return static_cast<s32>(s64{tx_final_.total} * FS / EPOCH);
}

/* see pep.h */
s32 PepModem::rx_rate() const
{
	return static_cast<s32>(s64{rx_final_.total} * FS / EPOCH);
}

/* see pep.h */
s32 PepModem::turn_octets() const
{
	return std::max(0, tx_final_.octets() - OVERHEAD) * cfg_.turn_max;
}

/* see pep.h */
s32 PepModem::duplex_rate(bool rx) const
{
	const PepLoading &l = rx ? rx_final_ : tx_final_;
	s64 pay = s64{std::max(0, l.octets() - OVERHEAD)} * 8 * cfg_.turn_max;
	s64 turn = TURN_GAP + PRE_LEN + EPOCH + s64{cfg_.turn_max} * EPOCH;
	return static_cast<s32>(pay * FS / (2 * turn + static_cast<s64>(rtd_)));
}

} // namespace dsp
