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
| 21 to 1748 | DPTABLE: a private DP table (only if DPON 1, DPPRIV 1 and DPLAST 0) |

## The order of the data (Tables 7 and 11)

Three loops give the order of the SDEs: s (the stripe, top to
bottom), d (the resolution layer) and p (the bit-plane, the most
significant first). SEQ, ILEAVE and SMID select one of six orders.
Two combinations (SEQ 0, ILEAVE 0, SMID 1, and SEQ 1, ILEAVE 1, SMID
1) are not permitted.

| SEQ | ILEAVE | SMID | Outer | Middle | Inner |
|---|---|---|---|---|---|
| 0 | 0 | 0 | p | d | s |
| 0 | 1 | 0 | d | p | s |
| 0 | 1 | 1 | d | s | p |
| 1 | 0 | 0 | s | p | d |
| 1 | 0 | 1 | p | s | d |
| 1 | 1 | 0 | s | d | p |

HITOLO 0 sends the layers from the lowest resolution to the highest.
HITOLO 1 sends them from the highest to the lowest. With HITOLO 1, a
decoder must keep all the data before it can decode the image.

## The resolution layers (6.2.3 and 6.3)

Layer D has the full resolution (XD by YD). Each lower layer has half
the resolution: X(d-1) = ceil(X(d) / 2), Y(d-1) = ceil(Y(d) / 2). A
stripe has L0 lines in layer 0 and L0 x 2^d lines in layer d. All the
layers have the same number of stripes.

Resolution reduction (Figure 4 and Table 17) makes one low resolution
pixel from 12 pixels: 9 high resolution pixels around its 2 x 2 block,
and 3 low resolution pixels made before it. The 12 pixels make an
index into Table 17 (4096 bits).

## The differential layers (6.4, 6.6 and 6.7.2)

A differential layer codes each high resolution pixel with the low
resolution layer as a reference. The phase of a pixel is its place in
its 2 x 2 block (0 top left, 1 top right, 2 bottom left, 3 bottom
right). For each pixel, the coder does these steps in turn:

1. TPDON (6.4): at the start of each line pair, the coder codes the
   pseudo-pixel LNTP. LNTP 0 tells that the line pair is typical. In a
   typical line pair, if a low resolution pixel and its 8 neighbours
   have one colour, its 4 high resolution pixels have that colour too.
   The coder does not code them. The context of LNTP is the context of
   a phase 3 pixel with the pixels of Figure 8.
2. DPON (6.6): Tables 19 to 22 give a value for each pattern of the
   reference pixels of Figure 13. If the value is 0 or 1, resolution
   reduction makes the pixel that colour. The coder does not code it.
   A private table (DPPRIV 1) replaces the default tables.
3. The coder codes the pixel. The context has 12 bits: 6 high
   resolution pixels (with the AT pixel), 4 low resolution pixels
   (Figure 16), and 2 bits for the phase.

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
| Coding (`dsp/jbig.cpp`) | DL 0. Sequential coding (D 0) and progressive coding (D more than 0). One or more bit-planes. LRLTWO 0 or 1. TPBON, TPDON and DPON. |
| Orders | All six orders of Table 11, and HITOLO 0 or 1 |
| Resolution reduction | Table 17 (`jbig_reduce()`) |
| DP tables | The default Tables 19 to 22. The decoder also reads a private DPTABLE. |
| QM coder | All the 113 states of Table 24 |
| Encoder | ATMOVE in each layer (for tests), SDNORM. P 1 for T.85, more planes for T.43. |
| Decoder | ATMOVE, NEWLEN, COMMENT, ABORT, SDNORM and SDRST. A progressive BIE is decoded after all its SDEs are read. |
| Not implemented | DL more than 0 (an image in more than one BIE). DPLAST 1 with DPON 1 in a progressive BIE (the decoder keeps no DP table from an earlier BIE). MY more than 0. |

The tests `Jbig.*` (`dsp/test/jbig_test.cpp`) check the coder:

- `Jbig.CoderTestSequence`: the coder test sequence of 7.1.
- `Jbig.ArtificialImage`: the artificial test image of 7.2, with its
  BIE size.
- `Jbig.RoundTrip`: images that are coded and decoded again, with each
  option.
- `Jbig.MarkersAndLimits`: the marker segments, and the limits of the
  decoder.
- `Jbig.ProgressiveArtificialImage`: the progressive test of 7.2.3 (D
  6, L0 2, TPDON, TPBON, DPON). Each layer agrees with all the columns
  of Table 30. The PSCD, SDE and BIE sizes agree with Table 32.
- `Jbig.Tables`: Table 17 and the DP tables. The DP tables have the
  hits of Table 18, and each hit agrees with Table 17.
- `Jbig.ResolutionReduction`: the sizes and colours of a reduced
  image.
- `Jbig.ProgressiveRoundTrip`: images with 1 to 3 planes, in all the
  orders, with and without TPDON, DPON and LRLTWO.
- `Jbig.ProgressiveMarkersAndLimits`: ATMOVE in a differential layer,
  NEWLEN, ABORT, a limit of lines, a private DP table.
- `Jbig.StripeReset`: the stripe after an SDRST.
- `Jbig.SequentialStats`: the counts of the sequential encoder.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| Progressive coding (D more than 0), TPDON and DPON were not implemented. The decoder refused these BIEs. | The encoder and the decoder do progressive coding with TPDON and DPON (6.3, 6.4, 6.6, 6.7.2). |
| After SDRST, the decoder used the lines of the previous stripe as the lines above the next stripe. | The lines above the next stripe are background, as at the top of the image (6.2.5). |

The tests `Jbig.ProgressiveArtificialImage` and `Jbig.StripeReset`
check these corrections. The audit compared the QM table with all the
113 entries of Table 24, the BIH with 6.2.2, and the marker segments
with 6.2.6. They agree.
