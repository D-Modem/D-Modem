/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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
#include <string.h>

#include <modem_dp.h>
#include <modem_param.h>
#include <dsplibs.h>

namespace dsp {

/* debug output (dsplibs_debug_printf), routed through modem_debug.cpp like
   the original library */
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
	int v8_for;             /* a forced V.21 caller heard 2100 Hz: V.8
				   (if it is ANSam) for V.21 alone, then
				   back to it */
	int v8_tried;
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

/* the complex number re + j im */
static inline cplx cplx_make(float re, float im) { cplx c = { re, im }; return c; }
/* a + b */
static inline cplx cplx_add(cplx a, cplx b) { return cplx_make(a.re + b.re, a.im + b.im); }
/* a - b */
static inline cplx cplx_sub(cplx a, cplx b) { return cplx_make(a.re - b.re, a.im - b.im); }
/* a * b */
static inline cplx cplx_mul(cplx a, cplx b)
{
	return cplx_make(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}
static inline cplx cplx_mulc(cplx a, cplx b) /* a * conj(b) */
{
	return cplx_make(a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im);
}
/* a times the real s */
static inline cplx cplx_scale(cplx a, float s) { return cplx_make(a.re * s, a.im * s); }
/* the power |a|^2 */
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

	/* freq in Hz (negative: the other way round), srate in samples/s;
	   the phase goes on from where it is */
	void  set_freq(float freq, int srate);
	float next();           /* sin(), then advance */
	cplx  next_cplx();      /* exp(j phase), then advance */
};

/* second order IIR section */
struct Biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;

	/* design for a centre or corner freq (Hz) and q at srate, and
	   clear the state */
	void bandpass(float freq, float q, int srate);
	void lowpass(float freq, float q, int srate);
	void highpass(float freq, float q, int srate);
	/* one sample through the section (transposed direct form II) */
	float run(float x)
	{
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
private:
	/* coefficients normalized by a0, state cleared */
	void set(double b0, double b1, double b2, double a0, double a1,
		 double a2);
};

/* a forced V.21 caller's first block: after a 2100 Hz answer tone, once
   a call, to the V.8 pump (dsp_glue.cpp) */
int dp_v8_first(struct modem *m, enum DP_ID id);

/* block Goertzel tone power estimator */
struct Goertzel {
	float coef;
	float s1, s2;
	int n, len;
	float power;    /* last complete block: mean square of the tone */

	/* tone freq (Hz), block of len samples at srate */
	void init(float freq, int len, int srate);
	int  run(float x);      /* 1 when a block is complete */
};

/* single pole power tracker */
struct PowerMeter {
	float alpha;
	float power;

	/* time constant in seconds at srate; power back to 0 */
	void init(float time_const_sec, int srate);
	/* one sample in, the running mean of x^2 out */
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

	/* one bit (0/1) in, the line bit out */
	int scramble(int bit);
	/* one line bit in, the data bit out */
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

/* timing helpers: ms milliseconds as a number of samples at srate */
static inline long ms2samples(int ms, int srate) { return (long)ms * srate / 1000; }

/*
 *    FSK modulator / demodulator (dp_fsk.cpp), also used for caller ID
 */
struct FskMod {
	Osc osc;
	float mark, space;
	float amp;
	int srate;

	/* mark and space in Hz, amp the peak sample value */
	void  init(float mark, float space, float amp, int srate);
	/* the next sample of the tone for bit (1 mark, 0 space) */
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

	/* mark and space in Hz, baud in bit/s */
	void init(float mark, float space, float baud, int srate);
	/* returns the recovered bit (0/1) at the bit center, or -1 */
	int  run(float x);
};

/* an FSK transmitter on its own bit clock: at each bit boundary the
   next bit from a source, next() (0 or 1; < 0 when it has no more,
   which ends the signal) */
struct FskBitTx {
	FskMod mod;
	float phase, inc;       /* the bit clock: a bit due at 1 */
	int bit;                /* the one going out */
	int ended;              /* the source had no more */

	/* mark and space (Hz), peak amplitude amp, baud bit/s at srate;
	   the clock from phase_ (1: the first bit at once), a mark until
	   then */
	void init(float mark, float space, float amp, float baud, int srate,
		  float phase_ = 0)
	{
		mod.init(mark, space, amp, srate);
		phase = phase_;
		inc = baud / srate;
		bit = 1;
		ended = 0;
	}
	/* the next sample begins a bit */
	bool edge() const { return phase + inc >= 1; }
	/* the next sample; 0 once ended */
	template <class Next> float sample(Next next)
	{
		phase += inc;
		if (phase >= 1) {
			int b;
			phase -= 1;
			b = next();
			if (b < 0) {
				ended = 1;
				return 0;
			}
			bit = b & 1;
		}
		return mod.next(bit);
	}
};

/* the answerer's start (4.3/V.25, 8.2/V.8): 200 ms of silence (V.8's
   0.2 s at least; the core's answer delay, its S-register of 2 s, is
   V.25's silent interval of 1.8 to 2.5 s, the answer tone then within
   it), the 2100 Hz answer tone, 75 ms of silence */
#define ANS_SILENCE_MS  200
#define ANS_TONE_MS     3300
#define ANS_GAP_MS      75

/* a carrier detector with hysteresis on a power: on above one level,
   off below another */
struct CarrierDetect {
	float on_level, off_level;
	int on;

