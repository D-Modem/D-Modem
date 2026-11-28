# V.90: technical details

This page gives the technical details for [V.90](v90.md). The clause
numbers are the clause numbers of V.90 (09/98). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [V.90: technical
details](../dsp/v90-details.md) and [V.90 analogue modem: technical
details](../dsp/v90a-details.md).

## The digital modem (5)

| Item | Rule |
|---|---|
| Data rates (5.1) | 28000 bit/s to 56000 bit/s, in steps of 8000/6 bit/s |
| Symbol rate (5.2) | 8000 symbols each second, from the clock of the digital network |
| Upstream symbol rates (5.2) | 3000 and 3200. 3429 is optional. |
| Scrambler (5.3) | The V.34 scrambler with GPC (1 + x<sup>-18</sup> + x<sup>-23</sup>) |
| Data frame (5.4) | Six symbols, data frame intervals 0 to 5 |
| Mapping parameters (5.4.1) | Six code sets (one for each interval), K modulus encoder bits, S sign bits for data, Sr sign bits for spectral shaping (S + Sr = 6) |
| Ucode (3.6) | A number from 0 to 127 for each code magnitude, the same for µ-law and A-law |

- The modulus encoder (5.4.3) changes K bits into six numbers. Each
  number is less than the number of codes in the code set of its
  interval.
- The spectral shaper (5.4.5) uses the Sr redundant sign bits. It
  removes power near 0 Hz, or at other frequencies that the analogue
  modem selects.

## The analogue modem (6)

The upstream direction is the V.34 primary channel, with these limits:

- Data rates of 4800 bit/s to 28800 bit/s, in steps of 2400 bit/s.
  31200 bit/s and 33600 bit/s are optional. There is no auxiliary
  channel.
- Symbol rate 3200 is necessary. 3000 and 3429 are optional. 2400, 2743
  and 2800 are not permitted.
- The scrambler uses GPA (1 + x<sup>-5</sup> + x<sup>-23</sup>).

## Phase 1 and phase 2 (9.1, 9.2)

- V.8 selects V.90 (bit 5 of modn0 and the V.90 availability category).
  If the two modems can each be the digital modem, the call modem is the
  analogue modem.
- After CJ, the two modems are silent for 75 ms ± 5 ms.
- Phase 2 is the phase 2 of V.34. The digital modem has the role of the
  V.34 caller (INFO0d, then tone B). The analogue modem has the role of
  the V.34 answerer (INFO0a, then tone A).

| Step | Digital modem | Analogue modem |
|---|---|---|
| 1 | Sends INFO0d, then tone B | Sends INFO0a, then tone A |
| 2 | | After tone B and at least 50 ms of tone A, sends A-bar |
| 3 | Sends B-bar 40 ms ± 1 ms after A-bar arrives, then 10 ms more of tone B | Measures the round-trip delay |
| 4 | | Sends A-bar 40 ms ± 1 ms after B-bar arrives, 10 ms more of tone A, then L1 and L2 |
| 5 | Receives L1 (160 ms) and L2 (at most 500 ms). Sends tone B. | Sends tone A for 50 ms, then A-bar |
| 6 | Sends B-bar 40 ms ± 1 ms after A-bar arrives, then L1 and L2 | Receives L1 and L2, then sends tone A |
| 7 | Sends INFO1d | Sends INFO1a. Bits 37:39 select V.90 (6) or V.34 (0 to 5). |

## Phase 3 (9.3)

| Signal | From | Content |
|---|---|---|
| S, S-bar | Analogue | 128T and 16T, 70 ms ± 5 ms after INFO1a |
| MD | Analogue | Optional, with the length that INFO1a gives |
| PP, TRN | Analogue | TRN of 4 points, at least 512T |
| Ja | Analogue | The DIL descriptor: the DIL that the analogue modem wants |
| Sd, Sd-bar | Digital | 384T and 48T, at most 500 ms after Ja |
| TRN1d | Digital | At least 2040T. Jd starts in less than 4000 ms. |
| Jd, Jd' | Digital | The rates of the digital modem, until S from the analogue modem. Jd' is 12T. |
| DIL | Digital | N segments of training codes and reference codes, until the second S-bar from the analogue modem |

- The DIL segment c has (H<sub>c</sub> + 1) × 6 symbols. The sign
  pattern (SP) and the training pattern (TP) start again in each
  segment. The least significant bit applies to the first symbol.
- The analogue modem stops Ja when it detects Sd-bar. If Sd-bar does not
  come in 1500 ms, the analogue modem retrains.
- The digital modem retrains if S does not come in 5100 ms plus a
  round-trip delay from the start of TRN1d.

## Phase 4 (9.4)

| Signal | From | Content |
|---|---|---|
| CPt | Analogue | The constellations for the training of phase 4 |
| Ri, Ri-bar | Digital | Ri for at least 192T until CPt arrives, then Ri-bar for 24T |
| TRN2d | Digital | At least 2040T. MP starts in less than 2000 ms. |
| CP, CP' | Analogue | The constellations, the shaping and the rate for data |
| MP, MP' | Digital | The rates and the options. The prime (') acknowledges the far sequence. |
| E, Ed | Both | The end of the sequences |
| B1, B1d | Both | B1d is 48 data frames at the data rate |

- Each modem retrains if B1 (or B1d) does not come in 15 s plus five
  round-trip delays after INFO1a.

## Retrains and rate renegotiation (9.5, 9.6)

| Procedure | Rule |
|---|---|
| Retrain (9.5) | Silence for 70 ms ± 5 ms, then tone B (digital) or tone A (analogue). A tone of more than 50 ms starts a retrain at the far modem. |
| Rate renegotiation from the digital modem (9.6.1) | Rd for 384T and Rd-bar for 24T, on the boundary of a data frame. Optional TRN2d (at most 2000 ms), then MP. |
| Rate renegotiation from the analogue modem (9.6.2) | S for 128T and S-bar for 16T, optional SCR (at most 2000 ms), then CP |
| Echo canceller training (9.6.2.1) | CPs (bit 30 of CP) asks for silence. The digital modem sends Ucode 0 and keeps the data frame alignment. Then Rt and Rt-bar. |
| Timeout | A retrain if E (or Ed) does not come in 5000 ms plus two round-trip delays |
| Cleardown (9.7) | A rate renegotiation with drn = 0 in CP or MP |

## V.90 in D-Modem

| Item | D-Modem |
|---|---|
| Digital modem | `dsp/v90.cpp` (the transmitter) and `dsp/dp_v34.cpp` (the start-up) |
| Analogue modem | `dsp/v90a.cpp` (the PCM receiver) and `dsp/dp_v34.cpp` |
| Data rates | 28000 bit/s to 56000 bit/s down, V.34 rates up to 33600 bit/s up |
| Phase 2 answers | 40 ms ± 1 ms, with the V.34 code (`V34.ToneReversalDelay`). A recorded V.90 call (open modem to open modem) shows 40 ms on each side. |
| Sd, Sd-bar, TRN1d | 384T, 48T and 8000T (1 s), 50 ms after Ja |
| Jd' | 12T |
| Ri, Ri-bar | Ri until CPt and at least 192T, then Ri-bar for 24T |
| TRN2d | 2160T (360 data frames), then MP |
| B1d | 48 data frames |
| Rd, Rt | 384T and 24T, on the boundary of a data frame |
| Retrain silence | 70 ms |

We found no error in the V.90 procedures. The interop tests with the
modems of other companies are on [Interop](../interop.md).
