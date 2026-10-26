# The Agere USB 2.0 soft modem

Agere Systems' USB soft modem, later LSI's, is USB `047E:2892`; its Linux driver also takes
`047E:2828`. It is sold under other names: one copy came in a "Hiro" box with Agere's own
drivers. It is not a controller modem. The modem itself (V.92, fax, voice) runs in the host's
driver: `AGRSM.sys` on Windows, `agrmodem.ko` on Linux. The device is an Agere "USS" USB 2.0
controller with an ARM core and a mask ROM, and a Mercury (or "Polaris") codec and DAA behind
it. dmodemd drives it through libusb (`dmodemd --usb`, `dmodemd/usb_agere.cpp`), as it does
the SmartUSB56 ([hardware.md](hardware.md)). It needs a USB 2.0 (high speed) port.

This page says what the device wants. It was read from three things: the device's ROM (read out
of the first device with command 2: ROM 1.2.1, 16 KB at 0, mirrored to 64 KB), Agere's Windows
driver `AGRSM.sys` 2.2.98, and Agere's Linux driver (agrsm 2.1.73's `agrmodemlib.o`, an
unstripped i386 object whose symbols name its functions, with its glue `agrsoftmodem.c`), all
from the Hiro box's package. The function names below are the Linux driver's; the addresses in
the ROM and in `AGRSM.sys` are their own.

