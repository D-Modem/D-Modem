/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_contactid.cpp - Contact ID (SIA DC-05), the DTMF format alarm
 *                       panels report events to a central station's
 *                       receiver in (DP_CONTACTID)
 *
 *    The answerer is the receiver: its handshake (1400 Hz for 100 ms,
 *    100 ms of silence, 2300 Hz for 100 ms), then the panel's messages,
 *    each 16 DTMF digits (50 to 60 ms on and off): the account (4), the
 *    message type (18 or 98), the qualifier (1 new event or opening, 3
 *    new restore or closing, 6 a previous event still there), the event
 *    code (3), the group or partition (2), the zone or user (3) and the
 *    checksum (1).  A digit's value is its key's, but for 0 (10), * (11,
 *    B), # (12, C), A (13, D), B (14, E) and C (15, F); a message is
 *    good when the values add up to a multiple of 15.  A good one gets
 *    the kissoff, 1400 Hz for 900 ms, and goes to the DTE as a line of
 *    16 digits (0-9, B-F); a bad one gets nothing and the panel
 *    sends it again.  With no message 3 s after a handshake, the
 *    handshake again (four in all).
 *
 *    The caller is the panel: after its dialling, the receiver's
 *    handshake, then the DTE's lines, each a message of 16 digits, or
 *    15 and the checksum added (0-9, B-F, and A for 0: the same value;
 *    spaces and dashes ignored); each goes out 250 ms after the handshake or the previous
 *    kissoff, and again (four times in all) without a kissoff within
 *    1.25 s of its end.  Each line gets "ACK" back to the DTE, "NAK"
 *    after four times without a kissoff, "ERROR" if it is no message.
 *
 *    Half duplex: neither listens while it sends.  The DTE's characters
 *    and the lines to it are the core's asynchronous link's (8N1, no
 *    error correction).  The call is up from the handshake (the
 *    answerer's, the caller's heard) until either side hangs up.
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

/* the receiver's tones and times (DC-05) */
#define CID_HS_LO        1400   /* Hz */
#define CID_HS_HI        2300
#define CID_HS_MS        100    /* each tone, and the silence between */
#define CID_KISS_MS      900
#define CID_HS_WAIT_MS   3000   /* no message after a handshake: again */
#define CID_HS_TIMES     4
#define CID_GAP_MS       1000   /* between a message's digits at most */
#define CID_KISS_DELAY_MS 100   /* after the last digit's end */
/* the panel's */
#define CID_DIGIT_MS     55     /* on, and off after */
#define CID_PAUSE_MS     250    /* after a handshake or a kissoff */
#define CID_KISS_WAIT_MS 1250   /* a message's end to its kissoff */
#define CID_KISS_MIN_MS  400    /* the shortest kissoff taken */
#define CID_TRIES        4
/* the panel's tone detector's blocks */
#define CID_BLOCK_MS     10
#define CID_TONE_BLOCKS  5      /* a handshake tone: 50 ms of it */
/* the DTE's lines waiting */
#define CID_QUEUE        8
#define CID_DIGITS       16

/* the pump's rate to the core (CONNECT): 10 digits a second */
#define CID_RATE         40

enum cid_state {
	CID_HANDSHAKE,  /* the receiver: its handshake going out */
	CID_LISTEN,     /* the receiver: for the panel's digits */
	CID_KISSOFF,    /* the receiver: its kissoff going out */
	CID_WAIT_HS,    /* the panel: for the receiver's handshake */
	CID_READY,      /* the panel: a pause, then a message */
	CID_SEND,       /* the panel: a message going out */
	CID_WAIT_KISS,  /* the panel: for its kissoff */
	CID_KISS_END,   /* the panel: the kissoff heard, for its end */
};

/* the DTMF keys of the values 1 to 15, and their digits (10 is 0, as
   Contact ID writes it) */
static const char cid_keys[] = "1234567890*#ABC";
static const char cid_hex[] = "1234567890BCDEF";

