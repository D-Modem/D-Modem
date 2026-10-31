# The Agere USB 2.0 soft modem

Agere Systems (later LSI) made a small USB modem, USB ID `047e:2892`,
that was sold under several brand names. Like other soft modems, the
hardware only connects to the phone line; the modem itself (V.92, fax,
voice) ran as driver software on the computer. D-Modem's daemon,
`dmodemd`, can drive it directly with its own open modem, with no kernel
driver.

How the device works was worked out by reading its built-in software and
Agere's Windows and Linux drivers. For all of that, see
[the technical details](agere-usb-details.md).

> **Status:** not yet working reliably on a real device. At first the
> device tried did not send usable sound. The driver has since been
> changed to work the way Agere's Windows driver does; calls on the
> device then reached the far end, but were heard badly there. The
> latest version has so far been tested only on a simulated device.

## Using it

It needs a USB 2.0 (high-speed) port.

    dmodemd --usb=auto -d2 dmodem0

On Linux, to run as an ordinary user, give the user access to the
device with a udev rule in `/etc/udev/rules.d/60-agere-usb.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

On Windows, replace Agere's driver with WinUSB first
(see [Windows](windows.md)).

## If it does not work

These settings help find out why:

- `-d2` logs each step of starting the device.
- `DMODEM_AGERE_DUMP=1` also logs the device's memory.
- `DMODEM_USB_REC=<path>` records each call to a WAV file, so you can
  listen to what went in and out.
- `DMODEM_USB_TX_GAIN=<dB>` (or `auto`) makes the modem louder or softer
  on the line. The first device was too quiet for the far end to hear
  well.

Please send the log (with the dump, and a recording if you made one) so
the problem can be fixed.
