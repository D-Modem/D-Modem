/*
 * app_dmodem.c - a data modem on an Asterisk channel
 *
 * The call is answered by a software modem (V.21 to V.34, and V.90 as the
 * digital modem when the channel is G.711 end to end); once the modems
 * are connected, the data goes to a TCP server (a BBS, a telnet host) or
 * to a program on a pty (pppd, a getty, a door) as dmodem.conf's profile
 * says.
 *
 * The modem is libdmodem (../slmodemd/dmodem.h): the slmodemd modem core
 * with the open data pumps, no binary-only parts.
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2.
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/format_cache.h"
#include "asterisk/frame.h"
#include "asterisk/localtime.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/paths.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#include "dmodem.h"

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
			<para>Sets these variables on exit:</para>
			<variablelist>
				<variable name="DMODEMSTATUS">
					<value name="NOCARRIER">The modems did not connect.</value>
					<value name="REMOTE">The remote modem went away.</value>
					<value name="LOCAL">The server or program ended the session.</value>
					<value name="HANGUP">The channel hung up.</value>
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
	"  Sets DMODEMSTATUS (NOCARRIER, REMOTE, LOCAL, HANGUP or ERROR) and\n"
	"DMODEMRATE (transmit/receive bit/s).  The channel should be G.711\n"
	"end to end, without echo cancellation, jitter buffer or silence\n"
	"suppression.\n";

#define LINE_MAX_SAMPLES 1600   /* in one frame, 200 ms */
#define IDLE_MS 200             /* no frame for this long: go on alone */

enum mode { MODE_TCP, MODE_EXEC };

/* fax: [general] has the defaults, a profile its own */
struct fax_settings {
	int on;                 /* DModem(): a fax caller is received */
	char dir[256];          /* ... into this directory */
	char ident[21];         /* our number (CSI / TSI) */
	int max_bps;
};

struct profile {
	enum mode mode;
	char host[256];
	char port[16];
	char command[1024];
	char init[128];         /* extra AT commands, e.g. +MS=34 */
	enum dmodem_law law;    /* DMODEM_LAW_UNKNOWN: the channel's */
	int timeout;            /* seconds to connect */
	struct fax_settings fax;
};

/* the bytes between the modem and the server or program */
struct pipe_buf {
	char data[4096];
	int len;
};

static int nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void log_line(const char *line)
{
	if (!strncmp(line, "err: ", 5))
		ast_log(LOG_WARNING, "dmodem: %s", line + 5);
	else
		ast_verb(5, "dmodem: %s", line);
}

static void fax_defaults(struct fax_settings *f)
{
	f->on = 1;
	snprintf(f->dir, sizeof(f->dir), "%s/fax", ast_config_AST_SPOOL_DIR);
	f->ident[0] = 0;
	f->max_bps = 14400;
}

/* a fax option; 0 if it is something else */
static int fax_option(struct fax_settings *f, const char *name,
		      const char *value)
{
	if (!strcasecmp(name, "fax"))
		f->on = ast_true(value);
	else if (!strcasecmp(name, "fax_dir"))
		ast_copy_string(f->dir, value, sizeof(f->dir));
	else if (!strcasecmp(name, "fax_ident"))
		ast_copy_string(f->ident, value, sizeof(f->ident));
	else if (!strcasecmp(name, "fax_max_rate"))
		f->max_bps = atoi(value);
	else
		return 0;
	return 1;
}

static void load_fax_general(struct ast_config *cfg, struct fax_settings *f)
{
	struct ast_variable *v;
	fax_defaults(f);
	for (v = ast_variable_browse(cfg, "general"); v; v = v->next)
		fax_option(f, v->name, v->value);
}

/* the fax settings of [general] alone (for DModemSendFax/ReceiveFax) */
static void load_fax_settings(struct fax_settings *f)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg = ast_config_load2(config_file, "app_dmodem",
						  flags);
	fax_defaults(f);
	if (cfg && cfg != CONFIG_STATUS_FILEINVALID) {
		load_fax_general(cfg, f);
		ast_config_destroy(cfg);
	}
}

