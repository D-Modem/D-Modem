/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dsp_glue.cpp - entry points used by the modem core: data pump
 *                 registration, DC removal, the shared runtime object,
 *                 caller ID, voice and fax objects.
 *
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "dm_types.h"
#include "dsp.h"
#include "env.h"
#include <dsplibs.h>
#include "voice_codec.h"

using namespace dsp;

/* slmodemd's own pumps without a modulation (dp_dummy.cpp, dp_sinus.cpp) */
extern s16  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern s16  dp_sinus_init(void);
extern void dp_sinus_exit(void);


/*
 *    data pumps
 */

/* registers all the open data pumps with the core, and the dummy and
   sinus test pumps; always 0 */
int prop_dp_init(void)
{
	dp_dummy_init();
	dp_sinus_init();
	dp_callprog_init();
	dp_fsk_init();
	dp_text_init();
	dp_contactid_init();
	dp_sms_init();
	dp_relay_init();
	dp_v8bis_init();
	dp_v22_init();
	dp_v32_init();
	dp_v26_init();
	dp_v61_init();
	dp_v34_init();
	dp_vcvm_init();
	dp_vwb_init();
	dp_4wire_init();
	dp_clear_init();
	return 0;
}

/* deregisters them */
void prop_dp_exit(void)
{
	dp_dummy_exit();
	dp_sinus_exit();
	dp_callprog_exit();
	dp_fsk_exit();
	dp_text_exit();
	dp_contactid_exit();
	dp_sms_exit();
	dp_relay_exit();
	dp_v8bis_exit();
	dp_v22_exit();
	dp_v32_exit();
	dp_v26_exit();
	dp_v61_exit();
	dp_v34_exit();
	dp_vcvm_exit();
	dp_vwb_exit();
	dp_4wire_exit();
	dp_clear_exit();
}


/*
 *    DC removal, the same algorithm as the original dcr_* code:
 *    skip the first 0.6 s, average the next second for the initial
 *    estimate, then refresh it every 2 s with a 0.9/0.1 smoothing.
 */

#define DCR_ENABLE   0x1
#define DCR_SUBTRACT 0x2
#define DCR_TRACK    0x4

struct DcRemover {
	u16 flags;
	s16 state;
	s16 dc;
	s16 threshold;
	s32 sum;
	s32 count;
	s32 skip_len, init_len, update_len;

	void reset();
	s16 process(void *buf, s32 len);
};

/* a DC remover with the original's settings (the lengths in samples at
   9600 Hz: 0.6 s skipped, 1 s initial average, 2 s updates; 3000 the
   level reported as too much DC); NULL when out of memory */
void *dcr_create(void)
{
	DcRemover *d = new (std::nothrow) DcRemover();
	if (!d)
		return nullptr;
	d->flags = DCR_ENABLE | DCR_SUBTRACT | DCR_TRACK;
	d->state = 0;
	d->skip_len = 5760;
	d->init_len = 9600;
	d->update_len = 19200;
	d->threshold = 3000;
	return d;
}

/* frees a DC remover */
void dcr_delete(void *obj)
{
	delete static_cast<DcRemover*>(obj);
}

/* forgets the estimate and the running sum (the state is kept) */
void DcRemover::reset()
{
	dc = 0;
	sum = 0;
	count = 0;
}

/* DcRemover::reset() for the C interface (not in dsplibs.h) */
void dcr_reset(void *obj)
{
	static_cast<DcRemover*>(obj)->reset();
}

/* len s16 samples in place: steps the estimator (skip, initial average,
   then, while tracking, updates that ignore all zero buffers) and
   subtracts the estimate; returns 1 when |dc| >= threshold, else 0 */
s16 DcRemover::process(void *buf, s32 len)
{
	s16 *s = static_cast<s16*>(buf);
	s16 ret = 0;
	s32 i;

	if (!(flags & DCR_ENABLE))
		return 0;
	if (state >= 2) {
		/* ignore muted buffers once running */
		for (i = 0; i < len && !s[i]; i++)
			;
		if (i == len)
			return 0;
	}
	switch (state) {
	case 0:
		count += len;
		if (count >= skip_len) {
			state = 1;
			count = 0;
			sum = 0;
		}
		break;
	case 1:
		for (i = 0; i < len; i++)
			sum += s[i];
		count += len;
		if (count >= init_len) {
			dc = sum / count;
			DSP_DBG("dcr: dc %d\n", dc);
			state = (flags & DCR_TRACK) ? 2 : 3;
			sum = 0;
			count = 0;
		}
		break;
	case 2:
		for (i = 0; i < len; i++)
			sum += s[i];
		count += len;
		if (count >= update_len) {
			dc = (s32{dc} * 0x7333 + (sum / count) * 0xccd +
				 0x4000) >> 15;
			sum = 0;
			count = 0;
		}
		break;
	default:
		break;
	}
	if (abs(dc) >= threshold)
		ret = 1;
	if (flags & DCR_SUBTRACT && dc)
		for (i = 0; i < len; i++)
			s[i] = dsp_clip16(static_cast<float>(s[i]) - dc);
	return ret;
}

