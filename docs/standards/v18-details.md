# V.18: technical details

This page gives the signals, the automoding procedures and the modes of
V.18 (11/2000). For a summary, refer to [V.18](v18.md).

## The signals of V.18 (3)

| Signal | Description |
|---|---|
| CI | The CI of V.8 for a textphone, at 300 bit/s in V.21 channel 1. Bursts of 4 CI sequences (400 ms), with 2 s of silence between them. |
| TXP | Ten ONEs, then "TXP" as three 7-bit characters with even parity, at 300 bit/s. The caller sends it in V.21 channel 1, the answerer in channel 2. |
| XCI | 3 s of V.23 mark (1300 Hz) at 1200 bit/s, with four markers of two octets of ONEs: 400 ms of mark, then a marker and 800 ms of mark three times, then a marker and 100 ms of mark. |

## The operational requirements (4)

- The DCE does not start a disconnect.
- The DCE can go back to automoding after 10 s with no signal (an
  option).
- The calling DCE sends CI.
- The DCE gives call progress indications to the DTE.

## Automoding, calling (5.1)

The caller sends 1 s of silence, then three times CI and 2 s of
silence, then XCI. The cycle repeats. The caller listens meanwhile.

| The caller detects | It does |
|---|---|
| ANSam (5.1.2) | The V.8 procedures |
| ANS, 2100 Hz (5.1.3) | It stops, sends 0.5 s of silence, then TXP while ANS continues. It listens for TXP, 1650 Hz and 1300 Hz. |
| TXP in channel 2 (5.1.3.2) | V.18 mode (Annex G) |
| 1650 Hz for 0.5 s (5.1.3.3, 5.1.5) | Annex F |
| 1300 Hz for 1.7 s (5.1.3.4, 5.1.6) | Annex E, it sends at 75 bit/s |
| 2225 Hz for 0.5 s (5.1.4) | Annex D |
| 390 Hz for 3 s during XCI (5.1.7) | Annex E, it sends at 1200 bit/s. The XCI mark continues while 390 Hz continues. |
| 5-bit characters (5.1.8) | Annex A, at the rate of their bits |
| DTMF (5.1.9) | Annex B |
| 980 Hz or 1180 Hz (5.1.10) | 110 bit/s: Annex C. 980 Hz only for 1.5 s: Annex F, answer mode. |
| 1270 Hz for 0.7 s (5.1.11) | Annex D, answer mode |

## Automoding, answering (5.2)

The answerer listens for 3 s (timer Ta).

| The answerer detects | It does |
|---|---|
| CI or XCI (5.2.2) | It sends ANSam and listens for CM and TXP for 3 s (Tt). After TXP: 75 ms of silence, three TXPs in channel 2, then V.18 mode. |
| 2100 Hz for 0.7 s (5.2.3) | It listens for 980 Hz, 1300 Hz and 1650 Hz |
| 980 Hz (5.2.4) | V.21 channel 1 at 110 bit/s: Annex C. At 300 bit/s: Annex F. 980 Hz only for 1.5 s: Annex F, answer mode. |
| A calling tone: 1300 Hz or 980 Hz for 470 to 730 ms, then 1 s of silence (5.2.4.2, 5.2.10) | The probes at once |
| 5-bit characters (5.2.5) | Annex A, at the rate of their bits |
| DTMF (5.2.6) | Annex B |
| 1270 Hz for 0.7 s (5.2.7) | Annex D, answer mode |
| 2225 Hz for 1 s (5.2.8) | Annex D, calling mode |
| 1650 Hz for 0.4 s (5.2.9) | Annex F, calling mode |
| 1300 Hz for 1.7 s (5.2.11) | Annex E, it sends at 75 bit/s |
| Nothing for Ta (5.2.12) | The probes |

### The probes (5.2.12)

The probes start with ANSam. Then the answerer sends each probe of a
list in turn. Appendix I gives a list for each country.

| Probe | What the answerer sends |
|---|---|
| A message probe: 5-bit, EDT or DTMF (5.2.12.1) | The stored message in the mode, then silence for timer Tm (3 s by default). The 5-bit probe is at 47.6 bit/s (A.3). |
| A carrier probe: V.21, Bell 103 or V.23 (5.2.12.2) | ANSam for 1 s, 75 ± 5 ms of silence, then the carrier (1650 Hz, 2225 Hz or 1300 Hz) for timer Tc (6 s by default) |

## The modes (Annexes A to G)

