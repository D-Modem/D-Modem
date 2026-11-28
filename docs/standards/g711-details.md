# G.711: technical details

This page gives the technical details for [G.711](g711.md). The clause
and table numbers are the numbers of G.711 (11/88). This page is our own
description, not a copy of the Recommendation.

## The sampling and the order of the bits (2, 4)

| Item | Value |
|---|---|
| Sampling rate | 8000 samples each second, ±50 ppm |
| Bits for each sample | 8 |
| Order on a serial line | Bit 1 (the polarity) first, bit 8 (the least significant bit) last |

## The A-law (Table 1)

The uniform code has 13 bits (12 bits and the sign). In 13-bit steps,
the quantized value of segment s and step m is:

| Segment | Quantized value |
|---|---|
| 0 | 2m + 1 |
| 1 to 7 | (2m + 33) × 2^(s − 1) |

- Bit 1 is 1 for a positive value.
- The even bits (2, 4, 6 and 8) are inverted on the line. Thus the
  octets for the two zero values are `D5h` and `55h`.
- The largest value is 4032.

## The µ-law (Table 2)

The uniform code has 14 bits (13 bits and the sign). In 14-bit steps,
the quantized value of segment s and step m is (2m + 33) × 2^s − 33.

- All 8 bits are inverted on the line. Bit 1 is 1 for a positive value.
  Thus `FFh` is +0 and `7Fh` is −0.
- The largest value is 8031.
- A network that must not send the all-zero octet sends `02h` instead
  of `00h`. The decoder then gives −7519 (3.2).

## Encoding (3.6)

The decision values are between the quantized values. G.711 does not
specify the conversion from uniform PCM. G.721 gives one method.

## The conversions (Tables 3 and 4)

Table 3 gives the A-law value for each µ-law value. Table 4 gives the
µ-law value for each A-law value. A µ-A-µ conversion, or an A-µ-A
conversion, keeps bits 1 to 7.

## The level (5)

The sequences of Table 5 (A-law) and Table 6 (µ-law) decode to a 1 kHz
sine wave at 0 dBm0. The load capacity is +3.14 dBm0 for the A-law and
+3.17 dBm0 for the µ-law.

## G.711 in D-Modem

| Item | D-Modem (`dsp/g711.h`) |
|---|---|
| Decoders | `alaw2lin()` and `ulaw2lin()`, to 16-bit values (the 13-bit or 14-bit value multiplied by 8 or by 4) |
| Encoders | `lin2alaw()` and `lin2ulaw()`, the method of G.191. The µ-law input is clipped at 8159. |
| Sign and magnitude | `encode_sm()` codes the magnitude with the sign bit, as the ANSpcm tables of V.92 do |
| Suppression of the all-zero octet | Not used |
| µ-A and A-µ conversion (Tables 3 and 4) | Not implemented. The modem uses the law of the line. |
| Tests | `dsp/test/g711_test.cpp`: each octet decodes to the value of Tables 1 and 2, and codes back to itself |