/* DcRemover::process(), 0 without a remover */
int dcr_process(void *obj, void *buf, int len)
{
	if (!obj)
		return 0;
	return static_cast<DcRemover*>(obj)->process(buf, len);
}


/*
 *    runtime object shared by the data pumps of one call
 */

/* a zeroed runtime for modem m, NULL when out of memory */
void *dp_runtime_create(struct modem *m)
{
	struct dp_runtime *rt = new (std::nothrow) dp_runtime();
	if (rt)
		rt->modem = m;
	return rt;
}

/* the G.711 law of the network (dcore::Modem::set_line_law()) */
void dp_runtime_set_law(void *runtime, int law)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(runtime);
	if (rt)
		rt->law = law;
}

/* see dsplibs.h */
void dp_runtime_set_analog(void *runtime, int analog)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(runtime);
	if (rt)
		rt->analog = analog;
}

/* see dsplibs.h */
void dp_runtime_set_v110(void *runtime, long rate)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(runtime);
	if (rt) {
		rt->v110 = static_cast<s32>(rate);
		rt->v110_rate = 0;
	}
}

/* see dsplibs.h */
long dp_runtime_v110(void *runtime)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(runtime);
	return rt ? rt->v110_rate : 0;
}

/* 1 while a pump takes the exact PCM codes (no DC removal), else 0 */
int dp_runtime_pcm(void *runtime)
{
	struct dp_runtime *rt = static_cast<dp_runtime*>(runtime);
	return rt ? rt->pcm : 0;
}

/* frees the runtime */
void dp_runtime_delete(void *runtime)
{
	delete static_cast<struct dp_runtime *>(runtime);
}


/*
 *    Caller ID, from whichever of these the line has:
 *
 *    - FSK at 1200 bit/s, Bell 202's (mark 1200 Hz, space 2200 Hz:
 *      GR-30, North America) or V.23's (mark 1300 Hz, space 2100 Hz:
 *      ETSI EN 300 659-1 and BT SIN 227, Europe and the UK), each on a
 *      demodulator of its own, async 8N1 after a run of marks: an SDMF
 *      (0x04) or MDMF (0x80, ETSI's Call Set-up) message, its checksum
 *      checked.  The parameters of both are the same: 0x01 date and
 *      time, 0x02 number, 0x04 its absence, 0x07 name, 0x08 its absence.
 *    - DTMF (ETSI TS 101 109's, Denmark, the Netherlands, Sweden,
 *      Finland, Brazil, India...): A then the number, or B (some send D)
 *      then a two digit reason the number is absent (00 not available,
 *      10 private), ended with C or # or by a pause; made an MDMF message
 *      as the FSK ones are.
 *
 *    cid_val 1 reports the formatted fields, other values the message in
 *    hex; in voice mode (packet) inside <DLE>X and <DLE>. (V.253 7.1.2,
 *    its example 3), else after an empty line.
 */

#define CID_MAX_MSG 258

/* one FSK caller ID channel: its demodulator, its UART, its message */
struct CidFsk {
	FskDemod fsk;
	s16 in_char, nbits;
	u16 shift;
	s32 marks;
	u8 msg[CID_MAX_MSG];
	s16 len;

	/* mark and space in Hz at rate */
	void init(float mark, float space, s32 rate)
	{
		fsk.init(mark, space, 1200, rate);
	}
	s16 byte(u8 b);
	s16 sample(float x);
};

struct CallerId {
	struct modem *modem;
	u16 format;
	s16 packet;
	CidFsk bell, v23;
	/* DTMF: the detector, what came so far ('A' or 'B' first, 0 before
	   either), the samples since the last digit went */
	DtmfDetect dtmf;
	char dtmf_got[40];
	s16 dtmf_n;
	s32 dtmf_quiet, dtmf_pause;
	/* the message reported */
	u8 msg[CID_MAX_MSG];
	s16 len;

	void send(const char *s);
	void field(const char *tag, const u8 *p, s16 n);
	void report();
	void report_fields();
	s16 dtmf_digit(char d);
	s16 dtmf_done();
	s16 process(void *in, s32 count);
};