	/* the levels as the amplitudes of tones */
	void init(float on_amp, float off_amp)
	{
		on_level = tone_power(on_amp);
		off_level = tone_power(off_amp);
		on = 0;
	}
	/* the power now: 1 as the carrier comes, -1 as it goes, else 0 */
	int run(float power)
	{
		if (!on && power > on_level) {
			on = 1;
			return 1;
		}
		if (on && power < off_level) {
			on = 0;
			return -1;
		}
		return 0;
	}
};

/* a Bell 103 caller's mark and space (1270 and 1070 Hz, its originate
   band) with most of the power in them, in blocks of 20 ms */
struct Bell103Detect {
	Goertzel g1270, g1070;
	float energy;           /* of the block so far */
	int blocks;             /* in a row with the tones */

	void init(int srate)
	{
		g1270.init(1270, srate / 50, srate);
		g1070.init(1070, srate / 50, srate);
		energy = 0;
		blocks = 0;
	}
	void run(float x)
	{
		energy += x * x;
		g1070.run(x);
		if (g1270.run(x)) {
			float mean = energy / g1270.len;
			blocks = g1270.power + g1070.power > 0.6f * mean &&
				mean > tone_power(150) ? blocks + 1 : 0;
			energy = 0;
		}
	}
	/* 100 ms of it */
	bool found() const { return blocks >= 5; }
};

/* the data bits' end in the modem core, for the parts of a pump that
   send or receive them: get() what to send (ones when the core has
   none), put() what came */
struct CoreBits {
	struct modem *modem;

	void get(unsigned char *bits, int n) const
	{
		memset(bits, 1, n);
		modem_get_bits(modem, 1, bits, n);
	}
	void put(const unsigned char *bits, int n) const
	{
		modem_put_bits(modem, 1, (u8 *)bits, n);
	}
};

/*
 *    data pump registration helpers: *_init registers the pump's DP_IDs
 *    with the modem core (prop_dp_init), *_exit takes them back
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

	/* 0-9, *, #, A-D (or a-d); -1 for anything else */
	int   set(char digit, int srate);
	/* the next sample of the dual tone */
	float next();
};

/* DTMF detector (the voice object): Goertzel filters at the eight
   frequencies over blocks of 25.6 ms; a digit is on after two blocks of
   it in a row (51 ms, Q.24's 40 ms tones and V.253's 25 ms minimum),
   off after a block without it or at a quarter of its level */
struct DtmfDetect {
	enum Event { NONE, ON, OFF };

	int n_block;            /* samples a block */
	float coef[8];          /* 2 cos(2 pi f / srate) */
	float s1[8], s2[8];
	float energy;
	int n;
	char last;              /* the last block's digit, 0 if none */
	char digit;             /* the digit on, 0 if none */
	float ref;              /* its first block's energy */

	/* at srate, nothing on */
	void init(int srate);
	/* one sample: ON when a digit comes on (digit has it), OFF when it
	   goes, else NONE */
	Event sample(float x);
	/* the digit of a block of energy e and Goertzel powers p, 0 if it
	   has none */
	char classify(float e, const float *p) const;
};

/* call progress tones (E.180) after dialling (the voice object, V.253
   9.1.1): the ringing tone, steady 400 to 480 Hz (alone, or 440 + 480 Hz
   or 400 + 450 Hz) on for 0.6 s or more (the UK's 0.4 + 0.4 s with its
   0.2 s gap is one ring), off 0.3 s or more between rings; and a busy
   or congestion tone's cadence, four rounds of 0.2 to 0.8 s on and off
   in the band of call progress tones.  On blocks of 20 ms. */
struct CallToneDetect {
	Goertzel g[5];          /* 400, 425, 440, 450, 480 Hz */
	Biquad band;            /* the busy tones' band, around 480 Hz */
	float energy, band_energy;
	int n, len;
	/* the ringing tone: blocks of the segment that goes on (gaps of 40
	   ms at most), off since its last block, of segments of 0.3 s or
	   more in the burst; blocks since the ring sounded last */
	int seg, drop, burst;
	long since_ring;
	/* the busy tone's cadence: the band on, blocks of its run, rounds */
	int band_on, band_run, rounds;

	unsigned rings;         /* rings heard */
	bool ringing;           /* one sounds */
	unsigned busy;          /* busy cadences heard (each four rounds) */

	/* at srate, nothing heard */
	void init(int srate);
	/* one sample of the line */
	void sample(float x);
	/* ms since a ring sounded last (since the start if none did) */
	long quiet_ms() const { return since_ring * 20; }
	/* one block's verdicts */
	void block();
};

/* sample format helpers: the modem core always uses S16_LE */
/* x saturated to the s16 range (and truncated toward 0) */
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
