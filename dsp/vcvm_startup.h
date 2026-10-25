/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_startup.h - V.cvm's start-up after V.8 (docs/v_cvm.pdf,
 *                     clauses 8 and 9, Annexes A and B)
 *
 *    The capabilities: the information field of Annex A that CM and JM
 *    carry in their non-standard facilities.
 *
 *    The messages: INFO0, INFO1 and MP, each an HDLC frame sent twice
 *    in the message mode: the parametric mode's P20 where both DCEs
 *    have it, else V.21's FSK (channel 1 from the calling DCE, 2 from
 *    the answering one).
 *
 *    The probing: L1 (each comb probed, 30 symbols of DQPSK with the
 *    20 ms symbol), L2 (the 200 Hz comb with the 22.5 ms symbol, 22
 *    symbols) and L3 (P20's preamble and 40 symbols, P10's and 80), the
 *    phase steps and lags from the transmitter's scrambler fed with
 *    ones; what the receiver measures of them, and clause 9.3's
 *    loading.
 *
 *    The start-up itself (VcvmStartup), one DCE's, sample by sample:
 *
 *        calling    INFO0 L1 L2 L3 .............................. INFO1 MP..
 *        answering  ............. INFO0 L1 L2 L3 INFO1 ........... MP..
 *
 *    each part 100 ms after the last (the answering DCE's INFO0 50 ms
 *    after the end of the calling DCE's L3, the calling DCE's INFO1 50
 *    ms after the answering one's), so that each INFO1 carries what its
 *    DCE measured of its peer's probing (V.34's phase 2 orders its INFO1s
 *    so); then the MP exchange, at once in both directions, until each
 *    DCE has its peer's acknowledgement.  Its outcome: the loading each
 *    direction is to use, the round trip delay.
 *
 */

#ifndef DSP_VCVM_STARTUP_H
#define DSP_VCVM_STARTUP_H

#include <memory>
#include <string>
#include <vector>

#include "dm_types.h"
#include "vcvm.h"

namespace dsp {

/*
 *    Annex A: the capabilities
 */

/* the information field (Table A.1) */
struct VcvmCaps {
	s16 version = 1;
	bool fs16 = false;      /* the signal interface at 16000 Hz */
	bool fax = false;       /* Annex C */
	bool rings = true;      /* the amplitude loading, 10.11 */
	bool wide = false;      /* wideband, the 200 (and 300) Hz combs */
	bool wide400 = false;   /* wideband, the 400 Hz comb */
	bool dpsk8 = true;
	bool sym22 = true;      /* the 22.5 ms symbol */
	bool quick = false;     /* quick start, 10.10 */
	bool param = true;      /* the parametric mode, 10.6 */
	bool v44dict = true;    /* octet 1 bit 2: V.44's preset dictionary
				   of Annex E (11.1) */

