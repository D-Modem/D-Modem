/*
 *
 *    dsp.h - internal definitions of the open data pump library
 *
 *    This is a from-scratch C replacement for the interface exported by
 *    the binary-only dsplibs.o.  It provides the same entry points
 *    (prop_dp_init, dcr_*, dp_runtime_*, RD_*, CID_*, VOICE_*, FAX_*)
 *    and implements the low-speed data pumps: call progress, V.21,
 *    V.23, Bell 103, V.22, Bell 212A and V.22bis.
 *
 */

#ifndef __DSP_H__
#define __DSP_H__

#include <modem.h>
#include <modem_param.h>

/* debug output, routed through modem_debug.c like the original blob */
extern unsigned int dsplibs_debug_level;
extern int dsplibs_debug_printf(const char *fmt, ...);

#define DSP_DBG(fmt, arg...) do { if (dsplibs_debug_level > 1) \
	dsplibs_debug_printf("dsp: " fmt, ##arg); } while (0)
/* per symbol traces, only for development */
#define DSP_TRACE(fmt, arg...) do { if (dsplibs_debug_level > 4) \
	dsplibs_debug_printf("dsp: " fmt, ##arg); } while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* nominal transmit amplitude for 16 bit linear samples (about -12 dBFS) */
#define DSP_TX_AMPLITUDE 8000.0f

/*
 *    shared runtime object (dp_runtime_create)
 *
 *    It survives the switch from the call progress pump to the
 *    modulation pump, so the call progress pump can tell the data pump
 *    which answer tone it already heard.
 */
#define ANSTONE_NONE  0
#define ANSTONE_2100  1   /* ITU V.25 ANS / V.8 ANSam */
#define ANSTONE_2225  2   /* Bell answer tone */

struct dp_runtime {
	struct modem *modem;
	int answer_tone;
	long answer_tone_end;   /* sample counter when the tone was seen */
	int ans_sent;           /* the answer tone is already sent: an
				   automode fallback skips it */
	int v32_automode;       /* V.32bis started by the V.8/V.34 pump:
				   keep the V.22bis fallback */
	int law;                /* the G.711 network: 1 mu-law, 2 A-law, 0
				   unknown (then $DSP_G711) */
};

/*
 *    small DSP helpers (dsp_util.c)
 */

typedef struct {
	float re, im;
} cplx;

static inline cplx cplx_make(float re, float im) { cplx c = { re, im }; return c; }
static inline cplx cplx_add(cplx a, cplx b) { return cplx_make(a.re + b.re, a.im + b.im); }
static inline cplx cplx_sub(cplx a, cplx b) { return cplx_make(a.re - b.re, a.im - b.im); }
static inline cplx cplx_mul(cplx a, cplx b)
{
	return cplx_make(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}
static inline cplx cplx_mulc(cplx a, cplx b) /* a * conj(b) */
{
	return cplx_make(a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im);
}
static inline cplx cplx_scale(cplx a, float s) { return cplx_make(a.re * s, a.im * s); }
static inline float cplx_pow(cplx a) { return a.re * a.re + a.im * a.im; }

/* sine oscillator with phase continuity across frequency changes */
struct osc {
	double phase;   /* cycles, 0..1 */
	double inc;     /* cycles per sample */
};
void  osc_set_freq(struct osc *o, float freq, int srate);
float osc_next(struct osc *o);          /* sin(), then advance */
cplx  osc_next_cplx(struct osc *o);     /* exp(j phase), then advance */

/* second order IIR section */
struct biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};
void  biquad_bandpass(struct biquad *f, float freq, float q, int srate);
void  biquad_lowpass(struct biquad *f, float freq, float q, int srate);
void  biquad_highpass(struct biquad *f, float freq, float q, int srate);
static inline float biquad_run(struct biquad *f, float x)
{
	float y = f->b0 * x + f->z1;
	f->z1 = f->b1 * x - f->a1 * y + f->z2;
	f->z2 = f->b2 * x - f->a2 * y;
	return y;
}

/* block Goertzel tone power estimator */
struct goertzel {
	float coef;
	float s1, s2;
	int n, len;
	float power;    /* last complete block: mean square of the tone */
};
void goertzel_init(struct goertzel *g, float freq, int len, int srate);
int  goertzel_run(struct goertzel *g, float x); /* 1 when a block is complete */

/* single pole power tracker */
struct power_meter {
	float alpha;
	float power;
};
void power_meter_init(struct power_meter *p, float time_const_sec, int srate);
static inline float power_meter_run(struct power_meter *p, float x)
{
	p->power += p->alpha * (x * x - p->power);
	return p->power;
}

/* V.22 / V.22bis scrambler (1 + x^-14 + x^-17) */
struct v22_scrambler {
	unsigned reg;
	int ones;
};
int v22_scramble(struct v22_scrambler *s, int bit);
int v22_descramble(struct v22_scrambler *s, int bit);

/* root raised cosine impulse response, t in symbol periods */
float rrc_pulse(float t, float alpha);

/* rational sample rate converter L/M (polyphase FIR, 70 dB stop band,
   cut off at 0.484 of the lower of the two rates) */
#define RESAMPLER_MAX_TAPS 100
struct resampler {
	int L, M, taps, t;
	const float *h;         /* L * taps, phase major */
	float hist[2 * RESAMPLER_MAX_TAPS];
	int pos;
};
/* 8000 <-> 9600 Hz: (L, M) = (6, 5) or (5, 6) */
int  resampler_init(struct resampler *r, int L, int M);
/* one input sample in; returns the number of outputs (0 to 2) */
int  resampler_run(struct resampler *r, float x, float out[2]);

/* dB helper: power of a sine of amplitude 'a' */
static inline float tone_power(float a) { return a * a / 2; }

/* timing helpers */
static inline long ms2samples(int ms, int srate) { return (long)ms * srate / 1000; }

/*
 *    FSK modulator / demodulator (dp_fsk.c), also used for caller ID
 */
struct fsk_mod {
	struct osc osc;
	float mark, space;
	float amp;
	int srate;
};
void  fsk_mod_init(struct fsk_mod *f, float mark, float space, float amp, int srate);
float fsk_mod_next(struct fsk_mod *f, int bit);

struct fsk_demod {
	int srate;
	float sign;             /* +1 if mark is above space */
	float half_shift;
	struct biquad pre;      /* channel band pass */
	struct osc lo;          /* down converter */
	struct biquad lp_re[2], lp_im[2];
	cplx prev;
	struct power_meter pwr; /* base band power */
	float dev_avg;          /* average |frequency deviation| */
	float dev_alpha;
	float on_level, off_level;
	int carrier;
	/* bit clock recovery */
	float bit_phase, bit_inc, bit_gain;
	int last_dec;
};
void fsk_demod_init(struct fsk_demod *f, float mark, float space, float baud,
		    int srate);
/* returns the recovered bit (0/1) at the bit center, or -1 */
int  fsk_demod_run(struct fsk_demod *f, float x);

/*
 *    entry points used by the modem core (dsp_glue.c)
 */
int   prop_dp_init(void);
void  prop_dp_exit(void);
void *dp_runtime_create(struct modem *m);
void  dp_runtime_delete(void *runtime);

/*
 *    data pump registration helpers
 */
int  dp_callprog_init(void);
void dp_callprog_exit(void);
int  dp_fsk_init(void);
void dp_fsk_exit(void);
int  dp_v22_init(void);
void dp_v22_exit(void);
int  dp_v32_init(void);
void dp_v32_exit(void);
int  dp_v34_init(void);
void dp_v34_exit(void);

/* DTMF generator used by the call progress pump and the voice object */
struct dtmf_gen {
	struct osc lo, hi;
	float amp_lo, amp_hi;
};
int  dtmf_gen_set(struct dtmf_gen *d, char digit, int srate);
float dtmf_gen_next(struct dtmf_gen *d);

/* sample format helpers: the modem core always uses S16_LE */
static inline s16 dsp_clip16(float x)
{
	if (x > 32767.0f)
		return 32767;
	if (x < -32768.0f)
		return -32768;
	return (s16)x;
}

#endif /* __DSP_H__ */
