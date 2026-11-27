# V.8 bis: technical details

This page gives the technical details for [V.8 bis](v8bis.md). The
clause numbers are the clause numbers of V.8 bis (11/2000). This page is
our own description, not a copy of the Recommendation. The revision
number of this edition is 2.

## Terms (3)

| Term | Meaning |
|---|---|
| Call station, answer station | The station that dialled and the station that answered. |
| Initiating station | The station that starts a transaction. It can be the call station or the answer station. |
| Responding station | The other station. |
| Transaction | A sequence of signals and messages. It starts with a signal and ends with ACK(1), NAK or a time-out. |
| Telephony mode | Voice or other audio on the line. |
| "e" and "d" | The subscripts of the signals. "e" signals come from an automatic answer station at the start of a call. "d" signals come during a call. |

## The signals (6, 7.1)

Each signal has two segments:

1. **Segment 1**, 400 ms: two tones at the same time. The initiating
   station uses one pair of tones. The responding station uses a
   different pair.
2. **Segment 2**, 100 ms: one tone. The tone identifies the signal.

| Signal | Sent by | Segment 1 (Hz) | Segment 2 (Hz) | Meaning |
|---|---|---|---|---|
| MRe | Initiating (automatic answer) | 1375 + 2002 | 650 | Mode request: send MS |
| MRd | Initiating, or the call station after MRe | 1375 + 2002 (responding: 1529 + 2225) | 1150 | Mode request: send MS |
| CRe | Initiating (automatic answer) | 1375 + 2002 | 400 | Capabilities request: send CL |
| CRd | Initiating, or the call station after CRe or MRe | 1375 + 2002 (responding: 1529 + 2225) | 1900 | Capabilities request: send CL |
| ESi | Initiating | 1375 + 2002 | 980 | A message follows |
| ESr | Responding | 1529 + 2225 | 1650 | A message follows |

- The frequency tolerance is ± 250 ppm. The time tolerance is ± 2%.
- Segment 1 of MRe and CRe can be 285 ms, for older modems. On a line
  with echo suppressors, keep 400 ms.
- MRe and CRe are 12 dB to 15 dB below the usual permitted power. Thus,
  they are quiet for a person who answers. The other signals use the
  highest permitted power.
- The segment 2 of ES is the 100 ms preamble of the message that
  follows. The two frequencies of ES segment 2 are the mark frequencies
  of V.21.

## The messages (7.2)

The initiating station sends on V.21(L). The responding station sends on
V.21(H). The frequency tolerance is ± 0.01%.

Each message has this structure:

1. 100 ms of mark (ONE bits).
2. Two to five HDLC flags (`01111110`).
3. The information field, 64 octets or fewer (9.10).
4. A 16-bit FCS: the CRC of ISO/IEC 3309 (x<sup>16</sup> + x<sup>12</sup> + x<sup>5</sup> + 1).
5. One to three flags.

The transmitter inserts a ZERO after five ONE bits in a sequence. The
receiver removes it. A frame is invalid if one of these conditions is
true:

- The flags are not correct.
- The frame has fewer than three octets.
- The number of bits is not a whole number of octets.
- The FCS is not correct.

## The information field (8)

The information field has three parts:

1. **Identification field (I).** The first octet has the message type
   (bits 1 to 4) and the revision number (bits 5 to 8). Then a parameter
   tree follows.
2. **Standard information field (S).** A parameter tree of modes and
   capabilities.
3. **Non-standard information field (NS).** Optional data of a
   manufacturer.

| Message | Type (bits 4 to 1) |
|---|---|
| MS | `0001` |
| CL | `0010` |
| CLR | `0011` |
| ACK(1) | `0100` |
| ACK(2) | `0101` |
| NAK(1) | `1000` |
| NAK(2) | `1001` |
| NAK(3) | `1010` |
| NAK(4) | `1011` |

ACK and NAK have only the first octet. A receiver ignores a type that it
does not know.

### The parameter tree (8.2)

The parameters are bits in a tree with three levels. An NPar is a
parameter without sub-parameters. An SPar is a parameter with
sub-parameters. A ONE bit means that the parameter is valid.

The octets go in this sequence:

1. The level 1 NPars.
2. The level 1 SPars.
3. For each level 1 SPar that is ONE, a level 2 group. The group has
   the level 2 NPars, the level 2 SPars, and then the level 3 NPars of
   each level 2 SPar that is ONE.

A delimiter bit ends each block of octets. Bit 8 ends the blocks of level
1 and each level 2 group. Bit 7 ends the blocks inside a level 2 group.
ZERO means "more octets", and ONE means "the last octet". If a level 2
group has only NPars, bits 7 and 8 are both ONE in its last octet. A
receiver must parse all blocks and ignore the bits that it does not know.

### The identification field parameters (8.3.3)

| Level 1 NPar bit | Meaning |
|---|---|
| 1 | V.8 start-up (9.9.1) |
| 2 | Short V.8 start-up (9.9.2) |
| 3 | More information is available (9.10) |
| 4 | Send ACK(1) after MS (9.7) |
| 7 | A non-standard field follows |

