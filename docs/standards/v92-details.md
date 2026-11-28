# V.92: technical details

This page gives the technical details for [V.92](v92.md). The clause
numbers are the clause numbers of V.92 (11/2000). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [V.92: technical
details](../dsp/v92-details.md) and [V.90 and V.92 analogue modem:
technical details](../dsp/v90a-details.md).

## PCM upstream: the analogue modem (6)

| Item | Rule |
|---|---|
| Data rates (6.1) | 24000 bit/s to 48000 bit/s, in steps of 8000/6 bit/s |
| Symbol rate (6.2) | 8000 symbols each second, from the clock of the digital network |
| Transmitter (6.4) | A modulus encoder, a precoder and a prefilter that the digital modem designs, an inverse map and a convolutional (trellis) encoder |

The digital modem measures the upstream channel during the training.
Then it sends the coefficients of the precoder and the prefilter to the
analogue modem in CPd.

## Short phase 1: quick connect (8.2, 8.3, 9.2)

| Signal | Content |
|---|---|
| QC1a, QC2a | From the analogue modem, on V.21. QC1a comes before CM. The bits give UQTS, the Ucode for QTS (one of 15 values), and a LAPM request. UQTS 1111 is a cleardown from the on-hold state. |
| QC1d, QC2d | The same, from the digital modem |
| QCA1a, QCA1d, QCA2a, QCA2d | The answer to a QC signal |
| QTS, QTS-bar | 128 repetitions of {+V, +0, +V, −V, −0, −V}, V = UQTS, then 8 repetitions with the signs reversed. QTS starts in data frame interval 0. |
| ANSpcm | A PCM sequence that makes a tone of about 2100 Hz. It repeats every 301 symbols, with a phase reversal every 3612 symbols. Levels of −9.5, −12, −15 or −18 dBm0. |
| TONEq | The answer of the analogue modem to ANSpcm: the mark tone of V.21(L), 980 Hz |

The procedure when the digital modem answers (9.2.4):

1. It sends ANSam. It detects QC1a (or CM).
2. It sends QCA1d, then silence for 75 ms ± 5 ms, then QTS, QTS-bar and
   ANSpcm.
3. When it detects TONEq, it sends silence for 75 ms ± 5 ms, then
   starts phase 2.
4. If TONEq does not come in 2 s after QCA1d, it sends ANSam and
   continues with V.8.

The analogue modem (9.2.1) sends TONEq when ANSpcm was there for 1 s, or
at once if ANSam was there for 1 s before. It sends TONEq for at least
50 ms. When ANSpcm stops, it stops TONEq, sends silence for 75 ms ± 5 ms
and starts phase 2.

## Short phase 2 (9.4)

Short phase 2 has no line probe. INFO0d (bit 26) and INFO0a (bit 27)
ask for it.

| Step | Digital modem | Analogue modem |
|---|---|---|
| 1 | Sends INFO0d, then tone B | Sends INFO0a, then tone A |
| 2 | After tone A and at least 50 ms of tone B, sends B-bar and 10 ms more of tone B | |
| 3 | | Sends A-bar 40 ms ± 1 ms after B-bar arrives, 10 ms more of tone A, then INFO1a |
| 4 | Measures the round-trip delay. Receives INFO1a. | |

- If the A-bar does not come in 2500 ms after its B-bar, the digital
  modem uses the full phase 2.
- If INFO1a does not come in 2500 ms, the digital modem sends tone B and
  uses the full phase 2.

## Phase 3 and phase 4 with PCM upstream (9.5, 9.6)

| Signal | From | Content |
|---|---|---|
| Ru, Ru-bar | Analogue | 384T and 24T: a time reference for the upstream PCM |
| TRN1u | Analogue | At least 2040T. The digital modem trains its upstream equalizer. |
| Ja | Analogue | The DIL descriptor |
| Sd, Sd-bar, TRN1d, Jd | Digital | As V.90 |
| Jp, Jp' | Digital | The parameters of the upstream channel: the delay and the length of the filters |
| Su, Su-bar | Analogue | Su-bar is 24.5T, then (24 + ε)T. The fraction ε aligns the upstream symbols with the clock of the network. |
| DIL | Digital | As V.90 |
| CPt, E1u | Analogue | The constellations for the training of phase 4 |
| Ri, Ri-bar | Digital | As V.90 |
| TRN2u | Analogue | The upstream training of phase 4 |
| SUVu, CPu | Analogue | The upstream sequences: the downstream constellations and the rates |
| SUVd, CPd | Digital | The upstream constellations, the precoder and the prefilter |
| E2u, Ed | Both | The end of the sequences |
| B1u, B1d | Both | Data frames of ones at the data rate |

