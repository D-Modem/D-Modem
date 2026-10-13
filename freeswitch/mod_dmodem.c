/*
 * mod_dmodem.c - a data modem on a FreeSWITCH channel
 *
 * The call is answered by a software modem (V.21 to V.34, and V.90 as the
 * digital modem when the channel is G.711 end to end); once the modems
 * are connected, the data goes to a TCP server (a BBS, a telnet host) or
 * to a program on a pty (pppd, a getty, a door) as dmodem.conf's profile
 * says.  The module is also a group 3 fax machine.
 *
 * The modem is libdmodem (../slmodemd/dmodem.h): the slmodemd modem core
 * with the open data pumps, no binary-only parts.  The same as Asterisk's
 * app_dmodem (../asterisk), on FreeSWITCH's API.
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2.
 */

#include <switch.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "dmodem.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_dmodem_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dmodem_shutdown);
SWITCH_MODULE_DEFINITION(mod_dmodem, mod_dmodem_load, mod_dmodem_shutdown,
			 NULL);

static const char config_file[] = "dmodem.conf";

#define LINE_MAX_SAMPLES 1600   /* in one frame, 200 ms */

enum mode { MODE_TCP, MODE_EXEC };

/* fax: <settings> has the defaults, a profile its own */
struct fax_settings {
	int on;                 /* dmodem: a fax caller is received */
	char dir[256];          /* ... into this directory */
	char ident[21];         /* our number (CSI / TSI) */
	int max_bps;
	int ecm;                /* error correction mode offered */
};

/* <settings> and a profile: the jitter buffer (ms, 0 none) */
#define DEFAULT_JB_MS 60

struct profile {
	enum mode mode;
	char host[256];
	char port[16];
	char command[1024];
	char init[128];         /* extra AT commands, e.g. +MS=34 */
	enum dmodem_law law;    /* DMODEM_LAW_UNKNOWN: the channel's */
	int timeout;            /* seconds to connect */
	int jb_ms;              /* the jitter buffer */
	struct fax_settings fax;
};

