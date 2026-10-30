/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * app_dmodem.c - a data modem on an Asterisk channel
 *
 * The call is answered by a software modem (V.21 to V.34, and V.90 as the
 * digital modem when the channel is G.711 end to end); once the modems
 * are connected, the data goes to a TCP server (a BBS, a telnet host) or
 * to a program on a pty (pppd, a getty, a door) as dmodem.conf's profile
 * says.
 *
 * The modem is libdmodem (../dmodemd/dmodem.h): D-Modem's modem core
 * with the open data pumps, no binary-only parts.  The call between the
 * modem and the server or program is ../pbx/dmodem_bridge.c, as in
 * FreeSWITCH's mod_dmodem.
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2.
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/format_cache.h"
#include "asterisk/transcap.h"
#include "asterisk/frame.h"
#include "asterisk/localtime.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/paths.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#include "dmodem_bridge.h"

/*** DOCUMENTATION
	<application name="DModem" language="en_US">
		<synopsis>
			Answer the call with a data modem and connect it to a BBS or pppd.
		</synopsis>
		<syntax>
			<parameter name="profile">
				<para>The section of <filename>dmodem.conf</filename> to use,
				<literal>default</literal> if not given.</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="o">
						<para>Originate: the modem calls (waits for the answer
						tone) instead of answering, for a call Asterisk has
						placed to a remote modem.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Answers the channel with a software modem.  Once the modems are
			connected, the data goes to the profile's TCP server
			(<literal>mode=tcp</literal>) or program on a pty
			(<literal>mode=exec</literal>, pppd for one).</para>
			<para>The channel should be G.711 (ulaw or alaw) without transcoding,
			echo cancellation, jitter buffer or silence suppression on the way;
			V.90 needs the G.711 octets end to end.</para>
			<para>An ISDN data call (an unrestricted digital bearer, such as
			chan_dahdi's on a BRI or PRI span) runs the modem on a clear
			64 kbit/s channel instead (<literal>AT+MS=CLEAR</literal>, the
			channel's octets as they are, for another D-Modem's clear
			channel), unless the profile has <literal>clear=no</literal>;
			<literal>clear=yes</literal> does it for every call.  So does a
			SIP call in CLEARMODE (RFC 4040, with codec_clearmode loaded and
			<literal>allow = clearmode</literal> on the endpoint): the module
			reads and writes its octets in that format.  The link layer on
			the clear channel is X.75 unless the profile's
			<literal>init</literal> says otherwise (<literal>AT+B2</literal>).</para>
			<para>Sets these variables on exit:</para>
			<variablelist>
				<variable name="DMODEMSTATUS">
					<value name="NOCARRIER">The modems did not connect.</value>
					<value name="REMOTE">The remote modem went away.</value>
					<value name="LOCAL">The server or program ended the session.</value>
					<value name="HANGUP">The channel hung up.</value>
					<value name="FAX">A fax machine called (with fax receiving on in the profile): the fax receiver took the call.</value>
					<value name="ERROR">Configuration or system error.</value>
				</variable>
				<variable name="DMODEMRATE">The connect rate, transmit/receive bit/s.</variable>
			</variablelist>
		</description>
	</application>
 ***/

static const char app[] = "DModem";
static const char config_file[] = "dmodem.conf";

static const char synopsis[] =
	"Answer the call with a data modem, connect it to a BBS or pppd";
static const char descrip[] =
	"  DModem([profile[,options]]): a software modem answers the call (V.21\n"
	"to V.34, V.90 as the digital modem); once connected, the data goes to\n"
	"the TCP server (mode=tcp) or the program on a pty (mode=exec) of the\n"
	"profile in dmodem.conf ([default] if none is given).\n"
	"  Options: o - originate: the modem calls instead of answering.\n"
	"  Sets DMODEMSTATUS (NOCARRIER, REMOTE, LOCAL, HANGUP, FAX or ERROR) and\n"
	"DMODEMRATE (transmit/receive bit/s).  The channel should be G.711\n"
	"end to end, without echo cancellation, jitter buffer or silence\n"
	"suppression.\n";

#define LINE_MAX_SAMPLES 1600   /* in one frame, 200 ms */
#define IDLE_MS 200             /* no frame for this long: go on alone */

/* libdmodem's log lines: "err: " ones as warnings, the rest at verbose
   level 5 */
