/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_sms.cpp - fixed-line SMS (ETSI ES 201 912, protocol 1): short
 *                 messages between a telephone and a service centre
 *                 over an ordinary call (DP_SMS)
 *
 *    The line is V.23's forward channel both ways, half duplex: 1200
 *    bit/s FSK, 1300 Hz mark, 2100 Hz space.  A message is a pause
 *    (300 ms before the first, 25 ms before a reply), 80 bits of mark,
 *    its octets (each a start bit, 8 bits from the lowest, a stop bit),
 *    and 10 bits of mark; then the carrier stops.  Its octets are its
 *    type, its length, that many octets of payload and a checksum that
 *    makes them all add up to 0 (mod 256).  The types:
 *
 *    0x91 SMS_DATA   a TPDU (sms_pdu.cpp): SMS-SUBMIT or SMS-DELIVER
 *    0x92 SMS_ERROR  the last message was bad (1 checksum, 2 length, 3
 *                    type): it goes again
 *    0x93 SMS_EST    the answerer's first: send yours
 *    0x94 SMS_REL    no more: hang up
 *    0x95 SMS_ACK    the data taken (0, 0)
 *    0x96 SMS_NACK   the data refused (0, its cause, 0)
 *
 *    The answerer receives: SMS_EST first, then SMS_ACK for each
 *    SMS_DATA; each one's lines go to the DTE (+CMT: for a delivery,
 *    +SUBMIT: for a submission, sms_pdu.h).  SMS_REL ends the call.
 *
 *    The caller sends: after SMS_EST, each line the DTE types, the next
 *    after the last's SMS_ACK, and SMS_REL when the DTE has none for
 *    3 s.  A line is <number>,<text> (an SMS-SUBMIT to number, the
 *    telephone's message to its service centre), <<number>,<text> (an
 *    SMS-DELIVER from number, the service centre's to a telephone,
 *    stamped now) or :<hex> (a TPDU as it is).  Each gets OK back
 *    (SMS_ACK), +CMS ERROR: <cause> (SMS_NACK's cause), or ERROR (no
 *    message, or no answer to it three times).
 *
 *    Either side sends its last message again when nothing comes back
 *    within 3 s (the answerer 5 s; three times in all), then hangs up.  Neither listens
 *    while it sends.  The DTE's characters and the lines to it are the
 *    core's asynchronous link's (8N1, no error correction); the call
 *    is up (CONNECT 1200) from the answerer's start and the caller's
 *    SMS_EST.
 *
 */

#include <cinttypes>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"
#include "sms_pdu.h"

namespace dsp {

/* V.23's forward channel */
#define SMS_MARK         1300
#define SMS_SPACE        2100
#define SMS_BAUD         1200
/* a message's mark before and after its octets, the pause before it */
#define SMS_SYNC_BITS    80
#define SMS_TAIL_BITS    10
#define SMS_EST_PAUSE_MS 300
#define SMS_PAUSE_MS     25
/* the receiver deaf after our carrier stops */
#define SMS_DEAF_MS      20
/* nothing heard after our message: it again, three times in all (the
   answerer waits longer: the caller's DTE may still be typing) */
#define SMS_REPLY_MS     3000
#define SMS_WAIT_MS      5000
#define SMS_TRIES        3
/* the caller: no line from the DTE, SMS_REL */
#define SMS_REL_MS       3000
/* after SMS_REL, the time for its end to reach the far side */
#define SMS_END_MS       100
/* a message's octets: one late by 30 bits, the message is lost */
#define SMS_GAP_BITS     30
/* the DTE's messages waiting, a line's longest */
#define SMS_QUEUE        8
#define SMS_LINE         512
/* a message's octets at most (type, length, 255, checksum) */
#define SMS_MSG          258

/* the message types (protocol 1) */
enum sms_type {
	SMS_DATA  = 0x91,
	SMS_ERROR = 0x92,
	SMS_EST   = 0x93,
	SMS_REL   = 0x94,
	SMS_ACK   = 0x95,
	SMS_NACK  = 0x96,
};

/* SMS_ERROR's causes */
enum sms_cause {
	SMS_BAD_CHECKSUM = 1,
	SMS_BAD_LENGTH   = 2,
	SMS_BAD_TYPE     = 3,
};

enum sms_state {
	SMS_LISTEN,     /* for the far end's message */
	SMS_PAUSE,      /* the pause before ours */
	SMS_SEND,       /* ours going out */
	SMS_END,        /* SMS_REL sent or heard: the call over */
};

struct SmsPump : PumpBase<SmsPump, sms_state> {
	static constexpr const char *name = "fixed-line SMS (ES 201 912) pump";