static int load_profile(const char *name, struct profile *p)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg;
	struct ast_variable *v;

	memset(p, 0, sizeof(*p));
	p->mode = MODE_TCP;
	p->timeout = 60;
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
	for (v = ast_variable_browse(cfg, name); v; v = v->next) {
		if (fax_option(&p->fax, v->name, v->value))
			continue;
		if (!strcasecmp(v->name, "mode")) {
			if (!strcasecmp(v->value, "tcp"))
				p->mode = MODE_TCP;
			else if (!strcasecmp(v->value, "exec"))
				p->mode = MODE_EXEC;
			else
				ast_log(LOG_WARNING, "[%s]: unknown mode %s\n",
					name, v->value);
		} else if (!strcasecmp(v->name, "host")) {
			ast_copy_string(p->host, v->value, sizeof(p->host));
		} else if (!strcasecmp(v->name, "port")) {
			ast_copy_string(p->port, v->value, sizeof(p->port));
		} else if (!strcasecmp(v->name, "command")) {
			ast_copy_string(p->command, v->value,
					sizeof(p->command));
		} else if (!strcasecmp(v->name, "init")) {
			const char *s = v->value;
			if (!strncasecmp(s, "AT", 2))
				s += 2;
			ast_copy_string(p->init, s, sizeof(p->init));
		} else if (!strcasecmp(v->name, "law")) {
			if (!strcasecmp(v->value, "ulaw"))
				p->law = DMODEM_LAW_ULAW;
			else if (!strcasecmp(v->value, "alaw"))
				p->law = DMODEM_LAW_ALAW;
			else
				p->law = DMODEM_LAW_UNKNOWN;
		} else if (!strcasecmp(v->name, "timeout")) {
			p->timeout = atoi(v->value);
		}
	}
	ast_config_destroy(cfg);
	if (p->mode == MODE_TCP && (!*p->host || !*p->port)) {
		ast_log(LOG_ERROR, "[%s]: mode=tcp needs host and port\n",
			name);
		return -1;
	}
	if (p->mode == MODE_EXEC && !*p->command) {
		ast_log(LOG_ERROR, "[%s]: mode=exec needs command\n", name);
		return -1;
	}
	return 0;
}

static int connect_tcp(const struct profile *p)
{
	struct addrinfo hints = { 0 }, *res, *ai;
	int fd = -1, err;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	err = getaddrinfo(p->host, p->port, &hints, &res);
	if (err) {
		ast_log(LOG_WARNING, "dmodem: %s: %s\n", p->host,
			gai_strerror(err));
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		ast_log(LOG_WARNING, "dmodem: cannot connect to %s:%s: %s\n",
			p->host, p->port, strerror(errno));
		return -1;
	}
	nonblock(fd);
	return fd;
}

/* the program on the slave of a new pty, the master returned */
static int start_exec(const struct profile *p, const char *const *env,
		      pid_t *pid)
{
	char slave[64];
	int master, i;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) || unlockpt(master) ||
	    ptsname_r(master, slave, sizeof(slave))) {
		ast_log(LOG_WARNING, "dmodem: no pty: %s\n", strerror(errno));
		if (master >= 0)
			close(master);
		return -1;
	}
	*pid = ast_safe_fork(0);
	if (*pid < 0) {
		ast_log(LOG_WARNING, "dmodem: fork: %s\n", strerror(errno));
		close(master);
		return -1;
	}
	if (*pid == 0) {
		struct termios t;
		int fd;
		setsid();
		/* the pty becomes the program's controlling tty: closing
		   the master hangs it up */
		fd = open(slave, O_RDWR);
		if (fd < 0)
			_exit(127);
		if (!tcgetattr(fd, &t)) {
			cfmakeraw(&t);
			tcsetattr(fd, TCSANOW, &t);
		}
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		ast_close_fds_above_n(2);
		for (i = 0; env[i]; i += 2)
			setenv(env[i], env[i + 1], 1);
		execl("/bin/sh", "sh", "-c", p->command, (char *)NULL);
		_exit(127);
	}
	nonblock(master);
	return master;
}

/* move what is there from one fd to the other through b (from -1: only
   what b holds); -1 when the source is closed */
