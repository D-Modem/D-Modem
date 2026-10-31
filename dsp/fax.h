/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fax.h - fax modulations (V.21 channel 2, V.27ter, V.29, V.17) and
 *            the T.31 class 1 layer
 *
 */

#ifndef __FAX_H__
#define __FAX_H__

#include <cmath>
#include <vector>

#include "dm_types.h"
#include "qam.h"

namespace dsp {

enum fax_mod {
	FM_NONE,
	FM_V21,
	FM_V27_2400,
	FM_V27_4800,
	FM_V29_4800,
	FM_V29_7200,
	FM_V29_9600,
	FM_V17_7200,
	FM_V17_9600,
	FM_V17_12000,
	FM_V17_14400,
	FM_V26_A,               /* V.26 (4-wire leased): V.27ter 2400's
				   symbols, its data unscrambled */
	FM_V26_B,               /* and alternative B (Bell 201): the phase
				   changes 45 degrees on */
};

/* class 1 <mod> code -> modulation and training; returns 0 if unknown */
s16 fax_mod_from_code(u32 code, enum fax_mod *mod, s16 *short_train);
/* the data rate of a modulation, bit/s */
s32 fax_mod_bps(enum fax_mod mod);

/* the transmitter gets its data bits through this callback: 0/1, or
   FAX_BIT_END when there is no more data (it then shuts down) */
#define FAX_BIT_END (-1)
typedef s16 (*fax_get_bit_t)(void *ctx);
typedef void (*fax_put_bit_t)(void *ctx, s16 bit);

#define FAX_MAX_TRN 3100

struct FaxTx {
	enum fax_mod mod;
	s16 short_train;
	s32 srate;
	QamTx qam;
	FskBitTx fsk;                   /* V.21 */
	s16 seg;                        /* training segment */
	s32 seg_sym;                    /* symbols sent in this segment */
	u32 scr;                        /* data scrambler */
	s16 scr_count;                  /* V.27ter repeat pattern guard */
	u32 tscr;                       /* V.29 training scrambler */
	s16 state;                      /* differential phase state */
	TrellisEnc trellis;
	u16 bridge;                     /* V.17's segment 3 word (V.33: its
					   rate signal) */
	fax_get_bit_t get_bit;
	void *ctx;
	s16 ending;
	s16 done;

	/* past the training: the data path (the scrambled ones, then the
	   caller's bits) */
	bool in_data() const;

	s16 data_bit();
	cplx data_symbol();
	void next_segment(s16 seg_);
	/* mod_ at srate_ samples/s, the data bits from get_bit_(ctx_) */
	void init(enum fax_mod mod_, s16 short_train_, s32 srate_, fax_get_bit_t get_bit_, void *ctx_);
	/* the next output sample; done is set at the end of the signal */
	float sample();
};


struct FaxRxMemory;

struct FaxRx {
	enum fax_mod mod;
	s16 short_train;
	s32 srate;
	QamRx qam;
	FskDemod fsk;
	s16 nstates;                    /* phase states for training sync */
	/* searching for the training; in it (found on its reference
	   symbols); data */
	enum { RXS_SEARCH, RXS_TRAIN, RXS_DATA };
	s16 stage;
	s32 seg_sym;
	s16 last_state;
	s16 steps[32];
	s16 nsteps;
	/* reference training symbols (absolute phase states) */
	std::vector<signed char> ref;
	s16 ref_len;
	u32 scr;
	s16 scr_count;
	s16 state;
	Viterbi vit;
	s32 data_sym;                   /* trellis symbols fed to the decoder */
	float mse, mse_fast;
	float data_power;
	s16 carrier;
	float det_pwr, det_alpha;        /* fast power for carrier detection */
	float det_floor, det_level;     /* noise floor, signal level */
	s32 det_hold;                   /* samples before a carrier may start */
	float agc_alpha;                /* the normal AGC speed */
	s32 onset;                      /* symbols since the carrier came on */
	float sym_pow, mid_pow;         /* timing check on the alternations */
	cplx zh[2];                     /* the last symbols, newest first */
	s32 alt_run;                    /* length of the current alternation */
	s32 alt_end;                    /* symbol where the alternations ended */
	s32 carrier_run;
	s16 trained;                    /* data is flowing */
	cplx mem_eq[QAM_MAX_EQ];        /* equalizer from a long train */
	cplx good_eq[QAM_MAX_EQ];       /* recent equalizer with good data */
	cplx new_eq[QAM_MAX_EQ];
	float good_gain, new_gain;
	s16 have_good, have_new;
	float mem_gain;                 /* its AGC gain in the data */
	s16 use_mem;
	s16 bridge_hits;        /* V.17 short train: the bridge is there? */
	u16 bridge;             /* the V.17 bridge word expected (V.33: the
				   rate signal) */
	s16 strict_bridge;      /* the bridge's symbols are the reference's
				   (V.33), not the nearest A, B, C or D */
	/* how far the training symbols were from the reference: the sum
	   of |z - d|^2 / |d|^2 past its first 64, and their number; the
	   same over the bridge alone (4-wire leased: which rate it is) */
	float trn_err, bridge_err;
	s32 trn_n, bridge_n;
	s32 const_run;          /* data: the symbols in a row that were the
				   one before (an unmodulated carrier) */
	s32 symbols;            /* from the demodulator, in all */
	float zpow, zpow_slow;  /* received data power */
	fax_put_bit_t put_bit;
	void *ctx;

	s16 state_of(cplx z);
	s16 point_state(cplx p);
	cplx ref_point(s16 k);
	s16 make_reference();
	/* mod_ at srate_ samples/s, the data bits to put_bit_(ctx_) once
	   trained; mem: for a V.17 short train (may be NULL).  -1: no
	   memory */
	s16 init(enum fax_mod mod_, s16 short_train_, s32 srate_, fax_put_bit_t put_bit_, void *ctx_, const FaxRxMemory *mem);
	/* keep the trained equalizer for a later short train */
	void save(FaxRxMemory *mem);
	/* V.17: the bridge word w, its symbols checked against the
	   reference (strict): the reference made again.  -1: no memory */
	s16 set_bridge(u16 w, s16 strict);
	/* free what init took */
	void release();
	void bits_out(s16 q, s16 n, s16 msb_first);
	void data_start();
	void data_symbol();
	s16 window_match(s16 k);
	void track_alternations(cplx z);
	void symbol();
	/* an input sample; carrier and trained tell how it goes */
	void sample(float x);
};

/* what a V.17 short train needs from the previous long train */
struct FaxRxMemory {
	enum fax_mod mod;
	cplx eq[QAM_MAX_EQ];
	float agc_gain;
	s16 valid;
};

/* transmit levels (rms) of the fax signals, as the original library's */
#define FAX_CED_RMS 4525.0f
float fax_tx_rms(enum fax_mod mod);

/* the answering fax's CED (6.2.2.1/T.30): the answerer's silence, 3 s
   of 2100 Hz, its gap (dsp.h), then the frames */
#define FAX_CED_MS      3000
#define FAX_CED_END_MS  (ANS_SILENCE_MS + FAX_CED_MS + ANS_GAP_MS)

/* the sample of CED t samples after its start (0 in the silences) */
static inline float fax_ced_sample(s32 t, s32 srate)
{
	if (t < ms2samples(ANS_SILENCE_MS, srate) ||
	    t >= ms2samples(ANS_SILENCE_MS + FAX_CED_MS, srate))
		return 0;
	return FAX_CED_RMS * 1.41421f *
		sinf(2 * static_cast<float>(M_PI) * 2100 * t / srate);
}

} // namespace dsp

#endif /* __FAX_H__ */
