# T.42: technical details

This page gives the technical details of T.42. For the summary, refer
to [T.42](t42.md).

## The basic CIELAB space (6.2.1)

| Item | Value |
|---|---|
| Illuminant | CIE D50 |
| White point | X0 = 96.422, Y0 = 100.000, Z0 = 82.521 |
| Gamut | L* 0 to 100, a* -85 to 85, b* -75 to 125 |
| XYZ to CIELAB | CIE Publication 15.2 |

## The integer samples

For n-bit samples, N = ((2^n - 1) / RANGE) × value + OFFSET:

| Component | RANGE | OFFSET | 8-bit sample |
|---|---|---|---|
| L* | 100 | 0 | round(255/100 × L*) |
| a* | 170 | 2^(n-1) | round(255/170 × a* + 128) |
| b* | 200 | 2^(n-2) + 2^(n-3) | round(255/200 × b* + 96) |

White is 255, 128, 96 in 8 bits. Black is 0, 128, 96. For 12 bits, the
offsets are 2048 and 1536. A value out of the gamut is clipped.

## Other gamuts (6.2.1.3, T.4 E.6.6)

Other gamuts are options. The fax procedure must negotiate them (T.30
bit 75). T.4 Annex E sends the gamut in a G3FAX1 APP1 segment:

| Octets | Value |
|---|---|
| 2 | `FF E1` (APP1) |
| 2 | The length: `00 14` |
| 6 | `47 33 46 41 58 01` ("G3FAX", 1) |
| 12 | Three pairs of 16-bit signed integers, for L*, a* and b*: the OFFSET, then the RANGE |

The OFFSET is in the 8 most significant bits of the sample. For n-bit
samples, OFFSET × 2^(n-8) is the offset of the sample. The default gamut
is `0000 0064 0080 00AA 0060 00C8`. The example of 6.2.1.3 is L* 0 to
100, a* -128 to 127 and b* -128 to 127: the RANGE pairs are 100, 255 and
255, and the OFFSET pairs are 0, 128 and 128.

## Other illuminants (6.2.1.2, T.4 E.6.7)

Other illuminants are options. The fax procedure must negotiate them.
T.4 Annex E sends the illuminant in a G3FAX2 APP1 segment: `FF E1`, the
length `00 0C`, `47 33 46 41 58 02` ("G3FAX", 2), then a code of 4
octets.

| Code | Illuminant | White point (X0, Z0; Y0 = 100) |
|---|---|---|
| `00 44 35 30` | D50 | 96.422, 82.521 (T.42) |
| `00 44 36 35` | D65 | 95.045, 108.892 (T.42) |
| `00 44 37 35` | D75 | 94.972, 122.638 |
| `00 00 53 41` | A (SA) | 109.850, 35.585 |
| `00 00 53 43` | C (SC) | 98.074, 118.232 |
| `00 00 46 32` | F2 | 99.187, 67.395 |
| `00 00 46 37` | F7 | 95.044, 108.755 |
| `00 46 31 31` | F11 | 100.966, 64.370 |
| `43 54`, then 2 octets | A colour temperature in kelvin ("CT") | Refer to the D-Modem table |

T.42 and T.4 give only the codes. The white points of D75 to F11 are
from CIE 15 (2 degree observer).

## The ITU-YCC space (6.2.2)

ITU-YCC is sYCC (IEC 61966-2-1 Annex F) for soft copy. The illuminant is
D65, and other illuminants are not permitted. Y, Cb and Cr come from the
non-linear sR'G'B' values with the matrix of Appendix III (III-7):

| Component | From R', G', B' | RANGE | OFFSET | 8-bit sample |
|---|---|---|---|---|
| Y | 0.2990 R' + 0.5870 G' + 0.1140 B' | 1 | 0 | round(255 × Y) |
| Cb | -0.1687 R' - 0.3313 G' + 0.5000 B' | 1 | 2^(n-1) | round(255 × Cb + 128) |
| Cr | 0.5000 R' - 0.4187 G' - 0.0813 B' | 1 | 2^(n-1) | round(255 × Cr + 128) |

T.4 Annex I codes sYCC with JPEG. It has no G3FAX segments. T.30 bit
127 negotiates it.

## 12-bit samples (T.4 E.5, E.6.4)

T.30 bit 71 negotiates 12 bits a component. T.81 baseline JPEG has only
8 bits. Thus 12-bit samples use the extended process (SOF1, P = 12).
T.30 must negotiate SOF1 (T.4 E.6.2).

## T.42 in D-Modem

