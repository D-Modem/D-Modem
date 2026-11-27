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
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/format_cache.h"
#include "asterisk/transcap.h"
#include "asterisk/frame.h"
#include "asterisk/indications.h"
#include "asterisk/localtime.h"
#include "asterisk/linkedlists.h"
#include "asterisk/logger.h"
#include "asterisk/manager.h"
#include "asterisk/max_forwards.h"
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
			<para>With <literal>voice=exten@context</literal> in the profile,
			the modem listens first (<literal>voice_listen</literal> seconds):
			a person calling is put through to that extension (an IVR, a
			voicemail box), and the modem answers once the extension's leg
			ends, or once the caller's modem asks for data with its calling
			tone.  With <literal>voice_dsvd=yes</literal> (or
			<literal>open</literal>), a DSVD (V.70) or ASVD (V.61) modem's
			voice channel goes to the extension while the data goes on, and
			closes when the extension hangs up.</para>
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
				<variable name="DMODEMMODULATION">The modulation of the connection: V34, V90, V92, K56Flex...</variable>
				<variable name="DMODEMPROTOCOL">The error correction of the connection: LAPM, MNP or NONE.</variable>
				<variable name="DMODEMCOMPRESSION">The data compression of the connection: V42bis, V44, MNP5 or NONE.</variable>
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

/*
 *    DMODEMFAXOPT(): a channel's own fax settings, over [general]'s and a
 *    profile's.  They are the channel variables DMODEMFAXOPT_<ITEM>
 *    (DMODEMFAXOPT_MINRATE and the others), which dmodem-ari reads too
 */

/* the channel variable of a setting: DMODEMFAXOPT_ and the item in
   capitals */
static void faxopt_var(const char *item, char *buf, size_t len)
{
	size_t k, n = snprintf(buf, len, "DMODEMFAXOPT_");

	for (k = 0; item[k] && n + 1 < len; k++)
		buf[n++] = toupper((unsigned char)item[k]);
	buf[n < len ? n : len - 1] = 0;
}

/* the channel's DMODEMFAXOPT_<ITEM> variables over *f (one that is not
   valid is left out, with a warning) */
static void faxopt_apply(struct ast_channel *chan, struct dmbridge_fax *f)
{
	char var[64];
	const char *v;
	size_t k;

	ast_channel_lock(chan);
	for (k = 0; dmbridge_faxopt_items[k]; k++) {
		faxopt_var(dmbridge_faxopt_items[k], var, sizeof(var));
		v = pbx_builtin_getvar_helper(chan, var);
		if (!ast_strlen_zero(v) &&
		    dmbridge_faxopt(&host, f, dmbridge_faxopt_items[k], v))
			ast_log(LOG_WARNING, "%s: %s: not valid: '%s'\n",
				ast_channel_name(chan), var, v);
	}
	ast_channel_unlock(chan);
}

/* the results DMODEMFAXOPT() reads: its item, the channel variable */
static const struct {
	const char *item;
	const char *var;
} faxopt_results[] = {
	{ "status", "DMODEMFAXSTATUS" },
	{ "statusstr", "DMODEMFAXRESULT" },
	{ "error", "DMODEMFAXERROR" },
	{ "pages", "DMODEMFAXPAGES" },
	{ "rate", "DMODEMFAXRATE" },
	{ "resolution", "DMODEMFAXRESOLUTION" },
	{ "remotestationid", "DMODEMFAXREMOTE" },
	{ "sessionid", "DMODEMFAXSESSION" },
	{ "filenames", "DMODEMFAXFILE" },
};

/* DMODEMFAXOPT(item) read: a setting as the next fax will have it, or
   a result of the last one */
static int faxopt_read(struct ast_channel *chan, const char *cmd,
		       char *data, char *buf, size_t len)
{
	struct dmbridge_fax f;
	const char *v;
	size_t k;

	*buf = 0;
	if (!chan || ast_strlen_zero(data))
		return -1;
	for (k = 0; k < ARRAY_LEN(faxopt_results); k++) {
		if (strcasecmp(data, faxopt_results[k].item))
			continue;
		ast_channel_lock(chan);
		v = pbx_builtin_getvar_helper(chan, faxopt_results[k].var);
		ast_copy_string(buf, S_OR(v, ""), len);
		ast_channel_unlock(chan);
		return 0;
	}
	if (!strcasecmp(data, "filename")) {
		/* the first file of the last fax */
		char *amp;
		ast_channel_lock(chan);
		v = pbx_builtin_getvar_helper(chan, "DMODEMFAXFILE");
		ast_copy_string(buf, S_OR(v, ""), len);
		ast_channel_unlock(chan);
		if ((amp = strchr(buf, '&')))
			*amp = 0;
		return 0;
	}
	load_fax_settings(&f);
	faxopt_apply(chan, &f);
	if (!strcasecmp(data, "ecm"))
		ast_copy_string(buf, f.ecm ? "yes" : "no", len);
	else if (!strcasecmp(data, "maxrate"))
		snprintf(buf, len, "%d", f.max_bps);
	else if (!strcasecmp(data, "minrate"))
		snprintf(buf, len, "%d", f.min_bps);
	else if (!strcasecmp(data, "t38timeout"))
		snprintf(buf, len, "%d", f.t38_timeout);
	else if (!strcasecmp(data, "localstationid"))
		ast_copy_string(buf, f.ident, len);
	else if (!strcasecmp(data, "headerinfo"))
		ast_copy_string(buf, f.header, len);
	else if (!strcasecmp(data, "modems"))
		dmbridge_fax_modems_text(f.modems, buf, len);
	else {
		ast_log(LOG_WARNING, "DMODEMFAXOPT: unknown item '%s'\n",
			data);
		return -1;
	}
	return 0;
}

/* DMODEMFAXOPT(item)=value: a setting for the channel's next faxes, in
   its variable DMODEMFAXOPT_<ITEM> once it is valid */
static int faxopt_write(struct ast_channel *chan, const char *cmd,
			char *data, const char *value)
{
	struct dmbridge_fax f;
	char var[64];

	if (!chan || ast_strlen_zero(data) || !value)
		return -1;
	dmbridge_fax_defaults(&f, "");
	if (dmbridge_faxopt(&host, &f, data, value)) {
		ast_log(LOG_WARNING, "DMODEMFAXOPT(%s): cannot set '%s'\n",
			data, value);
		return -1;
	}
	faxopt_var(data, var, sizeof(var));
	pbx_builtin_setvar_helper(chan, var, value);
	return 0;
}

static struct ast_custom_function faxopt_function = {
	.name = "DMODEMFAXOPT",
	.read = faxopt_read,
	.write = faxopt_write,
};

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

/* how a fax call runs: the applications' options */
struct fax_run {
	int debug;              /* d: each phase in the log */
	int audio_only;         /* F: no T.38 */
	int ask_t38;            /* z: ask for T.38 (a receiver always asks) */
	int need_t38;           /* T: T.38 or no fax */
	int status_events;      /* s: AMI DModemFaxStatus events */
};

AST_APP_OPTIONS(fax_app_options, BEGIN_OPTIONS
	AST_APP_OPTION('d', 1 << 0),
	AST_APP_OPTION('f', 1 << 1),
	AST_APP_OPTION('F', 1 << 2),
	AST_APP_OPTION('z', 1 << 3),
	AST_APP_OPTION('T', 1 << 4),
	AST_APP_OPTION('s', 1 << 5),
END_OPTIONS);

