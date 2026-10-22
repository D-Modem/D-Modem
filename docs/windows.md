# D-Modem on Windows

dmodemd builds for 64-bit Windows with MinGW-w64 or with Microsoft's compiler (MSVC).  It is the same modem as on Linux: SIP on libre, the open DSP (data up to V.34 and V.90/V.92 as the digital modem, class 1 fax, voice), and T.38.  The USB modems (Smart Link's SmartUSB56 and the pico-serial Line) are dmodemd.exe's lines too (`--usb`, on WinUSB).  Linux's pty becomes one of four DTEs: a COM port, a named pipe, D-Modem's own virtual modem driver or a TCP connection.  libdmodem (`dmodemd/dmodem.h`) and `libdmodem_sip.a` build for Windows too.

The following are POSIX-only and are not built for Windows:
- the Asterisk and FreeSWITCH modules
- dmodem-ari
- the ALSA line (`-a`), the pty options (`-g`, `-p`) and modem_test
- the interop test peers (`dsp/test`'s `half_*`)

The tests cover Windows under Wine:
- the offline tests: `ctest -L quick`, in CI
- dmodemd's calls through a private Asterisk: `asterisk/test/sip.sh` with `DMODEMD=build-win/dmodemd/dmodemd.exe` (below)

They have not run on Windows itself yet.

## Building

The build is a cross build from Linux.  On Debian or Ubuntu:

    sudo apt-get install cmake ninja-build pkg-config g++-mingw-w64-x86-64-posix
    cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
    cmake --build build-win

`cmake/mingw-w64.cmake` takes the posix-thread compilers (`x86_64-w64-mingw32-g++-posix`), which `std::thread` and `std::mutex` need.  `-DMINGW_TRIPLE=i686-w64-mingw32` targets 32-bit Windows instead (not tested).  libre and Mbed TLS (its crypto, since Windows has no OpenSSL) are fetched and built with the same toolchain.  Everything links statically: `build-win/dmodemd/dmodemd.exe` is a single file that needs no DLLs beyond Windows' own.  CI keeps each build's `dmodemd.exe` as an artifact (`dmodemd-windows-x86_64`).

With Wine installed (`wine64`), `ctest --test-dir build-win -L quick` runs the tests under it (`CMAKE_CROSSCOMPILING_EMULATOR`).  Set `WINEDEBUG=-all` for quieter logs.

### MSVC

On Windows, from a Developer Command Prompt for Visual Studio 2022 (cl.exe and the Windows SDK on the path), with CMake, Ninja and Git:

    cmake -B build-msvc -G Ninja
    cmake --build build-msvc
    ctest --test-dir build-msvc -L quick

The build:
- Defaults to RelWithDebInfo, not MSVC's usual Debug: an unoptimised modem can't keep up with a call.  `-DCMAKE_BUILD_TYPE=Debug` is still there to ask for.
- Links MSVC's static C runtime (`/MT`), into libre and Mbed TLS too, so `dmodemd.exe` needs no vcruntime DLL.
- Fills in what MSVC's C library lacks of POSIX with small headers in `compat/msvc/`, for MSVC builds only (`unistd.h`, `strings.h`, `sys/time.h`, and `dmodem_msvc.h` for `clock_gettime`, `ssize_t` and the like).
- Turns off libre's typed `re_printf` arguments (`DISABLE_RE_ARG`), whose argument count needs GNU's comma elision.
- Needs Git's bash for the tests (they run under `cmake/test_run.sh`).  CMake looks for it in `Program Files\Git`, not System32's WSL launcher.
- Expects LF line endings, which `.gitattributes` asks for even with `core.autocrlf`.

CI's `windows-msvc` job does this on GitHub's Windows runner and keeps its `dmodemd.exe` (`dmodemd-windows-msvc-x86_64`).  It has not run yet.

From Linux, `cmake/clang-cl.cmake` builds the same way with LLVM's clang-cl and lld-link: MSVC's headers, C runtime and ABI, so it stands in for cl.exe here.  Clang 19 or later is needed (MSVC's current C++ library says so), and Microsoft's C runtime and Windows SDK come from [xwin](https://github.com/Jake-Shadle/xwin).  Its `--accept-license` accepts Microsoft's Visual Studio and Windows SDK license:

    cargo install xwin --locked
    xwin --accept-license --arch x86_64 splat --output ~/.xwin
    sudo apt-get install clang-20 clang-tools-20 lld-20
    cmake -B build-msvc -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl.cmake
    cmake --build build-msvc
    WINEDEBUG=-all ctest --test-dir build-msvc -L quick

clang-cl defines no `__GNUC__`, so the few GCC attributes and builtins the code uses (printf format checks, weak symbols) are left out there as they are for cl.exe.  The build is warning-free with `-DDMODEM_WERROR=ON`.  Its `dmodemd.exe` passes the quick tests and every `sip.sh` variant above under Wine: TCP, the COM port, both pipe modes, voice.  xwin leaves out the debug C runtime unless given `--include-debug-libs`, which a Debug build needs.

## Running

Set the SIP account, then start dmodemd:

    set SIP_LOGIN=username:password@sip.example.com
    dmodemd.exe -d1

    modem `dmodem0' created. DTE is TCP 127.0.0.1:2323
    Use TCP 127.0.0.1:2323 as the modem, Ctrl+C for termination.

The options are the Linux ones, less the POSIX-only ones (`dmodemd.exe --help`):
- `-L` takes incoming calls.
- `-c` sets the country.
- `-d` is the debug level.
- `-n` keeps normal priority. Without it the process runs at high priority, with the system timer at 1 ms for the call's 20 ms frames.

The modem keeps its line data (the V.34/V.90 parameters it learns) in `%LOCALAPPDATA%\dmodem\data.<name>`.  Windows' firewall asks once whether to allow dmodemd on the network: SIP and RTP need it.

### TCP (`--tcp`, the default)

`--tcp [address:]port` listens there; the default is `127.0.0.1:2323`, and `0.0.0.0:2323` accepts other machines too.  Any program that can open a raw TCP connection can be the terminal: PuTTY (connection type Raw), `telnet 127.0.0.1 2323`, or a BBS client.  It talks AT to the modem as on a serial line.  One connection at a time is served.  When it closes, the modem hangs up, as a pty closing does on Linux, and the next connection is taken.

### COM port (`--com`)

For programs that want a modem on a COM port (dial-up networking, fax programs, terminal programs with modem support), dmodemd opens one end of a virtual null-modem pair.  [com0com](https://sourceforge.net/projects/com0com/) makes such pairs, and the program opens the other end.  For example, with com0com's `setupc`:

    setupc install PortName=COM5 PortName=COM6
    setupc change CNCB0 cts=rdtr,dcd=rrts
    dmodemd.exe --com COM5

The program then uses COM6 as its modem.
- dmodemd sets its end to 115200 8N1 with DTR on.  com0com's port speed doesn't limit anything (unless `EmuBR=yes`).
- The program dropping DTR (our DSR) hangs the modem up.
- dmodemd's RTS follows the carrier: on while a call is connected.  With `dcd=rrts` above, the program sees that as DCD.
- `cts=rdtr` keeps its CTS on.  com0com's default would give it our RTS, so CTS would be off outside a call.
- There is no hardware flow control: the modem reads from the port as it has room.

The COM port's path is tested under Wine, with a pty pair in place of com0com: `WIN_COM=1` in `asterisk/test/sip.sh` (below).  A pty has no modem control lines, so the DSR hang-up and the RTS carrier signal are not covered there.

### Named pipe (`--pipe`, `--pipe-connect`)

A named pipe is how virtual machines give a guest's COM port to a program on the host, so a guest OS (Windows 98's dial-up networking, a DOS BBS, Linux's pppd) can use dmodemd as its modem without com0com.  The name is `\\.\pipe\<name>`, or just `<name>`.  Which side makes the pipe depends on the VM:
- `--pipe <name>`: dmodemd makes the pipe and the VM connects to it.
  - Hyper-V: `Set-VMComPort -VMName <vm> -Number 1 -Path \\.\pipe\dmodem`
  - VirtualBox: Serial Port, mode Host Pipe, path `\\.\pipe\dmodem`, with "Connect to existing pipe/socket" ticked
  - VMware: "Use named pipe", with "This end is the client"
- `--pipe-connect <name>`: the VM makes the pipe and dmodemd connects to it.
  - QEMU: `-serial pipe:dmodem`
  - VirtualBox: "Connect to existing pipe/socket" unticked
  - VMware: "This end is the server"

  dmodemd tries every 500 ms until the pipe is there, so the VM can start before or after it.

One client is served at a time:
- With `--pipe`, when the client goes (the VM stops, or the program closes the pipe), the modem hangs up and dmodemd waits for the next client.
- With `--pipe-connect`, when the pipe goes, the modem hangs up and dmodemd waits for the pipe to come back.
- With `--pipe`, the name is dmodemd's alone (`FILE_FLAG_FIRST_PIPE_INSTANCE`): if another program has it, dmodemd says so and stops.
- The pipe takes local clients only.

A pipe has no control lines, so the guest sees no DCD from the modem and can't hang up by dropping DTR.  It hangs up with `+++` and `ATH`.  If the guest's dial-up stack waits for DCD, set it to ignore DCD.  No real VM has been tried yet: the VM settings above are from their documentation.

The tests cover both ways under Wine:
- `pipe_test` (in `ctest -L quick`) runs dmodemd.exe with each option, talks AT through the pipe, and checks that a client can go and another be served, and that a taken name is refused.
- `WIN_PIPE=made` or `WIN_PIPE=connect` in `asterisk/test/sip.sh` makes the test's calls over a pipe (below).

### USB modems (`--usb`)

dmodemd.exe drives Smart Link's SmartUSB56 (ST7554, USB `0483:7554`) and the pico-serial Line stick (`1209:0002`) as dmodemd does on Linux ([hardware.md](hardware.md#smart-link-smartusb56)). There is no SIP then: the line is the modem's telephone line, and the DTE is any of the others (TCP, a COM port, a pipe, the virtual modem driver's port). The same code runs both:
- the samples are isochronous transfers;
- the hook and the ring detector go through the device's own requests and its interrupt endpoint;
- the pico-serial Line gets caller ID and the country's line settings.

    dmodem-driver usb dmodem-usb.inf      (once, as an administrator)
    dmodemd.exe --usb list
    dmodemd.exe --usb auto --tcp 127.0.0.1:2323

Windows has no kernel driver to hold either device. libusb reaches a USB device through WinUSB, Windows' own generic USB driver, so the device must have WinUSB as its driver. That takes one of these:
- `dmodem-usb.inf` (`winmodem/`) gives both devices WinUSB, and `dmodem-driver usb dmodem-usb.inf` installs it. The INF must be signed before Windows takes it (the test signing of the virtual modem driver, above, with `dmodem-usb.cat`).
- [Zadig](https://zadig.akeo.ie/) does the same and signs what it installs itself: choose the device, then WinUSB.

A SmartUSB56 with Smart Link's own Windows driver installed must be moved to WinUSB first. Until then, `--usb list` says "its driver is not WinUSB".

The pico-serial Line could ask for WinUSB itself, with no INF and no Zadig. That needs Microsoft OS 2.0 descriptors (the WinUSB compatible ID) in its firmware, which it doesn't have yet.

How dmodemd.exe runs a USB line:
- **libusb:** the pinned release (1.0.27, LGPL-2.1) is fetched and built while configuring (`cmake/libusb.cmake`), for MinGW-w64, MSVC and clang-cl alike. It links into dmodemd.exe, which stays a single file. `-DDMODEM_USB=OFF` builds without it, and `-DDMODEM_LIBUSB_DIR=` takes a libusb source tree already there. WinUSB's isochronous transfers need Windows 8.1 or later.
- **Polling:** Windows' libusb has no descriptors to wait on (Linux's main loop waits on its file descriptors), so the main loop handles libusb's events every 2 ms, as well as the DTE's socket.
- **Messages:** when WinUSB isn't the device's driver, dmodemd.exe says so, and says how to make it so.

**Not yet tried on a real device under Windows.** The USB tests below run dmodemd.exe under Wine against the same fake devices as Linux's tests.

### The virtual modem driver (`--driver`)

D-Modem has a Windows driver of its own (`winmodem/`): a virtual modem that Device Manager lists under Modems as "D-Modem Virtual Modem", with dmodemd as the modem behind it.  Windows programs use it as they would a modem card:
- through Windows' modem stack (Unimodem and TAPI): dial-up networking, Windows Fax and Scan, HyperTerminal;
- or by its COM port's name, as a terminal or BBS program would.

No com0com pair is needed.

    dmodem-driver install dmodem.inf      (once, as an administrator)
    dmodemd.exe --driver auto

The pieces:
- `dmodemvm.dll` is a UMDF 2 driver (UMDF 2.15, Windows 10 and later). It runs in Windows' driver host as a user-mode driver, so its faults can't crash Windows. Unimodem's `modem.sys` sits above it. Its device is a COM port: the reads, writes and serial IOCTLs a modem card's port answers, with serial.sys's semantics (`winmodem/port.h`):
  - the serial timeouts;
  - `WaitCommEvent`'s events;
  - the modem status lines;
  - DTR and RTS, set directly or by the handflow;
  - purges;
  - `LSRMST_INSERT`'s in-band status.
- `dmodem.inf` installs it in the Modem class, with Unimodem's settings for D-Modem's commands (init `AT&FE0V1&C1&D2S0=0`, `&H` for flow control, `\N` and `%C`). It also lists every result D-Modem gives as a response, each `CONNECT` rate with it. `winmodem/make_inf.py` makes the INF from the core's table, so a new rate can't be missed (the `winmodem_inf` test checks this). The INF needs Windows 10 1803 or later, for the driver store's DIRID 13.
- `dmodem-driver.exe` makes the root device (`Root\DModemVM`) and installs the driver, as the WDK's devcon would:
  - `install` adds a device, and a modem, each time it is run;
  - `update` moves the devices to a new INF;
  - `remove` takes the devices away;
  - `list` gives each device's COM port and pipe.
- The driver makes a pipe, `\\.\pipe\dmodem-vm-<port>`, for each device (`dmodem-vm-COM3` for COM3). dmodemd connects to it with `--driver COM3`, or with `--driver auto` to the first one there is. dmodemd waits for the pipe and connects again whenever the driver comes back (the device disabled, Windows resumed). Frames on the pipe carry the characters both ways, the modem's lines to the port, and the app's DTR and RTS back (`winmodem/wire.h`).
  - Who can connect: the pipe takes local clients only, and of those the system, the administrators, the local service and the users logged on here. dmodemd can therefore run as a program or as a service.
  - One dmodemd at a time per pipe.

Between the app and dmodemd:
- The modem's DSR and CTS are on while dmodemd is on the pipe, its DCD with the carrier.
- The app dropping DTR, or closing the port, hangs the modem up, as `&D2` does on a real one.
- Without dmodemd, the port acts as a modem switched off: no DSR or CTS, and whatever the app writes is dropped.
- RI isn't driven: Unimodem takes the `RING` result.
- A break, XON/XOFF and the baud rate are taken but change nothing: the link has no speed or flow control of its own.
- The port allows one handle at a time, as a serial port does.

The driver must be signed before Windows loads it. A package of your own can be signed for a test machine, with the WDK's `Inf2Cat` and the SDK's `signtool`, in an elevated prompt:

    bcdedit /set testsigning on                (then restart)
    Inf2Cat /driver:<dir> /os:10_X64           (<dir>: dmodemvm.dll, dmodem.inf)
    New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=D-Modem test" -CertStoreLocation Cert:\LocalMachine\My
    (export it and import it into LocalMachine's Root and TrustedPublisher stores)
    signtool sign /fd sha256 /sm /s My /n "D-Modem test" <dir>\dmodemvm.cat <dir>\dmodemvm.dll
    dmodem-driver install <dir>\dmodem.inf

Driver signing for release (attestation through Microsoft's Partner Center) needs an EV certificate.

Building the driver:
- **On Windows:** it needs Visual Studio's compiler and the Windows Driver Kit. CMake finds the WDK's UMDF 2.15 headers and stub library (`DMODEM_WDF_INCLUDE`, `DMODEM_WDF_STUB`) and builds `dmodemvm.dll`; `cmake --install` puts it beside `dmodem.inf` in `driver/`.
- **On Linux with clang-cl:** there is no WDK there. Give `-DDMODEM_WDF_INCLUDE=` the UMDF 2.15 headers of Microsoft's [Windows-Driver-Frameworks](https://github.com/microsoft/Windows-Driver-Frameworks) (`src/publicinc/wdf/umdf/2.15`, MIT-licensed). The driver is then compiled against them but not linked (`dmodemvm_check`); the WDK's stub library is the one piece missing.

**Not yet tried on Windows itself.** Only the port, the pipe and dmodemd's end have run, under Wine (below). The WDF part (`driver.cpp`) has only been compiled, and the INF has not been installed.

## Testing dmodemd under Wine

`asterisk/test/sip.sh` makes calls between dmodemd and a private Asterisk.  Given a Windows build, it runs `dmodemd.exe` under Wine:

    DMODEMD=$PWD/build-win/dmodemd/dmodemd.exe WINEDEBUG=-all asterisk/test/sip.sh
    INCOMING=1 DMODEMD=... asterisk/test/sip.sh
    VOICE_DIAL=1 DMODEMD=... asterisk/test/sip.sh
    WIN_COM=1 DMODEMD=... asterisk/test/sip.sh
    WIN_PIPE=made DMODEMD=... asterisk/test/sip.sh      (or WIN_PIPE=connect)
    WIN_DRIVER=1 DMODEMD=... asterisk/test/sip.sh

The DTE is a TCP connection by default.  With `WIN_COM=1` it is Wine's COM9, linked to a pty pair whose other end the test holds.  With `WIN_PIPE` it is a named pipe, which `pipe_test.exe bridge` (beside dmodemd.exe) passes to the test over TCP.  With `WIN_DRIVER=1` it is the virtual modem driver's port (`--driver SIPTEST`): `vmodem_test.exe bridge` runs the driver's end of the pipe (the port and the pipe, everything but the WDF part) and gives the test the port's app side over TCP. The test also checks that the port's DCD came on for the call and went off after it.  Asterisk and app_dmodem come from the Linux build (`build/`), as for the Linux test.  The variants (outgoing V.91 at 62666, incoming, A-law, voice in and voice dial, and over the COM port, a pipe either way or the driver's port) pass as on Linux.

The quick tests cover the driver without Windows:
- `port_test` (Linux and Windows): the COM port.
- `vmodem_test` (Windows): dmodemd.exe on the driver's pipe, with the driver's end of it in the test. It checks:
  - AT commands and the INF's settings answered;
  - DSR going when dmodemd does;
  - the driver going and coming back;
  - `--driver auto`;
  - a second port of the same name refused.
- `dmodem_driver_list` (Windows): `dmodem-driver.exe list`, with no device installed.

The USB modems' tests run dmodemd.exe under Wine: `dmodemd_usb_windows` and `dmodemd_pico_line_windows` (label `alsa`). They are Linux's `usb_test.py` and `pico_line_test.py`. They check each device's whole set: the two modems' calls through the exchange, rings, caller ID, BUSY, pulse dialling, voice, the country's line settings and unplugging.
- **The fake devices:** `dmodemd_fake_st7554.exe` and `dmodemd_fake_pico_line.exe` are dmodemd.exe with the test's fake device linked in libusb's place (Windows has no `LD_PRELOAD`).
- **The DTE:** a TCP connection.
- **The exchange:** it is the Linux build's `line_exchange` (`DMODEM_HOST_LINE_EXCHANGE`, by default `build/dmodemd/line_exchange`), and the tests run only when it is there. Wine's Winsock has no Unix sockets, so the tests relay the fakes' TCP connections to the exchange's sockets (`dmodemd/test/usb_dte.py`).
- **Serial:** the tests run one at a time.

## How it is done

The modem core and the DSP needed no changes beyond the build itself (MSVC added `std::call_once` for `pthread_once`, and a few GCC attributes and builtins kept to GCC and Clang):
- `_USE_MATH_DEFINES`, `NOMINMAX` and MinGW's C99 printf (`__USE_MINGW_ANSI_STDIO`).
- The pump interface's `modem_get_param` returns `intptr_t`, since a 64-bit Windows `long` is 32 bits.

The rest is dmodemd's front end (`dmodemd/portable.h`).  Windows' `select()` takes only sockets, so every descriptor of the main loop and the SIP thread is one:
- `socketpair()` becomes a pair of connected loopback TCP sockets.
- read, write and close become recv, send and closesocket.
- The COM port's and the pipe's reader threads feed such a pair; the DTE's going and coming (DSR, the pipe's client) travel in-band, escaped (`ThreadDte` in `windows_dte.h`).

Two Windows differences needed care:
- A plain close behind data the peer hasn't read isn't seen there. The modem's hang-up closes its audio socket with a reset, so the SIP side ends a call that is still ringing.
- A TCP read can split a 16-bit sample, so the audio reader keeps the odd byte for the next read.

Wine never completes an overlapped serial read that waits for the first character.  The COM reader therefore uses reads that return at once, and sleeps 5 ms when the port has nothing.
