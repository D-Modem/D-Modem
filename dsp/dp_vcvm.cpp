/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_vcvm.cpp - the V.cvm pump (DP_VCVM): what V.8's pump hands the
 *                  call to when CM and JM both carry V.cvm's information
 *                  field; one DCE of vcvm_modem.h at 8000 Hz (a 9600 Hz
 *                  line through converters), the core's bits its octets
 *                  (the first bit the least significant), CONNECT at the
 *                  coded rates once data goes both ways
 *
 */

#include <cinttypes>
#include <cstring>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"
#include "vcvm_modem.h"

namespace dsp {

enum class VcvmSt { RUN };

class VcvmPump : public PumpBase<VcvmPump, VcvmSt> {
public:
	static constexpr const char *name = "V_CVM";
	/* its name in the log */
	const char *tag() const { return "vcvm"; }

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 process(void *in, void *out, s32 cnt) override;

private:
	s16 source();
	void sink(const u8 *d, s32 n);
	void report();
	void log_lines();

	VcvmModem mdm_;
	struct dp_vcvm *info_ = nullptr;
	bool up_ = false;               /* CONNECT reported */
	s32 renegs_ = 0, retrains_ = 0;
	s32 line_rate_ = 8000;
	Resampler rs_up_, rs_down_;     /* 9600 -> 8000, 8000 -> 9600 */
	std::vector<float> in8_, out8_, back_;
	size_t back_pos_ = 0;
	const VcvmStartup *su_ = nullptr;
	size_t su_logged_ = 0, logged_ = 0;
};

/* the pump at srate_ (8000, or 9600 through the converters) once V.8
   agreed on V.cvm: the DCE's modes from MDMPRM_VCVM, the peer's from
   its field; 0, or -1 */
s16 VcvmPump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_,
		    s32)
{
	if (srate_ != 8000 && srate_ != 9600) {
		DSP_DBG("vcvm: sample rate %" PRId32 " not supported\n",
			srate_);
		return -1;
	}
	pump_setup(m, srate_, caller_);
	if (!rt || !rt->vcvm_peer) {
		DSP_DBG("vcvm: V.8 did not agree on V.cvm\n");
		return -1;
	}
	line_rate_ = srate_;
	if (srate_ == 9600) {
		rs_down_.init(5, 6);
		rs_up_.init(6, 5);
	}
	intptr_t v = modem_get_param(m, MDMPRM_VCVM);
	info_ = v == -1 || v == 0 ? nullptr :
				    reinterpret_cast<struct dp_vcvm *>(v);
	VcvmModem::Config c;
	c.su.calling = caller_;
	c.su.fs = 8000;
	c.su.modes = info_ ? info_->modes : VCVM_M_ALL;
	c.su.own = vcvm_caps_for(c.su.modes, 8000);
	c.su.peer = VcvmCaps::decode(rt->vcvm_info, VCVM_FIELD_OCTETS);
	if (mdm_.init(c, [this]() { return source(); },
		      [this](const u8 *d, s32 n) { sink(d, n); }))
		return -1;
	stat_dp(DP_VCVM);
	DSP_DBG("vcvm: %s, the peer's field %02x %02x\n",
		caller_ ? "calling" : "answering", rt->vcvm_info[0],
		rt->vcvm_info[1]);
	return 0;
}

/* an octet of the link layer's bits, the first the least significant
   (ones when it has none); HDLC flags instead while the peer's data is
   not decoding, the link left alone: its frames would go unanswered,
   and its T401, counting the bits it sends, would run out on polls
   the answers to which we cannot hear */
s16 VcvmPump::source()
{
	if (mdm_.rx_waiting())
		return 0x7e;
	u8 b[8];
	modem_get_bits(modem, 1, b, 8);
	s16 v = 0;
	for (s16 i = 0; i < 8; i++)
		v |= (b[i] & 1) << i;
	return v;
}

/* the octets the modem decoded, to the link layer, the first bit the
   least significant */
void VcvmPump::sink(const u8 *d, s32 n)
{
	for (s32 i = 0; i < n; i++) {
		u8 b[8];
		for (s16 k = 0; k < 8; k++)
			b[k] = (d[i] >> k) & 1;
		modem_put_bits(modem, 1, b, 8);
	}
}

/* what the start-up and the modem have logged since, to the DSP's
   log */
void VcvmPump::log_lines()
{
	const VcvmStartup *su = mdm_.startup();
	if (su != su_) {
		su_ = su;
		su_logged_ = 0;
	}
	auto out = [](const std::string &l, size_t *at) {
		while (*at < l.size()) {
			size_t e = l.find('\n', *at);
			if (e == std::string::npos)
				break;
			DSP_DBG("vcvm: %s\n", l.substr(*at, e - *at).c_str());
			*at = e + 1;
		}
	};
	if (su_)
		out(su_->log(), &su_logged_);
	out(mdm_.log(), &logged_);
}

