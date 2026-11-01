/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vmodem_test.cpp - dmodemd.exe on the virtual modem driver's pipe
 *                      (Windows; Wine in the tests): the driver's own end
 *                      of it (link.h, port.h) run here, with no driver
 *                      around it, and dmodemd started beside this program
 *                      with --driver.  As an app on the port: the modem's
 *                      DSR and CTS once dmodemd is there, AT commands
 *                      answered, dmodemd killed seen as DSR's going
 *                      (EV_DSR), the driver going and coming back, and
 *                      --driver auto finding the pipe.  No call: the SIP
 *                      account is one nobody answers.
 *
 *    vmodem_test bridge <port> <tcp>: the driver's end for port (COM-like
 *    name), dmodemd waited for on it, then the app's side on one TCP
 *    connection on 127.0.0.1:<tcp>
 *    (the port opened when it comes, closed when it goes), for
 *    frontend/asterisk/test/sip_call.py's WIN_DRIVER=1; the modem's DCD printed
 *    as it changes ("bridge: dcd on").
 *
 */

#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "link.h"
#include "port.h"

using namespace vmodem;

namespace {

/* a port name of our own: parallel runs don't meet */
std::string port_name(const char *what)
{
	return std::string("TEST") + what + std::to_string(GetCurrentProcessId());
}

/* dmodemd.exe, beside this program */
std::string dmodemd_path()
{
	char me[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, me, sizeof(me));
	std::string s(me, n);
	size_t slash = s.find_last_of("\\/");
	return s.substr(0, slash + 1) + "dmodemd.exe";
}

/* dmodemd with args, until the end of the test */
class Daemon {
public:
	/* dmodemd started with args (-n, an account nobody answers) */
	explicit Daemon(const std::string &args)
	{
		std::string cmd = "\"" + dmodemd_path() + "\" -n -d1 " + args;
		STARTUPINFOA si = {};
		si.cb = sizeof(si);
		SetEnvironmentVariableA("SIP_LOGIN", "test:test@127.0.0.1:9");
		SetEnvironmentVariableA("SIP_PORT", "0");
		ok_ = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
				     0, nullptr, nullptr, &si, &pi_);
	}
	/* dmodemd killed, waited for */
	~Daemon() { kill(); }
	/* see ~Daemon */
	void kill()
	{
		if (!ok_)
			return;
		TerminateProcess(pi_.hProcess, 0);
		WaitForSingleObject(pi_.hProcess, 10000);
		CloseHandle(pi_.hProcess);
		CloseHandle(pi_.hThread);
		ok_ = false;
	}
	/* it was started */
	bool ok() const { return ok_; }

private:
	PROCESS_INFORMATION pi_ = {};
	bool ok_ = false;
};

/* an app's request, waited for */
class Req : public Request {
public:
	/* n bytes of room */
	explicit Req(size_t n) : buf(n) {}
	/* s to write */
	explicit Req(const std::string &s) : buf(s.begin(), s.end()) {}
	/* (Request's) */
	uint8_t *data() override { return buf.data(); }
	/* (Request's) */
	size_t size() const override { return buf.size(); }
	/* (Request's: kept, and the waiter woken) */
	void complete(uint32_t st, size_t n) override
	{
		std::lock_guard<std::mutex> lk(mu);
		status = st;
		info = n;
		done = true;
		cv.notify_all();
	}
	/* completed within ms */
	bool wait(DWORD ms)
	{
		std::unique_lock<std::mutex> lk(mu);
		return cv.wait_for(lk, std::chrono::milliseconds(ms),
				   [this] { return done; });
	}

	std::vector<uint8_t> buf;
	std::mutex mu;
	std::condition_variable cv;
	bool done = false;
	uint32_t status = 0;
	size_t info = 0;
};

/* the app on a port: reads that give the first characters within a
   time, as PuTTY's and Unimodem's do */
