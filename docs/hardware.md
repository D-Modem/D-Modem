# Softmodem hardware

Usually, the daemon of D-Modem, `dmodemd`, makes its calls over the internet
(SIP). It can also use a real telephone line, through modem hardware
that you connect to your computer. Many of these modems were "soft modems". These are
cheap hardware that only connects to the line, and the computer does all the
work of the modem. Their original drivers were closed, and most of them operated only on
old 32-bit PCs. D-Modem replaces these drivers with its own open modem, which runs
on any processor.

For each chip, option and setting, refer to
[the technical details](hardware-details.md).

## What hardware works

| Hardware | How to use it | Status |
|---|---|---|
| **AC'97 modems** inside PCs and laptops from about 2000 to 2006 | `dmodemd -a hw:1` | tested on simulated hardware |
| **HD-audio modems** in laptops from about 2005 to 2010 | `dmodemd -a hw:0,6` | tested on simulated hardware |
| **Smart Link SmartUSB56** and other USB modems on its ST7554 chip (for example, the Modem Blaster V.92 USB of Creative) | `dmodemd --usb=auto` | tested on simulated hardware |
| **pico-serial Line**, a USB-C phone-line stick made for D-Modem | `dmodemd --usb=auto` | refer to the [hardware project](https://github.com/D-Modem/hardware) |
| **pico-serial WB**, a stick for a bare pair of wires (not a phone line) | `dmodemd --usb=auto` | tested on a simulated pair |
| **Agere USB 2.0 soft modem** | `dmodemd --usb=auto` | work in progress: refer to [its own page](agere-usb.md) |
| **Apple USB Modem** (A1101, for Macs) | `dmodemd --usb=auto` | tested on simulated hardware: refer to [its own page](apple-usb-modem.md) |
| **Conexant USB HSF modems** (the "Cadmus 2" bridge, USB `0572:1300` and others; plain boards, not SmartDAA ones) | `DMODEM_CD2_FIRMWARE=c2firmware.h dmodemd --usb=auto` | tested on simulated hardware: refer to [the details](hardware-details.md#conexant-usb-hsf-modems) |

The PC modems (`-a`) use the sound drivers of Linux (ALSA). Thus, they operate
on Linux only. The USB modems do not need a kernel driver. `dmodemd`
communicates with them directly, on Linux, [Windows](windows.md),
[macOS](macos.md) and [Haiku](haiku.md).

These modems are not supported, because there is no access to their sound:

- Modems with their own PCI controller chip (Smart Link SmartPCI, PCTel HSP, Lucent,
  Conexant HSF PCI, Broadcom).
- HD-audio modems that are not Si3054-compatible (for example, the modems of Conexant).

## Finding your modem

For a PC modem, look for a "Modem" device:

    aplay -l                      # "card 1, device 0" means hw:1

An HD-audio modem is part of the sound card. Thus, it shows as, for
example, `hw:0,6`. Some HD-audio modem chips are compatible, but Linux does not
recognise them. The `hda-modem-setup` tool corrects that.

For a USB modem:

    dmodemd --usb list

To run `dmodemd` as an ordinary user and not as root, give that user
access to the device with a udev rule (the
[technical details](hardware-details.md) have examples).

## What you get

- **Speeds** up to V.90 and V.92: 56000 bit/s down and up to 48000 bit/s
  up from the V.92 server of an internet provider. With all other modems, V.34 (33600 bit/s) or
  slower. Keep the line rate at the default 9600 Hz
  (`-r 9600`). At 8000 Hz, V.90 does not operate reliably.
- **Fax** (class 1) and **voice**.
- **Dialling** by tone or pulse (`ATDP`). The modem waits for a dial tone first.
- **Rings and caller ID** while on hook.
- **The rules of your country** (`-c UK`, or `AT+GCI`): dial pulses, busy
  tones, and on some chips the electrical settings of the line.

## Limits

- We tested the code on simulated hardware and a simulated phone
  exchange. We did not test it on a real modem chip or a real exchange yet.
- We tested the V.90 and V.92 modes only against the server side of D-Modem.
  We did not test them against the server of an internet provider yet.

## Testing without hardware

The `alsa` test label runs two copies of `dmodemd` on simulated modem cards
and simulated USB modems. The two copies call each other through a simulated exchange:

    ctest --test-dir build -L alsa
