/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    pipe_test.cpp - dmodemd.exe's named pipe DTE (Windows): dmodemd
 *                    started beside this program with --pipe (we its
 *                    client) or --pipe-connect (we make the pipe), its
 *                    AT commands answered through the pipe, its client
 *                    gone and another served.  No call: the SIP account
 *                    is one nobody answers.
 *
 *    pipe_test bridge (--pipe|--pipe-connect) <pipe> <port>: the pipe
 *    to one TCP connection on 127.0.0.1:<port>, both ways, until either
 *    ends (for asterisk/test/sip_call.py's WIN_PIPE=1): with --pipe
 *    dmodemd makes the pipe and we connect to it; with --pipe-connect
 *    we make it.
 *
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {

/* a pipe name of our own: parallel runs don't meet */
std::string pipe_name(const char *what)
{
	return "\\\\.\\pipe\\dmodem-test-" + std::string(what) + "-" +
		std::to_string(GetCurrentProcessId());
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
	/* dmodemd started with args (-n, an account nobody
	   answers) */
	explicit Daemon(const std::string &args)
	{
		std::string cmd = "\"" + dmodemd_path() + "\" -n " + args;
		STARTUPINFOA si = {};
		si.cb = sizeof(si);
		SetEnvironmentVariableA("SIP_LOGIN", "test:test@127.0.0.1:9");
		SetEnvironmentVariableA("SIP_PORT", "0");
		ok_ = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
				     0, nullptr, nullptr, &si, &pi_);
	}
	/* dmodemd killed, waited for */
	~Daemon()
	{
		if (!ok_)
			return;
		TerminateProcess(pi_.hProcess, 0);
		WaitForSingleObject(pi_.hProcess, 10000);
		CloseHandle(pi_.hProcess);
		CloseHandle(pi_.hThread);
	}
	/* it was started */
	bool ok() const { return ok_; }
	/* it is still running */
	bool running() const
	{
		return ok_ && WaitForSingleObject(pi_.hProcess, 0) ==
			WAIT_TIMEOUT;
	}

private:
	PROCESS_INFORMATION pi_ = {};
	bool ok_ = false;
};

/* an overlapped read of h for up to ms: what came ("" if nothing), or
   gone set when the pipe ended */
std::string read_some(HANDLE h, DWORD ms, bool *gone)
{
	char buf[1024];
	OVERLAPPED ov = {};
	DWORD got = 0;

	*gone = false;
	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!ReadFile(h, buf, sizeof(buf), &got, &ov)) {
		if (GetLastError() != ERROR_IO_PENDING) {
			*gone = true;
			got = 0;
		} else {
			if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0)
				CancelIoEx(h, &ov);
			if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
				if (GetLastError() != ERROR_OPERATION_ABORTED)
					*gone = true;
				got = 0;
			}
		}
	}
	CloseHandle(ov.hEvent);
	return std::string(buf, got);
}

/* s written to h (overlapped, waited for): false if not
   all of it */
