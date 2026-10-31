# The SmartUSB56 under Windows

The Smart Link SmartUSB56 is a USB modem that `dmodemd --usb` can drive
(see [Softmodem hardware](hardware.md)). D-Modem's support for it began
from Smart Link's Linux driver. This page is the result of studying what
Smart Link's **Windows** driver does differently, using the version
Creative shipped for its Modem Blaster V.92 USB (DE5671) in 2002. The
driver was studied by reading its files, without running it.

For the device's registers, the driver's internals and the method, see
[the technical details](smartusb56-windows-details.md).

## What D-Modem took from it

The Windows driver does several things the Linux driver did not.
`dmodemd` now does them too:

| Feature | How to turn it on |
|---|---|
| Caller ID | always on (`AT#CID=1`) |
| The newer ST75951 "silicon DAA" version of the hardware | automatic |
| Carrier and data lights, on boards that have them | `--usb-leds` |
| Waking the computer when the phone rings | `--usb-wake` |
| Noticing a phone on the modem's PHONE socket being picked up | `--usb-handset` |
| A second codec for a handset, on boards that have it | `--usb-voice-codec=<law>` |
| The board's buzzer as the speaker | `--usb-buzzer` |
| Extra line controls needed in a few countries (France, Algeria, Tunisia, Australia) | `--usb-international` |
| An unexplained hardware setting the driver turns on | `--usb-rhom` |

These have been tested on a simulated device, not yet on real hardware.

## What is still unknown

- What a few of the board's control lines do electrically.
- Which boards have the second codec, and which sound format it uses.
- What the RHOM setting is for.
- How the driver produces caller ID's date, number and name labels.
