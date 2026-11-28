# X.75: technical details

This page gives the technical details for [X.75](x75.md). The clause
numbers are the clause numbers of X.75 (10/96). Only the single link
procedure (SLP) is on this page. This page is our own description, not a
copy of the Recommendation. The details of the D-Modem implementation
are on [ISDN: technical details](../isdn-details.md#x75).

## The frames (2.2, 2.3)

| Field | Content |
|---|---|
| Flag | 01111110. Zero bits are inserted in the data, so that the flag does not occur. |
| Address | A (`03h`) or B (`01h`) for a single link. C and D are for multilink operation. |
| Control | I, S or U format. One octet in basic mode (modulo 8). |
| Information | Up to N1 bits, in I frames and in FRMR |
| FCS | 16 bits |

A frame with fewer than 32 bits, a bad FCS or a different address is
invalid. The receiver discards it and does nothing more (2.3.5.3).

| Frame | Type | Control (P/F 0) | Function |
|---|---|---|---|
| I | Command | `N(R) P N(S) 0` | Data |
| RR | Command or response | `N(R)` and `01h` | Receive ready: an acknowledgement |
| RNR | Command or response | `N(R)` and `05h` | Receive not ready: the receiver is busy |
| REJ | Command or response | `N(R)` and `09h` | Reject: send again from N(R) |
| SABM | Command | `2Fh` | Start or reset the link in basic mode |
| DISC | Command | `43h` | End the link |
| UA | Response | `63h` | Unnumbered acknowledgement |
| DM | Response | `0Fh` | Disconnected mode |
| FRMR | Response | `87h` | Frame reject, with 3 octets of information |

The P/F bit is bit 5 (`10h`). A command with P 1 gets a response with F
1 (2.4.3).

## Addresses (2.4.2)

Commands go with the address of the remote end. Responses go with the
address of the local end. The two ends agree which end is A and which
end is B.

## The phases (2.4.4)

1. **Link set-up.** One end sends SABM and starts T1. The other end
   sends UA and sets V(S) and V(R) to 0. An end that cannot start sends
   DM. After N2 SABMs with no answer, the end tells the higher layer.
2. **Information transfer.** The ends send I frames and S frames. A SABM
   in this phase resets the link (2.4.8).
3. **Disconnection.** One end sends DISC and starts T1. The other end
   sends UA. A UA or a DM ends the link. After N2 DISCs, the higher layer
   recovers.
4. **Disconnected phase.** The end answers a SABM as in step 1. It
   answers a DISC with DM. It answers each other command with P 1 with
   DM F 1. It ignores the other frames.

### Commands that cross (2.4.4.5)

| Sent | Received | Action |
|---|---|---|
| SABM | SABM | Send UA. Go to the data phase when the UA of the other end comes. |
| DISC | DISC | Send UA. Go to the disconnected phase when the UA comes. |
| SABM | DISC | Send DM. Go to the disconnected phase. |
| DISC | SABM | Send DM. Go to the disconnected phase. |

## Information transfer (2.4.5)

- An I frame has N(S) = V(S) and N(R) = V(R). V(S) then increases by 1.
  T1 starts if it does not run.
- The sender stops when V(S) is the last N(R) received plus k.
- An I frame with N(S) = V(R) is accepted. V(R) increases by 1. An RR,
  or an I frame of the receiver, acknowledges it.
- An I frame with a different N(S) is discarded. The receiver sends one
  REJ with N(R) = V(R). The REJ condition ends when the expected frame
  comes.
- An N(R) acknowledges all I frames up to N(R) − 1. T1 stops when an N(R)
  acknowledges new frames. T1 starts again if frames are still not
  acknowledged.
- A REJ sets V(S) to its N(R). The sender sends the frames again from
  there.
- An RNR stops the I frames until RR, REJ or a link reset.

### The busy condition (2.4.5.8)

A busy end sends RNR. It can discard the I frames that come, but it
uses their N(R) and their P bit. To end the busy condition, it sends REJ
if it discarded I frames, and RR if it did not.

### Timer recovery (2.4.5.9)

1. T1 expires. The end adds 1 to its attempt count. It sends the last
   I frame again with P 1, or an RR or RNR command with P 1.
2. A response with F 1 and a valid N(R) ends timer recovery. The end
   sends again from that N(R).
3. When the attempt count is N2, the end resets the link (2.4.8.2).

## Frame reject (2.3.4.10, 2.3.5.4, 2.4.8.3)

An end sends FRMR for a frame that has a correct FCS, but has one of
these errors:

| Condition | W | X | Y | Z |
|---|---|---|---|---|
| 1. The control field is not defined or not implemented | 1 | 0 | 0 | 0 |
| 2. The information field of an I frame is longer than N1 | 0 | 0 | 1 | 0 |
| 3. The N(R) is not valid | 0 | 0 | 0 | 1 |
| 4. An information field that the frame cannot have, or an S or U frame of the wrong length | 1 | 1 | 0 | 0 |
| 5. An S frame with F 1, not in timer recovery and not as a reply to a poll | 1 | 0 | 0 | 0 |
| 6. A UA or a DM that the end did not expect | 1 | 0 | 0 | 0 |
| 7. An N(S) that is not valid: V(R) is the last N(R) sent plus k | 1 | 0 | 0 | 0 |

A valid N(R) is from the oldest N(S) that is not acknowledged to V(S),
or to x in timer recovery.

The information field of FRMR (Table 12, modulo 8) has 3 octets:

| Octet | Bits |
|---|---|
| 1 | The control field of the rejected frame |
| 2 | Bit 1: 0. Bits 2 to 4: V(S). Bit 5: C/R (1 if the rejected frame was a response). Bits 6 to 8: V(R). |
| 3 | Bit 1: W. Bit 2: X. Bit 3: Y. Bit 4: Z. Bits 5 to 8: 0. |

After FRMR, the end is in the frame rejection condition:

- It does not send I frames or S frames.
- It sends the same FRMR again for each frame that comes, except a SABM,
  a DISC or an FRMR. It uses F 1 for a command with P 1.
- It can start T1. When T1 expires, it sends the FRMR again. After N2
  attempts, it resets the link itself.
- A SABM or a DISC, received or sent, ends the condition.

An end that receives FRMR resets the link with SABM (2.4.7.2).

## Link reset (2.4.8)

1. The end sends SABM and starts T1.
2. A UA sets V(S) and V(R) to 0. The link stays in the data phase.
3. A DM ends the link.
4. After N2 SABMs with no answer, the end goes to the disconnected
   phase.

## The parameters (2.4.9)

| Parameter | Function |
|---|---|
| T1 | The time to wait for an answer. It is longer than the time of a frame and its answer. |
| T2 | The longest time before an answer is sent. T2 is less than T1. |
| T3 | The time of an idle channel before the end tells the higher layer. T3 is more than T1. |
| N2 | The maximum number of attempts after T1 |
| N1 | The maximum number of bits in an I frame |
| k | The maximum number of I frames not acknowledged: 7 in basic mode, 127 in extended mode |

## X.75 in D-Modem

| Item | D-Modem (`link/lapb.cpp`) |
|---|---|
| Mode | Basic mode (modulo 8, SABM) only. No extended mode, super mode or multilink procedure. |
| Addresses | The calling end sends commands to B (`01h`). The called end sends commands to A (`03h`), as isdn4linux does. |
| Link set-up | The calling end sends SABM. The called end sends its own SABM if no SABM comes in 4 × T1. |
| k | 7 |
| N1 | 256 octets in our I frames. We accept up to 2048 octets. |
| T1 | 1 s at the line rate, plus the time of our longest frame and two of the longest frames of the remote end. T1 is counted in transmitted bits. |
| N2 | 4 |
| Timer recovery | An RR or RNR command with P 1 |
| SREJ, XID | No SREJ. XID only for the encryption of ZyXEL (`S102`). |
| FRMR conditions 5 and 6 | Not sent. An S frame with an unexpected F 1 is accepted. A UA in the data phase is ignored. A DM in the data phase ends the link. Other equipment sends these frames after a lost frame, so an FRMR would reset good links. |
| DM with F 0 to our SABM | Ignored. The SABM is sent again (X.25 uses this DM to ask for a SABM). |
| T3 | Not used. The call ends when the B-channel ends. |
| The last UA or DM | When the remote end ends the link, the line stays on for 40 ms, so that the response goes out. |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| Frame reject (2.3.4.10, 2.4.8.3) | A frame with an N(R) that is not valid, an unknown control field, a wrong length or too much information was discarded. The two ends did not know about the error. | FRMR with W, X, Y and Z, and the frame rejection condition until the remote end resets the link. After N2 × T1, the modem resets the link itself. |
| N2 attempts in the data phase (2.4.5.9) | DISC, and the call ended | A link reset with SABM |
| SABM and DISC that cross (2.4.4.5.2) | The received command was accepted with UA | DM, and the disconnected phase |
| Two SABMs that cross (2.4.4.5.1) | The data phase started at the received SABM | UA, and the data phase starts at the UA of the remote end |
| The end of the busy condition (2.4.5.8) | RR, also when I frames were discarded. The remote end waited for T1. | REJ when I frames were discarded |
| Other commands in the disconnected phase (2.4.4.4.1) | Ignored | DM F 1 for a command with P 1 |
| The UA to the DISC of the remote end | The line stopped before the UA went out. The remote end sent DISC again N2 times. | The UA goes out first |
| A valid N(R) after a go back (2.3.4.10) | After a REJ or the end of timer recovery, an N(R) for an I frame that waited to go again gave FRMR. The link reset, and the data in the I frames was lost. | An N(R) up to the last N(S) that went out is valid |
