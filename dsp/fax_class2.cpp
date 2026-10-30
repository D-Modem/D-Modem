/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fax_class2.cpp - T.32 service class 2.0, and the class 2 of the
 *                   older EIA/TIA-592 draft (SP-2388) as fax programs
 *                   have it, for the modem core: FAX2_create,
 *                   FAX2_process, FAX2_command (+FDT, +FDR, +FET, +FKS)
 *
 *    The modem runs T.30 itself (t30.cpp, paced by this engine as its
 *    host), and the DTE has the pages in T.4 MH with EOLs:
 *
 *    Sending (ATD): +FCO, +FCI:"<CSI>", +FIS:<DIS> and OK once the DIS
 *    came.  +FDT: the DCS with +FIS's resolution, rate and ECM, the
 *    training; +FCS:<DCS> and CONNECT once the CFR came (class 2: and an
 *    XON); the page, <DLE> shielded, ends with <DLE><ETX>: OK.  +FET=0
 *    (another page: MPS), 1 (EOM) or 2 (the last: EOP): +FPS:1,... once
 *    the MCF came, +FHS:00 after the last, OK.
 *
 *    Receiving (ATA): +FCO, +FTI:"<TSI>", +FCS:<DCS> and OK once the CFR
 *    went out.  +FDR: CONNECT, the page once the DTE sent DC2 (or a
 *    second went by), <DLE><ETX>, +FPS:1,<lines>,0,0,0, +FET:<0, 1 or
 *    2>, OK; after the last page +FHS:00 and OK.  The pages are taken
 *    whole and confirmed (MCF) as they come, whatever the DTE does; it
 *    has them in its time.
 *
 *    Class 2 says +FCON, +FCSI, +FDIS, +FDCS, +FTSI, +FPTS and +FHNG
 *    instead, and its hangup codes in decimal.  The data is MH whatever
 *    the remote used (MR, ECM: this end decodes and codes it again), its
 *    bit order +FBO's (0: the first bit of an octet its lowest).
 *
 *    Polling: answering with +FLP=1, a DTC has ATA report +FCO,
 *    +FPI:"<CIG>", +FTC:<DTC> and OK, and the pages go as when sending
 *    (+FDT, +FET).  Calling, a DIS that offers a document adds +FPO, and
 *    +FDR then polls (CIG, DTC): +FTI, +FCS, CONNECT and the pages as
 *    when receiving.  The line may be T.38 from any point
 *    (FAX2_t38_start()).
 *
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <utility>

#include "dm_types.h"
#include "dsp.h"
#include "reinit.h"
#include "t30.h"
#include "t4.h"
#include <dsplibs.h>

using namespace dsp;

namespace {

const char DLE = 0x10, ETX = 0x03, SUB = 0x1a, DC2 = 0x12, XON = 0x11;

/* +FDR: the DTE's DC2 waited for so long at most */
const u32 TRIGGER_MS = 1000;
/* the call over and no command for so long: the engine ends it */
const u32 IDLE_END_MS = 10000;

/* what the DTE waits for the result of */
enum Cmd {
	CMD_NONE,
	CMD_CALL,               /* ATD, ATA */
	CMD_FDT,                /* the session first */
	CMD_FDT_DATA,           /* the page from the DTE */
	CMD_FET,
	CMD_FDR,
	CMD_FDR_TRIGGER,        /* CONNECT out: DC2 */
	CMD_FKS,
};

/* text or a result for the DTE, in its turn */
struct Out {
	std::string text;
	int status;             /* FAX_STATUS_*, 0: the text */
};

/* receiving: a session (the CFR went out) or a page, as they came */
struct RxItem {
	bool session;
	unsigned params[8];
	char ident[21];
	T4Page page;
	s16 post;               /* the command after the page, -1 not yet */
};

struct Fax2 {
	struct modem *modem = nullptr;
	T30 *t30 = nullptr;
	struct fax2_settings set;
	bool caller = false;
	u32 srate = 8000;
	/* T.38: the core's sender of IFP packets (FAX2_t38_start()) */
	bool t38 = false;
	void (*t38_tx)(void *ctx, const void *ifp) = nullptr;
	void *t38_ctx = nullptr;
	bool ident_told = false;        /* receiving: +FTI said */
	std::deque<Out> out;
	size_t out_pos = 0;             /* of the first's text, written */
	Cmd cmd = CMD_CALL;
	u32 cmd_samples = 0;            /* since the state of cmd began */
	u32 idle_samples = 0;           /* the call over, no command */
	bool ending = false;            /* the call ends after the result */
	bool aborted = false;           /* +FKS */
	int hangup = -1;
	unsigned session[8] = {};
	bool session_set = false;

