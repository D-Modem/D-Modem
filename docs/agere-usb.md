# The Agere USB 2.0 soft modem

Agere Systems' USB soft modem, later LSI's, is USB `047E:2892`; its Linux driver also takes
`047E:2828`. It is sold under other names: one copy came in a "Hiro" box with Agere's own
drivers. It is not a controller modem. The modem itself (V.92, fax, voice) runs in the host's
driver: `AGRSM.sys` on Windows, `agrmodem.ko` on Linux. The device is an Agere "USS" USB 2.0
controller with an ARM core and a mask ROM, and a Mercury (or "Polaris") codec and DAA behind
it. dmodemd drives it through libusb (`dmodemd --usb`, `dmodemd/usb_agere.cpp`), as it does
the SmartUSB56 ([hardware.md](hardware.md)).

This page says what the device wants. It was read from Agere's drivers: agrsm 2.1.73's
`agrmodemlib.o`, an unstripped i386 object whose symbols name its functions, with the glue
`agrsoftmodem.c`, and `AGRSM.sys` 2.2.98 (Windows 7) where they differ. Both were read
statically and never run. The function names below are theirs.

**Not yet tried on the device.** The sample rate (8000 Hz) and the IN/OUT pacing are inferred
from the driver, not seen on the wire; the [first run](#bring-up) reports both.

## The bulk patch: not in D-Modem

The ROM streams its samples on isochronous endpoints. Agere's drivers patch it, at every start,
so that the samples go on bulk endpoints instead: a "bulk patch" of ARM code downloaded into its
RAM. The Linux driver always does this (`gUseBulk` = 1); Windows does it when the `UseBulk`
registry value is set. The patches are Agere's code. D-Modem does not ship them and dmodemd
takes them from the user's own copy of Agere's driver (`--usb-agere-driver`). The four are byte
for byte the same in every driver of the package, with no relocations in them:

| patch | octets | what it is for |
|---|---|---|
| `bulk_patch_v106` | 748 | ROM 1.0.6 |
| `bulk_patch_v121_flash` | 748 | ROM 1.2.1, the function table's entry 0 bit 29 set (flash) |
| `bulk_patch_v121_rom` | 1156 | ROM 1.2.1 otherwise |
| `async_clockchange` | 508 | ROM 1.0.6's clock change command (`USS_CMD_CUSTOM_ASYNC`) |

dmodemd knows each only by its length and two hashes (FNV-1a of its first 16 octets and of all
of it: `dmodemd/agere_patch.h`), and finds it in whatever file it is given: `AGRSM.sys` and
`AGRSM64.sys` of 2.1.70 to 2.2.98 (Windows 98 to 7) and agrsm 2.1.73's `agrmodem.ko` and
`agrmodemlib.o` all have all four. A directory is searched with everything under it.

## Commands

Everything but the samples is a vendor request on endpoint 0 (`Usb_Command`, `x_usb_command`):
`bRequest` the command, `wIndex` and `wValue` its arguments, `wLength` up to 1200 (0x4b0).
Commands 1, 2, 4, 6 and 10 are IN (`bmRequestType` 0xc0), the others OUT (0x40).

| command | dir | index, value | what it does |
|---|---|---|---|
| 0x01 | IN 2 | 0, 0 | the ROM's version, `0x0MmP` (major, minor, patch). Linux's driver reads it and then takes 0x121 regardless; Windows' keeps it, 0 meaning unknown (0xffff) |
| 0x02 | IN n | address's low half, high half | the ARM's memory read |
| 0x03 | OUT n | address's low half, high half | the ARM's memory written |
| 0x09 | OUT n | slot, 0 | a patch: slot 4 a bulk patch, 6 the clock change patch, 7 a bulk patch's hook (12 octets: u32 0, u16 2, u16 the word at address 0, u32 the function table's entry 7 as the patch has it) |
| 0x0a | IN 2 | register, 0 | a codec register read |
| 0x0b | OUT 0 | register, value | a codec register written |
| 0x10 | OUT 0 | 0, mode | the samples: 1 at full speed before the transfers start; at high speed 4 then 1 after they start |
| 0x11 | OUT 0 | 1, divider | `USS_CMD_CUSTOM_ASYNC` (ROM 1.0.6, with its clock change patch): the ARM's clock, 0xf lowered, 3 raised |
| 0x12 | OUT 0 | 1, divider | `USS_CMD_CHANGE_CLK` (other ROMs): the same |