static int fax_session_seq;     /* the last fax session's number */

/*
 *    the fax sessions: the ones that run (for the CLI and AMI), and the
 *    totals since the module loaded
 */

/* a fax that runs: what "dmodem fax show session" tells */
struct fax_session {
	int id;
	char channel[AST_CHANNEL_NAME];
	char file[1024];
	int sending;
	struct timeval start;
	struct dmodem_fax_status st;    /* the last status (st.result and
					   st.phase point into libdmodem's
					   constant strings) */
	char remote[21];
	char local[21];
	int t38;
	AST_LIST_ENTRY(fax_session) list;
};

static AST_LIST_HEAD_STATIC(fax_sessions, fax_session);

/* the totals, with fax_sessions locked */
static struct {
	int tx_attempts, rx_attempts;
	int completed, failed;
	/* the failures by code (DMODEMFAXERROR) */
	struct {
		char code[16];
		int n;
	} reasons[16];
} fax_stats;

/* a new session in the list (NULL when out of memory: the fax runs,
   not in the list) */
static struct fax_session *fax_session_add(struct ast_channel *chan, int id,
					   int sending, const char *file,
					   const char *local)
{
	struct fax_session *fs = ast_calloc(1, sizeof(*fs));

	if (!fs)
		return NULL;
	fs->id = id;
	ast_copy_string(fs->channel, ast_channel_name(chan),
			sizeof(fs->channel));
	ast_copy_string(fs->file, file, sizeof(fs->file));
	ast_copy_string(fs->local, S_OR(local, ""), sizeof(fs->local));
	fs->sending = sending;
	fs->start = ast_tvnow();
	fs->st.result = "in progress";
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_INSERT_TAIL(&fax_sessions, fs, list);
	if (sending)
		fax_stats.tx_attempts++;
	else
		fax_stats.rx_attempts++;
	AST_LIST_UNLOCK(&fax_sessions);
	return fs;
}

/* the session's status now (fs may be NULL) */
static void fax_session_update(struct fax_session *fs,
			       const struct dmodem_fax_status *st, int t38)
{
	if (!fs)
		return;
	AST_LIST_LOCK(&fax_sessions);
	fs->st = *st;
	ast_copy_string(fs->remote, st->remote_ident, sizeof(fs->remote));
	fs->t38 = t38;
	AST_LIST_UNLOCK(&fax_sessions);
}

/* a failure of the code counted */
static void fax_stats_reason(const char *code)
{
	size_t k;

	for (k = 0; k < ARRAY_LEN(fax_stats.reasons); k++) {
		if (!fax_stats.reasons[k].code[0])
			ast_copy_string(fax_stats.reasons[k].code, code,
					sizeof(fax_stats.reasons[k].code));
		if (!strcmp(fax_stats.reasons[k].code, code)) {
			fax_stats.reasons[k].n++;
			return;
		}
	}
}

/* the session out of the list, its result in the totals (fs may be
   NULL: the result counted all the same) */
static void fax_session_end(struct fax_session *fs, int ok, const char *error)
{
	AST_LIST_LOCK(&fax_sessions);
	if (ok) {
		fax_stats.completed++;
	} else {
		fax_stats.failed++;
		fax_stats_reason(S_OR(error, "UNKNOWN"));
	}
	if (fs)
		AST_LIST_REMOVE(&fax_sessions, fs, list);
	AST_LIST_UNLOCK(&fax_sessions);
	ast_free(fs);
}

/* the seconds since the session started */
static long fax_session_secs(const struct fax_session *fs)
{
	return (long)(ast_tvdiff_ms(ast_tvnow(), fs->start) / 1000);
}

/* "dmodem fax show sessions": one line for each fax that runs */
static char *cli_fax_sessions(struct ast_cli_entry *e, int cmd,
			      struct ast_cli_args *a)
{
	struct fax_session *fs;
	int n = 0;

	if (cmd == CLI_INIT) {
		e->command = "dmodem fax show sessions";
		e->usage = "Usage: dmodem fax show sessions\n"
			"       The DModem faxes that run now.\n";
		return NULL;
	}
	if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	ast_cli(a->fd, "%-6s %-32s %-4s %-5s %-5s %-6s %-6s %s\n", "ID",
		"Channel", "Dir", "T.38", "Pages", "Rate", "Secs", "Phase");
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_TRAVERSE(&fax_sessions, fs, list) {
		ast_cli(a->fd, "%-6d %-32.32s %-4s %-5s %-5d %-6d %-6ld %s\n",
			fs->id, fs->channel, fs->sending ? "TX" : "RX",
			fs->t38 ? "yes" : "no", fs->st.pages,
			(int)fs->st.bps, fax_session_secs(fs),
			S_OR(fs->st.phase, ""));
		n++;
	}
	AST_LIST_UNLOCK(&fax_sessions);
	ast_cli(a->fd, "%d active DModem fax session%s\n", n, n == 1 ? "" : "s");
	return CLI_SUCCESS;
}

/* "dmodem fax show session <id>": the details of one fax */
static char *cli_fax_session(struct ast_cli_entry *e, int cmd,
			     struct ast_cli_args *a)
{
	struct fax_session *fs;
	int id, found = 0;

	if (cmd == CLI_INIT) {
		e->command = "dmodem fax show session";
		e->usage = "Usage: dmodem fax show session <id>\n"
			"       The details of a DModem fax that runs now.\n";
		return NULL;
	}
	if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != 5)
		return CLI_SHOWUSAGE;
	id = atoi(a->argv[4]);
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_TRAVERSE(&fax_sessions, fs, list) {
		if (fs->id != id)
			continue;
		found = 1;
		ast_cli(a->fd, "Session:          %d\n", fs->id);
		ast_cli(a->fd, "Channel:          %s\n", fs->channel);
		ast_cli(a->fd, "Direction:        %s\n",
			fs->sending ? "send" : "receive");
		ast_cli(a->fd, "File:             %s\n", fs->file);
		ast_cli(a->fd, "Transport:        %s\n",
			fs->t38 ? "T.38" : "audio");
		ast_cli(a->fd, "Phase:            %s\n",
			S_OR(fs->st.phase, ""));
		ast_cli(a->fd, "Pages:            %d\n", fs->st.pages);
		ast_cli(a->fd, "Rate:             %d\n", (int)fs->st.bps);
		ast_cli(a->fd, "ECM:              %s\n",
			fs->st.ecm ? "yes" : "no");
		ast_cli(a->fd, "Resolution:       %s\n",
			fs->st.fine ? "fine" : "standard");
		ast_cli(a->fd, "Local station:    %s\n", fs->local);
		ast_cli(a->fd, "Remote station:   %s\n", fs->remote);
		ast_cli(a->fd, "Seconds:          %ld\n",
			fax_session_secs(fs));
		break;
	}
	AST_LIST_UNLOCK(&fax_sessions);
	if (!found)
		ast_cli(a->fd, "No DModem fax session %d\n", id);
	return CLI_SUCCESS;
}

