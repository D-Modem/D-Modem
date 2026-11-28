# T.44: technical details

This page gives the technical details of T.44. For the summary, refer
to [T.44](t44.md).

## The page (9.6)

| Part | Octets |
|---|---|
| SOP | 0xFF 0xD8 (SOI), APP13 (0xFF 0xED), the length, "MRC" 0, the version (2), the mode (1), the mask coders, the image coders, the mask resolution (2 octets), the page width (4 octets) |
| TN | 0xFF 0xD9 |
| Optional segments | APP13, the length, "MRC" n: the gamut (OMSgl, OMSgy) and the illuminant (OMSi). A decoder can skip them. |
| Each stripe | The SOSt, then the coded mask, the background and the foreground |
| EOP | 0xFF 0xD9 0xFF 0xD9 |

## The segment length (9.2)

The 2-octet length counts the octets after APP13. If two octets are
not sufficient, the 2-octet length is 0, and a 4-octet optional length
follows the 4-octet identifier. The lengths 1 to 5 are reserved.

## The coders (Tables 1 and 2)

| Mask coder bit | Coding | Image coder bit | Coding |
|---|---|---|---|
| 0 | MH | 0 | JPEG, LAB |
| 1 | MR | 1 | JBIG (T.43), LAB |
| 2 | MMR | 2 | T.45, LAB (not in mode 1) |
| 3 | JBIG (T.85) | 3 to 5 | The same codings, YCC |
| 4 | JBIG2 (Annex B) | 7 | Extend |

In mode 1, the mask has one coder. If the page has no coded mask, the
mask coder is 0. If the page has no coded image layer, the image coder
is 0.

## The SOSt (9.3)

| Field | Octets |
|---|---|
| APP13, the length, "MRC" 1 | 8 |
| The type of stripe: bit 0 background, bit 1 mask, bit 2 foreground | 1 |
| The background colour (default white: 0xFF 0x80 0x60) | 3 |
| The foreground colour (default black: 0x00 0x80 0x60) | 3 |
| The offsets of the background and of the foreground (x, y) | 16 |
| The stripe height | 4 |
| The coded length of the mask (0 if no mask) | 4 |

A stripe with two or more layers must have the mask. A stripe with
only a foreground has a mask of all 1. The resolution of an image
layer is an integral factor of the mask resolution.

## T.44 in D-Modem

| Item | D-Modem |
|---|---|
| Encoder (`dsp/t44.cpp`) | Mode 1. The mask in T.85 (JBIG) or MH. The image layers in JPEG. Stripes of 256 lines. A stripe with no text has only the background, at full resolution. A layer of one colour is not coded. |
| Decoder | Mode 1. Masks in MH, MR, MMR or JBIG. Image layers in JPEG or T.43. Offsets, lower layer resolutions and the 4-octet length. Optional segments are skipped. |
| T.30 (`dsp/t30.cpp`) | DIS bits 80, 88, 92 and 95. The mask in T.85 when the far end has bit 78, else in MH. |
| Not implemented | Modes 2 and up (Annexes A and B), JBIG2, T.45, YCC, other gamuts |

The tests `T44.*` (`dsp/test/t44_test.cpp`) check these items:

- The SOP octets, and the round trip with each mask coding.
- The stripe types.
- A page as another coder can write it: offsets, MMR, T.43, a
  foreground only.
- Damaged data.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| The decoder read the 4-octet optional length before the identifier. 9.2 puts it after the identifier. | The decoder reads it after the identifier, and refuses the reserved lengths 1 to 5 |
| The SOP gave the mask coder and the image coder when no stripe used them (9.2.1: 0 when there is no coded layer) | The SOP gives 0 for a layer that no stripe codes |

The tests `T44.OptionalLength` and `T44.SopCodersOfLayersNotCoded`
(`dsp/test/t44_test.cpp`) check these corrections.