| Annex | Mode | Signal | Characters |
|---|---|---|---|
| A | 5-bit (Baudot) | 1400 Hz (1), 1800 Hz (0), 45.45 or 50 bit/s, half duplex. The carrier starts 150 ms before the first character. | A start bit, 5 bits, 1.5 stop bits. LTRS before the first character, and LTRS or FIGS again after each 72 characters. |
| B | DTMF | Digits of at least 70 ms, with at least 50 ms of silence. The receiver detects 40 ms. | Up to four digits for each character (Tables B.1, B.2) |
| C | EDT | V.21 channel 1 at 110 bit/s, half duplex. The carrier starts 300 ms before the first character. | A start bit, 7 bits, even parity, 2 stop bits |
| D | Bell 103 | 1270/1070 Hz and 2225/2025 Hz, 300 bit/s duplex | 7 bits, even parity, a stop bit |
| E | V.23 (Minitel, Prestel) | 1200 bit/s and 75 bit/s | 7 bits, even parity, a stop bit |
| F | V.21 | 300 bit/s duplex | 7 bits, even parity, a stop bit |
| G | V.18 | V.21 at 300 bit/s | 8 bits, no parity, a stop bit (T.140) |

In the half duplex modes, the receiver is off for 300 ms after the
DCE sends a character, so that it does not detect the echo.

### The 5-bit code (Tables A.1 and A.2)

- 00000 is a backspace in letters and in figures.
- In figures: 00101 has no character, 10100 is `=`, 11010 is `+`.
- The DCE sends a near character for some characters that the code has
  not. For example, `#` is sent as `$`, `&` as `+`, `*` as `.`, `@` as
  X and `[` as `(`.
- DEL from the DTE puts the remote converter in letters (LTRS).

## V.18 in D-Modem

| Item | D-Modem (`dsp/dp_text.cpp`) |
|---|---|
| Modes | All of Annexes A to G. `+MBAUDOT` also gives telex and RTTY with other rates, tones and the ITA2 code. |
| Automoding, calling | CI, XCI and TXP as 5.1. ANSam and ANS both start TXP (5.1.3): the caller does not do the V.8 procedures. |
| Automoding, answering | As 5.2. The answer tone to CI or XCI is ANS (2100 Hz without modulation), not ANSam, so that a V.18 caller continues with TXP. |
| The 5-bit rate | The rate of the bits of the first character: 45.45 or 50 bit/s, when `+MBAUDOT` gives one of the two |
| The probes | 5-bit (47.6 bit/s), EDT, DTMF, V.21, Bell 103, V.23, in this order. ANSam first; Tm 3 s; Tc 6 s. `+MV18S`'s probe parameter 0 turns them off. |
| The message | `+MV18AM` |
| The return to automoding after 10 s (4, item 2) | Not done. The DCE stays in the mode. |
| The automoding monitor mode (5.3) | Not done |
| CM after ANSam (5.2.2.1, 5.2.12.3) | Not done: D-Modem does not do V.8 in automoding |
| The national letters of DTMF (#*1 to #*6) | Not done: the DTE characters are ASCII |
| The accent `` ` `` in the 5-bit code | Not sent. Table A.2 gives 00111 for it. |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The 5-bit code (Tables A.1, A.2) | The figures of the US TTY: BEL at 00101, `#` at 10100, `&` at 11010, no backspace. No near characters. | The figures of V.18: no character at 00101, `=` at 10100, `+` at 11010, backspace at 00000. The near characters of Table A.2. |
| LTRS first, and every 72 characters (A.4) | The first code was FIGS for a figure. No shift code again in a long line. | LTRS before the first character. LTRS or FIGS again after each 72 characters. DEL sends LTRS. |
| The 5-bit rate (5.1.8, 5.2.5) | The rate of `+MBAUDOT` only | 45.45 or 50 bit/s, from the bits of the first character |
| The 5-bit probe (A.3) | At the rate of `+MBAUDOT` (45.45 bit/s) | At 47.6 bit/s |
| The EDT lead (C.1) | 150 ms of mark | 300 ms of mark |
| Tm (5.2.12.1) | 2 s | 3 s |
| The carrier probes (5.2.12.2) | The carrier for 3 s | ANSam for 1 s, 75 ms of silence, then the carrier for 6 s |
| The start of the probes (5.2.12) | The first message at once | ANSam first |
| A calling tone (5.2.4.2, 5.2.10) | No effect: the probes after Ta | The probes at once, after the tone and 1 s of silence |

The tests are in `core/test/core_test.cpp`:

- `CoreText.LineEachWay/TtyCodeTable` (the code) and
  `CoreText.LineEachWay/V18Tty50` (a TTY at 50 bit/s);
- `CoreV18.FiveBitShifts` (LTRS, 72 characters, DEL),
  `CoreV18.EdtLead`, `CoreV18.Probes` (Ta, ANSam, 47.6 bit/s, Tm,
  Tc) and `CoreV18.CallingToneProbes`.
