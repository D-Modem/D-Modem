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

**Not yet tried on the device.** The IN/OUT pacing is inferred from the driver and the
patches, not seen on the wire. The sample rate (8000 Hz) is what the firmware's own clock patch
times ([below](#what-the-patches-do)). The [first run](#bring-up) reports both.

## The bulk patch: Agere's, shipped under Agere's licence

The ROM streams its samples on isochronous endpoints. Agere's drivers patch it, at every start,
so that the samples go on bulk endpoints instead: a "bulk patch" of ARM code downloaded into its
RAM. The Linux driver always does this (`gUseBulk` = 1); Windows does it when the `UseBulk`
registry value is set. The patches are Agere's code. D-Modem ships them unaltered in
[`firmware/agere/`](../firmware/agere/NOTICE.md), as data files that dmodemd reads at run time:
**their binary contents are subject to Agere's licence** (`firmware/agere/LICENSE`), not
D-Modem's. dmodemd takes them from there (installed: `<prefix>/share/dmodem/firmware/agere`; on
Windows `firmware\agere` beside dmodemd.exe; or the source tree's), or from the directory
`DMODEM_AGERE_FIRMWARE` names, or from a copy of Agere's driver (`--usb-agere-driver`). The four
are byte for byte the same in every driver of the package, with no relocations in them:

| patch | octets | what it is for |
|---|---|---|
| `bulk_patch_v106` | 748 | ROM 1.0.6 |
| `bulk_patch_v121_flash` | 748 | ROM 1.2.1, the function table's entry 0 bit 29 set (flash) |
| `bulk_patch_v121_rom` | 1156 | ROM 1.2.1 otherwise |
| `async_clockchange` | 508 | ROM 1.0.6's clock change command (`USS_CMD_CUSTOM_ASYNC`) |

dmodemd knows each by its length and two hashes (FNV-1a of its first 16 octets and of all of
it: `dmodemd/agere_patch.h`), and finds it in whatever file it is given: `AGRSM.sys` and
`AGRSM64.sys` of 2.1.70 to 2.2.98 (Windows 98 to 7) and agrsm 2.1.73's `agrmodem.ko` and
`agrmodemlib.o` all have all four. A directory is searched with everything under it.

### What the patches do

This was read from the patches' disassembly; nothing was run.

**The file.** Each patch is a 12-octet header and then ARM code: u32 the load address
(`0x70001140` for all four), u16 the code's length, u16 the sum of its octets mod 65536 (a
checksum: it matches for all four), and u32 the address the function table's entry 4 is to get.
The ROM takes the file with command 9 (index 4) and copies the code into its RAM at the load
address; command 9 with index 7 then points entry 7 at the patch's second function.
`bulk_patch_v106` and `bulk_patch_v121_flash` differ in 3 octets only, the ROM addresses they
call back into. `bulk_patch_v121_rom` is the same logic with the status reports and the LED
added.

**Entry 4: the samples, one at a time.** The function the ROM calls for each sample first calls
the ROM's own routine (the patch extends it), then, while streaming, moves one sample each way
between the codec's serial port and the USB endpoints' FIFOs:

- **IN:** when the IN FIFO has room, a 100-octet packet begins: in one piece at high speed, in
  64 and 36 octets (30 and 18 samples) at full speed. Its header is `48 | status << 16`, then
  two codec samples to a 32-bit word.
- **The status word:** if the host has asked for device registers (a mask of 25, through command
  0x10, below), the next one: its index + 1 in the high octet, its value in the low one (the
  Polaris DAA's states, the host driver's `udev_info`). Otherwise 1 while the codec interface's
  ring detect line is set, else 0: the ring bit.
- **OUT:** when the OUT FIFO has data, the 4-octet header is read and **dropped**, and 48 samples
  (30 and 18 at full speed) go to the codec as its transmit FIFO has room. The device does not
  look at the OUT count: every OUT transfer is played as 48 samples, so dmodemd always sends 48.
- **The LED** (GPIO bits 2 and 3): on, blinking slowly or blinking fast, as a mode word says.
- When the stream stops, both endpoints' FIFOs are flushed.

**Entry 7: command 0x10.** The ROM leaves the host's command 0x10 in a mailbox (`0x70000e24`),
and this function answers it: 1 starts the stream (the endpoints' modes for high or full speed,
the counters cleared), 2 stops it, 3 adds a register (its index the argument) to those reported
in the status words, 4 clears them; then it marks the command done.
It takes the mode from the low half of the mailbox's second word (`0x70000e28`) and the
argument from its high half, and marks the command done by writing 2 to that word. It keeps the
stream's state at `0x70000a0c`: 2 streaming (start), 1 stopped. The three bulk patches have the
same two addresses. The mode is the setup packet's `wIndex` (the first device streams with it
there, and not with it in `wValue`).

