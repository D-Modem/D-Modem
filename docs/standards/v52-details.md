# V.52 and O.153: technical details

This page gives the 511-bit pattern and the rules for its checker, from
V.52 (11/72) and O.153 (10/92). For a summary, refer to
[V.52 and O.153](v52.md).

## The 511-bit pattern (O.153 2.1)

| Item | Value |
|---|---|
| Shift register | 9 stages |
| Feedback | The outputs of the 5th and 9th stages, added modulo 2, go into the first stage |
| Length | 2^9 - 1 = 511 bits |
| Start | The first ONE of the 9 ONEs in a row |
| Longest run of ZEROs | 8 |
| Use | Error measurements at bit rates up to 14.4 kbit/s |

O.153 also gives a 2047-bit pattern (x^11 + x^9 + 1), a 1 048 575-bit
pattern (x^20 + x^3 + 1) and fixed patterns.

## Loss of synchronisation (O.153 2.6)

The checker synchronises again when:

- the bit error ratio is 0.20 or more in an interval of 1 s, or
- the received pattern is out of phase with the reference pattern.

## Error measurements (O.153 8)

- The bit error ratio can be from 10^-2 to 10^-7.
- For smaller ratios, the equipment counts the errors cumulatively.

## V.52 in D-Modem

| Item | D-Modem (`core/v54.h`, `core/v54.cpp`) |
|---|---|
| Generator | `Prbs9`: 9 stages, the 5th and 9th stages into the first. The output is the 9th stage. |
| Checker | `Checker`: it synchronises when 32 bits in a row follow from the 9 bits before them. Then it counts each bit that is different from the reference. |
| Loss of synchronisation | An error ratio of 0.20 or more in one interval. The interval is the bits of 1 s at the data rate of the call. |
| Tests | `AT&T7` (loop 2) and `AT&T8` (loop 3). `AT&T0` gives the bit errors, three digits, 255 at most. `+TTER` counts the bit errors and the block errors. |
| 2047-bit and other patterns | Not done (`+TTER` gives ERROR) |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The start of the pattern (O.153 2.1) | The output was the feedback bit. The pattern started 9 bits after its 9 ONEs. | The output is the 9th stage. The pattern starts with the first of its 9 ONEs. |
| Loss of synchronisation (O.153 2.6) | 64 errors in 256 bits (0.25 in 256 bits) | An error ratio of 0.20 or more in 1 s of bits at the data rate. A short burst of errors does not stop the count. |

The tests `V52.PatternStart` and `V52.CheckerLossOfSync`
(`core/test/core_test.cpp`) check the two corrections.
