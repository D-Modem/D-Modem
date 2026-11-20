/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usr_feature.h - the USRobotics remote feature upgrade: the host's
 *                    side of the exchange that turned on x2 in a
 *                    Courier V.Everything (docs/x2-upgrade.md)
 *
 *    In 1997 a Courier owner who bought x2 flashed the x2 code, then
 *    called a U.S. Robotics host, which turned the feature on.  The
 *    host's side was never published.  The Courier's side is in its
 *    firmware (supervisor 7.1.4, 02/25/97; the same code is in the
 *    builds to 7.4.16): LAPM UI frames whose information field starts
 *    with the type octet 0x0a, then a message number:
 *
 *       0  the six feature records (the answer to 1)
 *       1  ask for the records
 *       2  set one bit of a record (one selector octet)
 *       3  ask for the serial number
 *       4  the serial number (the answer to 3): a count, the octets
 *       5  clear one bit of a record (one selector octet)
 *
 *    The six records are ATY14's six fields: 1 the country (ATC10),
 *    2 the features (ATC8), 3 and 4 the board types, 5 and 6 unused.
 *    Message 0 has them from record 6 down to record 1.  A selector's
 *    high nibble is the record, its low nibble the bit (1 to 8:
 *    1 << (n - 1)).  The features: HST 1, fax 2, Terbo 4, V.FC and V.34
 *    8, x2 16 (V.90 too in the builds after x2).  The Courier answers 1
 *    and 3 in UI commands of its own, and keeps a set or clear without
 *    an answer; the records take effect at its next power on.  It
 *    passes over a frame with P set.  Builds after 7.1.4 keep a change
 *    only with S30 bit 0 ("Enable feature upgrades") set.
 *
 */

#ifndef DMODEM_LINK_USR_FEATURE_H
#define DMODEM_LINK_USR_FEATURE_H


#include "dm_types.h"

namespace dlink {

class Link;

enum {
	USR_UI_TYPE = 0x0a,     /* the first octet of the UI frame's
				   information field */
	USR_RECORDS = 6,        /* the feature records */
	USR_REC_FEATURES = 2,   /* the protocols (ATC8) */
	USR_BIT_TERBO = 3,      /* its bit for V.32terbo (the value 4) */
	USR_BIT_VFC = 4,        /* for V.FC and V.34 (8) */
	USR_BIT_X2 = 5,         /* for x2, and V.90 in the builds after
				   x2 (16) */
	/* the features the host may switch: Terbo, V.FC and x2 */
	USR_SWITCHED = 1 << (USR_BIT_TERBO - 1) | 1 << (USR_BIT_VFC - 1) |
		1 << (USR_BIT_X2 - 1),
	USR_SERIAL_MAX = 24,    /* the longest serial number we keep */
};

/* the message numbers after the type octet */
enum class UsrMsg : u8 {
	Records = 0,
	AskRecords = 1,
	SetBit = 2,
	AskSerial = 3,
	Serial = 4,
	ClearBit = 5,
};

/* a selector octet: record 1 to 6, bit 1 to 8 */
constexpr u8 usr_selector(s16 record, s16 bit)
{
	return static_cast<u8>((record & 0x0f) << 4 | (bit & 0x0f));
}

/* what the host asks for (+X2UP's <mode>) */
enum class UsrMode : s16 {
	Off = 0,
	Query = 1,      /* read the serial number and the records */
	Change = 2,     /* and enable and disable features (the
			   UsrFeature's set and clear masks) */
};

/* how the exchange ended */
enum class UsrResult {
	NoReply,        /* the remote did not answer (not a Courier) */
	Queried,        /* the records were read (UsrMode::Query) */
	Already,        /* the features were as asked already */
	Enabled,        /* features were enabled, as asked */
	Disabled,       /* features were disabled, as asked */
	Changed,        /* features were enabled and others disabled */
	Refused,        /* the changes were sent, the records did not
			   take them (S30) */
};

/* what the exchange found */
struct UsrReport {
	UsrResult result = UsrResult::NoReply;
	/* the serial number: printable octets as they came */
	char serial[USR_SERIAL_MAX + 1] = {};
	/* the records the first answer had, then the last one (index 0
	   is record 1); valid when has_records */
	bool has_records = false;
	u8 before[USR_RECORDS] = {};
	u8 after[USR_RECORDS] = {};
};

/* the text of a result for +X2UP: (ENABLED, ...) */
const char *usr_result_name(UsrResult r);

/* the host's side, on the answerer once LAPM is up: ask for the serial
   number and the records, enable and disable features as asked
   (UsrMode::Change) and read the records again, then report to the owner
   (Owner::usr_feature()).  Its timers count the bits we send. */
class UsrFeature {
public:
	/* a call: mode as asked; in UsrMode::Change, the features (record
	   2's values) to enable and to disable, each within USR_SWITCHED
	   (others are passed over); a feature in both is enabled */
	void init(Link *link, UsrMode mode, u8 enable = 1 << (USR_BIT_X2 - 1),
		  u8 disable = 0);
	/* LAPM is up: the exchange begins (nothing in UsrMode::Off, or
	   when it has been already in this call) */
	void start();
	/* bits bits went out */
	void clock(s32 bits);
	/* a UI frame came, with the n octets of its information field */
	void rx(const u8 *info, s16 n);
	/* the exchange is going on */
	bool running() const { return step_ != Step::Idle; }

private:
	enum class Step { Idle, Wait, Serial, Records, Set, Check };
	enum {
		START_MS = 1000,        /* from LAPM up to the first ask */
		REPLY_MS = 3000,        /* for an answer */
		SET_MS = 1500,          /* from the set to the check */
		TRIES = 3,              /* asks before no reply */
	};

	s32 ms_bits(s32 ms) const;
	void send(UsrMsg m, s16 sel = -1);
	void next();
	void ask_records();
	void finish(UsrResult r);

	Link *link_ = nullptr;
	UsrMode mode_ = UsrMode::Off;
	Step step_ = Step::Idle;
	s32 timer_ = 0;         /* bits to go (0: none) */
	s16 tries_ = 0;
	u8 enable_ = 0;         /* the features to enable */
	u8 disable_ = 0;        /* and to disable */
	u8 want_ = 0;           /* record 2 as it is to be, in USR_SWITCHED */
	bool done_ = false;     /* once in a call */
	UsrReport rep_;
};

} // namespace dlink

#endif
