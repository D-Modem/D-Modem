/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usr_feature.cpp - the USRobotics remote feature upgrade, the host's
 *                      side (usr_feature.h, docs/x2-upgrade.md)
 *
 *    The steps: a second after LAPM is up, ask for the serial number,
 *    then for the records; with x2 to be set and not set, send the set,
 *    wait, and ask for the records again.  Each ask goes up to three
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
	case UsrResult::Refused:
		return "REFUSED";
	}
	return "?";
}

/* the records' bit for x2 is set in recs */
static bool has_x2(const u8 *recs)
{
	return recs[USR_REC_FEATURES - 1] & 1 << (USR_BIT_X2 - 1);
}

/* (see usr_feature.h) */
void UsrFeature::init(Link *link, UsrMode mode)
{
	link_ = link;
	mode_ = mode;
	step_ = Step::Idle;
	timer_ = 0;
	tries_ = 0;
	set_sent_ = false;
	done_ = false;
	rep_ = UsrReport();
}

/* (see usr_feature.h) */
void UsrFeature::start()
{
	if (mode_ == UsrMode::Off || step_ != Step::Idle || done_)
		return;
	rep_ = UsrReport();
	set_sent_ = false;
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
		if (mode_ == UsrMode::Query) {
			finish(UsrResult::Queried);
		} else if (has_x2(recs)) {
			finish(UsrResult::Already);
		} else {
			step_ = Step::Set;
			set_sent_ = true;
			send(UsrMsg::SetBit,
			     usr_selector(USR_REC_FEATURES, USR_BIT_X2));
			timer_ = ms_bits(SET_MS);
		}
	} else if (step_ == Step::Check) {
		finish(has_x2(recs) ? UsrResult::Enabled : UsrResult::Refused);
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
