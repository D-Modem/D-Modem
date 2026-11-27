# V.25: technical details

This page gives the technical details for [V.25](v25.md). The clause
numbers are the clause numbers of V.25 (10/1996). This page is our own
description, not a copy of the Recommendation.

## The signals (2)

| Signal | Frequency | Timing |
|---|---|---|
| Calling tone | 1300 Hz ± 15 Hz | ON for 0.5 s to 0.7 s, then OFF for 1.5 s to 2.0 s. Then again. |
| Answer tone | 2100 Hz ± 15 Hz | Continuous, 3.3 s ± 0.7 s. |
| Phase reversals | (in the answer tone) | 180° each 425 ms to 475 ms. |
| Calling station response | (refer to the modulation) | An optional signal of the call modem after it identifies the answer tone. |

- A phase reversal must be complete in 1 ms, with an error of ± 10°.
  During the reversal, the amplitude must not be more than 3 dB below its
  usual value for more than 400 µs.
- The calling tone and the calling station response must have no power in
  the band 2100 Hz ± 250 Hz. Thus, they do not interfere with the answer
  tone.
- The levels of all signals agree with V.2.

## The answer modem (4)

1. The ring signal comes. The modem sets circuit 125 (ring indicator) ON.
2. If circuit 108 (data terminal ready) is ON, the modem connects to the
   line. If not, the modem waits until circuit 108 comes ON. If circuit
   108 does not come ON, the modem does not answer.
3. The modem is silent for 1.8 s to 2.5 s.
4. The modem sends the answer tone. To disable echo cancellers too, the
   modem adds phase reversals.
5. The answer tone continues for 3.3 s ± 0.7 s. If the modem receives a
   calling station response for 100 ms, it can stop the answer tone
   earlier.
6. The modem is silent for 75 ms ± 20 ms. Then it sets circuit 107 (data
   set ready) ON and starts its modulation.

A modem that only receives calls from acoustic couplers can send the
answer tone for up to 10 s. This gives the person time to put the
telephone handset on the coupler.

## The call modem (3)

1. The DTE tells the modem to dial. The modem connects to the line and
   dials.
2. If the modem has a calling tone, it sends the calling tone.
3. The modem identifies the answer tone in 100 ms to 600 ms. Then it
   stops the calling tone.
4. The modem can send a calling station response. It must wait at least
   400 ms after the last calling tone burst. With phase reversals, it must
   wait at least 1 s.
5. The modem finds the end of the answer tone (no tone for 75 ms ± 20
   ms). Then it sets circuit 107 ON and continues with its modulation.

The answer tone must not set circuits 104 (received data) or 109
(carrier detected).

## Echo control (5, 8)

- The answer tone disables the echo suppressors. It must arrive during a
  silent period of the calling tone.
- The phase reversals disable the echo cancellers.
- After the 75 ms silence, the modems must send a signal without gaps. If
  a gap is more than 100 ms, the echo control equipment can start again.
  This is important on satellite circuits.

## Calls by hand (6, 7, 8, 10)

- **A person calls an automatic modem.** The person dials and hears the
  answer tone. The person then pushes the data button of the modem. The
  microphone must be silent for at least 1 s during the answer tone.
- **An automatic modem calls a person.** The person hears the calling
  tone. The person pushes the data button. The modem sends up to 4 s of
  answer tone.
- **Two persons change from voice to data.** The two persons agree which
  modem sends the answer tone. Then they push the data buttons.
- A modem must let the operator select automatic answer or manual answer.
  A call that a person answers by hand starts in voice mode.

## The detector of the answer tone (11)

- The detector can stop during the bursts of the calling tone. Thus, the
  calling tone does not cause a false detection.
- The detector must not respond to speech or to the tones of the network.
  For example, the detector must ignore 2100 Hz if another signal has
  almost the same level in one of these bands: 350 Hz to 1800 Hz, or
  2500 Hz to 3400 Hz.

## V.25 in D-Modem

| Item of V.25 | D-Modem | File |
|---|---|---|
| Calling tone | 1300 Hz, 0.6 s ON in each 2.5 s (1.9 s OFF), from 0.5 s after the dial | `dsp/dp_callprog.cpp` |
| Answer tone detection | 300 ms of 2100 Hz | `dsp/dp_callprog.cpp` |
| Silence before the answer tone | 2 s (the answer delay of the core) and 200 ms | `dsp/dsp.h` (`ANS_SILENCE_MS`), `dsp/dp_v34.cpp` |
| Answer tone | 3.3 s | `dsp/dsp.h` (`ANS_TONE_MS`) |
| Silence after the answer tone | 75 ms | `dsp/dsp.h` (`ANS_GAP_MS`) |
| Phase reversals | Each 450 ms, in ANSam for V.8 | `dsp/dp_v34.cpp` |

A fax call sends the CNG tone of T.30 (1100 Hz) in place of the calling
tone. Some special modes send other calling tones. For example, FastPOS
sends 980 Hz and ETC sends 1500 Hz.

The check against V.25 found no errors.