/* the bytes between the modem and the server or program */
struct pipe_buf {
	char data[4096];
	int len;
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

/* O_NONBLOCK on fd; -1 on failure */
static int nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

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

/* the built-in fax settings: on, <storage>/fax, 14400 bit/s, ECM */
static void fax_defaults(struct fax_settings *f)
{
	f->on = 1;
	snprintf(f->dir, sizeof(f->dir), "%s%sfax",
		 SWITCH_GLOBAL_dirs.storage_dir, SWITCH_PATH_SEPARATOR);
	f->ident[0] = 0;
	f->max_bps = 14400;
	f->ecm = 1;
}

/* a fax option; 0 if it is something else */
static int fax_option(struct fax_settings *f, const char *name,
		      const char *value)
{
	if (!strcasecmp(name, "fax"))
		f->on = switch_true(value);
	else if (!strcasecmp(name, "fax-dir"))
		switch_copy_string(f->dir, value, sizeof(f->dir));
	else if (!strcasecmp(name, "fax-ident"))
		switch_copy_string(f->ident, value, sizeof(f->ident));
	else if (!strcasecmp(name, "fax-max-rate"))
		f->max_bps = atoi(value);
	else if (!strcasecmp(name, "fax-ecm"))
		f->ecm = switch_true(value);
	else
		return 0;
	return 1;
}

/* <settings>: debug, the jitter buffer, the fax defaults */
static void load_settings(switch_xml_t cfg, struct fax_settings *f,
			  int *jb_ms, int *debug)
{
	switch_xml_t settings, param;

	fax_defaults(f);
	*jb_ms = DEFAULT_JB_MS;
	if (debug)
		*debug = 0;
	if (!(settings = switch_xml_child(cfg, "settings")))
		return;
	for (param = switch_xml_child(settings, "param"); param;
	     param = param->next) {
		const char *name = switch_xml_attr_soft(param, "name");
		const char *value = switch_xml_attr_soft(param, "value");
		if (fax_option(f, name, value))
			continue;
		if (!strcasecmp(name, "jitter-buffer"))
			*jb_ms = atoi(value);
		else if (!strcasecmp(name, "debug") && debug)
			*debug = atoi(value);
	}
}

/* the settings of <settings> alone (for the fax applications) */
static void load_fax_settings(struct fax_settings *f, int *jb_ms)
{
	switch_xml_t xml, cfg;

	if (!(xml = switch_xml_open_cfg(config_file, &cfg, NULL))) {
		fax_defaults(f);
		*jb_ms = DEFAULT_JB_MS;
		return;
	}
	load_settings(cfg, f, jb_ms, NULL);
	switch_xml_free(xml);
}

/* <profiles><profile name="name"> of dmodem.conf into *p (fax and
   jitter buffer: <settings>, then the profile's own); -1 if there is no
   such profile or it lacks host and port (tcp) or command (exec) */
static int load_profile(const char *name, struct profile *p)
{
	switch_xml_t xml, cfg, profiles, prof, param;

	memset(p, 0, sizeof(*p));
	p->mode = MODE_TCP;
	p->timeout = 60;
	if (!(xml = switch_xml_open_cfg(config_file, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				  "cannot load %s\n", config_file);
		return -1;
	}
	load_settings(cfg, &p->fax, &p->jb_ms, NULL);
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
	for (param = switch_xml_child(prof, "param"); param;
	     param = param->next) {
		const char *n = switch_xml_attr_soft(param, "name");
		const char *v = switch_xml_attr_soft(param, "value");
		if (fax_option(&p->fax, n, v))
			continue;
		if (!strcasecmp(n, "mode")) {
			if (!strcasecmp(v, "tcp"))
				p->mode = MODE_TCP;
			else if (!strcasecmp(v, "exec"))
				p->mode = MODE_EXEC;
			else
				switch_log_printf(SWITCH_CHANNEL_LOG,
						  SWITCH_LOG_WARNING,
						  "profile %s: unknown mode %s\n",
						  name, v);
		} else if (!strcasecmp(n, "host")) {
			switch_copy_string(p->host, v, sizeof(p->host));
		} else if (!strcasecmp(n, "port")) {
			switch_copy_string(p->port, v, sizeof(p->port));
		} else if (!strcasecmp(n, "command")) {
			switch_copy_string(p->command, v, sizeof(p->command));
		} else if (!strcasecmp(n, "init")) {
			if (!strncasecmp(v, "AT", 2))
				v += 2;
			switch_copy_string(p->init, v, sizeof(p->init));
		} else if (!strcasecmp(n, "law")) {
			if (!strcasecmp(v, "ulaw"))
				p->law = DMODEM_LAW_ULAW;
			else if (!strcasecmp(v, "alaw"))
				p->law = DMODEM_LAW_ALAW;
			else
				p->law = DMODEM_LAW_UNKNOWN;
		} else if (!strcasecmp(n, "timeout")) {
			p->timeout = atoi(v);
		} else if (!strcasecmp(n, "jitter-buffer")) {
			p->jb_ms = atoi(v);
		}
	}
	switch_xml_free(xml);
	if (p->mode == MODE_TCP && (!*p->host || !*p->port)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				  "profile %s: mode tcp needs host and port\n",
				  name);
		return -1;
	}
	if (p->mode == MODE_EXEC && !*p->command) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				  "profile %s: mode exec needs command\n",
				  name);
		return -1;
	}
	return 0;
}

/* the profile's host:port, the first address that takes it (a blocking
   connect), non-blocking once connected; -1 on failure */
static int connect_tcp(const struct profile *p)
{
	struct addrinfo hints = { 0 }, *res, *ai;
	int fd = -1, err;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	err = getaddrinfo(p->host, p->port, &hints, &res);
	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				  "dmodem: %s: %s\n", p->host,
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
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				  "dmodem: cannot connect to %s:%s: %s\n",
				  p->host, p->port, strerror(errno));
		return -1;
	}
	nonblock(fd);
	return fd;
}

