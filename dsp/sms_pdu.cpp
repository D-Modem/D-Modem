/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sms_pdu.cpp - short messages' transfer layer (3GPP TS 23.040):
 *                  SMS-SUBMIT and SMS-DELIVER for fixed-line SMS
 *
 *    A TPDU's fields, in order:
 *
 *    SMS-SUBMIT   first octet (TP-MTI 01, TP-VPF, TP-UDHI), TP-MR,
 *                 TP-DA, TP-PID, TP-DCS, TP-VP (0, 1 or 7 octets, by
 *                 TP-VPF), TP-UDL, TP-UD
 *    SMS-DELIVER  first octet (TP-MTI 00, TP-MMS, TP-UDHI), TP-OA,
 *                 TP-PID, TP-DCS, TP-SCTS (7 octets), TP-UDL, TP-UD
 *
 *    An address is its length in digits (semi-octets), its type (0x81
 *    national, 0x91 international, 0xD0 alphanumeric) and the digits two
 *    an octet, the first in the low half (0xF fills the last).  TP-UDL
 *    counts septets in the 7-bit alphabet (packed 8 in 7 octets, the
 *    first in the low bits), octets in 8-bit data and UCS2.  With
 *    TP-UDHI the user data starts with a header (its length first),
 *    which the DTE does not see.
 *
 */

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sms_pdu.h"