	/* sending */
	bool trained = false;           /* a CFR since the DIS */
	bool fcs_due = false;           /* +FCS not yet told for it */
	int fet = -1;                   /* +FET's for the page, -1: none */
	bool page_sent = false;         /* its MCF came */
	int page_post = 0;
	std::string data;               /* the page from the DTE */
	bool dle = false;
	T4Page ready;                   /* decoded, for next_page() */
	bool have_ready = false;

	/* receiving */
	std::deque<RxItem> rx;

	/* the name 2.0 has (c20), or class 2 (c2) */
	const char *name(const char *c20, const char *c2) const {
		return set.cls == 2 ? c2 : c20;
	}
	/* a line of text to the DTE */
	void text(const std::string &s) {
		out.push_back({ s + "\r\n", 0 });
	}
	/* characters to the DTE as they are */
	void raw(const std::string &s) {
		out.push_back({ s, 0 });
	}
	/* a result (FAX_STATUS_*) after what is due before it */
	void result(int st) {
		out.push_back({ std::string(), st });
	}
	/* the command's state c, its time from now */
	void start(Cmd c) {
		cmd = c;
		cmd_samples = 0;
	}
	std::string params(const unsigned *v) const;
	void info(const char *c20, const char *c2, const unsigned *v);
	void ident(const char *c20, const char *c2, const char *id);
	int hangup_code(const struct t30_status &st) const;
	void hangup_line(const struct t30_status &st);
	void end_command(const struct t30_status &st);
	void dte_page();
	void deliver(RxItem &it);
	void step(const struct t30_status &st);
	void snapshot(unsigned *v, const struct t30_status &st) const;
};

/* the T.32 parameter codes of a rate */
unsigned br_code(s32 bps)
{
	return bps >= 14400 ? 5 : bps >= 12000 ? 4 : bps >= 9600 ? 3 :
		bps >= 7200 ? 2 : bps >= 4800 ? 1 : 0;
}

/* +FIS's br as bit/s */
s32 br_bps(unsigned br)
{
	static const s32 bps[6] = { 2400, 4800, 7200, 9600, 12000, 14400 };
	return bps[br > 5 ? 5 : br];
}

/* vr,br,wd,ln,df,ec,bf,st */
std::string Fax2::params(const unsigned *v) const
{
	std::string s;
	for (int i = 0; i < 8; i++) {
		char b[16];
		snprintf(b, sizeof(b), "%s%u", i ? "," : "", v[i]);
		s += b;
	}
	return s;
}

/* +FIS:<v> or the like */
void Fax2::info(const char *c20, const char *c2, const unsigned *v)
{
	text(std::string("+") + name(c20, c2) + ":" + params(v));
}

/* +FCI:"<id>" or the like (none for no identity) */
void Fax2::ident(const char *c20, const char *c2, const char *id)
{
	if (id && *id)
		text(std::string("+") + name(c20, c2) + ":\"" + id + "\"");
}

/* the session as +FCS has it (MH, any length, the rows' fill ours) */
void Fax2::snapshot(unsigned *v, const struct t30_status &st) const
{
	v[0] = st.fine ? 1 : 0;
	v[1] = br_code(st.bps);
	v[2] = 0;
	v[3] = 2;
	v[4] = 0;
	v[5] = st.ecm ? 2 : 0;
	v[6] = 0;
	v[7] = 0;
}

/* T.32's hangup status for how the call ended */
int Fax2::hangup_code(const struct t30_status &st) const
{
	bool tx = st.sending != 0;
	if (aborted)
		return 2;
	switch (st.result) {
	case T30_OK:
		return 0;
	case T30_ERR_NO_FAX:
		return 11;
	case T30_ERR_INCOMPATIBLE:
		return tx ? 21 : 70;
	case T30_ERR_TRAINING:
		return tx ? 27 : 70;
	case T30_ERR_NO_RESPONSE:
		return tx ? (st.pages ? 52 : 25) : 73;
	case T30_ERR_PAGE:
		return tx ? 50 : 90;
	case T30_ERR_HANGUP:
		return tx ? 20 : 70;
	case T30_ERR_DOCUMENT:
		return 40;
	case T30_ERR_FILE:
		return 90;
	case T30_ERR_HOST:
	default:
		return tx ? 40 : 90;
	}
}

/* +FHS:<code> (2.0: the decimal digits as hexadecimal ones, 100 A0) or
   +FHNG:<code> */
void Fax2::hangup_line(const struct t30_status &st)
{
	char b[24];
	hangup = hangup_code(st);
	if (set.cls == 2)
		snprintf(b, sizeof(b), "+FHNG:%d", hangup);
	else
		snprintf(b, sizeof(b), "+FHS:%02X",
			 (hangup / 10) << 4 | hangup % 10);
	text(b);
}

/* the call is over with the command under way: its hangup status and
   result, then the end of the call */
void Fax2::end_command(const struct t30_status &st)
{
	hangup_line(st);
	result(cmd == CMD_CALL && st.result != T30_OK ? FAX_STATUS_NOCARRIER :
	       FAX_STATUS_OK);
	start(CMD_NONE);
	ending = true;
}

/* +FDT: the DTE's characters, the <DLE> codes taken out; at <DLE><ETX>
   the page decoded for next_page(), and OK */
void Fax2::dte_page()
{
	char c;

	/* a character at a time: what follows <DLE><ETX> is commands */
	while (modem_recv_from_tty(modem, &c, 1) == 1) {
		if (!dle) {
			if (c == DLE)
				dle = true;
			else
				data += c;
			continue;
		}
		dle = false;
		if (c == DLE) {
			data += DLE;
		} else if (c == SUB) {
			data += DLE;
			data += DLE;
		} else if (c == ETX) {
			struct t30_status st;
			t30_get_status(t30, &st);
			ready.release();
			ready.init(T4_WIDTH, st.fine);
			ready.decode(reinterpret_cast<const unsigned char *>(
					     data.data()),
				     data.size(), 3, 0, set.bo & 1, 0);
			DSP_DBG("fax2: page from the DTE: %zu octets, "
				"%" PRId32 " rows\n", data.size(),
				ready.rows);
			data.clear();
			start(CMD_NONE);
			if (ready.rows <= 0) {
				ready.release();
				result(FAX_STATUS_FRAME_ERROR);
				return;
			}
			have_ready = true;
			result(FAX_STATUS_OK);
			/* what follows is commands */
			return;
		}
	}
}

/* +FDR's page to the DTE: MH, the octets in +FBO's order, <DLE>
   shielded; +FPS, +FET, OK */
void Fax2::deliver(RxItem &it)
{
	T4Bits b;
	std::string d;
	char line[64];

	it.page.encode_mh(&b, 0, 0, 1);
	d.reserve((b.n + 7) / 8 + 16);
	for (size_t k = 0; k < (b.n + 7) / 8; k++) {
		u8 c = b.buf[k];
		if (!(set.bo & 1)) {
			/* the first bit lowest */
			u8 r = 0;
			for (int j = 0; j < 8; j++)
				if (c & (1 << j))
					r |= static_cast<u8>(0x80 >> j);
			c = r;
		}
		if (c == static_cast<u8>(DLE))
			d += DLE;
		d += static_cast<char>(c);
	}
	d += DLE;
	d += ETX;
	raw(d);
	snprintf(line, sizeof(line), "+%s:1,%" PRId32 ",0,0,0",
		 name("FPS", "FPTS"), it.page.rows);
	text(line);
	snprintf(line, sizeof(line), "+FET:%d", it.post < 0 ? 2 : it.post);
	text(line);
	result(FAX_STATUS_OK);
	DSP_DBG("fax2: page to the DTE: %" PRId32 " rows, %zu octets\n",
		it.page.rows, d.size());
}

/* the command under way, as far as the call lets it go */
void Fax2::step(const struct t30_status &st)
{
	switch (cmd) {
	case CMD_CALL:
		if (st.done) {
			end_command(st);
		} else if (st.sending && st.waiting == T30_WAIT_DCS) {
			/* the DIS, or (polled) the DTC */
			unsigned v[8] = { static_cast<unsigned>(st.remote_fine),
					  br_code(st.remote_bps), 0, 2, 0,
					  st.remote_ecm ? 2u : 0u, 0, 0 };
			text(std::string("+") + name("FCO", "FCON"));
			if (caller) {
				ident("FCI", "FCSI", st.remote_ident);
				info("FIS", "FDIS", v);
				if (st.remote_poll)
					text(std::string("+") +
					     name("FPO", "FPOLL"));
			} else {
				ident("FPI", "FCIG", st.remote_ident);
				info("FTC", "FDTC", v);
			}
			result(FAX_STATUS_OK);
			start(CMD_NONE);
		} else if (!caller && !rx.empty() && rx.front().session) {
			RxItem &it = rx.front();
			text(std::string("+") + name("FCO", "FCON"));
			ident("FTI", "FTSI", it.ident);
			info("FCS", "FDCS", it.params);
			ident_told = true;
			rx.pop_front();
			result(FAX_STATUS_OK);
			start(CMD_NONE);
		}
		break;
	case CMD_FDT:
		if (st.done) {
			end_command(st);
		} else if (st.waiting == T30_WAIT_DCS) {
			t30_session(t30, set.vr != 0, br_bps(set.br),
				    set.ec != 0);
			t30_resume(t30);
		} else if (trained) {
			if (fcs_due) {
				snapshot(session, st);
				session_set = true;
				info("FCS", "FDCS", session);
				fcs_due = false;
			}
			result(FAX_STATUS_CONNECT);
			if (set.cls == 2)
				raw(std::string(1, XON));
			data.clear();
			dle = false;
			start(CMD_FDT_DATA);
		}
		break;
	case CMD_FDT_DATA:
		dte_page();
		break;
	case CMD_FET:
		if (page_sent) {
			char line[48];
			snprintf(line, sizeof(line), "+%s:1,0,0,0,0",
				 name("FPS", "FPTS"));
			text(line);
			page_sent = false;
			if (page_post == 2) {
				/* the DCN, then +FHS */
				start(CMD_FKS);
				break;
			}
			result(FAX_STATUS_OK);
			start(CMD_NONE);
		} else if (st.done) {
			end_command(st);
		} else if (st.waiting == T30_WAIT_POST) {
			t30_resume(t30);
		}
		break;
	case CMD_FDR:
		if (st.sending && st.waiting == T30_WAIT_DCS) {
			/* polling: the DTC, then we receive */
			if (t30_poll(t30) < 0) {
				result(FAX_STATUS_FRAME_ERROR);
				start(CMD_NONE);
			}
			break;
		}
		while (!rx.empty() && rx.front().session) {
			/* (a poller hears the sender's TSI here first) */
			if (!ident_told) {
				ident("FTI", "FTSI", rx.front().ident);
				ident_told = true;
			}
			info("FCS", "FDCS", rx.front().params);
			rx.pop_front();
		}
		if (!rx.empty() && (rx.front().post >= 0 || st.done)) {
			result(FAX_STATUS_CONNECT);
			start(CMD_FDR_TRIGGER);
		} else if (rx.empty() && st.done) {
			end_command(st);
		}
		break;
	case CMD_FDR_TRIGGER: {
		char b[64];
		int n;
		bool go = cmd_samples >= TRIGGER_MS * srate / 1000;
		while ((n = modem_recv_from_tty(modem, b, sizeof(b))) > 0)
			for (int i = 0; i < n; i++)
				if (b[i] == DC2)
					go = true;
		if (go) {
			deliver(rx.front());
			rx.pop_front();
			start(CMD_NONE);
		}
		break;
	}
	case CMD_FKS:
		if (st.done)
			end_command(st);
		break;
	case CMD_NONE:
		break;
	}
}

/* T.30's next page: the one the DTE gave, or not yet (2) */
s16 next_page(void *ctx, T4Page *page)
{
	Fax2 *f = static_cast<Fax2 *>(ctx);
	if (!f->have_ready)
		return 2;
	*page = std::move(f->ready);
	f->ready = T4Page();
	f->have_ready = false;
	return 1;
}

/* T.30's command after the page: +FET's, or not yet */
s16 post_page(void *ctx)
{
	return static_cast<s16>(static_cast<Fax2 *>(ctx)->fet);
}

/* T.30 receiving: a page came */
void page_received(void *ctx, T4Page *page)
{
	Fax2 *f = static_cast<Fax2 *>(ctx);
	f->rx.emplace_back();
	RxItem &it = f->rx.back();
	it.session = false;
	it.page = *page;
	it.post = -1;
}

/* T.30's events */
void event(void *ctx, s16 ev, s16 arg)
{
	Fax2 *f = static_cast<Fax2 *>(ctx);
	struct t30_status st;

	t30_get_status(f->t30, &st);
	switch (ev) {
	case T30_EV_DIS:
		f->trained = false;
		break;
	case T30_EV_TRAINED:
		if (st.sending) {
			f->trained = true;
			f->fcs_due = true;
			break;
		}
		f->rx.emplace_back();
		{
			RxItem &it = f->rx.back();
			it.session = true;
			f->snapshot(it.params, st);
			std::memcpy(it.ident, st.remote_ident, sizeof(it.ident));
			it.post = -1;
			std::memcpy(f->session, it.params, sizeof(f->session));
			f->session_set = true;
		}
		break;
	case T30_EV_POST:
		/* the page that came last */
		for (auto it = f->rx.rbegin(); it != f->rx.rend(); ++it)
			if (!it->session) {
				if (it->post < 0)
					it->post = arg;
				break;
			}
		break;
	case T30_EV_PAGE_SENT:
		f->page_sent = true;
		f->page_post = arg;
		f->fet = -1;
		if (arg == 1)
			f->trained = false;     /* EOM: a DIS again */
		break;
	}
}


/* T.30's IFP packet to the core */
void t38_out(void *ctx, const struct t38_ifp *p)
{
	Fax2 *f = static_cast<Fax2 *>(ctx);
	if (f->t38 && f->t38_tx)
		f->t38_tx(f->t38_ctx, p);
}

} // namespace

