/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    zyxel_sec.h - ZyXEL's type 2 security (AT*G3 to AT*G5): the
 *                  answerer asks the remote for a password before the
 *                  data passes, and can call back
 *
 *    From the U-1496S 5.06B firmware (docs/zyxel-security.md).  After CONNECT the answerer sends "PassWord:"
 *    to the remote and echoes '*' for each character.  The password is
 *    1 to 8 characters and S3 (CR) ends it.  It must be one of the 10
 *    user passwords (AT*H0= to AT*H9=), with case.  A wrong password
 *    sends "\r\nError..., try again\r\n" and asks again; the third
 *    wrong one hangs up.  Then:
 *      *G3  "\r\nCorrect\r\n", and the data passes
 *      *G4  "\r\nCorrect\r\n", a hang-up, and a call to the stored
 *           number of the password's slot (AT&Zn=)
 *      *G5  "\r\nCall back phone number:", up to 26 characters (S5
 *           erases one), S3 ends them; a hang-up and a call to them
 *
 *    The class has no I/O: the modem gives it the remote's characters
 *    (feed()) and sends what it puts in out.
 *
 */
#ifndef CORE_ZYXEL_SEC_H
#define CORE_ZYXEL_SEC_H

#include <string>

#include "dm_types.h"

namespace dcore {

struct ZyxelSecurity {
	static constexpr s16 SLOTS = 10;           /* *H0 to *H9 */
	static constexpr size_t PW_MAX = 8;        /* characters a password */
	static constexpr size_t NUMBER_MAX = 26;   /* a call-back number's */
	static constexpr s16 TRIES = 3;

	enum class Step { Off, Password, Number, Done };
	enum class End { None, Pass, Fail, CallStored, CallNumber };

	/* AT*G: 0 to 5 (1 and 2, type 1, are ZyXEL to ZyXEL only and not
	   in D-Modem) */
	s16 type = 0;
	/* AT*Hn=: the user passwords; AT*HS=: the supervisory one (AT*V
	   and changes to the table need it, when it is set) */
	std::string user[SLOTS];
	std::string super;

	/* the call's: its step, what the remote must get, how it ends,
	   the slot of the password, the number to call back */
	Step step = Step::Off;
	std::string out;
	End end = End::None;
	s16 slot = -1;
	std::string number;

	/* type 2 runs on this answered call */
	bool on() const { return type >= 3 && type <= 5; }
	/* the data waits (the remote's and the DTE's) */
	bool holds() const { return step != Step::Off; }

	/* the call's start: the prompt */
	void start()
	{
		tries_ = 0;
		end = End::None;
		slot = -1;
		number.clear();
		out.clear();
		prompt();
	}

	/* the call is over */
	void stop()
	{
		step = Step::Off;
		out.clear();
		buf_.clear();
	}

	/* one of the remote's characters; cr and bs are S3 and S5 */
	void feed(u8 c, u8 cr, u8 bs)
	{
		if (step == Step::Password)
			password_char(c, cr);
		else if (step == Step::Number)
			number_char(c, cr, bs);
	}

	/* the slot whose password is pw, or -1 (an empty slot is none) */
	s16 match(const std::string &pw) const
	{
		for (s16 i = 0; i < SLOTS; i++)
			if (!user[i].empty() && user[i] == pw)
				return i;
		return -1;
	}

private:
	s16 tries_ = 0;
	std::string buf_;

	void prompt()
	{
		out += "PassWord:";
		buf_.clear();
		step = Step::Password;
	}

	/* a wrong password, or a number too long: again, or the end */
	void wrong()
	{
		if (++tries_ >= TRIES) {
			end = End::Fail;
			step = Step::Done;
			return;
		}
		out += "\r\nError..., try again\r\n";
		prompt();
	}

	void password_char(u8 c, u8 cr)
	{
		if (c != cr) {
			if (buf_.size() == PW_MAX) {
				wrong();
				return;
			}
			buf_ += static_cast<char>(c);
			out += '*';
			return;
		}
		slot = match(buf_);
		buf_.clear();
		if (slot < 0) {
			wrong();
			return;
		}
		if (type == 5) {
			out += "\r\nCall back phone number:";
			step = Step::Number;
			return;
		}
		out += "\r\nCorrect\r\n";
		end = type == 4 ? End::CallStored : End::Pass;
		step = Step::Done;
	}

	void number_char(u8 c, u8 cr, u8 bs)
	{
		if (c == bs) {
			if (!buf_.empty()) {
				buf_.pop_back();
				out += static_cast<char>(bs);
			}
			return;
		}
		if (c == cr) {
			number = buf_;
			buf_.clear();
			end = End::CallNumber;
			step = Step::Done;
			return;
		}
		if (buf_.size() == NUMBER_MAX) {
			wrong();
			return;
		}
		buf_ += static_cast<char>(c);
		out += static_cast<char>(c);
	}
};

} // namespace dcore

#endif /* CORE_ZYXEL_SEC_H */