/* a caller ID receiver at rate Hz, cid_val the report format (1:
   formatted fields), packet the voice mode's; NULL when out of memory */
void *CID_create(struct modem *m, u32 rate, unsigned cid_val,
		 int packet)
{
	CallerId *c = new (std::nothrow) CallerId();
	if (!c)
		return nullptr;
	c->modem = m;
	c->format = cid_val;
	c->packet = packet;
	c->bell.init(1200, 2200, rate);
	c->v23.init(1300, 2100, rate);
	c->dtmf.init(rate);
	/* the end of DTMF digits without an end code: 0.5 s without one */
	c->dtmf_pause = static_cast<s32>(rate) / 2;
	DSP_DBG("cid: create, format %u\n", cid_val);
	return c;
}

/* frees a caller ID receiver */
void CID_delete(void *obj)
{
	delete static_cast<CallerId*>(obj);
}

/* the string s to the DTE */
void CallerId::send(const char *s)
{
	modem_send_to_tty(modem, s, strlen(s));
}

/* "tag = value\r\n" to the DTE, value the n bytes at p, unprintable
   ones as '?', cut to fit 128 bytes */
void CallerId::field(const char *tag, const u8 *p, s16 n)
{
	char buf[128];
	s16 k = snprintf(buf, sizeof(buf), "%s = ", tag);
	while (n-- > 0 && k < static_cast<s16>(sizeof(buf)) - 3)
		buf[k++] = (*p >= 0x20 && *p < 0x7f) ? *p : '?', p++;
	buf[k++] = '\r';
	buf[k++] = '\n';
	buf[k] = 0;
	send(buf);
}

/* the complete message in msg to the DTE, after an empty line or (in
   voice mode) as a packet */
void CallerId::report()
{
	static const char head[] = { 0x10, 'X', 0 }, end[] = { 0x10, '.', 0 };

	send(packet ? head : "\r\n");
	report_fields();
	if (packet)
		send(end);
}

/* all of the message in hex (MESG) for format != 1, else DATE, TIME,
   NMBR and NAME from an SDMF message or the MDMF parameters (0x01 date
   and time, 0x02 number, 0x04 number absent, 0x07 name, 0x08 name
   absent) */
void CallerId::report_fields()
{
	const u8 *p = msg + 2;
	s16 n = msg[1];
	char buf[8];

	if (format != 1) {
		s16 i;
		send("MESG = ");
		for (i = 0; i < len; i++) {
			snprintf(buf, sizeof(buf), "%02X", msg[i]);
			send(buf);
		}
		send("\r\n");
		return;
	}
	if (msg[0] == 0x04) {        /* SDMF: MMDDHHMM number */
		if (n >= 8) {
			field("DATE", p, 4);
			field("TIME", p + 4, 4);
			field("NMBR", p + 8, n - 8);
		}
		return;
	}
	while (n >= 2) {                /* MDMF parameters */
		s16 t = p[0], l = p[1];
		if (l + 2 > n)
			break;
		switch (t) {
		case 0x01:
			if (l >= 8) {
				field("DATE", p + 2, 4);
				field("TIME", p + 6, 4);
			}
			break;
		case 0x02: field("NMBR", p + 2, l); break;
		/* the reason there is none: 'O' (out of area) or 'P'
		   (private) */
		case 0x04:
			field("NMBR",
			      l >= 1 ? p + 2 :
				       reinterpret_cast<const u8 *>("O"),
			      1);
			break;
		case 0x07: field("NAME", p + 2, l); break;
		case 0x08:
			field("NAME",
			      l >= 1 ? p + 2 :
				       reinterpret_cast<const u8 *>("O"),
			      1);
			break;
		default: break;
		}
		p += l + 2;
		n -= l + 2;
	}
}

/* a message octet: 0 to continue, 1 when msg is a whole message with
   a good checksum, -1 for a bad one or an overflow (the message
   dropped, the channel ready for the next) */
s16 CidFsk::byte(u8 b)
{
	u8 sum = 0;

	if (len == 0 && b != 0x04 && b != 0x80)
		return 0;
	msg[len++] = b;
	if (len < 2 || len < msg[1] + 3)
		return len >= CID_MAX_MSG ? (len = 0, -1) : 0;
	for (s16 i = 0; i < len; i++)
		sum += msg[i];
	if (sum) {
		DSP_DBG("cid: checksum error\n");
		len = 0;
		return -1;
	}
	return 1;
}

/* one sample through the demodulator and an async 8N1 receiver (a start
   bit only after 10 marks before the first octet, octets with framing
   errors dropped): as byte() */
