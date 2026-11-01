/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dp_v8bis.cpp - V.8 bis's transactions at the start of a call
 *                   (DP_V8BIS), before V.8 (9.9.1/V.8 bis)
 *
 *    The answering modem is the initiating station (9.3, 10.2.2): after
 *    400 ms of silence it sends MRe (at 12 dB under the messages' level,
 *    7.1.4) and listens for 3 s for the calling modem's answer:
 *
 *        ESr MS          transaction 1: ACK(1) back when MS asks for it
 *                        (NAK(3) for a mode we have not got)
 *        CRd             transaction 10: our CL, the caller's MS, ACK(1)
 *        MRd             transaction 7: that would have us send MS and
 *                        be the calling modem of the start-up after it,
 *                        which is not done here
 *
 *    and then hands over to V.8 (the core's next pump, DP_V8): its
 *    ANSam, the caller's CM and so on.  Nothing heard, MRd, a NAK, an
 *    invalid frame (NAK(1) back, 9.8) or a message out of turn is the
 *    same V.8 start-up without V.8 bis, which a modem that has no V.8
 *    bis answers as usual.
 *
 *    The calling modem is the responding station: its call progress
 *    heard an initiating signal (rt->v8bis_sig) and handed over.  To
 *    MRe, CRe, MRd or CRd it sends CRd and waits for the CL; to ESi, the
 *    message after it.  From the CL it picks its MS: the data mode, the
 *    best modulation both have, V.42 and V.42 bis where both have them,
 *    the V.8 start-up when the answerer has it, ACK(1) asked for.  At the
 *    ACK(1) (or ANSam heard: the ACK lost) it hands over to V.8 as the
 *    caller.  A NAK, a CL with nothing in common, an MS or CLR after ESi
 *    (the answering modem would then be V.8's caller), or 5 s without an
 *    answer: it waits for the answer tone as call progress would.
 *
 *    The station that receives MS is V.8's answering modem (9.9), which
 *    in both cases here is the one that answered the call.
 *
 */

#include <algorithm>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"
#include "v8bis.h"

namespace dsp {

using namespace v8bis;

enum class Vb {
	SILENCE,        /* answerer: the silence before MRe */
	MR,             /* answerer: MRe, then the caller's answer */
	CL,             /* answerer: our CL, then the MS */
	START,          /* caller: a moment before our answer */
	ESI,            /* caller: the message after ESi */
	WAIT_CL,        /* caller: our CRd, then the CL */
	WAIT_ACK,       /* caller: our MS, then ACK(1) or NAK */
	ANSWER,         /* caller: the answer tone */
	FINISH,         /* ours goes out, then then_ */
};

class V8bisPump : public PumpBase<V8bisPump, Vb> {
public:
	static constexpr const char *name = "V8BIS";
	/* its name in the log */
	const char *tag() const { return "v8bis"; }

	/* the messages' level (V.8's), MRe's and CRe's 12 dB under it */
	static constexpr float AMP = 4225.0f;
	static constexpr float AMP_E = AMP / 4;

	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 process(void *in, void *out, s32 cnt) override;
	/* one sample in, one out (pump_loop()) */
	int sample(float x, float *y);
	/* never in data */
	bool in_data() const { return false; }

private:
	enum class Then { V8, FALLBACK };

	/* what we have: the V.8 start-up, data with these */
	static Caps ours()
	{
		Caps c;
		c.id = ID_V8;
		c.modes = MODE_DATA;
		c.data = DATA_V42 | DATA_V42BIS | DATA_V34 | DATA_V32BIS |
			DATA_V32 | DATA_V22BIS | DATA_V22 | DATA_V21;
		return c;
	}
	void signal(u8 s, bool initiating, float amp);
	void send(u8 type, const Caps &c = Caps());
	float tx();
	int message(const Msg &m);
	int answer_ms(const Caps &c);
	int caller_cl(const Caps &c);
	int finish(Then t, const char *why);
	int to_v8(s16 tone);
	int answer_tone(float x);

	SigTx sigtx_;
	bool sig_on_ = false;
	MsgTx msgtx_;
	SigRx sigrx_;
	MsgRx msgrx_;
	Then then_ = Then::V8;
	u8 heard_ = SIG_NONE;           /* the caller's: call progress's */
	/* the answer tone (caller): 10 ms blocks of 2100 and 2225 Hz */
	Goertzel g2100_, g2225_;
	float blk_e_ = 0;
	s16 run2100_ = 0, run2225_ = 0;
	bool acked_ = false;            /* the answerer's ACK(1) came */
};

/* the answerer's MRe after its silence, or the caller's answer to the
   signal call progress heard; 0 */
s16 V8bisPump::start(struct modem *m, enum DP_ID, s16 caller_, s32 srate_,
		     s32)
{
	pump_setup(m, srate_, caller_);
	/* the initiating station (the answerer) sends on V.21's low
	   channel and hears the high one; the responding one the other way
	   round */
	msgtx_.init(caller_, AMP, srate_);
	msgrx_.init(!caller_, srate_);
	sigrx_.init(srate_, caller_ ? SigRx::PAIR_INIT : SigRx::PAIR_RESP);
	g2100_.init(2100, static_cast<s16>(srate_ / 100), srate_);
	g2225_.init(2225, static_cast<s16>(srate_ / 100), srate_);
	if (caller_ && rt) {
		heard_ = static_cast<u8>(rt->v8bis_sig);
		rt->v8bis_sig = SIG_NONE;
	}
	set_state(caller_ ? Vb::START : Vb::SILENCE);
	DSP_DBG("v8bis: start, %s%s%s\n", caller_ ? "caller after " :
		"answerer", caller_ ? sig_name(heard_) : "",
		caller_ ? " heard" : "");
	return 0;
}

/* the block, a sample at a time (sample()) */
s16 V8bisPump::process(void *in, void *out, s32 cnt)
{
	return pump_loop(in, out, cnt);
}

/* signal s out (the initiating pair or the responding one) at amp */
void V8bisPump::signal(u8 s, bool initiating, float amp)
{
	DSP_DBG("v8bis: %s out\n", sig_name(s));
	sigtx_.init(s, initiating, amp, srate);
	sig_on_ = true;
}

/* a message of type with c out (after what is going out) */
void V8bisPump::send(u8 type, const Caps &c)
{
	Msg m = msg_of(type, c);
	DSP_DBG("v8bis: %s out\n", type_name(type));
	msgtx_.send(msg_encode(m));
}

/* our next sample: the signal, then the messages */
float V8bisPump::tx()
{
	if (sig_on_) {
		if (!sigtx_.done())
			return sigtx_.next();
		sig_on_ = false;
	}
	return msgtx_.next();
}

/* the answerer's end of the transaction once ours went out: V.8 after
   V.8 bis (t V8) or without it (FALLBACK), why for the log */
int V8bisPump::finish(Then t, const char *why)
{
	DSP_DBG("v8bis: %s\n", why);
	then_ = t;
	set_state(Vb::FINISH);
	return DPSTAT_OK;
}

/* the caller to V.8 (tone the answer tone heard); the answerer to V.8's
   ANSam: DPSTAT_CHANGEDP */
int V8bisPump::to_v8(s16 tone)
{
	if (caller && rt) {
		rt->answer_tone = tone;
		rt->answer_tone_end = 0;
	}
	DSP_DBG("v8bis: on to V.8\n");
	return DPSTAT_CHANGEDP;
}

/* the answerer: the caller's MS (c) */
int V8bisPump::answer_ms(const Caps &c)
{
	u32 mods = c.data & DATA_MODS;
	if (!(c.modes & MODE_DATA) || (c.id & ID_SHORT_V8) ||
	    (mods && !(mods & ours().data))) {
		send(NAK3);
		return finish(Then::FALLBACK, "MS: a mode we have not got");
	}
	if (c.id & ID_ACK1)
		send(ACK1);
	return finish(Then::V8, "MS: data");
}

/* the caller: the answerer's CL (c), our MS */
int V8bisPump::caller_cl(const Caps &c)
{
	static const u32 best[] = { DATA_V34, DATA_V32BIS, DATA_V32,
				    DATA_V22BIS, DATA_V22, DATA_V21 };
	Caps o = ours(), ms;
	u32 both = c.data & o.data;
	if (!(c.modes & MODE_DATA)) {
		DSP_DBG("v8bis: CL: no data mode\n");
		set_state(Vb::ANSWER);
		return DPSTAT_OK;
	}
	for (u32 b : best) {
		if (both & b) {
			ms.data = b;
			break;
		}
	}
	if (!ms.data) {
		DSP_DBG("v8bis: CL: no modulation in common\n");
		set_state(Vb::ANSWER);
		return DPSTAT_OK;
	}
	ms.id = ID_ACK1 | (c.id & ID_V8);
	ms.modes = MODE_DATA;
	ms.data |= both & (DATA_V42 | DATA_V42BIS);
	send(MS, ms);
	set_state(Vb::WAIT_ACK);
	return DPSTAT_OK;
}

/* a message m from the far end */
int V8bisPump::message(const Msg &m)
{
	Caps c = caps_of(m);
	DSP_DBG("v8bis: %s in (revision %u, I %02x, modes %02x, data %06x)\n",
		type_name(m.type), m.rev, c.id, c.modes,
		static_cast<unsigned>(c.data));
	switch (state) {
	case Vb::MR:
	case Vb::CL:
		if (m.type == MS)
			return answer_ms(c);
		return finish(Then::FALLBACK, "a message out of turn");
	case Vb::ESI:
	case Vb::WAIT_CL:
		if (m.type == CL)
			return caller_cl(c);
		/* (MS, CLR: the answering modem would be V.8's caller) */
		DSP_DBG("v8bis: %s: not done here\n", type_name(m.type));
		set_state(Vb::ANSWER);
		return DPSTAT_OK;
	case Vb::WAIT_ACK:
		/* V.8's caller once it hears ANSam (its 0.2 s of silence
		   first) */
		if (m.type == ACK1) {
			acked_ = true;
			DSP_DBG("v8bis: ACK(1) in\n");
		} else {
			DSP_DBG("v8bis: no ACK(1)\n");
		}
		set_state(Vb::ANSWER);
		return DPSTAT_OK;
	default:
		return DPSTAT_OK;
	}
}

/* the caller's answer tone detector on x: 1 (2100 Hz) or 2 (2225 Hz)
   when one has been on for 100 ms (after our MS) or 300 ms, else 0 */
int V8bisPump::answer_tone(float x)
{
	blk_e_ += x * x;
	g2225_.run(x);
	if (!g2100_.run(x))
		return 0;
	float bp = blk_e_ / g2100_.len, min = tone_power(150);
	blk_e_ = 0;
	run2100_ = g2100_.power > 0.6f * bp && bp > min ?
		static_cast<s16>(run2100_ + 1) : 0;
	run2225_ = g2225_.power > 0.6f * bp && bp > min ?
		static_cast<s16>(run2225_ + 1) : 0;
	s16 need = state == Vb::WAIT_ACK || acked_ ? 10 : 30;
	return run2100_ >= need ? 1 : run2225_ >= need ? 2 : 0;
}

/* a sample x in, ours in *y: the receivers, then the transaction's
   step */
int V8bisPump::sample(float x, float *y)
{
	u8 sig = sigrx_.run(x);
	int tone = caller ? answer_tone(x) : 0;
	msgrx_.run(x);
	*y = tx();

	if (state == Vb::FINISH) {
		if (sig_on_ || !msgtx_.idle())
			return DPSTAT_OK;
		if (then_ == Then::FALLBACK)
			DSP_DBG("v8bis: V.8 without V.8 bis\n");
		return to_v8(ANSTONE_NONE);
	}
	/* an invalid frame (9.8): NAK(1) */
	if (msgrx_.bad && (state == Vb::MR || state == Vb::CL ||
			   state == Vb::ESI || state == Vb::WAIT_CL ||
			   state == Vb::WAIT_ACK)) {
		msgrx_.bad = 0;
		msgrx_.msgs.clear();
		send(NAK1);
		return finish(Then::FALLBACK, "an invalid frame");
	}
	msgrx_.bad = 0;
	while (!msgrx_.msgs.empty()) {
		std::vector<u8> f = msgrx_.msgs.front();
		Msg m;
		msgrx_.msgs.erase(msgrx_.msgs.begin());
		if (!msg_decode(f.data(), f.size(), &m)) {
			DSP_DBG("v8bis: a message we cannot read\n");
			continue;
		}
		int r = message(m);
		if (r != DPSTAT_OK)
			return r;
	}

	switch (state) {
	case Vb::SILENCE:
		/* 400 ms at least (10.2.2) */
		if (count >= ms2samples(400, srate)) {
			signal(SIG_MRE, true, AMP_E);
			set_state(Vb::MR);
		}
		break;
	case Vb::MR:
		if (sig == SIG_CRD) {
			DSP_DBG("v8bis: CRd in\n");
			send(CL, ours());
			set_state(Vb::CL);
		} else if (sig == SIG_MRD) {
			/* transaction 7 */
			return finish(Then::FALLBACK, "MRd in: not done here");
		} else if (count >= ms2samples(3500, srate)) {
			/* MRe and 3 s after it */
			return finish(Then::FALLBACK, "no answer to MRe");
		}
		break;
	case Vb::CL:
		if (count >= ms2samples(5000, srate))
			return finish(Then::FALLBACK, "no MS");
		break;
	case Vb::START:
		/* the end of its segment 2 */
		if (count < ms2samples(100, srate))
			break;
		if (heard_ == SIG_ESI) {
			set_state(Vb::ESI);
		} else if (heard_ == SIG_MRE || heard_ == SIG_CRE ||
			   heard_ == SIG_MRD || heard_ == SIG_CRD) {
			signal(SIG_CRD, false, AMP);
			set_state(Vb::WAIT_CL);
		} else {
			set_state(Vb::ANSWER);
		}
		break;
	case Vb::ESI:
		if (count >= ms2samples(2000, srate)) {
			DSP_DBG("v8bis: no message after ESi\n");
			set_state(Vb::ANSWER);
		}
		break;
	case Vb::WAIT_CL:
		if (count >= ms2samples(5000, srate)) {
			DSP_DBG("v8bis: no CL\n");
			set_state(Vb::ANSWER);
		}
		break;
	case Vb::WAIT_ACK:
		/* ANSam: the ACK(1) was lost */
		if (tone == 1)
			return to_v8(ANSTONE_2100);
		if (count >= ms2samples(5000, srate)) {
			DSP_DBG("v8bis: no ACK(1)\n");
			set_state(Vb::ANSWER);
		}
		break;
	case Vb::ANSWER:
		if (tone)
			return to_v8(tone == 1 ? ANSTONE_2100 : ANSTONE_2225);
		break;
	case Vb::FINISH:
		break;
	}
	return DPSTAT_OK;
}

/* registers the pump as DP_V8BIS */
s16 dp_v8bis_init(void)
{
	return modem_dp_register(DP_V8BIS, &PumpOps<V8bisPump>::ops);
}

/* and no more */
void dp_v8bis_exit(void)
{
	modem_dp_deregister(DP_V8BIS, &PumpOps<V8bisPump>::ops);
}

} // namespace dsp