| Item | D-Modem |
|---|---|
| Colour spaces (`dsp/jpeg.cpp`) | CIELAB with any gamut (`T42Gamut`), and ITU-YCC with the basic gamut. The samples have 1 to 16 bits (`T42Colour`). JPEG has 8 or 12 bits. |
| CIELAB | sRGB to XYZ, with the Bradford adaptation from D65 to the white point of the illuminant. For D50, the matrix is the same as before. |
| Illuminants (`t42_white`) | The 8 codes of T.4 E.6.7. A colour temperature from 4000 K to 25000 K is on the CIE daylight locus. A colour temperature from 1667 K to 3999 K is on the Planckian locus (the cubic fit of Kim and others). Other codes are refused. |
| ITU-YCC | sRGB octets to R', G', B' (divided by 255), then the matrix III-7. Back with the inverse of the matrix. |
| Grey-scale | L* only (the same for each illuminant), or Y only for ITU-YCC |
| JPEG encoder (T.4 Annex E) | 8 bits: baseline (SOF0) with the T.81 K.3 to K.6 tables. 12 bits: extended (SOF1) with Huffman tables for the image (T.81 K.2), because K.3 to K.6 do not have the larger categories. The quantisation tables are T.81 K.1 and K.2 for the two sizes. The G3FAX APP1 (version 1994, the resolution). G3FAX1 and G3FAX2 for CIELAB when the gamut or the illuminant is not the default (`colour_info` writes them for the default also). No G3FAX1 or G3FAX2 for ITU-YCC. Colour: a* and b* at half the pels in each direction (4:1:1), or with no subsampling. |
| JPEG decoder | Baseline frames, and extended Huffman frames of 8 or 12 bits. Restarts and DNL. It reads G3FAX1 and G3FAX2 into `JpegImage::colour`. The JPEG data does not identify ITU-YCC: the caller sets it from T.30. |
| Fax procedure (`dsp/t30.cpp`) | It uses only the default (CIELAB, D50, 8 bits). It does not set T.30 bits 71, 75 or 127. |
| Not implemented | Other gamuts for ITU-YCC (no segment sends them). Gamut values of 12 bits with more precision than the 8-bit OFFSET of G3FAX1. The colour reproduction information (clause 7). |

The tests `Jpeg.*` (`dsp/test/jpeg_test.cpp`) do these checks:

- `Jpeg.DataStructure` to `Jpeg.Cielab`: the data structure, the round
  trip, restarts, DNL, damaged data, and the 8-bit octets of white,
  black and red.
- `Jpeg.TwelveBit`: the SOF1 frame with P = 12. The samples come back
  with an error of less than one 8-bit step.
- `Jpeg.G3faxSegments`: the G3FAX1 and G3FAX2 octets of the T.4 examples
  (the default gamut, a 7500 K illuminant). The decoder reads them.
- `Jpeg.Cielab12`: the 12-bit white (4095, 2048, 1536) and black.
- `Jpeg.OptionalGamut`: the optional gamut of 6.2.1.3. It keeps a
  saturated blue that the default gamut clips.
- `Jpeg.Illuminants`: the white points, and sRGB red in CIELAB D65.
- `Jpeg.Ycc`: white (255, 128, 128) and red in ITU-YCC, 8 and 12 bits.
- `Jpeg.ImageRoundTrip`: sRGB through the samples and JPEG, and back.

We also decoded the 12-bit JPEG data of the encoder with libjpeg-turbo
3.2. The samples were the same within 1. The decoder decoded 12-bit
JPEG data from libjpeg-turbo.

## Corrections (2026-11)

The audit compared the white point, the gamut, the ranges and the
offsets with 6.2.1. They agree. The audit found these deviations of
D-Modem from T.42. Now D-Modem does them:

| Item | Before | Now |
|---|---|---|
| 12-bit samples (6.2.1.3, T.4 E.6.4) | Not implemented. The decoder refused P = 12. | The encoder and the decoder use 12-bit samples in SOF1 frames. |
| Other gamuts (6.2.1.3, T.4 E.6.6) | Not implemented. The decoder skipped G3FAX1. | The conversions use any gamut. The encoder writes G3FAX1 and the decoder reads it. |
| Other illuminants (6.2.1.2, T.4 E.6.7) | Not implemented. The decoder skipped G3FAX2. | The conversions use the illuminants of E.6.7. The encoder writes G3FAX2 and the decoder reads it. |
| ITU-YCC (6.2.2) | Not implemented | The conversions use ITU-YCC in 8 or 12 bits. |
