# MNP 4

MNP 4 adds two features to MNP 3: data phase optimization and adaptive
packet assembly. V.42 Annex A gives the data phase optimization. A
Microcom patent gives adaptive packet assembly.

## Data phase optimization

LR parameter 8 offers the optimizations (V.42 A.6.4.1.8). The
responder sends parameter 8 only if it agrees to one or more of them.

| Bit (mask) | V.42 name | What it does |
|---|---|---|
| 0x01 (V.42 bit 1) | Maximum information field of 256 octets | N401 becomes 256, whatever parameter 4 says |
| 0x02 (V.42 bit 2) | Fixed-field LT and LA frames | The LT and LA headers have no parameter types or lengths |

With fixed-field frames, the headers are shorter:

| Frame | Header without optimization | Header with fixed fields |
|---|---|---|
| LT | `04 04 01 01 N(S)` | `02 04 N(S)` |
| LA | `07 05 01 01 N(R) 02 01 N(k)` | `03 05 N(R) N(k)` |

The first octet of each header is the length indication. It does not
count itself.

The DeskPorte firmware gives the class number 4 when the 0x02 bit is
negotiated (see
[firmware-findings.md](firmware-findings.md#f1-the-class-number)). The
0x01 bit alone does not give class 4.

## Adaptive packet assembly

Adaptive packet assembly changes the size of the LT frames to agree
with the quality of the line. Small frames are better on a bad line,
because a retransmission costs less. Large frames are better on a good
line, because the headers cost less.

US 4,691,314 (Microcom, 1987) gives the method:

1. Adaptive packet assembly starts only if the negotiated class
   supports it. The maximum size is then 256 octets.
2. The transmitter counts the LT frames that it sends (T) and the LT
   frames that it sends again (R).
3. It calculates a line quality: LQ = MAX[MIN[LQ' + T × Y, 255] − R × X, 0].
   The example values are X = 20, Y = 1 and a start value of 192.
4. Five ranges of LQ give the LT sizes 32, 64, 128, 192 and 256 octets.
   The patent does not give the limits of the ranges.

Wikipedia gives LT sizes from 64 to 256 octets, and says that the user
can set a fixed size (secondary source).

The receiver does not have to know about adaptive packet assembly. Any
LT up to N401 octets is correct. Thus adaptive packet assembly has no
LR parameter of its own, and it is not a wire rule.

## The DeskPorte

The DeskPorte firmware keeps the LT size in one variable, P (the size
minus 1). The MNP 10 specification gives its rules:

- A retransmission makes P smaller, but only when the 256-octet
  optimization is negotiated.
- Only the MNP 10 line probes make P larger.
- Without the 256-octet optimization, the LT size is N401 (64 by
  default).

We do not know if P can become smaller on a call without MNP 10.
If it can, the LT size on such a call can only decrease.

## D-Modem

D-Modem supports the data phase optimization, but not adaptive packet
assembly.

- D-Modem's LR offers both optimizations (0x03). The responder answers
  with the optimizations that both modems have.
- With the 256-octet optimization, D-Modem sends LT frames of up to 256
  octets. Without it, the LT size is N401. `AT\A0` to `AT\A3` set N401 to
  64, 128, 192 or 256 octets.
- The LT size does not change during a call without MNP 10. On an MNP 10
  call, the line probes and the retransmissions change it (see
  [mnp10.md](mnp10.md)).
- The `Link.MnpOctets` test examines a call with no optimizations.

## What is necessary to support adaptive packet assembly

This is a local policy. The peer does not see it, only the LT sizes.

1. Count the LT frames sent and sent again in `Mnp`.
2. Calculate LQ as US 4,691,314 gives it.
3. Select the LT size from LQ. Use the ranges 32, 64, 128, 192 and 256.
4. Use it only when MNP 10 is not active, because MNP 10 has its own
   rules.
