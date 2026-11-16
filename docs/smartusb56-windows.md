# The SmartUSB56 under Windows

The Smart Link SmartUSB56 is a USB modem that `dmodemd --usb` can operate
(refer to [Softmodem hardware](hardware.md)). The support of D-Modem for it started
from the Linux driver of Smart Link. This page gives the result of a study of the
differences in the **Windows** driver of Smart Link. We used the version that
Creative supplied for its Modem Blaster V.92 USB (DE5671) in 2002. We studied the
driver from its files only. We did not run it.

For the registers of the device, the internal parts of the driver and the method, refer to
[the technical details](smartusb56-windows-details.md).

## What D-Modem took from it

The Windows driver does some things that the Linux driver did not do.
Now, `dmodemd` also does them:

| Feature | How to enable it |
|---|---|
| Caller ID | `AT#CID=1` (disabled until you enable it) |
| The newer ST75951 "silicon DAA" version of the hardware | automatic |
| Carrier and data lights, on boards that have them | `--usb-leds` |
| Wake the computer when the phone rings | `--usb-wake` |
| Find when a person lifts a phone on the PHONE socket of the modem | `--usb-handset` |
| A second codec for a handset, on boards that have it | `--usb-voice-codec=<law>` |
| The buzzer of the board as the speaker | `--usb-buzzer` |
| More line controls that some countries must have (France, Algeria, Tunisia, Australia) | `--usb-international` |
| A hardware setting that the driver enables, with no known function | `--usb-rhom` |

We tested these features on a simulated device. We did not test them on real hardware yet.

## What is still unknown

- The electrical function of some of the control lines of the board.
- Which boards have the second codec, and which sound format it uses.
- The function of the RHOM setting.
- How the driver makes the date, number and name labels of caller ID.
