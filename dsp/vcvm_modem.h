/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_modem.h - one V.cvm DCE after V.8 (docs/v_cvm.pdf clauses 8 to
 *                   11), sample by sample: the start-up (vcvm_startup.h),
 *                   then phase 4 in each direction (vcvm_data.h), its
 *                   renegotiations and its retrains (9.5).
 *
 *    Phase 4: each transmitter begins as its start-up ends; each
 *    receiver looks for the peer's phase 4 from the end of the last
 *    message it received, and takes it as data once a block decodes.
 *    Until then the first MP from the peer is its last of the exchange
 *    (the receiver starts again at its end), and a further one, the
 *    peer having lost our last, is answered with another and phase 4
 *    begun again.
 *
 *    A renegotiation: the control sequence, then the MP exchange in the
 *    message mode with the new loadings, then phase 4 again (the
 *    scrambler and the interleaver restarted).  The DCE that asks sends
 *    the control sequence and its MP; the other, on the control
 *    sequence, answers with its own and its MP.  A retrain is a
 *    renegotiation whose MP asks for one: after the exchange both
 *    return to phase 2, the calling DCE's INFO0 after 3 s (the other's
 *    last MP is through by then).
 *
 *    What asks for them (9.5): a loaded tone's phase error over its
 *    order's threshold, or codewords failing, for a renegotiation; no
 *    codeword decoded for 2 s (or the signal lost), for a retrain; and
 *    in the parametric mode, or in no data mode, a retrain 30 s on and
 *    every 60 s after, none while c-bar is under 0.85, at once when it
 *    rises to it.
 *
 */

#ifndef DSP_VCVM_MODEM_H
#define DSP_VCVM_MODEM_H

#include <memory>
#include <string>
#include <vector>

#include "dm_types.h"
#include "vcvm_data.h"
#include "vcvm_startup.h"

namespace dsp {

class VcvmModem {
public:
	using Source = VcvmDataTx::Source;
	using Sink = VcvmDataRx::Sink;

	struct Config {
		VcvmStartup::Config su;
		bool auto_reneg = true;         /* 9.5's renegotiations */
		bool retrain_timers = true;     /* the parametric mode's */
	};
	enum State { STARTUP, DATA, RENEG, FAILED };

	s16 init(const Config &c, Source src, Sink sink);
	void process(const float *in, float *out, s32 n);

	/* ask for a renegotiation (with the loading measured in phase 4)
	   or a retrain, at the next symbol */
	void renegotiate() { want_reneg_ = true; }
	/* ask for a retrain at the next symbol */
	void retrain() { want_retrain_ = true; }

	/* the state */
	State state() const { return state_; }
	/* the reason, if FAILED */
	const char *why() const { return why_; }
	/* phase 4 both ways: the transmitter in it, the peer's data
	   decoding */
	bool connected() const;
	/* the loading received */
	const VcvmLoading &rx_loading() const { return rxl_; }
	/* the loading sent */
	const VcvmLoading &tx_loading() const { return txl_; }
	/* the round trip the start-up measured, ms (-1 before) */
	s16 rtt_ms() const { return rtt_ms_; }
	/* both DCEs indicated V.44's preset dictionary (INFO0) */
	bool v44_preset() const { return su_ && su_->v44_preset(); }
	/* the renegotiations completed */
	s32 renegotiations() const { return renegs_; }
	/* the retrains */
	s32 retrains() const { return retrains_; }
	/* what the receiver has measured of the data */
	const VcvmDataStats &rx_stats() const { return drx_.stats(); }
	/* the start-up now (or the last one) */
	const VcvmStartup *startup() const { return su_.get(); }
	/* the log, a line an event */
	const std::string &log() const { return log_; }

private:
	void note(const char *fmt, ...);
	void fail(const char *why);
	/* the sample at which what is queued to send runs out */
	s32 queued() const
	{
		return t_ + static_cast<s32>(txq_.size() - txpos_);
	}
	void silence(s16 ms);
	void send_mp();
	void start_tx();
	void start_rx(s32 from);
	void enter_data();
	void begin_reneg(bool remote, bool retrain);
	void finish_reneg();
	void start_startup(bool after_retrain);
	VcvmLoading rechoose(bool *retrain);
	void check_data();
	void on_msg(const VcvmMsg &m);
	template <class R> void replay(R &r, s32 from);
	void step();

	Config c_{};
	Source src_;
	Sink sink_;
	State state_ = STARTUP;
	const char *why_ = "";
	std::unique_ptr<VcvmStartup> su_;
	s32 su_at_ = 0;                 /* where it started */
	VcvmDataTx dtx_;
	VcvmDataRx drx_;
	VcvmMsgTx mtx_;
	VcvmMsgRx mrx_;
	bool msgp_ = false;
	VcvmLoading rxl_{}, txl_{};
	s16 rtt_ms_ = -1;
	std::vector<float> txq_;
	size_t txpos_ = 0;
	std::vector<float> hist_;
	s32 hbase_ = 0;
	s32 t_ = 0;
	bool tx_data_ = false;          /* the transmitter in phase 4 */
	bool rx_on_ = false, mrx_on_ = false;
	s32 rx_from_ = 0;
	/* the renegotiation */
	bool want_reneg_ = false, want_retrain_ = false;
	bool retrain_ = false, peer_retrain_ = false;
	bool got_mp_ = false, got_ack_ = false, sent_ack_ = false;
	s16 mps_ = 0;
	s32 deadline_ = 0;
	s32 last_rx_end_ = 0;
	s16 p4_mps_ = 0;                /* the peer's MPs in phase 4 (the
					   first its last of the exchange) */
	s32 renegs_ = 0, retrains_ = 0;
	/* phase 4's watch */
	s32 data_at_ = 0;               /* phase 4 (re)entered */
	s32 confirmed_at_ = 0;          /* the peer's data decoding since */
	s32 next_check_ = 0;
	s32 words_ = 0, failed_ = 0; /* at the last check */
	s32 next_retrain_ = -1;         /* the parametric mode's timer */
	bool cbar_low_ = false;
	std::string log_;
};

} // namespace dsp

#endif /* DSP_VCVM_MODEM_H */
