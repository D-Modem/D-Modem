/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pep.h - Telebit's PEP (Packetized Ensemble Protocol, the
 *            TrailBlazer's, 1985): many carriers, each with the bits
 *            the line allows it, half duplex in packets with their own
 *            retransmission (docs/dsp/pep.md)
 *
 *    Telebit never published PEP.  What is known comes from its patents
 *    (US 4,679,227, 4,731,816 and 4,833,706, Hughes-Hartogs; 4,438,511,
 *    Baran), the T2500's reference manual and two Telebit notes of 1990
 *    (docs/dsp/pep-details.md).  This modem takes their numbers:
 *
 *    the ensemble: a 1024 point FFT at 8000 Hz, its 511 carriers 7.8125
 *              Hz apart; an epoch is the 1024 samples and their first 61
 *              again after them (the guard; 1085 samples, 135.6 ms, 7.37
 *              baud, as a T3000's DSP and a T2000's recordings have it);
 *              0, 2, 4, 5 or 6 bits a carrier, in Telebit's
 *              constellations (a T3000's firmware's: the quadrant in the
 *              two lowest bits, 5 bits a cross)
 *    the start-up (the patent's eight steps): the answerer's epoch of
 *              1437.5 and 1687.5 Hz at -3 dBR each, then its comb of all
 *              carriers at -27 dBR; both measure the line's noise in
 *              silence; the caller's tones and comb; the caller's epoch
 *              of the carriers that carry 2 bits the answerer's way (180
 *              degrees, else 0, at -28 dBR); the answerer's of those that
 *              carry 2 bits both ways; then each sends the other, in
 *              packets on those carriers, the bits and power of each
 *              carrier its way (Hughes-Hartogs's allocation: the next bit
 *              always where it costs the least power)
 *    packets: one an epoch, a CRC-16 each, a sliding window with
 *              selective retransmission
 *    Adaptive Duplex: the modems take turns, each a turn of one packet
 *              at least (the link kept alive) and N at most (about 3 KB)
 *
 *    and fills in what they do not say with choices of its own: a turn
 *    starts with 32 ms of the two timing tones and one comb epoch (the
 *    receiver's timing and channel), the comb's sign telling the
 *    loading; the two timing tones' carriers are pilots in data epochs;
 *    the packet's header (type, length, sequence number, the epochs left
 *    in the turn, the far side's acknowledgements) and a scrambler on
 *    it.  So it talks to another D-Modem; a TrailBlazer would not
 *    understand it (Telebit's packets are still unknown).  A T3000's
 *    firmware (docs/dsp/pep-details.md) gave the
 *    constellations and the micro packets' carriers, which this modem
 *    uses, but not the packets.
 *
 */

#ifndef DSP_PEP_H
#define DSP_PEP_H

#include <complex>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dm_types.h"
#include "vwb.h"

namespace dsp {

namespace pep {

const s32 FS = 8000;            /* the ensemble's sample rate */
const s32 N = 1024;             /* the FFT */
const s32 GUARD = 61;           /* the first 61 samples repeated after it */
const s32 EPOCH = N + GUARD;    /* 1085 samples, 135.6 ms: the start-up's */
const s32 T_GUARD = 56;
const s32 T_EPOCH = N + T_GUARD;        /* 1080, a turn's: 9 micro epochs */
const s16 CARRIERS = 511;       /* bins 1 to 511 */
const s16 TONE1 = 184;          /* 1437.5 Hz: the timing tones' bins */
const s16 TONE2 = 216;          /* 1687.5 Hz */
const s16 MAX_BITS = 6;
const s16 PILOT = 15;           /* a loading's code for a pilot */

} // namespace pep

/* how one way's carriers are used: by bin (0 to 511) its bits (0, 2,
   4, 5, 6, or pep::PILOT) and its amplitude against the comb's */
struct PepLoading {
	std::vector<u8> bits;
	std::vector<float> gain;
	s32 total = 0;          /* data bits an epoch */

	/* every carrier off, the pilots at the comb's level */
	void clear();
	/* bits on the carriers of mask (by bin), at the comb's level */
	void uniform(const std::vector<u8> &mask, s16 b);
	/* total counted */
	void count();
	/* the octets of a packet in an epoch */
	s32 octets() const { return total / 8; }
	/* "b2x10 b6x300, 1838 bits" */
	std::string str() const;
};

class PepModem {
public:
	/* up to n octets to send into buf: how many (0 none now) */
	using Source = std::function<s32(u8 *buf, s32 n)>;
	/* n octets received, in order */
	using Sink = std::function<void(const u8 *, s32)>;

	struct Config {
		bool calling = true;
		s16 max_bits = pep::MAX_BITS;
		float margin_db = 3;    /* over a carrier's bits' SNR */
		s16 turn_max = 10;      /* packets a turn at most (N) */
		bool micro = true;      /* micro turns when little is to go */
		s32 timeout_s = 60;     /* nothing received so long: FAILED */
	};
	enum State { STARTUP, DATA, FAILED };

	/* for c, its data from src, what comes to sink */
	void init(const Config &c, Source src, Sink sink);
	/* n samples (8000 Hz) from the line in, n to it out */
	void process(const s16 *in, s16 *out, s32 n);

	/* the start-up, data, or given up */
	State state() const { return state_; }
	/* both ways on their own loadings (CONNECT) */
	bool connected() const { return state_ == DATA; }
	/* why FAILED */
	const char *why() const { return why_; }
	/* the line's rates, bit/s of the packets' octets: ours, the far
	   side's (T2500's S70 and S72) */
	s32 tx_rate() const;
	/* (see tx_rate()) */
	s32 rx_rate() const;
	/* the data's rate one way when both send all they can: a turn's
	   payload each way over the time of both turns */
	s32 duplex_rate(bool rx) const;
	/* the payload of our longest turn (octets) */
	s32 turn_octets() const;
	/* the loadings: ours to send with, the far side's (S71, S73) */
	const PepLoading &tx_loading() const { return tx_final_; }
	/* (see tx_loading()) */
	const PepLoading &rx_loading() const { return rx_final_; }
	/* carrier k's noise, dBm0 in its bin's band (S76) */
	float noise_dbm0(s16 k) const;
	/* the line's noise by bin, dB against the comb's carrier */
	const std::vector<float> &noise_db() const { return noise_db_; }
	/* packets received again (S74) and taken (S75) */
	s64 packets_bad() const { return pk_bad_; }
	/* (see packets_bad()) */
	s64 packets_good() const { return pk_good_; }
	/* packets we sent again */
	s64 packets_again() const { return pk_again_; }
	/* the far side's sample clock against ours, less 1, in ppm */
	double clock_ppm() const { return (r_ - 1) * 1e6; }
	/* the mean SNR of the carriers received (dB) */
	float snr_db() const { return snr_db_; }
	/* the round trip's delay (samples at 8000 Hz) between the turns */
	double rtd() const { return rtd_; }
	/* the turns each way: ours sent, the far side's received */
	s32 turns_tx() const { return turns_tx_; }
	/* (see turns_tx()) */
	s32 turns_rx() const { return turns_rx_; }
	/* of them, micro turns (one short packet) */
	s32 micro_tx() const { return micro_tx_; }
	/* (see micro_tx()) */
	s32 micro_rx() const { return micro_rx_; }
	/* the far side's start-up tones heard: it is a PEP modem */
	bool far_heard() const { return far_heard_; }
	/* the samples (8000 Hz) run so far */
	s64 now() const { return t_; }
	/* the answerer: when its first tones and comb end (samples) */
	s64 first_end() const { return first_end_; }
	/* the log since the last call */
	std::string take_log();

private:
	enum Tx { TX_IDLE, TX_RUN };
	enum Rx {
		RX_OFF,         /* sending, or just sent */
		RX_TONES,       /* the start-up's tone epoch */
		RX_COMB,        /* its comb */
		RX_NOISE,       /* the silence after it */
		RX_PHASE,       /* the caller's phase epochs (answerer) */
		RX_PRE,         /* a turn's tones */
		RX_SYNC,        /* its comb epoch */
		RX_EPOCHS,      /* its packets (or the answerer's phase) */
		RX_MICRO,       /* a micro turn's, if it is one */
	};
	enum Phase {
		PH_A_SEND,      /* answerer: its tones and comb */
		PH_A_LISTEN,    /* noise, then the caller's */
		PH_C_LISTEN,    /* caller: the answerer's tones and comb, noise */
		PH_C_SEND,      /* its tones, comb, phase epochs */
		PH_C_WAIT6,     /* the answerer's phase turn */
		PH_LINK,        /* turns of packets */
	};

	/* a packet kept until acknowledged */
	struct Out {
		u8 type;
		std::vector<u8> pay;
	};

	/* the transmitter */
	void tx_start_tones_comb(s16 combs, s16 phases,
				 const std::vector<u8> *mask);
	void tx_turn();
	void tx_epoch_tones(s32 len);
	void tx_epoch_comb(float sign, float gain,
			   const std::vector<u8> *mask, s32 len);
	void tx_epoch_data(const PepLoading &l, const std::vector<u8> &oct,
			   s32 len);
	void tx_samples(const std::vector<cf> &X, s32 len);
	std::vector<u8> make_packet(u8 type, u8 seq, const u8 *pay, s32 n,
				    u8 left, s32 cap);
	void micro_bits(const PepLoading &l, std::vector<u8> &b) const;
	s32 micro_octets(const PepLoading &l) const;
	void tx_short(const std::vector<cf> &X);
	void tx_micro(const PepLoading &l, const std::vector<u8> &oct);

	/* the receiver */
	void rx_sample(float x);
	void rx_run();
	bool tones_found(s64 at, float *ratio, double *energy) const;
	void window(double at, cf *Y) const;
	void window(double at, cf *Y, double r) const;
	double comb_fit(const cf *Y, const std::vector<cf> *H, cf *C) const;
	void rx_comb_epochs();
	void rx_noise();
	void rx_phase_epochs();
	void rx_turn_sync();
	void rx_turn_epoch();
	bool rx_packet(const std::vector<u8> &oct);
	void rx_turn_end();
	void window_short(double at, cf *Y) const;
	bool rx_micro();
	bool phase_bit(s16 k) const;
	void decode(const cf *Z, const PepLoading &l, std::vector<u8> &oct,
		    std::vector<cf> *dec) const;

	/* the start-up's decisions */
	std::vector<u8> capable(s16 b) const;
	double noise(s16 k) const;
	PepLoading allocate() const;
	void load_bytes(const PepLoading &l, std::vector<u8> &b) const;
	bool load_from(const u8 *b, s32 n, PepLoading &l) const;
	void start_link();
	void ctrl_in(const u8 *p, s32 n);
	void check_up();

	void fail(const char *why);
	void logf(const char *fmt, ...)
#ifdef __GNUC__
		__attribute__((format(printf, 2, 3)))
#endif
		;

	Config cfg_;
	Source src_;
	Sink sink_;
	State state_ = STARTUP;
	Phase ph_ = PH_A_SEND;
	const char *why_ = "";
	std::string log_;
	VwbFft fft_;
	VwbInterp interp_;
	std::vector<cf> comb_;          /* the comb's points, by bin */
	float scale_ = 0;               /* a unit point's amplitude */

	/* samples */
	s64 t_ = 0;                     /* the next one's index */
	VwbHistory in_;
	std::deque<float> out_;         /* to send */
	s64 tx_end_ = 0;                /* when what was queued ends */
	s64 first_end_ = 0;             /* (see first_end()) */
	bool far_heard_ = false;        /* (see far_heard()) */

	/* the receiver */
	Rx rx_ = RX_OFF;
	s64 rx_from_ = 0;               /* listen from this sample on */
	s64 rx_scan_ = 0;               /* the next block to look at */
	s32 hits_ = 0;
	double at_ = 0;                 /* the next epoch's start */
	double r_ = 1;                  /* far samples per ours */
	s16 rx_left_ = 0;               /* epochs to come */
	s32 rx_got_ = 0;                /* of the turn's, decoded */
	bool rx_final_on_ = false;      /* the turn on the final loading */
	std::vector<cf> H_;             /* the channel by bin */
	std::vector<double> nacc_;      /* noise power by bin */
	std::vector<double> dist_;      /* the comb's unsteadiness by bin */
	s32 nacc_n_ = 0;
	std::vector<float> noise_db_;
	std::vector<cf> phase_acc_;
	s32 phase_n_ = 0;
	std::vector<u8> mask_far_;      /* the far side's 2 bit carriers */
	std::vector<u8> mask_both_;     /* the link's */
	s64 heard_ = 0;                 /* the last good packet's sample */
	s64 wait_until_ = 0;            /* no turn by then: ours again */
	double floor_ = 0;              /* the quiet line's block energy */
	float snr_db_ = 0;
	double rtd_ = 0;

	/* loadings */
	PepLoading link_;               /* 2 bits both ways */
	PepLoading tx_final_, rx_final_;
	bool have_tx_final_ = false;    /* the far side's plan for us */
	bool tx_final_on_ = false;      /* our turns use it */
	bool rx_final_seen_ = false;    /* a turn of theirs on rx_final_ */
	bool sent_load_ = false;

	/* packets */
	u8 tx_seq_ = 0;                 /* the next new one */
	u8 tx_base_ = 0;                /* the oldest not acknowledged */
	std::map<u8, Out> outq_;        /* sent, not acknowledged */
	std::vector<u8> ctrl_out_;      /* control octets to send */
	u8 rx_base_ = 0;                /* the next expected */
	std::map<u8, Out> rxq_;         /* out of order */
	std::vector<u8> ctrl_in_;
	u8 far_ack_ = 0;
	u16 far_map_ = 0;
	s64 pk_good_ = 0, pk_bad_ = 0, pk_again_ = 0;
	s32 turns_tx_ = 0, turns_rx_ = 0;
	s32 micro_tx_ = 0, micro_rx_ = 0;
};

} // namespace dsp

#endif /* DSP_PEP_H */
