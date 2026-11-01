/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    misdn_line.h - the modem's line an ISDN basic rate interface
 *                   through Linux's mISDN (an HFC-S PCI or USB card, an
 *                   AVM Fritz!Card, ...): the S0 bus's terminal side,
 *                   one call at a time
 *
 *    The D-channel's layer 2 (LAPD, the TEI) is the kernel's, through
 *    an ISDN_P_LAPD_TE socket; its messages are Q.931's (q931.h), here.
 *    Each call's B-channel is an ISDN_P_B_RAW socket: its octets, each
 *    bit reversed (the cards' transparent mode sends an octet's least
 *    significant bit first, G.711 and the clear channel its most
 *    significant), are A-law samples through the modem, its answer
 *    out as many as came, so the B-channel's clock paces the modem.
 *
 *    A call we dial has the bearer unrestricted digital information
 *    with AT+MS=CLEAR (the clear channel pump, DP_CLEAR: X.75, V.110,
 *    V.120 and the rest of docs/isdn.md), else 3.1 kHz audio (the
 *    modem's other modes, as on a digital line).  A call that comes
 *    for our MSN (any, without one) rings as SIP's do, every 5 s, its
 *    caller's number as caller ID; one with the bearer unrestricted
 *    digital information is answered on the clear channel.
 *
 */
#ifndef DMODEMD_MISDN_LINE_H
#define DMODEMD_MISDN_LINE_H

#include <string>
#include <vector>

#include <dcore.h>

#include "dm_types.h"
#include "pty.h"
#include "q931.h"
#include "run_loop.h"

namespace dmd {

class MisdnLine final : public dcore::Host, private Q931::User {
public:
	/* the line of pty's modem, not yet open */
	explicit MisdnLine(Pty &pty) : pty_(pty), q931_(*this) {}
	~MisdnLine();
	MisdnLine(const MisdnLine &) = delete;
	MisdnLine &operator=(const MisdnLine &) = delete;

	/* mISDN's device dev (its number, or its name: "hfc-pci.1"),
	   calls for msn alone (nullptr or "": any), the B-channel's octets
	   bit reversed (flip: the cards' transparent mode) or not: the
	   D-channel's layer 2 open; false with the reason on stderr.  As
	   root (the device list's socket needs CAP_NET_RAW) */
	bool open(const char *dev, const char *msn, bool flip);
	/* the D-channel's and the call's B-channel's sockets watched this
	   turn */
	void watch(RunLoop &loop);
	/* what came on them: D-channel messages to Q.931, the B-channel's
	   octets through the modem (its answer out); the timers due.  0,
	   or -1 if the D-channel has failed for good */
	s16 pump(dcore::Modem &m);

	/* Host: start() dials (SETUP) or answers (CONNECT) */
	s16 start(dcore::Modem &m) override;
	/* hangs up (DISCONNECT), the B-channel closed */
	s16 stop(dcore::Modem &m) override;
	/* Q.931 says when a call we dial is answered, and why it failed */
	bool signals_answer(dcore::Modem &) override { return true; }
	/* a call with the bearer unrestricted digital information */
	bool clear_line(dcore::Modem &) override { return udi_; }
	/* for ATI3 */
	const char *driver_name() override { return "mISDN"; }
	/* the modem's characters to the pty: how many it took */
	s32 dte_write(dcore::Modem &, const char *buf, s32 n) override {
		return pty_.write(buf, n);
	}
	/* the clock: the monotonic one's ms */
	u64 now_ms() override { return dcore::monotonic_ms(); }

	/* Q.931's cause value as the modem's result for a call it
	   dialled: user busy BUSY, no answer NO ANSWER, the rest NO
	   CARRIER */
	static dcore::Result result_of(s16 cause);

private:
	/* Q931::User */
	void q931_send(const u8 *msg, s16 n) override;
	void q931_channel(s16 b) override;
	void q931_offered(const Q931::Call &c) override;
	void q931_connected() override;
	void q931_ended(s16 cause) override;

	/* a message of prim on fd with n octets of data; false (logged)
	   if it could not go */
	bool send(int fd, u32 prim, const u8 *data, s32 n);
	/* the D-channel's messages that came; false if it has failed */
	bool d_receive();
	/* the B-channel's */
	void b_receive();
	/* the B-channel's octets through the modem, its answer out */
	void b_data(const u8 *data, s32 n);
	/* the call's B-channel closed */
	void b_close();

	Pty &pty_;
	Q931 q931_;
	dcore::Modem *m_ = nullptr;     /* pump()'s */
	u8 dev_ = 0;
	bool flip_ = true;
	int d_ = -1;
	/* layer 2 is up (or asked to come up); Q.931's messages waiting
	   for it */
	bool l2_up_ = false, l2_asked_ = false;
	std::vector<std::vector<u8>> queue_;
	int b_ = -1;
	s16 channel_ = 0;
	bool b_active_ = false;
	/* the call: its bearer unrestricted digital information; ringing
	   (the next ring due then); answered */
	bool udi_ = false;
	bool ringing_ = false;
	u64 next_ring_ = 0;
	bool connected_ = false;
};

} // namespace dmd

#endif