/* in the child: none of FreeSWITCH's descriptors (sockets, files) */
static void close_fds_above_2(void)
{
#ifdef SYS_close_range
	if (!syscall(SYS_close_range, 3U, ~0U, 0))
		return;
#endif
	{
		long fd, max = sysconf(_SC_OPEN_MAX);
		if (max < 0 || max > 65536)
			max = 65536;
		for (fd = 3; fd < max; fd++)
			close(fd);
	}
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
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				  "dmodem: no pty: %s\n", strerror(errno));
		if (master >= 0)
			close(master);
		return -1;
	}
	*pid = fork();
	if (*pid < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				  "dmodem: fork: %s\n", strerror(errno));
		close(master);
		return -1;
	}
	if (*pid == 0) {
		struct termios t;
		sigset_t none;
		int fd;
		/* FreeSWITCH's threads block signals: the program gets
		   them */
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		for (i = 1; i < NSIG; i++)
			signal(i, SIG_DFL);
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
		close_fds_above_2();
		for (i = 0; env[i]; i += 2)
			setenv(env[i], env[i + 1], 1);
		execl("/bin/sh", "sh", "-c", p->command, (char *)NULL);
		_exit(127);
	}
	nonblock(master);
	return master;
}

/* the program's end: SIGHUP (its pty is closed as well), then it is
   collected, by force after a while */
