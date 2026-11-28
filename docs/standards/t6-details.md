# T.6: technical details

This page gives the MMR code of T.6 (11/88). For a summary, refer to
[T.6](t6.md).

## The basic coding scheme (2.2)

- Each line is coded against the line above it (the reference line).
  After a line is coded, it becomes the reference line.
- The reference line of the first line is an imaginary white line.
- The changing elements a0, a1, a2, b1 and b2 are as in T.4 4.2.1.3.1.

| Mode | When | Code word (Table 1) | New a0 |
|---|---|---|---|
| Pass (P) | b2 is to the left of a1. b2 above a1 is not a pass. | 0001 | Below b2 |
| Vertical (V) | \|a1b1\| ≤ 3 | V(0) 1, VR(1) 011, VR(2) 000011, VR(3) 0000011, VL(1) 010, VL(2) 000010, VL(3) 0000010 | a1 |
| Horizontal (H) | All other cases | 001, then M(a0a1) and M(a1a2) | a2 |
| Extension | The uncompressed mode (an option) | 0000001xxx | |

- M() is a run in the code words of Tables 2 and 3. They are the same
  code words as in T.4 Tables 2, 3a and 3b.
- A run of 64 to 2623 pels has a make-up code word, then a terminating
  code word. A run of 2624 pels or more starts with make-up code words
  of 2560.
- At the start of a line, the first run a0a1 is replaced by a0a1 − 1.
  If the line starts with black, the first horizontal run is a white
  run of 0.
- If b1 or b2 is not on the reference line, it is at the imaginary
  element after the last pel.

## The control functions (2.4)

| Signal | Code |
|---|---|
| EOFB | 000000000001 000000000001: the end of the facsimile block |
| Pad bits | Zeros after EOFB, to an octet boundary or a fixed block size |

There is no EOL between the lines, and no fill.

## T.6 in D-Modem

| Item | D-Modem (`dsp/t4.cpp`) |
|---|---|
| Decoder | `T4Page::decode()` with compression 4 (TIFF). An EOFB, or any code word that is not in Table 1, ends the page. |
| Code words | The tables of T.4 (`T.4: technical details`), the same as Tables 1 to 3 of T.6 |
| Encoder | `T4Page::encode_mmr()`: each line against the line above (a white line above the first), no EOL, EOFB at the end, padded to an octet |
| On the line | With ECM only. The DIS sets bit 31. A sender sends MMR when bit 31 of the DIS is 1 and no other coding (T.85, T.42, T.43, T.44, T.434) is in use. |
| Uncompressed mode | Not implemented |

The tests `T4Libtiff` (`dsp/test/fax_test.cpp`) decode MMR pages that
libtiff codes (the `g4` case). The tests
`SpandspT4.OurEncoderIntoSpandsp` and `SpandspT4Random` send our MMR
pages into the decoder of spandsp.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| There was no MMR encoder. A fax call did not offer or send MMR. | `encode_mmr()` codes MMR. With ECM, the DIS sets bit 31, and a call sends and receives MMR. |

The tests `T30Pair.SendsTheDocument` (`dsp/test/fax_test.cpp`, the
`_ecm` cases with T.85 off) send pages in MMR.
