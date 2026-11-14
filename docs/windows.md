# D-Modem on Windows

The daemon of D-Modem, `dmodemd`, runs on Windows 7 and later as one
`dmodemd.exe`. It does not need other installed items. It is the same modem as
on Linux. It makes calls over SIP (internet telephony), it sends and receives data,
fax and voice, and it can operate the USB modems that D-Modem supports.

For build options, all the methods to connect a program to it, and how
the port operates, refer to [the technical details](windows-details.md).

> **Status:** the Windows version is built and tested automatically,
> under Wine on Linux and on a Windows build server (the
> Windows Server 2022 machines of GitHub). Nobody ran it manually on a
> desktop Windows yet, or with a real USB modem.

## Building

You can build it on Linux for Windows:

    sudo apt-get install cmake ninja-build pkg-config g++-mingw-w64-x86-64-posix clang lld
    cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake
    cmake --build build-win

Or you can build it on Windows, from a Developer Command Prompt for Visual Studio 2022:

    cmake -B build-msvc -G Ninja
    cmake --build build-msvc

With the two methods, you get `dmodemd.exe`, one file with no extra DLLs.

## Running

Set your SIP account, then start the daemon:

    set SIP_LOGIN=username:password@sip.example.com
    dmodemd.exe -d1

The firewall of Windows asks one time if it can let the daemon use the network. The daemon
must have network access for SIP calls.

## Connecting a program to the modem

On Linux, a program communicates with D-Modem through a serial device. Windows does
not have such a device built in. Thus, `dmodemd.exe` gives four other methods:

| Option | Use it for |
|---|---|
| `--tcp` (the default, `127.0.0.1:2323`) | terminal programs that can open a raw TCP connection, for example PuTTY or `telnet` |
| `--com COM5` | programs that must have a COM port, with a virtual null-modem pair from [com0com](https://sourceforge.net/projects/com0com/) |
| `--pipe <name>` | the COM port of a virtual machine (Hyper-V, VirtualBox, VMware), so that an old guest system can dial |
| `--driver auto` | the virtual modem driver of D-Modem (refer to the subsequent section) |

### The virtual modem driver

D-Modem includes a Windows driver. With this driver, D-Modem shows in Device Manager
as a real modem, "D-Modem Virtual Modem". Then the dial-up
networking of Windows, Windows Fax and Scan, and all programs that use a modem can
use D-Modem directly. com0com is not necessary.

    dmodem-driver install dmodem.inf      (once, as an administrator)
    dmodemd.exe --driver auto

The driver runs as a user-mode driver. Thus, a bug in it cannot cause Windows to crash. There
is a version for Windows 10 and later, and a version for Windows 7, 8 and 8.1.
Nobody tried it on Windows itself yet.

## USB modems

`dmodemd.exe` operates the SmartUSB56, the pico-serial sticks and the
Agere USB modem (refer to [Softmodem hardware](hardware.md)). They must have the
generic WinUSB driver, not the drivers of their manufacturers:

    dmodem-driver usb dmodem-usb.inf      (once, as an administrator)
    dmodemd.exe --usb list
    dmodemd.exe --usb auto

On Windows 7 and 8, use libusbK or UsbDk instead of WinUSB.

## What is not available on Windows

These items are not available on Windows:

- The Asterisk and FreeSWITCH modules.
- `dmodem-ari`.
- The PC modem chips (`-a`, which is for Linux only).
- The tests against the modems of other companies.