static int relay(int from, int to, struct pipe_buf *b)
{
	int n, eof = 0;

	if (from >= 0 && b->len < (int)sizeof(b->data)) {
		n = read(from, b->data + b->len, sizeof(b->data) - b->len);
		if (n > 0)
			b->len += n;
		else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			eof = 1;        /* a pty master reads EIO at the end */
			ast_verb(4, "dmodem: fd %d read %d (%s)\n", from, n,
				 n ? strerror(errno) : "end of file");
		}
	}
	if (b->len > 0) {
		n = write(to, b->data, b->len);
		if (n > 0) {
			memmove(b->data, b->data + n, b->len - n);
			b->len -= n;
		}
	}
	return eof ? -1 : 0;
}

static void drain(int fd)
{
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

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

/* a fax call on the channel until it ends; -1 if the channel hung up */
static int run_fax(struct ast_channel *chan, struct ast_format *fmt,
		   enum dmodem_law law, enum dmodem_fax_dir dir,
		   const char *file, const struct fax_settings *fs,
		   unsigned char *out)
{
	struct dmodem_fax_config cfg = { 0 };
	struct dmodem_fax_status st = { 0 };
	struct dmodem_fax *f;
	const char *name = ast_channel_name(chan);
	char buf[32];
	int res = 0;

	cfg.law = law;
	cfg.dir = dir;
	cfg.file = file;
	cfg.ident = fs->ident;
	cfg.max_bps = fs->max_bps;
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
	for (;;) {
		struct ast_frame *fr;
		int n = ast_waitfor(chan, IDLE_MS);

		if (n < 0 || (n > 0 && !(fr = ast_read(chan)))) {
			dmodem_fax_hangup(f);
			res = -1;
			break;
		}
		if (n == 0) {
			unsigned char sil[160];
			struct ast_frame wf = { .frametype = AST_FRAME_VOICE,
						.src = app };
			memset(sil, law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF,
			       sizeof(sil));
			dmodem_fax_process_g711(f, sil, out + AST_FRIENDLY_OFFSET,
						sizeof(sil));
			wf.subclass.format = fmt;
			wf.data.ptr = out + AST_FRIENDLY_OFFSET;
			wf.offset = AST_FRIENDLY_OFFSET;
			wf.datalen = wf.samples = sizeof(sil);
			ast_write(chan, &wf);
		} else {
			if (fr->frametype == AST_FRAME_VOICE &&
			    fr->datalen > 0 &&
			    ast_format_cmp(fr->subclass.format, fmt) ==
			    AST_FORMAT_CMP_EQUAL) {
				struct ast_frame wf = {
					.frametype = AST_FRAME_VOICE,
					.src = app };
				int len = fr->datalen;
				if (len > LINE_MAX_SAMPLES)
					len = LINE_MAX_SAMPLES;
				dmodem_fax_process_g711(f, fr->data.ptr,
					out + AST_FRIENDLY_OFFSET, len);
				wf.subclass.format = fmt;
				wf.data.ptr = out + AST_FRIENDLY_OFFSET;
				wf.offset = AST_FRIENDLY_OFFSET;
				wf.datalen = wf.samples = len;
				ast_write(chan, &wf);
			}
			ast_frfree(fr);
		}
		dmodem_fax_status(f, &st);
		if (st.done)
			break;
	}
	dmodem_fax_status(f, &st);
	dmodem_fax_destroy(f);
	ast_verb(3, "%s: DModem fax %s: %s, %d pages at %d bit/s, remote '%s'\n",
		 name, dir == DMODEM_FAX_SEND ? "sent" : "received", st.result,
		 st.pages, st.bps, st.remote_ident);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXSTATUS",
				  st.ok ? "SUCCESS" : "FAILED");
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRESULT", st.result);
	snprintf(buf, sizeof(buf), "%d", st.pages);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXPAGES", buf);
	snprintf(buf, sizeof(buf), "%d", st.bps);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXRATE", buf);
	pbx_builtin_setvar_helper(chan, "DMODEMFAXREMOTE", st.remote_ident);
	return res;
}