static void log_line(const char *line)
{
	if (!strncmp(line, "err: ", 5))
		ast_log(LOG_WARNING, "dmodem: %s", line + 5);
	else
		ast_verb(5, "dmodem: %s", line);
}

/* the bridge's log lines: a call's under its channel's name (call), the
   call's progress and its details at verbose level 4 */
static void bridge_log(void *call, enum dmbridge_level level, const char *line)
{
	char who[96];

	if (call)
		snprintf(who, sizeof(who), "%s: DModem", (const char *)call);
	else
		ast_copy_string(who, "dmodem", sizeof(who));
	if (level == DMBRIDGE_ERROR)
		ast_log(LOG_ERROR, "%s: %s\n", who, line);
	else if (level == DMBRIDGE_WARNING)
		ast_log(LOG_WARNING, "%s: %s\n", who, line);
	else
		ast_verb(4, "%s: %s\n", who, line);
}

/* yes, true, on, ... */
static int bridge_true(const char *value)
{
	return ast_true(value);
}

/* the program's fork: the child is ours to reap (dmbridge_call_end()), not
   Asterisk's reaper's, so that its pid stays ours until then */
static pid_t bridge_fork(void)
{
	return ast_safe_fork(1);
}

/* the TCP connect's thread, one of Asterisk's */
static int bridge_thread(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	return ast_pthread_create_detached(&t, NULL, fn, arg) ? -1 : 0;
}

static const struct dmbridge_host host = {
	.log = bridge_log,
	.is_true = bridge_true,
	.fork = bridge_fork,
	.fork_done = ast_safe_fork_cleanup,
	.thread = bridge_thread,
};

/* the defaults, then the fax options of [general] */
static void load_fax_general(struct ast_config *cfg, struct dmbridge_fax *f)
{
	struct ast_variable *v;
	char dir[256];

	snprintf(dir, sizeof(dir), "%s/fax", ast_config_AST_SPOOL_DIR);
	dmbridge_fax_defaults(f, dir);
	if (!cfg)
		return;
	for (v = ast_variable_browse(cfg, "general"); v; v = v->next)
		dmbridge_fax_option(&host, f, v->name, v->value);
}

/* the fax settings of [general] alone (for DModemSendFax/ReceiveFax) */
static void load_fax_settings(struct dmbridge_fax *f)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg = ast_config_load2(config_file, "app_dmodem",
						  flags);
	if (cfg == CONFIG_STATUS_FILEINVALID)
		cfg = NULL;
	load_fax_general(cfg, f);
	if (cfg)
		ast_config_destroy(cfg);
}

/* [name] of dmodem.conf into *p (fax: [general]'s, then the profile's
   own); -1 if there is no such profile or it lacks host and port (tcp)
   or command (exec) */
static int load_profile(const char *name, struct dmbridge_profile *p)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg;
	struct ast_variable *v;
	char where[96];

	dmbridge_profile_init(p);
	cfg = ast_config_load2(config_file, "app_dmodem", flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "cannot load %s\n", config_file);
		return -1;
	}
	if (!ast_category_browse_filtered(cfg, name, NULL, NULL)) {
		ast_log(LOG_ERROR, "%s: no profile [%s]\n", config_file, name);
		ast_config_destroy(cfg);
		return -1;
	}
	load_fax_general(cfg, &p->fax);
	snprintf(where, sizeof(where), "[%s]", name);
	for (v = ast_variable_browse(cfg, name); v; v = v->next)
		dmbridge_profile_option(&host, p, where, v->name, v->value);
	ast_config_destroy(cfg);
	return dmbridge_profile_check(&host, p, where);
}

/* the channel's law: A-law if it has alaw and not ulaw, else mu-law */
static enum dmodem_law channel_law(struct ast_channel *chan)
{
	struct ast_format_cap *caps = ast_channel_nativeformats(chan);
	if (caps && ast_format_cap_iscompatible_format(caps,
	    ast_format_alaw) != AST_FORMAT_CMP_NOT_EQUAL &&
	    ast_format_cap_iscompatible_format(caps,
	    ast_format_ulaw) == AST_FORMAT_CMP_NOT_EQUAL)
		return DMODEM_LAW_ALAW;
	return DMODEM_LAW_ULAW;
}

/* the CLEARMODE format (codec_clearmode.so) with a reference, when it
   is the channel's own (first of its native formats): a SIP call in
   RFC 4040's clear channel; NULL else */
