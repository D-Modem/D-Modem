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
#include <cstdlib>
#include <deque>
#include <string>

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
	void sink(const u8 *d, s32 n);
	void log_lines();
	void run8k(float x);

	PepModem mdm_;
	bool up_ = false;               /* CONNECT reported */
	bool resample_ = false;         /* 9600 Hz */
	Resampler down_, up_rs_;
	std::deque<float> tx_;          /* at srate, to send */
	std::string pending_;
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
	c.timeout_s = static_cast<s32>(carrier_loss / srate_) + 20;
	if (const char *e = pump_test_env("DSP_PEP_MAXBITS", caller_))
		c.max_bits = static_cast<s16>(std::strtol(e, nullptr, 10));
	if (const char *e = pump_test_env("DSP_PEP_TURN", caller_))
		c.turn_max = static_cast<s16>(std::strtol(e, nullptr, 10));
	mdm_.init(c, [this](u8 *b, s32 n) { return source(b, n); },
		  [this](const u8 *d, s32 n) { sink(d, n); });
	stat_dp(DP_PEP);
	log_lines();
	DSP_DBG("pep: create %s, srate %" PRId32 "\n",
		caller_ ? "caller" : "answerer", srate_);
	return 0;
}

/* n octets of the link layer's bits, the first the least significant:
   it always has them (ones when it has nothing of its own) */
s32 PepPump::source(u8 *buf, s32 n)
{
	u8 b[8];
	for (s32 i = 0; i < n; i++) {
		std::fill_n(b, 8, 1);
		modem_get_bits(modem, 1, b, 8);
		u8 v = 0;
		for (s16 k = 0; k < 8; k++)
			v = static_cast<u8>(v | (b[k] & 1) << k);
		buf[i] = v;
	}
	return n;
}

/* octets received, to the link layer, the first bit the least
   significant */
void PepPump::sink(const u8 *d, s32 n)
{
	for (s32 i = 0; i < n; i++) {
		u8 b[8];
		for (s16 k = 0; k < 8; k++)
			b[k] = (d[i] >> k) & 1;
		modem_put_bits(modem, 1, b, 8);
	}
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

/* cnt samples each way through the modem: CONNECT once both ways are
   on their own loadings; ERROR when it fails, or no CONNECT within S7 */
s16 PepPump::process(void *in, void *out, s32 cnt)
{
	const s16 *src = static_cast<const s16 *>(in);
	s16 *dst = static_cast<s16 *>(out);
	s16 ret = DPSTAT_OK;
	for (s32 i = 0; i < cnt; i++) {
		if (resample_) {
			float y[2];
			s16 n = down_.run(src[i], y);
			for (s16 k = 0; k < n; k++)
				run8k(y[k]);
		} else
			run8k(src[i]);
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
		stat_line(static_cast<float>(pep::FS) / pep::EPOCH, 0,
			  static_cast<float>(pep::FS) / pep::EPOCH, 0);
		ret = connect_at(mdm_.duplex_rate(false),
				 mdm_.duplex_rate(true));
	}
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