The fast parameter exchange (9.9) changes the rates and the parameters
with no training. The digital modem starts it with Rf (384T) and Rf-bar
(24T). The analogue modem starts it with RM (384T) and RM' (24T). Then
the two modems send SUV and CP sequences with the modulation of the data
mode, and keep the data frame alignment.

## Modem-on-hold (8.9, 9.10)

| Item | Rule |
|---|---|
| RT | Tone A or tone B, as the modem sends in a retrain. At least 50 ms before the first MH sequence (20 ms between two sequences). |
| MH sequences | 40 bits, modulated as the INFO sequences of phase 2, sent again and again |
| MHreq | Asks the far modem to go on hold |
| MHack | Agrees. Bits 16:19 give T1, the time the modem waits on hold: 10 s to 16 min, or no limit. |
| MHnack | Refuses, and asks for a cleardown or a fast reconnect |
| MHclrd, MHcda | Ask for a cleardown, and agree to it |
| MHfrr | Asks for a fast reconnect |

| Procedure | Rule |
|---|---|
| Initiator | It sends its sequence until the answer comes. If no answer comes in 2 s plus a round-trip delay, it retrains or disconnects. |
| Responder | It sends its answer until ANSam or silence comes, or the sequence of the initiator stops for 200 ms |
| On hold (after MHack) | When RT is there for 100 ms or silence for 2 s, the modem stops MHack. In less than 80 ms it sends ANSam. It waits for phase 1 (QC or CM) for T1 from the end of the first MHack. Then it disconnects. |
| After MHnack | The initiator sends MHcda or MHfrr in less than 10 s. On MHfrr, the far modem sends silence for at most 80 ms, then ANSam. |

## V.92 in D-Modem

| Item | D-Modem |
|---|---|
| Roles | Digital modem (`AT+MS=92`, the default on a digital line) and analogue modem |
| Quick connect | QC1a and QCA1d. We do not send QC2a or QC2d (V.8 bis). |
| ANSpcm | The 301-symbol sequence, with a reversal every 3612 symbols |
| Silence after TONEq (digital modem) | Until TONEq stops, then 75 ms. V.92 asks for 75 ms ± 5 ms from the detection of TONEq. The Broadcom, SM56 and HSF modems do not receive INFO0d when it comes this early, and the call fails. Thus, we keep this difference. |
| Short phase 2 | The A-bar of the analogue modem 40 ms ± 1 ms after B-bar, with the V.34 code |
| PCM upstream | Transmitter and receiver, up to 48000 bit/s, with the precoder and the prefilter |
| Fast parameter exchange | The two ends |
| Modem-on-hold | MHreq, MHack, MHnack, MHcda and MHfrr, at the two ends. Our RT is 600 ms before MHreq. |
| Settings | `DSP_V92=0`, `DSP_V92_QC=0`, `DSP_V92_PCMUP=0`, `DSP_V92_MOH` |

### Corrections (2026-11)

| Error | Correction |
|---|---|
| On hold, T1 counted from the start of the hold (the end of RT or silence after MHack). The modem then waited for phase 1 longer than T1, by up to the 30 s of MHack. | T1 counts from the end of the first MHack (`moh_ack_end`). `open.open_v92a_moh_theirs` checks it. |
| The initiator of MHreq waited 2.5 s for the answer, also on a line with a round-trip delay of more than 500 ms | It waits 2 s plus the measured round-trip delay |
| The digital modem waited 2 s for TONEq from the start of QTS, that is, 2.075 s after QCA1d | It waits 2 s from the end of QCA1d |
