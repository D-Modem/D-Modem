# D-Modem on Windows: technical details

This is the technical companion to [D-Modem on Windows](windows.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

dmodemd builds for 64-bit Windows, either with MinGW-w64 (Clang or GCC) or with Microsoft's compiler (MSVC). It is the same modem as on Linux:
- SIP (internet telephony) through the libre library;
- the open DSP: data up to V.34, V.90/V.92 as the digital modem, class 1 fax and voice;
- T.38 fax over IP.

The USB modems are lines of `dmodemd.exe` too (`--usb`, on WinUSB): Smart Link's SmartUSB56, the pico-serial Line and WB sticks, and Agere's USB 2.0 soft modem.

On Linux the DTE (the serial device that programs talk AT commands to) is a pty. On Windows it is one of four:
- a TCP connection (the default);
- a COM port;
- a named pipe;
- D-Modem's own virtual modem driver.

libdmodem (`dmodemd/dmodem.h`) and `libdmodem_sip.a` build for Windows too.

## Which Windows

Windows 7 is the oldest Windows that D-Modem runs on:

| Part | Windows |
|---|---|
| dmodemd.exe, dmodem-driver.exe | 7 and later |
| USB modems (`--usb`) | 8.1 and later on WinUSB; 7 and 8 on libusbK or UsbDk ([below](#windows-7-and-8)) |
| Virtual modem driver (`--driver`) | 10 (1803 and later): `dmodemvm.dll`, `dmodem.inf`; 7, 8 and 8.1: `dmodemvm7.dll`, `dmodem7.inf` ([below](#windows-7-8-and-81)) |

These parts are POSIX-only and are not built for Windows:
- the Asterisk and FreeSWITCH modules;
- dmodem-ari;
- the ALSA line (`-a`, `--alsa-ctl`), the ISDN line (`--misdn`, `--misdn-msn`, `--misdn-noflip`), the pty's options (`-g`, `-p`) and `modem_test`;
- the interop test peers (`dsp/test`'s `half_*`).

## Status

What has run, and where:
- **Under Wine on Linux:** the offline tests (`ctest -L quick`) in CI's `windows` job, on every push. By hand: dmodemd's calls through a private Asterisk (`asterisk/test/sip.sh` with `DMODEMD=build-win/dmodemd/dmodemd.exe`, [below](#testing-dmodemd-under-wine)), and the USB modems' tests against fake devices.
- **On Windows:** CI's `windows-msvc` job builds with MSVC on GitHub's Windows Server 2022 runner (`windows-2022`) and runs the offline tests there. The commit history shows this job running and its failures being fixed (for example the driver's link against `ntdll`, and tests that needed `tiffcp`). Whether its latest run passed is not recorded in the repository: see the job's page on GitHub.

What has not run yet:
- dmodemd on a desktop Windows, by hand;
- a real USB modem under Windows;
- either virtual modem driver loaded by Windows itself.

## Building

The usual build is a cross build from Linux. On Debian or Ubuntu:

    sudo apt-get install cmake ninja-build pkg-config g++-mingw-w64-x86-64-posix clang lld
    cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
    cmake --build build-win

**The compiler.** `cmake/mingw-w64.cmake` compiles with Clang (`--target=x86_64-w64-mingw32`, linking with lld) where Clang and lld are installed, and otherwise with MinGW-w64's GCC. `-DMINGW_COMPILER=gcc` asks for GCC.

**GCC's libraries.** Clang still uses MinGW-w64's GCC installation for its headers, C runtime, libstdc++ and winpthreads, so the GCC package is needed either way. The toolchain file takes the posix-thread compilers' installation (`x86_64-w64-mingw32-g++-posix`), which `std::thread` and `std::mutex` need. Two flags make Clang's objects fit GCC's libraries:
- `-femulated-tls`: thread-local variables are emulated, as GCC's are (libstdc++'s `std::call_once` uses them);
- `-pthread` on the link, for winpthreads.

**The libraries.** libre and Mbed TLS are fetched and built with the same toolchain. Mbed TLS provides libre's crypto, since Windows has no OpenSSL. It is built without its `-Werror` (`MBEDTLS_FATAL_WARNINGS=OFF`), because Clang warns about its `gnu_printf` format attribute on MinGW-w64.

**One file.** Everything links statically. `build-win/dmodemd/dmodemd.exe` is a single file that needs no DLLs beyond Windows' own. CI keeps each build's `dmodemd.exe` as an artifact (`dmodemd-windows-x86_64`).

**32-bit.** `-DMINGW_TRIPLE=i686-w64-mingw32` targets 32-bit Windows instead. This is not tested.

**Tests under Wine.** With Wine installed (`wine64`), `ctest --test-dir build-win -L quick` runs the tests under it (`CMAKE_CROSSCOMPILING_EMULATOR`). Set `WINEDEBUG=-all` for quieter logs.

### MSVC

On Windows, open a Developer Command Prompt for Visual Studio 2022 (it puts cl.exe and the Windows SDK on the path). With CMake, Ninja and Git installed:

    cmake -B build-msvc -G Ninja
    cmake --build build-msvc
    ctest --test-dir build-msvc -L quick

What the MSVC build does differently:
- **The build type** defaults to RelWithDebInfo, not MSVC's usual Debug, because an unoptimised modem can't keep up with a call. You can still ask for `-DCMAKE_BUILD_TYPE=Debug`.
- **The C runtime** is MSVC's static one (`/MT`), in libre and Mbed TLS too, so `dmodemd.exe` needs no vcruntime DLL.
- **POSIX headers**: small headers in `compat/msvc/` fill in what MSVC's C library lacks (`unistd.h`, `strings.h`, `sys/time.h`, and `dmodem_msvc.h` for `clock_gettime`, `ssize_t` and the like). They are used by MSVC builds only.
- **libre's typed `re_printf` arguments** are turned off (`DISABLE_RE_ARG`), because counting them needs GNU's comma elision.
- **The tests need Git's bash**, since they run under `cmake/test_run.sh`. CMake looks for it in `Program Files\Git\bin`, not at System32's WSL launcher.
- **Line endings** must be LF. `.gitattributes` asks for them even with `core.autocrlf`.

CI's `windows-msvc` job does this on GitHub's Windows runner. It keeps its `dmodemd.exe` (`dmodemd-windows-msvc-x86_64`), and the virtual modem driver's unsigned package (`dmodem-driver-windows-x86_64-unsigned`: `dmodemvm.dll` when the runner's WDK has UMDF 2.15, `dmodem-driver.exe` and `dmodem.inf`).

### clang-cl, from Linux

`cmake/clang-cl.cmake` builds the MSVC way from Linux, with LLVM's clang-cl and lld-link. It uses MSVC's headers, C runtime and ABI, so it stands in for cl.exe.
- It needs Clang 19 or later, because MSVC's current C++ library says so.
- Microsoft's C runtime and Windows SDK come from [xwin](https://github.com/Jake-Shadle/xwin). Its `--accept-license` accepts Microsoft's Visual Studio and Windows SDK licence.

The steps:

    cargo install xwin --locked
    xwin --accept-license --arch x86_64 splat --output ~/.xwin
    sudo apt-get install clang-20 clang-tools-20 lld-20
    cmake -B build-msvc -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl.cmake
    cmake --build build-msvc
    WINEDEBUG=-all ctest --test-dir build-msvc -L quick

Notes:
- clang-cl defines no `__GNUC__`. The few GCC attributes and builtins the code uses (printf format checks, weak symbols) are therefore left out, as they are for cl.exe.
- The build is warning-free with `-DDMODEM_WERROR=ON`.
- Its `dmodemd.exe` passes the quick tests and every `sip.sh` variant [below](#testing-dmodemd-under-wine) under Wine: TCP, the COM port, both pipe modes and voice.
- xwin leaves out the debug C runtime unless it is given `--include-debug-libs`. A Debug build needs it.

### Windows 7

The builds target Windows 7. `_WIN32_WINNT` and `WINVER` are `0x0601` (the CMake cache variable `DMODEM_WINNT`), so Windows' headers declare nothing newer.

The `win7_imports` test (in `ctest -L quick`; `cmake/win_imports.py`) reads the PE headers of `dmodemd.exe`, `dmodem-driver.exe` and whichever of the drivers (`dmodemvm.dll`, `dmodemvm7.dll`) were built. It fails on any of these:
- a subsystem version newer than 6.1;
- an import of a Universal CRT, Visual C++ runtime or API-set DLL;
- an import of a function that Windows 8 or later brought, from its list of the ones that libraries are known to reach for.

`python3 cmake/win_imports.py --list <file>` prints every import, so that a new one can be checked against Microsoft's "minimum supported client".

Every function the programs import today, libre's and libusb's included, was already in Windows Vista or 7. The toolchains:
- **MinGW-w64:** its runtime, `msvcrt.dll`, is in every Windows.
- **MSVC:** Visual Studio 2022's build tools still support Windows 7. Later ones may not: Microsoft's C++ library dropped Windows 7, 8 and 8.1 in the 14.50 tools (Visual Studio 2026). `win7_imports` shows whether a build still loads there.

## Running

Set the SIP account, then start dmodemd:

    set SIP_LOGIN=username:password@sip.example.com
    dmodemd.exe -d1

    modem `dmodem0' created. DTE is TCP 127.0.0.1:2323
    Use TCP 127.0.0.1:2323 as the modem, Ctrl+C for termination.

The options are Linux's, less the POSIX-only ones (`dmodemd.exe --help` lists them). Among them:
- `-L` registers and takes incoming calls.
- `-c` sets the country.
- `-d` sets the debug level.
- `-n` keeps the normal priority. Without it the process runs at high priority (`HIGH_PRIORITY_CLASS`). Either way the system timer is set to 1 ms (`timeBeginPeriod(1)`), so that the call's 20 ms frames come on time.

The modem keeps its line data (the V.34/V.90 parameters it learns) in `%LOCALAPPDATA%\dmodem\data.<name>`.

Windows' firewall asks once whether to allow dmodemd on the network. SIP and RTP need it.

### TCP (`--tcp`, the default)

`--tcp [address:]port` listens on that address and port. The default is `127.0.0.1:2323`; `0.0.0.0:2323` accepts other machines too.

Any program that can open a raw TCP connection can be the terminal: PuTTY (connection type Raw), `telnet 127.0.0.1 2323`, or a BBS client. It talks AT to the modem as it would on a serial line.
- One connection is served at a time.
- When the connection closes, the modem hangs up, as it does on Linux when the pty closes. dmodemd then takes the next connection.

### COM port (`--com`)

Some programs want a modem on a COM port: dial-up networking, fax programs, terminal programs with modem support. For them, dmodemd opens one end of a virtual null-modem pair, and the program opens the other end. [com0com](https://sourceforge.net/projects/com0com/) makes such pairs. For example, with com0com's `setupc`:

    setupc install PortName=COM5 PortName=COM6
    setupc change CNCB0 cts=rdtr,dcd=rrts
    dmodemd.exe --com COM5

The program then uses COM6 as its modem.
- dmodemd sets its end to 115200 8N1 with DTR on. com0com's port speed doesn't limit anything (unless `EmuBR=yes`).
- The program dropping DTR (which dmodemd sees as DSR) hangs the modem up.
- dmodemd's RTS follows the carrier: it is on while a call is connected. With `dcd=rrts` above, the program sees it as DCD.
- `cts=rdtr` keeps the program's CTS on. com0com's default would give it dmodemd's RTS, and CTS would then be off outside a call.
- There is no hardware flow control: the modem reads from the port as it has room.

The COM port's code is tested under Wine, with a pty pair in place of com0com: `WIN_COM=1` in `asterisk/test/sip.sh` ([below](#testing-dmodemd-under-wine)). A pty has no modem control lines, so the test does not cover the DSR hang-up or the RTS carrier signal.

### Named pipe (`--pipe`, `--pipe-connect`)

Virtual machines give a guest's COM port to a program on the host as a named pipe. So a guest OS (Windows 98's dial-up networking, a DOS BBS, Linux's pppd) can use dmodemd as its modem without com0com.

The name is `\\.\pipe\<name>`, or just `<name>`. Which side makes the pipe depends on the VM:
- `--pipe <name>`: dmodemd makes the pipe, and the VM connects to it.
  - Hyper-V: `Set-VMComPort -VMName <vm> -Number 1 -Path \\.\pipe\dmodem`
  - VirtualBox: Serial Port, mode Host Pipe, path `\\.\pipe\dmodem`, with "Connect to existing pipe/socket" ticked
  - VMware: "Use named pipe", with "This end is the client"
- `--pipe-connect <name>`: the VM makes the pipe, and dmodemd connects to it.
  - QEMU: `-serial pipe:dmodem`
  - VirtualBox: "Connect to existing pipe/socket" unticked
  - VMware: "This end is the server"

  dmodemd tries every 500 ms until the pipe is there, so the VM can start before or after it.

One client is served at a time:
- With `--pipe`, when the client goes (the VM stops, or the program closes the pipe), the modem hangs up and dmodemd waits for the next client.
- With `--pipe-connect`, when the pipe goes, the modem hangs up and dmodemd waits for the pipe to come back.
- With `--pipe`, the name is dmodemd's alone (`FILE_FLAG_FIRST_PIPE_INSTANCE`). If another program has it, dmodemd says so and stops.
- The pipe takes local clients only (`PIPE_REJECT_REMOTE_CLIENTS`).

A pipe has no control lines. The guest therefore sees no DCD from the modem, and can't hang up by dropping DTR: it hangs up with `+++` and `ATH`. If the guest's dial-up stack waits for DCD, set it to ignore DCD.

No real VM has been tried yet: the VM settings above come from their documentation.

The tests cover both ways under Wine:
- `pipe_test` (in `ctest -L quick`) runs dmodemd.exe with each option and talks AT through the pipe. It checks that a client can go and another be served, and that a name already taken is refused.
- `WIN_PIPE=made` or `WIN_PIPE=connect` in `asterisk/test/sip.sh` makes the test's calls over a pipe ([below](#testing-dmodemd-under-wine)).

### USB modems (`--usb`)

dmodemd.exe drives the same USB modems as dmodemd does on Linux ([hardware.md](hardware-details.md#smart-link-smartusb56)):
- Smart Link's SmartUSB56 (ST7554, USB `0483:7554`);
- the pico-serial Line stick (`1209:0002`);
- the pico-serial WB stick (`1209:0003`, a bare pair: [hardware.md](hardware-details.md#pico-serial-wb-wideband-bare-pair));
- Agere's USB 2.0 soft modem (`047E:2892`, `047E:2828`: [agere-usb.md](agere-usb.md)).

There is no SIP then: the line is the modem's telephone line. The DTE is any of the others: TCP, a COM port, a pipe, or the virtual modem driver's port.

The same code runs on both systems:
- the samples are isochronous transfers, on every one of the four;
- the hook and the ring detector go through the device's own requests and its interrupt endpoint;
- the SmartUSB56 and the pico-serial Line get caller ID, and the pico-serial Line gets the country's line settings too.

To set a modem up and use it:

    dmodem-driver usb dmodem-usb.inf      (once, as an administrator)
    dmodemd.exe --usb list
    dmodemd.exe --usb auto --tcp 127.0.0.1:2323

**The driver.** Windows has no kernel driver of its own for any of these devices. libusb reaches a USB device through WinUSB, Windows' generic USB driver, so the device must have WinUSB as its driver. One of these gives it that:
- **The pico-serial sticks ask for WinUSB themselves**, with no INF and no Zadig. Their firmware has Microsoft OS 2.0 descriptors: the WinUSB compatible ID, and D-Modem's interface GUID (the one `dmodem-usb.inf` gives). Line firmware older than that needs one of the two ways below.
- **`dmodem-usb.inf`** (`winmodem/`) gives all four devices WinUSB, and `dmodem-driver usb dmodem-usb.inf` installs it. The INF must be signed before Windows takes it: use the test signing of the virtual modem driver ([below](#the-virtual-modem-driver---driver)), with `dmodem-usb.cat`.
- **[Zadig](https://zadig.akeo.ie/)** does the same, and signs what it installs itself: choose the device, then WinUSB.

A SmartUSB56 with Smart Link's own Windows driver installed must be moved to WinUSB first. Until then, `--usb list` says "its driver is not WinUSB or libusbK". The Agere modem, likewise, must be moved from Agere's `AGRSM.sys` to WinUSB, with `dmodem-usb.inf` or Zadig.

#### Windows 7 and 8

WinUSB can't stream before Windows 8.1: it has no isochronous transfers there, and all four modems' samples are isochronous. On Windows 7 and 8 the modem needs one of these instead:
- **libusbK** as its driver: in [Zadig](https://zadig.akeo.ie/), choose the device, then libusbK. libusb drives it as it drives WinUSB, isochronous transfers included.
- **[UsbDk](https://github.com/daynix/UsbDk)**, installed from its MSI: a filter that lends libusb any USB device, whatever its driver. dmodemd.exe uses it by itself when WinUSB can't stream and UsbDk is there (`usb: through UsbDk` in its log). In the environment, `DMODEM_USBDK=1` makes it use UsbDk on any Windows, and `DMODEM_USBDK=0` never.

The pico-serial sticks' Microsoft OS 2.0 descriptors don't help there: Windows 7 and 8 don't read them, and the WinUSB they would give couldn't stream anyway.

When the driver can't stream, dmodemd.exe says so, and what to do: "WinUSB streams only from Windows 8.1 on: on Windows 7 and 8 give the modem libusbK with Zadig, or install UsbDk". `--usb list` adds that a modem listed as ready on WinUSB can't be used, and `dmodem-driver usb` says it after installing.

#### How dmodemd.exe runs a USB line

- **libusb:** the pinned release (1.0.27, LGPL-2.1-or-later) is fetched and built while configuring (`cmake/libusb.cmake`), for MinGW-w64, MSVC and clang-cl alike. It links into dmodemd.exe, which stays a single file.
  - `-DDMODEM_USB=OFF` builds without it.
  - `-DDMODEM_LIBUSB_DIR=` takes a libusb source tree that is already there.
- **Polling:** Windows' libusb has no file descriptors to wait on (Linux's main loop waits on its descriptors). So the main loop handles libusb's events at least every 2 ms (`UsbLine::POLL_MS`), along with the DTE's socket.
- **Messages:** when WinUSB isn't the device's driver, dmodemd.exe says so, and says how to make it so.

**Not yet tried on a real device under Windows.** The USB tests [below](#testing-dmodemd-under-wine) run dmodemd.exe under Wine against the same fake devices as Linux's tests.

### The virtual modem driver (`--driver`)

D-Modem has a Windows driver of its own (`winmodem/`). It is a virtual modem that Device Manager lists under Modems as "D-Modem Virtual Modem", with dmodemd as the modem behind it. Windows programs use it as they would a modem card:
- through Windows' modem stack (Unimodem and TAPI): dial-up networking, Windows Fax and Scan, HyperTerminal;
- or by its COM port's name, as a terminal or BBS program would.

No com0com pair is needed.

    dmodem-driver install dmodem.inf      (once, as an administrator)
    dmodemd.exe --driver auto

#### The pieces

**`dmodemvm.dll`** is a UMDF 2 driver (UMDF 2.15, Windows 10 and later). UMDF is Windows' User-Mode Driver Framework: the driver runs in Windows' driver host as a user-mode driver, so its faults can't crash Windows. Unimodem's `modem.sys` sits above it.

Its device is a COM port. It answers the reads, writes and serial IOCTLs that a modem card's port answers, with serial.sys's semantics (`winmodem/port.h`):
- the serial timeouts;
- `WaitCommEvent`'s events;
- the modem status lines;
- DTR and RTS, set directly or by the handflow settings;
- purges;
- `LSRMST_INSERT`'s in-band status.

**`dmodem.inf`** installs it in the Modem class.
- It has Unimodem's settings for D-Modem's commands: the init string `AT&FE0V1&C1&D2S0=0`, `&H` for flow control, `\N` for error control and `%C` for compression.
- It lists every result D-Modem gives as a response, each `CONNECT` rate among them. `winmodem/make_inf.py` makes the INF from the core's table, so a new rate can't be missed; the `winmodem_inf` test checks this.
- It needs Windows 10 1803 or later, for the driver store's DIRID 13.

**`dmodem-driver.exe`** makes the root device (`Root\DModemVM`) and installs the driver, as the WDK's devcon would:
- `install <inf>` adds a device, and a modem, each time it is run;
- `update <inf>` moves the devices to a new INF;
- `remove` takes the devices away;
- `list` gives each device's COM port and pipe;
- `usb <inf>` installs WinUSB for the USB modems ([above](#usb-modems---usb)).

**The pipe.** The driver makes a pipe, `\\.\pipe\dmodem-vm-<port>`, for each device (`dmodem-vm-COM3` for COM3).
- dmodemd connects to it with `--driver COM3`, or with `--driver auto` to the first one there is.
- dmodemd waits for the pipe, and connects again whenever the driver comes back (the device disabled and enabled, or Windows resumed).
- Frames on the pipe carry the characters both ways, the modem's status lines to the port, and the app's DTR and RTS back (`winmodem/wire.h`).
- Who can connect: the pipe takes local clients only, and of those only the system, the administrators, the local service and the interactively logged-on users (`PIPE_SDDL` in `winmodem/link.cpp`). dmodemd can therefore run as a program or as a service.
- One dmodemd at a time per pipe.

#### Between the app and dmodemd

- The modem's DSR and CTS are on while dmodemd is on the pipe, and its DCD is on with the carrier.
- The app dropping DTR, or closing the port, hangs the modem up, as `&D2` does on a real one.
- Without dmodemd, the port acts like a modem that is switched off: no DSR or CTS, and whatever the app writes is dropped.
- RI isn't driven: Unimodem takes the `RING` result.
- A break, XON/XOFF and the baud rate are accepted but change nothing: the link has no speed or flow control of its own.
- The port allows one handle at a time, as a serial port does.

#### Signing

The driver must be signed before Windows loads it. A package of your own can be signed for a test machine with the WDK's `Inf2Cat` and the SDK's `signtool`, in an elevated prompt:

    bcdedit /set testsigning on                (then restart)
    Inf2Cat /driver:<dir> /os:10_X64           (<dir>: dmodemvm.dll, dmodem.inf)
    New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=D-Modem test" -CertStoreLocation Cert:\LocalMachine\My
    (export it and import it into LocalMachine's Root and TrustedPublisher stores)
    signtool sign /fd sha256 /sm /s My /n "D-Modem test" <dir>\dmodemvm.cat <dir>\dmodemvm.dll
    dmodem-driver install <dir>\dmodem.inf

Signing for release (attestation through Microsoft's Partner Center) needs an EV certificate.

#### Building the driver

- **On Windows:** it needs Visual Studio's compiler and the Windows Driver Kit (WDK). CMake finds the WDK's UMDF 2.15 headers and stub library (`DMODEM_WDF_INCLUDE`, `DMODEM_WDF_STUB`) and builds `dmodemvm.dll`. `cmake --install` puts it beside `dmodem.inf` in `driver/`.
- **On Linux with clang-cl:** there is no WDK there. Give `-DDMODEM_WDF_INCLUDE=` the UMDF 2.15 headers of Microsoft's [Windows-Driver-Frameworks](https://github.com/microsoft/Windows-Driver-Frameworks) (`src/publicinc/wdf/umdf/2.15`, MIT-licensed). The driver is then compiled against them but not linked (`dmodemvm_check`): the WDK's stub library is the one piece missing.

**Not yet loaded by Windows.** Only the port, the pipe and dmodemd's end have run, under Wine ([below](#testing-dmodemd-under-wine)). The WDF part (`driver.cpp`) has been compiled and linked (in the `windows-msvc` CI job, when its WDK has UMDF 2.15), but not run, and the INF has not been installed.

#### Windows 7, 8 and 8.1

UMDF 2 begins with Windows 8.1, and its version 2.15 with Windows 10. So Windows 7, 8 and 8.1 get `dmodemvm7.dll` (`winmodem/driver7.cpp`) instead:
- **The framework:** it is the same device on UMDF 1.9, Windows 7's own user-mode framework, which later Windows still run. It is a COM DLL that UMDF's host loads. The COM port and the pipe to dmodemd are the same code (`port.h`, `link.h`), and so are dmodemd's side and everything an app sees.
- **The INF:** `dmodem7.inf`, made by `make_inf.py` with `dmodem.inf`'s Unimodem settings and responses, for x64 and x86. The DLL goes to `System32\drivers\UMDF`, with `UmdfLibraryVersion` 1.9.0 and the driver's class (`DriverCLSID`). Windows 7 has UMDF 1.9 built in, so no co-installer is needed.
- **Installing:**

      dmodem-driver install dmodem7.inf     (once, as an administrator)
      dmodemd.exe --driver auto

- **Signing:** as for Windows 10, with `Inf2Cat /os:7_X64` (or `7_X86`, `8_X64`, `6_3_X64`). 64-bit Windows 7 also needs `bcdedit /set testsigning on` for a test-signed package.
- **One difference inside:** a request the port holds has a buffer of its own. Whichever completes the request, the port or a cancel, copies that buffer into the request's memory. A request cancelled before the port has it is never touched again, because UMDF 1 can cancel a request just as it is marked cancelable.

Building it needs only UMDF 1.9's headers, no library:
- **On Windows:** CMake finds them in the WDK (`Include\wdf\umdf\1.9`).
- **Elsewhere (clang-cl):** give `-DDMODEM_UMDF1_INCLUDE=` the directory. The WDK's NuGet package `Microsoft.Windows.WDK.x64` has it as `c/Include/wdf/umdf/1.9` (`wudfddi.h`, `wudfddi_types.h`). The WDK's licence applies to these headers, so they aren't in this repository.
- **The output:** `cmake --install` puts `dmodemvm7.dll` beside `dmodem7.inf` in `driver7/`.

**Not yet tried on Windows itself either.** `umdf1_test` runs the whole DLL under Wine, in a UMDF 1 host of its own ([below](#testing-dmodemd-under-wine)). Windows' own host, the reflector and the INF haven't run it.

## Testing dmodemd under Wine

### Calls through Asterisk

`asterisk/test/sip.sh` makes calls between dmodemd and a private Asterisk. Given a Windows build, it runs `dmodemd.exe` under Wine:

    DMODEMD=$PWD/build-win/dmodemd/dmodemd.exe WINEDEBUG=-all asterisk/test/sip.sh
    INCOMING=1 DMODEMD=... asterisk/test/sip.sh
    VOICE_DIAL=1 DMODEMD=... asterisk/test/sip.sh
    WIN_COM=1 DMODEMD=... asterisk/test/sip.sh
    WIN_PIPE=made DMODEMD=... asterisk/test/sip.sh      (or WIN_PIPE=connect)
    WIN_DRIVER=1 DMODEMD=... asterisk/test/sip.sh

The DTE is a TCP connection by default. The variables change it (`asterisk/test/sip_call.py`):
- `WIN_COM=1`: Wine's COM9 (`--com COM9`), linked to a pty pair whose other end the test holds.
- `WIN_PIPE`: a named pipe, which `pipe_test.exe bridge` (beside dmodemd.exe) passes to the test over TCP.
- `WIN_DRIVER=1`: the virtual modem driver's port (`--driver SIPTEST`). `vmodem_test.exe bridge` runs the driver's end of the pipe (the port and the pipe: everything but the WDF part) and gives the test the port's app side over TCP. The test also checks that the port's DCD came on for the call and went off after it.

Asterisk and app_dmodem come from the Linux build (`build/`), as for the Linux test. The variants pass as they do on Linux: outgoing V.91 at 62666 bit/s, incoming, A-law, voice in and voice dial, and over the COM port, a pipe either way, or the driver's port.

### The driver's quick tests

The quick tests cover the driver without Windows:
- `port_test` (Linux and Windows): the COM port.
- `vmodem_test` (Windows): dmodemd.exe on the driver's pipe, with the driver's end of it in the test. It checks:
  - AT commands, and the INF's settings answered;
  - DSR going when dmodemd goes;
  - the driver going and coming back;
  - `--driver auto`;
  - a second port of the same name refused.
- `dmodem_driver_list` (Windows): `dmodem-driver.exe list`, with no device installed ("no virtual modem installed").
- `umdf1_test` (Windows, with `DMODEM_UMDF1_INCLUDE`): `dmodemvm7.dll`, the Windows 7 driver, in a UMDF 1 host of the test's own (`winmodem/test/umdf1_host.cpp`: the framework's objects that the driver uses, recording what it asks of them), with dmodemd.exe on its pipe. It checks:
  - the class factory, the device, its interface and its queue;
  - the COM port's symbolic link and SERIALCOMM value, made and taken back;
  - one handle at a time;
  - AT commands through the driver's reads, writes and IOCTLs;
  - a read cancelled while the port holds it, and one cancelled as it is marked cancelable;
  - `WaitCommEvent`;
  - no object of the DLL left at the end.
- `win7_imports` (Windows): the programs and drivers load on Windows 7 ([above](#windows-7)).
- `winmodem_inf`: both INFs are what `make_inf.py` makes.

### The USB modems' tests

`dmodemd_usb_windows` and `dmodemd_pico_line_windows` (label `alsa`) run dmodemd.exe under Wine. They are Linux's `usb_test.py` and `pico_line_test.py`. They check each device's whole feature set: the two modems' calls through the exchange, rings, caller ID, BUSY, pulse dialling, voice, the country's line settings and unplugging.

Wine's WinUSB has no isochronous transfers, as Windows 7's hasn't. So `pico_line_test.py` also checks the Windows 7 and 8 behaviour there:
- a driver that can't stream (the fake's `FAKE_USB_NO_ISO`) is reported, with what to do;
- UsbDk is taken when it is installed (`FAKE_USB_USBDK`), and not with `DMODEM_USBDK=0`;
- `--usb list`'s note.

How the tests are set up:
- **The fake devices:** `dmodemd_fake_st7554.exe` and `dmodemd_fake_pico_line.exe` are dmodemd.exe with the test's fake device linked in libusb's place, since Windows has no `LD_PRELOAD`. (`dmodemd_fake_pico_wb.exe` is built too, but no Windows test runs it yet.)
- **The DTE:** a TCP connection.
- **The exchange:** the Linux build's `line_exchange` (`DMODEM_HOST_LINE_EXCHANGE`, by default `build/dmodemd/line_exchange`). The tests are registered only in a cross build, and only when that file is there. Wine's Winsock has no Unix sockets, so the tests relay the fakes' TCP connections to the exchange's sockets (`dmodemd/test/usb_dte.py`).
- **Serial:** the tests run one at a time (`RUN_SERIAL`).

## How it is done

The modem core and the DSP needed no changes beyond the build itself. MSVC needed `std::call_once` in place of `pthread_once`, and the few GCC attributes and builtins are kept to GCC and Clang. Beyond that:
- `_USE_MATH_DEFINES`, `NOMINMAX`, and MinGW's C99 printf (`__USE_MINGW_ANSI_STDIO`).
- The pump interface's `modem_get_param` returns `intptr_t` (`dsp/modem_param.h`), since a `long` on 64-bit Windows is 32 bits.

The rest is in dmodemd's front end (`dmodemd/portable.h`). Windows' `select()` takes only sockets, so every descriptor of the main loop and the SIP thread is a socket:
- `socketpair()` becomes a pair of connected loopback TCP sockets.
- read, write and close become recv, send and closesocket.
- The COM port's and the pipe's reader threads feed such a pair. The DTE's going and coming (DSR, the pipe's client) travel in-band, escaped (`ThreadDte` in `dmodemd/windows_dte.h`).

Two Windows differences needed care:
- A plain close behind data that the peer hasn't read isn't seen there. So the modem's hang-up closes its audio socket with a reset (`SO_LINGER`), and the SIP side then ends a call that is still ringing.
- A TCP read can split a 16-bit sample, so the audio reader keeps the odd byte for the next read (`dmodemd/sip_modem.cpp`).

Wine never completes an overlapped serial read that waits for the first character. The COM reader therefore uses reads that return at once, and sleeps 5 ms when the port has nothing (`READ_POLL_MS` in `dmodemd/windows_dte.cpp`).
