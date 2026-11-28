# V.76: technical details

This page gives the technical details for [V.76](v76.md). The clause
numbers are the clause numbers of V.76 (08/96). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [DSVD: technical
details](../v70-details.md).

## The address field (6.1)

| Octet | Bits 8 to 3 | Bit 2 | Bit 1 |
|---|---|---|---|
| 1 | DLCI (the high bits with two octets) | C/R | EA |
| 2 (optional) | DLCI, bits 8 to 2: the low 7 bits | | EA |

- EA 1 marks the last octet. A one-octet address gives DLCIs 0 to 63. A
  two-octet address gives DLCIs 0 to 8191.
- Each DLC entity must receive frames with a two-octet address.
- The initiator takes new DLCIs from 0 up. The responder takes new
  DLCIs from 63 down (8191 with two octets). A free DLCI is used again
  before a new one.
- If the two ends take the same DLCI, the responder gives up its
  attempt.

| Frame | From the initiator | From the responder |
|---|---|---|
| Command | C/R 1 | C/R 0 |
| Response | C/R 0 | C/R 1 |

## The frames (6.2 to 6.4)

The control field is the control field of LAPM: two octets with
sequence numbers (I, RR, RNR, REJ, SREJ), one octet without (U). The
FCS has 8, 16 or 32 bits.

| Frame | Function |
|---|---|
| SABME | Opens the DLC. P is always 1. It can carry user data. |
| DISC | Closes the DLC. P is always 1. It can carry user data. |
| UA, DM | The answer to SABME and DISC. They can carry user data. |
| UI | Data with no recovery. P is 0. |
| I, RR, RNR, REJ, SREJ | ERM |
| XID | The parameters (L-SETPARM) |
| FRMR | A frame that a retransmission cannot correct |
| TEST | Optional loop-back test |

## Opening a DLC (7.1)

1. The entity sends SABME with P 1 and the user data, and starts T401.
   It ignores frames that are not U frames.
2. The other entity accepts with UA, F as P. It sets V(S), V(R) and V(A)
   to 0 and goes to the connected state. Or it refuses with DM, F as P.
3. A UA with F 1 opens the DLC. A DM with F 1 refuses it. A DM with F 0
   is ignored.
4. An I frame or an S frame can take the place of a lost UA.
5. When T401 expires, the entity sends the SABME again. After N400
   attempts, the open fails.

The UA and the DM use the FCS length of the SABME. All later frames on
the DLC use the same length.

## Closing a DLC (7.3)

1. The entity discards its queued data. It sends DISC with P 1 and the
   user data, and starts T401. It ignores frames that are not U frames.
2. The other entity sends UA, F as P, and goes to the disconnected
   state.
3. A UA or a DM with F 1 ends the DLC.
4. When T401 expires, the entity sends the DISC again. After N400
   attempts, it goes to the disconnected state.

## The disconnected state (7.4)

| Frame received | Action |
|---|---|
| DISC | DM, F as P |
| SABME | As in 7.1 |
| DM with F 0 | The entity can open the DLC with SABME, or ignore the DM |
| Other frames | Discarded |

## Commands that cross (7.5)

| Sent | Received | Action |
|---|---|---|
| SABME | SABME | UA. The DLC opens when the UA comes. |
| DISC | DISC | UA. The DLC closes when the UA comes. |
| SABME | DISC | DM. The DLC closes when a DM with F 1 comes. |
| DISC | SABME | DM. The DLC closes when a DM with F 1 comes. |
| SABME or DISC | DM with F 0 | The DM is ignored |

## Information transfer (8)

- **ERM (8.1)** is the data phase of LAPM. It has I frames with N(S)
  and N(R), and acknowledgements. It also has REJ or SREJ recovery, the
  busy condition and the timer recovery.
- **UNERM (8.2)** sends each block of data in a UI command with P 0. The
  receiver gives the data of each UI command to its user. Lost frames
  are not sent again.
- **User-control information (8.3, Annex B)** is a break or another
  signal of the user.

## The parameters (9, Annex C)

| Parameter | Default for V.70 | Selectable |
|---|---|---|
| Address field | 1 octet | 2 octets |
| FCS | 16 bits | 8 or 32 bits |
| Mode | ERM or UNERM, for each DLC | |
| ERM recovery | REJ | s-SREJ or m-SREJ |
| UNERM transport | UI frames | UIH frames |
| T401, N400 | System values (N400 at least 1) | |
| N401 | 128 octets | 1 to 4095 |
| k | 15 (ERM only) | 1 to 127 |
| DLCI values | Initiator from 0 up, responder from 63 down | |
| Real-time DLCs (Annex A) | Not used | Suspend and resume |
| TEST | Not used | TEST frames |

## V.76 in D-Modem

| Item | D-Modem (`link/v76.cpp`, `link/lapm.cpp`) |
|---|---|
| Addresses | One octet for our DLCs. A SABME or a DISC with a two-octet address gets DM. Other frames with a two-octet address are discarded. |
| DLCIs | The initiator: data on 0, voice on 1. The responder: voice on 63 (62 if the data DLC has 63). |
| ERM | One data DLC, LAPM with the defaults of Annex C (REJ, k 15, N401 128, 16-bit FCS) |
| UNERM | One voice DLC, UI commands with P 0 |
| SABME, DISC of the voice DLC | Sent again after about 1 s of line bits, 4 attempts in total |
| FCS | 16 bits only. A DLC with an 8-bit or 32-bit FCS is refused. |
| SREJ, UIH, TEST, Annex A, Annex B | Not implemented |
| DM with F 0 in the disconnected state | Ignored |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The DLCI of the responder (6.1.1) | The responder opened its voice DLC on DLCI 1, a DLCI of the initiator | DLCI 63, then down |
| The answer to our voice SABME (7.1.2.1) | A UA or a DM with F 0 opened or refused the DLC | Only F 1. A DM with F 0 is ignored. |
| The DISC of the voice DLC (7.3) | Sent once. The DLC closed at once. | Sent again until a UA or a DM with F 1, 4 attempts. A DISC that crosses gets UA, and a SABME gets DM. |
| Two-octet addresses (6.1.1) | All frames discarded. The other end sent its SABME again N400 times. | A SABME or a DISC gets DM |
