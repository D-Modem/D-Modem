/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
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
 *    second after we answered one.  Its audio stops then (the socket
 *    stays, for the call's end), and the modem's fax engine has the
 *    call's UDPTL instead (t38()); a "T38" line on the control
 *    descriptor says so, an "AUDIO" line that the call is back on the
 *    audio.
 *
 */

#ifndef __SIP_H__
#define __SIP_H__

#include <string>

#include <sys/socket.h>

namespace sip {

struct Config {
	const char *user;       /* account (SIP_LOGIN=user:password@domain) */
	const char *password;
	const char *domain;     /* host[:port] of the registrar / proxy */
	int port;               /* local UDP port (5060) */
	bool listen;            /* register, and take calls in */
	const char *record;     /* $DMODEM_REC: the audio both ways in
				   <record>.rx and <record>.tx */
	bool t38;               /* $SIP_T38 (not "no"): fax over T.38 */
	const char *dns = nullptr;      /* the DNS servers for SRV and
					   NAPTR, addresses (with :port, 53
					   if none) apart by commas or
					   spaces; NULL: /etc/resolv.conf's
					   (a system without one, Android,
					   gives them here) */
	int log_level = -1;             /* 1 errors .. 4 the SIP messages,
					   0 none; -1: $SIP_LOG_LEVEL, or
					   4.  The lines go to the modem's
					   log hook if it has one
					   (dcore::log_set_hook()), else to
					   stderr */
};

/* $SIP_LOGIN and friends into cfg; false (with a message) if unusable */
bool config_from_env(Config &cfg, bool listen);

/* start the user agent; returns the descriptor its RING and CANCEL lines
   come on (a call waits for ANSWER, or the caller gave up), T38 and
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

/* the G.711 law of the current call: 1 mu-law, 2 A-law, 0 not known yet */
int law();

/* the modem is in fax mode: its calls may go over to T.38 */
void set_fax(bool fax);

/* telephone-events (RFC 4733): the current call has them (its far end
   offered or took ours); a hook flash of ms on it, as event 16, sent
   in place of the audio while it lasts.  The far end's DTMF digits and
   RFC 4734's tones sent as events reach the modem as their tones, as on
   an analogue line (rtp_events.h). */
bool can_flash();
void flash(unsigned ms);

/* the current call's T.38, as the far end has it */
struct T38 {
	int fd;                         /* our UDPTL socket: the caller's
					   own descriptor of it */
	struct sockaddr_storage peer;   /* the far end's UDPTL */
	socklen_t peer_len;
	int version;                    /* T38FaxVersion agreed */
	int max_datagram;               /* its T38FaxMaxDatagram, 0 if
					   none */
	bool redundancy;                /* T38FaxUdpEC t38UDPRedundancy */
};
/* the current call's T.38 into t, while it is on; false if it is not */
bool t38(T38 *t);

} // namespace sip

#endif /* __SIP_H__ */