static struct ast_format *clearmode_format(struct ast_channel *chan)
{
	struct ast_format_cap *caps = ast_channel_nativeformats(chan);
	struct ast_format *cm, *first;
	int same;

	if (!caps || !ast_format_cap_count(caps))
		return NULL;
	cm = ast_format_cache_get("clearmode");
	if (!cm)
		return NULL;
	first = ast_format_cap_get_format(caps, 0);
	same = first && ast_format_cmp(first, cm) == AST_FORMAT_CMP_EQUAL;
	ao2_cleanup(first);
	if (same)
		return cm;
	ao2_ref(cm, -1);
	return NULL;
}

/* n octets of the line through the modem, its answer written back */
static int line_io(struct ast_channel *chan, struct dmodem *d,
		   struct ast_format *fmt, const unsigned char *in, int n,
		   unsigned char *out)
{
	struct ast_frame wf = {
		.frametype = AST_FRAME_VOICE,
		.src = app,
	};

	if (n > LINE_MAX_SAMPLES)
		n = LINE_MAX_SAMPLES;
	dmodem_process_g711(d, in, out + AST_FRIENDLY_OFFSET, n);
	wf.subclass.format = fmt;
	wf.data.ptr = out + AST_FRIENDLY_OFFSET;
	wf.offset = AST_FRIENDLY_OFFSET;
	wf.datalen = n;
	wf.samples = n;
	ast_write(chan, &wf);
	return n;
}

/*
 *    fax
 */

/* where a received fax goes: <dir>/<date>-<time>-<caller>-<uniqueid>.tif */
static void fax_file_name(struct ast_channel *chan, const char *dir,
			  char *path, size_t len)
{
	char when[32], caller[32] = "", id[64];
	const char *num = S_COR(ast_channel_caller(chan)->id.number.valid,
				ast_channel_caller(chan)->id.number.str, "");
	struct timeval now = ast_tvnow();
	struct ast_tm tm;
	size_t k, n = 0;

	ast_localtime(&now, &tm, NULL);
	ast_strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &tm);
	for (k = 0; num[k] && n < sizeof(caller) - 1; k++)
		if (isalnum((unsigned char)num[k]) || num[k] == '+')
			caller[n++] = num[k];
	caller[n] = 0;
	ast_copy_string(id, ast_channel_uniqueid(chan), sizeof(id));
	for (k = 0; id[k]; k++)
		if (!isalnum((unsigned char)id[k]) && id[k] != '.')
			id[k] = '_';
	snprintf(path, len, "%s/%s-%s-%s.tif", dir, when,
		 *caller ? caller : "unknown", id);
}

/*
 *    T.38: the channel's (chan_pjsip with t38_udptl): its re-INVITEs as
 *    AST_CONTROL_T38_PARAMETERS, the IFP packets as AST_FRAME_MODEM
 *    frames (the UDPTL the channel's), as res_fax has them
 */

#define T38_ASK_MS 6000         /* our request's answer waited for */
#define T38_TICK_MS 20          /* the fax machine's time in steps of */

/* our side of T.38: version, the longest IFP packet we take, the fax's
   top rate, TCF sent (not made locally) */
static void t38_ours(struct ast_control_t38_parameters *t, int version,
		     int max_bps, enum ast_control_t38 what)
{
	memset(t, 0, sizeof(*t));
	t->request_response = what;
	t->version = version;
	t->max_ifp = 400;
	t->rate = max_bps >= 14400 ? AST_T38_RATE_14400 :
		max_bps >= 12000 ? AST_T38_RATE_12000 :
		max_bps >= 9600 ? AST_T38_RATE_9600 :
		max_bps >= 7200 ? AST_T38_RATE_7200 :
		max_bps >= 4800 ? AST_T38_RATE_4800 : AST_T38_RATE_2400;
	t->rate_management = AST_T38_RATE_MANAGEMENT_TRANSFERRED_TCF;
}

/* the frame's T.38 parameters if it is AST_CONTROL_T38_PARAMETERS, else
   NULL */
static const struct ast_control_t38_parameters *t38_frame(
	const struct ast_frame *fr)
{
	if (fr->frametype != AST_FRAME_CONTROL ||
	    fr->subclass.integer != AST_CONTROL_T38_PARAMETERS ||
	    fr->datalen != sizeof(struct ast_control_t38_parameters))
		return NULL;
	return fr->data.ptr;
}

