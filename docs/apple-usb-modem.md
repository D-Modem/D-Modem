# The Apple USB Modem

Apple sold a small white USB modem (model A1101, part MA034Z/A, USB ID
`05ac:1401`) for Macs that no longer had a modem built in. Like other
soft modems it only connects to the phone line: the modem itself ran as
driver software on the Mac, and only on Mac OS X. D-Modem's daemon,
`dmodemd`, can now drive it directly with its own open modem, with no
kernel driver.

How the device works was worked out by reading Apple's own drivers for
it, from Mac OS X 10.4.11. For all of that, see
[the technical details](apple-usb-modem-details.md).

> **Status:** tested on a simulated device only. Nobody has tried it on
> a real Apple USB Modem yet. If you have one, please try it and send
> the log (`-d3`).

## Using it

    dmodemd --usb=auto -d2 dmodem0

On Linux, to run as an ordinary user, give the user access to the
device with a udev rule in `/etc/udev/rules.d/60-apple-usb-modem.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="1401", MODE="0660", GROUP="dialout"

Linux's sound driver takes the modem's audio part when it is plugged in.
`dmodemd` takes it back by itself while it runs.

On Windows, give the device the WinUSB driver first, with Zadig
(see [Windows](windows.md)). On macOS, its own audio driver may hold the
modem's audio part; that has not been tried.

## What you get

The same as the other USB modems ([Softmodem hardware](hardware.md)):
V.90 and V.92, V.34, fax and voice, tone and pulse dialling, rings and
caller ID on hook, and your country's line settings (`-c`, `AT+GCI`).

## If it does not work

- `-d3` logs every USB request to the modem and its answers, and the
  modem's interfaces and sample rates as it reports them.
- If `dmodemd` says the rate is not one the modem has, it lists the
  rates it has: try another with `-r` (8000 or 9600).
- `DMODEM_USB_REC=<path>` records each call to a WAV file, so you can
  hear what the modem heard and sent.
