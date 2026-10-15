/*
 * mod_dmodem.c - a data modem on a FreeSWITCH channel
 *
 * The call is answered by a software modem (V.21 to V.34, and V.90 as the
 * digital modem when the channel is G.711 end to end); once the modems
 * are connected, the data goes to a TCP server (a BBS, a telnet host) or
 * to a program on a pty (pppd, a getty, a door) as dmodem.conf's profile
 * says.  The module is also a group 3 fax machine.
 *
 * The modem is libdmodem (../dmodemd/dmodem.h): D-Modem's modem core
 * with the open data pumps, no binary-only parts.  The same as Asterisk's
 * app_dmodem (../asterisk), on FreeSWITCH's API; the call between the
 * modem and the server or program is theirs (../pbx/dmodem_bridge.c).
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2.
 */

#include <switch.h>

#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#include "dmodem_bridge.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_dmodem_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dmodem_shutdown);
SWITCH_MODULE_DEFINITION(mod_dmodem, mod_dmodem_load, mod_dmodem_shutdown,
			 NULL);

static const char config_file[] = "dmodem.conf";

#define LINE_MAX_SAMPLES 1600   /* in one frame, 200 ms */

/* <settings> and a profile: the jitter buffer (ms, 0 none) */
#define DEFAULT_JB_MS 60

/* the bridge's profile and the jitter buffer */
struct profile {
	struct dmbridge_profile b;
	int jb_ms;
};

/* the line: the channel's frames in G.711 (the channel's own codec, the
   octets as they come) or, on another codec, linear through FreeSWITCH's
   transcoding */
struct line {
	switch_core_session_t *session;
	switch_channel_t *channel;
	enum dmodem_law law;
	int linear;
	switch_codec_t codec;   /* linear: our L16 codec */
	int packet;             /* samples in a frame we make */
	unsigned char out[LINE_MAX_SAMPLES * 2];
};

/* libdmodem's log lines: "err: " ones as warnings, the rest as debug */
static void log_line(const char *line)
{
	if (!strncmp(line, "err: ", 5))
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				  "dmodem: %s", line + 5);
	else
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				  "dmodem: %s", line);
}

/* the bridge's log lines: a call's (call: its session) in the session's
   log */
static void bridge_log(void *call, enum dmbridge_level level, const char *line)
{
	static const switch_log_level_t lv[] = {
		[DMBRIDGE_ERROR] = SWITCH_LOG_ERROR,
		[DMBRIDGE_WARNING] = SWITCH_LOG_WARNING,
		[DMBRIDGE_INFO] = SWITCH_LOG_INFO,
		[DMBRIDGE_DEBUG] = SWITCH_LOG_DEBUG,
	};
	switch_core_session_t *session = call;

	if (session)
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), lv[level],
				  "dmodem: %s\n", line);
	else
		switch_log_printf(SWITCH_CHANNEL_LOG, lv[level], "dmodem: %s\n",
				  line);
}

/* yes, true, on, ... */
static int bridge_true(const char *value)
{
	return switch_true(value);
}

/* fork() and pthread_create() */
static const struct dmbridge_host host = {
	.log = bridge_log,
	.is_true = bridge_true,
};

/* <settings>: debug, the jitter buffer, the fax defaults (no cfg: the
   built-in ones) */
static void load_settings(switch_xml_t cfg, struct dmbridge_fax *f, int *jb_ms,
			  int *debug)
{
	switch_xml_t settings, param;
	char dir[256];

	snprintf(dir, sizeof(dir), "%s%sfax", SWITCH_GLOBAL_dirs.storage_dir,
		 SWITCH_PATH_SEPARATOR);
	dmbridge_fax_defaults(f, dir);
	*jb_ms = DEFAULT_JB_MS;
	if (debug)
		*debug = 0;
	if (!cfg || !(settings = switch_xml_child(cfg, "settings")))
		return;
	for (param = switch_xml_child(settings, "param"); param;
	     param = param->next) {
		const char *name = switch_xml_attr_soft(param, "name");
		const char *value = switch_xml_attr_soft(param, "value");
		if (dmbridge_fax_option(&host, f, name, value))
			continue;
		if (dmbridge_option_is(name, "jitter-buffer"))
			*jb_ms = atoi(value);
		else if (dmbridge_option_is(name, "debug") && debug)
			*debug = atoi(value);
	}
}