	void encode(u8 o[2]) const;
	/* a field of another version: version 1 and the bits it knows */
	static VcvmCaps decode(const u8 o[2]);
	u8 modes() const;       /* octet 2: the modes */
	s32 fs() const { return fs16 ? 16000 : 8000; }
};

/* the identifier octet before the field in the non-standard facilities */
#define VCVM_NSF_ID     0x43
/* the T.35 country and provider codes the field follows: placeholders
   until an implementer's own (Annex A.1 a)) */
#define VCVM_T35_COUNTRY        0xB5
#define VCVM_T35_PROVIDER0      0x00
#define VCVM_T35_PROVIDER1      0x00

/* the non-standard facilities' octets (country, provider, 'C', the
   field): their number */
s16 vcvm_nsf_build(const VcvmCaps &c, u8 *out);
/* the field in non-standard facilities' octets, if there */
bool vcvm_nsf_find(const u8 *p, s16 n, VcvmCaps *c);

/* V.8's non-standard facilities category for CM and JM (6.6/V.8): the
   category octet (0x0F), then the NS field (its length, the T.35
   country code, the provider code's length, the provider code, then the
   identifier and the information field) in extension octets of five
   bits each (b0-b2 and b6-b7), the field's bits least significant
   first.  Its octets to out (21 at most): their number */
s16 vcvm_v8_ns(const VcvmCaps &c, u8 *out);
/* the information field among a CM's or JM's octets, if there */
bool vcvm_v8_find(const u8 *msg, s16 n, VcvmCaps *c);

/* +CVM's mode set (Table 12-1) */
enum {
	VCVM_M_ROBUST = 1, VCVM_M_NARROW = 2, VCVM_M_WIDE = 4,
	VCVM_M_8DPSK = 8, VCVM_M_RINGS = 16, VCVM_M_PARAM = 32,
	VCVM_M_ALL = 63,
	VCVM_M_SYM22 = 64,      /* not +CVM's: both have the 22.5 ms symbol */
	VCVM_M_WIDE400 = 128,   /* nor this: both have the W400 comb */
};

/* this DCE's information field for +CVM's mode set (VCVM_M_*) at the
   signal interface's fs (DSP_VCVM_V44DICT=0: without the preset
   dictionary, for a test) */
VcvmCaps vcvm_caps_for(s16 modes, s32 fs);

/* what both DCEs have and the mode set allows, as VCVM_M_* bits */
s16 vcvm_allowed(const VcvmCaps &own, const VcvmCaps &peer, s16 modes);

/*
 *    Annex B: the messages
 */

/* INFO0 (Table B.1) */
struct VcvmInfo0 {
	u8 modes = 0;           /* the information field's octet 2 */
	u16 combs = 0;          /* bit c: comb code c is probed */
	bool quick_offer = false, quick_accept = false;
	bool rings = true;      /* octet 3 bit 5: the amplitude rings allowed
				   (where both have them) */
	s16 level = 6;          /* the transmit level, 0.5 dB below -9 dBFS */

	s16 encode(u8 *o) const;                /* 4 octets */
	s16 decode(const u8 *o, s16 n);         /* 0, or -1 */
	bool l2() const { return modes & 0x04; }
	/* L3 is probed */
	bool l3() const { return modes & 0x01; }
};

/* a loading: what INFO1 and MP ask for */
struct VcvmLoading {
	enum Kind { NONE, COMB, PARAM };
	Kind kind = NONE;
	s16 code = 0;           /* the comb */
	bool sym22 = false;     /* its 22.5 ms symbol */
	bool p10 = false;       /* the parametric mode's 10 ms symbol */
	s16 rate = 0;           /* the code rate: 0 1/2, 1 3/4, 2 7/8 */
	s16 tones = 0;          /* the comb's tones (N) */
	VcvmLoad load{};

	double sym_ms() const;
	s16 bits() const;               /* a symbol's */
	s32 raw_bps() const;
	s32 coded_bps() const;          /* the raw rate times k / 32 */
	s16 k() const { return rate == 2 ? 28 : rate == 1 ? 24 : 16; }
	s16 loaded() const;             /* tones loaded (0 parametric) */
	std::string str() const;        /* for the logs */
};

/* INFO1 and MP (Table B.2) */
struct VcvmMp {
	bool ack = false;       /* MP only */
	bool retrain = false;   /* MP only, octet 2 bit 8: the DCE asks
				   for a retrain (both return to phase 2
				   after the exchange) */
	s16 rtt = 255;          /* the round trip delay, 10 ms; 255 unknown */
	VcvmLoading l;

