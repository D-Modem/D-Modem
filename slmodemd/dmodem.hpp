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

inline void set_debug(int level) { dmodem_set_debug(level); }
inline void set_log(void (*fn)(const char *line)) { dmodem_set_log(fn); }

class Modem {
public:
	explicit Modem(const dmodem_config &cfg) : d_(dmodem_create(&cfg))
	{
		if (!d_)
			throw std::runtime_error("dmodem_create failed");
	}
	~Modem() { reset(); }
	Modem(Modem &&o) noexcept : d_(std::exchange(o.d_, nullptr)) {}
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

	bool answer() { return dmodem_answer(d_) == 0; }
	bool dial(const std::string &number)
	{
		return dmodem_dial(d_, number.c_str()) == 0;
	}
	void process(const short *in, short *out, int n)
	{
		dmodem_process(d_, in, out, n);
	}
	void process_g711(const unsigned char *in, unsigned char *out, int n)
	{
		dmodem_process_g711(d_, in, out, n);
	}
	/* false when the DTE side is closed */
	bool poll_dte() { return dmodem_poll_dte(d_) >= 0; }
	struct dmodem_status status() const
	{
		struct dmodem_status s;
		dmodem_status(d_, &s);
		return s;
	}
	bool online() const { return status().link == DMODEM_ONLINE; }
	void hangup() { dmodem_hangup(d_); }

	/* the C object, for what the class does not cover */
	struct dmodem *get() const { return d_; }

private:
	void reset()
	{
		if (d_)
			dmodem_destroy(d_);
		d_ = nullptr;
	}
	struct dmodem *d_;
};

class Fax {
public:
	explicit Fax(const dmodem_fax_config &cfg) : f_(dmodem_fax_create(&cfg))
	{
		if (!f_)
			throw std::runtime_error("dmodem_fax_create failed");
	}
	~Fax() { reset(); }
	Fax(Fax &&o) noexcept : f_(std::exchange(o.f_, nullptr)) {}
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

	void process(const short *in, short *out, int n)
	{
		dmodem_fax_process(f_, in, out, n);
	}
	void process_g711(const unsigned char *in, unsigned char *out, int n)
	{
		dmodem_fax_process_g711(f_, in, out, n);
	}
	struct dmodem_fax_status status() const
	{
		struct dmodem_fax_status s;
		dmodem_fax_status(f_, &s);
		return s;
	}
	void hangup() { dmodem_fax_hangup(f_); }

	struct dmodem_fax *get() const { return f_; }

private:
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