/* the settings of <settings> alone (for the fax applications) */
static void load_fax_settings(struct dmbridge_fax *f, int *jb_ms)
{
	switch_xml_t xml, cfg = NULL;

	xml = switch_xml_open_cfg(config_file, &cfg, NULL);
	load_settings(xml ? cfg : NULL, f, jb_ms, NULL);
	if (xml)
		switch_xml_free(xml);
}

/* <profiles><profile name="name"> of dmodem.conf into *p (fax and
   jitter buffer: <settings>, then the profile's own); -1 if there is no
   such profile or it lacks host and port (tcp) or command (exec) */
static int load_profile(const char *name, struct profile *p)
{
	switch_xml_t xml, cfg, profiles, prof, param;
	char where[96];

	dmbridge_profile_init(&p->b);
	if (!(xml = switch_xml_open_cfg(config_file, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				  "cannot load %s\n", config_file);
		return -1;
	}
	load_settings(cfg, &p->b.fax, &p->jb_ms, NULL);
	prof = NULL;
	if ((profiles = switch_xml_child(cfg, "profiles")))
		prof = switch_xml_find_child(profiles, "profile", "name",
					     name);
	if (!prof) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				  "%s: no profile %s\n", config_file, name);
		switch_xml_free(xml);
		return -1;
	}
	snprintf(where, sizeof(where), "profile %s", name);
	for (param = switch_xml_child(prof, "param"); param;
	     param = param->next) {
		const char *n = switch_xml_attr_soft(param, "name");
		const char *v = switch_xml_attr_soft(param, "value");
		if (dmbridge_profile_option(&host, &p->b, where, n, v))
			continue;
		if (dmbridge_option_is(n, "jitter-buffer"))
			p->jb_ms = atoi(v);
	}
	switch_xml_free(xml);
	return dmbridge_profile_check(&host, &p->b, where);
}

/*
 *    the line
 */

/* a jitter buffer of jb_ms on the channel, without packet loss
   concealment, unless the channel has one of its own (jitterbuffer_msec):
   the modem needs a frame of the line at each tick of the clock, and a
   packet a few ms late would otherwise be a frame of silence, and then a
   frame too many (the modem loses its training) */
static void line_jitter_buffer(switch_core_session_t *session, int jb_ms)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_core_session_message_t msg = { 0 };

	if (jb_ms <= 0 ||
	    switch_channel_get_variable(channel, "jitterbuffer_msec"))
		return;
	switch_channel_set_variable(channel, "rtp_jitter_buffer_plc", "false");
	msg.from = __FILE__;
	msg.string_arg = switch_core_session_sprintf(session, "%d", jb_ms);
	msg.message_id = SWITCH_MESSAGE_INDICATE_JITTER_BUFFER;
	switch_core_session_receive_message(session, &msg);
}

/* the line on the session: law (DMODEM_LAW_UNKNOWN: the channel's) */
static int line_open(struct line *l, switch_core_session_t *session,
		     enum dmodem_law law, int jb_ms)
{
	switch_codec_t *rc = switch_core_session_get_read_codec(session);
	const char *name = rc && rc->implementation ?
		rc->implementation->iananame : "";
	enum dmodem_law chan_law =
		!strcasecmp(name, "PCMU") ? DMODEM_LAW_ULAW :
		!strcasecmp(name, "PCMA") ? DMODEM_LAW_ALAW :
		DMODEM_LAW_UNKNOWN;