/* an IFP packet of the fax machine's to the channel */
static void t38_send(void *ctx, const unsigned char *pkt, int len)
{
	struct ast_frame wf = {
		.frametype = AST_FRAME_MODEM,
		.subclass.integer = AST_MODEM_T38,
		.src = app,
	};
	wf.data.ptr = (void *)pkt;
	wf.datalen = len;
	ast_write(ctx, &wf);
}

/* back to audio at the end of a fax over T.38 (as res_fax does), its
   answer waited for: 5 s at most */
static void t38_stop(struct ast_channel *chan)
{
	struct ast_control_t38_parameters t = {
		.request_response = AST_T38_REQUEST_TERMINATE };
	struct timeval start = ast_tvnow();
	int ms;

	if (ast_indicate_data(chan, AST_CONTROL_T38_PARAMETERS, &t,
			      sizeof(t)))
		return;
	while ((ms = ast_remaining_ms(start, 5000)) > 0) {
		const struct ast_control_t38_parameters *tp;
		struct ast_frame *fr;
		int over;

		if (ast_waitfor(chan, ms) <= 0 || !(fr = ast_read(chan)))
			return;
		tp = t38_frame(fr);
		over = tp && tp->request_response != AST_T38_REQUEST_NEGOTIATE;
		ast_frfree(fr);
		if (over)
			return;
	}
}

/* n octets of silence through the fax machine (NULL: not it, only the
   silence out) to the channel */
static void fax_audio(struct ast_channel *chan, struct ast_format *fmt,
		      enum dmodem_law law, struct dmodem_fax *f,
		      const unsigned char *in, int n, unsigned char *out)
{
	struct ast_frame wf = { .frametype = AST_FRAME_VOICE, .src = app };

	if (n > LINE_MAX_SAMPLES)
		n = LINE_MAX_SAMPLES;
	if (f)
		dmodem_fax_process_g711(f, in, out + AST_FRIENDLY_OFFSET, n);
	else
		memset(out + AST_FRIENDLY_OFFSET,
		       law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF, n);
	wf.subclass.format = fmt;
	wf.data.ptr = out + AST_FRIENDLY_OFFSET;
	wf.offset = AST_FRIENDLY_OFFSET;
	wf.datalen = wf.samples = n;
	ast_write(chan, &wf);
}

/* a fax call on the channel until it ends; -1 if the channel hung up.

   With fax_t38 on a channel that can (T.38 not refused), the fax goes
   over to T.38 when the channel does, before a page has gone (the fax
   machine anew on IFP packets, its time by the clock): a receiver asks
   for it as ReceiveFAX does, and waits for the answer (T38_ASK_MS at
   most, silence on the line) before it starts on the audio; a sender
   takes it when the other end asks.  A request that came before (the
   channel negotiating) is had again, and answered */
