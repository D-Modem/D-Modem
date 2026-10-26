# Softmodem hardware: dmodemd on ALSA and USB

dmodemd's line is normally a SIP call. With `-a` it is instead a
softmodem's codec on a telephone line, through ALSA. With `--usb` it is
a Smart Link SmartUSB56 ([below](#smart-link-smartusb56)), a
pico-serial Line stick ([below](#pico-serial-line)) or an Agere USB 2.0
soft modem ([below](#agere-usb-20-soft-modem)), through libusb.
The modem is then D-Modem's open DSP, on any architecture. Smart Link's
slmodemd, Agere's agrsm and their kind were closed, mostly i386-only
programs; dmodemd can replace them.

    dmodemd -a hw:1 -r 9600
    dmodemd --usb=auto

## The hardware

Linux still drives these modem codecs at the level a softmodem needs: a
stream of samples each way, and switches for the hook and for the line
monitor.

| Kind | Kernel driver | Codecs |
|---|---|---|
| AC'97 modem (MC97, AMR and CNR risers, laptops of about 2000 to 2006) | `snd-intel8x0m` (Intel ICH to ICH7, AMD-768/8111, nForce, SiS 7013, ALi), `snd-atiixp-modem` (ATI IXP), `snd-via82xx-modem` (VIA) | the codec's own AC'97 modem GPIOs: Silicon Labs Si3036/38, Agere, Motorola and others; Conexant's SmartDAA (below) |
| HD-audio modem (laptops of about 2005 to 2010) | `snd-hda-intel` with `snd-hda-codec-si3054` | Silicon Labs Si3054/3055, Agere (11c1:3026, 3055, 3155), Motorola (1057:3055, 3057, 3155), SM56 (1543:3155), LG (1854:0018), a VIA board's (1106:3288) |

The codec appears as a card's PCM, with two switches among its controls:
"Off-hook Switch" and "Caller ID Switch". To find it:

    aplay -l                      # a "Modem" device: card 1, device 0 is hw:1
    amixer -c 1 controls | grep -i -e hook -e 'caller id'

An HD-audio modem is a device of the audio card, e.g. `hw:0,6`.

### Si3054 line settings

On an HD-audio Si3054 with a Si3018 line side, dmodemd also writes the
country's line settings (`-c`, `AT+GCI`) into the codec's DAA
registers, through the kernel's HD-audio hwdep device
(`/dev/snd/hwC<card>D<codec>`, which needs `CONFIG_SND_HDA_HWDEP`).
They are the datasheet's (Si3054/18 Rev. 1.0, Table 19, the HD-audio
columns): the on-hook speed (OHS, OHS2), the ringer impedance and
threshold (RZ, RT), the AC termination (ACT), the DC termination (DCV,
MINI) and the loop current limit (ILIM), in nodes 13, 15, 16 and 18
(the spark quenching OHS needs). Each register is read first and only
those bits changed. Where the table allows ACT 0 or 1 (the TBR21
countries, Egypt), dmodemd takes 1, the complex impedance. RZ is
cleared off hook, as the datasheet asks for pulse dialling, and set
again on hook. The settings are written again before each off-hook,
since the kernel's driver resets the codec on resume. A country the
table has not gets the USA's.

dmodemd writes nothing (and logs `line settings: the kernel driver's`)
on an AC'97 modem, on a kernel without the hwdep device, and on a codec
whose chip ID (node 12) is not a Si3054 with a Si3018, such as the
other vendors' chips the Si3054 driver binds: those keep the kernel
driver's settings, the USA's.

### Conexant AC'97 codecs

Conexant's (and Rockwell's) AC'97 modem codecs, vendor ID `CX` with a
device 0x54xx or `RS` with 0x53xx, keep their hook in their SmartDAA,
not in the AC'97 modem GPIO that the kernel's "Off-hook Switch" drives.
The kernel gives their samples, so `-a` works with them except for the
hook. On an ICH modem controller (`snd-intel8x0m`), dmodemd run as root
also puts the hook into the SmartDAA. It reaches the codec's registers
through the controller's I/O ports, under the codec access semaphore
the kernel's driver also keeps to, and logs `the hook in its SmartDAA`
with the line side's version. Without root, on another controller, or
on another codec, it logs why and the hook stays the kernel's.

The SmartDAA is two chips: the system side (SSD) in the codec and the
line side (LSD) beyond the isolation barrier. What dmodemd knows of
them comes from Conexant's HSF driver for ICH modems (`hsfmc97ich.o`
and its DAA library `hsfsoar.o`), from the code and its debug messages:

- an SSD register (a byte, 0 to 63) is codec register 0x7a: written
  with `(reg << 8) | value`, read by writing `0x8000 | (reg << 8)` and
  reading 0x7a's low byte (0xffff: no SSD). SSD register 3 wants 40 us
  before each access;
- an LSD register goes through SSD registers 1 to 3 and a status: the
  byte to write into 2, the command into 3 (the register in bits 5:0,
  bit 7 a write), the answer from 1. The status is SSD register 0, or 4
  on some SSDs: bit 4 done and bit 3 an error, both cleared before each
  command, bit 2 toggling at each frame over the barrier, bit 1 the
  link up. A write to one of the line's control registers (0x03, 0x14,
  0x15, 0x1c to 0x25) first sets SSD register 0x17's bit 1; one kind of
  SSD (below, 0x1a) has the command's bit 6 for them instead. A write
  is good when its answer is the byte written; the driver tries 5 times
  (LSD register 0x17 once);
- the hook is LSD register 0x15, bits 7:6: 01 on hook, 10 off hook, 00
  and 11 a dial pulse's make and break;
- the driver's library knows 39 kinds of SSD, which differ in the LSD
  access: some prime SSD register 1 before each command (0x55 for a
  read, the byte's inverse for a write) to tell an absent LSD, some
  have the status in register 4, some send a read's command before
  clearing the status. The ICH driver takes the kind from the codec's
  DAA type, register 0x7e's low byte: 0x20 kind 5, 0x21 0x0c, 0x22
  0x13, 0x23 0x15, 0x28 0x0d, 0x29 0x12, 0x30 0x17, 0x31 0x20, others
  5; type 0x24's SSD is in the codec's own registers, not behind 0x7a.
  dmodemd does the same (`cx_kind()`), and leaves a type 0x24 codec's
  hook to the kernel.

The HSF interop harness (`half_hsf`, [interop.md](interop.md)) runs
Conexant's library against a simulated SmartDAA of this kind (codec
0x5429, kind 0x12). With the LSD answering as above, its version 5 and
its register 2 reading 0x0f once powered, the library sets the line up
as follows for the USA (LSD writes, in order):

- start: 0x03 = 00 and 0x04 = 2f, then waits for 0x02 to read 0x0f (the
  LSD powered); 0x20 = 00, 0x25 = 29, 0x24 = 28, 0x21 = 80, 0x19 = 80,
  0x08 = 00, 0x0a = d9, 0x0e = 00, 0x0c = 07, 0x12 = 00, 0x10 = 00,
  0x19 = c0, 0x1f = 08, 0x15 = 40 (on hook), 0x1c = 78, 0x14 = e0,
  0x07 = 04, 0x18 = 43, 0x19 = cb, 0x06 = 02; then 0x0c = 06, 0x12 =
  60, 0x1c = 30, 0x0e = 00, 0x18 = 43, 0x10 = 00, 0x1c = 68, 0x1c = 6a,
  0x0c = 02, 0x0a = df, 0x10 = 00, 0x0e = 02, 0x10 = 00;
- ATA: 0x15 = 41, 0x12 = 40, 0x0e = 00, 0x14 = 20, 0x18 = 43, 0x10 =
  00, 0x21 = 82, 0x1c = 32, 0x15 = 40, then 0x15 = 80 (off hook) and
  0x19 = 8b.

Before that it writes SSD registers directly: 0x1c = 40, 0x17 = 04,
0x14 = 08, 0x0b = 5f, 0x06 = c0, 0x19 = 20, 0x05, 0x04 and 0x07 to
0x0a = 00, 0x0d = 10, 0x1e = 00, 0x19 = 28, 0x1c = 7f, 0x1a, 0x1b and
0x1d = 00, 0x25 = 5a, 0x23 = 43, 0x24 = 80, 0x22 = 5a, 0x20 = 63, 0x21
= a8. The simulated LSD's status registers (0x16, 0x1a, 0x1e, 0x1f)
read 0, so a real line may take other values where the library adjusts
to what it measures (the loop voltage, the drive).

Two runs, the simulated line side's registers starting at 0 and at
0xff (`HSF_DAA_FILL=0xff`), show which bits each of the library's
writes keeps. A third, over two calls, gives its hangup: SSD register
0x14's bits 1:0 back to 0, LSD 0x12's bits 6:5 = 11, 0x15 = 40, 0x19 =
cb, 0x0e = 00, 0x21 = 80, 0x14's bits 7:2 = 111000, 0x18 = 43, 0x10 =
00, 0x1c's bits 6:0 = 6a, 0x10 = 00, 0x0e = 02, 0x10 = 00; going off
hook ends with SSD 0x14's bits 1:0 = 01.

For a codec of kind 0x12 dmodemd does the same (`CxDaa::power_up()`,
`go_off_hook()`, `go_on_hook()`): the SSD's writes, the LSD's power
up, its version (0 to 0x11, as the library takes), the line's writes
with the bits the library keeps, and the hook's sequences, with the
100 ms the library waits after 0x15 = 41. `cx_daa_test` checks its LSD
writes against the library's, from both starts. A dial pulse is the
hook bits alone. For the other kinds the setup is not known: dmodemd
reads the line side's version and puts the hook bits alone, which
holds the line only if something else has set the line side up.

The country and the board's parameters are Conexant's, as the other
backends' are their datasheets' (`cx_country.cpp`, generated from the
HSF driver's files):

- the country: of the line settings above only LSD 0x12's bits 6:5
  change with it, in the library's runs for a country of each of
  `hsf.cty`'s 14 `SMART_RELAYS` tables (`HSF_COUNTRY=<T.35 code>`):
  they are bits 7:6 of the low byte of the table's off-hook and on-hook
  state words. That gives three settings, 11 on hook and 10 off (the
  USA, Japan, ...), 10 and 10 (most, the ITU's), 00 and 00 (the TBR21
  countries). The table has all 242 of `hsf.cty`'s regions under its
  names, and D-Modem's for the three it names otherwise (BAHREIN,
  SOUTHKOREA, UAE). `-c` and `+GCI` choose the row, as for the Si3054;
  on hook the new setting goes to the line side at once;
- the board: the `.inf`'s `SmartDAAParamsK3` for DAA types 0x22 to
  0x24, 0x29, 0x30 and 0x31, its `SmartDAAParams` for the others, as
  the ICH driver chooses (95 bytes each). Changing each byte of the
  block in turn (`HSF_NVM` with an edited `.inf`), three change the
  library's writes: byte 87 is SSD 0x21 (0x80 | it), byte 93 SSD 0x0b,
  byte 94 SSD 0x06's bits 7:4. The others (its resistors and DC tables)
  change nothing on the line sides dmodemd sets up (below): the
  library uses them only for the line side of version 0x11, which it
  measures.

`cx_daa_test` checks the writes against the library's for the USA,
Germany and Canada, from both starts.

The line side's version (LSD 0x22's bits 5:0) changes what the library
does (`HSF_LSD_VERSION` in the harness, each version tried):

- 1 and 3 to 0x0f: the setup above;
- 2: the same but for three things: no writes to 0x24 and 0x25, 0x21's
  bit 7 clear in each of its writes (00 and 02, not 80 and 82), 0x19's
  plain writes with bit 5 (eb and ab, not cb and 8b). Conexant's own
  `hsfconfig --info` has shown `SmartDAA SSD=10 LSD=0x2` for an HSF PCI
  modem, so it is one real line sides have;
- 0x10 and 0x11: another setup altogether (0x1d, 0x1f = 30, 0x15 = c0
  on hook and 00 off hook, the hook's bit 7 the other way), and for 0x11
  the line measured (below);
- 0: the library reads the monitor ADC over and over.

dmodemd does the library's setup for versions 1 to 0x0f, 2's
differences included (`cx_daa_test` checks version 2's writes too).
For 0, 0x10 and 0x11 it reads the version and puts the hook bits alone,
bit 7 the other way for 0x10 and 0x11.

The line side's monitor ADC: LSD 0x16's bits 6:4 choose the channel,
its bit 3 starts a conversion and reads 1 when it is done, LSD 0x17 is
the reading (8 bits, written back when read). The library takes it as
reading * 2000 / 255 mV plus an offset (500 mV, for version 0x10
calibrated on channel 3 against a reference it wants at 1350 to 1650
mV); versions 0 and 2 have 4000 mV for 255 (7 bits on 0). Only on
version 0x11 does the library measure the line with it: the battery on
hook on channel 1 (Vbatt = 28.2 times the reading's mV plus 400, the
factor from the board's resistors), off hook channel 0 (the receiver's
DC offset) and channel 1 again (the tip-ring voltage, from which it
works out the loop's resistance, current and power), a DAC (LSD 0x19)
calibrated on channel 4, then set so that the tip-ring voltage meets
its DC curve, and adjusted while the line settles ("adaptive
offhook"). dmodemd does none of that, as it does no version 0x11 setup.
None of this has run on a real codec yet.

### HD-audio modems outside the Si3054 list

`snd-hda-codec-si3054` binds only the vendor IDs above. A modem codec
made to the same interface under another ID gets no driver, and so no
ALSA device. `snd-hda-intel` can give such a codec the Si3054's vendor
ID through its early patching (`CONFIG_SND_HDA_PATCH_LOADER`, in most
distributions' kernels), and the driver then binds it.
`hda-modem-setup` (installed beside dmodemd) does this:

    hda-modem-setup               # list the modem codecs
    sudo hda-modem-setup write    # the patch for those with no driver
    sudo reboot
    hda-modem-setup list          # now driven by snd_hda_codec_si3054?

`list` shows each codec with a modem function group
(`/sys/bus/hdaudio/devices`): its card and address, its IDs, its
driver, and whether it is in the Si3054 list. `write` takes the modem
codecs that have no driver and are not in the list (or those named
with `--codec hdaudioC<card>D<addr>`). For each it writes a `[codec]`
section, with `[vendor_id] 0x163c3055`, to
`/lib/firmware/dmodem-hda-modem.fw`. It also writes the module option
that loads the file for the codec's card to
`/etc/modprobe.d/dmodem-hda-modem.conf`
(`options snd-hda-intel patch=...`). It checks the running kernel's
configuration for the patch loader and the Si3054 driver, and warns
of another `patch=` option for `snd-hda-intel`, as only one takes
effect. The change takes effect the next time `snd-hda-intel` loads.
If the module is in the initramfs, rebuild that first
(`update-initramfs -u`, `dracut -f`).

Whether the codec really is Si3054-compatible, only the driver can
tell once it has bound it. At its init it checks the link to the
line side chip, and logs the chip's ID. "Link Frame Detect(FDT) is not
ready" in `dmesg` means the codec is not compatible:
`sudo hda-modem-setup remove`, and reboot. Conexant's HD-audio modems
(`14f1:2bfa`, `14f1:2c06` and the others of `14f1`) are of another
kind: their DAA is behind Conexant's own verbs, and this does not
cover them.

`dmodemd/test/hda_modem_setup_test.py` (ctest's `hda_modem_setup`,
label `quick`) runs the helper on a fake sysfs and kernel
configuration. The patch itself has not been tried on a real codec.

### Smart Link SmartUSB56

Smart Link's USB modem (the SmartUSB56 and the modems built on its
ST7554 bridge, USB `0483:7554`) needs no kernel driver: dmodemd drives
it through libusb. The ST7554 is a USB bridge to an STLC7550 codec and a
DAA. Its registers are vendor control requests, its samples isochronous
transfers both ways, and its GPIOs the hook and the ring detector.

    lsusb -d 0483:7554            # Bus 001 Device 007: ID 0483:7554 ...
    dmodemd --usb=auto            # the first one; --usb=1:7 that one

Started as root, dmodemd opens the device before it goes on as nobody.
To start it as a user, give that user the device with a udev rule, e.g.
`/etc/udev/rules.d/60-smartusb56.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="7554", MODE="0660", GROUP="dialout"

If Smart Link's own kernel driver (`slusb`) is loaded, dmodemd detaches
it from the device while it runs. dmodemd is built with `--usb` when
pkg-config finds libusb-1.0 (libusb-1.0-0-dev). `dmodemd --usb list`
lists the SmartUSB56s and pico-serial Lines there are, and whether each
can be opened. On Windows dmodemd.exe drives both the same way, on
WinUSB ([windows.md](windows.md#usb-modems---usb)).

The ST7554's register values and start-up are those of Smart Link's
Linux driver for it (`drivers/st7554.c` of slmodem 2.9.11, under a BSD
licence). In `dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 9600 (the default) or 8000 Hz,
  in isochronous packets of 1 ms. Three transfers of five packets are in
  flight each way. Each OUT transfer goes when an IN one is done, with
  that one's packets' lengths, so the playback keeps the codec's clock.
  It takes the modem's samples queued, a frame and a half ahead. The
  modem's time is the codec's: the samples it has given.
- **The hook.** GPIO 3 (and the hook LED, GPIO 7). A hook flash clears
  it for its length.
- **Rings.** The ring detector (GPIO 0) changes with the ring signal.
  Each change comes on the interrupt endpoint and is a pulse to the
  core's `ring()`, which checks the cadence.
- **Caller ID.** On hook the codec hears nothing unless GPIO 5 (CID1)
  connects it to the line. With `AT#CID=1` dmodemd sets it from the
  first ring while the core's caller ID receiver listens, and clears it
  before the hook changes, as Smart Link's Windows driver does (its
  Linux one never did). So caller ID comes between the first two rings.
- **The codec.** An STLC7550, or an ST75951/2 "silicon DAA"
  (`MONOLITHIC`, GPIO status bit 14). The ST75951's control words name
  one of its four registers; dmodemd starts it up as the Windows driver
  does. Hook, ring and caller ID are the same GPIOs.
- **LEDs.** The hook LED (GPIO 7) goes with the hook. With
  `--usb-leds`, as Smart Link's Windows driver does on boards whose INF
  says they have them (Creative's Modem Blaster V.92 USB): the carrier
  LED (GPIO 6) is lit while the call is online, the data LED (GPIO 13)
  once data has passed in it, both off on hook. Without the option
  dmodemd leaves those GPIOs alone, as another board may use them for
  something else.
- **Wake on ring.** With `--usb-wake` dmodemd arms the device's USB
  remote wakeup (SET_FEATURE) if its configuration says it can wake,
  as Smart Link's Windows driver does when the PC sleeps. The ring
  detector's interrupt is on while on hook, so a ring wakes the
  computer. On Linux the kernel must allow it too: dmodemd writes
  `enabled` to the device's `/sys/bus/usb/devices/<bus>-<ports>/power/wakeup`
  while it is still root. Started as a user, a udev rule does it:
  `ATTR{power/wakeup}="enabled"` in the rule above. On Windows,
  Device Manager's "Allow this device to wake the computer" is the
  kernel's half. The remote wakeup is cleared when dmodemd exits.
- **Suspend and resume.** While the computer sleeps the transfers
  fail. dmodemd keeps them and, every second, sets the device up again
  (its registers, the remote wakeup, the FIFO: a resume may have reset
  it) and submits them again. It gives up after 30 s, as for a device
  gone. A call is lost across a sleep; the rings after the one that woke
  the computer are heard as usual, so the modem can still answer.
- **The handset detector.** With `--usb-handset` dmodemd enables the
  interrupt of GPIO 2 (`GPIO_HANDSET` in Smart Link's Linux source; the
  Windows driver enables it too and does nothing with it). On each
  change it reads the GPIO: set is the phone on the modem's PHONE jack
  off hook, as far as is known (no driver says). The core reports it in
  voice mode as V.253's local phone event, `<DLE>H` off hook and
  `<DLE>h` on hook, and lists event 8 in `+VLS=?`.
- **International line controls.** With `--usb-international`
  dmodemd drives three more outputs, as Smart Link's Windows driver
  does with its `Options.International` (off in Creative's INF):
  - DP (GPIO 10): set off hook; cleared on hook with RFC.
  - DC (GPIO 12): set off hook in France, Algeria and Tunisia, the
    countries whose record asks for it (byte 90, bit 0, of Smart
    Link's table). On an ST75951 it is the other way: set off hook in
    every other country.
  - RFC (GPIO 8): for 350 ms after each off hook in Australia (bit 1,
    "the Australian timer"), and all through pulse dialling (DP and RFC
    left alone then, DC cleared on each break).

  What the three switch in the DAA is not known (Smart Link's names
  only); they are for a board built for these countries. The country
  is `-c` or `+GCI`'s. dmodemd learns when pulse dialling begins and
  ends from the call progress pump (`MDMPRM_PULSE_DIALLING`).
- **The voice codec (a handset).** With `--usb-voice-codec=<law>`
  dmodemd takes alternate setting 3 instead of 1, whose second pair of
  isochronous endpoints is the ST7554's SSI2 codec (Smart Link's
  `K7554VoiceCodecInterface`: 8000 octets a second, powered up with
  SSI_CTRL = 0x21, its period in register 6). In voice mode `+VLS=?`
  then lists V.253's 14 (`"H"`: the handset alone, the line on hook)
  and 15 (`"HT"`: the handset on the call, off hook). With 14 the voice
  engine's samples are the handset's (its microphone to `+VRX`, `+VTX`
  to its earpiece); with 15 they are the line's, and dmodemd mixes the
  handset in both ways (the line and the modem's samples to the
  earpiece, the microphone to the line and to `+VRX`). The two codecs'
  rates are bridged by linear interpolation. What the octets are is not
  known (the Windows driver calls its format 3 and never uses it, and
  streams this codec only from its test interface): `ulaw`, `alaw` or
  `linear` (signed 8-bit). Creative's INF uses alternate setting 1, so
  the DE5671 may have no codec there; a board whose alternate setting 3
  has no such endpoints is refused.
- **RHOM.** With `--usb-rhom` dmodemd sets FIFO_CONTROL2's bit 15 (and
  bit 7, the voice codec's, with `--usb-voice-codec`) while the modem
  runs (from the line's start to its stop), as Smart Link's Windows
  driver does with `Options.RHOM` (`ConfigureRHOM`, `EnableRHOM`),
  which Creative's INF leaves off. A chip whose revision (register 0)
  is below 0x200 never gets it, as on Windows. What RHOM does is not
  known.
- **The buzzer.** With `--usb-buzzer` the modem's speaker is GPIO 11
  (BUZEN): on whenever the core has the speaker on at any volume (ATM1,
  the default: from dialling or answering to the CONNECT; ATM2 always;
  ATM0 never; ATL0 is off too), off otherwise, as Smart Link's Windows
  driver does in `SetSpeakerVolume` with `Options.Buzzer` (off in
  Creative's INF). A buzzer has no volume, so ATL1 to 3 are the same.
  Not every board has one.
- **Unplugged.** dmodemd says so and exits.

Limits: the device's line settings are its own, and it has not yet been
tried on a real SmartUSB56; caller ID and the ST75951 follow the Windows
driver (read from its code, [smartusb56-windows.md](smartusb56-windows.md))
and have been tested on the fake device only.

### pico-serial Line

The [pico-serial](https://github.com/Neo-Desktop/pico-serial) project's
phone-line stick ([SPEC-L.md](https://github.com/Neo-Desktop/pico-serial/blob/main/SPEC-L.md)):
a USB-C stick with an RJ11 socket, made for D-Modem. It is a softmodem's
line and nothing more: a Skyworks Si3056 DAA and its Si3018 line side
behind an RP2040, at 9600 Hz so the open DSP's V.90 works. dmodemd drives
it through libusb, as the SmartUSB56, and it needs no kernel driver.

    lsusb -d 1209:0002            # Bus 001 Device 008: ID 1209:0002 ...
    dmodemd --usb=auto            # the first SmartUSB56 or stick; --usb=1:8 that one

A udev rule to start dmodemd as a user, e.g.
`/etc/udev/rules.d/60-pico-serial-line.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0002", MODE="0660", GROUP="dialout"

Its protocol is its firmware's (`firmware/PROTOCOL.md`, "Line stick"),
shaped like the ST7554's on purpose. In `dmodemd/usb_line.cpp`:

- **Samples.** S16 little-endian mono at 9600 (the default) or 8000 Hz,
  isochronous both ways in interface 0's alternate setting 1, which
  starts them (the rate is set first, `SET_RATE`). Each IN packet holds
  the samples the stick's codec gave in that millisecond, and each OUT
  transfer goes with an IN one's lengths, as for the ST7554.
- **The hook.** A vendor request (`SET_HOOK`); a hook flash the stick
  times itself (`HOOK_FLASH`); pulse dialling `SET_HOOK` for each break
  and make, as the modem's frames are processed.
- **Rings and caller ID.** On hook the stick's line monitor is on
  (`SET_MONITOR`), so it hears the line: **caller ID** comes in its
  samples, as on an ALSA codec. Rings come from its ring detector: each
  change it counts (an 8-octet event on the interrupt endpoint) is a
  pulse to the core's `ring()`, as the ST7554's ring GPIO's are.
- **At start-up** dmodemd reads its version and line side (`GET_INFO`),
  and says so if the line side does not answer across the isolation
  barrier. ATI3 says `pico-serial Line (Si3056)`.
- **The country's line.** dmodemd writes the country's settings (`-c`,
  and again on each `AT+GCI`) into the DAA: its AC termination (ACT and
  ACT2 on the stick's Si3018 line side, ACIM on a Si3019), DC termination
  (DCV, MINI), TBR21 current limit (ILIM), ringer impedance and threshold
  (RZ, RT) and on-hook speed (OHS, OHS2, with Australia's spark
  quenching). The values are the Si3056 datasheet's (Rev. 1.03, Table 15,
  "Country Specific Register Settings"), in `dmodemd/si3056.cpp`. Each
  register (16, 26, 30, 31, 59) is read first (`DAA_READ`) and only the
  country's bits change (`DAA_WRITE`). A change while off hook waits
  until the line is on hook again. Countries the table has no row for
  (Algeria, Brunei, Estonia, Lithuania, Sri Lanka, Tunisia, Turkey,
  Uruguay, Vietnam and most of the ITU list's) keep the USA's settings,
  the Si3056's reset values, and dmodemd's log says so (`-d1`); the
  datasheet's TBR21 row is `-c CTR21EUROPE`.
- **Unplugged.** dmodemd says so and exits.

Limits: the transmit and receive gains stay at their reset values (Table
15 lists none), and Australia keeps its Japan-mode DC termination
throughout the call (the datasheet's option of FCC mode 500 ms after
going off hook, for a higher transmit level, is not taken). The stick's
USB ID is a pid.codes test PID until it has its own. It has been tested
against a fake of it (below), not yet on a real one.

### Agere USB 2.0 soft modem

Agere's (later LSI's) USB soft modem, USB `047e:2892` (and `047e:2828`),
sold under other names too, has no kernel driver: dmodemd drives it
through libusb. It is an Agere USB controller with an ARM in it and a
Mercury codec and DAA. Its ROM is patched at every start with a "bulk
patch" of Agere's. D-Modem ships Agere's patches, unaltered, in
`firmware/agere/` (installed in `<prefix>/share/dmodem/firmware/agere`):
their binary contents are subject to Agere's licence
(`firmware/agere/LICENSE`, [NOTICE.md](../firmware/agere/NOTICE.md)),
not D-Modem's.

    lsusb -d 047e:                # Bus 001 Device 009: ID 047e:2892 ...
    dmodemd --usb=auto

`--usb-agere-driver` takes the patches from Agere's own driver instead:
any of them has them (`AGRSM.sys` or `AGRSM64.sys`, Windows 98 to 7;
`agrmodem.ko` or `agrmodemlib.o`, agrsm's Linux package), or a directory
with one of them (an unpacked driver package). The line runs at 8000 Hz, whatever `-r` says. A udev
rule (`/etc/udev/rules.d/60-agere-usb.rules`) lets dmodemd open it
without root:

    SUBSYSTEM=="usb", ATTR{idVendor}=="047e", ATTR{idProduct}=="2892", MODE="0660", GROUP="dialout"

What it does, from Agere's drivers ([agere-usb.md](agere-usb.md)), in
`dmodemd/usb_agere.cpp`:

- **Start-up.** Interface 0's alternate setting 2 (bulk IN and OUT), the
  ROM's version, its GPIOs and clock, the Mercury codec's registers as
  Agere's driver writes them, on hook; then the bulk patch for the ROM
  (1.0.6's, or 1.2.1's in flash or ROM) unless it is in, the clock raised,
  the samples started. A "Polaris" DAA (its codec registers do not
  answer) is not driven yet: dmodemd says so.
- **Samples.** 100-octet bulk transfers each way: a header (the samples'
  count, a status word) and 48 samples. One OUT transfer goes for each IN
  one, with its count, so the playback keeps the device's clock.
- **Hook.** Codec register 0xc0 (0 off hook); pulse dialling and the
  hook flash break and make it.
- **Rings.** Bit 0 of the IN transfers' status word: a pulse to the
  core's `ring()` for each transfer that has it.
- **Caller ID.** The codec hears the line on hook, so caller ID comes
  in the samples.
- **Unplugged.** dmodemd says so and exits.

It has been tested against a fake of it (below), not yet on a real one:
[agere-usb.md](agere-usb.md#bring-up) says what to look for, and what to
send, on the first run.

### Not covered

Not covered yet, because Linux has no driver that gives their samples:

- HD-audio modems that are not Si3054-compatible, such as Conexant's
  (the compatible ones outside the Si3054 list: `hda-modem-setup`,
  above);
- modems with their own PCI controller (Smart Link SmartPCI, PCTel HSP,
  Lucent/Agere PCI, Conexant HSF PCI, Broadcom BCM4212).

Smart Link's PCI modems are two kinds. Those on an AC'97 controller
(Intel ICH, VIA, SiS, ALi, ATI, nForce) are AC'97 modem codecs, covered
by `-a` above. The SmartPCI cards (SL1900, SL2800: PCI vendors 163c,
10a5, 2000 and 2003) have Smart Link's own controller. Its registers are
only in Smart Link's closed `amrlibs.o`, so there is no open driver to
start from.

## Options

| Option | |
|---|---|
| `-a <pcm>`, `--alsa=<pcm>` | the codec's PCM (`hw:1`, `hw:0,6`, `plughw:...`): the line is a telephone line, not SIP |
| `--alsa-ctl=<ctl>` | its controls, if not the card's (`hw:1` for `hw:1,0` or `plughw:1,0`; the PCM's own name otherwise) |
| `--usb=<dev>` | a SmartUSB56, pico-serial Line or Agere USB modem (`auto`, the first one; or `<bus>:<address>`, as `lsusb` and `--usb list` show it): the line is a telephone line, not SIP; `--usb list` lists them and exits |
| `--usb-leds` | a SmartUSB56's carrier and data LEDs lit (GPIO 6 and 13), as on Creative's Modem Blaster; only for a board that has them |
| `--usb-wake` | a ring wakes the computer: the SmartUSB56's USB remote wakeup armed (and on Linux the kernel's `power/wakeup` of it enabled, as root) |
| `--usb-handset` | the SmartUSB56's handset detector (GPIO 2): the phone on its PHONE jack off or on hook, `<DLE>H` and `<DLE>h` in voice mode |
| `--usb-voice-codec=<law>` | the SmartUSB56's second codec (SSI2, alternate setting 3) as a handset: `+VLS=14` and `15` in voice mode; its octets `ulaw`, `alaw` or `linear` |
| `--usb-buzzer` | the SmartUSB56's buzzer (GPIO 11, BUZEN) as the speaker: on while ATM/ATL have it on, as Windows' `Options.Buzzer` |
| `--usb-rhom` | the SmartUSB56's RHOM bit (FIFO_CONTROL2 bit 15, and the voice codec's bit 7) while the modem runs, as Windows' `Options.RHOM`; not below revision 0x200 |
| `--usb-international` | the SmartUSB56's international line controls (GPIO 8, 10 and 12), as Smart Link's Windows driver has them: the country's DC bit, Australia's 350 ms after going off hook, and pulse dialling's |
| `--usb-agere-driver=<file>` | an Agere USB modem's bulk patch from Agere's driver for it (`AGRSM.sys`, `AGRSM64.sys`, `agrmodem.ko` or `agrmodemlib.o`), or a directory with one, instead of the patches D-Modem ships (`firmware/agere`) |
| `-r <Hz>`, `--rate=<Hz>` | the codec's rate: 9600 (the default) or 8000, the data pumps' (an Agere USB modem's is 8000 whatever it says) |

Started as root, dmodemd opens the codec before it goes on as nobody, so
the device's group (usually `audio`) does not matter. Started as a user,
that user needs access to `/dev/snd/*`.

`SIP_LOGIN` is not needed with `-a` or `--usb`.

## How it works

`dmodemd/alsa_line.cpp`:

- **Samples.** The PCM runs both ways from the start, on hook too: S16
  mono at the line's rate, in periods of 20 ms. The playback runs two
  periods ahead of the capture, and each captured frame's answer is
  written at once, so the two keep their distance. An overrun or underrun
  (dmodemd held up) restarts both; the log says so. The modem's time is
  the codec's: the samples it has given.
- **The hook.** Off hook (a call, voice), "Off-hook Switch" is on and
  "Caller ID Switch" is off. On hook, the hook switch is off and the line
  monitor is on, so the codec hears the line: rings and caller ID. A hook
  flash (`AT+VTS=!`) turns the hook switch off for its length.
- **Rings.** On hook every sample goes through `dsp::RingDetect`. The
  ring signal through the line monitor is a large wave of 15 to 70 Hz,
  and each cycle of a burst is a pulse to the core's `ring()`. That is
  what a ring indicator line gives, and the core checks the cadence
  (`RING`, `ATS0` answering, `S1`): a burst of four pulses or more, after
  a gap of 1.5 s, is a ring; ringing with no such gap (Egypt's 2 s on
  and 1 off) rings every 6 s; a ring counts in `S1` for 8 s. Every
  country of the ITU's list rings this way (`ring_tones_test`,
  [interop.md](interop.md#the-line-rung)).
- **Caller ID.** At the first ring the core runs its caller ID receiver
  (`AT#CID`) on the monitor's samples for 4 s.
- **Dialling.** The modem dials the number itself, in DTMF, or in pulses
  with `ATP` or `ATDP` (a `P` or `T` in the dial string changes over for
  the rest of it). With `ATX4` (the default) it waits for a dial tone
  first; with `ATX3` it waits 2 s.
- **Pulse dialling.** The call progress pump times the pulses, by the
  country's table (`-c`, `AT+GCI`): the break and the make (64 and
  36 ms for the USA, 67 and 33 for New Zealand, 60 and 40 for most of
  Europe), the gap between digits (800 ms) and the digit pattern (n
  pulses for n and ten for 0; Sweden n + 1; New Zealand 10 - n). The line
  breaks and makes its hook (`Host::pulse_hook()`): the "Off-hook
  Switch" on ALSA, GPIO 3 on the SmartUSB56. The switch is written as
  the modem's frames are processed, so an edge comes when its frame does
  (20 ms apart on ALSA): 60/40 pulses keep their times, others come
  out within one frame of theirs. `*`, `#` and A to D have no pulses
  and are skipped. A line that cannot pulse (SIP) dials in DTMF.
- **Busy tone.** The pump takes the country's busy and congestion
  tones (`dsp/tones.h`, the ITU's list, 192 countries): four cycles of
  either's cadence, 15% or 80 ms either side, and not the country's
  ringing tone. A country with neither would have the table's bounds of
  on and off times, or 0.2 to 0.8 s.
  In voice mode, `ATD` waits 2 s, dials, then takes the call as answered
  by the ringing tone's end (`+VRA`), or by `+VRN` without one (V.253
  9.1.1).
- **Modulations.** The core treats the line as an analogue one
  (`Host::analog_line()`): the modem is the analogue end of the call.
  `+MS`'s V.90 and V.92 (the default) run PCM as the analogue modem at
  9600 Hz (`-r 9600`, the default): up to 56000 bit/s down and 33600 up
  from a V.90 server, and with V.92 (a V.92 server) PCM upstream up to
  48000 bit/s, quick connect and modem-on-hold (see [V.90 analogue
  modem](dsp/v90a.md)),
  and V.34 (up to 33600 bit/s) and below with any other modem. V.91,
  between two digital modems, becomes V.34. At 8000 Hz (`-r 8000`)
  V.90 is there only with `DSP_V90A=1`: a codec at 8000 Hz on a clock
  of its own loses the PCM signal every few seconds (see
  [8000 Hz lines](dsp/v90a.md#8000-hz-lines)), so keep 9600. It has fax
  (class 1) and voice too.

## Limits

- **Speed.** As the analogue modem the open DSP has V.90, with its
  retrains and rate renegotiations (a line that changes mid-call
  renegotiates, and one PCM cannot use falls back to V.34), and V.92's
  PCM upstream (with its renegotiations), quick connect and
  modem-on-hold.
  The analogue modem has been tested against our own digital modem on
  the simulated network only, not yet against an ISP's server.
- **Pulse timing.** Pulses keep the modem's frames' timing (20 ms on
  ALSA); not yet tried on a real exchange. `AT&P` (S28's pulse ratio) is
  kept, but the country's table sets the times.
- **Country rules.** The country (`-c`, `AT+GCI`) sets the pulse
  dialling, the busy cadence and the dial pause and hook flash times
  (S8, S24). `-c` takes any country of the ITU's list too: one the table
  has not dials with the USA's settings and its own busy tones. The codec's line settings (impedance, ring thresholds,
  current limit) are the country's on an HD-audio Si3054 with a Si3018
  ([above](#si3054-line-settings)); any other codec keeps its kernel
  driver's, the USA's: ALSA gives no control of them. The DTMF levels
  stay the pump's own.
- **Real hardware.** The code has been tested against the fake card
  below, not yet on a real modem codec.

## Testing without hardware

`dmodemd/test/alsa_test.py` (ctest's `dmodemd_alsa`, label `alsa`)
runs two dmodemd on fake ALSA modem cards whose lines are a simulated
telephone exchange's:

- `dmodemd/test/fake_modem_card.cpp` is alsa-lib's external PCM
  (ioplug) and control (ctl_ext) plugins, `dmline`. They are loaded
  through an ALSA configuration of the test's own (`ALSA_CONFIG_PATH`),
  so dmodemd goes through the same ALSA calls as with a real card. The
  capture is what the exchange sends, the playback's position runs on
  the clock like a DMA's, and the control has the two switches.
- `dmodemd/test/line_exchange.cpp` is the exchange. Each number's line
  gets:
  - a dial tone, then its DTMF digits collected and the number routed;
  - for the number called, the ring signal (2 s of 20 Hz, 4 s off,
    heard through the line monitor) and caller ID between the first two
    rings (Bell 202, MDMF); for the caller, the ringing tone;
  - a busy tone if the line called is off hook or unknown;
  - once answered, the call, with a loss of 3 dB, noise and an echo at
    -24 dB (`LINE_LOSS`, `LINE_ECHO=<dB>|off`);
  - a busy tone after the other end hangs up.

In real time, the test checks:

- a data call: dial tone, DTMF, rings, caller ID, the answer at the
  second ring, V.34 at 33600, a line of text each way, the hang-up;
- a stall of 0.6 s (SIGSTOP), after which the codec's restart is
  logged;
- a busy line;
- a call dialled in pulses (`ATDP`), which the exchange counts (a break
  of 200 ms at most a pulse, 300 ms made again the digit's end);
- a voice call: the ringing tone, caller ID in voice mode's packet
  (`<DLE>X` ... `<DLE>.`), OK at `+VRA`, `+VTS` DTMF across, and the
  busy tone after the other end hangs up.

`RATE=8000` runs it at 8000 Hz. It needs libasound2-dev.

`dmodemd/test/usb_test.py` (ctest's `dmodemd_usb`, label `alsa`) runs
two dmodemd with `--usb` on the same exchange. `dmodemd/test/fake_st7554.cpp`
is the part of libusb-1.0 that dmodemd uses, put in front of the real one
with `LD_PRELOAD`. Its one device is an ST7554 whose line is the
exchange's: its registers set the hook, its IN transfers take the line's
samples as the exchange sends them, its OUT transfers' samples go to the
exchange, and its ring detector changes on each half cycle of the ring
signal; it hears the line on hook only with CID1 set. One of the two
fakes says it is an ST75951/2 (`FAKE_ST7554_DAA`) and checks its
start-up. The test checks that a wrong `<bus>:<address>` is refused, a
data call (rings, caller ID between the first two rings, the answer at
the second ring, V.34 at 33600, text each way, the LEDs of the one with
`--usb-leds` and none on the other, the buzzer of the one with
`--usb-buzzer` on as it dials and off at the CONNECT), a busy line, a call
dialled in pulses, a voice call, the handset detector in voice mode,
the international line controls (Australia's timer and pulse dialling,
France's DC bit, the ST75951's inverted one), the voice codec as a
handset (the fake's `FAKE_ST7554_VOICE`: its microphone a 1000 Hz
tone, its earpiece to a file; `+VLS=14` and `15`, `+VRX` and `+VTX`), RHOM (`--usb-rhom`: on while
the modem runs, none on a revision 0x0100 chip, `FAKE_ST7554_REVISION`), a
call that wakes a suspended 1002 (`--usb-wake`: the fake's ring with its
remote wakeup armed; the device reset on resume, set up again, the call
answered), and that both exit once the exchange (the devices) has
gone. It needs libusb-1.0-0-dev.

`dmodemd/test/pico_line_test.py` (ctest's `dmodemd_pico_line`, label
`alsa`) does the same with two pico-serial Line sticks:
`dmodemd/test/fake_pico_line.cpp` is a stick (1209:0002) whose line is
the exchange's, speaking its firmware's protocol: `SET_HOOK` and
`SET_MONITOR` are the exchange's switches, alternate setting 1 starts
its samples, its ring detector's changes come as events. On top of the
SmartUSB56's checks it has the country's line settings, and a stick
whose line side does not answer (said, and dmodemd goes on).

`dmodemd/test/agere_test.py` (ctest's `dmodemd_agere`, label `alsa`)
does it with two Agere USB modems at 8000 Hz: `dmodemd/test/fake_agere.cpp`
is one (047e:2892) whose line is the exchange's, with its vendor
commands, its ARM's memory and function table, its codec registers (the
hook register 0xc0 the exchange's switch) and its 100-octet bulk
transfers, whose status word's bit 0 is set while the ring signal is
there. It streams only once the patch its ROM wants is downloaded and
hooked in. The patches are the tests' own (`DMODEM_AGERE_TEST_PATCHES=1`
makes dmodemd look for them, `agere_patch.h`), put in a "driver" file of
other octets in a directory. It checks that dmodemd takes the patches
D-Modem ships when there is no `--usb-agere-driver`, and says what is
wrong with none there, a driver without the patches and a Polaris DAA; each ROM's patch (1.2.1 in ROM and in flash, 1.0.6 with its clock
change patch); 8000 Hz with `-r 9600`; a full speed device; a data call
(rings, caller ID, the answer at the second ring, V.34 at 33600, text
each way); a call dialled in pulses; and that both exit once the
exchange has gone.