	memset(l, 0, sizeof(*l));
	l->session = session;
	l->channel = switch_core_session_get_channel(session);
	if (!rc || !rc->implementation) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
				  SWITCH_LOG_WARNING, "dmodem: no codec\n");
		return -1;
	}
	l->law = law ? law : chan_law ? chan_law : DMODEM_LAW_ULAW;
	line_jitter_buffer(session, jb_ms);
	if (chan_law == l->law &&
	    rc->implementation->actual_samples_per_second == DMODEM_RATE) {
		l->packet = rc->implementation->samples_per_packet;
		if (l->packet <= 0 || l->packet > LINE_MAX_SAMPLES)
			l->packet = 160;
		return 0;
	}
	/* another codec (or law): linear samples, FreeSWITCH transcodes;
	   no V.90, and the modem loses some speed */
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			  SWITCH_LOG_WARNING,
			  "dmodem: the channel's codec is %s@%d, not %s: transcoding\n",
			  name, rc->implementation->actual_samples_per_second,
			  l->law == DMODEM_LAW_ALAW ? "PCMA" : "PCMU");
	l->packet = 160;
	if (switch_core_codec_init(&l->codec, "L16", NULL, NULL, DMODEM_RATE,
				   20, 1, SWITCH_CODEC_FLAG_ENCODE |
				   SWITCH_CODEC_FLAG_DECODE, NULL,
				   switch_core_session_get_pool(session)) !=
	    SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
				  SWITCH_LOG_WARNING,
				  "dmodem: no L16 codec\n");
		return -1;
	}
	switch_core_session_set_read_codec(session, &l->codec);
	l->linear = 1;
	return 0;
}

/* the channel's own read codec back, ours destroyed (if linear) */
static void line_close(struct line *l)
{
	if (!l->linear)
		return;
	/* the channel's own again */
	switch_core_session_set_read_codec(l->session, NULL);
	switch_core_codec_destroy(&l->codec);
	l->linear = 0;
}

/* the modem's samples on the line */
static void line_write(struct line *l, int n)
{
	switch_frame_t wf = { 0 };

	wf.codec = l->linear ? &l->codec :
		switch_core_session_get_write_codec(l->session);
	wf.data = l->out;
	wf.datalen = l->linear ? n * 2 : n;
	wf.samples = n;
	wf.rate = DMODEM_RATE;
	switch_core_session_write_frame(l->session, &wf, SWITCH_IO_FLAG_NONE,
					0);
}

/* the next frame from the line (silence for a frame that is missing or
   comfort noise) through process, its answer written back: the samples,
   -1 when the channel is gone */
typedef void (*line_fn)(void *obj, const void *in, void *out, int n,
			int linear);

/* fn(obj, in, l->out, n, l->linear): the modem or the fax machine on
   the frame, n samples (linear) or G.711 octets */
static int line_step(struct line *l, line_fn fn, void *obj)
{
	static const short zeros[LINE_MAX_SAMPLES];
	unsigned char sil[LINE_MAX_SAMPLES];
	switch_frame_t *f = NULL;
	switch_status_t s;
	const void *in;
	int n;

	if (!switch_channel_ready(l->channel))
		return -1;
	s = switch_core_session_read_frame(l->session, &f, SWITCH_IO_FLAG_NONE,
					   0);
	if (!SWITCH_READ_ACCEPTABLE(s))
		return -1;
	if (!f || switch_test_flag(f, SFF_CNG) || !f->datalen ||
	    !f->data) {
		/* nothing from the line (silence suppression, a lost
		   packet): the modem goes on with silence */
		n = f && f->samples > 0 && f->samples <= LINE_MAX_SAMPLES ?
			(int)f->samples : l->packet;
		if (l->linear) {
			in = zeros;
		} else {
			memset(sil, l->law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF,
			       n);
			in = sil;
		}
	} else {
		n = l->linear ? (int)f->datalen / 2 : (int)f->datalen;
		if (n > LINE_MAX_SAMPLES)
			n = LINE_MAX_SAMPLES;
		in = f->data;
	}
	fn(obj, in, l->out, n, l->linear);
	line_write(l, n);
	return n;
}

/* line_fn of the modem (obj: struct dmodem) */
static void modem_fn(void *obj, const void *in, void *out, int n, int linear)
{
	if (linear)
		dmodem_process(obj, in, out, n);
	else
		dmodem_process_g711(obj, in, out, n);
}

/* line_fn of the fax machine (obj: struct dmodem_fax) */
static void fax_fn(void *obj, const void *in, void *out, int n, int linear)
{
	if (linear)
		dmodem_fax_process(obj, in, out, n);
	else
		dmodem_fax_process_g711(obj, in, out, n);
}

/*
 *    fax
 */

