# Softmodem hardware

Normally D-Modem's daemon, `dmodemd`, makes its calls over the internet
(SIP). It can also use a real telephone line, through modem hardware
plugged into your computer. Many of these were "soft modems": cheap
hardware that only connects to the line, while the computer does all the
modem's work. Their original drivers were closed and mostly ran only on
old 32-bit PCs. D-Modem replaces them with its own open modem, which runs
on any processor.

For every chip, option and setting, see
[the technical details](hardware-details.md).

## What hardware works

| Hardware | How to use it | Status |
|---|---|---|
| **AC'97 modems** inside PCs and laptops from about 2000 to 2006 | `dmodemd -a hw:1` | tested on simulated hardware |
| **HD-audio modems** in laptops from about 2005 to 2010 | `dmodemd -a hw:0,6` | tested on simulated hardware |
| **Smart Link SmartUSB56** and other USB modems on its ST7554 chip (such as Creative's Modem Blaster V.92 USB) | `dmodemd --usb=auto` | tested on simulated hardware |
| **pico-serial Line**, a USB-C phone-line stick made for D-Modem | `dmodemd --usb=auto` | see the [hardware project](https://github.com/D-Modem/hardware) |
| **pico-serial WB**, a stick for a bare pair of wires (not a phone line) | `dmodemd --usb=auto` | tested on a simulated pair |
| **Agere USB 2.0 soft modem** | `dmodemd --usb=auto` | being brought up: see [its own page](agere-usb.md) |

The PC modems (`-a`) use Linux's own sound drivers (ALSA), so they work
on Linux only. The USB modems need no kernel driver at all: `dmodemd`
talks to them directly, on Linux, [Windows](windows.md),
[macOS](macos.md) and [Haiku](haiku.md).

Not supported, because nothing gives access to their sound: modems with
their own PCI controller chip (Smart Link SmartPCI, PCTel HSP, Lucent,
Conexant HSF PCI, Broadcom) and HD-audio modems that are not
Si3054-compatible (such as Conexant's).

## Finding your modem

For a PC modem, look for a "Modem" device:

    aplay -l                      # "card 1, device 0" means hw:1

An HD-audio modem is part of the sound card, so it shows up as, for
example, `hw:0,6`. Some HD-audio modem chips are compatible but not
recognised by Linux; the `hda-modem-setup` tool fixes that.

For a USB modem:

    dmodemd --usb list

To run `dmodemd` as an ordinary user rather than root, give that user
access to the device with a udev rule (examples are in the
[technical details](hardware-details.md)).

## What you get

- **Speeds** up to V.90 and V.92: 56000 bit/s down and up to 48000 bit/s
  up from an internet provider's V.92 server, and V.34 (33600 bit/s) or
  slower with any other modem. Keep the line rate at the default 9600 Hz
  (`-r 9600`); at 8000 Hz V.90 does not work reliably.
- **Fax** (class 1) and **voice**.
- **Dialling** by tone or pulse (`ATDP`), waiting for a dial tone first.
- **Rings and caller ID** while on hook.
- **Your country's rules** (`-c UK`, or `AT+GCI`): dial pulses, busy
  tones, and on some chips the line's electrical settings.

## Limits

- The code has been tested on simulated hardware and a simulated phone
  exchange, but not yet on a real modem chip or a real exchange.
- The V.90 and V.92 modes have been tested against D-Modem's own server
  side only, not yet against an internet provider's.

## Testing without hardware

The `alsa` test label runs two copies of `dmodemd` on fake modem cards
and fake USB modems, calling each other through a simulated exchange:

    ctest --test-dir build -L alsa