namespace dsp {
namespace sms {

/* the GSM 7-bit default alphabet (TS 23.038 6.2.1): each septet's
   Unicode code point (0x1B the escape to the table below) */
static const u16 gsm7[128] = {
	0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
	0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
	0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
	0x03A3, 0x0398, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
	0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
	0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
	0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
	0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
	0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
	0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
	0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
	0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
	0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
	0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
	0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
	0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

/* the escape table's characters (6.2.1.1): the septet after 0x1B and
   its code point */
static const struct {
	u8 septet;
	u16 cp;
} gsm7_ext[] = {
	{ 0x0A, 0x000C }, { 0x14, 0x005E }, { 0x28, 0x007B },
	{ 0x29, 0x007D }, { 0x2F, 0x005C }, { 0x3C, 0x005B },
	{ 0x3D, 0x007E }, { 0x3E, 0x005D }, { 0x40, 0x007C },
	{ 0x65, 0x20AC },
};

/* the longest user data: 160 septets, 140 octets */
#define SMS_MAX_SEPTETS 160
#define SMS_MAX_OCTETS  140

/* UTF-8 text s as code points (a bad sequence U+FFFD) */
static std::vector<u32> utf8_decode(const std::string &s)
{
	std::vector<u32> out;
	size_t i = 0, n = s.size();

	while (i < n) {
		u8 c = static_cast<u8>(s[i++]);
		s16 more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 :
			(c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (more < 0) {
			out.push_back(0xFFFD);
			continue;
		}
		u32 cp = more ? c & (0x3F >> more) : c;
		s16 k;
		for (k = 0; k < more && i < n &&
			    (static_cast<u8>(s[i]) & 0xC0) == 0x80; k++)
			cp = cp << 6 | (static_cast<u8>(s[i++]) & 0x3F);
		out.push_back(k == more ? cp : 0xFFFD);
	}
	return out;
}

/* code point cp onto UTF-8 text out */
static void utf8_put(std::string *out, u32 cp)
{
	if (cp < 0x80) {
		*out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		*out += static_cast<char>(0xC0 | cp >> 6);
		*out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*out += static_cast<char>(0xE0 | cp >> 12);
		*out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
		*out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		*out += static_cast<char>(0xF0 | cp >> 18);
		*out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
		*out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
		*out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

/* code points cps as 7-bit septets into out (escapes for the second
   table); false if one has no place in either */
static bool gsm7_encode(std::vector<u8> *out, const std::vector<u32> &cps)
{
	out->clear();
	for (u32 cp : cps) {
		s16 found = -1;
		for (s16 i = 0; i < 128 && found < 0; i++)
			if (gsm7[i] == cp && i != 0x1B)
				found = i;
		if (found >= 0) {
			out->push_back(static_cast<u8>(found));
			continue;
		}
		for (const auto &e : gsm7_ext)
			if (e.cp == cp)
				found = e.septet;
		if (found < 0)
			return false;
		out->push_back(0x1B);
		out->push_back(static_cast<u8>(found));
	}
	return true;
}

/* septets packed into octets onto out */
static void gsm7_pack(std::vector<u8> *out, const std::vector<u8> &sep)
{
	size_t base = out->size(), bits = sep.size() * 7;
	out->resize(base + (bits + 7) / 8, 0);
	for (size_t i = 0; i < sep.size(); i++) {
		size_t pos = i * 7, o = base + pos / 8;
		u32 v = static_cast<u32>(sep[i] & 0x7F) << (pos % 8);
		(*out)[o] |= static_cast<u8>(v);
		if (v >> 8)
			(*out)[o + 1] |= static_cast<u8>(v >> 8);
	}
}

/* septet i of the packed octets p (n of them), -1 past their end */
static s16 gsm7_septet(const u8 *p, s32 n, s32 i)
{
	s32 pos = i * 7, o = pos / 8, sh = pos % 8;
	if (o >= n)
		return -1;
	u32 v = p[o] >> sh;
	if (sh > 1) {
		if (o + 1 >= n)
			return -1;
		v |= static_cast<u32>(p[o + 1]) << (8 - sh);
	}
	return static_cast<s16>(v & 0x7F);
}

/* septets first to count of the packed octets p (n) as UTF-8 text
   onto out; false if they run past the octets */
static bool gsm7_text(std::string *out, const u8 *p, s32 n, s32 first,
		      s32 count)
{
	for (s32 i = first; i < count; i++) {
		s16 c = gsm7_septet(p, n, i);
		if (c < 0)
			return false;
		if (c != 0x1B) {
			utf8_put(out, gsm7[c]);
			continue;
		}
		/* an escape: the next septet's in the second table, else
		   its own (23.038 6.2.1.1) */
		if (++i >= count)
			break;
		c = gsm7_septet(p, n, i);
		if (c < 0)
			return false;
		u32 cp = gsm7[c];
		for (const auto &e : gsm7_ext)
			if (e.septet == c)
				cp = e.cp;
		utf8_put(out, cp);
	}
	return true;
}

/* bytes p (n) in upper case hex onto out */
static void hex_put(std::string *out, const u8 *p, s32 n)
{
	static const char digits[] = "0123456789ABCDEF";
	for (s32 i = 0; i < n; i++) {
		*out += digits[p[i] >> 4];
		*out += digits[p[i] & 15];
	}
}

/* see sms_pdu.h */
bool from_hex(std::vector<u8> *out, const std::string &hex)
{
	out->clear();
	if (hex.size() % 2)
		return false;
	for (size_t i = 0; i < hex.size(); i += 2) {
		unsigned v;
		char two[3] = { hex[i], hex[i + 1], 0 };
		char *end;
		if (!isxdigit(static_cast<unsigned char>(two[0])) ||
		    !isxdigit(static_cast<unsigned char>(two[1])))
			return false;
		v = static_cast<unsigned>(strtoul(two, &end, 16));
		out->push_back(static_cast<u8>(v));
	}
	return !out->empty();
}

/* a semi-octet's digit (0xA *, 0xB #, 0xC to 0xE a to c) */
static char bcd_digit(u8 v)
{
	return "0123456789*#abc?"[v & 15];
}

/* digit c's semi-octet, -1 for none */
static s16 bcd_value(char c)
{
	if (c >= '0' && c <= '9')
		return static_cast<s16>(c - '0');
	if (c == '*')
		return 10;
	if (c == '#')
		return 11;
	return -1;
}

/* number as an address onto out (letters alphanumeric when alnum);
   false if it is none */
static bool address_put(std::vector<u8> *out, const std::string &number,
			bool alnum)
{
	bool intl = !number.empty() && number[0] == '+';
	std::string digits = number.substr(intl ? 1 : 0);
	bool letters = false;

	for (char c : digits)
		if (bcd_value(c) < 0)
			letters = true;
	if (digits.empty())
		return false;
	if (letters) {
		std::vector<u8> sep;
		if (!alnum || intl ||
		    !gsm7_encode(&sep, utf8_decode(digits)) || sep.size() > 11)
			return false;
		std::vector<u8> packed;
		gsm7_pack(&packed, sep);
		out->push_back(static_cast<u8>((sep.size() * 7 + 3) / 4));
		out->push_back(0xD0);
		out->insert(out->end(), packed.begin(), packed.end());
		return true;
	}
	if (digits.size() > 20)
		return false;
	out->push_back(static_cast<u8>(digits.size()));
	out->push_back(intl ? 0x91 : 0x81);
	for (size_t i = 0; i < digits.size(); i += 2) {
		u8 lo = static_cast<u8>(bcd_value(digits[i]));
		u8 hi = i + 1 < digits.size() ?
			static_cast<u8>(bcd_value(digits[i + 1])) : 0xF;
		out->push_back(static_cast<u8>(hi << 4 | lo));
	}
	return true;
}

/* the address at p[*i] (n octets in all) into number, *i past it;
   false if it runs past them */
static bool address_get(std::string *number, const u8 *p, s32 n, s32 *i)
{
	if (*i + 2 > n)
		return false;
	s32 len = p[*i], toa = p[*i + 1], octets = (len + 1) / 2;
	const u8 *d = p + *i + 2;
	if (*i + 2 + octets > n)
		return false;
	*i += 2 + octets;
	number->clear();
	if ((toa & 0x70) == 0x50)
		return gsm7_text(number, d, octets, 0, len * 4 / 7);
	if ((toa & 0x70) == 0x10)
		*number += '+';
	for (s32 k = 0; k < len; k++)
		*number += bcd_digit(k & 1 ? d[k / 2] >> 4 : d[k / 2]);
	return true;
}

/* text as user data: its TP-DCS, TP-UDL and the data onto out; false if
   it is empty or too long */
static bool user_data_put(std::vector<u8> *out, const std::string &text)
{
	std::vector<u32> cps = utf8_decode(text);
	std::vector<u8> sep;

	if (cps.empty())
		return false;
	if (gsm7_encode(&sep, cps)) {
		if (sep.size() > SMS_MAX_SEPTETS)
			return false;
		out->push_back(0x00);           /* TP-DCS: 7-bit */
		out->push_back(static_cast<u8>(sep.size()));
		gsm7_pack(out, sep);
		return true;
	}
	std::vector<u8> ucs2;
	for (u32 cp : cps) {
		if (cp >= 0x10000) {
			cp -= 0x10000;
			u32 hi = 0xD800 | cp >> 10, lo = 0xDC00 | (cp & 0x3FF);
			ucs2.push_back(static_cast<u8>(hi >> 8));
			ucs2.push_back(static_cast<u8>(hi));
			ucs2.push_back(static_cast<u8>(lo >> 8));
			ucs2.push_back(static_cast<u8>(lo));
		} else {
			ucs2.push_back(static_cast<u8>(cp >> 8));
			ucs2.push_back(static_cast<u8>(cp));
		}
	}
	if (ucs2.size() > SMS_MAX_OCTETS)
		return false;
	out->push_back(0x08);                   /* TP-DCS: UCS2 */
	out->push_back(static_cast<u8>(ucs2.size()));
	out->insert(out->end(), ucs2.begin(), ucs2.end());
	return true;
}

/* see sms_pdu.h */
bool submit(std::vector<u8> *pdu, const std::string &number,
	    const std::string &text, u8 mr)
{
	pdu->assign({ 0x01, mr });              /* TP-MTI SMS-SUBMIT, no VP */
	if (!address_put(pdu, number, false))
		return false;
	pdu->push_back(0x00);                   /* TP-PID */
	return user_data_put(pdu, text);
}

/* v (0 to 99) in swapped BCD */
static u8 bcd2(s16 v)
{
	return static_cast<u8>((v % 10) << 4 | (v / 10 % 10));
}

/* see sms_pdu.h */
bool deliver(std::vector<u8> *pdu, const std::string &number,
	     const std::string &text, const Scts &t)
{
	/* TP-MTI SMS-DELIVER, TP-MMS: no more messages */
	pdu->assign({ 0x04 });
	if (!address_put(pdu, number, true))
		return false;
	pdu->push_back(0x00);                   /* TP-PID */
	std::vector<u8> ud;
	if (!user_data_put(&ud, text))
		return false;
	pdu->push_back(ud[0]);                  /* TP-DCS */
	s16 z = t.zone < 0 ? static_cast<s16>(-t.zone) : t.zone;
	pdu->insert(pdu->end(), {
		bcd2(t.year), bcd2(t.month), bcd2(t.day), bcd2(t.hour),
		bcd2(t.minute), bcd2(t.second),
		static_cast<u8>(bcd2(z) | (t.zone < 0 ? 0x08 : 0)) });
	pdu->insert(pdu->end(), ud.begin() + 1, ud.end());
	return true;
}

/* see sms_pdu.h: the civil date of the day count (Hinnant's
   days_from_civil inverted) */
Scts now()
{
	using namespace std::chrono;
	s64 secs = duration_cast<seconds>(
		system_clock::now().time_since_epoch()).count();
	s64 days = secs / 86400, sod = secs % 86400;
	days += 719468;
	s64 era = days / 146097;
	s64 doe = days - era * 146097;
	s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	s64 mp = (5 * doy + 2) / 153;
	s64 d = doy - (153 * mp + 2) / 5 + 1;
	s64 m = mp < 10 ? mp + 3 : mp - 9;
	s64 y = yoe + era * 400 + (m <= 2);
	Scts t;
	t.year = static_cast<s16>(y % 100);
	t.month = static_cast<s16>(m);
	t.day = static_cast<s16>(d);
	t.hour = static_cast<s16>(sod / 3600);
	t.minute = static_cast<s16>(sod / 60 % 60);
	t.second = static_cast<s16>(sod % 60);
	t.zone = 0;
	return t;
}

/* TP-DCS's alphabet: 0 7-bit, 1 8-bit data (and compressed text), 2
   UCS2 (23.038 4) */
static s16 alphabet(u8 dcs)
{
	if ((dcs & 0x80) == 0) {
		if (dcs & 0x20)
			return 1;
		s16 a = (dcs >> 2) & 3;
		return a == 3 ? 0 : a;
	}
	if ((dcs & 0xF0) == 0xF0)
		return dcs & 0x04 ? 1 : 0;
	if ((dcs & 0xF0) == 0xE0)
		return 2;
	if ((dcs & 0xE0) == 0xC0)
		return 0;
	return 1;
}

/* the user data at p[i] (n octets in all; TP-UDL first) as text onto
   out: udhi its header skipped; false if it runs past them */
static bool user_data_get(std::string *out, const u8 *p, s32 n, s32 i,
			  u8 dcs, bool udhi)
{
	if (i >= n)
		return false;
	s32 udl = p[i++];
	const u8 *ud = p + i;
	s32 left = n - i, skip = 0;
	s16 a = alphabet(dcs);

	if (udhi) {
		if (left < 1)
			return false;
		skip = ud[0] + 1;
	}
	if (a == 0) {
		if ((udl * 7 + 7) / 8 > left)
			return false;
		return gsm7_text(out, ud, left, (skip * 8 + 6) / 7, udl);
	}
	if (udl > left || skip > udl)
		return false;
	if (a == 1) {
		hex_put(out, ud + skip, udl - skip);
		return true;
	}
	for (s32 k = skip; k + 1 < udl; k += 2) {
		u32 c = static_cast<u32>(ud[k]) << 8 | ud[k + 1];
		if (c >= 0xD800 && c < 0xDC00 && k + 3 < udl) {
			u32 lo = static_cast<u32>(ud[k + 2]) << 8 | ud[k + 3];
			if (lo >= 0xDC00 && lo < 0xE000) {
				c = 0x10000 + ((c - 0xD800) << 10) +
					(lo - 0xDC00);
				k += 2;
			}
		}
		utf8_put(out, c);
	}
	return true;
}

/* swapped BCD v's value (a bad digit as 0) */
static s16 bcd_get(u8 v)
{
	s16 lo = v & 15, hi = v >> 4;
	return static_cast<s16>((lo > 9 ? 0 : lo) * 10 + (hi > 9 ? 0 : hi));
}

/* the lines of an SMS-DELIVER or SMS-SUBMIT p (n), "" if it is neither
   or runs short */
static std::string decode(const u8 *p, s32 n)
{
	std::string number, text, head;
	s32 i = 1;

	if (n < 1)
		return "";
	u8 fo = p[0];
	bool udhi = fo & 0x40;
	switch (fo & 3) {
	case 0: {                               /* SMS-DELIVER */
		if (!address_get(&number, p, n, &i) || i + 9 > n)
			return "";
		u8 dcs = p[i + 1];
		const u8 *t = p + i + 2;
		char when[32];
		s16 zone = bcd_get(t[6] & 0xF7);
		snprintf(when, sizeof(when),
			 "%02d/%02d/%02d,%02d:%02d:%02d%c%02d",
			 bcd_get(t[0]), bcd_get(t[1]), bcd_get(t[2]),
			 bcd_get(t[3]), bcd_get(t[4]), bcd_get(t[5]),
			 t[6] & 0x08 ? '-' : '+', zone);
		if (!user_data_get(&text, p, n, i + 9, dcs, udhi))
			return "";
		head = "+CMT: \"" + number + "\",,\"" + when + "\"";
		break;
	}
	case 1: {                               /* SMS-SUBMIT */
		static const s16 vp_len[4] = { 0, 7, 1, 7 };
		i = 2;                          /* (past TP-MR) */
		if (n < 2 || !address_get(&number, p, n, &i))
			return "";
		s32 vp = vp_len[fo >> 3 & 3];
		if (i + 2 + vp > n)
			return "";
		u8 dcs = p[i + 1];
		if (!user_data_get(&text, p, n, i + 2 + vp, dcs, udhi))
			return "";
		head = "+SUBMIT: \"" + number + "\"";
		break;
	}
	default:
		return "";
	}
	return head + "\r\n" + text + "\r\n";
}

/* see sms_pdu.h */
std::string to_dte(const u8 *pdu, s32 n)
{
	std::string s = decode(pdu, n);
	if (!s.empty())
		return s;
	s = "+PDU: ";
	hex_put(&s, pdu, n);
	return s + "\r\n";
}

} // namespace sms
} // namespace dsp
