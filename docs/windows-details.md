# D-Modem on Windows: technical details

This page gives the technical details for [D-Modem on Windows](windows.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

dmodemd builds for 64-bit Windows, with MinGW-w64 (Clang or GCC) or with the compiler of Microsoft (MSVC). It is the same modem as on Linux. It has these parts:
- SIP (internet telephony) through the libre library.
- The open DSP: data up to V.34, V.90/V.92 as the digital modem, class 1 fax and voice.
- T.38 fax over IP.

The USB modems are also lines of `dmodemd.exe` (`--usb`, on WinUSB): the SmartUSB56 of Smart Link, the pico-serial Line and WB sticks, and the USB 2.0 soft modem of Agere.

On Linux, the DTE (the serial device to which programs send AT commands) is a pty. On Windows, it is one of four items:
- A TCP connection (the default).
- A COM port.
- A named pipe.
- The own virtual modem driver of D-Modem.

libdmodem (`frontend/dmodemd/dmodem.h`) and `libdmodem_sip.a` also build for Windows.

## Which Windows

Windows 7 is the oldest Windows on which D-Modem runs:

| Part | Windows |
|---|---|
| dmodemd.exe, dmodem-driver.exe | 7 and later |
| USB modems (`--usb`) | 8.1 and later on WinUSB; 7 and 8 on libusbK or UsbDk ([below](#windows-7-and-8)) |
| Virtual modem driver (`--driver`) | 10 (1803 and later): `dmodemvm.dll`, `dmodem.inf`; 7, 8 and 8.1: `dmodemvm7.dll`, `dmodem7.inf` ([below](#windows-7-8-and-81)) |

These parts are only for POSIX and do not build for Windows:
- The Asterisk and FreeSWITCH modules.
- dmodem-ari.
- The ALSA line (`-a`, `--alsa-ctl`), the ISDN line (`--misdn`, `--misdn-msn`, `--misdn-noflip`), the options of the pty (`-g`, `-p`) and `modem_test`.
- The interop test peers (the `half_*` of `dsp/test`).

## Status

These items have run:
- **Under Wine on Linux:** the offline tests (`ctest -L quick`) in the `windows` job of the CI, on each push. Manually: the calls of dmodemd through a private Asterisk (`frontend/asterisk/test/sip.sh` with `DMODEMD=build-win/dmodemd/dmodemd.exe`, [below](#testing-dmodemd-under-wine)), and the tests of the USB modems against fake devices.
- **On Windows:** the `windows-msvc` job of the CI builds with MSVC on the Windows Server 2022 runner of GitHub (`windows-2022`) and runs the offline tests there. The commit history shows that this job ran and that we repaired its failures (for example, the link of the driver with `ntdll`, and tests that needed `tiffcp`). The repository does not record the result of its latest run. Refer to the page of the job on GitHub.

These items have not run:
- dmodemd on a desktop Windows, manually.
- A real USB modem under Windows.
- A virtual modem driver that Windows loaded.

## Building

The usual build is a cross build from Linux. On Debian or Ubuntu:

    sudo apt-get install cmake ninja-build pkg-config g++-mingw-w64-x86-64-posix clang lld
    cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake
    cmake --build build-win

**The compiler.** `platforms/windows/mingw-w64.cmake` compiles with Clang (`--target=x86_64-w64-mingw32`, with lld to link) if Clang and lld are installed. If not, it uses the GCC of MinGW-w64. `-DMINGW_COMPILER=gcc` asks for GCC.

**The libraries of GCC.** Clang still uses the GCC installation of MinGW-w64 for its headers, C runtime, libstdc++ and winpthreads. Thus, the GCC package is always necessary. The toolchain file uses the installation of the posix-thread compilers (`x86_64-w64-mingw32-g++-posix`), which `std::thread` and `std::mutex` need. Two flags make the objects of Clang compatible with the libraries of GCC:
- `-femulated-tls`: thread-local variables are emulated, as in GCC (the `std::call_once` of libstdc++ uses them).
- `-pthread` on the link, for winpthreads.

**The libraries.** The configure step downloads libre and Mbed TLS and builds them with the same toolchain. Mbed TLS gives the crypto of libre (as on all systems), and also its AES through `cmake/libre-mbedtls-aes.patch`. Thus, SRTP operates on Windows. It is built without its `-Werror` (`MBEDTLS_FATAL_WARNINGS=OFF`), because Clang gives warnings about its `gnu_printf` format attribute on MinGW-w64.

**One file.** All parts link statically. `build-win/dmodemd/dmodemd.exe` is a single file that needs no DLLs other than those of Windows. The CI keeps the `dmodemd.exe` of each build as an artifact (`dmodemd-windows-x86_64`).

**32-bit.** `-DMINGW_TRIPLE=i686-w64-mingw32` builds for 32-bit Windows. We did not test this.

**Tests under Wine.** With Wine installed (`wine64`), `ctest --test-dir build-win -L quick` runs the tests under it (`CMAKE_CROSSCOMPILING_EMULATOR`). For quieter logs, set `WINEDEBUG=-all`.

### MSVC

On Windows, open a Developer Command Prompt for Visual Studio 2022 (it puts cl.exe and the Windows SDK on the path). With CMake, Ninja and Git installed:

    cmake -B build-msvc -G Ninja
    cmake --build build-msvc
    ctest --test-dir build-msvc -L quick

The MSVC build has these differences:
- **The build type** is RelWithDebInfo by default, not the usual Debug of MSVC, because a modem without optimization is too slow for a call. You can still ask for `-DCMAKE_BUILD_TYPE=Debug`.
- **The C runtime** is the static runtime of MSVC (`/MT`), also in libre and Mbed TLS. Thus, `dmodemd.exe` needs no vcruntime DLL.
- **POSIX headers**: small headers in `platforms/windows/msvc/` add the items that the C library of MSVC does not have (`unistd.h`, `strings.h`, `sys/time.h`, and `dmodem_msvc.h` for `clock_gettime`, `ssize_t` and similar items). Only MSVC builds use them.
- **The typed `re_printf` arguments of libre** are disabled (`DISABLE_RE_ARG`), because the count of them needs the comma elision of GNU.
- **The tests need the bash of Git**, because they run under `cmake/test_run.sh`. CMake looks for it in `Program Files\Git\bin`, not at the WSL launcher of System32.
- **Line endings** must be LF. `.gitattributes` asks for them also with `core.autocrlf`.

The `windows-msvc` job of the CI does this on the Windows runner of GitHub. It keeps its `dmodemd.exe` (`dmodemd-windows-msvc-x86_64`), and the unsigned package of the virtual modem driver (`dmodem-driver-windows-x86_64-unsigned`: `dmodemvm.dll` when the WDK of the runner has UMDF 2.15, `dmodem-driver.exe` and `dmodem.inf`).

### clang-cl, from Linux

`platforms/windows/clang-cl.cmake` builds in the MSVC mode from Linux, with the clang-cl and lld-link of LLVM. It uses the headers, C runtime and ABI of MSVC. Thus, it replaces cl.exe.
- It needs Clang 19 or later, because the current C++ library of MSVC requires it.
- The C runtime and Windows SDK of Microsoft come from [xwin](https://github.com/Jake-Shadle/xwin). Its `--accept-license` accepts the Visual Studio and Windows SDK licence of Microsoft.

The steps:

    cargo install xwin --locked
    xwin --accept-license --arch x86_64 splat --output ~/.xwin
    sudo apt-get install clang-20 clang-tools-20 lld-20
    cmake -B build-msvc -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/windows/clang-cl.cmake
    cmake --build build-msvc
    WINEDEBUG=-all ctest --test-dir build-msvc -L quick

Notes:
- clang-cl defines no `__GNUC__`. Thus, the code does not use its few GCC attributes and builtins (printf format checks, weak symbols), as for cl.exe.
- The build has no warnings with `-DDMODEM_WERROR=ON`.
- Its `dmodemd.exe` passes the quick tests and all `sip.sh` variants [below](#testing-dmodemd-under-wine) under Wine: TCP, the COM port, the two pipe modes and voice.
- xwin does not include the debug C runtime unless you give it `--include-debug-libs`. A Debug build needs it.

### Windows 7

The builds are for Windows 7. `_WIN32_WINNT` and `WINVER` are `0x0601` (the CMake cache variable `DMODEM_WINNT`). Thus, the headers of Windows declare nothing newer.

The `win7_imports` test (in `ctest -L quick`; `platforms/windows/win_imports.py`) reads the PE headers of `dmodemd.exe`, `dmodem-driver.exe` and the drivers that were built (`dmodemvm.dll`, `dmodemvm7.dll`). It fails on one of these items:
- A subsystem version newer than 6.1.
- An import of a Universal CRT, Visual C++ runtime or API-set DLL.
- An import of a function that came in Windows 8 or later, from its list of the functions that libraries frequently use.

`python3 platforms/windows/win_imports.py --list <file>` prints all imports. Thus, you can compare a new import with the "minimum supported client" of Microsoft.

All functions that the programs import now, also those of libre and libusb, were already in Windows Vista or 7. The toolchains:
- **MinGW-w64:** its runtime, `msvcrt.dll`, is in all versions of Windows.
- **MSVC:** the build tools of Visual Studio 2022 still support Windows 7. Later build tools possibly do not. The C++ library of Microsoft removed Windows 7, 8 and 8.1 in the 14.50 tools (Visual Studio 2026). `win7_imports` shows if a build still loads there.

## Running

Set the SIP account, then start dmodemd:

    set SIP_LOGIN=username:password@sip.example.com
    dmodemd.exe -d1

    modem `dmodem0' created. DTE is TCP 127.0.0.1:2323
    Use TCP 127.0.0.1:2323 as the modem, Ctrl+C for termination.

The options are those of Linux, without the options that are only for POSIX (`dmodemd.exe --help` gives a list of them). Some of them:
- `-L` registers and accepts incoming calls.
- `-c` sets the country.
- `-d` sets the debug level.
- `-n` keeps the normal priority. Without it, the process runs at high priority (`HIGH_PRIORITY_CLASS`). In the two cases, the system timer is set to 1 ms (`timeBeginPeriod(1)`). Thus, the 20 ms frames of the call come on time.

The modem keeps its line data (the V.34/V.90 parameters that it learns) in `%LOCALAPPDATA%\dmodem\data.<name>`.

The firewall of Windows asks one time if dmodemd can use the network. SIP and RTP need it.

### TCP (`--tcp`, the default)

`--tcp [address:]port` listens on that address and port. The default is `127.0.0.1:2323`. `0.0.0.0:2323` also accepts other machines.

All programs that can open a raw TCP connection can be the terminal: PuTTY (connection type Raw), `telnet 127.0.0.1 2323`, or a BBS client. The program sends AT commands to the modem as on a serial line.
- dmodemd accepts one connection at a time.
- When the connection closes, the modem hangs up, as on Linux when the pty closes. Then dmodemd accepts the next connection.

### COM port (`--com`)

Some programs want a modem on a COM port: dial-up networking, fax programs, terminal programs with modem support. For them, dmodemd opens one end of a virtual null-modem pair, and the program opens the other end. [com0com](https://sourceforge.net/projects/com0com/) makes such pairs. For example, with the `setupc` of com0com:

    setupc install PortName=COM5 PortName=COM6
    setupc change CNCB0 cts=rdtr,dcd=rrts
    dmodemd.exe --com COM5

Then the program uses COM6 as its modem.
- dmodemd sets its end to 115200 8N1 with DTR on. The port speed of com0com does not set a limit (unless `EmuBR=yes`).
- When the program drops DTR (which dmodemd sees as DSR), the modem hangs up.
- The RTS of dmodemd follows the carrier. It is on while a call is connected. With `dcd=rrts` above, the program sees it as DCD.
- `cts=rdtr` keeps the CTS of the program on. The default of com0com gives it the RTS of dmodemd. Then CTS is off outside a call.
- There is no hardware flow control. The modem reads from the port when it has space.

We test the code of the COM port under Wine, with a pty pair in place of com0com: `WIN_COM=1` in `frontend/asterisk/test/sip.sh` ([below](#testing-dmodemd-under-wine)). A pty has no modem control lines. Thus, the test does not include the DSR hang-up or the RTS carrier signal.

### Named pipe (`--pipe`, `--pipe-connect`)

Virtual machines give the COM port of a guest to a program on the host as a named pipe. Thus, a guest OS (the dial-up networking of Windows 98, a DOS BBS, the pppd of Linux) can use dmodemd as its modem without com0com.

The name is `\\.\pipe\<name>`, or only `<name>`. The side that makes the pipe depends on the VM:
- `--pipe <name>`: dmodemd makes the pipe, and the VM connects to it.
  - Hyper-V: `Set-VMComPort -VMName <vm> -Number 1 -Path \\.\pipe\dmodem`
  - VirtualBox: Serial Port, mode Host Pipe, path `\\.\pipe\dmodem`, with "Connect to existing pipe/socket" ticked
  - VMware: "Use named pipe", with "This end is the client"
- `--pipe-connect <name>`: the VM makes the pipe, and dmodemd connects to it.
  - QEMU: `-serial pipe:dmodem`
  - VirtualBox: "Connect to existing pipe/socket" unticked
  - VMware: "This end is the server"

  dmodemd tries each 500 ms until the pipe is there. Thus, the VM can start before or after it.

dmodemd accepts one client at a time:
- With `--pipe`, when the client stops (the VM stops, or the program closes the pipe), the modem hangs up and dmodemd waits for the next client.
- With `--pipe-connect`, when the pipe stops, the modem hangs up and dmodemd waits for the pipe to come back.
- With `--pipe`, only dmodemd can use the name (`FILE_FLAG_FIRST_PIPE_INSTANCE`). If a different program has it, dmodemd tells this and stops.
- The pipe accepts only local clients (`PIPE_REJECT_REMOTE_CLIENTS`).

A pipe has no control lines. Thus, the guest sees no DCD from the modem, and it cannot hang up with a drop of DTR. It hangs up with `+++` and `ATH`. If the dial-up stack of the guest waits for DCD, set it to ignore DCD.

Nobody has tried a real VM. The VM settings above come from their documentation.

The tests include the two modes under Wine:
- `pipe_test` (in `ctest -L quick`) runs dmodemd.exe with each option and sends AT commands through the pipe. It does a check that a client can stop and dmodemd then accepts a different client, and that dmodemd refuses a name that is already in use.
- `WIN_PIPE=made` or `WIN_PIPE=connect` in `frontend/asterisk/test/sip.sh` makes the calls of the test through a pipe ([below](#testing-dmodemd-under-wine)).

### USB modems (`--usb`)

dmodemd.exe operates the same USB modems as dmodemd on Linux ([hardware.md](hardware-details.md#smart-link-smartusb56)):
- The SmartUSB56 of Smart Link (ST7554, USB `0483:7554`).
- The pico-serial Line stick (`1209:0002`).
- The pico-serial WB stick (`1209:0003`, a bare pair: [hardware.md](hardware-details.md#pico-serial-wb-wideband-bare-pair)).
- The USB 2.0 soft modem of Agere (`047E:2892`, `047E:2828`: [agere-usb.md](agere-usb.md)).

In this mode, there is no SIP. The line is the telephone line of the modem. The DTE is one of the others: TCP, a COM port, a pipe, or the port of the virtual modem driver.

The same code runs on the two systems:
- The samples are isochronous transfers, on all four devices.
- The hook and the ring detector go through the own requests of the device and its interrupt endpoint.
- The SmartUSB56 and the pico-serial Line get caller ID. The pico-serial Line also gets the line settings of the country.

To configure a modem and use it:

    dmodem-driver usb dmodem-usb.inf      (once, as an administrator)
    dmodemd.exe --usb list
    dmodemd.exe --usb auto --tcp 127.0.0.1:2323

**The driver.** Windows has no kernel driver for these devices. libusb connects to a USB device through WinUSB, the generic USB driver of Windows. Thus, the device must have WinUSB as its driver. One of these items gives it WinUSB:
- **The pico-serial sticks ask for WinUSB themselves**, with no INF and no Zadig. Their firmware has Microsoft OS 2.0 descriptors: the WinUSB compatible ID, and the interface GUID of D-Modem (the GUID that `dmodem-usb.inf` gives). Line firmware that is older than that needs one of the two methods below.
- **`dmodem-usb.inf`** (`platforms/windows/winmodem/`) gives all four devices WinUSB, and `dmodem-driver usb dmodem-usb.inf` installs it. The INF must be signed before Windows accepts it. Use the test signing of the virtual modem driver ([below](#the-virtual-modem-driver---driver)), with `dmodem-usb.cat`.
- **[Zadig](https://zadig.akeo.ie/)** does the same, and signs the driver that it installs. Select the device, then WinUSB.

If the Windows driver of Smart Link is installed on a SmartUSB56, first move the SmartUSB56 to WinUSB. Until then, `--usb list` shows "its driver is not WinUSB or libusbK". Also, move the Agere modem from the `AGRSM.sys` of Agere to WinUSB, with `dmodem-usb.inf` or Zadig.

#### Windows 7 and 8

Before Windows 8.1, WinUSB cannot stream. It has no isochronous transfers there, and the samples of all four modems are isochronous. On Windows 7 and 8, the modem needs one of these items:
- **libusbK** as its driver: in [Zadig](https://zadig.akeo.ie/), select the device, then libusbK. libusb operates it as it operates WinUSB, also with isochronous transfers.
- **[UsbDk](https://github.com/daynix/UsbDk)**, installed from its MSI: a filter that gives libusb access to all USB devices, whatever their driver. dmodemd.exe uses it automatically when WinUSB cannot stream and UsbDk is installed (`usb: through UsbDk` in its log). In the environment, `DMODEM_USBDK=1` makes it use UsbDk on all versions of Windows, and `DMODEM_USBDK=0` never.

The Microsoft OS 2.0 descriptors of the pico-serial sticks do not help there. Windows 7 and 8 do not read them, and the WinUSB that they would give cannot stream.

When the driver cannot stream, dmodemd.exe tells this, and what to do: "WinUSB streams only from Windows 8.1 on: on Windows 7 and 8 give the modem libusbK with Zadig, or install UsbDk". `--usb list` adds that you cannot use a modem that it shows as ready on WinUSB. `dmodem-driver usb` tells this after it installs the driver.

#### How dmodemd.exe runs a USB line

- **libusb:** the configure step downloads the pinned release (1.0.27, LGPL-2.1-or-later) and builds it (`cmake/libusb.cmake`), for MinGW-w64, MSVC and clang-cl. It links into dmodemd.exe, which stays a single file.
  - `-DDMODEM_USB=OFF` builds without it.
  - `-DDMODEM_LIBUSB_DIR=` uses a libusb source tree that is already there.
- **Polling:** the libusb of Windows has no file descriptors to wait on (the main loop of Linux waits on its descriptors). Thus, the main loop does the events of libusb a minimum of each 2 ms (`UsbLine::POLL_MS`), together with the socket of the DTE.
- **Messages:** when WinUSB is not the driver of the device, dmodemd.exe tells this, and tells how to change it.

**Nobody has tried it on a real device under Windows.** The USB tests [below](#testing-dmodemd-under-wine) run dmodemd.exe under Wine against the same fake devices as the tests of Linux.

### The virtual modem driver (`--driver`)

D-Modem has its own Windows driver (`platforms/windows/winmodem/`). It is a virtual modem that Device Manager shows under Modems as "D-Modem Virtual Modem", with dmodemd as the modem behind it. Windows programs use it as they use a modem card:
- Through the modem stack of Windows (Unimodem and TAPI): dial-up networking, Windows Fax and Scan, HyperTerminal.
- Or by the name of its COM port, as a terminal or BBS program does.

A com0com pair is not necessary.

    dmodem-driver install dmodem.inf      (once, as an administrator)
    dmodemd.exe --driver auto

#### The pieces

**`dmodemvm.dll`** is a UMDF 2 driver (UMDF 2.15, Windows 10 and later). UMDF is the User-Mode Driver Framework of Windows. The driver runs in the driver host of Windows as a user-mode driver. Thus, its faults cannot crash Windows. The `modem.sys` of Unimodem is above it.

Its device is a COM port. It answers the reads, writes and serial IOCTLs that the port of a modem card answers, with the semantics of serial.sys (`platforms/windows/winmodem/port.h`):
- The serial timeouts.
- The events of `WaitCommEvent`.
- The modem status lines.
- DTR and RTS, set directly or by the handflow settings.
- Purges.
- The in-band status of `LSRMST_INSERT`.

**`dmodem.inf`** installs it in the Modem class.
- It has the Unimodem settings for the commands of D-Modem: the init string `AT&FE0V1&C1&D2S0=0`, `&H` for flow control, `\N` for error control and `%C` for compression.
- It gives a list of all results of D-Modem as responses, with each `CONNECT` rate. `platforms/windows/winmodem/make_inf.py` makes the INF from the table of the core. Thus, a new rate cannot be missed. The `winmodem_inf` test does a check of this.
- It needs Windows 10 1803 or later, for the DIRID 13 of the driver store.

**`dmodem-driver.exe`** makes the root device (`Root\DModemVM`) and installs the driver, as the devcon of the WDK does:
- `install <inf>` adds a device, and a modem, each time that you run it.
- `update <inf>` moves the devices to a new INF.
- `remove` removes the devices.
- `list` gives the COM port and pipe of each device.
- `usb <inf>` installs WinUSB for the USB modems ([above](#usb-modems---usb)).

**The pipe.** The driver makes a pipe, `\\.\pipe\dmodem-vm-<port>`, for each device (`dmodem-vm-COM3` for COM3).
- dmodemd connects to it with `--driver COM3`, or with `--driver auto` to the first pipe that is available.
- dmodemd waits for the pipe. It connects again each time that the driver comes back (the device disabled and enabled, or Windows resumed).
- Frames on the pipe send the characters in the two directions, the status lines of the modem to the port, and the DTR and RTS of the app back (`platforms/windows/winmodem/wire.h`).
- Who can connect: the pipe accepts only local clients. Of these, it accepts only the system, the administrators, the local service and the users that logged on interactively (`PIPE_SDDL` in `platforms/windows/winmodem/link.cpp`). Thus, dmodemd can run as a program or as a service.
- One dmodemd at a time for each pipe.

#### Between the app and dmodemd

- The DSR and CTS of the modem are on while dmodemd is on the pipe. Its DCD is on with the carrier.
- If the app drops DTR or closes the port, the modem hangs up, as `&D2` does on a real modem.
- Without dmodemd, the port operates as a modem that is off: no DSR or CTS, and the data that the app writes is discarded.
- The driver does not control RI. Unimodem uses the `RING` result.
- The driver accepts a break, XON/XOFF and the baud rate, but they change nothing. The link has no speed or flow control of its own.
- The port allows one handle at a time, as a serial port does.

#### Signing

The driver must be signed before Windows loads it. You can sign your own package for a test machine with the `Inf2Cat` of the WDK and the `signtool` of the SDK, in an elevated prompt:

    bcdedit /set testsigning on                (then restart)
    Inf2Cat /driver:<dir> /os:10_X64           (<dir>: dmodemvm.dll, dmodem.inf)
    New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=D-Modem test" -CertStoreLocation Cert:\LocalMachine\My
    (export it and import it into LocalMachine's Root and TrustedPublisher stores)
    signtool sign /fd sha256 /sm /s My /n "D-Modem test" <dir>\dmodemvm.cat <dir>\dmodemvm.dll
    dmodem-driver install <dir>\dmodem.inf

The signing for release (attestation through the Partner Center of Microsoft) needs an EV certificate.

#### Building the driver

- **On Windows:** it needs the compiler of Visual Studio and the Windows Driver Kit (WDK). CMake finds the UMDF 2.15 headers and stub library of the WDK (`DMODEM_WDF_INCLUDE`, `DMODEM_WDF_STUB`) and builds `dmodemvm.dll`. `cmake --install` puts it next to `dmodem.inf` in `driver/`.
- **On Linux with clang-cl:** there is no WDK there. Give `-DDMODEM_WDF_INCLUDE=` the UMDF 2.15 headers of the [Windows-Driver-Frameworks](https://github.com/microsoft/Windows-Driver-Frameworks) of Microsoft (`src/publicinc/wdf/umdf/2.15`, MIT-licensed). The driver is then compiled with them but not linked (`dmodemvm_check`). The stub library of the WDK is the one missing piece.

**Windows has not loaded it.** Only the port, the pipe and the end of dmodemd have run, under Wine ([below](#testing-dmodemd-under-wine)). The WDF part (`driver.cpp`) was compiled and linked (in the `windows-msvc` CI job, when its WDK has UMDF 2.15), but it did not run. Nobody has installed the INF.

#### Windows 7, 8 and 8.1

UMDF 2 starts with Windows 8.1, and its version 2.15 with Windows 10. Thus, Windows 7, 8 and 8.1 get `dmodemvm7.dll` (`platforms/windows/winmodem/driver7.cpp`):
- **The framework:** it is the same device on UMDF 1.9, the user-mode framework of Windows 7, which later versions of Windows still run. It is a COM DLL that the host of UMDF loads. The COM port and the pipe to dmodemd are the same code (`port.h`, `link.h`). The side of dmodemd and all items that an app sees are also the same.
- **The INF:** `dmodem7.inf`, made by `make_inf.py` with the Unimodem settings and responses of `dmodem.inf`, for x64 and x86. The DLL goes to `System32\drivers\UMDF`, with `UmdfLibraryVersion` 1.9.0 and the class of the driver (`DriverCLSID`). Windows 7 has UMDF 1.9 built in. Thus, a co-installer is not necessary.
- **Installing:**

      dmodem-driver install dmodem7.inf     (once, as an administrator)
      dmodemd.exe --driver auto

- **Signing:** as for Windows 10, with `Inf2Cat /os:7_X64` (or `7_X86`, `8_X64`, `6_3_X64`). 64-bit Windows 7 also needs `bcdedit /set testsigning on` for a test-signed package.
- **One internal difference:** a request that the port holds has its own buffer. The item that completes the request, the port or a cancel, copies that buffer into the memory of the request. The driver never touches again a request that was cancelled before the port has it, because UMDF 1 can cancel a request at the time that it is marked cancelable.

To build it, only the headers of UMDF 1.9 are necessary, and no library:
- **On Windows:** CMake finds them in the WDK (`Include\wdf\umdf\1.9`).
- **On other systems (clang-cl):** give `-DDMODEM_UMDF1_INCLUDE=` the directory. The NuGet package `Microsoft.Windows.WDK.x64` of the WDK has it as `c/Include/wdf/umdf/1.9` (`wudfddi.h`, `wudfddi_types.h`). The licence of the WDK applies to these headers. Thus, they are not in this repository.
- **The output:** `cmake --install` puts `dmodemvm7.dll` next to `dmodem7.inf` in `driver7/`.

**Nobody has tried it on Windows.** `umdf1_test` runs the full DLL under Wine, in its own UMDF 1 host ([below](#testing-dmodemd-under-wine)). The host of Windows, the reflector and the INF have not run it.

## Testing dmodemd under Wine

### Calls through Asterisk

`frontend/asterisk/test/sip.sh` makes calls between dmodemd and a private Asterisk. With a Windows build, it runs `dmodemd.exe` under Wine:

    DMODEMD=$PWD/build-win/dmodemd/dmodemd.exe WINEDEBUG=-all frontend/asterisk/test/sip.sh
    INCOMING=1 DMODEMD=... frontend/asterisk/test/sip.sh
    VOICE_DIAL=1 DMODEMD=... frontend/asterisk/test/sip.sh
    WIN_COM=1 DMODEMD=... frontend/asterisk/test/sip.sh
    WIN_PIPE=made DMODEMD=... frontend/asterisk/test/sip.sh      (or WIN_PIPE=connect)
    WIN_DRIVER=1 DMODEMD=... frontend/asterisk/test/sip.sh

The DTE is a TCP connection by default. The variables change it (`frontend/asterisk/test/sip_call.py`):
- `WIN_COM=1`: the COM9 of Wine (`--com COM9`), linked to a pty pair. The test holds the other end of the pair.
- `WIN_PIPE`: a named pipe, which `pipe_test.exe bridge` (next to dmodemd.exe) sends to the test through TCP.
- `WIN_DRIVER=1`: the port of the virtual modem driver (`--driver SIPTEST`). `vmodem_test.exe bridge` runs the end of the pipe of the driver (the port and the pipe: all parts except the WDF part). It gives the test the app side of the port through TCP. The test also does a check that the DCD of the port came on for the call and went off after it.

Asterisk and app_dmodem come from the Linux build (`build/`), as for the Linux test. The variants pass as on Linux: outgoing V.91 at 62666 bit/s, incoming, A-law, voice in and voice dial, and through the COM port, a pipe in the two modes, or the port of the driver.

### The driver's quick tests

The quick tests test the driver without Windows:
- `port_test` (Linux and Windows): the COM port.
- `vmodem_test` (Windows): dmodemd.exe on the pipe of the driver, with the end of the driver in the test. It does a check of these items:
  - AT commands, and the answers to the settings of the INF.
  - DSR goes off when dmodemd stops.
  - The driver stops and comes back.
  - `--driver auto`.
  - A second port with the same name is refused.
- `dmodem_driver_list` (Windows): `dmodem-driver.exe list`, with no device installed ("no virtual modem installed").
- `umdf1_test` (Windows, with `DMODEM_UMDF1_INCLUDE`): `dmodemvm7.dll`, the Windows 7 driver, in the own UMDF 1 host of the test (`platforms/windows/winmodem/test/umdf1_host.cpp`: the objects of the framework that the driver uses, which record what the driver asks of them), with dmodemd.exe on its pipe. It does a check of these items:
  - The class factory, the device, its interface and its queue.
  - The symbolic link and SERIALCOMM value of the COM port, made and removed.
  - One handle at a time.
  - AT commands through the reads, writes and IOCTLs of the driver.
  - A read cancelled while the port holds it, and a read cancelled at the time that it is marked cancelable.
  - `WaitCommEvent`.
  - No object of the DLL remains at the end.
- `win7_imports` (Windows): the programs and drivers load on Windows 7 ([above](#windows-7)).
- `winmodem_inf`: the two INFs are what `make_inf.py` makes.

### The USB modems' tests

`dmodemd_usb_windows` and `dmodemd_pico_line_windows` (label `alsa`) run dmodemd.exe under Wine. They are the `usb_test.py` and `pico_line_test.py` of Linux. They do a check of all functions of each device: the calls of the two modems through the exchange, rings, caller ID, BUSY, pulse dialling, voice, the line settings of the country and unplugging.

The WinUSB of Wine has no isochronous transfers, as the WinUSB of Windows 7 has none. Thus, `pico_line_test.py` also does a check of the behaviour of Windows 7 and 8 there:
- A driver that cannot stream (the `FAKE_USB_NO_ISO` of the fake) is reported, with what to do.
- UsbDk is used when it is installed (`FAKE_USB_USBDK`), and not with `DMODEM_USBDK=0`.
- The note of `--usb list`.

How the tests are configured:
- **The fake devices:** `dmodemd_fake_st7554.exe` and `dmodemd_fake_pico_line.exe` are dmodemd.exe with the fake device of the test linked in place of libusb, because Windows has no `LD_PRELOAD`. (The build also makes `dmodemd_fake_pico_wb.exe`, but no Windows test runs it now.)
- **The DTE:** a TCP connection.
- **The exchange:** the `line_exchange` of the Linux build (`DMODEM_HOST_LINE_EXCHANGE`, by default `build/dmodemd/line_exchange`). The tests are registered only in a cross build, and only when that file is there. The Winsock of Wine has no Unix sockets. Thus, the tests relay the TCP connections of the fakes to the sockets of the exchange (`frontend/dmodemd/test/usb_dte.py`).
- **Serial:** the tests run one at a time (`RUN_SERIAL`).

## How it is done

The modem core and the DSP needed no changes other than the build. MSVC needed `std::call_once` in place of `pthread_once`. The few GCC attributes and builtins are only for GCC and Clang. Also:
- `_USE_MATH_DEFINES`, `NOMINMAX`, and the C99 printf of MinGW (`__USE_MINGW_ANSI_STDIO`).
- The `modem_get_param` of the pump interface returns `intptr_t` (`dsp/modem_param.h`), because a `long` on 64-bit Windows is 32 bits.

The other changes are in the front end of dmodemd (`frontend/dmodemd/portable.h`). The `select()` of Windows accepts only sockets. Thus, each descriptor of the main loop and the SIP thread is a socket:
- `socketpair()` becomes a pair of connected loopback TCP sockets.
- read, write and close become recv, send and closesocket.
- The reader threads of the COM port and the pipe write to such a pair. The changes of the DTE (DSR, the client of the pipe) go in-band, escaped (`ThreadDte` in `frontend/dmodemd/windows_dte.h`).

Two differences of Windows needed care:
- On Windows, the peer does not see a plain close after data that it has not read. Thus, the hang-up of the modem closes its audio socket with a reset (`SO_LINGER`). Then the SIP side ends a call that still rings.
- A TCP read can split a 16-bit sample. Thus, the audio reader keeps the odd byte for the next read (`frontend/dmodemd/sip_modem.cpp`).

Wine never completes an overlapped serial read that waits for the first character. Thus, the COM reader uses reads that return immediately, and sleeps 5 ms when the port has no data (`READ_POLL_MS` in `frontend/dmodemd/windows_dte.cpp`).