/* (dsplibs.h) */
void *FAX2_create(struct modem *m, unsigned caller, unsigned srate,
		  const struct fax2_settings *s)
{
	Fax2 *f = new (std::nothrow) Fax2();
	struct t30_config c;

	if (!f)
		return nullptr;
	f->modem = m;
	f->set = *s;
	f->set.ident[sizeof(f->set.ident) - 1] = '\0';
	f->caller = caller != 0;
	f->srate = srate ? srate : 8000;
	zero(c);
	c.sending = f->caller;
	c.ident = f->set.ident;
	c.max_bps = br_bps(s->br);
	c.ecm = s->ec != 0;
	c.srate = static_cast<s32>(f->srate);
	c.next_page = next_page;
	c.page_received = page_received;
	c.ctx = f;
	c.host = 1;
	c.fine = s->vr != 0;
	c.post_page = post_page;
	c.event = event;
	c.polled = s->lp != 0;
	f->set.pi[sizeof(f->set.pi) - 1] = '\0';
	c.poll_ident = f->set.pi[0] ? f->set.pi : nullptr;
	c.t38_tx = t38_out;
	f->t30 = t30_create(&c);
	if (!f->t30) {
		delete f;
		return nullptr;
	}
	DSP_DBG("fax2: create, class %s, %s\n", s->cls == 2 ? "2" : "2.0",
		caller ? "caller" : "answerer");
	return f;
}

