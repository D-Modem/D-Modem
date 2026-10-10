/*
 *
 *    fax.h - fax modulations (V.21 channel 2, V.27ter, V.29, V.17) and
 *            the T.31 class 1 layer
 *
 */

#ifndef __FAX_H__
#define __FAX_H__

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
int fax_mod_bps(enum fax_mod mod);

/* the transmitter gets its data bits through this callback: 0/1, or
   FAX_BIT_END when there is no more data (it then shuts down) */
#define FAX_BIT_END (-1)
typedef int (*fax_get_bit_t)(void *ctx);
typedef void (*fax_put_bit_t)(void *ctx, int bit);

#define FAX_MAX_TRN 3100

struct fax_tx {
	enum fax_mod mod;
	int short_train;
	int srate;
	QamTx qam;
	FskMod fsk;
	float bit_phase, bit_inc;       /* V.21 bit clock */
	int fsk_bit;
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
};

void  fax_tx_init(struct fax_tx *t, enum fax_mod mod, int short_train,
		  int srate, fax_get_bit_t get_bit, void *ctx);
float fax_tx_sample(struct fax_tx *t);

struct fax_rx {
	enum fax_mod mod;
	int short_train;
	int srate;
	QamRx qam;
	FskDemod fsk;
	int nstates;                    /* phase states for training sync */
	int stage;
	long seg_sym;
	int last_state;
	int steps[32];
	int nsteps;
	/* reference training symbols (absolute phase states) */
	signed char *ref;
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
	float mem_gain;
	int use_mem;
	int bridge_hits;        /* V.17 short train: the bridge is there? */
	float zpow, zpow_slow;  /* received data power */
	fax_put_bit_t put_bit;
	void *ctx;
};

/* what a V.17 short train needs from the previous long train */
struct fax_rx_memory {
	enum fax_mod mod;
	cplx eq[QAM_MAX_EQ];
	float agc_gain;
	int valid;
};

int  fax_rx_init(struct fax_rx *r, enum fax_mod mod, int short_train,
		 int srate, fax_put_bit_t put_bit, void *ctx,
		 const struct fax_rx_memory *mem);
/* keep the trained equalizer for a later short train */
void fax_rx_save(struct fax_rx *r, struct fax_rx_memory *mem);
void fax_rx_free(struct fax_rx *r);
void fax_rx_sample(struct fax_rx *r, float x);

/* transmit levels (rms) of the fax signals, as the original library's */
#define FAX_CED_RMS 4525.0f
float fax_tx_rms(enum fax_mod mod);

} // namespace dsp

#endif /* __FAX_H__ */