dmodemd streams the samples as Windows' driver does: on the ROM's own isochronous endpoints, with
no code downloaded into the device. **Not yet run on the device** this way: the
[first device](#the-first-device) streamed on bulk endpoints, as Agere's Linux driver has it,
which does not work (below).

## The stream

Windows' driver reads `UseBulk` (`...\SERVICES\ISOUSB\Parameters`), which its INF does not set, so
at high speed it takes interface 0's alternate setting 1 and the ROM streams the samples itself.

**The endpoints** (configuration 2, interface 0):

| alternate setting | endpoints |
|---|---|
| 0 | none |
| 1 | isochronous IN 0x82 and OUT 0x02, 16 octets, `bInterval` 2: at high speed a packet every 2 microframes, 4000 a second |
| 2 | bulk IN 0x81 and OUT 0x01 (512 octets), interrupt IN 0x82 (16, `bInterval` 16) |
| 3 | as 1, `bInterval` 1 |

**The start** (`AGRSM.sys` at 0x1e0e0, the ROM's command 0x0c at 0x2a78): command 0x0c, OUT,
`wValue` 8, `wIndex` 1, 8 octets of data, four 16-bit words:

| octets | Windows' at high speed | what the ROM does with it |
|---|---|---|
| 0–1 | 2 | an empty IN packet is (this + 2) × 2 octets |
| 2–3 | 0 | the receive ring's write index once streaming |
| 4–5 | 24 | the transmit ring's: 24 of its 48 samples of silence ahead of the codec |
| 6–7 | 0x2aaa | written to `0x90000040`: the microframes served, as it looks |

(At full speed, `0x90000018` not 1, Windows sends 8, 0, 24, 0 and 24-octet packets; dmodemd
does not do full speed.) The ROM then clears codec register 0x0ac, leaves the codec's page at 2,
takes (0x2d4 & 7) + 1 samples a codec interrupt (2 after either codec's start-up), sends two IN
packets of silence, and turns on two interrupts: 25, the codec's (0x4ac), and 17, the data
endpoints' (0x578). **Command 0x0d** stops it (0x2b02: the two interrupts off).

**The codec interrupt** moves n samples (above) from the codec's data register `0xe0000100` into
a receive ring of 32, and n from a transmit ring of 48 to it: the samples go at the codec's
clock, 8000 a second.

**The OUT packet**, each one taken by the endpoint interrupt:

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

So the host paces the packets, and the samples' rate is the codec's. dmodemd sends back as many
samples as it has been sent (at most 6 a packet, the 16 octets' room), as it does for the
ST7554's packets: the playback keeps the codec's clock. Its transfers are 24 packets (6 ms)
each way, as Windows' are, three of them in flight (`DMODEM_AGERE_TRANSFERS` 2 to 16 instead);
an OUT transfer goes again as soon as it is done, so that the ROM is never left without one.

**The function table.** The ROM's main routine (0x1616) sets the codec interface up at boot
(0x1416), then calls entries 4 and 5 of a table of functions at `0x70000a64` for ever. At boot
(from 0x3970) it is 0x14af, 0x14af, 0x221c, 0x14af, 0x14b1, 0x14af, 0x14af, 0x14af: entry 4 the
ROM's handler of its deferred commands (0x14b0), entry 2 its own, the others an empty function.
Agere's drivers rewrite entries with ARM code of theirs (command 9): its Linux driver a "bulk
patch" (entries 4 and 7), which moves the samples between the codec and bulk endpoints itself;
Windows' an LED patch (entry 4: the ROM's handler, then the LED). The samples need neither. A
patch stays until the device is reset, so dmodemd puts the table back as the ROM has it before
it starts the stream.

## Commands

Everything but the samples is a vendor request on endpoint 0 (`Usb_Command`, `x_usb_command`):
`bRequest` the command, `wIndex` and `wValue` its arguments, `wLength` up to 1200 (0x4b0).
Commands 1, 2, 4, 6 and 10 are IN (`bmRequestType` 0xc0), the others OUT (0x40). The ROM's
table of them is at 0x2a2c (by command − 1).

| command | dir | index, value | what it does |
|---|---|---|---|
| 0x01 | IN 2 | 0, 0 | the ROM's version, `0x0MmP` (major, minor, patch). Linux's driver reads it and then takes 0x121 regardless; Windows' keeps it, 0 meaning unknown (0xffff) |
| 0x02 | IN n | address's low half, high half | the ARM's memory read |
| 0x03 | OUT n | address's low half, high half | the ARM's memory written |
| 0x09 | OUT n | slot, 0 | code into the ARM's memory, and into the function table (not used) |
| 0x0a | IN 2 | register, 0 | a codec register read |
| 0x0b | OUT 0 | register, value | a codec register written |
| 0x0c | OUT 8 | 1, 8 | the stream started (above) |
| 0x0d | OUT 0 | 0, 0 | the stream stopped |
| 0x10 | OUT 0 | mode, 0 | the function table's entry 7 called (a patch's; nothing of the ROM's) |
| 0x11 | OUT 0 | 1, divider | `USS_CMD_CUSTOM_ASYNC`: entry 6 called (ROM 1.0.6's clock change, a patch of Agere's) |
| 0x12 | OUT 0 | 1, divider | `USS_CMD_CHANGE_CLK` (ROMs but 1.0.6): the ARM's clock, 0xf lowered, 3 raised |

The codec's registers are memory at `0xe0000000` plus the register's low octet, its high octet
the page (`0xe0000048`), as commands 0x0a and 0x0b do it.

The ARM's memory that the drivers touch:

| address | what |
|---|---|
| `0x30000000` | GPIO directions: 0xffff at start (`U2InitGpio`) |
| `0x30000004` | GPIO inputs (`UsbReadGPIO`) |
| `0x30000008` | GPIO outputs (`UsbWriteGPIO`); bits 2 and 3 the LEDs (READY/DATA and OH), unlit set: set at start and on stop |
| `0x70000a64` | the ROM's function table, 8 words (above) |
| `0x90000018` | the USB controller's speed: 1 high |
| `0xcd000008` | the PLL's state: 0 when not locked |
| `0xcd00000c` | ARMCDR, the ARM clock's divider: the clock is 480 / (2 × ARMCDR + 2) MHz; 2 set by hand when the PLL is not locked |
| `0xcd000030` | bits cleared at start: 3 (`U2InitGpio`), 2 and 3 (`CUsbInit`), 2 (`UsbMerInitCodec`) |
| `0xcd000034` | 2 written at start |

## Start-up

As Agere's drivers go (`uss_probe`, `Usb_HandleStartDevice`, then `CUsbInit` and `CUsbStart`),
and as `usb_agere.cpp` does it:

1. `SET_CONFIGURATION` 1, the ROM's one configuration (its descriptors at 0x39bc and 0x39dc).
   Its answer is ignored. (The Linux driver sends 2 with `bmRequestType` 0x0b: its arguments to
   `usb_control_msg` are the wrong way round.)
2. Interface 0's alternate setting 1: the isochronous endpoints.
3. Windows' first step at start (AGRSM.sys 0x1e820 with mode 1, from 0x1e530; Linux's
   `U2SetRing`, which only its Polaris code calls): `0xcd000030` bit 2 cleared, `0xcd000044` = 3, then codec registers 0x0e0 = 0x0304, 0x0a4 = 0x0004 and 0x0b0 =
   0x000a. Nothing else writes 0xcd000044 or 0x0b0 (it reads 0xbb80, 48000, from power-on;
   Windows writes that back at stop). It reads back as written, but did not change the first
   device's rate.
4. The ROM's version (command 1). The GPIOs: `0xcd000030` bit 3 cleared, `0xcd000034` = 2,
   `0x30000000` = 0xffff. The LED (`0x30000008` bits 2 and 3 set). The clock lowered (0x12
   with 0xf; not with a ROM 1.0.6, whose clock change is Agere's code, nor one whose version
   is not known: its clock is left as it is).
5. `CUsbInit`: `0xcd000030` bits 2 and 3 cleared, `0xcd000034` = 2, then codec register 0xc0
   read. With its bit 15 set the codec is a "Polaris" DAA (`U2Pol*`; Linux's `CUsbInit` at
   d61e2, Windows' 0x27950); otherwise (a read that fails reads 0) a Mercury (`UsbMer*`). The
   Hiro H50113's is a Polaris: 0xc0 reads 0x8000. (The ROM's own boot writes Polaris values:
   0x0d0 = 0x0fd2, 0x0e0 = 0x0307, 0x0f4 = 0xff, at 0x1416.)
6. A Polaris's start-up (`U2PolInitCodec`, Windows' 0x1f330): `0xcd000030` bits 2 and 3
   cleared, `0xcd000034` = 2, `0xcd00003c` = 5; codec 0x0ac = 0, 0x048 = 2, 0x2c0 = 7, 0x2c4 =
   0xfffe, 0x2d4 = 0x1411, 0x2d0 = 0x9f02, 0x2cc = 0x01c0, 0x2cc = 0x41c0, 0x048 = 0, 0x0b4 =
   0x11ff. Then its link, until 0x0c4's bit 2 says it is up (500 ms at most): 0x0a4 = 0,
   `0xcd00003c` = 7, 5 ms, 5, 5 ms; 0x0d0 = 0x2fd2, 5 ms, 0x1fd2, 0xd7d2, 0x17d2, 5 ms. Then its
   calibration: 16 readings (of 100 at most) of 0x0dc's bits 10 to 14 that are not 0, Gray
   coded; their mean + 3, n, if 7 < n < 20 into 0x0d0's bits 6 to 10 (`(0x0d0 & 0xf83f) |
   (n | 0x20) << 6`), else 0x0d0 = 0xdfd2, 0x1fd2. Then 0x08c = 0, `U2SetRing(2)` (step 3 with
   0x0a4 = 0, 0x0b0 = 0xbb80), on hook. Then the country's (`U2PolConfigureCountry`, Windows'
   0x1f720), DAA registers written as 0x8000 | reg << 8 | value to codec 0x0c8 (read: reg << 8
   to 0x0c8, 5 ms, then 0x0cc, whose high octet is reg once it answers): 1 = 0x80, 1 = 1, 2 =
   4, 5 ms, its revision (0x16) read, 5 = 0x50 (0x54 if the revision is 2 or less), 6 = 0x0a,
   7 = 0x0a, 0x1b = 1, codec 0x0e0 = 0x0304, 4 = 0. (Not done yet: the pulse dialling's timings,
   0x0e4 and 0x0e8, and the country's register 3.) Then caller ID on (DAA 2 = 8).
6. (Or) a Mercury's start-up (`UsbMerInitCodec`, the same in Windows' driver), as codec register
   writes, in order: 0x0b4 = 0x1387; 0x0a4 |= 0x0004; 0x0e0 = 0x0304, 0x0e4 = 0x0070, 0x0e8 =
   0x042f, 0x0f0 = 0x2242, 0x048 = 2, 0x2c0 = 0x01ff, 0x2c4 = 0xfaac, 0x2d4 = 0x1411, 0x2d0 =
   0x9f02, 0x2cc = 0x01c0, 0x048 = 3, 0x3cc = 1, 0x3c0 = 0x187c, 0x3d0 = 0, 0x3cc = 0x0101,
   0x3c4 = 0x3dd0, 0x3c8 = 0. Then two gains, each once register 0x2dc's bit 0 says the codec
   is ready (200 ms at most): 0x3d0 = 0x10, 0x3c4 = 0xc990 (`MerGain1`); 0x3d0 = 8, 0x3c4 =
   0x3b0e (`MerGain2`). Then 0x048 = 2, 0x2cc = 0x41c0, 0x048 = 0, 0x0c0 = 0, 0x08c = 0.
   `CUsbInit` then puts it on hook (0x0c0 = 1). dmodemd writes 1 at once instead of 0: going
   off hook for a moment at start is a dialled pulse to an exchange.
7. The function table put back as the ROM has it (above), the clock raised (divider 3),
   command 0x0d, then 0x0c, then the transfers.

While it runs, the LEDs ([below](#the-line)). On close: command 0x0d, on hook, the LED bits set
again.

## The line

- **Hook:** codec register 0xc0: a Mercury's 0 off hook and 1 on hook (`UsbMerSetHook`), a
  Polaris's the other way round, 1 off hook (`U2PolSetHook`). Pulse dialling and the hook
  flash break and make it. A Polaris's caller ID is DAA register 2: 8 on hook, 4 off hook. Off hook the drivers also write the ROM's ring
  indices, `0x70001020` = {24, 0} (two words, command 3 with 8 octets); dmodemd does not: on
  the first device that stopped the stream's samples, then its commands. (The indices are four
  words from `0x7000101c`.)
- **Ring:** bit 0 of the IN status word. dmodemd gives the core a ring pulse for each IN
  packet that has it, while on hook.
- **Caller ID:** the Mercury's `EnableCallerID` does nothing, so the codec hears the line on
  hook; a Polaris's is on while on hook (above). Caller ID comes in the samples, as on an ALSA
  codec.
- **Country:** the Mercury's `ConfigureCountry` does nothing in this driver; a Polaris's gets
  its default registers (above). dmodemd's `-c` and `AT+GCI` set the core's rules only.
- **Loop current:** fixed in this driver (`UsbMerCheckLoopCurrent`: 20).
- **LEDs:** two on the Hiro H50113's board (the FCC's AS560USB), D8 "READY/DATA" and D6 "OH".
  Agere's drivers set GPIOs 2 and 3 (`0x30000008`) at start and on stop (`U2SetEnumerationLED`,
  `CUsbStop`), and Windows' LED patch blinks those two: the reference design's LEDs, but not
  this board's. The first device's LED probe (below) lit OH on GPIO 4 while it is set; dmodemd
  lights it off hook and blinks it fast while the line rings, and puts it out at close.
  READY/DATA lit on none of GPIOs 0 to 15. The GPIOs are 24 (the directions read 0x00ffffff
  as words); 16 to 23 are still to be probed. `DMODEM_AGERE_LED_PROBE=1` (or a build with
  `DMODEM_AGERE_LED_PROBE` defined) flips each GPIO output 0 to 31 in turn at start, as words,
  1.5 s each with 0.5 s between (`=<ms>`: that long each), and logs each step (`Agere: LED
  probe step N: ...`, what was written and read back): which step lights an LED says which
  output drives it.

The samples are S16 little-endian, mono, at 8000 Hz (the host driver's sample rate converter
takes only 8000 Hz: `ConverterSetSampleRate`). dmodemd runs the line at 8000 Hz whatever `-r`
says, and after 10 s of samples logs their rate (`Agere: N samples a second`, with their range),
as an error if it is not 8000. With `-d2` it logs the controller's clock registers, codec
registers 0x080 to 0x2dc and the ROM's ring indices as the stream starts and again then
(`Agere: streaming: ...`, `Agere: after 10 s: ...`).

The first device's stream came at 2457 samples a second, all -32768 (0x8000, from the codec's
data register `0xe0000100` itself), with the stream above and with the bulk patch alike, at
either ARM clock: the codec is not sampling at 8000 Hz. Step 3, the start-up's one write that
Windows' driver makes and dmodemd did not, did not change that. Codec register 0x0c0 reads with
bit 15 set (0x8001 on hook), 0x080 0x1f40 (8000).

## Bring-up

    dmodemd --usb=auto -d2 dmodem0

On Linux the device needs read and write access, as root or with a udev rule
(`/etc/udev/rules.d/60-agere-usb.rules`):

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

On Windows it needs WinUSB (`dmodem-driver usb dmodem-usb.inf`, or Zadig) in place of Agere's
driver ([windows.md](windows.md#usb-modems---usb)).

With `-d2` dmodemd logs each step: the endpoints, the ROM's version, the codec, every vendor
command and its result, the ARM's clock, any function table entry put back, and every change of
the status word. `DMODEM_AGERE_DUMP=1` also logs, once streaming, the configuration's endpoints
and the ARM's memory as hex: the ROM from 0 (64 KB, until a read fails), its RAM
(`0x70000000`, 8 KB), the controller's, the codec's and the endpoints' registers.

Useful to send if it does not work: that log, with the dump.

### The first device

An Android phone's, through D-Modem for Android: ROM 1.2.1, a Polaris (driven at first as a
Mercury: dmodemd had the test of register 0xc0 the wrong way round), high speed. dmodemd then
streamed on alternate setting 2 with Agere's Linux bulk patch, as `agrmodemlib.o` does. Its
command 0x10 takes its mode in `wIndex` (`Usb_StartIsoDma` sends it so). The samples came, but at
2459 a second whatever the ARM's clock, and all -32768: the patch polls the codec's FIFOs from
the ROM's main loop with the codec's interrupt off, and the codec's samples did not come through
it. Reading the ROM and Windows' driver showed the stream above, which needs no patch. On it the
samples were the same, 2457 a second of -32768: the Polaris was given the Mercury's start-up,
its link never pulsed up.

Not done yet: full speed, a Polaris's country-specific DAA settings (its pulse timings and
register 3), and the voice and speakerphone parts.
