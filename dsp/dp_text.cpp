/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_text.cpp - text telephone pumps: the 5-bit (Baudot) mode of
 *                  V.18 and the TTYs (and telex and RTTY with other
 *                  tones and rates), EDT, and V.18's automoding
 *                  (DP_V18), which finds which of those, V.21, V.23
 *                  or Bell 103 the remote uses
 *
 *    The text modes are half duplex: a carrier only while characters go
 *    out (150 ms of mark before them, 300 ms after), the receiver deaf
 *    meanwhile.  The pump frames the characters itself (5 bits and 1.5
 *    stop bits, or 7 bits, even parity and 2 stop bits) and talks to
 *    the core's asynchronous link in 8N1 characters: the DTE's are
 *    taken off the link's bit stream, the remote's put on it.  A call
 *    in a text mode is up whether or not either side sends: no carrier
 *    is no reason to hang up.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"

namespace dsp {

/*
 *    The 5-bit code (ITA2, and the US TTY's figures, TIA-825-A): each
 *    code's character in letters and in figures, 0 for none
 */
#define BAUDOT_FIGS  0x1B
#define BAUDOT_LTRS  0x1F
#define BAUDOT_SPACE 0x04

static const char baudot_ltrs[32] = {
	0, 'E', '\n', 'A', ' ', 'S', 'I', 'U',
	'\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
	'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
	'O', 'B', 'G', 0, 'M', 'X', 'V', 0,
};
static const char ita2_figs[32] = {
	0, '3', '\n', '-', ' ', '\'', '8', '7',
	'\r', 0x05, '4', 0x07, ',', '!', ':', '(',
	'5', '+', ')', '2', '#', '6', '0', '1',
	'9', '?', '&', 0, '.', '/', '=', 0,
};
static const char ustty_figs[32] = {
	0, '3', '\n', '-', ' ', 0x07, '8', '7',
	'\r', '$', '4', '\'', ',', '!', ':', '(',
	'5', '"', ')', '2', '#', '6', '0', '1',
	'9', '?', '&', 0, '.', '/', ';', 0,
};

/* the settings a core without MDMPRM_TEXT gets: the US TTY's */
static const struct dp_text text_defaults = {
	4545, 1400, 1800, 0, 1, 0, "",
};

/*
 *    The pump
 */
enum text_state {
	TXT_LISTEN,             /* automoding: the remote's signal */
	TXT_PROBE,              /* automoding: a probe out, listening */
	TXT_DATA,
};

/* a text mode */
enum text_mode {
	TM_BAUDOT,
	TM_EDT,
};

/* automoding's probes, in their order */
enum text_probe {
	PR_BAUDOT,              /* the answer message in 5-bit */
	PR_EDT,                 /* and in EDT */
	PR_V21,                 /* our V.21 mark */
	PR_B103,                /* our Bell 103 mark */
	PR_V23,                 /* our V.23 mark */
	PR_COUNT,
};

/* automoding: listening before the first probe, after a message
   probe, and a mark probe's length */
#define LISTEN_MS        3000
#define PROBE_LISTEN_MS  2000
#define PROBE_TONE_MS    3000
/* a carrier's lead and hang (TIA-825-A: 150 and 300 ms of mark) */
#define LEAD_MS          150
#define HANG_MS          300
/* the receiver deaf after our carrier stops */
#define DEAF_MS          100

struct TextPump : PumpBase<TextPump, text_state> {
	static constexpr const char *name = "text telephone (V.18) pump";

	struct dp_text set;
	enum text_mode mode;
	bool automode;          /* DP_V18 */

	/* the text mode's transmitter: half bits at its rate */
	FskMod mod;
	float tx_phase, tx_inc;
	u8 halves[48];          /* the half bits to go, marks for 1 */
	s16 h_n, h_at;
	s16 cur;                /* the half bit going out */
	bool tx_on;
	s32 lead_left;          /* half bits of mark before the frame */
	s32 hang_left;          /* half bits of mark before the carrier
				   stops */
	s32 deaf;               /* samples the receiver still ignores */
	u8 codes[96];           /* characters to send before the DTE's
				   (the message; one of the DTE's while
				   its shift code goes first) */
	s16 c_n, c_at;
	s16 tx_shift;           /* 5-bit: 0 letters, 1 figures, -1 not
				   sent yet in this burst */
	s16 rx_shift;

