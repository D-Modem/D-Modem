# V.42: technical details

This page gives the technical details for [V.42](v42.md). The clause
numbers are the clause numbers of V.42 (03/2002). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [the data link layer: technical
details](../link-details.md).

## The detection phase (7.2, 8.1)

| Item | Rule |
|---|---|
| ODP (originator) | DC1 with even parity, then DC1 with odd parity, with 8 to 16 ones after each. Again and again. |
| ADP (answerer) | "E", then "C" (LAPM), with 8 to 16 ones after each. 10 times or more. |
| T400 (9.1.1) | 750 ms. The time that a modem waits for the ODP or the ADP. |
| No reply | The modems use asynchronous characters (V.14), or MNP |

- A modem can start LAPM without the detection phase. Then the HDLC
  flags of the other modem show that it has V.42.

## The frames (8.1, 8.2)

| Field | Content |
|---|---|
| Flag | 01111110. Zero bits are inserted in the data, so that the flag does not occur. |
| Address | The DLCI (0 for data), the C/R bit and the EA bit |
| Control | I, S or U format. Sequence numbers modulo 128 (two octets) |
| Information | Up to N401 octets |
| FCS | 16 bits, or 32 bits if XID agrees (10 c) |

| Frame | Function |
|---|---|
| I | Data, with N(S) and N(R) |
| RR | Receive ready: an acknowledgement |
| RNR | Receive not ready: the receiver is busy |
| REJ | Reject: send again from N(R) |
| SREJ | Selective reject (optional) |
| SABME | Start the link in extended mode (modulo 128) |
| UA | Unnumbered acknowledgement |
| DM | Disconnected mode |
| DISC | Disconnect |
| FRMR | Frame reject |
| XID | Exchange identification: the parameters |
| UI | Unnumbered information: the break (BRK, BRKACK) |
| TEST | The loop-back test (optional) |

## The parameters (9)

| Parameter | Meaning | Default |
|---|---|---|
| T400 | Detection phase timer | 750 ms |
| T401 | Acknowledgement timer. It is not in XID. | Not specified (Appendix IV) |
| N400 | Maximum number of retransmissions | Not specified, 1 or more |
| N401 | Maximum octets in an information field, for each direction | 128 |
| k | Window size, for each direction | 15 |
| T402, T403 | Reply delay timer and inactivity timer | Optional |

- For N401 and k, the responder selects a value between the value of
  the initiator and the default, inclusive (9.2.3, 9.2.4). A value that
  is not in the XID is the default.

## The procedures (8)

1. **Establishment (8.3).** After XID, the originator sends SABME. The
   answerer replies with UA. All sequence variables are 0.
2. **Data (8.4).** Each I frame has N(S). The receiver acknowledges up to
   N(R). After a frame is missing, it sends REJ. If T401 expires, the
   sender polls (P = 1) and continues after the reply.
3. **Busy (8.4.7).** A receiver that cannot accept data sends RNR.
4. **Release (8.7).** DISC, then UA.
5. **Negotiation (8.10).** The initiator sends an XID command. The
   responder replies with an XID response. T401 and N400 apply.
6. **Break (8.13).** A BRK in a UI command has a sequence bit N(SB),
   the break-handling option and the length of the break. The receiver
   replies with BRKACK in a UI response, with N(RB) = V(RB) and F = P.
   A repeated BRK gets the BRKACK again, with no new break.

### The break-handling options (7.4, 7.5, 12.3.1)

| Option (D, S) | Action at the receiver |
|---|---|
| Destructive, expedited (1, 1) | Discard the data that is not yet delivered and not yet sent. Reset all state variables. Deliver the break. |
| Non-destructive, expedited (0, 1) | Deliver the break immediately |
| Non-destructive, in sequence (0, 0) | Deliver the break in sequence with the data |

- A destructive break also starts the data compression again (C-INIT,
  5.2/V.42 bis, 7.5/V.44).

## V.42 in D-Modem

| Item | D-Modem (`link/lapm.cpp`, `link/hdlc.cpp`) |
|---|---|
| T400 | 750 ms. After V.8, the originator waits 3 × T400. |
| T401 | 2 s at the data pump rate, plus the time of three frames |
| N400 | 5 |
| N401, k | 128 octets and 15 frames, or less if XID agrees on less |
| FCS | 16 bits or 32 bits |
| SREJ, TEST, T402, T403 | Not used. We do not ask for SREJ or TEST in XID. |
| Break, received | BRKACK. The link logs the break, because the DTE interface has no break signal. A destructive break discards the data, resets the sequence variables and starts the compression again. |
| Break, sent | Never: the DTE interface has no break signal |
| Re-establishment | V.42 bis or V.44 starts again (C-INIT) |
| MNP (Annex A of the 1996 edition) | `link/mnp.cpp` |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| Break (8.13) | A BRK went to the USRobotics feature code. It got no BRKACK. The other modem sent the BRK again until N400. | BRKACK, with the sequence bit. A destructive break resets the link and the compression. |
| C-INIT on re-establishment (5.2/V.42 bis, 7.5/V.44) | After a second SABME in the data phase, the compression continued with its old dictionary. The other modem started a new one, so the data was wrong. | The compression starts again |