/* "dmodem fax show stats": the totals since the module loaded */
static char *cli_fax_stats(struct ast_cli_entry *e, int cmd,
			   struct ast_cli_args *a)
{
	struct fax_session *fs;
	int active = 0;
	size_t k;

	if (cmd == CLI_INIT) {
		e->command = "dmodem fax show stats";
		e->usage = "Usage: dmodem fax show stats\n"
			"       The DModem fax totals since the module loaded.\n";
		return NULL;
	}
	if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_TRAVERSE(&fax_sessions, fs, list)
		active++;
	ast_cli(a->fd, "Current sessions:      %d\n", active);
	ast_cli(a->fd, "Transmit attempts:     %d\n", fax_stats.tx_attempts);
	ast_cli(a->fd, "Receive attempts:      %d\n", fax_stats.rx_attempts);
	ast_cli(a->fd, "Completed faxes:       %d\n", fax_stats.completed);
	ast_cli(a->fd, "Failed faxes:          %d\n", fax_stats.failed);
	for (k = 0; k < ARRAY_LEN(fax_stats.reasons) &&
	     fax_stats.reasons[k].code[0]; k++)
		ast_cli(a->fd, "  %-20s %d\n", fax_stats.reasons[k].code,
			fax_stats.reasons[k].n);
	AST_LIST_UNLOCK(&fax_sessions);
	return CLI_SUCCESS;
}

/* "dmodem fax show settings": the [general] fax settings */
static char *cli_fax_settings(struct ast_cli_entry *e, int cmd,
			      struct ast_cli_args *a)
{
	struct dmbridge_fax f;

	if (cmd == CLI_INIT) {
		e->command = "dmodem fax show settings";
		e->usage = "Usage: dmodem fax show settings\n"
			"       The fax settings of dmodem.conf [general].\n";
		return NULL;
	}
	if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	load_fax_settings(&f);
	ast_cli(a->fd, "Receive in DModem():   %s\n", f.on ? "yes" : "no");
	ast_cli(a->fd, "Fax directory:         %s\n", f.dir);
	ast_cli(a->fd, "Local station:         %s\n", f.ident);
	ast_cli(a->fd, "Maximum rate:          %d\n", f.max_bps);
	ast_cli(a->fd, "Minimum rate:          %d\n", f.min_bps);
	ast_cli(a->fd, "ECM:                   %s\n", f.ecm ? "yes" : "no");
	{
		char m[32];
		dmbridge_fax_modems_text(f.modems, m, sizeof(m));
		ast_cli(a->fd, "Modems:                %s\n", m);
	}
	ast_cli(a->fd, "Header:                %s\n", f.header);
	ast_cli(a->fd, "T.38:                  %s\n", f.t38 ? "yes" : "no");
	ast_cli(a->fd, "T.38 timeout (ms):     %d\n", f.t38_timeout);
	ast_cli(a->fd, "res_fax variables:     %s\n",
		f.res_vars ? "yes" : "no");
	return CLI_SUCCESS;
}

/* "dmodem fax show capabilities": what the fax machine can do */
static char *cli_fax_capabilities(struct ast_cli_entry *e, int cmd,
				  struct ast_cli_args *a)
{
	if (cmd == CLI_INIT) {
		e->command = "dmodem fax show capabilities";
		e->usage = "Usage: dmodem fax show capabilities\n"
			"       What the DModem fax machine can do.\n";
		return NULL;
	}
	if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	ast_cli(a->fd, "Technology:   DModem (libdmodem T.30)\n");
	ast_cli(a->fd, "Directions:   send, receive\n");
	ast_cli(a->fd, "Transports:   audio (G.711), T.38 (versions 0 to 3)\n");
	ast_cli(a->fd, "Modems:       V.17, V.29, V.27ter, 2400 to 14400 bit/s\n");
	ast_cli(a->fd, "ECM:          yes\n");
	ast_cli(a->fd, "Coding:       MH, MR, MMR, T.85 (with ECM)\n");
	ast_cli(a->fd, "Files:        TIFF (several in one fax)\n");
	return CLI_SUCCESS;
}

static struct ast_cli_entry fax_cli[] = {
	AST_CLI_DEFINE(cli_fax_sessions, "Show the DModem faxes that run"),
	AST_CLI_DEFINE(cli_fax_session, "Show one DModem fax"),
	AST_CLI_DEFINE(cli_fax_stats, "Show the DModem fax totals"),
	AST_CLI_DEFINE(cli_fax_settings, "Show the DModem fax settings"),
	AST_CLI_DEFINE(cli_fax_capabilities, "Show what the DModem fax can do"),
};

/* a session's fields in an AMI event or response (fax_sessions
   locked) */
static void ami_session_fields(struct mansession *s, const char *id_text,
			       const struct fax_session *fs)
{
	astman_append(s, "%sSessionNumber: %d\r\nChannel: %s\r\n"
		      "Operation: %s\r\nTransport: %s\r\nFileName: %s\r\n"
		      "Pages: %d\r\nRate: %d\r\nECM: %s\r\n"
		      "LocalStationID: %s\r\nRemoteStationID: %s\r\n"
		      "Phase: %s\r\nSeconds: %ld\r\n",
		      id_text, fs->id, fs->channel,
		      fs->sending ? "send" : "receive",
		      fs->t38 ? "T.38" : "audio", fs->file, fs->st.pages,
		      (int)fs->st.bps, fs->st.ecm ? "yes" : "no", fs->local,
		      fs->remote, S_OR(fs->st.phase, ""),
		      fax_session_secs(fs));
}

/* the ActionID header as text for the responses */
static void ami_id_text(const struct message *m, char *buf, size_t len)
{
	const char *id = astman_get_header(m, "ActionID");
	*buf = 0;
	if (!ast_strlen_zero(id))
		snprintf(buf, len, "ActionID: %s\r\n", id);
}

/* AMI DModemFaxSessions: a DModemFaxSessionsEntry for each fax that
   runs, then DModemFaxSessionsComplete */
static int ami_fax_sessions(struct mansession *s, const struct message *m)
{
	struct fax_session *fs;
	char id_text[256];
	int n = 0;

	ami_id_text(m, id_text, sizeof(id_text));
	astman_send_listack(s, m, "DModemFaxSessionsEntry event list will follow",
			    "Start");
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_TRAVERSE(&fax_sessions, fs, list) {
		astman_append(s, "Event: DModemFaxSessionsEntry\r\n");
		ami_session_fields(s, id_text, fs);
		astman_append(s, "\r\n");
		n++;
	}
	AST_LIST_UNLOCK(&fax_sessions);
	astman_send_list_complete_start(s, m, "DModemFaxSessionsComplete", n);
	astman_send_list_complete_end(s);
	return 0;
}

/* AMI DModemFaxSession (SessionNumber): the details of one fax */
static int ami_fax_session(struct mansession *s, const struct message *m)
{
	const char *num = astman_get_header(m, "SessionNumber");
	struct fax_session *fs;
	char id_text[256];
	int id, found = 0;

	if (ast_strlen_zero(num)) {
		astman_send_error(s, m, "SessionNumber is required");
		return 0;
	}
	id = atoi(num);
	ami_id_text(m, id_text, sizeof(id_text));
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_TRAVERSE(&fax_sessions, fs, list) {
		if (fs->id != id)
			continue;
		found = 1;
		astman_send_ack(s, m, NULL);
		astman_append(s, "Event: DModemFaxSession\r\n");
		ami_session_fields(s, id_text, fs);
		astman_append(s, "\r\n");
		break;
	}
	AST_LIST_UNLOCK(&fax_sessions);
	if (!found)
		astman_send_error(s, m, "No such session");
	return 0;
}