bool write_all(HANDLE h, const std::string &s)
{
	OVERLAPPED ov = {};
	DWORD done = 0;
	BOOL ok;

	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	ok = WriteFile(h, s.data(), (DWORD)s.size(), &done, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
		ok = GetOverlappedResult(h, &ov, &done, TRUE);
	CloseHandle(ov.hEvent);
	return ok && done == s.size();
}

/* cmd to the modem, and what it says until want (or ERROR), within
   ms */
std::string command(HANDLE h, const std::string &cmd, const char *want,
		    DWORD ms = 5000)
{
	std::string said;
	DWORD t0 = GetTickCount();
	bool gone;

	if (!write_all(h, cmd + "\r"))
		return "(write failed)";
	while (GetTickCount() - t0 < ms) {
		said += read_some(h, 200, &gone);
		if (said.find(want) != std::string::npos ||
		    said.find("ERROR") != std::string::npos || gone)
			break;
	}
	return said;
}

/* the client end of name, as soon as it is there (within ms) */
HANDLE open_client(const std::string &name, DWORD ms = 30000)
{
	DWORD t0 = GetTickCount();
	while (GetTickCount() - t0 < ms) {
		HANDLE h = CreateFileA(name.c_str(),
				       GENERIC_READ | GENERIC_WRITE, 0,
				       nullptr, OPEN_EXISTING,
				       FILE_FLAG_OVERLAPPED, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			return h;
		Sleep(100);
	}
	return INVALID_HANDLE_VALUE;
}

/* a pipe of ours whose client (within ms) is dmodemd */
HANDLE serve_pipe(const std::string &name, DWORD ms = 30000)
{
	OVERLAPPED ov = {};
	HANDLE h = CreateNamedPipeA(name.c_str(),
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
		4096, 4096, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return h;
	ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	BOOL ok = ConnectNamedPipe(h, &ov);
	DWORD e = ok ? 0 : GetLastError(), n;
	if (!ok && e == ERROR_IO_PENDING) {
		if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0)
			CancelIoEx(h, &ov);
		ok = GetOverlappedResult(h, &ov, &n, TRUE);
	} else if (e == ERROR_PIPE_CONNECTED) {
		ok = TRUE;
	}
	CloseHandle(ov.hEvent);
	if (!ok) {
		CloseHandle(h);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

/* the modem answers through h */
void talk(HANDLE h)
{
	std::string s = command(h, "ATZ", "OK\r\n");
	EXPECT_NE(s.find("OK\r\n"), std::string::npos) << s;
	s = command(h, "ATI", "OK\r\n");
	EXPECT_NE(s.find("D-Modem"), std::string::npos) << s;
	EXPECT_NE(s.find("OK\r\n"), std::string::npos) << s;
}

/* the pipe to a TCP connection both ways until either ends */
int bridge(bool made_by_dmodemd, const char *name, const char *port)
{
	WSADATA wsa;
	SOCKET l, c;
	struct sockaddr_in in = {};
	HANDLE p;

	WSAStartup(MAKEWORD(2, 2), &wsa);
	l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	in.sin_port = htons((u_short)atoi(port));
	if (bind(l, (struct sockaddr *)&in, sizeof(in)) || listen(l, 1)) {
		fprintf(stderr, "bridge: can't listen on %s\n", port);
		return 1;
	}
	printf("bridge: listening on 127.0.0.1:%s\n", port);
	fflush(stdout);
	c = accept(l, nullptr, nullptr);
	closesocket(l);
	p = made_by_dmodemd ? open_client(name) : serve_pipe(name, INFINITE);
	if (p == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "bridge: no pipe %s\n", name);
		return 1;
	}
	printf("bridge: connected\n");
	fflush(stdout);
	/* TCP to the pipe in a thread, the pipe to TCP here */
	std::atomic<bool> done{false};
	std::thread up([&] {
		char buf[4096];
		int n;
		while ((n = recv(c, buf, sizeof(buf), 0)) > 0)
			if (!write_all(p, std::string(buf, n)))
				break;
		done = true;
	});
	while (!done) {
		bool gone;
		std::string s = read_some(p, 200, &gone);
		if (gone)
			break;
		if (!s.empty() &&
		    send(c, s.data(), (int)s.size(), 0) != (int)s.size())
			break;
	}
	shutdown(c, SD_BOTH);
	up.join();
	CloseHandle(p);
	closesocket(c);
	return 0;
}

} // namespace

/* --pipe: dmodemd makes the pipe; a client, then another */
TEST(PipeDte, Made)
{
	std::string name = pipe_name("made");
	Daemon d("--pipe " + name + " pipetest-made");
	ASSERT_TRUE(d.ok());
	for (int i = 0; i < 2; i++) {
		HANDLE h = open_client(name);
		ASSERT_NE(h, INVALID_HANDLE_VALUE) << "client " << i;
		talk(h);
		CloseHandle(h);
	}
	EXPECT_TRUE(d.running());
}

/* --pipe-connect: we make the pipe, dmodemd connects; we close it and
   make it again, and dmodemd comes back */
TEST(PipeDte, Connected)
{
	std::string name = pipe_name("connect");
	Daemon d("--pipe-connect " + name + " pipetest-connect");
	ASSERT_TRUE(d.ok());
	for (int i = 0; i < 2; i++) {
		HANDLE h = serve_pipe(name);
		ASSERT_NE(h, INVALID_HANDLE_VALUE) << "pipe " << i;
		talk(h);
		DisconnectNamedPipe(h);
		CloseHandle(h);
	}
	EXPECT_TRUE(d.running());
}

/* --pipe on a name that is taken: dmodemd says so and ends */
TEST(PipeDte, NameTaken)
{
	std::string name = pipe_name("taken");
	HANDLE h = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
				    PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr);
	ASSERT_NE(h, INVALID_HANDLE_VALUE);
	Daemon d("--pipe " + name + " pipetest-taken");
	ASSERT_TRUE(d.ok());
	DWORD t0 = GetTickCount();
	while (d.running() && GetTickCount() - t0 < 20000)
		Sleep(100);
	EXPECT_FALSE(d.running());
	CloseHandle(h);
}

/* "bridge" (see above), else the tests */
int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "bridge") &&
	    (!strcmp(argv[2], "--pipe") || !strcmp(argv[2], "--pipe-connect")))
		return bridge(!strcmp(argv[2], "--pipe"), argv[3], argv[4]);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
