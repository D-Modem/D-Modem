# D-Modem on Windows

The daemon of D-Modem, `dmodemd`, runs on Windows as one `dmodemd.exe`. The
64-bit build runs on Windows 7 and later. The 32-bit build of clang-retro
(`win98`) is for Windows 98 SE, ME, 2000, XP and later. It does not need other installed items. The `win32-picolibc` build is for Windows 95, 98, ME, NT 4.0 and later. On Windows 95, it needs Winsock 2. It is the same modem as
on Linux. It makes calls over SIP (internet telephony), it sends and receives data,
fax and voice, and it can operate the USB modems that D-Modem supports.

For build options, all the methods to connect a program to it, and how
the port operates, refer to [the technical details](windows-details.md).

> **Status:** the Windows version is built and tested automatically,
> under Wine on Linux and on a Windows build server (the
> Windows Server 2022 machines of GitHub). Nobody ran it manually on a
> desktop Windows yet, or with a real USB modem. The 32-bit build for
> Windows 98 SE is only built: its imports are checked against
> Windows 98 and NT 4.0, but nobody ran it on Windows 98 SE yet. The
> same is true for the build for Windows 95 and NT 4.0.

## Building

Use the toolchain of [clang-retro](https://github.com/clang-retro/dos-sdk)
to build it for Windows. Use this toolchain on Linux or on Windows. Get the
toolchain package one time:

    git clone https://github.com/clang-retro/dos-sdk
    sh dos-sdk/packaging/get-toolchain.sh /opt/clang-dos

Then build `dmodemd.exe` for Windows 7 and later (64 bit):

    sudo apt-get install cmake ninja-build pkg-config
    cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/windows/clang-retro.cmake \
        -DCLANG_DOS=/opt/clang-dos -DCLANG_RETRO_CONFIG=win7-x64
    cmake --build build-win

`CLANG_RETRO_CONFIG` sets the oldest Windows that the program operates on.
Use `win98` for Windows 98 SE, ME, 2000, XP and later (32 bit),
`win32-picolibc` for Windows 95, NT 4.0 and later (32 bit), or
`win10-x64` for Windows 10 and later (64 bit).

You can also build it with MinGW-w64
(`-DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake`).

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

### The icon in the notification area

When `dmodemd.exe` starts, it puts an icon in the notification area (the
system tray) of the taskbar. When you put the mouse pointer on the icon,
it shows "D-Modem" and the version.

- Double-click the icon to open the log window. The log window shows all
  the text that `dmodemd.exe` writes, also the text in the console. The
  newest lines are at the bottom.
- Right-click the icon to open the menu:

| Menu item | Function |
|---|---|
| View Logs | Opens the log window. |
| About | Shows the version, the modem name and its DTE. |
| Exit | Stops the modem and closes `dmodemd.exe` (the same as Ctrl+C). |

To start `dmodemd.exe` without the icon, use `--no-tray`.

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

On Windows 98 SE, ME, 2000 and XP, use the `win98` build. Give the modem
the driver of D-Modem: `dmusb.inf` and `dmodem.sys` of the
[windows-driver](https://github.com/D-Modem/windows-driver) repository.
Then use `--usb list` and `--usb auto` as above. Nobody tried this on a
real Windows 98 SE yet.

## What is not available on Windows

These items are not available on Windows:

- The Asterisk and FreeSWITCH modules.
- `dmodem-ari`.
- The PC modem chips (`-a`, which is for Linux only).
- The tests against the modems of other companies.
