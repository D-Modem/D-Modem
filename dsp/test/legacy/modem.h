/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 *    modem.h - modem definitions
 *
 *    Author: sashak@smlink.com
 */

#ifndef __MODEM_H__
#define __MODEM_H__

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <termios.h>

#include <modem_defs.h>
#include <modem_homolog.h>
#include <modem_dp.h>
#include <modem_timer.h>

#ifdef __cplusplus
extern "C" {
#endif


/* modem modes */


/* modem status */
#define STATUS_OK         0x0
#define STATUS_NOCARRIER  0x0002 // 3
#define STATUS_ERROR      0x0004 // 4
#define STATUS_NODIALTONE 0x0010 // 6
#define STATUS_BUSY       0x0020 // 7
#define STATUS_NOANSWER   0x0040 // 8
#define STATUS_DP_LINK    0x0100 // 13
#define STATUS_DP_ERROR   0x0200 // 14
#define STATUS_PACK_LINK  0x0400 // 15
#define STATUS_PACK_ERROR 0x0800 // 16
#define STATUS_EC_LINK    0x1000 // 17
#define STATUS_EC_RELEASE 0x2000 // 18
#define STATUS_EC_ERROR   0x4000 // 19


/*
 *    parameters
 *
 */

#define MODEM_FORMAT MFMT_S16_LE
#ifndef MODEM_RATE
#define MODEM_RATE   9600 /* 8000 */
#endif
#define MODEM_FRAG   (MODEM_RATE/200)


#define MODEM_MIN_RATE 300
#define MODEM_MAX_RATE 56000

#define MODEM_DEFAULT_COUNTRY_CODE 0xb5 /* USA */

#define MODEM_CONFIG_CID   1
#define MODEM_CONFIG_VOICE 1
#define MODEM_CONFIG_FAX   1
#define MODEM_CONFIG_FAX_CLASS1 1

#define MODEM_CONFIG_RING_DETECTOR 1

/*
 *    type definitions
 *
 */


/* modem driver struct */
struct modem_driver {
	const char *name;
        int (*start)(struct modem *m);
        int (*stop )(struct modem *m);
	int (*ioctl)(struct modem *m,unsigned cmd, unsigned long arg);
};

/* modem data pumps driver struct */
struct dp_driver {
	const enum DP_ID id;
	const char *name;
	const char *code;
	struct dp_operations *op;
};


/* the data link layer (link/): V.42 detection, LAPM, V.42bis, async */
namespace dlink { class Link; }
struct core_link;

/* the parts of struct modem (C++ would nest them in it) */
struct xmit_queue {
	int count;
	int head;
	int tail;
	int size;
	unsigned char *buf;
};

struct modem_config {
	/* ec params */
	u8  ec;
	u8  ec_detector;
	u8  ec_tx_win_size;
	u8  ec_rx_win_size;
	u16 ec_tx_info_size;
	u16 ec_rx_info_size;
	/* compressor params */
	u8  comp;
	u8  comp_v44;           /* V.44 rather than V.42bis */
	int comp_dict_size;
	int comp_max_string;
};

/* modem struct */
struct modem {
	const char *name;
	int pty; /* pty descriptor */
	const char *dev_name, *pty_name; /* and names */
	struct termios termios;
	/* device driver */
	struct modem_driver driver;
	void *dev_data;
        /* modem states */
        unsigned modem_info;
        unsigned state;
	unsigned started;
	unsigned command;
	unsigned result_code;
        unsigned hook;
        unsigned caller;
	unsigned rx_rate;
	unsigned tx_rate;
	enum MODEM_MODE mode; /* data,fax,voice */
        /* modem events (ring,escape,waiting for connect,etc) */
	unsigned event;
	unsigned new_event;
        struct timer event_timer;
	/* modem parameters */
	unsigned min_rate;
	unsigned max_rate;
        unsigned char sregs[256];   /* s-registers */
        const struct homolog_set *homolog; /* homologation parameters */
        /* at processor */
        unsigned int at_count;
        char at_line[64];
        char at_cmd[64];
	/* ring detector */
	unsigned int ring_count;
	unsigned long ring_first;
	unsigned long ring_last;
#ifdef MODEM_CONFIG_RING_DETECTOR
	void *rd_obj;
#endif
	/* cid */
#ifdef MODEM_CONFIG_CID
	unsigned cid_requested;
	void *cid;
#endif
        /* dialer */
        char dial_string[128];
        /* escape counter */
        unsigned escape_count;
	unsigned long last_esc_check;
	/* xmit queue */
	struct xmit_queue xmit;
	/* run time configuration */
	struct modem_config cfg;
	/* the data link layer and the core's side of it */
	dlink::Link *link;
	struct core_link *link_owner;
        /* modem data pump interface */
        unsigned srate;
        unsigned format;
        unsigned frag;
        unsigned dp_requested;
        unsigned automode_requested;
	int update_delay;
	void (*process)(struct modem *m,void *in,void *out,int cnt);
        /* process counter and sample timer */
        unsigned long count;
        unsigned long sample_timer;
	void (*sample_timer_func)(struct modem *);
        /* data pump */
        struct dp_operations **dp_ops;
        struct dp *dp;
	void *dp_runtime;
	/* dc evaluator and cleaner */
	void *dcr;
	/* dsp info data */
	struct dsp_info dsp_info;
	/* voice info */
#ifdef MODEM_CONFIG_VOICE
	void *voice_obj;
	struct voice_info voice_info;
#endif
#ifdef MODEM_CONFIG_FAX
	void *fax_obj;
#endif
};



/*
 *    prototypes
 *
 */

/* device driver --> modem */
extern struct modem *modem_create(struct modem_driver *drv, const char *dev_name);
extern void modem_delete(struct modem *m);
extern int modem_write(struct modem *m, const char *buf, int n);
extern int modem_link_state(struct modem *m);
extern void modem_hangup(struct modem *m);
extern void modem_update_termios(struct modem *m, struct termios *tios);
extern void modem_error  (struct modem *m);
extern void modem_ring   (struct modem *m);
extern void modem_ring_signal(struct modem *m, int on);
extern void modem_event  (struct modem *m);
extern void modem_process(struct modem *m,void *in,void *out,int cnt);

/* status & config */
extern void modem_update_status(struct modem *m, unsigned status);

/* modem parameters access macros */
#define CRLF_CHARS(m)     ((char *)((m)->sregs+SREG_CR_CHAR))
#define MODEM_DP(m)       ((m)->sregs[SREG_DP])
#define MODEM_AUTOMODE(m) ((m)->sregs[SREG_AUTOMODE])

#ifdef __cplusplus
}
#endif

#endif /* __MODEM_H__ */

