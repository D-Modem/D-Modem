# V.150.1: technical details

This page gives the technical details for [V.150.1](v150-1.md). The
clause numbers are the clause numbers of V.150.1 (01/2003). This page
is our own description, not a copy of the Recommendation. The details
of the D-Modem implementation are on
[modem relay: technical details](../v150-details.md).

## Bit order (15.1.2)

Bit 0 is the most significant bit of the first octet. A field of more
than one octet sends its most significant octet first.

## The SSE payload (C.3.2)

| Bits | Field | Function |
|---|---|---|
| 0 to 5 | Event | The media state (Table C.1). 32 to 63 are for vendors. |
| 6 | F | Force response. Only with explicit acknowledgement. |
| 7 | X | 1: an extension follows |
| 8 to 15 | RIC | The reason identifier code (Table 12) |
| 16 to 31 | RIC information | For CM and JM, the modulations (Table 13) |
| 32 to 36, 37 to 47 | Reserved, extension length | Only with X 1 |

| Event | Media state |
|---|---|
| 1 | Audio |
| 2 | VBD |
| 3 | Modem relay |
| 4 | Fax relay |
| 5 | Text relay |

| RIC | Name |
|---|---|
| 0 | Null |
| 1 | CM: the modulations of the CM sequence |
| 2 | JM: the modulations of the JM sequence |
| 18 | Time-out (Table 14) |
| 19 | p' state transition |
| 20 | Cleardown (Table 15) |
| 22 | ANSam |

The bits of Table 13, from bit 16: PCM, V.34 duplex, V.34 half
duplex, V.32 and V.32 bis, V.22 and V.22 bis, V.17, V.29 half duplex,
V.27 ter, V.26 ter, V.26 bis, V.23 duplex, V.23 half duplex, V.21, V.90
or V.92 analogue, V.90 or V.92 digital, V.91.

An SSE is sent three times at intervals of 20 ms, with the same RTP
timestamp (C.4.1). A receiver ignores the copies.

## The SPRT header (B.2.2)

| Bits | Field | Function |
|---|---|---|
| 0 | X | 0 |
| 1 to 7 | SSID | The subsession. It starts at 0. A destructive primitive increases it. |
| 8 | R | 0 |
| 9 to 15 | PT | The payload type of SPRT, from SDP. It is different from the RTP payload types. |
| 16 to 17 | TC | The transport channel |
| 18 to 31 | Sequence number | Of the packet on its channel. 0 for a packet with no payload. |
| 32 to 33 | NOA | The number of acknowledgements (0 to 3) |
| 34 to 47 | BSN | The base sequence number: the next packet that the user takes. Only for TC 1 and 2. |
| 48 to 95 | TCN and SQN | Up to 3 acknowledgements, each a channel (2 bits) and a sequence number (14 bits) |

| TC | Channel | Use |
|---|---|---|
| 0 | Unreliable, not sequenced | Acknowledgements only |
| 1 | Reliable, sequenced | Data |
| 2 | Expedited, reliable, sequenced | Control messages |
| 3 | Unreliable, sequenced | Data with no retransmission |

| Parameter (Tables B.2, B.3) | Default |
|---|---|
| Payload of TC1 and TC2 | 132 octets (132 to 256) |
| Payload of TC0 and TC3 | 140 octets (140 to 256) |
| Window of TC1 | 32 packets (32 to 96) |
| Window of TC2 | 8 packets (8 to 32) |
| TA01: the wait before an acknowledgement-only packet | 90 ms |
| TA02: the wait before a packet that updates the BSN | 130 ms (TC1), 500 ms (TC2) |
| TR03: the wait before a retransmission | 500 ms |

The receiver acknowledges each packet of TC1 and TC2. Up to three
acknowledgements go in the header of the next packet. Three
acknowledgements that wait, or TA01, cause a packet with no payload.
The BSN gives the flow control: the transmitter sends only in the
window from the BSN of the other end.

## The messages (15.4, Table 16)

Each message starts with one octet: bit 0 is reserved (0), and bits 1
to 7 are the message ID.

