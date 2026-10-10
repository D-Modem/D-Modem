/* 
 * Copyright (C) 2021 Aon plc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#include <pjsua-lib/pjsua.h>

#define SIGNATURE PJMEDIA_SIG_CLASS_PORT_AUD('D','M')

struct dmodem {
	pjmedia_port base;
	pj_timestamp timestamp;
	pj_sock_t sock;
};

static struct dmodem port;

/*
 * slmodemd runs at 9600 Hz, the call at 8000 Hz.  The sample rate
 * conversion is done here, not by the conference bridge: the resampler of
 * pjmedia cuts below 4000 Hz, which takes away part of a V.90 signal (its
 * PCM codes have energy up to 4000 Hz and the analogue modem recovers its
 * timing from it).  Towards slmodemd, a raised cosine (Nyquist at 4000 Hz)
 * at 48 kHz, like the codec on an analogue line, so that the PCM samples
 * stay free of intersymbol interference; from it, a low pass at 3800 Hz.
 */
#define RS_PHASES 6             /* 48 kHz / 8000 Hz */
#define RS_TAPS 120             /* per phase: 15 ms */
#define DN_TAPS 144             /* per phase of the down converter (5 at
				   48 kHz / 9600 Hz): 15 ms */
struct rs_up {
	float h[RS_PHASES][RS_TAPS];
	float hist[2 * RS_TAPS];
	int pos, t;
};
struct rs_down {
	float h[5][DN_TAPS];
	float hist[2 * DN_TAPS];
	int pos, t;
};
static struct rs_up up;
static struct rs_down down;

static double hann(int n, int len)
{
	return 0.5 - 0.5 * cos(2 * M_PI * (n + 0.5) / len);
}

static void rs_init(void)
{
	const double a = 0.15;
	int n, len = RS_PHASES * RS_TAPS;
	for (n = 0; n < len; n++) {
		double x = (n - len / 2) * 8000.0 / 48000, h;
		double d = 1 - (2 * a * x) * (2 * a * x);
		h = x == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
		h *= fabs(d) < 1e-9 ? M_PI / 4 : cos(M_PI * a * x) / d;
		up.h[n % RS_PHASES][n / RS_PHASES] = (float)(h * hann(n, len));
	}
	len = 5 * DN_TAPS;
	for (n = 0; n < len; n++) {
		double x = (n - len / 2) * 2 * 3800.0 / 48000, h;
		h = x == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
		/* the gain of 3800 / 4800 of one input sample at 9600 Hz */
		down.h[n % 5][n / 5] = (float)(h * hann(n, len) * 3800 / 4800);
	}
}

/* 160 samples at 8000 Hz in, 192 at 9600 Hz out */
static void rs_run_up(const short *in, short *out)
{
	int i, o = 0, k;
	for (i = 0; i < 160; i++) {
		if (--up.pos < 0)
			up.pos = RS_TAPS - 1;
		up.hist[up.pos] = up.hist[up.pos + RS_TAPS] = in[i];
		while (up.t < RS_PHASES) {
			const float *h = up.h[up.t], *b = up.hist + up.pos;
			float y = 0;
			for (k = 0; k < RS_TAPS; k++)
				y += h[k] * b[k];
			out[o++] = (short)(y > 32767 ? 32767 :
					   y < -32768 ? -32768 : lrintf(y));
			up.t += 5;
		}
		up.t -= RS_PHASES;
	}
}

/* 192 samples at 9600 Hz in, 160 at 8000 Hz out */
static void rs_run_down(const short *in, short *out)
{
	int i, o = 0, k;
	for (i = 0; i < 192; i++) {
		if (--down.pos < 0)
			down.pos = DN_TAPS - 1;
		down.hist[down.pos] = down.hist[down.pos + DN_TAPS] = in[i];
		while (down.t < 5) {
			const float *h = down.h[down.t], *b = down.hist + down.pos;
			float y = 0;
			for (k = 0; k < DN_TAPS; k++)
				y += h[k] * b[k];
			out[o++] = (short)(y > 32767 ? 32767 :
					   y < -32768 ? -32768 : lrintf(y));
			down.t += RS_PHASES;
		}
		down.t -= 5;
	}
}
static bool destroying = false;
/* DMODEM_REC=<prefix>: the audio to and from slmodemd, 16 bit at 9600 Hz,
   in <prefix>.rx (from the call) and <prefix>.tx (to the call) */
static FILE *rec_rx, *rec_tx;
static pj_pool_t *pool;