/* (dsplibs.h) */
void FAX2_delete(void *obj)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	if (!f)
		return;
	t30_free(f->t30);
	f->ready.release();
	delete f;
}

/* (dsplibs.h) the T.30 machine on the samples, the command under way,
   what is due to the DTE: its text, and the next result */
int FAX2_process(void *obj, void *in, void *out, int count)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	struct t30_status st;

	if (f->t38) {
		/* the samples are T.38's time; the audio silent */
		t30_t38_time(f->t30, count);
		std::memset(out, 0, sizeof(s16) * static_cast<size_t>(count));
	} else {
		t30_process(f->t30, static_cast<const s16 *>(in),
			    static_cast<s16 *>(out), count);
	}
	f->cmd_samples += static_cast<u32>(count);
	t30_get_status(f->t30, &st);
	/* the host's part: the page it waits for is there */
	if (st.waiting == T30_WAIT_PAGE && f->have_ready) {
		t30_resume(f->t30);
		t30_get_status(f->t30, &st);
	}
	f->step(st);
	t30_get_status(f->t30, &st);
	if (st.done && f->cmd == CMD_NONE && f->out.empty()) {
		/* over, and nothing asked: the end of the call by itself in
		   a while (the DTE has its +FDR or ATH first) */
		f->idle_samples += static_cast<u32>(count);
		if (f->ending ||
		    f->idle_samples >= IDLE_END_MS * f->srate / 1000)
			return -1;
	} else {
		f->idle_samples = 0;
	}

	/* to the DTE, in turn: text as far as it takes it, a result */
	while (!f->out.empty()) {
		Out &o = f->out.front();
		if (o.status) {
			int s = o.status;
			f->out.pop_front();
			return s;
		}
		while (f->out_pos < o.text.size()) {
			int n = modem_send_to_tty(
				f->modem, o.text.data() + f->out_pos,
				static_cast<int>(o.text.size() - f->out_pos));
			if (n <= 0)
				return 0;
			f->out_pos += static_cast<size_t>(n);
		}
		f->out_pos = 0;
		f->out.pop_front();
	}
	return 0;
}

