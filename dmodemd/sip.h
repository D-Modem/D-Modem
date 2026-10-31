/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sip.h - dmodemd's telephone line: a SIP user agent (on libre) in the
 *            same process, in place of the external d-modem program
 *
 *    The modem gets each call's audio through one end of a socketpair,
 *    16 bit linear at 8000 Hz in frames of 20 ms, paced by the SIP side
 *    (it reads a frame from the call, the modem answers with one).  A
 *    call ends with that socket: whichever side closes it.
 *
 *    A fax call may go over to T.38 (SIP_T38, on by default) while the
 *    modem is in fax mode (set_fax()): when the far end asks, or a
 *    second after we answered one; and when the far end asks while the
 *    modem answers adaptively (set_t38_offers(): +FAA=1).  Its audio stops then (the socket
 *    stays, for the call's end), and the modem's fax engine has the
 *    call's UDPTL instead (t38()); a "T38" line on the control
 *    descriptor says so, an "AUDIO" line that the call is back on the
 *    audio.
 *
 */

#ifndef __SIP_H__
#define __SIP_H__

#include <string>

#include "dm_types.h"
#include "portable.h"

namespace sip {

struct Config {
	const char *user;       /* account (SIP_LOGIN=user:password@domain) */
	const char *password;
	const char *domain;     /* host[:port] of the registrar / proxy */
	s32 port;               /* local UDP port (5060) */
	bool listen;            /* register, and take calls in */
	const char *record;     /* $DMODEM_REC: the audio both ways in
				   <record>.rx and <record>.tx */
	bool t38;               /* $SIP_T38 (not "no"): fax over T.38 */
	bool clearmode = true;  /* $SIP_CLEARMODE (not "no"): RFC 4040's
				   64 kbit/s clear channel, an ISDN data
				   call's, taken when an offer has it
				   first, and offered first for the
				   modem's AT+MS=CLEAR */
	s16 srtp = 1;           /* $SIP_SRTP: SRTP (RFC 3711, its keys in
				   SDP's a=crypto, RFC 4568): 0 ("no")
				   never, an RTP/SAVP line declined; 1
				   (the default) an RTP/SAVP offer
				   answered in SRTP, ours plain RTP; 2
				   ("require") ours RTP/SAVP, and plain
				   RTP declined */
	bool v152 = true;       /* $SIP_V152 (not "no"): V.152's voice
				   band data, our G.711 formats marked
				   a=gpmd:<pt> vbd=yes, and a VBD
				   format the far end has taken first */
	bool v150 = false;      /* $SIP_V150 ("yes"): V.150.1's modem
				   relay, a data call's octets over SPRT
				   when the far end (a gateway) has it:
				   offered with our audio, and taken
				   when an offer has it (v150.h) */
	const char *dns = nullptr;      /* the DNS servers for SRV and
					   NAPTR, addresses (with :port, 53
					   if none) apart by commas or
					   spaces; NULL: /etc/resolv.conf's
					   (a system without one, Android,
					   gives them here) */
	s16 log_level = -1;             /* 1 errors .. 4 the SIP messages,
					   0 none; -1: $SIP_LOG_LEVEL, or
					   4.  The lines go to the modem's
					   log hook if it has one
					   (dcore::log_set_hook()), else to
					   stderr */
};

/* $SIP_LOGIN and friends into cfg; false (with a message) if unusable */
bool config_from_env(Config &cfg, bool listen);

/* start the user agent; returns the descriptor its RING and CANCEL lines
   come on (a call waits for ANSWER, or the caller gave up; ahead of the
   first RING, CALLER with the date, number and name: sip_modem.cpp's
   caller()), T38 and
   AUDIO (the call's fax went over to T.38, or back), and for a call we
   dial ANSWERED (it is up) or FAILED <SIP status> (it never came up; 0
   for no response), ahead of the end of its audio; or -1.  A call we
   dial has its audio from a provisional response with SDP on (183
   Session Progress's early media: the ringing tone, a busy tone, an
   announcement). */
int start(const Config &cfg);
/* the SIP thread ended (the call hung up, the registration given up) */
void stop();

/* hand a call its audio socket (taken over in any case): dial a number,
   or answer the call that rings.  If there is none, or the line is busy,
   the socket is closed at once (NO CARRIER). */
void dial(const char *number, int audio_fd);
/* the user part of the URI that dial string dial calls: a telephone
   number as V.250 has it (ATDT555-1000, ATD9,W5551000) its digits and
   DTMF characters only (5551000, 95551000); anything else is a name
   (ATDalice), taken as it is (sip_dial.cpp) */
std::string dial_user(const char *dial);
void answer(int audio_fd);

/* the caller of a call that rings, as caller ID gives it: a number and
   a name, each withheld or not (empty when not known) */
struct Caller {
	std::string number, name;
	bool number_withheld = false, name_withheld = false;
};
/* the caller from its address (P-Asserted-Identity's if there is one,
   else From's): uri its URI (sip:, sips: or tel:), dname its display
   name, privacy the Privacy header ("" if none: "id", "user" or
   "header" withhold both, as does an anonymous URI or display name)
   (sip_dial.cpp) */
Caller caller_of(const std::string &uri, const std::string &dname,
		 const std::string &privacy);

/* the format of the current call: 1 mu-law, 2 A-law, 3 CLEARMODE (its
   octets as A-law samples, which give every octet back), 0 not known
   yet */
s16 law();
/* the current call is a clear 64 kbit/s channel (its offer asked for
   CLEARMODE first, or it is in CLEARMODE): the modem runs its clear
   channel pump on it */
bool clear();
/* the modem's next calls are for a clear channel (AT+MS=CLEAR): our
   offers have CLEARMODE first, then PCMA and PCMU */
void set_clear(bool want);

/* the modem is in fax mode: its calls may go over to T.38 */
void set_fax(bool fax);
/* the modem answers adaptively (+FAA=1), the call not known to be data
   or fax yet: a T.38 offer of the far end's is taken (it makes the call
   a fax call), though we ask for none */
void set_t38_offers(bool take);

/* telephone-events (RFC 4733): the current call has them (its far end
   offered or took ours); a hook flash of ms on it, as event 16, sent
   in place of the audio while it lasts.  The far end's DTMF digits and
   RFC 4734's tones sent as events reach the modem as their tones, as on
   an analogue line (rtp_events.h). */
bool can_flash();
void flash(u32 ms);

/* V.150.1's modem relay: the current call has it (both ends' SDP), and
   the modem is not in fax mode: the modem runs its relay pump on it
   (DP_RELAY), and its data goes as octets over SPRT, the audio off
   while it does.  relay_state() starts the session (the caller's
   SSE:M, the answerer's answer tone) and gives its state as struct
   dp_relay's: 0 not up yet, 1 up (its rates each way into *tx, *rx),
   -1 over or none; relay_read() the far end's octets (up to max: how
   many), relay_write() ours (how many it took) */
bool relay();
s16 relay_state(s32 *tx, s32 *rx);
s32 relay_read(u8 *buf, s32 max);
s32 relay_write(const u8 *buf, s32 n);

/* the current call's T.38, as the far end has it */
struct T38 {
	int fd;                         /* our UDPTL socket: the caller's
					   own descriptor of it */
	struct sockaddr_storage peer;   /* the far end's UDPTL */
	socklen_t peer_len;
	s32 version;                    /* T38FaxVersion agreed */
	s32 max_datagram;               /* its T38FaxMaxDatagram, 0 if
					   none */
	bool redundancy;                /* T38FaxUdpEC t38UDPRedundancy */
};
/* the current call's T.38 into t, while it is on; false if it is not */
bool t38(T38 *t);

} // namespace sip

#endif /* __SIP_H__ */
