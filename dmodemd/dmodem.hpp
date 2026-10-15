/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dmodem.hpp - libdmodem for C++ (namespace dm): the modem and the
 *                 fax machine as classes that own their C objects (see
 *                 dmodem.h for what each call does)
 *
 *    A constructor that cannot create its object throws
 *    std::runtime_error.  The objects move, they do not copy.
 *
 */

#ifndef DMODEM_HPP
#define DMODEM_HPP

#include <stdexcept>
#include <string>
#include <utility>

#include "dmodem.h"

namespace dm {

/* once per process, before the first modem */
inline void init()
{
	if (dmodem_init())
		throw std::runtime_error("dmodem_init failed");
}

/* the log's level (dmodem_set_debug) */
inline void set_debug(int level) { dmodem_set_debug(level); }
/* where the log lines go (dmodem_set_log) */
inline void set_log(void (*fn)(const char *line)) { dmodem_set_log(fn); }

/* the owner of a C object made by a create call (Modem, Fax): destroy()
   once, at the end or when another is moved in; it moves, it does not
   copy */
template <class T, void (*destroy)(T *)>
class Owner {
public:
	/* the moved-from object is left empty */
	Owner(Owner &&o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
	/* our own object destroyed first */
	Owner &operator=(Owner &&o) noexcept
	{
		if (this != &o) {
			reset();
			p_ = std::exchange(o.p_, nullptr);
		}
		return *this;
	}
	Owner(const Owner &) = delete;
	Owner &operator=(const Owner &) = delete;

	/* the C object, for what the class does not cover */
	T *get() const { return p_; }

protected:
	/* p from the create call what: std::runtime_error if it failed */
	Owner(T *p, const char *what) : p_(p)
	{
		if (!p_)
			throw std::runtime_error(std::string(what) + " failed");
	}
	~Owner() { reset(); }

	T *p_;

private:
	/* the C object destroyed, if any */
	void reset()
	{
		if (p_)
			destroy(p_);
		p_ = nullptr;
	}
};

/* a struct dmodem: the modem of one call at a time; dmodem_destroy()
   at the end hangs up a call still on */
class Modem : public Owner<struct dmodem, dmodem_destroy> {
public:
	/* throws std::runtime_error if dmodem_create() fails */
	explicit Modem(const dmodem_config &cfg)
		: Owner(dmodem_create(&cfg), "dmodem_create") {}

	/* ATA now; false if the modem can't (a call is on) */
	bool answer() { return dmodem_answer(p_) == 0; }
	/* ATD<number> as a command; true (how it goes: the link's state) */
	bool dial(const std::string &number)
	{
		return dmodem_dial(p_, number.c_str()) == 0;
	}
	/* a command line as the DTE's (dmodem_command); false if it is too
	   long */
	bool command(const std::string &line)
	{
		return dmodem_command(p_, line.c_str()) == 0;
	}
	/* n samples in, n out (dmodem_process) */
	void process(const short *in, short *out, int n)
	{
		dmodem_process(p_, in, out, n);
	}
	/* n G.711 octets in, n out */
	void process_g711(const unsigned char *in, unsigned char *out, int n)
	{
		dmodem_process_g711(p_, in, out, n);
	}
	/* false when the DTE side is closed */
	bool poll_dte() { return dmodem_poll_dte(p_) >= 0; }
	/* dmodem_status() */
	struct dmodem_status status() const
	{
		struct dmodem_status s;
		dmodem_status(p_, &s);
		return s;
	}
	/* the modems are connected */
	bool online() const { return status().link == DMODEM_ONLINE; }
	/* ATH */
	void hangup() { dmodem_hangup(p_); }
};

/* a struct dmodem_fax: the fax machine of one call; dmodem_fax_destroy()
   at the end closes the received file */
class Fax : public Owner<struct dmodem_fax, dmodem_fax_destroy> {
public:
	/* throws std::runtime_error if dmodem_fax_create() fails (the file to
	   send can't be read) */
	explicit Fax(const dmodem_fax_config &cfg)
		: Owner(dmodem_fax_create(&cfg), "dmodem_fax_create") {}

	/* n samples in, n out (dmodem_fax_process) */
	void process(const short *in, short *out, int n)
	{
		dmodem_fax_process(p_, in, out, n);
	}
	/* n G.711 octets in, n out */
	void process_g711(const unsigned char *in, unsigned char *out, int n)
	{
		dmodem_fax_process_g711(p_, in, out, n);
	}
	/* dmodem_fax_status() */
	struct dmodem_fax_status status() const
	{
		struct dmodem_fax_status s;
		dmodem_fax_status(p_, &s);
		return s;
	}
	/* the call went away */
	void hangup() { dmodem_fax_hangup(p_); }
};

} /* namespace dm */

#endif
