/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hayes_xid.cpp - the Hayes private parameter sets (see hayes_xid.h)
 *
 *    The firmware's addresses (bank 3): the template 765Fh, the local
 *    values 7699h, the parser 7940h (the sets found by name at 79BEh),
 *    the results 7B2Ah to 7C21h, the selection of the compression 6759h.
 *
 */

#include <cstring>

#include "hayes_xid.h"

namespace dlink {

/* the parameter that names a set, the length of a name, and the
   parameters in the sets */
static const u8 PI_SET = 0xff, SET_NAME_LEN = 3;
static const u8 PI_P1 = 0x01, PI_P2 = 0x02;
/* V.44's set identifier: another set starts */
static const u8 PI_V44_SET = 0x40;

/* our sets (7699h): HMP P1 is s46, HMP P2 its compression bit */
HayesSets hayes_local(u8 s46, u8 aft_caps, u8 aft_opts)
{
	HayesSets s;
	s.aft = s.hmp = true;
	s.aft_p1 = aft_caps;
	s.aft_p2 = aft_opts;
	s.hmp_p1 = s46;
	s.hmp_p2 = s46 & HAYES_HMP_COMP ? 1 : 0;
	return s;
}

/* one set at p: FFh, the length of the name, the name, P1 and P2 of
   one octet each; returns the end */
static u8 *put_set(u8 *p, const char *name, u8 p1, u8 p2)
{
	*p++ = PI_SET;
	*p++ = SET_NAME_LEN;
	memcpy(p, name, SET_NAME_LEN);
	p += SET_NAME_LEN;
	*p++ = PI_P1;
	*p++ = 1;
	*p++ = p1;
	*p++ = PI_P2;
	*p++ = 1;
	*p++ = p2;
	return p;
}

/* the AFT set, then the HMP set (the order of the template) */
u8 *hayes_put_sets(u8 *p, const HayesSets &s)
{
	p = put_set(p, "AFT", s.aft_p1, s.aft_p2);
	return put_set(p, "HMP", s.hmp_p1, s.hmp_p2);
}

/* one parameter of the user data subfield: a name starts a set (by
   name, as 79BEh finds them), P1 and P2 go to the set that is open */
void HayesSetParser::param(u8 pi, const u8 *pv, s16 pl)
{
	if (pi == PI_SET) {
		set_ = Set::None;
		if (pl == SET_NAME_LEN && !memcmp(pv, "AFT", SET_NAME_LEN)) {
			set_ = Set::Aft;
			sets_.aft = true;
		} else if (pl == SET_NAME_LEN &&
			   !memcmp(pv, "HMP", SET_NAME_LEN)) {
			set_ = Set::Hmp;
			sets_.hmp = true;
		}
		return;
	}
	if (pi == PI_V44_SET) {
		set_ = Set::None;
		return;
	}
	if (set_ == Set::None || pl < 1 || (pi != PI_P1 && pi != PI_P2))
		return;
	/* (a longer value: its last octet, the low one) */
	const u8 v = pv[pl - 1];
	if (set_ == Set::Aft)
		(pi == PI_P1 ? sets_.aft_p1 : sets_.aft_p2) = v;
	else
		(pi == PI_P1 ? sets_.hmp_p1 : sets_.hmp_p2) = v;
}

/* the results (7B2Ah): AFT P1 the AND, AFT P2 the OR, HMP P1 the AND
   with its bit 1 the compression; nothing agreed if either has no HMP
   set */
HayesAgreed hayes_agree(const HayesSets &local, const HayesSets &remote)
{
	HayesAgreed a;
	if (!local.hmp || !remote.hmp)
		return a;
	a.hayes = true;
	if (local.aft && remote.aft) {
		a.aft_caps = static_cast<u8>(local.aft_p1 & remote.aft_p1);
		a.aft_opts = static_cast<u8>(local.aft_p2 | remote.aft_p2);
	}
	a.hmp = static_cast<u8>(local.hmp_p1 & remote.hmp_p1);
	a.comp = (a.hmp & HAYES_HMP_COMP) != 0;
	return a;
}

/* (6759h) no compression flag: none; V.42bis agreed: it; else ADC in
   the two directions */
HayesComp hayes_select(const HayesAgreed &a, bool v42bis)
{
	if (!a.comp)
		return HayesComp::None;
	return v42bis ? HayesComp::V42bis : HayesComp::Adc;
}

/* (bank 8 760Ah: 69 minus the mode) */
s16 hayes_comp_result(HayesComp c)
{
	return c == HayesComp::Adc ? 68 : c == HayesComp::V42bis ? 67 : 69;
}

/* the template (765Fh) with the local values (7699h) */
s16 hayes_xid_template(u8 *p, u8 k, u8 p0, u8 s46, u8 aft_caps,
		       u8 aft_opts)
{
	static const u8 head[] = {
		0x82,                                   /* FI */
		0x80, 0x00, 0x13,                       /* GI 80h, GL 19 */
		0x03, 0x03, 0x8a, 0x89, 0x00,           /* HDLC options */
		0x05, 0x02, 0x04, 0x00,                 /* N401 transmit */
		0x06, 0x02, 0x04, 0x00,                 /* N401 receive */
		0x07, 0x01, 0x00,                       /* k transmit */
		0x08, 0x01, 0x00,                       /* k receive */
		0xf0, 0x00, 0x0f,                       /* GI F0h, GL 15 */
		0x00, 0x03, 0x56, 0x34, 0x32,           /* "V42" */
		0x01, 0x01, 0x00,                       /* V.42bis P0 */
		0x02, 0x02, 0x04, 0x00,                 /* P1 1024 */
		0x03, 0x01, 0x20,                       /* P2 32 */
		0xff,                                   /* the private part */
	};
	const u8 kk = static_cast<u8>(k < 1 ? 1 : k > 7 ? 7 : k);
	u8 *q = p;
	memcpy(q, head, sizeof(head));
	q[19] = q[22] = kk;
	q[33] = p0;
	q += sizeof(head);
	q = hayes_put_sets(q, hayes_local(s46, aft_caps, aft_opts));
	return static_cast<s16>(q - p);
}

} // namespace dlink
