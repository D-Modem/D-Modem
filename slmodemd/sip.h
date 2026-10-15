/*
 *
 *    sip.h - slmodemd's telephone line: a SIP user agent (pjsua) in the
 *            same process, in place of the external d-modem program
 *
 *    The modem gets each call's audio through one end of a socketpair,
 *    16 bit linear at 8000 Hz in frames of 20 ms, paced by the SIP side
 *    (it reads a frame from the call, the modem answers with one).  A
 *    call ends with that socket: whichever side closes it.
 *
 */

#ifndef __SIP_H__
#define __SIP_H__

#include <string>

namespace sip {

struct Config {
	const char *user;       /* account (SIP_LOGIN=user:password@domain) */
	const char *password;
	const char *domain;     /* host[:port] of the registrar / proxy */
	int port;               /* local UDP port (5060) */
	bool listen;            /* register, and take calls in */
	const char *record;     /* $DMODEM_REC: the audio both ways in
				   <record>.rx and <record>.tx */
};

/* $SIP_LOGIN and friends into cfg; false (with a message) if unusable */
bool config_from_env(Config &cfg, bool listen);

/* start the user agent; returns the descriptor its RING and CANCEL lines
   come on (a call waits for ANSWER, or the caller gave up), or -1 */
int start(const Config &cfg);
/* the service thread ended (the call hung up), pjsua destroyed */
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

} // namespace sip

#endif /* __SIP_H__ */