The level 1 SPar "network type" has the level 2 NPars cellular access,
ISDN access and digital PSTN access. If this SPar is ZERO, the station is
on an analogue line.

### The standard information field (8.4)

The level 1 SPars of the S field are the types of call: data,
simultaneous voice and data, H.324, V.18, T.30, telephony and T.101.
Under each type, level 2 and level 3 give the details. For a data call,
the details are the modulations (V.21 to V.92), the protocols (V.42,
V.42 bis and others) and other items.

In CL and CLR, a station sets all of its capabilities. In MS, a station
sets one mode, or more than one only if it can use them at the same
time. An MS has only the octets that are necessary for the selected
mode.

## The transactions (9.1)

| Number | Sequence | When |
|---|---|---|
| 1 | MR, MS, ACK/NAK | Any time. The initiator does not know if the far station has V.8 bis. |
| 2 | CR, CL, MS, ACK/NAK | As 1 |
| 3 | CR, CLR, CL-MS, ACK/NAK | As 1 |
| 4 | ESi + MS, ACK/NAK | The initiator knows that the far station has V.8 bis |
| 5 | ESi + CL, MS, ACK/NAK | As 4 |
| 6 | ESi + CLR, CL, MS, ACK/NAK | As 4 |
| 7 to 13 | MRe or CRe, then MRd or CRd from the call station, then as 1 to 3 | Only at the start of an automatic answer. The call station takes control. |

"CL-MS" is a CL and then an MS, with no silence between them. After
MRe or CRe, the call station puts ESr before its message (9.4).

### ACK and NAK (6.7, 6.8, 9.5, 9.7)

| Message | Meaning |
|---|---|
| ACK(1) | MS received. This ends the transaction. |
| ACK(2) | Send the more information that you have. |
| NAK(1) | I cannot interpret the message. |
| NAK(2) | I cannot use the mode now. |
| NAK(3) | I do not have the mode, or it is disabled. |
| NAK(4) | Send the message again (for further study). |

If the "send ACK(1)" bit of MS is ZERO, the station that accepts the MS
does not send ACK(1). It sends ANS or ANSam immediately.

### Errors and time-outs (9.8)

- If a station receives an invalid frame, it sends NAK(1). Then it goes
  back to the initial state.
- A station can stay in a state for 5 s at most. After 5 s, it goes back
  to the initial state. This rule does not apply to telephony mode and
  to the selected mode.

## The start of the modem (9.9)

The station that receives MS becomes the answer modem. The ID field of MS
selects one of three start-ups:

| ID field bits | Start-up | What occurs |
|---|---|---|
| V.8 = 1 | V.8 | ACK, ANSam, then the full V.8 (CM, JM, CJ). |
| Short V.8 = 1 | Short V.8 | ACK, ANSam for the time Te, then JM with the one mode of MS. The other station sends no CM. It replies with CJ. |
| Both 0 | V.25 | ACK, then ANS of V.25 and the start-up of the modulation. |

The parameters of the CM and JM of V.8 have priority over the parameters
of MS.

## Procedures (10)

### At an automatic answer (10.2)

1. The answer station waits at least 400 ms. Then it sends MRe or CRe.
2. The answer station can play a recorded message (OGM) after the
   signal. When it detects ESr, it stops the OGM.
3. If it detects no V.8 bis signal in 3 s, it can send the signal again
   or disconnect.
4. If it detects a calling tone (CNG, CT or CI), it can send ANS or
   ANSam and use the usual start-up.

The call station listens for MRe, MRd, CRe, CRd, ESi, ANS and ANSam. If
it detects ANS or ANSam first, it continues with V.25 or V.8.

### Echo suppressors (Appendix III)

On a line with echo suppressors, a station can add a silence of 1.5 s
between ESi (or the OGM) and the message.

## V.8 bis in D-Modem

| Item | D-Modem |
|---|---|
| Signals | `SigTx` and `SigRx` in `dsp/v8bis.cpp`. Segment 1 is 400 ms. Segment 2 is 100 ms. ES has no segment 2 because the message preamble follows. |
| Levels | MRe and CRe at about -22 dBm0. The messages and the "d" signals at about -14.5 dBm0 (`AMP_E`, `AMP_MSG` in `dsp/v8bis.h`). |
| Messages | 30 bits (100 ms) of mark, 3 flags at the start, 2 flags at the end (`MsgTx`). |
| Answer station | Silence of 400 ms, MRe, then 3 s for a reply (`dsp/dp_v8bis.cpp`). |
| Transactions | Transaction 10: the answer station sends MRe, and the call station replies with CRd. If the call station replies with MRd (transaction 7), D-Modem uses V.8 without V.8 bis. |
| Time-outs | 5 s in each state. |

For the full description, refer to
[V.8bis: technical details](../dsp/v8bis-details.md).

### Correction (2026-11)

A check against 6.8 and 9.8 found one error. If a frame had a good FCS
but its parameter tree could not be parsed, D-Modem ignored the frame.
Now D-Modem sends NAK(1) and continues with V.8, as for an invalid
frame. A frame with a message type that D-Modem does not know is still
ignored (8.3.1).
