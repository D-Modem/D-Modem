/*
 *    dmodem_cxx.cpp - libdmodem through dmodem.hpp: two modems back to
 *    back (the caller dials, the answerer answers), a line of text each
 *    way through their DTE socketpairs.
 *
 *    dmodem_cxx [AT+MS=132] [seconds]
 */

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "dmodem.hpp"

namespace {

constexpr int frame = 160;      // 20 ms

struct Dte {
	int fd[2];
	Dte()
	{
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			fcntl(f, F_SETFL, fcntl(f, F_GETFL) | O_NONBLOCK);
	}
	~Dte()
	{
		close(fd[0]);
		close(fd[1]);
	}
	Dte(const Dte &) = delete;
	Dte &operator=(const Dte &) = delete;
	int modem_end() const { return fd[0]; }
	std::string read_all()
	{
		std::string s;
		char buf[256];
		ssize_t n;
		while ((n = read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, n);
		return s;
	}
	void write_str(const std::string &s)
	{
		if (write(fd[1], s.data(), s.size()) !=
		    static_cast<ssize_t>(s.size()))
			throw std::runtime_error("DTE write");
	}
};

dm::Modem make(const char *name, Dte &dte, const std::string &init)
{
	dmodem_config c{};
	c.name = name;
	c.dte_fd = dte.modem_end();
	c.law = DMODEM_LAW_ULAW;
	c.init = init.c_str();
	return dm::Modem(c);
}

} // namespace

int main(int argc, char **argv)
{
	const std::string ms = argc > 1 ? argv[1] : "+MS=132";
	const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;
	const std::string msg = "The quick brown fox jumps over the lazy dog.\r\n";

	try {
		dm::init();
		Dte da, db;
		const std::string init = "ATE0Q1X3" + ms + "\r";
		dm::Modem a = make("caller", da, init);
		dm::Modem b = make("answerer", db, init);
		da.read_all();
		db.read_all();
		b.answer();
		a.dial("");

		std::array<short, frame> ab{}, ba{}, oa{}, ob{};
		std::string got_a, got_b;
		int online = 0;
		for (int i = 0; i < seconds * 50; i++) {
			a.process(ba.data(), oa.data(), frame);
			b.process(ab.data(), ob.data(), frame);
			ab = oa;
			ba = ob;
			a.poll_dte();
			b.poll_dte();
			if (!online && a.online() && b.online()) {
				online = i;
				std::printf("online after %.1f s, %u/%u bit/s\n",
					    i / 50.0, a.status().tx_rate,
					    a.status().rx_rate);
				da.read_all();
				db.read_all();
				da.write_str(msg);
				db.write_str(msg);
			}
			if (online) {
				got_a += da.read_all();
				got_b += db.read_all();
			}
			if (got_a == msg && got_b == msg)
				break;
		}
		const bool ok = got_a == msg && got_b == msg;
		std::printf("received %zu and %zu of %zu: %s\n", got_a.size(),
			    got_b.size(), msg.size(), ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "dmodem_cxx: %s\n", e.what());
		return 2;
	}
}
