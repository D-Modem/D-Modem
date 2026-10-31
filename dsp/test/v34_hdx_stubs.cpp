/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v34_hdx_stubs.cpp - half-duplex V.34 for the class 1 tests built
 *    without the modem DSP: there is none, so class 1.0 never brings
 *    one up
 */

#include "../v34.h"

using namespace dsp;

/* no modem to make */
V34Hdx *dsp::v34_hdx_create(struct modem *, s16, s16, s32,
                            const V34HdxHost *) { return nullptr; }
/* no samples taken: failed */
s16 dsp::v34_hdx_process(V34Hdx *, s16 *, s16 *, s32) { return DPSTAT_ERROR; }
/* no primary channel to ask for */
void dsp::v34_hdx_primary(V34Hdx *) {}
/* no rates to limit */
void dsp::v34_hdx_rates(V34Hdx *, s16, s16) {}
/* never on */
s16 dsp::v34_hdx_on(const V34Hdx *) { return 0; }
/* no source */
s16 dsp::v34_hdx_source(const V34Hdx *) { return 0; }
/* no rate */
s32 dsp::v34_hdx_rate(const V34Hdx *) { return 0; }
/* nothing to free */
void dsp::v34_hdx_free(V34Hdx *) {}