static void end_exec(pid_t pid)
{
	int i;

	kill(pid, SIGHUP);
	for (i = 0; i < 50; i++) {
		pid_t r = waitpid(pid, NULL, WNOHANG);
		if (r == pid || (r < 0 && errno == ECHILD))
			return;
		switch_yield(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
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
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					  "dmodem: fd %d read %d (%s)\n", from,
					  n, n ? strerror(errno) :
					  "end of file");
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

/* what fd has, read and thrown away */
static void drain(int fd)
{
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0)
		;
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
		   const struct fax_settings *fs)
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
	struct dmodem_config cfg = { 0 };
	struct dmodem_status st;
	struct dmodem *d = NULL;
	struct pipe_buf to_line = { .len = 0 }, to_host = { .len = 0 };
	char *argv[3] = { 0 }, *mydata, init[192], rate[32] = "", name[64];
	const char *profile, *status = "ERROR";
	long long samples = 0, deadline, closing = -1;
	int dte[2] = { -1, -1 }, host = -1, online = 0, orig, line_ok = 0;
	pid_t pid = 0;

	mydata = switch_core_session_strdup(session, data ? data : "");
	/* "profile o" or "profile,o" (no space to quote in an originate) */
	for (char *c = mydata; *c; c++)
		if (*c == ',')
			*c = ' ';
	switch_separate_string(mydata, ' ', argv, 3);
	profile = !zstr(argv[0]) ? argv[0] : "default";
	orig = argv[1] && strchr(argv[1], 'o');
	if (load_profile(profile, &p))
		goto out;

	if (switch_channel_answer(channel) != SWITCH_STATUS_SUCCESS) {
		status = "HANGUP";
		goto out;
	}
	if (line_open(&l, session, p.law, p.jb_ms))
		goto out;
	line_ok = 1;
	/* transcoded: whatever law the network has, the modem does not see
	   its octets (no V.90) */
	cfg.law = l.linear ? DMODEM_LAW_UNKNOWN : l.law;

	/* the modem's DTE: one end of a socketpair, the other end ours */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, dte)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
				  SWITCH_LOG_WARNING, "socketpair: %s\n",
				  strerror(errno));
		goto out;
	}
	nonblock(dte[0]);
	nonblock(dte[1]);
	/* no echo, no result codes, no auto answer, no +++ escape; the call
	   is there already: no dial tone to wait for */
	snprintf(init, sizeof(init), "ATE0Q1S0=0S2=255X3%s\r", p.init);
	switch_copy_string(name, switch_channel_get_name(channel),
			   sizeof(name));
	cfg.name = name;
	cfg.dte_fd = dte[0];
	cfg.init = init;
	d = dmodem_create(&cfg);
	if (!d) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
				  SWITCH_LOG_WARNING, "dmodem: no modem\n");
		goto out;
	}
	drain(dte[1]);
	if (orig)
		dmodem_dial(d, "");
	else
		dmodem_answer(d);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			  "dmodem %s (profile %s, %s%s)\n",
			  orig ? "calling" : "answering", profile,
			  cfg.law == DMODEM_LAW_ALAW ? "PCMA" : "PCMU",
			  l.linear ? " transcoded" : "");

	status = "NOCARRIER";
	deadline = (long long)p.timeout * DMODEM_RATE;
	for (;;) {
		int n = line_step(&l, modem_fn, d);

		if (n < 0) {
			status = "HANGUP";
			break;
		}
		samples += n;
		dmodem_poll_dte(d);
		dmodem_status(d, &st);
		if (!online) {
			/* (not what the link delivered with ONLINE: the
			   remote's first data) */
			if (st.link != DMODEM_ONLINE)
				drain(dte[1]);
			if (!orig && p.fax.on && st.fax_caller) {
				/* a fax machine calling: a fax receiver
				   instead of the modem */
				char path[1024];
				dmodem_destroy(d);
				d = NULL;
				switch_dir_make_recursive(p.fax.dir,
					SWITCH_DEFAULT_DIR_PERMS,
					switch_core_session_get_pool(session));
				fax_file_name(session, p.fax.dir, path,
					      sizeof(path));
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
						  SWITCH_LOG_INFO,
						  "dmodem: a fax machine is calling\n");
				status = "FAX";
				run_fax(&l, DMODEM_FAX_RECEIVE, path, &p.fax);
				break;
			}
			if (st.link == DMODEM_ONLINE) {
				online = 1;
				snprintf(rate, sizeof(rate), "%u/%u",
					 st.tx_rate, st.rx_rate);
				switch_channel_set_variable(channel,
							    "dmodem_rate",
							    rate);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
						  SWITCH_LOG_INFO,
						  "dmodem connected at %s bit/s\n",
						  rate);
				if (p.mode == MODE_TCP) {
					host = connect_tcp(&p);
				} else {
					const char *cid =
						switch_channel_get_variable(channel,
							"caller_id_number");
					const char *env[] = {
						"DMODEM_CHANNEL", name,
						"DMODEM_UUID",
						switch_core_session_get_uuid(session),
						"DMODEM_PROFILE", profile,
						"DMODEM_RATE", rate,
						"DMODEM_CALLERID",
						cid ? cid : "",
						NULL
					};
					host = start_exec(&p, env, &pid);
				}
				if (host < 0) {
					status = "ERROR";
					break;
				}
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
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
						  SWITCH_LOG_INFO,
						  "dmodem: the server or program ended the session\n");
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
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
						  SWITCH_LOG_WARNING,
						  "dmodem: %d bytes not sent at the hangup\n",
						  to_line.len + (int)st.tx_pending);
			break;
		}
	}
	if (d && online && st.link == DMODEM_ONLINE)
		dmodem_hangup(d);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			  "dmodem done: %s\n", status);

out:
	if (d)
		dmodem_destroy(d);
	if (line_ok)
		line_close(&l);
	if (host >= 0)
		close(host);
	if (pid > 0)
		end_exec(pid);
	if (dte[0] >= 0)
		close(dte[0]);
	if (dte[1] >= 0)
		close(dte[1]);
	switch_channel_set_variable(channel, "dmodem_status", status);
}

#define FAX_SYNTAX "<file> [ident]"

/* dmodem_send_fax / dmodem_receive_fax <file> [ident] in direction dir */
static void fax_app(switch_core_session_t *session, const char *data,
		    enum dmodem_fax_dir dir)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct fax_settings fs;
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
	struct fax_settings f;
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
