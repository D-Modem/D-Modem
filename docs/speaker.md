# The speaker of the modem

A hardware modem has a small speaker. You hear the dial tone, the
digits, the ringing tone and the handshake of the call. Then the speaker
stops when the modems connect. With `--speaker`, `dmodemd` does the same
on a sound device of the computer. Use it to hear why a call fails, for
example a handshake that does not complete.

For the full technical description, refer to
[the technical details](speaker-details.md).

## Start dmodemd with a speaker

Give the sound device to `--speaker`:

| System | Device | Example |
|---|---|---|
| Linux | An ALSA PCM | `dmodemd --speaker=default` or `--speaker=plughw:0` |
| Windows | A waveOut device: `default`, its number, or a part of its name | `dmodemd.exe --speaker=default` or `--speaker=Speakers` |
| Haiku | `default` (the system mixer) | `dmodemd --speaker=default` |

The speaker operates on all lines: SIP, the ALSA line (`-a`), the USB
modems (`--usb`) and ISDN (`--misdn`). On Linux, `dmodemd` must have the
ALSA library at build time (libasound2-dev). On macOS and on other
systems, `--speaker` gives an error.

## Control the speaker with AT commands

| Command | Speaker |
|---|---|
| `ATM0` | Always off. |
| `ATM1` | On until the modems connect (the default). |
| `ATM2` | Always on, also during the data connection. |
| `ATM3` | Off while the modem dials. On after the modem dials, until the modems connect. |
| `ATL0` | Off. |
| `ATL1`, `ATL2`, `ATL3` | Low, medium and high volume (`ATL3` is the default). |

For example, to hear only the handshake of a call that you dial:

    ATM3L2
    OK
    ATDT5551234

## Status

- **Linux (ALSA):** a test call between two `dmodemd` through a
  simulated exchange records the speaker (`alsa_test.py`).
- **Windows and Haiku:** the automated build servers compile the speaker.
  Nobody has listened to it on these systems yet.