s16 CidFsk::sample(float x)
{
	s16 bit = fsk.run(x);

	if (bit < 0 || !fsk.carrier)
		return 0;
	if (!in_char) {
		/* start bit after the mark preamble */
		if (bit) {
			marks++;
			return 0;
		}
		if (marks < 10 && len == 0)
			return 0;
		in_char = 1;
		nbits = 0;
		shift = 0;
		return 0;
	}
	if (nbits < 8) {
		shift |= bit << nbits;
		nbits++;
		return 0;
	}
	/* stop bit */
	in_char = 0;
	marks = 0;
	if (!bit)
		return 0;       /* framing error */
	return byte(static_cast<u8>(shift));
}

/* a DTMF digit of the caller ID: A or B (or D) starts it, C or # ends
   it; 1 once it is reported, else 0 */
s16 CallerId::dtmf_digit(char d)
{
	if (d == 'A' || d == 'B' || d == 'D') {
		dtmf_got[0] = d == 'A' ? 'A' : 'B';
		dtmf_n = 1;
		return 0;
	}
	if (!dtmf_n)
		return 0;
	if (d == 'C' || d == '#')
		return dtmf_done();
	if (d >= '0' && d <= '9' &&
	    dtmf_n < static_cast<s16>(sizeof(dtmf_got)))
		dtmf_got[dtmf_n++] = d;
	return 0;
}

/* the DTMF digits so far as an MDMF message, reported: the number, or
   for B the reason it is absent (10: 'P' private, else 'O'); 1 if there
   was one, else 0 */
s16 CallerId::dtmf_done()
{
	s16 n = dtmf_n - 1;
	const char *what = dtmf_got + 1;
	u8 sum = 0;

	DSP_DBG("cid: DTMF %.*s\n", dtmf_n, dtmf_got);
	dtmf_n = 0;
	if (n <= 0)
		return 0;
	len = 0;
	msg[len++] = 0x80;
	msg[len++] = 0;
	if (dtmf_got[0] == 'A') {
		msg[len++] = 0x02;
		msg[len++] = static_cast<u8>(n);
		std::memcpy(msg + len, what, n);
		len += n;
	} else {
		msg[len++] = 0x04;
		msg[len++] = 1;
		msg[len++] = n >= 2 && what[0] == '1' && what[1] == '0' ?
			'P' : 'O';
	}
	msg[1] = static_cast<u8>(len - 2);
	for (s16 i = 0; i < len; i++)
		sum += msg[i];
	msg[len++] = static_cast<u8>(-sum);
	report();
	return 1;
}

/* count s16 samples through the receivers: 1 when a message was
   reported, else 0 (a channel's bad message is dropped: the other may
   have the good one) */
s16 CallerId::process(void *in, s32 count)
{
	s16 *s = static_cast<s16*>(in);

	for (s32 i = 0; i < count; i++) {
		CidFsk *got = nullptr;
		for (CidFsk *c : { &bell, &v23 })
			if (c->sample(s[i]) > 0)
				got = c;
		if (got) {
			std::memcpy(msg, got->msg, got->len);
			len = got->len;
			report();
			return 1;
		}
		switch (dtmf.sample(s[i])) {
		case DtmfDetect::ON:
			if (dtmf_digit(dtmf.digit))
				return 1;
			dtmf_quiet = -1;
			break;
		case DtmfDetect::OFF:
			dtmf_quiet = 0;
			break;
		default:
			if (dtmf_n && dtmf_quiet >= 0 &&
			    ++dtmf_quiet >= dtmf_pause && dtmf_done())
				return 1;
			break;
		}
	}
	return 0;
}

/* see dsplibs.h: the message as if the receiver had it */
int CID_report(struct modem *m, unsigned cid_val, int packet,
	       const void *msg, int len)
{
	const u8 *b = static_cast<const u8 *>(msg);

	if (len < 3 || len > CID_MAX_MSG || b[1] + 3 != len ||
	    (b[0] != 0x04 && b[0] != 0x80))
		return -1;
	CallerId *c = static_cast<CallerId *>(CID_create(m, 8000, cid_val,
							 packet));
	if (!c)
		return -1;
	std::memcpy(c->msg, b, len);
	c->len = static_cast<s16>(len);
	c->report();
	CID_delete(c);
	return 0;
}

/* CallerId::process() for the C interface */
int CID_process(void *obj, void *in, int count)
{
	return static_cast<CallerId*>(obj)->process(in, count);
}


/*
 *    Voice: IS-101 style streaming of samples in +VSM's format and rate
 *    (voice_codec.h) with <DLE> shielding, tone (+VTS) generation.
 */