**Entry 4's samples come at the codec's pace.** IN: while the codec's receive FIFO has data
(`0xe00000d8` & 0xf), a sample from its data register (`0xe0000100`), two to a word into the IN
endpoint's FIFO. OUT: while its transmit FIFO has room (`0xe00000d8` & 0xf00 below 0x800), a
sample from the OUT endpoint's FIFO to the codec. So the samples' rate is the codec's.

**The clock change patch** (`async_clockchange`, ROM 1.0.6's command 0x11, ARM code): it waits
for the PLL to lock, then sets the divider in ARMCDR and runs the ARM from the PLL, or falls
back to the 30 MHz crystal. It then works out the ROM's timing constants from the new clock:
ticks per millisecond and **ticks per 1/8000 s**. The firmware times its samples at 8000 Hz.

## The ROM's own isochronous stream (Windows')

Windows' `AGRSM.sys` (2.2.98, from the Hiro box's package) streams on the ROM's isochronous
endpoints unless `UseBulk` is set, which its INF does not do: at high speed it takes interface 0's
alternate setting 1, and alternate setting 2 (bulk) only with `UseBulk`. The ROM does the
streaming itself; no bulk patch. This was read from the ROM (16 KB at 0, mirrored to 64 KB, read
with command 2 from the first device: ROM 1.2.1) and from `AGRSM.sys`, and not yet run.

**The endpoints** (configuration 2, interface 0): alternate setting 1, isochronous IN 0x82 and OUT
0x02, 16 octets a packet, `bInterval` 2 (at high speed a packet every 2 microframes: 4000 a
second); alternate setting 3, the same with `bInterval` 1; alternate setting 2, bulk 0x81 and
0x01 (512) and interrupt 0x82 (16, `bInterval` 16).

**Its start** (`AGRSM.sys` 0x1e0e0): command 0x0c, OUT, `wValue` 8, `wIndex` 1, 8 octets:

| octets | high speed (`0x90000018` reads 1) | otherwise | what the ROM does with it (0x2a78) |
|---|---|---|---|
| 0–1 | 2 | 8 | `0x70000a04`: an empty IN packet is (n + 2) × 2 octets |
| 2–3 | 0 | 0 | the receive ring's write index once streaming |
| 4–5 | 24 | 24 | the transmit ring's write index once streaming: 24 of its 48 samples of silence ahead |
| 6–7 | 0x2aaa | 0 | written to `0x90000040` (the microframes it serves, as it looks) |

