# The Agere USB 2.0 soft modem: technical details

This page gives the technical data for [The Agere USB 2.0 soft modem](agere-usb.md), which is the overview in plain English. Read the overview first. This page gives all the data: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

The USB soft modem of Agere Systems (later of LSI) is USB `047E:2892`.
Its Linux driver also accepts `047E:2828`, and dmodemd also accepts it.
It is sold under other names. One unit came in a "Hiro" box (the H50113)
with the drivers of Agere.

It is not a controller modem. The modem function (V.92, fax, voice) runs
in the driver on the host: `AGRSM.sys` on Windows, `agrmodem.ko` on
Linux. The device is an Agere "USS" USB 2.0 controller, with an ARM core
and a mask ROM. Behind the controller is a Mercury codec or a "Polaris"
DAA (data access arrangement, the line interface). dmodemd controls it
through libusb (`dmodemd --usb`, `frontend/dmodemd/usb_agere.cpp`), as it
does the SmartUSB56
([hardware.md](hardware-details.md#agere-usb-20-soft-modem)). It must
have a USB 2.0 (high speed) port.

## Where this comes from

This page tells what the device expects. The data comes from three
sources, all from the package in the Hiro box:

- **The ROM of the device**, read from the first device with command 2:
  ROM 1.2.1, 16 KB at address 0, mirrored up to 64 KB.
- **The Windows driver of Agere**, `AGRSM.sys` 2.2.98.
- **The Linux driver of Agere**: the `agrmodemlib.o` of agrsm 2.1.73 (an
  unstripped i386 object, whose symbols give the names of its functions)
  and its glue code `agrsoftmodem.c`.

The function names on this page are the names in the Linux driver. The
addresses in the ROM and in `AGRSM.sys` are each in their own address
space.

dmodemd moves the samples as the Windows driver does: on the isochronous
endpoints of the ROM, and it does not download code into the device.

> **Status: not yet reliable on a real device.** The
> [first device](#the-first-device) first ran on bulk endpoints, as in
> the Linux driver of Agere, and gave no usable samples. The notes
> below on the isochronous stream and on the transmit gain come from
> later runs on it. These runs got to the remote modem, but the remote
> modem received the signal badly. The current stream and start-up
> procedure have had a full test only with the fake device.

## The stream

The Windows driver reads a `UseBulk` setting
(`...\SERVICES\ISOUSB\Parameters`), which its INF does not set. Thus at
high speed it selects alternate setting 1 of interface 0, and the ROM
itself moves the samples.

### The endpoints

Configuration 1, interface 0:

| Alternate setting | Endpoints |
|---|---|
| 0 | none |
| 1 | isochronous IN 0x82 and OUT 0x02, 16 octets, `bInterval` 2: at high speed one packet each 2 microframes, 4000 each second |
| 2 | bulk IN 0x81 and OUT 0x01 (512 octets), interrupt IN 0x82 (16 octets, `bInterval` 16) |
| 3 | as 1, with `bInterval` 1 |

### Starting and stopping it

**Command 0x0c** starts the stream (`AGRSM.sys` at 0x1e0e0, the handler
of the ROM at 0x2a78). It is an OUT request with `wValue` 8, `wIndex` 1
and 8 octets of data: four 16-bit words.

| Octets | The value from Windows at high speed | What the ROM does with it |
|---|---|---|
| 0–1 | 2 | an empty IN packet is (this + 2) × 2 octets |
| 2–3 | 0 | the write index of the receive ring during the stream |
| 4–5 | 24 | the same for the transmit ring: 24 of its 48 samples of silence in front of the codec |
| 6–7 | 0x2aaa | written to `0x90000040`; it is possibly the microframes served |

At full speed (`0x90000018` not 1), Windows sends 8, 0, 24, 0, with
24-octet packets. dmodemd does not use full speed.

Then the ROM does these steps:

1. It clears codec register 0x0ac and sets the codec page to 2.
2. It takes (0x2d4 & 7) + 1 samples for each codec interrupt (2 after
   the start-up of each of the two codecs).
3. It sends two IN packets of silence.
4. It enables two interrupts: 25, the interrupt of the codec (handler at
   0x4ac), and 17, the interrupt of the data endpoints (0x578).

**Command 0x0d** stops the stream (handler at 0x2b02: it disables the
two interrupts).

### What the ROM does with the samples

**The codec interrupt** moves n samples (as above) from the data
register of the codec, `0xe0000100`, into a receive ring of 32 samples.
It also moves n samples from a transmit ring of 48 to the codec. Thus
the samples move at the clock rate of the codec, 8000 each second.

**Each OUT packet** goes to the endpoint interrupt:

| Octets | Contents |
|---|---|
| 0–1 | n, the number of samples in it (more than 48: the ROM takes none) |
| 2–3 | not used |
| 4… | n samples, S16 little-endian, into the transmit ring |

**One IN packet** goes into the queue for each OUT packet that the ROM
takes. It holds the contents of the receive ring:

| Octets | Contents |
|---|---|
| 0–1 | n, the number of samples (10 maximum); 0xaa55 if there are none, with no samples after it |
| 2–3 | a status word: bit 0 is the ring detector (the copy in the ROM of bit 2 of codec register 0x0a8) |
| 4… | n samples, S16 little-endian |

Thus the host sets the rate of the packets, and the codec sets the rate
of the samples.

### Keeping the rings in step

The rings of the ROM do no checks. Their indices wrap. The ROM writes
over a full transmit ring, and plays an empty one again. An IN packet
can hold up to 10 samples (ROM at 0x940), which is 24 octets. This is
more than the 16 octets of a high speed packet. Thus a backlog of 7 or
more samples in the receive ring (some slots with no OUT packet, about
0.75 ms) makes a packet that the host thinks is babble, and the stream
stops. This occurred in the stream of the first device after 38 s, with
3 transfers in flight.

Thus dmodemd fills each slot, as the Windows driver does (AGRSM.sys
0x1ded8, 0x1ba37):

- **16 transfers of 24 packets (6 ms)** are in flight in each direction
  (`DMODEM_AGERE_TRANSFERS`, 2 to 32; Windows uses 32).
- **dmodemd sends an OUT transfer again immediately when it completes.**
  Each transfer holds 48 samples, 2 in each packet: the rate of the
  codec. Thus the transmit ring of the codec keeps the lead of 24 that
  command 0x0c gave it. A burst is the exception. When dmodemd is late
  (on a busy machine), all the packets of the device from that time
  complete at the same time. The OUT transfers then complete before the
  modem has made their samples from the related IN transfers. dmodemd
  holds a transfer until the playback has the samples for it (until the
  end of that round of libusb events). It does not send it with
  silence. It holds all transfers but 4 at most, so that the device
  always has OUT packets in its queue. (If a burst is longer than the
  spare samples and the held transfers can cover, dmodemd sends the
  transfers as they are.) Together, these steps make sure that a stall
  of about 100 ms causes no gap.
- **The drift correction.** The USB clock and the codec clock are not
  exactly the same. dmodemd monitors the error in the lead of the ring:
  the OUT samples minus the IN samples of the transfers, paired in
  sequence (the k-th OUT transfer with the k-th IN transfer, which cover
  the times of the same packets). While this error, plus the
  corrections that are in flight, is more than 3 samples, each new OUT
  transfer has 47 or 49 samples (its last packet has 1 or 3) instead of
  48.

The Windows driver uses a different method. At each transfer, it makes
the transfer 46 or 50 samples, by the sign of the samples sent minus the
samples received. This count changes by the 48 samples of one transfer
when IN and OUT transfers complete in a different sequence. Thus its
corrections go in a random direction, and the lead of the ring moves
away from 24. When the first device used this method, the V.8 call menu
went out in pieces, and the remote did not answer it.

**The playback buffer.** At the start, the playback has the silence of
the transfers plus three frames (60 ms) spare. Thus a late frame from
the modem causes no gap in the signal to the line. If the modem has no
samples ready at that time, dmodemd sends silence (and counts it in the
log). If the playback gets more than seven frames ahead (after it
recovers from a stall), dmodemd discards the extra frames.

The fake device (`test/fake_agere.cpp`) models the transmit ring, its
overruns and underruns, and a codec clock that is not the same as the
USB clock (`FAKE_AGERE_DRIFT`, in ppm).

### The function table

The main routine of the ROM (0x1616) sets up the codec interface at boot
(0x1416). Then it calls entries 4 and 5 of a table of functions at
`0x70000a64`, continuously.

At boot (from 0x3970) the table is 0x14af, 0x14af, 0x221c, 0x14af,
0x14b1, 0x14af, 0x14af, 0x14af:

- entry 4 is the handler of the ROM for its deferred commands (0x14b0);
- entry 2 is a function of the ROM;
- the other entries are an empty function.

The drivers of Agere write their own ARM code into entries (command 9):

- the Linux driver loads a "bulk patch" (entries 4 and 7), which itself
  moves the samples between the codec and the bulk endpoints;
- the Windows driver loads an LED patch (entry 4: the handler of the
  ROM, then the LED).

The samples need neither patch. A patch stays until a reset of the
device. Thus, before dmodemd starts the stream, it restores each entry
that is not the same as in the table of the ROM (and records it in the
log).

## Commands

All data other than the samples goes as a vendor request on endpoint 0
(`Usb_Command`, `x_usb_command`):

- `bRequest` is the command;
- `wIndex` and `wValue` are its arguments;
- `wLength` is up to 1200 (0x4b0).

Commands 1, 2, 4, 6 and 10 are IN (`bmRequestType` 0xc0). The other
commands are OUT (0x40). The table of handlers in the ROM is at 0x2a2c
(indexed by command − 1).

| Command | Direction, length | Index, value | What it does |
|---|---|---|---|
| 0x01 | IN 2 | 0, 0 | The version of the ROM, `0x0MmP` (major, minor, patch). The Linux driver reads it and then always uses 0x121. The Windows driver keeps it, with 0 as unknown (0xffff), and dmodemd does the same. |
| 0x02 | IN n | low half of the address, high half | Reads the memory of the ARM. |
| 0x03 | OUT n | low half of the address, high half | Writes the memory of the ARM. |
| 0x09 | OUT n | slot, 0 | Loads code into the memory of the ARM and into the function table. dmodemd does not use it. |
| 0x0a | IN 2 | register, 0 | Reads a codec register. |
| 0x0b | OUT 0 | register, value | Writes a codec register. |
| 0x0c | OUT 8 | 1, 8 | Starts the stream ([above](#starting-and-stopping-it)). |
| 0x0d | OUT 0 | 0, 0 | Stops the stream. |
| 0x10 | OUT 0 | mode, 0 | Calls entry 7 of the function table (an entry of a patch; the ROM has nothing there). |
| 0x11 | OUT 0 | 1, divider | `USS_CMD_CUSTOM_ASYNC`: calls entry 6 (the clock change of ROM 1.0.6, which is a patch from Agere). |
| 0x12 | OUT 0 | 1, divider | `USS_CMD_CHANGE_CLK` (all ROMs but 1.0.6): sets the ARM clock; 0xf decreases it, 3 increases it. |

The codec registers are memory at `0xe0000000` plus the low octet of the
register. The high octet of the register is the page, at `0xe0000048`.
Commands 0x0a and 0x0b do this addressing for the host.

The memory of the ARM that the drivers use:

| Address | What it is |
|---|---|
| `0x30000000` | GPIO directions: 0xffff at the start (`U2InitGpio`) |
| `0x30000004` | GPIO inputs (`UsbReadGPIO`) |
| `0x30000008` | GPIO outputs (`UsbWriteGPIO`). The drivers of Agere set bits 2 and 3 (the READY/DATA and OH LEDs of the reference design, off when set) at the start and at the stop. |
| `0x70000a64` | the function table of the ROM, 8 words ([above](#the-function-table)) |
| `0x90000018` | the speed of the USB controller: 1 is high speed |
| `0xcd000008` | the state of the PLL: 0 when not locked |
| `0xcd00000c` | ARMCDR, the divider of the ARM clock: the clock is 480 / (2 × ARMCDR + 2) MHz. Set to 2 manually when the PLL is not locked. |
| `0xcd000030` | bits cleared at the start: bit 3 (`U2InitGpio`), bits 2 and 3 (`CUsbInit`), bit 2 (`UsbMerInitCodec`) |
| `0xcd000034` | 2 written at the start |

## Start-up

This procedure follows the drivers of Agere (`uss_probe`,
`Usb_HandleStartDevice`, then `CUsbInit` and `CUsbStart`), as
`usb_agere.cpp` does it:

1. **Speed.** If the device is not at high speed, dmodemd tells you that
   it must have a USB 2.0 port, and stops.
2. **Configuration.** `SET_CONFIGURATION` 1, the one configuration of the
   ROM (its descriptors are at 0x39bc and 0x39dc). dmodemd ignores the
   answer. (The Linux driver sends 2 with `bmRequestType` 0x0b: its
   arguments to `usb_control_msg` are in the incorrect sequence.)
3. **Alternate setting.** Alternate setting 1 of interface 0, the
   isochronous endpoints. If this fails (because a stream from the last
   run continues), dmodemd selects setting 0 and then 1 again.
4. **The version of the ROM** (command 1). `DMODEM_AGERE_ROM=<hex>`
   replaces the value from the device.
5. **The first start step of Windows** (AGRSM.sys 0x1e820 with mode 1,
   from 0x1e530; the `U2SetRing` of Linux, which only its Polaris code
   calls): `0xcd000030` bit 2 cleared and `0xcd000044` = 3, then codec
   registers 0x0e0 = 0x0304, 0x0a4 = 0x0004 and 0x0b0 = 0x000a. No
   other step writes 0xcd000044 or 0x0b0. Register 0x0b0 reads 0xbb80
   (48000) from power-on, and Windows writes that value back at the
   stop. It reads back as written, but the write did not change the
   rate of the first device.
6. **The GPIOs and the clock.** `0xcd000030` bit 3 cleared,
   `0xcd000034` = 2, `0x30000000` = 0xffff. The LED bits (`0x30000008`
   bits 2 and 3) set, and the OH LED of the Hiro (bit 4) cleared. The
   clock decreased (command 0x12 with 0xf). dmodemd does not do this
   step on a ROM 1.0.6, whose clock change is patch code from Agere,
   and on a ROM of unknown version. On these, the clock stays as it is.
7. **Which codec** (`CUsbInit`): `0xcd000030` bits 2 and 3 cleared,
   `0xcd000034` = 2, then a read of codec register 0xc0. If its bit 15
   is set, the codec is a "Polaris" DAA (`U2Pol*`; the `CUsbInit` of
   Linux at d61e2, of Windows at 0x27950). If not, it is a Mercury
   (`UsbMer*`). A read that fails counts as 0, a Mercury. The Hiro
   H50113 has a Polaris: 0xc0 reads 0x8000. (The boot code of the ROM
   writes Polaris values: 0x0d0 = 0x0fd2, 0x0e0 = 0x0307, 0x0f4 = 0xff,
   at 0x1416.)
8. **The start-up of the codec**, for a Polaris or a Mercury
   ([below](#a-polariss-start-up)).
9. **The LED probe**, if you set it (`DMODEM_AGERE_LED_PROBE`,
   [below](#the-line)).
10. **The stream.** The function table restored to the values of the
    ROM ([above](#the-function-table)), the clock increased (divider 3;
    if the PLL is not locked, ARMCDR = 2 and bit 1 of the PLL set
    manually), command 0x0d, then 0x0c, then the transfers.

During the run, dmodemd controls the LEDs ([below](#the-line)). At
close, it sends command 0x0d, puts the line on hook, sets GPIOs 2 and 3
again as the `CUsbStop` of Agere does, turns off the OH LED, and
selects alternate setting 0.

### A Polaris's start-up

This is `U2PolInitCodec` (0x1f330 in Windows), then
`U2PolConfigureCountry` (0x1f720 in Windows):

1. `0xcd000030` bits 2 and 3 cleared, `0xcd000034` = 2, `0xcd00003c` = 5.
2. Codec registers: 0x0ac = 0, 0x048 = 2, 0x2c0 = 7, 0x2c4 = 0xfffe, 0x2d4
   = 0x1411, 0x2d0 = 0x9f02, 0x2cc = 0x01c0, 0x2cc = 0x41c0, 0x048 = 0,
   0x0b4 = 0x11ff.
3. **Its link**, done again until bit 2 of 0x0c4 shows that the link is
   up (500 ms maximum): 0x0a4 = 0; `0xcd00003c` = 7, 5 ms, then 5, 5 ms;
   0x0d0 = 0x2fd2, 5 ms, then 0x1fd2, 0xd7d2, 0x17d2, 5 ms.
4. **Its calibration**: 16 readings (of 100 maximum) of bits 10 to 14 of
   0x0dc that are not 0, Gray coded. Their mean + 3 is n. If there were
   16 readings and 7 < n < 20, 0x0d0 = `(0x17d2 & 0xf83f) |
   (n | 0x20) << 6` (n into bits 6 to 10). If not, 0x0d0 = 0xdfd2, then
   0x1fd2.
5. 0x08c = 0, then `U2SetRing(2)`: start-up step 5 again, but with 0x0a4
   = 0 and 0x0b0 = 0xbb80. Then on hook.
6. **The DAA registers for the country.** A write to a DAA register is
   `0x8000 | reg << 8 | value` to codec register 0x0c8. To read one,
   write `reg << 8` to 0x0c8 and, after 5 ms, read 0x0cc. Its high octet
   is `reg` when the DAA has answered (10 tries). The writes: 1 = 0x80,
   1 = 1, 2 = 4, 5 ms, a read of its revision (register 0x16), 5 = 0x50
   (0x54 if the revision is 2 or less), 6 = 0x0a, 7 = 0x0a, 0x1b = 1,
   codec 0x0e0 = 0x0304, 4 = 0. (Not done yet: the timings for pulse
   dialling, 0x0e4 and 0x0e8, and register 3 for the country.)
7. Caller ID on (DAA register 2 = 8).

### A Mercury's start-up

This is `UsbMerInitCodec`, which is the same in the Windows driver.
First, `0xcd000030` bit 2 is cleared and `0xcd000034` = 2. Then these
codec register writes occur, in sequence:

1. 0x0b4 = 0x1387; 0x0a4 |= 0x0004.
2. 0x0e0 = 0x0304, 0x0e4 = 0x0070, 0x0e8 = 0x042f, 0x0f0 = 0x2242, 0x048
   = 2, 0x2c0 = 0x01ff, 0x2c4 = 0xfaac, 0x2d4 = 0x1411, 0x2d0 = 0x9f02,
   0x2cc = 0x01c0, 0x048 = 3, 0x3cc = 1, 0x3c0 = 0x187c, 0x3d0 = 0, 0x3cc
   = 0x0101, 0x3c4 = 0x3dd0, 0x3c8 = 0.
3. Two gains. Each is written when bit 0 of register 0x2dc shows that
   the codec is ready (200 ms maximum): 0x3d0 = 0x10, 0x3c4 = 0xc990
   (`MerGain1`); then 0x3d0 = 8, 0x3c4 = 0x3b0e (`MerGain2`).
4. 0x048 = 2, 0x2cc = 0x41c0, 0x048 = 0, 0x0c0 = 0, 0x08c = 0.

Then `CUsbInit` puts the line on hook (0x0c0 = 1). dmodemd writes 1
immediately instead of 0. A short off-hook at the start can look like a
dialled pulse to an exchange.

## The line

- **Hook.** Codec register 0xc0. On a Mercury, it is 0 off hook and 1
  on hook (`UsbMerSetHook`). On a Polaris, it is the opposite, 1 off
  hook (`U2PolSetHook`). Pulse dialling and the hook flash break and
  make it.

  The caller ID path of a Polaris is DAA register 2: 8 on hook, 4 off
  hook.

  Off hook, the drivers of Agere also write the ring indices of the
  ROM, `0x70001020` = {24, 0} (two words, command 3 with 8 octets).
  dmodemd does not do this: on the first device, it stopped the samples
  of the stream, and then its commands. (The indices are four words
  from `0x7000101c`.)
- **Ring.** On a Mercury, the ring is bit 0 of the IN status word. This
  bit is the copy in the ROM of bit 2 of codec register 0x0a8 (ROM at
  0x948). The two drivers use only that bit for the ring.

  A Polaris does not set that bit. Its ring detector is bit 1 of 0x0a8.
  On the first device, 0x0a8 read 0x0010 on hook, and 0x0012 or 0x0013
  during a ring. Bit 3 of 0x0c4 is also set, its bits 6 and 7 are the
  two halves of the ring, and the samples go to full scale.

  Thus, on hook, dmodemd reads 0x0a8 (and 0x0c4 on a Polaris) each
  100 ms. It gives the core a ring pulse at each read while the ring bit
  of the codec is set (bit 1 on a Polaris, bit 2 on a Mercury). It also
  gives one for each IN packet whose status has bit 0 set. Three guards
  prevent false rings:
  - after a call, 0x0a8 on the first device kept bit 1 set with no
    ring. Thus a ring on a Polaris counts only while dmodemd has seen
    the two bits 6 and 7 of 0x0c4 (the halves of the ring) set in the
    last 500 ms;
  - no value of 0x0a8 counts as a ring in the first 2 s after a call
    goes back on hook;
  - if a ring bit stays set for more than 6 s (no cadence has rings that
    long), dmodemd thinks it is stuck, and ignores it until it clears.

  The `FAKE_AGERE_RING_STICKS` setting of the fake device makes the
  ring bit of a Polaris stick in the same way.
- **Caller ID.** The `EnableCallerID` of the Mercury does nothing, thus
  the codec receives the line on hook. The caller ID path of a Polaris
  is on while on hook (above). In the two cases, caller ID comes in the
  samples, as on an ALSA codec.
- **Country.** The `ConfigureCountry` of the Mercury does nothing in
  this driver. A Polaris gets its default registers (above). The `-c`
  option and `AT+GCI` of dmodemd set only the rules of the core
  (dialling, tones).
- **Loop current.** Fixed in this driver (`UsbMerCheckLoopCurrent`
  returns 20).
- **LEDs.** The board of the Hiro H50113 (the AS560USB of the FCC) has
  two LEDs: D8 "READY/DATA" and D6 "OH".
  - The drivers of Agere set GPIOs 2 and 3 (`0x30000008`) at the start
    and at the stop (`U2SetEnumerationLED`, `CUsbStop`). The LED patch
    of Windows flashes those two. They are the LEDs of the reference
    design, but not of this board.
  - The LED probe (below) on the first device lit OH on GPIO 4, while
    the GPIO is set. dmodemd turns it on off hook, flashes it quickly
    (125 ms on, 125 ms off) during a ring, and turns it off at close.
  - READY/DATA did not come on with any of the GPIOs in the probe. (The
    directions read 0x00ffffff as a word, thus there are 24.) Nothing in
    the two drivers or in the ROM controls another LED:
    `U2SetEnumerationLED` sets GPIOs 2 and 3, and the ROM writes
    `0x30000008` only in its parallel loader at boot time. Possibly
    this LED connects to the power or to the USB connection, not to the
    software.
  - `DMODEM_AGERE_LED_PROBE=1` (or a build with `DMODEM_AGERE_LED_PROBE`
    defined) inverts each GPIO output, 0 to 31, one at a time at the
    start. It writes full words. Each output stays inverted for 1.5 s,
    with 0.5 s between steps. `DMODEM_AGERE_LED_PROBE=<ms>` sets the
    length of each step (with a gap of a third of that). The log shows
    each step (`Agere: LED probe step N:
    ...`), with the written value and the value read back. The step that
    turns on an LED shows which output controls it.

The samples are S16 little-endian, mono, at 8000 Hz. (The sample rate
converter of the host driver accepts only 8000 Hz:
`ConverterSetSampleRate`.) dmodemd runs the line at 8000 Hz, and
ignores the value of `-r`. After 10 s of samples it records their rate
in the log (`Agere: N samples a second, from X to Y`, with their range).
It records an error if the rate is more than 5% away from 8000. With
`-d1` or more, it records the clock registers and GPIOs of the
controller, codec registers 0x080, 0x0a4, 0x0a8, 0x0ac, 0x0b0, 0x0b8,
0x0c0, 0x2d4 and 0x2dc, and the ring indices of the ROM. It records them
when the stream starts and 10 s later (`Agere: streaming: ...`,
`Agere: after 10 s: ...`).

The stream of the first device came at 2457 samples each second, all
-32768 (0x8000, directly from the data register of the codec,
`0xe0000100`). This was the same with the stream above and with the bulk
patch, at the two ARM clocks: the codec does not sample at 8000 Hz.
Start-up step 5, the one write that the Windows driver made and that
dmodemd did not make at that time, did not change this. Codec register
0x0c0 reads with bit 15 set (0x8001 on hook), and 0x080 reads 0x1f40
(8000).

## Bring-up

    dmodemd --usb=auto -d2 dmodem0

On Linux, the device must have read and write access. Run as root, or
add a udev rule (`/etc/udev/rules.d/60-agere-usb.rules`):

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

On Windows, it must have WinUSB (`dmodem-driver usb dmodem-usb.inf`, or
Zadig) instead of the driver of Agere
([windows.md](windows-details.md#usb-modems---usb)).

### What the log shows

With `-d2`, dmodemd records each step in the log: the endpoints, the
version of the ROM, the codec, each vendor command and its result, the
ARM clock, each entry of the function table that it restores, and each
change of the status word.

Each 10 s it records the counts of the stream (`Agere: N samples in, N out,
lead ...`): the samples in each direction, the lead of the ring, the
OUT transfers made longer or shorter, the packets of silence, and the
IN packets that failed, were short, or had an unusual count. If no
samples come for 500 ms, it records `Agere: its samples stopped ...`
one time.

### Settings for debugging

| Setting | What it does |
|---|---|
| `DMODEM_AGERE_DUMP=1` | During the stream, also records in the log the endpoints of the configuration and the memory of the ARM as hex: the ROM from 0 (64 KB, until a read fails), its RAM (`0x70000000`, 8 KB), and the registers of the controller, the codec and the endpoints. |
| `DMODEM_AGERE_TRANSFERS=<n>` | The transfers in flight in each direction, 2 to 32 (16 by default). |
| `DMODEM_AGERE_ROM=<hex>` | Uses this as the version of the ROM instead of the value from command 1. |
| `DMODEM_AGERE_LED_PROBE=1` or `=<ms>` | The LED probe ([above](#the-line)). |
| `DMODEM_USB_TX_GAIN=<dB>` | All USB modems: increases or decreases the level of the samples from the modem to the line (-20 to 20 dB), clipped at full scale (the log tells about the clipping one time). |
| `DMODEM_USB_TX_GAIN=auto` | All USB modems: sets that gain for each call from the answer tone of the remote (below). |
| `DMODEM_USB_REC=<path>` | All USB modems: records each call (below). |

**A fixed transmit gain.** The remote of the first device received it
badly (the V.34 MP sequence from the remote asked for 7200 bit/s
maximum, and V.22bis lost the carrier). The received samples of the
first device were clean. It sent at approximately -21 dBFS.

**The automatic transmit gain** (`DMODEM_USB_TX_GAIN=auto`) comes from
the answer tone of the remote: 2100 Hz, received while the modem is
quiet (that is, on the calling side), the median of 10 frames.

- On an Agere modem: a tone at -14.8 dBFS, as from the remote of the
  first device (where +6 and +10 dB connected at 19200 bit/s), gives
  +8 dB. Each dB less (a longer line) adds half a dB, in the range 0 to
  +12 dB.
- On the other USB modems (the SmartUSB56 and the pico-serial sticks):
  the STLC7550 of the SmartUSB56 is much louder (+12 dB got to a USR
  Courier at -6 dBm, with clipping). Thus a tone at -14.8 dBFS gives
  -2 dB, in the range -12 to +6 dB.
- Until dmodemd receives a tone (on the answering side), the gain is
  the gain from the tone of the last call. If there was no tone yet, it
  is +6 dB (-2 dB on the other USB modems).
- For each second with more than 8 clipped samples, the gain decreases:
  the loudest sample of that second goes to 1 dB below full scale, and
  the decrease is a minimum of one dB. This does not occur when the call
  is online. The receiver of the remote, trained on the old level, would
  lose the data until a retrain (the log records the clipping instead).

The log shows the gain and the level of the tone.

**Recording a call** (`DMODEM_USB_REC=<path>`) records each call, from
off hook to on hook, to `<path>-<date>-<time>.wav`. It is stereo at the
rate of the line: the left channel is the samples from the line as they
came, and the right channel is the samples from the modem as it sent
them.

If it does not operate correctly, send that log, with the dump (and a
recording of a call).

### The first device

The first device ran from an Android phone, through D-Modem for
Android. It has ROM 1.2.1 and a Polaris codec, and runs at high speed.
At first, dmodemd operated it as a Mercury, because its test of bit 15
of register 0xc0 was inverted.

Then dmodemd used alternate setting 2 for the stream, with the Linux
bulk patch of Agere, as `agrmodemlib.o` does. Its command 0x10 takes
its mode in `wIndex` (`Usb_StartIsoDma` sends it that way). Samples
came, but at 2459 each second with each ARM clock, and all -32768. The
patch polls the FIFOs of the codec from the main loop of the ROM with
the codec interrupt off, and the samples of the codec did not come
through it.

The ROM and the Windows driver showed the stream described above, which
needs no patch. On it, the samples were the same, 2457 each second of
-32768. The Polaris had the start-up of the Mercury, and its link did
not get a pulse to come up. Now dmodemd gives a Polaris its own start-up
([above](#a-polariss-start-up)).

Not done yet: full speed; the DAA settings of a Polaris for each country
(its pulse timings and register 3); and the voice and speakerphone
functions.
