/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sms_pdu.h - short messages' transfer layer (3GPP TS 23.040, once
 *                GSM 03.40): SMS-SUBMIT and SMS-DELIVER to and from
 *                the lines fixed-line SMS's pump (dp_sms.cpp) has with
 *                its DTE
 *
 *    The text is UTF-8 at the DTE; in the message it is GSM's 7-bit
 *    default alphabet (with its escapes) when every character has a
 *    place there, else UCS2.  A number is digits (and *, #), '+' first
 *    for an international one; in an SMS-DELIVER's originator, letters
 *    make it alphanumeric.
 *
 */

#ifndef __SMS_PDU_H__
#define __SMS_PDU_H__

#include <string>
#include <vector>

#include "dm_types.h"

namespace dsp {
namespace sms {

/* the service centre's time stamp: years since 2000, month, day, hour,
   minute, second, and the zone in quarter hours east of UTC */
struct Scts {
	s16 year, month, day, hour, minute, second, zone;
};

/* an SMS-SUBMIT of text to number with message reference mr into pdu;
   false if the number or text will not go (empty, too long) */
bool submit(std::vector<u8> *pdu, const std::string &number,
	    const std::string &text, u8 mr);

/* an SMS-DELIVER of text from number, stamped t, into pdu; false if it
   will not go */
bool deliver(std::vector<u8> *pdu, const std::string &number,
	     const std::string &text, const Scts &t);

/* the time now (UTC) as a time stamp */
Scts now();

/* the DTE's lines for a received TPDU: +CMT: "<oa>",,"<scts>" and the
   text for an SMS-DELIVER, +SUBMIT: "<da>" and the text for an
   SMS-SUBMIT (8-bit data in hex), +PDU: <hex> for anything else; each
   line ends in CR LF */
std::string to_dte(const u8 *pdu, s32 n);

/* hex text into bytes; false if it is not an even number of hex
   digits */
bool from_hex(std::vector<u8> *out, const std::string &hex);

} // namespace sms
} // namespace dsp

#endif /* __SMS_PDU_H__ */
