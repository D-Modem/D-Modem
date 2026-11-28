# T.85: technical details

This page gives the technical details of T.85. For the summary, refer
to [T.85](t85.md).

## The BIH parameters (Table 1)

| Parameter | Value |
|---|---|
| DL, D | 0 |
| P | 1 |
| XD | A T.4 width (1728, 2048, 2432, and the other T.4 widths) |
| YD | The full T.82 range. 0xFFFFFFFF with NEWLEN when the length is not known. |
| L0 | 128 (basic). From 1 to YD as an option that T.30 negotiates. |
| MX | 0 to 127 |
| MY | 0 |
| HITOLO, SEQ, ILEAVE, SMID | 0 |
| LRLTWO | 0 (three lines) or 1 (two lines) |
| VLENGTH | 0 or 1 (NEWLEN can come) |
| TPDON, DPON, DPPRIV, DPLAST | 0 |
| TPBON | 0 or 1 |

The sender sets the unused parameters to 0. The receiver does not need
to examine them.

## The negotiation (T.30)

| DIS/DCS bit | Meaning |
|---|---|
| 78 | T.85 (basic: L0 128) |
| 79 | T.85 optional L0 (other L0 values) |

T.85 needs ECM (bit 27). A COMMENT marker segment has a meaning only
if the two ends agree on it.

## T.85 in D-Modem

| Item | D-Modem |
|---|---|
| Send (`dsp/t30.cpp`) | DCS bit 78 when the far DIS has it. L0 128, TPBON, LRLTWO 0, MX 0, YD the real height. |
| Receive | A DCS with bit 78, or bit 79 alone (spandsp sends it). Any L0, NEWLEN, ATMOVE and COMMENT. A BIE with D more than 0 or with DPON 1 is refused (Table 1 sets them to 0). |
| DIS | Bits 78 and 79 (T.85 with any L0), with ECM |
| Pages | Each ECM document is one BIE. The decoder stops at a limit of rows. |

The tests `Jbig.*` (`dsp/test/jbig_test.cpp`) check the coding. The tests
`Rates/T30Pair.SendsTheDocument/*` (`dsp/test/fax_test.cpp`) send T.85
pages with ECM between two D-Modem T.30 machines.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| After SDRST, the decoder used the lines of the previous stripe as the lines above the next stripe. | The lines above the next stripe are background (T.82 6.2.5). |

The test `Jbig.StripeReset` checks this correction. The BIH that
D-Modem sends agrees with Table 1.
