/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_text.cpp - text telephone pumps (V.18): the 5-bit (Baudot)
 *                  mode of the TTYs (and telex and RTTY with other
 *                  tones and rates, Annex A), DTMF (Annex B), EDT
 *                  (Annex C), Bell 103 (Annex D), V.23's Minitel and
 *                  Prestel (Annex E), the V.21 text telephones of the
 *                  Nordic countries and the UK (Annex F), V.18's own
 *                  mode between two V.18 modems (Annex G), and V.18's
 *                  automoding (DP_V18), which finds which of them the
 *                  remote uses
 *
 *    The carrierless modes (5-bit, DTMF, EDT) are half duplex: a
 *    signal only while characters go out (for the 5-bit mode and EDT
 *    150 ms of mark before them, 300 ms after), the receiver deaf
 *    meanwhile.  The carrier modes (Bell 103, V.23, V.21 and V.18's
 *    own) are duplex FSK on a continuous carrier.  The pump frames the
 *    characters itself (5 bits and 1.5 stop bits; 7 bits, even parity
 *    and 2 stop bits; 7 bits, even parity and a stop bit; 8 bits and a
 *    stop bit, V.18's mode; or a DTMF digit or up to four) and talks
 *    to the core's asynchronous link in 8N1 characters: the DTE's are
 *    taken off the link's bit stream, the remote's put on it.  A call
 *    in a text mode is up whether or not either side sends: no carrier
 *    is no reason to hang up (clause 4/V.18).
 *
 */

#include <algorithm>
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

/*
 *    The DTMF mode's code (Table B.1/V.18): a character is up to three
 *    of * and # and then a digit.  The national letters (#*1 to #*6)
 *    are left out: the DTE's characters are ASCII.
 */
struct DtmfCode {
	const char *seq;
	char c;
};
static const DtmfCode dtmf_codes[] = {
	{ "1", 'b' }, { "2", 'e' }, { "3", 'h' }, { "4", 'k' },
	{ "5", 'n' }, { "6", 'q' }, { "7", 't' }, { "8", 'w' },
	{ "9", 'z' }, { "0", ' ' },
	{ "*1", 'a' }, { "*2", 'd' }, { "*3", 'g' }, { "*4", 'j' },
	{ "*5", 'm' }, { "*6", 'p' }, { "*7", 's' }, { "*8", 'v' },
	{ "*9", 'y' }, { "*0", '\b' },
	{ "#1", 'c' }, { "#2", 'f' }, { "#3", 'i' }, { "#4", 'l' },
	{ "#5", 'o' }, { "#6", 'r' }, { "#7", 'u' }, { "#8", 'x' },
	{ "#9", '.' }, { "#0", '?' },
	{ "*#1", '1' }, { "*#2", '2' }, { "*#3", '3' }, { "*#4", '4' },
	{ "*#5", '5' }, { "*#6", '6' }, { "*#7", '7' }, { "*#8", '8' },
	{ "*#9", '9' }, { "*#0", '0' },
	{ "**1", '+' }, { "**2", '-' }, { "**3", '=' }, { "**4", ':' },
	{ "**5", '%' }, { "**6", '(' }, { "**7", ')' }, { "**8", ',' },
	{ "**9", '\n' },
	{ "##*1", 'A' }, { "##*2", 'D' }, { "##*3", 'G' }, { "##*4", 'J' },
	{ "##*5", 'M' }, { "##*6", 'P' }, { "##*7", 'S' }, { "##*8", 'V' },
	{ "##*9", 'Y' },
	{ "##1", 'B' }, { "##2", 'E' }, { "##3", 'H' }, { "##4", 'K' },
	{ "##5", 'N' }, { "##6", 'Q' }, { "##7", 'T' }, { "##8", 'W' },
	{ "##9", 'Z' }, { "##0", ' ' },
	{ "###1", 'C' }, { "###2", 'F' }, { "###3", 'I' }, { "###4", 'L' },
	{ "###5", 'O' }, { "###6", 'R' }, { "###7", 'U' }, { "###8", 'X' },
	{ "###9", ';' }, { "###0", '!' },
};

/* c's DTMF digits (Table B.2/V.18: the characters the code has not
   as their stand-ins), nullptr for one it drops (CR among them: a new
   line is LF alone) */
static const char *dtmf_seq(char c)
{
	switch (c) {
	case '\t': case '_': case '~': case 0x1F:
		c = ' ';
		break;
	case 0x0B: case 0x0C: case 0x1C: case 0x1D: case 0x1E:
		c = '\n';
		break;
	case 0x1A:
		c = '?';
		break;
	case 0x7F:
		c = '\b';
		break;
	case '@': c = 'X'; break;
	case '&': c = '+'; break;
	case '*': c = '.'; break;
	case '<': c = '('; break;
	case '>': c = ')'; break;
	}
	for (const DtmfCode &d : dtmf_codes)
		if (d.c == c)
			return d.seq;
	return nullptr;
}

/* the character of DTMF digits seq, 0 for none */
static char dtmf_char(const char *seq)
{
	for (const DtmfCode &d : dtmf_codes)
		if (!strcmp(d.seq, seq))
			return d.c;
	return 0;
}

/* the settings a core without MDMPRM_TEXT gets: the US TTY's */
static const struct dp_text text_defaults = {
	4545, 1400, 1800, 0, 1, 0, "",
};

/*
 *    V.18's signals (clause 3/V.18): CI (V.8's, coded for a textphone:
 *    ten ones, the sync 0000000001, the call function octet 0x41) and
 *    TXP (ten ones and "TXP" in 7 bits even parity), at 300 bit/s in a
 *    V.21 channel; XCI, V.23's 1300 Hz mark with two-octet markers of
 *    ones
 */
#define V8_CALLF_TEXT   0x41
#define CI_BITS         30
#define TXP_BITS        40
/* XCI: 400 ms of mark, a marker, 800 ms of mark (three times), a
   marker, 100 ms of mark, at 1200 bit/s */
#define XCI_MARKER_BITS 20
#define XCI_BITS        (480 + 3 * 960 + 120 + 4 * XCI_MARKER_BITS)

/* the 7 bits of c and its even parity */
static u16 even_parity(u16 c)
{
	s16 ones = 0;
	c &= 0x7F;
	for (u16 v = c; v; v >>= 1)
		ones += v & 1;
	return static_cast<u16>(c | ((ones & 1) << 7));
}

/* bit i (0 or 1) of a CI sequence */
static s16 ci_bit(s32 i)
{
	if (i < 10)
		return 1;               /* ten ones */
	if (i < 20)
		return i == 19;         /* the sync */
	if (i == 20)
		return 0;               /* the octet's start bit */
	if (i == 29)
		return 1;               /* its stop bit */
	return (V8_CALLF_TEXT >> (i - 21)) & 1;
}

/* bit i of a TXP sequence */
static s16 txp_bit(s32 i)
{
	static const char txp[] = "TXP";
	if (i < 10)
		return 1;
	i -= 10;
	s16 k = static_cast<s16>(i % 10);
	if (!k)
		return 0;
	if (k == 9)
		return 1;
	return (even_parity(static_cast<u16>(txp[i / 10])) >> (k - 1)) & 1;
}

/* bit i of XCI: a marker's start bits its only spaces */
static s16 xci_bit(s32 i)
{
	static const s32 marks[5] = { 480, 960, 960, 960, 120 };
	for (s16 k = 0; k < 5; k++) {
		if (i < marks[k])
			return 1;
		i -= marks[k];
		if (k == 4)
			break;
		if (i < XCI_MARKER_BITS)
			return i % 10 != 0;
		i -= XCI_MARKER_BITS;
	}
	return 1;
}

/*
 *    The pump
 */
enum text_state {
	TXT_LISTEN,             /* automoding: the remote's signal (the
				   caller sends CI and XCI meanwhile) */
	TXT_PROBE,              /* automoding, answering: a probe out,
				   listening */
	TXT_ANS,                /* answering: CI or XCI heard, our answer
				   tone, listening for TXP (Tt) */
	TXT_TXP_GAP,            /* answering: TXP heard, 75 ms of silence */
	TXT_TXP_OUT,            /* answering: our three TXPs, then V.18 */
	TXT_ANS_GAP,            /* calling: an answer tone heard, its end
				   or 0.5 s of silence */
	TXT_TXP,                /* calling: our TXPs while the answer tone
				   lasts, listening for the answerer's */
	TXT_DATA,
};

/* a text mode */
enum text_mode {
	TM_BAUDOT,              /* the carrierless ones */
	TM_EDT,
	TM_DTMF,
	TM_V21,                 /* the carrier ones */
	TM_B103,
	TM_V23,
	TM_V18,
};

/* automoding's probes (answering), in their order */
enum text_probe {
	PR_BAUDOT,              /* the answer message in 5-bit */
	PR_EDT,                 /* in EDT */
	PR_DTMF,                /* in DTMF */
	PR_V21,                 /* our V.21 mark */
	PR_B103,                /* our Bell 103 mark */
	PR_V23,                 /* our V.23 mark */
	PR_COUNT,
};

/* the caller's signals (5.1.1/V.18), each this long */
enum call_sig { CS_SILENCE, CS_CI, CS_XCI };
struct CallSeg {
	enum call_sig sig;
	s32 ms;
};
static const CallSeg call_cycle[] = {
	{ CS_SILENCE, 1000 }, { CS_CI, 400 }, { CS_SILENCE, 2000 },
	{ CS_CI, 400 }, { CS_SILENCE, 2000 }, { CS_CI, 400 },
	{ CS_SILENCE, 2000 }, { CS_XCI, 3000 },
};
#define CALL_SEGS (static_cast<s16>(sizeof(call_cycle) / sizeof(call_cycle[0])))

/* automoding: listening before the first probe, after a message
   probe, and a mark probe's length */
#define LISTEN_MS        3000
#define PROBE_LISTEN_MS  2000
#define PROBE_TONE_MS    3000
/* a carrier's lead and hang (TIA-825-A: 150 and 300 ms of mark) */
#define LEAD_MS          150
#define HANG_MS          300
/* the receiver deaf after our carrier stops; after a DTMF character
   (B.1/V.18) */
#define DEAF_MS          100
#define DTMF_DEAF_MS     300
/* a DTMF digit and the silence after it (B.3/V.18: 70 and 50 ms at
   least) */
#define DTMF_ON_MS       75
#define DTMF_OFF_MS      55
/* the answering V.18's answer tone, waiting for TXP (Tt, 5.2.2) */
#define TT_MS            3000
/* the calling V.18 after an answer tone: silence before TXP (5.1.3),
   and how long it listens once the tone is gone */
#define TXP_GAP_MS       500
#define TXP_LISTEN_MS    5000
/* an answer tone, heard this long */
#define ANS_MS           400

/* automoding's receivers of the carrier modes' channels */
enum text_rx {
	RX_CH1,                 /* V.21's channel 1 */
	RX_CH2,                 /* and 2 */
	RX_V23F,                /* V.23's 1200 bit/s channel */
	RX_V23B,                /* its 75 bit/s one */
	RX_B103L,               /* Bell 103's originating channel */
	RX_B103H,               /* its answering one */
	RX_COUNT,
};

/* an asynchronous character receiver on a demodulator's bits: a start
   bit, 8 bits, a stop bit (ready zeroed) */
struct BitUart {
	s16 at;                 /* 0 idle, 1 to 8 the next data bit, 9
				   the stop bit */
	u16 v;

	/* a bit in: the character's 8 bits once its stop bit came good,
	   -1 else */
	s32 put(s16 b)
	{
		if (!at) {
			if (!b) {
				at = 1;
				v = 0;
			}
			return -1;
		}
		if (at <= 8) {
			v |= static_cast<u16>((b & 1) << (at - 1));
			at++;
			return -1;
		}
		at = 0;
		return b ? v : -1;
	}
};

struct TextPump : PumpBase<TextPump, text_state> {
	static constexpr const char *name = "text telephone (V.18) pump";

	struct dp_text set;
	enum text_mode mode;
	bool automode;          /* DP_V18 */
	bool low;               /* a carrier mode: we send in the low
				   channel (V.21's and Bell 103's
				   originating one, V.23's 75 bit/s one) */

	/* the 5-bit mode's and EDT's transmitter: half bits at its rate */
	FskMod mod;
	float tx_phase, tx_inc;
	u8 halves[48];          /* the half bits to go, marks for 1 */
	s16 h_n, h_at;
	s16 cur;                /* the half bit going out */
	bool tx_on;             /* a carrierless mode's signal is out */
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

	/* the DTMF mode's */
	DtmfGen dgen;
	const char *d_seq;      /* the character's digits going out */
	s32 d_left;             /* samples of its tone or gap still to go */
	bool d_tone;
	DtmfDetect ddet;
	char d_rx[5];           /* the remote's digits of a character */
	s16 d_rx_n;
	s32 d_since;            /* samples since its last digit */

	/* the carrier modes' */
	FskBitTx ctx;
	FskDemod crx;
	BitUart cuart;
	u16 c_frame;            /* the bits of the frame going out */
	s16 c_bits;
	bool seven;             /* 7 bits even parity (8 bits: V.18's) */

	/* the carrierless modes' receiver */
	FskDemod dem;
	Uart uart;

	/* automoding's detectors */
	FskDemod d_baudot, d_ch1, d_ch2, d_v23f, d_v23b, d_b103l, d_b103h;
	Uart u_baudot, u_edt;
	BitUart b_ch1, b_ch2, b_v23f, b_v23b, b_b103l, b_b103h;
	/* the characters each carrier's receiver has had (the latest,
	   held for the link if the remote's mode is its) */
	char pre[RX_COUNT][16];
	s16 pre_n[RX_COUNT];
	s16 rx_shift_auto;      /* 5-bit decoding while detecting */
	s32 ch1_run, ch1_min, ch1_runs, ch1_on;
	s16 ch1_prev;
	s32 ch2_on;
	s16 ch1_other;          /* V.21 characters in a row that are not
				   CI's or TXP's */
	s16 ch1_last;           /* the last one on channel 1 (CI: 0x00
				   and then the call function) */
	s16 ch1_txp, ch2_txp;   /* TXP's characters heard in a row */
	s16 xci_ff;             /* XCI's marker octets in a row */
	s32 xci_seen;           /* samples since an XCI marker (0: none
				   yet) */
	DtmfDetect d_dtmf;
	Bell103Detect bell;
	Goertzel g390, g300, g500, g1300, g1100, g1500, g2100, g2225, g2050,
		g1270, g1070, g1650, g980;
	s32 v23b_blocks, v23f_blocks, b2225_blocks, ans_blocks, b1270_blocks;
	s32 ans_gone;           /* blocks without the answer tone */
	float blk_energy;       /* of the tones' block so far */
	enum text_probe probe;
	Osc tone;               /* a mark probe's, the answer tone */
	/* the caller's CI and XCI, the TXPs */
	FskBitTx sig;
	s32 sig_bit;            /* the signal's next bit */
	s16 sig_seq;            /* TXPs out */
	s16 seg;                /* call_cycle's */
	s32 seg_left;
	bool sig_on;            /* our CI or TXP is out: channel 1 or 2
				   deaf to it */
	bool xci_on;            /* our XCI is out (1300 Hz) */
	bool txp_done;          /* calling: our TXPs are over */
	s32 txp_skip;           /* V.18's mode: samples the remote's
				   TXPs may still come in */
	bool connected;         /* DPSTAT_CONNECT returned */
	bool reply;             /* answering, automoding found a text
				   mode: our answer in it once the remote
				   is quiet (another automoding modem,
				   whose probe this may have been, finds
				   us by it) */
	bool rx_busy;           /* the remote's carrier or tone: no
				   burst of ours starts (half duplex) */
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

	/* a carrier mode (duplex, a continuous carrier) */
	bool carrier() const { return mode >= TM_V21; }
	void set_mode(enum text_mode m_, bool low_ = false);
	/* the mode's rate, bit/s: the carrierless modes' both ways, a
	   carrier mode's (V.23's) one way */
	float baud(bool rx = false) const;
	void push_halves(s16 bit, s16 n);
	void frame(u16 code);
	s32 next_code();
	s32 next_char();
	s32 dte_char();
	void queue_text(const char *s);
	s16 baudot_code(char c, s16 *shift) const;
	char baudot_char(s16 code, s16 *shift) const;
	void to_link(char c);
	void rx_char(char c);
	float tx_sample();
	float dtmf_tx_sample();
	float carrier_tx_sample();
	void dtmf_digit(char d, bool detecting);
	void rx_sample(float x);
	s16 detect(float x);
	enum DP_ID report_id() const;
	void pre_put(enum text_rx ch, s32 c);
	void chan_run(FskDemod &d, BitUart &u, enum text_rx ch, float x,
		      bool on);
	s16 found(enum text_mode m_, bool low_);
	float probe_sample(s16 *done);
	float call_sample();
	float sig_sample(const float *f, s32 bits);
	s16 connect_now();
};

/* the mode's rate (see the declaration) */
float TextPump::baud(bool rx) const
{
	switch (mode) {
	case TM_BAUDOT: return set.baud100 / 100.0f;
	case TM_EDT:    return 110.0f;
	case TM_DTMF:   return 10.0f;  /* a character in about 0.1 s */
	case TM_V23:    return (low != rx) ? 75.0f : 1200.0f;
	default:        return 300.0f;
	}
}

/* the mode's tones, rate and framing on both sides; the transmitter
   silent (a carrier mode's mark at once), the receiver listening; a
   carrier mode sends in the low channel if low_ */
void TextPump::set_mode(enum text_mode m_, bool low_)
{
	float mark, space, b;

	mode = m_;
	low = low_;
	h_n = h_at = 0;
	cur = 1;
	tx_on = false;
	tx_shift = -1;
	d_seq = nullptr;
	d_left = 0;
	d_tone = false;
	d_rx_n = 0;
	c_bits = 0;
	if (carrier()) {
		/* (mark, space) of the low channel, then the high one */
		static const float ch[3][4] = {
			{ 980, 1180, 1650, 1850 },      /* V.21 */
			{ 1270, 1070, 2225, 2025 },     /* Bell 103 */
			{ 390, 450, 1300, 2100 },       /* V.23 */
		};
		const float *f = ch[mode == TM_B103 ? 1 :
				   mode == TM_V23 ? 2 : 0];
		const float *t = low ? f : f + 2, *r = low ? f + 2 : f;
		ctx.init(t[0], t[1], DSP_TX_AMPLITUDE, baud(false), srate);
		crx.init(r[0], r[1], baud(true), srate);
		cuart = BitUart();
		seven = mode != TM_V18;
		return;
	}
	b = baud();
	if (mode == TM_DTMF) {
		ddet.init(srate, 128);
		return;
	}
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
}

/* the settings (the core's, or the US TTY's), the mode by id: DP_V18
   listens first (the caller sending CI and XCI, the answerer probing),
   the others are up at once (V.21 and Bell 103 in the caller's
   channel when calling; V.23's 75 bit/s one calling, as a Minitel
   does) */
s16 TextPump::start(struct modem *m, enum DP_ID id, s16 caller_,
		    s32 srate_, s32)
{
	enum text_mode m_;

	switch (id) {
	case DP_V18:     m_ = TM_BAUDOT; break;
	case DP_BAUDOT:  m_ = TM_BAUDOT; break;
	case DP_EDT:     m_ = TM_EDT; break;
	case DP_DTMFTXT: m_ = TM_DTMF; break;
	case DP_V21TXT:  m_ = TM_V21; break;
	case DP_B103TXT: m_ = TM_B103; break;
	case DP_V23TXT:  m_ = TM_V23; break;
	default: return -1;
	}
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
	set_mode(m_, caller_ != 0);
	state = automode ? TXT_LISTEN : TXT_DATA;
	if (!automode && carrier())
		stat_dp(report_id());
	if (automode) {
		float b = set.baud100 / 100.0f;
		d_baudot.init(static_cast<float>(set.mark),
			      static_cast<float>(set.space), b, srate);
		u_baudot.init(b, 5, srate);
		d_ch1.init(980, 1180, 300, srate);
		u_edt.init(110, 8, srate);
		d_ch2.init(1650, 1850, 300, srate);
		d_v23f.init(1300, 2100, 1200, srate);
		d_v23b.init(390, 450, 75, srate);
		d_b103l.init(1270, 1070, 300, srate);
		d_b103h.init(2225, 2025, 300, srate);
		d_dtmf.init(srate, 128);
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
		g1270.init(1270, blk, srate);
		g1070.init(1070, blk, srate);
		g1650.init(1650, blk, srate);
		g980.init(980, blk, srate);
		ch1_min = 1 << 30;
		ch1_last = -1;
		seg = 0;
		seg_left = ms2samples(call_cycle[0].ms, srate);
	}
	DSP_DBG("text: create %s %s, %d.%02d bit/s, %d/%d Hz\n",
		id == DP_V18 ? "V.18" : id == DP_EDT ? "EDT" :
		id == DP_DTMFTXT ? "DTMF" : id == DP_V21TXT ? "V.21" :
		id == DP_B103TXT ? "Bell 103" : id == DP_V23TXT ? "V.23" :
		"5-bit", caller_ ? "caller" : "answerer", set.baud100 / 100,
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

/* s queued to go out in the mode, before the DTE's (in the 5-bit mode
   and DTMF the characters they have) */
void TextPump::queue_text(const char *s)
{
	for (; *s; s++) {
		s16 sh;
		if (c_n >= static_cast<s16>(sizeof(codes)))
			return;
		if (mode == TM_BAUDOT ? baudot_code(*s, &sh) < 0 :
		    mode == TM_DTMF ? !dtmf_seq(*s) : false)
			continue;
		codes[c_n++] = static_cast<u8>(*s);
	}
}

/* the next character to send: the queue's, then (in data) the DTE's;
   -1 for none.  A carrierless mode starts none while the remote's
   signal is on (half duplex). */
s32 TextPump::next_char()
{
	if (c_at < c_n)
		return codes[c_at++];
	c_n = c_at = 0;
	if (state != TXT_DATA || (!carrier() && !tx_on && rx_busy))
		return -1;
	return dte_char();
}

/* the next 5-bit or EDT code to frame: the shift a character needs,
   the queue's, then the DTE's (those the mode has not are dropped);
   -1 for none */
s32 TextPump::next_code()
{
	for (;;) {
		s32 c;
		if (c_at < c_n) {
			c = codes[c_at];
		} else {
			c = next_char();
			if (c < 0)
				return -1;
			/* back into the queue, a shift code may go first */
			codes[c_n++] = static_cast<u8>(c);
			c_at = 0;
		}
		if (mode == TM_EDT) {
			c_at++;
			return even_parity(static_cast<u16>(c));
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

/* the remote's character c in data: to the link, after those held
   from detecting (until they go, behind them) */
void TextPump::rx_char(char c)
{
	if (!held_n)
		to_link(c);
	else if (held_n < static_cast<s16>(sizeof(held)))
		held[held_n++] = c;
}

/* a DTMF character's samples: each of its digits for DTMF_ON_MS and
   DTMF_OFF_MS of silence after it, the receiver deaf DTMF_DEAF_MS
   after the last (B.1/V.18) */
float TextPump::dtmf_tx_sample()
{
	if (d_left <= 0) {
		if (d_tone) {
			d_tone = false;
			d_left = ms2samples(DTMF_OFF_MS, srate);
		} else if (d_seq && *d_seq) {
			dgen.set(*d_seq++, srate);
			d_tone = true;
			d_left = ms2samples(DTMF_ON_MS, srate);
		} else {
			s32 c;
			d_seq = nullptr;
			while ((c = next_char()) >= 0 &&
			       !(d_seq = dtmf_seq(static_cast<char>(c))))
				;
			if (d_seq) {
				tx_on = true;
				dgen.set(*d_seq++, srate);
				d_tone = true;
				d_left = ms2samples(DTMF_ON_MS, srate);
			} else if (tx_on) {
				tx_on = false;
				deaf = ms2samples(DTMF_DEAF_MS, srate);
			}
		}
	}
	if (d_left > 0)
		d_left--;
	return d_tone ? dgen.next() : 0;
}

/* a carrier mode's sample: at each bit the frame's next, a new
   character's frame (7 bits and even parity, or V.18's 8 bits), or a
   mark */
float TextPump::carrier_tx_sample()
{
	return ctx.sample([&]() -> s16 {
		if (!c_bits) {
			s32 c = next_char();
			if (c < 0)
				return 1;
			u16 v = seven ? even_parity(static_cast<u16>(c)) :
				static_cast<u16>(c & 0xFF);
			c_frame = static_cast<u16>((v << 1) | (1 << 9));
			c_bits = 10;
		}
		s16 b = c_frame & 1;
		c_frame >>= 1;
		c_bits--;
		return b;
	});
}

/* the next transmit sample: the mode's (5-bit and EDT: at each half
   bit the frame's next, a new character's frame after the lead's
   marks if the carrier was off, or the hang's marks and then
   silence) */
float TextPump::tx_sample()
{
	if (carrier())
		return carrier_tx_sample();
	if (mode == TM_DTMF)
		return dtmf_tx_sample();
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

/* the remote's DTMF digit d: a character once a digit ends its *s
   and #s (to the link, or held while detecting); a new line, which
   has no CR in DTMF, as CR LF */
void TextPump::dtmf_digit(char d, bool detecting)
{
	if (d_rx_n >= 4)
		d_rx_n = 0;
	d_rx[d_rx_n++] = d;
	d_since = 0;
	if (d < '0' || d > '9')
		return;
	d_rx[d_rx_n] = 0;
	char c = dtmf_char(d_rx);
	d_rx_n = 0;
	if (!c)
		return;
	const char crlf[] = "\r\n", one[2] = { c, 0 };
	for (const char *s = c == '\n' ? crlf : one; *s; s++) {
		if (!detecting)
			rx_char(*s);
		else if (held_n < static_cast<s16>(sizeof(held)))
			held[held_n++] = *s;
	}
}

/* a received sample in data: the characters to the link (in the
   carrierless modes none while we send and just after) */
void TextPump::rx_sample(float x)
{
	if (carrier()) {
		if (txp_skip > 0)
			txp_skip--;
		s16 b = crx.run(x);
		if (b < 0 || !crx.carrier)
			return;
		s32 v = cuart.put(b);
		char c = static_cast<char>(seven ? v & 0x7F : v);
		/* (the rest of the remote's TXPs, V.18's mode just up) */
		if (txp_skip > 0 && (v == 0xD4 || v == 0xD8 || v == 0x50))
			return;
		if (v >= 0 && c)
			rx_char(c);
		return;
	}
	bool listening = deaf <= 0 && !tx_on;
	if (deaf > 0)
		deaf--;
	if (mode == TM_DTMF) {
		DtmfDetect::Event ev = ddet.sample(x);
		rx_busy = ddet.digit != 0;
		if (ev == DtmfDetect::ON && listening)
			dtmf_digit(ddet.digit, false);
		/* a character's digits come within a second */
		if (++d_since >= srate)
			d_rx_n = 0;
		return;
	}
	dem.run(x);
	bool on = dem.carrier && listening;
	rx_busy = on;
	s32 v = uart.run(dem.last_dec, on);
	if (v < 0)
		return;
	if (mode == TM_EDT) {
		char c = static_cast<char>(v & 0x7F);
		if (c)
			rx_char(c);
		return;
	}
	char c = baudot_char(static_cast<s16>(v), &rx_shift);
	if (c)
		rx_char(c);
}

/* the mode found, as the call's statistics name it: DP_V21, DP_V23 and
   DP_B103 for their text telephones, DP_V18 for V.18's own mode */
enum DP_ID TextPump::report_id() const
{
	switch (mode) {
	case TM_BAUDOT: return DP_BAUDOT;
	case TM_EDT:    return DP_EDT;
	case TM_DTMF:   return DP_DTMFTXT;
	case TM_V21:    return DP_V21;
	case TM_B103:   return DP_B103;
	case TM_V23:    return DP_V23;
	default:        return DP_V18;
	}
}

/* in data at once (DPSTAT_CONNECT), at the mode's rates */
s16 TextPump::connect_now()
{
	stat_dp(report_id());
	set_state(TXT_DATA);
	connected = true;
	found_at = ms2samples(100, srate);
	return connect_at(static_cast<s32>(lrintf(baud(false))),
			  static_cast<s32>(lrintf(baud(true))));
}

/* automoding found the remote's mode m_ (a carrier mode's channel by
   low_): in it, in data.  A carrierless mode's receiver goes on where
   the detector is, in the middle of the remote's characters. */
s16 TextPump::found(enum text_mode m_, bool low_)
{
	static const char *const names[] = {
		"5-bit", "EDT", "DTMF", "V.21", "Bell 103", "V.23", "V.18",
	};
	DSP_DBG("text: the remote is %s%s\n", names[m_],
		m_ >= TM_V21 && m_ != TM_V18 ?
		(low_ ? ", sending low" : ", sending high") : "");
	set_mode(m_, low_);
	c_n = c_at = 0;
	sig_on = xci_on = false;
	if (m_ == TM_BAUDOT) {
		dem = d_baudot;
		uart = u_baudot;
		rx_shift = rx_shift_auto;
	} else if (m_ == TM_EDT) {
		dem = d_ch1;
		uart = u_edt;
	} else if (carrier()) {
		/* the receiver of the channel we do not send in */
		enum text_rx ch = mode == TM_B103 ?
			(low ? RX_B103H : RX_B103L) : mode == TM_V23 ?
			(low ? RX_V23F : RX_V23B) : low ? RX_CH2 : RX_CH1;
		static FskDemod TextPump::*const dems[RX_COUNT] = {
			&TextPump::d_ch1, &TextPump::d_ch2, &TextPump::d_v23f,
			&TextPump::d_v23b, &TextPump::d_b103l,
			&TextPump::d_b103h,
		};
		static BitUart TextPump::*const uarts[RX_COUNT] = {
			&TextPump::b_ch1, &TextPump::b_ch2, &TextPump::b_v23f,
			&TextPump::b_v23b, &TextPump::b_b103l,
			&TextPump::b_b103h,
		};
		crx = this->*dems[ch];
		cuart = this->*uarts[ch];
		/* (V.18's: the characters were its TXPs) */
		for (s16 i = 0; m_ != TM_V18 && i < pre_n[ch]; i++) {
			char c = static_cast<char>(pre[ch][i] & 0x7F);
			if (c && held_n < static_cast<s16>(sizeof(held)))
				held[held_n++] = c;
		}
	}
	if (m_ == TM_V18)
		txp_skip = ms2samples(1000, srate);
	reply = !caller && (m_ == TM_BAUDOT || m_ == TM_EDT ||
			    m_ == TM_DTMF);
	return connect_now();
}

/* c (a character, or -1 for none) kept among channel ch's latest */
void TextPump::pre_put(enum text_rx ch, s32 c)
{
	if (c < 0)
		return;
	s16 &n = pre_n[ch];
	if (n == static_cast<s16>(sizeof(pre[ch]))) {
		memmove(pre[ch], pre[ch] + 1, sizeof(pre[ch]) - 1);
		n--;
	}
	pre[ch][n++] = static_cast<char>(c);
}

/* x into channel ch's demodulator d and its characters' receiver u
   while on (and its carrier is there) */
void TextPump::chan_run(FskDemod &d, BitUart &u, enum text_rx ch, float x,
			bool on)
{
	s16 b = d.run(x);
	if (!on || !d.carrier) {
		u = BitUart();
		pre_n[ch] = 0;
	} else if (b >= 0) {
		pre_put(ch, u.put(b));
	}
}

/* automoding's detectors on x: a remote's mode found (found()'s
   result), else DPSTAT_OK.  While a message probe goes out (and just
   after) they decide nothing (its echo is no remote); a mark probe
   hears only its answer; our own CI, TXP and XCI make their channels
   deaf. */
s16 TextPump::detect(float x)
{
	bool probing_tone = state == TXT_PROBE && probe >= PR_V21;
	bool own_text = tx_on || deaf > 0;
	/* answering: CI or XCI heard, TXP (or CM) expected next */
	bool v18_wait = state == TXT_ANS || state == TXT_TXP_GAP ||
		state == TXT_TXP_OUT;

	if (deaf > 0)
		deaf--;

	/* 5-bit: a good character */
	d_baudot.run(x);
	s32 v = u_baudot.run(d_baudot.last_dec,
			     d_baudot.carrier && !own_text && !sig_on);
	if (v >= 0 && !probing_tone && !v18_wait) {
		char c = baudot_char(static_cast<s16>(v), &rx_shift_auto);
		if (c && held_n < static_cast<s16>(sizeof(held)))
			held[held_n++] = c;
		if ((v & 0x1F) != 0)
			return found(TM_BAUDOT, false);
	}

	/* DTMF: a digit (B.3/V.18) */
	if (d_dtmf.sample(x) == DtmfDetect::ON && !own_text &&
	    !probing_tone && !v18_wait) {
		s16 r = found(TM_DTMF, false);
		/* the detector goes on in data, the digit the start of
		   the first character (held until the CONNECT is out) */
		ddet = d_dtmf;
		dtmf_digit(d_dtmf.digit, true);
		return r;
	}

	/* V.21's channel 1: EDT at 110 bit/s; at 300 V.18's CI and TXP,
	   or else (characters that are neither) a V.21 text telephone;
	   or a steady V.21 mark */
	s16 bit = d_ch1.run(x);
	bool ch1 = d_ch1.carrier && !own_text && !(sig_on && caller);
	v = u_edt.run(d_ch1.last_dec, ch1);
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
		b_ch1 = BitUart();
		pre_n[RX_CH1] = 0;
		ch1_other = ch1_txp = 0;
		ch1_last = -1;
	}
	bool fast = ch1_runs >= 3 && ch1_min < srate * 6 / 1000;
	if (ch1 && bit >= 0) {
		s32 c = b_ch1.put(bit);
		pre_put(RX_CH1, c);
		if (c >= 0) {
			static const char txp[] = "TXP";
			bool ci = ch1_last == 0 && (c & 0x1F) == 0x01;
			if ((c & 0x7F) == txp[ch1_txp]) {
				ch1_txp++;
			} else {
				ch1_txp = (c & 0x7F) == 'T';
			}
			if (ch1_txp == 3 && !caller && v18_wait) {
				/* 5.2.2.2: TXP, ours after 75 ms */
				set_state(TXT_TXP_GAP);
				return DPSTAT_OK;
			}
			if (ci && !caller && !v18_wait &&
			    state != TXT_PROBE) {
				/* 5.2.2: CI, our answer tone */
				set_state(TXT_ANS);
				return DPSTAT_OK;
			}
			bool sig_char = c == 0 || ci || ch1_txp > 0;
			ch1_other = sig_char ? 0 : ch1_other + 1;
			ch1_last = static_cast<s16>(c);
			if (!caller && !v18_wait && ch1_other >= 3 &&
			    (!probing_tone || probe == PR_V21))
				return found(TM_V21, false);
		}
	}
	if (!caller && !v18_wait && (!probing_tone || probe == PR_V21) &&
	    d_ch1.last_dec && ch1_runs == 0 &&
	    ch1_on >= ms2samples(probe == PR_V21 && state == TXT_PROBE ?
				 400 : 1500, srate))
		return found(TM_V21, false);
	if (caller && !sig_on && d_ch1.last_dec && ch1_runs == 0 &&
	    ch1_on >= ms2samples(1500, srate))
		/* 5.1.10.2: a V.21 text telephone in its answer mode */
		return found(TM_V21, false);
	if (v >= 0 && !fast && !probing_tone && !v18_wait) {
		char c = static_cast<char>(v & 0x7F);
		if (even_parity(static_cast<u16>(c)) == (v & 0xFF) && c) {
			if (held_n < static_cast<s16>(sizeof(held)))
				held[held_n++] = c;
			return found(TM_EDT, false);
		}
	}

	/* V.21's channel 2: TXP (calling: V.18's mode), or 1650 Hz
	   (a V.21 text telephone answering; answering, one calling) */
	bit = d_ch2.run(x);
	bool ch2 = d_ch2.carrier && !own_text && !(sig_on && !caller);
	ch2_on = ch2 ? ch2_on + 1 : 0;
	if (!ch2) {
		b_ch2 = BitUart();
		pre_n[RX_CH2] = 0;
		ch2_txp = 0;
	} else if (bit >= 0) {
		s32 c = b_ch2.put(bit);
		pre_put(RX_CH2, c);
		if (c >= 0) {
			static const char txp[] = "TXP";
			if ((c & 0x7F) == txp[ch2_txp])
				ch2_txp++;
			else
				ch2_txp = (c & 0x7F) == 'T';
			if (ch2_txp == 3 && caller)
				/* 5.1.3.2 */
				return found(TM_V18, true);
		}
	}
	if (ch2_on >= ms2samples(caller ? 500 : 400, srate) && !v18_wait &&
	    (!probing_tone || probe == PR_V21))
		/* 5.1.5 (or 5.1.3.3), 5.2.9 */
		return found(TM_V21, true);

	/* V.23's forward channel: XCI's markers */
	bit = d_v23f.run(x);
	if (!xci_on && d_v23f.carrier && bit >= 0) {
		s32 c = b_v23f.put(bit);
		pre_put(RX_V23F, c);
		if (c >= 0)
			xci_ff = c == 0xFF ? xci_ff + 1 : 0;
		if (xci_ff >= 2 && !caller) {
			xci_ff = 0;
			xci_seen = 1;
			if (!v18_wait && state != TXT_PROBE) {
				/* 5.2.2: XCI, our answer tone */
				set_state(TXT_ANS);
				return DPSTAT_OK;
			}
		}
	} else if (!d_v23f.carrier) {
		b_v23f = BitUart();
		pre_n[RX_V23F] = 0;
		xci_ff = 0;
	}
	chan_run(d_v23b, b_v23b, RX_V23B, x, !own_text);
	chan_run(d_b103l, b_b103l, RX_B103L, x, !own_text);
	chan_run(d_b103h, b_b103h, RX_B103H, x, !own_text);
	if (xci_seen && ++xci_seen > ms2samples(4000, srate))
		xci_seen = 0;

	/* the tones in blocks of 20 ms: V.23's 390 and 1300 Hz, Bell
	   103's 1270 and 2225 Hz, the answer tone */
	if (!caller) {
		bell.run(x);
		if (bell.found() && !own_text && !v18_wait &&
		    probing_tone && probe == PR_B103)
			return found(TM_B103, false);
	}
	g300.run(x);
	g500.run(x);
	g1100.run(x);
	g1500.run(x);
	g2100.run(x);
	g2050.run(x);
	g1300.run(x);
	g2225.run(x);
	g1270.run(x);
	g1070.run(x);
	g1650.run(x);
	g980.run(x);
	blk_energy += x * x;
	if (g390.run(x)) {
		/* a tone has the most of its block's power (another's
		   leaks some into its filter) */
		const float level = std::max(tone_power(100),
					     0.5f * blk_energy / g390.len);
		blk_energy = 0;
		/* (the blocks end together) */
		bool b390 = g390.power > level &&
			g390.power > 4 * (g300.power + g500.power);
		/* (1270 and 1300 Hz in each other's filters a quarter
		   of their power) */
		bool b1300 = g1300.power > level &&
			g1300.power > 4 * (g1100.power + g1500.power +
					   g2100.power) &&
			g1300.power > 2 * g1270.power;
		bool b2225 = g2225.power > level &&
			g2225.power > 4 * (g2050.power + g2100.power);
		bool b1270 = g1270.power > level &&
			g1270.power > 4 * (g1070.power + g1500.power) &&
			g1270.power > 2 * g1300.power;
		bool b2100 = g2100.power > level &&
			g2100.power > 4 * (g2225.power + g1650.power +
					   g1300.power + g1500.power);
		v23b_blocks = b390 && !own_text ? v23b_blocks + 1 : 0;
		v23f_blocks = b1300 && !own_text && !xci_on ?
			v23f_blocks + 1 : 0;
		b2225_blocks = b2225 && !own_text ? b2225_blocks + 1 : 0;
		b1270_blocks = b1270 && !own_text ? b1270_blocks + 1 : 0;
		ans_blocks = b2100 && !own_text ? ans_blocks + 1 : 0;
		ans_gone = ans_blocks ? 0 : ans_gone + 1;
		auto blocks = [](s32 ms) { return ms / 20; };
		if (!caller && !v18_wait && v23b_blocks >= blocks(300) &&
		    (!probing_tone || probe == PR_V23))
			return found(TM_V23, false);
		if (caller && xci_on && v23b_blocks >= blocks(3000))
			/* 5.1.7: a V.23 terminal calling us back */
			return found(TM_V23, false);
		if (v23f_blocks >= blocks(1700) && !v18_wait &&
		    !xci_seen && (!probing_tone || probe == PR_V23))
			/* 5.1.6, 5.2.11 */
			return found(TM_V23, true);
		if (b2225_blocks >= blocks(caller ? 500 : 1000) &&
		    !v18_wait && (!probing_tone || probe == PR_B103))
			/* 5.1.4, 5.2.8 */
			return found(TM_B103, true);
		if ((caller ? b1270_blocks : bell.blocks) >= blocks(700) &&
		    !v18_wait && (!probing_tone || probe == PR_B103))
			/* 5.1.11, 5.2.7 */
			return found(TM_B103, false);
		if (caller && (state == TXT_LISTEN) &&
		    ans_blocks >= blocks(ANS_MS)) {
			/* 5.1.3: an answer tone, our TXP after it */
			sig_on = xci_on = false;
			set_state(TXT_ANS_GAP);
		}
	}
	return DPSTAT_OK;
}

/* a message probe's sample: its text, then *done when it has gone
   out and the listening after it is over */
float TextPump::probe_sample(s16 *done)
{
	float y = tx_sample();

	*done = 0;
	if (!tx_on && c_at >= c_n && h_at >= h_n && !d_seq) {
		/* listening after it: counted from the end */
		if (++listened >= ms2samples(PROBE_LISTEN_MS, srate))
			*done = 1;
	} else {
		listened = 0;
	}
	return y;
}

/* a V.18 signal's sample on its own bit clock (FSK on mark f[0] and
   space f[1] at f[2] bit/s): the bits of CI, TXP or XCI by sig_bit,
   bits of them in all */
float TextPump::sig_sample(const float *f, s32 bits)
{
	if (!sig_on && !xci_on) {
		sig.init(f[0], f[1], DSP_TX_AMPLITUDE, f[2], srate, 1);
		sig_bit = 0;
	}
	return sig.sample([&]() -> s16 {
		s32 i = sig_bit++;
		if (bits == XCI_BITS)
			return i < bits ? xci_bit(i) : 1;
		return bits == CI_BITS ? ci_bit(i % CI_BITS) :
			txp_bit(i % TXP_BITS);
	});
}

/* the calling V.18's sample while it listens: CI and XCI in their
   cycle (5.1.1); XCI's mark kept on while a 390 Hz answer lasts
   (5.1.7) */
float TextPump::call_sample()
{
	static const float ci[3] = { 980, 1180, 300 };
	static const float xci[3] = { 1300, 2100, 1200 };
	float y = 0;

	switch (call_cycle[seg].sig) {
	case CS_SILENCE:
		break;
	case CS_CI:
		y = sig_sample(ci, CI_BITS);
		sig_on = true;
		break;
	case CS_XCI:
		y = sig_sample(xci, XCI_BITS);
		xci_on = true;
		break;
	}
	if (--seg_left <= 0) {
		if (call_cycle[seg].sig == CS_XCI && v23b_blocks > 0) {
			/* 390 Hz: more mark */
			seg_left = 1;
			return y;
		}
		sig_on = xci_on = false;
		seg = static_cast<s16>((seg + 1) % CALL_SEGS);
		seg_left = ms2samples(call_cycle[seg].ms, srate);
	}
	return y;
}

/* a sample x in, *y out: automoding's listening, signals and probes,
   then the text mode's data */
s16 TextPump::sample(float x, float *y)
{
	s16 r;

	switch (state) {
	case TXT_LISTEN:
		if (caller)
			*y = call_sample();
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (state != TXT_LISTEN)
			break;
		if (!caller && set.probe &&
		    count >= ms2samples(LISTEN_MS, srate)) {
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
		if (probe <= PR_DTMF) {
			*y = probe_sample(&done);
		} else {
			float f;
			switch (probe) {
			case PR_V21:  f = 1650; break;
			case PR_B103: f = 2225; break;
			default:      f = 1300; break;
			}
			tone.set_freq(f, srate);
			*y = DSP_TX_AMPLITUDE * tone.next();
			done = count >= ms2samples(PROBE_TONE_MS, srate);
		}
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (state != TXT_PROBE)
			break;
		if (done) {
			probe = static_cast<enum text_probe>(
				(probe + 1) % PR_COUNT);
			if (probe <= PR_DTMF) {
				set_mode(probe == PR_EDT ? TM_EDT :
					 probe == PR_DTMF ? TM_DTMF :
					 TM_BAUDOT);
				c_n = c_at = 0;
				queue_text(set.msg);
				queue_text("\r\n");
				listened = 0;
			}
			set_state(TXT_PROBE);
		}
		break;
	}
	case TXT_ANS:
		/* 5.2.2: the answer tone (V.25's, so that a V.18 caller
		   goes on with TXP: 5.1.3) until TXP, or Tt */
		tone.set_freq(2100, srate);
		*y = DSP_TX_AMPLITUDE * tone.next();
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (state == TXT_ANS && count >= ms2samples(TT_MS, srate)) {
			xci_seen = 0;
			set_state(TXT_LISTEN);
		}
		break;
	case TXT_TXP_GAP:
		if (count >= ms2samples(75, srate)) {
			sig_seq = 0;
			set_state(TXT_TXP_OUT);
		}
		break;
	case TXT_TXP_OUT: {
		/* 5.2.2.2: three TXPs in V.21's channel 2, then V.18 */
		static const float ch2[3] = { 1650, 1850, 300 };
		*y = sig_sample(ch2, TXP_BITS);
		sig_on = true;
		if (sig_bit >= 3 * TXP_BITS && sig.edge())
			return found(TM_V18, false);
		break;
	}
	case TXT_ANS_GAP:
		/* 5.1.3: 0.5 s of silence after the answer tone came */
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (state == TXT_ANS_GAP &&
		    count >= ms2samples(TXP_GAP_MS, srate)) {
			sig_on = txp_done = false;
			set_state(TXT_TXP);
		}
		break;
	case TXT_TXP: {
		/* 5.1.3, 5.1.3.1: TXP in V.21's channel 1 while the answer
		   tone lasts (the one going out finished), listening for
		   the answerer's TXP, 1650 Hz and 1300 Hz */
		static const float ch1[3] = { 980, 1180, 300 };
		if (!txp_done) {
			if (!sig_on || ans_gone < 2 ||
			    sig_bit % TXP_BITS != 0) {
				*y = sig_sample(ch1, TXP_BITS);
				sig_on = true;
			} else {
				sig_on = false;
				txp_done = true;
			}
		}
		r = detect(x);
		if (r != DPSTAT_OK)
			return r;
		if (state == TXT_TXP && !sig_on && ans_gone >=
		    ms2samples(TXP_LISTEN_MS, srate) / ms2samples(20, srate)) {
			/* nothing came: CI and XCI again */
			seg = 0;
			seg_left = ms2samples(call_cycle[0].ms, srate);
			set_state(TXT_LISTEN);
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
			*y = tx_sample();
			rx_sample(x);
			return connect_at(static_cast<s32>(lrintf(baud(false))),
					  static_cast<s32>(lrintf(baud(true))));
		}
		quiet = (carrier() ? crx.carrier : mode == TM_DTMF ?
			 ddet.digit != 0 : dem.carrier) ? 0 : quiet + 1;
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

/* the pump's ids */
static const enum DP_ID text_ids[] = {
	DP_V18, DP_BAUDOT, DP_EDT, DP_DTMFTXT, DP_V21TXT, DP_B103TXT,
	DP_V23TXT,
};

/* registers the pump as DP_V18, DP_BAUDOT, DP_EDT, DP_DTMFTXT,
   DP_V21TXT, DP_B103TXT and DP_V23TXT; 0 */
s16 dp_text_init(void)
{
	for (enum DP_ID id : text_ids)
		modem_dp_register(id, &PumpOps<TextPump>::ops);
	return 0;
}

/* deregisters them */
void dp_text_exit(void)
{
	for (enum DP_ID id : text_ids)
		modem_dp_deregister(id, &PumpOps<TextPump>::ops);
}

} // namespace dsp