/* AMI DModemFaxStats: the totals since the module loaded */
static int ami_fax_stats(struct mansession *s, const struct message *m)
{
	struct fax_session *fs;
	char id_text[256];
	int active = 0;

	ami_id_text(m, id_text, sizeof(id_text));
	astman_send_ack(s, m, NULL);
	AST_LIST_LOCK(&fax_sessions);
	AST_LIST_TRAVERSE(&fax_sessions, fs, list)
		active++;
	astman_append(s, "Event: DModemFaxStats\r\n%sCurrentSessions: %d\r\n"
		      "TransmitAttempts: %d\r\nReceiveAttempts: %d\r\n"
		      "CompletedFAXes: %d\r\nFailedFAXes: %d\r\n\r\n",
		      id_text, active, fax_stats.tx_attempts,
		      fax_stats.rx_attempts, fax_stats.completed,
		      fax_stats.failed);
	AST_LIST_UNLOCK(&fax_sessions);
	return 0;
}

/* AMI events DModemFaxStatus (option s: a phase or a page) */
static void ami_fax_status(struct ast_channel *chan, int session,
			   int sending, const struct dmodem_fax_status *st,
			   const char *file)
{
	ast_manager_event(chan, EVENT_FLAG_CALL, "DModemFaxStatus",
			  "Channel: %s\r\nSessionNumber: %d\r\n"
			  "Operation: %s\r\nStatus: %s\r\nPages: %d\r\n"
			  "FileName: %s\r\n",
			  ast_channel_name(chan), session,
			  sending ? "send" : "receive", S_OR(st->phase, ""),
			  st->pages, file);
}

/* AMI events DModemSendFax and DModemReceiveFax: the result at the end
   of a fax */
static void ami_fax_result(struct ast_channel *chan, int session,
			   int sending, const struct dmbridge_fax *fs,
			   const struct dmodem_fax_status *st, int t38,
			   const char *file)
{
	ast_manager_event(chan, EVENT_FLAG_CALL,
			  sending ? "DModemSendFax" : "DModemReceiveFax",
			  "Channel: %s\r\nSessionNumber: %d\r\n"
			  "Status: %s\r\nError: %s\r\nStatusString: %s\r\n"
			  "Pages: %d\r\nRate: %d\r\nResolution: %s\r\n"
			  "ECM: %s\r\nTransport: %s\r\n"
			  "LocalStationID: %s\r\nRemoteStationID: %s\r\n"
			  "FileName: %s\r\n",
			  ast_channel_name(chan), session,
			  st->ok ? "SUCCESS" : "FAILED",
			  S_OR(st->error, "UNKNOWN"), S_OR(st->result, ""),
			  st->pages, (int)st->bps,
			  st->fine ? "8040x7700" : "8040x3850",
			  st->ecm ? "yes" : "no", t38 ? "T.38" : "audio",
			  fs->ident, st->remote_ident, file);
}

/* the fax's results in the channel's variables: DMODEMFAX*, and
   res_fax's (FAXSTATUS and the rest) with res_vars */
static void fax_variables(struct ast_channel *chan,
			  const struct dmbridge_fax *fs,
			  const struct dmodem_fax_status *st, int t38,
			  int session)
{
	char pages[16], rate[16], id[16];
	const char *status = st->ok ? "SUCCESS" : "FAILED";
	const char *res = st->fine ? "8040x7700" : "8040x3850";
	const char *error = S_OR(st->error, "UNKNOWN");
	const char *result = S_OR(st->result, "");

	snprintf(pages, sizeof(pages), "%d", st->pages);
	snprintf(rate, sizeof(rate), "%d", st->bps);
	snprintf(id, sizeof(id), "%d", session);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS", status);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRESULT", result);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXERROR", error);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXPAGES", pages);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRATE", rate);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRESOLUTION", res);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXREMOTE", st->remote_ident);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXECM", st->ecm ? "yes" : "no");
	pbx_builtin_setvar_helper(chan, "DMODEMFAXT38", t38 ? "yes" : "no");
	pbx_builtin_setvar_helper(chan, "DMODEMFAXSESSION", id);
	if (!fs->res_vars)
		return;
	pbx_builtin_setvar_helper(chan, "FAXSTATUS", status);
	pbx_builtin_setvar_helper(chan, "FAXSTATUSSTRING", result);
	pbx_builtin_setvar_helper(chan, "FAXERROR", error);
	pbx_builtin_setvar_helper(chan, "FAXPAGES", pages);
	pbx_builtin_setvar_helper(chan, "FAXBITRATE", rate);
	pbx_builtin_setvar_helper(chan, "FAXRESOLUTION", res);
	pbx_builtin_setvar_helper(chan, "REMOTESTATIONID", st->remote_ident);
	pbx_builtin_setvar_helper(chan, "LOCALSTATIONID", fs->ident);
}

/* a fax that did not start or stopped (error the code, result its
   text): the variables, the totals and the AMI event; sess (or NULL)
   out of the list */
static void fax_failed(struct ast_channel *chan, const struct dmbridge_fax *fs,
		       const char *error, const char *result, int session,
		       int sending, const char *file, struct fax_session *sess)
{
	struct dmodem_fax_status st = { 0 };
	st.error = error;
	st.result = result;
	fax_variables(chan, fs, &st, 0, session);
	if (!sess) {
		/* (an attempt all the same) */
		AST_LIST_LOCK(&fax_sessions);
		if (sending)
			fax_stats.tx_attempts++;
		else
			fax_stats.rx_attempts++;
		AST_LIST_UNLOCK(&fax_sessions);
	}
	fax_session_end(sess, 0, error);
	ami_fax_result(chan, session, sending, fs, &st, 0, file);
}

/* a fax call on the channel until it ends; -1 if the channel hung up.

   With fax_t38 on a channel that can (T.38 not refused), the fax goes
   over to T.38 when the channel does, before a page has gone (the fax
   machine anew on IFP packets, its time by the clock): a receiver asks
   for it as ReceiveFAX does, and waits for the answer (fax_t38_timeout
   ms at most, silence on the line) before it starts on the audio; a
   sender does the same with option z, else takes it when the other end
   asks.  A request that came before (the channel negotiating) is had
   again, and answered.  ro: the options (F no T.38, T T.38 or no fax, d
   the phases in the log) */
