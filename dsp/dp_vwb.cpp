/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_vwb.cpp - the V.wb pump (DP_VWB, AT+MS=VWB or 200): one end of
 *                 vwb.h's wideband modem on a bare pair at 48000, 96000
 *                 or 192000 Hz, the caller on the lower band; the core's
 *                 bits its octets (the first bit the least significant),
 *                 CONNECT once data goes both ways, the rates again
 *                 after a retrain
 *
 */

#include <cinttypes>
#include <cstdlib>
#include <string>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"
#include "vwb.h"

namespace dsp {

enum class VwbSt { RUN };

class VwbPump : public PumpBase<VwbPump, VwbSt> {
public:
	static constexpr const char *name = "VWB";
	/* its name in the log */
	const char *tag() const { return "vwb"; }

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 process(void *in, void *out, s32 cnt) override;

private:
	u8 source();
	void sink(const u8 *d, s32 n);
	void log_lines();

	VwbModem mdm_;
	bool up_ = false;               /* CONNECT reported */
	s32 retrains_ = 0;
	s32 up_rates_[2] = {};          /* the rates the core has */
	std::string pending_;
};

/* the pump at srate_ (48000 Hz and up), calling or answering; S7 the
   time to a data connection (and to one again after a retrain): 0, or
   -1 */
s16 VwbPump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_,
		   s32)
{
	if (srate_ < 48000) {
		DSP_DBG("vwb: %" PRId32 " Hz: the wideband mode needs 48000 "
			"Hz or more\n", srate_);
		return -1;
	}
	pump_setup(m, srate_, caller_);
	VwbModem::Config c;
	c.fs = srate_;
	c.calling = caller_;
	c.timeout_s = static_cast<s32>(timeout / srate_);
	if (const char *e = pump_test_env("DSP_VWB_MAXBITS", caller_))
		c.max_bits = static_cast<s16>(std::strtol(e, nullptr, 10));
	if (mdm_.init(c, [this]() { return source(); },
		      [this](const u8 *d, s32 n) { sink(d, n); }))
		return -1;
	stat_dp(DP_VWB);
	log_lines();
	return 0;
}

/* an octet of the link layer's bits, the first the least significant
   (ones when it has none) */
u8 VwbPump::source()
{
	u8 b[8];
	std::fill_n(b, 8, 1);
	modem_get_bits(modem, 1, b, 8);
	u8 v = 0;
	for (s16 i = 0; i < 8; i++)
		v = static_cast<u8>(v | (b[i] & 1) << i);
	return v;
}

/* octets received, to the link layer, the first bit the least
   significant */
void VwbPump::sink(const u8 *d, s32 n)
{
	for (s32 i = 0; i < n; i++) {
		u8 b[8];
		for (s16 k = 0; k < 8; k++)
			b[k] = (d[i] >> k) & 1;
		modem_put_bits(modem, 1, b, 8);
	}
}

/* the modem's log lines to the DSP's log */
void VwbPump::log_lines()
{
	pending_ += mdm_.take_log();
	size_t at = 0;
	for (;;) {
		size_t e = pending_.find('\n', at);
		if (e == std::string::npos)
			break;
		DSP_DBG("vwb: %s\n", pending_.substr(at, e - at).c_str());
		at = e + 1;
	}
	pending_.erase(0, at);
}

/* cnt samples each way through the modem: CONNECT when data first goes
   both ways, the rates (and a retrain) after; ERROR when it fails (no
   data again within S7) */
s16 VwbPump::process(void *in, void *out, s32 cnt)
{
	s16 ret = DPSTAT_OK;
	mdm_.process(static_cast<const s16 *>(in), static_cast<s16 *>(out),
		     cnt);
	total += cnt;
	log_lines();
	if (mdm_.state() == VwbModem::FAILED) {
		DSP_DBG("vwb: %s\n", mdm_.why());
		return DPSTAT_ERROR;
	}
	if (mdm_.retrains() != retrains_) {
		retrains_ = mdm_.retrains();
		stat_retrain(false);
	}
	if (mdm_.connected()) {
		if (!up_) {
			up_ = true;
			stat_snr(mdm_.snr_db());
			ret = connect_at(mdm_.tx_rate(), mdm_.rx_rate());
		}
		if (up_rates_[0] != mdm_.tx_rate() ||
		    up_rates_[1] != mdm_.rx_rate()) {
			up_rates_[0] = mdm_.tx_rate();
			up_rates_[1] = mdm_.rx_rate();
			set_rates(up_rates_[0], up_rates_[1]);
		}
	}
	return ret;
}

/* registers the pump as DP_VWB */
s16 dp_vwb_init(void)
{
	return modem_dp_register(DP_VWB, &PumpOps<VwbPump>::ops);
}

/* and no more */
void dp_vwb_exit(void)
{
	modem_dp_deregister(DP_VWB, &PumpOps<VwbPump>::ops);
}

} // namespace dsp