	/* the line */
	FskBitTx tx;
	FskDemod dem;
	Uart uart;
	s32 deaf;                       /* samples the receiver still
					   ignores */
	/* ours: the last message (for SMS_ERROR or no reply), its bits */
	u8 omsg[SMS_MSG];
	s16 olen;
	u8 obits[SMS_SYNC_BITS + SMS_MSG * 10 + SMS_TAIL_BITS];
	s32 ob_n, ob_i;
	s32 pause;
	bool end_after;                 /* SMS_REL: the call over once it
					   is out */
	/* theirs */
	u8 imsg[SMS_MSG];
	s16 in_n;
	s32 since_byte;                 /* samples since the last octet */
	s32 quiet;                      /* samples since we heard or sent
					   anything */
	s16 tries;                      /* times our last went out */
	/* the caller */
	bool est;                       /* SMS_EST heard */
	bool waiting;                   /* for the reply to SMS_DATA */
	char line[SMS_LINE];
	s16 line_n;
	std::deque<std::vector<u8>> queue;
	u8 mr;                          /* the next TP-MR */
	bool connected;

	/* its name in the log */
	const char *tag() const { return "sms"; }
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
	void to_link(const std::string &s);
	s32 dte_char();
	void dte_line();
	void send(u8 type, const u8 *payload, s16 n, s32 pause_ms);
	void again();
	void error(u8 cause);
	void next();
	s16 got_message();
	s16 rx_sample(float x);
	float tx_sample();
};

/* the answerer starts with SMS_EST; the caller listens for it */
s16 SmsPump::start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		   s32)
{
	if (id != DP_SMS)
		return -1;
	pump_setup(m, srate_, caller_);
	dem.init(SMS_MARK, SMS_SPACE, SMS_BAUD, srate);
	uart.init(SMS_BAUD, 8, srate);
	state = SMS_LISTEN;
	if (!caller)
		send(SMS_EST, nullptr, 0, SMS_EST_PAUSE_MS);
	DSP_DBG("sms: create %s\n", caller ? "caller" : "answerer");
	return 0;
}

/* line s to the DTE, a character at a time in 8N1 */
void SmsPump::to_link(const std::string &s)
{
	for (char ch : s) {
		u8 bits[11];
		bits[0] = 1;
		bits[1] = 0;
		for (s16 i = 0; i < 8; i++)
			bits[2 + i] = (static_cast<u8>(ch) >> i) & 1;
		bits[10] = 1;
		modem_put_bits(modem, 1, bits, 11);
	}
}

/* the DTE's next character off the link's 8N1 bit stream (its idle
   marks one at a time), -1 for none */
s32 SmsPump::dte_char()
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

/* the caller: the DTE's line ended: its TPDU queued, or ERROR */
void SmsPump::dte_line()
{
	std::string s(line, static_cast<size_t>(line_n));
	std::vector<u8> pdu;
	bool ok;

	line_n = 0;
	if (s.empty())
		return;
	if (s[0] == ':') {
		ok = sms::from_hex(&pdu, s.substr(1)) && pdu.size() <= 255;
	} else {
		bool dlv = s[0] == '<';
		size_t comma = s.find(',');
		ok = comma != std::string::npos;
		if (ok) {
			std::string number = s.substr(dlv, comma - dlv);
			std::string text = s.substr(comma + 1);
			ok = dlv ? sms::deliver(&pdu, number, text, sms::now()) :
				sms::submit(&pdu, number, text, mr);
			if (ok && !dlv)
				mr++;
		}
	}
	if (!ok || queue.size() >= SMS_QUEUE) {
		to_link("ERROR\r\n");
		return;
	}
	queue.push_back(pdu);
}

