/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    qam_rx_core.h - what the QAM pumps' receivers share (V.26 ter,
 *                    V.26 bis, V.32, V.61, HST), a mixin on the pump P
 *                    (CRTP): the QAM receiver and its carrier detector,
 *                    a sample through them, and the receiver's reset.
 *
 *    P gives:
 *      void rx_symbol()        a symbol received (rx.z)
 *      void carrier_came()     the carrier detector's change to on ...
 *      void carrier_went()     ... and to off
 *    and is a PumpBase (carrier_tick()).
 *
 */

#ifndef DSP_QAM_RX_CORE_H
#define DSP_QAM_RX_CORE_H

#include "dm_types.h"
#include "dsp.h"
#include "qam.h"

namespace dsp {

template <class P>
struct QamRxCore {
	QamRx rx;
	CarrierDetect carrier;

	/* one received sample x (the echo cancelled): a symbol to P when
	   one is done, the carrier's changes to P, the carrier counted */
	void qam_rx(float x)
	{
		P &p = static_cast<P &>(*this);
		if (rx.sample(x))
			p.rx_symbol();
		switch (carrier.run(rx.power)) {
		case 1:
			p.carrier_came();
			break;
		case -1:
			p.carrier_went();
			break;
		}
		p.carrier_tick(carrier.on);
	}
	/* the receiver from its start again, the gain free */
	void qam_rx_reset()
	{
		rx.reset();
		rx.agc_hold = 0;
	}

protected:
	~QamRxCore() = default;
};

} // namespace dsp

#endif