static void error_exit(const char *title, pj_status_t status) {
	pjsua_perror(__FILE__, title, status);
	if (!destroying) {
		destroying = true;
		pjsua_destroy();
		exit(1);
	}
}

static pj_status_t dmodem_put_frame(pjmedia_port *this_port, pjmedia_frame *frame) {
	struct dmodem *sm = (struct dmodem *)this_port;
	short buf[192];

	if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO) {
		rs_run_up(frame->buf, buf);
		if (rec_rx)
			fwrite(buf, 1, sizeof(buf), rec_rx);
		if (write(sm->sock, buf, sizeof(buf)) != sizeof(buf)) {
			error_exit("error writing frame",0);
		}
	}

	return PJ_SUCCESS;
}

static pj_status_t dmodem_get_frame(pjmedia_port *this_port, pjmedia_frame *frame) {
	struct dmodem *sm = (struct dmodem *)this_port;
	short buf[192];

	if (read(sm->sock, buf, sizeof(buf)) != sizeof(buf)) {
		error_exit("error reading frame",0);
	}
	if (rec_tx)
		fwrite(buf, 1, sizeof(buf), rec_tx);
	rs_run_down(buf, frame->buf);
	frame->size = PJMEDIA_PIA_AVG_FSZ(&this_port->info);

	frame->timestamp.u64 = sm->timestamp.u64;
	frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
	sm->timestamp.u64 += PJMEDIA_PIA_SPF(&this_port->info);

	return PJ_SUCCESS;
}

static pj_status_t dmodem_on_destroy(pjmedia_port *this_port) {
	printf("destroy\n");
	exit(-1);
}

/* Callback called by the library when call's state has changed */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
	pjsua_call_info ci;

	PJ_UNUSED_ARG(e);

	pjsua_call_get_info(call_id, &ci);
	PJ_LOG(3,(__FILE__, "Call %d state=%.*s", call_id,
				(int)ci.state_text.slen,
				ci.state_text.ptr));

	if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
		close(port.sock);
		if (!destroying) {
			destroying = true;
			pjsua_destroy();
			exit(0);
		}
	}
}

/* Callback called by the library when call's media state has changed */
static void on_call_media_state(pjsua_call_id call_id) {
	pjsua_call_info ci;
	pjsua_conf_port_id port_id;
	static int done=0;

	pjsua_call_get_info(call_id, &ci);

//	printf("media_status %d media_cnt %d ci.conf_slot %d aud.conf_slot %d\n",ci.media_status,ci.media_cnt,ci.conf_slot,ci.media[0].stream.aud.conf_slot);
	if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
		if (!done) {
			pjsua_conf_add_port(pool, &port.base, &port_id);
			pjsua_conf_connect(ci.conf_slot, port_id);
			pjsua_conf_connect(port_id, ci.conf_slot);
			done = 1;
		}
	} else {
		done = 0;
	}
}