/* the connection's figures to the core (+CVM?, S44, S45) */
void VcvmPump::report()
{
	if (!info_)
		return;
	const VcvmLoading &t = mdm_.tx_loading(), &r = mdm_.rx_loading();
	info_->on = 1;
	info_->raw_tx = t.raw_bps();
	info_->raw_rx = r.raw_bps();
	info_->coded_tx = t.coded_bps();
	info_->coded_rx = r.coded_bps();
	info_->sym_tenths = static_cast<int>(r.sym_ms() * 10 + 0.5);
	info_->tones_tx = t.loaded();
	info_->tones_rx = r.loaded();
	info_->code_rate = r.rate;
	info_->renegs = mdm_.renegotiations();
	info_->erasures = mdm_.rx_stats().erasures;
	info_->v44_preset = mdm_.v44_preset();
	info_->zstd = mdm_.zstd();
}

/* cnt of the line's samples in, cnt out, through the modem at 8000 Hz:
   the renegotiations, retrains and CONNECT reported; DPSTAT_ERROR on a
   failure or no connection in time */
s16 VcvmPump::process(void *in, void *out, s32 cnt)
{
	const s16 *src = static_cast<s16 *>(in);
	s16 *dst = static_cast<s16 *>(out);
	int ret = DPSTAT_OK;
	/* the line's samples at 8000 Hz */
	in8_.clear();
	for (s32 i = 0; i < cnt; i++) {
		if (line_rate_ == 8000) {
			in8_.push_back(src[i]);
			continue;
		}
		float o[2];
		s16 k = rs_down_.run(src[i], o);
		for (s16 j = 0; j < k; j++)
			in8_.push_back(o[j]);
	}
	out8_.assign(in8_.size(), 0.0f);
	if (!in8_.empty())
		mdm_.process(in8_.data(), out8_.data(),
			     static_cast<s32>(in8_.size()));
	log_lines();
	if (line_rate_ == 8000) {
		for (s32 i = 0; i < cnt; i++)
			dst[i] = dsp_clip16(out8_[static_cast<size_t>(i)]);
	} else {
		/* 8000 -> 9600: what comes out a block, kept in back_ to
		   give exactly cnt */
		for (float x : out8_) {
			float o[2];
			s16 k = rs_up_.run(x, o);
			for (s16 j = 0; j < k; j++)
				back_.push_back(o[j]);
		}
		for (s32 i = 0; i < cnt; i++)
			dst[i] = back_pos_ < back_.size() ?
				dsp_clip16(back_[back_pos_++]) : 0;
		if (back_pos_ > 4096) {
			back_.erase(back_.begin(),
				    back_.begin() +
					    static_cast<s32>(back_pos_));
			back_pos_ = 0;
		}
	}
	total += cnt;
	if (mdm_.state() == VcvmModem::FAILED) {
		DSP_DBG("vcvm: %s\n", mdm_.why());
		return DPSTAT_ERROR;
	}
	if (mdm_.renegotiations() != renegs_) {
		renegs_ = mdm_.renegotiations();
		stat_reneg(false);
		if (up_) {
			set_rates(mdm_.tx_loading().coded_bps(),
				  mdm_.rx_loading().coded_bps());
			report();
		}
	}
	if (mdm_.retrains() != retrains_) {
		retrains_ = mdm_.retrains();
		stat_retrain(false);
	}
	if (!up_ && mdm_.connected()) {
		up_ = true;
		if (mdm_.rtt_ms() > 0)
			stat_rtd(static_cast<s32>(mdm_.rtt_ms()) * srate /
				 1000);
		report();
		DSP_DBG("vcvm: phase 4: transmitting %s, receiving %s\n",
			mdm_.tx_loading().str().c_str(),
			mdm_.rx_loading().str().c_str());
		ret = connect_at(mdm_.tx_loading().coded_bps(),
				 mdm_.rx_loading().coded_bps());
	}
	if (!up_ && total >= timeout) {
		DSP_DBG("vcvm: no connection in time\n");
		return DPSTAT_ERROR;
	}
	return ret;
}

/* registers the pump as DP_VCVM */
s16 dp_vcvm_init(void)
{
	return modem_dp_register(DP_VCVM, &PumpOps<VcvmPump>::ops);
}

/* and no more */
void dp_vcvm_exit(void)
{
	modem_dp_deregister(DP_VCVM, &PumpOps<VcvmPump>::ops);
}

} // namespace dsp