static int run_fax(struct ast_channel *chan, struct ast_format *fmt,
		   enum dmodem_law law, enum dmodem_fax_dir dir,
		   const char *file, const struct dmbridge_fax *fs,
		   unsigned char *out)
{
	struct dmodem_fax_config cfg = { 0 };
	struct dmodem_fax_status st = { 0 };
	struct dmodem_fax *f;
	const char *name = ast_channel_name(chan);
	enum ast_t38_state ts = ast_channel_get_t38_state(chan);
	struct timeval t0 = { 0 }, asked = { 0 };
	long long samples = 0;
	char buf[32];
	int res = 0, t38 = 0, asking = 0, our_version = 0;
	int may_switch = fs->t38 && (ts == T38_STATE_UNKNOWN ||
				     ts == T38_STATE_NEGOTIATING);

	cfg.law = law;
	cfg.dir = dir;
	cfg.file = file;
	cfg.ident = fs->ident;
	cfg.max_bps = fs->max_bps;
	cfg.no_ecm = !fs->ecm;
	pbx_builtin_setvar_helper(chan, "DMODEMFAXFILE", file);
	f = dmodem_fax_create(&cfg);
	if (!f) {
		ast_log(LOG_WARNING, "%s: cannot %s fax file %s\n", name,
			dir == DMODEM_FAX_SEND ? "read" : "use", file);
		pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS", "FAILED");
		pbx_builtin_setvar_helper(chan, "DMODEMFAXRESULT",
					  "no document");
		return 0;
	}
	ast_verb(3, "%s: DModem fax: %s %s\n", name,
		 dir == DMODEM_FAX_SEND ? "sending" : "receiving", file);
	if (may_switch && ts == T38_STATE_NEGOTIATING) {
		/* the other end's request again, to answer */
		struct ast_control_t38_parameters t = {
			.request_response = AST_T38_REQUEST_PARMS };
		if (ast_indicate_data(chan, AST_CONTROL_T38_PARAMETERS, &t,
				      sizeof(t)) != AST_T38_REQUEST_PARMS)
			may_switch = 0;
	} else if (may_switch && dir == DMODEM_FAX_RECEIVE) {
		struct ast_control_t38_parameters t;
		t38_ours(&t, 0, fs->max_bps, AST_T38_REQUEST_NEGOTIATE);
		if (!ast_indicate_data(chan, AST_CONTROL_T38_PARAMETERS, &t,
				       sizeof(t))) {
			asking = 1;
			asked = ast_tvnow();
			ast_verb(3, "%s: DModem fax: T.38 asked for\n", name);
		}
	}
	for (;;) {
		const struct ast_control_t38_parameters *tp;
		struct ast_frame *fr = NULL;
		int n = ast_waitfor(chan, t38 ? T38_TICK_MS : IDLE_MS);

		if (n < 0 || (n > 0 && !(fr = ast_read(chan)))) {
			dmodem_fax_hangup(f);
			res = -1;
			break;
		}
		if (asking && ast_tvdiff_ms(ast_tvnow(), asked) > T38_ASK_MS)
			asking = 0;
		if (!fr) {
			/* nothing from the line: on alone, on silence */
			if (!t38) {
				unsigned char sil[160];
				memset(sil, law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF,
				       sizeof(sil));
				fax_audio(chan, fmt, law, asking ? NULL : f, sil,
					  sizeof(sil), out);
			}
		} else if ((tp = t38_frame(fr))) {
			struct ast_control_t38_parameters t;
			switch (tp->request_response) {
			case AST_T38_REQUEST_NEGOTIATE:
				/* the other end asks: yes until a page has
				   gone (and again once on T.38) */
				if (!t38)
					our_version = 3;
				t38_ours(&t, our_version, fs->max_bps,
					 may_switch || t38 ? AST_T38_NEGOTIATED :
					 AST_T38_REFUSED);
				ast_indicate_data(chan,
						  AST_CONTROL_T38_PARAMETERS,
						  &t, sizeof(t));
				break;
			case AST_T38_NEGOTIATED:
				if (t38)
					break;
				/* over to T.38: the fax machine anew */
				dmodem_fax_destroy(f);
				cfg.t38 = 1;
				cfg.t38_ifp = 1;
				cfg.t38_version = (int)tp->version < our_version ?
					(int)tp->version : our_version;
				cfg.t38_max_datagram = tp->max_ifp;
				cfg.t38_send = t38_send;
				cfg.t38_ctx = chan;
				f = dmodem_fax_create(&cfg);
				if (!f) {
					ast_log(LOG_WARNING, "%s: cannot %s fax "
						"file %s\n", name,
						dir == DMODEM_FAX_SEND ? "read" :
						"use", file);
					pbx_builtin_setvar_helper(chan,
						"DMODEMFAXSTATUS", "FAILED");
					ast_frfree(fr);
					t38_stop(chan);
					return 0;
				}
				ast_verb(3, "%s: DModem fax: T.38 (version %d, "
					 "IFP packets up to %u octets)\n", name,
					 cfg.t38_version, tp->max_ifp);
				t38 = 1;
				asking = may_switch = 0;
				t0 = ast_tvnow();
				samples = 0;
				break;
			case AST_T38_REFUSED:
				asking = 0;
				break;
			case AST_T38_TERMINATED:
			case AST_T38_REQUEST_TERMINATE:
				/* the other end back on audio: the fax
				   over T.38 ends */
				if (t38)
					dmodem_fax_hangup(f);
				asking = 0;
				break;
			default:
				break;
			}
		} else if (t38) {
			if (fr->frametype == AST_FRAME_MODEM &&
			    fr->subclass.integer == AST_MODEM_T38 &&
			    fr->datalen > 0)
				dmodem_fax_t38_rx(f, fr->data.ptr, fr->datalen);
		} else if (fr->frametype == AST_FRAME_VOICE &&
			   fr->datalen > 0 &&
			   ast_format_cmp(fr->subclass.format, fmt) ==
			   AST_FORMAT_CMP_EQUAL) {
			/* a frame out for each frame in (T.38 asked for:
			   silence, the fax machine waits) */
			fax_audio(chan, fmt, law, asking ? NULL : f,
				  fr->data.ptr, fr->datalen, out);
		}
		if (fr)
			ast_frfree(fr);
		if (t38) {
			/* the time since, by the clock (a second at most) */
			long long due = ast_tvdiff_us(ast_tvnow(), t0) * 8 /
				1000;
			if (due > samples) {
				dmodem_fax_t38_time(f, due - samples > 8000 ?
						    8000 : (int)(due - samples));
				samples = due;
			}
		}
		dmodem_fax_status(f, &st);
		if (may_switch && (st.pages || st.complete))
			/* a page has gone on audio: it stays */
			may_switch = 0;
		if (st.done)
			break;
	}
	dmodem_fax_status(f, &st);
	dmodem_fax_destroy(f);
	if (t38 && res == 0)
		t38_stop(chan);
	ast_verb(3, "%s: DModem fax %s: %s, %d pages at %d bit/s%s%s, remote '%s'\n",
		 name, dir == DMODEM_FAX_SEND ? "sent" : "received", st.result,
		 st.pages, st.bps, st.ecm ? " (ECM)" : "",
		 t38 ? " over T.38" : "", st.remote_ident);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS",
				  st.ok ? "SUCCESS" : "FAILED");
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRESULT", st.result);
	snprintf(buf, sizeof(buf), "%d", st.pages);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXPAGES", buf);
	snprintf(buf, sizeof(buf), "%d", st.bps);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRATE", buf);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXREMOTE", st.remote_ident);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXECM", st.ecm ? "yes" : "no");
	pbx_builtin_setvar_helper(chan, "DMODEMFAXT38", t38 ? "yes" : "no");
	return res;
}

