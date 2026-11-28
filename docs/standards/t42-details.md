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

## The options

- Other gamuts and other illuminants: they must be negotiated. T.4
  Annex E sends them in G3FAX APP1 marker segments (G3FAX1, G3FAX2).
- The YCC space (6.2.2): sYCC, D65, for soft copy.

## T.42 in D-Modem

| Item | D-Modem |
|---|---|
| Colour space (`dsp/jpeg.cpp`) | The basic CIELAB, D50, 8 bits. sRGB to XYZ with the Bradford adaptation to D50. |
| Grey-scale | L* only, from the sRGB grey level |
| JPEG (T.4 Annex E) | Baseline. The G3FAX APP1 (version 1994, the resolution). The T.81 K.1 to K.6 tables. Colour: a* and b* at half the pels in each direction (4:1:1), or with no subsampling. |
| Decoder | Baseline and extended 8-bit Huffman frames. Restarts and DNL. The other G3FAX segments are skipped and the basic values are used. |
| Not implemented | The 12-bit samples in JPEG, other gamuts and illuminants, YCC |

The tests `Jpeg.*` (`dsp/test/jpeg_test.cpp`) check the data structure,
the round trip, restarts, DNL and damaged data. `Jpeg.Cielab` checks
the octets of white, black and red, and the round trip through CIELAB.

## Corrections (2026-11)

None. The audit compared the white point, the gamut, the ranges and the
offsets with 6.2.1. They agree.