static int dmodem_exec(struct ast_channel *chan, const char *data)
{
	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(profile); AST_APP_ARG(options););
	struct profile p;
	struct dmodem_config cfg = { 0 };
	struct dmodem_status st;
	struct dmodem *d = NULL;
	struct ast_format *fmt;
	struct pipe_buf to_line = { .len = 0 }, to_host = { .len = 0 };
	char *parse, init[192], rate[32] = "", name[64];
	const char *status = "ERROR";
	unsigned char out[AST_FRIENDLY_OFFSET + LINE_MAX_SAMPLES];
	long long samples = 0, deadline, closing = -1;
	int dte[2] = { -1, -1 }, host = -1, online = 0, res = 0, orig;
	pid_t pid = 0;

	parse = ast_strdupa(S_OR(data, ""));
	AST_STANDARD_APP_ARGS(args, parse);
	if (ast_strlen_zero(args.profile))
		args.profile = "default";
	if (load_profile(args.profile, &p))
		goto out;
	orig = args.options && strchr(args.options, 'o');

	cfg.law = p.law ? p.law : channel_law(chan);
	fmt = cfg.law == DMODEM_LAW_ALAW ? ast_format_alaw : ast_format_ulaw;
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

	/* the modem's DTE: one end of a socketpair, the other end ours */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, dte)) {
		ast_log(LOG_WARNING, "socketpair: %s\n", strerror(errno));
		goto out;
	}
	nonblock(dte[0]);
	nonblock(dte[1]);
	/* no echo, no result codes, no auto answer, no +++ escape; the call
	   is there already: no dial tone to wait for */
	snprintf(init, sizeof(init), "ATE0Q1S0=0S2=255X3%s\r", p.init);
	ast_copy_string(name, ast_channel_name(chan), sizeof(name));
	cfg.name = name;
	cfg.dte_fd = dte[0];
	cfg.init = init;
	d = dmodem_create(&cfg);
	if (!d) {
		ast_log(LOG_WARNING, "%s: no modem\n", name);
		goto out;
	}
	drain(dte[1]);
	if (orig)
		dmodem_dial(d, "");
	else
		dmodem_answer(d);
	ast_verb(3, "%s: DModem %s (profile %s, %s)\n", name,
		 orig ? "calling" : "answering", args.profile,
		 ast_format_get_name(fmt));

	status = "NOCARRIER";
	deadline = (long long)p.timeout * DMODEM_RATE;
	for (;;) {
		struct ast_frame *f;
		int n = ast_waitfor(chan, IDLE_MS);

		if (n < 0) {
			status = "HANGUP";
			res = -1;
			break;
		}
		if (n == 0) {
			/* nothing from the line (no clock on the channel,
			   silence suppression): keep the modem going on
			   silence */
			unsigned char sil[160];
			memset(sil, cfg.law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF,
			       sizeof(sil));
			line_io(chan, d, fmt, sil, sizeof(sil), out);
			samples += sizeof(sil);
		} else {
			f = ast_read(chan);
			if (!f) {
				status = "HANGUP";
				res = -1;
				break;
			}
			/* a frame out for each frame in: the line's clock */
			if (f->frametype == AST_FRAME_VOICE &&
			    f->datalen > 0 &&
			    ast_format_cmp(f->subclass.format, fmt) ==
			    AST_FORMAT_CMP_EQUAL)
				samples += line_io(chan, d, fmt, f->data.ptr,
						   f->datalen, out);
			ast_frfree(f);
		}

		dmodem_poll_dte(d);
		dmodem_status(d, &st);
		if (!online) {
			drain(dte[1]);
			if (!orig && p.fax.on && st.fax_caller) {
				/* a fax machine calling: a fax receiver
				   instead of the modem */
				char path[1024];
				dmodem_destroy(d);
				d = NULL;
				ast_mkdir(p.fax.dir, 0755);
				fax_file_name(chan, p.fax.dir, path,
					      sizeof(path));
				ast_verb(3, "%s: DModem: a fax machine is calling\n",
					 name);
				status = "FAX";
				if (run_fax(chan, fmt, cfg.law,
					    DMODEM_FAX_RECEIVE, path, &p.fax,
					    out) < 0)
					res = -1;
				break;
			}
			if (st.link == DMODEM_ONLINE) {
				online = 1;
				snprintf(rate, sizeof(rate), "%u/%u",
					 st.tx_rate, st.rx_rate);
				pbx_builtin_setvar_helper(chan, "DMODEMRATE",
							  rate);
				ast_verb(3, "%s: DModem connected at %s bit/s\n",
					 name, rate);
				if (p.mode == MODE_TCP) {
					host = connect_tcp(&p);
				} else {
					const char *env[] = {
						"DMODEM_CHANNEL", name,
						"DMODEM_PROFILE", args.profile,
						"DMODEM_RATE", rate,
						"DMODEM_CALLERID",
						S_COR(ast_channel_caller(chan)->id.number.valid,
						      ast_channel_caller(chan)->id.number.str, ""),
						NULL
					};
					host = start_exec(&p, env, &pid);
				}
				if (host < 0) {
					status = "ERROR";
					break;
				}
				ast_verb(4, "%s: DModem: server fd %d, dte fds %d %d\n",
					 name, host, dte[0], dte[1]);
			} else if (st.link == DMODEM_IDLE ||
				   st.link == DMODEM_HANGUP ||
				   samples > deadline) {
				break;
			}
			continue;
		}
		/* online */
		relay(dte[1], host, &to_host);
		if (st.link != DMODEM_ONLINE) {
			status = "REMOTE";
			break;
		}
		if (closing < 0) {
			if (relay(host, dte[1], &to_line) < 0) {
				ast_debug(1, "%s: the server closed (%s)\n",
					  name, strerror(errno));
				ast_verb(4, "%s: DModem: the server or program ended the session\n",
					 name);
				closing = samples;
			}
			continue;
		}
		/* the server is gone: once what it sent last is on the
		   line, hang up */
		relay(-1, dte[1], &to_line);
		if ((!to_line.len && !st.tx_pending &&
		     samples - closing > DMODEM_RATE) ||
		    samples - closing > 30 * DMODEM_RATE) {
			status = "LOCAL";
			if (to_line.len || st.tx_pending)
				ast_log(LOG_WARNING, "%s: %d bytes not sent at the hangup\n",
					name, to_line.len + (int)st.tx_pending);
			break;
		}
	}
	if (d && online && st.link == DMODEM_ONLINE)
		dmodem_hangup(d);
	ast_verb(3, "%s: DModem done: %s\n", name, status);