The ROM then clears codec register 0x0ac, leaves the codec's page at 2, takes (0x2d4 & 7) + 1
samples a codec interrupt (2 after the Mercury's start-up), sends two IN packets of silence, and
turns on interrupts 25 (the codec: 0x4ac) and 17 (the data endpoints: 0x578). Command 0x0d stops
it (0x2b02: the two interrupts off). Its transfers (0x1d0..): 24 packets of 12 octets each way at
high speed, 6 of 24 otherwise. Before it, Windows downloads a 380-octet patch into slot 4 (command
9) that only drives the LED (its command 0x10 modes 5, 6 and 7); the samples do not need it.

**The codec interrupt** (0x4ac): n samples (above) from the codec's data register `0xe0000100`
into a receive ring of 32, and n from a transmit ring of 48 to it.

**The OUT packet**, on each one the endpoint interrupt (0x578, with `0x70000f40` 2) handles:

| octets | what |
|---|---|
| 0–1 | n, the samples in it (more than 48: none) |
| 2–3 | not looked at |
| 4… | n samples, S16 little-endian, into the transmit ring |

**The IN packet**, one queued for each OUT packet taken, of what the receive ring holds:

| octets | what |
|---|---|
| 0–1 | n, the samples (at most 10); 0xaa55 if there are none, and no samples |
| 2–3 | a status word: bit 0 the ring detector (codec register 0x0a8 bit 2) |
| 4… | n samples, S16 little-endian |

So the host paces the stream with its OUT packets (2 samples each at 4000 a second at high
speed), and the samples' rate is the codec's: the host sends back as many as it has been sent,
as dmodemd does for the ST7554's packets.

**The ROM otherwise.** Its main routine (0x1616) sets the codec interface up at boot (0x1416:
0x0a4 |= 4, 0x0f4 = 0xff, 0x0d0 = 0x0fd2, 0x0e0 = 0x0307) when `0xcd000018` & 3 is 2 (kept at
`0x70000f40`), then calls the function table's entries 4 and 5 for ever: the bulk patch's
per-sample function runs from there, polling the codec's and the endpoints' FIFOs with the two
interrupts off. Its commands (the table at 0x2a2c, by command − 1): 1 the version, 2 and 3 memory,
9 a patch, 0x0a and 0x0b the codec, **0x0c and 0x0d its isochronous stream**, 0x10 the function
table's entry 7 (nothing of its own: the bulk patch's hook), 0x11 entry 6, 0x12 the clock. The
codec's registers are memory at `0xe0000000` + the register's low octet, its high octet the page
(`0xe0000048`). The entries the patches do not take are 0x14af, an empty function.

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
| 0x10 | OUT 0 | mode, 0 | the samples: 1 at full speed before the transfers start; at high speed 4 then 1 after they start. The mode goes in `wIndex`: the ROM leaves `wIndex` in the patch's mailbox (seen on the first device; `wValue` there does nothing) |
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

S16 little-endian, mono, at 8000 Hz: the clock change patch times the samples at 8000 Hz, the
host driver's sample rate converter takes only 8000 Hz (`ConverterSetSampleRate`: 8000,
resampled to 7200 inside), and the AC'97 build of the same library runs its codec at 8000 Hz. dmodemd runs the line at 8000 Hz
whatever `-r` says, and checks the rate once (below).

Each bulk transfer is 100 octets each way (`uss_write`, `urb_complete`, `x_usb_copyplaybuf`):

| octets | IN | OUT |
|---|---|---|
| 0–1 | the samples' count (48; up to 50 is accepted; `0xaa55` a marker, no samples) | the samples' count (ignored by the device: 48 are played) |
| 2–3 | a status word: bit 0 the ring detector (`UsbMerRingDetected`). For a Polaris, the high octet indexes a table of the device's states (`udev_info`) | 0 |
| 4… | the samples | the samples, the rest zeros |

Agere's driver expects 96 samples (two transfers) a period, 12 ms at 8000 Hz. When fewer come
it sends fewer back (`gSendLessSamples`: packets of 46 instead of 48), so the playback keeps
the device's clock. dmodemd does it in its own way. It sends one OUT transfer of 48 samples for
each IN transfer done, so the playback keeps the device's clock, as it does for the ST7554's
isochronous packets. The first two OUT transfers are silence.

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

