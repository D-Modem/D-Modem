# T.45: technical details

This page gives the technical details of T.45. For the summary, refer
to [T.45](t45.md).

## The header (2.1.1)

All numbers are big-endian.

| Field | Octets | Value |
|---|---|---|
| NCOMP | 1 | The components in a value, 1 to 255 |
| COMPLEN | 1 | The octets in a component: 1, 2 or 4 |
| NVALS | 4 | The number of values, 0 to 2^32 - 1 |

For palette indices, NCOMP is 1. COMPLEN is 1, or 2 for more than 256
entries, or 4 for more than 65536 entries.

## The runs (2.1.2)

The runs follow the header. Each run has these fields:

| Field | Octets | Value |
|---|---|---|
| RUNLEN | 1 or 3 | The number of times that the value occurs, 0 to 65535. A first octet of 0x01 to 0xFF is the length. A first octet of 0x00 tells that the length is in the next 2 octets. |
| CVAL | NCOMP × COMPLEN | The value |

The data ends when the sum of the RUNLEN fields is NVALS. Appendix I
gives an example of 10 values in 4 runs.

## T.45 in D-Modem

| Item | D-Modem |
|---|---|
| Encoder (`dsp/t45.cpp`) | A run for each sequence of equal values, a maximum of 65535 in a run. A run of 1 to 255 has a 1-octet RUNLEN. A longer run has the 3-octet form. |
| Decoder | All the fields. A run of 0 is valid. The decoder refuses an NCOMP of 0, a COMPLEN that is not 1, 2 or 4, a run past NVALS, data that is cut short, and more values than a limit (16777216 by default). |

The tests `T45.*` (`dsp/test/t45_test.cpp`) check these items:

- The example of Appendix I.
- The 3-octet RUNLEN, runs longer than 65535, runs of 0 and no values.
- Components of 1, 2 and 4 octets.
- Data that is not valid.