/* digit h's value (A and 0 both 10), 0 if it is none */
static s16 cid_value(char h)
{
	if (h >= 'a' && h <= 'f')
		h = static_cast<char>(h - 'a' + 'A');
	if (h == 'A')
		return 10;
	const char *p = strchr(cid_hex, h);
	return p && h ? static_cast<s16>(p - cid_hex + 1) : 0;
}

struct ContactIdPump : PumpBase<ContactIdPump, cid_state> {
	static constexpr const char *name = "Contact ID (SIA DC-05) pump";

	/* the receiver */
	DtmfDetect dtmf;
	char rx[CID_DIGITS + 1];        /* the message's values so far, in
					   hex */
	s16 rx_n;
	s32 since_digit;                /* samples since the last digit's
					   end (or its start while on) */
	s16 handshakes;
	s32 messages;

	/* the panel */
	Goertzel g_lo, g_hi;
	PowerMeter level;
	s16 lo_blocks, hi_blocks, lo_seen;
	char line[40];                  /* the DTE's line so far */
	s16 line_n;
	char queue[CID_QUEUE][CID_DIGITS];
	s16 q_n;
	s16 tries;
	DtmfGen gen;
	s16 tx_digit;                   /* the digit going out */
	bool tx_tone;                   /* its tone (else the pause after) */

	/* both: the tone going out */
	Osc tone;
	bool connected;

	/* its name in the log */
	const char *tag() const { return "contactid"; }
	/* in data (no handshake timeout; the carrier is not watched) */
	bool in_data() const { return connected; }
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 sample(float x, float *y);
	/* cnt samples in and out, through sample() */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}

	/* n ms in samples */
	s32 ms(s32 n) const { return ms2samples(n, srate); }
	void to_link(const char *s);
	s32 dte_char();
	void dte_line();
	void rx_digit(char key);
	float rx_sample(float x);
	float tx_sample(float x);
	void listen_tones(float x);
};

/* the receiver sends its handshake at once; the panel listens for it */
s16 ContactIdPump::start(struct modem *m, enum DP_ID id, s16 caller_,
			 s32 srate_, s32)
{
	if (id != DP_CONTACTID)
		return -1;
	pump_setup(m, srate_, caller_);
	dtmf.init(srate, 128);
	s16 blk = static_cast<s16>(ms(CID_BLOCK_MS));
	g_lo.init(CID_HS_LO, blk, srate);
	g_hi.init(CID_HS_HI, blk, srate);
	level.init(CID_BLOCK_MS / 1000.0f, srate);
	tone.set_freq(CID_HS_LO, srate);
	state = caller ? CID_WAIT_HS : CID_HANDSHAKE;
	DSP_DBG("contactid: create %s\n", caller ? "panel" : "receiver");
	return 0;
}

/* line s to the DTE, a character at a time in 8N1 */
void ContactIdPump::to_link(const char *s)
{
	for (; *s; s++) {
		u8 bits[11];
		bits[0] = 1;
		bits[1] = 0;
		for (s16 i = 0; i < 8; i++)
			bits[2 + i] = (static_cast<u8>(*s) >> i) & 1;
		bits[10] = 1;
		modem_put_bits(modem, 1, bits, 11);
	}
}

/* the DTE's next character off the link's 8N1 bit stream (its idle
   marks one at a time), -1 for none */
s32 ContactIdPump::dte_char()
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

/* the panel: the DTE's line ended: its message queued (the checksum
   added to 15 digits), or ERROR */
void ContactIdPump::dte_line()
{
	char msg[CID_DIGITS];
	s16 n = 0, sum = 0;

	for (s16 i = 0; i < line_n; i++) {
		char c = line[i];
		if (c == ' ' || c == '-')
			continue;
		s16 v = cid_value(c);
		if (!v || n >= CID_DIGITS) {
			n = -1;
			break;
		}
		msg[n++] = cid_hex[v - 1];
		sum += v;
	}
	line_n = 0;
	if (n == CID_DIGITS - 1) {
		s16 r = 15 - sum % 15;
		msg[n++] = cid_hex[r - 1];
	} else if (n != CID_DIGITS) {
		if (n)
			to_link("ERROR\r\n");
		return;
	}
	if (q_n >= CID_QUEUE) {
		to_link("ERROR\r\n");
		return;
	}
	memcpy(queue[q_n++], msg, CID_DIGITS);
}

