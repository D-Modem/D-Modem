/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t30_v34.cpp - V.34 half-duplex (v34.h) as T.30's Annex F modem
 *    (t30.h's t30_hdx_ops)
 */

#include "dm_types.h"
#include "dsplibs.h"
#include "t30.h"
#include "v34.h"

namespace dsp {

namespace {

/* (t30_hdx_ops) */
void *create(struct modem *modem, s16 caller, s16 sending, s32 srate,
	     const struct t30_hdx_host *h)
{
	V34HdxHost v = { h->ctx, h->get_bit, h->put_bit, h->event };
	return v34_hdx_create(modem, caller, sending, srate, &v);
}

/* (t30_hdx_ops) */
s16 process(void *m, const s16 *in, s16 *out, s32 n)
{
	s16 r = v34_hdx_process(static_cast<V34Hdx *>(m),
				const_cast<s16 *>(in), out, n);
	return r == DPSTAT_OK || r == DPSTAT_CONNECT ? 0 : -1;
}

/* (t30_hdx_ops) */
void primary(void *m)
{
	v34_hdx_primary(static_cast<V34Hdx *>(m));
}

/* (t30_hdx_ops) */
s16 on(void *m)
{
	return v34_hdx_on(static_cast<V34Hdx *>(m));
}

/* (t30_hdx_ops) */
s16 source(void *m)
{
	return v34_hdx_source(static_cast<V34Hdx *>(m));
}

/* (t30_hdx_ops) */
s32 rate(void *m)
{
	return v34_hdx_rate(static_cast<V34Hdx *>(m));
}

/* (t30_hdx_ops) */
void release(void *m)
{
	v34_hdx_free(static_cast<V34Hdx *>(m));
}

const struct t30_hdx_ops ops = {
	create, process, primary, on, source, rate, release,
};

} // namespace

/* (t30.h) */
const struct t30_hdx_ops *t30_v34(void)
{
	return &ops;
}

} // namespace dsp
