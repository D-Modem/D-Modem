# The Agere USB 2.0 soft modem

Agere Systems (later LSI) made a small USB modem, USB ID `047e:2892`.
Companies sold it under different brand names. As with other soft modems,
the hardware only connects to the telephone line. The modem (V.92, fax,
voice) was driver software on the computer. The daemon of D-Modem,
`dmodemd`, can operate it directly with its own open modem, with no
kernel driver.

We found how the device operates from its built-in software and from the
Windows and Linux drivers of Agere. For all of these data, refer to
[the technical details](agere-usb-details.md).

> **Status:** It does not operate reliably on a real device now. At
> first, the device that we tried did not send usable sound. Then we
> changed the driver to operate as the Windows driver of Agere does.
> After this change, calls on the device got to the far end, but the far
> end heard them badly. We tested the latest version only on a simulated
> device.

## How to use it

A USB 2.0 (high-speed) port is necessary.

    dmodemd --usb=auto -d2 dmodem0

On Linux, to run as a usual user, give the user access to the device.
Use a udev rule in `/etc/udev/rules.d/60-agere-usb.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

On Windows, first replace the driver of Agere with WinUSB
(refer to [Windows](windows.md)).

## If it does not work

These settings help you to find the cause:

- `-d2` logs each step of the start of the device.
- `DMODEM_AGERE_DUMP=1` also logs the memory of the device.
- `DMODEM_USB_REC=<path>` records each call to a WAV file. You can then
  listen to the audio that went in and out.
- `DMODEM_USB_TX_GAIN=<dB>` (or `auto`) makes the modem louder or softer
  on the line. The first device was too quiet. The far end did not hear
  it well.

Send the log (with the dump, and a recording if you made one) to us. We
can then repair the problem.
