# T.43: technical details

This page gives the technical details of T.43. For the summary, refer
to [T.43](t43.md).

## The BCIE (7.2)

| Part | Octets |
|---|---|
| Magic number | 0xFF 0xA8 |
| Entries | 0xFF 0xE1 (2-octet length) or 0xFF 0xE3 (4-octet length), the length, "G3FAX" or "G4FAX" and a number n, the data |
| BCID | One JBIG BIE with all the bit-planes |
| End marker | 0xFF 0xA9 |

| Entry | Contents |
|---|---|
| G3FAX0 (mandatory) | The version (1997), the resolution (200, 300 or 400), the coding (0: JBIG), the image type, the bits of each component (4 octets). Its length is 18. |
| G3FAX1, G3FAX2 | The gamut and the illuminant (options) |
| G3FAX3 | The palette: a table ID (0: 8-bit, 4: 12-bit), the number of entries, the CIELAB entries |
| ECIH (G3FAX255, mandatory) | The end of the header |

## The image types

| Type | Image | Planes |
|---|---|---|
| 0, 1, 2 | One bit for each colour: RGB, CMY, CMYK | R, G, B (or C, M, Y, K) |
| 16, 17 | A palette with 8-bit or 12-bit entries | The index, MSB first (up to 12 bits, or 16 as an option) |
| 32 | Grey-scale (L*) | MSB first (up to 12 bits) |
| 48 | Colour (CIELAB) | L* MSB first, then a*, then b* (up to 12 bits each) |

Types 32 and 48 are Gray coded: g = n XOR (n >> 1). No subsampling is
used.

## The JBIG parameters (Table 7)

Table 7 is the same as T.85 Table 1, but P is from 1 to 36 and ILEAVE
is 1. SMID 1 is stripe interleave (the basic method, L0 128). SMID 0 is
plane interleave, which needs the DIS/DCS bit 37.

## T.43 in D-Modem

| Item | D-Modem |
|---|---|
| Coding (`dsp/t43.cpp`) | All the image types, with the palette entry. Stripe interleave (L0 128), or plane interleave as an option. |
| Decoder | Both interleaves, any L0, 2-octet and 4-octet entries. The G3FAX1 and G3FAX2 entries and unknown entries are skipped. |
| T.30 (`dsp/t30.cpp`) | DIS bits 36 and 37. A page with at most a small number of colours or grey levels goes in T.43 (type 16 or 32), else in JPEG. |

The tests `T43.*` (`dsp/test/t43_test.cpp`) check these items:

- The header octets and the Gray code (T.43 Table 6).
- The round trip of each image type, and the colours.
- Damaged data and the JBIG planes.

## Corrections (2026-11)

None. The audit compared the entries with 7.2, the plane order and the
Gray code with 7.3.1, and the JBIG parameters with Table 7. They
agree.