| ID | Message | Channel | Content |
|---|---|---|---|
| 1 | INIT | 2 | Sent when the IP-TLP starts: the channel preferences, XID profile exchange, and the optional data types (Table 17) |
| 2 | XID_XCHG | 2 | The XID compression parameters (N-TCX only) |
| 3 | JM_INFO | 2 | The JM of the modem of G2, in categories of 16 bits (Tables 19 to 24) |
| 4 | START_JM | 2 | The delayed JM procedure (20.7) |
| 5 | CONNECT | 2 | The modulation, compression, error correction and rates of the local link (Table 25) |
| 6, 7 | BREAK, BREAKACK | – | A break from the DCE, and its acknowledgement |
| 8 | MR_EVENT | 2 | A retrain or a rate change |
| 9 | CLEARDOWN | 2 | The end of the call, with a reason (Table 28) |
| 10 | PROF_XCHG | 2 | The XID profile of the modem |
| 16 | I_RAW-OCTET | 1 or 3 | Octets, with repeated segments |
| 17 | I_RAW-BIT | 1 or 3 | Bits, with repeated segments (optional) |
| 18 | I_OCTET | 1 or 3 | Octets with no format |
| 19, 20 | I_CHAR-STAT, I_CHAR-DYN | 1 or 3 | Start-stop characters with a format octet (optional) |
| 21 | I_FRAME | 1 or 3 | Framed data (optional) |

### CONNECT (Table 25)

| Bits | Field |
|---|---|
| 0 to 5 | SELMOD: 1 V.92, 2 V.91, 3 V.90, 4 V.34, 5 V.32 bis, 6 V.32, 7 V.22 bis, 8 V.22, 9 V.17, 10 V.29, 11 V.27 ter, 12 V.26 ter, 13 V.26 bis, 14 V.23, 15 V.21, 16 Bell 212, 17 Bell 103 |
| 6 to 7 | The compression direction |
| 8 to 11 | The compression: 0 none, 1 V.42 bis, 2 V.44, 3 MNP5 |
| 12 to 15 | The error correction: 0 none or V.14, 1 V.42 LAPM, 2 Annex A/V.42 |
| 16 to 31 | TDSR: the transmit rate in bit/s |
| 32 to 47 | RDSR: the receive rate in bit/s |
| 48 | DLCI enabled |
| 49 to 63 | The data types that the peer can use |
| 64 to 143 | The compression parameters, only when there is compression |

### JM_INFO (Table 19)

Each category has a 4-bit ID and 12 bits of information:

| ID (bits 0 to 3) | Category |
|---|---|
| 1000 | Call function. Data is 011 (Table 20). |
| 1010 | Modulation modes: V.34 duplex in bit 4 to V.21 in bit 15 (Table 21) |
| 0101 | Protocols. LAPM is 100 (Table 22). |
| 1011 | PSTN access (Table 23) |
| 1110 | PCM modem availability (Table 24) |
| 0000 | An extension of the category before |

### CLEARDOWN reasons (Table 28)

| Code | Reason |
|---|---|
| 0 | Unknown |
| 1 | Physical layer release |
| 2 | Link layer disconnect |
| 3 | Data compression disconnect |
| 4 | Abort |
| 5 | On-hook |
| 6 | Network layer termination |
| 7 | Administrative |

## A modem relay call (Figure 21)

1. G2 detects the answer tone of its modem. It sends ANSam as RFC 2833
   events.
2. G1 detects the CM of its modem. It sends SSE event 3 (modem relay)
   with RIC CM and the modulations.
3. G2 answers with SSE event 3 and RIC p'.
4. The two gateways send INIT on TC2.
5. G2 sends JM_INFO with the JM of its modem.
6. Each gateway trains with its modem and does V.42. Then it sends
   CONNECT with its local link.
7. The data flows as I_OCTET. CLEARDOWN ends the call.

## V.150.1 in D-Modem

| Item | D-Modem (`frontend/dmodemd/v150.cpp`) |
|---|---|
| Role | A modem relay endpoint (U-MR): the modem and its gateway in one |
| TCX | D-TCX. A far gateway that is not D-TCX, and whose modem compresses, gets CLEARDOWN with reason 3. |
| INIT | All bits 0: no optional data types, no XID profile exchange |
| Our modulations | V.34, V.32 bis, V.22 bis and V.21. The answerer selects the best that both have. |
| Our CONNECT | V.42 LAPM, no compression, no DLCI |
| Data sent | I_OCTET on TC1 |
| Data received | I_OCTET, I_RAW-OCTET, I_CHAR-STAT and I_CHAR-DYN |
| SPRT | The defaults of Tables B.2 and B.3. TA02 is sent only when the BSN changed. The SSID of the other end is not checked. |
| SSE | No extension, no explicit acknowledgement, no RFC 2198 redundancy for SSE |
| BREAK | Answered with BREAKACK. The break is not given to the DTE. |
| XID_XCHG, START_JM, MR_EVENT, PROF_XCHG | Ignored |
| Fax relay, text relay | Not done |

No change to the code was necessary for this page.
