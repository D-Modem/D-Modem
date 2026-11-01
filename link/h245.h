/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    h245.h - the few H.245 messages V.75 (DSVD control) carries in
 *             V.76's frames, in H.245's aligned PER encoding
 *
 *    V.75 opens and closes a V.76 channel with H.245's
 *    OpenLogicalChannel and CloseLogicalChannel in the user data of the
 *    SABME and DISC that set it up and take it down (8.1, 8.2/V.75), the
 *    answers to them in the UA or DM; a terminal's capabilities go in an
 *    XID (8.3).  The user data is an FI octet of 133 then the message, a
 *    MultimediaSystemControlMessage (H.245's ASN.1 module, ITU-T X.691's
 *    aligned PER).  Only what V.75 lists is read and written: the
 *    channel's number, its port, its data type (G.729 Annex A audio, or
 *    data with a DataProtocolCapability), its V.76 parameters both ways;
 *    the rest of a message is passed over.
 *
 */

#ifndef DMODEM_LINK_H245_H
#define DMODEM_LINK_H245_H


#include <vector>

#include "dm_types.h"

namespace dlink {
namespace h245 {

/* V.75's FI field: what comes first in the user data */
static const u8 FI_V75 = 133;

/* a direction's V.76 parameters (V76LogicalChannelParameters): ERM
   (with REJ recovery and its window) or UNERM, N401, the FCS's length
   in bits, whether audio frames have V.75's audio header */
struct V76Params {
	bool erm = true;
	u8 window = 15;
	u16 n401 = 128;
	u8 crc = 16;
	bool audio_header = false;
};

/* what a channel carries (DataType) */
enum class Kind : u8 {
	Data,           /* data: a DataApplicationCapability */
	Audio,          /* audioData: G.729 Annex A */
	Control,        /* data: dsvdControl, the out-of-band channel */
	Other,          /* anything else */
};

/* the DataProtocolCapability of a data channel (its index in H.245) */
enum class Proto : u8 {
	NonStandard = 0, V14Buffered = 1, V42Lapm = 2, Hdlc = 3,
	Transparent = 6, Other = 0xff,
};

/* a channel as OpenLogicalChannel describes it, both ways */
struct Channel {
	u16 lcn = 1;            /* forwardLogicalChannelNumber */
	s32 port = -1;          /* portNumber, -1 absent */
	Kind kind = Kind::Data;
	Proto proto = Proto::V14Buffered;
	u32 max_rate = 0;       /* data: maxBitRate, in 100 bit/s */
	u16 frames = 1;         /* audio: G.729 Annex A frames a packet */
	V76Params fwd, rev;     /* opener to remote, remote to opener */
	bool has_rev = true;    /* reverseLogicalChannelParameters */
};

/* the messages known */
enum class Type : u8 {
	None,
	Olc,            /* request openLogicalChannel */
	OlcAck,         /* response openLogicalChannelAck */
	OlcReject,      /* response openLogicalChannelReject */
	Clc,            /* request closeLogicalChannel */
	ClcAck,         /* response closeLogicalChannelAck */
	Tcs,            /* request terminalCapabilitySet (its number) */
	TcsAck,         /* response terminalCapabilitySetAck */
	TcsReject,      /* response terminalCapabilitySetReject */
	Other,          /* a message not read here */
};

/* OpenLogicalChannelReject's causes, root alternatives */
enum Cause : u8 {
	UNSPECIFIED = 0, UNSUITABLE_REVERSE = 1, TYPE_NOT_SUPPORTED = 2,
	TYPE_NOT_AVAILABLE = 3, UNKNOWN_TYPE = 4, AL_NOT_SUPPORTED = 5,
};

/* a message: its type, the channel of an Olc, the channel number of
   the rest, a reject's cause, the sequence number of the capability
   messages, CloseLogicalChannel's source (lcse rather than user) */
struct Msg {
	Type type = Type::None;
	Channel ch;
	u16 lcn = 1;
	u8 cause = UNSPECIFIED;
	u8 seq = 0;
	bool lcse = false;
};

/* m in aligned PER (Type::None and Other: empty) */
std::vector<u8> encode(const Msg &m);
/* the n octets at p as a message; false if they are not one (too
   short, a choice out of range) */
bool decode(const u8 *p, size_t n, Msg *m);
/* V.75's user data: FI 133, then m */
std::vector<u8> user_data(const Msg &m);
/* the user data at p (n octets) read; false without FI 133 or a
   message after it */
bool from_user_data(const u8 *p, size_t n, Msg *m);
/* the type's name, for the log */
const char *type_name(Type t);

} // namespace h245
} // namespace dlink

#endif
