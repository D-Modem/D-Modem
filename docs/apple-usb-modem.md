# The Apple USB Modem

Apple sold a small white USB modem (model A1101, part MA034Z/A, USB ID
`05ac:1401`) for Macs that did not have an internal modem. This modem is
a soft modem, as other soft modems are. It only connects to the phone line.
The modem itself operated as driver software on the Mac, and only on Mac OS X.
Now, the daemon of D-Modem, `dmodemd`, can operate it directly with its own
open modem. A kernel driver is not necessary.

We found how the device operates from the drivers of Apple
for it, from Mac OS X 10.4.11. For all of the details, refer to
[the technical details](apple-usb-modem-details.md).

> **Status:** we tested it only on a simulated device. Nobody tried it on
> a real Apple USB Modem yet. If you have one, please try it and send
> the log (`-d3`).

## Using it

    dmodemd --usb=auto -d2 dmodem0

On Linux, to run as an ordinary user, give the user access to the
device with a udev rule in `/etc/udev/rules.d/60-apple-usb-modem.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="1401", MODE="0660", GROUP="dialout"

When you connect the modem, the sound driver of Linux takes the audio part of the modem.
While `dmodemd` runs, it takes the audio part back automatically.

On Windows, first give the WinUSB driver to the device, with Zadig
(refer to [Windows](windows.md)). On macOS, the audio driver of macOS can possibly hold the
audio part of the modem. We did not try this.

## What you get

You get the same features as with the other USB modems ([Softmodem hardware](hardware.md)):

- V.90 and V.92, V.34, fax and voice.
- Tone and pulse dialling.
- Rings and caller ID on hook.
- The line settings of your country (`-c`, `AT+GCI`).

## If it does not work

- `-d3` records in the log each USB request to the modem and its answers. It also records
  the interfaces and sample rates of the modem, as the modem reports them.
- If `dmodemd` tells you that the modem does not have the rate, it gives the
  rates that the modem has. Try a different rate with `-r` (8000 or 9600).
- `DMODEM_USB_REC=<path>` records each call to a WAV file. Then you can
  hear what the modem received and sent.