**The first device (an Android phone, ROM 1.2.1, a Mercury codec, high speed):** everything up
to command 0x10 goes as above, and the function table shows the patch in. With command 0x10's
mode in `wValue` no IN transfer ever came back: the stream's state stayed 0. With it in `wIndex`
the state is 2 and the samples come, but at 2459 a second, not 8000, and every one of them
-32768 (0x8000), off hook as well. 2459 is 8000 × 24.576 / 80 to within the measurement: the
ARM runs at 80 MHz there (ARMCDR 2 after USS_CMD_CHANGE_CLK with 3), and 24.576 MHz is an audio
master clock. The rate is the same at every ARM clock from 16 to 80 MHz (dividers 2 to 0xf), so
it is the codec's own. Its registers 0x2d4 and 0x0e0 read back otherwise than written (0x0401
for 0x1411, 0x0337 for 0x0304); 0x3c0 to 0x3cc read 0 with 0x048 back at 0. Not yet known: what
sets the codec's rate, and why its samples are all 0x8000.

dmodemd reads the mailbox and the stream state back after command 0x10 and logs them (`-d1`).
If the state is not 2 it says so, sends command 0x10 again with the mode in `wValue`, and logs
whether that started the stream. `DMODEM_AGERE_CLOCK_SWEEP=1` measures the samples' rate (and
their range) at several ARM clocks while streaming at start, and logs each.
Agere's Linux driver (agrsm 2.1.73's `agrmodemlib.o`, from the Hiro box's package) does what
dmodemd does: `Usb_StartIsoDma` sends command 0x10 with the mode in `wIndex` (`Usb_Command`'s
third argument; `x_usb_command` builds the setup packet by hand, `wIndex` from it), the
Mercury's start-up is the same writes, and it keeps two 100-octet transfers in flight each
way, sent again from the kernel's completion handler. The patch starts an IN packet only once
the IN FIFO has room, so a host slower than that (a user-space one, Android's) loses samples
while it is not there: `DMODEM_AGERE_TRANSFERS=n` (2 to 16) keeps n each way in flight.

With 8 transfers in flight each way the rate is the same, 2459 a second, each IN transfer
coming back 19.5 ms after the last: the device's pace, not the host's. Windows' driver reads
`UseBulk` (`...\SERVICES\ISOUSB\Parameters`), which its INF does not set, so on Windows the
device streams on the ROM's isochronous endpoints with the ROM's own command 0x10, which the
bulk patch replaces. `DMODEM_AGERE_DUMP=1` logs the descriptors and the ARM's memory (the ROM
from 0, its RAM, the controller's and the codec interface's registers) as hex, for that.

**The ROM (16 KB at 0, read with command 2).** Its main routine (0x1616) sets the codec
interface up at boot (0x1416: 0x0a4 |= 4, 0x0f4 = 0xff, 0x0d0 = 0x0fd2, 0x0e0 = 0x0307), then
calls the function table's entries 4 and 5 for ever: the bulk patch's per-sample function runs
from that loop. The codec's registers are memory at `0xe0000000` + the register's low octet, its
high octet the page (`0xe0000048`), as the ROM's commands 0x0a and 0x0b do it. Its commands
(0x2958's table): 1 the version, 2 and 3 memory, 9 a patch, 0x0a and 0x0b the codec, **0x0c its
own isochronous stream's start** (0x2a78, with data), 0x0d its stop, 0x10 the function table's
entry 7 (the patch's hook, nothing else), 0x11 entry 6, 0x12 the clock. Its codec interrupt
(0x4ac) moves the samples between `0xe0000100` and its buffers. The first device, after the
Mercury's start-up, has 0x9f02 at `0xe00000d0`, the start-up's 0x2d0: `DMODEM_AGERE_CODEC_TRY=1`
measures the rate as it is, with 0x0d0 = 0x0fd2, then with 0x0e0 = 0x0307 as well.

`DMODEM_AGERE_PROBE=1` logs the controller's registers (`0xcd000000` on), the codec
interface's (`0xe00000c0` on) and the codec's (0x000 to 0x3fc), then writes 0 to 7 to
`0xcd000034` (2 at start; what for is not known) while streaming, logs the rate at each, and
puts it back.

    dmodemd --usb=auto -d2 dmodem0

(the patches D-Modem ships; `--usb-agere-driver /path/to/AGRSM.sys` takes them from Agere's driver
instead)

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
