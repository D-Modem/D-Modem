# Softmodem hardware: dmodemd on ALSA

dmodemd's line is normally a SIP call. With `-a` it is instead a
softmodem's codec on a telephone line, through ALSA. The modem is then
D-Modem's open DSP, on any architecture. Smart Link's slmodemd, Agere's
agrsm and their kind were closed, mostly i386-only programs; dmodemd can
replace them.

    dmodemd -a hw:1 -r 9600

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

Not covered yet, because Linux has no driver that gives their samples:

- Conexant's AC'97 codecs (their hook is in the SmartDAA behind codec
  register 0x7a, not the standard GPIO);
- Conexant's and some Agere HD-audio modems not in the Si3054 list;
- modems with their own PCI controller (Smart Link SmartPCI, PCTel HSP,
  Lucent/Agere PCI, Conexant HSF PCI, Broadcom BCM4212);
- Smart Link's USB modem (ST7554).

## Options

| Option | |
|---|---|
| `-a <pcm>`, `--alsa=<pcm>` | the codec's PCM (`hw:1`, `hw:0,6`, `plughw:...`): the line is a telephone line, not SIP |
| `--alsa-ctl=<ctl>` | its controls, if not the card's (`hw:1` for `hw:1,0` or `plughw:1,0`; the PCM's own name otherwise) |
| `-r <Hz>`, `--rate=<Hz>` | the codec's rate: 9600 (the default) or 8000, the data pumps' |

Started as root, dmodemd opens the codec before it goes on as nobody, so
the device's group (usually `audio`) does not matter. Started as a user,
that user needs access to `/dev/snd/*`.

`SIP_LOGIN` is not needed with `-a`.

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
  (`RING`, `ATS0` answering, `S1`).
- **Caller ID.** At the first ring the core runs its caller ID receiver
  (`AT#CID`) on the monitor's samples for 4 s.
- **Dialling.** The modem dials the number itself in DTMF. With `ATX4`
  (the default) it waits for a dial tone first; with `ATX3` it waits 2 s.
  In voice mode, `ATD` waits 2 s, dials, then takes the call as answered
  by the ringing tone's end (`+VRA`), or by `+VRN` without one (V.253
  9.1.1).
- **Modulations.** The core treats the line as an analogue one
  (`Host::analog_line()`): the modem is the analogue end of the call.
  `+MS`'s V.90 and V.92 (the default) run V.90 as the analogue modem
  at 9600 Hz (`-r 9600`, the default), up to 56000 bit/s down and
  33600 up from a V.90 server (see [V.90 analogue modem](dsp/v90a.md)),
  and V.34 (up to 33600 bit/s) and below with any other modem. V.91,
  between two digital modems, becomes V.34. At 8000 Hz (`-r 8000`)
  V.90 is there only with `DSP_V90A=1`: a codec at 8000 Hz on a clock
  of its own loses the PCM signal every few seconds (see
  [8000 Hz lines](dsp/v90a.md#8000-hz-lines)), so keep 9600. It has fax
  (class 1) and voice too.

## Limits

- **Speed.** As the analogue modem the open DSP has V.90, with its
  retrains and rate renegotiations (a line that changes mid-call
  renegotiates, and one PCM cannot use falls back to V.34), but not
  yet V.92's (no quick connect, PCM upstream or modem-on-hold). The
  V.90 analogue modem has been tested against our own digital modem on
  the simulated network only, not yet against an ISP's server.
- **Dialling.** DTMF only: `ATDP` dials in DTMF too, as a hook switch
  through a mixer control cannot time pulses.
- **Country rules.** The codec's line settings (impedance, ring
  thresholds, levels) are what its kernel driver sets.
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
- a voice call: the ringing tone, OK at `+VRA`, `+VTS` DTMF across, and
  the busy tone after the other end hangs up.

`RATE=8000` runs it at 8000 Hz. It needs libasound2-dev.
