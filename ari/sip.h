/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    sip.h - dmodem-ari's own SIP side, for T.38: Asterisk calls it on a
 *            PJSIP endpoint of its own (one leg for each call, the call's
 *            audio in RTP), and the leg goes over to T.38 (UDPTL) by a
 *            re-INVITE, Asterisk's or ours
 *
 *    A user agent for Asterisk next to it and nothing else: SIP over UDP,
 *    the calls Asterisk makes to it (no calls of its own), and in them
 *    re-INVITEs both ways; RFC 3261's retransmissions over UDP, no
 *    authentication, no routes.
 *
 */
#ifndef DMODEM_ARI_SIP_H
#define DMODEM_ARI_SIP_H

#include <netinet/in.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ari {

/* an SDP's media line */
struct SdpMedia {
	std::string type;               /* audio, image */
	int32_t port = 0;
	std::string proto;              /* RTP/AVP, udptl */
	std::vector<std::string> fmts;
	std::string addr;               /* its c= (or the session's) */
	std::map<std::string, std::string> attrs;      /* a=name:value */
};

/* an SDP: its media lines in their order */
struct Sdp {
	std::vector<SdpMedia> media;
	/* the first line of type with a port, or NULL */
	const SdpMedia *find(const std::string &type) const;
};

bool sdp_parse(const std::string &body, Sdp *s);

/* T.38's settings as SDP says them (T38Fax...) */
struct T38Params {
	int version = 0;
	int32_t max_bitrate = 14400;
	int32_t max_datagram = 0;       /* 0: not said */
	bool redundancy = false;        /* T38FaxUdpEC: t38UDPRedundancy */
	std::string addr;               /* where the UDPTL goes */
	int32_t port = 0;
};

/* where a leg is with T.38 */
enum T38State { T38_NONE, T38_ASKED, T38_ON, T38_REFUSED };

/* one call's leg (all of it under the agent's lock) */
struct SipLeg {
	std::string token;              /* the user Asterisk calls */
	int pt = 0;                     /* our G.711: 0 PCMU, 8 PCMA */
	int32_t rtp_port = 0, udptl_port = 0;   /* ours */

	/* the dialog */
	bool up = false, gone = false;
	std::string call_id, local_tag, remote_tag;
	std::string local_uri, remote_uri;      /* To and From, untagged */
	std::string target;                     /* Asterisk's Contact */
	struct sockaddr_in peer = {};           /* where requests go */
	uint32_t local_cseq = 0, sdp_version = 1;
	/* the last INVITE that came: its CSeq, our response, which goes
	   again until its ACK comes */
	uint32_t rx_cseq = 0;
	std::string response;
	struct sockaddr_in response_to = {};
	bool acked = true;
	int64_t retx_at = 0, retx_ms = 0, retx_end = 0;
	/* our re-INVITE, again until a response */
	bool asking = false;
	std::string request, request_branch;
	uint32_t request_cseq = 0;
	int64_t req_at = 0, req_ms = 0, req_end = 0;

	int64_t forget_at = 0;          /* forgotten: kept for its BYE till
					   then */

	/* the media */
	std::string rtp_addr;           /* the audio's other end */
	int32_t rtp_peer_port = 0;
	bool accept_t38 = false;        /* a T.38 offer is answered */
	T38State t38 = T38_NONE;
	T38Params t38_peer;             /* the other end's, once on */
};

/* the user agent: its socket and thread */
class SipAgent {
public:
	/* bound to host:port (port 0: any), its thread started; false
	   (errno) if the socket can't be */
	bool start(const std::string &host, int32_t port);
	/* the address and port we are bound to */
	const std::string &host() const { return host_; }
	/* (see host()) */
	int32_t port() const { return port_; }

	/* a leg for a call Asterisk is to make to token, its audio in RTP
	   payload type pt to our rtp_port, UDPTL to our udptl_port */
	std::shared_ptr<SipLeg> expect(const std::string &token, int pt,
				       int32_t rtp_port, int32_t udptl_port);
	/* the leg forgotten (its call is over; its BYE still answered) */
	void forget(const std::shared_ptr<SipLeg> &leg);
	/* the leg up (Asterisk has our answer) within ms; false if not */
	bool wait_up(const std::shared_ptr<SipLeg> &leg, int ms);
	/* a copy of the leg's state */
	SipLeg get(const std::shared_ptr<SipLeg> &leg);
	/* T.38 offers answered (on) or refused (488) from now on */
	void accept_t38(const std::shared_ptr<SipLeg> &leg, bool on);
	/* our re-INVITE for T.38 sent (the answer to it by get().t38);
	   false if the leg is not up, or one is on its way */
	bool ask_t38(const std::shared_ptr<SipLeg> &leg);

	/* (the thread's) */
	void run();

private:
	int fd_ = -1;
	std::string host_;
	int32_t port_ = 0;
	std::mutex lock_;
	std::condition_variable changed_;
	std::vector<std::shared_ptr<SipLeg>> legs_;
	std::thread thread_;

	void send_to(const struct sockaddr_in &to, const std::string &msg);
	void received(const std::string &msg, const struct sockaddr_in &from);
	void timers();
};

} /* namespace ari */

#endif
