# G.729: technical details

This page gives the technical details for [G.729](g729.md). The clause
numbers are the clause numbers of G.729, Annex A and Annex B. This page
is our own description, not a copy of the Recommendation. The details of
the D-Modem implementation are on [DSVD: technical
details](../v70-details.md#g729-annex-a).

## The frame (2, 3)

| Item | Value |
|---|---|
| Input | 16-bit linear PCM, 8000 samples each second |
| Frame | 10 ms, 80 samples |
| Subframe | 5 ms, 40 samples |
| Look-ahead | 5 ms |
| Bit rate | 8 kbit/s: 80 bits in each frame |

## The parameters of a frame (Table 8)

| Parameter | Bits |
|---|---|
| LSP: L0, L1, L2, L3 | 1 + 7 + 5 + 5 = 18 |
| Pitch delay of subframe 1 (P1) | 8 |
| Parity of the pitch delay (P0) | 1 |
| Fixed codebook of subframe 1: positions and signs (C1, S1) | 13 + 4 |
| Gains of subframe 1 (GA1, GB1) | 3 + 4 |
| Pitch delay of subframe 2 (P2), relative to P1 | 5 |
| Fixed codebook of subframe 2 (C2, S2) | 13 + 4 |
| Gains of subframe 2 (GA2, GB2) | 3 + 4 |
| Total | 80 |

The bits go in the order of the table, the most significant bit of each
parameter first.

## The encoder (3)

1. A high-pass filter at 140 Hz, and a scale of 1/2.
2. Linear prediction of order 10, from the autocorrelation of a 30 ms
   window. The coefficients become LSPs, quantized with two stages and
   a predictor of 4 frames (MA).
3. The open-loop pitch, then the closed-loop pitch for each subframe.
   Subframe 1 has a resolution of 1/3 sample for delays from 19 1/3 to
   84 2/3.
4. The fixed codebook: 4 pulses with signs (ACELP, 17 bits).
5. The two gains: vector quantization with conjugate codebooks, and a
   prediction of the fixed codebook gain.

## The decoder (4)

1. The LSPs, the pitch, the pulses and the gains from the bits.
2. The excitation through the synthesis filter.
3. A postfilter: long term (pitch), short term (formants), tilt
   compensation and adaptive gain control.
4. A high-pass filter and a scale of 2.
5. **Frame erasure (4.4).** A lost frame gets the LSPs of the last
   frame, a pitch delay that increases a little, and gains that
   decrease. The parity bit P0 marks a bad pitch delay.

## Annex A: less complexity

The bit stream is the bit stream of G.729. These parts are simpler:

- The perceptual weighting filter uses the quantized coefficients.
- The open-loop pitch search uses decimation.
- The fixed codebook search is a depth-first tree search.
- The postfilter is simpler.

## Annex B: silence compression

| Part | Function |
|---|---|
| VAD (B.3) | It decides each frame: active speech, or not. It uses the energy, the low-band energy, the zero-crossing rate and the LSFs. |
| DTX (B.4) | In silence, the encoder sends a SID frame when the noise changes (Itakura distance or energy). Between the SID frames, it sends nothing. |
| SID frame (B.4.3) | 15 bits in 2 octets: the LSF index (1 + 5 + 4) and the energy (5) |
| CNG (B.4.4) | The decoder makes comfort noise from the last SID: a random excitation through the filter of the SID, at the level of the SID |

| Frame type | Octets |
|---|---|
| Speech | 10 |
| SID | 2 |
| Not transmitted | 0 |

## Test vectors

The ITU-T software package has test vectors for the encoder (`.IN` to
`.BIT`) and for the decoder (`.BIT` to `.PST`). A coder that conforms
gives the same bits and the same samples.

## G.729 in D-Modem

| Item | D-Modem (`link/g729a_*.cpp`, `link/g729b.cpp`) |
|---|---|
| Coder | Annex A, with Annex B when DTX is on, in the fixed point of the reference |
| Frame | 80 samples in, 10 octets out, the bits MSB first as RFC 3551 |
| Test vectors | All Annex A vectors, encoder and decoder, bit for bit. The Annex B vectors `tstseq1` to `tstseq6`, bit for bit. |
| Full G.729 (not Annex A) | Not implemented. The decoder of Annex A decodes the bit stream of G.729. |
| Use | The voice channel of V.70 |
