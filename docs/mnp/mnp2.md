# MNP 2

MNP 2 sends start-stop octets in frames, in the two directions at the
same time (two-way simultaneous, or full duplex). It is framing mode 2
of V.42 Annex A.

## What it does

- A modem can send a frame while the other modem sends.
- Each modem can have up to k LT frames that are not acknowledged.
- The start and stop bits stay on each octet. Thus, the efficiency is
  about 84% (secondary source: Wikipedia).

## On the line

V.42 Annex A gives the full procedure (A.3 and A.6):

- A frame is SYN DLE STX, the header, the information, DLE ETX and the
  FCS. The FCS polynomial is x^16 + x^15 + x^2 + 1. The FCS goes low
  octet first.
- In the header and the information, each DLE is sent two times
  (transparency).
- The LR exchange always uses start-stop octets, also when the data
  phase uses framing mode 3.

| Frame | Type | Use |
|---|---|---|
| LR | 1 | Link request: the parameters of the connection |
| LD | 2 | Link disconnect, with a reason |
| LT | 4 | Link transfer: the data, with N(S) |
| LA | 5 | Link acknowledgement: N(R) and the credit N(k) |
| LN | 6 | Link attention: a break |
| LNA | 7 | Link attention acknowledgement |

The sequence numbers are modulo 256. V(S), V(R), V(SA) and V(RA) start
at 1 in the data phase.

## D-Modem

D-Modem supports MNP 2.

- As the responder, D-Modem accepts framing mode 2 from the initiator,
  and uses it in the data phase (`MnpAsync` in `link/mnp.h`).
- As the initiator, D-Modem always offers framing mode 3. No AT command
  makes it offer framing mode 2. The tests set `mnp_mode = 2` in the
  link configuration to examine it (`Link.MnpOctets`).
- The `Link.MnpOctets` and `Link.MnpBitErrors` tests examine the data
  phase in framing mode 2.

Refer to [the technical details of the link layer](../link-details.md#mnp)
for the timers and the retransmission rules.
