/*
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

/* a struct dmodem: the modem of one call at a time */
class Modem {
public:
	/* throws std::runtime_error if dmodem_create() fails */
	explicit Modem(const dmodem_config &cfg) : d_(dmodem_create(&cfg))
	{
		if (!d_)
			throw std::runtime_error("dmodem_create failed");
	}
	/* dmodem_destroy(): a call still on is hung up */
	~Modem() { reset(); }
	/* the moved-from object is left empty */
	Modem(Modem &&o) noexcept : d_(std::exchange(o.d_, nullptr)) {}
	/* our own modem destroyed first */
	Modem &operator=(Modem &&o) noexcept
	{
		if (this != &o) {
			reset();
			d_ = std::exchange(o.d_, nullptr);
		}
		return *this;
	}
	Modem(const Modem &) = delete;
	Modem &operator=(const Modem &) = delete;

	/* ATA now; false if the modem can't (a call is on) */
	bool answer() { return dmodem_answer(d_) == 0; }
	/* ATD<number> as a command; true (how it goes: the link's state) */
	bool dial(const std::string &number)
	{
		return dmodem_dial(d_, number.c_str()) == 0;
	}
	/* n samples in, n out (dmodem_process) */
	void process(const short *in, short *out, int n)
	{
		dmodem_process(d_, in, out, n);
	}
	/* n G.711 octets in, n out */
	void process_g711(const unsigned char *in, unsigned char *out, int n)
	{
		dmodem_process_g711(d_, in, out, n);
	}
	/* false when the DTE side is closed */
	bool poll_dte() { return dmodem_poll_dte(d_) >= 0; }
	/* dmodem_status() */
	struct dmodem_status status() const
	{
		struct dmodem_status s;
		dmodem_status(d_, &s);
		return s;
	}
	/* the modems are connected */
	bool online() const { return status().link == DMODEM_ONLINE; }
	/* ATH */
	void hangup() { dmodem_hangup(d_); }

	/* the C object, for what the class does not cover */
	struct dmodem *get() const { return d_; }

private:
	/* the C object destroyed, if any */
	void reset()
	{
		if (d_)
			dmodem_destroy(d_);
		d_ = nullptr;
	}
	struct dmodem *d_;
};

/* a struct dmodem_fax: the fax machine of one call */
class Fax {
public:
	/* throws std::runtime_error if dmodem_fax_create() fails (the file to
	   send can't be read) */
	explicit Fax(const dmodem_fax_config &cfg) : f_(dmodem_fax_create(&cfg))
	{
		if (!f_)
			throw std::runtime_error("dmodem_fax_create failed");
	}
	/* dmodem_fax_destroy(): the received file closed */
	~Fax() { reset(); }
	/* the moved-from object is left empty */
	Fax(Fax &&o) noexcept : f_(std::exchange(o.f_, nullptr)) {}
	/* our own fax machine destroyed first */
	Fax &operator=(Fax &&o) noexcept
	{
		if (this != &o) {
			reset();
			f_ = std::exchange(o.f_, nullptr);
		}
		return *this;
	}
	Fax(const Fax &) = delete;
	Fax &operator=(const Fax &) = delete;

	/* n samples in, n out (dmodem_fax_process) */
	void process(const short *in, short *out, int n)
	{
		dmodem_fax_process(f_, in, out, n);
	}
	/* n G.711 octets in, n out */
	void process_g711(const unsigned char *in, unsigned char *out, int n)
	{
		dmodem_fax_process_g711(f_, in, out, n);
	}
	/* dmodem_fax_status() */
	struct dmodem_fax_status status() const
	{
		struct dmodem_fax_status s;
		dmodem_fax_status(f_, &s);
		return s;
	}
	/* the call went away */
	void hangup() { dmodem_fax_hangup(f_); }

	/* the C object */
	struct dmodem_fax *get() const { return f_; }

private:
	/* the C object destroyed, if any */
	void reset()
	{
		if (f_)
			dmodem_fax_destroy(f_);
		f_ = nullptr;
	}
	struct dmodem_fax *f_;
};

} /* namespace dm */

#endif
