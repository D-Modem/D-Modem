# V.13: technical details

This page gives the technical details for [V.13](v13.md). The clause and
note numbers are the numbers of V.13 (03/1993). This page is our own
description, not a copy of the Recommendation.

## The scrambler (3)

The two patterns use the scrambler 1 + x<sup>-3</sup> + x<sup>-7</sup>.
This is a self-synchronizing scrambler with seven stages. V.13 does not
specify the start state of the scrambler.

- **The idle pattern** is the output of the scrambler for a continuous
  input of ONE bits.
- **The ON pattern** is the output of the scrambler for 8 input bits of
  ZERO. The scrambler continues from its state at the end of the idle
  pattern (note 1).

If all seven stages are ONE, a scrambler with a ONE input gives only ONE
bits. Then the idle pattern is not pseudo-random. The transmitter must
prevent this state. The idle pattern must be a sequence of 127 bits
(note 6).

## The transmitter (3)

1. While circuit 105 is OFF, send the idle pattern in place of data.
2. When circuit 105 comes ON, send the ON pattern immediately.
3. Then send the data.
4. Circuit 106 (ready for sending) can come ON in 8 bit intervals after
   circuit 105. If circuit 106 comes ON before the ON pattern is complete,
   keep the data in a buffer until the ON pattern is complete.
5. After circuit 105 goes OFF, ignore circuit 105 for at least 128 bit
   intervals. Thus, the far modem receives at least 128 bits of the idle
   pattern (note 5).

## The receiver (3)

1. Descramble the received bits.
2. If 48 to 64 bits in a sequence are ONE (the idle pattern), set circuit
   109 OFF (note 3). Some of the idle pattern can go to circuit 104 before
   circuit 109 goes OFF.
3. While circuit 109 is OFF, hold circuit 104 (received data) at ONE.
4. After the idle pattern, if 8 bits in a sequence are ZERO (the ON
   pattern), set circuit 109 ON (note 4).
5. If circuit 109 stays OFF for a long time without the idle pattern, set
   circuit 109 ON. Thus, a lost ON pattern does not stop all of the data
   (note 8).

Errors on the line can set circuit 109 to the incorrect condition. Also,
user data can look like the idle pattern (note 7).

## V.13 in D-Modem

`core/v13.h` has the scrambler, the descrambler and `SimCarrier`.

| Item of V.13 | D-Modem |
|---|---|
| Idle pattern | Scrambled ONE bits. The state `0x7F` (all ONE) changes to `0x7E` (note 6). |
| ON pattern | 8 scrambled ZERO bits (`ON_BITS`), from the state at the end of the idle pattern. |
| Circuit 105 OFF to ON | The first ZERO bit from the data link layer (a start bit). The data waits for the ON pattern. |
| Circuit 105 ON to OFF | 64 ONE bits in a sequence from the data link layer (`OFF_IDLE`). |
| Idle pattern before the next ON | At least 128 bits (`MIN_IDLE`, note 5). |
| Circuit 109 ON to OFF | 56 ONE bits after the descrambler (`DETECT_IDLE`, in the range of 48 to 64). |
| Circuit 109 OFF to ON | 8 ZERO bits after the descrambler (`ON_BITS`). |
| Note 8 | Circuit 109 comes ON after 2048 bits without the idle pattern (`LOST_IDLE`). |
| Circuit 104 | ONE while circuit 109 is OFF. The bits go to the link 64 bits late (`DELAY`). Thus, no idle pattern reaches the DTE. |

The check against V.13 found no errors.