/* DModem([profile[,options]]): the modem on the channel until the call,
   the session or the time to connect ends (a calling fax machine goes
   to run_fax()); DMODEMSTATUS and DMODEMRATE set; -1 only when the
   channel hung up */
static int dmodem_exec(struct ast_channel *chan, const char *data)
{
	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(profile); AST_APP_ARG(options););
	struct dmbridge_profile p;
	struct dmbridge_call c;
	enum dmodem_law law;
	struct ast_format *fmt, *clear_fmt = NULL;
	char *parse, name[64];
	const char *status = "ERROR";
	unsigned char out[AST_FRIENDLY_OFFSET + LINE_MAX_SAMPLES];
	int res = 0, orig, t38_fax = 0;

	dmbridge_call_init(&c);
	parse = ast_strdupa(S_OR(data, ""));
	AST_STANDARD_APP_ARGS(args, parse);
	if (ast_strlen_zero(args.profile))
		args.profile = "default";
	if (load_profile(args.profile, &p))
		goto out;
	orig = args.options && strchr(args.options, 'o');

	/* an ISDN data call (chan_dahdi's on a BRI or PRI: an unrestricted
	   digital bearer): the channel's octets as they come, in its own
	   format, for the modem's clear channel */
	c.digital = ast_channel_transfercapability(chan) ==
		AST_TRANS_CAP_DIGITAL;
	law = p.law && !(c.digital && p.clear != DMBRIDGE_CLEAR_NO) ?
		p.law : channel_law(chan);
	fmt = law == DMODEM_LAW_ALAW ? ast_format_alaw : ast_format_ulaw;
	/* a SIP call in CLEARMODE: the same, in that format (its octets
	   are no audio, whatever clear says) */
	if ((clear_fmt = clearmode_format(chan))) {
		if (p.clear == DMBRIDGE_CLEAR_NO)
			ast_log(LOG_NOTICE, "%s: a CLEARMODE call: clear all the "
				"same\n", ast_channel_name(chan));
		p.clear = DMBRIDGE_CLEAR_YES;
		c.digital = 1;
		law = DMODEM_LAW_ALAW;
		fmt = clear_fmt;
	}
	if (ast_channel_state(chan) != AST_STATE_UP && ast_answer(chan)) {
		status = "HANGUP";
		res = -1;
		goto out;
	}
	if (ast_set_read_format(chan, fmt) || ast_set_write_format(chan, fmt)) {
		ast_log(LOG_WARNING, "%s: cannot set %s\n",
			ast_channel_name(chan), ast_format_get_name(fmt));
		goto out;
	}

	ast_copy_string(name, ast_channel_name(chan), sizeof(name));
	if (dmbridge_call_start(&c, &host, name, &p, name, law, orig))
		goto out;
	ast_verb(3, "%s: DModem %s (profile %s, %s)\n", name,
		 orig ? "calling" : "answering", args.profile,
		 ast_format_get_name(fmt));

	for (;;) {
		const struct ast_control_t38_parameters *tp;
		struct ast_frame *f;
		enum dmbridge_event ev;
		int n = ast_waitfor(chan, IDLE_MS), k = 0;

		if (n < 0) {
			c.status = "HANGUP";
			res = -1;
			break;
		}
		if (n == 0) {
			/* nothing from the line (no clock on the channel,
			   silence suppression): keep the modem going on
			   silence */
			unsigned char sil[160];
			memset(sil, law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF,
			       sizeof(sil));
			k = line_io(chan, c.d, fmt, sil, sizeof(sil), out);
		} else {
			f = ast_read(chan);
			if (!f) {
				c.status = "HANGUP";
				res = -1;
				break;
			}
			/* a frame out for each frame in: the line's clock */
			if (f->frametype == AST_FRAME_VOICE &&
			    f->datalen > 0 &&
			    ast_format_cmp(f->subclass.format, fmt) ==
			    AST_FORMAT_CMP_EQUAL)
				k = line_io(chan, c.d, fmt, f->data.ptr,
					    f->datalen, out);
			else if ((tp = t38_frame(f)) &&
				 tp->request_response ==
				 AST_T38_REQUEST_NEGOTIATE) {
				/* T.38 asked for before the modems are
				   connected: a fax machine (a gateway's) */
				if (!orig && !c.online && p.fax.on &&
				    p.fax.t38) {
					t38_fax = 1;
					c.status = "FAX";
				} else {
					struct ast_control_t38_parameters t;
					t38_ours(&t, 0, p.fax.max_bps,
						 AST_T38_REFUSED);
					ast_indicate_data(chan,
						AST_CONTROL_T38_PARAMETERS, &t,
						sizeof(t));
				}
			}
			ast_frfree(f);
		}

		ev = t38_fax ? DMBRIDGE_FAX : dmbridge_call_step(&c, k);
		if (ev == DMBRIDGE_CONNECTED) {
			char connected[24];
			const char *env[] = {
				"DMODEM_CHANNEL", name,
				"DMODEM_PROFILE", args.profile,
				"DMODEM_RATE", c.rate,
				"DMODEM_CALLERID",
				S_COR(ast_channel_caller(chan)->id.number.valid,
				      ast_channel_caller(chan)->id.number.str, ""),
				"DMODEM_DIALED",
				S_OR(ast_channel_dialed(chan)->number.str,
				     ast_channel_exten(chan)),
				"DMODEM_CONNECTED", connected,
				NULL
			};
			snprintf(connected, sizeof(connected), "%lld",
				 (long long)time(NULL));
			pbx_builtin_setvar_helper(chan, "DMODEMRATE", c.rate);
			ast_verb(3, "%s: DModem connected at %s bit/s\n", name,
				 c.rate);
			ev = dmbridge_call_serve(&c, env);
		}
		if (ev == DMBRIDGE_FAX) {
			/* a fax machine calling: a fax receiver instead of
			   the modem */
			char path[1024];
			ast_mkdir(p.fax.dir, 0755);
			fax_file_name(chan, p.fax.dir, path, sizeof(path));
			ast_verb(3, "%s: DModem: a fax machine is calling%s\n",
				 name, t38_fax ? " (T.38)" : "");
			if (run_fax(chan, fmt, law, DMODEM_FAX_RECEIVE, path,
				    &p.fax, out) < 0)
				res = -1;
			break;
		}
		if (ev == DMBRIDGE_END)
			break;
	}
	dmbridge_call_hangup(&c);
	status = c.status;
	ast_verb(3, "%s: DModem done: %s\n", name, status);