#define DLE 0x10
#define ETX 0x03
#define CAN 0x18
#define DC4 0x14   /* <DLE><DC4> could be sent by the DTE as well */

struct Voice {
	struct modem *modem;
	s32 srate;
	u16 state;
	s16 pending;            /* status to return from VOICE_process */
	/* the DTE's format both ways, its rate (+VSM) */
	voice::Codec tx_codec, rx_codec;
	u32 vsr;
	/* resampling between srate and vsr: the DTE's samples, the next
	   decoded ones waiting; the line's last sample and where the next
	   DTE sample falls after it, in line samples */
	double tx_pos;
	float tx_prev, tx_cur;
	s16 tx_more[2], tx_nmore, tx_next;
	double rx_t;
	float rx_prev;
	/* this process()'s characters to the DTE */
	std::string rxbuf;
	s16 dle;
	s16 tx_end;
	/* tones */
	s32 tone_left;
	Osc t1, t2;
	s16 tone1, tone2;      /* each sounds (its frequency is not 0) */
	DtmfGen dtmf;
	s16 tone_dtmf;
	/* DTMF on the line: the detector, the samples since the digit on
	   was last reported, whether it is held off (our own tone plays) */
	DtmfDetect rx_dtmf;
	s32 dtmf_since;
	s16 dtmf_held;
	/* the call progress tones on the line, the rings and busy
	   cadences reported */
	CallToneDetect rx_cpt;
	u16 rings_told, busy_told, answer_told[2];
	/* the calling tones (a fax machine's CNG, V.25's CT), each
	   reported 4 s apart at least (V.253 7.3, events 6 and 7): the
	   bursts reported, the samples since the last report */
	CngDetect rx_ct[2];
	u16 ct_told[2];
	s32 ct_since[2];
	/* Type II caller ID (GR-30's, a call waiting's caller on the call):
	   the CPE alerting signal (2130 + 2750 Hz, 80 ms) found in blocks of
	   10 ms, the blocks of it in a row; then our DTMF A for 60 ms, the
	   samples of it left; then the caller ID receiver for 1 s, the
	   samples left, the line's audio to the DTE silent meanwhile */
	Goertzel cas[2];
	float cas_energy;
	s16 cas_blocks;
	DtmfGen ack;
	s32 ack_left;
	CallerId *cid2;
	s32 cid2_left;

	struct voice_info *info();
	void set_format();
	s16 get_tx_sample(float *x);
	void tx_done();
	s16 command(enum VOICE_CMD cmd);
	float type2(float x);
	s16 process(void *in, void *out, s32 count);
};

/* a voice object at srate Hz in command state, its OK (the line is
   taken: AT+VLS=1's result, which the core leaves to it) due from the
   first process(); NULL when out of memory */
void *VOICE_create(struct modem *m, u32 srate)
{
	Voice *v = new (std::nothrow) Voice();
	if (!v)
		return nullptr;
	v->modem = m;
	v->srate = srate;
	v->state = VOICE_STATE_COMMAND;
	v->pending = VOICE_STATUS_OK;
	v->set_format();
	v->rx_dtmf.init(srate);
	v->rx_cpt.init(srate);
	v->cas[0].init(2130, static_cast<s16>(srate / 100), srate);
	v->cas[1].init(2750, static_cast<s16>(srate / 100), srate);
	for (s16 k = 0; k < 2; k++) {
		v->rx_ct[k].init(srate, k ? 1300 : 1100);
		v->ct_since[k] = 4L * srate;
	}
	DSP_DBG("voice: VOICE_create...\n");
	return v;
}

/* frees a voice object */
void VOICE_delete(void *obj)
{
	DSP_DBG("voice: VOICE_delete...\n");
	Voice *v = static_cast<Voice*>(obj);
	if (v)
		delete v->cid2;
	delete v;
}

/* the core's voice settings (MDMPRM_VOICEINFO), may be NULL */
struct voice_info *Voice::info()
{
	return reinterpret_cast<struct voice_info *>(
		modem_get_param(modem, MDMPRM_VOICEINFO));
}

/* a +V command: receive, transmit or duplex streaming (CONNECT on the
   next process()), a beep or a DTMF tone of tone_duration * 10 ms (OK
   when it is over), abort or back to command state (OK); 0, or -1 for an
   unknown command or a tone without voice settings */