/* a message of type with payload (n octets) out after pause_ms: our
   last from now on */
void SmsPump::send(u8 type, const u8 *payload, s16 n, s32 pause_ms)
{
	u8 sum = 0;

	omsg[0] = type;
	omsg[1] = static_cast<u8>(n);
	if (n)
		memcpy(omsg + 2, payload, static_cast<size_t>(n));
	olen = static_cast<s16>(n + 3);
	for (s16 i = 0; i < olen - 1; i++)
		sum = static_cast<u8>(sum + omsg[i]);
	omsg[olen - 1] = static_cast<u8>(-sum);
	tries = 0;
	end_after = type == SMS_REL;
	pause = ms(pause_ms);
	again();
}

/* our last message out again (its pause first) */
void SmsPump::again()
{
	ob_n = 0;
	for (s16 i = 0; i < SMS_SYNC_BITS; i++)
		obits[ob_n++] = 1;
	for (s16 k = 0; k < olen; k++) {
		obits[ob_n++] = 0;
		for (s16 i = 0; i < 8; i++)
			obits[ob_n++] = (omsg[k] >> i) & 1;
		obits[ob_n++] = 1;
	}
	for (s16 i = 0; i < SMS_TAIL_BITS; i++)
		obits[ob_n++] = 1;
	ob_i = 0;
	tries++;
	tx.init(SMS_MARK, SMS_SPACE, DSP_TX_AMPLITUDE, SMS_BAUD, srate, 1);
	in_n = 0;
	set_state(SMS_PAUSE);
}

/* SMS_ERROR with cause out */
void SmsPump::error(u8 cause)
{
	DSP_DBG("sms: error %d\n", cause);
	send(SMS_ERROR, &cause, 1, SMS_PAUSE_MS);
}

/* the caller: the next message, SMS_REL once the DTE has none for a
   while (sample() sees to that) */
void SmsPump::next()
{
	waiting = false;
	if (queue.empty())
		return;
	const std::vector<u8> &pdu = queue.front();
	waiting = true;
	send(SMS_DATA, pdu.data(), static_cast<s16>(pdu.size()),
	     SMS_PAUSE_MS);
}

/* a whole message in imsg: answered; DPSTAT_ERROR to end the call */
s16 SmsPump::got_message()
{
	u8 type = imsg[0], sum = 0;

	for (s16 i = 0; i < in_n; i++)
		sum = static_cast<u8>(sum + imsg[i]);
	in_n = 0;
	if (sum) {
		error(SMS_BAD_CHECKSUM);
		return DPSTAT_OK;
	}
	DSP_DBG("sms: message 0x%02x, %d octets\n", type, imsg[1]);
	switch (type) {
	case SMS_REL:
		set_state(SMS_END);
		return DPSTAT_ERROR;
	case SMS_ERROR:
		again();
		return DPSTAT_OK;
	default:
		break;
	}
	if (!caller) {
		if (type != SMS_DATA) {
			error(SMS_BAD_TYPE);
			return DPSTAT_OK;
		}
		to_link(sms::to_dte(imsg + 2, imsg[1]));
		static const u8 ack[2] = { 0, 0 };
		send(SMS_ACK, ack, 2, SMS_PAUSE_MS);
		return DPSTAT_OK;
	}
	switch (type) {
	case SMS_EST:
		/* (again, if it missed our first: ours again) */
		if (est && waiting) {
			again();
			return DPSTAT_OK;
		}
		est = true;
		next();
		return DPSTAT_OK;
	case SMS_ACK:
	case SMS_NACK:
		if (!waiting)
			return DPSTAT_OK;
		if (type == SMS_ACK)
			to_link("OK\r\n");
		else
			to_link("+CMS ERROR: " +
				std::to_string(imsg[1] >= 2 ? imsg[3] : 255) +
				"\r\n");
		queue.pop_front();
		next();
		return DPSTAT_OK;
	default:
		error(SMS_BAD_TYPE);
		return DPSTAT_OK;
	}
}

