# D-Modem on Windows

dmodemd builds for 64-bit Windows with MinGW-w64.  It is the same modem as on Linux: SIP on libre, the open DSP (data up to V.34 and V.90/V.92 as the digital modem, class 1 fax, voice), and T.38.  Linux's pty becomes one of two DTEs: a COM port or a TCP connection.  libdmodem (`dmodemd/dmodem.h`) and `libdmodem_sip.a` build for Windows too.

The following are POSIX-only and are not built for Windows:
- the Asterisk and FreeSWITCH modules
- dmodem-ari
- the ALSA line (`-a`), the pty options (`-g`, `-p`, `-r`) and modem_test
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

## Testing dmodemd under Wine

`asterisk/test/sip.sh` makes calls between dmodemd and a private Asterisk.  Given a Windows build, it runs `dmodemd.exe` under Wine:

    DMODEMD=$PWD/build-win/dmodemd/dmodemd.exe WINEDEBUG=-all asterisk/test/sip.sh
    INCOMING=1 DMODEMD=... asterisk/test/sip.sh
    VOICE_DIAL=1 DMODEMD=... asterisk/test/sip.sh
    WIN_COM=1 DMODEMD=... asterisk/test/sip.sh

The DTE is a TCP connection by default.  With `WIN_COM=1` it is Wine's COM9, linked to a pty pair whose other end the test holds.  Asterisk and app_dmodem come from the Linux build (`build/`), as for the Linux test.  The variants (outgoing V.91 at 62666, incoming, A-law, voice in and voice dial, and over the COM port) pass as on Linux.

## How it is done

The modem core and the DSP needed no changes beyond the build itself:
- `_USE_MATH_DEFINES`, `NOMINMAX` and MinGW's C99 printf (`__USE_MINGW_ANSI_STDIO`).
- The pump interface's `modem_get_param` returns `intptr_t`, since a 64-bit Windows `long` is 32 bits.

The rest is dmodemd's front end (`dmodemd/portable.h`).  Windows' `select()` takes only sockets, so every descriptor of the main loop and the SIP thread is one:
- `socketpair()` becomes a pair of connected loopback TCP sockets.
- read, write and close become recv, send and closesocket.
- The COM port's reader thread feeds such a pair; the port's DSR changes travel in-band, escaped.

Two Windows differences needed care:
- A plain close behind data the peer hasn't read isn't seen there. The modem's hang-up closes its audio socket with a reset, so the SIP side ends a call that is still ringing.
- A TCP read can split a 16-bit sample, so the audio reader keeps the odd byte for the next read.

Wine never completes an overlapped serial read that waits for the first character.  The COM reader therefore uses reads that return at once, and sleeps 5 ms when the port has nothing.
