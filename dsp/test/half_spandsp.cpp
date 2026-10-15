/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_spandsp.cpp - spandsp's data modems (libspandsp 0.0.6, as
 *    Asterisk and FreeSWITCH have it) as a test peer (modem_half.hpp):
 *    line audio from fd 3 and to fd 4 (16 bit, 8000 Hz), AT commands on
 *    a pty whose name is printed ("created spandsp: /dev/pts/N").
 *
 *    spandsp has the pumps but no modem around them (its data_modems of
 *    later versions is a sketch, and its V.32bis and V.34 there are not
 *    finished), so this is one: a few AT commands, V.8 or a forced
 *    modulation, and its V.21, V.23, Bell 103 and V.22/V.22bis pumps with
 *    asynchronous 8N1 characters.  No error correction: our modem's V.42
 *    detection finds none and goes on without.
 *
 *    AT+MS=<m> as ours (21, 22, 122, 23, 103; or V21, V22, V22B, V23C,
 *    B103), default V.8 (V.22bis, V.22, V.21 offered); ATD, ATA, ATH, ATO,
 *    +++, ATZ, the rest OK.  SPANDSP_SYNC as the others';
 *    SPANDSP_DEBUG=1 prints spandsp's logs and ours.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <spandsp.h>

#include "modem_half.hpp"

using namespace half;

#define RATE 8000
#define MS(x) ((x) * RATE / 1000)

/* spandsp's log messages: to stderr */
static void span_msg(int level, const char *text)
{
	(void)level;
	fprintf(stderr, "spandsp: %s", text);
}

/*
 *    the modem
 */

class Spandsp : public HalfModem {
public:
	Spandsp() : HalfModem("spandsp", "SPANDSP") {}

protected:
	int start() override { return 0; }
	int line_block(const short *in, short *out, int n) override;
	void dte_poll() override;

private:
	enum State {
		IDLE,           /* on hook, or in command mode */
		ANS_SILENCE,    /* answering: 1 s */
		ANS_TONE,       /* 2100 Hz for 3.3 s (V.25) */
		ANS_GAP,        /* 75 ms */
		CALL_WAIT,      /* calling: for the answer tone */
		CALL_TONE,      /* in it: for its end */
		V8,             /* V.8, either side */
		PUMP,           /* the modulation's handshake and data */
	};

	State state = IDLE;
	int caller = 0;
	long count = 0;                 /* samples in the state */
	int ms_req = 0;                 /* +MS: 0 V.8, else 21, 22, 122, 23, 103 */
	int pump_ms = 0;                /* the one running */
	int online = 0, connected = 0;  /* in data mode; CONNECT given */
	long carrier_off = 0;           /* samples without the remote's carrier */
	long rx_up = 0;                 /* samples since its carrier came */
	int rx_carrier = 0;
	long last_dte = 0;              /* line_n of the DTE's last byte */
	int plus_n = 0;                 /* +++ seen */
	float pwr = 0;                  /* the line's power, for the caller's
					   wait for the answer tone and its
					   end */

	/* data from the DTE, for the line */
	unsigned char txq[8192];
	int txq_head = 0, txq_tail = 0;

	/* (spandsp 0.0.6's states are opaque: its own allocations) */
	v8_state_t *v8 = nullptr;
	v8_parms_t v8_parms;
	modem_connect_tones_tx_state_t *ans_tx = nullptr;
	v22bis_state_t *v22 = nullptr;
	fsk_tx_state_t *ftx = nullptr;
	fsk_rx_state_t *frx = nullptr;
	/* 8N1 characters each way, ours (0.0.6's async_tx gives the pump a
	   status for a bit when there is nothing to send) */
	int tx_frame = 0, tx_bits = 0;  /* the character going out, bits left */
	int rx_frame = 0, rx_bits = 0;  /* the one coming in, bits so far */
	int v8_result = -1, v8_mods = 0;

	char cmd[256];                  /* the AT command line so far */
	int cmd_n = 0;

	void result(const char *s);
	void set_state(State s);
	int get_byte();
	void put_byte(int byte);
	void do_connect(int rate);
	void status(int st);
	int get_bit();
	void put_bit(int bit);
	void start_pump(int ms);
	int fsk_rate() const;
	void start_v8();
	void hang_up();
	void dial(int as_caller);
	void at_line(char *s);