	s16 encode(u8 *o) const;                /* octets, 5 to 39 */
	s16 decode(const u8 *o, s16 n);         /* 0, or -1 */
};

/* one received message mode transmission */
struct VcvmMsg {
	std::vector<u8> octets;         /* the first copy whose FCS checked */
	bool ok = false;                /* there was one */
	s32 start = 0, end = 0;         /* the signal's, samples received */
};

/* the message mode's transmitter */
class VcvmMsgTx {
public:
	/* P20 (param) or V.21's channel of the calling or answering DCE */
	s16 init(s32 fs, bool param, bool calling);
	/* a message (its octets) appended to out: P20's preamble, or
	   flags, then its frame twice and flags */
	void send(const u8 *m, s16 n, std::vector<float> &out);

private:
	s32 fs_ = 8000;
	bool param_ = true;
	bool calling_ = true;
	VcvmParamTx ptx_;
	double ph_ = 0;                 /* the FSK's phase */
	double clk_ = 0;                /* its bit clock */
};

/* V.21's FSK as the message mode receives it: two filters matched over
   80 per cent of the bit (Appendix I's detector), the bit clock from the
   zero crossings of the difference of their magnitudes */
class VcvmFskRx {
public:
	void init(float mark, float space, s32 fs);
	/* a sample in: the bit decided at its centre, or -1 */
	s16 run(float x);
	/* a signal begins: the bit clock acquired anew */
	void resync() { acq_ = 0; }

private:
	s32 fs_ = 8000;
	s16 w_ = 0;
	double wm_ = 0, ws_ = 0;        /* rad a sample */
	double pm_ = 0, ps_ = 0;        /* the mixers' phases */
	std::vector<double> buf_;       /* the products, 4 a sample */
	size_t pos_ = 0;
	double sum_[4] = {};
	double phase_ = 0, inc_ = 0;    /* the bit clock: a bit at 1 */
	double last_ = 0;
	u32 n_ = 0;
	s16 acq_ = 0;                   /* crossings since the resync */
};

/* the message mode's receiver */
class VcvmMsgRx {
public:
	VcvmMsgRx();
	~VcvmMsgRx();
	VcvmMsgRx(const VcvmMsgRx &) = delete;
	VcvmMsgRx &operator=(const VcvmMsgRx &) = delete;

	/* P20, or the channel the peer (calling or answering) sends on */
	s16 init(s32 fs, bool param, bool peer_calling);
	/* nothing received (nor kept): the next sample pushed is sample
	   at */
	void restart(s32 at);
	void push(const float *x, s32 n);
	/* a transmission received, once its signal has ended */
	bool get(VcvmMsg *m);
	/* the samples received: the index of the next one pushed */
	s32 received() const { return n_; }
	/* a transmission is coming in */
	bool busy() const { return active_; }

	/* the deframer's: a frame between flags */
	void frame(const u8 *d, s16 len);

private:
	struct Hdlc;
	struct Fsk;
	void ended(s32 at);

	s32 fs_ = 8000;
	bool param_ = true;
	VcvmParamRx prx_;
	std::unique_ptr<Fsk> fsk_;
	std::unique_ptr<Hdlc> hdlc_;
	s32 n_ = 0;                     /* samples received */
	bool active_ = false;           /* a signal is being received */
	s32 start_ = 0;
	std::vector<double> win_;       /* the last 10 ms, squared */
	double wsum_ = 0;
	size_t wpos_ = 0;
	s32 quiet_ = 0;                 /* samples quiet in a row */
	s32 loud_ = 0;                  /* and loud */
	bool got_ = false;
	std::vector<u8> first_;         /* this transmission's first copy */
	struct Sym {
		u8 idx, second;
		float margin;
	};
	std::vector<Sym> syms_;         /* P20's, this transmission's */
	bool chase();
	std::vector<VcvmMsg> out_;
};

/*
 *    Annex B: the probing
 */

/* the probing's parts at a rate */
struct VcvmProbe {
	s32 fs = 8000;
	u16 combs = 0;          /* L1: bit c comb code c */
	bool l2 = false, l3 = false;
	s16 tap = 18;           /* the transmitter's scrambler (SCR_GPC,
				   SCR_GPA) */