	/* the text mode's receiver */
	FskDemod dem;
	Uart uart;

	/* automoding's detectors */
	FskDemod d_baudot, d_ch1, d_ch2;
	Uart u_baudot, u_edt;
	s16 rx_shift_auto;      /* 5-bit decoding while detecting */
	s32 ch1_run, ch1_min, ch1_runs, ch1_on;
	s16 ch1_prev;
	s32 ch2_on;
	Bell103Detect bell;
	Goertzel g390, g300, g500, g1300, g1100, g1500, g2100, g2225, g2050;
	s32 v23b_blocks, v23f_blocks, b2225_blocks;
	enum text_probe probe;
	Osc tone;               /* a mark probe's */
	bool connected;         /* DPSTAT_CONNECT returned */
	bool reply;             /* answering, automoding found a text
				   mode: our answer in it once the remote
				   is quiet (another automoding modem,
				   whose probe this may have been, finds
				   us by it) */
	bool rx_busy;           /* the remote's carrier: no burst of
				   ours starts (half duplex) */
	s32 quiet;              /* samples the remote has had no
				   carrier */
	s32 listened;           /* samples since a message probe went
				   out */
	s32 found_at;           /* samples of DATA before the held
				   characters go to the link */
	char held[64];          /* the remote's characters while
				   detecting */
	s16 held_n;

	/* its name in the log */
	const char *tag() const { return "text"; }
	/* in data (no handshake timeout; the carrier is not watched) */
	bool in_data() const { return state == TXT_DATA; }
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 sample(float x, float *y);
	/* cnt samples in and out, through sample() */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}

	void set_mode(enum text_mode m_);
	/* the mode's rate, bit/s */
	float baud() const
	{
		return mode == TM_BAUDOT ? set.baud100 / 100.0f : 110.0f;
	}
	void push_halves(s16 bit, s16 n);
	void frame(u16 code);
	s32 next_code();
	s32 dte_char();
	void queue_text(const char *s);
	s16 baudot_code(char c, s16 *shift) const;
	char baudot_char(s16 code, s16 *shift) const;
	void to_link(char c);
	float tx_sample();
	void rx_sample(float x);
	s16 detect(float x);
	s16 found(enum DP_ID id);
	float probe_sample(s16 *done);
};

/* the mode's tones, rate and framing on both sides; the transmitter
   silent, the receiver listening */
void TextPump::set_mode(enum text_mode m_)
{
	float mark, space, b;

	mode = m_;
	b = baud();
	if (mode == TM_BAUDOT) {
		mark = static_cast<float>(set.mark);
		space = static_cast<float>(set.space);
		uart.init(b, 5, srate);
	} else {
		mark = 980;
		space = 1180;
		uart.init(b, 8, srate);
	}
	mod.init(mark, space, DSP_TX_AMPLITUDE, srate);
	dem.init(mark, space, b, srate);
	tx_inc = 2 * b / srate;
	tx_phase = 0;
	h_n = h_at = 0;
	cur = 1;
	tx_on = false;
	tx_shift = -1;
}

/* the settings (the core's, or the US TTY's), the mode by id: DP_V18
   listens first (and probes), the others are up at once */
s16 TextPump::start(struct modem *m, enum DP_ID id, s16 caller_,
		    s32 srate_, s32)
{
	if (id != DP_V18 && id != DP_BAUDOT && id != DP_EDT)
		return -1;
	pump_setup(m, srate_, caller_);
	intptr_t p = modem_get_param(m, MDMPRM_TEXT);
	set = p > 0 ? *reinterpret_cast<const struct dp_text *>(p) :
		text_defaults;
	set.msg[sizeof(set.msg) - 1] = 0;
	if (set.baud100 < 1000 || set.baud100 > 30000)
		set.baud100 = text_defaults.baud100;
	if (set.mark < 300 || set.mark > 3400 || set.space < 300 ||
	    set.space > 3400 || set.mark == set.space) {
		set.mark = text_defaults.mark;
		set.space = text_defaults.space;
	}

	automode = id == DP_V18;
	set_mode(id == DP_EDT ? TM_EDT : TM_BAUDOT);
	state = automode ? TXT_LISTEN : TXT_DATA;
	if (automode) {
		float b = set.baud100 / 100.0f;
		d_baudot.init(static_cast<float>(set.mark),
			      static_cast<float>(set.space), b, srate);
		u_baudot.init(b, 5, srate);
		d_ch1.init(980, 1180, 300, srate);
		u_edt.init(110, 8, srate);
		d_ch2.init(1650, 1850, 300, srate);
		bell.init(srate);
		s16 blk = static_cast<s16>(srate / 50);
		g390.init(390, blk, srate);
		g300.init(300, blk, srate);
		g500.init(500, blk, srate);
		g1300.init(1300, blk, srate);
		g1100.init(1100, blk, srate);
		g1500.init(1500, blk, srate);
		g2100.init(2100, blk, srate);
		g2225.init(2225, blk, srate);
		g2050.init(2050, blk, srate);
		ch1_min = 1 << 30;
	}
	DSP_DBG("text: create %s %s, %d.%02d bit/s, %d/%d Hz\n",
		id == DP_V18 ? "V.18" : id == DP_EDT ? "EDT" : "5-bit",
		caller_ ? "caller" : "answerer", set.baud100 / 100,
		set.baud100 % 100, set.mark, set.space);
	return 0;
}

