# Softmodem hardware: dmodemd on ALSA and USB: technical details

This page gives the technical data for [Softmodem hardware](hardware.md), which is the overview in plain English. Read the overview first. This page gives all the data: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

Usually, the line of dmodemd is a SIP call. Two options give it a real
telephone line instead:

- With `-a`, the line is the codec of a softmodem, controlled through
  ALSA (the Advanced Linux Sound Architecture, the sound drivers of
  Linux).
- With `--usb`, the line is a USB modem, controlled through libusb.
  dmodemd knows six types: a Smart Link SmartUSB56
  ([below](#smart-link-smartusb56)), a pico-serial Line stick
  ([below](#pico-serial-line)), a pico-serial WB stick, an Agere USB
  2.0 soft modem ([below](#agere-usb-20-soft-modem)), an Apple USB
  Modem ([below](#apple-usb-modem)) and a Conexant USB HSF modem
  ([below](#conexant-usb-hsf-modems)).

The WB stick is not for a telephone line. It connects to a bare copper
pair and runs the wideband mode of D-Modem. All ALSA codecs that run at
96 kHz can do the same ([pico-serial WB](#pico-serial-wb-wideband-bare-pair)).

In all cases, the modem is the open DSP (digital signal processing)
code of D-Modem, which runs on all processor architectures. The
programs of the vendors for this hardware (the slmodemd of Smart Link,
the agrsm of Agere and other similar programs) were closed, and mostly
ran only on 32-bit x86 (i386). dmodemd can replace them.

    dmodemd -a hw:1 -r 9600
    dmodemd --usb=auto

## The hardware

Linux still controls these modem codecs at the level that a softmodem
needs: a stream of samples in each direction, and switches for the hook
and for the line monitor.

| Kind | Kernel driver | Codecs |
|---|---|---|
| AC'97 modem (MC97, AMR and CNR risers; laptops of approximately 2000 to 2006) | `snd-intel8x0m` (Intel ICH to ICH7, AMD-768/8111, nForce, SiS 7013, ALi), `snd-atiixp-modem` (ATI IXP), `snd-via82xx-modem` (VIA) | all codecs whose hook is the AC'97 modem GPIO: Silicon Labs Si3036/38, Agere, Motorola and others; the SmartDAA of Conexant ([below](#conexant-ac97-codecs)) |
| HD-audio modem (laptops of approximately 2005 to 2010) | `snd-hda-intel` with `snd-hda-codec-si3054` | Silicon Labs Si3054/3055 (163c:3055, 3155), Agere (11c1:3026, 3055, 3155), Motorola (1057:3055, 3057, 3155), SM56 (1543:3155), LG (1854:0018), the codec of a VIA board (1106:3288) |

The codec shows as one of the PCM devices of the sound card. The
controls of the card include two switches, "Off-hook Switch" and
"Caller ID Switch". To find them:

    aplay -l                      # a "Modem" device: card 1, device 0 is hw:1
    amixer -c 1 controls | grep -i -e hook -e 'caller id'

An HD-audio modem is a device of the audio card, for example `hw:0,6`.

dmodemd must have the "Off-hook Switch". Without it, dmodemd tells you
that the card is possibly not a modem codec, and stops. Without the
"Caller ID Switch", it continues, but gives a warning that it possibly
cannot receive rings and caller ID on hook.

### Si3054 line settings

On an HD-audio Si3054 with a Si3018 line side, dmodemd also writes the
line settings of the country (from `-c` or `AT+GCI`) into the DAA (data
access arrangement, the line interface) registers of the codec. It gets
access to them through the HD-audio hwdep device of the kernel
(`/dev/snd/hwC<card>D<codec>`), which needs a kernel built with
`CONFIG_SND_HDA_HWDEP`. The code is in
`frontend/dmodemd/si3054.cpp`, `frontend/dmodemd/hda_codec.cpp` and
`frontend/dmodemd/alsa_line.cpp`.

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
spark quenching bits that OHS needs. dmodemd first reads each register,
and changes only the bits of the country.

- Where the table permits ACT 0 or 1 (the TBR21 countries, and Egypt),
  dmodemd uses 1, the complex impedance.
- RZ is cleared off hook, as the datasheet specifies for pulse
  dialling, and set again on hook.
- dmodemd writes the settings again before each off-hook, because the
  driver of the kernel resets the codec on resume.
- A country that is not in the table gets the settings of the USA.

dmodemd writes nothing, and records `line settings: the kernel driver's`
in the log, in three cases:

- on an AC'97 modem;
- on a kernel without the hwdep device;
- on a codec whose chip ID (node 12) is not a Si3054 with a Si3018, for
  example the chips of other vendors that the Si3054 driver binds.

Those codecs keep the settings of the kernel driver, which are the
settings of the USA.

### Conexant AC'97 codecs

The AC'97 modem codecs of Conexant (and of Rockwell) have vendor ID
`CX` with a device ID 0x54xx, or `RS` with 0x53xx. Their hook is in
their SmartDAA, not in the AC'97 modem GPIO that the "Off-hook Switch"
of the kernel controls. The kernel gives their samples, thus `-a`
operates with them, but not for the hook.

On an Intel ICH modem controller (`snd-intel8x0m`, on x86 Linux), when
you start dmodemd as root, it also controls the hook in the SmartDAA.
It gets access to the registers of the codec through the I/O ports of
the controller, under the codec access semaphore that the driver of the
kernel also obeys (`frontend/dmodemd/ac97_ports.cpp`). Then it records
`the hook in its SmartDAA` in the log,
with the version of the line side. Without root, on a different
controller, or on a different codec, it records the reason, and the
switch of the kernel keeps the hook.

The code is `frontend/dmodemd/cx_daa.cpp` and `frontend/dmodemd/cx_country.cpp`.

#### How the SmartDAA's registers are reached

The SmartDAA is two chips: the system side (SSD) in the codec, and the
line side (LSD) on the other side of the isolation barrier. The data
that dmodemd has about them comes from the HSF driver of Conexant for
ICH modems (`hsfmc97ich.o` and its DAA library `hsfsoar.o`): from the
code and from its debug messages.

- **An SSD register** is a byte, numbered 0 to 63, which is accessed
  through codec register 0x7a. A write puts `(reg << 8) | value` into
  0x7a. A read writes `0x8000 | (reg << 8)` and then reads the low byte
  of 0x7a. 0xffff means that there is no SSD. SSD register 3 needs a
  wait of 40 µs before each access.
- **An LSD register** is accessed through SSD registers 1 to 3 and a
  status register:
  - the byte to write goes into SSD register 2;
  - the command goes into SSD register 3: the LSD register in bits 5:0,
    and bit 7 set for a write;
  - the answer comes back in SSD register 1;
  - the status is SSD register 0 (register 4 on some SSDs). Bit 4 means
    done and bit 3 an error. Both are cleared before each command.
    Bit 2 toggles at each frame across the barrier, and bit 1 means that
    the link is up.

  A write to one of the control registers of the line (0x03, 0x14,
  0x15, 0x1c to 0x25) first sets bit 1 of SSD register 0x17. One type
  of SSD (type 0x1a of SOAR) uses bit 6 of the command for them instead.
  A write is good when its answer is the written byte. The driver tries
  5 times (LSD register 0x17 only one time).
- **The hook** is LSD register 0x15, bits 7:6: 01 on hook, 10 off hook,
  and 00 and 11 the make and break of a dial pulse.
- **The kinds of SSD.** The library of the driver knows 39 types of
  SSD. They are different in how they get access to the LSD. Some prime
  SSD register 1 before each command (0x55 for a read, the inverse of
  the byte for a write), so that they can find an absent LSD. Some have
  the status in register 4. Some send the command of a read before they
  clear the status. The ICH driver gets the type from the DAA type of
  the codec, the low byte of register 0x7e:

  | DAA type | 0x20 | 0x21 | 0x22 | 0x23 | 0x28 | 0x29 | 0x30 | 0x31 | others |
  |---|---|---|---|---|---|---|---|---|---|
  | SSD kind | 5 | 0x0c | 0x13 | 0x15 | 0x0d | 0x12 | 0x17 | 0x20 | 5 |

  The SSD of type 0x24 is in the registers of the codec, not behind
  0x7a. dmodemd does the same (`cx_kind()`), and lets the kernel
  control the hook of a type 0x24 codec.

#### What Conexant's library writes

The HSF interop harness (`half_hsf`, [interop.md](interop.md)) runs the
library of Conexant against a simulated SmartDAA of this type (codec
0x5429, SSD type 0x12). The simulated LSD answers as above, reports
version 5, and its register 2 reads 0x0f when it has power. For the
USA, the library then sets up the line with these LSD writes, in
sequence:

- **Start:** 0x03 = 00 and 0x04 = 2f, then it waits until 0x02 reads
  0x0f (the LSD has power). Then 0x20 = 00, 0x25 = 29, 0x24 = 28, 0x21 =
  80, 0x19 = 80, 0x08 = 00, 0x0a = d9, 0x0e = 00, 0x0c = 07, 0x12 = 00,
  0x10 = 00, 0x19 = c0, 0x1f = 08, 0x15 = 40 (on hook), 0x1c = 78, 0x14
  = e0, 0x07 = 04, 0x18 = 43, 0x19 = cb, 0x06 = 02. Then 0x0c = 06, 0x12
  = 60, 0x1c = 30, 0x0e = 00, 0x18 = 43, 0x10 = 00, 0x1c = 68, 0x1c =
  6a, 0x0c = 02, 0x0a = df, 0x10 = 00, 0x0e = 02, 0x10 = 00.
- **ATA (off hook):** 0x15 = 41, 0x12 = 40, 0x0e = 00, 0x14 = 20,
  0x18 = 43, 0x10 = 00, 0x21 = 82, 0x1c = 32, 0x15 = 40, then 0x15 = 80
  (off hook) and 0x19 = 8b.

Before the LSD start, it writes SSD registers directly: 0x1c = 40, 0x17 =
04, 0x14 = 08, 0x0b = 5f, 0x06 = c0, 0x19 = 20; 0x05, 0x04 and 0x07 to
0x0a = 00; 0x0d = 10, 0x1e = 00, 0x19 = 28, 0x1c = 7f; 0x1a, 0x1b and
0x1d = 00; 0x25 = 5a, 0x23 = 43, 0x24 = 80, 0x22 = 5a, 0x20 = 63, 0x21 =
a8.

The status registers of the simulated LSD (0x16, 0x1a, 0x1e, 0x1f) read
0. On a real line, the library can write other values where it adjusts
to the values that it measures (the loop voltage, the drive).

Two runs show which bits each write of the library keeps. In the first
run, the registers of the simulated line side start at 0, and in the
second at 0xff (`HSF_DAA_FILL=0xff`). A third run, over two calls,
shows its hang-up:

- bits 1:0 of SSD register 0x14 back to 0;
- bits 6:5 of LSD 0x12 = 11, 0x15 = 40, 0x19 = cb, 0x0e = 00, 0x21 = 80,
  bits 7:2 of 0x14 = 111000, 0x18 = 43, 0x10 = 00, bits 6:0 of 0x1c = 6a,
  0x10 = 00, 0x0e = 02, 0x10 = 00.

The off-hook sequence ends with bits 1:0 of SSD 0x14 = 01.

#### What dmodemd does

For a codec of type 0x12, dmodemd does the same as the library
(`CxDaa::power_up()`, `go_off_hook()`, `go_on_hook()`):

- the writes of the SSD;
- the power-up of the LSD, and a read of its version (0 to 0x11, the
  versions that the library accepts);
- the writes of the line, which keep the bits that the library keeps;
- the sequences of the hook, which include the 100 ms that the library
  waits after 0x15 = 41.

A dial pulse changes only the hook bits. `cx_daa_test` compares the LSD
writes with the writes of the library, from the two start states.

For the other types of SSD, the setup is not known. dmodemd reads the
version of the line side and changes only the hook bits. That holds the
line only if a different program has set up the line side.

#### The country and the board

The parameters of the country and of the board come from Conexant, in
the same way as the parameters of the other backends come from their
datasheets. `cx_country.cpp` is generated from the files of the HSF
driver.

- **The country.** Of the line settings above, only bits 6:5 of LSD
  0x12 change with the country. This was found when the library ran for
  a country from each of the 14 `SMART_RELAYS` tables of `hsf.cty`
  (`HSF_COUNTRY=<T.35 code>`). The two bits are bits 7:6 of the low byte
  of the off-hook and on-hook state words of the table. There are three
  settings:

  | Setting | On hook | Off hook |
  |---|---|---|
  | US (the USA, Japan, ...) | 11 | 10 |
  | ITU (most countries) | 10 | 10 |
  | TBR21 (its countries) | 00 | 00 |

  The table has all 242 regions of `hsf.cty` under their own names, and
  also the names of D-Modem for the three regions that it names
  differently (BAHREIN, SOUTHKOREA, UAE). `-c` and `+GCI` select the
  row, as for the Si3054. On hook, a new setting goes to the line side
  immediately.
- **The board.** dmodemd uses the `SmartDAAParamsK3` block of the
  `.inf` for DAA types 0x22 to 0x24, 0x29, 0x30 and 0x31, and its
  `SmartDAAParams` block for the other types, as the ICH driver selects
  (95 bytes each). A change of each byte of the block, one at a time
  (`HSF_NVM` with an edited `.inf`), showed that three bytes change the
  writes of the library:

  | Byte | Goes to |
  |---|---|
  | 87 | SSD 0x21, as 0x80 \| byte |
  | 93 | SSD 0x0b |
  | 94 | SSD 0x06's bits 7:4 |

  The other bytes (its resistors and DC tables) change nothing on the
  line sides that dmodemd sets up. The library uses them only for a line
  side of version 0x11, which it measures (below).

`cx_daa_test` compares the writes with the writes of the library for
the USA, Germany and Canada, from the two start states.

#### Line side versions

The version of the line side (bits 5:0 of LSD 0x22) changes what the
library does. Each version had a test with `HSF_LSD_VERSION` in the
harness:

| Version | What the library does |
|---|---|
| 1, and 3 to 0x0f | the setup above |
| 2 | the same, but with three differences: no writes to 0x24 and 0x25; bit 7 of 0x21 clear in each of its writes (00 and 02, not 80 and 82); and the plain writes of 0x19 with bit 5 set (eb and ab, not cb and 8b) |
| 0x10 and 0x11 | a fully different setup (0x1d, 0x1f = 30, 0x15 = c0 on hook and 00 off hook: bit 7 of the hook is inverted); for 0x11, the library also measures the line (below) |
| 0 | it reads the monitor ADC again and again |

Real line sides have version 2: the `hsfconfig --info` of Conexant has
shown `SmartDAA SSD=10 LSD=0x2` for an HSF PCI modem.

dmodemd does the setup of the library for versions 1 to 0x0f, with the
differences of version 2 (`cx_daa_test` also checks the writes of
version 2). For versions 0, 0x10 and 0x11, it reads the version and
changes only the hook bits, with bit 7 inverted for 0x10 and 0x11.

#### The line side's monitor ADC

- Bits 6:4 of LSD 0x16 select the channel. Its bit 3 starts a
  conversion and reads 1 when the conversion is done.
- LSD 0x17 is the reading (8 bits, written back when read).
- The library calculates the value as reading × 2000 / 255 mV plus an
  offset of 500 mV. For version 0x10, the offset is calibrated on
  channel 3, against a reference that must be 1350 to 1650 mV. Versions
  0 and 2 have 4000 mV for 255 (and version 0 has 7 bits).

The library measures the line with it only on version 0x11:

- on hook, the battery on channel 1 (Vbatt = 28.2 × the mV of the
  reading + 400; the factor comes from the resistors of the board);
- off hook, channel 0 (the DC offset of the receiver), then channel 1
  again (the tip-ring voltage, from which it calculates the resistance,
  current and power of the loop);
- a DAC (LSD 0x19), calibrated on channel 4 and then set so that the
  tip-ring voltage agrees with the DC curve, and adjusted while the line
  becomes stable ("adaptive offhook").

dmodemd does none of this, because it does no version 0x11 setup.

None of the Conexant code has run on a real codec yet.

### HD-audio modems outside the Si3054 list

`snd-hda-codec-si3054` binds only the vendor IDs given above. A modem
codec made to the same interface under a different ID gets no driver,
and thus no ALSA device. `snd-hda-intel` can give such a codec the
vendor ID of the Si3054 through its early patching
(`CONFIG_SND_HDA_PATCH_LOADER`, enabled in the kernels of most
distributions). Then the Si3054 driver binds it. `hda-modem-setup`
(installed with dmodemd, on Linux) does this:

    hda-modem-setup               # list the modem codecs
    sudo hda-modem-setup write    # the patch for those with no driver
    sudo reboot
    hda-modem-setup list          # now driven by snd_hda_codec_si3054?

The three actions:

- **`list`** (the default) shows each codec with a modem function group
  (from `/sys/bus/hdaudio/devices`): its card and address, its IDs, its
  driver, and if it is in the Si3054 list.
- **`write`** selects the modem codecs that have no driver and are not
  in the list, or the codecs that you name with
  `--codec hdaudioC<card>D<addr>`. For each, it writes a `[codec]`
  section, with `[vendor_id] 0x163c3055`, to
  `/lib/firmware/dmodem-hda-modem.fw`. It also writes the module option
  that loads the file for the card of each such codec to
  `/etc/modprobe.d/dmodem-hda-modem.conf`
  (`options snd-hda-intel patch=...`). Before it writes, it checks the
  configuration of the running kernel for the patch loader and the
  Si3054 driver. It gives a warning if a different file already gives
  `snd-hda-intel` a `patch=` option, because only one has an effect.
- **`remove`** deletes the two files.

The change has an effect the next time that `snd-hda-intel` loads. If
the module is in the initramfs, first build it again
(`update-initramfs -u`, `dracut -f`).

Only the driver can tell if the codec is really Si3054-compatible,
after it has bound it. At its init, it checks the link to the line side
chip and records the ID of the chip in the log. "Link Frame
Detect(FDT) is not ready" in `dmesg` means that the codec is not
compatible: run `sudo hda-modem-setup remove`, and reboot.

The HD-audio modems of Conexant (`14f1:2bfa`, `14f1:2c06` and the other
modems of vendor `14f1`) are of a different type. Their DAA is behind
the verbs of Conexant, and this procedure does not include them.

`frontend/dmodemd/test/hda_modem_setup_test.py` (`hda_modem_setup` of ctest, label
`quick`) runs the helper on a fake sysfs and kernel configuration. The
patch has not had a test on a real codec.

### Smart Link SmartUSB56

The USB modem of Smart Link (the SmartUSB56, and the modems built on
its ST7554 bridge, USB `0483:7554`) does not need a kernel driver:
dmodemd controls it through libusb. The ST7554 is a USB bridge to an
STLC7550 codec and a DAA:

- its registers are vendor control requests;
- its samples are isochronous transfers in the two directions;
- its GPIOs are the hook and the ring detector.

```
lsusb -d 0483:7554            # Bus 001 Device 007: ID 0483:7554 ...
dmodemd --usb=auto            # the first one; --usb=1:7 that one
```

When you start dmodemd as root, it opens the device before it
continues as `nobody`. To start it as an ordinary user, give that user
the device with a udev rule, for example
`/etc/udev/rules.d/60-smartusb56.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="7554", MODE="0660", GROUP="dialout"

If the kernel driver of Smart Link (`slusb`) is loaded, dmodemd
detaches it from the device while it runs. The build includes `--usb`
in dmodemd when pkg-config finds libusb-1.0 (the libusb-1.0-0-dev
package). `dmodemd --usb list` gives the SmartUSB56s, pico-serial Lines
and WBs and Agere modems that are there, and if each can open. On
Windows, dmodemd.exe controls them in the same way, on WinUSB (or on
libusbK or UsbDk on Windows 7 and 8, whose WinUSB cannot stream)
([windows.md](windows-details.md#usb-modems---usb)).

The register values and start-up of the ST7554 are those of the Linux
driver of Smart Link for it (`drivers/st7554.c` of slmodem 2.9.11, under
a BSD licence). The code is in `frontend/dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 9600 Hz (the default) or 8000
  Hz, in isochronous packets of 1 ms. Three transfers of five packets
  are in flight in each direction. Each OUT transfer is sent when an IN
  transfer completes, with the packet lengths of that IN transfer. Thus
  the playback follows the clock of the codec. The OUT transfers take
  the samples in the queue of the modem. The queue starts a frame and a
  half (30 ms) in front. The clock of the modem is the clock of the
  codec: the number of samples that it has delivered.
- **The hook.** GPIO 3, with the hook LED on GPIO 7. A hook flash
  clears GPIO 3 for the length of the flash.
- **Rings.** The ring detector (GPIO 0) changes with the ring signal.
  Each change comes on the interrupt endpoint and is a pulse to the
  `ring()` of the core, which checks the cadence. dmodemd does not count
  a change that comes with a FIFO underrun or overrun, because the ring
  bit can come set with those when the line rings and when it does not.
- **Caller ID.** On hook, the codec receives nothing unless GPIO 5
  (CID1) connects it to the line. With `AT#CID=1`, dmodemd sets CID1
  from the first ring while the caller ID receiver of the core listens,
  and clears it before the hook changes, as the Windows driver of Smart
  Link does (its Linux driver never did). Thus caller ID is received
  between the first two rings.
- **The codec.** An STLC7550, or an ST75951/2 "silicon DAA"
  (`MONOLITHIC`, bit 14 of the GPIO status). The control words of the
  ST75951 name one of its four registers. dmodemd starts it up as the
  Windows driver does. The hook, ring and caller ID use the same GPIOs.
- **LEDs.** The hook LED (GPIO 7) follows the hook. With `--usb-leds`,
  dmodemd also controls two more LEDs, as the Windows driver of Smart
  Link does on boards whose INF specifies them (the Modem Blaster V.92
  USB of Creative):
  - the carrier LED (GPIO 6), on while the call is online;
  - the data LED (GPIO 13), on when data has gone through in the call.

  The two go off on hook. Without the option, dmodemd does not change
  those GPIOs, because a different board can use them for a different
  function.
- **Wake on ring.** With `--usb-wake`, dmodemd arms the USB remote
  wakeup of the device (SET_FEATURE) if its configuration specifies that
  it can wake, as the Windows driver of Smart Link does when the PC
  sleeps. The interrupt of the ring detector stays on while on hook,
  thus a ring wakes the computer. The kernel must also permit it:
  - On Linux, dmodemd writes `enabled` to the
    `/sys/bus/usb/devices/<bus>-<ports>/power/wakeup` of the device
    while it is still root. If you start it as a user, add
    `ATTR{power/wakeup}="enabled"` to the udev rule above.
  - On Windows, it is "Allow this device to wake the computer" in
    Device Manager.

  dmodemd clears the remote wakeup when it exits.
- **Suspend and resume.** While the computer sleeps, the transfers
  fail. dmodemd keeps them and, each second, sets up the device again
  (its registers, the remote wakeup and the FIFO, because a resume can
  reset it) and submits them again. It stops after 30 s, as for a
  device that is not there. A call is lost across a sleep. dmodemd
  receives the rings after the ring that woke the computer as usual,
  thus the modem can still answer.
- **The handset detector.** With `--usb-handset`, dmodemd enables the
  interrupt of GPIO 2 (`GPIO_HANDSET` in the Linux source of Smart Link;
  the Windows driver also enables it, and does nothing with it). On each
  change, it reads the GPIO. Set possibly means that the phone on the
  PHONE jack of the modem is off hook (no driver tells). The core
  reports it in voice mode as the local phone event of V.253: `<DLE>H`
  off hook and `<DLE>h` on hook. It gives event 8 in `+VLS=?`.
- **International line controls.** With `--usb-international`, dmodemd
  controls three more outputs, as the Windows driver of Smart Link does
  with its `Options.International` (off in the INF of Creative):
  - **DP** (GPIO 10): set off hook; cleared on hook, together with RFC.
  - **DC** (GPIO 12): set off hook in France, Algeria and Tunisia, the
    countries whose record asks for it (byte 90, bit 0, of the table of
    Smart Link). On an ST75951 it is the opposite: set off hook in all
    other countries.
  - **RFC** (GPIO 8): set for 350 ms after each off-hook in Australia
    (bit 1, "the Australian timer"), and during all of pulse dialling.
    During pulse dialling, dmodemd does not change DP and RFC, and
    clears DC on each break.

  The function of the three in the DAA is not known: only the names
  that Smart Link gives them are known. They are for a board built for
  these countries. The country is the one that `-c` or `+GCI` sets.
  dmodemd gets the start and end of pulse dialling from the call
  progress pump (`MDMPRM_PULSE_DIALLING`).
- **The voice codec (a handset).** With `--usb-voice-codec=<law>`,
  dmodemd selects alternate setting 3 instead of 1. Its second pair of
  isochronous endpoints is the SSI2 codec of the ST7554 (the
  `K7554VoiceCodecInterface` of Smart Link: 8000 octets each second,
  powered up with SSI_CTRL = 0x21, its period in register 6). In voice
  mode, `+VLS=?` then gives two more V.253 devices:
  - **14** (`"H"`): the handset alone, the line on hook. The samples of
    the voice engine are the samples of the handset: its microphone goes
    to `+VRX`, and `+VTX` goes to its earpiece.
  - **15** (`"HT"`): the handset on the call, off hook. The samples of
    the voice engine are the samples of the line, and dmodemd mixes the
    handset in the two directions: the samples of the line and of the
    modem go to the earpiece, and the microphone goes to the line and to
    `+VRX`.

  Linear interpolation connects the rates of the two codecs. The format
  of the octets is not known: the Windows driver calls its format 3 and
  never uses it, and streams this codec only from its test interface.
  Thus the option accepts `ulaw`, `alaw` or `linear` (signed 8-bit).
  The INF of Creative uses alternate setting 1, thus possibly the DE5671
  has no codec there. dmodemd refuses a board whose alternate setting 3
  does not have these endpoints.
- **RHOM.** With `--usb-rhom`, dmodemd sets bit 15 of FIFO_CONTROL2
  (and bit 7, the bit of the voice codec, with `--usb-voice-codec`)
  while the modem runs (from the start of the line to its stop). The
  Windows driver of Smart Link does this with `Options.RHOM`
  (`ConfigureRHOM`, `EnableRHOM`), which the INF of Creative leaves off.
  A chip whose revision (register 0) is less than 0x200 never gets it,
  as on Windows. The function of RHOM is not known.
- **The buzzer.** With `--usb-buzzer`, the speaker of the modem is
  GPIO 11 (BUZEN). It is on when the core has the speaker on at any
  volume, and off at other times:
  - ATM1 (the default): on from dialling or answering until the CONNECT;
  - ATM2: always on;
  - ATM0, or ATL0: never on.

  The Windows driver of Smart Link does this in `SetSpeakerVolume` with
  `Options.Buzzer` (off in the INF of Creative). A buzzer has no volume,
  thus ATL1 to ATL3 are the same. Not all boards have one.
- **Unplugged.** dmodemd tells you and exits.

Limits: the line settings of the device are its own. dmodemd has not yet
had a test on a real SmartUSB56. Caller ID and the ST75951 follow the
Windows driver (read from its code, [smartusb56-windows.md](smartusb56-windows.md))
and have had a test only on the fake device.

### pico-serial Line

The phone-line stick of the [pico-serial](https://github.com/D-Modem/hardware)
project ([SPEC-L.md](https://github.com/D-Modem/hardware/blob/HEAD/SPEC-L.md))
is a USB-C stick with an RJ11 socket, made for D-Modem. It is only the
line of a softmodem: a Skyworks Si3056 DAA and its Si3018 line side
behind an RP2040 microcontroller, at 9600 Hz so that the V.90 of the
open DSP operates. dmodemd controls it through libusb, as it does the
SmartUSB56, and it does not need a kernel driver.

    lsusb -d 1209:0002            # Bus 001 Device 008: ID 1209:0002 ...
    dmodemd --usb=auto            # the first SmartUSB56 or stick; --usb=1:8 that one

A udev rule to start dmodemd as a user, for example
`/etc/udev/rules.d/60-pico-serial-line.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0002", MODE="0660", GROUP="dialout"

Its protocol is the protocol of its firmware (`firmware/PROTOCOL.md`,
"Line stick"). It is intentionally similar to the protocol of the
ST7554. The code is in `frontend/dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 9600 Hz (the default) or 8000
  Hz, isochronous in the two directions in alternate setting 1 of
  interface 0. The selection of that alternate setting starts them. The
  rate is set first (`SET_RATE`). Each IN packet holds the samples that
  the codec of the stick gave in that millisecond. Each OUT transfer is
  sent with the packet lengths of an IN transfer, as for the ST7554.
- **The hook.** A vendor request (`SET_HOOK`). The stick itself times a
  hook flash (`HOOK_FLASH`, 1 to 2000 ms). Pulse dialling sends
  `SET_HOOK` for each break and make, while the frames of the modem are
  processed.
- **Rings and caller ID.** On hook, the line monitor of the stick is on
  (`SET_MONITOR`), thus it receives the line: **caller ID** comes in its
  samples, as on an ALSA codec. Rings come from its ring detector. Each
  change that it counts arrives in an 8-octet event on the interrupt
  endpoint and is a pulse to the `ring()` of the core, as each change of
  the ring GPIO of the ST7554 is.
- **At start-up**, dmodemd reads the protocol version, firmware version
  and line side of the stick (`GET_INFO`). It refuses a protocol version
  other than 1. If the line side does not answer across the isolation
  barrier, it tells you and continues. ATI3 shows
  `pico-serial Line (Si3056)`.
- **The country's line.** dmodemd writes the settings of the country
  (from `-c`, and again on each `AT+GCI`) into the DAA:
  - the AC termination: ACT and ACT2 on the Si3018 line side of the
    stick, or ACIM on a Si3019;
  - the DC termination (DCV, MINI);
  - the TBR21 current limit (ILIM);
  - the ringer impedance and threshold (RZ, RT);
  - the on-hook speed (OHS, OHS2, with the spark quenching of
    Australia).

  The values are from the Si3056 datasheet (Rev. 1.03, Table 15,
  "Country Specific Register Settings"), in
  `frontend/dmodemd/si3056.cpp`. dmodemd first reads each register (16,
  26, 31 and 59, and 30 on a Si3019) (`DAA_READ`), and changes only the
  bits of the country (`DAA_WRITE`). If you make a change while off
  hook, it waits until the line is on hook again.

  Countries that have no row in the table (Algeria, Brunei, Estonia,
  Lithuania, Sri Lanka, Tunisia, Turkey, Uruguay, Vietnam and most of
  the ITU list) keep the settings of the USA, which are the reset values
  of the Si3056. The log of dmodemd tells you this (`-d1`). The TBR21
  row of the datasheet is `-c CTR21EUROPE`.
- **Unplugged.** dmodemd tells you and exits.

Limits:

- The transmit and receive gains stay at their reset values (Table 15
  gives none).
- Australia keeps its Japan-mode DC termination during all of the call.
  The datasheet gives an option to change to FCC mode 500 ms after the
  off-hook, for a higher transmit level. dmodemd does not use it.
- The USB ID of the stick is a pid.codes test PID until it has its own.
- It has had a test with a fake of it ([below](#testing-without-hardware)),
  but not yet on a real stick.

### pico-serial WB (wideband, bare pair)

The wideband stick of pico-serial ([SPEC-WB.md](https://github.com/D-Modem/hardware/blob/HEAD/SPEC-WB.md))
is the sibling of the Line stick for a **bare copper pair**: a dry
pair, an alarm or leased circuit, or a pair of your own between
buildings. It is not for a telephone line. Two WB sticks communicate
point to point through a wideband transformer, and sample the pair at
96 kHz instead of 9.6 kHz. They use a band of approximately 4 to 36 kHz.

This is the line for the wideband mode of D-Modem: V.wb, `AT+MS=VWB`
([dsp/vwb.md](dsp/vwb.md)), at 48000 Hz and higher (`-r 96000`). It
runs with `AT&L1`, a leased line with no dial tone or ringing: `ATD`
calls, and `ATA` or `ATS0=1` answers. In simulation, it carries
approximately 149 kbit/s in each direction on a short pair, and
approximately 70 kbit/s on 5 km.

dmodemd controls the stick through libusb, as it controls the Line
stick, and it does not need a kernel driver. `--usb=auto` finds it
together with the other USB modems. With no `-r`, it runs at the
96000 Hz of the stick:

    lsusb -d 1209:0003            # Bus 001 Device 009: ID 1209:0003 ...
    dmodemd --usb=auto            # 96000 Hz; -r 48000 or -r 192000 for those

A udev rule to start dmodemd as a user, for example
`/etc/udev/rules.d/60-pico-serial-wb.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0003", MODE="0660", GROUP="dialout"

At the two ends of the pair, one end calls and the other end answers
automatically:

    AT&L1S0=1                     (the answering end, once)
    AT&L1                         (the calling end)
    ATD
    CONNECT 148656

At 48000 Hz and higher, V.wb is the modulation by default (`AT+MS?`
shows 200), because no other modulation runs there. You do not need
`AT+MS=VWB`. A leased line has no hang-up signal. When one end hangs up
(`ATH`), the other end shows `NO CARRIER` when V.42 tells it, or when
the carrier has been absent for S7 seconds (`ATS7=10` for 10).

Its protocol is the protocol of its firmware (`firmware/PROTOCOL.md`,
"WB stick", and `src/core/wb_proto.h`): the USB device of the Line
stick, with the own requests of the WB stick. The code is in
`frontend/dmodemd/usb_wb.cpp`, on top of the transfers of
`frontend/dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 96000 Hz (also 48000 and
  192000 Hz), isochronous in the two directions in alternate setting 1
  of interface 0, which starts them. The packets are 1 ms, of up to 480
  octets. Each IN packet holds the samples that the codec of the stick
  gave in that millisecond. Each OUT transfer is sent with the packet
  lengths of an IN transfer, as for the Line stick. `SET_RATE` (in kHz)
  is sent first. dmodemd refuses a rate that the stick does not have
  (`-r 8000` or `9600`).
- **At start-up**, dmodemd reads the protocol version and board of the
  stick (`GET_INFO`). It refuses a protocol version other than 1 or a
  board that is not the WB stick. Then it turns off all loopback
  (`SET_LOOPBACK`), streams the input of the hybrid (`SET_INPUT`: the
  line minus our own signal, not the line itself), sets the rate and the
  gains, turns off the LINK LED, and tells you if the codec does not run
  (`GET_STATUS`). ATI3 shows `pico-serial WB (TLV320AIC3204)`.
- **Gains.**
  - `--usb-wb-tx-gain=<dB>` sets the transmit level: the DAC volume of
    the codec (`SET_TX_GAIN`), -63.5 to +24 dB in 0.5 dB steps.
  - `--usb-wb-rx-gain=<dB>` sets the gain of the hybrid input: its
    MicPGA (`SET_RX_GAIN`), 0 to 47.5 dB.

  The two are 0 by default, the levels of the stick: approximately
  +3 dBm on the line for a full-scale sine. This puts the signal of V.wb
  (11 dB below full scale) at approximately -8 dBm.
- **The LINK LED** shows the carrier of the modem: on while the call is
  online (also through a retrain), off at other times (`SET_CARRIER`).
  At the end, dmodemd gives it back to the stick, which turns it on
  while its samples run.
- **No hook, ring or caller ID.** The hook of the modem only starts and
  stops the call. Nothing rings, and `ATS0=1` answers only with
  `AT&L1`.
- **Events.** The log records over current on the line drivers as an
  error. It records the overflows of the codec (the transmit level or
  the gain of the hybrid too high) and the underruns and overruns of the
  stick at `-d1`.
- **Unplugged.** dmodemd tells you and exits.

It has not yet had a test on a real stick or a real pair. It has had a
test with a fake of the stick ([below](#testing-without-hardware)) on a
simulated pair, with the protocol of the firmware as its sources give
it. These items must still have a check on hardware:

- **The real delay of the stick.** The filters of its codec are not
  counted in the `io_delay` of dmodemd. V.wb measures its own echo and
  does not need it.
- **The levels.** The default gains (0 dB) have had no test against the
  residue of a real hybrid and the loss of a real pair. On a long pair,
  it is possibly necessary to increase the receive gain.
- **V.42 through the buffers of the host.** dmodemd sends its samples
  4672 samples in front at 96 kHz, approximately 50 ms: a frame and a
  half of 20 ms, 15 ms of transfers, and the buffers of the stick. The
  window of LAPM of 15 frames of 128 octets waits a round trip. In
  simulation, with that lead at the two ends, the 148656 bit/s of V.wb
  carried 83 kbit/s in each direction through V.42, against 137 kbit/s
  with no buffers.
- **The USB ID.** The USB ID of the stick is a pid.codes test PID until
  it has its own.

All ALSA codecs that run at 96 kHz (`-a hw:N -r 96000`, through a line
transformer) are also a line for V.wb. libdmodem runs it at each of the
three rates (`dmodem_create_rate()`).

### Agere USB 2.0 soft modem

The USB soft modem of Agere (later of LSI), USB `047e:2892` (and
`047e:2828`), is also sold under other names. It has no kernel driver:
dmodemd controls it through libusb. It is an Agere USB controller with
an ARM core in it, and behind that a Mercury codec or a "Polaris" DAA.
Its ROM itself streams the samples on isochronous endpoints, as in the
Windows driver of Agere. Thus dmodemd does not download code into it.
It needs a USB 2.0 (high speed) port. On a full speed port, dmodemd
tells you and stops.

    lsusb -d 047e:                # Bus 001 Device 009: ID 047e:2892 ...
    dmodemd --usb=auto

The line runs at 8000 Hz, and dmodemd ignores the value of `-r`. A udev
rule (`/etc/udev/rules.d/60-agere-usb.rules`) lets dmodemd open it
without root:

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

[agere-usb.md](agere-usb-details.md) has all the data, read from the ROM
of the device and the drivers of Agere. In summary,
`frontend/dmodemd/usb_agere.cpp` does these steps:

- **Start-up.** Alternate setting 1 of interface 0 (isochronous IN and
  OUT), the version of the ROM, its GPIOs and clock, and the registers
  of its codec as the drivers of Agere write them. Bit 15 of codec
  register 0xc0 shows if it is a Polaris or a Mercury. A Polaris gets a
  pulse to bring its link up, its calibration and the DAA registers for
  its country. Then on hook. Then dmodemd restores the function table of
  the ROM (it removes all patches from a previous driver), increases the
  clock, and starts the stream of the ROM (command 0x0c).
- **Samples.** A packet in each direction each 2 microframes (4000 each
  second): a header (the count of the samples and a status word), then
  the samples. Sixteen transfers of 24 packets are in flight in each
  direction. Each OUT transfer carries 48 samples (2 in each packet), the
  rate of the codec. Sometimes it has one sample more or less, to follow
  the drift between the USB clock and the codec clock.
- **Hook.** Codec register 0xc0: 0 is off hook on a Mercury, 1 on a
  Polaris. Pulse dialling and the hook flash break and make it.
- **Rings.** Bit 0 of the status word of the IN packets: a pulse to the
  `ring()` of the core for each packet that has it. On hook, dmodemd
  also reads codec register 0x0a8 each 100 ms, because a Polaris does
  not set the status bit.
- **Caller ID.** The codec receives the line on hook (the caller ID path
  of a Polaris is on while on hook), thus caller ID comes in the
  samples.
- **LEDs.** The OH LED (GPIO 4, on while set) is on off hook and
  flashes quickly while the line rings. dmodemd has not found the
  READY/DATA LED of the board on a GPIO, thus it does not control it.
- **Unplugged.** dmodemd tells you and exits.

ATI3 shows `Agere USB 2.0 (Mercury)` for the two codecs. The modem has
had a test with a fake of it ([below](#testing-without-hardware)), but
it is not yet reliable on a real one: [agere-usb.md](agere-usb-details.md#bring-up)
tells what to look for, and what to send, after a run.

### Apple USB Modem

The USB modem of Apple for Macs (A1101, MA034Z/A), USB `05ac:1401` (and
`05ac:8220`, which the driver of Apple also accepts), is the SM56 USB
soft modem of Motorola with a Silicon Labs Si3056 DAA. dmodemd controls
it through libusb (`frontend/dmodemd/usb_apple.cpp`).

    lsusb -d 05ac:1401            # Bus 001 Device 007: ID 05ac:1401 Apple, Inc. Modem
    dmodemd --usb=auto

It operates as the Mac OS X drivers of Apple do:

- **Samples:** USB audio class 1, an isochronous stream in each
  direction (interfaces 3 and 4, alternate setting 1), 16-bit mono at
  the `-r` rate, set with `SET_CUR` on each endpoint. dmodemd refuses a
  rate that the device does not list, and gives the rates that it has.
- **Hook:** the `SET_HOOK_STATE` of CDC to interface 0 (Direct Line
  Control).
- **Rings and the phone cord:** the interrupt notifications of interface
  0 (`RING_DETECT`, and 0x29 for the cord).
- **DAA:** the registers of the Si3056 through 3-octet commands to
  interface 1 (`SEND_ENCAPSULATED_COMMAND`, the value of a read with
  `GET_ENCAPSULATED_RESPONSE`): the line side powered up, its link
  checked, the on-hook line monitor for caller ID, the registers of the
  country (`-c`, `AT+GCI`).

A udev rule (`/etc/udev/rules.d/60-apple-usb-modem.rules`) lets dmodemd
open it without root:

    SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="1401", MODE="0660", GROUP="dialout"

ATI3 shows `Apple USB Modem (Si3056)`. It has had a test with a fake of
it ([below](#the-apple-usb-modem)), but not yet on a real one.
[apple-usb-modem-details.md](apple-usb-modem-details.md) has the full
protocol and the source of each part.

### Conexant USB HSF modems

The USB soft modems of Conexant use its "Cadmus 2" USB bridge: USB
`0572:1300` to `0572:1303`, `08e3:0111`, `0803:1300`, `0803:1301`,
`145f:0106`, `148d:1671` and `148d:1672` (the IDs that the Linux driver
of Conexant, the `hsfusbcd2` module of hsfmodem, accepts). dmodemd
controls the bridge through libusb (`frontend/dmodemd/usb_cadmus2.cpp`),
with its own modem instead of the modem of Conexant.

    lsusb -d 0572:                # Bus 001 Device 009: ID 0572:1300 Conexant Systems ...
    DMODEM_CD2_FIRMWARE=/path/to/c2firmware.h dmodemd --usb=auto

The procedure is what the `hsfusbcd2` module does with it. The interop
harness showed this when it ran the module against a simulated bridge
(`half_hsf_usb`, [interop-details.md](interop-details.md)):

- **Firmware:** the bridge starts with no firmware (`GET_INFORMATION`,
  vendor request 0, tells this) and the host uploads it, 64 octets at a
  time (vendor request 1) with the device unconfigured. Then the host
  configures it again when it reports that the firmware runs. The
  firmware is from Conexant (`ROM_IMAGE[]` in the
  `modules/imported/include/c2firmware.h` of hsfmodem, 7395 octets), and
  we must not distribute it: `DMODEM_CD2_FIRMWARE` names that file, or a
  file of its bare octets. A bridge that already runs it (a second
  start, with or without an unplug between) does not need it.
- **Scripts:** the bridge runs small programs that the host sends
  (vendor request 2, `wValue` `0xff01`, or `0xff02` for the one that
  stays; `wIndex` the slot). Their reports come on its interrupt
  endpoint (`0x82`). dmodemd sends the scripts that the module sends to
  a plain board, byte for byte: the start, the set-up words of the codec
  (`2004`, `a208`, `f200`, `aae8`, `6040`, `35b4`), the monitor (slot 3),
  and the start of the streams.
- **Hook:** the codec word `aac8` off hook, `aae8` on hook, as the
  module writes them; pulse dialling the same.
- **Samples:** bulk transfers, `0x81` in and `0x01` out, 64 samples
  (128 octets) each, 16-bit little-endian mono at 16000 Hz; one transfer
  out for each transfer in. The modem runs at 9600 Hz (dmodemd ignores
  the value of `-r`) and the samples are converted in the two directions
  (`dsp::Resampler`, 3/5 and 5/3). The module starts the streams only
  off hook. dmodemd runs them from the start, as for the other devices.
- **Rings:** the report `08 01` of the monitor (its channel 4). Two or
  more of them are a ring.

Not covered yet:

- **SmartDAA boards** (the reply to the start tells the type of a
  board): their line side is behind other scripts, which are not known
  yet. dmodemd tells you and stops.
- Caller ID: on hook, the samples are silence here.
- The speaker (`35b7` and `35b4`, the `ATM` of the module).

ATI3 shows `Conexant USB HSF (Cadmus 2)`. The function of each codec
word (the bit of the hook, the bit of the speaker) comes from what the
module sends, not from a datasheet. It has had a test with a fake bridge
([below](#the-conexant-usb-hsf-modems)), but not on a real one.

### Not covered

These are not covered yet, because Linux has no driver that gives their
samples:

- HD-audio modems that are not Si3054-compatible, for example the
  modems of Conexant. (Compatible modems outside the Si3054 list can use
  `hda-modem-setup`, [above](#hd-audio-modems-outside-the-si3054-list).)
- Modems with their own PCI controller: Smart Link SmartPCI, PCTel HSP,
  Lucent/Agere PCI, Conexant HSF PCI, Broadcom BCM4212.

The PCI modems of Smart Link are of two types:

- The modems on an AC'97 controller (Intel ICH, VIA, SiS, ALi, ATI,
  nForce) are AC'97 modem codecs, which `-a` above covers.
- The SmartPCI cards (SL1900, SL2800; PCI vendors 163c, 10a5, 2000 and
  2003) have the own controller of Smart Link. Only the closed
  `amrlibs.o` of Smart Link documents its registers, thus there is no
  open driver to start from.

## Options

| Option | What it does |
|---|---|
| `-a <pcm>`, `--alsa=<pcm>` | The line is the PCM of the codec (`hw:1`, `hw:0,6`, `plughw:...`): a telephone line, not SIP. |
| `--alsa-ctl=<ctl>` | The controls of the codec, if they are not the controls of the card. By default `hw:<card>` for `hw:` and `plughw:` PCMs (`hw:1` for `hw:1,0` or `plughw:1,0`), and the name of the PCM for other PCMs. |
| `--usb=<dev>` | The line is a SmartUSB56, pico-serial Line or WB, or Agere USB modem: `auto` (the first one), or `<bus>:<address>` as `lsusb` and `--usb list` show it. The line is a telephone line (a bare pair for the WB), not SIP. `--usb list` gives the devices and exits. |
| `--misdn=<dev>` | The line is an ISDN basic rate interface through the mISDN of Linux (its number, or its name, for example `hfc-pci.1`), not SIP ([isdn.md](isdn.md)). |
| `--misdn-msn=<msn>` | With `--misdn`: accept calls only for this MSN, and send it as our number. |
| `--misdn-noflip` | With `--misdn`: use the octets of the B-channel as the card gives them, not bit reversed. |
| `--usb-leds` | Turn on the carrier and data LEDs of a SmartUSB56 (GPIO 6 and 13), as on the Modem Blaster of Creative. Only for a board that has them. |
| `--usb-wake` | Let a ring wake the computer: arm the USB remote wakeup of the SmartUSB56 (and on Linux, as root, enable the `power/wakeup` of the kernel for it). |
| `--usb-handset` | Use the handset detector of the SmartUSB56 (GPIO 2): the phone on its PHONE jack off or on hook gives `<DLE>H` and `<DLE>h` in voice mode. |
| `--usb-voice-codec=<law>` | Use the second codec of the SmartUSB56 (SSI2, alternate setting 3) as a handset: `+VLS=14` and `15` in voice mode. Its octets are `ulaw`, `alaw` or `linear`. |
| `--usb-buzzer` | Use the buzzer of the SmartUSB56 (GPIO 11, BUZEN) as the speaker: on while ATM/ATL have the speaker on, as the `Options.Buzzer` of Windows. |
| `--usb-rhom` | Set the RHOM bit of the SmartUSB56 (FIFO_CONTROL2 bit 15, and bit 7 of the voice codec) while the modem runs, as the `Options.RHOM` of Windows. Not below chip revision 0x200. |
| `--usb-international` | Control the international line controls of the SmartUSB56 (GPIO 8, 10 and 12) as the Windows driver of Smart Link does: the DC bit of the country, the 350 ms of Australia after the off-hook, and the controls of pulse dialling. |
| `--usb-wb-tx-gain=<dB>` | The transmit level of a pico-serial WB: the volume of its DAC, -63.5 to +24 dB in 0.5 dB steps (0, the default, is the level of the stick). |
| `--usb-wb-rx-gain=<dB>` | The hybrid input gain of a pico-serial WB: its MicPGA, 0 (the default) to 47.5 dB in 0.5 dB steps. |
| `-r <Hz>`, `--rate=<Hz>` | The rate of the codec. 9600 (the default) or 8000 for the voice-band data pumps; an Agere USB modem always runs at 8000, and ignores this value. 48000, 96000 or 192000 for the wideband mode on a bare pair (V.wb, [dsp/vwb.md](dsp/vwb.md)), on a line that has the rate: an ALSA codec, or a pico-serial WB (whose default is 96000), but not the SmartUSB56 or the Line stick. At those rates, V.wb is the default modulation (`AT+MS=VWB`), the voice-band pumps are refused (`NO CARRIER`), and `ATD` dials nothing. All other values are refused. |

You can give only one of `-a`, `--usb` and `--misdn`.

When you start dmodemd as root, it opens the codec before it continues
as `nobody`. Thus the group of the device (usually `audio`) is not
important. When you start it as an ordinary user, that user must have
access to `/dev/snd/*`.

`SIP_LOGIN` is not necessary with `-a` or `--usb`.

## How it works

The ALSA line is `frontend/dmodemd/alsa_line.cpp`:

- **Samples.** The PCM runs in the two directions from the start, also
  on hook: S16 mono at the rate of the line, in periods of 20 ms, with a
  buffer of 8 periods. The playback runs two periods in front of the
  capture. The answer to each captured frame is written immediately,
  thus the two keep their distance. An overrun or underrun (when
  dmodemd is late) starts the two directions again, and the log tells
  you. The clock of the modem is the clock of the codec: the number of
  samples that it has delivered.
- **The hook.** Off hook (a call, or voice), the "Off-hook Switch" is on
  and the "Caller ID Switch" is off. On hook, the hook switch is off and
  the line monitor is on, thus the codec receives the line: rings and
  caller ID. A hook flash (`AT+VTS=!`) turns off the hook switch for the
  length of the flash.
- **Rings.** On hook, each sample goes through `dsp::RingDetect`. The
  ring signal, received through the line monitor, is a large wave of 15
  to 70 Hz. Each cycle of a burst is a pulse to the `ring()` of the
  core. A ring indicator line gives the same signal. The core checks the
  cadence (`RING`, the answer by `ATS0`, and `S1`):
  - a burst of four pulses or more, after a gap of 1.5 s, is a ring;
  - ringing with no such gap (for example the 2 s on and 1 s off of
    Egypt) rings each 6 s;
  - a ring counts in `S1` for 8 s.

  All countries of the list of the ITU ring in this way
  (`ring_tones_test`, [interop.md](interop-details.md#the-line-rung)).
- **Caller ID.** At the first ring, the core runs its caller ID receiver
  (`AT#CID`) on the samples of the monitor for 4 s.
- **Dialling.** The modem itself dials the number, in DTMF, or in
  pulses with `ATP` or `ATDP`. A `P` or `T` in the dial string changes
  the method for the remainder of the string. With `ATX4` (the default),
  the modem first waits for a dial tone. With `ATX3`, it waits 2 s
  instead.
- **Pulse dialling.** The call progress pump times the pulses from the
  table of the country (`-c`, `AT+GCI`):
  - the break and the make: 64 and 36 ms for the USA, 67 and 33 ms for
    New Zealand, approximately 60 and 40 ms for most of Europe;
  - the gap between digits: 800 ms;
  - the digit pattern: n pulses for n and ten for 0; n + 1 in Sweden;
    10 - n in New Zealand.

  The line breaks and makes its hook (`Host::pulse_hook()`): the
  "Off-hook Switch" on ALSA, GPIO 3 on the SmartUSB56. The switch is
  written while the frames of the modem are processed, thus an edge
  comes when its frame comes (20 ms apart on ALSA). 60/40 pulses keep
  their times. Other pulses come out in one frame of their times. `*`,
  `#` and A to D have no pulses, and the modem skips them. A line that
  cannot pulse (SIP) dials in DTMF.
- **Busy tone.** The pump uses the busy and congestion tones of the
  country (`dsp/tones.h`, the list of the ITU of 192 countries). It
  needs four cycles of one of the two cadences, in 15% or 80 ms on each
  side, and not the ringing tone of the country. A country with neither
  would use the limits of on and off times of the table, or 0.2 to
  0.8 s.
- **Voice mode.** In voice mode, `ATD` waits 2 s, dials, then thinks
  that the call is answered when the ringing tone ends (`+VRA`), or
  after `+VRN` if there is no ringing tone (V.253 9.1.1).
- **Modulations.** The core thinks that the line is an analogue line
  (`Host::analog_line()`): the modem is the analogue end of the call.
  - V.90 and V.92 (the default) of `+MS` run PCM as the analogue modem
    at 9600 Hz (`-r 9600`, the default): up to 56000 bit/s down and
    33600 up from a V.90 server. With V.92 (and a V.92 server), it adds
    PCM upstream up to 48000 bit/s, quick connect and modem-on-hold
    (refer to [V.90 analogue modem](dsp/v90a.md)).
  - With all other modems, it runs V.34 (up to 33600 bit/s) and lower
    modulations.
  - V.91, which is between two digital modems, becomes V.34.
  - At 8000 Hz (`-r 8000`), V.90 is available only with `DSP_V90A=1`.
    A codec at 8000 Hz on a separate clock loses the PCM signal each few
    seconds (refer to [8000 Hz lines](dsp/v90a-details.md#8000-hz-lines)).
    Thus keep 9600.
  - It also has fax (class 1) and voice.

## Limits

- **Speed.** As the analogue modem, the open DSP has V.90 with its
  retrains and rate renegotiations: a line that changes during the call
  renegotiates, and a line that PCM cannot use falls back to V.34. It
  also has the PCM upstream of V.92 (with its renegotiations), quick
  connect and modem-on-hold. The analogue modem has had a test only
  with our own digital modem on the simulated network, not yet with the
  server of an ISP.
- **Pulse timing.** Pulses keep the timing of the frames of the modem
  (20 ms on ALSA). They have not had a test on a real exchange yet.
  `AT&P` (the pulse ratio of S28) is kept, but the table of the country
  sets the times.
- **Country rules.** The country (`-c`, `AT+GCI`) sets the pulse
  dialling, the busy cadence, and the dial pause and hook flash times
  (S8, S24). `-c` also accepts all countries of the list of the ITU. A
  country that is not in the table dials with the settings of the USA
  and its own busy tones. The line settings of the codec (impedance,
  ring thresholds, current limit) are the settings of the country on an
  HD-audio Si3054 with a Si3018 ([above](#si3054-line-settings)), and
  partly on a Conexant SmartDAA ([above](#the-country-and-the-board)).
  All other ALSA codecs keep the settings of their kernel driver, the
  settings of the USA, because ALSA gives no control of them. The DTMF
  levels stay the levels of the pump.
- **Real hardware.** The code has had a test with the fake card below,
  but not yet on a real modem codec.

## Testing without hardware

### The ALSA line

`frontend/dmodemd/test/alsa_test.py` (`dmodemd_alsa` of ctest, label `alsa`) runs
two dmodemd on fake ALSA modem cards whose lines are part of a
simulated telephone exchange. It needs libasound2-dev.

- `frontend/dmodemd/test/fake_modem_card.cpp` is the external PCM
  (ioplug) and control (ctl_ext) plugins of alsa-lib, `dmline`. They
  load through an ALSA configuration of the test (`ALSA_CONFIG_PATH`),
  thus dmodemd uses the same ALSA calls as with a real card. The capture
  is the signal that the exchange sends, the position of the playback
  runs on the clock as a DMA does, and the control has the two switches.
- `frontend/dmodemd/test/line_exchange.cpp` is the exchange. The line of
  each number gets:
  - a dial tone, then the exchange collects its DTMF or pulse digits and
    routes the number;
  - for the called number, the ring signal (2 s of 20 Hz, 4 s off,
    received through the line monitor) and caller ID between the first
    two rings (Bell 202, MDMF); for the caller, the ringing tone;
  - a busy tone if the called line is off hook or unknown;
  - when answered, the call, with a loss of 3 dB, noise, and an echo at
    -24 dB (`LINE_LOSS=<dB>`, `LINE_ECHO=<dB>|off`);
  - a busy tone after the other end hangs up.
- `frontend/dmodemd/test/fake_hda_hwdep.cpp` is the hwdep device of an
  HD-audio Si3054, put in front of alsa-lib with `LD_PRELOAD`.

In real time, the test checks:

- a data call: the dial tone, DTMF, rings, caller ID, the answer at the
  second ring, CONNECT at the two ends, a line of text in each
  direction, and the hang-up;
- a stall of 0.6 s (SIGSTOP), after which the log records the restart
  of the codec;
- a busy line;
- a call dialled in pulses (`ATDP`), which the exchange counts (a break
  of 200 ms maximum is a pulse; a make of 300 ms ends the digit);
- a voice call: the ringing tone, caller ID in the packet of voice mode
  (`<DLE>X` ... `<DLE>.`), OK at `+VRA`, `+VTS` DTMF across, and the
  busy tone after the other end hangs up;
- with the fake hwdep device, the Si3054 line settings: the settings of
  `-c UK` at start-up, the settings of South Africa after `AT+GCI=9F`
  (with RZ cleared off hook and set again on hook), and the settings of
  Australia; nothing written to a codec with a different chip ID; and
  the settings of the kernel driver kept on a card with no hwdep device.

`RATE=8000` runs it at 8000 Hz.

The line settings and the SmartDAA also have unit tests (label
`quick`): `si3054_test` (the rows and bits of the Si3054),
`si3056_test` (the rows and bits of the pico-serial Line, on a Si3018
and a Si3019) and `cx_daa_test` (the register access, types and hook of
the Conexant SmartDAA, and the type 0x12 setup against the writes of the
library, [above](#conexant-ac97-codecs)).

### The SmartUSB56

`frontend/dmodemd/test/usb_test.py` (`dmodemd_usb` of ctest, label `alsa`) runs
two dmodemd with `--usb` on the same exchange. It needs libusb-1.0-0-dev.

`frontend/dmodemd/test/fake_st7554.cpp` is the part of libusb-1.0 that
dmodemd uses, put in front of the real one with `LD_PRELOAD`. Its one
device is an ST7554 whose line is a line of the exchange:

- its registers set the hook;
- its IN transfers take the samples of the line as the exchange sends
  them, and the samples of its OUT transfers go to the exchange;
- its ring detector changes on each half cycle of the ring signal;
- it receives the line on hook only with CID1 set.

One of the two fakes reports that it is an ST75951/2 (`FAKE_ST7554_DAA`)
and checks its start-up. The test checks:

- that dmodemd refuses an incorrect `<bus>:<address>`;
- a data call: rings, caller ID between the first two rings, the answer
  at the second ring, CONNECT at the two ends, text in each direction;
  the LEDs of the modem with `--usb-leds`, and none on the other; the
  buzzer of the modem with `--usb-buzzer`, on when it dials and off at
  the CONNECT; and no RING from FIFO underruns;
- a busy line, a call dialled in pulses, and a voice call;
- the handset detector in voice mode;
- the international line controls: the timer and pulse dialling of
  Australia, the DC bit of France, and the inverted DC bit of the
  ST75951;
- the voice codec as a handset (`FAKE_ST7554_VOICE` of the fake: its
  microphone a 1000 Hz tone, its earpiece written to a file): `+VLS=14`
  and `15`, `+VRX` and `+VTX`;
- RHOM (`--usb-rhom`): on while the modem runs, and none on a revision
  0x0100 chip (`FAKE_ST7554_REVISION`);
- a call that wakes a suspended 1002 (`--usb-wake`): the ring of the
  fake with its remote wakeup armed, the device reset on resume and set
  up again, and the call answered;
- that the two dmodemd exit when the exchange (and thus the devices) is
  not there.

### The pico-serial Line

`frontend/dmodemd/test/pico_line_test.py` (`dmodemd_pico_line` of ctest, label
`alsa`) does the same with two pico-serial Line sticks.
`frontend/dmodemd/test/fake_pico_line.cpp` is a stick (1209:0002) whose
line is a line of the exchange. It uses the protocol of its firmware:
`SET_HOOK` and `SET_MONITOR` are the switches of the exchange, alternate
setting 1 starts its samples, and the changes of its ring detector come
as events. In addition to the calls, it checks:

- the line settings of the country: the settings of `-c UK` at
  start-up, of Australia after `AT+GCI=09`, and of the USA for a country
  that the Si3056 datasheet does not give;
- a stick whose line side does not answer (dmodemd tells you, and
  continues);
- under Wine, a Windows whose WinUSB cannot stream (dmodemd tells you,
  with the procedure), and UsbDk used where it is installed.

### The pico-serial WB

`frontend/dmodemd/test/pico_wb_test.py` (`dmodemd_pico_wb` of ctest, labels
`alsa` and `quick`; approximately 20 s) runs two pico-serial WB sticks on
a bare pair instead of an exchange:

- `frontend/dmodemd/test/fake_pico_wb.cpp` is a stick (1209:0003) that
  uses the protocol of its firmware.
- `frontend/dmodemd/test/wb_pair.cpp` is the pair between the two, in
  real time at 96 kHz. It is the simulated pair of the V.wb tests
  (`dsp/test/vwb_line.cpp`: 1 km of cable, the transformers, noise, the
  hybrid echo of each end, and the two clocks 100 ppm apart). The
  samples of each end play as the stick plays them: after a head start,
  and silence when there are no more samples. `PAIR_KM`, `PAIR_SNR`,
  `PAIR_PPM` and `PAIR_ECHO` change the pair.

It checks:

- that dmodemd refuses an incorrect `<bus>:<address>`, and that
  `--usb list` shows the stick;
- that dmodemd refuses a rate that the stick does not have, and sets
  the other rates (`SET_RATE`); the 96000 Hz default; the options of the
  gains (and the refusal of a value out of range); V.wb as the default
  modulation; a codec that does not come up (dmodemd tells you);
- a call: `AT&L1` at the two ends, the answerer automatically (`S0=1`),
  CONNECT at the rates of V.wb at the two ends, the LINK LEDs on, 32 kB
  in each direction at the same time byte for byte through V.42, and the
  hang-up (NO CARRIER at the other end, the LEDs off);
- that the two exit when the pair (the devices) is not there.

### The Agere USB modem

`frontend/dmodemd/test/agere_test.py` (`dmodemd_agere` of ctest, label `alsa`)
runs two Agere USB modems at 8000 Hz.
`frontend/dmodemd/test/fake_agere.cpp` is one (047e:2892) whose line is
a line of the exchange. It has:

- its vendor commands, the memory and function table of its ARM, and its
  codec registers (the hook register 0xc0 is the switch of the
  exchange);
- the isochronous stream of its ROM: each 2 samples of the line are the
  time of a packet, in which it takes an OUT packet and answers it with
  an IN packet of the samples that have come. Bit 0 of the status word
  is set while the ring signal is there;
- a transmit ring of 48 samples, which reports its overruns and
  underruns, and a codec clock that can be different from the USB clock
  (`FAKE_AGERE_DRIFT`, ppm);
- a Polaris DAA (`FAKE_AGERE_POLARIS`), whose samples stay 0x8000 until
  its link gets a pulse to come up, and whose ring bit can stick after a
  call (`FAKE_AGERE_RING_STICKS`).

It refuses a patch (command 9 stalls), and gives no samples while an
entry of a patch is in its function table. It reports its OH LED when
it changes. The test checks:

- that dmodemd tells you the problem with a full speed device;
- that the samples come at 8000 each second: with the entries of a bulk
  patch left in the table (restored), with a ROM 1.0.6 (its clock not
  changed), with no active configuration reported, with
  `DMODEM_AGERE_TRANSFERS` and `DMODEM_AGERE_DUMP`, and after the LED
  probe (`DMODEM_AGERE_LED_PROBE`);
- 8000 Hz with `-r 9600`;
- that the link of a Polaris DAA comes up and is calibrated (1002 has
  one);
- a data call: rings (the rings of a Polaris from codec register
  0x0a8), caller ID, the answer at the second ring, CONNECT at the two
  ends, text in each direction; the OH LED during the call;
  `DMODEM_USB_TX_GAIN` (`auto` and a fixed gain); the transmit ring
  kept with the clock of the codec 300 ppm off (no overrun or underrun);
  and the call recorded (`DMODEM_USB_REC`);
- no ring from a ring bit stuck after the call;
- a call dialled in pulses;
- that the two exit when the exchange is not there.

### The Apple USB Modem

`frontend/dmodemd/test/apple_test.py` (`dmodemd_apple` of ctest, label `alsa`)
runs two Apple USB Modems. `frontend/dmodemd/test/fake_apple.cpp` is one
(05ac:1401) whose line is a line of the exchange: its six interfaces
and their descriptors (the format type I rates of the streams), the
commands of the DAA on its registers (ONHM of register 5 is the monitor
switch of the exchange), `SET_HOOK_STATE` is the hook of the exchange,
and the notifications of its ring detector and phone cord. It checks
the interfaces found, a line side that does not answer, a rate that it
does not have, the registers of the country, and the same calls as the
test of the pico-serial Line
([apple-usb-modem-details.md](apple-usb-modem-details.md#testing-without-hardware)).

### The Conexant USB HSF modems

`frontend/dmodemd/test/cadmus2_test.py` (`dmodemd_cadmus2` of ctest,
label `alsa`) runs two Conexant USB HSF modems at 16000 Hz.
`frontend/dmodemd/test/fake_cadmus2.cpp` is one (0572:1300) whose line
is a line of the exchange: its bridge runs the scripts in the
interpreter of the interop harness (`dsp/test/hw/cadmus2.hpp`), the
codec word `aaXX` is the hook of the exchange, and its channel 4 comes
each 40 ms while the line rings. It checks the upload of the firmware (a
stand-in of the same size, in the form of `c2firmware.h`), a missing
firmware and a SmartDAA board (dmodemd tells you), the rate, no ring at
the start, and a call: rings, an answer at the second ring, V.34 in the
two directions and a line of text in each direction, the hang-up and
the unplugged device.