s16 Voice::command(enum VOICE_CMD cmd)
{
	struct voice_info *vi = info();

	DSP_DBG("voice: command %d\n", cmd);
	switch (cmd) {
	case VOICE_CMD_STATE_RX:
	case VOICE_CMD_STATE_TX:
	case VOICE_CMD_STATE_DUPLEX:
		state = cmd == VOICE_CMD_STATE_RX ? VOICE_STATE_RX :
			cmd == VOICE_CMD_STATE_TX ? VOICE_STATE_TX :
			VOICE_STATE_DUPLEX;
		dle = 0;
		tx_end = 0;
		set_format();
		pending = VOICE_STATUS_CONNECT;
		return 0;
	case VOICE_CMD_BEEP:
	case VOICE_CMD_DTMF:
		if (!vi)
			return -1;
		tone_left = static_cast<long>(vi->tone_duration) * srate / 100;
		tone_dtmf = cmd == VOICE_CMD_DTMF;
		if (tone_dtmf) {
			if (dtmf.set(vi->dtmf_symbol, srate))
				tone_left = 0;
		} else {
			t1.set_freq(vi->tone1_freq, srate);
			t2.set_freq(vi->tone2_freq, srate);
			tone1 = vi->tone1_freq != 0;
			tone2 = vi->tone2_freq != 0;
		}
		if (!tone_left)
			pending = VOICE_STATUS_OK;
		return 0;
	case VOICE_CMD_ABORT:
	case VOICE_CMD_STATE_COMMAND:
		state = VOICE_STATE_COMMAND;
		tone_left = 0;
		pending = VOICE_STATUS_OK;
		return 0;
	default:
		return -1;
	}
}

/* Voice::command() for the C interface */
int VOICE_command(void *obj, enum VOICE_CMD cmd)
{
	return static_cast<Voice*>(obj)->command(cmd);
}

/* the DTE's format and rate as +VSM has them (8 bit unsigned at 8000
   if it has none the engine takes), its coders and resamplers from the
   start */
void Voice::set_format()
{
	struct voice_info *vi = info();
	const voice::Format *f = vi ? voice::format(vi->comp_method) : nullptr;
	voice::Coding c = f ? f->coding : voice::UNSIGNED8;

	vsr = f && voice::runs_at(vi->comp_method, vi->sample_rate) ?
		vi->sample_rate : 8000;
	tx_codec.reset(c);
	rx_codec.reset(c);
	tx_pos = 0;
	tx_prev = tx_cur = 0;
	tx_nmore = tx_next = 0;
	rx_t = 1;
	rx_prev = 0;
}

/* the next sample from the DTE, <DLE> codes taken out (<DLE><DLE> a
   0x10 octet; in transmit state <DLE><ETX>, <DLE>! and <DLE><CAN> end
   the transmission, in duplex state <DLE><^> does and the others are
   ignored (V.253 10.1.4.1): tx_end), decoded from +VSM's format as a
   16 bit level in *x; returns 1 for a sample, 0 when there is none (yet
   or any more) */
s16 Voice::get_tx_sample(float *x)
{
	char ch;

	if (tx_next < tx_nmore) {
		*x = tx_more[tx_next++];
		return 1;
	}
	while (!tx_end && modem_recv_from_tty(modem, &ch, 1) == 1) {
		u8 c = ch;
		if (dle) {
			dle = 0;
			if (c != DLE) {
				if (state == VOICE_STATE_DUPLEX ? c == '^' :
				    c == ETX || c == '!' || c == CAN) {
					tx_end = 1;
					break;
				}
				continue;
			}
		} else if (c == DLE) {
			dle = 1;
			continue;
		}
		tx_nmore = static_cast<s16>(tx_codec.decode(c, tx_more));
		tx_next = 0;
		if (tx_nmore) {
			*x = tx_more[tx_next++];
			return 1;
		}
	}
	return 0;
}

/* the DTE's stream over: transmit state back to command state; duplex
   state too, its samples to the DTE ended with <DLE><ETX> (after those
   in rxbuf so far); OK due */
void Voice::tx_done()
{
	DSP_DBG("voice: the DTE's stream ends\n");
	if (state == VOICE_STATE_DUPLEX)
		rxbuf += "\x10\x03";
	state = VOICE_STATE_COMMAND;
	pending = VOICE_STATUS_OK;
}

/* Type II caller ID on the line's sample x (with #CID on): the
   alerting signal, 60 ms of it or more, then its end, start our
   acknowledgement (DTMF A, GR-30's for a CPE without ADSI), then the
   caller ID receiver (its report a <DLE>X packet).  Returns our
   acknowledgement's sample, 0 when none. */