/* the receiver: a digit (its key) heard */
void ContactIdPump::rx_digit(char key)
{
	const char *p = strchr(cid_keys, key);
	if (!p || !key) {
		DSP_DBG("contactid: key %c is no digit\n", key);
		rx_n = 0;
		return;
	}
	rx[rx_n++] = cid_hex[p - cid_keys];
	if (rx_n < CID_DIGITS)
		return;
	rx[rx_n] = 0;
	rx_n = 0;
	s16 sum = 0;
	for (s16 i = 0; i < CID_DIGITS; i++)
		sum += cid_value(rx[i]);
	if (sum % 15) {
		DSP_DBG("contactid: %s: bad checksum\n", rx);
		return;
	}
	DSP_DBG("contactid: message %s\n", rx);
	messages++;
	to_link(rx);
	to_link("\r\n");
	tone.set_freq(CID_HS_LO, srate);
	/* the kissoff after the digit's end and a pause */
	set_state(CID_KISSOFF);
	count = -ms(CID_KISS_DELAY_MS);
}

/* the receiver's sample: its tones out, digits in */
float ContactIdPump::rx_sample(float x)
{
	float y = 0;
	switch (state) {
	case CID_HANDSHAKE:
		if (count < ms(CID_HS_MS)) {
			y = tone.next();
		} else if (count < 2 * ms(CID_HS_MS)) {
			if (count == ms(CID_HS_MS))
				tone.set_freq(CID_HS_HI, srate);
		} else if (count < 3 * ms(CID_HS_MS)) {
			y = tone.next();
		} else {
			handshakes++;
			dtmf.init(srate, 128);
			rx_n = 0;
			since_digit = 0;
			set_state(CID_LISTEN);
		}
		return y * DSP_TX_AMPLITUDE;
	case CID_KISSOFF:
		/* (count negative: the pause before it, still listening
		   for the end of the last digit) */
		if (count >= 0 && count < ms(CID_KISS_MS))
			return tone.next() * DSP_TX_AMPLITUDE;
		if (count >= ms(CID_KISS_MS)) {
			dtmf.init(srate, 128);
			rx_n = 0;
			since_digit = 0;
			set_state(CID_LISTEN);
		}
		return 0;
	default:
		break;
	}
	/* CID_LISTEN */
	switch (dtmf.sample(x)) {
	case DtmfDetect::ON:
		rx_digit(dtmf.digit);
		since_digit = 0;
		break;
	case DtmfDetect::OFF:
		since_digit = 0;
		break;
	default:
		since_digit++;
		break;
	}
	if (state != CID_LISTEN)
		return 0;
	if (rx_n && !dtmf.digit && since_digit >= ms(CID_GAP_MS)) {
		DSP_DBG("contactid: %d digits, then none\n", rx_n);
		rx_n = 0;
	}
	if (!messages && !rx_n && !dtmf.digit &&
	    count >= ms(CID_HS_WAIT_MS) && handshakes < CID_HS_TIMES) {
		tone.set_freq(CID_HS_LO, srate);
		set_state(CID_HANDSHAKE);
	}
	return 0;
}

/* the panel: the receiver's tones counted in blocks of 10 ms (a tone:
   its power half the block's or more, and above -50 dBFS) */
void ContactIdPump::listen_tones(float x)
{
	float e = level.run(x);
	bool lo = g_lo.run(x), hi = g_hi.run(x);
	if (!lo || !hi)
		return;
	float floor = 100.0f * 100.0f;
	bool on_lo = g_lo.power > floor && g_lo.power > 0.5f * e;
	bool on_hi = g_hi.power > floor && g_hi.power > 0.5f * e;
	lo_blocks = on_lo ? lo_blocks + 1 : 0;
	hi_blocks = on_hi ? hi_blocks + 1 : 0;
}