The ARM's memory that the drivers touch:

| address | what |
|---|---|
| `0x30000000` | GPIO directions: 0xffff at start (`U2InitGpio`) |
| `0x30000004` | GPIO inputs (`UsbReadGPIO`) |
| `0x30000008` | GPIO outputs (`UsbWriteGPIO`); bits 2 and 3 the "enumeration" LED, set at start and on stop |
| `0xcd000008` | the PLL's state: 0 when not locked |
| `0xcd00000c` | ARMCDR, the ARM clock's divider: the clock is 480 / (2 × ARMCDR + 2) MHz; 2 set by hand when the PLL is not locked |
| `0xcd000030` | bits cleared at start: 3 (`U2InitGpio`), 2 and 3 (`CUsbInit`), 2 (`UsbMerInitCodec`) |
| `0xcd000034` | 2 written at start |
| `0x70000a64` | the ROM's function table, 8 words. A patch rewrites entries: 4 and 7 for the bulk patches (1.0.6 and 1.2.1 in flash: `0x700013f0`, `0x70001400`; 1.2.1 in ROM: `0x70001588`, `0x70001598`), 6 for the clock change patch (`0x70001140`). Entry 0's bit 29: a 1.2.1 in flash |
| `0x90002048`, `0x90004048` | the IN and OUT endpoints' FIFOs: 0x000f written to reset them (`x_usb_reset_pipe`, after `CLEAR_FEATURE(ENDPOINT_HALT)`) |

## Start-up

As Agere's Linux driver goes (`uss_probe`, `Usb_HandleStartDevice`, then `LXHardwareStart`'s
`CUsbInit` and `CUsbStart`), and as `usb_agere.cpp` does it:

1. `SET_CONFIGURATION` 2, sent as the driver sends it: `bmRequestType` 0x0b (its arguments to
   `usb_control_msg` are the wrong way round). Its answer is ignored.
2. Interface 0's alternate setting 2: a bulk IN and a bulk OUT endpoint.
3. The ROM's version (command 1). The GPIOs: `0xcd000030` bit 3 cleared, `0xcd000034` = 2,
   `0x30000000` = 0xffff. The LED (`0x30000008` bits 2 and 3 set). The clock lowered (0x12 or
   0x11 with 0xf; none if the version is unknown).
4. `CUsbInit`: `0xcd000030` bits 2 and 3 cleared, `0xcd000034` = 2, then codec register 0xc0
   read. If the read fails the codec is a "Polaris" DAA (`U2Pol*`), whose start-up has a
   calibration loop; dmodemd does not drive one yet and says so. Otherwise it is a Mercury.
5. The Mercury's start-up (`UsbMerInitCodec`), as codec register writes, in order: 0x0b4 =
   0x1387; 0x0a4 |= 0x0004; 0x0e0 = 0x0304, 0x0e4 = 0x0070, 0x0e8 = 0x042f, 0x0f0 = 0x2242,
   0x048 = 2, 0x2c0 = 0x01ff, 0x2c4 = 0xfaac, 0x2d4 = 0x1411, 0x2d0 = 0x9f02, 0x2cc = 0x01c0,
   0x048 = 3, 0x3cc = 1, 0x3c0 = 0x187c, 0x3d0 = 0, 0x3cc = 0x0101, 0x3c4 = 0x3dd0, 0x3c8 = 0.
   Then two gains, each once register 0x2dc's bit 0 says the codec is ready (200 ms at most):
   0x3d0 = 0x10, 0x3c4 = 0xc990 (`MerGain1`); 0x3d0 = 8, 0x3c4 = 0x3b0e (`MerGain2`). Then
   0x048 = 2, 0x2cc = 0x41c0, 0x048 = 0, 0x0c0 = 0, 0x08c = 0. `CUsbInit` then puts it on hook
   (0x0c0 = 1). dmodemd writes 1 at once instead of 0: going off hook for a moment at start is
   a dialled pulse to an exchange.