out:
	dmbridge_call_end(&c);
	ao2_cleanup(clear_fmt);
	pbx_builtin_setvar_helper(chan, "DMODEMSTATUS", status);
	return res;
}

static const char app_send[] = "DModemSendFax";
static const char send_synopsis[] = "Send a fax (TIFF file) with DModem's fax";
static const char send_descrip[] =
	"  DModemSendFax(file[,ident]): sends the TIFF file as a group 3 fax\n"
	"on a call placed to a fax machine (it waits for the machine's DIS).\n"
	"ident: our number (TSI), else fax_ident of dmodem.conf [general].\n"
	"  Sets DMODEMFAXSTATUS (SUCCESS or FAILED), DMODEMFAXRESULT,\n"
	"DMODEMFAXPAGES, DMODEMFAXRATE, DMODEMFAXREMOTE, DMODEMFAXECM (yes\n"
	"when the call had error correction mode), DMODEMFAXFILE.\n";
static const char app_recv[] = "DModemReceiveFax";
static const char recv_synopsis[] = "Receive a fax into a TIFF file with DModem's fax";
static const char recv_descrip[] =
	"  DModemReceiveFax(file[,ident]): answers the call as a group 3 fax\n"
	"machine and writes what it receives to the TIFF file (none if no page\n"
	"came).  ident: our number (CSI), else fax_ident of [general].\n"
	"  Sets the same variables as DModemSendFax.  DModem() receives faxes\n"
	"by itself when a fax machine calls (fax = yes, the default).\n";

