# T.30: technical details

This page gives the phases, the signals, the frames, the DIS bits that
D-Modem uses, the timers and the error correction mode of T.30
(09/2005). For a summary, refer to [T.30](t30.md).

## The phases of a call (2, 5)

| Phase | Content |
|---|---|
| A | The call set-up. The caller sends CNG. The answerer sends CED. |
| B | The pre-message procedure: identification (NSF, CSI, DIS; TSI, DCS), the training (TCF) and the answer to it (CFR or FTT) |
| C | The message: the pages (T.4), or the ECM blocks |
| D | The post-message procedure: MPS, EOM or EOP, and the answer (MCF, RTP, RTN, PIP or PIN). With ECM: PPS and EOR, and the answer (MCF, PPR, RNR, ERR) |
| E | The release of the call: DCN |

## The tones of phase A (4)

| Signal | Value |
|---|---|
| CNG | 1100 Hz, 0.5 s on, 3 s off |
| CED | 2100 Hz for 2.6 s to 4.0 s. The answerer waits 1.8 s to 2.5 s (at least 0.2 s) after the connection, before CED. After CED, 75 ms ± 20 ms of silence. |

## The control messages (5.3)

- A message is one or more HDLC frames on V.21 channel 2 (300 bit/s).
- Each frame has the address 0xFF, the control field 0x03 (0x13 on the
  last frame of the message), the FCF, the FIF and a 16-bit FCS.
- The preamble is flags for 1 s ± 15 % before the first frame.
- The gap between a received message and the answer is 75 ms ± 20 ms.
- A frame that is longer than 3.45 s (± 15 %) is not valid.
- The bits go LSB first. In the FIF, bit 1 of T.30 is the first bit
  on the line (the LSB of the first octet).

## The training check, TCF (5.3.6)

- TCF is zeros for 1.5 s ± 10 % on the modulation and the rate that
  the DCS selects.
- The receiver decides if the zeros are good enough. If yes, it sends
  CFR. If no, it sends FTT, and the sender selects a lower rate.

## The DIS, DTC and DCS bits that D-Modem uses (Table 2)

| Bits | Meaning |
|---|---|
| 6 | V.8 capabilities (V.34 fax) |
| 9 | A document to poll (DIS) |
| 10 | Receives (DIS); receive mode (DCS) |
| 11–14 | The modulation and the rate: V.27 ter, V.29, V.17 |
| 15 | Fine resolution (7.7 lines/mm) |
| 16 | MR coding |
| 17, 18 | The recording width. 0 0 is A4 (1728 pels). |
| 19, 20 | The recording length. 0 0 is A4, 1 0 is A4 and B4, 0 1 is unlimited. |
| 21–23 | The minimum scan line time. 1 1 1 is 0 ms. |
| 24 | Extend: a fourth octet follows (and each eighth bit after it) |
| 27 | ECM |
| 28 | The ECM frame size: 0 is 256 octets, 1 is 64 octets |
| 36, 37 | T.43 and plane interleave |
| 53 | Binary file transfer (T.434) |
| 68, 69, 73 | JPEG (T.42), full colour, no subsampling |
| 78, 79 | T.85 and the L0 option |
| 80, 88, 92–95 | T.44 (MRC) |

## The timers (5.4.3, Annex A)

| Timer | Value | Use |
|---|---|---|
| T1 | 35 s ± 5 s | The time to find the other fax machine |
| T2 | 6 s ± 1 s | The time to wait for a command |
| T3 | 10 s ± 5 s | The time to alert the operator |
| T4 | 3 s ± 15 % | The time to wait for a response |
| T5 | 60 s ± 5 s | ECM: the time from the first RNR. If the receiver is still busy at the end of T5, the sender disconnects. |

A command without a response is sent again. After three tries, the
machine disconnects (5.4.2).

## The error correction mode (Annex A)

- The page goes in blocks of up to 256 frames. Each frame (FCD) has a
  frame number and 64 or 256 octets of page data.
- RCP frames end the block on the fast modulation.
- Then the sender sends PPS on V.21 with the page and block counts and
  the number of frames.
- The receiver sends MCF if all frames are good. Else it sends PPR
  with a map of the frames to send again.
- After four PPRs for the same block, the sender sends CTC (continue
  to correct, perhaps at a lower rate) or EOR (end of retransmission).
- RNR tells the sender that the receiver is busy. The sender sends RR
  until the receiver sends MCF or PPR, or until T5 ends.

## T.30 in D-Modem

| Item | D-Modem (`dsp/t30.cpp`) |
|---|---|
| Roles | Sender and receiver, caller and answerer, polling (DTC) |
| Tones | CNG 0.5 s on, 3 s off. CED after 200 ms of silence, for 3 s, then 75 ms of silence. |
| Preamble | 296 bits of flags (0.99 s) before a message on V.21 |
| Frame codes | All the FCFs agree with T.30 (checked code by code) |
| TCF | 1.5 s of zeros. The receiver sends CFR for a run of zeros of 0.75 s or more. |
| Modulations | V.27 ter, V.29 and V.17. V.34 half-duplex with V.8. |
| Coding | MH and MR; with ECM, also MMR, T.85, T.42, T.43, T.44 and T.434 |
| Recording length | The DIS offers an unlimited length. The DCS selects the longest length in the DIS. |
| Timers | T1 35 s, T2 6 s, T4 3 s, T5 60 s. Three tries for a command. |
| ECM | 256-frame blocks, 64 or 256 octets, PPR, CTC after four PPRs, RNR and RR |
| T.38 | The same procedure on IFP packets |
| Not implemented | Password, sub-address and selective polling (SUB, SEP, PWD), the T3 operator alert, the limit of 3.45 s on a frame |

The tests `T30*` (`dsp/test/fax_test.cpp` and
`dsp/test/t30_v34_test.cpp`) send fax calls between two D-Modem fax
machines, with the different modes and options. The tests `T30Spandsp`
(`dsp/test/t30_spandsp.cpp`) send fax calls between D-Modem and
spandsp. The tests in `dsp/test/t38_test.cpp` send fax calls on T.38.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| The DIS and the DCS set bit 19 only (A4 and B4). D-Modem sends any length of page. | The DIS sets bit 20 only (unlimited length). The DCS selects bit 20, else bit 19, else A4, from the bits of the DIS. |
| The sender counted RNR messages (60) to end a busy receiver. With the gaps and the T4 waits, this took more than 3 minutes. | T5 is a timer of 60 s from the first RNR. |

The tests `T30.RecordingLengthBits` and `T30.T5EndsTheBusyReceiver`
(`dsp/test/fax_test.cpp`) check these corrections.