/* the receiver's sample x: its octets into imsg, a whole message
   answered; DPSTAT_ERROR to end the call */
s16 SmsPump::rx_sample(float x)
{
	dem.run(x);
	bool on = dem.carrier && deaf <= 0;
	if (deaf > 0)
		deaf--;
	if (on)
		quiet = 0;
	s32 c = uart.run(dem.last_dec, on);
	since_byte++;
	if (in_n && (!on || since_byte > SMS_GAP_BITS * srate / SMS_BAUD)) {
		DSP_DBG("sms: %d octets, then none\n", in_n);
		if (in_n >= 2 && in_n < imsg[1] + 3)
			error(SMS_BAD_LENGTH);
		in_n = 0;
		return DPSTAT_OK;
	}
	if (c < 0)
		return DPSTAT_OK;
	since_byte = 0;
	imsg[in_n++] = static_cast<u8>(c);
	if (in_n >= 2 && in_n == imsg[1] + 3)
		return got_message();
	return DPSTAT_OK;
}

/* our message's next sample (silent in its pause) */
float SmsPump::tx_sample()
{
	if (state == SMS_PAUSE) {
		if (count < pause)
			return 0;
		set_state(SMS_SEND);
	}
	float y = tx.sample([this]() -> s16 {
		return ob_i < ob_n ? obits[ob_i++] : -1;
	});
	if (!tx.ended)
		return y;
	deaf = ms(SMS_DEAF_MS);
	quiet = 0;
	in_n = 0;
	set_state(end_after ? SMS_END : SMS_LISTEN);
	return 0;
}

/* see pump.h */
s16 SmsPump::sample(float x, float *y)
{
	s32 c;
	s16 r = DPSTAT_OK;

	carrier_tick(1);
	/* the caller's DTE types its messages; the answerer's is
	   dropped */
	while ((c = dte_char()) >= 0) {
		if (!caller)
			continue;
		if (c == '\r' || c == '\n')
			dte_line();
		else if (line_n < SMS_LINE)
			line[line_n++] = static_cast<char>(c);
	}
	*y = 0;
	switch (state) {
	case SMS_PAUSE:
	case SMS_SEND:
		*y = tx_sample();
		break;
	case SMS_END:
		if (count >= ms(SMS_END_MS))
			return DPSTAT_ERROR;
		break;
	case SMS_LISTEN:
		quiet++;
		r = rx_sample(x);
		if (r != DPSTAT_OK || state != SMS_LISTEN)
			break;
		if (in_n)
			break;
		/* the caller: a line waiting, or none for long: SMS_REL */
		if (caller && est && !waiting) {
			if (!queue.empty())
				next();
			else if (quiet >= ms(SMS_REL_MS))
				send(SMS_REL, nullptr, 0, 0);
			break;
		}
		if (caller && !est)
			break;
		if (quiet >= ms(caller ? SMS_REPLY_MS : SMS_WAIT_MS)) {
			if (tries >= SMS_TRIES) {
				DSP_DBG("sms: no reply\n");
				if (caller)
					to_link("ERROR\r\n");
				return DPSTAT_ERROR;
			}
			DSP_DBG("sms: no reply (%d)\n", tries);
			again();
		}
		break;
	}
	if (!connected && (!caller || est)) {
		connected = true;
		return connect_at(SMS_BAUD, SMS_BAUD);
	}
	return r;
}

/* registers the pump as DP_SMS; 0 */
s16 dp_sms_init(void)
{
	modem_dp_register(DP_SMS, &PumpOps<SmsPump>::ops);
	return 0;
}

/* deregisters it */
void dp_sms_exit(void)
{
	modem_dp_deregister(DP_SMS, &PumpOps<SmsPump>::ops);
}

} // namespace dsp