out:
	if (d)
		dmodem_destroy(d);
	if (host >= 0)
		close(host);
	if (pid > 0)
		kill(pid, SIGHUP);      /* the reaper collects it */
	if (dte[0] >= 0)
		close(dte[0]);
	if (dte[1] >= 0)
		close(dte[1]);
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
	"DMODEMFAXPAGES, DMODEMFAXRATE, DMODEMFAXREMOTE, DMODEMFAXFILE.\n";
static const char app_recv[] = "DModemReceiveFax";
static const char recv_synopsis[] = "Receive a fax into a TIFF file with DModem's fax";
static const char recv_descrip[] =
	"  DModemReceiveFax(file[,ident]): answers the call as a group 3 fax\n"
	"machine and writes what it receives to the TIFF file (none if no page\n"
	"came).  ident: our number (CSI), else fax_ident of [general].\n"
	"  Sets the same variables as DModemSendFax.  DModem() receives faxes\n"
	"by itself when a fax machine calls (fax = yes, the default).\n";

static int fax_exec(struct ast_channel *chan, const char *data,
		    enum dmodem_fax_dir dir)
{
	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(file); AST_APP_ARG(ident););
	struct fax_settings fs;
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

static int send_exec(struct ast_channel *chan, const char *data)
{
	return fax_exec(chan, data, DMODEM_FAX_SEND);
}

static int recv_exec(struct ast_channel *chan, const char *data)
{
	return fax_exec(chan, data, DMODEM_FAX_RECEIVE);
}

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

static int unload_module(void)
{
	int r = ast_unregister_application(app);
	r |= ast_unregister_application(app_send);
	r |= ast_unregister_application(app_recv);
	return r;
}

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
