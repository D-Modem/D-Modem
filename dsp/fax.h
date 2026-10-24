/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fax.h - fax modulations (V.21 channel 2, V.27ter, V.29, V.17) and
 *            the T.31 class 1 layer
 *
 */

#ifndef __FAX_H__
#define __FAX_H__

#include <cmath>
#include <vector>

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
};

/* class 1 <mod> code -> modulation and training; returns 0 if unknown */
int fax_mod_from_code(int code, enum fax_mod *mod, int *short_train);
/* the data rate of a modulation, bit/s */
int fax_mod_bps(enum fax_mod mod);

/* the transmitter gets its data bits through this callback: 0/1, or
   FAX_BIT_END when there is no more data (it then shuts down) */
#define FAX_BIT_END (-1)
typedef int (*fax_get_bit_t)(void *ctx);
typedef void (*fax_put_bit_t)(void *ctx, int bit);

#define FAX_MAX_TRN 3100

struct FaxTx {
	enum fax_mod mod;
	int short_train;
	int srate;
	QamTx qam;
	FskBitTx fsk;                   /* V.21 */
	int seg;                        /* training segment */
	long seg_sym;                   /* symbols sent in this segment */
	unsigned scr;                   /* data scrambler */
	int scr_count;                  /* V.27ter repeat pattern guard */
	unsigned tscr;                  /* V.29 training scrambler */
	int state;                      /* differential phase state */
	TrellisEnc trellis;
	fax_get_bit_t get_bit;
	void *ctx;
	int ending;
	int done;

	int data_bit();
	cplx data_symbol();
	void next_segment(int seg_);
	/* mod_ at srate_ samples/s, the data bits from get_bit_(ctx_) */
	void init(enum fax_mod mod_, int short_train_, int srate_, fax_get_bit_t get_bit_, void *ctx_);
	/* the next output sample; done is set at the end of the signal */
	float sample();
};


struct FaxRxMemory;

struct FaxRx {
	enum fax_mod mod;
	int short_train;
	int srate;
	QamRx qam;
	FskDemod fsk;
	int nstates;                    /* phase states for training sync */
	/* searching for the training; in it (found on its reference
	   symbols); data */
	enum { RXS_SEARCH, RXS_TRAIN, RXS_DATA };
	int stage;
	long seg_sym;
	int last_state;
	int steps[32];
	int nsteps;
	/* reference training symbols (absolute phase states) */
	std::vector<signed char> ref;
	int ref_len;
	unsigned scr;
	int scr_count;
	int state;
	Viterbi vit;
	long data_sym;                  /* trellis symbols fed to the decoder */
	float mse, mse_fast;
	float data_power;
	int carrier;
	float det_pwr, det_alpha;        /* fast power for carrier detection */
	float det_floor, det_level;     /* noise floor, signal level */
	int det_hold;                   /* samples before a carrier may start */
	float agc_alpha;                /* the normal AGC speed */
	int onset;                      /* symbols since the carrier came on */
	float sym_pow, mid_pow;         /* timing check on the alternations */
	cplx zh[2];                     /* the last symbols, newest first */
	int alt_run;                    /* length of the current alternation */
	long alt_end;                   /* symbol where the alternations ended */
	long carrier_run;
	int trained;                    /* data is flowing */
	cplx mem_eq[QAM_MAX_EQ];        /* equalizer from a long train */
	cplx good_eq[QAM_MAX_EQ];       /* recent equalizer with good data */
	cplx new_eq[QAM_MAX_EQ];
	float good_gain, new_gain;
	int have_good, have_new;
	float mem_gain;                 /* its AGC gain in the data */
	int use_mem;
	int bridge_hits;        /* V.17 short train: the bridge is there? */
	float zpow, zpow_slow;  /* received data power */
	fax_put_bit_t put_bit;
	void *ctx;

	int state_of(cplx z);
	int point_state(cplx p);
	cplx ref_point(int k);
	int make_reference();
	/* mod_ at srate_ samples/s, the data bits to put_bit_(ctx_) once
	   trained; mem: for a V.17 short train (may be NULL).  -1: no
	   memory */
	int init(enum fax_mod mod_, int short_train_, int srate_, fax_put_bit_t put_bit_, void *ctx_, const FaxRxMemory *mem);
	/* keep the trained equalizer for a later short train */
	void save(FaxRxMemory *mem);
	/* free what init took */
	void release();
	void bits_out(int q, int n, int msb_first);
	void data_start();
	void data_symbol();
	int window_match(int k);
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
	int valid;
};

/* transmit levels (rms) of the fax signals, as the original library's */
#define FAX_CED_RMS 4525.0f
float fax_tx_rms(enum fax_mod mod);

/* the answering fax's CED (6.2.2.1/T.30): the answerer's silence, 3 s
   of 2100 Hz, its gap (dsp.h), then the frames */
#define FAX_CED_MS      3000
#define FAX_CED_END_MS  (ANS_SILENCE_MS + FAX_CED_MS + ANS_GAP_MS)

/* the sample of CED t samples after its start (0 in the silences) */
static inline float fax_ced_sample(long t, int srate)
{
	if (t < ms2samples(ANS_SILENCE_MS, srate) ||
	    t >= ms2samples(ANS_SILENCE_MS + FAX_CED_MS, srate))
		return 0;
	return FAX_CED_RMS * 1.41421f *
		sinf(2 * (float)M_PI * 2100 * t / srate);
}

} // namespace dsp

#endif /* __FAX_H__ */
