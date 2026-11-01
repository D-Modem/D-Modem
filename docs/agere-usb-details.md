# The Agere USB 2.0 soft modem: technical details

This is the technical companion to [The Agere USB 2.0 soft modem](agere-usb.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

Agere Systems' USB soft modem (later LSI's) is USB `047E:2892`. Its
Linux driver also takes `047E:2828`, and so does dmodemd. It is sold
under other names: one copy came in a "Hiro" box (the H50113) with
Agere's own drivers.

It is not a controller modem. The modem itself (V.92, fax, voice) runs
in the host's driver: `AGRSM.sys` on Windows, `agrmodem.ko` on Linux. The
device is an Agere "USS" USB 2.0 controller, with an ARM core and a mask
ROM, and behind it a Mercury codec or a "Polaris" DAA (data access
arrangement, the line interface). dmodemd drives it through libusb
(`dmodemd --usb`, `frontend/dmodemd/usb_agere.cpp`), as it does the SmartUSB56
([hardware.md](hardware-details.md#agere-usb-20-soft-modem)). It needs a
USB 2.0 (high speed) port.

## Where this comes from

This page says what the device expects. It was worked out from three
sources, all from the Hiro box's package:

- **The device's ROM**, read out of the first device with command 2: ROM
  1.2.1, 16 KB at address 0, mirrored up to 64 KB.
- **Agere's Windows driver**, `AGRSM.sys` 2.2.98.
- **Agere's Linux driver**: agrsm 2.1.73's `agrmodemlib.o` (an unstripped
  i386 object whose symbols name its functions) and its glue
  `agrsoftmodem.c`.

The function names below are the Linux driver's. Addresses in the ROM
and in `AGRSM.sys` are given in each one's own address space.

dmodemd streams the samples the way Windows' driver does: on the ROM's
own isochronous endpoints, with no code downloaded into the device.

> **Status: not yet working reliably on a real device.** The
> [first device](#the-first-device) was first run on bulk endpoints, as
> Agere's Linux driver has it, and gave no usable samples. The notes
> below on the isochronous stream and on the transmit gain come from
> later runs on it, which reached the remote modem but were heard badly
> there. The current stream and start-up have been tested in full only
> against the fake device.

## The stream

Windows' driver reads a `UseBulk` setting
(`...\SERVICES\ISOUSB\Parameters`), which its INF does not set. So at
high speed it takes interface 0's alternate setting 1, and the ROM
streams the samples itself.

### The endpoints

Configuration 1, interface 0:

| Alternate setting | Endpoints |
|---|---|
| 0 | none |
| 1 | isochronous IN 0x82 and OUT 0x02, 16 octets, `bInterval` 2: at high speed a packet every 2 microframes, 4000 a second |
| 2 | bulk IN 0x81 and OUT 0x01 (512 octets), interrupt IN 0x82 (16 octets, `bInterval` 16) |
| 3 | as 1, with `bInterval` 1 |

### Starting and stopping it

**Command 0x0c** starts the stream (`AGRSM.sys` at 0x1e0e0, the ROM's
handler at 0x2a78). It is an OUT request with `wValue` 8, `wIndex` 1 and
8 octets of data: four 16-bit words.

| Octets | Windows' value at high speed | What the ROM does with it |
|---|---|---|
| 0–1 | 2 | an empty IN packet is (this + 2) × 2 octets |
| 2–3 | 0 | the receive ring's write index once streaming |
| 4–5 | 24 | the transmit ring's: 24 of its 48 samples of silence ahead of the codec |
| 6–7 | 0x2aaa | written to `0x90000040`; it looks like the microframes served |

At full speed (`0x90000018` not 1), Windows sends 8, 0, 24, 0, with
24-octet packets. dmodemd does not do full speed.

The ROM then:

1. clears codec register 0x0ac and leaves the codec's page at 2;
2. takes (0x2d4 & 7) + 1 samples per codec interrupt (2 after either
   codec's start-up);
3. sends two IN packets of silence;
4. turns on two interrupts: 25, the codec's (handler at 0x4ac), and 17,
   the data endpoints' (0x578).

**Command 0x0d** stops the stream (handler at 0x2b02: the two interrupts
turned off).

### What the ROM does with the samples

**The codec interrupt** moves n samples (as above) from the codec's data
register `0xe0000100` into a receive ring of 32 samples, and n samples
from a transmit ring of 48 to the codec. So the samples move at the
codec's clock, 8000 a second.

**Each OUT packet** is taken by the endpoint interrupt:

| Octets | Contents |
|---|---|
| 0–1 | n, the number of samples in it (more than 48: none are taken) |
| 2–3 | not looked at |
| 4… | n samples, S16 little-endian, into the transmit ring |

**One IN packet** is queued for each OUT packet taken, holding what the
receive ring has:

| Octets | Contents |
|---|---|
| 0–1 | n, the number of samples (at most 10); 0xaa55 if there are none, with no samples following |
| 2–3 | a status word: bit 0 is the ring detector (the ROM's copy of codec register 0x0a8's bit 2) |
| 4… | n samples, S16 little-endian |

So the host paces the packets, and the samples' rate is the codec's.

### Keeping the rings in step

The ROM's rings check nothing. Their indices simply wrap: a full
transmit ring is written over, and an empty one is played again. An IN
packet can take up to 10 samples (ROM at 0x940), which is 24 octets, more
than the 16 octets of a high speed packet. A backlog of 7 or more samples
in the receive ring (a few slots without an OUT packet, about 0.75 ms)
therefore makes a packet that the host takes for babble, and the stream
stops. That is what happened to the first device's stream, 38 s in, with
3 transfers in flight.

So dmodemd keeps every slot filled, as Windows' driver does (AGRSM.sys
0x1ded8, 0x1ba37):

- **16 transfers of 24 packets (6 ms)** are in flight each way
  (`DMODEM_AGERE_TRANSFERS`, 2 to 32; Windows uses 32).
- **An OUT transfer is sent again as soon as it completes.** Each one
  holds 48 samples, 2 a packet: the codec's own pace. That way the
  codec's transmit ring keeps the lead of 24 that command 0x0c gave it.
- **The drift correction.** The USB's clock and the codec's are not quite
  the same. dmodemd tracks how far the ring's lead has strayed: the OUT
  samples less the IN samples of the transfers paired in order (the k-th
  OUT transfer with the k-th IN one, which cover the same packets'
  times). While that, together with the corrections still in flight, is
  more than 3 samples off, each new OUT transfer carries 47 or 49
  samples (its last packet 1 or 3) instead of 48.

Windows' driver does it differently: it makes a transfer 46 or 50 by the
sign of the samples sent less those received at each transfer. That
count jumps by a transfer's 48 as IN and OUT transfers complete in either
order. Its corrections therefore go either way at random, and the ring's
lead wanders past its 24. On the first device that way, the V.8 call
menu went out chopped, and the remote never answered it.

**The playback buffer.** The playback starts with the transfers' silence
plus three frames (60 ms) to spare, so that a frame of the modem's that
comes late leaves no gap in what goes to the line. If the modem has no
samples ready, silence is sent (counted in the log). If the playback gets
more than seven frames ahead (after a stall has been caught up), the
excess is dropped.

The fake device (`test/fake_agere.cpp`) models the transmit ring, its
overruns and underruns, and a codec clock running off the USB's
(`FAKE_AGERE_DRIFT`, in ppm).

### The function table

The ROM's main routine (0x1616) sets the codec interface up at boot
(0x1416). Then it calls entries 4 and 5 of a table of functions at
`0x70000a64`, for ever.

At boot (from 0x3970) the table is 0x14af, 0x14af, 0x221c, 0x14af,
0x14b1, 0x14af, 0x14af, 0x14af:

- entry 4 is the ROM's handler of its deferred commands (0x14b0);
- entry 2 is its own;
- the others are an empty function.

Agere's drivers rewrite entries with ARM code of their own (command 9):

- the Linux driver loads a "bulk patch" (entries 4 and 7), which moves
  the samples between the codec and the bulk endpoints itself;
- the Windows driver loads an LED patch (entry 4: the ROM's handler,
  then the LED).

The samples need neither. A patch stays until the device is reset, so
before it starts the stream, dmodemd puts back any entry that differs
from the ROM's table (and logs it).

## Commands

Everything except the samples is a vendor request on endpoint 0
(`Usb_Command`, `x_usb_command`):

- `bRequest` is the command;
- `wIndex` and `wValue` are its arguments;
- `wLength` is up to 1200 (0x4b0).

Commands 1, 2, 4, 6 and 10 are IN (`bmRequestType` 0xc0); the others are
OUT (0x40). The ROM's table of handlers is at 0x2a2c (indexed by command
− 1).

| Command | Direction, length | Index, value | What it does |
|---|---|---|---|
| 0x01 | IN 2 | 0, 0 | The ROM's version, `0x0MmP` (major, minor, patch). Linux's driver reads it and then uses 0x121 regardless. Windows' keeps it, with 0 meaning unknown (0xffff), and so does dmodemd. |
| 0x02 | IN n | address's low half, high half | Reads the ARM's memory. |
| 0x03 | OUT n | address's low half, high half | Writes the ARM's memory. |
| 0x09 | OUT n | slot, 0 | Loads code into the ARM's memory and into the function table. dmodemd does not use it. |
| 0x0a | IN 2 | register, 0 | Reads a codec register. |
| 0x0b | OUT 0 | register, value | Writes a codec register. |
| 0x0c | OUT 8 | 1, 8 | Starts the stream ([above](#starting-and-stopping-it)). |
| 0x0d | OUT 0 | 0, 0 | Stops the stream. |
| 0x10 | OUT 0 | mode, 0 | Calls the function table's entry 7 (a patch's; the ROM has nothing there). |
| 0x11 | OUT 0 | 1, divider | `USS_CMD_CUSTOM_ASYNC`: calls entry 6 (ROM 1.0.6's clock change, which is a patch of Agere's). |
| 0x12 | OUT 0 | 1, divider | `USS_CMD_CHANGE_CLK` (every ROM except 1.0.6): sets the ARM's clock; 0xf lowers it, 3 raises it. |

The codec's registers are memory at `0xe0000000` plus the register's low
octet. The register's high octet is the page, at `0xe0000048`. Commands
0x0a and 0x0b do this addressing for the host.

The ARM's memory that the drivers touch:

| Address | What it is |
|---|---|
| `0x30000000` | GPIO directions: 0xffff at start (`U2InitGpio`) |
| `0x30000004` | GPIO inputs (`UsbReadGPIO`) |
| `0x30000008` | GPIO outputs (`UsbWriteGPIO`). Agere's drivers set bits 2 and 3 (the reference design's READY/DATA and OH LEDs, unlit when set) at start and on stop. |
| `0x70000a64` | the ROM's function table, 8 words ([above](#the-function-table)) |
| `0x90000018` | the USB controller's speed: 1 is high speed |
| `0xcd000008` | the PLL's state: 0 when not locked |
| `0xcd00000c` | ARMCDR, the ARM clock's divider: the clock is 480 / (2 × ARMCDR + 2) MHz. Set to 2 by hand when the PLL is not locked. |
| `0xcd000030` | bits cleared at start: bit 3 (`U2InitGpio`), bits 2 and 3 (`CUsbInit`), bit 2 (`UsbMerInitCodec`) |
| `0xcd000034` | 2 written at start |

## Start-up

This follows Agere's drivers (`uss_probe`, `Usb_HandleStartDevice`, then
`CUsbInit` and `CUsbStart`), as `usb_agere.cpp` does it:

1. **Speed.** If the device is not at high speed, dmodemd says it needs
   a USB 2.0 port and stops.
2. **Configuration.** `SET_CONFIGURATION` 1, the ROM's one configuration
   (its descriptors are at 0x39bc and 0x39dc). The answer is ignored.
   (The Linux driver sends 2 with `bmRequestType` 0x0b: its arguments to
   `usb_control_msg` are the wrong way round.)
3. **Alternate setting.** Interface 0's alternate setting 1, the
   isochronous endpoints. If that fails (a stream left over from the last
   run), dmodemd selects setting 0 and then 1 again.
4. **The ROM's version** (command 1). `DMODEM_AGERE_ROM=<hex>` overrides
   what the device says.
5. **Windows' first step at start** (AGRSM.sys 0x1e820 with mode 1, from
   0x1e530; Linux's `U2SetRing`, which only its Polaris code calls):
   `0xcd000030` bit 2 cleared and `0xcd000044` = 3, then codec registers
   0x0e0 = 0x0304, 0x0a4 = 0x0004 and 0x0b0 = 0x000a. Nothing else writes
   0xcd000044 or 0x0b0. Register 0x0b0 reads 0xbb80 (48000) from
   power-on, and Windows writes that back at stop. It reads back as
   written, but writing it did not change the first device's rate.
6. **The GPIOs and the clock.** `0xcd000030` bit 3 cleared,
   `0xcd000034` = 2, `0x30000000` = 0xffff. The LED bits (`0x30000008`
   bits 2 and 3) set, and the Hiro's OH LED (bit 4) cleared. The clock
   lowered (command 0x12 with 0xf). This is skipped on a ROM 1.0.6, whose
   clock change is Agere's own patch code, and on a ROM whose version is
   not known: its clock is left as it is.
7. **Which codec** (`CUsbInit`): `0xcd000030` bits 2 and 3 cleared,
   `0xcd000034` = 2, then codec register 0xc0 read. If its bit 15 is set,
   the codec is a "Polaris" DAA (`U2Pol*`; Linux's `CUsbInit` at d61e2,
   Windows' 0x27950). Otherwise it is a Mercury (`UsbMer*`); a read that
   fails counts as 0, a Mercury. The Hiro H50113's is a Polaris: 0xc0
   reads 0x8000. (The ROM's own boot code writes Polaris values: 0x0d0 =
   0x0fd2, 0x0e0 = 0x0307, 0x0f4 = 0xff, at 0x1416.)
8. **The codec's start-up**, a Polaris's or a Mercury's
   ([below](#a-polariss-start-up)).
9. **The LED probe**, if asked for (`DMODEM_AGERE_LED_PROBE`,
   [below](#the-line)).
10. **The stream.** The function table put back as the ROM has it
    ([above](#the-function-table)), the clock raised (divider 3; if the
    PLL is not locked, ARMCDR = 2 and the PLL's bit 1 set by hand),
    command 0x0d, then 0x0c, then the transfers.

While it runs, dmodemd drives the LEDs ([below](#the-line)). On close it
sends command 0x0d, puts the line on hook, sets GPIOs 2 and 3 again as
Agere's `CUsbStop` leaves them, puts the OH LED out, and selects
alternate setting 0.

### A Polaris's start-up

This is `U2PolInitCodec` (Windows' 0x1f330), then `U2PolConfigureCountry`
(Windows' 0x1f720):

1. `0xcd000030` bits 2 and 3 cleared, `0xcd000034` = 2, `0xcd00003c` = 5.
2. Codec registers: 0x0ac = 0, 0x048 = 2, 0x2c0 = 7, 0x2c4 = 0xfffe, 0x2d4
   = 0x1411, 0x2d0 = 0x9f02, 0x2cc = 0x01c0, 0x2cc = 0x41c0, 0x048 = 0,
   0x0b4 = 0x11ff.
3. **Its link**, repeated until 0x0c4's bit 2 says it is up (500 ms at
   most): 0x0a4 = 0; `0xcd00003c` = 7, 5 ms, then 5, 5 ms; 0x0d0 =
   0x2fd2, 5 ms, then 0x1fd2, 0xd7d2, 0x17d2, 5 ms.
4. **Its calibration**: 16 readings (out of 100 at most) of 0x0dc's bits
   10 to 14 that are not 0, Gray coded. Their mean + 3 is n. If 16
   readings were found and 7 < n < 20, 0x0d0 = `(0x17d2 & 0xf83f) |
   (n | 0x20) << 6` (n into bits 6 to 10); otherwise 0x0d0 = 0xdfd2, then
   0x1fd2.
5. 0x08c = 0, then `U2SetRing(2)`: start-up step 5 again, but with 0x0a4
   = 0 and 0x0b0 = 0xbb80. Then on hook.
6. **The country's DAA registers.** DAA registers are written as `0x8000
   | reg << 8 | value` to codec register 0x0c8. To read one, `reg << 8`
   is written to 0x0c8 and, 5 ms later, 0x0cc is read; its high octet is
   `reg` once the DAA has answered (10 tries). The writes: 1 = 0x80, 1 =
   1, 2 = 4, 5 ms, its revision (register 0x16) read, 5 = 0x50 (0x54 if
   the revision is 2 or less), 6 = 0x0a, 7 = 0x0a, 0x1b = 1, codec 0x0e0
   = 0x0304, 4 = 0. (Not done yet: the pulse dialling's timings, 0x0e4
   and 0x0e8, and the country's register 3.)
7. Caller ID on (DAA register 2 = 8).

### A Mercury's start-up

This is `UsbMerInitCodec`, the same in Windows' driver. First
`0xcd000030` bit 2 is cleared and `0xcd000034` = 2. Then the codec
register writes, in order:

1. 0x0b4 = 0x1387; 0x0a4 |= 0x0004.
2. 0x0e0 = 0x0304, 0x0e4 = 0x0070, 0x0e8 = 0x042f, 0x0f0 = 0x2242, 0x048
   = 2, 0x2c0 = 0x01ff, 0x2c4 = 0xfaac, 0x2d4 = 0x1411, 0x2d0 = 0x9f02,
   0x2cc = 0x01c0, 0x048 = 3, 0x3cc = 1, 0x3c0 = 0x187c, 0x3d0 = 0, 0x3cc
   = 0x0101, 0x3c4 = 0x3dd0, 0x3c8 = 0.
3. Two gains, each written once register 0x2dc's bit 0 says the codec is
   ready (200 ms at most): 0x3d0 = 0x10, 0x3c4 = 0xc990 (`MerGain1`);
   then 0x3d0 = 8, 0x3c4 = 0x3b0e (`MerGain2`).
4. 0x048 = 2, 0x2cc = 0x41c0, 0x048 = 0, 0x0c0 = 0, 0x08c = 0.

`CUsbInit` then puts the line on hook (0x0c0 = 1). dmodemd writes 1 at
once instead of 0: going off hook for a moment at start would look like a
dialled pulse to an exchange.

## The line

- **Hook.** Codec register 0xc0. A Mercury's is 0 off hook and 1 on hook
  (`UsbMerSetHook`); a Polaris's is the other way round, 1 off hook
  (`U2PolSetHook`). Pulse dialling and the hook flash break and make it.

  A Polaris's caller ID path is DAA register 2: 8 on hook, 4 off hook.

  Off hook, Agere's drivers also write the ROM's ring indices,
  `0x70001020` = {24, 0} (two words, command 3 with 8 octets). dmodemd
  does not: on the first device that stopped the stream's samples, and
  then its commands. (The indices are four words from `0x7000101c`.)
- **Ring.** On a Mercury, the ring is bit 0 of the IN status word, which
  is the ROM's copy of codec register 0x0a8's bit 2 (ROM at 0x948). Both
  drivers take the ring from that bit alone.

  A Polaris does not set that bit. Its ring detector is 0x0a8's bit 1:
  on the first device, 0x0a8 read 0x0010 on hook, and 0x0012 or 0x0013
  while the line rang. 0x0c4's bit 3 is set too, its bits 6 and 7 are
  the ring's two halves, and the samples go to full scale.

  So, on hook, dmodemd reads 0x0a8 (and a Polaris's 0x0c4) every 100 ms.
  It gives the core a ring pulse at each read while the codec's ring bit
  is set (a Polaris's bit 1, a Mercury's bit 2), as well as for each IN
  packet whose status has bit 0 set. Three guards keep it from false
  rings:
  - after a call, the first device's 0x0a8 kept bit 1 set with no ring.
    So a Polaris's ring counts only while both of 0x0c4's bits 6 and 7
    (the ring's halves) have been seen set within the last 500 ms;
  - nothing on 0x0a8 counts as a ring in the first 2 s after a call
    goes back on hook;
  - a ring bit set for longer than 6 s (no cadence rings that long) is
    taken as stuck, and ignored until it clears.

  The fake device's `FAKE_AGERE_RING_STICKS` makes a Polaris's ring bit
  stick in the same way.
- **Caller ID.** The Mercury's `EnableCallerID` does nothing, so the
  codec hears the line on hook. A Polaris's caller ID path is on while
  on hook (above). Either way, caller ID comes in the samples, as on an
  ALSA codec.
- **Country.** The Mercury's `ConfigureCountry` does nothing in this
  driver; a Polaris gets its default registers (above). dmodemd's `-c`
  and `AT+GCI` set only the core's rules (dialling, tones).
- **Loop current.** Fixed in this driver (`UsbMerCheckLoopCurrent`
  returns 20).
- **LEDs.** The Hiro H50113's board (the FCC's AS560USB) has two LEDs:
  D8 "READY/DATA" and D6 "OH".
  - Agere's drivers set GPIOs 2 and 3 (`0x30000008`) at start and on
    stop (`U2SetEnumerationLED`, `CUsbStop`), and Windows' LED patch
    blinks those two. They are the reference design's LEDs, but not this
    board's.
  - The first device's LED probe (below) lit OH on GPIO 4, while the
    GPIO is set. dmodemd lights it off hook, blinks it fast (125 ms on,
    125 ms off) while the line rings, and puts it out at close.
  - READY/DATA lit on none of the GPIOs probed. (The directions read
    0x00ffffff as a word, so there are 24.) Nothing in either driver or
    in the ROM drives another LED: `U2SetEnumerationLED` sets GPIOs 2 and
    3, and the ROM writes `0x30000008` only in its boot-time parallel
    loader. It seems to be wired to the power or the USB connection, not
    to the software.
  - `DMODEM_AGERE_LED_PROBE=1` (or a build with `DMODEM_AGERE_LED_PROBE`
    defined) flips each GPIO output, 0 to 31, in turn at start, writing
    whole words. Each stays flipped 1.5 s, with 0.5 s between steps;
    `DMODEM_AGERE_LED_PROBE=<ms>` makes each step that long (with a gap of
    a third of that). Each step is logged (`Agere: LED probe step N:
    ...`), with what was written and read back. The step that lights an
    LED tells which output drives it.

The samples are S16 little-endian, mono, at 8000 Hz. (The host driver's
sample rate converter takes only 8000 Hz: `ConverterSetSampleRate`.)
dmodemd runs the line at 8000 Hz whatever `-r` says. After 10 s of
samples it logs their rate (`Agere: N samples a second, from X to Y`,
with their range), and logs an error if the rate is more than 5% off
8000. With `-d1` or more it logs the controller's clock registers and
GPIOs, codec registers 0x080, 0x0a4, 0x0a8, 0x0ac, 0x0b0, 0x0b8, 0x0c0,
0x2d4 and 0x2dc, and the ROM's ring indices, both as the stream starts
and 10 s later (`Agere: streaming: ...`, `Agere: after 10 s: ...`).

The first device's stream came at 2457 samples a second, all -32768
(0x8000, straight from the codec's data register `0xe0000100`). That was
the same with the stream above and with the bulk patch, at either ARM
clock: the codec is not sampling at 8000 Hz. Start-up step 5, the one
write that Windows' driver makes and dmodemd did not make at the time,
did not change that. Codec register 0x0c0 reads with bit 15 set (0x8001
on hook), and 0x080 reads 0x1f40 (8000).

## Bring-up

    dmodemd --usb=auto -d2 dmodem0

On Linux the device needs read and write access: run as root, or add a
udev rule (`/etc/udev/rules.d/60-agere-usb.rules`):

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

On Windows it needs WinUSB (`dmodem-driver usb dmodem-usb.inf`, or
Zadig) in place of Agere's driver
([windows.md](windows-details.md#usb-modems---usb)).

### What the log shows

With `-d2`, dmodemd logs each step: the endpoints, the ROM's version,
the codec, every vendor command and its result, the ARM's clock, any
function table entry put back, and every change of the status word.

Every 10 s it logs the stream's counts (`Agere: N samples in, N out,
lead ...`): the samples each way, the ring's lead, the OUT transfers made
longer or shorter, the packets of silence, and the IN packets that
failed, came short, or had an odd count. If no samples have come for
500 ms it logs `Agere: its samples stopped ...` once.

### Settings for debugging

| Setting | What it does |
|---|---|
| `DMODEM_AGERE_DUMP=1` | Once streaming, also logs the configuration's endpoints and the ARM's memory as hex: the ROM from 0 (64 KB, until a read fails), its RAM (`0x70000000`, 8 KB), and the controller's, the codec's and the endpoints' registers. |
| `DMODEM_AGERE_TRANSFERS=<n>` | The transfers in flight each way, 2 to 32 (16 by default). |
| `DMODEM_AGERE_ROM=<hex>` | Takes this as the ROM's version instead of what command 1 says. |
| `DMODEM_AGERE_LED_PROBE=1` or `=<ms>` | The LED probe ([above](#the-line)). |
| `DMODEM_USB_TX_GAIN=<dB>` | Any USB modem: makes the modem's samples to the line louder or softer (-20 to 20 dB), clipped at full scale (the clipping said once in the log). |
| `DMODEM_USB_TX_GAIN=auto` | Any USB modem: sets that gain for each call from the remote's answer tone (below). |
| `DMODEM_USB_REC=<path>` | Any USB modem: records each call (below). |

**A fixed transmit gain.** The first device's remote heard it badly
(V.34's MP sequence from the remote asked for 7200 bit/s at most, and
V.22bis lost the carrier), while the first device's own received samples
came in clean. It sent at about -21 dBFS.

**The automatic transmit gain** (`DMODEM_USB_TX_GAIN=auto`) is set from
the remote's answer tone: 2100 Hz, heard while the modem is quiet (that
is, on the calling side), the median of 10 frames.

- On an Agere modem: a tone heard at -14.8 dBFS, as the first device's
  remote was (where +6 and +10 dB connected at 19200 bit/s), gives +8 dB.
  Each dB softer (a longer line) adds half a dB, within 0 to +12 dB.
- On the other USB modems (the SmartUSB56 and the pico-serial sticks):
  the SmartUSB56's STLC7550 is far louder (+12 dB reached a USR Courier at
  -6 dBm, clipping), so a tone at -14.8 dBFS gives -2 dB, within -12 to
  +6 dB.
- Until a tone is heard (on the answering side), the gain is the one
  from the last call's tone; with no tone yet, +6 dB (-2 dB on the other
  USB modems).
- For each second with more than 8 samples clipped, the gain goes
  softer: the second's loudest sample to 1 dB below full scale, at least
  a dB. Not once the call is online: the remote's receiver, trained on
  the level it had, would lose the data until a retrain (the clipping is
  logged instead).

Both the gain and the tone's level are logged.

**Recording a call** (`DMODEM_USB_REC=<path>`) records each call, from
off hook to on hook, to `<path>-<date>-<time>.wav`. It is stereo at the
line's rate: the left channel is the line's samples as they came, and the
right the modem's as it sent them.

Useful to send if it does not work: that log, with the dump (and a
call's recording).

### The first device

The first device was used from an Android phone, through D-Modem for
Android. It has ROM 1.2.1, a Polaris codec, and runs at high speed. At
first dmodemd drove it as a Mercury, because it had the test of register
0xc0's bit 15 the wrong way round.

dmodemd then streamed on alternate setting 2 with Agere's Linux bulk
patch, as `agrmodemlib.o` does. Its command 0x10 takes its mode in
`wIndex` (`Usb_StartIsoDma` sends it that way). Samples came, but at
2459 a second whatever the ARM's clock, and all -32768. The patch polls
the codec's FIFOs from the ROM's main loop with the codec's interrupt
off, and the codec's samples did not come through it.

Reading the ROM and Windows' driver showed the stream described above,
which needs no patch. On it the samples were the same, 2457 a second of
-32768: the Polaris had been given the Mercury's start-up, and its link
was never pulsed up. dmodemd now gives a Polaris its own start-up
([above](#a-polariss-start-up)).

Not done yet: full speed; a Polaris's country-specific DAA settings
(its pulse timings and register 3); and the voice and speakerphone
parts.
