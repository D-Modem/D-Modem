# The speaker of the modem: technical details

This page tells how `dmodemd --speaker` sends the audio of a call to a
sound device. For a summary, refer to [The speaker of the modem](speaker.md).

## Parts

| Part | File | Function |
|---|---|---|
| `dcore::SpeakerPath` | `core/dcore.h` | The interface between the core and the speaker of a program. `volume(level, rate)` tells the volume when it changes (0 is off, 1 to 3 are the `ATL` levels) and the sample rate of the line. `audio(rx, tx, n)` gives each block of samples of the line while the speaker is on. |
| `dcore::Options::speaker` | `core/dcore.h` | The `SpeakerPath` of the program, or `NULL`. |
| `Modem::set_speaker()` | `core/modem.cpp` | Tells `Host::speaker()` and the `SpeakerPath` the volume. It calls the `SpeakerPath` only when the volume changes. |
| `MDMPRM_DIAL_DONE` | `dsp/modem_param.h`, `dsp/dp_callprog.cpp` | The call progress pump sets it at the end of the dial string. For `ATM3`, the core then sets the speaker on. |
| `dmd::SpeakerOut` | `frontend/dmodemd/speaker_out.h`, `speaker_out.cpp` | The `SpeakerPath` of `dmodemd`, on ALSA, waveOut or the Media Kit. |
| `--speaker` | `frontend/dmodemd/options.cpp`, `main.cpp`, `main_windows.cpp` | Opens the device before the modem starts, and gives it to the core. |
| `dmodem_speaker` | `frontend/dmodemd/CMakeLists.txt` | The library of `speaker_out.cpp`. It links ALSA (Linux), `winmm` (Windows) or `media` and `be` (Haiku). |

## When the speaker is on

`S29` keeps the setting of `ATM`, and `S30` keeps the setting of `ATL`.

| Event | `ATM0` | `ATM1` | `ATM2` | `ATM3` |
|---|---|---|---|---|
| `ATM` or `ATL` command | off | off | on | off |
| The call starts: the modem dials | off | on | on | off |
| The call starts: the modem answers | off | on | on | on |
| The call progress pump has dialed the full number | off | on | on | on |
| The modems connect (`CONNECT`) | off | off | on | off |
| The call stops | off | off | on | off |

On a digital line (SIP, ISDN), the modem does not dial digits. Thus the
dial string of the call progress pump is empty, and `ATM3` sets the
speaker on almost at the start of the call. A dial string that stops at
`;` does not set the speaker on.

`ATL0` sets the volume to 0. Thus, `ATL0` stops the speaker for all
settings of `ATM`.

## The audio

The core calls `audio()` in `Modem::process()`, after the pump. `rx` is
the signal that came from the line (before the DC filter of the core),
and `tx` is the signal that the modem sent. A speaker on the line of a
hardware modem hears the two signals. Thus `SpeakerOut` adds the two
signals together and sets the volume:

| `ATL` | Gain |
|---|---|
| 1 | -12 dB |
| 2 | -6 dB |
| 3 | 0 dB |

The result is clipped to 16 bits. The sample rate is the rate of the
line: 8000 Hz for SIP and ISDN, the `-r` rate for `-a` and `--usb`. The
sound system of the computer changes the rate for its device.

The core calls `audio()` on the thread of the line. `SpeakerOut` does
not block this thread. When the device cannot accept the samples, it
discards them. Thus a slow sound device causes gaps in the speaker, but
it does not cause errors on the line.

## Back ends

### Linux: ALSA

- `open()` opens the PCM (`snd_pcm_open()`, `SND_PCM_NONBLOCK`) at start.
  `dmodemd` can then open a device of the `audio` group before it changes
  to the user `nobody`.
- When the speaker comes on for the first time, `snd_pcm_set_params()`
  sets the format (S16, one channel), the rate of the line and a latency
  of 120 ms. The PCM starts to play when it has 40 ms of samples.
- When the speaker comes on, `snd_pcm_prepare()`. When it goes off,
  `snd_pcm_drop()`.
