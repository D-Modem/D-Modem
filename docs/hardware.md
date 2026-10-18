# Softmodem hardware: dmodemd on ALSA and USB

dmodemd's line is normally a SIP call. With `-a` it is instead a
softmodem's codec on a telephone line, through ALSA. With `--usb` it is
a Smart Link SmartUSB56, through libusb ([below](#smart-link-smartusb56)).
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
| AC'97 modem (MC97, AMR and CNR risers, laptops of about 2000 to 2006) | `snd-intel8x0m` (Intel ICH to ICH7, AMD-768/8111, nForce, SiS 7013, ALi), `snd-atiixp-modem` (ATI IXP), `snd-via82xx-modem` (VIA) | the codec's own AC'97 modem GPIOs: Silicon Labs Si3036/38, Agere, Motorola and others |
| HD-audio modem (laptops of about 2005 to 2010) | `snd-hda-intel` with `snd-hda-codec-si3054` | Silicon Labs Si3054/3055, Agere (11c1:3026, 3055, 3155), Motorola (1057:3055, 3057, 3155), SM56 (1543:3155), LG (1854:0018), a VIA board's (1106:3288) |

The codec appears as a card's PCM, with two switches among its controls:
"Off-hook Switch" and "Caller ID Switch". To find it:

    aplay -l                      # a "Modem" device: card 1, device 0 is hw:1
    amixer -c 1 controls | grep -i -e hook -e 'caller id'

An HD-audio modem is a device of the audio card, e.g. `hw:0,6`.

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
pkg-config finds libusb-1.0 (libusb-1.0-0-dev).

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

### Not covered

Not covered yet, because Linux has no driver that gives their samples:

- Conexant's AC'97 codecs (their hook is in the SmartDAA behind codec
  register 0x7a, not the standard GPIO);
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
| `--usb=<dev>` | a SmartUSB56 (`auto`, the first one; or `<bus>:<address>`, as `lsusb` shows it): the line is a telephone line, not SIP |
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
  levels) are what its kernel driver sets: ALSA gives no control of
  them. The DTMF levels stay the pump's own.
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
