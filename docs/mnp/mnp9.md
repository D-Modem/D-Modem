# MNP 9

MNP 9 is MNP 7 compression on the V.32 modulation, with changes to the
link layer. No specification is public, and we have no firmware that
has MNP 9.

All the statements on this page come from secondary sources. We did
not examine them in firmware or on a line. Wikipedia gives only the
ULN item and says that MNP 9 is otherwise "identical to MNP 7". The
other items come from other descriptions of MNP, and we did not
confirm them.

## What it does

- **V.32.** MNP 9 runs on V.32 at 9600 bit/s, a two-way simultaneous
  modulation. Thus it does not need the statistical duplexing of MNP 6.
- **MNP 7 compression.** The data compression is the same as in
  [MNP 7](mnp7.md).
- **Better acknowledgements.** An LT frame can also carry the
  acknowledgement for the other direction. Thus fewer LA frames are
  necessary.
- **Selective retransmission.** The receiver asks for only the LT
  frames that have errors. The transmitter does not send again all the
  LT frames after the bad frame.
- **Universal Link Negotiation.** MNP 9 adds the V.32 modes to the ULN
  of [MNP 6](mnp6.md) (secondary source: Wikipedia).

## On the line

We do not know the frame formats of MNP 9:

- the LT frame that carries an acknowledgement;
- the frame that asks for a selective retransmission;
- the LR parameter or bit that offers MNP 9.

V.42 (LAPM) has the same two functions: an I frame carries N(R), and
the SREJ frame asks for one frame. MNP 9 can have a similar design, but
this is a guess.

## D-Modem

D-Modem does not support MNP 9.

D-Modem has V.32 and V.32bis data pumps, and LAPM with SREJ. Thus the
data pump side is ready. The link layer needs the MNP 9 frames, and the
compression needs [MNP 7](mnp7.md).

## What is necessary to support it

1. Get the firmware of a Microcom V.32 modem with MNP 9.
2. Find the LR parameters, the frame formats and the ULN changes.
3. Add the frames to `Mnp` in `link/mnp.cpp`.
4. Add MNP 7 (see [MNP 7](mnp7.md)).