static int run_fax(struct ast_channel *chan, struct ast_format *fmt,
		   enum dmodem_law law, enum dmodem_fax_dir dir,
		   const char *file, const struct dmbridge_fax *fs,
		   const struct fax_run *ro, unsigned char *out)
{
	struct dmodem_fax_config cfg = { 0 };
	struct dmodem_fax_status st = { 0 };
	struct dmodem_fax *f;
	const char *name = ast_channel_name(chan);
	enum ast_t38_state ts = ast_channel_get_t38_state(chan);
	struct timeval t0 = { 0 }, asked = { 0 };
	long long samples = 0;
	const char *phase = NULL;
	struct fax_session *sess = NULL;
	int res = 0, t38 = 0, asking = 0, our_version = 0, pages = 0;
	int session = ast_atomic_fetchadd_int(&fax_session_seq, 1) + 1;
	int may_switch = fs->t38 && !ro->audio_only &&
		(ts == T38_STATE_UNKNOWN || ts == T38_STATE_NEGOTIATING);

	cfg.law = law;
	cfg.dir = dir;
	cfg.file = file;
	cfg.ident = fs->ident;
	cfg.header = fs->header[0] ? fs->header : NULL;
	cfg.max_bps = fs->max_bps;
	cfg.min_bps = fs->min_bps;
	cfg.no_ecm = !fs->ecm;
	cfg.modems = fs->modems;
	pbx_builtin_setvar_helper(chan, "DMODEMFAXFILE", file);
	if (ro->need_t38 && !may_switch) {
		ast_log(LOG_WARNING, "%s: DModem fax: T.38 needed, and the "
			"channel cannot have it\n", name);
		fax_failed(chan, fs, "T38", "no T.38", session,
			   dir == DMODEM_FAX_SEND, file, NULL);
		return 0;
	}
	f = dmodem_fax_create(&cfg);
	if (!f) {
		ast_log(LOG_WARNING, "%s: cannot %s fax file %s\n", name,
			dir == DMODEM_FAX_SEND ? "read" : "use", file);
		fax_failed(chan, fs, "DOCUMENT", "no document", session,
			   dir == DMODEM_FAX_SEND, file, NULL);
		return 0;
	}
	sess = fax_session_add(chan, session, dir == DMODEM_FAX_SEND, file,
			       fs->ident);
	ast_verb(3, "%s: DModem fax %d: %s %s\n", name, session,
		 dir == DMODEM_FAX_SEND ? "sending" : "receiving", file);
	if (may_switch && ts == T38_STATE_NEGOTIATING) {
		/* the other end's request again, to answer */
		struct ast_control_t38_parameters t = {
			.request_response = AST_T38_REQUEST_PARMS };
		if (ast_indicate_data(chan, AST_CONTROL_T38_PARAMETERS, &t,
				      sizeof(t)) != AST_T38_REQUEST_PARMS)
			may_switch = 0;
		else if (ro->need_t38) {
			/* T.38 or no fax: the fax machine waits for it */
			asking = 1;
			asked = ast_tvnow();
		}
	} else if (may_switch && (dir == DMODEM_FAX_RECEIVE ||
				  ro->ask_t38 || ro->need_t38)) {
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
		if (asking && ast_tvdiff_ms(ast_tvnow(), asked) >
		    fs->t38_timeout)
			asking = 0;
		if (ro->need_t38 && !t38 && !asking) {
			/* no T.38: no fax (option T) */
			ast_log(LOG_WARNING, "%s: DModem fax: no T.38\n", name);
			if (fr)
				ast_frfree(fr);
			dmodem_fax_destroy(f);
			fax_failed(chan, fs, "T38", "no T.38", session,
				   dir == DMODEM_FAX_SEND, file, sess);
			return 0;
		}
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
					fax_failed(chan, fs, "DOCUMENT",
						   "no document", session,
						   dir == DMODEM_FAX_SEND,
						   file, sess);
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
		fax_session_update(sess, &st, t38);
		if (st.phase && (!phase || strcmp(phase, st.phase) ||
				 st.pages != pages)) {
			if (ro->debug)
				ast_verb(3, "%s: DModem fax %d: %s\n", name,
					 session, st.phase);
			if (ro->status_events)
				ami_fax_status(chan, session,
					       dir == DMODEM_FAX_SEND, &st,
					       file);
			phase = st.phase;
			pages = st.pages;
		}
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
	ast_verb(3, "%s: DModem fax %d %s: %s, %d pages at %d bit/s%s%s, remote '%s'\n",
		 name, session, dir == DMODEM_FAX_SEND ? "sent" : "received",
		 st.result, st.pages, st.bps, st.ecm ? " (ECM)" : "",
		 t38 ? " over T.38" : "", st.remote_ident);
	fax_variables(chan, fs, &st, t38, session);
	fax_session_end(sess, st.ok, st.error);
	ami_fax_result(chan, session, dir == DMODEM_FAX_SEND, fs, &st, t38,
		       file);
	return res;
}

/*
 *    voice: a person's call to an extension, a DSVD or ASVD caller's voice
 */

/* the profile's voice extension (exten@context, or exten in the
   channel's own context) called on a Local channel, the caller's caller
   ID and variables its, fmt its audio both ways; NULL if not */
static struct ast_channel *ext_call(struct ast_channel *chan,
				    const char *voice, struct ast_format *fmt)
{
	struct ast_format_cap *cap;
	struct ast_channel *ext;
	char tech[64] = "Local", dest[256];
	const char *slash = strchr(voice, '/');
	int cause = 0;

	if (slash && (size_t)(slash - voice) < sizeof(tech)) {
		/* TECH/data: that channel, e.g. PJSIP/7000 */
		snprintf(tech, sizeof(tech), "%.*s", (int)(slash - voice),
			 voice);
		snprintf(dest, sizeof(dest), "%s", slash + 1);
	} else if (strchr(voice, '@'))
		snprintf(dest, sizeof(dest), "%s/n", voice);
	else
		snprintf(dest, sizeof(dest), "%s@%s/n", voice,
			 ast_channel_context(chan));
	cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!cap)
		return NULL;
	ast_format_cap_append(cap, fmt, 0);
	ext = ast_request(tech, cap, NULL, chan, dest, &cause);
	ao2_ref(cap, -1);
	if (!ext) {
		ast_log(LOG_WARNING, "%s: no %s/%s: %s\n",
			ast_channel_name(chan), tech, dest,
			ast_cause2str(cause));
		return NULL;
	}
	ast_channel_lock_both(chan, ext);
	ast_connected_line_copy_from_caller(ast_channel_connected(ext),
					    ast_channel_caller(chan));
	ast_channel_inherit_variables(chan, ext);
	ast_channel_datastore_inherit(chan, ext);
	ast_max_forwards_decrement(ext);
	ast_channel_language_set(ext, ast_channel_language(chan));
	ast_channel_req_accountcodes(ext, chan,
				     AST_CHANNEL_REQUESTOR_BRIDGE_PEER);
	ast_channel_unlock(ext);
	ast_channel_unlock(chan);
	if (ast_set_read_format(ext, fmt) || ast_set_write_format(ext, fmt) ||
	    ast_call(ext, dest, 0)) {
		ast_log(LOG_WARNING, "%s: cannot call %s/%s\n",
			ast_channel_name(chan), tech, dest);
		ast_hangup(ext);
		return NULL;
	}
	ast_verb(3, "%s: DModem: voice to %s/%s\n", ast_channel_name(chan),
		 tech, dest);
	return ext;
}

/* the extension's ringing in band on the (answered) channel, or not */
static void ext_ringback(struct ast_channel *chan, int on)
{
	struct ast_tone_zone_sound *ts;

	if (!on) {
		ast_playtones_stop(chan);
		return;
	}
	ts = ast_get_indication_tone(ast_channel_zone(chan), "ring");
	if (ts) {
		ast_playtones_start(chan, 0, ts->data, 0);
		ast_tone_zone_sound_unref(ts);
	}
}

