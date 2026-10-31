# Softmodem hardware: dmodemd on ALSA and USB: technical details

This is the technical companion to [Softmodem hardware](hardware.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

Normally dmodemd's line is a SIP call. Two options give it a real
telephone line instead:

- With `-a`, the line is a softmodem's codec, driven through ALSA (the
  Advanced Linux Sound Architecture, Linux's sound drivers).
- With `--usb`, the line is a USB modem, driven through libusb. dmodemd
  knows four kinds: a Smart Link SmartUSB56
  ([below](#smart-link-smartusb56)), a pico-serial Line stick
  ([below](#pico-serial-line)), a pico-serial WB stick and an Agere USB
  2.0 soft modem ([below](#agere-usb-20-soft-modem)).

The WB stick is not for a telephone line: it sits on a bare copper pair
and runs D-Modem's wideband mode. Any ALSA codec that runs at 96 kHz can
do the same ([pico-serial WB](#pico-serial-wb-wideband-bare-pair)).

In every case the modem itself is D-Modem's open DSP (digital signal
processing) code, which runs on any processor architecture. The vendors'
own programs for this hardware (Smart Link's slmodemd, Agere's agrsm and
others like them) were closed, and mostly ran only on 32-bit x86 (i386).
dmodemd can take their place.

    dmodemd -a hw:1 -r 9600
    dmodemd --usb=auto

## The hardware

Linux still drives these modem codecs at the level a softmodem needs: a
stream of samples each way, and switches for the hook and for the line
monitor.

| Kind | Kernel driver | Codecs |
|---|---|---|
| AC'97 modem (MC97, AMR and CNR risers; laptops of about 2000 to 2006) | `snd-intel8x0m` (Intel ICH to ICH7, AMD-768/8111, nForce, SiS 7013, ALi), `snd-atiixp-modem` (ATI IXP), `snd-via82xx-modem` (VIA) | any codec whose hook is the AC'97 modem GPIO: Silicon Labs Si3036/38, Agere, Motorola and others; Conexant's SmartDAA ([below](#conexant-ac97-codecs)) |
| HD-audio modem (laptops of about 2005 to 2010) | `snd-hda-intel` with `snd-hda-codec-si3054` | Silicon Labs Si3054/3055 (163c:3055, 3155), Agere (11c1:3026, 3055, 3155), Motorola (1057:3055, 3057, 3155), SM56 (1543:3155), LG (1854:0018), a VIA board's (1106:3288) |

The codec appears as one of the sound card's PCM devices. Among the
card's controls are two switches, "Off-hook Switch" and "Caller ID
Switch". To find them:

    aplay -l                      # a "Modem" device: card 1, device 0 is hw:1
    amixer -c 1 controls | grep -i -e hook -e 'caller id'

An HD-audio modem is a device of the audio card, for example `hw:0,6`.

dmodemd needs the "Off-hook Switch": without it, it says the card is
probably not a modem codec and stops. Without the "Caller ID Switch" it
goes on, but warns that rings and caller ID may not be heard on hook.

### Si3054 line settings

On an HD-audio Si3054 with a Si3018 line side, dmodemd also writes the
country's line settings (from `-c` or `AT+GCI`) into the codec's DAA
(data access arrangement, the line interface) registers. It reaches them
through the kernel's HD-audio hwdep device (`/dev/snd/hwC<card>D<codec>`),
which needs a kernel built with `CONFIG_SND_HDA_HWDEP`. The code is in
`dmodemd/si3054.cpp`, `dmodemd/hda_codec.cpp` and
`dmodemd/alsa_line.cpp`.

The values come from the datasheet (Si3054/18 Rev. 1.0, Table 19, the
HD-audio columns). They are:

| Setting | Bits |
|---|---|
| on-hook speed | OHS, OHS2 |
| ringer impedance and threshold | RZ, RT |
| AC termination | ACT |
| DC termination | DCV, MINI |
| loop current limit | ILIM |

They go into nodes 13, 15, 16 and 18 of the codec. Node 18 holds the
spark quenching bits that OHS needs. dmodemd reads each register first
and changes only the country's bits.

- Where the table allows ACT 0 or 1 (the TBR21 countries, and Egypt),
  dmodemd takes 1, the complex impedance.
- RZ is cleared off hook, as the datasheet asks for pulse dialling, and
  set again on hook.
- The settings are written again before each off-hook, because the
  kernel's driver resets the codec on resume.
- A country that the table does not list gets the USA's settings.

dmodemd writes nothing, and logs `line settings: the kernel driver's`,
in three cases:

- on an AC'97 modem;
- on a kernel without the hwdep device;
- on a codec whose chip ID (node 12) is not a Si3054 with a Si3018, such
  as the other vendors' chips that the Si3054 driver binds.

Those codecs keep the kernel driver's settings, which are the USA's.

### Conexant AC'97 codecs

Conexant's (and Rockwell's) AC'97 modem codecs have vendor ID `CX` with
a device ID 0x54xx, or `RS` with 0x53xx. They keep their hook in their
SmartDAA, not in the AC'97 modem GPIO that the kernel's "Off-hook
Switch" drives. The kernel does give their samples, so `-a` works with
them, except for the hook.

On an Intel ICH modem controller (`snd-intel8x0m`, on x86 Linux), dmodemd
started as root also drives the hook in the SmartDAA. It reaches the
codec's registers through the controller's I/O ports, under the codec
access semaphore that the kernel's driver also respects
(`dmodemd/ac97_ports.cpp`). It then logs `the hook in its SmartDAA`,
with the line side's version. Without root, on another controller, or on
another codec, it logs why, and the hook stays with the kernel's switch.

The code is `dmodemd/cx_daa.cpp` and `dmodemd/cx_country.cpp`.

#### How the SmartDAA's registers are reached

The SmartDAA is two chips: the system side (SSD) in the codec, and the
line side (LSD) beyond the isolation barrier. What dmodemd knows of them
comes from Conexant's HSF driver for ICH modems (`hsfmc97ich.o` and its
DAA library `hsfsoar.o`): from the code and from its debug messages.

- **An SSD register** is a byte, numbered 0 to 63, reached through codec
  register 0x7a. A write puts `(reg << 8) | value` into 0x7a. A read
  writes `0x8000 | (reg << 8)` and then reads 0x7a's low byte; 0xffff
  means there is no SSD. SSD register 3 needs a 40 µs wait before each
  access.
- **An LSD register** goes through SSD registers 1 to 3 and a status
  register:
  - the byte to write goes into SSD register 2;
  - the command goes into SSD register 3: the LSD register in bits 5:0,
    and bit 7 set for a write;
  - the answer comes back in SSD register 1;
  - the status is SSD register 0 (register 4 on some SSDs). Bit 4 means
    done and bit 3 an error; both are cleared before each command. Bit 2
    toggles at each frame over the barrier, and bit 1 means the link is
    up.

  A write to one of the line's control registers (0x03, 0x14, 0x15, 0x1c
  to 0x25) first sets bit 1 of SSD register 0x17. One kind of SSD (SOAR's
  kind 0x1a) uses bit 6 of the command for them instead. A write is good
  when its answer is the byte written. The driver tries 5 times (LSD
  register 0x17 only once).
- **The hook** is LSD register 0x15, bits 7:6: 01 on hook, 10 off hook,
  and 00 and 11 a dial pulse's make and break.
- **The kinds of SSD.** The driver's library knows 39 kinds of SSD,
  which differ in how they reach the LSD. Some prime SSD register 1
  before each command (0x55 for a read, the byte's inverse for a write)
  so that an absent LSD can be told. Some have the status in register 4.
  Some send a read's command before clearing the status. The ICH driver
  takes the kind from the codec's DAA type, the low byte of register
  0x7e:

  | DAA type | 0x20 | 0x21 | 0x22 | 0x23 | 0x28 | 0x29 | 0x30 | 0x31 | others |
  |---|---|---|---|---|---|---|---|---|---|
  | SSD kind | 5 | 0x0c | 0x13 | 0x15 | 0x0d | 0x12 | 0x17 | 0x20 | 5 |

  Type 0x24's SSD is in the codec's own registers, not behind 0x7a.
  dmodemd does the same (`cx_kind()`), and leaves a type 0x24 codec's
  hook to the kernel.

#### What Conexant's library writes

The HSF interop harness (`half_hsf`, [interop.md](interop.md)) runs
Conexant's library against a simulated SmartDAA of this kind (codec
0x5429, SSD kind 0x12). The simulated LSD answers as above, reports
version 5, and its register 2 reads 0x0f once powered. For the USA, the
library then sets the line up with these LSD writes, in order:

- **Start:** 0x03 = 00 and 0x04 = 2f, then it waits for 0x02 to read
  0x0f (the LSD powered). Then 0x20 = 00, 0x25 = 29, 0x24 = 28, 0x21 =
  80, 0x19 = 80, 0x08 = 00, 0x0a = d9, 0x0e = 00, 0x0c = 07, 0x12 = 00,
  0x10 = 00, 0x19 = c0, 0x1f = 08, 0x15 = 40 (on hook), 0x1c = 78, 0x14
  = e0, 0x07 = 04, 0x18 = 43, 0x19 = cb, 0x06 = 02. Then 0x0c = 06, 0x12
  = 60, 0x1c = 30, 0x0e = 00, 0x18 = 43, 0x10 = 00, 0x1c = 68, 0x1c =
  6a, 0x0c = 02, 0x0a = df, 0x10 = 00, 0x0e = 02, 0x10 = 00.
- **ATA (going off hook):** 0x15 = 41, 0x12 = 40, 0x0e = 00, 0x14 = 20,
  0x18 = 43, 0x10 = 00, 0x21 = 82, 0x1c = 32, 0x15 = 40, then 0x15 = 80
  (off hook) and 0x19 = 8b.

Before the LSD start it writes SSD registers directly: 0x1c = 40, 0x17 =
04, 0x14 = 08, 0x0b = 5f, 0x06 = c0, 0x19 = 20; 0x05, 0x04 and 0x07 to
0x0a = 00; 0x0d = 10, 0x1e = 00, 0x19 = 28, 0x1c = 7f; 0x1a, 0x1b and
0x1d = 00; 0x25 = 5a, 0x23 = 43, 0x24 = 80, 0x22 = 5a, 0x20 = 63, 0x21 =
a8.

The simulated LSD's status registers (0x16, 0x1a, 0x1e, 0x1f) read 0.
On a real line the library may write other values where it adjusts to
what it measures (the loop voltage, the drive).

Two runs, with the simulated line side's registers starting at 0 and at
0xff (`HSF_DAA_FILL=0xff`), show which bits each of the library's writes
keeps. A third run, over two calls, shows its hang-up:

- SSD register 0x14's bits 1:0 back to 0;
- LSD 0x12's bits 6:5 = 11, 0x15 = 40, 0x19 = cb, 0x0e = 00, 0x21 = 80,
  0x14's bits 7:2 = 111000, 0x18 = 43, 0x10 = 00, 0x1c's bits 6:0 = 6a,
  0x10 = 00, 0x0e = 02, 0x10 = 00.

Going off hook ends with SSD 0x14's bits 1:0 = 01.

#### What dmodemd does

For a codec of kind 0x12, dmodemd does the same as the library
(`CxDaa::power_up()`, `go_off_hook()`, `go_on_hook()`):

- the SSD's writes;
- the LSD's power-up, and reading its version (0 to 0x11, the versions
  the library takes);
- the line's writes, keeping the bits the library keeps;
- the hook's sequences, including the 100 ms the library waits after
  0x15 = 41.

A dial pulse changes the hook bits alone. `cx_daa_test` checks the LSD
writes against the library's, from both starting states.

For the other kinds of SSD the setup is not known. dmodemd reads the
line side's version and changes only the hook bits. That holds the line
only if something else has set the line side up.

#### The country and the board

The country's and the board's parameters are Conexant's, just as the
other backends' come from their datasheets. `cx_country.cpp` is
generated from the HSF driver's files.

- **The country.** Of the line settings above, only LSD 0x12's bits 6:5
  change with the country. That was found by running the library for a
  country from each of `hsf.cty`'s 14 `SMART_RELAYS` tables
  (`HSF_COUNTRY=<T.35 code>`). The two bits are bits 7:6 of the low byte
  of the table's off-hook and on-hook state words. There are three
  settings:

  | Setting | On hook | Off hook |
  |---|---|---|
  | US (the USA, Japan, ...) | 11 | 10 |
  | ITU (most countries) | 10 | 10 |
  | TBR21 (its countries) | 00 | 00 |

  The table has all 242 of `hsf.cty`'s regions under their own names,
  and also D-Modem's names for the three it names differently (BAHREIN,
  SOUTHKOREA, UAE). `-c` and `+GCI` choose the row, as for the Si3054.
  On hook, a new setting goes to the line side at once.
- **The board.** dmodemd takes the `.inf`'s `SmartDAAParamsK3` block for
  DAA types 0x22 to 0x24, 0x29, 0x30 and 0x31, and its `SmartDAAParams`
  block for the others, as the ICH driver chooses (95 bytes each).
  Changing each byte of the block in turn (`HSF_NVM` with an edited
  `.inf`) showed that three bytes change the library's writes:

  | Byte | Goes to |
  |---|---|
  | 87 | SSD 0x21, as 0x80 \| byte |
  | 93 | SSD 0x0b |
  | 94 | SSD 0x06's bits 7:4 |

  The others (its resistors and DC tables) change nothing on the line
  sides dmodemd sets up. The library uses them only for a line side of
  version 0x11, which it measures (below).

`cx_daa_test` checks the writes against the library's for the USA,
Germany and Canada, from both starting states.

#### Line side versions

The line side's version (LSD 0x22's bits 5:0) changes what the library
does. Each version was tried with `HSF_LSD_VERSION` in the harness:

| Version | What the library does |
|---|---|
| 1, and 3 to 0x0f | the setup above |
| 2 | the same, except for three things: no writes to 0x24 and 0x25; 0x21's bit 7 clear in each of its writes (00 and 02, not 80 and 82); and 0x19's plain writes with bit 5 set (eb and ab, not cb and 8b) |
| 0x10 and 0x11 | another setup altogether (0x1d, 0x1f = 30, 0x15 = c0 on hook and 00 off hook: the hook's bit 7 the other way round); for 0x11, the line is also measured (below) |
| 0 | it reads the monitor ADC over and over |

Version 2 is one that real line sides have: Conexant's own
`hsfconfig --info` has shown `SmartDAA SSD=10 LSD=0x2` for an HSF PCI
modem.

dmodemd does the library's setup for versions 1 to 0x0f, including
version 2's differences (`cx_daa_test` checks version 2's writes too).
For versions 0, 0x10 and 0x11 it reads the version and changes only the
hook bits, with bit 7 the other way round for 0x10 and 0x11.

#### The line side's monitor ADC

- LSD 0x16's bits 6:4 choose the channel. Its bit 3 starts a conversion
  and reads 1 when it is done.
- LSD 0x17 is the reading (8 bits, written back when read).
- The library takes it as reading × 2000 / 255 mV plus an offset of 500
  mV. For version 0x10 the offset is calibrated on channel 3, against a
  reference it wants at 1350 to 1650 mV. Versions 0 and 2 have 4000 mV
  for 255 (and version 0 7 bits).

Only on version 0x11 does the library measure the line with it:

- on hook, the battery on channel 1 (Vbatt = 28.2 × the reading's mV +
  400; the factor comes from the board's resistors);
- off hook, channel 0 (the receiver's DC offset), then channel 1 again
  (the tip-ring voltage, from which it works out the loop's resistance,
  current and power);
- a DAC (LSD 0x19), calibrated on channel 4 and then set so that the
  tip-ring voltage meets the DC curve, and adjusted while the line
  settles ("adaptive offhook").

dmodemd does none of this, since it does no version 0x11 setup.

None of the Conexant code has run on a real codec yet.

### HD-audio modems outside the Si3054 list

`snd-hda-codec-si3054` binds only the vendor IDs listed above. A modem
codec made to the same interface under another ID gets no driver, and so
no ALSA device. `snd-hda-intel` can give such a codec the Si3054's
vendor ID through its early patching (`CONFIG_SND_HDA_PATCH_LOADER`,
enabled in most distributions' kernels), and the Si3054 driver then
binds it. `hda-modem-setup` (installed beside dmodemd, on Linux) does
this:

    hda-modem-setup               # list the modem codecs
    sudo hda-modem-setup write    # the patch for those with no driver
    sudo reboot
    hda-modem-setup list          # now driven by snd_hda_codec_si3054?

The three actions:

- **`list`** (the default) shows each codec with a modem function group
  (from `/sys/bus/hdaudio/devices`): its card and address, its IDs, its
  driver, and whether it is in the Si3054 list.
- **`write`** takes the modem codecs that have no driver and are not in
  the list, or those named with `--codec hdaudioC<card>D<addr>`. For each
  it writes a `[codec]` section, with `[vendor_id] 0x163c3055`, to
  `/lib/firmware/dmodem-hda-modem.fw`. It also writes the module option
  that loads the file for each such codec's card to
  `/etc/modprobe.d/dmodem-hda-modem.conf`
  (`options snd-hda-intel patch=...`). Before writing, it checks the
  running kernel's configuration for the patch loader and the Si3054
  driver. It warns if another file already gives `snd-hda-intel` a
  `patch=` option, as only one takes effect.
- **`remove`** deletes both files again.

The change takes effect the next time `snd-hda-intel` loads. If the
module is in the initramfs, rebuild that first (`update-initramfs -u`,
`dracut -f`).

Only the driver can tell whether the codec really is Si3054-compatible,
once it has bound it. At its init it checks the link to the line side
chip and logs the chip's ID. "Link Frame Detect(FDT) is not ready" in
`dmesg` means the codec is not compatible: run
`sudo hda-modem-setup remove`, and reboot.

Conexant's HD-audio modems (`14f1:2bfa`, `14f1:2c06` and the others of
vendor `14f1`) are of another kind. Their DAA is behind Conexant's own
verbs, and this does not cover them.

`dmodemd/test/hda_modem_setup_test.py` (ctest's `hda_modem_setup`, label
`quick`) runs the helper on a fake sysfs and kernel configuration. The
patch itself has not been tried on a real codec.

### Smart Link SmartUSB56

Smart Link's USB modem (the SmartUSB56, and the modems built on its
ST7554 bridge, USB `0483:7554`) needs no kernel driver: dmodemd drives
it through libusb. The ST7554 is a USB bridge to an STLC7550 codec and a
DAA:

- its registers are vendor control requests;
- its samples are isochronous transfers both ways;
- its GPIOs are the hook and the ring detector.

```
lsusb -d 0483:7554            # Bus 001 Device 007: ID 0483:7554 ...
dmodemd --usb=auto            # the first one; --usb=1:7 that one
```

Started as root, dmodemd opens the device before it goes on as `nobody`.
To start it as an ordinary user, give that user the device with a udev
rule, for example `/etc/udev/rules.d/60-smartusb56.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="7554", MODE="0660", GROUP="dialout"

If Smart Link's own kernel driver (`slusb`) is loaded, dmodemd detaches
it from the device while it runs. dmodemd is built with `--usb` when
pkg-config finds libusb-1.0 (the libusb-1.0-0-dev package).
`dmodemd --usb list` lists the SmartUSB56s, pico-serial Lines and WBs
and Agere modems there are, and whether each can be opened. On Windows
dmodemd.exe drives them the same way, on WinUSB (or on libusbK or UsbDk
on Windows 7 and 8, whose WinUSB cannot stream)
([windows.md](windows-details.md#usb-modems---usb)).

The ST7554's register values and start-up are those of Smart Link's
Linux driver for it (`drivers/st7554.c` of slmodem 2.9.11, under a BSD
licence). The code is in `dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 9600 Hz (the default) or 8000
  Hz, in isochronous packets of 1 ms. Three transfers of five packets
  are in flight each way. Each OUT transfer is sent when an IN transfer
  completes, with that IN transfer's packet lengths, so the playback
  keeps to the codec's clock. The OUT transfers take the modem's queued
  samples; the queue starts a frame and a half (30 ms) ahead. The
  modem's clock is the codec's: the number of samples it has delivered.
- **The hook.** GPIO 3, with the hook LED on GPIO 7. A hook flash clears
  GPIO 3 for the flash's length.
- **Rings.** The ring detector (GPIO 0) changes with the ring signal.
  Each change comes on the interrupt endpoint and is a pulse to the
  core's `ring()`, which checks the cadence. A change that comes with a
  FIFO underrun or overrun is not counted, since the ring bit can come
  set with those whether the line rings or not.
- **Caller ID.** On hook the codec hears nothing unless GPIO 5 (CID1)
  connects it to the line. With `AT#CID=1`, dmodemd sets CID1 from the
  first ring while the core's caller ID receiver listens, and clears it
  before the hook changes, as Smart Link's Windows driver does (its
  Linux driver never did). So caller ID is heard between the first two
  rings.
- **The codec.** Either an STLC7550, or an ST75951/2 "silicon DAA"
  (`MONOLITHIC`, bit 14 of the GPIO status). The ST75951's control words
  name one of its four registers; dmodemd starts it up as the Windows
  driver does. The hook, ring and caller ID use the same GPIOs.
- **LEDs.** The hook LED (GPIO 7) follows the hook. With `--usb-leds`,
  dmodemd also drives two more LEDs, as Smart Link's Windows driver does
  on boards whose INF says they have them (Creative's Modem Blaster V.92
  USB):
  - the carrier LED (GPIO 6), lit while the call is online;
  - the data LED (GPIO 13), lit once data has passed in the call.

  Both go off on hook. Without the option dmodemd leaves those GPIOs
  alone, as another board may use them for something else.
- **Wake on ring.** With `--usb-wake`, dmodemd arms the device's USB
  remote wakeup (SET_FEATURE) if its configuration says it can wake, as
  Smart Link's Windows driver does when the PC sleeps. The ring
  detector's interrupt stays on while on hook, so a ring wakes the
  computer. The kernel must allow it too:
  - On Linux, dmodemd writes `enabled` to the device's
    `/sys/bus/usb/devices/<bus>-<ports>/power/wakeup` while it is still
    root. Started as a user, add `ATTR{power/wakeup}="enabled"` to the
    udev rule above.
  - On Windows, it is Device Manager's "Allow this device to wake the
    computer".

  The remote wakeup is cleared when dmodemd exits.
- **Suspend and resume.** While the computer sleeps, the transfers fail.
  dmodemd keeps them and, every second, sets the device up again (its
  registers, the remote wakeup and the FIFO, since a resume may have
  reset it) and submits them again. It gives up after 30 s, as for a
  device that has gone. A call is lost across a sleep. The rings after
  the one that woke the computer are heard as usual, so the modem can
  still answer.
- **The handset detector.** With `--usb-handset`, dmodemd enables the
  interrupt of GPIO 2 (`GPIO_HANDSET` in Smart Link's Linux source; the
  Windows driver enables it too, and does nothing with it). On each
  change it reads the GPIO. Set means that the phone on the modem's
  PHONE jack is off hook, as far as is known (no driver says). The core
  reports it in voice mode as V.253's local phone event: `<DLE>H` off
  hook and `<DLE>h` on hook. It lists event 8 in `+VLS=?`.
- **International line controls.** With `--usb-international`, dmodemd
  drives three more outputs, as Smart Link's Windows driver does with its
  `Options.International` (off in Creative's INF):
  - **DP** (GPIO 10): set off hook; cleared on hook, together with RFC.
  - **DC** (GPIO 12): set off hook in France, Algeria and Tunisia, the
    countries whose record asks for it (byte 90, bit 0, of Smart Link's
    table). On an ST75951 it is the other way round: set off hook in
    every other country.
  - **RFC** (GPIO 8): set for 350 ms after each off-hook in Australia
    (bit 1, "the Australian timer"), and all through pulse dialling.
    During pulse dialling DP and RFC are left alone, and DC is cleared
    on each break.

  What the three switch in the DAA is not known: only Smart Link's names
  for them are. They are for a board built for these countries. The
  country is the one set by `-c` or `+GCI`. dmodemd learns when pulse
  dialling begins and ends from the call progress pump
  (`MDMPRM_PULSE_DIALLING`).
- **The voice codec (a handset).** With `--usb-voice-codec=<law>`,
  dmodemd takes alternate setting 3 instead of 1. Its second pair of
  isochronous endpoints is the ST7554's SSI2 codec (Smart Link's
  `K7554VoiceCodecInterface`: 8000 octets a second, powered up with
  SSI_CTRL = 0x21, its period in register 6). In voice mode `+VLS=?` then
  lists two more V.253 devices:
  - **14** (`"H"`): the handset alone, the line on hook. The voice
    engine's samples are the handset's: its microphone goes to `+VRX`,
    and `+VTX` goes to its earpiece.
  - **15** (`"HT"`): the handset on the call, off hook. The voice
    engine's samples are the line's, and dmodemd mixes the handset in
    both ways: the line and the modem's samples go to the earpiece, and
    the microphone goes to the line and to `+VRX`.

  The two codecs' rates are bridged by linear interpolation. What the
  octets are is not known: the Windows driver calls its format 3 and
  never uses it, and streams this codec only from its test interface.
  So the option takes `ulaw`, `alaw` or `linear` (signed 8-bit).
  Creative's INF uses alternate setting 1, so the DE5671 may have no
  codec there. A board whose alternate setting 3 lacks these endpoints is
  refused.
- **RHOM.** With `--usb-rhom`, dmodemd sets FIFO_CONTROL2's bit 15 (and
  bit 7, the voice codec's, with `--usb-voice-codec`) while the modem
  runs (from the line's start to its stop). This is what Smart Link's
  Windows driver does with `Options.RHOM` (`ConfigureRHOM`,
  `EnableRHOM`), which Creative's INF leaves off. A chip whose revision
  (register 0) is below 0x200 never gets it, as on Windows. What RHOM
  does is not known.
- **The buzzer.** With `--usb-buzzer`, the modem's speaker is GPIO 11
  (BUZEN). It is on whenever the core has the speaker on at any volume,
  and off otherwise:
  - ATM1 (the default): on from dialling or answering until the CONNECT;
  - ATM2: always on;
  - ATM0, or ATL0: never on.

  This is what Smart Link's Windows driver does in `SetSpeakerVolume`
  with `Options.Buzzer` (off in Creative's INF). A buzzer has no volume,
  so ATL1 to ATL3 are the same. Not every board has one.
- **Unplugged.** dmodemd says so and exits.

Limits: the device's line settings are its own. dmodemd has not yet been
tried on a real SmartUSB56. Caller ID and the ST75951 follow the Windows
driver (read from its code, [smartusb56-windows.md](smartusb56-windows.md))
and have been tested on the fake device only.

### pico-serial Line

The [pico-serial](https://github.com/D-Modem/hardware) project's
phone-line stick ([SPEC-L.md](https://github.com/D-Modem/hardware/blob/HEAD/SPEC-L.md))
is a USB-C stick with an RJ11 socket, made for D-Modem. It is a
softmodem's line and nothing more: a Skyworks Si3056 DAA and its Si3018
line side behind an RP2040 microcontroller, at 9600 Hz so that the open
DSP's V.90 works. dmodemd drives it through libusb, as it does the
SmartUSB56, and it needs no kernel driver.

    lsusb -d 1209:0002            # Bus 001 Device 008: ID 1209:0002 ...
    dmodemd --usb=auto            # the first SmartUSB56 or stick; --usb=1:8 that one

A udev rule to start dmodemd as a user, for example
`/etc/udev/rules.d/60-pico-serial-line.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0002", MODE="0660", GROUP="dialout"

Its protocol is its firmware's (`firmware/PROTOCOL.md`, "Line stick"),
shaped like the ST7554's on purpose. The code is in
`dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 9600 Hz (the default) or 8000
  Hz, isochronous both ways in interface 0's alternate setting 1.
  Selecting that alternate setting starts them; the rate is set first
  (`SET_RATE`). Each IN packet holds the samples that the stick's codec
  gave in that millisecond, and each OUT transfer is sent with an IN
  transfer's packet lengths, as for the ST7554.
- **The hook.** A vendor request (`SET_HOOK`). The stick times a hook
  flash itself (`HOOK_FLASH`, 1 to 2000 ms). Pulse dialling sends
  `SET_HOOK` for each break and make, as the modem's frames are
  processed.
- **Rings and caller ID.** On hook the stick's line monitor is on
  (`SET_MONITOR`), so it hears the line: **caller ID** comes in its
  samples, as on an ALSA codec. Rings come from its ring detector. Each
  change it counts arrives in an 8-octet event on the interrupt endpoint
  and is a pulse to the core's `ring()`, as each change of the ST7554's
  ring GPIO is.
- **At start-up** dmodemd reads the stick's protocol version, firmware
  version and line side (`GET_INFO`). It refuses a protocol version other
  than 1. If the line side does not answer across the isolation barrier,
  it says so and goes on. ATI3 says `pico-serial Line (Si3056)`.
- **The country's line.** dmodemd writes the country's settings (from
  `-c`, and again on each `AT+GCI`) into the DAA:
  - the AC termination: ACT and ACT2 on the stick's Si3018 line side, or
    ACIM on a Si3019;
  - the DC termination (DCV, MINI);
  - the TBR21 current limit (ILIM);
  - the ringer impedance and threshold (RZ, RT);
  - the on-hook speed (OHS, OHS2, with Australia's spark quenching).

  The values are the Si3056 datasheet's (Rev. 1.03, Table 15, "Country
  Specific Register Settings"), in `dmodemd/si3056.cpp`. Each register
  (16, 26, 31 and 59, and 30 on a Si3019) is read first (`DAA_READ`), and
  only the country's bits change (`DAA_WRITE`). A change made while off
  hook waits until the line is on hook again.

  Countries the table has no row for (Algeria, Brunei, Estonia,
  Lithuania, Sri Lanka, Tunisia, Turkey, Uruguay, Vietnam and most of the
  ITU list's) keep the USA's settings, which are the Si3056's reset
  values, and dmodemd's log says so (`-d1`). The datasheet's TBR21 row is
  `-c CTR21EUROPE`.
- **Unplugged.** dmodemd says so and exits.

Limits:

- The transmit and receive gains stay at their reset values (Table 15
  lists none).
- Australia keeps its Japan-mode DC termination throughout the call. The
  datasheet's option of switching to FCC mode 500 ms after going off hook,
  for a higher transmit level, is not taken.
- The stick's USB ID is a pid.codes test PID until it has its own.
- It has been tested against a fake of it ([below](#testing-without-hardware)),
  not yet on a real one.

### pico-serial WB (wideband, bare pair)

pico-serial's wideband stick ([SPEC-WB.md](https://github.com/D-Modem/hardware/blob/HEAD/SPEC-WB.md))
is the Line stick's sibling for a **bare copper pair**: a dry pair, an
alarm or leased circuit, or a pair of your own between buildings. It is
not for a telephone line. Two WB sticks talk to each other point to
point through a wideband transformer, and sample the pair at 96 kHz
instead of 9.6 kHz. They use a band of about 4 to 36 kHz.

This is the line that D-Modem's wideband mode is for: V.wb, `AT+MS=VWB`
([dsp/vwb.md](dsp/vwb.md)), at 48000 Hz and up (`-r 96000`). It runs
with `AT&L1`, a leased line with no dial tone or ringing: `ATD` calls,
and `ATA` or `ATS0=1` answers. In simulation it carries about 149
kbit/s each way on a short pair, and about 70 kbit/s on 5 km.

dmodemd drives the stick through libusb, as it drives the Line stick,
and it needs no kernel driver. `--usb=auto` finds it beside the other USB
modems. With no `-r` it runs at the stick's own 96000 Hz:

    lsusb -d 1209:0003            # Bus 001 Device 009: ID 1209:0003 ...
    dmodemd --usb=auto            # 96000 Hz; -r 48000 or -r 192000 for those

A udev rule to start dmodemd as a user, for example
`/etc/udev/rules.d/60-pico-serial-wb.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0003", MODE="0660", GROUP="dialout"

At the two ends of the pair, one end calls and the other answers by
itself:

    AT&L1S0=1                     (the answering end, once)
    AT&L1                         (the calling end)
    ATD
    CONNECT 148656

At 48000 Hz and up, V.wb is the modulation by default (`AT+MS?` says
200), since no other runs there; `AT+MS=VWB` is not needed. A leased
line has no hang-up signal of its own. When one end hangs up (`ATH`), the
other sees `NO CARRIER` when V.42 tells it, or once the carrier has been
gone for S7 seconds (`ATS7=10` for 10).

Its protocol is its firmware's (`firmware/PROTOCOL.md`, "WB stick", and
`src/core/wb_proto.h`): the Line stick's USB device, with the WB stick's
own requests. The code is in `dmodemd/usb_wb.cpp`, on top of
`dmodemd/usb_line.cpp`'s transfers:

- **Samples.** S16 little-endian mono at 96000 Hz (48000 and 192000 Hz
  too), isochronous both ways in interface 0's alternate setting 1,
  which starts them. The packets are 1 ms, of up to 480 octets. Each IN
  packet holds the samples the stick's codec gave in that millisecond,
  and each OUT transfer is sent with an IN transfer's packet lengths, as
  for the Line stick. `SET_RATE` (in kHz) is sent first. A rate the
  stick does not have (`-r 8000` or `9600`) is refused.
- **At start-up** dmodemd reads the stick's protocol version and board
  (`GET_INFO`), and refuses a protocol version other than 1 or a board
  that is not the WB stick's. It then turns any loopback off
  (`SET_LOOPBACK`), streams the hybrid's input (`SET_INPUT`: the line
  less our own signal, not the line itself), sets the rate and the gains,
  turns the LINK LED off, and says so if the codec does not run
  (`GET_STATUS`). ATI3 says `pico-serial WB (TLV320AIC3204)`.
- **Gains.**
  - `--usb-wb-tx-gain=<dB>` sets the transmit level: the codec's DAC
    volume (`SET_TX_GAIN`), -63.5 to +24 dB in 0.5 dB steps.
  - `--usb-wb-rx-gain=<dB>` sets the hybrid input's gain: its MicPGA
    (`SET_RX_GAIN`), 0 to 47.5 dB.

  Both are 0 by default, the stick's own levels: about +3 dBm on the
  line for a full-scale sine, which puts V.wb's signal (11 dB below full
  scale) at about -8 dBm.
- **The LINK LED** shows the modem's carrier: on while the call is
  online (through a retrain too), off otherwise (`SET_CARRIER`). At the
  end dmodemd gives it back to the stick, which lights it while its
  samples run.
- **No hook, ring or caller ID.** The modem's hook only starts and stops
  the call. Nothing rings, and `ATS0=1` answers only with `AT&L1`.
- **Events.** Over current on the line's drivers is logged as an error.
  The codec's overflows (the transmit level or the hybrid's gain too
  high) and the stick's underruns and overruns are logged at `-d1`.
- **Unplugged.** dmodemd says so and exits.

It has not yet been tried on a real stick or a real pair. It has been
tested against a fake of the stick ([below](#testing-without-hardware))
on a simulated pair, with the firmware's protocol as its sources have
it. What is still to be checked on hardware:

- **The stick's real delay.** Its codec's filters are not counted in
  dmodemd's `io_delay`. V.wb measures its own echo and needs none.
- **The levels.** The default gains (0 dB) are untried against a real
  hybrid's residue and a real pair's loss; the receive gain may need
  raising on a long pair.
- **V.42 over the host's buffers.** dmodemd sends its samples 4672
  samples ahead at 96 kHz, about 50 ms: a frame and a half of 20 ms,
  15 ms of transfers, and the stick's own buffering. LAPM's window of 15
  frames of 128 octets waits a round trip. In simulation, with that lead
  at both ends, V.wb's 148656 bit/s carried 83 kbit/s each way through
  V.42, against 137 kbit/s with no buffers.
- **The USB ID.** The stick's USB ID is a pid.codes test PID until it
  has its own.

Any ALSA codec that runs at 96 kHz (`-a hw:N -r 96000`, through a line
transformer) is a line for V.wb too. libdmodem runs it at any of the
three rates (`dmodem_create_rate()`).

### Agere USB 2.0 soft modem

Agere's (later LSI's) USB soft modem, USB `047e:2892` (and `047e:2828`),
is sold under other names too. It has no kernel driver: dmodemd drives it
through libusb. It is an Agere USB controller with an ARM core in it,
and behind that either a Mercury codec or a "Polaris" DAA. Its ROM
streams the samples itself on isochronous endpoints, as Agere's Windows
driver has it, so nothing is downloaded into it. It needs a USB 2.0
(high speed) port; on a full speed port dmodemd says so and stops.

    lsusb -d 047e:                # Bus 001 Device 009: ID 047e:2892 ...
    dmodemd --usb=auto

The line runs at 8000 Hz, whatever `-r` says. A udev rule
(`/etc/udev/rules.d/60-agere-usb.rules`) lets dmodemd open it without
root:

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

[agere-usb.md](agere-usb-details.md) has the full detail, read from the
device's ROM and Agere's drivers. In short, `dmodemd/usb_agere.cpp`
does this:

- **Start-up.** Interface 0's alternate setting 1 (isochronous IN and
  OUT), the ROM's version, its GPIOs and clock, and its codec's
  registers as Agere's drivers write them. Codec register 0xc0's bit 15
  tells a Polaris from a Mercury. A Polaris gets its link pulsed up, its
  calibration and its country's DAA registers. Then on hook. Then the
  ROM's function table is put back as the ROM has it (any patch left by
  an earlier driver is removed), the clock is raised, and the ROM's
  stream is started (command 0x0c).
- **Samples.** A packet each way every 2 microframes (4000 a second): a
  header (the samples' count and a status word), then the samples.
  Sixteen transfers of 24 packets are in flight each way. Each OUT
  transfer carries 48 samples (2 a packet), the codec's own pace, with
  one sample more or less now and then to follow the drift between the
  USB's clock and the codec's.
- **Hook.** Codec register 0xc0: 0 is off hook on a Mercury, 1 on a
  Polaris. Pulse dialling and the hook flash break and make it.
- **Rings.** Bit 0 of the IN packets' status word: a pulse to the core's
  `ring()` for each packet that has it. On hook dmodemd also reads codec
  register 0x0a8 every 100 ms, since a Polaris does not set the status
  bit.
- **Caller ID.** The codec hears the line on hook (a Polaris's caller ID
  path is switched on while on hook), so caller ID comes in the samples.
- **LEDs.** The OH LED (GPIO 4, lit while set) is lit off hook and
  blinks fast while the line rings. The board's READY/DATA LED is not on
  any GPIO dmodemd has found, so it is not driven.
- **Unplugged.** dmodemd says so and exits.

ATI3 says `Agere USB 2.0 (Mercury)` for either codec. The modem has been
tested against a fake of it ([below](#testing-without-hardware)), but
does not yet work reliably on a real one: [agere-usb.md](agere-usb-details.md#bring-up)
says what to look for, and what to send, on a run.

### Not covered

These are not covered yet, because Linux has no driver that gives their
samples:

- HD-audio modems that are not Si3054-compatible, such as Conexant's.
  (Compatible ones outside the Si3054 list can use `hda-modem-setup`,
  [above](#hd-audio-modems-outside-the-si3054-list).)
- Modems with their own PCI controller: Smart Link SmartPCI, PCTel HSP,
  Lucent/Agere PCI, Conexant HSF PCI, Broadcom BCM4212.

Smart Link's PCI modems are of two kinds:

- Those on an AC'97 controller (Intel ICH, VIA, SiS, ALi, ATI, nForce)
  are AC'97 modem codecs, covered by `-a` above.
- The SmartPCI cards (SL1900, SL2800; PCI vendors 163c, 10a5, 2000 and
  2003) have Smart Link's own controller. Its registers are documented
  only in Smart Link's closed `amrlibs.o`, so there is no open driver to
  start from.

## Options

| Option | What it does |
|---|---|
| `-a <pcm>`, `--alsa=<pcm>` | The line is the codec's PCM (`hw:1`, `hw:0,6`, `plughw:...`): a telephone line, not SIP. |
| `--alsa-ctl=<ctl>` | The codec's controls, if not the card's. By default `hw:<card>` for `hw:` and `plughw:` PCMs (`hw:1` for `hw:1,0` or `plughw:1,0`), and the PCM's own name otherwise. |
| `--usb=<dev>` | The line is a SmartUSB56, pico-serial Line or WB, or Agere USB modem: `auto` (the first one), or `<bus>:<address>` as `lsusb` and `--usb list` show it. The line is a telephone line (a bare pair for the WB), not SIP. `--usb list` lists the devices and exits. |
| `--misdn=<dev>` | The line is an ISDN basic rate interface through Linux's mISDN (its number, or its name such as `hfc-pci.1`), not SIP ([isdn.md](isdn.md)). |
| `--misdn-msn=<msn>` | With `--misdn`: take calls for this MSN alone, and send it as our number. |
| `--misdn-noflip` | With `--misdn`: take the B-channel's octets as the card gives them, not bit reversed. |
| `--usb-leds` | Light a SmartUSB56's carrier and data LEDs (GPIO 6 and 13), as on Creative's Modem Blaster. Only for a board that has them. |
| `--usb-wake` | Let a ring wake the computer: arm the SmartUSB56's USB remote wakeup (and on Linux, as root, enable the kernel's `power/wakeup` for it). |
| `--usb-handset` | Use the SmartUSB56's handset detector (GPIO 2): the phone on its PHONE jack off or on hook gives `<DLE>H` and `<DLE>h` in voice mode. |
| `--usb-voice-codec=<law>` | Use the SmartUSB56's second codec (SSI2, alternate setting 3) as a handset: `+VLS=14` and `15` in voice mode. Its octets are `ulaw`, `alaw` or `linear`. |
| `--usb-buzzer` | Use the SmartUSB56's buzzer (GPIO 11, BUZEN) as the speaker: on while ATM/ATL have the speaker on, as Windows' `Options.Buzzer`. |
| `--usb-rhom` | Set the SmartUSB56's RHOM bit (FIFO_CONTROL2 bit 15, and the voice codec's bit 7) while the modem runs, as Windows' `Options.RHOM`. Not below chip revision 0x200. |
| `--usb-international` | Drive the SmartUSB56's international line controls (GPIO 8, 10 and 12) as Smart Link's Windows driver does: the country's DC bit, Australia's 350 ms after going off hook, and pulse dialling's. |
| `--usb-wb-tx-gain=<dB>` | A pico-serial WB's transmit level: its DAC's volume, -63.5 to +24 dB in 0.5 dB steps (0, the default, is the stick's own). |
| `--usb-wb-rx-gain=<dB>` | A pico-serial WB's hybrid input gain: its MicPGA, 0 (the default) to 47.5 dB in 0.5 dB steps. |
| `-r <Hz>`, `--rate=<Hz>` | The codec's rate. 9600 (the default) or 8000 for the voice-band data pumps; an Agere USB modem always runs at 8000, whatever this says. 48000, 96000 or 192000 for the wideband mode on a bare pair (V.wb, [dsp/vwb.md](dsp/vwb.md)), on a line that has the rate: an ALSA codec, or a pico-serial WB (whose default is 96000), but not the SmartUSB56 or the Line stick. At those rates V.wb is the default modulation (`AT+MS=VWB`), the voice-band pumps are refused (`NO CARRIER`), and `ATD` dials nothing. Any other value is refused. |

Only one of `-a`, `--usb` and `--misdn` can be given.

Started as root, dmodemd opens the codec before it goes on as `nobody`,
so the device's group (usually `audio`) does not matter. Started as an
ordinary user, that user needs access to `/dev/snd/*`.

`SIP_LOGIN` is not needed with `-a` or `--usb`.

## How it works

The ALSA line is `dmodemd/alsa_line.cpp`:

- **Samples.** The PCM runs both ways from the start, on hook too: S16
  mono at the line's rate, in periods of 20 ms, with a buffer of 8
  periods. The playback runs two periods ahead of the capture. Each
  captured frame's answer is written at once, so the two keep their
  distance. An overrun or underrun (dmodemd held up) restarts both ways,
  and the log says so. The modem's clock is the codec's: the number of
  samples it has delivered.
- **The hook.** Off hook (a call, or voice), the "Off-hook Switch" is on
  and the "Caller ID Switch" is off. On hook, the hook switch is off and
  the line monitor is on, so the codec hears the line: rings and caller
  ID. A hook flash (`AT+VTS=!`) turns the hook switch off for its length.
- **Rings.** On hook every sample goes through `dsp::RingDetect`. The
  ring signal, heard through the line monitor, is a large wave of 15 to
  70 Hz, and each cycle of a burst is a pulse to the core's `ring()`.
  That is what a ring indicator line gives. The core checks the cadence
  (`RING`, answering by `ATS0`, and `S1`):
  - a burst of four pulses or more, after a gap of 1.5 s, is a ring;
  - ringing with no such gap (such as Egypt's 2 s on and 1 s off) rings
    every 6 s;
  - a ring counts in `S1` for 8 s.

  Every country of the ITU's list rings this way (`ring_tones_test`,
  [interop.md](interop-details.md#the-line-rung)).
- **Caller ID.** At the first ring the core runs its caller ID receiver
  (`AT#CID`) on the monitor's samples for 4 s.
- **Dialling.** The modem dials the number itself, in DTMF, or in pulses
  with `ATP` or `ATDP`. A `P` or `T` in the dial string switches for the
  rest of it. With `ATX4` (the default) the modem waits for a dial tone
  first; with `ATX3` it waits 2 s instead.
- **Pulse dialling.** The call progress pump times the pulses from the
  country's table (`-c`, `AT+GCI`):
  - the break and the make: 64 and 36 ms for the USA, 67 and 33 ms for
    New Zealand, about 60 and 40 ms for most of Europe;
  - the gap between digits: 800 ms;
  - the digit pattern: n pulses for n and ten for 0; n + 1 in Sweden;
    10 - n in New Zealand.

  The line breaks and makes its hook (`Host::pulse_hook()`): the
  "Off-hook Switch" on ALSA, GPIO 3 on the SmartUSB56. The switch is
  written as the modem's frames are processed, so an edge comes when its
  frame does (20 ms apart on ALSA). 60/40 pulses keep their times; others
  come out within one frame of theirs. `*`, `#` and A to D have no pulses
  and are skipped. A line that cannot pulse (SIP) dials in DTMF.
- **Busy tone.** The pump takes the country's busy and congestion tones
  (`dsp/tones.h`, the ITU's list of 192 countries). It needs four cycles
  of either cadence, within 15% or 80 ms either side, and not the
  country's ringing tone. A country with neither would use the table's
  bounds of on and off times, or 0.2 to 0.8 s.
- **Voice mode.** In voice mode, `ATD` waits 2 s, dials, then takes the
  call as answered when the ringing tone ends (`+VRA`), or after `+VRN`
  if there is no ringing tone (V.253 9.1.1).
- **Modulations.** The core treats the line as an analogue one
  (`Host::analog_line()`): the modem is the analogue end of the call.
  - `+MS`'s V.90 and V.92 (the default) run PCM as the analogue modem at
    9600 Hz (`-r 9600`, the default): up to 56000 bit/s down and 33600 up
    from a V.90 server. With V.92 (and a V.92 server) it adds PCM
    upstream up to 48000 bit/s, quick connect and modem-on-hold (see
    [V.90 analogue modem](dsp/v90a.md)).
  - With any other modem it runs V.34 (up to 33600 bit/s) and below.
  - V.91, which is between two digital modems, becomes V.34.
  - At 8000 Hz (`-r 8000`), V.90 is there only with `DSP_V90A=1`. A
    codec at 8000 Hz on a clock of its own loses the PCM signal every few
    seconds (see [8000 Hz lines](dsp/v90a-details.md#8000-hz-lines)), so
    keep 9600.
  - It has fax (class 1) and voice too.

## Limits

- **Speed.** As the analogue modem, the open DSP has V.90 with its
  retrains and rate renegotiations: a line that changes mid-call
  renegotiates, and one that PCM cannot use falls back to V.34. It also
  has V.92's PCM upstream (with its renegotiations), quick connect and
  modem-on-hold. The analogue modem has been tested against our own
  digital modem on the simulated network only, not yet against an ISP's
  server.
- **Pulse timing.** Pulses keep the modem's frames' timing (20 ms on
  ALSA). They have not yet been tried on a real exchange. `AT&P` (S28's
  pulse ratio) is kept, but the country's table sets the times.
- **Country rules.** The country (`-c`, `AT+GCI`) sets the pulse
  dialling, the busy cadence, and the dial pause and hook flash times
  (S8, S24). `-c` takes any country of the ITU's list too: one the table
  does not have dials with the USA's settings and its own busy tones.
  The codec's line settings (impedance, ring thresholds, current limit)
  are the country's on an HD-audio Si3054 with a Si3018
  ([above](#si3054-line-settings)), and partly on a Conexant SmartDAA
  ([above](#the-country-and-the-board)). Any other ALSA codec keeps its
  kernel driver's settings, the USA's, since ALSA gives no control of
  them. The DTMF levels stay the pump's own.
- **Real hardware.** The code has been tested against the fake card
  below, not yet on a real modem codec.

## Testing without hardware

### The ALSA line

`dmodemd/test/alsa_test.py` (ctest's `dmodemd_alsa`, label `alsa`) runs
two dmodemd on fake ALSA modem cards whose lines belong to a simulated
telephone exchange. It needs libasound2-dev.

- `dmodemd/test/fake_modem_card.cpp` is alsa-lib's external PCM (ioplug)
  and control (ctl_ext) plugins, `dmline`. They are loaded through an
  ALSA configuration of the test's own (`ALSA_CONFIG_PATH`), so dmodemd
  goes through the same ALSA calls as with a real card. The capture is
  what the exchange sends, the playback's position runs on the clock
  like a DMA's, and the control has the two switches.
- `dmodemd/test/line_exchange.cpp` is the exchange. Each number's line
  gets:
  - a dial tone, then its DTMF or pulse digits collected and the number
    routed;
  - for the number called, the ring signal (2 s of 20 Hz, 4 s off,
    heard through the line monitor) and caller ID between the first two
    rings (Bell 202, MDMF); for the caller, the ringing tone;
  - a busy tone if the line called is off hook or unknown;
  - once answered, the call, with a loss of 3 dB, noise, and an echo at
    -24 dB (`LINE_LOSS=<dB>`, `LINE_ECHO=<dB>|off`);
  - a busy tone after the other end hangs up.
- `dmodemd/test/fake_hda_hwdep.cpp` is an HD-audio Si3054's hwdep
  device, put in front of alsa-lib with `LD_PRELOAD`.

In real time, the test checks:

- a data call: the dial tone, DTMF, rings, caller ID, the answer at the
  second ring, CONNECT at both ends, a line of text each way, and the
  hang-up;
- a stall of 0.6 s (SIGSTOP), after which the codec's restart is logged;
- a busy line;
- a call dialled in pulses (`ATDP`), which the exchange counts (a break
  of 200 ms at most is a pulse; 300 ms made again ends the digit);
- a voice call: the ringing tone, caller ID in voice mode's packet
  (`<DLE>X` ... `<DLE>.`), OK at `+VRA`, `+VTS` DTMF across, and the
  busy tone after the other end hangs up;
- with the fake hwdep device, the Si3054 line settings: `-c UK`'s at
  start-up, South Africa's after `AT+GCI=9F` (with RZ cleared off hook
  and set again on hook), and Australia's; nothing written to a codec
  with another chip ID; and the kernel driver's settings kept on a card
  with no hwdep device.

`RATE=8000` runs it at 8000 Hz.

The line settings and the SmartDAA also have unit tests (label `quick`):
`si3054_test` (the Si3054's rows and bits), `si3056_test` (the pico-serial
Line's, on a Si3018 and a Si3019) and `cx_daa_test` (the Conexant
SmartDAA's register access, kinds, hook, and the kind 0x12 setup against
the library's writes, [above](#conexant-ac97-codecs)).

### The SmartUSB56

`dmodemd/test/usb_test.py` (ctest's `dmodemd_usb`, label `alsa`) runs
two dmodemd with `--usb` on the same exchange. It needs libusb-1.0-0-dev.

`dmodemd/test/fake_st7554.cpp` is the part of libusb-1.0 that dmodemd
uses, put in front of the real one with `LD_PRELOAD`. Its one device is
an ST7554 whose line is the exchange's:

- its registers set the hook;
- its IN transfers take the line's samples as the exchange sends them,
  and its OUT transfers' samples go to the exchange;
- its ring detector changes on each half cycle of the ring signal;
- it hears the line on hook only with CID1 set.

One of the two fakes says it is an ST75951/2 (`FAKE_ST7554_DAA`) and
checks its start-up. The test checks:

- that a wrong `<bus>:<address>` is refused;
- a data call: rings, caller ID between the first two rings, the answer
  at the second ring, CONNECT at both ends, text each way; the LEDs of
  the modem with `--usb-leds`, and none on the other; the buzzer of the
  modem with `--usb-buzzer`, on as it dials and off at the CONNECT; and
  no RING from FIFO underruns;
- a busy line, a call dialled in pulses, and a voice call;
- the handset detector in voice mode;
- the international line controls: Australia's timer and pulse
  dialling, France's DC bit, and the ST75951's inverted one;
- the voice codec as a handset (the fake's `FAKE_ST7554_VOICE`: its
  microphone a 1000 Hz tone, its earpiece written to a file): `+VLS=14`
  and `15`, `+VRX` and `+VTX`;
- RHOM (`--usb-rhom`): on while the modem runs, and none on a revision
  0x0100 chip (`FAKE_ST7554_REVISION`);
- a call that wakes a suspended 1002 (`--usb-wake`): the fake's ring with
  its remote wakeup armed, the device reset on resume and set up again,
  and the call answered;
- that both dmodemd exit once the exchange (and so the devices) has gone.

### The pico-serial Line

`dmodemd/test/pico_line_test.py` (ctest's `dmodemd_pico_line`, label
`alsa`) does the same with two pico-serial Line sticks.
`dmodemd/test/fake_pico_line.cpp` is a stick (1209:0002) whose line is
the exchange's, speaking its firmware's protocol: `SET_HOOK` and
`SET_MONITOR` are the exchange's switches, alternate setting 1 starts its
samples, and its ring detector's changes come as events. Besides the
calls, it checks:

- the country's line settings: `-c UK`'s at start-up, Australia's after
  `AT+GCI=09`, and the USA's for a country the Si3056 datasheet has none
  for;
- a stick whose line side does not answer (said, and dmodemd goes on);
- under Wine, a Windows whose WinUSB cannot stream (said, with what to
  do), and UsbDk taken where it is installed.

### The pico-serial WB

`dmodemd/test/pico_wb_test.py` (ctest's `dmodemd_pico_wb`, labels `alsa`
and `quick`; about 20 s) runs two pico-serial WB sticks on a bare pair
instead of an exchange:

- `dmodemd/test/fake_pico_wb.cpp` is a stick (1209:0003) speaking its
  firmware's protocol.
- `dmodemd/test/wb_pair.cpp` is the pair between the two, in real time
  at 96 kHz. It is the V.wb tests' simulated pair
  (`dsp/test/vwb_line.cpp`: 1 km of cable, the transformers, noise, each
  end's hybrid echo, and the two clocks 100 ppm apart). Each end's
  samples are played as the stick plays them: after a head start, and
  silence when they run dry. `PAIR_KM`, `PAIR_SNR`, `PAIR_PPM` and
  `PAIR_ECHO` change the pair.

It checks:

- that a wrong `<bus>:<address>` is refused, and that `--usb list` shows
  the stick;
- that a rate the stick does not have is refused, and the others are set
  (`SET_RATE`); the 96000 Hz default; the gains' options (and one out of
  range refused); V.wb as the default modulation; a codec that does not
  come up (said);
- a call: `AT&L1` at both ends, the answerer by itself (`S0=1`), CONNECT
  at V.wb's rates at both ends, the LINK LEDs on, 32 kB each way at once
  byte for byte through V.42, and the hang-up (NO CARRIER at the other
  end, the LEDs off);
- that both exit once the pair (the devices) has gone.

### The Agere USB modem

`dmodemd/test/agere_test.py` (ctest's `dmodemd_agere`, label `alsa`)
runs two Agere USB modems at 8000 Hz. `dmodemd/test/fake_agere.cpp` is
one (047e:2892) whose line is the exchange's. It has:

- its vendor commands, its ARM's memory and function table, and its
  codec registers (the hook register 0xc0 is the exchange's switch);
- its ROM's isochronous stream: every 2 of the line's samples is a
  packet's time, in which it takes an OUT packet and answers it with an
  IN packet of the samples that have come. The status word's bit 0 is
  set while the ring signal is there;
- a transmit ring of 48 samples, which reports its overruns and
  underruns, and a codec clock that can run off the USB's
  (`FAKE_AGERE_DRIFT`, ppm);
- a Polaris DAA (`FAKE_AGERE_POLARIS`), whose samples stay 0x8000 until
  its link is pulsed up, and whose ring bit can stick after a call
  (`FAKE_AGERE_RING_STICKS`).

It refuses a patch (command 9 stalls), and gives no samples while a
patch's entry is in its function table. It reports its OH LED as it
changes. The test checks:

- that dmodemd says what is wrong with a full speed device;
- that the samples come at 8000 a second: with a bulk patch's entries
  left in the table (put back), with a ROM 1.0.6 (its clock left alone),
  with no active configuration reported, with `DMODEM_AGERE_TRANSFERS`
  and `DMODEM_AGERE_DUMP`, and after the LED probe
  (`DMODEM_AGERE_LED_PROBE`);
- 8000 Hz with `-r 9600`;
- that a Polaris DAA's link comes up and is calibrated (1002 has one);
- a data call: rings (a Polaris's from codec register 0x0a8), caller ID,
  the answer at the second ring, CONNECT at both ends, text each way;
  the OH LED through it; `DMODEM_USB_TX_GAIN` (`auto` and a fixed gain);
  the transmit ring kept with the codec's clock 300 ppm off (no overrun
  or underrun); and the call recorded (`DMODEM_USB_REC`);
- no ring from a ring bit stuck after the call;
- a call dialled in pulses;
- that both exit once the exchange has gone.