/* the panel's sample: the receiver's tones in, the messages out */
float ContactIdPump::tx_sample(float x)
{
	s32 c;
	while ((c = dte_char()) >= 0) {
		if (c == '\r' || c == '\n')
			dte_line();
		else if (line_n < static_cast<s16>(sizeof(line)))
			line[line_n++] = static_cast<char>(c);
	}
	switch (state) {
	case CID_WAIT_HS:
		listen_tones(x);
		if (lo_blocks >= CID_TONE_BLOCKS)
			lo_seen = 1;
		if (lo_seen && hi_blocks >= CID_TONE_BLOCKS) {
			DSP_DBG("contactid: handshake\n");
			set_state(CID_READY);
		}
		return 0;
	case CID_READY:
		/* (a handshake again, the 2300 Hz still on: from its end) */
		listen_tones(x);
		if (hi_blocks || lo_blocks)
			count = 0;
		if (count < ms(CID_PAUSE_MS) || !q_n)
			return 0;
		tries++;
		tx_digit = 0;
		tx_tone = true;
		gen.set(cid_keys[cid_value(queue[0][0]) - 1], srate);
		set_state(CID_SEND);
		return gen.next();
	case CID_SEND:
		if (count >= ms(CID_DIGIT_MS)) {
			count = 0;
			if (tx_tone) {
				tx_tone = false;
			} else if (++tx_digit >= CID_DIGITS) {
				lo_blocks = hi_blocks = 0;
				set_state(CID_WAIT_KISS);
				return 0;
			} else {
				tx_tone = true;
				gen.set(cid_keys[cid_value(
					queue[0][tx_digit]) - 1], srate);
			}
		}
		return tx_tone ? gen.next() : 0;
	case CID_WAIT_KISS:
		listen_tones(x);
		if (lo_blocks * CID_BLOCK_MS >= CID_KISS_MIN_MS) {
			DSP_DBG("contactid: kissoff\n");
			to_link("ACK\r\n");
			memmove(queue[0], queue[1],
				sizeof(queue[0]) * (CID_QUEUE - 1));
			q_n--;
			tries = 0;
			set_state(CID_KISS_END);
		} else if (!lo_blocks &&
			   count >= ms(CID_KISS_WAIT_MS + CID_KISS_MIN_MS)) {
			DSP_DBG("contactid: no kissoff (%d)\n", tries);
			if (tries >= CID_TRIES) {
				to_link("NAK\r\n");
				memmove(queue[0], queue[1],
					sizeof(queue[0]) * (CID_QUEUE - 1));
				q_n--;
				tries = 0;
			}
			set_state(CID_READY);
		}
		return 0;
	case CID_KISS_END:
		listen_tones(x);
		if (!lo_blocks && count >= ms(CID_BLOCK_MS * 2))
			set_state(CID_READY);
		else if (lo_blocks)
			count = 0;
		return 0;
	default:
		return 0;
	}
}

/* see pump.h: connected from the handshake (the receiver's sent, the
   panel's heard) */
s16 ContactIdPump::sample(float x, float *y)
{
	carrier_tick(1);
	if (caller) {
		*y = tx_sample(x);
		if (!connected && state != CID_WAIT_HS) {
			connected = true;
			return connect_at(CID_RATE, CID_RATE);
		}
		return DPSTAT_OK;
	}
	/* the receiver drops what its DTE types */
	while (dte_char() >= 0) {
	}
	*y = rx_sample(x);
	if (!connected) {
		connected = true;
		return connect_at(CID_RATE, CID_RATE);
	}
	return DPSTAT_OK;
}

/* registers the pump as DP_CONTACTID; 0 */
s16 dp_contactid_init(void)
{
	modem_dp_register(DP_CONTACTID, &PumpOps<ContactIdPump>::ops);
	return 0;
}

/* deregisters it */
void dp_contactid_exit(void)
{
	modem_dp_deregister(DP_CONTACTID, &PumpOps<ContactIdPump>::ops);
}

} // namespace dsp