/* a frame of the extension's leg (NULL: it is gone): 0 while it goes on,
   -1 when it has ended (hung up, its dialplan over); voice frames to the
   channel when to_chan, the ringing as tones */
static int ext_frame(struct ast_channel *chan, struct ast_frame *f,
		     int to_chan)
{
	if (!f)
		return -1;
	if (f->frametype == AST_FRAME_CONTROL) {
		switch (f->subclass.integer) {
		case AST_CONTROL_HANGUP:
			return -1;
		case AST_CONTROL_RINGING:
			if (to_chan)
				ext_ringback(chan, 1);
			break;
		case AST_CONTROL_ANSWER:
		case AST_CONTROL_PROGRESS:
			if (to_chan)
				ext_ringback(chan, 0);
			break;
		case AST_CONTROL_BUSY:
		case AST_CONTROL_CONGESTION:
			return -1;
		default:
			break;
		}
	} else if (f->frametype == AST_FRAME_VOICE && to_chan) {
		ext_ringback(chan, 0);
		ast_write(chan, f);
	}
	return 0;
}

/* the voice leg: a person on the line through to the extension (ext),
   its audio back, the caller's DTMF too, the caller's audio also through
   the modem for its calling tones; 1 when the leg is over (the extension
   gone, or the caller's modem asking for data: the leg hung up), -1 if
   the channel hung up */
static int voice_leg(struct ast_channel *chan, struct ast_channel *ext,
		     struct dmbridge_call *c, struct ast_format *fmt)
{
	unsigned char scratch[LINE_MAX_SAMPLES];
	int res = 0;

	while (!res) {
		struct ast_channel *cs[2] = { chan, ext }, *w;
		struct ast_frame *f;
		int ms = IDLE_MS;

		w = ast_waitfor_n(cs, 2, &ms);
		if (!w) {
			if (ms < 0)
				res = -1;
			continue;
		}
		f = ast_read(w);
		if (w == ext) {
			if (ext_frame(chan, f, 1) < 0) {
				ast_verb(3, "%s: DModem: the extension's leg "
					 "ended: the modem answers\n",
					 ast_channel_name(chan));
				res = 1;
			}
		} else if (!f) {
			res = -1;
		} else if (f->frametype == AST_FRAME_VOICE &&
			   f->datalen > 0 &&
			   ast_format_cmp(f->subclass.format, fmt) ==
			   AST_FORMAT_CMP_EQUAL) {
			int n = f->datalen < LINE_MAX_SAMPLES ? f->datalen :
				LINE_MAX_SAMPLES;
			ast_write(ext, f);
			dmodem_process_g711(c->d, f->data.ptr, scratch, n);
			/* (the bridge's log says why) */
			if (dmbridge_voice_step(c, n) == DMBRIDGE_DATA)
				res = 1;
		} else if (f->frametype == AST_FRAME_DTMF_BEGIN ||
			   f->frametype == AST_FRAME_DTMF_END) {
			ast_write(ext, f);
		}
		if (f)
			ast_frfree(f);
	}
	ext_ringback(chan, 0);
	ast_hangup(ext);
	return res;
}

/* the DSVD leg's frame (ext's, f; NULL: it is gone): its speech to the
   voice channel; -1 when the leg has ended */
static int dsvd_frame(struct ast_channel *chan, struct dmbridge_call *c,
		      struct ast_frame *f)
{
	if (ext_frame(chan, f, 0) < 0)
		return -1;
	if (f->frametype == AST_FRAME_VOICE && f->datalen > 0 &&
	    ast_format_cmp(f->subclass.format, ast_format_slin) ==
	    AST_FORMAT_CMP_EQUAL)
		dmbridge_dsvd_mic(c, f->data.ptr, f->datalen / 2);
	return 0;
}

/* the voice channel's far end speech, n samples (the line's), to the
   DSVD leg */
static void dsvd_ear(struct ast_channel *ext, struct dmbridge_call *c, int n)
{
	int16_t pcm[AST_FRIENDLY_OFFSET / 2 + LINE_MAX_SAMPLES];
	struct ast_frame wf = { .frametype = AST_FRAME_VOICE, .src = app };

	if (n > LINE_MAX_SAMPLES)
		n = LINE_MAX_SAMPLES;
	n = dmbridge_dsvd_ear(c, pcm + AST_FRIENDLY_OFFSET / 2, n);
	if (n <= 0)
		return;
	wf.subclass.format = ast_format_slin;
	wf.data.ptr = pcm + AST_FRIENDLY_OFFSET / 2;
	wf.offset = AST_FRIENDLY_OFFSET;
	wf.datalen = n * 2;
	wf.samples = n;
	ast_write(ext, &wf);
}

/* peer_address: the address the call came from by SIP ("" if not SIP or
   not known), for the BBS to place a caller with no caller ID */
