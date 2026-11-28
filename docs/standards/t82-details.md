# T.82: technical details

This page gives the technical details of T.82 (JBIG). For the summary,
refer to [T.82](t82.md).

## The BIE

A BIE has a 20-octet BIH, then stripe data entities (SDE) and marker
segments. Each SDE ends with the ESC octet (0xFF) and SDNORM (0x02) or
SDRST (0x03). In the coded data, a 0xFF octet is sent as 0xFF 0x00.

| BIH octets | Field |
|---|---|
| 1 | DL: the first layer that is sent |
| 2 | D: the number of differential layers |
| 3 | P: the number of bit-planes |
| 4 | Fill (0) |
| 5 to 8 | XD: the width |
| 9 to 12 | YD: the height |
| 13 to 16 | L0: lines in a stripe at the lowest resolution |
| 17 | MX: the largest left lag of the AT pixel |
| 18 | MY: the largest vertical lag of the AT pixel |
| 19 | Order (Table 7): HITOLO, SEQ, ILEAVE, SMID |
| 20 | Options (Table 8): LRLTWO, VLENGTH, TPDON, TPBON, DPON, DPPRIV, DPLAST |

## The order of the data (Table 7)

With more than one bit-plane, ILEAVE and SMID give the order of the
stripes. ILEAVE 0: all the stripes of a plane, then the next plane.
ILEAVE 1, SMID 1: each stripe of each plane in turn. ILEAVE 1, SMID 0:
the stripes of the planes in turn, as layers (not used here).

## The marker segments (6.2.6)

| Marker | Octets | Use |
|---|---|---|
| ATMOVE | 0xFF 0x06, YAT (4), tX, tY | Moves the AT pixel from line YAT of the stripe. tX is 0 or from 3 to MX. |
| NEWLEN | 0xFF 0x05, YD (4) | Gives a smaller page height (VLENGTH 1) |
| COMMENT | 0xFF 0x07, length (4), data | Data that the coding ignores |
| ABORT | 0xFF 0x04 | Stops the BIE |
| RESERVE | 0xFF 0x01 | Reserved |

## The coder

- The template: three lines (LRLTWO 0) or two lines (LRLTWO 1), with
  10 pixels. The AT pixel has a default place. ATMOVE moves it.
- Typical prediction (TPBON): before each line, the coder codes one
  pseudo-pixel (SLNTP). It tells if the line is the same as the line
  above it. Its context is a fixed value (0x195 for three lines, 0x0E5
  for two lines).
- The QM coder: 113 probability states (Table 24). Each state has a
  LSZ value, a next state for MPS and LPS, and a SWITCH bit. An SDRST
  sets all the states to 0. An SDNORM keeps them.
- A coder test sequence (7.1) and an artificial test image (7.2) give
  known outputs for the coder.

## T.82 in D-Modem

| Item | D-Modem |
|---|---|
| Coding (`dsp/jbig.cpp`) | Sequential coding, DL 0, D 0. One or more bit-planes. LRLTWO 0 or 1. TPBON. |
| QM coder | All the 113 states of Table 24 |
| Encoder | ATMOVE (for tests), SDNORM. P 1 for T.85, more planes for T.43. |
| Decoder | ATMOVE, NEWLEN, COMMENT, ABORT, SDNORM and SDRST. ILEAVE 0, and ILEAVE 1 with SMID 1. |
| Not implemented | Progressive coding (D more than 0), TPDON, deterministic prediction (DPON) |

The tests `Jbig.*` (`dsp/test/jbig_test.cpp`) check the coder:

- `Jbig.CoderTestSequence`: the coder test sequence of 7.1.
- `Jbig.ArtificialImage`: the artificial test image of 7.2, with its
  BIE size.
- `Jbig.RoundTrip`: images that are coded and decoded again, with each
  option.
- `Jbig.MarkersAndLimits`: the marker segments, and the limits of the
  decoder.

## Corrections (2026-11)

None. The audit compared the QM table with all the 113 entries of
Table 24, the BIH with 6.2.2, and the marker segments with 6.2.6. They
agree.
