# V.120: technical details

This page gives the technical details for [V.120](v120.md). The clause
numbers are the clause numbers of V.120 (10/96). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on
[ISDN: technical details](../isdn-details.md#atb29-v120).

## The terminal adaption header (3.1.1)

| Bit | Name | Function |
|---|---|---|
| 8 | E | 1: the header has one octet. 0: a CS octet follows. |
| 7 | BR | Asynchronous mode: 1 is a break. HDLC mode: 1 is an idle line (all ones). |
| 6, 5 | – | Reserved. Set to 0. |
| 4, 3 | C2, C1 | Errors (Table 1). Asynchronous mode: a stop bit error, a parity error, or both. HDLC mode: an FCS error, an abort, or an overrun. 0, 0: no error. |
| 2 | B | 1: the first segment of a frame |
| 1 | F | 1: the last segment of a frame |

A frame that is not segmented has B 1 and F 1. Thus, the usual header
is `83h`.

## The control state octet (3.1.2)

| Bit | Name | Function |
|---|---|---|
| 8 | E | 1: the last octet of the header |
| 7 | DR | Data ready (circuit 108 or 107) |
| 6 | SR | Send ready (circuit 105 or 106) |
| 5 | RR | Receive ready (circuit 109) |
| 4 to 1 | – | Reserved. Set to 0. |

The CS octet is optional. The low layer compatibility of the call tells
if it is used (octet 5b, bit 7).

## The modes (3.2.1)

| Mode | Function |
|---|---|
| Asynchronous | The TA removes the start and stop bits. The characters go in the data field. A break sets BR. |
| Synchronous HDLC | The address, control and information of each HDLC frame go in one or more terminal adaption frames. The TA removes the flags and the FCS. |
| Bit transparent | The bits go with no change. This mode is for further study. |

The data field has at most N2120 octets. N2120 is N201 of Q.922 minus
the length of the header (3.2.2).

## The address (6.2.2)

| Octet | Bits 8 to 3 | Bit 2 | Bit 1 |
|---|---|---|---|
| 1 | LLI0: the 6 high bits of the LLI | C/R | EA0 = 0 |
| 2 | Bits 8 to 2: LLI1, the 7 low bits of the LLI | | EA1 = 1 |

| LLI | Function (Table 3) |
|---|---|
| 0 | In-channel signalling |
| 1 to 255 | Reserved |
| 256 | The default LLI |
| 257 to 2047 | Assigned to logical links |
| 2048 to 8190 | Reserved |
| 8191 | In-channel layer management |

The C/R bit is the same for the two directions (Table 4). A command
has C/R 0. A response has C/R 1. Thus, for LLI 256, a command has the
address `08h 01h`, and a response has `0Ah 01h`.

## The data link (4, 6.2)

- The link uses the procedures of Q.922 (core and control). On a
  circuit mode B-channel, the address is the two-octet address of
  6.2.2.
- The multiple frame mode starts with SABME and UA. It uses sequence
  numbers modulo 128 in two control octets.
- The unacknowledged mode uses UI frames. XID can verify the link
  (4.2.2).
- The timers and counters (T200, N200, N201, k) are as in Q.922.

## Logical links (6.3)

- An LLI can be set up when the call is set up, in the SETUP message,
  or later on the active B-channel.
- The default LLI 256 needs no signalling.

## V.120 in D-Modem

| Item | D-Modem (`link/lapb.cpp`) |
|---|---|
| Logical links | Only LLI 256, one link |
| Address | Commands `08h 01h` (C/R 0) and responses `0Ah 01h` (C/R 1), from the two ends |
| Mode | Asynchronous mode, multiple frame mode (SABME, modulo 128) |
| Header | `83h` in each I frame. A received header with E 0 has its CS octets removed. |
| BR, C1, C2, CS | Not used. A received break or error is ignored. |
| Segments | Not used: each frame is complete. |
| N201 | 257 octets sent: the header and 256 octets of data. We accept up to 2048 octets. |
| k | 7 |
| T200, N200 | As T1 and N2 of [X.75](x75-details.md#x75-in-d-modem) |
| FRMR | Not sent. A frame with an error is discarded. |
| UI frames, XID, the synchronous and bit transparent modes, the low layer compatibility | Not done |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The C/R bit (6.2.2.3, Table 4) | As Q.921: the commands of the caller had C/R 0, and the commands of the answerer had C/R 1 | C/R 0 for each command, and C/R 1 for each response, from the two ends. A D-Modem from before this change and a new one cannot use V.120 together. |