/* where a received fax goes: <dir>/<date>-<time>-<caller>-<uuid>.tif */
static void fax_file_name(switch_core_session_t *session, const char *dir,
			  char *path, size_t len)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *num = switch_channel_get_variable(channel,
						      "caller_id_number");
	char when[32], caller[32] = "", id[64];
	time_t now = time(NULL);
	struct tm tm;
	size_t k, n = 0;

	localtime_r(&now, &tm);
	strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &tm);
	for (k = 0; num && num[k] && n < sizeof(caller) - 1; k++)
		if (isalnum((unsigned char)num[k]) || num[k] == '+')
			caller[n++] = num[k];
	caller[n] = 0;
	switch_copy_string(id, switch_core_session_get_uuid(session),
			   sizeof(id));
	for (k = 0; id[k]; k++)
		if (!isalnum((unsigned char)id[k]) && id[k] != '-')
			id[k] = '_';
	snprintf(path, len, "%s%s%s-%s-%s.tif", dir, SWITCH_PATH_SEPARATOR,
		 when, *caller ? caller : "unknown", id);
}

/* a fax call on the line until it ends; -1 if the channel hung up */
static int run_fax(struct line *l, enum dmodem_fax_dir dir, const char *file,
		   const struct dmbridge_fax *fs)
{
	struct dmodem_fax_config cfg = { 0 };
	struct dmodem_fax_status st = { 0 };
	struct dmodem_fax *f;
	switch_channel_t *channel = l->channel;
	int res = 0;

	cfg.law = l->law;
	cfg.dir = dir;
	cfg.file = file;
	cfg.ident = fs->ident;
	cfg.max_bps = fs->max_bps;
	cfg.no_ecm = !fs->ecm;
	switch_channel_set_variable(channel, "dmodem_fax_file", file);
	f = dmodem_fax_create(&cfg);
	if (!f) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel),
				  SWITCH_LOG_WARNING,
				  "dmodem: cannot %s fax file %s\n",
				  dir == DMODEM_FAX_SEND ? "read" : "use",
				  file);
		switch_channel_set_variable(channel, "dmodem_fax_status",
					    "FAILED");
		switch_channel_set_variable(channel, "dmodem_fax_result",
					    "no document");
		return 0;
	}
	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel),
			  SWITCH_LOG_INFO, "dmodem fax: %s %s\n",
			  dir == DMODEM_FAX_SEND ? "sending" : "receiving",
			  file);
	for (;;) {
		if (line_step(l, fax_fn, f) < 0) {
			dmodem_fax_hangup(f);
			res = -1;
			break;
		}
		dmodem_fax_status(f, &st);
		if (st.done)
			break;
	}
	dmodem_fax_status(f, &st);
	dmodem_fax_destroy(f);
	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel),
			  SWITCH_LOG_INFO,
			  "dmodem fax %s: %s, %d pages at %d bit/s%s, remote '%s'\n",
			  dir == DMODEM_FAX_SEND ? "sent" : "received",
			  st.result, st.pages, st.bps, st.ecm ? " (ECM)" : "",
			  st.remote_ident);
	switch_channel_set_variable(channel, "dmodem_fax_status",
				    st.ok ? "SUCCESS" : "FAILED");
	switch_channel_set_variable(channel, "dmodem_fax_result", st.result);
	switch_channel_set_variable_printf(channel, "dmodem_fax_pages", "%d",
					   st.pages);
	switch_channel_set_variable_printf(channel, "dmodem_fax_rate", "%d",
					   st.bps);
	switch_channel_set_variable(channel, "dmodem_fax_remote",
				    st.remote_ident);
	switch_channel_set_variable(channel, "dmodem_fax_ecm",
				    st.ecm ? "true" : "false");
	return res;
}

/*
 *    the applications
 */

#define DMODEM_SYNTAX "[profile][,o]"
#define DMODEM_DESC \
	"Answers the call with a software modem (V.21 to V.34, V.90 as the\n" \
	"digital modem); once connected, the data goes to the TCP server\n" \
	"(mode tcp) or the program on a pty (mode exec) of the profile in\n" \
	"dmodem.conf (\"default\" if none is given).  o, after a comma or a\n" \
	"space: originate, the modem calls instead of answering.  Sets\n" \
	"dmodem_status (NOCARRIER, REMOTE, LOCAL, HANGUP, FAX or ERROR) and\n" \
	"dmodem_rate (transmit/receive bit/s).  The channel should be G.711\n" \
	"end to end, without echo cancellation, jitter buffer or silence\n" \
	"suppression.\n"

