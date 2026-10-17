/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    loopback.cpp - two of the open data pumps back to back through a
 *    simulated line (loopback.h)
 *
 *    No modem core is linked into the test programs, so this file is
 *    the core the pumps see: it keeps the registered pumps, answers
 *    their parameter and S-register questions, gives their transmitters
 *    O.150's 2^23 - 1 pattern and keeps what their receivers deliver.
 *    loopback_run() then runs a caller (call progress first) and an
 *    answerer in blocks of 48 samples of simulated time.  Each direction
 *    of the line delays, may drift (a sample clock offset), band limits,
 *    scales and adds white noise relative to the signal it carries; each
 *    side also hears the echoes of its own signal from the near and the
 *    far hybrid.  After the run each side's bits since its CONNECT are
 *    compared with what the other side sent.
 *
 *    The numbers below (seeds, delays, filters, the noise's order, the
 *    error count's windows) are what the pump tests are calibrated
 *    against.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "dsp.h"
#include "modem_dp.h"
#include "modem_param.h"
#include "dsplibs.h"
#include "loopback.h"
#include "test_util.h"

using dsp::Biquad;
using dsp::dsp_clip16;

/* samples a block (the pumps' max_frag) */
static const int FRAG = 48;
/* bits kept of what each side sends and of what it receives */
static const long RECORD_BITS = 1L << 20;
/* ok(): the share of the bits expected that a call must carry, and the
   bit error rate it may have */
static const double CARRIED_MIN = 0.9;
static const double BER_MAX = 1e-3;

/* the line's delay each way, samples */
static const int DELAY_TO_ANSWERER = 37;
static const int DELAY_TO_CALLER = 53;
/* the near hybrid's echo delay, and the far hybrid's beyond the two
   line delays */
static const int NEAR_ECHO_DELAY = 20;
static const int FAR_HYBRID_DELAY = 8;
/* each side's own samples kept for the echoes (a power of two) */
static const int ECHO_HISTORY = 32768;

/* the error count: bits skipped after the CONNECT at most, checked at
   most, left unchecked at the end, and the fewest worth checking */
static const long SKIP_MAX = 2000;
static const long CHECK_MAX = 200000;
static const long TAIL = 64;
static const long CHECK_MIN = 300;
/* an alignment: a window of this many bits with fewer mismatches than
   MISMATCH_LIMIT; a slip once the run score exceeds SLIP_SCORE, looked
   for from this far back */
static const int WINDOW = 128;
static const int MISMATCH_LIMIT = 8;
static const long SLIP_SCORE = 24;
static const long SLIP_BACK = 65536;

/* the two modems the pumps are created for, by their side (0 the
   caller, 1 the answerer) */
struct modem {
	int side;
};

/* one side of the call as the core keeps it */
struct Side {
	struct dp *pump;        /* the current one, or none */
	void *runtime;          /* dp_runtime_create()'s, for all its pumps */
	int tx_rate, rx_rate;   /* as the pumps last set them */
	bool connected;
	long connect_at;        /* the CONNECT's block, its first sample */
	long rx_at_connect;     /* bits received by then */
	int requested;          /* MDMPRM_DP_REQUESTED, 0: none */
	unsigned prbs;          /* the pattern's last 23 bits, bit 0 the
				   latest */
	std::vector<u8> sent;   /* the first RECORD_BITS bits sent */
	std::vector<u8> received;       /* and received, one a byte */
};

static const char *const side_name[2] = { "caller", "answerer" };
static struct modem modems[2] = { { 0 }, { 1 } };
static Side sides[2];

/* the registered pumps by id, for the whole process (some ids are
   above 128) */
static struct dp_operations *pump_ops[256];

/* the current block's first sample, and the first sample from which
   received bits are dropped (-1: none) */
static long block_at;
static long cut_at = -1;

/* what the core hands out: no voice settings, nothing to dial */
static struct voice_info voice;
static const char dial_string[] = "";

/* the side of modem m */
static Side &side_of(struct modem *m)
{
	return sides[m->side];
}

/* the next bit of O.150's 2^23 - 1 pattern (x^23 + x^18 + 1, not
   inverted): the XOR of the bits 23 and 18 back, which are bits 22 and
   17 of the register */
static int prbs_next(unsigned &r)
{
	int b = ((r >> 22) ^ (r >> 17)) & 1;
	r = ((r << 1) | b) & 0x7fffff;
	return b;
}

/*
 *    The modem core towards the pumps
 */

/* op serves id from now on, whatever served it before: 0 (-1 for an id
   past the table) */
int modem_dp_register(enum DP_ID id, struct dp_operations *op)
{
	if ((unsigned)id >= 256)
		return -1;
	pump_ops[id] = op;
	return 0;
}

/* nothing serves id any more */
void modem_dp_deregister(enum DP_ID id, struct dp_operations *)
{
	if ((unsigned)id < 256)
		pump_ops[id] = nullptr;
}

/* n of the side's next pattern bits, one a byte, into buf (the first
   RECORD_BITS also into its record); n */
int modem_get_bits(struct modem *m, int, u8 *buf, int n)
{
	Side &s = side_of(m);

	for (int i = 0; i < n; i++) {
		buf[i] = prbs_next(s.prbs);
		if ((long)s.sent.size() < RECORD_BITS)
			s.sent.push_back(buf[i]);
	}
	return n;
}

/* n bits (the low bit of each byte) into the side's record, as long as
   it has room and the line is not cut; n */
int modem_put_bits(struct modem *m, int, u8 *buf, int n)
{
	Side &s = side_of(m);

	if (cut_at >= 0 && block_at >= cut_at)
		return n;
	for (int i = 0; i < n && (long)s.received.size() < RECORD_BITS; i++)
		s.received.push_back(buf[i] & 1);
	return n;
}

/* the side's runtime, an empty dial string, the voice settings, the
   calling tone wanted; -1 for anything else */
intptr_t modem_get_param(struct modem *m, unsigned name)
{
	switch (name) {
	case MDMPRM_DPRUNTIME:
		return (intptr_t)side_of(m).runtime;
	case MDMPRM_DIALSTR:
		return (intptr_t)dial_string;
	case MDMPRM_VOICEINFO:
		return (intptr_t)&voice;
	case GetCallingToneFlag:
		return 1;
	default:
		return -1;
	}
}

/* keeps the pump the side asks for and its rates, ignores the rest: 0 */
long modem_set_param(struct modem *m, unsigned name, int val)
{
	Side &s = side_of(m);

	switch (name) {
	case MDMPRM_DP_REQUESTED:
		s.requested = val;
		break;
	case MDMPRM_TX_RATE:
		s.tx_rate = val;
		break;
	case MDMPRM_RX_RATE:
		s.rx_rate = val;
		break;
	}
	return 0;
}

/* the S-registers the pumps read: 50 s for the handshake, 1.4 s of
   carrier loss, 95 ms DTMF and the dial timings; 0 for all others
   (blind dialling among them) */
int modem_get_sreg(struct modem *, unsigned int num)
{
	switch (num) {
	case SREG_DIAL_TONE_WAIT_TIME:
		return 5;
	case SREG_WAIT_CARRIER_AFTER_DIAL:
		return 50;
	case SREG_DIAL_PAUSE_TIME:
		return 2;
	case SREG_CARRIER_LOSS_DISCONNECT_TIME:
		return 14;
	case SREG_DTMF_DURATION:
		return 95;
	default:
		return 0;
	}
}

/* no DTE: what is sent to it is dropped */
int modem_send_to_tty(struct modem *, const char *, int n)
{
	return n;
}

/* and nothing comes from it */
int modem_recv_from_tty(struct modem *, char *, int)
{
	return 0;
}

/*
 *    The line
 */

/* a sample clock offset: one output for each input, the input read
   between its samples at lag samples behind the newest and drifting by
   step - 1 a sample, through 32 taps of a Blackman windowed sinc */
class ClockOffset {
public:
	/* for a clock step and a run of n samples: the lag and the
	   history sized so that the reading neither overtakes the input
	   nor falls off the end of what is kept */
	void init(double step_, long n)
	{
		long drift = (long)(fabs(step_ - 1) * n) + 64;
		long need;
		size_t size = 1024;

		step = step_;
		lag = 256;
		if (step > 1 && drift + 32 > lag)
			lag = drift + 32;
		need = lag + 64 + (step < 1 ? drift : 0);
		while ((long)size < need)
			size <<= 1;
		hist.assign(size, 0);
		mask = size - 1;
		count = 0;
	}

	/* x in, the interpolated sample out (0 while the taps are not
	   all within what has come and is kept) */
	float run(float x)
	{
		long newest = count++;
		double pos = newest * step - lag;
		long i = (long)floor(pos);

		hist[newest & mask] = x;
		if (i < 16 || i + 16 > newest ||
		    i - 15 <= newest - (long)hist.size())
			return 0;

		/* sin(pi (k - f)) is -(-1)^k sin(pi f), and the window's
		   second cosine is the first's double angle */
		double f = pos - i;
		double s = sin(M_PI * f);
		double acc = 0;
		for (int k = -15; k <= 16; k++) {
			double d = k - f;
			double sinc = d == 0 ? 1 :
				((k & 1) ? s : -s) / (M_PI * d);
			double c = cos(M_PI * d / 16.5);
			double win = 0.42 + 0.5 * c + 0.08 * (2 * c * c - 1);
			acc += sinc * win * hist[(i + k) & mask];
		}
		return (float)acc;
	}

private:
	std::vector<float> hist;
	size_t mask;
	long count;             /* inputs so far */
	double step;
	long lag;
};

/* one direction of the line: a delay, the clock offset, the band
   limit, the gain, then white noise snr dB below a slow estimate of the
   signal's power (held through its quiet parts) */
class LineDirection {
public:
	/* delay_n samples of delay, a clock step (1: none) over a run of
	   n samples, the rest from cfg */
	void init(int delay_n, double step, long n, const LoopbackConfig &cfg)
	{
		delay.assign(delay_n, 0);
		delay_at = 0;
		drift = step != 1;
		if (drift)
			clock.init(step, n);
		flat = cfg.flat;
		lowpass.lowpass(3000, 0.6f, cfg.srate);
		gain = cfg.gain;
		noise = pow(10.0, -cfg.snr / 20.0);
		power = 0;
		ref = 0;
	}

	/* a sample of the sender's into the line, the sample at the far
	   end out (one gauss() drawn each time) */
	float run(float x)
	{
		float y = delay[delay_at];

		delay[delay_at] = x;
		if (++delay_at == delay.size())
			delay_at = 0;
		if (drift)
			y = clock.run(y);
		if (!flat)
			y = lowpass.run(y);
		y *= gain;
		power += ((double)y * y - power) / 2400;
		if (power > 0.05 * ref)
			ref = power;
		return (float)(y + sqrt(ref) * noise * gauss());
	}

private:
	std::vector<float> delay;
	size_t delay_at;
	bool drift;
	ClockOffset clock;
	bool flat;
	Biquad lowpass;
	float gain;
	double noise;           /* the noise's rms for a reference of 1 */
	double power;           /* the signal's slow mean square */
	double ref;             /* the one the noise follows */
};

/* what a side hears of its own signal: the near hybrid's echo and the
   far one's, each through a band pass of its own */
class Echoes {
public:
	void init(const LoopbackConfig &cfg)
	{
		own.assign(ECHO_HISTORY, 0);
		count = 0;
		near_bp.bandpass(1500, 0.5f, cfg.srate);
		far_bp.bandpass(1500, 0.4f, cfg.srate);
		near_gain = cfg.echo_db < 0 ? pow(10.0, cfg.echo_db / 20.0) : 0;
		far_gain = cfg.far_db < 0 ?
			pow(10.0, cfg.far_db / 20.0) * cfg.gain : 0;
		far_n = DELAY_TO_ANSWERER + DELAY_TO_CALLER + FAR_HYBRID_DELAY +
			std::min(std::max(cfg.far_delay, 0),
				 ECHO_HISTORY - 200);
	}

	/* the side's sample x in, both echoes at its input out */
	float run(float x)
	{
		long n = count++;

		own[n & (ECHO_HISTORY - 1)] = x;
		float e_near = near_bp.run(own[(n - NEAR_ECHO_DELAY) &
					       (ECHO_HISTORY - 1)]);
		float e_far = far_bp.run(own[(n - far_n) & (ECHO_HISTORY - 1)]);
		return near_gain * e_near + far_gain * e_far;
	}

private:
	std::vector<float> own; /* the side's samples, the last ones */
	long count;
	Biquad near_bp, far_bp;
	float near_gain, far_gain;
	long far_n;             /* the far echo's delay */
};

/* the whole line: the two directions and each side's echoes */
class Line {
public:
	void init(const LoopbackConfig &cfg, long n)
	{
		double offset = cfg.ppm * 1e-6;

		to_answerer.init(DELAY_TO_ANSWERER, 1 + offset, n, cfg);
		to_caller.init(DELAY_TO_CALLER, 1 - offset, n, cfg);
		for (Echoes &e : echoes)
			e.init(cfg);
	}

	/* a block of what each side sent (out) into what each side hears
	   in the next (in), saturated to 16 bits; the noise towards the
	   answerer drawn first for each sample */
	void run(const s16 out[2][FRAG], s16 in[2][FRAG])
	{
		for (int i = 0; i < FRAG; i++) {
			float a = to_answerer.run(out[0][i]);
			float c = to_caller.run(out[1][i]);
			in[1][i] = dsp_clip16(a + echoes[1].run(out[1][i]));
			in[0][i] = dsp_clip16(c + echoes[0].run(out[0][i]));
		}
	}

private:
	LineDirection to_answerer, to_caller;
	Echoes echoes[2];
};

/*
 *    The bit errors
 */

/* one side's bits received since its CONNECT against what the other
   side sent: the bits checked, the errors among them and the slips the
   count realigned after */
class ErrorCount {
public:
	ErrorCount(const std::vector<u8> &rx_, const std::vector<u8> &tx_,
		   const char *name_)
		: rx(rx_), tx(tx_), rx_words(pack(rx_)), tx_words(pack(tx_)),
		  name(name_), show(getenv("SHOW_ERRORS") != nullptr)
	{
	}

	/* the bits from rx's bit connect on into r's checked, errors and
	   resyncs */
	void count(long connect, LoopbackSide &r)
	{
		long nrx = rx.size(), ntx = tx.size();
		long start = connect + std::min((nrx - connect) / 4, SKIP_MAX);
		long len = std::min(nrx - TAIL - start, CHECK_MAX);
		long score = 0;

		r.checked = r.errors = r.resyncs = 0;
		if (len < CHECK_MIN)
			return;
		long j = start, o = align(start, 0);
		if (o < 0) {
			r.checked = len;
			r.errors = len / 2;
			return;
		}
		while (r.checked < len && o < ntx) {
			r.checked++;
			if (rx[j] != tx[o]) {
				r.errors++;
				score++;
				if (show)
					fprintf(stderr, "%s: bit error at "
						"%ld\n", name, j);
			} else if (score > 0) {
				score--;
			}
			j++;
			o++;
			if (score <= SLIP_SCORE)
				continue;
			long at = align(j, std::max(o - SLIP_BACK, 0L));
			if (at < 0)
				continue;
			if (show)
				fprintf(stderr, "%s: realigned by %ld bits "
					"at %ld\n", name, at - o, j);
			o = at;
			r.resyncs++;
			r.errors -= score;
			score = 0;
		}
	}

private:
	const std::vector<u8> &rx, &tx;
	std::vector<uint64_t> rx_words, tx_words;
	const char *name;
	bool show;              /* SHOW_ERRORS */

	/* the bits, 64 a word (bit i at bit i % 64 of word i / 64), with
	   two words of zeros after them */
	static std::vector<uint64_t> pack(const std::vector<u8> &bits)
	{
		std::vector<uint64_t> w(bits.size() / 64 + 2, 0);
		for (size_t i = 0; i < bits.size(); i++)
			w[i / 64] |= (uint64_t)(bits[i] & 1) << (i % 64);
		return w;
	}

	/* the 64 bits from bit pos on */
	static uint64_t bits64(const std::vector<uint64_t> &w, long pos)
	{
		size_t i = pos / 64;
		int sh = pos % 64;
		uint64_t v = w[i] >> sh;
		if (sh)
			v |= w[i + 1] << (64 - sh);
		return v;
	}

	/* whether the len bits of rx from j can be that close to a stretch
	   of the pattern: each wrong bit fails at most three of the
	   pattern's own checks (a bit the XOR of those 23 and 18 before
	   it), so more failures than that rule out every offset */
	bool can_match(long j, long len) const
	{
		int fails = 0;
		for (long n = 23; n < len; n++)
			fails += rx[j + n] ^ rx[j + n - 23] ^ rx[j + n - 18];
		return fails <= 3 * (MISMATCH_LIMIT - 1);
	}

	/* the lowest offset of tx from from on, with WINDOW + 1 bits after
	   it, where the next WINDOW bits of rx from j (fewer at its end)
	   differ in fewer than MISMATCH_LIMIT places; -1 if none */
	long align(long j, long from) const
	{
		long len = std::min((long)WINDOW, (long)rx.size() - j);
		long last = (long)tx.size() - (WINDOW + 1);

		if (len <= 0 || !can_match(j, len))
			return -1;
		uint64_t m0 = len >= 64 ? ~0ULL : (1ULL << len) - 1;
		uint64_t m1 = len >= 128 ? ~0ULL :
			len > 64 ? (1ULL << (len - 64)) - 1 : 0;
		uint64_t r0 = bits64(rx_words, j) & m0;
		uint64_t r1 = bits64(rx_words, j + 64) & m1;
		for (long o = from; o <= last; o++) {
			int miss = __builtin_popcountll((bits64(tx_words, o) ^
							 r0) & m0);
			if (miss >= MISMATCH_LIMIT)
				continue;
			miss += __builtin_popcountll((bits64(tx_words, o + 64) ^
						      r1) & m1);
			if (miss < MISMATCH_LIMIT)
				return o;
		}
		return -1;
	}
};

/*
 *    The run
 */

/* side k's pump id created (calling for the caller) at srate, in the
   block at sample t; false, and no pump, if id has none or it cannot be
   created */
static bool start_pump(int k, int id, long t, int srate)
{
	struct dp_operations *op = id > 0 && id < 256 ? pump_ops[id] :
		nullptr;

	sides[k].pump = op ? op->create(&modems[k], (enum DP_ID)id, k == 0,
					 srate, FRAG, op) : nullptr;
	if (sides[k].pump)
		return true;
	printf("%.2f s: %s has no data pump %d\n", (double)t / srate,
	       side_name[k], id);
	return false;
}

/* side k's pump gone */
static void stop_pump(int k)
{
	struct dp *d = sides[k].pump;

	if (d)
		d->op->destroy(d);
	sides[k].pump = nullptr;
}

/* side k's pump returned st in the block at sample t: a CONNECT noted,
   a change of pump made, anything else the pump's failure */
static void pump_status(int k, int st, long t, const LoopbackConfig &cfg,
			LoopbackResult &res)
{
	Side &s = sides[k];
	double at = (double)t / cfg.srate;

	if (st == DPSTAT_OK)
		return;
	if (st == DPSTAT_CONNECT) {
		s.connected = true;
		s.connect_at = t;
		s.rx_at_connect = s.received.size();
		printf("%.2f s: %s connected at %d/%d bit/s\n", at,
		       side_name[k], s.tx_rate, s.rx_rate);
		return;
	}
	stop_pump(k);
	if (st == DPSTAT_CHANGEDP) {
		/* the pump asked for, else the caller's modulation after the
		   call progress */
		int id = s.requested;
		if (!id && k == 0)
			id = cfg.caller_dp ? cfg.caller_dp : cfg.dp;
		s.requested = 0;
		printf("%.2f s: %s to data pump %d\n", at, side_name[k], id);
		if (start_pump(k, id, t, cfg.srate))
			return;
	} else {
		printf("%.2f s: %s's data pump ended with status %d\n", at,
		       side_name[k], st);
	}
	res.dp_failed = true;
}

/* side k's result at the end of the run, its bits compared with what
   the other side sent */
static void side_result(int k, const LoopbackConfig &cfg, LoopbackSide &r)
{
	const Side &s = sides[k];

	r.tx_rate = s.tx_rate;
	r.rx_rate = s.rx_rate;
	r.received = s.received.size();
	if (!s.connected)
		return;
	r.connected = true;
	r.connect_s = (double)s.connect_at / cfg.srate;
	r.carried = r.received - s.rx_at_connect;
	r.expected = std::min((long)(r.rx_rate * (cfg.secs - r.connect_s)),
			      RECORD_BITS - s.rx_at_connect);
	printf("%s: %ld of %ld bits carried (%.1f%%)\n", side_name[k],
	       r.carried, r.expected,
	       r.expected ? 100.0 * r.carried / r.expected : 0.0);
	ErrorCount(s.received, sides[1 - k].sent, side_name[k])
		.count(s.rx_at_connect, r);
}

/* see loopback.h */
LoopbackResult loopback_run(const LoopbackConfig &cfg)
{
	static bool registered;
	static const unsigned seed[2] = { 0x7654321, 0x1234567 };
	LoopbackResult res;
	FILE *dump[2] = { nullptr, nullptr };
	s16 in[2][FRAG], out[2][FRAG];
	long n = (long)(cfg.secs * cfg.srate);
	Line line;

	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (cfg.dump) {
		std::string name = cfg.dump;
		dump[0] = fopen((name + ".c").c_str(), "wb");
		dump[1] = fopen((name + ".a").c_str(), "wb");
	}
	srand(1);

	for (int k = 0; k < 2; k++) {
		Side &s = sides[k];
		s.pump = nullptr;
		s.runtime = dp_runtime_create(&modems[k]);
		s.tx_rate = s.rx_rate = 0;
		s.connected = false;
		s.connect_at = s.rx_at_connect = 0;
		s.requested = 0;
		s.prbs = seed[k] & 0x7fffff;
		s.sent.clear();
		s.sent.reserve(RECORD_BITS);
		s.received.clear();
		s.received.reserve(RECORD_BITS);
	}
	if (!registered) {
		prop_dp_init();
		registered = true;
	}
	block_at = 0;
	cut_at = cfg.cut_s > 0 ? (long)(cfg.cut_s * cfg.srate) : -1;
	line.init(cfg, n);
	memset(in, 0, sizeof(in));

	if (!start_pump(0, DP_CALLPROG, 0, cfg.srate))
		res.dp_failed = true;
	if (!start_pump(1, cfg.dp, 0, cfg.srate))
		res.dp_failed = true;

	for (long t = 0; t < n; t += FRAG) {
		block_at = t;
		for (int k = 0; k < 2; k++) {
			struct dp *d = sides[k].pump;
			if (!d) {
				memset(out[k], 0, sizeof(out[k]));
				continue;
			}
			pump_status(k, d->op->process(d, in[k], out[k], FRAG),
				    t, cfg, res);
		}
		line.run(out, in);
		for (int k = 0; k < 2; k++)
			if (dump[k])
				fwrite(in[k], sizeof(s16), FRAG, dump[k]);
	}

	for (int k = 0; k < 2; k++)
		if (dump[k])
			fclose(dump[k]);
	for (int k = 0; k < 2; k++)
		side_result(k, cfg, res.side[k]);
	for (int k = 0; k < 2; k++) {
		Side &s = sides[k];
		stop_pump(k);
		dp_runtime_delete(s.runtime);
		s.runtime = nullptr;
		std::vector<u8>().swap(s.sent);
		std::vector<u8>().swap(s.received);
	}
	return res;
}

/* see loopback.h */
bool LoopbackSide::ok() const
{
	return connected && carried >= CARRIED_MIN * expected && checked &&
		(double)errors / checked <= BER_MAX;
}

/* see loopback.h */
std::string LoopbackSide::text(const char *name) const
{
	char buf[200];

	if (!connected)
		return std::string(name) + ": no connection";
	snprintf(buf, sizeof(buf), ": %ld bits received, %ld of %ld carried, "
		 "%ld checked, %ld errors, BER %.2e", received, carried,
		 expected, checked, errors,
		 checked ? (double)errors / checked : 1.0);
	std::string s = std::string(name) + buf;
	if (resyncs > 0)
		s += ", realigned " + std::to_string(resyncs) + " times";
	return s;
}