/* (dsplibs.h) */
int FAX2_command(void *obj, unsigned cmd, unsigned param,
		 const struct fax2_settings *s)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	struct t30_status st;

	if (!f || f->cmd != CMD_NONE)
		return -1;
	t30_get_status(f->t30, &st);
	f->set.vr = s->vr;
	f->set.br = s->br;
	f->set.ec = s->ec;
	f->set.bo = s->bo;
	switch (cmd) {
	case FAX2_CMD_FDT:
		if (!st.sending || f->have_ready)
			return -1;
		f->start(CMD_FDT);
		break;
	case FAX2_CMD_FDR:
		/* (a caller polls, if the remote has a document) */
		if (st.sending && !(f->caller &&
				    st.waiting == T30_WAIT_DCS &&
				    st.remote_poll))
			return -1;
		f->start(CMD_FDR);
		break;
	case FAX2_CMD_FET:
		if (!st.sending || param > 2)
			return -1;
		f->fet = static_cast<int>(param);
		f->start(CMD_FET);
		break;
	case FAX2_CMD_FKS:
		if (!st.done) {
			f->aborted = true;
			t30_abort(f->t30);
		}
		f->start(CMD_FKS);
		break;
	default:
		return -1;
	}
	return 1;
}

/* (dsplibs.h) */
void FAX2_info(void *obj, struct fax2_info *i)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	std::memset(i, 0, sizeof(*i));
	i->hangup = -1;
	if (!f)
		return;
	std::memcpy(i->session, f->session, sizeof(i->session));
	i->session_set = f->session_set;
	i->hangup = f->hangup;
}

/* (dsplibs.h) the line becomes T.38: what goes out starts over on it */
int FAX2_t38_start(void *obj, void (*tx)(void *ctx, const void *ifp),
		   void *ctx, int max_data)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	if (!f)
		return -1;
	f->t38_tx = tx;
	f->t38_ctx = ctx;
	f->t38 = true;
	t30_set_t38(f->t30, 1, max_data);
	DSP_DBG("fax2: T.38 from now on\n");
	return 0;
}

/* (dsplibs.h) back to the audio */
void FAX2_t38_stop(void *obj)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	if (!f || !f->t38)
		return;
	f->t38 = false;
	t30_set_t38(f->t30, 0, 0);
	DSP_DBG("fax2: T.38 over\n");
}

/* (dsplibs.h) a packet that came */
void FAX2_t38_rx(void *obj, const void *ifp)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	if (f && f->t38)
		t30_t38_rx(f->t30, static_cast<const struct t38_ifp *>(ifp));
}

/* (dsplibs.h) a packet lost */
void FAX2_t38_lost(void *obj)
{
	Fax2 *f = static_cast<Fax2 *>(obj);
	if (f && f->t38)
		t30_t38_lost(f->t30);
}