/* n half bits of bit into the frame going out */
void TextPump::push_halves(s16 bit, s16 n)
{
	while (n-- > 0 && h_n < static_cast<s16>(sizeof(halves)))
		halves[h_n++] = static_cast<u8>(bit);
}

/* a character's frame: 5-bit, a start bit, 5 bits and 1.5 stop bits;
   EDT, a start bit, 8 bits (parity the last) and 2 stop bits */
void TextPump::frame(u16 code)
{
	s16 i, nb = mode == TM_BAUDOT ? 5 : 8;

	push_halves(0, 2);
	for (i = 0; i < nb; i++)
		push_halves((code >> i) & 1, 2);
	push_halves(1, mode == TM_BAUDOT ? 3 : 4);
}

/* the DTE's next character off the link's 8N1 bit stream (its idle
   marks one at a time), -1 for none */
s32 TextPump::dte_char()
{
	u8 b = 1, bits[9];
	s32 c = 0;

	modem_get_bits(modem, 1, &b, 1);
	if (b & 1)
		return -1;
	modem_get_bits(modem, 1, bits, 9);
	for (s16 i = 0; i < 8; i++)
		c |= (bits[i] & 1) << i;
	return c;
}

/* c's 5-bit code, and the shift it needs into *shift (-1 for either);
   -1 for a character the code has not */
s16 TextPump::baudot_code(char c, s16 *shift) const
{
	const char *figs = set.ita2 ? ita2_figs : ustty_figs;

	if (c >= 'a' && c <= 'z')
		c = static_cast<char>(c - 'a' + 'A');
	*shift = -1;
	if (c == ' ' || c == '\r' || c == '\n')
		return c == ' ' ? BAUDOT_SPACE : c == '\r' ? 0x08 : 0x02;
	if (!c)
		return -1;
	for (s16 i = 0; i < 32; i++) {
		if (baudot_ltrs[i] == c) {
			*shift = 0;
			return i;
		}
		if (figs[i] == c) {
			*shift = 1;
			return i;
		}
	}
	return -1;
}

/* code's character in the receiver's *shift, which a shift code (and
   on a US TTY a space) changes; 0 for none */
char TextPump::baudot_char(s16 code, s16 *shift) const
{
	code &= 0x1F;
	if (code == BAUDOT_LTRS || code == BAUDOT_FIGS) {
		*shift = code == BAUDOT_FIGS;
		return 0;
	}
	char c = (*shift ? (set.ita2 ? ita2_figs : ustty_figs) :
		  baudot_ltrs)[code];
	/* the US TTY's unshift on space */
	if (code == BAUDOT_SPACE && !set.ita2)
		*shift = 0;
	return c;
}

/* s queued to go out in the mode, before the DTE's */
void TextPump::queue_text(const char *s)
{
	for (; *s; s++) {
		s16 sh;
		if (mode == TM_EDT) {
			if (c_n < static_cast<s16>(sizeof(codes)))
				codes[c_n++] = static_cast<u8>(*s & 0x7F);
			continue;
		}
		s16 code = baudot_code(*s, &sh);
		if (code >= 0 && c_n < static_cast<s16>(sizeof(codes)))
			codes[c_n++] = static_cast<u8>(*s);
	}
}

/* the next code to frame: the shift a character needs, the queue's,
   then the DTE's (those the mode has not are dropped); -1 for none */
