# Softmodem hardware: dmodemd on ALSA and USB

dmodemd's line is normally a SIP call. With `-a` it is instead a
softmodem's codec on a telephone line, through ALSA. With `--usb` it is
a Smart Link SmartUSB56 ([below](#smart-link-smartusb56)) or a
pico-serial Line stick ([below](#pico-serial-line)), through libusb.
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
  bit 7 a write, bit 6 with the line's control registers 0x03, 0x14,
  0x15, 0x17 and 0x1c to 0x25), the answer from 1. The status is SSD
  register 0, or 4 on some SSDs: bit 4 done and bit 3 an error, both
  cleared before each command, bit 2 toggling at each frame over the
  barrier, bit 1 the link up. A write is good when its answer is the
  byte written; the driver tries 5 times;
- the hook is LSD register 0x15, bits 7:6: 01 on hook, 10 off hook, 00
  and 11 a dial pulse's make and break;
- the driver's library knows 39 kinds of SSD, which differ in the LSD
  access: some prime SSD register 1 before each command (0x55 for a
  read, the byte's inverse for a write) to tell an absent LSD, some
  have the status in register 4, some send a read's command before
  clearing the status. dmodemd takes the plain kind, which the newest
  ones are.

Only the hook is done yet. Conexant's driver also sets the line's DC
termination and voltage on going off hook (LSD registers 0x18, 0x19 and
0x1d), detects rings and reads caller ID through the LSD, and keeps
country settings there. A codec whose SSD is of another kind, or whose
line side needs that setup to hold the line, may not go off hook yet.
It has been tested against a simulated SmartDAA (`cx_daa_test`), not
yet on a real codec.

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
- **Unplugged.** dmodemd says so and exits.

Limits: the ST7554's codec hears nothing on hook, so there is **no
caller ID** (Smart Link's driver had none either). The ST75951/2
"silicon DAA" variant (`MONOLITHIC` in the GPIO status) is refused, as
Smart Link's driver did not support it. The device's line settings are
its own, and it has not yet been tried on a real SmartUSB56.

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
| `--usb=<dev>` | a SmartUSB56 or pico-serial Line (`auto`, the first one; or `<bus>:<address>`, as `lsusb` and `--usb list` show it): the line is a telephone line, not SIP; `--usb list` lists them and exits |
| `-r <Hz>`, `--rate=<Hz>` | the codec's rate: 9600 (the default) or 8000, the data pumps' |

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
signal. The test checks that an ST75951/2 and a wrong `<bus>:<address>`
are refused, a data call (rings, the answer at the second ring, V.34 at
33600, text each way), a busy line, a call dialled in pulses, a voice
call, and that both exit once the exchange (the devices) has gone. It needs libusb-1.0-0-dev.

`dmodemd/test/pico_line_test.py` (ctest's `dmodemd_pico_line`, label
`alsa`) does the same with two pico-serial Line sticks:
`dmodemd/test/fake_pico_line.cpp` is a stick (1209:0002) whose line is
the exchange's, speaking its firmware's protocol: `SET_HOOK` and
`SET_MONITOR` are the exchange's switches, alternate setting 1 starts
its samples, its ring detector's changes come as events. On top of the
SmartUSB56's checks it has caller ID between the first two rings (the
stick hears the line on hook), and a stick whose line side does not
answer (said, and dmodemd goes on).
