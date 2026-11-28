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

Bit 7 of each coder octet tells that one more octet follows. The bits
of the next octet continue the numbers: bit 0 of the second octet is
coder 8. The type of stripe (Table 3) uses the same method for layers
8 and up.

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

## Modes 2 and 3 (Annex A)

The SOP is the same as in mode 1, but the version is 0 (A.9.2.1) and
the mode is 2 or 3. The SOSt has only the type of stripe (Table 3).
Then each layer has these marker segments, and its coded data:

| Part | Octets |
|---|---|
| SLC | APP13, the length (30), "MRC" 2, the layer number (1), the coder (2 or more), the resolution (2), the width and the height (4 each, in mask pels), the layer base colour (3), the offset (x, y: 4 each) |
| Encoder segments (EMSe) | APP13, the length, "MRC" 12 to 254, the parameters. Foreign segments (APPn) can also be here. |
| EOH | APP13, the length (10), "MRC" 255, the length of the coded data (4) |
| The coded data | That many octets |

The first octet of the coder tells if the layer has coded data (bit 0)
and if the coder is from Table 2 (bit 1, an image layer) or Table 1
(a mask layer). The second octet is the bit number of the coder in
that table. A layer with no coded data has an EOH with a length of 0.

The layers come in this sequence: the main mask (layer 2), the
background (1), the foreground (3), then layers 4, 5, 6 and more. The
first SLC after the SOSt is always that of a mask, with or without
coded data. Its height is the height of the stripe.

### The layers of a stripe (A.6.4, A.7.4)

- Layer 1 is the background. Layers 2, 4, 6 and more are masks. Layers
  3, 5, 7 and more are images.
- Mode 2 has layers 1 to 3 only. Mode 3 can have more.
- The main mask is the full width and height of the stripe. A
  different layer can be smaller, at an offset in the stripe.
- The decoder draws the background first. Then it draws each image in
  the sequence of the numbers. An image shows where the mask below it
  (layer 3 above layer 2, 5 above 4) is 1. An image, or a part of it,
  that has no mask is drawn over its area.
- A layer with no coded data is its base colour. The default base
  colour of the background is white. The default base colour of the
  other image layers is black.

## T.44 in D-Modem

| Item | D-Modem |
|---|---|
| Encoder (`dsp/t44.cpp`) | Modes 1, 2 and 3 (`T44Options::mode`). The mask in T.85 (JBIG) or MH. The image layers in JPEG. Stripes of 256 lines. A stripe with no text has only the background, at full resolution. A layer of one colour is not coded. In mode 3, text of 2 to 6 flat colours in a stripe has a mask and a base colour for each colour (layers 2 and 3, 4 and 5, and more), and no foreground JPEG. Each mask after the main mask has the size of its text, at an offset. |
| Decoder | Modes 1, 2 and 3. Masks in MH, MR, MMR or JBIG, at all resolutions that divide that of the main mask. Image layers in JPEG or T.43. Offsets, lower layer resolutions and the 4-octet length. Up to 28 layers in a stripe. Optional segments, encoder segments and foreign segments are skipped. |
| T.30 (`dsp/t30.cpp`) | DIS bits 80, 88, 92 and 95 (mode 1 only). The mask in T.85 when the far end has bit 78, else in MH. |
| Not implemented | Mode 4 (Annex B), JBIG2, T.45 layers, YCC, other gamuts |

The tests `T44.*` (`dsp/test/t44_test.cpp`) check these items:

- The SOP octets, and the round trip with each mask coding.
- The stripe types.
- A page as another coder can write it: offsets, MMR, T.43, a
  foreground only.
- Damaged data.
- Mode 2: the SOP, the sequence of the SLCs, the same pels as mode 1,
  and a stripe with a main mask that has no coded data
  (`T44.Mode2`).
- Mode 3: text in two colours comes back exactly, in a seventh of the
  octets of mode 1 (`T44.Mode3TextColours`).
- An N-layer page as another coder can write it: masks at a lower
  resolution and at an offset, layers with no coded data, an image with
  no mask, encoder and foreign segments, the 4-octet length in the SLC
  (`T44.Mode3HandWritten`).
- Damaged data in mode 3 (`T44.Mode3Damaged`).

## Corrections (2026-11)

| Error | Correction |
|---|---|
| The decoder read the 4-octet optional length before the identifier. 9.2 puts it after the identifier. | The decoder reads it after the identifier, and refuses the reserved lengths 1 to 5 |
| The SOP gave the mask coder and the image coder when no stripe used them (9.2.1: 0 when there is no coded layer) | The SOP gives 0 for a layer that no stripe codes |

| The decoder did not accept modes 2 and 3 (Annex A): a page in these modes gave no image | The decoder accepts the SLC and EOH of each layer, and N-layer stripes. The encoder can write modes 2 and 3. |

The tests `T44.OptionalLength`, `T44.SopCodersOfLayersNotCoded`,
`T44.Mode2` and `T44.Mode3HandWritten` (`dsp/test/t44_test.cpp`) check
these corrections.