int main(int argc, char *argv[]) {
	pjsua_acc_id acc_id;
	pj_status_t status;

	if (argc != 3) {
		return -1;
	}

	signal(SIGPIPE,SIG_IGN);

	if (getenv("DMODEM_REC")) {
		char name[256];
		snprintf(name, sizeof(name), "%s.rx", getenv("DMODEM_REC"));
		rec_rx = fopen(name, "wb");
		snprintf(name, sizeof(name), "%s.tx", getenv("DMODEM_REC"));
		rec_tx = fopen(name, "wb");
	}

	char *dialstr = argv[1];

	char *sip_user = getenv("SIP_LOGIN");
	if (!sip_user) {
		return -1;
	}
	char *sip_domain = strchr(sip_user,'@');
	if (!sip_domain) {
		return -1;
	}
	*sip_domain++ = '\0';
	char *sip_pass = strchr(sip_user,':');
	if (!sip_pass) {
		return -1;
	}
	*sip_pass++ = '\0';

	status = pjsua_create();
	if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);

	/* Init pjsua */
	{
		pjsua_config cfg;
		pjsua_logging_config log_cfg;
		pjsua_media_config med_cfg;

		pjsua_config_default(&cfg);
		cfg.cb.on_call_media_state = &on_call_media_state;
		cfg.cb.on_call_state = &on_call_state;

		pjsua_logging_config_default(&log_cfg);
		log_cfg.console_level = 4;

		pjsua_media_config_default(&med_cfg);
		med_cfg.no_vad = true;
		med_cfg.ec_tail_len = 0;
		/* a modem can't take samples slipping: a fixed 60 ms in the
		   jitter buffer, never adapted down by discarding audio.  The
		   far end often sends in step with what it receives, so its
		   packets come right when the bridge takes a frame, and a
		   buffer of one frame would run dry and fill in (a slip of a
		   frame) on any jitter */
		med_cfg.jb_max = 2000;
		med_cfg.jb_init = 60;
		med_cfg.jb_min_pre = 60;
		med_cfg.jb_max_pre = 60;
		med_cfg.jb_discard_algo = PJMEDIA_JB_DISCARD_NONE;
		/* the conference bridge in frames of the modem's port (20
		   ms): with smaller ones, where the port's frame falls among
		   them changes from call to call, and so does the delay the
		   modem sees from its transmitter to its receiver (V.34
		   phase 2 depends on it, see MDMCTL_IODELAY in slmodemd) */
		med_cfg.audio_frame_ptime = 20;
		/* the bridge at the rate of the call: no resampling (see
		   rs_init) */
		med_cfg.clock_rate = 8000;
		if (getenv("DMODEM_CLOCK"))
			med_cfg.clock_rate = atoi(getenv("DMODEM_CLOCK"));
		if (getenv("DMODEM_PTIME"))
			med_cfg.audio_frame_ptime = atoi(getenv("DMODEM_PTIME"));

		status = pjsua_init(&cfg, &log_cfg, &med_cfg);
		if (status != PJ_SUCCESS) error_exit("Error in pjsua_init()", status);
	}

	pjsua_set_ec(0,0); // maybe?
	pjsua_set_null_snd_dev();
	
	/* g711 only */
	pjsua_codec_info codecs[32];
	unsigned count = sizeof(codecs)/sizeof(*codecs);
	pjsua_enum_codecs(codecs,&count);
	for (int i=0; i<count; i++) {
		int pri = 0;
		if (pj_strcmp2(&codecs[i].codec_id,"PCMU/8000/1") == 0) {
			pri = 1;
		} else if (pj_strcmp2(&codecs[i].codec_id,"PCMA/8000/1") == 0) {
			pri = 2;
		}
		pjsua_codec_set_priority(&codecs[i].codec_id, pri);
//		printf("codec: %s %d\n",pj_strbuf(&codecs[i].codec_id),pri);
	}

	/* Add UDP transport. */
	{
		pjsua_transport_config cfg;

		pjsua_transport_config_default(&cfg);
		cfg.port = 5060;
		status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
		if (status != PJ_SUCCESS) error_exit("Error creating transport", status);
	}

	pj_caching_pool cp;
	pj_caching_pool_init(&cp, NULL, 1024*1024);
	pool = pj_pool_create(&cp.factory, "pool1", 4000, 4000, NULL);

	pj_str_t name = pj_str("dmodem");
	
	memset(&port,0,sizeof(port));
	port.sock = atoi(argv[2]); // inherited from parent
	rs_init();
	pjmedia_port_info_init(&port.base.info, &name, SIGNATURE, 8000, 1, 16, 160);
	port.base.put_frame = dmodem_put_frame;
	port.base.get_frame = dmodem_get_frame;
	port.base.on_destroy = dmodem_on_destroy;

	char buf[384];
	memset(buf,0,sizeof(buf));
	write(port.sock, buf, sizeof(buf));

	/* Initialization is done, now start pjsua */
	status = pjsua_start();
	if (status != PJ_SUCCESS) error_exit("Error starting pjsua", status);

	{
		pjsua_acc_config cfg;
		pjsua_acc_config_default(&cfg);
		snprintf(buf,sizeof(buf),"sip:%s@%s",sip_user,sip_domain);
		pj_strdup2(pool,&cfg.id,buf);
		snprintf(buf,sizeof(buf),"sip:%s",sip_domain);
		pj_strdup2(pool,&cfg.reg_uri,buf);
		cfg.register_on_acc_add = false;
		cfg.cred_count = 1;
		cfg.cred_info[0].realm = pj_str("*");
		cfg.cred_info[0].scheme = pj_str("digest");
		cfg.cred_info[0].username = pj_str(sip_user);
		cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
		cfg.cred_info[0].data = pj_str(sip_pass);

		status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
		if (status != PJ_SUCCESS) error_exit("Error adding account", status);
	}

	snprintf(buf,sizeof(buf),"sip:%s@%s",dialstr,sip_domain);
	printf("calling %s\n",buf);
	pj_str_t uri = pj_str(buf);
	
	pjsua_call_id callid;
	status = pjsua_call_make_call(acc_id, &uri, 0, NULL, NULL, &callid);
	if (status != PJ_SUCCESS) error_exit("Error making call", status);

	struct timespec ts = {100, 0};
	while(1) {
		nanosleep(&ts,NULL);
	}

	return 0;
}
