# D-Modem on Windows

D-Modem's daemon, `dmodemd`, runs on Windows 7 and later as a single
`dmodemd.exe` that needs nothing else installed. It is the same modem as
on Linux: it makes calls over SIP (internet telephony), handles data,
fax and voice, and can drive the USB modems D-Modem supports.

For build options, every way of connecting a program to it, and how
the port works, see [the technical details](windows-details.md).

> **Status:** the Windows version is built and tested automatically,
> both under Wine on Linux and on a Windows build server (GitHub's
> Windows Server 2022 machines). Nobody has yet run it by hand on a
> desktop Windows, or with a real USB modem.

## Building

You can build it on Linux for Windows:

    sudo apt-get install cmake ninja-build pkg-config g++-mingw-w64-x86-64-posix clang lld
    cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake
    cmake --build build-win

Or on Windows, from a Developer Command Prompt for Visual Studio 2022:

    cmake -B build-msvc -G Ninja
    cmake --build build-msvc

Either way you get `dmodemd.exe`, one file with no extra DLLs.

## Running

Set your SIP account, then start the daemon:

    set SIP_LOGIN=username:password@sip.example.com
    dmodemd.exe -d1

Windows' firewall will ask once whether to let it use the network; it
needs that for SIP calls.

## Connecting a program to the modem

On Linux a program talks to D-Modem through a serial device. Windows has
no such thing built in, so `dmodemd.exe` offers four ways instead:

| Option | Use it for |
|---|---|
| `--tcp` (the default, `127.0.0.1:2323`) | terminal programs that can open a raw TCP connection, such as PuTTY or `telnet` |
| `--com COM5` | programs that want a COM port, using a virtual null-modem pair from [com0com](https://sourceforge.net/projects/com0com/) |
| `--pipe <name>` | a virtual machine's COM port (Hyper-V, VirtualBox, VMware), so an old guest system can dial out |
| `--driver auto` | D-Modem's own virtual modem driver (below) |

### The virtual modem driver

D-Modem includes a Windows driver that makes it appear in Device Manager
as a real modem, "D-Modem Virtual Modem". Windows' own dial-up
networking, Windows Fax and Scan, and any program that uses a modem can
then use D-Modem directly, with no com0com needed.

    dmodem-driver install dmodem.inf      (once, as an administrator)
    dmodemd.exe --driver auto

It runs as a user-mode driver, so a bug in it cannot crash Windows. There
is a version for Windows 10 and later and one for Windows 7, 8 and 8.1.
It has not yet been tried on Windows itself.

## USB modems

`dmodemd.exe` drives the SmartUSB56, the pico-serial sticks and the
Agere USB modem (see [Softmodem hardware](hardware.md)). They need the
generic WinUSB driver instead of their makers' drivers:

    dmodem-driver usb dmodem-usb.inf      (once, as an administrator)
    dmodemd.exe --usb list
    dmodemd.exe --usb auto

On Windows 7 and 8, use libusbK or UsbDk instead of WinUSB.

## What is not available on Windows

The Asterisk and FreeSWITCH modules, `dmodem-ari`, the PC modem chips
(`-a`, which is Linux only), and the tests against other companies'
modems.