/* dmodem [profile][,o]: the modem on the channel until the call, the
   session or the time to connect ends (a calling fax machine goes to
   run_fax()); dmodem_status and dmodem_rate set */
SWITCH_STANDARD_APP(dmodem_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct profile p;
	struct line l;
	struct dmbridge_call c;
	enum dmodem_law law;
	char *argv[3] = { 0 }, *mydata, name[64];
	const char *profile, *status = "ERROR";
	int orig, line_ok = 0;

	dmbridge_call_init(&c);
	mydata = switch_core_session_strdup(session, data ? data : "");
	/* "profile o" or "profile,o" (no space to quote in an originate) */
	for (char *ch = mydata; *ch; ch++)
		if (*ch == ',')
			*ch = ' ';
	switch_separate_string(mydata, ' ', argv, 3);
	profile = !zstr(argv[0]) ? argv[0] : "default";
	orig = argv[1] && strchr(argv[1], 'o');
	if (load_profile(profile, &p))
		goto out;

	if (switch_channel_answer(channel) != SWITCH_STATUS_SUCCESS) {
		status = "HANGUP";
		goto out;
	}
	if (line_open(&l, session, p.b.law, p.jb_ms))
		goto out;
	line_ok = 1;
	/* transcoded: whatever law the network has, the modem does not see
	   its octets (no V.90) */
	law = l.linear ? DMODEM_LAW_UNKNOWN : l.law;

	switch_copy_string(name, switch_channel_get_name(channel),
			   sizeof(name));
	if (dmbridge_call_start(&c, &host, session, &p.b, name, law, orig))
		goto out;
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			  "dmodem %s (profile %s, %s%s)\n",
			  orig ? "calling" : "answering", profile,
			  law == DMODEM_LAW_ALAW ? "PCMA" : "PCMU",
			  l.linear ? " transcoded" : "");

	for (;;) {
		enum dmbridge_event ev;
		int n = line_step(&l, modem_fn, c.d);

		if (n < 0) {
			c.status = "HANGUP";
			break;
		}
		ev = dmbridge_call_step(&c, n);
		if (ev == DMBRIDGE_CONNECTED) {
			const char *cid = switch_channel_get_variable(channel,
							"caller_id_number");
			const char *dialed = switch_channel_get_variable(
				channel, "destination_number");
			char connected[24];
			const char *env[] = {
				"DMODEM_CHANNEL", name,
				"DMODEM_UUID", switch_core_session_get_uuid(session),
				"DMODEM_PROFILE", profile,
				"DMODEM_RATE", c.rate,
				"DMODEM_CALLERID", cid ? cid : "",
				"DMODEM_DIALED", dialed ? dialed : "",
				"DMODEM_CONNECTED", connected,
				NULL
			};
			snprintf(connected, sizeof(connected), "%lld",
				 (long long)time(NULL));
			switch_channel_set_variable(channel, "dmodem_rate",
						    c.rate);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
					  SWITCH_LOG_INFO,
					  "dmodem connected at %s bit/s\n", c.rate);
			ev = dmbridge_call_serve(&c, env);
		}
		if (ev == DMBRIDGE_FAX) {
			/* a fax machine calling: a fax receiver instead of
			   the modem */
			char path[1024];
			switch_dir_make_recursive(p.b.fax.dir,
				SWITCH_DEFAULT_DIR_PERMS,
				switch_core_session_get_pool(session));
			fax_file_name(session, p.b.fax.dir, path, sizeof(path));
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
					  SWITCH_LOG_INFO,
					  "dmodem: a fax machine is calling\n");
			run_fax(&l, DMODEM_FAX_RECEIVE, path, &p.b.fax);
			break;
		}
		if (ev == DMBRIDGE_END)
			break;
	}
	dmbridge_call_hangup(&c);
	status = c.status;
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			  "dmodem done: %s\n", status);