	enum { L1_SYMS = 30, L2_SYMS = 22, L3_P20 = 40, L3_P10 = 80 };
	/* L2's comb: W200 at 16000 Hz, else N200 */
	s16 l2_code() const { return fs == 16000 ? 3 : 0; }
};

/* the combs a DCE probes with at fs (its band's), as allowed */
u16 vcvm_probe_combs(s32 fs, s16 allowed);

/* the probing's transmitter: L1, L2, L3 appended to out */
void vcvm_probe_send(const VcvmProbe &p, std::vector<float> &out);

/* what the receiver measured of a comb (Annex B.2) */
struct VcvmCombMeas {
	bool ok = false;        /* received */
	s16 code = -1;
	bool sym22 = false;
	s16 tones = 0;
	s16 n = 0;              /* the symbols measured */
	float sigma[VCVM_TONES];        /* the phase error, degrees rms
					   about its mean */
	float level_db[VCVM_TONES];     /* the mean level, dB below the
					   strongest tone's */
	float sigma_a[VCVM_TONES];      /* the magnitude's change, dB rms */
	s16 errs[VCVM_TONES];           /* DQPSK decisions in error */
};

struct VcvmProbeMeas {
	s16 ncomb = 0;
	VcvmCombMeas comb[3];   /* L1's, in order */
	VcvmCombMeas l2;
	bool l3 = false;        /* L3 was received (its P20 part; n10 is 0
				   where P10's was not found) */
	s16 n20 = 0, e20 = 0;   /* P20's symbols measured, in error */
	s16 n10 = 0, e10 = 0;
	float cbar = 0;         /* P20's mean c */
};

/* the loading of clause 9.3 from the measurements, of the modes
   allowed (vcvm_allowed()) */
VcvmLoading vcvm_choose(const VcvmProbeMeas &m, s16 allowed);

/* the probing's receiver */
class VcvmProbeRx {
public:
	s16 init(const VcvmProbe &p);
	/* L1's onset looked for from sample from */
	void start(s32 from);
	/* samples from the line, the next being sample received() */
	void push(const float *x, s32 n);
	/* the index of the next sample pushed */
	s32 received() const
	{
		return hbase_ + static_cast<s32>(hist_.size());
	}
	/* the probing received */
	bool done() const { return st_ == DONE; }
	/* the probing not found or lost */
	bool failed() const { return st_ == FAILED; }
	/* the end of the probing (its last symbol's), once done */
	s32 end() const { return end_; }
	/* what has been measured */
	const VcvmProbeMeas &meas() const { return m_; }

private:
	enum St { IDLE, ONSET, COMB, L3A, L3B, DONE, FAILED };
	struct Part {
		s16 code;
		bool sym22;
		s16 n;                  /* symbols, the reference's with them */
		VcvmCombMeas *m;
	};
	void run();
	void begin_comb();
	void begin_param(bool p10);
	void finish_comb(VcvmCombMeas *cm);
	s16 resolve_start();

	VcvmProbe p_{};
	VcvmProbeMeas m_{};
	std::vector<Part> parts_;       /* L1's combs, then L2 */
	St st_ = IDLE;
	std::vector<float> hist_;       /* what came, from hbase_ */
	s32 hbase_ = 0;
	s32 scan_ = 0;                  /* the onset's search */
	s32 first_ = 0;                 /* the part's expected start */
	s32 end_ = 0;
	s32 deadline_ = 0;
	u32 scr_ = 0;                   /* the known sequence's scrambler */
	bool pend_ = false;             /* the next symbol's known steps or
					   lag already taken from it */
	u8 ph8_[VCVM_TONES] = {};
	s16 want_ = 0;
	/* the comb being received */
	VcvmCombRx crx_;
	s16 part_ = 0;
	s16 sym_ = 0;                   /* its symbols so far */
	bool acquired_ = false, onset_ = false;
	bool resolve_ = false;          /* the first comb's symbol still to
					   be told by its known steps */
	s16 n_ = 0;                     /* the symbols measured */
	double se_[VCVM_TONES], see_[VCVM_TONES], sa_[VCVM_TONES],
		lv_[VCVM_TONES], err_[VCVM_TONES];
	/* L3 */
	VcvmParamRx prx_;
	double csum_ = 0;
};

/*
 *    the start-up
 */

class VcvmStartup {
public:
	struct Config {
		bool calling = true;
		s32 fs = 8000;          /* the signal interface (both DCEs') */
		VcvmCaps own, peer;     /* from CM and JM */
		s16 modes = VCVM_M_ALL; /* +CVM's */
		s16 level = 6;          /* INFO0's transmit level */
		s16 delay_ms = 100;     /* the calling DCE's silence before
					   INFO0 (after V.8; longer after a
					   retrain, while the peer finishes
					   its MP) */
	};
	enum State { RUNNING, DONE, FAILED };