class App {
public:
	/* the port opened, with those reads */
	explicit App(Port &p) : port_(p)
	{
		port_.open();
		SerialTimeouts t = { 0xffffffff, 0xffffffff, 200, 0, 1000 };
		size_t info;
		port_.ioctl(IO_SET_TIMEOUTS, &t, sizeof(t), nullptr, 0, &info,
			    nullptr);
	}
	/* (the port closed) */
	~App() { port_.close(); }
	/* what came within 200 ms ("" if nothing) */
	std::string read_some()
	{
		Req r(1024);
		port_.read(&r);
		if (!r.wait(5000)) {
			port_.cancel(&r);
			return "";
		}
		return std::string(r.buf.begin(), r.buf.begin() + r.info);
	}
	/* s written */
	bool write(const std::string &s)
	{
		Req w(s);
		port_.write(&w);
		return w.wait(5000) && w.info == s.size();
	}
	/* cmd to the modem, and what it says until want (or ERROR) within
	   ms */
	std::string command(const std::string &cmd, const char *want,
			    DWORD ms = 10000)
	{
		std::string said;
		DWORD t0 = GetTickCount();
		if (!write(cmd + "\r"))
			return "(write failed)";
		while (GetTickCount() - t0 < ms) {
			said += read_some();
			if (said.find(want) != std::string::npos ||
			    said.find("ERROR") != std::string::npos)
				break;
		}
		return said;
	}
	/* the modem status register */
	uint32_t status()
	{
		uint32_t s = 0;
		size_t info;
		port_.ioctl(IO_GET_MODEMSTATUS, nullptr, 0, &s, 4, &info,
			    nullptr);
		return s;
	}

private:
	Port &port_;
};

/* until ok() or ms gone: whether ok() */
template <class F> bool within(DWORD ms, F ok)
{
	DWORD t0 = GetTickCount();
	while (!ok()) {
		if (GetTickCount() - t0 > ms)
			return false;
		Sleep(50);
	}
	return true;
}

/* (messages from the link, for the test's log) */
void say(const char *msg)
{
	printf("link: %s\n", msg);
	fflush(stdout);
}

/* the driver's end for name, and dmodemd on it with dte (--driver
   name, or auto) */
struct Rig {
	/* (see the struct) */
	explicit Rig(const std::string &name, const char *dte = nullptr)
		: link(say), daemon("--driver " + std::string(dte ? dte :
							       name.c_str()))
	{
		std::string why;
		started = link.start(name, &why);
		EXPECT_TRUE(started) << why;
	}
	Link link;
	Daemon daemon;
	bool started = false;
};

/* the modem's lines and its AT commands through the port */
TEST(VirtualModem, AtCommands)
{
	Rig s(port_name("A"));
	ASSERT_TRUE(s.daemon.ok());
	ASSERT_TRUE(within(60000, [&] { return s.link.connected(); }));
	App app(s.link.port());
	ASSERT_TRUE(within(5000, [&] {
		return (app.status() & (PMSR_DSR | PMSR_CTS)) ==
			(PMSR_DSR | PMSR_CTS);
	}));
	EXPECT_FALSE(app.status() & PMSR_DCD);
	std::string r = app.command("ATZ", "OK\r\n");
	EXPECT_NE(r.find("OK\r\n"), std::string::npos) << r;
	r = app.command("ATI", "OK\r\n");
	EXPECT_NE(r.find("D-Modem"), std::string::npos) << r;
	/* Unimodem's init string and settings, from dmodem.inf (its %%C
	   is %C) */
	for (const char *cmd : { "AT&FE0V1&C1&D2S0=0", "ATS7=60X4&H1\\N3%C3",
				 "ATX3&H0\\N0%C0", "AT&H2\\N4", "ATS0=0" }) {
		r = app.command(cmd, "OK\r\n");
		EXPECT_NE(r.find("OK\r\n"), std::string::npos) << cmd << ": "
								  << r;
	}
}

/* dmodemd gone: its lines off (EV_DSR), the app's characters dropped */
TEST(VirtualModem, DmodemdGoes)
{
	Rig s(port_name("G"));
	ASSERT_TRUE(within(60000, [&] { return s.link.connected(); }));
	App app(s.link.port());
	ASSERT_TRUE(within(5000, [&] { return app.status() & PMSR_DSR; }));
	uint32_t m = PEV_DSR;
	size_t info;
	ASSERT_EQ(s.link.port().ioctl(IO_SET_WAIT_MASK, &m, 4, nullptr, 0,
				      &info, nullptr), ST_SUCCESS);
	Req ev(4);
	ASSERT_EQ(s.link.port().ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev.data(),
				      4, &info, &ev), ST_PENDING);
	s.daemon.kill();
	ASSERT_TRUE(ev.wait(10000));
	uint32_t got;
	memcpy(&got, ev.data(), 4);
	EXPECT_EQ(got, (uint32_t)PEV_DSR);
	EXPECT_FALSE(s.link.connected());
	EXPECT_FALSE(app.status() & (PMSR_DSR | PMSR_CTS));
	EXPECT_TRUE(app.write("ATZ\r"));
}