out:
	dmbridge_call_end(&c);
	if (line_ok)
		line_close(&l);
	switch_channel_set_variable(channel, "dmodem_status", status);
}

#define FAX_SYNTAX "<file> [ident]"

/* dmodem_send_fax / dmodem_receive_fax <file> [ident] in direction dir */
static void fax_app(switch_core_session_t *session, const char *data,
		    enum dmodem_fax_dir dir)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct dmbridge_fax fs;
	struct line l;
	char *argv[3] = { 0 }, *mydata;
	int argc, jb_ms;

	mydata = switch_core_session_strdup(session, data ? data : "");
	/* the file, the rest the ident (it may have spaces) */
	argc = switch_separate_string(mydata, ' ', argv, 2);
	if (argc < 1 || zstr(argv[0])) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
				  SWITCH_LOG_WARNING,
				  "dmodem fax: a file name is needed\n");
		switch_channel_set_variable(channel, "dmodem_fax_status",
					    "FAILED");
		return;
	}
	load_fax_settings(&fs, &jb_ms);
	if (argc > 1 && !zstr(argv[1]))
		switch_copy_string(fs.ident, argv[1], sizeof(fs.ident));
	if (switch_channel_answer(channel) != SWITCH_STATUS_SUCCESS)
		return;
	if (line_open(&l, session, DMODEM_LAW_UNKNOWN, jb_ms)) {
		switch_channel_set_variable(channel, "dmodem_fax_status",
					    "FAILED");
		return;
	}
	run_fax(&l, dir, argv[0], &fs);
	line_close(&l);
}

/* dmodem_send_fax */
SWITCH_STANDARD_APP(send_fax_function)
{
	fax_app(session, data, DMODEM_FAX_SEND);
}

/* dmodem_receive_fax */
SWITCH_STANDARD_APP(receive_fax_function)
{
	fax_app(session, data, DMODEM_FAX_RECEIVE);
}

/* libdmodem's log level: debug of <settings> (0 if none) */
static void load_config(void)
{
	switch_xml_t xml, cfg;
	struct dmbridge_fax f;
	int debug = 0, jb_ms;

	if ((xml = switch_xml_open_cfg(config_file, &cfg, NULL))) {
		load_settings(cfg, &f, &jb_ms, &debug);
		switch_xml_free(xml);
	}
	dmodem_set_debug(debug);
}

/* the log hook, the pumps, <settings>, the three applications */
SWITCH_MODULE_LOAD_FUNCTION(mod_dmodem_load)
{
	switch_application_interface_t *app_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool,
								      modname);
	dmodem_set_log(log_line);
	dmodem_init();
	load_config();
	SWITCH_ADD_APP(app_interface, "dmodem",
		       "Answer the call with a data modem, connect it to a BBS or pppd",
		       DMODEM_DESC, dmodem_function, DMODEM_SYNTAX, SAF_NONE);
	SWITCH_ADD_APP(app_interface, "dmodem_send_fax",
		       "Send a fax (TIFF file) with DModem's fax",
		       "Sends the TIFF file as a group 3 fax on a call placed to a fax\n"
		       "machine.  ident: our number (TSI), else fax-ident of dmodem.conf.\n"
		       "Sets dmodem_fax_status (SUCCESS or FAILED), dmodem_fax_result,\n"
		       "dmodem_fax_pages, dmodem_fax_rate, dmodem_fax_remote, dmodem_fax_ecm,\n"
		       "dmodem_fax_file.\n",
		       send_fax_function, FAX_SYNTAX, SAF_NONE);
	SWITCH_ADD_APP(app_interface, "dmodem_receive_fax",
		       "Receive a fax into a TIFF file with DModem's fax",
		       "Answers the call as a group 3 fax machine and writes what it receives\n"
		       "to the TIFF file.  ident: our number (CSI).  Sets the same variables\n"
		       "as dmodem_send_fax.\n",
		       receive_fax_function, FAX_SYNTAX, SAF_NONE);
	return SWITCH_STATUS_SUCCESS;
}

/* nothing to free */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dmodem_shutdown)
{
	return SWITCH_STATUS_SUCCESS;
}