float Voice::type2(float x)
{
	struct voice_info *vi = info();
	float y = 0;

	if (ack_left > 0) {
		y = ack.next();
		if (--ack_left == 0) {
			cid2 = static_cast<CallerId *>(
				CID_create(modem, srate, vi ? vi->cid_format : 1,
					   1));
			cid2_left = srate;
		}
		return y;
	}
	if (cid2) {
		s16 s = dsp_clip16(x);
		if (cid2->process(&s, 1) || --cid2_left <= 0) {
			DSP_DBG("voice: Type II caller ID over\n");
			delete cid2;
			cid2 = nullptr;
		}
		return 0;
	}
	if (!vi || !vi->cid_format)
		return 0;
	cas_energy += x * x;
	cas[1].run(x);
	if (!cas[0].run(x))
		return 0;
	float ms = cas_energy / cas[0].len;
	cas_energy = 0;
	if (ms > 30000.0f && cas[0].power + cas[1].power > 0.6f * ms &&
	    cas[0].power > 0.2f * ms && cas[1].power > 0.2f * ms) {
		cas_blocks++;
		return 0;
	}
	if (cas_blocks >= 6) {
		DSP_DBG("voice: CPE alerting signal, Type II caller ID\n");
		ack.set('A', srate);
		ack_left = srate * 60 / 1000;
	}
	cas_blocks = 0;
	return 0;
}

/* count samples each way at srate: out a pending tone or the DTE's
   samples (+VSM's, linearly interpolated from its rate), in sent to the
   DTE in +VSM's format at its rate (<DLE> doubled); in receive only state any character
   from the DTE ends it with <DLE><ETX>.  DTMF digits on the line are
   reported in every state (V.253 7.5): <DLE><digit> when one comes on
   and every 70 ms while it lasts, <DLE><~> when it goes, among the
   samples in receive (or duplex) state, whose audio is silence from the report on
   (7.6, its method 2: 51 ms of the tone are left); not while a tone of
   ours plays.  So are a ringing tone's rings (<DLE>r, as one comes on),
   a busy tone's cadences (<DLE>b) and answer tones (<DLE>a for 2100 Hz,
   <DLE>f for 2225 Hz), which the line is quiet for while a tone of ours
   plays, and calling tones: a fax machine's CNG (<DLE>c) and V.25's
   1300 Hz (<DLE>e), each burst once it ends, 4 s apart at least.
   Returns the VOICE_STATUS_* due, or 0. */