6. `CUsbStart` (`Usb_StartIsoDma`): `0xcd000008` read; the bulk patch for the ROM unless the
   function table shows it is in (`Uss_InitForUsingBulkEps`); the clock raised (divider 3); at
   full speed command 0x10 with 1; the endpoints' halts and FIFOs cleared; two IN transfers and
   two OUT ones submitted; at high speed command 0x10 with 4, then 1.

`CUsbStop` sends no command: the transfers stop, and the LED bits are set again.

## The samples

S16 little-endian, mono. The rate is not stated anywhere in the driver, but its sample rate
converter takes only 8000 Hz (`ConverterSetSampleRate`: 8000, resampled to 7200 inside), and the
AC'97 build of the same library runs its codec at 8000 Hz. dmodemd runs the line at 8000 Hz
whatever `-r` says, and checks the rate once (below).

Each bulk transfer is 100 octets each way (`uss_write`, `urb_complete`, `x_usb_copyplaybuf`):

| octets | IN | OUT |
|---|---|---|
| 0–1 | the samples' count (48; up to 50 is accepted; `0xaa55` a marker, no samples) | the samples' count |
| 2–3 | a status word: bit 0 the ring detector (`UsbMerRingDetected`). For a Polaris, the high octet indexes a table of the device's states (`udev_info`) | 0 |
| 4… | the samples | the samples, the rest zeros |

Agere's driver expects 96 samples (two transfers) a period, 12 ms at 8000 Hz. When fewer come
it sends fewer back (`gSendLessSamples`: packets of 46 instead of 48), so the playback keeps
the device's clock. dmodemd does the same in its own way. It sends one OUT transfer for each IN
transfer done, with that one's count of samples, as it does for the ST7554's isochronous
packets. The first two OUT transfers are silence.

## The line

- **Hook:** codec register 0xc0, 0 off hook and 1 on hook (`UsbMerSetHook`). Pulse dialling
  and the hook flash break and make it.
- **Ring:** bit 0 of the IN status word. dmodemd gives the core a ring pulse for each IN
  transfer that has it, while on hook.
- **Caller ID:** the Mercury's `EnableCallerID` does nothing, so the codec hears the line on
  hook. Caller ID comes in the samples, as on an ALSA codec.
- **Country:** the Mercury's `ConfigureCountry` does nothing in this driver (the Polaris has
  one). dmodemd's `-c` and `AT+GCI` set the core's rules only.
- **Loop current:** fixed in this driver (`UsbMerCheckLoopCurrent`: 20).

## Bring-up

    dmodemd --usb=auto --usb-agere-driver /path/to/AGRSM.sys -d2 dmodem0

On Linux the device needs read and write access, as root or with a udev rule
(`/etc/udev/rules.d/60-agere-usb.rules`):

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

On Windows it needs WinUSB (`dmodem-driver usb dmodem-usb.inf`, or Zadig) in place of Agere's
driver ([windows.md](windows.md#usb-modems---usb)).

With `-d2` dmodemd logs each step: the patches found, the ROM's version, the codec, the patch
downloaded, the ARM's clock, every vendor command and its result, and every change of the
status word. After 10 s of samples it logs their rate (`Agere: N samples a second`), and says
so as an error if it is not 8000.

Useful to send if it does not work: that log, `lsusb -v -d 047e:` (the endpoints), and a
usbmon (Linux) or USBPcap (Windows) capture of Agere's own driver starting the device.

Not done yet: the Polaris DAA, the country's DAA settings (none for the Mercury in this
driver), and the voice and speakerphone parts.
