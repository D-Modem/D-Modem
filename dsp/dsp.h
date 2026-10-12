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

/* the interface to the modem core: the data pumps register and exchange
   bits through modem_dp.h, read and set parameters through modem_param.h,
   and dsplibs.h is the rest of the library's interface (both ways) */
#include <modem_dp.h>
#include <modem_param.h>
#include <dsplibs.h>

namespace dsp {

/* debug output (dsplibs_debug_printf), routed through modem_debug.cpp like
   the original blob */
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
	int after_v8;           /* the modulation was agreed in V.8: the
				   caller starts at once after CJ */
	int law;                /* the G.711 network: 1 mu-law, 2 A-law, 0
				   unknown (then $DSP_G711) */
	int pcm;                /* V.91: the received codes as they are
				   (no DC removal) */
};

/*
 *    small DSP helpers (dsp_util.cpp)
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

/*
 *    The building blocks below are classes with plain data: an object of
 *    one is ready to use zeroed (as part of a pump allocated with calloc)
 *    and is set up by its init method.
 */

/* sine oscillator with phase continuity across frequency changes */
struct Osc {
	double phase;   /* cycles, 0..1 */
	double inc;     /* cycles per sample */

	void  set_freq(float freq, int srate);
	float next();           /* sin(), then advance */
	cplx  next_cplx();      /* exp(j phase), then advance */
};

/* second order IIR section */
struct Biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;

	void bandpass(float freq, float q, int srate);
	void lowpass(float freq, float q, int srate);
	void highpass(float freq, float q, int srate);
	float run(float x)
	{
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
private:
	void set(double b0, double b1, double b2, double a0, double a1,
		 double a2);
};

/* block Goertzel tone power estimator */
struct Goertzel {
	float coef;
	float s1, s2;
	int n, len;
	float power;    /* last complete block: mean square of the tone */

	void init(float freq, int len, int srate);
	int  run(float x);      /* 1 when a block is complete */
};

/* single pole power tracker */
struct PowerMeter {
	float alpha;
	float power;

	void init(float time_const_sec, int srate);
	float run(float x)
	{
		power += alpha * (x * x - power);
		return power;
	}
};

/* V.22 / V.22bis scrambler (1 + x^-14 + x^-17) */
struct V22Scrambler {
	unsigned reg;
	int ones;

	int scramble(int bit);
	int descramble(int bit);
};

/* root raised cosine impulse response, t in symbol periods */
float rrc_pulse(float t, float alpha);

/* rational sample rate converter L/M (polyphase FIR, 70 dB stop band,
   cut off at 0.484 of the lower of the two rates) */
#define RESAMPLER_MAX_TAPS 100
struct Resampler {
	int L, M, taps, t;
	const float *h;         /* L * taps, phase major */
	float hist[2 * RESAMPLER_MAX_TAPS];
	int pos;

	/* 8000 <-> 9600 Hz: (L, M) = (6, 5) or (5, 6) */
	int init(int L, int M);
	/* one input sample in; returns the number of outputs (0 to 2) */
	int run(float x, float out[2]);
};

/* dB helper: power of a sine of amplitude 'a' */
static inline float tone_power(float a) { return a * a / 2; }

/* timing helpers */
static inline long ms2samples(int ms, int srate) { return (long)ms * srate / 1000; }

/*
 *    FSK modulator / demodulator (dp_fsk.cpp), also used for caller ID
 */
struct FskMod {
	Osc osc;
	float mark, space;
	float amp;
	int srate;

	void  init(float mark, float space, float amp, int srate);
	float next(int bit);
};

struct FskDemod {
	int srate;
	float sign;             /* +1 if mark is above space */
	float half_shift;
	Biquad pre;             /* channel band pass */
	Osc lo;                 /* down converter */
	Biquad lp_re[2], lp_im[2];
	cplx prev;
	PowerMeter pwr;         /* base band power */
	float dev_avg;          /* average |frequency deviation| */
	float dev_alpha;
	float on_level, off_level;
	int carrier;
	/* bit clock recovery */
	float bit_phase, bit_inc, bit_gain;
	int last_dec;

	void init(float mark, float space, float baud, int srate);
	/* returns the recovered bit (0/1) at the bit center, or -1 */
	int  run(float x);
};

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
struct DtmfGen {
	Osc lo, hi;
	float amp_lo, amp_hi;

	int   set(char digit, int srate);
	float next();
};

/* sample format helpers: the modem core always uses S16_LE */
static inline s16 dsp_clip16(float x)
{
	if (x > 32767.0f)
		return 32767;
	if (x < -32768.0f)
		return -32768;
	return (s16)x;
}

} // namespace dsp

#endif /* __DSP_H__ */
