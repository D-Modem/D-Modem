# T.4: technical details

This page gives the dimensions, the line times, the codes and the line
format of T.4 (07/2003). For a summary, refer to [T.4](t4.md).

## The dimensions (2)

| Item | Value |
|---|---|
| Vertical resolution | 3.85 lines/mm ± 1 % (standard). Options: 7.7 and 15.4 lines/mm. |
| Pels on a line | 1728 on 215 mm (standard). Options: 2048 on 255 mm, 2432 on 303 mm, and 3456, 4096, 4864 at the higher horizontal resolution. |
| Inch-based options (Table 1) | 100 × 100 to 1200 × 1200 pels/25.4 mm. For example, 200 × 200 gives 1728 pels for A4, 2048 for B4 and 2432 for A3. |
| Scan | Left to right, then the next line below |

## The time of a total coded scan line (3)

A total coded scan line is the data, the fill and the EOL (and the tag
bit in MR).

| Item | Value |
|---|---|
| Minimum time (3.1) | 20 ms (standard). Options: 0, 5, 10 and 40 ms. With alternative 2, the time at the higher resolution is half the time at the standard resolution. T.30 selects the time in phase B. |
| Maximum time (3.2) | Less than 13 s (19 s at 600 pels/25.4 mm, 37 s at 1200 pels/25.4 mm). If a line takes longer, the receiver must disconnect. An older receiver can disconnect after 5 s. |
| Fill (4.1.3) | Zeros between the data of a line and the EOL, never in the data. Fill for less than 5 s. |

## The one-dimensional code, MH (4.1)

- Each line is a series of runs. White runs and black runs alternate.
- Each line starts with a white run. A line that starts with black
  starts with a white run of 0.
- A run of 0 to 63 pels has one terminating code word (Table 2).
- A run of 64 to 1728 pels has a make-up code word (Table 3a), then a
  terminating code word. The make-up code word is for the largest
  multiple of 64 that is not more than the run.
- Table 3b gives make-up code words for 1792 to 2560, the same for
  the two colours. A run of 2624 pels or more starts with make-up code
  words of 2560, until less than 2560 pels remain.
- White and black have different code words.

| Signal | Code |
|---|---|
| EOL | 000000000001, before the first line of the page and after each line |
| RTC | Six EOLs: the end of the page. Then T.30 sends the post-message commands. |

## The two-dimensional code, MR (4.2)

MR is an option. After each line coded in MH, at most K − 1 lines are
coded in two dimensions. K is 2 at the standard resolution, 4 at 7.7
lines/mm (200 lines/25.4 mm), and up to 24 at 1200 lines/25.4 mm.

A changing element is a pel with a colour different from the pel
before it. A two-dimensional line codes the position of each changing
element against the line above it (the reference line):

| Element | Description |
|---|---|
| a0 | The start element on the coding line. At the start of the line, an imaginary white element before the first pel. |
| a1, a2 | The next changing elements to the right of a0 on the coding line |
| b1 | The first changing element on the reference line to the right of a0 and of the colour opposite to a0 |
| b2 | The next changing element after b1 on the reference line |

| Mode | When | Code word (Table 4) | New a0 |
|---|---|---|---|
| Pass (P) | b2 is to the left of a1 | 0001 | Below b2 |
| Vertical (V) | \|a1b1\| ≤ 3 | V(0) 1, VR(1) 011, VR(2) 000011, VR(3) 0000011, VL(1) 010, VL(2) 000010, VL(3) 0000010 | a1 |
| Horizontal (H) | All other cases | 001, then the MH code words of a0a1 and a1a2 | a2 |
| Extension | Uncompressed mode (an option) | 0000001111 (in a two-dimensional line) | |

- At the start of a line, the first run a0a1 is replaced by a0a1 − 1.
- If b1 or b2 is not on the reference line, it is at the imaginary
  element after the last pel.
- After each line: EOL and a tag bit. EOL + 1: the next line is MH.
  EOL + 0: the next line is two-dimensional. EOL + 1 comes before the
  first line.
- RTC in MR is six times EOL + 1.

## The other coding schemes (4.3, 4.4)

- The T.6 code (MMR) is an option in Group 3, with ECM only.
- JBIG (T.82, with the rules of T.85) is an option, with ECM only.

## The annexes

| Annex | Subject |
|---|---|
| A | The error correction mode (ECM): HDLC frames of 64 or 256 octets |
| B | File transfer |
| C | Character mode (T.4 characters) |
| D | Mixed mode |
| E | Continuous-tone colour and grey-scale (JPEG, T.42) |
| G | Lossless colour and grey-scale (JBIG, T.43) |
| H | Mixed raster content (T.44) |
| I | sYCC colour |

## T.4 in D-Modem

| Item | D-Modem (`dsp/t4.cpp`) |
|---|---|
| Code tables | Tables 2, 3a, 3b and 4. The tables agree with the text of T.4 (checked word by word). |
| Encoder | MH and MR (`encode_mh()`, `encode_mr()`): an EOL before each line (in MR, with the tag bit), fill up to the minimum time from the DIS, RTC at the end. In MR, K is 2 at standard resolution and 4 at fine. With ECM, no fill. A call sends MR when bit 16 of the DIS is 1. With `mh_only` (`t30_config`), a call sends MH only. |
| Line decoder (`T4Rx`) | MH and MR, from the bits of the line, with the tag bit. A line with an error is a copy of the line above it. Six EOLs with no data between them give RTC. |
| Maximum line time | A line of more than 13 s (data and fill) ends the call with DCN. |
| Page decoder (TIFF) | MH without EOLs (TIFF compression 2), T.4 with EOLs (3, MH or MR) and T.6 (4) |
| Page width | 1728 pels. The DIS offers A4 width only. |
| Resolutions | Standard and fine (7.7 lines/mm) |
| Not implemented | The uncompressed mode, widths of more than 1728 pels on the line |

The tests `SpandspT4` (`dsp/test/spandsp_test.cpp`) send MH and MR
pages from the encoder of spandsp into the line decoder, with and
without fill. `SpandspT4.OurEncoderIntoSpandsp` and
`SpandspT4Random.OurEncoderIntoSpandsp` send our MR and MMR pages into
the decoder of spandsp. `T4.MrAndMmrRoundTrip` decodes our MR and MMR
pages again. The tests `T4Libtiff` (`dsp/test/fax_test.cpp`) decode
the MH, MR and MMR pages that libtiff codes.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| The receiver did not limit the time of a scan line. A sender that sent fill without an end kept the call in phase C. | A line of more than 13 s ends the call (3.2). |
| A call sent pages in MH only. There was no MR encoder (4.2). | `encode_mr()` codes MR. A call sends MR when the remote accepts it. |

The test `T4.LineTimeLimit` (`dsp/test/fax_test.cpp`) checks the line
time. `T4.MrAndMmrRoundTrip` and the tests `T30Pair.SendsTheDocument`
check the MR encoder.