- `-EAGAIN` (the buffer is full): the samples are discarded. `-EPIPE`
  (an underrun): `snd_pcm_prepare()`, then the write again. Another
  error stops the speaker until `dmodemd` stops, with a line in the log.
- Some ALSA plugins open their files late (for example, the `file`
  plugin at `snd_pcm_set_params()`). When `dmodemd` starts as root,
  these files open as the user `nobody`.

### Windows: waveOut

- `open()` finds the device: `default` is `WAVE_MAPPER`, a number is the
  device of that number, and other text is a part of the name of a device
  (`waveOutGetDevCapsA()`, letters of the two cases are the same). If no
  device agrees, `dmodemd` shows the list of devices and stops.
- When the speaker comes on for the first time, `waveOutOpen()` opens the
  device at the rate of the line (16 bit PCM, one channel) with 6 buffers
  of 20 ms each. A different rate opens the device again.
- `write()` fills a buffer, and sends it with `waveOutWrite()` when it is
  full. If the next buffer is still in the queue of the device, the
  samples are discarded. Thus the delay is 120 ms at most.
- When the speaker goes off, `waveOutReset()`.
- The functions are in `winmm.dll` on all versions of Windows that
  `dmodemd` supports (Windows 95 and later).

### Haiku: the Media Kit

- `--speaker` accepts only `default`: the `BSoundPlayer` of the Media Kit
  goes to the system mixer.
- When the speaker comes on for the first time, `SpeakerOut` makes a
  `BSoundPlayer` at the rate of the line (16 bit, one channel, buffers of
  20 ms). If `InitCheck()` fails (for example, the media server does not
  run), the speaker stops until `dmodemd` stops.
- The thread of the line writes the samples into a ring of 8192 samples.
  The thread of the Media Kit reads them in the callback of the player
  (`SpeakerOut::pull()`). The ring has no lock: each thread writes only
  its own index. If the ring holds more than 160 ms, the reader discards
  the oldest samples and keeps 60 ms. This prevents a long delay when the
  two clocks are different. If the ring has too few samples, the reader
  gives silence.
- The callback accepts the 16 bit and the float formats, with one or more
  channels.
- Nobody has run this back end on Haiku. The build on Linux for Haiku
  compiles and links it (`libmedia.so`, `libbe.so`).

### Other systems

On macOS and the other systems, `speaker_supported()` is false, and
`--speaker` gives an error. `Host::speaker()` still operates (for example,
the buzzer of a SmartUSB56 with `--usb-buzzer`).

## The buzzer of the SmartUSB56

`--usb-buzzer` and `--speaker` can operate together. The buzzer follows
`Host::speaker()`, and `--speaker` follows the `SpeakerPath`. The two get
the same volumes. Refer to [Softmodem hardware](hardware-details.md).

## libdmodem_sip

A program that uses `libdmodem_sip` (for example, the Android app) can
also use `SipModem::on_speaker()` (`frontend/dmodemd/sip_modem.h`).
`SpeakerOut` does not change this function. A program can also put its
own `SpeakerPath` in `dcore::Options::speaker`.

## Tests

| Test | What it checks |
|---|---|
| `Core.SpeakerUntilConnect` (`core/test/core_test.cpp`, label `quick`) | `ATM1`: the speaker is on at the `ATL` volume at the two ends while the call starts, it gets samples, and it goes off at `CONNECT`. After `CONNECT`, it gets no samples. |
| `Core.SpeakerAfterDialling` (`core_test.cpp`, label `quick`) | `ATM3` on an analogue line: the speaker is off while the modem dials 3 DTMF digits, then it is on until `CONNECT`. `ATM1`: on at once. |
| `Core.SpeakerAlwaysAndNever` (`core_test.cpp`, label `quick`) | `ATM2`: on at once and during the data connection. `ATM0`: never on. |
| `dmodemd_alsa` (`frontend/dmodemd/test/alsa_test.py`, label `alsa`) | The first `dmodemd` has `--speaker`, a PCM of the `file` plugin of alsa-lib. After `CONNECT`, the file has more than one second of samples with a peak above 1000. During the data connection, the file does not get larger. |