	/* spandsp's callbacks, to this modem */
	static int get_bit_cb(void *m) { return ((Spandsp *)m)->get_bit(); }
	static void put_bit_cb(void *m, int bit) { ((Spandsp *)m)->put_bit(bit); }
	static void status_cb(void *m, int st) { ((Spandsp *)m)->status(st); }
	static void v8_cb(void *m, v8_parms_t *r);
};

static Spandsp spandsp;
#define LOG(...) do { if (spandsp.dbg) spandsp.log(__VA_ARGS__); } while (0)

/*
 *    the DTE
 */

/* a result code, "\r\n<s>\r\n" */
void Spandsp::result(const char *s)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "\r\n%s\r\n", s);
	pty.write(buf, strlen(buf));
	LOG("result %s\n", s);
}

/*
 *    the call
 */

/* to state s, its sample count from 0 */
void Spandsp::set_state(State s)
{
	static const char *const state_name[] = {
		"idle", "ans silence", "ans tone", "ans gap", "call wait",
		"call tone", "v8", "pump"
	};
	LOG("state %s -> %s at %ld ms\n", state_name[state], state_name[s],
	    (long)(line_n * 1000 / RATE));
	state = s;
	count = 0;
}

/* the DTE's next character for the line, -1 if none or offline */
int Spandsp::get_byte()
{
	if (!online || txq_head == txq_tail)
		return -1;
	int c = txq[txq_tail];
	txq_tail = (txq_tail + 1) % (int)sizeof(txq);
	return c;
}

/* a character from the line to the DTE, online (< 0: none) */
void Spandsp::put_byte(int byte)
{
	unsigned char c = (unsigned char)byte;
	if (byte < 0 || !online)
		return;
	pty.write(&c, 1);
}

/* CONNECT <rate>, once a call; data mode */
void Spandsp::do_connect(int rate)
{
	char buf[32];
	if (connected)
		return;
	connected = online = 1;
	snprintf(buf, sizeof(buf), "CONNECT %d", rate);
	result(buf);
}

/* a pump's status: V.22bis trained (CONNECT), the remote's carrier
   up and down */
void Spandsp::status(int st)
{
	LOG("status %s\n", signal_status_to_str(st));
	switch (st) {
	case SIG_STATUS_TRAINING_SUCCEEDED:
		if (pump_ms == 22 || pump_ms == 122)
			do_connect(v22bis_get_current_bit_rate(v22));
		break;
	case SIG_STATUS_CARRIER_UP:
		rx_carrier = 1;
		rx_up = 0;
		break;
	case SIG_STATUS_CARRIER_DOWN:
		rx_carrier = 0;
		break;
	}
}

/* the pump's next bit to send: 8N1 characters LSB first, marks
   between them */
int Spandsp::get_bit()
{
	int bit;
	if (!tx_bits) {
		int c = get_byte();
		if (c < 0)
			return 1;               /* (mark: idle) */
		tx_frame = c << 1 | 0x200;      /* start, 8 bits, stop */
		tx_bits = 10;
	}
	bit = tx_frame & 1;
	tx_frame >>= 1;
	tx_bits--;
	return bit;
}

/* a bit from the pump (< 0: a status): once connected, 8N1 characters
   out of them, those with their stop bit to the DTE */
void Spandsp::put_bit(int bit)
{
	if (bit < 0) {
		status(bit);
		return;
	}
	if (!connected)
		return;
	if (!rx_bits) {
		if (!bit)
			rx_bits = 1;            /* a start bit */
		rx_frame = 0;
		return;
	}
	if (rx_bits <= 8) {
		rx_frame |= bit << (rx_bits - 1);
		rx_bits++;
		return;
	}
	/* the stop bit */
	if (bit)
		put_byte(rx_frame);
	rx_bits = 0;
}

/* the pump of +MS code ms (22 and 122 V.22bis's, 23, 103, else V.21:
   FSK) for our role; the PUMP state */