	s16 init(const Config &c);
	/* n samples from the line in, n to it out */
	void process(const float *in, float *out, s32 n);

	/* the state */
	State state() const { return state_; }
	/* the reason */
	const char *why() const { return why_; }        /* if FAILED */
	/* the loading this DCE asked for (it receives with it) and the one
	   its peer asked for (it transmits with it) */
	const VcvmLoading &rx_loading() const { return rxl_; }
	/* the loading the peer asked for */
	const VcvmLoading &tx_loading() const { return txl_; }
	/* the round trip delay measured, ms (-1 not yet) */
	s16 rtt_ms() const { return rtt_ms_; }
	/* what this DCE measured of the peer's probing */
	const VcvmProbeMeas &meas() const { return prx_.meas(); }
	/* the messages in P20 (both have the parametric mode) */
	bool param_messages() const { return msgp_; }
	/* the sample at which the start-up was done (-1 before) */
	s32 done_at() const { return done_at_; }
	/* the end of the last message received (phase 4's signal is
	   looked for from there) */
	s32 last_rx_end() const { return last_rx_end_; }
	/* what happened, a line a step */
	const std::string &log() const { return log_; }

private:
	enum Step {
		WAIT_INFO0,     /* the peer's INFO0 */
		PROBE,          /* its L1 to L3 */
		WAIT_INFO1,     /* its INFO1 */
		MP,             /* the MP exchange */
	};
	void note(const char *fmt, ...);
	void fail(const char *why);
	s32 queued() const;             /* the sample after what is queued */
	void silence(s16 ms);
	void silence_until(s32 at);
	template <class R> void replay(R &r, s32 from);
	void send_msg(const u8 *m, s16 n);
	void send_probing();
	void send_mp();
	void on_msg(const VcvmMsg &m);
	void step();

	Config c_{};
	s16 allowed_ = 0;
	bool msgp_ = false;
	State state_ = RUNNING;
	Step step_ = WAIT_INFO0;
	const char *why_ = "";
	VcvmMsgTx mtx_;
	VcvmMsgRx mrx_;
	VcvmProbeRx prx_;
	VcvmInfo0 info0_{}, peer0_{};
	VcvmLoading rxl_{}, txl_{};
	std::vector<float> txq_;
	size_t txpos_ = 0;
	std::vector<float> hist_;       /* received, from hbase_ */
	s32 hbase_ = 0;
	s32 t_ = 0;                     /* samples so far, both ways */
	s32 probe_end_tx_ = -1;         /* our L3's end, sent */
	s32 info1_end_tx_ = -1;         /* our INFO1's end, sent */
	bool got_mp_ = false, got_ack_ = false;
	bool sent_ack_ = false;         /* an MP with the acknowledgement sent
					   after the peer's came */
	bool mp_on_ = false;
	s16 rtt_ms_ = -1;
	s32 deadline_ = 0;
	s32 done_at_ = -1;
	s32 last_rx_end_ = 0;
	s16 mps_ = 0;
	std::string log_;
};

} // namespace dsp

#endif /* DSP_VCVM_STARTUP_H */