/* the driver gone (the device disabled) and back: dmodemd on it again */
TEST(VirtualModem, DriverComesBack)
{
	std::string name = port_name("B");
	Rig s(name);
	ASSERT_TRUE(within(60000, [&] { return s.link.connected(); }));
	s.link.stop();
	Link again(say);
	std::string why;
	ASSERT_TRUE(again.start(name, &why)) << why;
	ASSERT_TRUE(within(20000, [&] { return again.connected(); }));
	App app(again.port());
	std::string r = app.command("AT", "OK\r\n");
	EXPECT_NE(r.find("OK\r\n"), std::string::npos) << r;
}

/* --driver auto: the first dmodem-vm-* pipe there is */
TEST(VirtualModem, Auto)
{
	/* (a COM port's name: Windows that lists no pipes, Wine, has
	   COM1 to COM32's tried) */
	Rig s("COM" + std::to_string(10 + GetCurrentProcessId() % 20),
	      "auto");
	ASSERT_TRUE(within(60000, [&] { return s.link.connected(); }));
	App app(s.link.port());
	std::string r = app.command("AT", "OK\r\n");
	EXPECT_NE(r.find("OK\r\n"), std::string::npos) << r;
}

/* a second driver's port of the same name: refused */
TEST(VirtualModem, NameTaken)
{
	std::string name = port_name("T");
	Link a, b;
	std::string why;
	ASSERT_TRUE(a.start(name, &why)) << why;
	EXPECT_FALSE(b.start(name, &why));
	EXPECT_NE(why.find("dmodem-vm-" + name), std::string::npos) << why;
}

/* see the top: the port's app side on TCP until the connection ends */
int bridge(const char *name, const char *tcp)
{
	WSADATA wsa;
	struct sockaddr_in in = {};
	Link link(say);
	std::string why;

	WSAStartup(MAKEWORD(2, 2), &wsa);
	if (!link.start(name, &why)) {
		fprintf(stderr, "bridge: %s\n", why.c_str());
		return 1;
	}
	/* (dmodemd there first: a port without it drops what is written,
	   the test's first commands too) */
	if (!within(60000, [&] { return link.connected(); })) {
		fprintf(stderr, "bridge: no dmodemd on %s\n",
			link.pipe_name().c_str());
		return 1;
	}
	SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	in.sin_port = htons((u_short)atoi(tcp));
	if (bind(l, (struct sockaddr *)&in, sizeof(in)) || listen(l, 1)) {
		fprintf(stderr, "bridge: can't listen on %s\n", tcp);
		return 1;
	}
	printf("bridge: listening on 127.0.0.1:%s\n", tcp);
	fflush(stdout);
	SOCKET c = accept(l, nullptr, nullptr);
	closesocket(l);
	printf("bridge: connected\n");
	fflush(stdout);
	std::atomic<bool> done{false};
	{
		App app(link.port());
		/* TCP to the port in a thread, the port to TCP here */
		std::thread up([&] {
			char buf[4096];
			int n;
			while ((n = recv(c, buf, sizeof(buf), 0)) > 0)
				if (!app.write(std::string(buf, n)))
					break;
			done = true;
		});
		bool dcd = false;
		while (!done) {
			std::string s = app.read_some();
			bool now = (app.status() & PMSR_DCD) != 0;
			if (now != dcd) {
				dcd = now;
				printf("bridge: dcd %s\n", dcd ? "on" : "off");
				fflush(stdout);
			}
			if (!s.empty() && send(c, s.data(), (int)s.size(), 0) !=
			    (int)s.size())
				break;
		}
		shutdown(c, SD_BOTH);
		up.join();
	}
	closesocket(c);
	return 0;
}

} // namespace

/* "bridge" (see above), else the tests */
int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "bridge"))
		return bridge(argv[2], argv[3]);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
