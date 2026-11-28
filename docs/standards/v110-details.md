# V.110: technical details

This page gives the technical details for [V.110](v110.md). The clause
numbers are the clause numbers of V.110 (02/2000). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on
[ISDN: technical details](../isdn-details.md#atcbst-v110-rate-adaption).

## The three steps (5.1.1)

| Step | Function |
|---|---|
| RA0 | An asynchronous user rate to the next rate of 2^n × 600 bit/s. Only for asynchronous terminals (5.3.3). |
| RA1 | The synchronous user rate to an intermediate rate of 8, 16 or 32 kbit/s, in 80-bit frames (5.1.2). 64 kbit/s is the intermediate rate for 24000, 28800 and 38400 bit/s. |
| RA2 | The intermediate rate to 64 kbit/s (5.1.4, I.460). The rate fills the first 1, 2, 4 or 8 bits of each octet. The other bits are ones. |

## The frame (5.1.2.1, Table 2)

| Octet | Bit 1 | Bits 2 to 7 | Bit 8 |
|---|---|---|---|
| 0 | 0 | 0 | 0 |
| 1 | 1 | D1 to D6 | S1 |
| 2 | 1 | D7 to D12 | X |
| 3 | 1 | D13 to D18 | S3 |
| 4 | 1 | D19 to D24 | S4 |
| 5 | 1 | E1 to E6 | E7 |
| 6 | 1 | D25 to D30 | S6 |
| 7 | 1 | D31 to D36 | X |
| 8 | 1 | D37 to D42 | S8 |
| 9 | 1 | D43 to D48 | S9 |

The bits go from left to right, and from octet 0 to octet 9. The
alignment pattern is the 8 zeros of octet 0 and the first bit (1) of
each other octet: 17 bits.

## Status bits (5.1.2.3)

- A status bit is ON when it is 0, and OFF when it is 1.
- The S bits carry the state of the terminal (circuit 108 to 107, and
  105 to 109).
- The X bits carry the frame synchronisation, and the flow control of
  5.4.2 when it is used.

## The E bits (5.1.2.4, Table 5)

| User rate (bit/s) at 8, 16, 32 and 64 kbit/s | E1 | E2 | E3 |
|---|---|---|---|
| 600 | 1 | 0 | 0 |
| 1200 | 0 | 1 | 0 |
| 2400 | 1 | 1 | 0 |
| –, –, 12000, 24000 | 0 | 0 | 1 |
| –, 7200, 14400, 28800 | 1 | 0 | 1 |
| 4800, 9600, 19200, 38400 | 0 | 1 | 1 |

- E4 to E6 carry the clock phase of clause 8. When they are not used,
  they are ones.
- E7 is 1. At 600 bit/s, E7 is 0 in each fourth frame. This gives the
  4 × 80 bit multiframe of X.30 (Note 2).
- The E bits give only synchronous rates. An asynchronous rate needs
  signalling in the D-channel, or the exchange of Appendix I (Note 4).

## The data bits (5.1.2.6, 5.1.2.7, Tables 6a to 6f)

- 4800, 9600, 19200 and 38400 bit/s use all 48 D bits.
- 600, 1200 and 2400 bit/s repeat each bit 8, 4 or 2 times.
- 7200, 14400 and 28800 bit/s use 36 D bits. 12000 and 24000 bit/s use
  30 D bits. The other D bits are fill (1).

## Frame synchronisation (5.1.3)

1. The receiver looks for the 17-bit pattern. Two good patterns in
   sequence are the recommended minimum.
2. It checks that S and X are OFF before the data phase.
3. It monitors the pattern continuously. Three frames in sequence with
   an error in the pattern are a loss of synchronisation.

## 48000 and 56000 bit/s (5.2, Tables 7a to 7c)

| Rate | Frame |
|---|---|
| 48000 | 4 octets: bit 1 is the alignment (1, 0, 1, 1), bits 2 to 7 are D bits, bit 8 is S or X |
| 56000 | 7 D bits in bits 1 to 7 of each octet, and bit 8 is 1 (Table 7b). Table 7c is another frame with status bits. |

## Asynchronous terminals (5.3)

| Item | Rule |
|---|---|
| Rates (Table 8) | 50 to 38400 bit/s. 50 to 600 bit/s go on 600 bit/s. 3600 goes on 4800. 7200 goes on 9600. 12000 and 14400 go on 19200. 24000 and 28800 go on 38400. |
| RA0 (5.3.3) | The TA adds stop elements to the characters, so that they fill the synchronous rate. |
| Overspeed (5.3.4) | The TA can delete one stop element in each 8 characters at 1 % overspeed. The TA at the other end puts them back. |
| Break (5.3.5) | M to 2M + 3 bits of start polarity are sent as 2M + 3 bits. M is the bits of a character. |
| Parity (5.3.6) | Parity bits are data bits for RA0. |

## The data phase (7.1)

1. **Connect.** The TA sends frames with S and X OFF and the D bits 1.
   When it has synchronisation, it sets X ON. When the other end is
   ready, it sets S ON too (Figure 3).
2. **Data transfer.** S and X are ON in the two directions (7.1.3).
3. **Disconnect (7.1.4).** The TA that ends the call sets S OFF, keeps
   X ON, and sets the D bits to 0. The other TA sees this, and does the
   same. A TA that sees S OFF, or loses synchronisation, after its own
   request goes to the disconnected mode. Then the D-channel clears the
   call. Timer T2 (5 s) limits the wait.
4. **Loss of synchronisation (7.1.5).** The TA sets X OFF and looks for
   the frames again. When it finds them, it sets X ON. After 3 s with
   no frames, it sends at least three frames with all status bits OFF
   and the D bits 0, and disconnects.

## V.110 in D-Modem

| Item | D-Modem (`dsp/v110.cpp`, `dsp/dp_clear.cpp`) |
|---|---|
| Rates | 300 to 38400 bit/s on 80-bit frames, and 56000 bit/s (Table 7b) |
| 48000 bit/s (Table 7a) | Not done |
| 56000 bit/s with status bits (Table 7c) | Not done |
| E4 to E6 (clause 8) | Not used: ones |
| Synchronisation | Three good patterns in sequence. Three bad patterns in sequence are a loss. |
| 300 bit/s | On the frames of 600 bit/s, each bit twice |
| RA0 | Asynchronous 14400 and 28800 bit/s go on the frames of 19200 and 38400 bit/s. The modem adds stop elements, so that the characters come at the user rate. The other rates of `AT+CBST` need no RA0. |
| An answerer with `AT+CBST=0` | It finds the rate from E1 to E3. A caller with asynchronous 14400 or 28800 bit/s is found as 19200 or 38400 bit/s (Note 4). |
| Disconnect | As 7.1.4. The wait for the answer of the other end is 1 s, not 5 s. |
| Overspeed, break, flow control (5.3.4, 5.3.5, 5.4) | Not done. The characters come from a program, not from a V.24 circuit. |
| Half duplex (7.2), automatic calling (7.3) | Not done |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| E7 at 600 bit/s (Table 5, Note 2) | 1 in all frames | 0 in each fourth frame |
| Asynchronous 14400 and 28800 bit/s (5.3.3, Table 8) | On the frames of synchronous 14400 and 28800 bit/s (E1 to E3: 1, 0, 1) | RA0: on the frames of 19200 and 38400 bit/s (0, 1, 1), with added stop elements. `CONNECT` gives the user rate. |
| Disconnect (7.1.4) | The frames went on as before until the line stopped. The other end did not know that the call ended. | S OFF, X ON and the D bits 0, in the two directions. The two ends give `NO CARRIER`. |
| Loss of synchronisation (7.1.5) | S and X OFF, and no end to the call | X OFF and S ON. After 3 s, all status bits OFF and the D bits 0, and the call ends. |