s16 Voice::process(void *in, void *out, s32 count)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	const double step = static_cast<double>(vsr) / srate;
	const double rx_step = static_cast<double>(srate) / vsr;
	s16 status;
	s32 i;
	const long repeat = static_cast<long>(srate) * 70 / 1000;
	auto report = [&](char c) {
		rxbuf += static_cast<char>(DLE);
		rxbuf += c;
	};
	/* a sample to the DTE in its format, <DLE> doubled */
	auto to_dte = [&](float y) {
		u8 b[2];
		int n = rx_codec.encode(dsp_clip16(y), b);
		for (int k = 0; k < n; k++) {
			rxbuf += static_cast<char>(b[k]);
			if (b[k] == DLE)
				rxbuf += static_cast<char>(DLE);
		}
	};

	rxbuf.clear();

	for (i = 0; i < count; i++) {
		float y = 0;
		s16 x_in = src[i];

		if (tone_left > 0) {
			/* a frequency of 0 is no tone: [0,0,d] is silence */
			if (tone_dtmf)
				y = dtmf.next();
			else if (tone1 && tone2)
				y = 0.5f * DSP_TX_AMPLITUDE *
					(t1.next() + t2.next());
			else if (tone1 || tone2)
				y = DSP_TX_AMPLITUDE *
					(tone1 ? t1.next() : t2.next());
			if (--tone_left == 0)
				pending = VOICE_STATUS_OK;
		} else if (state & VOICE_STATE_TX && !tx_end) {
			/* linear interpolation from vsr */
			tx_pos += step;
			while (tx_pos >= 1) {
				float x;
				tx_pos -= 1;
				tx_prev = tx_cur;
				if (get_tx_sample(&x))
					tx_cur = x;
				else if (tx_end) {
					tx_done();
					break;
				} else {
					/* an underrun: silence (V.253's
					   event 23) */
					tx_cur = 0;
				}
			}
			y = tx_prev +
			    static_cast<float>(tx_pos) * (tx_cur - tx_prev);
		}
		{
			/* Type II caller ID: our acknowledgement out, the
			   line's audio to the DTE silent while it is on */
			bool was = ack_left > 0 || cid2;
			float a = type2(src[i]);
			if (was || ack_left > 0 || cid2) {
				y = a;
				x_in = 0;
			}
		}
		dst[i] = dsp_clip16(y);

		rx_cpt.sample(tone_left > 0 ? 0 : x_in);
		if (rx_cpt.rings != rings_told) {
			rings_told = rx_cpt.rings;
			DSP_DBG("voice: ringing tone\n");
			report('r');
		}
		if (rx_cpt.busy != busy_told) {
			busy_told = rx_cpt.busy;
			DSP_DBG("voice: busy tone\n");
			report('b');
		}
		for (s16 k = 0; k < 2; k++) {
			if (rx_cpt.answer[k] == answer_told[k])
				continue;
			answer_told[k] = rx_cpt.answer[k];
			DSP_DBG("voice: answer tone %s\n", k ? "2225" : "2100");
			report(k ? 'f' : 'a');
		}
		for (s16 k = 0; k < 2; k++) {
			rx_ct[k].sample(tone_left > 0 ? 0 : x_in);
			ct_since[k]++;
			if (rx_ct[k].bursts == ct_told[k])
				continue;
			ct_told[k] = rx_ct[k].bursts;
			if (ct_since[k] < 4L * srate)
				continue;
			ct_since[k] = 0;
			DSP_DBG("voice: calling tone %s\n", k ? "1300" : "1100");
			report(k ? 'e' : 'c');
		}

		if (tone_left > 0) {
			if (!dtmf_held) {
				if (rx_dtmf.digit)
					report('~');
				rx_dtmf.init(srate);
				dtmf_held = 1;
			}
		} else {
			dtmf_held = 0;
			switch (rx_dtmf.sample(x_in)) {
			case DtmfDetect::ON:
				report(rx_dtmf.digit);
				dtmf_since = 0;
				break;
			case DtmfDetect::OFF:
				report('~');
				break;
			default:
				if (rx_dtmf.digit && ++dtmf_since >= repeat) {
					report(rx_dtmf.digit);
					dtmf_since = 0;
				}
				break;
			}
		}

		/* the samples after the CONNECT, which the core writes when
		   this returns */
		if ((state & VOICE_STATE_RX) && pending != VOICE_STATUS_CONNECT) {
			/* the DTE's samples between the last line sample and
			   this one, interpolated (this one's own at rx_t 1) */
			float cur = rx_dtmf.digit ? 0.0f : x_in;
			while (rx_t <= 1 + 1e-9) {
				to_dte(rx_prev + static_cast<float>(rx_t) *
				       (cur - rx_prev));
				rx_t += rx_step;
			}
			rx_t -= 1;
			rx_prev = cur;
		}
	}
	if (!rxbuf.empty())
		modem_send_to_tty(modem, rxbuf.data(),
				  static_cast<int>(rxbuf.size()));

	/* any character from the DTE ends receive only mode */
	if (state == VOICE_STATE_RX) {
		char ch;
		if (modem_recv_from_tty(modem, &ch, 1) == 1) {
			static const char end[2] = { DLE, ETX };
			modem_send_to_tty(modem, end, 2);
			state = VOICE_STATE_COMMAND;
			pending = VOICE_STATUS_OK;
		}
	}

	status = pending;
	pending = 0;
	return status;
}

/* see dsplibs.h */
void VOICE_tones(void *obj, struct voice_tones *t)
{
	const CallToneDetect &d = static_cast<Voice*>(obj)->rx_cpt;

	t->rings = d.rings;
	t->ringing = d.ringing;
	t->quiet_ms = d.quiet_ms();
	t->busy = d.busy;
	t->ans2100 = d.answer[0];
	t->ans2225 = d.answer[1];
	t->cng = static_cast<Voice*>(obj)->rx_ct[0].bursts;
	t->ct = static_cast<Voice*>(obj)->rx_ct[1].bursts;
}

/* Voice::process() for the C interface */
int VOICE_process(void *obj, void *in, void *out, int count)
{
	return static_cast<Voice*>(obj)->process(in, out, count);
}

/* a forced V.21 caller's first block: after a 2100 Hz answer tone, once
   a call, to the V.8 pump (DSP_V8_LOWSPEED=0: not), which offers V.21
   alone if the tone is ANSam and else hands back */
s16 dsp::dp_v8_first(struct modem *m, enum DP_ID id)
{
	struct dp_runtime *rt = reinterpret_cast<struct dp_runtime *>(
		modem_get_param(m, MDMPRM_DPRUNTIME));
	if (!rt || rt->answer_tone != ANSTONE_2100 || rt->v8_tried ||
	    rt->after_v8 ||
	    env_off("DSP_V8_LOWSPEED"))
		return 0;
	rt->v8_tried = 1;
	rt->v8_for = id;
	modem_set_param(m, MDMPRM_DP_REQUESTED, DP_V34);
	return 1;
}