static void peer_address(struct ast_channel *chan, char *buf, size_t len)
{
	const char *tech = ast_channel_tech(chan)->type;

	*buf = '\0';
	if (!strcasecmp(tech, "PJSIP")) {
		if (ast_func_read(chan, "CHANNEL(pjsip,remote_addr)", buf,
				  len))
			*buf = '\0';
	} else if (!strcasecmp(tech, "SIP")) {
		if (ast_func_read(chan, "CHANNEL(peerip)", buf, len))
			*buf = '\0';
	}
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
	struct ast_channel *ext = NULL; /* a DSVD voice channel's leg */
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
		struct ast_channel *cs[2] = { chan, ext }, *w;
		struct ast_frame *f;
		enum dmbridge_event ev;
		int n = IDLE_MS, k = 0;

		w = ast_waitfor_n(cs, ext ? 2 : 1, &n);
		if (!w && n < 0) {
			c.status = "HANGUP";
			res = -1;
			break;
		}
		if (w && w == ext) {
			/* the DSVD leg: its speech to the voice channel */
			f = ast_read(ext);
			if (dsvd_frame(chan, &c, f) < 0) {
				ast_hangup(ext);
				ext = NULL;
				dmbridge_dsvd_close(&c);
			}
			if (f)
				ast_frfree(f);
			continue;
		}
		if (!w) {
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

		if (ext && k)
			dsvd_ear(ext, &c, k);
		ev = t38_fax ? DMBRIDGE_FAX : dmbridge_call_step(&c, k);
		if (ev == DMBRIDGE_VOICE) {
			/* a person: to the extension until its leg is over,
			   then the modem answers */
			struct ast_channel *leg = ext_call(chan, p.voice, fmt);
			int r = leg ? voice_leg(chan, leg, &c, fmt) : 1;
			if (r < 0) {
				c.status = "HANGUP";
				res = -1;
				break;
			}
			if (dmbridge_call_resume(&c))
				break;
			continue;
		}
		if (ev == DMBRIDGE_DSVD_OPEN && !ext) {
			/* a DSVD or ASVD caller's voice channel: its leg to
			   the extension, linear audio both ways */
			ext = ext_call(chan, p.voice, ast_format_slin);
			if (!ext)
				dmbridge_dsvd_close(&c);
			continue;
		}
		if (ev == DMBRIDGE_DSVD_CLOSED) {
			if (ext) {
				ast_verb(3, "%s: DModem: the voice channel "
					 "closed\n", name);
				ast_hangup(ext);
				ext = NULL;
			}
			continue;
		}
		if (ev == DMBRIDGE_CONNECTED) {
			char connected[24], peer[128];
			const char *env[] = {
				"DMODEM_CHANNEL", name,
				"DMODEM_PROFILE", args.profile,
				"DMODEM_RATE", c.rate,
				"DMODEM_MODULATION", c.modulation,
				"DMODEM_PROTOCOL", c.protocol,
				"DMODEM_COMPRESSION", c.compression,
				"DMODEM_CALLERID",
				S_COR(ast_channel_caller(chan)->id.number.valid,
				      ast_channel_caller(chan)->id.number.str, ""),
				"DMODEM_DIALED",
				S_OR(ast_channel_dialed(chan)->number.str,
				     ast_channel_exten(chan)),
				"DMODEM_CONNECTED", connected,
				"DMODEM_PEER", peer,
				NULL
			};
			snprintf(connected, sizeof(connected), "%lld",
				 (long long)time(NULL));
			peer_address(chan, peer, sizeof(peer));
			pbx_builtin_setvar_helper(chan, "DMODEMRATE", c.rate);
			pbx_builtin_setvar_helper(chan, "DMODEMMODULATION",
						  c.modulation);
			pbx_builtin_setvar_helper(chan, "DMODEMPROTOCOL",
						  c.protocol);
			pbx_builtin_setvar_helper(chan, "DMODEMCOMPRESSION",
						  c.compression);
			ast_verb(3, "%s: DModem connected at %s bit/s (%s %s/%s)\n",
				 name, c.rate, c.modulation, c.protocol,
				 c.compression);
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
			struct fax_run ro = { 0 };
			faxopt_apply(chan, &p.fax);
			if (run_fax(chan, fmt, law, DMODEM_FAX_RECEIVE, path,
				    &p.fax, &ro, out) < 0)
				res = -1;
			break;
		}
		if (ev == DMBRIDGE_END)
			break;
	}
	if (ext)
		ast_hangup(ext);
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
	"  DModemSendFax(file[&file2...][,ident[,options]]): sends the TIFF\n"
	"files as one group 3 fax on a call placed to a fax machine (it waits\n"
	"for the machine's DIS).  ident: our number (TSI), else\n"
	"DMODEMFAXOPT(localstationid), else fax_ident of dmodem.conf\n"
	"[general].  Each page has a header line (date, time, the header text,\n"
	"ident and page number) when DMODEMFAXOPT(headerinfo) or fax_header\n"
	"of [general] has a text.\n"
	"  Options: d - each phase in the log; f - audio when T.38 fails (the\n"
	"default); F - audio only, no T.38; z - ask for T.38 first; T - T.38\n"
	"or no fax; s - AMI DModemFaxStatus events.\n"
	"  Sets DMODEMFAXSTATUS (SUCCESS or FAILED), DMODEMFAXRESULT,\n"
	"DMODEMFAXERROR, DMODEMFAXPAGES, DMODEMFAXRATE, DMODEMFAXRESOLUTION,\n"
	"DMODEMFAXREMOTE, DMODEMFAXECM, DMODEMFAXT38, DMODEMFAXSESSION,\n"
	"DMODEMFAXFILE; also res_fax's FAXSTATUS, FAXSTATUSSTRING, FAXERROR,\n"
	"FAXPAGES, FAXBITRATE, FAXRESOLUTION, REMOTESTATIONID and\n"
	"LOCALSTATIONID (unless fax_res_vars = no).  DMODEMFAXOPT() sets\n"
	"ecm, maxrate, minrate, t38timeout, localstationid, headerinfo and\n"
	"modems first.\n";
static const char app_recv[] = "DModemReceiveFax";
static const char recv_synopsis[] = "Receive a fax into a TIFF file with DModem's fax";
static const char recv_descrip[] =
	"  DModemReceiveFax(file[,ident[,options]]): answers the call as a\n"
	"group 3 fax machine and writes what it receives to the TIFF file\n"
	"(none if no page came).  ident: our number (CSI), else\n"
	"DMODEMFAXOPT(localstationid), else fax_ident of [general].\n"
	"  Options: d, f, F, T and s as for DModemSendFax (it always asks for\n"
	"T.38 when the channel can have it).\n"
	"  Sets the same variables as DModemSendFax.  DModem() receives faxes\n"
	"by itself when a fax machine calls (fax = yes, the default).\n";

/* DModemSendFax / DModemReceiveFax (file[,ident]) in direction dir on
   the channel's law; -1 if the channel hung up */
static int fax_exec(struct ast_channel *chan, const char *data,
		    enum dmodem_fax_dir dir)
{
	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(file); AST_APP_ARG(ident);
			     AST_APP_ARG(options););
	struct dmbridge_fax fs;
	struct fax_run ro = { 0 };
	struct ast_flags opts = { 0 };
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
	faxopt_apply(chan, &fs);
	if (!ast_strlen_zero(args.ident))
		ast_copy_string(fs.ident, args.ident, sizeof(fs.ident));
	if (!ast_strlen_zero(args.options) &&
	    ast_app_parse_options(fax_app_options, &opts, NULL, args.options)) {
		ast_log(LOG_WARNING, "%s: bad options '%s'\n",
			dir == DMODEM_FAX_SEND ? app_send : app_recv,
			args.options);
		pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS", "FAILED");
		return 0;
	}
	ro.debug = ast_test_flag(&opts, 1 << 0);
	/* (f, audio after a failed T.38, is what happens anyway) */
	ro.audio_only = ast_test_flag(&opts, 1 << 2);
	ro.ask_t38 = ast_test_flag(&opts, 1 << 3);
	ro.need_t38 = ast_test_flag(&opts, 1 << 4);
	ro.status_events = ast_test_flag(&opts, 1 << 5);
	if (ro.audio_only && ro.need_t38) {
		ast_log(LOG_WARNING, "%s: options F and T together\n",
			dir == DMODEM_FAX_SEND ? app_send : app_recv);
		pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS", "FAILED");
		return 0;
	}
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
	return run_fax(chan, fmt, law, dir, args.file, &fs, &ro, out);
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

static const char app_detect[] = "DModemFaxDetect";
static const char detect_synopsis[] = "Detect a fax machine that calls, and go to the fax extension";
static const char detect_descrip[] =
	"  DModemFaxDetect([seconds[,options]]): answers the call and listens\n"
	"for up to seconds (default 4) for a fax machine: its calling tone\n"
	"(CNG, 1100 Hz) or its request for T.38.  When it hears one, it sets\n"
	"DMODEMFAXDETECTED (CNG or T38) and DMODEMFAXEXTEN (the extension\n"
	"that the call was on), and goes to the fax extension of the context\n"
	"if there is one.  Else it sets DMODEMFAXDETECTED to NONE, and the\n"
	"dialplan continues.\n"
	"  Options: c - the CNG only; t - the T.38 request only; n - do not\n"
	"answer (early media); r - ringing to the caller while it listens.\n";

AST_APP_OPTIONS(detect_app_options, BEGIN_OPTIONS
	AST_APP_OPTION('c', 1 << 0),
	AST_APP_OPTION('t', 1 << 1),
	AST_APP_OPTION('n', 1 << 2),
	AST_APP_OPTION('r', 1 << 3),
END_OPTIONS);

/* listens on chan (read in fmt, law) for up to ms: "CNG" when the
   detector heard a fax machine's tone (unless no_cng), "T38" when the
   other end asked for T.38 (unless no_t38; the request is not answered
   here: the fax application answers it), "NONE" when the time ended,
   NULL when the channel hung up */
static const char *fax_listen(struct ast_channel *chan,
			      enum dmodem_law law, long ms, int no_cng,
			      int no_t38)
{
	struct dmodem_faxdetect *d = dmodem_faxdetect_create(law);
	struct timeval start = ast_tvnow();
	const char *what = "NONE";

	if (!d)
		return "NONE";
	if (!no_t38 && ast_channel_get_t38_state(chan) == T38_STATE_NEGOTIATING)
		what = "T38";
	while (!strcmp(what, "NONE")) {
		const struct ast_control_t38_parameters *tp;
		struct ast_frame *fr;
		long left = ms - ast_tvdiff_ms(ast_tvnow(), start);
		int n;

		if (left <= 0)
			break;
		n = ast_waitfor(chan, left > IDLE_MS ? IDLE_MS : left);
		if (n < 0 || (n > 0 && !(fr = ast_read(chan)))) {
			what = NULL;
			break;
		}
		if (n == 0)
			continue;
		if (fr->frametype == AST_FRAME_VOICE && !no_cng &&
		    fr->samples > 0 && fr->datalen >= fr->samples &&
		    dmodem_faxdetect_process_g711(d, fr->data.ptr,
						  fr->samples))
			what = "CNG";
		else if ((tp = t38_frame(fr)) && !no_t38 &&
			 tp->request_response == AST_T38_REQUEST_NEGOTIATE)
			what = "T38";
		ast_frfree(fr);
	}
	dmodem_faxdetect_destroy(d);
	return what;
}

/* DModemFaxDetect(): a fax machine heard, the call to the fax
   extension; or NONE, the dialplan on */
static int detect_exec(struct ast_channel *chan, const char *data)
{
	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(seconds);
			     AST_APP_ARG(options););
	struct ast_flags opts = { 0 };
	struct ast_format *fmt, *old_fmt;
	enum dmodem_law law;
	const char *what;
	char *parse = ast_strdupa(S_OR(data, ""));
	double secs = 4;
	int ringing = 0;

	AST_STANDARD_APP_ARGS(args, parse);
	if (!ast_strlen_zero(args.seconds) &&
	    (sscanf(args.seconds, "%lf", &secs) != 1 || secs <= 0)) {
		ast_log(LOG_WARNING, "%s: bad time '%s'\n", app_detect,
			args.seconds);
		secs = 4;
	}
	if (!ast_strlen_zero(args.options) &&
	    ast_app_parse_options(detect_app_options, &opts, NULL,
				  args.options)) {
		ast_log(LOG_WARNING, "%s: bad options '%s'\n", app_detect,
			args.options);
		return 0;
	}
	if (ast_test_flag(&opts, 1 << 0) && ast_test_flag(&opts, 1 << 1)) {
		ast_log(LOG_WARNING, "%s: options c and t together\n",
			app_detect);
		return 0;
	}
	if (!ast_test_flag(&opts, 1 << 2) &&
	    ast_channel_state(chan) != AST_STATE_UP && ast_answer(chan))
		return -1;
	law = channel_law(chan);
	fmt = law == DMODEM_LAW_ALAW ? ast_format_alaw : ast_format_ulaw;
	old_fmt = ao2_bump(ast_channel_readformat(chan));
	if (ast_set_read_format(chan, fmt)) {
		ast_log(LOG_WARNING, "%s: cannot set %s\n",
			ast_channel_name(chan), ast_format_get_name(fmt));
		ao2_cleanup(old_fmt);
		return 0;
	}
	if (ast_test_flag(&opts, 1 << 3)) {
		struct ast_tone_zone_sound *ts =
			ast_get_indication_tone(ast_channel_zone(chan), "ring");
		if (ts) {
			ringing = !ast_playtones_start(chan, 0, ts->data, 0);
			ast_tone_zone_sound_unref(ts);
		}
	}
	what = fax_listen(chan, law, (long)(secs * 1000),
			  ast_test_flag(&opts, 1 << 1),
			  ast_test_flag(&opts, 1 << 0));
	if (ringing)
		ast_playtones_stop(chan);
	if (!what) {
		ao2_cleanup(old_fmt);
		return -1;
	}
	if (old_fmt)
		ast_set_read_format(chan, old_fmt);
	ao2_cleanup(old_fmt);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXDETECTED", what);
	if (!strcmp(what, "NONE")) {
		ast_verb(3, "%s: DModem fax detect: no fax\n",
			 ast_channel_name(chan));
		return 0;
	}
	pbx_builtin_setvar_helper(chan, "DMODEMFAXEXTEN",
				  ast_channel_exten(chan));
	if (strcmp(ast_channel_exten(chan), "fax") &&
	    ast_exists_extension(chan, ast_channel_context(chan), "fax", 1,
		S_COR(ast_channel_caller(chan)->id.number.valid,
		      ast_channel_caller(chan)->id.number.str, NULL))) {
		ast_verb(3, "%s: DModem fax detect: %s, to the fax extension\n",
			 ast_channel_name(chan), what);
		ast_explicit_goto(chan, NULL, "fax", 1);
	} else {
		ast_verb(3, "%s: DModem fax detect: %s (no fax extension in "
			 "%s)\n", ast_channel_name(chan), what,
			 ast_channel_context(chan));
	}
	return 0;
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

/* the four applications, DMODEMFAXOPT(), the CLI commands and the AMI
   actions unregistered */
static int unload_module(void)
{
	int r = ast_unregister_application(app);
	r |= ast_unregister_application(app_send);
	r |= ast_unregister_application(app_recv);
	r |= ast_unregister_application(app_detect);
	r |= ast_custom_function_unregister(&faxopt_function);
	ast_cli_unregister_multiple(fax_cli, ARRAY_LEN(fax_cli));
	r |= ast_manager_unregister("DModemFaxSessions");
	r |= ast_manager_unregister("DModemFaxSession");
	r |= ast_manager_unregister("DModemFaxStats");
	return r;
}

/* the log hook, the pumps, [general], the four applications,
   DMODEMFAXOPT(), the fax CLI commands and AMI actions */
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
				     recv_descrip) ||
	    ast_register_application(app_detect, detect_exec,
				     detect_synopsis, detect_descrip) ||
	    ast_custom_function_register(&faxopt_function) ||
	    ast_cli_register_multiple(fax_cli, ARRAY_LEN(fax_cli)) ||
	    ast_manager_register("DModemFaxSessions", EVENT_FLAG_REPORTING,
				 ami_fax_sessions, "List the DModem faxes") ||
	    ast_manager_register("DModemFaxSession", EVENT_FLAG_REPORTING,
				 ami_fax_session, "Show one DModem fax") ||
	    ast_manager_register("DModemFaxStats", EVENT_FLAG_REPORTING,
				 ami_fax_stats, "Show the DModem fax totals")) {
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