s32 TextPump::next_code()
{
	for (;;) {
		s32 c;
		if (c_at < c_n) {
			c = codes[c_at];
		} else {
			c_n = c_at = 0;
			if (state != TXT_DATA || (!tx_on && rx_busy))
				return -1;
			c = dte_char();
			if (c < 0)
				return -1;
			/* back into the queue, a shift code may go first */
			codes[c_n++] = static_cast<u8>(c);
			c_at = 0;
		}
		if (mode == TM_EDT) {
			c_at++;
			c &= 0x7F;
			s16 ones = 0;
			for (s32 v = c; v; v >>= 1)
				ones += v & 1;
			return c | ((ones & 1) << 7);
		}
		s16 sh;
		s16 code = baudot_code(static_cast<char>(c), &sh);
		if (code < 0) {
			c_at++;
			continue;
		}
		if (sh < 0 && tx_shift < 0)
			sh = 0;
		if (sh >= 0 && sh != tx_shift) {
			tx_shift = sh;
			return sh ? BAUDOT_FIGS : BAUDOT_LTRS;
		}
		c_at++;
		/* a US TTY goes back to letters after a space */
		if (code == BAUDOT_SPACE && !set.ita2 && tx_shift == 1)
			tx_shift = -1;
		return code;
	}
}

/* the remote's character c to the link, as 8N1 after a mark */
void TextPump::to_link(char c)
{
	u8 bits[11];
	bits[0] = 1;
	bits[1] = 0;
	for (s16 i = 0; i < 8; i++)
		bits[2 + i] = (static_cast<u8>(c) >> i) & 1;
	bits[10] = 1;
	modem_put_bits(modem, 1, bits, 11);
}

/* the next transmit sample: at each half bit the frame's next, a new
   character's frame (after the lead's marks if the carrier was off),
   or the hang's marks and then silence */
float TextPump::tx_sample()
{
	tx_phase += tx_inc;
	if (tx_phase >= 1) {
		tx_phase -= 1;
		if (lead_left > 0) {
			lead_left--;
			cur = 1;
		} else if (h_at < h_n) {
			cur = halves[h_at++];
		} else {
			h_at = h_n = 0;
			s32 code = next_code();
			if (code >= 0) {
				frame(static_cast<u16>(code));
				hang_left = static_cast<s32>(
					2 * baud() * HANG_MS / 1000);
				if (!tx_on) {
					tx_on = true;
					lead_left = static_cast<s32>(
						2 * baud() * LEAD_MS / 1000);
					cur = 1;
				} else {
					cur = halves[h_at++];
				}
			} else if (tx_on && hang_left > 0) {
				hang_left--;
				cur = 1;
			} else if (tx_on) {
				tx_on = false;
				tx_shift = -1;
			}
		}
	}
	if (tx_on)
		deaf = ms2samples(DEAF_MS, srate);
	return tx_on ? mod.next(cur) : 0;
}

/* a received sample in data: the characters to the link (none while
   we send and just after) */
void TextPump::rx_sample(float x)
{
	dem.run(x);
	bool on = dem.carrier && deaf <= 0;
	rx_busy = on;
	if (deaf > 0)
		deaf--;
	s32 v = uart.run(dem.last_dec, on);
	if (v < 0)
		return;
	if (mode == TM_EDT) {
		char c = static_cast<char>(v & 0x7F);
		if (c)
			to_link(c);
		return;
	}
	char c = baudot_char(static_cast<s16>(v), &rx_shift);
	if (c)
		to_link(c);
}

/* automoding found the remote's mode: 5-bit and EDT here, at once in
   data (DPSTAT_CONNECT); V.21, V.23 and Bell 103 to their pumps, which
   start as after V.8, sending at once (DPSTAT_CHANGEDP) */
s16 TextPump::found(enum DP_ID id)
{
	DSP_DBG("text: the remote is %s\n",
		id == DP_BAUDOT ? "5-bit" : id == DP_EDT ? "EDT" :
		id == DP_V21 ? "V.21" : id == DP_V23 ? "V.23" : "Bell 103");
	if (id == DP_BAUDOT || id == DP_EDT) {
		set_mode(id == DP_EDT ? TM_EDT : TM_BAUDOT);
		c_n = c_at = 0;
		/* the receiver goes on where the detector is, in the
		   middle of the remote's characters */
		if (id == DP_BAUDOT) {
			dem = d_baudot;
			uart = u_baudot;
			rx_shift = rx_shift_auto;
		} else {
			dem = d_ch1;
			uart = u_edt;
		}
		reply = !caller;
		stat_dp(id);
		set_state(TXT_DATA);
		connected = true;
		found_at = ms2samples(100, srate);
		s32 b = static_cast<s32>(lrintf(baud()));
		return connect_at(b, b);
	}
	if (stats)
		stats->dp = 0;
	if (rt)
		rt->after_v8 = 1;
	return hand_over(id, 0);
}

