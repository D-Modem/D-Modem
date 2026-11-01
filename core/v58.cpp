/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v58.cpp - V.250's DCE testing commands (6.7/V.250), on V.58's test
 *              objects: the +T commands, over V.54's loops and V.52's
 *              pattern (v54.h)
 *
 *    V.58 (1994) models a V-series DCE for a management system; its test
 *    objects (the loops, their status, the error rate test and the self
 *    test) are what V.250 6.7 made into AT commands.  Here they drive
 *    the loop tests AT&T has:
 *
 *      +TERDL=<0|1>    the far end may ask for loop 2 here (AT&T4, AT&T5)
 *      +TLDL=<0|1>     loop 2 here, online command state (AT&T3, AT&T0)
 *      +TRDL=<0|1>     loop 2 at the far end, online command state: OK
 *                      when the far end loops, ERROR when it does not
 *                      (AT&T6, but OK in online command state, not
 *                      CONNECT); 0 ends it (OK when the TERM has gone)
 *      +TAL=<0|1>[,<band>]  loop 3, command state, 4-wire leased line:
 *                      OK when it runs, in online command state (AT&T1,
 *                      but OK, not CONNECT); 0 ends it
 *      +TTER=<type>,<block_length>,<blocks>,<pattern>  the error rate
 *                      test on the loop that runs (A's loop 2, loop 3),
 *                      online command state: V.52's 511-bit pattern
 *                      (<pattern> 1, the only one) for <blocks> blocks
 *                      of <block_length> bits; +TTER=0 stops it
 *      +TNUM?          its bit and block errors (the last test's)
 *      +TALS?, +TDLS?, +TRDLS?  the loops' status: loop 3 (0 none, 3
 *                      a command's), loop 2 here (1 none, 3 ours, 4 the
 *                      far end's), loop 2 there (1 none, 4 ours)
 *      +TSELF=<0|1>    the self-test (both kinds alike: the loop tests'
 *                      machinery checks itself); +TRES? its result
 *      +TE140, +TE141, +TEPDL, +TEPAL, +TMODE, +TADR  0 alone: no
 *                      circuits 140 and 141, no front panel, point to
 *                      point, no V.54 address
 *
 *    Each with ? and =? as V.250 has them.  +TNUM's line is "+TNUM: "
 *    (6.7.2.12/V.250 prints "+TTER: " for it, a slip).
 */
#include <algorithm>
#include <cinttypes>
#include <cstring>

#include <dsplibs.h>
#include <link.h>
#include <modem_dp.h>

#include "dm_types.h"
#include "dcore.h"
#include "str.h"

namespace dcore {

namespace {

/* the parameters that are 0 alone here */
const char *const t_fixed[] = {
	"+TE140", "+TE141", "+TEPDL", "+TEPAL", "+TMODE", "+TADR",
};

/* a decimal number at p (digits), *end after it; false when there is
   none or it is above most */
bool t_number(const char *p, const char **end, u32 most, u32 *v)
{
	u32 n = 0;
	const char *q = p;
	while (*q >= '0' && *q <= '9') {
		n = n * 10 + static_cast<u32>(*q - '0');
		if (n > most)
			return false;
		q++;
	}
	*end = q;
	*v = n;
	return q != p;
}

}  // namespace

/* the +T commands (see the top of this file) */
s16 Modem::at_T(char *p, s16 *len)
{
	using Phase = v54::LoopTest::Phase;
	const char *q;
	auto line = [this](const std::string &s) {
		puts(s);
		crlf();
	};
	/* the command's name (the longest that matches), q after it */
	static const char *const names[] = {
		"+TE140", "+TE141", "+TEPDL", "+TEPAL", "+TMODE", "+TADR",
		"+TERDL", "+TALS", "+TDLS", "+TRDLS", "+TTER", "+TNUM",
		"+TLDL", "+TRDL", "+TAL", "+TSELF", "+TRES",
	};
	const char *name = nullptr;
	size_t name_n = 0;
	for (const char *n : names) {
		size_t k = strlen(n);
		if (k > name_n && starts_nocase(p, n)) {
			name = n;
			name_n = k;
		}
	}
	if (!name)
		return -1;
	q = p + name_n;
	const std::string nm(name);
	const bool test = q[0] == '=' && q[1] == '?';
	const bool read = q[0] == '?';
	const bool set = q[0] == '=' && !test;
	*len = static_cast<s16>(q - p + (test ? 2 : read ? 1 : 0));
	const bool online_cmd = state_ == COMMAND_ONLINE;
	const Phase ph = test_.phase();

	for (const char *f : t_fixed) {
		if (nm != f)
			continue;
		if (test) {
			line(nm + ": (0)");
		} else if (read) {
			line(nm + ": 0");
		} else if (set) {
			u32 v;
			if (!t_number(q + 1, &q, 0, &v))
				return -1;
			*len = static_cast<s16>(q - p);
		} else {
			return -1;
		}
		return 0;
	}

	/* the read-only status parameters */
	if (nm == "+TALS" || nm == "+TDLS" || nm == "+TRDLS" ||
	    nm == "+TNUM" || nm == "+TRES") {
		if (test) {
			line(nm + ": " + (nm == "+TALS" ? "(0,3)" :
					  nm == "+TDLS" ? "(1,3,4)" :
					  nm == "+TRDLS" ? "(1,4)" :
					  nm == "+TRES" ? "(0-2)" :
					  "(0-65535),(0-65535)"));
			return 0;
		}
		if (!read)
			return -1;
		u32 v = 0;
		if (nm == "+TALS") {
			v = loop3_ && ph == Phase::Local3 ? 3 : 0;
		} else if (nm == "+TDLS") {
			v = ph == Phase::Ack || ph == Phase::Loop ?
				(test_.asked() ? 4 : 3) : 1;
		} else if (nm == "+TRDLS") {
			v = ph == Phase::Prep || ph == Phase::WaitAck ||
				ph == Phase::Settle || ph == Phase::Test ||
				ph == Phase::Term || ph == Phase::TermOnes ? 4 : 1;
		} else if (nm == "+TRES") {
			v = tres_;
		} else {
			const auto &b = test_.ber();
			line("+TNUM: " + dec(std::min<u32>(b.bit_errors, 65535)) +
			     "," + dec(std::min<u32>(b.block_errors, 65535)));
			return 0;
		}
		line(nm + ": " + dec(v));
		return 0;
	}

	if (nm == "+TERDL") {
		if (test) {
			line("+TERDL: (0,1)");
		} else if (read) {
			line(std::string("+TERDL: ") + (test_.grant() ? "1" : "0"));
		} else if (set) {
			u32 v;
			if (!t_number(q + 1, &q, 1, &v))
				return -1;
			*len = static_cast<s16>(q - p);
			test_.set_grant(v == 1);
		} else {
			return -1;
		}
		return 0;
	}

	if (nm == "+TSELF") {
		if (test) {
			line("+TSELF: (0,1)");
			return 0;
		}
		u32 v;
		if (!set || !t_number(q + 1, &q, 1, &v))
			return -1;
		*len = static_cast<s16>(q - p);
		/* the full test is intrusive: not with a call up */
		if (v == 0 && state_ != IDLE)
			return -1;
		tres_ = v54::self_check() ? 1 : 2;
		logf("self-test: %s\n", tres_ == 1 ? "pass" : "fail");
		return 0;
	}

	if (nm == "+TTER") {
		const auto &b = test_.ber();
		if (test) {
			line("+TTER: (0-3),(1-65535),(1-65535),(1)");
			return 0;
		}
		if (read) {
			line("+TTER: " + dec(b.on ? b.type : 0) + "," +
			     dec(b.block_len) + "," + dec(b.on ? b.left : 0) +
			     ",1");
			return 0;
		}
		if (!set)
			return -1;
		u32 type, block_len = 0, blocks = 0, pattern = 1;
		if (!t_number(q + 1, &q, 3, &type))
			return -1;
		if (type) {
			if (*q != ',' || !t_number(q + 1, &q, 65535, &block_len) ||
			    *q != ',' || !t_number(q + 1, &q, 65535, &blocks) ||
			    !block_len || !blocks)
				return -1;
			if (*q == ',' && !t_number(q + 1, &q, 4, &pattern))
				return -1;
		}
		*len = static_cast<s16>(q - p);
		if (!type) {
			test_.ber_stop();
			return 0;
		}
		/* V.52's 511-bit pattern alone; a loop of ours, from online
		   command state */
		if (pattern != 1 || !online_cmd || !test_.loop_up())
			return -1;
		test_.ber_start(static_cast<u8>(type), block_len, blocks);
		logf("error rate test: %" PRIu32 " blocks of %" PRIu32 " bits\n",
		     blocks, block_len);
		return 0;
	}

	/* the loops: +TLDL, +TRDL, +TAL */
	const bool ldl = nm == "+TLDL", rdl = nm == "+TRDL";
	const bool local2 = (ph == Phase::Ack || ph == Phase::Loop) &&
		!test_.asked();
	const bool remote2 = ph == Phase::Prep || ph == Phase::WaitAck ||
		ph == Phase::Settle || ph == Phase::Test ||
		ph == Phase::Term || ph == Phase::TermOnes;
	if (test) {
		line(nm + (ldl || rdl ? ": (0,1)" : ": (0,1),(0,1)"));
		return 0;
	}
	if (read) {
		bool on = ldl ? local2 : rdl ? remote2 : loop3_;
		line(nm + ": " + (on ? "1" : "0"));
		return 0;
	}
	u32 v, band = 0;
	if (!set || !t_number(q + 1, &q, 1, &v))
		return -1;
	if (!ldl && !rdl && *q == ',' && !t_number(q + 1, &q, 1, &band))
		return -1;
	*len = static_cast<s16>(q - p);
	if (ldl) {
		if (!v) {
			if (local2)
				test_.end();
			return 0;
		}
		if (!online_cmd || !link_plain() || test_.active())
			return -1;
		logf("loop 2 here\n");
		test_.local2();
		return 0;
	}
	if (rdl) {
		if (!v) {
			if (!remote2)
				return 0;
			test_.ber_stop();
			return test_end(false);
		}
		if (!online_cmd || !link_plain() || test_.active())
			return -1;
		logf("loop 2 at the far end\n");
		test_cmd_ = true;
		test_.remote(false);
		return 1;
	}
	/* +TAL (<band>: the 4-wire pumps have no bands) */
	if (!v) {
		if (!loop3_)
			return 0;
		test_.ber_stop();
		return test_end(false);
	}
	s16 r = loop3_start(false);
	if (r > 0)
		test_cmd_ = true;
	return r;
}

}  // namespace dcore