/* DModemSendFax / DModemReceiveFax (file[,ident]) in direction dir on
   the channel's law; -1 if the channel hung up */
static int fax_exec(struct ast_channel *chan, const char *data,
		    enum dmodem_fax_dir dir)
{
	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(file); AST_APP_ARG(ident););
	struct dmbridge_fax fs;
	enum dmodem_law law;
	struct ast_format *fmt;
	unsigned char out[AST_FRIENDLY_OFFSET + LINE_MAX_SAMPLES];
	char *parse = ast_strdupa(S_OR(data, ""));

	AST_STANDARD_APP_ARGS(args, parse);
	if (ast_strlen_zero(args.file)) {
		ast_log(LOG_WARNING, "%s needs a file name\n",
			dir == DMODEM_FAX_SEND ? app_send : app_recv);
		pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS", "FAILED");
		return 0;
	}
	load_fax_settings(&fs);
	if (!ast_strlen_zero(args.ident))
		ast_copy_string(fs.ident, args.ident, sizeof(fs.ident));
	law = channel_law(chan);
	fmt = law == DMODEM_LAW_ALAW ? ast_format_alaw : ast_format_ulaw;
	if (ast_channel_state(chan) != AST_STATE_UP && ast_answer(chan))
		return -1;
	if (ast_set_read_format(chan, fmt) || ast_set_write_format(chan, fmt)) {
		ast_log(LOG_WARNING, "%s: cannot set %s\n",
			ast_channel_name(chan), ast_format_get_name(fmt));
		pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS", "FAILED");
		return 0;
	}
	return run_fax(chan, fmt, law, dir, args.file, &fs, out);
}

/* DModemSendFax() */
static int send_exec(struct ast_channel *chan, const char *data)
{
	return fax_exec(chan, data, DMODEM_FAX_SEND);
}

/* DModemReceiveFax() */
static int recv_exec(struct ast_channel *chan, const char *data)
{
	return fax_exec(chan, data, DMODEM_FAX_RECEIVE);
}

/* libdmodem's log level: debug of [general] (0 if none) */
static void load_general(void)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg = ast_config_load2(config_file, "app_dmodem",
						  flags);
	const char *v;

	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID)
		return;
	v = ast_variable_retrieve(cfg, "general", "debug");
	dmodem_set_debug(v ? atoi(v) : 0);
	ast_config_destroy(cfg);
}

/* the three applications unregistered */
static int unload_module(void)
{
	int r = ast_unregister_application(app);
	r |= ast_unregister_application(app_send);
	r |= ast_unregister_application(app_recv);
	return r;
}

/* the log hook, the pumps, [general], the three applications */
static int load_module(void)
{
	dmodem_set_log(log_line);
	dmodem_init();
	load_general();
	/* built out of the tree, the XML documentation is not in Asterisk's:
	   the text here for "core show application DModem" */
	if (ast_register_application(app, dmodem_exec, synopsis, descrip) ||
	    ast_register_application(app_send, send_exec, send_synopsis,
				     send_descrip) ||
	    ast_register_application(app_recv, recv_exec, recv_synopsis,
				     recv_descrip)) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

/* the log level again */
static int reload_module(void)
{
	load_general();
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Software data modem and fax (BBS, PPP, G3 fax)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);