void Spandsp::start_pump(int ms)
{
	const fsk_spec_t *tx, *rx;

	pump_ms = ms;
	tx_bits = rx_bits = 0;
	rx_carrier = 0;
	carrier_off = 0;
	LOG("pump %d, %s\n", ms, caller ? "caller" : "answerer");
	switch (ms) {
	case 22:
	case 122:
		if (v22)
			v22bis_free(v22);
		v22 = v22bis_init(NULL, ms == 122 ? 2400 : 1200, 0, caller,
				  get_bit_cb, this, put_bit_cb, this);
		v22bis_set_modem_status_handler(v22, status_cb, this);
		if (dbg) {
			span_log_set_level(v22bis_get_logging_state(v22),
					   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
			span_log_set_tag(v22bis_get_logging_state(v22),
					 "V.22bis");
			span_log_set_message_handler(
				v22bis_get_logging_state(v22), span_msg);
		}
		break;
	default:
		/* the caller sends in the low channel (V.23: the 75 bit/s
		   backward one) */
		switch (ms) {
		case 103: {
			/* (which of its channels is the low one differs
			   between spandsp's versions) */
			const fsk_spec_t *lo = &preset_fsk_specs[FSK_BELL103CH1],
				*hi = &preset_fsk_specs[FSK_BELL103CH2];
			if (lo->freq_one > hi->freq_one) {
				lo = hi;
				hi = &preset_fsk_specs[FSK_BELL103CH1];
			}
			tx = caller ? lo : hi;
			rx = caller ? hi : lo;
			break;
		}
		case 23:
			tx = &preset_fsk_specs[caller ? FSK_V23CH2 :
					       FSK_V23CH1];
			rx = &preset_fsk_specs[caller ? FSK_V23CH1 :
					       FSK_V23CH2];
			break;
		default:
			tx = &preset_fsk_specs[caller ? FSK_V21CH1 :
					       FSK_V21CH2];
			rx = &preset_fsk_specs[caller ? FSK_V21CH2 :
					       FSK_V21CH1];
			break;
		}
		if (ftx)
			fsk_tx_free(ftx);
		if (frx)
			fsk_rx_free(frx);
		ftx = fsk_tx_init(NULL, tx, get_bit_cb, this);
		frx = fsk_rx_init(NULL, rx, FSK_FRAME_MODE_ASYNC, put_bit_cb, this);
		break;
	}
	set_state(PUMP);
}

/* the FSK pump's rate for the CONNECT: what this side receives (V.23:
   the caller 1200, the answerer 75) */
int Spandsp::fsk_rate() const
{
	return pump_ms == 23 ? (caller ? 1200 : 75) : 300;
}

/* V.8's news: its status and the modulations, an offer cut to ours */
void Spandsp::v8_cb(void *m, v8_parms_t *r)
{
	Spandsp *s = (Spandsp *)m;
	LOG("v8 status %d, modulations %#x\n", r->status, r->modulations);
	/* the CM's offer: the JM says what both have (0.0.6 leaves that to
	   us, and would send the CM's back) */
	if (r->status == V8_STATUS_V8_OFFERED)
		r->modulations &= V8_MOD_V21 | V8_MOD_V22 | V8_MOD_V23;
	s->v8_result = r->status;
	s->v8_mods = r->modulations;
}

/* V.8 in our role (the answerer with ANSam), offering V.21, V.22(bis)
   and V.23, no protocol */
void Spandsp::start_v8()
{
	memset(&v8_parms, 0, sizeof(v8_parms));
	v8_parms.modem_connect_tone = caller ? MODEM_CONNECT_TONES_NONE :
		MODEM_CONNECT_TONES_ANSAM_PR;
	v8_parms.send_ci = 0;
	v8_parms.v92 = -1;
	v8_parms.call_function = V8_CALL_V_SERIES;
	v8_parms.modulations = V8_MOD_V21 | V8_MOD_V22 | V8_MOD_V23;
	v8_parms.protocol = V8_PROTOCOL_NONE;
	v8_parms.pstn_access = 0;
	v8_parms.pcm_modem_availability = 0;
	v8_parms.nsf = -1;
	v8_parms.t66 = -1;
	v8_result = -1;
	if (v8)
		v8_free(v8);
	v8 = v8_init(NULL, caller, &v8_parms, v8_cb, this);
	if (dbg) {
		span_log_set_level(v8_get_logging_state(v8),
				   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
		span_log_set_tag(v8_get_logging_state(v8), "V.8");
		span_log_set_message_handler(v8_get_logging_state(v8),
					     span_msg);
	}
	set_state(V8);
}

/* on hook, offline */
void Spandsp::hang_up()
{
	online = connected = 0;
	pump_ms = 0;
	set_state(IDLE);
}

/* ATD (as_caller) or ATA: V.8 (the answerer after 1 s), or +MS's
   modulation with its tones */
void Spandsp::dial(int as_caller)
{
	caller = as_caller;
	online = connected = 0;
	txq_head = txq_tail = 0;
	if (ms_req == 0) {
		if (caller)
			start_v8();
		else
			set_state(ANS_SILENCE);
		return;
	}
	set_state(caller ? CALL_WAIT : ANS_SILENCE);
}

/* one block of line samples each way (to the next 48th of the line);
   all of it */
int Spandsp::line_block(const short *in, short *out, int n)
{
	int i;

	for (i = 0; i < n; i++)
		pwr = 0.95f * pwr + 0.05f * (float)in[i] * in[i];
	memset(out, 0, n * sizeof(*out));
	if (dbg > 1 && line_n % 800 < (unsigned long long)n)
		LOG("%ld ms: power %.0f\n", (long)(line_n * 1000 / RATE), pwr);
	count += n;
	switch (state) {
	case IDLE:
		break;
	case ANS_SILENCE:
		if (count >= MS(1000)) {
			if (ms_req == 0) {
				start_v8();
			} else if (ms_req == 103) {
				/* (the answerer's mark is its answer) */
				start_pump(ms_req);
			} else {
				if (ans_tx)
					modem_connect_tones_tx_free(ans_tx);
				ans_tx = modem_connect_tones_tx_init(NULL,
					MODEM_CONNECT_TONES_ANS);
				set_state(ANS_TONE);
			}
		}
		break;
	case ANS_TONE:
		modem_connect_tones_tx(ans_tx, out, n);
		if (count >= MS(3300))
			set_state(ANS_GAP);
		break;
	case ANS_GAP:
		if (count >= MS(75))
			start_pump(ms_req);
		break;
	case CALL_WAIT:
		if (pwr > 1e5f) {
			if (ms_req == 103)
				start_pump(ms_req);
			else
				set_state(CALL_TONE);
		} else if (count >= MS(60000)) {
			result("NO CARRIER");
			hang_up();
		}
		break;
	case CALL_TONE:
		/* its end (a short gap, then the answerer's signal) */
		if (pwr < 1e4f && count >= MS(500))
			start_pump(ms_req);
		break;
	case V8:
		v8_rx(v8, in, n);
		v8_tx(v8, out, n);
		if (v8_result == V8_STATUS_V8_CALL) {
			start_pump(v8_mods & V8_MOD_V22 ? 122 :
				   v8_mods & V8_MOD_V23 ? 23 : 21);
		} else if (v8_result == V8_STATUS_NON_V8_CALL) {
			/* an answer tone without V.8: as V.22bis */
			start_pump(122);
		} else if (v8_result == V8_STATUS_FAILED ||
			   count >= MS(60000)) {
			result("NO CARRIER");
			hang_up();
		}
		break;
	case PUMP:
		if (pump_ms == 22 || pump_ms == 122) {
			v22bis_rx(v22, in, n);
			v22bis_tx(v22, out, n);
		} else {
			fsk_rx(frx, in, n);
			fsk_tx(ftx, out, n);
			if (rx_carrier) {
				rx_up += n;
				if (rx_up >= MS(500))
					do_connect(fsk_rate());
			}
		}
		/* the remote's carrier gone for 1.4 s (S10) */
		carrier_off = rx_carrier || !connected ? 0 : carrier_off + n;
		if (carrier_off >= MS(1400) ||
		    (!connected && count >= MS(60000))) {
			result("NO CARRIER");
			hang_up();
		}
		break;
	}
	line_n += n;
	return n;
}

/*
 *    AT commands
 */

/* AT+MS='s modulation: 21, 22, 122, 23, 103 as numbers or names; 0 for
   V.8 (and what we do not have, from 32 up), -1 for an error */
static int parse_ms(const char *p)
{
	static const struct { const char *name; int ms; } t[] = {
		{ "V21", 21 }, { "V22B", 122 }, { "V22", 22 }, { "V23C", 23 },
		{ "V23", 23 }, { "B103", 103 }, { "V8", 0 },
	};
	unsigned i;
	if (*p >= '0' && *p <= '9') {
		int v = atoi(p);
		return v == 21 || v == 22 || v == 122 || v == 23 ||
			v == 103 ? v : v >= 32 ? 0 : -1;
	}
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strncasecmp(p, t[i].name, strlen(t[i].name)))
			return t[i].ms;
	/* (V32, V34, V90, V92: V.8, where we offer what we have) */
	return 0;
}

/* an AT command line: ATD and ATA start a call (no OK), ATO goes back
   online (CONNECT), +MS sets the modulation, the rest OK or ERROR */
void Spandsp::at_line(char *s)
{
	char *p;
	int ok = 1;

	while (*s == ' ')
		s++;
	if (strncasecmp(s, "AT", 2)) {
		return;
	}
	LOG("cmd %s\n", s);
	for (p = s + 2; *p && ok; ) {
		int c = toupper((unsigned char)*p++);
		switch (c) {
		case ' ':
			break;
		case 'Z':
			ms_req = 0;
			while (*p >= '0' && *p <= '9')
				p++;
			break;
		case 'D':
			dial(1);
			return;
		case 'A':
			dial(0);
			return;
		case 'H':
			while (*p >= '0' && *p <= '9')
				p++;
			hang_up();
			break;
		case 'O':
			while (*p >= '0' && *p <= '9')
				p++;
			if (!connected) {
				ok = 0;
				break;
			}
			online = 1;
			result("CONNECT");
			return;
		case '+':
			if (!strncasecmp(p, "MS=", 3)) {
				int m = parse_ms(p + 3);
				if (m < 0)
					ok = 0;
				else
					ms_req = m;
				p += strcspn(p, ";");
			} else {
				p += strcspn(p, ";");
			}
			break;
		case '&':
		case '\\':
		case '%':
			if (*p)
				p++;
			while (*p >= '0' && *p <= '9')
				p++;
			break;
		case 'S':
			while (*p && *p != ';' && !isalpha((unsigned char)*p))
				p++;
			break;
		case ';':
			break;
		default:
			/* (E, V, X, Q, M, L, I, ...: as they come) */
			while (*p >= '0' && *p <= '9')
				p++;
			break;
		}
	}
	result(ok ? "OK" : "ERROR");
}

/* what waits to the DTE, then the DTE's input: data online (+++ with
   a second of quiet each side back to commands), else commands, echoed;
   in command mode at SPANDSP_SYNC's samples only */
void Spandsp::dte_poll()
{
	unsigned char buf[256];
	int n = sizeof(buf), k;

	pty.flush();
	if (dte_waits(online))
		return;
	/* online, no more than the line's queue has room for: the rest
	   waits in the pty */
	if (online) {
		k = (txq_tail - txq_head - 1 + (int)sizeof(txq)) %
		    (int)sizeof(txq);
		if (k < n)
			n = k;
		if (!n)
			return;
	}
	n = (int)::read(pty.fd(), buf, (size_t)n);
	for (k = 0; k < n; k++) {
		unsigned char c = buf[k];
		if (online) {
			/* +++ after a second of quiet, and a second after
			   it: command mode */
			if (c == '+' && (plus_n ? plus_n < 3 :
					 (long)line_n - last_dte >= MS(1000)))
				plus_n++;
			else
				plus_n = 0;
			last_dte = (long)line_n;
			if ((txq_head + 1) % (int)sizeof(txq) != txq_tail) {
				txq[txq_head] = c;
				txq_head = (txq_head + 1) % (int)sizeof(txq);
			}
			continue;
		}
		pty.write(&c, 1);               /* (echo) */
		if (c == '\r' || c == '\n') {
			cmd[cmd_n] = 0;
			if (cmd_n)
				at_line(cmd);
			cmd_n = 0;
		} else if (cmd_n < (int)sizeof(cmd) - 1) {
			cmd[cmd_n++] = (char)c;
		}
	}
	if (online && plus_n == 3 && (long)line_n - last_dte >= MS(1000)) {
		plus_n = 0;
		online = 0;
		result("OK");
	}
}

int main()
{
	return spandsp.run();
}