/* automoding's detectors on x: a remote's mode found (found()'s
   result), else DPSTAT_OK.  While a message probe goes out (and just
   after) they decide nothing (its echo is no remote); a mark probe
   hears only its answer */
s16 TextPump::detect(float x)
{
	bool probing_tone = state == TXT_PROBE && probe >= PR_V21;
	bool own_text = tx_on || deaf > 0;

	if (deaf > 0)
		deaf--;

	/* 5-bit: a good character */
	d_baudot.run(x);
	s32 v = u_baudot.run(d_baudot.last_dec,
			     d_baudot.carrier && !own_text);
	if (v >= 0 && !probing_tone) {
		char c = baudot_char(static_cast<s16>(v), &rx_shift_auto);
		if (c && held_n < static_cast<s16>(sizeof(held)))
			held[held_n++] = c;
		if ((v & 0x1F) != 0)
			return found(DP_BAUDOT);
	}

	/* V.21's channel 1: EDT at 110 bit/s, V.21 at 300 (the shortest
	   run of a decision tells), or a steady V.21 mark */
	d_ch1.run(x);
	bool ch1 = d_ch1.carrier && !own_text;
	v = u_edt.run(d_ch1.last_dec, ch1 && !own_text);
	if (ch1) {
		ch1_on++;
		if (d_ch1.last_dec != ch1_prev) {
			if (ch1_run > srate / 1000) {
				ch1_runs++;
				if (ch1_run < ch1_min)
					ch1_min = ch1_run;
			}
			ch1_run = 0;
			ch1_prev = d_ch1.last_dec;
		} else {
			ch1_run++;
		}
	} else {
		ch1_on = ch1_run = ch1_runs = 0;
		ch1_min = 1 << 30;
	}
	bool fast = ch1_runs >= 3 && ch1_min < srate * 6 / 1000;
	if (!caller && (!probing_tone || probe == PR_V21)) {
		if (fast || (d_ch1.last_dec && ch1_on >=
			     ms2samples(probe == PR_V21 &&
					state == TXT_PROBE ? 300 : 500,
					srate) && ch1_runs == 0))
			return found(DP_V21);
	}
	if (v >= 0 && !fast && !probing_tone) {
		s16 ones = 0;
		for (s32 b = v; b; b >>= 1)
			ones += b & 1;
		char c = static_cast<char>(v & 0x7F);
		if (!(ones & 1) && c) {
			if (held_n < static_cast<s16>(sizeof(held)))
				held[held_n++] = c;
			return found(DP_EDT);
		}
	}

	/* the answerer's V.21 channel 2 (a caller's) */
	d_ch2.run(x);
	ch2_on = d_ch2.carrier && !own_text ? ch2_on + 1 : 0;
	if (caller && (!probing_tone || probe == PR_V21) &&
	    ch2_on >= ms2samples(300, srate))
		return found(DP_V21);

	/* Bell 103 and V.23 marks in blocks of 20 ms: the caller's
	   1270 Hz or 390 Hz to an answerer, the answerer's 2225 Hz or
	   1300 Hz to a caller */
	if (!caller) {
		bell.run(x);
		if (bell.found() && !own_text &&
		    (!probing_tone || probe == PR_B103))
			return found(DP_B103);
	}
	g300.run(x);
	g500.run(x);
	g1100.run(x);
	g1500.run(x);
	g2100.run(x);
	g2050.run(x);
	g1300.run(x);
	g2225.run(x);
	if (g390.run(x)) {
		const float level = tone_power(100);
		/* (the blocks end together) */
		bool b390 = g390.power > level &&
			g390.power > 4 * (g300.power + g500.power);
		bool b1300 = g1300.power > level &&
			g1300.power > 4 * (g1100.power + g1500.power +
					   g2100.power);
		bool b2225 = g2225.power > level &&
			g2225.power > 4 * (g2050.power + g2100.power);
		v23b_blocks = b390 && !own_text ? v23b_blocks + 1 : 0;
		v23f_blocks = b1300 && !own_text ? v23f_blocks + 1 : 0;
		b2225_blocks = b2225 && !own_text ? b2225_blocks + 1 : 0;
		const s32 need = 300 / 20;
		if (!caller && v23b_blocks >= need &&
		    (!probing_tone || probe == PR_V23))
			return found(DP_V23);
		if (caller && v23f_blocks >= need &&
		    (!probing_tone || probe == PR_V23))
			return found(DP_V23);
		if (caller && b2225_blocks >= need &&
		    (!probing_tone || probe == PR_B103))
			return found(DP_B103);
	}
	return DPSTAT_OK;
}

