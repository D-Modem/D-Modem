/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usr_feature.cpp - the USRobotics remote feature upgrade, the host's
 *                      side (usr_feature.h, docs/x2-upgrade.md)
 *
 *    The steps: a second after LAPM is up, ask for the serial number,
 *    then for the records; with features to change, send a set or a
 *    clear for each, wait, and ask for the records again.  Each ask goes up to three
 *    times, three seconds apart.  A remote that never answers the ask
 *    for the records is not a Courier (or has no remote upgrade).
 *
 */

#include <cstring>

#include "link.h"
#include "usr_feature.h"

namespace dlink {

/* T401's rate when the pump's is not known (as LAPM's) */
static const s32 NO_RATE = 28800;

/* (see usr_feature.h) */
const char *usr_result_name(UsrResult r)
{
	switch (r) {
	case UsrResult::NoReply:
		return "NO REPLY";
	case UsrResult::Queried:
		return "QUERIED";
	case UsrResult::Already:
		return "ALREADY";
	case UsrResult::Enabled:
		return "ENABLED";
	case UsrResult::Disabled:
		return "DISABLED";
	case UsrResult::Changed:
		return "CHANGED";
	case UsrResult::Refused:
		return "REFUSED";
	}
	return "?";
}

/* the bits of the value v (1 to 8, as a selector has them) in turn:
   the next after bit, 0 when none is left */
static s16 next_bit(u8 v, s16 bit)
{
	while (++bit <= 8)
		if (v & 1 << (bit - 1))
			return bit;
	return 0;
}

/* (see usr_feature.h) */
void UsrFeature::init(Link *link, UsrMode mode, u8 enable, u8 disable)
{
	link_ = link;
	mode_ = mode;
	enable_ = enable & USR_SWITCHED;
	disable_ = disable & USR_SWITCHED & ~enable_;
	want_ = 0;
	step_ = Step::Idle;
	timer_ = 0;
	tries_ = 0;
	done_ = false;
	rep_ = UsrReport();
}

/* (see usr_feature.h) */
void UsrFeature::start()
{
	if (mode_ == UsrMode::Off || step_ != Step::Idle || done_)
		return;
	rep_ = UsrReport();
	step_ = Step::Wait;
	timer_ = ms_bits(START_MS);
	link_->logf("x2up: LAPM is up, the host begins.");
}

/* ms milliseconds as the bits we send in them */
s32 UsrFeature::ms_bits(s32 ms) const
{
	const s32 rate = link_->tx_rate() > 0 ? link_->tx_rate() : NO_RATE;
	return static_cast<s32>(static_cast<int64_t>(rate) * ms / 1000);
}

/* a message m (with the selector sel, if not -1) in a UI frame */
void UsrFeature::send(UsrMsg m, s16 sel)
{
	u8 info[3] = { USR_UI_TYPE, static_cast<u8>(m), 0 };
	s16 n = 2;
	if (sel >= 0)
		info[n++] = static_cast<u8>(sel);
	link_->usr_send(info, n);
	if (sel >= 0)
		link_->logf("x2up: sent message %d, selector %02x.",
			    static_cast<int>(m), sel);
	else
		link_->logf("x2up: sent message %d.", static_cast<int>(m));
}

/* (see usr_feature.h) */
void UsrFeature::clock(s32 bits)
{
	if (!timer_ || (timer_ -= bits) > 0)
		return;
	timer_ = 0;
	next();
}

/* the records asked for (again) */
void UsrFeature::ask_records()
{
	send(UsrMsg::AskRecords);
	timer_ = ms_bits(REPLY_MS);
}

/* the timer has run out: the step's next action */
void UsrFeature::next()
{
	switch (step_) {
	case Step::Idle:
		break;
	case Step::Wait:
		step_ = Step::Serial;
		tries_ = 1;
		send(UsrMsg::AskSerial);
		timer_ = ms_bits(REPLY_MS);
		break;
	case Step::Serial:
		if (tries_++ < TRIES) {
			send(UsrMsg::AskSerial);
			timer_ = ms_bits(REPLY_MS);
			break;
		}
		/* no serial number: the records all the same */
		step_ = Step::Records;
		tries_ = 1;
		ask_records();
		break;
	case Step::Records:
		if (tries_++ < TRIES)
			ask_records();
		else
			finish(UsrResult::NoReply);
		break;
	case Step::Set:
		step_ = Step::Check;
		tries_ = 1;
		ask_records();
		break;
	case Step::Check:
		if (tries_++ < TRIES)
			ask_records();
		else
			finish(UsrResult::Refused);
		break;
	}
}

/* (see usr_feature.h) */
void UsrFeature::rx(const u8 *info, s16 n)
{
	if (n < 2 || info[0] != USR_UI_TYPE) {
		link_->logf("x2up: UI frame of %d octets passed over.", n);
		return;
	}
	const UsrMsg m = static_cast<UsrMsg>(info[1]);
	if (m == UsrMsg::Serial && n >= 3) {
		const s16 count = info[2];
		s16 k = 0;
		for (s16 i = 0; i < count && 3 + i < n &&
		     k < USR_SERIAL_MAX; i++) {
			const u8 c = info[3 + i];
			rep_.serial[k++] = c >= 0x20 && c < 0x7f ?
				static_cast<char>(c) : '?';
		}
		rep_.serial[k] = '\0';
		link_->logf("x2up: serial number %s.", rep_.serial);
		if (step_ == Step::Serial) {
			step_ = Step::Records;
			tries_ = 1;
			ask_records();
		}
		return;
	}
	if (m != UsrMsg::Records || n < 2 + USR_RECORDS) {
		link_->logf("x2up: message %d (%d octets) passed over.",
			    info[1], n);
		return;
	}
	/* records 6 down to 1 */
	u8 recs[USR_RECORDS];
	for (s16 i = 0; i < USR_RECORDS; i++)
		recs[i] = info[2 + USR_RECORDS - 1 - i];
	link_->logf("x2up: records %03u,%03u,%03u,%03u,%03u,%03u.",
		    recs[0], recs[1], recs[2], recs[3], recs[4], recs[5]);
	memcpy(rep_.after, recs, sizeof(recs));
	if (step_ == Step::Records) {
		memcpy(rep_.before, recs, sizeof(recs));
		rep_.has_records = true;
		const u8 have = recs[USR_REC_FEATURES - 1];
		const u8 on = enable_ & ~have, off = disable_ & have;
		want_ = static_cast<u8>((have | enable_) & ~disable_);
		if (mode_ == UsrMode::Query) {
			finish(UsrResult::Queried);
		} else if (!on && !off) {
			finish(UsrResult::Already);
		} else {
			for (s16 b = next_bit(on, 0); b; b = next_bit(on, b))
				send(UsrMsg::SetBit,
				     usr_selector(USR_REC_FEATURES, b));
			for (s16 b = next_bit(off, 0); b; b = next_bit(off, b))
				send(UsrMsg::ClearBit,
				     usr_selector(USR_REC_FEATURES, b));
			step_ = Step::Set;
			timer_ = ms_bits(SET_MS);
		}
	} else if (step_ == Step::Check) {
		const u8 sw = enable_ | disable_;
		const u8 have = rep_.before[USR_REC_FEATURES - 1];
		const bool on = enable_ & ~have, off = disable_ & have;
		if ((recs[USR_REC_FEATURES - 1] & sw) != (want_ & sw))
			finish(UsrResult::Refused);
		else
			finish(on && off ? UsrResult::Changed :
			       on ? UsrResult::Enabled : UsrResult::Disabled);
	}
}

/* the end, to the owner */
void UsrFeature::finish(UsrResult r)
{
	step_ = Step::Idle;
	timer_ = 0;
	done_ = true;
	rep_.result = r;
	link_->logf("x2up: %s.", usr_result_name(r));
	link_->usr_report(rep_);
}

} // namespace dlink