/* a message probe's sample: its text, then *done when it has gone
   out and the listening after it is over */
float TextPump::probe_sample(s16 *done)
{
	float y = tx_sample();

	*done = 0;
	if (!tx_on && c_at >= c_n && h_at >= h_n) {
		/* listening after it: counted from the end */
		if (++listened >= ms2samples(PROBE_LISTEN_MS, srate))
			*done = 1;
	} else {
		listened = 0;
	}
	return y;
}

/* a sample x in, *y out: automoding's listening and probes, then the
   text mode's data */
s16 TextPump::sample(float x, float *y)
{
	s16 r;

	switch (state) {
	case TXT_LISTEN:
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (set.probe && count >= ms2samples(LISTEN_MS, srate)) {
			probe = PR_BAUDOT;
			set_mode(TM_BAUDOT);
			c_n = c_at = 0;
			queue_text(set.msg);
			queue_text("\r\n");
			listened = 0;
			set_state(TXT_PROBE);
		}
		break;
	case TXT_PROBE: {
		s16 done;
		if (probe <= PR_EDT) {
			*y = probe_sample(&done);
		} else {
			float f;
			switch (probe) {
			case PR_V21:  f = caller ? 980 : 1650; break;
			case PR_B103: f = caller ? 1270 : 2225; break;
			default:      f = caller ? 390 : 1300; break;
			}
			if (!count)
				tone.set_freq(f, srate);
			*y = DSP_TX_AMPLITUDE * tone.next();
			done = count >= ms2samples(PROBE_TONE_MS, srate);
		}
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (done) {
			probe = static_cast<enum text_probe>(
				(probe + 1) % PR_COUNT);
			if (probe <= PR_EDT) {
				set_mode(probe == PR_EDT ? TM_EDT : TM_BAUDOT);
				c_n = c_at = 0;
				queue_text(set.msg);
				queue_text("\r\n");
				listened = 0;
			}
			set_state(TXT_PROBE);
		}
		break;
	}
	case TXT_DATA:
		carrier_tick(1);
		if (!connected) {
			connected = true;
			if (!caller && set.answer_msg && set.msg[0]) {
				queue_text(set.msg);
				queue_text("\r\n");
			}
			s32 b = static_cast<s32>(lrintf(baud()));
			*y = tx_sample();
			rx_sample(x);
			return connect_at(b, b);
		}
		quiet = dem.carrier ? 0 : quiet + 1;
		if (reply && quiet >= ms2samples(500, srate)) {
			reply = false;
			queue_text(set.msg);
			queue_text("\r\n");
		}
		if (held_n && count >= found_at) {
			for (s16 i = 0; i < held_n; i++)
				to_link(held[i]);
			held_n = 0;
		}
		*y = tx_sample();
		rx_sample(x);
		break;
	}
	return DPSTAT_OK;
}

/* registers the pump as DP_V18, DP_BAUDOT and DP_EDT; 0 */
s16 dp_text_init(void)
{
	modem_dp_register(DP_V18, &PumpOps<TextPump>::ops);
	modem_dp_register(DP_BAUDOT, &PumpOps<TextPump>::ops);
	modem_dp_register(DP_EDT, &PumpOps<TextPump>::ops);
	return 0;
}

/* deregisters them */
void dp_text_exit(void)
{
	modem_dp_deregister(DP_V18, &PumpOps<TextPump>::ops);
	modem_dp_deregister(DP_BAUDOT, &PumpOps<TextPump>::ops);
	modem_dp_deregister(DP_EDT, &PumpOps<TextPump>::ops);
}

} // namespace dsp
